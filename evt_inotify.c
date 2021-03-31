#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "def.h"
#include "evt.h"
#include "log.h"
#include "coredesc.h"

/*
 * Inotify works with inodes and file names, unlike kqueue.
 * This means that we have to do the same work as with kqueue
 * to monitor for file creation. But this time, the file that had
 * appeared is given in the 'name' in the event field, so no need
 * to manually check every single file in the watched list.
 *
 * But unlike kqueue, there is another level of indirection
 * with inotify, as all events are accessed through
 * inotify's FD. This means we can't deal with 'read' events
 * here. With inotify, files are always read-ready. But we can observe
 * 'write' state of the file(s) and keep them in the 'pending' list
 * for later reads.
 *
 * NOTE: We can't monitor file deletion through inotify - as it observes
 * inodes only and no delete event will trigger as long as there are any
 * references to the file (of which we'll hold at least one). The only
 * way around this is to monitor IN_ATTRIB changes and observe if the
 * amount of links has gone down, and then access() the file to check
 * if still there.
 */

int inotify_fd;
static EVT_SYS _evtsys_;

typedef struct filew
{
	int wd;
	int link_count;
	descriptor* d;
	char* basename;

	TAILQ_ENTRY(filew) link;
} filew;

typedef struct dirw
{
	int wd;
	char* dirname;

    TAILQ_ENTRY(dirw) link;
	TAILQ_HEAD(, filew) files;
} dirw;

/* directory monitors */
TAILQ_HEAD(, dirw) dirwatchers = TAILQ_HEAD_INITIALIZER(dirwatchers);

/* file monitors */
TAILQ_HEAD(, filew) filewatchers = TAILQ_HEAD_INITIALIZER(filewatchers);

static dirw* find_dir(int wd);
static filew* find_file_in_dir(dirw* dr, const char* base_filename);
static filew* find_watched_file(int filewd);
static void remove_watch_file_from_dir(dirw* d, filew* f);
static void remove_watch_file(filew* f);
static int file_link_count(int fd);

void
evt_sys_create(void)
{
	_evtsys_ = epoll_create1(EPOLL_CLOEXEC);
}

EVT_SYS
evt_sys(void)
{
	return _evtsys_;
}

void
evt_sys_destroy(void)
{
	close(_evtsys_);
}

/* -1 - failed to watch directory, 0 - watcher added, 1 - already watching */
int
evt_watch_vnode(struct descriptor* d)
{
	/* Linux has a nasty habit of overwriting strings in dirname.
	 * Make sure to copy it again when reused
	 */
	char path[PATH_MAX];
	char dirn[PATH_MAX];
	char basen[PATH_MAX];
	dirw* dir;
	filew* file;
	int ifd, ret=1;

	strcpy(path, d->origin->file.path);
	strcpy(dirn, dirname(path));
	strcpy(path, d->origin->file.path);
	strcpy(basen, basename(path));

	LOG_DEBUG("(1)Watching directory (%s) for dir \'%s\' for file \'%s\'",
		d->origin->file.path, dirn, basen);

	ifd = inotify_add_watch(inotify_fd, dirn, IN_CREATE|IN_ONLYDIR);
	if (ifd == -1) {
		LOG_SYS_ERROR("Failed to watch directory %s", dirn);
		ret = -1;
		goto done;
	}

	if ((dir = find_dir(ifd)) == NULL) {
		dir = calloc(1, sizeof(*dir));
		dir->wd = ifd;
		dir->dirname = dirn;
		TAILQ_INIT(&dir->files);
		TAILQ_INSERT_TAIL(&dirwatchers, dir, link);
	}

	TAILQ_FOREACH(file, &dir->files, link) {
		if (d == file->d) {
			goto done;
		}
	}

	file = calloc(1, sizeof(*file));
	file->d = d;
	file->basename = strdup(basen);
	file->wd = -1;
	TAILQ_INSERT_TAIL(&dir->files, file, link);

	LOG_DEBUG("Watching directory \'%s\' for file \'%s\'", dirn, basen);

	return 0;

done:
	return ret;
}


