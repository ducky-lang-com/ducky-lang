/* sema.c - name resolution, type checking and stack slot assignment. */
#include "sema.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- symbol tables ------------------------------------------------ */

typedef struct Var {
    const char *name;
    Type type;
    int offset; /* rbp-relative stack slot */
    struct Var *next;
} Var;

typedef struct Scope {
    struct Scope *parent;
    Var *vars;
} Scope;

static const SourceFile *g_src;
static Program *g_prog;
static Func *g_fn;
static Scope *g_scope;
static int g_slots; /* parameters + locals declared so far */
static int g_loops; /* loop nesting depth, for break/continue checks */

static void err(int line, int col, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fatal_at(g_src, line, col, "%s", buf);
}

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static Func *find_func(Program *prog, const char *name) {
    for (int i = 0; i < prog->nfuncs; i++) {
        if (strcmp(prog->funcs[i]->name, name) == 0) return prog->funcs[i];
    }
    return NULL;
}

static int is_builtin_name(const char *name) {
    return strcmp(name, "serve") == 0 || strcmp(name, "len") == 0 ||
           strcmp(name, "str") == 0 || strcmp(name, "input_line") == 0;
}

static void push_scope(void) {
    Scope *s = arena_alloc(sizeof(Scope));
    s->parent = g_scope;
    s->vars = NULL;
    g_scope = s;
}

static void pop_scope(void) {
    g_scope = g_scope->parent;
}

static Var *find_var(const char *name) {
    for (Scope *sc = g_scope; sc; sc = sc->parent) {
        for (Var *v = sc->vars; v; v = v->next) {
            if (strcmp(v->name, name) == 0) return v;
        }
    }
    return NULL;
}

