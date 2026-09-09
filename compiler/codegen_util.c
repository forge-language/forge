/* Output primitives: indentation, ForgeStr emission, C type spellings and C
 * string literals. Everything here writes to a FILE* and knows nothing about
 * the AST beyond types and names. */
#include "codegen_internal.h"
#include <stdarg.h>

void cg_indent(Codegen *cg) {
    for (int i = 0; i < cg->indent; i++) fputs("    ", cg->out);
}

void cg_line(Codegen *cg, const char *fmt, ...) {
    cg_indent(cg);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(cg->out, fmt, ap);
    va_end(ap);
    fputc('\n', cg->out);
}

void emit_str(FILE *out, ForgeStr s) {
    if (s.len) fwrite(s.data, 1, s.len, out);
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

void c_type_name(FILE *out, ForgeType ty) {
    if (ty.kind == TY_STRUCT) emit_str(out, ty.struct_name);
    else fputs(c_type(ty), out);
}

/* Emit `s` as a C string literal, escaping everything the C compiler could
 * misread. Shared by expression strings and const declarations so both paths
 * stay in sync. */
void emit_c_string_literal(FILE *out, ForgeStr s) {
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

/* `name` may be empty, which emits a bare function type (return type plus
 * parameter list) rather than a declarator. */
void emit_fn_signature(FILE *out, ForgeStr name, const FnDecl *fn) {
    c_type_name(out, fn->ret_type);
    if (name.len) {
        fputc(' ', out);
        emit_str(out, name);
    }
    fputc('(', out);
    for (Param *p = fn->params; p; p = p->next) {
        if (p != fn->params) fputs(", ", out);
        c_type_name(out, p->type);
        fputc(' ', out);
        emit_str(out, p->name);
    }
    fputc(')', out);
}
