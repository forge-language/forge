#ifndef FORGE_NATIVE_H
#define FORGE_NATIVE_H

#include "ir.h"
#include "target.h"
#include <stdbool.h>

bool forge_native_emit(const IRModule *module, const ForgeTarget *target,
                       const char *output_path);

#endif
