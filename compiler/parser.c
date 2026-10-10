#include "parser.h"

#define FORGE_MAX_EXPR_DEPTH 2000

typedef struct {
    Lexer *lx;
    SourceFile *source;
    int expr_depth;
} Parser;

static SourceSpan token_span(Parser *p, Token t) {
    return (SourceSpan){p->source, t.start, t.end};
}
static SourceSpan parsed_span(Parser *p, Token start) {
    return (SourceSpan){p->source, start.start, p->lx->last.end};
}
static void parser_error(Parser *p, const char *msg) {
    Token t = lexer_peek(p->lx);
    char legacy[512];
    snprintf(legacy, sizeof(legacy), "forge: parse error at %d:%d: %s", t.line, t.col, msg);
    forge_source_diagnostic(token_span(p,t), "parse", msg, legacy);
    exit(1);
}

/* Parser-local capacities keep the public AST layout unchanged. Geometric
 * growth bounds reallocations and copying as declaration lists get larger. */
static void *reserve_declarations(void *items, size_t count, size_t *capacity,
                                  size_t item_size) {
    if (count < *capacity) return items;
    size_t max_count = SIZE_MAX / item_size;
    if (count >= max_count) forge_die("too many declarations");
    size_t next = *capacity ? *capacity : 8;
    if (next <= count) next = next <= max_count / 2 ? next * 2 : max_count;
    if (next > max_count) next = max_count;
    void *grown = realloc(items, next * item_size);
    if (!grown) forge_die("out of memory growing declarations");
    *capacity = next;
    return grown;
}

static void expect(Parser *p, TokenKind kind) {
    if (!lexer_match(p->lx, kind)) {
        parser_error(p, "unexpected token");
    }
}

static ForgeStr token_str(Token t) {
    return t.lexeme;
}

static ForgeType parse_type(Parser *p) {
    Token t = lexer_peek(p->lx);
    switch (t.kind) {
    case TOK_KW_INT: lexer_next(p->lx); return forge_type_int();
    case TOK_KW_FLOAT: lexer_next(p->lx); return forge_type_float();
    case TOK_KW_BOOL: lexer_next(p->lx); return forge_type_bool();
    case TOK_KW_STRING: lexer_next(p->lx); return forge_type_string();
    case TOK_KW_VOID: lexer_next(p->lx); return forge_type_void();
    case TOK_KW_PTR: lexer_next(p->lx); return forge_type_ptr();
    case TOK_IDENT:
        lexer_next(p->lx);
        return forge_type_struct(token_str(t));
    default:
        parser_error(p, "expected type");
        return forge_type_void();
    }
}

static Expr *parse_expr(Parser *p);
static Expr *parse_primary(Parser *p);
static Expr *parse_unary(Parser *p);
static Expr *parse_mul(Parser *p);
static Expr *parse_add(Parser *p);
static Expr *parse_cmp(Parser *p);
static Expr *parse_and(Parser *p);
static Expr *parse_or(Parser *p);
static Block *parse_block(Parser *p);
static Stmt *parse_stmt(Parser *p);

static Stmt *parse_assign(Parser *p, Token name_token) {
    ForgeStr name = token_str(name_token);
    if (lexer_match(p->lx, TOK_EQ)) {
        Stmt *s = stmt_assign(name, parse_expr(p));
        s->span = parsed_span(p,name_token); s->focus = token_span(p,name_token); return s;
    }
    Token t = lexer_peek(p->lx);
    TokenKind k = lexer_peek(p->lx).kind;
    BinOp op;
    switch (k) {
    case TOK_PLUSEQ: op = BIN_ADD; break;
    case TOK_MINUSEQ: op = BIN_SUB; break;
    case TOK_STAREQ: op = BIN_MUL; break;
    case TOK_SLASHEQ: op = BIN_DIV; break;
    case TOK_PERCENTEQ: op = BIN_MOD; break;
    default:
        parser_error(p, "expected assignment operator");
        return NULL;
    }
    lexer_next(p->lx);
    Expr *rhs = parse_expr(p);
    Expr *left = expr_ident(name);
    left->span = left->focus = token_span(p,name_token);
    Expr *value = expr_binary(op,left,rhs); value->focus = token_span(p,t);
    Stmt *s = stmt_assign(name,value);
    s->span = parsed_span(p,name_token); s->focus = token_span(p,name_token); return s;
}

static Expr *parse_postfix(Parser *p, Expr *e) {
    for (;;) {
        if (lexer_match(p->lx, TOK_DOT)) {
            Token field = lexer_peek(p->lx);
            expect(p, TOK_IDENT);
            SourceSpan base = e->span;
            e = expr_field(e, token_str(field));
            e->span = (SourceSpan){base.file, base.start, p->lx->last.end};
            e->focus = token_span(p,field);
        } else if (lexer_match(p->lx, TOK_LBRACKET)) {
            Expr *idx = parse_expr(p);
            expect(p, TOK_RBRACKET);
            SourceSpan base = e->span;
            e = expr_index(e, idx);
            e->span = (SourceSpan){base.file, base.start, p->lx->last.end};
            e->focus = idx->span;
        } else {
            break;
        }
    }
    return e;
}

