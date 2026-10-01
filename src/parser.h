/* parser.h - recursive descent parser entry point. */
#ifndef DUCK_PARSER_H
#define DUCK_PARSER_H

#include "ast.h"
#include "common.h"
#include "lexer.h"

Program *parse(const SourceFile *src, Token *toks, int ntoks);

#endif /* DUCK_PARSER_H */
