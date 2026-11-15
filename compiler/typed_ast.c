#include "typed_ast.h"

size_t forge_typed_scope(ForgeTypedProgram *p, size_t parent) {
    p->scope_parents = forge_array_reserve(p->scope_parents, p->scope_count,
        &p->scope_capacity, sizeof(*p->scope_parents), 16);
    p->scope_parents[p->scope_count] = parent;
    return p->scope_count++;
}
size_t forge_typed_binding(ForgeTypedProgram *p, ForgeStr name, ForgeType type,
                         SourceSpan span, size_t scope) {
    p->bindings = forge_array_reserve(p->bindings, p->binding_count,
        &p->binding_capacity, sizeof(*p->bindings), 16);
    p->bindings[p->binding_count] = (ForgeTypedBinding){.name=name, .type=type, .span=span, .scope=scope};
    return p->binding_count++;
}
size_t forge_typed_expression(ForgeTypedProgram *p, const Expr *e, size_t parent, size_t scope) {
    p->expressions = forge_array_reserve(p->expressions, p->expression_count,
        &p->expression_capacity, sizeof(*p->expressions), 32);
    ForgeTypedExpr node = {.origin=e, .kind=e->kind, .span=e->span,
        .type={.kind=FORGE_TYPED_UNKNOWN}, .parent=parent, .scope=scope,
        .binding=FORGE_TYPED_NONE, .left=FORGE_TYPED_NONE, .right=FORGE_TYPED_NONE};
    p->expressions[p->expression_count] = node;
    return p->expression_count++;
}
static bool scalar_numeric(ForgeType t) {
    return t.kind == TY_INT || t.kind == TY_BOOL || t.kind == TY_FLOAT;
}
static ForgeTypedConversion conversion(ForgeTypedType from, ForgeType to, ForgeTypedUse use) {
    if (from.kind == FORGE_TYPED_UNKNOWN) return FORGE_CONVERT_OPAQUE;
    if (from.kind == FORGE_TYPED_FUNCTION)
        return use == FORGE_USE_ARGUMENT && (to.kind == TY_INT || to.kind == TY_PTR) ?
            FORGE_CONVERT_CALLBACK : FORGE_CONVERT_INVALID;
    if (from.kind != FORGE_TYPED_VALUE) return FORGE_CONVERT_INVALID;
    ForgeType value = from.value;
    if (use == FORGE_USE_TRUTH)
        return to.kind == TY_BOOL && scalar_numeric(value) ? FORGE_CONVERT_TRUTH : FORGE_CONVERT_INVALID;
    if (to.kind == value.kind)
        return to.kind != TY_STRUCT || forge_str_eq(to.struct_name, value.struct_name) ?
            FORGE_CONVERT_NONE : FORGE_CONVERT_INVALID;
    if (to.kind == TY_FLOAT && value.kind == TY_INT) return FORGE_CONVERT_INT_FLOAT;
    if (to.kind == TY_FLOAT && value.kind == TY_BOOL) return FORGE_CONVERT_BOOL_FLOAT;
    if (to.kind == TY_INT && value.kind == TY_BOOL) return FORGE_CONVERT_BOOL_INT;
    if (to.kind == TY_BOOL && value.kind == TY_INT) return FORGE_CONVERT_INT_BOOL_LEGACY;
    return FORGE_CONVERT_INVALID;
}
void forge_typed_expect(ForgeTypedProgram *p, size_t node, ForgeType to, ForgeTypedUse use) {
    ForgeTypedExpr *e = &p->expressions[node];
    e->has_expected = true;
    e->expected = to;
    e->use = use;
    e->conversion = conversion(e->type, to, use);
}
ForgeTypedComparison forge_typed_comparison(ForgeTypedType a, ForgeTypedType b) {
    if (a.kind == FORGE_TYPED_UNKNOWN || b.kind == FORGE_TYPED_UNKNOWN) return FORGE_COMPARE_OPAQUE;
    if (a.kind != FORGE_TYPED_VALUE || b.kind != FORGE_TYPED_VALUE ||
        !scalar_numeric(a.value) || !scalar_numeric(b.value)) return FORGE_COMPARE_NONE;
    bool af = a.value.kind == TY_FLOAT, bf = b.value.kind == TY_FLOAT;
    return af && bf ? FORGE_COMPARE_FLOAT : af || bf ? FORGE_COMPARE_EXACT_MIXED : FORGE_COMPARE_INTEGER;
}
static unsigned local_effects(const ForgeTypedExpr *e) {
    switch (e->kind) {
    case EXPR_CALL: case EXPR_QUAL_CALL: return FORGE_TYPED_CALL_EFFECT;
    case EXPR_RECV: return FORGE_TYPED_RECEIVE_EFFECT;
    case EXPR_INDEX: return FORGE_TYPED_MAY_TRAP;
    case EXPR_BINARY: {
        BinOp op = e->origin->as.binary.op;
        if (op == BIN_AND || op == BIN_OR) return FORGE_TYPED_SHORT_CIRCUIT;
        if ((op == BIN_DIV || op == BIN_MOD) &&
            (e->type.kind != FORGE_TYPED_VALUE || e->type.value.kind != TY_FLOAT))
            return FORGE_TYPED_MAY_TRAP;
        return 0;
    }
    default: return 0;
    }
}
void forge_typed_summarize(ForgeTypedProgram *p) {
    for (size_t i = 0; i < p->expression_count; ++i) {
        p->expressions[i].effects = local_effects(&p->expressions[i]);
        p->expressions[i].subtree_effects = p->expressions[i].effects;
    }
    for (size_t i = p->expression_count; i-- > 0;) {
        ForgeTypedExpr *e = &p->expressions[i];
        if (e->parent != FORGE_TYPED_NONE)
            p->expressions[e->parent].subtree_effects |= e->subtree_effects;
    }
}
static bool valid_type(ForgeType t) {
    return t.kind >= TY_VOID && t.kind <= TY_STRUCT &&
        (t.kind != TY_STRUCT || (t.struct_name.data && t.struct_name.len));
}
static bool valid_span(SourceSpan s) {
    return s.start <= s.end && (!s.file || s.end <= s.file->length);
}
static bool visible_scope(const ForgeTypedProgram *p, size_t declaration, size_t use) {
    while (use != FORGE_TYPED_NONE) {
        if (use == declaration) return true;
        use = p->scope_parents[use];
    }
    return false;
}
bool forge_typed_verify(const ForgeTypedProgram *p) {
    if (!p || p->expression_count > p->expression_capacity ||
        p->binding_count > p->binding_capacity || p->scope_count > p->scope_capacity ||
        !p->scope_count || !p->scope_parents ||
        (p->expression_count && !p->expressions) || (p->binding_count && !p->bindings)) return false;
    for (size_t i=0; i<p->scope_count; ++i)
        if (i == 0 ? p->scope_parents[i] != FORGE_TYPED_NONE : p->scope_parents[i] >= i) return false;
    for (size_t i=0; i<p->binding_count; ++i) {
        const ForgeTypedBinding *b=&p->bindings[i];
        if (!b->name.data || !b->name.len || !valid_type(b->type) ||
            !valid_span(b->span) || b->scope >= p->scope_count) return false;
        if (b->cleanup_origin && (b->type.kind!=TY_INT ||
            b->cleanup_origin->kind!=STMT_LET || !b->cleanup_origin->as.let.owned_ ||
            !forge_str_eq(b->name,b->cleanup_origin->as.let.name))) return false;
    }
    for (size_t i=0; i<p->expression_count; ++i) {
        const ForgeTypedExpr *e=&p->expressions[i];
        if (!e->origin || e->kind != e->origin->kind || !valid_span(e->span) ||
            e->scope >= p->scope_count || (e->parent != FORGE_TYPED_NONE && e->parent >= i)) return false;
        if (e->type.kind == FORGE_TYPED_VALUE) { if (!valid_type(e->type.value)) return false; }
        else if (e->type.kind == FORGE_TYPED_FUNCTION) { if (!e->function) return false; }
        else if (e->type.kind != FORGE_TYPED_UNKNOWN) return false;
        if (e->binding != FORGE_TYPED_NONE) {
            if (e->binding >= p->binding_count ||
                !visible_scope(p,p->bindings[e->binding].scope,e->scope)) return false;
        }
        if (e->kind == EXPR_IDENT && e->binding == FORGE_TYPED_NONE && !e->constant && !e->function) return false;
        if (e->kind == EXPR_CALL || e->kind == EXPR_QUAL_CALL) {
            if (e->call <= FORGE_CALL_NONE || e->call > FORGE_CALL_OPAQUE) return false;
            if (e->call == FORGE_CALL_SOURCE && !e->function) return false;
            if (e->call == FORGE_CALL_STANDARD && !e->standard) return false;
            if (e->call == FORGE_CALL_STANDARD && e->resource_contract != forge_resource_contract(e->standard)) return false;
            if (e->call == FORGE_CALL_OPAQUE && e->resource_contract != forge_resource_contract(NULL)) return false;
        } else if (e->call != FORGE_CALL_NONE) return false;
        if (e->use < FORGE_USE_NONE || e->use > FORGE_USE_MATCH) return false;
        if (e->has_expected) {
            if (e->use == FORGE_USE_NONE || !valid_type(e->expected) ||
                e->conversion == FORGE_CONVERT_INVALID ||
                e->conversion != conversion(e->type, e->expected, e->use)) return false;
        } else if (e->conversion != FORGE_CONVERT_NONE) return false;
        if (e->effects != local_effects(e)) return false;
        bool conditional = false;
        if (e->parent != FORGE_TYPED_NONE) {
            const ForgeTypedExpr *parent = &p->expressions[e->parent];
            conditional = parent->kind == EXPR_BINARY && parent->right == i &&
                (parent->origin->as.binary.op == BIN_AND || parent->origin->as.binary.op == BIN_OR);
        }
        if (e->conditional != conditional) return false;
        if (e->kind == EXPR_BINARY) {
            if (e->left <= i || e->left >= p->expression_count ||
                e->right <= e->left || e->right >= p->expression_count ||
                p->expressions[e->left].parent != i || p->expressions[e->right].parent != i ||
                p->expressions[e->left].origin != e->origin->as.binary.left ||
                p->expressions[e->right].origin != e->origin->as.binary.right) return false;
            BinOp op = e->origin->as.binary.op;
            ForgeTypedComparison mode = op >= BIN_EQ && op <= BIN_GE ?
                forge_typed_comparison(p->expressions[e->left].type, p->expressions[e->right].type) : FORGE_COMPARE_NONE;
            if (e->comparison != mode) return false;
            bool logical = op == BIN_AND || op == BIN_OR;
            size_t operands[] = {e->left, e->right};
            for (size_t j = 0; j < 2; ++j) {
                const ForgeTypedExpr *value = &p->expressions[operands[j]];
                ForgeTypedUse use = logical ? FORGE_USE_TRUTH : mode != FORGE_COMPARE_NONE ?
                    FORGE_USE_COMPARISON : FORGE_USE_ARITHMETIC;
                ForgeTypedType expected = logical ?
                    (ForgeTypedType){FORGE_TYPED_VALUE, forge_type_bool()} :
                    mode != FORGE_COMPARE_NONE ? value->type : e->type;
                if (value->use != use || value->has_expected != (expected.kind == FORGE_TYPED_VALUE))
                    return false;
                if (value->has_expected && (value->expected.kind != expected.value.kind ||
                    (value->expected.kind == TY_STRUCT &&
                     !forge_str_eq(value->expected.struct_name, expected.value.struct_name)))) return false;
            }
        } else if (e->left != FORGE_TYPED_NONE || e->right != FORGE_TYPED_NONE ||
                   e->comparison != FORGE_COMPARE_NONE) return false;
    }
    if (!p->expression_count) return true;
    unsigned *effects = forge_calloc_array(p->expression_count, sizeof(*effects));
    for (size_t i = 0; i < p->expression_count; ++i) effects[i] = p->expressions[i].effects;
    bool valid = true;
    for (size_t i = p->expression_count; i-- > 0;) {
        const ForgeTypedExpr *e = &p->expressions[i];
        valid &= effects[i] == e->subtree_effects;
        if (e->parent != FORGE_TYPED_NONE) effects[e->parent] |= effects[i];
    }
    free(effects);
    return valid;
}
void forge_typed_free(ForgeTypedProgram *p) {
    free(p->expressions);
    free(p->bindings);
    free(p->scope_parents);
    *p = (ForgeTypedProgram){0};
}
