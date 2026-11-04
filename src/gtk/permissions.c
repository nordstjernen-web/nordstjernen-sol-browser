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

static const char *const perm_kind_names[NS_PERM_N_KINDS] = {
    [NS_PERM_CAMERA] = "camera",
    [NS_PERM_WEBGL]  = "webgl",
};

const char *
ns_perm_kind_name(ns_perm_kind kind)
{
    return kind < NS_PERM_N_KINDS ? perm_kind_names[kind] : "";
}

gboolean
ns_perm_kind_from_name(const char *name, ns_perm_kind *out)
{
    for (int k = 0; k < NS_PERM_N_KINDS; k++) {
        if (name && strcmp(name, perm_kind_names[k]) == 0) {
            *out = (ns_perm_kind)k;
            return TRUE;
        }
    }
    return FALSE;
}

static const char *
perm_state_name(ns_perm_state state)
{
    switch (state) {
    case NS_PERM_ALLOW: return "allow";
    case NS_PERM_DENY:  return "deny";
    default:            return "ask";
    }
}

static ns_perm_state
perm_state_from_name(const char *name)
{
    if (g_strcmp0(name, "allow") == 0) return NS_PERM_ALLOW;
    if (g_strcmp0(name, "deny") == 0) return NS_PERM_DENY;
    return NS_PERM_ASK;
}

static char *
perm_key(const char *origin, ns_perm_kind kind)
{
    return g_strdup_printf("%s %s", ns_perm_kind_name(kind), origin);
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
                                            perm_kind_names[k], NULL);
            ns_perm_state st = perm_state_from_name(v);
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

ns_perm_state
ns_perm_get(const char *origin, ns_perm_kind kind)
{
    if (!origin || !*origin) return NS_PERM_ASK;
    g_mutex_lock(&g_perm_lock);
    ns_perm_state st = NS_PERM_ASK;
    if (g_perm_entries) {
        char *key = perm_key(origin, kind);
        st = GPOINTER_TO_INT(g_hash_table_lookup(g_perm_entries, key));
        g_free(key);
    }
    g_mutex_unlock(&g_perm_lock);
    return st;
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
    ns_perm_state prev = GPOINTER_TO_INT(g_hash_table_lookup(g_perm_entries,
                                                             key));
    if (state == NS_PERM_ASK)
        g_hash_table_remove(g_perm_entries, key);
    else
        g_hash_table_insert(g_perm_entries, g_strdup(key),
                            GINT_TO_POINTER(state));
    g_free(key);
    if (g_perm_file && perm_origin_storable(origin)) {
        if (state == NS_PERM_ASK)
            g_key_file_remove_key(g_perm_file, origin,
                                  perm_kind_names[kind], NULL);
        else
            g_key_file_set_string(g_perm_file, origin, perm_kind_names[kind],
                                  perm_state_name(state));
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
    return g_strdup_printf("%s %s %s\n", ns_perm_kind_name(kind),
                           perm_state_name(state), origin);
}

char *
ns_perm_serialize(void)
{
    GString *out = g_string_new(NULL);
    g_mutex_lock(&g_perm_lock);
    if (g_perm_entries) {
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, g_perm_entries);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            const char *sp = strchr(key, ' ');
            if (!sp) continue;
            g_string_append_len(out, key, sp - (const char *)key);
            g_string_append_printf(out, " %s %s\n",
                                   perm_state_name(GPOINTER_TO_INT(value)),
                                   sp + 1);
        }
    }
    g_mutex_unlock(&g_perm_lock);
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