/* Parses whatever follows an already-consumed identifier token as the start
 * of an expression: a qualified call/field access (a.b / a.b(...)), a direct
 * call (a(...)), or a bare identifier. Shared by parse_primary and by
 * parse_stmt's expression-statement path so both see identical grammar. */
static Expr *parse_ident_start_inner(Parser *p, ForgeStr name);
static Expr *parse_ident_start(Parser *p, Token token) {
    Expr *e = parse_ident_start_inner(p, token_str(token));
    e->span = parsed_span(p,token);
    if (!e->focus.file) e->focus = token_span(p,token);
    if (e->kind == EXPR_FIELD && e->as.field.base) {
        e->as.field.base->span = e->as.field.base->focus = token_span(p,token);
    }
    return e;
}
static Expr *parse_ident_start_inner(Parser *p, ForgeStr name) {
    if (lexer_match(p->lx, TOK_DOT)) {
        Token fn = lexer_peek(p->lx);
        expect(p, TOK_IDENT);
        if (lexer_match(p->lx, TOK_LPAREN)) {
            Expr **args = NULL;
            size_t n = 0, cap = 0;
            if (!lexer_match(p->lx, TOK_RPAREN)) {
                do {
                    if (n == cap) {
                        cap = cap ? cap * 2 : 4;
                        args = (Expr **)realloc(args, cap * sizeof(Expr *));
                    }
                    args[n++] = parse_expr(p);
                } while (lexer_match(p->lx, TOK_COMMA));
                expect(p, TOK_RPAREN);
            }
            Expr *e = expr_qual_call(name, token_str(fn), args, n);
            e->focus = token_span(p,fn); return e;
        }
        Expr *e = expr_field(expr_ident(name), token_str(fn));
        e->focus = token_span(p,fn); return e;
    }
    if (lexer_match(p->lx, TOK_LPAREN)) {
        Expr **args = NULL;
        size_t n = 0, cap = 0;
        if (!lexer_match(p->lx, TOK_RPAREN)) {
            do {
                if (n == cap) {
                    cap = cap ? cap * 2 : 4;
                    args = (Expr **)realloc(args, cap * sizeof(Expr *));
                }
                args[n++] = parse_expr(p);
            } while (lexer_match(p->lx, TOK_COMMA));
            expect(p, TOK_RPAREN);
        }
        return expr_call(name, args, n);
    }
    return expr_ident(name);
}

static Expr *parse_primary_inner(Parser *p);
static Expr *parse_primary(Parser *p) {
    Token start = lexer_peek(p->lx);
    Expr *e = parse_primary_inner(p);
    if (!e->focus.file) e->focus = token_span(p,start);
    e->span = parsed_span(p,start);
    return e;
}
static Expr *parse_primary_inner(Parser *p) {
    Token t = lexer_peek(p->lx);
    switch (t.kind) {
    case TOK_INT:
        lexer_next(p->lx);
        return expr_int(t.int_val);
    case TOK_FLOAT:
        lexer_next(p->lx);
        return expr_float(t.float_val);
    case TOK_KW_TRUE:
        lexer_next(p->lx);
        return expr_bool(true);
    case TOK_KW_FALSE:
        lexer_next(p->lx);
        return expr_bool(false);
    case TOK_STRING:
        lexer_next(p->lx);
        return expr_string(token_str(t));
    case TOK_IDENT: {
        lexer_next(p->lx);
        return parse_ident_start(p, t);
    }
    case TOK_KW_RECV:
        lexer_next(p->lx);
        expect(p, TOK_LPAREN);
        expect(p, TOK_RPAREN);
        return expr_recv();
    case TOK_LPAREN: {
        lexer_next(p->lx);
        Expr *e = parse_expr(p);
        expect(p, TOK_RPAREN);
        return e;
    }
    default:
        parser_error(p, "expected expression");
        return expr_int(0);
    }
}

static Expr *parse_unary(Parser *p) {
    if (++p->expr_depth > FORGE_MAX_EXPR_DEPTH) {
        forge_die("expression nesting too deep");
    }
    Token start = lexer_peek(p->lx);
    Expr *e;
    if (lexer_match(p->lx, TOK_KW_MOVE)) {
        e = parse_postfix(p, expr_move(parse_unary(p)));
    } else if (lexer_match(p->lx, TOK_MINUS)) {
        e = parse_postfix(p, expr_binary(BIN_SUB, expr_int(0), parse_unary(p)));
    } else if (lexer_match(p->lx, TOK_BANG)) {
        e = parse_postfix(p, expr_binary(BIN_EQ, parse_unary(p), expr_bool(false)));
    } else {
        e = parse_postfix(p, parse_primary(p));
    }
    if (start.kind == TOK_KW_MOVE || start.kind == TOK_MINUS || start.kind == TOK_BANG) {
        e->span = parsed_span(p,start); e->focus = token_span(p,start);
    }
    p->expr_depth--;
    return e;
}

