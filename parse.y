%{
#include <sys/types.h>
#include <sys/queue.h>
#include <ctype.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <err.h>

#include "def.h"
#include "log.h"
#include "hashtable.h"
#include "strpartial.h"
#include "coredesc.h"
#include "node.h"
#include "env.h"

// #define YYDEBUG 1

int yyparse(void);
void yyerror(const char *, ...);
int yylex(void);
int parse_config(void);

static void node_add_child(struct node* parent, struct node* n);
static int tstring_is_symbol(const char* s);
static void add_origin(struct dorigin* or);
static dynstr *strpartial_resolve_ex(const strpartial* part);
static bool strpartial_isstatic(const strpartial* part, bool allow_vars);

struct yystype_t
{
	char* v;
	int is_quoted;
	int is_symbol;
};

#define YYSTYPE struct yystype_t

/* file related stuff */
TAILQ_HEAD(cfgfiles, cfgfile)  cfgfiles = TAILQ_HEAD_INITIALIZER(cfgfiles);

static struct cfgfile
{
	TAILQ_ENTRY(cfgfile) entry;
	FILE* fp;
	char* filename;
	int lineno;
} *yyfp;


struct cfgfile*
cfgfile_include(const char* filename)
{
	struct cfgfile *nfile = calloc(1, sizeof(*nfile));
	if (!(nfile->fp = fopen(filename, "r"))) {
		LOG_SYS_ERROR("fopen failed for %s", filename);
		return (NULL);
	}

	nfile->filename = strdup(filename);
	TAILQ_INSERT_TAIL(&cfgfiles, nfile, entry);
	return nfile;
}

void
cfgfile_pop()
{
	struct cfgfile* f;

	fclose(yyfp->fp);
	free(yyfp->filename);

	f = TAILQ_PREV(yyfp, cfgfiles, entry);
	TAILQ_REMOVE(&cfgfiles, yyfp, entry);
	yyfp = f;
}

/* nodes */
static struct node	*rootnode;
static struct node	*curblock;
static struct node	*curnode;

/* hashtable deep-copies the key (dynstr) */
#define ADD_VAR(char_sym, val) \
	do {\
		dynstr* _sym = dynstr_new((char_sym));\
		ht_upsert(dlogenv->vars_table, (uintptr_t)(_sym), (val));\
		dynstr_free(_sym);\
	}while(0)

#define CHECK_PARTIAL(var, arg) \
	do { var = strpartial_split(arg.v);\
	if (!var) {\
		yyerror("invalid format (%s)", arg.v);\
		strpartial_del(var);\
		YYABORT;\
	} }while(0)

#define CHECK_PARTIAL_STATIC(var, arg) \
	do { var = strpartial_split(arg.v);\
	if (!var) {\
		yyerror("invalid format (%s)", arg.v);\
		strpartial_del(var);\
		YYABORT;\
	} else {\
		if (!strpartial_isstatic(var, true)) {\
			yyerror("String not static (%s)", arg.v);\
			strpartial_del(var);\
			YYABORT;\
		}\
	}}while(0)


#define CHECK_SYMBOL(arg) \
	do {if (arg.is_quoted || !(arg.is_symbol)) {\
		yyerror("invalid symbol (%s)", arg.v);\
		YYABORT;\
	}} while(0)


#define ADD_NODE(n, typ)\
		do { n = calloc(1, sizeof(struct node));\
		n->context.type = typ;\
		node_add_child(curblock, n); }while(0)

#define ADD_NODE_WPARENT(n, typ)\
		do { n = calloc(1, sizeof(struct node));\
		n->context.type = typ;\
		node_add_child(curblock->parent, n);\
		node_add_child(n, curblock);\
		curblock = curblock->parent; }while(0)

%}

%token TINCLUDE TPIDFILE TLOGFILE TLISTEN TDATETIMEFORMAT TTIMESTAMPRES TSOURCE TDESTINATION
%token TTCP TFILE TFIFO TMAXSIZE TROTLOG
%token TRULE TMATCH TMATCHALL TFROM TELSE TWRITE TBREAK TVAR TAS
%token T__INVALID__
//%token <v.string> TSTRING
%token TSTRING

%%

commands:
	|
	commands command
	;

command:
	var ';'
	| config_cmd ';'
	| include
	| descriptor_cmd ';'
	| rule_cmd
	;

