/* Nordstjernen — PDF documents shown as an inline HTML page holding a sandboxed PDF.js viewer.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "pdf.h"
#include "html.h"
#include "net.h"
#include "security.h"

#include <string.h>

#ifdef NS_HAVE_PDFJS
#include "pdf_viewer_js.h"
#include "pdf_viewer_css.h"

#define NS_PDF_VIEWER_MAX_BYTES ((gsize)64u * 1024u * 1024u)

static const char *const ns_pdfjs_required[] = {
    "pdf.min.mjs", "pdf.worker.min.mjs", "openjpeg.wasm", "jbig2.wasm",
    "qcms_bg.wasm",
};

static const char *const ns_pdfjs_data_dirs[] = { "cmaps/", "standard_fonts/" };

static char       *g_pdfjs_dir;
static gsize       g_pdfjs_once;
static GMutex      g_pdfjs_lock;
static GHashTable *g_pdfjs_cache;

static gboolean
ns_pdfjs_dir_complete(const char *dir)
{
    for (gsize i = 0; i < G_N_ELEMENTS(ns_pdfjs_required); i++) {
        char *path = g_build_filename(dir, ns_pdfjs_required[i], NULL);
        gboolean present = g_file_test(path, G_FILE_TEST_IS_REGULAR);
        g_free(path);
        if (!present) return FALSE;
    }
    return TRUE;
}

static char *
ns_pdfjs_find_dir(void)
{
    const char *override = g_getenv("NS_PDFJS_DIR");
    if (override && *override && ns_pdfjs_dir_complete(override))
        return g_strdup(override);
    static const char *const exe_relative[] = {
        "../Resources/share/nordstjernen/pdfjs",
        "../share/nordstjernen/pdfjs",
        "share/nordstjernen/pdfjs",
        "pdfjs",
        "../pdfjs",
    };
    char *exe_dir = ns_net_exe_dir();
    for (gsize i = 0; exe_dir && i < G_N_ELEMENTS(exe_relative); i++) {
        char *dir = g_build_filename(exe_dir, exe_relative[i], NULL);
        if (ns_pdfjs_dir_complete(dir)) {
            g_free(exe_dir);
            return dir;
        }
        g_free(dir);
    }
    g_free(exe_dir);
    const char *const *system_dirs = g_get_system_data_dirs();
    for (int i = 0; system_dirs && system_dirs[i]; i++) {
        char *dir = g_build_filename(system_dirs[i], "nordstjernen", "pdfjs",
                                     NULL);
        if (ns_pdfjs_dir_complete(dir)) return dir;
        g_free(dir);
    }
    return NULL;
}

static const char *
ns_pdfjs_dir(void)
{
    if (g_once_init_enter(&g_pdfjs_once)) {
        g_pdfjs_dir = ns_pdfjs_find_dir();
        g_once_init_leave(&g_pdfjs_once, 1);
    }
    return g_pdfjs_dir;
}

static gboolean
ns_pdfjs_is_required(const char *name)
{
    for (gsize i = 0; i < G_N_ELEMENTS(ns_pdfjs_required); i++)
        if (strcmp(name, ns_pdfjs_required[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean
ns_pdfjs_name_valid(const char *name)
{
    if (ns_pdfjs_is_required(name)) return TRUE;
    const char *file = NULL;
    for (gsize i = 0; !file && i < G_N_ELEMENTS(ns_pdfjs_data_dirs); i++)
        if (g_str_has_prefix(name, ns_pdfjs_data_dirs[i]))
            file = name + strlen(ns_pdfjs_data_dirs[i]);
    if (!file || !*file || *file == '.') return FALSE;
    for (const char *p = file; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_' && *p != '-')
            return FALSE;
    return TRUE;
}

GBytes *
ns_pdfjs_file(const char *name)
{
    const char *dir = ns_pdfjs_dir();
    if (!dir || !name || !ns_pdfjs_name_valid(name)) return NULL;
    gboolean cacheable = ns_pdfjs_is_required(name);
    g_mutex_lock(&g_pdfjs_lock);
    GBytes *bytes = g_pdfjs_cache ? g_hash_table_lookup(g_pdfjs_cache, name)
                                  : NULL;
    if (bytes) {
        g_bytes_ref(bytes);
        g_mutex_unlock(&g_pdfjs_lock);
        return bytes;
    }
    char *path = g_build_filename(dir, name, NULL);
    char *data = NULL;
    gsize len = 0;
    if (g_file_get_contents(path, &data, &len, NULL)) {
        bytes = g_bytes_new_take(data, len);
        if (cacheable) {
            if (!g_pdfjs_cache)
                g_pdfjs_cache = g_hash_table_new_full(
                    g_str_hash, g_str_equal, g_free,
                    (GDestroyNotify)g_bytes_unref);
            g_hash_table_insert(g_pdfjs_cache, g_strdup(name),
                                g_bytes_ref(bytes));
        }
    }
    g_free(path);
    g_mutex_unlock(&g_pdfjs_lock);
    return bytes;
}
#else
GBytes *
ns_pdfjs_file(const char *name)
{
    (void)name;
    return NULL;
}
#endif

static const char ns_pdf_style[] =
    "<style>"
    "html,body{margin:0;background:#1c1d1e}"
    ".doc{max-width:900px;margin:0 auto;padding:24px 16px}"
    ".note{color:#bbb;font:14px/1.6 system-ui,sans-serif;"
    "text-align:center;padding:48px 16px}"
    ".note a{color:#6ab7ff}"
    "</style>";

static char *
ns_pdf_notice_html(const char *url, const char *message)
{
    char *esc_url = ns_html_escape_text(url ? url : "");
    char *esc_msg = ns_html_escape_text(message ? message : "");
    char *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>PDF</title>%s</head><body><div class=\"doc\">"
        "<p class=\"note\">%s<br><a href=\"%s\" download>%s</a></p>"
        "</div></body></html>",
        ns_pdf_style, esc_msg, esc_url, esc_url);
    g_free(esc_url);
    g_free(esc_msg);
    return html;
}

#ifdef NS_HAVE_PDFJS
static char *
ns_pdf_title_html(const char *url)
{
    char *path = g_strndup(url ? url : "", strcspn(url ? url : "", "?#"));
    char *name = g_path_get_basename(path);
    char *readable = g_uri_unescape_string(name, NULL);
    const char *title = readable && g_utf8_validate(readable, -1, NULL)
        ? readable : name;
    char *escaped = ns_html_escape_text(*title && strcmp(title, ".") != 0
                                            ? title : "PDF");
    g_free(readable);
    g_free(name);
    g_free(path);
    return escaped;
}

static void
ns_pdf_append_base64_block(GString *out, const char *id, const void *data,
                           gsize len)
{
    char *b64 = g_base64_encode(data, len);
    g_string_append_printf(out, "<script type=\"text/plain\" id=\"%s\">", id);
    g_string_append(out, b64);
    g_string_append(out, "</script>");
    g_free(b64);
}

static char *
ns_pdf_script_nonce(void)
{
    guint8 raw[16];
    if (!ns_security_csprng_fill(raw, sizeof raw)) return NULL;
    return g_base64_encode(raw, sizeof raw);
}

static char *
ns_pdf_viewer_csp(const char *nonce)
{
    return g_strdup_printf(
        "sandbox allow-scripts allow-modals allow-popups "
        "allow-popups-to-escape-sandbox; "
        "default-src 'none'; "
        "script-src 'nonce-%s' 'wasm-unsafe-eval' blob: about:; "
        "worker-src blob:; style-src 'unsafe-inline'; "
        "img-src data: blob:; font-src data: blob:; "
        "form-action 'none'; base-uri 'none'", nonce);
}

static char *
ns_pdf_viewer_html(const guint8 *data, gsize len, const char *url,
                   const char *nonce)
{
    char *title = ns_pdf_title_html(url);
    char *href = ns_html_escape_text(url ? url : "");
    GString *out = g_string_sized_new(len / 3 * 4 + 64 * 1024);
    g_string_append_printf(out,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>%s</title><style>%s</style></head><body>"
        "<div id=\"toolbar\" hidden>"
        "<span id=\"page-number\"></span>"
        "<button id=\"zoom-out\" title=\"Zoom out\">&minus;</button>"
        "<span id=\"zoom-level\"></span>"
        "<button id=\"zoom-in\" title=\"Zoom in\">+</button>"
        "<button id=\"zoom-fit\" title=\"Fit to width\">Fit</button>"
        "<input id=\"find\" type=\"search\" placeholder=\"Find\" "
        "autocomplete=\"off\"><button id=\"find-next\" "
        "title=\"Find next\">&darr;</button><span id=\"find-status\"></span>"
        "<button id=\"print\" title=\"Print every page\">Print</button>"
        "<a id=\"save\" href=\"%s\" download>Download</a></div>"
        "<p id=\"status\" hidden></p>"
        "<div id=\"password\" hidden>"
        "<input id=\"password-input\" type=\"password\" "
        "autocomplete=\"off\"><button id=\"password-ok\">Open</button></div>"
        "<div id=\"pages\"></div><div id=\"print-pages\"></div>",
        title, ns_pdf_viewer_css, href);
    g_free(title);
    g_free(href);
    ns_pdf_append_base64_block(out, "pdf-data", data, len);
    g_string_append_printf(out, "<script nonce=\"%s\">", nonce);
    g_string_append(out, ns_pdf_viewer_js);
    g_string_append(out, "</script></body></html>");
    return g_string_free(out, FALSE);
}
#endif

char *
ns_pdf_document_html(const guint8 *data, gsize len, const char *url,
                     char **csp_out)
{
    *csp_out = g_strdup("sandbox allow-downloads; default-src 'none'; "
                        "style-src 'unsafe-inline'");
#ifdef NS_HAVE_PDFJS
    if (!ns_pdfjs_dir())
        return ns_pdf_notice_html(
            url, "The PDF viewer (PDF.js) is not installed.");
    char *nonce = data && len > 0 && len <= NS_PDF_VIEWER_MAX_BYTES
        ? ns_pdf_script_nonce() : NULL;
    if (nonce) {
        g_free(*csp_out);
        *csp_out = ns_pdf_viewer_csp(nonce);
        char *html = ns_pdf_viewer_html(data, len, url, nonce);
        g_free(nonce);
        return html;
    }
    return ns_pdf_notice_html(
        url, data && len > NS_PDF_VIEWER_MAX_BYTES
            ? "This PDF is too large to show in the built-in viewer."
            : "This document could not be opened.");
#else
    (void)data; (void)len;
    return ns_pdf_notice_html(
        url, "Inline PDF viewing is not available in this build.");
#endif
}
