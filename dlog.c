#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <execinfo.h>

#include "def.h"
#include "log.h"
#include "arena.h"
#include "env.h"
#include "coredesc.h"
#include "proc.h"
#include "fdxfer.h"
#include "evt.h"
#include "hashtable.h"
#include "lr.h"
#include "lw.h"
#include "node.h"
#include "rotlog.h"

static int get_opts(int argc, char** argv);
static void env_init(void);
static void env_post_init(void);
static void usage(void);
static int main_loop(void);
static int idle_loop(void);
static void descriptor_read(descriptor* d, size_t size_hint);
static void descriptor_write(dynstr* sym, dynstr* line);
static void descriptor_write_direct(descriptor*, dynstr* line);
static void desc_pending_add(descriptor* d);
static void desc_pending_remove(descriptor* d);
static void desc_pending_drain_all(int desc_bitmask);
static void desc_active_writes_drain(bool);
static void origin_destroy();
static int _read_pending(intptr_t fd, void* desc, void* udata);
static void process_signals(void);
static void dlog_sig_shutdown(void);
static void dlog_sig_restart(void);
static void dlog_sig_rotlog(void);
extern int parse_config(void);

/* globals */
arena* _dynstr_arena;
dlog_env* dlogenv;

#ifdef DLOG_HAVE_LINUX
extern int inotify_fd;
static descriptor* inotify_d;
#endif

static struct dorigin listen_skt_or =
{
	.type = D_SOCKET_LISTEN,
	.symbol = DLOG_LISTEN_SOCKET_SYM,
	.socket.host = "0.0.0.0",
	.socket.port = NULL,
	.inherited.fd = -1,
	.next = NULL
};

static descriptor* listen_skt = NULL;
static int _has_pidfile = 0;

/******* dynstr arena *****************/
static int _dynstr_buckets[7][2] =
{
	{32,	512},
	{128,	512},
	{256,	256},
	{1024,	256},
	{2048,	128},
	{8192,	32},
	{32768, 16},
};

dynstr* alloc_dynstr_alloc(int sz, int* real_cap)
{
	return arena_alloc(_dynstr_arena, sz, real_cap);
}

void alloc_dynstr_free(dynstr* s)
{
	arena_free(_dynstr_arena, s);
}

dynstr* alloc_dynstr_realloc(dynstr* s, int newsize, int* new_cap)
{
	return arena_realloc(_dynstr_arena, s, newsize, new_cap);
}

/*******************************/

