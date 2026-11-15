#include "safety_internal.h"

static SafetyValue expression_inner(Safety *s, Expr *e, const ForgeTypedExpr *typed) {
    switch (e->kind) {
    case EXPR_STRING: return safety_value(FORGE_RESOURCE_STRING);
    case EXPR_INT: case EXPR_BOOL: case EXPR_FLOAT: case EXPR_RECV:
        return safety_value(FORGE_RESOURCE_SCALAR);
    case EXPR_IDENT: {
        if (typed->constant) return safety_expression(s,typed->constant->value);
        size_t id=safety_binding(s,e->as.ident);
        if (id==SIZE_MAX) safety_error(e->span,"function reference cannot become checked scalar bits");
        SafetyValue value=s->bindings[id].value;
        safety_validate_value(s,value,e->span);
        s->bindings[id].value.pending=false;
        return value;
    }
    case EXPR_CALL: case EXPR_QUAL_CALL: return safety_call(s,e,typed);
    case EXPR_BINARY: {
        SafetyValue left=safety_expression(s,e->as.binary.left);
        BinOp op=e->as.binary.op;
        bool logical=op==BIN_AND || op==BIN_OR;
        bool literal=e->as.binary.left->kind==EXPR_BOOL || e->as.binary.left->kind==EXPR_INT;
        bool truth=e->as.binary.left->kind==EXPR_BOOL ? e->as.binary.left->as.bool_val :
            e->as.binary.left->kind==EXPR_INT && e->as.binary.left->as.int_val!=0;
        if (logical && literal && (op==BIN_AND ? !truth : truth)) {
            SafetyValue value=safety_value(FORGE_RESOURCE_SCALAR);
            value.pending=left.pending;
            return value;
        }
        SafetyBinding *before=logical && !literal ? safety_snapshot(s,s->count) : NULL;
        SafetyValue right=safety_expression(s,e->as.binary.right);
        if (before) { safety_join(s,before,s->count); free(before); }
        safety_validate_value(s,left,e->as.binary.left->span);
        safety_validate_value(s,right,e->as.binary.right->span);
        if (left.kind!=FORGE_RESOURCE_SCALAR || right.kind!=FORGE_RESOURCE_SCALAR) {
            bool equality=op==BIN_EQ || op==BIN_NE;
            bool left_zero=e->as.binary.left->kind==EXPR_INT && e->as.binary.left->as.int_val==0;
            bool right_zero=e->as.binary.right->kind==EXPR_INT && e->as.binary.right->as.int_val==0;
            if (!equality || !(left.kind==right.kind || left_zero || right_zero))
                safety_error(e->span,"resource handles cannot be forged through scalar arithmetic");
        }
        SafetyValue value=safety_value(FORGE_RESOURCE_SCALAR);
        value.pending=left.pending || right.pending;
        return value;
    }
    case EXPR_INDEX: {
        SafetyValue base=safety_expression(s,e->as.index.base);
        SafetyValue index=safety_expression(s,e->as.index.index);
        safety_validate_value(s,base,e->span);
        if (base.kind!=FORGE_RESOURCE_STRING || index.kind!=FORGE_RESOURCE_SCALAR)
            safety_error(e->span,"raw pointer indexing is outside the checked boundary");
        if (index.pending) safety_error(e->span,"failure result cannot be used as an unchecked index");
        return safety_value(FORGE_RESOURCE_SCALAR);
    }
    case EXPR_FIELD: safety_error(e->span,"raw field/callback ABI requires an explicit checked contract"); break;
    case EXPR_MOVE: safety_error(e->span,"general resource move is unavailable"); break;
    }
    return safety_value(FORGE_RESOURCE_OPAQUE);
}
SafetyValue safety_expression(Safety *s, Expr *e) {
    if (!e) return safety_value(FORGE_RESOURCE_SCALAR);
    if (++s->depth>512) safety_error(e->span,"checked expression nesting limit exceeded");
    if (!s->summaries->work_left--) safety_error(e->span,"checked analysis work budget exceeded");
    size_t saved=s->context;
    s->context=forge_typed_find(s->index,e,saved);
    if (s->context==FORGE_TYPED_NONE) forge_die("missing checked expression interpretation");
    const ForgeTypedExpr *typed=&s->index->program->expressions[s->context];
    if (typed->type.kind==FORGE_TYPED_VALUE && typed->type.value.kind==TY_PTR)
        safety_error(e->span,"raw pointer values are outside the checked boundary");
    SafetyValue result=expression_inner(s,e,typed);
    s->context=saved;
    --s->depth;
    return result;
}
