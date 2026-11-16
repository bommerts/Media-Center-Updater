#include "xml.h"

typedef struct Parser { const wchar_t *p; Arena *arena; int depth; } Parser;

static bool is_space(wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r'; }
static bool is_name_char(wchar_t ch) { return ch && !is_space(ch) && !wcschr(L"<>/=\"'&?!", ch); }
static void skip_space(Parser *parser) { while (is_space(*parser->p)) parser->p++; }

static const wchar_t *arena_text(Arena *arena, const wchar_t *text, size_t length)
{
    wchar_t *copy = arena_alloc(arena, (length + 1) * sizeof(wchar_t));
    memcpy(copy, text, length * sizeof(wchar_t));
    copy[length] = 0;
    return copy;
}

/* Skips "<!-- -->" and "<? ?>". Returns false for anything malformed. */
static bool skip_comment_or_instruction(Parser *parser)
{
    const wchar_t *end;
    if (wcsncmp(parser->p, L"<!--", 4) == 0)
    {
        end = wcsstr(parser->p + 4, L"-->");
        if (!end) return false;
        parser->p = end + 3;
        return true;
    }
    if (wcsncmp(parser->p, L"<?", 2) == 0)
    {
        end = wcsstr(parser->p + 2, L"?>");
        if (!end) return false;
        parser->p = end + 2;
        return true;
    }
    return false;
}

/* Decodes one "&...;" reference at the cursor. Only the predefined and numeric forms exist without a DTD. */
static bool decode_reference(Parser *parser, StrBuf *out)
{
    static const struct { const wchar_t *name; wchar_t ch; } named[] =
        { { L"&lt;", L'<' }, { L"&gt;", L'>' }, { L"&amp;", L'&' }, { L"&quot;", L'"' }, { L"&apos;", L'\'' } };
    for (size_t i = 0; i < COUNTOF(named); i++)
    {
        size_t n = wcslen(named[i].name);
        if (wcsncmp(parser->p, named[i].name, n) == 0)
        {
            sb_append_ch(out, named[i].ch);
            parser->p += n;
            return true;
        }
    }
    if (parser->p[1] != L'#') return false;
    const wchar_t *p = parser->p + 2;
    bool hex = *p == L'x';
    if (hex) p++;
    unsigned code = 0;
    int digits = 0;
    for (; *p && *p != L';'; p++, digits++)
    {
        int digit = *p >= L'0' && *p <= L'9' ? *p - L'0' :
            hex && *p >= L'a' && *p <= L'f' ? *p - L'a' + 10 :
            hex && *p >= L'A' && *p <= L'F' ? *p - L'A' + 10 : -1;
        if (digit < 0 || digits > 7) return false;
        code = code * (hex ? 16 : 10) + (unsigned)digit;
    }
    if (*p != L';' || !digits || !code || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    if (code >= 0x10000)
    {
        code -= 0x10000;
        sb_append_ch(out, (wchar_t)(0xD800 + (code >> 10)));
        sb_append_ch(out, (wchar_t)(0xDC00 + (code & 0x3FF)));
    }
    else sb_append_ch(out, (wchar_t)code);
    parser->p = p + 1;
    return true;
}

static XmlNode *parse_element(Parser *parser)
{
    if (*parser->p != L'<' || ++parser->depth > 64) return NULL;
    const wchar_t *name = ++parser->p;
    while (is_name_char(*parser->p)) parser->p++;
    size_t name_length = (size_t)(parser->p - name);
    if (!name_length) return NULL;

    XmlNode *node = arena_alloc(parser->arena, sizeof(XmlNode));
    memset(node, 0, sizeof(*node));
    const wchar_t *colon = wmemchr(name, L':', name_length);
    node->name = colon ? arena_text(parser->arena, colon + 1, name_length - (size_t)(colon + 1 - name))
        : arena_text(parser->arena, name, name_length);

    /* Attributes. Only a default namespace declaration is kept. */
    for (;;)
    {
        bool spaced = is_space(*parser->p);
        skip_space(parser);
        if (*parser->p == L'>' || *parser->p == L'/') break;
        if (!spaced) return NULL;
        const wchar_t *attribute = parser->p;
        while (is_name_char(*parser->p)) parser->p++;
        size_t attribute_length = (size_t)(parser->p - attribute);
        if (!attribute_length) return NULL;
        skip_space(parser);
        if (*parser->p++ != L'=') return NULL;
        skip_space(parser);
        wchar_t quote = *parser->p++;
        if (quote != L'"' && quote != L'\'') return NULL;
        StrBuf value;
        sb_init(&value);
        while (*parser->p && *parser->p != quote)
        {
            if (*parser->p == L'<') { sb_free(&value); return NULL; }
            if (*parser->p == L'&') { if (!decode_reference(parser, &value)) { sb_free(&value); return NULL; } }
            else sb_append_ch(&value, *parser->p++);
        }
        if (*parser->p++ != quote) { sb_free(&value); return NULL; }
        if (attribute_length == 5 && wcsncmp(attribute, L"xmlns", 5) == 0)
            node->xmlns = arena_text(parser->arena, value.p, value.len);
        sb_free(&value);
    }

    if (*parser->p == L'/')
    {
        if (parser->p[1] != L'>') return NULL;
        parser->p += 2;
        node->text = L"";
        parser->depth--;
        return node;
    }
    parser->p++;

    StrBuf text;
    sb_init(&text);
    XmlNode **tail = &node->first;
    bool ok = false;
    for (;;)
    {
        if (!*parser->p) break;
        if (*parser->p == L'&') { if (!decode_reference(parser, &text)) break; continue; }
        if (*parser->p != L'<') { sb_append_ch(&text, *parser->p++); continue; }
        if (wcsncmp(parser->p, L"<![CDATA[", 9) == 0)
        {
            const wchar_t *end = wcsstr(parser->p + 9, L"]]>");
            if (!end) break;
            sb_append_n(&text, parser->p + 9, (size_t)(end - parser->p - 9));
            parser->p = end + 3;
            continue;
        }
        if (parser->p[1] == L'!' || parser->p[1] == L'?')
        {
            if (!skip_comment_or_instruction(parser)) break;
            continue;
        }
        if (parser->p[1] == L'/')
        {
            parser->p += 2;
            if (wcsncmp(parser->p, name, name_length) != 0) break;
            parser->p += name_length;
            skip_space(parser);
            if (*parser->p != L'>') break;
            parser->p++;
            ok = true;
            break;
        }
        XmlNode *child = parse_element(parser);
        if (!child) break;
        *tail = child;
        tail = &child->next;
        sb_append(&text, child->text);
    }
    if (ok) node->text = arena_text(parser->arena, text.p, text.len);
    sb_free(&text);
    parser->depth--;
    return ok ? node : NULL;
}

static wchar_t *decode_document(const char *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;
    if (length >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF)))
    {
        bool big_endian = bytes[0] == 0xFE;
        size_t count = (length - 2) / 2;
        wchar_t *wide = xalloc((count + 1) * sizeof(wchar_t));
        for (size_t i = 0; i < count; i++)
        {
            unsigned char a = bytes[2 + i * 2], b = bytes[3 + i * 2];
            wide[i] = (wchar_t)(big_endian ? (a << 8) | b : (b << 8) | a);
        }
        return wide;
    }
    if (length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) { data += 3; length -= 3; }
    if (!length || length > 0x7FFFFFF0) return NULL;
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, (int)length, NULL, 0);
    if (!needed) return NULL;
    wchar_t *wide = xalloc(((size_t)needed + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, (int)length, wide, needed);
    return wide;
}

