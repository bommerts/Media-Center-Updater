/* A small JSON reader and writer. Text is UTF-16 in memory and UTF-8 on disk and on the network. */
#ifndef MCU_JSON_H
#define MCU_JSON_H

#include "common.h"

typedef enum { JSON_NULL, JSON_FALSE, JSON_TRUE, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonKind;

typedef struct JsonValue
{
    JsonKind kind;
    const wchar_t *name;        /* member name when the parent is an object */
    const wchar_t *text;        /* string value, or the digits of a number */
    struct JsonValue *first;    /* first child of an array or object */
    struct JsonValue *next;     /* next sibling */
    int count;                  /* number of children */
} JsonValue;

typedef struct JsonDoc { JsonValue *root; Arena arena; } JsonDoc;

JsonDoc *json_parse(const wchar_t *text);                    /* NULL when the text is not valid JSON */
JsonDoc *json_parse_utf8(const char *data, size_t length);
void json_free(JsonDoc *doc);

const JsonValue *json_get(const JsonValue *object, const wchar_t *name);   /* NULL when missing or not an object */
/* The member as text: a string's value, a number's digits, "True"/"False", or "" when missing. */
const wchar_t *json_text(const JsonValue *object, const wchar_t *name);
/* The member's value only when it is a string, otherwise "". */
const wchar_t *json_string(const JsonValue *object, const wchar_t *name);
bool json_is_true(const JsonValue *object, const wchar_t *name);
bool json_int(const JsonValue *value, int *out);            /* whole numbers that fit an int */

void json_write_string(StrBuf *sb, const wchar_t *text);     /* quoted and escaped */

#endif
