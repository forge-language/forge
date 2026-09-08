#include "codegen.h"
#include "mod_registry.h"
#include <stdarg.h>
#include <string.h>

/* Guard against unbounded recursion over deeply nested ASTs: emitting a
 * 60k-deep expression tree overflows the C stack long before it overflows
 * anything else. */
#define CG_MAX_DEPTH 3000

/* One symbol-table slot. `module` is empty for top-level and library symbols,
 * and holds the module name for module-scoped ones. */
typedef struct {
    ForgeStr module;
    ForgeStr name;
    void *value;
} CgSlot;

/* Open-addressed map keyed by (module, name). Replaces the linear scans (and
 * the modules x functions nested loop) that made every identifier lookup
 * proportional to the size of the program. */
typedef struct {
    CgSlot *slots;
    size_t cap; /* power of two, 0 when empty */
} CgMap;

typedef struct {
    FILE *out;
    int indent;
    Program *prog;
    const char *state_prefix;
    struct ForgeLocal { ForgeStr name; ForgeType type; } *locals;
    size_t local_count;
    size_t local_cap;
    ForgeStr *imports;
    size_t import_count;
    CgMap fn_map;      /* (module, name) -> FnDecl*  */
    CgMap const_map;   /* ("", name)      -> ConstDecl* */
    ForgeStr *moved_locals;
    size_t moved_count;
    size_t moved_cap;
    ForgeStr current_module;
    int depth;            /* current AST recursion depth */
    int label_seq;        /* generator for unique generated C labels */
    int continue_label;   /* >0: `continue` must jump to __forge_cont_N */
    Stmt **hoisted;       /* coroutine lets already initialised in the spawn stub */
    size_t hoisted_count;
} Codegen;

static void cg_enter(Codegen *cg) {
    if (++cg->depth > CG_MAX_DEPTH)
        forge_die("expression or statement nesting too deep (max "
                  "3000); simplify the source");
}

static void cg_leave(Codegen *cg) { cg->depth--; }

/* Reset the per-function bookkeeping. `moved_locals` in particular is
 * append-only within a function; leaking it into the next function would make
 * every later use of a same-named local look like a use-after-move. */
static void cg_begin_function(Codegen *cg) {
    cg->local_count = 0;
    cg->moved_count = 0;
    cg->continue_label = 0;
    cg->depth = 0;
}

