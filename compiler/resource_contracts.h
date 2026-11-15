#ifndef FORGE_RESOURCE_CONTRACTS_H
#define FORGE_RESOURCE_CONTRACTS_H
#include "mod_registry.h"

typedef enum {
    FORGE_RESOURCE_SCALAR, FORGE_RESOURCE_STRING,
    FORGE_RESOURCE_VIEW, FORGE_RESOURCE_BUILDER, FORGE_RESOURCE_OPAQUE
} ForgeResourceKind;
typedef enum {
    FORGE_REGION_NONE, FORGE_REGION_ARENA, FORGE_REGION_ARGUMENT
} ForgeResourceRegion;
typedef struct {
    const char *c_name;
    ForgeResourceKind result;
    ForgeResourceKind arguments[7];
    ForgeResourceRegion region;
    unsigned borrow_arguments, mutate_arguments, consume_arguments, release_arguments;
    bool allocate, reset_arena, must_use, nullable;
    bool transfer_safe, share_safe;
    const char *allocator, *release;
    const char *opaque_reason;
    bool scope_cleanup;
} ForgeResourceContract;

/* A scalar ABI signature is not a safety contract. Every unlisted SDK call
 * and unknown binary boundary has an explicit opaque reason. */
const ForgeResourceContract *forge_resource_contract(const ForgeStdFn *);
#endif
