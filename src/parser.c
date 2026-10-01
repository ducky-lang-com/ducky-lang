/* parser.c - recursive descent parser producing the Duck AST. */
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const SourceFile *src;
    Token *toks;
    int ntoks;
    int pos;
} Parser;

static Parser P;

static Token *peek(void) {
    return &P.toks[P.pos];
}

static Token *peek_at(int off) {
    int i = P.pos + off;
    if (i >= P.ntoks) i = P.ntoks - 1;
    return &P.toks[i];
}

static Token *next(void) {
    Token *t = &P.toks[P.pos];
    if (P.pos < P.ntoks - 1) P.pos++;
    return t;
}

static int check(TokenKind k) {
    return peek()->kind == k;
}

static Token *accept(TokenKind k) {
    if (check(k)) return next();
    return NULL;
}

/* How a token is shown inside parser error messages. */
static void describe(Token *t, char *buf, size_t buflen) {
    if (t->kind == TK_EOF) {
        snprintf(buf, buflen, "end of file");
    } else {
        snprintf(buf, buflen, "'%.*s'", t->len, t->text);
    }
}

/* The retired English keywords and the Duck-native word that replaced
 * them. The old words are ordinary identifiers now; when their use breaks
 * the syntax we point the user at the new spelling. */
static const char *retired_keyword(const char *name) {
    if (strcmp(name, "fn") == 0) return "wing";
    if (strcmp(name, "let") == 0) return "nest";
    if (strcmp(name, "if") == 0) return "when";
    if (strcmp(name, "else") == 0) return "otherwise";
    if (strcmp(name, "return") == 0) return "send";
    if (strcmp(name, "print") == 0) return "serve";
    return NULL;
}

static Token *expect(TokenKind k, const char *what) {
    if (check(k)) return next();
    char got[64];
    describe(peek(), got, sizeof(got));
    fatal_at(P.src, peek()->line, peek()->col, "expected %s, found %s", what, got);
    return NULL; /* unreachable */
}

/* ---------- dynamic pointer vectors ------------------------------------ */

typedef struct {
    void **data;
    int len;
    int cap;
} Vec;

static void vec_push(Vec *v, void *item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        void **nd = realloc(v->data, (size_t)v->cap * sizeof(void *));
        if (!nd) fatal("out of memory");
        v->data = nd;
    }
    v->data[v->len++] = item;
}

static void *vec_finish(Vec *v, size_t elem_size) {
    if (v->len == 0) return NULL;
    void *arr = arena_alloc(elem_size * (size_t)v->len);
    memcpy(arr, v->data, elem_size * (size_t)v->len);
    free(v->data);
    v->data = NULL;
    v->len = v->cap = 0;
    return arr;
}

static Expr *new_expr(ExprKind kind, int line, int col) {
    Expr *e = arena_alloc(sizeof(Expr));
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->line = line;
    e->col = col;
    return e;
}

static Stmt *new_stmt(StmtKind kind, int line, int col) {
    Stmt *s = arena_alloc(sizeof(Stmt));
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->line = line;
    s->col = col;
    return s;
}

/* ---------- forward declarations ---------------------------------------- */

static Expr *parse_expr(void);
static Block *parse_block(void);
static Stmt *parse_stmt(void);

/* ---------- expressions -------------------------------------------------- */

static Type parse_type(void) {
    Token *t = peek();
    switch (t->kind) {
    case TK_KW_INT:    next(); return TY_INT;
    case TK_KW_BOOL:   next(); return TY_BOOL;
    case TK_KW_STRING: next(); return TY_STRING;
    default: {
        char got[64];
        describe(t, got, sizeof(got));
        fatal_at(P.src, t->line, t->col, "expected a type (int, bool or string), found %s", got);
    }
    }
    return TY_VOID; /* unreachable */
}

