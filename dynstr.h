#ifndef _DLOG_DYNSTR_H__
#define _DLOG_DYNSTR_H__
#include "def.h"
#include <stddef.h>

#define DYNSTR_MIN_SIZE 16
#define DYNSTR_TOTALSZ(len) (sizeof(dynstr) + len + 1)
#define DYNSTR_USABLE_SIZE(sz) ((sz) - sizeof(dynstr))

typedef struct
{
	int len;
	int cap;
	char str[];
} dynstr;

dynstr*		dynstr_reserve(int size);
dynstr*		dynstr_new(const char* src_or_null);
dynstr*		dynstr_cnew(const char* start, const char* end);
void		dynstr_free(dynstr* s);

void		dynstr_reset(dynstr* s);

const char*	dynstr_ptr(const dynstr* dstr);
const char* dynstr_endptr(const dynstr*);
char*		dynstr_wendptr(const dynstr*);
int			dynstr_len(const dynstr* );
int			dynstr_cap(const dynstr* );
bool		dynstr_empty(const dynstr*);
int			dynstr_cmp(const dynstr* s1, const dynstr* s2);
int			dynstr_ccmp(const dynstr* s1, const char* s2);
dynstr*		dynstr_assign(dynstr* s, const char* src);
dynstr*		dynstr_insert(dynstr* base, const dynstr* needle, int index);
dynstr*		dynstr_ccat_range(dynstr* dst, const char* start, const char* end);
dynstr*		dynstr_ccat(dynstr* dst, const char* string);
dynstr*		dynstr_ncat(dynstr* dst, int num);
dynstr*		dynstr_cat(dynstr* dst, const dynstr* src);
dynstr*		dynstr_mcat(dynstr* dst, dynstr* src);
dynstr*		dynstr_remove_range(dynstr* str, int idx1, int idx2);
dynstr*		dynstr_copy(const dynstr* src);
int			dynstr_in_range(const dynstr* s, const char* check);
dynstr*		dynstr_resize(dynstr* str, int new_len);
dynstr*		dynstr_padright(dynstr* str, size_t padding);
void		dynstr_fill(dynstr* s, int numbytes);
dynstr*		cstr_cast(const char*);
int			dynstr_slack(const dynstr*);

bool		dynstr_isnewline(const dynstr*);

#endif

