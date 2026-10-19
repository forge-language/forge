#ifndef FORGE_SEMANTIC_H
#define FORGE_SEMANTIC_H
#include "typed_ast.h"
/* Checks visible declarations before optimization can erase invalid expressions.
 * Catalogued stdlib signatures are checked here; opaque binary library ABI
 * compatibility remains the host compiler's responsibility. */
void forge_check_program(Program *program);
/* Requires a successfully checked, optimized Program. The returned graph owns
 * its arrays and borrows that Program until forge_typed_free(). */
ForgeTypedProgram forge_type_program(Program *program);
#endif
