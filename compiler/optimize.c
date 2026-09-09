#include "optimize.h"

/* `fold_binary` and `simplify_binary` both return the node that should replace
 * the binary expression, or NULL when there is nothing to replace it with. The
 * caller keeps the original node in that case, so a program full of
 * non-foldable arithmetic costs no allocations at all. A non-NULL result has
 * taken ownership of whichever operands it did not return. */

static Expr *fold_int_binary(BinOp op, Expr *l, Expr *r) {
    int64_t a = l->as.int_val, b = r->as.int_val, v = 0;
    switch (op) {
    case BIN_ADD:
        if (__builtin_add_overflow(a, b, &v)) return NULL;
        break;
    case BIN_SUB:
        if (__builtin_sub_overflow(a, b, &v)) return NULL;
        break;
    case BIN_MUL:
        if (__builtin_mul_overflow(a, b, &v)) return NULL;
        break;
    case BIN_DIV:
        /* INT64_MIN / -1 overflows and traps (SIGFPE) on the divide
         * instruction; decline to fold just like the div-by-zero case. */
        if (b == 0 || (a == INT64_MIN && b == -1)) return NULL;
        v = a / b;
        break;
    case BIN_MOD:
        /* INT64_MIN % -1 traps for the same reason as the division above
         * (the CPU computes both via the same instruction). */
        if (b == 0 || (a == INT64_MIN && b == -1)) return NULL;
        v = a % b;
        break;
    case BIN_EQ: return expr_bool(a == b);
    case BIN_NE: return expr_bool(a != b);
    case BIN_LT: return expr_bool(a < b);
    case BIN_LE: return expr_bool(a <= b);
    case BIN_GT: return expr_bool(a > b);
    case BIN_GE: return expr_bool(a >= b);
    default: return NULL;
    }
    return expr_int(v);
}

static Expr *fold_float_binary(BinOp op, Expr *l, Expr *r) {
    double a = l->as.float_val, b = r->as.float_val, v = 0;
    switch (op) {
    case BIN_ADD: v = a + b; break;
    case BIN_SUB: v = a - b; break;
    case BIN_MUL: v = a * b; break;
    case BIN_DIV:
        if (b == 0.0) return NULL;
        v = a / b;
        break;
    default: return NULL;
    }
    return expr_float(v);
}

static Expr *fold_bool_binary(BinOp op, Expr *l, Expr *r) {
    bool a = l->as.bool_val, b = r->as.bool_val;
    switch (op) {
    case BIN_EQ: return expr_bool(a == b);
    case BIN_NE: return expr_bool(a != b);
    case BIN_AND: return expr_bool(a && b);
    case BIN_OR: return expr_bool(a || b);
    default: return NULL;
    }
}

static Expr *fold_binary(BinOp op, Expr *l, Expr *r) {
    Expr *folded = NULL;
    if (l->kind == EXPR_INT && r->kind == EXPR_INT)
        folded = fold_int_binary(op, l, r);
    else if (l->kind == EXPR_FLOAT && r->kind == EXPR_FLOAT)
        folded = fold_float_binary(op, l, r);
    else if (l->kind == EXPR_BOOL && r->kind == EXPR_BOOL)
        folded = fold_bool_binary(op, l, r);
    if (!folded) return NULL;
    /* Both operands were literals, so neither owns any children to leak. */
    free(l);
    free(r);
    return folded;
}

/* Whether dropping this operand would lose an observable effect. Literals and
 * plain identifiers are safe to discard; a call is not. */
static bool expr_is_effect_free(const Expr *e) {
    switch (e->kind) {
    case EXPR_INT:
    case EXPR_FLOAT:
    case EXPR_BOOL:
    case EXPR_STRING:
    case EXPR_IDENT:
        return true;
    default:
        return false;
    }
}

static bool expr_is_int(const Expr *e, int64_t v) {
    return e->kind == EXPR_INT && e->as.int_val == v;
}

