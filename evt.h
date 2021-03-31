#ifndef DLOG_EVT_H__
#define DLOG_EVT_H__
#include "def.h"

struct descriptor;
typedef int EVT_SYS;

#if defined(DLOG_HAVE_LINUX)
/****************************/
#	define DLOG_HAVE_EPOLL 1
#	define DLOG_HAVE_INOTIFY 1

#	include <sys/epoll.h>
#	include <sys/inotify.h>
	typedef struct epoll_event EVT_CONTEXT;

#	define EPOLL_DEFAULT_READ_SIZE 1500
#	define EVT_LOOP(evts, num_evts, timeout_msec) \
		epoll_wait(evt_sys(), evts, num_evts, timeout_msec)

#	define EVT_IS_READ(evt_ctx) evt_ctx->events & EPOLLIN
#	define EVT_IS_EOF(evt_ctx) evt_ctx->events & (EPOLLHUP|EPOLLRDHUP)
#	define EVT_IS_WRITE(evt_ctx) evt_ctx->events & EPOLLOUT
#	define EVT_IS_ERROR(evt_ctx) evt_ctx->events & EPOLLERR
#	define EVT_IS_VNODE(evt_ctx) (((descriptor*)evt_ctx->data.ptr)->origin->type == D_INOTIFY)
#	define EVT_GET_DESCRIPTOR(evt_ctx) (descriptor*)evt_ctx->data.ptr
#	define EVT_GET_READ_SIZE_HINT(evt_ctx) (int)(0)

#elif defined(DLOG_HAVE_BSD) || defined(DLOG_HAVE_OSX)
/*****************************************************/
#	define DLOG_HAVE_KQUEUE

#	include <sys/event.h>
	typedef struct kevent EVT_CONTEXT;

#	define EVT_LOOP(evts, num_evts, timeout_msec)\
		kevent(evt_sys(), NULL, 0, evts, num_evts, &(struct timespec){0, timeout_msec*1000000LL})

#	define EVT_IS_READ(evt_ctx) evt_ctx->filter == EVFILT_READ
#	define EVT_IS_EOF(evt_ctx) evt_ctx->flags & EV_EOF
#	define EVT_IS_WRITE(evt_ctx) evt_ctx->filter == EVFILT_WRITE
#	define EVT_IS_ERROR(evt_ctx) evt_ctx->flags == EV_ERROR
#	define EVT_IS_VNODE(evt_ctx) evt_ctx->filter == EVFILT_VNODE
#	define EVT_GET_DESCRIPTOR(evt_ctx) (descriptor *)evt_ctx->udata
#	define EVT_GET_READ_SIZE_HINT(evt_ctx) (int)(evt_ctx->data)
#endif


void	evt_sys_create();
void	evt_sys_destroy(void);
EVT_SYS evt_sys(void);
int		evt_reg_read(struct descriptor* d);
int		evt_reg_write(struct descriptor* d);
int		evt_reg_remove(struct descriptor* d);
int		evt_clear_state(EVT_CONTEXT* e, int fd);
void	evt_process_vnode(EVT_CONTEXT* evt, struct descriptor** files, int* nb_files);
int		evt_watch_vnode(struct descriptor* d);
void	evt_unwatch_vnode(int dirfd, struct descriptor* d);
void	evt_reg_vnode_del(struct descriptor* );

#endif

