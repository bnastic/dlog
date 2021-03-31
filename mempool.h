#ifndef DLOG_MEM_POOL_H__
#define DLOG_MEM_POOL_H__

typedef struct mempool mempool;

size_t mempool_memsize(size_t elem_sz_b, int num_slots);
mempool* mempool_create(size_t elem_sz_b, int num_slots);
mempool* mempool_create_from_buffer(void* buf, size_t elem_sz_b, int num_slots);
void mempool_destroy(mempool* m);

void* mempool_alloc(mempool* pool);
void mempool_free(mempool* pool, void* ptr);

int mempool_get_numslots(mempool*);
int mempool_get_slotsz(mempool*);
bool mempool_has_ptr(mempool* , void* );

#endif

