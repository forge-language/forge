#include "semantic.h"
#include "mod_registry.h"

/* Unknown is deliberately distinct from every language type: imported binary
 * APIs do not expose a typed Forge AST. Never guess signatures from names. */
#define UNKNOWN ((ForgeTypeKind)-1)
#define FUNCTION_REFERENCE ((ForgeTypeKind)-2)
typedef struct { ForgeStr name; ForgeType type; bool initialized, owned; } Local;
typedef struct {
    Program *program;
    ForgeStr module;
    ProcessDecl *process;
    Local *locals;
    size_t count, capacity;
    ForgeType result;
    unsigned loops, depth;
    bool coroutine;
} Check;

static void error(ForgeStr name, const char *reason) {
    fprintf(stderr, "forge: semantic: %s '%.*s'\n", reason, (int)name.len, name.data);
    exit(1);
}
static ForgeType unknown(void) { ForgeType t = { .kind = UNKNOWN }; return t; }
static bool numeric(ForgeType t) {
    return t.kind == UNKNOWN || t.kind == TY_INT || t.kind == TY_FLOAT || t.kind == TY_BOOL;
}
static bool compatible(ForgeType to, ForgeType from) {
    if (to.kind == FUNCTION_REFERENCE || from.kind == FUNCTION_REFERENCE) return false;
    if (to.kind == UNKNOWN || from.kind == UNKNOWN) return true;
    if (to.kind == TY_FLOAT && numeric(from)) return true;
    if ((to.kind == TY_INT || to.kind == TY_BOOL) &&
        (from.kind == TY_INT || from.kind == TY_BOOL)) return true;
    return to.kind == from.kind &&
        (to.kind != TY_STRUCT || forge_str_eq(to.struct_name, from.struct_name));
}
static void require(ForgeType to, ForgeType from, ForgeStr name) {
    if (!compatible(to, from)) error(name, "type mismatch for");
}
static Local *local(Check *c, ForgeStr name) {
    for (size_t i = c->count; i; --i)
        if (forge_str_eq(c->locals[i-1].name, name)) return &c->locals[i-1];
    return NULL;
}
static void declare(Check *c, ForgeStr name, ForgeType t, bool initialized) {
    if (c->count == c->capacity) {
        c->capacity = c->capacity ? c->capacity * 2 : 16;
        Local *grown = realloc(c->locals, c->capacity * sizeof(*grown));
        if (!grown) forge_die("out of memory");
        c->locals = grown;
    }
    c->locals[c->count++] = (Local){name, t, initialized, false};
}
static FnDecl *find_fn(FnDecl *fns, size_t count, ForgeStr name) {
    for (size_t i = 0; i < count; ++i)
        if (forge_str_eq(fns[i].name, name)) return &fns[i];
    return NULL;
}
static FnDecl *resolve(Check *c, ForgeStr module, ForgeStr name) {
    Program *p = c->program;
    if (module.len) {
        for (size_t i = 0; i < p->module_count; ++i)
            if (forge_str_eq(p->modules[i].name, module))
                return find_fn(p->modules[i].functions, p->modules[i].fn_count, name);
        if (p->library.present && forge_str_eq(p->library.name, module))
            return find_fn(p->library.functions, p->library.fn_count, name);
        return NULL;
    }
    FnDecl *fn = find_fn(p->functions, p->fn_count, name);
    if (!fn && p->library.present)
        fn = find_fn(p->library.functions, p->library.fn_count, name);
    return fn;
}
static ForgeType expression(Check *, Expr *);
/* Callback ABIs currently use int/ptr handles. Keep their compatibility scoped
 * to argument positions; a function reference is never a scalar value. A source
 * wrapper is eligible only when its parameter is forwarded directly to an
 * opaque extern/binary slot. This is not a substitute for function types. */
