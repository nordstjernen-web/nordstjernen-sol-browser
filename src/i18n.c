/* Nordstjernen — UI string translation: picks the catalogue matching the
 * operating-system language and translates UI strings through it. */

#include "i18n.h"

#include <string.h>

static GHashTable *g_catalogue;

static char *
catalogue_path_in(const char *dir, const char *lang)
{
    char *file = g_strdup_printf("%s.lang", lang);
    char *path = g_build_filename(dir, file, NULL);
    g_free(file);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) return path;
    g_free(path);
    return NULL;
}

static char *
find_catalogue(const char *self_exe, const char *lang)
{
    const char *override = g_getenv("NS_I18N_DIR");
    if (override && *override) return catalogue_path_in(override, lang);

    static const char *const rel[] = {
        "../Resources/share/nordstjernen/i18n",
        "../share/nordstjernen/i18n",
        "share/nordstjernen/i18n",
        "data/i18n",
        "../data/i18n",
        "../../data/i18n",
        "../../../data/i18n",
        "../../../../data/i18n",
    };
    if (self_exe) {
        char *exe_dir = g_path_get_dirname(self_exe);
        for (gsize i = 0; i < G_N_ELEMENTS(rel); i++) {
            char *dir = g_build_filename(exe_dir, rel[i], NULL);
            char *path = catalogue_path_in(dir, lang);
            g_free(dir);
            if (path) {
                g_free(exe_dir);
                return path;
            }
        }
        g_free(exe_dir);
    }
    return catalogue_path_in("data/i18n", lang);
}

static GHashTable *
catalogue_load(const char *path)
{
    char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) return NULL;
    GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, g_free);
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines && lines[i]; i++) {
        char *line = lines[i];
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        char *key = g_strndup(line, (gsize)(eq - line));
        char *value = g_strdup(eq + 1);
        g_strchomp(value);
        if (*value)
            g_hash_table_replace(table, key, value);
        else {
            g_free(key);
            g_free(value);
        }
    }
    g_strfreev(lines);
    g_free(contents);
    if (g_hash_table_size(table) == 0) {
        g_hash_table_destroy(table);
        return NULL;
    }
    return table;
}

void
ns_i18n_init(const char *self_exe)
{
    if (g_catalogue) return;
    const gchar *const *names = g_get_language_names();
    for (int i = 0; names && names[i]; i++) {
        if (strcmp(names[i], "C") == 0 || strcmp(names[i], "POSIX") == 0)
            continue;
        if (strchr(names[i], '.') || strchr(names[i], '@'))
            continue;
        char *path = find_catalogue(self_exe, names[i]);
        if (!path) continue;
        GHashTable *table = catalogue_load(path);
        g_free(path);
        if (table) {
            g_catalogue = table;
            return;
        }
    }
}

const char *
ns_i18n(const char *text)
{
    if (!text || !g_catalogue) return text;
    const char *hit = g_hash_table_lookup(g_catalogue, text);
    return hit ? hit : text;
}
