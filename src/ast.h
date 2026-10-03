/* ast.h - abstract syntax tree for Ducky programs. */
#ifndef DUCK_AST_H
#define DUCK_AST_H

#include <stddef.h> /* NULL, used by the type helpers below */

/* ---------- types ------------------------------------------------------ */
/* Scalar and array types are small integers. Struct and tensor types are
 * interned: every `struct` declaration gets TY_STRUCT_BASE + index and every
 * distinct tensor shape gets TY_TENSOR_BASE + index into its own registry
 * (common.c), so nominal typing and static tensor shapes both need no other
 * refactor. The two ranges never overlap: struct kinds are capped at
 * TY_TENSOR_BASE - TY_STRUCT_BASE by struct_type_intern(). */
#define DUCKY_MAX_RANK 8 /* dimensions of the widest tensor type */

enum {
    TY_INT,     /* 64-bit signed integer  */
    TY_FLOAT,   /* 64-bit IEEE-754 float  */
    TY_BOOL,    /* boolean                */
    TY_STRING,  /* NUL-terminated string  */
    TY_ARR_INT,     /* [int]    fixed-length array of int    */
    TY_ARR_FLOAT,   /* [float]  fixed-length array of float  */
    TY_ARR_BOOL,    /* [bool]   fixed-length array of bool   */
    TY_ARR_STRING,  /* [string] fixed-length array of string */
    TY_VOID,    /* no value (only for calls) */
    TY_STRUCT_BASE = 64,      /* struct types: TY_STRUCT_BASE + index      */
    TY_TENSOR_BASE = 4160     /* tensor types: TY_TENSOR_BASE + index      */
};

typedef int Type;

/* A struct declaration; opaque here, defined below. */
typedef struct StructDecl StructDecl;

/* An interned tensor shape: the *type* `tensor[d0, d1, ...]`. Tensor types
 * carry their dimensions at compile time, which is what lets shape mistakes
 * such as matmul(a, b) with a mismatched inner dimension be reported by the
 * compiler instead of at run time. */
typedef struct TensorShape {
    Type type;                 /* TY_TENSOR_BASE + registry index */
    int rank;
    int dims[DUCKY_MAX_RANK];
    long nelems;               /* product of dims, checked at interning */
    char name[128];            /* "tensor[2, 3]" - what diagnostics show */
} TensorShape;

/* Struct and tensor type registries (common.c). */
StructDecl *struct_type_intern(const char *name);
StructDecl *struct_type_lookup(const char *name);
StructDecl *struct_type_decl(Type t);
const char *struct_type_name(Type t);

TensorShape *tensor_type_intern(const int *dims, int rank);
TensorShape *tensor_type_decl(Type t);
const char *tensor_type_name(Type t);

static inline int type_is_struct(Type t) {
    return t >= TY_STRUCT_BASE && t < TY_TENSOR_BASE;
}

static inline int type_is_tensor(Type t) {
    return t >= TY_TENSOR_BASE;
}

static inline TensorShape *type_shape(Type t) {
    return type_is_tensor(t) ? tensor_type_decl(t) : NULL;
}

/* Rank, dimension i and element count of a tensor type. */
static inline int tensor_rank(Type t) {
    TensorShape *ts = type_shape(t);
    return ts ? ts->rank : 0;
}

static inline int tensor_dim(Type t, int i) {
    TensorShape *ts = type_shape(t);
    return (ts && i >= 0 && i < ts->rank) ? ts->dims[i] : 0;
}

static inline long tensor_nelems(Type t) {
    TensorShape *ts = type_shape(t);
    return ts ? ts->nelems : 0;
}

static inline const char *type_name(Type t) {
    if (type_is_tensor(t)) return tensor_type_name(t);
    if (type_is_struct(t)) return struct_type_name(t);
    switch (t) {
    case TY_INT:        return "int";
    case TY_FLOAT:      return "float";
    case TY_BOOL:       return "bool";
    case TY_STRING:     return "string";
    case TY_ARR_INT:    return "[int]";
    case TY_ARR_FLOAT:  return "[float]";
    case TY_ARR_BOOL:   return "[bool]";
    case TY_ARR_STRING: return "[string]";
    case TY_VOID:       return "void";
    }
    return "?";
}

static inline int type_is_array(Type t) {
    return t == TY_ARR_INT || t == TY_ARR_FLOAT || t == TY_ARR_BOOL ||
           t == TY_ARR_STRING;
}

static inline Type type_elem(Type t) {
    switch (t) {
    case TY_ARR_INT:    return TY_INT;
    case TY_ARR_FLOAT:  return TY_FLOAT;
    case TY_ARR_BOOL:   return TY_BOOL;
    case TY_ARR_STRING: return TY_STRING;
    default:            return TY_VOID;
    }
}

