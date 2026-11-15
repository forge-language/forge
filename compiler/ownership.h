#ifndef FORGE_OWNERSHIP_H
#define FORGE_OWNERSHIP_H
#include "common.h"

typedef enum {
    FORGE_OWN_UNINITIALIZED, FORGE_OWN_LIVE, FORGE_OWN_MOVED
} ForgeOwnershipState;
typedef struct {
    ForgeOwnershipState state;
    bool owned;
} ForgeOwnershipBinding;
typedef struct {
    ForgeOwnershipBinding *bindings;
    size_t count, capacity;
} ForgeOwnership;
enum { FLOW_NEXT=1, FLOW_RETURN=2, FLOW_BREAK=4, FLOW_CONTINUE=8 };
typedef struct {
    bool live, reachable;
} ForgeOwnershipJoin;

/* IDs are append-only for one semantic pass. Lexical stacks may truncate and
 * grow without reusing a consumed binding's identity. LIVE describes a binding,
 * not allocator provenance or exclusive ownership of its string bytes. */
size_t forge_ownership_bind(ForgeOwnership *, bool initialized);
bool forge_ownership_live(const ForgeOwnership *, size_t binding);
bool forge_ownership_owned(const ForgeOwnership *, size_t binding);
void forge_ownership_qualify(ForgeOwnership *, size_t binding, bool owned);
void forge_ownership_set_live(ForgeOwnership *, size_t binding, bool live);
/* Return exits leave this scope. Break/continue exits must retain consumed
 * owned bindings until the enclosing loop join, even before its back edge. */
void forge_ownership_join_add(ForgeOwnershipJoin *, ForgeOwnershipBinding,
                              bool live, unsigned exits);
void forge_ownership_join_finish(ForgeOwnership *, size_t binding,
                                 ForgeOwnershipJoin, bool before);
void forge_ownership_loop_join(ForgeOwnership *, size_t binding, bool before,
                               bool after);
void forge_ownership_free(ForgeOwnership *);
#endif
