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

typedef enum {
    NS_PDFJS_LIB,
    NS_PDFJS_WORKER,
    NS_PDFJS_OPENJPEG,
    NS_PDFJS_JBIG2,
    NS_PDFJS_QCMS,
    NS_PDFJS_FILE_COUNT,
} ns_pdfjs_file_id;

static const char *const ns_pdfjs_names[NS_PDFJS_FILE_COUNT] = {
    "pdf.min.mjs", "pdf.worker.min.mjs", "openjpeg.wasm", "jbig2.wasm",
    "qcms_bg.wasm",
};

typedef struct {
    char  *data;
    gsize  len;
} ns_pdfjs_file;

static ns_pdfjs_file g_pdfjs[NS_PDFJS_FILE_COUNT];
static gsize         g_pdfjs_once;
static gboolean      g_pdfjs_ready;

static gboolean
ns_pdfjs_closes_script(const char *text)
{
    char *lower = g_ascii_strdown(text, -1);
    gboolean closes = strstr(lower, "</script") != NULL;
    g_free(lower);
    return closes;
}

static gboolean
ns_pdfjs_load_from(const char *dir)
{
    ns_pdfjs_file files[NS_PDFJS_FILE_COUNT] = {{0}};
    gboolean ok = TRUE;
    for (int i = 0; ok && i < NS_PDFJS_FILE_COUNT; i++) {
        char *path = g_build_filename(dir, ns_pdfjs_names[i], NULL);
        ok = g_file_get_contents(path, &files[i].data, &files[i].len, NULL);
        g_free(path);
    }
    if (ok)
        ok = !ns_pdfjs_closes_script(files[NS_PDFJS_LIB].data) &&
             !ns_pdfjs_closes_script(files[NS_PDFJS_WORKER].data);
    if (!ok) {
        for (int i = 0; i < NS_PDFJS_FILE_COUNT; i++)
            g_free(files[i].data);
        return FALSE;
    }
    memcpy(g_pdfjs, files, sizeof files);
    return TRUE;
}

static gboolean
ns_pdfjs_find(void)
{
    const char *override = g_getenv("NS_PDFJS_DIR");
    if (override && *override && ns_pdfjs_load_from(override))
        return TRUE;
    static const char *const exe_relative[] = {
        "../Resources/share/nordstjernen/pdfjs",
        "../share/nordstjernen/pdfjs",
        "share/nordstjernen/pdfjs",
        "pdfjs",
        "../pdfjs",
    };
    char *exe_dir = ns_net_exe_dir();
    if (exe_dir) {
        gboolean found = FALSE;
        for (gsize i = 0; !found && i < G_N_ELEMENTS(exe_relative); i++) {
            char *dir = g_build_filename(exe_dir, exe_relative[i], NULL);
            found = ns_pdfjs_load_from(dir);
            g_free(dir);
        }
        g_free(exe_dir);
        if (found) return TRUE;
    }
    const char *const *system_dirs = g_get_system_data_dirs();
    for (int i = 0; system_dirs && system_dirs[i]; i++) {
        char *dir = g_build_filename(system_dirs[i], "nordstjernen", "pdfjs",
                                     NULL);
        gboolean found = ns_pdfjs_load_from(dir);
        g_free(dir);
        if (found) return TRUE;
    }
    return FALSE;
}

static gboolean
ns_pdfjs_available(void)
{
    if (g_once_init_enter(&g_pdfjs_once)) {
        g_pdfjs_ready = ns_pdfjs_find();
        g_once_init_leave(&g_pdfjs_once, 1);
    }
    return g_pdfjs_ready;
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

static void
ns_pdf_append_text_block(GString *out, const char *id, const char *text)
{
    g_string_append_printf(out, "<script type=\"text/plain\" id=\"%s\">", id);
    g_string_append(out, text);
    g_string_append(out, "</script>");
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
        "default-src 'none'; script-src 'nonce-%s' 'wasm-unsafe-eval' blob:; "
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
    GString *out = g_string_sized_new(len / 3 * 4 +
                                      g_pdfjs[NS_PDFJS_LIB].len +
                                      g_pdfjs[NS_PDFJS_WORKER].len +
                                      1024 * 1024);
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
    for (int i = NS_PDFJS_OPENJPEG; i <= NS_PDFJS_QCMS; i++) {
        char *id = g_strconcat("wasm:", ns_pdfjs_names[i], NULL);
        ns_pdf_append_base64_block(out, id, g_pdfjs[i].data, g_pdfjs[i].len);
        g_free(id);
    }
    ns_pdf_append_text_block(out, "pdfjs-lib", g_pdfjs[NS_PDFJS_LIB].data);
    ns_pdf_append_text_block(out, "pdfjs-worker",
                             g_pdfjs[NS_PDFJS_WORKER].data);
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
    if (!ns_pdfjs_available())
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
