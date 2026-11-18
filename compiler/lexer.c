#include "lexer.h"
#include <errno.h>
#include <math.h>

static bool is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident(char c) {
    return is_ident_start(c) || isdigit((unsigned char)c);
}

static Token make_token(Lexer *lx, TokenKind kind, ForgeStr lexeme) {
  Token t = { .kind=kind, .lexeme=lexeme, .line=lx->line, .col=lx->col,
              .start=lx->token_start, .end=lx->pos };
  return t;
}

static void skip_ws(Lexer *lx) {
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            lx->pos++;
            lx->col++;
        } else if (c == '\n') {
            lx->pos++;
            lx->line++;
            lx->col = 1;
        } else if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/') {
            lx->pos += 2;
            lx->col += 2;
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') {
                lx->pos++;
                lx->col++;
            }
        } else {
            break;
        }
    }
}

static TokenKind keyword_kind(ForgeStr s) {
    switch (s.len) {
    case 2:
        if (memcmp(s.data, "on", s.len) == 0) return TOK_KW_ON;
        if (memcmp(s.data, "fn", s.len) == 0) return TOK_KW_FN;
        if (memcmp(s.data, "if", s.len) == 0) return TOK_KW_IF;
        break;
    case 3:
        if (memcmp(s.data, "let", s.len) == 0) return TOK_KW_LET;
        if (memcmp(s.data, "mut", s.len) == 0) return TOK_KW_MUT;
        if (memcmp(s.data, "for", s.len) == 0) return TOK_KW_FOR;
        if (memcmp(s.data, "int", s.len) == 0) return TOK_KW_INT;
        if (memcmp(s.data, "ptr", s.len) == 0) return TOK_KW_PTR;
        if (memcmp(s.data, "own", s.len) == 0) return TOK_KW_OWN;
        break;
    case 4:
        if (memcmp(s.data, "send", s.len) == 0) return TOK_KW_SEND;
        if (memcmp(s.data, "recv", s.len) == 0) return TOK_KW_RECV;
        if (memcmp(s.data, "else", s.len) == 0) return TOK_KW_ELSE;
        if (memcmp(s.data, "enum", s.len) == 0) return TOK_KW_ENUM;
        if (memcmp(s.data, "true", s.len) == 0) return TOK_KW_TRUE;
        if (memcmp(s.data, "bool", s.len) == 0) return TOK_KW_BOOL;
        if (memcmp(s.data, "void", s.len) == 0) return TOK_KW_VOID;
        if (memcmp(s.data, "move", s.len) == 0) return TOK_KW_MOVE;
        break;
    case 5:
        if (memcmp(s.data, "spawn", s.len) == 0) return TOK_KW_SPAWN;
        if (memcmp(s.data, "yield", s.len) == 0) return TOK_KW_YIELD;
        if (memcmp(s.data, "while", s.len) == 0) return TOK_KW_WHILE;
        if (memcmp(s.data, "break", s.len) == 0) return TOK_KW_BREAK;
        if (memcmp(s.data, "false", s.len) == 0) return TOK_KW_FALSE;
        if (memcmp(s.data, "float", s.len) == 0) return TOK_KW_FLOAT;
        if (memcmp(s.data, "await", s.len) == 0) return TOK_KW_AWAIT;
        if (memcmp(s.data, "match", s.len) == 0) return TOK_KW_MATCH;
        if (memcmp(s.data, "const", s.len) == 0) return TOK_KW_CONST;
        break;
    case 6:
        if (memcmp(s.data, "return", s.len) == 0) return TOK_KW_RETURN;
        if (memcmp(s.data, "struct", s.len) == 0) return TOK_KW_STRUCT;
        if (memcmp(s.data, "native", s.len) == 0) return TOK_KW_NATIVE;
        if (memcmp(s.data, "extern", s.len) == 0) return TOK_KW_EXTERN;
        if (memcmp(s.data, "string", s.len) == 0) return TOK_KW_STRING;
        if (memcmp(s.data, "import", s.len) == 0) return TOK_KW_IMPORT;
        if (memcmp(s.data, "export", s.len) == 0) return TOK_KW_EXPORT;
        break;
    case 7:
        if (memcmp(s.data, "process", s.len) == 0) return TOK_KW_PROCESS;
        if (memcmp(s.data, "receive", s.len) == 0) return TOK_KW_RECEIVE;
        if (memcmp(s.data, "restart", s.len) == 0) return TOK_KW_RESTART;
        if (memcmp(s.data, "library", s.len) == 0) return TOK_KW_LIBRARY;
        break;
    case 8:
        if (memcmp(s.data, "continue", s.len) == 0) return TOK_KW_CONTINUE;
        break;
    case 9:
        if (memcmp(s.data, "coroutine", s.len) == 0) return TOK_KW_COROUTINE;
        break;
    case 10:
        if (memcmp(s.data, "supervisor", s.len) == 0) return TOK_KW_SUPERVISOR;
        break;
    }
    return TOK_IDENT;
}

