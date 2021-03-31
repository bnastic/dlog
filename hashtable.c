#include "def.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hashtable.h"
#include "dynstr.h"

/*
 * FNV hash function
 */
static const uint64_t PRIME = 0x100000001b3;
static const uint64_t SEED  = 0xcbf29ce484222325;
static uint64_t fnv1a(const char* text)
{
	uint64_t hash = SEED;
	while (*text)
		hash = (((unsigned char)*text++) ^ hash) * PRIME;

	return hash;
}

/*
 * hash element
 */
typedef struct ht_element_t
{
	intptr_t key;
	void* value;
	struct ht_element_t* next;
} ht_element;


/*
 * hash table
 */
struct hashtable
{
	HT_KEY_TYPE type;
	int num_buckets;
	ht_value_deleter fn_del;
	ht_element* head;
};

/* aux functions */
static void
_default_del(void* value)
{
	free(value);
}

static uint64_t
_hash(HT_KEY_TYPE type, intptr_t key)
{
	/* we rely on intrptr_t <-> int64_t */
	if (type == HT_DYNSTR)
		return fnv1a(dynstr_ptr((dynstr *)key));
	else if (type == HT_CSTR)
		return fnv1a((const char*)key);
	else
		return key;
}

static int
_cmp(HT_KEY_TYPE type, intptr_t key1, intptr_t key2)
{
	if (type == HT_DYNSTR) {
		dynstr* s1 = (dynstr* )key1;
		dynstr* s2 = (dynstr* )key2;
		return strcmp(dynstr_ptr(s1), dynstr_ptr(s2));
	} else if (type == HT_CSTR) {
		return strcmp((const char* )key1, (const char *)key2);
	} else if (type == HT_INT) {
		return key1 - key2;
	}
	else return 0;
}

static void
_free_key(HT_KEY_TYPE type, intptr_t key)
{
	/* we always have ownership of the key for char-based objects */
	if (type == HT_DYNSTR)
		dynstr_free((dynstr *)key);
	else if (type == HT_CSTR)
		free((char *)key);
}

static void
_free_value(hashtable* tbl, void* val)
{
	/* if we have no deleter, it means we don't
	   have ownership, so don't delete it */
	if (tbl->fn_del)
		tbl->fn_del(val);
}

static intptr_t
_copy_key(HT_KEY_TYPE type, intptr_t key)
{
	if (type == HT_DYNSTR)
		return (intptr_t)dynstr_copy((void *)key);
	else if (type == HT_CSTR)
		return (intptr_t)strdup((const char *)key);
	else
		return key;
}

hashtable*
ht_create(HT_KEY_TYPE type, int num_buckets, ht_value_deleter deleter)
{
	hashtable* tbl = calloc(1, sizeof(hashtable));
	tbl->type = type;
	tbl->fn_del = deleter ? deleter : _default_del;
	tbl->num_buckets = num_buckets;
	tbl->head = calloc(num_buckets, sizeof(ht_element));
	return tbl;
}

void
ht_destroy(hashtable* tbl)
{
	ht_element* head;

	for (int i=0; i<tbl->num_buckets; i++) {
		head = tbl->head + i;
		_free_key(tbl->type, head->key);
		_free_value(tbl, head->value);
		head = head->next;
		while(head) {
			_free_key(tbl->type, head->key);
			_free_value(tbl, head->value);
			ht_element* n = head->next;
			free(head);
			head=n;
		}
	}
	// hash table doesn't own its elements!
	free(tbl->head);
	free(tbl);
}

/* insert or update */
void
ht_upsert(hashtable* tbl, intptr_t key, void* value)
{
	int bucket = _hash(tbl->type, key) % tbl->num_buckets;
	ht_element* head = tbl->head + bucket;
	if(!head->value) {
		head->key = _copy_key(tbl->type, key);
		head->value = value;
	} else {
		ht_element* el = head;
		while (el) {
			if (!_cmp(tbl->type, key, el->key)) {
				/* delete old value, if the deleter is provided */
				_free_value(tbl, el->value);
				el->value = value;
				break;
			} else if (!el->next) {
				ht_element* newel = calloc(1, sizeof(ht_element));
				newel->key = _copy_key(tbl->type, key);
				newel->value = value;
				el->next = newel;
				newel->next = NULL;
				break;
			}
			el = el->next;
		}
	}
}

void*
ht_find(hashtable* tbl, intptr_t key)
{
	int bucket = _hash(tbl->type, key) % tbl->num_buckets;
	ht_element* el = tbl->head + bucket;
	while(el) {
		if (el->key && !_cmp(tbl->type, key, el->key)) {
			return (el->value);
		}
		el = el->next;
	}
	return NULL;
}

void
ht_remove(hashtable* tbl, intptr_t key)
{
	int bucket = _hash(tbl->type, key) % tbl->num_buckets;
	ht_element* el = tbl->head + bucket;
	if (el) {
		if (el->key && !_cmp(tbl->type, key, el->key)) {
			_free_key(tbl->type, key);
			_free_value(tbl, el->value);
			el->key = (intptr_t)0;
			el->value = NULL;
			if (el->next) {
				ht_element* n = el->next;
				memcpy(el, n, sizeof(ht_element));
				free(n);
			}
		} else {
			ht_element** pp = &(el->next);
			ht_element* en = el->next;
			while(en) {
				if (!_cmp(tbl->type, key, en->key)) {
					*pp = en->next;
					_free_key(tbl->type, key);
					_free_value(tbl, el->value);
					free(en);
				}
				pp = &en->next;
				en = en->next;
			}
		}
	}
}

void
ht_visit(hashtable* ht, int (*visitor) (intptr_t key, void* value, void* userdata), void* udata )
{
	ht_element* el;

	for (int i=0; i<ht->num_buckets; i++) {
		el = ht->head + i;
		while (el && el->value) {
			ht_element* tmpe = el->next;
			if (-1 == visitor(el->key, el->value, udata)) {
				goto done;
			}
			el = tmpe;
		}
	}
done:
	return;
}

void ht_value_deleter_null(void* v)
{
}

