/* Nordstjernen — Connection-Allowlist response header: parsing and URL pattern matching. */

#include "connection_allowlist.h"

#include <string.h>

typedef struct {
    GRegex  *scheme;
    GRegex  *host;
    GRegex  *port;
    GRegex  *path;
} ns_conn_pattern;

typedef struct {
    GPtrArray *patterns;
} ns_conn_policy;

struct ns_conn_allowlist {
    GPtrArray *policies;
};

static void
conn_pattern_free(gpointer p)
{
    ns_conn_pattern *pattern = p;
    if (!pattern) return;
    g_clear_pointer(&pattern->scheme, g_regex_unref);
    g_clear_pointer(&pattern->host, g_regex_unref);
    g_clear_pointer(&pattern->port, g_regex_unref);
    g_clear_pointer(&pattern->path, g_regex_unref);
    g_free(pattern);
}

static void
conn_policy_free(gpointer p)
{
    ns_conn_policy *policy = p;
    if (!policy) return;
    g_ptr_array_unref(policy->patterns);
    g_free(policy);
}

static const char *
conn_group_end(const char *p)
{
    int depth = 0;
    for (; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
        } else if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            if (--depth == 0) return p + 1;
        }
    }
    return p;
}

static GRegex *
conn_component_regex(const char *text, gsize len, char segment_separator)
{
    GString *re = g_string_new("^");
    const char *end = text + len;
    for (const char *p = text; p < end;) {
        if (*p == '(') {
            const char *close = conn_group_end(p);
            if (close > end) close = end;
            g_string_append_len(re, p, close - p);
            p = close;
        } else if (*p == '*') {
            g_string_append(re, ".*");
            p++;
        } else if (*p == ':' && p + 1 < end &&
                   (g_ascii_isalpha(p[1]) || p[1] == '_')) {
            p++;
            while (p < end && (g_ascii_isalnum(*p) || *p == '_')) p++;
            g_string_append_printf(re, "[^%c]+", segment_separator);
        } else if (*p == '{' || *p == '}') {
            p++;
        } else {
            if (*p == '\\' && p + 1 < end) p++;
            const char *next = g_utf8_next_char(p);
            if (next > end) next = end;
            g_autofree char *literal = g_strndup(p, (gsize)(next - p));
            g_autofree char *escaped = g_regex_escape_string(literal, -1);
            g_string_append(re, escaped);
            p = next;
        }
    }
    g_string_append_c(re, '$');
    GRegex *compiled = g_regex_new(re->str, G_REGEX_CASELESS | G_REGEX_OPTIMIZE,
                                   0, NULL);
    g_string_free(re, TRUE);
    return compiled;
}

static const char *
conn_scan_outside_groups(const char *p, const char *stops)
{
    while (*p) {
        if (*p == '(') {
            p = conn_group_end(p);
            continue;
        }
        if (*p == '\\' && p[1]) {
            p += 2;
            continue;
        }
        if (strchr(stops, *p)) return p;
        p++;
    }
    return p;
}

static const char *
conn_port_separator(const char *authority, const char *end)
{
    const char *found = NULL;
    for (const char *p = authority; p < end;) {
        if (*p == '(') {
            const char *close = conn_group_end(p);
            p = close > end ? end : close;
            continue;
        }
        if (*p == '[') {
            while (p < end && *p != ']') p++;
            continue;
        }
        if (*p == ':' && p + 1 < end && (g_ascii_isdigit(p[1]) || p[1] == '*'))
            found = p;
        p++;
    }
    return found;
}

static ns_conn_pattern *
conn_pattern_parse(const char *text)
{
    const char *scheme_end = strstr(text, "://");
    if (!scheme_end || scheme_end == text) return NULL;
    const char *authority = scheme_end + 3;
    const char *authority_end = conn_scan_outside_groups(authority, "/?#");
    if (authority_end == authority) return NULL;
    const char *port_at = conn_port_separator(authority, authority_end);
    const char *host_end = port_at ? port_at : authority_end;
    const char *path_end = conn_scan_outside_groups(authority_end, "?#");
    gsize path_len = (gsize)(path_end - authority_end);

    ns_conn_pattern *pattern = g_new0(ns_conn_pattern, 1);
    pattern->scheme = conn_component_regex(text, (gsize)(scheme_end - text), '/');
    pattern->host = conn_component_regex(authority,
                                         (gsize)(host_end - authority), '.');
    pattern->port = port_at
        ? conn_component_regex(port_at + 1,
                               (gsize)(authority_end - port_at - 1), '/')
        : conn_component_regex("", 0, '/');
    pattern->path = path_len > 1
        ? conn_component_regex(authority_end, path_len, '/') : NULL;
    if (!pattern->scheme || !pattern->host || !pattern->port ||
        (path_len > 1 && !pattern->path)) {
        conn_pattern_free(pattern);
        return NULL;
    }
    return pattern;
}

