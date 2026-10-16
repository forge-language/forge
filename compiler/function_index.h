#ifndef FORGE_FUNCTION_INDEX_H
#define FORGE_FUNCTION_INDEX_H

#include "ast.h"

typedef struct {
    FnDecl *function;
    size_t ordinal;
} ForgeFunctionEntry;

/* Borrows an immutable owner array and its names for a single compiler pass.
 * Sort an auxiliary array, never the AST: declaration order affects resolution,
 * diagnostics and emission. Small owners stay linear; others spend a bounded
 * probe budget before sorting. Each index belongs to one caller/pass, not to
 * concurrent readers or a global cache. Unqueried owners never allocate. */
typedef struct {
    FnDecl *functions;
    ForgeFunctionEntry *entries;
    size_t count;
    size_t probes_left;
} ForgeFunctionIndex;

static inline int forge_function_name_compare(ForgeStr a, ForgeStr b) {
    size_t length = a.len < b.len ? a.len : b.len;
    int order = length ? strncmp(a.data, b.data, length) : 0;
    return order ? order : (a.len > b.len) - (a.len < b.len);
}

static inline int forge_function_entry_compare(const void *a, const void *b) {
    const ForgeFunctionEntry *left = a, *right = b;
    int order = forge_function_name_compare(left->function->name, right->function->name);
    return order ? order : (left->ordinal > right->ordinal) - (left->ordinal < right->ordinal);
}

static inline ForgeFunctionIndex forge_function_index_build(FnDecl *functions, size_t count) {
    return (ForgeFunctionIndex){.functions = functions, .count = count,
        .probes_left = count > SIZE_MAX / 4 ? SIZE_MAX : count * 4};
}

static inline FnDecl *forge_function_index_get(ForgeFunctionIndex *index, ForgeStr name) {
    if (index->count <= 16) {
        for (size_t i = 0; i < index->count; i++)
            if (forge_str_eq(index->functions[i].name, name)) return &index->functions[i];
        return NULL;
    }
    if (!index->entries) {
        /* Charge comparisons, not calls: repeated early hits can be cheaper
         * than sorting even for a large owner. At most 4*count linear probes
         * precede binary lookup. An unfinished search falls through to it. */
        size_t i = 0;
        for (; i < index->count && index->probes_left; i++) {
            index->probes_left--;
            if (forge_str_eq(index->functions[i].name, name)) return &index->functions[i];
        }
        if (i == index->count) return NULL;
        index->entries = forge_calloc_array(index->count, sizeof(*index->entries));
        for (i = 0; i < index->count; i++)
            index->entries[i] = (ForgeFunctionEntry){&index->functions[i], i};
        qsort(index->entries, index->count, sizeof(*index->entries), forge_function_entry_compare);
    }
    size_t low = 0, high = index->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = forge_function_name_compare(index->entries[middle].function->name, name);
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    /* Lower bound retains the first declaration even for duplicate names. */
    return low < index->count && forge_str_eq(index->entries[low].function->name, name)
        ? index->entries[low].function : NULL;
}

static inline void forge_function_index_free(ForgeFunctionIndex *index) {
    free(index->entries);
    *index = (ForgeFunctionIndex){0};
}

#endif
