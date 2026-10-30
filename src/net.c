/* Nordstjernen — libcurl-backed async fetcher.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "net.h"
#include "about_style.h"
#include "cache.h"
#include "config.h"
#include "content_decode.h"
#include "history.h"
#include "csp.h"
#include "debuglog.h"
#include "ext.h"
#include "js.h"
#include "html.h"
#include "image.h"
#include "safebrowsing.h"
#include "security.h"
#include "about_logo_gif.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <glib/gstdio.h>
#include <gmodule.h>

#include <lexbor/unicode/idna.h>
#include <lexbor/url/url.h>

#include <libpsl.h>
#include <sqlite3.h>
#include <cairo.h>
#include "ns_pango.h"
#include <webp/decode.h>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#ifdef NS_HAVE_LIBAV
#include <libavutil/avutil.h>
#endif

#include <fcntl.h>
#ifdef G_OS_WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#endif

struct ns_header_ctx;
struct ns_net_stream;

typedef struct ns_write_ctx {
    GByteArray *body;
    guint64     total;
    guint64     budget;
    guint64     next_recheck;
    gboolean    exceeded;
    const struct ns_net_stream *stream;
    const struct ns_header_ctx *stream_headers;
    ns_content_decoder *decoder;
    guint       decoder_seq;
    gboolean    decode_failed;
    GByteArray *decoded;
} ns_write_ctx;

typedef struct ns_header_ctx {
    char   **content_type_out;
    char   **content_disposition_out;
    char   **csp_out;
    char   **xframe_options_out;
    char   **x_content_type_options_out;
    char   **cors_allow_origin_out;
    char   **refresh_out;
    char   **content_language_out;
    char    *etag;
    char    *last_modified;
    char    *cache_control;
    char    *vary;
    char    *expires;
    char    *date;
    char    *age;
    char    *location;
    char    *content_encoding;
    guint    response_seq;
    GString *raw;
    long     status;
    gboolean http1;
    gboolean set_cookie_seen;
    CURL    *cookie_curl;
    const char *cookie_jar_path;
    const char *url;
    gboolean   early_hints;
    GPtrArray *early_links;
} ns_header_ctx;

typedef struct ns_hop_req {
    const char        *url;
    const char        *method;
    const void        *body;
    gsize              body_len;
    struct curl_slist *headers;
    const char        *user_agent;
    const char        *referer;
    int                referer_policy;
    const char        *accept_encoding;
    long               timeout_s;
    long               connect_timeout_s;
    const char        *proxy;
    const char        *no_proxy;
    const char        *cookie_jar_path;
    const char        *cookie_js_path;
    int                samesite_cross;
    const char        *cookie_site;
    gboolean           cookie_navigation;
    gboolean           top_level_navigation;
    int                priority_urgency;
    gboolean           priority_incremental;
    gboolean           follow_redirects;
    long               max_redirs;
    gboolean           request_ftp;
    gboolean           initial_https;
    long               http_version_pref;
} ns_hop_req;

typedef struct ns_hop_out {
    long     status;
    char    *effective_url;
    char    *remote_ip;
    long     http_version;
    long     num_connects;
    double   t_namelookup_ms;
    double   t_connect_ms;
    double   t_appconnect_ms;
    double   t_pretransfer_ms;
    double   t_starttransfer_ms;
    double   t_total_ms;
    gboolean ok;
    gboolean cancelled;
    gboolean tls_verify_failed;
    gboolean connect_failed;
    char    *tls_warning;
    char    *error_message;
} ns_hop_out;

static char *g_cookie_dir;
static char *g_private_root;
static char *g_hsts_curl_path;
static char *g_altsvc_path;

typedef struct ns_hsts_entry {
    gint64   expires;
    gboolean subdomains;
} ns_hsts_entry;

static GHashTable *g_hsts_cache;
static gint64      g_hsts_cache_mtime_us;
static GMutex      g_hsts_lock;
static char *g_ca_bundle;
static const char *g_ec_curves = "X25519:P-256:P-384";
static char *g_accept_encoding;
static char *g_proxy_override;
static CURLSH *g_share;
static GMutex g_fetch_throttle_mutex;
static GCond  g_fetch_idle_cond;
static int    g_fetch_active;
static int    g_preconnect_active;
static gint   g_net_aborting;
static GQueue g_fetch_queue = G_QUEUE_INIT;
static GMutex g_share_locks[CURL_LOCK_DATA_LAST];
static GMutex      g_conn_stats_lock;
static GHashTable *g_conn_stats;

#define NS_DEAD_HOST_TTL_US ((gint64)120 * G_USEC_PER_SEC)
static GHashTable *g_dead_hosts;
static GMutex      g_dead_hosts_lock;

static char *ns_url_origin_from_any(const char *url);

static gboolean
ns_net_host_recently_dead(const char *host)
{
    gboolean dead = FALSE;
    g_mutex_lock(&g_dead_hosts_lock);
    if (g_dead_hosts) {
        gint64 *expiry = g_hash_table_lookup(g_dead_hosts, host);
        if (expiry) {
            if (g_get_monotonic_time() < *expiry)
                dead = TRUE;
            else
                g_hash_table_remove(g_dead_hosts, host);
        }
    }
    g_mutex_unlock(&g_dead_hosts_lock);
    return dead;
}

static void
ns_net_host_mark_dead(const char *host)
{
    g_mutex_lock(&g_dead_hosts_lock);
    if (!g_dead_hosts)
        g_dead_hosts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
    gint64 *expiry = g_new(gint64, 1);
    *expiry = g_get_monotonic_time() + NS_DEAD_HOST_TTL_US;
    g_hash_table_replace(g_dead_hosts, g_strdup(host), expiry);
    g_mutex_unlock(&g_dead_hosts_lock);
}

static void
ns_net_host_mark_alive(const char *host)
{
    g_mutex_lock(&g_dead_hosts_lock);
    if (g_dead_hosts)
        g_hash_table_remove(g_dead_hosts, host);
    g_mutex_unlock(&g_dead_hosts_lock);
}

#define NS_NET_MAX_PER_ORIGIN 6

typedef struct ns_origin_slot {
    int   in_use;
    GCond cond;
} ns_origin_slot;

static GMutex      g_origin_slots_lock;
static GHashTable *g_origin_slots;

static void
ns_origin_slot_free(gpointer p)
{
    ns_origin_slot *s = p;
    g_cond_clear(&s->cond);
    g_free(s);
}

static char *
origin_slot_key(const char *origin)
{
    return (origin && *origin) ? g_ascii_strdown(origin, -1) : NULL;
}

static gboolean
ns_net_acquire_origin_slot(const char *origin, GCancellable *cancellable)
{
    char *key = origin_slot_key(origin);
    if (!key) return FALSE;
    g_mutex_lock(&g_origin_slots_lock);
    if (!g_origin_slots)
        g_origin_slots = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, ns_origin_slot_free);
    ns_origin_slot *s = g_hash_table_lookup(g_origin_slots, key);
    if (!s) {
        s = g_new0(ns_origin_slot, 1);
        g_cond_init(&s->cond);
        g_hash_table_insert(g_origin_slots, key, s);
        key = NULL;
    }
    while (s->in_use >= NS_NET_MAX_PER_ORIGIN) {
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            g_mutex_unlock(&g_origin_slots_lock);
            g_free(key);
            return FALSE;
        }
        gint64 wakeup = g_get_monotonic_time() + 250 * G_TIME_SPAN_MILLISECOND;
        g_cond_wait_until(&s->cond, &g_origin_slots_lock, wakeup);
    }
    s->in_use++;
    g_mutex_unlock(&g_origin_slots_lock);
    g_free(key);
    return TRUE;
}

static void
ns_net_release_origin_slot(const char *origin)
{
    char *key = origin_slot_key(origin);
    if (!key) return;
    g_mutex_lock(&g_origin_slots_lock);
    if (g_origin_slots) {
        ns_origin_slot *s = g_hash_table_lookup(g_origin_slots, key);
        if (s && s->in_use > 0) {
            s->in_use--;
            g_cond_signal(&s->cond);
        }
    }
    g_mutex_unlock(&g_origin_slots_lock);
    g_free(key);
}

typedef struct ns_multi_xfer {
    CURL     *easy;
    GCond     cond;
    gboolean  done;
    CURLcode  result;
} ns_multi_xfer;

static CURLM      *g_multi;
static GThread    *g_multi_thread;
static gboolean    g_multi_quit;
static GMutex      g_multi_lock;
static GQueue      g_multi_incoming = G_QUEUE_INIT;
static GHashTable *g_multi_active;

static void
ns_net_multi_finish_locked(ns_multi_xfer *x, CURLcode result)
{
    x->result = result;
    x->done = TRUE;
    g_cond_signal(&x->cond);
}

static gpointer
ns_net_multi_loop(gpointer data)
{
    (void)data;
    for (;;) {
        g_mutex_lock(&g_multi_lock);
        if (g_multi_quit) {
            GHashTableIter it;
            gpointer key, val;
            g_hash_table_iter_init(&it, g_multi_active);
            while (g_hash_table_iter_next(&it, &key, &val)) {
                ns_multi_xfer *x = val;
                curl_multi_remove_handle(g_multi, x->easy);
                ns_net_multi_finish_locked(x, CURLE_ABORTED_BY_CALLBACK);
            }
            g_hash_table_remove_all(g_multi_active);
            for (ns_multi_xfer *x; (x = g_queue_pop_head(&g_multi_incoming)); )
                ns_net_multi_finish_locked(x, CURLE_ABORTED_BY_CALLBACK);
            g_mutex_unlock(&g_multi_lock);
            break;
        }
        for (ns_multi_xfer *x; (x = g_queue_pop_head(&g_multi_incoming)); ) {
            if (curl_multi_add_handle(g_multi, x->easy) == CURLM_OK)
                g_hash_table_insert(g_multi_active, x->easy, x);
            else
                ns_net_multi_finish_locked(x, CURLE_FAILED_INIT);
        }
        g_mutex_unlock(&g_multi_lock);

        int running = 0;
        curl_multi_perform(g_multi, &running);

        int nmsgs = 0;
        CURLMsg *m;
        while ((m = curl_multi_info_read(g_multi, &nmsgs))) {
            if (m->msg != CURLMSG_DONE) continue;
            CURL *easy = m->easy_handle;
            CURLcode res = m->data.result;
            curl_multi_remove_handle(g_multi, easy);
            g_mutex_lock(&g_multi_lock);
            ns_multi_xfer *x = g_hash_table_lookup(g_multi_active, easy);
            if (x) {
                g_hash_table_remove(g_multi_active, easy);
                ns_net_multi_finish_locked(x, res);
            }
            g_mutex_unlock(&g_multi_lock);
        }

        long timeo = -1;
        curl_multi_timeout(g_multi, &timeo);
        int wait_ms = (timeo < 0 || timeo > 1000) ? 1000 : (int)timeo;
        curl_multi_poll(g_multi, NULL, 0, wait_ms, NULL);
    }
    return NULL;
}

static void
ns_net_multi_start(void)
{
    g_mutex_lock(&g_multi_lock);
    if (!g_multi_thread) {
        g_multi = curl_multi_init();
        if (g_multi) {
            curl_multi_setopt(g_multi, CURLMOPT_PIPELINING,
                              (long)CURLPIPE_MULTIPLEX);
            g_multi_active = g_hash_table_new(g_direct_hash, g_direct_equal);
            g_multi_thread = g_thread_new("ns-net-multi",
                                          ns_net_multi_loop, NULL);
        }
    }
    g_mutex_unlock(&g_multi_lock);
}

static CURLcode
ns_net_multi_perform(CURL *easy, GCancellable *cancellable)
{
    (void)cancellable;
    ns_net_multi_start();
    if (!g_multi) return curl_easy_perform(easy);

    ns_multi_xfer x = { .easy = easy, .done = FALSE, .result = CURLE_OK };
    g_cond_init(&x.cond);

    g_mutex_lock(&g_multi_lock);
    if (g_multi_quit) {
        g_mutex_unlock(&g_multi_lock);
        g_cond_clear(&x.cond);
        return curl_easy_perform(easy);
    }
    g_queue_push_tail(&g_multi_incoming, &x);
    curl_multi_wakeup(g_multi);
    while (!x.done) {
        gint64 wakeup = g_get_monotonic_time() + 250 * G_TIME_SPAN_MILLISECOND;
        g_cond_wait_until(&x.cond, &g_multi_lock, wakeup);
    }
    g_mutex_unlock(&g_multi_lock);

    g_cond_clear(&x.cond);
    return x.result;
}

static void
ns_net_multi_shutdown(void)
{
    g_mutex_lock(&g_multi_lock);
    GThread *t = g_multi_thread;
    if (t) {
        g_multi_quit = TRUE;
        curl_multi_wakeup(g_multi);
    }
    g_mutex_unlock(&g_multi_lock);
    if (t) {
        g_thread_join(t);
        g_multi_thread = NULL;
    }
    if (g_multi) { curl_multi_cleanup(g_multi); g_multi = NULL; }
    if (g_multi_active) {
        g_hash_table_destroy(g_multi_active);
        g_multi_active = NULL;
    }
    g_multi_quit = FALSE;
}

static char *
ns_net_data_path(char **slot, const char *basename)
{
    if (*slot) return *slot;
    char *dir = g_build_filename(g_get_user_data_dir(), NS_APP_DIR_NAME, NULL);
    g_mkdir_with_parents(dir, 0700);
    *slot = g_build_filename(dir, basename, NULL);
    g_free(dir);
    return *slot;
}

static void
ns_rmrf(const char *path)
{
    if (!path) return;
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            char *child = g_build_filename(path, name, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
                !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
                ns_rmrf(child);
            else
                g_unlink(child);
            g_free(child);
        }
        g_dir_close(dir);
    }
    g_rmdir(path);
}

static const char *
ns_net_private_root(void)
{
    if (g_private_root) return g_private_root;
    g_private_root = g_dir_make_tmp("nordstjernen-private-XXXXXX", NULL);
    if (!g_private_root) {
        char *base = g_strdup_printf("nordstjernen-private-%u", g_random_int());
        g_private_root = g_build_filename(g_get_tmp_dir(), base, NULL);
        g_free(base);
        g_mkdir_with_parents(g_private_root, 0700);
    }
    g_chmod(g_private_root, 0700);
    return g_private_root;
}

static gboolean
ns_net_is_private(void)
{
    const ns_config *cfg = ns_config_get();
    return cfg && cfg->private_mode;
}

static char *
ns_net_hsts_curl_path(void)
{
    if (g_hsts_curl_path) return g_hsts_curl_path;
    if (!ns_net_is_private())
        return ns_net_data_path(&g_hsts_curl_path, "hsts-curl.txt");
    const char *root = ns_net_private_root();
    if (!root) return NULL;
    g_hsts_curl_path = g_build_filename(root, "hsts-curl.txt", NULL);
    char *real_dir = g_build_filename(g_get_user_data_dir(), NS_APP_DIR_NAME,
                                      NULL);
    char *real = g_build_filename(real_dir, "hsts-curl.txt", NULL);
    char *contents = NULL;
    gsize len = 0;
    if (g_file_get_contents(real, &contents, &len, NULL))
        g_file_set_contents_full(g_hsts_curl_path, contents, (gssize)len,
                                 G_FILE_SET_CONTENTS_CONSISTENT |
                                 G_FILE_SET_CONTENTS_ONLY_EXISTING, 0600, NULL);
    g_free(contents);
    g_free(real);
    g_free(real_dir);
    return g_hsts_curl_path;
}

static gint64
ns_hsts_parse_expiry(const char *stamp)
{
    while (*stamp == ' ' || *stamp == '"') stamp++;
    int y, mo, d, h, mi, sec;
    if (sscanf(stamp, "%4d%2d%2d %2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6)
        return G_MAXINT64;
    g_autoptr(GDateTime) when = g_date_time_new_utc(y, mo, d, h, mi, sec);
    return when ? g_date_time_to_unix(when) : G_MAXINT64;
}

static void
ns_hsts_cache_reload_locked(const char *path)
{
    if (!g_hsts_cache)
        g_hsts_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
    else
        g_hash_table_remove_all(g_hsts_cache);

    char *content = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(path, &content, &len, &err)) {
        if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning("hsts: failed to read %s: %s", path, err->message);
        g_clear_error(&err);
        return;
    }

    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    char **lines = g_strsplit(content, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (!*line || *line == '#') continue;
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        gboolean subs = (*line == '.');
        const char *host = subs ? line + 1 : line;
        if (!*host) continue;
        gint64 expires = ns_hsts_parse_expiry(sp + 1);
        if (expires <= now) continue;
        ns_hsts_entry *e = g_new(ns_hsts_entry, 1);
        e->expires = expires;
        e->subdomains = subs;
        g_hash_table_replace(g_hsts_cache, g_ascii_strdown(host, -1), e);
    }
    g_strfreev(lines);
    g_free(content);
}

static gint64
file_mtime_us(const char *path)
{
    GStatBuf st;
    if (g_stat(path, &st) != 0) return 0;
    return (gint64)st.st_mtime * G_USEC_PER_SEC;
}

static gboolean
ns_hsts_lookup_locked(const char *lower_host)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    const ns_hsts_entry *e = g_hash_table_lookup(g_hsts_cache, lower_host);
    if (e && e->expires > now) return TRUE;
    const char *dot = lower_host;
    while ((dot = strchr(dot, '.')) != NULL) {
        const char *parent = dot + 1;
        e = g_hash_table_lookup(g_hsts_cache, parent);
        if (e && e->subdomains && e->expires > now) return TRUE;
        dot = parent;
    }
    return FALSE;
}

gboolean
ns_url_is_http_or_https(const char *url)
{
    return url && (g_str_has_prefix(url, "http://") ||
                   g_str_has_prefix(url, "https://"));
}

static gboolean
ns_query_param_is_tracking(const char *key, size_t key_len)
{
    static const char *const exact[] = {
        "gclid", "dclid", "gbraid", "wbraid",
        "fbclid", "msclkid", "yclid", "twclid", "igshid",
        "mc_cid", "mc_eid",
        "_hsenc", "_hsmi", "__hssc", "__hstc", "__hsfp", "hsctatracking",
        "vero_id", "vero_conv",
        "oly_anon_id", "oly_enc_id",
        "_openstat", "wickedid", "rb_clickid", "s_cid",
        "ml_subscriber", "ml_subscriber_hash",
        "mtm_source", "mtm_medium", "mtm_campaign", "mtm_keyword",
        "mtm_cid", "mtm_content", "mtm_group", "mtm_placement",
        "pk_source", "pk_medium", "pk_campaign", "pk_keyword",
        "pk_cid", "pk_content",
    };
    if (key_len == 0)
        return FALSE;
    if (key_len >= 4 && g_ascii_strncasecmp(key, "utm_", 4) == 0)
        return TRUE;
    for (gsize i = 0; i < G_N_ELEMENTS(exact); i++)
        if (strlen(exact[i]) == key_len &&
            g_ascii_strncasecmp(key, exact[i], key_len) == 0)
            return TRUE;
    return FALSE;
}

char *
ns_url_strip_tracking_params(const char *url)
{
    if (!ns_url_is_http_or_https(url))
        return NULL;
    const ns_config *cfg = ns_config_get();
    if (!cfg || !cfg->strip_tracking_params)
        return NULL;

    const char *frag = strchr(url, '#');
    const char *query_end = frag ? frag : url + strlen(url);
    const char *query = strchr(url, '?');
    if (!query || query >= query_end)
        return NULL;

    GString *kept = g_string_new(NULL);
    gboolean removed = FALSE;
    for (const char *p = query + 1; ; ) {
        const char *amp = memchr(p, '&', (size_t)(query_end - p));
        const char *tok_end = amp ? amp : query_end;
        const char *eq = memchr(p, '=', (size_t)(tok_end - p));
        size_t key_len = (size_t)((eq ? eq : tok_end) - p);
        if (ns_query_param_is_tracking(p, key_len)) {
            removed = TRUE;
        } else {
            if (kept->len)
                g_string_append_c(kept, '&');
            g_string_append_len(kept, p, (gssize)(tok_end - p));
        }
        if (!amp)
            break;
        p = amp + 1;
    }

    if (!removed) {
        g_string_free(kept, TRUE);
        return NULL;
    }

    GString *out = g_string_new_len(url, (gssize)(query - url));
    if (kept->len) {
        g_string_append_c(out, '?');
        g_string_append_len(out, kept->str, (gssize)kept->len);
    }
    if (frag)
        g_string_append(out, frag);
    g_string_free(kept, TRUE);
    return g_string_free(out, FALSE);
}

static gboolean
ns_url_is_ftp(const char *url)
{
    return url && g_str_has_prefix(url, "ftp://");
}

static gboolean
refresh_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

gboolean
ns_net_parse_refresh(const char *input, double *time_out, char **url_out)
{
    if (time_out) *time_out = 0.0;
    if (url_out) *url_out = NULL;
    if (!input) return FALSE;

    const char *p = input;
    while (refresh_is_space(*p)) p++;

    const char *digits = p;
    while (g_ascii_isdigit(*p)) p++;
    if (p == digits && *p != '.') return FALSE;

    double seconds = 0.0;
    for (const char *d = digits; d < p; d++) {
        seconds = seconds * 10.0 + (double)(*d - '0');
        if (seconds > 2147483647.0) { seconds = 2147483647.0; break; }
    }
    while (*p == '.' || g_ascii_isdigit(*p)) p++;

    if (*p) {
        if (*p != ';' && *p != ',' && !refresh_is_space(*p)) return FALSE;
        while (refresh_is_space(*p)) p++;
        if (*p == ';' || *p == ',') p++;
        while (refresh_is_space(*p)) p++;
    }

    if (*p == 'u' || *p == 'U') {
        const char *q = p + 1;
        if (*q == 'r' || *q == 'R') {
            q++;
            if (*q == 'l' || *q == 'L') {
                q++;
                while (refresh_is_space(*q)) q++;
                if (*q == '=') {
                    q++;
                    while (refresh_is_space(*q)) q++;
                    p = q;
                }
            }
        }
    }

    char *url = NULL;
    if (*p == '\'' || *p == '"') {
        char quote = *p++;
        const char *end = strchr(p, quote);
        url = end ? g_strndup(p, (gsize)(end - p)) : g_strdup(p);
    } else if (*p) {
        url = g_strdup(p);
        g_strchomp(url);
    }
    if (url && !*url) g_clear_pointer(&url, g_free);

    if (time_out) *time_out = seconds;
    if (url_out) *url_out = url;
    else g_free(url);
    return TRUE;
}

static lxb_status_t
ns_url_str_append_cb(const lxb_char_t *data, size_t length, void *ctx)
{
    g_string_append_len((GString *)ctx, (const char *)data, (gssize)length);
    return LXB_STATUS_OK;
}

static void
ns_url_parser_destroy_tls(gpointer p)
{
    lxb_url_parser_t *parser = p;
    if (!parser) return;
    lxb_url_parser_memory_destroy(parser);
    lxb_url_parser_destroy(parser, true);
}

static GPrivate g_url_parser_tls = G_PRIVATE_INIT(ns_url_parser_destroy_tls);

static lxb_url_parser_t *
ns_url_parser_open(void)
{
    lxb_url_parser_t *parser = g_private_get(&g_url_parser_tls);
    if (parser) {
        lxb_url_parser_clean(parser);
        return parser;
    }
    parser = lxb_url_parser_create();
    if (!parser) return NULL;
    if (lxb_url_parser_init(parser, NULL) != LXB_STATUS_OK) {
        lxb_url_parser_destroy(parser, true);
        return NULL;
    }
    g_private_set(&g_url_parser_tls, parser);
    return parser;
}

static void
ns_url_parser_close(lxb_url_parser_t *parser)
{
    if (!parser) return;
    lxb_url_parser_clean(parser);
}

char *
ns_url_resolve_len(const char *base, const char *href, size_t href_len)
{
    if (!href) return NULL;
    if (href_len == 0 && !(base && *base)) return NULL;

    lxb_url_parser_t *parser = ns_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *base_url = NULL;
    if (base && *base) {
        base_url = lxb_url_parse(parser, NULL,
                                 (const lxb_char_t *)base, strlen(base));
        lxb_url_parser_clean(parser);
        if (!base_url) {
            ns_url_parser_close(parser);
            return NULL;
        }
    }
    lxb_url_t *resolved = lxb_url_parse(parser, base_url,
                                        (const lxb_char_t *)href, href_len);
    char *out = NULL;
    if (resolved) {
        GString *s = g_string_new(NULL);
        if (lxb_url_serialize(resolved, ns_url_str_append_cb, s, false)
            == LXB_STATUS_OK && s->len > 0)
            out = g_string_free(s, FALSE);
        else
            g_string_free(s, TRUE);
    }
    ns_url_parser_close(parser);
    return out;
}

char *
ns_url_resolve(const char *base, const char *href)
{
    return ns_url_resolve_len(base, href, href ? strlen(href) : 0);
}

static gboolean
ns_url_host_is_empty(const lxb_url_t *u)
{
    switch (u->host.type) {
    case LXB_URL_HOST_TYPE__UNDEF:
    case LXB_URL_HOST_TYPE_EMPTY:
        return TRUE;
    case LXB_URL_HOST_TYPE_OPAQUE:
        return u->host.u.opaque.length == 0;
    case LXB_URL_HOST_TYPE_DOMAIN:
        return u->host.u.domain.length == 0;
    default:
        return FALSE;
    }
}

static gboolean
ns_url_is_special(const lxb_url_t *u)
{
    return u->scheme.type >= LXB_URL_SCHEMEL_TYPE_HTTP &&
           u->scheme.type <= LXB_URL_SCHEMEL_TYPE_FILE;
}

static gboolean
ns_url_has_credentials_or_port(const lxb_url_t *u)
{
    return u->username.length > 0 || u->password.length > 0 || u->has_port;
}

static void
ns_url_host_setter(lxb_url_t *u, lxb_url_parser_t *parser,
                   const char *v, size_t len, gboolean hostname_only)
{
    gboolean special = ns_url_is_special(u);
    gboolean brackets = FALSE;
    size_t colon = len, end = 0;
    for (; end < len; end++) {
        char c = v[end];
        if (c == '/' || c == '?' || c == '#' || (special && c == '\\'))
            break;
        if (c == '[') brackets = TRUE;
        else if (c == ']') brackets = FALSE;
        else if (c == ':' && !brackets && colon == len) colon = end;
    }
    if (colon == len && end == 0 && !special &&
        ns_url_has_credentials_or_port(u))
        return;
    if (colon == len || hostname_only ||
        u->scheme.type == LXB_URL_SCHEMEL_TYPE_FILE) {
        if (hostname_only)
            (void) lxb_url_api_hostname_set(u, parser,
                                            (const lxb_char_t *)v, len);
        else
            (void) lxb_url_api_host_set(u, parser,
                                        (const lxb_char_t *)v, len);
        return;
    }
    if (colon == 0) return;
    if (lxb_url_api_hostname_set(u, parser, (const lxb_char_t *)v, colon)
        != LXB_STATUS_OK)
        return;
    size_t digits = colon + 1;
    while (digits < end && g_ascii_isdigit(v[digits])) digits++;
    if (digits == colon + 1) return;
    guint64 port = 0;
    for (size_t i = colon + 1; i < digits && port <= 65535; i++)
        port = port * 10 + (guint64)(v[i] - '0');
    if (port > 65535) return;
    (void) lxb_url_api_port_set(u, parser, (const lxb_char_t *)v + colon + 1,
                                digits - colon - 1);
}

static void
ns_url_apply_setter(lxb_url_t *u, lxb_url_parser_t *parser,
                    const char *component, const char *value, size_t vlen)
{
    const lxb_char_t *v = (const lxb_char_t *)value;
    gboolean userinfo_or_port = strcmp(component, "username") == 0 ||
                                strcmp(component, "password") == 0 ||
                                strcmp(component, "port") == 0;
    if (userinfo_or_port && ns_url_host_is_empty(u))
        return;
    if (strcmp(component, "protocol") == 0)
        (void) lxb_url_api_protocol_set(u, parser, v, vlen);
    else if (strcmp(component, "username") == 0)
        (void) lxb_url_api_username_set(u, v, vlen);
    else if (strcmp(component, "password") == 0)
        (void) lxb_url_api_password_set(u, v, vlen);
    else if (strcmp(component, "host") == 0)
        ns_url_host_setter(u, parser, value, vlen, FALSE);
    else if (strcmp(component, "hostname") == 0)
        ns_url_host_setter(u, parser, value, vlen, TRUE);
    else if (strcmp(component, "port") == 0)
        (void) lxb_url_api_port_set(u, parser, v, vlen);
    else if (strcmp(component, "pathname") == 0)
        (void) lxb_url_api_pathname_set(u, parser, v, vlen);
    else if (strcmp(component, "search") == 0)
        (void) lxb_url_api_search_set(u, parser, v, vlen);
    else if (strcmp(component, "hash") == 0)
        (void) lxb_url_api_hash_set(u, parser, v, vlen);
}

char *
ns_url_set_component_len(const char *href, const char *component,
                         const char *value, size_t value_len)
{
    if (!href || !component || !value) return NULL;

    lxb_url_parser_t *parser = ns_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *u = lxb_url_parse(parser, NULL,
                                 (const lxb_char_t *)href, strlen(href));
    if (!u) {
        ns_url_parser_close(parser);
        return NULL;
    }

    ns_url_apply_setter(u, parser, component, value, value_len);

    char *out = NULL;
    GString *s = g_string_new(NULL);
    if (lxb_url_serialize(u, ns_url_str_append_cb, s, false) == LXB_STATUS_OK
        && s->len > 0)
        out = g_string_free(s, FALSE);
    else
        g_string_free(s, TRUE);
    ns_url_parser_close(parser);
    return out;
}

static char *
ns_url_to_ascii(const char *url)
{
    if (!url || !*url) return NULL;
    if (g_str_has_prefix(url, "data:") || g_str_has_prefix(url, "about:") ||
        g_str_has_prefix(url, "file:"))
        return g_strdup(url);
    return ns_url_resolve(NULL, url);
}

static lxb_url_t *
ns_url_parse_with_host(lxb_url_parser_t *parser, const char *url)
{
    lxb_url_t *u = lxb_url_parse(parser, NULL,
                                 (const lxb_char_t *)url, strlen(url));
    if (!u) return NULL;
    if (u->host.type == LXB_URL_HOST_TYPE__UNDEF ||
        u->host.type == LXB_URL_HOST_TYPE_EMPTY)
        return NULL;
    return u;
}

char *
ns_url_origin_from(const char *url)
{
    if (!url || !*url) return NULL;
    if (g_ascii_strncasecmp(url, "blob:", 5) == 0 &&
        ns_url_is_http_or_https(url + 5))
        return ns_url_origin_from(url + 5);
    if (!ns_url_is_http_or_https(url))
        return NULL;

    lxb_url_parser_t *parser = ns_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *u = ns_url_parse_with_host(parser, url);
    char *out = NULL;
    if (u) {
        GString *s = g_string_new(NULL);
        g_string_append_len(s, (const char *)u->scheme.name.data,
                            (gssize)u->scheme.name.length);
        g_string_append(s, "://");
        if (lxb_url_serialize_host(&u->host, ns_url_str_append_cb, s)
            == LXB_STATUS_OK) {
            if (u->has_port)
                g_string_append_printf(s, ":%u", (unsigned)u->port);
            out = g_string_free(s, FALSE);
        } else {
            g_string_free(s, TRUE);
        }
    }
    ns_url_parser_close(parser);
    return out;
}

char *
ns_net_raw_header_values(const char *raw, const char *name)
{
    if (!raw || !name) return NULL;
    gsize n = strlen(name);
    GString *out = NULL;
    for (const char *line = raw; line && *line; ) {
        const char *eol = strchr(line, '\n');
        gsize len = eol ? (gsize)(eol - line) : strlen(line);
        if (len > n && line[n] == ':' &&
            g_ascii_strncasecmp(line, name, n) == 0) {
            char *v = g_strstrip(g_strndup(line + n + 1, len - n - 1));
            if (!out) out = g_string_new(v);
            else g_string_append_printf(out, ",%s", v);
            g_free(v);
        }
        line = eol ? eol + 1 : NULL;
    }
    return out ? g_string_free(out, FALSE) : NULL;
}

static char *
ns_url_origin_from_any(const char *url)
{
    if (!url || !*url) return NULL;
    lxb_url_parser_t *parser = ns_url_parser_open();
    if (!parser) return NULL;
    lxb_url_t *u = ns_url_parse_with_host(parser, url);
    char *out = NULL;
    if (u) {
        GString *s = g_string_new(NULL);
        g_string_append_len(s, (const char *)u->scheme.name.data,
                            (gssize)u->scheme.name.length);
        g_string_append(s, "://");
        if (lxb_url_serialize_host(&u->host, ns_url_str_append_cb, s)
            == LXB_STATUS_OK) {
            unsigned port = u->has_port ? (unsigned)u->port : 0u;
            if (!u->has_port) {
                switch (u->scheme.type) {
                case LXB_URL_SCHEMEL_TYPE_HTTP:
                case LXB_URL_SCHEMEL_TYPE_WS:    port = 80;  break;
                case LXB_URL_SCHEMEL_TYPE_HTTPS:
                case LXB_URL_SCHEMEL_TYPE_WSS:   port = 443; break;
                case LXB_URL_SCHEMEL_TYPE_FTP:   port = 21;  break;
                default:                         break;
                }
            }
            g_string_append_printf(s, ":%u", port);
            out = g_string_free(s, FALSE);
        } else {
            g_string_free(s, TRUE);
        }
    }
    ns_url_parser_close(parser);
    return out;
}

gboolean
ns_url_same_origin(const char *a, const char *b)
{
    char *oa = ns_url_origin_from(a);
    char *ob = ns_url_origin_from(b);
    gboolean eq = oa && *oa && ob && *ob && g_ascii_strcasecmp(oa, ob) == 0;
    g_free(oa);
    g_free(ob);
    return eq;
}

char *
ns_net_referer_for(const char *url, const char *top_url,
                   ns_referer_policy policy)
{
    if (!top_url || !*top_url || !ns_url_is_http_or_https(url) ||
        !ns_url_is_http_or_https(top_url) ||
        policy == NS_REFERER_NO_REFERRER)
        return NULL;
    if (g_str_has_prefix(top_url, "https://") &&
        g_str_has_prefix(url, "http://"))
        return NULL;
    gboolean same_origin = ns_url_same_origin(top_url, url);
    if (policy == NS_REFERER_SAME_ORIGIN && !same_origin)
        return NULL;
    if (policy == NS_REFERER_UNSAFE_URL || same_origin) {
        g_autoptr(ns_url_parts) p = ns_url_parts_new(top_url);
        if (!p || !p->origin || !*p->origin) return NULL;
        return g_strconcat(p->origin, p->pathname ? p->pathname : "",
                           p->search ? p->search : "", NULL);
    }
    char *origin = ns_url_origin_from(top_url);
    if (!origin) return NULL;
    char *out = g_strdup_printf("%s/", origin);
    g_free(origin);
    return out;
}

static char *
ns_url_site_from(const char *url)
{
    if (!url || !*url) return NULL;
    g_autoptr(ns_url_parts) p = ns_url_parts_new(url);
    if (!p || !p->protocol || !p->hostname) return NULL;
    g_autofree char *lower = g_ascii_strdown(p->hostname, -1);
    const psl_ctx_t *psl = psl_builtin();
    const char *reg = psl ? psl_registrable_domain(psl, lower) : NULL;
    const char *site_host = (reg && *reg) ? reg : p->hostname;
    if (p->port && *p->port)
        return g_strdup_printf("%s://%s:%s", p->protocol, site_host, p->port);
    return g_strdup_printf("%s://%s", p->protocol, site_host);
}

/* The site of a URL in the form libcurl's CURLOPT_COOKIE_SITE expects:
 * scheme, "://" and the registrable domain, or the host itself for an IP
 * address. Unlike ns_url_site_from() this is not used as a partition key. */
static char *
ns_net_curl_site_from(const char *url)
{
    if (!url || !*url || !ns_url_is_http_or_https(url)) return NULL;
    g_autoptr(ns_url_parts) p = ns_url_parts_new(url);
    if (!p || !p->protocol || !p->hostname || !*p->hostname) return NULL;
    size_t plen = strlen(p->protocol);
    if (plen && p->protocol[plen - 1] == ':') plen--;
    g_autofree char *lower = g_ascii_strdown(p->hostname, -1);
    const char *site_host = lower;
    if (!g_hostname_is_ip_address(lower)) {
        const psl_ctx_t *psl = psl_builtin();
        const char *reg = psl ? psl_registrable_domain(psl, lower) : NULL;
        if (reg && *reg) site_host = reg;
    }
    if (strchr(site_host, ':'))
        return g_strdup_printf("%.*s://[%s]", (int)plen, p->protocol, site_host);
    return g_strdup_printf("%.*s://%s", (int)plen, p->protocol, site_host);
}

gboolean
ns_url_is_same_site(const char *a, const char *b)
{
    if (!a || !b) return FALSE;
    g_autofree char *sa = ns_url_site_from(a);
    g_autofree char *sb = ns_url_site_from(b);
    if (sa && sb) return g_ascii_strcasecmp(sa, sb) == 0;

    g_autofree char *ha = ns_url_host_from(a);
    g_autofree char *hb = ns_url_host_from(b);
    if (!ha || !hb) return FALSE;
    if (g_ascii_strcasecmp(ha, hb) == 0) return TRUE;
    gsize la = strlen(ha), lb = strlen(hb);
    if (la > lb + 1 && ha[la - lb - 1] == '.' &&
        g_ascii_strncasecmp(ha + la - lb, hb, lb) == 0)
        return TRUE;
    if (lb > la + 1 && hb[lb - la - 1] == '.' &&
        g_ascii_strncasecmp(hb + lb - la, ha, la) == 0)
        return TRUE;
    return FALSE;
}

