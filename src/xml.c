/* Nordstjernen — namespaced XML / XHTML parser to the DOM, checking well-formedness.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "html.h"
#include "dom.h"

#include <glib.h>
#include <string.h>

#define XML_NS_XML   "http://www.w3.org/XML/1998/namespace"
#define XML_NS_XMLNS "http://www.w3.org/2000/xmlns/"
#define XML_NS_XHTML "http://www.w3.org/1999/xhtml"
#define XML_NS_SVG   "http://www.w3.org/2000/svg"

#define XML_ENTITY_MAX_DEPTH      16
#define XML_ENTITY_FREE_BYTES     ((gsize)8u * 1024u * 1024u)
#define XML_ENTITY_AMPLIFICATION  100

typedef struct {
    char *prefix;
    char *uri;
} xml_binding;

typedef struct {
    char    *value;
    gboolean external;
    gboolean expanding;
} xml_entity;

typedef struct {
    ns_node *el;
    char    *qname;
    guint    ns_base;
} xml_open;

typedef struct {
    const char *start;
    const char *p;
    const char *end;
    GPtrArray  *ns_stack;
    GArray     *open;
    GHashTable *entities;
    GHashTable *attr_names;
    GHashTable *attr_expanded;
    gsize       entity_bytes;
    gboolean    ok;
    gboolean    named_entities;
} xml_parser;

static const char *const xml_named_entity_dtds[] = {
    "-//W3C//DTD XHTML 1.0 Transitional//EN",
    "-//W3C//DTD XHTML 1.1//EN",
    "-//W3C//DTD XHTML 1.0 Strict//EN",
    "-//W3C//DTD XHTML 1.0 Frameset//EN",
    "-//W3C//DTD XHTML Basic 1.0//EN",
    "-//W3C//DTD XHTML 1.1 plus MathML 2.0//EN",
    "-//W3C//DTD XHTML 1.1 plus MathML 2.0 plus SVG 1.1//EN",
    "-//W3C//DTD MathML 2.0//EN",
    "-//WAPFORUM//DTD XHTML Mobile 1.0//EN",
};

static gboolean
xml_fail(xml_parser *xp)
{
    xp->ok = FALSE;
    return FALSE;
}

static gboolean
xml_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static gboolean
xml_skip_space(xml_parser *xp)
{
    const char *s = xp->p;
    while (xp->p < xp->end && xml_is_space(*xp->p)) xp->p++;
    return xp->p > s;
}

static gboolean
xml_at(const xml_parser *xp, const char *lit)
{
    gsize n = strlen(lit);
    return (gsize)(xp->end - xp->p) >= n && memcmp(xp->p, lit, n) == 0;
}

static const char *
xml_find(const char *p, const char *end, const char *lit)
{
    gsize n = strlen(lit);
    for (; (gsize)(end - p) >= n; p++)
        if (memcmp(p, lit, n) == 0) return p;
    return NULL;
}

static gboolean
xml_is_char(guint32 cp)
{
    return cp == 0x9 || cp == 0xA || cp == 0xD ||
           (cp >= 0x20 && cp <= 0xD7FF) ||
           (cp >= 0xE000 && cp <= 0xFFFD) ||
           (cp >= 0x10000 && cp <= 0x10FFFF);
}

static const char *
xml_first_illegal_char(const char *s, const char *end)
{
    const char *bad = NULL;
    if (!g_utf8_validate(s, (gssize)(end - s), &bad)) return bad;
    for (const unsigned char *p = (const unsigned char *)s;
         p < (const unsigned char *)end; p++) {
        if (*p < 0x20 && *p != '\t' && *p != '\n' && *p != '\r')
            return (const char *)p;
        if (*p == 0xEF && p[1] == 0xBF && (p[2] == 0xBE || p[2] == 0xBF))
            return (const char *)p;
    }
    return NULL;
}

static gboolean
xml_name_start(gunichar c)
{
    if (c < 0x80)
        return c == ':' || c == '_' || g_ascii_isalpha((char)c);
    return (c >= 0xC0 && c <= 0x2FF && c != 0xD7 && c != 0xF7) ||
           (c >= 0x370 && c <= 0x1FFF && c != 0x37E) ||
           c == 0x200C || c == 0x200D ||
           (c >= 0x2070 && c <= 0x218F) || (c >= 0x2C00 && c <= 0x2FEF) ||
           (c >= 0x3001 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF) ||
           (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}

static gboolean
xml_name_part(gunichar c)
{
    if (c < 0x80)
        return c == '-' || c == '.' || g_ascii_isdigit((char)c) ||
               xml_name_start(c);
    return c == 0xB7 || (c >= 0x300 && c <= 0x36F) ||
           c == 0x203F || c == 0x2040 || xml_name_start(c);
}

static const char *
xml_name_end(const char *p, const char *end)
{
    if (p >= end || !xml_name_start(g_utf8_get_char(p))) return NULL;
    p = g_utf8_next_char(p);
    while (p < end && xml_name_part(g_utf8_get_char(p)))
        p = g_utf8_next_char(p);
    return p;
}

static char *
xml_read_name(xml_parser *xp)
{
    const char *e = xml_name_end(xp->p, xp->end);
    if (!e) return NULL;
    char *name = g_strndup(xp->p, (gsize)(e - xp->p));
    xp->p = e;
    return name;
}

static gboolean
xml_qname_valid(const char *qname)
{
    const char *colon = strchr(qname, ':');
    if (!colon) return TRUE;
    return colon != qname && colon[1] != '\0' && !strchr(colon + 1, ':') &&
           xml_name_end(colon + 1, colon + 1 + strlen(colon + 1)) != NULL;
}

static const char *
xml_char_ref(const char *p, const char *end, guint32 *out)
{
    gboolean hex = p < end && *p == 'x';
    if (hex) p++;
    const char *digits = p;
    guint32 cp = 0;
    for (; p < end; p++) {
        int d = hex ? g_ascii_xdigit_value(*p) : g_ascii_digit_value(*p);
        if (d < 0) break;
        if (cp <= 0x10FFFF) cp = cp * (hex ? 16u : 10u) + (guint32)d;
    }
    if (p == digits || p >= end || *p != ';' || !xml_is_char(cp)) return NULL;
    *out = cp;
    return p + 1;
}

static void
xml_append_codepoint(GString *out, guint32 cp)
{
    char buf[6];
    gint nb = g_unichar_to_utf8((gunichar)cp, buf);
    g_string_append_len(out, buf, nb);
}

static gboolean
xml_append_predefined(GString *out, const char *name, gsize len)
{
    static const struct { const char *name; char c; } predefined[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' },
        { "quot", '"' }, { "apos", '\'' },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(predefined); i++) {
        if (strlen(predefined[i].name) == len &&
            strncmp(predefined[i].name, name, len) == 0) {
            g_string_append_c(out, predefined[i].c);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
xml_entity_budget_ok(const xml_parser *xp)
{
    gsize input = (gsize)(xp->end - xp->start);
    return xp->entity_bytes <= XML_ENTITY_FREE_BYTES ||
           xp->entity_bytes / XML_ENTITY_AMPLIFICATION <= input;
}

static gboolean xml_decode(xml_parser *xp, GString *out, const char *s,
                           gsize len, gboolean attr, int depth);

static gboolean
xml_expand_entity(xml_parser *xp, GString *out, const char *name, gsize len,
                  gboolean attr, int depth)
{
    char *key = g_strndup(name, len);
    xml_entity *ent = xp->entities ? g_hash_table_lookup(xp->entities, key)
                                   : NULL;
    g_free(key);
    if (!ent) {
        if (!xp->named_entities) return FALSE;
        char *value = ns_html_named_char_ref(name, len + 1);
        if (!value) return FALSE;
        g_string_append(out, value);
        g_free(value);
        return TRUE;
    }
    if (ent->external) return !attr;
    if (ent->expanding || depth >= XML_ENTITY_MAX_DEPTH) return FALSE;
    gsize before = out->len;
    ent->expanding = TRUE;
    gboolean ok = xml_decode(xp, out, ent->value, strlen(ent->value), attr,
                             depth + 1);
    ent->expanding = FALSE;
    xp->entity_bytes += out->len - before;
    return ok && xml_entity_budget_ok(xp);
}

static gboolean
xml_decode(xml_parser *xp, GString *out, const char *s, gsize len,
           gboolean attr, int depth)
{
    const char *p = s, *end = s + len;
    while (p < end) {
        if (*p == '<') return FALSE;
        if (*p != '&') {
            g_string_append_c(out, attr && xml_is_space(*p) ? ' ' : *p);
            p++;
            continue;
        }
        p++;
        if (p < end && *p == '#') {
            guint32 cp = 0;
            p = xml_char_ref(p + 1, end, &cp);
            if (!p) return FALSE;
            xml_append_codepoint(out, cp);
            continue;
        }
        const char *ne = xml_name_end(p, end);
        if (!ne || ne >= end || *ne != ';') return FALSE;
        gsize nlen = (gsize)(ne - p);
        if (!xml_append_predefined(out, p, nlen) &&
            !xml_expand_entity(xp, out, p, nlen, attr, depth))
            return FALSE;
        p = ne + 1;
    }
    return TRUE;
}

static const char *
xml_lookup_prefix(xml_parser *xp, const char *prefix)
{
    if (prefix && strcmp(prefix, "xml") == 0)   return XML_NS_XML;
    if (prefix && strcmp(prefix, "xmlns") == 0) return XML_NS_XMLNS;
    for (guint i = xp->ns_stack->len; i > 0; i--) {
        xml_binding *b = g_ptr_array_index(xp->ns_stack, i - 1);
        if ((prefix == NULL && b->prefix == NULL) ||
            (prefix && b->prefix && strcmp(prefix, b->prefix) == 0))
            return b->uri;
    }
    return NULL;
}

static gboolean
xml_push_binding(xml_parser *xp, const char *prefix, const char *uri)
{
    gboolean is_xml_uri = strcmp(uri, XML_NS_XML) == 0;
    if (strcmp(uri, XML_NS_XMLNS) == 0) return FALSE;
    if (prefix) {
        if (strcmp(prefix, "xmlns") == 0 || *uri == '\0') return FALSE;
        if ((strcmp(prefix, "xml") == 0) != is_xml_uri) return FALSE;
    } else if (is_xml_uri) {
        return FALSE;
    }
    xml_binding *b = g_new0(xml_binding, 1);
    b->prefix = g_strdup(prefix);
    b->uri = g_strdup(uri);
    g_ptr_array_add(xp->ns_stack, b);
    return TRUE;
}

static void
xml_binding_free(gpointer p)
{
    xml_binding *b = p;
    g_free(b->prefix);
    g_free(b->uri);
    g_free(b);
}

static void
xml_entity_free(gpointer p)
{
    xml_entity *ent = p;
    g_free(ent->value);
    g_free(ent);
}

static gboolean
xml_parse_comment(xml_parser *xp, ns_node *parent)
{
    const char *body = xp->p + 4;
    const char *e = xml_find(body, xp->end, "--");
    if (!e || xp->end - e < 3 || e[2] != '>') return xml_fail(xp);
    ns_node_append_child(parent,
        ns_node_new_comment(g_strndup(body, (gsize)(e - body))));
    xp->p = e + 3;
    return TRUE;
}

static gboolean
xml_parse_pi(xml_parser *xp, ns_node *parent)
{
    xp->p += 2;
    char *target = xml_read_name(xp);
    if (!target || g_ascii_strcasecmp(target, "xml") == 0) {
        g_free(target);
        return xml_fail(xp);
    }
    const char *data = xp->p;
    if (!xml_at(xp, "?>") && !xml_skip_space(xp)) {
        g_free(target);
        return xml_fail(xp);
    }
    data = xp->p;
    const char *e = xml_find(data, xp->end, "?>");
    if (!e) {
        g_free(target);
        return xml_fail(xp);
    }
    ns_node *pi = ns_node_new_comment(g_strndup(data, (gsize)(e - data)));
    ns_node_set_name_owned(pi, target);
    pi->flags |= NS_NODE_PI;
    ns_node_append_child(parent, pi);
    xp->p = e + 2;
    return TRUE;
}

static gboolean
xml_skip_xml_declaration(xml_parser *xp)
{
    if (!xml_at(xp, "<?xml") || xp->end - xp->p < 6 || !xml_is_space(xp->p[5]))
        return TRUE;
    const char *e = xml_find(xp->p, xp->end, "?>");
    if (!e) return xml_fail(xp);
    xp->p = e + 2;
    return TRUE;
}

static char *
xml_read_literal(xml_parser *xp)
{
    if (xp->p >= xp->end || (*xp->p != '"' && *xp->p != '\'')) return NULL;
    char q = *xp->p++;
    const char *v = xp->p;
    const char *e = memchr(v, q, (gsize)(xp->end - v));
    if (!e) return NULL;
    xp->p = e + 1;
    return g_strndup(v, (gsize)(e - v));
}

static gboolean
xml_skip_declaration(xml_parser *xp)
{
    char quote = 0;
    for (; xp->p < xp->end; xp->p++) {
        char c = *xp->p;
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            xp->p++;
            return TRUE;
        }
    }
    return xml_fail(xp);
}

static char *
xml_entity_replacement_text(const char *literal)
{
    GString *out = g_string_new(NULL);
    const char *p = literal, *end = literal + strlen(literal);
    while (p < end) {
        if (*p == '%') {
            g_string_free(out, TRUE);
            return NULL;
        }
        if (p[0] == '&' && p[1] == '#') {
            guint32 cp = 0;
            p = xml_char_ref(p + 2, end, &cp);
            if (!p) {
                g_string_free(out, TRUE);
                return NULL;
            }
            xml_append_codepoint(out, cp);
            continue;
        }
        g_string_append_c(out, *p++);
    }
    return g_string_free(out, FALSE);
}

static gboolean
xml_parse_entity_declaration(xml_parser *xp)
{
    xp->p += 8;
    if (!xml_skip_space(xp)) return xml_fail(xp);
    if (xp->p < xp->end && *xp->p == '%') return xml_skip_declaration(xp);
    char *name = xml_read_name(xp);
    if (!name || !xml_skip_space(xp)) {
        g_free(name);
        return xml_fail(xp);
    }
    xml_entity *ent = g_new0(xml_entity, 1);
    char *literal = xml_read_literal(xp);
    if (literal) {
        ent->value = xml_entity_replacement_text(literal);
        g_free(literal);
        xml_skip_space(xp);
        if (!ent->value || xp->p >= xp->end || *xp->p != '>') {
            g_free(name);
            xml_entity_free(ent);
            return xml_fail(xp);
        }
        xp->p++;
    } else {
        ent->external = TRUE;
        if (!xml_skip_declaration(xp)) {
            g_free(name);
            xml_entity_free(ent);
            return FALSE;
        }
    }
    if (!xp->entities)
        xp->entities = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, xml_entity_free);
    if (g_hash_table_contains(xp->entities, name)) {
        g_free(name);
        xml_entity_free(ent);
    } else {
        g_hash_table_insert(xp->entities, name, ent);
    }
    return TRUE;
}

static gboolean
xml_parse_internal_subset(xml_parser *xp)
{
    ns_node *discard = ns_node_new_document();
    gboolean closed = FALSE;
    while (xp->ok && !closed) {
        xml_skip_space(xp);
        if (xp->p >= xp->end) {
            xml_fail(xp);
        } else if (*xp->p == ']') {
            xp->p++;
            closed = TRUE;
        } else if (*xp->p == '%') {
            const char *e = xml_name_end(xp->p + 1, xp->end);
            if (!e || e >= xp->end || *e != ';') xml_fail(xp);
            else xp->p = e + 1;
        } else if (xml_at(xp, "<!--")) {
            xml_parse_comment(xp, discard);
        } else if (xml_at(xp, "<?")) {
            xml_parse_pi(xp, discard);
        } else if (xml_at(xp, "<!ENTITY")) {
            xml_parse_entity_declaration(xp);
        } else if (xml_at(xp, "<!ELEMENT") || xml_at(xp, "<!ATTLIST") ||
                   xml_at(xp, "<!NOTATION")) {
            xml_skip_declaration(xp);
        } else {
            xml_fail(xp);
        }
    }
    ns_node_free(discard);
    return xp->ok;
}

static gboolean
xml_parse_external_id(xml_parser *xp, char **pub, char **sys)
{
    if (xml_at(xp, "PUBLIC")) {
        xp->p += 6;
        if (!xml_skip_space(xp)) return xml_fail(xp);
        *pub = xml_read_literal(xp);
        if (!*pub || !xml_skip_space(xp)) return xml_fail(xp);
        *sys = xml_read_literal(xp);
        return *sys ? TRUE : xml_fail(xp);
    }
    if (xml_at(xp, "SYSTEM")) {
        xp->p += 6;
        if (!xml_skip_space(xp)) return xml_fail(xp);
        *sys = xml_read_literal(xp);
        return *sys ? TRUE : xml_fail(xp);
    }
    return TRUE;
}

static void
xml_enable_dtd_entities(xml_parser *xp, const char *pub)
{
    for (gsize i = 0; pub && i < G_N_ELEMENTS(xml_named_entity_dtds); i++)
        if (strcmp(pub, xml_named_entity_dtds[i]) == 0)
            xp->named_entities = TRUE;
}

static gboolean
xml_parse_doctype(xml_parser *xp, ns_node *doc)
{
    xp->p += 9;
    if (!xml_skip_space(xp)) return xml_fail(xp);
    char *name = xml_read_name(xp);
    if (!name) return xml_fail(xp);
    char *pub = NULL, *sys = NULL;
    gboolean spaced = xml_skip_space(xp);
    if (spaced && xml_parse_external_id(xp, &pub, &sys))
        xml_skip_space(xp);
    if (xp->ok && xp->p < xp->end && *xp->p == '[') {
        xp->p++;
        if (xml_parse_internal_subset(xp)) xml_skip_space(xp);
    }
    if (xp->ok && (xp->p >= xp->end || *xp->p != '>')) xml_fail(xp);
    if (xp->ok) {
        xp->p++;
        xml_enable_dtd_entities(xp, pub);
        ns_node *dt = ns_node_new_element(NULL);
        ns_node_set_name_owned(dt, name);
        name = NULL;
        ns_element_set_attr(dt, "publicId", pub ? pub : "");
        ns_element_set_attr(dt, "systemId", sys ? sys : "");
        dt->kind = NS_NODE_DOCTYPE;
        ns_node_append_child(doc, dt);
    }
    g_free(name);
    g_free(pub);
    g_free(sys);
    return xp->ok;
}

static gboolean
xml_parse_misc(xml_parser *xp, ns_node *doc, gboolean allow_doctype)
{
    while (xp->ok) {
        xml_skip_space(xp);
        if (xml_at(xp, "<!--")) {
            xml_parse_comment(xp, doc);
        } else if (xml_at(xp, "<?")) {
            xml_parse_pi(xp, doc);
        } else if (allow_doctype && xml_at(xp, "<!DOCTYPE")) {
            allow_doctype = FALSE;
            xml_parse_doctype(xp, doc);
        } else {
            break;
        }
    }
    return xp->ok;
}

static gboolean
xml_parse_attributes(xml_parser *xp, GPtrArray *names, GPtrArray *values)
{
    g_hash_table_remove_all(xp->attr_names);
    for (;;) {
        gboolean spaced = xml_skip_space(xp);
        if (xp->p >= xp->end) return xml_fail(xp);
        if (*xp->p == '>' || *xp->p == '/') return TRUE;
        if (!spaced) return xml_fail(xp);
        char *aname = xml_read_name(xp);
        if (!aname) return xml_fail(xp);
        g_ptr_array_add(names, aname);
        if (!xml_qname_valid(aname) ||
            !g_hash_table_add(xp->attr_names, aname))
            return xml_fail(xp);
        xml_skip_space(xp);
        if (xp->p >= xp->end || *xp->p != '=') return xml_fail(xp);
        xp->p++;
        xml_skip_space(xp);
        if (xp->p >= xp->end || (*xp->p != '"' && *xp->p != '\''))
            return xml_fail(xp);
        char q = *xp->p++;
        const char *vs = xp->p;
        const char *ve = memchr(vs, q, (gsize)(xp->end - vs));
        if (!ve) return xml_fail(xp);
        GString *value = g_string_sized_new((gsize)(ve - vs));
        gboolean decoded = xml_decode(xp, value, vs, (gsize)(ve - vs), TRUE, 0);
        char *aval = g_string_free(value, FALSE);
        g_ptr_array_add(values, aval);
        if (!decoded) return xml_fail(xp);
        xp->p = ve + 1;

        gboolean bound = TRUE;
        if (strcmp(aname, "xmlns") == 0)
            bound = xml_push_binding(xp, NULL, aval);
        else if (strncmp(aname, "xmlns:", 6) == 0)
            bound = xml_push_binding(xp, aname + 6, aval);
        if (!bound) return xml_fail(xp);
    }
}

static void
xml_apply_namespace(ns_node *el, const char *qname, const char *uri)
{
    const char *colon = strchr(qname, ':');
    gboolean has_prefix = colon != NULL;
    el->flags |= NS_NODE_KEEP_CASE;
    if (uri && strcmp(uri, XML_NS_XHTML) == 0) {
        if (has_prefix) {
            char *prefix = g_strndup(qname, (gsize)(colon - qname));
            ns_element_set_attr(el, "data-nd-ns-prefix", prefix);
            g_free(prefix);
        }
        return;
    }
    if (uri && strcmp(uri, XML_NS_SVG) == 0)
        el->flags |= NS_NODE_SVG_NS;
    else
        el->flags |= NS_NODE_FOREIGN_NS;
    if (uri && *uri)
        ns_element_set_attr(el, "data-nd-ns-uri", uri);
}

static gboolean
xml_set_attributes(xml_parser *xp, ns_node *el, GPtrArray *names,
                   GPtrArray *values)
{
    g_hash_table_remove_all(xp->attr_expanded);
    for (guint i = 0; i < names->len; i++) {
        const char *an = g_ptr_array_index(names, i);
        const char *av = g_ptr_array_index(values, i);
        const char *acolon = strchr(an, ':');
        const char *auri = NULL;
        char *apfx = acolon ? g_strndup(an, (gsize)(acolon - an)) : NULL;
        if (apfx) {
            auri = xml_lookup_prefix(xp, apfx);
            if (!auri ||
                !g_hash_table_add(xp->attr_expanded,
                                  g_strconcat(auri, " ", acolon + 1, NULL))) {
                g_free(apfx);
                return xml_fail(xp);
            }
        }
        if (ns_attr_name_is_internal(an) ||
            (acolon && ns_attr_name_is_internal(acolon + 1))) {
            g_free(apfx);
            continue;
        }
        if (strcmp(an, "xmlns") == 0)
            ns_element_set_attr_ns(el, XML_NS_XMLNS, NULL, "xmlns", "xmlns", av);
        else if (acolon)
            ns_element_set_attr_ns(el, auri, apfx, acolon + 1, an, av);
        else
            ns_element_set_attr_ns(el, NULL, NULL, an, an, av);
        g_free(apfx);
    }
    return TRUE;
}

static void
xml_close_element(xml_parser *xp)
{
    xml_open *top = &g_array_index(xp->open, xml_open, xp->open->len - 1);
    g_ptr_array_set_size(xp->ns_stack, top->ns_base);
    g_free(top->qname);
    g_array_set_size(xp->open, xp->open->len - 1);
}

static gboolean
xml_parse_start_tag(xml_parser *xp, ns_node *parent)
{
    if (xp->open->len >= NS_DOM_MAX_DEPTH) return xml_fail(xp);
    xp->p++;
    char *qname = xml_read_name(xp);
    if (!qname) return xml_fail(xp);

    guint ns_base = xp->ns_stack->len;
    GPtrArray *anames = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *avals  = g_ptr_array_new_with_free_func(g_free);
    const char *colon = strchr(qname, ':');
    char *prefix = colon ? g_strndup(qname, (gsize)(colon - qname)) : NULL;
    const char *el_uri = NULL;
    ns_node *el = NULL;

    if (!xml_qname_valid(qname) || (prefix && strcmp(prefix, "xmlns") == 0))
        xml_fail(xp);
    if (xp->ok) xml_parse_attributes(xp, anames, avals);
    if (xp->ok) {
        el_uri = xml_lookup_prefix(xp, prefix);
        if (prefix && !el_uri) xml_fail(xp);
    }
    if (xp->ok) {
        gboolean is_xhtml = el_uri && strcmp(el_uri, XML_NS_XHTML) == 0;
        el = ns_node_new_element((is_xhtml && colon) ? g_strdup(colon + 1)
                                                     : g_strdup(qname));
        el->flags |= NS_NODE_NOT_PARSER_INSERTED;
        xml_apply_namespace(el, qname, el_uri);
        ns_node_append_child(parent, el);
        xml_set_attributes(xp, el, anames, avals);
    }
    g_ptr_array_free(anames, TRUE);
    g_ptr_array_free(avals, TRUE);
    g_free(prefix);

    gboolean self_closing = xp->ok && xml_at(xp, "/>");
    if (xp->ok && !self_closing && !xml_at(xp, ">")) xml_fail(xp);
    if (!xp->ok || self_closing) {
        if (self_closing) xp->p += 2;
        g_ptr_array_set_size(xp->ns_stack, ns_base);
        g_free(qname);
        return xp->ok;
    }
    xp->p++;
    xml_open entry = { .el = el, .qname = qname, .ns_base = ns_base };
    g_array_append_val(xp->open, entry);
    return TRUE;
}

static gboolean
xml_parse_end_tag(xml_parser *xp)
{
    const xml_open *top = &g_array_index(xp->open, xml_open, xp->open->len - 1);
    gsize n = strlen(top->qname);
    xp->p += 2;
    if ((gsize)(xp->end - xp->p) < n || memcmp(xp->p, top->qname, n) != 0)
        return xml_fail(xp);
    xp->p += n;
    xml_skip_space(xp);
    if (xp->p >= xp->end || *xp->p != '>') return xml_fail(xp);
    xp->p++;
    xml_close_element(xp);
    return TRUE;
}

static gboolean
xml_parse_cdata(xml_parser *xp, ns_node *parent)
{
    const char *body = xp->p + 9;
    const char *e = xml_find(body, xp->end, "]]>");
    if (!e) return xml_fail(xp);
    ns_node *t = ns_node_new_text(g_strndup(body, (gsize)(e - body)));
    t->flags |= NS_NODE_CDATA;
    ns_node_append_child(parent, t);
    xp->p = e + 3;
    return TRUE;
}

static gboolean
xml_parse_text(xml_parser *xp, ns_node *parent)
{
    const char *ts = xp->p;
    const char *te = memchr(ts, '<', (gsize)(xp->end - ts));
    if (!te) te = xp->end;
    GString *text = g_string_sized_new((gsize)(te - ts));
    if (xml_find(ts, te, "]]>") ||
        !xml_decode(xp, text, ts, (gsize)(te - ts), FALSE, 0)) {
        g_string_free(text, TRUE);
        return xml_fail(xp);
    }
    ns_node_append_child(parent, ns_node_new_text(g_string_free(text, FALSE)));
    xp->p = te;
    return TRUE;
}

static gboolean
xml_parse_root(xml_parser *xp, ns_node *doc)
{
    if (xp->p >= xp->end || *xp->p != '<' || !xml_parse_start_tag(xp, doc))
        return xml_fail(xp);
    while (xp->ok && xp->open->len > 0) {
        ns_node *el = g_array_index(xp->open, xml_open, xp->open->len - 1).el;
        if (xp->p >= xp->end)             xml_fail(xp);
        else if (*xp->p != '<')           xml_parse_text(xp, el);
        else if (xml_at(xp, "</"))        xml_parse_end_tag(xp);
        else if (xml_at(xp, "<!--"))      xml_parse_comment(xp, el);
        else if (xml_at(xp, "<![CDATA[")) xml_parse_cdata(xp, el);
        else if (xml_at(xp, "<?"))        xml_parse_pi(xp, el);
        else                              xml_parse_start_tag(xp, el);
    }
    while (xp->open->len > 0) xml_close_element(xp);
    return xp->ok;
}

static void
xml_offset_to_line_column(const char *input, gsize offset, int *line, int *column)
{
    int ln = 1, col = 1;
    for (gsize i = 0; i < offset; i++) {
        if (input[i] == '\n') { ln++; col = 1; }
        else col++;
    }
    if (line) *line = ln;
    if (column) *column = col;
}

static GString *
xml_normalize_newlines(const char *input, gsize n)
{
    GString *out = g_string_sized_new(n);
    for (gsize i = 0; i < n; i++) {
        if (input[i] != '\r') { g_string_append_c(out, input[i]); continue; }
        g_string_append_c(out, '\n');
        if (i + 1 < n && input[i + 1] == '\n') i++;
    }
    return out;
}

static gboolean
xml_is_xhtml_template(const ns_node *n)
{
    return n->kind == NS_NODE_ELEMENT && n->name &&
           strcmp(n->name, "template") == 0 &&
           !(n->flags & (NS_NODE_SVG_NS | NS_NODE_FOREIGN_NS));
}

static ns_node *
xml_take_template_children(ns_node *tpl)
{
    ns_node *content = ns_template_content_get(tpl);
    if (!content) return tpl;
    while (tpl->first_child) {
        ns_node *c = tpl->first_child;
        ns_node_remove(c);
        ns_node_append_child(content, c);
    }
    return content;
}

static void
xml_move_template_contents(ns_node *n, int depth)
{
    if (!n || depth > NS_DOM_MAX_DEPTH) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        ns_node *inner = xml_is_xhtml_template(c)
            ? xml_take_template_children(c) : c;
        xml_move_template_contents(inner, depth + 1);
    }
}

static void
xml_parse_document(xml_parser *xp, ns_node *doc)
{
    const char *illegal = xml_first_illegal_char(xp->p, xp->end);
    if (illegal) {
        xp->p = illegal;
        xml_fail(xp);
        return;
    }
    if (xml_at(xp, "\xEF\xBB\xBF")) xp->p += 3;
    if (xml_skip_xml_declaration(xp) && xml_parse_misc(xp, doc, TRUE) &&
        xml_parse_root(xp, doc) && xml_parse_misc(xp, doc, FALSE) &&
        xp->p < xp->end)
        xml_fail(xp);
}

static ns_node *
xml_parse_buffer(const char *input, gsize n, gboolean html_entities,
                 int *line, int *column)
{
    xml_parser xp = {
        .start = input,
        .p = input,
        .end = input + n,
        .ns_stack = g_ptr_array_new_with_free_func(xml_binding_free),
        .open = g_array_new(FALSE, FALSE, sizeof(xml_open)),
        .attr_names = g_hash_table_new(g_str_hash, g_str_equal),
        .attr_expanded = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL),
        .ok = TRUE,
        .named_entities = html_entities,
    };
    ns_node *doc = ns_node_new_document();
    xml_parse_document(&xp, doc);

    g_ptr_array_free(xp.ns_stack, TRUE);
    g_array_free(xp.open, TRUE);
    g_hash_table_destroy(xp.attr_names);
    g_hash_table_destroy(xp.attr_expanded);
    if (xp.entities) g_hash_table_destroy(xp.entities);

    if (!xp.ok) {
        xml_offset_to_line_column(input, (gsize)(xp.p - input), line, column);
        ns_node_free(doc);
        return NULL;
    }
    xml_move_template_contents(doc, 0);
    return doc;
}

static ns_node *
xml_parse_with(const char *input, gssize len, gboolean html_entities,
               int *line, int *column)
{
    if (line) *line = 1;
    if (column) *column = 1;
    if (!input) return NULL;
    gsize n = (len < 0) ? strlen(input) : (gsize)len;
    if (!memchr(input, '\r', n))
        return xml_parse_buffer(input, n, html_entities, line, column);
    GString *normalized = xml_normalize_newlines(input, n);
    ns_node *doc = xml_parse_buffer(normalized->str, normalized->len,
                                    html_entities, line, column);
    g_string_free(normalized, TRUE);
    return doc;
}

ns_node *
ns_xml_parse_reporting(const char *input, gssize len, int *line, int *column)
{
    return xml_parse_with(input, len, FALSE, line, column);
}

ns_node *
ns_xml_parse(const char *input, gssize len)
{
    return xml_parse_with(input, len, FALSE, NULL, NULL);
}

ns_node *
ns_xml_parse_html_entities(const char *input, gssize len)
{
    return xml_parse_with(input, len, TRUE, NULL, NULL);
}

static char *
xml_element_namespace(const ns_node *el)
{
    if (el->flags & NS_NODE_SVG_NS) return g_strdup(XML_NS_SVG);
    if (el->flags & NS_NODE_FOREIGN_NS)
        return g_strdup(ns_element_get_attr(el, "data-nd-ns-uri"));
    return g_strdup(XML_NS_XHTML);
}

gboolean
ns_xml_well_formed(const char *input, gssize len, char **out_root_ns)
{
    if (out_root_ns) *out_root_ns = NULL;
    ns_node *doc = ns_xml_parse(input, len);
    if (!doc) return FALSE;
    for (const ns_node *c = doc->first_child; out_root_ns && c;
         c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT) continue;
        *out_root_ns = xml_element_namespace(c);
        break;
    }
    ns_node_free(doc);
    return TRUE;
}
