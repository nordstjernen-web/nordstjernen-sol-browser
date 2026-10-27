/* Nordstjernen — flat key/value config loader.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

#include "net.h"
#include "security.h"

static ns_config g_cfg;
static char     *g_cfg_path;
static gboolean  g_cfg_defer_saves;
static char     *g_cfg_pending_save;

static void
set_string(char **slot, const char *value)
{
    g_free(*slot);
    *slot = g_strdup(value ? value : "");
}

typedef struct { const char *name; int val; } ns_enum_choice;

static int
parse_enum(const char *v, const ns_enum_choice *choices, gsize n, int dflt)
{
    if (!v || !*v) return dflt;
    for (gsize i = 0; i < n; i++)
        if (g_ascii_strcasecmp(v, choices[i].name) == 0) return choices[i].val;
    return dflt;
}

static gboolean
parse_bool(const char *v, gboolean dflt)
{
    static const ns_enum_choice map[] = {
        { "true", TRUE }, { "yes", TRUE }, { "on",  TRUE }, { "1", TRUE },
        { "false", FALSE }, { "no", FALSE }, { "off", FALSE }, { "0", FALSE },
    };
    return parse_enum(v, map, G_N_ELEMENTS(map), dflt) ? TRUE : FALSE;
}

static int
parse_int(const char *v, int dflt)
{
    if (!v || !*v) return dflt;
    char *end = NULL;
    gint64 n = g_ascii_strtoll(v, &end, 10);
    if (end == v) return dflt;
    if (n < (gint64)G_MININT) return G_MININT;
    if (n > (gint64)G_MAXINT) return G_MAXINT;
    return (int)n;
}

static ns_referer_policy
parse_referer_policy(const char *v, ns_referer_policy dflt)
{
    static const ns_enum_choice map[] = {
        { "none",                            NS_REFERER_NO_REFERRER },
        { "no-referrer",                     NS_REFERER_NO_REFERRER },
        { "same-origin",                     NS_REFERER_SAME_ORIGIN },
        { "strict-origin-when-cross-origin", NS_REFERER_STRICT_ORIGIN_WHEN_CROSS },
        { "default",                         NS_REFERER_STRICT_ORIGIN_WHEN_CROSS },
        { "unsafe-url",                      NS_REFERER_UNSAFE_URL },
        { "full",                            NS_REFERER_UNSAFE_URL },
    };
    return (ns_referer_policy)parse_enum(v, map, G_N_ELEMENTS(map), dflt);
}

static ns_cookie_policy
parse_cookie_policy(const char *v, ns_cookie_policy dflt)
{
    static const ns_enum_choice map[] = {
        { "always",           NS_COOKIE_ALWAYS },
        { "first-party",      NS_COOKIE_FIRST_PARTY },
        { "first-party-only", NS_COOKIE_FIRST_PARTY },
        { "never",            NS_COOKIE_NEVER },
        { "off",              NS_COOKIE_NEVER },
    };
    return (ns_cookie_policy)parse_enum(v, map, G_N_ELEMENTS(map), dflt);
}

static ns_color_scheme_pref
parse_color_scheme(const char *v, ns_color_scheme_pref dflt)
{
    static const ns_enum_choice map[] = {
        { "auto",   NS_COLOR_SCHEME_PREF_AUTO },
        { "system", NS_COLOR_SCHEME_PREF_AUTO },
        { "light",  NS_COLOR_SCHEME_PREF_LIGHT },
        { "dark",   NS_COLOR_SCHEME_PREF_DARK },
    };
    return (ns_color_scheme_pref)parse_enum(v, map, G_N_ELEMENTS(map), dflt);
}

static ns_reduced_motion_pref
parse_reduced_motion(const char *v, ns_reduced_motion_pref dflt)
{
    static const ns_enum_choice map[] = {
        { "auto",          NS_REDUCED_MOTION_PREF_AUTO },
        { "system",        NS_REDUCED_MOTION_PREF_AUTO },
        { "no-preference", NS_REDUCED_MOTION_PREF_NO_PREFERENCE },
        { "off",           NS_REDUCED_MOTION_PREF_NO_PREFERENCE },
        { "reduce",        NS_REDUCED_MOTION_PREF_REDUCE },
        { "on",            NS_REDUCED_MOTION_PREF_REDUCE },
    };
    return (ns_reduced_motion_pref)parse_enum(v, map, G_N_ELEMENTS(map), dflt);
}

typedef enum cfg_kind {
    CFG_STRING,
    CFG_BOOL,
    CFG_INT,
    CFG_REFERER,
    CFG_COOKIE,
    CFG_COLOR_SCHEME,
    CFG_REDUCED_MOTION,
} cfg_kind;

typedef struct cfg_field {
    const char *key;
    cfg_kind    kind;
    size_t      offset;
    const char *def_str;
    int         def_int;
} cfg_field;

#define FS(name, val)       { #name, CFG_STRING,       G_STRUCT_OFFSET(ns_config, name), val,   0 }
#define FB(name, val)       { #name, CFG_BOOL,         G_STRUCT_OFFSET(ns_config, name), NULL,  val }
#define FI(name, val)       { #name, CFG_INT,          G_STRUCT_OFFSET(ns_config, name), NULL,  val }
#define FE(name, kind, val) { #name, kind,             G_STRUCT_OFFSET(ns_config, name), NULL,  val }

static const cfg_field cfg_fields[] = {
    FS(home_url,              "about:start"),
    FS(user_agent,            ""),
    FS(compat_mode,           "chrome"),
    FS(accept_language,       ""),
    FS(search_engine,         NS_DEFAULT_SEARCH_ENGINE),
    FS(ai_model_mirror,       ""),
    FS(http_proxy,            ""),
    FS(https_proxy,           ""),
    FS(no_proxy,              ""),
    FS(doh_url,               ""),
    FS(gsk_renderer,          "auto"),
    FE(referer_policy,        CFG_REFERER,      NS_REFERER_STRICT_ORIGIN_WHEN_CROSS),
    FE(cookie_policy,         CFG_COOKIE,       NS_COOKIE_FIRST_PARTY),
    FE(color_scheme,          CFG_COLOR_SCHEME,    NS_COLOR_SCHEME_PREF_AUTO),
    FE(reduced_motion,        CFG_REDUCED_MOTION,  NS_REDUCED_MOTION_PREF_AUTO),
    FB(do_not_track,          FALSE),
    FB(global_privacy_control, FALSE),
    FB(strip_tracking_params, TRUE),
    FB(https_first,           TRUE),
    FB(harden_allocator,      TRUE),
    FB(speculative_preload,   TRUE),
    FB(async_image_decode,    TRUE),
    FB(images_enabled,        TRUE),
    FB(javascript_enabled,    TRUE),
    FB(webgl_enabled,         TRUE),
    { "webgpu", CFG_BOOL, G_STRUCT_OFFSET(ns_config, webgpu_enabled), NULL, TRUE },
    FB(camera_enabled,        FALSE),
    FB(microphone_enabled,    FALSE),
    FB(local_storage_enabled, TRUE),
    FB(cache_enabled,         TRUE),
    FB(tls_allow_insecure_override, FALSE),
    FB(watchdog_enabled,      TRUE),
    FB(remote_control,        FALSE),
    FI(cache_cap_mb,          256),
    FI(js_eval_budget_ms,     60000),
    FI(js_memory_cap_mb,      2048),
    FI(max_redirects,         NS_MAX_REDIRECTS),
    FI(window_width_px,       1280),
    FI(window_height_px,      800),
    FI(layout_viewport_px,    1000),
};

#undef FS
#undef FB
#undef FI
#undef FE

static void
apply_default(ns_config *c)
{
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++) {
        const cfg_field *f = &cfg_fields[i];
        void *slot = (char *)c + f->offset;
        switch (f->kind) {
        case CFG_STRING:      set_string((char **)slot, f->def_str); break;
        case CFG_BOOL:        *(gboolean *)slot = (gboolean)f->def_int; break;
        case CFG_INT:         *(int *)slot      = f->def_int; break;
        case CFG_REFERER:     *(ns_referer_policy *)slot     = (ns_referer_policy)f->def_int; break;
        case CFG_COOKIE:      *(ns_cookie_policy *)slot      = (ns_cookie_policy)f->def_int; break;
        case CFG_COLOR_SCHEME:    *(ns_color_scheme_pref *)slot   = (ns_color_scheme_pref)f->def_int; break;
        case CFG_REDUCED_MOTION:  *(ns_reduced_motion_pref *)slot = (ns_reduced_motion_pref)f->def_int; break;
        }
    }
}

static void
apply_pair(ns_config *c, const char *key, const char *value)
{
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++) {
        const cfg_field *f = &cfg_fields[i];
        if (strcmp(key, f->key) != 0) continue;
        void *slot = (char *)c + f->offset;
        switch (f->kind) {
        case CFG_STRING:       set_string((char **)slot, value); break;
        case CFG_BOOL:         *(gboolean *)slot = parse_bool(value, *(gboolean *)slot); break;
        case CFG_INT:          *(int *)slot      = parse_int(value, *(int *)slot); break;
        case CFG_REFERER:      *(ns_referer_policy *)slot     = parse_referer_policy(value, *(ns_referer_policy *)slot); break;
        case CFG_COOKIE:       *(ns_cookie_policy *)slot      = parse_cookie_policy(value, *(ns_cookie_policy *)slot); break;
        case CFG_COLOR_SCHEME:   *(ns_color_scheme_pref *)slot   = parse_color_scheme(value, *(ns_color_scheme_pref *)slot); break;
        case CFG_REDUCED_MOTION: *(ns_reduced_motion_pref *)slot = parse_reduced_motion(value, *(ns_reduced_motion_pref *)slot); break;
        }
        return;
    }
}

static void
parse_text(ns_config *c, const char *contents)
{
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key   = g_strstrip(line);
        char *value = g_strstrip(eq + 1);
        if (*key) apply_pair(c, key, value);
    }
    g_strfreev(lines);
}

static void
load_file(ns_config *c, const char *path)
{
    char *contents = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(path, &contents, &len, &err)) {
        if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning("config: failed to read %s: %s", path, err->message);
        g_clear_error(&err);
        return;
    }
    parse_text(c, contents);
    g_free(contents);
}

static const struct { const char *env; const char *key; } env_disable[] = {
    { "NS_NO_CACHE",         "cache_enabled"         },
    { "NS_NO_LOCAL_STORAGE", "local_storage_enabled" },
    { "NS_NO_IMAGES",        "images_enabled"        },
    { "NS_NO_JAVASCRIPT",    "javascript_enabled"    },
    { "NS_NO_WATCHDOG",      "watchdog_enabled"      },
    { "NS_NO_HTTPS_FIRST",   "https_first"           },
    { "NS_NO_HARDEN_ALLOC",  "harden_allocator"      },
    { "NS_NO_PRELOAD_SCAN",  "speculative_preload"   },
    { "NS_NO_ASYNC_IMG_DECODE", "async_image_decode" },
};

static const struct { const char *env; const char *key; } env_value[] = {
    { "NS_HOME_URL",    "home_url"    },
    { "NS_USER_AGENT",  "user_agent"  },
    { "NS_COMPAT_MODE", "compat_mode" },
    { "NS_HTTP_PROXY",  "http_proxy"  },
    { "NS_HTTPS_PROXY", "https_proxy" },
    { "NS_NO_PROXY",    "no_proxy"    },
    { "NS_DOH_URL",     "doh_url"     },
    { "NS_GSK_RENDERER","gsk_renderer"},
};

static void
apply_env(ns_config *c)
{
    for (gsize i = 0; i < G_N_ELEMENTS(env_disable); i++)
        if (ns_security_env_flag(env_disable[i].env))
            apply_pair(c, env_disable[i].key, "false");
    for (gsize i = 0; i < G_N_ELEMENTS(env_value); i++) {
        const char *v = g_getenv(env_value[i].env);
        if (v && *v) apply_pair(c, env_value[i].key, v);
    }
    if (ns_security_env_flag("NS_PRIVATE")) c->private_mode = TRUE;
}

static void
load_config(ns_config *c, const char *path)
{
    apply_default(c);
    load_file(c, path);
    if (g_strcmp0(c->search_engine, "https://lite.duckduckgo.com/lite/?q=%s") == 0 ||
        g_strcmp0(c->search_engine, "https://duckduckgo.com/?q=%s") == 0)
        set_string(&c->search_engine, NS_DEFAULT_SEARCH_ENGINE);
    apply_env(c);
}

void
ns_config_init(void)
{
    if (g_cfg_path)
        return;
    g_cfg_path = g_build_filename(g_get_user_config_dir(),
                                  NS_APP_DIR_NAME, "nordstjernen.conf",
                                  NULL);
    load_config(&g_cfg, g_cfg_path);
}

static void
adopt_field(ns_config *dst, ns_config *src, const cfg_field *f)
{
    void *to = (char *)dst + f->offset;
    void *from = (char *)src + f->offset;
    switch (f->kind) {
    case CFG_STRING: {
        char **to_s = to, **from_s = from;
        if (g_strcmp0(*to_s, *from_s) != 0) {
            g_free(*to_s);
            *to_s = *from_s;
        } else {
            g_free(*from_s);
        }
        *from_s = NULL;
        break;
    }
    case CFG_BOOL:           *(gboolean *)to = *(gboolean *)from; break;
    case CFG_INT:            *(int *)to = *(int *)from; break;
    case CFG_REFERER:        *(ns_referer_policy *)to = *(ns_referer_policy *)from; break;
    case CFG_COOKIE:         *(ns_cookie_policy *)to = *(ns_cookie_policy *)from; break;
    case CFG_COLOR_SCHEME:   *(ns_color_scheme_pref *)to = *(ns_color_scheme_pref *)from; break;
    case CFG_REDUCED_MOTION: *(ns_reduced_motion_pref *)to = *(ns_reduced_motion_pref *)from; break;
    }
}

void
ns_config_reload(void)
{
    if (!g_cfg_path)
        return;
    ns_config fresh;
    memset(&fresh, 0, sizeof fresh);
    load_config(&fresh, g_cfg_path);
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++)
        adopt_field(&g_cfg, &fresh, &cfg_fields[i]);
}

void
ns_config_shutdown(void)
{
    g_free(g_cfg.home_url);
    g_free(g_cfg.user_agent);
    g_free(g_cfg.accept_language);
    g_free(g_cfg.search_engine);
    g_free(g_cfg.ai_model_mirror);
    g_free(g_cfg.http_proxy);
    g_free(g_cfg.https_proxy);
    g_free(g_cfg.no_proxy);
    g_free(g_cfg.doh_url);
    g_free(g_cfg.gsk_renderer);
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_clear_pointer(&g_cfg_path, g_free);
}

const ns_config *
ns_config_get(void)
{
    return &g_cfg;
}

ns_config *
ns_config_mut(void)
{
    return &g_cfg;
}

static GMutex g_cfg_mutex;

void
ns_config_lock(void)
{
    g_mutex_lock(&g_cfg_mutex);
}

void
ns_config_unlock(void)
{
    g_mutex_unlock(&g_cfg_mutex);
}

static const char *const referer_policy_names[] = {
    [NS_REFERER_NO_REFERRER]              = "none",
    [NS_REFERER_SAME_ORIGIN]              = "same-origin",
    [NS_REFERER_STRICT_ORIGIN_WHEN_CROSS] = "strict-origin-when-cross-origin",
    [NS_REFERER_UNSAFE_URL]               = "unsafe-url",
};

static const char *const cookie_policy_names[] = {
    [NS_COOKIE_ALWAYS]      = "always",
    [NS_COOKIE_FIRST_PARTY] = "first-party",
    [NS_COOKIE_NEVER]       = "never",
};

static const char *const color_scheme_names[] = {
    [NS_COLOR_SCHEME_PREF_AUTO]  = "auto",
    [NS_COLOR_SCHEME_PREF_LIGHT] = "light",
    [NS_COLOR_SCHEME_PREF_DARK]  = "dark",
};

static const char *const reduced_motion_names[] = {
    [NS_REDUCED_MOTION_PREF_AUTO]          = "auto",
    [NS_REDUCED_MOTION_PREF_NO_PREFERENCE] = "no-preference",
    [NS_REDUCED_MOTION_PREF_REDUCE]        = "reduce",
};

static const char *
referer_policy_name(ns_referer_policy p)
{
    if ((unsigned)p >= G_N_ELEMENTS(referer_policy_names) || !referer_policy_names[p])
        return "strict-origin-when-cross-origin";
    return referer_policy_names[p];
}

static const char *
cookie_policy_name(ns_cookie_policy p)
{
    if ((unsigned)p >= G_N_ELEMENTS(cookie_policy_names) || !cookie_policy_names[p])
        return "first-party";
    return cookie_policy_names[p];
}

static const char *
color_scheme_name(ns_color_scheme_pref p)
{
    if ((unsigned)p >= G_N_ELEMENTS(color_scheme_names) || !color_scheme_names[p])
        return "auto";
    return color_scheme_names[p];
}

static const char *
reduced_motion_name(ns_reduced_motion_pref p)
{
    if ((unsigned)p >= G_N_ELEMENTS(reduced_motion_names) || !reduced_motion_names[p])
        return "auto";
    return reduced_motion_names[p];
}

static char *
field_string(const ns_config *c, const cfg_field *f)
{
    const void *slot = (const char *)c + f->offset;
    switch (f->kind) {
    case CFG_STRING: {
        const char *v = *(const char *const *)slot;
        char *line = g_strdup(v ? v : "");
        g_strdelimit(line, "\r\n", ' ');
        return line;
    }
    case CFG_BOOL:
        return g_strdup(*(const gboolean *)slot ? "true" : "false");
    case CFG_INT:
        return g_strdup_printf("%d", *(const int *)slot);
    case CFG_REFERER:
        return g_strdup(referer_policy_name(*(const ns_referer_policy *)slot));
    case CFG_COOKIE:
        return g_strdup(cookie_policy_name(*(const ns_cookie_policy *)slot));
    case CFG_COLOR_SCHEME:
        return g_strdup(color_scheme_name(*(const ns_color_scheme_pref *)slot));
    case CFG_REDUCED_MOTION:
        return g_strdup(reduced_motion_name(*(const ns_reduced_motion_pref *)slot));
    }
    return g_strdup("");
}

static GString *
config_serialize(const ns_config *c)
{
    GString *s = g_string_new(NULL);
    g_string_append(s, "# nordstjernen configuration\n");
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++) {
        g_autofree char *v = field_string(c, &cfg_fields[i]);
        g_string_append_printf(s, "%s = %s\n", cfg_fields[i].key, v);
    }
    return s;
}

static void
config_free_strings(ns_config *c)
{
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++)
        if (cfg_fields[i].kind == CFG_STRING)
            g_clear_pointer((char **)(void *)((char *)c + cfg_fields[i].offset), g_free);
}

static const char *const unprompted_keys[] = {
    "gsk_renderer", "speculative_preload", "async_image_decode",
    "images_enabled", "cache_cap_mb", "window_width_px", "window_height_px",
    "layout_viewport_px",
};

static gboolean
config_key_needs_prompt(const char *key)
{
    for (gsize k = 0; k < G_N_ELEMENTS(unprompted_keys); k++)
        if (strcmp(key, unprompted_keys[k]) == 0) return FALSE;
    return TRUE;
}

static gboolean
config_char_is_display_safe(gunichar c)
{
    if (c < 0x20 || c == 0x7f) return FALSE;
    if (c == 0x061c || c == 0x200e || c == 0x200f) return FALSE;
    if (c >= 0x202a && c <= 0x202e) return FALSE;
    if (c >= 0x2066 && c <= 0x2069) return FALSE;
    return TRUE;
}

static char *
config_display_value(const char *raw)
{
    enum { MAX_CHARS = 60 };
    g_autofree char *valid = g_utf8_make_valid(raw, -1);
    GString *out = g_string_new(NULL);
    gsize n = 0;
    for (const char *p = valid; *p; p = g_utf8_next_char(p)) {
        if (n++ == MAX_CHARS) {
            g_string_append(out, "\xe2\x80\xa6");
            break;
        }
        gunichar c = g_utf8_get_char(p);
        g_string_append_unichar(out, config_char_is_display_safe(c) ? c : 0xfffd);
    }
    return g_string_free(out, FALSE);
}

void
ns_config_defer_saves(gboolean defer)
{
    g_cfg_defer_saves = defer;
}

char *
ns_config_take_pending_save(void)
{
    return g_steal_pointer(&g_cfg_pending_save);
}

char *
ns_config_sensitive_changes(const char *text)
{
    if (!text) return NULL;
    ns_config proposed;
    memset(&proposed, 0, sizeof proposed);
    apply_default(&proposed);
    parse_text(&proposed, text);
    GString *out = g_string_new(NULL);
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++) {
        const cfg_field *f = &cfg_fields[i];
        if (!config_key_needs_prompt(f->key)) continue;
        g_autofree char *was = field_string(&g_cfg, f);
        g_autofree char *now = field_string(&proposed, f);
        if (strcmp(was, now) == 0) continue;
        g_autofree char *was_shown = config_display_value(was);
        g_autofree char *now_shown = config_display_value(now);
        g_string_append_printf(out, "%s: \u201c%s\u201d \u2192 \u201c%s\u201d\n",
                               f->key, was_shown, now_shown);
    }
    config_free_strings(&proposed);
    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

static gboolean config_write_file(GError **error);

gboolean
ns_config_apply_text(const char *text, GError **error)
{
    if (!text) return FALSE;
    ns_config fresh;
    memset(&fresh, 0, sizeof fresh);
    apply_default(&fresh);
    parse_text(&fresh, text);
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++)
        adopt_field(&g_cfg, &fresh, &cfg_fields[i]);
    config_free_strings(&fresh);
    return config_write_file(error);
}

gboolean
ns_config_save(GError **error)
{
    if (g_cfg_defer_saves) {
        g_free(g_cfg_pending_save);
        g_cfg_pending_save = g_string_free(config_serialize(&g_cfg), FALSE);
        return TRUE;
    }
    return config_write_file(error);
}

static gboolean
config_write_file(GError **error)
{
    if (!g_cfg_path) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                    "config path not initialized");
        return FALSE;
    }
    char *dir = g_path_get_dirname(g_cfg_path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "could not create config directory %s: %s",
                    dir, g_strerror(errno));
        g_free(dir);
        return FALSE;
    }
    g_free(dir);

    GString *s = config_serialize(&g_cfg);
    gboolean ok = g_file_set_contents_full(g_cfg_path, s->str, (gssize)s->len,
                                           G_FILE_SET_CONTENTS_CONSISTENT |
                                           G_FILE_SET_CONTENTS_ONLY_EXISTING, 0600, error);
    if (ok) g_chmod(g_cfg_path, 0600);
    g_string_free(s, TRUE);
    return ok;
}

char *
ns_config_dump(void)
{
    const ns_config *c = &g_cfg;
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "# nordstjernen effective config\n");
    g_string_append_printf(s, "# file: %s\n", g_cfg_path ? g_cfg_path : "(none)");
    g_string_append_printf(s, "home_url              = %s\n", c->home_url);
    g_string_append_printf(s, "user_agent            = %s\n", c->user_agent);
    if (c->accept_language && *c->accept_language)
        g_string_append_printf(s, "accept_language       = %s\n",
                               c->accept_language);
    else
        g_string_append_printf(s, "accept_language       = (auto: %s)\n",
                               ns_net_default_accept_language());
    g_string_append_printf(s, "search_engine         = %s\n", c->search_engine);
    if (c->ai_model_mirror && *c->ai_model_mirror)
        g_string_append_printf(s, "ai_model_mirror       = %s\n",
                               c->ai_model_mirror);
    g_string_append_printf(s, "gsk_renderer          = %s\n",
                           c->gsk_renderer && *c->gsk_renderer
                               ? c->gsk_renderer : "auto");
    {
        char *hp  = ns_net_proxy_mask(c->http_proxy);
        char *hsp = ns_net_proxy_mask(c->https_proxy);
        g_string_append_printf(s, "http_proxy            = %s\n",
                               hp  && *hp  ? hp  : "(none)");
        g_string_append_printf(s, "https_proxy           = %s\n",
                               hsp && *hsp ? hsp : "(none)");
        g_string_append_printf(s, "no_proxy              = %s\n",
                               c->no_proxy && *c->no_proxy ? c->no_proxy : "(none)");
        g_free(hp);
        g_free(hsp);
    }
    g_string_append_printf(s, "doh_url               = %s\n",
                           c->doh_url && *c->doh_url ? c->doh_url : "(system resolver)");
    g_string_append_printf(s, "referer_policy        = %s\n", referer_policy_name(c->referer_policy));
    g_string_append_printf(s, "cookie_policy         = %s\n", cookie_policy_name(c->cookie_policy));
    g_string_append_printf(s, "color_scheme          = %s\n", color_scheme_name(c->color_scheme));
    g_string_append_printf(s, "reduced_motion        = %s\n", reduced_motion_name(c->reduced_motion));
    g_string_append_printf(s, "do_not_track          = %s\n", c->do_not_track ? "true" : "false");
    g_string_append_printf(s, "global_privacy_control = %s\n", c->global_privacy_control ? "true" : "false");
    g_string_append_printf(s, "strip_tracking_params = %s\n", c->strip_tracking_params ? "true" : "false");
    g_string_append_printf(s, "https_first           = %s\n", c->https_first ? "true" : "false");
    g_string_append_printf(s, "harden_allocator      = %s\n", c->harden_allocator ? "true" : "false");
    g_string_append_printf(s, "speculative_preload   = %s\n", c->speculative_preload ? "true" : "false");
    g_string_append_printf(s, "async_image_decode    = %s\n", c->async_image_decode ? "true" : "false");
    g_string_append_printf(s, "images_enabled        = %s\n", c->images_enabled ? "true" : "false");
    g_string_append_printf(s, "javascript_enabled    = %s\n", c->javascript_enabled ? "true" : "false");
    g_string_append_printf(s, "webgl_enabled         = %s\n", c->webgl_enabled ? "true" : "false");
    g_string_append_printf(s, "webgpu                = %s\n", c->webgpu_enabled ? "true" : "false");
    g_string_append_printf(s, "camera_enabled        = %s\n", c->camera_enabled ? "true" : "false");
    g_string_append_printf(s, "microphone_enabled    = %s\n", c->microphone_enabled ? "true" : "false");
    g_string_append_printf(s, "local_storage_enabled = %s\n", c->local_storage_enabled ? "true" : "false");
    g_string_append_printf(s, "cache_enabled         = %s\n", c->cache_enabled ? "true" : "false");
    g_string_append_printf(s, "tls_allow_insecure_override = %s\n", c->tls_allow_insecure_override ? "true" : "false");
    g_string_append_printf(s, "watchdog_enabled      = %s\n", c->watchdog_enabled ? "true" : "false");
    g_string_append_printf(s, "remote_control        = %s\n", c->remote_control ? "true" : "false");
    g_string_append_printf(s, "private_mode          = %s\n", c->private_mode ? "true" : "false");
    g_string_append_printf(s, "cache_cap_mb          = %d\n", c->cache_cap_mb);
    g_string_append_printf(s, "js_eval_budget_ms     = %d\n", c->js_eval_budget_ms);
    g_string_append_printf(s, "js_memory_cap_mb      = %d\n", c->js_memory_cap_mb);
    g_string_append_printf(s, "max_redirects         = %d\n", c->max_redirects);
    g_string_append_printf(s, "window_width_px       = %d\n", c->window_width_px);
    g_string_append_printf(s, "window_height_px      = %d\n", c->window_height_px);
    g_string_append_printf(s, "layout_viewport_px    = %d\n", c->layout_viewport_px);
    return g_string_free(s, FALSE);
}