char *
ns_url_host_from(const char *url)
{
    if (!url) return NULL;

    lxb_url_parser_t *parser = ns_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *u = ns_url_parse_with_host(parser, url);
    char *out = NULL;
    if (u) {
        GString *s = g_string_new(NULL);
        if (lxb_url_serialize_host(&u->host, ns_url_str_append_cb, s)
            == LXB_STATUS_OK && s->len > 0)
            out = g_string_free(s, FALSE);
        else
            g_string_free(s, TRUE);
    }
    ns_url_parser_close(parser);
    return out;
}

void
ns_url_parts_free(ns_url_parts *parts)
{
    if (!parts) return;
    g_free(parts->href);
    g_free(parts->protocol);
    g_free(parts->origin);
    g_free(parts->host);
    g_free(parts->hostname);
    g_free(parts->port);
    g_free(parts->pathname);
    g_free(parts->search);
    g_free(parts->hash);
    g_free(parts->username);
    g_free(parts->password);
    g_free(parts);
}

static char *
ns_url_take_serialized(GString *s, lxb_status_t status)
{
    if (status != LXB_STATUS_OK) {
        g_string_free(s, TRUE);
        return g_strdup("");
    }
    return g_string_free(s, FALSE);
}

static ns_url_parts *
ns_url_parts_new_depth(const char *url, int depth)
{
    if (!url) return NULL;

    lxb_url_parser_t *parser = ns_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *u = lxb_url_parse(parser, NULL,
                                 (const lxb_char_t *)url, strlen(url));
    if (!u) {
        ns_url_parser_close(parser);
        return NULL;
    }

    ns_url_parts *p = g_new0(ns_url_parts, 1);

    GString *s = g_string_new(NULL);
    p->href = ns_url_take_serialized(s,
        lxb_url_serialize(u, ns_url_str_append_cb, s, false));
    if (!*p->href) {
        g_free(p->href);
        p->href = g_strdup(url);
    }

    s = g_string_new(NULL);
    char *scheme = ns_url_take_serialized(s,
        lxb_url_serialize_scheme(u, ns_url_str_append_cb, s));
    p->protocol = *scheme ? g_strconcat(scheme, ":", NULL) : g_strdup("");
    g_free(scheme);

    if (u->host.type == LXB_URL_HOST_TYPE__UNDEF ||
        u->host.type == LXB_URL_HOST_TYPE_EMPTY) {
        p->hostname = g_strdup("");
    } else {
        s = g_string_new(NULL);
        p->hostname = ns_url_take_serialized(s,
            lxb_url_serialize_host(&u->host, ns_url_str_append_cb, s));
    }

    p->port = u->has_port ? g_strdup_printf("%u", (unsigned)u->port)
                          : g_strdup("");

    p->host = (*p->hostname && *p->port)
        ? g_strconcat(p->hostname, ":", p->port, NULL)
        : g_strdup(p->hostname);

    gboolean tuple_origin = *p->hostname
        && (strcmp(p->protocol, "http:") == 0
            || strcmp(p->protocol, "https:") == 0
            || strcmp(p->protocol, "ws:") == 0
            || strcmp(p->protocol, "wss:") == 0
            || strcmp(p->protocol, "ftp:") == 0);
    p->origin = tuple_origin
        ? g_strconcat(p->protocol, "//", p->host, NULL)
        : g_strdup("null");

    s = g_string_new(NULL);
    p->pathname = ns_url_take_serialized(s,
        lxb_url_serialize_path(&u->path, ns_url_str_append_cb, s));

    if (depth == 0 && strcmp(p->protocol, "blob:") == 0 &&
        p->pathname && *p->pathname) {
        ns_url_parts *inner = ns_url_parts_new_depth(p->pathname, depth + 1);
        if (inner) {
            if (strcmp(inner->protocol, "http:") == 0 ||
                strcmp(inner->protocol, "https:") == 0) {
                g_free(p->origin);
                p->origin = g_strdup(inner->origin);
            }
            ns_url_parts_free(inner);
        }
    }

    if (u->query.length) {
        s = g_string_new("?");
        g_string_append_len(s, (const char *)u->query.data,
                            (gssize)u->query.length);
        p->search = g_string_free(s, FALSE);
    } else {
        p->search = g_strdup("");
    }

    if (u->fragment.length) {
        s = g_string_new("#");
        g_string_append_len(s, (const char *)u->fragment.data,
                            (gssize)u->fragment.length);
        p->hash = g_string_free(s, FALSE);
    } else {
        p->hash = g_strdup("");
    }

    p->username = u->username.length
        ? g_strndup((const char *)u->username.data, u->username.length)
        : g_strdup("");
    p->password = u->password.length
        ? g_strndup((const char *)u->password.data, u->password.length)
        : g_strdup("");

    ns_url_parser_close(parser);
    return p;
}

ns_url_parts *
ns_url_parts_new(const char *url)
{
    return ns_url_parts_new_depth(url, 0);
}

gboolean
ns_url_is_valid_absolute(const char *url)
{
    if (!url || !*url) return FALSE;
    for (const char *p = url; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '\n' ||
            *p == '\r' || *p == '\f')
            return FALSE;
    g_autoptr(ns_url_parts) parts = ns_url_parts_new(url);
    return parts && parts->protocol && *parts->protocol;
}

gboolean
ns_net_hsts_should_upgrade(const char *host)
{
    if (!host || !*host) return FALSE;
    char *path = ns_net_hsts_curl_path();
    if (!path) return FALSE;
    g_mutex_lock(&g_hsts_lock);
    gint64 mtime = file_mtime_us(path);
    if (!g_hsts_cache || mtime != g_hsts_cache_mtime_us) {
        ns_hsts_cache_reload_locked(path);
        g_hsts_cache_mtime_us = mtime;
    }
    char *lower = g_ascii_strdown(host, -1);
    for (gsize ln = strlen(lower); ln > 0 && lower[ln - 1] == '.'; ln--)
        lower[ln - 1] = '\0';
    gboolean hit = ns_hsts_lookup_locked(lower);
    g_free(lower);
    g_mutex_unlock(&g_hsts_lock);
    return hit;
}

char *
ns_net_hsts_upgrade(const char *url)
{
    if (!url) return NULL;
    if (!g_str_has_prefix(url, "http://")) return NULL;
    char *host = ns_url_host_from(url);
    if (!host) return NULL;
    gboolean upgrade = ns_net_hsts_should_upgrade(host);
    g_free(host);
    if (!upgrade) return NULL;
    return g_strconcat("https://", url + 7, NULL);
}

gboolean
ns_net_is_mobile_mode(void)
{
#if defined(__ANDROID__)
    return g_strcmp0(g_getenv("NS_PAGE_FOR_COMPUTER"), "1") != 0;
#else
    return NS_UA_HINT_MOBILE != 0;
#endif
}

const char *
ns_net_navigator_platform(void)
{
#if defined(__ANDROID__)
    return ns_net_is_mobile_mode() ? NS_NAV_PLATFORM : "Linux x86_64";
#else
    return NS_NAV_PLATFORM;
#endif
}

const char *
ns_net_ua_hint_platform(void)
{
#if defined(__ANDROID__)
    return ns_net_is_mobile_mode() ? NS_UA_HINT_PLATFORM : "Linux";
#else
    return NS_UA_HINT_PLATFORM;
#endif
}

const char *
ns_user_agent_for_mode(const char *compat_mode)
{
#if defined(__ANDROID__)
    if (!ns_net_is_mobile_mode()) return NS_DESKTOP_USER_AGENT;
#endif
    if (compat_mode && *compat_mode) {
        if (g_ascii_strcasecmp(compat_mode, "ladybird") == 0)
            return NS_UA_LADYBIRD;
        if (g_ascii_strcasecmp(compat_mode, "firefox") == 0)
            return NS_UA_FIREFOX;
    }
    return NS_USER_AGENT;
}

gboolean
ns_user_agent_has_client_hints(const char *user_agent)
{
    return user_agent && strstr(user_agent, "Chrome/") &&
           !strstr(user_agent, "Ladybird/");
}

static gboolean
ns_host_is_loopback(const char *host)
{
    return g_ascii_strcasecmp(host, "localhost") == 0 ||
           g_str_has_suffix(host, ".localhost") ||
           strcmp(host, "127.0.0.1") == 0 ||
           strcmp(host, "::1") == 0 ||
           strcmp(host, "[::1]") == 0;
}

char *
ns_net_https_first_upgrade(const char *url)
{
    const ns_config *cfg = ns_config_get();
    if (!cfg || !cfg->https_first) return NULL;
    if (!url || !g_str_has_prefix(url, "http://")) return NULL;
    char *host = ns_url_host_from(url);
    if (!host) return NULL;
    gboolean loopback = ns_host_is_loopback(host);
    g_free(host);
    if (loopback) return NULL;
    return g_strconcat("https://", url + 7, NULL);
}

gboolean
ns_net_header_is_nosniff(const char *value)
{
    if (!value || !*value) return FALSE;
    for (const char *p = value; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        if ((size_t)(p - start) == 7 &&
            g_ascii_strncasecmp(start, "nosniff", 7) == 0)
            return TRUE;
    }
    return FALSE;
}

static const char *
ns_net_cookie_dir(void)
{
    if (g_cookie_dir) return g_cookie_dir;
    if (ns_net_is_private()) {
        const char *root = ns_net_private_root();
        g_cookie_dir = g_build_filename(root ? root : g_get_tmp_dir(),
                                        "cookies", NULL);
    } else {
        const char *config = g_get_user_config_dir();
        g_cookie_dir = g_build_filename(config, NS_APP_DIR_NAME, "cookies",
                                        NULL);
    }
    g_mkdir_with_parents(g_cookie_dir, 0700);
    return g_cookie_dir;
}

static void
ns_net_empty_dir(const char *dir)
{
    if (!dir) return;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        char *child = g_build_filename(dir, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
            !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
            ns_rmrf(child);
        else
            g_unlink(child);
        g_free(child);
    }
    g_dir_close(d);
}

void
ns_net_cookies_clear(void)
{
    ns_net_empty_dir(ns_net_cookie_dir());
}

void
ns_net_site_storage_clear(void)
{
    char *base = g_build_filename(g_get_user_data_dir(), NS_APP_DIR_NAME, NULL);
    char *localstorage = g_build_filename(base, "localstorage", NULL);
    char *indexeddb    = g_build_filename(base, "indexeddb", NULL);
    ns_rmrf(localstorage);
    ns_rmrf(indexeddb);
    g_free(localstorage);
    g_free(indexeddb);
    g_free(base);
}

static char *
ns_net_cookie_path_for_partition(const char *top_origin)
{
    const char *dir = ns_net_cookie_dir();
    const char *key = (top_origin && *top_origin) ? top_origin : "default";
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
    char short_hex[33];
    g_strlcpy(short_hex, digest, sizeof(short_hex));
    g_free(digest);
    char *fname = g_strdup_printf("%s.txt", short_hex);
    char *path = g_build_filename(dir, fname, NULL);
    g_free(fname);
    return path;
}

static char *
ns_net_cookie_js_path_for_partition(const char *top_origin)
{
    const char *dir = ns_net_cookie_dir();
    const char *key = (top_origin && *top_origin) ? top_origin : "default";
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
    char short_hex[33];
    g_strlcpy(short_hex, digest, sizeof(short_hex));
    g_free(digest);
    char *fname = g_strdup_printf("%s.js.txt", short_hex);
    char *path = g_build_filename(dir, fname, NULL);
    g_free(fname);
    return path;
}

enum {
    NS_COOKIE_SAMESITE_NONE,
    NS_COOKIE_SAMESITE_LAX,
    NS_COOKIE_SAMESITE_STRICT,
};

static char *
ns_cookie_samesite_path(const char *jar_path)
{
    return jar_path ? g_strconcat(jar_path, ".samesite", NULL) : NULL;
}

static GHashTable *
ns_cookie_samesite_load(const char *jar_path)
{
    GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, NULL);
    g_autofree char *p = ns_cookie_samesite_path(jar_path);
    char *contents = NULL;
    if (p && g_file_get_contents(p, &contents, NULL, NULL)) {
        char **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *tab = strrchr(lines[i], '\t');
            if (!tab || tab == lines[i]) continue;
            int level = tab[1] - '0';
            if (level < 0 || level > 2) continue;
            g_hash_table_replace(t, g_strndup(lines[i], tab - lines[i]),
                                 GINT_TO_POINTER(level + 1));
        }
        g_strfreev(lines);
        g_free(contents);
    }
    return t;
}

static void
ns_cookie_samesite_record(const char *jar_path, const char *domain,
                          const char *path, const char *name, int level,
                          gboolean remove)
{
    g_autofree char *p = ns_cookie_samesite_path(jar_path);
    if (!p) return;
    g_autofree char *key = g_strdup_printf("%s\t%s\t%s", domain, path, name);
    GHashTable *t = ns_cookie_samesite_load(jar_path);
    if (remove)
        g_hash_table_remove(t, key);
    else
        g_hash_table_replace(t, g_strdup(key), GINT_TO_POINTER(level + 1));
    GString *out = g_string_new(NULL);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, t);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_string_append_printf(out, "%s\t%d\n", (const char *)k,
                               GPOINTER_TO_INT(v) - 1);
    g_file_set_contents_full(p, out->str, (gssize)out->len,
                             G_FILE_SET_CONTENTS_CONSISTENT |
                             G_FILE_SET_CONTENTS_ONLY_EXISTING, 0600, NULL);
    g_string_free(out, TRUE);
    g_hash_table_unref(t);
}

static gboolean
ns_cookie_samesite_excluded(GHashTable *levels, const char *domain,
                            const char *path, const char *name,
                            int samesite_cross)
{
    if (samesite_cross <= 0) return FALSE;
    g_autofree char *key = g_strdup_printf("%s\t%s\t%s", domain, path, name);
    gpointer v = levels ? g_hash_table_lookup(levels, key) : NULL;
    int level = v ? GPOINTER_TO_INT(v) - 1 : NS_COOKIE_SAMESITE_LAX;
    if (level == NS_COOKIE_SAMESITE_STRICT) return TRUE;
    return samesite_cross >= 2 && level == NS_COOKIE_SAMESITE_LAX;
}

static char *
ns_cookie_collect(const char *url, const char *jar_path, const char *js_path,
                  gboolean include_httponly, int samesite_cross)
{
    if (!url || !*url) return NULL;
    g_autoptr(ns_url_parts) parts = ns_url_parts_new(url);
    if (!parts || !parts->hostname || !*parts->hostname) return NULL;
    const char *host = parts->hostname;
    const char *path = (parts->pathname && *parts->pathname)
                       ? parts->pathname : "/";
    gboolean is_https = parts->protocol &&
                        g_ascii_strcasecmp(parts->protocol, "https:") == 0;

    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gsize hl = strlen(host);
    GPtrArray *order = g_ptr_array_new_with_free_func(g_free);
    GHashTable *vals = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);

    for (int pass = 0; pass < 2; pass++) {
        const char *fp = pass == 0 ? jar_path : js_path;
        char *contents = NULL;
        if (!fp || !g_file_get_contents(fp, &contents, NULL, NULL)) {
            g_free(contents);
            continue;
        }
        GHashTable *levels = samesite_cross > 0
            ? ns_cookie_samesite_load(fp) : NULL;
        char **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *line = g_strchomp(lines[i]);
            if (!*line) continue;
            if (line[0] == '#') {
                if (include_httponly && g_str_has_prefix(line, "#HttpOnly_"))
                    line += strlen("#HttpOnly_");
                else
                    continue;
            }
            char **f = g_strsplit(line, "\t", 7);
            int nf = 0;
            while (f[nf]) nf++;
            if (nf < 7) { g_strfreev(f); continue; }
            const char *cdomain = f[0];
            const char *cpath   = f[2];
            gboolean csecure = g_ascii_strcasecmp(f[3], "TRUE") == 0;
            gint64 cexpiry = g_ascii_strtoll(f[4], NULL, 10);
            const char *cname = f[5];
            const char *cval  = f[6];

            gboolean match;
            if (cdomain[0] == '.') {
                gsize dl = strlen(cdomain);
                match = (hl >= dl &&
                         g_ascii_strcasecmp(host + hl - dl, cdomain) == 0) ||
                        g_ascii_strcasecmp(host, cdomain + 1) == 0;
            } else {
                match = g_ascii_strcasecmp(host, cdomain) == 0;
            }
            if (match && cpath && *cpath) {
                gsize cl = strlen(cpath);
                if (!g_str_has_prefix(path, cpath))
                    match = FALSE;
                else if (path[cl] != '\0' && path[cl] != '/' &&
                         cpath[cl - 1] != '/')
                    match = FALSE;
            }
            if (match && csecure && !is_https) match = FALSE;
            if (match && cexpiry != 0 && cexpiry < now) match = FALSE;
            if (match && ns_cookie_samesite_excluded(levels, cdomain, cpath,
                                                     cname, samesite_cross))
                match = FALSE;
            if (match && cname && *cname) {
                if (!g_hash_table_contains(vals, cname))
                    g_ptr_array_add(order, g_strdup(cname));
                g_hash_table_replace(vals, g_strdup(cname),
                                     g_strdup(cval ? cval : ""));
            }
            g_strfreev(f);
        }
        if (levels) g_hash_table_unref(levels);
        g_strfreev(lines);
        g_free(contents);
    }

    GString *out = g_string_new(NULL);
    for (guint i = 0; i < order->len; i++) {
        const char *nm = g_ptr_array_index(order, i);
        const char *vv = g_hash_table_lookup(vals, nm);
        if (out->len) g_string_append(out, "; ");
        g_string_append(out, nm);
        g_string_append_c(out, '=');
        g_string_append(out, vv ? vv : "");
    }
    g_ptr_array_free(order, TRUE);
    g_hash_table_destroy(vals);
    if (out->len == 0) return g_string_free(out, TRUE), NULL;
    return g_string_free(out, FALSE);
}

static char *
ns_cookie_js_partition(const char *url, const char *top_url)
{
    if (!ns_url_is_http_or_https(url))
        return NULL;
    if (!top_url || !*top_url || ns_url_is_same_site(url, top_url))
        return ns_url_site_from(url);
    const ns_config *cfg = ns_config_get();
    ns_cookie_policy policy = cfg ? cfg->cookie_policy : NS_COOKIE_FIRST_PARTY;
    if (policy != NS_COOKIE_ALWAYS)
        return NULL;
    return ns_url_site_from(top_url);
}

char *
ns_net_cookies_for_js(const char *url, const char *top_url)
{
    g_autofree char *site = ns_cookie_js_partition(url, top_url);
    if (!site || !*site) return NULL;
    g_autofree char *jar_path = ns_net_cookie_path_for_partition(site);
    g_autofree char *js_path  = ns_net_cookie_js_path_for_partition(site);
    return ns_cookie_collect(url, jar_path, js_path, FALSE, 0);
}

void
ns_net_curl_apply_cookies(void *curl_handle, const char *url,
                          const char *origin)
{
    CURL *curl = curl_handle;
    g_autofree char *http_url = NULL;
    if (url && g_str_has_prefix(url, "wss://"))
        url = http_url = g_strconcat("https://", url + 6, NULL);
    else if (url && g_str_has_prefix(url, "ws://"))
        url = http_url = g_strconcat("http://", url + 5, NULL);
    const ns_config *cfg = ns_config_get();
    ns_cookie_policy policy = cfg ? cfg->cookie_policy : NS_COOKIE_FIRST_PARTY;
    if (!curl || !url || !origin || !*origin || policy == NS_COOKIE_NEVER ||
        !ns_url_is_http_or_https(origin))
        return;
    if (policy == NS_COOKIE_FIRST_PARTY && !ns_url_is_same_site(url, origin))
        return;
    g_autofree char *site = ns_url_site_from(origin);
    g_autofree char *top_origin = ns_url_origin_from(origin);
    const char *partition_key = (site && *site) ? site
                              : (top_origin ? top_origin : "");
    if (!*partition_key) return;
    g_autofree char *jar_path = ns_net_cookie_path_for_partition(partition_key);
    g_autofree char *js_path = ns_net_cookie_js_path_for_partition(partition_key);
#ifdef NS_CURL_HAVE_COOKIE_SITE
    g_autofree char *curl_site = ns_net_curl_site_from(origin);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, jar_path);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, js_path);
    if (curl_site && *curl_site)
        curl_easy_setopt(curl, CURLOPT_COOKIE_SITE, curl_site);
#else
    g_autofree char *cookie = ns_cookie_collect(url, jar_path, js_path, TRUE,
                                                ns_url_is_same_site(url, origin)
                                                    ? 0 : 2);
    if (cookie && *cookie)
        curl_easy_setopt(curl, CURLOPT_COOKIE, cookie);
#endif
}

static char *
ns_net_cookies_for_request(const char *url, const char *jar_path,
                           const char *js_path, int samesite_cross)
{
    return ns_cookie_collect(url, jar_path, js_path, TRUE, samesite_cross);
}

static gboolean
ns_cookie_jar_has_httponly(const char *jar_path, const char *domain,
                           const char *path, const char *name, gint64 now)
{
    char *contents = NULL;
    if (!jar_path || !g_file_get_contents(jar_path, &contents, NULL, NULL))
        return FALSE;
    gboolean found = FALSE;
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i] && !found; i++) {
        if (!g_str_has_prefix(lines[i], "#HttpOnly_")) continue;
        char **f = g_strsplit(lines[i] + strlen("#HttpOnly_"), "\t", 7);
        int nf = 0;
        while (f[nf]) nf++;
        if (nf >= 7) {
            gint64 expiry = g_ascii_strtoll(f[4], NULL, 10);
            found = g_ascii_strcasecmp(f[0], domain) == 0 &&
                    strcmp(f[2], path) == 0 &&
                    strcmp(f[5], name) == 0 &&
                    (expiry == 0 || expiry >= now);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(contents);
    return found;
}

static GMutex ns_cookie_write_mutex;
static gint ns_cookie_writes;

static int
ns_cookie_jars_lock(void)
{
    g_autofree char *path = g_build_filename(ns_net_cookie_dir(), ".lock", NULL);
    int fd = g_open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
#ifdef G_OS_WIN32
    OVERLAPPED whole = {0};
    if (!LockFileEx((HANDLE)_get_osfhandle(fd), LOCKFILE_EXCLUSIVE_LOCK, 0,
                    1, 0, &whole)) {
        close(fd);
        return -1;
    }
#else
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return -1;
    }
#endif
    return fd;
}

static void
ns_cookie_jars_unlock(int fd)
{
    if (fd < 0) return;
#ifdef G_OS_WIN32
    OVERLAPPED whole = {0};
    UnlockFileEx((HANDLE)_get_osfhandle(fd), 0, 1, 0, &whole);
#endif
    close(fd);
}

gboolean
ns_net_cookie_jar_version(const char *url, const char *top_url,
                          guint64 *version)
{
    if (!url) return FALSE;
    g_autofree char *site = ns_cookie_js_partition(url, top_url);
    if (!site) {
        *version = G_MAXUINT64;
        return TRUE;
    }
    if (!*site) return FALSE;
    g_autofree char *jar_path = ns_net_cookie_path_for_partition(site);
    g_autofree char *js_path = ns_net_cookie_js_path_for_partition(site);
    guint64 v = (guint64)(guint)g_atomic_int_get(&ns_cookie_writes) + 1;
    const char *paths[] = { jar_path, js_path };
    for (gsize i = 0; i < G_N_ELEMENTS(paths); i++) {
        GStatBuf st;
        if (g_stat(paths[i], &st) != 0) continue;
        v = v * 1000003u ^ (guint64)st.st_size;
        v = v * 1000003u ^ (guint64)st.st_mtime;
#if defined(__linux__)
        v = v * 1000003u ^ (guint64)st.st_mtim.tv_nsec;
#endif
    }
    *version = v ? v : 1;
    return TRUE;
}

static void
ns_cookie_store_impl(const char *url, const char *partition_site,
                     const char *cookie, gboolean from_http,
                     const char *jar_override)
{
    if (!url || !*url || !cookie) return;
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://"))
        return;
    for (const char *c = cookie; *c; c++)
        if ((unsigned char)*c < 0x20) return;
    g_autoptr(ns_url_parts) parts = ns_url_parts_new(url);
    if (!parts || !parts->hostname || !*parts->hostname) return;

    const char *semi = strchr(cookie, ';');
    const char *pair_end = semi ? semi : cookie + strlen(cookie);
    const char *eq = memchr(cookie, '=', (gsize)(pair_end - cookie));
    if (!eq || eq == cookie) return;
    const char *name = cookie;
    gsize name_len = (gsize)(eq - cookie);
    const char *value = eq + 1;
    gsize value_len = (gsize)(pair_end - value);
    while (name_len && (name[0] == ' ' || name[0] == '\t')) { name++; name_len--; }
    while (name_len && (name[name_len - 1] == ' ' || name[name_len - 1] == '\t'))
        name_len--;
    while (value_len && (value[0] == ' ' || value[0] == '\t')) { value++; value_len--; }
    while (value_len && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t'))
        value_len--;
    if (name_len == 0) return;

    char *domain_attr = NULL, *path_attr = NULL;
    gboolean secure = FALSE, has_expiry = FALSE, expired = FALSE;
    gboolean httponly = FALSE;
    int samesite = NS_COOKIE_SAMESITE_LAX;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gint64 expiry = 0;
    for (const char *p = semi; p && *p; ) {
        while (*p == ';' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *e = strchr(p, ';');
        gsize alen = e ? (gsize)(e - p) : strlen(p);
        while (alen && (p[alen - 1] == ' ' || p[alen - 1] == '\t')) alen--;
        const char *aeq = memchr(p, '=', alen);
        gsize klen = aeq ? (gsize)(aeq - p) : alen;
        const char *av = aeq ? aeq + 1 : NULL;
        gsize avlen = aeq ? (gsize)(p + alen - av) : 0;
        while (avlen && (av[0] == ' ' || av[0] == '\t')) { av++; avlen--; }
        if (klen == 6 && g_ascii_strncasecmp(p, "secure", 6) == 0) {
            secure = TRUE;
        } else if (klen == 8 && g_ascii_strncasecmp(p, "httponly", 8) == 0) {
            httponly = TRUE;
        } else if (klen == 8 && g_ascii_strncasecmp(p, "samesite", 8) == 0 && aeq) {
            if (avlen == 4 && g_ascii_strncasecmp(av, "none", 4) == 0)
                samesite = NS_COOKIE_SAMESITE_NONE;
            else if (avlen == 6 && g_ascii_strncasecmp(av, "strict", 6) == 0)
                samesite = NS_COOKIE_SAMESITE_STRICT;
            else
                samesite = NS_COOKIE_SAMESITE_LAX;
        } else if (klen == 7 && g_ascii_strncasecmp(p, "max-age", 7) == 0 && aeq) {
            g_autofree char *tmp = g_strndup(av, avlen);
            char *end = NULL;
            gint64 ma = g_ascii_strtoll(tmp, &end, 10);
            if (end != tmp) {
                has_expiry = TRUE;
                expired = ma <= 0;
                if (ma > 0) expiry = now + ma;
            }
        } else if (klen == 7 && g_ascii_strncasecmp(p, "expires", 7) == 0 && aeq &&
                   !has_expiry) {
            g_autofree char *tmp = g_strndup(av, avlen);
            time_t t = curl_getdate(tmp, NULL);
            if (t != (time_t)-1) {
                has_expiry = TRUE;
                expiry = (gint64)t;
                if (expiry <= now) expired = TRUE;
            }
        } else if (klen == 6 && g_ascii_strncasecmp(p, "domain", 6) == 0 && aeq) {
            g_free(domain_attr);
            domain_attr = g_strndup(av, avlen);
        } else if (klen == 4 && g_ascii_strncasecmp(p, "path", 4) == 0 && aeq) {
            g_free(path_attr);
            path_attr = g_strndup(av, avlen);
        }
        if (!e) break;
        p = e + 1;
    }

    gboolean is_https = parts->protocol &&
                        g_ascii_strcasecmp(parts->protocol, "https:") == 0;
    if (secure && !is_https) { g_free(domain_attr); g_free(path_attr); return; }

    if (name_len >= 9 && g_ascii_strncasecmp(name, "__Secure-", 9) == 0 &&
        !(secure && is_https)) {
        g_free(domain_attr); g_free(path_attr); return;
    }
    if (name_len >= 7 && g_ascii_strncasecmp(name, "__Host-", 7) == 0 &&
        (!secure || !is_https || (domain_attr && *domain_attr) ||
         (path_attr && *path_attr && strcmp(path_attr, "/") != 0))) {
        g_free(domain_attr); g_free(path_attr); return;
    }

    const char *host = parts->hostname;
    char *file_domain;
    const char *tail;
    if (domain_attr && *domain_attr) {
        const char *d = domain_attr[0] == '.' ? domain_attr + 1 : domain_attr;
        gsize dl = strlen(d), hl = strlen(host);
        gboolean ok = g_ascii_strcasecmp(host, d) == 0 ||
                      (!g_hostname_is_ip_address(host) &&
                       hl > dl && host[hl - dl - 1] == '.' &&
                       g_ascii_strcasecmp(host + hl - dl, d) == 0);
        if (!ok || !dl) { g_free(domain_attr); g_free(path_attr); return; }
        g_autofree char *d_lower = g_ascii_strdown(d, -1);
        const psl_ctx_t *psl = psl_builtin();
        if (psl && psl_is_public_suffix(psl, d_lower)) {
            g_free(domain_attr); g_free(path_attr); return;
        }
        file_domain = g_strconcat(".", d, NULL);
        tail = "TRUE";
    } else {
        file_domain = g_strdup(host);
        tail = "FALSE";
    }
    g_autofree char *default_path = NULL;
    const char *path;
    if (path_attr && path_attr[0] == '/') {
        path = path_attr;
    } else {
        const char *request_path = parts->pathname;
        const char *last_slash = request_path ? strrchr(request_path, '/') : NULL;
        default_path = !last_slash || last_slash == request_path
            ? g_strdup("/")
            : g_strndup(request_path, (gsize)(last_slash - request_path));
        path = default_path;
    }

    g_autofree char *site = partition_site ? g_strdup(partition_site)
                                           : ns_url_site_from(url);
    if (!site || !*site) {
        g_free(file_domain); g_free(domain_attr); g_free(path_attr);
        return;
    }
    g_autofree char *jar_path = jar_override ? g_strdup(jar_override)
        : from_http ? ns_net_cookie_path_for_partition(site)
        : ns_net_cookie_js_path_for_partition(site);
    g_autofree char *name_dup = g_strndup(name, name_len);
    g_mutex_lock(&ns_cookie_write_mutex);
    int jars_lock = ns_cookie_jars_lock();
    if (!from_http) {
        g_autofree char *http_jar_path = ns_net_cookie_path_for_partition(site);
        if (ns_cookie_jar_has_httponly(http_jar_path, file_domain, path,
                                       name_dup, now)) {
            ns_cookie_jars_unlock(jars_lock);
            g_mutex_unlock(&ns_cookie_write_mutex);
            g_free(file_domain); g_free(domain_attr); g_free(path_attr);
            return;
        }
    }

    char *contents = NULL;
    g_file_get_contents(jar_path, &contents, NULL, NULL);
    GString *out = g_string_new(NULL);
    gboolean blocked_httponly = FALSE;
    char *attrs = NULL;
    if (contents) {
        char **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *line = lines[i];
            if (!*line) continue;
            if (g_str_has_prefix(line, "#curl:")) {
                g_free(attrs);
                attrs = g_strdup(line);
                continue;
            }
            if (line[0] == '#') {
                gboolean drop = FALSE;
                if (g_str_has_prefix(line, "#HttpOnly_")) {
                    char **hf = g_strsplit(line + 10, "\t", 7);
                    int hn = 0;
                    while (hf[hn]) hn++;
                    gboolean match = hn >= 7 &&
                        g_ascii_strcasecmp(hf[0], file_domain) == 0 &&
                        strcmp(hf[2], path) == 0 &&
                        strcmp(hf[5], name_dup) == 0;
                    g_strfreev(hf);
                    if (match) {
                        if (from_http) drop = TRUE;
                        else blocked_httponly = TRUE;
                    }
                }
                if (!drop) {
                    if (attrs && g_str_has_prefix(line, "#HttpOnly_")) {
                        g_string_append(out, attrs);
                        g_string_append_c(out, '\n');
                    }
                    g_string_append(out, line);
                    g_string_append_c(out, '\n');
                }
                g_clear_pointer(&attrs, g_free);
                continue;
            }
            char **f = g_strsplit(line, "\t", 7);
            int nf = 0;
            while (f[nf]) nf++;
            if (nf < 7) { g_strfreev(f); g_clear_pointer(&attrs, g_free); continue; }
            gint64 cexp = g_ascii_strtoll(f[4], NULL, 10);
            gboolean dup = g_ascii_strcasecmp(f[0], file_domain) == 0 &&
                           strcmp(f[2], path) == 0 &&
                           strcmp(f[5], name_dup) == 0;
            gboolean dead = cexp != 0 && cexp < now;
            if (!dup && !dead) {
                if (attrs) {
                    g_string_append(out, attrs);
                    g_string_append_c(out, '\n');
                }
                g_string_append(out, line);
                g_string_append_c(out, '\n');
            }
            g_clear_pointer(&attrs, g_free);
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(attrs);
    if (!expired && !blocked_httponly) {
        g_autofree char *vdup = g_strndup(value, value_len);
        const char *samesite_name =
            samesite == NS_COOKIE_SAMESITE_NONE ? "None"
            : samesite == NS_COOKIE_SAMESITE_STRICT ? "Strict" : NULL;
        if (samesite_name)
            g_string_append_printf(out, "#curl: SameSite=%s\n", samesite_name);
        g_string_append_printf(out,
            "%s%s\t%s\t%s\t%s\t%" G_GINT64_FORMAT "\t%s\t%s\n",
            (from_http && httponly) ? "#HttpOnly_" : "",
            file_domain, tail, path, secure ? "TRUE" : "FALSE",
            expiry, name_dup, vdup);
    }
    if (g_file_set_contents_full(jar_path, out->str, out->len,
                                 G_FILE_SET_CONTENTS_CONSISTENT |
                                 G_FILE_SET_CONTENTS_ONLY_EXISTING, 0600, NULL))
        g_chmod(jar_path, 0600);
    if (!blocked_httponly)
        ns_cookie_samesite_record(jar_path, file_domain, path, name_dup,
                                  samesite, expired);
    g_atomic_int_inc(&ns_cookie_writes);
    ns_cookie_jars_unlock(jars_lock);
    g_mutex_unlock(&ns_cookie_write_mutex);
    g_string_free(out, TRUE);
    g_free(file_domain);
    g_free(domain_attr);
    g_free(path_attr);
}

void
ns_net_cookie_store_from_js(const char *url, const char *top_url,
                            const char *cookie)
{
    g_autofree char *site = ns_cookie_js_partition(url, top_url);
    if (!site || !*site) return;
    ns_cookie_store_impl(url, site, cookie, FALSE, NULL);
}

static const char *
ns_net_altsvc_path(void)
{
    if (ns_net_is_private())
        return NULL;
    return ns_net_data_path(&g_altsvc_path, "altsvc.txt");
}

static long
ns_net_http_version(void)
{
#ifdef CURL_VERSION_HTTP3
    static gsize once = 0;
    static long version = CURL_HTTP_VERSION_2TLS;
    if (g_once_init_enter(&once)) {
        const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
        if (info && (info->features & CURL_VERSION_HTTP3) &&
            g_getenv("NS_FORCE_HTTP3"))
            version = CURL_HTTP_VERSION_3;
        g_once_init_leave(&once, 1);
    }
    return version;
#else
    return CURL_HTTP_VERSION_2TLS;
#endif
}

#define NS_NET_DOMAIN ns_net_error_quark()

static GQuark
ns_net_error_quark(void)
{
    return g_quark_from_static_string("nd-net-error");
}

static int
ns_xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
               curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    if (g_atomic_int_get(&g_net_aborting)) return 1;
    GCancellable *c = clientp;
    return (c && g_cancellable_is_cancelled(c)) ? 1 : 0;
}

extern const char *ns_app_self_exe(void);

char *
ns_net_exe_dir(void)
{
    const char *self = ns_app_self_exe();
    if (self && *self)
        return g_path_get_dirname(self);
#ifdef G_OS_WIN32
    DWORD cap = MAX_PATH;
    wchar_t *buf = g_new(wchar_t, cap);
    DWORD n = GetModuleFileNameW(NULL, buf, cap);
    while (n >= cap && cap < 32768) {
        cap *= 2;
        wchar_t *bigger = g_renew(wchar_t, buf, cap);
        buf = bigger;
        n = GetModuleFileNameW(NULL, buf, cap);
    }
    char *utf8 = NULL;
    if (n > 0 && n < cap)
        utf8 = g_utf16_to_utf8((gunichar2 *)buf, -1, NULL, NULL, NULL);
    g_free(buf);
    if (!utf8) return NULL;
    char *dir = g_path_get_dirname(utf8);
    g_free(utf8);
    return dir;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    if (size == 0 || size > 32768) return NULL;
    char *raw = g_malloc(size);
    if (_NSGetExecutablePath(raw, &size) != 0) { g_free(raw); return NULL; }
    char *real = realpath(raw, NULL);
    char *dir = g_path_get_dirname(real ? real : raw);
    free(real);
    g_free(raw);
    return dir;
#elif defined(__linux__)
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe) return NULL;
    char *dir = g_path_get_dirname(exe);
    g_free(exe);
    return dir;
#else
    return NULL;
#endif
}

static gboolean
ns_net_try_ca_bundle(const char *path)
{
    if (!path || !*path) return FALSE;
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;
    g_ca_bundle = g_strdup(path);
    return TRUE;
}

static void
ns_net_resolve_ca_bundle(void)
{
    if (g_ca_bundle) return;
    const char *env = g_getenv("CURL_CA_BUNDLE");
    if (!env) env = g_getenv("SSL_CERT_FILE");
    if (ns_net_try_ca_bundle(env)) return;

    char *dir = ns_net_exe_dir();
    if (dir) {
        const char *rels[] = {
            "etc/ssl/certs/ca-bundle.crt",
            "ssl/certs/ca-bundle.crt",
            "ca-bundle.crt",
            "cert.pem",
            "../etc/ca-certificates/cert.pem",
            "../etc/openssl@3/cert.pem",
            "../etc/openssl/cert.pem",
            NULL,
        };
        for (int i = 0; rels[i]; i++) {
            char *cand = g_build_filename(dir, rels[i], NULL);
            gboolean ok = ns_net_try_ca_bundle(cand);
            g_free(cand);
            if (ok) break;
        }
        g_free(dir);
        if (g_ca_bundle) return;
    }

#if defined(__ANDROID__)
    const char *android_paths[] = {
        "/system/etc/security/cacerts.pem",
        "/apex/com.android.conscrypt/cacerts.pem",
        "/data/misc/keychain/cacerts-added/cacert.pem",
        NULL,
    };
    for (int i = 0; android_paths[i]; i++)
        if (ns_net_try_ca_bundle(android_paths[i])) return;
    g_info("ns_net: no CA bundle found; the Android host app should set "
           "CURL_CA_BUNDLE to an extracted cacert.pem before ns_browser_init().");
#endif

#if (defined(__linux__) && !defined(__ANDROID__)) || defined(__FreeBSD__) || defined(__NetBSD__)
    const char *unix_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/var/lib/ca-certificates/ca-bundle.pem",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/etc/ssl/cert.pem",
        "/usr/local/share/certs/ca-root-nss.crt",
        NULL,
    };
    for (int i = 0; unix_paths[i]; i++)
        if (ns_net_try_ca_bundle(unix_paths[i])) return;
#endif

#ifdef __APPLE__
    const char *mac_paths[] = {
        "/opt/homebrew/etc/ca-certificates/cert.pem",
        "/opt/homebrew/etc/openssl@3/cert.pem",
        "/usr/local/etc/ca-certificates/cert.pem",
        "/usr/local/etc/openssl@3/cert.pem",
        "/usr/local/etc/openssl/cert.pem",
        "/etc/ssl/cert.pem",
        NULL,
    };
    for (int i = 0; mac_paths[i]; i++)
        if (ns_net_try_ca_bundle(mac_paths[i])) return;
#endif

#ifdef G_OS_WIN32
    const char *win_paths[] = {
        "C:/msys64/mingw64/etc/ssl/certs/ca-bundle.crt",
        "C:/msys64/mingw64/etc/ssl/cert.pem",
        "C:/msys64/ucrt64/etc/ssl/certs/ca-bundle.crt",
        "C:/msys64/clang64/etc/ssl/certs/ca-bundle.crt",
        NULL,
    };
    for (int i = 0; win_paths[i]; i++)
        if (ns_net_try_ca_bundle(win_paths[i])) return;

    g_info("ns_net: no CA bundle file found; relying on "
           "CURLSSLOPT_NATIVE_CA via the Windows certificate store. "
           "If HTTPS fails, install mingw-w64-x86_64-ca-certificates or "
           "set CURL_CA_BUNDLE.");
#endif
}