void
evt_process_vnode(struct epoll_event* evt, descriptor** descarr, int* nbdesc)
{
	char buf[4096]
	__attribute__ ((aligned(__alignof__(struct inotify_event))));

	const struct inotify_event *event;
	char *ptr = NULL; //, *evt_file = NULL;
	*nbdesc = 0;

	while(1) {
		ssize_t len = read(inotify_fd, buf, sizeof buf);
		if (len == -1 && errno != EAGAIN) {
			LOG_SYS_ERROR("inotify - read error");
			break;
		}

	   if (len <= 0)
		   break;

		for (ptr = buf; ptr < buf + len;
				ptr += sizeof(struct inotify_event) + event->len)
		{
			event = (const struct inotify_event *) ptr;

			dirw* dr = find_dir(event->wd);
			if (dr) {

				/* directory event - only care for created new files */
				if (event->mask & (IN_CREATE|IN_MOVED_TO)) {
					filew* file = find_file_in_dir(dr, event->name);

					if (file) {

						remove_watch_file_from_dir(dr, file);

						open_descriptor(file->d->origin, file->d,
										NULL, DOPEN_SEEKSTART|DOPEN_KEEP_BUFFERS);

						continue;
					}
				}
			} else {

				/* file event - we care about deletion and write events */

				if (event->mask & IN_IGNORED) {
					/* events gone missing... ignore them */
					continue;
				}

				filew* file = find_watched_file(event->wd);

				if (!file) {
					LOG_ERROR("Failed to find watched file in response to inotify event");
					continue;
				}

				if (event->mask & IN_MODIFY) {

					/* file was written to, save it */
					descarr[*nbdesc] = file->d;

					(*nbdesc)++;

				} else if (event->mask & IN_ATTRIB) {

					/* file attribute(s) changes, check link count and if still accessible */

					if (file_link_count(file->d->fd) < file->link_count) {
						if (-1 == access(file->d->origin->file.path, F_OK)) {
							LOG_DEBUG("File (%s) is gone", file->d->origin->file.path);
							remove_watch_file(file);
							file->d->state = DSTATE_DRAIN_ROTATE;
						}
					}
				}
			}
		}
	}
}

int
evt_reg_read(struct descriptor* d)
{
	struct epoll_event evt;
	filew* file;

	if (!(D_IS_FILE(d->type))) {
		evt.events = EPOLLIN | EPOLLET;
		evt.data.ptr = d;
		if (-1 == epoll_ctl(evt_sys(), EPOLL_CTL_ADD, d->fd, &evt)) {
			LOG_SYS_ERROR("Failed to register read event for %s", d->origin->symbol);
			return -1;
		}
	} else {

		file = calloc(1, sizeof(*file));
		file->d = d;
		file->basename = d->origin->file.path;
		file->link_count = -1;
		file->wd = inotify_add_watch(inotify_fd,
									d->origin->file.path,
									IN_MODIFY | IN_MASK_ADD);

		if (file->wd == -1) {
			LOG_SYS_ERROR("Failed to register file modification event in inotify");
			free(file);
			return -1;
		} else {

			if (d->fd != -1) {
				file->link_count = file_link_count(d->fd);
			}

			TAILQ_INSERT_TAIL(&filewatchers, file, link);
		}
	}

	return 0;
}

int
evt_reg_write(struct descriptor* d)
{
	struct epoll_event evt;

	if (D_IS_SOCKET_WRITE(d->type)) {
		evt.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
		evt.data.ptr = d;
		if (-1 == epoll_ctl(evt_sys(), EPOLL_CTL_ADD, d->fd, &evt)) {
			LOG_SYS_ERROR("Failed to register write event for %s", d->origin->symbol);
			return -1;
		}
	}
	return 0;
}

int
evt_reg_remove(struct descriptor* d)
{
	int ret = epoll_ctl(evt_sys(), EPOLL_CTL_DEL, d->fd, NULL);
	if (ret == -1)
		LOG_SYS_ERROR("Failed to unregister epool for %s", d->origin->symbol);

	return ret;
}

int
evt_clear_state(EVT_CONTEXT* e, int fd)
{
	/* epool doesn't need EOF clearing */
	return 0;
}

void
evt_unwatch_vnode(int dirfd, struct descriptor* d)
{
	/* TODO this is never used */
}

void
evt_reg_vnode_del(struct descriptor* d)
{
	int wd;
	if (-1 == (wd = inotify_add_watch(inotify_fd, d->origin->file.path,
										IN_ATTRIB | IN_MASK_ADD))) {
		LOG_SYS_ERROR("inotify failed to register file for deletion - %s", d->origin->symbol);
	}
}


static dirw*
find_dir(int wd)
{
	dirw* dir;

	TAILQ_FOREACH(dir, &dirwatchers, link) {
		if (wd == dir->wd) {
			return dir;
		}
	}

	return NULL;
}

static filew*
find_file_in_dir(dirw* dr, const char* base_filename)
{
	filew* file;

	if(dr) {
		TAILQ_FOREACH(file, &dr->files, link) {
			if (!strcmp(base_filename, file->basename)) {
				return file;
			}
		}
	}

	return NULL;
}

static void
remove_watch_file_from_dir(dirw* dr, filew* f)
{
	TAILQ_REMOVE(&dr->files, f, link);
}

static filew*
find_watched_file(int filewd)
{
	filew* f;
	TAILQ_FOREACH(f, &filewatchers, link) {
		if (filewd == f->wd) {
			return f;
		}
	}

	return NULL;
}

static void
remove_watch_file(filew* f)
{
	TAILQ_REMOVE(&filewatchers, f, link);
	LOG_DEBUG("inotify_rm_watch for file");
	inotify_rm_watch(inotify_fd, f->wd);
}

static int
file_link_count(int fd)
{
	struct stat st;

	if (fstat(fd, &st) == 0) {
		return st.st_nlink;
	}

	return -1;
}

