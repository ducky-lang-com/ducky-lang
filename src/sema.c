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

/* Names that no declaration may take. These are the ones the language itself
 * depends on: shadowing `len` or `push` would silently break every array in
 * the program, so they are rejected outright.
 *
 * The rest of the builtin surface - `sum`, `max`, `rand`, `shape`, the
 * activations - is deliberately absent. They are ordinary words that programs
 * have always been free to use as their own, and a user definition of one
 * takes precedence over the builtin at every call site (see check_expr). */
static int is_reserved_builtin_name(const char *name) {
    return strcmp(name, "serve") == 0 || strcmp(name, "len") == 0 ||
           strcmp(name, "str") == 0 || strcmp(name, "input_line") == 0 ||
           strcmp(name, "int") == 0 || strcmp(name, "float") == 0 ||
           strcmp(name, "push") == 0 || strcmp(name, "scan_int") == 0 ||
           strcmp(name, "scan_float") == 0 ||
           strcmp(name, "scan_int_line") == 0 ||
           strcmp(name, "scan_float_line") == 0 ||
           strcmp(name, "tensor") == 0 ||
           strcmp(name, "grad") == 0 ||
           strcmp(name, "step") == 0 ||
           strcmp(name, "matmul_tn") == 0 ||
           strcmp(name, "matmul_nt") == 0 ||
           strcmp(name, "cross_entropy_grad") == 0;
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

static Type check_expr(Expr *e);
static void check_func(Func *f);
static Func *grad_build(Func *src, Expr *call);

/* The type obtained by indexing one dimension away from a tensor:
 * `tensor[2, 3][i]` has type `tensor[3]`, and `tensor[3][i]` is a plain
 * `float`. Both the reader and the assignment statement need this, so it
 * lives in one place. */
static Type tensor_tail(Type t) {
    TensorShape *ts = type_shape(t);
    if (!ts) return TY_VOID;
    if (ts->rank == 1) return TY_FLOAT;
    return tensor_type_intern(ts->dims + 1, ts->rank - 1)->type;
}

/* Type-check the shape argument of `tensor(...)`, `zeros(...)`, `ones(...)`
 * and `rand(...)`: an array literal whose elements are all compile-time
 * integers. The elements arrive already substituted when they name a `const`
 * (check_expr replaces constant references with their literal), which is
 * what makes `zeros([HIDDEN, 10])` work. */
static Type check_shape_arg(Expr *e) {
    if (e->kind == EX_ARRAY && e->array.nelems == 0) {
        err_node(e, "a tensor shape needs at least one dimension, as in [2, 3]");
    }
    if (e->kind == EX_ARRAY && e->array.nelems > DUCKY_MAX_RANK) {
        err_node(e, "a tensor shape may have at most %d dimensions, found %d",
                 DUCKY_MAX_RANK, e->array.nelems);
    }

    Type t = check_expr(e);
    if (e->kind != EX_ARRAY || !type_is_array(t)) {
        err_node(e, "the shape must be an array literal of integers, as in [2, 3]");
    }
    if (t != TY_ARR_INT) {
        err_node(e, "tensor dimensions must be integers, found '%s'", type_name(t));
    }

    int dims[DUCKY_MAX_RANK];
    for (int i = 0; i < e->array.nelems; i++) {
        Expr *el = e->array.elems[i];
        if (el->kind != EX_INT) {
            err_node(el,
                "tensor dimension %d must be a compile-time integer constant - a literal "
                "or a top-level 'const NAME = <int>;'", i + 1);
        }
        if (el->ival < 1) {
            err_node(el, "tensor dimensions must be positive, found %ld", el->ival);
        }
        dims[i] = (int)el->ival;
    }
    return tensor_type_intern(dims, e->array.nelems)->type;
}

/* Tensor arithmetic: '+', '-' and '*' accept two tensors of the *same*
 * shape, or one tensor and one float (broadcast over every element). No
 * other combination is allowed, so every shape mismatch is a compile error
 * instead of a run-time surprise. */
static Type check_tensor_arith(Expr *e, Type l, Type r, BinaryOp op) {
    if (type_is_tensor(l) && type_is_tensor(r)) {
        if (l != r) {
            err_node(e,
                "operator '%s' requires tensors of the same shape, found '%s' and '%s'",
                binary_op_name(op), type_name(l), type_name(r));
        }
        return l;
    }
    Type ts = type_is_tensor(l) ? l : r;
    Type scalar = type_is_tensor(l) ? r : l;
    if (scalar != TY_FLOAT) {
        err_node(e,
            "operator '%s' cannot combine '%s' and '%s' - a tensor combines with a "
            "tensor of the same shape or with a 'float'",
            binary_op_name(op), type_name(l), type_name(r));
    }
    return ts;
}

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
            if (t != TY_INT && t != TY_FLOAT && !type_is_tensor(t)) {
                err_node(e, "unary '-' requires 'int', 'float' or a tensor, found '%s'",
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

        /* Tensors take part in exactly four operators. Everything else -
         * comparisons, the remainder and the bitwise operators - stops here
         * so the message names tensors instead of falling through to the
         * integer rules below. */
        int arithmetic = op == BOP_ADD || op == BOP_SUB ||
                         op == BOP_MUL || op == BOP_DIV;
        if ((type_is_tensor(l) || type_is_tensor(r)) && !arithmetic) {
            err_node(e,
                "operator '%s' is not defined for tensors ('%s' and '%s') - "
                "tensors support '+', '-', '*', '/' and unary '-'",
                binary_op_name(op), type_name(l), type_name(r));
        }
        if (arithmetic && (type_is_tensor(l) || type_is_tensor(r))) {
            return e->type = check_tensor_arith(e, l, r, op);
        }

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
                        "operator '+' requires two 'float' values or two 'int' values - Ducky has no implicit conversion (use float(x)), found '%s' and '%s'",
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
                        "operator '%s' requires two 'float' values or two 'int' values - Ducky has no implicit conversion (use float(x)), found '%s' and '%s'",
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
                        "operator '%s' requires two 'float' values or two 'int' values - Ducky has no implicit conversion (use float(x)), found '%s' and '%s'",
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
            if (type_is_struct(l)) {
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
        if (type_is_array(et) || type_is_struct(et) || type_is_tensor(et)) {
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
        if (type_is_tensor(ot)) return e->type = tensor_tail(ot);
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

        /* `grad(f, x)` - the one special form in the language. `grad` is a
         * reserved name (see is_reserved_builtin_name), so nothing else can
         * be called `grad`, and the first argument is a *name*, not a value:
         * it is resolved to a function here, never evaluated. */
        if (strcmp(name, "grad") == 0) {
            if (e->call.nargs != 2) {
                err_node(e, "grad() expects exactly 2 arguments, found %d - "
                            "as in grad(f, x)",
                    e->call.nargs);
            }
            Expr *fnref = e->call.args[0];
            if (fnref->kind != EX_VAR) {
                err_node(fnref, "grad() expects the name of a function as its "
                                "first argument, as in grad(f, x)");
            }
            Func *target = find_func(g_prog, fnref->var.name);
            if (!target) {
                err_node(fnref, "grad() cannot find a function named '%s'",
                    fnref->var.name);
            }

            Type xt = check_expr(e->call.args[1]);

            Func *gf = grad_build(target, e);

            if (xt != gf->params[0]->type) {
                err_node(e->call.args[1],
                    "grad() cannot differentiate '%s' with respect to a value "
                    "of type '%s': '%s' takes '%s'",
                    target->name, type_name(xt), target->name,
                    type_name(gf->params[0]->type));
            }

            /* Rewrite the node in place into an ordinary call to the
             * synthesized `f$grad`. From here on sema and code generation
             * treat it as a plain function: the argument is the value being
             * differentiated and has already been checked. */
            {
                Expr **args = arena_alloc(sizeof(Expr *));
                args[0] = e->call.args[1];
                e->call.args = args;
                e->call.nargs = 1;
            }
            e->call.name = gf->name;
            e->call.builtin = BUILTIN_NONE;
            e->call.fn = gf;
            e->call.sdef = NULL;
            e->call.slot = 0;
            return e->type = gf->ret;
        }

        /* A user definition of the same name wins over the builtin. This is
         * what keeps `sum`, `rand`, `max` and `min` usable as ordinary
         * function names: they became builtins in v0.8.0, but programs have
         * always been allowed to declare their own, and reserving them now
         * would break every one of those programs on upgrade.
         *
         * The dispatch below is a chain of tests on `name`, so pointing it at
         * the empty string skips the whole chain in one place - nothing
         * matches "", and every path that uses `name` afterwards is only
         * reached when nothing matched. */
        if (g_prog && find_func(g_prog, name)) name = "";

        if (strcmp(name, "serve") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "serve() expects exactly 1 argument, found %d",
                    e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            /* Arrays, structs and tensors all print (v0.8.0 lifted the
             * restriction that used to sit on arrays and structs); only a
             * value that does not exist - void - is left. */
            if (t == TY_VOID) {
                err_node(e, "cannot pass a value of type 'void' to serve()");
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
            if (t != TY_STRING && !type_is_array(t) && !type_is_tensor(t)) {
                err_node(e->call.args[0],
                    "len() expects a 'string', an array or a tensor, found '%s'",
                    type_name(t));
            }
            /* For a tensor this is the outermost dimension, so
             * `for i in 0..len(t) { t[i] ... }` walks exactly one index. */
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

        if (strcmp(name, "scan_int") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "scan_int() expects exactly 1 argument, found %d",
                    e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (t != TY_STRING) {
                err_node(e->call.args[0],
                    "scan_int() expects a 'string', found '%s'", type_name(t));
            }
            e->call.builtin = BUILTIN_SCAN_INT;
            return e->type = TY_INT;
        }

        if (strcmp(name, "scan_float") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "scan_float() expects exactly 1 argument, found %d",
                    e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (t != TY_STRING) {
                err_node(e->call.args[0],
                    "scan_float() expects a 'string', found '%s'", type_name(t));
            }
            e->call.builtin = BUILTIN_SCAN_FLOAT;
            return e->type = TY_FLOAT;
        }

        if (strcmp(name, "scan_int_line") == 0) {
            if (e->call.nargs != 0) {
                err_node(e, "scan_int_line() expects no arguments, found %d",
                    e->call.nargs);
            }
            e->call.builtin = BUILTIN_SCAN_INT_LINE;
            return e->type = TY_INT;
        }

        if (strcmp(name, "scan_float_line") == 0) {
            if (e->call.nargs != 0) {
                err_node(e, "scan_float_line() expects no arguments, found %d",
                    e->call.nargs);
            }
            e->call.builtin = BUILTIN_SCAN_FLOAT_LINE;
            return e->type = TY_FLOAT;
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

        /* ---------- tensor builtins ------------------------------------
         * A tensor's shape is part of its type, so every one of these
         * resolves a compile-time shape here. Whatever the compiler knows at
         * this point is what it can reject before the program ever runs: a
         * matmul whose inner dimensions differ, a data literal that does not
         * fill its shape, an argument of the wrong rank. */

        if (strcmp(name, "tensor") == 0) {
            if (e->call.nargs != 2) {
                err_node(e,
                    "tensor() expects 2 arguments - the shape and the data - found %d",
                    e->call.nargs);
            }
            Type shape = check_shape_arg(e->call.args[0]);
            long n = tensor_nelems(shape);
            Expr *data = e->call.args[1];
            if (data->kind == EX_ARRAY && data->array.nelems == 0) {
                err_node(data, "the data literal is empty, but %s holds %ld elements",
                         type_name(shape), n);
            }
            Type dt = check_expr(data);
            if (dt != TY_ARR_FLOAT) {
                err_node(data, "tensor() expects the data to be a '[float]', found '%s'",
                         type_name(dt));
            }
            if (data->kind == EX_ARRAY && (long)data->array.nelems != n) {
                err_node(data, "%s holds %ld elements but the data literal has %d",
                         type_name(shape), n, data->array.nelems);
            }
            e->call.builtin = BUILTIN_TENSOR;
            return e->type = shape;
        }

        if (strcmp(name, "zeros") == 0 || strcmp(name, "ones") == 0 ||
            strcmp(name, "rand") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "%s() expects 1 argument - the shape - found %d",
                         name, e->call.nargs);
            }
            Type shape = check_shape_arg(e->call.args[0]);
            e->call.builtin = strcmp(name, "zeros") == 0  ? BUILTIN_ZEROS
                              : strcmp(name, "ones") == 0 ? BUILTIN_ONES
                                                          : BUILTIN_RAND;
            return e->type = shape;
        }

        if (strcmp(name, "seed") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "seed() expects exactly 1 argument, found %d",
                         e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (t != TY_INT) {
                err_node(e->call.args[0], "seed() expects an 'int', found '%s'",
                         type_name(t));
            }
            e->call.builtin = BUILTIN_SEED;
            return e->type = TY_VOID;
        }

        if (strcmp(name, "shape") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "shape() expects exactly 1 argument, found %d",
                         e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (!type_is_tensor(t)) {
                err_node(e->call.args[0], "shape() expects a tensor, found '%s'",
                         type_name(t));
            }
            e->call.builtin = BUILTIN_SHAPE;
            return e->type = TY_ARR_INT;
        }

        if (strcmp(name, "matmul") == 0) {
            if (e->call.nargs != 2) {
                err_node(e, "matmul() expects exactly 2 arguments, found %d",
                         e->call.nargs);
            }
            Type a = check_expr(e->call.args[0]);
            Type b = check_expr(e->call.args[1]);
            if (!type_is_tensor(a)) {
                err_node(e->call.args[0],
                    "matmul() expects a tensor as its first argument, found '%s'",
                    type_name(a));
            }
            if (!type_is_tensor(b)) {
                err_node(e->call.args[1],
                    "matmul() expects a tensor as its second argument, found '%s'",
                    type_name(b));
            }
            int ra = tensor_rank(a), rb = tensor_rank(b);
            if (ra != 2 || (rb != 2 && rb != 1)) {
                err_node(e,
                    "matmul() needs a rank-2 tensor on the left and a rank-2 "
                    "or rank-1 tensor on the right, found '%s' and '%s'",
                    type_name(a), type_name(b));
            }
            int ka = tensor_dim(a, 1), kb = tensor_dim(b, 0);
            if (ka != kb) {
                err_node(e,
                    "matmul cannot multiply '%s' by '%s': the inner dimensions "
                    "differ (%d and %d)",
                    type_name(a), type_name(b), ka, kb);
            }
            /* A rank-1 right operand is a single column, so the product is a
             * vector: the layout of tensor[k] is byte-identical to that of
             * tensor[k, 1] and the same kernel runs with n = 1. */
            int out[2] = { tensor_dim(a, 0), rb == 1 ? 1 : tensor_dim(b, 1) };
            e->call.builtin = BUILTIN_MATMUL;
            return e->type = tensor_type_intern(out, rb == 1 ? 1 : 2)->type;
        }

        if (strcmp(name, "dot") == 0) {
            if (e->call.nargs != 2) {
                err_node(e, "dot() expects exactly 2 arguments, found %d",
                         e->call.nargs);
            }
            Type a = check_expr(e->call.args[0]);
            Type b = check_expr(e->call.args[1]);
            if (!type_is_tensor(a)) {
                err_node(e->call.args[0],
                    "dot() expects a tensor as its first argument, found '%s'",
                    type_name(a));
            }
            if (!type_is_tensor(b)) {
                err_node(e->call.args[1],
                    "dot() expects a tensor as its second argument, found '%s'",
                    type_name(b));
            }
            if (tensor_rank(a) != 1 || tensor_rank(b) != 1) {
                err_node(e, "dot() needs two vectors, found '%s' and '%s' - "
                            "use matmul() for matrices",
                         type_name(a), type_name(b));
            }
            if (a != b) {
                err_node(e, "dot() needs two vectors of the same length, found "
                            "'%s' and '%s'",
                         type_name(a), type_name(b));
            }
            e->call.builtin = BUILTIN_DOT;
            return e->type = TY_FLOAT;
        }

        /* Activations: shape in, same shape out. */
        if (strcmp(name, "relu") == 0 || strcmp(name, "sigmoid") == 0 ||
            strcmp(name, "tanh") == 0 || strcmp(name, "gelu") == 0 ||
            strcmp(name, "softmax") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "%s() expects exactly 1 argument, found %d",
                         name, e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (!type_is_tensor(t)) {
                err_node(e->call.args[0], "%s() expects a tensor, found '%s'",
                         name, type_name(t));
            }
            e->call.builtin = strcmp(name, "relu") == 0      ? BUILTIN_RELU
                              : strcmp(name, "sigmoid") == 0 ? BUILTIN_SIGMOID
                              : strcmp(name, "tanh") == 0    ? BUILTIN_TANH
                              : strcmp(name, "gelu") == 0    ? BUILTIN_GELU
                                                            : BUILTIN_SOFTMAX;
            return e->type = t;
        }

        /* Reductions: a tensor becomes a scalar. */
        if (strcmp(name, "sum") == 0 || strcmp(name, "mean") == 0 ||
            strcmp(name, "max") == 0 || strcmp(name, "min") == 0 ||
            strcmp(name, "argmax") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "%s() expects exactly 1 argument, found %d",
                         name, e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (!type_is_tensor(t)) {
                err_node(e->call.args[0], "%s() expects a tensor, found '%s'",
                         name, type_name(t));
            }
            e->call.builtin = strcmp(name, "sum") == 0      ? BUILTIN_SUM
                              : strcmp(name, "mean") == 0   ? BUILTIN_MEAN
                              : strcmp(name, "max") == 0    ? BUILTIN_MAX
                              : strcmp(name, "min") == 0    ? BUILTIN_MIN
                                                            : BUILTIN_ARGMAX;
            return e->type = strcmp(name, "argmax") == 0 ? TY_INT : TY_FLOAT;
        }

        if (strcmp(name, "mse") == 0) {
            if (e->call.nargs != 2) {
                err_node(e, "mse() expects exactly 2 arguments, found %d",
                         e->call.nargs);
            }
            Type p = check_expr(e->call.args[0]);
            Type t = check_expr(e->call.args[1]);
            if (!type_is_tensor(p)) {
                err_node(e->call.args[0],
                    "mse() expects a tensor as its first argument, found '%s'",
                    type_name(p));
            }
            if (!type_is_tensor(t)) {
                err_node(e->call.args[1],
                    "mse() expects a tensor as its second argument, found '%s'",
                    type_name(t));
            }
            if (p != t) {
                err_node(e, "mse() needs two tensors of the same shape, found "
                            "'%s' and '%s'",
                         type_name(p), type_name(t));
            }
            e->call.builtin = BUILTIN_MSE;
            return e->type = TY_FLOAT;
        }

        if (strcmp(name, "cross_entropy") == 0) {
            if (e->call.nargs != 2) {
                err_node(e, "cross_entropy() expects exactly 2 arguments, found %d",
                         e->call.nargs);
            }
            Type l = check_expr(e->call.args[0]);
            Type t = check_expr(e->call.args[1]);
            if (!type_is_tensor(l)) {
                err_node(e->call.args[0],
                    "cross_entropy() expects a tensor of logits, found '%s'",
                    type_name(l));
            }
            if (tensor_rank(l) != 1) {
                err_node(e->call.args[0],
                    "cross_entropy() expects a rank-1 tensor of logits, found '%s'",
                    type_name(l));
            }
            if (t != TY_INT) {
                err_node(e->call.args[1],
                    "cross_entropy() expects the class index as an 'int', found '%s'",
                    type_name(t));
            }
            e->call.builtin = BUILTIN_CROSS_ENTROPY;
            return e->type = TY_FLOAT;
        }

        /* Differentiation (v0.9.0). `step` is the derivative of relu and an
         * activation in its own right; the two transposed products are what
         * the backward pass of matmul() is written with; and
         * cross_entropy_grad() is d/dlogits of cross_entropy(). The gradient
         * code sema emits for grad() refers to all four by name, which is
         * why they are reserved (see is_reserved_builtin_name). */
        if (strcmp(name, "step") == 0) {
            if (e->call.nargs != 1) {
                err_node(e, "step() expects exactly 1 argument, found %d",
                         e->call.nargs);
            }
            Type t = check_expr(e->call.args[0]);
            if (!type_is_tensor(t))
                err_node(e->call.args[0], "step() expects a tensor, found '%s'",
                         type_name(t));
            e->call.builtin = BUILTIN_STEP;
            return e->type = t;
        }

        if (strcmp(name, "cross_entropy_grad") == 0) {
            if (e->call.nargs != 2) {
                err_node(e,
                         "cross_entropy_grad() expects exactly 2 arguments, found %d",
                         e->call.nargs);
            }
            Type l = check_expr(e->call.args[0]);
            Type c = check_expr(e->call.args[1]);
            if (!type_is_tensor(l))
                err_node(e->call.args[0],
                         "cross_entropy_grad() expects a tensor of logits, found '%s'",
                         type_name(l));
            if (tensor_rank(l) != 1)
                err_node(e->call.args[0],
                         "cross_entropy_grad() expects a rank-1 tensor of logits, found '%s'",
                         type_name(l));
            if (c != TY_INT)
                err_node(e->call.args[1],
                         "cross_entropy_grad() expects the class index as an 'int', found '%s'",
                         type_name(c));
            e->call.builtin = BUILTIN_XENT_GRAD;
            return e->type = l;
        }

        if (strcmp(name, "matmul_tn") == 0 || strcmp(name, "matmul_nt") == 0) {
            int tn = strcmp(name, "matmul_tn") == 0;
            if (e->call.nargs != 2) {
                err_node(e, "%s() expects exactly 2 arguments, found %d", name,
                         e->call.nargs);
            }
            Type a = check_expr(e->call.args[0]);
            Type b = check_expr(e->call.args[1]);
            if (!type_is_tensor(a))
                err_node(e->call.args[0],
                         "%s() expects a tensor as its first argument, found '%s'",
                         name, type_name(a));
            if (!type_is_tensor(b))
                err_node(e->call.args[1],
                         "%s() expects a tensor as its second argument, found '%s'",
                         name, type_name(b));
            if (tn && tensor_rank(a) != 2)
                err_node(e->call.args[0],
                         "matmul_tn(A, B) computes A^T * B, so A must be rank 2, "
                         "found '%s'",
                         type_name(a));
            if (tensor_rank(a) > 2 || tensor_rank(b) > 2)
                err_node(e, "%s() needs rank-1 or rank-2 tensors, found '%s' "
                            "and '%s'",
                         name, type_name(a), type_name(b));
            /* A rank-1 operand is read as a single column: tensor[k] has the
             * same layout as tensor[k, 1], which is what makes both products
             * below fall out of one kernel per case. */
            int ra = tensor_rank(a), rb = tensor_rank(b);
            int m_a = tensor_dim(a, 0);
            int m_b = tensor_dim(b, 0);
            int out[2];
            int outrank;
            if (tn) {
                /* A (m,k) transposed against B (m,n) -> (k,n) */
                if (m_a != m_b)
                    err_node(e, "matmul_tn(A, B) needs the same leading "
                                "dimension, found %d in '%s' and %d in '%s'",
                             m_a, type_name(a), m_b, type_name(b));
                out[0] = tensor_dim(a, 1);
                out[1] = rb == 1 ? 1 : tensor_dim(b, 1);
                outrank = rb;
            } else {
                /* A (m,n) against B (k,n) transposed -> (m,k) */
                int ca = ra == 1 ? 1 : tensor_dim(a, 1);
                int cb = rb == 1 ? 1 : tensor_dim(b, 1);
                if (ca != cb)
                    err_node(e, "matmul_nt(A, B) needs the same number of "
                                "columns, found %d in '%s' and %d in '%s'",
                             ca, type_name(a), cb, type_name(b));
                out[0] = m_a;
                out[1] = m_b;
                outrank = 2;
            }
            e->call.builtin = tn ? BUILTIN_MATMUL_TN : BUILTIN_MATMUL_NT;
            return e->type = tensor_type_intern(out, outrank)->type;
        }

        /* Optimizers (v0.9.0). Each writes its update into the parameter
         * itself and evaluates to that parameter, so the call reads the same
         * way whether you use it as a statement or assign it back to the same
         * name. The distinction that matters is state: `sgd` has none and
         * works on a `float` too, while `momentum` and `adam` keep a running
         * average that has to live in memory the callee can reach - a
         * `float` argument would be a copy. */
        if (strcmp(name, "sgd") == 0 || strcmp(name, "momentum") == 0 ||
            strcmp(name, "adam") == 0) {
            int is_sgd = strcmp(name, "sgd") == 0;
            int is_mom = strcmp(name, "momentum") == 0;
            int nwant = is_sgd ? 3 : is_mom ? 5 : 6;
            if (e->call.nargs != nwant) {
                err_node(e, "%s() expects exactly %d arguments, found %d - as in "
                            "%s",
                         name, nwant, e->call.nargs,
                         is_sgd ? "sgd(w, g, lr)"
                                : is_mom ? "momentum(w, g, lr, v, beta)"
                                         : "adam(w, g, lr, m, v, t)");
            }
            Type p = check_expr(e->call.args[0]);
            if (p != TY_FLOAT && !type_is_tensor(p))
                err_node(e->call.args[0],
                         "%s() expects a 'float' or a tensor to update, found '%s'",
                         name, type_name(p));
            /* Before anything else: a parameter without a home for the state
             * is not a matter of shapes, and saying so first keeps the
             * diagnostic about the argument the reader has to change. */
            if (!is_sgd && p == TY_FLOAT)
                err_node(e->call.args[0],
                         "%s() needs a tensor: it keeps state, and a 'float' "
                         "argument would only carry a copy of it. For a scalar "
                         "parameter write `x = x - lr * g` by hand",
                         name);
            Type gt = check_expr(e->call.args[1]);
            Type lr = check_expr(e->call.args[2]);
            if (gt != p)
                err_node(e->call.args[1],
                         "%s() expects the gradient to have the parameter's type, "
                         "found '%s' for '%s'",
                         name, type_name(gt), type_name(p));
            if (lr != TY_FLOAT)
                err_node(e->call.args[2],
                         "%s() expects the learning rate as a 'float', found '%s' - "
                         "as in %s(w, g, 0.01)",
                         name, type_name(lr), name);
            if (!is_sgd) {
                Type s = check_expr(e->call.args[3]);
                if (s != p)
                    err_node(e->call.args[3],
                             "%s() expects the state to have the parameter's type, "
                             "found '%s' for '%s'",
                             name, type_name(s), type_name(p));
            }
            if (is_mom) {
                Type b = check_expr(e->call.args[4]);
                if (b != TY_FLOAT)
                    err_node(e->call.args[4],
                             "momentum() expects beta as a 'float', found '%s' - "
                             "as in momentum(w, g, 0.01, v, 0.9)",
                             type_name(b));
                e->call.builtin = BUILTIN_MOMENTUM;
            } else if (!is_sgd) {
                Type s = check_expr(e->call.args[4]);
                if (s != p)
                    err_node(e->call.args[4],
                             "adam() expects the second state to have the "
                             "parameter's type, found '%s' for '%s'",
                             type_name(s), type_name(p));
                Type t = check_expr(e->call.args[5]);
                if (t != TY_INT)
                    err_node(e->call.args[5],
                             "adam() expects the step count as an 'int', found '%s' "
                             "- it counts updates already made, starting at 0",
                             type_name(t));
                e->call.builtin = BUILTIN_ADAM;
            } else {
                e->call.builtin = BUILTIN_SGD;
            }
            return e->type = p;
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

        /* Resolved against the global function table collected in analyze().
         * The real name is used here, not the possibly-empty `name`, so that
         * a shadowing definition is what this finds. */
        Func *fn = find_func(g_prog, e->call.name);
        if (!fn) {
            if (find_var(name)) {
                err_node(e, "'%s' is a variable, not a function", name);
            }
            if (strcmp(name, "print") == 0) {
                err_node(e,
                    "'print' does not exist in Ducky - the output builtin is 'serve'");
            }
            err_node(e, "undefined function '%s'", name);
        }

        if (fn->is_variadic) {
            if (e->call.nargs < fn->nparams) {
                err_node(e, "function '%s' expects at least %d argument%s, found %d",
                    fn->name, fn->nparams, fn->nparams == 1 ? "" : "s", e->call.nargs);
            }
        } else if (e->call.nargs != fn->nparams) {
            err_node(e, "function '%s' expects %d argument%s, found %d", fn->name,
                fn->nparams, fn->nparams == 1 ? "" : "s", e->call.nargs);
        }
        for (int i = 0; i < e->call.nargs; i++) {
            Type t = check_expr(e->call.args[i]);
            if (i < fn->nparams) {
                if (t != fn->params[i]->type) {
                    err_node(e->call.args[i],
                        "argument %d of '%s' expects '%s', found '%s'", i + 1, fn->name,
                        type_name(fn->params[i]->type), type_name(t));
                }
            } else if (t != TY_INT && t != TY_FLOAT && t != TY_BOOL && t != TY_STRING) {
                /* Extra argument of a C variadic call: only scalars map. */
                err_node(e->call.args[i],
                    "argument %d of '%s' expects 'int', 'float', 'bool' or 'string', "
                    "found '%s'", i + 1, fn->name, type_name(t));
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
            if (!type_is_array(ot) && !type_is_tensor(ot)) {
                if (ot == TY_STRING) {
                    err_node(tg,
                        "a 'string' cannot be modified through an index - strings are immutable");
                }
                err_node(tg, "cannot assign to an element of type '%s'",
                    type_name(ot));
            }
            /* The target's own type decides what may be stored: a float when
             * the index reaches the innermost dimension, another tensor when
             * it only drops one. `t[i][j] = 5.0` and `t[i] = row` are both
             * checked here, against the shape the compiler already knows. */
            Type target = type_is_tensor(ot) ? tensor_tail(ot) : type_elem(ot);
            Type t = check_expr(s->assign.value);
            if (t == TY_VOID) {
                err_node(s, "cannot assign a value of type 'void'");
            }
            if (t != target) {
                err_node(s,
                    "type mismatch: cannot assign '%s' to an element of type '%s'",
                    type_name(t), type_name(target));
            }
            tg->type = target;
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
    if (f->is_extern) return; /* a C declaration has no body to analyze */

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

/* ---------- automatic differentiation: grad(f, x) -------------------------
 *
 * `grad(f, x)` differentiates the top-level function `f` with respect to `x`
 * and expands, at compile time, into a call to a synthesized `f$grad`.
 *
 * This is reverse mode, but there is no tape. `f` must be straight-line -
 * only `let` statements and a final `send`, no branches, no loops, no
 * assignment and no calls to other functions - so every intermediate is
 * still in scope when the function ends and the derivative of the result can
 * be written as ordinary expressions over values that are still live. Each
 * gradient is a real local in the synthesized frame:
 *
 *     fn f(p: Params) -> float        fn f$grad(p: Params) -> Params
 *     { ... }                     =>  { <the forward statements, unchanged>
 *                                        let g$p_w = zeros([2, 3]);
 *                                        let g$y = 0.0;
 *                                        g$y = g$y + 1.0;
 *                                        g$p_w = g$p_w + ...;
 *                                        send Params(g$p_w); }
 *
 * The statements are visited in reverse, so a gradient is complete before
 * anything that feeds it is touched. Because the result is an ordinary
 * function, it goes through exactly the same type checker and the same code
 * generator as anything the user writes - there is no second evaluation
 * path to keep honest.
 *
 * A struct contributes one gradient local per *leaf* (a `float` or a tensor
 * reached through it), named `g$<var>_<path>`, which is what lets a parameter
 * struct be differentiated without ever writing arithmetic on a struct.
 */

/* ---- node construction --------------------------------------------------- */

/* Expr and Stmt open with the same four members, so one helper can copy the
 * position of either kind of node. */
typedef struct {
    int kind;
    int line;
    int col;
    const SourceFile *src;
} NodeHead;

#define err_here(at, ...)                                                     \
    err_at(((const NodeHead *)(at))->src, ((const NodeHead *)(at))->line,     \
           ((const NodeHead *)(at))->col, __VA_ARGS__)

static Expr *ge(ExprKind k, const void *at) {
    const NodeHead *h = at;
    Expr *e = arena_alloc(sizeof(Expr));
    memset(e, 0, sizeof *e);
    e->kind = k;
    e->line = h->line;
    e->col = h->col;
    e->src = h->src;
    return e;
}

static Stmt *gs(StmtKind k, const void *at) {
    const NodeHead *h = at;
    Stmt *s = arena_alloc(sizeof(Stmt));
    memset(s, 0, sizeof *s);
    s->kind = k;
    s->line = h->line;
    s->col = h->col;
    s->src = h->src;
    return s;
}

static Expr *g_num(double v, const void *at) {
    Expr *e = ge(EX_FLOAT, at);
    e->dval = v;
    e->type = TY_FLOAT;
    return e;
}

static Expr *g_int(long v, const void *at) {
    Expr *e = ge(EX_INT, at);
    e->ival = v;
    e->type = TY_INT;
    return e;
}

static Expr *g_var(const char *name, Type t, const void *at) {
    Expr *e = ge(EX_VAR, at);
    e->var.name = (char *)name;
    e->type = t;
    return e;
}

/* The type of a binary result: a tensor wins whenever one side has one, which
 * is what `float * tensor` and `tensor * float` both need. sema recomputes it
 * later; this only has to be good enough for `fit` below. */
static Expr *g_bin(BinaryOp op, Expr *a, Expr *b) {
    Expr *e = ge(EX_BINARY, a);
    e->binary.op = op;
    e->binary.lhs = a;
    e->binary.rhs = b;
    e->type = type_is_tensor(b->type) ? b->type : a->type;
    return e;
}

static Expr *g_neg(Expr *a) {
    Expr *e = ge(EX_UNARY, a);
    e->unary.op = UOP_NEG;
    e->unary.operand = a;
    e->type = a->type;
    return e;
}

static Expr **g_args(int n, ...) {
    Expr **v = arena_alloc((size_t)n * sizeof(Expr *));
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) v[i] = va_arg(ap, Expr *);
    va_end(ap);
    return v;
}

static Expr *g_call(const char *name, Builtin b, Expr **args, int n, Type ret,
                    const void *at) {
    Expr *e = ge(EX_CALL, at);
    e->call.name = (char *)name;
    e->call.args = args;
    e->call.nargs = n;
    e->call.builtin = b;
    e->type = ret;
    return e;
}

static Expr *g_index(Expr *obj, Expr *idx, const void *at) {
    Expr *e = ge(EX_INDEX, at);
    e->index.obj = obj;
    e->index.idx = idx;
    e->type = type_is_tensor(obj->type) ? tensor_tail(obj->type)
                                        : type_elem(obj->type);
    return e;
}

/* `zeros([2, 3])` written out with literal dimensions: the shape argument of
 * the tensor builtins has to be a literal, and the shape is already known
 * statically from the type anyway. */
static Expr *g_fill(Type t, double value, const void *at) {
    if (!type_is_tensor(t)) return g_num(value, at); /* a scalar seed */
    TensorShape *ts = type_shape(t);
    Expr **dims = arena_alloc((size_t)ts->rank * sizeof(Expr *));
    for (int i = 0; i < ts->rank; i++) dims[i] = g_int(ts->dims[i], at);
    Expr *shape = ge(EX_ARRAY, at);
    shape->array.elems = dims;
    shape->array.nelems = ts->rank;
    shape->type = type_array_of(TY_INT);
    return g_call(value == 0.0 ? "zeros" : "ones",
                  value == 0.0 ? BUILTIN_ZEROS : BUILTIN_ONES, g_args(1, shape),
                  1, t, at);
}

/* ---- deep copy ----------------------------------------------------------- */

/* The forward statements of `f` are copied rather than shared: they get their
 * stack slots assigned inside the new frame, and `f`'s own slots must not be
 * moved by that. */
static Expr *copy_expr(const Expr *e) {
    if (!e) return NULL;
    Expr *c = ge(e->kind, e);
    c->type = e->type;
    switch (e->kind) {
    case EX_INT:   c->ival = e->ival; break;
    case EX_FLOAT: c->dval = e->dval; break;
    case EX_BOOL:  c->bval = e->bval; break;
    case EX_STRING: c->sval = e->sval; break;
    case EX_VAR:
        c->var.name = e->var.name;
        c->var.offset = e->var.offset;
        break;
    case EX_UNARY:
        c->unary.op = e->unary.op;
        c->unary.operand = copy_expr(e->unary.operand);
        break;
    case EX_BINARY:
        c->binary.op = e->binary.op;
        c->binary.lhs = copy_expr(e->binary.lhs);
        c->binary.rhs = copy_expr(e->binary.rhs);
        break;
    case EX_CALL:
        c->call.name = e->call.name;
        c->call.nargs = e->call.nargs;
        if (e->call.nargs) {
            c->call.args = arena_alloc((size_t)e->call.nargs * sizeof(Expr *));
            for (int i = 0; i < e->call.nargs; i++)
                c->call.args[i] = copy_expr(e->call.args[i]);
        }
        c->call.fn = e->call.fn;
        c->call.builtin = e->call.builtin;
        c->call.sdef = e->call.sdef;
        c->call.slot = 0; /* sema gives the copy its own hidden slot */
        break;
    case EX_ARRAY:
        c->array.nelems = e->array.nelems;
        if (e->array.nelems) {
            c->array.elems =
                arena_alloc((size_t)e->array.nelems * sizeof(Expr *));
            for (int i = 0; i < e->array.nelems; i++)
                c->array.elems[i] = copy_expr(e->array.elems[i]);
        }
        c->array.slot = 0;
        break;
    case EX_INDEX:
        c->index.obj = copy_expr(e->index.obj);
        c->index.idx = copy_expr(e->index.idx);
        break;
    case EX_FIELD:
        c->field.obj = copy_expr(e->field.obj);
        c->field.name = e->field.name;
        c->field.offset = e->field.offset;
        break;
    }
    return c;
}

/* Only `let` statements are ever copied: grad_validate has already rejected
 * everything else in the body of a function being differentiated. */
static Stmt *copy_stmt(const Stmt *s) {
    Stmt *c = gs(s->kind, s);
    c->let.name = s->let.name;
    c->let.ann = s->let.ann;
    c->let.has_ann = s->let.has_ann;
    c->let.init = copy_expr(s->let.init);
    c->let.offset = 0; /* sema gives the copy its own slot */
    return c;
}

/* ---- gradient slots ------------------------------------------------------ */

#define GRAD_MAX_SLOTS 512

typedef struct {
    const char *var; /* the variable being differentiated          */
    const char *path; /* "" for the value itself, "w" or "inner.x" */
    const char *name; /* the local that holds the gradient: "g$p_w" */
    Type type;
} GSlot;

static GSlot g_gslot[GRAD_MAX_SLOTS];
static int g_ngslot;

/* A value that can carry a gradient at all. */
static int is_diff_leaf(Type t) { return t == TY_FLOAT || type_is_tensor(t); }

/* `a.b.c` joined by `head`, for addressing a leaf through a struct. */
static const char *path_join(const char *head, const char *tail) {
    size_t a = strlen(head), b = strlen(tail);
    char *p = arena_alloc(a + b + 2);
    memcpy(p, head, a);
    p[a] = '.';
    memcpy(p + a + 1, tail, b + 1);
    return p;
}

/* The local that carries the gradient of `var`'s leaf at `path`. Dots become
 * underscores because a stack slot is named with a plain identifier. */
static const char *grad_slot_name(const char *var, const char *path) {
    size_t lv = strlen(var), lp = strlen(path);
    char *p = arena_alloc(lv + lp + 4);
    char *w = p;
    *w++ = 'g';
    *w++ = '$';
    memcpy(w, var, lv);
    w += lv;
    if (lp) {
        *w++ = '_';
        for (size_t i = 0; i < lp; i++) *w++ = (path[i] == '.') ? '_' : path[i];
    }
    *w = '\0';
    return p;
}

static void add_gslot(const char *var, const char *path, Type t,
                      const void *at) {
    for (int i = 0; i < g_ngslot; i++)
        if (strcmp(g_gslot[i].var, var) == 0 &&
            strcmp(g_gslot[i].path, path) == 0)
            return;
    if (g_ngslot >= GRAD_MAX_SLOTS)
        err_here(at, "grad() cannot differentiate this: more than %d gradient "
                     "values are needed",
                 GRAD_MAX_SLOTS);
    GSlot *s = &g_gslot[g_ngslot++];
    s->var = var;
    s->path = arena_strndup(path, strlen(path));
    s->name = grad_slot_name(var, path);
    s->type = t;
}

/* One slot per leaf reachable through `var`. `strict` is set for the
 * parameter being differentiated: there the gradient has to come back in the
 * same type, so a field with no gradient is an error rather than a hole. */
static void add_gleaves(const char *var, const char *path, Type t,
                        const void *at, int strict) {
    if (type_is_struct(t)) {
        StructDecl *sd = struct_type_decl(t);
        for (int i = 0; i < sd->nfields; i++) {
            const char *sub = path[0] ? path_join(path, sd->fields[i]->name)
                                      : sd->fields[i]->name;
            add_gleaves(var, sub, sd->fields[i]->type, at, strict);
        }
        return;
    }
    if (is_diff_leaf(t)) {
        add_gslot(var, path, t, at);
        return;
    }
    if (strict)
        err_here(at, "grad() cannot differentiate with respect to '%s' of type "
                     "'%s' - only floats, tensors and structs of those",
                 path[0] ? path : var, type_name(t));
}

static int gslot_find(const char *var, const char *path) {
    for (int i = 0; i < g_ngslot; i++)
        if (strcmp(g_gslot[i].var, var) == 0 &&
            strcmp(g_gslot[i].path, path) == 0)
            return i;
    return -1;
}

/* ---- the reverse pass ---------------------------------------------------- */

typedef struct {
    Expr *target;  /* the gradient local (or element of one) being updated */
    Expr *contrib; /* what is added to it                                  */
} GItem;

typedef struct {
    GItem *v;
    int n, cap;
} GList;

static void glist(GList *l, Expr *target, Expr *contrib) {
    if (!target || !contrib) return;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        GItem *nv = arena_alloc((size_t)l->cap * sizeof(GItem));
        if (l->n) memcpy(nv, l->v, (size_t)l->n * sizeof(GItem));
        l->v = nv;
    }
    l->v[l->n].target = target;
    l->v[l->n].contrib = contrib;
    l->n++;
}

/* One contribution to a struct-valued expression, addressed by the leaf. */
typedef struct {
    const char *path;
    Expr *contrib;
} SItem;

/* Reshape a contribution to the type of the slot it is added to. `t + 2.0`
 * gives the tensor and the scalar each a gradient, and the scalar's is the
 * sum over the elements. */
static Expr *fit(Expr *c, Type want, const void *at) {
    if (c->type == want) return c;
    if (want == TY_FLOAT && type_is_tensor(c->type))
        return g_call("sum", BUILTIN_SUM, g_args(1, c), 1, TY_FLOAT, at);
    err_here(at, "grad() cannot build a gradient of type '%s' (built one of "
                 "type '%s')",
             type_name(want), type_name(c->type));
    return c;
}

/* Is there anything below `e` that could receive a gradient at all? Used to
 * skip building a contribution for `t * 2.0`'s constant half, which would
 * otherwise be computed and thrown away. */
static int has_diff_leaf(Expr *e) {
    if (!e) return 0;
    switch (e->kind) {
    case EX_VAR: return is_diff_leaf(e->type) || type_is_struct(e->type);
    /* Indexing an array cannot receive a gradient, but it must not be
     * skipped either: `a[0] + a[1]` would otherwise quietly differentiate to
     * zero instead of reporting that arrays are not supported. Saying "yes"
     * here routes it to grad_target, which raises the diagnostic. */
    case EX_INDEX: return has_diff_leaf(e->index.obj) ||
                            type_is_array(e->index.obj->type);
    case EX_FIELD: return has_diff_leaf(e->field.obj);
    case EX_UNARY: return has_diff_leaf(e->unary.operand);
    case EX_BINARY:
        return has_diff_leaf(e->binary.lhs) || has_diff_leaf(e->binary.rhs);
    case EX_ARRAY:
        for (int i = 0; i < e->array.nelems; i++)
            if (has_diff_leaf(e->array.elems[i])) return 1;
        return 0;
    case EX_CALL:
        for (int i = 0; i < e->call.nargs; i++)
            if (has_diff_leaf(e->call.args[i])) return 1;
        return 0;
    default: return 0;
    }
}

/* The expression that names the gradient of a leaf-valued expression: the
 * local `g$...`, or a subscript of it when the expression is an element or a
 * slice of a tensor. NULL when nothing below it can receive a gradient (a
 * constant, or a `const` that was already folded into a literal). */
static Expr *grad_target(Expr *e, const void *at) {
    switch (e->kind) {
    case EX_VAR: {
        int i = gslot_find(e->var.name, "");
        if (i < 0) return NULL;
        return g_var(g_gslot[i].name, g_gslot[i].type, at);
    }
    case EX_FIELD: {
        const Expr *chain[24];
        int n = 0;
        const Expr *cur = e;
        while (cur && cur->kind == EX_FIELD && n < 24) {
            chain[n++] = cur;
            cur = cur->field.obj;
        }
        if (!cur || cur->kind != EX_VAR) return NULL;
        const char *path = "";
        for (int i = n - 1; i >= 0; i--)
            path = path[0] ? path_join(path, chain[i]->field.name)
                           : chain[i]->field.name;
        int k = gslot_find(cur->var.name, path);
        if (k < 0) return NULL;
        return g_var(g_gslot[k].name, g_gslot[k].type, at);
    }
    case EX_INDEX: {
        if (type_is_array(e->index.obj->type))
            err_here(at, "grad() cannot differentiate an array - use a tensor");
        Expr *base = grad_target(e->index.obj, at);
        if (!base) return NULL;
        return g_index(base, copy_expr(e->index.idx), at);
    }
    default: return NULL;
    }
}

static void rev_at(Expr *e, const char *path, Expr *g, GList *out);
static void rev_leaf(Expr *e, Expr *g, GList *out);

/* The gradient of a struct-valued expression arrives one leaf at a time:
 * there is no arithmetic on structs to combine them with. */
static void rev_struct(Expr *e, const SItem *items, int n, GList *out) {
    if (e->kind == EX_VAR) {
        for (int i = 0; i < n; i++) {
            int k = gslot_find(e->var.name, items[i].path);
            if (k >= 0)
                glist(out, g_var(g_gslot[k].name, g_gslot[k].type, e),
                      items[i].contrib);
        }
        return;
    }
    if (e->kind == EX_FIELD) {
        SItem sub[24];
        int m = 0;
        for (int i = 0; i < n && m < 24; i++) {
            sub[m].path = items[i].path[0]
                              ? path_join(e->field.name, items[i].path)
                              : e->field.name;
            sub[m].contrib = items[i].contrib;
            m++;
        }
        rev_struct(e->field.obj, sub, m, out);
        return;
    }
    if (e->kind == EX_CALL && e->call.sdef) {
        StructDecl *sd = e->call.sdef;
        for (int i = 0; i < n; i++) {
            const char *dot = strchr(items[i].path, '.');
            char head[64];
            if (dot) {
                size_t k = (size_t)(dot - items[i].path);
                memcpy(head, items[i].path, k);
                head[k] = '\0';
            } else {
                snprintf(head, sizeof head, "%s", items[i].path);
            }
            int idx = -1;
            for (int f = 0; f < sd->nfields; f++)
                if (strcmp(sd->fields[f]->name, head) == 0) {
                    idx = f;
                    break;
                }
            if (idx < 0) continue;
            rev_at(e->call.args[idx], dot ? dot + 1 : "", items[i].contrib,
                   out);
        }
        return;
    }
    /* Anything else has no structure to descend into. */
}

static void rev_at(Expr *e, const char *path, Expr *g, GList *out) {
    if (!e) return;
    if (type_is_struct(e->type)) {
        SItem it;
        it.path = path;
        it.contrib = g;
        rev_struct(e, &it, 1, out);
        return;
    }
    if (path[0])
        err_here(e, "grad() internal error: a gradient path reached a value "
                    "of type '%s'",
                 type_name(e->type));
    rev_leaf(e, g, out);
}

/* The chain rule at one node: `g` is d(result)/d(e), and the call adds
 * d(result)/d(operand) for each operand that can carry one. */
static void rev_leaf(Expr *e, Expr *g, GList *out) {
    if (!e || !is_diff_leaf(e->type)) return;

    switch (e->kind) {
    case EX_VAR:
    case EX_FIELD:
    case EX_INDEX: {
        Expr *t = grad_target(e, e);
        if (t) glist(out, t, g);
        return;
    }

    case EX_UNARY:
        if (e->unary.op == UOP_NEG && has_diff_leaf(e->unary.operand))
            rev_leaf(e->unary.operand, g_neg(g), out);
        return;

    case EX_BINARY: {
        Expr *a = e->binary.lhs;
        Expr *b = e->binary.rhs;
        switch (e->binary.op) {
        case BOP_ADD:
            if (has_diff_leaf(a)) rev_leaf(a, fit(g, a->type, e), out);
            if (has_diff_leaf(b)) rev_leaf(b, fit(g, b->type, e), out);
            return;
        case BOP_SUB:
            if (has_diff_leaf(a)) rev_leaf(a, fit(g, a->type, e), out);
            if (has_diff_leaf(b)) rev_leaf(b, fit(g_neg(g), b->type, e), out);
            return;
        case BOP_MUL:
            if (has_diff_leaf(a))
                rev_leaf(a, fit(g_bin(BOP_MUL, g, copy_expr(b)), a->type, e),
                         out);
            if (has_diff_leaf(b))
                rev_leaf(b, fit(g_bin(BOP_MUL, g, copy_expr(a)), b->type, e),
                         out);
            return;
        case BOP_DIV:
            if (has_diff_leaf(a))
                rev_leaf(a, fit(g_bin(BOP_DIV, g, copy_expr(b)), a->type, e),
                         out);
            if (has_diff_leaf(b)) {
                Expr *bb = g_bin(BOP_MUL, copy_expr(b), copy_expr(b));
                Expr *r =
                    g_neg(g_bin(BOP_DIV, g_bin(BOP_MUL, g, copy_expr(a)), bb));
                rev_leaf(b, fit(r, b->type, e), out);
            }
            return;
        default:
            break; /* comparisons and bitwise operators are not leaves */
        }
        break;
    }

    case EX_CALL: {
        Builtin b = e->call.builtin;
        Expr *a0 = e->call.nargs > 0 ? e->call.args[0] : NULL;
        Expr *a1 = e->call.nargs > 1 ? e->call.args[1] : NULL;

        switch (b) {
        case BUILTIN_MATMUL: {
            /* A (m,k) * B (k,n) = C (m,n), G (m,n):
             *   dA = G * B^T      dB = A^T * G
             * A rank-1 operand is one column, which is exactly what both
             * products already assume. */
            Expr *A = a0, *B = a1;
            rev_leaf(A, g_call("matmul_nt", BUILTIN_MATMUL_NT,
                               g_args(2, copy_expr(g), copy_expr(B)), 2, A->type,
                               e),
                     out);
            rev_leaf(B, g_call("matmul_tn", BUILTIN_MATMUL_TN,
                               g_args(2, copy_expr(A), copy_expr(g)), 2, B->type,
                               e),
                     out);
            return;
        }
        case BUILTIN_DOT: {
            Expr *A = a0, *B = a1;
            if (has_diff_leaf(A))
                rev_leaf(A, fit(g_bin(BOP_MUL, g, copy_expr(B)), A->type, e),
                         out);
            if (has_diff_leaf(B))
                rev_leaf(B, fit(g_bin(BOP_MUL, g, copy_expr(A)), B->type, e),
                         out);
            return;
        }
        case BUILTIN_RELU: {
            Expr *step = g_call("step", BUILTIN_STEP, g_args(1, copy_expr(a0)),
                                1, a0->type, e);
            rev_leaf(a0, g_bin(BOP_MUL, g, step), out);
            return;
        }
        case BUILTIN_SIGMOID: {
            Expr *s = g_call("sigmoid", BUILTIN_SIGMOID,
                             g_args(1, copy_expr(a0)), 1, a0->type, e);
            Expr *d = g_bin(BOP_MUL, s, g_bin(BOP_SUB, g_num(1.0, e),
                                              copy_expr(s)));
            rev_leaf(a0, g_bin(BOP_MUL, g, d), out);
            return;
        }
        case BUILTIN_TANH: {
            Expr *s =
                g_call("tanh", BUILTIN_TANH, g_args(1, copy_expr(a0)), 1,
                       a0->type, e);
            Expr *d = g_bin(BOP_SUB, g_num(1.0, e),
                            g_bin(BOP_MUL, copy_expr(s), copy_expr(s)));
            rev_leaf(a0, g_bin(BOP_MUL, g, d), out);
            return;
        }
        case BUILTIN_GELU: {
            /* gelu(x) = 0.5 x (1 + tanh(u)), u = c (x + k x^3)
             *   d/dx = 0.5 (1 + t) + 0.5 x (1 - t^2) c (1 + 3 k x^2)
             * which is then scaled by the gradient arriving from above. */
            Expr *x = copy_expr(a0);
            const double k = 0.044715;
            const double c = 0.79788456080286535588;
            Expr *u = g_bin(
                BOP_MUL, g_num(c, e),
                g_bin(BOP_ADD, copy_expr(x),
                      g_bin(BOP_MUL, g_num(k, e),
                            g_bin(BOP_MUL, copy_expr(x),
                                  g_bin(BOP_MUL, copy_expr(x),
                                        copy_expr(x))))));
            Expr *t = g_call("tanh", BUILTIN_TANH, g_args(1, u), 1, a0->type, e);
            Expr *head = g_bin(BOP_MUL, g_num(0.5, e),
                               g_bin(BOP_ADD, g_num(1.0, e), copy_expr(t)));
            Expr *tail = g_bin(
                BOP_MUL, g_num(0.5, e),
                g_bin(BOP_MUL, copy_expr(x),
                      g_bin(BOP_MUL,
                            g_bin(BOP_SUB, g_num(1.0, e),
                                  g_bin(BOP_MUL, copy_expr(t), copy_expr(t))),
                            g_bin(BOP_MUL, g_num(c, e),
                                  g_bin(BOP_ADD, g_num(1.0, e),
                                        g_bin(BOP_MUL, g_num(3.0 * k, e),
                                              g_bin(BOP_MUL, copy_expr(x),
                                                    copy_expr(x))))))));
            rev_leaf(a0, g_bin(BOP_MUL, g, g_bin(BOP_ADD, head, tail)), out);
            return;
        }
        case BUILTIN_SUM:
            /* d sum(t) / dt = 1 for every element. */
            rev_leaf(a0, g_bin(BOP_MUL, g, g_fill(a0->type, 1.0, e)), out);
            return;
        case BUILTIN_MEAN: {
            long n = tensor_nelems(a0->type);
            Expr *scale = g_bin(BOP_DIV, g, g_num((double)n, e));
            rev_leaf(a0, g_bin(BOP_MUL, scale, g_fill(a0->type, 1.0, e)), out);
            return;
        }
        case BUILTIN_MSE: {
            /* mse = sum((p - t)^2) / n, so dm/dp = 2 (p - t) / n and the
             * target takes the negative of the same thing. */
            Expr *p = a0, *t = a1;
            long n = tensor_nelems(p->type);
            Expr *diff = g_bin(BOP_SUB, copy_expr(p), copy_expr(t));
            Expr *core = g_bin(
                BOP_MUL, g_bin(BOP_MUL, g, g_num(2.0 / (double)n, e)),
                copy_expr(diff));
            if (has_diff_leaf(p)) rev_leaf(p, core, out);
            if (has_diff_leaf(t)) rev_leaf(t, g_neg(copy_expr(core)), out);
            return;
        }
        case BUILTIN_CROSS_ENTROPY: {
            /* cross_entropy = logsumexp(logits) - logits[class], whose
             * derivative is softmax(logits) - onehot(class). */
            Expr *logits = a0;
            Expr *d = g_call("cross_entropy_grad", BUILTIN_XENT_GRAD,
                             g_args(2, copy_expr(logits), copy_expr(a1)), 2,
                             logits->type, e);
            rev_leaf(logits, g_bin(BOP_MUL, g, d), out);
            return;
        }
        case BUILTIN_TENSOR:
        case BUILTIN_ZEROS:
        case BUILTIN_ONES:
        case BUILTIN_RAND:
        case BUILTIN_FLOAT:
        case BUILTIN_SCAN_FLOAT:
        case BUILTIN_SCAN_FLOAT_LINE:
            return; /* a constant: nothing below it can move */

        default:
            break;
        }
        err_here(e, "grad() cannot differentiate the call to '%s()'",
                 e->call.name);
        return;
    }

    default:
        return; /* a literal carries no gradient */
    }

    err_here(e, "grad() cannot differentiate the '%s' operator",
             binary_op_name(e->binary.op));
}

/* ---- validation ---------------------------------------------------------- */

static const char *stmt_word(Stmt *s) {
    switch (s->kind) {
    case ST_LET: return "'let'";
    case ST_ASSIGN: return "an assignment";
    case ST_EXPR: return "a statement that is not 'let'";
    case ST_IF: return "'if'";
    case ST_WHILE: return "'while'";
    case ST_FOR: return "'for'";
    case ST_BREAK: return "'break'";
    case ST_CONTINUE: return "'continue'";
    case ST_RETURN: return "'send'";
    case ST_BLOCK: return "a nested block";
    }
    return "this";
}

static void grad_validate_expr(Expr *e, Func *f) {
    if (!e) return;
    switch (e->kind) {
    case EX_UNARY:
        grad_validate_expr(e->unary.operand, f);
        return;
    case EX_BINARY:
        grad_validate_expr(e->binary.lhs, f);
        grad_validate_expr(e->binary.rhs, f);
        return;
    case EX_ARRAY:
        for (int i = 0; i < e->array.nelems; i++)
            grad_validate_expr(e->array.elems[i], f);
        return;
    case EX_INDEX:
        grad_validate_expr(e->index.obj, f);
        grad_validate_expr(e->index.idx, f);
        return;
    case EX_FIELD:
        grad_validate_expr(e->field.obj, f);
        return;
    case EX_CALL:
        if (e->call.builtin == BUILTIN_NONE && !e->call.sdef)
            err_at(e->src, e->line, e->col,
                   "grad() cannot differentiate '%s': the call to '%s()' is not "
                   "a builtin",
                   f->name, e->call.name);
        for (int i = 0; i < e->call.nargs; i++)
            grad_validate_expr(e->call.args[i], f);
        return;
    default:
        return;
    }
}

static void grad_validate(Func *f) {
    Block *b = f->body;
    if (b->nstmts == 0)
        err_at(f->src, b->line, b->col,
               "grad() cannot differentiate '%s': its body is empty", f->name);
    for (int i = 0; i < b->nstmts; i++) {
        Stmt *s = b->stmts[i];
        int last = (i == b->nstmts - 1);
        if (s->kind == ST_LET) {
            grad_validate_expr(s->let.init, f);
        } else if (s->kind == ST_RETURN && last && s->value) {
            grad_validate_expr(s->value, f);
        } else {
            err_at(s->src, s->line, s->col,
                   "grad() cannot differentiate '%s': only 'let' statements "
                   "and a final 'send' are supported (found %s)",
                   f->name, stmt_word(s));
        }
    }
    if (b->stmts[b->nstmts - 1]->kind != ST_RETURN)
        err_at(f->src, b->line, b->col,
               "grad() cannot differentiate '%s': it must end in a 'send'", f->name);
}

/* ---- building the gradient function ------------------------------------- */

typedef struct {
    Stmt **v;
    int n, cap;
} GBody;

static void body_push(GBody *bd, Stmt *s) {
    if (bd->n == bd->cap) {
        bd->cap = bd->cap ? bd->cap * 2 : 32;
        Stmt **nv = arena_alloc((size_t)bd->cap * sizeof(Stmt *));
        if (bd->n) memcpy(nv, bd->v, (size_t)bd->n * sizeof(Stmt *));
        bd->v = nv;
    }
    bd->v[bd->n++] = s;
}

/* One `target = target + contrib;` per accumulated contribution. The
 * contribution is copied: one `g` can appear in two contributions (both
 * halves of `a * b` need it), and two statements must not share a subtree. */
static void emit_grads(const GList *l, GBody *bd) {
    for (int i = 0; i < l->n; i++) {
        Expr *target = l->v[i].target;
        Stmt *st = gs(ST_ASSIGN, target);
        st->assign.value = g_bin(BOP_ADD, copy_expr(target),
                                 copy_expr(l->v[i].contrib));
        if (target->kind == EX_VAR) {
            st->assign.name = target->var.name;
            st->assign.target = NULL;
        } else {
            st->assign.name = NULL;
            st->assign.target = target;
        }
        body_push(bd, st);
    }
}

/* The gradient of the parameter itself: the local for a float or a tensor,
 * a construction of the struct's gradient leaves for a parameter struct. */
static Expr *grad_result(Type t, const char *var, const char *path,
                         const void *at) {
    if (type_is_struct(t)) {
        StructDecl *sd = struct_type_decl(t);
        Expr **args = arena_alloc((size_t)sd->nfields * sizeof(Expr *));
        for (int i = 0; i < sd->nfields; i++) {
            const char *sub = path[0] ? path_join(path, sd->fields[i]->name)
                                      : sd->fields[i]->name;
            args[i] = grad_result(sd->fields[i]->type, var, sub, at);
        }
        Expr *e = ge(EX_CALL, at);
        e->call.name = (char *)struct_type_name(t);
        e->call.args = args;
        e->call.nargs = sd->nfields;
        e->call.builtin = BUILTIN_NONE;
        e->call.sdef = sd;
        e->type = t;
        return e;
    }
    int k = gslot_find(var, path);
    if (k < 0)
        err_here(at, "grad() has no gradient for '%s'",
                 path[0] ? path : var);
    return g_var(g_gslot[k].name, g_gslot[k].type, at);
}

/* Functions currently being differentiated: a `grad` inside a `grad` is
 * bounded, and a cycle between two of them is reported instead of looping. */
static Func *g_grad_active[16];
static int g_grad_nactive;

static Func *grad_build(Func *src, Expr *call) {
    if (src->is_extern)
        err_here(call, "grad() cannot differentiate '%s': it is declared "
                       "'extern'",
                 src->name);
    if (src->nparams != 1)
        err_here(call, "grad() expects '%s' to take exactly 1 parameter, found %d",
                 src->name, src->nparams);
    if (src->ret != TY_FLOAT)
        err_here(call,
                 "grad() expects '%s' to return 'float' - the quantity being "
                 "minimised, found '%s'",
                 src->name, type_name(src->ret));

    /* Already built: two call sites share one gradient function. */
    char gname[192];
    snprintf(gname, sizeof gname, "%s$grad", src->name);
    Func *cached = find_func(g_prog, gname);
    if (cached) return cached;

    for (int i = 0; i < g_grad_nactive; i++)
        if (g_grad_active[i] == src)
            err_here(call, "grad() cannot differentiate '%s': it is already "
                           "being differentiated (definitions must not be "
                           "recursive)",
                     src->name);
    if (g_grad_nactive >= 16)
        err_here(call, "grad() is nested more than 16 deep");
    g_grad_active[g_grad_nactive++] = src;

    /* `src` may sit later in the program than its caller, so it may not have
     * been checked yet; the body below is only readable once it has. The
     * caller's own analysis is parked and restored around it. */
    Func *save_fn = g_fn;
    Scope *save_scope = g_scope;
    int save_slots = g_slots;
    int save_loops = g_loops;
    g_fn = NULL;
    g_scope = NULL;
    g_slots = 0;
    g_loops = 0;
    check_func(src);
    g_fn = save_fn;
    g_scope = save_scope;
    g_slots = save_slots;
    g_loops = save_loops;

    grad_validate(src);

    Param *param = src->params[0];
    if (!is_diff_leaf(param->type) && !type_is_struct(param->type))
        err_here(call,
                 "grad() cannot differentiate with respect to '%s' of type "
                 "'%s' - only floats, tensors and structs of those",
                 param->name, type_name(param->type));

    /* Every gradient local that could be needed, declared up front so the
     * reverse walk can write to any of them in any order. */
    g_ngslot = 0;
    add_gleaves(param->name, "", param->type, call, 1);
    Block *fb = src->body;
    Stmt **lets = arena_alloc((size_t)fb->nstmts * sizeof(Stmt *));
    int nlets = 0;
    for (int i = 0; i < fb->nstmts; i++) {
        Stmt *s = fb->stmts[i];
        if (s->kind != ST_LET) continue;
        lets[nlets++] = s;
        add_gleaves(s->let.name, "",
                    s->let.has_ann ? s->let.ann : s->let.init->type, s, 0);
    }

    GBody bd = {0};

    /* Forward: the original statements, unchanged, minus the `send`. */
    for (int i = 0; i < nlets; i++) body_push(&bd, copy_stmt(lets[i]));

    /* Gradient storage. */
    for (int i = 0; i < g_ngslot; i++) {
        Stmt *st = gs(ST_LET, call);
        st->let.name = (char *)g_gslot[i].name;
        st->let.has_ann = 0;
        st->let.ann = TY_VOID;
        st->let.init = g_fill(g_gslot[i].type, 0.0, call);
        body_push(&bd, st);
    }

    /* Backward. The seed is d(loss)/d(loss) = 1. */
    Stmt *ret = fb->stmts[fb->nstmts - 1];
    GList list = {0};
    rev_leaf(ret->value, g_num(1.0, call), &list);
    emit_grads(&list, &bd);

    /* Then each binding, in reverse, so a gradient is finished before
     * anything that feeds it is touched. */
    for (int i = nlets - 1; i >= 0; i--) {
        Stmt *s = lets[i];
        GList l2 = {0};
        int nitems = 0;
        SItem *items = arena_alloc((size_t)g_ngslot * sizeof(SItem));
        for (int k = 0; k < g_ngslot && nitems < GRAD_MAX_SLOTS; k++) {
            if (strcmp(g_gslot[k].var, s->let.name) != 0) continue;
            items[nitems].path = g_gslot[k].path;
            items[nitems].contrib = g_var(g_gslot[k].name, g_gslot[k].type, s);
            nitems++;
        }
        for (int k = 0; k < nitems; k++)
            rev_at(s->let.init, items[k].path, items[k].contrib, &l2);
        emit_grads(&l2, &bd);
    }

    Stmt *send = gs(ST_RETURN, call);
    send->value = grad_result(param->type, param->name, "", call);
    body_push(&bd, send);

    Block *nb = arena_alloc(sizeof(Block));
    nb->stmts = bd.v;
    nb->nstmts = bd.n;
    nb->line = call->line;
    nb->col = call->col;

    Func *gf = arena_alloc(sizeof(Func));
    gf->name = arena_strndup(gname, strlen(gname));
    gf->params = src->params;
    gf->nparams = 1;
    gf->ret = param->type;
    gf->body = nb;
    gf->line = src->line;
    gf->col = src->col;
    gf->src = src->src;
    gf->frame_size = 0;
    gf->is_extern = 0;
    gf->is_variadic = 0;

    Func **nf = arena_alloc((size_t)(g_prog->nfuncs + 1) * sizeof(Func *));
    if (g_prog->nfuncs)
        memcpy(nf, g_prog->funcs, (size_t)g_prog->nfuncs * sizeof(Func *));
    nf[g_prog->nfuncs++] = gf;
    g_prog->funcs = nf;

    g_grad_nactive--;
    return gf;
}

/* ---------- entry point ------------------------------------------------------ */

void analyze(const SourceFile *src, Program *prog) {
    g_src = src;
    g_prog = prog;

    /* Pass 0: validate the top-level constants and give each literal its
     * type once, so references can be substituted with a plain copy. */
    for (int i = 0; i < prog->nconsts; i++) {
        Const *c = prog->consts[i];

        if (c->name[0] == '_' || starts_with(c->name, "ducky_")) {
            err_node(c, "constant name '%s' is reserved", c->name);
        }
        if (is_reserved_builtin_name(c->name)) {
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

        if (sd->name[0] == '_' || starts_with(sd->name, "ducky_")) {
            err_node(sd, "struct name '%s' is reserved", sd->name);
        }
        if (is_reserved_builtin_name(sd->name)) {
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
            if (f->name[0] == '_' || starts_with(f->name, "ducky_")) {
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

        if (f->name[0] == '_' || starts_with(f->name, "ducky_")) {
            err_node(f, "function name '%s' is reserved", f->name);
        }
        if (is_reserved_builtin_name(f->name)) {
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

        if (f->is_extern) {
            /* Only scalars and strings have an unambiguous C ABI mapping. */
            for (int j = 0; j < f->nparams; j++) {
                Type t = f->params[j]->type;
                if (t != TY_INT && t != TY_FLOAT && t != TY_BOOL && t != TY_STRING) {
                    err_node(f->params[j],
                        "extern function '%s': parameter %d cannot be '%s' - "
                        "use 'int', 'float', 'bool' or 'string'",
                        f->name, j + 1, type_name(t));
                }
            }
            if (f->ret != TY_VOID && f->ret != TY_INT && f->ret != TY_FLOAT &&
                f->ret != TY_BOOL && f->ret != TY_STRING) {
                err_node(f,
                    "extern function '%s': the return type cannot be '%s' - "
                    "use 'int', 'float', 'bool', 'string' or no return type",
                    f->name, type_name(f->ret));
            }
        }
    }

    Func *entry = find_func(prog, "main");
    if (!entry) {
        err(1, 1, "the program must define an entry point: 'fn main() -> int'");
    }
    if (entry->is_extern) {
        err_node(entry, "the entry point 'main' must be defined, not declared 'extern'");
    }
    if (entry->nparams != 0 || entry->ret != TY_INT) {
        err_node(entry, "the entry point must be declared as 'fn main() -> int'");
    }

    /* Pass 2: check every body. */
    for (int i = 0; i < prog->nfuncs; i++) check_func(prog->funcs[i]);
}
