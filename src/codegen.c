#include "codegen.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

#include "astwalker.h"
#include "core.h"
#include "hash.h"
#include "opcodes.h"

#define CODE ((codegen_t*)self->data)->code
#define LOCALS ((codegen_t*)self->data)->locals
#define CONSTANTS ((codegen_t*)self->data)->constants
#define SYMTABLE ((codegen_t*)self->data)->symtable

#define GET_CONTEXT vector_peek(((codegen_t*)self->data)->decls)
#define CONTEXT_PEEKN(_n) vector_get(((codegen_t*)self->data)->decls, vector_size(((codegen_t*)self->data)->decls) - _n)
#define PUSH_CONTEXT(x) push_context((codegen_t*)self->data, x)
#define POP_CONTEXT pop_context((codegen_t*)self->data)

#define AS_GEN(self) ((codegen_t*)self->data)

#define MAX_LITERAL_INT 256

static void codegen_error(astwalker_t *self, const char *msg, ...)
{
    printf("error: ");
    va_list args;

    va_start(args, msg);
    vprintf(msg, args);
    va_end(args);
    printf("\n");

    self->nerrors++;
}

static void push_context(codegen_t *gen, value_t context)
{
    vector_push(value_t, gen->decls, context);
    if (IS_CLOSURE(context))
    {
        function_t *f = AS_CLOSURE(context)->f;
        gen->code = &f->bytecode;
        gen->constants = &f->constpool;
    }
}

static void pop_context(codegen_t *gen)
{
    vector_pop(gen->decls);
    value_t context = vector_peek(gen->decls);
    if (IS_CLOSURE(context))
    {
        function_t *f = AS_CLOSURE(context)->f;
        gen->code = &f->bytecode;
        gen->constants = &f->constpool;
    }
}

static void emit_byte(byte_r *code, uint8_t b)
{
    vector_push(uint8_t, *code, b);
}

static void emit_bytes(byte_r *code, uint8_t b1, uint8_t b2)
{
    vector_push(uint8_t, *code, b1);
    vector_push(uint8_t, *code, b2);
}

static void emit_loadstore(byte_r *code, location_e loc, uint8_t idx, bool store)
{
    if (loc == LOC_GLOBAL)
    {
        emit_bytes(code, store ? OP_STOREG : OP_LOADG, idx);
    }
    else if (loc == LOC_LOCAL)
    {
        emit_bytes(code, store ? OP_STOREL : OP_LOADL, idx);
    }
    else if (loc == LOC_UPVALUE)
    {
        emit_bytes(code, store ? OP_STOREU : OP_LOADU, idx);
    }
    else if (loc == LOC_CLASS)
    {
        if (store) emit_byte(code, OP_STOREF);
        else emit_bytes(code, OP_LOADF, 0);
    }
}

static uint8_t cpool_add_constant(value_r *cpool, value_t v)
{
    if (vector_size(*cpool) > 255)
    {
        printf("error: maximum amount of constants\n");
        return 255;
    }

    for (int i = 0; i < vector_size(*cpool); i++)
    {
        value_t val = vector_get(*cpool, i);
        if (value_equals(val, v)) return i;
    }

    vector_push(value_t, *cpool, v);
    return vector_size(*cpool) - 1;
}

static void gen_node_block(astwalker_t *self, node_block_t *node)
{
    for (int i = 0; i < vector_size(*node->stmts); i++)
    {
        walk_ast(self, vector_get(*node->stmts, i));
    }
}

static void gen_node_if(astwalker_t *self, node_if_t *node)
{
    walk_ast(self, node->cond);
    emit_bytes(CODE, (uint8_t)OP_JIF, 0);
    int idx = vector_size(*CODE) - 1;
    walk_ast(self, node->then);
    
    if (node->els)
    {
        // + 2 to skip (jmp n) instruction
        int jmp = vector_size(*CODE) - idx + 2;
        vector_set(*CODE, idx, (uint8_t)jmp);

        emit_bytes(CODE, (uint8_t)OP_JMP, 0);
        int idx2 = vector_size(*CODE) - 1;
        walk_ast(self, node->els);
        jmp = vector_size(*CODE) - idx2;
        vector_set(*CODE, idx2, (uint8_t)jmp);
    }
    else
    {
        int jmp = vector_size(*CODE) - idx;
        vector_set(*CODE, idx, (uint8_t)jmp);
    }
}

