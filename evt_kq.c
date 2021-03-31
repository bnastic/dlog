#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <libgen.h>
#include "log.h"
#include "evt.h"
#include "coredesc.h"


/* file watcher
 *
 * kqueue can't monitor filenames. So we have to watch directory
 * and check (access()) every file every time there is a change
 * in the directory. Because kqueue uses fd-filter as the key
 * we can't just keep adding events for every file as they may
 * share the same directory, and every time we add a directory
 * we have to open it and get a new fd which will create a new
 * entry in kqueue. Hence, we have to keep a helper structure
 * of already added directories that points to a list of
 * descriptors (with filenames) that we are monitoring
 *
 * XXX Replace this stuff with hashtables once it's all integrated
 *     back
 */
enum vnode_type
{
	VNODE_FILE,
	VNODE_DIR
};

typedef struct filew
{
   descriptor* d;
   TAILQ_ENTRY(filew) link;
} filew;

typedef struct dirw
{
	char* path;
	int dirfd;
    TAILQ_ENTRY(dirw) link;
	TAILQ_HEAD(, filew) files;
} dirw;

TAILQ_HEAD(, dirw) dirwatchers = TAILQ_HEAD_INITIALIZER(dirwatchers);

static void file_watch_deletion(descriptor*);
static dirw* find_dir(const char*);
static dirw* find_dir_fd(int fd);
static int add_file_watch(descriptor*, int* , char** );
static int remove_file_watch(int , descriptor*);

static EVT_SYS _evtsys_;

static dirw*
find_dir(const char* path)
{
	dirw* dir;

	TAILQ_FOREACH(dir, &dirwatchers, link) {
		if (!strcmp(path, dir->path)) {
			return dir;
		}
	}

	return NULL;
}

static dirw*
find_dir_fd(int fd)
{
	dirw* dir;

	TAILQ_FOREACH(dir, &dirwatchers, link) {
		if (fd == dir->dirfd) {
			return dir;
		}
	}

	return NULL;
}

/* wait for file to show up */
static int
add_file_watch(descriptor* d, int* fd, char** dirpath)
{
	char path[PATH_MAX];
	dirw* dir;
	filew* file, *newfile;
	int flags;

#if defined (__FreeBSD__)
	/* FreeBSD dirname overwrites input buffer */
	strncpy(path, d->origin->file.path, PATH_MAX);
	(void) dirname(path);
#elif defined (DLOG_HAVE_OSX)
	/* OS X  has re-entrant version available */
	if (dirname_r(d->origin->file.path, path) == NULL) {
		return -1;
	}
#elif defined(__OpenBSD__) || defined(__NetBSD__)
	/* Open/NetBSD returns static storage (not re-entrant) */
	strncpy(path, dirname(d->origin->file.path), PATH_MAX);
#endif
	LOG_DEBUG("file_watch, directory extracted is (%s)", path);

	// try to open it before adding
#if defined (DLOG_HAVE_OSX)
	flags = O_EVTONLY;
#else
	flags = O_RDONLY | O_DIRECTORY;
#endif
	flags |= O_NONBLOCK;

	int dfd = open(path, flags);
	if (dfd == -1) {
		return -1;
	}

	file = newfile = NULL;

	if ((dir = find_dir(path)) == NULL) {
		dir = calloc(1, sizeof(*dir));
		dir->path = strdup(path);
		dir->dirfd = dfd;
		TAILQ_INIT(&dir->files);
		TAILQ_INSERT_TAIL(&dirwatchers, dir, link);
	}

	TAILQ_FOREACH(file, &dir->files, link) {
		if (d == file->d) {
			close(dfd);
			return 0; // already exists
		}
	}

	file = calloc(1, sizeof(*file));
	file->d = d;
	TAILQ_INSERT_TAIL(&dir->files, file, link);

	*fd = dfd;
	*dirpath = dir->path;
	return dfd;
}

static int
remove_file_watch(int dirfd, descriptor* d)
{
	filew* file;
	dirw* dir = find_dir_fd(dirfd);

	if (!dir) {
		LOG_ERROR("Removing unknown directory watcher for %s", d->origin->file.path);
		return 1;
	}

	TAILQ_FOREACH(file, &dir->files, link) {
		if (d == file->d) {
			TAILQ_REMOVE(&dir->files, file, link);
			return 0;
		}
			/* no point in removing the directory entry */
#if 0

			if (TAILQ_EMPTY(&dir->files)) {
				/* last file watcher removed */
				close(dirfd);
				TAILQ_REMOVE(&dirwatchers, dir, link);
			}
#endif
	}
	return 1;
}


