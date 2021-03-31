#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "arena.h"
#include "mempool.h"

struct pool_hdr
{
	mempool* this;
	mempool* next;
};

struct arena
{
	bool allow_heap;
	int num_pools;
	struct pool_hdr pools[] __attribute__((aligned(16)));
};

static int _poolidx_from_size(arena* ar, int sz);
static int _poolidx_from_ptr(arena* ar, void* ptr);

/* sizes[][0] - slot size, sizes[][1] - num slots */
arena*
arena_create(int sizes[][2], int nbpools, alloc_arena_fn allocator, bool allow_heap_alloc)
{
	size_t pool_size = 0;
	size_t header_size = sizeof(arena) + nbpools * sizeof(struct pool_hdr);

	for (int i=0; i<nbpools; i++) {
		pool_size += mempool_memsize(sizes[i][0], sizes[i][1]);
	}

	arena* ar = NULL;
	if (allocator)
		ar = allocator(header_size + pool_size);
	else
		ar = malloc(header_size + pool_size);

	ar->num_pools = nbpools;

	mempool* pooladdr = (mempool*)((char *)ar + header_size);

	for (int i=0; i<nbpools; i++) {
		(void)mempool_create_from_buffer(pooladdr, sizes[i][0], sizes[i][1]);
		mempool* next = (mempool *)((char *)pooladdr + mempool_memsize(sizes[i][0], sizes[i][1]));
		ar->pools[i].this = pooladdr;
		ar->pools[i].next = next;
		pooladdr = next;
	}
	ar->pools[ar->num_pools-1].next = NULL;
	ar->allow_heap = allow_heap_alloc;
	return ar;
}

/* done */
void
arena_destroy(arena* ar, dealloc_arena_fn fn)
{
	/* ignore memorypools, just dump memory */
	if (fn)
		fn(ar);
	else
		free(ar);
}

/* done */
void*
arena_alloc(arena* ar, int sz, int* capacity)
{
	int pidx = _poolidx_from_size(ar, sz);
	if (-1 != pidx) {
		do {
			mempool* pool = ar->pools[pidx].this;
			void* p = mempool_alloc(pool);
			if (p) {
				if (capacity)
					*capacity = mempool_get_slotsz(pool);

				return p;
			}
		} while(++pidx < ar->num_pools);
		/* no pool could allocate this! fall-through to heap */
	}
	if (ar->allow_heap) {
		LOG_DEBUG("arena ran out of space allocating %d bytes - going to heap", sz);

		if (capacity)
			*capacity = sz;

		return calloc(sz, 1);
	} else
		return NULL;
}

void*
arena_realloc(arena* ar, void* p, int newlen, int* newcap)
{
	int poolidx = _poolidx_from_ptr(ar, p);

	if(-1 == poolidx) {

		/* stay on the heap */
		*newcap = newlen;
		return realloc(p, newlen);

	} else {

		int newidx = _poolidx_from_size(ar, newlen);

		if (newidx == poolidx) {
			/* fits in the same block */
			*newcap =  mempool_get_slotsz(ar->pools[poolidx].this);
			return p;
		}

		void* newp = arena_alloc(ar, newlen, newcap);

		if (!newp) {
			return NULL;
		} else {
			int newidx = _poolidx_from_ptr(ar, newp);

			if (newidx == -1) {
				/* going to the heap */
				*newcap = newlen;
				memcpy(newp, p, dlog_min(mempool_get_slotsz(ar->pools[poolidx].this),
										newlen));
			} else if (newidx > poolidx) {
				memcpy(newp, p, mempool_get_slotsz(ar->pools[poolidx].this));
			} else {
				memcpy(newp, p, mempool_get_slotsz(ar->pools[newidx].this));
			}

			arena_free(ar, p);
			return newp;
		}
	}

	return NULL;
}

void
arena_free(arena* ar, void* p)
{
	int poolidx = _poolidx_from_ptr(ar, p);
	if (-1 == poolidx && ar->allow_heap) {
		free(p);
	} else {
		mempool_free(ar->pools[poolidx].this, p);
	}
}

static int
_poolidx_from_size(arena* ar, int sz)
{
	/* return -1 if heap allocation is needed */
	for (int i=0; i<ar->num_pools; i++) {
		mempool* pool = ar->pools[i].this;
		if (mempool_get_slotsz(pool) >= sz)
			return i;
	}
	return -1;
}

static int
_poolidx_from_ptr(arena* ar, void* ptr)
{
	for (int i=0; i<ar->num_pools; i++) {
		mempool* pool = ar->pools[i].this;
		if (mempool_has_ptr(pool, ptr)) {
			return i;
		}
	}
	return -1;
}
