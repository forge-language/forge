#include "ir.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    IRFunction *fn;
    const Program *program;
    IRModule *module_data;
    ForgeStr module;
    uint32_t block_id;
    size_t *scopes;
    size_t scope_count, scope_cap;
    uint32_t *active_locals;
    size_t active_local_count, active_local_cap;
    struct { uint32_t break_to, continue_to; } loops[128];
    size_t loop_count;
    const char *error;
} Lower;

static void *ir_grow(void *p, size_t *cap, size_t count, size_t item_size) {
    if (count <= *cap) return p;
    size_t next = *cap ? *cap * 2 : 8;
    while (next < count) next *= 2;
    void *grown = realloc(p, next * item_size);
    if (!grown) forge_die("out of memory");
    *cap = next;
    return grown;
}

static uint32_t ir_new_block(IRFunction *fn) {
    fn->blocks = ir_grow(fn->blocks, &fn->block_cap, fn->block_count + 1, sizeof(IRBlock));
    uint32_t id = (uint32_t)fn->block_count++;
    memset(&fn->blocks[id], 0, sizeof(IRBlock));
    fn->blocks[id].id = id;
    fn->blocks[id].target = fn->blocks[id].target_false = UINT32_MAX;
    fn->blocks[id].value = IR_NO_VALUE;
    return id;
}

static IRBlock *ir_block(IRFunction *fn, uint32_t id) {
    if (id >= fn->block_count) forge_die("invalid IR block");
    return &fn->blocks[id];
}

static bool ir_op_has_result(IROp op, ForgeType type, bool type_known) {
    if (op == IR_CALL || op == IR_QUAL_CALL)
        return !type_known || type.kind != TY_VOID;
    return type.kind != TY_VOID && op != IR_STORE_LOCAL && op != IR_SPAWN &&
           op != IR_SEND && op != IR_YIELD && op != IR_AWAIT && op != IR_EVAL;
}

static IRValue ir_emit(Lower *l, IRInst inst) {
    IRBlock *b = ir_block(l->fn, l->block_id);
    b->insts = ir_grow(b->insts, &b->inst_cap, b->inst_count + 1, sizeof(IRInst));
    inst.result = ir_op_has_result(inst.op, inst.type, inst.type_known) ? l->fn->next_value++ : IR_NO_VALUE;
    b->insts[b->inst_count++] = inst;
    return inst.result;
}

static void ir_term_jump(Lower *l, uint32_t to) {
    IRBlock *b=ir_block(l->fn,l->block_id);
    if (b->term != IR_TERM_NONE) return;
    b->term = IR_TERM_JUMP; b->target = to;
}

static void ir_term_branch(Lower *l, IRValue cond, uint32_t yes, uint32_t no) {
    IRBlock *b=ir_block(l->fn,l->block_id);
    if (b->term != IR_TERM_NONE) return;
    b->term = IR_TERM_BRANCH; b->value = cond;
    b->target = yes; b->target_false = no;
}

static void ir_term_return(Lower *l, IRValue value, bool has_value) {
    IRBlock *b=ir_block(l->fn,l->block_id);
    if (b->term != IR_TERM_NONE) return;
    b->term = IR_TERM_RETURN; b->value = value; b->has_value = has_value;
}

static bool ir_terminated(const Lower *l) { return ir_block(l->fn,l->block_id)->term != IR_TERM_NONE; }

static void ir_scope_enter(Lower *l) {
    l->scopes = ir_grow(l->scopes, &l->scope_cap, l->scope_count + 1, sizeof(size_t));
    l->scopes[l->scope_count++] = l->active_local_count;
}

static void ir_scope_leave(Lower *l) {
    if (l->scope_count) l->active_local_count = l->scopes[--l->scope_count];
}

static uint32_t ir_local_add(Lower *l, ForgeStr name, ForgeType type) {
    l->fn->locals = ir_grow(l->fn->locals, &l->fn->local_cap, l->fn->local_count + 1, sizeof(IRLocal));
    uint32_t slot = (uint32_t)l->fn->local_count++;
    l->fn->locals[slot] = (IRLocal){name, type};
    l->active_locals = ir_grow(l->active_locals, &l->active_local_cap,
                               l->active_local_count + 1, sizeof(uint32_t));
    l->active_locals[l->active_local_count++] = slot;
    return slot;
}

static int ir_local_find(const Lower *l, ForgeStr name) {
    const IRFunction *fn = l->fn;
    for (size_t i = l->active_local_count; i > 0; i--) {
        uint32_t slot = l->active_locals[i - 1];
        if (forge_str_eq(fn->locals[slot].name, name)) return (int)slot;
    }
    return -1;
}

static const IRGlobal *ir_global_find(const IRModule *m, ForgeStr name) {
    for (size_t i=0;i<m->global_count;i++)
        if (forge_str_eq(m->globals[i].name,name)) return &m->globals[i];
    return NULL;
}

