#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "log.h"
#include "lr.h"
#include "dynstr.h"

/* TODO enable custom line terminators (support for windows line ends) */
#define LINE_TERMINATOR '\n'
#define DEFAULTBUF_SIZE 1024


static linereader*
_reader_new_size(size_t bufsz)
{
	linereader* st = calloc(1, sizeof(*st));
	st->buf = dynstr_reserve(bufsz);
	return st;
}

linereader*
reader_new()
{
	return _reader_new_size(DYNSTR_USABLE_SIZE(DEFAULTBUF_SIZE));
}

void
reader_reset(linereader* r)
{
	dynstr_reset(r->buf);
	r->cur_idx = 0;
}

void
reader_reset_with_buffer(linereader* lr, const char* newbuf, int newidx)
{
	dynstr_free(lr->buf);
	LOG_DEBUG("Reader resetting with a buffer of size %d, index %d", strlen(newbuf), newidx);
	lr->cur_idx = newidx;
	lr->buf = dynstr_new(newbuf);
}

void
reader_destroy(linereader* r)
{
	if (r && r->buf)
		dynstr_free(r->buf);
	free(r);
}

/* return char* into the buffer and current slack (if smaller than required) */
char*
reader_get_buffer(linereader* r, int* min_size_hint)
{
	if (dynstr_slack(r->buf) < *min_size_hint) {
		r->buf = dynstr_resize(r->buf, r->buf->len + *min_size_hint+1);
	}

	return (char *)dynstr_endptr(r->buf);
}

void
reader_buffer_fill(linereader* r, int numbytes)
{
	dynstr_fill(r->buf, numbytes);
}

dynstr*
reader_get_next_line(linereader* r)
{
	char* nl = NULL;

	while (1) {
		nl = (char *)memchr(dynstr_ptr(r->buf) + r->cur_idx,
							LINE_TERMINATOR,
							r->buf->len - r->cur_idx);
		if (nl == &(r->buf->str[0]) + r->cur_idx) {
			r->cur_idx++;
			continue;
		}
		break;
	}

	if (!nl) {
		r->cur_idx = r->buf->len;
		return NULL;
	} else {
		/* Keep the new line character in */
		dynstr* line = dynstr_cnew(dynstr_ptr(r->buf), nl);
		size_t range = (ptrdiff_t)(nl - dynstr_ptr(r->buf));
		r->buf = dynstr_remove_range(r->buf, 0, range);
		r->cur_idx = 0;
		return line;
	}
}

dynstr* reader_raw_buffer(linereader* r, int* idx)
{
	*idx = r->cur_idx;
	return r->buf;
}

