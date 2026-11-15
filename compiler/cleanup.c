#include "cleanup.h"

typedef struct { ForgeStr name; bool owner; } Binding;
typedef struct {
    ForgeTypedIndex index;
    Binding *bindings;
    size_t count, capacity;
    unsigned depth;
    bool suspended, step;
} Cleanup;
static void fail(SourceSpan span, const char *message) {
    forge_source_diagnostic(span,"ownership",message,message);
    exit(1);
}
const ForgeResourceContract *forge_cleanup_factory(const ForgeTypedIndex *index, const Expr *e) {
    size_t id=forge_typed_find(index,e,FORGE_TYPED_NONE);
    if (id==FORGE_TYPED_NONE) return NULL;
    const ForgeTypedExpr *node=&index->program->expressions[id];
    const ForgeResourceContract *c=node->resource_contract;
    return node->call==FORGE_CALL_STANDARD && c && c->scope_cleanup ? c : NULL;
}
static void bind(Cleanup *c, ForgeStr name, bool owner) {
    c->bindings=forge_array_reserve(c->bindings,c->count,&c->capacity,sizeof(*c->bindings),16);
    c->bindings[c->count++]=(Binding){name,owner};
}
static bool owner(Cleanup *c, ForgeStr name) {
    for (size_t i=c->count;i-- > 0;)
        if (forge_str_eq(c->bindings[i].name,name)) return c->bindings[i].owner;
    return false;
}
static void statements(Cleanup *, Stmt *);
static void block(Cleanup *c, Block *b) {
    size_t count=c->count;
    if (b) statements(c,b->first);
    c->count=count;
}
static void factory(Cleanup *c, Expr *e, SourceSpan span) {
    if (!forge_cleanup_factory(&c->index,e))
        fail(span,"owned resource requires a verified SDK factory");
}
static void statements(Cleanup *c, Stmt *s) {
    if (++c->depth>512) forge_die("resource cleanup nesting limit exceeded");
    for (;s;s=s->next) switch (s->kind) {
    case STMT_LET: {
        bool managed=s->as.let.owned_ && s->as.let.type.kind==TY_INT;
        if (managed) {
            if (c->step) fail(s->span,"owned resource declaration in a loop step is unsupported");
            if (c->suspended) fail(s->span,"owned resource cleanup is not supported in processes or coroutines");
            factory(c,s->as.let.init,s->span);
        }
        bind(c,s->as.let.name,managed);
        break;
    }
    case STMT_ASSIGN:
        if (owner(c,s->as.assign.name)) {
            if (c->step) fail(s->span,"owned resource replacement in a loop step is unsupported");
            factory(c,s->as.assign.value,s->span);
        }
        break;
    case STMT_IF:
        block(c,s->as.if_stmt.then_br); block(c,s->as.if_stmt.else_br); break;
    case STMT_WHILE: block(c,s->as.while_stmt.body); break;
    case STMT_FOR: {
        size_t count=c->count;
        statements(c,s->as.for_stmt.init);
        block(c,s->as.for_stmt.body);
        bool step=c->step; c->step=true;
        statements(c,s->as.for_stmt.step);
        c->step=step; c->count=count; break;
    }
    case STMT_BLOCK: block(c,s->as.block); break;
    case STMT_MATCH:
        for (MatchArm *a=s->as.match_stmt.arms;a;a=a->next) block(c,a->body);
        break;
    default: break;
    }
    --c->depth;
}
static void function(Cleanup *c, FnDecl *fn) {
    c->count=0;
    for (Param *p=fn->params;p;p=p->next) bind(c,p->name,false);
    if (!fn->is_extern) block(c,&fn->body);
}
static bool command_borrow(const ForgeTypedExpr *call) {
    return call->call==FORGE_CALL_STANDARD && call->resource_contract &&
        (call->resource_contract->borrow_arguments&1) &&
        (!strcmp(call->standard->c_name,"fr_os_command_arg") ||
         !strcmp(call->standard->c_name,"fr_os_command_run"));
}
static bool wrapper_borrow(const ForgeTypedIndex *index, const ForgeTypedExpr *call,
                           const ForgeTypedExpr *argument) {
    const FnDecl *fn=call->function;
    if (call->call!=FORGE_CALL_SOURCE || !fn || fn->is_extern) return false;
    const Stmt *s=fn->body.first;
    if (!s || s->next || (s->kind!=STMT_RETURN && s->kind!=STMT_EXPR)) return false;
    const Expr *body=s->kind==STMT_RETURN ? s->as.ret : s->as.expr;
    size_t id=forge_typed_find(index,body,FORGE_TYPED_NONE);
    const ForgeTypedProgram *p=index->program;
    if (id==FORGE_TYPED_NONE || !command_borrow(&p->expressions[id])) return false;
    Expr **body_args=body->kind==EXPR_CALL ? body->as.call.args : body->as.qual_call.args;
    const Expr *handle=body_args[0];
    if (handle->kind!=EXPR_IDENT) return false;
    const Expr *origin=call->origin;
    Expr **args=origin->kind==EXPR_CALL ? origin->as.call.args : origin->as.qual_call.args;
    const Param *param=fn->params;
    for (size_t i=0;param;param=param->next,++i) {
        if (args[i]!=argument->origin) continue;
        if (param->type.kind!=TY_INT || !forge_str_eq(param->name,handle->as.ident)) return false;
        size_t first=forge_typed_find(index,handle,id);
        if (first==FORGE_TYPED_NONE || p->expressions[first].binding==FORGE_TYPED_NONE) return false;
        size_t binding=p->expressions[first].binding;
        for (size_t j=id+1;j<p->expression_count;++j) {
            const ForgeTypedExpr *use=&p->expressions[j];
            if (use->parent==FORGE_TYPED_NONE || use->parent<id) break;
            if (use->binding==binding && j!=first) return false;
        }
        return true;
    }
    return false;
}
static bool borrowed_use(const ForgeTypedIndex *index, const ForgeTypedExpr *e) {
    const ForgeTypedProgram *p=index->program;
    if (e->parent==FORGE_TYPED_NONE)
        return e->has_expected && e->use==FORGE_USE_TRUTH;
    const ForgeTypedExpr *parent=&p->expressions[e->parent];
    if (command_borrow(parent)) {
        const Expr *call=parent->origin;
        Expr **args=call->kind==EXPR_CALL ? call->as.call.args : call->as.qual_call.args;
        return args[0]==e->origin;
    }
    if (wrapper_borrow(index,parent,e)) return true;
    if (parent->kind!=EXPR_BINARY) return false;
    const Expr *op=parent->origin;
    const Expr *other=op->as.binary.left==e->origin ? op->as.binary.right : op->as.binary.left;
    return (op->as.binary.op==BIN_EQ || op->as.binary.op==BIN_NE) &&
        other->kind==EXPR_INT && other->as.int_val==0;
}
void forge_check_cleanup(Program *p, const ForgeTypedProgram *typed) {
    Cleanup c={.index=forge_typed_index(typed)};
    for (size_t i=0;i<typed->expression_count;++i) {
        const ForgeTypedExpr *e=&typed->expressions[i];
        if (e->binding!=FORGE_TYPED_NONE && typed->bindings[e->binding].cleanup_origin &&
            !borrowed_use(&c.index,e))
            fail(e->span,"owned resource cannot escape, alias, or be manually released; borrow it through its SDK operations");
    }
    for (size_t i=0;i<p->fn_count;++i) function(&c,&p->functions[i]);
    for (size_t i=0;i<p->library.fn_count;++i) function(&c,&p->library.functions[i]);
    for (size_t i=0;i<p->module_count;++i)
        for (size_t j=0;j<p->modules[i].fn_count;++j) function(&c,&p->modules[i].functions[j]);
    for (size_t i=0;i<p->native_count;++i) { c.count=0; block(&c,&p->natives[i].body); }
    c.suspended=true;
    for (size_t i=0;i<p->process_count;++i) {
        ProcessDecl *proc=&p->processes[i];
        c.count=0; block(&c,&proc->body);
        if (proc->has_receive) { c.count=0; block(&c,proc->on_receive); }
        for (size_t j=0;j<proc->coro_count;++j) { c.count=0; block(&c,&proc->coros[j].body); }
    }
    free(c.bindings); forge_typed_index_free(&c.index);
}
