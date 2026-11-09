/* Nordstjernen — browser-owned cookie store: in-memory partitions backed by one SQLite database with write-behind flushes. */

#include "cookies.h"
#include "db.h"

#include <curl/curl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libpsl.h>
#include <sqlite3.h>
#include <string.h>

#define NS_COOKIES_SCHEMA_VERSION 1
#define NS_COOKIE_MAX_PAIR_BYTES 4096
#define NS_COOKIE_MAX_ATTR_BYTES 1024
#define NS_COOKIES_PER_DOMAIN 180
#define NS_COOKIES_PER_PARTITION 3000
#define NS_COOKIE_FLUSH_DELAY_US (G_USEC_PER_SEC / 2)
#define NS_COOKIE_FLUSH_RETRY_US (5 * G_USEC_PER_SEC)
#define NS_COOKIE_FLUSH_BATCH 512
#define NS_COOKIE_ACCESS_PERSIST_US (60 * G_USEC_PER_SEC)

enum {
    NS_COOKIE_SAMESITE_DEFAULT = 0,
    NS_COOKIE_SAMESITE_NONE = 1,
    NS_COOKIE_SAMESITE_LAX = 2,
    NS_COOKIE_SAMESITE_STRICT = 3,
};

enum {
    NS_COOKIE_SOURCE_HTTP = 0,
    NS_COOKIE_SOURCE_SCRIPT = 1,
};

typedef struct ns_cookie {
    char    *key;
    char    *domain;
    char    *path;
    char    *name;
    char    *value;
    gint64   expires;
    gint64   created;
    gint64   last_access;
    gint64   access_persisted;
    int      samesite;
    int      source;
    gboolean host_only;
    gboolean secure;
    gboolean httponly;
} ns_cookie;

typedef struct ns_cookie_partition {
    char       *name;
    GHashTable *cookies;
    guint       epoch;
} ns_cookie_partition;

typedef struct ns_cookie_op {
    char      *partition;
    ns_cookie *cookie;
    gboolean   deleted;
} ns_cookie_op;

typedef struct ns_cookie_line {
    const char *name;
    gsize       name_len;
    const char *value;
    gsize       value_len;
    char       *domain;
    char       *path;
    gint64      expires;
    int         samesite;
    gboolean    secure;
    gboolean    httponly;
    gboolean    expired;
} ns_cookie_line;

typedef struct ns_cookie_store {
    GMutex        lock;
    GMutex        db_lock;
    GCond         cond;
    GHashTable   *partitions;
    GHashTable   *pending;
    GThread      *flusher;
    sqlite3      *db;
    sqlite3_stmt *st_load;
    sqlite3_stmt *st_data_version;
    sqlite3_stmt *st_upsert;
    sqlite3_stmt *st_delete;
    sqlite3_stmt *st_expire;
    char         *dir;
    gint64        first_pending_us;
    gint          generation;
    guint         epoch;
    int           data_version;
    gboolean      in_memory;
    gboolean      open_attempted;
    gboolean      kick;
    gboolean      quit;
} ns_cookie_store;

static ns_cookie_store S = { .generation = 1 };

static gint64
now_us(void)
{
    return g_get_real_time();
}

static gint64
now_s(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

static gint64
store_next_created_locked(void)
{
    static gint64 last_created;
    gint64 created = MAX(now_us(), last_created + 1);
    last_created = created;
    return created;
}

static char *
cookie_key(const char *domain, gboolean host_only, const char *path,
           const char *name)
{
    return g_strdup_printf("%s\x1f%d\x1f%s\x1f%s", domain, host_only ? 1 : 0,
                           path, name);
}

static void
cookie_free(ns_cookie *c)
{
    if (!c) return;
    g_free(c->key);
    g_free(c->domain);
    g_free(c->path);
    g_free(c->name);
    g_free(c->value);
    g_free(c);
}

static ns_cookie *
cookie_copy(const ns_cookie *c)
{
    ns_cookie *d = g_new(ns_cookie, 1);
    *d = *c;
    d->key = g_strdup(c->key);
    d->domain = g_strdup(c->domain);
    d->path = g_strdup(c->path);
    d->name = g_strdup(c->name);
    d->value = g_strdup(c->value);
    return d;
}

static void
op_free(ns_cookie_op *op)
{
    g_free(op->partition);
    cookie_free(op->cookie);
    g_free(op);
}

static void
partition_free(ns_cookie_partition *p)
{
    g_free(p->name);
    g_hash_table_destroy(p->cookies);
    g_free(p);
}

static GHashTable *
pending_table_new(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                 (GDestroyNotify)op_free);
}

static void
store_tables_ensure(void)
{
    if (!S.partitions)
        S.partitions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                             (GDestroyNotify)partition_free);
    if (!S.pending)
        S.pending = pending_table_new();
}

static char *
partition_normalize(const char *raw)
{
    const char *odd = strstr(raw, ":://");
    if (!odd) return g_strdup(raw);
    return g_strdup_printf("%.*s%s", (int)(odd - raw), raw, odd + 1);
}

static gboolean
store_exec(const char *sql)
{
    return ns_db_exec(S.db, sql, "cookies");
}

static gboolean
store_exec_quiet(const char *sql)
{
    return S.db && sqlite3_exec(S.db, sql, NULL, NULL, NULL) == SQLITE_OK;
}

static int
store_user_version(void)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(S.db, "PRAGMA user_version", -1, &st, NULL)
        != SQLITE_OK)
        return -1;
    int version = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0)
                                                 : -1;
    sqlite3_finalize(st);
    return version;
}

