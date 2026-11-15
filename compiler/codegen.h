#ifndef FORGE_CODEGEN_H
#define FORGE_CODEGEN_H

#include "typed_ast.h"
#include <stdio.h>

void codegen_emit(Program *prog, const ForgeTypedProgram *typed, FILE *out, const char *runtime_include);
void codegen_emit_js(Program *prog, const ForgeTypedProgram *typed, FILE *out);
void codegen_emit_library(Program *prog, const ForgeTypedProgram *typed, FILE *out_c, FILE *out_h, const char *runtime_include);

#endif
