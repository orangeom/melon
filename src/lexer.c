#include "lexer.h"

#include <stdio.h>
#include <ctype.h>

#include "vector.h"

static bool is_identifier(char c)
{
    return isalpha(c) || c == '_';
}

static bool is_digit(char c)
{
    return isdigit(c);
}

static bool is_punc(char c)
{
    return c == '(' || c == ')' || c == ',' || c == '.' || c == '{' || c == '}'
        || c == ';' || c == '[' || c == ']';
}

static bool is_comment(char c)
{
    return c == '#';
}

static bool is_op(char c)
{
    return c == '+' || c == '-' || c == '=' || c == '*' || c == '%' || c == '&' || 
        c == '|' || c == '<' || c == '>' || c == '!' || c == '/'; 
}

static bool is_string(char c)
{
    return c == '"' || c == '\'';
}

static bool is_space(char c)
{
    return isspace(c);
}

static void scan_comment(charstream_t *source)
{
    while (charstream_next(source) != '\n')
    {
        if (charstream_eof(source)) break;
    }
}

static token_t scan_string(charstream_t *source)
{
    charstream_next(source);

    int start = source->offset;
    int bytes = 0;

    while (!is_string(charstream_peek(source)))
    {
        charstream_next(source);
        bytes++;
        if (charstream_eof(source)) break;
    }
    charstream_next(source);

    return token_create(TOK_STR, start, bytes, source->line, source->col);
}

static bool is_number(char c)
{
    return isdigit(c) || c == '.'; 
}

static token_t scan_number(charstream_t *source)
{
    int start = source->offset;
    int bytes = 0;
    bool dot_found = false;

    while (is_number(charstream_peek(source)))
    {
        if (charstream_peek(source) == '.')
        {
            if (dot_found)
            {
                printf("Error: float cannot have more than one decimal point\n");
                return token_error();
            }
            dot_found = true;
        }

        bytes++;
        charstream_next(source);
        if (*(source->pos) == '.' && *(source->pos + 1) == '.') break;
        
        if (charstream_eof(source)) break;
    }

    return token_create(dot_found ? TOK_FLOAT : TOK_INT, start, bytes, source->line, source->col);
}

static bool strequals(const char *s1, int len, const char *s2)
{
    int i = 0;
    while (i < len)
    {
        if (s1[i] != s2[i]) return false;
        i++;
    }

    return true;
}

static token_type get_keyword(charstream_t *source, int start, int bytes)
{
    char *iden = (char*)source->buffer + start;
    if (bytes == 2)
    {
        if (strequals(iden, bytes, "if")) return TOK_IF;
        if (strequals(iden, bytes, "in")) return TOK_IN;
    }
    if (bytes == 3)
    {
        if (strequals(iden, bytes, "var")) return TOK_VAR;
        if (strequals(iden, bytes, "for")) return TOK_FOR;
    }
    if (bytes == 4)
    {
        if (strequals(iden, bytes, "func")) return TOK_FUNC;
        if (strequals(iden, bytes, "else")) return TOK_ELSE;
        if (strequals(iden, bytes, "true")) return TOK_TRUE;
    }
    if (bytes == 5)
    {
        if (strequals(iden, bytes, "while")) return TOK_WHILE;
        if (strequals(iden, bytes, "false")) return TOK_FALSE;
        if (strequals(iden, bytes, "class")) return TOK_CLASS;
    }
    if (bytes == 6)
    {
        if (strequals(iden, bytes, "return")) return TOK_RETURN;
        if (strequals(iden, bytes, "static")) return TOK_STATIC;
    }
    if (bytes == 8)
    {
        if (strequals(iden, bytes, "operator")) return TOK_OPERATOR;
    }
    return TOK_IDENTIFIER;
}

static token_t scan_identifier(charstream_t *source)
{
    uint32_t start = source->offset;
    uint32_t col = source->col;
    uint32_t bytes = 0;

    while (is_identifier(charstream_peek(source)) || 
        is_digit(charstream_peek(source)))
    {
        charstream_next(source);
        bytes++;
        if (charstream_eof(source)) break;
    }

    return token_create(get_keyword(source, start, bytes), start, bytes, source->line, col);
}

static token_t scan_punc(charstream_t *source)
{
    char c = charstream_next(source);
    char next = charstream_peek(source);
    if (c == '.' && next == '.')
    {
        token_t t = token_create(TOK_RANGE, source->offset - 1, 2, source->line, source->col - 1);
        charstream_next(source);
        return t;
    }
    return token_create(token_punc(c), source->offset - 1, 1, source->line, source->col);
}