static gboolean
store_schema(void)
{
    if (!S.in_memory &&
        (!store_exec("PRAGMA journal_mode=WAL") ||
         !store_exec("PRAGMA synchronous=NORMAL")))
        return FALSE;
    if (store_user_version() != NS_COOKIES_SCHEMA_VERSION &&
        !store_exec("DROP TABLE IF EXISTS cookies"))
        return FALSE;
    return store_exec("CREATE TABLE IF NOT EXISTS cookies("
                      "partition TEXT NOT NULL,"
                      "domain TEXT NOT NULL COLLATE NOCASE,"
                      "host_only INTEGER NOT NULL,"
                      "path TEXT NOT NULL,"
                      "name TEXT NOT NULL,"
                      "value TEXT NOT NULL,"
                      "expires INTEGER NOT NULL,"
                      "secure INTEGER NOT NULL,"
                      "httponly INTEGER NOT NULL,"
                      "samesite INTEGER NOT NULL,"
                      "source INTEGER NOT NULL,"
                      "created INTEGER NOT NULL,"
                      "last_access INTEGER NOT NULL,"
                      "PRIMARY KEY (partition, domain, host_only, path, name)"
                      ") WITHOUT ROWID") &&
           store_exec("CREATE INDEX IF NOT EXISTS cookies_partition "
                      "ON cookies(partition)") &&
           store_exec("CREATE INDEX IF NOT EXISTS cookies_expires "
                      "ON cookies(expires) WHERE expires > 0") &&
           store_exec("PRAGMA user_version="
                      G_STRINGIFY(NS_COOKIES_SCHEMA_VERSION));
}

static gboolean
store_prepare(sqlite3_stmt **st, const char *sql)
{
    if (sqlite3_prepare_v3(S.db, sql, -1, SQLITE_PREPARE_PERSISTENT, st, NULL)
        == SQLITE_OK)
        return TRUE;
    g_warning("cookies: prepare failed: %s", sqlite3_errmsg(S.db));
    return FALSE;
}

static void
store_close_db(void)
{
    g_clear_pointer(&S.st_load, sqlite3_finalize);
    g_clear_pointer(&S.st_data_version, sqlite3_finalize);
    g_clear_pointer(&S.st_upsert, sqlite3_finalize);
    g_clear_pointer(&S.st_delete, sqlite3_finalize);
    g_clear_pointer(&S.st_expire, sqlite3_finalize);
    if (S.db) sqlite3_close(S.db);
    S.db = NULL;
}

static int
store_read_data_version(void)
{
    if (!S.st_data_version) return 0;
    sqlite3_reset(S.st_data_version);
    int v = sqlite3_step(S.st_data_version) == SQLITE_ROW
          ? sqlite3_column_int(S.st_data_version, 0) : 0;
    sqlite3_reset(S.st_data_version);
    return v;
}

static void
store_remove_jar_files(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".txt") &&
            !g_str_has_suffix(name, ".txt.samesite") &&
            strcmp(name, ".lock") != 0)
            continue;
        g_autofree char *path = g_build_filename(dir, name, NULL);
        g_unlink(path);
    }
    g_dir_close(d);
}

static void
store_open_locked(void)
{
    store_tables_ensure();
    if (S.open_attempted) return;
    S.open_attempted = TRUE;
    if (!S.in_memory && !S.dir) return;
    if (!S.in_memory) store_remove_jar_files(S.dir);
    g_autofree char *path = S.in_memory
        ? g_strdup(":memory:") : g_build_filename(S.dir, "cookies.db", NULL);
    S.db = ns_db_open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                      SQLITE_OPEN_FULLMUTEX, "cookies");
    if (!S.db) return;
    gboolean ok = store_schema() &&
        store_prepare(&S.st_load,
            "SELECT domain,host_only,path,name,value,expires,secure,httponly,"
            "samesite,source,created,last_access FROM cookies "
            "WHERE partition=?") &&
        store_prepare(&S.st_data_version, "PRAGMA data_version") &&
        store_prepare(&S.st_upsert,
            "INSERT OR REPLACE INTO cookies(partition,domain,host_only,path,"
            "name,value,expires,secure,httponly,samesite,source,created,"
            "last_access) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)") &&
        store_prepare(&S.st_delete,
            "DELETE FROM cookies WHERE partition=? AND domain=? AND "
            "host_only=? AND path=? AND name=?") &&
        store_prepare(&S.st_expire,
            "DELETE FROM cookies WHERE expires > 0 AND expires < ?");
    if (!ok) {
        store_close_db();
        return;
    }
    S.data_version = store_read_data_version();
}

static void
store_check_external_writes_locked(void)
{
    if (!S.db || S.in_memory) return;
    int v = store_read_data_version();
    if (v == S.data_version) return;
    S.data_version = v;
    S.epoch++;
    g_atomic_int_inc(&S.generation);
}

static const char *
column_text(sqlite3_stmt *st, int col)
{
    const char *s = (const char *)sqlite3_column_text(st, col);
    return s ? s : "";
}

static void
partition_load_rows_locked(ns_cookie_partition *p)
{
    sqlite3_reset(S.st_load);
    sqlite3_bind_text(S.st_load, 1, p->name, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(S.st_load) == SQLITE_ROW) {
        ns_cookie *c = g_new0(ns_cookie, 1);
        c->domain = g_strdup(column_text(S.st_load, 0));
        c->host_only = sqlite3_column_int(S.st_load, 1) != 0;
        c->path = g_strdup(column_text(S.st_load, 2));
        c->name = g_strdup(column_text(S.st_load, 3));
        c->value = g_strdup(column_text(S.st_load, 4));
        c->expires = sqlite3_column_int64(S.st_load, 5);
        c->secure = sqlite3_column_int(S.st_load, 6) != 0;
        c->httponly = sqlite3_column_int(S.st_load, 7) != 0;
        c->samesite = sqlite3_column_int(S.st_load, 8);
        c->source = sqlite3_column_int(S.st_load, 9);
        c->created = sqlite3_column_int64(S.st_load, 10);
        c->last_access = sqlite3_column_int64(S.st_load, 11);
        c->access_persisted = c->last_access;
        c->key = cookie_key(c->domain, c->host_only, c->path, c->name);
        g_hash_table_replace(p->cookies, c->key, c);
    }
    sqlite3_reset(S.st_load);
}