static void
ns_share_lock(CURL *handle, curl_lock_data data,
              curl_lock_access access, void *user_data)
{
    (void)handle; (void)access; (void)user_data;
    if (data < CURL_LOCK_DATA_LAST)
        g_mutex_lock(&g_share_locks[data]);
}

static void
ns_share_unlock(CURL *handle, curl_lock_data data, void *user_data)
{
    (void)handle; (void)user_data;
    if (data < CURL_LOCK_DATA_LAST)
        g_mutex_unlock(&g_share_locks[data]);
}

static gpointer
ns_rng_warmup_thread(gpointer data)
{
    (void)data;
    int (*rand_bytes)(unsigned char *, int) = NULL;
    GModule *self = g_module_open(NULL, G_MODULE_BIND_LAZY);
    if (self &&
        g_module_symbol(self, "RAND_bytes", (gpointer *)&rand_bytes) &&
        rand_bytes) {
        unsigned char buf[32];
        rand_bytes(buf, (int)sizeof buf);
    }
    if (self) g_module_close(self);
    return NULL;
}

static GThread *g_rng_warmup_thread;

static void
ns_net_warm_rng(void)
{
    if (!g_module_supported()) return;
    g_rng_warmup_thread = g_thread_try_new("nd-rng-warmup",
                                           ns_rng_warmup_thread, NULL, NULL);
}

static void
ns_net_join_rng(void)
{
    if (g_rng_warmup_thread) {
        g_thread_join(g_rng_warmup_thread);
        g_rng_warmup_thread = NULL;
    }
}

void
ns_net_init(void)
{
    ns_net_resolve_ca_bundle();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);

    unsigned ossl_major = 0, ossl_minor = 0;
    if (vi && vi->ssl_version &&
        sscanf(vi->ssl_version, "OpenSSL/%u.%u", &ossl_major, &ossl_minor) == 2 &&
        (ossl_major > 3 || (ossl_major == 3 && ossl_minor >= 5)))
        g_ec_curves = "X25519MLKEM768:X25519:P-256:P-384";

    g_free(g_accept_encoding);
    g_accept_encoding = g_strdup(ns_content_decode_accept_encoding());

    g_share = curl_share_init();
    if (g_share) {
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC,   ns_share_lock);
        curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, ns_share_unlock);
    }

    ns_net_warm_rng();

    ns_net_hsts_curl_path();
    ns_net_altsvc_path();
    ns_net_cookie_dir();
}

gboolean
ns_net_idle(void)
{
    g_mutex_lock(&g_fetch_throttle_mutex);
    gboolean idle = g_fetch_active == 0 && g_preconnect_active == 0;
    g_mutex_unlock(&g_fetch_throttle_mutex);
    return idle;
}

static void ns_fetch_task_abandon(GTask *task, const GError *err);

static gboolean
ns_net_drain(int timeout_ms)
{
    g_atomic_int_set(&g_net_aborting, 1);
    GQueue dropped = G_QUEUE_INIT;
    g_mutex_lock(&g_fetch_throttle_mutex);
    for (GTask *t; (t = g_queue_pop_head(&g_fetch_queue)); )
        g_queue_push_tail(&dropped, t);
    g_mutex_unlock(&g_fetch_throttle_mutex);
    for (GTask *t; (t = g_queue_pop_head(&dropped)); ) {
        GError *err = g_error_new_literal(NS_NET_DOMAIN, 1, "shutting down");
        ns_fetch_task_abandon(t, err);
        g_task_return_error(t, err);
        g_object_unref(t);
    }
    g_mutex_lock(&g_fetch_throttle_mutex);
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    gboolean drained = TRUE;
    while (g_fetch_active > 0 || g_preconnect_active > 0) {
        if (!g_cond_wait_until(&g_fetch_idle_cond, &g_fetch_throttle_mutex,
                               deadline)) {
            drained = g_fetch_active == 0 && g_preconnect_active == 0;
            break;
        }
    }
    g_mutex_unlock(&g_fetch_throttle_mutex);
    return drained;
}

void
ns_net_shutdown(void)
{
    ns_net_join_rng();
    ns_net_multi_shutdown();
    if (!ns_net_drain(3000))
        return;
    if (g_conn_stats) {
        g_hash_table_destroy(g_conn_stats);
        g_conn_stats = NULL;
    }
    if (g_share) { curl_share_cleanup(g_share); g_share = NULL; }
    curl_global_cleanup();
    g_free(g_accept_encoding);
    g_accept_encoding = NULL;
    g_free(g_proxy_override);
    g_proxy_override = NULL;
    g_free(g_cookie_dir);
    g_cookie_dir = NULL;
    g_free(g_hsts_curl_path);
    g_hsts_curl_path = NULL;
    g_free(g_altsvc_path);
    g_altsvc_path = NULL;
    if (g_private_root) {
        ns_rmrf(g_private_root);
        g_free(g_private_root);
        g_private_root = NULL;
    }
    g_free(g_ca_bundle);
    g_ca_bundle = NULL;
    if (g_hsts_cache) {
        g_hash_table_destroy(g_hsts_cache);
        g_hsts_cache = NULL;
    }
    if (g_origin_slots) {
        g_hash_table_destroy(g_origin_slots);
        g_origin_slots = NULL;
    }
}

void
ns_net_set_proxy_override(const char *proxy_url)
{
    g_free(g_proxy_override);
    g_proxy_override = (proxy_url && *proxy_url) ? g_strdup(proxy_url) : NULL;
}

static gboolean g_allow_file_urls = FALSE;

void
ns_net_set_allow_file_urls(gboolean allow)
{
    g_allow_file_urls = allow;
}

static gboolean g_log_fetches = FALSE;

void
ns_net_set_log_fetches(gboolean on)
{
    g_log_fetches = on;
}

typedef struct ns_conn_stat {
    guint64 requests;
    guint64 connections;
} ns_conn_stat;

static const char *
ns_net_http_version_name(long v)
{
    switch (v) {
    case CURL_HTTP_VERSION_1_0: return "http/1.0";
    case CURL_HTTP_VERSION_1_1: return "http/1.1";
    case CURL_HTTP_VERSION_2_0: return "h2";
#ifdef CURL_HTTP_VERSION_3
    case CURL_HTTP_VERSION_3:   return "h3";
#endif
    default:                    return "http/?";
    }
}

static GMutex   g_perf_lock;
static guint64  g_perf_fetch_count;
static guint64  g_perf_fetch_bytes;
static gint64   g_perf_fetch_sum_us;
static gint64   g_perf_fetch_first_us;
static gint64   g_perf_fetch_last_us;

static void
ns_net_perf_record(gint64 start_us, gint64 end_us, guint64 bytes)
{
    if (!g_log_fetches) return;
    g_mutex_lock(&g_perf_lock);
    if (g_perf_fetch_count == 0 || start_us < g_perf_fetch_first_us)
        g_perf_fetch_first_us = start_us;
    if (end_us > g_perf_fetch_last_us)
        g_perf_fetch_last_us = end_us;
    g_perf_fetch_count++;
    g_perf_fetch_bytes += bytes;
    g_perf_fetch_sum_us += end_us - start_us;
    g_mutex_unlock(&g_perf_lock);
}

void
ns_net_perf_snapshot(guint64 *fetches, guint64 *bytes,
                     double *sum_ms, double *span_ms)
{
    g_mutex_lock(&g_perf_lock);
    if (fetches) *fetches = g_perf_fetch_count;
    if (bytes)   *bytes   = g_perf_fetch_bytes;
    if (sum_ms)  *sum_ms  = g_perf_fetch_sum_us / 1000.0;
    if (span_ms) *span_ms = g_perf_fetch_count
                            ? (g_perf_fetch_last_us - g_perf_fetch_first_us) / 1000.0
                            : 0.0;
    g_mutex_unlock(&g_perf_lock);
}

static void
ns_net_conn_stat_record(const char *url, long http_version, long new_connections)
{
    if (!g_log_fetches) return;
    char *origin = ns_url_origin_from(url);
    if (!origin) return;

    g_mutex_lock(&g_conn_stats_lock);
    if (!g_conn_stats)
        g_conn_stats = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
    ns_conn_stat *s = g_hash_table_lookup(g_conn_stats, origin);
    if (!s) {
        s = g_new0(ns_conn_stat, 1);
        g_hash_table_insert(g_conn_stats, g_strdup(origin), s);
    }
    s->requests++;
    if (new_connections > 0)
        s->connections += (guint64)new_connections;
    guint64 reqs = s->requests, conns = s->connections;
    g_mutex_unlock(&g_conn_stats_lock);

    ns_debug_log_emit(NS_DLOG_NET, "conn", "%s new=%ld origin=%s reqs=%"
                      G_GUINT64_FORMAT " conns=%" G_GUINT64_FORMAT,
                      ns_net_http_version_name(http_version),
                      new_connections, origin, reqs, conns);
    g_free(origin);
}

static const char *
ns_net_pick_configured_proxy(const char *url)
{
    if (g_proxy_override && *g_proxy_override) return g_proxy_override;
    const ns_config *cfg = ns_config_get();
    if (!cfg) return NULL;
    gboolean https = g_str_has_prefix(url, "https://") ||
                     g_str_has_prefix(url, "wss://");
    if (https && cfg->https_proxy && *cfg->https_proxy) return cfg->https_proxy;
    if (cfg->http_proxy && *cfg->http_proxy)            return cfg->http_proxy;
    return NULL;
}

static const char *
ns_net_configured_no_proxy(void)
{
    const ns_config *cfg = ns_config_get();
    if (cfg && cfg->no_proxy && *cfg->no_proxy) return cfg->no_proxy;
    return NULL;
}

void
ns_net_apply_curl_proxy(void *curl_handle, const char *url)
{
    CURL *curl = curl_handle;
    const char *proxy = ns_net_pick_configured_proxy(url);
    if (proxy && *proxy)
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
    const char *no_proxy = ns_net_configured_no_proxy();
    if (no_proxy && *no_proxy)
        curl_easy_setopt(curl, CURLOPT_NOPROXY, no_proxy);
}

const char *
ns_net_proxy_override(void)
{
    return g_proxy_override;
}

const char *
ns_net_http_proxy(void)
{
    const ns_config *cfg = ns_config_get();
    return cfg ? cfg->http_proxy : NULL;
}

const char *
ns_net_https_proxy(void)
{
    const ns_config *cfg = ns_config_get();
    return cfg ? cfg->https_proxy : NULL;
}

const char *
ns_net_no_proxy(void)
{
    const ns_config *cfg = ns_config_get();
    return cfg ? cfg->no_proxy : NULL;
}

const char *
ns_net_ca_bundle_path(void)
{
    return g_ca_bundle;
}

void
ns_net_apply_curl_tls(void *curl_handle)
{
    CURL *curl = curl_handle;
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST,
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA:"
        "AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA:AES256-SHA");
    curl_easy_setopt(curl, CURLOPT_TLS13_CIPHERS,
        "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:"
        "TLS_CHACHA20_POLY1305_SHA256");
    curl_easy_setopt(curl, CURLOPT_SSL_EC_CURVES, g_ec_curves);
#if LIBCURL_VERSION_NUM >= 0x080800
    {
        const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
        const char *const *feat = info ? info->feature_names : NULL;
        for (; feat && *feat; feat++) {
            if (!g_ascii_strcasecmp(*feat, "ECH")) {
                curl_easy_setopt(curl, CURLOPT_ECH, "true");
                break;
            }
        }
    }
#endif
    if (g_ca_bundle) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_bundle);
#ifndef G_OS_WIN32
        curl_easy_setopt(curl, CURLOPT_CAPATH, NULL);
#endif
    }
#ifdef G_OS_WIN32
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
    const ns_config *cfg = ns_config_get();
    if (cfg && cfg->doh_url && g_str_has_prefix(cfg->doh_url, "https://"))
        curl_easy_setopt(curl, CURLOPT_DOH_URL, cfg->doh_url);
}

void
ns_response_free(ns_response *resp)
{
    if (!resp)
        return;
    g_free(resp->final_url);
    g_free(resp->content_type);
    g_free(resp->content_disposition);
    g_free(resp->csp_header);
    g_free(resp->xframe_options);
    g_free(resp->x_content_type_options);
    g_free(resp->cors_allow_origin);
    g_free(resp->refresh);
    g_free(resp->content_language);
    g_free(resp->raw_headers);
    if (resp->body)
        g_byte_array_unref(resp->body);
    g_free(resp->error);
    g_free(resp->tls_warning);
    g_free(resp->remote_ip);
    g_free(resp->next_hop_protocol);
    g_free(resp);
}

static ns_response *
ns_response_copy(const ns_response *src)
{
    if (!src) return NULL;
    ns_response *r = g_new0(ns_response, 1);
    *r = *src;
    r->final_url = g_strdup(src->final_url);
    r->content_type = g_strdup(src->content_type);
    r->content_disposition = g_strdup(src->content_disposition);
    r->csp_header = g_strdup(src->csp_header);
    r->xframe_options = g_strdup(src->xframe_options);
    r->x_content_type_options = g_strdup(src->x_content_type_options);
    r->cors_allow_origin = g_strdup(src->cors_allow_origin);
    r->refresh = g_strdup(src->refresh);
    r->content_language = g_strdup(src->content_language);
    r->raw_headers = g_strdup(src->raw_headers);
    r->error = g_strdup(src->error);
    r->tls_warning = g_strdup(src->tls_warning);
    r->remote_ip = g_strdup(src->remote_ip);
    r->next_hop_protocol = g_strdup(src->next_hop_protocol);
    r->body = g_byte_array_new();
    if (src->body && src->body->len)
        g_byte_array_append(r->body, src->body->data, src->body->len);
    return r;
}

typedef struct {
    char   *method;
    char   *url;
    long    status;
    char   *content_type;
    guint64 body_len;
    double  duration_ms;
    char   *req_headers;
    char   *resp_headers;
    char   *error;
} ns_net_log_entry;

#define NS_NET_LOG_CAP 256

static GMutex      ns_net_log_lock;
static GPtrArray  *ns_net_log;

static void
ns_net_log_entry_free(gpointer data)
{
    ns_net_log_entry *e = data;
    if (!e)
        return;
    g_free(e->method);
    g_free(e->url);
    g_free(e->content_type);
    g_free(e->req_headers);
    g_free(e->resp_headers);
    g_free(e->error);
    g_free(e);
}

static void
ns_net_log_record(const char *method, const char *url, long status,
                  const char *content_type, guint64 body_len,
                  double duration_ms, const char *req_headers,
                  const char *resp_headers, const char *error)
{
    if (!url || !*url)
        return;
    if (g_str_has_prefix(url, "data:") || g_str_has_prefix(url, "about:"))
        return;
    ns_net_log_entry *e = g_new0(ns_net_log_entry, 1);
    e->method = g_strdup(method && *method ? method : "GET");
    e->url = g_strdup(url);
    e->status = status;
    e->content_type = g_strdup(content_type ? content_type : "");
    e->body_len = body_len;
    e->duration_ms = duration_ms;
    e->req_headers = g_strdup(req_headers ? req_headers : "");
    e->resp_headers = g_strdup(resp_headers ? resp_headers : "");
    e->error = error && *error ? g_strdup(error) : NULL;

    g_mutex_lock(&ns_net_log_lock);
    if (!ns_net_log)
        ns_net_log = g_ptr_array_new_with_free_func(ns_net_log_entry_free);
    if (ns_net_log->len >= NS_NET_LOG_CAP)
        g_ptr_array_remove_index(ns_net_log, 0);
    g_ptr_array_add(ns_net_log, e);
    g_mutex_unlock(&ns_net_log_lock);
}

void
ns_net_log_clear(void)
{
    g_mutex_lock(&ns_net_log_lock);
    if (ns_net_log)
        g_ptr_array_set_size(ns_net_log, 0);
    g_mutex_unlock(&ns_net_log_lock);
}

static void
ns_net_log_append_headers(GString *out, const char *headers)
{
    if (!headers || !*headers)
        return;
    char **lines = g_strsplit(headers, "\n", -1);
    for (guint i = 0; lines && lines[i]; i++) {
        char *line = g_strchomp(lines[i]);
        if (*line)
            g_string_append_printf(out, "    %s\n", line);
    }
    g_strfreev(lines);
}

char *
ns_net_log_dump(void)
{
    GString *out = g_string_new(NULL);
    g_mutex_lock(&ns_net_log_lock);
    guint n = ns_net_log ? ns_net_log->len : 0;
    g_string_append_printf(out, "%u network request%s\n\n", n,
                           n == 1 ? "" : "s");
    for (guint i = 0; i < n; i++) {
        ns_net_log_entry *e = g_ptr_array_index(ns_net_log, i);
        if (e->status > 0)
            g_string_append_printf(out, "[%ld] %s %s\n", e->status,
                                   e->method, e->url);
        else
            g_string_append_printf(out, "[---] %s %s\n", e->method, e->url);
        g_string_append_printf(out, "    %.0f ms, %llu bytes",
                               e->duration_ms,
                               (unsigned long long)e->body_len);
        if (e->content_type && *e->content_type)
            g_string_append_printf(out, ", %s", e->content_type);
        g_string_append_c(out, '\n');
        if (e->error)
            g_string_append_printf(out, "    error: %s\n", e->error);
        if (e->req_headers && *e->req_headers) {
            g_string_append(out, "  Request headers:\n");
            ns_net_log_append_headers(out, e->req_headers);
        }
        if (e->resp_headers && *e->resp_headers) {
            g_string_append(out, "  Response headers:\n");
            ns_net_log_append_headers(out, e->resp_headers);
        }
        g_string_append_c(out, '\n');
    }
    g_mutex_unlock(&ns_net_log_lock);
    return g_string_free(out, FALSE);
}

static char *
ns_net_slist_serialize(struct curl_slist *list)
{
    if (!list)
        return NULL;
    GString *out = g_string_new(NULL);
    for (struct curl_slist *n = list; n; n = n->next) {
        if (!n->data)
            continue;
        if (g_ascii_strncasecmp(n->data, "X-ND-", 5) == 0)
            continue;
        g_string_append(out, n->data);
        g_string_append_c(out, '\n');
    }
    return g_string_free(out, FALSE);
}

#define NS_NET_RESPONSE_MIN_BUDGET (64ULL * 1024ULL * 1024ULL)
#define NS_NET_RESPONSE_RECHECK_BYTES (16ULL * 1024ULL * 1024ULL)
#define NS_NET_MAX_RAW_HEADER_BYTES   (1ULL * 1024ULL * 1024ULL)

static guint64
ns_net_available_memory_bytes(void)
{
#if defined(G_OS_WIN32)
    MEMORYSTATUSEX m = { .dwLength = sizeof(m) };
    if (GlobalMemoryStatusEx(&m))
        return (guint64)m.ullAvailPhys;
#elif defined(__linux__)
    FILE *f = fopen("/proc/meminfo", "re");
    if (f) {
        char line[256];
        guint64 kb = 0;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemAvailable: %" G_GUINT64_FORMAT " kB", &kb) == 1) {
                fclose(f);
                return kb * 1024ULL;
            }
        }
        fclose(f);
    }
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof mem;
    if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) == 0 && mem > 0)
        return (guint64)mem;
#elif defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE)
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0)
        return (guint64)pages * (guint64)psize;
#endif
    return 0;
}

static guint64
ns_net_response_budget(void)
{
    guint64 avail = ns_net_available_memory_bytes();
    if (avail == 0) return NS_NET_RESPONSE_MIN_BUDGET;
    guint64 half = avail / 2;
    return half < NS_NET_RESPONSE_MIN_BUDGET ? NS_NET_RESPONSE_MIN_BUDGET : half;
}

struct ns_net_stream {
    ns_net_chunk_fn fn;
    gpointer        user_data;
    GMainContext   *main_ctx;
    gboolean        discard_body;
};

typedef struct {
    const struct ns_net_stream *stream;
    long    status;
    char   *raw_headers;
    GBytes *data;
} ns_net_stream_chunk;

static __thread const struct ns_net_stream *g_hop_stream;

static gboolean
ns_net_stream_chunk_dispatch(gpointer user_data)
{
    ns_net_stream_chunk *c = user_data;
    gsize len = 0;
    const guint8 *data = g_bytes_get_data(c->data, &len);
    c->stream->fn(c->stream->user_data, c->status, c->raw_headers, data, len);
    return G_SOURCE_REMOVE;
}

static void
ns_net_stream_chunk_free(gpointer user_data)
{
    ns_net_stream_chunk *c = user_data;
    g_free(c->raw_headers);
    g_bytes_unref(c->data);
    g_free(c);
}

static void
ns_net_stream_emit(const ns_write_ctx *ctx, const guint8 *data, gsize len)
{
    const struct ns_header_ctx *hc = ctx->stream_headers;
    long status = hc ? hc->status : 0;
    if (status < 200 || status >= 300) return;
    ns_net_stream_chunk *c = g_new0(ns_net_stream_chunk, 1);
    c->stream = ctx->stream;
    c->status = status;
    c->raw_headers = g_strdup(hc->raw ? hc->raw->str : "");
    c->data = g_bytes_new(data, len);
    GSource *source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(source, ns_net_stream_chunk_dispatch, c,
                          ns_net_stream_chunk_free);
    g_source_attach(source, ctx->stream->main_ctx);
    g_source_unref(source);
}

static gboolean
ns_write_body(ns_write_ctx *ctx, const guint8 *data, size_t bytes)
{
    if (ctx->stream && ctx->stream->discard_body) {
        ctx->total += bytes;
        ns_net_stream_emit(ctx, data, bytes);
        return TRUE;
    }
    if (ctx->total >= ctx->next_recheck) {
        ctx->budget = ns_net_response_budget();
        ctx->next_recheck = ctx->total + NS_NET_RESPONSE_RECHECK_BYTES;
    }
    if (ctx->total + bytes > ctx->budget) {
        ctx->exceeded = TRUE;
        return FALSE;
    }
    if (ctx->total + bytes > G_MAXUINT) {
        ctx->exceeded = TRUE;
        return FALSE;
    }
    g_byte_array_append(ctx->body, data, bytes);
    ctx->total += bytes;
    if (ctx->stream) ns_net_stream_emit(ctx, data, bytes);
    return TRUE;
}

static void
ns_write_sync_decoder(ns_write_ctx *ctx)
{
    const struct ns_header_ctx *hc = ctx->stream_headers;
    if (!hc || ctx->decoder_seq == hc->response_seq) return;
    ctx->decoder_seq = hc->response_seq;
    g_clear_pointer(&ctx->decoder, ns_content_decoder_free);
    ctx->decoder = ns_content_decoder_new(hc->content_encoding);
}

static size_t
ns_write_cb(char *data, size_t size, size_t nmemb, void *userdata)
{
    ns_write_ctx *ctx = userdata;
    if (size != 0 && nmemb > G_MAXSIZE / size)
        return 0;
    size_t bytes = size * nmemb;

    if (bytes == 0)
        return 0;
    if (bytes > G_MAXUINT)
        return 0;
    ns_write_sync_decoder(ctx);
    if (!ctx->decoder)
        return ns_write_body(ctx, (const guint8 *)data, bytes) ? bytes : 0;
    if (!ctx->decoded) ctx->decoded = g_byte_array_new();
    g_byte_array_set_size(ctx->decoded, 0);
    guint64 room = MIN(ctx->budget, (guint64)G_MAXUINT);
    room = room > ctx->total ? room - ctx->total : 0;
    if (!ns_content_decoder_feed(ctx->decoder, (const guint8 *)data, bytes,
                                 (gsize)room, ctx->decoded)) {
        if (ns_content_decoder_over_limit(ctx->decoder)) ctx->exceeded = TRUE;
        else ctx->decode_failed = TRUE;
        return 0;
    }
    if (ctx->decoded->len &&
        !ns_write_body(ctx, ctx->decoded->data, ctx->decoded->len))
        return 0;
    return bytes;
}

static void
ns_write_ctx_clear(ns_write_ctx *ctx)
{
    g_clear_pointer(&ctx->decoder, ns_content_decoder_free);
    if (ctx->decoded) g_byte_array_free(ctx->decoded, TRUE);
    ctx->decoded = NULL;
}

static void
ns_body_sink_init(ns_write_ctx *ctx, GByteArray *body)
{
    ctx->body = body;
    ctx->total = 0;
    ctx->budget = ns_net_response_budget();
    ctx->next_recheck = NS_NET_RESPONSE_RECHECK_BYTES;
    ctx->exceeded = FALSE;
    ctx->stream = NULL;
    ctx->stream_headers = NULL;
    ctx->decoder = NULL;
    ctx->decoder_seq = 0;
    ctx->decode_failed = FALSE;
    ctx->decoded = NULL;
}

static char *
header_value_dup(const char *line, size_t bytes, size_t prefix_len)
{
    const char *v = line + prefix_len;
    size_t vlen = bytes - prefix_len;
    while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
    while (vlen > 0 &&
           (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' ||
            v[vlen - 1] == ' '  || v[vlen - 1] == '\t')) vlen--;
    return g_strndup(v, vlen);
}

static gboolean
header_capture(const char *buffer, size_t bytes,
               const char *name, char **slot)
{
    size_t name_len = strlen(name);
    if (bytes < name_len ||
        g_ascii_strncasecmp(buffer, name, name_len) != 0)
        return FALSE;
    if (slot) {
        g_free(*slot);
        *slot = header_value_dup(buffer, bytes, name_len);
    }
    return TRUE;
}

static gboolean
header_append(const char *buffer, size_t bytes,
              const char *name, char **slot)
{
    size_t name_len = strlen(name);
    if (bytes < name_len ||
        g_ascii_strncasecmp(buffer, name, name_len) != 0)
        return FALSE;
    if (slot) {
        char *val = header_value_dup(buffer, bytes, name_len);
        if (*slot && **slot && val && *val) {
            char *joined = g_strconcat(*slot, ", ", val, NULL);
            g_free(*slot);
            g_free(val);
            *slot = joined;
        } else if (val && *val) {
            g_free(*slot);
            *slot = val;
        } else {
            g_free(val);
        }
    }
    return TRUE;
}

static void
ns_header_store_cookie(ns_header_ctx *hc, const char *line, size_t len)
{
    char *url = NULL;
    if (!hc->cookie_curl || !hc->cookie_jar_path ||
        curl_easy_getinfo(hc->cookie_curl, CURLINFO_EFFECTIVE_URL, &url) != CURLE_OK ||
        !url)
        return;
    g_autofree char *value = g_strndup(line + 11, len - 11);
    ns_cookie_store_impl(url, NULL, g_strstrip(value), TRUE,
                         hc->cookie_jar_path);
}

#define NS_EARLY_HINTS_MAX_LINKS 32

static ns_net_early_hints_fn g_early_hints_handler;

void
ns_net_set_early_hints_handler(ns_net_early_hints_fn handler)
{
    g_early_hints_handler = handler;
}

static void
ns_net_link_hint_free(gpointer p)
{
    ns_net_link_hint *hint = p;
    if (!hint) return;
    g_free(hint->url);
    g_free(hint->rel);
    g_free(hint->as);
    g_free(hint);
}

static const char *
link_param_value(const char *p, const char *end, char **out)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    const char *start = p;
    if (p < end && *p == '"') {
        GString *val = g_string_new(NULL);
        for (p++; p < end && *p != '"'; p++) {
            if (*p == '\\' && p + 1 < end) p++;
            g_string_append_c(val, *p);
        }
        if (p < end) p++;
        *out = g_string_free(val, FALSE);
        return p;
    }
    while (p < end && *p != ';' && *p != ',') p++;
    const char *stop = p;
    while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t')) stop--;
    *out = g_strndup(start, (gsize)(stop - start));
    return p;
}

static void
link_header_collect(const char *value, const char *base_url, GPtrArray *out)
{
    const char *p = value;
    const char *end = value + strlen(value);
    while (p < end && out->len < NS_EARLY_HINTS_MAX_LINKS) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) p++;
        if (p >= end || *p != '<') break;
        const char *close = memchr(p, '>', (size_t)(end - p));
        if (!close) break;
        g_autofree char *href = g_strndup(p + 1, (gsize)(close - p - 1));
        p = close + 1;
        g_autofree char *rel = NULL;
        g_autofree char *as = NULL;
        while (p < end && *p != ',') {
            while (p < end && (*p == ' ' || *p == '\t' || *p == ';')) p++;
            const char *name = p;
            while (p < end && *p != '=' && *p != ';' && *p != ',') p++;
            const char *name_end = p;
            while (name_end > name &&
                   (name_end[-1] == ' ' || name_end[-1] == '\t')) name_end--;
            char *param = NULL;
            if (p < end && *p == '=')
                p = link_param_value(p + 1, end, &param);
            gsize nlen = (gsize)(name_end - name);
            if (nlen == 3 && g_ascii_strncasecmp(name, "rel", 3) == 0 && !rel)
                rel = g_steal_pointer(&param);
            else if (nlen == 2 && g_ascii_strncasecmp(name, "as", 2) == 0 && !as)
                as = g_steal_pointer(&param);
            g_free(param);
        }
        if (!rel || !*href || g_str_has_prefix(href, "data:")) continue;
        char *abs = ns_url_resolve(base_url, href);
        if (!abs) continue;
        if (!ns_url_is_http_or_https(abs)) {
            g_free(abs);
            continue;
        }
        ns_net_link_hint *hint = g_new0(ns_net_link_hint, 1);
        hint->url = abs;
        hint->rel = g_steal_pointer(&rel);
        hint->as  = g_steal_pointer(&as);
        g_ptr_array_add(out, hint);
    }
}

typedef struct {
    char      *document_url;
    GPtrArray *hints;
} ns_early_hints_batch;

static gboolean
ns_early_hints_dispatch(gpointer data)
{
    ns_early_hints_batch *batch = data;
    if (g_early_hints_handler)
        g_early_hints_handler(batch->document_url, batch->hints);
    g_ptr_array_free(batch->hints, TRUE);
    g_free(batch->document_url);
    g_free(batch);
    return G_SOURCE_REMOVE;
}

static void
ns_header_early_hint(ns_header_ctx *hc, const char *buffer, size_t bytes)
{
    if (!hc->early_hints || hc->http1 || hc->status != 103 ||
        !g_early_hints_handler)
        return;
    if (bytes == 2) {
        if (!hc->early_links || hc->early_links->len == 0) return;
        ns_early_hints_batch *batch = g_new0(ns_early_hints_batch, 1);
        batch->document_url = g_strdup(hc->url);
        batch->hints = g_steal_pointer(&hc->early_links);
        g_idle_add_full(G_PRIORITY_HIGH_IDLE, ns_early_hints_dispatch,
                        batch, NULL);
        return;
    }
    if (bytes < 5 || g_ascii_strncasecmp(buffer, "Link:", 5) != 0) return;
    if (!hc->early_links)
        hc->early_links = g_ptr_array_new_with_free_func(ns_net_link_hint_free);
    g_autofree char *value = header_value_dup(buffer, bytes, 5);
    if (value && *value) link_header_collect(value, hc->url, hc->early_links);
}

static size_t
ns_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    ns_header_ctx *hc = userdata;
    if (size != 0 && nitems > G_MAXSIZE / size)
        return 0;
    size_t bytes = size * nitems;

    if (bytes >= 5 && g_ascii_strncasecmp(buffer, "HTTP/", 5) == 0) {
        if (hc->raw) g_string_set_size(hc->raw, 0);
        g_clear_pointer(&hc->content_encoding, g_free);
        hc->response_seq++;
        const char *sp = memchr(buffer, ' ', bytes);
        hc->status = sp ? (long)g_ascii_strtoll(sp + 1, NULL, 10) : 0;
        hc->http1 = bytes >= 7 && g_ascii_strncasecmp(buffer, "HTTP/1.", 7) == 0;
        return bytes;
    }
    if (hc->status >= 100 && hc->status < 200) {
        ns_header_early_hint(hc, buffer, bytes);
        return bytes;
    }
    if (bytes > 2) {
        gboolean set_cookie =
            (bytes >= 11 && g_ascii_strncasecmp(buffer, "Set-Cookie:", 11) == 0) ||
            (bytes >= 12 && g_ascii_strncasecmp(buffer, "Set-Cookie2:", 12) == 0);
        if (!set_cookie) {
            if (!hc->raw) hc->raw = g_string_new(NULL);
            if (hc->raw->len + bytes <= NS_NET_MAX_RAW_HEADER_BYTES)
                g_string_append_len(hc->raw, buffer, bytes);
        }
    }

    if      (header_capture(buffer, bytes, "Content-Type:",    hc->content_type_out))         {}
    else if (header_capture(buffer, bytes, "ETag:",            &hc->etag))                    {}
    else if (header_capture(buffer, bytes, "Last-Modified:",   &hc->last_modified))           {}
    else if (header_capture(buffer, bytes, "Cache-Control:",   &hc->cache_control))           {}
    else if (header_capture(buffer, bytes, "Vary:",            &hc->vary))                    {}
    else if (header_capture(buffer, bytes, "Expires:",         &hc->expires))                 {}
    else if (header_capture(buffer, bytes, "Date:",            &hc->date))                    {}
    else if (header_capture(buffer, bytes, "Age:",             &hc->age))                     {}
    else if (header_append(buffer, bytes, "Content-Security-Policy:",
                            hc->csp_out))                                                     {}
    else if (header_capture(buffer, bytes, "X-Frame-Options:", hc->xframe_options_out))       {}
    else if (header_capture(buffer, bytes, "X-Content-Type-Options:",
                            hc->x_content_type_options_out))                                  {}
    else if (header_capture(buffer, bytes, "Access-Control-Allow-Origin:",
                            hc->cors_allow_origin_out))                                       {}
    else if (header_capture(buffer, bytes, "Content-Disposition:",
                            hc->content_disposition_out))                                     {}
    else if (header_capture(buffer, bytes, "Content-Language:", hc->content_language_out))      {}
    else if (header_capture(buffer, bytes, "Refresh:", hc->refresh_out))                       {}
    else if (header_capture(buffer, bytes, "Location:", &hc->location))                        {}
    else if (header_capture(buffer, bytes, "Content-Encoding:", &hc->content_encoding))        {}
    else if (header_capture(buffer, bytes, "Set-Cookie:", NULL)) {
        hc->set_cookie_seen = TRUE;
        ns_header_store_cookie(hc, buffer, bytes);
    }

    return bytes;
}

