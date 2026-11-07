/* Nordstjernen — SQLite-backed browsing history.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "history.h"
#include "about_style.h"
#include "config.h"
#include "db.h"

#include <glib/gstdio.h>
#include <libpsl.h>
#include <sqlite3.h>
#include <string.h>

#define NS_HISTORY_MAX_ROWS  10000
#define NS_HISTORY_PAGE_ROWS 200

static sqlite3 *g_history_db;
static gboolean g_history_disabled;
static GMutex   g_history_mutex;

static gboolean
history_exec(const char *sql)
{
    return ns_db_exec(g_history_db, sql, "history");
}

static gboolean
history_is_recordable(const char *url)
{
    if (!url || !*url) return FALSE;
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://"))
        return FALSE;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p < 0x20 || *p == 0x7F) return FALSE;
    return TRUE;
}

static gboolean
history_schema(void)
{
    return history_exec("PRAGMA journal_mode=WAL") &&
           history_exec("PRAGMA synchronous=NORMAL") &&
           history_exec("CREATE TABLE IF NOT EXISTS visits("
                        "url TEXT PRIMARY KEY,"
                        "title TEXT,"
                        "visit_count INTEGER NOT NULL DEFAULT 1,"
                        "last_visit INTEGER NOT NULL)") &&
           history_exec("CREATE INDEX IF NOT EXISTS idx_visits_last "
                        "ON visits(last_visit)");
}

static void
history_prune(void)
{
    if (!g_history_db) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_history_db,
            "DELETE FROM visits WHERE url NOT IN ("
            "SELECT url FROM visits ORDER BY last_visit DESC LIMIT ?)",
            -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, NS_HISTORY_MAX_ROWS);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void
ns_history_init(void)
{
    g_mutex_lock(&g_history_mutex);
    if (g_history_db || g_history_disabled) {
        g_mutex_unlock(&g_history_mutex);
        return;
    }
    char *dir = g_build_filename(g_get_user_data_dir(), NS_APP_DIR_NAME, NULL);
    g_mkdir_with_parents(dir, 0700);
    g_chmod(dir, 0700);
    char *path = g_build_filename(dir, "history.sqlite", NULL);
    g_free(dir);

    g_history_db = ns_db_open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "history");
    if (!g_history_db) {
        g_history_disabled = TRUE;
        g_free(path);
        g_mutex_unlock(&g_history_mutex);
        return;
    }
    if (!history_schema()) {
        sqlite3_close(g_history_db);
        g_history_db = NULL;
        g_history_disabled = TRUE;
        g_free(path);
        g_mutex_unlock(&g_history_mutex);
        return;
    }
    g_chmod(path, 0600);
    g_free(path);
    history_prune();
    g_mutex_unlock(&g_history_mutex);
}

void
ns_history_shutdown(void)
{
    g_mutex_lock(&g_history_mutex);
    if (g_history_db) {
        sqlite3_close(g_history_db);
        g_history_db = NULL;
    }
    g_mutex_unlock(&g_history_mutex);
}

void
ns_history_record(const char *url, const char *title)
{
    if (!history_is_recordable(url)) return;
    g_mutex_lock(&g_history_mutex);
    if (!g_history_db) {
        g_mutex_unlock(&g_history_mutex);
        return;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_history_db,
            "INSERT INTO visits(url,title,visit_count,last_visit) "
            "VALUES(?,?,1,?) "
            "ON CONFLICT(url) DO UPDATE SET "
            "visit_count=visit_count+1,last_visit=excluded.last_visit,"
            "title=COALESCE(NULLIF(excluded.title,''),title)",
            -1, &st, NULL) != SQLITE_OK) {
        g_mutex_unlock(&g_history_mutex);
        return;
    }
    sqlite3_bind_text (st, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, (title && *title) ? title : NULL, -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, g_get_real_time() / G_USEC_PER_SEC);
    sqlite3_step(st);
    sqlite3_finalize(st);
    g_mutex_unlock(&g_history_mutex);
}

void
ns_history_clear(void)
{
    g_mutex_lock(&g_history_mutex);
    history_exec("DELETE FROM visits");
    g_mutex_unlock(&g_history_mutex);
}

static const char k_history_style[] =
    "<style>" NS_ABOUT_BASE_CSS
    ".wrap{max-width:860px;margin:0 auto;padding:36px 24px 56px}\n"
    ".top{display:flex;align-items:flex-start;justify-content:space-between;"
    "gap:16px;min-height:42px;margin:0 0 22px}\n"
    "h1{margin:0;font-size:28px;letter-spacing:-.02em;line-height:1.2}\n"
    ".tools{display:flex;align-items:center;gap:10px;flex:0 1 auto;"
    "margin-top:-4px}\n"
    ".tools .btn{flex:0 0 auto;white-space:nowrap}\n"
    ".filter{flex:0 1 300px;display:flex;align-items:center;gap:10px;"
    "height:42px;padding:0 16px;border-radius:999px;background:var(--card);"
    "border:1px solid var(--line);box-shadow:var(--shadow);"
    "color:var(--faint)}\n"
    ".filter:focus-within{border-color:var(--accent);"
    "box-shadow:0 0 0 3px var(--accent-soft)}\n"
    ".filter input{flex:1 1 auto;min-width:0;border:0;outline:0;"
    "background:transparent;color:var(--text);font:inherit;font-size:14px}\n"
    ".day{margin:0 0 30px}\n"
    ".day h2{margin:0 0 10px 20px;font-size:15px;font-weight:600;"
    "color:var(--text)}\n"
    ".day ul{list-style:none;margin:0;padding:6px;}\n"
    ".day li a{display:flex;align-items:center;gap:12px;padding:6px 14px;"
    "border-radius:12px;color:var(--text)}\n"
    ".day li a:hover{background:var(--field);text-decoration:none}\n"
    ".av{flex:0 0 auto;display:flex;align-items:center;"
    "justify-content:center;width:28px;height:28px;border-radius:9px;"
    "background:var(--accent-soft);color:var(--accent);font-weight:700;"
    "font-size:14px;text-transform:uppercase}\n"
    NS_ABOUT_TINT_CSS

    ".tx{flex:1 1 auto;min-width:0;display:flex;flex-direction:column}\n"
    ".t{font-size:14px;font-weight:500;white-space:nowrap;overflow:hidden;"
    "text-overflow:ellipsis}\n"
    ".u{font-size:12.5px;color:var(--muted);white-space:nowrap;"
    "overflow:hidden;text-overflow:ellipsis}\n"
    ".tm{flex:0 0 auto;font-size:12.5px;color:var(--muted);"
    "font-variant-numeric:tabular-nums}\n"
    ".empty{display:flex;flex-direction:column;align-items:center;"
    "padding:36px 24px 34px;text-align:center;color:var(--muted)}\n"
    ".empty .ic{display:flex;align-items:center;justify-content:center;"
    "width:48px;height:48px;margin-bottom:14px;border-radius:14px;"
    "background:var(--accent-soft);color:var(--accent)}\n"
    ".empty b{display:block;margin-bottom:4px;font-size:16px;"
    "color:var(--text)}\n"
    "[hidden]{display:none}\n"
    "</style>";

static const char k_history_script[] =
    "<script>\n"
    "var q=document.getElementById('hq');\n"
    "if(q)q.addEventListener('input',function(){\n"
    " var v=q.value.toLowerCase();\n"
    " var days=document.querySelectorAll('.day');\n"
    " for(var i=0;i<days.length;i++){var any=false;\n"
    "  var rows=days[i].querySelectorAll('li');\n"
    "  for(var j=0;j<rows.length;j++){\n"
    "   var hit=!v||rows[j].textContent.toLowerCase().indexOf(v)>=0;\n"
    "   rows[j].hidden=!hit;if(hit)any=true;}\n"
    "  days[i].hidden=!any;}});\n"
    "</script>";

static char *
history_day_label(GDateTime *when, GDateTime *now)
{
    int days = g_date_time_get_day_of_year(now) -
               g_date_time_get_day_of_year(when);
    gboolean same_year =
        g_date_time_get_year(now) == g_date_time_get_year(when);
    if (same_year && days == 0)
        return g_strdup("Today");
    if (same_year && days == 1)
        return g_strdup("Yesterday");
    static const char *const weekdays[] = {
        "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
        "Sunday" };
    static const char *const months[] = {
        "January", "February", "March", "April", "May", "June", "July",
        "August", "September", "October", "November", "December" };
    const char *weekday = weekdays[g_date_time_get_day_of_week(when) - 1];
    const char *month = months[g_date_time_get_month(when) - 1];
    int day = g_date_time_get_day_of_month(when);
    if (same_year)
        return g_strdup_printf("%s, %d %s", weekday, day, month);
    return g_strdup_printf("%s, %d %s %d", weekday, day, month,
                           g_date_time_get_year(when));
}

static char *
history_host(const char *url)
{
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    const char *host = uri ? g_uri_get_host(uri) : NULL;
    char *out = g_utf8_make_valid(host && *host ? host : url, -1);
    if (uri)
        g_uri_unref(uri);
    if (g_str_has_prefix(out, "www.")) {
        char *trimmed = g_strdup(out + 4);
        g_free(out);
        out = trimmed;
    }
    return out;
}

static char *
history_site(const char *host)
{
    char *lower = g_ascii_strdown(host, -1);
    const psl_ctx_t *psl = psl_builtin();
    const char *site = psl && !g_hostname_is_ip_address(lower)
        ? psl_registrable_domain(psl, lower) : NULL;
    char *out = g_strdup(site && *site ? site : lower);
    g_free(lower);
    return out;
}

static guint
history_initial(const char *host, char out[8])
{
    char *key = history_site(host);
    guint tint = g_str_hash(key) % 8;
    gunichar first = g_utf8_get_char_validated(key, -1);
    if (first != (gunichar)-1 && first != (gunichar)-2 &&
        g_unichar_isalnum(first))
        out[g_unichar_to_utf8(first, out)] = '\0';
    else
        g_strlcpy(out, "\xe2\x80\xa2", 8);
    g_free(key);
    return tint;
}

static void
history_switch_day(GString *s, char **open_day, const char *day)
{
    if (*open_day && strcmp(*open_day, day) == 0)
        return;
    if (*open_day)
        g_string_append(s, "</ul></section>");
    char *e_day = g_markup_escape_text(day, -1);
    g_string_append_printf(s,
        "<section class=\"day\"><h2>%s</h2><ul class=\"card\">", e_day);
    g_free(e_day);
    g_free(*open_day);
    *open_day = g_strdup(day);
}

static void
history_append_row(GString *s, const char *raw_url, const char *raw_title,
                   GDateTime *when)
{
    char *url = g_utf8_make_valid(raw_url, -1);
    char *title = (raw_title && *raw_title)
        ? g_utf8_make_valid(raw_title, -1) : NULL;
    char *host = history_host(url);
    char initial[8];
    guint tint = history_initial(host, initial);
    char *e_url   = g_markup_escape_text(url, -1);
    char *e_host  = g_markup_escape_text(host, -1);
    char *e_init  = g_markup_escape_text(initial, -1);
    char *e_title = g_markup_escape_text(title ? title : url, -1);
    char *clock   = when ? g_date_time_format(when, "%H:%M") : NULL;
    g_string_append_printf(s,
        "<li><a href=\"%s\"><span class=\"av h%u\">%s</span>"
        "<span class=\"tx\"><span class=\"t\">%s</span>"
        "<span class=\"u\">%s</span></span>"
        "<span class=\"tm\">%s</span></a></li>",
        e_url, tint, e_init, e_title, e_host, clock ? clock : "");
    g_free(clock);
    g_free(e_url);
    g_free(e_host);
    g_free(e_init);
    g_free(e_title);
    g_free(host);
    g_free(title);
    g_free(url);
}

static gboolean
history_append_visits(GString *s)
{
    gboolean have = FALSE;
    char *open_day = NULL;
    GDateTime *now = g_date_time_new_now_local();
    g_mutex_lock(&g_history_mutex);
    sqlite3_stmt *st = NULL;
    if (g_history_db &&
        sqlite3_prepare_v2(g_history_db,
            "SELECT url,title,last_visit FROM visits "
            "ORDER BY last_visit DESC LIMIT ?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, NS_HISTORY_PAGE_ROWS);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *url   = (const char *)sqlite3_column_text(st, 0);
            const char *title = (const char *)sqlite3_column_text(st, 1);
            if (!url) continue;
            have = TRUE;
            GDateTime *dt =
                g_date_time_new_from_unix_local(sqlite3_column_int64(st, 2));
            char *day = dt ? history_day_label(dt, now) : g_strdup("Earlier");
            history_switch_day(s, &open_day, day);
            history_append_row(s, url, title, dt);
            g_free(day);
            if (dt) g_date_time_unref(dt);
        }
        sqlite3_finalize(st);
    }
    g_mutex_unlock(&g_history_mutex);
    g_date_time_unref(now);
    if (open_day)
        g_string_append(s, "</ul></section>");
    g_free(open_day);
    return have;
}

static gboolean
history_label_fits(const char *s)
{
    static const char *const generic[] = { "Home", "Homepage", "Start",
                                           "Welcome" };
    if (!s || !*s || g_utf8_strlen(s, -1) > 18)
        return FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(generic); i++)
        if (g_ascii_strcasecmp(s, generic[i]) == 0)
            return FALSE;
    return TRUE;
}

static char *
history_site_label(const char *title, const char *fallback)
{
    static const char *const separators[] = {
        " - ", " | ", " \xe2\x80\x93 ", " \xe2\x80\x94 ", " \xc2\xb7 " };
    if (!title || !*title)
        return g_strdup(fallback);
    GString *joined = g_string_new(title);
    for (gsize i = 0; i < G_N_ELEMENTS(separators); i++)
        g_string_replace(joined, separators[i], "\x1f", 0);
    char **parts = g_strsplit(joined->str, "\x1f", -1);
    g_string_free(joined, TRUE);
    guint n = g_strv_length(parts);
    const char *pick = NULL;
    if (n > 1 && history_label_fits(g_strstrip(parts[n - 1])))
        pick = parts[n - 1];
    else if (history_label_fits(g_strstrip(parts[0])))
        pick = parts[0];
    char *out = g_strdup(pick ? pick : fallback);
    g_strfreev(parts);
    return out;
}

char *
ns_history_top_sites_html(int max_sites)
{
    GString *s = g_string_new(NULL);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    int count = 0;
    g_mutex_lock(&g_history_mutex);
    sqlite3_stmt *st = NULL;
    if (g_history_db &&
        sqlite3_prepare_v2(g_history_db,
            "SELECT url,title FROM visits WHERE url LIKE 'http%' "
            "ORDER BY visit_count DESC, last_visit DESC LIMIT 60",
            -1, &st, NULL) == SQLITE_OK) {
        while (count < max_sites && sqlite3_step(st) == SQLITE_ROW) {
            const char *raw_url = (const char *)sqlite3_column_text(st, 0);
            const char *raw_title = (const char *)sqlite3_column_text(st, 1);
            if (!raw_url)
                continue;
            char *url = g_utf8_make_valid(raw_url, -1);
            char *host = history_host(url);
            char initial[8];
            guint tint = history_initial(host, initial);
            char *site = history_site(host);
            if (g_hash_table_contains(seen, site)) {
                g_free(site);
                g_free(host);
                g_free(url);
                continue;
            }
            g_hash_table_add(seen, site);
            char *title = raw_title && *raw_title
                ? g_utf8_make_valid(raw_title, -1) : g_strdup(host);
            char *e_url = g_markup_escape_text(url, -1);
            char *e_init = g_markup_escape_text(initial, -1);
            char *e_title = g_markup_escape_text(title, -1);
            char *label = history_site_label(raw_title ? title : NULL, site);
            char *e_site = g_markup_escape_text(label, -1);
            g_free(label);
            if (count == 0)
                g_string_append(s, "<nav class=\"sites\" "
                                   "aria-label=\"Top sites\">");
            g_string_append_printf(s,
                "<a class=\"site\" href=\"%s\" title=\"%s\">"
                "<span class=\"av h%u\">%s</span>"
                "<span class=\"nm\">%s</span></a>",
                e_url, e_title, tint, e_init, e_site);
            count++;
            g_free(e_site);
            g_free(e_url);
            g_free(e_init);
            g_free(e_title);
            g_free(title);
            g_free(host);
            g_free(url);
        }
        sqlite3_finalize(st);
    }
    g_mutex_unlock(&g_history_mutex);
    g_hash_table_destroy(seen);
    if (count)
        g_string_append(s, "</nav>");
    return g_string_free(s, FALSE);
}

char *
ns_history_html_page(void)
{
    GString *s = g_string_new(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"color-scheme\" content=\"light dark\">"
        "<title>History</title>" NS_ABOUT_ICON_HISTORY);
    g_string_append(s, k_history_style);
    g_string_append(s,
        "</head><body><main class=\"wrap\">"
        "<div class=\"top\"><h1>History</h1><div class=\"tools\" id=\"tools\">"
        "<label class=\"filter\">"
        "<svg width=\"16\" height=\"16\" viewBox=\"0 0 16 16\" fill=\"none\""
        " stroke=\"currentColor\" stroke-width=\"1.6\""
        " stroke-linecap=\"round\"><circle cx=\"7\" cy=\"7\" r=\"4.6\"/>"
        "<path d=\"M10.4 10.4 14 14\"/></svg>"
        "<input id=\"hq\" type=\"search\" placeholder=\"Search history\""
        " aria-label=\"Search history\" autocomplete=\"off\"></label>"
        "<a class=\"btn\" href=\"about:settings#data\">"
        "Clear browsing data\xe2\x80\xa6</a></div></div>");
    if (!history_append_visits(s))
        g_string_append(s, "<style>#tools{display:none}</style>"
            "<div class=\"card empty\"><span class=\"ic\">"
            "<svg width=\"22\" height=\"22\" viewBox=\"0 0 16 16\""
            " fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.5\""
            " stroke-linecap=\"round\" stroke-linejoin=\"round\">"
            "<circle cx=\"8\" cy=\"8\" r=\"5.75\"/>"
            "<path d=\"M8 4.75V8l2.25 1.5\"/></svg></span>"
            "<b>No history yet</b>"
            "Pages you visit will show up here.</div>");
    g_string_append(s, "</main>");
    g_string_append(s, k_history_script);
    g_string_append(s, "</body></html>");
    return g_string_free(s, FALSE);
}
