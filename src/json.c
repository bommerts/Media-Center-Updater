#include "json.h"

typedef struct Parser { const wchar_t *p; Arena *arena; int depth; bool failed; } Parser;

static JsonValue *parse_value(Parser *parser);

static void skip_space(Parser *parser)
{
    while (*parser->p == L' ' || *parser->p == L'\t' || *parser->p == L'\n' || *parser->p == L'\r') parser->p++;
}

static int hex_digit(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}

static const wchar_t *parse_string(Parser *parser)
{
    const wchar_t *p = parser->p + 1, *end = p;
    while (*end && *end != L'"') { if (*end == L'\\' && end[1]) end++; end++; }
    if (*end != L'"') { parser->failed = true; return NULL; }
    wchar_t *out = arena_alloc(parser->arena, ((size_t)(end - p) + 1) * sizeof(wchar_t));
    size_t n = 0;
    while (p < end)
    {
        wchar_t ch = *p++;
        if (ch < 0x20) { parser->failed = true; return NULL; }
        if (ch != L'\\') { out[n++] = ch; continue; }
        ch = *p++;
        switch (ch)
        {
        case L'"': case L'\\': case L'/': out[n++] = ch; break;
        case L'b': out[n++] = L'\b'; break;
        case L'f': out[n++] = L'\f'; break;
        case L'n': out[n++] = L'\n'; break;
        case L'r': out[n++] = L'\r'; break;
        case L't': out[n++] = L'\t'; break;
        case L'u':
        {
            int code = 0;
            for (int i = 0; i < 4; i++)
            {
                int digit = p + i < end ? hex_digit(p[i]) : -1;
                if (digit < 0) { parser->failed = true; return NULL; }
                code = code * 16 + digit;
            }
            p += 4;
            out[n++] = (wchar_t)code;
            break;
        }
        default: parser->failed = true; return NULL;
        }
    }
    out[n] = 0;
    parser->p = end + 1;
    return out;
}

static bool is_digit(wchar_t ch) { return ch >= L'0' && ch <= L'9'; }

static const wchar_t *parse_number(Parser *parser)
{
    const wchar_t *start = parser->p, *p = start;
    if (*p == L'-') p++;
    if (*p == L'0') p++;
    else if (is_digit(*p)) while (is_digit(*p)) p++;
    else { parser->failed = true; return NULL; }
    if (*p == L'.')
    {
        p++;
        if (!is_digit(*p)) { parser->failed = true; return NULL; }
        while (is_digit(*p)) p++;
    }
    if (*p == L'e' || *p == L'E')
    {
        p++;
        if (*p == L'+' || *p == L'-') p++;
        if (!is_digit(*p)) { parser->failed = true; return NULL; }
        while (is_digit(*p)) p++;
    }
    size_t length = (size_t)(p - start);
    wchar_t *out = arena_alloc(parser->arena, (length + 1) * sizeof(wchar_t));
    memcpy(out, start, length * sizeof(wchar_t));
    out[length] = 0;
    parser->p = p;
    return out;
}

static bool literal(Parser *parser, const wchar_t *word)
{
    size_t n = wcslen(word);
    if (wcsncmp(parser->p, word, n) != 0) return false;
    parser->p += n;
    return true;
}

static JsonValue *parse_container(Parser *parser, JsonValue *value, bool object)
{
    wchar_t close = object ? L'}' : L']';
    JsonValue **tail = &value->first;
    parser->p++;
    skip_space(parser);
    if (*parser->p == close) { parser->p++; return value; }
    for (;;)
    {
        const wchar_t *name = NULL;
        skip_space(parser);
        if (object)
        {
            if (*parser->p != L'"') { parser->failed = true; return NULL; }
            name = parse_string(parser);
            if (parser->failed) return NULL;
            skip_space(parser);
            if (*parser->p++ != L':') { parser->failed = true; return NULL; }
        }
        JsonValue *child = parse_value(parser);
        if (!child) return NULL;
        child->name = name;
        *tail = child;
        tail = &child->next;
        value->count++;
        skip_space(parser);
        if (*parser->p == L',') { parser->p++; continue; }
        if (*parser->p == close) { parser->p++; return value; }
        parser->failed = true;
        return NULL;
    }
}

