#include "log.h"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>
#include <sys/stat.h>

enum DLOG_LOGTYPE
{
	LOGTYPE_INVALID,
	LOGTYPE_FILE,
	LOGTYPE_SYSLOG,
	LOGTYPE_STDERR
};

enum DLOG_MSG_SEVERITY
{
	DLOG_MSG_DEBUG,
	DLOG_MSG_INFO,
	DLOG_MSG_WARNING,
	DLOG_MSG_ERROR
};

static enum DLOG_LOGTYPE log_type = LOGTYPE_INVALID;
static int log_file_fd = -1;
static char* _log_filename = NULL;

static int
_log_file(const char* line)
{
	if (log_file_fd != -1) {
		if (-1 == write(log_file_fd, line, strlen(line))) {
			if (errno == EBADF || errno == EINVAL) {
				log_create_file(_log_filename);
				_log_file(line);
			}
		}
		return 0;
	}
	return -1;
}

static int
_log_syslog(int severity, const char* line)
{
	int sev = severity == DLOG_MSG_DEBUG?LOG_DEBUG:
		(DLOG_MSG_INFO?LOG_INFO:
		(DLOG_MSG_WARNING?LOG_WARNING:
		(DLOG_MSG_ERROR?LOG_ERR:LOG_NOTICE)));
	syslog(sev, "dlog - %s", line);
	return 0;
}

static int
_log_stderr(const char* line)
{
	return write(2, line, strlen(line));
}

static int
_write_log(int severity, const char* line)
{
	switch (log_type) {
		case LOGTYPE_FILE: return _log_file(line);
		case LOGTYPE_SYSLOG: return _log_syslog(severity, line);
		case LOGTYPE_STDERR: return _log_stderr(line);
		default: break;
	}
	return -1;
}

void
log_create_stderr()
{
	log_type = LOGTYPE_STDERR;
}

void
log_create_file(const char* filename)
{
	if (!_log_filename)
		_log_filename = strdup(filename);
	log_type = LOGTYPE_FILE;
	log_file_fd = open(_log_filename, O_CREAT|O_WRONLY|O_APPEND, S_IRWXU|S_IRGRP);
}

void
log_create_syslog()
{
	log_type = LOGTYPE_SYSLOG;
}

void
log_close()
{
	if (log_type == LOGTYPE_FILE) {
		free(_log_filename);
		close(log_file_fd);
	}
}

void
log_dbgtrace(const char* file, int line, const char* msg, ...)
{
	char buffer[2048];
	int offset = snprintf(buffer, sizeof(buffer), "DEBUG - %s:%d - ", file, line);
	va_list args;
	va_start(args, msg);
	vsnprintf(buffer+offset, sizeof(buffer)-offset, msg, args);
	va_end(args);
	_write_log(DLOG_MSG_DEBUG, buffer);
}

void
log_info(const char* msg, ...)
{
	char buffer[2048];
	int offset = snprintf(buffer, sizeof(buffer), "INFO  - ");
	va_list args;
	va_start(args, msg);
	vsnprintf(buffer+offset, sizeof(buffer)-offset, msg, args);
	va_end(args);
	_write_log(DLOG_MSG_INFO, buffer);
}

void
log_warn(const char* msg, ...)
{
	char buffer[2048];
	int offset = snprintf(buffer, sizeof(buffer), "WARN  - ");
	va_list args;
	va_start(args, msg);
	vsnprintf(buffer+offset, sizeof(buffer)-offset, msg, args);
	va_end(args);
	_write_log(DLOG_MSG_WARNING, buffer);
}

void
log_error(const char* msg, ...)
{
	char buffer[2048];
	int offset = snprintf(buffer, sizeof(buffer), "ERROR - ");
	va_list args;
	va_start(args, msg);
	vsnprintf(buffer+offset, sizeof(buffer)-offset, msg, args);
	va_end(args);
	_write_log(DLOG_MSG_ERROR, buffer);
}


void
log_syserr(int syserror, const char* msg, ...)
{
	char buffer[2048];
	char errbuf[512];
	int offset = snprintf(buffer, sizeof(buffer), "ERROR - ");
	va_list args;
	va_start(args, msg);
	offset += vsnprintf(buffer+offset, sizeof(buffer)-offset, msg, args);
	va_end(args);
	strerror_r(syserror, errbuf, sizeof(errbuf));
	snprintf(buffer+offset, sizeof(buffer)-offset, " (%s)\n", errbuf);
	_write_log(DLOG_MSG_ERROR, buffer);
}

