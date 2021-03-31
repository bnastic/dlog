#include "def.h"

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <libgen.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "log.h"
#include "env.h"
#include "hashtable.h"
#include "coredesc.h"
#include "evt.h"
#include "lr.h"
#include "lw.h"

struct descriptor;

static void open_file_r(descriptor* d, int flags, bool reuse);
static void open_file_w(descriptor* d, int flag);
static void open_fifo_r(descriptor* d, int flags);
static void open_fifo_w(descriptor* d, int flag);
static void open_skt_w(descriptor* d);
static void open_socket_read(descriptor* d);
#if defined(DLOG_HAVE_LINUX)
static void open_inotify(descriptor* d);
#endif

static int _socket_listen_pre_read(descriptor* listenfd, int size_hint);
static int _sktw_post_line_write(struct descriptor*, ssize_t nbytes, int write_err_code);
static void _open_socket_listen(descriptor* d);

struct dorigin inotify_origin =
{
	.type	= D_INOTIFY,
	.symbol = DLOG_INOTIFY_SYMBOL,
	.next	= NULL
};

struct dorigin socket_read_origin =
{
	.type	= D_SOCKETR,
	.symbol = DLOG_CLIENT_SOCKET_SYM,
	.next	= NULL
};


descriptor*
open_descriptor(dorigin* or, descriptor* d /* or NULL*/,
				struct vdescfn* fn /* or NULL */, int flags)
{
	bool reuse = true;

	if (!d && or->type != D_TYPEINVALID)
	{
		d = calloc(1, sizeof(*d));
		d->state = DSTATE_INIT;
		d->type = or->type;
		d->origin = or;
		d->vfn.on_activate = fn ? fn->on_activate : NULL;
		d->vfn.on_deactivate = fn ? fn->on_deactivate : NULL;
		d->vfn.pre_read = fn ? fn->pre_read : NULL;
		d->vfn.post_line_write = fn ? fn->post_line_write : NULL;
		d->vfn.state = fn ? fn->state : NULL;
		d->symbol = dynstr_new(d->origin->symbol);

		if (D_IS_READ_SIDE(d->type)) {
			d->reader = reader_new();
			if(or->inherited.buffer) {
				reader_reset_with_buffer(d->reader,
										or->inherited.buffer,
										or->inherited.buf_idx);
			}
		} else if (D_IS_WRITE_SIDE(d->type)) {
			d->wqueue = wq_new();
		}

		reuse = false;
	}

	/* core types */
	switch (or->type & DESC_CORE_BITMASK)
	{
		case D_FILER: {
			open_file_r(d, flags, reuse);
		}
		break;

		case D_FIFOR: {
			open_fifo_r(d, flags);
		}
		break;

#if defined (DLOG_HAVE_LINUX)
		case D_INOTIFY: {
			open_inotify(d);
		}
		break;
#endif

		case D_FILEW: {
			open_file_w(d, flags);
		}
		break;

		case D_FIFOW: {
			open_fifo_w(d, flags);
		}
		break;

		case D_SOCKETW: {
			open_skt_w(d);
		}
		break;

		case D_SOCKETR: {
			/* socket already open by accept(), or xfer'd */
			//d->fd = or->inherited.fd;
			//d->state = DSTATE_ACTIVE;
			open_socket_read(d);
		}
		break;

		case D_SOCKET_LISTEN: {
			_open_socket_listen(d);
		}
		break;

		default:
			return NULL;
	}
	/* unsuccessful. terminal state*/
	if (d->state == DSTATE_DEAD)
	{
		close_descriptor(d);
		return NULL;
	} else {
		if (!(flags & DOPEN_KEEP_BUFFERS)) {
			if (D_IS_READ_SIDE(d->type)) {
				reader_reset(d->reader);
			} else if (D_IS_WRITE_SIDE(d->type)) {
				wq_destroy(d->wqueue);
			}
			/* nothing for generic bypass */

		}

		if (d->state == DSTATE_ACTIVE && d->vfn.on_activate)
			d->vfn.on_activate(d);

		if (!reuse) {
			if(!(D_IS_SOCKET_READ(d->type))) {
				ht_upsert(dlogenv->symbol_table, (uintptr_t)d->symbol, d);
			}
		}

		if (d->state == DSTATE_ACTIVE) {
			TAILQ_INSERT_TAIL(&dlogenv->desc_active_list, d, _lnk);
		}
	}