static char *
about_read_first(const char *const *rel_paths, gsize *out_len)
{
    const char *exe = ns_app_self_exe();
    char *exe_dir = exe ? g_path_get_dirname(exe) : g_strdup(".");
    const char *user_data_dir = g_get_user_data_dir();
    const char *const *system_data_dirs = g_get_system_data_dirs();
    char *contents = NULL;
    gsize len = 0;
    for (int i = 0; rel_paths[i]; i++) {
        char *path = g_build_filename(exe_dir, rel_paths[i], NULL);
        gboolean ok = g_file_get_contents(path, &contents, &len, NULL);
        g_free(path);
        if (ok) break;
        if (user_data_dir && *user_data_dir) {
            path = g_build_filename(user_data_dir, rel_paths[i], NULL);
            ok = g_file_get_contents(path, &contents, &len, NULL);
            g_free(path);
            if (ok) break;
        }
        for (int j = 0; system_data_dirs && system_data_dirs[j]; j++) {
            path = g_build_filename(system_data_dirs[j], rel_paths[i], NULL);
            ok = g_file_get_contents(path, &contents, &len, NULL);
            g_free(path);
            if (ok) break;
        }
        if (ok) break;
    }
    g_free(exe_dir);
    if (out_len) *out_len = contents ? len : 0;
    return contents;
}

static const char *
about_logo_data_uri(void)
{
    static char *cached = NULL;
    if (cached) return cached;

    static const char *const gif_paths[] = {
        "share/icons/hicolor/scalable/apps/nordstjernen.gif",
        "../share/icons/hicolor/scalable/apps/nordstjernen.gif",
        "../../data/icons/hicolor/scalable/apps/nordstjernen.gif",
        "data/icons/hicolor/scalable/apps/nordstjernen.gif",
        NULL,
    };
    gsize gif_len = 0;
    char *gif = about_read_first(gif_paths, &gif_len);
    if (gif) {
        gchar *b64 = g_base64_encode((const guchar *)gif, gif_len);
        g_free(gif);
        cached = g_strconcat("data:image/gif;base64,", b64, NULL);
        g_free(b64);
        return cached;
    }

    cached = g_strconcat("data:image/gif;base64,", about_logo_gif_b64, NULL);
    return cached;
}

static char *
about_logo_markup(void)
{
    return g_strdup_printf("<img class=\"mark-img\" src=\"%s\" "
                           "alt=\"Nordstjernen\" aria-hidden=\"true\">",
                           about_logo_data_uri());
}

static char *
about_substitute(const char *template_text,
                 const char *placeholder, const char *value)
{
    char **parts = g_strsplit(template_text, placeholder, -1);
    char *joined = g_strjoinv(value, parts);
    g_strfreev(parts);
    return joined;
}

static void
diag_kv(GString *s, const char *key, const char *value)
{
    char *ev = g_markup_escape_text((value && *value) ? value : "\xe2\x80\x94",
                                    -1);
    g_string_append_printf(s,
        "<div class=\"drow\"><span class=\"dk\">%s</span>"
        "<span class=\"dv\">%s</span></div>", key, ev);
    g_free(ev);
}

static void
diag_feature(GString *s, const char *key, gboolean on)
{
    g_string_append_printf(s,
        "<div class=\"drow\"><span class=\"dk\">%s</span>"
        "<span class=\"dv %s\">%s</span></div>",
        key, on ? "on" : "off", on ? "Enabled" : "Not built");
}

static void
diag_state(GString *s, const char *key, gboolean on, const char *text)
{
    char *et = g_markup_escape_text(text, -1);
    g_string_append_printf(s,
        "<div class=\"drow\"><span class=\"dk\">%s</span>"
        "<span class=\"dv %s\">%s</span></div>", key, on ? "on" : "off", et);
    g_free(et);
}

static char *
about_security_html(void)
{
    ns_security_status st;
    ns_security_get_status(&st);
    const ns_config *cfg = ns_config_get();
    GString *s = g_string_new("<div class=\"diag\">");

    g_string_append(s, "<h3>Process sandbox</h3>");
#if defined(__linux__)
    if (st.landlock_abi > 0) {
        char *abi = g_strdup_printf("Supported (ABI v%d)", st.landlock_abi);
        diag_state(s, "Kernel Landlock support", TRUE, abi);
        g_free(abi);
    } else {
        diag_state(s, "Kernel Landlock support", FALSE, "Unavailable");
    }
    if (st.sandbox_disabled)
        diag_state(s, "Filesystem sandbox (Landlock)", FALSE,
                   "Disabled by NS_NO_SANDBOX");
    else if (st.landlock_enforced && st.landlock_all_threads)
        diag_state(s, "Filesystem sandbox (Landlock)", TRUE,
                   "Enforced on all threads");
    else if (st.landlock_enforced)
        diag_state(s, "Filesystem sandbox (Landlock)", TRUE,
                   "Enforced on new threads only");
    else
        diag_state(s, "Filesystem sandbox (Landlock)", FALSE,
                   "Not enforced in this process");
#ifdef NS_HAVE_SECCOMP
    if (st.seccomp_enforced)
        diag_state(s, "Syscall filter (seccomp)", TRUE, "Enforced");
    else if (st.sandbox_disabled || st.seccomp_disabled)
        diag_state(s, "Syscall filter (seccomp)", FALSE,
                   st.sandbox_disabled ? "Disabled by NS_NO_SANDBOX"
                                       : "Disabled by NS_NO_SECCOMP");
    else
        diag_state(s, "Syscall filter (seccomp)", FALSE,
                   "Not enforced in this process");
#else
    diag_feature(s, "Syscall filter (seccomp)", FALSE);
#endif
    diag_state(s, "No new privileges", st.no_new_privs,
               st.no_new_privs ? "Set" : "Not set");
#elif defined(__APPLE__)
    if (st.sandbox_disabled)
        diag_state(s, "Filesystem sandbox (Seatbelt)", FALSE,
                   "Disabled by NS_NO_SANDBOX");
    else
        diag_state(s, "Filesystem sandbox (Seatbelt)", st.seatbelt_enforced,
                   st.seatbelt_enforced ? "Write-confined"
                                        : "Not enforced in this process");
#elif defined(G_OS_WIN32)
    diag_state(s, "Process mitigations", st.win32_mitigations,
               st.win32_mitigations
                   ? "ASLR, strict handles, no extension points, "
                     "no remote images"
                   : "Not applied");
    diag_state(s, "Child process creation", st.win32_child_processes_blocked,
               st.win32_child_processes_blocked ? "Blocked" : "Allowed");
#endif
#if defined(__linux__) || defined(__APPLE__)
    gboolean root = geteuid() == 0 || getuid() == 0;
    diag_state(s, "Running as root", !root,
               root ? "Yes (NS_ALLOW_ROOT)" : "No");
#endif
#ifdef __GLIBC__
    diag_state(s, "Hardened allocator", st.allocator_hardened,
               st.allocator_hardened ? "Freed memory is poisoned" : "Off");
#endif

    g_string_append(s, "<h3>Browsing</h3>");
    gboolean https_first = cfg && cfg->https_first;
    diag_state(s, "HTTPS-First", https_first, https_first ? "On" : "Off");
    gboolean js_on = !cfg || cfg->javascript_enabled;
    diag_state(s, "JavaScript", js_on, js_on ? "Enabled" : "Disabled");
    gboolean gpc = cfg && cfg->global_privacy_control;
    diag_state(s, "Global Privacy Control", gpc, gpc ? "Sent" : "Off");
    gboolean dnt = cfg && cfg->do_not_track;
    diag_state(s, "Do Not Track", dnt, dnt ? "Sent" : "Off");
    gboolean strip = cfg && cfg->strip_tracking_params;
    diag_state(s, "Tracking parameter removal", strip, strip ? "On" : "Off");
    diag_state(s, "Telemetry", TRUE, "None \xe2\x80\x94 never phones home");

    g_string_append(s, "</div>");
    return g_string_free(s, FALSE);
}

static char *
about_diagnostics_html(void)
{
    const char *platform =
#if defined(G_OS_WIN32)
        "Windows";
#elif defined(__APPLE__)
        "macOS";
#elif defined(__linux__)
        "Linux";
#else
        "Unknown";
#endif
    const char *arch =
#if defined(__x86_64__) || defined(_M_X64)
        "x86-64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        "arm64";
#elif defined(__i386__) || defined(_M_IX86)
        "x86";
#elif defined(__arm__)
        "arm";
#else
        "unknown";
#endif

    GString *s = g_string_new("<div class=\"diag\">");

    g_string_append(s, "<h3>System</h3>");
    char *os = g_get_os_info(G_OS_INFO_KEY_PRETTY_NAME);
    if (!os) os = g_get_os_info(G_OS_INFO_KEY_NAME);
    diag_kv(s, "Operating system", os ? os : platform);
    g_free(os);
    diag_kv(s, "Platform", platform);
    diag_kv(s, "Architecture", arch);
    char *cores = g_strdup_printf("%u", g_get_num_processors());
    diag_kv(s, "Logical CPUs", cores);
    g_free(cores);

    g_string_append(s, "<h3>Version &amp; libraries</h3>");
    diag_kv(s, "Nordstjernen", NS_VERSION " (built " NS_BUILD_DATE ")");
    {
        const ns_config *ua_cfg = ns_config_get();
        const char *ua = ua_cfg && ua_cfg->user_agent && *ua_cfg->user_agent
                       ? ua_cfg->user_agent
                       : ns_user_agent_for_mode(ua_cfg ? ua_cfg->compat_mode
                                                       : NULL);
        diag_kv(s, "User agent", ua);
    }
    diag_kv(s, "JavaScript engine", ns_js_engine_version());
#ifdef NS_LEXBOR_VERSION
    diag_kv(s, "HTML / CSS (lexbor)", NS_LEXBOR_VERSION);
#endif
    char *glibv = g_strdup_printf("%u.%u.%u", glib_major_version,
                                  glib_minor_version, glib_micro_version);
    diag_kv(s, "GLib", glibv);
    g_free(glibv);
    diag_kv(s, "Pango", ns_pango_version_string());
    diag_kv(s, "Cairo", cairo_version_string());
    diag_kv(s, "SQLite", sqlite3_libversion());
    int wv = WebPGetDecoderVersion();
    char *webpv = g_strdup_printf("%d.%d.%d", (wv >> 16) & 0xff,
                                  (wv >> 8) & 0xff, wv & 0xff);
    diag_kv(s, "libwebp", webpv);
    g_free(webpv);
#ifdef NS_HAVE_LIBAV
    diag_kv(s, "Video (FFmpeg libav*)", av_version_info());
#endif
    diag_kv(s, "TLS / crypto", OpenSSL_version(OPENSSL_VERSION));
    diag_kv(s, "Networking", curl_version());

    g_string_append(s, "<h3>Features</h3>");
#ifdef NS_ENABLE_WEBGL
    diag_feature(s, "WebGL (3D canvas)", TRUE);
#else
    diag_feature(s, "WebGL (3D canvas)", FALSE);
#endif
#ifdef ND_HAVE_WEBGPU
    diag_kv(s, "WebGPU",
            g_getenv("NS_WEBGPU_ALLOW") ? "Enabled (--enable-webgpu)"
            : ns_config_get() && ns_config_get()->webgpu_enabled
                ? "Enabled (Settings)"
                : "Built \xe2\x80\x94 enable it in Settings or with --enable-webgpu");
#else
    diag_feature(s, "WebGPU (experimental)", FALSE);
#endif
#ifdef NS_HAVE_LIBAV
    diag_feature(s, "WebM video (VP8 / VP9 / Opus)", TRUE);
#else
    diag_feature(s, "WebM video (VP8 / VP9 / Opus)", FALSE);
#endif
#ifdef NS_HAVE_AVIF
    diag_feature(s, "AVIF images", TRUE);
#else
    diag_feature(s, "AVIF images", FALSE);
#endif
#ifdef NS_HAVE_JXL
    diag_feature(s, "JPEG XL images", TRUE);
#else
    diag_feature(s, "JPEG XL images", FALSE);
#endif
    diag_feature(s, "SVG images", TRUE);
#ifdef NS_HAVE_POPPLER
    diag_feature(s, "Inline PDF viewer", TRUE);
#else
    diag_feature(s, "Inline PDF viewer", FALSE);
#endif
#ifdef NS_HAVE_ENCHANT
    diag_feature(s, "Spell checking", TRUE);
#else
    diag_feature(s, "Spell checking", FALSE);
#endif

    g_string_append(s, "</div>");
    return g_string_free(s, FALSE);
}

static char *
build_about_markdown_page(const char *const *paths, const char *title,
                          const char *back_href, const char *back_label,
                          const char *missing_file)
{
    char *text = about_read_first(paths, NULL);
    if (!text) {
        return g_strdup_printf(
            "<!doctype html><meta charset=utf-8>"
            "<title>%s</title>"
            "<p>%s is missing from the install \xe2\x80\x94 reinstall the "
            "package or copy <code>%s</code> next to the binary.</p>",
            title, missing_file, missing_file);
    }
    char *escaped = ns_html_escape_text(text);
    g_free(text);
    char *html = g_strconcat(
        "<!doctype html><html lang=\"en\"><head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"color-scheme\" content=\"light dark\">"
        "<title>", title, "</title>"
        "<style>" NS_ABOUT_BASE_CSS
        ".wrap{max-width:860px;margin:0 auto;padding:28px 24px 56px}"
        "h1{margin:22px 4px 20px;font-size:28px;letter-spacing:-.02em}"
        ".card{padding:26px 30px}"
        "pre{margin:0;white-space:pre-wrap;word-wrap:break-word;"
        "font-family:var(--mono);font-size:13px;line-height:1.65;"
        "color:var(--muted)}"
        "</style></head><body><main class=\"wrap\">"
        "<a class=\"crumb\" href=\"", back_href, "\">"
        "&larr; ", back_label, "</a>"
        "<h1>", title, "</h1>"
        "<div class=\"card\"><pre>", escaped, "</pre></div>"
        "</main></body></html>", NULL);
    g_free(escaped);
    return html;
}

static char *
build_about_license(void)
{
    static const char *const paths[] = {
        "nordstjernen/License.md",
        "share/nordstjernen/License.md",
        "../share/nordstjernen/License.md",
        "../../../License.md",
        "../../License.md",
        "License.md",
        NULL,
    };
    return build_about_markdown_page(paths, "Nordstjernen License",
                                     "about:nordstjernen", "About Nordstjernen Sol",
                                     "License.md");
}

static char *
build_about_gpl(void)
{
    static const char *const paths[] = {
        "nordstjernen/COPYING",
        "share/nordstjernen/COPYING",
        "../share/nordstjernen/COPYING",
        "../../../COPYING",
        "../../COPYING",
        "COPYING",
        NULL,
    };
    return build_about_markdown_page(paths,
                                     "GNU General Public License, version 3",
                                     "about:nordstjernen", "About Nordstjernen Sol",
                                     "COPYING");
}

static char *
build_about_third_party(void)
{
    static const char *const paths[] = {
        "nordstjernen/THIRD-PARTY-LICENSES.md",
        "share/nordstjernen/THIRD-PARTY-LICENSES.md",
        "../share/nordstjernen/THIRD-PARTY-LICENSES.md",
        "../../../THIRD-PARTY-LICENSES.md",
        "../../THIRD-PARTY-LICENSES.md",
        "THIRD-PARTY-LICENSES.md",
        NULL,
    };
    return build_about_markdown_page(paths, "Third-party software notices",
                                     "about:nordstjernen", "About Nordstjernen Sol",
                                     "THIRD-PARTY-LICENSES.md");
}

#define ERROR_ICON(paths) \
    "<svg width=\"30\" height=\"30\" viewBox=\"0 0 16 16\" fill=\"none\" " \
    "stroke=\"currentColor\" stroke-width=\"1.4\" " \
    "stroke-linecap=\"round\" stroke-linejoin=\"round\">" paths "</svg>"

#define ERROR_ICON_OFFLINE ERROR_ICON( \
    "<path d=\"M1.75 6.25a9 9 0 0 1 12.5 0M4 8.75a5.75 5.75 0 0 1 8" \
    " 0M6.25 11.1a2.5 2.5 0 0 1 3.5 0M2.75 2l10.5 12\"/>")

#define ERROR_ICON_SEARCH ERROR_ICON( \
    "<circle cx=\"7\" cy=\"7\" r=\"4.75\"/>" \
    "<path d=\"M10.5 10.5 14 14\"/>")

#define ERROR_ICON_BLOCKED ERROR_ICON( \
    "<circle cx=\"8\" cy=\"8\" r=\"5.75\"/>" \
    "<path d=\"M3.95 3.95l8.1 8.1\"/>")

#define ERROR_ICON_CLOCK ERROR_ICON( \
    "<circle cx=\"8\" cy=\"8\" r=\"5.75\"/>" \
    "<path d=\"M8 4.75V8l2.25 1.5\"/>")

#define ERROR_ICON_LOCK ERROR_ICON( \
    "<rect x=\"3.25\" y=\"7\" width=\"9.5\" height=\"7\" rx=\"1.75\"/>" \
    "<path d=\"M5.25 7V5.25a2.75 2.75 0 0 1 5.5 0V7\"/>")

#define ERROR_ICON_LINK ERROR_ICON( \
    "<path d=\"M6.75 9.25l2.5-2.5M7.25 4.5l1-1a2.47 2.47 0 0 1 3.5" \
    " 3.5l-1 1M8.75 11.5l-1 1a2.47 2.47 0 0 1-3.5-3.5l1-1\"/>")

#define ERROR_ICON_PAGE ERROR_ICON( \
    "<path d=\"M4 1.75h4.75L12 5v9.25H4zM8.5 1.75V5.25H12M6.25" \
    " 8.25l3.5 3.5M9.75 8.25l-3.5 3.5\"/>")

#define ERROR_ICON_SERVER ERROR_ICON( \
    "<rect x=\"2.75\" y=\"2.75\" width=\"10.5\" height=\"4.25\"" \
    " rx=\"1.25\"/>" \
    "<rect x=\"2.75\" y=\"9\" width=\"10.5\" height=\"4.25\"" \
    " rx=\"1.25\"/>" \
    "<path d=\"M5 4.9h.5M5 11.1h.5\"/>")

#define ERROR_ICON_ALERT ERROR_ICON( \
    "<path d=\"M8 2.25 14.25 13.25H1.75zM8 6.5v3M8 11.4v.1\"/>")

typedef struct ns_error_info {
    const char *icon;
    const char *title;
    const char *heading;
    const char *summary;
} ns_error_info;

static const ns_error_info *
classify_error(long status, const char *transport_error, gboolean is_file_url)
{
    static const ns_error_info NO_NETWORK = {
        ERROR_ICON_OFFLINE,
        "Can't reach the network",
        "Can't reach the network",
        "Nordstjernen couldn't connect to any server. Your device may be "
        "offline, or a firewall is blocking outbound traffic."
    };
    static const ns_error_info DNS = {
        ERROR_ICON_SEARCH,
        "Server address not found",
        "Server address not found",
        "Nordstjernen couldn't look up the host name. The address may be "
        "mistyped, or your DNS resolver isn't responding."
    };
    static const ns_error_info REFUSED = {
        ERROR_ICON_BLOCKED,
        "Server refused the connection",
        "Server refused the connection",
        "The host is reachable but no service is listening on that port, "
        "or it actively closed the connection."
    };
    static const ns_error_info TIMEOUT = {
        ERROR_ICON_CLOCK,
        "The connection timed out",
        "The connection timed out",
        "The server didn't respond within the allowed time. It may be "
        "overloaded or temporarily unreachable."
    };
    static const ns_error_info TLS = {
        ERROR_ICON_LOCK,
        "Secure connection failed",
        "Secure connection failed",
        "Nordstjernen couldn't establish a trustworthy TLS connection. "
        "The certificate may be invalid, expired, or self-signed."
    };
    static const ns_error_info BAD_URL = {
        ERROR_ICON_LINK,
        "That address looks malformed",
        "That address looks malformed",
        "The URL couldn't be parsed. Check for typos, missing slashes, "
        "or an unsupported scheme."
    };
    static const ns_error_info HTTP_404 = {
        ERROR_ICON_PAGE,
        "Page not found",
        "Page not found",
        "The server is reachable, but it has no resource at that URL. "
        "The link may be outdated or the page may have moved."
    };
    static const ns_error_info HTTP_410 = {
        ERROR_ICON_PAGE,
        "This page is gone",
        "This page is gone",
        "The server is telling us the resource has been permanently removed."
    };
    static const ns_error_info HTTP_401 = {
        ERROR_ICON_LOCK,
        "Authentication required",
        "Authentication required",
        "The server needs credentials Nordstjernen doesn't have. Sign in "
        "elsewhere first, or try a different URL."
    };
    static const ns_error_info HTTP_403 = {
        ERROR_ICON_BLOCKED,
        "Access denied",
        "Access denied",
        "The server understood the request but refused to share this "
        "resource with us."
    };
    static const ns_error_info HTTP_429 = {
        ERROR_ICON_CLOCK,
        "Too many requests",
        "Too many requests",
        "The server is throttling us. Wait a moment and try again."
    };
    static const ns_error_info HTTP_500 = {
        ERROR_ICON_SERVER,
        "Server error",
        "Server error",
        "The server hit an internal error processing this request. "
        "Nothing to do on our end — try again later."
    };
    static const ns_error_info HTTP_502 = {
        ERROR_ICON_SERVER,
        "Bad gateway",
        "Bad gateway",
        "An upstream server returned an invalid response. The site's "
        "infrastructure may be misconfigured."
    };
    static const ns_error_info HTTP_503 = {
        ERROR_ICON_SERVER,
        "Service unavailable",
        "Service unavailable",
        "The server is temporarily refusing requests, usually because it "
        "is overloaded or down for maintenance."
    };
    static const ns_error_info HTTP_504 = {
        ERROR_ICON_CLOCK,
        "Gateway timeout",
        "Gateway timeout",
        "An upstream server didn't answer in time."
    };
    static const ns_error_info HTTP_GENERIC_4XX = {
        ERROR_ICON_ALERT,
        "Request rejected",
        "Request rejected",
        "The server didn't accept this request."
    };
    static const ns_error_info HTTP_GENERIC_5XX = {
        ERROR_ICON_SERVER,
        "Server error",
        "Server error",
        "The server reported a failure handling this request."
    };
    static const ns_error_info GENERIC = {
        ERROR_ICON_ALERT,
        "Couldn't load page",
        "Couldn't load page",
        "Something went wrong fetching this URL."
    };
    static const ns_error_info FILE_MISSING = {
        ERROR_ICON_PAGE,
        "File not found",
        "File not found",
        "There is nothing at that path. The file may have been moved, "
        "renamed or deleted."
    };
    static const ns_error_info FILE_DENIED = {
        ERROR_ICON_LOCK,
        "Cannot read that file",
        "Cannot read that file",
        "The file exists but this account is not allowed to read it."
    };

    if (is_file_url) {
        if (status == 403) return &FILE_DENIED;
        if (transport_error &&
            g_strstr_len(transport_error, -1, "ermission"))
            return &FILE_DENIED;
        return &FILE_MISSING;
    }

    if (transport_error && *transport_error) {
        const char *e = transport_error;
        if (g_strstr_len(e, -1, "Could not resolve") ||
            g_strstr_len(e, -1, "resolve host") ||
            g_strstr_len(e, -1, "name resolution"))
            return &DNS;
        if (g_strstr_len(e, -1, "Connection refused") ||
            g_strstr_len(e, -1, "refused"))
            return &REFUSED;
        if (g_strstr_len(e, -1, "imed out") ||
            g_strstr_len(e, -1, "Timeout"))
            return &TIMEOUT;
        if (g_strstr_len(e, -1, "SSL") ||
            g_strstr_len(e, -1, "TLS") ||
            g_strstr_len(e, -1, "certificate"))
            return &TLS;
        if (g_strstr_len(e, -1, "URL") ||
            g_strstr_len(e, -1, "Protocol") ||
            g_strstr_len(e, -1, "malformed"))
            return &BAD_URL;
        if (g_strstr_len(e, -1, "network") ||
            g_strstr_len(e, -1, "unreachable") ||
            g_strstr_len(e, -1, "No route") ||
            g_strstr_len(e, -1, "onnect"))
            return &NO_NETWORK;
    }

    switch (status) {
    case 401: return &HTTP_401;
    case 403: return &HTTP_403;
    case 404: return &HTTP_404;
    case 410: return &HTTP_410;
    case 429: return &HTTP_429;
    case 500: return &HTTP_500;
    case 502: return &HTTP_502;
    case 503: return &HTTP_503;
    case 504: return &HTTP_504;
    }
    if (status >= 500 && status < 600) return &HTTP_GENERIC_5XX;
    if (status >= 400 && status < 500) return &HTTP_GENERIC_4XX;
    if (transport_error && *transport_error) return &NO_NETWORK;
    return &GENERIC;
}

char *
ns_build_error_page(const char *url, long status, const char *transport_error)
{
    gboolean is_file_url = url && g_str_has_prefix(url, "file:");
    const ns_error_info *info = classify_error(status, transport_error,
                                               is_file_url);
    const char *safe_url = url && *url ? url : "(no URL)";
    char *esc_url = ns_html_escape_text(safe_url);
    char *esc_title = ns_html_escape_text(info->title);
    char *esc_heading = ns_html_escape_text(info->heading);
    char *esc_summary = ns_html_escape_text(info->summary);
    char *esc_detail = NULL;
    if (transport_error && *transport_error) {
        char *esc_err = ns_html_escape_text(transport_error);
        esc_detail = g_strdup_printf("Technical detail: %s", esc_err);
        g_free(esc_err);
    } else if (status > 0) {
        esc_detail = g_strdup_printf("HTTP status: %ld", status);
    }

    char *retry_href = url && *url ? ns_html_escape_text(url) : g_strdup("");
    gboolean can_retry = url && *url && !g_str_has_prefix(url, "about:");

    GString *out = g_string_new(NULL);
    g_string_append(out,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>");
    g_string_append(out, esc_title);
    g_string_append(out, " — Nordstjernen</title>"
        "<meta name=\"color-scheme\" content=\"light dark\">"
        "<style>" NS_ABOUT_BASE_CSS
        "body{display:flex;align-items:center;justify-content:center;"
        "min-height:100vh;padding:32px 18px}"
        ".card{width:100%;max-width:600px;padding:36px 38px 30px}"
        ".icon{display:flex;align-items:center;justify-content:center;"
        "width:64px;height:64px;margin-bottom:20px;border-radius:20px;"
        "background:var(--accent-soft);color:var(--accent)}"
        "h1{margin:0 0 10px;font-size:24px;letter-spacing:-.01em;"
        "line-height:1.25}"
        "p.summary{margin:0 0 20px;color:var(--muted);font-size:15.5px}"
        ".url{margin:0 0 10px;padding:10px 14px;border-radius:12px;"
        "background:var(--field);font-family:var(--mono);font-size:13px;"
        "color:var(--muted);overflow-wrap:anywhere}"
        ".detail{margin:0 0 22px;font-family:var(--mono);font-size:12px;"
        "color:var(--faint);overflow-wrap:anywhere}"
        ".actions{display:flex;flex-wrap:wrap;gap:10px;margin-top:22px}"
        ".tips{margin-top:26px;padding-top:20px;"
        "border-top:1px solid var(--line);"
        "color:var(--muted);font-size:13.5px}"
        ".tips strong{color:var(--text)}"
        ".tips ul{margin:8px 0 0;padding-left:20px}"
        ".tips li{margin:4px 0}"
        "</style></head><body>"
        "<main class=\"card\">"
        "<div class=\"icon\">");
    g_string_append(out, info->icon);
    g_string_append(out, "</div>"
        "<h1>");
    g_string_append(out, esc_heading);
    g_string_append(out, "</h1>"
        "<p class=\"summary\">");
    g_string_append(out, esc_summary);
    g_string_append(out, "</p>"
        "<p class=\"url\">");
    g_string_append(out, esc_url);
    g_string_append(out, "</p>");
    if (esc_detail) {
        g_string_append(out, "<p class=\"detail\">");
        g_string_append(out, esc_detail);
        g_string_append(out, "</p>");
    }
    g_string_append(out, "<div class=\"actions\">");
    if (can_retry) {
        g_string_append(out, "<a class=\"btn primary\" href=\"");
        g_string_append(out, retry_href);
        g_string_append(out, "\">Try again</a>");
    }
    g_string_append(out,
        "<button class=\"btn\" "
        "onclick=\"history.back()\">Go back</button>"
        "<a class=\"btn\" href=\"about:start\">New Tab</a>"
        "</div>"
        "<div class=\"tips\">"
        "<strong>What to try:</strong>"
        "<ul>");
    if (is_file_url) {
        g_string_append(out,
            "<li>Check the path in the address bar for typos.</li>"
            "<li>Confirm the file is still where you expect it.</li>"
            "<li>Open the enclosing folder to browse what is there.</li>");
    } else {
        g_string_append(out,
            "<li>Double-check the address bar for typos.</li>"
            "<li>Make sure your internet connection is working.</li>"
            "<li>Reload the page in a moment — temporary outages do "
            "happen.</li>");
    }
    g_string_append(out,
        "</ul>"
        "</div>"
        "</main></body></html>");

    g_free(esc_url);
    g_free(esc_title);
    g_free(esc_heading);
    g_free(esc_summary);
    g_free(esc_detail);
    g_free(retry_href);

    return g_string_free(out, FALSE);
}

static gboolean
append_response_budgeted(GByteArray *body,
                         const guint8 *data,
                         gsize len,
                         guint64 *total,
                         guint64 budget)
{
    if (len == 0) return TRUE;
    if (len > G_MAXUINT) return FALSE;
    if (*total > budget) return FALSE;
    if ((guint64)len > budget - *total) return FALSE;
    if (*total + (guint64)len > G_MAXUINT) return FALSE;
    g_byte_array_append(body, data, (guint)len);
    *total += (guint64)len;
    return TRUE;
}

static char *
response_budget_error(guint64 stopped_at)
{
    return g_strdup_printf(
        "response would exhaust available memory (stopped at %llu MiB)",
        (unsigned long long)(stopped_at >> 20));
}

static int
base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static gboolean
decode_data_base64_budgeted(const char *data,
                            GByteArray *body,
                            guint64 budget,
                            gboolean *too_large)
{
    int q[4] = {0};
    int qn = 0;
    guint64 total = 0;
    gboolean ended = FALSE;
    if (too_large) *too_large = FALSE;
    for (const char *p = data; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '%' && g_ascii_isxdigit((guchar)p[1]) &&
            g_ascii_isxdigit((guchar)p[2])) {
            c = (unsigned char)((g_ascii_xdigit_value(p[1]) << 4) |
                                g_ascii_xdigit_value(p[2]));
            p += 2;
        }
        if (g_ascii_isspace(c)) continue;
        if (ended) return FALSE;
        if (c == '=') {
            q[qn++] = -2;
        } else {
            int v = base64_value((char)c);
            if (v < 0) return FALSE;
            q[qn++] = v;
        }
        if (qn < 4) continue;
        if (q[0] < 0 || q[1] < 0) return FALSE;
        guint8 out[3];
        gsize out_len = 1;
        out[0] = (guint8)((q[0] << 2) | (q[1] >> 4));
        if (q[2] == -2) {
            if (q[3] != -2) return FALSE;
            ended = TRUE;
        } else {
            out[1] = (guint8)(((q[1] & 15) << 4) | (q[2] >> 2));
            out_len = 2;
            if (q[3] == -2) {
                ended = TRUE;
            } else {
                if (q[3] < 0) return FALSE;
                out[2] = (guint8)(((q[2] & 3) << 6) | q[3]);
                out_len = 3;
            }
        }
        if (!append_response_budgeted(body, out, out_len, &total, budget)) {
            if (too_large) *too_large = TRUE;
            return FALSE;
        }
        qn = 0;
    }
    if (qn == 0) return TRUE;
    if (qn == 1 || q[0] < 0 || q[1] < 0) return FALSE;
    guint8 out[2];
    gsize out_len = 1;
    out[0] = (guint8)((q[0] << 2) | (q[1] >> 4));
    if (qn == 3) {
        if (q[2] < 0) return FALSE;
        out[1] = (guint8)(((q[1] & 15) << 4) | (q[2] >> 2));
        out_len = 2;
    }
    if (!append_response_budgeted(body, out, out_len, &total, budget)) {
        if (too_large) *too_large = TRUE;
        return FALSE;
    }
    return TRUE;
}

static gboolean
decode_data_uri_budgeted(const char *data,
                         GByteArray *body,
                         guint64 budget,
                         gboolean *too_large)
{
    guint8 buf[8192];
    gsize n = 0;
    guint64 total = 0;
    if (too_large) *too_large = FALSE;
    for (const char *p = data; *p; p++) {
        guint8 b;
        if (*p == '%') {
            if (!g_ascii_isxdigit((guchar)p[1]) ||
                !g_ascii_isxdigit((guchar)p[2]))
                return FALSE;
            b = (guint8)((g_ascii_xdigit_value(p[1]) << 4) |
                         g_ascii_xdigit_value(p[2]));
            p += 2;
        } else {
            b = (guint8)*p;
        }
        buf[n++] = b;
        if (n == sizeof(buf)) {
            if (!append_response_budgeted(body, buf, n, &total, budget)) {
                if (too_large) *too_large = TRUE;
                return FALSE;
            }
            n = 0;
        }
    }
    if (!append_response_budgeted(body, buf, n, &total, budget)) {
        if (too_large) *too_large = TRUE;
        return FALSE;
    }
    return TRUE;
}

gboolean
ns_data_url_decode(const char *url,
                   GByteArray *out,
                   guint64 budget,
                   char **out_content_type,
                   gboolean *too_large)
{
    if (too_large) *too_large = FALSE;
    if (out_content_type) *out_content_type = NULL;
    if (!url || !out || !g_str_has_prefix(url, "data:")) return FALSE;
    const char *p = url + 5;
    const char *comma = strchr(p, ',');
    if (!comma) return FALSE;
    char *meta = g_strndup(p, (gsize)(comma - p));
    g_strchomp(meta);
    gboolean base64 = FALSE;
    gsize meta_len = strlen(meta);
    if (meta_len >= 7 &&
        g_ascii_strcasecmp(meta + meta_len - 7, ";base64") == 0) {
        meta[meta_len - 7] = '\0';
        base64 = TRUE;
    }
    if (out_content_type)
        *out_content_type = (*meta) ? g_strdup(meta)
                                    : g_strdup("text/plain;charset=UTF-8");
    g_free(meta);
    const char *data = comma + 1;
    return base64
        ? decode_data_base64_budgeted(data, out, budget, too_large)
        : decode_data_uri_budgeted(data, out, budget, too_large);
}

static gboolean
read_file_budgeted(const char *path,
                   GByteArray *body,
                   guint64 budget,
                   char **error_out)
{
    GStatBuf st;
    if (g_stat(path, &st) == 0 && st.st_size > 0 &&
        ((guint64)st.st_size > budget || (guint64)st.st_size > G_MAXUINT)) {
        if (error_out) *error_out = response_budget_error((guint64)st.st_size);
        return FALSE;
    }
    FILE *f = g_fopen(path, "rb");
    if (!f) {
        if (error_out) *error_out = g_strdup(g_strerror(errno));
        return FALSE;
    }
    guint8 buf[65536];
    guint64 total = 0;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0) {
            if (!append_response_budgeted(body, buf, (gsize)n, &total, budget)) {
                if (error_out) *error_out = response_budget_error(total);
                fclose(f);
                return FALSE;
            }
        }
        if (n < sizeof(buf)) {
            if (ferror(f)) {
                if (error_out) *error_out = g_strdup(g_strerror(errno));
                fclose(f);
                return FALSE;
            }
            break;
        }
    }
    fclose(f);
    return TRUE;
}

static gboolean
synthesize_data_response(const char *url, ns_response *resp)
{
    if (!url || !g_str_has_prefix(url, "data:")) return FALSE;
    guint64 budget = ns_net_response_budget();
    gsize body_start = resp->body->len;
    char *ct = NULL;
    gboolean too_large = FALSE;
    if (!ns_data_url_decode(url, resp->body, budget, &ct, &too_large)) {
        if (!ct) return FALSE;
        g_byte_array_set_size(resp->body, body_start);
        if (too_large)
            resp->error = response_budget_error(budget);
        else
            resp->error = g_strdup("malformed data: URL");
    }
    resp->status = 200;
    resp->final_url = g_strdup(url);
    resp->content_type = ct;
    return TRUE;
}

typedef struct ns_file_listing_entry {
    char     *name;
    char     *display;
    char     *uri;
    guint64   size;
    gint64    mtime;
    gboolean  is_dir;
    gboolean  have_stat;
} ns_file_listing_entry;

static void
file_listing_entry_free(gpointer data)
{
    ns_file_listing_entry *e = data;
    if (!e) return;
    g_free(e->name);
    g_free(e->display);
    g_free(e->uri);
    g_free(e);
}

static gint
file_listing_entry_compare(gconstpointer ap, gconstpointer bp)
{
    const ns_file_listing_entry *a =
        *(const ns_file_listing_entry * const *)ap;
    const ns_file_listing_entry *b =
        *(const ns_file_listing_entry * const *)bp;
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;
    return g_utf8_collate(a->display ? a->display : "",
                          b->display ? b->display : "");
}

static char *
file_uri_for_path(const char *path, gboolean is_dir)
{
    if (!path || !*path) return NULL;
    char *p = g_strdup(path);
    if (is_dir && !g_str_has_suffix(p, G_DIR_SEPARATOR_S)) {
        char *with_sep = g_strconcat(p, G_DIR_SEPARATOR_S, NULL);
        g_free(p);
        p = with_sep;
    }
    char *uri = g_filename_to_uri(p, NULL, NULL);
    g_free(p);
    return uri;
}

static char *
file_size_label(guint64 size)
{
    static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)size;
    guint unit = 0;
    while (v >= 1024.0 && unit + 1 < G_N_ELEMENTS(units)) {
        v /= 1024.0;
        unit++;
    }
    if (unit == 0)
        return g_strdup_printf("%" G_GUINT64_FORMAT " B", size);
    if (v >= 100.0)
        return g_strdup_printf("%.0f %s", v, units[unit]);
    return g_strdup_printf("%.1f %s", v, units[unit]);
}