static uint64_t cg_key_hash(ForgeStr module, ForgeStr name) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < module.len; i++) {
        h ^= (unsigned char)module.data[i];
        h *= 1099511628211ULL;
    }
    h ^= 0xff;
    h *= 1099511628211ULL;
    for (size_t i = 0; i < name.len; i++) {
        h ^= (unsigned char)name.data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void cg_map_init(CgMap *m, size_t expected) {
    size_t cap = 16;
    while (cap < expected * 2 + 1) cap *= 2;
    m->slots = (CgSlot *)calloc(cap, sizeof(CgSlot));
    if (!m->slots) forge_die("out of memory");
    m->cap = cap;
}

static void *cg_map_get(const CgMap *m, ForgeStr module, ForgeStr name) {
    if (!m->cap) return NULL;
    size_t mask = m->cap - 1;
    size_t i = (size_t)cg_key_hash(module, name) & mask;
    for (size_t probe = 0; probe < m->cap; probe++) {
        CgSlot *slot = &m->slots[(i + probe) & mask];
        if (!slot->value) return NULL;
        if (forge_str_eq(slot->module, module) && forge_str_eq(slot->name, name))
            return slot->value;
    }
    return NULL;
}

/* First writer wins, matching the "first match in declaration order" behaviour
 * of the linear scans this replaces. */
static void cg_map_put(CgMap *m, ForgeStr module, ForgeStr name, void *value) {
    if (!m->cap || !value) return;
    size_t mask = m->cap - 1;
    size_t i = (size_t)cg_key_hash(module, name) & mask;
    for (size_t probe = 0; probe < m->cap; probe++) {
        CgSlot *slot = &m->slots[(i + probe) & mask];
        if (!slot->value) {
            slot->module = module;
            slot->name = name;
            slot->value = value;
            return;
        }
        if (forge_str_eq(slot->module, module) && forge_str_eq(slot->name, name))
            return;
    }
    forge_die("symbol table full");
}

static void cg_map_free(CgMap *m) {
    free(m->slots);
    m->slots = NULL;
    m->cap = 0;
}

static FnDecl *lookup_module_fn(Codegen *cg, ForgeStr module, ForgeStr name) {
    if (module.len == 0) return NULL;
    return (FnDecl *)cg_map_get(&cg->fn_map, module, name);
}

static void cg_mark_moved(Codegen *cg, ForgeStr name) {
    for (size_t i = 0; i < cg->moved_count; i++) {
        if (forge_str_eq(cg->moved_locals[i], name)) return;
    }
    if (cg->moved_count == cg->moved_cap) {
        size_t cap = cg->moved_cap ? cg->moved_cap * 2 : 8;
        ForgeStr *grown = (ForgeStr *)realloc(cg->moved_locals, cap * sizeof(ForgeStr));
        if (!grown) forge_die("out of memory");
        cg->moved_locals = grown;
        cg->moved_cap = cap;
    }
    cg->moved_locals[cg->moved_count++] = name;
}

static int cg_is_moved(Codegen *cg, ForgeStr name) {
    for (size_t i = 0; i < cg->moved_count; i++) {
        if (forge_str_eq(cg->moved_locals[i], name)) return 1;
    }
    return 0;
}

static FnDecl *cg_lookup_fn(Codegen *cg, ForgeStr name) {
    return (FnDecl *)cg_map_get(&cg->fn_map, forge_str(""), name);
}

static size_t fn_param_count(FnDecl *fn) {
    size_t n = 0;
    if (!fn) return 0;
    for (Param *p = fn->params; p; p = p->next) n++;
    return n;
}

static FnDecl *cg_resolve_fn_ref(Codegen *cg, ForgeStr name) {
    FnDecl *fn = cg_lookup_fn(cg, name);
    if (fn) return fn;
    if (cg->current_module.len > 0)
        return lookup_module_fn(cg, cg->current_module, name);
    return NULL;
}

static void emit_fn_c_symbol(Codegen *cg, ForgeStr name, FnDecl *fn) {
    if (cg->current_module.len > 0 &&
        lookup_module_fn(cg, cg->current_module, name) == fn) {
        char sym[128];
        forge_mod_mangle(sym, sizeof(sym), cg->current_module, name);
        fputs(sym, cg->out);
    } else {
        fprintf(cg->out, "%.*s", (int)name.len, name.data);
    }
}

static void emit_expr(Codegen *cg, Expr *e);

static int emit_thread_call(Codegen *cg, ForgeStr name, Expr **args, size_t arg_count) {
    if (forge_str_eq(name, forge_str("thread_spawn")) && arg_count == 2) {
        Expr *fn_expr = args[0];
        if (fn_expr->kind != EXPR_IDENT) {
            fprintf(stderr, "forge: thread_spawn requires a function name\n");
            exit(1);
        }
        ForgeStr fn_name = fn_expr->as.ident;
        FnDecl *fn = cg_resolve_fn_ref(cg, fn_name);
        if (!fn || fn_param_count(fn) != 1) {
            fprintf(stderr, "forge: thread_spawn(fn, arg) requires fn(arg: int): int\n");
            exit(1);
        }
        fputs("fr_threading_spawn((fr_threading_fn1_t)", cg->out);
        emit_fn_c_symbol(cg, fn_name, fn);
        fputs(", ", cg->out);
        emit_expr(cg, args[1]);
        fputc(')', cg->out);
        return 1;
    }

    if (forge_str_eq(name, forge_str("thread_spawn_indexed")) && arg_count == 2) {
        Expr *fn_expr = args[0];
        if (fn_expr->kind != EXPR_IDENT) {
            fprintf(stderr, "forge: thread_spawn_indexed requires a function name\n");
            exit(1);
        }
        ForgeStr fn_name = fn_expr->as.ident;
        FnDecl *fn = cg_resolve_fn_ref(cg, fn_name);
        if (!fn || fn_param_count(fn) != 2) {
            fprintf(stderr, "forge: thread_spawn_indexed(fn, n) requires fn(id: int, total: int): int\n");
            exit(1);
        }
        fputs("fr_threading_spawn_indexed((fr_threading_fn2_t)", cg->out);
        emit_fn_c_symbol(cg, fn_name, fn);
        fputs(", ", cg->out);
        emit_expr(cg, args[1]);
        fputc(')', cg->out);
        return 1;
    }

    return 0;
}

static ConstDecl *cg_find_const(Codegen *cg, ForgeStr name) {
    return (ConstDecl *)cg_map_get(&cg->const_map, forge_str(""), name);
}

static int cg_lookup_const(Codegen *cg, ForgeStr name) {
    return cg_find_const(cg, name) != NULL;
}

static int cg_is_local(Codegen *cg, ForgeStr name) {
    for (size_t i = 0; i < cg->local_count; i++) {
        if (forge_str_eq(cg->locals[i].name, name)) return 1;
    }
    return 0;
}

/* Index every function and const once, up front, so emission never rescans. */
static void cg_build_symbols(Codegen *cg) {
    Program *prog = cg->prog;
    ForgeStr top = forge_str("");
    size_t n = prog->fn_count;
    if (prog->library.present) n += prog->library.fn_count;
    for (size_t i = 0; i < prog->module_count; i++) n += prog->modules[i].fn_count;
    cg_map_init(&cg->fn_map, n);
    cg_map_init(&cg->const_map, prog->const_count);

    /* externs are included too: their declared return type is what tells us
     * how to print/consume their result. */
    for (size_t i = 0; i < prog->fn_count; i++)
        cg_map_put(&cg->fn_map, top, prog->functions[i].name, &prog->functions[i]);
    if (prog->library.present) {
        for (size_t i = 0; i < prog->library.fn_count; i++)
            cg_map_put(&cg->fn_map, top, prog->library.functions[i].name,
                       &prog->library.functions[i]);
    }
    for (size_t i = 0; i < prog->module_count; i++) {
        FileModule *mod = &prog->modules[i];
        for (size_t j = 0; j < mod->fn_count; j++)
            cg_map_put(&cg->fn_map, mod->name, mod->functions[j].name, &mod->functions[j]);
    }
    for (size_t i = 0; i < prog->const_count; i++)
        cg_map_put(&cg->const_map, top, prog->consts[i].name, &prog->consts[i]);
}

static void cg_free_symbols(Codegen *cg) {
    cg_map_free(&cg->fn_map);
    cg_map_free(&cg->const_map);
}

static void cg_push_local(Codegen *cg, ForgeStr name, ForgeType ty) {
    for (size_t i = 0; i < cg->local_count; i++) {
        if (forge_str_eq(cg->locals[i].name, name)) {
            cg->locals[i].type = ty;
            return;
        }
    }
    if (cg->local_count == cg->local_cap) {
        size_t cap = cg->local_cap ? cg->local_cap * 2 : 8;
        struct ForgeLocal *grown =
            (struct ForgeLocal *)realloc(cg->locals, cap * sizeof(struct ForgeLocal));
        if (!grown) forge_die("out of memory");
        cg->locals = grown;
        cg->local_cap = cap;
    }
    cg->locals[cg->local_count].name = name;
    cg->locals[cg->local_count].type = ty;
    cg->local_count++;
}

static void cg_push_params(Codegen *cg, Param *params) {
    for (Param *p = params; p; p = p->next) cg_push_local(cg, p->name, p->type);
}

static ForgeType cg_lookup_local(Codegen *cg, ForgeStr name) {
    for (size_t i = 0; i < cg->local_count; i++) {
        if (forge_str_eq(cg->locals[i].name, name)) return cg->locals[i].type;
    }
    return forge_type_int();
}

/* Sorted for bsearch: the C names of the stdlib functions that return a
 * string. Keep in strcmp order. */
static const char *const STDLIB_STRING_FNS[] = {
    "fr_doc_get",
    "fr_fs_list_dir", "fr_fs_read", "fr_fs_temp_path",
    "fr_gpu_backend", "fr_gpu_device_name",
    "fr_http_get", "fr_http_post",
    "fr_http_req_body", "fr_http_req_method", "fr_http_req_path",
    "fr_io_prompt", "fr_io_read_fd", "fr_io_read_line", "fr_io_read_stdin",
    "fr_json_array_item", "fr_json_get_path_raw", "fr_json_get_path_string",
    "fr_json_get_string", "fr_json_stringify_int", "fr_json_stringify_str",
    "fr_lsp_read_message",
    "fr_os_argv", "fr_os_getenv",
    "fr_proc_output",
    "fr_str_append", "fr_str_append_str", "fr_str_concat", "fr_str_from_int",
    "fr_str_sub", "fr_str_trim",
    "fr_tcp_recv",
    "fr_udp_peer", "fr_udp_recv",
};

static int cg_str_fn_cmp(const void *key, const void *elem) {
    return strcmp((const char *)key, *(const char *const *)elem);
}

static int cg_c_name_returns_string(const char *mapped) {
    if (!mapped) return 0;
    return bsearch(mapped, STDLIB_STRING_FNS,
                   sizeof(STDLIB_STRING_FNS) / sizeof(STDLIB_STRING_FNS[0]),
                   sizeof(STDLIB_STRING_FNS[0]), cg_str_fn_cmp) != NULL;
}

/* Resolve a `module.fn(...)` target to its real declaration. File modules and
 * the library currently being compiled are known; a prebuilt library imported
 * as a binary + header is not, and resolution then fails (see CG_STR_UNKNOWN). */
static FnDecl *cg_resolve_qual_fn(Codegen *cg, ForgeStr module, ForgeStr name) {
    FnDecl *fn = lookup_module_fn(cg, module, name);
    if (fn) return fn;
    LibraryDecl *lib = &cg->prog->library;
    if (lib->present && forge_str_eq(lib->name, module)) {
        for (size_t i = 0; i < lib->fn_count; i++) {
            if (forge_str_eq(lib->functions[i].name, name)) return &lib->functions[i];
        }
    }
    return NULL;
}

/* Tri-state: whether an expression yields a C string. UNKNOWN means the
 * declaration is genuinely not visible to this compilation (an external
 * function), in which case the C compiler decides via _Generic instead of us
 * guessing from the spelling of the name. */
typedef enum { CG_STR_NO = 0, CG_STR_YES, CG_STR_UNKNOWN } CgStrKind;

static CgStrKind cg_type_str_kind(ForgeType ty) {
    return ty.kind == TY_STRING ? CG_STR_YES : CG_STR_NO;
}

static CgStrKind cg_expr_str_kind(Codegen *cg, Expr *e) {
    if (!e) return CG_STR_NO;
    switch (e->kind) {
    case EXPR_STRING:
        return CG_STR_YES;
    case EXPR_MOVE:
        return cg_expr_str_kind(cg, e->as.move_expr);
    case EXPR_IDENT: {
        if (cg_is_local(cg, e->as.ident))
            return cg_type_str_kind(cg_lookup_local(cg, e->as.ident));
        ConstDecl *c = cg_find_const(cg, e->as.ident);
        if (c) return (c->value && c->value->kind == EXPR_STRING) ? CG_STR_YES : CG_STR_NO;
        return cg_type_str_kind(cg_lookup_local(cg, e->as.ident));
    }
    case EXPR_CALL: {
        const char *mapped = forge_std_c_name(e->as.call.name, cg->imports, cg->import_count);
        if (mapped) return cg_c_name_returns_string(mapped) ? CG_STR_YES : CG_STR_NO;
        /* Covers top-level, library and module-internal functions alike. */
        FnDecl *fn = cg_resolve_fn_ref(cg, e->as.call.name);
        if (fn) return cg_type_str_kind(fn->ret_type);
        return CG_STR_UNKNOWN;
    }
    case EXPR_QUAL_CALL: {
        FnDecl *fn = cg_resolve_qual_fn(cg, e->as.qual_call.module, e->as.qual_call.name);
        if (fn) return cg_type_str_kind(fn->ret_type);
        return CG_STR_UNKNOWN;
    }
    default:
        return cg_type_str_kind(e->type);
    }
}

static int cg_expr_is_string(Codegen *cg, Expr *e) {
    return cg_expr_str_kind(cg, e) == CG_STR_YES;
}

static void cg_indent(Codegen *cg) {
    for (int i = 0; i < cg->indent; i++) fputs("    ", cg->out);
}

static void cg_line(Codegen *cg, const char *fmt, ...) {
    cg_indent(cg);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(cg->out, fmt, ap);
    va_end(ap);
    fputc('\n', cg->out);
}

static const char *c_type(ForgeType ty) {
    switch (ty.kind) {
    case TY_INT: return "int64_t";
    case TY_FLOAT: return "double";
    case TY_BOOL: return "int";
    case TY_STRING: return "const char*";
    case TY_PTR: return "void*";
    case TY_STRUCT:
        fprintf(stderr, "forge: internal error: use c_type_name for struct types\n");
        return "void*";
    default: return "void";
    }
}

static void c_type_name(FILE *out, ForgeType ty) {
    if (ty.kind == TY_STRUCT) {
        fprintf(out, "%.*s", (int)ty.struct_name.len, ty.struct_name.data);
    } else {
        fputs(c_type(ty), out);
    }
}

/* Emit `s` as a C string literal, escaping everything the C compiler could
 * misread. Shared by expression strings and const declarations so both paths
 * stay in sync. */
static void emit_c_string_literal(FILE *out, ForgeStr s) {
    fputc('"', out);
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.data[i];
        switch (c) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\t': fputs("\\t", out); break;
        case '\r': fputs("\\r", out); break;
        case '\a': fputs("\\a", out); break;
        case '\b': fputs("\\b", out); break;
        case '\f': fputs("\\f", out); break;
        case '\v': fputs("\\v", out); break;
        case '?': fputs("\\?", out); break; /* defeats trigraphs */
        default:
            /* Octal (never more than 3 digits) rather than \x, which would
             * greedily swallow a following hex digit. */
            if (c < 0x20 || c == 0x7f) fprintf(out, "\\%03o", c);
            else fputc((char)c, out);
            break;
        }
    }
    fputc('"', out);
}

