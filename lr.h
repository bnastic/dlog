#ifndef _DLOG_LINE_READER_H__
#define _DLOG_LINE_READER_H__
#include "dynstr.h"

typedef struct linereader
{
	int		cur_idx;
	dynstr	*buf;
} linereader;

linereader* reader_new();
void reader_reset(linereader*);
void reader_reset_with_buffer(linereader*, const char* , int);
void reader_destroy(linereader*);

/* API 2 - retrieve internal buffer to be used for read() call. Suggest min. size */
char* reader_get_buffer(linereader*, int* min_size_hint);
void  reader_buffer_fill(linereader*, int numbytes);

/* extract a line from buffer (return NULL if no full line found) */
dynstr* reader_get_next_line(linereader*);
dynstr* reader_raw_buffer(linereader*, int* idx);

#endif