/* Each "_from" variant continues precedence-climbing from an already-parsed
 * left operand. This lets callers that had to hand-parse a leading operand
 * (e.g. parse_stmt's ident-led expression-statements) rejoin the normal
 * expression grammar instead of re-implementing a subset of it. */
static Expr *parse_binary(Parser *p, Token token, BinOp op, Expr *left, Expr *right) {
    Expr *e = expr_binary(op,left,right);
    e->focus = token_span(p,token);
    return e;
}

static Expr *parse_mul_from(Parser *p, Expr *left) {
    for (;;) {
        Token token = lexer_peek(p->lx);
        if (lexer_match(p->lx, TOK_STAR)) left = parse_binary(p, token, BIN_MUL, left, parse_unary(p));
        else if (lexer_match(p->lx, TOK_SLASH)) left = parse_binary(p, token, BIN_DIV, left, parse_unary(p));
        else if (lexer_match(p->lx, TOK_PERCENT)) left = parse_binary(p, token, BIN_MOD, left, parse_unary(p));
        else break;
    }
    return left;
}

static Expr *parse_mul(Parser *p) {
    return parse_mul_from(p, parse_unary(p));
}

static Expr *parse_add_from(Parser *p, Expr *left) {
    left = parse_mul_from(p, left);
    for (;;) {
        Token token = lexer_peek(p->lx);
        if (lexer_match(p->lx, TOK_PLUS)) left = parse_binary(p, token, BIN_ADD, left, parse_mul(p));
        else if (lexer_match(p->lx, TOK_MINUS)) left = parse_binary(p, token, BIN_SUB, left, parse_mul(p));
        else break;
    }
    return left;
}

static Expr *parse_add(Parser *p) {
    return parse_add_from(p, parse_mul(p));
}

static Expr *parse_cmp_from(Parser *p, Expr *left) {
    left = parse_add_from(p, left);
    for (;;) {
        Token token = lexer_peek(p->lx);
        if (lexer_match(p->lx, TOK_EQEQ)) left = parse_binary(p, token, BIN_EQ, left, parse_add(p));
        else if (lexer_match(p->lx, TOK_NE)) left = parse_binary(p, token, BIN_NE, left, parse_add(p));
        else if (lexer_match(p->lx, TOK_LT)) left = parse_binary(p, token, BIN_LT, left, parse_add(p));
        else if (lexer_match(p->lx, TOK_LE)) left = parse_binary(p, token, BIN_LE, left, parse_add(p));
        else if (lexer_match(p->lx, TOK_GT)) left = parse_binary(p, token, BIN_GT, left, parse_add(p));
        else if (lexer_match(p->lx, TOK_GE)) left = parse_binary(p, token, BIN_GE, left, parse_add(p));
        else break;
    }
    return left;
}

static Expr *parse_cmp(Parser *p) {
    return parse_cmp_from(p, parse_add(p));
}

static Expr *parse_and_from(Parser *p, Expr *left) {
    left = parse_cmp_from(p, left);
    while (lexer_peek(p->lx).kind == TOK_ANDAND) {
        Token token = lexer_next(p->lx);
        left = parse_binary(p, token, BIN_AND, left, parse_cmp(p));
    }
    return left;
}

static Expr *parse_and(Parser *p) {
    return parse_and_from(p, parse_cmp(p));
}

static Expr *parse_or_from(Parser *p, Expr *left) {
    left = parse_and_from(p, left);
    while (lexer_peek(p->lx).kind == TOK_OROR) {
        Token token = lexer_next(p->lx);
        left = parse_binary(p, token, BIN_OR, left, parse_and(p));
    }
    return left;
}

static Expr *parse_or(Parser *p) {
    return parse_or_from(p, parse_and(p));
}