static void emit_stmts(Codegen *cg, Stmt *s, const char *proc_var, bool in_coro);
static void emit_expr_inner(Codegen *cg, Expr *e);

static void emit_expr(Codegen *cg, Expr *e) {
    cg_enter(cg);
    emit_expr_inner(cg, e);
    cg_leave(cg);
}

static void emit_expr_inner(Codegen *cg, Expr *e) {
    switch (e->kind) {
    case EXPR_INT:
        fprintf(cg->out, "%lld", (long long)e->as.int_val);
        break;
    case EXPR_FLOAT:
        fprintf(cg->out, "%a", e->as.float_val);
        break;
    case EXPR_BOOL:
        fprintf(cg->out, "%s", e->as.bool_val ? "1" : "0");
        break;
    case EXPR_STRING:
        emit_c_string_literal(cg->out, e->as.string_val);
        break;
    case EXPR_IDENT:
        if (cg_is_moved(cg, e->as.ident)) {
            fprintf(stderr, "forge: use of moved value '%.*s'\n",
                    (int)e->as.ident.len, e->as.ident.data);
            exit(1);
        }
        if (!cg_is_local(cg, e->as.ident) && cg_lookup_const(cg, e->as.ident)) {
            fprintf(cg->out, "forge_const_%.*s", (int)e->as.ident.len, e->as.ident.data);
            break;
        }
        if (cg->state_prefix) fputs(cg->state_prefix, cg->out);
        fprintf(cg->out, "%.*s", (int)e->as.ident.len, e->as.ident.data);
        break;
    case EXPR_MOVE:
        if (e->as.move_expr && e->as.move_expr->kind == EXPR_IDENT) {
            ForgeStr name = e->as.move_expr->as.ident;
            cg_mark_moved(cg, name);
            fputs("fr_own_take(&", cg->out);
            if (cg->state_prefix) fputs(cg->state_prefix, cg->out);
            fprintf(cg->out, "%.*s)", (int)name.len, name.data);
        } else {
            emit_expr(cg, e->as.move_expr);
        }
        break;
    case EXPR_BINARY: {
        const char *op = "?";
        switch (e->as.binary.op) {
        case BIN_ADD: op = "+"; break;
        case BIN_SUB: op = "-"; break;
        case BIN_MUL: op = "*"; break;
        case BIN_DIV: op = "/"; break;
        case BIN_MOD: op = "%"; break;
        case BIN_EQ: op = "=="; break;
        case BIN_NE: op = "!="; break;
        case BIN_LT: op = "<"; break;
        case BIN_LE: op = "<="; break;
        case BIN_GT: op = ">"; break;
        case BIN_GE: op = ">="; break;
        case BIN_AND: op = "&&"; break;
        case BIN_OR: op = "||"; break;
        }
        fputc('(', cg->out);
        emit_expr(cg, e->as.binary.left);
        fprintf(cg->out, " %s ", op);
        emit_expr(cg, e->as.binary.right);
        fputc(')', cg->out);
        break;
    }
    case EXPR_CALL: {
        ForgeStr name = e->as.call.name;
        if (emit_thread_call(cg, name, e->as.call.args, e->as.call.arg_count)) break;
        if (forge_str_eq(name, forge_str("println"))) {
            fputs("({ ", cg->out);
            for (size_t i = 0; i < e->as.call.arg_count; i++) {
                Expr *arg = e->as.call.args[i];
                if (i > 0) fputs(" ", cg->out);
                switch (cg_expr_str_kind(cg, arg)) {
                case CG_STR_YES: fputs("fr_print_str(", cg->out); break;
                case CG_STR_NO: fputs("fr_print_int(", cg->out); break;
                /* Declaration not visible here (external library function):
                 * let the C compiler pick the printer from the real type
                 * instead of guessing from the function's name. */
                case CG_STR_UNKNOWN: fputs("FORGE_PRINT_AUTO(", cg->out); break;
                }
                emit_expr(cg, arg);
                fputs("); ", cg->out);
            }
            fputs("fr_println(); })", cg->out);
            break;
        }
        const char *c_fn = forge_std_c_name(name, cg->imports, cg->import_count);
        FnDecl *mod_fn = cg->current_module.len > 0
                             ? lookup_module_fn(cg, cg->current_module, name)
                             : NULL;
        if (c_fn) {
            fprintf(cg->out, "%s(", c_fn);
        } else if (mod_fn) {
            char sym[128];
            forge_mod_mangle(sym, sizeof(sym), cg->current_module, name);
            fprintf(cg->out, "%s(", sym);
            e->type = mod_fn->ret_type;
        } else {
            FnDecl *fn = cg_lookup_fn(cg, name);
            if (fn) e->type = fn->ret_type;
            fprintf(cg->out, "%.*s(", (int)name.len, name.data);
        }
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            if (i) fputc(',', cg->out);
            emit_expr(cg, e->as.call.args[i]);
        }
        fputc(')', cg->out);
        break;
    }
    case EXPR_RECV:
        fputs("(fr_recv(fr_coro_process(__coro), &__recv_msg), __recv_msg.value)", cg->out);
        break;
    case EXPR_QUAL_CALL: {
        char sym[128];
        FnDecl *qfn = cg_resolve_qual_fn(cg, e->as.qual_call.module, e->as.qual_call.name);
        if (qfn) e->type = qfn->ret_type;
        if (forge_import_is_file_module(cg->prog, e->as.qual_call.module))
            forge_mod_mangle(sym, sizeof(sym), e->as.qual_call.module, e->as.qual_call.name);
        else
            forge_lib_mangle(sym, sizeof(sym), e->as.qual_call.module, e->as.qual_call.name);
        fprintf(cg->out, "%s(", sym);
        for (size_t i = 0; i < e->as.qual_call.arg_count; i++) {
            if (i) fputc(',', cg->out);
            emit_expr(cg, e->as.qual_call.args[i]);
        }
        fputc(')', cg->out);
        break;
    }
    case EXPR_INDEX: {
        ForgeType base_ty = e->as.index.base->type;
        if (base_ty.kind == TY_STRING || cg_expr_is_string(cg, e->as.index.base)) {
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
    }
    case EXPR_FIELD:
        emit_expr(cg, e->as.field.base);
        fprintf(cg->out, ".%.*s", (int)e->as.field.field.len, e->as.field.field.data);
        break;
    }
}

