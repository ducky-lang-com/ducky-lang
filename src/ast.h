/* ast.h - abstract syntax tree for Duck programs. */
#ifndef DUCK_AST_H
#define DUCK_AST_H

/* ---------- types ------------------------------------------------------ */
typedef enum {
    TY_INT,    /* 64-bit signed integer  */
    TY_BOOL,   /* boolean                */
    TY_STRING, /* NUL-terminated string  */
    TY_VOID    /* no value (only for calls) */
} Type;

static inline const char *type_name(Type t) {
    switch (t) {
    case TY_INT:    return "int";
    case TY_BOOL:   return "bool";
    case TY_STRING: return "string";
    case TY_VOID:   return "void";
    }
    return "?";
}

/* ---------- expressions ------------------------------------------------ */
typedef enum {
    EX_INT,
    EX_BOOL,
    EX_STRING,
    EX_VAR,
    EX_UNARY,
    EX_BINARY,
    EX_CALL
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
    BUILTIN_INPUT
} Builtin;

typedef struct Func Func;
typedef struct Block Block;
typedef struct Expr Expr;

struct Expr {
    ExprKind kind;
    int line;
    int col;
    Type type; /* filled in by semantic analysis */
    union {
        long ival; /* EX_INT */
        int bval;  /* EX_BOOL */
        char *sval; /* EX_STRING */
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
            Func *fn;  /* resolved callee */
            Builtin builtin;
        } call;
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
} Param;

struct Func {
    char *name;
    Param **params;
    int nparams;
    Type ret;
    Block *body;
    int line;
    int col;
    int frame_size; /* bytes of stack for parameters + locals, set by sema */
};

typedef struct Program {
    Func **funcs;
    int nfuncs;
} Program;

const char *unary_op_name(UnaryOp op);
const char *binary_op_name(BinaryOp op);

#endif /* DUCK_AST_H */
