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
           strcmp(name, "tensor") == 0;
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