static JsonValue *parse_value(Parser *parser)
{
    skip_space(parser);
    if (++parser->depth > 64) { parser->failed = true; return NULL; }
    JsonValue *value = arena_alloc(parser->arena, sizeof(JsonValue));
    memset(value, 0, sizeof(*value));
    wchar_t ch = *parser->p;
    if (ch == L'{' || ch == L'[')
    {
        value->kind = ch == L'{' ? JSON_OBJECT : JSON_ARRAY;
        if (!parse_container(parser, value, ch == L'{')) return NULL;
    }
    else if (ch == L'"')
    {
        value->kind = JSON_STRING;
        value->text = parse_string(parser);
    }
    else if (ch == L'-' || is_digit(ch))
    {
        value->kind = JSON_NUMBER;
        value->text = parse_number(parser);
    }
    else if (literal(parser, L"true")) value->kind = JSON_TRUE;
    else if (literal(parser, L"false")) value->kind = JSON_FALSE;
    else if (literal(parser, L"null")) value->kind = JSON_NULL;
    else parser->failed = true;
    parser->depth--;
    return parser->failed ? NULL : value;
}

JsonDoc *json_parse(const wchar_t *text)
{
    JsonDoc *doc = xalloc(sizeof(JsonDoc));
    Parser parser = { text, &doc->arena, 0, false };
    if (*parser.p == 0xFEFF) parser.p++;
    doc->root = parse_value(&parser);
    if (doc->root) skip_space(&parser);
    if (!doc->root || parser.failed || *parser.p)
    {
        json_free(doc);
        return NULL;
    }
    return doc;
}

JsonDoc *json_parse_utf8(const char *data, size_t length)
{
    if (!data || length > 0x7FFFFFF0) return NULL;
    wchar_t *wide = utf8_to_wide(data, (int)length);
    /* An embedded NUL would cut the text short and hide whatever follows it. */
    JsonDoc *doc = wcslen(wide) == (size_t)MultiByteToWideChar(CP_UTF8, 0, data, (int)length, NULL, 0) ? json_parse(wide) : NULL;
    free(wide);
    return doc;
}

void json_free(JsonDoc *doc)
{
    if (!doc) return;
    arena_free(&doc->arena);
    free(doc);
}

const JsonValue *json_get(const JsonValue *object, const wchar_t *name)
{
    if (!object || object->kind != JSON_OBJECT) return NULL;
    const JsonValue *found = NULL;
    for (const JsonValue *child = object->first; child; child = child->next)
        if (child->name && wcscmp(child->name, name) == 0) found = child;   /* the last duplicate wins */
    return found;
}

const wchar_t *json_text(const JsonValue *object, const wchar_t *name)
{
    const JsonValue *value = json_get(object, name);
    if (!value) return L"";
    switch (value->kind)
    {
    case JSON_STRING: case JSON_NUMBER: return value->text;
    case JSON_TRUE: return L"True";
    case JSON_FALSE: return L"False";
    default: return L"";
    }
}

const wchar_t *json_string(const JsonValue *object, const wchar_t *name)
{
    const JsonValue *value = json_get(object, name);
    return value && value->kind == JSON_STRING ? value->text : L"";
}

bool json_is_true(const JsonValue *object, const wchar_t *name)
{
    const JsonValue *value = json_get(object, name);
    return value && value->kind == JSON_TRUE;
}

bool json_int(const JsonValue *value, int *out)
{
    if (!value || value->kind != JSON_NUMBER) return false;
    const wchar_t *p = value->text;
    bool negative = *p == L'-';
    if (negative) p++;
    int64_t result = 0;
    if (!*p) return false;
    for (; *p; p++)
    {
        if (!is_digit(*p)) return false;
        result = result * 10 + (*p - L'0');
        if (result > 0x80000000LL) return false;
    }
    if (negative) result = -result;
    if (result > INT_MAX || result < INT_MIN) return false;
    *out = (int)result;
    return true;
}

void json_write_string(StrBuf *sb, const wchar_t *text)
{
    sb_append_ch(sb, L'"');
    for (; *text; text++)
    {
        switch (*text)
        {
        case L'"': sb_append(sb, L"\\\""); break;
        case L'\\': sb_append(sb, L"\\\\"); break;
        case L'\n': sb_append(sb, L"\\n"); break;
        case L'\r': sb_append(sb, L"\\r"); break;
        case L'\t': sb_append(sb, L"\\t"); break;
        default:
            if (*text >= 0xD800 && *text <= 0xDBFF && text[1] >= 0xDC00 && text[1] <= 0xDFFF)
            {
                sb_append_n(sb, text, 2);   /* a valid surrogate pair */
                text++;
            }
            /* Escape control characters and lone surrogates so the file is always valid UTF-8. */
            else if (*text < 0x20 || (*text >= 0xD800 && *text <= 0xDFFF))
                sb_appendf(sb, L"\\u%04X", (unsigned)*text);
            else sb_append_ch(sb, *text);
        }
    }
    sb_append_ch(sb, L'"');
}