void lexer_init(Lexer *lx, const char *src, size_t len) {
    lx->src = src;
    lx->pos = 0;
    lx->len = len;
    lx->line = 1;
    lx->col = 1;
    lx->has_current = false;
    lx->token_start = 0;
    lx->last = (Token){0};
}

static Token read_number(Lexer *lx) {
    size_t start = lx->pos;
    int col = lx->col;
    bool is_float = false;
    while (lx->pos < lx->len && isdigit((unsigned char)lx->src[lx->pos])) {
        lx->pos++;
        lx->col++;
    }
    if (lx->pos < lx->len && lx->src[lx->pos] == '.') {
        is_float = true;
        lx->pos++;
        lx->col++;
        while (lx->pos < lx->len && isdigit((unsigned char)lx->src[lx->pos])) {
            lx->pos++;
            lx->col++;
        }
    }
    ForgeStr lex = { (char *)lx->src + start, lx->pos - start };
    char *buf = forge_strdup(lex);
    if (!buf) forge_die("out of memory");
    Token t = make_token(lx, is_float ? TOK_FLOAT : TOK_INT, lex);
    t.line = lx->line;
    t.col = col;
    errno = 0;
    if (is_float) t.float_val = strtod(buf, NULL);
    else t.int_val = strtoll(buf, NULL, 10);
    free(buf);
    if (errno == ERANGE || (is_float && !isfinite(t.float_val))) {
        fprintf(stderr, "forge:%d:%d: numeric literal out of range\n", t.line, t.col);
        exit(1);
    }
    return t;
}

/* Decode one escape sequence character into its byte value. Unknown escapes
 * keep the escaped character itself (so "\q" is a plain 'q'). */
static char decode_escape(char c) {
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return '\0';
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'e': return 27;
    default: return c; /* covers '\\', '"', '\'' and anything unrecognised */
    }
}

static Token read_string(Lexer *lx) {
    int line = lx->line, col = lx->col;
    lx->pos++;
    lx->col++;
    size_t start = lx->pos;
    bool has_escape = false;
    while (lx->pos < lx->len && lx->src[lx->pos] != '"') {
        if (lx->src[lx->pos] == '\\' && lx->pos + 1 < lx->len) {
            has_escape = true;
            lx->pos += 2;
            lx->col += 2;
            continue;
        }
        if (lx->src[lx->pos] == '\n') {
            lx->line++;
            lx->col = 1;
        } else {
            lx->col++;
        }
        lx->pos++;
    }
    if (lx->pos >= lx->len) forge_die("unterminated string");
    size_t raw_len = lx->pos - start;
    ForgeStr lex = { (char *)lx->src + start, raw_len };
    if (has_escape) {
        /* Store the decoded bytes, not the source spelling: consumers (codegen)
         * re-escape on emit, so keeping "\n" verbatim here would produce a
         * literal backslash-n in the output instead of a newline. The buffer
         * outlives the lexer along with the rest of the AST. */
        char *buf = (char *)malloc(raw_len + 1);
        if (!buf) forge_die("out of memory");
        size_t out_len = 0;
        for (size_t i = 0; i < raw_len; i++) {
            char c = lex.data[i];
            if (c == '\\' && i + 1 < raw_len) {
                i++;
                buf[out_len++] = decode_escape(lex.data[i]);
            } else {
                buf[out_len++] = c;
            }
        }
        buf[out_len] = '\0';
        lex.data = buf;
        lex.len = out_len;
    }
    lx->pos++;
    lx->col++;
    Token t = make_token(lx, TOK_STRING, lex);
    t.line = line;
    t.col = col;
    return t;
}

