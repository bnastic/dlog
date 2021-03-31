#ifndef DLOG_ENV_H__
#define DLOG_ENV_H__
#include <sys/queue.h>
#include <unistd.h>
#include <signal.h>

#include "def.h"
#include "arena.h"

struct descriptor;
struct dorigin;
struct hashtable;
struct node;

struct dlog_config
{
	char*	datetime_format;
	int		fractsec_divider;
	char*	pidfile;
	char*	logfile;
	char*	configfile;
	char*	listenskt_port;

	struct {
		bool showhelp;
		bool testconfig;
		bool newbinary;
		bool nodaemon;
		char*  listen_port;
		char* configfile;
	} cmdopt;
};

typedef struct dlog_env
{
	struct dlog_config config;

	pid_t		pid;

	sig_atomic_t sig_delivered;

	TAILQ_HEAD(, descriptor) desc_active_list;

	/* symbol -> descriptor */
	struct hashtable*	symbol_table;

	/* fd -> descriptor */
	struct hashtable* pending_reads_table;

	/* inherited server sockets */
	int* inherited_skt_fds;
	int  nb_inherited_skts;

	/* descriptors' origin */
	struct dorigin* origins;

	/* hashtable of VARs ( dynstr(symbol) -> str_partial(value) )*/
	struct hashtable* vars_table;

	struct node* root_node;

	struct {
		int argc;
		int envc;
		char** argv;
		char** envp;
	} env_param;

} dlog_env;

extern dlog_env* dlogenv;

#endif