/* Does this single statement (recursing into its own nested bodies only --
 * never into its siblings) contain a yield/await? Used to decide whether a
 * compound statement needs a re-entry case label, and whether a loop can be
 * emitted as a plain C loop. */
static bool stmt_has_yield(Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case STMT_YIELD:
    case STMT_AWAIT:
        return true;
    case STMT_IF: {
        for (Stmt *b = s->as.if_stmt.then_br->first; b; b = b->next)
            if (stmt_has_yield(b)) return true;
        if (s->as.if_stmt.else_br)
            for (Stmt *b = s->as.if_stmt.else_br->first; b; b = b->next)
                if (stmt_has_yield(b)) return true;
        return false;
    }
    case STMT_WHILE:
        for (Stmt *b = s->as.while_stmt.body->first; b; b = b->next)
            if (stmt_has_yield(b)) return true;
        return false;
    case STMT_BLOCK:
        for (Stmt *b = s->as.block->first; b; b = b->next)
            if (stmt_has_yield(b)) return true;
        return false;
    default:
        return false;
    }
}

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

static int cg_is_hoisted(Codegen *cg, Stmt *s) {
    for (size_t i = 0; i < cg->hoisted_count; i++)
        if (cg->hoisted[i] == s) return 1;
    return 0;
}

static int stmt_list_has_name(StmtList *l, ForgeStr name) {
    for (size_t i = 0; i < l->count; i++)
        if (forge_str_eq(l->items[i]->as.let.name, name)) return 1;
    return 0;
}

/* Collect every `let` in a coroutine body, including ones nested in if/while/
 * for/block/match bodies: body emission rewrites all of them into state-struct
 * fields, so the struct has to declare all of them or the generated C
 * references undeclared members. Duplicate names share one slot. */
