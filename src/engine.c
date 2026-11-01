/* Nordstjernen — synchronous fetch/cascade/layout/capture pipeline.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "engine.h"

#include <cairo-pdf.h>
#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "csp.h"
#include "css.h"
#include "css_syntax.h"
#include "debuglog.h"
#include "image.h"
#include "paint.h"
#include "print.h"
#include "render.h"
#include "security.h"
#include "svg_external.h"

typedef struct fetch_state {
    GMainLoop  *loop;
    ns_response *resp;
    GError      *error;
} fetch_state;

static int g_engine_blocking_depth;

gboolean
ns_engine_in_blocking_fetch(void)
{
    return g_engine_blocking_depth > 0;
}

static guint64 g_engine_relayout_count;
static gint64  g_engine_relayout_us;

static void
ns_engine_perf_add_relayout(gint64 elapsed_us)
{
    g_engine_relayout_count++;
    g_engine_relayout_us += elapsed_us;
}

void
ns_engine_layout_perf(guint64 *relayouts, double *total_ms)
{
    if (relayouts) *relayouts = g_engine_relayout_count;
    if (total_ms)  *total_ms  = g_engine_relayout_us / 1000.0;
}

static guint64 g_engine_blocking_count;
static gint64  g_engine_blocking_us;

static void
engine_blocking_wait(GMainLoop *loop)
{
    gboolean outermost = g_engine_blocking_depth == 0;
    gint64 t0 = g_get_monotonic_time();
    g_engine_blocking_depth++;
    g_main_loop_run(loop);
    g_engine_blocking_depth--;
    g_engine_blocking_count++;
    if (outermost) g_engine_blocking_us += g_get_monotonic_time() - t0;
}

void
ns_engine_blocking_perf(guint64 *waits, double *total_ms)
{
    if (waits)    *waits    = g_engine_blocking_count;
    if (total_ms) *total_ms = g_engine_blocking_us / 1000.0;
}

static gint64 g_engine_encode_us;

double
ns_engine_encode_ms(void)
{
    return g_engine_encode_us / 1000.0;
}

static void
on_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    fetch_state *st = user_data;
    st->resp = ns_net_fetch_finish(result, &st->error);
    g_main_loop_quit(st->loop);
}

static ns_response *
engine_request_blocking(const char *url, const char *top_url,
                        const char *method, const void *body, gsize body_len,
                        const char *content_type, gboolean navigation,
                        const char *initiator, gboolean user_activated,
                        const char *const *resource_headers,
                        GError **error)
{
    fetch_state st = {0};
    st.loop = g_main_loop_new(NULL, FALSE);
    g_autofree char *initiator_origin =
        navigation && initiator ? ns_url_origin_from(initiator) : NULL;
    g_autofree char *initiator_header =
        initiator_origin && *initiator_origin &&
        !strpbrk(initiator_origin, "\r\n")
            ? g_strdup_printf("X-ND-Initiator: %s", initiator_origin) : NULL;
    const char *navigation_headers[4] = { "X-ND-Navigate: 1" };
    int nh = 1;
    if (user_activated) navigation_headers[nh++] = "X-ND-User-Activated: 1";
    if (initiator_header) navigation_headers[nh++] = initiator_header;
    const char *const *headers = navigation ? navigation_headers
                                            : resource_headers;
    ns_net_request_async(url, top_url, method, body, body_len, content_type,
                         headers,
                         NULL, on_fetch_done, &st);
    engine_blocking_wait(st.loop);
    g_main_loop_unref(st.loop);
    if (error) *error = st.error;
    else g_clear_error(&st.error);
    return st.resp;
}

ns_response *
ns_engine_fetch_blocking(const char *url, const char *top_url, GError **error)
{
    return engine_request_blocking(url, top_url, "GET", NULL, 0, NULL,
                                   FALSE, NULL, FALSE, NULL, error);
}

static ns_response *
engine_fetch_blocking_with_headers(const char *url, const char *top_url,
                                   const char *const *headers, GError **error)
{
    return engine_request_blocking(url, top_url, "GET", NULL, 0, NULL,
                                   FALSE, NULL, FALSE, headers, error);
}

ns_response *
ns_engine_navigate_blocking(const char *url, const char *top_url,
                            const char *initiator,
                            gboolean user_activated, GError **error)
{
    return engine_request_blocking(url, top_url, "GET", NULL, 0, NULL,
                                   TRUE, initiator, user_activated, NULL,
                                   error);
}

ns_response *
ns_engine_reload_blocking(const char *url, const char *top_url, GError **error)
{
    static const char *const reload_headers[] = {
        "X-ND-Navigate: 1", "X-ND-User-Activated: 1", "X-ND-Reload: 1", NULL
    };
    return engine_request_blocking(url, top_url, "GET", NULL, 0, NULL,
                                   FALSE, NULL, FALSE, reload_headers, error);
}

ns_response *
ns_engine_navigate_post_blocking(const char *url, const char *top_url,
                                 const char *initiator,
                                 const void *body, gsize body_len,
                                 const char *content_type,
                                 gboolean user_activated, GError **error)
{
    return engine_request_blocking(url, top_url, "POST", body, body_len,
                                   content_type, TRUE, initiator,
                                   user_activated, NULL, error);
}

static gboolean
content_type_is_css(const char *ct)
{
    if (!ct || !*ct) return TRUE;
    while (*ct == ' ' || *ct == '\t') ct++;
    return g_ascii_strncasecmp(ct, "text/css", 8) == 0 &&
           (ct[8] == '\0' || ct[8] == ';' || ct[8] == ' ' || ct[8] == '\t');
}

#define NS_LINKED_CSS_MAX 128

static GHashTable *g_linked_css;
static GHashTable *g_css_redirected_cross_origin;

static void
engine_note_css_landing(const char *url, const char *landed)
{
    if (!url || !*url) return;
    if (!g_css_redirected_cross_origin)
        g_css_redirected_cross_origin =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (landed && !ns_url_same_origin(url, landed))
        g_hash_table_add(g_css_redirected_cross_origin, g_strdup(url));
    else
        g_hash_table_remove(g_css_redirected_cross_origin, url);
}

static void
engine_remember_linked_css(const char *url, GBytes *bytes)
{
    if (!url || !*url || !bytes) return;
    if (g_css_redirected_cross_origin &&
        g_hash_table_contains(g_css_redirected_cross_origin, url)) {
        if (g_linked_css) g_hash_table_remove(g_linked_css, url);
        return;
    }
    if (!g_linked_css)
        g_linked_css = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)g_bytes_unref);
    if (g_hash_table_size(g_linked_css) >= NS_LINKED_CSS_MAX &&
        !g_hash_table_contains(g_linked_css, url))
        g_hash_table_remove_all(g_linked_css);
    g_hash_table_insert(g_linked_css, g_strdup(url), g_bytes_ref(bytes));
}

gboolean
ns_engine_linked_css_known(const char *url)
{
    return url && *url && g_linked_css &&
           g_hash_table_contains(g_linked_css, url);
}

char *
ns_engine_linked_css_text(const char *url)
{
    if (!url || !*url || !g_linked_css) return NULL;
    GBytes *b = g_hash_table_lookup(g_linked_css, url);
    if (!b) return NULL;
    gsize len = 0;
    const char *data = g_bytes_get_data(b, &len);
    if (!data || len == 0) return NULL;
    return g_strndup(data, len);
}

#define NS_ENGINE_TIMING_CAP 256

static GMutex     engine_timing_lock;
static GPtrArray *engine_timings;

static void
engine_resource_timing_free(gpointer p)
{
    ns_engine_resource_timing *t = p;
    if (!t) return;
    g_free(t->top_url);
    g_free(t->url);
    ns_response_free(t->resp);
    g_free(t);
}

static void
engine_record_timing(const char *top_url, const char *url,
                     const char *initiator, gint64 start_us,
                     ns_response *resp, gboolean render_blocking,
                     gboolean in_frame)
{
    if (!top_url || !url || !resp) return;
    ns_engine_resource_timing *t = g_new0(ns_engine_resource_timing, 1);
    t->top_url = g_strdup(top_url);
    t->url = g_strdup(url);
    t->initiator = initiator;
    t->render_blocking = render_blocking;
    t->in_frame = in_frame;
    t->start_us = start_us;
    t->end_us = g_get_monotonic_time();
    t->resp = resp;
    g_mutex_lock(&engine_timing_lock);
    if (!engine_timings)
        engine_timings = g_ptr_array_new_with_free_func(engine_resource_timing_free);
    if (engine_timings->len >= NS_ENGINE_TIMING_CAP)
        g_ptr_array_remove_index(engine_timings, 0);
    g_ptr_array_add(engine_timings, t);
    g_mutex_unlock(&engine_timing_lock);
}

GPtrArray *
ns_engine_take_resource_timings(const char *top_url)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(engine_resource_timing_free);
    if (!top_url) return out;
    g_mutex_lock(&engine_timing_lock);
    for (guint i = 0; engine_timings && i < engine_timings->len; ) {
        ns_engine_resource_timing *t = g_ptr_array_index(engine_timings, i);
        if (g_strcmp0(t->top_url, top_url) == 0) {
            g_ptr_array_add(out, g_ptr_array_steal_index(engine_timings, i));
        } else {
            i++;
        }
    }
    g_mutex_unlock(&engine_timing_lock);
    return out;
}

static GBytes *
fetch_css_bytes(const char *url, const char *top_url, GHashTable *cache,
                gboolean strict_mime, const char *initiator,
                gboolean render_blocking, gboolean in_frame)
{
    if (!url || !*url) return NULL;
    guint8 attempts = 0;
    if (cache) {
        GBytes *hit = g_hash_table_lookup(cache, url);
        if (hit) {
            gsize hsize = 0;
            const guint8 *hdata = g_bytes_get_data(hit, &hsize);
            gboolean fail_marker = hsize == 0 ||
                (hsize == 1 && hdata && hdata[0] <= 8);
            if (!fail_marker) return g_bytes_ref(hit);
            attempts = hsize == 1 ? hdata[0] : 1;
            if (attempts >= 3) return NULL;
        }
    }
    gint64 start_us = g_get_monotonic_time();
    ns_response *resp = engine_fetch_blocking_with_headers(
        url, top_url, ns_net_accept_headers_for(NS_FETCH_DEST_STYLE), NULL);
    GBytes *bytes = NULL;
    const char *landed = resp && resp->final_url && *resp->final_url
        ? resp->final_url : url;
    if (resp) engine_note_css_landing(url, landed);
    gboolean enforce_mime = strict_mime ||
        (top_url && !ns_url_same_origin(landed, top_url)) ||
        (resp && ns_net_header_is_nosniff(resp->x_content_type_options));
    gboolean mime_ok = !enforce_mime || !resp ||
                       content_type_is_css(resp->content_type);
    if (resp && !resp->error && resp->status < 400 && mime_ok &&
        resp->body && resp->body->len > 0) {
        bytes = g_bytes_new(resp->body->data, resp->body->len);
        if (cache)
            g_hash_table_insert(cache, g_strdup(url), g_bytes_ref(bytes));
    } else if (cache) {
        guint8 marker = attempts + 1;
        g_hash_table_insert(cache, g_strdup(url),
                            g_bytes_new(&marker, 1));
    }
    if (resp) engine_record_timing(top_url, url, initiator, start_us, resp,
                                   render_blocking, in_frame);
    return bytes;
}

static gboolean
rel_has_token(const char *rel, const char *token)
{
    if (!rel || !token || !*token) return FALSE;
    gchar **parts = g_strsplit_set(rel, " \t\r\n\f", -1);
    gboolean found = FALSE;
    for (gchar **p = parts; *p; p++) {
        if (**p && g_ascii_strcasecmp(*p, token) == 0) {
            found = TRUE;
            break;
        }
    }
    g_strfreev(parts);
    return found;
}

static gboolean
rel_is_stylesheet(const char *rel)
{
    return rel_has_token(rel, "stylesheet") &&
           !rel_has_token(rel, "alternate");
}

typedef struct {
    char                *url;
    ns_fetch_destination dest;
} preload_target;

static void
preload_target_free(gpointer p)
{
    preload_target *t = p;
    if (!t) return;
    g_free(t->url);
    g_free(t);
}

ns_fetch_destination
ns_engine_preload_destination(const char *rel, const char *as)
{
    if (rel_has_token(rel, "modulepreload")) return NS_FETCH_DEST_SCRIPT;
    if (!as || !*as) return NS_FETCH_DEST_DEFAULT;
    if (g_ascii_strcasecmp(as, "script") == 0) return NS_FETCH_DEST_SCRIPT;
    if (g_ascii_strcasecmp(as, "style") == 0)  return NS_FETCH_DEST_STYLE;
    return NS_FETCH_DEST_DEFAULT;
}

static void
preload_add(GPtrArray *out, GHashTable *seen, const char *base,
            const char *ref, ns_fetch_destination dest)
{
    if (!ref || !*ref || g_str_has_prefix(ref, "data:")) return;
    char *abs = ns_url_resolve(base, ref);
    if (!abs) return;
    char *slot = g_strdup_printf("%d\x1f%s", (int)dest, abs);
    if (!ns_url_is_http_or_https(abs) || g_hash_table_contains(seen, slot)) {
        g_free(slot);
        g_free(abs);
        return;
    }
    g_hash_table_add(seen, slot);
    preload_target *target = g_new0(preload_target, 1);
    target->url  = abs;
    target->dest = dest;
    g_ptr_array_add(out, target);
}

static void
preload_collect(const ns_node *n, const char *base, gboolean include_images,
                GPtrArray *out, GHashTable *seen,
                GPtrArray *connect_out, GHashTable *connect_seen, int depth)
{
    if (!n || depth >= 512) return;
    if (include_images && ns_node_is_element_named(n, "img")) {
        const char *loading = ns_element_get_attr(n, "loading");
        if (!loading || g_ascii_strcasecmp(loading, "lazy") != 0)
            preload_add(out, seen, base, ns_element_get_attr(n, "src"),
                        NS_FETCH_DEST_DEFAULT);
    } else if (ns_node_is_element_named(n, "script")) {
        preload_add(out, seen, base, ns_element_get_attr(n, "src"),
                    NS_FETCH_DEST_SCRIPT);
    } else if (ns_node_is_element_named(n, "link")) {
        const char *rel = ns_element_get_attr(n, "rel");
        if (rel && (rel_has_token(rel, "preconnect") ||
                    rel_has_token(rel, "dns-prefetch"))) {
            const char *href = ns_element_get_attr(n, "href");
            char *abs = (href && *href) ? ns_url_resolve(base, href) : NULL;
            char *origin = abs ? ns_url_origin_from(abs) : NULL;
            if (origin && ns_url_is_http_or_https(origin) &&
                !g_hash_table_contains(connect_seen, origin)) {
                g_hash_table_add(connect_seen, g_strdup(origin));
                g_ptr_array_add(connect_out, origin);
                origin = NULL;
            }
            g_free(origin);
            g_free(abs);
        } else if (rel && rel_has_token(rel, "stylesheet")) {
            preload_add(out, seen, base, ns_element_get_attr(n, "href"),
                        NS_FETCH_DEST_STYLE);
        } else if (rel && rel_has_token(rel, "prefetch")) {
            preload_add(out, seen, base, ns_element_get_attr(n, "href"),
                        NS_FETCH_DEST_DEFAULT);
        } else if (rel && (rel_has_token(rel, "preload") ||
                           rel_has_token(rel, "modulepreload"))) {
            preload_add(out, seen, base, ns_element_get_attr(n, "href"),
                        ns_engine_preload_destination(rel,
                                                ns_element_get_attr(n, "as")));
        }
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        preload_collect(c, base, include_images, out, seen,
                        connect_out, connect_seen, depth + 1);
}

typedef struct {
    char   *url;
    gint64  start_us;
} preload_flight;

static GHashTable *g_preload_results;

static void
preload_flight_free(preload_flight *flight)
{
    if (!flight) return;
    g_free(flight->url);
    g_free(flight);
}

static void
preload_remember_result(preload_flight *flight, const ns_response *resp)
{
    if (!flight || !resp || resp->status <= 0) return;
    if (!g_preload_results)
        g_preload_results = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, g_free);
    ns_engine_preload_result *r = g_new0(ns_engine_preload_result, 1);
    r->status = resp->status;
    r->start_us = flight->start_us;
    r->end_us = g_get_monotonic_time();
    g_hash_table_replace(g_preload_results, g_strdup(flight->url), r);
}

static void
preload_results_clear(void)
{
    if (g_preload_results) g_hash_table_remove_all(g_preload_results);
}

gboolean
ns_engine_preload_result_for(const char *url, ns_engine_preload_result *out)
{
    const ns_engine_preload_result *r = url && g_preload_results
        ? g_hash_table_lookup(g_preload_results, url) : NULL;
    if (r && out) *out = *r;
    return r != NULL;
}

static void
on_preload_fetched(GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void)src;
    preload_flight *flight = user_data;
    ns_response *resp = ns_net_fetch_finish(res, NULL);
    preload_remember_result(flight, resp);
    preload_flight_free(flight);
    if (resp) ns_response_free(resp);
}

static preload_flight *
preload_flight_new(const char *url)
{
    preload_flight *flight = g_new0(preload_flight, 1);
    flight->url = g_strdup(url);
    flight->start_us = g_get_monotonic_time();
    return flight;
}

static ns_csp_kind
preload_csp_kind(ns_fetch_destination dest)
{
    return dest == NS_FETCH_DEST_SCRIPT ? NS_CSP_SCRIPT
         : dest == NS_FETCH_DEST_STYLE ? NS_CSP_STYLE
         : dest == NS_FETCH_DEST_IMAGE ? NS_CSP_IMG
         : dest == NS_FETCH_DEST_FONT ? NS_CSP_FONT
         : NS_CSP_CONNECT;
}

static GPtrArray  *g_early_hint_keys;
static GHashTable *g_early_hint_slots;
static char       *g_early_hint_document;

static void
early_hints_forget(void)
{
    if (g_early_hint_keys) g_ptr_array_set_size(g_early_hint_keys, 0);
    if (g_early_hint_slots) g_hash_table_remove_all(g_early_hint_slots);
    g_clear_pointer(&g_early_hint_document, g_free);
}

static gboolean
early_hints_cover(const char *document_url)
{
    return g_early_hint_document && document_url &&
           g_early_hint_slots && g_hash_table_size(g_early_hint_slots) > 0 &&
           ns_url_is_same_site(g_early_hint_document, document_url);
}

void
ns_engine_early_hints(const char *document_url, GPtrArray *hints)
{
    if (!document_url || !hints || !ns_url_is_http_or_https(document_url))
        return;
    if (!early_hints_cover(document_url)) {
        early_hints_forget();
        g_early_hint_document = g_strdup(document_url);
    }
    if (!g_early_hint_keys)
        g_early_hint_keys = g_ptr_array_new_with_free_func(g_free);
    if (!g_early_hint_slots)
        g_early_hint_slots = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, NULL);
    for (guint i = 0; i < hints->len; i++) {
        const ns_net_link_hint *hint = g_ptr_array_index(hints, i);
        if (!hint->url || !hint->rel) continue;
        if (rel_has_token(hint->rel, "preconnect") ||
            rel_has_token(hint->rel, "dns-prefetch")) {
            g_autofree char *origin = ns_url_origin_from(hint->url);
            if (origin && ns_url_is_http_or_https(origin))
                ns_net_preconnect_async(origin);
            continue;
        }
        if (!rel_has_token(hint->rel, "preload") &&
            !rel_has_token(hint->rel, "modulepreload"))
            continue;
        ns_fetch_destination dest =
            ns_engine_preload_destination(hint->rel, hint->as);
        char *slot = g_strdup_printf("%d\x1f%s", (int)dest, hint->url);
        if (g_hash_table_contains(g_early_hint_slots, slot) ||
            g_hash_table_size(g_early_hint_slots) >= NS_ENGINE_EARLY_HINTS_MAX) {
            g_free(slot);
            continue;
        }
        if (!ns_net_csp_allows(document_url, preload_csp_kind(dest), hint->url,
                               NULL, TRUE)) {
            g_free(slot);
            continue;
        }
        g_hash_table_add(g_early_hint_slots, slot);
        const char *const *headers = ns_net_accept_headers_for(dest);
        if (dest != NS_FETCH_DEST_DEFAULT) {
            char *key = ns_net_request_key(hint->url, document_url, "GET",
                                           headers);
            if (key) {
                ns_net_preload_expect(key);
                g_ptr_array_add(g_early_hint_keys, key);
            }
        }
        ns_net_request_async(hint->url, document_url, "GET", NULL, 0, NULL,
                             headers, NULL, on_preload_fetched,
                             preload_flight_new(hint->url));
    }
}

static void
early_hints_adopt(const char *document_url, GHashTable *seen)
{
    if (!early_hints_cover(document_url)) {
        early_hints_forget();
        ns_net_preload_clear();
        preload_results_clear();
        return;
    }
    GPtrArray *keep = g_ptr_array_new();
    for (guint i = 0; i < g_early_hint_keys->len; i++)
        g_ptr_array_add(keep, g_ptr_array_index(g_early_hint_keys, i));
    g_ptr_array_add(keep, NULL);
    ns_net_preload_clear_except((const char *const *)keep->pdata);
    g_ptr_array_free(keep, TRUE);
    GHashTableIter it;
    gpointer slot;
    g_hash_table_iter_init(&it, g_early_hint_slots);
    while (g_hash_table_iter_next(&it, &slot, NULL))
        g_hash_table_add(seen, g_strdup(slot));
    early_hints_forget();
}

void
ns_engine_preload_script(const char *url, const char *top_url)
{
    if (!url || !top_url || !ns_url_is_http_or_https(url)) return;
    const char *const *headers = ns_net_accept_headers_for(NS_FETCH_DEST_SCRIPT);
    char *key = ns_net_request_key(url, top_url, "GET", headers);
    if (!key) return;
    ns_net_preload_expect(key);
    g_free(key);
    ns_net_request_async(url, top_url, "GET", NULL, 0, NULL, headers, NULL,
                         on_preload_fetched, NULL);
}

void
ns_engine_speculative_preload(ns_node *doc, const char *base_url,
                              gboolean include_images)
{
    if (!doc || !base_url || !ns_url_is_http_or_https(base_url))
        return;
    GPtrArray *urls = g_ptr_array_new_with_free_func(preload_target_free);
    GPtrArray *connects = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    GHashTable *connect_seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, NULL);
    g_autofree char *doc_base = ns_js_base_url_for_document(NULL, doc, base_url);
    early_hints_adopt(base_url, seen);
    preload_collect(doc, doc_base ? doc_base : base_url, include_images, urls,
                    seen, connects, connect_seen, 0);
    g_hash_table_destroy(seen);
    g_hash_table_destroy(connect_seen);
    for (guint i = 0; i < connects->len; i++)
        ns_net_preconnect_async(g_ptr_array_index(connects, i));
    for (guint i = 0; i < urls->len; i++) {
        const preload_target *target = g_ptr_array_index(urls, i);
        if (!ns_net_csp_allows(base_url, preload_csp_kind(target->dest),
                               target->url, NULL, TRUE))
            continue;
        const char *const *headers = ns_net_accept_headers_for(target->dest);
        if (target->dest != NS_FETCH_DEST_DEFAULT) {
            char *key = ns_net_request_key(target->url, base_url, "GET", headers);
            ns_net_preload_expect(key);
            g_free(key);
        }
        ns_net_request_async(target->url, base_url, "GET", NULL, 0, NULL,
                             headers, NULL, on_preload_fetched,
                             preload_flight_new(target->url));
    }
    g_ptr_array_free(urls, TRUE);
    g_ptr_array_free(connects, TRUE);
}

static void
append_stylesheet_expanded(GPtrArray *out, ns_css_stylesheet *sh,
                           const char *base_url, const char *top_url,
                           const char *csp_url, GHashTable *seen,
                           GHashTable *cache, int depth,
                           gboolean render_blocking, gboolean in_frame)
{
    if (!out || !sh) return;
    if (depth < NS_CSS_IMPORT_MAX_DEPTH && sh->imports) {
        for (guint i = 0; i < sh->imports->len; i++) {
            ns_css_import *im = &g_array_index(sh->imports, ns_css_import, i);
            if (!im->url || !*im->url) continue;
            if (im->media && *im->media &&
                !ns_css_media_query_matches(im->media))
                continue;
            char *abs = ns_url_resolve(base_url, im->url);
            if (!abs) continue;
            if (!ns_net_csp_allows(csp_url, NS_CSP_STYLE, abs, NULL, TRUE)) {
                g_free(abs);
                continue;
            }
            if (seen && g_hash_table_contains(seen, abs)) {
                g_free(abs);
                continue;
            }
            if (seen) g_hash_table_add(seen, g_strdup(abs));
            GBytes *bytes = fetch_css_bytes(abs, top_url, cache, TRUE, "css",
                                            render_blocking, in_frame);
            if (bytes) {
                ns_css_stylesheet *child =
                    ns_css_stylesheet_parse_import_cached(abs, im->layer_name,
                                                          bytes);
                if (child)
                    append_stylesheet_expanded(out, child, abs, top_url, csp_url,
                                               seen, cache, depth + 1,
                                               render_blocking, in_frame);
                g_bytes_unref(bytes);
            }
            g_free(abs);
        }
    }
    ns_css_stylesheet_resolve_urls(sh, base_url);
    g_ptr_array_add(out, sh);
}

typedef struct {
    GPtrArray  *out;
    GHashTable *cache;
    GArray     *run_chunks;
    const char *run_base;
    const char *top_url;
    const char *csp_url;
    gboolean    strict_css_mime;
    gboolean    media_seen;
    int         frame_depth;
    GPtrArray  *docs;
    ns_node    *doc;
} sheet_collect_ctx;

static gboolean
engine_node_in_head(const ns_node *n)
{
    for (const ns_node *p = n ? n->parent : NULL;
         p && p->kind == NS_NODE_ELEMENT; p = p->parent)
        if (ns_node_is_element_named(p, "head")) return TRUE;
    return FALSE;
}

typedef struct {
    const char *css;
    gsize       len;
    guint64     fp;
    gboolean    has_at;
} sheet_run_chunk;

static void
sheet_run_chunk_clear(gpointer data)
{
    sheet_run_chunk *chunk = data;
    g_ref_string_release((char *)chunk->css);
}

static void
sheet_run_append(sheet_collect_ctx *cc, char *css, gsize len,
                 guint64 fp, gboolean has_at, const char *base_url)
{
    sheet_run_chunk chunk = { g_ref_string_acquire(css), len, fp, has_at };
    g_array_append_val(cc->run_chunks, chunk);
    cc->run_base = base_url;
}

static guint
sheet_run_chunk_hash(gconstpointer p)
{
    const sheet_run_chunk *c = p;
    return (guint)(c->fp ^ (c->fp >> 32));
}

static gboolean
sheet_run_chunk_equal(gconstpointer pa, gconstpointer pb)
{
    const sheet_run_chunk *a = pa, *b = pb;
    return a->fp == b->fp && a->len == b->len &&
           memcmp(a->css, b->css, a->len) == 0;
}

static void
sheet_run_drop_repeats(sheet_collect_ctx *cc)
{
    guint n = cc->run_chunks->len;
    if (n < 2) return;
    GHashTable *later = g_hash_table_new(sheet_run_chunk_hash,
                                         sheet_run_chunk_equal);
    gboolean *drop = g_new0(gboolean, n);
    for (guint i = n; i-- > 0; ) {
        sheet_run_chunk *chunk = &g_array_index(cc->run_chunks,
                                                sheet_run_chunk, i);
        if (chunk->has_at) continue;
        if (g_hash_table_contains(later, chunk)) drop[i] = TRUE;
        else g_hash_table_add(later, chunk);
    }
    g_hash_table_destroy(later);
    for (guint i = n; i-- > 0; )
        if (drop[i]) g_array_remove_index(cc->run_chunks, i);
    g_free(drop);
}

static GArray *
sheet_run_chunks_new(void)
{
    GArray *chunks = g_array_new(FALSE, FALSE, sizeof(sheet_run_chunk));
    g_array_set_clear_func(chunks, sheet_run_chunk_clear);
    return chunks;
}

#define SHEET_RUN_CHUNK_ALONE 16384
#define SHEET_RUN_GROUP_MIN   (32 * 1024)
#define SHEET_RUN_GROUP_MAX   (128 * 1024)

typedef struct {
    const sheet_run_chunk *chunks;
    guint                  count;
    gboolean               alone;
} sheet_run_span;

static void
sheet_run_span_text(GString *out, gpointer data)
{
    const sheet_run_span *span = data;
    for (guint i = 0; i < span->count; i++) {
        g_string_append_len(out, span->chunks[i].css,
                            (gssize)span->chunks[i].len);
        if (!span->alone) g_string_append_c(out, '\n');
    }
}

static void
sheet_run_emit(sheet_collect_ctx *cc, const sheet_run_chunk *chunks,
               guint count, gboolean alone)
{
    if (count == 0) return;
    guint64 digest = alone ? 0x9e3779b97f4a7c15ULL : 1469598103934665603ULL;
    gsize len = 0;
    for (guint i = 0; i < count; i++) {
        digest = (digest ^ chunks[i].fp) * 1099511628211ULL;
        digest ^= (guint64)chunks[i].len * 0x100000001b3ULL;
        len += chunks[i].len + (alone ? 0 : 1);
    }
    sheet_run_span span = { chunks, count, alone };
    ns_css_stylesheet *sh = ns_css_merged_styles_cached_digest(
        digest, len, cc->run_base, sheet_run_span_text, &span);
    if (!sh) return;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    append_stylesheet_expanded(cc->out, sh, cc->run_base, cc->top_url,
                               cc->csp_url, seen, cc->cache, 0, FALSE,
                               cc->frame_depth > 0);
    g_hash_table_destroy(seen);
}

static void
sheet_run_flush(sheet_collect_ctx *cc)
{
    if (!cc->run_chunks || cc->run_chunks->len == 0) return;
    sheet_run_drop_repeats(cc);
    const sheet_run_chunk *all = &g_array_index(cc->run_chunks,
                                                sheet_run_chunk, 0);
    guint start = 0;
    gsize group_len = 0;
    for (guint i = 0; i < cc->run_chunks->len; i++) {
        if (all[i].len >= SHEET_RUN_CHUNK_ALONE) {
            sheet_run_emit(cc, all + start, i - start, FALSE);
            sheet_run_emit(cc, all + i, 1, TRUE);
            start = i + 1;
            group_len = 0;
            continue;
        }
        group_len += all[i].len;
        if ((group_len >= SHEET_RUN_GROUP_MIN && (all[i].fp & 3) == 0) ||
            group_len >= SHEET_RUN_GROUP_MAX) {
            sheet_run_emit(cc, all + start, i + 1 - start, FALSE);
            start = i + 1;
            group_len = 0;
        }
    }
    sheet_run_emit(cc, all + start, cc->run_chunks->len - start, FALSE);
    g_array_set_size(cc->run_chunks, 0);
    cc->run_base = NULL;
}

static void
sheet_docs_sync(sheet_collect_ctx *cc)
{
    sheet_run_flush(cc);
    if (!cc->docs) return;
    while (cc->docs->len < cc->out->len)
        g_ptr_array_add(cc->docs, cc->doc);
}

static double
frame_dimension_from_style_attr(const ns_node *frame, const char *prop,
                                double *out)
{
    const char *style = ns_element_get_attr(frame, "style");
    if (!style) return FALSE;
    gsize plen = strlen(prop);
    for (const char *p = style; (p = strstr(p, prop)) != NULL; p += plen) {
        if (p != style && (g_ascii_isalnum(p[-1]) || p[-1] == '-'))
            continue;
        const char *q = p + plen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        char *end = NULL;
        double v = g_ascii_strtod(q, &end);
        if (end && end != q && v >= 0 &&
            (g_ascii_strncasecmp(end, "px", 2) == 0 ||
             *end == ';' || *end == '\0' || *end == ' ')) {
            *out = v;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
frame_dimension_from_attr(const ns_node *frame, const char *attr, double *out)
{
    const char *av = ns_element_get_attr(frame, attr);
    if (!av || !*av) return FALSE;
    char *end = NULL;
    double v = g_ascii_strtod(av, &end);
    if (end == av || v < 0) return FALSE;
    *out = v;
    return TRUE;
}

typedef struct {
    double w, h;
} ns_collect_frame_vp;

static gboolean
css_has_viewport_media(const char *css)
{
    if (!css) return FALSE;
    for (const char *p = css; (p = strstr(p, "@media")) != NULL; p += 6) {
        const char *end = strchr(p, '{');
        if (!end) break;
        for (const char *q = p; q < end; q++)
            if (g_ascii_strncasecmp(q, "width", 5) == 0 ||
                g_ascii_strncasecmp(q, "height", 6) == 0 ||
                g_ascii_strncasecmp(q, "aspect-ratio", 12) == 0 ||
                g_ascii_strncasecmp(q, "orientation", 11) == 0)
                return TRUE;
    }
    return FALSE;
}

static GHashTable *g_collect_frame_vp;
static GHashTable *g_viewport_media_memo;

static gboolean
css_bytes_have_viewport_media(GBytes *bytes)
{
    if (!g_viewport_media_memo)
        g_viewport_media_memo =
            g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                  (GDestroyNotify)g_bytes_unref, NULL);
    gpointer memo = g_hash_table_lookup(g_viewport_media_memo, bytes);
    if (memo) return GPOINTER_TO_INT(memo) == 2;
    if (g_hash_table_size(g_viewport_media_memo) >= 256)
        g_hash_table_remove_all(g_viewport_media_memo);
    gsize len = 0;
    const char *data = g_bytes_get_data(bytes, &len);
    char *terminated = g_strndup(data, len);
    gboolean seen = css_has_viewport_media(terminated);
    g_free(terminated);
    g_hash_table_insert(g_viewport_media_memo, g_bytes_ref(bytes),
                        GINT_TO_POINTER(seen ? 2 : 1));
    return seen;
}

enum {
    STYLE_TEXT_VIEWPORT_MEDIA = 1,
    STYLE_TEXT_NEEDS_OWN_SHEET = 2,
    STYLE_TEXT_HAS_AT_RULE = 4,
};

static int
style_text_traits(const char *css, gsize len)
{
    int traits = 0;
    if (css_has_viewport_media(css)) traits |= STYLE_TEXT_VIEWPORT_MEDIA;
    if (strchr(css, '@')) traits |= STYLE_TEXT_HAS_AT_RULE;
    if (((traits & STYLE_TEXT_HAS_AT_RULE) && strstr(css, "@import")) ||
        !ns_css_syntax_is_self_contained(css, len))
        traits |= STYLE_TEXT_NEEDS_OWN_SHEET;
    return traits;
}

static void
frame_viewport_px(const ns_node *frame, double *w, double *h)
{
    gboolean have_w = frame_dimension_from_style_attr(frame, "width", w);
    gboolean have_h = frame_dimension_from_style_attr(frame, "height", h);
    if (!have_w || !have_h) {
        double lw = 0, lh = 0;
        if (ns_layout_frame_viewport(frame, &lw, &lh)) {
            if (!have_w) { *w = lw; have_w = TRUE; }
            if (!have_h) { *h = lh; have_h = TRUE; }
        }
    }
    if (!have_w && !frame_dimension_from_attr(frame, "width", w)) *w = 300;
    if (!have_h && !frame_dimension_from_attr(frame, "height", h)) *h = 150;
}

static void
frame_viewport_measured(const ns_node *frame, double *w, double *h)
{
    *w = 0;
    *h = 0;
    gboolean have_w = frame_dimension_from_style_attr(frame, "width", w);
    gboolean have_h = frame_dimension_from_style_attr(frame, "height", h);
    if (!have_w || !have_h) {
        double lw = 0, lh = 0;
        if (ns_layout_frame_viewport(frame, &lw, &lh)) {
            if (!have_w) { *w = lw; have_w = TRUE; }
            if (!have_h) { *h = lh; have_h = TRUE; }
        }
    }
    if (!have_w) have_w = frame_dimension_from_attr(frame, "width", w);
    if (!have_h) have_h = frame_dimension_from_attr(frame, "height", h);
    if (!have_w || !have_h) { *w = 0; *h = 0; }
}

static void
frame_viewport_record(const ns_node *frame, double w, double h)
{
    if (!g_collect_frame_vp) return;
    ns_collect_frame_vp *e = g_new0(ns_collect_frame_vp, 1);
    e->w = w;
    e->h = h;
    g_hash_table_insert(g_collect_frame_vp, (gpointer)frame, e);
}

static gboolean
frame_viewports_disagree_with_layout(void)
{
    if (!g_collect_frame_vp) return FALSE;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_collect_frame_vp);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const ns_collect_frame_vp *e = v;
        double lw = 0, lh = 0;
        if (!ns_layout_frame_viewport(k, &lw, &lh)) continue;
        if (fabs(e->w - lw) > 0.01 || fabs(e->h - lh) > 0.01) return TRUE;
    }
    return FALSE;
}

static gboolean
sheet_type_is_css(const char *type)
{
    if (!type || !*type) return TRUE;
    gsize n = strcspn(type, ";");
    while (n > 0 && g_ascii_isspace(type[n - 1])) n--;
    return n == 8 && g_ascii_strncasecmp(type, "text/css", 8) == 0;
}

static gboolean
style_sheet_enabled(const ns_node *style)
{
    return !(style->flags & NS_NODE_SHEET_DISABLED) &&
           sheet_type_is_css(ns_element_get_attr(style, "type"));
}

static gboolean
link_sheet_enabled(const ns_node *link)
{
    return !ns_element_get_attr(link, "disabled") &&
           sheet_type_is_css(ns_element_get_attr(link, "type"));
}

static void collect_stylesheets_walk(ns_node *n, const char *base_url,
                                     sheet_collect_ctx *cc, int depth);

static void
collect_frame_children(ns_node *frame, const char *base_url,
                       sheet_collect_ctx *cc, int depth)
{
    for (ns_node *c = frame->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_DOCUMENT) {
            collect_stylesheets_walk(c, base_url, cc, depth + 1);
            continue;
        }
        sheet_docs_sync(cc);
        ns_node *outer = cc->doc;
        cc->doc = c;
        collect_stylesheets_walk(c, base_url, cc, depth + 1);
        sheet_docs_sync(cc);
        cc->doc = outer;
    }
}

typedef struct {
    guint64             hash;
    gsize               len;
    char               *host_id;
    double              vw;
    double              vh;
    guint               generation;
    guint64             pass;
    gboolean            viewport_media;
    ns_css_stylesheet  *sheet;
} adopted_sheet_entry;

static GHashTable *g_adopted_sheets;
static guint64     g_adopted_pass;

static void
adopted_sheet_entry_free(gpointer data)
{
    adopted_sheet_entry *e = data;
    if (!e) return;
    if (e->sheet) {
        e->sheet->cached = FALSE;
        ns_css_stylesheet_free(e->sheet);
    }
    g_free(e->host_id);
    g_free(e);
}

typedef struct {
    gsize       len;
    const char *host_id;
    const char *base_url;
    double      vw;
    double      vh;
    guint       generation;
} adopted_sheet_key;

static adopted_sheet_key
adopted_sheet_key_now(gsize len, const char *host_id, const char *base_url)
{
    adopted_sheet_key key = {
        .len = len,
        .host_id = host_id,
        .base_url = base_url,
        .vw = ns_css_media_viewport_current_w(),
        .vh = ns_css_media_viewport_current_h(),
        .generation = ns_css_stylesheet_cache_generation(),
    };
    return key;
}

static gboolean
adopted_sheet_entry_matches(const adopted_sheet_entry *e,
                            const adopted_sheet_key *key)
{
    const char *resolved_base = e->sheet->resolved_base;
    return e->len == key->len && e->vw == key->vw && e->vh == key->vh &&
           e->generation == key->generation &&
           g_strcmp0(e->host_id, key->host_id) == 0 &&
           (!resolved_base || g_strcmp0(resolved_base, key->base_url) == 0);
}

static adopted_sheet_entry *
adopted_sheet_entry_new(guint64 hash, const adopted_sheet_key *key,
                        gboolean viewport_media, ns_css_stylesheet *sheet)
{
    sheet->cached = TRUE;
    adopted_sheet_entry *e = g_new0(adopted_sheet_entry, 1);
    e->hash = hash;
    e->len = key->len;
    e->host_id = g_strdup(key->host_id);
    e->vw = key->vw;
    e->vh = key->vh;
    e->generation = key->generation;
    e->pass = g_adopted_pass;
    e->viewport_media = viewport_media;
    e->sheet = sheet;
    return e;
}

static guint64
adopted_text_hash(const char *s, gsize n)
{
    guint64 h = 1469598103934665603ULL;
    gsize i = 0;
    for (; i + 8 <= n; i += 8) {
        guint64 w;
        memcpy(&w, s + i, 8);
        h = (h ^ w) * 1099511628211ULL;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ (guchar)s[i]) * 1099511628211ULL;
    return h ^ n;
}

static GHashTable *g_large_style_sheets;

static void
cached_sheets_trim(GHashTable *table)
{
    if (!table || g_hash_table_size(table) < 512) return;
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, table);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        adopted_sheet_entry *e = value;
        if (e->pass + 1 < g_adopted_pass) g_hash_table_iter_remove(&it);
    }
}

static void style_node_memos_trim(void);

static void
adopted_sheets_trim(void)
{
    cached_sheets_trim(g_adopted_sheets);
    cached_sheets_trim(g_large_style_sheets);
    style_node_memos_trim();
}

static void
style_text_fingerprint(const ns_node *n, guint64 *hash, gsize *len, int depth)
{
    if (depth >= 512) return;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_TEXT && c->text) {
            *hash = (*hash * 1099511628211ULL) ^
                    adopted_text_hash(c->text, c->text_len);
            *len += c->text_len;
        } else if (c->kind == NS_NODE_ELEMENT) {
            style_text_fingerprint(c, hash, len, depth + 1);
        }
    }
}

static const char *
style_shadow_host_id(const ns_node *style)
{
    for (const ns_node *a = style; a; a = a->parent)
        if (a->kind == NS_NODE_ELEMENT && a->parent &&
            ns_element_has_shadow_marker(a))
            return ns_element_get_attr(a->parent, NS_HOST_SCOPE_ATTR);
    return NULL;
}

typedef struct {
    guint64  text_hash;
    gsize    text_len;
    guint64  context;
    char    *css;
    gsize    css_len;
    guint64  css_hash;
    int      traits;
    guint64  pass;
} style_node_memo;

static GHashTable *g_style_node_memos;

static void
style_node_memo_free(gpointer data)
{
    style_node_memo *m = data;
    if (m->css) g_ref_string_release(m->css);
    g_free(m);
}

static guint64
style_node_context(const ns_node *style)
{
    const ns_node *root = style;
    while (root->parent) root = root->parent;
    guint64 context = (guint64)(guintptr)root * 0x9e3779b97f4a7c15ULL;
    const char *media = ns_element_get_attr(style, "media");
    if (media && *media) {
        context ^= adopted_text_hash(media, strlen(media));
        if (ns_css_media_query_matches(media)) context ^= 0x5bd1e995ULL;
    }
    const char *host = style_shadow_host_id(style);
    if (host) context ^= adopted_text_hash(host, strlen(host)) * 31;
    return context;
}

static const style_node_memo *
style_node_memo_get(ns_node *style, guint64 text_hash, gsize text_len)
{
    if (!g_style_node_memos)
        g_style_node_memos = g_hash_table_new_full(g_direct_hash,
                                                   g_direct_equal, NULL,
                                                   style_node_memo_free);
    guint64 context = style_node_context(style);
    style_node_memo *m = g_hash_table_lookup(g_style_node_memos, style);
    if (m && m->text_hash == text_hash && m->text_len == text_len &&
        m->context == context) {
        m->pass = g_adopted_pass;
        return m;
    }
    m = g_new0(style_node_memo, 1);
    m->text_hash = text_hash;
    m->text_len = text_len;
    m->context = context;
    m->pass = g_adopted_pass;
    char *css = ns_css_style_element_text(style);
    if (css) {
        m->css_len = strlen(css);
        m->css = g_ref_string_new_len(css, (gssize)m->css_len);
        m->css_hash = adopted_text_hash(css, m->css_len);
        m->traits = style_text_traits(css, m->css_len);
        g_free(css);
    }
    g_hash_table_replace(g_style_node_memos, style, m);
    return m;
}

static void
style_node_memos_trim(void)
{
    if (!g_style_node_memos || g_hash_table_size(g_style_node_memos) < 4096)
        return;
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, g_style_node_memos);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        style_node_memo *m = value;
        if (m->pass + 1 < g_adopted_pass) g_hash_table_iter_remove(&it);
    }
}

static adopted_sheet_entry *
large_style_entry(ns_node *style, guint64 hash, gsize len, const char *base_url)
{
    if (!g_large_style_sheets)
        g_large_style_sheets = g_hash_table_new_full(g_direct_hash,
                                                     g_direct_equal, NULL,
                                                     adopted_sheet_entry_free);
    adopted_sheet_key key = adopted_sheet_key_now(len,
                                                  style_shadow_host_id(style),
                                                  base_url);
    adopted_sheet_entry *e = g_hash_table_lookup(g_large_style_sheets, style);
    if (e && e->hash == hash && adopted_sheet_entry_matches(e, &key))
        return e;
    char *css = ns_css_style_element_text(style);
    if (!css) return NULL;
    ns_css_stylesheet *sheet = ns_css_stylesheet_parse_author(css, -1);
    gboolean media_dep = css_has_viewport_media(css);
    g_free(css);
    if (!sheet) return NULL;
    e = adopted_sheet_entry_new(hash, &key, media_dep, sheet);
    g_hash_table_replace(g_large_style_sheets, style, e);
    return e;
}

static gboolean
collect_large_style(ns_node *style, const char *base_url, sheet_collect_ctx *cc,
                    guint64 hash, gsize len)
{
    if (cc->frame_depth > 0) return FALSE;
    if (len < SHEET_RUN_CHUNK_ALONE) return FALSE;
    const char *media = ns_element_get_attr(style, "media");
    if (media && *media && !ns_css_media_query_matches(media)) return TRUE;
    adopted_sheet_entry *e = large_style_entry(style, hash, len, base_url);
    if (!e) return TRUE;
    e->pass = g_adopted_pass;
    if (e->viewport_media) cc->media_seen = TRUE;
    sheet_run_flush(cc);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    append_stylesheet_expanded(cc->out, e->sheet, base_url, cc->top_url,
                               cc->csp_url, seen, cc->cache, 0,
                               engine_node_in_head(style), FALSE);
    g_hash_table_destroy(seen);
    return TRUE;
}

static guint64
adopted_sheet_hash(const char *css, gsize len, const char *host_id)
{
    guint64 hash = adopted_text_hash(css, len);
    if (host_id)
        hash ^= adopted_text_hash(host_id, strlen(host_id)) * 31;
    return hash;
}

static ns_css_stylesheet *
adopted_sheet_parse(ns_node *root, const char *css, adopted_sheet_key *key,
                    gboolean *viewport_media)
{
    char *scoped = ns_css_shadow_adopted_css(root);
    if (!scoped) return NULL;
    ns_css_stylesheet *sheet = ns_css_stylesheet_parse_author(scoped, -1);
    gboolean media = css_has_viewport_media(scoped);
    g_free(scoped);
    if (!sheet) return NULL;
    *viewport_media = media;
    key->host_id = ns_element_get_attr(root->parent, NS_HOST_SCOPE_ATTR);
    if (!key->host_id) return sheet;
    adopted_sheet_entry *e = adopted_sheet_entry_new(
        adopted_sheet_hash(css, key->len, key->host_id), key, media, sheet);
    g_hash_table_replace(g_adopted_sheets, &e->hash, e);
    return sheet;
}

static ns_css_stylesheet *
adopted_sheet_for(ns_node *root, const char *base_url, gboolean *viewport_media)
{
    const char *css = ns_element_get_attr(root, NS_ADOPTED_CSS_ATTR);
    if (!css || !*css || !root->parent) return NULL;
    if (!g_adopted_sheets)
        g_adopted_sheets = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                                 NULL, adopted_sheet_entry_free);
    gsize len = strlen(css);
    const char *host_id = ns_element_get_attr(root->parent, NS_HOST_SCOPE_ATTR);
    adopted_sheet_key key = adopted_sheet_key_now(len, host_id, base_url);
    guint64 hash = adopted_sheet_hash(css, len, host_id);
    adopted_sheet_entry *e = g_hash_table_lookup(g_adopted_sheets, &hash);
    if (e && host_id && adopted_sheet_entry_matches(e, &key)) {
        e->pass = g_adopted_pass;
        *viewport_media = e->viewport_media;
        return e->sheet;
    }
    return adopted_sheet_parse(root, css, &key, viewport_media);
}

static void
collect_adopted_css(ns_node *root, const char *base_url, sheet_collect_ctx *cc)
{
    if (root->kind != NS_NODE_ELEMENT) return;
    gboolean viewport_media = FALSE;
    ns_css_stylesheet *sh = adopted_sheet_for(root, base_url, &viewport_media);
    if (!sh) return;
    if (viewport_media) cc->media_seen = TRUE;
    sheet_run_flush(cc);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    append_stylesheet_expanded(cc->out, sh, base_url, cc->top_url,
                               cc->csp_url, seen, cc->cache, 0, FALSE,
                               cc->frame_depth > 0);
    g_hash_table_destroy(seen);
}

static gboolean
collect_name_may_matter(const ns_node *n)
{
    if (n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    switch (n->name[0]) {
    case 'f': case 'i': case 'l': case 'n': case 'o': case 's':
        return TRUE;
    default:
        return FALSE;
    }
}

static void
collect_internal_attrs(const ns_node *n, gboolean *shadow_root,
                       gboolean *adopted)
{
    *shadow_root = FALSE;
    *adopted = FALSE;
    if (n->kind != NS_NODE_ELEMENT) return;
    for (const ns_attr *a = n->attrs; a; a = a->next) {
        if (!a->name || a->name[0] != 'd' ||
            strncmp(a->name, "data-nd-", 8) != 0)
            continue;
        if (strcmp(a->name, NS_SHADOW_ATTR) == 0) *shadow_root = TRUE;
        else if (strcmp(a->name, NS_ADOPTED_CSS_ATTR) == 0 && a->value &&
                 *a->value)
            *adopted = TRUE;
    }
}

static void
collect_stylesheets_walk(ns_node *n, const char *base_url,
                         sheet_collect_ctx *cc, int depth)
{
    if (!n || depth >= 512) return;
    gboolean named = collect_name_may_matter(n);
    if (named && ns_node_is_element_named(n, "noscript")) return;
    if (named && (ns_node_is_element_named(n, "iframe") ||
                  ns_node_is_element_named(n, "frame") ||
                  ns_node_is_element_named(n, "object"))) {
        sheet_run_flush(cc);
        const char *furl = ns_element_get_attr(n, "data-nd-frame-url");
        if (furl && *furl) base_url = furl;
        gboolean has_doc = FALSE;
        for (const ns_node *c = n->first_child; c; c = c->next_sibling)
            if (c->kind == NS_NODE_DOCUMENT) { has_doc = TRUE; break; }
        if (has_doc) {
            double fw, fh;
            frame_viewport_px(n, &fw, &fh);
            gboolean outer_media = cc->media_seen;
            cc->media_seen = FALSE;
            ns_css_media_viewport_push(fw, fh);
            const char *outer_csp_url = cc->csp_url;
            if (furl && *furl) cc->csp_url = furl;
            cc->frame_depth++;
            collect_frame_children(n, base_url, cc, depth);
            sheet_run_flush(cc);
            cc->frame_depth--;
            cc->csp_url = outer_csp_url;
            gboolean frame_media = cc->media_seen;
            ns_css_media_viewport_pop();
            cc->media_seen = outer_media || frame_media;
            if (frame_media) frame_viewport_record(n, fw, fh);
            return;
        }
    }
    GPtrArray *out = cc->out;
    GHashTable *cache = cc->cache;
    if (named && ns_node_is_element_named(n, "style") &&
        style_sheet_enabled(n)) {
        guint64 text_hash = 1469598103934665603ULL;
        gsize text_len = 0;
        style_text_fingerprint(n, &text_hash, &text_len, 0);
        const style_node_memo *memo =
            collect_large_style(n, base_url, cc, text_hash, text_len)
            ? NULL : style_node_memo_get(n, text_hash, text_len);
        if (memo && memo->css) {
            int traits = memo->traits;
            if (traits & STYLE_TEXT_VIEWPORT_MEDIA) cc->media_seen = TRUE;
            if (cc->run_base && cc->run_base != base_url)
                sheet_run_flush(cc);
            if (traits & STYLE_TEXT_NEEDS_OWN_SHEET) {
                sheet_run_flush(cc);
                ns_css_stylesheet *sh =
                    ns_css_stylesheet_from_style_element_cached(n);
                if (sh) {
                    GHashTable *seen =
                        g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
                    append_stylesheet_expanded(out, sh, base_url, cc->top_url,
                                               cc->csp_url,
                                               seen, cache, 0,
                                               engine_node_in_head(n),
                                               cc->frame_depth > 0);
                    g_hash_table_destroy(seen);
                }
            } else {
                sheet_run_append(cc, memo->css, memo->css_len, memo->css_hash,
                                 (traits & STYLE_TEXT_HAS_AT_RULE) != 0,
                                 base_url);
            }
        }
    } else if (named && ns_node_is_element_named(n, "link") && base_url) {
        sheet_run_flush(cc);
        const char *rel = ns_element_get_attr(n, "rel");
        const char *href = ns_element_get_attr(n, "href");
        const char *media = ns_element_get_attr(n, "media");
        if (href && *href && rel_is_stylesheet(rel) &&
            link_sheet_enabled(n) &&
            (!media || !*media || ns_css_media_query_matches(media))) {
            char *abs = ns_url_resolve(base_url, href);
            if (abs && !ns_net_csp_allows(cc->csp_url, NS_CSP_STYLE, abs,
                                          ns_element_get_attr(n, "nonce"),
                                          TRUE))
                g_clear_pointer(&abs, g_free);
            gboolean blocking = engine_node_in_head(n);
            GBytes *bytes = fetch_css_bytes(abs, cc->top_url, cache,
                                            cc->strict_css_mime, "link",
                                            blocking, cc->frame_depth > 0);
            const char *integrity = ns_element_get_attr(n, "integrity");
            if (bytes && !ns_security_sri_check(integrity,
                                                g_bytes_get_data(bytes, NULL),
                                                g_bytes_get_size(bytes))) {
                ns_debug_log_emit(NS_DLOG_WARN, "sri",
                                  "SRI mismatch: stylesheet %s", abs);
                g_clear_pointer(&bytes, g_bytes_unref);
            }
            if (bytes) {
                gsize len = 0;
                const char *data = g_bytes_get_data(bytes, &len);
                engine_remember_linked_css(abs, bytes);
                if (css_bytes_have_viewport_media(bytes)) cc->media_seen = TRUE;
                ns_css_stylesheet *sh =
                    ns_css_stylesheet_parse_url_cached(abs, data, (gssize)len);
                if (sh) {
                    GHashTable *seen =
                        g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
                    if (abs) g_hash_table_add(seen, g_strdup(abs));
                    append_stylesheet_expanded(out, sh, abs, cc->top_url,
                                               cc->csp_url, seen, cache, 0,
                                               blocking,
                                               cc->frame_depth > 0);
                    g_hash_table_destroy(seen);
                }
                g_bytes_unref(bytes);
            }
            g_free(abs);
        }
    }
    gboolean shadow_attr, adopted_attr;
    collect_internal_attrs(n, &shadow_attr, &adopted_attr);
    gboolean shadow_scope = cc->docs && shadow_attr &&
                            n->parent && n->parent->kind == NS_NODE_ELEMENT;
    ns_node *outer_doc = cc->doc;
    if (shadow_scope) {
        sheet_docs_sync(cc);
        cc->doc = n->parent;
    }
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        collect_stylesheets_walk(c, base_url, cc, depth + 1);
    if (adopted_attr) collect_adopted_css(n, base_url, cc);
    if (shadow_scope) {
        sheet_docs_sync(cc);
        cc->doc = outer_doc;
    }
}

static void
engine_collect_stylesheets(ns_node *doc, const char *page_url, ns_js *js,
                           GPtrArray *out, GPtrArray *out_docs,
                           GHashTable *css_cache)
{
    g_autofree char *base_url = ns_js_base_url_for_document(js, doc, page_url);
    if (!g_collect_frame_vp)
        g_collect_frame_vp = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                   NULL, g_free);
    else
        g_hash_table_remove_all(g_collect_frame_vp);
    g_adopted_pass++;
    adopted_sheets_trim();
    sheet_collect_ctx cc = {
        .out = out, .cache = css_cache,
        .run_chunks = sheet_run_chunks_new(),
        .run_base = NULL,
        .top_url = page_url,
        .csp_url = page_url,
        .strict_css_mime = doc && !(doc->flags & NS_NODE_QUIRKS),
        .docs = out_docs, .doc = doc,
    };
    collect_stylesheets_walk(doc, base_url ? base_url : page_url, &cc, 0);
    sheet_docs_sync(&cc);
    g_array_free(cc.run_chunks, TRUE);
    ns_css_style_element_cache_end();
}

void
ns_engine_collect_stylesheets(ns_node *doc, const char *page_url,
                              GPtrArray *out, GPtrArray *out_docs,
                              GHashTable *css_cache)
{
    engine_collect_stylesheets(doc, page_url, NULL, out, out_docs, css_cache);
}

static void
engine_pass_begin(ns_node *doc, const char *page_url, const char *base_url,
                  ns_js *js, GHashTable *css_cache,
                  GPtrArray **sheets, GPtrArray **sheet_docs)
{
    ns_css_set_frame_viewport_cb(frame_viewport_measured);
    ns_css_relayout_enter();
    ns_css_set_doc_base(base_url);
    ns_css_style_element_cache_begin();
    *sheets = g_ptr_array_new();
    *sheet_docs = g_ptr_array_new();
    engine_collect_stylesheets(doc, page_url, js, *sheets, *sheet_docs,
                               css_cache);
}

static void
engine_sheets_free(GPtrArray *sheets)
{
    for (guint i = 0; i < sheets->len; i++)
        ns_css_stylesheet_free(g_ptr_array_index(sheets, i));
    g_ptr_array_free(sheets, TRUE);
}

static void
engine_pass_end(GPtrArray *sheets, GPtrArray *sheet_docs)
{
    engine_sheets_free(sheets);
    g_ptr_array_free(sheet_docs, TRUE);
    ns_css_relayout_leave();
}

ns_js_navigation_timing
ns_engine_navigation_timing(const ns_response *resp)
{
    return (ns_js_navigation_timing){
        .origin_us = resp->request_start_us,
        .origin_real_ms = resp->request_start_real_ms,
        .domain_lookup_start_ms = 0,
        .domain_lookup_end_ms = resp->domain_lookup_ms,
        .connect_start_ms = resp->domain_lookup_ms,
        .connect_end_ms = resp->connect_ms,
        .secure_connection_start_ms = resp->connect_ms < resp->tls_ms
            ? resp->connect_ms : 0,
        .request_start_ms = resp->pretransfer_ms,
        .response_start_ms = resp->response_start_ms,
        .response_end_ms = resp->response_end_ms,
    };
}

GHashTable *
ns_engine_compute_cascade(ns_node *doc, const char *page_url,
                          GHashTable *css_cache, ns_anim *anim)
{
    g_autofree char *base_url = ns_js_base_url_for_document(NULL, doc, page_url);
    GPtrArray *page_sheets, *sheet_docs;
    engine_pass_begin(doc, page_url, base_url, NULL, css_cache, &page_sheets,
                      &sheet_docs);
    if (anim)
        for (guint i = 0; i < page_sheets->len; i++)
            ns_anim_load_from_stylesheet(anim, g_ptr_array_index(page_sheets, i));
    GHashTable *styles = ns_css_compute(doc,
        (const ns_css_stylesheet *const *)page_sheets->pdata,
        (const ns_node *const *)sheet_docs->pdata, page_sheets->len);
    engine_pass_end(page_sheets, sheet_docs);
    return styles;
}

GHashTable *
ns_engine_restyle(ns_node *doc, const char *page_url,
                  int viewport_width, double viewport_height,
                  ns_image_cache *images, ns_anim *anim,
                  ns_js *js, GHashTable *css_cache,
                  const ns_node *focused, const ns_node *hover)
{
    g_autofree char *base_url = ns_js_base_url_for_document(js, doc, page_url);
    ns_svg_external_note_base(doc, base_url);
    GPtrArray *sheets, *sheet_docs;
    engine_pass_begin(doc, page_url, base_url, js, css_cache, &sheets,
                      &sheet_docs);
    ns_render_ctx rc = {
        .doc             = doc,
        .sheets          = (const ns_css_stylesheet *const *)sheets->pdata,
        .sheet_docs      = (const ns_node *const *)sheet_docs->pdata,
        .n_sheets        = sheets->len,
        .viewport_width  = (double)viewport_width,
        .viewport_height = viewport_height > 0 ? viewport_height
                                               : (double)viewport_width * 0.75,
        .zoom            = 1.0,
        .images          = images,
        .base_url        = base_url,
        .anim            = anim,
        .js              = js,
        .focused_input   = focused,
        .hover_node      = hover,
    };
    gint64 t0 = g_get_monotonic_time();
    GHashTable *styles = ns_render_restyle(&rc);
    if (g_getenv("NS_PROFILE"))
        g_printerr("[profile] restyle vw=%d nodes=%u total=%.2fms\n",
                   viewport_width, styles ? g_hash_table_size(styles) : 0u,
                   (g_get_monotonic_time() - t0) / 1000.0);
    engine_pass_end(sheets, sheet_docs);
    return styles;
}

GHashTable *
ns_engine_relayout(ns_node *doc, const char *page_url,
                   int viewport_width, double viewport_height,
                   ns_image_cache *images, ns_anim *anim,
                   ns_js *js, GHashTable *css_cache,
                   const ns_node *focused, const ns_node *hover,
                   gsize caret_byte,
                   gsize sel_anchor_byte, ns_box **out_layout)
{
    g_autofree char *base_url = ns_js_base_url_for_document(js, doc, page_url);
    ns_svg_external_note_base(doc, base_url);
    GPtrArray *sheets, *sheet_docs;
    engine_pass_begin(doc, page_url, base_url, js, css_cache, &sheets,
                      &sheet_docs);

    ns_render_ctx rc = {
        .doc             = doc,
        .sheets          = (const ns_css_stylesheet *const *)sheets->pdata,
        .sheet_docs      = (const ns_node *const *)sheet_docs->pdata,
        .n_sheets        = sheets->len,
        .viewport_width  = (double)viewport_width,
        .viewport_height = viewport_height > 0 ? viewport_height
                                               : (double)viewport_width * 0.75,
        .zoom            = 1.0,
        .images          = images,
        .base_url        = base_url,
        .anim            = anim,
        .js              = js,
        .focused_input   = focused,
        .hover_node      = hover,
        .caret_byte      = caret_byte,
        .sel_anchor_byte = sel_anchor_byte,
    };
    static int profile_env = -1;
    if (profile_env < 0)
        profile_env = g_getenv("NS_PROFILE") != NULL ? 1 : 0;
    GHashTable *styles;
    gint64 relayout_t0 = g_get_monotonic_time();
    if (profile_env) {
        ns_render_profile prof;
        gint64 t0 = g_get_monotonic_time();
        styles = ns_render_relayout_profile(&rc, out_layout, &prof);
        gint64 total = g_get_monotonic_time() - t0;
        guint nstyles = styles ? g_hash_table_size(styles) : 0u;
        g_printerr("[profile] relayout vw=%d nodes=%u total=%.2fms "
                   "css=%.2f style=%.2f layout=%.2f",
                   viewport_width, nstyles, total / 1000.0,
                   prof.css1_us / 1000.0, prof.style1_us / 1000.0,
                   prof.layout1_us / 1000.0);
        if (prof.container_pass)
            g_printerr(" | containers=%u passes=%u cq_collect=%.2f css2=%.2f "
                       "style2=%.2f layout2=%.2f",
                       prof.containers, prof.container_passes,
                       prof.container_us / 1000.0,
                       prof.css2_us / 1000.0, prof.style2_us / 1000.0,
                       prof.layout2_us / 1000.0);
        g_printerr("\n");
    } else {
        styles = ns_render_relayout(&rc, out_layout);
    }
    if (frame_viewports_disagree_with_layout()) {
        if (js) {
            ns_js_set_layout_root(js, NULL);
            ns_js_set_style_table(js, NULL);
        }
        if (out_layout && *out_layout) {
            ns_box_free(*out_layout);
            *out_layout = NULL;
        }
        if (styles) g_hash_table_destroy(styles);
        for (guint i = 0; i < sheets->len; i++)
            ns_css_stylesheet_free(g_ptr_array_index(sheets, i));
        g_ptr_array_set_size(sheets, 0);
        g_ptr_array_set_size(sheet_docs, 0);
        ns_css_style_element_cache_begin();
        engine_collect_stylesheets(doc, page_url, js, sheets, sheet_docs,
                                   css_cache);
        rc.sheets     = (const ns_css_stylesheet *const *)sheets->pdata;
        rc.sheet_docs = (const ns_node *const *)sheet_docs->pdata;
        rc.n_sheets   = sheets->len;
        styles = ns_render_relayout(&rc, out_layout);
    }
    ns_engine_perf_add_relayout(g_get_monotonic_time() - relayout_t0);
    ns_debug_log_emit(NS_DLOG_RENDER, "relayout", "styles=%u vw=%d",
                      styles ? g_hash_table_size(styles) : 0u, viewport_width);

    engine_pass_end(sheets, sheet_docs);
    return styles;
}

void
ns_engine_load_keyframes(ns_anim *anim, ns_node *doc, const char *base_url,
                         GHashTable *css_cache)
{
    if (!anim) return;
    GPtrArray *sheets = g_ptr_array_new();
    ns_engine_collect_stylesheets(doc, base_url, sheets, NULL, css_cache);
    for (guint i = 0; i < sheets->len; i++) {
        const ns_css_stylesheet *sh = g_ptr_array_index(sheets, i);
        if (sh) ns_anim_load_from_stylesheet(anim, sh);
    }
    engine_sheets_free(sheets);
}

void
ns_engine_anim_observe(ns_anim *anim, GHashTable *styles, gint64 now_us)
{
    ns_anim_observe_all(anim, styles, now_us);
}

typedef gboolean (*engine_box_fn)(const ns_box *b, gpointer user);

static gboolean
engine_box_walk(const ns_box *b, engine_box_fn fn, gpointer user)
{
    if (!b) return FALSE;
    if (fn(b, user)) return TRUE;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        if (engine_box_walk(c, fn, user)) return TRUE;
    for (guint i = 0; b->inline_atomics && i < b->inline_atomics->len; i++)
        if (engine_box_walk(g_array_index(b->inline_atomics, ns_inline_atomic, i).box,
                            fn, user))
            return TRUE;
    return FALSE;
}

typedef struct {
    const ns_node *node;
    double        *pos;
    double        *max;
} engine_scroll_find;

static gboolean
engine_scroll_find_box(const ns_box *b, gpointer user)
{
    engine_scroll_find *f = user;
    if (b->dom != f->node) return FALSE;
    f->pos[0] = b->scroll_x;
    f->pos[1] = b->scroll_y;
    f->max[0] = b->scroll_max_x;
    f->max[1] = b->scroll_max_y;
    return TRUE;
}

static gboolean
engine_scroll_lookup(const ns_node *node, double pos[2], double max[2],
                     gpointer user)
{
    ns_box *const *layout = user;
    engine_scroll_find f = { node, pos, max };
    return engine_box_walk(*layout, engine_scroll_find_box, &f);
}

static gboolean
engine_scroll_note_box(const ns_box *b, gpointer user)
{
    ns_anim *anim = user;
    if (ns_anim_is_scroll_source(anim, b->dom))
        ns_anim_note_scroll(anim, b->dom, b->scroll_x, b->scroll_y,
                            b->scroll_max_x, b->scroll_max_y);
    return FALSE;
}

static void
engine_scroll_note_root(ns_anim *anim, const ns_box *layout, double vw,
                        double vh)
{
    double x = 0, y = 0;
    double width = layout->content_width;
    double bottom = ns_box_max_bottom(layout, layout->content_height);
    ns_box_hit_viewport(&x, &y);
    ns_anim_note_scroll(anim, NULL, x, y, MAX(width - vw, 0.0),
                        MAX(bottom - vh, 0.0));
}

void
ns_engine_anim_track_layout(ns_anim *anim, ns_box **layout)
{
    ns_anim_set_scroll_lookup(anim, engine_scroll_lookup, layout);
}

gboolean
ns_engine_anim_scroll_sync(ns_anim *anim, const ns_box *layout, double vw,
                           double vh, gboolean *out_layout)
{
    guint epoch = ns_box_scroll_epoch();
    if (!layout || !ns_anim_scroll_needs_sync(anim, epoch)) return FALSE;
    ns_anim_scroll_reset(anim, epoch);
    if (ns_anim_scroll_uses_root(anim))
        engine_scroll_note_root(anim, layout, vw, vh);
    if (ns_anim_scroll_has_sources(anim))
        engine_box_walk(layout, engine_scroll_note_box, anim);
    return ns_anim_scroll_resample(anim, out_layout);
}

typedef struct {
    GMainLoop      *loop;
    int             pending;
    ns_image_cache *cache;
} imgs_fetch_state;

typedef struct {
    imgs_fetch_state *st;
    char             *abs;
} img_fetch_item;

static void
on_image_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    img_fetch_item *it = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (resp && !resp->error && resp->body && resp->body->len > 0) {
        ns_image *img = ns_image_cache_insert_encoded(it->st->cache, it->abs,
                                                      resp->body->data,
                                                      resp->body->len);
        if (img && !img->final_url) {
            img->final_url = g_strdup(resp->final_url);
            img->cors_allow_origin = g_strdup(resp->cors_allow_origin);
        }
    }
    if (resp) ns_response_free(resp);
    g_clear_error(&err);
    if (--it->st->pending == 0)
        g_main_loop_quit(it->st->loop);
    g_free(it->abs);
    g_free(it);
}

#define NS_LAZY_IMAGE_MARGIN_PX 4000.0

static const char *
engine_node_frame_base(const ns_node *n, const char *dflt)
{
    for (const ns_node *p = n ? n->parent : NULL; p; p = p->parent) {
        if (ns_node_is_element_named(p, "iframe") ||
            ns_node_is_element_named(p, "frame") ||
            ns_node_is_element_named(p, "object")) {
            const char *fu = ns_element_get_attr(p, "data-nd-frame-url");
            if (fu && *fu) return fu;
        }
    }
    return dflt;
}

gboolean
ns_engine_frame_csp_allows(const ns_node *n, ns_csp_kind kind,
                           const char *abs_url)
{
    for (const ns_node *p = n ? n->parent : NULL; p; p = p->parent) {
        if (!ns_node_is_element_named(p, "iframe") &&
            !ns_node_is_element_named(p, "frame") &&
            !ns_node_is_element_named(p, "object"))
            continue;
        const char *header = ns_element_get_attr(p, "data-nd-frame-csp");
        if (!header || !*header) return TRUE;
        ns_csp *csp = ns_csp_parse(header);
        if (!csp) return TRUE;
        gboolean ok = ns_csp_allows(csp, kind, abs_url,
                                    ns_element_get_attr(p, "data-nd-frame-url"));
        ns_csp_free(csp);
        return ok;
    }
    return TRUE;
}

static gboolean
engine_image_is_lazy(const ns_box *box)
{
    if (!box || box->kind != NS_BOX_IMAGE || !box->dom) return FALSE;
    const char *l = ns_element_get_attr(box->dom, "loading");
    return l && g_ascii_strcasecmp(l, "lazy") == 0;
}

static GHashTable *
engine_collect_wanted_images(ns_box *root, const char *base_url,
                             ns_image_cache *cache, double scroll_y,
                             double viewport_h, gboolean *deferred_any)
{
    GPtrArray *imgs = g_ptr_array_new();
    ns_layout_collect_images(root, imgs);
    GHashTable *wanted = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    double lazy_limit = (viewport_h > 0.0)
        ? scroll_y + viewport_h + NS_LAZY_IMAGE_MARGIN_PX : G_MAXDOUBLE;
    for (guint i = 0; i < imgs->len; i++) {
        ns_box *box = g_ptr_array_index(imgs, i);
        if (!box->media) continue;
        if (box->y > lazy_limit && engine_image_is_lazy(box)) {
            if (deferred_any) *deferred_any = TRUE;
            continue;
        }
        GPtrArray *srcs = g_ptr_array_new();
        if (box->media->image_src)
            g_ptr_array_add(srcs, box->media->image_src);
        else if (box->media->bg_layer_srcs) {
            for (guint li = 0; li < box->media->bg_layer_srcs->len; li++) {
                char *lsrc = g_ptr_array_index(box->media->bg_layer_srcs, li);
                if (lsrc) g_ptr_array_add(srcs, lsrc);
            }
        } else if (box->media->bg_image_src)
            g_ptr_array_add(srcs, box->media->bg_image_src);
        if (box->media->marker_image_src)
            g_ptr_array_add(srcs, box->media->marker_image_src);
        if (box->media->border_image_src)
            g_ptr_array_add(srcs, box->media->border_image_src);
        if (box->media->mask_image_src)
            g_ptr_array_add(srcs, box->media->mask_image_src);
        for (guint ai = 0; box->attrs && ai < box->attrs->len; ai++) {
            const ns_inline_attr *a =
                &g_array_index(box->attrs, ns_inline_attr, ai);
            if (a->bg_image_src && a->bg_image_src != box->media->bg_image_src)
                g_ptr_array_add(srcs, (gpointer)a->bg_image_src);
        }
        const char *box_base = engine_node_frame_base(box->dom, base_url);
        for (guint si = 0; si < srcs->len; si++) {
            const char *src = g_ptr_array_index(srcs, si);
            if (g_str_has_prefix(src, "nd-inline-svg:")) continue;
            char *abs = ns_url_resolve(box_base, src);
            if (!abs) continue;
            if (box_base != base_url &&
                !ns_engine_frame_csp_allows(box->dom, NS_CSP_IMG, abs)) {
                g_free(abs);
                continue;
            }
            if (ns_image_cache_peek(cache, abs) ||
                g_hash_table_contains(wanted, abs)) {
                g_free(abs);
                continue;
            }
            g_hash_table_add(wanted, abs);
        }
        g_ptr_array_free(srcs, TRUE);
    }
    g_ptr_array_free(imgs, TRUE);
    return wanted;
}

struct ns_engine_img_session {
    int             refs;
    gboolean        dead;
    int             outstanding;
    ns_image_cache *cache;
    void          (*arrived_cb)(gpointer user_data);
    gpointer        user_data;
};

static void
img_session_unref(ns_engine_img_session *s)
{
    if (--s->refs == 0)
        g_free(s);
}

typedef struct img_async_item {
    ns_engine_img_session *session;
    char                  *abs;
} img_async_item;

static void
on_image_fetch_async_done(GObject *src, GAsyncResult *result,
                          gpointer user_data)
{
    (void)src;
    img_async_item *it = user_data;
    ns_engine_img_session *s = it->session;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (!s->dead && resp && !resp->error && resp->body &&
        resp->body->len > 0) {
        ns_image *img = ns_image_cache_insert_encoded(s->cache, it->abs,
                                                      resp->body->data,
                                                      resp->body->len);
        if (img && !img->final_url) {
            img->final_url = g_strdup(resp->final_url);
            img->cors_allow_origin = g_strdup(resp->cors_allow_origin);
        }
    }
    if (resp) ns_response_free(resp);
    g_clear_error(&err);
    if (s->outstanding > 0) s->outstanding--;
    if (!s->dead && s->arrived_cb)
        s->arrived_cb(s->user_data);
    img_session_unref(s);
    g_free(it->abs);
    g_free(it);
}

ns_engine_img_session *
ns_engine_fetch_images_start(ns_box *root, const char *base_url,
                             ns_image_cache *cache,
                             GHashTable *requested,
                             double scroll_y, double viewport_h,
                             gboolean *deferred_any,
                             void (*arrived_cb)(gpointer user_data),
                             gpointer user_data)
{
    if (!root || !base_url || !cache) return NULL;
    GHashTable *wanted = engine_collect_wanted_images(root, base_url, cache,
                                                      scroll_y, viewport_h,
                                                      deferred_any);
    if (requested) {
        GHashTableIter rit;
        gpointer rkey;
        g_hash_table_iter_init(&rit, wanted);
        while (g_hash_table_iter_next(&rit, &rkey, NULL))
            if (g_hash_table_contains(requested, rkey))
                g_hash_table_iter_remove(&rit);
        g_hash_table_iter_init(&rit, wanted);
        while (g_hash_table_iter_next(&rit, &rkey, NULL))
            g_hash_table_add(requested, g_strdup(rkey));
    }
    guint n = g_hash_table_size(wanted);
    if (n == 0) {
        g_hash_table_destroy(wanted);
        return NULL;
    }

    ns_engine_img_session *s = g_new0(ns_engine_img_session, 1);
    s->refs = 1;
    s->outstanding = (int)n;
    s->cache = cache;
    s->arrived_cb = arrived_cb;
    s->user_data = user_data;

    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, wanted);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        img_async_item *item = g_new0(img_async_item, 1);
        item->session = s;
        item->abs = g_strdup(key);
        s->refs++;
        ns_net_request_async(
            item->abs, base_url, "GET", NULL, 0, NULL,
            ns_net_accept_headers_for(NS_FETCH_DEST_IMAGE), NULL,
            on_image_fetch_async_done, item);
    }
    g_hash_table_destroy(wanted);
    return s;
}

int
ns_engine_img_session_outstanding(const ns_engine_img_session *s)
{
    return s ? s->outstanding : 0;
}

void
ns_engine_img_session_close(ns_engine_img_session *s)
{
    if (!s) return;
    s->dead = TRUE;
    s->arrived_cb = NULL;
    img_session_unref(s);
}

void
ns_engine_fetch_images(ns_box *root, const char *base_url,
                       ns_image_cache *cache)
{
    if (!root || !base_url || !cache) return;
    GHashTable *wanted = engine_collect_wanted_images(root, base_url, cache,
                                                      0.0, 0.0, NULL);

    guint n = g_hash_table_size(wanted);
    if (n == 0) {
        g_hash_table_destroy(wanted);
        return;
    }

    imgs_fetch_state st = {0};
    st.loop = g_main_loop_new(NULL, FALSE);
    st.pending = (int)n;
    st.cache = cache;

    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, wanted);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        img_fetch_item *item = g_new0(img_fetch_item, 1);
        item->st = &st;
        item->abs = g_strdup(key);
        ns_net_request_async(
            item->abs, base_url, "GET", NULL, 0, NULL,
            ns_net_accept_headers_for(NS_FETCH_DEST_IMAGE), NULL,
            on_image_fetch_done, item);
    }
    engine_blocking_wait(st.loop);
    g_main_loop_unref(st.loop);
    g_hash_table_destroy(wanted);
}

int
ns_engine_write_png(const ns_box *root, const char *path)
{
    if (!root || !path) return 2;
    const int kCairoMax = 30000;
    double cw = root->content_width;
    if (!(cw > 0)) cw = 1024;
    if (cw > kCairoMax) cw = kCairoMax;
    int w = (int)cw;
    double max_bottom = ns_box_max_bottom(root, root->content_height);
    if (!(max_bottom > 0)) max_bottom = 0;
    if (max_bottom > (double)kCairoMax) max_bottom = kCairoMax;
    int h = (int)max_bottom + 32;
    if (h <= 0) h = 768;
    if (w > kCairoMax) w = kCairoMax;
    if (h > kCairoMax) {
        fprintf(stderr,
            "engine: page is %d px tall; PNG capped at %d (cairo limit)\n",
            h, kCairoMax);
        h = kCairoMax;
    }
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        fprintf(stderr, "engine: failed to create PNG surface\n");
        return 2;
    }
    cairo_t *cr = cairo_create(surf);
    ns_paint(cr, root, NULL);
    cairo_destroy(cr);
    gint64 encode_t0 = g_get_monotonic_time();
    cairo_status_t st = cairo_surface_write_to_png(surf, path);
    g_engine_encode_us += g_get_monotonic_time() - encode_t0;
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "engine: PNG write failed: %s\n",
                cairo_status_to_string(st));
        return 2;
    }
    return 0;
}

static void
pdf_set_metadata(cairo_surface_t *surf)
{
    cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_CREATOR,
                                   "Nordstjernen Sol");
    time_t now = time(NULL);
    struct tm tm_utc;
#if defined(_WIN32)
    int have_tm = gmtime_s(&tm_utc, &now) == 0;
#else
    int have_tm = gmtime_r(&now, &tm_utc) != NULL;
#endif
    if (have_tm) {
        char iso[32];
        if (strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) > 0)
            cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_CREATE_DATE,
                                           iso);
    }
}

int
ns_engine_write_pdf(const ns_box *root, const char *path)
{
    if (!root || !path) return 2;
    double w = root->content_width > 0 ? root->content_width : 595.0;
    double h = root->content_height > 0 ? (root->content_height + 32) : 842.0;
    cairo_surface_t *surf = cairo_pdf_surface_create(path, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        fprintf(stderr, "engine: failed to create PDF surface\n");
        return 2;
    }
    pdf_set_metadata(surf);
    cairo_t *cr = cairo_create(surf);
    ns_paint(cr, root, NULL);
    cairo_show_page(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return 0;
}

#define NS_PRINT_PT_PER_PX (72.0 / 96.0)

int
ns_engine_write_pdf_paged(const ns_box *root, const char *path,
                          const ns_print_setup *setup)
{
    if (!root || !path || !setup) return 2;
    double page_h = setup->height - setup->margin_top - setup->margin_bottom;
    GArray *offsets = ns_print_page_offsets(root, page_h);
    cairo_surface_t *surf = cairo_pdf_surface_create(
        path, setup->width * NS_PRINT_PT_PER_PX,
        setup->height * NS_PRINT_PT_PER_PX);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        g_array_free(offsets, TRUE);
        fprintf(stderr, "engine: failed to create PDF surface\n");
        return 2;
    }
    pdf_set_metadata(surf);
    cairo_t *cr = cairo_create(surf);
    for (guint i = 0; i < offsets->len; i++) {
        double top = g_array_index(offsets, double, i);
        ns_print_draw_page(cr, root, setup, NS_PRINT_PT_PER_PX, top,
                           ns_print_page_bottom(offsets, i, page_h));
        cairo_show_page(cr);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    g_array_free(offsets, TRUE);
    return 0;
}

GPtrArray *
ns_engine_print_recordings(const ns_box *root, const ns_print_setup *setup)
{
    if (!root || !setup) return NULL;
    double page_h = setup->height - setup->margin_top - setup->margin_bottom;
    GArray *offsets = ns_print_page_offsets(root, page_h);
    GPtrArray *pages = g_ptr_array_new();
    cairo_rectangle_t extent = { 0, 0, setup->width, setup->height };
    for (guint i = 0; i < offsets->len; i++) {
        double top = g_array_index(offsets, double, i);
        cairo_surface_t *rec =
            cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, &extent);
        cairo_t *cr = cairo_create(rec);
        ns_print_draw_page(cr, root, setup, 1.0, top,
                           ns_print_page_bottom(offsets, i, page_h));
        cairo_destroy(cr);
        g_ptr_array_add(pages, rec);
    }
    g_array_free(offsets, TRUE);
    return pages;
}

void
ns_engine_dump_text(const ns_box *b, GString *out)
{
    if (!b) return;
    if (b->kind == NS_BOX_INLINE && b->text && *b->text) {
        g_string_append(out, b->text);
        g_string_append_c(out, '\n');
    } else if (b->kind == NS_BOX_IMAGE && b->dom) {
        const char *alt = ns_element_get_attr(b->dom, "alt");
        const char *src = b->media ? b->media->image_src : NULL;
        if (alt && *alt) g_string_append_printf(out, "[image: %s]\n", alt);
        else if (src)    g_string_append_printf(out, "[image: %s]\n", src);
        else             g_string_append(out, "[image]\n");
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        ns_engine_dump_text(c, out);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            ns_engine_dump_text(
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box, out);
}

void
ns_engine_dump_layout(const ns_box *b, int indent, GString *out)
{
    if (!b) return;
    for (int i = 0; i < indent; i++) g_string_append_c(out, ' ');
    g_string_append_printf(out, "%s @(%.0f,%.0f) %.0fx%.0f",
        ns_box_kind_name(b->kind), b->x, b->y,
        b->content_width, b->content_height);
    if (b->dom && b->dom->name) {
        const char *id = b->dom->kind == NS_NODE_ELEMENT
                       ? ns_element_id_attr(b->dom) : NULL;
        if (id && *id)
            g_string_append_printf(out, " <%s#%s>", b->dom->name, id);
        else
            g_string_append_printf(out, " <%s>", b->dom->name);
    }
    if (b->media && b->media->image_src)
        g_string_append_printf(out, " img=%s", b->media->image_src);
    if (b->text && *b->text) {
        gsize n = strlen(b->text);
        if (n > 40) {
            g_string_append_printf(out, " text=\"%.40s…\"", b->text);
        } else {
            g_string_append_printf(out, " text=\"%s\"", b->text);
        }
    }
    g_string_append_c(out, '\n');
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        ns_engine_dump_layout(c, indent + 2, out);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            ns_engine_dump_layout(
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box,
                indent + 2, out);
}

char *
ns_engine_suffix_before_ext(const char *path, const char *suffix)
{
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
    const char *back  = strrchr(path, '\\');
    if (back && (!slash || back > slash)) slash = back;
    const char *dot = strrchr(path, '.');
    if (dot && (!slash || dot > slash))
        return g_strdup_printf("%.*s%s%s",
                               (int)(dot - path), path, suffix, dot);
    return g_strconcat(path, suffix, NULL);
}
