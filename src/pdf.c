/* Nordstjernen — PDF documents shown as an inline HTML page holding a sandboxed PDF.js viewer.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "pdf.h"
#include "html.h"

#include <string.h>

#ifdef NS_HAVE_PDFJS
#include "pdfjs_lib.h"
#include "pdfjs_worker.h"
#include "pdfjs_openjpeg.h"
#include "pdfjs_jbig2.h"
#include "pdfjs_qcms.h"
#include "pdf_viewer_js.h"
#include "pdf_viewer_css.h"

#define NS_PDF_VIEWER_MAX_BYTES ((gsize)64u * 1024u * 1024u)
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
        "<p class=\"note\">%s<br><a href=\"%s\">%s</a></p>"
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
    char *name = url ? g_path_get_basename(url) : NULL;
    if (name) {
        char *query = strchr(name, '?');
        if (query) *query = '\0';
    }
    char *escaped = ns_html_escape_text(name && *name ? name : "PDF");
    g_free(name);
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
ns_pdf_viewer_html(const guint8 *data, gsize len, const char *url)
{
    char *title = ns_pdf_title_html(url);
    GString *out = g_string_sized_new(len / 3 * 4 + sizeof(ns_pdfjs_lib) +
                                      sizeof(ns_pdfjs_worker) + 1024 * 1024);
    g_string_append_printf(out,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>%s</title><style>%s</style></head><body>"
        "<p id=\"status\" hidden></p><div id=\"pages\"></div>",
        title, ns_pdf_viewer_css);
    g_free(title);
    ns_pdf_append_base64_block(out, "pdf-data", data, len);
    ns_pdf_append_base64_block(out, "wasm:openjpeg.wasm", ns_pdfjs_openjpeg,
                               sizeof(ns_pdfjs_openjpeg) - 1);
    ns_pdf_append_base64_block(out, "wasm:jbig2.wasm", ns_pdfjs_jbig2,
                               sizeof(ns_pdfjs_jbig2) - 1);
    ns_pdf_append_base64_block(out, "wasm:qcms_bg.wasm", ns_pdfjs_qcms,
                               sizeof(ns_pdfjs_qcms) - 1);
    ns_pdf_append_text_block(out, "pdfjs-lib", ns_pdfjs_lib);
    ns_pdf_append_text_block(out, "pdfjs-worker", ns_pdfjs_worker);
    g_string_append_printf(out, "<script>%s</script></body></html>",
                           ns_pdf_viewer_js);
    return g_string_free(out, FALSE);
}
#endif

const char *
ns_pdf_document_csp(void)
{
#ifdef NS_HAVE_PDFJS
    return "sandbox allow-scripts allow-popups "
           "allow-top-navigation-by-user-activation; default-src 'none'; "
           "script-src 'unsafe-inline' 'wasm-unsafe-eval' blob:; "
           "worker-src blob:; style-src 'unsafe-inline'; "
           "img-src data: blob:; font-src data: blob:";
#else
    return NULL;
#endif
}

char *
ns_pdf_document_html(const guint8 *data, gsize len, const char *url)
{
#ifdef NS_HAVE_PDFJS
    if (data && len > 0 && len <= NS_PDF_VIEWER_MAX_BYTES)
        return ns_pdf_viewer_html(data, len, url);
    return ns_pdf_notice_html(
        url, data && len > 0
            ? "This PDF is too large to show in the built-in viewer."
            : "This document could not be opened.");
#else
    (void)data; (void)len;
    return ns_pdf_notice_html(
        url, "Inline PDF viewing is not available in this build.");
#endif
}
