/* Nordstjernen — compression dictionary transport (RFC 9842): the store of dictionaries a response offered and their matching. */

#include "compression_dictionary.h"

#include <string.h>

#include "net.h"

#define NS_DICTIONARY_MAX_BYTES (16u * 1024u * 1024u)
#define NS_DICTIONARY_MAX_TOTAL (64u * 1024u * 1024u)
#define NS_DICTIONARY_MAX_COUNT 128u
#define NS_DICTIONARY_MAX_ID 1024u

struct ns_dictionary {
    gint     refcount;
    char    *origin;
    char    *url;
    char    *pattern;
    char    *id;
    char   **destinations;
    char    *available;
    guint8   hash[NS_DICTIONARY_HASH_LEN];
    GBytes  *bytes;
    gint64   expires_us;
    guint64  sequence;
};

static GMutex     g_dictionary_lock;
static GPtrArray *g_dictionaries;
static guint64    g_dictionary_sequence;

static void
dictionary_free(ns_dictionary *d)
{
    g_free(d->origin);
    g_free(d->url);
    g_free(d->pattern);
    g_free(d->id);
    g_strfreev(d->destinations);
    g_free(d->available);
    if (d->bytes) g_bytes_unref(d->bytes);
    g_free(d);
}

void
ns_dictionary_unref(ns_dictionary *dictionary)
{
    if (dictionary && g_atomic_int_dec_and_test(&dictionary->refcount))
        dictionary_free(dictionary);
}

static void
dictionary_unref_pointer(gpointer p)
{
    ns_dictionary_unref(p);
}

const char *
ns_dictionary_available_value(const ns_dictionary *dictionary)
{
    return dictionary->available;
}

const char *
ns_dictionary_id(const ns_dictionary *dictionary)
{
    return dictionary->id;
}

const guint8 *
ns_dictionary_hash(const ns_dictionary *dictionary)
{
    return dictionary->hash;
}

const guint8 *
ns_dictionary_data(const ns_dictionary *dictionary, gsize *len)
{
    return g_bytes_get_data(dictionary->bytes, len);
}

static const char *
dictionary_skip_spaces(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static const char *
dictionary_parse_string(const char *p, char **out)
{
    GString *s = g_string_new(NULL);
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;
        g_string_append_c(s, *p);
    }
    *out = g_string_free(s, FALSE);
    return *p == '"' ? p + 1 : p;
}

static const char *
dictionary_parse_string_list(const char *p, char ***out)
{
    GPtrArray *items = g_ptr_array_new();
    for (p++; *p && *p != ')';) {
        p = dictionary_skip_spaces(p);
        if (*p == '"') {
            char *item = NULL;
            p = dictionary_parse_string(p, &item);
            g_ptr_array_add(items, item);
        } else if (*p && *p != ')') {
            p++;
        }
    }
    g_ptr_array_add(items, NULL);
    *out = (char **)g_ptr_array_free(items, FALSE);
    return *p == ')' ? p + 1 : p;
}

static gboolean
dictionary_parse_header(const char *header, char **match, char **id,
                        char ***destinations, gboolean *raw_type)
{
    *raw_type = TRUE;
    for (const char *p = header; *p;) {
        p = dictionary_skip_spaces(p);
        while (*p == ',') p = dictionary_skip_spaces(p + 1);
        const char *key = p;
        while (*p && *p != '=' && *p != ',') p++;
        gsize key_len = (gsize)(p - key);
        if (*p != '=') continue;
        p = dictionary_skip_spaces(p + 1);
        if (*p == '"') {
            char *value = NULL;
            p = dictionary_parse_string(p, &value);
            if (key_len == 5 && !g_ascii_strncasecmp(key, "match", 5) && !*match)
                *match = g_steal_pointer(&value);
            else if (key_len == 2 && !g_ascii_strncasecmp(key, "id", 2) && !*id)
                *id = g_steal_pointer(&value);
            g_free(value);
        } else if (*p == '(') {
            char **list = NULL;
            p = dictionary_parse_string_list(p, &list);
            if (key_len == 10 && !g_ascii_strncasecmp(key, "match-dest", 10) &&
                !*destinations)
                *destinations = g_steal_pointer(&list);
            g_strfreev(list);
        } else {
            const char *token = p;
            while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != ';') p++;
            if (key_len == 4 && !g_ascii_strncasecmp(key, "type", 4))
                *raw_type = (gsize)(p - token) == 3 &&
                            !g_ascii_strncasecmp(token, "raw", 3);
        }
        while (*p && *p != ',') p++;
    }
    return *match && **match;
}

