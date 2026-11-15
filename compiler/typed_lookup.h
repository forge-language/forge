#ifndef FORGE_TYPED_LOOKUP_H
#define FORGE_TYPED_LOOKUP_H

#include "typed_ast.h"

typedef struct {
    const Expr *origin;
    size_t node;
} ForgeTypedEntry;
typedef struct {
    const ForgeTypedProgram *program;
    ForgeTypedEntry *entries;
} ForgeTypedIndex;

/* Pass-owned occurrence index. A shared constant origin is resolved within
 * its interpretation parent, never cached as one type for all callers. */
ForgeTypedIndex forge_typed_index(const ForgeTypedProgram *);
size_t forge_typed_find(const ForgeTypedIndex *, const Expr *, size_t context);
void forge_typed_index_free(ForgeTypedIndex *);

#endif