static token_type get_op(charstream_t *source, int start, int bytes)
{
    if (bytes == 1)
    {
        char c = source->buffer[start];
        if (c == '=') return TOK_EQ;
        if (c == '!') return TOK_BANG;
        if (c == '+') return TOK_ADD;
        if (c == '-') return TOK_SUB;
        if (c == '*') return TOK_MUL;
        if (c == '/') return TOK_DIV;
        if (c == '%') return TOK_MOD;
        if (c == '<') return TOK_LT;
        if (c == '>') return TOK_GT;
    }
    else if (bytes == 2)
    {
        char c1 = source->buffer[start];
        char c2 = source->buffer[start + 1];
        if (c2 == '=')
        {
            if (c1 == '=') return TOK_EQEQ;
            if (c1 == '!') return TOK_NEQ;
            if (c1 == '<') return TOK_LTE;
            if (c1 == '>') return TOK_GTE;
            if (c1 == '+') return TOK_ADDEQ;
            if (c1 == '-') return TOK_SUBEQ;
            if (c1 == '*') return TOK_MULEQ;
            if (c1 == '/') return TOK_DIVEQ;
        }

        if (c1 == '&' && c2 == '&') return TOK_AND;
        if (c1 == '|' && c2 == '|') return TOK_OR;

    }
    printf("Error: Unrecognized op\n");
    return TOK_ERROR;
}

static token_t scan_op(charstream_t *source)
{
    int start = source->offset;
    int bytes = 0;

    while (is_op(charstream_peek(source)))
    {
        charstream_next(source);
        bytes++;
        if (charstream_eof(source)) break;
    }

    return token_create(get_op(source, start, bytes), start, bytes, source->line, source->col);
}

static token_t read_next(lexer_t *lexer)
{
    token_t token = token_error();

    if (charstream_eof(&lexer->source)) token = token_create(TOK_EOF, 0, 0, 0, 0);

    while (!charstream_eof(&lexer->source))
    {
        char c = charstream_peek(&lexer->source);

        if (is_space(c)) { charstream_next(&lexer->source); continue; }
        if (is_comment(c)) { scan_comment(&lexer->source); continue; }

        if (is_string(c)) { token = scan_string(&lexer->source); break; }
        if (is_digit(c)) { token = scan_number(&lexer->source); break; }
        if (is_identifier(c)) { token = scan_identifier(&lexer->source); break; }
        if (is_punc(c)) { token = scan_punc(&lexer->source); break; }
        if (is_op(c)) { token = scan_op(&lexer->source); break; }

        charstream_error(&lexer->source, "Can't handle character");
        charstream_next(&lexer->source);
    }

    if (charstream_eof(&lexer->source) && token.type == TOK_ERROR) 
        token = token_create(TOK_EOF, 0, 0, 0, 0);

    return token;
}

lexer_t lexer_create(const char *source)
{
    lexer_t lexer;
    lexer.source = charstream_create(source);
    lexer.nerrors = 0;

    vector_t(token_t) tokens;
    vector_init(tokens);
    token_t current = read_next(&lexer);
    while (current.type != TOK_EOF)
    {
        //printf("arg: %d %.*s\n", current.type, current.length, source + current.offset);
        if (current.type != TOK_ERROR) vector_push(token_t, tokens, current);
        else lexer.nerrors++;
        current = read_next(&lexer);
    }

    lexer.ntokens = vector_size(tokens);
    lexer.tokens = tokens.a;
    lexer.current = 0;

    return lexer;
}

void lexer_destroy(lexer_t *lexer)
{
    if (lexer)
    {
        if (lexer->tokens) free(lexer->tokens);
    }
}

token_t lexer_consume(lexer_t *lexer, token_type type)
{
    if (lexer_check(lexer, type)) return lexer_advance(lexer);
    lexer->nerrors++;
    return token_error();
}

token_t lexer_advance(lexer_t *lexer)
{
    return lexer->tokens[lexer->current++];
}

token_t lexer_peek(lexer_t *lexer)
{
    return lexer->tokens[lexer->current];
}

token_t lexer_previous(lexer_t * lexer)
{
    return lexer->tokens[lexer->current - 1];
}

bool lexer_match(lexer_t *lexer, token_type type)
{
    if (lexer_check(lexer, type)) 
    {
        lexer_advance(lexer);
        return true;
    }
    return false;
}

bool lexer_check(lexer_t *lexer, token_type type)
{
    return lexer_peek(lexer).type == type;
}

bool lexer_end(lexer_t *lexer)
{
    return lexer->current >= lexer->ntokens;
}