var:
	TVAR TSTRING '=' TSTRING {
	/* var  <symbol> = <value (strpartial)> */
		strpartial* val;

		CHECK_SYMBOL($2);
		CHECK_PARTIAL(val, $4);
		LOG_DEBUG("Adding VAR for symbol %s", $2.v);

		ADD_VAR($2.v, val);
	}
	;

include:
	TINCLUDE TSTRING ';' {
		strpartial* val;
		dynstr* fl;
		CHECK_PARTIAL_STATIC(val, $2);
		fl = strpartial_resolve_ex(val);
		if (fl && access(dynstr_ptr(fl), R_OK) == 0) {
			yyfp = cfgfile_include(dynstr_ptr(fl));
		} else {
			yyerror("Include file not found (%s)", dynstr_ptr(fl));
			YYABORT;
		}
	}
	;

descriptor_cmd:
	TSOURCE TFILE TSTRING TAS TSTRING {
	/*source file <path (partial_ex)> as <symbol> */
		strpartial *f;
		dynstr *filename;
		CHECK_PARTIAL_STATIC(f, $3);
		CHECK_SYMBOL($5);

		filename = strpartial_resolve_ex(f);

		struct dorigin* or = calloc(1, sizeof(*or));
		or->type = D_FILER;
		or->symbol = strdup($5.v);
		or->file.path = strdup(dynstr_ptr(filename));
		add_origin(or);

		dynstr_free(filename);
		strpartial_del(f);
	}
	|
	TSOURCE TFIFO TSTRING TAS TSTRING {
	/*source fifo <path (partial_ex)> as <symbol> */
		strpartial *f;
		dynstr *fifopath;
		CHECK_PARTIAL_STATIC(f, $3);
		CHECK_SYMBOL($5);

		fifopath = strpartial_resolve_ex(f);

		struct dorigin* or = calloc(1, sizeof(*or));
		or->type = D_FIFOR;
		or->symbol = strdup($5.v);
		or->file.path = strdup(dynstr_ptr(fifopath));
		add_origin(or);

		dynstr_free(fifopath);
		strpartial_del(f);
	}
	|
	TDESTINATION TFILE TSTRING TAS TSTRING {

		strpartial *f;
		dynstr *filename;
		CHECK_PARTIAL_STATIC(f, $3);
		CHECK_SYMBOL($5);

		filename = strpartial_resolve_ex(f);

		if (!filename) {
			yyerror("Invalid filename for symbol (%s)", $5.v);
			YYABORT;
		}

		struct dorigin* or = calloc(1, sizeof(*or));
		or->type = D_FILEW;
		or->symbol = strdup($5.v);
		or->file.path = strdup(dynstr_ptr(filename));
		add_origin(or);

		LOG_DEBUG("Adding destination file %s (%s)", or->file.path, or->symbol);

		dynstr_free(filename);
		strpartial_del(f);
	}
	|
	TDESTINATION TFIFO TSTRING TAS TSTRING {
	/* destination fifo <path/strpartial> as <symbol> */
		strpartial *f;
		dynstr *fifopath;
		CHECK_PARTIAL_STATIC(f, $3);
		CHECK_SYMBOL($5);

		fifopath = strpartial_resolve_ex(f);

		if (!fifopath) {
			yyerror("Invalid filename for symbol (%s)", $5.v);
			YYABORT;
		}

		struct dorigin* or = calloc(1, sizeof(*or));
		or->type = D_FIFOW;
		or->symbol = strdup($5.v);
		or->file.path = strdup(dynstr_ptr(fifopath));
		add_origin(or);
		LOG_DEBUG("Adding destination FIFO %s (%s)", or->file.path, or->symbol);

		dynstr_free(fifopath);
		strpartial_del(f);
	}
	|
	TDESTINATION TROTLOG TSTRING TSTRING TAS TSTRING {
	/* destination rotlog <file path> <max file size as string> as <symbol> */

		strpartial *f;
		dynstr *filename;
		CHECK_PARTIAL_STATIC(f, $3);
		CHECK_SYMBOL($6);

		filename = strpartial_resolve_ex(f);

		long maxsizebytes = strtol($4.v, NULL, 10);
		if (!maxsizebytes && errno == EINVAL) {
			yyerror("Invalid file size (%s)", $4.v);
			YYABORT;
		}

		struct dorigin* or = calloc(1, sizeof(*or));
		or->type = D_ROTLOG;
		or->symbol = strdup($6.v);
		or->file.path = strdup(dynstr_ptr(filename));
		or->file.size = maxsizebytes;
		add_origin(or);
		LOG_DEBUG("Adding destination Rotlog %s (%s)", or->file.path, or->symbol);

		dynstr_free(filename);
		strpartial_del(f);
	}
	|
	TDESTINATION TTCP TSTRING TSTRING TAS TSTRING {
	/* destination tcp <host> <port> as <symbol> */

		strpartial *host, *port;
		dynstr *shost, *sport;

		CHECK_PARTIAL_STATIC(host, $3);
		CHECK_PARTIAL_STATIC(port, $4);
		CHECK_SYMBOL($6);

		shost = strpartial_resolve_ex(host);
		sport = strpartial_resolve_ex(port);

		struct dorigin* or = calloc(1, sizeof(*or));
		or->type = D_SOCKETW;
		or->symbol = strdup($6.v);
		or->socket.host = strdup(dynstr_ptr(shost));
		or->socket.port = strdup(dynstr_ptr(sport));
		add_origin(or);

		dynstr_free(shost);
		dynstr_free(sport);
		strpartial_del(host);
		strpartial_del(port);
	}
	;

