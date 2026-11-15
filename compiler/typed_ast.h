#ifndef FORGE_TYPED_AST_H
#define FORGE_TYPED_AST_H

#include "ast.h"
#include "mod_registry.h"
#include "resource_contracts.h"

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
typedef enum {
    FORGE_USE_NONE, FORGE_USE_INITIALIZER, FORGE_USE_ASSIGNMENT,
    FORGE_USE_ARGUMENT, FORGE_USE_RETURN, FORGE_USE_ARITHMETIC,
    FORGE_USE_COMPARISON, FORGE_USE_TRUTH, FORGE_USE_INDEX, FORGE_USE_MATCH
} ForgeTypedUse;
typedef enum {
    FORGE_CONVERT_NONE, FORGE_CONVERT_INT_FLOAT, FORGE_CONVERT_BOOL_FLOAT,
    FORGE_CONVERT_BOOL_INT, FORGE_CONVERT_INT_BOOL_LEGACY,
    FORGE_CONVERT_TRUTH, FORGE_CONVERT_OPAQUE, FORGE_CONVERT_CALLBACK,
    FORGE_CONVERT_INVALID
} ForgeTypedConversion;
typedef enum {
    FORGE_COMPARE_NONE, FORGE_COMPARE_INTEGER, FORGE_COMPARE_FLOAT,
    FORGE_COMPARE_EXACT_MIXED, FORGE_COMPARE_OPAQUE
} ForgeTypedComparison;
enum {
    FORGE_TYPED_CALL_EFFECT = 1,
    FORGE_TYPED_MAY_TRAP = 2,
    FORGE_TYPED_SHORT_CIRCUIT = 4,
    FORGE_TYPED_RECEIVE_EFFECT = 8
};
typedef struct {
    ForgeStr name;
    ForgeType type;
    SourceSpan span;
    size_t scope;
    const Stmt *cleanup_origin;
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
    const ForgeResourceContract *resource_contract;
    ForgeTypedCall call;
    unsigned effects;
    unsigned subtree_effects;
    size_t left, right;
    bool conditional;
    bool has_expected;
    ForgeType expected;
    ForgeTypedUse use;
    ForgeTypedConversion conversion;
    ForgeTypedComparison comparison;
} ForgeTypedExpr;

/* Arrays belong to this pass. Origins, names, signatures and source spans borrow
 * the immutable Program after optimization. Free before mutating/freeing that
 * Program. Every interpretation, including a shared constant initializer, gets
 * a fresh node; origin pointers are provenance, never unique lookup keys.
 * Binding IDs are append-only and independent of the reusable semantic stack.
 * A conversion belongs to this use's output edge; it does not mutate the origin
 * or a shared constant initializer. Legacy bool boundaries do not normalize.
 * Opaque boundaries retain unverified C ABI behavior. Effects are local;
 * subtree_effects also includes potentially skipped conditional descendants.
 * CALL_EFFECT is conservative and may include arbitrary failure/side effects. */
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
void forge_typed_expect(ForgeTypedProgram *, size_t, ForgeType, ForgeTypedUse);
ForgeTypedComparison forge_typed_comparison(ForgeTypedType, ForgeTypedType);
void forge_typed_summarize(ForgeTypedProgram *);
bool forge_typed_verify(const ForgeTypedProgram *);
void forge_typed_free(ForgeTypedProgram *);
#endif