static void gen_loop_while_cfor(astwalker_t *self, node_loop_t *node)
{
    if (node->init)
    {
        walk_ast(self, node->init);
    }

    int loop_idx = vector_size(*CODE) - 1;
    walk_ast(self, node->cond);

    emit_bytes(CODE, (uint8_t)OP_JIF, 0);
    int jif_idx = vector_size(*CODE) - 1;
    walk_ast(self, node->body);
    if (node->inc)
    {
        walk_ast(self, node->inc);
    }

    int loop_jmp = vector_size(*CODE) - loop_idx;

    emit_bytes(CODE, (uint8_t)OP_LOOP, (uint8_t)loop_jmp);

    int jif_jmp = vector_size(*CODE) - jif_idx;
    vector_set(*CODE, jif_idx, (uint8_t)jif_jmp);
}

static void gen_loop_forin(astwalker_t *self, node_loop_t *node)
{
    uint8_t it_k = cpool_add_constant(CONSTANTS, FROM_CSTR(CORE_ITERATOR_STRING));
    uint8_t itval_k = cpool_add_constant(CONSTANTS, FROM_CSTR(CORE_ITER_VAL_STRING));
    uint8_t null_k = cpool_add_constant(CONSTANTS, FROM_NULL);
    walk_ast(self, node->init);
    emit_bytes(CODE, OP_LOADK, null_k);
    emit_bytes(CODE, OP_LOADK, null_k);

    // target
    walk_ast(self, node->cond);
    emit_loadstore(CODE, node->loc, node->target_idx, true);
    emit_loadstore(CODE, node->loc, node->target_idx, false);
    emit_bytes(CODE, OP_LOADK, it_k);
    emit_bytes(CODE, OP_LOADF, 1);
    emit_bytes(CODE, OP_CALL, 1);

    //iterator
    emit_loadstore(CODE, node->loc, node->it_idx, true);

    int loop_idx = vector_size(*CODE) - 1;
    emit_loadstore(CODE, node->loc, node->it_idx, false);
    emit_bytes(CODE, (uint8_t)OP_JIF, 0);
    int jif_idx = vector_size(*CODE) - 1;

    // iterator value
    node_var_decl_t *val = (node_var_decl_t*)node->init;
    emit_loadstore(CODE, node->loc, node->target_idx, false);
    emit_bytes(CODE, OP_LOADK, itval_k);
    emit_bytes(CODE, OP_LOADF, 1);
    emit_loadstore(CODE, node->loc, node->it_idx, false);
    emit_bytes(CODE, OP_CALL, 2);
    emit_loadstore(CODE, val->loc, val->idx, true);

    // body
    walk_ast(self, node->body);
    
    // iterator
    emit_loadstore(CODE, node->loc, node->target_idx, false);
    emit_bytes(CODE, OP_LOADK, it_k);
    emit_bytes(CODE, OP_LOADF, 1);
    emit_loadstore(CODE, node->loc, node->it_idx, false);
    emit_bytes(CODE, OP_CALL, 2);
    emit_loadstore(CODE, node->loc, node->it_idx, true);

    int loop_jmp = vector_size(*CODE) - loop_idx;
    emit_bytes(CODE, (uint8_t)OP_LOOP, (uint8_t)loop_jmp);

    int jif_jmp = vector_size(*CODE) - jif_idx;
    vector_set(*CODE, jif_idx, (uint8_t)jif_jmp);
}

static void gen_node_loop(astwalker_t *self, node_loop_t *node)
{
    switch (node->type)
    {
    case LOOP_CFOR:
    case LOOP_WHILE:
        gen_loop_while_cfor(self, node); break;
    case LOOP_FORIN:
        gen_loop_forin(self, node); break;
    default: break;
    }
}

static void gen_node_return(astwalker_t *self, node_return_t *node)
{
    walk_ast(self, node->expr);
    emit_byte(CODE, (uint8_t)OP_RETURN);
}