int main(int argc, char** argv, char** envp)
{
	log_create_stderr();

	env_init();

	if (0 != get_opts(argc, argv)) {
		usage();
		return 1;
	}

	env_post_init();
	proc_savecmd(argc, argv, envp);

	if (0 != parse_config()) {
		LOG_ERROR("Failed to parse config file. Exiting");
		exit(1);
	}

	if (dlogenv->config.cmdopt.testconfig) {
		LOG_INFO("Configuration file parsed OK");
		LOG_INFO("Config node tree:");
		print_node_tree(dlogenv->root_node);
		return 0;
	}

	LOG_INFO("Starting Dlog...");

	sig_blockall(dlogenv->config.cmdopt.nodaemon);

	if (!dlogenv->config.cmdopt.nodaemon) {
		LOG_INFO("Dlog daemonizing... Log file will be: %s", dlogenv->config.logfile);
		proc_daemonize();

		log_create_file(dlogenv->config.logfile);
		if (0 == pidfile_create()) {
			LOG_INFO("pidfile is: %s", dlogenv->config.pidfile);
			_has_pidfile = 1;
		}
	} else {
		LOG_INFO("Dlog starting in foreground mode");
	}

	if (dlogenv->config.cmdopt.newbinary) {
		LOG_INFO("Dlog starting from parent process with parent pid %d", getppid());
		xfer_msg** msgs = NULL;
		int nmsg;

		if(0 != fdxfer_open_recv(&msgs, &nmsg)) {
			LOG_ERROR("FD transfer failed, will continue without it...");
		} else {
			for (int i=0; i<nmsg; ++i) {
				xfer_msg* msg = msgs[i];
				char* sym = msg->buf;
				char* xbuf = sym + strlen(sym) + 1;
				/* if socket came through it needs to be recreated. Sockets
				   don't come from config, so can't match them
				*/
				struct dorigin* dor = dlogenv->origins;
				if (!strcmp(sym, DLOG_CLIENT_SOCKET_SYM)) {
					struct dorigin* or = calloc(1, sizeof(*or));
					or->type = D_SOCKETR;
					or->symbol = strdup(sym);
					or->inherited.buffer = strdup(xbuf);
					or->inherited.buf_idx = msg->buf_idx;
					or->inherited.fd = msg->in_fd;
					dlogenv->origins = or;
					or->next = dor;
					continue;
				}
				/* go through dorigin entries, find 'sym' match (check type) */
				int foundfd = 0;
				while (dor) {
					if (!strcmp(sym, dor->symbol) && dor->type == msg->desc_type) {
						dor->inherited.buffer = strdup(xbuf);
						dor->inherited.buf_idx = msg->buf_idx;
						dor->inherited.fd = msg->in_fd;
						foundfd = 1;
						break;
					}

					dor = dor->next;
				}
				if (!foundfd) {
					LOG_ERROR("Xfer of fd symbol %s, not found", sym);
				}
			}
		}
		fdxfer_close();
	}

	evt_sys_create();

#if defined(DLOG_HAVE_LINUX)
	inotify_d = open_descriptor(&inotify_origin, NULL, NULL, DOPEN_NOFLAGS);
	if (inotify_d && inotify_d->state != DSTATE_DEAD)
		inotify_fd = inotify_d->fd;

#endif

	if (dlogenv->config.listenskt_port) {
		if ((listen_skt = open_descriptor(&listen_skt_or, NULL, NULL, DOPEN_NOFLAGS))) {
			LOG_INFO("Listening socket created on port %s", dlogenv->config.listenskt_port);
		} else {
			LOG_ERROR("Listening socket failed, DLog starting without socket support");
		}
	} else {
		LOG_INFO("DLog starting without socket support");
	}

	struct dorigin *origin = dlogenv->origins;

	while(origin) {
		descriptor* d = NULL;

		/* complex types */
		switch (origin->type) {
		case D_ROTLOG:
			d = open_rotlog(origin);
			break;

		default:
			d = open_descriptor(origin, NULL, NULL, DOPEN_NOFLAGS);
			break;
		}

		if (!d) {
			LOG_ERROR("Failed to create descriptor for '%s'", origin->symbol);
			continue;
		}

		origin = origin->next;
	}

	LOG_INFO("Finished setting up descriptors");

	/* enable signals */
	sig_init(dlogenv->config.cmdopt.nodaemon);

	/*			main loop							 */
	/* ============================================= */

	main_loop();

	return 0;
}

static int
get_opts(int argc, char** argv)
{
	int config_missing = 1;

	for(int i=1; i<argc; i++) {
		char* opt = argv[i];
		if (*opt++ != '-') {
			LOG_ERROR("Unknown option: %s", opt);
			return 1;
		}

		switch (*opt) {
			case 'v':
			case '?':
				return -1;
				continue;
			case 't':
				dlogenv->config.cmdopt.testconfig = true;
				continue;
			case 'c':
				if (argv[++i]) {
					dlogenv->config.cmdopt.configfile = argv[i];
					config_missing = 0;
				} else {
					LOG_ERROR("-c paramater missing");
					return 1;
				}
				continue;

			case 'l':
				if (argv[++i]) {
					int port = strtol(argv[i], NULL, 10);
					if ((port == 0 && errno == EINVAL) || port > 65535) {
						LOG_ERROR("Invalid port number %s", argv[i]);
						return -1;
					} else {
						dlogenv->config.cmdopt.listen_port = argv[i];
					}
				}
				continue;

			case 'x':
				dlogenv->config.cmdopt.newbinary = true;
				continue;

			case 'n':
				dlogenv->config.cmdopt.nodaemon = true;
				continue;

			default:
				LOG_ERROR("Unknown option %s", argv[i]);
				return 1;
		}
	}

	return config_missing;
}