static void collect_coro_lets(Stmt *s, StmtList *out) {
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_LET:
            if (!stmt_list_has_name(out, s->as.let.name)) stmt_list_push(out, s);
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

static void emit_coro_spawn_inits(Codegen *cg, CoroDecl *coro) {
    StmtList hoisted = { NULL, 0, 0 };
    collect_hoisted_lets(coro, &hoisted);
    for (size_t i = 0; i < hoisted.count; i++) {
        Stmt *s = hoisted.items[i];
        cg_indent(cg);
        fprintf(cg->out, "init->%.*s = ", (int)s->as.let.name.len, s->as.let.name.data);
        const char *saved = cg->state_prefix;
        cg->state_prefix = "init->";
        emit_expr(cg, s->as.let.init);
        cg->state_prefix = saved;
        fputs(";\n", cg->out);
    }
    free(hoisted.items);
}

static int param_named(Param *params, ForgeStr name) {
    for (Param *p = params; p; p = p->next)
        if (forge_str_eq(p->name, name)) return 1;
    return 0;
}

static void emit_coro_state_struct(Codegen *cg, CoroDecl *coro) {
    fprintf(cg->out, "typedef struct {\n");
    fprintf(cg->out, "    int _forge_step;\n");
    fprintf(cg->out, "    fr_process_t *proc;\n");
    fprintf(cg->out, "    int64_t _forge_ret;\n");
    for (Param *p = coro->params; p; p = p->next) {
        fputs("    ", cg->out);
        c_type_name(cg->out, p->type);
        fprintf(cg->out, " %.*s;\n", (int)p->name.len, p->name.data);
    }
    StmtList lets = { NULL, 0, 0 };
    collect_coro_lets(coro->body.first, &lets);
    for (size_t i = 0; i < lets.count; i++) {
        Stmt *s = lets.items[i];
        if (param_named(coro->params, s->as.let.name)) continue;
        fputs("    ", cg->out);
        c_type_name(cg->out, s->as.let.type);
        fprintf(cg->out, " %.*s;\n", (int)s->as.let.name.len, s->as.let.name.data);
    }
    free(lets.items);
    fprintf(cg->out, "} %.*s_state_t;\n\n", (int)coro->name.len, coro->name.data);
}

static void emit_coro_body(Codegen *cg, CoroDecl *coro, const char *state_var, int *step) {
    Stmt *s = coro->body.first;
    cg_enter(cg);
    while (s) {
        if (s->kind == STMT_YIELD) {
            int resume = ++(*step);
            cg_line(cg, "    fr_coro_set_step(__coro, %d);", resume);
            cg_line(cg, "    return fr_yield(__coro);");
            cg_line(cg, "case %d:", resume);
            s = s->next;
            continue;
        }

        if (s->kind == STMT_AWAIT) {
            int resume = ++(*step);
            cg_line(cg, "    fr_coro_set_step(__coro, %d);", resume);
            cg_indent(cg);
            fputs("if (!fr_await_fd(__coro, ", cg->out);
            emit_expr(cg, s->as.await_expr);
            fputs(", FR_EVENT_READ)) return fr_yield(__coro);\n", cg->out);
            cg_line(cg, "case %d:", resume);
            s = s->next;
            continue;
        }

        if (s->kind == STMT_LET) {
            /* The spawn stub already ran this initialiser into the state
             * struct; re-running it here would duplicate its side effects. */
            if (cg_is_hoisted(cg, s)) {
                s = s->next;
                continue;
            }
            cg_indent(cg);
            if (cg->state_prefix) fputs(cg->state_prefix, cg->out);
            fprintf(cg->out, "%.*s = ", (int)s->as.let.name.len, s->as.let.name.data);
            if (s->as.let.init) emit_expr(cg, s->as.let.init);
            else fputs("0", cg->out);
            fputs(";\n", cg->out);
            s = s->next;
            continue;
        }

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
            emit_coro_body(cg, &(CoroDecl){ .body = *s->as.if_stmt.then_br }, state_var, step);
            cg->indent--;
            cg_indent(cg);
            fputs("}", cg->out);
            if (s->as.if_stmt.else_br) {
                fputs(" else {\n", cg->out);
                cg->indent++;
                emit_coro_body(cg, &(CoroDecl){ .body = *s->as.if_stmt.else_br }, state_var, step);
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
            emit_coro_body(cg, &(CoroDecl){ .body = *s->as.while_stmt.body }, state_var, step);
            cg->indent--;
            cg_line(cg, "}");
            break;
        case STMT_SPAWN:
            cg_line(cg, "%.*s_spawn(%s->proc", (int)s->as.spawn.coro_name.len,
                    s->as.spawn.coro_name.data, state_var);
            for (size_t i = 0; i < s->as.spawn.arg_count; i++) {
                fputs(", ", cg->out);
                emit_expr(cg, s->as.spawn.args[i]);
            }
            fputs(");\n", cg->out);
            break;
        case STMT_SEND:
            cg_indent(cg);
            if (s->as.send.move_) {
                fputs("fr_send_move(", cg->out);
                emit_expr(cg, s->as.send.target);
                fprintf(cg->out, ", %d, ", s->as.send.tag);
                emit_expr(cg, s->as.send.value);
                fputs(");\n", cg->out);
            } else {
                fputs("fr_send(", cg->out);
                emit_expr(cg, s->as.send.target);
                fprintf(cg->out, ", %d, ", s->as.send.tag);
                emit_expr(cg, s->as.send.value);
                fputs(", NULL, 0);\n", cg->out);
            }
            break;
        case STMT_ASSIGN:
            cg_indent(cg);
            if (cg->state_prefix) fputs(cg->state_prefix, cg->out);
            fprintf(cg->out, "%.*s = ", (int)s->as.assign.name.len, s->as.assign.name.data);
            emit_expr(cg, s->as.assign.value);
            fputs(";\n", cg->out);
            break;
        case STMT_BLOCK:
            cg->indent++;
            emit_coro_body(cg, &(CoroDecl){ .body = *s->as.block }, state_var, step);
            cg->indent--;
            break;
        default:
            break;
        }
        s = s->next;
    }
    cg_leave(cg);
}

static void emit_coro_fn(Codegen *cg, CoroDecl *coro) {
    emit_coro_state_struct(cg, coro);
    fprintf(cg->out, "static fr_coro_status_t %.*s_fn(fr_coro_t *__coro, void *__userdata) {\n",
            (int)coro->name.len, coro->name.data);
    fprintf(cg->out, "    %.*s_state_t *%s = (%.*s_state_t *)__userdata;\n",
            (int)coro->name.len, coro->name.data, "st",
            (int)coro->name.len, coro->name.data);
    fputs("    fr_msg_t __recv_msg;\n", cg->out);
    fputs("    switch (fr_coro_step(__coro)) {\n", cg->out);
    cg->indent++;
    cg_line(cg, "default:");
    const char *saved_prefix = cg->state_prefix;
    cg->state_prefix = "st->";
    cg_begin_function(cg);
    for (Param *p = coro->params; p; p = p->next) cg_push_local(cg, p->name, p->type);
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
    emit_coro_body(cg, coro, "st", &step);

    cg->hoisted = NULL;
    cg->hoisted_count = 0;
    free(hoisted.items);
    cg->state_prefix = saved_prefix;
    cg->local_count = 0;
    cg->indent--;
    fputs("    }\n", cg->out);
    fputs("    return FR_CORO_DONE;\n", cg->out);
    fputs("}\n\n", cg->out);

    cg_begin_function(cg);
    fprintf(cg->out, "static void %.*s_spawn(fr_process_t *proc", (int)coro->name.len, coro->name.data);
    for (Param *p = coro->params; p; p = p->next) {
        fputs(", ", cg->out);
        c_type_name(cg->out, p->type);
        fprintf(cg->out, " %.*s", (int)p->name.len, p->name.data);
    }
    fputs(") {\n", cg->out);
    fprintf(cg->out, "    %.*s_state_t *init = (%.*s_state_t *)calloc(1, sizeof(%.*s_state_t));\n",
            (int)coro->name.len, coro->name.data,
            (int)coro->name.len, coro->name.data,
            (int)coro->name.len, coro->name.data);
    fputs("    if (!init) { fprintf(stderr, \"forge: out of memory\\n\"); abort(); }\n", cg->out);
    fputs("    init->proc = proc;\n", cg->out);
    for (Param *p = coro->params; p; p = p->next) {
        fprintf(cg->out, "    init->%.*s = %.*s;\n", (int)p->name.len, p->name.data,
                (int)p->name.len, p->name.data);
    }
    emit_coro_spawn_inits(cg, coro);
    fprintf(cg->out, "    fr_coro_spawn(proc, %.*s_fn, init, sizeof(%.*s_state_t));\n",
            (int)coro->name.len, coro->name.data,
            (int)coro->name.len, coro->name.data);
    fputs("}\n\n", cg->out);
}

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
        if (es && !es->next && es->kind == STMT_IF) {
            fputs(" else ", cg->out);
            emit_if_stmt(cg, es, proc_var, in_coro);
            fputc('\n', cg->out);
        } else {
            fputs(" else {\n", cg->out);
            cg->indent++;
            emit_stmts(cg, s->as.if_stmt.else_br->first, proc_var, in_coro);
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
            fprintf(cg->out, "%.*s = ", (int)s->as.assign.name.len, s->as.assign.name.data);
            emit_expr(cg, s->as.assign.value);
        } else {
            emit_expr(cg, s->as.expr);
        }
    }
}

