#ifndef __PARSER__
#define __PARSER__

#include "ast.h"
#include "lexer.h"

node_t *parse(lexer_t *lexer);

#endif