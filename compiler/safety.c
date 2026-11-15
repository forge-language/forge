#include "safety_internal.h"

void safety_error(SourceSpan span, const char *message) {
    char legacy[512];
    snprintf(legacy, sizeof(legacy), "forge: safety: %s", message);
    forge_source_diagnostic(span, "safety", message, legacy);
    exit(1);
}
SafetyValue safety_value(ForgeResourceKind kind) {
    return (SafetyValue){.kind=kind, .valid=true};
}
size_t safety_binding(Safety *s, ForgeStr name) {
    for (size_t i=s->count; i-- > 0;)
        if (forge_str_eq(s->bindings[i].name, name)) return i;
    return SIZE_MAX;
}
void safety_declare(Safety *s, ForgeStr name, SourceSpan span, SafetyValue value) {
    if (forge_str_eq(name, forge_str("_"))) value.pending=false;
    s->bindings=forge_array_reserve(s->bindings,s->count,&s->capacity,sizeof(*s->bindings),16);
    s->bindings[s->count++]=(SafetyBinding){name,span,value};
}
void safety_validate_value(Safety *s, SafetyValue value, SourceSpan span) {
    if (!value.valid || (value.arena && value.generation != s->generation))
        safety_error(span,"use of an arena loan after reset or invalidation");
    if (value.kind == FORGE_RESOURCE_OPAQUE)
        safety_error(span,"resource identity differs across reachable paths");
}
void safety_reset(Safety *s) {
    if (s->generation == SIZE_MAX) forge_die("too many arena generations");
    ++s->generation;
    s->resets=true;
    for (size_t i=0;i<s->count;++i)
        if (s->bindings[i].value.arena) s->bindings[i].value.valid=false;
}
void safety_check_pending(Safety *s, size_t from) {
    for (size_t i=from;i<s->count;++i)
        if (s->bindings[i].value.pending)
            safety_error(s->bindings[i].span,"failure result must be tested, returned, or explicitly discarded with let _");
}
SafetyBinding *safety_snapshot(const Safety *s, size_t count) {
    SafetyBinding *copy=forge_calloc_array(count ? count : 1,sizeof(*copy));
    if (count) memcpy(copy,s->bindings,count*sizeof(*copy));
    return copy;
}
void safety_join(Safety *s, const SafetyBinding *other, size_t count) {
    for (size_t i=0;i<count;++i) {
        SafetyValue *a=&s->bindings[i].value;
        SafetyValue b=other[i].value;
        if (a->kind != b.kind) a->kind=FORGE_RESOURCE_OPAQUE;
        a->valid &= b.valid;
        a->arena |= b.arena;
        a->pending |= b.pending;
        a->parameters |= b.parameters;
        a->generation=s->generation;
    }
}
static void check_body(Safety *root, Param *params, Block *body, bool coroutine) {
    Safety s=*root;
    s.bindings=NULL; s.count=0; s.capacity=0; s.context=FORGE_TYPED_NONE;
    s.coroutine=coroutine; s.loop=NULL;
    for (Param *p=params;p;p=p->next) {
        if (p->type.kind == TY_PTR || p->type.kind == TY_STRUCT)
            safety_error(p->span,"raw pointer and struct ABI parameters require an explicit checked contract");
        SafetyValue value=safety_value(p->type.kind==TY_STRING ? FORGE_RESOURCE_STRING : FORGE_RESOURCE_SCALAR);
        value.arena=p->type.kind==TY_STRING;
        safety_declare(&s,p->name,p->span,value);
    }
    if (body) safety_statements(&s,body->first);
    safety_check_pending(&s,0);
    free(s.bindings);
}
void forge_check_safety(Program *p, const ForgeTypedProgram *typed) {
    ForgeTypedIndex index=forge_typed_index(typed);
    SafetySummaries summaries={.work_left=200000};
    Safety root={.program=p,.index=&index,.summaries=&summaries,.context=FORGE_TYPED_NONE};
    if (p->struct_count) safety_error((SourceSpan){0},"struct resource fields require an explicit checked contract");
    for (size_t i=0;i<p->native_count;++i) check_body(&root,NULL,&p->natives[i].body,false);
    for (size_t i=0;i<p->process_count;++i) {
        root.process=&p->processes[i];
        check_body(&root,NULL,&p->processes[i].body,false);
        if (p->processes[i].has_receive)
            check_body(&root,&p->processes[i].receive_param,p->processes[i].on_receive,false);
        for (size_t j=0;j<p->processes[i].coro_count;++j)
            check_body(&root,p->processes[i].coros[j].params,&p->processes[i].coros[j].body,true);
    }
    if (p->library.present)
        for (size_t i=0;i<p->library.fn_count;++i)
            check_body(&root,p->library.functions[i].params,&p->library.functions[i].body,false);
    free(summaries.items);
    forge_typed_index_free(&index);
}