static Expr *pipe_rhs_to_call(Expr *left, Expr *right, Parser *p) {
    if (right->kind == EXPR_CALL) {
        size_t n = right->as.call.arg_count + 1;
        Expr **args = (Expr **)calloc(n, sizeof(Expr *));
        if (!args) forge_die("out of memory");
        args[0] = left;
        for (size_t i = 1; i < n; i++) args[i] = right->as.call.args[i - 1];
        free(right->as.call.args);
        right->as.call.args = args;
        right->as.call.arg_count = n;
        right->span.start = left->span.start;
        return right;
    }
    if (right->kind == EXPR_QUAL_CALL) {
        size_t n = right->as.qual_call.arg_count + 1;
        Expr **args = (Expr **)calloc(n, sizeof(Expr *));
        if (!args) forge_die("out of memory");
        args[0] = left;
        for (size_t i = 1; i < n; i++) args[i] = right->as.qual_call.args[i - 1];
        free(right->as.qual_call.args);
        right->as.qual_call.args = args;
        right->as.qual_call.arg_count = n;
        right->span.start = left->span.start;
        return right;
    }
    if (right->kind == EXPR_IDENT) {
        Expr *e = expr_call(right->as.ident, &left, 1);
        e->span = (SourceSpan){left->span.file,left->span.start,right->span.end};
        e->focus = right->focus; return e;
    }
    parser_error(p, "pipe right-hand side must be a function call");
    return left;
}

static Expr *parse_pipe_from(Parser *p, Expr *left) {
    left = parse_or_from(p, left);
    while (lexer_match(p->lx, TOK_PIPE)) {
        left = pipe_rhs_to_call(left, parse_or(p), p);
    }
    return left;
}

static Expr *parse_pipe(Parser *p) {
    return parse_pipe_from(p, parse_or(p));
}

static Expr *parse_expr(Parser *p) {
    if (++p->expr_depth > FORGE_MAX_EXPR_DEPTH) {
        forge_die("expression nesting too deep");
    }
    Expr *e = parse_pipe(p);
    p->expr_depth--;
    return e;
}

static Block *parse_block(Parser *p) {
    Token start = lexer_peek(p->lx);
    expect(p, TOK_LBRACE);
    Block *b = (Block *)calloc(1, sizeof(Block));
    while (!lexer_match(p->lx, TOK_RBRACE)) {
        block_append(b, parse_stmt(p));
    }
    b->span = parsed_span(p,start);
    b->closing = token_span(p,p->lx->last);
    return b;
}

static Stmt *parsed_let(Parser *p, Token name, bool mut, bool owned, ForgeType ty, Expr *init) {
    Stmt *s = stmt_let(mut,owned,token_str(name),ty,init);
    s->focus = token_span(p,name); s->span = parsed_span(p,name); return s;
}

