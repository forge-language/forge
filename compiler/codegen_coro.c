/* Coroutine emission: the state struct that holds a suspended coroutine's
 * locals, the resumable body compiled into a switch over step labels, and the
 * spawn stub that allocates and seeds the state. */
#include "codegen_internal.h"

/* A growable array of the `let` statements gathered from a coroutine body. */
typedef struct {
    Stmt **items;
    size_t count;
    size_t cap;
} StmtList;

static void stmt_list_push(StmtList *l, Stmt *s) {
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        Stmt **grown = (Stmt **)realloc(l->items, cap * sizeof(Stmt *));
        if (!grown) forge_die("out of memory");
        l->items = grown;
        l->cap = cap;
    }
    l->items[l->count++] = s;
}

static bool let_list_has_name(const StmtList *l, ForgeStr name) {
    for (size_t i = 0; i < l->count; i++)
        if (forge_str_eq(l->items[i]->as.let.name, name)) return true;
    return false;
}

/* Collect every `let` in a coroutine body, including ones nested in if/while/
 * for/block/match bodies: body emission rewrites all of them into state-struct
 * fields, so the struct has to declare all of them or the generated C
 * references undeclared members. Duplicate names share one slot. */
static void collect_coro_lets(Stmt *s, StmtList *out) {
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_LET:
            if (!let_list_has_name(out, s->as.let.name)) stmt_list_push(out, s);
            break;
        case STMT_IF:
            collect_coro_lets(s->as.if_stmt.then_br->first, out);
            if (s->as.if_stmt.else_br) collect_coro_lets(s->as.if_stmt.else_br->first, out);
            break;
        case STMT_WHILE:
            collect_coro_lets(s->as.while_stmt.body->first, out);
            break;
        case STMT_FOR:
            collect_coro_lets(s->as.for_stmt.init, out);
            collect_coro_lets(s->as.for_stmt.body->first, out);
            break;
        case STMT_BLOCK:
            collect_coro_lets(s->as.block->first, out);
            break;
        case STMT_MATCH:
            for (MatchArm *a = s->as.match_stmt.arms; a; a = a->next)
                collect_coro_lets(a->body->first, out);
            break;
        default:
            break;
        }
    }
}

/* The top-level `let`s whose initialiser the spawn stub runs eagerly, and which
 * the body therefore must not re-run (or their side effects happen twice).
 * Only the leading run of `let`s qualifies: once any other statement has run,
 * an initialiser may depend on what that statement did, and evaluating it
 * early in the stub would read the wrong value. */
static void collect_hoisted_lets(CoroDecl *coro, StmtList *out) {
    for (Stmt *s = coro->body.first; s; s = s->next) {
        if (s->kind != STMT_LET) break;
        if (s->as.let.init) stmt_list_push(out, s);
    }
}

static bool cg_is_hoisted(const Codegen *cg, const Stmt *s) {
    for (size_t i = 0; i < cg->hoisted_count; i++)
        if (cg->hoisted[i] == s) return true;
    return false;
}

static bool param_named(Param *params, ForgeStr name) {
    for (Param *p = params; p; p = p->next)
        if (forge_str_eq(p->name, name)) return true;
    return false;
}

/* --------------------------------------------------------- state struct --- */

static void emit_coro_state_struct(Codegen *cg, CoroDecl *coro) {
    FILE *out = cg->out;
    fputs("typedef struct {\n", out);
    fputs("    int _forge_step;\n", out);
    fputs("    fr_process_t *proc;\n", out);
    fputs("    int64_t _forge_ret;\n", out);
    for (Param *p = coro->params; p; p = p->next) {
        fputs("    ", out);
        c_type_name(out, p->type);
        fprintf(out, " %.*s;\n", FSTR(p->name));
    }
    StmtList lets = { NULL, 0, 0 };
    collect_coro_lets(coro->body.first, &lets);
    for (size_t i = 0; i < lets.count; i++) {
        Stmt *s = lets.items[i];
        if (param_named(coro->params, s->as.let.name)) continue;
        fputs("    ", out);
        c_type_name(out, s->as.let.type);
        fprintf(out, " %.*s;\n", FSTR(s->as.let.name));
    }
    free(lets.items);
    fprintf(out, "} %.*s_state_t;\n\n", FSTR(coro->name));
}