static Expr *parse_primary(void) {
    Token *t = peek();

    switch (t->kind) {
    case TK_INT: {
        next();
        Expr *e = new_expr(EX_INT, t->line, t->col);
        e->ival = t->ival;
        return e;
    }
    case TK_TRUE:
    case TK_FALSE: {
        next();
        Expr *e = new_expr(EX_BOOL, t->line, t->col);
        e->bval = t->kind == TK_TRUE;
        return e;
    }
    case TK_STRING: {
        next();
        Expr *e = new_expr(EX_STRING, t->line, t->col);
        e->sval = t->sval;
        return e;
    }
    case TK_IDENT: {
        next();
        Expr *e = new_expr(EX_VAR, t->line, t->col);
        e->var.name = t->name;
        return e;
    }
    case TK_LPAREN: {
        next();
        Expr *e = parse_expr();
        expect(TK_RPAREN, "')' to close the group");
        return e;
    }
    default: {
        char got[64];
        describe(t, got, sizeof(got));
        fatal_at(P.src, t->line, t->col, "expected an expression, found %s", got);
    }
    }
    return NULL; /* unreachable */
}

static Expr *parse_postfix(void) {
    Expr *e = parse_primary();

    while (check(TK_LPAREN)) {
        if (e->kind != EX_VAR) {
            fatal_at(P.src, peek()->line, peek()->col,
                     "only named functions can be called");
        }
        Token *paren = next();

        Vec args = {0};
        if (!check(TK_RPAREN)) {
            do {
                vec_push(&args, parse_expr());
            } while (accept(TK_COMMA));
        }
        expect(TK_RPAREN, "')' to close the argument list");

        Expr *call = new_expr(EX_CALL, e->line, e->col);
        call->call.name = e->var.name;
        call->call.nargs = args.len;
        call->call.args = vec_finish(&args, sizeof(Expr *));
        (void)paren;
        e = call;
    }
    return e;
}

static Expr *parse_unary(void) {
    Token *t = peek();
    if (t->kind == TK_MINUS || t->kind == TK_NOT) {
        next();
        Expr *operand = parse_unary();
        Expr *e = new_expr(EX_UNARY, t->line, t->col);
        e->unary.op = t->kind == TK_MINUS ? UOP_NEG : UOP_NOT;
        e->unary.operand = operand;
        return e;
    }
    return parse_postfix();
}

static Expr *parse_mul(void) {
    Expr *lhs = parse_unary();
    for (;;) {
        BinaryOp op;
        if (check(TK_STAR)) op = BOP_MUL;
        else if (check(TK_SLASH)) op = BOP_DIV;
        else if (check(TK_PERCENT)) op = BOP_MOD;
        else break;
        next();
        Expr *rhs = parse_unary();
        Expr *e = new_expr(EX_BINARY, lhs->line, lhs->col);
        e->binary.op = op;
        e->binary.lhs = lhs;
        e->binary.rhs = rhs;
        lhs = e;
    }
    return lhs;
}

static Expr *parse_add(void) {
    Expr *lhs = parse_mul();
    for (;;) {
        BinaryOp op;
        if (check(TK_PLUS)) op = BOP_ADD;
        else if (check(TK_MINUS)) op = BOP_SUB;
        else break;
        next();
        Expr *rhs = parse_mul();
        Expr *e = new_expr(EX_BINARY, lhs->line, lhs->col);
        e->binary.op = op;
        e->binary.lhs = lhs;
        e->binary.rhs = rhs;
        lhs = e;
    }
    return lhs;
}

static Expr *parse_rel(void) {
    Expr *lhs = parse_add();
    for (;;) {
        BinaryOp op;
        if (check(TK_LT)) op = BOP_LT;
        else if (check(TK_LE)) op = BOP_LE;
        else if (check(TK_GT)) op = BOP_GT;
        else if (check(TK_GE)) op = BOP_GE;
        else break;
        next();
        Expr *rhs = parse_add();
        Expr *e = new_expr(EX_BINARY, lhs->line, lhs->col);
        e->binary.op = op;
        e->binary.lhs = lhs;
        e->binary.rhs = rhs;
        lhs = e;
    }
    return lhs;
}

