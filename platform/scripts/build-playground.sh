#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FORGE_SOURCE_ROOT=${FORGE_SOURCE_ROOT:-$(CDPATH= cd -- "$ROOT/.." && pwd)}
cd "$ROOT"
mkdir -p frontend/public/playground
emcc playground/main.c "$FORGE_SOURCE_ROOT/compiler/ast.c" \
 "$FORGE_SOURCE_ROOT/compiler/lexer.c" "$FORGE_SOURCE_ROOT/compiler/parser.c" \
 "$FORGE_SOURCE_ROOT/compiler/optimize.c" "$FORGE_SOURCE_ROOT/compiler/mod_registry.c" \
 "$FORGE_SOURCE_ROOT/compiler/module_loader.c" "$FORGE_SOURCE_ROOT/compiler/codegen_js.c" \
 "$FORGE_SOURCE_ROOT/runtime/platform.c" \
 -I "$FORGE_SOURCE_ROOT/compiler" -I "$FORGE_SOURCE_ROOT/include" -std=c2x -O2 \
 -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=worker \
 -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 \
 -sINITIAL_MEMORY=16777216 -sMAXIMUM_MEMORY=67108864 \
 -sEXPORTED_RUNTIME_METHODS=FS,callMain \
 -o frontend/public/playground/compiler.mjs