/* ---------------------------------------------------------------- body --- */

/* Emit a statement list as resumable coroutine code. `step` is a single
 * monotonic counter threaded through the whole body so every suspend point and
 * every re-entry label gets exactly one unique `case` value -- two independent
 * counters is what previously produced duplicate case labels. */
static void emit_coro_stmts(Codegen *cg, Stmt *s, const char *state_var, int *step) {
    cg_enter(cg);
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_YIELD: {
            int resume = ++(*step);
            cg_line(cg, "    fr_coro_set_step(__coro, %d);", resume);
            cg_line(cg, "    return fr_yield(__coro);");
            cg_line(cg, "case %d:", resume);
            continue;
        }
        case STMT_AWAIT: {
            int resume = ++(*step);
            cg_line(cg, "    fr_coro_set_step(__coro, %d);", resume);
            cg_indent(cg);
            fputs("if (!fr_await_fd(__coro, ", cg->out);
            emit_expr(cg, s->as.await_expr);
            fputs(", FR_EVENT_READ)) return fr_yield(__coro);\n", cg->out);
            cg_line(cg, "case %d:", resume);
            continue;
        }
        case STMT_LET:
            /* The spawn stub already ran this initialiser into the state
             * struct; re-running it here would duplicate its side effects. */
            if (cg_is_hoisted(cg, s)) continue;
            cg_indent(cg);
            if (cg->state_prefix) fputs(cg->state_prefix, cg->out);
            fprintf(cg->out, "%.*s = ", FSTR(s->as.let.name));
            if (s->as.let.init) emit_expr(cg, s->as.let.init);
            else fputs("0", cg->out);
            fputs(";\n", cg->out);
            continue;
        default:
            break;
        }

        /* A compound statement may be where execution resumes, so it needs a
         * label of its own to jump back into. */
        if (stmt_has_yield(s) || s->kind == STMT_IF || s->kind == STMT_WHILE ||
            s->kind == STMT_BLOCK) {
            cg_line(cg, "case %d:", ++(*step));
        }

        switch (s->kind) {
        case STMT_EXPR:
            cg_indent(cg);
            emit_expr(cg, s->as.expr);
            fputs(";\n", cg->out);
            break;
        case STMT_RETURN:
            /* Store the returned value in the state struct so it survives the
             * coroutine finishing, instead of evaluating and dropping it. */
            if (s->as.ret) {
                cg_indent(cg);
                fprintf(cg->out, "%s->_forge_ret = (int64_t)", state_var);
                if (cg_expr_is_string(cg, s->as.ret)) fputs("(intptr_t)", cg->out);
                fputc('(', cg->out);
                emit_expr(cg, s->as.ret);
                fputs(");\n", cg->out);
            }
            cg_line(cg, "return FR_CORO_DONE;");
            break;
        case STMT_IF:
            cg_line(cg, "if (");
            emit_expr(cg, s->as.if_stmt.cond);
            fputs(") {\n", cg->out);
            cg->indent++;
            emit_coro_stmts(cg, s->as.if_stmt.then_br->first, state_var, step);
            cg->indent--;
            cg_indent(cg);
            fputs("}", cg->out);
            if (s->as.if_stmt.else_br) {
                fputs(" else {\n", cg->out);
                cg->indent++;
                emit_coro_stmts(cg, s->as.if_stmt.else_br->first, state_var, step);
                cg->indent--;
                cg_line(cg, "}");
            } else {
                fputc('\n', cg->out);
            }
            break;
        case STMT_WHILE:
            cg_line(cg, "while (");
            emit_expr(cg, s->as.while_stmt.cond);
            fputs(") {\n", cg->out);
            cg->indent++;
            emit_coro_stmts(cg, s->as.while_stmt.body->first, state_var, step);
            cg->indent--;
            cg_line(cg, "}");
            break;
        case STMT_SPAWN: {
            /* Children spawn into the coroutine's own process. */
            char proc_expr[CG_SYM_MAX];
            snprintf(proc_expr, sizeof(proc_expr), "%s->proc", state_var);
            emit_spawn_stmt(cg, s, proc_expr);
            break;
        }
        case STMT_SEND:
            emit_send_stmt(cg, s);
            break;
        case STMT_ASSIGN:
            emit_assign_stmt(cg, s);
            break;
        case STMT_BLOCK:
            cg->indent++;
            emit_coro_stmts(cg, s->as.block->first, state_var, step);
            cg->indent--;
            break;
        default:
            break;
        }
    }
    cg_leave(cg);
}

