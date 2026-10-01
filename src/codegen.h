/* codegen.h - x86-64 code generation (AT&T syntax, System V ABI). */
#ifndef DUCK_CODEGEN_H
#define DUCK_CODEGEN_H

#include <stdio.h>

#include "ast.h"
#include "common.h"

/* Writes a complete assembly file: runtime support, the user's functions
 * and the string constant pool. */
void generate(const SourceFile *src, Program *prog, FILE *out);

#endif /* DUCK_CODEGEN_H */
