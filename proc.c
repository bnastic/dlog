#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

#include "log.h"
#include "proc.h"
#include "env.h"

static void _sig_handler(int signo, siginfo_t* si, void* uctx);

struct sig_s signals[] =
{
	{ dlog_signal(DLOG_SIG_RESTART),
		"SIG" dlog_value(SIG_RESTART),
		0,
		_sig_handler },
	{ dlog_signal(DLOG_SIG_ROTLOG_ROT),
		"SIG" dlog_value(SIG_ROTLOG_ROT),
		0,
		_sig_handler },
	{ dlog_signal(DLOG_SIG_SHUTDOWN),
		"SIG" dlog_value(SIG_SHUTDOWN),
		0,
		_sig_handler },
	{0, NULL, 0, NULL}
};


int
sig_init(bool not_daemon)
{
	/* make sure the mask is unblocked... */
	struct sigaction sa;
	sigset_t sset;

	for (struct sig_s* sig = &signals[0]; sig->signo; sig++) {
		memset(&sa, 0, sizeof(struct sigaction));
		if (sig->handler) {
			sa.sa_sigaction = sig->handler;
			sa.sa_flags = SA_SIGINFO;
			sigaddset(&sset, sig->signo);
		} else {
			sa.sa_handler = SIG_IGN;
		}
		sigemptyset(&sa.sa_mask);
		if (-1 == sigaction(sig->signo, &sa, NULL)) {
			LOG_SYS_ERROR("Failed to install signal handler for %s", sig->signame);
			return -1;
		}
	}
	if (sigprocmask(SIG_UNBLOCK, &sset, NULL) != 0) {
		LOG_SYS_ERROR("Failed to unblock signals, will ignore");
	}

	return 0;
}

int
sig_blockall(bool allow_int)
{
	sigset_t s;
	sigfillset(&s);

	if (allow_int) {
		sigdelset(&s, SIGINT);
	}

	if (sigprocmask(SIG_BLOCK, &s, NULL) != 0) {
		LOG_SYS_ERROR("Failed to block all signals");
		return -1;
	}
	return 0;

}

void
sig_resetall(void)
{
#if defined(DLOG_HAVE_LINUX) && defined(_NSIG)
	const int nsig = _NSIG;
#elif defined(NSIG)
	const int nsig = NSIG;
#else
	const int nsig = 32;
#endif
	for (int i = 0; i < nsig; i++)
		signal(i, SIG_DFL);
}

int
sig_send_to_pid(int sig)
{
	pid_t proc_pid = pidfile_read();
	if (proc_pid > 1) {
		if (-1 == kill(proc_pid, sig)) {
			return -1;
		}
		return 0;
	}

	return -1;
}

static void
_sig_handler(int signo, siginfo_t* si, void* uctx)
{
	for (struct sig_s* sig = &signals[0]; sig->signo; sig++) {
		if (sig->signo == signo) {
			sig->flag = 1;

			dlogenv->sig_delivered = 1;

			break;
		}
	}
}

int
pidfile_create(void)
{
	char* buf = NULL;
	int pidfd;
	const char* filename = dlogenv->config.pidfile;
	pid_t pid = getpid();

	pidfile_delete();

	pidfd = open(filename, O_RDWR|O_CREAT,
					 S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (-1 == pidfd) {
		LOG_SYS_ERROR("Failed to create new pidfile %s", filename);
		return -1;
	}

	ftruncate(pidfd, 0);

	asprintf(&buf, "%d", pid);
	if (-1 == write(pidfd, buf, strlen(buf))) {
		LOG_SYS_ERROR("Failed to write pid to pid file");
		return 1;
	}

	free(buf);
	return 0;
}

int
pidfile_delete(void)
{
	const char* filename = dlogenv->config.pidfile;

	if (access(filename, F_OK) != -1) {
		unlink(filename);
		errno = 0;
	}

	return 0;
}

pid_t
pidfile_read(void)
{
	pid_t pid = -1;

	/* no special considerations here, use FILE */
	FILE* pidf = fopen(dlogenv->config.pidfile, "r");
	if (!pidf) {
		LOG_SYS_ERROR("Failed to open pidfile");
		return -1;
	} else {
		int ret = fscanf(pidf, "%d", &pid);
		if (ret == 0 || ret == EOF) {
			LOG_SYS_ERROR("Failed to read pidfile");
		}
	}
	fclose(pidf);
	return pid;
}

void
proc_savecmd(int argc, char** argv, char** envp)
{
	int i=0;

	dlogenv->env_param.argc = argc;
	dlogenv->env_param.argv = malloc(sizeof(char*) * (argc + 1));

	do {
		dlogenv->env_param.argv[i] = argv[i];
	} while(++i < argc);
	dlogenv->env_param.argv[argc] = NULL;

	i = 0;
	dlogenv->env_param.envp = malloc(sizeof(char *));
	while(envp[i]) {
		dlogenv->env_param.envp[i] = strdup(envp[i]);
		i++;
		dlogenv->env_param.envp = realloc(dlogenv->env_param.envp,
										  (i+1) * sizeof(char *));
		dlogenv->env_param.envp[i] = NULL;
	}

	proc_addparam("-x");
}

void
proc_addenv(const char* penv)
{
	dlogenv->env_param.envc++;
	dlogenv->env_param.envp = realloc(dlogenv->env_param.envp,
							sizeof(char*) * (dlogenv->env_param.envc + 1));
	dlogenv->env_param.envp[dlogenv->env_param.envc] = strdup(penv);
	dlogenv->env_param.envp[dlogenv->env_param.envc + 1] = NULL;
}

void
proc_addparam(const char* param)
{
	dlogenv->env_param.argc++;
	dlogenv->env_param.argv = realloc(dlogenv->env_param.argv,
								sizeof(char*) * (dlogenv->env_param.argc + 1));
	dlogenv->env_param.argv[dlogenv->env_param.argc-1] = strdup(param);
	dlogenv->env_param.argv[dlogenv->env_param.argc] = NULL;
}

void
proc_restart_with_newbinary(void)
{
	if (fork() == 0) {
		execve(dlogenv->env_param.argv[0], dlogenv->env_param.argv, NULL);
	}
}

int
proc_daemonize(void)
{
	int  fd;
	//pid_t ppid = getpid();

	switch (fork()) {
	case -1:
		LOG_SYS_ERROR("fork() failed");
		return -1;

	case 0:		/* daemon */
		break;

	default:	/* parent */
		LOG_INFO("Master process exiting.");
		exit(0);
	}

	if (setsid() == -1) {
		LOG_SYS_ERROR("setsid() failed");
		return -1;
	}

	umask(0);

	fd = open("/dev/null", O_RDWR);
	if (fd == -1) {
	    LOG_SYS_ERROR("open(\"/dev/null\") failed");
	    return -1;
	}

	if (dup2(fd, STDIN_FILENO) == -1) {
	    LOG_SYS_ERROR("dup2(STDIN) failed");
	    return -1;
	}

	if (dup2(fd, STDOUT_FILENO) == -1) {
	    LOG_SYS_ERROR("dup2(STDOUT) failed");
	    return -1;
	}

	if (fd > STDERR_FILENO) {
	    if (close(fd) == -1) {
		    LOG_SYS_ERROR("close() failed");
		    return -1;
	    }
	}

	return 0;
}