static void
partition_overlay_pending_locked(ns_cookie_partition *p)
{
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, S.pending);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        ns_cookie_op *op = v;
        if (strcmp(op->partition, p->name) != 0) continue;
        if (op->deleted) {
            g_hash_table_remove(p->cookies, op->cookie->key);
        } else {
            ns_cookie *c = cookie_copy(op->cookie);
            g_hash_table_replace(p->cookies, c->key, c);
        }
    }
}

static ns_cookie_partition *
partition_get_locked(const char *raw)
{
    g_autofree char *name = partition_normalize(raw);
    ns_cookie_partition *p = g_hash_table_lookup(S.partitions, name);
    if (p && p->epoch == S.epoch) return p;
    if (!p) {
        p = g_new0(ns_cookie_partition, 1);
        p->name = g_strdup(name);
        p->cookies = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                           (GDestroyNotify)cookie_free);
        g_hash_table_replace(S.partitions, p->name, p);
    } else {
        g_hash_table_remove_all(p->cookies);
    }
    if (S.db) partition_load_rows_locked(p);
    partition_overlay_pending_locked(p);
    p->epoch = S.epoch;
    return p;
}

static gpointer flusher_main(gpointer data);

static void
store_wake_flusher_locked(void)
{
    if (!S.flusher && !S.quit)
        S.flusher = g_thread_new("ns-cookies", flusher_main, NULL);
    g_cond_broadcast(&S.cond);
}

static void
store_queue_locked(const char *partition, const ns_cookie *c, gboolean deleted)
{
    if (!S.db) return;
    char *pkey = g_strdup_printf("%s\x1e%s", partition, c->key);
    ns_cookie_op *op = g_new0(ns_cookie_op, 1);
    op->partition = g_strdup(partition);
    op->cookie = cookie_copy(c);
    op->deleted = deleted;
    if (!g_hash_table_size(S.pending))
        S.first_pending_us = g_get_monotonic_time();
    g_hash_table_replace(S.pending, pkey, op);
    store_wake_flusher_locked();
}

static void
bind_text(sqlite3_stmt *st, int col, const char *s)
{
    sqlite3_bind_text(st, col, s ? s : "", -1, SQLITE_TRANSIENT);
}

static gboolean
store_write_op(const ns_cookie_op *op)
{
    const ns_cookie *c = op->cookie;
    sqlite3_stmt *st = op->deleted ? S.st_delete : S.st_upsert;
    sqlite3_reset(st);
    if (op->deleted) {
        bind_text(st, 1, op->partition);
        bind_text(st, 2, c->domain);
        sqlite3_bind_int(st, 3, c->host_only ? 1 : 0);
        bind_text(st, 4, c->path);
        bind_text(st, 5, c->name);
    } else {
        bind_text(st, 1, op->partition);
        bind_text(st, 2, c->domain);
        sqlite3_bind_int(st, 3, c->host_only ? 1 : 0);
        bind_text(st, 4, c->path);
        bind_text(st, 5, c->name);
        bind_text(st, 6, c->value);
        sqlite3_bind_int64(st, 7, c->expires);
        sqlite3_bind_int(st, 8, c->secure ? 1 : 0);
        sqlite3_bind_int(st, 9, c->httponly ? 1 : 0);
        sqlite3_bind_int(st, 10, c->samesite);
        sqlite3_bind_int(st, 11, c->source);
        sqlite3_bind_int64(st, 12, c->created);
        sqlite3_bind_int64(st, 13, c->last_access);
    }
    gboolean ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_reset(st);
    return ok;
}

static gboolean
store_write_ops(GHashTable *ops)
{
    gboolean ok = S.db && store_exec_quiet("BEGIN IMMEDIATE");
    if (!ok) return FALSE;
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, ops);
    while (ok && g_hash_table_iter_next(&it, NULL, &v))
        ok = store_write_op(v);
    if (ok) {
        sqlite3_reset(S.st_expire);
        sqlite3_bind_int64(S.st_expire, 1, now_s());
        ok = sqlite3_step(S.st_expire) == SQLITE_DONE;
        sqlite3_reset(S.st_expire);
    }
    if (ok) ok = store_exec_quiet("COMMIT");
    if (!ok) store_exec_quiet("ROLLBACK");
    return ok;
}

static gboolean
store_flush_locked(void)
{
    S.kick = FALSE;
    if (!S.pending || !g_hash_table_size(S.pending)) return TRUE;
    GHashTable *ops = S.pending;
    S.pending = pending_table_new();
    gboolean has_db = S.db != NULL;
    g_mutex_lock(&S.db_lock);
    g_mutex_unlock(&S.lock);
    gboolean ok = !has_db || store_write_ops(ops);
    g_mutex_unlock(&S.db_lock);
    g_mutex_lock(&S.lock);
    if (ok) {
        g_hash_table_destroy(ops);
        return TRUE;
    }
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, ops);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (g_hash_table_contains(S.pending, k)) continue;
        g_hash_table_iter_steal(&it);
        g_hash_table_replace(S.pending, k, v);
    }
    g_hash_table_destroy(ops);
    S.first_pending_us = g_get_monotonic_time();
    return FALSE;
}

