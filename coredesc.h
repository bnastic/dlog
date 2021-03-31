#ifndef DLOG_COREDESC_H__
#define DLOG_COREDESC_H__
#include "def.h"
#include "dynstr.h"
#include <sys/types.h>
#include <sys/queue.h>

struct descriptor;
struct origin_file;
struct origin_socket;
/* struct origin; */
struct linereader;
struct writequeue;

enum DSTATE
{
	DSTATE_INVALID = 0,
	DSTATE_INIT = 1,
	DSTATE_PENDING = 2,
	DSTATE_ACTIVE = 4,
	DSTATE_DRAIN = 8,
	DSTATE_DRAIN_ROTATE = 16,
	DSTATE_DEAD = 32
};

enum DOPENFLAGS
{
	DOPEN_NOFLAGS = 0,
	DOPEN_SEEKSTART = 1,
	DOPEN_SEEKEND = 2,
	DOPEN_TRUNC = 4,
	DOPEN_KEEP_BUFFERS = 8
};

typedef struct origin_file
{
	char* path;
	int size;
} origin_file;

typedef struct origin_socket
{
	char* host;
	char* port;
} origin_socket;

#define DESC_CORE_BITMASK 255

typedef struct dorigin {
	enum {
		D_TYPEINVALID		= 0,
		D_FILER				= 1,
		D_FILEW				= 1 << 1,
		D_FIFOR				= 1 << 2,
		D_FIFOW				= 1 << 3,
		D_SOCKETR			= 1 << 4,
		D_SOCKETW			= 1 << 5,
		D_SOCKET_LISTEN		= 1 << 6,
		D_INOTIFY			= 1 << 7,
		/* complex types */
		D_ROTLOG			=(1 << 8) | D_FILEW
	} type;

	union {
		origin_file file;
		origin_socket socket;
	};

	struct {
		char* buffer;
		int buf_idx;
		int fd;
	} inherited;

	char* symbol;
	struct dorigin* next;

} dorigin;

#define D_CORE_TYPE(t) ((t) & 255)
#define D_CORE_READ_TYPES (D_FILER|D_FIFOR|D_SOCKETR)
#define D_IS_WRITE_SIDE(t) ((t) & (D_FILEW | D_FIFOW | D_SOCKETW))
#define D_IS_READ_SIDE(t) ((t) & (D_FILER | D_FIFOR | D_SOCKETR | D_SOCKET_LISTEN))
#define D_IS_SOCKET_READ(t) ((t) & (D_SOCKETR|D_SOCKET_LISTEN))
#define D_IS_SOCKET_WRITE(t) ((t) & (D_SOCKETW))
#define D_IS_FILE(t) ((t) & (D_FILEW|D_FILER|D_FIFOR|D_FIFOW))

struct vdescfn
{
	int (*on_activate)(struct descriptor* );
	int (*on_deactivate)(struct descriptor* );
	int (*pre_read)(struct descriptor*, int);
	int (*post_line_write)(struct descriptor*, ssize_t nbytes, int write_err_code);
	void * state;
};

typedef struct descriptor
{
	int state;
	int type;
	int fd;
	dorigin* origin;
	struct vdescfn vfn;
	dynstr* symbol;

	union {
		struct linereader* reader;
		struct writequeue* wqueue;
	};

	TAILQ_ENTRY(descriptor) _lnk;

} descriptor;

descriptor* open_descriptor(dorigin* or, descriptor* d, struct vdescfn*, int flags);
void close_descriptor(descriptor* d);
void reset_descriptor(descriptor* d);
//descriptor* open_socket_read(int fd);
//descriptor* open_socket_read_with_buffer(int fd, const char*);
void free_dorigin(struct dorigin *);

extern struct dorigin inotify_origin;

#endif