static bool callback_forwarded(Check *, Block *, ForgeStr, unsigned);
static bool references_parameter(Expr *e,ForgeStr name,unsigned depth) {
    if (!e || depth>32) return depth>32;
    switch (e->kind) {
    case EXPR_IDENT: return forge_str_eq(e->as.ident,name);
    case EXPR_BINARY: return references_parameter(e->as.binary.left,name,depth+1) ||
        references_parameter(e->as.binary.right,name,depth+1);
    case EXPR_CALL:
        for (size_t i=0;i<e->as.call.arg_count;++i)
            if (references_parameter(e->as.call.args[i],name,depth+1)) return true;
        return false;
    case EXPR_QUAL_CALL:
        for (size_t i=0;i<e->as.qual_call.arg_count;++i)
            if (references_parameter(e->as.qual_call.args[i],name,depth+1)) return true;
        return false;
    case EXPR_INDEX: return references_parameter(e->as.index.base,name,depth+1) ||
        references_parameter(e->as.index.index,name,depth+1);
    case EXPR_FIELD: return references_parameter(e->as.field.base,name,depth+1);
    case EXPR_MOVE: return references_parameter(e->as.move_expr,name,depth+1);
    default: return false;
    }
}
static bool callback_expression(Check *c, Expr *e, ForgeStr parameter, unsigned depth) {
    if (!e || depth > 32) return false;
    ForgeStr module=forge_str(""), name=forge_str("");
    Expr **args=NULL; size_t count=0;
    if (e->kind==EXPR_CALL) { name=e->as.call.name; args=e->as.call.args; count=e->as.call.arg_count; }
    else if (e->kind==EXPR_QUAL_CALL) { module=e->as.qual_call.module; name=e->as.qual_call.name; args=e->as.qual_call.args; count=e->as.qual_call.arg_count; }
    else return false;
    FnDecl *callee=resolve(c,module.len ? module : c->module,name);
    if (!callee && !module.len) callee=resolve(c,forge_str(""),name);
    Param *slot=callee ? callee->params : NULL;
    bool binary=false,forwarded=false;
    if (module.len && !forge_import_is_file_module(c->program,module))
        for (size_t i=0;i<c->program->import_count;++i)
            if (forge_str_eq(module,c->program->imports[i]) && !forge_import_is_stdlib(module)) binary=true;
    for (size_t i=0;i<count;++i) {
        if (args[i]->kind==EXPR_IDENT && forge_str_eq(args[i]->as.ident,parameter)) {
            bool accepted=binary;
            if (slot && (slot->type.kind==TY_INT || slot->type.kind==TY_PTR)) {
                if (callee->is_extern) accepted=true;
                else {
                    ForgeStr saved=c->module; if (module.len) c->module=module;
                    accepted=callback_forwarded(c,&callee->body,slot->name,depth+1);
                    c->module=saved;
                }
            }
            if (!accepted) return false;
            forwarded=true;
        } else if (references_parameter(args[i],parameter,depth+1)) return false;
        if (slot) slot=slot->next;
    }
    return forwarded;
}
static bool callback_forwarded(Check *c, Block *body, ForgeStr parameter, unsigned depth) {
    if (!body || depth>32 || !body->first || body->first->next) return false;
    /* Only a transparent forwarding wrapper can preserve an opaque callback:
     * no local shadowing, arithmetic, additional statements or hidden uses. */
    Stmt *s=body->first;
    if (s->kind==STMT_RETURN) return callback_expression(c,s->as.ret,parameter,depth);
    if (s->kind==STMT_EXPR) return callback_expression(c,s->as.expr,parameter,depth);
    return false;
}
static void arguments(Check *c, ForgeStr name, Param *params, Expr **args, size_t count,
                      FnDecl *callee, ForgeStr module) {
    size_t expected = 0;
    for (Param *p = params; p; p = p->next) ++expected;
    if (count != expected) error(name, "wrong argument count for");
    for (size_t i = 0; params; params = params->next, ++i) {
        ForgeType value=expression(c,args[i]);
        if (value.kind==FUNCTION_REFERENCE && callee &&
            (params->type.kind==TY_INT || params->type.kind==TY_PTR)) {
            ForgeStr saved=c->module; if (module.len) c->module=module;
            bool callback=callee->is_extern || callback_forwarded(c,&callee->body,params->name,0);
            c->module=saved;
            if (callback) continue;
        }
        require(params->type,value,name);
    }
}
static ForgeType call(Check *c, ForgeStr module, ForgeStr name, Expr **args, size_t count) {
    if (!module.len && (forge_str_eq(name, forge_str("thread_spawn")) ||
                        forge_str_eq(name, forge_str("thread_spawn_indexed")))) {
        if (count != 2) error(name, "wrong argument count for");
        if (args[0]->kind != EXPR_IDENT || local(c, args[0]->as.ident))
            error(name, "requires a function identifier for");
        FnDecl *callback = resolve(c, forge_str(""), args[0]->as.ident);
        if (!callback) callback = resolve(c, c->module, args[0]->as.ident);
        if (!callback || callback->ret_type.kind != TY_INT)
            error(name, "requires an integer callback for");
        size_t arity = 0;
        for (Param *p = callback->params; p; p = p->next) {
            if (p->type.kind != TY_INT) error(name, "requires integer callback parameters for");
            ++arity;
        }
        size_t required = forge_str_eq(name, forge_str("thread_spawn")) ? 1 : 2;
        if (arity != required) error(name, "wrong callback argument count for");
        require(forge_type_int(), expression(c, args[1]), name);
        return forge_type_int();
    }
    FnDecl *fn = NULL;
    const char *standard = module.len ? NULL :
        forge_std_c_name(name, c->program->imports, c->program->import_count);
    if (!standard && !module.len && c->program->library.present)
        standard = forge_std_c_name(name, c->program->library.imports, c->program->library.import_count);
    if (!standard) {
        fn = resolve(c, module.len ? module : c->module, name);
        if (!fn && !module.len) fn = resolve(c, forge_str(""), name);
    }
    if (fn) {
        arguments(c, name, fn->params, args, count, fn, module);
        return fn->ret_type;
    }
    bool builtin = !module.len && (forge_str_eq(name, forge_str("println")) ||
        forge_str_eq(name, forge_str("print")) || forge_str_eq(name, forge_str("print_int")) ||
        forge_str_eq(name, forge_str("print_str")) || forge_str_eq(name, forge_str("eprint")) ||
        forge_str_eq(name, forge_str("eprintln")));
    bool binary = false;
    if (module.len && !forge_import_is_file_module(c->program, module)) {
        for (size_t i = 0; i < c->program->import_count; ++i)
            if (forge_str_eq(module, c->program->imports[i]) && !forge_import_is_stdlib(module)) binary = true;
    }
    if (!standard && !builtin && !binary) error(name, "unknown function");
    for (size_t i = 0; i < count; ++i) {
        ForgeType t = expression(c, args[i]);
        if (t.kind == TY_VOID) error(name, "void argument for");
        if (t.kind == FUNCTION_REFERENCE && !binary) error(name, "function reference is not a scalar argument for");
    }
    return builtin ? forge_type_void() : unknown();
}
static ForgeType expression(Check *c, Expr *e) {
    if (!e) return forge_type_void();
    if (++c->depth > 2000) forge_die("semantic expression nesting limit exceeded");
    ForgeType t = unknown();
    switch (e->kind) {
    case EXPR_INT: t = forge_type_int(); break;
    case EXPR_FLOAT: t = forge_type_float(); break;
    case EXPR_BOOL: t = forge_type_bool(); break;
    case EXPR_STRING: t = forge_type_string(); break;
    case EXPR_IDENT: {
        Local *l = local(c, e->as.ident);
        if (l) {
            if (!l->initialized) error(e->as.ident, l->owned ? "use of moved owned value" : "uninitialized value");
            t = l->type; break;
        }
        for (size_t i = 0; i < c->program->const_count; ++i)
            if (forge_str_eq(c->program->consts[i].name, e->as.ident)) {
                t = expression(c, c->program->consts[i].value); goto done;
            }
        /* Function identifiers are accepted as callback references. */
        if (resolve(c, c->module, e->as.ident) || resolve(c, forge_str(""), e->as.ident)) { t.kind=FUNCTION_REFERENCE; break; }
        error(e->as.ident, "unknown value"); break;
    }
    case EXPR_CALL:
        t = call(c, forge_str(""), e->as.call.name, e->as.call.args, e->as.call.arg_count); break;
    case EXPR_QUAL_CALL:
        t = call(c, e->as.qual_call.module, e->as.qual_call.name, e->as.qual_call.args, e->as.qual_call.arg_count); break;
    case EXPR_BINARY: {
        ForgeType a = expression(c, e->as.binary.left), b = expression(c, e->as.binary.right);
        if (!numeric(a) || !numeric(b)) error(forge_str("operator"), "non-numeric operand for");
        if (e->as.binary.op == BIN_MOD && (a.kind == TY_FLOAT || b.kind == TY_FLOAT))
            error(forge_str("%"), "non-integer operand for");
        t = e->as.binary.op >= BIN_EQ ? forge_type_bool() :
            a.kind == TY_FLOAT || b.kind == TY_FLOAT ? forge_type_float() :
            a.kind == UNKNOWN || b.kind == UNKNOWN ? unknown() : forge_type_int();
        break;
    }
    case EXPR_RECV:
        if (!c->coroutine) error(forge_str("recv"), "requires coroutine context for");
        t = forge_type_int(); break;
    case EXPR_INDEX: {
        ForgeType base = expression(c, e->as.index.base);
        require(forge_type_int(), expression(c, e->as.index.index), forge_str("index"));
        if (base.kind != TY_STRING && base.kind != TY_PTR && base.kind != UNKNOWN)
            error(forge_str("index"), "invalid base for");
        t = forge_type_int(); break;
    }
    case EXPR_FIELD: {
        /* JS callbacks use module.function field syntax in expression position. */
        if (e->as.field.base->kind == EXPR_IDENT &&
            !local(c, e->as.field.base->as.ident) &&
            resolve(c, e->as.field.base->as.ident, e->as.field.field)) { t.kind=FUNCTION_REFERENCE; break; }
        ForgeType base = expression(c, e->as.field.base);
        if (base.kind == UNKNOWN) break;
        if (base.kind != TY_STRUCT) error(e->as.field.field, "field access on non-struct");
        bool found = false;
        for (size_t i = 0; i < c->program->struct_count; ++i)
            if (forge_str_eq(base.struct_name, c->program->structs[i].name))
                for (Field *f = c->program->structs[i].fields; f; f = f->next)
                    if (forge_str_eq(f->name, e->as.field.field)) { t = f->type; found = true; }
        if (!found) error(e->as.field.field, "unknown field");
        break;
    }
    case EXPR_MOVE:
        error(forge_str("move"), "ownership move expressions require unsupported ownership result types; use send-move for"); break;
    }
done:
    --c->depth;
    return t;
}
static void block(Check *, Block *);
enum { FLOW_NEXT=1, FLOW_RETURN=2, FLOW_BREAK=4, FLOW_CONTINUE=8 };
static unsigned flow_block(Block *, unsigned);
static bool *snapshot(Check *c) {
    bool *state = malloc((c->count + 1) * sizeof(bool));
    if (!state) forge_die("out of memory");
    for (size_t i = 0; i < c->count; ++i) state[i] = c->locals[i].initialized;
    return state;
}
static void restore(Check *c, bool *state, size_t count) {
    for (size_t i = 0; i < count; ++i) c->locals[i].initialized = state[i];
}
static void condition(Check *c, Expr *e) {
    if (e && !numeric(expression(c, e))) error(forge_str("condition"), "non-numeric");
}
static void statement(Check *c, Stmt *s) {
    switch (s->kind) {
    case STMT_LET:
        if (s->as.let.init) require(s->as.let.type, expression(c, s->as.let.init), s->as.let.name);
        if (s->as.let.owned_ && (s->as.let.type.kind!=TY_STRING || !s->as.let.init))
            error(s->as.let.name,"owned bindings require initialized strings for");
        declare(c, s->as.let.name, s->as.let.type, s->as.let.init != NULL);
        c->locals[c->count-1].owned=s->as.let.owned_; break;
    case STMT_ASSIGN: {
        Local *l = local(c, s->as.assign.name);
        if (!l) error(s->as.assign.name, "assignment to unknown value");
        require(l->type, expression(c, s->as.assign.value), s->as.assign.name);
        l->initialized = true; break;
    }
    case STMT_EXPR: expression(c, s->as.expr); break;
    case STMT_RETURN: require(c->result, expression(c, s->as.ret), forge_str("return")); break;
    case STMT_IF: {
        condition(c, s->as.if_stmt.cond);
        size_t n = c->count;
        bool *before = snapshot(c);
        block(c, s->as.if_stmt.then_br);
        bool *then = snapshot(c);
        restore(c, before, n);
        if (s->as.if_stmt.else_br) block(c, s->as.if_stmt.else_br);
        unsigned then_flow=flow_block(s->as.if_stmt.then_br,0);
        unsigned else_flow=s->as.if_stmt.else_br ? flow_block(s->as.if_stmt.else_br,0) : FLOW_NEXT;
        for (size_t i = 0; i < n; ++i) {
            /* A moved handle on a break/continue path must survive until the
             * surrounding loop join. Keeping those exits here is conservative
             * for reads before the join, but never forgets a possible move. */
            unsigned exits=c->locals[i].owned ? FLOW_NEXT|FLOW_BREAK|FLOW_CONTINUE : FLOW_NEXT;
            bool then_next=(then_flow&exits)!=0,else_next=(else_flow&exits)!=0;
            if (then_next && else_next) c->locals[i].initialized &= then[i];
            else if (then_next) c->locals[i].initialized=then[i];
            else if (!else_next) c->locals[i].initialized=before[i];
        }
        free(before); free(then); break;
    }
    case STMT_WHILE: {
        condition(c, s->as.while_stmt.cond);
        size_t n = c->count; bool *before = snapshot(c);
        ++c->loops; block(c, s->as.while_stmt.body);
        bool *after=snapshot(c); bool consumed=false;
        for (size_t i=0;i<n;++i) consumed|=c->locals[i].owned && before[i] && !after[i];
        if (consumed && (flow_block(s->as.while_stmt.body,0)&(FLOW_NEXT|FLOW_CONTINUE))) {
            condition(c,s->as.while_stmt.cond);
            block(c,s->as.while_stmt.body);
        }
        --c->loops;
        for (size_t i=0;i<n;++i) c->locals[i].initialized=before[i] && after[i];
        free(after); free(before); break;
    }
    case STMT_FOR: {
        size_t outer = c->count;
        if (s->as.for_stmt.init) statement(c, s->as.for_stmt.init);
        condition(c, s->as.for_stmt.cond);
        size_t n = c->count; bool *before = snapshot(c);
        ++c->loops; block(c, s->as.for_stmt.body);
        if (s->as.for_stmt.step) statement(c, s->as.for_stmt.step);
        bool *after=snapshot(c); bool consumed=false;
        for (size_t i=0;i<n;++i) consumed|=c->locals[i].owned && before[i] && !after[i];
        if (consumed && (flow_block(s->as.for_stmt.body,0)&(FLOW_NEXT|FLOW_CONTINUE))) {
            condition(c,s->as.for_stmt.cond);
            block(c,s->as.for_stmt.body);
            if (s->as.for_stmt.step) statement(c,s->as.for_stmt.step);
        }
        --c->loops;
        for (size_t i=0;i<n;++i) c->locals[i].initialized=before[i] && after[i];
        free(after); free(before); c->count = outer; break;
    }
    case STMT_BREAK: case STMT_CONTINUE:
        if (!c->loops) error(forge_str("loop control"), "outside loop");
        break;
    case STMT_BLOCK: block(c, s->as.block); break;
    case STMT_SPAWN: {
        CoroDecl *coro = NULL;
        if (c->process) for (size_t i = 0; i < c->process->coro_count; ++i)
            if (forge_str_eq(c->process->coros[i].name, s->as.spawn.coro_name)) coro = &c->process->coros[i];
        if (!coro) error(s->as.spawn.coro_name, "unknown coroutine");
        arguments(c, s->as.spawn.coro_name, coro->params, s->as.spawn.args, s->as.spawn.arg_count, NULL, forge_str("")); break;
    }
    case STMT_SEND: {
        ForgeType target=expression(c,s->as.send.target);
        if (target.kind!=TY_INT && target.kind!=TY_PTR && target.kind!=UNKNOWN)
            error(forge_str("send"),"send target requires an integer or pointer process handle for");
        ForgeType value=expression(c,s->as.send.value);
        if (s->as.send.move_) {
            if (s->as.send.value->kind!=EXPR_IDENT)
                error(forge_str("send"),"send-move requires a named owned string for");
            Local *binding=local(c,s->as.send.value->as.ident);
            if (!binding || !binding->owned || value.kind!=TY_STRING)
                error(forge_str("send"),"send-move requires a named owned string for");
            binding->initialized=false;
        } else if (value.kind!=TY_INT && value.kind!=TY_BOOL && value.kind!=UNKNOWN)
            error(forge_str("send"),"ordinary send requires an integer value for");
        break;
    }
    case STMT_YIELD: case STMT_AWAIT:
        if (!c->coroutine) error(forge_str("suspension"), "requires coroutine context for");
        if (s->kind == STMT_AWAIT) expression(c, s->as.await_expr);
        break;
    case STMT_MATCH: {
        require(forge_type_int(), expression(c, s->as.match_stmt.scrutinee), forge_str("match"));
        size_t n = c->count; bool *before = snapshot(c);
        bool *joined=malloc((n+1)*sizeof(bool));
        if (!joined) forge_die("out of memory");
        for (size_t i=0;i<n;++i) joined[i]=true;
        bool exhaustive=false,continuing=false,owned_continuing=false;
        for (MatchArm *a = s->as.match_stmt.arms; a; a = a->next) {
            restore(c, before, n); block(c, a->body);
            unsigned exits=flow_block(a->body,0);
            continuing|=(exits&FLOW_NEXT)!=0;
            owned_continuing|=(exits&(FLOW_NEXT|FLOW_BREAK|FLOW_CONTINUE))!=0;
            for (size_t i=0;i<n;++i)
                if (exits&(c->locals[i].owned ? FLOW_NEXT|FLOW_BREAK|FLOW_CONTINUE : FLOW_NEXT))
                    joined[i]&=c->locals[i].initialized;
            exhaustive|=a->wildcard;
            if (a->wildcard && a->next) error(forge_str("match"), "wildcard must be final in");
        }
        for (size_t i=0;i<n;++i)
            c->locals[i].initialized=(!exhaustive ? before[i]&&joined[i] :
                (c->locals[i].owned ? owned_continuing : continuing) ? joined[i] : before[i]);
        free(joined); free(before); break;
    }
    }
}
static void block(Check *c, Block *b) {
    if (!b) return;
    if (++c->depth > 2000) forge_die("semantic block nesting limit exceeded");
    size_t count = c->count;
    for (Stmt *s = b->first; s; s = s->next) statement(c, s);
    c->count = count;
    --c->depth;
}
static void body(Check *c, Param *params, Block *b, ForgeType result) {
    c->count = 0; c->loops = 0; c->result = result;
    for (Param *p = params; p; p = p->next) declare(c, p->name, p->type, true);
    block(c, b);
}
/* Reachability summarizes exits rather than searching for a return token.
 * Only literal true loops establish nontermination; arbitrary conditions remain
 * conservative. Breaks from nested loops are consumed by their own loop. */
