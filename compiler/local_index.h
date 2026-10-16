#ifndef FORGE_LOCAL_INDEX_H
#define FORGE_LOCAL_INDEX_H

#include "common.h"
#include <stddef.h>

typedef struct { ForgeStr name; size_t next; } ForgeLocalEntry;

/* A private index for a binding stack. Store indices, never pointers into its
 * reallocating owner. Names are borrowed while active; types and flow/ownership
 * state always remain in the owner. Push/pop hooks mirror every stack change. */
typedef struct {
    ForgeLocalEntry *entries;
    size_t *heads;
    size_t capacity, buckets, count, probes;
    bool active;
} ForgeLocalIndex;

static inline uint64_t forge_local_hash(ForgeStr name) {
    uint64_t hash = UINT64_C(14695981039346656037);
    /* Match forge_str_eq's length + strncmp equality even for embedded NULs. */
    for (size_t i = 0; i < name.len && name.data[i]; i++) {
        hash ^= (unsigned char)name.data[i];
        hash *= UINT64_C(1099511628211);
    }
    return (hash ^ (uint64_t)name.len) * UINT64_C(1099511628211);
}

static inline size_t forge_local_capacity(ForgeLocalIndex *index, size_t count) {
    size_t maximum = SIZE_MAX / sizeof(*index->entries);
    size_t head_maximum = SIZE_MAX / sizeof(*index->heads) / 2;
    if (maximum > head_maximum) maximum = head_maximum;
    size_t capacity = 32;
    if (capacity > maximum) forge_die("too many indexed locals");
    while (capacity < count) {
        if (capacity > maximum / 2) forge_die("too many indexed locals");
        capacity *= 2;
    }
    if (capacity > index->capacity) {
        void *entries = realloc(index->entries, capacity * sizeof(*index->entries));
        if (!entries) forge_die("out of memory");
        index->entries = entries;
        void *heads = realloc(index->heads, capacity * 2 * sizeof(*index->heads));
        if (!heads) forge_die("out of memory");
        index->heads = heads;
        index->capacity = capacity;
    }
    return capacity;
}

static inline void forge_local_rehash(ForgeLocalIndex *index, size_t buckets) {
    index->buckets = buckets;
    memset(index->heads, 0, buckets * sizeof(*index->heads));
    for (size_t i = 0; i < index->count; i++) {
        size_t bucket = (size_t)forge_local_hash(index->entries[i].name) & (buckets - 1);
        index->entries[i].next = index->heads[bucket];
        index->heads[bucket] = i + 1;
    }
}

static inline void forge_local_push(ForgeLocalIndex *index, ForgeStr name) {
    if (index->count == SIZE_MAX) forge_die("too many indexed locals");
    if (index->active) {
        if (index->count == index->buckets / 2) {
            size_t capacity = forge_local_capacity(index, index->count + 1);
            forge_local_rehash(index, capacity * 2);
        }
        size_t bucket = (size_t)forge_local_hash(name) & (index->buckets - 1);
        index->entries[index->count] = (ForgeLocalEntry){name, index->heads[bucket]};
        index->heads[bucket] = index->count + 1;
    }
    index->count++;
}

static inline void forge_local_truncate(ForgeLocalIndex *index, size_t count) {
    if (count > index->count) forge_die("local index scope mismatch");
    if (index->active) {
        while (index->count > count) {
            size_t i = --index->count;
            size_t bucket = (size_t)forge_local_hash(index->entries[i].name) & (index->buckets - 1);
            if (index->heads[bucket] != i + 1) forge_die("local index scope mismatch");
            index->heads[bucket] = index->entries[i].next;
        }
    } else index->count = count;
    if (!count) {
        index->active = false;
        index->probes = 0;
    }
}

/* Bindings are a contiguous array with ForgeStr as the first member. memcpy
 * avoids alignment/aliasing assumptions; the owner's actual stride is supplied.
 * A miss is SIZE_MAX, so binding zero is never confused with a missing name. */
static inline size_t forge_local_find(ForgeLocalIndex *index, const void *bindings,
                                      size_t count, size_t stride, ForgeStr name) {
    if (count != index->count) forge_die("local index scope mismatch");
    if (!count) return SIZE_MAX;
    if (stride < sizeof(ForgeStr) || count > SIZE_MAX / stride)
        forge_die("invalid indexed local storage");
    if (!index->active && count > 32 && index->probes / 4 >= count) {
        size_t capacity = forge_local_capacity(index, count);
        for (size_t i = 0; i < count; i++)
            memcpy(&index->entries[i].name, (const char *)bindings + i * stride, sizeof(ForgeStr));
        forge_local_rehash(index, capacity * 2);
        index->active = true;
    }
    if (index->active) {
        size_t bucket = (size_t)forge_local_hash(name) & (index->buckets - 1);
        /* Links strictly decrease: ascending insertion gives latest-name wins
         * and bounds even a deliberately colliding chain by active count. */
        for (size_t slot = index->heads[bucket]; slot; slot = index->entries[slot - 1].next)
            if (forge_str_eq(index->entries[slot - 1].name, name)) return slot - 1;
        return SIZE_MAX;
    }
    size_t found = SIZE_MAX, probes = 0;
    for (size_t i = count; i; i--) {
        ForgeStr candidate;
        memcpy(&candidate, (const char *)bindings + (i - 1) * stride, sizeof(candidate));
        probes++;
        if (forge_str_eq(candidate, name)) { found = i - 1; break; }
    }
    if (count > 32)
        index->probes = probes > SIZE_MAX - index->probes ? SIZE_MAX : index->probes + probes;
    return found;
}

static inline void forge_local_free(ForgeLocalIndex *index) {
    free(index->entries);
    free(index->heads);
    *index = (ForgeLocalIndex){0};
}

#endif
