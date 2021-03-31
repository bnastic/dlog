#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include "node.h"
#include "hashtable.h"
#include "env.h"
#include "log.h"

typedef enum
{
	EVAL_TRUE,
	EVAL_FALSE,
	EVAL_BREAK,
	EVAL_ERROR
} eval_result;

static eval_result	_eval_node_passthrough(struct node*, struct exec_ctx*, eval_result prev_res);
static eval_result  _eval_node_assign(struct node*, struct exec_ctx*, eval_result prev_res);
static eval_result	_eval_node_match(struct node*, struct exec_ctx*, eval_result prev_res);
static eval_result	_eval_node_matchall(struct node*, struct exec_ctx*, eval_result prev_res);
static eval_result	_eval_node_match_else(struct node*, struct exec_ctx*, eval_result prev_res);
static eval_result	_eval_node_write(struct node*, struct exec_ctx*, eval_result prev_res);
static eval_result	_eval_node_break(struct node*, struct exec_ctx*, eval_result prev_res);
static eval_result	_eval_node(struct node*, struct exec_ctx*, eval_result prev_res);
static void			_match_outofscope(struct node*, struct exec_ctx*);
static void			_del_assign(struct node*);
static void			_del_block_match(struct node*);
static void			_del_match(struct node*);
static void			_del_matchall(struct node*);
static void			_del_write(struct node*);


struct node_handler
{
	node_type type;
	eval_result	(*eval)(struct node*, struct exec_ctx*, eval_result);
	void (*node_cleanup)(struct node *, struct exec_ctx*);
	void (*block_cleanup)(struct node *);
	void (*final_cleanup)(struct node *);
} _handler[] =
{
	{ NODE_PASSTHROUGH,	_eval_node_passthrough,	NULL,				NULL, NULL },
	{ NODE_ASSIGN,		_eval_node_assign,      NULL,				NULL, _del_assign},
	{ NODE_BREAK,		_eval_node_break,		NULL,				NULL, NULL },
	{ NODE_MATCH,		_eval_node_match,		_match_outofscope,	_del_block_match, _del_match },
	{ NODE_MATCHALL,	_eval_node_matchall,	_match_outofscope,	NULL, _del_matchall },
	{ NODE_MELSE,		_eval_node_match_else,	NULL,				NULL, NULL },
	{ NODE_WRITE,		_eval_node_write,		NULL,				NULL, _del_write }
};

#define COPY_STRMATCH(to, from) \
			memcpy(&(to), &(from), sizeof(struct str_match))

#define NIL_STRMATCH(sm) \
			(sm).sm_nmatch = 0L

/* aux function to print the node tree when checking config */
static void
_print_tree(struct node* n, int indent)
{
	char ind[128] = {0}; for (int i=0; i<indent && i<128; i++) ind[i]=' ';
	char* str1 = NULL, *str2 = NULL;

	switch (n->context.type) {
		case NODE_PASSTHROUGH: str1="PASSTHROUGH"; str2=""; break;
		case NODE_ASSIGN:
		{
			str1="ASSIGN"; str2=n->context.assign.var->str;
		} break;
		case NODE_BREAK:
		{
			str1="BREAK"; str2="";
		} break;
		case NODE_MATCH:
		{
			str1="MATCH"; if(n->context.match.source) str2=n->context.match.source->str; else str2 = "";
		} break;
		case NODE_MATCHALL:
		{
			str1="MATCHALL"; if(n->context.matchall.source) str2=n->context.matchall.source->str; else str2 = "";
		} break;
		case NODE_MELSE:
		{
			str1="ELSE"; str2="";
		} break;
		case NODE_WRITE:
		{
			str1="WRITE"; str2=n->context.nwrite.dest_sym->str;
		} break;
	}
	LOG_INFO("%s Node: %s %s", ind, str1, str2);
	if (n->child) {
		_print_tree(n->child, indent+2);
	}
	if (n->sibling) {
		_print_tree(n->sibling, indent);
	}
}

