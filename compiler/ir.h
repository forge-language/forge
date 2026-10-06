#ifndef FORGE_IR_H
#define FORGE_IR_H

#include "ast.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

typedef uint32_t IRValue;
#define IR_NO_VALUE UINT32_MAX

typedef enum {
    IR_CONST_INT, IR_CONST_FLOAT, IR_CONST_BOOL, IR_CONST_STRING,
    IR_LOAD_LOCAL, IR_LOAD_GLOBAL, IR_LOAD_SYMBOL, IR_PARAM,
    IR_BINARY, IR_PHI, IR_CALL, IR_QUAL_CALL, IR_RECV, IR_INDEX, IR_FIELD, IR_MOVE,
    IR_STORE_LOCAL, IR_SPAWN, IR_SEND, IR_YIELD, IR_AWAIT, IR_EVAL
} IROp;

typedef enum { IR_TERM_NONE, IR_TERM_JUMP, IR_TERM_BRANCH, IR_TERM_RETURN } IRTermKind;

typedef struct {
    IROp op;
    IRValue result;
    ForgeType type;
    uint32_t local;
    IRValue a, b;
    BinOp bin_op;
    ForgeStr name, module;
    IRValue *args;
    size_t arg_count;
    int64_t int_value;
    double float_value;
    bool bool_value;
    bool type_known;
} IRInst;

typedef struct {
    uint32_t id;
    IRInst *insts;
    size_t inst_count, inst_cap;
    IRTermKind term;
    uint32_t target, target_false;
    IRValue value;
    bool has_value;
} IRBlock;

typedef struct {
    ForgeStr name;
    ForgeType type;
} IRLocal;

typedef enum { IR_GLOBAL_INT, IR_GLOBAL_FLOAT, IR_GLOBAL_BOOL, IR_GLOBAL_STRING } IRGlobalKind;

typedef struct {
    ForgeStr name;
    ForgeType type;
    IRGlobalKind kind;
    int64_t int_value;
    double float_value;
    bool bool_value;
    ForgeStr string_value;
    char *owned_name;
} IRGlobal;

typedef struct {
    ForgeStr name;
    IRLocal *fields;
    size_t field_count;
} IRStruct;

typedef struct {
    ForgeStr name;
    ForgeStr *variants;
    int64_t *values;
    size_t variant_count;
} IREnum;

typedef struct {
    ForgeStr name, module;
    ForgeType return_type;
    IRLocal *locals;
    size_t local_count, local_cap;
    IRBlock *blocks;
    size_t block_count, block_cap;
    uint32_t next_value;
    bool is_extern;
    bool is_native;
} IRFunction;

typedef struct {
    IRFunction *functions;
    size_t function_count, function_cap;
    IRGlobal *globals;
    size_t global_count, global_cap;
    IRStruct *structs;
    size_t struct_count, struct_cap;
    IREnum *enums;
    size_t enum_count, enum_cap;
} IRModule;

/* Lowers supported ordinary functions and native entry blocks into
 * target-neutral basic blocks and typed values. Calls without a visible
 * declaration are kept with an explicitly unknown result type. Literal
 * constants, structs and enums are represented explicitly; unsupported
 * declaration forms fail with a diagnostic rather than producing partial IR. */
bool ir_lower_program(const Program *program, IRModule *out);
void ir_dump(const IRModule *module, FILE *out);
void ir_module_free(IRModule *module);

#endif
