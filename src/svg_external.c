/* Nordstjernen — SVG documents fetched for external <use> references.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "svg_external.h"

#include <string.h>

#include "html.h"
#include "net.h"
#include "svg.h"

enum {
    NS_SVG_EXTERNAL_MAX_BYTES = 8 * 1024 * 1024,
    NS_SVG_EXTERNAL_MAX_DOCS  = 64,
};

typedef struct {
    ns_node  *doc;
    gboolean  pending;
} svg_external_doc;

typedef struct {
    char *base_url;
    char *document_url;
} svg_external_base;

static void
svg_external_base_free(gpointer data)
{
    svg_external_base *b = data;
    g_free(b->base_url);
    g_free(b->document_url);
    g_free(b);
}

static GHashTable *g_bases;
static GHashTable *g_docs;
static guint       g_pending;
static guint64     g_generation;

static void
svg_external_doc_free(gpointer data)
{
    svg_external_doc *d = data;
    if (d->doc) ns_node_free(d->doc);
    g_free(d);
}

void
ns_svg_external_note_base(const ns_node *doc, const char *base_url,
                          const char *document_url)
{
    if (!doc || !base_url) return;
    if (!document_url) document_url = base_url;
    if (!g_bases)
        g_bases = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                        NULL, svg_external_base_free);
    const svg_external_base *known = g_hash_table_lookup(g_bases, doc);
    if (known && strcmp(known->base_url, base_url) == 0 &&
        strcmp(known->document_url, document_url) == 0)
        return;
    svg_external_base *b = g_new0(svg_external_base, 1);
    b->base_url = g_strdup(base_url);
    b->document_url = g_strdup(document_url);
    g_hash_table_insert(g_bases, (gpointer)doc, b);
}

static gboolean
svg_external_urls_for(const ns_node *n, const char **base_url,
                      const char **document_url)
{
    while (n && n->parent &&
           (n->kind != NS_NODE_DOCUMENT || (n->flags & NS_NODE_FRAGMENT)))
        n = n->parent;
    if (!n) return FALSE;
    if (n->parent) {
        if (n->flags & NS_NODE_SANDBOX_OPAQUE) return FALSE;
        const char *frame_url = ns_element_get_attr(n->parent,
                                                    "data-nd-frame-url");
        if (!frame_url || !*frame_url) return FALSE;
        *base_url = frame_url;
        *document_url = frame_url;
        return TRUE;
    }
    const svg_external_base *top = g_bases ? g_hash_table_lookup(g_bases, n)
                                           : NULL;
    if (!top) return FALSE;
    *base_url = top->base_url;
    *document_url = top->document_url;
    return TRUE;
}

static void
svg_external_fetched(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    char *url = user_data;
    ns_response *resp = ns_net_fetch_finish(result, NULL);
    svg_external_doc *d = g_docs ? g_hash_table_lookup(g_docs, url) : NULL;
    if (d && d->pending) {
        d->pending = FALSE;
        GByteArray *body = resp ? resp->body : NULL;
        if (resp && resp->status >= 200 && resp->status < 300 && body &&
            (!resp->final_url || ns_url_same_origin(resp->final_url, url)) &&
            body->len > 0 && body->len <= NS_SVG_EXTERNAL_MAX_BYTES &&
            ns_svg_bytes_look_like_svg(body->data, body->len))
            d->doc = ns_svg_parse_bytes(body->data, body->len);
    }
    if (g_pending > 0) g_pending--;
    g_generation++;
    if (resp) ns_response_free(resp);
    g_free(url);
}

static svg_external_doc *
svg_external_doc_for(const char *url, const char *base)
{
    if (!g_docs)
        g_docs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       svg_external_doc_free);
    svg_external_doc *d = g_hash_table_lookup(g_docs, url);
    if (d) return d;
    if (g_hash_table_size(g_docs) >= NS_SVG_EXTERNAL_MAX_DOCS) return NULL;
    d = g_new0(svg_external_doc, 1);
    d->pending = TRUE;
    g_hash_table_insert(g_docs, g_strdup(url), d);
    g_pending++;
    ns_net_fetch_async(url, base, NULL, svg_external_fetched, g_strdup(url));
    return d;
}

const ns_node *
ns_svg_external_target(const ns_node *referrer, const char *href)
{
    const char *hash = href ? strchr(href, '#') : NULL;
    if (!hash || hash == href || !hash[1]) return NULL;
    const char *base_url = NULL;
    const char *document_url = NULL;
    if (!svg_external_urls_for(referrer, &base_url, &document_url))
        return NULL;
    g_autofree char *raw = g_strndup(href, (gsize)(hash - href));
    g_autofree char *url = ns_url_resolve(base_url, raw);
    if (!url || !ns_url_is_http_or_https(url) ||
        !ns_url_same_origin(url, document_url))
        return NULL;
    svg_external_doc *d = svg_external_doc_for(url, document_url);
    if (!d || !d->doc) return NULL;
    return ns_node_find_by_id(d->doc, hash + 1);
}

guint
ns_svg_external_pending(void)
{
    return g_pending;
}

guint64
ns_svg_external_generation(void)
{
    return g_generation;
}
