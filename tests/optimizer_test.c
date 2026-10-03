#include "ast.h"
#include "optimize.h"
#include <math.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static Program constant(Expr *e) {
    Program p = {0};
    p.const_count = 1;
    p.consts = calloc(1, sizeof(*p.consts));
    CHECK(p.consts != NULL);
    p.consts[0].value = e;
    return p;
}

static void int_case(BinOp op, int64_t a, int64_t b, bool folds, int64_t result) {
    Expr *node = expr_binary(op, expr_int(a), expr_int(b));
    Program p = constant(node);
    optimize_program(&p);
    Expr *e = p.consts[0].value;
    if (folds) { CHECK(e->kind == EXPR_INT); CHECK(e->as.int_val == result); }
    else { CHECK(e == node); CHECK(e->kind == EXPR_BINARY); }
    program_free(&p);
}

int main(void) {
    int_case(BIN_ADD, 2, 3, true, 5);
    int_case(BIN_ADD, INT64_MAX, 1, false, 0);
    int_case(BIN_ADD, INT64_MIN, -1, false, 0);
    int_case(BIN_ADD, INT64_MIN, INT64_MAX, true, -1);
    int_case(BIN_SUB, INT64_MIN, 1, false, 0);
    int_case(BIN_SUB, INT64_MAX, -1, false, 0);
    int_case(BIN_SUB, -1, INT64_MIN, true, INT64_MAX);
    int_case(BIN_SUB, 0, INT64_MIN, false, 0);
    int_case(BIN_MUL, INT64_MAX, 2, false, 0);
    int_case(BIN_MUL, INT64_MIN, -1, false, 0);
    int_case(BIN_MUL, -1, INT64_MIN, false, 0);
    int_case(BIN_MUL, INT64_MIN, 2, false, 0);
    int_case(BIN_MUL, -3037000500LL, -3037000500LL, false, 0);
    int_case(BIN_MUL, -3037000499LL, -3037000499LL, true, 9223372030926249001LL);
    int_case(BIN_MUL, INT64_MIN, 0, true, 0);
    int_case(BIN_DIV, INT64_MIN, -1, false, 0);
    int_case(BIN_MOD, INT64_MIN, -1, false, 0);
    int_case(BIN_DIV, 3, 0, false, 0);
    int_case(BIN_MOD, 3, 0, false, 0);
    int_case(BIN_DIV, -7, 3, true, -2);
    int_case(BIN_MOD, -7, 3, true, -1);

    Program p = constant(expr_binary(BIN_MUL, expr_ident(forge_str("x")), expr_int(0)));
    optimize_program(&p);
    CHECK(p.consts[0].value->kind == EXPR_BINARY);
    program_free(&p);

    p = constant(expr_binary(BIN_MUL, expr_int(0), expr_call(forge_str("side_effect"), NULL, 0)));
    optimize_program(&p);
    CHECK(p.consts[0].value->kind == EXPR_BINARY);
    CHECK(p.consts[0].value->as.binary.right->kind == EXPR_CALL);
    program_free(&p);

    p = constant(expr_binary(BIN_ADD,
        expr_binary(BIN_MUL, expr_int(0), expr_ident(forge_str("unknown_float"))), expr_int(0)));
    CHECK(p.consts[0].value->type.kind == TY_VOID);
    optimize_program(&p);
    CHECK(p.consts[0].value->kind == EXPR_BINARY);
    CHECK(p.consts[0].value->as.binary.op == BIN_ADD);
    program_free(&p);

    p = constant(expr_binary(BIN_ADD, expr_float(-0.0), expr_int(0)));
    optimize_program(&p);
    CHECK(p.consts[0].value->kind == EXPR_BINARY);
    program_free(&p);

    p = constant(expr_binary(BIN_EQ, expr_int(3), expr_int(3)));
    CHECK(p.consts[0].value->type.kind == TY_BOOL);
    optimize_program(&p);
    CHECK(p.consts[0].value->kind == EXPR_BOOL);
    CHECK(p.consts[0].value->as.bool_val);
    program_free(&p);

    p = constant(expr_move(expr_binary(BIN_ADD, expr_int(2), expr_int(3))));
    optimize_program(&p);
    CHECK(p.consts[0].value->as.move_expr->kind == EXPR_INT);
    CHECK(p.consts[0].value->as.move_expr->as.int_val == 5);
    program_free(&p);

    Expr *base = expr_ident(forge_str("item"));
    base->type = forge_type_struct(forge_str("Item"));
    p = constant(expr_field(base, forge_str("value")));
    p.consts[0].value->type = forge_type_int();
    optimize_program(&p);
    CHECK(p.consts[0].value->type.kind == TY_INT);
    program_free(&p);
    puts("optimizer regression tests passed");
    return 0;
}