static Stmt *parse_stmt_inner(Parser *p);
static Stmt *parse_stmt(Parser *p) {
    Token start = lexer_peek(p->lx);
    Stmt *s = parse_stmt_inner(p);
    s->span = parsed_span(p,start);
    if (!s->focus.file) s->focus = token_span(p,start);
    return s;
}
static Stmt *parse_stmt_inner(Parser *p) {
    if (lexer_match(p->lx, TOK_KW_BREAK)) {
        expect(p, TOK_SEMI);
        return stmt_break();
    }
    if (lexer_match(p->lx, TOK_KW_CONTINUE)) {
        expect(p, TOK_SEMI);
        return stmt_continue();
    }
    if (lexer_match(p->lx, TOK_KW_OWN)) {
        expect(p, TOK_KW_LET);
        Token name = lexer_peek(p->lx);
        expect(p, TOK_IDENT);
        ForgeType ty = forge_type_string();
        if (lexer_match(p->lx, TOK_COLON)) ty = parse_type(p);
        Expr *init = NULL;
        if (lexer_match(p->lx, TOK_EQ)) init = parse_expr(p);
        expect(p, TOK_SEMI);
        return parsed_let(p,name,false,true,ty,init);
    }
    if (lexer_match(p->lx, TOK_KW_LET) || lexer_match(p->lx, TOK_KW_MUT)) {
        bool mut = false;
        if (lexer_peek(p->lx).kind == TOK_KW_MUT) {
            mut = true;
            lexer_next(p->lx);
        }
        Token name = lexer_peek(p->lx);
        expect(p, TOK_IDENT);
        ForgeType ty = forge_type_int();
        if (lexer_match(p->lx, TOK_COLON)) ty = parse_type(p);
        Expr *init = NULL;
        if (lexer_match(p->lx, TOK_EQ)) init = parse_expr(p);
        expect(p, TOK_SEMI);
        return parsed_let(p,name,mut,false,ty,init);
    }
    if (lexer_match(p->lx, TOK_KW_RETURN)) {
        Expr *e = NULL;
        if (lexer_peek(p->lx).kind != TOK_SEMI) e = parse_expr(p);
        expect(p, TOK_SEMI);
        return stmt_return(e);
    }
    if (lexer_match(p->lx, TOK_KW_MATCH)) {
        Expr *scrutinee = parse_expr(p);
        expect(p, TOK_LBRACE);
        MatchArm *arms = NULL, *tail = NULL;
        while (!lexer_match(p->lx, TOK_RBRACE)) {
            MatchArm *arm = (MatchArm *)calloc(1, sizeof(MatchArm));
            if (lexer_peek(p->lx).kind == TOK_INT) {
                Token t = lexer_next(p->lx);
                arm->int_pat = t.int_val;
            } else {
                Token t = lexer_peek(p->lx);
                expect(p, TOK_IDENT);
                if (!forge_str_eq(token_str(t), forge_str("_"))) {
                    parser_error(p, "match pattern must be integer or _");
                }
                arm->wildcard = true;
            }
            expect(p, TOK_FAT_ARROW);
            Block *body = (Block *)calloc(1, sizeof(Block));
            if (lexer_peek(p->lx).kind == TOK_LBRACE) {
                body = parse_block(p);
            } else {
                block_append(body, parse_stmt(p));
            }
            arm->body = body;
            if (!arms) arms = tail = arm;
            else { tail->next = arm; tail = arm; }
        }
        return stmt_match(scrutinee, arms);
    }
    if (lexer_match(p->lx, TOK_KW_IF)) {
        expect(p, TOK_LPAREN);
        Expr *cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        Block *then_br = parse_block(p);
        Block *else_br = NULL;
        if (lexer_match(p->lx, TOK_KW_ELSE)) {
            if (lexer_peek(p->lx).kind == TOK_KW_IF) {
                else_br = (Block *)calloc(1, sizeof(Block));
                block_append(else_br, parse_stmt(p));
            } else {
                else_br = parse_block(p);
            }
        }
        return stmt_if(cond, then_br, else_br);
    }
    if (lexer_match(p->lx, TOK_KW_WHILE)) {
        expect(p, TOK_LPAREN);
        Expr *cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        Block *body = parse_block(p);
        return stmt_while(cond, body);
    }
    if (lexer_match(p->lx, TOK_KW_FOR)) {
        expect(p, TOK_LPAREN);
        Stmt *init = NULL;
        if (lexer_peek(p->lx).kind == TOK_KW_LET || lexer_peek(p->lx).kind == TOK_KW_MUT || lexer_peek(p->lx).kind == TOK_KW_OWN) {
            bool owned = lexer_match(p->lx, TOK_KW_OWN);
            bool mut = false;
            if (!owned) mut = lexer_match(p->lx, TOK_KW_MUT);
            if (!owned && !mut) expect(p, TOK_KW_LET);
            else if (owned) expect(p, TOK_KW_LET);
            Token name = lexer_peek(p->lx);
            expect(p, TOK_IDENT);
            ForgeType ty = owned ? forge_type_string() : forge_type_int();
            if (lexer_match(p->lx, TOK_COLON)) ty = parse_type(p);
            Expr *init_expr = NULL;
            if (lexer_match(p->lx, TOK_EQ)) init_expr = parse_expr(p);
            init = parsed_let(p,name,mut,owned,ty,init_expr);
        } else if (lexer_peek(p->lx).kind == TOK_IDENT) {
            Token name = lexer_peek(p->lx);
            lexer_next(p->lx);
            init = parse_assign(p, name);
        }
        expect(p, TOK_SEMI);
        Expr *cond = parse_expr(p);
        expect(p, TOK_SEMI);
        Stmt *step = NULL;
        if (lexer_peek(p->lx).kind == TOK_IDENT) {
            Token name = lexer_peek(p->lx);
            lexer_next(p->lx);
            step = parse_assign(p, name);
        }
        expect(p, TOK_RPAREN);
        Block *body = parse_block(p);
        return stmt_for(init, cond, step, body);
    }
    if (lexer_match(p->lx, TOK_KW_SPAWN)) {
        Token name = lexer_peek(p->lx);
        expect(p, TOK_IDENT);
        expect(p, TOK_LPAREN);
        Expr **args = NULL;
        size_t n = 0, cap = 0;
        if (!lexer_match(p->lx, TOK_RPAREN)) {
            do {
                if (n == cap) {
                    cap = cap ? cap * 2 : 4;
                    args = (Expr **)realloc(args, cap * sizeof(Expr *));
                }
                args[n++] = parse_expr(p);
            } while (lexer_match(p->lx, TOK_COMMA));
            expect(p, TOK_RPAREN);
        }
        expect(p, TOK_SEMI);
        Stmt *s = stmt_spawn(token_str(name),args,n);
        s->focus = token_span(p,name); return s;
    }
    if (lexer_match(p->lx, TOK_KW_SEND)) {
        Expr *target = parse_expr(p);
        expect(p, TOK_COMMA);
        Token tag_tok = lexer_peek(p->lx);
        expect(p, TOK_IDENT);
        expect(p, TOK_COMMA);
        bool move_val = lexer_match(p->lx, TOK_KW_MOVE);
        Expr *value = parse_expr(p);
        expect(p, TOK_SEMI);
        int tag = 0;
        for (size_t i = 0; i < tag_tok.lexeme.len; i++) {
            tag = tag * 31 + (unsigned char)tag_tok.lexeme.data[i];
        }
        return stmt_send(target, tag, value, move_val);
    }
    if (lexer_match(p->lx, TOK_KW_YIELD)) {
        expect(p, TOK_SEMI);
        return stmt_yield();
    }
    if (lexer_match(p->lx, TOK_KW_AWAIT)) {
        Expr *e = parse_expr(p);
        expect(p, TOK_SEMI);
        return stmt_await(e);
    }
    if (lexer_peek(p->lx).kind == TOK_LBRACE) {
        Block *b = parse_block(p);
        return stmt_block(b);
    }
    if (lexer_peek(p->lx).kind == TOK_IDENT) {
        Lexer saved = *p->lx;
        Token name = lexer_next(p->lx);
        TokenKind next = lexer_peek(p->lx).kind;
        if (next == TOK_EQ || next == TOK_PLUSEQ || next == TOK_MINUSEQ ||
            next == TOK_STAREQ || next == TOK_SLASHEQ || next == TOK_PERCENTEQ) {
            Stmt *s = parse_assign(p, name);
            expect(p, TOK_SEMI);
            return s;
        }
        *p->lx = saved;
    }
    Expr *e = parse_expr(p);
    expect(p, TOK_SEMI);
    return stmt_expr(e);
}

