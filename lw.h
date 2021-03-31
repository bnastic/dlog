#ifndef _DLOG_LINE_WRITER_H__
#define _DLOG_LINE_WRITER_H__
#include <sys/uio.h>

#include "def.h"
#include "dynstr.h"

struct writequeue
{
	dynstr* line[DLOG_WRITE_HIGH_WM];
	struct iovec iov[DLOG_WRITE_HIGH_WM];
	int num_entries;
	size_t write_off;
};

struct writequeue* wq_new(void);
void wq_destroy(struct writequeue *);
int wq_add_line(struct writequeue* , dynstr* );
ssize_t wq_write(struct writequeue* , int fd, int* errcode);

#endif

