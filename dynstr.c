#include "dynstr.h"
#include "arena.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* externally defined allocators for run-time dynstr */
extern dynstr* alloc_dynstr_alloc(int sz, int* real_capacity);
extern void alloc_dynstr_free(dynstr* );
extern dynstr* alloc_dynstr_realloc(dynstr*, int newsize, int* new_capacity);

/* internals */
static dynstr*
_alloc(int sz)
{
	int real_cap;
	dynstr* s = alloc_dynstr_alloc(DYNSTR_TOTALSZ(sz), &real_cap);
	s->cap = DYNSTR_USABLE_SIZE(real_cap);
	return s;
}
static void
_free(dynstr* d)
{
	alloc_dynstr_free(d);
}
static dynstr*
_realloc(dynstr* d, int newsz)
{
	int new_cap;
	dynstr* s = alloc_dynstr_realloc(d, DYNSTR_TOTALSZ(newsz), &new_cap);
	s->cap = DYNSTR_USABLE_SIZE(new_cap);
	return s;
}

/***********************************************************/

dynstr*
dynstr_resize(dynstr* str, int new_len)
{
	if (new_len < str->cap)
		return str;

	dynstr* n = _realloc(str, new_len);
	return n;
}

dynstr*
dynstr_reserve(int capacity)
{
	dynstr* s = _alloc(capacity);
	s->len = 0;
	// capacity is filled from allocator
	//s->cap = capacity;
	s->str[0] = 0;
	return s;
}

dynstr*
dynstr_new(const char* src)
{
	if (src && *src) {
		return dynstr_cnew(src, src + strlen(src)-1);
	} else {
		return dynstr_reserve(DYNSTR_MIN_SIZE);
	}
}

/*
 * copy [start, end] (inclusive range)
 */
dynstr*
dynstr_cnew(const char* start, const char* end)
{
	ptrdiff_t len = end - start + 1;
	dynstr* s = dynstr_reserve(len);
	strncpy(s->str, start, len);
	s->len = len;
	s->str[len] = 0;
	return s;
}

void
dynstr_free(dynstr* s)
{
	if (s)
		_free(s);
}

void
dynstr_reset(dynstr* s)
{
	s->len = 0;
	s->str[0] = 0;
}

const char*
dynstr_ptr(const dynstr* dstr)
{
	return dstr->str;
}

const char*
dynstr_endptr(const dynstr* dstr)
{
	return dstr->str + dstr->len;
}

char*
dynstr_wendptr(const dynstr* str)
{
	return (char *)dynstr_endptr(str);
}

int
dynstr_len(const dynstr* s)
{
	return s->len;
}

bool
dynstr_empty(const dynstr* s)
{
	return s->len == 0;
}

int
dynstr_cmp(const dynstr* s1, const dynstr* s2)
{
	return (memcmp(s1->str, s2->str, dlog_min(s1->len, s2->len)));
}

int
dynstr_ccmp(const dynstr* s1, const char* s2)
{
	return (memcmp(s1->str, s2, dlog_min(s1->len, (int)strlen(s2))));
}

dynstr*
dynstr_assign(dynstr* s, const char* src)
{
	size_t len = strlen(src);
	s = dynstr_resize(s, len);
	strncpy(s->str, src, len+1);
	s->len = len;
	s->str[len] = 0;
	return s;
}

dynstr*
dynstr_insert(dynstr* into, const dynstr* needle, int index)
{
	if (index >= into->len) {
		return dynstr_cat(into, needle);
	} else {
		dynstr* newstr = dynstr_resize(into, into->len + needle->len);
		dynstr* tail = dynstr_cnew(newstr->str + index, newstr->str + newstr->len - 1);
		newstr->len = index;
		newstr->str[index] = 0;
		newstr = dynstr_cat(newstr, needle);
		newstr = dynstr_cat(newstr, tail);
		dynstr_free(tail);
		return newstr;
	}
}

dynstr*
dynstr_ccat_range(dynstr* dst, const char* start, const char* end)
{
	dynstr* news = dynstr_cnew(start, end);
	dst = dynstr_cat(dst, news);
	dynstr_free(news);
	return dst;
}

dynstr*
dynstr_ccat(dynstr* dst, const char* s)
{
	return dynstr_ccat_range(dst, s, s+strlen(s)-1);
}

dynstr*
dynstr_ncat(dynstr* dst, int num)
{
	char* s;
	asprintf(&s, "%d", num);
	dynstr* d = dynstr_ccat(dst, s);
	free(s);
	return d;
}

dynstr*
dynstr_cat(dynstr* dst, const dynstr* src)
{
	/* concatenate strings, both remain allocated */
	dst = dynstr_resize(dst, dst->len + src->len);
	memcpy(dst->str + dst->len, src->str, src->len);
	dst->len += src->len;
	dst->str[dst->len] = 0;
	return dst;
}

dynstr*
dynstr_mcat(dynstr* dst, dynstr* src)
{
	/* "moves" content, destroys 'src' */
	dst = dynstr_cat(dst, src);
	dynstr_free(src);
	return dst;
}

dynstr*
dynstr_remove_range(dynstr* s, int idx1, int idx2)
{
	/* in place removal, remove [idx1, idx2], inclusive */
	/* shortcuts */
	if (idx1 == 0 && idx2 >= s->len) {
		dynstr_reset(s);
		return s;
	}

	if (!idx2 || idx2 < idx1) {
		s->len = idx1-1;
		s->str[idx1] = 0;
		return s;
	}

	memmove(&s->str[idx1], &s->str[idx2+1], s->len - idx2 - 1);
	s->len = s->len - (idx2 - idx1 + 1);
	s->str[s->len] = 0;
	return s;
}

dynstr*
dynstr_copy(const dynstr* src)
{
	return dynstr_new(dynstr_ptr(src));
}

int
dynstr_in_range(const dynstr* s, const char* ptr)
{
	/* check single pointer - not the whole string!*/
	return ((ptr > s->str) &&
			(s->str + s->len > ptr));
}

/*
 * Fill - the buffer has been filled externally, so move
 * the length by 'numbytes' bytes
 * NB. We assume buffer has not been breached here!
 */
void
dynstr_fill(dynstr* s, int numbytes)
{
	s->len += numbytes;
	s->str[s->len] = '\0';
}

dynstr*
dynstr_padright(dynstr* str, size_t pad)
{
	/* make sure there is 'pad' free on the right */
	return dynstr_resize(str, dynstr_len(str) + pad);
}

dynstr*
cstr_cast(const char* s)
{
	return (dynstr*)(s - offsetof(dynstr, str[0]));
}

int
dynstr_slack(const dynstr* s)
{
	return s->cap - s->len - 1;
}

bool
dynstr_isnewline(const dynstr* s)
{
	if (s->len)
		return s->str[s->len-1] == '\n';
	else
		return false;
}
