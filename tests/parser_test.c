#include "parser.h"
#include <stdarg.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static char source[100000];
static size_t used;
static void append(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(source + used, sizeof(source) - used, format, args);
    va_end(args);
    CHECK(n >= 0 && (size_t)n < sizeof(source) - used);
    used += (size_t)n;
}
static void check_name(ForgeStr name, const char *prefix, size_t index) {
    char expected[64];
    snprintf(expected, sizeof(expected), "%s%zu", prefix, index);
    CHECK(forge_str_eq(name, forge_str(expected)));
}
int main(void) {
    /* Cross every geometric capacity boundary up to 64, using independent
     * lists and interleaved external and normal functions in one list. */
    const size_t count = 65;
    for (size_t i = 0; i < count; ++i) {
        append("import module%zu; import \"path%zu.fg\";\n", i, i);
        append("struct S%zu { value: int; } enum E%zu { One; }\n", i, i);
        append("native N%zu {} extern fn ext%zu(); fn fn%zu() {}\n", i, i, i);
        append("process P%zu {} supervisor Sup%zu { P%zu; }\n", i, i, i);
        append("const c%zu = %zu;\n", i, i);
    }
    append("supervisor Group { restart: all; ");
    for (size_t i = 0; i < count; ++i) append("P%zu; ", i);
    append("}\nlibrary L {\n");
    for (size_t i = 0; i < count; ++i)
        append("import module%zu; export fn exported%zu() {}\n", i, i);
    append("}\n");
    Lexer lx;
    lexer_init(&lx, source, used);
    Program p = parse_program(&lx);
    CHECK(p.import_count == count && p.path_import_count == count);
    CHECK(p.struct_count == count && p.enum_count == count);
    CHECK(p.native_count == count && p.fn_count == count * 2);
    CHECK(p.process_count == count && p.supervisor_count == count + 1);
    CHECK(p.const_count == count);
    CHECK(p.library.present && p.library.import_count == count && p.library.fn_count == count);
    CHECK(p.supervisors[count].child_count == count);
    for (size_t i = 0; i < count; ++i) {
        check_name(p.imports[i], "module", i);
        char path[64]; snprintf(path, sizeof(path), "path%zu.fg", i);
        CHECK(forge_str_eq(p.path_imports[i], forge_str(path)));
        check_name(p.structs[i].name, "S", i);
        CHECK(p.structs[i].fields && p.structs[i].fields->type.kind == TY_INT);
        check_name(p.enums[i].name, "E", i);
        CHECK(p.enums[i].variants && p.enums[i].variants->value == 0);
        check_name(p.natives[i].name, "N", i);
        check_name(p.functions[i * 2].name, "ext", i);
        check_name(p.functions[i * 2 + 1].name, "fn", i);
        CHECK(p.functions[i * 2].is_extern && !p.functions[i * 2 + 1].is_extern);
        check_name(p.processes[i].name, "P", i);
        check_name(p.supervisors[i].name, "Sup", i);
        CHECK(p.supervisors[i].child_count == 1);
        check_name(p.supervisors[i].children[0], "P", i);
        check_name(p.consts[i].name, "c", i);
        CHECK(p.consts[i].value->kind == EXPR_INT && p.consts[i].value->as.int_val == (int64_t)i);
        check_name(p.library.imports[i], "module", i);
        check_name(p.library.functions[i].name, "exported", i);
        check_name(p.supervisors[count].children[i], "P", i);
    }
    CHECK(p.supervisors[count].policy == SUP_RESTART_ALL);
    program_free(&p);
    lexer_init(&lx, "", 0);
    p = parse_program(&lx);
    CHECK(p.fn_count == 0 && p.functions == NULL && p.consts == NULL);
    program_free(&p);
    puts("parser declaration growth tests passed");
    return 0;
}