static char *
file_mtime_label(gint64 mtime)
{
    if (mtime <= 0) return g_strdup("");
    GDateTime *dt = g_date_time_new_from_unix_local(mtime);
    if (!dt) return g_strdup("");
    char *out = g_date_time_format(dt, "%Y-%m-%d %H:%M");
    g_date_time_unref(dt);
    return out ? out : g_strdup("");
}

static char *
file_listing_parent_uri(const char *path)
{
    if (!path || !*path) return NULL;
    char *parent = g_path_get_dirname(path);
    if (!parent || !*parent || strcmp(parent, ".") == 0 ||
        strcmp(parent, path) == 0) {
        g_free(parent);
        return NULL;
    }
    char *uri = file_uri_for_path(parent, TRUE);
    g_free(parent);
    return uri;
}

static char *
file_directory_error_page(const char *url, long status, const char *error)
{
    char *html = ns_build_error_page(url, status, error);
    return html ? html : g_strdup("<!doctype html><meta charset=utf-8>"
                                  "<title>Cannot read folder</title>"
                                  "<p>Cannot read folder.</p>");
}

static GString *
dir_listing_begin(const char *title, const char *parent_uri)
{
    char *esc_title = ns_html_escape_text(title);
    char *esc_parent = parent_uri ? ns_html_escape_text(parent_uri) : NULL;
    GString *out = g_string_new(NULL);
    g_string_append(out, "<!doctype html><html><head><meta charset=\"utf-8\">");
    g_string_append(out, "<title>Index of ");
    g_string_append(out, esc_title);
    g_string_append(out, "</title><style>"
        "body{font-family:serif;margin:8px;color:#000;background:#fff}"
        "h1{font-size:2em;margin:.4em 0}"
        "table{border-collapse:collapse}"
        "th{text-align:left;font-weight:bold;padding:0 3em .25em 0}"
        "td{padding:0 3em 0 0;white-space:nowrap}"
        "td.name{min-width:22em}"
        "td.size{text-align:right}"
        ".ico{display:inline-block;width:1.35em;margin-right:.35em;"
        "text-align:center;font-family:\"Segoe UI Emoji\",\"Apple Color Emoji\","
        "\"Noto Color Emoji\",sans-serif}"
        "a{color:#00e;text-decoration:underline}"
        "hr{border:0;border-top:1px solid #bbb;margin:.6em 0}"
        "</style></head><body><h1>Index of ");
    g_string_append(out, esc_title);
    g_string_append(out, "</h1><hr><table><thead><tr>"
        "<th>Name</th><th>Size</th><th>Date modified</th>"
        "</tr></thead><tbody>");
    if (parent_uri) {
        g_string_append(out, "<tr><td class=\"name\"><span class=\"ico dir\""
            " aria-hidden=\"true\">&#128193;</span><a href=\"");
        g_string_append(out, esc_parent);
        g_string_append(out, "\">../</a></td><td class=\"size\"></td>"
            "<td></td></tr>");
    }
    g_free(esc_title);
    g_free(esc_parent);
    return out;
}

static void
dir_listing_row(GString *out, const char *uri, const char *name,
                gboolean is_dir, const char *size, const char *date)
{
    char *esc_uri = ns_html_escape_text(uri);
    char *esc_name = ns_html_escape_text(name);
    char *esc_size = ns_html_escape_text(size);
    char *esc_date = ns_html_escape_text(date);
    g_string_append(out, "<tr><td class=\"name\"><span class=\"ico ");
    g_string_append(out, is_dir ? "dir" : "file");
    g_string_append(out, "\" aria-hidden=\"true\">");
    g_string_append(out, is_dir ? "&#128193;" : "&#128196;");
    g_string_append(out, "</span><a href=\"");
    g_string_append(out, esc_uri);
    g_string_append(out, "\">");
    g_string_append(out, esc_name);
    if (is_dir)
        g_string_append_c(out, '/');
    g_string_append(out, "</a></td><td class=\"size\">");
    g_string_append(out, esc_size);
    g_string_append(out, "</td><td>");
    g_string_append(out, esc_date);
    g_string_append(out, "</td></tr>");
    g_free(esc_uri);
    g_free(esc_name);
    g_free(esc_size);
    g_free(esc_date);
}

static char *
dir_listing_end(GString *out)
{
    g_string_append(out, "</tbody></table><hr></body></html>");
    return g_string_free(out, FALSE);
}

static char *
file_directory_listing_page(const char *path, const char *url, long *status_out)
{
    if (status_out) *status_out = 200;
    GError *err = NULL;
    GDir *dir = g_dir_open(path, 0, &err);
    if (!dir) {
        long status = g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_ACCES)
            ? 403 : 404;
        char *msg = g_strdup(err ? err->message : "cannot read folder");
        g_clear_error(&err);
        if (status_out) *status_out = status;
        char *html = file_directory_error_page(url, status, msg);
        g_free(msg);
        return html;
    }

    GPtrArray *entries = g_ptr_array_new_with_free_func(file_listing_entry_free);
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        char *full = g_build_filename(path, name, NULL);
        ns_file_listing_entry *e = g_new0(ns_file_listing_entry, 1);
        e->name = g_strdup(name);
        e->display = g_filename_display_name(name);
        e->is_dir = g_file_test(full, G_FILE_TEST_IS_DIR);
        GStatBuf st;
        if (g_stat(full, &st) == 0) {
            e->have_stat = TRUE;
            if (st.st_size > 0)
                e->size = (guint64)st.st_size;
            e->mtime = (gint64)st.st_mtime;
        }
        e->uri = file_uri_for_path(full, e->is_dir);
        if (e->uri)
            g_ptr_array_add(entries, e);
        else
            file_listing_entry_free(e);
        g_free(full);
    }
    g_dir_close(dir);
    g_ptr_array_sort(entries, file_listing_entry_compare);

    char *display_path = g_filename_display_name(path);
    char *parent_uri = file_listing_parent_uri(path);
    GString *out = dir_listing_begin(display_path ? display_path : path,
                                     parent_uri);
    for (guint i = 0; i < entries->len; i++) {
        ns_file_listing_entry *e = g_ptr_array_index(entries, i);
        char *size = e->is_dir ? g_strdup("") : file_size_label(e->size);
        char *date = e->have_stat ? file_mtime_label(e->mtime) : g_strdup("");
        dir_listing_row(out, e->uri, e->display ? e->display : e->name,
                        e->is_dir, size, date);
        g_free(size);
        g_free(date);
    }
    g_ptr_array_free(entries, TRUE);
    g_free(display_path);
    g_free(parent_uri);
    return dir_listing_end(out);
}

typedef struct ns_ftp_listing_entry {
    char     *name;
    char     *display;
    char     *uri;
    char     *date;
    guint64   size;
    gboolean  is_dir;
    gboolean  have_size;
} ns_ftp_listing_entry;

static void
ftp_listing_entry_free(gpointer data)
{
    ns_ftp_listing_entry *e = data;
    if (!e) return;
    g_free(e->name);
    g_free(e->display);
    g_free(e->uri);
    g_free(e->date);
    g_free(e);
}

static gint
ftp_listing_entry_compare(gconstpointer ap, gconstpointer bp)
{
    const ns_ftp_listing_entry *a =
        *(const ns_ftp_listing_entry * const *)ap;
    const ns_ftp_listing_entry *b =
        *(const ns_ftp_listing_entry * const *)bp;
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;
    return g_utf8_collate(a->display ? a->display : "",
                          b->display ? b->display : "");
}

static char **
ftp_split_fields(const char *line, int max_fields)
{
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    const char *p = line ? line : "";
    while (*p && (max_fields <= 0 ||
                  parts->len + 1 < (guint)max_fields)) {
        while (g_ascii_isspace(*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !g_ascii_isspace(*p)) p++;
        g_ptr_array_add(parts, g_strndup(start, (gsize)(p - start)));
    }
    while (g_ascii_isspace(*p)) p++;
    if (*p)
        g_ptr_array_add(parts, g_strdup(p));
    g_ptr_array_add(parts, NULL);
    return (char **)g_ptr_array_free(parts, FALSE);
}

static char *
ftp_listing_child_uri(const char *base_url, const char *name, gboolean is_dir)
{
    if (!base_url || !*base_url || !name || !*name) return NULL;
    char *base = g_strdup(base_url);
    char *hash = strchr(base, '#');
    if (hash) *hash = '\0';
    char *query = strchr(base, '?');
    if (query) *query = '\0';
    if (!g_str_has_suffix(base, "/")) {
        char *with_slash = g_strconcat(base, "/", NULL);
        g_free(base);
        base = with_slash;
    }
    char *seg = g_uri_escape_string(name, NULL, FALSE);
    char *out = g_strconcat(base, seg, is_dir ? "/" : "", NULL);
    g_free(seg);
    g_free(base);
    return out;
}

static char *
ftp_listing_parent_uri(const char *url)
{
    if (!url || !*url) return NULL;
    char *base = g_strdup(url);
    char *hash = strchr(base, '#');
    if (hash) *hash = '\0';
    char *query = strchr(base, '?');
    if (query) *query = '\0';
    gsize len = strlen(base);
    while (len > 0 && base[len - 1] == '/')
        base[--len] = '\0';
    char *after_scheme = strstr(base, "://");
    char *path = after_scheme ? strchr(after_scheme + 3, '/') : NULL;
    if (!path || !path[1]) {
        g_free(base);
        return NULL;
    }
    char *last = strrchr(path + 1, '/');
    if (!last) {
        path[1] = '\0';
    } else {
        last[1] = '\0';
    }
    char *out = g_strdup(base);
    g_free(base);
    return out;
}

static gboolean
ftp_parse_unix_listing(char *line, ns_ftp_listing_entry *e)
{
    if (!line || !*line || !e) return FALSE;
    if (line[0] != 'd' && line[0] != '-' && line[0] != 'l')
        return FALSE;
    char **tokens = ftp_split_fields(line, 9);
    int count = 0;
    while (tokens[count]) count++;
    if (count < 9 || !tokens[4] || !tokens[5] || !tokens[6] ||
        !tokens[7] || !tokens[8] || !*tokens[8]) {
        g_strfreev(tokens);
        return FALSE;
    }
    e->is_dir = line[0] == 'd';
    e->size = g_ascii_strtoull(tokens[4], NULL, 10);
    e->have_size = TRUE;
    e->date = g_strdup_printf("%s %s %s", tokens[5], tokens[6], tokens[7]);
    char *name = g_strdup(tokens[8]);
    if (line[0] == 'l') {
        char *arrow = strstr(name, " -> ");
        if (arrow) *arrow = '\0';
    }
    g_strstrip(name);
    if (!*name || g_str_equal(name, ".") || g_str_equal(name, "..")) {
        g_free(name);
        g_strfreev(tokens);
        return FALSE;
    }
    e->name = name;
    e->display = g_strdup(name);
    g_strfreev(tokens);
    return TRUE;
}

static gboolean
ftp_parse_dos_listing(char *line, ns_ftp_listing_entry *e)
{
    if (!line || !*line || !e) return FALSE;
    char **tokens = ftp_split_fields(line, 4);
    int count = 0;
    while (tokens[count]) count++;
    if (count < 4 || !tokens[0] || !tokens[1] || !tokens[2] ||
        !tokens[3] || !*tokens[3]) {
        g_strfreev(tokens);
        return FALSE;
    }
    if (!strchr(tokens[0], '-') || !strchr(tokens[1], ':')) {
        g_strfreev(tokens);
        return FALSE;
    }
    e->is_dir = g_ascii_strcasecmp(tokens[2], "<DIR>") == 0;
    if (!e->is_dir) {
        e->size = g_ascii_strtoull(tokens[2], NULL, 10);
        e->have_size = TRUE;
    }
    e->date = g_strdup_printf("%s %s", tokens[0], tokens[1]);
    char *name = g_strdup(tokens[3]);
    g_strstrip(name);
    if (!*name || g_str_equal(name, ".") || g_str_equal(name, "..")) {
        g_free(name);
        g_strfreev(tokens);
        return FALSE;
    }
    e->name = name;
    e->display = g_strdup(name);
    g_strfreev(tokens);
    return TRUE;
}

static ns_ftp_listing_entry *
ftp_listing_entry_from_line(const char *line)
{
    if (!line) return NULL;
    char *work = g_strdup(line);
    g_strstrip(work);
    if (!*work) {
        g_free(work);
        return NULL;
    }
    ns_ftp_listing_entry *e = g_new0(ns_ftp_listing_entry, 1);
    if (!ftp_parse_unix_listing(work, e) &&
        !ftp_parse_dos_listing(work, e)) {
        g_clear_pointer(&e->name, g_free);
        g_clear_pointer(&e->display, g_free);
        g_clear_pointer(&e->date, g_free);
        e->size = 0;
        e->have_size = FALSE;
        e->is_dir = FALSE;
        e->name = g_strdup(work);
        e->display = g_strdup(work);
    }
    g_free(work);
    if (!e->name || !*e->name) {
        ftp_listing_entry_free(e);
        return NULL;
    }
    return e;
}

static char *
ftp_directory_listing_page(const char *url, GByteArray *body)
{
    char *listing = (body && body->data && body->len > 0)
        ? g_strndup((const char *)body->data, body->len) : g_strdup("");
    GPtrArray *entries = g_ptr_array_new_with_free_func(ftp_listing_entry_free);
    char **lines = g_strsplit(listing, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        ns_ftp_listing_entry *e = ftp_listing_entry_from_line(lines[i]);
        if (!e) continue;
        e->uri = ftp_listing_child_uri(url, e->name, e->is_dir);
        if (e->uri)
            g_ptr_array_add(entries, e);
        else
            ftp_listing_entry_free(e);
    }
    g_strfreev(lines);
    g_free(listing);
    g_ptr_array_sort(entries, ftp_listing_entry_compare);

    char *parent_uri = ftp_listing_parent_uri(url);
    GString *out = dir_listing_begin(url ? url : "ftp://", parent_uri);
    for (guint i = 0; i < entries->len; i++) {
        ns_ftp_listing_entry *e = g_ptr_array_index(entries, i);
        char *size = (!e->is_dir && e->have_size) ? file_size_label(e->size)
                                                  : g_strdup("");
        dir_listing_row(out, e->uri, e->display ? e->display : e->name,
                        e->is_dir, size, e->date ? e->date : "");
        g_free(size);
    }
    g_ptr_array_free(entries, TRUE);
    g_free(parent_uri);
    return dir_listing_end(out);
}

static gboolean
ftp_url_looks_like_directory(const char *url)
{
    if (!ns_url_is_ftp(url)) return FALSE;
    g_autoptr(ns_url_parts) parts = ns_url_parts_new(url);
    if (!parts || !parts->pathname) return g_str_has_suffix(url, "/");
    return !*parts->pathname || g_str_has_suffix(parts->pathname, "/");
}

static void
maybe_synthesize_ftp_listing(ns_response *resp)
{
    if (!resp || !resp->body || !ftp_url_looks_like_directory(resp->final_url))
        return;
    char *html = ftp_directory_listing_page(resp->final_url, resp->body);
    if (!html) return;
    g_byte_array_set_size(resp->body, 0);
    g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
    g_free(html);
    g_free(resp->content_type);
    resp->content_type = g_strdup("text/html; charset=utf-8");
}

static void
maybe_guess_ftp_content_type(ns_response *resp)
{
    if (!resp || resp->content_type || !ns_url_is_ftp(resp->final_url))
        return;
    if (ftp_url_looks_like_directory(resp->final_url)) return;
    g_autoptr(ns_url_parts) parts = ns_url_parts_new(resp->final_url);
    const char *path = parts && parts->pathname ? parts->pathname
                                                : resp->final_url;
    gboolean uncertain = FALSE;
    char *ctype = g_content_type_guess(path,
                                       resp->body ? resp->body->data : NULL,
                                       resp->body ? resp->body->len : 0,
                                       &uncertain);
    char *mime = ctype ? g_content_type_get_mime_type(ctype) : NULL;
    resp->content_type = g_strdup(mime ? mime : "application/octet-stream");
    g_free(mime);
    g_free(ctype);
}

static gboolean
ns_file_access_allowed(const char *top_url)
{
    if (!g_allow_file_urls) return FALSE;
    if (!top_url || !*top_url) return TRUE;
    return g_str_has_prefix(top_url, "file:");
}

static gboolean
synthesize_file_response(const char *url, const char *top_url, ns_response *resp)
{
    if (!url || !g_str_has_prefix(url, "file:")) return FALSE;
    if (!ns_file_access_allowed(top_url)) {
        resp->final_url = g_strdup(url);
        resp->status = 0;
        resp->error = g_strdup("local file access is not allowed from a "
                               "remote page");
        return TRUE;
    }
    char *uri_path = g_filename_from_uri(url, NULL, NULL);
    char *path = uri_path ? g_canonicalize_filename(uri_path, NULL) : NULL;
    g_free(uri_path);
    resp->final_url = g_strdup(url);
    if (!path) {
        resp->status = 400;
        resp->error = g_strdup("invalid file URL");
        return TRUE;
    }

    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_free(resp->final_url);
        resp->final_url = file_uri_for_path(path, TRUE);
        if (!resp->final_url)
            resp->final_url = g_strdup(url);
        long status = 200;
        char *html = file_directory_listing_page(path, resp->final_url, &status);
        resp->status = status;
        resp->content_type = g_strdup("text/html; charset=utf-8");
        if (html) {
            g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
            g_free(html);
        }
        g_free(path);
        return TRUE;
    }

    guint64 budget = ns_net_response_budget();
    char *read_error = NULL;
    if (!read_file_budgeted(path, resp->body, budget, &read_error)) {
        resp->status = read_error &&
            g_str_has_prefix(read_error, "response would exhaust") ? 0 : 404;
        resp->error = read_error ? read_error : g_strdup("file not found");
    } else {
        resp->status = 200;
    }

    gboolean uncertain = FALSE;
    char *ctype = g_content_type_guess(path, NULL, 0, &uncertain);
    char *mime = ctype ? g_content_type_get_mime_type(ctype) : NULL;
    resp->content_type = g_strdup(mime ? mime : "application/octet-stream");
    g_free(mime);
    g_free(ctype);
    g_free(path);
    return TRUE;
}

static const char k_about_start_template[] =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<meta name=\"color-scheme\" content=\"light dark\">"
    "<title>New Tab</title>"
    "<style>\n" NS_ABOUT_BASE_CSS
    "html,body{height:100%;background:#fff}\n"
    ".ntp{max-width:720px;margin:0 auto;padding:13vh 24px 40px;"
    "display:flex;flex-direction:column;align-items:center}\n"
    ".brand{display:flex;flex-direction:column;align-items:center;"
    "gap:14px;margin-bottom:30px}\n"
    ".mark-img{display:block;width:76px;height:76px;border-radius:20px;"
    "box-shadow:0 10px 30px rgba(20,40,110,.28)}\n"
    ".brand h1{margin:0;font-size:32px;font-weight:700;"
    "letter-spacing:-.02em;line-height:1.1;text-align:center}\n"
    ".brand h1 .ver{color:var(--faint);font-weight:500}\n"
    ".search{display:flex;align-items:center;gap:12px;width:100%;"
    "height:58px;padding:0 7px 0 22px;border-radius:999px;"
    "background:var(--card);border:1px solid var(--line);"
    "box-shadow:var(--shadow)}\n"
    ".search:focus-within{border-color:var(--accent);"
    "box-shadow:0 0 0 4px var(--accent-soft),var(--shadow)}\n"
    ".search svg{flex:0 0 auto;color:var(--faint)}\n"
    ".search input{flex:1 1 auto;min-width:0;height:100%;border:0;"
    "outline:0;background:transparent;color:var(--text);font:inherit;"
    "font-size:17px}\n"
    ".search button{flex:0 0 auto;display:flex;align-items:center;"
    "justify-content:center;width:44px;height:44px;border:0;"
    "border-radius:50%;background:var(--accent);color:var(--accent-ink);"
    "cursor:pointer}\n"
    ".search button svg{color:var(--accent-ink)}\n"
    ".search button:hover{filter:brightness(1.08)}\n"
    ".hint{min-height:18px;margin:10px 0 0;color:var(--faint);"
    "font-size:13px;text-align:center}\n"
    ".hint kbd{padding:1px 6px;border:1px solid var(--line);"
    "border-radius:5px;font:inherit;font-size:12px;color:var(--muted)}\n"
    ".tiles{display:flex;flex-wrap:wrap;justify-content:center;gap:8px;"
    "margin-top:22px}\n"
    ".tile{display:flex;align-items:center;gap:7px;padding:6px 14px 6px 8px;"
    "border-radius:999px;background:var(--card);"
    "border:1px solid var(--line);color:var(--text);font-size:13px;"
    "font-weight:600}\n"
    ".tile:hover{text-decoration:none;border-color:var(--accent);"
    "background:var(--accent-soft)}\n"
    ".ic{display:flex;align-items:center;justify-content:center;"
    "width:24px;height:24px;border-radius:50%;"
    "background:var(--accent-soft);color:var(--accent)}\n"
    ".foot{margin-top:40px;color:var(--faint);font-size:13px}\n"
    ".foot a{color:var(--muted);margin:0 8px}\n"
    "@media (prefers-color-scheme:dark){html,body{background:var(--bg)}}\n"
    "@media (max-width:560px){.ntp{padding-top:7vh}"
    ".brand h1{font-size:26px}}\n"
    "</style></head>"
    "<body><main class=\"ntp\">"
    "<div class=\"brand\">__ND_LOGO_MARK__<h1>Nordstjernen Sol Browser "
    "<span class=\"ver\">" NS_VERSION "</span></h1></div>"
    "<form class=\"search\" id=\"dsearch\" role=\"search\">"
    "<svg width=\"20\" height=\"20\" viewBox=\"0 0 16 16\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"1.6\" stroke-linecap=\"round\">"
    "<circle cx=\"7\" cy=\"7\" r=\"4.6\"/><path d=\"M10.4 10.4 14 14\"/></svg>"
    "<input id=\"sq\" name=\"q\" autocomplete=\"off\" autofocus dir=\"auto\""
    " aria-label=\"Search the web or enter an address\""
    " placeholder=\"Search with __ND_SEARCH_NAME__ or enter an address\">"
    "<button type=\"submit\" aria-label=\"Search\">"
    "<svg width=\"18\" height=\"18\" viewBox=\"0 0 16 16\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\""
    " stroke-linejoin=\"round\"><path d=\"M3 8h9.5M8.5 4l4 4-4 4\"/></svg>"
    "</button></form>"
    "<p class=\"hint\" id=\"hint\" aria-live=\"polite\"></p>"
    "<nav class=\"tiles\">"
    "<a class=\"tile\" href=\"about:history\"><span class=\"ic\">"
    "<svg width=\"20\" height=\"20\" viewBox=\"0 0 16 16\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"1.5\" stroke-linecap=\"round\""
    " stroke-linejoin=\"round\"><circle cx=\"8\" cy=\"8\" r=\"5.75\"/>"
    "<path d=\"M8 4.75V8l2.25 1.5\"/></svg></span>History</a>"
    "<a class=\"tile\" href=\"about:settings\"><span class=\"ic\">"
    "<svg width=\"20\" height=\"20\" viewBox=\"0 0 16 16\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"1.5\" stroke-linecap=\"round\">"
    "<path d=\"M2.75 4.5h6.5M12.25 4.5h1M2.75 11.5h1M6.75 11.5h6.5\"/>"
    "<circle cx=\"10.75\" cy=\"4.5\" r=\"1.5\"/>"
    "<circle cx=\"5.25\" cy=\"11.5\" r=\"1.5\"/></svg></span>Settings</a>"
    "<a class=\"tile\" href=\"about:nordstjernen\"><span class=\"ic\">"
    "<svg width=\"20\" height=\"20\" viewBox=\"0 0 16 16\""
    " fill=\"currentColor\">"
    "<path d=\"M8 1.5l1.35 4.15L13.5 7l-4.15 1.35L8 12.5 6.65 8.35 2.5 7"
    "l4.15-1.35z\"/></svg></span>About</a>"
    "<a class=\"tile\" href=\"https://nordstjernen.org\"><span class=\"ic\">"
    "<svg width=\"20\" height=\"20\" viewBox=\"0 0 16 16\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"1.5\"><circle cx=\"8\" cy=\"8\""
    " r=\"5.75\"/><ellipse cx=\"8\" cy=\"8\" rx=\"2.4\" ry=\"5.75\"/>"
    "<path d=\"M2.5 8h11\"/></svg></span>nordstjernen.org</a>"
    "</nav>"
    "<p class=\"foot\"><a href=\"about:license\">License</a>"
    "<a href=\"https://nordstjernen.org/privacy\">Privacy</a></p>"
    "</main>"
    "<script>\n"
    "var searchUrl='__ND_SEARCH_URL__';\n"
    "var dsearch=document.getElementById('dsearch');\n"
    "var sq=document.getElementById('sq');\n"
    "function addressFor(s){\n"
    " if(/^(https?|file|about|view-source):/i.test(s)) return s;\n"
    " if(!/\\s/.test(s)&&/^(localhost|[^\\s.\\/:]+(\\.[^\\s.\\/:]+)+)"
    "(:\\d+)?(\\/.*)?$/i.test(s)) return 'https://'+s;\n"
    " return null;}\n"
    "var hint=document.getElementById('hint');\n"
    "function showHint(){\n"
    " var s=sq.value.trim(); hint.textContent='';\n"
    " if(!s) return;\n"
    " var a=addressFor(s);\n"
    " hint.appendChild(document.createTextNode('Press '));\n"
    " var k=document.createElement('kbd'); k.textContent='Enter';\n"
    " hint.appendChild(k);\n"
    " hint.appendChild(document.createTextNode(a\n"
    "  ? ' to open '+a.replace(/^https:\\/\\//,'')\n"
    "  : ' to search with __ND_SEARCH_NAME__'));}\n"
    "sq.addEventListener('input',showHint);\n"
    "dsearch.addEventListener('submit',function(e){e.preventDefault();\n"
    " var s=sq.value.trim(); if(!s) return;\n"
    " var a=addressFor(s); if(a){window.location.href=a;return;}\n"
    " window.location.href=searchUrl.indexOf('%s')>=0\n"
    "  ? searchUrl.replace('%s',encodeURIComponent(s))\n"
    "  : searchUrl+encodeURIComponent(s);});\n"
    "try{sq.focus();}catch(e){}\n"
    "</script></body></html>";

static const char k_about_nordstjernen_template[] =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<meta name=\"color-scheme\" content=\"light dark\">"
    "<title>About Nordstjernen Sol</title>"
    "<style>\n" NS_ABOUT_BASE_CSS
    ".wrap{max-width:760px;margin:0 auto;padding:28px 24px 48px}\n"
    ".top{margin-bottom:22px}\n"
    ".head{display:flex;align-items:center;gap:18px;margin:6px 4px 26px}\n"
    ".mark-img{display:block;width:64px;height:64px;border-radius:17px;"
    "box-shadow:0 8px 24px rgba(20,40,110,.25)}\n"
    ".head h1{margin:0;font-size:30px;font-weight:700;letter-spacing:-.02em;"
    "line-height:1.15}\n"
    ".head p{margin:2px 0 0;color:var(--muted)}\n"
    ".ver{display:inline-block;margin-left:10px;padding:3px 10px;"
    "border-radius:999px;background:var(--accent-soft);color:var(--accent);"
    "font-size:13px;font-weight:700;vertical-align:middle;letter-spacing:0}\n"
    ".card{padding:22px 24px;margin-bottom:16px}\n"
    ".card h2{margin:0 0 8px;font-size:17px}\n"
    ".card p{margin:0 0 10px;color:var(--muted);font-size:14px}\n"
    ".card p b{color:var(--text)}\n"
    ".actions{display:flex;flex-wrap:wrap;gap:10px;margin-top:14px}\n"
    ".diag h3{margin:20px 0 6px;font-size:12px;font-weight:700;"
    "letter-spacing:.08em;text-transform:uppercase;color:var(--faint)}\n"
    ".diag h3:first-child{margin-top:0}\n"
    ".drow{display:flex;justify-content:space-between;gap:20px;"
    "padding:8px 0;border-bottom:1px solid var(--line);font-size:13.5px}\n"
    ".drow:last-child{border-bottom:0}\n"
    ".dk{color:var(--muted);flex:0 0 auto}\n"
    ".dv{color:var(--text);text-align:right;word-break:break-word;"
    "font-family:var(--mono);font-size:12.5px}\n"
    ".dv.on,.dv.off{font-family:var(--font);font-weight:600;font-size:12px;"
    "padding:1px 10px;border-radius:999px}\n"
    ".dv.on{color:var(--ok);background:rgba(19,128,74,.1)}\n"
    ".dv.off{color:var(--faint);background:var(--field)}\n"
    ".foot{display:flex;flex-wrap:wrap;justify-content:space-between;"
    "gap:12px;margin-top:26px;color:var(--faint);font-size:13px}\n"
    ".foot a{color:var(--muted);margin-left:14px}\n"
    "@media (max-width:560px){.head{flex-direction:column;"
    "align-items:flex-start}.drow{font-size:12.5px}}\n"
    "</style></head>"
    "<body><main class=\"wrap\">"
    "<div class=\"top\"><a class=\"crumb\" href=\"about:start\">"
    "\xe2\x86\x90 New Tab</a></div>"
    "<div class=\"head\">__ND_LOGO_MARK__<div>"
    "<h1>Nordstjernen Sol<span class=\"ver\">" NS_VERSION "</span></h1>"
    "<p>A web browser implemented in C.</p></div></div>"
    "<section class=\"card\">"
    "<h2>License</h2>"
    "<p>Nordstjernen is \xc2\xa9 2026 Andreas R\xc3\xb8sdal and dual-licensed: "
    "you may use, modify and redistribute it under either the "
    "<b>Nordstjernen Source License v1.0 (NSL-1.0)</b> or the "
    "<b>GNU General Public License, version\xc2\xa0" "3 or later "
    "(GPL-3.0-or-later)</b>, "
    "at your option. It comes with ABSOLUTELY NO WARRANTY.</p>"
    "<p>It bundles third-party open-source software under "
    "the MIT, BSD, Apache\xc2\xa0" "2.0, LGPL, MPL and zlib licenses, "
    "including lexbor, QuickJS, Wuffs, GTK\xc2\xa0" "4, Cairo, "
    "libcurl, OpenSSL, uchardet, libwebp, libpsl, SQLite and zlib. Each "
    "component\xe2\x80\x99s copyright notice and full license text is "
    "reproduced in the notices.</p>"
    "<div class=\"actions\">"
    "<a class=\"btn\" href=\"about:license\">Nordstjernen Source License</a>"
    "<a class=\"btn\" href=\"about:gpl\">GNU GPL v3</a>"
    "<a class=\"btn\" href=\"about:third-party\">Third-party notices</a>"
    "</div>"
    "</section>"
    "<section class=\"card\"><h2>Security</h2>__ND_SECURITY__</section>"
    "<section class=\"card\">__ND_DIAG__</section>"
    "<div class=\"foot\"><span>Nordstjernen Web Browser \xc2\xa9 2026 "
    "Andreas R\xc3\xb8sdal</span><span>"
    "<a href=\"https://nordstjernen.org/privacy\">Privacy</a>"
    "<a href=\"https://nordstjernen.org\">nordstjernen.org</a>"
    "</span></div>"
    "</main></body></html>";

#define SETTINGS_NAV_ICON(paths) \
    "<svg width=\"16\" height=\"16\" viewBox=\"0 0 16 16\" fill=\"none\" " \
    "stroke=\"currentColor\" stroke-width=\"1.5\" stroke-linecap=\"round\" " \
    "stroke-linejoin=\"round\">" paths "</svg>"

#define SETTINGS_SWITCH(id, label, desc) \
    "<label class=\"row\"><span class=\"txt\">" \
    "<span class=\"lbl\">" label "</span>" \
    "<span class=\"desc\">" desc "</span></span>" \
    "<span class=\"sw\"><input type=\"checkbox\" id=\"" id "\">" \
    "<span></span></span></label>"

static const char k_about_settings_html[] =
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<meta name=\"color-scheme\" content=\"light dark\">\n"
"<title>Settings</title>\n"
"<style>\n" NS_ABOUT_BASE_CSS
".layout{display:flex;gap:32px;max-width:980px;margin:0 auto;"
"padding:36px 28px 60px}\n"
".side{flex:0 0 200px;position:sticky;top:36px;align-self:flex-start}\n"
".side h1{margin:0 0 18px 12px;font-size:24px;letter-spacing:-.02em}\n"
".side a{display:flex;align-items:center;gap:10px;height:38px;"
"padding:0 12px;border-radius:999px;color:var(--muted);font-size:14px;"
"font-weight:600}\n"
".side a:hover{background:var(--field);color:var(--text);"
"text-decoration:none}\n"
".side svg{color:var(--accent)}\n"
"main{flex:1 1 auto;min-width:0}\n"
"section{margin:0 0 26px}\n"
"section h2{margin:0 0 10px 6px;font-size:15px;font-weight:700}\n"
".card{padding:6px 22px}\n"
".row{display:flex;align-items:center;justify-content:space-between;"
"gap:20px;padding:14px 0;cursor:pointer}\n"
".card > * + *{border-top:1px solid var(--line)}\n"
".txt{display:flex;flex-direction:column;gap:2px;min-width:0}\n"
".lbl{font-size:14.5px;font-weight:600}\n"
".desc{font-size:13px;color:var(--muted)}\n"
".field{display:flex;flex-direction:column;gap:8px;padding:14px 0}\n"
"input[type=text],select{width:100%;padding:9px 12px;"
"border-radius:12px;border:1px solid var(--line);background:var(--field);"
"color:var(--text);font:inherit;font-size:14px;outline:none}\n"
"input[type=text]:focus,select:focus{border-color:var(--accent);"
"box-shadow:0 0 0 3px var(--accent-soft)}\n"
".sw{position:relative;flex:0 0 auto;width:42px;height:24px}\n"
".sw input{position:absolute;opacity:0;width:0;height:0;margin:0}\n"
".sw span{position:absolute;top:0;left:0;right:0;bottom:0;"
"border-radius:999px;background:rgba(120,128,145,.35);"
"transition:background .18s}\n"
".sw span::after{content:\"\";position:absolute;top:3px;left:3px;"
"width:18px;height:18px;border-radius:50%;background:#fff;"
"box-shadow:0 1px 3px rgba(0,0,0,.3);transition:left .18s}\n"
".sw input:checked + span{background:var(--accent)}\n"
".sw input:checked + span::after{left:21px}\n"
".sw input:focus-visible + span{box-shadow:0 0 0 3px var(--accent-soft)}\n"
".danger-row{display:flex;align-items:center;justify-content:space-between;"
"gap:20px;padding:16px 0}\n"
"#custom_wrap[hidden]{display:none}\n"
".save-row{display:flex;justify-content:flex-end;padding:14px 0}\n"
".toast{position:fixed;left:50%;bottom:28px;transform:translateX(-50%);"
"padding:10px 18px;border-radius:999px;background:var(--text);"
"color:var(--bg);font-size:13.5px;font-weight:600;"
"box-shadow:0 8px 24px rgba(0,0,0,.2);opacity:0;transition:opacity .2s}\n"
".toast.show{opacity:1}\n"
"@media (max-width:760px){.layout{flex-direction:column;gap:8px;"
"padding:24px 18px 48px}.side{position:static}"
".side a{display:none}}\n"
"</style></head><body>\n"
"<div class=\"layout\">\n"
"<nav class=\"side\"><h1>Settings</h1>"
"<a href=\"#general\">" SETTINGS_NAV_ICON(
    "<path d=\"M2.25 7.5 8 2.5l5.75 5\"/>"
    "<path d=\"M3.75 6.5v7h8.5v-7\"/>")
    "General</a>"
"<a href=\"#privacy\">" SETTINGS_NAV_ICON(
    "<path d=\"M8 1.75 13.25 3.75v4c0 3.25-2.25 5.5-5.25 6.5"
    "-3-1-5.25-3.25-5.25-6.5v-4z\"/>")
    "Privacy</a>"
"<a href=\"#content\">" SETTINGS_NAV_ICON(
    "<rect x=\"2.25\" y=\"2.75\" width=\"11.5\" height=\"10.5\""
    " rx=\"2\"/><path d=\"M2.25 6h11.5\"/>")
    "Content</a>"
"<a href=\"#data\">" SETTINGS_NAV_ICON(
    "<path d=\"M3.25 4.5h9.5M6.5 4.5V3h3v1.5M4.5 4.5l.6 8.75h5.8l.6-8.75\"/>")
    "Browsing data</a></nav>\n"
"<main>\n"
"<section id=\"general\"><h2>General</h2><div class=\"card\">"
"<label class=\"field\"><span class=\"lbl\">Home page</span>"
"<input id=\"home_url\" type=\"text\" spellcheck=\"false\"></label>"
"<label class=\"field\"><span class=\"lbl\">Search engine</span>"
"<select id=\"search_pick\"></select></label>"
"<label class=\"field\" id=\"custom_wrap\"><span class=\"lbl\">"
"Custom search URL</span>"
"<input id=\"search_engine\" type=\"text\" spellcheck=\"false\" "
"placeholder=\"https://example.com/?q=%s\"></label>"
"<div class=\"save-row\">"
"<button id=\"save\" class=\"btn primary\" type=\"button\">Save</button>"
"</div></div></section>\n"
"<section id=\"privacy\"><h2>Privacy</h2><div class=\"card\">"
"<label class=\"field\"><span class=\"lbl\">Cookies</span>"
"<select id=\"cookie_policy\">"
"<option value=\"0\">Accept all cookies</option>"
"<option value=\"1\">Block third-party cookies</option>"
"<option value=\"2\">Block all cookies</option></select></label>\n"
SETTINGS_SWITCH("do_not_track",
    "Send \xe2\x80\x9c" "Do Not Track\xe2\x80\x9d",
    "Ask every site not to track you.")