config_cmd:
	TPIDFILE TSTRING {
		free(dlogenv->config.pidfile);
		dlogenv->config.pidfile = strdup($2.v);
	}
	| TLOGFILE TSTRING {
		free(dlogenv->config.logfile);
		dlogenv->config.logfile = strdup($2.v);
	}
	| TLISTEN TSTRING {
		/* don't overwrite the port that came from cmd line */
		if (!dlogenv->config.listenskt_port) {
			dlogenv->config.listenskt_port = strdup($2.v);
		}
	}
	| TDATETIMEFORMAT TSTRING {
		free(dlogenv->config.datetime_format);
		dlogenv->config.datetime_format = strdup($2.v);
	}
	| TTIMESTAMPRES TSTRING {
		int div = DLOG_DEFAULT_FRACTSEC_DIV;
		if (!strcmp($2.v, "millisecond")) {
			div = 1000000;
		} else if (!strcmp($2.v, "microsecond")) {
			div = 1000;
		} else if (!strcmp($2.v, "nanosecond")) {
			div = 1;
		} else if (!strcmp($2.v, "none")) {
			/* nothing */
		} else {
			yyerror("invalid value for timestamp resolution (%s)", $2.v);
			YYABORT;
		}
		dlogenv->config.fractsec_divider = div;
	}
	;

rule_cmd:
	TRULE block {
	}
	;

rule_statements:
	| rule_statements rule_statement
	;

rule_statement:
	match_block
	| else_block
	| match_cmd ';'
	;

match_cmd:
	|
	TWRITE TSTRING TSTRING {
	/* write <what (strpartial)> <where (symbol)> */
		strpartial *writep;
		struct node* n;

		CHECK_PARTIAL(writep, $2);
		CHECK_SYMBOL($3);

		ADD_NODE(n, NODE_WRITE);
		n->context.nwrite.string_fmt = writep;
		n->context.nwrite.dest_sym = dynstr_new($3.v);

	}
	|
	TBREAK {
		struct node* n;
		ADD_NODE(n, NODE_BREAK);
	}
	|
	TSTRING '=' TSTRING {
	/* <var (symbol)> = <value (strpartial)> */
		strpartial* val;
		struct node* n;
		CHECK_SYMBOL($1);
		CHECK_PARTIAL(val, $3);

		dynstr* vval = dynstr_new($1.v);
		if (NULL == ht_find(dlogenv->vars_table, (intptr_t)vval)) {
			yyerror("Unitialised var (%s)", $1.v);\
			dynstr_free(vval);
			YYABORT;\
		}

		ADD_NODE(n, NODE_ASSIGN);
		n->context.assign.var = vval;
		n->context.assign.pattern = val;
		//ht_upsert(dlogenv->vars_table, (uintptr_t)n->context.assign.var, NULL);
	}
	;