static void
usage()
{
	static const char* use =
	"dlog usage:\n\n"
	"dlog [options]\nOptions:\n"
	"-c <config file>, specify config file to use. Required.\n"
	"-t test configuration file and exit.\n"
	"-l Socket server listen port.\n"
	"-n Start in foreground mode.\n"
	"-v, -? show this text\n"
	;
	printf("%s\n", use);
}

static void
env_init(void)
{
	dlogenv = calloc(1, sizeof(*dlogenv));

	dlogenv->config.datetime_format = strdup(DLOG_DEFAULT_DATETIME_FORMAT);
	dlogenv->config.pidfile = strdup(DLOG_OPT_PIDFILE);
	dlogenv->config.fractsec_divider = DLOG_DEFAULT_FRACTSEC_DIV;
	dlogenv->config.logfile = strdup(DLOG_OPT_LOGFILE);

	dlogenv->symbol_table = ht_create(HT_DYNSTR, 53, ht_value_deleter_null);
	dlogenv->pending_reads_table = ht_create(HT_INT, 17, ht_value_deleter_null);
	dlogenv->vars_table = ht_create(HT_DYNSTR, 53, NULL);

	_dynstr_arena = arena_create(_dynstr_buckets, 7, NULL, true);

	TAILQ_INIT(&dlogenv->desc_active_list);
}

static void
env_post_init(void)
{
	dlogenv->config.configfile = dlogenv->config.cmdopt.configfile;

	if (dlogenv->config.cmdopt.listen_port) {
		dlogenv->config.listenskt_port = strdup(dlogenv->config.cmdopt.listen_port);
	}
}

static int
main_loop(void)
{
	EVT_CONTEXT evts[DLOG_MAX_FILES];

	descriptor* read_files[DLOG_MAX_FILES];
	int nb_files = 0;

	while(1) {

		process_signals();

		int nev = EVT_LOOP(evts, DLOG_MAX_FILES, DLOG_EVENTLOOP_TIMEOUT);

		if (nev == -1) {
			if (errno == EINTR) {
				LOG_DEBUG("event loop interrupted, signal likely, continuing...");
				continue;
			} else {
				LOG_SYS_ERROR("event loop interrupted, aborting");
				return -1;
			}
		} else if (nev == 0) {
			idle_loop();
		} else {

			nb_files = 0;

			for (int i=0; i<nev; i++) {

				EVT_CONTEXT* evt = &evts[i];

				if (EVT_IS_ERROR(evt)) {
					LOG_ERROR("Error in event structure");
					continue;
				}

				if (EVT_IS_VNODE(evt)) {
					evt_process_vnode(evt, read_files, &nb_files);
					continue;
				}

				descriptor* d = EVT_GET_DESCRIPTOR(evt);

				if (EVT_IS_READ(evt)) {

					/* XXX Linux only comes here for sockets */
					if (EVT_IS_EOF(evt)) {

						switch(D_CORE_TYPE(d->type)) {

							case D_FIFOR: {
								evt_clear_state(evt, d->fd);
								break;
							}
							case D_SOCKETR: {
								LOG_DEBUG("Socket-read EOF, drain state");
								d->state = DSTATE_DRAIN;
								desc_pending_add(d);
								evt_reg_remove(d);
								break;
							}
						}
					}
					if (d->type & (D_FILER | D_FIFOR)) {
						/* only collect files ready for reading (already done for Linux,
							which will never enter this code path) */
						read_files[nb_files++] = d;
					} else {
						int size_hint = EVT_GET_READ_SIZE_HINT(evt);
						descriptor_read(d, size_hint);
					}

				} else if (EVT_IS_WRITE(evt)) {
					if (d->type == D_SOCKETW) {
						/* FreeBSD and OSX behave very differently with nonblocking
						   connect(). For local sockets, FreeBSD goes to ECONNREFUSED
						   straight away, while OSX goes to EINPROGRESS. But when
						   coming here, OSX will report a write event with EOF flag
						   when not connected. No other errors (getsockopt) reported
						   on OSX.
						*/
						if (d->state == DSTATE_PENDING) {
							if (EVT_IS_EOF(evt)) {
								int serr;
								socklen_t errlen = sizeof(serr);

								/* error in connect(), check which one again */
								if (0 == getsockopt(d->fd, SOL_SOCKET,
													SO_ERROR, &serr, &errlen)) {
									LOG_DEBUG("EVT_WRITE: socket error for %s - %s", d->origin->symbol, strerror(serr));
									if (serr == ECONNREFUSED) {
										LOG_DEBUG("Socket connection refused, restarting...");
										reset_descriptor(d);
										d->state = DSTATE_PENDING;
									}
								}
							} else {
								/* socket connected! */
								LOG_DEBUG("Socket %s connected.", d->origin->symbol);
								d->state = DSTATE_ACTIVE;
							}
						}
					}
				}
			}
			/* event queue emptied, empty file queue */
			for (int i=0; i<nb_files; i++) {
				descriptor* filed = read_files[i];
				/* trick = this loop works off evt system. DRAN_ROTATE files
				   will only come here ONCE so we can add them to drain list here
				*/
				if (filed->state == DSTATE_DRAIN_ROTATE)
					desc_pending_add(filed);

				descriptor_read(filed, 0);
			}
		}
	}

	return 0;
}