static bool block_has_yield(Stmt *first) {
    for (Stmt *b = first; b; b = b->next)
        if (stmt_has_yield(b)) return true;
    return false;
}

static void emit_stmts(Codegen *cg, Stmt *s, const char *proc_var, bool in_coro) {
    cg_enter(cg);
    while (s) {
        switch (s->kind) {
        case STMT_LET:
            cg_push_local(cg, s->as.let.name, s->as.let.type);
            cg_indent(cg);
            c_type_name(cg->out, s->as.let.type);
            fprintf(cg->out, " %.*s", (int)s->as.let.name.len, s->as.let.name.data);
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
        case STMT_WHILE: {
            cg_line(cg, "while (");
            emit_expr(cg, s->as.while_stmt.cond);
            fputs(") {\n", cg->out);
            cg->indent++;
            /* C's own `continue` is correct for a while loop. */
            int saved_cont = cg->continue_label;
            cg->continue_label = 0;
            emit_stmts(cg, s->as.while_stmt.body->first, proc_var, in_coro);
            cg->continue_label = saved_cont;
            cg->indent--;
            cg_indent(cg);
            fputs("}\n", cg->out);
            break;
        }
        case STMT_FOR: {
            bool suspends = block_has_yield(s->as.for_stmt.body->first);
            bool inline_step = for_step_inlinable(s->as.for_stmt.step);
            int saved_cont = cg->continue_label;
            cg_indent(cg);
            fputs("{\n", cg->out);
            cg->indent++;
            if (s->as.for_stmt.init) emit_stmts(cg, s->as.for_stmt.init, proc_var, in_coro);
            if (!suspends && inline_step) {
                /* Real C for-loop: `continue` then runs the step, as it must.
                 * The while-desugaring below skips it and spins forever. */
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
                cg->indent--;
                cg_indent(cg);
                fputs("}\n", cg->out);
            } else {
                /* Suspending body (or a step we cannot inline): desugar to a
                 * while loop, and route `continue` to a label placed just
                 * before the step so the step still runs. */
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
                cg->indent--;
                cg_indent(cg);
                fputs("}\n", cg->out);
            }
            cg->indent--;
            cg_indent(cg);
            fputs("}\n", cg->out);
            break;
        }
        case STMT_BREAK:
            cg_line(cg, "break;");
            break;
        case STMT_CONTINUE:
            if (cg->continue_label) cg_line(cg, "goto __forge_cont_%d;", cg->continue_label);
            else cg_line(cg, "continue;");
            break;
        case STMT_SPAWN:
            cg_indent(cg);
            fprintf(cg->out, "%.*s_spawn(%s", (int)s->as.spawn.coro_name.len,
                    s->as.spawn.coro_name.data, proc_var);
            for (size_t i = 0; i < s->as.spawn.arg_count; i++) {
                fputs(", ", cg->out);
                emit_expr(cg, s->as.spawn.args[i]);
            }
            fputs(");\n", cg->out);
            break;
        case STMT_SEND:
            cg_indent(cg);
            if (s->as.send.move_) {
                fputs("fr_send_move(", cg->out);
                emit_expr(cg, s->as.send.target);
                fprintf(cg->out, ", %d, ", s->as.send.tag);
                emit_expr(cg, s->as.send.value);
                fputs(");\n", cg->out);
            } else {
                fputs("fr_send(", cg->out);
                emit_expr(cg, s->as.send.target);
                fprintf(cg->out, ", %d, ", s->as.send.tag);
                emit_expr(cg, s->as.send.value);
                fputs(", NULL, 0);\n", cg->out);
            }
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
            cg_indent(cg);
            fprintf(cg->out, "%.*s = ", (int)s->as.assign.name.len, s->as.assign.name.data);
            emit_expr(cg, s->as.assign.value);
            fputs(";\n", cg->out);
            break;
        case STMT_MATCH: {
            cg_indent(cg);
            fputs("{\n", cg->out);
            cg->indent++;
            cg_indent(cg);
            fputs("int64_t __match_val = ", cg->out);
            emit_expr(cg, s->as.match_stmt.scrutinee);
            fputs(";\n", cg->out);
            MatchArm *arm = s->as.match_stmt.arms;
            int first = 1;
            while (arm) {
                /* A wildcard matches everything, so anything after it is
                 * unreachable - and a second wildcard would emit `else else`. */
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
                first = 0;
                arm = arm->next;
            }
            cg->indent--;
            cg_line(cg, "}");
            break;
        }
        case STMT_BLOCK:
            cg->indent++;
            emit_stmts(cg, s->as.block->first, proc_var, in_coro);
            cg->indent--;
            break;
        }
        s = s->next;
    }
    cg_leave(cg);
}

static ProcessDecl *find_process(Program *prog, ForgeStr name) {
    for (size_t i = 0; i < prog->process_count; i++) {
        if (forge_str_eq(prog->processes[i].name, name)) return &prog->processes[i];
    }
    return NULL;
}

