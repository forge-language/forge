#include "safety_internal.h"

bool safety_block(Safety *s, Block *body) {
    if (!body) return true;
    size_t count=s->count;
    bool reachable=safety_statements(s,body->first);
    safety_check_pending(s,count);
    s->count=count;
    return reachable;
}
static bool branch(Safety *s, Stmt *stmt) {
    (void)safety_expression(s,stmt->as.if_stmt.cond);
    size_t count=s->count, generation=s->generation;
    SafetyBinding *before=safety_snapshot(s,count);
    bool a=safety_block(s,stmt->as.if_stmt.then_br);
    SafetyBinding *then=safety_snapshot(s,count);
    size_t then_generation=s->generation;
    if (count) memcpy(s->bindings,before,count*sizeof(*before));
    s->generation=generation;
    bool b=safety_block(s,stmt->as.if_stmt.else_br);
    if (a && b) {
        if (then_generation>s->generation) s->generation=then_generation;
        safety_join(s,then,count);
    } else if (a) {
        if (count) memcpy(s->bindings,then,count*sizeof(*then));
        s->generation=then_generation;
    }
    free(before); free(then);
    return a || b;
}
static bool match_statement(Safety *s, Stmt *stmt) {
    (void)safety_expression(s,stmt->as.match_stmt.scrutinee);
    size_t count=s->count, generation=s->generation;
    SafetyBinding *before=safety_snapshot(s,count), *joined=NULL;
    bool any=false, exhaustive=false;
    for (MatchArm *a=stmt->as.match_stmt.arms;a;a=a->next) {
        if (count) memcpy(s->bindings,before,count*sizeof(*before));
        s->generation=generation;
        if (safety_block(s,a->body)) {
            if (any) safety_join(s,joined,count);
            free(joined); joined=safety_snapshot(s,count); any=true;
        }
        exhaustive |= a->wildcard;
        if (a->wildcard) break;
    }
    if (!exhaustive) {
        if (count) memcpy(s->bindings,before,count*sizeof(*before));
        s->generation=generation;
        if (any) safety_join(s,joined,count);
        free(joined); joined=safety_snapshot(s,count); any=true;
    }
    if (any && count) memcpy(s->bindings,joined,count*sizeof(*joined));
    /* Join generations by rebasing only values that survived every path. */
    for (size_t i=0;i<count;++i) s->bindings[i].value.generation=s->generation;
    free(joined); free(before);
    return any;
}
bool safety_statement(Safety *s, Stmt *stmt) {
    if (!s->summaries->work_left--) safety_error(stmt->span,"checked analysis work budget exceeded");
    switch (stmt->kind) {
    case STMT_LET: {
        if (stmt->as.let.type.kind==TY_PTR || stmt->as.let.type.kind==TY_STRUCT)
            safety_error(stmt->span,"raw pointer and struct locals are outside the checked boundary");
        SafetyValue value=safety_expression(s,stmt->as.let.init);
        safety_declare(s,stmt->as.let.name,stmt->focus,value);
        return true;
    }
    case STMT_ASSIGN: {
        size_t id=safety_binding(s,stmt->as.assign.name);
        SafetyValue value=safety_expression(s,stmt->as.assign.value);
        if (id==SIZE_MAX) forge_die("missing checked assignment binding");
        if (s->bindings[id].value.pending) safety_error(stmt->span,"overwriting an unhandled failure result");
        if (forge_str_eq(stmt->as.assign.name,forge_str("_"))) value.pending=false;
        s->bindings[id].value=value;
        return true;
    }
    case STMT_EXPR: {
        SafetyValue value=safety_expression(s,stmt->as.expr);
        if (value.pending) safety_error(stmt->span,"discarded failure result; test it or explicitly use let _");
        return true;
    }
    case STMT_RETURN: {
        SafetyValue value=safety_expression(s,stmt->as.ret);
        safety_check_pending(s,0);
        if (!s->has_return) { s->returned=value; s->has_return=true; }
        else {
            if (s->returned.kind!=value.kind) s->returned.kind=FORGE_RESOURCE_OPAQUE;
            s->returned.arena |= value.arena;
            s->returned.pending |= value.pending;
            s->returned.parameters |= value.parameters;
            s->returned.valid &= value.valid;
        }
        return false;
    }
    case STMT_IF: return branch(s,stmt);
    case STMT_WHILE: case STMT_FOR: return safety_loop(s,stmt);
    case STMT_BLOCK: return safety_block(s,stmt->as.block);
    case STMT_MATCH: return match_statement(s,stmt);
    case STMT_BREAK: safety_loop_exit(s,false); return false;
    case STMT_CONTINUE: safety_loop_exit(s,true); return false;
    case STMT_YIELD: return true;
    case STMT_AWAIT: (void)safety_expression(s,stmt->as.await_expr); return true;
    case STMT_SPAWN:
        for (size_t i=0;i<stmt->as.spawn.arg_count;++i) {
            SafetyValue value=safety_expression(s,stmt->as.spawn.args[i]);
            if (value.arena || value.kind==FORGE_RESOURCE_VIEW || value.kind==FORGE_RESOURCE_BUILDER)
                safety_error(stmt->as.spawn.args[i]->span,"spawn cannot transfer a loan owned by another arena or worker");
            if (value.pending) safety_error(stmt->span,"spawn cannot discard an unhandled failure");
        }
        return true;
    case STMT_SEND: {
        SafetyValue target=safety_expression(s,stmt->as.send.target);
        if (target.kind!=FORGE_RESOURCE_SCALAR || stmt->as.send.target->kind!=EXPR_INT || stmt->as.send.target->as.int_val!=0)
            safety_error(stmt->as.send.target->span,"process target lacks a verified handle contract");
        SafetyValue value=safety_expression(s,stmt->as.send.value);
        if (value.kind!=FORGE_RESOURCE_SCALAR && !(stmt->as.send.move_ && value.kind==FORGE_RESOURCE_STRING))
            safety_error(stmt->span,"send requires scalar data or a copying owned-string boundary");
        if (value.pending) safety_error(stmt->span,"send cannot discard a failure result");
        return true;
    }
    }
    forge_die("unknown checked statement");
}
bool safety_statements(Safety *s, Stmt *stmt) {
    if (++s->depth>512) safety_error(stmt ? stmt->span : (SourceSpan){0},"checked block nesting limit exceeded");
    bool reachable=true;
    for (;stmt && reachable;stmt=stmt->next) reachable=safety_statement(s,stmt);
    --s->depth;
    return reachable;
}