static char *
dictionary_path_of(const char *url)
{
    const char *scheme_end = strstr(url, "://");
    const char *path = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
    if (!path) return g_strdup("/");
    gsize len = strcspn(path, "#");
    return g_strndup(path, len);
}

static char *
dictionary_resolve_pattern(const char *match, const char *url,
                           const char *origin)
{
    const char *path_part = match;
    if (strstr(match, "://")) {
        gsize origin_len = strlen(origin);
        if (g_ascii_strncasecmp(match, origin, origin_len) != 0) return NULL;
        path_part = match + origin_len;
        if (*path_part && *path_part != '/') return NULL;
    }
    if (strpbrk(path_part, "({:")) return NULL;
    if (path_part != match) return g_strdup(*path_part ? path_part : "/");
    if (match[0] == '/') return g_strdup(match);
    g_autofree char *base = dictionary_path_of(url);
    char *query = strchr(base, '?');
    if (query) *query = '\0';
    char *slash = strrchr(base, '/');
    if (slash) slash[1] = '\0';
    return g_strconcat(base, match, NULL);
}

static gboolean
dictionary_glob(const char *pattern, const char *text)
{
    const char *star = NULL, *resume = NULL;
    while (*text) {
        if (*pattern == '*') {
            star = pattern++;
            resume = text;
        } else if (*pattern == *text) {
            pattern++;
            text++;
        } else if (star) {
            pattern = star + 1;
            text = ++resume;
        } else {
            return FALSE;
        }
    }
    while (*pattern == '*') pattern++;
    return *pattern == '\0';
}

static gboolean
dictionary_pattern_matches(const char *pattern, const char *url)
{
    g_autofree char *path = dictionary_path_of(url);
    if (!strchr(pattern, '?')) {
        char *query = strchr(path, '?');
        if (query) *query = '\0';
    }
    return dictionary_glob(pattern, path);
}

static gint64
dictionary_lifetime_us(const char *cache_control)
{
    if (!cache_control) return 0;
    g_autofree char *lower = g_ascii_strdown(cache_control, -1);
    if (strstr(lower, "no-store")) return 0;
    const char *max_age = strstr(lower, "max-age=");
    if (!max_age) return 0;
    gint64 seconds = g_ascii_strtoll(max_age + 8, NULL, 10);
    if (seconds <= 0) return 0;
    return MIN(seconds, (gint64)86400 * 365) * G_USEC_PER_SEC;
}

static gsize
dictionary_total_locked(void)
{
    gsize total = 0;
    for (guint i = 0; i < g_dictionaries->len; i++) {
        const ns_dictionary *d = g_ptr_array_index(g_dictionaries, i);
        total += g_bytes_get_size(d->bytes);
    }
    return total;
}

static void
dictionary_evict_locked(gsize incoming)
{
    gint64 now = g_get_monotonic_time();
    for (guint i = g_dictionaries->len; i > 0; i--) {
        const ns_dictionary *d = g_ptr_array_index(g_dictionaries, i - 1);
        if (d->expires_us <= now) g_ptr_array_remove_index(g_dictionaries, i - 1);
    }
    while (g_dictionaries->len > 0 &&
           (g_dictionaries->len >= NS_DICTIONARY_MAX_COUNT ||
            dictionary_total_locked() + incoming > NS_DICTIONARY_MAX_TOTAL))
        g_ptr_array_remove_index(g_dictionaries, 0);
}