static inline Type type_array_of(Type elem) {
    switch (elem) {
    case TY_INT:    return TY_ARR_INT;
    case TY_FLOAT:  return TY_ARR_FLOAT;
    case TY_BOOL:   return TY_ARR_BOOL;
    case TY_STRING: return TY_ARR_STRING;
    default:        return TY_VOID;
    }
}

/* ---------- expressions ------------------------------------------------ */
typedef enum {
    EX_INT,
    EX_FLOAT,
    EX_BOOL,
    EX_STRING,
    EX_VAR,
    EX_UNARY,
    EX_BINARY,
    EX_CALL,
    EX_ARRAY,
    EX_INDEX,
    EX_FIELD
} ExprKind;

typedef enum { UOP_NEG, UOP_NOT, UOP_BITNOT } UnaryOp;

typedef enum {
    BOP_ADD, BOP_SUB, BOP_MUL, BOP_DIV, BOP_MOD,
    BOP_EQ, BOP_NE, BOP_LT, BOP_LE, BOP_GT, BOP_GE,
    BOP_AND, BOP_OR,
    BOP_BITAND, BOP_BITOR, BOP_XOR, BOP_SHL, BOP_SHR
} BinaryOp;

/* Which builtin a call refers to (BUILTIN_NONE for ordinary calls). */
typedef enum {
    BUILTIN_NONE = 0,
    BUILTIN_SERVE,
    BUILTIN_LEN,
    BUILTIN_STR,
    BUILTIN_INPUT,
    BUILTIN_INT,   /* int(float) -> int   */
    BUILTIN_FLOAT, /* float(int) -> float */
    BUILTIN_PUSH,  /* push([T], T) -> [T] */
    BUILTIN_SCAN_INT,      /* scan_int(string) -> int     (like C's atoi)   */
    BUILTIN_SCAN_FLOAT,    /* scan_float(string) -> float (like C's atof)   */
    BUILTIN_SCAN_INT_LINE, /* scan_int_line() -> int  (reads stdin)         */
    BUILTIN_SCAN_FLOAT_LINE,/* scan_float_line() -> float (reads stdin)     */

    /* tensor construction and shape builtins (v0.8.0) */
    BUILTIN_TENSOR,  /* tensor([d0,...], [float]) -> tensor[...] */
    BUILTIN_ZEROS,   /* zeros([d0,...])  -> tensor[...]          */
    BUILTIN_ONES,    /* ones([d0,...])   -> tensor[...]          */
    BUILTIN_RAND,    /* rand([d0,...])   -> tensor[...]          */
    BUILTIN_SEED,    /* seed(int)        -> void                 */
    BUILTIN_SHAPE,   /* shape(tensor)    -> [int]                */

    /* linear algebra */
    BUILTIN_MATMUL,  /* matmul(MxK, KxN) -> MxN  (K checked at compile time) */
    BUILTIN_DOT,     /* dot([K], [K])    -> float                         */

    /* activations: every one preserves the shape of its argument */
    BUILTIN_RELU,
    BUILTIN_SIGMOID,
    BUILTIN_TANH,
    BUILTIN_GELU,
    BUILTIN_SOFTMAX, /* normalizes along the last axis */

    /* reductions: a tensor becomes a scalar */
    BUILTIN_SUM,
    BUILTIN_MEAN,
    BUILTIN_MAX,
    BUILTIN_MIN,
    BUILTIN_ARGMAX,  /* -> int, the flattened index of the largest element */

    /* losses */
    BUILTIN_MSE,           /* mse(pred, target) -> float, same shape */
    BUILTIN_CROSS_ENTROPY, /* cross_entropy(logits[N], class) -> float */

    /* Differentiation (v0.9.0). Every one of these is either a step that
     * grad() needs and that is worth having in its own right, or one of the
     * two transposed matrix products the backward pass of matmul() is built
     * from. None of the names is user-definable - see is_reserved_builtin_name
     * - because the gradient code sema emits refers to them by name. */
    BUILTIN_STEP,          /* step(t)  -> elementwise Heaviside, 1 where x > 0 */
    BUILTIN_MATMUL_TN,     /* matmul_tn(A, B) = A^T * B                      */
    BUILTIN_MATMUL_NT,     /* matmul_nt(A, B) = A * B^T                      */
    BUILTIN_XENT_GRAD      /* cross_entropy_grad(logits, class) -> tensor     */
} Builtin;

typedef struct Func Func;
typedef struct Block Block;
typedef struct Expr Expr;

