/* Statement emission for ordinary (non-coroutine) function bodies, plus the
 * statement forms the coroutine emitter shares with this one. */
#include "codegen_internal.h"

static bool block_has_yield(Stmt *first);

bool stmt_has_yield(Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case STMT_YIELD:
    case STMT_AWAIT:
        return true;
    case STMT_IF:
        return block_has_yield(s->as.if_stmt.then_br->first) ||
               (s->as.if_stmt.else_br && block_has_yield(s->as.if_stmt.else_br->first));
    case STMT_WHILE:
        return block_has_yield(s->as.while_stmt.body->first);
    case STMT_BLOCK:
        return block_has_yield(s->as.block->first);
    default:
        return false;
    }
}

static bool block_has_yield(Stmt *first) {
    for (Stmt *b = first; b; b = b->next)
        if (stmt_has_yield(b)) return true;
    return false;
}

/* ----------------------------------------- forms shared with the coro path --- */

void emit_send_stmt(Codegen *cg, Stmt *s) {
    cg_indent(cg);
    fputs(s->as.send.move_ ? "fr_send_move(" : "fr_send(", cg->out);
    emit_expr(cg, s->as.send.target);
    fprintf(cg->out, ", %d, ", s->as.send.tag);
    emit_expr(cg, s->as.send.value);
    fputs(s->as.send.move_ ? ");\n" : ", NULL, 0);\n", cg->out);
}

/* `proc_expr` is the C expression naming the owning process: the enclosing
 * function's process parameter, or `st->proc` from inside a coroutine. */
void emit_spawn_stmt(Codegen *cg, Stmt *s, const char *proc_expr) {
    cg_indent(cg);
    fprintf(cg->out, "%.*s_spawn(%s", FSTR(s->as.spawn.coro_name), proc_expr);
    for (size_t i = 0; i < s->as.spawn.arg_count; i++) {
        fputs(", ", cg->out);
        emit_expr(cg, s->as.spawn.args[i]);
    }
    fputs(");\n", cg->out);
}

void emit_assign_stmt(Codegen *cg, Stmt *s) {
    cg_indent(cg);
    if (cg->state_prefix) fputs(cg->state_prefix, cg->out);
    fprintf(cg->out, "%.*s = ", FSTR(s->as.assign.name));
    emit_expr(cg, s->as.assign.value);
    fputs(";\n", cg->out);
}

/* ------------------------------------------------------- compound statements --- */

static void emit_if_stmt(Codegen *cg, Stmt *s, const char *proc_var, bool in_coro) {
    cg_enter(cg);
    cg_indent(cg);
    fputs("if (", cg->out);
    emit_expr(cg, s->as.if_stmt.cond);
    fputs(") {\n", cg->out);
    cg->indent++;
    emit_stmts(cg, s->as.if_stmt.then_br->first, proc_var, in_coro);
    cg->indent--;
    cg_indent(cg);
    fputs("}", cg->out);
    if (s->as.if_stmt.else_br) {
        Stmt *es = s->as.if_stmt.else_br->first;
        /* `else if` rather than a nested `else { if ... }`, so a long chain
         * does not march off the right-hand margin. */
        if (es && !es->next && es->kind == STMT_IF) {
            fputs(" else ", cg->out);
            emit_if_stmt(cg, es, proc_var, in_coro);
            fputc('\n', cg->out);
        } else {
            fputs(" else {\n", cg->out);
            cg->indent++;
            emit_stmts(cg, es, proc_var, in_coro);
            cg->indent--;
            cg_line(cg, "}");
        }
    } else {
        fputc('\n', cg->out);
    }
    cg_leave(cg);
}

/* A `for` step can live in a real C for-header only if every step statement is
 * a plain assignment or expression. */
static bool for_step_inlinable(Stmt *step) {
    for (Stmt *s = step; s; s = s->next) {
        if (s->kind != STMT_ASSIGN && s->kind != STMT_EXPR) return false;
    }
    return true;
}

static void emit_for_step_inline(Codegen *cg, Stmt *step) {
    for (Stmt *s = step; s; s = s->next) {
        if (s != step) fputs(", ", cg->out);
        if (s->kind == STMT_ASSIGN) {
            if (cg->state_prefix) fputs(cg->state_prefix, cg->out);
            fprintf(cg->out, "%.*s = ", FSTR(s->as.assign.name));
            emit_expr(cg, s->as.assign.value);
        } else {
            emit_expr(cg, s->as.expr);
        }
    }
}

