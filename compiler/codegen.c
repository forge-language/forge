/* Top-level C emission: the program preamble, declarations (consts, structs,
 * enums, function prototypes), function bodies, and the generated `main`.
 * The heavy lifting lives in the sibling translation units listed in
 * codegen_internal.h. */
#include "codegen_internal.h"
#include "mod_registry.h"

/* ------------------------------------------------------- declarations --- */

static void emit_consts(Program *prog, FILE *out) {
    for (size_t i = 0; i < prog->const_count; i++) {
        ConstDecl *c = &prog->consts[i];
        ExprKind k = c->value ? c->value->kind : EXPR_IDENT;
        switch (k) {
        case EXPR_INT:
            fprintf(out, "static const int64_t forge_const_%.*s = %lld;\n",
                    FSTR(c->name), (long long)c->value->as.int_val);
            break;
        case EXPR_FLOAT:
            fprintf(out, "static const double forge_const_%.*s = %.17g;\n",
                    FSTR(c->name), c->value->as.float_val);
            break;
        case EXPR_BOOL:
            fprintf(out, "static const int forge_const_%.*s = %d;\n",
                    FSTR(c->name), c->value->as.bool_val ? 1 : 0);
            break;
        case EXPR_STRING:
            fprintf(out, "static const char *forge_const_%.*s = ", FSTR(c->name));
            emit_c_string_literal(out, c->value->as.string_val);
            fputs(";\n", out);
            break;
        default:
            /* Dropping it silently would leave every `forge_const_NAME`
             * reference dangling in the generated C. */
            fprintf(stderr, "forge: const '%.*s' must be an int, float, bool or "
                            "string literal\n", FSTR(c->name));
            exit(1);
        }
    }
    if (prog->const_count > 0) fputs("\n", out);
}

static void emit_structs(Program *prog, FILE *out) {
    for (size_t i = 0; i < prog->struct_count; i++) {
        StructDecl *sd = &prog->structs[i];
        fputs("typedef struct {\n", out);
        for (Field *f = sd->fields; f; f = f->next) {
            fputs("    ", out);
            c_type_name(out, f->type);
            fprintf(out, " %.*s;\n", FSTR(f->name));
        }
        fprintf(out, "} %.*s;\n\n", FSTR(sd->name));
    }
}

static void emit_enums(Program *prog, FILE *out) {
    for (size_t i = 0; i < prog->enum_count; i++) {
        EnumDecl *ed = &prog->enums[i];
        fputs("typedef enum {\n", out);
        for (EnumVariant *v = ed->variants; v; v = v->next) {
            fprintf(out, "    %.*s_%.*s = %lld,\n",
                    FSTR(ed->name), FSTR(v->name), (long long)v->value);
        }
        fprintf(out, "} %.*s;\n\n", FSTR(ed->name));
    }
}

