#ifndef FORGE_SAFETY_H
#define FORGE_SAFETY_H
#include "typed_ast.h"

/* Additive checked mode. Unspecified legacy/FFI contracts are rejected rather
 * than silently classified as checked handles or transferable loans. */
void forge_check_safety(Program *, const ForgeTypedProgram *);
#endif
