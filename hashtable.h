#ifndef _HASH_TABLE_H__
#define _HASH_TABLE_H__
#include <stdint.h>

/* small hash table implementation
 * keys -> intptr_t (pointer or integer, based on HT_KEY_TYPE)
 * values -> void* (opaque). Ownership taken if deleter is provided
 *
 * We keep the first element of the bucket _inside_ the
 * bucket, instead of allocating it on the heap. That way
 * we improve locality for HTs with no collisions.
 */

typedef struct hashtable hashtable;

typedef enum
{
	HT_INT,
	HT_DYNSTR,
	HT_CSTR
} HT_KEY_TYPE;

/*
 * value deleter
 */
typedef void (*ht_value_deleter)(void* );

/*
 * default do-nothing deleter
 */
void ht_value_deleter_null(void*);

/*
 * API
 */
hashtable*	ht_create(HT_KEY_TYPE type, int bkts, ht_value_deleter /* or NULL */);
void		ht_destroy(hashtable* );
void		ht_upsert(hashtable* , intptr_t key, void* value);
void*		ht_find(hashtable* , intptr_t key);
void		ht_remove(hashtable* , intptr_t key);
void		ht_visit(hashtable*, int (*visitor)(intptr_t, void*, void*), void* );

#endif

