#ifndef FORGE_TYPED_AST_H
#define FORGE_TYPED_AST_H

#include "ast.h"
#include "mod_registry.h"

#define FORGE_TYPED_NONE SIZE_MAX

typedef enum {
    FORGE_TYPED_VALUE, FORGE_TYPED_UNKNOWN, FORGE_TYPED_FUNCTION
} ForgeTypedKind;
typedef struct {
    ForgeTypedKind kind;
    ForgeType value;
} ForgeTypedType;
typedef enum {
    FORGE_CALL_NONE, FORGE_CALL_SOURCE, FORGE_CALL_STANDARD,
    FORGE_CALL_BUILTIN, FORGE_CALL_OPAQUE
} ForgeTypedCall;
enum {
    FORGE_TYPED_CALL_EFFECT = 1,
    FORGE_TYPED_MAY_TRAP = 2,
    FORGE_TYPED_SHORT_CIRCUIT = 4
};
typedef struct {
    ForgeStr name;
    ForgeType type;
    SourceSpan span;
    size_t scope;
} ForgeTypedBinding;
typedef struct {
    const Expr *origin;
    ExprKind kind;
    SourceSpan span;
    ForgeTypedType type;
    size_t parent, scope, binding;
    const ConstDecl *constant;
    const FnDecl *function;
    const ForgeStdFn *standard;
    ForgeTypedCall call;
    unsigned effects;
    bool has_expected;
    ForgeType expected;
} ForgeTypedExpr;

/* Arrays belong to this pass. Origins, names, signatures and source spans borrow
 * the immutable Program after optimization. Free before mutating/freeing that
 * Program. Every interpretation, including a shared constant initializer, gets
 * a fresh node; origin pointers are provenance, never unique lookup keys.
 * Binding IDs are append-only and independent of the reusable semantic stack.
 * Expected types record existing compatibility requirements, not new coercion
 * or normalization rules. Effects describe the node itself, not its subtree. */
typedef struct {
    ForgeTypedExpr *expressions;
    size_t expression_count, expression_capacity;
    ForgeTypedBinding *bindings;
    size_t binding_count, binding_capacity;
    size_t *scope_parents;
    size_t scope_count, scope_capacity;
} ForgeTypedProgram;

size_t forge_typed_scope(ForgeTypedProgram *, size_t parent);
size_t forge_typed_binding(ForgeTypedProgram *, ForgeStr, ForgeType, SourceSpan, size_t scope);
size_t forge_typed_expression(ForgeTypedProgram *, const Expr *, size_t parent, size_t scope);
bool forge_typed_verify(const ForgeTypedProgram *);
void forge_typed_free(ForgeTypedProgram *);
#endif