static gpointer
flusher_main(gpointer data)
{
    (void)data;
    g_mutex_lock(&S.lock);
    while (!S.quit) {
        guint n = S.pending ? g_hash_table_size(S.pending) : 0;
        if (!n) {
            g_cond_wait(&S.cond, &S.lock);
            continue;
        }
        gint64 due = S.first_pending_us + NS_COOKIE_FLUSH_DELAY_US;
        if (S.kick || n >= NS_COOKIE_FLUSH_BATCH ||
            g_get_monotonic_time() >= due) {
            if (!store_flush_locked() && !S.quit)
                g_cond_wait_until(&S.cond, &S.lock,
                                  g_get_monotonic_time() +
                                  NS_COOKIE_FLUSH_RETRY_US);
            continue;
        }
        g_cond_wait_until(&S.cond, &S.lock, due);
    }
    g_mutex_unlock(&S.lock);
    return NULL;
}

void
ns_cookie_store_init(const char *dir, gboolean in_memory)
{
    g_mutex_lock(&S.lock);
    g_free(S.dir);
    S.dir = g_strdup(dir);
    S.in_memory = in_memory;
    S.quit = FALSE;
    store_tables_ensure();
    g_mutex_unlock(&S.lock);
}

void
ns_cookie_store_flush(void)
{
    g_mutex_lock(&S.lock);
    store_tables_ensure();
    store_flush_locked();
    g_mutex_unlock(&S.lock);
}

void
ns_cookie_store_flush_soon(void)
{
    g_mutex_lock(&S.lock);
    if (S.pending && g_hash_table_size(S.pending)) {
        S.kick = TRUE;
        store_wake_flusher_locked();
    }
    g_mutex_unlock(&S.lock);
}

void
ns_cookie_store_shutdown(void)
{
    g_mutex_lock(&S.lock);
    S.quit = TRUE;
    g_cond_broadcast(&S.cond);
    GThread *flusher = S.flusher;
    S.flusher = NULL;
    g_mutex_unlock(&S.lock);
    if (flusher) g_thread_join(flusher);

    g_mutex_lock(&S.lock);
    store_tables_ensure();
    store_flush_locked();
    g_hash_table_remove_all(S.partitions);
    g_hash_table_remove_all(S.pending);
    g_mutex_lock(&S.db_lock);
    store_close_db();
    g_mutex_unlock(&S.db_lock);
    S.open_attempted = FALSE;
    g_clear_pointer(&S.dir, g_free);
    g_mutex_unlock(&S.lock);
}

static gboolean
host_is_ip(const char *host)
{
    return g_hostname_is_ip_address(host) ||
           (host[0] == '[' && host[strlen(host) - 1] == ']');
}

static gboolean
domain_matches(const char *host, const char *domain)
{
    if (g_ascii_strcasecmp(host, domain) == 0) return TRUE;
    gsize hl = strlen(host), dl = strlen(domain);
    return hl > dl && host[hl - dl - 1] == '.' &&
           g_ascii_strcasecmp(host + hl - dl, domain) == 0 &&
           !host_is_ip(host);
}

static gboolean
path_matches(const char *request_path, const char *cookie_path)
{
    if (strcmp(request_path, cookie_path) == 0) return TRUE;
    if (!g_str_has_prefix(request_path, cookie_path)) return FALSE;
    gsize cl = strlen(cookie_path);
    return cl > 0 && (cookie_path[cl - 1] == '/' || request_path[cl] == '/');
}

static gboolean
host_is_loopback_address(const char *host)
{
    gsize len = strlen(host);
    g_autofree char *literal = len > 2 && host[0] == '[' && host[len - 1] == ']'
        ? g_strndup(host + 1, len - 2) : g_strdup(host);
    GInetAddress *address = g_inet_address_new_from_string(literal);
    if (!address) return FALSE;
    gboolean loopback = g_inet_address_get_is_loopback(address);
    g_object_unref(address);
    return loopback;
}

static gboolean
url_is_secure_context(const char *scheme, const char *host)
{
    if (g_ascii_strcasecmp(scheme, "https") == 0 ||
        g_ascii_strcasecmp(scheme, "wss") == 0)
        return TRUE;
    return g_ascii_strcasecmp(host, "localhost") == 0 ||
           g_str_has_suffix(host, ".localhost") ||
           host_is_loopback_address(host);
}

typedef struct ns_request_url {
    char *scheme;
    char *host;
    char *path;
    gboolean secure;
} ns_request_url;

static gboolean
request_url_parse(const char *url, ns_request_url *out)
{
    memset(out, 0, sizeof(*out));
    if (!url) return FALSE;
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_ENCODED | G_URI_FLAGS_SCHEME_NORMALIZE, NULL);
    if (!uri) return FALSE;
    const char *scheme = g_uri_get_scheme(uri);
    const char *host = g_uri_get_host(uri);
    const char *path = g_uri_get_path(uri);
    if (!scheme || !host || !*host) {
        g_uri_unref(uri);
        return FALSE;
    }
    out->scheme = g_ascii_strdown(scheme, -1);
    out->host = strchr(host, ':') ? g_strdup_printf("[%s]", host)
                                  : g_ascii_strdown(host, -1);
    out->path = path && *path ? g_strdup(path) : g_strdup("/");
    out->secure = url_is_secure_context(out->scheme, out->host);
    g_uri_unref(uri);
    return TRUE;
}

static void
request_url_clear(ns_request_url *u)
{
    g_free(u->scheme);
    g_free(u->host);
    g_free(u->path);
}

static char *
default_path_for(const char *request_path)
{
    if (!request_path || request_path[0] != '/') return g_strdup("/");
    const char *last_slash = strrchr(request_path, '/');
    if (!last_slash || last_slash == request_path) return g_strdup("/");
    return g_strndup(request_path, (gsize)(last_slash - request_path));
}

static void
trim_wsp(const char **s, gsize *len)
{
    while (*len && ((*s)[0] == ' ' || (*s)[0] == '\t')) { (*s)++; (*len)--; }
    while (*len && ((*s)[*len - 1] == ' ' || (*s)[*len - 1] == '\t')) (*len)--;
}

