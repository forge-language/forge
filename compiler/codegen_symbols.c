/* Symbol resolution for codegen: the program-wide (module, name) map built once
 * up front, the per-function local and moved-value tables, and the "does this
 * expression produce a C string" query those feed. */
#include "codegen_internal.h"
#include "mod_registry.h"
#include <string.h>

/* ---------------------------------------------------------------- CgMap --- */

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

/* --------------------------------------------------- program-wide symbols --- */

/* Index every function and const once, up front, so emission never rescans. */
void cg_build_symbols(Codegen *cg) {
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

void cg_free_symbols(Codegen *cg) {
    cg_map_free(&cg->fn_map);
    cg_map_free(&cg->const_map);
}

ConstDecl *cg_find_const(const Codegen *cg, ForgeStr name) {
    return (ConstDecl *)cg_map_get(&cg->const_map, forge_str(""), name);
}

FnDecl *cg_lookup_fn(const Codegen *cg, ForgeStr name) {
    return (FnDecl *)cg_map_get(&cg->fn_map, forge_str(""), name);
}

FnDecl *cg_lookup_module_fn(const Codegen *cg, ForgeStr module, ForgeStr name) {
    if (module.len == 0) return NULL;
    return (FnDecl *)cg_map_get(&cg->fn_map, module, name);
}

/* An unqualified name: a top-level or library function, or -- when emitting a
 * module's own body -- one of that module's functions. */
FnDecl *cg_resolve_fn_ref(const Codegen *cg, ForgeStr name) {
    FnDecl *fn = cg_lookup_fn(cg, name);
    if (fn) return fn;
    return cg_lookup_module_fn(cg, cg->current_module, name);
}

/* Resolve a `module.fn(...)` target to its real declaration. File modules and
 * the library currently being compiled are known; a prebuilt library imported
 * as a binary + header is not, and resolution then fails (see CG_STR_UNKNOWN). */
FnDecl *cg_resolve_qual_fn(const Codegen *cg, ForgeStr module, ForgeStr name) {
    FnDecl *fn = cg_lookup_module_fn(cg, module, name);
    if (fn) return fn;
    LibraryDecl *lib = &cg->prog->library;
    if (lib->present && forge_str_eq(lib->name, module)) {
        for (size_t i = 0; i < lib->fn_count; i++) {
            if (forge_str_eq(lib->functions[i].name, name)) return &lib->functions[i];
        }
    }
    return NULL;
}

size_t fn_param_count(const FnDecl *fn) {
    size_t n = 0;
    if (!fn) return 0;
    for (Param *p = fn->params; p; p = p->next) n++;
    return n;
}

/* ------------------------------------------------------ per-function state --- */

/* Reset the per-function bookkeeping. `moved_locals` in particular is
 * append-only within a function; leaking it into the next function would make
 * every later use of a same-named local look like a use-after-move. */
void cg_begin_function(Codegen *cg) {
    cg->local_count = 0;
    cg->moved_count = 0;
    cg->continue_label = 0;
    cg->depth = 0;
}

static CgLocal *cg_find_local(const Codegen *cg, ForgeStr name) {
    for (size_t i = 0; i < cg->local_count; i++) {
        if (forge_str_eq(cg->locals[i].name, name)) return &cg->locals[i];
    }
    return NULL;
}

bool cg_is_local(const Codegen *cg, ForgeStr name) {
    return cg_find_local(cg, name) != NULL;
}

void cg_push_local(Codegen *cg, ForgeStr name, ForgeType ty) {
    CgLocal *existing = cg_find_local(cg, name);
    if (existing) {
        existing->type = ty;
        return;
    }
    if (cg->local_count == cg->local_cap) {
        size_t cap = cg->local_cap ? cg->local_cap * 2 : 8;
        CgLocal *grown = (CgLocal *)realloc(cg->locals, cap * sizeof(CgLocal));
        if (!grown) forge_die("out of memory");
        cg->locals = grown;
        cg->local_cap = cap;
    }
    cg->locals[cg->local_count].name = name;
    cg->locals[cg->local_count].type = ty;
    cg->local_count++;
}

void cg_push_params(Codegen *cg, Param *params) {
    for (Param *p = params; p; p = p->next) cg_push_local(cg, p->name, p->type);
}

bool cg_is_moved(const Codegen *cg, ForgeStr name) {
    for (size_t i = 0; i < cg->moved_count; i++) {
        if (forge_str_eq(cg->moved_locals[i], name)) return true;
    }
    return false;
}

void cg_mark_moved(Codegen *cg, ForgeStr name) {
    if (cg_is_moved(cg, name)) return;
    if (cg->moved_count == cg->moved_cap) {
        size_t cap = cg->moved_cap ? cg->moved_cap * 2 : 8;
        ForgeStr *grown = (ForgeStr *)realloc(cg->moved_locals, cap * sizeof(ForgeStr));
        if (!grown) forge_die("out of memory");
        cg->moved_locals = grown;
        cg->moved_cap = cap;
    }
    cg->moved_locals[cg->moved_count++] = name;
}

/* ------------------------------------------------------- string-ness query --- */

/* Sorted for bsearch: the C names of the stdlib functions that return a
 * string. Keep in strcmp order. */
static const char *const STDLIB_STRING_FNS[] = {
    "fr_fs_list_dir", "fr_fs_read",
    "fr_gpu_backend", "fr_gpu_device_name",
    "fr_http_get", "fr_http_post",
    "fr_http_req_body", "fr_http_req_method", "fr_http_req_path",
    "fr_io_prompt", "fr_io_read_fd", "fr_io_read_line", "fr_io_read_stdin",
    "fr_json_get_string", "fr_json_stringify_int", "fr_json_stringify_str",
    "fr_os_argv", "fr_os_getenv",
    "fr_str_append", "fr_str_append_str", "fr_str_concat", "fr_str_from_int",
    "fr_str_sub", "fr_str_trim",
    "fr_tcp_recv",
    "fr_udp_peer", "fr_udp_recv",
};

static int cg_str_fn_cmp(const void *key, const void *elem) {
    return strcmp((const char *)key, *(const char *const *)elem);
}

static bool cg_c_name_returns_string(const char *mapped) {
    if (!mapped) return false;
    return bsearch(mapped, STDLIB_STRING_FNS,
                   sizeof(STDLIB_STRING_FNS) / sizeof(STDLIB_STRING_FNS[0]),
                   sizeof(STDLIB_STRING_FNS[0]), cg_str_fn_cmp) != NULL;
}

static CgStrKind cg_type_str_kind(ForgeType ty) {
    return ty.kind == TY_STRING ? CG_STR_YES : CG_STR_NO;
}

CgStrKind cg_expr_str_kind(Codegen *cg, Expr *e) {
    if (!e) return CG_STR_NO;
    switch (e->kind) {
    case EXPR_STRING:
        return CG_STR_YES;
    case EXPR_MOVE:
        return cg_expr_str_kind(cg, e->as.move_expr);
    case EXPR_IDENT: {
        const CgLocal *local = cg_find_local(cg, e->as.ident);
        if (local) return cg_type_str_kind(local->type);
        ConstDecl *c = cg_find_const(cg, e->as.ident);
        if (c) return (c->value && c->value->kind == EXPR_STRING) ? CG_STR_YES : CG_STR_NO;
        return CG_STR_NO;
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

bool cg_expr_is_string(Codegen *cg, Expr *e) {
    return cg_expr_str_kind(cg, e) == CG_STR_YES;
}
