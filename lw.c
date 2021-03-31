#include <stdlib.h>
#include <string.h>
#include "lw.h"
#include "log.h"


struct writequeue* wq_new(void)
{
	return calloc(1, sizeof(struct writequeue));
}

void wq_destroy(struct writequeue *wq)
{
	if (!wq)
		return;

	for (int i=0; i<wq->num_entries; i++) {
		dynstr_free(wq->line[i]);
	}
	wq->num_entries = 0;
	wq->write_off = 0;
}

int wq_add_line(struct writequeue* wq, dynstr* ln)
{
	if (wq->num_entries == DLOG_WRITE_HIGH_WM) {
		LOG_ERROR("High watermark reached, ignoring new lines");
		return -1;
	}

	wq->line[wq->num_entries++] = ln;
	return 0;
}

ssize_t wq_write(struct writequeue* wq, int fd, int* errcode)
{
	int i=0, len=0, r, bytes;
	*errcode = 0;

	if (!wq->num_entries)
		return (0);

	for (int i=0; i<wq->num_entries; i++) {
		wq->iov[i].iov_base = (void *)dynstr_ptr(wq->line[i]);
		wq->iov[i].iov_len  = dynstr_len(wq->line[i]);
	}

	bytes = r = writev(fd, wq->iov, wq->num_entries);

	if (-1 == r && errno != EAGAIN && errno != EWOULDBLOCK) {
		LOG_SYS_ERROR("writev() failed");
		*errcode = errno;
		return -1;
	} else if (r > 0) {

		while(1) {
			len = dynstr_len(wq->line[i]);

			if (r < len) {
				break;
			} else if (r >= len) {
				dynstr_free(wq->line[i]);
				r -= len;
				if (!r)
					break;
			}
			i++;
		}
		if (i>0) {
			memmove(&wq->line[0], &wq->line[i], wq->num_entries * sizeof(dynstr *));
		}
		wq->num_entries -= i+1;
	}

	return bytes;
}
