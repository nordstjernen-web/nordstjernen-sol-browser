/* Nordstjernen — in-process debug event log shared with the JS console.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "debuglog.h"

#include <stdio.h>
#include <string.h>

#include <glib/gstdio.h>

#define NS_DLOG_CAPACITY 1024
#define NS_DLOG_FILE_MAX_BYTES (4 * 1024 * 1024)

typedef struct ns_dlog_sub {
    guint              id;
    ns_dlog_listener   cb;
    gpointer           user_data;
} ns_dlog_sub;

static GMutex   g_dlog_mutex;
static GQueue  *g_dlog_entries;
static GArray  *g_dlog_subs;
static guint    g_dlog_next_id = 1;
static gboolean g_dlog_inited;

static char    *g_dlog_file_path;
static FILE    *g_dlog_file;
static gboolean g_dlog_file_tried;

static gboolean
ns_dlog_file_enabled(void)
{
#ifdef G_OS_WIN32
    return !g_getenv("NS_NO_LOG_FILE");
#else
    return g_getenv("NS_LOG_FILE") != NULL;
#endif
}

const char *
ns_debug_log_file_path(void)
{
    if (!g_dlog_file_path && ns_dlog_file_enabled()) {
        const char *override = g_getenv("NS_LOG_FILE");
        if (override && *override)
            g_dlog_file_path = g_strdup(override);
        else
            g_dlog_file_path = g_build_filename(g_get_user_data_dir(),
                                                "Nordstjernen",
                                                "nordstjernen-debug.log", NULL);
    }
    return g_dlog_file_path;
}

static gboolean
ns_dlog_file_wants(ns_dlog_level level)
{
    if (!ns_dlog_file_enabled()) return FALSE;
    if (g_getenv("NS_LOG_FILE")) return TRUE;
    return level == NS_DLOG_WARN || level == NS_DLOG_ERROR;
}

static void
ns_dlog_file_write(const ns_dlog_entry *e)
{
    if (!ns_dlog_file_wants(e->level)) return;
    if (!g_dlog_file && !g_dlog_file_tried) {
        g_dlog_file_tried = TRUE;
        const char *path = ns_debug_log_file_path();
        if (path) {
            char *dir = g_path_get_dirname(path);
            if (dir) { g_mkdir_with_parents(dir, 0700); g_free(dir); }
            GStatBuf st;
            gboolean oversized = g_stat(path, &st) == 0 &&
                                 st.st_size > NS_DLOG_FILE_MAX_BYTES;
            g_dlog_file = g_fopen(path, oversized ? "w" : "a");
        }
    }
    if (!g_dlog_file) return;
    gint64 t = g_get_real_time() / 1000;
    fprintf(g_dlog_file, "%" G_GINT64_FORMAT " %-5s %s: %s\n",
            t, ns_dlog_level_name(e->level),
            e->category ? e->category : "", e->message ? e->message : "");
    fflush(g_dlog_file);
}

static void
ns_dlog_entry_free(gpointer p)
{
    ns_dlog_entry *e = p;
    if (!e) return;
    g_free(e->category);
    g_free(e->message);
    g_free(e);
}

void
ns_debug_log_init(void)
{
    g_mutex_lock(&g_dlog_mutex);
    if (!g_dlog_inited) {
        g_dlog_entries = g_queue_new();
        g_dlog_subs = g_array_new(FALSE, FALSE, sizeof(ns_dlog_sub));
        g_dlog_inited = TRUE;
    }
    g_mutex_unlock(&g_dlog_mutex);
}

const char *
ns_dlog_level_name(ns_dlog_level lvl)
{
    switch (lvl) {
    case NS_DLOG_INFO:   return "info";
    case NS_DLOG_WARN:   return "warn";
    case NS_DLOG_ERROR:  return "error";
    case NS_DLOG_RENDER: return "render";
    case NS_DLOG_NET:    return "net";
    case NS_DLOG_JS:     return "js";
    }
    return "?";
}

static void
ns_dlog_dispatch(const ns_dlog_entry *snap)
{
    GArray *subs_copy = NULL;
    g_mutex_lock(&g_dlog_mutex);
    if (g_dlog_subs && g_dlog_subs->len > 0) {
        subs_copy = g_array_sized_new(FALSE, FALSE, sizeof(ns_dlog_sub),
                                      g_dlog_subs->len);
        g_array_append_vals(subs_copy, g_dlog_subs->data, g_dlog_subs->len);
    }
    g_mutex_unlock(&g_dlog_mutex);
    if (!subs_copy) return;
    for (guint i = 0; i < subs_copy->len; i++) {
        ns_dlog_sub s = g_array_index(subs_copy, ns_dlog_sub, i);
        if (s.cb) s.cb(snap, s.user_data);
    }
    g_array_free(subs_copy, TRUE);
}

G_GNUC_PRINTF(3, 0)
static void
ns_debug_log_emit_v(ns_dlog_level level, const char *category,
                    const char *fmt, va_list ap)
{
    if (!g_dlog_inited) ns_debug_log_init();
    ns_dlog_entry *e = g_new0(ns_dlog_entry, 1);
    e->level = level;
    e->category = g_strdup(category ? category : "");
    e->message = fmt ? g_strdup_vprintf(fmt, ap) : g_strdup("");

    ns_dlog_dispatch(e);

    g_mutex_lock(&g_dlog_mutex);
    ns_dlog_file_write(e);
    g_queue_push_tail(g_dlog_entries, e);
    while (g_queue_get_length(g_dlog_entries) > NS_DLOG_CAPACITY) {
        ns_dlog_entry *drop = g_queue_pop_head(g_dlog_entries);
        ns_dlog_entry_free(drop);
    }
    g_mutex_unlock(&g_dlog_mutex);
}

void
ns_debug_log_emit(ns_dlog_level level, const char *category,
                  const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ns_debug_log_emit_v(level, category, fmt, ap);
    va_end(ap);
}

guint
ns_debug_log_subscribe(ns_dlog_listener cb, gpointer user_data)
{
    if (!cb) return 0;
    if (!g_dlog_inited) ns_debug_log_init();
    g_mutex_lock(&g_dlog_mutex);
    ns_dlog_sub s = { g_dlog_next_id++, cb, user_data };
    g_array_append_val(g_dlog_subs, s);
    guint id = s.id;
    g_mutex_unlock(&g_dlog_mutex);
    return id;
}

void
ns_debug_log_unsubscribe(guint id)
{
    if (id == 0 || !g_dlog_inited) return;
    g_mutex_lock(&g_dlog_mutex);
    for (guint i = 0; i < g_dlog_subs->len; i++) {
        if (g_array_index(g_dlog_subs, ns_dlog_sub, i).id == id) {
            g_array_remove_index(g_dlog_subs, i);
            break;
        }
    }
    g_mutex_unlock(&g_dlog_mutex);
}

