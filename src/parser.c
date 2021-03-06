#include "parser.h"

#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

typedef enum {
    PREC_LOWEST,    // literals
    PREC_ASSIGN,    // =
    PREC_OR,        // ||
    PREC_AND,       // &&
    PREC_COMP,      // < > <= >= == !=
    PREC_TERM,      // + -
    PREC_FACTOR,    // * / %
    PREC_UNARY,     // ! -
    PREC_CALL,      // ()
} precedence_t;

typedef node_t *(*prefix_func)(lexer_t *lexer, token_t token);
typedef node_t *(*infix_func)(lexer_t *lexer, node_t *node, token_t token);

typedef struct
{
    prefix_func prefix;
    infix_func infix;
    precedence_t prec;
} parse_rule;

#define RULE(prefix, infix, prec)  (parse_rule){ prefix, infix, prec }
#define PREFIX_OP(prec)            (parse_rule){ parse_unary, NULL, prec }
#define INFIX_OP(prec)             (parse_rule){ NULL, parse_infix, prec }
#define PREFIX_RULE(prec, fn)      (parse_rule){ fn, NULL, prec }
#define INFIX_RULE(prec, fn)       (parse_rule){ NULL, fn, prec }

static parse_rule rules[TOK_LAST];
static bool rules_initialized = false;

static bool whitespace_char(char c)
{
    return c == '\n' || c == '\t' || c == '\r';
}

static void print_error_line(const char *buffer, token_t token)
{
    const uint32_t max_length = 40;
    uint32_t start = token.offset;
    uint32_t end = token.offset;
    while (start >= 0 && token.offset - start < max_length
        && !whitespace_char(buffer[start]))
    {
        if (start == 0) break;
        start--;
    }
    if (whitespace_char(buffer[start])) start++;
    while (end < strlen(buffer) && end - token.offset < max_length
        && !whitespace_char(buffer[end]))
    {
        end++;
    }

    char linestr[128];
    sprintf(linestr, "        ");
    printf("%s", linestr);
    while (start < end)
    {
        putchar(buffer[start++]);
    }
    printf("\n");

    uint8_t ntabs = 0;
    int i = token.offset;
    while (i >= 0 && (int)token.offset - i <= token.col)
    {
        if (buffer[i] == '\t') ntabs++;
        if (buffer[i] == '\n') break;
        i--;
    }

    for (uint32_t i = 0; i < strlen(linestr) + token.col - ntabs; i++)
    {
        putchar(' ');
    }
    printf("^\n");
}

static void report_error(const char *msg, ...)
{
    printf("error: ");
    va_list args;

    va_start(args, msg);
    vprintf(msg, args);
    va_end(args);
}

static void parser_error(lexer_t *lexer, token_t token, const char *msg, ...)
{
    printf("line %d: error: ", token.line);
    va_list args;

    va_start(args, msg);
    vprintf(msg, args);
    va_end(args);

    print_error_line(lexer->source.buffer, token);

    lexer->nerrors++;
}

static char *substr(const char *s, int offset, int length)
{
    char *sub = (char*)calloc(length + 1, sizeof(char));
    sub[length] = '\0';

    for (int i = 0; i < length; i++)
    {
        sub[i] = s[offset + i];
    }

    return sub;
}

static bool parse_required(lexer_t *lexer, token_type type, bool report)
{
    token_t token = lexer_consume(lexer, type);
    if (token.type == TOK_ERROR)
    {
        if (report)
        {
            token_t next = lexer_advance(lexer);
            char *value = substr(lexer->source.buffer, next.offset, next.length);
            parser_error(lexer, next, "Expected token %s but got value %s\n", token_type_string(type), value);
            free(value);
        }
        return false;
    }
    return true;
}

static node_t *parse_expression(lexer_t *lexer);
static node_t *parse_precedence(lexer_t *lexer, precedence_t prec);
static node_t *parse_block(lexer_t *lexer);

static node_t *parse_num(lexer_t *lexer, token_t token)
{
    if (token.type == TOK_INT)
    {
        int val = strtol(lexer->source.buffer + token.offset, NULL, 10);
        return node_literal_int_new(val);
    }
    else if (token.type == TOK_FLOAT)
    {
        double val = strtod(lexer->source.buffer + token.offset, NULL);
        return node_literal_float_new(val);
    }
    
    report_error("Expected number\n");
    return NULL;
}

