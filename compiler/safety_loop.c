#include "safety_internal.h"

static void merge_exit(Safety *s, SafetyBinding **states, bool *present, size_t *generation, size_t count) {
    if (*present) {
        if (*generation>s->generation) s->generation=*generation;
        safety_join(s,*states,count);
        free(*states);
    }
    *states=safety_snapshot(s,count);
    *present=true;
    *generation=s->generation;
}
void safety_loop_exit(Safety *s, bool is_continue) {
    if (!s->loop) forge_die("checked loop exit without loop");
    SafetyLoop *loop=s->loop;
    if (is_continue)
        merge_exit(s,&loop->continues,&loop->has_continue,&loop->continue_generation,loop->count);
    else merge_exit(s,&loop->breaks,&loop->has_break,&loop->break_generation,loop->count);
}
static int truth(const Expr *e) {
    if (!e) return 1;
    if (e->kind==EXPR_INT) return e->as.int_val!=0;
    if (e->kind==EXPR_BOOL) return e->as.bool_val;
    if (e->kind==EXPR_FLOAT) return e->as.float_val!=0;
    return -1;
}
static bool same_states(const SafetyBinding *a, const SafetyBinding *b, size_t count) {
    for (size_t i=0;i<count;++i) {
        SafetyValue x=a[i].value,y=b[i].value;
        if (x.kind!=y.kind || x.valid!=y.valid || x.arena!=y.arena ||
            x.pending!=y.pending || x.parameters!=y.parameters) return false;
    }
    return true;
}
bool safety_loop(Safety *s, Stmt *stmt) {
    size_t outer=s->count;
    bool is_for=stmt->kind==STMT_FOR;
    if (is_for && stmt->as.for_stmt.init) safety_statement(s,stmt->as.for_stmt.init);
    Expr *condition=is_for ? stmt->as.for_stmt.cond : stmt->as.while_stmt.cond;
    Block *body=is_for ? stmt->as.for_stmt.body : stmt->as.while_stmt.body;
    size_t count=s->count, initial_generation=s->generation;
    SafetyBinding *initial=safety_snapshot(s,count), *header=safety_snapshot(s,count), *exits=NULL;
    bool has_exit=false;
    size_t exit_generation=0;
    SafetyLoop loop={.parent=s->loop,.count=count};
    s->loop=&loop;
    for (size_t pass=0;;++pass) {
        if (pass>count+2) safety_error(stmt->span,"checked loop-state convergence limit exceeded");
        if (count) memcpy(s->bindings,header,count*sizeof(*header));
        (void)safety_expression(s,condition);
        int known=truth(condition);
        if (known!=1) merge_exit(s,&exits,&has_exit,&exit_generation,count);
        if (!known) break;
        loop.has_continue=false;
        free(loop.continues); loop.continues=NULL;
        bool back=safety_block(s,body);
        if (loop.has_continue) {
            if (back) safety_join(s,loop.continues,count);
            else if (count) memcpy(s->bindings,loop.continues,count*sizeof(*loop.continues));
            if (loop.continue_generation>s->generation) s->generation=loop.continue_generation;
            for (size_t i=0;i<count;++i) s->bindings[i].value.generation=s->generation;
            back=true;
        }
        if (!back) break;
        if (is_for && stmt->as.for_stmt.step) safety_statement(s,stmt->as.for_stmt.step);
        if (initial_generation>s->generation) s->generation=initial_generation;
        safety_join(s,initial,count);
        bool stable=same_states(header,s->bindings,count);
        if (count) memcpy(header,s->bindings,count*sizeof(*header));
        if (stable) break;
    }
    if (loop.has_break) {
        if (count) memcpy(s->bindings,loop.breaks,count*sizeof(*loop.breaks));
        s->generation=loop.break_generation;
        merge_exit(s,&exits,&has_exit,&exit_generation,count);
    }
    if (has_exit && count) memcpy(s->bindings,exits,count*sizeof(*exits));
    if (has_exit) s->generation=exit_generation;
    s->loop=loop.parent;
    safety_check_pending(s,outer);
    s->count=outer;
    free(loop.breaks); free(loop.continues); free(initial); free(header); free(exits);
    return has_exit;
}
