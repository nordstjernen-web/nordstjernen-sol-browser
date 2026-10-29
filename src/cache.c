/* Nordstjernen — SQLite-indexed HTTP cache with on-disk bodies.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "cache.h"
#include "config.h"
#include "db.h"
#include "net.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

#ifdef G_OS_WIN32
#  include <windows.h>
#  include <aclapi.h>
#endif

static char    *g_cache_dir;
static sqlite3 *g_cache_db;
static gboolean g_cache_disabled;
static GMutex   g_cache_mutex;

#ifdef G_OS_WIN32
static gboolean
set_owner_only_w32(const char *utf8_path, gboolean container)
{
    wchar_t *wpath = g_utf8_to_utf16(utf8_path, -1, NULL, NULL, NULL);
    if (!wpath) return FALSE;
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        g_free(wpath);
        return FALSE;
    }
    DWORD need = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &need);
    if (need == 0) { CloseHandle(token); g_free(wpath); return FALSE; }
    TOKEN_USER *tu = g_malloc(need);
    if (!GetTokenInformation(token, TokenUser, tu, need, &need)) {
        g_free(tu); CloseHandle(token); g_free(wpath); return FALSE;
    }
    CloseHandle(token);

    EXPLICIT_ACCESSW ea;
    memset(&ea, 0, sizeof ea);
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = container
        ? (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)
        : NO_INHERITANCE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName    = (LPWSTR)tu->User.Sid;

    PACL dacl = NULL;
    DWORD r = SetEntriesInAclW(1, &ea, NULL, &dacl);
    if (r != ERROR_SUCCESS) {
        g_free(tu); g_free(wpath);
        return FALSE;
    }
    r = SetNamedSecurityInfoW(wpath, SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION |
                              PROTECTED_DACL_SECURITY_INFORMATION,
                              NULL, NULL, dacl, NULL);
    LocalFree(dacl);
    g_free(tu);
    g_free(wpath);
    return r == ERROR_SUCCESS;
}
#endif

static void
restrict_to_owner(const char *path, gboolean is_dir)
{
#ifdef G_OS_WIN32
    set_owner_only_w32(path, is_dir);
#else
    g_chmod(path, is_dir ? 0700 : 0600);
#endif
}

#define NS_CACHE_MAX_AGE_SECONDS (30 * 24 * 60 * 60)
#define NS_CACHE_HEURISTIC_MAX_SECONDS (24 * 60 * 60)
#define NS_CACHE_STALE_WHILE_REVALIDATE_MAX_SECONDS (7 * 24 * 60 * 60)

static void delete_key(const char *key);
static void cache_rmrf(const char *path);
static void evict_aged_out(void);
static void evict_to_cap(void);

static guint64
cache_cap_bytes(void)
{
    const ns_config *c = ns_config_get();
    int mb = c ? c->cache_cap_mb : 256;
    if (mb <= 0) mb = 256;
    return (guint64)mb * 1024ULL * 1024ULL;
}

static char *
base_key_for_url(const char *url, const char *partition)
{
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(c, (const guchar *)url, (gssize)strlen(url));
    if (partition && *partition) {
        g_checksum_update(c, (const guchar *)"\x1f", 1);
        g_checksum_update(c, (const guchar *)partition, (gssize)strlen(partition));
    }
    char *digest = g_strdup(g_checksum_get_string(c));
    g_checksum_free(c);
    return digest;
}

static char *
body_path_for_key(const char *key, gboolean ensure_dir)
{
    if (!g_cache_dir || !key || !key[0] || !key[1]) return NULL;
    char prefix[3] = { key[0], key[1], '\0' };
    char *sub = g_build_filename(g_cache_dir, "bodies", prefix, NULL);
    if (ensure_dir) {
        if (g_mkdir_with_parents(sub, 0700) == 0)
            restrict_to_owner(sub, TRUE);
    }
    char *out = g_build_filename(sub, key + 2, NULL);
    g_free(sub);
    return out;
}

static const char *
request_header_value(const char *const *headers, const char *name)
{
    gsize len = strlen(name);
    for (int i = 0; headers && headers[i]; i++) {
        if (g_ascii_strncasecmp(headers[i], name, len) != 0) continue;
        const char *p = headers[i] + len;
        if (*p != ':') continue;
        for (p++; *p == ' ' || *p == '\t'; p++) {}
        return p;
    }
    return NULL;
}

static gboolean
vary_selector(const char *vary, const char *const *request_headers, char **out)
{
    *out = NULL;
    if (!vary || !*vary) {
        *out = g_strdup("");
        return TRUE;
    }
    g_auto(GStrv) tokens = g_strsplit(vary, ",", -1);
    GString *selector = g_string_new(NULL);
    for (int i = 0; tokens[i]; i++) {
        g_autofree char *name = g_ascii_strdown(g_strstrip(tokens[i]), -1);
        if (!*name || strcmp(name, "accept-encoding") == 0) continue;
        if (strcmp(name, "accept") != 0 &&
            strcmp(name, "accept-language") != 0 &&
            strcmp(name, "user-agent") != 0 &&
            strcmp(name, "origin") != 0) {
            g_string_free(selector, TRUE);
            return FALSE;
        }
        const char *value = request_header_value(request_headers, name);
        if (!value) {
            g_string_free(selector, TRUE);
            return FALSE;
        }
        g_string_append(selector, name);
        g_string_append_c(selector, ':');
        g_string_append(selector, value);
        g_string_append_c(selector, '\n');
    }
    *out = g_string_free(selector, FALSE);
    return TRUE;
}

static char *
row_key_for(const char *base_key, const char *selector)
{
    if (!selector || !*selector) return g_strdup(base_key);
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(c, (const guchar *)base_key, (gssize)strlen(base_key));
    g_checksum_update(c, (const guchar *)"\x1f", 1);
    g_checksum_update(c, (const guchar *)selector, (gssize)strlen(selector));
    char *digest = g_strdup(g_checksum_get_string(c));
    g_checksum_free(c);
    return digest;
}

static gboolean
cache_exec(const char *sql)
{
    return ns_db_exec(g_cache_db, sql, "cache");
}

static void
cache_drop_if_legacy(void)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_cache_db,
            "SELECT name FROM pragma_table_info('entries')",
            -1, &st, NULL) != SQLITE_OK)
        return;
    gboolean table_exists = FALSE, has_body_size = FALSE;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 0);
        table_exists = TRUE;
        if (name && g_str_equal((const char *)name, "body_size"))
            has_body_size = TRUE;
    }
    sqlite3_finalize(st);
    if (table_exists && !has_body_size)
        cache_exec("DROP TABLE entries");
}

#define NS_CACHE_SCHEMA_VERSION 4

static int
cache_user_version(void)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_cache_db, "PRAGMA user_version", -1, &st, NULL)
        != SQLITE_OK)
        return -1;
    int version = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0)
                                                   : -1;
    sqlite3_finalize(st);
    return version;
}

static gboolean
cache_schema(void)
{
    if (!cache_exec("PRAGMA journal_mode=WAL") ||
        !cache_exec("PRAGMA synchronous=NORMAL"))
        return FALSE;
    if (cache_user_version() != NS_CACHE_SCHEMA_VERSION) {
        if (!cache_exec("DROP TABLE IF EXISTS entries")) return FALSE;
        if (g_cache_dir) {
            char *bodies = g_build_filename(g_cache_dir, "bodies", NULL);
            cache_rmrf(bodies);
            g_free(bodies);
        }
    }
    return cache_exec("CREATE TABLE IF NOT EXISTS entries("
                      "key TEXT PRIMARY KEY,"
                      "base_key TEXT NOT NULL,"
                      "url TEXT NOT NULL,"
                      "final_url TEXT,"
                      "status INTEGER NOT NULL,"
                      "content_type TEXT,"
                      "cors_allow_origin TEXT,"
                      "policy_headers TEXT,"
                      "etag TEXT,"
                      "last_modified TEXT,"
                      "vary TEXT,"
                      "expires_at INTEGER NOT NULL,"
                      "fetched_at INTEGER NOT NULL,"
                      "last_used INTEGER NOT NULL,"
                      "body_size INTEGER NOT NULL DEFAULT 0,"
                      "stale_until INTEGER NOT NULL DEFAULT 0)") &&
           cache_exec("CREATE INDEX IF NOT EXISTS idx_entries_last_used "
                      "ON entries(last_used)") &&
           cache_exec("CREATE INDEX IF NOT EXISTS idx_entries_base_key "
                      "ON entries(base_key)") &&
           cache_exec("PRAGMA user_version=" G_STRINGIFY(NS_CACHE_SCHEMA_VERSION));
}

static char *
find_row_key(const char *base_key, const char *const *request_headers)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_cache_db,
            "SELECT key,vary FROM entries WHERE base_key=?",
            -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, base_key, -1, SQLITE_TRANSIENT);
    char *found = NULL;
    while (!found && sqlite3_step(st) == SQLITE_ROW) {
        const char *stored = (const char *)sqlite3_column_text(st, 0);
        const char *vary   = (const char *)sqlite3_column_text(st, 1);
        g_autofree char *selector = NULL;
        if (!vary_selector(vary, request_headers, &selector)) continue;
        g_autofree char *want = row_key_for(base_key, selector);
        if (stored && strcmp(stored, want) == 0) found = g_strdup(stored);
    }
    sqlite3_finalize(st);
    return found;
}

void
ns_cache_init(void)
{
    const ns_config *c = ns_config_get();
    if (c && (!c->cache_enabled || c->private_mode)) {
        g_cache_disabled = TRUE;
        return;
    }
    const char *base = g_get_user_cache_dir();
    g_cache_dir = g_build_filename(base, NS_APP_DIR_NAME, "cache", NULL);
    g_mkdir_with_parents(g_cache_dir, 0700);
    restrict_to_owner(g_cache_dir, TRUE);

    char *db_path = g_build_filename(g_cache_dir, "http-cache.sqlite", NULL);
    g_cache_db = ns_db_open(db_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            "cache");
    if (!g_cache_db) {
        g_free(db_path);
        g_cache_disabled = TRUE;
        return;
    }
    cache_drop_if_legacy();
    if (!cache_schema()) {
        sqlite3_close(g_cache_db);
        g_cache_db = NULL;
        g_free(db_path);
        g_cache_disabled = TRUE;
        return;
    }
    restrict_to_owner(db_path, FALSE);
    g_free(db_path);
    evict_to_cap();
}

void
ns_cache_shutdown(void)
{
    G_GNUC_UNUSED g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&g_cache_mutex);
    if (g_cache_db) {
        sqlite3_close(g_cache_db);
        g_cache_db = NULL;
    }
    g_clear_pointer(&g_cache_dir, g_free);
}

static void
cache_rmrf(const char *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            char *child = g_build_filename(path, name, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
                !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
                cache_rmrf(child);
            else
                g_unlink(child);
            g_free(child);
        }
        g_dir_close(dir);
    }
    g_rmdir(path);
}

void
ns_cache_clear(void)
{
    G_GNUC_UNUSED g_autoptr(GMutexLocker) locker =
        g_mutex_locker_new(&g_cache_mutex);
    if (g_cache_db)
        cache_exec("DELETE FROM entries");
    if (g_cache_dir) {
        char *bodies = g_build_filename(g_cache_dir, "bodies", NULL);
        cache_rmrf(bodies);
        g_free(bodies);
    }
}

static gboolean
ns_cache_enabled(void)
{
    return !g_cache_disabled && g_cache_db != NULL;
}

void
ns_cache_entry_free(ns_cache_entry *e)
{
    if (!e) return;
    g_free(e->final_url);
    g_free(e->content_type);
    g_free(e->cors_allow_origin);
    g_free(e->policy_headers);
    g_free(e->etag);
    g_free(e->last_modified);
    if (e->body) g_byte_array_unref(e->body);
    g_free(e);
}

static int
month_from_name(const char *m)
{
    static const char *const months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    for (int i = 0; i < 12; i++)
        if (g_ascii_strncasecmp(m, months[i], 3) == 0) return i + 1;
    return 0;
}

static gint64
parse_http_date(const char *s)
{
    if (!s || !*s) return 0;
    GDateTime *dt = g_date_time_new_from_iso8601(s, NULL);
    if (dt) {
        gint64 r = g_date_time_to_unix(dt);
        g_date_time_unref(dt);
        return r;
    }
    const char *comma = strchr(s, ',');
    const char *p = comma ? comma + 1 : s;
    while (*p == ' ') p++;
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = {0};
    char sep = 0;
    if (sscanf(p, "%d %3s %d %d:%d:%d",
               &day, mon, &year, &hh, &mm, &ss) == 6) {
    } else if (sscanf(p, "%d%c%3s%c%d %d:%d:%d",
                      &day, &sep, mon, &sep, &year, &hh, &mm, &ss) == 8) {
        if (year < 100) year += (year < 70 ? 2000 : 1900);
    } else if (sscanf(p, "%3s %d %d:%d:%d %d",
                      mon, &day, &hh, &mm, &ss, &year) == 6) {
    } else {
        return 0;
    }
    int month = month_from_name(mon);
    if (!month) return 0;
    if (year < 1970 || year > 9999 || day < 1 || day > 31 ||
        hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60)
        return 0;
    GTimeZone *utc = g_time_zone_new_utc();
    dt = g_date_time_new(utc, year, month, day, hh, mm, (double)ss);
    g_time_zone_unref(utc);
    if (!dt) return 0;
    gint64 r = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return r;
}

static gboolean
cache_control_directive(const char *cache_control, const char *name,
                        gint64 *number)
{
    if (!cache_control) return FALSE;
    size_t name_len = strlen(name);
    const char *p = cache_control;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        size_t klen = eq ? (size_t)(eq - p) : len;
        while (klen && (p[klen - 1] == ' ' || p[klen - 1] == '\t')) klen--;
        if (klen == name_len && g_ascii_strncasecmp(p, name, name_len) == 0) {
            if (number) {
                *number = -1;
                if (eq) {
                    const char *v = eq + 1;
                    while (*v == ' ' || *v == '\t' || *v == '"') v++;
                    char *stop = NULL;
                    gint64 n = g_ascii_strtoll(v, &stop, 10);
                    if (stop != v && n >= 0) *number = n;
                }
            }
            return TRUE;
        }
        p += len;
    }
    return FALSE;
}

static gboolean
heuristic_freshness_allowed(long status)
{
    return status == 200 || status == 203 || status == 204 ||
           status == 301 || status == 308 || status == 404 ||
           status == 405 || status == 410 || status == 414 ||
           status == 501;
}

static gint64
freshness_lifetime(const char *cache_control, const char *expires_header,
                   gint64 date, const char *last_modified, long status)
{
    gint64 max_age = -1;
    if (cache_control_directive(cache_control, "max-age", &max_age) &&
        max_age >= 0)
        return MIN(max_age, 86400LL * 3650);
    if (cache_control_directive(cache_control, "immutable", NULL))
        return 86400LL * 30;
    if (expires_header) {
        gint64 expires = parse_http_date(expires_header);
        return expires > date ? expires - date : 0;
    }
    if (!heuristic_freshness_allowed(status) || !last_modified) return 0;
    gint64 modified = parse_http_date(last_modified);
    if (modified <= 0 || modified >= date) return 0;
    return MIN((date - modified) / 10, NS_CACHE_HEURISTIC_MAX_SECONDS);
}

static gint64
stale_until_from_headers(const char *cache_control, gint64 expires_at)
{
    gint64 window = -1;
    if (expires_at <= 0 ||
        cache_control_directive(cache_control, "must-revalidate", NULL) ||
        !cache_control_directive(cache_control, "stale-while-revalidate",
                                 &window) ||
        window <= 0)
        return 0;
    return expires_at + MIN(window, NS_CACHE_STALE_WHILE_REVALIDATE_MAX_SECONDS);
}

static gint64
freshness_from_headers(const char *cache_control, const char *expires_header,
                       const char *date_header, const char *age_header,
                       const char *last_modified, long status)
{
    if (cache_control_directive(cache_control, "no-store", NULL)) return -1;
    if (cache_control_directive(cache_control, "no-cache", NULL)) return 0;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gint64 date = date_header ? parse_http_date(date_header) : 0;
    if (date <= 0) date = now;
    gint64 lifetime = freshness_lifetime(cache_control, expires_header, date,
                                        last_modified, status);
    if (lifetime <= 0) return 0;
    gint64 age = 0;
    if (age_header) {
        char *stop = NULL;
        gint64 declared = g_ascii_strtoll(age_header, &stop, 10);
        if (stop != age_header && declared > 0) age = declared;
    }
    if (now > date) age = MAX(age, now - date);
    return age < lifetime ? now + lifetime - age : 0;
}

static gboolean
is_cacheable_status(long s)
{
    return s == 200 || s == 203 || s == 301 || s == 410;
}

static gint64
now_seconds(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

static char *
column_dup(sqlite3_stmt *st, int col)
{
    const unsigned char *t = sqlite3_column_text(st, col);
    return t ? g_strdup((const char *)t) : NULL;
}

static void
delete_key(const char *key)
{
    char *bp = body_path_for_key(key, FALSE);
    if (bp) g_unlink(bp);
    g_free(bp);
    if (!g_cache_db) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_cache_db, "DELETE FROM entries WHERE key=?",
                           -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static void
touch_key(const char *key)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_cache_db,
            "UPDATE entries SET last_used=? WHERE key=?",
            -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, now_seconds());
    sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

ns_cache_entry *
ns_cache_get(const char *url, const char *partition,
             const char *const *request_headers)
{
    G_GNUC_UNUSED g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&g_cache_mutex);
    if (!ns_cache_enabled() || !url) return NULL;
    g_autofree char *base = base_key_for_url(url, partition);
    g_autofree char *key = find_row_key(base, request_headers);
    if (!key) return NULL;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_cache_db,
            "SELECT final_url,status,content_type,cors_allow_origin,"
            "etag,last_modified,expires_at,fetched_at,body_size,"
            "policy_headers,stale_until FROM entries WHERE key=?",
            -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return NULL;
    }

    long status = (long)sqlite3_column_int64(st, 1);
    if (!is_cacheable_status(status)) {
        sqlite3_finalize(st);
        delete_key(key);
        return NULL;
    }
    gint64 body_size = sqlite3_column_int64(st, 8);
    if (body_size < 0 || (guint64)body_size > cache_cap_bytes()) {
        sqlite3_finalize(st);
        return NULL;
    }

    ns_cache_entry *e = g_new0(ns_cache_entry, 1);
    e->final_url        = column_dup(st, 0);
    e->status           = status;
    e->content_type     = column_dup(st, 2);
    e->cors_allow_origin = column_dup(st, 3);
    e->etag             = column_dup(st, 4);
    e->last_modified    = column_dup(st, 5);
    e->expires_at       = sqlite3_column_int64(st, 6);
    e->fetched_at       = sqlite3_column_int64(st, 7);
    e->policy_headers   = column_dup(st, 9);
    e->stale_until      = sqlite3_column_int64(st, 10);
    if (!e->final_url) e->final_url = g_strdup(url);
    sqlite3_finalize(st);

    g_autofree char *bp = body_path_for_key(key, FALSE);
    char *data = NULL;
    gsize dlen = 0;
    GStatBuf stbuf;
    if (!bp || g_stat(bp, &stbuf) != 0 || stbuf.st_size < 0 ||
        (guint64)stbuf.st_size > cache_cap_bytes()) {
        ns_cache_entry_free(e);
        delete_key(key);
        return NULL;
    }
    if (!g_file_get_contents(bp, &data, &dlen, NULL)) {
        ns_cache_entry_free(e);
        delete_key(key);
        return NULL;
    }
    if (dlen > G_MAXUINT || dlen > cache_cap_bytes()) {
        g_free(data);
        ns_cache_entry_free(e);
        delete_key(key);
        return NULL;
    }
    e->body = g_byte_array_new();
    if (dlen > 0)
        g_byte_array_append(e->body, (const guint8 *)data, (guint)dlen);
    g_free(data);

    touch_key(key);
    return e;
}

gboolean
ns_cache_is_fresh(const ns_cache_entry *e)
{
    if (!e) return FALSE;
    return e->expires_at > now_seconds();
}

gboolean
ns_cache_may_serve_stale(const ns_cache_entry *e)
{
    if (!e) return FALSE;
    gint64 now = now_seconds();
    return e->expires_at <= now && e->stale_until > now;
}

static gboolean
url_should_cache(const char *url)
{
    if (!url) return FALSE;
    if (!ns_url_is_http_or_https(url)) return FALSE;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p < 0x20 || *p == 0x7F) return FALSE;
    return TRUE;
}

void
ns_cache_put(const char *url,
             const char *partition,
             const char *final_url,
             long status,
             const char *content_type,
             const char *cors_allow_origin,
             const char *policy_headers,
             const char *etag,
             const char *last_modified,
             const char *cache_control,
             const char *expires_header,
             const char *date_header,
             const char *age_header,
             const char *vary,
             const char *const *request_headers,
             const void *body, gsize body_len)
{
    G_GNUC_UNUSED g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&g_cache_mutex);
    if (!ns_cache_enabled() || !url_should_cache(url)) return;
    if (!is_cacheable_status(status)) return;
    if (body_len > G_MAXINT || (guint64)body_len > cache_cap_bytes()) return;
    gint64 expires_at = freshness_from_headers(cache_control, expires_header,
                                               date_header, age_header,
                                               last_modified, status);
    if (expires_at < 0) return;
    gint64 stale_until = stale_until_from_headers(cache_control, expires_at);
    g_autofree char *selector = NULL;
    if (!vary_selector(vary, request_headers, &selector)) return;
    g_autofree char *base = base_key_for_url(url, partition);
    g_autofree char *key = row_key_for(base, selector);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_cache_db,
            "INSERT INTO entries(key,base_key,url,final_url,status,content_type,"
            "cors_allow_origin,etag,last_modified,vary,expires_at,fetched_at,"
            "last_used,body_size,policy_headers,stale_until) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(key) DO UPDATE SET "
            "url=excluded.url,final_url=excluded.final_url,status=excluded.status,"
            "content_type=excluded.content_type,"
            "cors_allow_origin=excluded.cors_allow_origin,etag=excluded.etag,"
            "last_modified=excluded.last_modified,vary=excluded.vary,"
            "expires_at=excluded.expires_at,"
            "fetched_at=excluded.fetched_at,last_used=excluded.last_used,"
            "body_size=excluded.body_size,"
            "policy_headers=excluded.policy_headers,"
            "stale_until=excluded.stale_until",
            -1, &st, NULL) != SQLITE_OK)
        return;
    gint64 now = now_seconds();
    sqlite3_bind_text (st, 1,  key,              -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2,  base,             -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3,  url,              -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 4,  final_url ? final_url : url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5,  status);
    sqlite3_bind_text (st, 6,  content_type,     -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 7,  cors_allow_origin,-1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 8,  etag,             -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 9,  last_modified,    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 10, vary,             -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 11, expires_at);
    sqlite3_bind_int64(st, 12, now);
    sqlite3_bind_int64(st, 13, now);
    sqlite3_bind_int64(st, 14, (gint64)body_len);
    sqlite3_bind_text (st, 15, policy_headers,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 16, stale_until);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return;

    g_autofree char *bp = body_path_for_key(key, TRUE);
    if (!bp) return;
    GError *err = NULL;
    if (!g_file_set_contents_full(bp, body ? body : "", (gssize)body_len,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, &err)) {
        g_warning("cache: failed to write %s: %s", bp, err->message);
        g_clear_error(&err);
        delete_key(key);
        return;
    }
    evict_to_cap();
}

void
ns_cache_promote_304(const char *url,
                     const char *partition,
                     const char *const *request_headers,
                     const char *cache_control,
                     const char *expires_header,
                     const char *date_header,
                     const char *age_header,
                     const char *last_modified)
{
    G_GNUC_UNUSED g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&g_cache_mutex);
    if (!ns_cache_enabled() || !url_should_cache(url)) return;
    g_autofree char *base = base_key_for_url(url, partition);
    g_autofree char *key = find_row_key(base, request_headers);
    if (!key) return;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_cache_db,
            "SELECT status,last_modified FROM entries WHERE key=?",
            -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return;
    }
    long status = (long)sqlite3_column_int64(st, 0);
    g_autofree char *stored_last_modified = column_dup(st, 1);
    sqlite3_finalize(st);

    const char *effective_last_modified =
        last_modified ? last_modified : stored_last_modified;
    gint64 expires_at = freshness_from_headers(cache_control, expires_header,
                                               date_header, age_header,
                                               effective_last_modified, status);
    if (expires_at < 0) {
        delete_key(key);
        return;
    }
    gint64 stale_until = stale_until_from_headers(cache_control, expires_at);

    if (sqlite3_prepare_v2(g_cache_db,
            "UPDATE entries SET expires_at=?,stale_until=?,fetched_at=?,"
            "last_used=?,last_modified=COALESCE(?,last_modified) WHERE key=?",
            -1, &st, NULL) != SQLITE_OK)
        return;
    gint64 now = now_seconds();
    sqlite3_bind_int64(st, 1, expires_at);
    sqlite3_bind_int64(st, 2, stale_until);
    sqlite3_bind_int64(st, 3, now);
    sqlite3_bind_int64(st, 4, now);
    sqlite3_bind_text (st, 5, last_modified, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 6, key, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static void
evict_by_select(const char *sql, gboolean has_bind, gint64 bind_value)
{
    if (!g_cache_db) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_cache_db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    if (has_bind) sqlite3_bind_int64(st, 1, bind_value);
    GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *k = sqlite3_column_text(st, 0);
        if (k) g_ptr_array_add(keys, g_strdup((const char *)k));
    }
    sqlite3_finalize(st);
    for (guint i = 0; i < keys->len; i++)
        delete_key(g_ptr_array_index(keys, i));
    g_ptr_array_free(keys, TRUE);
}

static void
evict_aged_out(void)
{
    evict_by_select("SELECT key FROM entries WHERE last_used < ?",
                    TRUE, now_seconds() - NS_CACHE_MAX_AGE_SECONDS);
}

static void
evict_to_cap(void)
{
    if (!g_cache_db) return;
    evict_aged_out();
    evict_by_select(
        "SELECT key FROM ("
        "SELECT key, SUM(body_size) OVER "
        "(ORDER BY last_used DESC, key DESC) AS running FROM entries"
        ") WHERE running > ?",
        TRUE, (gint64)cache_cap_bytes());
}