static Expr *simplify_binary(BinOp op, Expr *l, Expr *r) {
    switch (op) {
    case BIN_ADD:
        if (expr_is_int(l, 0)) { free(l); return r; }
        if (expr_is_int(r, 0)) { free(r); return l; }
        break;
    case BIN_SUB:
        if (expr_is_int(r, 0)) { free(r); return l; }
        break;
    case BIN_MUL:
        /* x * 0 == 0 and 0 * x == 0, but only when the surviving operand is
         * side-effect free -- otherwise dropping it would silently discard
         * e.g. a call. */
        if (expr_is_int(l, 0)) {
            if (!expr_is_effect_free(r)) return NULL;
            free(r);
            return l;
        }
        if (expr_is_int(r, 0)) {
            if (!expr_is_effect_free(l)) return NULL;
            free(l);
            return r;
        }
        if (expr_is_int(l, 1)) { free(l); return r; }
        if (expr_is_int(r, 1)) { free(r); return l; }
        break;
    default:
        break;
    }
    return fold_binary(op, l, r);
}

static Expr *optimize_expr(Expr *e) {
    if (!e) return NULL;
    switch (e->kind) {
    case EXPR_BINARY: {
        e->as.binary.left = optimize_expr(e->as.binary.left);
        e->as.binary.right = optimize_expr(e->as.binary.right);
        Expr *folded = simplify_binary(e->as.binary.op, e->as.binary.left, e->as.binary.right);
        if (folded) {
            free(e);
            return folded;
        }
        /* Kept in place, but an operand may have changed shape underneath us,
         * so re-derive the result type the same way expr_binary would. */
        switch (e->as.binary.op) {
        case BIN_EQ: case BIN_NE: case BIN_LT: case BIN_LE:
        case BIN_GT: case BIN_GE:
            e->type = forge_type_bool();
            break;
        default:
            e->type = e->as.binary.left->type;
            break;
        }
        return e;
    }
    case EXPR_MOVE:
        e->as.move_expr = optimize_expr(e->as.move_expr);
        return e;
    case EXPR_CALL:
        for (size_t i = 0; i < e->as.call.arg_count; i++)
            e->as.call.args[i] = optimize_expr(e->as.call.args[i]);
        return e;
    case EXPR_QUAL_CALL:
        for (size_t i = 0; i < e->as.qual_call.arg_count; i++)
            e->as.qual_call.args[i] = optimize_expr(e->as.qual_call.args[i]);
        return e;
    case EXPR_INDEX:
        e->as.index.base = optimize_expr(e->as.index.base);
        e->as.index.index = optimize_expr(e->as.index.index);
        return e;
    case EXPR_FIELD:
        e->as.field.base = optimize_expr(e->as.field.base);
        e->type = e->as.field.base->type;
        return e;
    default:
        return e;
    }
}

static void optimize_block(Block *b);

/* Optimize a bare statement list (a `for` header's init/step clauses, which are
 * lists rather than Blocks). Wrapping the list in a correctly-terminated Block
 * and writing the head back means a statement the pass splices out at the front
 * actually leaves the list, instead of being dropped into a temporary. */
static void optimize_stmt_list(Stmt **head) {
    if (!*head) return;
    Stmt *last = *head;
    while (last->next) last = last->next;
    Block b = { *head, last };
    optimize_block(&b);
    *head = b.first;
}