static void store_decl(astwalker_t *self, value_t decl, bool isstatic, node_func_decl_t *node)
{
    value_t context = GET_CONTEXT;
    bool env_initf = IS_CLOSURE(context) && strcmp(AS_CLOSURE(context)->f->identifier, CORE_INIT_STRING) == 0;

    if (env_initf)
    {
        function_t *contextf = AS_CLOSURE(context)->f;
        class_t *contextc = AS_CLASS(CONTEXT_PEEKN(2));
        if (isstatic) contextc = contextc->metaclass;
        const char *identifier = AS_CLOSURE(decl)->f->identifier;
        class_bind(contextc, identifier, decl);
        emit_bytes(&contextf->bytecode, OP_LOADL, 0);
        emit_bytes(&contextf->bytecode, OP_LOADK, cpool_add_constant(&contextf->constpool, FROM_CSTR(identifier)));
        emit_bytes(&contextf->bytecode, OP_LOADF, 0);
    }
    else
    {
        function_t *contextf = AS_CLOSURE(context)->f;
        vector_push(value_t, contextf->constpool, decl);
        emit_bytes(&contextf->bytecode, (uint8_t)OP_LOADK, vector_size(contextf->constpool) - 1);

        if (IS_CLOSURE(decl))
        {
            function_t *f = AS_CLOSURE(decl)->f;

            ast_upvalue_r *upvalues = node->upvalues;
            f->nupvalues = vector_size(*upvalues);
            emit_byte(CODE, (uint8_t)OP_CLOSURE);

            uint8_t upindex = 0;
            for (uint8_t i = 0; i < f->nupvalues; i++)
            {
                ast_upvalue_t upvalue = vector_get(*upvalues, i);
                emit_bytes(&contextf->bytecode, (uint8_t)OP_NEWUP, (uint8_t)upvalue.is_direct);
                emit_byte(&contextf->bytecode, upvalue.is_direct ? upvalue.idx : upindex++);
            }
        }
    }
}

static void gen_node_var_decl(astwalker_t *self, node_var_decl_t *node)
{
    value_t context = GET_CONTEXT;
    bool env_local = IS_CLOSURE(context);
    bool env_class = IS_CLASS(context);

    if (env_local)
    {
        if (node->init)
        {
            walk_ast(self, node->init);
        }
        else
        {
            value_r *cpool = &AS_CLOSURE(context)->f->constpool;
            emit_bytes(CODE, (uint8_t)OP_LOADK, cpool_add_constant(cpool, FROM_NULL));
        }
        emit_loadstore(CODE, node->loc, node->idx, true);
    }
    else if (env_class)
    {
        class_t *c = AS_CLASS(context);

        if (node->storage.type == TOK_STATIC)
            c = c->metaclass;

        class_bind(c, node->ident, FROM_INT(node->idx));

        if (node->init)
        {
            value_t lookup = FROM_CSTR(CORE_INIT_STRING);
            closure_t *initf = class_lookup_closure(c, lookup);
            string_free(AS_STR(lookup));
            if (!initf) return;

            PUSH_CONTEXT(FROM_CLOSURE(initf));
            walk_ast(self, node->init);
            POP_CONTEXT;

            //if (node->init->type != NODE_FUNC_DECL)
            {
                emit_bytes(&initf->f->bytecode, (uint8_t)OP_LOADL, 0);
                emit_bytes(&initf->f->bytecode, (uint8_t)OP_LOADI, node->idx);
                emit_loadstore(&initf->f->bytecode, LOC_CLASS, node->idx, true);
            }
        }
    }
}

static void gen_node_func_decl(astwalker_t *self, node_func_decl_t *node)
{
    function_t *f = function_new(strdup(node->identifier));
    closure_t *cl = closure_new(f);

    PUSH_CONTEXT(FROM_CLOSURE(cl));

    walk_ast(self, (node_t*)node->body);
    if (vector_get(*CODE, vector_size(*CODE) - 1) != OP_RETURN)
        emit_byte(CODE, (uint8_t)OP_RET0);

    POP_CONTEXT;

    bool is_static;
    if (!node->parent) is_static = false;
    else is_static = node->parent->storage.type == TOK_STATIC;

    store_decl(self, FROM_CLOSURE(cl), is_static, node);
}

