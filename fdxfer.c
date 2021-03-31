#include "def.h"
#include "fdxfer.h"
#include "log.h"
#include "lr.h"
#include "coredesc.h"

#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#if defined(DLOG_HAVE_BSD) || defined(DLOG_HAVE_OSX)
#	include <sys/param.h>
#	include <sys/ucred.h>
#endif
#include <assert.h>

#define XFER_BUF_LEN (64*1024)

static xfer_msg** _recv_msg_bufs = NULL;
static int _skt, _nmsg = 0, _capmsg = 16;

static dynstr*
_parent_skt_name()
{
	dynstr* s = dynstr_new(DLOG_UNIX_SKT_NAME);
	return dynstr_ncat(s, getppid());
}

static dynstr*
_skt_name()
{
	dynstr* s = dynstr_new(DLOG_UNIX_SKT_NAME);
	return dynstr_ncat(s, getpid());
}

static void
_add_new_msg(xfer_msg* msg)
{
	if (!_recv_msg_bufs) {
		_recv_msg_bufs = calloc(_capmsg, sizeof(xfer_msg*));
	}

	if (_nmsg == _capmsg) {
		_capmsg *= 2;
		_recv_msg_bufs = realloc(_recv_msg_bufs, sizeof(xfer_msg*)*_capmsg);
	}

	_recv_msg_bufs[_nmsg++] = msg;
}

int
fdxfer_open_send(void)
{
    int r=0,  loop=3;
    struct sockaddr_un remote;

    if ((_skt = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        LOG_SYS_ERROR("Failed to create Unix socket");
        return -1;
    }

	memset(&remote, 0, sizeof(struct sockaddr_un));
    remote.sun_family = AF_UNIX;
	dynstr* addr = _skt_name();
    strncpy(remote.sun_path, dynstr_ptr(addr), sizeof(remote.sun_path)-1);

    while ((r=connect(_skt, (struct sockaddr *)&remote, sizeof(struct sockaddr_un))) < 0 && loop--) {
        LOG_DEBUG("Failed to connect to Unix socket, continuing to wait");
		sleep(1);
    }

	return r;
}

int
fdxfer_open_recv(xfer_msg*** msgs, int* nummsg)
{
	char recv_buf[XFER_BUF_LEN]
			__attribute__((aligned(__alignof__(xfer_msg))));
    int r;
    struct sockaddr_un addr;

    if ((_skt = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        LOG_SYS_ERROR("Failed to create Unix socket");
        return -1;
    }

	memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, dynstr_ptr(_parent_skt_name()), sizeof(addr.sun_path)-1);
    unlink(addr.sun_path);
    if (bind(_skt, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == -1) {
        LOG_SYS_ERROR("Failed to bind Unix socket");
        return (-1);
    }

    if (listen(_skt, 1) == -1) {
        LOG_SYS_ERROR("Failed to listen on Unix socket");
        return (-1);
    }


	LOG_DEBUG("Accepting xfer connection...");
    if ((_skt = accept(_skt, NULL, NULL)) == -1) {
        LOG_SYS_ERROR("Failed to accept on Unix socket");
        return (-1);
    }

	size_t ucredsz = sizeof(STRUCT_UCRED);
	size_t ctrl_msg_sz = CMSG_SPACE(sizeof(int)) + CMSG_SPACE(ucredsz);
	char* ctrl_msg = malloc(ctrl_msg_sz);

    struct iovec iov;
	iov.iov_base = recv_buf;
	iov.iov_len = sizeof(recv_buf);

    struct msghdr msgh;
    struct cmsghdr *cmsg;

    msgh.msg_name = NULL;
    msgh.msg_namelen = 0;
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_flags = 0;
    msgh.msg_control = ctrl_msg;
    msgh.msg_controllen = ctrl_msg_sz;
    cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_len = msgh.msg_controllen;
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;

	while(1) {
		r = recvmsg(_skt, &msgh, 0);
		if(r == 0 || (r<0 && errno == ECONNRESET)) {
			LOG_INFO("Finished FD transfer.");
			*msgs = _recv_msg_bufs;
			*nummsg = _nmsg;
			return 0;
		}

		if (r < 0) {
			LOG_SYS_ERROR("XFER - Failed to receive data");
			goto fail;
		}

		xfer_msg* data = (xfer_msg *)recv_buf;
		if (r < sizeof(xfer_msg) + data->buf_len) {
			LOG_ERROR("XFER - not all data transferred over, dumping the fd...");
			continue;
		}

		/* copy data */
		xfer_msg* xfer = malloc(r);
		memcpy(xfer, data, r);

		/* there should be only one header... */
		struct cmsghdr* cmsgp = CMSG_FIRSTHDR(&msgh);
		if (cmsgp->cmsg_level != SOL_SOCKET) {
			LOG_ERROR("XFER - Invalid level in FD transfer, will ignore it");
			continue;
		}

		switch(cmsgp->cmsg_type) {
		case SCM_RIGHTS:
			/* one fd transfer */
			memcpy(&xfer->in_fd, CMSG_DATA(cmsgp), sizeof(int));

			/* complete transfer */
			_add_new_msg(xfer);
		}
	}

	return 0;

fail:
	return -1;
}

/* send single FD and accompanying data across */
int
fdxfer_send(descriptor* d)
{
	struct msghdr msghdr;
	struct iovec buf_data;
	struct cmsghdr *cmsg;
	int ret = 0, bufidx = 0;

    union {
        char   buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } ctrlMsg;

	// buffer data
	size_t strlen = dynstr_len(d->symbol) +
					dynstr_len(reader_raw_buffer(d->reader, &bufidx)) + 2;

	size_t buflen = sizeof(xfer_msg) + strlen;

	xfer_msg* sendbuf = (xfer_msg *)calloc(1, buflen);
	char* b = stpcpy(sendbuf->buf, dynstr_ptr(d->symbol));
	b++;
	// TODO maybe don't need this check?
	if (dynstr_ptr(reader_raw_buffer(d->reader, &sendbuf->buf_idx)))
		stpcpy(b, dynstr_ptr(reader_raw_buffer(d->reader, &sendbuf->buf_idx)));
	else
		*b = 0;
	sendbuf->desc_type = d->type;
	sendbuf->buf_len = strlen;

	buf_data.iov_base = sendbuf;
	buf_data.iov_len = buflen;
	msghdr.msg_name = NULL;
	msghdr.msg_namelen = 0;
	msghdr.msg_iov = &buf_data;
	msghdr.msg_iovlen = 1;
	msghdr.msg_flags = 0;
	msghdr.msg_control = &ctrlMsg;
	msghdr.msg_controllen = sizeof(ctrlMsg.buf);
    memset(ctrlMsg.buf, 0, sizeof(ctrlMsg.buf));
	cmsg = CMSG_FIRSTHDR(&msghdr);
	cmsg->cmsg_len = msghdr.msg_controllen;
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;

    memcpy(CMSG_DATA(cmsg), &d->fd, sizeof(int));

	if (sendmsg(_skt, &msghdr, 0) < 0) {
		LOG_SYS_ERROR("XFER - sending failed");
		ret = -1;
	}

	free(sendbuf);
	return ret;
}

void
fdxfer_close(void)
{
	LOG_DEBUG("xfer socket closed");
	free(_recv_msg_bufs);
	close(_skt);
	// FIXME unlink socket
}


