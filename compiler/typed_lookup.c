#include "typed_lookup.h"

static int compare_entry(const void *left, const void *right) {
    const ForgeTypedEntry *a = left, *b = right;
    uintptr_t x = (uintptr_t)a->origin, y = (uintptr_t)b->origin;
    if (x != y) return x < y ? -1 : 1;
    return a->node < b->node ? -1 : a->node > b->node;
}
ForgeTypedIndex forge_typed_index(const ForgeTypedProgram *p) {
    ForgeTypedIndex index = {.program = p};
    if (!p) forge_die("missing typed program for code generation");
    if (!p->expression_count) return index;
    index.entries = forge_calloc_array(p->expression_count, sizeof(*index.entries));
    for (size_t i = 0; i < p->expression_count; ++i)
        index.entries[i] = (ForgeTypedEntry){p->expressions[i].origin, i};
    qsort(index.entries, p->expression_count, sizeof(*index.entries), compare_entry);
    return index;
}
size_t forge_typed_find(const ForgeTypedIndex *index, const Expr *origin, size_t context) {
    if (!origin) return FORGE_TYPED_NONE;
    const ForgeTypedProgram *p = index->program;
    size_t low = 0, high = p->expression_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if ((uintptr_t)index->entries[mid].origin < (uintptr_t)origin) low = mid + 1;
        else high = mid;
    }
    size_t unique = FORGE_TYPED_NONE, matches = 0;
    for (size_t i = low; i < p->expression_count && index->entries[i].origin == origin; ++i) {
        size_t node = index->entries[i].node;
        const ForgeTypedExpr *e = &p->expressions[node];
        if (node == context || e->parent == context) return node;
        if (context != FORGE_TYPED_NONE) {
            size_t ancestor = e->parent;
            while (ancestor != FORGE_TYPED_NONE) {
                if (ancestor == context) return node;
                ancestor = p->expressions[ancestor].parent;
            }
        }
        unique = node;
        ++matches;
    }
    /* Proof-based native emission can start at a surviving subexpression.
     * Only a unique interpretation is safe without its containing expression. */
    return matches == 1 ? unique : FORGE_TYPED_NONE;
}
void forge_typed_index_free(ForgeTypedIndex *index) {
    free(index->entries);
    *index = (ForgeTypedIndex){0};
}
