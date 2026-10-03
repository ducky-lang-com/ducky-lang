/* common.c - arena allocator, file reading and diagnostics. */
#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"

/* ---------- arena allocator ------------------------------------------- */

static ArenaBlock *g_arena;

/* The header must stay 32 bytes so the payload starts 16-byte aligned
 * (malloc guarantees 16-byte alignment on x86-64). */
_Static_assert(sizeof(ArenaBlock) == 32, "arena header must be 32 bytes");

void *arena_alloc(size_t nbytes) {
    size_t need = (nbytes + 15) & ~(size_t)15;
    if (need == 0) need = 16;

    if (!g_arena || g_arena->used + need > g_arena->cap) {
        size_t cap = need < (size_t)1 << 16 ? (size_t)1 << 16 : need;
        ArenaBlock *b = malloc(sizeof(ArenaBlock) + cap);
        if (!b) fatal("out of memory");
        b->next = g_arena;
        b->used = 0;
        b->cap = cap;
        b->header_pad = 0;
        g_arena = b;
    }

    void *p = (char *)(g_arena + 1) + g_arena->used;
    g_arena->used += need;
    return p;
}

char *arena_strndup(const char *s, size_t n) {
    char *p = arena_alloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ---------- source files ------------------------------------------------ */

const SourceFile *read_source(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fatal("cannot open '%s': %s", path, strerror(errno));

    if (fseek(f, 0, SEEK_END) != 0) fatal("cannot read '%s': %s", path, strerror(errno));
    long size = ftell(f);
    if (size < 0) fatal("cannot read '%s': %s", path, strerror(errno));
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (!buf) fatal("out of memory");

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';

    SourceFile *sf = malloc(sizeof(*sf));
    if (!sf) fatal("out of memory");
    sf->path = path;
    sf->src = buf;
    sf->len = got;
    return sf;
}

/* ---------- diagnostics -------------------------------------------------- */

static void print_source_line(const SourceFile *f, int line, int col) {
    const char *p = f->src;
    const char *end = f->src + f->len;
    int cur = 1;

    while (cur < line && p < end) {
        if (*p == '\n') cur++;
        p++;
    }
    const char *line_start = p;
    while (p < end && *p != '\n') p++;

    fprintf(stderr, "  %.*s\n", (int)(p - line_start), line_start);
    fprintf(stderr, "  %*s^\n", col > 0 ? col - 1 : 0, "");
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("duckyc: error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void fatal_at(const SourceFile *f, int line, int col, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: error: ", f->path, line, col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (f->src) print_source_line(f, line, col);
    exit(1);
}

/* ---------- struct type registry ------------------------------------------
 * Struct names are interned by a parser pre-pass before any declaration is
 * parsed, so a struct may be used before (or after) its declaration. Each
 * interned struct owns the Type value TY_STRUCT_BASE + index, which is what
 * makes the typing nominal: only a declaration can produce that integer.
 * The range stops at TY_TENSOR_BASE so struct and tensor kinds never share
 * an integer. */

static StructDecl **g_structs;
static int g_nstructs;
static int g_structs_cap;

StructDecl *struct_type_intern(const char *name) {
    if (g_nstructs == TY_TENSOR_BASE - TY_STRUCT_BASE) {
        fatal("too many distinct struct types (limit is %d)",
              TY_TENSOR_BASE - TY_STRUCT_BASE);
    }
    if (g_nstructs == g_structs_cap) {
        g_structs_cap = g_structs_cap ? g_structs_cap * 2 : 8;
        StructDecl **ns = realloc(g_structs,
                                  (size_t)g_structs_cap * sizeof(StructDecl *));
        if (!ns) fatal("out of memory");
        g_structs = ns;
    }
    StructDecl *sd = arena_alloc(sizeof(StructDecl));
    memset(sd, 0, sizeof(*sd));
    sd->name = arena_strndup(name, strlen(name));
    sd->type = TY_STRUCT_BASE + g_nstructs;
    g_structs[g_nstructs++] = sd;
    return sd;
}

StructDecl *struct_type_lookup(const char *name) {
    for (int i = 0; i < g_nstructs; i++) {
        if (strcmp(g_structs[i]->name, name) == 0) return g_structs[i];
    }
    return NULL;
}

StructDecl *struct_type_decl(Type t) {
    int idx = t - TY_STRUCT_BASE;
    if (idx < 0 || idx >= g_nstructs) return NULL;
    return g_structs[idx];
}

const char *struct_type_name(Type t) {
    StructDecl *sd = struct_type_decl(t);
    return sd ? sd->name : "?";
}

/* ---------- tensor shape registry ------------------------------------------
 * `tensor[2, 3]` and `tensor[3, 2]` are different types, and both differ
 * from `tensor[6]`. Each distinct shape is interned once and owns the Type
 * value TY_TENSOR_BASE + index, so shape equality is integer equality and
 * every diagnostic can print the canonical spelling kept in ts->name. */

static TensorShape **g_tensors;
static int g_ntensors;
static int g_tensors_cap;

TensorShape *tensor_type_intern(const int *dims, int rank) {
    for (int i = 0; i < g_ntensors; i++) {
        TensorShape *ts = g_tensors[i];
        if (ts->rank != rank) continue;
        int same = 1;
        for (int d = 0; d < rank; d++) {
            if (ts->dims[d] != dims[d]) { same = 0; break; }
        }
        if (same) return ts;
    }

    if (g_ntensors == g_tensors_cap) {
        g_tensors_cap = g_tensors_cap ? g_tensors_cap * 2 : 8;
        TensorShape **ns = realloc(g_tensors,
                                   (size_t)g_tensors_cap * sizeof(TensorShape *));
        if (!ns) fatal("out of memory");
        g_tensors = ns;
    }

    TensorShape *ts = arena_alloc(sizeof(TensorShape));
    memset(ts, 0, sizeof(*ts));
    ts->type = TY_TENSOR_BASE + g_ntensors;
    ts->rank = rank;
    for (int i = 0; i < rank; i++) ts->dims[i] = dims[i];

    /* The element count is known here and forever: check it once so that
     * code generation can size blocks without ever re-deriving it. */
    long n = 1;
    for (int i = 0; i < rank; i++) {
        if (n > 1000000000L / dims[i]) {
            fatal("tensor shape is too large: [%d] holds more than "
                  "1000000000 elements", dims[i]);
        }
        n *= dims[i];
    }
    ts->nelems = n;

    int off = snprintf(ts->name, sizeof(ts->name), "tensor[");
    for (int i = 0; i < rank && off < (int)sizeof(ts->name); i++) {
        off += snprintf(ts->name + off, sizeof(ts->name) - (size_t)off,
                        "%s%d", i ? ", " : "", dims[i]);
    }
    if (off < (int)sizeof(ts->name)) {
        snprintf(ts->name + off, sizeof(ts->name) - (size_t)off, "]");
    }

    g_tensors[g_ntensors++] = ts;
    return ts;
}

TensorShape *tensor_type_decl(Type t) {
    int idx = t - TY_TENSOR_BASE;
    if (idx < 0 || idx >= g_ntensors) return NULL;
    return g_tensors[idx];
}

const char *tensor_type_name(Type t) {
    TensorShape *ts = tensor_type_decl(t);
    return ts ? ts->name : "tensor[?]";
}

/* ---------- compile-time integer constants ---------------------------------
 * A tensor dimension must be known while the type is parsed, but `const`
 * declarations may appear anywhere in the program (and in any file of it).
 * The parser therefore runs a token pre-pass over every file first - the
 * same pre-pass that interns struct names - and records each
 * `const NAME = <int literal>;` here. Semantic analysis still owns the real
 * validation (duplicates, reserved names, non-integer initializers); this
 * table only answers "what integer is NAME" while parsing a tensor type. */

typedef struct CtInt {
    const char *name;
    long value;
    struct CtInt *next;
} CtInt;

static CtInt *g_ct_ints;

void ct_int_add(const char *name, long value) {
    for (CtInt *c = g_ct_ints; c; c = c->next) {
        if (strcmp(c->name, name) == 0) return; /* first declaration wins */
    }
    CtInt *c = arena_alloc(sizeof(CtInt));
    c->name = arena_strndup(name, strlen(name));
    c->value = value;
    c->next = g_ct_ints;
    g_ct_ints = c;
}

int ct_int_find(const char *name, long *out) {
    for (CtInt *c = g_ct_ints; c; c = c->next) {
        if (strcmp(c->name, name) == 0) {
            *out = c->value;
            return 1;
        }
    }
    return 0;
}

/* ---------- AST helper names -------------------------------------------- */

const char *unary_op_name(UnaryOp op) {
    switch (op) {
    case UOP_NEG: return "-";
    case UOP_NOT: return "!";
    case UOP_BITNOT: return "~";
    }
    return "?";
}

const char *binary_op_name(BinaryOp op) {
    switch (op) {
    case BOP_ADD: return "+";
    case BOP_SUB: return "-";
    case BOP_MUL: return "*";
    case BOP_DIV: return "/";
    case BOP_MOD: return "%";
    case BOP_EQ:  return "==";
    case BOP_NE:  return "!=";
    case BOP_LT:  return "<";
    case BOP_LE:  return "<=";
    case BOP_GT:  return ">";
    case BOP_GE:  return ">=";
    case BOP_AND: return "&&";
    case BOP_OR:  return "||";
    case BOP_BITAND: return "&";
    case BOP_BITOR:  return "|";
    case BOP_XOR:    return "^";
    case BOP_SHL:    return "<<";
    case BOP_SHR:    return ">>";
    }
    return "?";
}