static void gen_node_class_decl(struct astwalker *self, node_class_decl_t *node)
{
    class_t *c = 
        class_new_with_meta(strdup(node->identifier), node->num_instvars, node->num_staticvars, melon_class_object);
    c->meta_inited = false;
    closure_t *meta_init = NULL;
    if (node->num_staticvars > 0)
    {
        meta_init = closure_new(function_new(strdup(CORE_INIT_STRING)));
        class_bind(c->metaclass, CORE_INIT_STRING, FROM_CLOSURE(meta_init));
    }
    closure_t *init = closure_new(function_new(strdup(CORE_INIT_STRING)));
    class_bind(c, CORE_INIT_STRING, FROM_CLOSURE(init));

    PUSH_CONTEXT(FROM_CLASS(c));

    for (size_t i = 0; i < vector_size(*node->decls); i++)
    {
        walk_ast(self, vector_get(*node->decls, i));
    }

    POP_CONTEXT;

    emit_bytes(&init->f->bytecode, (uint8_t)OP_LOADL, 0);
    node_var_decl_t *constructor = node->constructor;
    if (constructor)
    {
        node_func_decl_t *constr_node = (node_func_decl_t*)constructor->init;
        emit_bytes(&init->f->bytecode, (uint8_t)OP_LOADI, constructor->idx);
        emit_bytes(&init->f->bytecode, (uint8_t)OP_LOADF, 1);
        uint8_t nparams = constr_node->params ? vector_size(*constr_node->params) : 0;
        for (size_t i = 0; i < nparams; i++)
        {
            emit_bytes(&init->f->bytecode, (uint8_t)OP_LOADL, i + 1);
        }
        emit_bytes(&init->f->bytecode, (uint8_t)OP_CALL, 1 + nparams);
        emit_bytes(&init->f->bytecode, (uint8_t)OP_LOADL, 0);
    }

    emit_byte(&init->f->bytecode, (uint8_t)OP_RETURN);

    if (meta_init)
    {
        emit_bytes(&meta_init->f->bytecode, (uint8_t)OP_LOADL, 0);
        emit_byte(&meta_init->f->bytecode, (uint8_t)OP_RETURN);
    }

    store_decl(self, FROM_CLASS(c), false, NULL);

    emit_loadstore(CODE, LOC_GLOBAL, node->idx, true);
}

static void gen_node_binary(astwalker_t *self, node_binary_t *node)
{
    if (node->op.type == TOK_EQ)
    {
        if (node->right->type == NODE_FUNC_DECL)
            node->right->is_assign = true;
        walk_ast(self, node->right);
        node->left->is_assign = true;
        walk_ast(self, node->left);
        return;
    }
    walk_ast(self, node->left);
    walk_ast(self, node->right);
    emit_byte(CODE, (uint8_t)token_to_binary_op(node->op));
}

static void gen_node_unary(astwalker_t *self, node_unary_t *node)
{
    walk_ast(self, node->right);
    emit_byte(CODE, (uint8_t)token_to_unary_op(node->op));
}

static void gen_node_postfix(astwalker_t *self, node_postfix_t *node)
{
    walk_ast(self, node->target);

    // bool is_method = vector_get(*node->exprs, 0)->type == POST_ACCESS;
    int len = vector_size(*node->exprs);

    for (int i = 0; i < len; i++)
    {
        postfix_expr_t *expr = vector_get(*node->exprs, i);
        if (expr->type == POST_CALL)
        {
            bool is_method = i > 0 && vector_get(*node->exprs, i - 1)->type == POST_ACCESS;
            uint8_t nargs = expr->args ? vector_size(*expr->args) : 0;

            for (size_t j = 0; j < nargs; j++)
            {
                walk_ast(self, vector_get(*expr->args, j));
            }

            if (is_method) nargs++;
            emit_bytes(CODE, (uint8_t)OP_CALL, nargs);
        }
        else if (expr->type == POST_ACCESS)
        {
            bool is_method = i < len - 1 && vector_get(*node->exprs, i + 1)->type == POST_CALL;
            node_var_t *var = (node_var_t*)expr->accessor;
            emit_bytes(CODE, (uint8_t)OP_LOADK, 
                cpool_add_constant(CONSTANTS, FROM_CSTR(var->identifier)));
            if (node->base.is_assign && i == len - 1)
                emit_byte(CODE, OP_STOREF);
            else
                emit_bytes(CODE, OP_LOADF, is_method ? 1 : 0);
        }
        else if (expr->type == POST_SUBSCRIPT)
        {
            walk_ast(self, expr->accessor);
            if (node->base.is_assign && i == len - 1)
                emit_byte(CODE, OP_STOREA);
            else
                emit_byte(CODE, OP_LOADA);
        }
    }

}

