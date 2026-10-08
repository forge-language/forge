#ifndef FORGE_SEMANTIC_H
#define FORGE_SEMANTIC_H
#include "ast.h"
/* Checks visible declarations before optimization can erase invalid expressions.
 * Binary library and stdlib signatures remain the host compiler's responsibility. */
void forge_check_program(Program *program);
#endif