static void emit_for_stmt(Codegen *cg, Stmt *s, const char *proc_var, bool in_coro) {
    bool suspends = block_has_yield(s->as.for_stmt.body->first);
    bool inline_step = for_step_inlinable(s->as.for_stmt.step);
    int saved_cont = cg->continue_label;

    /* The init declarations get their own scope so two sibling `for`s can each
     * declare the same loop variable. */
    cg_indent(cg);
    fputs("{\n", cg->out);
    cg->indent++;
    if (s->as.for_stmt.init) emit_stmts(cg, s->as.for_stmt.init, proc_var, in_coro);

    if (!suspends && inline_step) {
        /* Real C for-loop: `continue` then runs the step, as it must. The
         * while-desugaring below skips it and spins forever. */
        cg_indent(cg);
        fputs("for (; ", cg->out);
        emit_expr(cg, s->as.for_stmt.cond);
        fputs("; ", cg->out);
        if (s->as.for_stmt.step) emit_for_step_inline(cg, s->as.for_stmt.step);
        fputs(") {\n", cg->out);
        cg->indent++;
        cg->continue_label = 0;
        emit_stmts(cg, s->as.for_stmt.body->first, proc_var, in_coro);
        cg->continue_label = saved_cont;
    } else {
        /* Suspending body (or a step we cannot inline): desugar to a while
         * loop, and route `continue` to a label placed just before the step so
         * the step still runs. */
        int label = ++cg->label_seq;
        cg_indent(cg);
        fputs("while (", cg->out);
        emit_expr(cg, s->as.for_stmt.cond);
        fputs(") {\n", cg->out);
        cg->indent++;
        cg->continue_label = s->as.for_stmt.step ? label : 0;
        emit_stmts(cg, s->as.for_stmt.body->first, proc_var, in_coro);
        cg->continue_label = saved_cont;
        if (s->as.for_stmt.step) {
            cg_line(cg, "__forge_cont_%d: ;", label);
            emit_stmts(cg, s->as.for_stmt.step, proc_var, in_coro);
        }
    }

    cg->indent--;
    cg_indent(cg);
    fputs("}\n", cg->out);
    cg->indent--;
    cg_indent(cg);
    fputs("}\n", cg->out);
}

static void emit_match_stmt(Codegen *cg, Stmt *s, const char *proc_var, bool in_coro) {
    cg_indent(cg);
    fputs("{\n", cg->out);
    cg->indent++;
    cg_indent(cg);
    fputs("int64_t __match_val = ", cg->out);
    emit_expr(cg, s->as.match_stmt.scrutinee);
    fputs(";\n", cg->out);

    bool first = true;
    for (MatchArm *arm = s->as.match_stmt.arms; arm; arm = arm->next) {
        /* A wildcard matches everything, so anything after it is unreachable
         * - and a second wildcard would emit `else else`. */
        if (arm->wildcard && arm->next) {
            fprintf(stderr, "forge: match arm '_' must be last; "
                            "arms after it are unreachable\n");
            exit(1);
        }
        cg_indent(cg);
        if (arm->wildcard) {
            fputs(first ? "if (1)" : "else", cg->out);
        } else {
            fprintf(cg->out, "%s (__match_val == %lld)", first ? "if" : "else if",
                    (long long)arm->int_pat);
        }
        fputs(" {\n", cg->out);
        cg->indent++;
        emit_stmts(cg, arm->body->first, proc_var, in_coro);
        cg->indent--;
        cg_line(cg, "}");
        first = false;
    }
    cg->indent--;
    cg_line(cg, "}");
}

/* ---------------------------------------------------------------- driver --- */

void emit_stmts(Codegen *cg, Stmt *s, const char *proc_var, bool in_coro) {
    cg_enter(cg);
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_LET:
            cg_push_local(cg, s->as.let.name, s->as.let.type);
            cg_indent(cg);
            c_type_name(cg->out, s->as.let.type);
            fprintf(cg->out, " %.*s", FSTR(s->as.let.name));
            if (s->as.let.init) {
                fputs(" = ", cg->out);
                emit_expr(cg, s->as.let.init);
            }
            fputs(";\n", cg->out);
            break;
        case STMT_EXPR:
            cg_indent(cg);
            emit_expr(cg, s->as.expr);
            fputs(";\n", cg->out);
            break;
        case STMT_RETURN:
            cg_indent(cg);
            fputs("return ", cg->out);
            if (s->as.ret) emit_expr(cg, s->as.ret);
            fputs(";\n", cg->out);
            break;
        case STMT_IF:
            emit_if_stmt(cg, s, proc_var, in_coro);
            break;
        case STMT_WHILE:
            cg_line(cg, "while (");
            emit_expr(cg, s->as.while_stmt.cond);
            fputs(") {\n", cg->out);
            cg->indent++;
            {
                /* C's own `continue` is correct for a while loop. */
                int saved_cont = cg->continue_label;
                cg->continue_label = 0;
                emit_stmts(cg, s->as.while_stmt.body->first, proc_var, in_coro);
                cg->continue_label = saved_cont;
            }
            cg->indent--;
            cg_indent(cg);
            fputs("}\n", cg->out);
            break;
        case STMT_FOR:
            emit_for_stmt(cg, s, proc_var, in_coro);
            break;
        case STMT_BREAK:
            cg_line(cg, "break;");
            break;
        case STMT_CONTINUE:
            if (cg->continue_label) cg_line(cg, "goto __forge_cont_%d;", cg->continue_label);
            else cg_line(cg, "continue;");
            break;
        case STMT_SPAWN:
            emit_spawn_stmt(cg, s, proc_var);
            break;
        case STMT_SEND:
            emit_send_stmt(cg, s);
            break;
        case STMT_YIELD:
            if (in_coro) cg_line(cg, "return fr_yield(__coro);");
            break;
        case STMT_AWAIT:
            if (in_coro) {
                cg_indent(cg);
                fputs("if (!fr_await_fd(__coro, ", cg->out);
                emit_expr(cg, s->as.await_expr);
                fputs(", FR_EVENT_READ)) return fr_yield(__coro);\n", cg->out);
            }
            break;
        case STMT_ASSIGN:
            emit_assign_stmt(cg, s);
            break;
        case STMT_MATCH:
            emit_match_stmt(cg, s, proc_var, in_coro);
            break;
        case STMT_BLOCK:
            cg->indent++;
            emit_stmts(cg, s->as.block->first, proc_var, in_coro);
            cg->indent--;
            break;
        }
    }
    cg_leave(cg);
}