static gboolean
line_has_forbidden_control(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 0x20 && c != '\t') || c == 0x7f) return TRUE;
    }
    return FALSE;
}

static void
line_apply_attribute(ns_cookie_line *line, const char *k, gsize klen,
                     const char *v, gsize vlen, gint64 now,
                     gboolean *has_max_age)
{
    if (vlen > NS_COOKIE_MAX_ATTR_BYTES) return;
    if (klen == 6 && g_ascii_strncasecmp(k, "secure", 6) == 0) {
        line->secure = TRUE;
    } else if (klen == 8 && g_ascii_strncasecmp(k, "httponly", 8) == 0) {
        line->httponly = TRUE;
    } else if (klen == 8 && g_ascii_strncasecmp(k, "samesite", 8) == 0) {
        if (vlen == 4 && g_ascii_strncasecmp(v, "none", 4) == 0)
            line->samesite = NS_COOKIE_SAMESITE_NONE;
        else if (vlen == 3 && g_ascii_strncasecmp(v, "lax", 3) == 0)
            line->samesite = NS_COOKIE_SAMESITE_LAX;
        else if (vlen == 6 && g_ascii_strncasecmp(v, "strict", 6) == 0)
            line->samesite = NS_COOKIE_SAMESITE_STRICT;
        else
            line->samesite = NS_COOKIE_SAMESITE_DEFAULT;
    } else if (klen == 7 && g_ascii_strncasecmp(k, "max-age", 7) == 0) {
        if (!vlen || (!g_ascii_isdigit(v[0]) && v[0] != '-')) return;
        g_autofree char *tmp = g_strndup(v, vlen);
        char *end = NULL;
        gint64 max_age = g_ascii_strtoll(tmp, &end, 10);
        if (!end || *end) return;
        *has_max_age = TRUE;
        line->expired = max_age <= 0;
        line->expires = max_age > 0 ? now + max_age : 0;
    } else if (klen == 7 && g_ascii_strncasecmp(k, "expires", 7) == 0) {
        if (*has_max_age || !vlen) return;
        g_autofree char *tmp = g_strndup(v, vlen);
        time_t t = curl_getdate(tmp, NULL);
        if (t == (time_t)-1) return;
        if ((gint64)t <= now) {
            line->expired = TRUE;
            line->expires = 0;
        } else {
            line->expired = FALSE;
            line->expires = (gint64)t;
        }
    } else if (klen == 6 && g_ascii_strncasecmp(k, "domain", 6) == 0) {
        if (vlen && v[0] == '.') { v++; vlen--; }
        g_free(line->domain);
        line->domain = vlen ? g_ascii_strdown(v, (gssize)vlen) : NULL;
    } else if (klen == 4 && g_ascii_strncasecmp(k, "path", 4) == 0) {
        g_free(line->path);
        line->path = (vlen && v[0] == '/') ? g_strndup(v, vlen) : NULL;
    }
}

static gboolean
line_parse(const char *set_cookie, ns_cookie_line *line, gint64 now)
{
    memset(line, 0, sizeof(*line));
    if (line_has_forbidden_control(set_cookie)) return FALSE;
    const char *semi = strchr(set_cookie, ';');
    const char *pair = set_cookie;
    gsize pair_len = semi ? (gsize)(semi - set_cookie) : strlen(set_cookie);
    const char *eq = memchr(pair, '=', pair_len);
    if (eq) {
        line->name = pair;
        line->name_len = (gsize)(eq - pair);
        line->value = eq + 1;
        line->value_len = pair_len - line->name_len - 1;
    } else {
        line->name = pair;
        line->name_len = 0;
        line->value = pair;
        line->value_len = pair_len;
    }
    trim_wsp(&line->name, &line->name_len);
    trim_wsp(&line->value, &line->value_len);
    if (!line->name_len && !line->value_len) return FALSE;
    if (line->name_len + line->value_len > NS_COOKIE_MAX_PAIR_BYTES) return FALSE;

    gboolean has_max_age = FALSE;
    for (const char *p = semi; p && *p; ) {
        p++;
        const char *end = strchr(p, ';');
        gsize alen = end ? (gsize)(end - p) : strlen(p);
        const char *aeq = memchr(p, '=', alen);
        const char *k = p;
        gsize klen = aeq ? (gsize)(aeq - p) : alen;
        const char *v = aeq ? aeq + 1 : p + alen;
        gsize vlen = aeq ? alen - klen - 1 : 0;
        trim_wsp(&k, &klen);
        trim_wsp(&v, &vlen);
        if (klen)
            line_apply_attribute(line, k, klen, v, vlen, now, &has_max_age);
        p = end;
    }
    return TRUE;
}

static void
line_clear(ns_cookie_line *line)
{
    g_free(line->domain);
    g_free(line->path);
}

static gboolean
bytes_have_prefix(const char *s, gsize len, const char *prefix)
{
    gsize plen = strlen(prefix);
    return len >= plen && g_ascii_strncasecmp(s, prefix, plen) == 0;
}

static gboolean
name_has_prefix(const ns_cookie_line *line, const char *prefix)
{
    return bytes_have_prefix(line->name, line->name_len, prefix);
}

static gboolean
nameless_value_forges_cookie(const ns_cookie_line *line)
{
    if (line->name_len) return FALSE;
    return memchr(line->value, '=', line->value_len) != NULL ||
           bytes_have_prefix(line->value, line->value_len, "__Secure-") ||
           bytes_have_prefix(line->value, line->value_len, "__Host-");
}

