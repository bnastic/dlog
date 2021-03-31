#ifndef SHMEM_H__
#define SHMEM_H__
#include "def.h"

typedef struct shmem
{
	int fd;
	size_t size;
	int num_elems;
	char buf[] __attribute__((aligned(16)));
} shmem;

shmem* shmem_create(size_t size, bool create);
shmem* shmem_attach(void);
int shmem_close(shmem* mem);
int shmem_close_and_unlink(shmem* mem);

void* shmem_baseptr(shmem*);

#endif

