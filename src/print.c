/* Nordstjernen — paginates a laid-out page onto sheets of paper.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "print.h"

#include <stdlib.h>
#include <string.h>

#include "dom.h"
#include "paint.h"

#define NS_PRINT_MAX_SPANS (1u << 20)
#define NS_PRINT_KEEP_PX   48.0

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

static int
style_count(const ns_box *b, ns_css_prop prop, int fallback)
{
    const char *kw = break_keyword(b, prop);
    if (!kw || *kw < '0' || *kw > '9') return fallback;
    int n = atoi(kw);
    return n < 1 ? 1 : (n > 64 ? 64 : n);
}

static void
add_line_spans(GArray *spans, const ns_box *b, const GArray *lines,
               double page_h)
{
    guint n = lines->len / 2;
    guint orphans = (guint)style_count(b, NS_CSS_ORPHANS, 2);
    guint widows = (guint)style_count(b, NS_CSS_WIDOWS, 2);
    const double *y = &g_array_index(lines, double, 0);
    if (n < orphans + widows) {
        add_span(spans, y[0], y[2 * n - 1], page_h);
        return;
    }
    guint first = orphans > 1 ? orphans : 0;
    guint last = widows > 1 ? n - widows : n;
    if (first)
        add_span(spans, y[0], y[2 * first - 1], page_h);
    for (guint i = first; i < last && spans->len < NS_PRINT_MAX_SPANS; i++)
        add_span(spans, y[2 * i], y[2 * i + 1], page_h);
    if (last < n)
        add_span(spans, y[2 * last], y[2 * n - 1], page_h);
}

static void
add_text_line_spans(GArray *spans, const ns_box *b, double top, double bottom,
                    double page_h)
{
    NsPangoLayout *layout =
        b->vertical_wm ? NULL : ns_paint_build_inline_layout(NULL, b);
    int count = layout ? ns_pango_layout_get_line_count(layout) : 0;
    if (count < 2) {
        g_clear_object(&layout);
        add_span(spans, top, bottom, page_h);
        return;
    }
    GArray *lines = g_array_new(FALSE, FALSE, sizeof(double));
    const GArray *heights = b->atomic_line_heights;
    if (heights && heights->len == (guint)count) {
        double line_top = b->y;
        for (guint i = 0; i < heights->len; i++) {
            double line_bottom = line_top + g_array_index(heights, double, i);
            g_array_append_val(lines, line_top);
            g_array_append_val(lines, line_bottom);
            line_top = line_bottom;
        }
    } else {
        double y_origin = b->y + ns_paint_inline_y_offset_for_layout(b, layout);
        NsPangoLayoutIter *it = ns_pango_layout_get_iter(layout);
        do {
            NsPangoRectangle line;
            ns_pango_layout_iter_get_line_extents(it, NULL, &line);
            double line_top = y_origin + (double)line.y / NS_PANGO_SCALE;
            double line_bottom =
                y_origin + (double)(line.y + line.height) / NS_PANGO_SCALE;
            g_array_append_val(lines, line_top);
            g_array_append_val(lines, line_bottom);
        } while (ns_pango_layout_iter_next_line(it));
        ns_pango_layout_iter_free(it);
    }
    add_line_spans(spans, b, lines, page_h);
    g_array_free(lines, TRUE);
    g_object_unref(layout);
}

static void
add_content(GArray *content, double top, double bottom)
{
    if (!(bottom > top) || content->len >= NS_PRINT_MAX_SPANS) return;
    ns_print_span s = { top, bottom };
    g_array_append_val(content, s);
}

static int
heading_level(const ns_node *n)
{
    if (!n || !n->name || (n->name[0] != 'h' && n->name[0] != 'H')) return 0;
    if (n->name[1] < '1' || n->name[1] > '6' || n->name[2] != '\0') return 0;
    return n->name[1] - '0';
}

static gboolean
stays_with_next(const ns_box *b)
{
    return avoids_page_break(break_keyword(b, NS_CSS_BREAK_AFTER)) ||
           (b->kind != NS_BOX_INLINE && heading_level(b->dom) > 0);
}

static void
keep_together(const ns_box *prev, const ns_box *next, ns_print_scan *scan)
{
    double prev_top, prev_bottom, next_top, next_bottom;
    box_page_extent(prev, &prev_top, &prev_bottom);
    box_page_extent(next, &next_top, &next_bottom);
    if (!(next_bottom > next_top) || !(prev_bottom > prev_top)) return;
    double from = prev_bottom - prev_top <= scan->page_h / 4
        ? prev_top : prev_bottom - NS_PRINT_KEEP_PX;
    double to = MIN(next_bottom, next_top + NS_PRINT_KEEP_PX);
    add_span(scan->spans, from, to, scan->page_h);
}

static gboolean
collect_own_breaks(const ns_box *b, ns_print_scan *scan, double top,
                   double bottom)
{
    if (!b->parent) return FALSE;
    if (forces_page_break(break_keyword(b, NS_CSS_BREAK_BEFORE)))
        g_array_append_val(scan->forced, top);
    if (forces_page_break(break_keyword(b, NS_CSS_BREAK_AFTER)))
        g_array_append_val(scan->forced, bottom);
    gboolean whole = b->kind == NS_BOX_TABLE_ROW ||
        avoids_page_break(break_keyword(b, NS_CSS_BREAK_INSIDE));
    if (!whole || bottom - top > scan->page_h) return FALSE;
    add_span(scan->spans, top, bottom, scan->page_h);
    add_content(scan->content, top, bottom);
    return TRUE;
}

static void
collect_leaf(const ns_box *b, ns_print_scan *scan, double top, double bottom)
{
    if (b->kind == NS_BOX_INLINE && b->text && *b->text)
        add_text_line_spans(scan->spans, b, top, bottom, scan->page_h);
    else
        add_span(scan->spans, top, bottom, scan->page_h);
    add_content(scan->content, top, bottom);
}

static void
collect_breaks(const ns_box *b, ns_print_scan *scan)
{
    if (!b) return;
    double top, bottom;
    box_page_extent(b, &top, &bottom);
    if (collect_own_breaks(b, scan, top, bottom)) return;
    if (!b->first_child) {
        collect_leaf(b, scan, top, bottom);
        return;
    }
    const ns_box *prev = NULL;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
        if (prev && (stays_with_next(prev) ||
                     avoids_page_break(break_keyword(c, NS_CSS_BREAK_BEFORE))))
            keep_together(prev, c, scan);
        collect_breaks(c, scan);
        prev = c;
    }
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

void
ns_print_draw_page_scaled(cairo_t *cr, const ns_box *root,
                          const ns_print_setup *setup, double unit,
                          double content_scale, double page_top,
                          double page_bottom)
{
    if (!cr || !root || !setup) return;
    if (!(content_scale > 0)) content_scale = 1.0;
    double content_w = setup->width - setup->margin_left - setup->margin_right;
    double content_h = (page_bottom - page_top) * content_scale;
    if (!(content_w > 0) || !(content_h > 0)) return;
    cairo_save(cr);
    cairo_scale(cr, unit, unit);
    cairo_translate(cr, setup->margin_left, setup->margin_top);
    cairo_rectangle(cr, 0, 0, content_w, content_h);
    cairo_clip(cr);
    cairo_scale(cr, content_scale, content_scale);
    cairo_translate(cr, 0, -page_top);
    ns_paint(cr, root, NULL);
    cairo_restore(cr);
}

static void
add_link_rects(const ns_box *b, const ns_link_range *r, GArray *rects,
               GArray *out)
{
    g_array_set_size(rects, 0);
    if (!ns_paint_inline_range_rects(b, r->start, r->len, NULL, rects))
        return;
    for (guint k = 0; k + 3 < rects->len; k += 4) {
        ns_print_link link = {
            .x = g_array_index(rects, double, k) + b->x,
            .y = g_array_index(rects, double, k + 1) + b->y,
            .w = g_array_index(rects, double, k + 2),
            .h = g_array_index(rects, double, k + 3),
            .uri = g_strdup(r->href),
        };
        if (link.w > 0 && link.h > 0 && out->len < NS_PRINT_MAX_SPANS)
            g_array_append_val(out, link);
        else
            g_free(link.uri);
    }
}

static void
collect_text_links(const ns_box *b, GArray *out)
{
    if (b->kind != NS_BOX_INLINE || !b->links || !b->text || b->vertical_wm)
        return;
    GArray *rects = g_array_new(FALSE, FALSE, sizeof(double));
    for (guint i = 0; i < b->links->len; i++) {
        const ns_link_range *r = &g_array_index(b->links, ns_link_range, i);
        if (r->href && *r->href && r->len > 0)
            add_link_rects(b, r, rects, out);
    }
    g_array_free(rects, TRUE);
}

static void
collect_links(const ns_box *b, GArray *out)
{
    if (!b) return;
    collect_text_links(b, out);
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        collect_links(c, out);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            collect_links(g_array_index(b->inline_atomics, ns_inline_atomic,
                                        i).box, out);
}

GArray *
ns_print_links(const ns_box *root)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(ns_print_link));
    collect_links(root, out);
    return out;
}

void
ns_print_links_free(GArray *links)
{
    if (!links) return;
    for (guint i = 0; i < links->len; i++)
        g_free(g_array_index(links, ns_print_link, i).uri);
    g_array_free(links, TRUE);
}

static void
collect_headings(const ns_box *b, GArray *out)
{
    if (!b) return;
    int level = heading_level(b->dom);
    if (level && b->kind != NS_BOX_INLINE && out->len < NS_PRINT_MAX_SPANS) {
        char *text = ns_node_collect_text(b->dom);
        char *title = text ? g_strstrip(g_strdup(text)) : NULL;
        g_free(text);
        if (title && *title) {
            for (char *p = title; *p; p++)
                if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
            ns_print_heading h = { level, b->y + b->margin.top, title };
            g_array_append_val(out, h);
        } else {
            g_free(title);
        }
        return;
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        collect_headings(c, out);
}

GArray *
ns_print_headings(const ns_box *root)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(ns_print_heading));
    collect_headings(root, out);
    return out;
}

void
ns_print_headings_free(GArray *headings)
{
    if (!headings) return;
    for (guint i = 0; i < headings->len; i++)
        g_free(g_array_index(headings, ns_print_heading, i).text);
    g_array_free(headings, TRUE);
}

void
ns_print_draw_margin_text(cairo_t *cr, const ns_print_setup *setup, double unit,
                          const char *text, int slot, gboolean footer)
{
    if (!cr || !setup || !text || !*text) return;
    cairo_save(cr);
    cairo_scale(cr, unit, unit);
    NsPangoLayout *layout = ns_pango_cairo_create_layout(cr);
    NsPangoFontDescription *font =
        ns_pango_font_description_from_string("Sans 8");
    ns_pango_layout_set_font_description(layout, font);
    ns_pango_font_description_free(font);
    ns_pango_layout_set_text(layout, text, -1);
    int tw = 0, th = 0;
    ns_pango_layout_get_pixel_size(layout, &tw, &th);
    double x = slot == 0 ? setup->margin_left
             : slot == 1 ? (setup->width - tw) / 2.0
                         : setup->width - setup->margin_right - tw;
    double band = footer ? setup->margin_bottom : setup->margin_top;
    double y = footer ? setup->height - band + (band - th) / 2.0
                      : (band - th) / 2.0;
    if (band > th) {
        cairo_set_source_rgb(cr, 0.27, 0.27, 0.27);
        cairo_move_to(cr, x, y);
        ns_pango_cairo_show_layout(cr, layout);
    }
    g_object_unref(layout);
    cairo_restore(cr);
}