void
evt_sys_create(void)
{
	_evtsys_ = kqueue();
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

int
evt_reg_read(descriptor* d)
{
	struct kevent evt;

	EV_SET(&evt, d->fd, EVFILT_READ, EV_ADD|EV_CLEAR, 0, 0, d);

	return kevent(_evtsys_, &evt, 1, NULL, 0, NULL);
}

int
evt_reg_write(descriptor* d)
{
	struct kevent evt;

	EV_SET(&evt, d->fd, EVFILT_WRITE, EV_ADD|EV_CLEAR|EV_ONESHOT, 0, 0, d);

	return kevent(_evtsys_, &evt, 1, NULL, 0, NULL);
}


void
evt_reg_vnode_del(descriptor* d)
{
	file_watch_deletion(d);
}

/* -1 - failed to watch directory, 0 - watcher added, 1 - already watching */
int
evt_watch_vnode(descriptor* d)
{
	int dfd;
	char* path;
	struct kevent ke;
	uintptr_t tagptr;

	dfd = add_file_watch(d, &dfd, &path);
	if (dfd == -1) {
		LOG_SYS_ERROR("Invalid directory name");
		return -1;
	}

	if (dfd == 0) {
		LOG_DEBUG("File already watched");
		return 1;
	}

	tagptr = TAGPTR(path, VNODE_DIR);
    EV_SET(&ke, dfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
			NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE, 0, (void*)tagptr);
	(void) kevent(_evtsys_, &ke, 1, NULL, 0, NULL);

	LOG_DEBUG("evt_watch_vnode added path (%s)", path);
	return 0;
}

static void
file_watch_deletion(descriptor* d)
{
	struct kevent ke;
	uintptr_t tagp = TAGPTR(d, VNODE_FILE);

	EV_SET(&ke, d->fd, EVFILT_VNODE, EV_ADD | EV_CLEAR | EV_ONESHOT, NOTE_DELETE | NOTE_RENAME, 0, (void*)tagp);
	(void) kevent(_evtsys_, &ke, 1, NULL, 0, NULL);

	LOG_DEBUG("evt_watch_deletion added for (%s)", d->origin->symbol);

}

void
evt_unwatch_vnode(int dirfd, descriptor* d)
{
	remove_file_watch(dirfd, d);
}

/*
 * NOTE
 *
 * OS X - only reports NOTE_WRITE event when there are any changes in the directory
 */
void
evt_process_vnode(struct kevent* evt, descriptor** ds, int* numfd)
{
	filew* file;
	dirw* dir;
	void* data;
	unsigned vnodetype;

	UNTAGPTR((uintptr_t)evt->udata, vnodetype, data);

	if (vnodetype == VNODE_DIR) {
		char* path = (char* )data;
		TAILQ_HEAD(, filew) flist = TAILQ_HEAD_INITIALIZER(flist);

		if (evt->fflags & (NOTE_WRITE|NOTE_EXTEND)) {
			if ((dir = find_dir(path))) {
				TAILQ_FOREACH(file, &dir->files, link) {
					if (access(file->d->origin->file.path, O_RDONLY) == 0) {

						TAILQ_REMOVE(&dir->files, file, link);
						TAILQ_INSERT_TAIL(&flist, file, link);

						descriptor* d = file->d;
						open_descriptor(d->origin, d, NULL, DOPEN_SEEKSTART|DOPEN_KEEP_BUFFERS);
					}
				}

				TAILQ_FOREACH(file, &flist, link) {
					remove_file_watch(dir->dirfd, file->d);
				}
			}
		}
	} else if (vnodetype == VNODE_FILE) {
		descriptor* d = (descriptor* )data;
		/* file was deleted? */
		if (evt->fflags & (NOTE_DELETE|NOTE_RENAME)) {
			struct stat st;
			if (fstat(d->fd, &st) == 0) {
				LOG_DEBUG("File unlink()-ed, current link count %d, resetting descriptor", st.st_nlink);

				/* this will close(fd) which removes evt registry for that fd (on BSD) */
				//reset_descriptor(d);
				//open_descriptor(d->origin, d, NULL, DOPEN_SEEKSTART|DOPEN_KEEP_BUFFERS);
				d->state = DSTATE_DRAIN_ROTATE;
				ds[0] = d;
				*numfd = 1;
			}
		}
	}
}

int
evt_clear_state(struct kevent* e, int fd)
{
	struct kevent ke;
	EV_SET(&ke, fd, e->filter, EV_CLEAR, 0, 0, NULL);
	LOG_DEBUG("- event - clearing state...");
	return kevent(_evtsys_, &ke, 1, NULL, 0, NULL);
}

int	evt_reg_remove(descriptor* d)
{
	struct kevent ke[3];

    EV_SET(&ke[0], d->fd, EVFILT_VNODE, EV_DELETE, 0, 0, d);
    EV_SET(&ke[1], d->fd, EVFILT_READ, EV_DELETE, 0, 0, d);
    EV_SET(&ke[2], d->fd, EVFILT_WRITE, EV_DELETE, 0, 0, d);
	return kevent(_evtsys_, ke, 3, NULL, 0, NULL);
	return 0;
}