static void emit_consts(Program *prog, FILE *out) {
    for (size_t i = 0; i < prog->const_count; i++) {
        ConstDecl *c = &prog->consts[i];
        ExprKind k = c->value ? c->value->kind : EXPR_IDENT;
        switch (k) {
        case EXPR_INT:
            fprintf(out, "static const int64_t forge_const_%.*s = %lld;\n",
                    (int)c->name.len, c->name.data, (long long)c->value->as.int_val);
            break;
        case EXPR_FLOAT:
            fprintf(out, "static const double forge_const_%.*s = %a;\n",
                    (int)c->name.len, c->name.data, c->value->as.float_val);
            break;
        case EXPR_BOOL:
            fprintf(out, "static const int forge_const_%.*s = %d;\n",
                    (int)c->name.len, c->name.data, c->value->as.bool_val ? 1 : 0);
            break;
        case EXPR_STRING:
            fprintf(out, "static const char *forge_const_%.*s = ",
                    (int)c->name.len, c->name.data);
            emit_c_string_literal(out, c->value->as.string_val);
            fputs(";\n", out);
            break;
        default:
            /* Dropping it silently would leave every `forge_const_NAME`
             * reference dangling in the generated C. */
            fprintf(stderr, "forge: const '%.*s' must be an int, float, bool or "
                            "string literal\n", (int)c->name.len, c->name.data);
            exit(1);
        }
    }
    if (prog->const_count > 0) fputs("\n", out);
}

static void emit_import_headers(Program *prog, FILE *out) {
    for (size_t i = 0; i < prog->import_count; i++) {
        if (forge_import_is_file_module(prog, prog->imports[i])) continue;
        const char *hdr = forge_std_header(prog->imports[i]);
        if (hdr) fprintf(out, "#include \"%s\"\n", hdr);
        else fprintf(out, "#include \"%.*s.h\"\n", (int)prog->imports[i].len, prog->imports[i].data);
    }
}

static void emit_fn_signature(FILE *out, const char *name, FnDecl *fn) {
    c_type_name(out, fn->ret_type);
    if (name && name[0]) fprintf(out, " %s", name);
    fputs("(", out);
    Param *p = fn->params;
    bool first = true;
    while (p) {
        if (!first) fputs(", ", out);
        c_type_name(out, p->type);
        fprintf(out, " %.*s", (int)p->name.len, p->name.data);
        first = false;
        p = p->next;
    }
    fputs(")", out);
}

static void emit_structs(Program *prog, FILE *out) {
    for (size_t i = 0; i < prog->struct_count; i++) {
        StructDecl *sd = &prog->structs[i];
        fprintf(out, "typedef struct {\n");
        for (Field *f = sd->fields; f; f = f->next) {
            fputs("    ", out);
            c_type_name(out, f->type);
            fprintf(out, " %.*s;\n", (int)f->name.len, f->name.data);
        }
        fprintf(out, "} %.*s;\n\n", (int)sd->name.len, sd->name.data);
    }
}

static void emit_enums(Program *prog, FILE *out) {
    for (size_t i = 0; i < prog->enum_count; i++) {
        EnumDecl *ed = &prog->enums[i];
        fprintf(out, "typedef enum {\n");
        for (EnumVariant *v = ed->variants; v; v = v->next) {
            fprintf(out, "    %.*s_%.*s = %lld,\n",
                    (int)ed->name.len, ed->name.data,
                    (int)v->name.len, v->name.data,
                    (long long)v->value);
        }
        fprintf(out, "} %.*s;\n\n", (int)ed->name.len, ed->name.data);
    }
}

/* Used for calls whose declaration this compilation cannot see (functions in a
 * prebuilt library). The C compiler knows their real prototype from the
 * generated header, so let it pick the printer rather than guessing here. */
static void emit_print_auto_macro(FILE *out) {
    fputs("#define FORGE_PRINT_AUTO(x) _Generic((x), \\\n"
          "    char*: fr_print_str, const char*: fr_print_str, \\\n"
          "    default: fr_print_int)(x)\n", out);
}

static NativeDecl *find_native(Program *prog, ForgeStr name) {
    for (size_t i = 0; i < prog->native_count; i++) {
        if (forge_str_eq(prog->natives[i].name, name)) return &prog->natives[i];
    }
    return NULL;
}

void codegen_emit_library(Program *prog, FILE *out_c, FILE *out_h, const char *runtime_include) {
    if (!prog->library.present) {
        fprintf(stderr, "forge: no library block found\n");
        exit(1);
    }
    LibraryDecl *lib = &prog->library;
    /* Heap-allocated: a fixed buffer would silently truncate (and so collide)
     * on a long library name. */
    size_t guard_cap = lib->name.len + 16;
    char *guard = (char *)malloc(guard_cap);
    if (!guard) forge_die("out of memory");
    snprintf(guard, guard_cap, "FRLIB_%.*s_H", (int)lib->name.len, lib->name.data);
    for (char *p = guard; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    }

    if (!out_h && !out_c) {
        free(guard);
        fprintf(stderr, "forge: library mode requires header or C output\n");
        exit(1);
    }

    if (out_h) {
        fprintf(out_h, "#ifndef %s\n#define %s\n\n", guard, guard);
        fprintf(out_h, "#include <stdint.h>\n\n");
        for (size_t i = 0; i < lib->fn_count; i++) {
            char sym[128];
            forge_lib_mangle(sym, sizeof(sym), lib->name, lib->functions[i].name);
            emit_fn_signature(out_h, sym, &lib->functions[i]);
            fputs(";\n", out_h);
        }
        fprintf(out_h, "\n#endif\n");
    }

    if (!out_c) {
        free(guard);
        return;
    }
    free(guard);

    fprintf(out_c, "// Generated by Forge compiler (library mode)\n");
    fprintf(out_c, "#include \"%s\"\n", runtime_include);
    fputs("#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n", out_c);
    for (size_t i = 0; i < lib->import_count; i++) {
        const char *hdr = forge_std_header(lib->imports[i]);
        if (hdr) fprintf(out_c, "#include \"%s\"\n", hdr);
        else fprintf(out_c, "#include \"%.*s.h\"\n", (int)lib->imports[i].len, lib->imports[i].data);
    }
    emit_print_auto_macro(out_c);
    fputs("\n", out_c);

    Codegen cg = { .out = out_c, .prog = prog,
                   .imports = lib->imports, .import_count = lib->import_count };
    cg_build_symbols(&cg);
    for (size_t i = 0; i < lib->fn_count; i++) {
        FnDecl *fn = &lib->functions[i];
        char sym[128];
        forge_lib_mangle(sym, sizeof(sym), lib->name, fn->name);
        emit_fn_signature(out_c, sym, fn);
        fputs(" {\n", out_c);
        cg.indent = 1;
        cg_begin_function(&cg);
        cg_push_params(&cg, fn->params);
        emit_stmts(&cg, fn->body.first, NULL, false);
        cg.indent = 0;
        fputs("}\n\n", out_c);
    }
    free(cg.moved_locals);
    free(cg.locals);
    cg_free_symbols(&cg);
}

