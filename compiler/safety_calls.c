#include "safety_internal.h"

SafetyValue safety_function(Safety *s, const FnDecl *fn, const SafetyValue *args, size_t count) {
    if (fn->is_extern) safety_error(fn->span,"extern callback or function lacks a checked ABI/lifetime contract");
    if (s->depth >= 64) safety_error(fn->span,"checked call-summary depth limit exceeded");
    uint64_t arenas=0, pending=0;
    for (size_t i=0;i<count;++i) {
        if (args[i].arena) arenas|=UINT64_C(1)<<i;
        if (args[i].pending) pending|=UINT64_C(1)<<i;
    }
    size_t id=0;
    for (;id<s->summaries->count;++id) {
        SafetySummary *m=&s->summaries->items[id];
        if (m->function!=fn || m->count!=count || m->arena_arguments!=arenas || m->pending_arguments!=pending) continue;
        bool same=true;
        for (size_t i=0;i<count;++i) same &= m->arguments[i]==args[i].kind;
        if (same) break;
    }
    if (id==s->summaries->count) {
        if (id>=FORGE_SAFETY_MAX_SUMMARIES) safety_error(fn->span,"checked call-summary budget exceeded");
        s->summaries->items=forge_array_reserve(s->summaries->items,id,&s->summaries->capacity,sizeof(*s->summaries->items),16);
        SafetySummary *m=&s->summaries->items[s->summaries->count++];
        *m=(SafetySummary){.function=fn,.count=count,.arena_arguments=arenas,.pending_arguments=pending};
        for (size_t i=0;i<count;++i) m->arguments[i]=args[i].kind;
    }
    if (s->summaries->items[id].state==1)
        safety_error(fn->span,"recursive checked resource summaries require an explicit contract");
    if (!s->summaries->items[id].state) {
        s->summaries->items[id].state=1;
        Safety child=*s;
        child.bindings=NULL; child.count=0; child.capacity=0; child.context=FORGE_TYPED_NONE;
        child.generation=0; child.resets=false; child.has_return=false; child.loop=NULL; ++child.depth;
        size_t i=0;
        for (Param *p=fn->params;p;p=p->next,++i) {
            if (p->type.kind==TY_PTR || p->type.kind==TY_STRUCT)
                safety_error(p->span,"raw pointer and struct parameters lack a checked resource contract");
            SafetyValue value=args[i];
            value.generation=0; value.parameters=UINT64_C(1)<<i;
            safety_declare(&child,p->name,p->span,value);
        }
        safety_statements(&child,fn->body.first);
        safety_check_pending(&child,0);
        SafetySummary *m=&s->summaries->items[id];
        m->result=child.has_return ? child.returned : safety_value(FORGE_RESOURCE_SCALAR);
        m->resets=child.resets;
        m->state=2;
        free(child.bindings);
    }
    SafetySummary m=s->summaries->items[id];
    if (m.resets) safety_reset(s);
    SafetyValue result=m.result;
    result.parameters=0;
    for (size_t i=0;i<count;++i) if (m.result.parameters & (UINT64_C(1)<<i)) {
        result.parameters|=args[i].parameters;
        result.arena|=args[i].arena;
        result.valid &= args[i].valid && (!args[i].arena || !m.resets);
    }
    result.generation=s->generation;
    return result;
}
static SafetyValue thread_call(Safety *s, Expr *e) {
    size_t callback=forge_typed_find(s->index,e->as.call.args[0],s->context);
    const FnDecl *fn=callback==FORGE_TYPED_NONE ? NULL : s->index->program->expressions[callback].function;
    bool indexed=forge_str_eq(e->as.call.name,forge_str("thread_spawn_indexed"));
    if (!fn || fn->ret_type.kind!=TY_INT) safety_error(e->span,"thread callback requires the checked int64 return ABI");
    for (Param *p=fn->params;p;p=p->next)
        if (p->type.kind!=TY_INT) safety_error(p->span,"thread callback arguments must use the int64 ABI");
    SafetyValue count=safety_expression(s,e->as.call.args[1]);
    if (count.kind!=FORGE_RESOURCE_SCALAR) safety_error(e->span,"thread spawn cannot transfer a borrowed resource as scalar bits");
    Safety detached=*s;
    detached.bindings=NULL; detached.count=0; detached.capacity=0; detached.context=FORGE_TYPED_NONE;
    SafetyValue input[2]={safety_value(FORGE_RESOURCE_SCALAR),safety_value(FORGE_RESOURCE_SCALAR)};
    (void)safety_function(&detached,fn,input,indexed ? 2 : 1);
    SafetyValue result=safety_value(FORGE_RESOURCE_SCALAR);
    result.pending=!indexed;
    return result;
}
SafetyValue safety_call(Safety *s, Expr *e, const ForgeTypedExpr *typed) {
    Expr **expressions=e->kind==EXPR_CALL ? e->as.call.args : e->as.qual_call.args;
    size_t count=e->kind==EXPR_CALL ? e->as.call.arg_count : e->as.qual_call.arg_count;
    if (count>FORGE_SAFETY_MAX_ARGS) safety_error(e->span,"checked calls support at most 32 arguments");
    if (typed->call==FORGE_CALL_BUILTIN && e->kind==EXPR_CALL &&
        (forge_str_eq(e->as.call.name,forge_str("thread_spawn")) ||
         forge_str_eq(e->as.call.name,forge_str("thread_spawn_indexed")))) return thread_call(s,e);
    SafetyValue args[FORGE_SAFETY_MAX_ARGS];
    for (size_t i=0;i<count;++i) args[i]=safety_expression(s,expressions[i]);
    for (size_t i=0;i<count;++i) safety_validate_value(s,args[i],expressions[i]->span);
    if (typed->call==FORGE_CALL_SOURCE) return safety_function(s,typed->function,args,count);
    if (typed->call==FORGE_CALL_BUILTIN) {
        for (size_t i=0;i<count;++i) {
            if (args[i].kind!=FORGE_RESOURCE_STRING && args[i].kind!=FORGE_RESOURCE_SCALAR)
                safety_error(expressions[i]->span,"printing raw resource handles is outside the checked boundary");
            if (args[i].pending) safety_error(expressions[i]->span,"printing a failure result does not handle it");
        }
        return safety_value(FORGE_RESOURCE_SCALAR);
    }
    const ForgeResourceContract *contract=typed->resource_contract;
    if (!contract || contract->opaque_reason)
        safety_error(e->span,contract ? contract->opaque_reason : "call lacks a checked resource contract");
    for (size_t i=0;i<count;++i) {
        ForgeResourceKind expected=contract->arguments[i];
        bool empty=expressions[i]->kind==EXPR_INT && expressions[i]->as.int_val==0 &&
            (expected==FORGE_RESOURCE_VIEW || expected==FORGE_RESOURCE_BUILDER);
        if (args[i].kind!=expected && !empty)
            safety_error(expressions[i]->span,"forged or wrong-kind resource argument");
        if (args[i].pending && expected==FORGE_RESOURCE_SCALAR)
            safety_error(expressions[i]->span,"failure result must be tested or explicitly discarded");
    }
    if (contract->reset_arena) safety_reset(s);
    SafetyValue result=safety_value(contract->result);
    result.arena=contract->region==FORGE_REGION_ARENA;
    if (contract->region==FORGE_REGION_ARGUMENT && count) {
        result.arena=args[0].arena;
        result.parameters=args[0].parameters;
    }
    result.generation=s->generation;
    result.pending=contract->must_use;
    return result;
}
