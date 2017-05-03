#ifndef __CODEGEN__
#define __CODEGEN__

#include <stdint.h>

#include "ast.h"
#include "symtable.h"
#include "value.h"
#include "vector.h"

typedef struct
{
    byte_r *code;
    string_r locals;
    value_r *constants;
    function_t *func;
    symtable_t *symtable;
    node_func_decl_t *parent_func;

    vector_t(node_func_decl_t*) functions;
} codegen_t;

codegen_t codegen_create(function_t *f);
void codegen_destroy(codegen_t *gen);
void codegen_run(codegen_t *gen, node_t *ast);

#endif