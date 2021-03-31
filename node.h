#ifndef DLOG_NODE_H__
#define DLOG_NODE_H__
#include "def.h"
#include "patterns.h"
#include "strpartial.h"
#include "dynstr.h"

typedef void (*write_line_cb)(dynstr* symbol, dynstr* line);


/*
 * current execution context
 */
struct exec_ctx
{
	struct str_match*	 re_match;
	const char			*datetime;
	long				 fract_sec;
	const dynstr		*source;
	const dynstr		*line;

	write_line_cb		write_cb;
};

typedef enum {
	NODE_PASSTHROUGH,
	NODE_ASSIGN,
	NODE_BREAK,
	NODE_MATCH,
	NODE_MATCHALL,
	NODE_MELSE,
	NODE_WRITE
} node_type;

/*
 * node contexts
 */

struct node_ctx_assign
{
	dynstr			*var;
	strpartial      *pattern;
};

struct node_ctx_match
{
	strpartial			*re_pattern;
	strpartial			*target;
	dynstr				*source;
	struct str_match	 cur_match;
	struct str_match	*prev_match;
};

struct node_ctx_matchall
{
	dynstr			*source;
};

struct node_ctx_write
{
	strpartial *string_fmt;
	dynstr* dest_sym;
};

struct node
{
	struct node	*child;
	struct node	*parent;
	struct node	*sibling;

	struct {
		int type;
		union {
			struct node_ctx_assign		assign;
			struct node_ctx_match		match;
			struct node_ctx_matchall	matchall;
			struct node_ctx_write		nwrite;
		};
	} context;
};


/* entry point */
void node_eval_root(struct node* root, const dynstr* line, const dynstr* source, write_line_cb);
void node_destroyall(struct node* root);
void print_node_tree(struct node* root);

#endif