static node_t *parse_str(lexer_t *lexer, token_t token)
{
    char *str = substr(lexer->source.buffer, token.offset, token.length);
    return node_literal_str_new(str, token.length);
}

static node_t *parse_bool(lexer_t *lexer, token_t token)
{
    return node_literal_bool_new(token.type == TOK_TRUE);
}

static node_t *parse_array(lexer_t *lexer, token_t token)
{
    node_r *items = (node_r*)calloc(1, sizeof(node_r));

    if (lexer_match(lexer, TOK_CLOSED_BRACKET))
    {
        return node_list_new(items);
    }

    do
    {
        vector_push(node_t*, *items, parse_expression(lexer));
    } while (lexer_match(lexer, TOK_COMMA));

    parse_required(lexer, TOK_CLOSED_BRACKET, true);

    return node_list_new(items);
}

static node_t *parse_identifier(lexer_t *lexer, token_t token)
{
    char *name = substr(lexer->source.buffer, token.offset, token.length);
    return node_var_new(token, name);
}

static node_t *parse_nested_expr(lexer_t *lexer, token_t token)
{
    node_t *expr = parse_expression(lexer);
    parse_required(lexer, TOK_CLOSED_PAREN, true);
    return expr;
}

static postfix_expr_t *parse_postfix_call(lexer_t *lexer)
{
    if (lexer_match(lexer, TOK_CLOSED_PAREN))
    {
        return postfix_call_new(NULL);
    }

    node_r *args = (node_r*)calloc(1, sizeof(node_r));

    do
    {
        vector_push(node_t*, *args, parse_expression(lexer));
    } while (lexer_match(lexer, TOK_COMMA));
    
    parse_required(lexer, TOK_CLOSED_PAREN, true);
    return postfix_call_new(args);
}

static postfix_expr_t *parse_postfix_access(lexer_t *lexer)
{
    node_t *expr = parse_identifier(lexer, lexer_advance(lexer));
    return postfix_access_new(expr);
}

static postfix_expr_t *parse_postfix_subscript(lexer_t *lexer)
{
    node_t *expr = parse_expression(lexer);
    parse_required(lexer, TOK_CLOSED_BRACKET, true);
    return postfix_subscript_new(expr);
}

static node_t *parse_postfix(lexer_t *lexer, node_t *node, token_t token)
{
    postfix_expr_r *exprs = (postfix_expr_r*)calloc(1, sizeof(postfix_expr_r));
    vector_init(*exprs);

    postfix_expr_t *current = NULL;
    if (token.type == TOK_DOT) current = parse_postfix_access(lexer);
    else if (token.type == TOK_OPEN_PAREN) current = parse_postfix_call(lexer);
    else if (token.type == TOK_OPEN_BRACKET) current = parse_postfix_subscript(lexer);
    vector_push(postfix_expr_t*, *exprs, current);

    while (lexer_match(lexer, TOK_DOT) || lexer_match(lexer, TOK_OPEN_PAREN)
           || lexer_match(lexer, TOK_OPEN_BRACKET))
    {
        token_t previous = lexer_previous(lexer);
        if (previous.type == TOK_DOT) current = parse_postfix_access(lexer);
        else if (previous.type == TOK_OPEN_PAREN) current = parse_postfix_call(lexer);
        else if (previous.type == TOK_OPEN_BRACKET) current = parse_postfix_subscript(lexer);
        vector_push(postfix_expr_t*, *exprs, current);
    }

    return node_postfix_new(node, exprs);
}

static node_t *parse_range(lexer_t *lexer, node_t *node, token_t token)
{
    return node_range_new(node, parse_expression(lexer));
}

static node_var_r *parse_func_params(lexer_t *lexer)
{
    if (lexer_check(lexer, TOK_CLOSED_PAREN)) return NULL;

    node_var_r *params = (node_var_r*)calloc(1, sizeof(node_var_r));

    if (!lexer_check(lexer, TOK_CLOSED_PAREN))
    {
        do
        {
            vector_push(node_var_t*, *params, (node_var_t*)parse_expression(lexer));
        } while (lexer_match(lexer, TOK_COMMA));
    }

    return params;
}