static Param *parse_params(Parser *p) {
    Param *head = NULL, *tail = NULL;
    if (lexer_match(p->lx, TOK_RPAREN)) return NULL;
    do {
        Token name = lexer_peek(p->lx);
        expect(p, TOK_IDENT);
        expect(p, TOK_COLON);
        ForgeType ty = parse_type(p);
        Param *param = (Param *)calloc(1, sizeof(Param));
        param->span = token_span(p,name);
        param->name = token_str(name);
        param->type = ty;
        if (!head) head = tail = param;
        else { tail->next = param; tail = param; }
    } while (lexer_match(p->lx, TOK_COMMA));
    expect(p, TOK_RPAREN);
    return head;
}

static FnDecl parse_fn_body(Parser *p, bool is_extern) {
    Token name = lexer_peek(p->lx);
    expect(p, TOK_IDENT);
    expect(p, TOK_LPAREN);
    Param *params = parse_params(p);
    ForgeType ret = forge_type_void();
    if (lexer_match(p->lx, TOK_COLON)) ret = parse_type(p);
    FnDecl fn = { .name=token_str(name), .params=params, .ret_type=ret, .body=block_new(), .is_extern=is_extern, .span=token_span(p,name) };
    if (is_extern) {
        expect(p, TOK_SEMI);
    } else {
        Block *body = parse_block(p);
        fn.body = *body;
        free(body);
    }
    return fn;
}

static FnDecl parse_fn(Parser *p) {
    expect(p, TOK_KW_FN);
    return parse_fn_body(p, false);
}

static FnDecl parse_extern_fn(Parser *p) {
    expect(p, TOK_KW_EXTERN);
    expect(p, TOK_KW_FN);
    return parse_fn_body(p, true);
}

static CoroDecl parse_coroutine(Parser *p) {
    expect(p, TOK_KW_COROUTINE);
    Token name = lexer_peek(p->lx);
    expect(p, TOK_IDENT);
    expect(p, TOK_LPAREN);
    Param *params = parse_params(p);
    Block *body = parse_block(p);
    CoroDecl coro = { token_str(name), params, *body };
    free(body);
    (void)params;
    return coro;
}

static ProcessDecl parse_process(Parser *p) {
    expect(p, TOK_KW_PROCESS);
    Token name = lexer_peek(p->lx);
    expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);

    ProcessDecl proc = {0};
    proc.name = token_str(name);
    proc.body = block_new();

    CoroDecl *coros = NULL;
    size_t coro_count = 0, coro_cap = 0;

    while (lexer_peek(p->lx).kind != TOK_RBRACE) {
        if (lexer_peek(p->lx).kind == TOK_KW_COROUTINE) {
            if (coro_count == coro_cap) {
                coro_cap = coro_cap ? coro_cap * 2 : 4;
                coros = (CoroDecl *)realloc(coros, coro_cap * sizeof(CoroDecl));
            }
            coros[coro_count++] = parse_coroutine(p);
        } else if (lexer_match(p->lx, TOK_KW_ON)) {
            expect(p, TOK_KW_RECEIVE);
            expect(p, TOK_LPAREN);
            Token param = lexer_peek(p->lx);
            expect(p, TOK_IDENT);
            expect(p, TOK_COLON);
            ForgeType ty = parse_type(p);
            expect(p, TOK_RPAREN);
            proc.has_receive = true;
            proc.receive_param.name = token_str(param);
            proc.receive_param.type = ty;
            proc.on_receive = parse_block(p);
        } else {
            block_append(&proc.body, parse_stmt(p));
        }
    }
    expect(p, TOK_RBRACE);
    proc.coros = coros;
    proc.coro_count = coro_count;
    return proc;
}