static dynstr
*strpartial_resolve(strpartial* part, struct exec_ctx* ctx)
{
	char fract[10], *env;
	dynstr *v;
	dynstr *str = dynstr_new(NULL);

	while(part) {
		switch (part->type) {

			case STR_INVALID:
				goto fail;

			case STR_VERBATIM:
				str = dynstr_cat(str, part->value.rawstr);
				break;

			case STR_VAR:
				v = ht_find(dlogenv->vars_table,
							(uintptr_t)part->value.rawstr);
				if (v)
					str = dynstr_cat(str, v);
				break;

			case STR_ENV:

				env = getenv(dynstr_ptr(part->value.rawstr));
				dynstr_reset(part->value.rawstr);
				dynstr_mcat(part->value.rawstr, dynstr_new(env));
				part->type = STR_VERBATIM;
				str = dynstr_cat(str, part->value.rawstr);
				break;

			case STR_CAPTURE_GROUP:
				if ((int)ctx->re_match->sm_nmatch > part->value.number) {
					str = dynstr_ccat(str, ctx->re_match->sm_match[part->value.number]);
				}
				break;

			case STR_DATETIME:
				str = dynstr_ccat(str, ctx->datetime);
				break;

			case STR_SOURCE:
				str = dynstr_cat(str, ctx->source);
				break;

			case STR_LOGLINE:
				str = dynstr_cat(str, ctx->line);
				break;

			case STR_FRACTSECOND:
				str = dynstr_padright(str, 10);
				int ret = snprintf(dynstr_wendptr(str), 10, "%ld", ctx->fract_sec);
				dynstr_fill(str, ret);
				break;

			case STR_DATETIMEFRACT:
				str = dynstr_ccat(str, ctx->datetime);
				snprintf(fract, sizeof(fract)-1, "%ld", ctx->fract_sec);
				fract[sizeof(fract)-1] = 0;
				str = dynstr_ccat(str, ".");
				str = dynstr_ccat(str, fract);
				break;

			default:
				goto fail;
		}
		part = part->next;
	}

	return str;

fail:
	dynstr_free(str);
	return NULL;
}

static eval_result
_eval_node(struct node* n, struct exec_ctx* ctx, eval_result prev_res)
{
	eval_result topr, r;

	topr = _handler[n->context.type].eval(n, ctx, prev_res);

	if (topr == EVAL_BREAK || topr == EVAL_ERROR) {
		goto end;
	}

	if (EVAL_TRUE == topr && n->child) {

		r = _eval_node(n->child, ctx, topr);

		if (_handler[n->context.type].node_cleanup)
			_handler[n->context.type].node_cleanup(n, ctx);

		if (r == EVAL_BREAK || r == EVAL_ERROR) {
			/* don't let breaks escape this block */
			goto end;
		}
	}

	if (n->sibling) {
		r = _eval_node(n->sibling, ctx, topr);
	}

end:
	if (_handler[n->context.type].block_cleanup)
		_handler[n->context.type].block_cleanup(n);
	return topr;
}

static eval_result
_eval_node_assign(struct node* n, struct exec_ctx* ex_ctx, eval_result prev_res)
{
	struct node_ctx_assign* ctx = &n->context.assign;
	dynstr* res = strpartial_resolve(ctx->pattern, ex_ctx);
	if (res) {
		ht_upsert(dlogenv->vars_table, (intptr_t)ctx->var, res);
		return EVAL_TRUE;
	}

	return EVAL_ERROR;
}

static eval_result
_eval_node_match(struct node* n, struct exec_ctx* ex_ctx, eval_result prev_res)
{
	const char* err = NULL;
	eval_result ret;

	struct node_ctx_match* ctx = &n->context.match;
	struct str_match *sm = &ctx->cur_match;

	dynstr* re = strpartial_resolve(ctx->re_pattern, ex_ctx);
	dynstr* t = strpartial_resolve(ctx->target, ex_ctx);

	ctx->prev_match = ex_ctx->re_match;

	if (re && t) {
		if (!ctx->source || (ctx->source && !dynstr_cmp(ctx->source, ex_ctx->source))) {
			if (-1 == str_match(dynstr_ptr(t), dynstr_ptr(re), sm, &err)) {
				if (err) {
					free((void *)err);
					ret = EVAL_ERROR;
				} else {
					ret = EVAL_FALSE;
				}
			} else {
				/* save current regex context into global context */
				ex_ctx->re_match = sm;
				ret = sm->sm_nmatch > 0 ? EVAL_TRUE : EVAL_FALSE;
			}
		} else {
			ret = EVAL_FALSE;
		}
	}
	else {
		ret = EVAL_ERROR;
	}

	dynstr_free(re);
	dynstr_free(t);

	return ret;
}