static node_t *parse_func_expr(lexer_t *lexer, token_t token)
{
    parse_required(lexer, TOK_OPEN_PAREN, true);
    node_var_r *params = parse_func_params(lexer);
    parse_required(lexer, TOK_CLOSED_PAREN, true);

    node_t *body = parse_block(lexer);
    return node_func_decl_new((token_t){.type = TOK_FUNC}, strdup("{anonymous func}"), params, (node_block_t*)body);
}

static node_t *parse_unary(lexer_t *lexer, token_t token)
{
    node_t *node = parse_precedence(lexer, PREC_UNARY);
    return node_unary_new(token, node);
}

static node_t *parse_infix(lexer_t *lexer, node_t *node, token_t token)
{
    //printf("parse_infix: %d %.*s\n", token.type, token.length, lexer->source.buffer + token.offset);

    node_t *right = parse_precedence(lexer, rules[token.type].prec);

    if (token_is_op_assign(token))
    {
        token.type = token_op_assign_to_op(token);
        // This leads to node being double-freed later. As a temporary workaround, assume that
        // node is always a node_var_t and clone it.
        node_var_t* as_var = (node_var_t*)node;
        node_var_t* copy = (node_var_t*)node_var_new(as_var->base.token, strdup(as_var->identifier));
        right = node_binary_new(token, (node_t*)copy, right);
        token.type = TOK_EQ;
    }

    return node_binary_new(token, node, right);
}

static int get_precedence(lexer_t *lexer)
{
    parse_rule *rule = &rules[lexer_peek(lexer).type];
    return rule->prec;
}

static node_t *parse_precedence(lexer_t *lexer, precedence_t prec)
{
    token_t token = lexer_advance(lexer);
    //printf("parse_prec: %d %.*s\n", token.type, token.length, lexer->source.buffer + token.offset);

    prefix_func prefix = rules[token.type].prefix;

    if (!prefix)
    {
        parser_error(lexer, token, "Prefix parse function does not exist for token %s\n", token_type_string(token.type));
        //report_error("Prefix parse function does not exist for token %s\n", token_type_string(token.type));
        return NULL;
    }

    node_t *left = prefix(lexer, token);
    if (lexer_end(lexer)) goto token_end;

    while (prec < get_precedence(lexer))
    {
        token = lexer_advance(lexer);

        infix_func infix = rules[token.type].infix;
        left = infix(lexer, left, token);
        if (lexer_end(lexer)) goto token_end;
    }

token_end:
    return left;
}

static node_t *parse_expression(lexer_t *lexer)
{
    return parse_precedence(lexer, PREC_LOWEST);
}

static void init_parse_rules()
{
    if (rules_initialized) return;

    rules[TOK_OPEN_PAREN] = RULE(parse_nested_expr, parse_postfix, PREC_CALL);
    rules[TOK_OPEN_BRACKET] = RULE(parse_array, parse_postfix, PREC_CALL);
    rules[TOK_DOT] = INFIX_RULE(PREC_CALL, parse_postfix);
    rules[TOK_RANGE] = INFIX_RULE(PREC_CALL, parse_range);

    rules[TOK_TRUE] = PREFIX_RULE(PREC_LOWEST, parse_bool);
    rules[TOK_FALSE] = PREFIX_RULE(PREC_LOWEST, parse_bool);
    rules[TOK_INT] = PREFIX_RULE(PREC_LOWEST, parse_num);
    rules[TOK_FLOAT] = PREFIX_RULE(PREC_LOWEST, parse_num);
    rules[TOK_STR] = PREFIX_RULE(PREC_LOWEST, parse_str);
    rules[TOK_IDENTIFIER] = PREFIX_RULE(PREC_LOWEST, parse_identifier);
    rules[TOK_FUNC] = PREFIX_RULE(PREC_LOWEST, parse_func_expr);

    rules[TOK_EQ] = INFIX_OP(PREC_ASSIGN);
    rules[TOK_ADDEQ] = INFIX_OP(PREC_ASSIGN);
    rules[TOK_SUBEQ] = INFIX_OP(PREC_ASSIGN);
    rules[TOK_MULEQ] = INFIX_OP(PREC_ASSIGN);
    rules[TOK_DIVEQ] = INFIX_OP(PREC_ASSIGN);

    rules[TOK_BANG] = PREFIX_OP(PREC_UNARY);
    rules[TOK_SUB] = RULE(parse_unary, parse_infix, PREC_TERM);

    rules[TOK_AND] = INFIX_OP(PREC_AND);

    rules[TOK_OR] = INFIX_OP(PREC_OR);

    rules[TOK_EQEQ] = INFIX_OP(PREC_COMP);
    rules[TOK_NEQ] = INFIX_OP(PREC_COMP);
    rules[TOK_LT] = INFIX_OP(PREC_COMP);
    rules[TOK_GT] = INFIX_OP(PREC_COMP);
    rules[TOK_LTE] = INFIX_OP(PREC_COMP);
    rules[TOK_GTE] = INFIX_OP(PREC_COMP);

    rules[TOK_ADD] = INFIX_OP(PREC_TERM);

    rules[TOK_MUL] = INFIX_OP(PREC_FACTOR);
    rules[TOK_DIV] = INFIX_OP(PREC_FACTOR);
    rules[TOK_MOD] = INFIX_OP(PREC_FACTOR);

    rules_initialized = true;
}