static void
conn_policy_add(ns_conn_policy *policy, const char *text)
{
    ns_conn_pattern *pattern = conn_pattern_parse(text);
    if (pattern) g_ptr_array_add(policy->patterns, pattern);
}

static const char *
conn_parse_string(const char *p, GString *out)
{
    g_string_truncate(out, 0);
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;
        g_string_append_c(out, *p);
    }
    return *p == '"' ? p + 1 : p;
}

static const char *
conn_parse_policy(const char *p, const char *response_origin,
                  ns_conn_policy *policy)
{
    g_autoptr(GString) item = g_string_new(NULL);
    for (p++; *p && *p != ')';) {
        if (*p == ' ' || *p == '\t') {
            p++;
        } else if (*p == '"') {
            p = conn_parse_string(p, item);
            conn_policy_add(policy, item->str);
        } else {
            const char *start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != ')') p++;
            if ((gsize)(p - start) == strlen("response-origin") &&
                g_ascii_strncasecmp(start, "response-origin", 15) == 0 &&
                response_origin && *response_origin)
                conn_policy_add(policy, response_origin);
        }
    }
    if (*p == ')') p++;
    while (*p && *p != ',') {
        if (*p == '"') {
            p = conn_parse_string(p, item);
            continue;
        }
        p++;
    }
    return p;
}

ns_conn_allowlist *
ns_conn_allowlist_parse(const char *header, const char *response_origin)
{
    if (!header) return NULL;
    ns_conn_allowlist *list = g_new0(ns_conn_allowlist, 1);
    list->policies = g_ptr_array_new_with_free_func(conn_policy_free);
    for (const char *p = header; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        ns_conn_policy *policy = g_new0(ns_conn_policy, 1);
        policy->patterns = g_ptr_array_new_with_free_func(conn_pattern_free);
        g_ptr_array_add(list->policies, policy);
        if (*p == '(') {
            p = conn_parse_policy(p, response_origin, policy);
        } else {
            while (*p && *p != ',') p++;
        }
    }
    if (list->policies->len == 0) {
        ns_conn_allowlist_free(list);
        return NULL;
    }
    return list;
}

void
ns_conn_allowlist_free(ns_conn_allowlist *list)
{
    if (!list) return;
    g_ptr_array_unref(list->policies);
    g_free(list);
}

static int
conn_default_port(const char *scheme)
{
    if (!g_ascii_strcasecmp(scheme, "http") || !g_ascii_strcasecmp(scheme, "ws"))
        return 80;
    if (!g_ascii_strcasecmp(scheme, "https") ||
        !g_ascii_strcasecmp(scheme, "wss"))
        return 443;
    return -1;
}

static gboolean
conn_policy_allows(const ns_conn_policy *policy, const char *scheme,
                   const char *host, const char *port, const char *path)
{
    for (guint i = 0; i < policy->patterns->len; i++) {
        const ns_conn_pattern *pattern = g_ptr_array_index(policy->patterns, i);
        if (g_regex_match(pattern->scheme, scheme, 0, NULL) &&
            g_regex_match(pattern->host, host, 0, NULL) &&
            g_regex_match(pattern->port, port, 0, NULL) &&
            (!pattern->path || g_regex_match(pattern->path, path, 0, NULL)))
            return TRUE;
    }
    return FALSE;
}

gboolean
ns_conn_allowlist_allows(const ns_conn_allowlist *list, const char *url)
{
    if (!list || !url) return TRUE;
    g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_PARSE_RELAXED |
                                           G_URI_FLAGS_ENCODED, NULL);
    if (!uri) return FALSE;
    const char *scheme = g_uri_get_scheme(uri);
    const char *host = g_uri_get_host(uri);
    if (!scheme || !host) return FALSE;
    int port_number = g_uri_get_port(uri);
    g_autofree char *port = port_number < 0 ||
                            port_number == conn_default_port(scheme)
        ? g_strdup("") : g_strdup_printf("%d", port_number);
    const char *raw_path = g_uri_get_path(uri);
    const char *path = raw_path && *raw_path ? raw_path : "/";
    for (guint i = 0; i < list->policies->len; i++)
        if (!conn_policy_allows(g_ptr_array_index(list->policies, i), scheme,
                                host, port, path))
            return FALSE;
    return TRUE;
}