static void gen_node_var(astwalker_t *self, node_var_t *node)
{
    if (node->location == LOC_CLASS)
    {
        emit_bytes(CODE, OP_LOADL, 0);
        emit_bytes(CODE, OP_LOADI, node->idx);
    }
    emit_loadstore(CODE, node->location, node->idx, node->base.is_assign);
}

static void gen_node_list(struct astwalker *self, node_list_t *node)
{
    uint32_t len = vector_size(*node->items);
    if (len > 255)
    {
        codegen_error(self, "list size is greater than max [255]");
        return;
    }

    for (size_t i = 0; i < len; i++)
    {
        walk_ast(self, vector_get(*node->items, i));
    }
    emit_bytes(CODE, OP_NEWARR, len);
}

static void gen_node_range(struct astwalker *self, node_range_t *node)
{
    walk_ast(self, node->start);
    walk_ast(self, node->end);
    emit_byte(CODE, OP_NEWRNG);
}

static void gen_node_literal(astwalker_t *self, node_literal_t *node)
{
    function_t *context = AS_CLOSURE(GET_CONTEXT)->f;
    value_r *constpool = &context->constpool;
    byte_r *code = &context->bytecode;

    switch (node->type)
    {
    case LITERAL_BOOL:
    {
        emit_bytes(code, OP_LOADK, 
            cpool_add_constant(constpool, FROM_BOOL(node->u.i)));
        break;
    }
    case LITERAL_INT:
    {
        if (node->u.i >= 0 && node->u.i < MAX_LITERAL_INT)
        {
            emit_bytes(code, OP_LOADI, (uint8_t)node->u.i);
            return;
        }
        else
        {
            emit_bytes(code, OP_LOADK, 
                cpool_add_constant(constpool, FROM_INT(node->u.i)));
        }
        break;
    }
    case LITERAL_FLT:
    {
        emit_bytes(code, OP_LOADK, 
            cpool_add_constant(constpool, FROM_FLOAT(node->u.d)));
        break;
    }
    case LITERAL_STR:
    {
        emit_bytes(code, OP_LOADK, 
            cpool_add_constant(constpool, FROM_CSTR(node->u.s)));
        break;
    }
    default: break;
    }
}

codegen_t codegen_create(function_t *f)
{
    codegen_t gen;

    gen.main_cl = closure_new(f);
    gen.code = &f->bytecode;
    gen.constants = &f->constpool;
    vector_init(gen.decls);
    vector_push(value_t, gen.decls, FROM_CLOSURE(gen.main_cl));
    return gen;
}

void codegen_destroy(codegen_t *gen)
{
    vector_destroy(gen->decls);
    free(gen->main_cl);
}

bool codegen_run(codegen_t *gen, node_t *ast)
{
    astwalker_t walker = {
        .nerrors = 0,
        .depth = 0,
        .data = (void*)gen,

        .visit_block = gen_node_block,
        .visit_if = gen_node_if,
        .visit_loop = gen_node_loop,
        .visit_return = gen_node_return,

        .visit_var_decl = gen_node_var_decl,
        .visit_func_decl = gen_node_func_decl,
        .visit_class_decl = gen_node_class_decl,

        .visit_binary = gen_node_binary,
        .visit_unary = gen_node_unary,
        .visit_postfix = gen_node_postfix,
        .visit_var = gen_node_var,
        .visit_list = gen_node_list,
        .visit_range = gen_node_range,
        .visit_literal = gen_node_literal
    };

    walk_ast(&walker, ast);
    emit_byte(gen->code, (uint8_t)OP_HALT);

    return walker.nerrors == 0;
}