static void ir_lower_block(Lower *l, const Block *block, bool own_scope);
static uint32_t ir_new_open_block(Lower *l);
static IRValue ir_lower_expr(Lower *l, Expr *expr);
static IRValue ir_need_value(Lower *l, IRValue value, const char *context) {
    if (value == IR_NO_VALUE && !l->error) l->error = context;
    return value;
}

static const FnDecl *ir_lookup_fn(const Program *p, ForgeStr module, ForgeStr name, bool qualified) {
    if (qualified) {
        for (size_t m=0;m<p->module_count;m++)
            if (forge_str_eq(p->modules[m].name,module))
                for (size_t f=0;f<p->modules[m].fn_count;f++)
                    if (forge_str_eq(p->modules[m].functions[f].name,name)) return &p->modules[m].functions[f];
        if (p->library.present && forge_str_eq(p->library.name,module))
            for (size_t f=0;f<p->library.fn_count;f++)
                if (forge_str_eq(p->library.functions[f].name,name)) return &p->library.functions[f];
        return NULL;
    }
    for (size_t f=0;f<p->fn_count;f++)
        if (forge_str_eq(p->functions[f].name,name)) return &p->functions[f];
    if (module.len)
        for (size_t m=0;m<p->module_count;m++)
            if (forge_str_eq(p->modules[m].name,module))
                for (size_t f=0;f<p->modules[m].fn_count;f++)
                    if (forge_str_eq(p->modules[m].functions[f].name,name)) return &p->modules[m].functions[f];
    return NULL;
}

static ForgeType ir_expr_type(Lower *l, Expr *e) {
    if (!e) return forge_type_void();
    if (e->type.kind != TY_VOID) return e->type;
    switch (e->kind) {
    case EXPR_IDENT: {
        int slot=ir_local_find(l,e->as.ident);
        if(slot>=0) return l->fn->locals[slot].type;
        const IRGlobal *global=ir_global_find(l->module_data,e->as.ident);
        return global?global->type:forge_type_void();
    }
    case EXPR_CALL: {
        const FnDecl *fn=ir_lookup_fn(l->program,l->module,e->as.call.name,false);
        return fn?fn->ret_type:forge_type_void();
    }
    case EXPR_QUAL_CALL: {
        const FnDecl *fn=ir_lookup_fn(l->program,e->as.qual_call.module,e->as.qual_call.name,true);
        return fn?fn->ret_type:forge_type_void();
    }
    case EXPR_MOVE: return ir_expr_type(l,e->as.move_expr);
    case EXPR_BINARY: {
        if(e->as.binary.op>=BIN_EQ) return forge_type_bool();
        ForgeType left=ir_expr_type(l,e->as.binary.left);
        ForgeType right=ir_expr_type(l,e->as.binary.right);
        if(left.kind==TY_FLOAT||right.kind==TY_FLOAT) return forge_type_float();
        if(left.kind==TY_INT&&right.kind==TY_INT) return forge_type_int();
        return forge_type_void();
    }
    default: return forge_type_void();
    }
}

