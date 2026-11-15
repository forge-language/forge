#ifndef FORGE_CLEANUP_H
#define FORGE_CLEANUP_H
#include "typed_lookup.h"

/* Only resolved SDK factories with an explicit release contract may initialize
 * a scoped owner. Legacy scalar handles and arena strings keep their ABI. */
const ForgeResourceContract *forge_cleanup_factory(const ForgeTypedIndex *, const Expr *);
void forge_check_cleanup(Program *, const ForgeTypedProgram *);
#endif
