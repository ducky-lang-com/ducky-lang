/* lexer.c - hand-written scanner for Duck source files. */
#include "lexer.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const SourceFile *f;
    const char *p;
    const char *end;
    int line;
    int col;
    Token *toks;
    int n;
    int cap;
} Lexer;

static void push_token(Lexer *lx, Token t) {
    if (lx->n == lx->cap) {
        lx->cap = lx->cap ? lx->cap * 2 : 64;
        Token *nt = realloc(lx->toks, (size_t)lx->cap * sizeof(Token));
        if (!nt) fatal("out of memory");
        lx->toks = nt;
    }
    lx->toks[lx->n++] = t;
}

static void advance(Lexer *lx) {
    if (*lx->p == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
    lx->p++;
}

static Token make_token(Lexer *lx, TokenKind kind, const char *start, int line, int col) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.kind = kind;
    t.line = line;
    t.col = col;
    t.text = start;
    t.len = (int)(lx->p - start);
    return t;
}

static struct {
    const char *word;
    TokenKind kind;
} keywords[] = {
    {"fn", TK_FN},         {"let", TK_LET},       {"if", TK_IF},
    {"else", TK_ELSE},     {"while", TK_WHILE},   {"return", TK_RETURN},
    {"true", TK_TRUE},     {"false", TK_FALSE},   {"int", TK_KW_INT},
    {"bool", TK_KW_BOOL},  {"string", TK_KW_STRING},
};

static void skip_trivia(Lexer *lx) {
    for (;;) {
        if (lx->p < lx->end && (*lx->p == ' ' || *lx->p == '\t' || *lx->p == '\r' ||
                                *lx->p == '\n')) {
            advance(lx);
            continue;
        }
        if (lx->p + 1 < lx->end && lx->p[0] == '/' && lx->p[1] == '/') {
            while (lx->p < lx->end && *lx->p != '\n') advance(lx);
            continue;
        }
        if (lx->p + 1 < lx->end && lx->p[0] == '/' && lx->p[1] == '*') {
            int line = lx->line, col = lx->col;
            advance(lx);
            advance(lx);
            while (lx->p + 1 < lx->end && !(lx->p[0] == '*' && lx->p[1] == '/')) advance(lx);
            if (lx->p + 1 >= lx->end) {
                fatal_at(lx->f, line, col, "unterminated block comment");
            }
            advance(lx);
            advance(lx);
            continue;
        }
        return;
    }
}

static void scan_ident(Lexer *lx, const char *start, int line, int col) {
    while (lx->p < lx->end && (isalnum((unsigned char)*lx->p) || *lx->p == '_')) advance(lx);

    int len = (int)(lx->p - start);
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if ((int)strlen(keywords[i].word) == len && memcmp(keywords[i].word, start, (size_t)len) == 0) {
            push_token(lx, make_token(lx, keywords[i].kind, start, line, col));
            return;
        }
    }

    Token t = make_token(lx, TK_IDENT, start, line, col);
    t.name = arena_strndup(start, (size_t)len);
    push_token(lx, t);
}

static void scan_number(Lexer *lx, const char *start, int line, int col) {
    long base = 10;
    if (lx->p < lx->end && *lx->p == '0' && lx->p + 1 < lx->end &&
        (lx->p[1] == 'x' || lx->p[1] == 'X')) {
        base = 16;
        advance(lx);
        advance(lx);
        if (lx->p == lx->end || !isxdigit((unsigned char)*lx->p)) {
            fatal_at(lx->f, lx->line, lx->col, "expected hexadecimal digits after '0x'");
        }
    }

    long value = 0;
    while (lx->p < lx->end) {
        char c = *lx->p;
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        if (digit >= base) break;

        if (value > (LONG_MAX - digit) / base) {
            fatal_at(lx->f, line, col, "integer literal is out of range for a 64-bit integer");
        }
        value = value * base + digit;
        advance(lx);
    }

    if (lx->p < lx->end && (isalpha((unsigned char)*lx->p) || *lx->p == '_')) {
        fatal_at(lx->f, line, col, "invalid suffix on integer literal");
    }

    Token t = make_token(lx, TK_INT, start, line, col);
    t.ival = value;
    push_token(lx, t);
}

static void scan_string(Lexer *lx, const char *start, int line, int col) {
    advance(lx); /* opening quote */

    size_t cap = 64;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) fatal("out of memory");

    for (;;) {
        if (lx->p == lx->end || *lx->p == '\n') {
            free(buf);
            fatal_at(lx->f, line, col, "unterminated string literal");
        }
        if (*lx->p == '"') {
            advance(lx);
            break;
        }
        char c = *lx->p;
        if (c == '\\') {
            advance(lx);
            if (lx->p == lx->end) {
                free(buf);
                fatal_at(lx->f, line, col, "unterminated string literal");
            }
            char esc = *lx->p;
            switch (esc) {
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            case 'r':  c = '\r'; break;
            case '\\': c = '\\'; break;
            case '"':  c = '"';  break;
            default:
                free(buf);
                fatal_at(lx->f, lx->line, lx->col, "unknown escape sequence '\\%c'", esc);
            }
            advance(lx);
        } else {
            advance(lx);
        }

        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                fatal("out of memory");
            }
            buf = nb;
        }
        buf[len++] = c;
    }

    Token t = make_token(lx, TK_STRING, start, line, col);
    t.sval = arena_strndup(buf, len);
    free(buf);
    push_token(lx, t);
}