static IRValue ir_lower_expr(Lower *l, Expr *e) {
    if (!e || l->error) return IR_NO_VALUE;
    IRInst inst = {0};
    inst.result = IR_NO_VALUE; inst.type = ir_expr_type(l,e); inst.type_known = inst.type.kind != TY_VOID;
    inst.a = inst.b = IR_NO_VALUE;
    switch (e->kind) {
    case EXPR_INT:
        inst.op = IR_CONST_INT; inst.int_value = e->as.int_val;
        return ir_emit(l, inst);
    case EXPR_FLOAT:
        inst.op = IR_CONST_FLOAT; inst.float_value = e->as.float_val;
        return ir_emit(l, inst);
    case EXPR_BOOL:
        inst.op = IR_CONST_BOOL; inst.bool_value = e->as.bool_val;
        return ir_emit(l, inst);
    case EXPR_STRING:
        inst.op = IR_CONST_STRING; inst.name = e->as.string_val;
        return ir_emit(l, inst);
    case EXPR_IDENT: {
        int slot = ir_local_find(l, e->as.ident);
        if (slot < 0) {
            const IRGlobal *global=ir_global_find(l->module_data,e->as.ident);
            inst.op = global ? IR_LOAD_GLOBAL : IR_LOAD_SYMBOL;
            inst.name = e->as.ident;
            if(global) { inst.type=global->type; inst.type_known=true; }
        } else { inst.op = IR_LOAD_LOCAL; inst.local = (uint32_t)slot; inst.type = l->fn->locals[slot].type; inst.type_known=inst.type.kind!=TY_VOID; }
        return ir_emit(l, inst);
    }
    case EXPR_BINARY:
        if (e->as.binary.op == BIN_AND || e->as.binary.op == BIN_OR) {
            IRValue left = ir_need_value(l,ir_lower_expr(l,e->as.binary.left),"void or unresolved left operand in logical expression");
            uint32_t short_block = ir_new_open_block(l);
            uint32_t rhs_block = ir_new_open_block(l);
            uint32_t merge_block = ir_new_open_block(l);
            if (e->as.binary.op == BIN_AND) ir_term_branch(l, left, rhs_block, short_block);
            else ir_term_branch(l, left, short_block, rhs_block);
            l->block_id = short_block;
            IRInst literal = {0}; literal.op=IR_CONST_BOOL; literal.type=forge_type_bool(); literal.type_known=true;
            literal.bool_value=e->as.binary.op==BIN_OR;
            IRValue short_value=ir_emit(l,literal);
            ir_term_jump(l,merge_block);
            l->block_id = rhs_block;
            IRValue rhs=ir_need_value(l,ir_lower_expr(l,e->as.binary.right),"void or unresolved right operand in logical expression");
            uint32_t rhs_end=l->block_id;
            ir_term_jump(l,merge_block);
            l->block_id=merge_block;
            IRInst phi={0}; phi.op=IR_PHI; phi.type=forge_type_bool(); phi.type_known=true; phi.a=short_value; phi.b=rhs;
            phi.local=short_block; phi.int_value=rhs_end;
            return ir_emit(l,phi);
        }
        inst.op = IR_BINARY; inst.bin_op = e->as.binary.op;
        inst.a = ir_need_value(l,ir_lower_expr(l,e->as.binary.left),"void or unresolved left operand in binary expression");
        inst.b = ir_need_value(l,ir_lower_expr(l,e->as.binary.right),"void or unresolved right operand in binary expression");
        return ir_emit(l, inst);
    case EXPR_CALL:
    case EXPR_QUAL_CALL: {
        Expr **args = e->kind == EXPR_CALL ? e->as.call.args : e->as.qual_call.args;
        size_t count = e->kind == EXPR_CALL ? e->as.call.arg_count : e->as.qual_call.arg_count;
        inst.op = e->kind == EXPR_CALL ? IR_CALL : IR_QUAL_CALL;
        const FnDecl *signature = e->kind == EXPR_CALL
            ? ir_lookup_fn(l->program,l->module,e->as.call.name,false)
            : ir_lookup_fn(l->program,e->as.qual_call.module,e->as.qual_call.name,true);
        inst.type = signature ? signature->ret_type : ir_expr_type(l,e);
        inst.type_known = signature != NULL || inst.type.kind != TY_VOID;
        inst.name = e->kind == EXPR_CALL ? e->as.call.name : e->as.qual_call.name;
        if (e->kind == EXPR_QUAL_CALL) inst.module = e->as.qual_call.module;
        if (count) {
            inst.args = malloc(count * sizeof(IRValue));
            if (!inst.args) forge_die("out of memory");
            inst.arg_count = count;
            for (size_t i = 0; i < count; i++) inst.args[i] = ir_need_value(l,ir_lower_expr(l,args[i]),"void or unresolved function argument");
        }
        return ir_emit(l, inst);
    }
    case EXPR_RECV:
        inst.op = IR_RECV; return ir_emit(l, inst);
    case EXPR_INDEX:
        inst.op = IR_INDEX; inst.a = ir_need_value(l,ir_lower_expr(l,e->as.index.base),"void or unresolved index base");
        inst.b = ir_need_value(l,ir_lower_expr(l,e->as.index.index),"void or unresolved index expression"); return ir_emit(l, inst);
    case EXPR_FIELD:
        inst.op = IR_FIELD; inst.a = ir_need_value(l,ir_lower_expr(l,e->as.field.base),"void or unresolved field base");
        inst.name = e->as.field.field; return ir_emit(l, inst);
    case EXPR_MOVE:
        inst.op = IR_MOVE; inst.a = ir_need_value(l,ir_lower_expr(l,e->as.move_expr),"void or unresolved moved value"); return ir_emit(l, inst);
    }
    l->error = "unknown expression kind";
    return IR_NO_VALUE;
}

static void ir_lower_stmt(Lower *l, const Stmt *s);

static void ir_lower_block(Lower *l, const Block *block, bool own_scope) {
    if (own_scope) ir_scope_enter(l);
    for (const Stmt *s = block->first; s && !l->error && !ir_terminated(l); s = s->next)
        ir_lower_stmt(l, s);
    if (own_scope) ir_scope_leave(l);
}

static uint32_t ir_new_open_block(Lower *l) { return ir_new_block(l->fn); }

