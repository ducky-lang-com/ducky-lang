/* parser.h - recursive descent parser entry point. */
#ifndef DUCK_PARSER_H
#define DUCK_PARSER_H

#include "ast.h"
#include "common.h"
#include "lexer.h"

Program *parse(const SourceFile *src, Token *toks, int ntoks);

/* Intern every struct name declared in this token stream. Must be called for
 * every file of the program before the first parse() call. */
void intern_struct_declarations(const SourceFile *src, Token *toks, int ntoks);

/* Record every `const NAME = <int literal>;` of this token stream so tensor
 * dimensions written as `tensor[NAME]` can be resolved while parsing a type.
 * Must be called for every file of the program before the first parse(). */
void collect_const_ints(const SourceFile *src, Token *toks, int ntoks);

#endif /* DUCK_PARSER_H */