struct Expr {
    ExprKind kind;
    int line;
    int col;
    const struct SourceFile *src; /* file this node was parsed from */
    Type type; /* filled in by semantic analysis */
    union {
        long ival;   /* EX_INT */
        double dval; /* EX_FLOAT */
        int bval;    /* EX_BOOL */
        char *sval;  /* EX_STRING */
        struct {
            char *name;
            int offset; /* stack slot, filled in by semantic analysis */
        } var; /* EX_VAR */
        struct {
            UnaryOp op;
            Expr *operand;
        } unary;
        struct {
            BinaryOp op;
            Expr *lhs;
            Expr *rhs;
        } binary;
        struct {
            char *name;
            Expr **args;
            int nargs;
            Func *fn;        /* resolved callee */
            Builtin builtin;
            StructDecl *sdef; /* struct being constructed (NULL otherwise) */
            int slot;         /* hidden stack slot holding the block */
        } call;
        struct {
            Expr **elems;
            int nelems;
            int slot; /* hidden stack slot holding the block, set by sema */
        } array; /* EX_ARRAY */
        struct {
            Expr *obj; /* array or string */
            Expr *idx;
        } index; /* EX_INDEX */
        struct {
            Expr *obj;      /* value of struct type */
            char *name;     /* field name */
            int offset;     /* byte offset in the block, set by sema */
        } field; /* EX_FIELD */
    };
};

/* ---------- statements -------------------------------------------------- */
typedef enum {
    ST_LET,
    ST_ASSIGN,
    ST_EXPR,
    ST_IF,
    ST_WHILE,
    ST_FOR,
    ST_BREAK,
    ST_CONTINUE,
    ST_RETURN,
    ST_BLOCK
} StmtKind;

typedef struct Stmt Stmt;

struct Stmt {
    StmtKind kind;
    int line;
    int col;
    const struct SourceFile *src; /* file this node was parsed from */
    union {
        struct {
            char *name;
            Type ann;     /* declared type, only valid when has_ann */
            int has_ann;
            Expr *init;
            int offset;   /* stack slot, filled in by semantic analysis */
        } let;
        struct {
            char *name;
            Expr *target; /* EX_INDEX when assigning to an element, else NULL */
            Expr *value;
            int offset;   /* resolved stack slot */
        } assign;
        Expr *expr;    /* ST_EXPR */
        Block *block;  /* ST_BLOCK: nested block used as a statement */
        struct {
            Expr *cond;
            Block *then_block;
            /* When parsing `else if`, the else branch holds a Block that
             * contains a single ST_IF statement. */
            Block *else_block;
        } ifs;
        struct {
            Expr *cond;
            Block *body;
        } whiles;
        struct {
            char *var_name;
            Expr *start; /* inclusive lower bound */
            Expr *end;   /* exclusive upper bound */
            Block *body;
            int var_offset; /* loop variable slot, set by sema */
            int end_offset; /* hidden slot holding the range end */
        } fors;
        Expr *value; /* ST_RETURN; NULL for a bare `send;` */
    };
};

struct Block {
    Stmt **stmts;
    int nstmts;
    int line;
    int col;
};

/* ---------- functions ---------------------------------------------------- */
typedef struct Param {
    char *name;
    Type type;
    int line;
    int col;
    const struct SourceFile *src;
} Param;

/* ---------- constants ----------------------------------------------------- */
/* A top-level `const NAME = <literal>;`. The initializer is a plain literal
 * (int, float, bool or string); references are replaced by the literal
 * during semantic analysis, so constants never occupy storage. */
typedef struct Const {
    char *name;
    Expr *value;
    int line;
    int col;
    const struct SourceFile *src;
} Const;

/* ---------- structs ------------------------------------------------------- */
/* A top-level `struct Name { field: Type, ... };`. A struct value is a
 * reference to a heap block of 8-byte field slots (no header), so every
 * field - including another struct - occupies exactly 8 bytes. */
typedef struct Field {
    char *name;
    Type type;
    int line;
    int col;
    const struct SourceFile *src;
} Field;

struct StructDecl {
    char *name;
    Field **fields;
    int nfields;
    Type type;  /* TY_STRUCT_BASE + registry index */
    int line;
    int col;
    const struct SourceFile *src;
};

struct Func {
    char *name;
    Param **params;
    int nparams;
    Type ret;
    Block *body;
    int line;
    int col;
    const struct SourceFile *src;
    int frame_size; /* bytes of stack for parameters + locals, set by sema */
    int is_extern;  /* `extern fn ...;` - declared, never defined here */
    int is_variadic; /* trailing `...` (only valid on extern declarations) */
};

typedef struct Program {
    Func **funcs;
    int nfuncs;
    Const **consts;
    int nconsts;
    StructDecl **structs;
    int nstructs;
} Program;

const char *unary_op_name(UnaryOp op);
const char *binary_op_name(BinaryOp op);

#endif /* DUCK_AST_H */
