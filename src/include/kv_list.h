#ifndef PG_CLICKHOUSE_KV_LIST_H
#define PG_CLICKHOUSE_KV_LIST_H

#include <stdbool.h>

#include "postgres.h"

#include "nodes/pathnodes.h"

/*
 * A simple data structure with a list of key/value string pairs. Use
 * new_kv_list_from_list() to create.
 */
typedef struct kv_list {
    int length;
    /* key/value char * pairs follow the length field. */
    char data[];
} kv_list;

/*
 * Iterator for a kv_list. Use new_kv_iter() to create; name and value are
 * valid only after kv_iter_next() returns true.
 *
 *     kv_iter iter = new_kv_iter(kv);
 *     while (kv_iter_next(&iter))
 *     {
 *         printf("%s => %s\n", iter.name, iter.value);
 *     }
 */
typedef struct kv_iter {
    int togo;
    /* Next unread name; one past end after last pair, never dereferenced. */
    char* next;
    char* name;
    char* value;
} kv_iter;

/*
 * Defines the allocator to use when creating a new kv_list.
 * kv_pair_guc_malloc is the same as kv_pair_malloc on Postgres 15 and
 * earlier, so be sure to free() the memory on those versions.
 */
enum kv_pair_alloc {
    kv_pair_guc_malloc,
    kv_pair_malloc,
    kv_pair_palloc,
};

/*
 * Create a new kv_list from a PostgreSQL List of DefElem. Allocate the memory
 * using the specified allocator.
 */
kv_list*
new_kv_list_from_pg_list(List* list, int allocate);

/* Create a new kv_iter for a key_pairs. */
kv_iter
new_kv_iter(const kv_list* ns);

/* Advance to the next item. Returns false when no items remain. */
bool
kv_iter_next(kv_iter* state);

#endif /* PG_CLICKHOUSE_KV_LIST_H */
