/* Nordstjernen — local phishing/malware blocklist and warning interstitial. */

#define _GNU_SOURCE 1
#include "safebrowsing.h"
#include "net.h"
#include "about_style.h"

#include <string.h>

#include <openssl/evp.h>

static GMutex      g_lock;
static GHashTable *g_blocked;
static GHashTable *g_allow;
static gboolean    g_loaded;

static gboolean ns_safebrowsing_blocked_locked(const char *host);

static char *
host_sha256_hex(const char *host)
{
    if (!host || !*host) return NULL;
    char *low = g_ascii_strdown(host, -1);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(low, strlen(low), digest, &len, EVP_sha256(), NULL) != 1) {
        g_free(low);
        return NULL;
    }
    g_free(low);
    GString *hex = g_string_sized_new(len * 2);
    for (unsigned int i = 0; i < len; i++)
        g_string_append_printf(hex, "%02x", digest[i]);
    return g_string_free(hex, FALSE);
}

static void
load_file(const char *path)
{
    char *contents = NULL;
    if (!path || !g_file_get_contents(path, &contents, NULL, NULL))
        return;
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines && lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (!*line || *line == '#')
            continue;
        char *end = line;
        while (*end && !g_ascii_isspace(*end))
            end++;
        *end = '\0';
        if (strlen(line) >= 16)
            g_hash_table_add(g_blocked, g_ascii_strdown(line, -1));
    }
    g_strfreev(lines);
    g_free(contents);
}

static gboolean
load_bundled(void)
{
    static const char *const rel[] = {
        "../Resources/share/nordstjernen/safebrowsing.list",
        "../share/nordstjernen/safebrowsing.list",
        "share/nordstjernen/safebrowsing.list",
        "data/safebrowsing.list",
        "../data/safebrowsing.list",
        "../../data/safebrowsing.list",
        "../../../data/safebrowsing.list",
    };
    char *dir = ns_net_exe_dir();
    if (dir) {
        for (gsize i = 0; i < G_N_ELEMENTS(rel); i++) {
            char *path = g_build_filename(dir, rel[i], NULL);
            gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS);
            if (exists) {
                load_file(path);
                g_free(path);
                g_free(dir);
                return TRUE;
            }
            g_free(path);
        }
        g_free(dir);
    }
    if (g_file_test("data/safebrowsing.list", G_FILE_TEST_EXISTS)) {
        load_file("data/safebrowsing.list");
        return TRUE;
    }
    return FALSE;
}

static void
ensure_loaded(void)
{
    if (g_loaded)
        return;
    g_loaded = TRUE;
    g_blocked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_allow = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    const char *override = g_getenv("NS_SAFEBROWSING_LIST");
    if (override && *override) {
        load_file(override);
        return;
    }
    char *user = g_build_filename(g_get_user_config_dir(), "nordstjernen",
                                  "safebrowsing.list", NULL);
    load_file(user);
    g_free(user);
    load_bundled();
}

void
ns_safebrowsing_allow_host(const char *host)
{
    if (!host || !*host)
        return;
    g_mutex_lock(&g_lock);
    ensure_loaded();
    g_hash_table_add(g_allow, g_ascii_strdown(host, -1));
    g_mutex_unlock(&g_lock);
}

gboolean
ns_safebrowsing_blocked(const char *host)
{
    if (!host || !*host)
        return FALSE;
    g_mutex_lock(&g_lock);
    gboolean hit = ns_safebrowsing_blocked_locked(host);
    g_mutex_unlock(&g_lock);
    return hit;
}

static gboolean
ns_safebrowsing_blocked_locked(const char *host)
{
    ensure_loaded();
    if (g_hash_table_size(g_blocked) == 0)
        return FALSE;
    char *low = g_ascii_strdown(host, -1);
    for (gsize ln = strlen(low); ln > 0 && low[ln - 1] == '.'; ln--)
        low[ln - 1] = '\0';
    if (g_hash_table_contains(g_allow, low)) {
        g_free(low);
        return FALSE;
    }
    gboolean hit = FALSE;
    for (const char *h = low; h && strchr(h, '.'); h = strchr(h, '.') + 1) {
        char *hex = host_sha256_hex(h);
        if (hex && g_hash_table_contains(g_blocked, hex))
            hit = TRUE;
        g_free(hex);
        if (hit)
            break;
    }
    g_free(low);
    return hit;
}