	return d;
}

void
close_descriptor(descriptor* d)
{
	if (!d || d->state == DSTATE_DEAD)
		return;

	evt_reg_remove(d);

	if (d->vfn.on_deactivate)
		d->vfn.on_deactivate(d);

	close(d->fd);
	d->fd = -1;

	if (D_IS_READ_SIDE(d->type)) {
		reader_destroy(d->reader);
	} else {
		wq_destroy(d->wqueue);
	}

	if (d->state == DSTATE_ACTIVE) {
		TAILQ_REMOVE(&dlogenv->desc_active_list, d, _lnk);
	}

	ht_remove(dlogenv->symbol_table, (uintptr_t)d->symbol);
	dynstr_free(d->symbol);

	if (d->vfn.state)
		free(d->vfn.state);

	d->state = DSTATE_DEAD;

	/* don't remove origin pointers, they are immutable */
	free(d);
}

void
reset_descriptor(descriptor* d)
{
	close(d->fd);
	//d->fd = -1;
	if (d->state == DSTATE_ACTIVE) {
		TAILQ_REMOVE(&dlogenv->desc_active_list, d, _lnk);
	}
	d->state = DSTATE_INIT;
}


static void
open_socket_read(descriptor* d)
{
	/* the socket is always coming from accept() or xfer */
	if (d) {
		d->fd = d->origin->inherited.fd;
		d->origin->inherited.fd = 0;
		if (evt_reg_read(d) == 0) {
			d->state = DSTATE_ACTIVE;
		}
		else {
			d->state = DSTATE_DEAD;
		}
	}
}

/* assumes inherited state */
//descriptor*
//open_socket_read_with_buffer(int fd, const char* buffer)
//{
//	descriptor* d = NULL;
//
//	if ((d = open_socket_read(fd))) {
//		reader_reset_with_buffer(d->reader, buffer);
//	}
//
//	return d;
//}

static void
open_file_r(descriptor* d, int flags, bool reuse)
{
	bool insert_into_pending = false;

	if (!d->origin->inherited.fd) {
		if (d->state == DSTATE_INIT) {
			if (access(d->origin->file.path, R_OK) == -1) {
				LOG_DEBUG("File (%s) not found, monitoring...", d->origin->symbol);
				evt_watch_vnode(d);
				d->state = DSTATE_PENDING;
				return;
			}
		}

		d->fd = open(d->origin->file.path, O_RDONLY | O_NONBLOCK);

		if (d->fd == -1) {
			LOG_SYS_ERROR("Failed to open file");
		}

		if (!reuse || (flags & DOPEN_SEEKEND)) {
			if (-1 == lseek(d->fd, 0, SEEK_END)) {
				LOG_SYS_ERROR("lseek failed");
			}
		}
	} else {
		d->fd = d->origin->inherited.fd;
		d->origin->inherited.fd = 0;
		LOG_DEBUG("File offset is %d", lseek(d->fd, 0, SEEK_CUR));
		//reader_reset_with_buffer(d->reader, d->origin->inherited.buffer);
	}

#ifdef DLOG_HAVE_LINUX
	/* Kqueue generates read events on a newly open file. Linux doesn't do that.
	   If the file is open with SEEKSTART we need to trigger that first read by
	   manually adding the file to pending reads list
	*/
	if (flags | DOPEN_SEEKSTART)
		insert_into_pending = true;
#endif

	if (evt_reg_read(d) == 0) {
		evt_reg_vnode_del(d);
		d->state = DSTATE_ACTIVE;

		if (insert_into_pending) {
			ht_upsert(dlogenv->pending_reads_table, d->fd, d);
		}
	} else {
		d->state = DSTATE_DEAD;
	}
}