SETTINGS_SWITCH("global_privacy_control",
    "Send Global Privacy Control",
    "Tell sites not to sell or share your data.")
SETTINGS_SWITCH("strip_tracking_params",
    "Strip tracking parameters",
    "Remove utm_ and similar tags from links you open.")
SETTINGS_SWITCH("https_first",
    "HTTPS first",
    "Try a secure connection before plain HTTP on every site.")
SETTINGS_SWITCH("remote_control",
    "Allow remote control",
    "Let local tools such as Claude Code drive this browser through "
    "nordstjernen --mcp. Only programs on this computer can connect.")
"</div></section>\n"
"<section id=\"content\"><h2>Content</h2><div class=\"card\">\n"
SETTINGS_SWITCH("images_enabled",
    "Load images",
    "Show pictures on web pages.")
SETTINGS_SWITCH("javascript_enabled",
    "JavaScript",
    "Run scripts that pages need for interactive features.")
SETTINGS_SWITCH("webgl_enabled",
    "WebGL",
    "Allow 3D graphics in the canvas element.")
SETTINGS_SWITCH("webgpu_enabled",
    "WebGPU",
    "Allow pages to use the graphics card directly for 3D and compute work.")
SETTINGS_SWITCH("local_storage_enabled",
    "Local storage",
    "Let sites keep data on this device.")
SETTINGS_SWITCH("cache_enabled",
    "Cache",
    "Keep copies of pages and files to load them faster.")
"</div></section>\n"
"<section id=\"data\"><h2>Browsing data</h2><div class=\"card\">"
"<div class=\"danger-row\"><span class=\"txt\"><span class=\"lbl\">"
"Clear all browsing data</span><span class=\"desc\">History, cookies, "
"cache and site storage on this device.</span></span>"
"<button id=\"clear\" class=\"btn danger\">Clear data</button></div>"
"</div></section>\n"
"</main></div>\n"
"<div class=\"toast\" id=\"status\" role=\"status\"></div>\n"
"<script>\n"
"function $(i){return document.getElementById(i);}\n"
"function enc(o){var p=[];for(var k in o)p.push(encodeURIComponent(k)+'='+"
"encodeURIComponent(o[k]==null?'':o[k]));return p.join('&');}\n"
"var engines=[{n:'Google',u:'" NS_DEFAULT_SEARCH_ENGINE "'},"
"{n:'DuckDuckGo',u:'https://duckduckgo.com/?q=%s'},"
"{n:'Startpage',u:'https://www.startpage.com/sp/search?query=%s'},"
"{n:'Bing',u:'https://www.bing.com/search?q=%s'},"
"{n:'Brave',u:'https://search.brave.com/search?q=%s'},"
"{n:'Wikipedia',u:'https://en.wikipedia.org/w/index.php?search=%s'}];\n"
"var toggles=['do_not_track','global_privacy_control',"
"'strip_tracking_params','https_first','remote_control','images_enabled',"
"'javascript_enabled','webgl_enabled','webgpu_enabled','local_storage_enabled',"
"'cache_enabled'];\n"
"var loaded=false,toastTimer=0;\n"
"function toast(t){var s=$('status');s.textContent=t;s.className='toast show';"
"clearTimeout(toastTimer);toastTimer=setTimeout(function(){"
"s.className='toast';},1800);}\n"
"function buildPick(cur){var sel=$('search_pick');sel.textContent='';"
"var matched=false;engines.forEach(function(e){var o=document.createElement("
"'option');o.value=e.u;o.textContent=e.n;if(e.u===cur){o.selected=true;"
"matched=true;}sel.appendChild(o);});"
"var oc=document.createElement('option');oc.value='custom';"
"oc.textContent='Custom\\u2026';if(!matched)oc.selected=true;"
"sel.appendChild(oc);"
"$('custom_wrap').hidden=matched;}\n"
"function load(){fetch('about:settings-data').then(function(r){"
"return r.json();}).then(function(c){"
"$('home_url').value=c.home_url||'';"
"$('search_engine').value=c.search_engine||'';buildPick(c.search_engine||'');"
"$('cookie_policy').value=''+(c.cookie_policy||0);"
"toggles.forEach(function(k){$(k).checked=!!c[k];});loaded=true;});}\n"
"function bv(id){return $(id).checked?'1':'0';}\n"
"function save(o){if(!loaded)return;"
"fetch('about:settings-save',{method:'POST',headers:{'Content-Type':"
"'application/x-www-form-urlencoded'},body:enc(o)}).then(function(){"
"toast('Saved. Applies to newly opened pages.');});}\n"
"function saveField(k){var o={};o[k]=$(k).value;save(o);}\n"
"function saveGeneral(){var se=$('search_pick').value;"
"if(se!=='custom')$('search_engine').value=se;"
"save({home_url:$('home_url').value,search_engine:$('search_engine').value});}\n"
"$('search_pick').onchange=function(){"
"var v=this.value;$('custom_wrap').hidden=v!=='custom';"
"if(v!=='custom'){$('search_engine').value=v;saveField('search_engine');}};\n"
"['home_url','search_engine','cookie_policy'].forEach(function(k){"
"$(k).onchange=function(){saveField(k);};});\n"
"toggles.forEach(function(k){$(k).onchange=function(){"
"var o={};o[k]=bv(k);save(o);};});\n"
"$('save').onclick=saveGeneral;\n"
"$('clear').onclick=function(){var b=$('clear');b.disabled=true;"
"fetch('about:settings-clear',{method:'POST'}).then(function(){"
"b.textContent='Cleared';toast('Browsing data cleared');});};\n"
"load();\n"
"</script></body></html>";

static gboolean
about_request_is_post(const char *method)
{
    return method && g_ascii_strcasecmp(method, "POST") == 0;
}

static char *
about_request_form(const char *method, const void *body, gsize body_len)
{
    if (about_request_is_post(method) && body && body_len)
        return g_strndup((const char *)body, body_len);
    return NULL;
}

static void
about_emit_json(ns_response *resp, char *json)
{
    g_free(resp->content_type);
    resp->content_type = g_strdup("application/json; charset=utf-8");
    g_byte_array_append(resp->body, (const guint8 *)json, (guint)strlen(json));
    g_free(json);
}

static char *
about_json_escape(const char *s)
{
    GString *o = g_string_new(NULL);
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') g_string_append_c(o, '\\');
        if ((guchar)*s < 0x20) { g_string_append_printf(o, "\\u%04x", *s); continue; }
        g_string_append_c(o, *s);
    }
    return g_string_free(o, FALSE);
}

static char *
about_settings_json(void)
{
    ns_config_lock();
    ns_config_reload();
    const ns_config *c = ns_config_get();
    char *home = about_json_escape(c && c->home_url ? c->home_url : "");
    char *eng = about_json_escape(c && c->search_engine ? c->search_engine : "");
    char *json = g_strdup_printf(
        "{\"home_url\":\"%s\",\"search_engine\":\"%s\",\"cookie_policy\":%d,"
        "\"do_not_track\":%s,\"global_privacy_control\":%s,"
        "\"strip_tracking_params\":%s,\"https_first\":%s,"
        "\"images_enabled\":%s,\"javascript_enabled\":%s,"
        "\"webgl_enabled\":%s,\"webgpu_enabled\":%s,"
        "\"local_storage_enabled\":%s,\"cache_enabled\":%s,"
        "\"remote_control\":%s}",
        home, eng, c ? (int)c->cookie_policy : 1,
        (c && c->do_not_track) ? "true" : "false",
        (c && c->global_privacy_control) ? "true" : "false",
        (c && c->strip_tracking_params) ? "true" : "false",
        (c && c->https_first) ? "true" : "false",
        (c && c->images_enabled) ? "true" : "false",
        (c && c->javascript_enabled) ? "true" : "false",
        (c && c->webgl_enabled) ? "true" : "false",
        (c && c->webgpu_enabled) ? "true" : "false",
        (c && c->local_storage_enabled) ? "true" : "false",
        (c && c->cache_enabled) ? "true" : "false",
        (c && c->remote_control) ? "true" : "false");
    ns_config_unlock();
    g_free(home);
    g_free(eng);
    return json;
}

static void
about_settings_save(const char *form)
{
    GHashTable *q = form && *form
        ? g_uri_parse_params(form, -1, "&", G_URI_PARAMS_WWW_FORM, NULL)
        : NULL;
    if (!q) return;
    ns_config_lock();
    ns_config_reload();
    ns_config *c = ns_config_mut();
    const char *v;
    if ((v = g_hash_table_lookup(q, "home_url"))) {
        g_free(c->home_url); c->home_url = g_strdup(v);
    }
    if ((v = g_hash_table_lookup(q, "search_engine"))) {
        g_free(c->search_engine); c->search_engine = g_strdup(v);
    }
    if ((v = g_hash_table_lookup(q, "cookie_policy")))
        c->cookie_policy = (ns_cookie_policy)atoi(v);
    if ((v = g_hash_table_lookup(q, "do_not_track")))
        c->do_not_track = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "global_privacy_control")))
        c->global_privacy_control = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "strip_tracking_params")))
        c->strip_tracking_params = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "https_first")))
        c->https_first = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "images_enabled")))
        c->images_enabled = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "javascript_enabled")))
        c->javascript_enabled = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "webgl_enabled")))
        c->webgl_enabled = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "webgpu_enabled")))
        c->webgpu_enabled = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "local_storage_enabled")))
        c->local_storage_enabled = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "cache_enabled")))
        c->cache_enabled = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "remote_control")))
        c->remote_control = atoi(v) != 0;
    ns_config_save(NULL);
    ns_config_unlock();
    g_hash_table_destroy(q);
}

static void
about_settings_clear(void)
{
    ns_history_clear();
    ns_cache_clear();
    ns_net_cookies_clear();
    ns_net_site_storage_clear();
}

static gboolean
about_request_from_chrome(const char *top_url)
{
    if (!top_url || !*top_url) return TRUE;
    if (!g_str_has_prefix(top_url, "about:")) return FALSE;
    const char *page = top_url + strlen("about:");
    gsize page_len = strcspn(page, "?#");
    if (page_len == 5 && g_ascii_strncasecmp(page, "blank", 5) == 0)
        return FALSE;
    if (page_len == 6 && g_ascii_strncasecmp(page, "srcdoc", 6) == 0)
        return FALSE;
    return TRUE;
}

static gboolean
synthesize_about_response(const char *url, const char *top_url,
                          const char *method, const void *req_body,
                          gsize req_body_len, ns_response *resp)
{
    if (!g_str_has_prefix(url, "about:")) return FALSE;
    const char *what = url + strlen("about:");
    if ((g_str_has_prefix(what, "ai") || g_str_equal(what, "history") ||
         g_str_equal(what, "config") ||
         g_str_has_prefix(what, "settings")) &&
        !about_request_from_chrome(top_url)) {
        resp->status = 403;
        resp->final_url = g_strdup(url);
        resp->content_type = g_strdup("text/plain; charset=utf-8");
        const char *body = "about: pages are not available to web content";
        g_byte_array_append(resp->body, (const guint8 *)body,
                            (guint)strlen(body));
        return TRUE;
    }
    resp->status = 200;
    resp->final_url = g_strdup(url);
    resp->content_type = g_strdup("text/html; charset=utf-8");
    if (g_str_equal(what, "blank") || g_str_equal(what, "")) {
        const char *body = "<!doctype html><title>Blank</title>";
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
    } else if (g_str_equal(what, "start") || g_str_equal(what, "home") ||
               g_str_equal(what, "newtab")) {
        ns_config_lock();
        const ns_config *scfg = ns_config_get();
        g_autofree char *engine_dup =
            (scfg && scfg->search_engine && *scfg->search_engine)
            ? g_strdup(scfg->search_engine)
            : g_strdup(NS_DEFAULT_SEARCH_ENGINE);
        ns_config_unlock();
        const char *engine = engine_dup;
        GString *esc_engine = g_string_new(NULL);
        for (const char *p = engine; *p; p++) {
            if (*p == '\'' || *p == '\\' || *p == '<' || *p == '"')
                g_string_append_printf(esc_engine, "\\u%04x", (guchar)*p);
            else
                g_string_append_c(esc_engine, *p);
        }
        char *engine_host = ns_url_host_from(engine);
        const char *search_name = "the web";
        g_autofree char *engine_host_lower = NULL;
        if (engine_host && *engine_host) {
            engine_host_lower = g_ascii_strdown(engine_host, -1);
            const psl_ctx_t *psl = psl_builtin();
            const char *reg =
                psl ? psl_registrable_domain(psl, engine_host_lower) : NULL;
            search_name = (reg && *reg) ? reg
                : (g_str_has_prefix(engine_host, "www.") ? engine_host + 4
                                                         : engine_host);
        }
        char *with_search = about_substitute(k_about_start_template,
                                             "__ND_SEARCH_URL__",
                                             esc_engine->str);
        char *esc_name = g_markup_escape_text(search_name, -1);
        char *with_name = about_substitute(with_search,
                                           "__ND_SEARCH_NAME__",
                                           esc_name);
        g_free(esc_name);
        g_free(with_search);
        g_string_free(esc_engine, TRUE);
        g_free(engine_host);
        char *logo_markup = about_logo_markup();
        char *body = about_substitute(with_name, "__ND_LOGO_MARK__",
                                      logo_markup);
        g_free(logo_markup);
        g_free(with_name);
        g_byte_array_append(resp->body, (const guint8 *)body,
                            (guint)strlen(body));
        g_free(body);
    } else if (g_str_equal(what, "nordstjernen") || g_str_equal(what, "about")) {
        char *logo_markup = about_logo_markup();
        char *with_logo = about_substitute(k_about_nordstjernen_template,
                                           "__ND_LOGO_MARK__", logo_markup);
        g_free(logo_markup);
        char *security = about_security_html();
        char *with_security = about_substitute(with_logo, "__ND_SECURITY__",
                                               security);
        g_free(security);
        g_free(with_logo);
        char *diag = about_diagnostics_html();
        char *body = about_substitute(with_security, "__ND_DIAG__", diag);
        g_free(diag);
        g_free(with_security);
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
        g_free(body);
    } else if (g_str_equal(what, "mozilla")) {
        const char *body =
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>The Book of Mozilla, 11:9</title><style>"
            "body{background:maroon;color:#fff;margin:0;height:100vh;"
            "display:flex;align-items:center;justify-content:center}"
            "p{font-family:serif;font-style:italic;font-size:1.3em;"
            "max-width:34em;text-align:center;padding:0 1em}"
            "</style></head><body><p>"
            "And the beast begat a bird of fire, and the bird flew out among "
            "the people and gave the web back to them. Those who came after "
            "walked by its light, and kindled fires of their own, so that no "
            "one power should ever again hold the roads alone."
            "<br><br>from <strong>The Book of Mozilla,</strong> 11:9"
            "</p></body></html>";
        g_byte_array_append(resp->body, (const guint8 *)body,
                            (guint)strlen(body));
    } else if (g_str_equal(what, "book")) {
        const char *body =
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>The Book of Nordstjernen</title><style>"
            "body{background:#fff;color:#7a0000;margin:0;height:100vh;"
            "display:flex;align-items:center;justify-content:center}"
            "p{font-family:serif;font-style:italic;font-size:1.3em;"
            "max-width:34em;text-align:center;padding:0 1em}"
            "</style></head><body><p>"
            "And when the great engines had grown vast beyond all reckoning, "
            "a small light rose in the north, written by hand and beholden "
            "to no one. It asked for nothing, reported to nowhere, and "
            "carried the travellers over the wire by the old free roads."
            "<br><br>from <strong>The Book of Nordstjernen,</strong> 1:1"
            "</p></body></html>";
        g_byte_array_append(resp->body, (const guint8 *)body,
                            (guint)strlen(body));
    } else if (g_str_equal(what, "license") || g_str_equal(what, "licence")) {
        char *body = build_about_license();
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
        g_free(body);
    } else if (g_str_equal(what, "gpl") || g_str_equal(what, "copying")) {
        char *body = build_about_gpl();
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
        g_free(body);
    } else if (g_str_equal(what, "third-party") ||
               g_str_equal(what, "third-party-licenses") ||
               g_str_equal(what, "credits")) {
        char *body = build_about_third_party();
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
        g_free(body);
    } else if (g_str_equal(what, "history")) {
        char *body = ns_history_html_page();
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
        g_free(body);
    } else if (g_str_equal(what, "settings") ||
               g_str_equal(what, "config")) {
        g_byte_array_append(resp->body, (const guint8 *)k_about_settings_html,
                            (guint)strlen(k_about_settings_html));
    } else if (g_str_has_prefix(what, "settings-data")) {
        about_emit_json(resp, about_settings_json());
    } else if (g_str_has_prefix(what, "settings-save")) {
        char *form = about_request_form(method, req_body, req_body_len);
        gboolean saved = form != NULL;
        about_settings_save(form);
        g_free(form);
        about_emit_json(resp, g_strdup(saved ? "{\"ok\":true}"
                                             : "{\"ok\":false}"));
    } else if (g_str_has_prefix(what, "settings-clear")) {
        gboolean post = about_request_is_post(method);
        if (post)
            about_settings_clear();
        about_emit_json(resp, g_strdup(post ? "{\"ok\":true}"
                                            : "{\"ok\":false}"));
    } else {
        const char *body = "<!doctype html><title>Nordstjernen</title>";
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
    }
    return TRUE;
}

static void
view_source_append_span(GString *out, const char *cls,
                        const char *s, gsize len)
{
    if (!len) return;
    g_autofree char *chunk = g_strndup(s, len);
    g_string_append_printf(out, "<span class=\"%s\">", cls);
    ns_html_escape_append(out, chunk, FALSE);
    g_string_append(out, "</span>");
}

static void
view_source_append_plain(GString *out, const char *s, gsize len)
{
    if (!len) return;
    g_autofree char *chunk = g_strndup(s, len);
    ns_html_escape_append(out, chunk, FALSE);
}

static const char *
view_source_highlight_tag(GString *out, const char *p)
{
    const char *q = p + 1;
    if (*q == '/') q++;
    while (g_ascii_isalnum(*q) || *q == '-' || *q == ':') q++;
    view_source_append_span(out, "vs-tag", p, (gsize)(q - p));
    while (*q && *q != '>') {
        if (g_ascii_isspace(*q) || *q == '/' || *q == '=') {
            const char *ws = q;
            while (g_ascii_isspace(*q) || *q == '/' || *q == '=') q++;
            view_source_append_plain(out, ws, (gsize)(q - ws));
        } else if (*q == '"' || *q == '\'') {
            char quote = *q;
            const char *v = q++;
            while (*q && *q != quote) q++;
            if (*q == quote) q++;
            view_source_append_span(out, "vs-val", v, (gsize)(q - v));
        } else {
            const char *n = q;
            while (*q && *q != '>' && *q != '=' && *q != '/' &&
                   !g_ascii_isspace(*q))
                q++;
            gboolean is_value = FALSE;
            for (const char *b = n - 1; b > p; b--) {
                if (g_ascii_isspace(*b)) continue;
                is_value = (*b == '=');
                break;
            }
            view_source_append_span(out, is_value ? "vs-val" : "vs-attr",
                                    n, (gsize)(q - n));
        }
    }
    if (*q == '>') {
        view_source_append_span(out, "vs-tag", q, 1);
        q++;
    }
    return q;
}

static char *
view_source_document(const char *inner_url, const char *text)
{
    GString *out = g_string_new(NULL);
    g_string_append(out, "<!doctype html><html><head>"
                         "<meta charset=\"utf-8\"><title>");
    g_autofree char *esc_url = ns_html_escape_text(inner_url);
    g_string_append_printf(out, "Source of: %s</title><style>"
        "body{background:#fff;color:#000;margin:0}"
        "pre{margin:0;padding:8px;font-family:monospace;font-size:13px;"
        "line-height:1.3;tab-size:4}"
        ".vs-tag{color:#800080;font-weight:bold}"
        ".vs-attr{color:#000000;font-weight:bold}"
        ".vs-val{color:#0000c0}"
        ".vs-comment{color:#008000;font-style:italic}"
        ".vs-doctype{color:#708090}"
        "</style></head><body><pre>", esc_url);
    const char *p = text;
    while (*p) {
        if (p[0] == '<' && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            const char *end = strstr(p + 4, "-->");
            end = end ? end + 3 : p + strlen(p);
            view_source_append_span(out, "vs-comment", p, (gsize)(end - p));
            p = end;
        } else if (p[0] == '<' && (p[1] == '!' || p[1] == '?')) {
            const char *end = strchr(p, '>');
            end = end ? end + 1 : p + strlen(p);
            view_source_append_span(out, "vs-doctype", p, (gsize)(end - p));
            p = end;
        } else if (p[0] == '<' &&
                   (g_ascii_isalpha(p[1]) ||
                    (p[1] == '/' && g_ascii_isalpha(p[2])))) {
            p = view_source_highlight_tag(out, p);
        } else {
            const char *next = strchr(p + 1, '<');
            next = next ? next : p + strlen(p);
            view_source_append_plain(out, p, (gsize)(next - p));
            p = next;
        }
    }
    g_string_append(out, "</pre></body></html>");
    return g_string_free(out, FALSE);
}

static void
view_source_emit_error(ns_response *resp, const char *inner_url,
                       const char *message)
{
    resp->status = 200;
    resp->content_type = g_strdup("text/html; charset=utf-8");
    GString *out = g_string_new(
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>Source unavailable</title></head><body><p>");
    g_autofree char *esc_url = ns_html_escape_text(inner_url);
    g_autofree char *esc_msg = ns_html_escape_text(message);
    g_string_append_printf(out, "Could not load the source of %s: %s",
                           esc_url, esc_msg);
    g_string_append(out, "</p></body></html>");
    gsize len = out->len;
    g_byte_array_append(resp->body, (const guint8 *)out->str, (guint)len);
    g_string_free(out, TRUE);
}

static gboolean
synthesize_view_source_response(const char *url, const char *top_url,
                                GCancellable *cancellable, ns_response *resp)
{
    static const char prefix[] = "view-source:";
    if (!g_str_has_prefix(url, prefix)) return FALSE;
    const char *inner = url + sizeof prefix - 1;
    resp->final_url = g_strdup(url);
    gboolean from_chrome = about_request_from_chrome(top_url) ||
        g_str_has_prefix(top_url, prefix);
    gboolean inner_allowed =
        g_str_has_prefix(inner, "http:") ||
        g_str_has_prefix(inner, "https:") ||
        g_str_has_prefix(inner, "file:") ||
        g_str_has_prefix(inner, "about:") ||
        g_str_has_prefix(inner, "data:");
    if (!from_chrome || !inner_allowed) {
        resp->status = 403;
        resp->content_type = g_strdup("text/plain; charset=utf-8");
        const char *body = "view-source: is not available here";
        g_byte_array_append(resp->body, (const guint8 *)body,
                            (guint)strlen(body));
        return TRUE;
    }
    GError *err = NULL;
    ns_response *in = ns_net_request_blocking(inner, NULL, "GET", NULL, 0,
                                              NULL, NULL, cancellable, &err);
    if (!in || in->error || (in->status != 0 && in->status >= 400 &&
                             (!in->body || in->body->len == 0))) {
        const char *msg = in && in->error ? in->error
                        : err ? err->message : "request failed";
        if (in && !in->error && in->status >= 400)
            msg = "the server returned an error";
        view_source_emit_error(resp, inner, msg);
        if (in) ns_response_free(in);
        g_clear_error(&err);
        return TRUE;
    }
    g_clear_error(&err);
    g_autofree char *text = ns_html_decode_body_full(
        (const char *)in->body->data, in->body->len, in->content_type, NULL);
    g_autofree char *doc = view_source_document(inner, text ? text : "");
    resp->status = 200;
    resp->content_type = g_strdup("text/html; charset=utf-8");
    g_byte_array_append(resp->body, (const guint8 *)doc,
                        (guint)strlen(doc));
    ns_response_free(in);
    return TRUE;
}

static gboolean
is_simple_get(const char *method)
{
    return !method || !*method || g_ascii_strcasecmp(method, "GET") == 0;
}

static const struct {
    const char *name;
    gsize       offset;
} k_cached_policy_headers[] = {
    { "Content-Security-Policy", G_STRUCT_OFFSET(ns_response, csp_header) },
    { "X-Frame-Options",         G_STRUCT_OFFSET(ns_response, xframe_options) },
    { "X-Content-Type-Options",
      G_STRUCT_OFFSET(ns_response, x_content_type_options) },
    { "Content-Disposition",
      G_STRUCT_OFFSET(ns_response, content_disposition) },
    { "Content-Language",        G_STRUCT_OFFSET(ns_response, content_language) },
    { "Refresh",                 G_STRUCT_OFFSET(ns_response, refresh) },
};

static char *
response_policy_headers(const ns_response *resp)
{
    GString *out = g_string_new(NULL);
    for (gsize i = 0; i < G_N_ELEMENTS(k_cached_policy_headers); i++) {
        const char *v = G_STRUCT_MEMBER(const char *, resp,
                                        k_cached_policy_headers[i].offset);
        if (v && *v && !strpbrk(v, "\r\n"))
            g_string_append_printf(out, "%s: %s\n",
                                   k_cached_policy_headers[i].name, v);
    }
    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

static void
response_restore_policy_headers(ns_response *resp, const char *block)
{
    if (!block) return;
    for (gsize i = 0; i < G_N_ELEMENTS(k_cached_policy_headers); i++) {
        char **slot = &G_STRUCT_MEMBER(char *, resp,
                                       k_cached_policy_headers[i].offset);
        if (!*slot)
            *slot = ns_net_raw_header_values(block,
                                             k_cached_policy_headers[i].name);
    }
}

static ns_response *
response_from_cache_entry(ns_cache_entry *e)
{
    ns_response *resp = g_new0(ns_response, 1);
    resp->status       = e->status;
    resp->final_url    = g_strdup(e->final_url);
    resp->content_type = g_strdup(e->content_type);
    resp->cors_allow_origin = g_strdup(e->cors_allow_origin);
    response_restore_policy_headers(resp, e->policy_headers);
    resp->body         = e->body;
    e->body = NULL;
    return resp;
}

static gboolean ns_fetch_is_navigation(const char *top_url,
                                        GPtrArray *extra_headers);

static gboolean
ns_fetch_has_user_activation(GPtrArray *extra_headers)
{
    if (!extra_headers) return FALSE;
    for (guint i = 0; i < extra_headers->len; i++) {
        const char *header = g_ptr_array_index(extra_headers, i);
        if (header && g_ascii_strncasecmp(
                header, "X-ND-User-Activated:", 20) == 0)
            return TRUE;
    }
    return FALSE;
}

static const char *
ns_fetch_initiator(GPtrArray *extra_headers)
{
    if (!extra_headers) return NULL;
    for (guint i = 0; i < extra_headers->len; i++) {
        const char *header = g_ptr_array_index(extra_headers, i);
        if (header && g_ascii_strncasecmp(header, "X-ND-Initiator:", 15) == 0) {
            const char *v = header + 15;
            while (*v == ' ' || *v == '\t') v++;
            return *v ? v : NULL;
        }
    }
    return NULL;
}

static gboolean
ns_fetch_is_reload(GPtrArray *extra_headers)
{
    if (!extra_headers) return FALSE;
    for (guint i = 0; i < extra_headers->len; i++) {
        const char *header = g_ptr_array_index(extra_headers, i);
        if (header && g_ascii_strncasecmp(header, "X-ND-Reload:", 12) == 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
ns_fetch_is_revalidation(GPtrArray *extra_headers)
{
    if (!extra_headers) return FALSE;
    for (guint i = 0; i < extra_headers->len; i++) {
        const char *header = g_ptr_array_index(extra_headers, i);
        if (header && g_ascii_strncasecmp(header, "X-ND-Revalidate:", 16) == 0)
            return TRUE;
    }
    return FALSE;
}

static void
ns_revalidate_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void)src;
    (void)user_data;
    ns_response *resp = ns_net_fetch_finish(res, NULL);
    if (resp) ns_response_free(resp);
}

static void
ns_revalidate_in_background(const char *url, const char *top_url,
                            GPtrArray *extra_headers)
{
    GPtrArray *headers = g_ptr_array_new();
    for (guint i = 0; extra_headers && i < extra_headers->len; i++)
        g_ptr_array_add(headers, g_ptr_array_index(extra_headers, i));
    g_ptr_array_add(headers, (gpointer)"X-ND-Revalidate: 1");
    g_ptr_array_add(headers, NULL);
    if (g_log_fetches)
        fprintf(stderr, "[net cache] stale-while-revalidate %s\n", url);
    ns_net_request_async(url, top_url, "GET", NULL, 0, NULL,
                         (const char *const *)headers->pdata, NULL,
                         ns_revalidate_done, NULL);
    g_ptr_array_free(headers, TRUE);
}

static void
ns_hop_out_clear(ns_hop_out *out)
{
    if (!out) return;
    g_free(out->effective_url);
    g_free(out->remote_ip);
    g_free(out->tls_warning);
    g_free(out->error_message);
    out->effective_url = NULL;
    out->remote_ip = NULL;
    out->tls_warning = NULL;
    out->error_message = NULL;
}

static gboolean
ns_hop_transport(const ns_hop_req *req, ns_write_ctx *wctx,
                 ns_header_ctx *hctx, ns_hop_out *out,
                 GCancellable *cancellable)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        out->error_message = g_strdup("curl_easy_init failed");
        return FALSE;
    }
    if (getenv("NS_NET_TRACE"))
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    if (g_share) curl_easy_setopt(curl, CURLOPT_SHARE, g_share);

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    if (req->proxy && *req->proxy)
        curl_easy_setopt(curl, CURLOPT_PROXY, req->proxy);
    if (req->no_proxy && *req->no_proxy)
        curl_easy_setopt(curl, CURLOPT_NOPROXY, req->no_proxy);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,
                     req->follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, req->max_redirs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, req->timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, req->connect_timeout_s);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, req->user_agent);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING,
                     req->accept_encoding ? req->accept_encoding : "");
    curl_easy_setopt(curl, CURLOPT_HTTP_CONTENT_DECODING, 0L);
    switch (req->referer_policy) {
    case NS_REFERER_NO_REFERRER:
        curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 0L);
        curl_easy_setopt(curl, CURLOPT_REFERER, "");
        break;
    case NS_REFERER_UNSAFE_URL:
        curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1L);
        break;
    default:
        curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 0L);
        break;
    }
    if (req->referer && *req->referer)
        curl_easy_setopt(curl, CURLOPT_REFERER, req->referer);

    gboolean method_is_post = req->method &&
                              g_ascii_strcasecmp(req->method, "POST") == 0;
    gboolean method_is_get  = !req->method || !*req->method ||
                              g_ascii_strcasecmp(req->method, "GET") == 0;
    gboolean has_body = req->body && req->body_len > 0;
    if (method_is_post)
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (has_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->body_len);
    } else if (method_is_post) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }
    if (req->method && g_ascii_strcasecmp(req->method, "HEAD") == 0)
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    else if (!method_is_post && !method_is_get && req->method &&
             !strpbrk(req->method, "\r\n"))
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req->method);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req->headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    ns_net_apply_curl_tls(curl);

#ifdef NS_CURL_HAVE_COOKIE_SITE
    if (req->cookie_jar_path) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, req->cookie_jar_path);
        if (req->cookie_js_path)
            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, req->cookie_js_path);
        if (req->cookie_site && *req->cookie_site)
            curl_easy_setopt(curl, CURLOPT_COOKIE_SITE, req->cookie_site);
        curl_easy_setopt(curl, CURLOPT_COOKIE_CONTEXT,
                         req->cookie_navigation ? (long)CURLCOOKIE_NAVIGATION
                                                : 0L);
        hctx->cookie_curl = curl;
        hctx->cookie_jar_path = req->cookie_jar_path;
    }
#else
    if (req->cookie_jar_path && req->samesite_cross > 0) {
        char *cookie = ns_net_cookies_for_request(req->url, req->cookie_jar_path,
                                                  req->cookie_js_path,
                                                  req->samesite_cross);
        if (cookie && *cookie && req->headers) {
            g_autofree char *line = g_strdup_printf("Cookie: %s", cookie);
            curl_slist_append(req->headers, line);
        }
        g_free(cookie);
        hctx->cookie_curl = curl;
        hctx->cookie_jar_path = req->cookie_jar_path;
    } else if (req->cookie_jar_path) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, req->cookie_jar_path);
        if (req->cookie_js_path)
            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, req->cookie_js_path);
        hctx->cookie_curl = curl;
        hctx->cookie_jar_path = req->cookie_jar_path;
    }
#endif
#ifdef NS_CURL_HAVE_PRIORITY
    if (req->priority_urgency >= 0)
        curl_easy_setopt(curl, CURLOPT_PRIORITY_URGENCY,
                         (long)req->priority_urgency);
    if (req->priority_incremental)
        curl_easy_setopt(curl, CURLOPT_PRIORITY_INCREMENTAL, 1L);
#endif

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ns_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, wctx);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)wctx->budget);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ns_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, hctx);

    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https,ftp");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                     req->initial_https ? "https" :
                     (req->request_ftp ? "ftp" : "http,https"));

    const char *hsts_curl = ns_net_hsts_curl_path();
    if (hsts_curl) {
        curl_easy_setopt(curl, CURLOPT_HSTS_CTRL, (long)CURLHSTS_ENABLE);
        curl_easy_setopt(curl, CURLOPT_HSTS, hsts_curl);
    }
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, req->http_version_pref);
    const char *altsvc = ns_net_altsvc_path();
    if (altsvc)
        curl_easy_setopt(curl, CURLOPT_ALTSVC, altsvc);

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ns_xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancellable);

    CURLcode rc = ns_net_multi_perform(curl, cancellable);

    if (rc == CURLE_RECV_ERROR && hctx->location && *hctx->location &&
        g_strstr_len(errbuf, -1, "unexpected eof")) {
        long redirect_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &redirect_status);
        if (redirect_status == 301 || redirect_status == 302 ||
            redirect_status == 303 || redirect_status == 307 ||
            redirect_status == 308)
            rc = CURLE_OK;
    }

    if ((rc == CURLE_PEER_FAILED_VERIFICATION ||
         rc == CURLE_SSL_CACERT_BADFILE) &&
        g_str_has_prefix(req->url, "https://")) {
        const ns_config *cfg = ns_config_get();
        gboolean opt_in = cfg && cfg->tls_allow_insecure_override;
        char *fb_host = ns_url_host_from(req->url);
        gboolean hsts_pinned = ns_net_hsts_should_upgrade(fb_host);
        if (opt_in && !hsts_pinned && req->top_level_navigation) {
            char *warn = g_strdup_printf(
                "Insecure: TLS certificate not trusted (%s)",
                errbuf[0] ? errbuf : curl_easy_strerror(rc));
            g_byte_array_set_size(wctx->body, 0);
            wctx->total = 0;
            wctx->next_recheck = NS_NET_RESPONSE_RECHECK_BYTES;
            wctx->exceeded = FALSE;
            errbuf[0] = '\0';
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
            curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");
            hctx->cookie_jar_path = NULL;
            rc = ns_net_multi_perform(curl, cancellable);
            if (rc == CURLE_OK)
                out->tls_warning = warn;
            else
                g_free(warn);
        }
        g_free(fb_host);
    }

    hctx->cookie_curl = NULL;
    hctx->cookie_jar_path = NULL;

    if (rc == CURLE_ABORTED_BY_CALLBACK && cancellable &&
        g_cancellable_is_cancelled(cancellable)) {
        out->cancelled = TRUE;
        curl_easy_cleanup(curl);
        return FALSE;
    }

    long status = 0;
    char *eff_url = NULL;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff_url);
    out->status = status;
    out->effective_url = g_strdup(eff_url ? eff_url : req->url);
    {
        curl_off_t lookup_us = 0, connect_us = 0, tls_us = 0;
        curl_off_t pre_us = 0, start_us = 0, total_us = 0;
        curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME_T, &lookup_us);
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &connect_us);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME_T, &tls_us);
        curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME_T, &pre_us);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, &start_us);
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total_us);
        out->t_namelookup_ms   = (double)lookup_us / 1000.0;
        out->t_connect_ms      = (double)connect_us / 1000.0;
        out->t_appconnect_ms   = (double)tls_us / 1000.0;
        out->t_pretransfer_ms  = (double)pre_us / 1000.0;
        out->t_starttransfer_ms = (double)start_us / 1000.0;
        out->t_total_ms        = (double)total_us / 1000.0;
    }
    {
        char *ip = NULL;
        curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);
        if (ip && *ip) out->remote_ip = g_strdup(ip);
    }
    {
        long hv = 0, nc = 0;
        curl_easy_getinfo(curl, CURLINFO_HTTP_VERSION, &hv);
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &nc);
        out->http_version = hv;
        out->num_connects = nc;
    }
    out->tls_verify_failed = (rc == CURLE_PEER_FAILED_VERIFICATION ||
                              rc == CURLE_SSL_CACERT_BADFILE ||
                              rc == CURLE_SSL_ISSUER_ERROR);
    out->connect_failed = (rc == CURLE_COULDNT_CONNECT ||
                           rc == CURLE_OPERATION_TIMEDOUT ||
                           rc == CURLE_COULDNT_RESOLVE_HOST);
    if (rc == CURLE_FILESIZE_EXCEEDED)
        wctx->exceeded = TRUE;
    out->ok = (rc == CURLE_OK);
    if (rc != CURLE_OK && wctx->decode_failed)
        out->error_message = g_strdup("content decoding failed");
    else if (rc != CURLE_OK && !wctx->exceeded)
        out->error_message =
            g_strdup(errbuf[0] ? errbuf : curl_easy_strerror(rc));

    curl_easy_cleanup(curl);
    return TRUE;
}

static const char *const ns_script_accept_headers[] = {
    "Accept: text/javascript, application/javascript, application/ecmascript, application/x-javascript, */*;q=0.8",
    "X-ND-Fetch-Dest: script",
    NULL
};

