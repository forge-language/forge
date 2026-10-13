#include "codegen.h"
#include "mod_registry.h"
#include <stdarg.h>
#include <string.h>

/* Guard against unbounded recursion over deeply nested ASTs: emitting a
 * 60k-deep expression tree overflows the C stack long before it overflows
 * anything else. */
#define CG_MAX_DEPTH 3000
#define CG_LOOP_SCAN_BUDGET 65536

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
    bool prove_loop_increments; /* plain native/library functions only */
    size_t loop_scan_left;      /* fail closed when nested scans exhaust it */
    Stmt *safe_increment;       /* one guard-dominated assignment, never ADD globally */
    ForgeStr safe_view_at_view;
    ForgeStr safe_view_at_index;
    struct { ForgeStr parameter; char name[64]; } invariant_mods[8];
    size_t invariant_mod_count;
} Codegen;

/* Match x*C[+K] % (2^k-1) with a local integer x and positive constants.
 * The emitted helper guards the fold's nonnegative range and signed-product
 * bound; its fallback keeps the original wrapping arithmetic and signed mod. */
static bool cg_mersenne_modmul(Codegen *cg, Expr *e, Expr **value,
                               int64_t *multiplier, int64_t *modulus,
                               int64_t *addend, unsigned *shift);

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
    cg->prove_loop_increments = false;
    cg->loop_scan_left = CG_LOOP_SCAN_BUDGET;
    cg->safe_increment = NULL;
    cg->safe_view_at_view = (ForgeStr){0};
    cg->safe_view_at_index = (ForgeStr){0};
    cg->invariant_mod_count = 0;
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
    size_t maximum = SIZE_MAX / sizeof(CgSlot);
    if (expected > (maximum - 1) / 2) forge_die("too many codegen symbols");
    size_t needed = expected * 2 + 1;
    size_t cap = 16;
    while (cap < needed) {
        if (cap > maximum / 2) forge_die("too many codegen symbols");
        cap *= 2;
    }
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
    cg->moved_locals = forge_array_reserve(cg->moved_locals, cg->moved_count,
        &cg->moved_cap, sizeof(*cg->moved_locals), 8);
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
    if (prog->library.present) {
        if (prog->library.fn_count > SIZE_MAX - n) forge_die("too many codegen symbols");
        n += prog->library.fn_count;
    }
    for (size_t i = 0; i < prog->module_count; i++) {
        if (prog->modules[i].fn_count > SIZE_MAX - n) forge_die("too many codegen symbols");
        n += prog->modules[i].fn_count;
    }
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
    cg->locals = forge_array_reserve(cg->locals, cg->local_count,
        &cg->local_cap, sizeof(*cg->locals), 8);
    cg->locals[cg->local_count].name = name;
    cg->locals[cg->local_count].type = ty;
    cg->local_count++;
}

static void cg_push_params(Codegen *cg, Param *params) {
    for (Param *p = params; p; p = p->next) cg_push_local(cg, p->name, p->type);
}