static void
open_file_w(descriptor* d, int flag)
{
	d->fd = open(d->origin->file.path,
				O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | flag,
				S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

	if (d->fd == -1) {
		LOG_SYS_ERROR("Failed to open file for writing, symbol %s", dynstr_ptr(d->symbol));
		d->state = DSTATE_DEAD;
		return;
	}

	d->state = DSTATE_ACTIVE;
}

static void
open_fifo_r(descriptor* d, int flags)
{
	int res = mkfifo(d->origin->file.path,
					 S_IRWXU | S_IRWXG | S_IRWXO);
	if (res == -1 && errno != EEXIST) {
		evt_watch_vnode(d);
		d->state = DSTATE_PENDING;
	} else {
		open_file_r(d, DOPEN_NOFLAGS, false);
	}
}

static void
open_fifo_w(descriptor* d, int flags)
{
	int res = mkfifo(d->origin->file.path,
					S_IRWXU | S_IRWXG | S_IRWXO);
	if (res == -1 && errno != EEXIST) {
		LOG_SYS_ERROR("Failed creating write-side fifo (%s)", d->origin->file.path);
		d->state = DSTATE_DEAD;
	} else {
		open_file_w(d, DOPEN_NOFLAGS);
	}
}

void
free_dorigin(struct dorigin * o)
{
	if (!o) return;

	while(o) {
		free(o->symbol);
		free(o->inherited.buffer);
		if (D_IS_FILE(o->type)) {
			free(o->file.path);
		} else if (D_IS_SOCKET_READ(o->type) || D_IS_SOCKET_WRITE(o->type)) {
			free(o->socket.host);
			free(o->socket.port);
		}
	}
	free (o);
}

static int
_socket_listen_pre_read(descriptor* listenfd, int size_hint)
{

	/* return -1 to stop actual reading */

	while(1) {
		struct sockaddr_storage their_addr;
		socklen_t sin_size = sizeof(their_addr);

		int fd = accept(listenfd->fd,
						(struct sockaddr *)&their_addr,
						&sin_size);

		if (-1 == fd) {
			if (errno != EWOULDBLOCK) {
				LOG_SYS_ERROR("Listening socket failed to accept new connection");
			}
			break;
		}

		if (-1 == fcntl(fd, F_SETFL, O_NONBLOCK | fcntl (fd, F_GETFL, 0))) {
			LOG_ERROR("Failed to make reader socket non-blocking, bailing out");
			close(fd);
			return -1;
		}

		/*
		inet_ntop(their_addr.ss_family,
				get_in_addr((struct sockaddr *)&their_addr),
				ip, sizeof ip);
		*/

		socket_read_origin.inherited.fd = fd;
		socket_read_origin.inherited.buffer = NULL;
		if (!open_descriptor(&socket_read_origin, NULL, NULL, 0)) {
			LOG_ERROR("Failed to open read socket");
		} else {
			LOG_DEBUG("Accepted client connection.");
		}
	}

	return -1;
}


static int
_sktw_post_line_write(struct descriptor* d, ssize_t nbytes, int err)
{
	/* the other side has gone */
	if (err != 0) {
		int olderr = errno;
		errno = err;
		LOG_SYS_ERROR("sktw_post_line_write error");
		errno = olderr;
	}

	if (d->state == DSTATE_PENDING || err == EPIPE) {
		/* drop socket, and reinitialise it for connection
		 * while keeping the buffers for later writing
		 */

		reset_descriptor(d);
		open_descriptor(d->origin, d, NULL, DOPEN_KEEP_BUFFERS);
	}

	return 0;
}


static void
_open_socket_listen(descriptor* d)
{
	struct addrinfo *servinfo, *p;
	int rv;

	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_PASSIVE,
	};

	if ((rv = getaddrinfo(NULL, dlogenv->config.listenskt_port, &hints, &servinfo)) != 0) {
		LOG_ERROR("getaddrinfo: %s\n", gai_strerror(rv));
		d->state = DSTATE_DEAD;
		return;
	}

	/* loop through all the results and bind to the first we can */
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((d->fd = socket(p->ai_family, p->ai_socktype,
						p->ai_protocol)) == -1)
		{
			continue;
		}

		if (setsockopt(d->fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) == -1) {
			LOG_ERROR("Failed to setsockopt on listen socket");
			close(d->fd);
			d->state = DSTATE_DEAD;
			return;
		}
		fcntl(d->fd, F_SETFD, FD_CLOEXEC | fcntl (d->fd, F_GETFD, 0));
		if (-1 == fcntl(d->fd, F_SETFL, O_NONBLOCK | fcntl (d->fd, F_GETFL, 0))) {
			LOG_ERROR("Failed to make listen socket non-blocking, bailing out");
			close(d->fd);
			d->state = DSTATE_DEAD;
			return;
		}

		if (bind(d->fd, p->ai_addr, p->ai_addrlen) == -1) {
			LOG_SYS_ERROR("Socket bind failed, but will ontinue if possible");
			continue;
		}
		break;
	}

	freeaddrinfo(servinfo);

	if (p == NULL)	{
		LOG_ERROR("TCP server socket failed to bind()");
		d->state = DSTATE_DEAD;
		return;
	}

	if (listen(d->fd, 10) == -1) {
		LOG_ERROR("Socket failed to listen()");
		close(d->fd);
		d->state = DSTATE_DEAD;
		return;
	}

	if (evt_reg_read(d) == 0) {
		d->state = DSTATE_ACTIVE;
		d->vfn.pre_read = _socket_listen_pre_read;
	}
	else {
		d->state = DSTATE_DEAD;
	}
}

