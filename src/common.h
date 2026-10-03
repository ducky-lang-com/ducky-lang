/* common.h - shared utilities for the duckyc compiler. */
#ifndef DUCK_COMMON_H
#define DUCK_COMMON_H

#include <stddef.h>

/* ---------- arena allocator -------------------------------------------
 * All compiler data (tokens, AST nodes, names) is allocated from a bump
 * allocator that lives for the whole compilation run. */
typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used;
    size_t cap;
    size_t header_pad; /* keeps the header 32 bytes so payloads stay 16-byte aligned */
} ArenaBlock;

void *arena_alloc(size_t nbytes);
char *arena_strndup(const char *s, size_t n);

/* ---------- source files ---------------------------------------------- */
typedef struct SourceFile {
    const char *path;
    const char *src;
    size_t len;
} SourceFile;

const SourceFile *read_source(const char *path);

/* ---------- diagnostics ------------------------------------------------
 * Errors print "file:line:col: error: message", the offending source line
 * and a caret, then terminate the compiler with exit status 1. */
void fatal(const char *fmt, ...);
void fatal_at(const SourceFile *f, int line, int col, const char *fmt, ...);

/* ---------- compile-time integer constants -------------------------------
 * Filled in by the parser pre-pass (see common.c) so that a tensor
 * dimension such as `tensor[HIDDEN, 10]` can be resolved while the type is
 * parsed, no matter where in the program the `const` was declared. */
void ct_int_add(const char *name, long value);
int ct_int_find(const char *name, long *out);

#endif /* DUCK_COMMON_H */