static void ir_lower_if(Lower *l, const Stmt *s) {
    IRValue cond = ir_need_value(l,ir_lower_expr(l,s->as.if_stmt.cond),"void or unresolved if condition");
    uint32_t yes = ir_new_open_block(l), no = ir_new_open_block(l), join = ir_new_open_block(l);
    ir_term_branch(l, cond, yes, no);
    l->block_id = yes;
    ir_lower_block(l, s->as.if_stmt.then_br, true);
    ir_term_jump(l, join);
    l->block_id = no;
    if (s->as.if_stmt.else_br) ir_lower_block(l, s->as.if_stmt.else_br, true);
    ir_term_jump(l, join);
    l->block_id = join;
}

static void ir_lower_while(Lower *l, const Stmt *s) {
    uint32_t cond_block = ir_new_open_block(l), body = ir_new_open_block(l), done = ir_new_open_block(l);
    ir_term_jump(l, cond_block);
    l->block_id = cond_block;
    IRValue cond = ir_need_value(l,ir_lower_expr(l,s->as.while_stmt.cond),"void or unresolved loop condition");
    ir_term_branch(l, cond, body, done);
    if (l->loop_count == sizeof(l->loops) / sizeof(l->loops[0])) { l->error = "loop nesting exceeds 128"; return; }
    l->loops[l->loop_count].break_to=done; l->loops[l->loop_count].continue_to=cond_block; l->loop_count++;
    l->block_id = body;
    ir_lower_block(l, s->as.while_stmt.body, true);
    ir_term_jump(l, cond_block);
    l->loop_count--;
    l->block_id = done;
}

static void ir_lower_for(Lower *l, const Stmt *s) {
    ir_scope_enter(l);
    if (s->as.for_stmt.init) ir_lower_stmt(l, s->as.for_stmt.init);
    uint32_t cond_block = ir_new_open_block(l), body = ir_new_open_block(l), step = ir_new_open_block(l), done = ir_new_open_block(l);
    ir_term_jump(l, cond_block);
    l->block_id = cond_block;
    IRValue cond;
    if (s->as.for_stmt.cond) cond = ir_need_value(l,ir_lower_expr(l,s->as.for_stmt.cond),"void or unresolved for condition");
    else { IRInst one = {0}; one.op=IR_CONST_BOOL; one.type=forge_type_bool(); one.type_known=true; one.bool_value=true; cond=ir_emit(l,one); }
    ir_term_branch(l, cond, body, done);
    if (l->loop_count == sizeof(l->loops) / sizeof(l->loops[0])) { l->error = "loop nesting exceeds 128"; ir_scope_leave(l); return; }
    l->loops[l->loop_count].break_to=done; l->loops[l->loop_count].continue_to=step; l->loop_count++;
    l->block_id = body;
    ir_lower_block(l, s->as.for_stmt.body, true);
    ir_term_jump(l, step);
    l->block_id = step;
    if (s->as.for_stmt.step) ir_lower_stmt(l, s->as.for_stmt.step);
    ir_term_jump(l, cond_block);
    l->loop_count--;
    l->block_id = done;
    ir_scope_leave(l);
}

static void ir_lower_match(Lower *l, const Stmt *s) {
    IRValue key = ir_need_value(l,ir_lower_expr(l,s->as.match_stmt.scrutinee),"void or unresolved match value");
    uint32_t done = ir_new_open_block(l);
    const MatchArm *arm = s->as.match_stmt.arms;
    uint32_t test = l->block_id;
    while (arm && !l->error) {
        l->block_id = test;
        uint32_t body = ir_new_open_block(l);
        uint32_t next = arm->wildcard ? done : ir_new_open_block(l);
        if (!arm->wildcard) {
            IRInst c = {0}; c.op=IR_CONST_INT; c.type=forge_type_int(); c.type_known=true; c.int_value=arm->int_pat;
            IRValue literal=ir_emit(l,c);
            IRInst eq = {0}; eq.op=IR_BINARY; eq.type=forge_type_bool(); eq.type_known=true; eq.bin_op=BIN_EQ; eq.a=key; eq.b=literal;
            IRValue equal=ir_emit(l,eq);
            ir_term_branch(l,equal,body,next);
        } else ir_term_jump(l, body);
        l->block_id=body;
        ir_lower_block(l,arm->body,true);
        ir_term_jump(l,done);
        test=next;
        if (arm->wildcard) break;
        arm=arm->next;
    }
    if (arm) { /* A wildcard arm consumes the remaining chain. */ }
    else if (test != done) { l->block_id=test; ir_term_jump(l,done); }
    l->block_id=done;
}

