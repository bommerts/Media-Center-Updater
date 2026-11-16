/* A small, strict XML reader for configuration files. Document type declarations are refused. */
#ifndef MCU_XML_H
#define MCU_XML_H

#include "common.h"

typedef struct XmlNode
{
    const wchar_t *name;        /* element name without any prefix */
    const wchar_t *xmlns;       /* the element's own default namespace declaration, or NULL */
    const wchar_t *text;        /* all text inside the element, including text of child elements */
    struct XmlNode *first;      /* first child element */
    struct XmlNode *next;       /* next sibling element */
} XmlNode;

typedef struct XmlDoc { XmlNode *root; Arena arena; } XmlDoc;

XmlDoc *xml_parse(const char *data, size_t length);     /* NULL when malformed or when it declares a DTD */
XmlDoc *xml_parse_text(const wchar_t *text);
void xml_free(XmlDoc *doc);
const XmlNode *xml_child(const XmlNode *node, const wchar_t *name);
void xml_escape(StrBuf *sb, const wchar_t *text);

#endif
