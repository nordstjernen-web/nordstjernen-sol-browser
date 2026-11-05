/* Nordstjernen — shell-owned per-site permission ledger shared by every tab. */

#include "permissions.h"

#include <string.h>

typedef struct {
    ns_perm_listener fn;
    gpointer         user_data;
    guint            id;
} perm_listener;

static GMutex      g_perm_lock;
static GHashTable *g_perm_entries;
static GKeyFile   *g_perm_file;
static char       *g_perm_path;
static GArray     *g_perm_listeners;
static guint       g_perm_next_listener = 1;

static char *
perm_key(const char *origin, ns_perm_kind kind)
{
    return g_strdup_printf("%s %s", ns_capability_name(kind), origin);
}

static gboolean
perm_origin_storable(const char *origin)
{
    return origin && strstr(origin, "://") && !strchr(origin, '\n') &&
           !strchr(origin, '[') && !strchr(origin, ']');
}

static void
perm_save_locked(void)
{
    if (!g_perm_file || !g_perm_path) return;
    GError *err = NULL;
    if (!g_key_file_save_to_file(g_perm_file, g_perm_path, &err)) {
        g_warning("[permissions] could not save %s: %s", g_perm_path,
                  err ? err->message : "unknown error");
        g_clear_error(&err);
    }
}

static void
perm_load_locked(void)
{
    gsize n_groups = 0;
    char **groups = g_key_file_get_groups(g_perm_file, &n_groups);
    for (gsize i = 0; i < n_groups; i++) {
        for (int k = 0; k < NS_PERM_N_KINDS; k++) {
            char *v = g_key_file_get_string(g_perm_file, groups[i],
                                            ns_capability_name((ns_perm_kind)k), NULL);
            ns_perm_state st = ns_cap_state_from_name(v);
            if (st != NS_PERM_ASK)
                g_hash_table_insert(g_perm_entries,
                                    perm_key(groups[i], (ns_perm_kind)k),
                                    GINT_TO_POINTER(st));
            g_free(v);
        }
    }
    g_strfreev(groups);
}

void
ns_perm_init(gboolean persistent)
{
    g_mutex_lock(&g_perm_lock);
    if (!g_perm_entries) {
        g_perm_entries = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
        g_perm_listeners = g_array_new(FALSE, TRUE, sizeof(perm_listener));
        if (persistent) {
            char *dir = g_build_filename(g_get_user_config_dir(),
                                         "nordstjernen", NULL);
            g_mkdir_with_parents(dir, 0700);
            g_perm_path = g_build_filename(dir, "permissions.ini", NULL);
            g_free(dir);
            g_perm_file = g_key_file_new();
            g_key_file_load_from_file(g_perm_file, g_perm_path,
                                      G_KEY_FILE_NONE, NULL);
            perm_load_locked();
        }
    }
    g_mutex_unlock(&g_perm_lock);
}

void
ns_perm_shutdown(void)
{
    g_mutex_lock(&g_perm_lock);
    g_clear_pointer(&g_perm_entries, g_hash_table_unref);
    g_clear_pointer(&g_perm_file, g_key_file_free);
    g_clear_pointer(&g_perm_path, g_free);
    if (g_perm_listeners) {
        g_array_free(g_perm_listeners, TRUE);
        g_perm_listeners = NULL;
    }
    g_mutex_unlock(&g_perm_lock);
}

static ns_perm_state
perm_default(ns_perm_kind kind)
{
    const ns_capability_info *info = ns_capability_info_get(kind);
    return info ? info->default_state : NS_PERM_ASK;
}

ns_perm_state
ns_perm_get(const char *origin, ns_perm_kind kind)
{
    if (!origin || !*origin) return perm_default(kind);
    g_mutex_lock(&g_perm_lock);
    gpointer stored = NULL;
    gboolean found = FALSE;
    if (g_perm_entries) {
        char *key = perm_key(origin, kind);
        found = g_hash_table_lookup_extended(g_perm_entries, key, NULL,
                                             &stored);
        g_free(key);
    }
    g_mutex_unlock(&g_perm_lock);
    return found ? (ns_perm_state)GPOINTER_TO_INT(stored) : perm_default(kind);
}

void
ns_perm_set(const char *origin, ns_perm_kind kind, ns_perm_state state)
{
    if (!origin || !*origin || kind >= NS_PERM_N_KINDS) return;
    g_mutex_lock(&g_perm_lock);
    if (!g_perm_entries) {
        g_mutex_unlock(&g_perm_lock);
        return;
    }
    char *key = perm_key(origin, kind);
    gpointer stored = NULL;
    ns_perm_state prev = g_hash_table_lookup_extended(g_perm_entries, key,
                                                      NULL, &stored)
        ? (ns_perm_state)GPOINTER_TO_INT(stored) : perm_default(kind);
    if (state == NS_PERM_ASK)
        g_hash_table_remove(g_perm_entries, key);
    else
        g_hash_table_insert(g_perm_entries, g_strdup(key),
                            GINT_TO_POINTER(state));
    g_free(key);
    if (g_perm_file && perm_origin_storable(origin)) {
        if (state == NS_PERM_ASK)
            g_key_file_remove_key(g_perm_file, origin,
                                  ns_capability_name(kind), NULL);
        else
            g_key_file_set_string(g_perm_file, origin, ns_capability_name(kind),
                                  ns_cap_state_name(state));
        gsize n_keys = 0;
        char **keys = g_key_file_get_keys(g_perm_file, origin, &n_keys, NULL);
        if (keys && n_keys == 0)
            g_key_file_remove_group(g_perm_file, origin, NULL);
        g_strfreev(keys);
        perm_save_locked();
    }
    GArray *snapshot = g_array_copy(g_perm_listeners);
    g_mutex_unlock(&g_perm_lock);

    if (prev != state) {
        for (guint i = 0; i < snapshot->len; i++) {
            perm_listener *l = &g_array_index(snapshot, perm_listener, i);
            l->fn(origin, kind, state, prev, l->user_data);
        }
    }
    g_array_free(snapshot, TRUE);
}

char *
ns_perm_entry_line(const char *origin, ns_perm_kind kind, ns_perm_state state)
{
    return g_strdup_printf("%s %s %s\n", ns_capability_name(kind),
                           ns_cap_state_name(state), origin);
}

char *
ns_perm_serialize_origin(const char *origin)
{
    GString *out = g_string_new(NULL);
    for (int k = 0; origin && *origin && k < NS_PERM_N_KINDS; k++) {
        ns_perm_state st = ns_perm_get(origin, (ns_perm_kind)k);
        if (st != perm_default((ns_perm_kind)k)) {
            char *line = ns_perm_entry_line(origin, (ns_perm_kind)k, st);
            g_string_append(out, line);
            g_free(line);
        }
    }
    return g_string_free(out, FALSE);
}

guint
ns_perm_add_listener(ns_perm_listener fn, gpointer user_data)
{
    g_mutex_lock(&g_perm_lock);
    guint id = 0;
    if (g_perm_listeners) {
        perm_listener l = { fn, user_data, g_perm_next_listener++ };
        g_array_append_val(g_perm_listeners, l);
        id = l.id;
    }
    g_mutex_unlock(&g_perm_lock);
    return id;
}

void
ns_perm_remove_listener(guint id)
{
    g_mutex_lock(&g_perm_lock);
    for (guint i = 0; g_perm_listeners && i < g_perm_listeners->len; i++) {
        if (g_array_index(g_perm_listeners, perm_listener, i).id == id) {
            g_array_remove_index(g_perm_listeners, i);
            break;
        }
    }
    g_mutex_unlock(&g_perm_lock);
}
