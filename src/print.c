/* Nordstjernen — paginates a laid-out page onto sheets of paper.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "print.h"

#include <string.h>

#include "paint.h"

#define NS_PRINT_MAX_SPANS (1u << 20)

typedef struct { double top, bottom; } ns_print_span;

typedef struct {
    GArray *forced;
    GArray *spans;
    GArray *content;
    double page_h;
} ns_print_scan;

void
ns_print_setup_apply_page_rule(ns_print_setup *setup,
                               const ns_css_page_rule *rule)
{
    if (!rule) return;
    if (rule->has_size && rule->width > 1 && rule->height > 1) {
        setup->width = rule->width;
        setup->height = rule->height;
    }
    if (rule->has_margin[0]) setup->margin_top = rule->margin[0];
    if (rule->has_margin[1]) setup->margin_right = rule->margin[1];
    if (rule->has_margin[2]) setup->margin_bottom = rule->margin[2];
    if (rule->has_margin[3]) setup->margin_left = rule->margin[3];
}

static const char *
break_keyword(const ns_box *b, ns_css_prop prop)
{
    return b->style ? ns_style_keyword(b->style, prop) : NULL;
}

static gboolean
forces_page_break(const char *kw)
{
    return kw && (strcmp(kw, "page") == 0 || strcmp(kw, "always") == 0 ||
                  strcmp(kw, "all") == 0 || strcmp(kw, "left") == 0 ||
                  strcmp(kw, "right") == 0 || strcmp(kw, "recto") == 0 ||
                  strcmp(kw, "verso") == 0);
}

static gboolean
avoids_page_break(const char *kw)
{
    return kw && (strcmp(kw, "avoid") == 0 || strcmp(kw, "avoid-page") == 0);
}

static void
box_page_extent(const ns_box *b, double *top, double *bottom)
{
    *top = b->y + b->margin.top;
    *bottom = *top + b->content_height + b->padding.top + b->padding.bottom +
              b->border.top + b->border.bottom;
}

static void
add_span(GArray *spans, double top, double bottom, double page_h)
{
    if (!(bottom > top) || bottom - top > page_h) return;
    ns_print_span s = { top, bottom };
    g_array_append_val(spans, s);
}

static void
add_text_line_spans(GArray *spans, const ns_box *b, double top, double bottom,
                    double page_h)
{
    NsPangoLayout *layout =
        b->vertical_wm ? NULL : ns_paint_build_inline_layout(NULL, b);
    int lines = layout ? ns_pango_layout_get_line_count(layout) : 0;
    if (lines < 2) {
        g_clear_object(&layout);
        add_span(spans, top, bottom, page_h);
        return;
    }
    const GArray *heights = b->atomic_line_heights;
    if (heights && heights->len == (guint)lines) {
        double line_top = b->y;
        for (guint i = 0; i < heights->len && spans->len < NS_PRINT_MAX_SPANS;
             i++) {
            double line_h = g_array_index(heights, double, i);
            add_span(spans, line_top, line_top + line_h, page_h);
            line_top += line_h;
        }
        g_object_unref(layout);
        return;
    }
    double y_origin = b->y + ns_paint_inline_y_offset_for_layout(b, layout);
    NsPangoLayoutIter *it = ns_pango_layout_get_iter(layout);
    do {
        NsPangoRectangle line;
        ns_pango_layout_iter_get_line_extents(it, NULL, &line);
        add_span(spans, y_origin + (double)line.y / NS_PANGO_SCALE,
                 y_origin + (double)(line.y + line.height) / NS_PANGO_SCALE,
                 page_h);
    } while (spans->len < NS_PRINT_MAX_SPANS &&
             ns_pango_layout_iter_next_line(it));
    ns_pango_layout_iter_free(it);
    g_object_unref(layout);
}

static void
add_content(GArray *content, double top, double bottom)
{
    if (!(bottom > top) || content->len >= NS_PRINT_MAX_SPANS) return;
    ns_print_span s = { top, bottom };
    g_array_append_val(content, s);
}

static void
collect_breaks(const ns_box *b, ns_print_scan *scan)
{
    if (!b) return;
    double top, bottom;
    box_page_extent(b, &top, &bottom);

    if (b->parent) {
        if (forces_page_break(break_keyword(b, NS_CSS_BREAK_BEFORE)))
            g_array_append_val(scan->forced, top);
        if (forces_page_break(break_keyword(b, NS_CSS_BREAK_AFTER)))
            g_array_append_val(scan->forced, bottom);
        if (avoids_page_break(break_keyword(b, NS_CSS_BREAK_INSIDE))) {
            add_span(scan->spans, top, bottom, scan->page_h);
            add_content(scan->content, top, bottom);
            return;
        }
    }

    if (!b->first_child) {
        if (b->kind == NS_BOX_INLINE && b->text && *b->text)
            add_text_line_spans(scan->spans, b, top, bottom, scan->page_h);
        else
            add_span(scan->spans, top, bottom, scan->page_h);
        add_content(scan->content, top, bottom);
        return;
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        collect_breaks(c, scan);
}

static int
cmp_double(gconstpointer a, gconstpointer b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static gboolean
has_content_between(const GArray *content, double from, double to)
{
    for (guint i = 0; i < content->len; i++) {
        const ns_print_span *s = &g_array_index(content, ns_print_span, i);
        if (s->top < to - 0.5 && s->bottom > from + 0.5) return TRUE;
    }
    return FALSE;
}

static double
next_forced_break(const ns_print_scan *scan, double page_top, double limit)
{
    for (guint i = 0; i < scan->forced->len; i++) {
        double f = g_array_index(scan->forced, double, i);
        if (f <= page_top + 0.5) continue;
        if (f > limit + 0.5) break;
        if (has_content_between(scan->content, page_top, f) &&
            has_content_between(scan->content, f, G_MAXDOUBLE))
            return f;
    }
    return -1;
}

static double
pull_above_spans(const GArray *spans, double start, double limit)
{
    double cut = limit;
    for (int pass = 0; pass < 8; pass++) {
        double moved = cut;
        for (guint i = 0; i < spans->len; i++) {
            const ns_print_span *s = &g_array_index(spans, ns_print_span, i);
            if (s->top > start + 0.5 && s->top < moved - 0.5 &&
                s->bottom > moved + 0.5)
                moved = s->top;
        }
        if (moved >= cut - 0.01) break;
        cut = moved;
    }
    return cut > start + 1 ? cut : limit;
}

GArray *
ns_print_page_offsets(const ns_box *root, double page_content_height)
{
    GArray *offsets = g_array_new(FALSE, FALSE, sizeof(double));
    double zero = 0;
    g_array_append_val(offsets, zero);
    if (!root || !(page_content_height > 1)) return offsets;

    ns_print_scan scan = {
        .forced = g_array_new(FALSE, FALSE, sizeof(double)),
        .spans = g_array_new(FALSE, FALSE, sizeof(ns_print_span)),
        .content = g_array_new(FALSE, FALSE, sizeof(ns_print_span)),
        .page_h = page_content_height,
    };
    collect_breaks(root, &scan);
    g_array_sort(scan.forced, cmp_double);

    double doc_bottom = ns_box_max_bottom(root, 0);
    double cur = 0;
    while (offsets->len < 4096) {
        double limit = cur + page_content_height;
        double cut = next_forced_break(&scan, cur, limit);
        if (cut < 0) {
            if (limit >= doc_bottom - 0.5) break;
            cut = pull_above_spans(scan.spans, cur, limit);
        }
        if (cut <= cur + 1) cut = limit;
        g_array_append_val(offsets, cut);
        cur = cut;
    }

    g_array_free(scan.forced, TRUE);
    g_array_free(scan.spans, TRUE);
    g_array_free(scan.content, TRUE);
    return offsets;
}

double
ns_print_page_bottom(const GArray *offsets, guint i, double page_content_height)
{
    if (!offsets || i >= offsets->len) return 0;
    if (i + 1 < offsets->len) return g_array_index(offsets, double, i + 1);
    return g_array_index(offsets, double, i) + page_content_height;
}

void
ns_print_draw_page(cairo_t *cr, const ns_box *root,
                   const ns_print_setup *setup, double scale,
                   double page_top, double page_bottom)
{
    if (!cr || !root || !setup) return;
    double content_w = setup->width - setup->margin_left - setup->margin_right;
    double content_h = page_bottom - page_top;
    if (!(content_w > 0) || !(content_h > 0)) return;
    cairo_save(cr);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, setup->margin_left, setup->margin_top);
    cairo_rectangle(cr, 0, 0, content_w, content_h);
    cairo_clip(cr);
    cairo_translate(cr, 0, -page_top);
    ns_paint(cr, root, NULL);
    cairo_restore(cr);
}