static void ir_lower_stmt(Lower *l, const Stmt *s) {
    if (!s || l->error || ir_terminated(l)) return;
    IRInst inst = {0}; inst.result=IR_NO_VALUE; inst.a=inst.b=IR_NO_VALUE;
    switch (s->kind) {
    case STMT_LET: {
        uint32_t slot=ir_local_add(l,s->as.let.name,s->as.let.type);
        if (s->as.let.init) {
            IRValue value=ir_need_value(l,ir_lower_expr(l,s->as.let.init),"void or unresolved initializer");
            inst.op=IR_STORE_LOCAL; inst.local=slot; inst.a=value; ir_emit(l,inst);
        }
        break;
    }
    case STMT_EXPR:
        inst.op=IR_EVAL; inst.a=ir_lower_expr(l,s->as.expr); ir_emit(l,inst); break;
    case STMT_RETURN:
        ir_term_return(l,s->as.ret?ir_need_value(l,ir_lower_expr(l,s->as.ret),"void or unresolved return value"):IR_NO_VALUE,s->as.ret!=NULL); break;
    case STMT_IF: ir_lower_if(l,s); break;
    case STMT_WHILE: ir_lower_while(l,s); break;
    case STMT_FOR: ir_lower_for(l,s); break;
    case STMT_ASSIGN: {
        int slot=ir_local_find(l,s->as.assign.name);
        IRValue value=ir_need_value(l,ir_lower_expr(l,s->as.assign.value),"void or unresolved assignment value");
        if(slot<0) { inst.op=IR_LOAD_SYMBOL; inst.name=s->as.assign.name; inst.type=s->as.assign.value->type; inst.a=value; l->error="assignment to non-local symbols is not supported by this IR"; break; }
        inst.op=IR_STORE_LOCAL; inst.local=(uint32_t)slot; inst.a=value; ir_emit(l,inst); break;
    }
    case STMT_BREAK:
        if (!l->loop_count) { l->error="break outside a loop"; break; }
        ir_term_jump(l,l->loops[l->loop_count-1].break_to); break;
    case STMT_CONTINUE:
        if (!l->loop_count) { l->error="continue outside a loop"; break; }
        ir_term_jump(l,l->loops[l->loop_count-1].continue_to); break;
    case STMT_BLOCK: ir_lower_block(l,s->as.block,true); break;
    case STMT_MATCH: ir_lower_match(l,s); break;
    case STMT_SPAWN:
        inst.op=IR_SPAWN; inst.name=s->as.spawn.coro_name; inst.arg_count=s->as.spawn.arg_count;
        if(inst.arg_count) { inst.args=malloc(inst.arg_count*sizeof(IRValue)); if(!inst.args) forge_die("out of memory"); for(size_t i=0;i<inst.arg_count;i++) inst.args[i]=ir_need_value(l,ir_lower_expr(l,s->as.spawn.args[i]),"void or unresolved spawn argument"); }
        ir_emit(l,inst); break;
    case STMT_SEND:
        inst.op=IR_SEND; inst.a=ir_need_value(l,ir_lower_expr(l,s->as.send.target),"void or unresolved send target"); inst.b=ir_need_value(l,ir_lower_expr(l,s->as.send.value),"void or unresolved send value"); inst.int_value=s->as.send.tag; inst.bool_value=s->as.send.move_; ir_emit(l,inst); break;
    case STMT_YIELD: inst.op=IR_YIELD; ir_emit(l,inst); break;
    case STMT_AWAIT: inst.op=IR_AWAIT; inst.a=ir_need_value(l,ir_lower_expr(l,s->as.await_expr),"void or unresolved await value"); inst.type=forge_type_void(); ir_emit(l,inst); break;
    }
}

static bool ir_lower_function(IRModule *m, const Program *program, ForgeStr module_name, const FnDecl *decl, const char **error) {
    m->functions=ir_grow(m->functions,&m->function_cap,m->function_count+1,sizeof(IRFunction));
    IRFunction *fn=&m->functions[m->function_count++]; memset(fn,0,sizeof(*fn));
    fn->name=decl->name; fn->module=module_name; fn->return_type=decl->ret_type; fn->is_extern=decl->is_extern;
    if(fn->is_extern) return true;
    uint32_t entry=ir_new_block(fn);
    Lower l={0}; l.fn=fn; l.program=program; l.module_data=m; l.module=module_name; l.block_id=entry;
    ir_scope_enter(&l);
    for(const Param *p=decl->params;p;p=p->next) {
        uint32_t slot=ir_local_add(&l,p->name,p->type);
        IRInst arg={0}; arg.op=IR_PARAM; arg.type=p->type; arg.type_known=true; arg.local=slot;
        IRValue v=ir_emit(&l,arg);
        IRInst store={0}; store.op=IR_STORE_LOCAL; store.result=IR_NO_VALUE; store.local=slot; store.a=v; ir_emit(&l,store);
    }
    ir_lower_block(&l,&decl->body,false);
    if(!ir_terminated(&l)) ir_term_return(&l,IR_NO_VALUE,false);
    ir_scope_leave(&l);
    free(l.scopes);
    free(l.active_locals);
    if(l.error) { *error=l.error; return false; }
    return true;
}