match_block:
	TMATCH TSTRING TSTRING block {
	/* match <regex (partial)>   <target (partial)> */
		LOG_INFO("Action: MATCH <%s> <%s>", $2.v, $3.v);

		strpartial *re, *tgt;
		struct node* n;

		CHECK_PARTIAL(re, $2);
		CHECK_PARTIAL(tgt, $3);
		ADD_NODE_WPARENT(n, NODE_MATCH);
		n->context.match.re_pattern = re;
		n->context.match.target = tgt;
		n->context.match.source = NULL;
	}
	|
	TMATCH TSTRING TSTRING TFROM TSTRING block {
	/* match <re> <target> from <source> */
		LOG_INFO("Action: MATCH <%s> <%s> FROM <%s>", $2.v, $3.v, $5.v);

		strpartial *re, *tgt;
		struct node* n;

		CHECK_PARTIAL(re, $2);
		CHECK_PARTIAL(tgt, $3);
		CHECK_SYMBOL($5);
		ADD_NODE_WPARENT(n, NODE_MATCH);
		n->context.match.re_pattern = re;
		n->context.match.target = tgt;
		n->context.match.source = dynstr_new($5.v);
	}
	|
	TMATCHALL TFROM TSTRING block {
	/* matchall from <source (symbol)> */
		LOG_INFO("Action: MATCHALL FROM <%s>", $3.v);
		struct node* n;

		CHECK_SYMBOL($3);
		ADD_NODE_WPARENT(n, NODE_MATCHALL);
		n->context.matchall.source = dynstr_new($3.v);

	}
	;

else_block:
	TELSE block {
		struct node* n;

		ADD_NODE_WPARENT(n, NODE_MELSE);
	}
	;

block:
	'{' {
		curnode = calloc(1, sizeof(struct node));
		if (curblock) {
			curnode->parent = curblock;
		}
		curblock = curnode;
		if (!rootnode)
			rootnode = curnode;
	} rule_statements '}'
	;

%%

static struct keyword
{
	const char *kword;
	int token;
} keywords[] = {
	/* parsing */
	{ "include", TINCLUDE},
	{ "var", TVAR},
	{ "listen", TLISTEN},
	{ "pidfile", TPIDFILE},
	{ "logfile", TLOGFILE},
	{ "datetimeformat", TDATETIMEFORMAT},
	{ "timestampresolution", TTIMESTAMPRES},
	{ "source", TSOURCE},
	{ "destination", TDESTINATION},
	{ "tcp", TTCP},
	{ "file", TFILE},
	{ "fifo", TFIFO},
	{ "maxsize", TMAXSIZE},
	{ "rotlog", TROTLOG},
	{ "as", TAS},
	/* runtime */
	{ "rule", TRULE},
	{ "match", TMATCH},
	{ "matchall", TMATCHALL},
	{ "from", TFROM},
	{ "else", TELSE},
	{ "write", TWRITE},
	{ "break", TBREAK},
};

int
yylex(void)
{
	char buf[4096], *ebuf, *p, *str;
	int c, quotes = 0, escape = 0, nonkw = 0, semicol = 0;
	size_t i;

	p = buf;
	ebuf = buf + sizeof(buf);

repeat:
	/* skip whitespace first - TODO unix simple ASCII only */
	for (c = getc(yyfp->fp); c==' ' || c=='\t' || c=='\n'; c = getc(yyfp->fp)) {
		if (c == '\n')
			yyfp->lineno++;
	}

	switch (c) {
		case ';':
		case '{':
		case '}':
		case '=':
			return c;
		case '#':
			/* skip comments; NUL is allowed; no continuation */
			while ((c = getc(yyfp->fp)) != '\n') {
				if (c == EOF)
					goto eof;
			}
			yyfp->lineno++;
			goto repeat;
		case EOF:
			goto eof;
	}

	/* parsing next word */
	for (;; c = getc(yyfp->fp)) {
		switch (c) {
		case '\0':
			yyerror("invalid character NUL, line %d",
			    yyfp->lineno);
			escape = 0;
			continue;
		case '\\':
			escape = !escape;
			if (escape)
				continue;
			break;
		case '\n':
			if (!semicol) {
				yyerror("Line terminator missing");
				return T__INVALID__;
			}
			if (quotes)
				yyerror("unterminated quotes in line %d",
					yyfp->lineno);
			if (escape) {
				nonkw = 1;
				escape = 0;
				yyfp->lineno++;
				continue;
			}
			goto eow;
		case EOF:
			if (escape)
				yyerror("unterminated escape, line %d",
				    yyfp->lineno);
			if (quotes)
				yyerror("unterminated quotes in line %d",
				    yyfp->lineno);
			goto eow;
		case ';':
			if (!escape && !quotes) {
				semicol = 1;
				goto eow;
			}
			/* FALLTHROUGH */
		case '{':
		case '}':
		case '#':
		case ' ':
		case '\t':
		case '=':
			if (!escape && !quotes)
				goto eow;
			break;
		case '"':
			if (!escape) {
				quotes = !quotes;
				if (quotes) {
					nonkw = 1;
				}
				continue;
			}
		}
		*p++ = c;
		if (p == ebuf) {
			yyerror("too long line");
			p = buf;
		}
		escape = 0;
		semicol = 0;
	}

eow:
	*p = 0;
	if (c != EOF)
		ungetc(c, yyfp->fp);
	if (p == buf) {
		if (c == EOF)
			goto eof;
	}
	if (!nonkw) {
		/* this def wasn't quoted */
		yylval.is_quoted = 0;
		yylval.is_symbol = tstring_is_symbol(buf);
		for (i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
			if (strcmp(buf, keywords[i].kword) == 0) {
				int tk = keywords[i].token;
				return tk;
			}
		}
	} else {
		/* quoted-str can never be a symbol*/
		yylval.is_quoted = 1;
		yylval.is_symbol = 0;
	}
	if ((str = strdup(buf)) == NULL)
		err(1, "%s", __func__);

	yylval.v = str;
	return TSTRING;

eof:
	if (ferror(yyfp->fp))
		yyerror("input error reading config");
	cfgfile_pop();
	if (yyfp)
		goto repeat;
	return 0;
}