static int
idle_loop(void)
{
	desc_pending_drain_all(D_CORE_READ_TYPES);

	return 0;
}

static void
descriptor_read(descriptor* d, size_t size_hint)
{
	if (d->vfn.pre_read && 0 != d->vfn.pre_read(d, size_hint))
		return;

	int r = 1, total_read=0;
	int rs = size_hint == 0 ? DLOG_READ_BUF_SZ : size_hint;

	/* make sure we don't read the whole file in one go */
	int max_chunk = DYNSTR_USABLE_SIZE(DLOG_READ_MAX_CHUNK);

	if (d->fd > 0) {
		while (r > 0) {

			char* buffer = reader_get_buffer(d->reader, &rs);

			r = read(d->fd, buffer, rs);
			total_read+=r;

			max_chunk -= r;

			if (r>0) {
				reader_buffer_fill(d->reader, r);

				if (max_chunk <= 0) {
					/* max read size exceeded, more data available */
					desc_pending_add(d);
					break;
				}
			} else if (r == -1) {
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					LOG_ERROR("Error reading descriptor %s", d->origin->symbol);
					break;
				}
			}
		}
	}

	dynstr* line;
	while ((line = reader_get_next_line(d->reader))) {

		node_eval_root(dlogenv->root_node, line, d->symbol, descriptor_write);

		dynstr_free(line);
	}


	if (r == 0) {
		// EOF reached - remove from pending
		desc_pending_remove(d);

		if (d->state == DSTATE_DRAIN) {
			close_descriptor(d);
		} else {
			if (d->type ==  D_FILER && d->state == DSTATE_DRAIN_ROTATE) {
				/* reopen the file */
				reset_descriptor(d);
				open_descriptor(d->origin, d,
							NULL, DOPEN_SEEKSTART|DOPEN_KEEP_BUFFERS);
			}
		}
	}
}

static void
descriptor_write(dynstr* symbol, dynstr* line)
{
	descriptor* d = (descriptor *)ht_find(dlogenv->symbol_table, (uintptr_t)symbol);

	if (!d) {
		LOG_ERROR("Symbol %s not found, write operation abandoned", dynstr_ptr(symbol));
		return;
	}

	descriptor_write_direct(d, line);
}

static void
descriptor_write_direct(descriptor* d, dynstr* line)
{
	ssize_t bytes_written = 0;
	struct writequeue* wq = (struct writequeue* )d->wqueue;

	/* Allow empty lines, for draining any write buffers */
	if (line) {
		/* check for new line */
		if (!dynstr_isnewline(line))
			line = dynstr_ccat(line, "\n");

		wq_add_line(wq, line);
	}

	if (d->state & ~(DSTATE_PENDING|DSTATE_ACTIVE)) {
		LOG_WARNING("Trying to write to inactive descriptor. Ignored");
		return;
	}

	int err;
	bytes_written = wq_write(wq, d->fd, &err);

	if (d->vfn.post_line_write && (err != 0 || bytes_written >= 0)) {
		d->vfn.post_line_write(d, bytes_written, err);
	}

	/* TODO want to be able to return how many bytes have been written, and if there is any remaining
	        (would be useful for drain states
	*/
}

