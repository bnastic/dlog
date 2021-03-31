#ifndef _DLOG_DEF_H__
#define _DLOG_DEF_H__
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <stddef.h>

/****************************************
	OS
 ****************************************/
#if defined(__linux__)
#	define DLOG_HAVE_LINUX
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonflyBSD__)
#	define DLOG_HAVE_BSD
#elif defined(__APPLE__) && defined(__MACH__)
#	define DLOG_HAVE_OSX
#else
#	error "Unsported Operating System"
#endif

/*************************************************
 * misc
 *************************************************/
#define dlog_defstr(n)				#n
#define dlog_value(n)				dlog_defstr(n)

/* typeof is OK if you don't insist on -pedantic */
#define dlog_max(a,b) \
	({ __typeof__ (a) _a = (a); \
	 __typeof__ (b) _b = (b); \
	 _a > _b ? _a : _b; })

#define dlog_min(a,b) \
	({ __typeof__ (a) _a = (a); \
	 __typeof__ (b) _b = (b); \
	 _a < _b ? _a : _b; })

#define TAGPTR(ptr, tag) \
	(uintptr_t)((uintptr_t)(ptr) | ((uintptr_t)(tag) << 48))

#define UNTAGPTR(tagptr, tag, intoptr) \
	tag = (uintptr_t)(tagptr) >> 48ULL;\
	intoptr = (__typeof__(intoptr))((tagptr) & (uintptr_t)0x0000ffffffffffffULL);


#define ELEM_IN_SET(retval, compare, ...) \
{int arr[] = { __VA_ARGS__ };\ var = 0;\
for (size_t i=0; i<sizeof(arr)/sizeof(int); i++) {\
	if(arr[i] == compare) {\
		retval = 1; break;\
	}\
}}


#if defined(DLOG_HAVE_OSX)
#	define dlog_aligned_alloc(algn, sz) malloc((sz))
#else
#	define dlog_aligned_alloc(algn, sz) aligned_alloc((algn), (sz));
#endif

__attribute__ ((const))
inline uint32_t pow2(uint32_t x)
{
	return 1 << (32 - __builtin_clz (x - 1));
}

/* borrowed from linux kernel */
#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)

/* FreeBSD doesn't know PIPE_BUF */
#ifndef PIPE_BUF
#	define PIPE_BUF _POSIX_PIPE_BUF
#endif

/* Linux doesn't have _SAFE */
#ifdef DLOG_HAVE_LINUX
#	define TAILQ_FOREACH_SAFE(a,b,c,d) (void)d;TAILQ_FOREACH(a,b,c)
#endif

/* TODO replace this with actual limits */
#if !defined (PATH_MAX)
#	define PATH_MAX 1024
#endif

#define DLOG_PATH_MAX PATH_MAX
#define DLOG_MAX_FILES 1024
#define UNUSED(...) (void)(__VA_ARGS__)

#if defined(__FreeBSD__)
#	define _WANT_UCRED
#endif

#if defined(__NetBSD__)
#	define STRUCT_UCRED struct uucred
#else
#	define STRUCT_UCRED struct ucred
#endif

/*************************************************
 * config
 *************************************************/
#define DLOG_ALLOC_DEFAULT_ALIGNMENT	16
#define DLOG_INOTIFY_SYMBOL				"#INOTIFY_SYM"
#define DLOG_LISTEN_SOCKET_SYM			"#LISTEN_SKT"
#define DLOG_CLIENT_SOCKET_SYM			"TCP_SOCKET"
#define DLOG_UNIX_SKT_NAME				"/tmp/.dlogxfer_"
#define DLOG_SHMEM_NAME					"/tmp/dlog.shmem"
#define DLOG_EVENTLOOP_TIMEOUT			200
#define	DLOG_READ_BUF_SZ				4096
#define DLOG_READ_MAX_CHUNK				(4*1024)
#define DLOG_WRITE_HIGH_WM				32
#define DLOG_OPT_PIDFILE				"/var/tmp/dlog.pid"
#define DLOG_OPT_LOGFILE				"dlog.logfile"
#define DLOG_DEFAULT_DATETIME_FORMAT	"%FT%T"
#define DLOG_DEFAULT_FRACTSEC_DIV		(1)
#define DLOG_ROTLOG_TIMESTAMPEXT		"%y%m%d.%H%M%S"

#endif

