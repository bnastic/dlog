#ifndef DLOG_PROC_H__
#define DLOG_PROC_H__

#include <signal.h>
#include <sys/types.h>

#include "def.h"

#define dlog_defsig(n)				SIG##n
#define dlog_signal(sig)			dlog_defsig(sig)

#define DLOG_SIG_RESTART			HUP
#define DLOG_SIG_ROTLOG_ROT			USR1
#define DLOG_SIG_SHUTDOWN			QUIT

struct sig_s
{
	int signo;
	char* signame;
	sig_atomic_t flag;
	void (*handler)(int signo, siginfo_t* si, void* ucontext);
};

extern struct sig_s signals[];
int		sig_init(bool not_daemon);
int		sig_blockall(bool allow_int);
void	sig_resetall(void);
int		sig_send_to_pid(int sig);
int		pidfile_check(void);
int		pidfile_create(void);
int		pidfile_delete(void);
pid_t	pidfile_read(void);
void	proc_savecmd(int argc, char** argv, char** envp);
void	proc_addenv(const char* penv);
void	proc_addparam(const char* param);
int		proc_daemonize(void);
void	proc_restart_with_newbinary(void);

#endif