void codegen_emit(Program *prog, FILE *out, const char *runtime_include) {
    Codegen cg = { .out = out, .prog = prog,
                   .imports = prog->imports, .import_count = prog->import_count };
    cg_build_symbols(&cg);

    fputs("// Generated by Forge compiler (native backend)\n", out);
    fprintf(out, "#include \"%s\"\n", runtime_include);
    fputs("#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n", out);
    fputs("#include \"forge/os.h\"\n", out);
    fputs("#include \"forge/ownership.h\"\n", out);
    fputs("#include \"forge/event.h\"\n", out);
    emit_import_headers(prog, out);
    emit_print_auto_macro(out);
    fputs("\n", out);

    emit_consts(prog, out);

    emit_structs(prog, out);
    emit_enums(prog, out);

    for (size_t i = 0; i < prog->fn_count; i++) {
        FnDecl *fn = &prog->functions[i];
        if (fn->is_extern) {
            fputs("extern ", out);
            char *fn_name = forge_strdup(fn->name);
            if (!fn_name) forge_die("out of memory");
            emit_fn_signature(out, fn_name, fn);
            free(fn_name);
            fputs(";\n", out);
        }
    }
    fputs("\n", out);

    for (size_t i = 0; i < prog->fn_count; i++) {
        FnDecl *fn = &prog->functions[i];
        if (fn->is_extern) continue;
        fputs("static ", out);
        char *fn_name = forge_strdup(fn->name);
        if (!fn_name) forge_die("out of memory");
        emit_fn_signature(out, fn_name, fn);
        free(fn_name);
        fputs(";\n", out);
    }
    fputs("\n", out);

    for (size_t i = 0; i < prog->process_count; i++) {
        ProcessDecl *proc = &prog->processes[i];
        for (size_t j = 0; j < proc->coro_count; j++) {
            emit_coro_fn(&cg, &proc->coros[j]);
        }
    }

    for (size_t i = 0; i < prog->fn_count; i++) {
        FnDecl *fn = &prog->functions[i];
        if (fn->is_extern) continue;
        fputs("static ", out);
        char *fn_name = forge_strdup(fn->name);
        if (!fn_name) forge_die("out of memory");
        emit_fn_signature(out, fn_name, fn);
        free(fn_name);
        fputs(" {\n", out);
        cg.indent = 1;
        cg_begin_function(&cg);
        cg.current_module = forge_str("");
        cg_push_params(&cg, fn->params);
        emit_stmts(&cg, fn->body.first, NULL, false);
        cg.indent = 0;
        fputs("}\n\n", out);
    }

    for (size_t i = 0; i < prog->module_count; i++) {
        FileModule *mod = &prog->modules[i];
        for (size_t j = 0; j < mod->fn_count; j++) {
            FnDecl *fn = &mod->functions[j];
            fputs("static ", out);
            char sym[128];
            forge_mod_mangle(sym, sizeof(sym), mod->name, fn->name);
            emit_fn_signature(out, sym, fn);
            fputs(" {\n", out);
            cg.indent = 1;
            cg_begin_function(&cg);
            cg.current_module = mod->name;
            cg_push_params(&cg, fn->params);
            emit_stmts(&cg, fn->body.first, NULL, false);
            cg.indent = 0;
            fputs("}\n\n", out);
        }
        cg.current_module = forge_str("");
    }

    for (size_t i = 0; i < prog->process_count; i++) {
        ProcessDecl *pd = &prog->processes[i];
        if (!pd->body.first) continue;
        fprintf(out, "static void %.*s_init(fr_process_t *proc) {\n", (int)pd->name.len, pd->name.data);
        cg.indent = 1;
        cg_begin_function(&cg);
        emit_stmts(&cg, pd->body.first, "proc", false);
        cg.indent = 0;
        fputs("}\n\n", out);
    }

    ForgeStr entry_name = forge_str("main");
    NativeDecl *native_main = find_native(prog, entry_name);

    if (native_main) {
        fputs("int main(int argc, char **argv) {\n", out);
        cg.indent = 1;
        cg_begin_function(&cg);
        cg_line(&cg, "fr_os_set_args(argc, argv);");
        emit_stmts(&cg, native_main->body.first, NULL, false);
        cg_line(&cg, "return 0;");
        cg.indent = 0;
        fputs("}\n", out);
        free(cg.moved_locals);
        free(cg.locals);
        cg_free_symbols(&cg);
        return;
    }

    ProcessDecl *entry = find_process(prog, entry_name);
    if (!entry && prog->process_count > 0) entry = &prog->processes[0];

    if (!entry) {
        fputs("int main(void) { return 0; }\n", out);
        free(cg.moved_locals);
        free(cg.locals);
        cg_free_symbols(&cg);
        return;
    }

    fputs("int main(int argc, char **argv) {\n", out);
    cg.indent = 1;
    cg_begin_function(&cg);
    cg_line(&cg, "fr_os_set_args(argc, argv);");
    cg_line(&cg, "fr_scheduler_t *sched = fr_scheduler_create(0);");

    for (size_t i = 0; i < prog->process_count; i++) {
        ProcessDecl *pd = &prog->processes[i];
        fprintf(out, "    fr_process_t *proc_%.*s = fr_process_create(\"%.*s\");\n",
                (int)pd->name.len, pd->name.data, (int)pd->name.len, pd->name.data);
        cg_line(&cg, "fr_scheduler_add_process(sched, proc_%.*s);", (int)pd->name.len, pd->name.data);
    }

    for (size_t i = 0; i < prog->supervisor_count; i++) {
        SupervisorDecl *sup = &prog->supervisors[i];
        const char *pol = "FR_RESTART_PROCESS";
        if (sup->policy == SUP_RESTART_CORO) pol = "FR_RESTART_CORO";
        else if (sup->policy == SUP_RESTART_ALL) pol = "FR_RESTART_ALL";
        fprintf(out, "    fr_process_t *sup_%.*s = fr_supervisor_create(\"%.*s\", %s);\n",
                (int)sup->name.len, sup->name.data, (int)sup->name.len, sup->name.data, pol);
        for (size_t j = 0; j < sup->child_count; j++) {
            ProcessDecl *child = find_process(prog, sup->children[j]);
            if (child) {
                fprintf(out, "    fr_supervisor_add_child(sup_%.*s, proc_%.*s);\n",
                        (int)sup->name.len, sup->name.data,
                        (int)child->name.len, child->name.data);
            }
        }
        cg_line(&cg, "fr_scheduler_add_process(sched, sup_%.*s);", (int)sup->name.len, sup->name.data);
    }

    for (size_t i = 0; i < prog->process_count; i++) {
        ProcessDecl *pd = &prog->processes[i];
        if (pd->body.first) {
            fprintf(out, "    %.*s_init(proc_%.*s);\n",
                    (int)pd->name.len, pd->name.data,
                    (int)pd->name.len, pd->name.data);
        }
    }

    cg_line(&cg, "fr_scheduler_run(sched);");
    cg_line(&cg, "fr_scheduler_destroy(sched);");
    cg_line(&cg, "return 0;");
    cg.indent = 0;
    fputs("}\n", out);
    free(cg.moved_locals);
    free(cg.locals);
    cg_free_symbols(&cg);
}