static unsigned flow_block(Block *, unsigned);
static int literal_truth(Expr *e) {
    if (!e) return 1;
    if (e->kind==EXPR_INT) return e->as.int_val!=0;
    if (e->kind==EXPR_BOOL) return e->as.bool_val!=0;
    if (e->kind==EXPR_FLOAT) return e->as.float_val!=0;
    return -1;
}
static unsigned flow_statement(Stmt *s,unsigned depth) {
    if (depth>2000) forge_die("control flow nesting limit exceeded");
    switch (s->kind) {
    case STMT_RETURN: return FLOW_RETURN;
    case STMT_BREAK: return FLOW_BREAK;
    case STMT_CONTINUE: return FLOW_CONTINUE;
    case STMT_BLOCK: return flow_block(s->as.block,depth+1);
    case STMT_IF: {
        unsigned a=flow_block(s->as.if_stmt.then_br,depth+1);
        unsigned b=s->as.if_stmt.else_br ? flow_block(s->as.if_stmt.else_br,depth+1) : FLOW_NEXT;
        int truth=literal_truth(s->as.if_stmt.cond);
        return truth==1 ? a : truth==0 ? b : a|b;
    }
    case STMT_WHILE: case STMT_FOR: {
        Expr *cond=s->kind==STMT_WHILE ? s->as.while_stmt.cond : s->as.for_stmt.cond;
        Block *body=s->kind==STMT_WHILE ? s->as.while_stmt.body : s->as.for_stmt.body;
        int truth=literal_truth(cond); if (truth==0) return FLOW_NEXT;
        unsigned exits=flow_block(body,depth+1);
        return (exits & FLOW_RETURN) | ((truth!=1 || (exits&FLOW_BREAK)) ? FLOW_NEXT : 0);
    }
    case STMT_MATCH: {
        unsigned exits=0; bool exhaustive=false;
        for (MatchArm *a=s->as.match_stmt.arms;a;a=a->next) {
            exits|=flow_block(a->body,depth+1); exhaustive|=a->wildcard;
        }
        return exits | (exhaustive ? 0 : FLOW_NEXT);
    }
    default: return FLOW_NEXT;
    }
}
static unsigned flow_block(Block *b,unsigned depth) {
    unsigned exits=FLOW_NEXT;
    if (b) for (Stmt *s=b->first;s;s=s->next)
        if (exits&FLOW_NEXT) exits=(exits&~FLOW_NEXT)|flow_statement(s,depth+1);
    return exits;
}
static void functions(Check *c, FnDecl *fns, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (!fns[i].is_extern) {
            body(c, fns[i].params, &fns[i].body, fns[i].ret_type);
            if (fns[i].ret_type.kind!=TY_VOID && (flow_block(&fns[i].body,0)&FLOW_NEXT))
                error(fns[i].name,"reachable end of non-void function");
        }
}
void forge_check_program(Program *p) {
    Check c = {.program = p};
    for (size_t i = 0; i < p->const_count; ++i) expression(&c, p->consts[i].value);
    functions(&c, p->functions, p->fn_count);
    if (p->library.present) functions(&c, p->library.functions, p->library.fn_count);
    for (size_t i = 0; i < p->module_count; ++i) {
        c.module = p->modules[i].name;
        functions(&c, p->modules[i].functions, p->modules[i].fn_count);
    }
    c.module = forge_str("");
    for (size_t i = 0; i < p->native_count; ++i)
        body(&c, NULL, &p->natives[i].body, forge_type_int());
    for (size_t i = 0; i < p->process_count; ++i) {
        c.process = &p->processes[i];
        body(&c, NULL, &c.process->body, forge_type_void());
        if (c.process->has_receive) body(&c, &c.process->receive_param, c.process->on_receive, forge_type_void());
        c.coroutine = true;
        for (size_t j = 0; j < c.process->coro_count; ++j)
            body(&c, c.process->coros[j].params, &c.process->coros[j].body, unknown());
        c.coroutine = false;
    }
    free(c.locals);
}