static eval_result
_eval_node_matchall(struct node* n, struct exec_ctx* ex_ctx, eval_result prev_res)
{
	eval_result ret = EVAL_FALSE;
	struct node_ctx_matchall* ctx = &n->context.matchall;

	if (!ctx->source || (ctx->source && !dynstr_cmp(ctx->source, ex_ctx->source))) {
		ret = EVAL_TRUE;
	}

	return ret;
}

static eval_result
_eval_node_match_else(struct node* n, struct exec_ctx* ex_ctx, eval_result prev_res)
{
	return prev_res == EVAL_FALSE ? EVAL_TRUE : EVAL_FALSE;
}

static eval_result
_eval_node_write(struct node* n, struct exec_ctx* ex_ctx, eval_result prev_res)
{
	dynstr* val = strpartial_resolve(n->context.nwrite.string_fmt, ex_ctx);
	dynstr* dst = n->context.nwrite.dest_sym;

	if (val && dst) {
		ex_ctx->write_cb(dst, val);
	} else {
		LOG_ERROR("NODE_WRITE failed to resolve format and/or destination");
	}

	return EVAL_TRUE;
}

static eval_result
_eval_node_passthrough(struct node* n, struct exec_ctx* ex_ctx, eval_result prev_res)
{
	return EVAL_TRUE;
}

static eval_result
_eval_node_break(struct node* n, struct exec_ctx* ex_ctx, eval_result prev_res)
{
	return EVAL_BREAK;
}

static void
_del_assign(struct node* n)
{
	dynstr_free(n->context.assign.var);
	strpartial_del(n->context.assign.pattern);
}

static void
_match_outofscope(struct node* n, struct exec_ctx* ctx)
{
	ctx->re_match = n->context.match.prev_match;
	str_match_free(&n->context.match.cur_match);
}

static void
_del_block_match(struct node* n)
{
	str_match_free(&n->context.match.cur_match);
}

static void
_del_match(struct node* n)
{
	strpartial_del(n->context.match.re_pattern);
	strpartial_del(n->context.match.target);
	dynstr_free(n->context.match.source);
	str_match_free(&n->context.match.cur_match);
}

static void
_del_matchall(struct node* n)
{
	dynstr_free(n->context.matchall.source);
}

static void
_del_write(struct node*n )
{
	strpartial_del(n->context.nwrite.string_fmt);
	dynstr_free(n->context.nwrite.dest_sym);
}


void
node_destroyall(struct node* root)
{
	if (!root)
		return;

	struct node* ch = root->child;
	struct node* sib = root->sibling;

	if (_handler[root->context.type].final_cleanup)
		_handler[root->context.type].final_cleanup(root);

	free(root);
	node_destroyall(ch);
	node_destroyall(sib);
}

void
node_eval_root(struct node* root, const dynstr* line, const dynstr* source_sym, write_line_cb wcb)
{
	struct tm tme;
	struct timespec ts;

	char tbuf[1024];

	if (unlikely(-1 == clock_gettime(CLOCK_REALTIME, &ts))) {
		LOG_SYS_ERROR("node_eval_root failed to acquire time, bailing.");
		return;
	}

	localtime_r(&ts.tv_sec, &tme);
	strftime(tbuf, sizeof(tbuf), dlogenv->config.datetime_format, &tme);

	struct exec_ctx ctx = {
		.re_match = NULL,
		.datetime = tbuf,
		.fract_sec = ts.tv_nsec / dlogenv->config.fractsec_divider,
		.source = source_sym,
		.line = line,
		.write_cb = wcb
	};

	(void) _eval_node(root, &ctx, EVAL_TRUE);
}

void
print_node_tree(struct node* root)
{
	_print_tree(root, 0);
}

