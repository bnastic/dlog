#include "shmem.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <string.h>


int shmem_close(shmem* mem);
int shmem_close_and_unlink(shmem* mem);

shmem*
shmmem_attach(void)
{
	int fd = shm_open(DLOG_SHMEM_NAME, O_RDONLY);
	if (-1 == fd) {
		LOG_SYS_ERROR("Failed to shmem_attach");
		return NULL;
	}

	void* addr = mmap(NULL,
					sizeof(shmem),
					PROT_READ,
					MAP_SHARED,
					fd,
					0);

	if ((void *) -1  == addr) {
		LOG_SYS_ERROR("Failed to open shared file");
		close(fd);
		return NULL;
	}

	size_t map_size = ((shmem*)addr)->size;
	munmap(addr, sizeof(shmem));

	/* remap to correct size */
	addr = mmap(NULL,
				map_size,
				PROT_READ,
				MAP_SHARED,
				fd,
				0);

	if ((void *) -1  == addr) {
		LOG_SYS_ERROR("Failed to open shared file");
		close(fd);
		return NULL;
	}

	return addr;
}

shmem*
shmmem_create(size_t size, bool create)
{
	size_t totalsz = size + sizeof(shmem);
	int flags = O_RDWR;

	if (create)
		flags |= O_CREAT;

	int fd = shm_open(DLOG_SHMEM_NAME, flags, 0644);
	if (-1 == fd) {
		LOG_SYS_ERROR("Failed to shm_create");
		return NULL;
	}

	if (create)
		ftruncate(fd, totalsz);

	void* addr = mmap(NULL,
					totalsz,
					PROT_READ | PROT_WRITE,
					MAP_SHARED,
					fd,
					0);

	if ((void *) -1  == addr) {
		LOG_SYS_ERROR("Failed to open shared file");
		close(fd);
		unlink(DLOG_SHMEM_NAME);
		return NULL;
	}

	shmem* m = (shmem* )addr;
	m->fd = fd;
	m->size = totalsz;
	if (create)
		m->base_addr = m->buf;

	return m;

	/*
	sem_t* semptr = sem_open(SemaphoreName,
							O_CREAT,
							AccessPerms,
							0);
	if (semptr == (void*) -1) report_and_exit("sem_open");
	strcpy(memptr, MemContents);
	if (sem_post(semptr) < 0) report_and_exit("sem_post");

	*/
}

int
shmem_close_and_unlink(shmem* mem)
{
	if (0 == shmem_close(mem)) {
		unlink(mem->slash_name);
		return 0;
	} else {
		LOG_SYS_ERROR("shm_close failed");
		return -1;
	}
}

int
shmem_close(shmem* mem)
{
	munmap(mem, mem->size);
	close(mem->fd);
	return 0;
}

void*
shmem_baseptr(shmem* base)
{
	return (base + 1);
}