static SupervisorDecl parse_supervisor(Parser *p) {
    expect(p, TOK_KW_SUPERVISOR);
    Token name = lexer_peek(p->lx);
    expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);

    SupervisorDecl sup = {0};
    sup.name = token_str(name);
    sup.policy = SUP_RESTART_PROCESS;
    size_t child_capacity = 0;

    while (lexer_peek(p->lx).kind != TOK_RBRACE) {
        if (lexer_match(p->lx, TOK_KW_RESTART)) {
            expect(p, TOK_COLON);
            Token pol = lexer_peek(p->lx);
            expect(p, TOK_IDENT);
            if (forge_str_eq(token_str(pol), forge_str("coro"))) sup.policy = SUP_RESTART_CORO;
            else if (forge_str_eq(token_str(pol), forge_str("all"))) sup.policy = SUP_RESTART_ALL;
            else sup.policy = SUP_RESTART_PROCESS;
            expect(p, TOK_SEMI);
        } else {
            Token child_tok = lexer_peek(p->lx);
            expect(p, TOK_IDENT);
            ForgeStr child = token_str(child_tok);
            sup.children = reserve_declarations(sup.children, sup.child_count,
                &child_capacity, sizeof(*sup.children));
            sup.children[sup.child_count++] = child;
            expect(p, TOK_SEMI);
        }
    }
    expect(p, TOK_RBRACE);
    return sup;
}

static LibraryDecl parse_library(Parser *p) {
    expect(p, TOK_KW_LIBRARY);
    Token name = lexer_peek(p->lx);
    expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);

    LibraryDecl lib = {0};
    lib.name = token_str(name);
    lib.present = true;
    size_t import_capacity = 0, fn_capacity = 0;

    while (lexer_peek(p->lx).kind != TOK_RBRACE) {
        if (lexer_match(p->lx, TOK_KW_IMPORT)) {
            Token mod = lexer_peek(p->lx);
            expect(p, TOK_IDENT);
            expect(p, TOK_SEMI);
            lib.imports = reserve_declarations(lib.imports, lib.import_count,
                &import_capacity, sizeof(*lib.imports));
            lib.imports[lib.import_count++] = token_str(mod);
        } else if (lexer_match(p->lx, TOK_KW_EXPORT)) {
            expect(p, TOK_KW_FN);
            FnDecl fn = parse_fn_body(p, false);
            lib.functions = reserve_declarations(lib.functions, lib.fn_count,
                &fn_capacity, sizeof(*lib.functions));
            lib.functions[lib.fn_count++] = fn;
        } else {
            parser_error(p, "expected import or export in library");
        }
    }
    expect(p, TOK_RBRACE);
    return lib;
}

static StructDecl parse_struct(Parser *p) {
    expect(p, TOK_KW_STRUCT);
    Token name = lexer_peek(p->lx);
    expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);

    StructDecl sd = { token_str(name), NULL };
    Field *tail = NULL;
    while (lexer_peek(p->lx).kind != TOK_RBRACE) {
        Token field_name = lexer_peek(p->lx);
        expect(p, TOK_IDENT);
        expect(p, TOK_COLON);
        ForgeType ty = parse_type(p);
        expect(p, TOK_SEMI);
        Field *f = (Field *)calloc(1, sizeof(Field));
        f->name = token_str(field_name);
        f->type = ty;
        if (!sd.fields) sd.fields = tail = f;
        else { tail->next = f; tail = f; }
    }
    expect(p, TOK_RBRACE);
    return sd;
}

static EnumDecl parse_enum(Parser *p) {
    expect(p, TOK_KW_ENUM);
    Token name = lexer_peek(p->lx);
    expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);

    EnumDecl ed = { token_str(name), NULL };
    EnumVariant *tail = NULL;
    int64_t next_val = 0;
    while (lexer_peek(p->lx).kind != TOK_RBRACE) {
        Token variant = lexer_peek(p->lx);
        expect(p, TOK_IDENT);
        int64_t val = next_val;
        if (lexer_match(p->lx, TOK_EQ)) {
            Token v = lexer_peek(p->lx);
            expect(p, TOK_INT);
            val = v.int_val;
            next_val = val + 1;
        } else {
            next_val++;
        }
        expect(p, TOK_SEMI);
        EnumVariant *ev = (EnumVariant *)calloc(1, sizeof(EnumVariant));
        ev->name = token_str(variant);
        ev->value = val;
        if (!ed.variants) ed.variants = tail = ev;
        else { tail->next = ev; tail = ev; }
    }
    expect(p, TOK_RBRACE);
    return ed;
}

