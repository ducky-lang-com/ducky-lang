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

static void err_at(const SourceFile *src, int line, int col, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fatal_at(src, line, col, "%s", buf);
}

static void err(int line, int col, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fatal_at(g_src, line, col, "%s", buf);
}

/* Report an error at a node's position, in the file the node came from.
 * Programs can be assembled from several source files with `import`, so the
 * file is part of the node and not a global property of the compilation. */
#define err_node(n, ...) err_at((n)->src, (n)->line, (n)->col, __VA_ARGS__)

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int is_builtin_name(const char *name) {
    return strcmp(name, "serve") == 0 || strcmp(name, "len") == 0 ||
           strcmp(name, "str") == 0 || strcmp(name, "input_line") == 0 ||
           strcmp(name, "int") == 0 || strcmp(name, "float") == 0 ||
           strcmp(name, "push") == 0;
}

static Func *find_func(Program *prog, const char *name) {
    for (int i = 0; i < prog->nfuncs; i++) {
        if (strcmp(prog->funcs[i]->name, name) == 0) return prog->funcs[i];
    }
    return NULL;
}

static Const *find_const(Program *prog, const char *name) {
    for (int i = 0; i < prog->nconsts; i++) {
        if (strcmp(prog->consts[i]->name, name) == 0) return prog->consts[i];
    }
    return NULL;
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

static Var *declare_var(const char *name, Type type, const SourceFile *src,
                        int line, int col) {
    if (find_var_current(name)) {
        err_at(src, line, col, "redefinition of '%s' in this scope", name);
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

    case EX_FLOAT:
        return e->type = TY_FLOAT;

    case EX_BOOL:
        return e->type = TY_BOOL;

    case EX_STRING:
        return e->type = TY_STRING;

    case EX_VAR: {
        Var *v = find_var(e->var.name);
        if (v) {
            e->var.offset = v->offset;
            return e->type = v->type;
        }
        Const *c = find_const(g_prog, e->var.name);
        if (c) {
            /* Replace the reference with the literal value in place, so
             * code generation needs no special case for constants. */
            Expr *lit = c->value;
            switch (lit->kind) {
            case EX_INT:   e->ival = lit->ival; break;
            case EX_FLOAT: e->dval = lit->dval; break;
            case EX_BOOL:  e->bval = lit->bval; break;
            default:       e->sval = lit->sval; break;
            }
            e->kind = lit->kind;
            return e->type = lit->type;
        }
        if (struct_type_lookup(e->var.name)) {
            err_node(e,
                "'%s' is a struct type - construct a value with %s(...)",
                e->var.name, e->var.name);
        }
        err_node(e, "undefined variable '%s'", e->var.name);
        return e->type = TY_VOID; /* unreachable */
    }

    case EX_UNARY: {
        Type t = check_expr(e->unary.operand);
        if (e->unary.op == UOP_NEG) {
            if (t != TY_INT && t != TY_FLOAT) {
                err_node(e, "unary '-' requires 'int' or 'float', found '%s'",
                    type_name(t));
            }
            return e->type = t;
        }
        if (e->unary.op == UOP_BITNOT) {
            if (t != TY_INT) {
                err_node(e, "unary '~' requires 'int', found '%s'", type_name(t));
            }
            return e->type = TY_INT;
        }
        if (t != TY_BOOL) {
            err_node(e, "unary '!' requires 'bool', found '%s'", type_name(t));
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
                err_node(e, "operator '%s' requires 'bool' operands, found '%s' and '%s'",
                    binary_op_name(op), type_name(l), type_name(r));
            }
            return e->type = TY_BOOL;

        case BOP_ADD:
            /* '+' adds two floats, adds two ints or concatenates two
             * strings; nothing is converted implicitly. */
            if (l == TY_FLOAT || r == TY_FLOAT) {
                if (l != TY_FLOAT || r != TY_FLOAT) {
                    err_node(e,
                        "operator '+' requires two 'float' values or two 'int' values - Duck has no implicit conversion (use float(x)), found '%s' and '%s'",
                        type_name(l), type_name(r));
                }
                return e->type = TY_FLOAT;
            }
            if (l == TY_STRING || r == TY_STRING) {
                if (l != TY_STRING || r != TY_STRING) {
                    err_node(e,
                        "operator '+' requires two 'string' values to concatenate or two 'int' values to add, found '%s' and '%s'",
                        type_name(l), type_name(r));
                }
                return e->type = TY_STRING;
            }
            if (l != TY_INT || r != TY_INT) {
                err_node(e, "operator '+' requires 'int' operands, found '%s' and '%s'",
                    type_name(l), type_name(r));
            }
            return e->type = TY_INT;

        case BOP_SUB:
        case BOP_MUL:
        case BOP_DIV:
            if (l == TY_FLOAT || r == TY_FLOAT) {
                if (l != TY_FLOAT || r != TY_FLOAT) {
                    err_node(e,
                        "operator '%s' requires two 'float' values or two 'int' values - Duck has no implicit conversion (use float(x)), found '%s' and '%s'",
                        binary_op_name(op), type_name(l), type_name(r));
                }
                return e->type = TY_FLOAT;
            }
            if (l != TY_INT || r != TY_INT) {
                err_node(e, "operator '%s' requires 'int' operands, found '%s' and '%s'",
                    binary_op_name(op), type_name(l), type_name(r));
            }
            return e->type = TY_INT;

        case BOP_MOD:
        case BOP_BITAND:
        case BOP_BITOR:
        case BOP_XOR:
        case BOP_SHL:
        case BOP_SHR:
            if (l != TY_INT || r != TY_INT) {
                err_node(e, "operator '%s' requires 'int' operands, found '%s' and '%s'",
                    binary_op_name(op), type_name(l), type_name(r));
            }
            return e->type = TY_INT;

        case BOP_LT:
        case BOP_LE:
        case BOP_GT:
        case BOP_GE:
            if (l == TY_FLOAT || r == TY_FLOAT) {
                if (l != TY_FLOAT || r != TY_FLOAT) {
                    err_node(e,
                        "operator '%s' requires two 'float' values or two 'int' values - Duck has no implicit conversion (use float(x)), found '%s' and '%s'",
                        binary_op_name(op), type_name(l), type_name(r));
                }
                return e->type = TY_BOOL;
            }
            if (l != TY_INT || r != TY_INT) {
                err_node(e, "operator '%s' requires 'int' operands, found '%s' and '%s'",
                    binary_op_name(op), type_name(l), type_name(r));
            }
            return e->type = TY_BOOL;

        case BOP_EQ:
        case BOP_NE:
            if (l != r) {
                err_node(e, "cannot compare a value of type '%s' with a value of type '%s'",
                    type_name(l), type_name(r));
            }
            if (l == TY_VOID) {
                err_node(e, "values of type 'void' cannot be compared");
            }
            if (type_is_array(l)) {
                err_node(e,
                    "arrays cannot be compared with '%s' - compare elements instead",
                    binary_op_name(op));
            }
            if (l >= TY_STRUCT_BASE) {
                err_node(e,
                    "struct values cannot be compared with '%s' - compare fields instead",
                    binary_op_name(op));
            }
            return e->type = TY_BOOL;
        }
        return e->type = TY_VOID; /* unreachable */
    }

    case EX_ARRAY: {
        if (e->array.nelems == 0) {
            err_node(e,
                "cannot infer the type of an empty array literal - declare the type, as in 'let xs: [int] = []'");
        }
        Type et = check_expr(e->array.elems[0]);
        for (int i = 1; i < e->array.nelems; i++) {
            Type t = check_expr(e->array.elems[i]);
            if (t != et) {
                err_node(e->array.elems[i],
                    "array elements must all have the same type: found '%s' and '%s'",
                    type_name(et), type_name(t));
            }
        }
        if (et == TY_VOID) {
            err_node(e, "cannot store a value of type 'void' in an array");
        }
        if (type_is_array(et) || et >= TY_STRUCT_BASE) {
            err_node(e, "arrays of '%s' are not supported yet", type_name(et));
        }
        e->array.slot = alloc_slot();
        return e->type = type_array_of(et);
    }

    case EX_INDEX: {
        Type ot = check_expr(e->index.obj);
        Type it = check_expr(e->index.idx);
        if (it != TY_INT) {
            err_node(e->index.idx,
                "an index must be an 'int', found '%s'", type_name(it));
        }
        if (type_is_array(ot)) return e->type = type_elem(ot);
        if (ot == TY_STRING) return e->type = TY_STRING;
        err_node(e, "cannot index a value of type '%s'", type_name(ot));
        return e->type = TY_VOID; /* unreachable */
    }

    case EX_FIELD: {
        Type ot = check_expr(e->field.obj);
        StructDecl *sd = ot >= TY_STRUCT_BASE ? struct_type_decl(ot) : NULL;
        if (!sd) {
            err_node(e, "cannot access field '%s' on a value of type '%s'",
                e->field.name, type_name(ot));
        }
        for (int i = 0; i < sd->nfields; i++) {
            if (strcmp(sd->fields[i]->name, e->field.name) == 0) {
                e->field.offset = 8 * i;
                return e->type = sd->fields[i]->type;
            }
        }
        err_node(e, "'%s' has no field '%s'", sd->name, e->field.name);
        return e->type = TY_VOID; /* unreachable */
    }

    case EX_CALL: {
        const char *name = e->call.name;

        if (strcmp(name, "serve") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "serve() expects exactly 1 argument, found %d",
                    e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (t == TY_VOID || type_is_array(t) || t >= TY_STRUCT_BASE) {
                err_node(e, "cannot pass a value of type '%s' to serve()",
                    type_name(t));
            }
            e->call.builtin = BUILTIN_SERVE;
            return e->type = TY_VOID;
        }

        if (strcmp(name, "len") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "len() expects exactly 1 argument, found %d",
                    e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (t != TY_STRING && !type_is_array(t)) {
                err_node(e->call.args[0],
                    "len() expects a 'string' or an array, found '%s'", type_name(t));
            }
            e->call.builtin = BUILTIN_LEN;
            return e->type = TY_INT;
        }

        if (strcmp(name, "str") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "str() expects exactly 1 argument, found %d",
                    e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (t != TY_INT && t != TY_BOOL && t != TY_FLOAT) {
                err_node(e->call.args[0],
                    "str() expects an 'int', a 'bool' or a 'float', found '%s'",
                    type_name(t));
            }
            e->call.builtin = BUILTIN_STR;
            return e->type = TY_STRING;
        }

        if (strcmp(name, "input_line") == 0) {
            if (e->call.nargs != 0) {
                err_node(e, "input_line() expects no arguments, found %d",
                    e->call.nargs);
            }
            e->call.builtin = BUILTIN_INPUT;
            return e->type = TY_STRING;
        }

        if (strcmp(name, "int") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "int() expects exactly 1 argument, found %d",
                    e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (t != TY_FLOAT) {
                err_node(e->call.args[0],
                    "int() expects a 'float', found '%s'", type_name(t));
            }
            e->call.builtin = BUILTIN_INT;
            return e->type = TY_INT;
        }

        if (strcmp(name, "float") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "float() expects exactly 1 argument, found %d",
                    e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (t != TY_INT) {
                err_node(e->call.args[0],
                    "float() expects an 'int', found '%s'", type_name(t));
            }
            e->call.builtin = BUILTIN_FLOAT;
            return e->type = TY_FLOAT;
        }

        if (strcmp(name, "push") == 0) {
            if (e->call.nargs != 2) {
                err_node(e, "push() expects exactly 2 arguments, found %d",
                    e->call.nargs);
            }
            Type at = check_expr(e->call.args[0]);
            if (!type_is_array(at)) {
                err_node(e->call.args[0],
                    "push() expects an array as its first argument, found '%s'",
                    type_name(at));
            }
            Type vt = check_expr(e->call.args[1]);
            if (vt != type_elem(at)) {
                err_node(e->call.args[1],
                    "push() expects a '%s' value to append, found '%s'",
                    type_name(type_elem(at)), type_name(vt));
            }
            e->call.builtin = BUILTIN_PUSH;
            return e->type = at;
        }

        /* A struct constructor: `Point(1, 2)` resolves against the registry
         * (interned by the parser pre-pass), not the function table. */
        StructDecl *sdef = struct_type_lookup(name);
        if (sdef) {
            if (e->call.nargs != sdef->nfields) {
                err_node(e,
                    "struct constructor '%s' expects %d argument%s, found %d",
                    name, sdef->nfields, sdef->nfields == 1 ? "" : "s",
                    e->call.nargs);
            }
            for (int i = 0; i < e->call.nargs; i++) {
                Type t = check_expr(e->call.args[i]);
                if (t != sdef->fields[i]->type) {
                    err_node(e->call.args[i],
                        "field '%s' of '%s' expects '%s', found '%s'",
                        sdef->fields[i]->name, name,
                        type_name(sdef->fields[i]->type), type_name(t));
                }
            }
            e->call.sdef = sdef;
            e->call.slot = alloc_slot(); /* hidden slot holding the block */
            return e->type = sdef->type;
        }

        /* Resolved against the global function table collected in analyze(). */
        Func *fn = find_func(g_prog, name);
        if (!fn) {
            if (find_var(name)) {
                err_node(e, "'%s' is a variable, not a function", name);
            }
            if (strcmp(name, "print") == 0) {
                err_node(e,
                    "'print' does not exist in Duck - the output builtin is 'serve'");
            }
            err_node(e, "undefined function '%s'", name);
        }

        if (e->call.nargs != fn->nparams) {
            err_node(e, "function '%s' expects %d argument%s, found %d", fn->name,
                fn->nparams, fn->nparams == 1 ? "" : "s", e->call.nargs);
        }
        for (int i = 0; i < e->call.nargs; i++) {
            Type t = check_expr(e->call.args[i]);
            if (t != fn->params[i]->type) {
                err_node(e->call.args[i],
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
        Type t;
        if (s->let.init->kind == EX_ARRAY && s->let.init->array.nelems == 0) {
            /* An empty literal only works with an explicit annotation. */
            if (!s->let.has_ann || !type_is_array(s->let.ann)) {
                err_node(s,
                    "cannot infer the type of an empty array literal - declare the type, as in 'let xs: [int] = []'");
            }
            t = s->let.ann;
            s->let.init->type = t;
            s->let.init->array.slot = alloc_slot();
        } else {
            t = check_expr(s->let.init);
            if (t == TY_VOID) {
                err_node(s, "cannot initialize '%s' with a value of type 'void'",
                    s->let.name);
            }
            if (s->let.has_ann && s->let.ann != t) {
                err_node(s,
                    "type mismatch: '%s' is declared as '%s' but the initializer has type '%s'",
                    s->let.name, type_name(s->let.ann), type_name(t));
            }
        }
        Var *v = declare_var(s->let.name, t, s->src, s->line, s->col);
        s->let.offset = v->offset;
        break;
    }

    case ST_ASSIGN: {
        if (s->assign.target) {
            Expr *tg = s->assign.target;
            if (tg->kind == EX_FIELD) {
                /* `p.field = v;` - resolve the field, then match the value. */
                check_expr(tg);
                Type t = check_expr(s->assign.value);
                if (t == TY_VOID) {
                    err_node(s, "cannot assign a value of type 'void'");
                }
                if (t != tg->type) {
                    err_node(s,
                        "type mismatch: cannot assign '%s' to field '%s' of type '%s'",
                        type_name(t), tg->field.name, type_name(tg->type));
                }
                break;
            }

            /* `xs[i] = v;` - the target must be an element of an array. */
            Type ot = check_expr(tg->index.obj);
            Type it = check_expr(tg->index.idx);
            if (it != TY_INT) {
                err_node(tg->index.idx,
                    "an index must be an 'int', found '%s'", type_name(it));
            }
            if (!type_is_array(ot)) {
                if (ot == TY_STRING) {
                    err_node(tg,
                        "a 'string' cannot be modified through an index - strings are immutable");
                }
                err_node(tg, "cannot assign to an element of type '%s'",
                    type_name(ot));
            }
            Type t = check_expr(s->assign.value);
            if (t == TY_VOID) {
                err_node(s, "cannot assign a value of type 'void'");
            }
            if (t != type_elem(ot)) {
                err_node(s,
                    "type mismatch: cannot assign '%s' to an element of type '%s'",
                    type_name(t), type_name(type_elem(ot)));
            }
            tg->type = type_elem(ot);
            break;
        }

        Var *v = find_var(s->assign.name);
        if (!v) {
            err_node(s, "undefined variable '%s'", s->assign.name);
        }
        Type t = check_expr(s->assign.value);
        if (t == TY_VOID) {
            err_node(s, "cannot assign a value of type 'void'");
        }
        if (t != v->type) {
            err_node(s, "type mismatch: cannot assign '%s' to variable '%s' of type '%s'",
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
            err_node(s->ifs.cond,
                "the condition of 'if' must have type 'bool', found '%s'", type_name(t));
        }
        check_block(s->ifs.then_block);
        if (s->ifs.else_block) check_block(s->ifs.else_block);
        break;
    }

    case ST_WHILE: {
        Type t = check_expr(s->whiles.cond);
        if (t != TY_BOOL) {
            err_node(s->whiles.cond,
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
            err_node(s->fors.start,
                "the start of a 'for' range must be 'int', found '%s'", type_name(st));
        }
        Type en = check_expr(s->fors.end);
        if (en != TY_INT) {
            err_node(s->fors.end,
                "the end of a 'for' range must be 'int', found '%s'", type_name(en));
        }
        s->fors.end_offset = alloc_slot(); /* hidden slot holding the range end */
        g_loops++;
        push_scope();
        Var *loop = declare_var(s->fors.var_name, TY_INT, s->src, s->line, s->col);
        s->fors.var_offset = loop->offset;
        check_block(s->fors.body);
        pop_scope();
        g_loops--;
        break;
    }

    case ST_BREAK:
        if (!g_loops) {
            err_node(s, "'break' can only be used inside a loop");
        }
        break;

    case ST_CONTINUE:
        if (!g_loops) {
            err_node(s, "'continue' can only be used inside a loop");
        }
        break;

    case ST_RETURN: {
        if (g_fn->ret == TY_VOID) {
            if (s->value) {
                err_node(s, "function '%s' has return type 'void' but returns a value",
                    g_fn->name);
            }
            break;
        }
        if (!s->value) {
            err_node(s, "missing return value: function '%s' returns '%s'", g_fn->name,
                type_name(g_fn->ret));
        }
        Type t = check_expr(s->value);
        if (t != g_fn->ret) {
            err_node(s, "return type mismatch: expected '%s', found '%s'",
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
        declare_var(f->params[i]->name, f->params[i]->type, f->params[i]->src,
                    f->params[i]->line, f->params[i]->col);
    }
    check_block(f->body);
    pop_scope();

    f->frame_size = (g_slots * 8 + 15) & ~15;

    if (f->ret != TY_VOID && !block_returns(f->body)) {
        err_at(f->src, f->body->line, f->body->col,
            "function '%s' must return a value of type '%s' on all execution paths", f->name,
            type_name(f->ret));
    }
}

/* ---------- entry point ------------------------------------------------------ */

void analyze(const SourceFile *src, Program *prog) {
    g_src = src;
    g_prog = prog;

    /* Pass 0: validate the top-level constants and give each literal its
     * type once, so references can be substituted with a plain copy. */
    for (int i = 0; i < prog->nconsts; i++) {
        Const *c = prog->consts[i];

        if (c->name[0] == '_' || starts_with(c->name, "duck_")) {
            err_node(c, "constant name '%s' is reserved", c->name);
        }
        if (is_builtin_name(c->name)) {
            err_node(c, "'%s' is a builtin function and cannot be redefined",
                c->name);
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(prog->consts[j]->name, c->name) == 0) {
                err_node(c, "duplicate constant '%s'", c->name);
            }
        }
        if (find_func(prog, c->name)) {
            err_node(c, "'%s' is already declared as a function", c->name);
        }
        switch (c->value->kind) {
        case EX_INT:   c->value->type = TY_INT; break;
        case EX_FLOAT: c->value->type = TY_FLOAT; break;
        case EX_BOOL:  c->value->type = TY_BOOL; break;
        default:       c->value->type = TY_STRING; break;
        }
    }

    /* Pass 0.5: validate the struct declarations (duplicate struct names are
     * already rejected by the parser pre-pass). */
    for (int i = 0; i < prog->nstructs; i++) {
        StructDecl *sd = prog->structs[i];

        if (sd->name[0] == '_' || starts_with(sd->name, "duck_")) {
            err_node(sd, "struct name '%s' is reserved", sd->name);
        }
        if (is_builtin_name(sd->name)) {
            err_node(sd, "'%s' is a builtin function and cannot be redefined",
                sd->name);
        }
        if (find_func(prog, sd->name)) {
            err_node(sd, "'%s' is already declared as a function", sd->name);
        }
        if (find_const(prog, sd->name)) {
            err_node(sd, "'%s' is already declared as a constant", sd->name);
        }
        for (int j = 0; j < sd->nfields; j++) {
            Field *f = sd->fields[j];
            if (f->name[0] == '_' || starts_with(f->name, "duck_")) {
                err_node(f, "field name '%s' is reserved", f->name);
            }
            for (int k = 0; k < j; k++) {
                if (strcmp(sd->fields[k]->name, f->name) == 0) {
                    err_node(f, "duplicate field '%s' in struct '%s'",
                        f->name, sd->name);
                }
            }
        }
    }

    /* Pass 1: collect every function signature so that call order does not
     * matter (mutual recursion works). */
    for (int i = 0; i < prog->nfuncs; i++) {
        Func *f = prog->funcs[i];

        if (f->name[0] == '_' || starts_with(f->name, "duck_")) {
            err_node(f, "function name '%s' is reserved", f->name);
        }
        if (is_builtin_name(f->name)) {
            err_node(f, "'%s' is a builtin function and cannot be redefined", f->name);
        }
        if (find_const(prog, f->name)) {
            err_node(f, "'%s' is already declared as a constant", f->name);
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(prog->funcs[j]->name, f->name) == 0) {
                err_node(f, "duplicate function '%s'", f->name);
            }
        }
        for (int j = 0; j < f->nparams; j++) {
            for (int k = 0; k < j; k++) {
                if (strcmp(f->params[j]->name, f->params[k]->name) == 0) {
                    err_node(f->params[j], "duplicate parameter '%s' in function '%s'",
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
        err_node(entry, "the entry point must be declared as 'fn main() -> int'");
    }

    /* Pass 2: check every body. */
    for (int i = 0; i < prog->nfuncs; i++) check_func(prog->funcs[i]);
}