static gboolean
line_passes_policy(const ns_cookie_line *line, const ns_request_url *req,
                   gboolean from_http)
{
    if (nameless_value_forges_cookie(line)) return FALSE;
    if (!from_http && line->httponly) return FALSE;
    if (line->secure && !req->secure) return FALSE;
    if (line->samesite == NS_COOKIE_SAMESITE_NONE && !line->secure) return FALSE;
    if (name_has_prefix(line, "__Secure-") && !line->secure) return FALSE;
    if (name_has_prefix(line, "__Host-") &&
        (!line->secure || line->domain ||
         !line->path || strcmp(line->path, "/") != 0))
        return FALSE;
    return TRUE;
}

static gboolean
line_resolve_domain(ns_cookie_line *line, const ns_request_url *req,
                    gboolean *host_only)
{
    *host_only = TRUE;
    if (!line->domain) return TRUE;
    const psl_ctx_t *psl = psl_builtin();
    if (psl && psl_is_public_suffix(psl, line->domain)) {
        if (g_ascii_strcasecmp(req->host, line->domain) != 0) return FALSE;
        g_clear_pointer(&line->domain, g_free);
        return TRUE;
    }
    if (!domain_matches(req->host, line->domain)) return FALSE;
    *host_only = FALSE;
    return TRUE;
}

static gboolean
secure_cookie_blocks_overlay(ns_cookie_partition *p, const ns_cookie *fresh)
{
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, p->cookies);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        const ns_cookie *old = v;
        if (!old->secure || strcmp(old->name, fresh->name) != 0) continue;
        if (!domain_matches(fresh->domain, old->domain) &&
            !domain_matches(old->domain, fresh->domain))
            continue;
        if (path_matches(fresh->path, old->path)) return TRUE;
    }
    return FALSE;
}

static int
cmp_last_access(gconstpointer a, gconstpointer b)
{
    const ns_cookie *ca = *(const ns_cookie *const *)a;
    const ns_cookie *cb = *(const ns_cookie *const *)b;
    return ca->last_access < cb->last_access ? -1
         : ca->last_access > cb->last_access ? 1 : 0;
}

static void
partition_remove_locked(ns_cookie_partition *p, ns_cookie *c)
{
    store_queue_locked(p->name, c, TRUE);
    g_hash_table_remove(p->cookies, c->key);
}

static void
partition_evict_locked(ns_cookie_partition *p, const char *domain)
{
    gint64 now = now_s();
    GPtrArray *same_domain = g_ptr_array_new();
    GPtrArray *all = g_ptr_array_new();
    GHashTable *victims = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, p->cookies);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        ns_cookie *c = v;
        if (c->expires && c->expires < now) {
            g_hash_table_add(victims, c);
            continue;
        }
        g_ptr_array_add(all, c);
        if (g_ascii_strcasecmp(c->domain, domain) == 0)
            g_ptr_array_add(same_domain, c);
    }
    gboolean over = same_domain->len > NS_COOKIES_PER_DOMAIN ||
                    g_hash_table_size(p->cookies) > NS_COOKIES_PER_PARTITION;
    if (over) {
        g_ptr_array_sort(same_domain, cmp_last_access);
        for (guint i = 0; same_domain->len - i > NS_COOKIES_PER_DOMAIN; i++)
            g_hash_table_add(victims, g_ptr_array_index(same_domain, i));
        g_ptr_array_sort(all, cmp_last_access);
        for (guint i = 0;
             g_hash_table_size(p->cookies) - g_hash_table_size(victims) >
                 NS_COOKIES_PER_PARTITION && i < all->len; i++)
            g_hash_table_add(victims, g_ptr_array_index(all, i));
        g_hash_table_iter_init(&it, victims);
        while (g_hash_table_iter_next(&it, &v, NULL))
            partition_remove_locked(p, v);
    }
    g_hash_table_destroy(victims);
    g_ptr_array_free(same_domain, TRUE);
    g_ptr_array_free(all, TRUE);
}

gboolean
ns_cookie_store_set(const char *partition, const char *url,
                    const char *set_cookie, gboolean from_http)
{
    if (!partition || !*partition || !url || !set_cookie) return FALSE;
    ns_request_url req;
    if (!request_url_parse(url, &req)) return FALSE;
    if (strcmp(req.scheme, "http") != 0 && strcmp(req.scheme, "https") != 0) {
        request_url_clear(&req);
        return FALSE;
    }
    gint64 now = now_s();
    ns_cookie_line line;
    gboolean host_only = TRUE;
    gboolean ok = line_parse(set_cookie, &line, now) &&
                  line_passes_policy(&line, &req, from_http) &&
                  line_resolve_domain(&line, &req, &host_only);
    if (!ok) {
        line_clear(&line);
        request_url_clear(&req);
        return FALSE;
    }

    ns_cookie fresh = {0};
    fresh.domain = line.domain ? g_strdup(line.domain) : g_strdup(req.host);
    fresh.host_only = host_only;
    fresh.path = line.path ? g_strdup(line.path) : default_path_for(req.path);
    fresh.name = g_strndup(line.name, line.name_len);
    fresh.value = g_strndup(line.value, line.value_len);
    fresh.expires = line.expires;
    fresh.secure = line.secure;
    fresh.httponly = line.httponly;
    fresh.samesite = line.samesite;
    fresh.source = from_http ? NS_COOKIE_SOURCE_HTTP : NS_COOKIE_SOURCE_SCRIPT;
    fresh.key = cookie_key(fresh.domain, fresh.host_only, fresh.path, fresh.name);

    gboolean changed = FALSE;
    g_mutex_lock(&S.lock);
    store_open_locked();
    store_check_external_writes_locked();
    fresh.created = store_next_created_locked();
    fresh.last_access = fresh.created;
    fresh.access_persisted = fresh.created;
    ns_cookie_partition *p = partition_get_locked(partition);
    ns_cookie *old = g_hash_table_lookup(p->cookies, fresh.key);
    gboolean allowed = TRUE;
    if (!from_http && old && old->httponly) allowed = FALSE;
    if (allowed && !fresh.secure && !req.secure &&
        secure_cookie_blocks_overlay(p, &fresh))
        allowed = FALSE;
    if (allowed && line.expired) {
        if (old) {
            partition_remove_locked(p, old);
            changed = TRUE;
        }
    } else if (allowed) {
        if (old) fresh.created = old->created;
        ns_cookie *stored = cookie_copy(&fresh);
        g_hash_table_replace(p->cookies, stored->key, stored);
        store_queue_locked(p->name, stored, FALSE);
        partition_evict_locked(p, stored->domain);
        changed = TRUE;
    }
    if (changed) {
        g_atomic_int_inc(&S.generation);
        if (!from_http) S.kick = TRUE;
    }
    g_mutex_unlock(&S.lock);

    g_free(fresh.key);
    g_free(fresh.domain);
    g_free(fresh.path);
    g_free(fresh.name);
    g_free(fresh.value);
    line_clear(&line);
    request_url_clear(&req);
    return changed;
}