static Token read_ident(Lexer *lx) {
    size_t start = lx->pos;
    int line = lx->line, col = lx->col;
    while (lx->pos < lx->len && is_ident(lx->src[lx->pos])) {
        lx->pos++;
        lx->col++;
    }
    ForgeStr lex = { (char *)lx->src + start, lx->pos - start };
    TokenKind kind = keyword_kind(lex);
    Token t = make_token(lx, kind, lex);
    t.line = line;
    t.col = col;
    return t;
}

static Token lexer_next_raw(Lexer *lx) {
    skip_ws(lx);
    lx->token_start = lx->pos;
    if (lx->pos >= lx->len) return make_token(lx, TOK_EOF, forge_str(""));

    char c = lx->src[lx->pos];
    int line = lx->line, col = lx->col;

    if (isdigit((unsigned char)c)) return read_number(lx);
    if (is_ident_start(c)) return read_ident(lx);
    if (c == '"') return read_string(lx);

    lx->pos++;
    lx->col++;

    switch (c) {
    case '(': return make_token(lx, TOK_LPAREN, forge_str("("));
    case ')': return make_token(lx, TOK_RPAREN, forge_str(")"));
    case '{': return make_token(lx, TOK_LBRACE, forge_str("{"));
    case '}': return make_token(lx, TOK_RBRACE, forge_str("}"));
    case '[': return make_token(lx, TOK_LBRACKET, forge_str("["));
    case ']': return make_token(lx, TOK_RBRACKET, forge_str("]"));
    case ',': return make_token(lx, TOK_COMMA, forge_str(","));
    case ':': return make_token(lx, TOK_COLON, forge_str(":"));
    case ';': return make_token(lx, TOK_SEMI, forge_str(";"));
    case '.': return make_token(lx, TOK_DOT, forge_str("."));
    case '+':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_PLUSEQ, forge_str("+="));
        }
        return make_token(lx, TOK_PLUS, forge_str("+"));
    case '-':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_MINUSEQ, forge_str("-="));
        }
        return make_token(lx, TOK_MINUS, forge_str("-"));
    case '*':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_STAREQ, forge_str("*="));
        }
        return make_token(lx, TOK_STAR, forge_str("*"));
    case '/':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_SLASHEQ, forge_str("/="));
        }
        return make_token(lx, TOK_SLASH, forge_str("/"));
    case '%':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_PERCENTEQ, forge_str("%="));
        }
        return make_token(lx, TOK_PERCENT, forge_str("%"));
    case '!':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_NE, forge_str("!="));
        }
        return make_token(lx, TOK_BANG, forge_str("!"));
    case '=':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_EQEQ, forge_str("=="));
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == '>') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_FAT_ARROW, forge_str("=>"));
        }
        return make_token(lx, TOK_EQ, forge_str("="));
    case '<':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_LE, forge_str("<="));
        }
        return make_token(lx, TOK_LT, forge_str("<"));
    case '>':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_GE, forge_str(">="));
        }
        return make_token(lx, TOK_GT, forge_str(">"));
    case '&':
        if (lx->pos < lx->len && lx->src[lx->pos] == '&') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_ANDAND, forge_str("&&"));
        }
        break;
    case '|':
        if (lx->pos < lx->len && lx->src[lx->pos] == '|') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_OROR, forge_str("||"));
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == '>') {
            lx->pos++; lx->col++;
            return make_token(lx, TOK_PIPE, forge_str("|>"));
        }
        break;
    default:
        break;
    }
    fprintf(stderr, "forge: unexpected character '%c' at %d:%d\n", c, line, col);
    exit(1);
}

Token lexer_next(Lexer *lx) {
    Token t;
    if (lx->has_current) { lx->has_current = false; t = lx->current; }
    else t = lexer_next_raw(lx);
    lx->last = t;
    return t;
}

Token lexer_peek(Lexer *lx) {
    if (!lx->has_current) {
        lx->current = lexer_next_raw(lx);
        lx->has_current = true;
    }
    return lx->current;
}

bool lexer_match(Lexer *lx, TokenKind kind) {
    Token t = lexer_peek(lx);
    if (t.kind == kind) {
        lexer_next(lx);
        return true;
    }
    return false;
}
