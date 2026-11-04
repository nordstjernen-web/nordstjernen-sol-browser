/* Nordstjernen — persistent tab sessions kept in the profile's session.db. */

#include "sessionstore.h"

#include <sqlite3.h>

#include "config.h"
#include "db.h"

#define NS_SESSION_SCHEMA_VERSION 1
#define NS_SESSION_BEAT_S         5
#define NS_SESSION_DEAD_S         15

static sqlite3 *g_db;
static char    *g_owner;
static guint    g_beat_source;

static gint64
session_now(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

static gboolean
session_exec(const char *sql)
{
    return ns_db_exec(g_db, sql, "session");
}

static sqlite3_stmt *
session_prepare(const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) {
        g_warning("[session] %s", sqlite3_errmsg(g_db));
        return NULL;
    }
    return st;
}

static void
session_step_done(sqlite3_stmt *st)
{
    if (!st) return;
    if (sqlite3_step(st) != SQLITE_DONE)
        g_warning("[session] %s", sqlite3_errmsg(g_db));
    sqlite3_finalize(st);
}

static void
session_beat(void)
{
    sqlite3_stmt *st = session_prepare(
        "INSERT OR REPLACE INTO owners(id, beat) VALUES(?1, ?2)");
    if (!st) return;
    sqlite3_bind_text(st, 1, g_owner, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, session_now());
    session_step_done(st);
}

static gboolean
session_beat_cb(gpointer data)
{
    (void)data;
    if (g_db) session_beat();
    return G_SOURCE_CONTINUE;
}

static int
session_user_version(void)
{
    sqlite3_stmt *st = session_prepare("PRAGMA user_version");
    if (!st) return -1;
    int v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return v;
}

static gboolean
session_schema(void)
{
    if (!session_exec("PRAGMA journal_mode=WAL") ||
        !session_exec("PRAGMA synchronous=NORMAL"))
        return FALSE;
    if (session_user_version() != NS_SESSION_SCHEMA_VERSION &&
        (!session_exec("DROP TABLE IF EXISTS entries") ||
         !session_exec("DROP TABLE IF EXISTS tabs") ||
         !session_exec("DROP TABLE IF EXISTS owners")))
        return FALSE;
    return session_exec("CREATE TABLE IF NOT EXISTS owners("
                        "id TEXT PRIMARY KEY, beat INTEGER NOT NULL)") &&
           session_exec("CREATE TABLE IF NOT EXISTS tabs("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        "owner TEXT NOT NULL,"
                        "window INTEGER NOT NULL DEFAULT 0,"
                        "position INTEGER NOT NULL DEFAULT 0,"
                        "active INTEGER NOT NULL DEFAULT 0,"
                        "hist_index INTEGER NOT NULL DEFAULT -1,"
                        "storage TEXT)") &&
           session_exec("CREATE TABLE IF NOT EXISTS entries("
                        "tab INTEGER NOT NULL,"
                        "idx INTEGER NOT NULL,"
                        "url TEXT NOT NULL,"
                        "title TEXT,"
                        "scroll_x INTEGER NOT NULL DEFAULT 0,"
                        "scroll_y INTEGER NOT NULL DEFAULT 0,"
                        "PRIMARY KEY (tab, idx)) WITHOUT ROWID") &&
           session_exec("PRAGMA user_version="
                        G_STRINGIFY(NS_SESSION_SCHEMA_VERSION));
}

gboolean
ns_session_open(const char *instance_id)
{
    if (g_db) return TRUE;
    char *dir = g_build_filename(g_get_user_config_dir(), NS_APP_DIR_NAME,
                                 NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "session.db", NULL);
    g_free(dir);
    g_db = ns_db_open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_FULLMUTEX, "session");
    g_free(path);
    if (!g_db) return FALSE;
    sqlite3_busy_timeout(g_db, 2000);
    if (!session_schema()) {
        sqlite3_close(g_db);
        g_db = NULL;
        return FALSE;
    }
    g_owner = instance_id && *instance_id ? g_strdup(instance_id)
                                          : g_uuid_string_random();
    session_beat();
    g_beat_source = g_timeout_add_seconds(NS_SESSION_BEAT_S,
                                          session_beat_cb, NULL);
    return TRUE;
}

gboolean
ns_session_available(void)
{
    return g_db != NULL;
}

void
ns_session_close(void)
{
    if (!g_db) return;
    if (g_beat_source) {
        g_source_remove(g_beat_source);
        g_beat_source = 0;
    }
    sqlite3_stmt *st = session_prepare("DELETE FROM owners WHERE id = ?1");
    if (st) {
        sqlite3_bind_text(st, 1, g_owner, -1, SQLITE_TRANSIENT);
        session_step_done(st);
    }
    sqlite3_close(g_db);
    g_db = NULL;
    g_clear_pointer(&g_owner, g_free);
}

void
ns_session_tab_free(ns_session_tab *tab)
{
    if (!tab) return;
    if (tab->entries) g_ptr_array_unref(tab->entries);
    g_free(tab->storage);
    g_free(tab);
}

static void
session_entry_free(gpointer p)
{
    ns_session_entry *e = p;
    g_free(e->url);
    g_free(e->title);
    g_free(e);
}

static char *
session_column_text(sqlite3_stmt *st, int col)
{
    const unsigned char *t = sqlite3_column_text(st, col);
    return t ? g_strdup((const char *)t) : NULL;
}