void
yyerror(const char *fmt, ...)
{
	char* msg;
	va_list va;

	va_start(va, fmt);
	if(-1 != vasprintf(&msg, fmt, va)) {
		va_end(va);
		LOG_ERROR(" dlog - config parser - %s - at line %d (%s)", msg, yyfp->lineno+1, yyfp->filename);
	}
}

int
parse_config(void)
{
	int res;
	//yydebug=1;

	if ((yyfp = cfgfile_include(dlogenv->config.configfile))) {
		if (0 == (res = yyparse())) {
			dlogenv->root_node = rootnode;
		}
		return res;
	} else {
		LOG_ERROR("Failed to open config file %s", dlogenv->config.configfile);
		return (-1);
	}
}


static void
node_add_child(struct node* parent, struct node* n)
{
	if (parent) {
		struct node* nn = parent->child;
		if (!nn) {
			n->sibling = NULL;
			parent->child = n;
		} else {
			while (nn->sibling) {
				nn = nn->sibling;
			}
			nn->sibling = n;
		}
	}
}

static int
tstring_is_symbol(const char* s)
{
	if (s && (isalpha(*s) || *s == '_')) {
		while(*++s) {
			if (!isalnum(*s) && *s != '_') {
				return 0;
			}
		}
		return 1;
	}
	return 0;
}

static void
add_origin(struct dorigin* or)
{
	struct dorigin* n = dlogenv->origins;
	dlogenv->origins = or;
	or->next = n;
}

/* mini-resolve for non-runtime strings. Only accepts
 * string interpolation with other static variants and env. variables */
static dynstr
*strpartial_resolve_ex(const strpartial* part)
{
	strpartial* s;
	dynstr* str = dynstr_new(NULL);
	while(part) {
		switch (part->type) {
			case STR_VERBATIM:
				str = dynstr_cat(str, part->value.rawstr);
				break;

			case STR_VAR:
				if (NULL == (s = ht_find(dlogenv->vars_table, (uintptr_t)part->value.rawstr)))
					goto fail;
				else {
					str = dynstr_cat(str, strpartial_resolve_ex(s));
				}
				break;

			case STR_ENV:
				str = dynstr_ccat(str,
						getenv(dynstr_ptr(part->value.rawstr)));
				break;

			default:
				yyerror("Unsupported complex substring %s", part->value.rawstr);
				return NULL;
		}
		part = part->next;
	}
	return str;

fail:
	dynstr_free(str);
	return NULL;
}

static bool strpartial_isstatic(const strpartial* part, bool allow_vars)
{
	while(part) {
		switch(part->type) {
			case STR_VAR: {
				if (!allow_vars)
					return false;

				strpartial* s;
				if ((s = ht_find(dlogenv->vars_table,
									(uintptr_t)part->value.rawstr)))
					return false;
				else {
					return strpartial_isstatic(s, false);
				}
			}
			break;

			case STR_VERBATIM:
			case STR_ENV:
			return true;
		}
		part = part->next;
	}
	return false;
}