void lex(const SourceFile *src, Token **out_toks, int *out_count) {
    Lexer lx;
    memset(&lx, 0, sizeof(lx));
    lx.f = src;
    lx.p = src->src;
    lx.end = src->src + src->len;
    lx.line = 1;
    lx.col = 1;

    for (;;) {
        skip_trivia(&lx);
        if (lx.p == lx.end) {
            Token t = make_token(&lx, TK_EOF, lx.p, lx.line, lx.col);
            push_token(&lx, t);
            *out_toks = lx.toks;
            *out_count = lx.n;
            return;
        }

        const char *start = lx.p;
        int line = lx.line, col = lx.col;
        char c = *lx.p;

        if (isalpha((unsigned char)c) || c == '_') {
            scan_ident(&lx, start, line, col);
            continue;
        }
        if (isdigit((unsigned char)c)) {
            scan_number(&lx, start, line, col);
            continue;
        }
        if (c == '"') {
            scan_string(&lx, start, line, col);
            continue;
        }

        Token t;
        advance(&lx);
        switch (c) {
        case '(': t = make_token(&lx, TK_LPAREN, start, line, col); break;
        case ')': t = make_token(&lx, TK_RPAREN, start, line, col); break;
        case '{': t = make_token(&lx, TK_LBRACE, start, line, col); break;
        case '}': t = make_token(&lx, TK_RBRACE, start, line, col); break;
        case ',': t = make_token(&lx, TK_COMMA, start, line, col); break;
        case ';': t = make_token(&lx, TK_SEMI, start, line, col); break;
        case ':': t = make_token(&lx, TK_COLON, start, line, col); break;
        case '+': t = make_token(&lx, TK_PLUS, start, line, col); break;
        case '*': t = make_token(&lx, TK_STAR, start, line, col); break;
        case '/': t = make_token(&lx, TK_SLASH, start, line, col); break;
        case '%': t = make_token(&lx, TK_PERCENT, start, line, col); break;
        case '-':
            if (lx.p < lx.end && *lx.p == '>') {
                advance(&lx);
                t = make_token(&lx, TK_ARROW, start, line, col);
            } else {
                t = make_token(&lx, TK_MINUS, start, line, col);
            }
            break;
        case '=':
            if (lx.p < lx.end && *lx.p == '=') {
                advance(&lx);
                t = make_token(&lx, TK_EQ, start, line, col);
            } else {
                t = make_token(&lx, TK_ASSIGN, start, line, col);
            }
            break;
        case '!':
            if (lx.p < lx.end && *lx.p == '=') {
                advance(&lx);
                t = make_token(&lx, TK_NE, start, line, col);
            } else {
                t = make_token(&lx, TK_NOT, start, line, col);
            }
            break;
        case '<':
            if (lx.p < lx.end && *lx.p == '=') {
                advance(&lx);
                t = make_token(&lx, TK_LE, start, line, col);
            } else {
                t = make_token(&lx, TK_LT, start, line, col);
            }
            break;
        case '>':
            if (lx.p < lx.end && *lx.p == '=') {
                advance(&lx);
                t = make_token(&lx, TK_GE, start, line, col);
            } else {
                t = make_token(&lx, TK_GT, start, line, col);
            }
            break;
        case '&':
            if (lx.p < lx.end && *lx.p == '&') {
                advance(&lx);
                t = make_token(&lx, TK_AND, start, line, col);
            } else {
                fatal_at(src, line, col, "unexpected character '&' (did you mean '&&'?)");
            }
            break;
        case '|':
            if (lx.p < lx.end && *lx.p == '|') {
                advance(&lx);
                t = make_token(&lx, TK_OR, start, line, col);
            } else {
                fatal_at(src, line, col, "unexpected character '|' (did you mean '||'?)");
            }
            break;
        default:
            fatal_at(src, line, col, "unexpected character '%c'", c);
        }
        push_token(&lx, t);
    }
}

const char *token_kind_name(TokenKind k) {
    switch (k) {
    case TK_EOF:       return "EOF";
    case TK_IDENT:     return "identifier";
    case TK_INT:       return "integer";
    case TK_STRING:    return "string";
    case TK_FN:        return "fn";
    case TK_LET:       return "let";
    case TK_IF:        return "if";
    case TK_ELSE:      return "else";
    case TK_WHILE:     return "while";
    case TK_RETURN:    return "return";
    case TK_TRUE:      return "true";
    case TK_FALSE:     return "false";
    case TK_KW_INT:    return "int";
    case TK_KW_BOOL:   return "bool";
    case TK_KW_STRING: return "string";
    case TK_LPAREN:    return "(";
    case TK_RPAREN:    return ")";
    case TK_LBRACE:    return "{";
    case TK_RBRACE:    return "}";
    case TK_COMMA:     return ",";
    case TK_SEMI:      return ";";
    case TK_COLON:     return ":";
    case TK_ARROW:     return "->";
    case TK_PLUS:      return "+";
    case TK_MINUS:     return "-";
    case TK_STAR:      return "*";
    case TK_SLASH:     return "/";
    case TK_PERCENT:   return "%";
    case TK_ASSIGN:    return "=";
    case TK_EQ:        return "==";
    case TK_NE:        return "!=";
    case TK_LT:        return "<";
    case TK_LE:        return "<=";
    case TK_GT:        return ">";
    case TK_GE:        return ">=";
    case TK_AND:       return "&&";
    case TK_OR:        return "||";
    case TK_NOT:       return "!";
    }
    return "?";
}