static node_t *parse_if(lexer_t *lexer)
{
    node_t *cond = NULL;
    node_t *then = NULL;
    node_t *els = NULL;

    parse_required(lexer, TOK_OPEN_PAREN, true);
    cond = parse_expression(lexer);
    parse_required(lexer, TOK_CLOSED_PAREN, true);

    then = parse_block(lexer);

    if (lexer_match(lexer, TOK_ELSE))
    {
        // Handles else if {...
        if (lexer_match(lexer, TOK_IF))
            els = parse_if(lexer);
        else
            els = parse_block(lexer);
    }

    return node_if_new(cond, then, els);
}

static node_t *parse_while(lexer_t *lexer)
{
    node_t *cond = NULL;
    node_t *body = NULL;

    parse_required(lexer, TOK_OPEN_PAREN, true);
    cond = parse_expression(lexer);
    parse_required(lexer, TOK_CLOSED_PAREN, true);

    body = parse_block(lexer);
    return node_loop_while_new(cond, body);
}

static node_t *parse_var_decl(lexer_t *lexer, token_t storage);

static node_t *parse_for(lexer_t *lexer)
{
    node_t *init = NULL;
    node_t *body = NULL;

    parse_required(lexer, TOK_OPEN_PAREN, true);
    parse_required(lexer, TOK_VAR, true);
    init = parse_var_decl(lexer, token_none());

    if (lexer_match(lexer, TOK_IN))
    {
        node_t *target = parse_expression(lexer);
        parse_required(lexer, TOK_CLOSED_PAREN, true);

        body = parse_block(lexer);
        return node_loop_forin_new(init, target, body);
    }
    else
    {
        node_t *inc = NULL;
        node_t *cond = parse_expression(lexer);

        parse_required(lexer, TOK_SEMICOLON, true);
        inc = parse_expression(lexer);

        parse_required(lexer, TOK_CLOSED_PAREN, true);

        body = parse_block(lexer);
        return node_loop_cfor_new(init, cond, inc, body);
    }
}

static node_t *parse_return(lexer_t *lexer)
{
    node_t *expr = parse_expression(lexer);
    lexer_match(lexer, TOK_SEMICOLON);

    return node_return_new(expr);
}

static node_t *parse_expr_stmt(lexer_t *lexer)
{
    node_t *node = parse_expression(lexer);
    lexer_match(lexer, TOK_SEMICOLON);

    return node;
}

static node_t *parse_stmt(lexer_t *lexer)
{
    if (lexer_match(lexer, TOK_IF)) return parse_if(lexer);
    else if (lexer_match(lexer, TOK_WHILE)) return parse_while(lexer);
    else if (lexer_match(lexer, TOK_FOR)) return parse_for(lexer);
    else if (lexer_match(lexer, TOK_RETURN)) return parse_return(lexer);
    else return parse_expr_stmt(lexer);
}

static node_t *parse_var_decl(lexer_t *lexer, token_t storage)
{
    if (!parse_required(lexer, TOK_IDENTIFIER, false))
    {
        parser_error(lexer, lexer_previous(lexer), "Missing identifier for variable\n");
        return NULL;
    }
    token_t token = lexer_previous(lexer);
    char *ident = substr(lexer->source.buffer, token.offset, token.length);

    node_t *init = NULL;
    if (lexer_match(lexer, TOK_EQ))
    {
        init = parse_expression(lexer);
    }

    lexer_match(lexer, TOK_SEMICOLON);

    return node_var_decl_new(token, storage, ident, init);
}

