/* Nordstjernen — Permissions Policy: the feature list and allowlist matching for the header and the iframe allow attribute. */

#include "permissions_policy.h"

#include <string.h>

typedef struct {
    const char *name;
    gboolean    default_all;
} ns_pp_feature;

static const ns_pp_feature g_features[] = {
    { "accelerometer",                    FALSE },
    { "autoplay",                         FALSE },
    { "camera",                           FALSE },
    { "clipboard-read",                   FALSE },
    { "clipboard-write",                  FALSE },
    { "cross-origin-isolated",            FALSE },
    { "display-capture",                  FALSE },
    { "encrypted-media",                  FALSE },
    { "focus-without-user-activation",    TRUE  },
    { "fullscreen",                       FALSE },
    { "gamepad",                          TRUE  },
    { "geolocation",                      FALSE },
    { "gyroscope",                        FALSE },
    { "magnetometer",                     FALSE },
    { "media-playback-while-not-visible", TRUE  },
    { "microphone",                       FALSE },
    { "midi",                             FALSE },
    { "payment",                          FALSE },
    { "picture-in-picture",               TRUE  },
    { "publickey-credentials-create",     FALSE },
    { "publickey-credentials-get",        FALSE },
    { "screen-wake-lock",                 FALSE },
    { "storage-access",                   TRUE  },
    { "sync-xhr",                         TRUE  },
    { "web-share",                        FALSE },
};

typedef struct {
    gboolean   all;
    gboolean   self;
    gboolean   src;
    GPtrArray *origins;
} ns_pp_allowlist;

gsize
ns_pp_feature_count(void)
{
    return G_N_ELEMENTS(g_features);
}

const char *
ns_pp_feature_name(gsize index)
{
    return index < G_N_ELEMENTS(g_features) ? g_features[index].name : NULL;
}

static const ns_pp_feature *
pp_feature_find(const char *feature)
{
    if (!feature) return NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(g_features); i++)
        if (strcmp(g_features[i].name, feature) == 0)
            return &g_features[i];
    return NULL;
}

gboolean
ns_pp_feature_known(const char *feature)
{
    return pp_feature_find(feature) != NULL;
}

gboolean
ns_pp_feature_default_all(const char *feature)
{
    const ns_pp_feature *f = pp_feature_find(feature);
    return f && f->default_all;
}

static void
pp_allowlist_clear(ns_pp_allowlist *list)
{
    if (list->origins) g_ptr_array_unref(list->origins);
    memset(list, 0, sizeof *list);
}

static void
pp_allowlist_reset(ns_pp_allowlist *list)
{
    pp_allowlist_clear(list);
    list->origins = g_ptr_array_new_with_free_func(g_free);
}

static char *
pp_origin_normalise(const char *text, gsize len)
{
    char *origin = g_ascii_strdown(text, (gssize)len);
    gsize n = strlen(origin);
    while (n > 0 && origin[n - 1] == '/')
        origin[--n] = '\0';
    return origin;
}

static void
pp_allowlist_add_token(ns_pp_allowlist *list, const char *text, gsize len,
                       gboolean quoted_keywords)
{
    if (len == 0) return;
    if (len == 1 && text[0] == '*') {
        list->all = TRUE;
        return;
    }
    const char *self = quoted_keywords ? "'self'" : "self";
    const char *src = quoted_keywords ? "'src'" : "src";
    if (strlen(self) == len && g_ascii_strncasecmp(text, self, len) == 0) {
        list->self = TRUE;
        return;
    }
    if (strlen(src) == len && g_ascii_strncasecmp(text, src, len) == 0) {
        list->src = TRUE;
        return;
    }
    if (quoted_keywords && len == 6 &&
        g_ascii_strncasecmp(text, "'none'", 6) == 0)
        return;
    g_ptr_array_add(list->origins, pp_origin_normalise(text, len));
}

static gboolean
pp_origins_equal(const char *a, const char *b)
{
    return a && b && *a && *b && g_ascii_strcasecmp(a, b) == 0;
}

static gboolean
pp_allowlist_matches(const ns_pp_allowlist *list, const char *self_origin,
                     const char *src_origin, const char *origin)
{
    if (list->all) return TRUE;
    if (list->self && pp_origins_equal(self_origin, origin)) return TRUE;
    if (list->src && pp_origins_equal(src_origin, origin)) return TRUE;
    for (guint i = 0; list->origins && i < list->origins->len; i++)
        if (pp_origins_equal(g_ptr_array_index(list->origins, i), origin))
            return TRUE;
    return FALSE;
}

static const char *
pp_skip_spaces(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static const char *
pp_skip_parameters(const char *p)
{
    while (*p && *p != ',') {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (*p) p++;
        } else {
            p++;
        }
    }
    return p;
}

