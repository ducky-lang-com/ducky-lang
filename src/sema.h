/* sema.h - semantic analysis (name resolution + type checking). */
#ifndef DUCK_SEMA_H
#define DUCK_SEMA_H

#include "ast.h"
#include "common.h"

/* Resolves every variable to a stack slot, every call to a function,
 * annotates expression types and computes function frame sizes. */
void analyze(const SourceFile *src, Program *prog);

#endif /* DUCK_SEMA_H */
