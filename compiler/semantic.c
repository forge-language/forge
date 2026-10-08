#include "semantic.h"
#include "mod_registry.h"

/* Unknown is deliberately distinct from every language type: imported binary
 * APIs do not expose a typed Forge AST. Never guess signatures from names. */
#define UNKNOWN ((ForgeTypeKind)-1)
typedef struct { ForgeStr name; ForgeType type; bool initialized; } Local;
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
    c->locals[c->count++] = (Local){name, t, initialized};
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
static void arguments(Check *c, ForgeStr name, Param *params, Expr **args, size_t count) {
    size_t expected = 0;
    for (Param *p = params; p; p = p->next) ++expected;
    if (count != expected) error(name, "wrong argument count for");
    for (size_t i = 0; params; params = params->next, ++i)
        require(params->type, expression(c, args[i]), name);
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
        arguments(c, name, fn->params, args, count);
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
            if (!l->initialized) error(e->as.ident, "uninitialized value");
            t = l->type; break;
        }
        for (size_t i = 0; i < c->program->const_count; ++i)
            if (forge_str_eq(c->program->consts[i].name, e->as.ident)) {
                t = expression(c, c->program->consts[i].value); goto done;
            }
        /* Function identifiers are accepted as callback references. */
        if (resolve(c, c->module, e->as.ident) || resolve(c, forge_str(""), e->as.ident)) break;
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
            resolve(c, e->as.field.base->as.ident, e->as.field.field)) break;
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
    case EXPR_MOVE: t = expression(c, e->as.move_expr); break;
    }
done:
    --c->depth;
    return t;
}
static void block(Check *, Block *);
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
        declare(c, s->as.let.name, s->as.let.type, s->as.let.init != NULL); break;
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
        for (size_t i = 0; i < n; ++i) c->locals[i].initialized &= then[i];
        free(before); free(then); break;
    }
    case STMT_WHILE: {
        condition(c, s->as.while_stmt.cond);
        size_t n = c->count; bool *before = snapshot(c);
        ++c->loops; block(c, s->as.while_stmt.body); --c->loops;
        restore(c, before, n); free(before); break;
    }
    case STMT_FOR: {
        size_t outer = c->count;
        if (s->as.for_stmt.init) statement(c, s->as.for_stmt.init);
        condition(c, s->as.for_stmt.cond);
        size_t n = c->count; bool *before = snapshot(c);
        ++c->loops; block(c, s->as.for_stmt.body);
        if (s->as.for_stmt.step) statement(c, s->as.for_stmt.step);
        --c->loops;
        restore(c, before, n); free(before); c->count = outer; break;
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
        arguments(c, s->as.spawn.coro_name, coro->params, s->as.spawn.args, s->as.spawn.arg_count); break;
    }
    case STMT_SEND: expression(c, s->as.send.target); expression(c, s->as.send.value); break;
    case STMT_YIELD: case STMT_AWAIT:
        if (!c->coroutine) error(forge_str("suspension"), "requires coroutine context for");
        if (s->kind == STMT_AWAIT) expression(c, s->as.await_expr);
        break;
    case STMT_MATCH: {
        require(forge_type_int(), expression(c, s->as.match_stmt.scrutinee), forge_str("match"));
        size_t n = c->count; bool *before = snapshot(c);
        /* Conservative: match arms do not establish initialization after match. */
        for (MatchArm *a = s->as.match_stmt.arms; a; a = a->next) {
            restore(c, before, n); block(c, a->body);
            if (a->wildcard && a->next) error(forge_str("match"), "wildcard must be final in");
        }
        restore(c, before, n); free(before); break;
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
static void functions(Check *c, FnDecl *fns, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (!fns[i].is_extern) body(c, fns[i].params, &fns[i].body, fns[i].ret_type);
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
