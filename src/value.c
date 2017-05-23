#include "value.h"

#include "debug.h"
#include "hash.h"
#include "opcodes.h"

void value_destroy(value_t val)
{
    if (IS_STR(val))
        free(AS_STR(val));
    else if (IS_CLOSURE(val))
        closure_free(AS_CLOSURE(val));
    else if (IS_CLASS(val))
        class_free(AS_CLASS(val));
}

void value_print(value_t v)
{
    switch (v.type)
    {
    case VAL_BOOL: printf("[bool]: %s\n", AS_BOOL(v) == 1 ? "true" : "false"); break;
    case VAL_INT: printf("[int]: %d\n", AS_INT(v)); break;
    case VAL_STR: printf("[string]: %s\n", AS_STR(v)); break;
    case VAL_FLOAT: printf("[float]: %f\n", AS_FLOAT(v)); break;
    case VAL_CLOSURE: {
        if (AS_CLOSURE(v)->f->type == FUNC_MELON)
            printf("[closure]: %s\n", AS_CLOSURE(v)->f->identifier);
        else
            printf("[native function]\n");
        break;
    }
    case VAL_CLASS: printf("[class]: %s\n", AS_CLASS(v)->identifier); break;
    case VAL_INST: printf("[instance]\n"); break;
    default: break;
    }
}

bool value_equals(value_t v1, value_t v2)
{
    if (v1.type != v2.type) return false;

    if (IS_INT(v1) || IS_BOOL(v1))
    {
        return AS_INT(v1) == AS_INT(v2);
    }
    else if (IS_FLOAT(v1))
    {
        return AS_FLOAT(v1) == AS_FLOAT(v2);
    }
    else if (IS_STR(v1))
    {
        return strcmp(AS_STR(v1), AS_STR(v2)) == 0;
    }  
    return false;
}

function_t *function_native_new(melon_c_func cfunc)
{
    function_t *func = (function_t*)calloc(1, sizeof(function_t));
    func->type = FUNC_NATIVE;
    func->nupvalues = 0;
    func->cfunc = cfunc;
    return func;
}

function_t *function_new(const char *identifier)
{
    function_t *func = (function_t*)calloc(1, sizeof(function_t));
    func->type = FUNC_MELON;
    func->nupvalues = 0;
    func->identifier = identifier;
    vector_init(func->bytecode);
    vector_init(func->constpool);
    return func;
}

void function_free(function_t *func)
{
    if (!func) return;
    if (func->type == FUNC_MELON)
    {
        free(func->identifier);
        for (int i = 0; i < vector_size(func->constpool); i++)
        {
            value_t val = vector_get(func->constpool, i);
            if (val.type == VAL_CLOSURE)
                function_free(val.cl->f);
            value_destroy(val);
        }
        vector_destroy(func->constpool);
        vector_destroy(func->bytecode);
    }
    free(func);
}

value_t function_cpool_get(function_t *func, int idx)
{
    return vector_get(func->constpool, idx);
}

static void print_tabs(uint8_t ntabs)
{
    for (uint8_t i = 0; i < ntabs; i++)
    {
        printf("\t");
    }
}

static void internal_disassemble(function_t *func, uint8_t depth)
{
    if (func->type == FUNC_MELON)
    {
        print_tabs(depth); printf("disassembly of function \"%s\"\n", func->identifier);
        print_tabs(depth); printf("bytes: %d\n", vector_size(func->bytecode));

        uint32_t ninsts = 0;
        for (int i = 0; i < vector_size(func->bytecode); i++)
        {
            uint8_t op = vector_get(func->bytecode, i);
            ninsts++;
            print_tabs(depth + 1); printf("%s", op_to_str((opcode)op));
            if (op == OP_LOADI || op == OP_STOREL || op == OP_LOADL || op == OP_JIF
                || op == OP_JMP || op == OP_LOOP || op == OP_LOADK || op == OP_LOADG
                || op == OP_STOREG || op == OP_CALL || op == OP_LOADU || op == OP_STOREU
                || op == OP_NEWUP)
            {
                printf(" %d", vector_get(func->bytecode, ++i));
            }
            if (op == OP_NEWUP)
            {
                printf(", %d", vector_get(func->bytecode, ++i));
            }
            printf(ninsts % 8 == 0 ? "\n\n" : "\n");
        }
        printf("\n");
    }
}