/*
 * Client socket
 */
static void
open_skt_w(descriptor* d)
{
	struct addrinfo *servinfo, *p;
	int rv;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM
	};

	d->vfn.post_line_write = _sktw_post_line_write;

	if ((rv = getaddrinfo(d->origin->socket.host, d->origin->socket.port,
							&hints, &servinfo)) != 0)
	{
		LOG_ERROR("getaddrinfo: %s\n", gai_strerror(rv));
		d->state = DSTATE_DEAD;
		return;
	}

	// loop through all the results and connect to the first we can
	p = servinfo;

	for(p = servinfo; p != NULL; p = p->ai_next) {

		if ((d->fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
			continue;
		}

		if (-1 == fcntl(d->fd, F_SETFL, O_NONBLOCK | fcntl (d->fd, F_GETFL, 0))) {
			continue;
		}

		if (connect(d->fd, p->ai_addr, p->ai_addrlen) == -1) {
			if (errno == EINPROGRESS) {
				LOG_DEBUG("Socket in EINPROGRESS...");
				d->state = DSTATE_PENDING;
				evt_reg_write(d);
				break;
			} else {
			    close(d->fd);
				continue;
			}
		} else {
			/* BSD will connect local sockets immediately */
			LOG_DEBUG("Socket %s connected.", d->origin->symbol);
			d->state = DSTATE_ACTIVE;
			break;
		}
	}

	freeaddrinfo(servinfo);

	if (!p) {

		int serr;
		socklen_t errlen = sizeof(serr);

		/* check why we're here */
		if (0 == getsockopt(d->fd, SOL_SOCKET, SO_ERROR, &serr, &errlen)) {
			if (serr == ECONNREFUSED) {
				LOG_WARNING("Socket %s - server not available, will retry");
				d->state = DSTATE_PENDING;
			} else {
				int olderr = errno;
				errno = serr;
				LOG_SYS_ERROR("Socket %s can't connect, will be shut down.", d->origin->symbol);
				errno = olderr;

				d->state = DSTATE_DEAD;
			}
		}
	}
}

#if defined (DLOG_HAVE_LINUX)

static void
open_inotify(descriptor* d)
{
	if (d->state == DSTATE_INIT) {
		if ((d->fd = inotify_init1(IN_NONBLOCK|IN_CLOEXEC)) != -1) {
			if (evt_reg_read(d) == 0) {
				d->state = DSTATE_ACTIVE;
			}
		}
	}

	if (d->state != DSTATE_ACTIVE)
		d->state = DSTATE_DEAD;
}

#endif

