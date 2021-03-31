#ifndef DLOG_STRPARTIAL_H__
#define DLOG_STRPARTIAL_H__
#include "def.h"
#include "dynstr.h"


typedef enum {
	STR_INVALID = 0,
	STR_VERBATIM,
	STR_VAR,
	STR_ENV,
	STR_CAPTURE_GROUP,
	STR_DATETIME,
	STR_FRACTSECOND,
	STR_DATETIMEFRACT,
	STR_SOURCE,
	STR_LOGLINE,
} partial_type;

typedef struct strpartial
{
	int type;
	union {
		dynstr* rawstr;
		int number;
	} value;

	struct strpartial* next;
} strpartial;

strpartial*	strpartial_split(const char* sym);
void		strpartial_del(strpartial* root);

#endif

