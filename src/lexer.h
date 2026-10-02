/* lexer.h - token stream produced by the scanner. */
#ifndef DUCK_LEXER_H
#define DUCK_LEXER_H

#include "common.h"

typedef enum {
    TK_EOF,
    TK_IDENT,
    TK_INT,
    TK_FLOAT,
    TK_STRING,

    /* keywords */
    TK_FN,
    TK_STRUCT,
    TK_CONST,
    TK_LET,
    TK_IF,
    TK_ELSE,
    TK_WHILE,
    TK_FOR,
    TK_IN,
    TK_RETURN,
    TK_BREAK,
    TK_CONTINUE,
    TK_TRUE,
    TK_FALSE,
    TK_KW_INT,
    TK_KW_FLOAT,
    TK_KW_BOOL,
    TK_KW_STRING,

    /* punctuation */
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_LBRACE,
    TK_RBRACE,
    TK_COMMA,
    TK_SEMI,
    TK_COLON,
    TK_ARROW,
    TK_DOT,
    TK_DOTDOT,

    /* operators */
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_PERCENT,
    TK_ASSIGN,
    TK_EQ,
    TK_NE,
    TK_LT,
    TK_LE,
    TK_GT,
    TK_GE,
    TK_AND,
    TK_OR,
    TK_NOT,
    TK_BITAND,
    TK_BITOR,
    TK_XOR,
    TK_TILDE,
    TK_SHL,
    TK_SHR
} TokenKind;

typedef struct Token {
    TokenKind kind;
    int line;
    int col;
    const char *text; /* raw lexeme, not NUL-terminated */
    int len;          /* length of the raw lexeme */
    long ival;        /* TK_INT: decoded literal */
    double dval;      /* TK_FLOAT: decoded literal */
    char *sval;       /* TK_STRING: decoded, NUL-terminated */
    char *name;       /* TK_IDENT: interned, NUL-terminated */
} Token;

void lex(const SourceFile *src, Token **out_toks, int *out_count);
const char *token_kind_name(TokenKind k);

#endif /* DUCK_LEXER_H */