static void internal_class_print(class_t *c, uint8_t depth);
static void internal_cpool_dump(function_t *func, uint8_t depth);

static void debug_print_val(value_t v, uint8_t depth)
{
    switch (v.type)
    {
    case VAL_BOOL: printf("[bool] %s\n", AS_BOOL(v) == 1 ? "true" : "false"); break;
    case VAL_INT: printf("[int] %d\n", AS_INT(v)); break;
    case VAL_FLOAT: printf("[float] %f\n", AS_FLOAT(v)); break;
    case VAL_STR: printf("[string] %s\n", AS_STR(v)); break;
    case VAL_CLOSURE:
    {
        printf("[function] %s\n", AS_CLOSURE(v)->f->identifier);
        internal_disassemble(AS_CLOSURE(v)->f, depth + 1);
        internal_cpool_dump(AS_CLOSURE(v)->f, depth + 1);
        break;
    }
    case VAL_CLASS:
    {
        printf("[class] %s\n", AS_CLASS(v)->identifier);
        internal_class_print(AS_CLASS(v), depth + 1); break;
    }
    default: break;
    }
}

void internal_class_print(class_t *c, uint8_t depth)
{
    print_tabs(depth); printf("nvars: %d\n", c->nvars);

    hashtable_dump(c->htable);
}

void internal_cpool_dump(function_t *func, uint8_t depth)
{
    if (func->type == FUNC_MELON)
    {
        print_tabs(depth); printf("function constants of \"%s\"\n", func->identifier);
        if (vector_size(func->constpool) == 0)
        {
            print_tabs(depth + 1); printf("none\n\n");
            return;
        }
        for (int i = 0; i < vector_size(func->constpool); i++)
        {
            value_t v = vector_get(func->constpool, i);
            print_tabs(depth + 1);
            debug_print_val(v, depth + 1);
        }
        printf("\n");
    }
}

void function_cpool_dump(function_t *func)
{
    internal_cpool_dump(func, 0);
}


void function_disassemble(function_t *func)
{
    internal_disassemble(func, 0);
}

upvalue_t *upvalue_new(value_t *value)
{
    upvalue_t *upvalue = (upvalue_t*)calloc(1, sizeof(upvalue_t));
    upvalue->value = value;
    upvalue->next = NULL;
    return upvalue;
}

void upvalue_free(upvalue_t *upvalue)
{
    free(upvalue);
}

closure_t *closure_new(function_t *func)
{
    closure_t *closure = (closure_t*)calloc(1, sizeof(closure_t));
    closure->f = func;
    closure->upvalues = NULL;
    return closure;
}

void closure_free(closure_t *closure)
{
    if (closure->upvalues)
    {
        upvalue_t *upvalue = *closure->upvalues;
        while (upvalue)
        {
            upvalue_t *next = upvalue->next;
            if (upvalue->value == &upvalue->closed)
                free(upvalue);
            upvalue = next;
        }
        free(closure->upvalues);
    }
    free(closure);
}

class_t *class_new(const char *identifier, uint16_t nvars)
{
    class_t *c = (class_t*)calloc(1, sizeof(class_t));
    c->identifier = identifier;
    c->nvars = nvars;
    c->htable = hashtable_new(384);
    return c;
}

void free_class_strings(hash_entry_t *node)
{
    if (IS_STR(node->key))
    {
        free(AS_STR(node->key));
    }
}

void class_free(class_t *c)
{
    if (c)
    {
        if (c->identifier) free(c->identifier);
        hashtable_iterate(c->htable, free_class_strings);
        hashtable_free(c->htable);
        free(c);
    }
}

void class_print(class_t *c)
{
    if (!c) return;

    internal_class_print(c, 0);
}

void class_bind(class_t *c, value_t key, value_t value)
{
    hashtable_set(c->htable, key, value);
}

value_t *class_lookup(class_t *c, value_t key)
{
    return hashtable_get(c->htable, key);
}

instance_t *instance_new(class_t *c)
{
    instance_t *inst = (instance_t*)calloc(1, sizeof(instance_t));
    inst->c = c;
    inst->nvars = c->nvars;
    inst->vars = (value_t*)calloc(inst->nvars, sizeof(value_t));
    return inst;
}

void instance_free(instance_t *inst)
{
    if (!inst) return;
    if (inst->vars) free(inst->vars);
    free(inst);
}