char *
ns_safebrowsing_interstitial(const char *url, const char *host)
{
    char *esc_host = g_markup_escape_text(host && *host ? host : "", -1);
    char *continue_href = g_strconcat(NS_UNSAFE_CONTINUE_SCHEME,
                                      url ? url : "", NULL);
    char *esc_continue = g_markup_escape_text(continue_href, -1);
    g_free(continue_href);

    char *html = g_strconcat(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"color-scheme\" content=\"light dark\">"
        "<title>Security warning</title>" NS_ABOUT_ICON_DANGER
        "<style>" NS_ABOUT_BASE_CSS
        "html,body{background:#a3201a}"
        "body{display:flex;align-items:center;justify-content:center;"
        "min-height:100vh;padding:32px 18px}"
        ".card{width:100%;max-width:600px;padding:36px 38px 30px}"
        ".btn.primary{background:var(--danger);color:#fff}"
        ".icon{display:flex;align-items:center;justify-content:center;"
        "width:64px;height:64px;margin-bottom:20px;border-radius:20px;"
        "background:rgba(198,47,58,.12);color:var(--danger)}"
        "h1{margin:0 0 10px;font-size:24px;letter-spacing:-.01em;"
        "line-height:1.25}"
        "p.summary{margin:0 0 20px;color:var(--muted);font-size:15.5px}"
        ".host{margin:0 0 22px;padding:10px 14px;border-radius:12px;"
        "background:rgba(198,47,58,.08);color:var(--danger);"
        "font-family:var(--mono);font-size:13px;font-weight:600;"
        "overflow-wrap:anywhere}"
        ".actions{display:flex;flex-wrap:wrap;gap:10px}"
        "details{margin-top:26px;padding-top:18px;"
        "border-top:1px solid var(--line);color:var(--muted);"
        "font-size:13.5px}"
        "details p{margin:8px 0 14px}"
        "@media (prefers-color-scheme:dark){html,body{background:#4a1210}"
        ".btn.primary{color:#2a0607}"
        ".icon{background:rgba(255,107,116,.14)}"
        ".host{background:rgba(255,107,116,.1)}"
        ".card{background:#241618;border-color:rgba(255,107,116,.18)}}"
        "</style></head><body><main class=\"card\">"
        "<div class=\"icon\"><svg width=\"30\" height=\"30\" "
        "viewBox=\"0 0 16 16\" fill=\"none\" stroke=\"currentColor\" "
        "stroke-width=\"1.4\" stroke-linecap=\"round\" "
        "stroke-linejoin=\"round\"><path d=\"M8 1.75 13.25 3.75v4c0 "
        "3.25-2.25 5.5-5.25 6.5-3-1-5.25-3.25-5.25-6.5v-4z\"/>"
        "<path d=\"M8 5.25v3.25M8 10.6v.1\"/></svg></div>"
        "<h1>Deceptive site ahead</h1>"
        "<p class=\"summary\">This site is on your list of known phishing "
        "and malware sites. It may try to steal passwords or payment details, "
        "or install harmful software.</p>"
        "<p class=\"host\">", esc_host, "</p>"
        "<div class=\"actions\">"
        "<a class=\"btn primary\" href=\"about:start\">Back to safety</a>"
        "</div>"
        "<details><summary>Details</summary>"
        "<p>This check runs on your device against a local list. Nothing "
        "about this page was sent anywhere.</p>"
        "<a class=\"btn danger\" href=\"", esc_continue,
        "\">Visit this unsafe site</a>"
        "</details>"
        "</main></body></html>", NULL);

    g_free(esc_host);
    g_free(esc_continue);
    return html;
}
