#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "strpartial.h"

/*
 * %{<number>}	-> capture group of enclosing block
 * %{var}		-> variable
 * %{d}			-> date/time string based on global strftime
 * %{s}			-> source symbol
 * %{m}			-> current log line, verbatim
 * %{t}			-> fractional second (resolution depends on configuration)
 * %{T}			-> Same as %{d}.%{t}
 */

static void
strpartial_from_format(const dynstr* sym, strpartial* str)
{
	char *endp;

	size_t slen = dynstr_len(sym);

	if (slen> 0)
	{
		if (isdigit(sym->str[0])) {
			int num = strtol(dynstr_ptr(sym), &endp, 10);
			if (*endp == '\0') {
				str->type = STR_CAPTURE_GROUP;
				str->value.number = num;
			} else {
				str->type = STR_INVALID;
			}
		} else if (slen == 1 && isalpha(sym->str[0])) {
			switch (sym->str[0]) {
			case 's':
				str->type = STR_SOURCE;
				break;
			case 'd':
				str->type = STR_DATETIME;
				break;
			case 'm':
				str->type = STR_LOGLINE;
				break;
			case 't':
				str->type = STR_FRACTSECOND;
				break;
			case 'T':
				str->type = STR_DATETIMEFRACT;
				break;
			default:
				str->type = STR_INVALID;
			}
		} else if (strncmp("env:", dynstr_ptr(sym), 4) == 0) {
			str->type = STR_ENV;
			str->value.rawstr = dynstr_new(dynstr_ptr(sym) + 4);
		} else {
			str->type = STR_VAR;
			str->value.rawstr = dynstr_copy(sym);
		}
	}
}

#define NEW_STRPART \
	do {\
		strpartial* new = calloc(1, sizeof(*root));\
		if (cur)\
			cur->next = new;\
		cur = new;\
		if (!root) root = cur;\
	} while(0)



strpartial
*strpartial_split(const char* sym)
{
	strpartial* root = NULL;
	strpartial* cur = NULL;

	size_t slen = strlen(sym);
	char* endptr = (char *)sym + slen;

	/* small optimisation */
	if (slen < 4) {
		root = calloc(1, sizeof(*root));
		root->value.rawstr = dynstr_new(sym);
		root->type = STR_VERBATIM;
		root->next = NULL;
		return root;
	}

	const char* left = sym;

	while (*left) {

		NEW_STRPART;

		const char* start = strstr(left, "%{");
		if (start) {
			const char* end = strchr(start, '}');
			if (end) {
				/* current raw text (if any) */
				if (start - left >= 1) {
					cur->value.rawstr = dynstr_cnew(left, start-1);
					cur->type = STR_VERBATIM;

					NEW_STRPART;
				}

				dynstr* pat = dynstr_cnew(start+2, end-1);
				strpartial_from_format(pat, cur);
				dynstr_free(pat);

				if (cur->type == STR_INVALID) {
					/* error condition, syntax error */
					strpartial_del(root);
					return NULL;
				}

				left = end+1;
				start = end = NULL;
				if (left > endptr)
					break;
			} else {
				/* brace not closed, syntax error */
				strpartial_del(root);
				return NULL;
			}
		} else {
			/* verbatim piece */
			cur->value.rawstr = dynstr_new(left);
			cur->type = STR_VERBATIM;
			cur->next = NULL;
			break;
		}
	}
	return root;
}

void
strpartial_del(strpartial* root)
{
	if (root->next)
		strpartial_del(root->next);

	if (root->type != STR_CAPTURE_GROUP)
		dynstr_free(root->value.rawstr);
	free(root);
}