static Expr *parse_eq(void) {
    Expr *lhs = parse_rel();
    for (;;) {
        BinaryOp op;
        if (check(TK_EQ)) op = BOP_EQ;
        else if (check(TK_NE)) op = BOP_NE;
        else break;
        next();
        Expr *rhs = parse_rel();
        Expr *e = new_expr(EX_BINARY, lhs->line, lhs->col);
        e->binary.op = op;
        e->binary.lhs = lhs;
        e->binary.rhs = rhs;
        lhs = e;
    }
    return lhs;
}

static Expr *parse_and(void) {
    Expr *lhs = parse_eq();
    while (check(TK_AND)) {
        next();
        Expr *rhs = parse_eq();
        Expr *e = new_expr(EX_BINARY, lhs->line, lhs->col);
        e->binary.op = BOP_AND;
        e->binary.lhs = lhs;
        e->binary.rhs = rhs;
        lhs = e;
    }
    return lhs;
}

static Expr *parse_or(void) {
    Expr *lhs = parse_and();
    while (check(TK_OR)) {
        next();
        Expr *rhs = parse_and();
        Expr *e = new_expr(EX_BINARY, lhs->line, lhs->col);
        e->binary.op = BOP_OR;
        e->binary.lhs = lhs;
        e->binary.rhs = rhs;
        lhs = e;
    }
    return lhs;
}

static Expr *parse_expr(void) {
    return parse_or();
}

/* ---------- statements ---------------------------------------------------- */

static Block *finish_block(int line, int col) {
    Vec stmts = {0};
    while (!check(TK_RBRACE) && !check(TK_EOF)) {
        vec_push(&stmts, parse_stmt());
    }
    expect(TK_RBRACE, "'}' to close the block");

    Block *b = arena_alloc(sizeof(Block));
    memset(b, 0, sizeof(*b));
    b->line = line;
    b->col = col;
    b->nstmts = stmts.len;
    b->stmts = vec_finish(&stmts, sizeof(Stmt *));
    return b;
}

static Block *parse_block(void) {
    Token *lbrace = expect(TK_LBRACE, "'{' to open a block");
    return finish_block(lbrace->line, lbrace->col);
}

static Stmt *parse_let(void) {
    Token *kw = next(); /* let */
    Token *name = expect(TK_IDENT, "a variable name after 'nest'");

    Stmt *s = new_stmt(ST_LET, kw->line, kw->col);
    s->let.name = name->name;
    if (accept(TK_COLON)) {
        s->let.has_ann = 1;
        s->let.ann = parse_type();
    }
    expect(TK_ASSIGN, "'=' in a 'nest' declaration");
    s->let.init = parse_expr();
    expect(TK_SEMI, "';' after the declaration");
    return s;
}

static Stmt *parse_if(void) {
    Token *kw = next(); /* if */
    Stmt *s = new_stmt(ST_IF, kw->line, kw->col);
    s->ifs.cond = parse_expr();
    s->ifs.then_block = parse_block();
    if (accept(TK_ELSE)) {
        if (check(TK_IF)) {
            /* `else if`: wrap the nested if statement in a block so that the
             * else branch always has the same shape. */
            Stmt *nested = parse_if();
            Block *b = arena_alloc(sizeof(Block));
            memset(b, 0, sizeof(*b));
            b->line = nested->line;
            b->col = nested->col;
            b->nstmts = 1;
            b->stmts = arena_alloc(sizeof(Stmt *));
            b->stmts[0] = nested;
            s->ifs.else_block = b;
        } else {
            s->ifs.else_block = parse_block();
        }
    }
    return s;
}

static Stmt *parse_while(void) {
    Token *kw = next(); /* while */
    Stmt *s = new_stmt(ST_WHILE, kw->line, kw->col);
    s->whiles.cond = parse_expr();
    s->whiles.body = parse_block();
    return s;
}

static Stmt *parse_return(void) {
    Token *kw = next(); /* return */
    Stmt *s = new_stmt(ST_RETURN, kw->line, kw->col);
    if (!check(TK_SEMI)) s->value = parse_expr();
    expect(TK_SEMI, "';' after 'send'");
    return s;
}