static void
session_claim_orphans(void)
{
    gint64 cutoff = session_now() - NS_SESSION_DEAD_S;
    sqlite3_stmt *st = session_prepare(
        "UPDATE tabs SET owner = ?1 WHERE owner NOT IN "
        "(SELECT id FROM owners WHERE beat >= ?2)");
    if (st) {
        sqlite3_bind_text(st, 1, g_owner, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, cutoff);
        session_step_done(st);
    }
    st = session_prepare("DELETE FROM owners WHERE beat < ?1 AND id != ?2");
    if (st) {
        sqlite3_bind_int64(st, 1, cutoff);
        sqlite3_bind_text(st, 2, g_owner, -1, SQLITE_TRANSIENT);
        session_step_done(st);
    }
}

GPtrArray *
ns_session_take_tabs(gboolean own_only)
{
    GPtrArray *tabs = g_ptr_array_new_with_free_func(
        (GDestroyNotify)ns_session_tab_free);
    if (!g_db) return tabs;
    if (!own_only)
        session_claim_orphans();
    sqlite3_stmt *st = session_prepare(
        "SELECT id, window, position, active, hist_index, storage FROM tabs "
        "WHERE owner = ?1 ORDER BY window, position, id");
    if (!st) return tabs;
    sqlite3_bind_text(st, 1, g_owner, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        ns_session_tab *t = g_new0(ns_session_tab, 1);
        t->id = sqlite3_column_int64(st, 0);
        t->window = sqlite3_column_int(st, 1);
        t->position = sqlite3_column_int(st, 2);
        t->active = sqlite3_column_int(st, 3) != 0;
        t->index = sqlite3_column_int(st, 4);
        t->storage = session_column_text(st, 5);
        t->entries = g_ptr_array_new_with_free_func(session_entry_free);
        g_ptr_array_add(tabs, t);
    }
    sqlite3_finalize(st);

    st = session_prepare("SELECT url, title, scroll_x, scroll_y FROM entries "
                         "WHERE tab = ?1 ORDER BY idx");
    for (guint i = 0; st && i < tabs->len; i++) {
        ns_session_tab *t = g_ptr_array_index(tabs, i);
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, t->id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            ns_session_entry *e = g_new0(ns_session_entry, 1);
            e->url = session_column_text(st, 0);
            e->title = session_column_text(st, 1);
            e->scroll_x = sqlite3_column_int(st, 2);
            e->scroll_y = sqlite3_column_int(st, 3);
            if (e->url) g_ptr_array_add(t->entries, e);
            else session_entry_free(e);
        }
        if (t->index >= (int)t->entries->len)
            t->index = (int)t->entries->len - 1;
    }
    if (st) sqlite3_finalize(st);
    for (guint i = tabs->len; i > 0; i--) {
        ns_session_tab *t = g_ptr_array_index(tabs, i - 1);
        if (t->entries->len == 0) {
            ns_session_tab_remove(t->id);
            g_ptr_array_remove_index(tabs, i - 1);
        }
    }
    return tabs;
}

gint64
ns_session_tab_add(void)
{
    if (!g_db) return 0;
    sqlite3_stmt *st = session_prepare("INSERT INTO tabs(owner) VALUES(?1)");
    if (!st) return 0;
    sqlite3_bind_text(st, 1, g_owner, -1, SQLITE_TRANSIENT);
    session_step_done(st);
    return sqlite3_last_insert_rowid(g_db);
}

void
ns_session_tab_save(const ns_session_tab *tab)
{
    if (!g_db || !tab || tab->id <= 0) return;
    session_exec("BEGIN");
    sqlite3_stmt *st = session_prepare(
        "UPDATE tabs SET owner = ?1, window = ?2, position = ?3, active = ?4,"
        " hist_index = ?5, storage = ?6 WHERE id = ?7");
    if (st) {
        sqlite3_bind_text(st, 1, g_owner, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, tab->window);
        sqlite3_bind_int(st, 3, tab->position);
        sqlite3_bind_int(st, 4, tab->active ? 1 : 0);
        sqlite3_bind_int(st, 5, tab->index);
        if (tab->storage && *tab->storage)
            sqlite3_bind_text(st, 6, tab->storage, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(st, 6);
        sqlite3_bind_int64(st, 7, tab->id);
        session_step_done(st);
    }
    st = session_prepare("DELETE FROM entries WHERE tab = ?1");
    if (st) {
        sqlite3_bind_int64(st, 1, tab->id);
        session_step_done(st);
    }
    st = session_prepare("INSERT INTO entries(tab, idx, url, title, scroll_x,"
                         " scroll_y) VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
    for (guint i = 0; st && tab->entries && i < tab->entries->len; i++) {
        const ns_session_entry *e = g_ptr_array_index(tab->entries, i);
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, tab->id);
        sqlite3_bind_int(st, 2, (int)i);
        sqlite3_bind_text(st, 3, e->url ? e->url : "", -1, SQLITE_TRANSIENT);
        if (e->title)
            sqlite3_bind_text(st, 4, e->title, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(st, 4);
        sqlite3_bind_int(st, 5, e->scroll_x);
        sqlite3_bind_int(st, 6, e->scroll_y);
        if (sqlite3_step(st) != SQLITE_DONE)
            g_warning("[session] %s", sqlite3_errmsg(g_db));
    }
    if (st) sqlite3_finalize(st);
    session_exec("COMMIT");
}

void
ns_session_tab_remove(gint64 id)
{
    if (!g_db || id <= 0) return;
    sqlite3_stmt *st = session_prepare("DELETE FROM entries WHERE tab = ?1");
    if (st) {
        sqlite3_bind_int64(st, 1, id);
        session_step_done(st);
    }
    st = session_prepare("DELETE FROM tabs WHERE id = ?1");
    if (st) {
        sqlite3_bind_int64(st, 1, id);
        session_step_done(st);
    }
}
