#include "def.h"
#include "mempool.h"
#include <stddef.h>
#include <stdlib.h>

#define ALIGN_TO(obj, algn_bytes) (((obj) + (algn_bytes-1)) &~ (algn_bytes-1))

struct slot_hdr {
	int free_next;
	int num_slots;
};

struct mempool
{
	int num_slots;
	int slot_sz;
	int free_list_head;
	int owns_buffer;
	char pool[] __attribute__((aligned(16)));
};

static struct slot_hdr*
_slot(mempool* p, int idx)
{
	return (struct slot_hdr *)(p->pool + idx * p->slot_sz);
}

static int
_idx(mempool* pool, void* ptr)
{
	ptrdiff_t diff = (char *)ptr - pool->pool;
	return diff / pool->slot_sz;
}

int
mempool_get_numslots(mempool* p)
{
	return (p->num_slots);
}

int
mempool_get_slotsz(mempool* p)
{
	return (p->slot_sz);
}

size_t
mempool_memsize(size_t elem_sz_b, int num_slots)
{
	return (sizeof(mempool) + num_slots * (ALIGN_TO(elem_sz_b, 16)));
}

bool
mempool_has_ptr(mempool* p, void* ptr)
{
	struct slot_hdr* h = (struct slot_hdr*)ptr;
	return (h >= _slot(p, 0) && h <= _slot(p, p->num_slots-1));
}

mempool*
mempool_create(size_t elem_sz_b, int num_slots)
{
	void* pool;
	if (0 == posix_memalign(&pool,
							DLOG_ALLOC_DEFAULT_ALIGNMENT,
							mempool_memsize(elem_sz_b, num_slots))) {
		mempool* p = mempool_create_from_buffer(pool, elem_sz_b, num_slots);
		p->owns_buffer = 1;
		return p;
	} else {
		return NULL;
	}
}

mempool*
mempool_create_from_buffer(void* buf, size_t elem_sz_b, int num_slots)
{
	/* assumes buffer is properly allocated and aligned */
	mempool* pool = (mempool* )buf;
	pool->num_slots = num_slots;
	pool->slot_sz = ALIGN_TO(elem_sz_b, DLOG_ALLOC_DEFAULT_ALIGNMENT);
	pool->free_list_head = 0;
	pool->owns_buffer = 0;

	struct slot_hdr* s = _slot(pool, 0);
	s->free_next = 1;
	s->num_slots = 1;

	s = _slot(pool, 1);
	s->free_next = -1;
	s->num_slots = num_slots - 1;

	return pool;
}

void
mempool_destroy(mempool* m)
{
	if (m->owns_buffer)
		free(m);
}

void*
mempool_alloc(mempool* pool)
{
	if (pool->free_list_head == -1)
		return NULL;

	struct slot_hdr* s = _slot(pool, pool->free_list_head);
	if (s->num_slots == 1) {
		pool->free_list_head = s->free_next;
		return s;
	} else if (s->num_slots > 1) {
		struct slot_hdr* nextslot = _slot(pool, pool->free_list_head+1);
		nextslot->free_next = -1;
		nextslot->num_slots = s->num_slots-1;
		if (pool->free_list_head + 1 == pool->num_slots)
			pool->free_list_head = -1;
		else
			pool->free_list_head++;
		return s;
	} else {
		return NULL;
	}
}

void
mempool_free(mempool* pool, void* ptr)
{
	int slotidx = _idx(pool, ptr);
	struct slot_hdr* s = (struct slot_hdr* )ptr;
	s->free_next = pool->free_list_head;
	s->num_slots = 1;
	pool->free_list_head = slotidx;
}