static void optimize_block(Block *b) {
    if (!b) return;
    Stmt *prev = NULL;
    Stmt *s = b->first;
    while (s) {
        Stmt *next = s->next;
        bool spliced = false;
        switch (s->kind) {
        case STMT_LET:
            s->as.let.init = optimize_expr(s->as.let.init);
            break;
        case STMT_EXPR:
            s->as.expr = optimize_expr(s->as.expr);
            break;
        case STMT_RETURN:
            s->as.ret = optimize_expr(s->as.ret);
            break;
        case STMT_IF: {
            s->as.if_stmt.cond = optimize_expr(s->as.if_stmt.cond);
            optimize_block(s->as.if_stmt.then_br);
            optimize_block(s->as.if_stmt.else_br);
            if (s->as.if_stmt.cond->kind == EXPR_BOOL) {
                /* Condition is compile-time known: splice the taken branch
                 * in place of the whole if statement, dropping the other
                 * branch entirely instead of emitting both. */
                Block *taken = s->as.if_stmt.cond->as.bool_val
                    ? s->as.if_stmt.then_br : s->as.if_stmt.else_br;
                Stmt *repl_first = taken ? taken->first : NULL;
                Stmt *repl_last = taken ? taken->last : NULL;
                if (repl_first) repl_last->next = next;
                if (prev) prev->next = repl_first ? repl_first : next;
                else b->first = repl_first ? repl_first : next;
                if (s == b->last) b->last = repl_last ? repl_last : prev;
                if (repl_last) prev = repl_last;
                spliced = true;
            }
            break;
        }
        case STMT_WHILE:
            s->as.while_stmt.cond = optimize_expr(s->as.while_stmt.cond);
            optimize_block(s->as.while_stmt.body);
            if (s->as.while_stmt.cond->kind == EXPR_BOOL &&
                !s->as.while_stmt.cond->as.bool_val) {
                /* Condition is compile-time false: the loop body never
                 * runs, so drop the whole while statement. */
                if (prev) prev->next = next;
                else b->first = next;
                if (s == b->last) b->last = prev;
                spliced = true;
            }
            break;
        case STMT_FOR:
            optimize_stmt_list(&s->as.for_stmt.init);
            s->as.for_stmt.cond = optimize_expr(s->as.for_stmt.cond);
            optimize_stmt_list(&s->as.for_stmt.step);
            optimize_block(s->as.for_stmt.body);
            break;
        case STMT_SPAWN:
            for (size_t i = 0; i < s->as.spawn.arg_count; i++)
                s->as.spawn.args[i] = optimize_expr(s->as.spawn.args[i]);
            break;
        case STMT_SEND:
            s->as.send.target = optimize_expr(s->as.send.target);
            s->as.send.value = optimize_expr(s->as.send.value);
            break;
        case STMT_ASSIGN:
            s->as.assign.value = optimize_expr(s->as.assign.value);
            break;
        case STMT_BLOCK:
            optimize_block(s->as.block);
            break;
        case STMT_MATCH: {
            s->as.match_stmt.scrutinee = optimize_expr(s->as.match_stmt.scrutinee);
            for (MatchArm *arm = s->as.match_stmt.arms; arm; arm = arm->next)
                optimize_block(arm->body);
            break;
        }
        case STMT_AWAIT:
            s->as.await_expr = optimize_expr(s->as.await_expr);
            break;
        default:
            break;
        }
        if (!spliced) prev = s;
        s = next;
    }
}

void optimize_program(Program *prog) {
    for (size_t i = 0; i < prog->const_count; i++) {
        if (prog->consts[i].value)
            prog->consts[i].value = optimize_expr(prog->consts[i].value);
    }
    for (size_t i = 0; i < prog->fn_count; i++)
        if (!prog->functions[i].is_extern) optimize_block(&prog->functions[i].body);
    for (size_t i = 0; i < prog->native_count; i++)
        optimize_block(&prog->natives[i].body);
    for (size_t i = 0; i < prog->process_count; i++) {
        ProcessDecl *pd = &prog->processes[i];
        optimize_block(&pd->body);
        for (size_t j = 0; j < pd->coro_count; j++)
            optimize_block(&pd->coros[j].body);
        optimize_block(pd->on_receive);
    }
    if (prog->library.present) {
        for (size_t i = 0; i < prog->library.fn_count; i++)
            optimize_block(&prog->library.functions[i].body);
    }
    for (size_t i = 0; i < prog->module_count; i++) {
        for (size_t j = 0; j < prog->modules[i].fn_count; j++)
            optimize_block(&prog->modules[i].functions[j].body);
    }
}