static gboolean
cookie_sent_in_context(const ns_cookie *c, int samesite_cross)
{
    if (samesite_cross <= 0) return TRUE;
    int level = c->samesite == NS_COOKIE_SAMESITE_DEFAULT
              ? NS_COOKIE_SAMESITE_LAX : c->samesite;
    if (level == NS_COOKIE_SAMESITE_STRICT) return FALSE;
    return samesite_cross == 1 || level == NS_COOKIE_SAMESITE_NONE;
}

static int
cmp_header_order(gconstpointer a, gconstpointer b)
{
    const ns_cookie *ca = *(const ns_cookie *const *)a;
    const ns_cookie *cb = *(const ns_cookie *const *)b;
    gsize la = strlen(ca->path), lb = strlen(cb->path);
    if (la != lb) return la > lb ? -1 : 1;
    return ca->created < cb->created ? -1 : ca->created > cb->created ? 1 : 0;
}

static GPtrArray *
partition_matches_locked(ns_cookie_partition *p, const ns_request_url *req,
                         int samesite_cross, gboolean include_httponly)
{
    gint64 now = now_s();
    gint64 now_micros = now_us();
    GPtrArray *out = g_ptr_array_new();
    GPtrArray *dead = g_ptr_array_new();
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, p->cookies);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        ns_cookie *c = v;
        if (c->expires && c->expires < now) {
            g_ptr_array_add(dead, c);
            continue;
        }
        gboolean domain_ok = c->host_only
            ? g_ascii_strcasecmp(req->host, c->domain) == 0
            : domain_matches(req->host, c->domain);
        if (!domain_ok || !path_matches(req->path, c->path)) continue;
        if (c->secure && !req->secure) continue;
        if (c->httponly && !include_httponly) continue;
        if (!cookie_sent_in_context(c, samesite_cross)) continue;
        c->last_access = now_micros;
        if (now_micros - c->access_persisted > NS_COOKIE_ACCESS_PERSIST_US) {
            c->access_persisted = now_micros;
            store_queue_locked(p->name, c, FALSE);
        }
        g_ptr_array_add(out, c);
    }
    for (guint i = 0; i < dead->len; i++)
        partition_remove_locked(p, g_ptr_array_index(dead, i));
    g_ptr_array_free(dead, TRUE);
    g_ptr_array_sort(out, cmp_header_order);
    return out;
}

static void
append_pair(GString *out, const ns_cookie *c)
{
    if (out->len) g_string_append(out, "; ");
    if (*c->name) {
        g_string_append(out, c->name);
        g_string_append_c(out, '=');
    }
    g_string_append(out, c->value);
}

char *
ns_cookie_store_header(const char *partition, const char *url,
                       int samesite_cross, gboolean include_httponly)
{
    if (!partition || !*partition || !url) return NULL;
    ns_request_url req;
    if (!request_url_parse(url, &req)) return NULL;
    GString *out = g_string_new(NULL);
    g_mutex_lock(&S.lock);
    store_open_locked();
    store_check_external_writes_locked();
    ns_cookie_partition *p = partition_get_locked(partition);
    GPtrArray *matches = partition_matches_locked(p, &req, samesite_cross,
                                                  include_httponly);
    for (guint i = 0; i < matches->len; i++)
        append_pair(out, g_ptr_array_index(matches, i));
    g_ptr_array_free(matches, TRUE);
    g_mutex_unlock(&S.lock);
    request_url_clear(&req);
    if (!out->len) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

guint
ns_cookie_store_generation(void)
{
    g_mutex_lock(&S.lock);
    store_open_locked();
    store_check_external_writes_locked();
    guint generation = (guint)g_atomic_int_get(&S.generation);
    g_mutex_unlock(&S.lock);
    return generation;
}

static void
pending_drop_partition_locked(const char *name)
{
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, S.pending);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        ns_cookie_op *op = v;
        if (!name || strcmp(op->partition, name) == 0)
            g_hash_table_iter_remove(&it);
    }
}

void
ns_cookie_store_clear(void)
{
    g_mutex_lock(&S.lock);
    store_open_locked();
    g_hash_table_remove_all(S.partitions);
    pending_drop_partition_locked(NULL);
    S.kick = FALSE;
    g_mutex_lock(&S.db_lock);
    if (S.db) store_exec("DELETE FROM cookies");
    g_mutex_unlock(&S.db_lock);
    g_atomic_int_inc(&S.generation);
    g_mutex_unlock(&S.lock);
}

