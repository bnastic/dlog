#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "def.h"
#include "log.h"
#include "coredesc.h"
#include "rotlog.h"

static int rotlog_on_activate(struct descriptor* d);
static int rotlog_post_line_write(struct descriptor* d, ssize_t nbytes, int err);

static struct vdescfn
rotlog_vfn =
{
	.on_activate = &rotlog_on_activate,
	.on_deactivate = NULL,
	.pre_read = NULL,
	.post_line_write = rotlog_post_line_write,
	.state = NULL
};

descriptor*
open_rotlog(dorigin* origin)
{
	return open_descriptor(origin, NULL, &rotlog_vfn, DOPEN_NOFLAGS);
}


static int
rotlog_on_activate(struct descriptor* d)
{
	if (!d->vfn.state)
		d->vfn.state = calloc(1, sizeof(int));
	else
		memset(d->vfn.state, 0, sizeof(int));

	// file guarantied to exist here
	struct stat st;
	fstat(d->fd, &st);
	*((int *)d->vfn.state) = st.st_size;

	return 0;
}

static int
rotlog_post_line_write(struct descriptor* d, ssize_t nbytes, int err)
{
	int *cur_file_size = (int *)d->vfn.state;

	*cur_file_size += nbytes;
	if (*cur_file_size < d->origin->file.size) {
		return 0;
	}

	rotlog_rotate(d);

	return 0;
}

void
rotlog_rotate(descriptor* d)
{
	struct tm tme;
	struct timespec ts;
	char *fbuf;
	char tbuf[16];
	int r;


	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tme);
	strftime(tbuf, sizeof(tbuf), DLOG_ROTLOG_TIMESTAMPEXT, &tme);

	asprintf(&fbuf, "%s.%s", d->origin->file.path, tbuf);
	close(d->fd);
	if ((r = rename(d->origin->file.path, fbuf)) == -1) {
		LOG_SYS_ERROR("Rotation log failed to rename file. Will"
					  " continue to write into the same file.");
	} else {
		open_descriptor(d->origin, d, &rotlog_vfn, DOPEN_NOFLAGS);
	}

	free(fbuf);
}

#if 0
void rotlog_rotate(descriptor* d)
{
	int r;
	char *fbuf;
	struct stat stbuf;

	/* close current file and rename it to tmp1 (for rotation) */
	asprintf(&fbuf, "%s.tmp1", d->origin->file.path);
	close(d->fd);

	if ((r = rename(d->origin->file.path, fbuf)) != -1) {

		/* rotate all files in a new process */
		if (fork() == 0) {
			int filecnt = 1;
			char* fl = NULL;
			char *tmp1, *tmp2;
			char* logf = strdup(d->origin->file.path);
			asprintf(&tmp1, "%s.tmp1", logf);
			asprintf(&tmp2, "%s.tmp2", logf);
			while(1) {
				asprintf(&fl, "%s.%d", logf, filecnt);
				if (stat(fl, &stbuf) == -1) {
					rename(tmp1, fl);
					break;
				} else {
					rename(fl, tmp2);
					rename(tmp1, fl);
					rename(tmp2, tmp1);
				}
				filecnt++;
			}
			free(logf);
			free(tmp1);
			free(tmp2);
			free(fl);
			exit(0);
		}
	} else {
		LOG_SYS_ERROR("Rotation log failed to rename file. Will"
		   " continue to write into the same file!");
	}

	free(fbuf);
	open_descriptor(d->origin, d, &rotlog_vfn, DOPEN_NOFLAGS);
}
#endif