XmlDoc *xml_parse_text(const wchar_t *text)
{
    XmlDoc *doc = xalloc(sizeof(XmlDoc));
    Parser parser = { text, &doc->arena, 0 };
    bool ok = true;
    if (*parser.p == 0xFEFF) parser.p++;
    /* Prolog: declaration, comments and processing instructions. "<!DOCTYPE" is not skipped, so it fails. */
    for (;;)
    {
        skip_space(&parser);
        if (parser.p[0] == L'<' && (parser.p[1] == L'?' || wcsncmp(parser.p, L"<!--", 4) == 0))
        {
            if (!skip_comment_or_instruction(&parser)) { ok = false; break; }
        }
        else break;
    }
    if (ok) doc->root = parse_element(&parser);
    while (doc->root)
    {
        skip_space(&parser);
        if (!*parser.p) break;
        if (!skip_comment_or_instruction(&parser)) { doc->root = NULL; break; }
    }
    if (!doc->root)
    {
        xml_free(doc);
        return NULL;
    }
    return doc;
}

XmlDoc *xml_parse(const char *data, size_t length)
{
    wchar_t *text = decode_document(data, length);
    if (!text) return NULL;
    XmlDoc *doc = xml_parse_text(text);
    free(text);
    return doc;
}

void xml_free(XmlDoc *doc)
{
    if (!doc) return;
    arena_free(&doc->arena);
    free(doc);
}

const XmlNode *xml_child(const XmlNode *node, const wchar_t *name)
{
    if (!node) return NULL;
    for (const XmlNode *child = node->first; child; child = child->next)
        if (wcscmp(child->name, name) == 0) return child;
    return NULL;
}

void xml_escape(StrBuf *sb, const wchar_t *text)
{
    for (; *text; text++)
    {
        switch (*text)
        {
        case L'<': sb_append(sb, L"&lt;"); break;
        case L'>': sb_append(sb, L"&gt;"); break;
        case L'&': sb_append(sb, L"&amp;"); break;
        case L'"': sb_append(sb, L"&quot;"); break;
        default: sb_append_ch(sb, *text);
        }
    }
}