static NativeDecl parse_native(Parser *p) {
    expect(p, TOK_KW_NATIVE);
    Token name = lexer_peek(p->lx);
    expect(p, TOK_IDENT);
    Block *body = parse_block(p);
    NativeDecl nd = { token_str(name), *body };
    free(body);
    return nd;
}

Program parse_program_named(Lexer *lx, const char *path) {
    SourceFile *source = calloc(1,sizeof(*source));
    if (!source) forge_die("out of memory");
    source->path = forge_strdup(forge_str(path ? path : "<input>"));
    if (!source->path) forge_die("out of memory");
    source->text = lx->src; source->length = lx->len;
    Parser p = { .lx = lx, .source = source };
    Program prog = {0};
    prog.source_files = malloc(sizeof(*prog.source_files));
    if (!prog.source_files) forge_die("out of memory");
    prog.source_files[0] = source; prog.source_count = 1;
    size_t path_import_capacity = 0, import_capacity = 0;
    size_t struct_capacity = 0, enum_capacity = 0, native_capacity = 0;
    size_t fn_capacity = 0, process_capacity = 0, const_capacity = 0;
    size_t supervisor_capacity = 0;

    while (lexer_peek(p.lx).kind != TOK_EOF) {
        Token t = lexer_peek(p.lx);
        if (t.kind == TOK_KW_IMPORT) {
            lexer_next(p.lx);
            if (lexer_peek(p.lx).kind == TOK_STRING) {
                Token path_tok = lexer_peek(p.lx);
                lexer_next(p.lx);
                expect(&p, TOK_SEMI);
                prog.path_imports = reserve_declarations(prog.path_imports, prog.path_import_count,
                    &path_import_capacity, sizeof(*prog.path_imports));
                prog.path_imports[prog.path_import_count++] = token_str(path_tok);
            } else {
                Token mod = lexer_peek(p.lx);
                expect(&p, TOK_IDENT);
                expect(&p, TOK_SEMI);
                prog.imports = reserve_declarations(prog.imports, prog.import_count,
                    &import_capacity, sizeof(*prog.imports));
                prog.imports[prog.import_count++] = token_str(mod);
            }
        } else if (t.kind == TOK_KW_LIBRARY) {
            if (prog.library.present) parser_error(&p, "only one library per file");
            prog.library = parse_library(&p);
        } else if (t.kind == TOK_KW_STRUCT) {
            prog.structs = reserve_declarations(prog.structs, prog.struct_count,
                &struct_capacity, sizeof(*prog.structs));
            prog.structs[prog.struct_count++] = parse_struct(&p);
        } else if (t.kind == TOK_KW_ENUM) {
            prog.enums = reserve_declarations(prog.enums, prog.enum_count,
                &enum_capacity, sizeof(*prog.enums));
            prog.enums[prog.enum_count++] = parse_enum(&p);
        } else if (t.kind == TOK_KW_NATIVE) {
            prog.natives = reserve_declarations(prog.natives, prog.native_count,
                &native_capacity, sizeof(*prog.natives));
            prog.natives[prog.native_count++] = parse_native(&p);
        } else if (t.kind == TOK_KW_EXTERN) {
            prog.functions = reserve_declarations(prog.functions, prog.fn_count,
                &fn_capacity, sizeof(*prog.functions));
            prog.functions[prog.fn_count++] = parse_extern_fn(&p);
        } else if (t.kind == TOK_KW_PROCESS) {
            prog.processes = reserve_declarations(prog.processes, prog.process_count,
                &process_capacity, sizeof(*prog.processes));
            prog.processes[prog.process_count++] = parse_process(&p);
        } else if (t.kind == TOK_KW_CONST) {
            lexer_next(p.lx);
            Token name = lexer_peek(p.lx);
            expect(&p, TOK_IDENT);
            expect(&p, TOK_EQ);
            Expr *val = parse_expr(&p);
            expect(&p, TOK_SEMI);
            prog.consts = reserve_declarations(prog.consts, prog.const_count,
                &const_capacity, sizeof(*prog.consts));
            prog.consts[prog.const_count++] = (ConstDecl){ token_str(name), val };
        } else if (t.kind == TOK_KW_FN) {
            prog.functions = reserve_declarations(prog.functions, prog.fn_count,
                &fn_capacity, sizeof(*prog.functions));
            prog.functions[prog.fn_count++] = parse_fn(&p);
        } else if (t.kind == TOK_KW_SUPERVISOR) {
            prog.supervisors = reserve_declarations(prog.supervisors, prog.supervisor_count,
                &supervisor_capacity, sizeof(*prog.supervisors));
            prog.supervisors[prog.supervisor_count++] = parse_supervisor(&p);
        } else {
            parser_error(&p, "expected top-level declaration");
        }
    }
    return prog;
}

Program parse_program(Lexer *lx) { return parse_program_named(lx,"<input>"); }