static const char *op_to_core_str(token_type op)
{
    switch (op)
    {
    case TOK_ADD: return CORE_ADD_STRING;
    case TOK_SUB: return CORE_SUB_STRING;
    case TOK_MUL: return CORE_MUL_STRING;
    case TOK_DIV: return CORE_DIV_STRING;
    case TOK_EQEQ: return CORE_EQEQ_STRING;
    default: return NULL;
    }
}

static node_t *parse_func_decl(lexer_t *lexer, token_t storage, bool is_operator)
{
    if (is_operator)
    {
        token_type op = lexer_advance(lexer).type;
        if (op < TOK_ADD || op > TOK_OR)
        {
            parser_error(lexer, lexer_previous(lexer), "Invalid overload\n");
            return NULL;
        }
    }
    else
    {
        if (!parse_required(lexer, TOK_IDENTIFIER, false))
        {
            parser_error(lexer, lexer_previous(lexer), "Missing identifier for function\n");
            return NULL;
        }
    }

    token_t token = lexer_previous(lexer);
    char *ident = is_operator ? strdup(op_to_core_str(token.type)) : 
        substr(lexer->source.buffer, token.offset, token.length);

    parse_required(lexer, TOK_OPEN_PAREN, true);
    node_var_r *params = parse_func_params(lexer);
    parse_required(lexer, TOK_CLOSED_PAREN, true);

    node_t *body = parse_block(lexer);

    return node_var_decl_new(token, storage, ident,
        node_func_decl_new(token, strdup(ident), params, (node_block_t*)body));
}

static node_t *parse_class_decl(lexer_t *lexer)
{
    if (!parse_required(lexer, TOK_IDENTIFIER, false))
    {
        parser_error(lexer, lexer_previous(lexer), "Missing identifier for class\n");
        return NULL;
    }
    token_t token = lexer_previous(lexer);
    char *ident = substr(lexer->source.buffer, token.offset, token.length);

    node_block_t *body = (node_block_t*)parse_block(lexer);
    node_r *decls = body->stmts;
    free(body);

    return node_class_decl_new(token, ident, decls);
}

static node_t *parse_decl(lexer_t *lexer)
{
    token_t storage = token_none();
    if (lexer_match(lexer, TOK_STATIC))
        storage = lexer_previous(lexer);

    if (lexer_match(lexer, TOK_VAR)) 
        return parse_var_decl(lexer, storage);
    if (lexer_match(lexer, TOK_FUNC) || lexer_match(lexer, TOK_OPERATOR))
        return parse_func_decl(lexer, storage, lexer_previous(lexer).type == TOK_OPERATOR);
    if (lexer_match(lexer, TOK_CLASS))
        return parse_class_decl(lexer);

    return parse_stmt(lexer);
}

static node_t *parse_block(lexer_t *lexer)
{
    node_r *stmts = (node_r*)calloc(1, sizeof(node_r));
    vector_init(*stmts);

    parse_required(lexer, TOK_OPEN_BRACE, true);

    while (!lexer_check(lexer, TOK_CLOSED_BRACE))
    {
        node_t *node = parse_decl(lexer);
        if (!node)
        {
            report_error("Parsed node was null\n");
            lexer->nerrors++;
            goto error;
        }
        vector_push(node_t*, *stmts, node);
        if (lexer_end(lexer))
        {
            report_error("Unexpected end of file while parsing\n");
            lexer->nerrors++;
            goto error;
        }
    }

    parse_required(lexer, TOK_CLOSED_BRACE, true);

error:
    return node_block_new(stmts);
}

node_t *parse(lexer_t *lexer)
{
    init_parse_rules();

    node_r *stmts = (node_r*)calloc(1, sizeof(node_r));
    vector_init(*stmts);

    while (!lexer_end(lexer))
    {
        node_t *node = parse_decl(lexer);
        if (!node)
        {
            report_error("Parsed node was null\n");
            lexer->nerrors++;
            break;
        }
        vector_push(node_t*, *stmts, node);
    }

    return node_block_new(stmts);
}