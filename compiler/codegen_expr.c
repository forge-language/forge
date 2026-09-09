/* Expression emission. */
#include "codegen_internal.h"
#include "mod_registry.h"

static void emit_expr_inner(Codegen *cg, Expr *e);

void emit_expr(Codegen *cg, Expr *e) {
    cg_enter(cg);
    emit_expr_inner(cg, e);
    cg_leave(cg);
}

/* Write the C identifier a function reference compiles to: mangled when it is
 * one of the current module's own functions, bare otherwise. */
static void emit_fn_c_symbol(Codegen *cg, ForgeStr name, FnDecl *fn) {
    if (fn && cg_lookup_module_fn(cg, cg->current_module, name) == fn) {
        char sym[CG_SYM_MAX];
        forge_mod_mangle(sym, sizeof(sym), cg->current_module, name);
        fputs(sym, cg->out);
    } else {
        emit_str(cg->out, name);
    }
}

/* thread.spawn takes a function *reference*, which the expression grammar has
 * no other way to express, so these two are recognised here rather than going
 * through the ordinary call path. Returns 0 if `name` is not one of them. */
static int emit_thread_call(Codegen *cg, ForgeStr name, Expr **args, size_t arg_count) {
    static const struct {
        const char *fr_name;
        size_t params;
        const char *emit;      /* runtime entry point */
        const char *cast;      /* function-pointer cast for the callee */
        const char *signature; /* shown when the callee does not match */
    } SPAWNERS[] = {
        { "thread_spawn", 1, "fr_threading_spawn", "fr_threading_fn1_t",
          "thread_spawn(fn, arg) requires fn(arg: int): int" },
        { "thread_spawn_indexed", 2, "fr_threading_spawn_indexed", "fr_threading_fn2_t",
          "thread_spawn_indexed(fn, n) requires fn(id: int, total: int): int" },
    };

    if (arg_count != 2) return 0;
    for (size_t i = 0; i < sizeof(SPAWNERS) / sizeof(SPAWNERS[0]); i++) {
        if (!forge_str_eq(name, forge_str(SPAWNERS[i].fr_name))) continue;

        if (args[0]->kind != EXPR_IDENT) {
            fprintf(stderr, "forge: %s requires a function name\n", SPAWNERS[i].fr_name);
            exit(1);
        }
        ForgeStr fn_name = args[0]->as.ident;
        FnDecl *fn = cg_resolve_fn_ref(cg, fn_name);
        if (!fn || fn_param_count(fn) != SPAWNERS[i].params) {
            fprintf(stderr, "forge: %s\n", SPAWNERS[i].signature);
            exit(1);
        }
        fprintf(cg->out, "%s((%s)", SPAWNERS[i].emit, SPAWNERS[i].cast);
        emit_fn_c_symbol(cg, fn_name, fn);
        fputs(", ", cg->out);
        emit_expr(cg, args[1]);
        fputc(')', cg->out);
        return 1;
    }
    return 0;
}

static const char *binop_c_operator(BinOp op) {
    switch (op) {
    case BIN_ADD: return "+";
    case BIN_SUB: return "-";
    case BIN_MUL: return "*";
    case BIN_DIV: return "/";
    case BIN_MOD: return "%";
    case BIN_EQ: return "==";
    case BIN_NE: return "!=";
    case BIN_LT: return "<";
    case BIN_LE: return "<=";
    case BIN_GT: return ">";
    case BIN_GE: return ">=";
    case BIN_AND: return "&&";
    case BIN_OR: return "||";
    }
    return "?";
}

/* println is variadic and type-directed, so it cannot be a plain stdlib
 * mapping: each argument picks its own printer. */
static void emit_println(Codegen *cg, Expr *e) {
    fputs("({ ", cg->out);
    for (size_t i = 0; i < e->as.call.arg_count; i++) {
        Expr *arg = e->as.call.args[i];
        if (i > 0) fputs(" ", cg->out);
        switch (cg_expr_str_kind(cg, arg)) {
        case CG_STR_YES: fputs("fr_print_str(", cg->out); break;
        case CG_STR_NO: fputs("fr_print_int(", cg->out); break;
        /* Declaration not visible here (external library function): let the C
         * compiler pick the printer from the real type instead of guessing
         * from the function's name. */
        case CG_STR_UNKNOWN: fputs("FORGE_PRINT_AUTO(", cg->out); break;
        }
        emit_expr(cg, arg);
        fputs("); ", cg->out);
    }
    fputs("fr_println(); })", cg->out);
}

static void emit_call_args(Codegen *cg, Expr **args, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (i) fputc(',', cg->out);
        emit_expr(cg, args[i]);
    }
    fputc(')', cg->out);
}

