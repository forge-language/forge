#include "parser.h"
#include "semantic.h"
#include "optimize.h"
#include "typed_lookup.h"
#include <assert.h>

int main(void) {
    const char *source = "const BASE=1;const VALUE=BASE;native main{"
        "let x:int=VALUE;{let BASE:string=\"text\";let y:string=VALUE;}return 0;}";
    Lexer lexer;
    lexer_init(&lexer, source, strlen(source));
    Program p = parse_program_named(&lexer, "lookup.fg");
    forge_check_program(&p);
    optimize_program(&p);
    ForgeTypedProgram typed = forge_type_program(&p);
    assert(forge_typed_verify(&typed));
    ForgeTypedIndex index = forge_typed_index(&typed);
    size_t integer = 0, string = 0;
    for (size_t i = 0; i < typed.expression_count; ++i) {
        const ForgeTypedExpr *e = &typed.expressions[i];
        if (e->constant != &p.consts[1]) continue;
        size_t node = forge_typed_find(&index, p.consts[1].value, i);
        assert(node != FORGE_TYPED_NONE && typed.expressions[node].parent == i);
        assert(typed.expressions[node].type.value.kind == e->type.value.kind);
        if (e->type.value.kind == TY_INT) ++integer;
        if (e->type.value.kind == TY_STRING) ++string;
        assert(forge_typed_find(&index, e->origin, i) == i);
    }
    assert(integer == 1 && string == 1);
    Expr absent = {.kind = EXPR_INT};
    assert(forge_typed_find(&index, &absent, FORGE_TYPED_NONE) == FORGE_TYPED_NONE);
    assert(forge_typed_find(&index, NULL, FORGE_TYPED_NONE) == FORGE_TYPED_NONE);
    forge_typed_index_free(&index);
    forge_typed_free(&typed);
    program_free(&p);
    puts("typed lookup preserves per-use constant interpretations");
    return 0;
}
