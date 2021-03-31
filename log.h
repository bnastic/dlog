#ifndef _DLOGLOG_H__
#define _DLOGLOG_H__
#include "def.h"
#include <errno.h>

void log_create_syslog();
void log_create_stderr();
void log_create_file(const char* filename);
void log_create_rotlog();
void log_close();

void log_dbgtrace(const char* file, int line, const char* msg, ...);
void log_info(const char* msg, ...);
void log_warn(const char* msg, ...);
void log_error(const char* msg, ...);
void log_syserr(int syserror, const char* msg, ...);

#define DLOG_NEWLINE "\n"

#if !defined(DLOG_HAVE_DEBUG)
# define LOG_DEBUG(msg, ...)
#else
# define LOG_DEBUG(msg, ...) log_dbgtrace(__FILE__, __LINE__, msg DLOG_NEWLINE, ##__VA_ARGS__)
#endif
# define LOG_TRACE(msg, ...) log_dbgtrace(__FILE__, __LINE__, msg DLOG_NEWLINE, ##__VA_ARGS__)
# define LOG_INFO(msg, ...) log_info(msg DLOG_NEWLINE, ##__VA_ARGS__)
# define LOG_WARNING(msg, ...) log_warn(msg DLOG_NEWLINE, ##__VA_ARGS__)
# define LOG_ERROR(msg, ...) log_error(msg DLOG_NEWLINE, ##__VA_ARGS__)
# define LOG_SYS_ERROR(msg, ...) log_syserr(errno, msg, ##__VA_ARGS__)


#endif