static void emit_call(Codegen *cg, Expr *e) {
    ForgeStr name = e->as.call.name;
    if (emit_thread_call(cg, name, e->as.call.args, e->as.call.arg_count)) return;
    if (forge_str_eq(name, forge_str("println"))) {
        emit_println(cg, e);
        return;
    }

    const char *c_fn = forge_std_c_name(name, cg->imports, cg->import_count);
    FnDecl *mod_fn = cg_lookup_module_fn(cg, cg->current_module, name);
    if (c_fn) {
        fprintf(cg->out, "%s(", c_fn);
    } else if (mod_fn) {
        char sym[CG_SYM_MAX];
        forge_mod_mangle(sym, sizeof(sym), cg->current_module, name);
        fprintf(cg->out, "%s(", sym);
        e->type = mod_fn->ret_type;
    } else {
        FnDecl *fn = cg_lookup_fn(cg, name);
        if (fn) e->type = fn->ret_type;
        emit_str(cg->out, name);
        fputc('(', cg->out);
    }
    emit_call_args(cg, e->as.call.args, e->as.call.arg_count);
}

static void emit_qual_call(Codegen *cg, Expr *e) {
    FnDecl *qfn = cg_resolve_qual_fn(cg, e->as.qual_call.module, e->as.qual_call.name);
    if (qfn) e->type = qfn->ret_type;

    char sym[CG_SYM_MAX];
    if (forge_import_is_file_module(cg->prog, e->as.qual_call.module))
        forge_mod_mangle(sym, sizeof(sym), e->as.qual_call.module, e->as.qual_call.name);
    else
        forge_lib_mangle(sym, sizeof(sym), e->as.qual_call.module, e->as.qual_call.name);
    fprintf(cg->out, "%s(", sym);
    emit_call_args(cg, e->as.qual_call.args, e->as.qual_call.arg_count);
}

/* Inside a coroutine every local lives in the state struct, so identifier
 * references need the `st->` / `init->` prefix that is in effect. */
static void emit_state_qualified(Codegen *cg, ForgeStr name) {
    if (cg->state_prefix) fputs(cg->state_prefix, cg->out);
    emit_str(cg->out, name);
}

static void emit_expr_inner(Codegen *cg, Expr *e) {
    switch (e->kind) {
    case EXPR_INT:
        fprintf(cg->out, "%lld", (long long)e->as.int_val);
        break;
    case EXPR_FLOAT:
        fprintf(cg->out, "%g", e->as.float_val);
        break;
    case EXPR_BOOL:
        fputs(e->as.bool_val ? "1" : "0", cg->out);
        break;
    case EXPR_STRING:
        emit_c_string_literal(cg->out, e->as.string_val);
        break;
    case EXPR_IDENT:
        if (cg_is_moved(cg, e->as.ident)) {
            fprintf(stderr, "forge: use of moved value '%.*s'\n", FSTR(e->as.ident));
            exit(1);
        }
        /* A local shadows a const of the same name, so the local table has to
         * be consulted first. */
        if (!cg_is_local(cg, e->as.ident) && cg_find_const(cg, e->as.ident))
            fprintf(cg->out, "forge_const_%.*s", FSTR(e->as.ident));
        else
            emit_state_qualified(cg, e->as.ident);
        break;
    case EXPR_MOVE:
        if (e->as.move_expr && e->as.move_expr->kind == EXPR_IDENT) {
            ForgeStr name = e->as.move_expr->as.ident;
            cg_mark_moved(cg, name);
            fputs("fr_own_take(&", cg->out);
            emit_state_qualified(cg, name);
            fputc(')', cg->out);
        } else {
            emit_expr(cg, e->as.move_expr);
        }
        break;
    case EXPR_BINARY:
        fputc('(', cg->out);
        emit_expr(cg, e->as.binary.left);
        fprintf(cg->out, " %s ", binop_c_operator(e->as.binary.op));
        emit_expr(cg, e->as.binary.right);
        fputc(')', cg->out);
        break;
    case EXPR_CALL:
        emit_call(cg, e);
        break;
    case EXPR_RECV:
        fputs("(fr_recv(fr_coro_process(__coro), &__recv_msg), __recv_msg.value)", cg->out);
        break;
    case EXPR_QUAL_CALL:
        emit_qual_call(cg, e);
        break;
    case EXPR_INDEX:
        if (e->as.index.base->type.kind == TY_STRING ||
            cg_expr_is_string(cg, e->as.index.base)) {
            fputs("fr_str_char_at(", cg->out);
            emit_expr(cg, e->as.index.base);
            fputs(", ", cg->out);
            emit_expr(cg, e->as.index.index);
            fputc(')', cg->out);
        } else {
            fputs("((int64_t*)", cg->out);
            emit_expr(cg, e->as.index.base);
            fputs(")[", cg->out);
            emit_expr(cg, e->as.index.index);
            fputs("]", cg->out);
        }
        break;
    case EXPR_FIELD:
        emit_expr(cg, e->as.field.base);
        fprintf(cg->out, ".%.*s", FSTR(e->as.field.field));
        break;
    }
}