static void emit_import_headers(Program *prog, FILE *out) {
    for (size_t i = 0; i < prog->import_count; i++) {
        if (forge_import_is_file_module(prog, prog->imports[i])) continue;
        const char *hdr = forge_std_header(prog->imports[i]);
        if (hdr) fprintf(out, "#include \"%s\"\n", hdr);
        else fprintf(out, "#include \"%.*s.h\"\n", FSTR(prog->imports[i]));
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

/* ------------------------------------------------------ function bodies --- */

/* Emit `<signature> { ... }` for one function. `module` is the module the body
 * belongs to (empty for top-level and library functions); it decides how
 * unqualified calls inside the body resolve. */
static void emit_fn_body(Codegen *cg, ForgeStr sym, FnDecl *fn, ForgeStr module) {
    emit_fn_signature(cg->out, sym, fn);
    fputs(" {\n", cg->out);
    cg->indent = 1;
    cg_begin_function(cg);
    cg->current_module = module;
    cg_push_params(cg, fn->params);
    emit_stmts(cg, fn->body.first, NULL, false);
    cg->indent = 0;
    fputs("}\n\n", cg->out);
}

static void cg_dispose(Codegen *cg) {
    free(cg->moved_locals);
    free(cg->locals);
    cg_free_symbols(cg);
}

/* ---------------------------------------------------------- library mode --- */

/* FRLIB_<NAME>_H, uppercased. Heap-allocated: a fixed buffer would silently
 * truncate (and so collide) on a long library name. */
static char *library_include_guard(ForgeStr lib_name) {
    size_t cap = lib_name.len + 16;
    char *guard = (char *)malloc(cap);
    if (!guard) forge_die("out of memory");
    snprintf(guard, cap, "FRLIB_%.*s_H", FSTR(lib_name));
    for (char *p = guard; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    }
    return guard;
}

/* The mangled C symbol for one of a library's exported functions. */
static void library_symbol(char *out, size_t cap, LibraryDecl *lib, FnDecl *fn) {
    forge_lib_mangle(out, cap, lib->name, fn->name);
}

void codegen_emit_library(Program *prog, FILE *out_c, FILE *out_h, const char *runtime_include) {
    if (!prog->library.present) {
        fprintf(stderr, "forge: no library block found\n");
        exit(1);
    }
    if (!out_h && !out_c) {
        fprintf(stderr, "forge: library mode requires header or C output\n");
        exit(1);
    }
    LibraryDecl *lib = &prog->library;

    if (out_h) {
        char *guard = library_include_guard(lib->name);
        fprintf(out_h, "#ifndef %s\n#define %s\n\n", guard, guard);
        fputs("#include <stdint.h>\n\n", out_h);
        for (size_t i = 0; i < lib->fn_count; i++) {
            char sym[CG_SYM_MAX];
            library_symbol(sym, sizeof(sym), lib, &lib->functions[i]);
            emit_fn_signature(out_h, forge_str(sym), &lib->functions[i]);
            fputs(";\n", out_h);
        }
        fputs("\n#endif\n", out_h);
        free(guard);
    }

    if (!out_c) return;

    fputs("// Generated by Forge compiler (library mode)\n", out_c);
    fprintf(out_c, "#include \"%s\"\n", runtime_include);
    fputs("#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n", out_c);
    for (size_t i = 0; i < lib->import_count; i++) {
        const char *hdr = forge_std_header(lib->imports[i]);
        if (hdr) fprintf(out_c, "#include \"%s\"\n", hdr);
        else fprintf(out_c, "#include \"%.*s.h\"\n", FSTR(lib->imports[i]));
    }
    emit_print_auto_macro(out_c);
    fputs("\n", out_c);

    Codegen cg = { .out = out_c, .prog = prog,
                   .imports = lib->imports, .import_count = lib->import_count };
    cg_build_symbols(&cg);
    for (size_t i = 0; i < lib->fn_count; i++) {
        char sym[CG_SYM_MAX];
        library_symbol(sym, sizeof(sym), lib, &lib->functions[i]);
        emit_fn_body(&cg, forge_str(sym), &lib->functions[i], forge_str(""));
    }
    cg_dispose(&cg);
}

/* --------------------------------------------------------- program mode --- */

static ProcessDecl *find_process(Program *prog, ForgeStr name) {
    for (size_t i = 0; i < prog->process_count; i++) {
        if (forge_str_eq(prog->processes[i].name, name)) return &prog->processes[i];
    }
    return NULL;
}

static NativeDecl *find_native(Program *prog, ForgeStr name) {
    for (size_t i = 0; i < prog->native_count; i++) {
        if (forge_str_eq(prog->natives[i].name, name)) return &prog->natives[i];
    }
    return NULL;
}

static void emit_program_preamble(Program *prog, FILE *out, const char *runtime_include) {
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
}

/* `extern` declarations for the externs, then forward declarations for
 * everything else, so definitions may appear in any order. */
static void emit_fn_prototypes(Program *prog, FILE *out) {
    for (size_t i = 0; i < prog->fn_count; i++) {
        FnDecl *fn = &prog->functions[i];
        if (!fn->is_extern) continue;
        fputs("extern ", out);
        emit_fn_signature(out, fn->name, fn);
        fputs(";\n", out);
    }
    fputs("\n", out);

    for (size_t i = 0; i < prog->fn_count; i++) {
        FnDecl *fn = &prog->functions[i];
        if (fn->is_extern) continue;
        fputs("static ", out);
        emit_fn_signature(out, fn->name, fn);
        fputs(";\n", out);
    }
    fputs("\n", out);
}

static void emit_all_fn_bodies(Codegen *cg, Program *prog) {
    for (size_t i = 0; i < prog->fn_count; i++) {
        FnDecl *fn = &prog->functions[i];
        if (fn->is_extern) continue;
        fputs("static ", cg->out);
        emit_fn_body(cg, fn->name, fn, forge_str(""));
    }

    for (size_t i = 0; i < prog->module_count; i++) {
        FileModule *mod = &prog->modules[i];
        for (size_t j = 0; j < mod->fn_count; j++) {
            FnDecl *fn = &mod->functions[j];
            char sym[CG_SYM_MAX];
            forge_mod_mangle(sym, sizeof(sym), mod->name, fn->name);
            fputs("static ", cg->out);
            emit_fn_body(cg, forge_str(sym), fn, mod->name);
        }
    }
    cg->current_module = forge_str("");
}

/* The per-process setup function that runs before the scheduler starts. */
static void emit_process_inits(Codegen *cg, Program *prog) {
    for (size_t i = 0; i < prog->process_count; i++) {
        ProcessDecl *pd = &prog->processes[i];
        if (!pd->body.first) continue;
        fprintf(cg->out, "static void %.*s_init(fr_process_t *proc) {\n", FSTR(pd->name));
        cg->indent = 1;
        cg_begin_function(cg);
        emit_stmts(cg, pd->body.first, "proc", false);
        cg->indent = 0;
        fputs("}\n\n", cg->out);
    }
}

/* main() for a program built out of processes and supervisors: create them,
 * wire up supervision, run their init blocks, then hand off to the scheduler. */
static void emit_scheduler_main(Codegen *cg, Program *prog) {
    FILE *out = cg->out;
    fputs("int main(int argc, char **argv) {\n", out);
    cg->indent = 1;
    cg_begin_function(cg);
    cg_line(cg, "fr_os_set_args(argc, argv);");
    cg_line(cg, "fr_scheduler_t *sched = fr_scheduler_create(0);");

    for (size_t i = 0; i < prog->process_count; i++) {
        ForgeStr n = prog->processes[i].name;
        cg_line(cg, "fr_process_t *proc_%.*s = fr_process_create(\"%.*s\");", FSTR(n), FSTR(n));
        cg_line(cg, "fr_scheduler_add_process(sched, proc_%.*s);", FSTR(n));
    }

    for (size_t i = 0; i < prog->supervisor_count; i++) {
        SupervisorDecl *sup = &prog->supervisors[i];
        const char *pol = "FR_RESTART_PROCESS";
        if (sup->policy == SUP_RESTART_CORO) pol = "FR_RESTART_CORO";
        else if (sup->policy == SUP_RESTART_ALL) pol = "FR_RESTART_ALL";
        cg_line(cg, "fr_process_t *sup_%.*s = fr_supervisor_create(\"%.*s\", %s);",
                FSTR(sup->name), FSTR(sup->name), pol);
        for (size_t j = 0; j < sup->child_count; j++) {
            ProcessDecl *child = find_process(prog, sup->children[j]);
            if (!child) continue;
            cg_line(cg, "fr_supervisor_add_child(sup_%.*s, proc_%.*s);",
                    FSTR(sup->name), FSTR(child->name));
        }
        cg_line(cg, "fr_scheduler_add_process(sched, sup_%.*s);", FSTR(sup->name));
    }

    for (size_t i = 0; i < prog->process_count; i++) {
        ProcessDecl *pd = &prog->processes[i];
        if (!pd->body.first) continue;
        cg_line(cg, "%.*s_init(proc_%.*s);", FSTR(pd->name), FSTR(pd->name));
    }

    cg_line(cg, "fr_scheduler_run(sched);");
    cg_line(cg, "fr_scheduler_destroy(sched);");
    cg_line(cg, "return 0;");
    cg->indent = 0;
    fputs("}\n", out);
}

void codegen_emit(Program *prog, FILE *out, const char *runtime_include) {
    Codegen cg = { .out = out, .prog = prog,
                   .imports = prog->imports, .import_count = prog->import_count };
    cg_build_symbols(&cg);

    emit_program_preamble(prog, out, runtime_include);
    emit_fn_prototypes(prog, out);

    for (size_t i = 0; i < prog->process_count; i++) {
        ProcessDecl *pd = &prog->processes[i];
        for (size_t j = 0; j < pd->coro_count; j++) emit_coro_fn(&cg, &pd->coros[j]);
    }

    emit_all_fn_bodies(&cg, prog);
    emit_process_inits(&cg, prog);

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
        cg_dispose(&cg);
        return;
    }

    /* Any process at all is enough to justify a scheduler main; `main` is only
     * preferred as the entry point when one exists. */
    bool has_entry = find_process(prog, entry_name) != NULL || prog->process_count > 0;
    if (!has_entry) fputs("int main(void) { return 0; }\n", out);
    else emit_scheduler_main(&cg, prog);
    cg_dispose(&cg);
}
