#ifndef _FD_XFER_H__
#define _FD_XFER_H__

struct descriptor;

typedef struct
{
	int in_fd;
	int desc_type;
	int	buf_idx; /* from reader */
	size_t buf_len;
	char buf[];
} xfer_msg;

int fdxfer_open_recv(xfer_msg***, int*);
int fdxfer_open_send(void);
int fdxfer_send(struct descriptor* );
void fdxfer_close(void);

#endif

