/* Nordstjernen — minimal EventSource / Server-Sent Events client (libcurl).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "eventsource.h"
#include "csp.h"
#include "net.h"

#include <curl/curl.h>
#include <string.h>

#define NS_ES_MAX_LINE  (1 << 20)
#define NS_ES_MAX_EVENT (8 << 20)
#define NS_ES_MAX_PENDING 256

struct ns_es {
    char     *url;
    char     *origin;
    ns_csp   *csp;
    char     *last_event_id;
    gint64    reconnect_ms;
    GThread  *thread;
    GMutex    lock;
    volatile gint exit_requested;
    volatile gint detached;
    volatile gint pending;
    int       refcount;
    ns_es_callbacks cbs;
    gpointer  user_data;
};

static ns_es *
ns_es_ref(ns_es *es)
{
    g_atomic_int_inc(&es->refcount);
    return es;
}

static void
ns_es_destroy(ns_es *es)
{
    g_free(es->url);
    g_free(es->origin);
    if (es->csp) ns_csp_free(es->csp);
    g_free(es->last_event_id);
    g_mutex_clear(&es->lock);
    g_free(es);
}

static void
ns_es_unref(ns_es *es)
{
    if (g_atomic_int_dec_and_test(&es->refcount))
        ns_es_destroy(es);
}

typedef struct {
    ns_es   *es;
    void   (*invoke)(ns_es *, gpointer);
    gpointer payload;
    void   (*payload_free)(gpointer);
} ns_es_dispatch;

static gboolean
ns_es_dispatch_run(gpointer data)
{
    ns_es_dispatch *d = data;
    ns_es *es = d->es;
    if (!g_atomic_int_get(&es->detached) && es->cbs.busy &&
        es->cbs.busy(es->user_data)) {
        g_timeout_add(4, ns_es_dispatch_run, d);
        return G_SOURCE_REMOVE;
    }
    if (!g_atomic_int_get(&es->detached) && d->invoke)
        d->invoke(es, d->payload);
    if (d->payload && d->payload_free) d->payload_free(d->payload);
    g_atomic_int_add(&es->pending, -1);
    ns_es_unref(es);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static void
ns_es_post(ns_es *es, void (*invoke)(ns_es *, gpointer), gpointer payload,
           void (*payload_free)(gpointer))
{
    if (g_atomic_int_get(&es->detached)) {
        if (payload && payload_free) payload_free(payload);
        return;
    }
    if (g_atomic_int_get(&es->pending) >= NS_ES_MAX_PENDING) {
        if (payload && payload_free) payload_free(payload);
        return;
    }
    g_atomic_int_inc(&es->pending);
    ns_es_dispatch *d = g_new0(ns_es_dispatch, 1);
    d->es = ns_es_ref(es);
    d->invoke = invoke;
    d->payload = payload;
    d->payload_free = payload_free;
    g_idle_add(ns_es_dispatch_run, d);
}

typedef struct {
    char *event;
    char *data;
    char *last_id;
} ns_es_msg;

static void
ns_es_msg_free(gpointer p)
{
    ns_es_msg *m = p;
    g_free(m->event);
    g_free(m->data);
    g_free(m->last_id);
    g_free(m);
}

static void
ns_es_invoke_open(ns_es *es, gpointer payload)
{
    (void)payload;
    if (es->cbs.on_open) es->cbs.on_open(es->user_data);
}

static void
ns_es_invoke_message(ns_es *es, gpointer payload)
{
    ns_es_msg *m = payload;
    if (es->cbs.on_message)
        es->cbs.on_message(m->event, m->data, m->last_id, es->user_data);
}

static void
ns_es_invoke_error(ns_es *es, gpointer payload)
{
    if (es->cbs.on_error) es->cbs.on_error(GPOINTER_TO_INT(payload), es->user_data);
}

static void
ns_es_emit_error(ns_es *es, gboolean fatal)
{
    ns_es_post(es, ns_es_invoke_error, GINT_TO_POINTER(fatal ? 1 : 0), NULL);
}

typedef struct {
    ns_es      *es;
    GByteArray *line;
    gboolean    last_was_cr;
    GString    *data;
    char       *event_type;
    long        status;
    gboolean    opened;
    gboolean    fatal;
    gboolean    is_event_stream;
    char       *allow_origin;
    CURL       *curl;
} ns_es_parse;

static void
ns_es_dispatch_event(ns_es_parse *p)
{
    if (!p->data->len && !p->event_type) return;
    if (!p->data->len) {
        g_free(p->event_type);
        p->event_type = NULL;
        return;
    }
    gsize n = p->data->len;
    if (n && p->data->str[n - 1] == '\n')
        g_string_truncate(p->data, n - 1);
    ns_es_msg *m = g_new0(ns_es_msg, 1);
    m->event = p->event_type ? p->event_type : g_strdup("message");
    m->data = g_strdup(p->data->str);
    g_mutex_lock(&p->es->lock);
    m->last_id = g_strdup(p->es->last_event_id ? p->es->last_event_id : "");
    g_mutex_unlock(&p->es->lock);
    p->event_type = NULL;
    g_string_truncate(p->data, 0);
    ns_es_post(p->es, ns_es_invoke_message, m, ns_es_msg_free);
}

static void
ns_es_process_line(ns_es_parse *p, const char *line)
{
    if (!*line) { ns_es_dispatch_event(p); return; }
    if (line[0] == ':') return;
    const char *colon = strchr(line, ':');
    char *field, *value;
    if (colon) {
        field = g_strndup(line, (gsize)(colon - line));
        const char *v = colon + 1;
        if (*v == ' ') v++;
        value = g_strdup(v);
    } else {
        field = g_strdup(line);
        value = g_strdup("");
    }
    if (!strcmp(field, "data")) {
        if (p->data->len + strlen(value) + 1 > NS_ES_MAX_EVENT)
            p->fatal = TRUE;
        else {
            g_string_append(p->data, value);
            g_string_append_c(p->data, '\n');
        }
    } else if (!strcmp(field, "event")) {
        g_free(p->event_type);
        p->event_type = *value ? g_strdup(value) : NULL;
    } else if (!strcmp(field, "id")) {
        g_mutex_lock(&p->es->lock);
        g_free(p->es->last_event_id);
        p->es->last_event_id = g_strdup(value);
        g_mutex_unlock(&p->es->lock);
    } else if (!strcmp(field, "retry")) {
        gboolean digits = *value != '\0';
        for (const char *c = value; *c; c++)
            if (*c < '0' || *c > '9') { digits = FALSE; break; }
        if (digits) {
            gint64 ms = g_ascii_strtoll(value, NULL, 10);
            if (ms > 0) {
                g_mutex_lock(&p->es->lock);
                p->es->reconnect_ms = MIN(ms, 86400000);
                g_mutex_unlock(&p->es->lock);
            }
        }
    }
    g_free(field);
    g_free(value);
}

static void
ns_es_feed(ns_es_parse *p, const char *buf, gsize len)
{
    for (gsize i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n' && p->last_was_cr) { p->last_was_cr = FALSE; continue; }
        p->last_was_cr = FALSE;
        if (c == '\r' || c == '\n') {
            if (c == '\r') p->last_was_cr = TRUE;
            g_byte_array_append(p->line, (const guint8 *)"", 1);
            p->line->data[p->line->len - 1] = '\0';
            ns_es_process_line(p, (const char *)p->line->data);
            g_byte_array_set_size(p->line, 0);
        } else {
            if (p->line->len >= NS_ES_MAX_LINE) { p->fatal = TRUE; return; }
            g_byte_array_append(p->line, (const guint8 *)&c, 1);
        }
    }
}

static gboolean
ns_es_cors_allows(const ns_es_parse *p)
{
    char *final_url = NULL;
    curl_easy_getinfo(p->curl, CURLINFO_EFFECTIVE_URL, &final_url);
    const char *origin = p->es->origin;
    if (final_url && origin && ns_url_same_origin(origin, final_url))
        return TRUE;
    return p->allow_origin &&
           (strcmp(p->allow_origin, "*") == 0 ||
            (origin && strcmp(p->allow_origin, origin) == 0));
}

static gboolean
ns_es_csp_allows(const ns_es_parse *p)
{
    if (!p->es->csp) return TRUE;
    char *final_url = NULL;
    curl_easy_getinfo(p->curl, CURLINFO_EFFECTIVE_URL, &final_url);
    return final_url &&
           ns_csp_allows(p->es->csp, NS_CSP_CONNECT, final_url, p->es->origin);
}

static size_t
ns_es_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    ns_es_parse *p = userdata;
    if (size != 0 && nitems > G_MAXSIZE / size) return 0;
    size_t total = size * nitems;
    if (total >= 5 && !g_ascii_strncasecmp(buffer, "HTTP/", 5)) {
        long code = 0;
        const char *sp = memchr(buffer, ' ', total);
        if (sp) {
            char digits[8];
            size_t avail = total - (size_t)(sp + 1 - buffer);
            size_t n = 0;
            while (n < avail && n < sizeof digits - 1 &&
                   g_ascii_isdigit(sp[1 + n])) {
                digits[n] = sp[1 + n];
                n++;
            }
            digits[n] = '\0';
            code = strtol(digits, NULL, 10);
        }
        p->status = code;
        p->is_event_stream = FALSE;
        g_clear_pointer(&p->allow_origin, g_free);
    } else if (total >= 9 && p->status >= 300 && p->status < 400 &&
               p->es->csp &&
               !g_ascii_strncasecmp(buffer, "Location:", 9)) {
        char *cur = NULL;
        curl_easy_getinfo(p->curl, CURLINFO_EFFECTIVE_URL, &cur);
        g_autofree char *loc = g_strstrip(g_strndup(buffer + 9, total - 9));
        g_autofree char *target = cur ? ns_url_resolve(cur, loc) : NULL;
        if (!target ||
            !ns_csp_allows(p->es->csp, NS_CSP_CONNECT, target, p->es->origin)) {
            p->fatal = TRUE;
            return 0;
        }
    } else if (total >= 28 &&
               !g_ascii_strncasecmp(buffer, "Access-Control-Allow-Origin:", 28)) {
        g_free(p->allow_origin);
        p->allow_origin = g_strstrip(g_strndup(buffer + 28, total - 28));
    } else if (total >= 13 &&
               !g_ascii_strncasecmp(buffer, "Content-Type:", 13)) {
        if (g_strstr_len(buffer, total, "text/event-stream"))
            p->is_event_stream = TRUE;
    } else if (total <= 2) {
        if (!p->opened) {
            if (p->status == 200 && p->is_event_stream &&
                (!ns_es_cors_allows(p) || !ns_es_csp_allows(p))) {
                p->fatal = TRUE;
            } else if (p->status == 200 && p->is_event_stream) {
                p->opened = TRUE;
                ns_es_post(p->es, ns_es_invoke_open, NULL, NULL);
            } else if (p->status >= 200 &&
                       (p->status < 300 || p->status >= 400)) {
                p->fatal = TRUE;
            }
        }
    }
    return g_atomic_int_get(&p->es->exit_requested) ? 0 : total;
}

static size_t
ns_es_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    ns_es_parse *p = userdata;
    if (g_atomic_int_get(&p->es->exit_requested) || p->fatal) return 0;
    if (size != 0 && nmemb > G_MAXSIZE / size) return 0;
    size_t total = size * nmemb;
    ns_es_feed(p, ptr, total);
    return total;
}

static int
ns_es_progress(void *clientp, curl_off_t dt, curl_off_t dn, curl_off_t ut,
               curl_off_t un)
{
    (void)dt; (void)dn; (void)ut; (void)un;
    const ns_es *es = clientp;
    return g_atomic_int_get(&es->exit_requested) ? 1 : 0;
}

static gboolean
ns_es_connect_once(ns_es *es, gboolean *opened_out)
{
    CURL *curl = curl_easy_init();
    if (!curl) return FALSE;

    ns_es_parse p = {0};
    p.es = es;
    p.line = g_byte_array_new();
    p.data = g_string_new(NULL);
    p.status = 0;
    p.curl = curl;

    curl_easy_setopt(curl, CURLOPT_URL, es->url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, NS_USER_AGENT);
    ns_net_curl_apply_cookies(curl, es->url, es->origin);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    ns_net_apply_curl_tls(curl);
    ns_net_apply_curl_proxy(curl, es->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                     g_str_has_prefix(es->url, "https://") ? "https"
                                                            : "http,https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ns_es_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &p);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ns_es_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &p);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ns_es_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, es);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    hdrs = curl_slist_append(hdrs, "Cache-Control: no-cache");
    if (es->origin && *es->origin) {
        char *h = g_strconcat("Origin: ", es->origin, NULL);
        hdrs = curl_slist_append(hdrs, h);
        g_free(h);
    }
    g_mutex_lock(&es->lock);
    if (es->last_event_id && *es->last_event_id) {
        char *h = g_strconcat("Last-Event-ID: ", es->last_event_id, NULL);
        hdrs = curl_slist_append(hdrs, h);
        g_free(h);
    }
    g_mutex_unlock(&es->lock);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    curl_easy_perform(curl);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    *opened_out = p.opened;
    gboolean fatal = p.fatal;
    g_byte_array_free(p.line, TRUE);
    g_string_free(p.data, TRUE);
    g_free(p.event_type);
    g_free(p.allow_origin);
    return !fatal;
}

static gpointer
ns_es_worker(gpointer data)
{
    ns_es *es = data;
    while (!g_atomic_int_get(&es->exit_requested)) {
        gboolean opened = FALSE;
        gboolean retriable = ns_es_connect_once(es, &opened);
        if (g_atomic_int_get(&es->exit_requested)) break;
        if (!retriable) {
            ns_es_emit_error(es, TRUE);
            break;
        }
        ns_es_emit_error(es, FALSE);
        g_mutex_lock(&es->lock);
        gint64 wait_ms = es->reconnect_ms;
        g_mutex_unlock(&es->lock);
        if (wait_ms <= 0) wait_ms = 3000;
        gint64 waited = 0;
        while (waited < wait_ms && !g_atomic_int_get(&es->exit_requested)) {
            g_usleep(50 * 1000);
            waited += 50;
        }
    }
    ns_es_unref(es);
    return NULL;
}

ns_es *
ns_es_new(const char *url, const char *origin, const char *last_event_id,
          const char *const *csp_headers,
          const ns_es_callbacks *cbs, gpointer user_data)
{
    ns_es *es = g_new0(ns_es, 1);
    es->url = g_strdup(url);
    es->origin = g_strdup(origin);
    for (const char *const *h = csp_headers; h && *h; h++) {
        ns_csp *parsed = ns_csp_parse(*h);
        if (!parsed) continue;
        if (es->csp) {
            ns_csp_merge(es->csp, parsed);
            ns_csp_free(parsed);
        } else {
            es->csp = parsed;
        }
    }
    es->last_event_id = g_strdup(last_event_id ? last_event_id : "");
    es->reconnect_ms = 3000;
    es->refcount = 1;
    if (cbs) es->cbs = *cbs;
    es->user_data = user_data;
    g_mutex_init(&es->lock);
    ns_es_ref(es);
    es->thread = g_thread_new("nd-eventsource", ns_es_worker, es);
    return es;
}

void
ns_es_close(ns_es *es)
{
    if (!es) return;
    g_atomic_int_set(&es->exit_requested, 1);
}

void
ns_es_free(ns_es *es)
{
    if (!es) return;
    g_atomic_int_set(&es->detached, 1);
    g_atomic_int_set(&es->exit_requested, 1);
    if (es->thread) {
        g_thread_join(es->thread);
        es->thread = NULL;
    }
    ns_es_unref(es);
}