static void
desc_pending_add(descriptor* d)
{
	ht_upsert(dlogenv->pending_reads_table, d->fd, d);
}

static void
desc_pending_remove(descriptor* d)
{
	ht_remove(dlogenv->pending_reads_table, d->fd);
}

static void
desc_pending_drain_all(int desc_bitmask)
{
	ht_visit(dlogenv->pending_reads_table, _read_pending, &desc_bitmask);
}


static int
_read_pending(intptr_t fd, void* desc, void* udata)
{
	/* udata is int* bitmask */
	int bitmask = *(int*)udata;
	descriptor* d = desc;
	if ((d->type & bitmask ) && (d->state & (DSTATE_ACTIVE | DSTATE_DRAIN | DSTATE_DRAIN_ROTATE))) {
		descriptor_read(d, 0);
	}

	return 0;
}

static void
process_signals(void)
{
	if (unlikely(dlogenv->sig_delivered)) {
		struct sig_s* sig = &signals[0];
		while (sig->handler) {
			if (sig->flag) {

				switch(sig->signo) {
				case dlog_signal(DLOG_SIG_RESTART): {
					dlog_sig_restart();
				}
				break;

				case dlog_signal(DLOG_SIG_SHUTDOWN): {
					dlog_sig_shutdown();
				}
				break;

				case dlog_signal(DLOG_SIG_ROTLOG_ROT): {
					dlog_sig_rotlog();
				}
				break;
				}

				sig->flag = false;
			}

			sig++;
		}

		dlogenv->sig_delivered = 0;
	}
}

static void
dlog_sig_rotlog(void)
{
	/* force-rotate all rotlogs */
	descriptor* d;
	TAILQ_FOREACH(d, &dlogenv->desc_active_list, _lnk) {
		if (d->type == D_ROTLOG)
			rotlog_rotate(d);
	}
}

static void
dlog_sig_restart(void)
{
	close_descriptor(listen_skt);
	desc_active_writes_drain(true);
	evt_sys_destroy();
	proc_restart_with_newbinary();

	descriptor* d;

	if (TAILQ_FIRST(&dlogenv->desc_active_list)) {
		if (fdxfer_open_send() == 0) {
			TAILQ_FOREACH(d, &dlogenv->desc_active_list, _lnk) {
				if (d->type & D_CORE_READ_TYPES) {
					fdxfer_send(d);
				}
			}
		} else {
			LOG_ERROR("Failed to open transfer channel");
		}
	}

	dlog_sig_shutdown();
}

static void
dlog_sig_shutdown(void)
{
	sig_blockall(false);

	close_descriptor(listen_skt);

#ifdef DLOG_HAVE_LINUX
	close_descriptor(inotify_d);
#endif

	desc_active_writes_drain(true);

	evt_sys_destroy();
	ht_destroy(dlogenv->symbol_table);
	ht_destroy(dlogenv->pending_reads_table);
	ht_destroy(dlogenv->vars_table);

	origin_destroy();

	free(dlogenv);

	if (_has_pidfile) {
		pidfile_delete();
	}

	fdxfer_close();

	arena_destroy(_dynstr_arena, NULL);

	LOG_INFO("Shutdown finished, bye bye.");
	exit(0);
}

static void
desc_active_writes_drain(bool also_close_fds)
{
	descriptor* d, *prev = NULL;

	TAILQ_FOREACH(d, &dlogenv->desc_active_list, _lnk) {
		if (D_IS_WRITE_SIDE(d->type)) {
			descriptor_write_direct(d, NULL);
		}
		if (prev && also_close_fds)
			close_descriptor(d);
		prev = d;
	}
}

static void
origin_destroy(void)
{
	/* FIXME DEBUG DISABLED */
// 	struct dorigin *origin = dlogenv->origins;
// 
// 	while(origin) {
// 		struct dorigin* n = origin->next;
// 		free_dorigin(origin);
// 		origin = n;
// 	}
}