static const char *
pp_parse_header_item(const char *p, ns_pp_allowlist *list)
{
    if (*p == '"') {
        const char *start = ++p;
        while (*p && *p != '"') p++;
        pp_allowlist_add_token(list, start, (gsize)(p - start), FALSE);
        return *p ? p + 1 : p;
    }
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != ')' && *p != ',' &&
           *p != ';')
        p++;
    pp_allowlist_add_token(list, start, (gsize)(p - start), FALSE);
    return p;
}

static const char *
pp_parse_header_value(const char *p, ns_pp_allowlist *list)
{
    pp_allowlist_reset(list);
    if (*p != '(') {
        p = pp_parse_header_item(p, list);
        return pp_skip_parameters(p);
    }
    p++;
    while (*p && *p != ')') {
        p = pp_skip_spaces(p);
        if (*p == ')' || !*p) break;
        const char *before = p;
        p = pp_parse_header_item(p, list);
        while (*p && *p != ' ' && *p != '\t' && *p != ')')
            p++;
        if (p == before) p++;
    }
    return pp_skip_parameters(p);
}

static gboolean
pp_header_find(const char *header, const char *feature, ns_pp_allowlist *out)
{
    gboolean found = FALSE;
    gsize feature_len = strlen(feature);
    const char *p = header ? header : "";
    while (*p) {
        p = pp_skip_spaces(p);
        while (*p == ',') p = pp_skip_spaces(p + 1);
        const char *key = p;
        while (*p && *p != '=' && *p != ',' && *p != ';') p++;
        gsize key_len = (gsize)(p - key);
        while (key_len > 0 && (key[key_len - 1] == ' ' ||
                               key[key_len - 1] == '\t'))
            key_len--;
        ns_pp_allowlist list = { 0 };
        if (*p == '=') {
            p = pp_parse_header_value(pp_skip_spaces(p + 1), &list);
        } else {
            pp_allowlist_reset(&list);
            list.all = TRUE;
            p = pp_skip_parameters(p);
        }
        if (key_len == feature_len &&
            g_ascii_strncasecmp(key, feature, feature_len) == 0) {
            pp_allowlist_clear(out);
            *out = list;
            found = TRUE;
        } else {
            pp_allowlist_clear(&list);
        }
        if (*p == ',') p++;
    }
    return found;
}

ns_pp_verdict
ns_pp_header_verdict(const char *header, const char *feature,
                     const char *self_origin, const char *origin)
{
    if (!header || !*header || !feature) return NS_PP_UNDECLARED;
    ns_pp_allowlist list = { 0 };
    if (!pp_header_find(header, feature, &list)) return NS_PP_UNDECLARED;
    gboolean ok = pp_allowlist_matches(&list, self_origin, NULL, origin);
    pp_allowlist_clear(&list);
    return ok ? NS_PP_ALLOWED : NS_PP_DENIED;
}

char **
ns_pp_header_allowlist(const char *header, const char *feature,
                       const char *self_origin)
{
    ns_pp_allowlist list = { 0 };
    if (!header || !feature || !pp_header_find(header, feature, &list))
        return NULL;
    GPtrArray *out = g_ptr_array_new();
    if (list.all) {
        g_ptr_array_add(out, g_strdup("*"));
    } else {
        if (list.self && self_origin && *self_origin)
            g_ptr_array_add(out, g_strdup(self_origin));
        for (guint i = 0; i < list.origins->len; i++)
            g_ptr_array_add(out, g_strdup(g_ptr_array_index(list.origins, i)));
    }
    g_ptr_array_add(out, NULL);
    pp_allowlist_clear(&list);
    return (char **)g_ptr_array_free(out, FALSE);
}

ns_pp_verdict
ns_pp_allow_attr_verdict(const char *allow, const char *feature,
                         const char *self_origin, const char *src_origin,
                         const char *origin)
{
    if (!allow || !*allow || !feature) return NS_PP_UNDECLARED;
    ns_pp_verdict verdict = NS_PP_UNDECLARED;
    g_auto(GStrv) directives = g_strsplit(allow, ";", -1);
    for (char **d = directives; *d && verdict == NS_PP_UNDECLARED; d++) {
        g_auto(GStrv) tokens = g_strsplit_set(g_strstrip(*d), " \t\r\n\f", -1);
        char **t = tokens;
        while (*t && !**t) t++;
        if (!*t || g_ascii_strcasecmp(*t, feature) != 0) continue;
        ns_pp_allowlist list = { 0 };
        pp_allowlist_reset(&list);
        gboolean any = FALSE;
        for (t++; *t; t++) {
            if (!**t) continue;
            any = TRUE;
            pp_allowlist_add_token(&list, *t, strlen(*t), TRUE);
        }
        if (!any) list.src = TRUE;
        verdict = pp_allowlist_matches(&list, self_origin, src_origin, origin)
            ? NS_PP_ALLOWED : NS_PP_DENIED;
        pp_allowlist_clear(&list);
    }
    return verdict;
}