static Var *find_var_current(const char *name) {
    if (!g_scope) return NULL;
    for (Var *v = g_scope->vars; v; v = v->next) {
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

/* Reserve the next 8-byte stack slot (no name: used for hidden storage
 * such as the end bound of a `for` range). */
static int alloc_slot(void) {
    g_slots++;
    return -8 * g_slots;
}

static Var *declare_var(const char *name, Type type, int line, int col) {
    if (find_var_current(name)) {
        err(line, col, "redefinition of '%s' in this scope", name);
    }
    Var *v = arena_alloc(sizeof(Var));
    v->name = name;
    v->type = type;
    v->offset = alloc_slot();
    v->next = g_scope->vars;
    g_scope->vars = v;
    return v;
}

/* ---------- definite return analysis -------------------------------------- */

static int block_returns(Block *b);

static int stmt_returns(Stmt *s) {
    switch (s->kind) {
    case ST_RETURN:
        return 1;
    case ST_BLOCK:
        return block_returns(s->block);
    case ST_IF:
        if (!s->ifs.else_block) return 0;
        return block_returns(s->ifs.then_block) && block_returns(s->ifs.else_block);
    default:
        return 0;
    }
}

/* A block definitely returns as soon as one of its statements does: the
 * statements after it are unreachable. */
static int block_returns(Block *b) {
    for (int i = 0; i < b->nstmts; i++) {
        if (stmt_returns(b->stmts[i])) return 1;
    }
    return 0;
}

/* ---------- expressions ---------------------------------------------------- */

static Type check_expr(Expr *e) {
    switch (e->kind) {
    case EX_INT:
        return e->type = TY_INT;

    case EX_BOOL:
        return e->type = TY_BOOL;

    case EX_STRING:
        return e->type = TY_STRING;

    case EX_VAR: {
        Var *v = find_var(e->var.name);
        if (!v) {
            err(e->line, e->col, "undefined variable '%s'", e->var.name);
        }
        e->var.offset = v->offset;
        return e->type = v->type;
    }

    case EX_UNARY: {
        Type t = check_expr(e->unary.operand);
        if (e->unary.op == UOP_NEG || e->unary.op == UOP_BITNOT) {
            if (t != TY_INT) {
                err(e->line, e->col, "unary '%s' requires 'int', found '%s'",
                    unary_op_name(e->unary.op), type_name(t));
            }
            return e->type = TY_INT;
        }
        if (t != TY_BOOL) {
            err(e->line, e->col, "unary '!' requires 'bool', found '%s'", type_name(t));
        }
        return e->type = TY_BOOL;
    }

    case EX_BINARY: {
        Type l = check_expr(e->binary.lhs);
        Type r = check_expr(e->binary.rhs);
        BinaryOp op = e->binary.op;

        switch (op) {
        case BOP_AND:
        case BOP_OR:
            if (l != TY_BOOL || r != TY_BOOL) {
                err(e->line, e->col, "operator '%s' requires 'bool' operands, found '%s' and '%s'",
                    binary_op_name(op), type_name(l), type_name(r));
            }
            return e->type = TY_BOOL;

        case BOP_ADD:
            /* '+' adds two ints or concatenates two strings; nothing is
             * converted implicitly. */
            if (l == TY_STRING || r == TY_STRING) {
                if (l != TY_STRING || r != TY_STRING) {
                    err(e->line, e->col,
                        "operator '+' requires two 'string' values to concatenate or two 'int' values to add, found '%s' and '%s'",
                        type_name(l), type_name(r));
                }
                return e->type = TY_STRING;
            }
            if (l != TY_INT || r != TY_INT) {
                err(e->line, e->col, "operator '+' requires 'int' operands, found '%s' and '%s'",
                    type_name(l), type_name(r));
            }
            return e->type = TY_INT;

        case BOP_SUB:
        case BOP_MUL:
        case BOP_DIV:
        case BOP_MOD:
        case BOP_BITAND:
        case BOP_BITOR:
        case BOP_XOR:
        case BOP_SHL:
        case BOP_SHR:
            if (l != TY_INT || r != TY_INT) {
                err(e->line, e->col, "operator '%s' requires 'int' operands, found '%s' and '%s'",
                    binary_op_name(op), type_name(l), type_name(r));
            }
            return e->type = TY_INT;

        case BOP_LT:
        case BOP_LE:
        case BOP_GT:
        case BOP_GE:
            if (l != TY_INT || r != TY_INT) {
                err(e->line, e->col, "operator '%s' requires 'int' operands, found '%s' and '%s'",
                    binary_op_name(op), type_name(l), type_name(r));
            }
            return e->type = TY_BOOL;

        case BOP_EQ:
        case BOP_NE:
            if (l != r) {
                err(e->line, e->col, "cannot compare a value of type '%s' with a value of type '%s'",
                    type_name(l), type_name(r));
            }
            if (l == TY_VOID) {
                err(e->line, e->col, "values of type 'void' cannot be compared");
            }
            return e->type = TY_BOOL;
        }
        return e->type = TY_VOID; /* unreachable */
    }

    case EX_CALL: {
        const char *name = e->call.name;

        if (strcmp(name, "serve") == 0) {
            if (e->call.nargs != 1) {
                err(e->line, e->col, "serve() expects exactly 1 argument, found %d",
                    e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (t == TY_VOID) err(e->line, e->col, "cannot pass a value of type 'void' to serve()");
            e->call.builtin = BUILTIN_SERVE;
            return e->type = TY_VOID;
        }

        if (strcmp(name, "len") == 0) {
            if (e->call.nargs != 1) {
                err(e->line, e->col, "len() expects exactly 1 argument, found %d",
                    e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (t != TY_STRING) {
                err(e->call.args[0]->line, e->call.args[0]->col,
                    "len() expects a 'string', found '%s'", type_name(t));
            }
            e->call.builtin = BUILTIN_LEN;
            return e->type = TY_INT;
        }

        if (strcmp(name, "str") == 0) {
            if (e->call.nargs != 1) {
                err(e->line, e->col, "str() expects exactly 1 argument, found %d",
                    e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (t != TY_INT && t != TY_BOOL) {
                err(e->call.args[0]->line, e->call.args[0]->col,
                    "str() expects an 'int' or a 'bool', found '%s'", type_name(t));
            }
            e->call.builtin = BUILTIN_STR;
            return e->type = TY_STRING;
        }

        if (strcmp(name, "input_line") == 0) {
            if (e->call.nargs != 0) {
                err(e->line, e->col, "input_line() expects no arguments, found %d",
                    e->call.nargs);
            }
            e->call.builtin = BUILTIN_INPUT;
            return e->type = TY_STRING;
        }

        /* Resolved against the global function table collected in analyze(). */
        Func *fn = find_func(g_prog, name);
        if (!fn) {
            if (find_var(name)) {
                err(e->line, e->col, "'%s' is a variable, not a function", name);
            }
            if (strcmp(name, "print") == 0) {
                err(e->line, e->col,
                    "'print' does not exist in Duck - the output builtin is 'serve'");
            }
            err(e->line, e->col, "undefined function '%s'", name);
        }

        if (e->call.nargs != fn->nparams) {
            err(e->line, e->col, "function '%s' expects %d argument%s, found %d", fn->name,
                fn->nparams, fn->nparams == 1 ? "" : "s", e->call.nargs);
        }
        for (int i = 0; i < e->call.nargs; i++) {
            Type t = check_expr(e->call.args[i]);
            if (t != fn->params[i]->type) {
                err(e->call.args[i]->line, e->call.args[i]->col,
                    "argument %d of '%s' expects '%s', found '%s'", i + 1, fn->name,
                    type_name(fn->params[i]->type), type_name(t));
            }
        }
        e->call.fn = fn;
        return e->type = fn->ret;
    }
    }
    return e->type = TY_VOID; /* unreachable */
}

/* ---------- statements ------------------------------------------------------ */

static void check_block(Block *b);

static void check_stmt(Stmt *s) {
    switch (s->kind) {
    case ST_BLOCK:
        check_block(s->block);
        break;

    case ST_LET: {
        Type t = check_expr(s->let.init);
        if (t == TY_VOID) {
            err(s->line, s->col, "cannot initialize '%s' with a value of type 'void'",
                s->let.name);
        }
        if (s->let.has_ann && s->let.ann != t) {
            err(s->line, s->col,
                "type mismatch: '%s' is declared as '%s' but the initializer has type '%s'",
                s->let.name, type_name(s->let.ann), type_name(t));
        }
        Var *v = declare_var(s->let.name, t, s->line, s->col);
        s->let.offset = v->offset;
        break;
    }

    case ST_ASSIGN: {
        Var *v = find_var(s->assign.name);
        if (!v) {
            err(s->line, s->col, "undefined variable '%s'", s->assign.name);
        }
        Type t = check_expr(s->assign.value);
        if (t == TY_VOID) {
            err(s->line, s->col, "cannot assign a value of type 'void'");
        }
        if (t != v->type) {
            err(s->line, s->col, "type mismatch: cannot assign '%s' to variable '%s' of type '%s'",
                type_name(t), s->assign.name, type_name(v->type));
        }
        s->assign.offset = v->offset;
        break;
    }

    case ST_EXPR:
        check_expr(s->expr);
        break;

    case ST_IF: {
        Type t = check_expr(s->ifs.cond);
        if (t != TY_BOOL) {
            err(s->ifs.cond->line, s->ifs.cond->col,
                "the condition of 'if' must have type 'bool', found '%s'", type_name(t));
        }
        check_block(s->ifs.then_block);
        if (s->ifs.else_block) check_block(s->ifs.else_block);
        break;
    }

    case ST_WHILE: {
        Type t = check_expr(s->whiles.cond);
        if (t != TY_BOOL) {
            err(s->whiles.cond->line, s->whiles.cond->col,
                "the condition of 'while' must have type 'bool', found '%s'", type_name(t));
        }
        g_loops++;
        check_block(s->whiles.body);
        g_loops--;
        break;
    }

    case ST_FOR: {
        Type st = check_expr(s->fors.start);
        if (st != TY_INT) {
            err(s->fors.start->line, s->fors.start->col,
                "the start of a 'for' range must be 'int', found '%s'", type_name(st));
        }
        Type en = check_expr(s->fors.end);
        if (en != TY_INT) {
            err(s->fors.end->line, s->fors.end->col,
                "the end of a 'for' range must be 'int', found '%s'", type_name(en));
        }
        s->fors.end_offset = alloc_slot(); /* hidden slot holding the range end */
        g_loops++;
        push_scope();
        Var *loop = declare_var(s->fors.var_name, TY_INT, s->line, s->col);
        s->fors.var_offset = loop->offset;
        check_block(s->fors.body);
        pop_scope();
        g_loops--;
        break;
    }

    case ST_BREAK:
        if (!g_loops) {
            err(s->line, s->col, "'break' can only be used inside a loop");
        }
        break;

    case ST_CONTINUE:
        if (!g_loops) {
            err(s->line, s->col, "'continue' can only be used inside a loop");
        }
        break;

    case ST_RETURN: {
        if (g_fn->ret == TY_VOID) {
            if (s->value) {
                err(s->line, s->col, "function '%s' has return type 'void' but returns a value",
                    g_fn->name);
            }
            break;
        }
        if (!s->value) {
            err(s->line, s->col, "missing return value: function '%s' returns '%s'", g_fn->name,
                type_name(g_fn->ret));
        }
        Type t = check_expr(s->value);
        if (t != g_fn->ret) {
            err(s->line, s->col, "return type mismatch: expected '%s', found '%s'",
                type_name(g_fn->ret), type_name(t));
        }
        break;
    }
    }
}

static void check_block(Block *b) {
    push_scope();
    for (int i = 0; i < b->nstmts; i++) check_stmt(b->stmts[i]);
    pop_scope();
}

static void check_func(Func *f) {
    g_fn = f;
    g_scope = NULL;
    g_slots = 0;
    g_loops = 0;

    push_scope();
    for (int i = 0; i < f->nparams; i++) {
        declare_var(f->params[i]->name, f->params[i]->type, f->params[i]->line, f->params[i]->col);
    }
    check_block(f->body);
    pop_scope();

    f->frame_size = (g_slots * 8 + 15) & ~15;

    if (f->ret != TY_VOID && !block_returns(f->body)) {
        err(f->body->line, f->body->col,
            "function '%s' must return a value of type '%s' on all execution paths", f->name,
            type_name(f->ret));
    }
}

/* ---------- entry point ------------------------------------------------------ */

void analyze(const SourceFile *src, Program *prog) {
    g_src = src;
    g_prog = prog;

    /* Pass 1: collect every function signature so that call order does not
     * matter (mutual recursion works). */
    for (int i = 0; i < prog->nfuncs; i++) {
        Func *f = prog->funcs[i];

        if (f->name[0] == '_' || starts_with(f->name, "duck_")) {
            err(f->line, f->col, "function name '%s' is reserved", f->name);
        }
        if (is_builtin_name(f->name)) {
            err(f->line, f->col, "'%s' is a builtin function and cannot be redefined", f->name);
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(prog->funcs[j]->name, f->name) == 0) {
                err(f->line, f->col, "duplicate function '%s'", f->name);
            }
        }
        for (int j = 0; j < f->nparams; j++) {
            for (int k = 0; k < j; k++) {
                if (strcmp(f->params[j]->name, f->params[k]->name) == 0) {
                    err(f->params[j]->line, f->params[j]->col, "duplicate parameter '%s' in function '%s'",
                        f->params[j]->name, f->name);
                }
            }
        }
    }

    Func *entry = find_func(prog, "main");
    if (!entry) {
        err(1, 1, "the program must define an entry point: 'fn main() -> int'");
    }
    if (entry->nparams != 0 || entry->ret != TY_INT) {
        err(entry->line, entry->col, "the entry point must be declared as 'fn main() -> int'");
    }

    /* Pass 2: check every body. */
    for (int i = 0; i < prog->nfuncs; i++) check_func(prog->funcs[i]);
}