/* --------------------------------------------------------- spawn stub --- */

static void emit_coro_spawn_inits(Codegen *cg, CoroDecl *coro) {
    StmtList hoisted = { NULL, 0, 0 };
    collect_hoisted_lets(coro, &hoisted);
    const char *saved = cg->state_prefix;
    cg->state_prefix = "init->";
    for (size_t i = 0; i < hoisted.count; i++) {
        Stmt *s = hoisted.items[i];
        cg_indent(cg);
        fprintf(cg->out, "init->%.*s = ", FSTR(s->as.let.name));
        emit_expr(cg, s->as.let.init);
        fputs(";\n", cg->out);
    }
    cg->state_prefix = saved;
    free(hoisted.items);
}

static void emit_coro_spawn_fn(Codegen *cg, CoroDecl *coro) {
    FILE *out = cg->out;
    ForgeStr name = coro->name;

    cg_begin_function(cg);
    fprintf(out, "static void %.*s_spawn(fr_process_t *proc", FSTR(name));
    for (Param *p = coro->params; p; p = p->next) {
        fputs(", ", out);
        c_type_name(out, p->type);
        fprintf(out, " %.*s", FSTR(p->name));
    }
    fputs(") {\n", out);
    fprintf(out, "    %.*s_state_t *init = (%.*s_state_t *)calloc(1, sizeof(%.*s_state_t));\n",
            FSTR(name), FSTR(name), FSTR(name));
    fputs("    if (!init) { fprintf(stderr, \"forge: out of memory\\n\"); abort(); }\n", out);
    fputs("    init->proc = proc;\n", out);
    for (Param *p = coro->params; p; p = p->next)
        fprintf(out, "    init->%.*s = %.*s;\n", FSTR(p->name), FSTR(p->name));
    emit_coro_spawn_inits(cg, coro);
    fprintf(out, "    fr_coro_spawn(proc, %.*s_fn, init, sizeof(%.*s_state_t));\n",
            FSTR(name), FSTR(name));
    fputs("}\n\n", out);
}

/* ---------------------------------------------------------------- entry --- */

void emit_coro_fn(Codegen *cg, CoroDecl *coro) {
    FILE *out = cg->out;
    ForgeStr name = coro->name;

    emit_coro_state_struct(cg, coro);

    fprintf(out, "static fr_coro_status_t %.*s_fn(fr_coro_t *__coro, void *__userdata) {\n",
            FSTR(name));
    fprintf(out, "    %.*s_state_t *st = (%.*s_state_t *)__userdata;\n",
            FSTR(name), FSTR(name));
    fputs("    fr_msg_t __recv_msg;\n", out);
    fputs("    switch (fr_coro_step(__coro)) {\n", out);
    cg->indent++;
    cg_line(cg, "default:");

    const char *saved_prefix = cg->state_prefix;
    cg->state_prefix = "st->";
    cg_begin_function(cg);
    cg_push_params(cg, coro->params);

    /* Every let in the body -- nested ones included -- is a state-struct
     * field, so all of them must be visible as locals while emitting. */
    StmtList lets = { NULL, 0, 0 };
    collect_coro_lets(coro->body.first, &lets);
    for (size_t i = 0; i < lets.count; i++)
        cg_push_local(cg, lets.items[i]->as.let.name, lets.items[i]->as.let.type);
    free(lets.items);

    StmtList hoisted = { NULL, 0, 0 };
    collect_hoisted_lets(coro, &hoisted);
    cg->hoisted = hoisted.items;
    cg->hoisted_count = hoisted.count;

    int step = 0;
    emit_coro_stmts(cg, coro->body.first, "st", &step);

    cg->hoisted = NULL;
    cg->hoisted_count = 0;
    free(hoisted.items);
    cg->state_prefix = saved_prefix;
    cg->local_count = 0;
    cg->indent--;
    fputs("    }\n", out);
    fputs("    return FR_CORO_DONE;\n", out);
    fputs("}\n\n", out);

    emit_coro_spawn_fn(cg, coro);
}