void
ns_cookie_store_clear_site(const char *partition)
{
    if (!partition || !*partition) return;
    g_autofree char *name = partition_normalize(partition);
    g_mutex_lock(&S.lock);
    store_open_locked();
    g_hash_table_remove(S.partitions, name);
    pending_drop_partition_locked(name);
    g_mutex_lock(&S.db_lock);
    if (S.db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(S.db, "DELETE FROM cookies WHERE partition=?",
                               -1, &st, NULL) == SQLITE_OK) {
            bind_text(st, 1, name);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    g_mutex_unlock(&S.db_lock);
    g_atomic_int_inc(&S.generation);
    g_mutex_unlock(&S.lock);
}

static void
json_append_string(GString *out, const char *s)
{
    g_string_append_c(out, '"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            g_string_append_c(out, '\\');
            g_string_append_c(out, (char)c);
        } else if (c < 0x20) {
            g_string_append_printf(out, "\\u%04x", c);
        } else {
            g_string_append_c(out, (char)c);
        }
    }
    g_string_append_c(out, '"');
}

static const char *
samesite_name(int samesite)
{
    switch (samesite) {
    case NS_COOKIE_SAMESITE_NONE:   return "none";
    case NS_COOKIE_SAMESITE_STRICT: return "strict";
    default:                        return "lax";
    }
}

static void
json_append_cookie(GString *out, const ns_cookie *c)
{
    g_string_append(out, "{\"name\":");
    json_append_string(out, c->name);
    g_string_append(out, ",\"value\":");
    json_append_string(out, c->value);
    g_string_append(out, ",\"domain\":");
    json_append_string(out, c->domain);
    g_string_append_printf(out, ",\"hostOnly\":%s,\"path\":",
                           c->host_only ? "true" : "false");
    json_append_string(out, c->path);
    if (c->expires)
        g_string_append_printf(out, ",\"expires\":%" G_GINT64_FORMAT,
                               c->expires * 1000);
    else
        g_string_append(out, ",\"expires\":null");
    g_string_append_printf(out, ",\"secure\":%s,\"httpOnly\":%s,"
                           "\"sameSite\":\"%s\",\"source\":\"%s\"}",
                           c->secure ? "true" : "false",
                           c->httponly ? "true" : "false",
                           samesite_name(c->samesite),
                           c->source == NS_COOKIE_SOURCE_SCRIPT ? "script"
                                                               : "http");
}

char *
ns_cookie_store_sites_json(void)
{
    ns_cookie_store_flush();
    GString *out = g_string_new("[");
    g_mutex_lock(&S.lock);
    store_open_locked();
    g_mutex_lock(&S.db_lock);
    if (S.db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(S.db,
                "SELECT partition, count(*) FROM cookies "
                "WHERE expires = 0 OR expires >= ? "
                "GROUP BY partition ORDER BY partition",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now_s());
            gboolean first = TRUE;
            while (sqlite3_step(st) == SQLITE_ROW) {
                if (!first) g_string_append_c(out, ',');
                first = FALSE;
                g_string_append(out, "{\"site\":");
                json_append_string(out, column_text(st, 0));
                g_string_append_printf(out, ",\"count\":%d}",
                                       sqlite3_column_int(st, 1));
            }
            sqlite3_finalize(st);
        }
    } else {
        GHashTableIter it;
        gpointer k, v;
        gboolean first = TRUE;
        g_hash_table_iter_init(&it, S.partitions);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            ns_cookie_partition *p = v;
            if (!g_hash_table_size(p->cookies)) continue;
            if (!first) g_string_append_c(out, ',');
            first = FALSE;
            g_string_append(out, "{\"site\":");
            json_append_string(out, p->name);
            g_string_append_printf(out, ",\"count\":%u}",
                                   g_hash_table_size(p->cookies));
        }
    }
    g_mutex_unlock(&S.db_lock);
    g_mutex_unlock(&S.lock);
    g_string_append_c(out, ']');
    return g_string_free(out, FALSE);
}

char *
ns_cookie_store_site_json(const char *partition)
{
    if (!partition || !*partition) return g_strdup("[]");
    GString *out = g_string_new("[");
    g_mutex_lock(&S.lock);
    store_open_locked();
    store_check_external_writes_locked();
    ns_cookie_partition *p = partition_get_locked(partition);
    gint64 now = now_s();
    GPtrArray *rows = g_ptr_array_new();
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, p->cookies);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        ns_cookie *c = v;
        if (!c->expires || c->expires >= now) g_ptr_array_add(rows, c);
    }
    g_ptr_array_sort(rows, cmp_header_order);
    for (guint i = 0; i < rows->len; i++) {
        if (i) g_string_append_c(out, ',');
        json_append_cookie(out, g_ptr_array_index(rows, i));
    }
    g_ptr_array_free(rows, TRUE);
    g_mutex_unlock(&S.lock);
    g_string_append_c(out, ']');
    return g_string_free(out, FALSE);
}

char *
ns_cookie_store_visible_json(const char *partition, const char *url)
{
    return ns_cookie_store_matching_json(partition, url, FALSE);
}

char *
ns_cookie_store_matching_json(const char *partition, const char *url,
                              gboolean include_httponly)
{
    if (!partition || !*partition || !url) return g_strdup("[]");
    ns_request_url req;
    if (!request_url_parse(url, &req)) return g_strdup("[]");
    GString *out = g_string_new("[");
    g_mutex_lock(&S.lock);
    store_open_locked();
    store_check_external_writes_locked();
    ns_cookie_partition *p = partition_get_locked(partition);
    GPtrArray *matches = partition_matches_locked(p, &req, 0,
                                                  include_httponly);
    for (guint i = 0; i < matches->len; i++) {
        if (i) g_string_append_c(out, ',');
        json_append_cookie(out, g_ptr_array_index(matches, i));
    }
    g_ptr_array_free(matches, TRUE);
    g_mutex_unlock(&S.lock);
    request_url_clear(&req);
    g_string_append_c(out, ']');
    return g_string_free(out, FALSE);
}