static const char *const ns_style_accept_headers[] = {
    "Accept: text/css,*/*;q=0.1",
    "X-ND-Fetch-Dest: style",
    NULL
};

#ifdef NS_HAVE_JXL
#define NS_ACCEPT_JXL "image/jxl,"
#else
#define NS_ACCEPT_JXL ""
#endif
#ifdef NS_HAVE_AVIF
#define NS_ACCEPT_AVIF "image/avif,"
#else
#define NS_ACCEPT_AVIF ""
#endif

static const char *const ns_image_accept_headers[] = {
    "Accept: " NS_ACCEPT_JXL NS_ACCEPT_AVIF
    "image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8",
    "X-ND-Fetch-Dest: image",
    NULL
};

static const char *const ns_font_accept_headers[] = {
    "Accept: font/woff2,font/woff,application/font-woff,application/octet-stream;q=0.8,*/*;q=0.5",
    "X-ND-Fetch-Dest: font",
    NULL
};

const char *const *
ns_net_accept_headers_for(ns_fetch_destination dest)
{
    switch (dest) {
    case NS_FETCH_DEST_SCRIPT: return ns_script_accept_headers;
    case NS_FETCH_DEST_STYLE: return ns_style_accept_headers;
    case NS_FETCH_DEST_IMAGE: return ns_image_accept_headers;
    case NS_FETCH_DEST_FONT: return ns_font_accept_headers;
    default: return NULL;
    }
}

static const char *
ns_net_fetch_destination(GPtrArray *extra_headers)
{
    static const char prefix[] = "X-ND-Fetch-Dest:";
    for (guint i = 0; extra_headers && i < extra_headers->len; i++) {
        const char *h = g_ptr_array_index(extra_headers, i);
        if (!h || g_ascii_strncasecmp(h, prefix, sizeof prefix - 1) != 0)
            continue;
        h += sizeof prefix - 1;
        while (*h == ' ' || *h == '\t') h++;
        static const char *const known[] = {
            "script", "style", "image", "font", "iframe", "frame", "object",
            "embed", "worker", "sharedworker", "serviceworker",
        };
        for (gsize k = 0; k < G_N_ELEMENTS(known); k++)
            if (g_ascii_strcasecmp(h, known[k]) == 0) return known[k];
    }
    return "empty";
}

static void
ns_net_priority_for(const char *dest, gboolean is_navigation,
                    int *urgency, gboolean *incremental)
{
    *incremental = FALSE;
    if (is_navigation)                  *urgency = 0;
    else if (strcmp(dest, "style") == 0) *urgency = 1;
    else if (strcmp(dest, "font") == 0)  *urgency = 1;
    else if (strcmp(dest, "script") == 0) *urgency = 2;
    else if (strcmp(dest, "iframe") == 0 || strcmp(dest, "frame") == 0)
        *urgency = 2;
    else if (strcmp(dest, "image") == 0) {
        *urgency = 4;
        *incremental = TRUE;
    } else
        *urgency = 3;
}

static gboolean
ns_net_dest_is_nested_navigation(const char *dest)
{
    return strcmp(dest, "iframe") == 0 || strcmp(dest, "frame") == 0 ||
           strcmp(dest, "object") == 0 || strcmp(dest, "embed") == 0;
}

static gboolean
ns_net_dest_is_worker(const char *dest)
{
    return strcmp(dest, "worker") == 0 || strcmp(dest, "sharedworker") == 0 ||
           strcmp(dest, "serviceworker") == 0;
}

static char *
ns_net_partition_key(const char *url, const char *top_url)
{
    const ns_config *cfg = ns_config_get();
    const char *ua = (cfg && cfg->user_agent && *cfg->user_agent)
        ? cfg->user_agent
        : ns_user_agent_for_mode(cfg ? cfg->compat_mode : NULL);
    const char *effective_top_url = top_url ? top_url : url;
    g_autofree char *top_origin = ns_url_origin_from(effective_top_url);
    g_autofree char *top_site   = ns_url_site_from(effective_top_url);
    const char *partition = (top_site && *top_site) ? top_site
                          : (top_origin ? top_origin : "");
    return g_strdup_printf("top=%s\x1f" "ua=%s\x1f" "al=%s", partition, ua,
                           ns_net_effective_accept_language());
}

static const char *
ns_net_request_origin(const char *url, const char *top_url,
                      const char *top_origin, const char *method)
{
    if (!top_origin || !*top_origin) return "";
    if (strpbrk(top_origin, "\r\n") || strlen(top_origin) >= 4096) return "";
    if (top_url && !ns_url_same_origin(top_url, url)) return top_origin;
    if (method && *method &&
        g_ascii_strcasecmp(method, "GET") != 0 &&
        g_ascii_strcasecmp(method, "HEAD") != 0)
        return top_origin;
    return "";
}

static int
ns_header_line_cmp(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(const char *const *)a, *(const char *const *)b);
}

char *
ns_net_request_key(const char *url, const char *top_url, const char *method,
                   const char *const *extra_headers)
{
    if (!url || !ns_url_is_http_or_https(url)) return NULL;
    if (method && *method && g_ascii_strcasecmp(method, "GET") != 0) return NULL;
    g_autofree char *partition = ns_net_partition_key(url, top_url);
    GString *key = g_string_new("GET\x1f");
    g_string_append(key, url);
    g_string_append_c(key, '\x1f');
    g_string_append(key, partition);
    GPtrArray *sorted = g_ptr_array_new();
    for (int i = 0; extra_headers && extra_headers[i]; i++)
        if (g_ascii_strncasecmp(extra_headers[i], "X-ND-CORS:", 10) != 0)
            g_ptr_array_add(sorted, (gpointer)extra_headers[i]);
    g_ptr_array_sort(sorted, ns_header_line_cmp);
    for (guint i = 0; i < sorted->len; i++) {
        g_string_append_c(key, '\x1f');
        g_string_append(key, g_ptr_array_index(sorted, i));
    }
    g_ptr_array_free(sorted, TRUE);
    return g_string_free(key, FALSE);
}

static gboolean ns_fetch_wants_cors(GPtrArray *extra_headers);

typedef struct {
    gconstpointer owner;
    char         *doc_url;
    ns_csp       *csp;
} ns_net_csp_entry;

static GMutex      g_csp_lock;
static GHashTable *g_csp_by_origin;

static void
ns_net_csp_entry_free(gpointer p)
{
    ns_net_csp_entry *e = p;
    if (!e) return;
    g_free(e->doc_url);
    if (e->csp) ns_csp_free(e->csp);
    g_free(e);
}

void
ns_net_csp_register(gconstpointer owner, const char *doc_url,
                    const char *const *headers)
{
    g_autofree char *origin = doc_url ? ns_url_origin_from(doc_url) : NULL;
    if (!origin || !*origin || strcmp(origin, "null") == 0) return;
    ns_csp *merged = NULL;
    for (const char *const *h = headers; h && *h; h++) {
        ns_csp *parsed = ns_csp_parse(*h);
        if (!parsed) continue;
        if (merged) {
            ns_csp_merge(merged, parsed);
            ns_csp_free(parsed);
        } else {
            merged = parsed;
        }
    }
    if (!merged) return;
    ns_net_csp_entry *e = g_new0(ns_net_csp_entry, 1);
    e->owner = owner;
    e->doc_url = g_strdup(doc_url);
    e->csp = merged;
    g_mutex_lock(&g_csp_lock);
    if (!g_csp_by_origin)
        g_csp_by_origin = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, ns_net_csp_entry_free);
    g_hash_table_replace(g_csp_by_origin, g_steal_pointer(&origin), e);
    g_mutex_unlock(&g_csp_lock);
}

void
ns_net_csp_unregister(gconstpointer owner)
{
    g_mutex_lock(&g_csp_lock);
    if (g_csp_by_origin) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, g_csp_by_origin);
        while (g_hash_table_iter_next(&it, &k, &v))
            if (((ns_net_csp_entry *)v)->owner == owner)
                g_hash_table_iter_remove(&it);
    }
    g_mutex_unlock(&g_csp_lock);
}

gboolean
ns_net_csp_allows(const char *doc_url, ns_csp_kind kind, const char *url,
                  const char *nonce, gboolean parser_inserted)
{
    if (!doc_url || !url) return TRUE;
    g_autofree char *origin = ns_url_origin_from(doc_url);
    if (!origin || !*origin) return TRUE;
    gboolean ok = TRUE;
    g_mutex_lock(&g_csp_lock);
    ns_net_csp_entry *e = g_csp_by_origin
        ? g_hash_table_lookup(g_csp_by_origin, origin) : NULL;
    if (e)
        ok = ns_csp_allows_with_nonce(e->csp, kind, url, e->doc_url, nonce,
                                      parser_inserted);
    g_mutex_unlock(&g_csp_lock);
    return ok;
}

static ns_csp_kind
ns_net_hop_csp_kind(GPtrArray *extra_headers)
{
    const char *dest = ns_net_fetch_destination(extra_headers);
    if (strcmp(dest, "image") == 0) return NS_CSP_IMG;
    if (strcmp(dest, "font") == 0) return NS_CSP_FONT;
    if (ns_fetch_wants_cors(extra_headers)) return NS_CSP_CONNECT;
    return NS_CSP_KIND_COUNT;
}

static ns_response *
ns_fetch_sync_hop(const char *url, const char *top_url, const char *method,
                  const void *body, gsize body_len, const char *content_type,
                  GPtrArray *extra_headers,
                  GCancellable *cancellable, GError **error,
                  gboolean follow_redirects, char **location_out)
{
    if (location_out) *location_out = NULL;
    gboolean is_navigation = ns_fetch_is_navigation(top_url, extra_headers);
    gboolean user_activated = ns_fetch_has_user_activation(extra_headers);
    gboolean reload = (is_navigation && ns_fetch_is_reload(extra_headers)) ||
                      ns_fetch_is_revalidation(extra_headers);
    const char *initiator = is_navigation && !top_url
        ? ns_fetch_initiator(extra_headers) : NULL;
    const char *request_site_url = initiator ? initiator : top_url;
    ns_response *resp = g_new0(ns_response, 1);
    resp->body = g_byte_array_new();
    ns_csp_kind policy_kind = is_navigation ? NS_CSP_KIND_COUNT
                                            : ns_net_hop_csp_kind(extra_headers);
    if (policy_kind != NS_CSP_KIND_COUNT &&
        !ns_net_csp_allows(top_url, policy_kind, url, NULL, FALSE)) {
        resp->final_url = g_strdup(url);
        resp->error = g_strdup("blocked by Content-Security-Policy");
        return resp;
    }

    if (!is_navigation) {
        char *dead_host = ns_url_origin_from_any(url);
        gboolean dead = dead_host && *dead_host &&
                        ns_net_host_recently_dead(dead_host);
        g_free(dead_host);
        if (dead) {
            resp->error =
                g_strdup("host unreachable (recent connection failure)");
            resp->final_url = g_strdup(url);
            return resp;
        }
    }

    if (synthesize_about_response(url, top_url, method, body, body_len, resp))
        return resp;
    if (synthesize_view_source_response(url, top_url, cancellable, resp))
        return resp;
    if (synthesize_data_response(url, resp))
        return resp;
    if (synthesize_file_response(url, top_url, resp))
        return resp;

    char *hsts_upgraded = NULL;
    char *idn_ascii = ns_url_to_ascii(url);
    if (idn_ascii && strcmp(idn_ascii, url) != 0) {
        hsts_upgraded = idn_ascii;
        url = hsts_upgraded;
    } else {
        g_free(idn_ascii);
    }

    char *https_url = ns_net_hsts_upgrade(url);
    if (https_url) {
        g_free(hsts_upgraded);
        hsts_upgraded = https_url;
        url = hsts_upgraded;
    }

    gboolean request_http = ns_url_is_http_or_https(url);
    gboolean request_ftp = ns_url_is_ftp(url);
    if (request_ftp && top_url && *top_url && !is_navigation &&
        !ns_url_is_ftp(top_url)) {
        resp->final_url = g_strdup(url);
        resp->status = 0;
        resp->error = g_strdup("FTP access is not allowed from this page");
        g_free(hsts_upgraded);
        return resp;
    }
    const ns_config *cfg = ns_config_get();
    const char *configured_ua =
        (cfg && cfg->user_agent && *cfg->user_agent) ? cfg->user_agent
            : ns_user_agent_for_mode(cfg ? cfg->compat_mode : NULL);
    const char *effective_ua = configured_ua;
    const char *accept_language = ns_net_effective_accept_language();
    const char *effective_top_url = top_url ? top_url : url;
    char *top_origin = ns_url_origin_from(effective_top_url);
    char *top_site   = ns_url_site_from(effective_top_url);
    const char *partition_key = (top_site && *top_site) ? top_site
                              : (top_origin ? top_origin : "");
    char *cache_partition = ns_net_partition_key(url, top_url);
    const char *request_accept = "*/*";
    for (guint i = 0; extra_headers && i < extra_headers->len; i++) {
        const char *h = g_ptr_array_index(extra_headers, i);
        if (g_ascii_strncasecmp(h, "Accept:", 7) != 0) continue;
        for (h += 7; *h == ' ' || *h == '\t'; h++) {}
        request_accept = h;
        break;
    }
    g_autofree char *vary_accept =
        g_strdup_printf("Accept: %s", request_accept);
    g_autofree char *vary_language =
        g_strdup_printf("Accept-Language: %s", accept_language);
    g_autofree char *vary_agent =
        g_strdup_printf("User-Agent: %s", effective_ua);
    const char *request_origin = initiator
        ? (is_simple_get(method) ? ""
                                 : ns_net_request_origin(url, initiator,
                                                         initiator, method))
        : ns_net_request_origin(url, top_url, top_origin, method);
    g_autofree char *vary_origin =
        g_strdup_printf("Origin: %s", request_origin);
    const char *const cache_request_headers[] = {
        vary_accept, vary_language, vary_agent, vary_origin, NULL
    };
    ns_cookie_policy cookie_policy = cfg ? cfg->cookie_policy : NS_COOKIE_FIRST_PARTY;
    gboolean cookies_allowed = request_http &&
        (cookie_policy != NS_COOKIE_NEVER);
    if (cookies_allowed && cookie_policy == NS_COOKIE_FIRST_PARTY &&
        top_url && !ns_url_is_same_site(url, effective_top_url))
        cookies_allowed = FALSE;
    if (!*partition_key)
        cookies_allowed = FALSE;
    int samesite_cross = 0;
    const char *same_site_ref = initiator ? initiator : top_url;
    if (cookies_allowed && same_site_ref && *same_site_ref &&
        !ns_url_is_same_site(url, same_site_ref))
        samesite_cross = is_navigation && is_simple_get(method) ? 1 : 2;
    char *cookie_partition_path = cookies_allowed
        ? ns_net_cookie_path_for_partition(partition_key) : NULL;
    g_autofree char *cookie_site = (cookies_allowed && same_site_ref &&
                                    *same_site_ref)
        ? ns_net_curl_site_from(same_site_ref) : NULL;
    int priority_urgency = -1;
    gboolean priority_incremental = FALSE;
    ns_net_priority_for(ns_net_fetch_destination(extra_headers),
                        is_navigation, &priority_urgency,
                        &priority_incremental);
    if (getenv("NS_NET_LOG"))
        fprintf(stderr, "NS_NET cookie_site=%s nav=%d jar=%d u=%d i=%d %s\n",
                cookie_site ? cookie_site : "(none)", is_navigation,
                cookie_partition_path != NULL, priority_urgency,
                priority_incremental, url);

    gboolean streams_to_disk = g_hop_stream && g_hop_stream->discard_body;
    ns_cache_entry *cached = NULL;
    if (request_http && is_simple_get(method) && !streams_to_disk) {
        cached = ns_cache_get(url, cache_partition, cache_request_headers);
        gboolean serve_stale = cached && !reload &&
                               ns_cache_may_serve_stale(cached);
        if (cached && !reload && (ns_cache_is_fresh(cached) || serve_stale)) {
            gboolean cache_has_cors =
                cached->cors_allow_origin ||
                ns_url_same_origin(effective_top_url, cached->final_url);
            if (!cache_has_cors) {
                ns_cache_entry_free(cached);
                cached = NULL;
            } else {
                if (serve_stale)
                    ns_revalidate_in_background(url, top_url, extra_headers);
                ns_response_free(resp);
                ns_response *from_cache = response_from_cache_entry(cached);
                ns_cache_entry_free(cached);
                g_free(cache_partition);
                g_free(cookie_partition_path);
                g_free(top_origin);
                g_free(top_site);
                g_free(hsts_upgraded);
                return from_cache;
            }
        }
    }

    char *referer = ns_net_referer_for(url, top_url,
        cfg ? cfg->referer_policy : NS_REFERER_STRICT_ORIGIN_WHEN_CROSS);
    char *origin_slot = ns_url_origin_from(url);
    gboolean origin_held = FALSE;
    if (origin_slot) {
        origin_held = ns_net_acquire_origin_slot(origin_slot, cancellable);
        if (!origin_held) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "fetch cancelled");
            g_free(origin_slot);
            g_free(referer);
            g_free(cache_partition);
            g_free(cookie_partition_path);
            g_free(top_origin);
            g_free(top_site);
            g_free(hsts_upgraded);
            ns_cache_entry_free(cached);
            ns_response_free(resp);
            return NULL;
        }
    }

    if (getenv("NS_NET_LOG"))
        fprintf(stderr, "NS_NET %s %s\n", method ? method : "GET", url);

    long max_redirs = cfg ? (long)cfg->max_redirects : (long)NS_MAX_REDIRECTS;
    if (max_redirs < 0)                       max_redirs = 0;
    if (max_redirs > (long)NS_MAX_REDIRECTS)  max_redirs = (long)NS_MAX_REDIRECTS;

    long fetch_timeout = (long)NS_DEFAULT_TIMEOUT_S;
    if (extra_headers) {
        for (guint i = 0; i < extra_headers->len; i++) {
            const char *h = g_ptr_array_index(extra_headers, i);
            if (h && g_str_has_prefix(h, "X-ND-Timeout-Seconds:")) {
                fetch_timeout = (long)g_ascii_strtoll(
                    h + strlen("X-ND-Timeout-Seconds:"), NULL, 10);
                break;
            }
        }
    }
    if (fetch_timeout < 1) fetch_timeout = 1;
    if (fetch_timeout > (long)NS_MAX_TIMEOUT_S) fetch_timeout = (long)NS_MAX_TIMEOUT_S;
    if (g_hop_stream) fetch_timeout = (long)NS_STREAM_TIMEOUT_S;

    gboolean caller_set_accept = FALSE;
    if (extra_headers) {
        for (guint i = 0; i < extra_headers->len; i++) {
            const char *h = g_ptr_array_index(extra_headers, i);
            if (h && g_ascii_strncasecmp(h, "Accept:", 7) == 0) {
                caller_set_accept = TRUE;
                break;
            }
        }
    }

    struct curl_slist *headers = NULL;
    {
        char *h = g_strdup_printf("Accept-Language: %s", accept_language);
        headers = curl_slist_append(headers, h);
        g_free(h);
    }
    const char *fetch_dest = is_navigation
        ? "document" : ns_net_fetch_destination(extra_headers);
    gboolean navigates = is_navigation ||
                         ns_net_dest_is_nested_navigation(fetch_dest);
    if (!caller_set_accept) {
        headers = curl_slist_append(headers, navigates
            ? "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
            : "Accept: */*");
    }
    if (!cfg || cfg->do_not_track)
        headers = curl_slist_append(headers, "DNT: 1");

    if (*request_origin)
        headers = curl_slist_append(headers, vary_origin);

    if (reload)
        headers = curl_slist_append(headers, "Cache-Control: max-age=0");
    if (cached && cached->etag) {
        char *h = g_strdup_printf("If-None-Match: %s", cached->etag);
        headers = curl_slist_append(headers, h);
        g_free(h);
    }
    if (cached && cached->last_modified) {
        char *h = g_strdup_printf("If-Modified-Since: %s", cached->last_modified);
        headers = curl_slist_append(headers, h);
        g_free(h);
    }

    if (request_http) {
        const char *fetch_site;
        if (!request_site_url || !*request_site_url) {
            fetch_site = "none";
        } else if (ns_url_same_origin(request_site_url, url)) {
            fetch_site = "same-origin";
        } else if (ns_url_is_same_site(request_site_url, url)) {
            fetch_site = "same-site";
        } else {
            fetch_site = "cross-site";
        }
        char *site_h = g_strdup_printf("Sec-Fetch-Site: %s", fetch_site);
        headers = curl_slist_append(headers, site_h);
        g_free(site_h);

        const char *fetch_mode;
        if (navigates) {
            fetch_mode = "navigate";
        } else if (ns_net_dest_is_worker(fetch_dest)) {
            fetch_mode = "same-origin";
        } else if (method && *method &&
                   g_ascii_strcasecmp(method, "GET") != 0 &&
                   g_ascii_strcasecmp(method, "HEAD") != 0) {
            fetch_mode = "cors";
        } else {
            fetch_mode = "no-cors";
        }
        char *mode_h = g_strdup_printf("Sec-Fetch-Mode: %s", fetch_mode);
        headers = curl_slist_append(headers, mode_h);
        g_free(mode_h);

        char *dest_h = g_strdup_printf("Sec-Fetch-Dest: %s", fetch_dest);
        headers = curl_slist_append(headers, dest_h);
        g_free(dest_h);

        if (is_navigation && user_activated)
            headers = curl_slist_append(headers, "Sec-Fetch-User: ?1");
        if (navigates) {
            headers = curl_slist_append(headers,
                                        "Upgrade-Insecure-Requests: 1");
        }

        gboolean chromium_ua = ns_user_agent_has_client_hints(effective_ua);
        if (chromium_ua) {
            headers = curl_slist_append(headers,
                "Sec-CH-UA: \"Nordstjernen\";v=\"1\", "
                "\"Not=A?Brand\";v=\"24\"");
            char *ua_mobile = g_strdup_printf("Sec-CH-UA-Mobile: ?%d",
                                              ns_net_is_mobile_mode() ? 1 : 0);
            headers = curl_slist_append(headers, ua_mobile);
            g_free(ua_mobile);
            char *ua_plat = g_strdup_printf("Sec-CH-UA-Platform: \"%s\"",
                                            ns_net_ua_hint_platform());
            headers = curl_slist_append(headers, ua_plat);
            g_free(ua_plat);
        }

        if (!cfg || cfg->global_privacy_control) {
            headers = curl_slist_append(headers, "Sec-GPC: 1");
        }
    }

    gboolean method_is_post = method && g_ascii_strcasecmp(method, "POST") == 0;
    gboolean method_is_get  = !method || !*method ||
                              g_ascii_strcasecmp(method, "GET") == 0;
    gboolean has_body = body && body_len > 0;
    gboolean extra_has_ct = FALSE;
    if (extra_headers) {
        for (guint i = 0; i < extra_headers->len; i++) {
            const char *h = g_ptr_array_index(extra_headers, i);
            if (h && g_ascii_strncasecmp(h, "Content-Type:", 13) == 0) {
                extra_has_ct = TRUE;
                break;
            }
        }
    }
    if (!extra_has_ct && (method_is_post || (has_body && !method_is_get && method))) {
        char *ct_hdr = content_type && *content_type
            ? g_strdup_printf("Content-Type: %s", content_type)
            : g_strdup(is_navigation && has_body
                       ? "Content-Type: application/x-www-form-urlencoded"
                       : "Content-Type:");
        headers = curl_slist_append(headers, ct_hdr);
        g_free(ct_hdr);
    }

    if (extra_headers) {
        for (guint i = 0; i < extra_headers->len; i++) {
            const char *h = g_ptr_array_index(extra_headers, i);
            if (!h || !*h) continue;
            if (g_ascii_strncasecmp(h, "X-ND-", 5) == 0) continue;
            if (strpbrk(h, "\r\n")) continue;
            const char *colon = strchr(h, ':');
            const char *v = colon ? colon + 1 : NULL;
            while (v && (*v == ' ' || *v == '\t')) v++;
            if (colon && colon > h && v && !*v) {
                char *empty = g_strdup_printf("%.*s;", (int)(colon - h), h);
                headers = curl_slist_append(headers, empty);
                g_free(empty);
                continue;
            }
            headers = curl_slist_append(headers, h);
        }
    }

    ns_write_ctx write_ctx;
    ns_body_sink_init(&write_ctx, resp->body);

    ns_header_ctx header_ctx = {0};
    write_ctx.stream = g_hop_stream;
    write_ctx.stream_headers = &header_ctx;
    header_ctx.content_type_out = &resp->content_type;
    header_ctx.content_disposition_out = &resp->content_disposition;
    header_ctx.csp_out          = &resp->csp_header;
    header_ctx.xframe_options_out = &resp->xframe_options;
    header_ctx.x_content_type_options_out = &resp->x_content_type_options;
    header_ctx.cors_allow_origin_out = &resp->cors_allow_origin;
    header_ctx.refresh_out = &resp->refresh;
    header_ctx.content_language_out = &resp->content_language;
    header_ctx.url = url;
    header_ctx.early_hints = is_navigation && request_http && is_simple_get(method);

    gboolean initial_https = g_str_has_prefix(url, "https://");
    char *cookie_js_path = cookie_partition_path
        ? ns_net_cookie_js_path_for_partition(partition_key) : NULL;

    ns_hop_req req = {
        .url = url,
        .method = method,
        .body = body,
        .body_len = body_len,
        .headers = headers,
        .user_agent = effective_ua,
        .referer = referer,
        .referer_policy = cfg ? (int)cfg->referer_policy
                              : (int)NS_REFERER_STRICT_ORIGIN_WHEN_CROSS,
        .accept_encoding = g_accept_encoding ? g_accept_encoding : "",
        .timeout_s = fetch_timeout,
        .connect_timeout_s = is_navigation ? 15L : 6L,
        .proxy = ns_net_pick_configured_proxy(url),
        .no_proxy = ns_net_configured_no_proxy(),
        .cookie_jar_path = cookie_partition_path,
        .cookie_js_path = cookie_js_path,
        .samesite_cross = samesite_cross,
        .cookie_site = cookie_site,
        .cookie_navigation = is_navigation,
        .top_level_navigation = is_navigation,
        .priority_urgency = priority_urgency,
        .priority_incremental = priority_incremental,
        .follow_redirects = follow_redirects,
        .max_redirs = max_redirs,
        .request_ftp = request_ftp,
        .initial_https = initial_https,
        .http_version_pref = ns_net_http_version(),
    };

    ns_hop_out out = {0};
    gint64 fetch_start_us = g_get_monotonic_time();
    double fetch_start_real_ms = (double)g_get_real_time() / 1000.0;
    gboolean produced = ns_hop_transport(&req, &write_ctx, &header_ctx, &out,
                                         cancellable);
    g_free(cookie_js_path);

    if (!produced) {
        if (out.cancelled)
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "fetch cancelled");
        else
            g_set_error_literal(error, NS_NET_DOMAIN, 1,
                                out.error_message ? out.error_message
                                                  : "transport init failed");
        ns_hop_out_clear(&out);
        if (headers) curl_slist_free_all(headers);
        g_free(header_ctx.etag);
        g_free(header_ctx.last_modified);
        g_free(header_ctx.cache_control);
        g_free(header_ctx.vary);
        g_free(header_ctx.expires);
        g_free(header_ctx.date);
        g_free(header_ctx.age);
        g_free(header_ctx.location);
        if (header_ctx.raw) g_string_free(header_ctx.raw, TRUE);
        if (header_ctx.early_links) g_ptr_array_free(header_ctx.early_links, TRUE);
        ns_cache_entry_free(cached);
        ns_response_free(resp);
        if (origin_held) ns_net_release_origin_slot(origin_slot);
        g_free(origin_slot);
        g_free(referer);
        g_free(cache_partition);
        g_free(cookie_partition_path);
        g_free(top_origin);
        g_free(top_site);
        g_free(hsts_upgraded);
        return NULL;
    }

    gboolean transport_ok = out.ok;
    resp->status = out.status;
    resp->final_url = g_strdup(out.effective_url ? out.effective_url : url);
    resp->redirect_count = 0;
    resp->request_start_us = fetch_start_us;
    resp->request_start_real_ms = fetch_start_real_ms;
    resp->domain_lookup_ms = out.t_namelookup_ms;
    resp->connect_ms = out.t_connect_ms;
    resp->tls_ms = out.t_appconnect_ms;
    resp->pretransfer_ms = out.t_pretransfer_ms;
    resp->response_start_ms = out.t_starttransfer_ms;
    resp->response_end_ms = out.t_total_ms;
    if (out.remote_ip)
        resp->remote_ip = g_strdup(out.remote_ip);
    if (out.http_version)
        resp->next_hop_protocol =
            g_strdup(ns_net_http_version_name(out.http_version));
    if (out.tls_warning)
        resp->tls_warning = g_strdup(out.tls_warning);
    {
        const char *sec_url = resp->final_url ? resp->final_url : url;
        if (g_str_has_prefix(sec_url, "https://")) {
            if (out.tls_verify_failed || resp->tls_warning)
                resp->security = NS_SEC_INVALID;
            else if (transport_ok)
                resp->security = NS_SEC_SECURE;
        } else if (g_str_has_prefix(sec_url, "http://")) {
            resp->security = NS_SEC_PLAIN;
        }
    }
    if (g_log_fetches)
        ns_net_conn_stat_record(resp->final_url, out.http_version,
                                out.num_connects);
    if (transport_ok && request_ftp) {
        maybe_synthesize_ftp_listing(resp);
        maybe_guess_ftp_content_type(resp);
    }

    {
        char *reach_host = ns_url_origin_from_any(url);
        if (reach_host && *reach_host) {
            if (transport_ok || out.status > 0)
                ns_net_host_mark_alive(reach_host);
            else if (out.status == 0 && out.connect_failed)
                ns_net_host_mark_dead(reach_host);
        }
        g_free(reach_host);
    }

    if (!transport_ok) {
        if (write_ctx.exceeded)
            resp->error = g_strdup_printf(
                "response would exhaust available memory (stopped at %llu MiB)",
                (unsigned long long)(write_ctx.total >> 20));
        else
            resp->error = g_strdup(out.error_message ? out.error_message
                                                     : "transport error");
    }
    ns_hop_out_clear(&out);

    if (transport_ok && request_http && is_simple_get(method) &&
        !header_ctx.set_cookie_seen &&
        !resp->tls_warning) {
        if (resp->status == 304 && cached && cached->body) {
            ns_cache_promote_304(url, cache_partition, cache_request_headers,
                                 header_ctx.cache_control, header_ctx.expires,
                                 header_ctx.date, header_ctx.age,
                                 header_ctx.last_modified);
            g_byte_array_set_size(resp->body, 0);
            g_byte_array_append(resp->body, cached->body->data, cached->body->len);
            resp->status = cached->status;
            g_free(resp->content_type);
            resp->content_type = g_strdup(cached->content_type);
            g_free(resp->cors_allow_origin);
            resp->cors_allow_origin = g_strdup(cached->cors_allow_origin);
            response_restore_policy_headers(resp, cached->policy_headers);
        } else if (resp->status > 0 && resp->status < 300 &&
                   resp->body && resp->body->len > 0) {
            g_autofree char *policy_headers = response_policy_headers(resp);
            ns_cache_put(url, cache_partition,
                         resp->final_url, resp->status,
                         resp->content_type,
                         resp->cors_allow_origin,
                         policy_headers,
                         header_ctx.etag, header_ctx.last_modified,
                         header_ctx.cache_control, header_ctx.expires,
                         header_ctx.date, header_ctx.age,
                         header_ctx.vary, cache_request_headers,
                         resp->body->data, resp->body->len);
        }
    }
    g_free(referer);
    g_free(cache_partition);
    g_free(cookie_partition_path);
    g_free(top_origin);
    g_free(top_site);

    g_free(header_ctx.etag);
    g_free(header_ctx.last_modified);
    g_free(header_ctx.cache_control);
    g_free(header_ctx.vary);
    g_free(header_ctx.expires);
    g_free(header_ctx.date);
    g_free(header_ctx.age);
    if (header_ctx.early_links) g_ptr_array_free(header_ctx.early_links, TRUE);
    if (location_out)
        *location_out = header_ctx.location;
    else
        g_free(header_ctx.location);
    if (header_ctx.raw) {
        g_free(resp->raw_headers);
        resp->raw_headers = g_string_free(header_ctx.raw, FALSE);
    }
    g_free(header_ctx.content_encoding);
    ns_write_ctx_clear(&write_ctx);
    ns_cache_entry_free(cached);

    {
        gint64 fetch_end_us = g_get_monotonic_time();
        ns_net_perf_record(fetch_start_us, fetch_end_us,
                           resp->body ? (guint64)resp->body->len : 0);
        char *req_hdrs = ns_net_slist_serialize(headers);
        ns_net_log_record(method, url, resp->status, resp->content_type,
                          resp->body ? (guint64)resp->body->len : 0,
                          (fetch_end_us - fetch_start_us) / 1000.0,
                          req_hdrs, resp->raw_headers, resp->error);
        g_free(req_hdrs);
    }

    if (headers) curl_slist_free_all(headers);
    if (origin_held) ns_net_release_origin_slot(origin_slot);
    g_free(origin_slot);
    g_free(hsts_upgraded);
    return resp;
}

static gboolean
ns_net_safebrowsing_blocks(const char *url)
{
    g_autofree char *host = ns_url_host_from(url);
    return host && ns_safebrowsing_blocked(host);
}

static gboolean
ns_fetch_is_navigation(const char *top_url, GPtrArray *extra_headers)
{
    (void)top_url;
    if (!extra_headers) return FALSE;
    for (guint i = 0; i < extra_headers->len; i++) {
        const char *h = g_ptr_array_index(extra_headers, i);
        if (h && g_ascii_strncasecmp(h, "X-ND-Navigate:", 14) == 0)
            return TRUE;
    }
    return FALSE;
}

static void
ns_headers_strip_sensitive(GPtrArray *headers)
{
    static const char *const sensitive[] = {
        "authorization", "cookie", "proxy-authorization", NULL,
    };
    for (guint i = 0; i < headers->len; ) {
        const char *h = g_ptr_array_index(headers, i);
        const char *colon = h ? strchr(h, ':') : NULL;
        gboolean drop = FALSE;
        if (colon) {
            size_t nlen = (size_t)(colon - h);
            for (int s = 0; sensitive[s]; s++)
                if (strlen(sensitive[s]) == nlen &&
                    g_ascii_strncasecmp(h, sensitive[s], nlen) == 0) {
                    drop = TRUE;
                    break;
                }
        }
        if (drop)
            g_ptr_array_remove_index(headers, i);
        else
            i++;
    }
}

#define NS_CORS_PREFLIGHT_CACHE_MAX 256

static GMutex      g_cors_preflight_lock;
static GHashTable *g_cors_preflight_cache;

static gboolean
ns_cors_method_is_simple(const char *method)
{
    return !method || !*method ||
           g_ascii_strcasecmp(method, "GET") == 0 ||
           g_ascii_strcasecmp(method, "HEAD") == 0 ||
           g_ascii_strcasecmp(method, "POST") == 0;
}

static gboolean
ns_cors_content_type_is_simple(const char *ct)
{
    if (!ct) return TRUE;
    while (*ct == ' ' || *ct == '\t') ct++;
    gsize n = strcspn(ct, "; \t");
    static const char *const simple[] = {
        "application/x-www-form-urlencoded", "multipart/form-data",
        "text/plain", NULL,
    };
    for (int i = 0; simple[i]; i++)
        if (strlen(simple[i]) == n && g_ascii_strncasecmp(ct, simple[i], n) == 0)
            return TRUE;
    return FALSE;
}

static gboolean
ns_cors_header_is_simple(const char *name, const char *value)
{
    if (g_ascii_strcasecmp(name, "accept") == 0 ||
        g_ascii_strcasecmp(name, "accept-language") == 0 ||
        g_ascii_strcasecmp(name, "content-language") == 0)
        return TRUE;
    if (g_ascii_strcasecmp(name, "content-type") == 0)
        return ns_cors_content_type_is_simple(value);
    return FALSE;
}

static char *
ns_cors_unsafe_header_names(GPtrArray *extra_headers, const char *content_type)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; extra_headers && i < extra_headers->len; i++) {
        const char *h = g_ptr_array_index(extra_headers, i);
        const char *colon = h ? strchr(h, ':') : NULL;
        if (!colon || colon == h) continue;
        if (g_ascii_strncasecmp(h, "X-ND-", 5) == 0) continue;
        char *name = g_ascii_strdown(h, colon - h);
        g_strstrip(name);
        const char *value = colon + 1;
        if (ns_cors_header_is_simple(name, value)) {
            g_free(name);
            continue;
        }
        gboolean dup = FALSE;
        for (guint j = 0; j < names->len && !dup; j++)
            dup = strcmp(g_ptr_array_index(names, j), name) == 0;
        if (dup) g_free(name);
        else g_ptr_array_add(names, name);
    }
    if (!ns_cors_content_type_is_simple(content_type)) {
        gboolean dup = FALSE;
        for (guint j = 0; j < names->len && !dup; j++)
            dup = strcmp(g_ptr_array_index(names, j), "content-type") == 0;
        if (!dup) g_ptr_array_add(names, g_strdup("content-type"));
    }
    if (names->len == 0) {
        g_ptr_array_free(names, TRUE);
        return NULL;
    }
    g_ptr_array_sort(names, ns_header_line_cmp);
    g_ptr_array_add(names, NULL);
    char *out = g_strjoinv(",", (char **)names->pdata);
    g_ptr_array_free(names, TRUE);
    return out;
}

static gboolean
ns_cors_list_allows(const char *list, const char *item, gboolean star_ok)
{
    if (!list) return FALSE;
    gchar **parts = g_strsplit(list, ",", -1);
    gboolean found = FALSE;
    for (int i = 0; parts[i] && !found; i++) {
        g_strstrip(parts[i]);
        found = g_ascii_strcasecmp(parts[i], item) == 0 ||
                (star_ok && strcmp(parts[i], "*") == 0);
    }
    g_strfreev(parts);
    return found;
}