static ForgeType cg_lookup_local(Codegen *cg, ForgeStr name) {
    for (size_t i = cg->local_count; i > 0; i--) {
        if (forge_str_eq(cg->locals[i - 1].name, name)) return cg->locals[i - 1].type;
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
    "fr_lsp_message", "fr_lsp_read_message",
    "fr_os_argv", "fr_os_executable_path", "fr_os_getenv", "fr_os_temp_file",
    "fr_proc_output", "fr_proc_stderr", "fr_proc_stdout",
    "fr_str_append", "fr_str_append_str", "fr_str_builder_finish", "fr_str_builder_finish_take", "fr_str_concat", "fr_str_from_int",
    "fr_str_sub", "fr_str_trim", "fr_str_view_sub",
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

/* The semantic pass does not cache all inferred expression types. Restrict
 * direct arithmetic lowering to expressions whose integer type is explicit. */
static bool cg_expr_known_int(Codegen *cg, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_INT: case EXPR_BOOL: case EXPR_RECV: case EXPR_INDEX:
        return true;
    case EXPR_IDENT: {
        if (cg_is_local(cg, e->as.ident)) {
            ForgeTypeKind kind = cg_lookup_local(cg, e->as.ident).kind;
            return kind == TY_INT || kind == TY_BOOL;
        }
        ConstDecl *constant = cg_find_const(cg, e->as.ident);
        return constant && constant->value &&
               (constant->value->kind == EXPR_INT || constant->value->kind == EXPR_BOOL);
    }
    case EXPR_BINARY:
        return cg_expr_known_int(cg, e->as.binary.left) &&
               cg_expr_known_int(cg, e->as.binary.right);
    case EXPR_CALL: {
        FnDecl *fn = cg_resolve_fn_ref(cg, e->as.call.name);
        return fn && (fn->ret_type.kind == TY_INT || fn->ret_type.kind == TY_BOOL);
    }
    case EXPR_QUAL_CALL: {
        FnDecl *fn = cg_resolve_qual_fn(cg, e->as.qual_call.module, e->as.qual_call.name);
        return fn && (fn->ret_type.kind == TY_INT || fn->ret_type.kind == TY_BOOL);
    }
    default:
        return e->type.kind == TY_INT || e->type.kind == TY_BOOL;
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
        if (e->as.int_val == INT64_MIN) {
            fputs("(-INT64_C(9223372036854775807) - INT64_C(1))", cg->out);
        } else if (e->as.int_val < 0) {
            fprintf(cg->out, "(-INT64_C(%lld))", (long long)-e->as.int_val);
        } else {
            fprintf(cg->out, "INT64_C(%lld)", (long long)e->as.int_val);
        }
        break;
    case EXPR_FLOAT: {
        char literal[64];
        snprintf(literal, sizeof(literal), "%.17g", e->as.float_val);
        fputs(literal, cg->out);
        if (!strpbrk(literal, ".eE")) fputs(".0", cg->out);
        break;
    }
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
        Expr *mersenne_value;
        int64_t mersenne_multiplier, mersenne_modulus, mersenne_addend;
        unsigned mersenne_shift;
        if (cg_mersenne_modmul(cg, e, &mersenne_value, &mersenne_multiplier,
                               &mersenne_modulus, &mersenne_addend, &mersenne_shift)) {
            fputs(mersenne_addend ? "__forge_mersenne_modmuladd_i64(" :
                                    "__forge_mersenne_modmul_i64(", cg->out);
            emit_expr(cg, mersenne_value);
            if (mersenne_addend) {
                fprintf(cg->out, ", INT64_C(%lld), INT64_C(%lld), INT64_C(%lld), %u)",
                        (long long)mersenne_multiplier, (long long)mersenne_addend,
                        (long long)mersenne_modulus, mersenne_shift);
            } else
                fprintf(cg->out, ", INT64_C(%lld), INT64_C(%lld), %u)",
                        (long long)mersenne_multiplier, (long long)mersenne_modulus,
                        mersenne_shift);
            break;
        }
        if (e->as.binary.op == BIN_MOD &&
            e->as.binary.right->kind == EXPR_IDENT &&
            cg_expr_known_int(cg, e->as.binary.left)) {
            bool emitted = false;
            for (size_t i = 0; i < cg->invariant_mod_count; i++) {
                if (!forge_str_eq(e->as.binary.right->as.ident,
                                  cg->invariant_mods[i].parameter)) continue;
                fputs("__forge_mod_invariant(", cg->out);
                emit_expr(cg, e->as.binary.left);
                fprintf(cg->out, ", %s)", cg->invariant_mods[i].name);
                emitted = true;
                break;
            }
            if (emitted) break;
        }
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
        const char *helper = NULL;
        switch (e->as.binary.op) {
        case BIN_ADD: helper = "FORGE_ADD"; break;
        case BIN_SUB: helper = "FORGE_SUB"; break;
        case BIN_MUL: helper = "FORGE_MUL"; break;
        case BIN_DIV: helper = "FORGE_DIV"; break;
        case BIN_MOD: helper = "FORGE_MOD"; break;
        default: break;
        }
        if (helper) {
            Expr *right = e->as.binary.right;
            bool direct_positive_division =
                (e->as.binary.op == BIN_MOD || e->as.binary.op == BIN_DIV) &&
                cg_expr_known_int(cg, e->as.binary.left) &&
                right->kind == EXPR_INT && right->as.int_val > 0;
            if (direct_positive_division) {
                /* A positive divisor cannot trigger Forge's zero or
                 * INT64_MIN/-1 division traps. Let C lower this constant
                 * operation while keeping every other case checked. */
                fputc('(', cg->out);
                emit_expr(cg, e->as.binary.left);
                fputs(e->as.binary.op == BIN_MOD ? " % " : " / ", cg->out);
                emit_expr(cg, right);
                fputc(')', cg->out);
            } else {
                fprintf(cg->out, "%s(", helper);
                emit_expr(cg, e->as.binary.left);
                fputs(", ", cg->out);
                emit_expr(cg, right);
                fputc(')', cg->out);
            }
        } else {
            fputc('(', cg->out);
            emit_expr(cg, e->as.binary.left);
            fprintf(cg->out, " %s ", op);
            emit_expr(cg, e->as.binary.right);
            fputc(')', cg->out);
        }
        break;
    }
    case EXPR_CALL: {
        ForgeStr name = e->as.call.name;
        if (emit_thread_call(cg, name, e->as.call.args, e->as.call.arg_count)) break;
        const char *mapped_name = forge_std_c_name(name, cg->imports, cg->import_count);
        if (mapped_name && strcmp(mapped_name, "fr_str_view_at") == 0 &&
            cg->safe_view_at_view.len && e->as.call.arg_count == 2 &&
            e->as.call.args[0]->kind == EXPR_IDENT &&
            e->as.call.args[1]->kind == EXPR_IDENT &&
            forge_str_eq(e->as.call.args[0]->as.ident, cg->safe_view_at_view) &&
            forge_str_eq(e->as.call.args[1]->as.ident, cg->safe_view_at_index)) {
            fputs("fr_detail_str_view_at_unchecked(", cg->out);
            emit_expr(cg, e->as.call.args[0]);
            fputc(',', cg->out);
            emit_expr(cg, e->as.call.args[1]);
            fputc(')', cg->out);
            break;
        }
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
    l->items = forge_array_reserve(l->items, l->count, &l->cap, sizeof(*l->items), 8);
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
            cg_line(cg, "    if (fr_coro_inline_yield(__coro, %d)) return FR_CORO_YIELDED;", resume);
            cg_line(cg, "    goto __forge_coro_resume_%d;", resume);
            cg_line(cg, "case %d:;", resume);
            cg_line(cg, "__forge_coro_resume_%d:;", resume);
            s = s->next;
            continue;
        }

        if (s->kind == STMT_AWAIT) {
            int resume = ++(*step);
            cg_line(cg, "    fr_coro_set_step(__coro, %d);", resume);
            cg_line(cg, "case %d:;", resume);
            cg_indent(cg);
            fputs("if (!fr_await_fd(__coro, ", cg->out);
            emit_expr(cg, s->as.await_expr);
            fputs(", FR_EVENT_READ)) return FR_CORO_WAITING_IO;\n", cg->out);
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
        case STMT_BREAK:
            /* The resume switch surrounds the generated loop. C therefore
             * targets the innermost loop, including after a yield resumes. */
            cg_line(cg, "break;");
            break;
        case STMT_CONTINUE:
            cg_line(cg, "continue;");
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
                /* Forge strings can point into a literal or an arena. The
                 * mailbox consumes a malloc-backed payload even on rejection,
                 * so never pass a borrowed string directly to fr_send_move. */
                fputs("__forge_send_owned_string((fr_process_t *)(intptr_t)(", cg->out);
                emit_expr(cg, s->as.send.target);
                fprintf(cg->out, "), %d, &", s->as.send.tag);
                if (!s->as.send.value || s->as.send.value->kind != EXPR_IDENT)
                    forge_die("owned send requires a named string binding");
                if (cg->state_prefix) fputs(cg->state_prefix, cg->out);
                fprintf(cg->out, "%.*s", (int)s->as.send.value->as.ident.len,
                        s->as.send.value->as.ident.data);
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
    cg->indent = 1;
    emit_coro_spawn_inits(cg, coro);
    cg->indent = 0;
    fprintf(cg->out, "    fr_coro_spawn(proc, %.*s_fn, init, sizeof(%.*s_state_t));\n",
            (int)coro->name.len, coro->name.data,
            (int)coro->name.len, coro->name.data);
    fputs("}\n\n", cg->out);
}

/* Local declarations are a stack: exiting a generated C block restores the
 * outer binding and its type rather than overwriting it during shadowing. */
static void emit_block_stmts(Codegen *cg, Stmt *s, const char *proc_var, bool in_coro) {
    size_t saved_locals = cg->local_count;
    emit_stmts(cg, s, proc_var, in_coro);
    cg->local_count = saved_locals;
}

static void emit_if_stmt(Codegen *cg, Stmt *s, const char *proc_var, bool in_coro) {
    cg_enter(cg);
    cg_indent(cg);
    fputs("if (", cg->out);
    emit_expr(cg, s->as.if_stmt.cond);
    fputs(") {\n", cg->out);
    cg->indent++;
    emit_block_stmts(cg, s->as.if_stmt.then_br->first, proc_var, in_coro);
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
            emit_block_stmts(cg, s->as.if_stmt.else_br->first, proc_var, in_coro);
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

typedef struct {
    ForgeStr name;
    size_t *budget;
    bool rejected, name_seen, repeated_mod;
} InvariantModScan;

static bool invariant_mod_visit(InvariantModScan *scan, int depth) {
    if (depth > CG_MAX_DEPTH || !*scan->budget) {
        scan->rejected = true;
        return false;
    }
    --*scan->budget;
    return true;
}

static void invariant_mod_expr(InvariantModScan *scan, Expr *e, bool repeated, int depth) {
    if (!e || scan->rejected || !invariant_mod_visit(scan, depth)) return;
    switch (e->kind) {
    case EXPR_IDENT:
        if (forge_str_eq(e->as.ident, scan->name)) scan->name_seen = true;
        break;
    case EXPR_BINARY:
        if (repeated && e->as.binary.op == BIN_MOD &&
            e->as.binary.right->kind == EXPR_IDENT &&
            forge_str_eq(e->as.binary.right->as.ident, scan->name))
            scan->repeated_mod = true;
        invariant_mod_expr(scan, e->as.binary.left, repeated, depth + 1);
        invariant_mod_expr(scan, e->as.binary.right, repeated, depth + 1);
        break;
    case EXPR_CALL:
    case EXPR_QUAL_CALL: {
        bool qualified = e->kind == EXPR_QUAL_CALL;
        ForgeStr name = qualified ? e->as.qual_call.name : e->as.call.name;
        Expr **args = qualified ? e->as.qual_call.args : e->as.call.args;
        size_t count = qualified ? e->as.qual_call.arg_count : e->as.call.arg_count;
        if (forge_str_eq(name, scan->name)) scan->name_seen = true;
        for (size_t i = 0; i < count && !scan->rejected; i++)
            invariant_mod_expr(scan, args[i], repeated, depth + 1);
        break;
    }
    case EXPR_INDEX:
        invariant_mod_expr(scan, e->as.index.base, repeated, depth + 1);
        invariant_mod_expr(scan, e->as.index.index, repeated, depth + 1);
        break;
    case EXPR_FIELD:
        invariant_mod_expr(scan, e->as.field.base, repeated, depth + 1);
        break;
    case EXPR_MOVE:
    case EXPR_RECV:
        /* Moves can mutate named storage; coroutine/receive paths have a
         * separate lifetime model. Fail closed for this function. */
        scan->rejected = true;
        break;
    default: break;
    }
}

static void invariant_mod_stmts(InvariantModScan *scan, Stmt *s, bool repeated, int depth) {
    for (; s && !scan->rejected; s = s->next) {
        if (!invariant_mod_visit(scan, depth)) return;
        switch (s->kind) {
        case STMT_LET:
            if (forge_str_eq(s->as.let.name, scan->name)) scan->rejected = true;
            invariant_mod_expr(scan, s->as.let.init, repeated, depth + 1);
            break;
        case STMT_ASSIGN:
            if (forge_str_eq(s->as.assign.name, scan->name)) scan->rejected = true;
            invariant_mod_expr(scan, s->as.assign.value, repeated, depth + 1);
            break;
        case STMT_EXPR: invariant_mod_expr(scan, s->as.expr, repeated, depth + 1); break;
        case STMT_RETURN: invariant_mod_expr(scan, s->as.ret, repeated, depth + 1); break;
        case STMT_IF:
            invariant_mod_expr(scan, s->as.if_stmt.cond, repeated, depth + 1);
            invariant_mod_stmts(scan, s->as.if_stmt.then_br->first, repeated, depth + 1);
            if (s->as.if_stmt.else_br)
                invariant_mod_stmts(scan, s->as.if_stmt.else_br->first, repeated, depth + 1);
            break;
        case STMT_WHILE:
            invariant_mod_expr(scan, s->as.while_stmt.cond, true, depth + 1);
            invariant_mod_stmts(scan, s->as.while_stmt.body->first, true, depth + 1);
            break;
        case STMT_FOR:
            invariant_mod_stmts(scan, s->as.for_stmt.init, repeated, depth + 1);
            invariant_mod_expr(scan, s->as.for_stmt.cond, true, depth + 1);
            invariant_mod_stmts(scan, s->as.for_stmt.body->first, true, depth + 1);
            invariant_mod_stmts(scan, s->as.for_stmt.step, true, depth + 1);
            break;
        case STMT_BLOCK:
            invariant_mod_stmts(scan, s->as.block->first, repeated, depth + 1);
            break;
        case STMT_MATCH:
            invariant_mod_expr(scan, s->as.match_stmt.scrutinee, repeated, depth + 1);
            for (MatchArm *arm = s->as.match_stmt.arms; arm && !scan->rejected; arm = arm->next) {
                if (!invariant_mod_visit(scan, depth)) return;
                invariant_mod_stmts(scan, arm->body->first, repeated, depth + 1);
            }
            break;
        case STMT_SPAWN:
            for (size_t i = 0; i < s->as.spawn.arg_count && !scan->rejected; i++)
                invariant_mod_expr(scan, s->as.spawn.args[i], repeated, depth + 1);
            break;
        case STMT_SEND:
            if (s->as.send.move_) scan->rejected = true;
            invariant_mod_expr(scan, s->as.send.target, repeated, depth + 1);
            invariant_mod_expr(scan, s->as.send.value, repeated, depth + 1);
            break;
        case STMT_YIELD:
        case STMT_AWAIT: scan->rejected = true; break;
        case STMT_BREAK:
        case STMT_CONTINUE: break;
        default: scan->rejected = true; break;
        }
    }
}

/* Only immutable integer parameters with loop-repeated remainder qualify.
 * Checking the whole function rejects writes before/after the loop and any
 * shadow binding. Primitive native parameters are otherwise passed by value.
 * This budget is independent of the existing loop-bound proof budget. */
static void emit_invariant_mod_descriptors(Codegen *cg, FnDecl *fn) {
    size_t budget = CG_LOOP_SCAN_BUDGET;
    unsigned name_id = 0;
    for (Param *p = fn->params; p && budget; p = p->next) {
        if (p->type.kind != TY_INT || cg->invariant_mod_count == 8) continue;
        InvariantModScan scan = { .name = p->name, .budget = &budget };
        invariant_mod_stmts(&scan, fn->body.first, false, 0);
        if (scan.rejected || !scan.repeated_mod) continue;
        char name[64];
        bool available = false;
        while (budget && !available) {
            snprintf(name, sizeof(name), "__forge_invariant_mod_%u", name_id++);
            ForgeStr candidate = forge_str(name);
            if (cg_is_local(cg, candidate) || cg_lookup_fn(cg, candidate)) continue;
            InvariantModScan collision = { .name = candidate, .budget = &budget };
            invariant_mod_stmts(&collision, fn->body.first, false, 0);
            available = !collision.rejected && !collision.name_seen;
        }
        if (!available) continue;
        size_t index = cg->invariant_mod_count++;
        cg->invariant_mods[index].parameter = p->name;
        memcpy(cg->invariant_mods[index].name, name, strlen(name) + 1);
        cg_line(cg, "const __forge_mod_descriptor %s = __forge_mod_prepare(%.*s);",
                name, (int)p->name.len, p->name.data);
    }
}

/* Expressions cannot assign primitive locals, and native scalar arguments are
 * passed by value. Scan every statement path for writes and shadow bindings.
 * Reject shadowing conservatively: Expr/Stmt store names, not binding IDs, and
 * coroutine locals have different storage/resume rules from lexical C locals.
 * A per-function visit budget bounds repeated scans of nested loop bodies. */
static bool loop_body_preserves(Codegen *cg, Stmt *s, Stmt *increment,
                                ForgeStr induction, ForgeStr bound, int depth) {
    if (depth > CG_MAX_DEPTH) return false;
    for (; s; s = s->next) {
        if (!cg->loop_scan_left) return false;
        cg->loop_scan_left--;
        switch (s->kind) {
        case STMT_LET:
            if (forge_str_eq(s->as.let.name, induction) ||
                (bound.len && forge_str_eq(s->as.let.name, bound))) return false;
            break;
        case STMT_ASSIGN:
            if (s != increment &&
                (forge_str_eq(s->as.assign.name, induction) ||
                 (bound.len && forge_str_eq(s->as.assign.name, bound)))) return false;
            break;
        case STMT_IF:
            if (!loop_body_preserves(cg, s->as.if_stmt.then_br->first, increment, induction, bound, depth + 1) ||
                (s->as.if_stmt.else_br && !loop_body_preserves(cg, s->as.if_stmt.else_br->first, increment, induction, bound, depth + 1))) return false;
            break;
        case STMT_WHILE:
            if (!loop_body_preserves(cg, s->as.while_stmt.body->first, increment, induction, bound, depth + 1)) return false;
            break;
        case STMT_FOR:
            if (!loop_body_preserves(cg, s->as.for_stmt.init, increment, induction, bound, depth + 1) ||
                !loop_body_preserves(cg, s->as.for_stmt.body->first, increment, induction, bound, depth + 1) ||
                !loop_body_preserves(cg, s->as.for_stmt.step, increment, induction, bound, depth + 1)) return false;
            break;
        case STMT_BLOCK:
            if (!loop_body_preserves(cg, s->as.block->first, increment, induction, bound, depth + 1)) return false;
            break;
        case STMT_MATCH:
            for (MatchArm *arm = s->as.match_stmt.arms; arm; arm = arm->next) {
                if (!cg->loop_scan_left) return false;
                cg->loop_scan_left--;
                if (!loop_body_preserves(cg, arm->body->first, increment, induction, bound, depth + 1)) return false;
            }
            break;
        case STMT_YIELD:
        case STMT_AWAIT:
            return false;
        default:
            break;
        }
    }
    return true;
}

static Stmt *prove_loop_increment(Codegen *cg, Expr *condition, Block *body,
                                  Stmt *increment) {
    if (!cg->prove_loop_increments || cg->state_prefix || !cg->loop_scan_left ||
        !condition || condition->kind != EXPR_BINARY || condition->as.binary.op != BIN_LT ||
        !increment || increment->kind != STMT_ASSIGN || increment->next) return NULL;
    Expr *left = condition->as.binary.left, *right = condition->as.binary.right;
    if (left->kind != EXPR_IDENT || !cg_is_local(cg, left->as.ident) ||
        cg_lookup_local(cg, left->as.ident).kind != TY_INT ||
        !forge_str_eq(increment->as.assign.name, left->as.ident)) return NULL;
    ForgeStr bound = {0};
    if (right->kind == EXPR_IDENT) {
        if (!cg_is_local(cg, right->as.ident) || cg_lookup_local(cg, right->as.ident).kind != TY_INT) return NULL;
        bound = right->as.ident;
    } else if (right->kind != EXPR_INT) return NULL;
    Expr *value = increment->as.assign.value;
    if (!value || value->kind != EXPR_BINARY || value->as.binary.op != BIN_ADD) return NULL;
    Expr *variable = value->as.binary.left, *one = value->as.binary.right;
    if (variable->kind == EXPR_INT) { Expr *swap = variable; variable = one; one = swap; }
    if (variable->kind != EXPR_IDENT || !forge_str_eq(variable->as.ident, left->as.ident) ||
        one->kind != EXPR_INT || one->as.int_val != 1) return NULL;
    if (!loop_body_preserves(cg, body->first, increment, left->as.ident, bound, 0)) return NULL;
    /* On every path reaching this sole update, the integer guard established
     * i < bound <= INT64_MAX, hence i <= INT64_MAX-1. Adding one cannot
     * overflow or underflow, even when i initially is negative. Native while
     * continue skips a tail update; native for continue executes its step. */
    return increment;
}

static bool cg_view_accessor_call(Codegen *cg, Expr *e, const char *c_name,
                                  size_t arg_count) {
    if (!e || e->kind != EXPR_CALL || e->as.call.arg_count != arg_count) return false;
    const char *mapped = forge_std_c_name(e->as.call.name, cg->imports, cg->import_count);
    return mapped && strcmp(mapped, c_name) == 0;
}

static bool cg_view_pure_expr(Codegen *cg, Expr *e, ForgeStr view, ForgeStr index,
                              bool allow_at, int depth) {
    if (!e || depth > CG_MAX_DEPTH) return false;
    switch (e->kind) {
    case EXPR_INT: case EXPR_FLOAT: case EXPR_BOOL: case EXPR_IDENT:
        return true;
    case EXPR_BINARY:
        return cg_view_pure_expr(cg, e->as.binary.left, view, index, allow_at, depth + 1) &&
               cg_view_pure_expr(cg, e->as.binary.right, view, index, allow_at, depth + 1);
    case EXPR_CALL:
        return allow_at && cg_view_accessor_call(cg, e, "fr_str_view_at", 2) &&
               e->as.call.args[0]->kind == EXPR_IDENT &&
               e->as.call.args[1]->kind == EXPR_IDENT &&
               forge_str_eq(e->as.call.args[0]->as.ident, view) &&
               forge_str_eq(e->as.call.args[1]->as.ident, index);
    default:
        return false;
    }
}

static bool cg_view_loop_body_safe(Codegen *cg, Stmt *s, Stmt *increment,
                                   ForgeStr view, ForgeStr index, ForgeStr bound,
                                   int depth, bool top_level, bool allow_at) {
    if (depth > CG_MAX_DEPTH) return false;
    bool increment_seen = false;
    for (; s; s = s->next) {
        if (!cg->loop_scan_left) return false;
        cg->loop_scan_left--;
        if (s == increment) {
            if (!top_level || increment_seen) return false;
            increment_seen = true;
            continue;
        }
        bool expression_allows_at = allow_at && !increment_seen;
        switch (s->kind) {
        case STMT_LET:
            if (forge_str_eq(s->as.let.name, view) || forge_str_eq(s->as.let.name, index) ||
                forge_str_eq(s->as.let.name, bound)) return false;
            if (s->as.let.init && !cg_view_pure_expr(cg, s->as.let.init,
                    view, index, expression_allows_at, depth + 1)) return false;
            break;
        case STMT_ASSIGN:
            if (forge_str_eq(s->as.assign.name, view) || forge_str_eq(s->as.assign.name, bound) ||
                (forge_str_eq(s->as.assign.name, index) && s != increment) ||
                !cg_view_pure_expr(cg, s->as.assign.value, view, index,
                    expression_allows_at, depth + 1)) return false;
            break;
        case STMT_EXPR:
            if (!cg_view_pure_expr(cg, s->as.expr, view, index,
                    expression_allows_at, depth + 1)) return false;
            break;
        case STMT_IF:
            if (!cg_view_pure_expr(cg, s->as.if_stmt.cond, view, index,
                    expression_allows_at, depth + 1) ||
                !cg_view_loop_body_safe(cg, s->as.if_stmt.then_br->first, increment,
                    view, index, bound, depth + 1, false, expression_allows_at) ||
                (s->as.if_stmt.else_br && !cg_view_loop_body_safe(cg,
                    s->as.if_stmt.else_br->first, increment, view, index, bound,
                    depth + 1, false, expression_allows_at)))
                return false;
            break;
        case STMT_BLOCK:
            if (!cg_view_loop_body_safe(cg, s->as.block->first, increment,
                    view, index, bound, depth + 1, false, expression_allows_at)) return false;
            break;
        case STMT_RETURN:
            if (s->as.ret && !cg_view_pure_expr(cg, s->as.ret, view, index,
                    expression_allows_at, depth + 1))
                return false;
            break;
        case STMT_BREAK: case STMT_CONTINUE:
            break;
        default:
            /* Calls other than the proven accessor, suspension, nested loops,
             * and sends can invalidate the borrowed view or change its arena. */
            return false;
        }
    }
    return true;
}

static bool cg_view_prefix_stmt_safe(Codegen *cg, Stmt *s, ForgeStr view,
                                     ForgeStr index, ForgeStr bound) {
    switch (s->kind) {
    case STMT_LET:
        return !forge_str_eq(s->as.let.name, view) &&
               !forge_str_eq(s->as.let.name, index) &&
               !forge_str_eq(s->as.let.name, bound) &&
               (!s->as.let.init || cg_view_pure_expr(cg, s->as.let.init,
                    view, index, false, 0));
    case STMT_ASSIGN:
        return !forge_str_eq(s->as.assign.name, view) &&
               !forge_str_eq(s->as.assign.name, index) &&
               !forge_str_eq(s->as.assign.name, bound) &&
               cg_view_pure_expr(cg, s->as.assign.value, view, index, false, 0);
    case STMT_EXPR:
        return cg_view_pure_expr(cg, s->as.expr, view, index, false, 0);
    default:
        /* Keep the pre-loop region straight-line; conditional or nested
         * control flow would need binding-aware alias analysis. */
        return false;
    }
}

static bool prove_view_at_loop(Codegen *cg, Stmt *block_first, Stmt *loop,
                               Stmt *increment) {
    if (!cg->prove_loop_increments || cg->state_prefix ||
        !loop || loop->kind != STMT_WHILE) return false;
    Expr *condition = loop->as.while_stmt.cond;
    if (!condition || condition->kind != EXPR_BINARY || condition->as.binary.op != BIN_LT)
        return false;
    Expr *left = condition->as.binary.left, *right = condition->as.binary.right;
    if (left->kind != EXPR_IDENT || right->kind != EXPR_IDENT) return false;
    ForgeStr index = left->as.ident, bound = right->as.ident, view = {0};
    if (!cg_is_local(cg, index) || !cg_is_local(cg, bound) ||
        cg_lookup_local(cg, index).kind != TY_INT || cg_lookup_local(cg, bound).kind != TY_INT)
        return false;
    Stmt *index_init = NULL, *bound_init = NULL;
    for (Stmt *s = block_first; s && s != loop; s = s->next) {
        if (s->kind != STMT_LET) continue;
        if (forge_str_eq(s->as.let.name, index)) index_init = s;
        if (forge_str_eq(s->as.let.name, bound)) bound_init = s;
    }
    if (!index_init || !bound_init || index_init == bound_init ||
        !index_init->as.let.init || index_init->as.let.init->kind != EXPR_INT ||
        index_init->as.let.init->as.int_val != 0 ||
        !cg_view_accessor_call(cg, bound_init->as.let.init, "fr_str_view_len", 1) ||
        bound_init->as.let.init->as.call.args[0]->kind != EXPR_IDENT) return false;
    view = bound_init->as.let.init->as.call.args[0]->as.ident;
    if (!cg_is_local(cg, view) || cg_lookup_local(cg, view).kind != TY_INT) return false;

    bool after_bound = false, after_index_init = false;
    for (Stmt *s = block_first; s && s != loop; s = s->next) {
        if (s == bound_init) {
            after_bound = true;
            continue;
        }
        if (s == index_init) {
            after_index_init = true;
            continue;
        }
        /* The zero initializer must remain the value of index through the
         * loop. Scan from it even when it precedes the captured length; the
         * bound initializer itself is the only statement skipped. Also scan
         * everything after the bound capture, including statements before
         * index_init, so view/bound invalidation cannot slip through. */
        if ((after_index_init || after_bound) &&
            !cg_view_prefix_stmt_safe(cg, s, view, index, bound)) return false;
    }
    if (!increment) {
        for (Stmt *s = loop->as.while_stmt.body->first; s; s = s->next) {
            if (s->kind == STMT_ASSIGN && forge_str_eq(s->as.assign.name, index)) {
                if (increment) return false;
                increment = s;
            }
        }
    }
    if (!increment || increment->kind != STMT_ASSIGN ||
        !forge_str_eq(increment->as.assign.name, index)) return false;
    Expr *step = increment->as.assign.value;
    if (!step || step->kind != EXPR_BINARY || step->as.binary.op != BIN_ADD) return false;
    Expr *variable = step->as.binary.left, *one = step->as.binary.right;
    if (variable->kind == EXPR_INT) { Expr *swap = variable; variable = one; one = swap; }
    if (variable->kind != EXPR_IDENT || !forge_str_eq(variable->as.ident, index) ||
        one->kind != EXPR_INT || one->as.int_val != 1 ||
        !loop_body_preserves(cg, loop->as.while_stmt.body->first, increment,
            index, bound, 0)) return false;
    if (!cg_view_loop_body_safe(cg, loop->as.while_stmt.body->first, increment,
            view, index, bound, 0, true, true)) return false;
    cg->safe_increment = increment;
    cg->safe_view_at_view = view;
    cg->safe_view_at_index = index;
    return true;
}

static void emit_assignment_rhs(Codegen *cg, Stmt *s) {
    if (s == cg->safe_increment) {
        Expr *value = s->as.assign.value;
        fputc('(', cg->out);
        emit_expr(cg, value->as.binary.left);
        fputs(" + ", cg->out);
        emit_expr(cg, value->as.binary.right);
        fputc(')', cg->out);
    } else {
        emit_expr(cg, s->as.assign.value);
    }
}

static void emit_for_step_inline(Codegen *cg, Stmt *step) {
    for (Stmt *s = step; s; s = s->next) {
        if (s != step) fputs(", ", cg->out);
        if (s->kind == STMT_ASSIGN) {
            if (cg->state_prefix) fputs(cg->state_prefix, cg->out);
            fprintf(cg->out, "%.*s = ", (int)s->as.assign.name.len, s->as.assign.name.data);
            emit_assignment_rhs(cg, s);
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

typedef struct { int64_t low, high; } CgIntRange;

static bool cg_range_add(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    *out = a + b;
    return true;
}

static bool cg_range_sub(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return false;
    *out = a - b;
    return true;
}

/* Only accept pure integer syntax. Besides preserving single evaluation, this
 * excludes calls that could alias the string or reset its arena. */
static bool cg_pure_int_expr(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_INT: case EXPR_IDENT: return true;
    case EXPR_BINARY:
        if (e->as.binary.op != BIN_ADD && e->as.binary.op != BIN_SUB &&
            e->as.binary.op != BIN_MUL && e->as.binary.op != BIN_DIV &&
            e->as.binary.op != BIN_MOD) return false;
        return cg_pure_int_expr(e->as.binary.left) && cg_pure_int_expr(e->as.binary.right);
    default: return false;
    }
}

static bool cg_expr_mentions(const Expr *e, ForgeStr name) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_IDENT: return forge_str_eq(e->as.ident, name);
    case EXPR_BINARY:
        return cg_expr_mentions(e->as.binary.left, name) ||
               cg_expr_mentions(e->as.binary.right, name);
    default: return false;
    }
}

static bool cg_int_range(const Expr *e, CgIntRange *range) {
    if (!e) return false;
    if (e->kind == EXPR_INT) {
        range->low = range->high = e->as.int_val;
        return true;
    }
    if (e->kind != EXPR_BINARY) return false;
    Expr *left = e->as.binary.left, *right = e->as.binary.right;
    if (e->as.binary.op == BIN_MOD && right->kind == EXPR_INT && right->as.int_val != 0) {
        int64_t divisor = right->as.int_val;
        uint64_t magnitude = divisor < 0 ? (uint64_t)(-(divisor + 1)) + 1 : (uint64_t)divisor;
        uint64_t bound = magnitude - 1;
        if (bound > INT64_MAX) bound = INT64_MAX;
        range->low = -(int64_t)bound;
        range->high = (int64_t)bound;
        return true;
    }
    CgIntRange a, b;
    if (!cg_int_range(left, &a) || !cg_int_range(right, &b)) return false;
    if (e->as.binary.op == BIN_ADD)
        return cg_range_add(a.low, b.low, &range->low) &&
               cg_range_add(a.high, b.high, &range->high);
    if (e->as.binary.op == BIN_SUB)
        return cg_range_sub(a.low, b.high, &range->low) &&
               cg_range_sub(a.high, b.low, &range->high);
    return false;
}

static bool cg_is_empty_string_let(const Stmt *s) {
    return s && s->kind == STMT_LET && s->as.let.type.kind == TY_STRING &&
           s->as.let.init && s->as.let.init->kind == EXPR_STRING &&
           s->as.let.init->as.string_val.len == 0;
}

typedef struct {
    Stmt *guard, *index, *loop;
    Expr *byte;
} CgReservedBuilderLoop;

/* Fresh, unescaped builders only. Resolve every scalar binding and bound the
 * proof walk; calls, dynamic division and references to the builder fail closed. */
static bool cg_reserved_scalar(Codegen *cg, Expr *e, ForgeStr builder,
                               ForgeStr index, unsigned depth) {
    if (!e || depth > 64 || !cg->loop_scan_left) return false;
    cg->loop_scan_left--;
    if (e->kind == EXPR_INT) return true;
    if (e->kind == EXPR_IDENT)
        return !forge_str_eq(e->as.ident, builder) &&
               (forge_str_eq(e->as.ident, index) ||
                (cg_is_local(cg, e->as.ident) && cg_lookup_local(cg, e->as.ident).kind == TY_INT));
    if (e->kind != EXPR_BINARY || e->as.binary.op > BIN_MOD) return false;
    if ((e->as.binary.op == BIN_DIV || e->as.binary.op == BIN_MOD) &&
        (e->as.binary.right->kind != EXPR_INT || e->as.binary.right->as.int_val <= 0)) return false;
    return cg_reserved_scalar(cg, e->as.binary.left, builder, index, depth + 1) &&
           cg_reserved_scalar(cg, e->as.binary.right, builder, index, depth + 1);
}

static bool cg_zero_test(Expr *e, ForgeStr name) {
    if (!e || e->kind != EXPR_BINARY || e->as.binary.op != BIN_EQ) return false;
    Expr *a = e->as.binary.left, *b = e->as.binary.right;
    if (a->kind == EXPR_INT) { Expr *swap = a; a = b; b = swap; }
    return a->kind == EXPR_IDENT && forge_str_eq(a->as.ident, name) &&
           b->kind == EXPR_INT && b->as.int_val == 0;
}

static bool cg_failure_return(Stmt *s) {
    Stmt *ret = s && s->kind == STMT_IF && s->as.if_stmt.then_br ?
                s->as.if_stmt.then_br->first : NULL;
    return ret && ret->kind == STMT_RETURN && !ret->next && !s->as.if_stmt.else_br &&
           ret->as.ret && ret->as.ret->kind == EXPR_STRING;
}

static CgReservedBuilderLoop cg_reserved_builder_loop(Codegen *cg, Stmt *binding) {
    CgReservedBuilderLoop none = {0}, plan = {0};
    ForgeStr builder = binding->as.let.name;
    Expr *ctor = binding->as.let.init;
    if (!cg->prove_loop_increments || cg->state_prefix || cg_is_local(cg, builder) ||
        binding->as.let.type.kind != TY_INT ||
        !cg_view_accessor_call(cg, ctor, "fr_str_builder_with_capacity", 1) ||
        cg_lookup_fn(cg, ctor->as.call.name)) return none;
    Expr *capacity = ctor->as.call.args[0];
    if (capacity->kind != EXPR_IDENT || forge_str_eq(capacity->as.ident, builder) ||
        !cg_is_local(cg, capacity->as.ident) || cg_lookup_local(cg, capacity->as.ident).kind != TY_INT)
        return none;
    Stmt *s = binding->next;
    if (s && s->kind == STMT_IF) {
        if (!cg_zero_test(s->as.if_stmt.cond, builder) || !cg_failure_return(s)) return none;
        plan.guard = s;
        s = s->next;
    }
    if (!s || s->kind != STMT_LET || s->as.let.type.kind != TY_INT ||
        !s->as.let.init || s->as.let.init->kind != EXPR_INT || s->as.let.init->as.int_val != 0 ||
        cg_is_local(cg, s->as.let.name) || forge_str_eq(s->as.let.name, builder) ||
        forge_str_eq(s->as.let.name, capacity->as.ident)) return none;
    plan.index = s;
    ForgeStr index = s->as.let.name;
    Stmt *loop = s->next;
    if (!loop || loop->kind != STMT_WHILE || !loop->as.while_stmt.body) return none;
    Expr *cond = loop->as.while_stmt.cond;
    if (!cond || cond->kind != EXPR_BINARY || cond->as.binary.op != BIN_LT ||
        cond->as.binary.left->kind != EXPR_IDENT || cond->as.binary.right->kind != EXPR_IDENT ||
        !forge_str_eq(cond->as.binary.left->as.ident, index) ||
        !forge_str_eq(cond->as.binary.right->as.ident, capacity->as.ident)) return none;
    Stmt *append = loop->as.while_stmt.body->first, *step = append ? append->next : NULL;
    if (!cg_failure_return(append) || !step || step->kind != STMT_ASSIGN || step->next ||
        !forge_str_eq(step->as.assign.name, index)) return none;
    Expr *test = append->as.if_stmt.cond;
    if (!test || test->kind != EXPR_BINARY || test->as.binary.op != BIN_EQ) return none;
    Expr *call = test->as.binary.left, *zero = test->as.binary.right;
    if (call->kind == EXPR_INT) { Expr *swap = call; call = zero; zero = swap; }
    if (zero->kind != EXPR_INT || zero->as.int_val != 0 ||
        !cg_view_accessor_call(cg, call, "fr_str_builder_char", 2) ||
        cg_lookup_fn(cg, call->as.call.name) || call->as.call.args[0]->kind != EXPR_IDENT ||
        !forge_str_eq(call->as.call.args[0]->as.ident, builder)) return none;
    Expr *increment = step->as.assign.value;
    if (!increment || increment->kind != EXPR_BINARY || increment->as.binary.op != BIN_ADD) return none;
    Expr *a = increment->as.binary.left, *b = increment->as.binary.right;
    if (a->kind == EXPR_INT) { Expr *swap = a; a = b; b = swap; }
    if (a->kind != EXPR_IDENT || !forge_str_eq(a->as.ident, index) ||
        b->kind != EXPR_INT || b->as.int_val != 1) return none;
    CgIntRange range;
    plan.byte = call->as.call.args[1];
    if (!cg_reserved_scalar(cg, plan.byte, builder, index, 0) ||
        !cg_int_range(plan.byte, &range) || range.low < 1 || range.high > 127) return none;
    plan.loop = loop;
    return plan;
}
static bool cg_append_loop_matches(Codegen *cg, ForgeStr text, Stmt *loop,
                                   const char *append_name) {
    if (!loop || loop->kind != STMT_WHILE || !loop->as.while_stmt.body ||
        !loop->as.while_stmt.cond || loop->as.while_stmt.cond->kind != EXPR_BINARY ||
        loop->as.while_stmt.cond->as.binary.op != BIN_LT) return false;
    Expr *cond = loop->as.while_stmt.cond;
    Expr *index = cond->as.binary.left;
    if (index->kind != EXPR_IDENT ||
        forge_str_eq(index->as.ident, text) || !cg_pure_int_expr(cond->as.binary.right)) return false;

    Stmt *append = loop->as.while_stmt.body->first;
    Stmt *step = append ? append->next : NULL;
    if (!append || append->kind != STMT_ASSIGN || !step || step->kind != STMT_ASSIGN || step->next ||
        !forge_str_eq(append->as.assign.name, text) ||
        !forge_str_eq(step->as.assign.name, index->as.ident)) return false;
    Expr *call = append->as.assign.value;
    if (!call || call->kind != EXPR_CALL || call->as.call.arg_count != 2 ||
        !forge_str_eq(call->as.call.args[0]->kind == EXPR_IDENT ?
                      call->as.call.args[0]->as.ident : (ForgeStr){0}, text)) return false;
    const char *mapped = forge_std_c_name(call->as.call.name, cg->imports, cg->import_count);
    if (!mapped || strcmp(mapped, append_name) != 0) return false;
    Expr *byte = call->as.call.args[1];
    CgIntRange byte_range;
    if (!cg_pure_int_expr(byte) || cg_expr_mentions(byte, text) ||
        !cg_int_range(byte, &byte_range) || byte_range.low < 1 || byte_range.high > 127) return false;

    Expr *increment = step->as.assign.value;
    if (!increment || increment->kind != EXPR_BINARY || increment->as.binary.op != BIN_ADD)
        return false;
    Expr *a = increment->as.binary.left, *b = increment->as.binary.right;
    bool forward = a->kind == EXPR_IDENT && forge_str_eq(a->as.ident, index->as.ident) &&
                   b->kind == EXPR_INT && b->as.int_val == 1;
    bool reverse = b->kind == EXPR_IDENT && forge_str_eq(b->as.ident, index->as.ident) &&
                   a->kind == EXPR_INT && a->as.int_val == 1;
    return (forward || reverse) && cg_pure_int_expr(increment);
}

static Stmt *cg_find_append_loop(Codegen *cg, Stmt *after_let, ForgeStr text) {
    Stmt *s = after_let;
    for (size_t count = 0; s && count < 8; count++) {
        if (s->kind == STMT_WHILE)
            return cg_append_loop_matches(cg, text, s, "fr_str_append") ? s : NULL;
        if (s->kind != STMT_LET || s->as.let.type.kind != TY_INT ||
            !cg_pure_int_expr(s->as.let.init) || cg_expr_mentions(s->as.let.init, text) ||
            forge_str_eq(s->as.let.name, text)) return NULL;
        s = s->next;
    }
    return NULL;
}

/* The view handle remains the original empty view until materialization. It
 * must never be read during construction, including in the guard or prelude.
 * Only explicit integer locals and literals participate in this proof. Prelude
 * lets have not yet been emitted, so resolve their names separately. Reject
 * shadowing rather than guessing binding identity from name-only AST nodes. */
static bool cg_view_append_int_expr(Codegen *cg, Expr *e, Stmt *first,
                                    Stmt *end, ForgeStr view, unsigned depth) {
    if (!e || depth > 64 || !cg->loop_scan_left) return false;
    cg->loop_scan_left--;
    if (e->kind == EXPR_INT) return true;
    if (e->kind == EXPR_IDENT) {
        if (forge_str_eq(e->as.ident, view)) return false;
        for (Stmt *s = first; s != end; s = s->next)
            if (forge_str_eq(s->as.let.name, e->as.ident)) return true;
        return cg_is_local(cg, e->as.ident) &&
               cg_lookup_local(cg, e->as.ident).kind == TY_INT;
    }
    if (e->kind != EXPR_BINARY || e->as.binary.op > BIN_MOD) return false;
    return cg_view_append_int_expr(cg, e->as.binary.left, first, end, view, depth + 1) &&
           cg_view_append_int_expr(cg, e->as.binary.right, first, end, view, depth + 1);
}

static Stmt *cg_find_view_append_loop(Codegen *cg, Stmt *binding) {
    Expr *init = binding->as.let.init;
    ForgeStr view = binding->as.let.name;
    if (!cg->prove_loop_increments || cg->state_prefix ||
        binding->as.let.type.kind != TY_INT || cg_is_local(cg, view) ||
        !cg_view_accessor_call(cg, init, "fr_str_view", 1) ||
        init->as.call.args[0]->kind != EXPR_STRING ||
        init->as.call.args[0]->as.string_val.len != 0) return NULL;

    Stmt *first = binding->next, *loop = first;
    for (unsigned count = 0; loop && count < 8; count++, loop = loop->next) {
        if (loop->kind == STMT_WHILE) break;
        if (loop->kind != STMT_LET || loop->as.let.type.kind != TY_INT ||
            forge_str_eq(loop->as.let.name, view) || cg_is_local(cg, loop->as.let.name) ||
            !cg_view_append_int_expr(cg, loop->as.let.init, first, loop, view, 0) ||
            cg_expr_mentions(loop->as.let.init, loop->as.let.name)) return NULL;
        for (Stmt *pre = first; pre != loop; pre = pre->next)
            if (forge_str_eq(pre->as.let.name, loop->as.let.name)) return NULL;
    }
    if (!loop || loop->kind != STMT_WHILE) return NULL;
    Expr *cond = loop->as.while_stmt.cond;
    if (!cond || cond->kind != EXPR_BINARY || cond->as.binary.op != BIN_LT ||
        cond->as.binary.left->kind != EXPR_IDENT ||
        (cond->as.binary.right->kind != EXPR_IDENT &&
         cond->as.binary.right->kind != EXPR_INT) ||
        !cg_view_append_int_expr(cg, cond->as.binary.right, first, loop, view, 0) ||
        forge_str_eq(cond->as.binary.left->as.ident,
                     cond->as.binary.right->kind == EXPR_IDENT ?
                     cond->as.binary.right->as.ident : (ForgeStr){0})) return NULL;
    ForgeStr index = cond->as.binary.left->as.ident;
    bool zero_based = false;
    for (Stmt *pre = first; pre != loop; pre = pre->next)
        if (forge_str_eq(pre->as.let.name, index))
            zero_based = pre->as.let.init->kind == EXPR_INT &&
                         pre->as.let.init->as.int_val == 0;
    Stmt *append = loop->as.while_stmt.body ? loop->as.while_stmt.body->first : NULL;
    if (!zero_based || !append || append->kind != STMT_ASSIGN) return NULL;
    Expr *call = append->as.assign.value;
    if (!cg_view_accessor_call(cg, call, "fr_str_view_append_byte", 2) ||
        !cg_view_append_int_expr(cg, call->as.call.args[1], first, loop, view, 0)) return NULL;
    /* Bound depth before the shared syntax/range matcher recurses. */
    return cg_append_loop_matches(cg, view, loop, "fr_str_view_append_byte") ? loop : NULL;
}

static void emit_local_let(Codegen *cg, Stmt *s) {
    cg_push_local(cg, s->as.let.name, s->as.let.type);
    cg_indent(cg);
    c_type_name(cg->out, s->as.let.type);
    fprintf(cg->out, " %.*s", (int)s->as.let.name.len, s->as.let.name.data);
    if (s->as.let.init) {
        fputs(" = ", cg->out);
        emit_expr(cg, s->as.let.init);
    }
    fputs(";\n", cg->out);
}

static void emit_append_loop_legacy(Codegen *cg, Stmt *loop,
                                    const char *proc_var, bool in_coro) {
    cg_line(cg, "while (");
    emit_expr(cg, loop->as.while_stmt.cond);
    fputs(") {\n", cg->out);
    cg->indent++;
    emit_block_stmts(cg, loop->as.while_stmt.body->first, proc_var, in_coro);
    cg->indent--;
    cg_line(cg, "}");
}

static void emit_append_loop_builder(Codegen *cg, Stmt *loop, ForgeStr text,
                                     const char *proc_var, bool in_coro) {
    Stmt *append = loop->as.while_stmt.body->first;
    Stmt *step = append->next;
    int id = ++cg->label_seq;
    char builder[64], byte[64], has_work[64], did_append[64];
    do {
        snprintf(builder, sizeof(builder), "__forge_append_builder_%d", id++);
        snprintf(byte, sizeof(byte), "__forge_append_byte_%d", id);
        snprintf(has_work, sizeof(has_work), "__forge_append_work_%d", id);
        snprintf(did_append, sizeof(did_append), "__forge_append_did_%d", id);
    } while (cg_is_local(cg, forge_str(builder)) || cg_is_local(cg, forge_str(byte)) ||
             cg_is_local(cg, forge_str(has_work)) || cg_is_local(cg, forge_str(did_append)));

    Stmt *saved_increment = cg->safe_increment;
    cg->safe_increment = prove_loop_increment(cg, loop->as.while_stmt.cond,
                                               loop->as.while_stmt.body, step);
    cg_line(cg, "{");
    cg->indent++;
    cg_line(cg, "int %s = (", has_work);
    emit_expr(cg, loop->as.while_stmt.cond);
    fputs(") != 0;\n", cg->out);
    cg_line(cg, "int64_t %s = %s ? fr_str_builder() : 0;", builder, has_work);
    cg_line(cg, "if (%s) {", builder);
    cg->indent++;
    cg_line(cg, "int %s = 0;", did_append);
    cg_line(cg, "while (");
    emit_expr(cg, loop->as.while_stmt.cond);
    fputs(") {\n", cg->out);
    cg->indent++;
    cg_line(cg, "int64_t %s = ", byte);
    emit_expr(cg, append->as.assign.value->as.call.args[1]);
    fputs(";\n", cg->out);
    cg_line(cg, "if (fr_str_builder_char(%s, %s) == 0) {", builder, byte);
    cg->indent++;
    cg_line(cg, "if (%s) %.*s = fr_str_append(fr_detail_str_builder_current(%s), %s);",
            did_append, (int)text.len, text.data, builder, byte);
    cg_line(cg, "else %.*s = fr_str_append(%.*s, %s);",
            (int)text.len, text.data, (int)text.len, text.data, byte);
    cg_indent(cg);
    fprintf(cg->out, "%.*s = ", (int)step->as.assign.name.len, step->as.assign.name.data);
    emit_assignment_rhs(cg, step);
    fputs(";\n", cg->out);
    cg_line(cg, "%s = 0;", builder);
    cg_line(cg, "break;");
    cg->indent--;
    cg_line(cg, "}");
    cg_line(cg, "%s = 1;", did_append);
    cg_indent(cg);
    fprintf(cg->out, "%.*s = ", (int)step->as.assign.name.len, step->as.assign.name.data);
    emit_assignment_rhs(cg, step);
    fputs(";\n", cg->out);
    cg->indent--;
    cg_line(cg, "}");
    cg_line(cg, "if (%s && %s) %.*s = fr_str_builder_finish_take(%s);",
            builder, did_append, (int)text.len, text.data, builder);
    cg_line(cg, "if (!%s) {", builder);
    cg->indent++;
    emit_append_loop_legacy(cg, loop, proc_var, in_coro);
    cg->indent--;
    cg_line(cg, "}");
    cg->indent--;
    cg_line(cg, "} else {");
    cg->indent++;
    emit_append_loop_legacy(cg, loop, proc_var, in_coro);
    cg->indent--;
    cg_line(cg, "}");
    cg->indent--;
    cg_line(cg, "}");
    cg->safe_increment = saved_increment;
}

static void emit_view_append_loop_builder(Codegen *cg, Stmt *loop, ForgeStr view) {
    Stmt *append = loop->as.while_stmt.body->first, *step = append->next;
    ForgeStr index = step->as.assign.name;
    char builder[64], byte[64], snapshot[64];
    do {
        int id = ++cg->label_seq;
        snprintf(builder, sizeof(builder), "__forge_view_builder_%d", id);
        snprintf(byte, sizeof(byte), "__forge_view_byte_%d", id);
        snprintf(snapshot, sizeof(snapshot), "__forge_view_snapshot_%d", id);
    } while (cg_is_local(cg, forge_str(builder)) || cg_is_local(cg, forge_str(byte)) ||
             cg_is_local(cg, forge_str(snapshot)));
    Stmt *saved_increment = cg->safe_increment;
    cg->safe_increment = prove_loop_increment(cg, loop->as.while_stmt.cond,
                                               loop->as.while_stmt.body, step);
    cg_line(cg, "{");
    cg->indent++;
    cg_line(cg, "int64_t %s = 0;", builder);
    cg_line(cg, "while (");
    emit_expr(cg, loop->as.while_stmt.cond);
    fputs(") {\n", cg->out);
    cg->indent++;
    /* Even a zero source view evaluates its byte argument, including traps.
     * Lazily allocate only after that evaluation and the first true guard. */
    cg_line(cg, "int64_t %s = ", byte);
    emit_expr(cg, append->as.assign.value->as.call.args[1]);
    fputs(";\n", cg->out);
    cg_line(cg, "if (%.*s) {", (int)view.len, view.data);
    cg->indent++;
    cg_line(cg, "if (!%s) %s = fr_str_builder();", builder, builder);
    /* Here the original prefix length equals the zero-based index. Match the
     * immutable operation's size limit, including its view metadata, on 32-bit
     * platforms too. Allocation failure is sticky; never use string recovery. */
    cg_line(cg, "if (!%s || (uint64_t)%.*s > SIZE_MAX - sizeof(fr_detail_str_view) - 2 ||",
            builder, (int)index.len, index.data);
    cg_line(cg, "    !fr_str_builder_char(%s, %s)) {", builder, byte);
    cg->indent++;
    cg_line(cg, "%.*s = 0;", (int)view.len, view.data);
    cg_line(cg, "%s = 0;", builder);
    cg->indent--;
    cg_line(cg, "}");
    cg->indent--;
    cg_line(cg, "}");
    cg_indent(cg);
    fprintf(cg->out, "%.*s = ", (int)index.len, index.data);
    emit_assignment_rhs(cg, step);
    fputs(";\n", cg->out);
    cg->indent--;
    cg_line(cg, "}");
    /* No iteration means no builder and leaves the exact initial view intact.
     * Detach before publishing: later consumers cannot see mutable storage. */
    cg_line(cg, "if (%s) {", builder);
    cg->indent++;
    cg_line(cg, "const char *%s = fr_str_builder_finish_take(%s);", snapshot, builder);
    cg_line(cg, "%.*s = %s ? fr_str_view(%s) : 0;",
            (int)view.len, view.data, snapshot, snapshot);
    cg->indent--;
    cg_line(cg, "}");
    cg->indent--;
    cg_line(cg, "}");
    cg->safe_increment = saved_increment;
}

static void emit_reserved_builder_loop(Codegen *cg, CgReservedBuilderLoop plan,
                                       ForgeStr builder, const char *proc_var) {
    Stmt *loop = plan.loop, *step = loop->as.while_stmt.body->first->next;
    ForgeStr index = plan.index->as.let.name;
    ForgeStr bound = loop->as.while_stmt.cond->as.binary.right->as.ident;
    char descriptor[64], data[64];
    int id = ++cg->label_seq;
    do {
        snprintf(descriptor, sizeof(descriptor), "__forge_reserved_builder_%d", id);
        snprintf(data, sizeof(data), "__forge_reserved_data_%d", id++);
    } while (cg_is_local(cg, forge_str(descriptor)) || cg_is_local(cg, forge_str(data)) ||
             cg_lookup_fn(cg, forge_str(descriptor)) || cg_lookup_fn(cg, forge_str(data)));
    Stmt *saved_increment = cg->safe_increment;
    cg->safe_increment = step;
    cg_line(cg, "{");
    cg->indent++;
    cg_line(cg, "fr_detail_str_builder *%s = (fr_detail_str_builder *)(intptr_t)%.*s;",
            descriptor, (int)builder.len, builder.data);
    cg_line(cg, "if (%s && %.*s > 0 && %s->data && %s->len == 0 && %s->cap > (uint64_t)%.*s) {",
            descriptor, (int)bound.len, bound.data, descriptor, descriptor, descriptor,
            (int)bound.len, bound.data);
    cg->indent++;
    cg_line(cg, "char *%s = %s->data;", data, descriptor);
    cg_line(cg, "while (%.*s < %.*s) {", (int)index.len, index.data, (int)bound.len, bound.data);
    cg->indent++;
    cg_indent(cg);
    fprintf(cg->out, "%s[%.*s] = (char)(", data, (int)index.len, index.data);
    emit_expr(cg, plan.byte);
    fputs(");\n", cg->out);
    cg_line(cg, "%.*s = %.*s + INT64_C(1);", (int)index.len, index.data, (int)index.len, index.data);
    cg->indent--;
    cg_line(cg, "}");
    cg_line(cg, "%s->len = (size_t)%.*s;", descriptor, (int)bound.len, bound.data);
    cg->indent--;
    cg_line(cg, "} else {");
    cg->indent++;
    cg_line(cg, "while (%.*s < %.*s) {", (int)index.len, index.data, (int)bound.len, bound.data);
    cg->indent++;
    emit_block_stmts(cg, loop->as.while_stmt.body->first, proc_var, false);
    cg->indent--;
    cg_line(cg, "}");
    cg->indent--;
    cg_line(cg, "}");
    cg->indent--;
    cg_line(cg, "}");
    cg->safe_increment = saved_increment;
}

/* A pure two-assignment recurrence closes the nonnegative Mersenne range.
 * Version the whole loop instead of testing that range for every update. */
static bool emit_mersenne_loop_version(Codegen *cg, Stmt *loop,
                                       const char *proc_var) {
    if (!cg->prove_loop_increments || cg->state_prefix || !cg->safe_increment) return false;
    Stmt *update = loop->as.while_stmt.body->first;
    Stmt *step = update ? update->next : NULL;
    if (!update || update->kind != STMT_ASSIGN || !step || step->next ||
        step != cg->safe_increment) return false;
    Expr *value = NULL;
    int64_t multiplier, modulus, addend;
    unsigned shift;
    if (!cg_mersenne_modmul(cg, update->as.assign.value, &value,
                            &multiplier, &modulus, &addend, &shift) ||
        !forge_str_eq(update->as.assign.name, value->as.ident) ||
        forge_str_eq(update->as.assign.name, step->as.assign.name) ||
        (uint64_t)(modulus - 1) > ((uint64_t)INT64_MAX - (uint64_t)addend) / (uint64_t)multiplier)
        return false;
    char product[64], folded[64];
    int id = ++cg->label_seq;
    do {
        snprintf(product, sizeof(product), "__forge_mersenne_product_%d", id);
        snprintf(folded, sizeof(folded), "__forge_mersenne_folded_%d", id++);
    } while (cg_is_local(cg, forge_str(product)) || cg_is_local(cg, forge_str(folded)) ||
             cg_lookup_fn(cg, forge_str(product)) || cg_lookup_fn(cg, forge_str(folded)));
    ForgeStr name = value->as.ident;
    cg_line(cg, "if ((");
    emit_expr(cg, loop->as.while_stmt.cond);
    fprintf(cg->out, ") && %.*s >= 0 && %.*s < INT64_C(%lld)) {\n",
            (int)name.len, name.data, (int)name.len, name.data, (long long)modulus);
    cg->indent++;
    cg_line(cg, "while (");
    emit_expr(cg, loop->as.while_stmt.cond);
    fputs(") {\n", cg->out);
    cg->indent++;
    cg_line(cg, "uint64_t %s = (uint64_t)%.*s * UINT64_C(%lld);",
            product, (int)name.len, name.data, (long long)multiplier);
    if (addend)
        cg_line(cg, "%s += UINT64_C(%lld);", product, (long long)addend);
    cg_line(cg, "uint64_t %s = (%s >> %u) + (%s & UINT64_C(%lld));",
            folded, product, shift, product, (long long)modulus);
    cg_line(cg, "if (%s >= UINT64_C(%lld)) %s -= UINT64_C(%lld);",
            folded, (long long)modulus, folded, (long long)modulus);
    cg_line(cg, "%.*s = (int64_t)%s;", (int)name.len, name.data, folded);
    cg_indent(cg);
    fprintf(cg->out, "%.*s = ", (int)step->as.assign.name.len, step->as.assign.name.data);
    emit_assignment_rhs(cg, step);
    fputs(";\n", cg->out);
    cg->indent--;
    cg_line(cg, "}");
    cg->indent--;
    cg_line(cg, "} else {");
    cg->indent++;
    cg_line(cg, "while (");
    emit_expr(cg, loop->as.while_stmt.cond);
    fputs(") {\n", cg->out);
    cg->indent++;
    emit_block_stmts(cg, loop->as.while_stmt.body->first, proc_var, false);
    cg->indent--;
    cg_line(cg, "}");
    cg->indent--;
    cg_line(cg, "}");
    return true;
}

static bool cg_ident_named(const Expr *e, ForgeStr name) {
    return e && e->kind == EXPR_IDENT && forge_str_eq(e->as.ident, name);
}

static bool cg_unit_assignment(const Stmt *s, ForgeStr name) {
    if (!s || s->kind != STMT_ASSIGN || !forge_str_eq(s->as.assign.name, name)) return false;
    Expr *e = s->as.assign.value;
    if (!e || e->kind != EXPR_BINARY || e->as.binary.op != BIN_ADD) return false;
    Expr *a = e->as.binary.left, *b = e->as.binary.right;
    if (a->kind == EXPR_INT) { Expr *swap = a; a = b; b = swap; }
    return cg_ident_named(a, name) && b->kind == EXPR_INT && b->as.int_val == 1;
}

/* Recognize a complete, pure periodic byte sum by syntax/binding, never by
 * function name. Only its sum escapes; index/weight state is dead on return. */
static bool emit_periodic_view_sum(Codegen *cg, FnDecl *fn) {
    if (!cg->prove_loop_increments || cg->state_prefix || fn->ret_type.kind != TY_INT) return false;
    for (Param *p = fn->params; p; p = p->next)
        if (p->type.kind != TY_INT) return false;
    Stmt *lets[4];
    Stmt *s = fn->body.first;
    for (unsigned i = 0; i < 4; i++) {
        if (!s || s->kind != STMT_LET || s->as.let.type.kind != TY_INT ||
            !s->as.let.init || cg_is_local(cg, s->as.let.name)) return false;
        for (unsigned j = 0; j < i; j++)
            if (forge_str_eq(lets[j]->as.let.name, s->as.let.name)) return false;
        lets[i] = s; s = s->next;
    }
    Stmt *loop = s, *ret = s ? s->next : NULL;
    if (!loop || loop->kind != STMT_WHILE || !loop->as.while_stmt.body ||
        !ret || ret->kind != STMT_RETURN || ret->next) return false;
    ForgeStr n = lets[0]->as.let.name, index = lets[1]->as.let.name;
    ForgeStr sum = lets[2]->as.let.name, weight = lets[3]->as.let.name;
    Expr *length = lets[0]->as.let.init;
    if (!cg_view_accessor_call(cg, length, "fr_str_view_len", 1) ||
        cg_resolve_fn_ref(cg, length->as.call.name) ||
        length->as.call.args[0]->kind != EXPR_IDENT) return false;
    ForgeStr view = length->as.call.args[0]->as.ident;
    if (!cg_is_local(cg, view) || cg_lookup_local(cg, view).kind != TY_INT ||
        lets[1]->as.let.init->kind != EXPR_INT || lets[1]->as.let.init->as.int_val != 0 ||
        lets[2]->as.let.init->kind != EXPR_INT || lets[2]->as.let.init->as.int_val != 0 ||
        !cg_ident_named(ret->as.ret, sum)) return false;
    Expr *initial = lets[3]->as.let.init;
    if (initial->kind != EXPR_BINARY || initial->as.binary.op != BIN_ADD) return false;
    Expr *mod = initial->as.binary.left, *one = initial->as.binary.right;
    if (mod->kind == EXPR_INT) { Expr *swap = mod; mod = one; one = swap; }
    if (one->kind != EXPR_INT || one->as.int_val != 1 || mod->kind != EXPR_BINARY ||
        mod->as.binary.op != BIN_MOD || mod->as.binary.left->kind != EXPR_IDENT ||
        mod->as.binary.right->kind != EXPR_INT) return false;
    ForgeStr offset = mod->as.binary.left->as.ident;
    int64_t period = mod->as.binary.right->as.int_val;
    if (period < 1 || period > 32 || !cg_is_local(cg, offset) ||
        cg_lookup_local(cg, offset).kind != TY_INT) return false;
    Expr *cond = loop->as.while_stmt.cond;
    if (!cond || cond->kind != EXPR_BINARY || cond->as.binary.op != BIN_LT ||
        !cg_ident_named(cond->as.binary.left, index) || !cg_ident_named(cond->as.binary.right, n)) return false;
    Stmt *add = loop->as.while_stmt.body->first;
    Stmt *step = add ? add->next : NULL, *advance = step ? step->next : NULL;
    Stmt *reset = advance ? advance->next : NULL;
    if (!add || add->kind != STMT_ASSIGN || !forge_str_eq(add->as.assign.name, sum) ||
        !cg_unit_assignment(step, index) || !cg_unit_assignment(advance, weight) ||
        !reset || reset->kind != STMT_IF || reset->next || reset->as.if_stmt.else_br) return false;
    Expr *addition = add->as.assign.value;
    if (!addition || addition->kind != EXPR_BINARY || addition->as.binary.op != BIN_ADD) return false;
    Expr *acc = addition->as.binary.left, *product = addition->as.binary.right;
    if (!cg_ident_named(acc, sum)) { Expr *swap = acc; acc = product; product = swap; }
    if (!cg_ident_named(acc, sum) || product->kind != EXPR_BINARY || product->as.binary.op != BIN_MUL) return false;
    Expr *at = product->as.binary.left, *factor = product->as.binary.right;
    if (cg_ident_named(at, weight)) { Expr *swap = at; at = factor; factor = swap; }
    if (!cg_ident_named(factor, weight) || !cg_view_accessor_call(cg, at, "fr_str_view_at", 2) ||
        cg_resolve_fn_ref(cg, at->as.call.name) ||
        !cg_ident_named(at->as.call.args[0], view) || !cg_ident_named(at->as.call.args[1], index)) return false;
    Expr *test = reset->as.if_stmt.cond;
    Stmt *assign = reset->as.if_stmt.then_br ? reset->as.if_stmt.then_br->first : NULL;
    if (!test || test->kind != EXPR_BINARY || test->as.binary.op != BIN_GT ||
        !cg_ident_named(test->as.binary.left, weight) || test->as.binary.right->kind != EXPR_INT ||
        test->as.binary.right->as.int_val != period || !assign || assign->kind != STMT_ASSIGN ||
        assign->next || !forge_str_eq(assign->as.assign.name, weight) ||
        !assign->as.assign.value || assign->as.assign.value->kind != EXPR_INT ||
        assign->as.assign.value->as.int_val != 1) return false;

    /* All syntax walks above are fixed-size. Bound the coefficient array and
     * the matching work independently of input size. */
    if (cg->loop_scan_left < 64) return false;
    cg->loop_scan_left -= 64;
    char names[8][64];
    const char *roles[] = {"view", "weight", "data", "count", "index", "sum", "weights", "j"};
    int id = ++cg->label_seq;
    bool collision;
    do {
        collision = false;
        for (unsigned i = 0; i < 8; i++) {
            snprintf(names[i], sizeof(names[i]), "__forge_periodic_%s_%d", roles[i], id);
            if (cg_is_local(cg, forge_str(names[i])) || cg_resolve_fn_ref(cg, forge_str(names[i]))) collision = true;
        }
        id++;
    } while (collision);
    cg_line(cg, "{"); cg->indent++;
    cg_line(cg, "const fr_detail_str_view *%s = (const fr_detail_str_view *)(intptr_t)%.*s;",
            names[0], (int)view.len, view.data);
    cg_line(cg, "int64_t %s = %.*s %% INT64_C(%lld) + INT64_C(1);",
            names[1], (int)offset.len, offset.data, (long long)period);
    cg_line(cg, "if (%s && %s->data && %s->len <= (uint64_t)INT64_MAX && %s > 0) {",
            names[0], names[0], names[0], names[1]); cg->indent++;
    cg_line(cg, "const unsigned char *%s = (const unsigned char *)%s->data;", names[2], names[0]);
    cg_line(cg, "uint64_t %s = (uint64_t)%s->len, %s = 0, %s = 0;", names[3], names[0], names[4], names[5]);
    cg_line(cg, "unsigned %s[%lld];", names[6], (long long)period);
    cg_line(cg, "for (unsigned %s = 0; %s < %lld; %s++) {", names[7], names[7], (long long)period, names[7]);
    cg->indent++;
    cg_line(cg, "%s[%s] = (unsigned)%s + %s;", names[6], names[7], names[1], names[7]);
    cg_line(cg, "if (%s[%s] > %lld) %s[%s] -= %lld;", names[6], names[7], (long long)period,
            names[6], names[7], (long long)period);
    cg->indent--; cg_line(cg, "}");
    cg_line(cg, "while (%s - %s >= %lld) {", names[3], names[4], (long long)period); cg->indent++;
    cg_line(cg, "for (unsigned %s = 0; %s < %lld; %s++)", names[7], names[7], (long long)period, names[7]);
    cg_line(cg, "    %s += (uint64_t)(%s[%s + %s] * %s[%s]);", names[5], names[2], names[4], names[7], names[6], names[7]);
    cg_line(cg, "%s += %lld;", names[4], (long long)period); cg->indent--; cg_line(cg, "}");
    cg_line(cg, "unsigned %s = 0;", names[7]);
    cg_line(cg, "while (%s < %s) {", names[4], names[3]); cg->indent++;
    cg_line(cg, "%s += (uint64_t)(%s[%s] * %s[%s]);", names[5], names[2], names[4], names[6], names[7]);
    cg_line(cg, "%s++; %s++;", names[4], names[7]); cg->indent--; cg_line(cg, "}");
    cg_line(cg, "return __forge_i64_bits(%s);", names[5]);
    cg->indent--; cg_line(cg, "}"); cg->indent--; cg_line(cg, "}");
    return true;
}

static void emit_stmts(Codegen *cg, Stmt *s, const char *proc_var, bool in_coro) {
    cg_enter(cg);
    Stmt *block_first = s;
    while (s) {
        switch (s->kind) {
        case STMT_LET: {
            Stmt *append_loop = NULL;
            CgReservedBuilderLoop reserved = !in_coro ? cg_reserved_builder_loop(cg, s) : (CgReservedBuilderLoop){0};
            Stmt *view_append_loop = !in_coro ? cg_find_view_append_loop(cg, s) : NULL;
            if (!in_coro && !cg->state_prefix && cg_is_empty_string_let(s))
                append_loop = cg_find_append_loop(cg, s->next, s->as.let.name);
            emit_local_let(cg, s);
            if (reserved.loop) {
                if (reserved.guard) emit_if_stmt(cg, reserved.guard, proc_var, false);
                emit_local_let(cg, reserved.index);
                emit_reserved_builder_loop(cg, reserved, s->as.let.name, proc_var);
                s = reserved.loop;
            } else if (view_append_loop) {
                for (Stmt *pre = s->next; pre != view_append_loop; pre = pre->next)
                    emit_local_let(cg, pre);
                emit_view_append_loop_builder(cg, view_append_loop, s->as.let.name);
                s = view_append_loop;
            } else if (append_loop) {
                for (Stmt *pre = s->next; pre != append_loop; pre = pre->next)
                    emit_local_let(cg, pre);
                emit_append_loop_builder(cg, append_loop, s->as.let.name, proc_var, in_coro);
                s = append_loop;
            }
            break;
        }
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
            Stmt *saved_increment = cg->safe_increment;
            ForgeStr saved_view_at_view = cg->safe_view_at_view;
            ForgeStr saved_view_at_index = cg->safe_view_at_index;
            cg->safe_increment = prove_loop_increment(cg, s->as.while_stmt.cond,
                s->as.while_stmt.body, s->as.while_stmt.body->last);
            cg->safe_view_at_view = (ForgeStr){0};
            cg->safe_view_at_index = (ForgeStr){0};
            prove_view_at_loop(cg, block_first, s, cg->safe_increment);
            if (!in_coro && emit_mersenne_loop_version(cg, s, proc_var)) {
                cg->safe_increment = saved_increment;
                cg->safe_view_at_view = saved_view_at_view;
                cg->safe_view_at_index = saved_view_at_index;
                break;
            }
            cg_line(cg, "while (");
            emit_expr(cg, s->as.while_stmt.cond);
            fputs(") {\n", cg->out);
            cg->indent++;
            /* C's own `continue` is correct for a while loop. */
            int saved_cont = cg->continue_label;
            cg->continue_label = 0;
            emit_block_stmts(cg, s->as.while_stmt.body->first, proc_var, in_coro);
            cg->continue_label = saved_cont;
            cg->safe_increment = saved_increment;
            cg->safe_view_at_view = saved_view_at_view;
            cg->safe_view_at_index = saved_view_at_index;
            cg->indent--;
            cg_indent(cg);
            fputs("}\n", cg->out);
            break;
        }
        case STMT_FOR: {
            Stmt *saved_increment = cg->safe_increment;
            size_t saved_locals = cg->local_count;
            bool suspends = block_has_yield(s->as.for_stmt.body->first);
            bool inline_step = for_step_inlinable(s->as.for_stmt.step);
            int saved_cont = cg->continue_label;
            cg_indent(cg);
            fputs("{\n", cg->out);
            cg->indent++;
            if (s->as.for_stmt.init) emit_stmts(cg, s->as.for_stmt.init, proc_var, in_coro);
            cg->safe_increment = prove_loop_increment(cg, s->as.for_stmt.cond,
                s->as.for_stmt.body, s->as.for_stmt.step);
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
                emit_block_stmts(cg, s->as.for_stmt.body->first, proc_var, in_coro);
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
                emit_block_stmts(cg, s->as.for_stmt.body->first, proc_var, in_coro);
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
            cg->local_count = saved_locals;
            cg->safe_increment = saved_increment;
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
                fputs("__forge_send_owned_string((fr_process_t *)(intptr_t)(", cg->out);
                emit_expr(cg, s->as.send.target);
                fprintf(cg->out, "), %d, &", s->as.send.tag);
                if (!s->as.send.value || s->as.send.value->kind != EXPR_IDENT)
                    forge_die("owned send requires a named string binding");
                fprintf(cg->out, "%.*s", (int)s->as.send.value->as.ident.len,
                        s->as.send.value->as.ident.data);
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
            emit_assignment_rhs(cg, s);
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
                emit_block_stmts(cg, arm->body->first, proc_var, in_coro);
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
            cg_line(cg, "{");
            cg->indent++;
            emit_block_stmts(cg, s->as.block->first, proc_var, in_coro);
            cg->indent--;
            cg_line(cg, "}");
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
            fprintf(out, "static const int64_t forge_const_%.*s = ",
                    (int)c->name.len, c->name.data);
            if (c->value->as.int_val == INT64_MIN) fputs("INT64_MIN", out);
            else fprintf(out, "INT64_C(%lld)", (long long)c->value->as.int_val);
            fputs(";\n", out);
            break;
        case EXPR_FLOAT:
            fprintf(out, "static const double forge_const_%.*s = %.17g;\n",
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

/* Dispatch from actual C operand types: imported calls and identifiers need
 * not have resolved AST types here. _Generic does not evaluate its controlling
 * expressions; each operand is evaluated once, as a function argument. */
static void emit_arithmetic_helpers(FILE *out) {
    fputs("static inline int64_t __forge_i64_bits(uint64_t bits) {\n"
          "    int64_t value; memcpy(&value, &bits, sizeof(value)); return value;\n"
          "}\n"
          "static inline int64_t __forge_add_i64(int64_t a, int64_t b) { return __forge_i64_bits((uint64_t)a + (uint64_t)b); }\n"
          "static inline int64_t __forge_sub_i64(int64_t a, int64_t b) { return __forge_i64_bits((uint64_t)a - (uint64_t)b); }\n"
          "static inline int64_t __forge_mul_i64(int64_t a, int64_t b) { return __forge_i64_bits((uint64_t)a * (uint64_t)b); }\n"
          "static inline void __forge_check_div_i64(int64_t a, int64_t b) {\n"
          "    if (b == 0 || (a == INT64_MIN && b == -1)) { fputs(\"Forge integer division\\n\", stderr); exit(EXIT_FAILURE); }\n"
          "}\n"
          "static inline int64_t __forge_div_i64(int64_t a, int64_t b) { __forge_check_div_i64(a, b); return a / b; }\n"
          "static inline int64_t __forge_mod_i64(int64_t a, int64_t b) { __forge_check_div_i64(a, b); return a % b; }\n"
          "static inline int64_t __forge_mersenne_modmul_i64(int64_t value, int64_t multiplier, int64_t modulus, unsigned shift) {\n"
          "    if (value >= 0 && value < modulus && multiplier > 0 && multiplier < modulus &&\n"
          "        (uint64_t)(modulus - 1) <= (uint64_t)INT64_MAX / (uint64_t)multiplier) {\n"
          "        uint64_t product = (uint64_t)value * (uint64_t)multiplier;\n"
          "        uint64_t folded = (product >> shift) + (product & (uint64_t)modulus);\n"
          "        if (folded >= (uint64_t)modulus) folded -= (uint64_t)modulus;\n"
          "        return (int64_t)folded;\n"
          "    }\n"
          "    return __forge_mod_i64(__forge_mul_i64(value, multiplier), modulus);\n"
          "}\n\n", out);
    fputs("static inline int64_t __forge_mersenne_modmuladd_i64(int64_t value, int64_t multiplier, int64_t addend, int64_t modulus, unsigned shift) {\n"
          "    if (value >= 0 && value < modulus && multiplier > 0 && multiplier < modulus &&\n"
          "        addend > 0 && addend < modulus &&\n"
          "        (uint64_t)(modulus - 1) <= (uint64_t)INT64_MAX / (uint64_t)multiplier) {\n"
          "        uint64_t product = (uint64_t)value * (uint64_t)multiplier;\n"
          "        if (product <= (uint64_t)INT64_MAX - (uint64_t)addend) {\n"
          "            uint64_t folded = (product >> shift) + (product & (uint64_t)modulus);\n"
          "            if (folded >= (uint64_t)modulus) folded -= (uint64_t)modulus;\n"
          "            folded += (uint64_t)addend;\n"
          "            if (folded >= (uint64_t)modulus) folded -= (uint64_t)modulus;\n"
          "            return (int64_t)folded;\n"
          "        }\n"
          "    }\n"
          "    return __forge_mod_i64(__forge_add_i64(__forge_mul_i64(value, multiplier), addend), modulus);\n"
          "}\n\n", out);
    fputs("typedef struct { int64_t divisor; uint64_t reciprocal; } __forge_mod_descriptor;\n"
          "static inline __forge_mod_descriptor __forge_mod_prepare(int64_t divisor) {\n"
          "    __forge_mod_descriptor result = {divisor, 0};\n"
          "#if defined(__SIZEOF_INT128__)\n"
          "    if (divisor > 1) result.reciprocal = UINT64_MAX / (uint64_t)divisor + UINT64_C(1);\n"
          "#endif\n"
          "    return result;\n"
          "}\n"
          "static inline int64_t __forge_mod_invariant(int64_t value, __forge_mod_descriptor descriptor) {\n"
          "#if defined(__SIZEOF_INT128__)\n"
          "    if (value >= 0 && descriptor.divisor > 1) {\n"
          "        uint64_t n = (uint64_t)value, d = (uint64_t)descriptor.divisor;\n"
          "        uint64_t q = (uint64_t)(((unsigned __int128)n * descriptor.reciprocal) >> 64);\n"
          "        /* ceil(2^64/d) gives the exact quotient or one larger.\n"
          "         * q*d <= n+d <= 2^64-2, so the product cannot wrap. */\n"
          "        uint64_t product = q * d;\n"
          "        if (product > n) product -= d;\n"
          "        return (int64_t)(n - product);\n"
          "    }\n"
          "#endif\n"
          "    return __forge_mod_i64(value, descriptor.divisor);\n"
          "}\n\n", out);
    const char *names[] = { "ADD", "SUB", "MUL", "DIV" };
    const char *lower[] = { "add", "sub", "mul", "div" };
    const char *ops[] = { "+", "-", "*", "/" };
    for (size_t i = 0; i < 4; i++) {
        fprintf(out, "static inline double __forge_%s_f64(double a, double b) { return a %s b; }\n", lower[i], ops[i]);
        fprintf(out, "#define FORGE_%s(a, b) _Generic((a), float: __forge_%s_f64, double: __forge_%s_f64, default: _Generic((b), float: __forge_%s_f64, double: __forge_%s_f64, default: __forge_%s_i64))((a), (b))\n", names[i], lower[i], lower[i], lower[i], lower[i], lower[i]);
    }
    /* An external header can reveal a float return type that the semantic
     * checker could not resolve. Preserve C's rejection of float remainder. */
    fputs("#define FORGE_MOD(a, b) ({ _Static_assert(_Generic((a), float: 0, double: 0, default: _Generic((b), float: 0, double: 0, default: 1)), \"Forge remainder requires integer operands\"); __forge_mod_i64((a), (b)); })\n\n", out);
}

static bool cg_mersenne_modmul(Codegen *cg, Expr *e, Expr **value,
                               int64_t *multiplier, int64_t *modulus,
                               int64_t *addend, unsigned *shift) {
    if (!e || e->kind != EXPR_BINARY || e->as.binary.op != BIN_MOD) return false;
    Expr *product = e->as.binary.left;
    Expr *mod = e->as.binary.right;
    Expr *increment = NULL;
    if (!product || !mod || mod->kind != EXPR_INT || mod->as.int_val <= 1) return false;
    if (product->kind == EXPR_BINARY && product->as.binary.op == BIN_ADD) {
        Expr *left = product->as.binary.left;
        Expr *right = product->as.binary.right;
        if (left && left->kind == EXPR_BINARY && left->as.binary.op == BIN_MUL &&
            right && right->kind == EXPR_INT) {
            product = left;
            increment = right;
        } else if (right && right->kind == EXPR_BINARY && right->as.binary.op == BIN_MUL &&
                   left && left->kind == EXPR_INT) {
            product = right;
            increment = left;
        } else {
            return false;
        }
    }
    if (product->kind != EXPR_BINARY || product->as.binary.op != BIN_MUL) return false;
    Expr *var = product->as.binary.left;
    Expr *factor = product->as.binary.right;
    if (var->kind == EXPR_INT) { Expr *swap = var; var = factor; factor = swap; }
    if (var->kind != EXPR_IDENT || !cg_is_local(cg, var->as.ident) ||
        cg_lookup_local(cg, var->as.ident).kind != TY_INT ||
        factor->kind != EXPR_INT || factor->as.int_val <= 0) return false;

    uint64_t m = (uint64_t)mod->as.int_val;
    uint64_t power = m + UINT64_C(1);
    if ((m & power) != 0 || (power & (power - 1)) != 0) return false;
    unsigned k = 0;
    while ((power >> k) > 1) k++;
    if (k < 2 || k > 63 || (uint64_t)factor->as.int_val >= m) return false;
    int64_t constant_addend = increment ? increment->as.int_val : 0;
    if (constant_addend < 0 || (increment && (uint64_t)constant_addend >= m)) return false;

    *value = var;
    *multiplier = factor->as.int_val;
    *modulus = mod->as.int_val;
    *addend = constant_addend;
    *shift = k;
    return true;
}

static void emit_owned_send_helper(FILE *out) {
    fputs("static inline void __forge_send_owned_string(fr_process_t *dst, int tag, const char **source) {\n"
          "    char *payload = fr_own_clone(*source);\n"
          "    if (*source && !payload) { fputs(\"forge: owned message allocation failed\\n\", stderr); abort(); }\n"
          "    *source = NULL;\n"
          "    fr_send_move(dst, tag, payload);\n"
          "}\n\n", out);
}

void codegen_emit_library(Program *prog, FILE *out_c, FILE *out_h, const char *runtime_include) {
    if (!prog->library.present) {
        fprintf(stderr, "forge: no library block found\n");
        exit(1);
    }
    LibraryDecl *lib = &prog->library;
    /* Heap-allocated: a fixed buffer would silently truncate (and so collide)
     * on a long library name. */
    if (lib->name.len > SIZE_MAX - 16) forge_die("library name too large");
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
    fputs("#include \"forge/io.h\"\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n", out_c);
    fputs("#include \"forge/ownership.h\"\n", out_c);
    emit_owned_send_helper(out_c);
    emit_arithmetic_helpers(out_c);
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
        cg.prove_loop_increments = true;
        cg_push_params(&cg, fn->params);
        emit_invariant_mod_descriptors(&cg, fn);
        emit_periodic_view_sum(&cg, fn);
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
    fputs("#include \"forge/io.h\"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n", out);
    fputs("#include \"forge/os.h\"\n", out);
    fputs("#include \"forge/ownership.h\"\n", out);
    emit_owned_send_helper(out);
    emit_arithmetic_helpers(out);
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

    for (size_t i = 0; i < prog->module_count; i++) {
        FileModule *mod = &prog->modules[i];
        for (size_t j = 0; j < mod->fn_count; j++) {
            char sym[128];
            forge_mod_mangle(sym, sizeof(sym), mod->name, mod->functions[j].name);
            fputs("static ", out);
            emit_fn_signature(out, sym, &mod->functions[j]);
            fputs(";\n", out);
        }
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
        cg.prove_loop_increments = true;
        cg_push_params(&cg, fn->params);
        emit_invariant_mod_descriptors(&cg, fn);
        emit_periodic_view_sum(&cg, fn);
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
            cg.prove_loop_increments = true;
            cg_push_params(&cg, fn->params);
            emit_invariant_mod_descriptors(&cg, fn);
            emit_periodic_view_sum(&cg, fn);
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
        cg.prove_loop_increments = true;
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
