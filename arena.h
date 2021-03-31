#ifndef _ARENA_H__
#define _ARENA_H__
#include "def.h"

typedef struct arena arena;

typedef void* (*alloc_arena_fn)(int totalsz);
typedef void (*dealloc_arena_fn)(arena*);

arena*	arena_create(int sizes[][2], int num_buckets, alloc_arena_fn /* or NULL */, bool allow_heap_alloc);
void	arena_destroy(arena*, dealloc_arena_fn /* or NULL */);
void*	arena_alloc(arena*, int sz, int *capacity);
void*	arena_realloc(arena*, void* ptr, int newsz, int* new_capacity);
void	arena_free(arena*, void* p);

#endif