static bool ir_copy_globals(const Program *program, IRModule *out, const char **error) {
    for(size_t i=0;i<program->const_count;i++) {
        const ConstDecl *decl=&program->consts[i];
        if(!decl->value) { *error="constant has no initializer"; return false; }
        out->globals=ir_grow(out->globals,&out->global_cap,out->global_count+1,sizeof(IRGlobal));
        IRGlobal *g=&out->globals[out->global_count++]; memset(g,0,sizeof(*g));
        g->name=decl->name;
        switch(decl->value->kind) {
        case EXPR_INT: g->kind=IR_GLOBAL_INT; g->type=forge_type_int(); g->int_value=decl->value->as.int_val; break;
        case EXPR_FLOAT: g->kind=IR_GLOBAL_FLOAT; g->type=forge_type_float(); g->float_value=decl->value->as.float_val; break;
        case EXPR_BOOL: g->kind=IR_GLOBAL_BOOL; g->type=forge_type_bool(); g->bool_value=decl->value->as.bool_val; break;
        case EXPR_STRING: g->kind=IR_GLOBAL_STRING; g->type=forge_type_string(); g->string_value=decl->value->as.string_val; break;
        default: *error="constant initializer is not a literal"; return false;
        }
    }
    for(size_t i=0;i<program->struct_count;i++) {
        const StructDecl *decl=&program->structs[i];
        out->structs=ir_grow(out->structs,&out->struct_cap,out->struct_count+1,sizeof(IRStruct));
        IRStruct *result=&out->structs[out->struct_count++]; memset(result,0,sizeof(*result));
        result->name=decl->name;
        for(const Field *f=decl->fields;f;f=f->next) result->field_count++;
        if(result->field_count) {
            result->fields=calloc(result->field_count,sizeof(IRLocal));
            if(!result->fields) forge_die("out of memory");
            size_t field=0;
            for(const Field *f=decl->fields;f;f=f->next) result->fields[field++]=(IRLocal){f->name,f->type};
        }
    }
    for(size_t i=0;i<program->enum_count;i++) {
        const EnumDecl *decl=&program->enums[i];
        out->enums=ir_grow(out->enums,&out->enum_cap,out->enum_count+1,sizeof(IREnum));
        IREnum *result=&out->enums[out->enum_count++]; memset(result,0,sizeof(*result));
        result->name=decl->name;
        for(const EnumVariant *v=decl->variants;v;v=v->next) result->variant_count++;
        if(result->variant_count) {
            result->variants=calloc(result->variant_count,sizeof(ForgeStr));
            result->values=calloc(result->variant_count,sizeof(int64_t));
            if(!result->variants||!result->values) forge_die("out of memory");
            size_t variant=0;
            for(const EnumVariant *v=decl->variants;v;v=v->next) {
                size_t n=decl->name.len+1+v->name.len;
                char *joined=malloc(n+1); if(!joined) forge_die("out of memory");
                memcpy(joined,decl->name.data,decl->name.len); joined[decl->name.len]='_';
                memcpy(joined+decl->name.len+1,v->name.data,v->name.len); joined[n]='\0';
                result->variants[variant]=(ForgeStr){joined,n};
                result->values[variant++]=v->value;
                out->globals=ir_grow(out->globals,&out->global_cap,out->global_count+1,sizeof(IRGlobal));
                IRGlobal *g=&out->globals[out->global_count++]; memset(g,0,sizeof(*g));
                g->name=result->variants[variant-1]; g->owned_name=joined;
                g->kind=IR_GLOBAL_INT; g->type=forge_type_struct(decl->name); g->int_value=v->value;
            }
        }
    }
    return true;
}

bool ir_lower_program(const Program *program, IRModule *out) {
    memset(out,0,sizeof(*out));
    if(program->process_count || program->supervisor_count) {
        fprintf(stderr,"forge: --emit-ir does not support processes, coroutines, or supervisors\n");
        return false;
    }
    const char *error=NULL;
    if(!ir_copy_globals(program,out,&error)) goto fail;
    for(size_t i=0;i<program->fn_count;i++) if(!ir_lower_function(out,program,forge_str(""),&program->functions[i],&error)) goto fail;
    for(size_t i=0;i<program->native_count;i++) {
        FnDecl entry={0};
        entry.name=program->natives[i].name;
        entry.ret_type=forge_type_int();
        entry.body=program->natives[i].body;
        if(!ir_lower_function(out,program,forge_str(""),&entry,&error)) goto fail;
        out->functions[out->function_count-1].is_native=true;
    }
    for(size_t m=0;m<program->module_count;m++) for(size_t i=0;i<program->modules[m].fn_count;i++)
        if(!ir_lower_function(out,program,program->modules[m].name,&program->modules[m].functions[i],&error)) goto fail;
    if(program->library.present) for(size_t i=0;i<program->library.fn_count;i++)
        if(!ir_lower_function(out,program,program->library.name,&program->library.functions[i],&error)) goto fail;
    return true;
fail:
    fprintf(stderr,"forge: --emit-ir: %s\n",error?error:"lowering failed");
    ir_module_free(out);
    return false;
}