static Stmt *parse_stmt(void) {
    Token *t = peek();

    switch (t->kind) {
    case TK_LBRACE: {
        Block *b = parse_block();
        Stmt *s = new_stmt(ST_BLOCK, b->line, b->col);
        s->block = b;
        return s;
    }
    case TK_LET:
        return parse_let();
    case TK_IF:
        return parse_if();
    case TK_WHILE:
        return parse_while();
    case TK_RETURN:
        return parse_return();
    case TK_IDENT:
        if (peek_at(1)->kind == TK_ASSIGN) {
            next(); /* name */
            Token *eq = next(); /* = */
            Stmt *s = new_stmt(ST_ASSIGN, t->line, t->col);
            s->assign.name = t->name;
            s->assign.value = parse_expr();
            expect(TK_SEMI, "';' after the assignment");
            (void)eq;
            return s;
        }
        break; /* fall through to expression statement */
    default:
        break;
    }

    /* An expression statement: if it does not end with ';' and it started
     * with one of the retired keywords, give a targeted hint instead of the
     * generic "expected ';'" message. */
    Token *start = peek();
    Expr *e = parse_expr();
    if (!check(TK_SEMI) && start->kind == TK_IDENT) {
        const char *new_word = retired_keyword(start->name);
        if (new_word) {
            fatal_at(P.src, start->line, start->col,
                     "'%s' is not a Duck keyword anymore - use '%s' instead",
                     start->name, new_word);
        }
    }
    expect(TK_SEMI, "';' after the expression");
    Stmt *s = new_stmt(ST_EXPR, e->line, e->col);
    s->expr = e;
    return s;
}

/* ---------- top level ------------------------------------------------------ */

static Func *parse_func(void) {
    Token *kw = expect(TK_FN, "'wing' to start a function declaration");
    Token *name = expect(TK_IDENT, "a function name");

    Func *f = arena_alloc(sizeof(Func));
    memset(f, 0, sizeof(*f));
    f->name = name->name;
    f->line = kw->line;
    f->col = kw->col;
    f->ret = TY_VOID;

    expect(TK_LPAREN, "'(' after the function name");

    Vec params = {0};
    if (!check(TK_RPAREN)) {
        do {
            Token *pname = expect(TK_IDENT, "a parameter name");
            expect(TK_COLON, "':' after the parameter name");
            Type ptype = parse_type();

            Param *p = arena_alloc(sizeof(Param));
            p->name = pname->name;
            p->type = ptype;
            p->line = pname->line;
            p->col = pname->col;
            vec_push(&params, p);
        } while (accept(TK_COMMA));
    }
    expect(TK_RPAREN, "')' after the parameter list");

    f->nparams = params.len;
    f->params = vec_finish(&params, sizeof(Param *));

    if (accept(TK_ARROW)) f->ret = parse_type();
    f->body = parse_block();
    return f;
}

Program *parse(const SourceFile *src, Token *toks, int ntoks) {
    P.src = src;
    P.toks = toks;
    P.ntoks = ntoks;
    P.pos = 0;

    Vec funcs = {0};
    while (!check(TK_EOF)) {
        if (!check(TK_FN)) {
            if (check(TK_IDENT)) {
                const char *new_word = retired_keyword(peek()->name);
                if (new_word) {
                    fatal_at(src, peek()->line, peek()->col,
                             "'%s' is not a Duck keyword anymore - use '%s' instead",
                             peek()->name, new_word);
                }
            }
            char got[64];
            describe(peek(), got, sizeof(got));
            fatal_at(src, peek()->line, peek()->col,
                     "expected a top-level function declaration ('wing'), found %s", got);
        }
        vec_push(&funcs, parse_func());
    }

    Program *prog = arena_alloc(sizeof(Program));
    prog->nfuncs = funcs.len;
    prog->funcs = vec_finish(&funcs, sizeof(Func *));
    if (!prog->funcs) {
        fatal_at(src, 1, 1, "expected at least one function declaration");
    }
    return prog;
}
