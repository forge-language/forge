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
    p->bindings[p->binding_count] = (ForgeTypedBinding){name, type, span, scope};
    return p->binding_count++;
}
size_t forge_typed_expression(ForgeTypedProgram *p, const Expr *e, size_t parent, size_t scope) {
    p->expressions = forge_array_reserve(p->expressions, p->expression_count,
        &p->expression_capacity, sizeof(*p->expressions), 32);
    ForgeTypedExpr node = {.origin=e, .kind=e->kind, .span=e->span,
        .type={.kind=FORGE_TYPED_UNKNOWN}, .parent=parent, .scope=scope,
        .binding=FORGE_TYPED_NONE};
    if (e->kind == EXPR_CALL || e->kind == EXPR_QUAL_CALL)
        node.effects |= FORGE_TYPED_CALL_EFFECT;
    if (e->kind == EXPR_INDEX || (e->kind == EXPR_BINARY &&
        (e->as.binary.op == BIN_DIV || e->as.binary.op == BIN_MOD)))
        node.effects |= FORGE_TYPED_MAY_TRAP;
    if (e->kind == EXPR_BINARY && (e->as.binary.op == BIN_AND || e->as.binary.op == BIN_OR))
        node.effects |= FORGE_TYPED_SHORT_CIRCUIT;
    p->expressions[p->expression_count] = node;
    return p->expression_count++;
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
        } else if (e->call != FORGE_CALL_NONE) return false;
        if (e->has_expected && !valid_type(e->expected)) return false;
    }
    return true;
}
void forge_typed_free(ForgeTypedProgram *p) {
    free(p->expressions);
    free(p->bindings);
    free(p->scope_parents);
    *p = (ForgeTypedProgram){0};
}
