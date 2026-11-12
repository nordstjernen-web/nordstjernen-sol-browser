/* Nordstjernen — HTML parser: the tree sink the Rust HTML5 parser builds ns_nodes through, and the parse entry points.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "html.h"
#include "json_string.h"
#include "parsers_rs.h"

#include <string.h>

enum {
    NS_H5_NS_HTML,
    NS_H5_NS_SVG,
    NS_H5_NS_MATHML,
};

enum {
    NS_H5_ATTR_NS_NONE,
    NS_H5_ATTR_NS_XLINK,
    NS_H5_ATTR_NS_XML,
    NS_H5_ATTR_NS_XMLNS,
};

#define NS_H5_ARENA_CHUNK (64u * 1024u)

struct ns_h5_sink {
    GPtrArray *chunks;
    char      *next;
    size_t     left;
};

static ns_h5_sink *
ns_h5_sink_new(void)
{
    ns_h5_sink *sink = g_new0(ns_h5_sink, 1);
    sink->chunks = g_ptr_array_new_with_free_func(g_free);
    return sink;
}

static void
ns_h5_sink_free(void *p)
{
    ns_h5_sink *sink = p;
    if (!sink) return;
    g_ptr_array_free(sink->chunks, TRUE);
    g_free(sink);
}

static char *
ns_h5_arena_copy(ns_h5_sink *sink, const char *text, size_t len)
{
    size_t n = text ? strnlen(text, len) : 0;
    if (n + 1 > sink->left) {
        size_t size = MAX((size_t)NS_H5_ARENA_CHUNK, n + 1);
        char *chunk = g_malloc(size);
        g_ptr_array_add(sink->chunks, chunk);
        if (size > NS_H5_ARENA_CHUNK) {
            memcpy(chunk, text, n);
            chunk[n] = '\0';
            return chunk;
        }
        sink->next = chunk;
        sink->left = size;
    }
    char *out = sink->next;
    if (n) memcpy(out, text, n);
    out[n] = '\0';
    sink->next += n + 1;
    sink->left -= n + 1;
    return out;
}

ns_node *
ns_h5_element(ns_h5_sink *sink, const char *name, size_t len, int borrowed,
              int line, int column)
{
    ns_node *el = ns_node_new_element(NULL);
    ns_node_set_name_borrow(el, borrowed ? name
                                         : ns_h5_arena_copy(sink, name, len));
    if (line > 0 && g_ascii_strcasecmp(el->name, "script") == 0) {
        el->src_line = line;
        el->src_col = column;
    }
    return el;
}

static const char *
ns_h5_attr_namespace_uri(int namespace_id)
{
    switch (namespace_id) {
    case NS_H5_ATTR_NS_XLINK: return "http://www.w3.org/1999/xlink";
    case NS_H5_ATTR_NS_XML:   return "http://www.w3.org/XML/1998/namespace";
    case NS_H5_ATTR_NS_XMLNS: return "http://www.w3.org/2000/xmlns/";
    default:                  return NULL;
    }
}

void
ns_h5_attribute(ns_h5_sink *sink, ns_node *element,
                const char *name, size_t name_len,
                const char *value, size_t value_len, int namespace_id,
                size_t prefix_len, int only_if_missing)
{
    if (!element || element->kind != NS_NODE_ELEMENT || name_len == 0) return;
    const char *qname = ns_h5_arena_copy(sink, name, name_len);
    const char *val = ns_h5_arena_copy(sink, value, value_len);
    if (ns_attr_name_is_internal(qname)) return;
    if (only_if_missing) {
        if (!ns_element_get_attr(element, qname))
            ns_element_set_attr(element, qname, val);
        return;
    }
    const char *uri = ns_h5_attr_namespace_uri(namespace_id);
    if (!uri) {
        ns_element_append_attr_borrow(element, qname, val);
        return;
    }
    g_autofree char *prefix = prefix_len ? g_strndup(qname, prefix_len) : NULL;
    const char *local = prefix_len && prefix_len < name_len
        ? qname + prefix_len + 1 : qname;
    ns_element_set_attr_ns(element, uri, prefix, local, qname, val);
}

void
ns_h5_set_namespace(ns_node *element, int namespace_id)
{
    if (namespace_id == NS_H5_NS_SVG) {
        element->flags |= NS_NODE_SVG_NS;
        ns_element_set_attr(element, "data-nd-ns-uri",
                            "http://www.w3.org/2000/svg");
    } else if (namespace_id == NS_H5_NS_MATHML) {
        element->flags |= NS_NODE_FOREIGN_NS;
        ns_element_set_attr(element, "data-nd-ns-uri",
                            "http://www.w3.org/1998/Math/MathML");
    }
}

ns_node *
ns_h5_comment(ns_h5_sink *sink, const char *text, size_t len)
{
    ns_node *out = ns_node_new_comment(NULL);
    ns_node_set_text_borrow(out, ns_h5_arena_copy(sink, text, len));
    return out;
}

ns_node *
ns_h5_processing_instruction(ns_h5_sink *sink,
                             const char *target, size_t target_len,
                             const char *data, size_t data_len)
{
    ns_node *pi = ns_h5_comment(sink, data, data_len);
    pi->name = g_strndup(target ? target : "", target_len);
    pi->flags |= NS_NODE_OWN_NAME | NS_NODE_PI;
    return pi;
}

void
ns_h5_doctype(ns_h5_sink *sink, ns_node *document,
              const char *name, size_t name_len,
              const char *public_id, size_t public_len,
              const char *system_id, size_t system_len)
{
    ns_node *out = ns_node_new_element(NULL);
    ns_node_set_name_borrow(out, name_len ? ns_h5_arena_copy(sink, name, name_len)
                                          : "html");
    ns_element_set_attr(out, "publicId",
                        ns_h5_arena_copy(sink, public_id, public_len));
    ns_element_set_attr(out, "systemId",
                        ns_h5_arena_copy(sink, system_id, system_len));
    out->kind = NS_NODE_DOCTYPE;
    ns_node_append_child(document, out);
}

void
ns_h5_append(ns_node *parent, ns_node *child)
{
    ns_node_append_child(parent, child);
}

void
ns_h5_insert_before(ns_node *sibling, ns_node *child)
{
    ns_node *parent = sibling->parent;
    if (!parent || sibling == child) return;
    ns_node_remove(child);
    child->parent = parent;
    child->prev_sibling = sibling->prev_sibling;
    child->next_sibling = sibling;
    if (sibling->prev_sibling)
        sibling->prev_sibling->next_sibling = child;
    else
        parent->first_child = child;
    sibling->prev_sibling = child;
    ns_dom_tree_changed();
}

static gsize
ns_h5_text_capacity(gsize len)
{
    gsize cap = 16;
    while (cap < len + 1) cap <<= 1;
    return cap;
}

static void
ns_h5_extend_text(ns_node *node, const char *text, size_t len)
{
    size_t add = strnlen(text, len);
    gsize old_len = node->text_len;
    gsize new_len = old_len + add;
    if (add == 0 || new_len > G_MAXUINT32) return;
    if (!(node->flags & NS_NODE_OWN_TEXT)) {
        char *owned = g_malloc(ns_h5_text_capacity(new_len));
        if (old_len) memcpy(owned, node->text, old_len);
        node->text = owned;
        node->flags |= NS_NODE_OWN_TEXT;
    } else if (ns_h5_text_capacity(new_len) != ns_h5_text_capacity(old_len)) {
        node->text = g_realloc(node->text, ns_h5_text_capacity(new_len));
    }
    memcpy(node->text + old_len, text, add);
    node->text[new_len] = '\0';
    node->text_len = (guint32)new_len;
}

static ns_node *
ns_h5_text_node(ns_h5_sink *sink, const char *text, size_t len)
{
    ns_node *out = ns_node_new_text(NULL);
    ns_node_set_text_borrow(out, ns_h5_arena_copy(sink, text, len));
    return out;
}

void
ns_h5_append_text(ns_h5_sink *sink, ns_node *parent,
                  const char *text, size_t len)
{
    if (!text || len == 0) return;
    ns_node *last = parent->last_child;
    if (last && last->kind == NS_NODE_TEXT)
        ns_h5_extend_text(last, text, len);
    else
        ns_node_append_child(parent, ns_h5_text_node(sink, text, len));
}

void
ns_h5_insert_text_before(ns_h5_sink *sink, ns_node *sibling,
                         const char *text, size_t len)
{
    if (!text || len == 0) return;
    ns_node *prev = sibling->prev_sibling;
    if (prev && prev->kind == NS_NODE_TEXT)
        ns_h5_extend_text(prev, text, len);
    else
        ns_h5_insert_before(sibling, ns_h5_text_node(sink, text, len));
}

ns_node *
ns_h5_parent(ns_node *node)
{
    return node->parent;
}

void
ns_h5_remove(ns_node *node)
{
    ns_node_remove(node);
}

void
ns_h5_reparent_children(ns_node *from, ns_node *to)
{
    while (from->first_child)
        ns_node_append_child(to, from->first_child);
}

ns_node *
ns_h5_template_contents(ns_node *template_element)
{
    return ns_template_content_get(template_element);
}

void
ns_h5_discard(ns_node *node)
{
    ns_node_free(node);
}

static gboolean
ns_valid_shadow_host(const char *name)
{
    if (!name) return FALSE;
    static const char *const hosts[] = {
        "article", "aside", "blockquote", "body", "div", "footer",
        "h1", "h2", "h3", "h4", "h5", "h6", "header", "main", "nav",
        "p", "section", "span",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(hosts); i++)
        if (g_ascii_strcasecmp(name, hosts[i]) == 0) return TRUE;
    return strchr(name, '-') != NULL;
}

static void
ns_dsd_convert(ns_node *n, int depth)
{
    if (!n || depth >= 512) return;
    gboolean host_ok = n->kind == NS_NODE_ELEMENT &&
                       ns_valid_shadow_host(n->name);
    gboolean shadow_done = FALSE;
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (host_ok && !shadow_done && c->kind == NS_NODE_ELEMENT && c->name &&
            g_ascii_strcasecmp(c->name, "template") == 0) {
            const char *mode = ns_element_get_attr(c, "shadowrootmode");
            if (!mode) mode = ns_element_get_attr(c, "shadowroot");
            if (mode && (g_ascii_strcasecmp(mode, "open") == 0 ||
                         g_ascii_strcasecmp(mode, "closed") == 0)) {
                shadow_done = TRUE;
                gboolean delegates =
                    ns_element_get_attr(c, "shadowrootdelegatesfocus") != NULL;
                gboolean serializable =
                    ns_element_get_attr(c, "shadowrootserializable") != NULL;
                gboolean clonable =
                    ns_element_get_attr(c, "shadowrootclonable") != NULL;
                ns_node_set_name_borrow(c, "div");
                ns_element_remove_attr(c, "shadowrootmode");
                ns_element_remove_attr(c, "shadowroot");
                ns_element_remove_attr(c, "shadowrootdelegatesfocus");
                ns_element_remove_attr(c, "shadowrootserializable");
                ns_element_remove_attr(c, "shadowrootclonable");
                if (c->tpl_content) {
                    ns_node *frag = c->tpl_content;
                    c->tpl_content = NULL;
                    ns_node *ch = frag->first_child;
                    while (ch) {
                        ns_node *next = ch->next_sibling;
                        ns_node_remove(ch);
                        ns_node_append_child(c, ch);
                        ch = next;
                    }
                    ns_node_free(frag);
                }
                ns_element_set_attr(c, NS_SHADOW_ATTR,
                    g_ascii_strcasecmp(mode, "closed") == 0 ? "closed" : "open");
                ns_element_set_attr(c, "data-nd-shadow-declarative", "1");
                if (delegates)
                    ns_element_set_attr(c, "data-nd-shadow-delegates", "1");
                if (serializable)
                    ns_element_set_attr(c, "data-nd-shadow-serializable", "1");
                if (clonable)
                    ns_element_set_attr(c, "data-nd-shadow-clonable", "1");
            }
        }
        ns_dsd_convert(c, depth + 1);
    }
    if (n->tpl_content)
        ns_dsd_convert(n->tpl_content, depth + 1);
}

static void
text_descendants_append(const ns_node *n, GString *out, int depth)
{
    if (!n || !out || depth >= 64) return;
    if (n->kind == NS_NODE_TEXT && n->text)
        g_string_append(out, n->text);
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        text_descendants_append(c, out, depth + 1);
}

static char *
script_text(const ns_node *n)
{
    GString *out = g_string_new(NULL);
    text_descendants_append(n, out, 0);
    return g_string_free(out, FALSE);
}

static char *
json_string_unescape(const char *p)
{
    if (!p) return NULL;
    const char *stop = p;
    ns_json_decode_string(p, NULL, NULL, &stop, 0);
    char *out = g_malloc((gsize)(stop - p) + 1);
    ns_json_decode_string(p, NULL, out, NULL, 0);
    return out;
}

static const char *
json_string_value_for_key(const char *text, const char *key, const char **out_key)
{
    const char *p = text;
    gsize klen = key ? strlen(key) : 0;
    if (!text || !key || klen == 0) return NULL;
    while ((p = strstr(p, key))) {
        const char *q = p + klen;
        while (g_ascii_isspace(*q)) q++;
        if (*q == ':') {
            q++;
            while (g_ascii_isspace(*q)) q++;
            if (*q == '"') {
                if (out_key) *out_key = p;
                return q + 1;
            }
        }
        p += klen;
    }
    return NULL;
}

static char *
json_first_url_for_key(const char *text, const char *key)
{
    const char *value = json_string_value_for_key(text, key, NULL);
    return value ? json_string_unescape(value) : NULL;
}

static gboolean
attr_contains_word(const ns_node *n, const char *attr, const char *word)
{
    const char *v = ns_element_get_attr(n, attr);
    return v && word && g_strstr_len(v, -1, word) != NULL;
}

static ns_node *
find_element_with_attr_contains(ns_node *n, const char *attr, const char *word,
                                int depth)
{
    if (!n || depth >= 512) return NULL;
    if (n->kind == NS_NODE_ELEMENT && attr_contains_word(n, attr, word))
        return n;
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        ns_node *hit = find_element_with_attr_contains(c, attr, word, depth + 1);
        if (hit) return hit;
    }
    return NULL;
}

static ns_node *
document_media_target(const ns_node *root)
{
    ns_node *video = ns_node_find_first_element(root, "video");
    if (video) return video;
    ns_node *target = ns_node_find_by_id(root, "player");
    if (target) return target;
    return find_element_with_attr_contains((ns_node *)root, "class", "player",
                                           0);
}

static ns_node *
find_meta_property(ns_node *n, const char *prop, int depth)
{
    if (!n || depth >= 512) return NULL;
    if (n->kind == NS_NODE_ELEMENT && n->name && strcmp(n->name, "meta") == 0) {
        const char *p = ns_element_get_attr(n, "property");
        if (!p) p = ns_element_get_attr(n, "name");
        const char *c = ns_element_get_attr(n, "content");
        if (p && c && *c && g_ascii_strcasecmp(p, prop) == 0)
            return n;
    }
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        ns_node *hit = find_meta_property(c, prop, depth + 1);
        if (hit) return hit;
    }
    return NULL;
}

static char *
meta_property_content(const ns_node *root, const char *prop)
{
    ns_node *m = find_meta_property((ns_node *)root, prop, 0);
    const char *c = m ? ns_element_get_attr(m, "content") : NULL;
    return (c && *c) ? g_strdup(c) : NULL;
}

static char *
jsonld_videoobject_url(const ns_node *n, const char *key, int depth)
{
    if (!n || depth >= 512) return NULL;
    if (n->kind == NS_NODE_ELEMENT && n->name &&
        strcmp(n->name, "script") == 0) {
        const char *type = ns_element_get_attr(n, "type");
        if (type && g_ascii_strcasecmp(type, "application/ld+json") == 0) {
            g_autofree char *text = script_text(n);
            if (text && strstr(text, "VideoObject")) {
                char *v = json_first_url_for_key(text, key);
                if (v && *v) return v;
                g_free(v);
            }
        }
    }
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        char *hit = jsonld_videoobject_url(c, key, depth + 1);
        if (hit) return hit;
    }
    return NULL;
}

static gboolean
media_url_is_direct_playable(const char *url)
{
    if (!url || !g_str_has_prefix(url, "http")) return FALSE;
    const char *cut = strpbrk(url, "?#");
    gsize len = cut ? (gsize)(cut - url) : strlen(url);
    const char *dot = NULL;
    for (const char *p = url; p < url + len; p++)
        if (*p == '.') dot = p;
    if (!dot) return FALSE;
    g_autofree char *ext = g_ascii_strdown(dot, (gssize)(url + len - dot));
    return strcmp(ext, ".webm") == 0 || strcmp(ext, ".mpg") == 0 ||
           strcmp(ext, ".mpeg") == 0 || strcmp(ext, ".m1v") == 0 ||
           strcmp(ext, ".ogv") == 0 || strcmp(ext, ".ogg") == 0;
}

static char *
first_nonempty(char *a, char *b)
{
    if (a) { g_free(b); return a; }
    return b;
}

static void
ns_media_extract_standard(const ns_node *root)
{
    ns_node *target = document_media_target(root);
    if (!target || ns_element_get_attr(target, NS_MEDIA_SRC_ATTR) ||
        ns_element_get_attr(target, NS_MEDIA_STREAM_ATTR))
        return;

    char *direct = jsonld_videoobject_url(root, "\"contentUrl\"", 0);
    if (direct && !media_url_is_direct_playable(direct)) {
        g_free(direct);
        direct = NULL;
    }
    char *og_video = meta_property_content(root, "og:video:secure_url");
    og_video = first_nonempty(og_video,
                              meta_property_content(root, "og:video:url"));
    og_video = first_nonempty(og_video,
                              meta_property_content(root, "og:video"));
    char *tw_player = meta_property_content(root, "twitter:player");

    char *poster = meta_property_content(root, "og:image:secure_url");
    poster = first_nonempty(poster, meta_property_content(root, "og:image"));
    poster = first_nonempty(poster,
                            jsonld_videoobject_url(root, "\"thumbnailUrl\"", 0));
    poster = first_nonempty(poster,
                            meta_property_content(root, "twitter:image"));

    char *embed = jsonld_videoobject_url(root, "\"embedUrl\"", 0);
    gboolean has_video = direct || og_video || tw_player || embed;
    if (has_video) {
        if (direct)
            ns_element_set_attr(target, NS_MEDIA_SRC_ATTR, direct);
        else if (og_video && media_url_is_direct_playable(og_video))
            ns_element_set_attr(target, NS_MEDIA_SRC_ATTR, og_video);
        else
            ns_element_set_attr(target, NS_MEDIA_STREAM_ATTR, "1");
        if (poster && *poster)
            ns_element_set_attr(target, NS_MEDIA_POSTER_ATTR, poster);
    }
    g_free(direct);
    g_free(og_video);
    g_free(tw_player);
    g_free(poster);
    g_free(embed);
}

static gboolean
ns_text_all_whitespace(const char *s)
{
    if (!s) return TRUE;
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s != '\f')
            return FALSE;
    return TRUE;
}

static void
ns_prune_html_interelement_whitespace(ns_node *root)
{
    if (!root) return;
    ns_node *html = NULL;
    for (ns_node *c = root->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            g_ascii_strcasecmp(c->name, "html") == 0) { html = c; break; }
    if (!html) return;
    gboolean has_head = FALSE, has_body = FALSE;
    for (ns_node *c = html->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_ELEMENT && c->name) {
            if (g_ascii_strcasecmp(c->name, "head") == 0) has_head = TRUE;
            else if (g_ascii_strcasecmp(c->name, "body") == 0) has_body = TRUE;
        }
    if (!has_head || !has_body) return;
    ns_node *c = html->first_child;
    while (c) {
        ns_node *next = c->next_sibling;
        if (c->kind == NS_NODE_TEXT && ns_text_all_whitespace(c->text)) {
            ns_node_remove(c);
            ns_node_free(c);
        }
        c = next;
    }
}

static void
ns_collect_script_elems(ns_node *n, GPtrArray *out, int depth)
{
    if (!n || depth >= 512) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            g_ascii_strcasecmp(c->name, "script") == 0)
            g_ptr_array_add(out, c);
        ns_collect_script_elems(c, out, depth + 1);
    }
}

static void
ns_html_assign_script_positions(ns_node *root, const char *input, size_t len)
{
    if (!root || !input) return;
    GArray *lines = g_array_new(FALSE, FALSE, sizeof(int));
    GArray *cols  = g_array_new(FALSE, FALSE, sizeof(int));
    int line = 1, col = 1;
    size_t i = 0;
    while (i < len) {
        if (input[i] == '<' && i + 7 <= len &&
            g_ascii_strncasecmp(input + i, "<script", 7) == 0 &&
            (i + 7 == len || !g_ascii_isalnum(input[i + 7]))) {
            size_t j = i;
            int l = line, c = col;
            while (j < len && input[j] != '>') {
                if (input[j] == '\n') { l++; c = 1; } else c++;
                j++;
            }
            if (j < len) {
                c++; j++;
                g_array_append_val(lines, l);
                g_array_append_val(cols, c);
                line = l; col = c; i = j;
                continue;
            }
        }
        if (input[i] == '\n') { line++; col = 1; } else col++;
        i++;
    }
    GPtrArray *elems = g_ptr_array_new();
    ns_collect_script_elems(root, elems, 0);
    guint m = MIN(elems->len, lines->len);
    for (guint k = 0; k < m; k++) {
        ns_node *e = g_ptr_array_index(elems, k);
        e->src_line = g_array_index(lines, int, k);
        e->src_col  = g_array_index(cols, int, k);
    }
    g_ptr_array_free(elems, TRUE);
    g_array_free(lines, TRUE);
    g_array_free(cols, TRUE);
}

ns_node *
ns_html_parse_with_scripting(const char *input, gssize len,
                             gboolean scripting)
{
    if (!input) return NULL;
    size_t n = (len < 0) ? strlen(input) : (size_t)len;
    ns_node *root = ns_node_new_document();
    ns_h5_sink *sink = ns_h5_sink_new();
    ns_node_attach_backing(root, sink, ns_h5_sink_free);
    int quirks = ns_rs_html_parse_document(input, n, scripting, root, sink);
    if (quirks == 2)
        root->flags |= NS_NODE_QUIRKS;
    else if (quirks == 1)
        root->flags |= NS_NODE_LIMITED_QUIRKS;
    if (!scripting) root->flags |= NS_NODE_SCRIPTING_DISABLED;
    ns_prune_html_interelement_whitespace(root);
    ns_dsd_convert(root, 0);
    ns_media_extract_standard(root);
    return root;
}

ns_node *
ns_html_parse(const char *input, gssize len)
{
    return ns_html_parse_with_scripting(input, len, TRUE);
}

char *
ns_html_named_char_ref(const char *name, gsize len)
{
    return ns_rs_html_named_char_ref(name, len);
}

static gboolean
ns_mime_is_xhtml(const char *content_type)
{
    static const char xhtml[] = "application/xhtml+xml";
    if (!content_type) return FALSE;
    while (g_ascii_isspace(*content_type)) content_type++;
    if (g_ascii_strncasecmp(content_type, xhtml, sizeof xhtml - 1) != 0)
        return FALSE;
    char next = content_type[sizeof xhtml - 1];
    return next == '\0' || next == ';' || g_ascii_isspace(next);
}

static void
ns_xml_mark_parser_inserted(ns_node *n, int depth)
{
    if (!n || depth >= 512) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        c->flags &= ~NS_NODE_NOT_PARSER_INSERTED;
        ns_xml_mark_parser_inserted(c, depth + 1);
        ns_xml_mark_parser_inserted(c->tpl_content, depth + 1);
    }
}

static ns_node *
ns_xhtml_parse_document(const char *input, size_t len, gboolean scripting)
{
    ns_node *root = ns_xml_parse_html_entities(input, (gssize)len);
    if (!root) return NULL;
    root->flags |= NS_NODE_XML_DOC;
    if (!scripting) root->flags |= NS_NODE_SCRIPTING_DISABLED;
    ns_xml_mark_parser_inserted(root, 0);
    ns_html_assign_script_positions(root, input, len);
    ns_media_extract_standard(root);
    return root;
}

ns_node *
ns_parse_document_for_type(const char *input, gssize len,
                           const char *content_type, gboolean scripting)
{
    if (!input) return NULL;
    size_t n = (len < 0) ? strlen(input) : (size_t)len;
    ns_node *doc = ns_mime_is_xhtml(content_type)
        ? ns_xhtml_parse_document(input, n, scripting) : NULL;
    return doc ? doc : ns_html_parse_with_scripting(input, (gssize)n,
                                                    scripting);
}

static ns_node *
parse_fragment_in_namespace(const char *context_tag, int context_ns,
                            const char *input, gssize len, gboolean scripting)
{
    if (!input) return NULL;
    size_t n = (len < 0) ? strlen(input) : (size_t)len;
    g_autofree char *context = !context_tag ? NULL
        : context_ns == NS_H5_NS_HTML ? g_ascii_strdown(context_tag, -1)
        : g_strdup(context_tag);
    ns_node *out = ns_node_new_document();
    if (!scripting) out->flags |= NS_NODE_SCRIPTING_DISABLED;
    ns_h5_sink *sink = ns_h5_sink_new();
    ns_node_attach_backing(out, sink, ns_h5_sink_free);
    ns_rs_html_parse_fragment(context, context ? strlen(context) : 0,
                              context_ns, input, n, scripting, out, sink);
    return out;
}

ns_node *
ns_html_parse_fragment_with_scripting(const char *context_tag,
                                      const char *input, gssize len,
                                      gboolean scripting)
{
    return parse_fragment_in_namespace(context_tag, NS_H5_NS_HTML, input, len,
                                       scripting);
}

ns_node *
ns_html_parse_fragment_in_context(const ns_node *context, const char *input,
                                  gssize len, gboolean scripting)
{
    const char *tag = context && context->kind == NS_NODE_ELEMENT
        ? context->name : NULL;
    int ns = NS_H5_NS_HTML;
    if (tag && (context->flags & NS_NODE_SVG_NS)) {
        ns = NS_H5_NS_SVG;
    } else if (tag && (context->flags & NS_NODE_FOREIGN_NS)) {
        const char *uri = ns_element_get_attr(context, "data-nd-ns-uri");
        if (uri && strcmp(uri, "http://www.w3.org/1998/Math/MathML") == 0)
            ns = NS_H5_NS_MATHML;
    }
    return parse_fragment_in_namespace(tag, ns, input, len, scripting);
}

void
ns_html_convert_declarative_shadow(ns_node *root)
{
    ns_dsd_convert(root, 0);
}