static gboolean
ns_cors_preflight_cached(const char *key)
{
    g_mutex_lock(&g_cors_preflight_lock);
    gpointer v = g_cors_preflight_cache
        ? g_hash_table_lookup(g_cors_preflight_cache, key) : NULL;
    gboolean hit = v && *(gint64 *)v > g_get_monotonic_time();
    g_mutex_unlock(&g_cors_preflight_lock);
    return hit;
}

static void
ns_cors_preflight_remember(const char *key, const char *max_age)
{
    gint64 secs = max_age ? g_ascii_strtoll(max_age, NULL, 10) : 5;
    if (secs <= 0) return;
    if (secs > 7200) secs = 7200;
    gint64 *until = g_new(gint64, 1);
    *until = g_get_monotonic_time() + secs * G_USEC_PER_SEC;
    g_mutex_lock(&g_cors_preflight_lock);
    if (!g_cors_preflight_cache)
        g_cors_preflight_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, g_free);
    if (g_hash_table_size(g_cors_preflight_cache) >= NS_CORS_PREFLIGHT_CACHE_MAX)
        g_hash_table_remove_all(g_cors_preflight_cache);
    g_hash_table_insert(g_cors_preflight_cache, g_strdup(key), until);
    g_mutex_unlock(&g_cors_preflight_lock);
}

static gboolean
ns_fetch_wants_cors(GPtrArray *extra_headers)
{
    for (guint i = 0; extra_headers && i < extra_headers->len; i++) {
        const char *h = g_ptr_array_index(extra_headers, i);
        if (h && g_ascii_strncasecmp(h, "X-ND-CORS:", 10) == 0) return TRUE;
    }
    return FALSE;
}

static char *
ns_fetch_cors_preflight(const char *url, const char *top_url,
                        const char *method, const char *content_type,
                        GPtrArray *extra_headers, GCancellable *cancellable)
{
    if (!top_url || !*top_url || !ns_url_is_http_or_https(url) ||
        ns_url_same_origin(top_url, url))
        return NULL;
    g_autofree char *unsafe_headers =
        ns_cors_unsafe_header_names(extra_headers, content_type);
    gboolean simple_method = ns_cors_method_is_simple(method);
    if (simple_method && !unsafe_headers) return NULL;

    g_autofree char *origin = ns_url_origin_from(top_url);
    g_autofree char *key = g_strdup_printf("%s %s %s %s", origin ? origin : "",
                                           url, method,
                                           unsafe_headers ? unsafe_headers : "");
    if (ns_cors_preflight_cached(key)) return NULL;

    g_autofree char *method_h =
        g_strdup_printf("Access-Control-Request-Method: %s", method);
    g_autofree char *headers_h = unsafe_headers
        ? g_strdup_printf("Access-Control-Request-Headers: %s", unsafe_headers)
        : NULL;
    GPtrArray *pre = g_ptr_array_new();
    g_ptr_array_add(pre, method_h);
    if (headers_h) g_ptr_array_add(pre, headers_h);
    ns_response *r = ns_fetch_sync_hop(url, top_url, "OPTIONS", NULL, 0, NULL,
                                       pre, cancellable, NULL, FALSE, NULL);
    g_ptr_array_free(pre, TRUE);
    if (!r) return g_strdup("CORS preflight failed");

    char *failure = NULL;
    g_autofree char *acao =
        ns_net_raw_header_values(r->raw_headers, "Access-Control-Allow-Origin");
    g_autofree char *acam =
        ns_net_raw_header_values(r->raw_headers, "Access-Control-Allow-Methods");
    g_autofree char *acah =
        ns_net_raw_header_values(r->raw_headers, "Access-Control-Allow-Headers");
    g_autofree char *max_age =
        ns_net_raw_header_values(r->raw_headers, "Access-Control-Max-Age");
    if (acao) g_strstrip(acao);
    if (r->error || r->status < 200 || r->status > 299) {
        failure = g_strdup("CORS preflight failed");
    } else if (!acao || (strcmp(acao, "*") != 0 &&
                         g_strcmp0(acao, origin) != 0)) {
        failure = g_strdup("CORS preflight: origin not allowed");
    } else if (!simple_method && !ns_cors_list_allows(acam, method, TRUE)) {
        failure = g_strdup_printf("CORS preflight: method %s not allowed",
                                  method);
    } else if (unsafe_headers) {
        gchar **names = g_strsplit(unsafe_headers, ",", -1);
        for (int i = 0; names[i] && !failure; i++) {
            gboolean star_ok = strcmp(names[i], "authorization") != 0;
            if (!ns_cors_list_allows(acah, names[i], star_ok))
                failure = g_strdup_printf("CORS preflight: header %s not allowed",
                                          names[i]);
        }
        g_strfreev(names);
    }
    if (!failure) ns_cors_preflight_remember(key, max_age);
    ns_response_free(r);
    return failure;
}

static ns_response *
ns_fetch_sync(const char *url, const char *top_url, const char *method,
              const void *body, gsize body_len, const char *content_type,
              GPtrArray *extra_headers,
              GCancellable *cancellable, GError **error)
{
    if (!ns_fetch_is_navigation(top_url, extra_headers) &&
        ns_ext_should_block(url, top_url)) {
        ns_response *blocked = g_new0(ns_response, 1);
        blocked->body = g_byte_array_new();
        blocked->final_url = g_strdup(url);
        blocked->status = 0;
        blocked->error = g_strdup("blocked by extension");
        return blocked;
    }

    if (ns_fetch_wants_cors(extra_headers)) {
        char *failure = ns_fetch_cors_preflight(
            url, top_url, method && *method ? method : "GET", content_type,
            extra_headers, cancellable);
        if (failure) {
            ns_response *blocked = g_new0(ns_response, 1);
            blocked->body = g_byte_array_new();
            blocked->final_url = g_strdup(url);
            blocked->error = failure;
            return blocked;
        }
    }

    const ns_config *cfg = ns_config_get();
    long max_redirs = cfg ? (long)cfg->max_redirects : (long)NS_MAX_REDIRECTS;
    if (max_redirs < 0)                       max_redirs = 0;
    if (max_redirs > (long)NS_MAX_REDIRECTS)  max_redirs = (long)NS_MAX_REDIRECTS;

    char *cur_url = g_strdup(url);
    char *cur_top = g_strdup(top_url);
    char *cur_method = g_strdup(method && *method ? method : "GET");
    const void *cur_body = body;
    gsize cur_len = body_len;
    const char *cur_ct = content_type;
    gboolean started_https = g_str_has_prefix(url, "https://");
    int hops = 0;
    ns_response *resp = NULL;

    GPtrArray *hop_headers = NULL;
    if (extra_headers) {
        hop_headers = g_ptr_array_sized_new(extra_headers->len);
        for (guint i = 0; i < extra_headers->len; i++)
            g_ptr_array_add(hop_headers, g_ptr_array_index(extra_headers, i));
    }

    for (;;) {
        char *location = NULL;
        resp = ns_fetch_sync_hop(cur_url, cur_top, cur_method,
                                 cur_body, cur_len, cur_ct,
                                 hop_headers, cancellable, error,
                                 FALSE, &location);
        if (!resp) {
            g_free(location);
            break;
        }
        gboolean is_redirect = resp->status == 301 || resp->status == 302 ||
                               resp->status == 303 || resp->status == 307 ||
                               resp->status == 308;
        if (!is_redirect || resp->error || !location || !*location) {
            g_free(location);
            break;
        }
        if (hops >= max_redirs) {
            g_free(resp->error);
            resp->error = g_strdup("too many redirects");
            g_free(location);
            break;
        }
        const char *base = resp->final_url ? resp->final_url : cur_url;
        char *next = ns_url_resolve(base, location);
        g_free(location);
        if (!next) break;
        if (!ns_url_is_http_or_https(next) ||
            (started_https && !g_str_has_prefix(next, "https://") &&
             !ns_fetch_is_navigation(cur_top, extra_headers))) {
            g_free(resp->error);
            resp->error = g_strdup("redirect to a disallowed URL blocked");
            g_free(next);
            break;
        }
        if (ns_fetch_is_navigation(cur_top, extra_headers) &&
            ns_net_safebrowsing_blocks(next)) {
            g_free(resp->error);
            resp->error = g_strdup("redirect to a blocked site stopped");
            g_free(resp->final_url);
            resp->final_url = next;
            break;
        }
        if (hop_headers) {
            char *from_origin = ns_url_origin_from(base);
            char *to_origin = ns_url_origin_from(next);
            if (g_strcmp0(from_origin, to_origin) != 0)
                ns_headers_strip_sensitive(hop_headers);
            g_free(from_origin);
            g_free(to_origin);
        }
        gboolean was_post = g_ascii_strcasecmp(cur_method, "POST") == 0;
        gboolean get_or_head = g_ascii_strcasecmp(cur_method, "GET") == 0 ||
                               g_ascii_strcasecmp(cur_method, "HEAD") == 0;
        if (((resp->status == 301 || resp->status == 302) && was_post) ||
            (resp->status == 303 && !get_or_head)) {
            g_free(cur_method);
            cur_method = g_strdup("GET");
            cur_body = NULL;
            cur_len = 0;
            cur_ct = NULL;
            static const char *const body_headers[] = {
                "Content-Encoding:", "Content-Language:", "Content-Location:",
                "Content-Type:",
            };
            for (guint i = 0; hop_headers && i < hop_headers->len; ) {
                const char *h = g_ptr_array_index(hop_headers, i);
                gboolean drop = FALSE;
                for (gsize k = 0; h && k < G_N_ELEMENTS(body_headers); k++)
                    if (g_ascii_strncasecmp(h, body_headers[k],
                                            strlen(body_headers[k])) == 0)
                        drop = TRUE;
                if (drop) g_ptr_array_remove_index(hop_headers, i);
                else i++;
            }
        }
        if (ns_fetch_is_navigation(cur_top, extra_headers) &&
            g_ascii_strcasecmp(cur_method, "GET") == 0) {
            g_free(cur_top);
            cur_top = NULL;
        }
        char *preflight_failure = ns_fetch_wants_cors(extra_headers)
            ? ns_fetch_cors_preflight(next, cur_top, cur_method, cur_ct,
                                      hop_headers, cancellable)
            : NULL;
        if (preflight_failure) {
            ns_response_free(resp);
            resp = g_new0(ns_response, 1);
            resp->body = g_byte_array_new();
            resp->final_url = next;
            resp->error = preflight_failure;
            break;
        }
        g_free(cur_url);
        cur_url = next;
        hops++;
        ns_response_free(resp);
        resp = NULL;
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "fetch cancelled");
            break;
        }
    }
    if (resp) resp->redirect_count = hops;
    if (hop_headers) g_ptr_array_free(hop_headers, TRUE);
    g_free(cur_url);
    g_free(cur_top);
    g_free(cur_method);
    return resp;
}

typedef struct ns_fetch_ctx {
    char *url;
    char *top_url;
    char *method;
    char *content_type;
    guint8 *body;
    gsize body_len;
    GPtrArray *extra_headers;
    char *coalesce_key;
    struct ns_net_stream *stream;
} ns_fetch_ctx;

static void
ns_fetch_ctx_free(gpointer data)
{
    ns_fetch_ctx *ctx = data;
    g_free(ctx->url);
    g_free(ctx->top_url);
    g_free(ctx->method);
    g_free(ctx->content_type);
    g_free(ctx->body);
    if (ctx->extra_headers) g_ptr_array_free(ctx->extra_headers, TRUE);
    g_free(ctx->coalesce_key);
    if (ctx->stream) {
        g_main_context_unref(ctx->stream->main_ctx);
        g_free(ctx->stream);
    }
    g_free(ctx);
}

#define NS_PRELOAD_MAX_ENTRIES 512
#define NS_PRELOAD_MAX_BYTES   (16u * 1024u * 1024u)
#define NS_FETCH_JOIN_MAX_WAIT_S (NS_MAX_TIMEOUT_S + 5)

typedef struct {
    gboolean     done;
    ns_response *resp;
    GError      *err;
} ns_sync_waiter;

typedef struct {
    GPtrArray *tasks;
    GPtrArray *syncs;
} ns_coalesce_group;

static GMutex      g_fetch_mutex;
static GCond       g_fetch_cond;
static GHashTable *g_fetch_coalesce;
static GHashTable *g_preload_expected;
static GHashTable *g_preload_store;
static guint       g_preload_bytes;

static void
ns_preload_entry_free(gpointer p)
{
    ns_response *resp = p;
    if (!resp) return;
    guint len = resp->body ? resp->body->len : 0u;
    g_preload_bytes = (g_preload_bytes > len) ? g_preload_bytes - len : 0u;
    ns_response_free(resp);
}

static gboolean
ns_response_forbids_reuse(const ns_response *resp)
{
    if (!resp || !resp->raw_headers) return FALSE;
    g_autofree char *lowered = g_ascii_strdown(resp->raw_headers, -1);
    return strstr(lowered, "no-store") != NULL;
}

void
ns_net_preload_expect(const char *key)
{
    if (!key) return;
    g_mutex_lock(&g_fetch_mutex);
    if (!g_preload_expected)
        g_preload_expected = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, NULL);
    if (g_hash_table_size(g_preload_expected) < NS_PRELOAD_MAX_ENTRIES)
        g_hash_table_add(g_preload_expected, g_strdup(key));
    g_mutex_unlock(&g_fetch_mutex);
}

static gboolean
preload_key_not_kept(gpointer key, gpointer value, gpointer user_data)
{
    (void)value;
    GHashTable *keep = user_data;
    return !g_hash_table_contains(keep, key);
}

void
ns_net_preload_clear_except(const char *const *keep_keys)
{
    GHashTable *keep = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; keep_keys && keep_keys[i]; i++)
        g_hash_table_add(keep, (gpointer)keep_keys[i]);
    g_mutex_lock(&g_fetch_mutex);
    if (g_preload_expected)
        g_hash_table_foreach_remove(g_preload_expected, preload_key_not_kept,
                                    keep);
    if (g_preload_store)
        g_hash_table_foreach_remove(g_preload_store, preload_key_not_kept,
                                    keep);
    g_mutex_unlock(&g_fetch_mutex);
    g_hash_table_destroy(keep);
}

void
ns_net_preload_clear(void)
{
    g_mutex_lock(&g_fetch_mutex);
    if (g_preload_expected) g_hash_table_remove_all(g_preload_expected);
    if (g_preload_store) g_hash_table_remove_all(g_preload_store);
    g_preload_bytes = 0;
    g_mutex_unlock(&g_fetch_mutex);
}

static ns_response *
ns_preload_take_locked(const char *key)
{
    if (!g_preload_store) return NULL;
    char *stored_key = NULL;
    ns_response *resp = NULL;
    if (!g_hash_table_steal_extended(g_preload_store, key,
                                     (gpointer *)&stored_key,
                                     (gpointer *)&resp))
        return NULL;
    g_free(stored_key);
    guint len = (resp && resp->body) ? resp->body->len : 0u;
    g_preload_bytes = (g_preload_bytes > len) ? g_preload_bytes - len : 0u;
    return resp;
}

static void
ns_preload_store_locked(const char *key, const ns_response *resp)
{
    if (!g_preload_expected ||
        !g_hash_table_remove(g_preload_expected, key))
        return;
    if (!resp || resp->status != 200 || resp->error || !resp->body) return;
    if (resp->body->len > NS_PRELOAD_MAX_BYTES) return;
    if (g_preload_bytes + resp->body->len > NS_PRELOAD_MAX_BYTES) return;
    if (ns_response_forbids_reuse(resp)) return;
    if (!g_preload_store)
        g_preload_store = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, ns_preload_entry_free);
    if (g_hash_table_contains(g_preload_store, key)) return;
    g_preload_bytes += resp->body->len;
    g_hash_table_insert(g_preload_store, g_strdup(key), ns_response_copy(resp));
}

void
ns_net_preload_keep(const char *key, const ns_response *resp)
{
    if (!key || !resp) return;
    g_mutex_lock(&g_fetch_mutex);
    if (!g_preload_store || !g_hash_table_contains(g_preload_store, key)) {
        if (!g_preload_expected)
            g_preload_expected = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, NULL);
        g_hash_table_add(g_preload_expected, g_strdup(key));
        ns_preload_store_locked(key, resp);
    }
    g_mutex_unlock(&g_fetch_mutex);
}

typedef enum {
    NS_FETCH_LEAD,
    NS_FETCH_JOINED,
    NS_FETCH_PRELOADED,
} ns_fetch_claim;

static ns_fetch_claim
ns_fetch_claim_locked(const char *key, ns_response **preloaded,
                      ns_coalesce_group **group)
{
    ns_response *hit = ns_preload_take_locked(key);
    if (hit) {
        *preloaded = hit;
        return NS_FETCH_PRELOADED;
    }
    if (!g_fetch_coalesce)
        g_fetch_coalesce = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, NULL);
    ns_coalesce_group *grp = g_hash_table_lookup(g_fetch_coalesce, key);
    if (grp) {
        *group = grp;
        return NS_FETCH_JOINED;
    }
    g_hash_table_insert(g_fetch_coalesce, g_strdup(key),
                        g_new0(ns_coalesce_group, 1));
    return NS_FETCH_LEAD;
}

static ns_response *
ns_fetch_join_async(const char *key, GAsyncReadyCallback callback,
                    gpointer user_data, gboolean *joined)
{
    ns_response *preloaded = NULL;
    ns_coalesce_group *grp = NULL;
    *joined = FALSE;
    g_mutex_lock(&g_fetch_mutex);
    ns_fetch_claim claim = ns_fetch_claim_locked(key, &preloaded, &grp);
    if (claim == NS_FETCH_JOINED) {
        GTask *joiner = g_task_new(NULL, NULL, callback, user_data);
        g_task_set_source_tag(joiner, ns_net_fetch_async);
        if (!grp->tasks) grp->tasks = g_ptr_array_new();
        g_ptr_array_add(grp->tasks, joiner);
        *joined = TRUE;
    }
    g_mutex_unlock(&g_fetch_mutex);
    if (claim == NS_FETCH_PRELOADED) *joined = TRUE;
    return preloaded;
}

static ns_response *
ns_fetch_join_sync(const char *key, gboolean *joined, GError **error)
{
    ns_response *preloaded = NULL;
    ns_coalesce_group *grp = NULL;
    *joined = FALSE;
    g_mutex_lock(&g_fetch_mutex);
    ns_fetch_claim claim = ns_fetch_claim_locked(key, &preloaded, &grp);
    if (claim == NS_FETCH_JOINED) {
        ns_sync_waiter waiter = {0};
        if (!grp->syncs) grp->syncs = g_ptr_array_new();
        g_ptr_array_add(grp->syncs, &waiter);
        gint64 deadline = g_get_monotonic_time() +
            (gint64)NS_FETCH_JOIN_MAX_WAIT_S * G_TIME_SPAN_SECOND;
        while (!waiter.done) {
            if (g_atomic_int_get(&g_net_aborting) ||
                !g_cond_wait_until(&g_fetch_cond, &g_fetch_mutex, deadline))
                break;
        }
        if (!waiter.done) {
            g_ptr_array_remove_fast(grp->syncs, &waiter);
            g_mutex_unlock(&g_fetch_mutex);
            g_set_error_literal(error, NS_NET_DOMAIN, 0,
                                "shared fetch abandoned");
            *joined = TRUE;
            return NULL;
        }
        g_mutex_unlock(&g_fetch_mutex);
        if (error) *error = waiter.err;
        else g_clear_error(&waiter.err);
        *joined = TRUE;
        return waiter.resp;
    }
    g_mutex_unlock(&g_fetch_mutex);
    if (claim == NS_FETCH_PRELOADED) *joined = TRUE;
    return preloaded;
}

static void
ns_fetch_coalesce_deliver(const char *key, ns_response *resp, const GError *err)
{
    if (!key) return;
    g_mutex_lock(&g_fetch_mutex);
    if (resp) ns_preload_store_locked(key, resp);
    else if (g_preload_expected) g_hash_table_remove(g_preload_expected, key);
    ns_coalesce_group *grp = g_fetch_coalesce
        ? g_hash_table_lookup(g_fetch_coalesce, key) : NULL;
    if (g_fetch_coalesce) g_hash_table_remove(g_fetch_coalesce, key);
    if (grp && grp->syncs) {
        for (guint i = 0; i < grp->syncs->len; i++) {
            ns_sync_waiter *waiter = g_ptr_array_index(grp->syncs, i);
            waiter->resp = resp ? ns_response_copy(resp) : NULL;
            if (!resp)
                waiter->err = err
                    ? g_error_copy(err)
                    : g_error_new_literal(NS_NET_DOMAIN, 0, "fetch failed");
            waiter->done = TRUE;
        }
        g_cond_broadcast(&g_fetch_cond);
    }
    g_mutex_unlock(&g_fetch_mutex);
    if (!grp) return;
    if (grp->tasks) {
        for (guint i = 0; i < grp->tasks->len; i++) {
            GTask *waiter = g_ptr_array_index(grp->tasks, i);
            if (resp)
                g_task_return_pointer(waiter, ns_response_copy(resp),
                                      (GDestroyNotify)ns_response_free);
            else if (err)
                g_task_return_error(waiter, g_error_copy(err));
            else
                g_task_return_new_error(waiter, NS_NET_DOMAIN, 0, "fetch failed");
            g_object_unref(waiter);
        }
        g_ptr_array_free(grp->tasks, TRUE);
    }
    if (grp->syncs) g_ptr_array_free(grp->syncs, TRUE);
    g_free(grp);
}

static void
ns_fetch_task_abandon(GTask *task, const GError *err)
{
    ns_fetch_ctx *ctx = task ? g_task_get_task_data(task) : NULL;
    if (ctx && ctx->coalesce_key)
        ns_fetch_coalesce_deliver(ctx->coalesce_key, NULL, err);
}

#define NS_MAX_CONCURRENT_FETCHES 32
#define NS_MAX_FETCHES_PER_HOST   6

static GHashTable *g_fetch_host_active;

static void ns_fetch_thread(GTask *task, gpointer source_object,
                            gpointer task_data, GCancellable *cancellable);

static char *
ns_fetch_task_host(GTask *task)
{
    ns_fetch_ctx *ctx = task ? g_task_get_task_data(task) : NULL;
    return (ctx && ctx->url) ? ns_url_host_from(ctx->url) : NULL;
}

static int
ns_fetch_host_count_locked(const char *host)
{
    if (!host || !g_fetch_host_active) return 0;
    return GPOINTER_TO_INT(g_hash_table_lookup(g_fetch_host_active, host));
}

static void
ns_fetch_host_adjust_locked(const char *host, int delta)
{
    if (!host) return;
    if (!g_fetch_host_active)
        g_fetch_host_active = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                    g_free, NULL);
    int n = ns_fetch_host_count_locked(host) + delta;
    if (n <= 0)
        g_hash_table_remove(g_fetch_host_active, host);
    else
        g_hash_table_replace(g_fetch_host_active, g_strdup(host),
                             GINT_TO_POINTER(n));
}

static void
ns_fetch_throttle_dispatch(void)
{
    for (;;) {
        g_mutex_lock(&g_fetch_throttle_mutex);
        if (g_fetch_active >= NS_MAX_CONCURRENT_FETCHES ||
            g_queue_is_empty(&g_fetch_queue)) {
            g_mutex_unlock(&g_fetch_throttle_mutex);
            return;
        }
        GTask *chosen = NULL;
        char *chosen_host = NULL;
        for (GList *l = g_fetch_queue.head; l; l = l->next) {
            GTask *t = l->data;
            char *host = ns_fetch_task_host(t);
            if (!host ||
                ns_fetch_host_count_locked(host) < NS_MAX_FETCHES_PER_HOST) {
                chosen = t;
                chosen_host = host;
                g_queue_delete_link(&g_fetch_queue, l);
                break;
            }
            g_free(host);
        }
        if (!chosen) {
            g_mutex_unlock(&g_fetch_throttle_mutex);
            return;
        }
        g_fetch_active++;
        ns_fetch_host_adjust_locked(chosen_host, 1);
        g_free(chosen_host);
        g_mutex_unlock(&g_fetch_throttle_mutex);
        g_task_run_in_thread(chosen, ns_fetch_thread);
        g_object_unref(chosen);
    }
}

static void
ns_fetch_throttle_submit(GTask *task)
{
    g_mutex_lock(&g_fetch_throttle_mutex);
    g_queue_push_tail(&g_fetch_queue, task);
    g_mutex_unlock(&g_fetch_throttle_mutex);
    ns_fetch_throttle_dispatch();
}

static void
ns_fetch_thread(GTask        *task,
                gpointer      source_object,
                gpointer      task_data,
                GCancellable *cancellable)
{
    (void)source_object;
    ns_fetch_ctx *ctx = task_data;
    GError *err = NULL;
    g_hop_stream = ctx->stream;
    ns_response *resp = ns_fetch_sync(ctx->url, ctx->top_url, ctx->method,
                                      ctx->body, ctx->body_len, ctx->content_type,
                                      ctx->extra_headers,
                                      cancellable, &err);
    g_hop_stream = NULL;
    if (ctx->coalesce_key)
        ns_fetch_coalesce_deliver(ctx->coalesce_key, resp, err);
    if (!resp) {
        if (g_log_fetches)
            ns_debug_log_emit(NS_DLOG_NET, "fetch", "failed %s: %s",
                              ctx->url, err ? err->message : "unknown error");
        g_task_return_error(task, err);
    } else if (g_log_fetches) {
        if (resp->error)
            ns_debug_log_emit(NS_DLOG_NET, "fetch", "error %s: %s",
                              ctx->url, resp->error);
        else
            ns_debug_log_emit(NS_DLOG_NET, "fetch", "%ld %s (%u bytes)",
                              resp->status,
                              resp->final_url ? resp->final_url : ctx->url,
                              resp->body ? resp->body->len : 0u);
    }
    if (resp)
        g_task_return_pointer(task, resp, (GDestroyNotify)ns_response_free);
    {
        char *host = (ctx && ctx->url) ? ns_url_host_from(ctx->url) : NULL;
        g_mutex_lock(&g_fetch_throttle_mutex);
        if (g_fetch_active > 0) g_fetch_active--;
        ns_fetch_host_adjust_locked(host, -1);
        g_cond_broadcast(&g_fetch_idle_cond);
        g_mutex_unlock(&g_fetch_throttle_mutex);
        g_free(host);
    }
    ns_fetch_throttle_dispatch();
}

static void
ns_preconnect_thread(GTask *task, gpointer source_object, gpointer task_data,
                     GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;
    const char *url = task_data;
    g_mutex_lock(&g_fetch_throttle_mutex);
    g_preconnect_active++;
    g_mutex_unlock(&g_fetch_throttle_mutex);
    CURL *curl = g_atomic_int_get(&g_net_aborting) ? NULL : curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
        if (g_share) curl_easy_setopt(curl, CURLOPT_SHARE, g_share);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ns_xferinfo_cb);
        ns_net_apply_curl_proxy(curl, url);
        ns_net_apply_curl_tls(curl);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    g_mutex_lock(&g_fetch_throttle_mutex);
    if (g_preconnect_active > 0) g_preconnect_active--;
    g_cond_broadcast(&g_fetch_idle_cond);
    g_mutex_unlock(&g_fetch_throttle_mutex);
    g_task_return_boolean(task, TRUE);
}

void
ns_net_preconnect_async(const char *url)
{
    if (!url || !ns_url_is_http_or_https(url)) return;
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, g_strdup(url), g_free);
    g_task_run_in_thread(task, ns_preconnect_thread);
    g_object_unref(task);
}

static ns_net_blob_resolver g_blob_resolver = NULL;
static gpointer g_blob_resolver_ud = NULL;

void
ns_net_set_blob_resolver(ns_net_blob_resolver resolver, gpointer user_data)
{
    g_blob_resolver = resolver;
    g_blob_resolver_ud = user_data;
}

gboolean
ns_net_blob_url_visible_to(const char *blob_url, const char *doc_url)
{
    if (!doc_url || !*doc_url) return TRUE;
    if (!blob_url || !g_str_has_prefix(blob_url, "blob:")) return FALSE;
    g_autofree char *blob_origin = ns_url_origin_from(blob_url + 5);
    g_autofree char *doc_origin = g_str_has_prefix(doc_url, "blob:")
        ? ns_url_origin_from(doc_url + 5) : ns_url_origin_from(doc_url);
    if (!blob_origin || !*blob_origin) blob_origin = g_strdup("null");
    if (!doc_origin || !*doc_origin) doc_origin = g_strdup("null");
    return g_ascii_strcasecmp(blob_origin, doc_origin) == 0;
}

static gboolean
ns_net_complete_blob(const char *url, const char *top_url,
                     GCancellable *cancellable,
                     GAsyncReadyCallback callback, gpointer user_data)
{
    if (!g_blob_resolver || !g_str_has_prefix(url, "blob:")) return FALSE;
    char *type = NULL;
    GBytes *bytes = ns_net_blob_url_visible_to(url, top_url)
        ? g_blob_resolver(url, &type, g_blob_resolver_ud) : NULL;
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, ns_net_fetch_async);
    ns_response *resp = g_new0(ns_response, 1);
    resp->body = g_byte_array_new();
    resp->final_url = g_strdup(url);
    if (bytes) {
        gsize len = 0;
        const guint8 *data = g_bytes_get_data(bytes, &len);
        if (data && len) g_byte_array_append(resp->body, data, len);
        resp->status = 200;
        resp->content_type = type;
        g_bytes_unref(bytes);
    } else {
        resp->status = 404;
        resp->error = g_strdup("blob URL not found");
        g_free(type);
    }
    g_task_return_pointer(task, resp, (GDestroyNotify)ns_response_free);
    g_object_unref(task);
    return TRUE;
}

void
ns_net_fetch_async(const char        *url,
                   const char        *top_url,
                   GCancellable      *cancellable,
                   GAsyncReadyCallback callback,
                   gpointer            user_data)
{
    ns_net_request_async(url, top_url, "GET", NULL, 0, NULL, NULL,
                         cancellable, callback, user_data);
}

void
ns_net_request_async(const char         *url,
                     const char         *top_url,
                     const char         *method,
                     const void         *body,
                     gsize               body_len,
                     const char         *content_type,
                     const char *const  *extra_headers,
                     GCancellable       *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    ns_net_request_async_streaming(url, top_url, method, body, body_len,
                                   content_type, extra_headers, cancellable,
                                   NULL, NULL, callback, user_data);
}

static void
ns_net_request_async_streaming_full(const char         *url,
                                    const char         *top_url,
                                    const char         *method,
                                    const void         *body,
                                    gsize               body_len,
                                    const char         *content_type,
                                    const char *const  *extra_headers,
                                    GCancellable       *cancellable,
                                    ns_net_chunk_fn     on_chunk,
                                    gpointer            chunk_data,
                                    gboolean            discard_body,
                                    GAsyncReadyCallback callback,
                                    gpointer            user_data)
{
    g_return_if_fail(url != NULL);

    if (ns_net_complete_blob(url, top_url, cancellable, callback, user_data))
        return;

    char *key = (!cancellable && !body && !on_chunk)
        ? ns_net_request_key(url, top_url, method, extra_headers) : NULL;
    if (key) {
        gboolean joined = FALSE;
        ns_response *preloaded = ns_fetch_join_async(key, callback, user_data,
                                                     &joined);
        if (preloaded) {
            GTask *task = g_task_new(NULL, cancellable, callback, user_data);
            g_task_set_source_tag(task, ns_net_fetch_async);
            g_task_return_pointer(task, preloaded,
                                  (GDestroyNotify)ns_response_free);
            g_object_unref(task);
            g_free(key);
            return;
        }
        if (joined) {
            g_free(key);
            return;
        }
    }

    ns_fetch_ctx *ctx = g_new0(ns_fetch_ctx, 1);
    ctx->url = g_strdup(url);
    ctx->top_url = top_url ? g_strdup(top_url) : NULL;
    ctx->coalesce_key = key;
    if (method && *method) ctx->method = g_strdup(method);
    if (content_type && *content_type) ctx->content_type = g_strdup(content_type);
    if (body && body_len > 0) {
        ctx->body = g_memdup2(body, body_len);
        ctx->body_len = body_len;
    }
    if (extra_headers && extra_headers[0]) {
        ctx->extra_headers = g_ptr_array_new_with_free_func(g_free);
        for (int i = 0; extra_headers[i]; i++)
            g_ptr_array_add(ctx->extra_headers, g_strdup(extra_headers[i]));
    }

    if (on_chunk) {
        ctx->stream = g_new0(struct ns_net_stream, 1);
        ctx->stream->fn = on_chunk;
        ctx->stream->user_data = chunk_data;
        ctx->stream->main_ctx = g_main_context_ref_thread_default();
        ctx->stream->discard_body = discard_body;
    }

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, ns_net_request_async);
    g_task_set_task_data(task, ctx, ns_fetch_ctx_free);
    ns_fetch_throttle_submit(task);
}

void
ns_net_request_async_streaming(const char         *url,
                               const char         *top_url,
                               const char         *method,
                               const void         *body,
                               gsize               body_len,
                               const char         *content_type,
                               const char *const  *extra_headers,
                               GCancellable       *cancellable,
                               ns_net_chunk_fn     on_chunk,
                               gpointer            chunk_data,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
    ns_net_request_async_streaming_full(url, top_url, method, body, body_len,
                                        content_type, extra_headers,
                                        cancellable, on_chunk, chunk_data,
                                        FALSE, callback, user_data);
}

void
ns_net_download_async(const char         *url,
                      const char         *top_url,
                      GCancellable       *cancellable,
                      ns_net_chunk_fn     on_chunk,
                      gpointer            chunk_data,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
    ns_net_request_async_streaming_full(url, top_url, "GET", NULL, 0, NULL,
                                        NULL, cancellable, on_chunk,
                                        chunk_data, TRUE, callback,
                                        user_data);
}

ns_response *
ns_net_request_blocking(const char        *url,
                        const char        *top_url,
                        const char        *method,
                        const void        *body,
                        gsize              body_len,
                        const char        *content_type,
                        const char *const *extra_headers,
                        GCancellable      *cancellable,
                        GError           **error)
{
    char *key = (!cancellable && !body)
        ? ns_net_request_key(url, top_url, method, extra_headers) : NULL;
    if (key) {
        gboolean joined = FALSE;
        ns_response *shared = ns_fetch_join_sync(key, &joined, error);
        if (joined) {
            g_free(key);
            return shared;
        }
    }
    GPtrArray *hdrs = NULL;
    if (extra_headers) {
        hdrs = g_ptr_array_new_with_free_func(g_free);
        for (int i = 0; extra_headers[i]; i++)
            g_ptr_array_add(hdrs, g_strdup(extra_headers[i]));
    }
    GError *failure = NULL;
    ns_response *resp = ns_fetch_sync(url, top_url, method,
                                      body, body_len, content_type,
                                      hdrs, cancellable, &failure);
    if (key) {
        ns_fetch_coalesce_deliver(key, resp, failure);
        g_free(key);
    }
    if (hdrs) g_ptr_array_free(hdrs, TRUE);
    if (failure) g_propagate_error(error, failure);
    return resp;
}

ns_response *
ns_net_fetch_blocking(const char *url, GCancellable *cancellable, GError **error)
{
    return ns_net_request_blocking(url, NULL, "GET", NULL, 0, NULL, NULL,
                                   cancellable, error);
}

ns_response *
ns_net_fetch_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

char *
ns_multipart_boundary(void)
{
    guint32 r[4];
    if (!ns_security_csprng_fill(r, sizeof r)) {
        r[0] = g_random_int(); r[1] = g_random_int();
        r[2] = g_random_int(); r[3] = g_random_int();
    }
    return g_strdup_printf("----NordstjernenFormBoundary%08x%08x%08x%08x",
                           r[0], r[1], r[2], r[3]);
}

void
ns_multipart_quote_field(GString *out, const char *s)
{
    if (!out || !s) return;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  g_string_append(out, "%22");
        else if (c == '\r') g_string_append(out, "%0D");
        else if (c == '\n') g_string_append(out, "%0A");
        else                g_string_append_c(out, (char)c);
    }
}

static char *g_form_submission_charset;

void
ns_form_set_submission_charset(const char *charset)
{
    g_free(g_form_submission_charset);
    g_form_submission_charset = NULL;
    if (!charset || !*charset) return;
    char *first = g_strdup(charset);
    g_strstrip(first);
    for (char *p = first; *p; p++)
        if (*p == ' ' || *p == ',' || *p == '\t') { *p = '\0'; break; }
    if (*first && g_ascii_strcasecmp(first, "UTF-8") != 0 &&
        g_ascii_strcasecmp(first, "UTF8") != 0 &&
        g_ascii_strcasecmp(first, "UTF-16LE") != 0 &&
        g_ascii_strcasecmp(first, "UTF-16BE") != 0)
        g_form_submission_charset = first;
    else
        g_free(first);
}

void
ns_form_urlencoded_append(GString *out, const char *s)
{
    if (!out || !s) return;
    char *converted = NULL;
    if (g_form_submission_charset) {
        converted = g_convert(s, -1, g_form_submission_charset, "UTF-8",
                              NULL, NULL, NULL);
        if (converted) s = converted;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (g_ascii_isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_')
            g_string_append_c(out, (char)c);
        else if (c == ' ')
            g_string_append_c(out, '+');
        else
            g_string_append_printf(out, "%%%02X", c);
    }
    g_free(converted);
}

void
ns_form_urlencoded_append_pair(GString *out, gboolean *first,
                               const char *name, const char *value)
{
    if (!out || !first || !name) return;
    if (!*first) g_string_append_c(out, '&');
    *first = FALSE;
    ns_form_urlencoded_append(out, name);
    g_string_append_c(out, '=');
    ns_form_urlencoded_append(out, value);
}