static const char *ir_type_name(ForgeType t) {
    switch(t.kind) { case TY_VOID:return "void"; case TY_INT:return "i64"; case TY_FLOAT:return "f64"; case TY_BOOL:return "bool"; case TY_STRING:return "string"; case TY_PTR:return "ptr"; case TY_STRUCT:return "struct"; }
    return "?";
}
static void ir_dump_type(FILE *out, ForgeType t) {
    if(t.kind==TY_STRUCT) fprintf(out,"struct %.*s",(int)t.struct_name.len,t.struct_name.data);
    else fputs(ir_type_name(t),out);
}
static const char *ir_bin_name(BinOp op) {
    static const char *names[]={"add","sub","mul","div","mod","eq","ne","lt","le","gt","ge","and","or"};
    return (unsigned)op<sizeof(names)/sizeof(names[0])?names[op]:"?";
}
static void ir_dump_value(FILE *out,IRValue v) { if(v==IR_NO_VALUE) fputs("none",out); else fprintf(out,"%%%u",v); }
static void ir_dump_fn(const IRFunction *fn,FILE *out) {
    if(fn->module.len) fprintf(out,"module %.*s::",(int)fn->module.len,fn->module.data);
    fprintf(out,"fn %.*s -> ",(int)fn->name.len,fn->name.data); ir_dump_type(out,fn->return_type); fprintf(out,"%s\n",fn->is_extern?" extern":"");
    for(size_t i=0;i<fn->local_count;i++) { fprintf(out,"  local %%%zu %.*s: ",i,(int)fn->locals[i].name.len,fn->locals[i].name.data); ir_dump_type(out,fn->locals[i].type); fputc('\n',out); }
    for(size_t bi=0;bi<fn->block_count;bi++) {
        const IRBlock *b=&fn->blocks[bi]; fprintf(out,"b%u:\n",b->id);
        for(size_t ii=0;ii<b->inst_count;ii++) {
            const IRInst *x=&b->insts[ii]; fputs("  ",out); if(x->result!=IR_NO_VALUE) { ir_dump_value(out,x->result); fprintf(out," = "); }
            switch(x->op) {
            case IR_CONST_INT: fprintf(out,"const.i64 %" PRId64,x->int_value); break;
            case IR_CONST_FLOAT: fprintf(out,"const.f64 %.17g",x->float_value); break;
            case IR_CONST_BOOL: fprintf(out,"const.bool %s",x->bool_value?"true":"false"); break;
            case IR_CONST_STRING:
                fputs("const.string \"",out);
                for(size_t c=0;c<x->name.len;c++) {
                    unsigned char ch=(unsigned char)x->name.data[c];
                    if(ch=='\\' || ch=='\"') { fputc('\\',out); fputc(ch,out); }
                    else if(ch>=32 && ch<127) fputc(ch,out);
                    else fprintf(out,"\\x%02x",ch);
                }
                fputc('\"',out); break;
            case IR_LOAD_LOCAL: fprintf(out,"load.local %%%u",x->local); break;
            case IR_LOAD_GLOBAL: fprintf(out,"load.global @%.*s",(int)x->name.len,x->name.data); break;
            case IR_LOAD_SYMBOL: fprintf(out,"load.symbol %.*s",(int)x->name.len,x->name.data); break;
            case IR_PARAM: fprintf(out,"param %%%u",x->local); break;
            case IR_BINARY: fprintf(out,"%s ",ir_bin_name(x->bin_op)); ir_dump_value(out,x->a); fputs(", ",out); ir_dump_value(out,x->b); break;
            case IR_PHI: fputs("phi ",out); ir_dump_value(out,x->a); fprintf(out," from b%u, ",x->local); ir_dump_value(out,x->b); fprintf(out," from b%" PRId64,x->int_value); break;
            case IR_CALL: fprintf(out,"call %.*s(",(int)x->name.len,x->name.data); goto dump_args;
            case IR_QUAL_CALL: fprintf(out,"call %.*s::%.*s(",(int)x->module.len,x->module.data,(int)x->name.len,x->name.data); dump_args: for(size_t a=0;a<x->arg_count;a++){if(a)fputs(", ",out);ir_dump_value(out,x->args[a]);} fputc(')',out); break;
            case IR_RECV: fputs("recv",out); break;
            case IR_INDEX: fputs("index ",out);ir_dump_value(out,x->a);fputs(", ",out);ir_dump_value(out,x->b);break;
            case IR_FIELD: fputs("field ",out);ir_dump_value(out,x->a);fprintf(out,".%.*s",(int)x->name.len,x->name.data);break;
            case IR_MOVE: fputs("move ",out);ir_dump_value(out,x->a);break;
            case IR_STORE_LOCAL: fprintf(out,"store.local %%%u, ",x->local);ir_dump_value(out,x->a);break;
            case IR_SPAWN: fprintf(out,"spawn %.*s(",(int)x->name.len,x->name.data);for(size_t a=0;a<x->arg_count;a++){if(a)fputs(", ",out);ir_dump_value(out,x->args[a]);}fputc(')',out);break;
            case IR_SEND: fputs("send ",out);ir_dump_value(out,x->a);fputs(", ",out);ir_dump_value(out,x->b);fprintf(out," tag=%" PRId64 " move=%s",x->int_value,x->bool_value?"true":"false");break;
            case IR_YIELD: fputs("yield",out);break;
            case IR_AWAIT: fputs("await ",out);ir_dump_value(out,x->a);break;
            case IR_EVAL: fputs("eval ",out);ir_dump_value(out,x->a);break;
            }
            if(x->result != IR_NO_VALUE)
                { fputs(" : ",out); if(x->type_known) ir_dump_type(out,x->type); else fputc('?',out); fputc('\n',out); }
            else fputc('\n',out);
        }
        fputs("  ",out);
        switch(b->term) {
        case IR_TERM_NONE:fputs("unreachable",out);break;
        case IR_TERM_JUMP:fprintf(out,"br b%u",b->target);break;
        case IR_TERM_BRANCH:fputs("br_if ",out);ir_dump_value(out,b->value);fprintf(out,", b%u, b%u",b->target,b->target_false);break;
        case IR_TERM_RETURN:fputs("ret",out);if(b->has_value){fputc(' ',out);ir_dump_value(out,b->value);}break;
        }
        fputc('\n',out);
    }
    fputc('\n',out);
}
static void ir_dump_string(FILE *out, ForgeStr s) {
    fputc('"',out);
    for(size_t i=0;i<s.len;i++) {
        unsigned char ch=(unsigned char)s.data[i];
        if(ch=='\\'||ch=='"') { fputc('\\',out); fputc(ch,out); }
        else if(ch>=32&&ch<127) fputc(ch,out);
        else fprintf(out,"\\x%02x",ch);
    }
    fputc('"',out);
}
void ir_dump(const IRModule *m,FILE *out) {
    for(size_t i=0;i<m->global_count;i++) {
        const IRGlobal *g=&m->globals[i];
        fprintf(out,"global @%.*s: ",(int)g->name.len,g->name.data); ir_dump_type(out,g->type); fputs(" = ",out);
        switch(g->kind) {
        case IR_GLOBAL_INT: fprintf(out,"%" PRId64,g->int_value); break;
        case IR_GLOBAL_FLOAT: fprintf(out,"%.17g",g->float_value); break;
        case IR_GLOBAL_BOOL: fputs(g->bool_value?"true":"false",out); break;
        case IR_GLOBAL_STRING: ir_dump_string(out,g->string_value); break;
        }
        fputc('\n',out);
    }
    for(size_t i=0;i<m->struct_count;i++) {
        const IRStruct *st=&m->structs[i]; fprintf(out,"struct %.*s {",(int)st->name.len,st->name.data);
        for(size_t f=0;f<st->field_count;f++) { fprintf(out," %.*s: ",(int)st->fields[f].name.len,st->fields[f].name.data); ir_dump_type(out,st->fields[f].type); fputc(';',out); }
        fputs(" }\n",out);
    }
    for(size_t i=0;i<m->enum_count;i++) {
        const IREnum *en=&m->enums[i]; fprintf(out,"enum %.*s {",(int)en->name.len,en->name.data);
        for(size_t v=0;v<en->variant_count;v++) fprintf(out," %.*s=%" PRId64 ";",(int)en->variants[v].len-(int)en->name.len-1,en->variants[v].data+en->name.len+1,en->values[v]);
        fputs(" }\n",out);
    }
    if(m->global_count||m->struct_count||m->enum_count) fputc('\n',out);
    for(size_t i=0;i<m->function_count;i++) ir_dump_fn(&m->functions[i],out);
}
void ir_module_free(IRModule *m) {
    for(size_t f=0;f<m->function_count;f++) {
        IRFunction *fn=&m->functions[f];
        for(size_t b=0;b<fn->block_count;b++) { for(size_t i=0;i<fn->blocks[b].inst_count;i++) free(fn->blocks[b].insts[i].args); free(fn->blocks[b].insts); }
        free(fn->blocks); free(fn->locals);
    }
    for(size_t i=0;i<m->global_count;i++) free(m->globals[i].owned_name);
    for(size_t i=0;i<m->struct_count;i++) free(m->structs[i].fields);
    for(size_t i=0;i<m->enum_count;i++) { free(m->enums[i].variants); free(m->enums[i].values); }
    free(m->globals); free(m->structs); free(m->enums);
    free(m->functions); memset(m,0,sizeof(*m));
}