void
ns_dictionary_store(const char *url, const char *use_as_dictionary,
                    const char *cache_control, const guint8 *body, gsize len)
{
    if (!url || !use_as_dictionary || !body || len == 0 ||
        len > NS_DICTIONARY_MAX_BYTES)
        return;
    gint64 lifetime = dictionary_lifetime_us(cache_control);
    if (lifetime <= 0) return;
    g_autofree char *origin = ns_url_origin_from(url);
    if (!origin || !*origin || strcmp(origin, "null") == 0) return;

    g_autofree char *match = NULL;
    g_autofree char *id = NULL;
    g_auto(GStrv) destinations = NULL;
    gboolean raw_type = TRUE;
    if (!dictionary_parse_header(use_as_dictionary, &match, &id, &destinations,
                                 &raw_type) || !raw_type)
        return;
    if (id && strlen(id) > NS_DICTIONARY_MAX_ID) return;
    g_autofree char *pattern = dictionary_resolve_pattern(match, url, origin);
    if (!pattern) return;

    ns_dictionary *d = g_new0(ns_dictionary, 1);
    d->refcount = 1;
    d->origin = g_steal_pointer(&origin);
    d->url = g_strdup(url);
    d->pattern = g_steal_pointer(&pattern);
    d->id = g_steal_pointer(&id);
    d->destinations = g_steal_pointer(&destinations);
    d->bytes = g_bytes_new(body, len);
    d->expires_us = g_get_monotonic_time() + lifetime;
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, body, (gssize)len);
    gsize hash_len = NS_DICTIONARY_HASH_LEN;
    g_checksum_get_digest(sum, d->hash, &hash_len);
    g_checksum_free(sum);
    g_autofree char *encoded = g_base64_encode(d->hash, NS_DICTIONARY_HASH_LEN);
    d->available = g_strdup_printf(":%s:", encoded);

    g_mutex_lock(&g_dictionary_lock);
    if (!g_dictionaries)
        g_dictionaries = g_ptr_array_new_with_free_func(dictionary_unref_pointer);
    for (guint i = g_dictionaries->len; i > 0; i--) {
        const ns_dictionary *old = g_ptr_array_index(g_dictionaries, i - 1);
        if (strcmp(old->origin, d->origin) == 0 &&
            strcmp(old->pattern, d->pattern) == 0 &&
            g_strv_equal((const char *const *)(old->destinations
                             ? old->destinations : (char *[]){ NULL }),
                         (const char *const *)(d->destinations
                             ? d->destinations : (char *[]){ NULL })))
            g_ptr_array_remove_index(g_dictionaries, i - 1);
    }
    dictionary_evict_locked(len);
    d->sequence = ++g_dictionary_sequence;
    g_ptr_array_add(g_dictionaries, d);
    g_mutex_unlock(&g_dictionary_lock);
}

static gboolean
dictionary_destination_matches(const ns_dictionary *d, const char *destination)
{
    if (!d->destinations || !d->destinations[0]) return TRUE;
    const char *wanted = destination ? destination : "";
    if (strcmp(wanted, "empty") == 0) wanted = "";
    for (char **dest = d->destinations; *dest; dest++)
        if (g_ascii_strcasecmp(*dest, wanted) == 0) return TRUE;
    return FALSE;
}

ns_dictionary *
ns_dictionary_match(const char *url, const char *destination)
{
    if (!url) return NULL;
    g_autofree char *origin = ns_url_origin_from(url);
    if (!origin || !*origin) return NULL;
    ns_dictionary *best = NULL;
    gint64 now = g_get_monotonic_time();
    g_mutex_lock(&g_dictionary_lock);
    for (guint i = 0; g_dictionaries && i < g_dictionaries->len; i++) {
        ns_dictionary *d = g_ptr_array_index(g_dictionaries, i);
        if (d->expires_us <= now || strcmp(d->origin, origin) != 0 ||
            !dictionary_destination_matches(d, destination) ||
            !dictionary_pattern_matches(d->pattern, url))
            continue;
        gboolean has_dest = d->destinations && d->destinations[0];
        gboolean best_has_dest = best && best->destinations &&
                                 best->destinations[0];
        if (!best || (has_dest && !best_has_dest) ||
            (has_dest == best_has_dest &&
             (strlen(d->pattern) > strlen(best->pattern) ||
              (strlen(d->pattern) == strlen(best->pattern) &&
               d->sequence > best->sequence))))
            best = d;
    }
    if (best) g_atomic_int_inc(&best->refcount);
    g_mutex_unlock(&g_dictionary_lock);
    return best;
}

void
ns_dictionary_clear_all(void)
{
    g_mutex_lock(&g_dictionary_lock);
    if (g_dictionaries) g_ptr_array_set_size(g_dictionaries, 0);
    g_mutex_unlock(&g_dictionary_lock);
}
