/* Nordstjernen — PDF documents shown as an inline HTML page: a sandboxed PDF.js viewer, or page images from poppler-glib.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "pdf.h"
#include "html.h"

#include <math.h>
#include <string.h>

#ifdef NS_HAVE_POPPLER
#include <cairo.h>
#include <poppler.h>
#endif

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

#define NS_PDF_TARGET_W   1240.0
#define NS_PDF_MIN_SCALE  0.5
#define NS_PDF_MAX_SCALE  4.0
#define NS_PDF_MAX_DIM    4000
#define NS_PDF_MAX_PAGES  300

static const char ns_pdf_style[] =
    "<style>"
    "html,body{margin:0;background:#1c1d1e}"
    ".doc{max-width:900px;margin:0 auto;padding:24px 16px}"
    ".page{background:#fff;margin:0 auto 24px;"
    "box-shadow:0 4px 12px rgba(0,0,0,.45);border:1px solid #444}"
    ".page:last-child{margin-bottom:0}"
    ".page img{display:block;width:100%;height:auto}"
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

#ifdef NS_HAVE_POPPLER
static cairo_status_t
ns_pdf_png_write(void *closure, const unsigned char *data, unsigned int length)
{
    g_byte_array_append((GByteArray *)closure, data, length);
    return CAIRO_STATUS_SUCCESS;
}

static char *
ns_pdf_page_data_uri(PopplerPage *page)
{
    double pw = 0, ph = 0;
    poppler_page_get_size(page, &pw, &ph);
    if (!isfinite(pw) || !isfinite(ph) || pw <= 0 || ph <= 0)
        return NULL;
    double scale = NS_PDF_TARGET_W / pw;
    scale = CLAMP(scale, NS_PDF_MIN_SCALE, NS_PDF_MAX_SCALE);
    int w = (int)CLAMP(pw * scale + 0.5, 1.0, (double)NS_PDF_MAX_DIM);
    int h = (int)CLAMP(ph * scale + 0.5, 1.0, (double)NS_PDF_MAX_DIM);

    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surf);

    GByteArray *png = g_byte_array_new();
    cairo_status_t st =
        cairo_surface_write_to_png_stream(surf, ns_pdf_png_write, png);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        g_byte_array_free(png, TRUE);
        return NULL;
    }
    char *b64 = g_base64_encode(png->data, png->len);
    g_byte_array_free(png, TRUE);
    char *uri = g_strconcat("data:image/png;base64,", b64, NULL);
    g_free(b64);
    return uri;
}
#endif

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

#ifdef NS_HAVE_PDFJS
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
#endif
#ifdef NS_HAVE_POPPLER
    if (!data || len == 0)
        return ns_pdf_notice_html(url, "This document could not be opened.");

    GBytes *bytes = g_bytes_new(data, len);
    GError *err = NULL;
    PopplerDocument *doc = poppler_document_new_from_bytes(bytes, NULL, &err);
    g_bytes_unref(bytes);
    if (!doc) {
        char *msg = g_strdup_printf("This PDF could not be displayed%s%s.",
                                    err ? ": " : "",
                                    err ? err->message : "");
        g_clear_error(&err);
        char *html = ns_pdf_notice_html(url, msg);
        g_free(msg);
        return html;
    }
    g_clear_error(&err);

    int n = poppler_document_get_n_pages(doc);
    if (n <= 0) {
        g_object_unref(doc);
        return ns_pdf_notice_html(url, "This PDF has no pages.");
    }

    char *esc_name = ns_pdf_title_html(url);

    GString *out = g_string_new(NULL);
    g_string_append_printf(out,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>%s</title>%s</head><body><div class=\"doc\">",
        esc_name, ns_pdf_style);
    g_free(esc_name);

    int limit = n < NS_PDF_MAX_PAGES ? n : NS_PDF_MAX_PAGES;
    for (int i = 0; i < limit; i++) {
        PopplerPage *page = poppler_document_get_page(doc, i);
        if (!page)
            continue;
        char *uri = ns_pdf_page_data_uri(page);
        g_object_unref(page);
        if (!uri)
            continue;
        g_string_append_printf(out,
            "<div class=\"page\"><img src=\"%s\" alt=\"Page %d\"></div>",
            uri, i + 1);
        g_free(uri);
    }
    if (limit < n)
        g_string_append_printf(out,
            "<p class=\"note\">Showing the first %d of %d pages.</p>",
            limit, n);

    g_string_append(out, "</div></body></html>");
    g_object_unref(doc);
    return g_string_free(out, FALSE);
#else
#ifdef NS_HAVE_PDFJS
    return ns_pdf_notice_html(
        url, data && len > 0
            ? "This PDF is too large to show in the built-in viewer."
            : "This document could not be opened.");
#else
    (void)data; (void)len;
    return ns_pdf_notice_html(
        url, "Inline PDF viewing is not available in this build.");
#endif
#endif
}
