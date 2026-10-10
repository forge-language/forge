#ifndef FORGE_PARSER_H
#define FORGE_PARSER_H

#include "ast.h"
#include "lexer.h"

Program parse_program(Lexer *lx);
Program parse_program_named(Lexer *lx, const char *path);

#endif
