#ifndef FORGE_CONST_INDEX_H
#define FORGE_CONST_INDEX_H

#include "ast.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ConstDecl *constant;
    size_t ordinal;
} ForgeConstEntry;

/* Indexes names only, never context-dependent expression types or values.
 * Borrows an immutable owner array and its names for a single compiler pass.
 * Sort an auxiliary array, never the AST: declaration order affects resolution,
 * diagnostics and emission. Small owners stay linear; others spend a bounded
 * probe budget before sorting. Each index belongs to one caller/pass, not to
 * concurrent readers or a global cache. Unqueried owners never allocate. */
typedef struct {
    ConstDecl *constants;
    ForgeConstEntry *entries;
    size_t count;
    size_t probes_left;
} ForgeConstIndex;

static inline int forge_const_name_compare(ForgeStr a, ForgeStr b) {
    size_t length = a.len < b.len ? a.len : b.len;
    int order = length ? strncmp(a.data, b.data, length) : 0;
    return order ? order : (a.len > b.len) - (a.len < b.len);
}

static inline int forge_const_entry_compare(const void *a, const void *b) {
    const ForgeConstEntry *left = a, *right = b;
    int order = forge_const_name_compare(left->constant->name, right->constant->name);
    return order ? order : (left->ordinal > right->ordinal) - (left->ordinal < right->ordinal);
}

static inline ForgeConstIndex forge_const_index_build(ConstDecl *constants, size_t count) {
    return (ForgeConstIndex){.constants = constants, .count = count,
        .probes_left = count > SIZE_MAX / 4 ? SIZE_MAX : count * 4};
}

static inline ConstDecl *forge_const_index_get(ForgeConstIndex *index, ForgeStr name) {
    if (index->count <= 16) {
        for (size_t i = 0; i < index->count; i++)
            if (forge_str_eq(index->constants[i].name, name)) return &index->constants[i];
        return NULL;
    }
    if (!index->entries) {
        /* Charge comparisons, not calls: repeated early hits can be cheaper
         * than sorting even for a large owner. At most 4*count linear probes
         * precede binary lookup. An unfinished search falls through to it. */
        size_t i = 0;
        for (; i < index->count && index->probes_left; i++) {
            index->probes_left--;
            if (forge_str_eq(index->constants[i].name, name)) return &index->constants[i];
        }
        if (i == index->count) return NULL;
        /* The shared allocator checks the byte count and aborts on failure. */
        index->entries = forge_calloc_array(index->count, sizeof(*index->entries));
        for (i = 0; i < index->count; i++)
            index->entries[i] = (ForgeConstEntry){&index->constants[i], i};
        qsort(index->entries, index->count, sizeof(*index->entries), forge_const_entry_compare);
    }
    size_t low = 0, high = index->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = forge_const_name_compare(index->entries[middle].constant->name, name);
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    /* Lower bound retains the first declaration even for duplicate names. */
    return low < index->count && forge_str_eq(index->entries[low].constant->name, name)
        ? index->entries[low].constant : NULL;
}

static inline void forge_const_index_free(ForgeConstIndex *index) {
    free(index->entries);
    *index = (ForgeConstIndex){0};
}

#endif
