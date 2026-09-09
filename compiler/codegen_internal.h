/* Internals shared by the codegen translation units (codegen.c, codegen_util.c,
 * codegen_symbols.c, codegen_expr.c, codegen_stmt.c, codegen_coro.c).
 * Not part of the compiler's public interface -- see codegen.h for that. */
#ifndef FORGE_CODEGEN_INTERNAL_H
#define FORGE_CODEGEN_INTERNAL_H

#include "codegen.h"

/* Guard against unbounded recursion over deeply nested ASTs: emitting a
 * 60k-deep expression tree overflows the C stack long before it overflows
 * anything else. */
#define CG_MAX_DEPTH 3000

/* Size of the buffers passed to forge_lib_mangle / forge_mod_mangle. They
 * hash-suffix rather than truncate, so this bounds symbol length, not
 * correctness. */
#define CG_SYM_MAX 128

/* The two printf arguments a "%.*s" conversion needs for a ForgeStr. */
#define FSTR(s) (int)(s).len, (s).data

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
    ForgeStr name;
    ForgeType type;
} CgLocal;

typedef struct {
    FILE *out;
    int indent;
    Program *prog;
    const char *state_prefix;
    CgLocal *locals;
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

static inline void cg_enter(Codegen *cg) {
    if (++cg->depth > CG_MAX_DEPTH)
        forge_die("expression or statement nesting too deep (max "
                  "3000); simplify the source");
}

static inline void cg_leave(Codegen *cg) { cg->depth--; }

/* Tri-state: whether an expression yields a C string. UNKNOWN means the
 * declaration is genuinely not visible to this compilation (an external
 * function), in which case the C compiler decides via _Generic instead of us
 * guessing from the spelling of the name. */
typedef enum { CG_STR_NO = 0, CG_STR_YES, CG_STR_UNKNOWN } CgStrKind;

/* ---- codegen_util.c ---- */

void cg_indent(Codegen *cg);
void cg_line(Codegen *cg, const char *fmt, ...);
void emit_str(FILE *out, ForgeStr s);
void c_type_name(FILE *out, ForgeType ty);
void emit_c_string_literal(FILE *out, ForgeStr s);
void emit_fn_signature(FILE *out, ForgeStr name, const FnDecl *fn);

/* ---- codegen_symbols.c ---- */

void cg_build_symbols(Codegen *cg);
void cg_free_symbols(Codegen *cg);
void cg_begin_function(Codegen *cg);

void cg_push_local(Codegen *cg, ForgeStr name, ForgeType ty);
void cg_push_params(Codegen *cg, Param *params);
bool cg_is_local(const Codegen *cg, ForgeStr name);

void cg_mark_moved(Codegen *cg, ForgeStr name);
bool cg_is_moved(const Codegen *cg, ForgeStr name);

ConstDecl *cg_find_const(const Codegen *cg, ForgeStr name);
FnDecl *cg_lookup_fn(const Codegen *cg, ForgeStr name);
FnDecl *cg_lookup_module_fn(const Codegen *cg, ForgeStr module, ForgeStr name);
FnDecl *cg_resolve_fn_ref(const Codegen *cg, ForgeStr name);
FnDecl *cg_resolve_qual_fn(const Codegen *cg, ForgeStr module, ForgeStr name);
size_t fn_param_count(const FnDecl *fn);

CgStrKind cg_expr_str_kind(Codegen *cg, Expr *e);
bool cg_expr_is_string(Codegen *cg, Expr *e);

/* ---- codegen_expr.c ---- */

void emit_expr(Codegen *cg, Expr *e);

/* ---- codegen_stmt.c ---- */

void emit_stmts(Codegen *cg, Stmt *s, const char *proc_var, bool in_coro);

/* Does this single statement (recursing into its own nested bodies only --
 * never into its siblings) contain a yield/await? */
bool stmt_has_yield(Stmt *s);

/* Statement forms emitted identically inside and outside a coroutine. */
void emit_send_stmt(Codegen *cg, Stmt *s);
void emit_spawn_stmt(Codegen *cg, Stmt *s, const char *proc_expr);
void emit_assign_stmt(Codegen *cg, Stmt *s);

/* ---- codegen_coro.c ---- */

void emit_coro_fn(Codegen *cg, CoroDecl *coro);

#endif
