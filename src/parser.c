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

/* The retired words and the canonical spelling that replaced them. The old
 * words are ordinary identifiers now; when their use breaks the syntax we
 * point the user at the right spelling. */
static const char *retired_keyword(const char *name) {
    if (strcmp(name, "wing") == 0) return "fn";
    if (strcmp(name, "nest") == 0) return "let";
    if (strcmp(name, "when") == 0) return "if";
    if (strcmp(name, "otherwise") == 0) return "else";
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
    case TK_KW_FLOAT:  next(); return TY_FLOAT;
    case TK_KW_BOOL:   next(); return TY_BOOL;
    case TK_KW_STRING: next(); return TY_STRING;
    case TK_IDENT: {
        next();
        StructDecl *sd = struct_type_lookup(t->name);
        if (!sd) {
            fatal_at(P.src, t->line, t->col,
                     "unknown type '%s' - expected int, float, bool, string, [T] or a struct name",
                     t->name);
        }
        return sd->type;
    }
    case TK_LBRACKET: {
        next(); /* [ */
        Type elem = parse_type();
        expect(TK_RBRACKET, "']' to close the array type");
        Type arr = type_array_of(elem);
        if (arr == TY_VOID) {
            fatal_at(P.src, t->line, t->col,
                     "arrays of '%s' are not supported yet", type_name(elem));
        }
        return arr;
    }
    default: {
        char got[64];
        describe(t, got, sizeof(got));
        fatal_at(P.src, t->line, t->col,
                 "expected a type (int, float, bool, string, [T] or a struct name), found %s", got);
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
    case TK_FLOAT: {
        next();
        Expr *e = new_expr(EX_FLOAT, t->line, t->col);
        e->dval = t->dval;
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
    case TK_KW_INT:
    case TK_KW_FLOAT: {
        /* `int(x)` and `float(x)` are conversion builtins whose names are
         * also type keywords. They become ordinary calls when followed by
         * '(' (parse_postfix turns the variable into a call). */
        if (peek_at(1)->kind != TK_LPAREN) {
            const char *kw = t->kind == TK_KW_INT ? "int" : "float";
            fatal_at(P.src, t->line, t->col,
                     "'%s' is a type name - use %s(x) to convert a value", kw, kw);
        }
        next();
        Expr *e = new_expr(EX_VAR, t->line, t->col);
        e->var.name = t->kind == TK_KW_INT ? "int" : "float";
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
    case TK_LBRACKET: {
        Token *lb = next(); /* [ */
        Vec elems = {0};
        if (check(TK_RBRACKET)) {
            /* An empty literal is only usable with an explicit annotation:
             * `let xs: [int] = [];` (sema checks the annotation). */
            next();
            Expr *e = new_expr(EX_ARRAY, lb->line, lb->col);
            e->array.nelems = 0;
            return e;
        }
        do {
            vec_push(&elems, parse_expr());
        } while (accept(TK_COMMA));
        expect(TK_RBRACKET, "']' to close the array literal");
        Expr *e = new_expr(EX_ARRAY, lb->line, lb->col);
        e->array.nelems = elems.len;
        e->array.elems = vec_finish(&elems, sizeof(Expr *));
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

    for (;;) {
        if (check(TK_LPAREN)) {
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
        } else if (check(TK_LBRACKET)) {
            next(); /* [ */
            Expr *idx = parse_expr();
            expect(TK_RBRACKET, "']' to close the index");
            Expr *x = new_expr(EX_INDEX, e->line, e->col);
            x->index.obj = e;
            x->index.idx = idx;
            e = x;
        } else if (check(TK_DOT)) {
            next(); /* . */
            Token *fname = expect(TK_IDENT, "a field name after '.'");
            Expr *x = new_expr(EX_FIELD, e->line, e->col);
            x->field.obj = e;
            x->field.name = fname->name;
            e = x;
        } else {
            return e;
        }
    }
}

static Expr *parse_unary(void) {
    Token *t = peek();
    if (t->kind == TK_MINUS || t->kind == TK_NOT || t->kind == TK_TILDE) {
        next();
        Expr *operand = parse_unary();
        Expr *e = new_expr(EX_UNARY, t->line, t->col);
        e->unary.op = t->kind == TK_MINUS   ? UOP_NEG
                      : t->kind == TK_NOT   ? UOP_NOT
                                            : UOP_BITNOT;
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

static Expr *parse_shift(void) {
    Expr *lhs = parse_add();
    for (;;) {
        BinaryOp op;
        if (check(TK_SHL)) op = BOP_SHL;
        else if (check(TK_SHR)) op = BOP_SHR;
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

static Expr *parse_rel(void) {
    Expr *lhs = parse_shift();
    for (;;) {
        BinaryOp op;
        if (check(TK_LT)) op = BOP_LT;
        else if (check(TK_LE)) op = BOP_LE;
        else if (check(TK_GT)) op = BOP_GT;
        else if (check(TK_GE)) op = BOP_GE;
        else break;
        next();
        Expr *rhs = parse_shift();
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

static Expr *parse_bitand(void) {
    Expr *lhs = parse_eq();
    while (check(TK_BITAND)) {
        next();
        Expr *rhs = parse_eq();
        Expr *e = new_expr(EX_BINARY, lhs->line, lhs->col);
        e->binary.op = BOP_BITAND;
        e->binary.lhs = lhs;
        e->binary.rhs = rhs;
        lhs = e;
    }
    return lhs;
}

static Expr *parse_bitxor(void) {
    Expr *lhs = parse_bitand();
    while (check(TK_XOR)) {
        next();
        Expr *rhs = parse_bitand();
        Expr *e = new_expr(EX_BINARY, lhs->line, lhs->col);
        e->binary.op = BOP_XOR;
        e->binary.lhs = lhs;
        e->binary.rhs = rhs;
        lhs = e;
    }
    return lhs;
}

static Expr *parse_bitor(void) {
    Expr *lhs = parse_bitxor();
    while (check(TK_BITOR)) {
        next();
        Expr *rhs = parse_bitxor();
        Expr *e = new_expr(EX_BINARY, lhs->line, lhs->col);
        e->binary.op = BOP_BITOR;
        e->binary.lhs = lhs;
        e->binary.rhs = rhs;
        lhs = e;
    }
    return lhs;
}

static Expr *parse_and(void) {
    Expr *lhs = parse_bitor();
    while (check(TK_AND)) {
        next();
        Expr *rhs = parse_bitor();
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
    Token *name = expect(TK_IDENT, "a variable name after 'let'");

    Stmt *s = new_stmt(ST_LET, kw->line, kw->col);
    s->let.name = name->name;
    if (accept(TK_COLON)) {
        s->let.has_ann = 1;
        s->let.ann = parse_type();
    }
    expect(TK_ASSIGN, "'=' in a 'let' declaration");
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
    Token *kw = next(); /* send */
    Stmt *s = new_stmt(ST_RETURN, kw->line, kw->col);
    if (!check(TK_SEMI)) s->value = parse_expr();
    expect(TK_SEMI, "';' after 'send'");
    return s;
}

static Stmt *parse_for(void) {
    Token *kw = next(); /* for */
    Stmt *s = new_stmt(ST_FOR, kw->line, kw->col);
    Token *var = expect(TK_IDENT, "a loop variable name after 'for'");
    expect(TK_IN, "'in' in a 'for' loop");
    s->fors.start = parse_expr();
    expect(TK_DOTDOT, "'..' between the bounds of a 'for' range");
    s->fors.end = parse_expr();
    s->fors.body = parse_block();
    s->fors.var_name = var->name;
    return s;
}

static Stmt *parse_break(void) {
    Token *kw = next();
    expect(TK_SEMI, "';' after 'break'");
    return new_stmt(ST_BREAK, kw->line, kw->col);
}

static Stmt *parse_continue(void) {
    Token *kw = next();
    expect(TK_SEMI, "';' after 'continue'");
    return new_stmt(ST_CONTINUE, kw->line, kw->col);
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
    case TK_FOR:
        return parse_for();
    case TK_BREAK:
        return parse_break();
    case TK_CONTINUE:
        return parse_continue();
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

    /* An expression statement: `xs[i] = v;` and `p.field = v;` are assignment
     * statements whose target comes out of the expression itself. If the
     * expression does not end with ';' and it started with one of the retired
     * keywords, give a targeted hint instead of the generic "expected ';'"
     * message. */
    Token *start = peek();
    Expr *e = parse_expr();
    if (accept(TK_ASSIGN)) {
        if (e->kind != EX_INDEX && e->kind != EX_FIELD) {
            fatal_at(P.src, e->line, e->col, "invalid assignment target");
        }
        Stmt *s = new_stmt(ST_ASSIGN, e->line, e->col);
        s->assign.target = e;
        s->assign.value = parse_expr();
        expect(TK_SEMI, "';' after the assignment");
        return s;
    }
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
    Token *kw = expect(TK_FN, "'fn' to start a function declaration");
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

static Const *parse_const(void) {
    Token *kw = next(); /* const */
    Token *name = expect(TK_IDENT, "a constant name after 'const'");
    expect(TK_ASSIGN, "'=' in a 'const' declaration");
    Expr *v = parse_expr();
    expect(TK_SEMI, "';' after the constant declaration");

    if (v->kind != EX_INT && v->kind != EX_FLOAT && v->kind != EX_BOOL &&
        v->kind != EX_STRING) {
        fatal_at(P.src, kw->line, kw->col,
                 "the initializer of '%s' must be a literal (int, float, bool or string)",
                 name->name);
    }

    Const *c = arena_alloc(sizeof(Const));
    c->name = name->name;
    c->value = v;
    c->line = kw->line;
    c->col = kw->col;
    return c;
}

/* `struct Name { field: Type, ... };` - fields are comma-separated and a
 * trailing comma is allowed. The declaration itself was already registered
 * by the pre-pass in parse(); this only fills in the fields. */
static StructDecl *parse_struct(void) {
    Token *kw = expect(TK_STRUCT, "'struct' to start a struct declaration");
    Token *name = expect(TK_IDENT, "a struct name after 'struct'");
    StructDecl *sd = struct_type_lookup(name->name);
    if (!sd) {
        fatal_at(P.src, name->line, name->col, "unknown struct '%s'", name->name);
    }
    sd->line = kw->line;
    sd->col = kw->col;

    expect(TK_LBRACE, "'{' to open the struct body");

    Vec fields = {0};
    while (!check(TK_RBRACE) && !check(TK_EOF)) {
        Token *fname = expect(TK_IDENT, "a field name");
        expect(TK_COLON, "':' after the field name");
        Type ft = parse_type();

        Field *f = arena_alloc(sizeof(Field));
        memset(f, 0, sizeof(*f));
        f->name = fname->name;
        f->type = ft;
        f->line = fname->line;
        f->col = fname->col;
        vec_push(&fields, f);
        if (!accept(TK_COMMA)) break;
    }
    expect(TK_RBRACE, "'}' to close the struct body");

    sd->nfields = fields.len;
    sd->fields = vec_finish(&fields, sizeof(Field *));
    return sd;
}

Program *parse(const SourceFile *src, Token *toks, int ntoks) {
    P.src = src;
    P.toks = toks;
    P.ntoks = ntoks;
    P.pos = 0;

    /* Pre-pass: intern every struct name before parsing anything, so that a
     * struct can be used before its declaration and a repeated name is
     * reported at the second declaration. */
    for (int i = 0; i + 1 < ntoks; i++) {
        if (toks[i].kind == TK_STRUCT && toks[i + 1].kind == TK_IDENT) {
            if (struct_type_lookup(toks[i + 1].name)) {
                fatal_at(src, toks[i + 1].line, toks[i + 1].col,
                         "duplicate struct '%s'", toks[i + 1].name);
            }
            struct_type_intern(toks[i + 1].name);
        }
    }

    Vec funcs = {0};
    Vec consts = {0};
    Vec structs = {0};
    while (!check(TK_EOF)) {
        if (check(TK_CONST)) {
            vec_push(&consts, parse_const());
            continue;
        }
        if (check(TK_STRUCT)) {
            vec_push(&structs, parse_struct());
            continue;
        }
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
                     "expected a top-level declaration ('fn', 'struct' or 'const'), found %s", got);
        }
        vec_push(&funcs, parse_func());
    }

    Program *prog = arena_alloc(sizeof(Program));
    prog->nfuncs = funcs.len;
    prog->funcs = vec_finish(&funcs, sizeof(Func *));
    prog->nconsts = consts.len;
    prog->consts = vec_finish(&consts, sizeof(Const *));
    prog->nstructs = structs.len;
    prog->structs = vec_finish(&structs, sizeof(StructDecl *));
    if (!prog->funcs) {
        fatal_at(src, 1, 1, "expected at least one function declaration");
    }
    return prog;
}
