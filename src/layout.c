/* Nordstjernen — block layout.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "layout.h"

#include <math.h>
#include "ns_pango.h"
#include <stdlib.h>
#include <string.h>

#include "css.h"
#include "css_anchor.h"
#include "html.h"
#include "image.h"
#include "svg.h"
#include "mathml.h"
#include "net.h"
#include "paint.h"

#define length_or ns_css_length_or

static void hit_enter_box(const ns_box *b, double *x, double *y);

static double
length_resolve(const ns_css_value *v, double basis, double fallback)
{
    if (!v) return fallback;
    if (ns_css_calc_is_math_fn(v))
        return ns_css_calc_math_fn_px(v, basis);
    if (v->kind == NS_CSS_V_CALC)
        return v->u.calc.pct / 100.0 * basis + v->u.calc.px;
    if (v->kind != NS_CSS_V_LENGTH) return fallback;
    if (v->u.length.unit == NS_CSS_UNIT_PX ||
        v->u.length.unit == NS_CSS_UNIT_NUMBER) return v->u.length.v;
    if (v->u.length.unit == NS_CSS_UNIT_EM ||
        v->u.length.unit == NS_CSS_UNIT_REM) return v->u.length.v * 16.0;
    if (v->u.length.unit == NS_CSS_UNIT_PERCENT)
        return v->u.length.v * basis / 100.0;
    if (v->u.length.unit == NS_CSS_UNIT_VW)
        return v->u.length.v * ns_css_viewport_w() / 100.0;
    if (v->u.length.unit == NS_CSS_UNIT_VH)
        return v->u.length.v * ns_css_viewport_h() / 100.0;
    if (v->u.length.unit == NS_CSS_UNIT_VMIN) {
        double m = MIN(ns_css_viewport_w(), ns_css_viewport_h());
        return v->u.length.v * m / 100.0;
    }
    if (v->u.length.unit == NS_CSS_UNIT_VMAX) {
        double m = MAX(ns_css_viewport_w(), ns_css_viewport_h());
        return v->u.length.v * m / 100.0;
    }
    if (v->u.length.unit == NS_CSS_UNIT_CQW) {
        double cw = ns_css_container_w();
        return v->u.length.v * (cw > 0 ? cw : ns_css_viewport_w()) / 100.0;
    }
    if (v->u.length.unit == NS_CSS_UNIT_CQH) {
        double ch = ns_css_container_h();
        return v->u.length.v * (ch > 0 ? ch : ns_css_viewport_h()) / 100.0;
    }
    if (v->u.length.unit == NS_CSS_UNIT_CQMIN ||
        v->u.length.unit == NS_CSS_UNIT_CQMAX) {
        double cw = ns_css_container_w(); if (cw <= 0) cw = ns_css_viewport_w();
        double ch = ns_css_container_h(); if (ch <= 0) ch = ns_css_viewport_h();
        double m = v->u.length.unit == NS_CSS_UNIT_CQMIN ? MIN(cw, ch) : MAX(cw, ch);
        return v->u.length.v * m / 100.0;
    }
    return fallback;
}

static double
length_resolve_nonnegative(const ns_css_value *v, double basis,
                           double fallback)
{
    double resolved = length_resolve(v, basis, fallback);
    return resolved < 0 ? 0 : resolved;
}

double
ns_inline_text_indent_px(const ns_box *run, const ns_style *s, double basis)
{
    return run && run->inline_split_tail ? 0 : ns_text_indent_px(s, basis);
}

double
ns_text_indent_px(const ns_style *s, double basis)
{
    const ns_css_value *v = s ? s->values[NS_CSS_TEXT_INDENT] : NULL;
    return v ? length_resolve(v, basis, 0) : 0;
}

static gboolean
length_is_auto(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           strcmp(v->u.keyword, "auto") == 0;
}

static const char *
abs_self_alignment(const ns_box *abox, ns_css_prop prop)
{
    const char *k = abox->style ? ns_style_keyword(abox->style, prop) : NULL;
    if (!k || strcmp(k, "auto") == 0) k = "normal";
    if (g_str_has_prefix(k, "safe ")) k += 5;
    else if (g_str_has_prefix(k, "unsafe ")) k += 7;
    if (strcmp(k, "normal") == 0) {
        gboolean replaced = abox->kind == NS_BOX_IMAGE ||
                            abox->kind == NS_BOX_VIDEO || abox->kind == NS_BOX_SVG;
        k = replaced ? "start" : "stretch";
    }
    return k;
}

static double
aspect_ratio_number(const ns_css_value *v, gboolean *with_auto)
{
    if (with_auto) *with_auto = FALSE;
    if (!v || v->kind != NS_CSS_V_SIZE || v->u.size.w_unit != NS_CSS_UNIT_NUMBER ||
        v->u.size.h_unit != NS_CSS_UNIT_NUMBER)
        return -1;
    if (with_auto) *with_auto = v->u.size.w_auto;
    return v->u.size.w > 0 && v->u.size.h > 0 ? v->u.size.w / v->u.size.h : -1;
}

static gboolean
value_is_percent(const ns_css_value *v)
{
    if (!v) return FALSE;
    if (v->kind == NS_CSS_V_LENGTH && v->u.length.unit == NS_CSS_UNIT_PERCENT)
        return TRUE;
    if (v->kind == NS_CSS_V_CALC && v->u.calc.pct != 0.0)
        return TRUE;
    if (ns_css_calc_is_math_fn(v))
        for (int i = 0; i < v->u.calc.n_args; i++)
            if (v->u.calc.args[i].pct != 0.0) return TRUE;
    return FALSE;
}

static gboolean
box_is_doc_root(const ns_box *b)
{
    return b && b->dom && b->dom->name &&
           (strcmp(b->dom->name, "html") == 0 ||
            strcmp(b->dom->name, "body") == 0);
}

static double containing_block_definite_height(const ns_box *box);
static gboolean style_is_absolute_or_fixed(const ns_style *s);
static gboolean height_keyword_stretches(const ns_css_value *v);
static gboolean size_keyword_is_intrinsic(const ns_css_value *v);
static double intrinsic_keyword_width(ns_box *box, const char *kw,
                                      const ns_style *mi, double avail);

static double
resolve_used_height(const ns_box *box, const ns_css_value *hv,
                    double width_basis, double fallback)
{
    if (!hv) return fallback;
    if (value_is_percent(hv)) {
        double vh;
        if (box_is_doc_root(box)) {
            vh = ns_css_viewport_h();
        } else if (box && box->style &&
                   ns_css_keyword_is(box->style->values[NS_CSS_POSITION],
                                     "fixed")) {
            vh = ns_css_viewport_h();
        } else {
            vh = containing_block_definite_height(box);
            if (vh < 0) return fallback;
        }
        if (hv->kind == NS_CSS_V_CALC)
            return hv->u.calc.pct / 100.0 * vh + hv->u.calc.px;
        return hv->u.length.v * vh / 100.0;
    }
    return length_resolve(hv, width_basis, fallback);
}

static double
flex_wrap_clamp_height(const ns_box *box, double h, double width_basis)
{
    const ns_style *s = box->style;
    double mn = resolve_used_height(box, s ? s->values[NS_CSS_MIN_HEIGHT] : NULL,
                                    width_basis, -1);
    double mx = resolve_used_height(box, s ? s->values[NS_CSS_MAX_HEIGHT] : NULL,
                                    width_basis, -1);
    if (s && ns_css_keyword_is(s->values[NS_CSS_BOX_SIZING], "border-box")) {
        double vex = box->border.top + box->border.bottom +
                     box->padding.top + box->padding.bottom;
        if (mn > 0) mn = MAX(mn - vex, 0);
        if (mx >= 0) mx = MAX(mx - vex, 0);
    }
    if (mx >= 0 && h > mx) h = mx;
    if (mn > 0 && h < mn) h = mn;
    return h;
}

static double
height_limit_in(const ns_css_value *v, double basis)
{
    if (!v || v->kind != NS_CSS_V_LENGTH) return -1;
    if (v->u.length.unit != NS_CSS_UNIT_PERCENT) return length_resolve(v, 0, -1);
    return basis >= 0 ? v->u.length.v * basis / 100.0 : -1;
}

static double
clamp_height_minmax_in(const ns_style *s, double h, double basis)
{
    if (!s || h < 0) return h;
    double mx = height_limit_in(s->values[NS_CSS_MAX_HEIGHT], basis);
    if (mx >= 0 && h > mx) h = mx;
    double mn = height_limit_in(s->values[NS_CSS_MIN_HEIGHT], basis);
    if (mn >= 0 && h < mn) h = mn;
    return h;
}

static double
clamp_height_minmax_px(const ns_style *s, double h)
{
    return clamp_height_minmax_in(s, h, -1);
}

static double
specified_height_to_content(const ns_box *b, double h)
{
    if (h < 0 || !b->style ||
        !ns_css_keyword_is(b->style->values[NS_CSS_BOX_SIZING], "border-box"))
        return h;
    h -= b->padding.top + b->padding.bottom + b->border.top + b->border.bottom;
    return h < 0 ? 0 : h;
}

static gboolean overflow_establishes_bfc(const ns_style *s);

static double
grid_item_min_block_contribution(const ns_box *c, double item_outer,
                                 double cb_height)
{
    if (!c->style) return item_outer;
    const ns_css_value *mnh = c->style->values[NS_CSS_MIN_HEIGHT];
    gboolean explicit_min = mnh && (mnh->kind == NS_CSS_V_LENGTH ||
                                    mnh->kind == NS_CSS_V_CALC);
    if (!explicit_min && !overflow_establishes_bfc(c->style))
        return item_outer;
    double min_h = explicit_min
        ? length_resolve(mnh, cb_height > 0 ? cb_height : 0, 0) : 0;
    if (min_h < 0) min_h = 0;
    double extras = c->padding.top + c->padding.bottom +
                    c->border.top + c->border.bottom +
                    c->margin.top + c->margin.bottom;
    return MIN(item_outer, min_h + extras);
}

static double
box_read_definite_height(const ns_box *box)
{
    ((ns_box *)box)->definite_height_read = TRUE;
    return box->definite_height;
}

static double
box_cb_height_known(const ns_box *box)
{
    if (!box) return 0;
    if (box->block_size_indefinite) return -1;
    return box->cb_height_override > 0 ? box->cb_height_override : 0;
}

static double
box_height_known_for_children(const ns_box *p)
{
    if (box_read_definite_height(p) > 0) return p->definite_height;
    return p->block_size_indefinite ? -1 : 0;
}

static double
containing_block_definite_height(const ns_box *box)
{
    double known = box_cb_height_known(box);
    if (known != 0) return known;
    const ns_box *p = box ? box->parent : NULL;
    while (p && !p->style) p = p->parent;
    if (!p) return -1;
    known = box_height_known_for_children(p);
    if (known != 0) return known;
    const ns_css_value *h = p->style->values[NS_CSS_HEIGHT];
    if (height_keyword_stretches(h)) {
        double base = containing_block_definite_height(p);
        if (base < 0) return -1;
        double inner = base - p->margin.top - p->margin.bottom
                     - p->border.top - p->border.bottom
                     - p->padding.top - p->padding.bottom;
        return clamp_height_minmax_in(p->style, inner > 0 ? inner : 0, base);
    }
    if (h && h->kind == NS_CSS_V_KEYWORD) h = NULL;
    if (!h) {
        const ns_css_value *top = p->style->values[NS_CSS_TOP];
        const ns_css_value *bottom = p->style->values[NS_CSS_BOTTOM];
        if (style_is_absolute_or_fixed(p->style) &&
            top && !length_is_auto(top) &&
            bottom && !length_is_auto(bottom) &&
            p->content_height > 0)
            return p->content_height;
        double ratio = aspect_ratio_number(p->style->values[NS_CSS_ASPECT_RATIO], NULL);
        if (ratio > 0 && p->content_width > 0)
            return p->content_width / ratio;
        return -1;
    }
    if (value_is_percent(h)) {
        double base = box_is_doc_root(p) ? ns_css_viewport_h()
                                         : containing_block_definite_height(p);
        if (base < 0) return -1;
        double ch = h->kind == NS_CSS_V_CALC
            ? h->u.calc.pct / 100.0 * base + h->u.calc.px
            : h->u.length.v * base / 100.0;
        return specified_height_to_content(p, clamp_height_minmax_in(p->style, ch, base));
    }
    if (p->content_height > 0) return p->content_height;
    return specified_height_to_content(
        p, clamp_height_minmax_px(p->style, length_resolve(h, 0, -1)));
}

static double
resolve_height_with_basis(const ns_css_value *hv, double width_basis,
                          double height_basis, double fallback)
{
    if (!hv) return fallback;
    if (hv->kind == NS_CSS_V_CALC) {
        if (ns_css_calc_is_math_fn(hv) && height_basis >= 0)
            return ns_css_calc_math_fn_px(hv, height_basis);
        if (hv->u.calc.pct != 0.0 && height_basis >= 0)
            return hv->u.calc.pct / 100.0 * height_basis + hv->u.calc.px;
        return length_resolve(hv, width_basis, fallback);
    }
    if (hv->kind == NS_CSS_V_LENGTH &&
        hv->u.length.unit == NS_CSS_UNIT_PERCENT) {
        if (height_basis >= 0)
            return hv->u.length.v * height_basis / 100.0;
        return fallback;
    }
    return length_resolve(hv, width_basis, fallback);
}

#define is_keyword ns_css_keyword_is
#define keyword_is ns_css_keyword_is

static gboolean
display_is_atomic_inline_container(ns_display d)
{
    return ns_display_is_atomic_inline(d) && d.inner != NS_DISPLAY_INNER_TABLE;
}

static gboolean
style_is_block(const ns_style *s)
{
    return ns_display_generates_own_box(ns_css_display_of(s));
}

static gboolean
style_is_block_level(const ns_style *s)
{
    return ns_display_is_block_level(ns_css_display_of(s));
}

static gboolean
style_display_is_table(const ns_style *s)
{
    return ns_display_is_table_wrapper(ns_css_display_of(s));
}

static gboolean
style_display_is_table_row(const ns_style *s)
{
    return ns_display_is(ns_css_display_of(s), NS_DISPLAY_INTERNAL_TABLE_ROW);
}

static gboolean
style_display_is_table_cell(const ns_style *s)
{
    return ns_display_is(ns_css_display_of(s), NS_DISPLAY_INTERNAL_TABLE_CELL);
}

static gboolean
style_display_is_table_caption(const ns_style *s)
{
    return ns_display_is(ns_css_display_of(s),
                         NS_DISPLAY_INTERNAL_TABLE_CAPTION);
}

static gboolean
style_is_flex_container(const ns_style *s)
{
    return ns_display_is_flex_container(ns_css_display_of(s));
}

static gboolean
style_is_grid_container(const ns_style *s)
{
    return ns_display_is_grid_container(ns_css_display_of(s));
}

static gboolean
box_is_form_widget(const ns_box *box)
{
    if (!box || box->kind != NS_BOX_BLOCK || !box->dom ||
        box->dom->kind != NS_NODE_ELEMENT || !box->dom->name)
        return FALSE;
    return strcmp(box->dom->name, "input") == 0 ||
           strcmp(box->dom->name, "textarea") == 0 ||
           strcmp(box->dom->name, "select") == 0;
}

static gboolean
box_is_flex_container(const ns_box *box)
{
    return box && !box_is_form_widget(box) &&
           style_is_flex_container(box->style);
}

static gboolean
box_is_grid_container(const ns_box *box)
{
    return box && !box_is_form_widget(box) &&
           style_is_grid_container(box->style);
}

static const char *
keyword_or(const ns_style *s, ns_css_prop p, const char *fallback)
{
    if (!s || !s->values[p]) return fallback;
    const ns_css_value *v = s->values[p];
    if (v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return fallback;
    return ns_css_alignment_base(v->u.keyword);
}

static const char *
flex_direction_of(const ns_style *s)
{
    const char *dir = keyword_or(s, NS_CSS_FLEX_DIRECTION, "row");
    int writing_mode = ns_css_writing_mode(s);
    if (!writing_mode) return dir;
    gboolean reverse = strstr(dir, "-reverse") != NULL;
    if (strncmp(dir, "row", 3) == 0) {
        gboolean rtl = keyword_is(s->values[NS_CSS_DIRECTION], "rtl");
        return reverse != rtl ? "column-reverse" : "column";
    }
    gboolean right_to_left = writing_mode == 1;
    return reverse != right_to_left ? "row-reverse" : "row";
}

static gboolean
self_start_is_far_side(const ns_style *s, gboolean horizontal_axis)
{
    int writing_mode = ns_css_writing_mode(s);
    gboolean rtl = s && keyword_is(s->values[NS_CSS_DIRECTION], "rtl");
    if (horizontal_axis)
        return writing_mode == 1 ? TRUE : writing_mode == 2 ? FALSE : rtl;
    if (!writing_mode) return FALSE;
    gboolean upward = keyword_is(s->values[NS_CSS_WRITING_MODE], "sideways-lr");
    return upward != rtl;
}

static double
number_or(const ns_css_value *v, double fallback)
{
    if (!v) return fallback;
    if (v->kind == NS_CSS_V_LENGTH) return v->u.length.v;
    return fallback;
}

static int
box_css_order(const ns_box *b)
{
    if (!b || !b->style) return 0;
    return (int)number_or(b->style->values[NS_CSS_ORDER], 0);
}

static void
reorder_children_by_order(ns_box *box)
{
    int n = 0;
    gboolean any = FALSE;
    for (ns_box *c = box->first_child; c; c = c->next_sibling) {
        n++;
        if (box_css_order(c) != 0) any = TRUE;
    }
    if (n < 2 || !any) return;
    ns_box **arr = g_new(ns_box *, n);
    int i = 0;
    for (ns_box *c = box->first_child; c; c = c->next_sibling) arr[i++] = c;
    for (int a = 1; a < n; a++) {
        ns_box *key = arr[a];
        int ko = box_css_order(key);
        int b = a - 1;
        while (b >= 0 && box_css_order(arr[b]) > ko) {
            arr[b + 1] = arr[b];
            b--;
        }
        arr[b + 1] = key;
    }
    for (int k = 0; k < n; k++)
        arr[k]->next_sibling = (k + 1 < n) ? arr[k + 1] : NULL;
    box->first_child = arr[0];
    box->last_child  = arr[n - 1];
    g_free(arr);
}

static gboolean
style_is_multicol(const ns_style *s)
{
    if (!s) return FALSE;
    const ns_css_value *cc = s->values[NS_CSS_COLUMN_COUNT];
    if (cc && cc->kind == NS_CSS_V_LENGTH && cc->u.length.v >= 2) return TRUE;
    const ns_css_value *cw = s->values[NS_CSS_COLUMN_WIDTH];
    return cw && cw->kind == NS_CSS_V_LENGTH && cw->u.length.v > 0;
}

static gboolean
style_is_absolute_or_fixed(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_POSITION] : NULL;
    return keyword_is(v, "absolute") || keyword_is(v, "fixed");
}

static gboolean
style_is_none(const ns_style *s)
{
    return ns_display_is_none(ns_css_display_of(s));
}

static gboolean
style_is_contents(const ns_style *s)
{
    return ns_display_is_contents(ns_css_display_of(s));
}

static gboolean
style_content_visibility_hidden(const ns_style *s)
{
    return s && s->values[NS_CSS_CONTENT_VISIBILITY] &&
           is_keyword(s->values[NS_CSS_CONTENT_VISIBILITY], "hidden");
}

static gboolean
style_contains_inline_size(const ns_style *s)
{
    const ns_css_value *ct = s ? s->values[NS_CSS_CONTAINER_TYPE] : NULL;
    if (!ct || ct->kind != NS_CSS_V_KEYWORD || !ct->u.keyword) return FALSE;
    return g_ascii_strcasecmp(ct->u.keyword, "size") == 0 ||
           g_ascii_strcasecmp(ct->u.keyword, "inline-size") == 0;
}

static gboolean
text_is_ws_only(const char *text)
{
    if (!text) return TRUE;
    for (const char *p = text; *p; p++)
        if (!g_ascii_isspace((unsigned char)*p)) return FALSE;
    return TRUE;
}

static double
border_side_width(const ns_style *s, ns_css_prop width_prop,
                  ns_css_prop style_prop)
{
    const ns_css_value *st = s->values[style_prop];
    if (!st || (st->kind == NS_CSS_V_KEYWORD && st->u.keyword &&
                (strcmp(st->u.keyword, "none") == 0 ||
                 strcmp(st->u.keyword, "hidden") == 0)))
        return 0;
    return length_or(s->values[width_prop], 0);
}

static void
edges_from_style(const ns_style *s, double basis,
                 ns_edges *margin, ns_edges *padding, ns_edges *border)
{
    if (!s) {
        memset(margin, 0, sizeof(*margin));
        memset(padding, 0, sizeof(*padding));
        memset(border, 0, sizeof(*border));
        return;
    }
    margin->top    = length_resolve(s->values[NS_CSS_MARGIN_TOP],    basis, 0);
    margin->right  = length_resolve(s->values[NS_CSS_MARGIN_RIGHT],  basis, 0);
    margin->bottom = length_resolve(s->values[NS_CSS_MARGIN_BOTTOM], basis, 0);
    margin->left   = length_resolve(s->values[NS_CSS_MARGIN_LEFT],   basis, 0);
    padding->top    = length_resolve_nonnegative(
        s->values[NS_CSS_PADDING_TOP], basis, 0);
    padding->right  = length_resolve_nonnegative(
        s->values[NS_CSS_PADDING_RIGHT], basis, 0);
    padding->bottom = length_resolve_nonnegative(
        s->values[NS_CSS_PADDING_BOTTOM], basis, 0);
    padding->left   = length_resolve_nonnegative(
        s->values[NS_CSS_PADDING_LEFT], basis, 0);
    border->top    = border_side_width(s, NS_CSS_BORDER_TOP_WIDTH,
                                       NS_CSS_BORDER_TOP_STYLE);
    border->right  = border_side_width(s, NS_CSS_BORDER_RIGHT_WIDTH,
                                       NS_CSS_BORDER_RIGHT_STYLE);
    border->bottom = border_side_width(s, NS_CSS_BORDER_BOTTOM_WIDTH,
                                       NS_CSS_BORDER_BOTTOM_STYLE);
    border->left   = border_side_width(s, NS_CSS_BORDER_LEFT_WIDTH,
                                       NS_CSS_BORDER_LEFT_STYLE);
}

static ns_box *g_box_pool[16384];
static int g_box_pool_n;

static ns_box *
box_new(ns_box_kind kind)
{
    ns_box *b;
    if (g_box_pool_n > 0) {
        b = g_box_pool[--g_box_pool_n];
        memset(b, 0, sizeof(*b));
    } else {
        b = g_new0(ns_box, 1);
    }
    b->kind = kind;
    b->colspan = 1;
    b->rowspan = 1;
    return b;
}

static void
link_clear(gpointer data)
{
    ns_link_range *r = data;
    g_free(r->href);
    g_free(r->target);
}

static GArray *inline_links_ensure(ns_box *b);

static ns_box_media *
ns_box_media_ensure(ns_box *b)
{
    if (!b->media) b->media = g_new0(ns_box_media, 1);
    return b->media;
}

static ns_box *
inline_merge_prefix(ns_box *prefix, ns_box *suffix)
{
    if (!prefix) return suffix;
    if (!suffix) return prefix;
    gsize plen = prefix->text ? strlen(prefix->text) : 0;
    gsize slen = suffix->text ? strlen(suffix->text) : 0;
    if (plen > G_MAXSIZE - slen - 1) { ns_box_free(suffix); return prefix; }
    char *combined = g_malloc(plen + slen + 1);
    if (plen) memcpy(combined, prefix->text, plen);
    if (slen) memcpy(combined + plen, suffix->text, slen);
    combined[plen + slen] = '\0';
    g_free(suffix->text);
    suffix->text = combined;

    if (suffix->attrs) {
        for (guint i = 0; i < suffix->attrs->len; i++) {
            ns_inline_attr *a = &g_array_index(suffix->attrs, ns_inline_attr, i);
            a->start += plen;
        }
    }
    if (suffix->links) {
        for (guint i = 0; i < suffix->links->len; i++) {
            ns_link_range *l = &g_array_index(suffix->links, ns_link_range, i);
            l->start += plen;
        }
    }
    if (prefix->attrs) {
        for (guint i = 0; i < prefix->attrs->len; i++) {
            ns_inline_attr a = g_array_index(prefix->attrs, ns_inline_attr, i);
            g_array_append_val(suffix->attrs, a);
        }
    }
    if (suffix->inline_atomics) {
        for (guint i = 0; i < suffix->inline_atomics->len; i++)
            g_array_index(suffix->inline_atomics, ns_inline_atomic, i).byte_off += plen;
    }
    if (prefix->inline_atomics) {
        if (!suffix->inline_atomics)
            suffix->inline_atomics = g_array_new(FALSE, FALSE, sizeof(ns_inline_atomic));
        for (guint i = 0; i < prefix->inline_atomics->len; i++) {
            ns_inline_atomic ia = g_array_index(prefix->inline_atomics, ns_inline_atomic, i);
            if (ia.box && ia.box->parent == prefix) ia.box->parent = suffix;
            g_array_append_val(suffix->inline_atomics, ia);
        }
        g_array_free(prefix->inline_atomics, TRUE);
        prefix->inline_atomics = NULL;
    }
    if (prefix->links) {
        GArray *dst = inline_links_ensure(suffix);
        for (guint i = 0; i < prefix->links->len; i++) {
            ns_link_range src = g_array_index(prefix->links, ns_link_range, i);
            ns_link_range dup = src;
            dup.href   = src.href   ? g_strdup(src.href)   : NULL;
            dup.target = src.target ? g_strdup(src.target) : NULL;
            g_array_append_val(dst, dup);
        }
    }
    ns_box_free(prefix);
    return suffix;
}

static ns_box *
box_new_inline(void)
{
    ns_box *b = box_new(NS_BOX_INLINE);
    b->attrs = g_array_new(FALSE, FALSE, sizeof(ns_inline_attr));
    return b;
}

static GArray *
inline_links_ensure(ns_box *b)
{
    if (!b->links) {
        b->links = g_array_new(FALSE, FALSE, sizeof(ns_link_range));
        g_array_set_clear_func(b->links, link_clear);
    }
    return b->links;
}

static void
inline_run_drop_caches(ns_box *run)
{
    run->inline_layout_cache_valid = FALSE;
    run->inline_natural_cache_valid = FALSE;
    run->inline_min_cache_valid = FALSE;
    if (run->paint_layout) ns_paint_drop_box_cache(run);
}

static void
inline_split_attrs(ns_box *run, ns_box *tail, gsize split)
{
    GArray *head_attrs = g_array_new(FALSE, FALSE, sizeof(ns_inline_attr));
    for (guint i = 0; run->attrs && i < run->attrs->len; i++) {
        ns_inline_attr a = g_array_index(run->attrs, ns_inline_attr, i);
        gsize end = a.start + a.len;
        if (a.start < split) {
            ns_inline_attr h = a;
            if (end > split) h.len = split - a.start;
            g_array_append_val(head_attrs, h);
        }
        if (end > split || a.start >= split) {
            ns_inline_attr t = a;
            t.start = MAX(a.start, split) - split;
            t.len = end > split ? end - MAX(a.start, split) : 0;
            g_array_append_val(tail->attrs, t);
        }
    }
    if (run->attrs) g_array_free(run->attrs, TRUE);
    run->attrs = head_attrs;
}

static ns_link_range
link_range_copy(const ns_link_range *l, gsize start, gsize len)
{
    ns_link_range c = *l;
    c.href = l->href ? g_strdup(l->href) : NULL;
    c.target = l->target ? g_strdup(l->target) : NULL;
    c.start = start;
    c.len = len;
    return c;
}

static void
inline_split_links(ns_box *run, ns_box *tail, gsize split)
{
    if (!run->links) return;
    GArray *head_links = g_array_new(FALSE, FALSE, sizeof(ns_link_range));
    g_array_set_clear_func(head_links, link_clear);
    for (guint i = 0; i < run->links->len; i++) {
        const ns_link_range *l = &g_array_index(run->links, ns_link_range, i);
        gsize end = l->start + l->len;
        if (l->start < split) {
            ns_link_range h = link_range_copy(l, l->start,
                                              MIN(end, split) - l->start);
            g_array_append_val(head_links, h);
        }
        if (end > split) {
            gsize from = MAX(l->start, split);
            ns_link_range t = link_range_copy(l, from - split, end - from);
            g_array_append_val(inline_links_ensure(tail), t);
        }
    }
    g_array_free(run->links, TRUE);
    run->links = head_links;
}

static void
inline_split_atomics(ns_box *run, ns_box *tail, gsize split)
{
    if (!run->inline_atomics) return;
    GArray *head_atomics = g_array_new(FALSE, FALSE, sizeof(ns_inline_atomic));
    for (guint i = 0; i < run->inline_atomics->len; i++) {
        ns_inline_atomic ia =
            g_array_index(run->inline_atomics, ns_inline_atomic, i);
        if (ia.byte_off < split) {
            g_array_append_val(head_atomics, ia);
            continue;
        }
        if (!tail->inline_atomics)
            tail->inline_atomics =
                g_array_new(FALSE, FALSE, sizeof(ns_inline_atomic));
        ia.byte_off -= split;
        if (ia.box && ia.box->parent == run) ia.box->parent = tail;
        g_array_append_val(tail->inline_atomics, ia);
    }
    g_array_free(run->inline_atomics, TRUE);
    run->inline_atomics = head_atomics;
}

static ns_box *
inline_run_split(ns_box *run, gsize split)
{
    ns_box *tail = box_new_inline();
    tail->dom = run->dom;
    tail->style = run->style;
    tail->inline_split_tail = TRUE;
    tail->text = g_strdup(run->text + split);
    run->text[split] = '\0';

    inline_split_attrs(run, tail, split);
    inline_split_links(run, tail, split);
    inline_split_atomics(run, tail, split);

    tail->parent = run->parent;
    tail->next_sibling = run->next_sibling;
    run->next_sibling = tail;
    if (run->parent && run->parent->last_child == run)
        run->parent->last_child = tail;
    inline_run_drop_caches(run);
    return tail;
}

static void
inline_runs_join_splits(ns_box *box)
{
    ns_box *prev = NULL;
    for (ns_box *c = box->first_child; c; ) {
        ns_box *next = c->next_sibling;
        if (!next || !next->inline_split_tail ||
            c->kind != NS_BOX_INLINE || next->kind != NS_BOX_INLINE) {
            prev = c;
            c = next;
            continue;
        }
        gboolean head_is_tail = c->inline_split_tail;
        ns_box *after = next->next_sibling;
        ns_box *joined = inline_merge_prefix(c, next);
        joined->inline_split_tail = head_is_tail;
        joined->parent = box;
        joined->next_sibling = after;
        if (prev) prev->next_sibling = joined;
        else box->first_child = joined;
        if (!after) box->last_child = joined;
        inline_run_drop_caches(joined);
        c = joined;
    }
}

static gsize
inline_run_break_before(const ns_box *run, double band_height)
{
    NsPangoLayout *layout = ns_paint_build_inline_layout(NULL, run);
    if (!layout) return 0;
    gsize split = 0;
    NsPangoLayoutIter *iter = ns_pango_layout_get_iter(layout);
    do {
        NsPangoRectangle logical;
        ns_pango_layout_iter_get_line_extents(iter, NULL, &logical);
        if ((double)logical.y / NS_PANGO_SCALE >= band_height - 0.5) {
            split = (gsize)ns_pango_layout_iter_get_index(iter);
            break;
        }
    } while (ns_pango_layout_iter_next_line(iter));
    ns_pango_layout_iter_free(iter);
    g_object_unref(layout);
    return split;
}

static void
box_append_child(ns_box *parent, ns_box *child)
{
    if (parent->last_child && parent->last_child->kind == NS_BOX_INLINE &&
        child->kind == NS_BOX_INLINE && !parent->last_child->style &&
        !child->style) {
        ns_box *prefix = parent->last_child;
        ns_box *previous = NULL;
        for (ns_box *c = parent->first_child; c && c != prefix;
             c = c->next_sibling)
            previous = c;
        ns_box *merged = inline_merge_prefix(prefix, child);
        merged->parent = parent;
        if (previous) previous->next_sibling = merged;
        else parent->first_child = merged;
        parent->last_child = merged;
        return;
    }
    child->parent = parent;
    if (!parent->first_child) parent->first_child = child;
    else                       parent->last_child->next_sibling = child;
    parent->last_child = child;
}

void
ns_box_free(ns_box *box)
{
    if (!box) return;
    GPtrArray *stack = g_ptr_array_new();
    g_ptr_array_add(stack, box);
    while (stack->len > 0) {
        ns_box *cur = g_ptr_array_index(stack, stack->len - 1);
        g_ptr_array_set_size(stack, stack->len - 1);
        for (ns_box *c = cur->first_child; c; ) {
            ns_box *next = c->next_sibling;
            g_ptr_array_add(stack, c);
            c = next;
        }
        if (cur->paint_layout) ns_paint_drop_box_cache(cur);
        if (cur->owned_style) ns_style_free(cur->owned_style);
        g_clear_pointer(&cur->negative_layers, g_ptr_array_unref);
        if (cur->links) g_array_free(cur->links, TRUE);
        if (cur->attrs) g_array_free(cur->attrs, TRUE);
        if (cur->table_col_hints) g_array_free(cur->table_col_hints, TRUE);
        if (cur->grid_col_tracks) g_array_free(cur->grid_col_tracks, TRUE);
        if (cur->grid_row_tracks) g_array_free(cur->grid_row_tracks, TRUE);
        if (cur->inline_atomics) {
            for (guint i = 0; i < cur->inline_atomics->len; i++) {
                ns_inline_atomic *ia = &g_array_index(cur->inline_atomics,
                                                      ns_inline_atomic, i);
                if (ia->box) g_ptr_array_add(stack, ia->box);
            }
            g_array_free(cur->inline_atomics, TRUE);
        }
        if (cur->atomic_line_heights) {
            g_array_free(cur->atomic_line_heights, TRUE);
        }
        if (cur->atomic_line_offsets)
            g_array_free(cur->atomic_line_offsets, TRUE);
        g_free(cur->text);
        if (cur->media) {
            g_free(cur->media->image_src);
            g_free(cur->media->bg_image_src);
            g_free(cur->media->marker_image_src);
            g_free(cur->media->border_image_src);
            g_free(cur->media->mask_image_src);
            if (cur->media->bg_layer_srcs)
                g_ptr_array_free(cur->media->bg_layer_srcs, TRUE);
            if (cur->media->bg_layer_images)
                g_ptr_array_free(cur->media->bg_layer_images, TRUE);
            g_free(cur->media->video_src);
            g_free(cur->media->video_poster);
            g_free(cur->media->video_audio_src);
            if (cur->media->svg_foreign) {
                for (guint i = 0; i < cur->media->svg_foreign->len; i++)
                    g_ptr_array_add(stack,
                        g_ptr_array_index(cur->media->svg_foreign, i));
                g_ptr_array_free(cur->media->svg_foreign, TRUE);
            }
            g_free(cur->media->svg_foreign_xform);
            g_free(cur->media);
        }
        if (g_box_pool_n < (int)G_N_ELEMENTS(g_box_pool))
            g_box_pool[g_box_pool_n++] = cur;
        else
            g_free(cur);
    }
    g_ptr_array_free(stack, TRUE);
}

static gboolean box_clips_children(const ns_box *b);
static gboolean box_first_baseline(const ns_box *b, double *out);
static gboolean style_has_layout_containment(const ns_style *s);

static gboolean
box_clips_for_page_height(const ns_box *b)
{
    if (!b->parent) return FALSE;
    if (b->dom && b->dom->name &&
        (strcmp(b->dom->name, "html") == 0 ||
         strcmp(b->dom->name, "body") == 0))
        return FALSE;
    return box_clips_children(b);
}

static void
box_walk_max_bottom(const ns_box *b, double *out)
{
    if (!b) return;
    double bottom = b->y + b->content_height;
    if (bottom > *out) *out = bottom;
    double mx0, my0, mx1, my1;
    if (ns_paint_inline_moved_extents(b, &mx0, &my0, &mx1, &my1) &&
        my1 > *out)
        *out = my1;
    if (box_clips_for_page_height(b)) return;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        box_walk_max_bottom(c, out);
}

double
ns_box_max_bottom(const ns_box *root, double seed)
{
    double out = seed;
    box_walk_max_bottom(root, &out);
    return out;
}

static gboolean
is_replaced_block_tag(const char *name)
{
    return name && (strcmp(name, "img") == 0 ||
                    strcmp(name, "svg") == 0 ||
                    strcmp(name, "canvas") == 0 ||
                    strcmp(name, "audio") == 0 ||
                    strcmp(name, "video") == 0 ||
                    strcmp(name, "math") == 0 ||
                    strcmp(name, "table") == 0);
}

static gboolean
is_inline_level_replaced(const ns_node *n, GHashTable *styles)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    if (!(strcmp(n->name, "img") == 0 || strcmp(n->name, "svg") == 0 ||
          strcmp(n->name, "audio") == 0 ||
          strcmp(n->name, "video") == 0 || strcmp(n->name, "math") == 0 ||
          strcmp(n->name, "canvas") == 0))
        return FALSE;
    const ns_style *s = styles ? g_hash_table_lookup(styles, n) : NULL;
    return !ns_display_is_block_level(ns_css_display_of(s));
}

static gboolean
video_natural_size(const ns_box *box, double *w, double *h)
{
    if (!box || box->kind != NS_BOX_VIDEO || !box->dom) return FALSE;
    const char *size = ns_element_get_attr(box->dom, NS_MEDIA_SIZE_ATTR);
    int vw = 0, vh = 0;
    if (!size || sscanf(size, "%dx%d", &vw, &vh) != 2 || vw <= 0 || vh <= 0)
        return FALSE;
    *w = vw;
    *h = vh;
    return TRUE;
}

static gboolean
node_has_media_metadata(const ns_node *n)
{
    return n && n->kind == NS_NODE_ELEMENT &&
           (ns_element_get_attr(n, NS_MEDIA_SRC_ATTR) != NULL ||
            ns_element_get_attr(n, NS_MEDIA_STREAM_ATTR) != NULL);
}

#define NS_LAYOUT_MAX_DEPTH 512
#define NS_LAYOUT_UNIT (1.0 / 64.0)
#define NS_TABLE_MAX_COLS 4096

static gboolean tag_is_non_rendering(const char *name);
static gboolean node_is_non_rendering(const ns_node *n);

static gboolean
node_is_frame_fallback(const ns_node *n)
{
    if (!n || n->kind == NS_NODE_DOCUMENT) return FALSE;
    const ns_node *p = n->parent;
    return p && p->kind == NS_NODE_ELEMENT && p->name &&
           (strcmp(p->name, "iframe") == 0 || strcmp(p->name, "frame") == 0);
}

static gboolean
node_is_frame_element(const ns_node *n)
{
    return n && n->kind == NS_NODE_ELEMENT && n->name &&
           (strcmp(n->name, "iframe") == 0 || strcmp(n->name, "frame") == 0 ||
            strcmp(n->name, "object") == 0);
}

static const ns_node *
frame_document_host(const ns_node *n)
{
    if (!n || n->kind != NS_NODE_DOCUMENT) return NULL;
    return node_is_frame_element(n->parent) ? n->parent : NULL;
}

static GHashTable *g_contains_block_media_cache;
static GHashTable *g_select_width_cache;

static const ns_node *layout_slot_host(const ns_node *slot);
static const ns_node *layout_shadow_root(const ns_node *host);
static gboolean contains_block_media_depth(const ns_node *n, GHashTable *styles,
                                           int depth);

static gboolean
child_makes_block_media(const ns_node *c, GHashTable *styles, int depth)
{
    if (c->kind != NS_NODE_ELEMENT || !c->name) return FALSE;
    if (node_is_non_rendering(c)) return FALSE;
    const ns_style *cs = styles ? g_hash_table_lookup(styles, c) : NULL;
    if (cs && (style_is_none(cs) || style_is_absolute_or_fixed(cs)))
        return FALSE;
    if (node_has_media_metadata(c) ||
        (is_replaced_block_tag(c->name) &&
         !is_inline_level_replaced(c, styles)) ||
        strcmp(c->name, "iframe") == 0)
        return TRUE;
    if (cs) {
        if (style_is_block_level(cs)) return TRUE;
        if (ns_display_is_atomic_inline(ns_css_display_of(cs)))
            return FALSE;
    }
    return contains_block_media_depth(c, styles, depth + 1);
}

static gboolean
slot_assigns_block_media(const ns_node *slot, GHashTable *styles, int depth)
{
    const ns_node *host = layout_slot_host(slot);
    const ns_node *sr = host ? layout_shadow_root(host) : NULL;
    if (!sr) return FALSE;
    const char *slot_name = ns_element_get_attr(slot, "name");
    for (const ns_node *lc = host->first_child; lc; lc = lc->next_sibling) {
        if (lc == sr || lc->kind != NS_NODE_ELEMENT) continue;
        const char *assigned = ns_element_get_attr(lc, "slot");
        if (g_strcmp0(assigned ? assigned : "", slot_name ? slot_name : "") != 0)
            continue;
        if (child_makes_block_media(lc, styles, depth)) return TRUE;
    }
    return FALSE;
}

static gboolean
contains_block_media_depth(const ns_node *n, GHashTable *styles, int depth)
{
    if (!n || depth >= NS_LAYOUT_MAX_DEPTH || n->kind != NS_NODE_ELEMENT)
        return FALSE;
    const ns_node *shadow_root = layout_shadow_root(n);
    const ns_node *first = shadow_root ? shadow_root->first_child : n->first_child;
    for (const ns_node *c = first; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            strcmp(c->name, "slot") == 0) {
            if (slot_assigns_block_media(c, styles, depth + 1)) return TRUE;
            continue;
        }
        if (child_makes_block_media(c, styles, depth)) return TRUE;
    }
    return FALSE;
}

static gboolean
contains_block_media(const ns_node *n, GHashTable *styles)
{
    if (!n) return FALSE;
    if (g_contains_block_media_cache) {
        gpointer hit = g_hash_table_lookup(g_contains_block_media_cache, n);
        if (hit) return GPOINTER_TO_INT(hit) == 2;
    }
    gboolean has_media = contains_block_media_depth(n, styles, 0);
    if (g_contains_block_media_cache)
        g_hash_table_insert(g_contains_block_media_cache,
                            (gpointer)n, GINT_TO_POINTER(has_media ? 2 : 1));
    return has_media;
}

static gboolean is_inline_dom(const ns_node *n, GHashTable *styles);

static gboolean
abs_joins_inline_run(const ns_node *n, const ns_style *s, GHashTable *styles)
{
    if (!s || !s->specified_inline || !n->parent) return FALSE;
    const ns_style *parent = g_hash_table_lookup(styles, n->parent);
    if (parent && (style_is_flex_container(parent) ||
                   style_is_grid_container(parent)))
        return FALSE;
    for (const ns_node *p = n->prev_sibling; p; p = p->prev_sibling) {
        if (p->kind == NS_NODE_TEXT) {
            for (const char *t = p->text; t && *t; t++)
                if (!g_ascii_isspace(*t)) return TRUE;
            continue;
        }
        if (p->kind != NS_NODE_ELEMENT) continue;
        const ns_style *ps = g_hash_table_lookup(styles, p);
        if (ps && (style_is_none(ps) || style_is_absolute_or_fixed(ps)))
            continue;
        return is_inline_dom(p, styles);
    }
    return FALSE;
}

enum { NS_WS_COLLAPSE = 0, NS_WS_PRESERVE = 1, NS_WS_PRE_LINE = 2 };

static int clear_kind_of(const ns_style *s);
static int float_side_of(const ns_style *s);
static int white_space_mode(const ns_node *node, GHashTable *styles);

static gboolean
is_inline_dom(const ns_node *n, GHashTable *styles)
{
    if (!n) return FALSE;
    if (n->kind == NS_NODE_TEXT) return TRUE;
    if (n->kind != NS_NODE_ELEMENT) return FALSE;
    if (n->name && strcmp(n->name, "slot") == 0) return FALSE;
    if (node_has_media_metadata(n)) return FALSE;
    if (layout_shadow_root(n)) {
        const ns_style *hs = g_hash_table_lookup(styles, n);
        if (!hs || style_is_none(hs) || style_is_block_level(hs) ||
            style_is_absolute_or_fixed(hs))
            return FALSE;
        if (!display_is_atomic_inline_container(ns_css_display_of(hs)) &&
            contains_block_media(n, styles))
            return FALSE;
    }
    if (is_replaced_block_tag(n->name)) {
        if (strcmp(n->name, "table") == 0) return FALSE;
        const ns_style *rs = g_hash_table_lookup(styles, n);
        if (rs && style_is_none(rs)) return FALSE;
        if (rs && style_is_absolute_or_fixed(rs))
            return abs_joins_inline_run(n, rs, styles);
        return is_inline_level_replaced(n, styles);
    }
    const ns_style *s = g_hash_table_lookup(styles, n);
    if (!s) return n->name && strchr(n->name, '-') != NULL;
    if (style_is_none(s)) return FALSE;
    if (style_is_absolute_or_fixed(s)) return abs_joins_inline_run(n, s, styles);
    ns_display d = ns_css_display_of(s);
    if (ns_display_is_table_internal(d)) return FALSE;
    if (ns_display_is_flex_container(d) || ns_display_is_grid_container(d)) {
        if (d.outer == NS_DISPLAY_OUTER_INLINE) return TRUE;
    }
    if (n->name && strcmp(n->name, "br") == 0)
        return !clear_kind_of(s);
    if (!style_is_block(s) && contains_block_media(n, styles)) return FALSE;
    if (display_is_atomic_inline_container(d)) return TRUE;
    return !style_is_block(s);
}

static gboolean
node_leaves_inline_run_open(const ns_node *n, GHashTable *styles)
{
    if (n->kind == NS_NODE_COMMENT) return TRUE;
    if (n->kind != NS_NODE_ELEMENT) return FALSE;
    const ns_style *s = g_hash_table_lookup(styles, n);
    return s && style_is_none(s);
}

static gboolean
continues_inline_run(const ns_node *n, GHashTable *styles)
{
    return is_inline_dom(n, styles) || node_leaves_inline_run_open(n, styles);
}

static gboolean
node_is_out_of_flow(const ns_node *n, GHashTable *styles)
{
    if (!n || n->kind != NS_NODE_ELEMENT) return FALSE;
    const ns_style *s = g_hash_table_lookup(styles, n);
    if (!s || style_is_none(s)) return FALSE;
    return float_side_of(s) >= 0 || style_is_absolute_or_fixed(s);
}

static gboolean
node_renders_nothing_inline(const ns_node *n, GHashTable *styles)
{
    if (n->kind == NS_NODE_TEXT)
        return text_is_ws_only(n->text) &&
               white_space_mode(n, styles) == NS_WS_COLLAPSE;
    return node_leaves_inline_run_open(n, styles) || node_is_non_rendering(n);
}

static gboolean
inline_range_waits_for_next(const ns_node *start, const ns_node *end,
                            GHashTable *styles)
{
    if (!node_is_out_of_flow(end, styles) &&
        !(end && style_is_contents(g_hash_table_lookup(styles, end))))
        return FALSE;
    for (const ns_node *n = start; n && n != end; n = n->next_sibling)
        if (!node_renders_nothing_inline(n, styles)) return FALSE;
    return TRUE;
}

static gboolean
inline_run_opens_parent(const ns_node *first, GHashTable *styles)
{
    for (const ns_node *p = first->prev_sibling; p; p = p->prev_sibling)
        if (!node_is_out_of_flow(p, styles) &&
            !node_renders_nothing_inline(p, styles))
            return FALSE;
    return TRUE;
}

static ns_display_internal
node_table_internal(const ns_node *n, GHashTable *styles)
{
    if (!n || n->kind != NS_NODE_ELEMENT) return NS_DISPLAY_INTERNAL_NONE;
    ns_display d = ns_css_display_of(
        styles ? g_hash_table_lookup(styles, n) : NULL);
    return ns_display_is_table_internal(d) ? d.internal
                                           : NS_DISPLAY_INTERNAL_NONE;
}

static gboolean
node_is_row_group(const ns_node *n, GHashTable *styles)
{
    ns_display_internal k = node_table_internal(n, styles);
    return k == NS_DISPLAY_INTERNAL_TABLE_ROW_GROUP ||
           k == NS_DISPLAY_INTERNAL_TABLE_HEADER_GROUP ||
           k == NS_DISPLAY_INTERNAL_TABLE_FOOTER_GROUP;
}

static gboolean
is_table_row(const ns_node *n, GHashTable *styles)
{
    if (!n || n->kind != NS_NODE_ELEMENT) return FALSE;
    const ns_style *s = styles ? g_hash_table_lookup(styles, n) : NULL;
    if (s) return style_display_is_table_row(s);
    return n->name && strcmp(n->name, "tr") == 0;
}

static gboolean
is_table_box(const ns_node *n, GHashTable *styles)
{
    if (!n || n->kind != NS_NODE_ELEMENT) return FALSE;
    const ns_style *s = styles ? g_hash_table_lookup(styles, n) : NULL;
    if (s) return style_display_is_table(s);
    return n->name && strcmp(n->name, "table") == 0;
}

static gboolean
is_cell_element(const ns_node *n, GHashTable *styles)
{
    if (!n || n->kind != NS_NODE_ELEMENT) return FALSE;
    const ns_style *s = styles ? g_hash_table_lookup(styles, n) : NULL;
    if (s) return style_display_is_table_cell(s);
    return ns_node_is_element_named(n, "td") ||
           ns_node_is_element_named(n, "th");
}

static gboolean
is_table_caption(const ns_node *n, GHashTable *styles)
{
    if (ns_node_is_element_named(n, "caption")) return TRUE;
    if (!n || n->kind != NS_NODE_ELEMENT) return FALSE;
    return style_display_is_table_caption(g_hash_table_lookup(styles, n));
}

static gboolean
node_is_table_internal(const ns_node *n, GHashTable *styles)
{
    return node_table_internal(n, styles) != NS_DISPLAY_INTERNAL_NONE ||
           is_cell_element(n, styles) || is_table_row(n, styles) ||
           is_table_caption(n, styles);
}

static gboolean
node_is_table_run_gap(const ns_node *n, GHashTable *styles)
{
    if (n->kind == NS_NODE_TEXT)
        return text_is_ws_only(n->text) &&
               white_space_mode(n, styles) == NS_WS_COLLAPSE;
    if (n->kind == NS_NODE_ELEMENT && n->name &&
        tag_is_non_rendering(n->name))
        return TRUE;
    return node_leaves_inline_run_open(n, styles);
}

static const ns_node *
table_internal_run_end(const ns_node *c, GHashTable *styles)
{
    while (c && (node_is_table_internal(c, styles) ||
                 node_is_table_run_gap(c, styles)))
        c = c->next_sibling;
    return c;
}

static void queue_absolute_node(const ns_node *n, const ns_style *s);

static gboolean
row_group_child_needs_anonymous_row(const ns_node *c, GHashTable *styles)
{
    if (!c || c->kind != NS_NODE_ELEMENT || !c->name) return FALSE;
    if (tag_is_non_rendering(c->name)) return FALSE;
    const ns_style *cs = g_hash_table_lookup(styles, c);
    if (!cs || style_is_none(cs) || style_is_contents(cs)) return FALSE;
    if (style_is_absolute_or_fixed(cs)) return FALSE;
    return !node_is_table_internal(c, styles);
}

static void
collect_rows_recurse(const ns_node *n, GHashTable *styles, GPtrArray *out, int depth)
{
    if (!n || depth >= NS_LAYOUT_MAX_DEPTH) return;
    if (n->kind == NS_NODE_ELEMENT && n->name) {
        const ns_style *rs = g_hash_table_lookup(styles, n);
        if (rs && style_is_none(rs)) return;
        if (rs && style_is_absolute_or_fixed(rs)) {
            queue_absolute_node(n, rs);
            return;
        }
        if (is_table_row(n, styles)) {
            g_ptr_array_add(out, (gpointer)n);
            return;
        }
        if (n != NULL && (strcmp(n->name, "table") == 0 ||
                          style_display_is_table_caption(g_hash_table_lookup(styles, n)) ||
                          style_display_is_table(g_hash_table_lookup(styles, n))))
            return;
        if (node_is_row_group(n, styles)) {
            for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
                if (row_group_child_needs_anonymous_row(c, styles))
                    g_ptr_array_add(out, (gpointer)c);
                else
                    collect_rows_recurse(c, styles, out, depth + 1);
            }
            return;
        }
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        collect_rows_recurse(c, styles, out, depth + 1);
}

typedef enum { ROW_GROUP_BODY, ROW_GROUP_HEADER, ROW_GROUP_FOOTER } row_group_kind;

static row_group_kind
node_row_group(const ns_node *n, GHashTable *styles)
{
    if (ns_node_is_element_named(n, "thead")) return ROW_GROUP_HEADER;
    if (ns_node_is_element_named(n, "tfoot")) return ROW_GROUP_FOOTER;
    const ns_style *s = styles ? g_hash_table_lookup(styles, n) : NULL;
    ns_display d = ns_css_display_of(s);
    if (ns_display_is(d, NS_DISPLAY_INTERNAL_TABLE_HEADER_GROUP))
        return ROW_GROUP_HEADER;
    if (ns_display_is(d, NS_DISPLAY_INTERNAL_TABLE_FOOTER_GROUP))
        return ROW_GROUP_FOOTER;
    return ROW_GROUP_BODY;
}

static void
collect_rows(const ns_node *table, GHashTable *styles, GPtrArray *out)
{
    if (!table) return;
    GPtrArray *header = g_ptr_array_new();
    GPtrArray *body   = g_ptr_array_new();
    GPtrArray *footer = g_ptr_array_new();
    for (const ns_node *c = table->first_child; c; c = c->next_sibling) {
        row_group_kind g = node_row_group(c, styles);
        GPtrArray *dst = g == ROW_GROUP_HEADER ? header
                       : g == ROW_GROUP_FOOTER ? footer
                                               : body;
        collect_rows_recurse(c, styles, dst, 0);
    }
    for (guint i = 0; i < header->len; i++)
        g_ptr_array_add(out, g_ptr_array_index(header, i));
    for (guint i = 0; i < body->len; i++)
        g_ptr_array_add(out, g_ptr_array_index(body, i));
    for (guint i = 0; i < footer->len; i++)
        g_ptr_array_add(out, g_ptr_array_index(footer, i));
    g_ptr_array_free(header, TRUE);
    g_ptr_array_free(body, TRUE);
    g_ptr_array_free(footer, TRUE);
}

static ns_box *build_block(const ns_node *n, GHashTable *styles);
static void layout_box(ns_box *box, double parent_content_width,
                       const ns_style *inherited_style);
static ns_box *build_inline_run(const ns_node *first, const ns_node *last_excl, GHashTable *styles);
static void translate_subtree(ns_box *box, double dx, double dy);
static ns_box *build_inline_run_no_abs_placeholders(const ns_node *first, const ns_node *last_excl, GHashTable *styles);
static gboolean inline_atomic_needs_layout(const ns_box *ab);
static ns_box *build_pseudo_inline_for(const ns_style *ps, const ns_node *host,
                                       const ns_style *host_style);
static ns_box *build_pseudo_block_for(const ns_style *ps, const ns_node *host);
static void append_node_children(ns_box *block, const ns_node *n,
                                 GHashTable *styles,
                                 gboolean blockify_children,
                                 ns_box **pending_before);
static void register_abs_pseudo(const ns_node *host, const ns_style *ps);
static ns_box *build_blockified_inline_item(const ns_node *n, GHashTable *styles,
                                            ns_box **pending_before);
static void layout_block(ns_box *box, double parent_content_width, const ns_style *inherited_style);
static void append_display_contents_children(ns_box *block, const ns_node *n,
                                             GHashTable *styles,
                                             gboolean blockify_children,
                                             ns_box **pending_before);

static gboolean
button_has_replaced_child_depth(const ns_node *n, int depth)
{
    if (depth >= NS_LAYOUT_MAX_DEPTH) return FALSE;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            (strcmp(c->name, "svg") == 0 || strcmp(c->name, "img") == 0 ||
             strcmp(c->name, "audio") == 0 ||
             strcmp(c->name, "video") == 0))
            return TRUE;
        if (c->kind == NS_NODE_ELEMENT &&
            button_has_replaced_child_depth(c, depth + 1))
            return TRUE;
    }
    return FALSE;
}

static gboolean
button_has_replaced_child(const ns_node *n)
{
    return button_has_replaced_child_depth(n, 0);
}

static gboolean
style_has_atomic_inline_box(const ns_style *s)
{
    if (!s) return FALSE;
    const ns_css_value *w = s->values[NS_CSS_WIDTH];
    const ns_css_value *h = s->values[NS_CSS_HEIGHT];
    gboolean has_w = w && (w->kind == NS_CSS_V_LENGTH || w->kind == NS_CSS_V_CALC);
    gboolean has_h = h && (h->kind == NS_CSS_V_LENGTH || h->kind == NS_CSS_V_CALC);
    if (has_w || has_h) return TRUE;
    if (s->values[NS_CSS_BACKGROUND_IMAGE] &&
        (s->values[NS_CSS_BACKGROUND_IMAGE]->kind == NS_CSS_V_URL ||
         s->values[NS_CSS_BACKGROUND_IMAGE]->kind == NS_CSS_V_GRADIENT))
        return TRUE;
    if (s->values[NS_CSS_BORDER_RADIUS] ||
        s->values[NS_CSS_BORDER_TOP_LEFT_RADIUS] ||
        s->values[NS_CSS_BORDER_TOP_RIGHT_RADIUS] ||
        s->values[NS_CSS_BORDER_BOTTOM_RIGHT_RADIUS] ||
        s->values[NS_CSS_BORDER_BOTTOM_LEFT_RADIUS])
        return TRUE;
    const ns_css_prop widths[4] = {
        NS_CSS_BORDER_TOP_WIDTH,
        NS_CSS_BORDER_RIGHT_WIDTH,
        NS_CSS_BORDER_BOTTOM_WIDTH,
        NS_CSS_BORDER_LEFT_WIDTH,
    };
    const ns_css_prop styles_p[4] = {
        NS_CSS_BORDER_TOP_STYLE,
        NS_CSS_BORDER_RIGHT_STYLE,
        NS_CSS_BORDER_BOTTOM_STYLE,
        NS_CSS_BORDER_LEFT_STYLE,
    };
    for (int i = 0; i < 4; i++) {
        if (length_or(s->values[widths[i]], 0) <= 0) continue;
        const ns_css_value *st = s->values[styles_p[i]];
        if (!keyword_is(st, "none") && !keyword_is(st, "hidden"))
            return TRUE;
    }
    const ns_css_value *bg = s->values[NS_CSS_BACKGROUND_COLOR];
    return bg && bg->kind == NS_CSS_V_COLOR && bg->u.color.a > 0;
}

static gboolean
is_atomic_inline(const ns_node *n, GHashTable *styles)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    const char *nm = n->name;
    if (strcmp(nm, "button") == 0) {
        const ns_style *bs = styles ? g_hash_table_lookup(styles, n) : NULL;
        if (display_is_atomic_inline_container(ns_css_display_of(bs)) ||
            style_has_atomic_inline_box(bs))
            return TRUE;
        if (button_has_replaced_child(n)) return TRUE;
        const ns_css_value *bw = bs ? bs->values[NS_CSS_WIDTH]  : NULL;
        const ns_css_value *bh = bs ? bs->values[NS_CSS_HEIGHT] : NULL;
        return (bw && (bw->kind == NS_CSS_V_LENGTH || bw->kind == NS_CSS_V_CALC)) ||
               (bh && (bh->kind == NS_CSS_V_LENGTH || bh->kind == NS_CSS_V_CALC));
    }
    if (strcmp(nm, "a") == 0 || strcmp(nm, "label") == 0 ||
        strcmp(nm, "summary") == 0) {
        const ns_style *s = styles ? g_hash_table_lookup(styles, n) : NULL;
        ns_display d = ns_css_display_of(s);
        return display_is_atomic_inline_container(d) ||
               ns_display_is_flex_container(d) ||
               ns_display_is_grid_container(d);
    }
    if (strcmp(nm, "img") == 0 || strcmp(nm, "svg") == 0 ||
        strcmp(nm, "audio") == 0 || strcmp(nm, "video") == 0 ||
        strcmp(nm, "math") == 0 || strcmp(nm, "canvas") == 0)
        return is_inline_level_replaced(n, styles);
    if (strcmp(nm, "input") == 0 || strcmp(nm, "textarea") == 0 ||
        strcmp(nm, "select") == 0 ||
        strcmp(nm, "progress") == 0 || strcmp(nm, "meter") == 0) {
        const ns_style *s = styles ? g_hash_table_lookup(styles, n) : NULL;
        gboolean atomic_display =
            display_is_atomic_inline_container(ns_css_display_of(s));
        if (atomic_display && strcmp(nm, "input") == 0) {
            const char *type = ns_element_get_attr(n, "type");
            if (type && (g_ascii_strcasecmp(type, "radio") == 0 ||
                         g_ascii_strcasecmp(type, "checkbox") == 0)) {
                const ns_css_value *ap = s ? s->values[NS_CSS_APPEARANCE] : NULL;
                const ns_css_value *w = s ? s->values[NS_CSS_WIDTH]  : NULL;
                const ns_css_value *h = s ? s->values[NS_CSS_HEIGHT] : NULL;
                gboolean styled =
                    keyword_is(ap, "none") ||
                    (w && (w->kind == NS_CSS_V_LENGTH || w->kind == NS_CSS_V_CALC)) ||
                    (h && (h->kind == NS_CSS_V_LENGTH || h->kind == NS_CSS_V_CALC));
                if (!styled) return FALSE;
            }
        }
        return atomic_display;
    }
    if (strcmp(nm, "br") == 0 || strcmp(nm, "wbr") == 0)
        return FALSE;
    const ns_style *s = styles ? g_hash_table_lookup(styles, n) : NULL;
    if (!s) return FALSE;
    return display_is_atomic_inline_container(ns_css_display_of(s));
}
static const ns_node *g_focused_input_for_layout;
static const ns_node *g_open_select_for_layout;
static gboolean       g_focused_is_contenteditable_for_layout;
static gsize          g_focused_caret_byte_for_layout;
static gsize          g_focused_sel_anchor_byte_for_layout;
static gboolean       g_datalist_open_for_layout;

void
ns_layout_set_open_select(const ns_node *select)
{
    g_open_select_for_layout = select;
}

void
ns_layout_set_datalist_open(gboolean open)
{
    g_datalist_open_for_layout = open;
}

char *
ns_vertical_stack_text(const char *text)
{
    if (!text) return g_strdup("");
    GString *out = g_string_new(NULL);
    for (const char *p = text; *p; ) {
        const char *next = g_utf8_next_char(p);
        if (out->len) g_string_append_c(out, '\n');
        g_string_append_len(out, p, (gssize)(next - p));
        p = next;
    }
    return g_string_free(out, FALSE);
}
static struct ns_image_cache *g_image_cache_for_layout;
static const char    *g_base_url_for_layout;
static GHashTable    *g_counters_for_layout;
static ns_box *ns_layout_build_(const ns_node *doc, GHashTable *styles, double viewport_width);

typedef struct ns_subgrid_cols {
    int n;
    double x[NS_CSS_TRACKS_MAX + 1];
    double sizes[NS_CSS_TRACKS_MAX];
    double gap;
} ns_subgrid_cols;
static const ns_subgrid_cols *g_pending_subgrid_cols;

typedef struct ns_subgrid_rows {
    int n;
    double y[NS_CSS_TRACKS_MAX + 1];
    double sizes[NS_CSS_TRACKS_MAX];
    double gap;
} ns_subgrid_rows;
static const ns_subgrid_rows *g_pending_subgrid_rows;

static gboolean
style_columns_are_subgrid(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_GRID_TEMPLATE_COLUMNS] : NULL;
    return v && v->kind == NS_CSS_V_TRACKS && v->u.tracks.subgrid;
}

static gboolean
style_rows_are_subgrid(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_GRID_TEMPLATE_ROWS] : NULL;
    return v && v->kind == NS_CSS_V_TRACKS && v->u.tracks.subgrid;
}

typedef struct ns_table_col_hint {
    const ns_style *style;
    int span;
} ns_table_col_hint;

typedef struct ns_abs_entry {
    const ns_node *dom;
    const ns_style *pseudo;
    gboolean       fixed;
} ns_abs_entry;

typedef struct ns_abs_static {
    ns_box *run;
    double  rel_x;
    double  rel_y;
    gsize   byte_off;
} ns_abs_static;

enum {
    NS_LAYOUT_DATA_IMAGE_BUDGET = 64ULL * 1024ULL * 1024ULL,
};

static GArray      *g_abs_pending;
static gboolean     g_abs_force_build;
static const ns_node *g_form_control_inline;
static GHashTable  *g_abs_ph_set;
static double       g_inline_atomic_cb_w;
static GHashTable  *g_abs_static;
static GHashTable  *g_abs_seen;

static void
queue_absolute_node(const ns_node *n, const ns_style *s)
{
    if (!g_abs_pending ||
        (g_abs_seen && !g_hash_table_add(g_abs_seen, (gpointer)n)))
        return;
    const ns_css_value *pv = s->values[NS_CSS_POSITION];
    ns_abs_entry e;
    e.dom = n;
    e.pseudo = NULL;
    e.fixed = pv && pv->kind == NS_CSS_V_KEYWORD && pv->u.keyword &&
              strcmp(pv->u.keyword, "fixed") == 0;
    g_array_append_val(g_abs_pending, e);
}
static const ns_node *g_inline_skip_node;
static const ns_node *g_pseudo_blocks_host;
static gboolean g_pseudo_block_before;
static gboolean g_pseudo_block_after;
static int          g_inline_collect_depth;

static void *
collect_peek_image(const char *src)
{
    if (!src || !g_image_cache_for_layout) return NULL;
    char *abs = g_base_url_for_layout
        ? ns_url_resolve(g_base_url_for_layout, src)
        : NULL;
    void *img = ns_image_cache_peek(g_image_cache_for_layout,
                                    abs ? abs : src);
    g_free(abs);
    return img;
}

static const char *
style_first_bg_url(const ns_style *s)
{
    for (const ns_css_value *l = s ? s->values[NS_CSS_BACKGROUND_IMAGE] : NULL;
         l; l = l->next_layer)
        if (l->kind == NS_CSS_V_URL && l->u.url) return l->u.url;
    return NULL;
}

static void
collect_box_bg_image(ns_box *box, const ns_style *s)
{
    const ns_css_value *mi = s ? s->values[NS_CSS_LIST_STYLE_IMAGE] : NULL;
    if (mi && mi->kind == NS_CSS_V_URL && mi->u.url &&
        box->dom && ns_node_is_element_named(box->dom, "li")) {
        ns_box_media *m = ns_box_media_ensure(box);
        m->marker_image_src = g_strdup(mi->u.url);
        m->marker_image = collect_peek_image(m->marker_image_src);
    }
    const ns_css_value *bi = ns_css_border_image_source(s);
    if (bi && bi->kind == NS_CSS_V_URL && bi->u.url) {
        ns_box_media *m = ns_box_media_ensure(box);
        m->border_image_src = g_strdup(bi->u.url);
        m->border_image = collect_peek_image(m->border_image_src);
    }
    const ns_css_value *bg = s ? s->values[NS_CSS_BACKGROUND_IMAGE] : NULL;
    gboolean any_url = FALSE;
    for (const ns_css_value *l = bg; l; l = l->next_layer)
        if (l->kind == NS_CSS_V_URL && l->u.url) { any_url = TRUE; break; }
    const ns_css_value *mask = NULL;
    for (const ns_css_value *l = s ? s->values[NS_CSS_MASK_IMAGE] : NULL; l;
         l = l->next_layer)
        if (l->kind == NS_CSS_V_URL && l->u.url) { mask = l; break; }
    if (mask) {
        ns_box_media *mm = ns_box_media_ensure(box);
        mm->mask_image_src = g_strdup(mask->u.url);
        mm->mask_image = collect_peek_image(mm->mask_image_src);
    }
    if (!any_url) return;
    ns_box_media *m = ns_box_media_ensure(box);
    if (bg->next_layer) {
        m->bg_layer_srcs = g_ptr_array_new_with_free_func(g_free);
        m->bg_layer_images = g_ptr_array_new();
        for (const ns_css_value *l = bg; l; l = l->next_layer) {
            char *src = (l->kind == NS_CSS_V_URL && l->u.url)
                        ? g_strdup(l->u.url) : NULL;
            g_ptr_array_add(m->bg_layer_srcs, src);
            g_ptr_array_add(m->bg_layer_images, collect_peek_image(src));
        }
    }
    for (const ns_css_value *l = bg; l; l = l->next_layer) {
        if (l->kind != NS_CSS_V_URL || !l->u.url) continue;
        m->bg_image_src = g_strdup(l->u.url);
        m->bg_image = collect_peek_image(m->bg_image_src);
        break;
    }
}

static void
inline_append_attrs(ns_box *last, const ns_box *gen, gsize offset)
{
    if (!gen->attrs || !gen->attrs->len) return;
    if (!last->attrs)
        last->attrs = g_array_new(FALSE, FALSE, sizeof(ns_inline_attr));
    for (guint i = 0; i < gen->attrs->len; i++) {
        ns_inline_attr a = g_array_index(gen->attrs, ns_inline_attr, i);
        a.start += offset;
        g_array_append_val(last->attrs, a);
    }
}

static void
inline_take_atomics(ns_box *last, ns_box *gen, gsize offset)
{
    if (!gen->inline_atomics) return;
    if (!last->inline_atomics)
        last->inline_atomics = g_array_new(FALSE, FALSE, sizeof(ns_inline_atomic));
    for (guint i = 0; i < gen->inline_atomics->len; i++) {
        ns_inline_atomic ia = g_array_index(gen->inline_atomics,
                                            ns_inline_atomic, i);
        ia.byte_off += offset;
        if (ia.box) ia.box->parent = last;
        g_array_append_val(last->inline_atomics, ia);
    }
    g_array_free(gen->inline_atomics, TRUE);
    gen->inline_atomics = NULL;
}

static void
append_generated_after(ns_box *block, ns_box *gen)
{
    ns_box *last = block->first_child;
    while (last && last->next_sibling) last = last->next_sibling;
    if (!last || last->kind != NS_BOX_INLINE) {
        box_append_child(block, gen);
        return;
    }
    gsize ll = last->text ? strlen(last->text) : 0;
    gsize gl = gen->text  ? strlen(gen->text)  : 0;
    if (ll > G_MAXSIZE - gl - 1) { ns_box_free(gen); return; }
    char *combined = g_malloc(ll + gl + 1);
    if (ll) memcpy(combined, last->text, ll);
    if (gl) memcpy(combined + ll, gen->text, gl);
    combined[ll + gl] = '\0';
    g_free(last->text);
    last->text = combined;
    inline_append_attrs(last, gen, ll);
    inline_take_atomics(last, gen, ll);
    ns_box_free(gen);
}

static gboolean pseudo_generates_box(const ns_style *ps);

static ns_box *
abs_pseudo_placeholder_run(const ns_box *block, const ns_style *ps)
{
    if (!g_abs_ph_set || !pseudo_generates_box(ps)) return NULL;
    const ns_box *last = block->first_child;
    while (last && last->next_sibling) last = last->next_sibling;
    if (!last || last->kind != NS_BOX_INLINE || !last->text || !*last->text)
        return NULL;
    ns_box *ph = box_new(NS_BOX_BLOCK);
    ns_box *run = box_new_inline();
    run->text = g_strdup("\xef\xbf\xbc");
    run->inline_atomics = g_array_new(FALSE, FALSE, sizeof(ns_inline_atomic));
    ns_inline_atomic ia = { .byte_off = 0, .box = ph };
    ph->parent = run;
    g_array_append_val(run->inline_atomics, ia);
    g_hash_table_insert(g_abs_ph_set, ph, (gpointer)ps);
    return run;
}

static ns_box *
cell_pseudo_before(ns_box *cell, const ns_node *n, const ns_style *s)
{
    if (!s || !s->before) return NULL;
    ns_box *before_block = build_pseudo_block_for(s->before, n);
    if (before_block) {
        box_append_child(cell, before_block);
        return NULL;
    }
    if (style_is_absolute_or_fixed(s->before)) return NULL;
    return build_pseudo_inline_for(s->before, n, s);
}

static void
cell_pseudo_after(ns_box *cell, const ns_node *n, const ns_style *s)
{
    if (!s || !s->after) return;
    ns_box *after_block = build_pseudo_block_for(s->after, n);
    if (after_block) {
        box_append_child(cell, after_block);
        return;
    }
    if (style_is_absolute_or_fixed(s->after)) return;
    ns_box *gen = build_pseudo_inline_for(s->after, n, s);
    if (gen) append_generated_after(cell, gen);
}

static void
cell_append_run(ns_box *cell, const ns_node *start, const ns_node *end,
                GHashTable *styles, ns_box **pending_before)
{
    if (inline_range_waits_for_next(start, end, styles)) return;
    ns_box *run = inline_merge_prefix(*pending_before,
                                      build_inline_run(start, end, styles));
    *pending_before = NULL;
    if (run->text && run->text[0] != '\0')
        box_append_child(cell, run);
    else
        ns_box_free(run);
}

static void
flush_pending_before(ns_box *parent, ns_box **pending_before,
                     const ns_node *next, GHashTable *styles)
{
    if (!*pending_before || node_is_out_of_flow(next, styles)) return;
    box_append_child(parent, *pending_before);
    *pending_before = NULL;
}

static ns_box *
build_cell(const ns_node *n, GHashTable *styles)
{
    ns_box *cell = box_new(NS_BOX_TABLE_CELL);
    cell->dom = n;
    cell->style = g_hash_table_lookup(styles, n);
    collect_box_bg_image(cell, cell->style);
    const char *cs_attr = ns_element_get_attr(n, "colspan");
    if (cs_attr) cell->colspan = ns_parse_int(cs_attr, 1, 1, 100);
    const char *rs_attr = ns_element_get_attr(n, "rowspan");
    if (rs_attr) cell->rowspan = ns_parse_int(rs_attr, 1, 1, 100);
    const ns_style *s = cell->style;
    if (s) {
        register_abs_pseudo(n, s->before);
        register_abs_pseudo(n, s->after);
    }
    ns_box *pending_before = cell_pseudo_before(cell, n, s);
    const ns_node *c = n->first_child;
    while (c) {
        if (continues_inline_run(c, styles)) {
            const ns_node *start = c;
            while (c && continues_inline_run(c, styles)) c = c->next_sibling;
            cell_append_run(cell, start, c, styles, &pending_before);
        } else {
            flush_pending_before(cell, &pending_before, c, styles);
            ns_box *child = build_block(c, styles);
            if (child) box_append_child(cell, child);
            if (c) c = c->next_sibling;
        }
    }
    flush_pending_before(cell, &pending_before, NULL, styles);
    cell_pseudo_after(cell, n, s);
    return cell;
}

static void
append_table_col_hint(ns_box *table, const ns_node *n, GHashTable *styles,
                      int span, const ns_style *fallback)
{
    if (span < 1) span = 1;
    if (!table->table_col_hints)
        table->table_col_hints = g_array_new(FALSE, FALSE,
                                             sizeof(ns_table_col_hint));
    const ns_style *s = styles ? g_hash_table_lookup(styles, n) : NULL;
    if (!s) s = fallback;
    ns_table_col_hint hint = { .style = s, .span = span };
    g_array_append_val(table->table_col_hints, hint);
}

static void
collect_table_col_hints(ns_box *table, const ns_node *n, GHashTable *styles)
{
    if (!table || !n) return;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "col") == 0) {
            int span = ns_parse_int(ns_element_get_attr(c, "span"), 1, 1, 1000);
            append_table_col_hint(table, c, styles, span, NULL);
            continue;
        }
        if (strcmp(c->name, "colgroup") != 0) continue;
        const ns_style *group_style = styles ? g_hash_table_lookup(styles, c) : NULL;
        gboolean saw_col = FALSE;
        for (const ns_node *col = c->first_child; col; col = col->next_sibling) {
            if (!ns_node_is_element_named(col, "col")) continue;
            int span = ns_parse_int(ns_element_get_attr(col, "span"), 1, 1, 1000);
            append_table_col_hint(table, col, styles, span, group_style);
            saw_col = TRUE;
        }
        if (!saw_col) {
            int span = ns_parse_int(ns_element_get_attr(c, "span"), 1, 1, 1000);
            append_table_col_hint(table, c, styles, span, NULL);
        }
    }
}

static ns_box *
build_table_caption(const ns_node *n, GHashTable *styles)
{
    ns_box *caption = box_new(NS_BOX_TABLE_CAPTION);
    caption->dom = n;
    caption->style = g_hash_table_lookup(styles, n);
    collect_box_bg_image(caption, caption->style);
    const ns_style *s = caption->style;
    if (s) {
        register_abs_pseudo(n, s->before);
        register_abs_pseudo(n, s->after);
    }
    ns_box *inline_before = cell_pseudo_before(caption, n, s);
    if (inline_before) box_append_child(caption, inline_before);
    append_node_children(caption, n, styles, FALSE, NULL);
    cell_pseudo_after(caption, n, s);
    return caption;
}

static ns_box *
build_anonymous_table_cell_range(const ns_node *first, const ns_node *end,
                                 GHashTable *styles)
{
    ns_box *cell = box_new(NS_BOX_TABLE_CELL);
    gboolean any = FALSE;
    const ns_node *c = first;
    while (c && c != end) {
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            tag_is_non_rendering(c->name)) {
            c = c->next_sibling;
            continue;
        }
        if (is_table_caption(c, styles)) {
            c = c->next_sibling;
            continue;
        }
        if (c->kind == NS_NODE_TEXT && text_is_ws_only(c->text)) {
            c = c->next_sibling;
            continue;
        }
        if (c->kind == NS_NODE_ELEMENT) {
            const ns_style *cs = g_hash_table_lookup(styles, c);
            if (cs && style_is_none(cs)) {
                c = c->next_sibling;
                continue;
            }
            if (style_is_contents(cs)) {
                append_display_contents_children(cell, c, styles, FALSE, NULL);
                if (cell->last_child) any = TRUE;
                c = c->next_sibling;
                continue;
            }
        }
        if (is_inline_dom(c, styles)) {
            const ns_node *start = c;
            c = c->next_sibling;
            while (c && c != end && !is_table_caption(c, styles)) {
                if (c->kind == NS_NODE_ELEMENT && c->name &&
                    tag_is_non_rendering(c->name)) {
                    c = c->next_sibling;
                    continue;
                }
                if (c->kind == NS_NODE_ELEMENT) {
                    const ns_style *cs = g_hash_table_lookup(styles, c);
                    if (style_is_contents(cs)) break;
                }
                if (!continues_inline_run(c, styles)) break;
                c = c->next_sibling;
            }
            ns_box *run = build_inline_run(start, c, styles);
            if (run && run->text && run->text[0]) {
                box_append_child(cell, run);
                any = TRUE;
            } else if (run) {
                ns_box_free(run);
            }
        } else {
            ns_box *child = build_block(c, styles);
            if (child) {
                box_append_child(cell, child);
                any = TRUE;
            }
            if (c) c = c->next_sibling;
        }
    }
    if (!any) {
        ns_box_free(cell);
        return NULL;
    }
    return cell;
}

static ns_box *
build_anonymous_table_cell(const ns_node *n, GHashTable *styles)
{
    return build_anonymous_table_cell_range(n ? n->first_child : NULL, NULL,
                                            styles);
}

static gboolean
row_children_need_anonymous_cell(const ns_node *first, const ns_node *end,
                                 GHashTable *styles)
{
    for (const ns_node *c = first; c && c != end; c = c->next_sibling) {
        if (c->kind == NS_NODE_TEXT) {
            if (!text_is_ws_only(c->text)) return TRUE;
            continue;
        }
        if (row_group_child_needs_anonymous_row(c, styles)) return TRUE;
    }
    return FALSE;
}

static ns_box *
build_table_row(const ns_node *tr, GHashTable *styles)
{
    ns_box *row = box_new(NS_BOX_TABLE_ROW);
    if (!is_table_row(tr, styles)) {
        ns_box *child = build_block(tr, styles);
        if (child) {
            ns_box *cell = box_new(NS_BOX_TABLE_CELL);
            box_append_child(cell, child);
            box_append_child(row, cell);
        }
        return row;
    }
    row->dom = tr;
    row->style = g_hash_table_lookup(styles, tr);
    const ns_node *c = tr->first_child;
    while (c) {
        if (is_cell_element(c, styles)) {
            if (!style_is_none(g_hash_table_lookup(styles, c)))
                box_append_child(row, build_cell(c, styles));
            c = c->next_sibling;
            continue;
        }
        const ns_node *start = c;
        while (c && !is_cell_element(c, styles)) c = c->next_sibling;
        if (row_children_need_anonymous_cell(start, c, styles)) {
            ns_box *cell = build_anonymous_table_cell_range(start, c, styles);
            if (cell) box_append_child(row, cell);
        }
    }
    return row;
}

static ns_box *
build_row_from_cell_run(const ns_node *start, const ns_node *end,
                        const ns_node *dom, GHashTable *styles)
{
    ns_box *row = box_new(NS_BOX_TABLE_ROW);
    row->dom = dom;
    for (const ns_node *c = start; c != end; c = c->next_sibling) {
        if (!is_cell_element(c, styles)) continue;
        const ns_style *cs = g_hash_table_lookup(styles, c);
        if (cs && (style_is_none(cs) || style_is_absolute_or_fixed(cs)))
            continue;
        box_append_child(row, build_cell(c, styles));
    }
    if (row->first_child) return row;
    ns_box_free(row);
    return NULL;
}

static ns_box *
build_table(const ns_node *n, GHashTable *styles)
{
    ns_box *table = box_new(NS_BOX_TABLE);
    table->dom = n;
    table->style = g_hash_table_lookup(styles, n);
    collect_table_col_hints(table, n, styles);
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (!is_table_caption(c, styles)) continue;
        ns_box *caption = build_table_caption(c, styles);
        box_append_child(table, caption);
    }
    GPtrArray *rows = g_ptr_array_new();
    collect_rows(n, styles, rows);
    gboolean has_direct_cells = FALSE;
    if (rows->len == 0)
        for (const ns_node *c = n->first_child; c; c = c->next_sibling)
            if (is_cell_element(c, styles)) { has_direct_cells = TRUE; break; }
    if (rows->len == 0 && has_direct_cells) {
        ns_box *row = build_row_from_cell_run(n->first_child, NULL, n, styles);
        if (row) box_append_child(table, row);
    } else if (rows->len == 0) {
        ns_box *cell = build_anonymous_table_cell(n, styles);
        if (cell) {
            ns_box *row = box_new(NS_BOX_TABLE_ROW);
            row->dom = n;
            box_append_child(row, cell);
            box_append_child(table, row);
        }
    } else {
        for (guint i = 0; i < rows->len; i++)
            box_append_child(table,
                             build_table_row(g_ptr_array_index(rows, i),
                                             styles));
    }
    g_ptr_array_free(rows, TRUE);
    return table;
}

static void
flush_anonymous_cell_run(ns_box *table, const ns_node **cells,
                         const ns_node *end, GHashTable *styles)
{
    if (!*cells) return;
    ns_box *row = build_row_from_cell_run(*cells, end, NULL, styles);
    if (row) box_append_child(table, row);
    *cells = NULL;
}

static void
append_anonymous_table_rows(ns_box *table, const ns_node *c,
                            GHashTable *styles)
{
    if (is_table_row(c, styles)) {
        box_append_child(table, build_table_row(c, styles));
        return;
    }
    if (!node_is_row_group(c, styles)) return;
    for (const ns_node *r = c->first_child; r; r = r->next_sibling)
        if (is_table_row(r, styles) ||
            row_group_child_needs_anonymous_row(r, styles))
            box_append_child(table, build_table_row(r, styles));
}

static ns_box *
build_anonymous_table(const ns_node *start, const ns_node *end,
                      GHashTable *styles)
{
    ns_box *table = box_new(NS_BOX_TABLE);
    for (const ns_node *c = start; c != end; c = c->next_sibling)
        if (is_table_caption(c, styles))
            box_append_child(table, build_table_caption(c, styles));

    const ns_node *cells = NULL;
    for (const ns_node *c = start; c != end; c = c->next_sibling) {
        if (is_cell_element(c, styles)) {
            if (!cells) cells = c;
            continue;
        }
        if (node_is_table_run_gap(c, styles)) continue;
        flush_anonymous_cell_run(table, &cells, c, styles);
        append_anonymous_table_rows(table, c, styles);
    }
    flush_anonymous_cell_run(table, &cells, end, styles);

    if (table->first_child) return table;
    ns_box_free(table);
    return NULL;
}

typedef struct collector_ctx {
    GHashTable *styles;
    const char *active_href;
    const char *active_target;
    const ns_node *active_link_node;
    GString    *out;
    GArray     *links;
    GArray     *attrs;
    int  bold_depth;
    int  italic_depth;
    int  mono_depth;
    int  underline_depth;
    int  overline_depth;
    int  strike_depth;
    int  q_depth;
    gsize bold_start;
    gsize italic_start;
    gsize mono_start;
    gsize underline_start;
    gsize overline_start;
    gsize strike_start;
    const char *text_transform;
    GArray     *atomics;
    gboolean    abs_placeholders;
    GArray     *ws_ranges;
    const ns_node *ws_parent;
    int         ws_parent_mode;
    double      rise;
} collector_ctx;

typedef struct ns_ws_range {
    gsize start, end;
    int mode;
} ns_ws_range;

typedef struct ns_atomic_raw {
    gsize start;
    ns_box *box;
    double rise;
} ns_atomic_raw;

static void
collector_note_ws(collector_ctx *ctx, const ns_node *parent, gsize start)
{
    if (!ctx->ws_ranges || !parent || ctx->out->len <= start) return;
    if (parent != ctx->ws_parent) {
        ctx->ws_parent = parent;
        ctx->ws_parent_mode = white_space_mode(parent, ctx->styles);
    }
    ns_ws_range r = { .start = start, .end = ctx->out->len,
                      .mode = ctx->ws_parent_mode };
    g_array_append_val(ctx->ws_ranges, r);
}

static void
append_inline_spacer(collector_ctx *ctx, double width)
{
    if (!(width > 0)) return;
    gsize start = ctx->out->len;
    g_string_append(ctx->out, "\xef\xbf\xbc");
    ns_inline_attr spacer = {
        .kind = NS_INLINE_SPACER,
        .start = start,
        .len = 3,
        .box_w = width,
    };
    g_array_append_val(ctx->attrs, spacer);
}

static double
inline_box_side_px(const ns_style *s, ns_css_prop padding,
                   ns_css_prop border_width, ns_css_prop border_style)
{
    if (!s) return 0;
    double side = length_or(s->values[padding], 0);
    if (s->values[border_style])
        side += border_side_width(s, border_width, border_style);
    return side;
}

static int
control_pad_spaces(const ns_style *s, ns_css_prop prop)
{
    if (!s) return 0;
    double fs = length_or(s->values[NS_CSS_FONT_SIZE], 16);
    if (!(fs > 0)) fs = 16;
    double pad = length_resolve(s->values[prop], fs * 20.0, 0);
    double space = fs * 0.25;
    if (!(space > 0) || pad <= space) return 0;
    int n = (int)((pad - space) / space + 0.5);
    if (n < 0) n = 0;
    if (n > 12) n = 12;
    return n;
}

static int
text_input_leading_spaces(const ns_style *s)
{
    return control_pad_spaces(s, NS_CSS_PADDING_LEFT);
}

static GHashTable *g_input_columns_for_layout;

static int
text_input_size_attr(const ns_node *n)
{
    const char *size_str = ns_element_get_attr(n, "size");
    return size_str ? ns_parse_int(size_str, 20, 4, 80) : 20;
}

static int
text_input_columns(const ns_node *n)
{
    int size = text_input_size_attr(n);
    if (!g_input_columns_for_layout) return size;
    gpointer fitted = g_hash_table_lookup(g_input_columns_for_layout, n);
    return fitted ? MIN(GPOINTER_TO_INT(fitted), 4096) : size;
}

static gboolean
text_input_align_rtl(const ns_style *s, const ns_node *n)
{
    for (const ns_node *p = n; p; p = p->parent) {
        if (p->kind != NS_NODE_ELEMENT) continue;
        const char *dir = ns_element_get_attr(p, "dir");
        if (!dir) continue;
        if (g_ascii_strcasecmp(dir, "rtl") == 0) return TRUE;
        if (g_ascii_strcasecmp(dir, "ltr") == 0) return FALSE;
    }
    return keyword_is(s ? s->values[NS_CSS_DIRECTION] : NULL, "rtl");
}

static glong
text_input_align_offset(const ns_style *s, const ns_node *n, glong span)
{
    if (span <= 0) return 0;
    const ns_css_value *ta = s ? s->values[NS_CSS_TEXT_ALIGN] : NULL;
    gboolean rtl = text_input_align_rtl(s, n);
    if (keyword_is(ta, "center"))
        return span / 2;
    if (keyword_is(ta, "right") ||
        (keyword_is(ta, "end") && !rtl) ||
        (keyword_is(ta, "start") && rtl) ||
        (!ta && rtl))
        return span;
    return 0;
}

static glong
text_input_align_skip_cps(const ns_style *s, const ns_node *n,
                          glong cps, glong size)
{
    if (cps <= size) return 0;
    return text_input_align_offset(s, n, cps - size);
}

static glong
text_input_focused_skip_cps(const char *value, gsize caret_byte,
                            glong cps, glong size)
{
    if (!value || cps <= size) return 0;
    glong max_skip = cps - size;
    glong caret_cp = g_utf8_pointer_to_offset(value, value + caret_byte);
    glong skip = caret_cp > size ? caret_cp - size : 0;
    if (skip > max_skip) skip = max_skip;
    if (skip < 0) skip = 0;
    return skip;
}

static const char *
text_input_advance_cps(const char *p, glong cps)
{
    for (glong i = 0; p && *p && i < cps; i++)
        p = g_utf8_next_char(p);
    return p ? p : "";
}

static gboolean
name_in(const char *name, const char *const *set)
{
    if (!name) return FALSE;
    for (; *set; set++) if (strcmp(name, *set) == 0) return TRUE;
    return FALSE;
}

static gboolean
tag_is_bold(const char *name)
{
    static const char *const set[] = { "b", "strong", NULL };
    return name_in(name, set);
}

static gboolean
tag_is_italic(const char *name)
{
    static const char *const set[] = { "i", "em", "cite", "dfn", NULL };
    return name_in(name, set);
}

static gboolean
tag_is_monospace(const char *name)
{
    static const char *const set[] = { "code", "tt", "kbd", "samp", "pre", NULL };
    return name_in(name, set);
}

static gboolean
tag_is_underline(const char *name)
{
    static const char *const set[] = { "u", "ins", NULL };
    return name_in(name, set);
}

static gboolean
tag_is_strike(const char *name)
{
    static const char *const set[] = { "s", "del", "strike", NULL };
    return name_in(name, set);
}

static gboolean
tag_is_non_rendering(const char *name)
{
    static const char *const set[] = {
        "area", "base", "head", "link", "meta", "noscript", "param",
        "script", "style", "template", "title", NULL,
    };
    return name_in(name, set);
}

static gboolean
node_is_non_rendering(const ns_node *n)
{
    if (!n || !tag_is_non_rendering(n->name)) return FALSE;
    if (n->name && g_ascii_strcasecmp(n->name, "noscript") == 0) {
        const ns_node *root = ns_node_root(n);
        if (root && (root->flags & NS_NODE_SCRIPTING_DISABLED))
            return FALSE;
    }
    return TRUE;
}

static void
emit_attr(GArray *attrs, ns_inline_attr_kind k, gsize start, gsize end)
{
    if (end <= start) return;
    ns_inline_attr a = { .kind = k, .start = start, .len = end - start };
    g_array_append_val(attrs, a);
}

static const ns_style *
inline_parent_box_style(const ns_node *n, GHashTable *styles)
{
    for (const ns_node *p = n->parent; p; p = p->parent) {
        const ns_style *ps = g_hash_table_lookup(styles, p);
        if (!ps || !style_is_contents(ps)) return ps;
    }
    return NULL;
}

static double
inline_text_edge_rise(const ns_style *s, const ns_style *ps, gboolean top)
{
    ns_css_font_metrics own = { 0 }, parent = { 0 };
    if (!ns_paint_style_font_metrics(s, &own) ||
        !ns_paint_style_font_metrics(ps, &parent))
        return 0;
    double content = own.ascent_px + own.descent_px;
    double lh = ns_paint_css_line_height_px(s);
    if (lh < 0) lh = content;
    double ascent = own.ascent_px + floor((lh - content) / 2.0);
    return top ? parent.ascent_px - ascent : lh - ascent - parent.descent_px;
}

static double
inline_vertical_align_length(const ns_css_value *va, const ns_style *s)
{
    double lh = MAX(0.0, ns_paint_css_line_height_px(s));
    if (ns_css_calc_is_math_fn(va)) return ns_css_calc_math_fn_px(va, lh);
    if (va->kind == NS_CSS_V_CALC)
        return va->u.calc.px + va->u.calc.pct * lh / 100.0;
    if (va->u.length.unit == NS_CSS_UNIT_PERCENT)
        return va->u.length.v * lh / 100.0;
    return ns_css_dimension_px(va, length_or(s->values[NS_CSS_FONT_SIZE], 16),
                               lh);
}

static double
vertical_align_script_rise(const ns_style *s, const ns_style *ps)
{
    const ns_css_value *va = s ? s->values[NS_CSS_VERTICAL_ALIGN] : NULL;
    double fs = length_or(ps ? ps->values[NS_CSS_FONT_SIZE] : NULL, 16);
    if (keyword_is(va, "super")) return fs / 3.0 + 1.0;
    if (keyword_is(va, "sub")) return -(fs / 5.0 + 1.0);
    return 0;
}

static double
atomic_vertical_align_rise(const ns_node *n, const ns_style *s,
                           GHashTable *styles)
{
    return vertical_align_script_rise(s, inline_parent_box_style(n, styles));
}

static double
inline_vertical_align_rise(const ns_node *n, const ns_style *s,
                           GHashTable *styles)
{
    const ns_css_value *va = s ? s->values[NS_CSS_VERTICAL_ALIGN] : NULL;
    if (!va || style_is_contents(s)) return 0;
    if (va->kind == NS_CSS_V_LENGTH || va->kind == NS_CSS_V_CALC)
        return inline_vertical_align_length(va, s);
    const ns_style *ps = inline_parent_box_style(n, styles);
    if (keyword_is(va, "text-top")) return inline_text_edge_rise(s, ps, TRUE);
    if (keyword_is(va, "text-bottom"))
        return inline_text_edge_rise(s, ps, FALSE);
    return vertical_align_script_rise(s, ps);
}

static void
emit_rise_attr(collector_ctx *ctx, gsize start, double own_rise)
{
    if (own_rise == 0 || ctx->out->len <= start) return;
    ns_inline_attr a = {
        .kind = NS_INLINE_RISE, .start = start,
        .len = ctx->out->len - start, .rise_px = ctx->rise,
    };
    g_array_append_val(ctx->attrs, a);
}

static void
emit_attr_styled(GArray *attrs, ns_inline_attr_kind k, gsize start, gsize end,
                 const ns_style *style)
{
    if (end <= start) return;
    ns_inline_attr a = { .kind = k, .start = start, .len = end - start,
                         .style = style };
    g_array_append_val(attrs, a);
}

static gboolean
inline_run_at_line_start(const GString *out)
{
    if (out->len == 0) return TRUE;
    gsize i = out->len;
    while (i > 0) {
        if ((guchar)out->str[i - 1] == ' ') { i--; continue; }
        if (i >= 2 && (guchar)out->str[i - 2] == 0xc2 &&
            (guchar)out->str[i - 1] == 0xa0) { i -= 2; continue; }
        break;
    }
    if (i == 0) return TRUE;
    if (i >= 3 && (guchar)out->str[i - 3] == 0xe2 &&
        (guchar)out->str[i - 2] == 0x80 &&
        ((guchar)out->str[i - 1] == 0xa8 || (guchar)out->str[i - 1] == 0xa9))
        return TRUE;
    if ((guchar)out->str[i - 1] == '\n') return TRUE;
    return FALSE;
}

double
ns_control_dim_px(const ns_css_value *v, double font_size, double basis)
{
    if (!v) return 0;
    if (v->kind == NS_CSS_V_CALC) {
        double out = v->u.calc.px;
        if (basis > 0) out += v->u.calc.pct * basis / 100.0;
        return out > 0 ? out : 0;
    }
    if (v->kind != NS_CSS_V_LENGTH) return 0;
    switch (v->u.length.unit) {
    case NS_CSS_UNIT_PX:
    case NS_CSS_UNIT_NUMBER:
        return v->u.length.v;
    case NS_CSS_UNIT_EM:
        return v->u.length.v * font_size;
    case NS_CSS_UNIT_REM:
        return v->u.length.v * 16.0;
    case NS_CSS_UNIT_PERCENT:
        return basis > 0 ? v->u.length.v * basis / 100.0 : 0;
    case NS_CSS_UNIT_VW:
        return v->u.length.v * ns_css_viewport_w() / 100.0;
    case NS_CSS_UNIT_VH:
        return v->u.length.v * ns_css_viewport_h() / 100.0;
    case NS_CSS_UNIT_VMIN:
        return v->u.length.v * MIN(ns_css_viewport_w(), ns_css_viewport_h()) / 100.0;
    case NS_CSS_UNIT_VMAX:
        return v->u.length.v * MAX(ns_css_viewport_w(), ns_css_viewport_h()) / 100.0;
    case NS_CSS_UNIT_CQW:
        return v->u.length.v * (ns_css_container_w() > 0 ? ns_css_container_w() : ns_css_viewport_w()) / 100.0;
    case NS_CSS_UNIT_CQH:
        return v->u.length.v * (ns_css_container_h() > 0 ? ns_css_container_h() : ns_css_viewport_h()) / 100.0;
    case NS_CSS_UNIT_CQMIN: {
        double cw = ns_css_container_w() > 0 ? ns_css_container_w() : ns_css_viewport_w();
        double ch = ns_css_container_h() > 0 ? ns_css_container_h() : ns_css_viewport_h();
        return v->u.length.v * MIN(cw, ch) / 100.0;
    }
    case NS_CSS_UNIT_CQMAX: {
        double cw = ns_css_container_w() > 0 ? ns_css_container_w() : ns_css_viewport_w();
        double ch = ns_css_container_h() > 0 ? ns_css_container_h() : ns_css_viewport_h();
        return v->u.length.v * MAX(cw, ch) / 100.0;
    }
    case NS_CSS_UNIT_EX:
    case NS_CSS_UNIT_CH:
        return v->u.length.v * font_size * 0.5;
    case NS_CSS_UNIT_CAP:
        return v->u.length.v * font_size * 0.7;
    case NS_CSS_UNIT_IC:
        return v->u.length.v * font_size;
    case NS_CSS_UNIT_LH:
        return v->u.length.v * font_size * 1.5;
    case NS_CSS_UNIT_RLH:
        return v->u.length.v * 24.0;
    case NS_CSS_UNIT_REX:
    case NS_CSS_UNIT_RCH:
        return v->u.length.v * 8.0;
    case NS_CSS_UNIT_RCAP:
        return v->u.length.v * 11.2;
    case NS_CSS_UNIT_RIC:
        return v->u.length.v * 16.0;
    }
    return 0;
}

static double
control_dim_px_clamped(const ns_style *s, ns_css_prop value_prop,
                       ns_css_prop min_prop, ns_css_prop max_prop,
                       double font_size, double basis)
{
    if (!s) return 0;
    double out = ns_control_dim_px(s->values[value_prop], font_size, basis);
    double mn = ns_control_dim_px(s->values[min_prop], font_size, basis);
    double mx = ns_control_dim_px(s->values[max_prop], font_size, basis);
    if (mn > 0 && out > 0 && out < mn) out = mn;
    if (mx > 0 && out > mx) out = mx;
    return out;
}

static gboolean
control_is_border_box(const ns_node *dom, const ns_style *s)
{
    if (s && keyword_is(s->values[NS_CSS_BOX_SIZING], "border-box"))
        return TRUE;
    if (s && keyword_is(s->values[NS_CSS_BOX_SIZING], "content-box"))
        return FALSE;
    if (!dom || !dom->name) return FALSE;
    if (strcmp(dom->name, "button") == 0 || strcmp(dom->name, "select") == 0)
        return TRUE;
    if (strcmp(dom->name, "input") != 0) return FALSE;
    const char *type = ns_element_get_attr(dom, "type");
    if (!type || !*type) return FALSE;
    return g_ascii_strcasecmp(type, "submit") == 0 ||
           g_ascii_strcasecmp(type, "reset") == 0 ||
           g_ascii_strcasecmp(type, "button") == 0 ||
           g_ascii_strcasecmp(type, "checkbox") == 0 ||
           g_ascii_strcasecmp(type, "radio") == 0 ||
           g_ascii_strcasecmp(type, "color") == 0 ||
           g_ascii_strcasecmp(type, "search") == 0;
}

double
ns_control_css_extra_w(const ns_node *dom, const ns_style *s)
{
    if (!s || control_is_border_box(dom, s)) return 0;
    ns_edges m, p, b;
    edges_from_style(s, 0, &m, &p, &b);
    return p.left + p.right + b.left + b.right;
}

double
ns_control_css_extra_h(const ns_node *dom, const ns_style *s)
{
    if (!s || control_is_border_box(dom, s)) return 0;
    ns_edges m, p, b;
    edges_from_style(s, 0, &m, &p, &b);
    return p.top + p.bottom + b.top + b.bottom;
}

double
ns_inline_attr_control_width(const ns_inline_attr *r, const ns_box *box)
{
    if (!r || !r->style) return r && r->box_w > 0 ? r->box_w : 0;
    double fs = length_or(r->style->values[NS_CSS_FONT_SIZE], 16);
    double w = control_dim_px_clamped(r->style, NS_CSS_WIDTH,
                                      NS_CSS_MIN_WIDTH, NS_CSS_MAX_WIDTH,
                                      fs, box ? box->content_width : 0);
    if (w > 0) w += ns_control_css_extra_w(r->dom, r->style);
    return w > 0 ? w : r->box_w;
}

static gboolean
style_has_visible_control_box(const ns_style *s)
{
    if (!s) return FALSE;
    const ns_css_value *bg = s->values[NS_CSS_BACKGROUND_COLOR];
    if (bg && bg->kind == NS_CSS_V_COLOR && bg->u.color.a > 0)
        return TRUE;
    if (s->values[NS_CSS_BOX_SHADOW] &&
        s->values[NS_CSS_BOX_SHADOW]->kind == NS_CSS_V_SHADOW &&
        s->values[NS_CSS_BOX_SHADOW]->u.shadow.n > 0)
        return TRUE;
    if (s->values[NS_CSS_BACKGROUND_IMAGE] &&
        (s->values[NS_CSS_BACKGROUND_IMAGE]->kind == NS_CSS_V_URL ||
         s->values[NS_CSS_BACKGROUND_IMAGE]->kind == NS_CSS_V_GRADIENT))
        return TRUE;
    if (length_or(s->values[NS_CSS_BORDER_RADIUS], 0) > 0 ||
        length_or(s->values[NS_CSS_BORDER_TOP_LEFT_RADIUS], 0) > 0 ||
        length_or(s->values[NS_CSS_BORDER_TOP_RIGHT_RADIUS], 0) > 0 ||
        length_or(s->values[NS_CSS_BORDER_BOTTOM_RIGHT_RADIUS], 0) > 0 ||
        length_or(s->values[NS_CSS_BORDER_BOTTOM_LEFT_RADIUS], 0) > 0)
        return TRUE;
    const ns_css_prop widths[4] = {
        NS_CSS_BORDER_TOP_WIDTH,
        NS_CSS_BORDER_RIGHT_WIDTH,
        NS_CSS_BORDER_BOTTOM_WIDTH,
        NS_CSS_BORDER_LEFT_WIDTH,
    };
    const ns_css_prop styles_p[4] = {
        NS_CSS_BORDER_TOP_STYLE,
        NS_CSS_BORDER_RIGHT_STYLE,
        NS_CSS_BORDER_BOTTOM_STYLE,
        NS_CSS_BORDER_LEFT_STYLE,
    };
    for (int i = 0; i < 4; i++) {
        if (length_or(s->values[widths[i]], 0) <= 0) continue;
        const ns_css_value *st = s->values[styles_p[i]];
        if (!keyword_is(st, "none") && !keyword_is(st, "hidden"))
            return TRUE;
    }
    return FALSE;
}

static gboolean
control_style_strips_chrome(const ns_style *s)
{
    static const ns_css_prop styles_p[4] = {
        NS_CSS_BORDER_TOP_STYLE, NS_CSS_BORDER_RIGHT_STYLE,
        NS_CSS_BORDER_BOTTOM_STYLE, NS_CSS_BORDER_LEFT_STYLE,
    };
    static const ns_css_prop widths_p[4] = {
        NS_CSS_BORDER_TOP_WIDTH, NS_CSS_BORDER_RIGHT_WIDTH,
        NS_CSS_BORDER_BOTTOM_WIDTH, NS_CSS_BORDER_LEFT_WIDTH,
    };
    if (!s) return FALSE;
    (void)widths_p;
    for (int i = 0; i < 4; i++) {
        const ns_css_value *st = s->values[styles_p[i]];
        if (!keyword_is(st, "none") && !keyword_is(st, "hidden"))
            return FALSE;
    }
    return TRUE;
}

static gboolean
control_prefers_css_chrome(ns_inline_attr_kind k, const ns_node *dom,
                           const ns_style *s)
{
    if (!dom || !s) return FALSE;
    if (keyword_is(s->values[NS_CSS_APPEARANCE], "none"))
        return TRUE;
    if (control_style_strips_chrome(s)) return TRUE;
    if (style_has_visible_control_box(s)) return TRUE;
    if (!ns_element_get_attr(dom, "class")) return FALSE;
    ns_display d = ns_css_display_of(s);
    if (ns_display_is_block_level(d) || display_is_atomic_inline_container(d))
        return k == NS_INLINE_INPUT_FIELD ||
               k == NS_INLINE_INPUT_FIELD_FOCUSED ||
               k == NS_INLINE_BUTTON;
    return FALSE;
}

static void
emit_form_attr_sized(GArray *attrs, ns_inline_attr_kind k, gsize start, gsize end,
                     const ns_node *dom, GHashTable *styles)
{
    if (end <= start) return;
    double bw = 0, bh = 0;
    const ns_style *s = NULL;
    double fs = 16;
    const char *bg_image_src = NULL;
    if (dom && styles) {
        s = g_hash_table_lookup(styles, dom);
        if (s) {
            fs = length_or(s->values[NS_CSS_FONT_SIZE], 16);
            bw = control_dim_px_clamped(s, NS_CSS_WIDTH,
                                        NS_CSS_MIN_WIDTH, NS_CSS_MAX_WIDTH,
                                        fs, 0);
            bh = control_dim_px_clamped(s, NS_CSS_HEIGHT,
                                        NS_CSS_MIN_HEIGHT, NS_CSS_MAX_HEIGHT,
                                        fs, 0);
            if (bw > 0) bw += ns_control_css_extra_w(dom, s);
            if (bh > 0) bh += ns_control_css_extra_h(dom, s);
            bg_image_src = style_first_bg_url(s);
        }
    }
    if (dom && dom->name && strcmp(dom->name, "textarea") == 0) {
        if (bw <= 0) {
            const char *cols = ns_element_get_attr(dom, "cols");
            int c = cols ? atoi(cols) : 20;
            if (c <= 0) c = 20;
            bw = c * (fs * 0.5) + 8.0;
        }
        if (bh <= 0) {
            const char *rows = ns_element_get_attr(dom, "rows");
            int r = rows ? atoi(rows) : 2;
            if (r <= 0) r = 2;
            double line_h = fs * 1.3;
            bh = r * line_h + 6.0;
        }
    }
    ns_inline_attr a = {
        .kind = k, .start = start, .len = end - start, .dom = dom,
        .box_w = bw, .box_h = bh,
        .native_chrome = !control_prefers_css_chrome(k, dom, s),
        .style = s,
        .bg_image_src = bg_image_src,
        .bg_image = collect_peek_image(bg_image_src),
    };
    g_array_append_val(attrs, a);
}

static double select_widest_label(const ns_node *n, const ns_style *s);

static void
select_size_to_widest_option(collector_ctx *ctx, const ns_node *n,
                             gsize start)
{
    if (!ctx->attrs || ctx->attrs->len == 0) return;
    ns_inline_attr *field = &g_array_index(ctx->attrs, ns_inline_attr,
                                           ctx->attrs->len - 1);
    if (field->dom != n || field->kind != NS_INLINE_INPUT_FIELD) return;
    gsize end = ctx->out->len;
    const char *text = ctx->out->str;
    gsize arrow = end;
    for (gsize i = start; i + 3 <= end; i++)
        if (memcmp(text + i, "\xe2\x96\xbe", 3) == 0 ||
            memcmp(text + i, "\xe2\x96\xb4", 3) == 0)
            arrow = i;
    if (arrow < end) {
        ns_inline_attr hide = {
            .kind = NS_INLINE_COLOR, .start = arrow, .len = 3,
            .r = 0, .g = 0, .b = 0, .a = 0,
        };
        g_array_append_val(ctx->attrs, hide);
        field = &g_array_index(ctx->attrs, ns_inline_attr,
                               ctx->attrs->len - 2);
    }
    if (field->box_w > 0 || !field->style) return;
    double widest = select_widest_label(n, field->style);
    if (widest > 0) field->box_w = widest;
}

static void
emit_control_text_style(GArray *attrs, const ns_style *s,
                        gsize field_start, gsize field_end,
                        gsize val_start, gsize val_end,
                        gboolean is_placeholder, gboolean disabled)
{
    if (field_end > field_start) {
        double ifs = s ? length_or(s->values[NS_CSS_FONT_SIZE], 0) : 0;
        if (ifs > 0) {
            ns_inline_attr a = {
                .kind = NS_INLINE_FONT_SIZE,
                .start = field_start, .len = field_end - field_start,
                .font_size_px = ifs,
            };
            g_array_append_val(attrs, a);
        }
        const ns_css_value *ffam = s ? s->values[NS_CSS_FONT_FAMILY] : NULL;
        if (ffam && ffam->kind == NS_CSS_V_KEYWORD && ffam->u.keyword) {
            ns_inline_attr a = {
                .kind = NS_INLINE_FONT_FAMILY,
                .start = field_start, .len = field_end - field_start,
                .family = ffam->u.keyword,
            };
            g_array_append_val(attrs, a);
        }
    }
    if (val_end <= val_start) return;
    const ns_style *phs = (s && is_placeholder) ? s->placeholder : NULL;
    guint8 cr = 0, cg = 0, cb = 0, ca = 255;
    gboolean have_color = FALSE;
    if (is_placeholder) {
        const ns_css_value *pc = phs ? phs->values[NS_CSS_COLOR] : NULL;
        if (pc && pc->kind == NS_CSS_V_COLOR) {
            cr = pc->u.color.r; cg = pc->u.color.g;
            cb = pc->u.color.b; ca = pc->u.color.a;
        } else {
            cr = cg = cb = 0x75;
        }
        have_color = TRUE;
    } else {
        const ns_css_value *cv = s ? s->values[NS_CSS_COLOR] : NULL;
        if (cv && cv->kind == NS_CSS_V_COLOR) {
            cr = cv->u.color.r; cg = cv->u.color.g;
            cb = cv->u.color.b; ca = cv->u.color.a;
            have_color = TRUE;
        }
    }
    if (disabled) { cr = cg = cb = 0x82; ca = 255; have_color = TRUE; }
    if (phs) {
        const ns_css_value *ov = phs->values[NS_CSS_OPACITY];
        if (ov && ov->kind == NS_CSS_V_LENGTH)
            ca = (guint8)lround(ca * CLAMP(ov->u.length.v, 0.0, 1.0));
        if (keyword_is(phs->values[NS_CSS_VISIBILITY], "hidden") ||
            keyword_is(phs->values[NS_CSS_VISIBILITY], "collapse"))
            ca = 0;
    }
    if (have_color) {
        ns_inline_attr a = {
            .kind = NS_INLINE_COLOR,
            .start = val_start, .len = val_end - val_start,
            .r = cr, .g = cg, .b = cb, .a = ca,
        };
        g_array_append_val(attrs, a);
    }
    if (phs) {
        if (keyword_is(phs->values[NS_CSS_FONT_STYLE], "italic") ||
            keyword_is(phs->values[NS_CSS_FONT_STYLE], "oblique"))
            emit_attr(attrs, NS_INLINE_ITALIC, val_start, val_end);
        int phw = ns_css_font_weight_number(phs->values[NS_CSS_FONT_WEIGHT], -1);
        if (phw > 0) {
            ns_inline_attr a = {
                .kind = NS_INLINE_FONT_WEIGHT,
                .start = val_start, .len = val_end - val_start,
                .font_weight = phw,
            };
            g_array_append_val(attrs, a);
        }
    }
}

static void
emit_font_size_attr(GArray *attrs, gsize start, gsize end, double font_size_px)
{
    if (end <= start) return;
    ns_inline_attr a = { .kind = NS_INLINE_FONT_SIZE, .start = start,
                         .len = end - start, .font_size_px = font_size_px };
    g_array_append_val(attrs, a);
}

static double
style_letter_spacing_px(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_LETTER_SPACING] : NULL;
    return v && v->kind == NS_CSS_V_LENGTH && v->u.length.unit == NS_CSS_UNIT_PX
        ? v->u.length.v : 0;
}

static void
emit_letter_spacing_attrs(GArray *attrs, const GString *out, gsize start,
                          gsize end, double letter_spacing_px)
{
    static const char placeholder[] = "\xef\xbf\xbc";
    gsize piece = start;
    for (gsize i = start; i + 3 <= end; i++) {
        if (letter_spacing_px == 0 || memcmp(out->str + i, placeholder, 3) != 0)
            continue;
        if (i > piece) {
            ns_inline_attr a = { .kind = NS_INLINE_LETTER_SPACING,
                                 .start = piece, .len = i - piece,
                                 .letter_spacing_px = letter_spacing_px };
            g_array_append_val(attrs, a);
        }
        ns_inline_attr none = { .kind = NS_INLINE_LETTER_SPACING,
                                .start = i, .len = 3 };
        g_array_append_val(attrs, none);
        piece = i + 3;
        i += 2;
    }
    if (end > piece) {
        ns_inline_attr a = { .kind = NS_INLINE_LETTER_SPACING, .start = piece,
                             .len = end - piece,
                             .letter_spacing_px = letter_spacing_px };
        g_array_append_val(attrs, a);
    }
}

static void
emit_font_weight_attr(GArray *attrs, gsize start, gsize end, int font_weight)
{
    if (end <= start || font_weight <= 0) return;
    ns_inline_attr a = { .kind = NS_INLINE_FONT_WEIGHT, .start = start,
                         .len = end - start, .font_weight = font_weight };
    g_array_append_val(attrs, a);
}

static void
emit_font_stretch_attr(GArray *attrs, gsize start, gsize end, int font_stretch)
{
    if (end <= start) return;
    ns_inline_attr a = { .kind = NS_INLINE_FONT_STRETCH, .start = start,
                         .len = end - start, .font_stretch = font_stretch };
    g_array_append_val(attrs, a);
}

static int
font_kerning_int_from_style(const ns_style *s)
{
    const char *kw = s ? ns_style_keyword(s, NS_CSS_FONT_KERNING) : NULL;
    if (!kw) return -1;
    if (strcmp(kw, "none") == 0) return 0;
    if (strcmp(kw, "normal") == 0 || strcmp(kw, "auto") == 0) return 1;
    return -1;
}

static const char *
font_ligatures_from_style(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_FONT_VARIANT_LIGATURES] : NULL;
    return v && v->kind == NS_CSS_V_KEYWORD ? v->u.keyword : NULL;
}

static const char *
font_feature_settings_from_style(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_FONT_FEATURE_SETTINGS] : NULL;
    return v && v->kind == NS_CSS_V_KEYWORD ? v->u.keyword : NULL;
}

static const char *
font_variation_settings_from_style(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_FONT_VARIATION_SETTINGS] : NULL;
    return v && v->kind == NS_CSS_V_KEYWORD ? v->u.keyword : NULL;
}

static void
emit_font_features_attr(GArray *attrs, gsize start, gsize end,
                        int font_kerning, const char *font_ligatures,
                        const char *font_features)
{
    if (end <= start ||
        (font_kerning < 0 && !font_ligatures && !font_features))
        return;
    ns_inline_attr a = { .kind = NS_INLINE_FONT_FEATURES, .start = start,
                         .len = end - start, .font_kerning = font_kerning,
                         .font_ligatures = font_ligatures,
                         .font_features = font_features };
    g_array_append_val(attrs, a);
}

static void
emit_font_variations_attr(GArray *attrs, gsize start, gsize end,
                          const char *font_variations)
{
    if (end <= start || !font_variations) return;
    ns_inline_attr a = { .kind = NS_INLINE_FONT_VARIATIONS, .start = start,
                         .len = end - start,
                         .font_variations = font_variations };
    g_array_append_val(attrs, a);
}

static void
emit_color_attr(GArray *attrs, gsize start, gsize end,
                guint8 r, guint8 g, guint8 b, guint8 a8)
{
    if (end <= start) return;
    ns_inline_attr a = { .kind = NS_INLINE_COLOR, .start = start,
                         .len = end - start, .r = r, .g = g, .b = b, .a = a8 };
    g_array_append_val(attrs, a);
}

static void
emit_generated_attr(GArray *attrs, gsize start, gsize end, const ns_style *ps)
{
    if (end <= start) return;
    ns_inline_attr a = { .kind = NS_INLINE_GENERATED, .start = start,
                         .len = end - start, .style = ps };
    g_array_append_val(attrs, a);
}

static void
emit_font_family_attr(GArray *attrs, gsize start, gsize end, const char *family)
{
    if (end <= start || !family) return;
    ns_inline_attr a = { .kind = NS_INLINE_FONT_FAMILY, .start = start,
                         .len = end - start, .family = family };
    g_array_append_val(attrs, a);
}

static void
counter_apply_decl(GHashTable *counters, const char *decl, gboolean increment)
{
    if (!decl || !*decl) return;
    const char *p = decl;
    while (*p) {
        while (*p && g_ascii_isspace(*p)) p++;
        if (!*p) break;
        const char *name_s = p;
        while (*p && !g_ascii_isspace(*p)) p++;
        gsize nlen = (gsize)(p - name_s);
        if (nlen == 0) break;
        char *name = g_strndup(name_s, nlen);
        if (strcmp(name, "none") == 0) { g_free(name); break; }
        while (*p && g_ascii_isspace(*p)) p++;
        int val = increment ? 1 : 0;
        if (*p == '-' || g_ascii_isdigit(*p)) {
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end != p) { val = (int)v; p = end; }
        }
        gint cur = increment
            ? GPOINTER_TO_INT(g_hash_table_lookup(counters, name))
            : 0;
        g_hash_table_insert(counters, g_strdup(name),
                            GINT_TO_POINTER(cur + val));
        g_free(name);
    }
}

static gchar *
counter_format(gint v, const char *style)
{
    if (!style || strcmp(style, "decimal") == 0)
        return g_strdup_printf("%d", v);
    if (strcmp(style, "decimal-leading-zero") == 0)
        return g_strdup_printf("%02d", v);
    if (strcmp(style, "lower-roman") == 0 || strcmp(style, "upper-roman") == 0) {
        if (v < 1 || v > 3999) return g_strdup_printf("%d", v);
        static const struct { int n; const char *s; } R[] = {
            {1000,"m"},{900,"cm"},{500,"d"},{400,"cd"},{100,"c"},{90,"xc"},
            {50,"l"},{40,"xl"},{10,"x"},{9,"ix"},{5,"v"},{4,"iv"},{1,"i"}
        };
        GString *s = g_string_new(NULL);
        for (gsize i = 0; i < G_N_ELEMENTS(R); i++)
            while (v >= R[i].n) { g_string_append(s, R[i].s); v -= R[i].n; }
        if (style[0] == 'u') {
            char *up = g_ascii_strup(s->str, -1);
            g_string_free(s, TRUE);
            return up;
        }
        return g_string_free(s, FALSE);
    }
    if (strcmp(style, "lower-alpha") == 0 || strcmp(style, "lower-latin") == 0 ||
        strcmp(style, "upper-alpha") == 0 || strcmp(style, "upper-latin") == 0) {
        if (v < 1) return g_strdup_printf("%d", v);
        char base = (style[0] == 'u') ? 'A' : 'a';
        GString *s = g_string_new(NULL);
        char buf[16]; int n = 0;
        while (v > 0 && n < (int)sizeof(buf)) { v--; buf[n++] = base + (v % 26); v /= 26; }
        while (n > 0) g_string_append_c(s, buf[--n]);
        return g_string_free(s, FALSE);
    }
    if (strcmp(style, "lower-greek") == 0) {
        static const gunichar greek[24] = {
            0x3B1, 0x3B2, 0x3B3, 0x3B4, 0x3B5, 0x3B6, 0x3B7, 0x3B8,
            0x3B9, 0x3BA, 0x3BB, 0x3BC, 0x3BD, 0x3BE, 0x3BF, 0x3C0,
            0x3C1, 0x3C3, 0x3C4, 0x3C5, 0x3C6, 0x3C7, 0x3C8, 0x3C9,
        };
        if (v < 1) return g_strdup_printf("%d", v);
        gunichar bg[16];
        int n = 0, w = v;
        while (w > 0 && n < 16) { w--; bg[n++] = greek[w % 24]; w /= 24; }
        GString *s = g_string_new(NULL);
        while (n > 0) g_string_append_unichar(s, bg[--n]);
        return g_string_free(s, FALSE);
    }
    return g_strdup_printf("%d", v);
}

static void
counter_apply_style(GHashTable *current, const ns_style *s)
{
    if (!s) return;
    if (s->values[NS_CSS_COUNTER_RESET] &&
        s->values[NS_CSS_COUNTER_RESET]->kind == NS_CSS_V_KEYWORD)
        counter_apply_decl(current,
            s->values[NS_CSS_COUNTER_RESET]->u.keyword, FALSE);
    if (s->values[NS_CSS_COUNTER_INCREMENT] &&
        s->values[NS_CSS_COUNTER_INCREMENT]->kind == NS_CSS_V_KEYWORD)
        counter_apply_decl(current,
            s->values[NS_CSS_COUNTER_INCREMENT]->u.keyword, TRUE);
}

static gboolean
pseudo_generates_box(const ns_style *ps)
{
    const ns_css_value *cv = ps ? ps->values[NS_CSS_CONTENT] : NULL;
    return cv && cv->kind == NS_CSS_V_KEYWORD && cv->u.keyword &&
           strcmp(cv->u.keyword, "none") != 0 &&
           strcmp(cv->u.keyword, "normal") != 0;
}

static void
counter_apply_pseudo(GHashTable *current, const ns_style *ps)
{
    if (pseudo_generates_box(ps)) counter_apply_style(current, ps);
}

static void
counter_walk(const ns_node *n, GHashTable *styles,
             GHashTable *current, GHashTable *snapshots, int depth)
{
    if (!n || depth >= NS_LAYOUT_MAX_DEPTH) return;
    const ns_style *s = NULL;
    if (n->kind == NS_NODE_ELEMENT && styles) {
        s = g_hash_table_lookup(styles, n);
        counter_apply_style(current, s);
        if (s) counter_apply_pseudo(current, s->before);
        gboolean need = FALSE;
        if (s && s->before && s->before->values[NS_CSS_CONTENT]) {
            const ns_css_value *cv = s->before->values[NS_CSS_CONTENT];
            if (cv->kind == NS_CSS_V_KEYWORD && cv->u.keyword &&
                strstr(cv->u.keyword, "counter")) need = TRUE;
        }
        if (s && s->after && s->after->values[NS_CSS_CONTENT]) {
            const ns_css_value *cv = s->after->values[NS_CSS_CONTENT];
            if (cv->kind == NS_CSS_V_KEYWORD && cv->u.keyword &&
                strstr(cv->u.keyword, "counter")) need = TRUE;
        }
        if (need) {
            GHashTable *snap = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, NULL);
            GHashTableIter it;
            gpointer k, v;
            g_hash_table_iter_init(&it, current);
            while (g_hash_table_iter_next(&it, &k, &v))
                g_hash_table_insert(snap, g_strdup((const char *)k), v);
            g_hash_table_insert(snapshots, (gpointer)n, snap);
        }
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        counter_walk(c, styles, current, snapshots, depth + 1);
    if (s) counter_apply_pseudo(current, s->after);
}

static void
counter_snapshot_destroy(gpointer p) { g_hash_table_destroy(p); }

static GHashTable *
build_counter_snapshots(const ns_node *root, GHashTable *styles)
{
    GHashTable *snapshots = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, counter_snapshot_destroy);
    GHashTable *current = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    counter_walk(root, styles, current, snapshots, 0);
    g_hash_table_destroy(current);
    return snapshots;
}

static char *
substitute_one_counter(const char *body, const ns_node *host, gboolean is_counters)
{
    const char *p = body;
    while (*p && g_ascii_isspace(*p)) p++;
    const char *name_s = p;
    while (*p && *p != ',' && *p != ')' && !g_ascii_isspace(*p)) p++;
    gsize nlen = (gsize)(p - name_s);
    if (nlen == 0) return g_strdup("");
    char *name = g_strndup(name_s, nlen);
    char *sep = NULL;
    char *style = NULL;
    while (*p == ',' || g_ascii_isspace(*p)) {
        if (*p == ',') p++;
        while (*p && g_ascii_isspace(*p)) p++;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            const char *vs = p;
            while (*p && *p != q) p++;
            char *v = g_strndup(vs, p - vs);
            if (*p) p++;
            if (!sep) sep = v;
            else { g_free(v); }
        } else if (g_ascii_isalpha(*p)) {
            const char *vs = p;
            while (*p && *p != ',' && *p != ')' && !g_ascii_isspace(*p)) p++;
            if (!style) style = g_strndup(vs, p - vs);
        } else {
            break;
        }
    }
    if (is_counters && !sep) sep = g_strdup("");
    GHashTable *snap = g_counters_for_layout
        ? g_hash_table_lookup(g_counters_for_layout, host) : NULL;
    int v = snap ? GPOINTER_TO_INT(g_hash_table_lookup(snap, name)) : 0;
    char *formatted = counter_format(v, style);
    g_free(name); g_free(sep); g_free(style);
    return formatted;
}

static char *
resolve_pseudo_content(const char *raw, const ns_node *host)
{
    if (!raw || !*raw) return NULL;
    if (strcmp(raw, "none") == 0 || strcmp(raw, "normal") == 0) return NULL;
    gboolean has_func = strchr(raw, '(') != NULL;
    gboolean has_string = strchr(raw, '"') || strchr(raw, '\'');
    if (!has_func && !has_string) return g_strdup(raw);
    GString *out = g_string_new(NULL);
    const char *p = raw;
    while (*p) {
        while (*p && g_ascii_isspace(*p)) p++;
        if (!*p || *p == '/') break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            const char *start = p;
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                p++;
            }
            char *raw_str = g_strndup(start, p - start);
            for (const char *r = raw_str; *r; )
                ns_css_append_unescaped(out, &r);
            g_free(raw_str);
            if (*p == q) p++;
        } else if (g_str_has_prefix(p, "attr(")) {
            p += 5;
            while (*p == ' ') p++;
            const char *start = p;
            while (*p && *p != ')' && *p != ',' && *p != ' ') p++;
            if (host && p != start) {
                char *attr_name = g_strndup(start, p - start);
                const char *val = ns_element_get_attr(host, attr_name);
                if (val) g_string_append(out, val);
                g_free(attr_name);
            }
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
        } else if (g_str_has_prefix(p, "counter(") ||
                   g_str_has_prefix(p, "counters(")) {
            gboolean is_counters = (p[7] == 's');
            p += is_counters ? 9 : 8;
            const char *body_s = p;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') { depth--; if (depth == 0) break; }
                p++;
            }
            char *body = g_strndup(body_s, p - body_s);
            if (*p == ')') p++;
            char *sub = substitute_one_counter(body, host, is_counters);
            if (sub) g_string_append(out, sub);
            g_free(sub);
            g_free(body);
        } else {
            const char *id = p;
            while (*p && (g_ascii_isalnum((guchar)*p) || *p == '-')) p++;
            if (*p == '(') {
                int depth = 0;
                for (; *p; p++) {
                    if (*p == '(') depth++;
                    else if (*p == ')') { depth--; if (depth == 0) { p++; break; } }
                }
            } else {
                while (*p && !g_ascii_isspace(*p) && *p != '"' && *p != '\'') p++;
                if (p > id)
                    g_string_append_len(out, id, p - id);
                else
                    p++;
            }
        }
    }
    return g_string_free(out, FALSE);
}

static char *
apply_text_transform(const char *src, const char *tt)
{
    if (!src || !tt) return NULL;
    if (strcmp(tt, "uppercase") == 0)
        return g_utf8_strup(src, -1);
    if (strcmp(tt, "lowercase") == 0)
        return g_utf8_strdown(src, -1);
    if (strcmp(tt, "capitalize") == 0) {
        GString *out = g_string_new(NULL);
        gboolean at_word_start = TRUE;
        for (const char *p = src; p && *p; ) {
            gunichar c = g_utf8_get_char(p);
            const char *next = g_utf8_next_char(p);
            if (g_unichar_isspace(c) || c == '-' || c == '/') {
                g_string_append_len(out, p, next - p);
                at_word_start = TRUE;
            } else {
                if (at_word_start) {
                    gunichar uc = g_unichar_totitle(c);
                    char buf[8];
                    gint nb = g_unichar_to_utf8(uc, buf);
                    g_string_append_len(out, buf, nb);
                    at_word_start = FALSE;
                } else {
                    g_string_append_len(out, p, next - p);
                }
            }
            p = next;
        }
        return g_string_free(out, FALSE);
    }
    return NULL;
}

typedef struct ns_progress_state {
    gboolean determinate;
    double max;
    double value;
    double frac;
} ns_progress_state;

typedef struct ns_meter_state {
    double min;
    double max;
    double value;
    double low;
    double high;
    double optimum;
    double frac;
    int quality;
} ns_meter_state;

static gboolean
parse_float_attr(const ns_node *n, const char *attr, double *out)
{
    return ns_html_parse_float(ns_element_get_attr(n, attr), out);
}

static ns_progress_state
progress_state_for(const ns_node *n)
{
    ns_progress_state st = {0};
    double parsed;
    st.max = parse_float_attr(n, "max", &parsed) && parsed > 0 ? parsed : 1.0;
    st.determinate = ns_element_get_attr(n, "value") != NULL;
    if (st.determinate) {
        st.value = parse_float_attr(n, "value", &parsed) && parsed > 0 ? parsed : 0.0;
        if (st.value > st.max) st.value = st.max;
        st.frac = st.max > 0 ? st.value / st.max : 0;
    } else {
        st.value = 0;
        st.frac = -1;
    }
    return st;
}

static ns_meter_state
meter_state_for(const ns_node *n)
{
    ns_meter_state st = {0};
    double parsed;
    st.min = parse_float_attr(n, "min", &parsed) ? parsed : 0.0;
    double candidate_max = parse_float_attr(n, "max", &parsed) ? parsed : 1.0;
    st.max = candidate_max >= st.min ? candidate_max : st.min;
    double candidate_value = parse_float_attr(n, "value", &parsed) ? parsed : 0.0;
    st.value = candidate_value;
    if (st.value < st.min) st.value = st.min;
    if (st.value > st.max) st.value = st.max;
    double candidate_low = parse_float_attr(n, "low", &parsed) ? parsed : st.min;
    st.low = candidate_low;
    if (st.low < st.min) st.low = st.min;
    if (st.low > st.max) st.low = st.max;
    double candidate_high = parse_float_attr(n, "high", &parsed) ? parsed : st.max;
    st.high = candidate_high;
    if (st.high < st.low) st.high = st.low;
    if (st.high > st.max) st.high = st.max;
    double midpoint = st.min + (st.max - st.min) / 2.0;
    double candidate_optimum = parse_float_attr(n, "optimum", &parsed) ? parsed : midpoint;
    st.optimum = candidate_optimum;
    if (st.optimum < st.min) st.optimum = st.min;
    if (st.optimum > st.max) st.optimum = st.max;
    st.frac = st.max > st.min ? (st.value - st.min) / (st.max - st.min) : 1.0;
    if (st.optimum >= st.low && st.optimum <= st.high)
        st.quality = (st.value >= st.low && st.value <= st.high) ? 0 : 1;
    else if (st.optimum < st.low)
        st.quality = st.value < st.low ? 0 : (st.value <= st.high ? 1 : 2);
    else
        st.quality = st.value > st.high ? 0 : (st.value >= st.low ? 1 : 2);
    return st;
}

static char *
quotes_string_for(const ns_style *s, int depth, gboolean closing)
{
    if (depth < 0) depth = 0;
    const ns_css_value *qv = s ? s->values[NS_CSS_QUOTES] : NULL;
    const char *spec = (qv && qv->kind == NS_CSS_V_KEYWORD) ? qv->u.keyword
                                                            : NULL;
    if (spec && strcmp(spec, "none") == 0) return g_strdup("");
    if (spec && strchr(spec, '"') == NULL && strchr(spec, '\'') == NULL)
        spec = NULL;
    if (spec) {
        GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
        for (const char *p = spec; *p; ) {
            if (*p == '"' || *p == '\'') {
                char term = *p++;
                GString *part = g_string_new(NULL);
                while (*p && *p != term)
                    ns_css_append_unescaped(part, &p);
                g_ptr_array_add(parts, g_string_free(part, FALSE));
                if (*p) p++;
            } else {
                p++;
            }
        }
        guint pairs = parts->len / 2;
        char *result = NULL;
        if (pairs > 0) {
            guint pair = (guint)depth < pairs ? (guint)depth : pairs - 1;
            result = g_strdup(g_ptr_array_index(parts,
                                                pair * 2 + (closing ? 1 : 0)));
        }
        g_ptr_array_free(parts, TRUE);
        if (result) return result;
    }
    if (depth % 2 == 0)
        return g_strdup(closing ? "\xe2\x80\x9d" : "\xe2\x80\x9c");
    return g_strdup(closing ? "\xe2\x80\x99" : "\xe2\x80\x98");
}

char *
ns_layout_pseudo_content_text(const ns_css_value *content, const ns_node *host)
{
    if (!content || content->kind != NS_CSS_V_KEYWORD || !content->u.keyword)
        return NULL;
    return resolve_pseudo_content(content->u.keyword, host);
}

static void
append_pseudo_content(GString *out, const ns_css_value *cv,
                      const ns_node *host, const ns_style *host_style)
{
    if (!cv || cv->kind != NS_CSS_V_KEYWORD || !cv->u.keyword) return;
    char *resolved = resolve_pseudo_content(cv->u.keyword, host);
    if (!resolved) return;
    char *quote = NULL;
    const char *txt = resolved;
    if (strcmp(resolved, "open-quote") == 0)
        txt = quote = quotes_string_for(host_style, 0, FALSE);
    else if (strcmp(resolved, "close-quote") == 0)
        txt = quote = quotes_string_for(host_style, 0, TRUE);
    else if (strcmp(resolved, "no-open-quote") == 0 ||
             strcmp(resolved, "no-close-quote") == 0) { g_free(resolved); return; }
    g_string_append(out, txt);
    g_free(quote);
    g_free(resolved);
}

static ns_box *pseudo_block_with_text(const ns_style *ps, const char *txt);

static void
append_abs_placeholder(collector_ctx *ctx, gconstpointer key)
{
    if (!ctx->atomics || !ctx->abs_placeholders || !g_abs_ph_set) return;
    ns_box *ph = box_new(NS_BOX_BLOCK);
    ns_atomic_raw rec = { .start = ctx->out->len, .box = ph };
    g_string_append(ctx->out, "\xef\xbf\xbc");
    g_array_append_val(ctx->atomics, rec);
    g_hash_table_insert(g_abs_ph_set, ph, (gpointer)key);
}

static void
append_pseudo_inline(collector_ctx *ctx, const ns_style *ps,
                     const ns_node *host)
{
    if (!pseudo_generates_box(ps) || style_is_none(ps)) return;
    if (style_is_absolute_or_fixed(ps)) {
        register_abs_pseudo(host, ps);
        if (!keyword_is(ps->values[NS_CSS_POSITION], "fixed"))
            append_abs_placeholder(ctx, ps);
        return;
    }
    if (ctx->atomics && ns_display_is_atomic_inline(ns_css_display_of(ps))) {
        char *txt = resolve_pseudo_content(
            ps->values[NS_CSS_CONTENT]->u.keyword, host);
        ns_atomic_raw rec = {
            .start = ctx->out->len,
            .box = pseudo_block_with_text(ps, txt),
            .rise = vertical_align_script_rise(
                ps, g_hash_table_lookup(ctx->styles, host)),
        };
        g_free(txt);
        g_string_append(ctx->out, "\xef\xbf\xbc");
        g_array_append_val(ctx->atomics, rec);
        return;
    }
    append_inline_spacer(ctx, length_or(ps->values[NS_CSS_MARGIN_LEFT], 0));
    append_inline_spacer(ctx, length_or(ps->values[NS_CSS_PADDING_LEFT], 0));
    gsize start = ctx->out->len;
    append_pseudo_content(ctx->out, ps->values[NS_CSS_CONTENT], host, ps);
    emit_generated_attr(ctx->attrs, start, ctx->out->len, ps);
    append_inline_spacer(ctx, length_or(ps->values[NS_CSS_PADDING_RIGHT], 0));
    append_inline_spacer(ctx, length_or(ps->values[NS_CSS_MARGIN_RIGHT], 0));
}

static void
emit_open_select_option(collector_ctx *ctx, const ns_node *option)
{
    if (!option) return;
    g_string_append(ctx->out, "\xe2\x80\xa8");
    gsize start = ctx->out->len;
    g_string_append(ctx->out, "\xc2\xa0\xc2\xa0");
    char *t = ns_option_label_dup(option);
    if (t && *t) g_string_append(ctx->out, t);
    g_free(t);
    g_string_append(ctx->out, "\xc2\xa0\xc2\xa0");
    emit_form_attr_sized(ctx->attrs, NS_INLINE_INPUT_FIELD,
                         start, ctx->out->len, option, ctx->styles);
}

static void
emit_listbox_option(collector_ctx *ctx, const ns_node *option, gboolean first)
{
    if (!option) return;
    if (!first) g_string_append(ctx->out, "\xe2\x80\xa8");
    gsize start = ctx->out->len;
    g_string_append(ctx->out, "\xc2\xa0\xc2\xa0");
    char *t = ns_option_label_dup(option);
    if (t && *t) g_string_append(ctx->out, t);
    g_free(t);
    g_string_append(ctx->out, "\xc2\xa0\xc2\xa0");
    emit_form_attr_sized(ctx->attrs, NS_INLINE_INPUT_FIELD,
                         start, ctx->out->len, option, ctx->styles);
}

static const ns_node *
find_datalist_by_id(const ns_node *node, const char *id, int depth)
{
    if (!node || !id || depth >= NS_LAYOUT_MAX_DEPTH) return NULL;
    if (node->kind == NS_NODE_ELEMENT && node->name &&
        strcmp(node->name, "datalist") == 0) {
        const char *did = ns_element_id_attr(node);
        if (did && strcmp(did, id) == 0) return node;
    }
    for (const ns_node *c = node->first_child; c; c = c->next_sibling) {
        const ns_node *m = find_datalist_by_id(c, id, depth + 1);
        if (m) return m;
    }
    return NULL;
}

static char *
datalist_option_value(const ns_node *option)
{
    const char *v = ns_element_get_attr(option, "value");
    if (v && *v) return g_strdup(v);
    return ns_option_label_dup(option);
}

static void
emit_datalist_suggestions(collector_ctx *ctx, const ns_node *input)
{
    const char *list_id = ns_element_get_attr(input, "list");
    if (!list_id || !*list_id) return;
    const ns_node *root = input;
    while (root->parent) root = root->parent;
    const ns_node *dl = find_datalist_by_id(root, list_id, 0);
    if (!dl) return;

    const char *cur = ns_input_used_value(input);
    char *needle = (cur && *cur) ? g_utf8_casefold(cur, -1) : NULL;
    int shown = 0;
    for (const ns_node *o = dl->first_child; o && shown < 8; o = o->next_sibling) {
        if (!ns_node_is_element_named(o, "option")) continue;
        char *val = datalist_option_value(o);
        if (!val || !*val) { g_free(val); continue; }
        gboolean match = TRUE;
        if (needle) {
            char *vl = g_utf8_casefold(val, -1);
            match = strstr(vl, needle) != NULL &&
                    g_ascii_strcasecmp(vl, needle) != 0;
            g_free(vl);
        }
        if (match) {
            g_string_append(ctx->out, "\xe2\x80\xa8");
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0\xc2\xa0");
            g_string_append(ctx->out, val);
            g_string_append(ctx->out, "\xc2\xa0\xc2\xa0");
            emit_form_attr_sized(ctx->attrs, NS_INLINE_INPUT_FIELD,
                                 start, ctx->out->len, o, ctx->styles);
            shown++;
        }
        g_free(val);
    }
    g_free(needle);
}

static void collect_walk(const ns_node *n, collector_ctx *ctx, int depth);

static gboolean
collect_walk_assigned(const ns_node *slot, collector_ctx *ctx, int depth)
{
    if (!slot || !slot->name || strcmp(slot->name, "slot") != 0) return FALSE;
    const ns_node *host = layout_slot_host(slot);
    const ns_node *sr = host ? layout_shadow_root(host) : NULL;
    if (!sr) return FALSE;
    const char *slot_name = ns_element_get_attr(slot, "name");
    gboolean any = FALSE;
    for (const ns_node *lc = host->first_child; lc; lc = lc->next_sibling) {
        if (lc == sr) continue;
        const char *assigned = lc->kind == NS_NODE_ELEMENT
            ? ns_element_get_attr(lc, "slot") : NULL;
        if (g_strcmp0(assigned ? assigned : "", slot_name ? slot_name : "") != 0)
            continue;
        if (lc->kind != NS_NODE_ELEMENT && lc->kind != NS_NODE_TEXT) continue;
        collect_walk(lc, ctx, depth + 1);
        any = TRUE;
    }
    return any;
}

static void
collect_walk(const ns_node *n, collector_ctx *ctx, int depth)
{
    if (!n || depth >= NS_LAYOUT_MAX_DEPTH) return;
    if (node_is_frame_fallback(n)) return;
    if (n->kind == NS_NODE_TEXT) {
        if (!n->text) return;
        gsize start = ctx->out->len;
        if (n->parent && ns_node_is_contenteditable_host(n->parent)) {
            gboolean focused = g_focused_is_contenteditable_for_layout &&
                               n->parent == g_focused_input_for_layout;
            const char *val = n->text;
            gsize vlen = strlen(val);
            gsize cb = g_focused_caret_byte_for_layout;
            gsize ab = g_focused_sel_anchor_byte_for_layout;
            if (cb > vlen) cb = vlen;
            if (ab > vlen) ab = vlen;
            gsize caret_pos = start, anchor_pos = start;
            for (gsize i = 0; i <= vlen; i++) {
                if (i == cb) caret_pos = ctx->out->len;
                if (i == ab) anchor_pos = ctx->out->len;
                if (i == vlen) break;
                char ch = val[i];
                if (ch == '\n') g_string_append(ctx->out, "\xe2\x80\xa8");
                else if (ch != '\r') g_string_append_c(ctx->out, ch);
            }
            if (ctx->active_href) {
                ns_link_range r = {
                    .start = start, .len = ctx->out->len - start,
                    .href  = g_strdup(ctx->active_href),
                    .target = ctx->active_target ? g_strdup(ctx->active_target) : NULL,
                    .dom   = ctx->active_link_node,
                };
                g_array_append_val(ctx->links, r);
            }
            if (focused) {
                g_string_append(ctx->out, "\xc2\xa0");
                if (anchor_pos != caret_pos) {
                    gsize s0 = anchor_pos < caret_pos ? anchor_pos : caret_pos;
                    gsize s1 = anchor_pos < caret_pos ? caret_pos : anchor_pos;
                    emit_attr(ctx->attrs, NS_INLINE_SELECTION, s0, s1);
                }
                emit_attr(ctx->attrs, NS_INLINE_CARET, caret_pos, caret_pos + 1);
            }
            return;
        }
        char *xformed = ctx->text_transform
                        ? apply_text_transform(n->text, ctx->text_transform)
                        : NULL;
        g_string_append(ctx->out, xformed ? xformed : n->text);
        g_free(xformed);
        collector_note_ws(ctx, n->parent, start);
        if (ctx->active_href) {
            ns_link_range r = {
                .start = start,
                .len   = ctx->out->len - start,
                .href  = g_strdup(ctx->active_href),
                .target = ctx->active_target ? g_strdup(ctx->active_target) : NULL,
                .dom   = ctx->active_link_node,
            };
            g_array_append_val(ctx->links, r);
        }
        return;
    }
    if (n->kind != NS_NODE_ELEMENT) return;
    if (node_is_non_rendering(n)) return;
    if (n == g_inline_skip_node) return;
    const ns_style *s = g_hash_table_lookup(ctx->styles, n);
    if (s && style_is_none(s)) return;
    if (s && style_is_absolute_or_fixed(s) && n != g_form_control_inline) {
        const ns_css_value *pv = s->values[NS_CSS_POSITION];
        gboolean fixed = pv && pv->kind == NS_CSS_V_KEYWORD && pv->u.keyword &&
                         strcmp(pv->u.keyword, "fixed") == 0;
        if (g_abs_pending &&
            (!g_abs_seen || g_hash_table_add(g_abs_seen, (gpointer)n))) {
            ns_abs_entry e;
            e.dom = n;
            e.pseudo = NULL;
            e.fixed = fixed;
            g_array_append_val(g_abs_pending, e);
        }
        if (!fixed) append_abs_placeholder(ctx, n);
        return;
    }

    if (ctx->atomics && n != g_form_control_inline &&
        is_atomic_inline(n, ctx->styles)) {
        int saved_collect_depth = g_inline_collect_depth;
        g_inline_collect_depth = depth + 1;
        ns_box *sub = build_block(n, ctx->styles);
        g_inline_collect_depth = saved_collect_depth;
        if (sub) {
            ns_atomic_raw rec = {
                .start = ctx->out->len, .box = sub,
                .rise = atomic_vertical_align_rise(n, s, ctx->styles),
            };
            g_string_append(ctx->out, "\xef\xbf\xbc");
            g_array_append_val(ctx->atomics, rec);
        }
        return;
    }

    if (!n->name) {
        for (const ns_node *c = n->first_child; c; c = c->next_sibling)
            collect_walk(c, ctx, depth + 1);
        return;
    }
    if (strcmp(n->name, "br") == 0) {
        g_string_append(ctx->out, "\xe2\x80\xa8");
        return;
    }
    if (strcmp(n->name, "wbr") == 0) {
        g_string_append(ctx->out, "\xe2\x80\x8b");
        return;
    }
    if (strcmp(n->name, "progress") == 0 || strcmp(n->name, "meter") == 0) {
        gboolean is_meter = strcmp(n->name, "meter") == 0;
        gsize start = ctx->out->len;
        for (int i = 0; i < 12; i++) g_string_append(ctx->out, "\xc2\xa0");
        double frac = 0;
        guint8 r = 0, g = 0, b = 0, alpha = 0;
        if (is_meter) {
            ns_meter_state st = meter_state_for(n);
            frac = st.frac;
            if (st.quality == 0) { r = 0x2e; g = 0x9d; b = 0x54; }
            else if (st.quality == 1) { r = 0xd0; g = 0xa4; b = 0x1f; }
            else { r = 0xc4; g = 0x43; b = 0x3c; }
            alpha = 255;
        } else {
            ns_progress_state st = progress_state_for(n);
            frac = st.frac;
        }
        ns_inline_attr attr = {
            .kind = is_meter ? NS_INLINE_METER : NS_INLINE_PROGRESS,
            .start = start, .len = ctx->out->len - start,
            .font_size_px = frac,
            .r = r, .g = g, .b = b, .a = alpha,
            .dom = n,
        };
        g_array_append_val(ctx->attrs, attr);
        return;
    }
    if (strcmp(n->name, "input") == 0) {
        const char *type = ns_element_get_attr(n, "type");
        gboolean is_password = type && g_ascii_strcasecmp(type, "password") == 0;
        gboolean is_text = !type || !*type ||
                           is_password ||
                           g_ascii_strcasecmp(type, "text") == 0 ||
                           g_ascii_strcasecmp(type, "search") == 0 ||
                           g_ascii_strcasecmp(type, "email") == 0 ||
                           g_ascii_strcasecmp(type, "url") == 0 ||
                           g_ascii_strcasecmp(type, "tel") == 0 ||
                           g_ascii_strcasecmp(type, "number") == 0;
        if (is_text) {
            gboolean boxed = n == g_form_control_inline;
            if (!boxed && !inline_run_at_line_start(ctx->out))
                g_string_append(ctx->out, "\xc2\xa0\xc2\xa0\xc2\xa0");
            gsize start = ctx->out->len;
            if (!boxed) {
                g_string_append(ctx->out, "\xc2\xa0");
                int leading = text_input_leading_spaces(s);
                for (int i = 0; i < leading; i++)
                    g_string_append(ctx->out, "\xc2\xa0");
            }
            const char *real_value = ns_input_used_value(n);
            const char *v = real_value;
            gboolean is_placeholder = FALSE;
            if (!v || !*v) {
                v = ns_element_get_attr(n, "placeholder");
                is_placeholder = (v && *v);
            }
            gboolean focused = (n == g_focused_input_for_layout);
            gsize val_start = ctx->out->len;
            gsize disp_end = val_start;
            gsize caret_pos = val_start;
            gsize anchor_pos = val_start;
            gsize caret_byte = g_focused_caret_byte_for_layout;
            gsize anchor_byte = g_focused_sel_anchor_byte_for_layout;
            if (real_value && caret_byte > strlen(real_value))
                caret_byte = strlen(real_value);
            if (real_value && anchor_byte > strlen(real_value))
                anchor_byte = strlen(real_value);
            else if (!real_value) anchor_byte = 0;
            int size = text_input_columns(n);
            glong displayed_chars = 0;
            if (v && *v && is_password && !is_placeholder) {
                glong cps = g_utf8_strlen(v, -1);
                glong skip_cps = focused
                    ? text_input_focused_skip_cps(real_value, caret_byte,
                                                  cps, size)
                    : text_input_align_skip_cps(s, n, cps, size);
                glong shown_cps = cps - skip_cps;
                if (shown_cps > size) shown_cps = size;
                for (glong i = 0; i < shown_cps; i++)
                    g_string_append(ctx->out, "\xe2\x80\xa2");
                displayed_chars = shown_cps;
                disp_end = ctx->out->len;
                if (focused) {
                    glong cp_before = real_value
                        ? g_utf8_pointer_to_offset(real_value, real_value + caret_byte)
                        : 0;
                    glong cp_in_shown = cp_before > skip_cps
                                        ? cp_before - skip_cps : 0;
                    if (cp_in_shown > shown_cps) cp_in_shown = shown_cps;
                    caret_pos = val_start + (gsize)cp_in_shown * 3;
                }
            } else if (v && *v) {
                const char *display_v = v;
                gsize skip_bytes = 0;
                gsize display_bytes = strlen(v);
                glong value_cps = g_utf8_strlen(v, -1);
                if (value_cps > size) {
                    glong skip_cps = focused && real_value && v == real_value
                        ? text_input_focused_skip_cps(real_value, caret_byte,
                                                      value_cps, size)
                        : text_input_align_skip_cps(s, n, value_cps, size);
                    const char *p = text_input_advance_cps(v, skip_cps);
                    const char *q = text_input_advance_cps(p, size);
                    display_v = p;
                    display_bytes = (gsize)(q - p);
                    if (real_value && v == real_value) {
                        skip_bytes = (gsize)(p - real_value);
                    }
                }
                g_string_append_len(ctx->out, display_v, (gssize)display_bytes);
                displayed_chars = g_utf8_strlen(display_v, (gssize)display_bytes);
                disp_end = ctx->out->len;
                if (focused && real_value && *real_value) {
                    gsize caret_in_shown = caret_byte > skip_bytes
                                           ? caret_byte - skip_bytes : 0;
                    if (caret_in_shown > display_bytes) caret_in_shown = display_bytes;
                    caret_pos = val_start + caret_in_shown;
                    gsize anchor_in_shown = anchor_byte > skip_bytes
                                            ? anchor_byte - skip_bytes : 0;
                    if (anchor_in_shown > display_bytes) anchor_in_shown = display_bytes;
                    anchor_pos = val_start + anchor_in_shown;
                } else if (focused) {
                    caret_pos = val_start;
                    anchor_pos = val_start;
                }
            } else {
                if (focused) { caret_pos = val_start; anchor_pos = val_start; }
            }
            glong pad = (glong)size - displayed_chars;
            if (pad < 0) pad = 0;
            glong left_pad = 0, right_pad = pad;
            if (pad > 0) {
                left_pad = text_input_align_offset(s, n, pad);
                right_pad = pad - left_pad;
            }
            if (left_pad > 0) {
                GString *lp = g_string_new(NULL);
                for (glong i = 0; i < left_pad; i++)
                    g_string_append(lp, "\xc2\xa0");
                g_string_insert_len(ctx->out, val_start, lp->str, lp->len);
                gsize shift = (gsize)lp->len;
                g_string_free(lp, TRUE);
                caret_pos += shift;
                anchor_pos += shift;
                disp_end += shift;
                val_start += shift;
            }
            for (glong i = 0; i < right_pad; i++)
                g_string_append(ctx->out, "\xc2\xa0");
            g_string_append(ctx->out, "\xc2\xa0");
            ns_inline_attr_kind kind = focused
                                       ? NS_INLINE_INPUT_FIELD_FOCUSED
                                       : NS_INLINE_INPUT_FIELD;
            emit_form_attr_sized(ctx->attrs, kind, start, ctx->out->len, n, ctx->styles);
            emit_control_text_style(ctx->attrs, s, start, ctx->out->len,
                                    val_start, disp_end, is_placeholder,
                                    ns_element_get_attr(n, "disabled") != NULL);
            if (!is_placeholder && disp_end > val_start &&
                ns_element_get_attr(n, "disabled") == NULL &&
                ns_node_spellcheck_used(n) && ns_node_spellcheck_armed(n)) {
                ns_inline_attr sc = { .kind = NS_INLINE_SPELLCHECK,
                                      .start = val_start,
                                      .len = disp_end - val_start, .dom = n };
                g_array_append_val(ctx->attrs, sc);
            }
            if (focused) {
                if (anchor_pos != caret_pos) {
                    gsize s0 = anchor_pos < caret_pos ? anchor_pos : caret_pos;
                    gsize s1 = anchor_pos < caret_pos ? caret_pos : anchor_pos;
                    emit_attr(ctx->attrs, NS_INLINE_SELECTION, s0, s1);
                }
                emit_attr(ctx->attrs, NS_INLINE_CARET, caret_pos, caret_pos + 1);
            }
            if (focused && g_datalist_open_for_layout)
                emit_datalist_suggestions(ctx, n);
        } else if (type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                            g_ascii_strcasecmp(type, "button") == 0 ||
                            g_ascii_strcasecmp(type, "reset") == 0)) {
            const char *v = ns_element_get_attr(n, "value");
            gsize start = ctx->out->len;
            if (!v)
                v = g_ascii_strcasecmp(type, "submit") == 0 ? "Submit"
                  : g_ascii_strcasecmp(type, "reset")  == 0 ? "Reset"
                                                            : "Button";
            int lead  = control_pad_spaces(s, NS_CSS_PADDING_LEFT);
            int trail = control_pad_spaces(s, NS_CSS_PADDING_RIGHT);
            g_string_append(ctx->out, "\xc2\xa0");
            for (int i = 0; i < lead; i++)
                g_string_append(ctx->out, "\xc2\xa0");
            g_string_append(ctx->out, v);
            for (int i = 0; i < trail; i++)
                g_string_append(ctx->out, "\xc2\xa0");
            g_string_append(ctx->out, "\xc2\xa0");
            emit_form_attr_sized(ctx->attrs, NS_INLINE_BUTTON, start, ctx->out->len, n, ctx->styles);
            g_string_append_c(ctx->out, ' ');
        } else if (type && g_ascii_strcasecmp(type, "checkbox") == 0) {
            gboolean checked = ns_input_is_checked(n);
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xef\xbf\xbc");
            emit_form_attr_sized(ctx->attrs,
                checked ? NS_INLINE_CHECKBOX_CHECKED : NS_INLINE_CHECKBOX,
                start, ctx->out->len, n, ctx->styles);
        } else if (type && g_ascii_strcasecmp(type, "radio") == 0) {
            gboolean checked = ns_input_is_checked(n);
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xef\xbf\xbc");
            emit_form_attr_sized(ctx->attrs,
                checked ? NS_INLINE_RADIO_CHECKED : NS_INLINE_RADIO,
                start, ctx->out->len, n, ctx->styles);
        } else if (type && g_ascii_strcasecmp(type, "file") == 0) {
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0" "Choose File" "\xc2\xa0");
            emit_form_attr_sized(ctx->attrs, NS_INLINE_BUTTON, start, ctx->out->len, n, ctx->styles);
            g_string_append(ctx->out, " (no file chosen)");
        } else if (type && g_ascii_strcasecmp(type, "color") == 0) {
            const char *v = ns_input_used_value(n);
            if (!v || !*v) v = ns_element_get_attr(n, "value");
            const char *hex = v && *v ? v : "#000000";
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0");
            gsize swatch_start = ctx->out->len;
            for (int i = 0; i < 7; i++)
                g_string_append(ctx->out, "\xc2\xa0");
            gsize swatch_end = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0");
            emit_form_attr_sized(ctx->attrs, NS_INLINE_BUTTON, start, ctx->out->len, n, ctx->styles);
            guint8 r8 = 0, g8 = 0, b8 = 0;
            if (hex[0] == '#' && strlen(hex) >= 7) {
                unsigned int rv, gv, bv;
                if (sscanf(hex + 1, "%2x%2x%2x", &rv, &gv, &bv) == 3) {
                    r8 = (guint8)rv; g8 = (guint8)gv; b8 = (guint8)bv;
                }
            }
            ns_inline_attr swatch = {
                .kind = NS_INLINE_BG_COLOR,
                .start = swatch_start, .len = swatch_end - swatch_start,
                .r = r8, .g = g8, .b = b8, .a = 255,
            };
            g_array_append_val(ctx->attrs, swatch);
        } else if (type && (g_ascii_strcasecmp(type, "range") == 0)) {
            const char *v = ns_input_used_value(n);
            if (!v || !*v) v = ns_element_get_attr(n, "value");
            const char *mn = ns_element_get_attr(n, "min");
            const char *mx = ns_element_get_attr(n, "max");
            double mnv = mn && *mn ? g_ascii_strtod(mn, NULL) : 0;
            double mxv = mx && *mx ? g_ascii_strtod(mx, NULL) : 100;
            if (mxv <= mnv) mxv = mnv + 1;
            double vv = v && *v ? g_ascii_strtod(v, NULL)
                                : mnv + (mxv - mnv) / 2;
            double frac = (vv - mnv) / (mxv - mnv);
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xef\xbf\xbc");
            emit_form_attr_sized(ctx->attrs, NS_INLINE_RANGE, start, ctx->out->len, n, ctx->styles);
            g_array_index(ctx->attrs, ns_inline_attr,
                          ctx->attrs->len - 1).font_size_px = frac;
        } else if (type && (g_ascii_strcasecmp(type, "date") == 0 ||
                            g_ascii_strcasecmp(type, "datetime-local") == 0 ||
                            g_ascii_strcasecmp(type, "time") == 0 ||
                            g_ascii_strcasecmp(type, "month") == 0 ||
                            g_ascii_strcasecmp(type, "week") == 0)) {
            const char *v = ns_element_get_attr(n, "value");
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0");
            if (v && *v) g_string_append(ctx->out, v);
            else         g_string_append(ctx->out, "____-__-__");
            g_string_append(ctx->out, "\xc2\xa0");
            emit_form_attr_sized(ctx->attrs, NS_INLINE_INPUT_FIELD, start, ctx->out->len, n, ctx->styles);
        }
        return;
    }
    if (strcmp(n->name, "button") == 0) {
        char *label = ns_node_collect_text(n);
        if (!label || !*label) {
            g_free(label);
            const ns_style *bs = ctx->styles
                ? g_hash_table_lookup(ctx->styles, n) : NULL;
            GString *pseudo = g_string_new(NULL);
            if (bs && bs->before)
                append_pseudo_content(pseudo, bs->before->values[NS_CSS_CONTENT],
                                      n, bs->before);
            if (bs && bs->after)
                append_pseudo_content(pseudo, bs->after->values[NS_CSS_CONTENT],
                                      n, bs->after);
            if (pseudo->len > 0) {
                label = g_string_free(pseudo, FALSE);
            } else {
                g_string_free(pseudo, TRUE);
                const char *aria = ns_element_get_attr(n, "aria-label");
                const char *title = ns_element_get_attr(n, "title");
                const char *value = ns_element_get_attr(n, "value");
                if (aria && *aria) label = g_strdup(aria);
                else if (title && *title) label = g_strdup(title);
                else if (value && *value) label = g_strdup(value);
                else label = g_strdup("");
            }
        }
        if (!*label) {
            g_free(label);
            return;
        }
        gsize start = ctx->out->len;
        g_string_append(ctx->out, "\xc2\xa0");
        g_string_append(ctx->out, label);
        g_string_append(ctx->out, "\xc2\xa0");
        emit_form_attr_sized(ctx->attrs, NS_INLINE_BUTTON, start, ctx->out->len, n, ctx->styles);
        g_free(label);
        return;
    }
    if (strcmp(n->name, "select") == 0) {
        gboolean multi = ns_element_get_attr(n, "multiple") != NULL;
        const char *size_attr = ns_element_get_attr(n, "size");
        int size_n = size_attr ? ns_parse_int(size_attr, 0, 0, 1000) : 0;
        gboolean listbox = multi || size_n > 1;
        if (listbox) {
            int shown = 0;
            int cap = size_n > 0 ? size_n : 6;
            gboolean first = TRUE;
            for (const ns_node *c = n->first_child; c && shown < cap; c = c->next_sibling) {
                if (c->kind != NS_NODE_ELEMENT || !c->name) continue;
                if (strcmp(c->name, "optgroup") == 0) {
                    const char *gl = ns_element_get_attr(c, "label");
                    if (gl && *gl) {
                        if (!first) g_string_append(ctx->out, "\xe2\x80\xa8");
                        g_string_append_printf(ctx->out, "\xc2\xa0%s\xc2\xa0", gl);
                        first = FALSE;
                        shown++;
                    }
                    for (const ns_node *opt = c->first_child;
                         opt && shown < cap; opt = opt->next_sibling) {
                        if (!ns_node_is_element_named(opt, "option")) continue;
                        emit_listbox_option(ctx, opt, first);
                        first = FALSE;
                        shown++;
                    }
                } else if (strcmp(c->name, "option") == 0) {
                    emit_listbox_option(ctx, c, first);
                    first = FALSE;
                    shown++;
                }
            }
            if (shown == 0) {
                gsize start = ctx->out->len;
                g_string_append(ctx->out, "\xc2\xa0\xc2\xa0");
                emit_form_attr_sized(ctx->attrs, NS_INLINE_INPUT_FIELD,
                                     start, ctx->out->len, n, ctx->styles);
            }
            return;
        }
        const ns_node *chosen = ns_select_chosen_option(n);
        char *label = chosen ? ns_node_collect_text(chosen) : g_strdup("");
        if (!label) label = g_strdup("");
        gsize start = ctx->out->len;
        g_string_append(ctx->out, "\xc2\xa0");
        if (*label) g_string_append(ctx->out, label);
        const ns_style *select_style = g_hash_table_lookup(ctx->styles, n);
        if (select_style &&
            keyword_is(select_style->values[NS_CSS_APPEARANCE], "none"))
            g_string_append(ctx->out, "\xc2\xa0");
        else
            g_string_append(ctx->out, n == g_open_select_for_layout
                                      ? " \xe2\x96\xb4\xc2\xa0"
                                      : " \xe2\x96\xbe\xc2\xa0");
        emit_form_attr_sized(ctx->attrs, NS_INLINE_INPUT_FIELD,
                             start, ctx->out->len, n, ctx->styles);
        select_size_to_widest_option(ctx, n, start);
        g_free(label);
        if (n == g_open_select_for_layout) {
            for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
                if (c->kind != NS_NODE_ELEMENT || !c->name) continue;
                const ns_node *opts[2] = { NULL, NULL };
                const ns_node *grp = NULL;
                if (strcmp(c->name, "option") == 0) {
                    opts[0] = c;
                } else if (strcmp(c->name, "optgroup") == 0) {
                    grp = c;
                } else {
                    continue;
                }
                if (grp) {
                    const char *gl = ns_element_get_attr(grp, "label");
                    if (gl && *gl) {
                        g_string_append(ctx->out, "\xe2\x80\xa8\xc2\xa0");
                        g_string_append(ctx->out, gl);
                        g_string_append(ctx->out, "\xc2\xa0");
                    }
                    for (const ns_node *o = grp->first_child; o; o = o->next_sibling)
                        if (ns_node_is_element_named(o, "option"))
                            emit_open_select_option(ctx, o);
                } else {
                    emit_open_select_option(ctx, opts[0]);
                }
            }
        }
        return;
    }
    if (strcmp(n->name, "option") == 0 || strcmp(n->name, "optgroup") == 0)
        return;
    if (strcmp(n->name, "textarea") == 0) {
        gsize start = ctx->out->len;
        gboolean focused = (n == g_focused_input_for_layout);
        g_string_append(ctx->out, "\xc2\xa0");
        gsize val_start = ctx->out->len;
        char *raw = ns_textarea_value_dup(n);
        gsize value_byte_len = strlen(raw);
        gboolean any = value_byte_len > 0;
        gsize caret_byte = g_focused_caret_byte_for_layout;
        if (caret_byte > value_byte_len) caret_byte = value_byte_len;
        gsize anchor_byte = g_focused_sel_anchor_byte_for_layout;
        if (anchor_byte > value_byte_len) anchor_byte = value_byte_len;
        gsize caret_pos = val_start;
        gsize anchor_pos = val_start;
        for (gsize i = 0; i <= value_byte_len; i++) {
            if (i == caret_byte) caret_pos = ctx->out->len;
            if (i == anchor_byte) anchor_pos = ctx->out->len;
            if (i == value_byte_len) break;
            char ch = raw[i];
            if (ch == '\n')
                g_string_append(ctx->out, "\xe2\x80\xa8");
            else if (ch != '\r')
                g_string_append_c(ctx->out, ch);
        }
        g_free(raw);
        gsize disp_end = ctx->out->len;
        gboolean is_placeholder = FALSE;
        gboolean ta_sized = ns_element_get_attr(n, "rows") ||
                            ns_element_get_attr(n, "cols");
        if (!any) {
            const char *ph = ns_element_get_attr(n, "placeholder");
            if (ph && *ph) {
                for (const char *p = ph; *p; p++) {
                    if (*p == '\n') g_string_append(ctx->out, "\xe2\x80\xa8");
                    else if (*p != '\r') g_string_append_c(ctx->out, *p);
                }
                disp_end = ctx->out->len;
                is_placeholder = TRUE;
                if (focused) { caret_pos = val_start; anchor_pos = val_start; }
                else         { caret_pos = 0; }
            } else if (ta_sized) {
                const char *rows_attr = ns_element_get_attr(n, "rows");
                int row_lines = rows_attr ? atoi(rows_attr) : 2;
                if (row_lines < 1) row_lines = 1;
                if (row_lines > 1000) row_lines = 1000;
                for (int r = 0; r < row_lines; r++) {
                    if (r) g_string_append(ctx->out, "\xe2\x80\xa8");
                    g_string_append(ctx->out, "\xc2\xa0");
                }
                if (focused) { caret_pos = val_start; anchor_pos = val_start; }
                else         { caret_pos = 0; }
            } else {
                for (int i = 0; i < 40; i++) g_string_append(ctx->out, "\xc2\xa0");
                if (focused) { caret_pos = val_start; anchor_pos = val_start; }
                else         { caret_pos = 0; }
            }
        }
        g_string_append(ctx->out, "\xc2\xa0");
        ns_inline_attr_kind ta_kind = focused
                                       ? NS_INLINE_INPUT_FIELD_FOCUSED
                                       : NS_INLINE_INPUT_FIELD;
        emit_form_attr_sized(ctx->attrs, ta_kind, start, ctx->out->len,
                             n, ctx->styles);
        emit_control_text_style(ctx->attrs, s, start, ctx->out->len,
                                val_start, disp_end, is_placeholder,
                                ns_element_get_attr(n, "disabled") != NULL);
        if (!is_placeholder && disp_end > val_start &&
            ns_element_get_attr(n, "disabled") == NULL &&
            ns_node_spellcheck_used(n) && ns_node_spellcheck_armed(n)) {
            ns_inline_attr sc = { .kind = NS_INLINE_SPELLCHECK,
                                  .start = val_start,
                                  .len = disp_end - val_start, .dom = n };
            g_array_append_val(ctx->attrs, sc);
        }
        if (focused) {
            if (anchor_pos != caret_pos) {
                gsize s0 = anchor_pos < caret_pos ? anchor_pos : caret_pos;
                gsize s1 = anchor_pos < caret_pos ? caret_pos : anchor_pos;
                emit_attr(ctx->attrs, NS_INLINE_SELECTION, s0, s1);
            }
            emit_attr(ctx->attrs, NS_INLINE_CARET, caret_pos, caret_pos + 1);
        }
        return;
    }

    const char *prev_href   = ctx->active_href;
    const char *prev_target = ctx->active_target;
    const ns_node *prev_link_node = ctx->active_link_node;
    if (strcmp(n->name, "a") == 0) {
        const char *h = ns_element_get_attr(n, "href");
        if (h && *h) {
            ctx->active_href   = h;
            ctx->active_target = ns_element_get_attr(n, "target");
            ctx->active_link_node = n;
        }
    }
    double ml = length_or(s ? s->values[NS_CSS_MARGIN_LEFT]  : NULL, 0);
    double mr = length_or(s ? s->values[NS_CSS_MARGIN_RIGHT] : NULL, 0);
    double pl = inline_box_side_px(s, NS_CSS_PADDING_LEFT,
                                   NS_CSS_BORDER_LEFT_WIDTH,
                                   NS_CSS_BORDER_LEFT_STYLE);
    double pr = inline_box_side_px(s, NS_CSS_PADDING_RIGHT,
                                   NS_CSS_BORDER_RIGHT_WIDTH,
                                   NS_CSS_BORDER_RIGHT_STYLE);
    gboolean bidi_override = FALSE;
    gboolean bidi_isolate = FALSE;
    gboolean bidi_plaintext = FALSE;
    if (s) {
        const char *ub = ns_style_keyword(s, NS_CSS_UNICODE_BIDI);
        if (ub) {
            if (strcmp(ub, "bidi-override") == 0)
                bidi_override = TRUE;
            else if (strcmp(ub, "isolate-override") == 0)
                bidi_override = bidi_isolate = TRUE;
            else if (strcmp(ub, "isolate") == 0)
                bidi_isolate = TRUE;
            else if (strcmp(ub, "plaintext") == 0)
                bidi_plaintext = TRUE;
        }
    }
    if (bidi_override || bidi_isolate || bidi_plaintext) {
        const char *bd_dir = ns_element_get_attr(n, "dir");
        if (!bd_dir || !*bd_dir) bd_dir = ns_style_keyword(s, NS_CSS_DIRECTION);
        gboolean rtl = bd_dir && g_ascii_strcasecmp(bd_dir, "rtl") == 0;
        gboolean dir_auto = (bd_dir && g_ascii_strcasecmp(bd_dir, "auto") == 0) ||
                            (strcmp(n->name, "bdi") == 0 &&
                             !ns_element_get_attr(n, "dir"));
        if (bidi_isolate || bidi_plaintext)
            g_string_append(ctx->out,
                (bidi_plaintext || dir_auto) ? "\xe2\x81\xa8"
                : rtl                        ? "\xe2\x81\xa7"
                                             : "\xe2\x81\xa6");
        if (bidi_override)
            g_string_append(ctx->out, rtl ? "\xe2\x80\xae"
                                           : "\xe2\x80\xad");
    }
    append_inline_spacer(ctx, ml);
    gsize elem_start = ctx->out->len;
    append_inline_spacer(ctx, pl);
    gboolean bold   = tag_is_bold(n->name);
    gboolean italic = tag_is_italic(n->name);
    gboolean mono   = tag_is_monospace(n->name);
    gboolean uline  = tag_is_underline(n->name);
    gboolean oline  = FALSE;
    gboolean strike = tag_is_strike(n->name);
    const ns_css_value *fw = s ? s->values[NS_CSS_FONT_WEIGHT] : NULL;
    int font_weight_self = ns_css_font_weight_number(fw, -1);
    gboolean font_weight_active = font_weight_self > 0;
    if (font_weight_active) {
        bold = FALSE;
    } else if (fw && fw->kind == NS_CSS_V_KEYWORD && fw->u.keyword) {
        const char *kw = fw->u.keyword;
        if (strcmp(kw, "bold") == 0 || strcmp(kw, "bolder") == 0) bold = TRUE;
    }
    const ns_css_value *fsv = s ? s->values[NS_CSS_FONT_STYLE] : NULL;
    if (fsv && fsv->kind == NS_CSS_V_KEYWORD && fsv->u.keyword)
        italic = strcmp(fsv->u.keyword, "italic") == 0 ||
                 g_str_has_prefix(fsv->u.keyword, "oblique");
    if (s && s->values[NS_CSS_TEXT_DECORATION] &&
        s->values[NS_CSS_TEXT_DECORATION]->kind == NS_CSS_V_KEYWORD) {
        const char *kw = s->values[NS_CSS_TEXT_DECORATION]->u.keyword;
        if (strstr(kw, "underline")) uline = TRUE;
        if (strstr(kw, "overline")) oline = TRUE;
        if (strstr(kw, "line-through")) strike = TRUE;
        if (strstr(kw, "none")) { uline = FALSE; oline = FALSE; strike = FALSE; }
    }
    const ns_css_value *tdc = s ? s->values[NS_CSS_TEXT_DECORATION_COLOR] : NULL;
    if (tdc && tdc->kind == NS_CSS_V_COLOR && tdc->u.color.a == 0)
        { uline = FALSE; oline = FALSE; strike = FALSE; }
    const ns_style *decor_style = (uline || oline || strike) ? s : NULL;
    if (bold && ctx->bold_depth++ == 0) ctx->bold_start = ctx->out->len;
    if (italic && ctx->italic_depth++ == 0) ctx->italic_start = ctx->out->len;
    if (mono && ctx->mono_depth++ == 0) ctx->mono_start = ctx->out->len;
    if (uline && ctx->underline_depth++ == 0) ctx->underline_start = ctx->out->len;
    if (oline && ctx->overline_depth++ == 0) ctx->overline_start = ctx->out->len;
    if (strike && ctx->strike_depth++ == 0) ctx->strike_start = ctx->out->len;

    gsize weight_start = ctx->out->len;
    const ns_css_value *fst = s ? s->values[NS_CSS_FONT_STRETCH] : NULL;
    int font_stretch_self = ns_css_font_stretch_rank(fst);
    gboolean font_stretch_active = fst != NULL;
    gsize stretch_start = ctx->out->len;
    int font_kerning_self = font_kerning_int_from_style(s);
    const char *font_ligatures_self = font_ligatures_from_style(s);
    const char *font_features_self = font_feature_settings_from_style(s);
    gboolean font_features_active =
        font_kerning_self >= 0 || font_ligatures_self != NULL ||
        font_features_self != NULL;
    gsize features_start = ctx->out->len;
    const char *font_variations_self = font_variation_settings_from_style(s);
    gsize variations_start = ctx->out->len;

    gboolean is_q = strcmp(n->name, "q") == 0;
    if (is_q) {
        char *open_q = quotes_string_for(s, ctx->q_depth, FALSE);
        g_string_append(ctx->out, open_q);
        g_free(open_q);
        ctx->q_depth++;
    }

    double outer_rise = ctx->rise;
    double own_rise = inline_vertical_align_rise(n, s, ctx->styles);
    ctx->rise += own_rise;
    gsize rise_start = ctx->out->len;
    gboolean small_caps = s && keyword_is(s->values[NS_CSS_FONT_VARIANT],
                                          "small-caps");
    gsize sc_start = ctx->out->len;

    double font_size_self = 0;
    if (s && s->values[NS_CSS_FONT_SIZE]) {
        const ns_css_value *fv = s->values[NS_CSS_FONT_SIZE];
        if (fv->kind == NS_CSS_V_LENGTH && fv->u.length.unit == NS_CSS_UNIT_PX)
            font_size_self = fv->u.length.v;
    }
    gsize fs_start = ctx->out->len;
    gboolean fs_active = font_size_self > 0;
    double letter_spacing_self = style_letter_spacing_px(s);
    gboolean letter_spacing_active = s && fabs(letter_spacing_self -
        style_letter_spacing_px(inline_parent_box_style(n, ctx->styles))) > 0.001;
    gsize letter_spacing_start = ctx->out->len;

    gsize color_start = ctx->out->len;
    gboolean color_active = FALSE;
    guint8 cr = 0, cg = 0, cb = 0, ca = 0;
    const ns_css_value *text_color = ns_style_text_fill_color(s);
    if (text_color && text_color->kind == NS_CSS_V_COLOR) {
        cr = text_color->u.color.r;
        cg = text_color->u.color.g;
        cb = text_color->u.color.b;
        ca = text_color->u.color.a;
        color_active = TRUE;
    }
    {
        const char *vis = s ? ns_style_keyword(s, NS_CSS_VISIBILITY) : NULL;
        if (vis && (strcmp(vis, "hidden") == 0 || strcmp(vis, "collapse") == 0)) {
            ca = 0;
            color_active = TRUE;
        }
    }

    gsize family_start = ctx->out->len;
    const char *family_str = NULL;
    if (s && s->values[NS_CSS_FONT_FAMILY] &&
        s->values[NS_CSS_FONT_FAMILY]->kind == NS_CSS_V_KEYWORD)
        family_str = s->values[NS_CSS_FONT_FAMILY]->u.keyword;

    const char *prev_text_transform = ctx->text_transform;
    if (s && s->values[NS_CSS_TEXT_TRANSFORM] &&
        s->values[NS_CSS_TEXT_TRANSFORM]->kind == NS_CSS_V_KEYWORD) {
        const char *kw = s->values[NS_CSS_TEXT_TRANSFORM]->u.keyword;
        if (strcmp(kw, "none") == 0)
            ctx->text_transform = NULL;
        else if (strcmp(kw, "uppercase") == 0 ||
                 strcmp(kw, "lowercase") == 0 ||
                 strcmp(kw, "capitalize") == 0)
            ctx->text_transform = kw;
    }

    gboolean pseudo_blocks = n == g_pseudo_blocks_host;
    if (s && s->before && s->before->values[NS_CSS_CONTENT] &&
        !(pseudo_blocks && g_pseudo_block_before))
        append_pseudo_inline(ctx, s->before, n);

    const ns_node *shadow_root = layout_shadow_root(n);
    if (shadow_root) {
        for (const ns_node *c = shadow_root->first_child; c; c = c->next_sibling)
            collect_walk(c, ctx, depth + 1);
    } else if (!collect_walk_assigned(n, ctx, depth)) {
        for (const ns_node *c = n->first_child; c; c = c->next_sibling)
            collect_walk(c, ctx, depth + 1);
    }

    if (s && s->after && s->after->values[NS_CSS_CONTENT] &&
        !(pseudo_blocks && g_pseudo_block_after))
        append_pseudo_inline(ctx, s->after, n);

    ctx->text_transform = prev_text_transform;

    if (fs_active && ctx->out->len > fs_start)
        emit_font_size_attr(ctx->attrs, fs_start, ctx->out->len, font_size_self);
    if (letter_spacing_active && ctx->out->len > letter_spacing_start)
        emit_letter_spacing_attrs(ctx->attrs, ctx->out, letter_spacing_start,
                                  ctx->out->len, letter_spacing_self);
    if (color_active && ctx->out->len > color_start)
        emit_color_attr(ctx->attrs, color_start, ctx->out->len, cr, cg, cb, ca);
    if (family_str && ctx->out->len > family_start)
        emit_font_family_attr(ctx->attrs, family_start, ctx->out->len, family_str);
    if (font_weight_active && ctx->out->len > weight_start)
        emit_font_weight_attr(ctx->attrs, weight_start, ctx->out->len, font_weight_self);
    if (font_stretch_active && ctx->out->len > stretch_start)
        emit_font_stretch_attr(ctx->attrs, stretch_start, ctx->out->len,
                               font_stretch_self);
    if (font_features_active && ctx->out->len > features_start)
        emit_font_features_attr(ctx->attrs, features_start, ctx->out->len,
                                font_kerning_self, font_ligatures_self,
                                font_features_self);
    if (font_variations_self && ctx->out->len > variations_start)
        emit_font_variations_attr(ctx->attrs, variations_start, ctx->out->len,
                                  font_variations_self);

    if (bold && --ctx->bold_depth == 0)
        emit_attr(ctx->attrs, NS_INLINE_BOLD, ctx->bold_start, ctx->out->len);
    if (italic && --ctx->italic_depth == 0)
        emit_attr(ctx->attrs, NS_INLINE_ITALIC, ctx->italic_start, ctx->out->len);
    if (mono && --ctx->mono_depth == 0)
        emit_attr(ctx->attrs, NS_INLINE_MONOSPACE, ctx->mono_start, ctx->out->len);
    if (uline && --ctx->underline_depth == 0)
        emit_attr_styled(ctx->attrs, NS_INLINE_UNDERLINE, ctx->underline_start,
                         ctx->out->len, decor_style);
    if (oline && --ctx->overline_depth == 0)
        emit_attr_styled(ctx->attrs, NS_INLINE_OVERLINE, ctx->overline_start,
                         ctx->out->len, decor_style);
    if (strike && --ctx->strike_depth == 0)
        emit_attr_styled(ctx->attrs, NS_INLINE_STRIKETHROUGH, ctx->strike_start,
                         ctx->out->len, decor_style);
    emit_rise_attr(ctx, rise_start, own_rise);
    if (small_caps && ctx->out->len > sc_start)
        emit_attr(ctx->attrs, NS_INLINE_SMALL_CAPS, sc_start, ctx->out->len);
    if (is_q) {
        ctx->q_depth--;
        char *close_q = quotes_string_for(s, ctx->q_depth, TRUE);
        g_string_append(ctx->out, close_q);
        g_free(close_q);
    }
    append_inline_spacer(ctx, pr);
    ns_inline_attr elem = {
        .kind = ctx->out->len > elem_start ? NS_INLINE_ELEMENT
                                           : NS_INLINE_EMPTY_ELEMENT,
        .start = elem_start,
        .len = ctx->out->len - elem_start,
        .rise_px = ctx->rise,
        .dom = n,
        .style = s,
        .bg_image_src = style_first_bg_url(s),
    };
    elem.bg_image = collect_peek_image(elem.bg_image_src);
    g_array_append_val(ctx->attrs, elem);
    ctx->rise = outer_rise;
    append_inline_spacer(ctx, mr);
    if (bidi_override)
        g_string_append(ctx->out, "\xe2\x80\xac");
    if (bidi_isolate || bidi_plaintext)
        g_string_append(ctx->out, "\xe2\x81\xa9");
    ctx->active_href   = prev_href;
    ctx->active_target = prev_target;
    ctx->active_link_node = prev_link_node;
}

static gsize
bidi_control_len(const char *s, gsize avail)
{
    if (avail < 3 || (guchar)s[0] != 0xe2) return 0;
    guchar b1 = (guchar)s[1], b2 = (guchar)s[2];
    if (b1 == 0x81 && b2 >= 0xa6 && b2 <= 0xa9) return 3;
    if (b1 == 0x80 && b2 >= 0xaa && b2 <= 0xae) return 3;
    return 0;
}

static int
white_space_mode(const ns_node *node, GHashTable *styles)
{
    for (const ns_node *p = node; p; p = p->parent) {
        if (p->kind != NS_NODE_ELEMENT) continue;
        const ns_style *ps = g_hash_table_lookup(styles, p);
        if (ps) {
            const ns_css_value *ws = ps->values[NS_CSS_WHITE_SPACE];
            if (ws && ws->kind == NS_CSS_V_KEYWORD && ws->u.keyword) {
                const char *kw = ws->u.keyword;
                if (strcmp(kw, "pre-line") == 0)
                    return NS_WS_PRE_LINE;
                if (strcmp(kw, "pre") == 0 ||
                    strcmp(kw, "pre-wrap") == 0 ||
                    strcmp(kw, "break-spaces") == 0)
                    return NS_WS_PRESERVE;
                if (strcmp(kw, "normal") == 0 || strcmp(kw, "nowrap") == 0)
                    return NS_WS_COLLAPSE;
            }
        }
        if (p->name && (strcmp(p->name, "pre") == 0 ||
                        strcmp(p->name, "textarea") == 0))
            return NS_WS_PRESERVE;
    }
    return NS_WS_COLLAPSE;
}

static void
append_decoration_attrs(GArray *attrs, const ns_style *ps, gsize len)
{
    const ns_css_value *v = ps ? ps->values[NS_CSS_TEXT_DECORATION] : NULL;
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return;
    const char *kw = v->u.keyword;
    const ns_css_value *pc = ps->values[NS_CSS_TEXT_DECORATION_COLOR];
    if (strstr(kw, "none") || (pc && pc->kind == NS_CSS_V_COLOR && pc->u.color.a == 0))
        return;
    static const struct { const char *word; ns_inline_attr_kind kind; } lines[] = {
        { "underline",    NS_INLINE_UNDERLINE },
        { "overline",     NS_INLINE_OVERLINE },
        { "line-through", NS_INLINE_STRIKETHROUGH },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(lines); i++) {
        if (!strstr(kw, lines[i].word)) continue;
        ns_inline_attr a = { .kind = lines[i].kind, .style = ps,
                             .start = 0, .len = len };
        g_array_append_val(attrs, a);
    }
}

static gboolean
decoration_stops_at(const ns_style *s)
{
    return style_is_absolute_or_fixed(s) || float_side_of(s) >= 0 ||
           ns_display_is_atomic_inline(ns_css_display_of(s));
}

static ns_box *
build_inline_run_impl(const ns_node *first, const ns_node *last_excl,
                      GHashTable *styles, gboolean abs_placeholders)
{
    GString *buf = g_string_new(NULL);
    GArray  *raw_links = g_array_new(FALSE, FALSE, sizeof(ns_link_range));
    GArray  *raw_attrs = g_array_new(FALSE, FALSE, sizeof(ns_inline_attr));
    GArray  *raw_atomics = g_array_new(FALSE, FALSE, sizeof(ns_atomic_raw));
    GArray  *ws_ranges = g_array_new(FALSE, FALSE, sizeof(ns_ws_range));
    g_array_set_clear_func(raw_links, link_clear);
    collector_ctx ctx = {
        .styles = styles, .out = buf, .links = raw_links, .attrs = raw_attrs,
        .atomics = raw_atomics, .abs_placeholders = abs_placeholders,
        .ws_ranges = ws_ranges,
    };
    if (first && first->parent) {
        const ns_style *ps = g_hash_table_lookup(styles, first->parent);
        if (ps && ps->values[NS_CSS_TEXT_TRANSFORM] &&
            ps->values[NS_CSS_TEXT_TRANSFORM]->kind == NS_CSS_V_KEYWORD) {
            const char *kw = ps->values[NS_CSS_TEXT_TRANSFORM]->u.keyword;
            if (kw && strcmp(kw, "none") != 0) ctx.text_transform = kw;
        }
    }
    if (first && first->parent && first->parent->kind == NS_NODE_ELEMENT) {
        const ns_style *li_style = g_hash_table_lookup(styles, first->parent);
        if (ns_paint_li_is_inside(li_style) &&
            inline_run_opens_parent(first, styles)) {
            char marker[64];
            if (ns_paint_li_marker_text(first->parent, li_style,
                                        marker, sizeof marker))
                g_string_append(buf, marker);
        }
    }
    for (const ns_node *n = first; n && n != last_excl; n = n->next_sibling)
        collect_walk(n, &ctx, g_inline_collect_depth);

    int run_ws_mode = first ? white_space_mode(first, styles) : NS_WS_COLLAPSE;

    GString *collapsed = g_string_new(NULL);
    gsize   *map = g_new(gsize, buf->len + 1);
    gboolean prev_ws = run_ws_mode != NS_WS_PRESERVE;
    gboolean trailing_preserved = FALSE;
    gsize collapsible_space_end = 0;
    guint ri = 0;
    for (gsize i = 0; i < buf->len; i++) {
        char c = buf->str[i];
        while (ri < ws_ranges->len &&
               g_array_index(ws_ranges, ns_ws_range, ri).end <= i)
            ri++;
        int ws_mode = run_ws_mode;
        if (ri < ws_ranges->len &&
            g_array_index(ws_ranges, ns_ws_range, ri).start <= i)
            ws_mode = g_array_index(ws_ranges, ns_ws_range, ri).mode;
        trailing_preserved = ws_mode == NS_WS_PRESERVE;
        if (ws_mode == NS_WS_PRESERVE) {
            map[i] = collapsed->len;
            g_string_append_c(collapsed, c);
            prev_ws = c == '\n';
            continue;
        }
        if (ws_mode == NS_WS_PRE_LINE && c == '\n') {
            map[i] = collapsed->len;
            g_string_append_c(collapsed, '\n');
            prev_ws = TRUE;
            continue;
        }
        gboolean ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f');
        gboolean line_break = i + 2 < buf->len &&
                              memcmp(buf->str + i, "\xe2\x80\xa8", 3) == 0;
        if (ws) {
            if (!prev_ws) {
                map[i] = collapsed->len;
                g_string_append_c(collapsed, ' ');
                collapsible_space_end = collapsed->len;
            } else {
                map[i] = collapsed->len;
            }
            prev_ws = TRUE;
        } else if (line_break) {
            if (collapsed->len > 0 && collapsible_space_end == collapsed->len) {
                g_string_set_size(collapsed, collapsed->len - 1);
                for (gsize k = i; k > 0 && map[k - 1] > collapsed->len; k--)
                    map[k - 1] = collapsed->len;
            }
            for (int k = 0; k < 3; k++) map[i + k] = collapsed->len;
            g_string_append_len(collapsed, buf->str + i, 3);
            i += 2;
            prev_ws = TRUE;
        } else if (bidi_control_len(buf->str + i, buf->len - i)) {
            for (int k = 0; k < 3; k++) map[i + k] = collapsed->len;
            g_string_append_len(collapsed, buf->str + i, 3);
            i += 2;
        } else {
            map[i] = collapsed->len;
            g_string_append_c(collapsed, c);
            prev_ws = FALSE;
        }
    }
    map[buf->len] = collapsed->len;

    ns_box *box = box_new_inline();
    gsize tail = collapsed->len;
    while (tail >= 3 && bidi_control_len(collapsed->str + tail - 3, 3)) tail -= 3;
    if (!trailing_preserved && tail > 0 && collapsed->str[tail - 1] == ' ') {
        g_string_erase(collapsed, (gssize)tail - 1, 1);
        for (gsize k = 0; k <= buf->len; k++)
            if (map[k] >= tail) map[k]--;
    }

    for (guint i = 0; i < raw_links->len; i++) {
        ns_link_range *r = &g_array_index(raw_links, ns_link_range, i);
        if (r->start > buf->len) r->start = buf->len;
        gsize end = r->start + r->len;
        if (end > buf->len) end = buf->len;
        gsize ns = map[r->start];
        gsize ne = map[end];
        if (ne > collapsed->len) ne = collapsed->len;
        if (ne <= ns) continue;
        ns_link_range out = {
            .start = ns,
            .len = ne - ns,
            .href = g_strdup(r->href),
            .target = r->target ? g_strdup(r->target) : NULL,
            .dom = r->dom,
        };
        g_array_append_val(inline_links_ensure(box), out);
    }

    for (guint i = 0; i < raw_attrs->len; i++) {
        ns_inline_attr *a = &g_array_index(raw_attrs, ns_inline_attr, i);
        if (a->start > buf->len) a->start = buf->len;
        gsize end = a->start + a->len;
        if (end > buf->len) end = buf->len;
        gsize ns = map[a->start];
        gsize ne = map[end];
        if (ne > collapsed->len) ne = collapsed->len;
        gboolean collapses_empty = ne <= ns &&
            (a->kind == NS_INLINE_EMPTY_ELEMENT ||
             a->kind == NS_INLINE_ELEMENT) && ns <= collapsed->len;
        if (ne <= ns && !collapses_empty)
            continue;
        ns_inline_attr out = *a;
        if (collapses_empty) {
            out.kind = NS_INLINE_EMPTY_ELEMENT;
            ne = ns;
        }
        out.start = ns;
        out.len = ne - ns;
        g_array_append_val(box->attrs, out);
        if (out.bg_image_src) {
            ns_box_media *m = ns_box_media_ensure(box);
            if (!m->bg_image_src) {
                m->bg_image_src = g_strdup(out.bg_image_src);
                m->bg_image = out.bg_image;
            }
        }
    }

    if (first && collapsed->len > 0) {
        for (const ns_node *p = first->parent;
             p && p->kind == NS_NODE_ELEMENT; p = p->parent) {
            const ns_style *ps = g_hash_table_lookup(styles, p);
            if (!ps) break;
            if (style_is_contents(ps)) continue;
            append_decoration_attrs(box->attrs, ps, collapsed->len);
            if (decoration_stops_at(ps)) break;
        }
    }

    if (first && first->parent && collapsed->len > 0 &&
        first == first->parent->first_child) {
        const ns_style *ps = g_hash_table_lookup(styles, first->parent);
        if (ps && ps->first_letter) {
            const ns_style *fl = ps->first_letter;
            const char *txt = collapsed->str;
            gsize i = 0;
            while (i < collapsed->len &&
                   (txt[i] == ' ' || txt[i] == '\t' || txt[i] == '\n'))
                i++;
            while (i < collapsed->len) {
                gunichar ch = g_utf8_get_char(txt + i);
                if (g_unichar_isalnum(ch)) break;
                i = (gsize)(g_utf8_next_char(txt + i) - txt);
            }
            if (i < collapsed->len) {
                gsize fl_start = i;
                gsize fl_end = (gsize)(g_utf8_next_char(txt + i) - txt);
                if (fl_end > collapsed->len) fl_end = collapsed->len;
                gsize fl_len = fl_end - fl_start;
                if (fl->values[NS_CSS_FONT_SIZE] &&
                    fl->values[NS_CSS_FONT_SIZE]->kind == NS_CSS_V_LENGTH &&
                    fl->values[NS_CSS_FONT_SIZE]->u.length.unit == NS_CSS_UNIT_PX) {
                    ns_inline_attr a = {
                        .kind = NS_INLINE_FONT_SIZE,
                        .start = fl_start, .len = fl_len,
                        .font_size_px = fl->values[NS_CSS_FONT_SIZE]->u.length.v,
                    };
                    g_array_append_val(box->attrs, a);
                }
                const ns_css_value *fl_color = ns_style_text_fill_color(fl);
                if (fl_color && fl_color->kind == NS_CSS_V_COLOR) {
                    ns_inline_attr a = {
                        .kind = NS_INLINE_COLOR,
                        .start = fl_start, .len = fl_len,
                        .r = fl_color->u.color.r,
                        .g = fl_color->u.color.g,
                        .b = fl_color->u.color.b,
                        .a = fl_color->u.color.a,
                    };
                    g_array_append_val(box->attrs, a);
                }
                if (fl->values[NS_CSS_BACKGROUND_COLOR] &&
                    fl->values[NS_CSS_BACKGROUND_COLOR]->kind == NS_CSS_V_COLOR) {
                    ns_inline_attr a = {
                        .kind = NS_INLINE_BG_COLOR,
                        .start = fl_start, .len = fl_len,
                        .r = fl->values[NS_CSS_BACKGROUND_COLOR]->u.color.r,
                        .g = fl->values[NS_CSS_BACKGROUND_COLOR]->u.color.g,
                        .b = fl->values[NS_CSS_BACKGROUND_COLOR]->u.color.b,
                        .a = fl->values[NS_CSS_BACKGROUND_COLOR]->u.color.a,
                    };
                    g_array_append_val(box->attrs, a);
                }
                const ns_css_value *fw = fl->values[NS_CSS_FONT_WEIGHT];
                int font_weight = ns_css_font_weight_number(fw, -1);
                if (font_weight > 0) {
                    ns_inline_attr a = {
                        .kind = NS_INLINE_FONT_WEIGHT,
                        .start = fl_start, .len = fl_len,
                        .font_weight = font_weight,
                    };
                    g_array_append_val(box->attrs, a);
                }
                if (keyword_is(fl->values[NS_CSS_FONT_STYLE], "italic") ||
                    keyword_is(fl->values[NS_CSS_FONT_STYLE], "oblique")) {
                    ns_inline_attr a = {
                        .kind = NS_INLINE_ITALIC,
                        .start = fl_start, .len = fl_len,
                    };
                    g_array_append_val(box->attrs, a);
                }
                if (fl->values[NS_CSS_FONT_FAMILY] &&
                    fl->values[NS_CSS_FONT_FAMILY]->kind == NS_CSS_V_KEYWORD) {
                    ns_inline_attr a = {
                        .kind = NS_INLINE_FONT_FAMILY,
                        .start = fl_start, .len = fl_len,
                        .family = fl->values[NS_CSS_FONT_FAMILY]->u.keyword,
                    };
                    g_array_append_val(box->attrs, a);
                }
                int fk = font_kerning_int_from_style(fl);
                const char *flig = font_ligatures_from_style(fl);
                const char *ffea = font_feature_settings_from_style(fl);
                if (fk >= 0 || flig || ffea) {
                    ns_inline_attr a = {
                        .kind = NS_INLINE_FONT_FEATURES,
                        .start = fl_start, .len = fl_len,
                        .font_kerning = fk,
                        .font_ligatures = flig,
                        .font_features = ffea,
                    };
                    g_array_append_val(box->attrs, a);
                }
                const char *fvar = font_variation_settings_from_style(fl);
                if (fvar) {
                    ns_inline_attr a = {
                        .kind = NS_INLINE_FONT_VARIATIONS,
                        .start = fl_start, .len = fl_len,
                        .font_variations = fvar,
                    };
                    g_array_append_val(box->attrs, a);
                }
            }
        }
    }

    for (guint i = 0; i < raw_atomics->len; i++) {
        ns_atomic_raw *rr = &g_array_index(raw_atomics, ns_atomic_raw, i);
        gsize start = rr->start;
        if (start > buf->len) start = buf->len;
        gsize ns = map[start];
        if (ns > collapsed->len) ns = collapsed->len;
        if (!box->inline_atomics)
            box->inline_atomics = g_array_new(FALSE, FALSE, sizeof(ns_inline_atomic));
        ns_inline_atomic ia = {
            .byte_off = ns, .box = rr->box, .rise_px = rr->rise,
        };
        if (rr->box && !rr->box->parent) rr->box->parent = box;
        g_array_append_val(box->inline_atomics, ia);
    }

    g_free(map);
    g_array_free(raw_links, TRUE);
    g_array_free(raw_attrs, TRUE);
    g_array_free(raw_atomics, TRUE);
    g_array_free(ws_ranges, TRUE);
    g_string_free(buf, TRUE);

    box->text = g_string_free(collapsed, FALSE);
    return box;
}

static ns_box *
build_inline_run(const ns_node *first, const ns_node *last_excl, GHashTable *styles)
{
    return build_inline_run_impl(first, last_excl, styles, TRUE);
}

static ns_box *
build_inline_run_no_abs_placeholders(const ns_node *first,
                                     const ns_node *last_excl,
                                     GHashTable *styles)
{
    return build_inline_run_impl(first, last_excl, styles, FALSE);
}

static gboolean
inline_run_has_empty_elements(const ns_box *run)
{
    for (guint i = 0; run->attrs && i < run->attrs->len; i++)
        if (g_array_index(run->attrs, ns_inline_attr, i).kind ==
            NS_INLINE_EMPTY_ELEMENT)
            return TRUE;
    return FALSE;
}

static gboolean
inline_run_worth_keeping(const ns_box *run)
{
    return run && run->text &&
           (run->text[0] || inline_run_has_empty_elements(run));
}

static ns_box *
build_form_control_block(const ns_node *n, const ns_style *s, GHashTable *styles)
{
    ns_box *block = box_new(NS_BOX_BLOCK);
    block->dom = n;
    block->style = s;
    collect_box_bg_image(block, s);
    const ns_node *prev_fc = g_form_control_inline;
    g_form_control_inline = n;
    ns_box *run = build_inline_run(n, n->next_sibling, styles);
    g_form_control_inline = prev_fc;
    if (run && run->text && run->text[0]) {
        box_append_child(block, run);
    } else if (run) {
        ns_box_free(run);
    }
    return block;
}

typedef struct {
    char    *url;
    double   density;
    double   width;
} srcset_candidate;

static gboolean
srcset_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static gboolean
srcset_valid_integer(const char *s, gsize n)
{
    if (n == 0) return FALSE;
    for (gsize i = 0; i < n; i++)
        if (!g_ascii_isdigit(s[i])) return FALSE;
    return TRUE;
}

static gboolean
srcset_valid_float(const char *s, gsize n)
{
    gsize i = 0, digits = 0;
    if (i < n && s[i] == '-') i++;
    while (i < n && g_ascii_isdigit(s[i])) { i++; digits++; }
    if (i < n && s[i] == '.') {
        i++;
        gsize frac = 0;
        while (i < n && g_ascii_isdigit(s[i])) { i++; frac++; }
        if (frac == 0) return FALSE;
        digits += frac;
    }
    if (digits == 0) return FALSE;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '-' || s[i] == '+')) i++;
        gsize exp = 0;
        while (i < n && g_ascii_isdigit(s[i])) { i++; exp++; }
        if (exp == 0) return FALSE;
    }
    return i == n;
}

static double
srcset_number(const char *s, gsize n)
{
    char *copy = g_strndup(s, n);
    double v = g_ascii_strtod(copy, NULL);
    g_free(copy);
    return v;
}

static gboolean
srcset_parse_descriptors(GPtrArray *descriptors, double *width, double *density)
{
    gboolean has_w = FALSE, has_x = FALSE, has_h = FALSE;
    for (guint i = 0; i < descriptors->len; i++) {
        const char *d = g_ptr_array_index(descriptors, i);
        gsize n = strlen(d);
        char last = n ? d[n - 1] : '\0';
        if (last == 'w') {
            if (has_w || has_x || !srcset_valid_integer(d, n - 1)) return FALSE;
            double v = srcset_number(d, n - 1);
            if (v <= 0) return FALSE;
            has_w = TRUE;
            *width = v;
        } else if (last == 'x') {
            if (has_w || has_x || has_h || !srcset_valid_float(d, n - 1))
                return FALSE;
            double v = srcset_number(d, n - 1);
            if (v < 0) return FALSE;
            has_x = TRUE;
            *density = v;
        } else if (last == 'h') {
            if (has_h || has_x || !srcset_valid_integer(d, n - 1)) return FALSE;
            if (srcset_number(d, n - 1) <= 0) return FALSE;
            has_h = TRUE;
        } else {
            return FALSE;
        }
    }
    return !has_h || has_w;
}

typedef enum {
    SRCSET_IN_DESCRIPTOR,
    SRCSET_IN_PARENS,
    SRCSET_AFTER_DESCRIPTOR,
} srcset_state;

static const char *
srcset_tokenize_descriptors(const char *p, GPtrArray *descriptors)
{
    while (srcset_is_space(*p)) p++;
    GString *cur = g_string_new(NULL);
    srcset_state state = SRCSET_IN_DESCRIPTOR;
    for (;; p++) {
        char c = *p;
        if (state == SRCSET_IN_DESCRIPTOR) {
            if (srcset_is_space(c)) {
                if (cur->len) {
                    g_ptr_array_add(descriptors, g_strdup(cur->str));
                    g_string_truncate(cur, 0);
                    state = SRCSET_AFTER_DESCRIPTOR;
                }
            } else if (c == ',') {
                p++;
                break;
            } else if (c == '\0') {
                break;
            } else {
                g_string_append_c(cur, c);
                if (c == '(') state = SRCSET_IN_PARENS;
            }
        } else if (state == SRCSET_IN_PARENS) {
            if (c == '\0') break;
            g_string_append_c(cur, c);
            if (c == ')') state = SRCSET_IN_DESCRIPTOR;
        } else {
            if (c == '\0') break;
            if (!srcset_is_space(c)) {
                state = SRCSET_IN_DESCRIPTOR;
                p--;
            }
        }
    }
    if (cur->len) g_ptr_array_add(descriptors, g_strdup(cur->str));
    g_string_free(cur, TRUE);
    return p;
}

static GArray *
srcset_parse(const char *input)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(srcset_candidate));
    const char *p = input ? input : "";
    for (;;) {
        while (srcset_is_space(*p) || *p == ',') p++;
        if (!*p) break;
        const char *url_s = p;
        while (*p && !srcset_is_space(*p)) p++;
        gsize url_len = (gsize)(p - url_s);
        GPtrArray *descriptors = g_ptr_array_new_with_free_func(g_free);
        if (url_s[url_len - 1] == ',') {
            while (url_len > 0 && url_s[url_len - 1] == ',') url_len--;
        } else {
            p = srcset_tokenize_descriptors(p, descriptors);
        }
        double width = -1, density = -1;
        if (url_len > 0 &&
            srcset_parse_descriptors(descriptors, &width, &density)) {
            srcset_candidate c = { g_strndup(url_s, url_len), density, width };
            g_array_append_val(out, c);
        }
        g_ptr_array_free(descriptors, TRUE);
    }
    return out;
}

static void
srcset_candidates_free(GArray *candidates)
{
    for (guint i = 0; i < candidates->len; i++)
        g_free(g_array_index(candidates, srcset_candidate, i).url);
    g_array_free(candidates, TRUE);
}

static char *
srcset_select(const char *srcset, const char *sizes, const char *src,
              double *density)
{
    const double dpr = ns_css_device_pixel_ratio();
    GArray *cands = srcset_parse(srcset);
    gboolean any_width = FALSE, any_unit_density = FALSE;
    double source_size = -1;
    for (guint i = 0; i < cands->len; i++) {
        srcset_candidate *c = &g_array_index(cands, srcset_candidate, i);
        if (c->width > 0) {
            any_width = TRUE;
            if (source_size < 0) {
                source_size = ns_css_sizes_resolve(sizes);
                if (!isfinite(source_size))
                    source_size = ns_css_sizes_resolve(NULL);
            }
            c->density = source_size > 0 ? c->width / source_size : INFINITY;
        } else if (c->density < 0) {
            c->density = 1.0;
        }
        if (c->width <= 0 && c->density == 1.0) any_unit_density = TRUE;
    }
    if (src && *src && !any_width && !any_unit_density) {
        srcset_candidate c = { g_strdup(src), 1.0, -1 };
        g_array_append_val(cands, c);
    }
    const srcset_candidate *best = NULL, *largest = NULL;
    for (guint i = 0; i < cands->len; i++) {
        const srcset_candidate *c = &g_array_index(cands, srcset_candidate, i);
        gboolean duplicate = FALSE;
        for (guint j = 0; j < i && !duplicate; j++)
            duplicate = g_array_index(cands, srcset_candidate, j).density ==
                        c->density;
        if (duplicate) continue;
        if (c->density >= dpr && (!best || c->density < best->density))
            best = c;
        if (!largest || c->density > largest->density) largest = c;
    }
    if (!best) best = largest;
    char *url = best ? g_strdup(best->url) : NULL;
    if (best && density) *density = best->density;
    srcset_candidates_free(cands);
    return url;
}

char *
ns_layout_select_srcset(const char *srcset, const char *sizes)
{
    return srcset_select(srcset, sizes, NULL, NULL);
}

static gboolean
srcset_has_width_descriptor(const char *srcset)
{
    GArray *cands = srcset_parse(srcset);
    gboolean any = FALSE;
    for (guint i = 0; i < cands->len && !any; i++)
        any = g_array_index(cands, srcset_candidate, i).width > 0;
    srcset_candidates_free(cands);
    return any;
}

static gboolean
ns_pixbuf_likely_supports(const char *mime)
{
    if (!mime || !*mime) return TRUE;
    return ns_image_supports_mime(mime);
}

static char *
pick_picture_source_url(const ns_node *picture, const ns_node *img,
                        double *density)
{
    if (!picture) return NULL;
    char *data_fallback = NULL;
    double data_density = 1.0;
    for (const ns_node *c = picture->first_child; c && c != img;
         c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "source") != 0) continue;
        const char *type = ns_element_get_attr(c, "type");
        if (type && !ns_pixbuf_likely_supports(type)) continue;
        const char *media = ns_element_get_attr(c, "media");
        if (media && *media && !ns_css_media_query_matches(media)) continue;
        const char *sizes = ns_element_get_attr(c, "sizes");
        const char *sets[2] = { ns_element_get_attr(c, "data-srcset"),
                                ns_element_get_attr(c, "srcset") };
        for (int i = 0; i < 2; i++) {
            double d = 1.0;
            char *u = srcset_select(sets[i], sizes, NULL, &d);
            if (u && !g_str_has_prefix(u, "data:")) {
                g_free(data_fallback);
                *density = d;
                return u;
            }
            if (u && !data_fallback) {
                data_fallback = u;
                data_density = d;
            } else {
                g_free(u);
            }
        }
        const char *s = ns_element_get_attr(c, "src");
        if (s && *s) {
            if (!g_str_has_prefix(s, "data:")) {
                g_free(data_fallback);
                *density = 1.0;
                return g_strdup(s);
            }
            if (!data_fallback) {
                data_fallback = g_strdup(s);
                data_density = 1.0;
            }
        }
    }
    *density = data_density;
    return data_fallback;
}

static char *
pick_img_url(const ns_node *n, double *density)
{
    if (!n) return NULL;
    const char *src    = ns_element_get_attr(n, "src");
    const char *srcset = ns_element_get_attr(n, "srcset");
    const char *dsrc   = ns_element_get_attr(n, "data-src");
    if (!dsrc || !*dsrc) dsrc = ns_element_get_attr(n, "data-original");
    if (!dsrc || !*dsrc) dsrc = ns_element_get_attr(n, "data-lazy-src");
    const char *dsset  = ns_element_get_attr(n, "data-srcset");
    if (!dsset || !*dsset) dsset = ns_element_get_attr(n, "data-lazy-srcset");
    const char *sizes  = ns_element_get_attr(n, "sizes");

    *density = 1.0;
    char *u = srcset_select(dsset, sizes, NULL, density);
    if (u && (srcset_has_width_descriptor(dsset) || !dsrc || !*dsrc))
        return u;
    g_free(u);
    *density = 1.0;
    if (dsrc && *dsrc) return g_strdup(dsrc);

    gboolean placeholder = src && g_str_has_prefix(src, "data:");
    u = srcset_select(srcset, sizes, placeholder ? NULL : src, density);
    if (u) return u;
    *density = 1.0;
    if (src && *src) return g_strdup(src);
    return NULL;
}

static char *
choose_img_url(const ns_node *n, const ns_node **img_out, double *density)
{
    const ns_node *img = n;
    char *url = NULL;
    *density = 1.0;
    if (ns_node_is_element_named(n, "img") &&
        ns_node_is_element_named(n->parent, "picture"))
        n = n->parent;
    if (n->name && strcmp(n->name, "picture") == 0) {
        for (const ns_node *c = n->first_child; c && img == n;
             c = c->next_sibling)
            if (ns_node_is_element_named(c, "img"))
                img = c;
        double source_density = 1.0;
        char *source_url = pick_picture_source_url(n, img != n ? img : NULL,
                                                   &source_density);
        if (source_url && !g_str_has_prefix(source_url, "data:")) {
            url = source_url;
            *density = source_density;
        } else {
            if (img != n) url = pick_img_url(img, density);
            if (!url && source_url) {
                url = source_url;
                *density = source_density;
            } else {
                g_free(source_url);
            }
        }
    } else {
        url = pick_img_url(n, density);
    }
    if (img_out) *img_out = img;
    return url;
}

static double
image_dimension_attr(const ns_node *n, const char *name)
{
    const char *s = ns_element_get_attr(n, name);
    if (!s || !*s) return 0;
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (end == s || !(v > 0)) return 0;
    return v;
}

static ns_image *
decode_data_image_for_layout(const char *url, const char *key)
{
    if (!url || !key || !g_image_cache_for_layout ||
        !g_str_has_prefix(url, "data:image/"))
        return NULL;
    ns_image *cached = ns_image_cache_peek(g_image_cache_for_layout, key);
    if (cached) return cached;
    GByteArray *bytes = g_byte_array_new();
    char *ctype = NULL;
    gboolean too_large = FALSE;
    gboolean ok = ns_data_url_decode(url, bytes, NS_LAYOUT_DATA_IMAGE_BUDGET,
                                     &ctype, &too_large);
    g_free(ctype);
    if (!ok || too_large || bytes->len == 0) {
        g_byte_array_free(bytes, TRUE);
        return NULL;
    }
    ns_image *img = ns_image_cache_insert_encoded(g_image_cache_for_layout, key,
                                                  bytes->data, bytes->len);
    g_byte_array_free(bytes, TRUE);
    return img;
}

char *
ns_img_chosen_url(const ns_node *n)
{
    if (!n) return NULL;
    double density = 1.0;
    return choose_img_url(n, NULL, &density);
}

double
ns_img_chosen_density(const ns_node *n)
{
    if (!n) return 1.0;
    double density = 1.0;
    g_free(choose_img_url(n, NULL, &density));
    return density > 0 ? density : 1.0;
}

static ns_box *
build_image_box(const ns_node *n)
{
    const ns_node *img = n;
    double density = 1.0;
    char *url = choose_img_url(n, &img, &density);
    ns_box *box = box_new(NS_BOX_IMAGE);
    box->dom = img;
    ns_box_media *m = ns_box_media_ensure(box);
    m->image_src = url;
    m->image_density = density > 0 ? density : 1.0;
    box->content_width = image_dimension_attr(img, "width");
    box->content_height = image_dimension_attr(img, "height");
    double file_w = image_dimension_attr(img, "data-file-width");
    double file_h = image_dimension_attr(img, "data-file-height");
    if (box->content_width <= 0 && box->content_height > 0 &&
        file_w > 0 && file_h > 0)
        box->content_width = box->content_height * file_w / file_h;
    if (box->content_height <= 0 && box->content_width > 0 &&
        file_w > 0 && file_h > 0)
        box->content_height = box->content_width * file_h / file_w;
    m->declared_image_size =
        box->content_width > 0 && box->content_height > 0;
    if (g_image_cache_for_layout) {
        char *abs = g_base_url_for_layout
            ? ns_url_resolve(g_base_url_for_layout, url)
            : NULL;
        const char *key = abs ? abs : url;
        m->image = decode_data_image_for_layout(url, key);
        if (!m->image)
            m->image = ns_image_cache_peek(g_image_cache_for_layout, key);
        g_free(abs);
    }
    if (!m->declared_image_size && url && !m->image &&
        box->content_width <= 0 && box->content_height <= 0) {
        box->content_width = 200;
        box->content_height = 150;
        m->placeholder_image_size = TRUE;
    }
    return box;
}

static const char *
first_media_source_url(const ns_node *n)
{
    const char *src = ns_element_get_attr(n, NS_MEDIA_SRC_ATTR);
    if (src && *src) return src;
    src = ns_element_get_attr(n, "src");
    if (src && *src) return src;
    src = ns_element_get_attr(n, "data-mp4");
    if (src && *src) return src;
    src = ns_element_get_attr(n, "data-webm");
    if (src && *src) return src;
    return ns_media_select_source(n);
}

static ns_box *
build_audio_box(const ns_node *n)
{
    ns_box *box = box_new(NS_BOX_VIDEO);
    box->dom = n;
    ns_box_media *m = ns_box_media_ensure(box);
    const char *src = first_media_source_url(n);
    if (src) m->video_audio_src = g_strdup(src);
    if (ns_element_get_attr(n, "controls")) {
        const char *ws = ns_element_get_attr(n, "width");
        const char *hs = ns_element_get_attr(n, "height");
        box->content_width = ws ? g_ascii_strtod(ws, NULL) : 250;
        box->content_height = hs ? g_ascii_strtod(hs, NULL) : 32;
    }
    return box;
}

static ns_box *
build_video_box(const ns_node *n)
{
    const char *src = first_media_source_url(n);
    ns_box *box = box_new(NS_BOX_VIDEO);
    box->dom = n;
    ns_box_media *m = ns_box_media_ensure(box);
    if (src) m->video_src = g_strdup(src);
    const char *poster = ns_element_get_attr(n, "poster");
    const char *data_poster = ns_element_get_attr(n, "data-poster");
    const char *fallback_poster = ns_element_get_attr(n, NS_MEDIA_POSTER_ATTR);
    if ((!poster || !*poster || g_str_has_prefix(poster, "data:image/")) &&
        data_poster && *data_poster)
        poster = data_poster;
    if ((!poster || !*poster) && fallback_poster && *fallback_poster)
        poster = fallback_poster;
    if (poster && *poster) m->video_poster = g_strdup(poster);
    const char *ws = ns_element_get_attr(n, "width");
    const char *hs = ns_element_get_attr(n, "height");
    gboolean metadata = node_has_media_metadata(n);
    box->content_width  = ws ? g_ascii_strtod(ws, NULL) : (metadata ? 640 : 300);
    box->content_height = hs ? g_ascii_strtod(hs, NULL) : (metadata ? 360 : 150);
    const char *audio = ns_element_get_attr(n, "data-audio-src");
    if (audio && *audio) m->video_audio_src = g_strdup(audio);
    return box;
}

static gboolean
pseudo_text_collapses(const ns_style *ps)
{
    const char *ws = ns_style_keyword(ps, NS_CSS_WHITE_SPACE);
    return !ws || strcmp(ws, "normal") == 0 || strcmp(ws, "nowrap") == 0;
}

static char *
pseudo_block_text(const ns_style *ps, const char *txt)
{
    if (!pseudo_text_collapses(ps)) return g_strdup(txt);
    GString *out = g_string_new(NULL);
    gboolean pending_space = FALSE;
    for (const char *p = txt; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') {
            pending_space = out->len > 0;
            continue;
        }
        if (pending_space) g_string_append_c(out, ' ');
        pending_space = FALSE;
        g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

static ns_box *
pseudo_block_with_text(const ns_style *ps, const char *txt)
{
    ns_box *block = box_new(NS_BOX_BLOCK);
    block->style = ps;
    collect_box_bg_image(block, ps);
    char *text = txt ? pseudo_block_text(ps, txt) : NULL;
    if (text && *text) {
        ns_box *txtrun = box_new_inline();
        txtrun->text = text;
        box_append_child(block, txtrun);
    } else {
        g_free(text);
    }
    return block;
}

static void
pseudo_box_add_edge_spacers(ns_box *box, const ns_style *ps)
{
    double edges[4] = {
        length_or(ps->values[NS_CSS_MARGIN_LEFT], 0),
        length_or(ps->values[NS_CSS_PADDING_LEFT], 0),
        length_or(ps->values[NS_CSS_PADDING_RIGHT], 0),
        length_or(ps->values[NS_CSS_MARGIN_RIGHT], 0),
    };
    static const char spacer[] = "\xef\xbf\xbc";
    gsize slen = sizeof spacer - 1;
    gsize lead = (edges[0] > 0 ? slen : 0) + (edges[1] > 0 ? slen : 0);
    if (lead == 0 && !(edges[2] > 0) && !(edges[3] > 0)) return;
    GString *text = g_string_new(NULL);
    for (guint i = 0; i < box->attrs->len; i++)
        g_array_index(box->attrs, ns_inline_attr, i).start += lead;
    for (int e = 0; e < 4; e++) {
        if (e == 2) g_string_append(text, box->text);
        if (!(edges[e] > 0)) continue;
        ns_inline_attr a = {
            .kind = NS_INLINE_SPACER,
            .start = text->len,
            .len = slen,
            .box_w = edges[e],
        };
        g_string_append(text, spacer);
        g_array_append_val(box->attrs, a);
    }
    g_free(box->text);
    box->text = g_string_free(text, FALSE);
}

static ns_box *
build_pseudo_inline_for(const ns_style *ps, const ns_node *host,
                        const ns_style *host_style)
{
    if (!pseudo_generates_box(ps)) return NULL;
    const ns_css_value *cv = ps->values[NS_CSS_CONTENT];
    gboolean inline_atomic = ns_display_is_atomic_inline(ns_css_display_of(ps));
    char *resolved = resolve_pseudo_content(cv->u.keyword, host);
    if (!resolved) {
        if (!inline_atomic) return NULL;
        resolved = g_strdup("");
    }
    char *quote = NULL;
    const char *txt = resolved;
    if (strcmp(txt, "open-quote") == 0)
        txt = quote = quotes_string_for(ps, 0, FALSE);
    else if (strcmp(txt, "close-quote") == 0)
        txt = quote = quotes_string_for(ps, 0, TRUE);
    else if (strcmp(txt, "no-open-quote") == 0 ||
             strcmp(txt, "no-close-quote") == 0) { g_free(resolved); return NULL; }

    if (inline_atomic) {
        ns_box *inner = pseudo_block_with_text(ps, txt);
        g_free(quote);
        g_free(resolved);
        ns_box *run = box_new_inline();
        run->style = ps;
        run->text = g_strdup("\xef\xbf\xbc");
        run->inline_atomics = g_array_new(FALSE, FALSE, sizeof(ns_inline_atomic));
        ns_inline_atomic ia = {
            .byte_off = 0, .box = inner,
            .rise_px = vertical_align_script_rise(ps, host_style),
        };
        inner->parent = run;
        g_array_append_val(run->inline_atomics, ia);
        return run;
    }

    ns_box *box = box_new_inline();
    box->text = g_strdup(txt);
    g_free(quote);
    g_free(resolved);
    box->style = ps;

    gsize tlen = strlen(box->text);
    emit_generated_attr(box->attrs, 0, tlen, ps);
    const char *vis = ns_style_keyword(ps, NS_CSS_VISIBILITY);
    gboolean invisible = vis && (strcmp(vis, "hidden") == 0 ||
                                 strcmp(vis, "collapse") == 0);
    const ns_css_value *pseudo_color = ns_style_text_fill_color(ps);
    if (invisible || (pseudo_color && pseudo_color->kind == NS_CSS_V_COLOR)) {
        const ns_css_value *color = pseudo_color;
        gboolean have = color && color->kind == NS_CSS_V_COLOR;
        ns_inline_attr a = {
            .kind = NS_INLINE_COLOR,
            .start = 0, .len = tlen,
            .r = have ? color->u.color.r : 0,
            .g = have ? color->u.color.g : 0,
            .b = have ? color->u.color.b : 0,
            .a = invisible ? 0 : color->u.color.a,
        };
        g_array_append_val(box->attrs, a);
    }
    if (ps->values[NS_CSS_BACKGROUND_COLOR] &&
        ps->values[NS_CSS_BACKGROUND_COLOR]->kind == NS_CSS_V_COLOR) {
        ns_inline_attr a = {
            .kind = NS_INLINE_BG_COLOR,
            .start = 0, .len = tlen,
            .r = ps->values[NS_CSS_BACKGROUND_COLOR]->u.color.r,
            .g = ps->values[NS_CSS_BACKGROUND_COLOR]->u.color.g,
            .b = ps->values[NS_CSS_BACKGROUND_COLOR]->u.color.b,
            .a = ps->values[NS_CSS_BACKGROUND_COLOR]->u.color.a,
        };
        g_array_append_val(box->attrs, a);
    }
    if (ps->values[NS_CSS_FONT_SIZE] &&
        ps->values[NS_CSS_FONT_SIZE]->kind == NS_CSS_V_LENGTH &&
        ps->values[NS_CSS_FONT_SIZE]->u.length.unit == NS_CSS_UNIT_PX) {
        ns_inline_attr a = {
            .kind = NS_INLINE_FONT_SIZE,
            .start = 0, .len = tlen,
            .font_size_px = ps->values[NS_CSS_FONT_SIZE]->u.length.v,
        };
        g_array_append_val(box->attrs, a);
    }
    const ns_css_value *fw = ps->values[NS_CSS_FONT_WEIGHT];
    int font_weight = ns_css_font_weight_number(fw, -1);
    if (font_weight > 0) {
        ns_inline_attr a = {
            .kind = NS_INLINE_FONT_WEIGHT,
            .start = 0, .len = tlen,
            .font_weight = font_weight,
        };
        g_array_append_val(box->attrs, a);
    }
    if (ps->values[NS_CSS_FONT_STRETCH]) {
        ns_inline_attr a = {
            .kind = NS_INLINE_FONT_STRETCH,
            .start = 0, .len = tlen,
            .font_stretch =
                ns_css_font_stretch_rank(ps->values[NS_CSS_FONT_STRETCH]),
        };
        g_array_append_val(box->attrs, a);
    }
    int fk = font_kerning_int_from_style(ps);
    const char *flig = font_ligatures_from_style(ps);
    const char *ffea = font_feature_settings_from_style(ps);
    if (fk >= 0 || flig || ffea) {
        ns_inline_attr a = {
            .kind = NS_INLINE_FONT_FEATURES,
            .start = 0, .len = tlen,
            .font_kerning = fk,
            .font_ligatures = flig,
            .font_features = ffea,
        };
        g_array_append_val(box->attrs, a);
    }
    const char *fvar = font_variation_settings_from_style(ps);
    if (fvar) {
        ns_inline_attr a = {
            .kind = NS_INLINE_FONT_VARIATIONS,
            .start = 0, .len = tlen,
            .font_variations = fvar,
        };
        g_array_append_val(box->attrs, a);
    }
    if (keyword_is(ps->values[NS_CSS_FONT_STYLE], "italic") ||
        keyword_is(ps->values[NS_CSS_FONT_STYLE], "oblique")) {
        ns_inline_attr a = { .kind = NS_INLINE_ITALIC, .start = 0, .len = tlen };
        g_array_append_val(box->attrs, a);
    }
    pseudo_box_add_edge_spacers(box, ps);
    return box;
}

static int g_build_block_depth;

static const ns_node *
layout_shadow_root(const ns_node *host)
{
    if (!host || host->kind != NS_NODE_ELEMENT) return NULL;
    for (const ns_node *c = host->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_ELEMENT && ns_element_has_shadow_marker(c))
            return c;
    return NULL;
}

static const ns_node *
layout_slot_host(const ns_node *slot)
{
    for (const ns_node *p = slot ? slot->parent : NULL; p; p = p->parent)
        if (p->kind == NS_NODE_ELEMENT && ns_element_has_shadow_marker(p))
            return p->parent;
    return NULL;
}

static const ns_node *
layout_find_slot(const ns_node *scope, const char *name, int depth)
{
    if (depth >= NS_LAYOUT_MAX_DEPTH) return NULL;
    for (const ns_node *c = scope ? scope->first_child : NULL; c;
         c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || ns_element_has_shadow_marker(c))
            continue;
        if (c->name && strcmp(c->name, "slot") == 0) {
            const char *slot_name = ns_element_get_attr(c, "name");
            if (g_strcmp0(slot_name ? slot_name : "", name) == 0) return c;
        }
        const ns_node *found = layout_find_slot(c, name, depth + 1);
        if (found) return found;
    }
    return NULL;
}

static const ns_node *
layout_flat_parent(const ns_node *n)
{
    const ns_node *p = n ? n->parent : NULL;
    if (!p || n->kind != NS_NODE_ELEMENT) return p;
    const ns_node *sr = layout_shadow_root(p);
    if (!sr || sr == n) return p;
    const char *name = ns_element_get_attr(n, "slot");
    const ns_node *slot = layout_find_slot(sr, name ? name : "", 0);
    return slot ? slot : p;
}

static ns_box *build_block_impl(const ns_node *n, GHashTable *styles);
static void apply_position_offsets(ns_box *box, double parent_w,
                                   double parent_h);
static void process_absolute_boxes(ns_box *root, GHashTable *styles,
                                   double viewport_width);
static void compute_paint_bounds(ns_box *b);
static void negative_layers_scan_root(ns_box *root);

static double
svg_foreign_width(const ns_node *fo, double fallback)
{
    const char *a = ns_element_get_attr(fo, "width");
    if (!a || !*a || strchr(a, '%')) return fallback;
    char *end = NULL;
    double v = g_ascii_strtod(a, &end);
    return end != a && v > 0 ? v : fallback;
}

static void extend_scroll_ranges_from_abs(ns_box *b);

static ns_box *
layout_svg_foreign_object(const ns_node *fo, GHashTable *styles, double width)
{
    GArray *saved_pending = g_abs_pending;
    GHashTable *saved_seen = g_abs_seen;
    GHashTable *saved_ph_set = g_abs_ph_set;
    GHashTable *saved_static = g_abs_static;
    gboolean saved_force = g_abs_force_build;
    g_abs_pending = g_array_new(FALSE, FALSE, sizeof(ns_abs_entry));
    g_abs_seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_abs_ph_set = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_abs_static = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, g_free);
    g_abs_force_build = FALSE;

    ns_box *root = box_new(NS_BOX_BLOCK);
    root->dom = fo;
    for (const ns_node *c = fo->first_child; c; c = c->next_sibling) {
        const ns_style *cs = c->kind == NS_NODE_ELEMENT
            ? g_hash_table_lookup(styles, c) : NULL;
        if (style_is_contents(cs)) {
            append_display_contents_children(root, c, styles, FALSE, NULL);
            continue;
        }
        ns_box *child = build_block(c, styles);
        if (child) box_append_child(root, child);
    }
    root->x = 0;
    root->y = 0;
    layout_block(root, width, NULL);
    apply_position_offsets(root, width, root->content_height);
    process_absolute_boxes(root, styles, width);
    ns_paint_sync_inline_atomic_offsets(root);
    extend_scroll_ranges_from_abs(root);
    compute_paint_bounds(root);
    negative_layers_scan_root(root);

    g_array_free(g_abs_pending, TRUE);
    g_hash_table_destroy(g_abs_seen);
    g_hash_table_destroy(g_abs_ph_set);
    g_hash_table_destroy(g_abs_static);
    g_abs_pending = saved_pending;
    g_abs_seen = saved_seen;
    g_abs_ph_set = saved_ph_set;
    g_abs_static = saved_static;
    g_abs_force_build = saved_force;
    return root;
}

static void
collect_svg_foreign_objects(ns_box *svg_box, const ns_node *n,
                            GHashTable *styles, double fallback_width)
{
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || !c->name) continue;
        const ns_style *cs = g_hash_table_lookup(styles, c);
        if (cs && style_is_none(cs)) continue;
        if (strcmp(c->name, "foreignObject") == 0) {
            if (!g_abs_pending) continue;
            ns_box_media *m = ns_box_media_ensure(svg_box);
            if (!m->svg_foreign) m->svg_foreign = g_ptr_array_new();
            g_ptr_array_add(m->svg_foreign,
                layout_svg_foreign_object(c, styles,
                    svg_foreign_width(c, fallback_width)));
            continue;
        }
        collect_svg_foreign_objects(svg_box, c, styles, fallback_width);
    }
}

static void
build_svg_foreign_objects(ns_box *svg_box, const ns_node *svg,
                          GHashTable *styles, double width)
{
    if (!styles) return;
    collect_svg_foreign_objects(svg_box, svg, styles, width > 0 ? width : 300);
}

static ns_box *
build_block(const ns_node *n, GHashTable *styles)
{
    if (!n || g_build_block_depth >= NS_LAYOUT_MAX_DEPTH) return NULL;
    g_build_block_depth++;
    ns_box *out = build_block_impl(n, styles);
    g_build_block_depth--;
    return out;
}

static int g_contents_depth;

static gboolean
node_skips_rendering(const ns_node *c)
{
    return c->kind == NS_NODE_ELEMENT && c->name &&
           tag_is_non_rendering(c->name);
}

static gboolean
blockified_child_is_block(const ns_node *c, const ns_style *cs,
                          GHashTable *styles)
{
    static const char *const block_tags[] = {
        "img", "svg", "audio", "video", "table", NULL,
    };
    return (cs && style_is_absolute_or_fixed(cs)) || style_is_block(cs) ||
           contains_block_media(c, styles) || node_has_media_metadata(c) ||
           name_in(c->name, block_tags);
}

static void
append_blockified_text(ns_box *block, const ns_node *c, GHashTable *styles)
{
    if (text_is_ws_only(c->text)) return;
    ns_box *item = box_new(NS_BOX_BLOCK);
    item->style = NULL;
    ns_box *run = build_inline_run(c, c->next_sibling, styles);
    if (run && run->text && run->text[0]) {
        box_append_child(item, run);
        box_append_child(block, item);
    } else {
        if (run) ns_box_free(run);
        ns_box_free(item);
    }
}

static void
append_blockified_child(ns_box *block, const ns_node *c, GHashTable *styles,
                        ns_box **pending_before)
{
    if (c->kind == NS_NODE_TEXT) {
        append_blockified_text(block, c, styles);
        return;
    }
    if (c->kind != NS_NODE_ELEMENT) return;
    const ns_style *cs = g_hash_table_lookup(styles, c);
    if (cs && style_is_none(cs)) return;
    if (style_is_contents(cs)) {
        append_display_contents_children(block, c, styles, TRUE, NULL);
        return;
    }
    ns_box *child = blockified_child_is_block(c, cs, styles)
        ? build_block(c, styles)
        : build_blockified_inline_item(c, styles, pending_before);
    if (child) box_append_child(block, child);
}

static const ns_node *
inline_run_end(const ns_node *c, const ns_node *end, GHashTable *styles)
{
    while (c && c != end) {
        if (node_skips_rendering(c)) {
            c = c->next_sibling;
            continue;
        }
        if (c->kind == NS_NODE_ELEMENT &&
            style_is_contents(g_hash_table_lookup(styles, c)))
            break;
        if (!continues_inline_run(c, styles)) break;
        c = c->next_sibling;
    }
    return c;
}

static const ns_node *
append_flow_child(ns_box *block, const ns_node *c, const ns_node *end,
                  GHashTable *styles)
{
    if (c->kind == NS_NODE_ELEMENT) {
        const ns_style *cs = g_hash_table_lookup(styles, c);
        if (cs && style_is_none(cs)) return c->next_sibling;
        if (style_is_contents(cs)) {
            append_display_contents_children(block, c, styles, FALSE, NULL);
            return c->next_sibling;
        }
    }
    if (is_inline_dom(c, styles)) {
        const ns_node *stop = inline_run_end(c->next_sibling, end, styles);
        ns_box *run = build_inline_run(c, stop, styles);
        if (inline_run_worth_keeping(run))
            box_append_child(block, run);
        else if (run)
            ns_box_free(run);
        return stop;
    }
    ns_box *child = build_block(c, styles);
    if (child) box_append_child(block, child);
    return c->next_sibling;
}

static void
append_contents_range(ns_box *block, const ns_node *first, const ns_node *end,
                      GHashTable *styles, gboolean blockify_children,
                      ns_box **pending_before)
{
    const ns_node *c = first;
    while (c && c != end) {
        if (node_skips_rendering(c)) {
            c = c->next_sibling;
        } else if (blockify_children) {
            append_blockified_child(block, c, styles, pending_before);
            c = c->next_sibling;
        } else {
            c = append_flow_child(block, c, end, styles);
        }
    }
}

static gboolean
slot_assigns_node(const ns_node *lc, const char *slot_name)
{
    if (lc->kind == NS_NODE_TEXT) return !slot_name || !*slot_name;
    if (lc->kind != NS_NODE_ELEMENT) return FALSE;
    const char *assigned = ns_element_get_attr(lc, "slot");
    return g_strcmp0(assigned ? assigned : "", slot_name ? slot_name : "") == 0;
}

static gboolean
append_slot_assigned(ns_box *block, const ns_node *slot, GHashTable *styles,
                     gboolean blockify_children, ns_box **pending_before)
{
    if (!slot || !slot->name || strcmp(slot->name, "slot") != 0) return FALSE;
    const ns_node *host = layout_slot_host(slot);
    const ns_node *sr = host ? layout_shadow_root(host) : NULL;
    if (!sr) return FALSE;
    const char *slot_name = ns_element_get_attr(slot, "name");
    gboolean any = FALSE;
    const ns_node *lc = host->first_child;
    while (lc) {
        if (lc == sr || !slot_assigns_node(lc, slot_name)) {
            lc = lc->next_sibling;
            continue;
        }
        const ns_node *range_end = lc->next_sibling;
        while (range_end && range_end != sr &&
               slot_assigns_node(range_end, slot_name))
            range_end = range_end->next_sibling;
        append_contents_range(block, lc, range_end, styles, blockify_children,
                              pending_before);
        any = TRUE;
        lc = range_end;
    }
    return any;
}

static void
append_node_children(ns_box *block, const ns_node *n, GHashTable *styles,
                     gboolean blockify_children, ns_box **pending_before)
{
    const ns_node *shadow_root = layout_shadow_root(n);
    if (!append_slot_assigned(block, n, styles, blockify_children,
                              pending_before))
        append_contents_range(block,
                              shadow_root ? shadow_root->first_child
                                          : n->first_child,
                              NULL, styles, blockify_children, pending_before);
}

static void
append_display_contents_children(ns_box *block, const ns_node *n,
                                 GHashTable *styles,
                                 gboolean blockify_children,
                                 ns_box **pending_before)
{
    if (g_contents_depth >= NS_LAYOUT_MAX_DEPTH) return;
    g_contents_depth++;
    const ns_style *s = g_hash_table_lookup(styles, n);
    ns_box *contents_before = (s && s->before &&
                               !style_is_absolute_or_fixed(s->before))
        ? build_pseudo_inline_for(s->before, n, s) : NULL;
    if (pending_before && *pending_before) {
        contents_before = inline_merge_prefix(*pending_before, contents_before);
        *pending_before = NULL;
    }
    if (contents_before)
        box_append_child(block, contents_before);

    append_node_children(block, n, styles, blockify_children, pending_before);

    if (s && s->after && !style_is_absolute_or_fixed(s->after)) {
        ns_box *contents_after = build_pseudo_inline_for(s->after, n, s);
        if (contents_after) box_append_child(block, contents_after);
    }
    g_contents_depth--;
}

static ns_box *
pseudo_item_block(ns_box *run, const ns_style *ps)
{
    ns_box *item = box_new(NS_BOX_BLOCK);
    item->style = ps;
    collect_box_bg_image(item, ps);
    for (guint i = run->attrs ? run->attrs->len : 0; i-- > 0;)
        if (g_array_index(run->attrs, ns_inline_attr, i).kind ==
            NS_INLINE_BG_COLOR)
            g_array_remove_index(run->attrs, i);
    box_append_child(item, run);
    return item;
}

static ns_box *
build_pseudo_block_for(const ns_style *ps, const ns_node *host)
{
    if (!ps) return NULL;
    if (style_is_absolute_or_fixed(ps) || !pseudo_generates_box(ps)) return NULL;
    const ns_css_value *cv = ps->values[NS_CSS_CONTENT];
    ns_display d = ns_css_display_of(ps);
    if (!ps->values[NS_CSS_DISPLAY] || ns_display_is_none(d) ||
        d.outer == NS_DISPLAY_OUTER_INLINE)
        return NULL;
    char *resolved = *cv->u.keyword
        ? resolve_pseudo_content(cv->u.keyword, host) : g_strdup("");
    if (!resolved) return NULL;
    char *quote = NULL;
    const char *txt = resolved;
    if (strcmp(txt, "open-quote") == 0)
        txt = quote = quotes_string_for(ps, 0, FALSE);
    else if (strcmp(txt, "close-quote") == 0)
        txt = quote = quotes_string_for(ps, 0, TRUE);
    else if (strcmp(txt, "no-open-quote") == 0 ||
             strcmp(txt, "no-close-quote") == 0)
        txt = "";
    ns_box *pb = pseudo_block_with_text(ps, txt);
    g_free(quote);
    g_free(resolved);
    return pb;
}

static void
register_abs_pseudo(const ns_node *host, const ns_style *ps)
{
    if (!ps || !g_abs_pending) return;
    if (!style_is_absolute_or_fixed(ps) || !pseudo_generates_box(ps)) return;
    for (guint i = 0; i < g_abs_pending->len; i++) {
        const ns_abs_entry *seen = &g_array_index(g_abs_pending, ns_abs_entry, i);
        if (seen->dom == host && seen->pseudo == ps) return;
    }
    ns_abs_entry e;
    e.dom = host;
    e.pseudo = ps;
    const ns_css_value *pv = ps->values[NS_CSS_POSITION];
    e.fixed = pv && pv->kind == NS_CSS_V_KEYWORD && pv->u.keyword &&
              strcmp(pv->u.keyword, "fixed") == 0;
    g_array_append_val(g_abs_pending, e);
}

static ns_box *
build_blockified_inline_item(const ns_node *n, GHashTable *styles,
                             ns_box **pending_before)
{
    const ns_style *s = g_hash_table_lookup(styles, n);
    ns_box *item = box_new(NS_BOX_BLOCK);
    item->dom = n;
    item->style = s;
    collect_box_bg_image(item, s);

    if (s) {
        register_abs_pseudo(n, s->before);
        register_abs_pseudo(n, s->after);
    }

    ns_box *before_block = (s && s->before)
        ? build_pseudo_block_for(s->before, n) : NULL;
    if (before_block) box_append_child(item, before_block);
    ns_box *after_block = (s && s->after)
        ? build_pseudo_block_for(s->after, n) : NULL;

    const ns_node *saved_host = g_pseudo_blocks_host;
    gboolean saved_before = g_pseudo_block_before;
    gboolean saved_after = g_pseudo_block_after;
    g_pseudo_blocks_host = n;
    g_pseudo_block_before = before_block != NULL;
    g_pseudo_block_after = after_block != NULL;
    ns_box *run = build_inline_run_no_abs_placeholders(n, n->next_sibling,
                                                       styles);
    g_pseudo_blocks_host = saved_host;
    g_pseudo_block_before = saved_before;
    g_pseudo_block_after = saved_after;
    if (pending_before && *pending_before) {
        run = inline_merge_prefix(*pending_before, run);
        *pending_before = NULL;
    }
    if (run && run->text && run->text[0]) {
        box_append_child(item, run);
    } else if (run) {
        ns_box_free(run);
    }

    if (after_block) box_append_child(item, after_block);

    if (!item->first_child && !style_has_atomic_inline_box(s)) {
        ns_box_free(item);
        return NULL;
    }
    return item;
}

static const ns_node *g_blockified_legend;

static gboolean
style_can_be_rendered_legend(const ns_style *s)
{
    return s && !style_is_none(s) && float_side_of(s) < 0 &&
           !style_is_absolute_or_fixed(s);
}

static const ns_node *
fieldset_rendered_legend_node(const ns_node *n, GHashTable *styles)
{
    if (!ns_node_is_element_named(n, "fieldset")) return NULL;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (!ns_node_is_element_named(c, "legend")) continue;
        if (style_can_be_rendered_legend(g_hash_table_lookup(styles, c)))
            return c;
    }
    return NULL;
}

static ns_box *
build_rendered_legend(const ns_node *n, GHashTable *styles)
{
    const ns_node *saved = g_blockified_legend;
    g_blockified_legend = n;
    ns_box *legend = build_block(n, styles);
    g_blockified_legend = saved;
    if (!legend) {
        legend = box_new(NS_BOX_BLOCK);
        legend->dom = n;
        legend->style = g_hash_table_lookup(styles, n);
    }
    legend->is_rendered_legend = TRUE;
    return legend;
}

static ns_box *
build_block_impl(const ns_node *n, GHashTable *styles)
{
    if (!n) return NULL;
    if (n->kind == NS_NODE_DOCUMENT) {
        const char *saved_base = g_base_url_for_layout;
        if (n->parent && n->parent->kind == NS_NODE_ELEMENT &&
            n->parent->name &&
            (strcmp(n->parent->name, "iframe") == 0 ||
             strcmp(n->parent->name, "frame") == 0 ||
             strcmp(n->parent->name, "object") == 0)) {
            const char *fu = ns_element_get_attr(n->parent,
                                                 "data-nd-frame-url");
            if (fu && *fu) g_base_url_for_layout = fu;
        }
        ns_box *root = box_new(NS_BOX_BLOCK);
        for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
            const ns_style *cs = c->kind == NS_NODE_ELEMENT
                ? g_hash_table_lookup(styles, c) : NULL;
            if (style_is_contents(cs)) {
                append_display_contents_children(root, c, styles, FALSE, NULL);
                continue;
            }
            ns_box *child = build_block(c, styles);
            if (child) box_append_child(root, child);
        }
        g_base_url_for_layout = saved_base;
        return root;
    }
    if (n->kind != NS_NODE_ELEMENT) return NULL;
    if (node_is_frame_fallback(n)) return NULL;
    if (ns_element_has_shadow_marker(n)) return NULL;

    const ns_style *s = g_hash_table_lookup(styles, n);
    if (s && style_is_none(s)) return NULL;
    if (s && style_is_absolute_or_fixed(s)) {
        if (!g_abs_force_build) {
            if (g_abs_pending &&
                (!g_abs_seen || g_hash_table_add(g_abs_seen, (gpointer)n))) {
                ns_abs_entry e;
                e.dom = n;
                e.pseudo = NULL;
                const ns_css_value *pv = s->values[NS_CSS_POSITION];
                e.fixed = pv && pv->kind == NS_CSS_V_KEYWORD && pv->u.keyword &&
                          strcmp(pv->u.keyword, "fixed") == 0;
                g_array_append_val(g_abs_pending, e);
            }
            return NULL;
        }
        g_abs_force_build = FALSE;
    }

    if (n->name && strcmp(n->name, "br") == 0) {
        if (!clear_kind_of(s)) return NULL;
        ns_box *clearance = box_new(NS_BOX_BLOCK);
        clearance->dom = n;
        clearance->style = s;
        return clearance;
    }

    if (n->name && strcmp(n->name, "img") == 0) {
        ns_box *ib = build_image_box(n);
        if (ib) ib->style = s;
        return ib;
    }

    if (n->name && strcmp(n->name, "math") == 0) {
        ns_box *mb = box_new(NS_BOX_MATH);
        mb->dom = n;
        mb->style = s;
        return mb;
    }

    if (n->name && strcmp(n->name, "input") == 0) {
        if (s && (style_is_absolute_or_fixed(s) || style_is_block(s)))
            return build_form_control_block(n, s, styles);
        ns_box *ir = build_inline_run(n, n->next_sibling, styles);
        if (ir) return ir;
    }

    if (n->name && strcmp(n->name, "textarea") == 0) {
        if (s && (style_is_absolute_or_fixed(s) || style_is_block(s)))
            return build_form_control_block(n, s, styles);
        ns_box *ir = build_inline_run(n, n->next_sibling, styles);
        if (ir) return ir;
    }

    if (n->name && strcmp(n->name, "select") == 0) {
        if (s && (style_is_absolute_or_fixed(s) || style_is_block(s)))
            return build_form_control_block(n, s, styles);
        ns_box *ir = build_inline_run(n, n->next_sibling, styles);
        if (ir) return ir;
    }

    if (n->name && strcmp(n->name, "svg") == 0) {
        ns_box *box = box_new(NS_BOX_SVG);
        box->dom = n;
        box->style = s;
        box->svg_styles = styles;
        ns_box_media *m = ns_box_media_ensure(box);

        ns_svg_size size;
        ns_svg_intrinsic_size(n, &size);

        double css_w = -1, css_h = -1;
        if (s) {
            const ns_css_value *wv = s->values[NS_CSS_WIDTH];
            const ns_css_value *hv = s->values[NS_CSS_HEIGHT];
            if (wv && wv->kind == NS_CSS_V_LENGTH &&
                wv->u.length.unit == NS_CSS_UNIT_PX)
                css_w = wv->u.length.v;
            if (hv && hv->kind == NS_CSS_V_LENGTH &&
                hv->u.length.unit == NS_CSS_UNIT_PX)
                css_h = hv->u.length.v;
        }

        double w = css_w > 0 ? css_w : (size.has_width  ? size.width  : 0);
        double h = css_h > 0 ? css_h : (size.has_height ? size.height : 0);
        if (w > 0 && h <= 0 && size.has_ratio) h = w / size.ratio;
        else if (h > 0 && w <= 0 && size.has_ratio) w = h * size.ratio;

        gboolean root_document = !n->parent || n->parent->kind == NS_NODE_DOCUMENT;
        if (root_document) {
            double vw = ns_css_viewport_w();
            double vh = ns_css_viewport_h();
            if (w <= 0 && vw > 0) w = vw;
            if (h <= 0 && vh > 0) h = vh;
        }

        if (w <= 0 && h <= 0 && size.has_ratio && !root_document) {
            m->intrinsic_ratio_only = TRUE;
            w = 0;
            h = 0;
        } else if (w <= 0 && h <= 0) { w = 300; h = 150; }
        else if (w <= 0) w = 300;
        else if (h <= 0) h = 150;

        box->content_width  = w;
        box->content_height = h;
        m->declared_image_size = w > 0 && h > 0;
        build_svg_foreign_objects(box, n, styles, w);
        return box;
    }

    if (n->name && strcmp(n->name, "canvas") == 0) {
        ns_box *cb = box_new(NS_BOX_IMAGE);
        cb->dom = n;
        cb->style = s;
        ns_box_media *cm = ns_box_media_ensure(cb);
        cb->content_width  = image_dimension_attr(n, "width");
        cb->content_height = image_dimension_attr(n, "height");
        if (cb->content_width  <= 0) cb->content_width  = 300;
        if (cb->content_height <= 0) cb->content_height = 150;
        cm->declared_image_size = TRUE;
        return cb;
    }

    if (n->name && strcmp(n->name, "audio") == 0) {
        ns_box *ab = build_audio_box(n);
        if (ab) ab->style = s;
        return ab;
    }

    if (n->name && strcmp(n->name, "video") == 0) {
        ns_box *vb = build_video_box(n);
        if (vb) {
            vb->style = s;
            collect_box_bg_image(vb, s);
        }
        return vb;
    }

    if (node_has_media_metadata(n)) {
        gboolean has_src = ns_element_get_attr(n, NS_MEDIA_SRC_ATTR) != NULL;
        gboolean empty_skeleton = TRUE;
        for (const ns_node *mc = n->first_child; mc; mc = mc->next_sibling)
            if (mc->kind == NS_NODE_ELEMENT) { empty_skeleton = FALSE; break; }
        if (has_src || empty_skeleton) {
            ns_box *vb = build_video_box(n);
            if (vb) {
                vb->style = s;
                collect_box_bg_image(vb, s);
            }
            return vb;
        }
    }

    if (is_table_box(n, styles))
        return build_table(n, styles);

    if (n->name && strcmp(n->name, "slot") == 0) {
        const ns_node *host = layout_slot_host(n);
        const ns_node *sr = host ? layout_shadow_root(host) : NULL;
        const char *slot_name = ns_element_get_attr(n, "name");
        ns_box *sbox = box_new(NS_BOX_BLOCK);
        sbox->dom = n;
        sbox->style = s;
        gboolean any = FALSE;
        if (host && sr) {
            for (const ns_node *lc = host->first_child; lc; lc = lc->next_sibling) {
                if (lc == sr) continue;
                const char *cs = (lc->kind == NS_NODE_ELEMENT)
                    ? ns_element_get_attr(lc, "slot") : NULL;
                if (g_strcmp0(cs ? cs : "", slot_name ? slot_name : "") != 0)
                    continue;
                ns_box *cb;
                if (is_inline_dom(lc, styles)) {
                    cb = build_inline_run(lc, lc->next_sibling, styles);
                    if (cb && !(cb->text && cb->text[0])) {
                        ns_box_free(cb);
                        cb = NULL;
                    }
                } else {
                    cb = build_block(lc, styles);
                }
                if (cb) { box_append_child(sbox, cb); any = TRUE; }
            }
        }
        if (any) return sbox;
        ns_box_free(sbox);
    }

    if (!style_is_block(s) && !contains_block_media(n, styles) &&
        !style_is_absolute_or_fixed(s) && n != g_blockified_legend) return NULL;

    ns_box *block = box_new(NS_BOX_BLOCK);
    block->dom = n;
    block->style = s;
    const ns_node *rendered_legend = fieldset_rendered_legend_node(n, styles);

    collect_box_bg_image(block, s);

    gboolean details_collapsed = FALSE;
    if (n->name && strcmp(n->name, "details") == 0 &&
        !ns_element_get_attr(n, "open"))
        details_collapsed = TRUE;

    if (s) {
        register_abs_pseudo(n, s->before);
        register_abs_pseudo(n, s->after);
    }

    ns_box *before_block = (s && s->before)
        ? build_pseudo_block_for(s->before, n) : NULL;
    if (before_block) box_append_child(block, before_block);

    ns_box *pending_before = (s && s->before && !before_block &&
                              !style_is_absolute_or_fixed(s->before))
        ? build_pseudo_inline_for(s->before, n, s) : NULL;

    gboolean blockify_children = style_is_flex_container(s) ||
                                 style_is_grid_container(s);
    if (blockify_children && pending_before) {
        box_append_child(block, pseudo_item_block(pending_before, s->before));
        pending_before = NULL;
    }

    const ns_node *saved_skip = g_inline_skip_node;
    if (rendered_legend) {
        ns_box *legend = build_rendered_legend(rendered_legend, styles);
        if (legend) box_append_child(block, legend);
        g_inline_skip_node = rendered_legend;
    }

    const ns_node *shadow_host_root = layout_shadow_root(n);
    const ns_node *c = shadow_host_root ? shadow_host_root->first_child
                                        : n->first_child;
    while (c) {
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            tag_is_non_rendering(c->name)) {
            c = c->next_sibling;
            continue;
        }
        if (details_collapsed) {
            if (c->kind != NS_NODE_ELEMENT || !c->name ||
                strcmp(c->name, "summary") != 0) {
                c = c->next_sibling;
                continue;
            }
        }
        if (c == rendered_legend) {
            c = c->next_sibling;
            continue;
        }
        if (blockify_children) {
            if (c->kind == NS_NODE_TEXT) {
                if (text_is_ws_only(c->text)) { c = c->next_sibling; continue; }
                ns_box *item = box_new(NS_BOX_BLOCK);
                item->style = NULL;
                ns_box *run = build_inline_run(c, c->next_sibling, styles);
                if (pending_before) {
                    run = inline_merge_prefix(pending_before, run);
                    pending_before = NULL;
                }
                if (run && run->text && run->text[0]) {
                    box_append_child(item, run);
                    box_append_child(block, item);
                } else {
                    if (run) ns_box_free(run);
                    ns_box_free(item);
                }
                c = c->next_sibling;
                continue;
            }
            if (c->kind != NS_NODE_ELEMENT) { c = c->next_sibling; continue; }
            const ns_style *cs = g_hash_table_lookup(styles, c);
            if (cs && style_is_none(cs)) { c = c->next_sibling; continue; }
            if (style_is_contents(cs)) {
                append_display_contents_children(block, c, styles, TRUE,
                                                 &pending_before);
                c = c->next_sibling;
                continue;
            }
            if (cs && style_is_absolute_or_fixed(cs)) {
                ns_box *child = build_block(c, styles);
                if (child) box_append_child(block, child);
                c = c->next_sibling;
                continue;
            }
            if (style_is_block(cs) ||
                contains_block_media(c, styles) ||
                node_has_media_metadata(c) ||
                (c->name && (strcmp(c->name, "img") == 0 ||
                             strcmp(c->name, "svg") == 0 ||
                             strcmp(c->name, "audio") == 0 ||
                             strcmp(c->name, "video") == 0 ||
                             strcmp(c->name, "table") == 0))) {
                ns_box *child = build_block(c, styles);
                if (child) box_append_child(block, child);
                c = c->next_sibling;
                continue;
            }
            ns_box *item = build_blockified_inline_item(c, styles,
                                                        &pending_before);
            if (item) box_append_child(block, item);
            c = c->next_sibling;
            continue;
        }
        if (c->kind == NS_NODE_ELEMENT) {
            const ns_style *cs = g_hash_table_lookup(styles, c);
            if (style_is_contents(cs)) {
                append_display_contents_children(block, c, styles, FALSE,
                                                 &pending_before);
                c = c->next_sibling;
                continue;
            }
        }
        if (node_is_table_internal(c, styles)) {
            const ns_node *start = c;
            c = table_internal_run_end(c, styles);
            if (pending_before) {
                box_append_child(block, pending_before);
                pending_before = NULL;
            }
            ns_box *anon = build_anonymous_table(start, c, styles);
            if (anon) box_append_child(block, anon);
            continue;
        }
        if (continues_inline_run(c, styles)) {
            const ns_node *start = c;
            c = c->next_sibling;
            while (c) {
                if (c->kind == NS_NODE_ELEMENT && c->name &&
                    tag_is_non_rendering(c->name)) {
                    c = c->next_sibling;
                    continue;
                }
                if (c == rendered_legend) {
                    c = c->next_sibling;
                    continue;
                }
                if (details_collapsed &&
                    (c->kind != NS_NODE_ELEMENT || !c->name ||
                     strcmp(c->name, "summary") != 0)) break;
                if (c->kind == NS_NODE_ELEMENT) {
                    const ns_style *cs = g_hash_table_lookup(styles, c);
                    if (style_is_contents(cs)) break;
                }
                if (!continues_inline_run(c, styles)) break;
                c = c->next_sibling;
            }
            if (inline_range_waits_for_next(start, c, styles)) continue;
            ns_box *run = inline_merge_prefix(
                pending_before, build_inline_run(start, c, styles));
            pending_before = NULL;
            if (inline_run_worth_keeping(run))
                box_append_child(block, run);
            else
                ns_box_free(run);
        } else {
            flush_pending_before(block, &pending_before, c, styles);
            ns_box *child = build_block(c, styles);
            if (child) box_append_child(block, child);
            if (c) c = c->next_sibling;
        }
    }
    g_inline_skip_node = saved_skip;

    if (pending_before) {
        box_append_child(block, pending_before);
        pending_before = NULL;
    }

    ns_box *after_block = (s && s->after)
        ? build_pseudo_block_for(s->after, n) : NULL;
    if (after_block) box_append_child(block, after_block);

    if (s && s->after && !after_block &&
        !style_is_absolute_or_fixed(s->after)) {
        ns_box *gen = build_pseudo_inline_for(s->after, n, s);
        if (gen && blockify_children) {
            box_append_child(block, pseudo_item_block(gen, s->after));
            gen = NULL;
        }
        if (gen) append_generated_after(block, gen);
    } else if (s && s->after && !blockify_children &&
               style_is_absolute_or_fixed(s->after) &&
               !keyword_is(s->after->values[NS_CSS_POSITION], "fixed")) {
        ns_box *ph = abs_pseudo_placeholder_run(block, s->after);
        if (ph) append_generated_after(block, ph);
    }
    return block;
}

static NsPangoLayout *
make_pango_layout(const ns_style *parent_style)
{
    NsPangoLayout *layout = ns_pango_layout_new(ns_paint_text_context());
    ns_paint_apply_inline_font(layout, parent_style);
    return layout;
}

typedef struct ns_text_measure {
    NsPangoRectangle logical;
    int              lines;
    int              baseline;
} ns_text_measure;

#define NS_TEXT_MEASURE_CACHE_MAX 32768
#define NS_ASCII_MIN_WIDTH_MAX_WORDS 64

typedef struct text_measure_key {
    guint                   hash;
    guint                   len;
    guint8                 *data;
    NsPangoFontDescription *font;
} text_measure_key;

typedef struct text_measure_head {
    guint serial;
    int   width, height, indent, spacing, strut_line_height;
    float line_spacing;
    guint flags;
    guint tabs_len;
    guint attrs_len;
} text_measure_head;

static GHashTable *g_text_measure_cache;
static GByteArray *g_text_measure_scratch;

static guint
text_measure_key_hash(gconstpointer p)
{
    return ((const text_measure_key *)p)->hash;
}

static gboolean
text_measure_key_equal(gconstpointer pa, gconstpointer pb)
{
    const text_measure_key *a = pa, *b = pb;
    if (a->hash != b->hash || a->len != b->len ||
        memcmp(a->data, b->data, a->len) != 0)
        return FALSE;
    if (!a->font || !b->font) return a->font == b->font;
    return ns_pango_font_description_equal(a->font, b->font);
}

static void
text_measure_key_free(gpointer p)
{
    text_measure_key *k = p;
    g_free(k->data);
    if (k->font) ns_pango_font_description_free(k->font);
    g_free(k);
}

typedef struct {
    GByteArray *buf;
    gboolean    uncacheable;
} text_measure_attr_sink;

static void
text_measure_attr_append_str(GByteArray *buf, const char *str)
{
    guint32 n = str ? (guint32)strlen(str) : 0;
    g_byte_array_append(buf, (const guint8 *)&n, sizeof n);
    if (n) g_byte_array_append(buf, (const guint8 *)str, n);
}

static gboolean
text_measure_attr_append(NsPangoAttribute *attr, gpointer data)
{
    text_measure_attr_sink *sink = data;
    if (sink->uncacheable) return FALSE;
    GByteArray *buf = sink->buf;
    guint32 head[3] = { (guint32)attr->klass->type, attr->start_index,
                        attr->end_index };
    g_byte_array_append(buf, (const guint8 *)head, sizeof head);
    NsPangoAttrInt *ai;
    NsPangoAttrSize *as;
    NsPangoAttrFloat *af;
    NsPangoAttrString *astr;
    NsPangoAttrColor *ac;
    NsPangoAttrLanguage *al;
    NsPangoAttrFontDesc *afd;
    NsPangoAttrFontFeatures *aff;
    if (attr->klass->type == NS_PANGO_ATTR_SHAPE) {
        const NsPangoAttrShape *sh = (const NsPangoAttrShape *)attr;
        gint32 v[8] = { sh->ink_rect.x, sh->ink_rect.y,
                        sh->ink_rect.width, sh->ink_rect.height,
                        sh->logical_rect.x, sh->logical_rect.y,
                        sh->logical_rect.width, sh->logical_rect.height };
        g_byte_array_append(buf, (const guint8 *)v, sizeof v);
    } else if ((as = ns_pango_attribute_as_size(attr))) {
        gint32 v[2] = { as->size, as->absolute };
        g_byte_array_append(buf, (const guint8 *)v, sizeof v);
    } else if ((ai = ns_pango_attribute_as_int(attr))) {
        gint32 v = ai->value;
        g_byte_array_append(buf, (const guint8 *)&v, sizeof v);
    } else if ((af = ns_pango_attribute_as_float(attr))) {
        g_byte_array_append(buf, (const guint8 *)&af->value, sizeof af->value);
    } else if ((astr = ns_pango_attribute_as_string(attr))) {
        text_measure_attr_append_str(buf, astr->value);
    } else if ((ac = ns_pango_attribute_as_color(attr))) {
        guint16 v[3] = { ac->color.red, ac->color.green, ac->color.blue };
        g_byte_array_append(buf, (const guint8 *)v, sizeof v);
    } else if ((al = ns_pango_attribute_as_language(attr))) {
        text_measure_attr_append_str(buf, ns_pango_language_to_string(al->value));
    } else if ((afd = ns_pango_attribute_as_font_desc(attr))) {
        char *desc = ns_pango_font_description_to_string(afd->desc);
        text_measure_attr_append_str(buf, desc);
        g_free(desc);
    } else if ((aff = ns_pango_attribute_as_font_features(attr))) {
        text_measure_attr_append_str(buf, aff->features);
    } else {
        sink->uncacheable = TRUE;
    }
    return FALSE;
}

static gboolean
text_measure_tabs_append(GByteArray *buf, NsPangoTabArray *tabs)
{
    gint32 head[2] = { ns_pango_tab_array_get_size(tabs),
                       ns_pango_tab_array_get_positions_in_pixels(tabs) };
    g_byte_array_append(buf, (const guint8 *)head, sizeof head);
    for (gint32 i = 0; i < head[0]; i++) {
        NsPangoTabAlign align;
        gint location;
        ns_pango_tab_array_get_tab(tabs, i, &align, &location);
        if (align == NS_PANGO_TAB_DECIMAL) return FALSE;
        gint32 v[2] = { (gint32)align, location };
        g_byte_array_append(buf, (const guint8 *)v, sizeof v);
    }
    return TRUE;
}

static gboolean
text_measure_key_build(NsPangoLayout *layout, text_measure_key *probe)
{
    NsPangoAttrList *attrs = ns_pango_layout_get_attributes(layout);
    text_measure_head head;
    memset(&head, 0, sizeof head);
    head.serial = ns_pango_context_get_serial(ns_pango_layout_get_context(layout));
    head.width = ns_pango_layout_get_width(layout);
    head.height = ns_pango_layout_get_height(layout);
    head.indent = ns_pango_layout_get_indent(layout);
    head.spacing = ns_pango_layout_get_spacing(layout);
    head.line_spacing = ns_pango_layout_get_line_spacing(layout);
#ifdef NS_USE_WEB_TEXT_LIB
    head.strut_line_height = ns_pango_layout_get_strut_line_height(layout);
#endif
    head.flags = (guint)ns_pango_layout_get_justify(layout) |
                 (guint)ns_pango_layout_get_single_paragraph_mode(layout) << 2 |
                 (guint)ns_pango_layout_get_auto_dir(layout) << 3 |
                 (guint)ns_pango_layout_get_alignment(layout) << 4 |
                 (guint)ns_pango_layout_get_wrap(layout) << 8 |
                 (guint)ns_pango_layout_get_ellipsize(layout) << 12;
    if (!g_text_measure_scratch) g_text_measure_scratch = g_byte_array_new();
    GByteArray *buf = g_text_measure_scratch;
    g_byte_array_set_size(buf, 0);
    g_byte_array_append(buf, (const guint8 *)&head, sizeof head);
    NsPangoTabArray *tabs = ns_pango_layout_get_tabs(layout);
    if (tabs) {
        gboolean keyed = text_measure_tabs_append(buf, tabs);
        ns_pango_tab_array_free(tabs);
        if (!keyed) return FALSE;
        head.tabs_len = buf->len - (guint)sizeof head;
    }
    if (attrs) {
        text_measure_attr_sink sink = { buf, FALSE };
        NsPangoAttrList *none = ns_pango_attr_list_filter(
            attrs, text_measure_attr_append, &sink);
        if (none) ns_pango_attr_list_unref(none);
        if (sink.uncacheable) return FALSE;
        head.attrs_len = buf->len - (guint)sizeof head - head.tabs_len;
    }
    memcpy(buf->data, &head, sizeof head);
    const char *text = ns_pango_layout_get_text(layout);
    g_byte_array_append(buf, (const guint8 *)text, (guint)strlen(text));
    guint32 h = 2166136261u;
    for (guint i = 0; i < buf->len; i++) h = (h ^ buf->data[i]) * 16777619u;
    const NsPangoFontDescription *fd = ns_pango_layout_get_font_description(layout);
    if (fd) h ^= ns_pango_font_description_hash(fd) * 0x9e3779b1u;
    probe->hash = h;
    probe->len = buf->len;
    probe->data = buf->data;
    probe->font = (NsPangoFontDescription *)fd;
    return TRUE;
}

typedef struct text_measure_entry {
    ns_text_measure m;
    int            *line_heights;
} text_measure_entry;

static void
text_measure_entry_free(gpointer p)
{
    text_measure_entry *e = p;
    g_free(e->line_heights);
    g_free(e);
}

static int *
text_measure_compute_line_heights(NsPangoLayout *layout, int lines)
{
    int *heights = g_new0(int, MAX(lines, 1));
    NsPangoLayoutIter *it = ns_pango_layout_get_iter(layout);
    int j = 0;
    do {
        NsPangoRectangle logical;
        ns_pango_layout_iter_get_line_extents(it, NULL, &logical);
        if (j < MAX(lines, 1)) heights[j] = logical.height;
        j++;
    } while (ns_pango_layout_iter_next_line(it));
    ns_pango_layout_iter_free(it);
    return heights;
}

static int *g_text_measure_uncached_heights;

static const int *
text_measure_lines(NsPangoLayout *layout, ns_text_measure *m,
                   gboolean want_line_heights)
{
    text_measure_key probe;
    gboolean keyed = text_measure_key_build(layout, &probe);
    text_measure_entry *hit = keyed && g_text_measure_cache
        ? g_hash_table_lookup(g_text_measure_cache, &probe) : NULL;
    if (hit) {
        *m = hit->m;
        if (!want_line_heights) return NULL;
        if (!hit->line_heights)
            hit->line_heights = text_measure_compute_line_heights(layout, m->lines);
        return hit->line_heights;
    }
    ns_pango_layout_get_extents(layout, NULL, &m->logical);
    m->lines = ns_pango_layout_get_line_count(layout);
    m->baseline = ns_pango_layout_get_baseline(layout);
    int *heights = want_line_heights
        ? text_measure_compute_line_heights(layout, m->lines) : NULL;
    if (!keyed) {
        g_free(g_text_measure_uncached_heights);
        g_text_measure_uncached_heights = heights;
        return heights;
    }
    if (!g_text_measure_cache)
        g_text_measure_cache = g_hash_table_new_full(
            text_measure_key_hash, text_measure_key_equal,
            text_measure_key_free, text_measure_entry_free);
    if (g_hash_table_size(g_text_measure_cache) >= NS_TEXT_MEASURE_CACHE_MAX)
        g_hash_table_remove_all(g_text_measure_cache);
    text_measure_key *key = g_new(text_measure_key, 1);
    key->hash = probe.hash;
    key->len = probe.len;
    key->data = g_memdup2(probe.data, probe.len);
    key->font = probe.font ? ns_pango_font_description_copy(probe.font) : NULL;
    text_measure_entry *entry = g_new(text_measure_entry, 1);
    entry->m = *m;
    entry->line_heights = heights;
    g_hash_table_insert(g_text_measure_cache, key, entry);
    return heights;
}

static void
text_measure(NsPangoLayout *layout, ns_text_measure *m)
{
    text_measure_lines(layout, m, FALSE);
}

static void
text_measure_pixel_size(NsPangoLayout *layout, int *width, int *height)
{
    ns_text_measure m;
    text_measure(layout, &m);
    ns_pango_extents_to_pixels(&m.logical, NULL);
    if (width) *width = m.logical.width;
    if (height) *height = m.logical.height;
}

static void
inline_unspace_placeholders(NsPangoAttrList *list, const char *text)
{
    for (const char *p = strstr(text, "\xef\xbf\xbc"); p;
         p = strstr(p + 3, "\xef\xbf\xbc")) {
        NsPangoAttribute *none = ns_pango_attr_letter_spacing_new(0);
        none->start_index = (guint)(p - text);
        none->end_index = none->start_index + 3;
        ns_pango_attr_list_insert(list, none);
    }
}

void
ns_inline_apply_spacing(NsPangoAttrList *list, const ns_style *style, const char *text)
{
    if (!list || !style || !text) return;
    double ls_px = 0, ws_px = 0;
    const ns_css_value *lv = style->values[NS_CSS_LETTER_SPACING];
    if (lv && lv->kind == NS_CSS_V_LENGTH && lv->u.length.unit == NS_CSS_UNIT_PX)
        ls_px = lv->u.length.v;
    const ns_css_value *wv = style->values[NS_CSS_WORD_SPACING];
    if (wv && wv->kind == NS_CSS_V_LENGTH && wv->u.length.unit == NS_CSS_UNIT_PX)
        ws_px = wv->u.length.v;
    if (ls_px != 0) {
        NsPangoAttribute *ls = ns_pango_attr_letter_spacing_new(
            (int)(ls_px * NS_PANGO_SCALE));
        ls->start_index = 0;
        ls->end_index = G_MAXUINT;
        ns_pango_attr_list_insert(list, ls);
    }
    if (ls_px > 0) inline_unspace_placeholders(list, text);
    if (ws_px != 0) {
        int per_space = (int)((ls_px + ws_px) * NS_PANGO_SCALE);
        for (const char *p = text; *p; p++) {
            if (*p == ' ') {
                gsize idx = (gsize)(p - text);
                NsPangoAttribute *a = ns_pango_attr_letter_spacing_new(per_space);
                a->start_index = (guint)idx;
                a->end_index = (guint)(idx + 1);
                ns_pango_attr_list_insert(list, a);
            }
        }
    }
}

static void
layout_attr_insert_range(NsPangoAttrList *attrs, NsPangoAttribute *a,
                         gsize start, gsize len)
{
    if (!a || len == 0) return;
    a->start_index = (guint)start;
    a->end_index = (guint)(start + len);
    ns_pango_attr_list_insert(attrs, a);
}

static void
apply_inline_layout_attrs(NsPangoAttrList *attrs, const ns_box *box)
{
    if (!attrs || !box || !box->attrs) return;
    for (gint ii = (gint)box->attrs->len - 1; ii >= 0; ii--) {
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, (guint)ii);
        if ((r->kind == NS_INLINE_INPUT_FIELD ||
             r->kind == NS_INLINE_INPUT_FIELD_FOCUSED ||
             r->kind == NS_INLINE_BUTTON) &&
            !(r->dom && r->dom->name &&
              strcmp(r->dom->name, "textarea") == 0))
            layout_attr_insert_range(attrs,
                ns_pango_attr_allow_breaks_new(FALSE), r->start, r->len);
        layout_attr_insert_range(attrs, ns_paint_inline_font_attr(r),
                                 r->start, r->len);
    }
}

static gboolean
field_attr_is_text_input(const ns_inline_attr *r)
{
    if (r->kind != NS_INLINE_INPUT_FIELD &&
        r->kind != NS_INLINE_INPUT_FIELD_FOCUSED)
        return FALSE;
    const ns_node *n = r->dom;
    if (!n || !n->name || strcmp(n->name, "input") != 0) return FALSE;
    const char *type = ns_element_get_attr(n, "type");
    return !type || !*type ||
        g_ascii_strcasecmp(type, "text") == 0 ||
        g_ascii_strcasecmp(type, "search") == 0 ||
        g_ascii_strcasecmp(type, "email") == 0 ||
        g_ascii_strcasecmp(type, "url") == 0 ||
        g_ascii_strcasecmp(type, "tel") == 0 ||
        g_ascii_strcasecmp(type, "number") == 0 ||
        g_ascii_strcasecmp(type, "password") == 0;
}

gboolean
ns_inline_toggle_geometry(const ns_inline_attr *r, double *side,
                          double margins[4])
{
    if (r->kind != NS_INLINE_CHECKBOX && r->kind != NS_INLINE_CHECKBOX_CHECKED &&
        r->kind != NS_INLINE_RADIO && r->kind != NS_INLINE_RADIO_CHECKED)
        return FALSE;
    double edge = 13;
    if (r->box_w > 0 || r->box_h > 0) {
        double bw = r->box_w > 0 ? r->box_w : r->box_h;
        double bh = r->box_h > 0 ? r->box_h : r->box_w;
        edge = MIN(bw, bh);
    }
    *side = edge;
    static const ns_css_prop sides[4] = {
        NS_CSS_MARGIN_TOP, NS_CSS_MARGIN_RIGHT,
        NS_CSS_MARGIN_BOTTOM, NS_CSS_MARGIN_LEFT,
    };
    for (int i = 0; i < 4; i++)
        margins[i] = r->style ? length_or(r->style->values[sides[i]], 0) : 0;
    return TRUE;
}

gboolean
ns_inline_range_geometry(const ns_inline_attr *r, const ns_box *box,
                         double *w, double *h, double margins[4])
{
    if (r->kind != NS_INLINE_RANGE) return FALSE;
    double css_w = ns_inline_attr_control_width(r, box);
    *w = css_w > 0 ? css_w : 129;
    *h = r->box_h > 0 ? r->box_h : 16;
    static const ns_css_prop sides[4] = {
        NS_CSS_MARGIN_TOP, NS_CSS_MARGIN_RIGHT,
        NS_CSS_MARGIN_BOTTOM, NS_CSS_MARGIN_LEFT,
    };
    for (int i = 0; i < 4; i++)
        margins[i] = r->style ? length_or(r->style->values[sides[i]], 2) : 2;
    return TRUE;
}

static void
inline_apply_toggle_shapes(NsPangoAttrList *list, const ns_box *box)
{
    if (!box->attrs) return;
    for (guint i = 0; i < box->attrs->len; i++) {
        const ns_inline_attr *r = &g_array_index(box->attrs, ns_inline_attr, i);
        double side, m[4], ww, hh;
        if (r->len == 0) continue;
        if (ns_inline_toggle_geometry(r, &side, m)) {
            ww = hh = side;
        } else if (!ns_inline_range_geometry(r, box, &ww, &hh, m)) {
            continue;
        }
        double w = MAX(0, m[3] + ww + m[1]);
        double h = MAX(0, m[0] + hh + m[2]);
        NsPangoRectangle rect = {
            0, (int)(-(m[0] + hh) * NS_PANGO_SCALE),
            (int)(w * NS_PANGO_SCALE), (int)(h * NS_PANGO_SCALE)
        };
        NsPangoAttribute *attr = ns_pango_attr_shape_new(&rect, &rect);
        attr->start_index = (guint)r->start;
        attr->end_index = (guint)(r->start + r->len);
        ns_pango_attr_list_insert(list, attr);
    }
}

void
ns_inline_apply_atomic_shapes(NsPangoAttrList *list, const ns_box *box)
{
    if (!box) return;
    inline_apply_toggle_shapes(list, box);
    if (box->inline_atomics) {
        double max_asc = 0;
        for (guint i = 0; i < box->inline_atomics->len; i++) {
            const ns_inline_atomic *a =
                &g_array_index(box->inline_atomics, ns_inline_atomic, i);
            const ns_box *ab = a->box;
            if (!ab) continue;
            double h = ab->margin.top + ab->border.top + ab->padding.top +
                       ab->content_height +
                       ab->padding.bottom + ab->border.bottom + ab->margin.bottom;
            if (h < 0) h = 0;
            double fs = length_or(ab->style ? ab->style->values[NS_CSS_FONT_SIZE]
                                            : NULL, 16);
            double xh = fs * 0.5;
            const char *va = ab->style
                ? ns_style_keyword(ab->style, NS_CSS_VERTICAL_ALIGN) : NULL;
            double a_asc = h;
            double ab_baseline;
            if (!box_clips_children(ab) && box_first_baseline(ab, &ab_baseline))
                a_asc = ab->margin.top + ab_baseline;
            if (va) {
                if (strcmp(va, "middle") == 0)      a_asc = h / 2 + xh / 2;
                else if (a->rise_px != 0)           a_asc = h + a->rise_px;
                else if (strcmp(va, "top") == 0 ||
                         strcmp(va, "text-top") == 0 ||
                         strcmp(va, "bottom") == 0 ||
                         strcmp(va, "text-bottom") == 0)
                    a_asc = fs * 0.8;
            }
            if (a_asc > max_asc) max_asc = a_asc;
        }
        for (guint i = 0; i < box->inline_atomics->len; i++) {
            const ns_inline_atomic *a =
                &g_array_index(box->inline_atomics, ns_inline_atomic, i);
            const ns_box *ab = a->box;
            if (!ab) continue;
            double w = ab->margin.left + ab->border.left + ab->padding.left +
                       ab->content_width +
                       ab->padding.right + ab->border.right + ab->margin.right;
            double h = ab->margin.top + ab->border.top + ab->padding.top +
                       ab->content_height +
                       ab->padding.bottom + ab->border.bottom + ab->margin.bottom;
            if (w < 0) w = 0;
            if (h < 0) h = 0;
            double fs = length_or(ab->style ? ab->style->values[NS_CSS_FONT_SIZE]
                                            : NULL, 16);
            double asc = fs * 0.8, desc = fs * 0.2, xh = fs * 0.5;
            double top = -h;
            double ab_baseline;
            if (!box_clips_children(ab) && box_first_baseline(ab, &ab_baseline))
                top = -(ab->margin.top + ab_baseline);
            if (ab->kind == NS_BOX_MATH) {
                double mw = 0, ma = 0, md = 0;
                ns_math_measure(ab->dom, fs, &mw, &ma, &md);
                top = -(ab->margin.top + ab->border.top + ab->padding.top + ma);
            }
            const char *va = ab->style
                ? ns_style_keyword(ab->style, NS_CSS_VERTICAL_ALIGN) : NULL;
            if (va) {
                double line_asc = max_asc > asc ? max_asc : asc;
                if (strcmp(va, "middle") == 0)           top = -(h / 2 + xh / 2);
                else if (strcmp(va, "text-top") == 0)    top = -asc;
                else if (strcmp(va, "top") == 0)         top = -line_asc;
                else if (strcmp(va, "text-bottom") == 0) top = desc - h;
                else if (strcmp(va, "bottom") == 0)      top = desc - h;
                else if (a->rise_px != 0)                top -= a->rise_px;
            }
            NsPangoRectangle r = { 0, (int)(top * NS_PANGO_SCALE),
                                 (int)(w * NS_PANGO_SCALE), (int)(h * NS_PANGO_SCALE) };
            NsPangoAttribute *attr = ns_pango_attr_shape_new(&r, &r);
            attr->start_index = (guint)a->byte_off;
            attr->end_index   = (guint)(a->byte_off + 3);
            ns_pango_attr_list_insert(list, attr);
        }
    }

}

static void
inline_insert_line_height_units(NsPangoAttrList *list, int units, guint start,
                                guint end)
{
    NsPangoAttribute *a = ns_pango_attr_line_height_new_absolute(units);
    a->start_index = start;
    a->end_index = end;
    ns_pango_attr_list_insert(list, a);
}

static gboolean inline_box_in_quirks_document(const ns_box *box);

static int
inline_line_height_units(double px)
{
    px = CLAMP(px, 0.0, (double)G_MAXINT16);
    return MAX(1, (int)lround(px * NS_PANGO_SCALE));
}

static void
inline_insert_line_height(NsPangoAttrList *list, double px, gboolean normal,
                          guint start, guint end)
{
    inline_insert_line_height_units(list, inline_line_height_units(px),
                                    start, end);
#ifdef NS_USE_WEB_TEXT_LIB
    NsPangoAttribute *a = ns_pango_attr_line_height_new(normal ? 1.0 : 0.0);
    a->start_index = start;
    a->end_index = end;
    ns_pango_attr_list_insert(list, a);
#else
    (void)normal;
#endif
}

static void
inline_insert_spacer_line_heights(NsPangoAttrList *list, const ns_box *box)
{
    for (guint i = 0; i < box->attrs->len; i++) {
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, i);
        gboolean toggle = r->kind == NS_INLINE_CHECKBOX ||
                          r->kind == NS_INLINE_CHECKBOX_CHECKED ||
                          r->kind == NS_INLINE_RANGE ||
                          r->kind == NS_INLINE_RADIO ||
                          r->kind == NS_INLINE_RADIO_CHECKED;
        if ((r->kind == NS_INLINE_SPACER || toggle) && r->len > 0)
            inline_insert_line_height_units(list, 0, (guint)r->start,
                                            (guint)(r->start + r->len));
    }
}

static gboolean
inline_attr_within_override(const ns_box *box, const double *px, guint i,
                            double strut_px)
{
    const ns_inline_attr *r = &g_array_index(box->attrs, ns_inline_attr, i);
    for (guint j = 0; j < box->attrs->len; j++) {
        if (j == i || px[j] < 0 || fabs(px[j] - strut_px) < 0.01) continue;
        const ns_inline_attr *o = &g_array_index(box->attrs, ns_inline_attr, j);
        if (o->start <= r->start && o->start + o->len >= r->start + r->len)
            return TRUE;
    }
    return FALSE;
}

static gboolean
inline_apply_line_heights(NsPangoAttrList *list, const ns_box *box,
                          const ns_css_line_height *strut)
{
    double strut_px = strut->px;
    inline_insert_line_height(list, strut_px, strut->normal, 0, G_MAXUINT);
    if (!box || !box->attrs) return FALSE;
    guint n = box->attrs->len;
    double *px = g_new(double, n);
    gboolean any_override = FALSE;
    for (guint i = 0; i < n; i++) {
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, i);
        px[i] = r->kind == NS_INLINE_ELEMENT && r->style && r->len > 0
            ? ns_paint_css_line_height_px(r->style) : -1;
        if (px[i] >= 0 && fabs(px[i] - strut_px) >= 0.01) any_override = TRUE;
    }
    gboolean has_shorter = FALSE;
    for (guint i = n; any_override && i-- > 0;) {
        if (px[i] < 0) continue;
        if (fabs(px[i] - strut_px) < 0.01 &&
            !inline_attr_within_override(box, px, i, strut_px))
            continue;
        if (px[i] < strut_px - 0.01) has_shorter = TRUE;
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, i);
        inline_insert_line_height(list, px[i],
                                  ns_paint_css_line_height_is_normal(r->style),
                                  (guint)r->start, (guint)(r->start + r->len));
    }
    g_free(px);
    inline_insert_spacer_line_heights(list, box);
    return has_shorter;
}

enum { STRUT_ROOT, STRUT_SHORTER, STRUT_SPACER };

static void
strut_mark_range(guint8 *kind, gsize n, const ns_inline_attr *r, guint8 k)
{
    for (gsize b = r->start; b < r->start + r->len && b < n; b++)
        kind[b] = k;
}

static guint8 *
inline_strut_kinds(const ns_box *box, gsize n, double strut_px)
{
    guint8 *kind = g_new0(guint8, n);
    for (guint i = box->attrs->len; i-- > 0;) {
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, i);
        if (r->kind != NS_INLINE_ELEMENT || !r->style) continue;
        double px = ns_paint_css_line_height_px(r->style);
        gboolean shorter = px >= 0 && px < strut_px - 0.01;
        strut_mark_range(kind, n, r, shorter ? STRUT_SHORTER : STRUT_ROOT);
    }
    for (guint i = 0; i < box->attrs->len; i++) {
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, i);
        if (r->kind == NS_INLINE_SPACER)
            strut_mark_range(kind, n, r, STRUT_SPACER);
    }
    return kind;
}

static gboolean
strut_line_lacks_root(const guint8 *kind, gsize s0, gsize s1)
{
    gboolean shorter = FALSE;
    for (gsize b = s0; b < s1; b++) {
        if (kind[b] == STRUT_ROOT) return FALSE;
        if (kind[b] == STRUT_SHORTER) shorter = TRUE;
    }
    return shorter;
}

static void
inline_restore_strut_lines(NsPangoLayout *layout, NsPangoAttrList *list,
                           const ns_box *box, const ns_css_line_height *strut)
{
    double strut_px = strut->px;
    gsize n = box->text ? strlen(box->text) : 0;
    if (n == 0) return;
    guint8 *kind = inline_strut_kinds(box, n, strut_px);
    if (!*ns_pango_layout_get_text(layout))
        ns_pango_layout_set_text(layout, box->text, -1);
    NsPangoAttrList *with_struts = NULL;
    NsPangoLayoutIter *it = ns_pango_layout_get_iter(layout);
    do {
        NsPangoLayoutLine *line = ns_pango_layout_iter_get_line_readonly(it);
        if (!line || line->length <= 0) continue;
        gsize s0 = (gsize)line->start_index;
        gsize s1 = MIN(n, s0 + (gsize)line->length);
        if (!strut_line_lacks_root(kind, s0, s1)) continue;
        if (!with_struts) with_struts = ns_pango_attr_list_copy(list);
        inline_insert_line_height(with_struts, strut_px, strut->normal,
                                  (guint)s0, (guint)s1);
    } while (ns_pango_layout_iter_next_line(it));
    ns_pango_layout_iter_free(it);
    g_free(kind);
    if (!with_struts) return;
    inline_insert_spacer_line_heights(with_struts, box);
    ns_pango_layout_set_attributes(layout, with_struts);
    ns_pango_attr_list_unref(with_struts);
}

void
ns_inline_layout_set_attrs(NsPangoLayout *layout, NsPangoAttrList *list,
                           const ns_box *box)
{
    const ns_css_line_height *strut = g_object_get_data(G_OBJECT(layout),
                                                        NS_CSS_LINE_HEIGHT_KEY);
    gboolean has_shorter = strut && list &&
                           inline_apply_line_heights(list, box, strut);
#ifdef NS_USE_WEB_TEXT_LIB
    if (strut && box && !inline_box_in_quirks_document(box)) {
        ns_pango_layout_set_strut_line_height(
            layout, inline_line_height_units(strut->px));
        has_shorter = FALSE;
    }
#endif
    ns_pango_layout_set_attributes(layout, list);
    if (!box || !box->attrs) return;
    gboolean stretched = FALSE;
    for (guint i = 0; i < box->attrs->len; i++) {
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, i);
        if (!field_attr_is_text_input(r)) continue;
        if (r->len < 4) continue;
        double css_w = ns_inline_attr_control_width(r, box);
        if (css_w <= 0) continue;
        NsPangoRectangle p0, p1;
        ns_pango_layout_index_to_pos(layout, (int)r->start, &p0);
        ns_pango_layout_index_to_pos(layout, (int)(r->start + r->len - 2), &p1);
        if (p1.y != p0.y) continue;
        double prefix = (double)(p1.x - p0.x) / NS_PANGO_SCALE;
        if (prefix < 0) continue;
        double fs = length_or(r->style ? r->style->values[NS_CSS_FONT_SIZE]
                                       : NULL, 16);
        double w = css_w - prefix;
        if (w < fs * 0.4) continue;
        NsPangoRectangle rect = { 0, (int)(-fs * 0.8 * NS_PANGO_SCALE),
                                (int)(w * NS_PANGO_SCALE), (int)(fs * NS_PANGO_SCALE) };
        NsPangoAttribute *attr = ns_pango_attr_shape_new(&rect, &rect);
        attr->start_index = (guint)(r->start + r->len - 2);
        attr->end_index   = (guint)(r->start + r->len);
        ns_pango_attr_list_insert(list, attr);
        stretched = TRUE;
    }
    if (stretched) ns_pango_layout_context_changed(layout);
    if (has_shorter) inline_restore_strut_lines(layout, list, box, strut);
}

static double
inline_line_height(const ns_style *parent_style)
{
    double font_size = length_or(parent_style ? parent_style->values[NS_CSS_FONT_SIZE] : NULL, 16);
    double used = ns_paint_css_line_height_px(parent_style);
    if (used >= 0) return used;
    return font_size * 1.2;
}

static double
inline_control_is_textarea(const ns_inline_attr *r)
{
    return r->dom && r->dom->name && strcmp(r->dom->name, "textarea") == 0;
}

static double
inline_control_line_height(const ns_box *box, double line_height)
{
    if (!box || !box->attrs) return line_height;
    double out = line_height;
    for (guint i = 0; i < box->attrs->len; i++) {
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, i);
        if (r->kind != NS_INLINE_INPUT_FIELD &&
            r->kind != NS_INLINE_INPUT_FIELD_FOCUSED &&
            r->kind != NS_INLINE_BUTTON)
            continue;
        if (inline_control_is_textarea(r)) continue;
        if (!r->native_chrome) continue;
        double cfs = 0;
        for (guint j = 0; j < box->attrs->len; j++) {
            const ns_inline_attr *f =
                &g_array_index(box->attrs, ns_inline_attr, j);
            if (f->kind != NS_INLINE_FONT_SIZE) continue;
            if (f->start <= r->start &&
                f->start + f->len >= r->start + r->len) {
                cfs = f->font_size_px;
                break;
            }
        }
        double font_box = cfs > 0 ? cfs * 1.3 + 12.0 : 0;
        double h = r->box_h > 0
                   ? r->box_h + (r->native_chrome ? 8.0 : 0.0)
                   : line_height + 18.0;
        if (font_box > h) h = font_box;
        if (h > out) out = h;
    }
    return out;
}

static gboolean
style_sets_block_height(const ns_style *s)
{
    const ns_css_value *hv = s->values[NS_CSS_HEIGHT];
    return hv && (hv->kind == NS_CSS_V_LENGTH || hv->kind == NS_CSS_V_CALC);
}

static double
inline_textarea_total_height(const ns_box *box, const ns_style *parent_style)
{
    if (!box || !box->attrs) return 0;
    double out = 0;
    for (guint i = 0; i < box->attrs->len; i++) {
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, i);
        if ((r->kind == NS_INLINE_INPUT_FIELD ||
             r->kind == NS_INLINE_INPUT_FIELD_FOCUSED) &&
            inline_control_is_textarea(r) && r->box_h > 0 &&
            !(r->style && r->style == parent_style &&
              style_sets_block_height(r->style))) {
            double h = r->box_h + (r->native_chrome ? 8.0 : 0.0);
            if (h > out) out = h;
        }
    }
    return out;
}

static gboolean
inline_attr_cacheable_kind(ns_inline_attr_kind k)
{
    switch (k) {
    case NS_INLINE_INPUT_FIELD:
    case NS_INLINE_INPUT_FIELD_FOCUSED:
    case NS_INLINE_BUTTON:
    case NS_INLINE_CHECKBOX:
    case NS_INLINE_CHECKBOX_CHECKED:
    case NS_INLINE_RADIO:
    case NS_INLINE_RADIO_CHECKED:
    case NS_INLINE_PROGRESS:
    case NS_INLINE_METER:
    case NS_INLINE_RANGE:
    case NS_INLINE_CARET:
    case NS_INLINE_SELECTION:
        return FALSE;
    default:
        return TRUE;
    }
}

static gboolean
inline_attr_affects_measure(ns_inline_attr_kind k)
{
    switch (k) {
    case NS_INLINE_BOLD:
    case NS_INLINE_ITALIC:
    case NS_INLINE_MONOSPACE:
    case NS_INLINE_FONT_SIZE:
    case NS_INLINE_FONT_WEIGHT:
    case NS_INLINE_FONT_STRETCH:
    case NS_INLINE_FONT_FEATURES:
    case NS_INLINE_FONT_VARIATIONS:
    case NS_INLINE_FONT_FAMILY:
    case NS_INLINE_SMALL_CAPS:
    case NS_INLINE_LETTER_SPACING:
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean
inline_box_measure_cacheable(const ns_box *box)
{
    if (!box || (box->inline_atomics && box->inline_atomics->len > 0))
        return FALSE;
    if (!box->attrs) return TRUE;
    for (guint i = 0; i < box->attrs->len; i++) {
        const ns_inline_attr *a =
            &g_array_index(box->attrs, ns_inline_attr, i);
        if (!inline_attr_cacheable_kind(a->kind)) return FALSE;
    }
    return TRUE;
}

static double measure_natural_width(ns_box *box, const ns_style *parent_style);
static double measure_max_content_width(ns_box *box, const ns_style *parent_style);
static double flex_gap_of(const ns_style *s, double basis);
static double flex_grow_of(const ns_box *c);
static double flex_shrink_of(const ns_box *c);
static gboolean flex_wraps(const ns_style *s);
typedef enum {
    NS_MEASURED_NONE,
    NS_MEASURED_MIN_CONTENT,
    NS_MEASURED_MAX_CONTENT,
} ns_measured_kind;

static double width_contribution_keyword_limits(ns_box *box, double w,
                                                const ns_style *parent_style,
                                                gboolean max_content,
                                                ns_measured_kind measured);
static gboolean flex_box_is_border_box(const ns_box *c);
static void legacy_align_block_child(ns_box *c, double avail_x, double avail_w,
                                     const ns_style *inherited);

static gboolean
inline_box_in_quirks_document(const ns_box *box)
{
    for (const ns_box *b = box; b; b = b->parent) {
        if (!b->dom) continue;
        const ns_node *n = b->dom;
        while (n->parent) n = n->parent;
        return n->kind == NS_NODE_DOCUMENT && (n->flags & NS_NODE_QUIRKS);
    }
    return FALSE;
}

#ifdef NS_USE_WEB_TEXT_LIB
static double
inline_font_legacy_ascent(NsPangoFont *font, double content_height)
{
    return (double)ns_pango_font_get_legacy_ascent(
        font, (int)lround(content_height * NS_PANGO_SCALE)) / NS_PANGO_SCALE;
}

static const NsPangoAttrShape *
inline_run_shape(const NsPangoLayoutRun *run)
{
    for (const GSList *l = run->item->analysis.extra_attrs; l; l = l->next) {
        const NsPangoAttribute *attr = l->data;
        if (attr->klass->type == NS_PANGO_ATTR_SHAPE)
            return (const NsPangoAttrShape *)attr;
    }
    return NULL;
}

static gboolean
inline_atomic_aligns_to_line_edge(const ns_box *box, gsize offset)
{
    for (guint i = 0; i < box->inline_atomics->len; i++) {
        const ns_inline_atomic *a =
            &g_array_index(box->inline_atomics, ns_inline_atomic, i);
        if (a->byte_off != offset) continue;
        if (!a->box || (g_abs_ph_set && g_hash_table_contains(g_abs_ph_set, a->box)))
            return TRUE;
        const char *va = a->box->style
            ? ns_style_keyword(a->box->style, NS_CSS_VERTICAL_ALIGN) : NULL;
        return va && (strcmp(va, "top") == 0 || strcmp(va, "bottom") == 0);
    }
    return FALSE;
}

static double
inline_run_line_height(const ns_box *box, gsize offset, double line_height,
                       gboolean normal, gboolean *out_normal)
{
    const ns_inline_attr *inner = NULL;
    for (guint i = 0; box->attrs && i < box->attrs->len; i++) {
        const ns_inline_attr *r = &g_array_index(box->attrs, ns_inline_attr, i);
        if (offset < r->start || offset >= r->start + r->len) continue;
        if (r->kind == NS_INLINE_SPACER || r->kind == NS_INLINE_CHECKBOX ||
            r->kind == NS_INLINE_CHECKBOX_CHECKED || r->kind == NS_INLINE_RANGE ||
            r->kind == NS_INLINE_RADIO || r->kind == NS_INLINE_RADIO_CHECKED)
            return -1;
        if (r->kind != NS_INLINE_ELEMENT || !r->style) continue;
        if (!inner || r->start > inner->start ||
            (r->start == inner->start && r->len < inner->len))
            inner = r;
    }
    double px = inner ? ns_paint_css_line_height_px(inner->style) : -1;
    *out_normal = px >= 0 ? ns_paint_css_line_height_is_normal(inner->style)
                          : normal;
    return px >= 0 ? px : line_height;
}

static gboolean
inline_line_css_extent(const ns_box *box, NsPangoLayoutLine *line,
                       double line_height, gboolean normal,
                       double *above, double *below)
{
    for (GSList *l = line ? line->runs : NULL; l; l = l->next) {
        NsPangoLayoutRun *run = l->data;
        double rise = (double)run->y_offset / NS_PANGO_SCALE;
        const NsPangoAttrShape *shape = inline_run_shape(run);
        if (shape) {
            if (inline_atomic_aligns_to_line_edge(box, (gsize)run->item->offset))
                return FALSE;
            double y = (double)shape->logical_rect.y / NS_PANGO_SCALE;
            double h = (double)shape->logical_rect.height / NS_PANGO_SCALE;
            *above = MAX(*above, rise - y);
            *below = MAX(*below, y + h - rise);
            continue;
        }
        gboolean run_normal = normal;
        double lh = inline_run_line_height(box, (gsize)run->item->offset,
                                           line_height, normal, &run_normal);
        if (lh < 0) continue;
        int run_above = 0, run_below = 0;
        ns_pango_layout_run_get_css_extents(run, inline_line_height_units(lh),
                                            run_normal, &run_above, &run_below);
        if (run_above + run_below <= 0) continue;
        *above = MAX(*above, (double)run_above / NS_PANGO_SCALE);
        *below = MAX(*below, (double)run_below / NS_PANGO_SCALE);
    }
    return TRUE;
}
#endif

static double *
inline_atomic_line_offsets(const ns_box *box, NsPangoLayout *layout,
                           const ns_style *parent_style, double line_height,
                           double *line_heights, int line_count)
{
    if (!box->inline_atomics || box->inline_atomics->len == 0 ||
        line_count < 1 || inline_box_in_quirks_document(box))
        return NULL;
    NsPangoLayout *strut = make_pango_layout(parent_style);
    ns_pango_layout_set_text(strut, " ", -1);
    NsPangoLayoutIter *strut_iter = ns_pango_layout_get_iter(strut);
    NsPangoRectangle strut_rect;
    ns_pango_layout_iter_get_line_extents(strut_iter, NULL, &strut_rect);
    double font_ascent = (double)(ns_pango_layout_iter_get_baseline(strut_iter) -
                                  strut_rect.y) / NS_PANGO_SCALE;
    double font_height = (double)strut_rect.height / NS_PANGO_SCALE;
#ifdef NS_USE_WEB_TEXT_LIB
    NsPangoLayoutLine *strut_line = ns_pango_layout_iter_get_line_readonly(strut_iter);
    NsPangoFont *strut_font = strut_line && strut_line->runs
        ? ((NsPangoLayoutRun *)strut_line->runs->data)->item->analysis.font
        : NULL;
    double content_ascent = round(font_ascent);
    double content_height = content_ascent + round(font_height - font_ascent);
    double legacy_ascent = inline_font_legacy_ascent(strut_font, content_height);
    content_ascent += legacy_ascent;
    content_height += legacy_ascent;
    double strut_ascent = content_ascent +
                          floor((line_height - content_height) / 2.0);
#endif
    ns_pango_layout_iter_free(strut_iter);
    g_object_unref(strut);
    if (font_height <= 0) return NULL;
#ifndef NS_USE_WEB_TEXT_LIB
    double strut_ascent = font_ascent + (line_height - font_height) / 2.0;
#endif
    double strut_descent = line_height - strut_ascent;
    gboolean *has_atomic = g_new0(gboolean, line_count);
    for (guint i = 0; i < box->inline_atomics->len; i++) {
        const ns_inline_atomic *a =
            &g_array_index(box->inline_atomics, ns_inline_atomic, i);
        if (!a->box || (g_abs_ph_set && g_hash_table_contains(g_abs_ph_set, a->box)))
            continue;
        const char *va = a->box->style
            ? ns_style_keyword(a->box->style, NS_CSS_VERTICAL_ALIGN) : NULL;
        if (va && (strcmp(va, "top") == 0 || strcmp(va, "bottom") == 0))
            continue;
        int line = 0;
        ns_pango_layout_index_to_line_x(layout, (int)a->byte_off, FALSE,
                                        &line, NULL);
        if (line >= 0 && line < line_count) has_atomic[line] = TRUE;
    }
    double *offsets = g_new0(double, line_count);
    NsPangoLayoutIter *iter = ns_pango_layout_get_iter(layout);
    for (int j = 0; j < line_count; j++) {
        NsPangoRectangle logical;
        ns_pango_layout_iter_get_line_extents(iter, NULL, &logical);
        double ascent = (double)(ns_pango_layout_iter_get_baseline(iter) -
                                 logical.y) / NS_PANGO_SCALE;
        double pango_h = (double)logical.height / NS_PANGO_SCALE;
#ifdef NS_USE_WEB_TEXT_LIB
        double above = strut_ascent, below = strut_descent;
        gboolean css_line = inline_line_css_extent(
            box, ns_pango_layout_iter_get_line_readonly(iter), line_height,
            ns_paint_css_line_height_is_normal(parent_style), &above, &below);
#else
        double above = 0, below = 0;
        gboolean css_line = FALSE;
#endif
        if (!css_line && has_atomic[j]) {
            above = MAX(strut_ascent, ascent);
            below = MAX(strut_descent, pango_h - ascent);
        }
        if (css_line || has_atomic[j]) {
            if (above + below > line_heights[j]) line_heights[j] = above + below;
            offsets[j] = above - ascent;
        } else {
            offsets[j] = (line_heights[j] - pango_h) / 2.0;
        }
        if (!ns_pango_layout_iter_next_line(iter)) break;
    }
    ns_pango_layout_iter_free(iter);
    g_free(has_atomic);
    return offsets;
}

static double
inline_atomic_outer_height(const ns_box *b)
{
    if (!b) return 0;
    return b->content_height + b->padding.top + b->padding.bottom +
           b->border.top + b->border.bottom + b->margin.top + b->margin.bottom;
}

static gboolean
inline_box_has_measure_attrs(const ns_box *box)
{
    if (!box || !box->attrs) return FALSE;
    for (guint i = 0; i < box->attrs->len; i++) {
        const ns_inline_attr *a =
            &g_array_index(box->attrs, ns_inline_attr, i);
        if (inline_attr_affects_measure(a->kind) ||
            ns_paint_inline_attr_nowrap(a))
            return TRUE;
    }
    return FALSE;
}

static gboolean
inline_style_has_measure_adjustments(const ns_style *style)
{
    const ns_css_value *lv = style ? style->values[NS_CSS_LETTER_SPACING] : NULL;
    if (lv && lv->kind == NS_CSS_V_LENGTH && lv->u.length.unit == NS_CSS_UNIT_PX &&
        fabs(lv->u.length.v) > 0.001)
        return TRUE;
    const ns_css_value *wv = style ? style->values[NS_CSS_WORD_SPACING] : NULL;
    if (wv && wv->kind == NS_CSS_V_LENGTH && wv->u.length.unit == NS_CSS_UNIT_PX &&
        fabs(wv->u.length.v) > 0.001)
        return TRUE;
    const char *fk = style ? ns_style_keyword(style, NS_CSS_FONT_KERNING) : NULL;
    if (fk && strcmp(fk, "auto") != 0 && strcmp(fk, "normal") != 0) return TRUE;
    const char *fl = style ? ns_style_keyword(style, NS_CSS_FONT_VARIANT_LIGATURES) : NULL;
    if (fl && strcmp(fl, "normal") != 0) return TRUE;
    const char *ff = style ? ns_style_keyword(style, NS_CSS_FONT_FEATURE_SETTINGS) : NULL;
    if (ff && strcmp(ff, "normal") != 0) return TRUE;
    const char *fv = style ? ns_style_keyword(style, NS_CSS_FONT_VARIATION_SETTINGS) : NULL;
    return fv && strcmp(fv, "normal") != 0;
}

static gboolean
inline_text_simple_ascii(const char *text)
{
    if (!text) return FALSE;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p >= 0x80) return FALSE;
    }
    return TRUE;
}

static double
measure_inline_ascii_min_width(ns_box *box, const ns_style *parent_style)
{
    if (!box || !box->text || !*box->text) return 0;
    if (box->inline_atomics && box->inline_atomics->len > 0) return -1;
    if (inline_box_has_measure_attrs(box)) return -1;
    if (inline_style_has_measure_adjustments(parent_style)) return -1;
    if (!inline_text_simple_ascii(box->text)) return -1;
    if (keyword_is(parent_style ? parent_style->values[NS_CSS_WHITE_SPACE] : NULL, "nowrap") ||
        keyword_is(parent_style ? parent_style->values[NS_CSS_WHITE_SPACE] : NULL, "pre"))
        return measure_natural_width(box, parent_style);

    gsize text_len = strlen(box->text);
    NsPangoLayout *layout = NULL;
    double widest = 0;
    int words = 0;
    const char *run = NULL;
    gsize run_len = 0;
    for (const char *p = box->text;; p++) {
        gboolean br = *p == '\0' || *p == ' ' || *p == '\t' ||
                      *p == '\r' || *p == '\n' || *p == '\f';
        if (!br) {
            if (!run) run = p;
            run_len++;
            continue;
        }
        if (run_len == text_len) return measure_natural_width(box, parent_style);
        if (run_len > 0) {
            if (++words > NS_ASCII_MIN_WIDTH_MAX_WORDS) {
                g_object_unref(layout);
                return -1;
            }
            if (!layout) {
                layout = make_pango_layout(parent_style);
                ns_pango_layout_set_width(layout, -1);
            }
            ns_pango_layout_set_text(layout, run, (int)run_len);
            ns_text_measure m;
            text_measure(layout, &m);
            double w = ceil((double)m.logical.width / NS_PANGO_SCALE);
            if (w > widest) widest = w;
        }
        run = NULL;
        run_len = 0;
        if (*p == '\0') break;
    }
    if (layout) g_object_unref(layout);
    return widest;
}

static void shift_box_tree(ns_box *b, double dx, double dy);

static void
ns_vertical_measure(ns_box *box, const ns_style *ps,
                    double *thickness, double *length)
{
    int orient = ns_css_text_orientation(ps);
    NsPangoLayout *layout = make_pango_layout(ps);
    int pw = 0, ph = 0;
    if (orient == 1) {
        char *stacked = ns_vertical_stack_text(box->text);
        ns_pango_layout_set_width(layout, -1);
        ns_pango_layout_set_alignment(layout, NS_PANGO_ALIGN_CENTER);
        ns_pango_layout_set_text(layout, stacked, -1);
        g_free(stacked);
        ns_pango_layout_get_pixel_size(layout, &pw, &ph);
        *thickness = pw;
        *length = ph;
    } else {
        ns_pango_layout_set_width(layout, -1);
        ns_paint_apply_css_line_spacing(layout, ps);
        NsPangoAttrList *i18n = ns_pango_attr_list_new();
        ns_paint_apply_i18n(layout, i18n, box);
        ns_paint_apply_font_features(i18n, ps, 0, G_MAXUINT);
        ns_inline_apply_spacing(i18n, ps, box->text);
        ns_inline_layout_set_attrs(layout, i18n, box);
        ns_pango_attr_list_unref(i18n);
        ns_pango_layout_set_text(layout, box->text, -1);
        ns_pango_layout_get_pixel_size(layout, &pw, &ph);
        *thickness = ph;
        *length = pw;
    }
    g_object_unref(layout);
}

static gboolean
box_first_baseline(const ns_box *b, double *out)
{
    if (!b) return FALSE;
    if (b->kind == NS_BOX_INLINE) {
        if (b->first_baseline <= 0) return FALSE;
        *out = b->margin.top + b->border.top + b->padding.top +
               b->first_baseline;
        return TRUE;
    }
    if (b->kind != NS_BOX_TABLE_ROW && style_has_layout_containment(b->style))
        return FALSE;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
        if (style_is_absolute_or_fixed(c->style) || c->is_rendered_legend)
            continue;
        double child_baseline;
        if (box_first_baseline(c, &child_baseline)) {
            *out = (c->y - b->y) + child_baseline;
            return TRUE;
        }
    }
    return FALSE;
}

static const char *
flex_item_align(const ns_box *c, const char *container_align)
{
    if (c && c->style) {
        const char *as = ns_style_keyword(c->style, NS_CSS_ALIGN_SELF);
        if (as && strcmp(as, "auto") != 0) return as;
    }
    return container_align;
}

static gboolean
flex_align_is_baseline(const char *align)
{
    return align && (strcmp(align, "baseline") == 0 ||
                     strcmp(align, "first baseline") == 0);
}

static double
flex_item_baseline(const ns_box *c, double fallback)
{
    if (c->parent && ns_css_writing_mode(c->parent->style) &&
        ns_css_writing_mode(c->style)) {
        double border_h = c->content_height + c->padding.top +
                          c->padding.bottom + c->border.top + c->border.bottom;
        return c->margin.top + border_h / 2.0;
    }
    double baseline;
    return box_first_baseline(c, &baseline) ? baseline : fallback;
}

static gboolean
inline_atomic_is_abs_placeholder_at(const ns_box *box, gsize byte)
{
    for (guint i = 0; i < box->inline_atomics->len; i++) {
        const ns_inline_atomic *a =
            &g_array_index(box->inline_atomics, ns_inline_atomic, i);
        if (a->byte_off == byte && a->box &&
            g_hash_table_contains(g_abs_ph_set, a->box))
            return TRUE;
    }
    return FALSE;
}

static gboolean
inline_run_holds_only_abs_placeholders(const ns_box *box,
                                       const ns_style *parent_style)
{
    if (!box->text || !box->inline_atomics || !g_abs_ph_set) return FALSE;
    const ns_css_value *ws = parent_style
        ? parent_style->values[NS_CSS_WHITE_SPACE] : NULL;
    if (ws && !keyword_is(ws, "normal") && !keyword_is(ws, "nowrap"))
        return FALSE;
    static const char placeholder[] = "\xef\xbf\xbc";
    const char *t = box->text;
    gsize i = 0;
    gboolean any = FALSE;
    while (t[i]) {
        if (t[i] == ' ' || t[i] == '\t') {
            i++;
        } else if (strncmp(t + i, placeholder, sizeof placeholder - 1) == 0 &&
                   inline_atomic_is_abs_placeholder_at(box, i)) {
            any = TRUE;
            i += sizeof placeholder - 1;
        } else {
            return FALSE;
        }
    }
    return any;
}

static void
inline_layout(ns_box *box, double content_width, const ns_style *parent_style)
{
    g_assert(box->kind == NS_BOX_INLINE);
    if (!box->text || !*box->text) {
        box->phantom_line = inline_run_has_empty_elements(box);
        box->content_width  = box->phantom_line ? content_width : 0;
        box->content_height = 0;
        box->first_baseline = 0;
        return;
    }

    box->vertical_wm = 0;
    box->text_orient = 0;
    if (ns_css_writing_mode(parent_style) &&
        !(box->inline_atomics && box->inline_atomics->len > 0)) {
        box->vertical_wm = ns_css_writing_mode(parent_style);
        box->text_orient = ns_css_text_orientation(parent_style);
        double thickness = 0, length = 0;
        ns_vertical_measure(box, parent_style, &thickness, &length);
        box->content_width  = thickness;
        box->content_height = length;
        box->first_baseline = 0;
        box->phantom_line = FALSE;
        box->inline_layout_cache_valid = FALSE;
        return;
    }

    double cb_width = g_inline_atomic_cb_w > content_width
        ? g_inline_atomic_cb_w : content_width;
    g_inline_atomic_cb_w = 0;
    for (guint i = 0; box->inline_atomics && i < box->inline_atomics->len; i++) {
        ns_box *ab = g_array_index(box->inline_atomics, ns_inline_atomic, i).box;
        if (!ab || (g_abs_ph_set && g_hash_table_contains(g_abs_ph_set, ab)))
            continue;
        gboolean replaced = ab->kind == NS_BOX_IMAGE ||
                            ab->kind == NS_BOX_VIDEO || ab->kind == NS_BOX_SVG;
        double basis = replaced ? content_width : cb_width;
        double w0 = ab->content_width, h0 = ab->content_height;
        layout_box(ab, basis, parent_style);
        if (ab->content_width != w0 || ab->content_height != h0)
            layout_box(ab, basis, parent_style);
    }

    gboolean cacheable = inline_box_measure_cacheable(box);
    if (cacheable && box->inline_layout_cache_valid &&
        box->inline_layout_cache_style == parent_style &&
        fabs(box->inline_layout_cache_width - content_width) < 0.001) {
        box->content_width = content_width;
        box->content_height = box->inline_layout_cache_height;
        return;
    }

    box->phantom_line = FALSE;
    box->content_width = content_width;
    NsPangoLayout *layout = make_pango_layout(parent_style);
    gboolean ws_nowrap = keyword_is(
        parent_style ? parent_style->values[NS_CSS_WHITE_SPACE] : NULL, "nowrap") ||
        keyword_is(parent_style ? parent_style->values[NS_CSS_WHITE_SPACE] : NULL, "pre");
    gboolean ellip = ns_paint_text_ellipsizes(parent_style);
    if (ws_nowrap && !ellip)
        ns_pango_layout_set_width(layout, -1);
    else
        ns_pango_layout_set_width(layout, (int)(content_width * NS_PANGO_SCALE));
    ns_pango_layout_set_wrap(layout, ns_paint_wrap_mode_for(parent_style));
    if (box->inline_atomics && box->inline_atomics->len > 0)
        ns_paint_apply_text_align(layout, parent_style);
    if (!(box->inline_atomics && box->inline_atomics->len > 0))
        ns_paint_apply_css_line_spacing(layout, parent_style);
    {
        double ti = ns_inline_text_indent_px(box, parent_style, content_width);
        if (ti > 0) ns_pango_layout_set_indent(layout, (int)(ti * NS_PANGO_SCALE));
    }
    if (ellip)
        ns_pango_layout_set_ellipsize(layout, NS_PANGO_ELLIPSIZE_END);
    {
        const ns_css_value *lc = parent_style ? parent_style->values[NS_CSS_LINE_CLAMP] : NULL;
        if (lc && lc->kind == NS_CSS_V_LENGTH && lc->u.length.v >= 1) {
            ns_pango_layout_set_height(layout, -(int)lc->u.length.v);
            ns_pango_layout_set_ellipsize(layout, NS_PANGO_ELLIPSIZE_END);
        }
    }
    NsPangoAttrList *i18n = ns_pango_attr_list_new();
    ns_paint_apply_i18n(layout, i18n, box);
    ns_paint_apply_font_features(i18n, parent_style, 0, G_MAXUINT);
    ns_inline_apply_atomic_shapes(i18n, box);
    ns_inline_apply_spacing(i18n, parent_style, box->text);
    apply_inline_layout_attrs(i18n, box);
    ns_inline_layout_set_attrs(layout, i18n, box);
    ns_pango_attr_list_unref(i18n);

    ns_paint_layout_set_inline_text(layout, box->text);
    ns_paint_start_align_overflow(layout);
    gboolean want_line_heights =
        g_object_get_data(G_OBJECT(layout), NS_CSS_LINE_HEIGHT_KEY) != NULL;
    ns_text_measure measured;
    const int *measured_line_heights =
        text_measure_lines(layout, &measured, want_line_heights);
    NsPangoRectangle measured_px = measured.logical;
    ns_pango_extents_to_pixels(&measured_px, NULL);
    int measured_h = measured_px.height;
    int line_count = measured.lines;
    if (line_count < 1) line_count = 1;
    double lh_default = inline_line_height(parent_style);
    if (box->parent && ns_input_is_one_line_text(box->parent->dom))
        lh_default = MAX(lh_default,
                         ns_paint_normal_line_height_px(parent_style));
    double lh_control = inline_control_line_height(box, lh_default);
    double *line_heights = g_new(double, line_count);
    for (int i = 0; i < line_count; i++) line_heights[i] = lh_control;
    if (measured_line_heights && lh_control <= lh_default + 0.01) {
        for (int j = 0; j < line_count; j++)
            line_heights[j] = (double)measured_line_heights[j] / NS_PANGO_SCALE;
    }
    if (box->inline_atomics) {
        for (guint i = 0; i < box->inline_atomics->len; i++) {
            const ns_inline_atomic *a =
                &g_array_index(box->inline_atomics, ns_inline_atomic, i);
            const ns_box *atomic = a->box;
            if (!atomic) continue;
            int line = 0;
            ns_pango_layout_index_to_line_x(layout, (int)a->byte_off,
                                         FALSE, &line, NULL);
            if (line < 0 || line >= line_count) continue;
            double outer = atomic->content_height
                + atomic->padding.top + atomic->padding.bottom
                + atomic->border.top + atomic->border.bottom
                + atomic->margin.top + atomic->margin.bottom;
            if (outer > line_heights[line]) line_heights[line] = outer;
        }
    }
    double *line_offsets = inline_atomic_line_offsets(box, layout, parent_style,
                                                      lh_control, line_heights,
                                                      line_count);
    if (g_abs_static && g_abs_ph_set && box->inline_atomics) {
        double indent = ns_inline_text_indent_px(box, parent_style, content_width);
        for (guint i = 0; i < box->inline_atomics->len; i++) {
            const ns_inline_atomic *a =
                &g_array_index(box->inline_atomics, ns_inline_atomic, i);
            if (!a->box) continue;
            gconstpointer key = g_hash_table_lookup(g_abs_ph_set, a->box);
            if (!key) continue;
            NsPangoRectangle pos;
            ns_pango_layout_index_to_pos(layout, (int)a->byte_off, &pos);
            int line_index = 0;
            ns_pango_layout_index_to_line_x(layout, (int)a->byte_off, FALSE,
                                            &line_index, NULL);
            double line_y = 0;
            for (int j = 0; j < line_index && j < line_count; j++)
                line_y += line_heights[j];
            ns_abs_static *st = g_new0(ns_abs_static, 1);
            st->run   = box;
            st->rel_x = indent + (double)pos.x / NS_PANGO_SCALE;
            st->rel_y = line_y;
            st->byte_off = a->byte_off;
            g_hash_table_insert(g_abs_static, (gpointer)key, st);
        }
    }
    double expected = 0;
    for (int i = 0; i < line_count; i++) expected += line_heights[i];
    if (box->inline_atomics && box->inline_atomics->len > 0) {
        if (!box->atomic_line_heights)
            box->atomic_line_heights = g_array_new(FALSE, FALSE, sizeof(double));
        g_array_set_size(box->atomic_line_heights, 0);
        g_array_append_vals(box->atomic_line_heights, line_heights,
                            (guint)line_count);
    }
    if (line_offsets) {
        if (!box->atomic_line_offsets)
            box->atomic_line_offsets = g_array_new(FALSE, FALSE, sizeof(double));
        g_array_set_size(box->atomic_line_offsets, 0);
        g_array_append_vals(box->atomic_line_offsets, line_offsets,
                            (guint)line_count);
    } else if (box->atomic_line_offsets) {
        g_array_free(box->atomic_line_offsets, TRUE);
        box->atomic_line_offsets = NULL;
    }
    box->content_width  = content_width;
    box->content_height = expected;
    double ta_h = inline_textarea_total_height(box, parent_style);
    if (ta_h > box->content_height) box->content_height = ta_h;
    box->first_baseline = (double)measured.baseline / NS_PANGO_SCALE;
    if (line_offsets)
        box->first_baseline += line_offsets[0];
    else if (line_heights[0] > measured_h)
        box->first_baseline += (line_heights[0] - measured_h) / 2.0;
    if (inline_run_holds_only_abs_placeholders(box, parent_style)) {
        box->phantom_line = TRUE;
        box->content_height = 0;
        box->first_baseline = 0;
    }
    if (cacheable) {
        box->inline_layout_cache_style = parent_style;
        box->inline_layout_cache_width = content_width;
        box->inline_layout_cache_height = box->content_height;
        box->inline_layout_cache_valid = TRUE;
    }

    if (box->inline_atomics && box->inline_atomics->len > 0) {
        ns_pango_layout_set_text(layout, box->text, -1);
        if (ns_pango_layout_get_width(layout) < 0 &&
            ns_pango_layout_get_alignment(layout) != NS_PANGO_ALIGN_LEFT) {
            int pw, ph;
            ns_pango_layout_get_pixel_size(layout, &pw, &ph);
            if (pw <= content_width)
                ns_pango_layout_set_width(layout,
                                          (int)(content_width * NS_PANGO_SCALE));
        }
        double *line_tops = g_new0(double, line_count);
        double *line_pango_h = g_new0(double, line_count);
        NsPangoLayoutIter *iter = ns_pango_layout_get_iter(layout);
        for (int j = 0; j < line_count; j++) {
            NsPangoRectangle logical;
            ns_pango_layout_iter_get_line_extents(iter, NULL, &logical);
            line_tops[j] = (double)logical.y / NS_PANGO_SCALE;
            line_pango_h[j] = (double)logical.height / NS_PANGO_SCALE;
            if (!ns_pango_layout_iter_next_line(iter)) break;
        }
        ns_pango_layout_iter_free(iter);
        double text_x0 = box->x;
        double ti = ns_inline_text_indent_px(box, parent_style, content_width);
        if (ti < 0) text_x0 += ti;
        for (guint i = 0; i < box->inline_atomics->len; i++) {
            const ns_inline_atomic *a =
                &g_array_index(box->inline_atomics, ns_inline_atomic, i);
            if (!a->box) continue;
            NsPangoRectangle pos;
            ns_pango_layout_index_to_pos(layout, (int)a->byte_off, &pos);
            int line = 0;
            ns_pango_layout_index_to_line_x(layout, (int)a->byte_off,
                                         FALSE, &line, NULL);
            double line_y = 0;
            for (int j = 0; j < line && j < line_count; j++)
                line_y += line_heights[j];
            double within = 0;
            if (line >= 0 && line < line_count)
                within = (double)pos.y / NS_PANGO_SCALE - line_tops[line]
                       + (line_offsets ? line_offsets[line]
                          : (line_heights[line] - line_pango_h[line]) / 2.0);
            double slack = (line >= 0 && line < line_count)
                ? line_heights[line] - inline_atomic_outer_height(a->box) : 0;
            if (slack < 0) slack = 0;
            if (within < 0) within = 0;
            if (within > slack) within = slack;
            double nx = text_x0 + (double)pos.x / NS_PANGO_SCALE;
            double ny = box->y + line_y + within;
            shift_box_tree(a->box, nx - a->box->x, ny - a->box->y);
        }
        g_free(line_tops);
        g_free(line_pango_h);
    }

    g_free(line_heights);
    g_free(line_offsets);
    g_object_unref(layout);
}

static gboolean style_blocks_hit_testing(const ns_style *s);
static gboolean node_is_form_hit_target(const ns_node *n);

static gboolean
inline_attr_is_form_hit(ns_inline_attr_kind k)
{
    return k == NS_INLINE_INPUT_FIELD ||
           k == NS_INLINE_INPUT_FIELD_FOCUSED ||
           k == NS_INLINE_RANGE ||
           k == NS_INLINE_BUTTON ||
           k == NS_INLINE_CHECKBOX ||
           k == NS_INLINE_CHECKBOX_CHECKED ||
           k == NS_INLINE_RADIO ||
           k == NS_INLINE_RADIO_CHECKED;
}

static gboolean
inline_attr_is_button_hit(ns_inline_attr_kind k)
{
    return k == NS_INLINE_BUTTON ||
           k == NS_INLINE_CHECKBOX ||
           k == NS_INLINE_CHECKBOX_CHECKED ||
           k == NS_INLINE_RADIO ||
           k == NS_INLINE_RADIO_CHECKED;
}

static const ns_node *
inline_box_form_hit(const ns_box *box, double local_x, double local_y,
                    const ns_style *parent_style)
{
    if (!box) return NULL;
    if ((!box->attrs || box->attrs->len == 0) &&
        (!box->inline_atomics || box->inline_atomics->len == 0))
        return NULL;
    if (!box->text || !*box->text) return NULL;
    NsPangoLayout *layout = make_pango_layout(parent_style);
    ns_pango_layout_set_width(layout, (int)(box->content_width * NS_PANGO_SCALE));
    ns_pango_layout_set_wrap(layout, ns_paint_wrap_mode_for(parent_style));
    if (!(box->inline_atomics && box->inline_atomics->len > 0))
        ns_paint_apply_css_line_spacing(layout, parent_style);
    {
        double ti = ns_inline_text_indent_px(box, parent_style, box->content_width);
        if (ti > 0) ns_pango_layout_set_indent(layout, (int)(ti * NS_PANGO_SCALE));
    }
    if (ns_paint_text_ellipsizes(parent_style))
        ns_pango_layout_set_ellipsize(layout, NS_PANGO_ELLIPSIZE_END);
    ns_paint_layout_set_inline_text(layout, box->text);
    NsPangoAttrList *i18n = ns_pango_attr_list_new();
    ns_paint_apply_i18n(layout, i18n, box);
    ns_paint_apply_font_features(i18n, parent_style, 0, G_MAXUINT);
    ns_inline_apply_atomic_shapes(i18n, box);
    ns_inline_apply_spacing(i18n, parent_style, box->text);
    apply_inline_layout_attrs(i18n, box);
    ns_inline_layout_set_attrs(layout, i18n, box);
    ns_pango_attr_list_unref(i18n);
    ns_paint_apply_text_align(layout, parent_style);
    ns_paint_start_align_overflow(layout);
    ns_inline_unshift_point(box, layout, &local_x, &local_y);

    int index = 0, trailing = 0;
    gboolean inside = ns_pango_layout_xy_to_index(
        layout,
        (int)(local_x * NS_PANGO_SCALE),
        (int)(local_y * NS_PANGO_SCALE),
        &index, &trailing);
    const ns_node *button_hit = NULL;
    const ns_node *field_hit = NULL;
    const ns_node *atomic_hit = NULL;
    if (inside && index >= 0) {
        gsize idx = (gsize)index;
        if (box->inline_atomics) {
            for (guint i = 0; i < box->inline_atomics->len; i++) {
                const ns_inline_atomic *a =
                    &g_array_index(box->inline_atomics, ns_inline_atomic, i);
                if (!a->box || idx < a->byte_off || idx >= a->byte_off + 3)
                    continue;
                if (node_is_form_hit_target(a->box->dom))
                    atomic_hit = a->box->dom;
            }
        }
        if (box->attrs) {
            for (guint i = 0; i < box->attrs->len; i++) {
                const ns_inline_attr *r =
                    &g_array_index(box->attrs, ns_inline_attr, i);
                if (!inline_attr_is_form_hit(r->kind)) continue;
                if (!r->dom) continue;
                const ns_style *rs = r->style ? r->style : parent_style;
                if (style_blocks_hit_testing(rs)) continue;
                if (idx < r->start || idx >= r->start + r->len) continue;
                if (inline_attr_is_button_hit(r->kind)) button_hit = r->dom;
                else if (!field_hit)             field_hit = r->dom;
            }
        }
    }
    if (box->attrs) {
        for (guint i = 0; i < box->attrs->len; i++) {
            const ns_inline_attr *r =
                &g_array_index(box->attrs, ns_inline_attr, i);
            if (!inline_attr_is_form_hit(r->kind)) continue;
            if (!r->dom) continue;
            const ns_style *rs = r->style ? r->style : parent_style;
            if (style_blocks_hit_testing(rs)) continue;
            NsPangoRectangle r0, r1;
            ns_pango_layout_index_to_pos(layout, (int)r->start, &r0);
            ns_pango_layout_index_to_pos(layout, (int)(r->start + r->len - 1), &r1);
            double bleed_x = r->box_w > 0 || r->box_h > 0 ? 0 : 10;
            double bleed_y = r->box_w > 0 || r->box_h > 0 ? 0 : 5;
            double x0 = (double)r0.x / NS_PANGO_SCALE - bleed_x;
            double y0 = (double)r0.y / NS_PANGO_SCALE - bleed_y;
            double x1 = (double)(r1.x + r1.width) / NS_PANGO_SCALE + bleed_x;
            double y1 = (double)(r0.y + r0.height) / NS_PANGO_SCALE + bleed_y;
            double css_w = ns_inline_attr_control_width(r, box);
            if (css_w > 0) {
                x0 = (double)r0.x / NS_PANGO_SCALE;
                x1 = x0 + css_w;
            }
            if (r->box_h > 0) {
                double cy = (y0 + y1) / 2.0;
                y0 = cy - r->box_h / 2.0;
                y1 = cy + r->box_h / 2.0;
            }
            if (local_x < x0 || local_x > x1 || local_y < y0 || local_y > y1)
                continue;
            if (inline_attr_is_button_hit(r->kind)) button_hit = r->dom;
            else if (!field_hit)             field_hit = r->dom;
        }
    }
    g_object_unref(layout);
    if (atomic_hit) return atomic_hit;
    if (button_hit) return button_hit;
    return field_hit;
}

static gboolean
inline_attr_can_fragment(ns_inline_attr_kind k)
{
    switch (k) {
    case NS_INLINE_INPUT_FIELD:
    case NS_INLINE_INPUT_FIELD_FOCUSED:
    case NS_INLINE_BUTTON:
    case NS_INLINE_CHECKBOX:
    case NS_INLINE_CHECKBOX_CHECKED:
    case NS_INLINE_RADIO:
    case NS_INLINE_RADIO_CHECKED:
    case NS_INLINE_PROGRESS:
    case NS_INLINE_METER:
    case NS_INLINE_RANGE:
    case NS_INLINE_CARET:
    case NS_INLINE_SELECTION:
        return FALSE;
    default:
        return TRUE;
    }
}

static gboolean
inline_box_can_fragment_multicol(const ns_box *box, const ns_style *style,
                                 double basis)
{
    if (!box || box->kind != NS_BOX_INLINE || !box->text || !*box->text)
        return FALSE;
    if (box->inline_atomics && box->inline_atomics->len > 0)
        return FALSE;
    if (keyword_is(style ? style->values[NS_CSS_WHITE_SPACE] : NULL, "nowrap") ||
        keyword_is(style ? style->values[NS_CSS_WHITE_SPACE] : NULL, "pre"))
        return FALSE;
    if (ns_paint_text_ellipsizes(style))
        return FALSE;
    if (style && style->values[NS_CSS_LINE_CLAMP])
        return FALSE;
    if (fabs(ns_text_indent_px(style, basis)) > 0.01)
        return FALSE;
    if (box->attrs) {
        for (guint i = 0; i < box->attrs->len; i++) {
            const ns_inline_attr *a =
                &g_array_index(box->attrs, ns_inline_attr, i);
            if (!inline_attr_can_fragment(a->kind)) return FALSE;
        }
    }
    return TRUE;
}

static gsize
range_end_clamped(gsize start, gsize len, gsize cap)
{
    gsize end = len > G_MAXSIZE - start ? G_MAXSIZE : start + len;
    return end > cap ? cap : end;
}

static ns_box *
inline_box_clone_range(const ns_box *src, gsize start, gsize end)
{
    gsize text_len = src && src->text ? strlen(src->text) : 0;
    if (!src || start >= text_len || end <= start) return NULL;
    if (end > text_len) end = text_len;

    ns_box *out = box_new_inline();
    out->dom = src->dom;
    out->style = src->style;
    out->text = g_strndup(src->text + start, end - start);

    if (src->attrs) {
        for (guint i = 0; i < src->attrs->len; i++) {
            const ns_inline_attr *a =
                &g_array_index(src->attrs, ns_inline_attr, i);
            gsize a_end = range_end_clamped(a->start, a->len, text_len);
            gsize is = MAX(a->start, start);
            gsize ie = MIN(a_end, end);
            gboolean marker_here = a->kind == NS_INLINE_EMPTY_ELEMENT &&
                a->start >= start &&
                (a->start < end || a->start == text_len);
            if (ie <= is && !marker_here) continue;
            ns_inline_attr copy = *a;
            copy.start = is - start;
            copy.len = ie - is;
            g_array_append_val(out->attrs, copy);
            if (copy.bg_image_src) {
                ns_box_media *m = ns_box_media_ensure(out);
                if (!m->bg_image_src) {
                    m->bg_image_src = g_strdup(copy.bg_image_src);
                    m->bg_image = copy.bg_image;
                }
            }
        }
    }

    if (src->links) {
        for (guint i = 0; i < src->links->len; i++) {
            const ns_link_range *r =
                &g_array_index(src->links, ns_link_range, i);
            gsize r_end = range_end_clamped(r->start, r->len, text_len);
            gsize is = MAX(r->start, start);
            gsize ie = MIN(r_end, end);
            if (ie <= is) continue;
            ns_link_range copy = {
                .start = is - start,
                .len = ie - is,
                .href = r->href ? g_strdup(r->href) : NULL,
                .target = r->target ? g_strdup(r->target) : NULL,
                .dom = r->dom,
            };
            g_array_append_val(inline_links_ensure(out), copy);
        }
    }

    return out;
}

static NsPangoLayout *
inline_box_layout_for_multicol(const ns_box *box, double content_width,
                               const ns_style *parent_style)
{
    NsPangoLayout *layout = make_pango_layout(parent_style);
    ns_pango_layout_set_width(layout, (int)(content_width * NS_PANGO_SCALE));
    ns_pango_layout_set_wrap(layout, ns_paint_wrap_mode_for(parent_style));
    if (!(box->inline_atomics && box->inline_atomics->len > 0))
        ns_paint_apply_css_line_spacing(layout, parent_style);
    ns_pango_layout_set_text(layout, box->text, -1);

    NsPangoAttrList *i18n = ns_pango_attr_list_new();
    ns_paint_apply_i18n(layout, i18n, box);
    ns_paint_apply_font_features(i18n, parent_style, 0, G_MAXUINT);
    ns_inline_apply_spacing(i18n, parent_style, box->text);
    apply_inline_layout_attrs(i18n, box);
    ns_inline_layout_set_attrs(layout, i18n, box);
    ns_pango_attr_list_unref(i18n);
    return layout;
}

static gboolean
layout_multicol_single_inline(ns_box *box, double inner_x, double inner_y,
                              double col_w, double col_gap, int n_cols,
                              const ns_style *child_inherited,
                              double *cursor_y)
{
    ns_box *src = box ? box->first_child : NULL;
    if (!src || src->next_sibling || n_cols < 2)
        return FALSE;
    if (!inline_box_can_fragment_multicol(src, child_inherited, col_w))
        return FALSE;

    NsPangoLayout *layout = inline_box_layout_for_multicol(src, col_w,
                                                         child_inherited);
    int line_count = ns_pango_layout_get_line_count(layout);
    if (line_count <= 1) {
        g_object_unref(layout);
        return FALSE;
    }

    GPtrArray *fragments = g_ptr_array_new();
    int per_col = line_count / n_cols + (line_count % n_cols != 0);
    gsize text_len = strlen(src->text);
    for (int col = 0; col < n_cols; col++) {
        int first_line = col * per_col;
        int after_line = first_line + per_col;
        if (first_line >= line_count) break;
        if (after_line > line_count) after_line = line_count;
        NsPangoLayoutLine *first =
            ns_pango_layout_get_line_readonly(layout, first_line);
        NsPangoLayoutLine *after =
            after_line < line_count
            ? ns_pango_layout_get_line_readonly(layout, after_line) : NULL;
        if (!first) continue;
        gsize start = (gsize)first->start_index;
        gsize end = after ? (gsize)after->start_index : text_len;
        ns_box *frag = inline_box_clone_range(src, start, end);
        if (!frag) continue;
        frag->x = inner_x + col * (col_w + col_gap);
        frag->y = inner_y;
        layout_box(frag, col_w, child_inherited);
        g_ptr_array_add(fragments, frag);
    }
    g_object_unref(layout);

    if (fragments->len < 2) {
        for (guint i = 0; i < fragments->len; i++)
            ns_box_free(g_ptr_array_index(fragments, i));
        g_ptr_array_free(fragments, TRUE);
        return FALSE;
    }

    box->first_child = NULL;
    box->last_child = NULL;
    for (guint i = 0; i < fragments->len; i++) {
        ns_box *frag = g_ptr_array_index(fragments, i);
        frag->parent = NULL;
        frag->next_sibling = NULL;
        box_append_child(box, frag);
    }
    g_ptr_array_free(fragments, TRUE);
    src->parent = NULL;
    src->next_sibling = NULL;
    ns_box_free(src);

    double max_h = 0;
    for (ns_box *c = box->first_child; c; c = c->next_sibling) {
        if (c->content_height > max_h) max_h = c->content_height;
    }
    box->content_width = col_w * n_cols + col_gap * (n_cols - 1);
    *cursor_y = inner_y + max_h;
    return TRUE;
}

static const char *
overflow_axis_keyword(const ns_style *s, ns_css_prop axis)
{
    return ns_style_overflow_keyword(s, axis);
}

static gboolean
overflow_kw_clips(const char *ov)
{
    return ov && (g_ascii_strcasecmp(ov, "hidden") == 0 ||
                  g_ascii_strcasecmp(ov, "clip")   == 0 ||
                  g_ascii_strcasecmp(ov, "auto")   == 0 ||
                  g_ascii_strcasecmp(ov, "scroll") == 0);
}

static gboolean
overflow_kw_scrolls(const char *ov)
{
    return ov && (g_ascii_strcasecmp(ov, "auto")   == 0 ||
                  g_ascii_strcasecmp(ov, "scroll") == 0);
}

static gboolean
box_clips_children(const ns_box *b)
{
    if (!b || !b->style) return FALSE;
    if (b->kind != NS_BOX_BLOCK && b->kind != NS_BOX_TABLE_CAPTION &&
        b->kind != NS_BOX_TABLE_CELL) return FALSE;
    return overflow_kw_clips(overflow_axis_keyword(b->style, NS_CSS_OVERFLOW_X)) ||
           overflow_kw_clips(overflow_axis_keyword(b->style, NS_CSS_OVERFLOW_Y));
}

static gboolean
overflow_kw_scroll_container(const char *ov)
{
    return overflow_kw_clips(ov) && g_ascii_strcasecmp(ov, "clip") != 0;
}

static gboolean
box_is_scroll_container(const ns_box *b)
{
    if (!box_clips_children(b)) return FALSE;
    return overflow_kw_scroll_container(
               overflow_axis_keyword(b->style, NS_CSS_OVERFLOW_X)) ||
           overflow_kw_scroll_container(
               overflow_axis_keyword(b->style, NS_CSS_OVERFLOW_Y));
}

static gboolean
box_padding_contains(const ns_box *b, double x, double y)
{
    double x0 = b->x + b->margin.left + b->border.left;
    double y0 = b->y + b->margin.top  + b->border.top;
    double x1 = x0 + b->content_width + b->padding.left + b->padding.right;
    double y1 = y0 + b->content_height + b->padding.top + b->padding.bottom;
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

gboolean
ns_box_clips_out_point(const ns_box *b, double x, double y)
{
    return box_clips_children(b) && !box_padding_contains(b, x, y);
}

static gboolean
box_border_contains(const ns_box *b, double x, double y)
{
    double x0 = b->x + b->margin.left;
    double y0 = b->y + b->margin.top;
    double x1 = x0 + b->border.left + b->padding.left + b->content_width +
                b->padding.right + b->border.right;
    double y1 = y0 + b->border.top + b->padding.top + b->content_height +
                b->padding.bottom + b->border.bottom;
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

static gboolean
node_is_form_hit_target(const ns_node *n)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    return strcmp(n->name, "button") == 0 ||
           strcmp(n->name, "input") == 0 ||
           strcmp(n->name, "select") == 0 ||
           strcmp(n->name, "textarea") == 0;
}

static gboolean
style_visibility_hidden(const ns_style *s)
{
    const char *vis = ns_style_keyword(s, NS_CSS_VISIBILITY);
    return vis && (strcmp(vis, "hidden") == 0 || strcmp(vis, "collapse") == 0);
}

static gboolean
style_blocks_hit_testing(const ns_style *s)
{
    return s && (ns_css_keyword_is(s->values[NS_CSS_POINTER_EVENTS], "none") ||
                 style_visibility_hidden(s));
}

static gboolean
box_blocks_hit_testing(const ns_box *b)
{
    return b && style_blocks_hit_testing(b->style);
}

typedef struct {
    const ns_box *box;
    int          key;
    guint        order;
} hit_stack_entry;

static int
hit_tree_order_cmp(const ns_box *a, const ns_box *b, guint order_a,
                   guint order_b)
{
    if (a->dom && b->dom && a->dom != b->dom) {
        int c = ns_node_document_order_cmp(a->dom, b->dom);
        if (c) return c;
    }
    return order_a < order_b ? -1 : order_a > order_b ? 1 : 0;
}

static int
hit_stack_cmp(const void *a, const void *b)
{
    const hit_stack_entry *pa = a, *pb = b;
    if (pa->key != pb->key) return pa->key < pb->key ? -1 : 1;
    return hit_tree_order_cmp(pa->box, pb->box, pa->order, pb->order);
}

static const ns_box **
hit_children_stacked(const ns_box *parent, guint *out_n)
{
    guint n = 0;
    gboolean need = FALSE;
    for (const ns_box *c = parent->first_child; c; c = c->next_sibling) {
        if (ns_box_stack_key(c) != 0) need = TRUE;
        n++;
    }
    if (!need || n == 0) { *out_n = 0; return NULL; }
    hit_stack_entry *e = g_new(hit_stack_entry, n);
    guint i = 0;
    for (const ns_box *c = parent->first_child; c; c = c->next_sibling) {
        e[i].box = c;
        e[i].key = ns_box_stack_key(c);
        e[i].order = i;
        i++;
    }
    qsort(e, n, sizeof(*e), hit_stack_cmp);
    const ns_box **arr = g_new(const ns_box *, n);
    for (i = 0; i < n; i++) arr[i] = e[i].box;
    g_free(e);
    *out_n = n;
    return arr;
}

static gboolean
style_is_positioned(const ns_style *s)
{
    const ns_css_value *p = s ? s->values[NS_CSS_POSITION] : NULL;
    return keyword_is(p, "relative") || keyword_is(p, "absolute") ||
           keyword_is(p, "fixed") || keyword_is(p, "sticky");
}

static gboolean
style_has_z_index(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_Z_INDEX] : NULL;
    return v && v->kind == NS_CSS_V_LENGTH;
}

gboolean
ns_box_is_flex_or_grid_item(const ns_box *b)
{
    const ns_box *p = b ? b->parent : NULL;
    while (p && !p->style) p = p->parent;
    if (!p) return FALSE;
    ns_display d = ns_css_display_of(p->style);
    return ns_display_is_flex_container(d) || ns_display_is_grid_container(d);
}

static gboolean
box_z_index_applies(const ns_box *b)
{
    return style_has_z_index(b->style) &&
           (style_is_positioned(b->style) || ns_box_is_flex_or_grid_item(b));
}

int
ns_box_layer_z(const ns_box *b)
{
    if (!b || !box_z_index_applies(b)) return 0;
    double z = b->style->values[NS_CSS_Z_INDEX]->u.length.v;
    if (isnan(z)) return 0;
    return (int)CLAMP(z, (double)G_MININT, (double)G_MAXINT);
}

int
ns_box_stack_key(const ns_box *b)
{
    return b && style_is_positioned(b->style) ? ns_box_layer_z(b) : 0;
}

static gboolean
style_differs_from(const ns_style *s, ns_css_prop prop, const char *initial)
{
    const ns_css_value *v = s->values[prop];
    return v && !keyword_is(v, initial);
}

static gboolean
style_has_layer_transform(const ns_style *s)
{
    static const ns_css_prop props[] = {
        NS_CSS_TRANSFORM, NS_CSS_TRANSLATE, NS_CSS_ROTATE, NS_CSS_SCALE,
    };
    for (guint i = 0; i < G_N_ELEMENTS(props); i++) {
        const ns_css_value *v = s->values[props[i]];
        if (v && v->kind == NS_CSS_V_TRANSFORM && v->u.transform.n_ops > 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
style_has_mask(const ns_style *s)
{
    for (const ns_css_value *l = s->values[NS_CSS_MASK_IMAGE]; l;
         l = l->next_layer)
        if (!keyword_is(l, "none")) return TRUE;
    return FALSE;
}

static gboolean
style_has_compositing_effect(const ns_style *s)
{
    const ns_css_value *op = s->values[NS_CSS_OPACITY];
    if (op && op->kind == NS_CSS_V_LENGTH && op->u.length.v < 1.0)
        return TRUE;
    return style_differs_from(s, NS_CSS_FILTER, "none") ||
           style_differs_from(s, NS_CSS_BACKDROP_FILTER, "none") ||
           style_differs_from(s, NS_CSS_CLIP_PATH, "none") ||
           style_differs_from(s, NS_CSS_MIX_BLEND_MODE, "normal") ||
           keyword_is(s->values[NS_CSS_ISOLATION], "isolate") ||
           style_has_mask(s);
}

static gboolean
style_has_3d_layer(const ns_style *s)
{
    return style_has_layer_transform(s) ||
           style_differs_from(s, NS_CSS_PERSPECTIVE, "none") ||
           keyword_is(s->values[NS_CSS_TRANSFORM_STYLE], "preserve-3d");
}

static gboolean
keyword_list_has(const ns_css_value *v, const char *const *names, gsize n)
{
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return FALSE;
    for (const char *p = v->u.keyword; *p;) {
        p += strspn(p, " ,\t\n");
        gsize len = strcspn(p, " ,\t\n");
        for (gsize k = 0; len && k < n; k++)
            if (!g_ascii_strncasecmp(p, names[k], len) && !names[k][len])
                return TRUE;
        p += len;
    }
    return FALSE;
}

static gboolean
style_has_layout_containment(const ns_style *s)
{
    static const char *const contain[] = { "layout", "content", "strict" };
    return s && keyword_list_has(s->values[NS_CSS_CONTAIN], contain,
                                 G_N_ELEMENTS(contain));
}

static gboolean
style_has_containment_layer(const ns_style *s)
{
    static const char *const contain[] = {
        "layout", "paint", "strict", "content",
    };
    static const char *const will_change[] = {
        "opacity", "transform", "translate", "rotate", "scale",
        "perspective", "filter", "backdrop-filter", "clip-path", "mask",
        "mask-image", "-webkit-mask", "-webkit-mask-image", "isolation",
        "mix-blend-mode", "contain", "offset-path",
    };
    return keyword_list_has(s->values[NS_CSS_CONTAIN], contain,
                            G_N_ELEMENTS(contain)) ||
           keyword_list_has(s->values[NS_CSS_WILL_CHANGE], will_change,
                            G_N_ELEMENTS(will_change)) ||
           style_differs_from(s, NS_CSS_CONTENT_VISIBILITY, "visible");
}

static gboolean
style_inline_size_contained(const ns_style *s, double *intrinsic)
{
    static const char *const contain[] = { "size", "inline-size", "strict" };
    if (!s) return FALSE;
    if (!style_contains_inline_size(s) && !style_content_visibility_hidden(s) &&
        !keyword_list_has(s->values[NS_CSS_CONTAIN], contain, G_N_ELEMENTS(contain)))
        return FALSE;
    *intrinsic = ns_css_contain_intrinsic_px(s, FALSE);
    return TRUE;
}

static gboolean
style_block_size_contained(const ns_style *s, double *intrinsic)
{
    static const char *const contain[] = { "size", "strict" };
    if (!s) return FALSE;
    const ns_css_value *ct = s->values[NS_CSS_CONTAINER_TYPE];
    if (!keyword_is(ct, "size") && !style_content_visibility_hidden(s) &&
        !keyword_list_has(s->values[NS_CSS_CONTAIN], contain, G_N_ELEMENTS(contain)))
        return FALSE;
    *intrinsic = ns_css_contain_intrinsic_px(s, TRUE);
    return TRUE;
}

static gboolean
style_has_positioned_layer(const ns_style *s)
{
    const ns_css_value *p = s->values[NS_CSS_POSITION];
    return keyword_is(p, "fixed") || keyword_is(p, "sticky") ||
           (style_has_z_index(s) && style_is_positioned(s));
}

gboolean
ns_style_is_stacking_context(const ns_style *s)
{
    if (!s) return FALSE;
    return style_has_positioned_layer(s) || style_has_compositing_effect(s) ||
           style_has_3d_layer(s) || style_has_containment_layer(s);
}

enum {
    BOX_LAYER_KNOWN = 1,
    BOX_LAYER_STACKING = 2,
    BOX_LAYER_UPPER = 4,
};

static gboolean
box_computes_stacking_context(const ns_box *b)
{
    if (!b->parent) return TRUE;
    return b->style && (ns_style_is_stacking_context(b->style) ||
                        box_z_index_applies(b));
}

static guint8
box_layer_flags(const ns_box *b)
{
    if (b->layer_flags) return b->layer_flags;
    guint8 f = BOX_LAYER_KNOWN;
    gboolean sc = box_computes_stacking_context(b);
    if (sc) f |= BOX_LAYER_STACKING;
    if (b->parent && b->style && ns_box_layer_z(b) >= 0 &&
        (sc || style_is_positioned(b->style)))
        f |= BOX_LAYER_UPPER;
    return f;
}

gboolean
ns_box_is_stacking_context(const ns_box *b)
{
    return b && (box_layer_flags(b) & BOX_LAYER_STACKING);
}

gboolean
ns_box_is_upper_layer(const ns_box *b)
{
    return b && (box_layer_flags(b) & BOX_LAYER_UPPER);
}

static gboolean
box_is_negative_layer(const ns_box *b)
{
    return b->style && ns_box_layer_z(b) < 0;
}

static gboolean
box_is_viewport_root(const ns_box *b)
{
    return ns_node_is_element_named(b->dom, "html") ||
           ns_node_is_element_named(b->dom, "body");
}

static gboolean
box_in_3d_context(const ns_box *b)
{
    return b->parent && b->parent->style &&
           keyword_is(b->parent->style->values[NS_CSS_TRANSFORM_STYLE],
                      "preserve-3d");
}

static gboolean
box_is_perspective_child(const ns_box *b)
{
    const ns_style *ps = b->parent ? b->parent->style : NULL;
    const ns_css_value *d = ps ? ps->values[NS_CSS_PERSPECTIVE] : NULL;
    return d && d->kind == NS_CSS_V_LENGTH && d->u.length.v > 0;
}

static gboolean
style_has_background(const ns_style *s)
{
    const ns_css_value *c = s ? s->values[NS_CSS_BACKGROUND_COLOR] : NULL;
    if (c && c->kind == NS_CSS_V_COLOR && c->u.color.a > 0) return TRUE;
    return s && style_differs_from(s, NS_CSS_BACKGROUND_IMAGE, "none");
}

static gboolean
box_paints_canvas_background(const ns_box *b)
{
    if (ns_node_is_element_named(b->dom, "html"))
        return style_has_background(b->style);
    return ns_node_is_element_named(b->dom, "body") && b->parent &&
           !style_has_background(b->parent->style) &&
           style_has_background(b->style);
}

static gboolean
box_holds_negative_layers(const ns_box *b)
{
    if (ns_box_is_stacking_context(b)) return TRUE;
    if (box_is_viewport_root(b)) return box_paints_canvas_background(b);
    return box_clips_children(b);
}

typedef struct {
    ns_box *box;
    guint order;
} negative_layer_entry;

typedef struct {
    const ns_node *node;
    int part;
} tree_position;

static gboolean node_is_ancestor_of(const ns_node *a, const ns_node *n);

static const ns_box *
box_pseudo_host_box(const ns_box *b)
{
    const ns_box *h = b->parent;
    while (h && !h->dom) h = h->parent;
    return h;
}

static tree_position
box_tree_position(const ns_box *b)
{
    if (b->dom) return (tree_position){ b->dom, 0 };
    if (b->pseudo_host)
        return (tree_position){ b->pseudo_host, b->pseudo_after ? 2 : 1 };
    const ns_box *h = box_pseudo_host_box(b);
    if (!h) return (tree_position){ NULL, 0 };
    gboolean after = h->style && h->style->after == b->style;
    return (tree_position){ h->dom, after ? 2 : 1 };
}

static int
tree_position_cmp(tree_position a, tree_position b)
{
    if (a.node == b.node) return (a.part > b.part) - (a.part < b.part);
    if (!a.node || !b.node) return a.node ? 1 : -1;
    if (node_is_ancestor_of(a.node, b.node)) return a.part == 2 ? 1 : -1;
    if (node_is_ancestor_of(b.node, a.node)) return b.part == 2 ? -1 : 1;
    return ns_node_document_order_cmp(a.node, b.node);
}

int
ns_layout_tree_order_cmp(const ns_box *a, const ns_node *a_dom,
                         const ns_box *b, const ns_node *b_dom)
{
    tree_position pa = a_dom ? (tree_position){ a_dom, 0 } : box_tree_position(a);
    tree_position pb = b_dom ? (tree_position){ b_dom, 0 } : box_tree_position(b);
    if (!pa.node && !pb.node) return 0;
    return tree_position_cmp(pa, pb);
}

static int
negative_layer_cmp(const ns_box *a, const ns_box *b)
{
    int za = ns_box_layer_z(a), zb = ns_box_layer_z(b);
    if (za != zb) return za < zb ? -1 : 1;
    return tree_position_cmp(box_tree_position(a), box_tree_position(b));
}

static int
negative_layer_entry_cmp(const void *pa, const void *pb)
{
    const negative_layer_entry *a = pa, *b = pb;
    int c = negative_layer_cmp(a->box, b->box);
    if (c) return c;
    return a->order < b->order ? -1 : a->order > b->order ? 1 : 0;
}

static gboolean
negative_layers_sorted(const GPtrArray *layers)
{
    for (guint i = 1; i < layers->len; i++)
        if (negative_layer_cmp(g_ptr_array_index(layers, i - 1),
                               g_ptr_array_index(layers, i)) > 0)
            return FALSE;
    return TRUE;
}

static void
negative_layers_sort(GPtrArray *layers)
{
    if (!layers || negative_layers_sorted(layers)) return;
    negative_layer_entry *e = g_new(negative_layer_entry, layers->len);
    for (guint i = 0; i < layers->len; i++)
        e[i] = (negative_layer_entry){ g_ptr_array_index(layers, i), i };
    qsort(e, layers->len, sizeof *e, negative_layer_entry_cmp);
    for (guint i = 0; i < layers->len; i++) layers->pdata[i] = e[i].box;
    g_free(e);
}

static ns_box *
negative_layers_scope_of(ns_box *scope, ns_box *c)
{
    c->layer_flags = box_layer_flags(c);
    if (!scope) return NULL;
    if (box_in_3d_context(c)) return c;
    if (box_is_negative_layer(c) && !box_is_perspective_child(c)) {
        if (!scope->negative_layers)
            scope->negative_layers = g_ptr_array_new();
        g_ptr_array_add(scope->negative_layers, c);
        c->negative_lifted = TRUE;
        return c;
    }
    return box_holds_negative_layers(c) ? c : scope;
}

static void negative_layers_scan(ns_box *scope, ns_box *b);

static void
negative_layers_scan_atomics(ns_box *b, gboolean painted)
{
    for (guint i = 0; b->inline_atomics && i < b->inline_atomics->len; i++) {
        ns_box *ab = g_array_index(b->inline_atomics, ns_inline_atomic, i).box;
        if (!ab) continue;
        ab->layer_flags = box_layer_flags(ab);
        negative_layers_scan(painted ? ab : NULL, ab);
    }
}

static void
negative_layers_scan(ns_box *scope, ns_box *b)
{
    gboolean painted = scope && ns_paint_box_paints_children(b);
    ns_box *inner = painted ? scope : NULL;
    for (ns_box *c = b->first_child; c; c = c->next_sibling)
        negative_layers_scan(negative_layers_scope_of(inner, c), c);
    negative_layers_scan_atomics(b, painted);
    if (painted && scope == b) negative_layers_sort(b->negative_layers);
}

static void
negative_layers_scan_root(ns_box *root)
{
    root->layer_flags = box_layer_flags(root);
    negative_layers_scan(root, root);
}

static gboolean box_hit_untransform_point(const ns_box *b, double *x,
                                          double *y);

static void
inline_atomic_hit_point(const ns_box *owner, const ns_inline_atomic *atomic,
                        double x, double y, double *child_x, double *child_y)
{
    double dx = owner->x + atomic->owner_offset_x + atomic->box->rel_dx -
                atomic->box->x;
    double dy = owner->y + atomic->owner_offset_y + atomic->box->rel_dy -
                atomic->box->y;
    *child_x = x - dx;
    *child_y = y - dy;
}

static const ns_node *
ns_form_hit_walk(const ns_box *box, double x, double y,
                 const ns_style *inherited)
{
    if (!box) return NULL;
    hit_enter_box(box, &x, &y);
    if (!box_hit_untransform_point(box, &x, &y)) return NULL;
    const ns_style *child_inherited = box->style ? box->style : inherited;
    const ns_node *self_hit = NULL;
    if (node_is_form_hit_target(box->dom) &&
        box_border_contains(box, x, y) &&
        !box_blocks_hit_testing(box))
        self_hit = box->dom;
    if (box->kind == NS_BOX_INLINE) {
        const ns_node *m = inline_box_form_hit(
            box, x - box->x, y - box->y, child_inherited);
        if (m) return m;
    }
    if (box_clips_children(box) && !box_padding_contains(box, x, y))
        return NULL;
    if (ns_paint_3d_registered(box)) return self_hit;
    double cx = x + box->scroll_x;
    double cy = y + box->scroll_y;
    const ns_node *best = NULL;
    guint sn = 0;
    const ns_box **stacked = hit_children_stacked(box, &sn);
    if (stacked) {
        for (guint i = 0; i < sn; i++) {
            const ns_node *m = ns_form_hit_walk(stacked[i], cx, cy, child_inherited);
            if (m) best = m;
        }
        g_free(stacked);
    } else {
        for (const ns_box *c = box->first_child; c; c = c->next_sibling) {
            const ns_node *m = ns_form_hit_walk(c, cx, cy, child_inherited);
            if (m) best = m;
        }
    }
    if (box->inline_atomics)
        for (guint i = 0; i < box->inline_atomics->len; i++) {
            const ns_inline_atomic *atomic =
                &g_array_index(box->inline_atomics, ns_inline_atomic, i);
            const ns_box *ab = atomic->box;
            if (!ab) continue;
            double ax, ay;
            inline_atomic_hit_point(box, atomic, cx, cy, &ax, &ay);
            const ns_node *m = ns_form_hit_walk(ab, ax, ay, child_inherited);
            if (m) best = m;
        }
    return best ? best : self_hit;
}

static double
min_width_of(ns_box *box, const ns_style *parent_style);
static double
min_content_width_of(ns_box *box, const ns_style *parent_style);
static void
table_border_spacing(const ns_style *s, double *hsp, double *vsp);

static int
float_side_of(const ns_style *s)
{
    if (!s) return -1;
    const ns_css_value *v = s->values[NS_CSS_FLOAT];
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return -1;
    if (strcmp(v->u.keyword, "left") == 0) return 0;
    if (strcmp(v->u.keyword, "right") == 0) return 1;
    return -1;
}

static int
multicol_distributable_children(const ns_box *box)
{
    int n = 0;
    for (const ns_box *c = box->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_BOX_BLOCK && c->kind != NS_BOX_TABLE) continue;
        if (style_is_absolute_or_fixed(c->style)) continue;
        if (float_side_of(c->style) >= 0) continue;
        if (++n >= 2) break;
    }
    return n;
}

static ns_box *
multicol_column_host(ns_box *box)
{
    ns_box *only = NULL;
    for (ns_box *c = box->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_BOX_BLOCK) continue;
        if (style_is_absolute_or_fixed(c->style)) continue;
        if (float_side_of(c->style) >= 0) continue;
        if (only) return NULL;
        only = c;
    }
    if (!only || multicol_distributable_children(only) < 2) return NULL;
    return only;
}

static int
clear_kind_of(const ns_style *s)
{
    if (!s) return 0;
    const ns_css_value *v = s->values[NS_CSS_CLEAR];
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return 0;
    if (strcmp(v->u.keyword, "left") == 0) return 1;
    if (strcmp(v->u.keyword, "right") == 0) return 2;
    if (strcmp(v->u.keyword, "both") == 0) return 3;
    return 0;
}

typedef struct float_ref {
    ns_box *box;
    int side;
    double top, bottom;
    double outer_w;
} float_ref;

static gboolean
overflow_establishes_bfc(const ns_style *s)
{
    if (!s) return FALSE;
    const ns_css_value *values[] = {
        s->values[NS_CSS_OVERFLOW],
        s->values[NS_CSS_OVERFLOW_X],
        s->values[NS_CSS_OVERFLOW_Y],
    };
    for (guint i = 0; i < G_N_ELEMENTS(values); i++) {
        const ns_css_value *v = values[i];
        if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) continue;
        if (strcmp(v->u.keyword, "hidden") == 0 ||
            strcmp(v->u.keyword, "auto") == 0 ||
            strcmp(v->u.keyword, "scroll") == 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
box_overflow_goes_to_viewport(const ns_box *box)
{
    const ns_node *html = box->dom ? box->dom->parent : NULL;
    if (!ns_node_is_element_named(box->dom, "body") ||
        !ns_node_is_element_named(html, "html") ||
        (html->parent && html->parent->kind == NS_NODE_ELEMENT) ||
        !box->parent || box->parent->dom != html)
        return FALSE;
    const ns_style *hs = box->parent->style;
    return g_ascii_strcasecmp(ns_style_overflow_keyword(hs, NS_CSS_OVERFLOW_X),
                              "visible") == 0 &&
           g_ascii_strcasecmp(ns_style_overflow_keyword(hs, NS_CSS_OVERFLOW_Y),
                              "visible") == 0;
}

static gboolean
box_establishes_bfc(const ns_box *box)
{
    if (!box || !box->parent) return TRUE;
    if (box->dom && box->dom->kind == NS_NODE_ELEMENT &&
        (!box->dom->parent || box->dom->parent->kind != NS_NODE_ELEMENT))
        return TRUE;
    if (box->kind == NS_BOX_TABLE_CELL || box->kind == NS_BOX_TABLE ||
        box->kind == NS_BOX_TABLE_CAPTION)
        return TRUE;
    if (float_side_of(box->style) >= 0 ||
        style_is_absolute_or_fixed(box->style) ||
        style_is_multicol(box->style) ||
        box_is_flex_container(box) ||
        box_is_grid_container(box))
        return TRUE;
    if (box_is_flex_container(box->parent) ||
        box_is_grid_container(box->parent))
        return TRUE;
    if (box->is_rendered_legend ||
        ns_node_is_element_named(box->dom, "fieldset"))
        return TRUE;
    const ns_css_value *d = box->style
        ? box->style->values[NS_CSS_DISPLAY] : NULL;
    if (keyword_is(d, "flow-root") || keyword_is(d, "inline-block") ||
        keyword_is(d, "inline-table"))
        return TRUE;
    return style_is_block_level(box->style) &&
           overflow_establishes_bfc(box->style) &&
           !box_overflow_goes_to_viewport(box);
}

static void
collect_escaping_floats(ns_box *box, GArray *floats, int depth)
{
    if (!box || !floats || depth >= NS_LAYOUT_MAX_DEPTH) return;
    int side = float_side_of(box->style);
    if (side >= 0) {
        float_ref ref = {
            .box = box,
            .side = side,
            .top = box->y,
            .bottom = box->y + box->margin.top + box->content_height
                + box->padding.top + box->padding.bottom
                + box->border.top + box->border.bottom
                + box->margin.bottom,
            .outer_w = box->content_width
                + box->padding.left + box->padding.right
                + box->border.left + box->border.right
                + box->margin.left + box->margin.right,
        };
        g_array_append_val(floats, ref);
        return;
    }
    if (box_establishes_bfc(box)) return;
    for (ns_box *child = box->first_child; child;
         child = child->next_sibling)
        collect_escaping_floats(child, floats, depth + 1);
}

static void
floats_offsets_at(const GArray *floats, double y,
                  double *left_out, double *right_out)
{
    double l = 0, r = 0;
    if (floats) {
        for (guint i = 0; i < floats->len; i++) {
            const float_ref *f = &g_array_index(floats, float_ref, i);
            if (y < f->top || y >= f->bottom) continue;
            if (f->side == 0) l += f->outer_w;
            else              r += f->outer_w;
        }
    }
    *left_out = l;
    *right_out = r;
}

static void
floats_offsets_over(const GArray *floats, double top, double bottom,
                    double *left_out, double *right_out)
{
    double l = 0, r = 0;
    floats_offsets_at(floats, top, &l, &r);
    if (floats && bottom > top) {
        for (guint i = 0; i < floats->len; i++) {
            const float_ref *f = &g_array_index(floats, float_ref, i);
            if (f->top <= top || f->top >= bottom) continue;
            double bl = 0, br = 0;
            floats_offsets_at(floats, f->top, &bl, &br);
            if (bl > l) l = bl;
            if (br > r) r = br;
        }
    }
    *left_out = l;
    *right_out = r;
}

static double
floats_clear_y(const GArray *floats, double y, int clear)
{
    if (!floats || clear == 0) return y;
    double out = y;
    for (guint i = 0; i < floats->len; i++) {
        const float_ref *f = &g_array_index(floats, float_ref, i);
        if (clear == 1 && f->side != 0) continue;
        if (clear == 2 && f->side != 1) continue;
        if (f->bottom > out) out = f->bottom;
    }
    return out;
}

static double
floats_max_bottom(const GArray *floats)
{
    double y = 0;
    if (!floats) return y;
    for (guint i = 0; i < floats->len; i++) {
        const float_ref *f = &g_array_index(floats, float_ref, i);
        if (f->bottom > y) y = f->bottom;
    }
    return y;
}

static gboolean
keyword_is_hidden_overflow(const char *kw)
{
    return kw && g_ascii_strcasecmp(kw, "hidden") == 0;
}

static gboolean
style_lines_never_wrap(const ns_style *s)
{
    const ns_css_value *ws = s ? s->values[NS_CSS_WHITE_SPACE] : NULL;
    return keyword_is(ws, "nowrap") || keyword_is(ws, "pre");
}

static gboolean
floats_advance_to_readable_width(const GArray *floats, double cw,
                                 double *y, double *left_out,
                                 double *right_out)
{
    if (!floats || floats->len == 0 || !y) return FALSE;
    double min_w = cw * 0.40;
    if (min_w > 260) min_w = 260;
    if (min_w < 120) min_w = 120;
    gboolean moved = FALSE;
    for (;;) {
        double left = 0, right = 0;
        floats_offsets_at(floats, *y, &left, &right);
        double avail = cw - left - right;
        if (avail >= min_w || cw <= min_w) {
            if (left_out) *left_out = left;
            if (right_out) *right_out = right;
            return moved;
        }
        double next_y = *y;
        gboolean advanced = FALSE;
        for (guint i = 0; i < floats->len; i++) {
            const float_ref *f = &g_array_index(floats, float_ref, i);
            if (f->bottom > *y && (!advanced || f->bottom < next_y)) {
                next_y = f->bottom;
                advanced = TRUE;
            }
        }
        if (!advanced || next_y <= *y) {
            if (left_out) *left_out = left;
            if (right_out) *right_out = right;
            return moved;
        }
        *y = next_y;
        moved = TRUE;
    }
}

typedef struct margin_above {
    const ns_box *box;
    double collapsed;
} margin_above;

static __thread margin_above g_margin_above;

typedef struct inherited_floats {
    const ns_box *box;
    const GArray *floats;
    double inner_x, cw;
} inherited_floats;

static __thread inherited_floats g_inherited_floats;

static gint
double_cmp(gconstpointer a, gconstpointer b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void
floats_inherit(GArray *floats, const inherited_floats *from, double inner_x,
               double cw, double from_y)
{
    const GArray *src = from->floats;
    if (!src || src->len == 0) return;
    GArray *edges = g_array_sized_new(FALSE, FALSE, sizeof(double),
                                      src->len * 2);
    for (guint i = 0; i < src->len; i++) {
        const float_ref *f = &g_array_index(src, float_ref, i);
        if (f->bottom <= from_y) continue;
        g_array_append_val(edges, f->top);
        g_array_append_val(edges, f->bottom);
    }
    g_array_sort(edges, double_cmp);
    for (guint i = 0; i + 1 < edges->len; i++) {
        double top = g_array_index(edges, double, i);
        double bottom = g_array_index(edges, double, i + 1);
        if (bottom <= top || bottom <= from_y) continue;
        double l = 0, r = 0;
        floats_offsets_at(src, top, &l, &r);
        double left_in = from->inner_x + l - inner_x;
        double right_in = inner_x + cw - (from->inner_x + from->cw - r);
        if (l > 0 && left_in > 0) {
            float_ref band = { NULL, 0, top, bottom, left_in };
            g_array_append_val(floats, band);
        }
        if (r > 0 && right_in > 0) {
            float_ref band = { NULL, 1, top, bottom, right_in };
            g_array_append_val(floats, band);
        }
    }
    g_array_free(edges, TRUE);
}

static void
floats_shift_placed(GArray *floats, double dy)
{
    if (dy == 0) return;
    for (guint i = 0; i < floats->len; i++) {
        float_ref *f = &g_array_index(floats, float_ref, i);
        if (!f->box) continue;
        shift_box_tree(f->box, 0, dy);
        f->top += dy;
        f->bottom += dy;
    }
}

static double
floats_band_bottom(const GArray *floats, double y)
{
    double bottom = -1;
    for (guint i = 0; floats && i < floats->len; i++) {
        const float_ref *f = &g_array_index(floats, float_ref, i);
        if (y < f->top || y >= f->bottom) continue;
        if (bottom < 0 || f->bottom < bottom) bottom = f->bottom;
    }
    return bottom;
}

static double
inline_run_leading_atomic_outer_width(const ns_box *run)
{
    if (!run || run->kind != NS_BOX_INLINE || !run->text ||
        !run->inline_atomics || run->inline_atomics->len == 0)
        return 0;
    const ns_inline_atomic *a =
        &g_array_index(run->inline_atomics, ns_inline_atomic, 0);
    const ns_box *ab = a->box;
    if (!ab || ab->kind == NS_BOX_IMAGE || ab->kind == NS_BOX_VIDEO ||
        ab->kind == NS_BOX_SVG)
        return 0;
    for (gsize i = 0; i < a->byte_off && run->text[i]; i++)
        if (!g_ascii_isspace(run->text[i])) return 0;
    return ab->content_width + ab->padding.left + ab->padding.right +
           ab->border.left + ab->border.right +
           ab->margin.left + ab->margin.right;
}

static double
inline_run_first_word_width(ns_box *run, const ns_style *style)
{
    if (!run || run->kind != NS_BOX_INLINE || !run->text)
        return min_width_of(run, style);
    const char *p = run->text;
    while (*p && g_ascii_isspace(*p)) p++;
    const char *end = p;
    while (*end && !g_ascii_isspace(*end)) end++;
    if (end == p || (run->inline_atomics && run->inline_atomics->len > 0))
        return min_width_of(run, style);
    NsPangoLayout *layout = make_pango_layout(style);
    ns_pango_layout_set_width(layout, -1);
    ns_pango_layout_set_text(layout, p, (int)(end - p));
    ns_text_measure m;
    text_measure(layout, &m);
    g_object_unref(layout);
    return ceil((double)m.logical.width / NS_PANGO_SCALE);
}

static gboolean
floats_advance_to_fit(const GArray *floats, double cw, double need,
                      double *y, double *left_out, double *right_out)
{
    gboolean moved = FALSE;
    for (;;) {
        double left = 0, right = 0;
        floats_offsets_at(floats, *y, &left, &right);
        double next_y = *y;
        gboolean advanced = FALSE;
        if (cw - left - right + 0.5 < need) {
            for (guint i = 0; i < floats->len; i++) {
                const float_ref *f = &g_array_index(floats, float_ref, i);
                if (f->bottom > *y && (!advanced || f->bottom < next_y)) {
                    next_y = f->bottom;
                    advanced = TRUE;
                }
            }
        }
        if (!advanced || next_y <= *y) {
            *left_out = left;
            *right_out = right;
            return moved;
        }
        *y = next_y;
        moved = TRUE;
    }
}

static gboolean
replaced_size_keyword(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           (strcmp(v->u.keyword, "fit-content") == 0 ||
            strcmp(v->u.keyword, "min-content") == 0 ||
            strcmp(v->u.keyword, "max-content") == 0);
}

static double
box_cb_basis(const ns_box *box, double fallback)
{
    return box->cb_width_set ? box->cb_width_override : fallback;
}

static double
image_content_limit(const ns_css_value *v, double base, double extras)
{
    double lim = length_resolve(v, base, -1);
    if (lim < 0) return lim;
    return MAX(lim - extras, 0);
}

static double
image_border_box_extras(const ns_box *box)
{
    if (!flex_box_is_border_box(box)) return 0;
    return box->padding.left + box->padding.right +
           box->border.left + box->border.right;
}

static gboolean
img_has_source(const ns_node *img)
{
    const char *src = ns_element_get_attr(img, "src");
    const char *srcset = ns_element_get_attr(img, "srcset");
    if ((src && *src) || (srcset && *srcset)) return TRUE;
    if (!ns_node_is_element_named(img->parent, "picture")) return FALSE;
    for (const ns_node *c = img->parent->first_child; c && c != img;
         c = c->next_sibling) {
        const char *set = ns_node_is_element_named(c, "source")
            ? ns_element_get_attr(c, "srcset") : NULL;
        if (set && *set) return TRUE;
    }
    return FALSE;
}

static double image_ratio_only(const ns_box *box);

static void
layout_image(ns_box *box, double parent_content_width)
{
    double cb_basis = box_cb_basis(box, parent_content_width);
    edges_from_style(box->style, cb_basis,
                     &box->margin, &box->padding, &box->border);
    const ns_css_value *wv  = box->style ? box->style->values[NS_CSS_WIDTH]      : NULL;
    const ns_css_value *hv  = box->style ? box->style->values[NS_CSS_HEIGHT]     : NULL;
    const ns_css_value *mxw = box->style ? box->style->values[NS_CSS_MAX_WIDTH]  : NULL;
    const ns_css_value *mxh = box->style ? box->style->values[NS_CSS_MAX_HEIGHT] : NULL;
    const ns_css_value *mnw = box->style ? box->style->values[NS_CSS_MIN_WIDTH]  : NULL;
    const ns_css_value *mnh = box->style ? box->style->values[NS_CSS_MIN_HEIGHT] : NULL;

    gboolean declared_size = box->media && box->media->declared_image_size;
    const char *parent_flex_dir = box->parent
        ? flex_direction_of(box->parent->style) : "row";
    gboolean flex_row_item = box->parent &&
        box_is_flex_container(box->parent) &&
        (strcmp(parent_flex_dir, "row") == 0 ||
         strcmp(parent_flex_dir, "row-reverse") == 0);
    double pct_width_base = cb_basis;
    if (flex_row_item && box->parent->content_width > 0)
        pct_width_base = box->parent->content_width;
    double w = -1, h = -1;
    gboolean svg_flex_main = box->kind == NS_BOX_SVG && box->has_flex_main &&
                             flex_row_item;
    box->has_flex_main = FALSE;
    if (svg_flex_main)
        w = box->flex_main_size > 0 ? box->flex_main_size : 0;
    else if (wv && (wv->kind == NS_CSS_V_LENGTH || wv->kind == NS_CSS_V_CALC))
        w = length_resolve(wv, pct_width_base, -1);
    else if (height_keyword_stretches(wv)) {
        w = parent_content_width
          - box->margin.left - box->margin.right
          - box->padding.left - box->padding.right
          - box->border.left - box->border.right;
        if (w < 0) w = 0;
    }
    if (hv && (hv->kind == NS_CSS_V_LENGTH || hv->kind == NS_CSS_V_CALC)) {
        if (value_is_percent(hv)) {
            double cb_h = containing_block_definite_height(box);
            if (cb_h >= 0) {
                h = (hv->kind == NS_CSS_V_CALC)
                    ? hv->u.calc.pct / 100.0 * cb_h + hv->u.calc.px
                    : hv->u.length.v * cb_h / 100.0;
            }
        } else {
            h = resolve_used_height(box, hv, parent_content_width, -1);
        }
    } else if (height_keyword_stretches(hv)) {
        double cb_h = containing_block_definite_height(box);
        if (cb_h >= 0) {
            h = cb_h - box->margin.top - box->margin.bottom
              - box->padding.top - box->padding.bottom
              - box->border.top - box->border.bottom;
            if (h < 0) h = 0;
        }
    }

    const ns_image *img = box->media ? (const ns_image *)box->media->image : NULL;
    double density = box->media && box->media->image_density > 0
        ? box->media->image_density : 1.0;
    double nat_w = (img && img->loaded && img->natural_width > 0)
                   ? (double)img->natural_width / density : -1;
    double nat_h = (img && img->loaded && img->natural_height > 0)
                   ? (double)img->natural_height / density : -1;
    double video_w, video_h;
    if (video_natural_size(box, &video_w, &video_h)) {
        nat_w = video_w;
        nat_h = video_h;
    }
    if (declared_size) {
        if (nat_w < 0 && box->content_width  > 0) nat_w = box->content_width;
        if (nat_h < 0 && box->content_height > 0) nat_h = box->content_height;
    }
    double only_ratio = image_ratio_only(box);
    if (only_ratio > 0) nat_w = nat_h = -1;
    double intrinsic_ratio = only_ratio > 0 ? only_ratio
                           : nat_w > 0 && nat_h > 0 ? nat_w / nat_h : -1;
    if (box->kind == NS_BOX_SVG && box->dom) {
        ns_svg_size svg_size;
        ns_svg_intrinsic_size(box->dom, &svg_size);
        if (svg_size.has_ratio && svg_size.ratio > 0)
            intrinsic_ratio = svg_size.ratio;
    }
    gboolean ratio_with_auto = FALSE;
    double specified_ratio = box->style
        ? aspect_ratio_number(box->style->values[NS_CSS_ASPECT_RATIO], &ratio_with_auto) : -1;
    gboolean ratio_overrides = specified_ratio > 0 &&
        (!ratio_with_auto || intrinsic_ratio <= 0);
    if (ratio_overrides) intrinsic_ratio = specified_ratio;

    gboolean metadata_video =
        box->kind == NS_BOX_VIDEO && node_has_media_metadata(box->dom);
    gboolean video_without_metadata = box->kind == NS_BOX_VIDEO && !metadata_video &&
        ns_node_is_element_named(box->dom, "video");
    if (nat_w < 0 && box->content_width  > 0) nat_w = box->content_width;
    if (nat_h < 0 && box->content_height > 0) nat_h = box->content_height;

    gboolean ratio_only = box->media &&
                          (box->media->intrinsic_ratio_only || only_ratio > 0);
    gboolean img_sourceless = box->kind == NS_BOX_IMAGE &&
        ns_node_is_element_named(box->dom, "img") && !(img && img->loaded) &&
        !img_has_source(box->dom);
    gboolean w_specified = w >= 0;
    gboolean h_specified = h >= 0;
    if (w < 0 && h < 0) {
        if (ratio_only && intrinsic_ratio > 0) {
            double cb_h = containing_block_definite_height(box);
            double fill = parent_content_width;
            if (only_ratio > 0)
                fill -= box->margin.left + box->margin.right +
                        box->padding.left + box->padding.right +
                        box->border.left + box->border.right;
            if (fill > 0) {
                w = fill; h = w / intrinsic_ratio;
            } else if (cb_h >= 0 && only_ratio <= 0) {
                h = cb_h; w = h * intrinsic_ratio;
            } else { w = 0; h = 0; }
        } else if (nat_w > 0 && nat_h > 0) {
            w = nat_w;
            h = ratio_overrides && !ratio_with_auto ? w / intrinsic_ratio : nat_h;
        } else { w = 0; h = 0; }
    } else if (w < 0) {
        w = intrinsic_ratio > 0 ? h * intrinsic_ratio
          : video_without_metadata ? 300 : img_sourceless ? 0 : h;
    } else if (h < 0) {
        h = intrinsic_ratio > 0 ? w / intrinsic_ratio
          : video_without_metadata ? 150 : img_sourceless ? 0 : w;
    }
    if (metadata_video && h <= 0 && w > 0 && nat_w > 0 && nat_h > 0)
        h = w * (nat_h / nat_w);
    if (metadata_video && w <= 0 && h > 0 && nat_w > 0 && nat_h > 0)
        w = h * (nat_w / nat_h);

    double bs_extras = image_border_box_extras(box);
    double max_w = image_content_limit(mxw, pct_width_base, bs_extras);
    double max_h = resolve_used_height(box, mxh, parent_content_width, -1);
    double min_w = image_content_limit(mnw, pct_width_base, bs_extras);
    double min_h = resolve_used_height(box, mnh, parent_content_width, -1);
    double keyword_w = intrinsic_ratio > 0 && h_specified ? h * intrinsic_ratio : nat_w;
    if (max_w < 0 && replaced_size_keyword(mxw) && keyword_w >= 0) max_w = keyword_w;
    if (min_w < 0 && replaced_size_keyword(mnw) && keyword_w >= 0) min_w = keyword_w;

    if (max_w >= 0 && w > max_w) {
        if (h > 0 && w > 0 && !h_specified) h *= max_w / w;
        w = max_w;
    }
    if (max_h >= 0 && h > max_h) {
        if (w > 0 && h > 0 && !w_specified) w *= max_h / h;
        h = max_h;
    }
    if (min_w >= 0 && w < min_w) {
        if (h > 0 && w > 0 && !h_specified && intrinsic_ratio > 0) h = min_w / intrinsic_ratio;
        w = min_w;
    }
    if (min_h >= 0 && h < min_h) {
        if (w > 0 && h > 0 && !w_specified && intrinsic_ratio > 0) w = min_h * intrinsic_ratio;
        h = min_h;
    }

    box->content_width = w;
    box->content_height = h;
}

static gboolean
inline_atomic_needs_layout(const ns_box *ab)
{
    if (!ab) return FALSE;
    return !(g_abs_ph_set && g_hash_table_contains(g_abs_ph_set, ab));
}

static double
inline_atomic_measure_basis(const ns_box *box)
{
    const ns_css_value *wv = box && box->style
        ? box->style->values[NS_CSS_WIDTH] : NULL;
    gboolean replaced = box && (box->kind == NS_BOX_IMAGE ||
                                box->kind == NS_BOX_VIDEO ||
                                box->kind == NS_BOX_SVG);
    if (value_is_percent(wv) && !replaced) {
        double content = measure_natural_width((ns_box *)box, box->style);
        if (content >= 0) return content;
    }
    double basis = ns_css_container_w();
    if (!(basis > 0)) {
        for (const ns_box *p = box ? box->parent : NULL; p; p = p->parent) {
            if (p->content_width > 0) {
                basis = p->content_width;
                break;
            }
        }
    }
    if (!(basis > 0)) basis = ns_css_viewport_w();
    if (!(basis > 0)) basis = 1000;
    if (basis < 32) basis = 32;
    if (basis > 1600) basis = 1600;
    return basis;
}

static gboolean
grid_flows_by_column(const ns_style *s)
{
    const ns_css_value *fv = s ? s->values[NS_CSS_GRID_AUTO_FLOW] : NULL;
    return fv && fv->kind == NS_CSS_V_KEYWORD && fv->u.keyword &&
           strstr(fv->u.keyword, "column") != NULL;
}

static int
grid_explicit_row_count(const ns_style *s)
{
    const ns_css_value *rv = s ? s->values[NS_CSS_GRID_TEMPLATE_ROWS] : NULL;
    if (!rv || rv->kind != NS_CSS_V_TRACKS || rv->u.tracks.subgrid ||
        rv->u.tracks.auto_repeat != NS_CSS_AUTO_REPEAT_NONE)
        return 1;
    return rv->u.tracks.n > 1 ? rv->u.tracks.n : 1;
}

static double
grid_column_flow_width(ns_box *box, const ns_style *child_style,
                       gboolean min_content)
{
    int rows = grid_explicit_row_count(box->style);
    double sum = 0, column = 0;
    int in_column = 0, columns = 0;
    for (ns_box *c = box->first_child; c; c = c->next_sibling) {
        if (style_is_absolute_or_fixed(c->style)) continue;
        double w = min_content ? min_width_of(c, child_style)
                               : measure_natural_width(c, child_style);
        if (c->style) {
            ns_edges m = {0}, pd = {0}, bd = {0};
            edges_from_style(c->style, 0, &m, &pd, &bd);
            w += m.left + m.right + pd.left + pd.right + bd.left + bd.right;
        }
        if (w > column) column = w;
        if (++in_column == rows) {
            sum += column;
            column = 0;
            in_column = 0;
            columns++;
        }
    }
    if (in_column > 0) {
        sum += column;
        columns++;
    }
    if (columns == 0) return -1;
    return sum + flex_gap_of(box->style, 0) * (columns - 1);
}

static double grid_max_content_width(ns_box *box,
                                     const ns_style *child_style);

static double
grid_natural_width(ns_box *box, const ns_style *child_style)
{
    if (grid_flows_by_column(box->style))
        return grid_column_flow_width(box, child_style, FALSE);
    return grid_max_content_width(box, child_style);
}

static guint
table_column_count(const ns_box *box)
{
    guint max_cols = 0;
    for (ns_box *row = box->first_child; row; row = row->next_sibling) {
        if (row->kind != NS_BOX_TABLE_ROW) continue;
        guint c = 0;
        for (ns_box *cell = row->first_child; cell; cell = cell->next_sibling) {
            c += cell->colspan > 0 ? (guint)cell->colspan : 1;
            if (c > NS_TABLE_MAX_COLS) { c = NS_TABLE_MAX_COLS; break; }
        }
        if (c > max_cols) max_cols = c;
    }
    return max_cols;
}

static void
table_widen_columns(double *cols, guint max_cols, guint col, int span,
                    double outer)
{
    if (span < 1) span = 1;
    double per = outer / (double)span;
    for (int i = 0; i < span && col + (guint)i < max_cols; i++)
        if (per > cols[col + (guint)i]) cols[col + (guint)i] = per;
}

static double
table_cell_definite_width(const ns_style *s, ns_css_prop prop, double h_extra)
{
    const ns_css_value *v = s ? s->values[prop] : NULL;
    if (!v || !(v->kind == NS_CSS_V_LENGTH || v->kind == NS_CSS_V_CALC) ||
        value_is_percent(v))
        return -1;
    double w = length_resolve(v, 0, -1);
    return w >= 0 ? w + h_extra : -1;
}

static double
table_cell_clamp(const ns_box *cell, double h_extra, double w)
{
    const ns_style *s = cell ? cell->style : NULL;
    double max_w = table_cell_definite_width(s, NS_CSS_MAX_WIDTH, h_extra);
    double min_w = table_cell_definite_width(s, NS_CSS_MIN_WIDTH, h_extra);
    if (max_w >= 0 && w > max_w) w = max_w;
    if (min_w >= 0 && w < min_w) w = min_w;
    return w;
}

static double
table_caption_min_content_width(ns_box *box, const ns_style *inherited)
{
    double widest = 0;
    for (ns_box *c = box->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_BOX_TABLE_CAPTION) continue;
        const ns_style *cs = c->style ? c->style : inherited;
        ns_edges m = {0}, pd = {0}, bd = {0};
        edges_from_style(c->style, 0, &m, &pd, &bd);
        double w = min_width_of(c, cs) +
                   m.left + m.right + pd.left + pd.right + bd.left + bd.right;
        if (w > widest) widest = w;
    }
    if (widest <= 0) return 0;
    ns_edges m = {0}, pd = {0}, bd = {0};
    edges_from_style(box->style, 0, &m, &pd, &bd);
    return MAX(widest - pd.left - pd.right - bd.left - bd.right, 0);
}

#define NS_TABLE_MAX_INLINE_SIZE 1000000.0

static gboolean
table_is_fixed_with_percent_width(const ns_box *box)
{
    const ns_style *s = box->style;
    if (!s || !keyword_is(s->values[NS_CSS_TABLE_LAYOUT], "fixed"))
        return FALSE;
    const ns_css_value *wv = s->values[NS_CSS_WIDTH];
    return wv && value_is_percent(wv) && length_resolve(wv, 100, -1) > 0;
}

static double
table_intrinsic_width(ns_box *box, const ns_style *inherited, gboolean min)
{
    if (!min && table_is_fixed_with_percent_width(box))
        return NS_TABLE_MAX_INLINE_SIZE;
    double captions = table_caption_min_content_width(box, inherited);
    guint max_cols = table_column_count(box);
    if (max_cols == 0) return captions;
    double *cols = g_new0(double, max_cols);
    for (ns_box *row = box->first_child; row; row = row->next_sibling) {
        if (row->kind != NS_BOX_TABLE_ROW) continue;
        guint col = 0;
        for (ns_box *cell = row->first_child; cell && col < max_cols;
             cell = cell->next_sibling) {
            int span = cell->colspan > 0 ? cell->colspan : 1;
            const ns_style *cs = cell->style ? cell->style : inherited;
            ns_edges m = {0}, pd = {0}, bd = {0};
            edges_from_style(cell->style, 0, &m, &pd, &bd);
            double extra = m.left + m.right + pd.left + pd.right +
                           bd.left + bd.right;
            double content_min = min_content_width_of(cell, cs);
            double w = min ? content_min : measure_natural_width(cell, cs);
            const ns_css_value *wv = cell->style
                ? cell->style->values[NS_CSS_WIDTH] : NULL;
            if (wv && (wv->kind == NS_CSS_V_LENGTH || wv->kind == NS_CSS_V_CALC) &&
                !value_is_percent(wv)) {
                double e = length_resolve(wv, 0, -1);
                if (e >= 0) w = e > content_min ? e : content_min;
            }
            table_widen_columns(cols, max_cols, col, span,
                                table_cell_clamp(cell, extra, w + extra));
            col += (guint)span;
        }
    }
    if (box->table_col_hints) {
        guint col = 0;
        for (guint i = 0; i < box->table_col_hints->len && col < max_cols; i++) {
            ns_table_col_hint *hint =
                &g_array_index(box->table_col_hints, ns_table_col_hint, i);
            int hspan = hint->span > 0 ? hint->span : 1;
            const ns_css_value *wv = hint->style
                ? hint->style->values[NS_CSS_WIDTH] : NULL;
            if (wv && wv->kind == NS_CSS_V_LENGTH &&
                wv->u.length.unit != NS_CSS_UNIT_PERCENT) {
                double w = length_resolve(wv, 0, -1);
                if (w >= 0) table_widen_columns(cols, max_cols, col, hspan, w);
            }
            col += (guint)hspan;
        }
    }
    double hsp = 0, vsp = 0;
    table_border_spacing(box->style, &hsp, &vsp);
    double sum = (double)(max_cols + 1) * hsp;
    for (guint i = 0; i < max_cols; i++) sum += cols[i];
    g_free(cols);
    return sum > captions ? sum : captions;
}

static gboolean
box_inline_size_is_definite(const ns_box *box)
{
    for (const ns_box *b = box; b; b = b->parent) {
        if (b->kind == NS_BOX_TABLE_CELL) return FALSE;
        const ns_style *s = b->style;
        if (!s) continue;
        const ns_css_value *wv = s->values[NS_CSS_WIDTH];
        gboolean fixed = wv && (wv->kind == NS_CSS_V_LENGTH ||
                                wv->kind == NS_CSS_V_CALC) &&
                         !value_is_percent(wv);
        if (fixed) return TRUE;
        if (style_is_absolute_or_fixed(s) || float_side_of(s) >= 0 ||
            ns_display_is_atomic_inline(ns_css_display_of(s)))
            return FALSE;
        if (b->parent && (box_is_flex_container(b->parent) ||
                          box_is_grid_container(b->parent)))
            return FALSE;
    }
    return TRUE;
}

static double
replaced_height_for_width(const ns_box *box)
{
    const ns_css_value *hv = box->style ? box->style->values[NS_CSS_HEIGHT] : NULL;
    if (!hv || !(hv->kind == NS_CSS_V_LENGTH || hv->kind == NS_CSS_V_CALC))
        return -1;
    double h;
    if (!value_is_percent(hv)) {
        h = length_resolve(hv, 0, -1);
    } else {
        double cb_h = containing_block_definite_height(box);
        h = cb_h >= 0 ? resolve_height_with_basis(hv, 0, cb_h, -1) : -1;
    }
    if (h > 0 && ns_css_keyword_is(box->style->values[NS_CSS_BOX_SIZING],
                                   "border-box")) {
        ns_edges m = {0}, pd = {0}, bd = {0};
        edges_from_style(box->style, 0, &m, &pd, &bd);
        h -= pd.top + pd.bottom + bd.top + bd.bottom;
        if (h < 0) h = 0;
    }
    return h;
}

static const ns_box *
replaced_containing_box(const ns_box *box)
{
    for (const ns_box *p = box->parent; p; p = p->parent)
        if (p->kind != NS_BOX_INLINE) return p;
    return NULL;
}

static gboolean
replaced_containing_block_is_definite(const ns_box *box)
{
    if (box->parent && box_is_grid_container(box->parent))
        return FALSE;
    const ns_box *cb = replaced_containing_box(box);
    return cb && box_inline_size_is_definite(cb);
}

static double
image_ratio_only(const ns_box *box)
{
    const ns_image *img = box->kind == NS_BOX_IMAGE && box->media
        ? (const ns_image *)box->media->image : NULL;
    return img && img->loaded ? img->natural_ratio_only : 0;
}

static double
ratio_only_replaced_width(const ns_box *box, gboolean max_content)
{
    double ratio = image_ratio_only(box);
    double h = replaced_height_for_width(box);
    if (h > 0 && ratio > 0) return h * ratio;
    if (h > 0) {
        ns_svg_size size;
        ns_svg_intrinsic_size(box->dom, &size);
        if (size.has_ratio && size.ratio > 0) return h * size.ratio;
    }
    if (max_content && replaced_containing_block_is_definite(box)) {
        const ns_box *cb = replaced_containing_box(box);
        if (cb && cb->content_width > 0) return cb->content_width;
    }
    return 0;
}

static double
loaded_image_auto_width(const ns_box *box, const ns_image *img)
{
    double h = replaced_height_for_width(box);
    if (h > 0) return h * img->natural_width / img->natural_height;
    double density = box->media->image_density > 0
        ? box->media->image_density : 1.0;
    return img->natural_width / density;
}

static double
svg_auto_width(const ns_box *box)
{
    ns_svg_size size;
    ns_svg_intrinsic_size(box->dom, &size);
    double h = replaced_height_for_width(box);
    if (h > 0 && size.has_ratio && size.ratio > 0) return h * size.ratio;
    return size.has_width && size.width > 0 ? size.width : 300;
}

static double
width_attribute_px(const ns_node *dom)
{
    const char *width_attr = dom ? ns_element_get_attr(dom, "width") : NULL;
    if (!width_attr || strchr(width_attr, '%')) return 0;
    return image_dimension_attr(dom, "width");
}

static double
replaced_auto_width(const ns_box *box)
{
    if (image_ratio_only(box) > 0) return ratio_only_replaced_width(box, FALSE);
    if (box->media && box->media->intrinsic_ratio_only) {
        double w = ratio_only_replaced_width(box, FALSE);
        return w > 0 ? w : 300;
    }
    const ns_image *img = box->media ? (const ns_image *)box->media->image : NULL;
    if (img && img->loaded && img->natural_width > 0 && img->natural_height > 0)
        return loaded_image_auto_width(box, img);
    if (box->kind == NS_BOX_SVG && box->dom) return svg_auto_width(box);
    double video_w, video_h;
    if (video_natural_size(box, &video_w, &video_h)) {
        double h = replaced_height_for_width(box);
        return h > 0 ? h * video_w / video_h : video_w;
    }
    double attr = width_attribute_px(box->dom);
    if (attr > 0) return attr;
    if (box->kind == NS_BOX_VIDEO) return 300;
    return box->media && box->media->placeholder_image_size ? 200 : 0;
}

static gboolean
replaced_width_is_cyclic(const ns_box *box)
{
    if (!box || !(box->kind == NS_BOX_IMAGE || box->kind == NS_BOX_VIDEO ||
                  box->kind == NS_BOX_SVG))
        return FALSE;
    const ns_css_value *wv = box->style ? box->style->values[NS_CSS_WIDTH] : NULL;
    if (value_is_percent(wv)) return !replaced_containing_block_is_definite(box);
    gboolean sized = wv && (wv->kind == NS_CSS_V_LENGTH || wv->kind == NS_CSS_V_CALC);
    return !sized && box->media &&
           (box->media->intrinsic_ratio_only || image_ratio_only(box) > 0);
}

static gboolean
replaced_width_is_percent(const ns_box *box)
{
    return (box->kind == NS_BOX_IMAGE || box->kind == NS_BOX_VIDEO ||
            box->kind == NS_BOX_SVG) && box->style &&
           value_is_percent(box->style->values[NS_CSS_WIDTH]);
}

static gboolean
replaced_max_width_is_percent(const ns_box *box)
{
    return (box->kind == NS_BOX_IMAGE || box->kind == NS_BOX_VIDEO ||
            box->kind == NS_BOX_SVG) && box->style &&
           value_is_percent(box->style->values[NS_CSS_MAX_WIDTH]);
}

static double definite_width_limit(const ns_box *box, const ns_css_value *v);

static double
replaced_contribution_limits(const ns_box *box, double w)
{
    if (!box->style) return w;
    double max_w = definite_width_limit(box, box->style->values[NS_CSS_MAX_WIDTH]);
    double min_w = definite_width_limit(box, box->style->values[NS_CSS_MIN_WIDTH]);
    if (max_w >= 0 && w > max_w) w = max_w;
    if (min_w >= 0 && w < min_w) w = min_w;
    return w;
}

static double
replaced_intrinsic_contribution(const ns_box *box, gboolean max_content)
{
    const ns_css_value *wv = box->style ? box->style->values[NS_CSS_WIDTH] : NULL;
    double w;
    if (value_is_percent(wv))
        w = max_content ? replaced_auto_width(box) : 0;
    else
        w = ratio_only_replaced_width(box, max_content);
    const ns_css_value *mxw = box->style ? box->style->values[NS_CSS_MAX_WIDTH] : NULL;
    const ns_css_value *mnw = box->style ? box->style->values[NS_CSS_MIN_WIDTH] : NULL;
    double max_w = value_is_percent(mxw) ? -1 : length_resolve(mxw, 0, -1);
    double min_w = value_is_percent(mnw) ? -1 : length_resolve(mnw, 0, -1);
    if (max_w >= 0 && w > max_w) w = max_w;
    if (min_w >= 0 && w < min_w) w = min_w;
    return w;
}

typedef struct ns_atomic_geometry {
    ns_box  *box;
    double   width, height;
    ns_edges margin, padding, border;
} ns_atomic_geometry;

static GArray *
measure_inline_atomics_begin(ns_box *box, const ns_style *parent_style,
                             gboolean max_content)
{
    GArray *saved = NULL;
    if (!box->inline_atomics) return NULL;
    for (guint ai = 0; ai < box->inline_atomics->len; ai++) {
        ns_box *ab = g_array_index(box->inline_atomics, ns_inline_atomic, ai).box;
        if (!ab) continue;
        gboolean cyclic = replaced_width_is_cyclic(ab) ||
            (!max_content && (replaced_width_is_percent(ab) ||
                              replaced_max_width_is_percent(ab)));
        gboolean to_min_content = !max_content && !cyclic &&
            (ab->kind == NS_BOX_BLOCK || ab->kind == NS_BOX_TABLE);
        if (cyclic || to_min_content) {
            if (!saved)
                saved = g_array_new(FALSE, FALSE, sizeof(ns_atomic_geometry));
            ns_atomic_geometry g = {
                .box = ab, .width = ab->content_width, .height = ab->content_height,
                .margin = ab->margin, .padding = ab->padding, .border = ab->border,
            };
            g_array_append_val(saved, g);
        }
        if (inline_atomic_needs_layout(ab))
            layout_box(ab, inline_atomic_measure_basis(ab), parent_style);
        if (to_min_content) {
            double w = min_width_of(ab, parent_style);
            if (w >= 0 && w < ab->content_width) ab->content_width = w;
        }
        if (cyclic) {
            double w = replaced_intrinsic_contribution(ab, max_content);
            double ratio = ab->content_width > 0 && ab->content_height > 0
                ? ab->content_width / ab->content_height : 0;
            ab->content_width = w;
            if (ratio > 0) ab->content_height = w / ratio;
        }
    }
    return saved;
}

static void
measure_inline_atomics_end(GArray *saved)
{
    if (!saved) return;
    for (guint i = 0; i < saved->len; i++) {
        ns_atomic_geometry *g = &g_array_index(saved, ns_atomic_geometry, i);
        g->box->content_width = g->width;
        g->box->content_height = g->height;
        g->box->margin = g->margin;
        g->box->padding = g->padding;
        g->box->border = g->border;
    }
    g_array_free(saved, TRUE);
}

static gboolean node_is_windowed_text_input(const ns_node *n);

static const char *const text_input_unreliable_avg_families[] = {
    "American Typewriter", "Arial Hebrew", "Chalkboard", "Cochin",
    "Corsiva Hebrew", "Courier", "Euphemia UCAS", "Geneva", "Gill Sans",
    "Hei", "Helvetica", "Hoefler Text", "InaiMathi", "Kai",
    "Lucida Grande", "Marker Felt", "Monaco", "Mshtakan", "New Peninim MT",
    "Osaka", "Raanana", "STHeiti", "Symbol", "Times", "Apple Braille",
    "Apple LiGothic", "Apple LiSung", "Apple Symbols", "AppleGothic",
    "AppleMyungjo", "#GungSeo", "#HeadLineA", "#PCMyungjo", "#PilGi",
};

static gboolean
text_input_family_avg_reliable(const ns_style *s)
{
    const ns_css_value *fv = s ? s->values[NS_CSS_FONT_FAMILY] : NULL;
    const char *css = fv && fv->kind == NS_CSS_V_KEYWORD && fv->u.keyword
        ? fv->u.keyword : "sans-serif";
    char *first = ns_css_font_family_first_name(css);
    gboolean ok = first && *first;
    for (gsize i = 0; ok && i < G_N_ELEMENTS(text_input_unreliable_avg_families); i++)
        ok = strcmp(first, text_input_unreliable_avg_families[i]) != 0;
    g_free(first);
    return ok;
}

static double
text_glyph_advance_px(NsPangoLayout *layout, const char *ch)
{
    ns_pango_layout_set_text(layout, ch, -1);
    ns_text_measure m;
    text_measure(layout, &m);
    return (double)m.logical.width / NS_PANGO_SCALE;
}

static void
text_input_char_widths(const ns_style *s, double *avg, double *max)
{
    NsPangoLayout *layout = make_pango_layout(s);
    ns_pango_layout_set_width(layout, -1);
    double x = text_glyph_advance_px(layout, "x");
    double zero = text_glyph_advance_px(layout, "0");
    g_object_unref(layout);
    *avg = zero;
    *max = 0;
    if (!text_input_family_avg_reliable(s) || (zero > 0 && x > zero * 1.7))
        return;
    ns_css_font_metrics fm = { 0 };
    *avg = MAX(x, round(x));
    if (ns_paint_style_font_metrics(s, &fm)) *max = fm.ascent_px;
}

typedef struct select_width_entry {
    const ns_style *style;
    double widest;
    double drawn;
} select_width_entry;

static double
letter_spacing_slack(double letter_spacing_px)
{
#ifdef NS_USE_WEB_TEXT_LIB
    (void)letter_spacing_px;
    return 0;
#else
    return MAX(letter_spacing_px, 0);
#endif
}

static double
letter_spacing_slack_px(const ns_style *s)
{
    return letter_spacing_slack(style_letter_spacing_px(s));
}

static double
select_label_width(NsPangoLayout *l, const ns_style *s, const char *label)
{
    gboolean arrow = !keyword_is(s->values[NS_CSS_APPEARANCE], "none");
    char *text = g_strdup_printf(arrow ? "\xc2\xa0%s \xe2\x96\xbe\xc2\xa0"
                                       : "\xc2\xa0%s\xc2\xa0", label);
    ns_pango_layout_set_text(l, text, -1);
    NsPangoAttrList *attrs = ns_pango_attr_list_new();
    ns_paint_apply_font_features(attrs, s, 0, G_MAXUINT);
    ns_inline_apply_spacing(attrs, s, text);
    ns_pango_layout_set_attributes(l, attrs);
    ns_pango_attr_list_unref(attrs);
    g_free(text);
    ns_text_measure m;
    text_measure(l, &m);
    return ceil((double)m.logical.width / NS_PANGO_SCALE +
                letter_spacing_slack_px(s));
}

static void
select_widest_label_walk(const ns_node *n, const ns_style *s,
                         NsPangoLayout *l, int depth, double *widest)
{
    if (depth > 8) return;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "optgroup")) {
            select_widest_label_walk(c, s, l, depth + 1, widest);
            continue;
        }
        if (!ns_node_is_element_named(c, "option")) continue;
        char *label = ns_node_collect_text(c);
        double w = select_label_width(l, s, label ? label : "");
        g_free(label);
        if (w > *widest) *widest = w;
    }
}

static double
select_widest_label_measure(const ns_node *n, const ns_style *s)
{
    NsPangoLayout *l = make_pango_layout(s);
    double widest = 0;
    select_widest_label_walk(n, s, l, 0, &widest);
    g_object_unref(l);
    return widest;
}

static select_width_entry *
select_width_entry_get(const ns_node *n, const ns_style *s)
{
    if (!g_select_width_cache) return NULL;
    select_width_entry *e = g_hash_table_lookup(g_select_width_cache, n);
    if (!e) {
        e = g_new0(select_width_entry, 1);
        g_hash_table_insert(g_select_width_cache, (gpointer)n, e);
    }
    if (e->style != s) {
        e->style = s;
        e->widest = select_widest_label_measure(n, s);
        e->drawn = -1;
    }
    return e;
}

static double
select_widest_label(const ns_node *n, const ns_style *s)
{
    select_width_entry *e = select_width_entry_get(n, s);
    return e ? e->widest : select_widest_label_measure(n, s);
}

static double
select_drawn_label_width(const ns_box *box)
{
    double w = 0;
    for (ns_box *c = box->first_child; c; c = c->next_sibling)
        if (c->kind == NS_BOX_INLINE)
            w = MAX(w, measure_natural_width(c, box->style));
    return w;
}

static double
menulist_label_width(const ns_box *box)
{
    select_width_entry *e = select_width_entry_get(box->dom, box->style);
    if (!e)
        return MAX(select_widest_label_measure(box->dom, box->style),
                   select_drawn_label_width(box));
    if (e->drawn < 0) e->drawn = select_drawn_label_width(box);
    return MAX(e->widest, e->drawn);
}

static double
menulist_intrinsic_width(const ns_box *box)
{
    const char *size = ns_element_get_attr(box->dom, "size");
    if (!box->style || ns_css_writing_mode(box->style) != 0 ||
        ns_element_get_attr(box->dom, "multiple") ||
        (size && ns_parse_int(size, 0, 0, 1000) > 1))
        return 0;
    return menulist_label_width(box);
}

static double
text_input_intrinsic_width(const ns_box *box)
{
    if (box && ns_node_is_element_named(box->dom, "select"))
        return menulist_intrinsic_width(box);
    if (!box || !node_is_windowed_text_input(box->dom)) return 0;
    int size = ns_parse_int(ns_element_get_attr(box->dom, "size"), 0, 0,
                            G_MAXINT16);
    if (size <= 0) size = 20;
    double avg = 0, max = 0;
    text_input_char_widths(box->style, &avg, &max);
    double width = avg * size;
    if (max > avg) width += max - avg;
    return ceil(width);
}

static double ratio_transferred_width(const ns_box *box);

static double
measure_natural_width(ns_box *box, const ns_style *parent_style)
{
    if (!box) return 0;
    if (box->kind == NS_BOX_INLINE) {
        if (box->text && *box->text && ns_css_writing_mode(parent_style) &&
            !(box->inline_atomics && box->inline_atomics->len > 0)) {
            double thickness = 0, length = 0;
            ns_vertical_measure(box, parent_style, &thickness, &length);
            return thickness;
        }
        if (!box->text || !*box->text) {
            if (!box->inline_atomics || box->inline_atomics->len == 0)
                return 0;
            double sum = 0;
            GArray *saved = measure_inline_atomics_begin(box, parent_style, TRUE);
            for (guint ai = 0; ai < box->inline_atomics->len; ai++) {
                ns_box *ab = g_array_index(box->inline_atomics,
                                           ns_inline_atomic, ai).box;
                if (!ab) continue;
                sum += ab->content_width +
                       ab->margin.left + ab->margin.right +
                       ab->padding.left + ab->padding.right +
                       ab->border.left + ab->border.right;
            }
            measure_inline_atomics_end(saved);
            return sum;
        }
        gboolean cacheable = inline_box_measure_cacheable(box);
        if (cacheable && box->inline_natural_cache_valid &&
            box->inline_natural_cache_style == parent_style)
            return box->inline_natural_cache_width;
        NsPangoLayout *layout = make_pango_layout(parent_style);
        ns_pango_layout_set_width(layout, -1);
        GArray *saved = measure_inline_atomics_begin(box, parent_style, TRUE);
        ns_pango_layout_set_text(layout, box->text, -1);
        NsPangoAttrList *i18n = ns_pango_attr_list_new();
        ns_paint_apply_i18n(layout, i18n, box);
        ns_paint_apply_font_features(i18n, parent_style, 0, G_MAXUINT);
        ns_inline_apply_atomic_shapes(i18n, box);
        ns_inline_apply_spacing(i18n, parent_style, box->text);
        apply_inline_layout_attrs(i18n, box);
        ns_inline_layout_set_attrs(layout, i18n, box);
        ns_pango_attr_list_unref(i18n);
        ns_text_measure natural;
        text_measure(layout, &natural);
        NsPangoRectangle logical = natural.logical;
        double slack = letter_spacing_slack_px(parent_style);
        for (guint ai = 0; box->attrs && ai < box->attrs->len; ai++) {
            const ns_inline_attr *r =
                &g_array_index(box->attrs, ns_inline_attr, ai);
            if (r->kind == NS_INLINE_LETTER_SPACING)
                slack = MAX(slack, letter_spacing_slack(r->letter_spacing_px));
        }
        double pw = ceil((double)logical.width / NS_PANGO_SCALE + slack);
        if (box->inline_atomics) {
            for (guint ai = 0; ai < box->inline_atomics->len; ai++) {
                const ns_inline_atomic *a = &g_array_index(
                    box->inline_atomics, ns_inline_atomic, ai);
                if (!a->box) continue;
                NsPangoRectangle pos;
                ns_pango_layout_index_to_pos(layout, (int)a->byte_off, &pos);
                double end = (double)(pos.x + pos.width) / NS_PANGO_SCALE;
                if (pw < end) pw = ceil(end);
            }
        }
        measure_inline_atomics_end(saved);
        g_object_unref(layout);
        if (cacheable) {
            box->inline_natural_cache_style = parent_style;
            box->inline_natural_cache_width = pw;
            box->inline_natural_cache_valid = TRUE;
        }
        return pw;
    }
    if (box->kind == NS_BOX_IMAGE || box->kind == NS_BOX_VIDEO ||
        box->kind == NS_BOX_SVG) {
        if (replaced_width_is_cyclic(box))
            return replaced_intrinsic_contribution(box, TRUE);
        const ns_css_value *wv = box->style
            ? box->style->values[NS_CSS_WIDTH] : NULL;
        if (wv && (wv->kind == NS_CSS_V_LENGTH ||
                   wv->kind == NS_CSS_V_CALC)) {
            double styled = length_resolve(wv, inline_atomic_measure_basis(box), -1);
            if (styled >= 0) return replaced_contribution_limits(box, styled);
        }
        return replaced_contribution_limits(
            box, box->content_width > 0 ? box->content_width : 200);
    }
    if (box->kind == NS_BOX_TEXT) {
        return box->content_width > 0 ? box->content_width : 0;
    }
    {
        const ns_css_value *wv = box->style ? box->style->values[NS_CSS_WIDTH] : NULL;
        double w = -1;
        if (wv && wv->kind == NS_CSS_V_LENGTH &&
            (wv->u.length.unit == NS_CSS_UNIT_PX ||
             wv->u.length.unit == NS_CSS_UNIT_NUMBER))
            w = wv->u.length.v;
        else if (wv && wv->kind == NS_CSS_V_CALC && !value_is_percent(wv))
            w = length_resolve(wv, 0, -1);
        else
            w = ratio_transferred_width(box);
        if (w >= 0) {
            if (flex_box_is_border_box(box)) {
                ns_edges m = {0}, pd = {0}, bd = {0};
                edges_from_style(box->style, 0, &m, &pd, &bd);
                w -= pd.left + pd.right + bd.left + bd.right;
                if (w < 0) w = 0;
            }
            return width_contribution_keyword_limits(box, w, parent_style,
                                                     TRUE, NS_MEASURED_NONE);
        }
        if (keyword_is(wv, "min-content"))
            return width_contribution_keyword_limits(
                box, min_content_width_of(box, parent_style),
                parent_style, TRUE, NS_MEASURED_MIN_CONTENT);
    }
    return width_contribution_keyword_limits(
        box, measure_max_content_width(box, parent_style), parent_style, TRUE,
        NS_MEASURED_MAX_CONTENT);
}

static double
ratio_percent_height_basis(const ns_box *box)
{
    if (style_is_absolute_or_fixed(box->style))
        return box->cb_height_override > 0 ? box->cb_height_override : -1;
    const ns_box *p = box->parent;
    while (p && !p->style) p = p->parent;
    if (p && box_is_grid_container(p)) return -1;
    return containing_block_definite_height(box);
}

static double
ratio_transferred_width(const ns_box *box)
{
    if (!box->style || box->kind != NS_BOX_BLOCK) return -1;
    const ns_css_value *wv = box->style->values[NS_CSS_WIDTH];
    if (wv && !length_is_auto(wv) && !size_keyword_is_intrinsic(wv)) return -1;
    double ratio = aspect_ratio_number(box->style->values[NS_CSS_ASPECT_RATIO],
                                       NULL);
    if (ratio <= 0) return -1;
    const ns_css_value *hv = box->style->values[NS_CSS_HEIGHT];
    if (!hv || !(hv->kind == NS_CSS_V_LENGTH || hv->kind == NS_CSS_V_CALC))
        return -1;
    double h;
    if (value_is_percent(hv)) {
        double basis = ratio_percent_height_basis(box);
        if (basis < 0) return -1;
        h = resolve_height_with_basis(hv, 0, basis, -1);
    } else {
        h = length_resolve(hv, 0, -1);
    }
    if (h < 0) return -1;
    return clamp_height_minmax_px(box->style, h) * ratio;
}

static double
definite_width_limit(const ns_box *box, const ns_css_value *v)
{
    if (!v || !(v->kind == NS_CSS_V_LENGTH || v->kind == NS_CSS_V_CALC) ||
        value_is_percent(v))
        return -1;
    double limit = length_resolve(v, 0, -1);
    if (limit < 0) return -1;
    if (flex_box_is_border_box(box)) {
        ns_edges m = {0}, pd = {0}, bd = {0};
        edges_from_style(box->style, 0, &m, &pd, &bd);
        limit -= pd.left + pd.right + bd.left + bd.right;
        if (limit < 0) limit = 0;
    }
    return limit;
}

static double
width_contribution_keyword_limits(ns_box *box, double w,
                                  const ns_style *parent_style,
                                  gboolean max_content, ns_measured_kind measured)
{
    if (!box->style || (box->kind != NS_BOX_BLOCK && box->kind != NS_BOX_TABLE))
        return w;
    double content = w;
    const ns_css_value *mxw = box->style->values[NS_CSS_MAX_WIDTH];
    const ns_css_value *mnw = box->style->values[NS_CSS_MIN_WIDTH];
    double max_limit = definite_width_limit(box, mxw);
    if (max_limit >= 0 && w > max_limit) w = max_limit;
    double min_limit = definite_width_limit(box, mnw);
    if (min_limit >= 0 && w < min_limit) w = min_limit;
    if (size_keyword_is_intrinsic(mxw)) {
        gboolean use_max = keyword_is(mxw, "max-content") ||
                           (max_content && keyword_is(mxw, "fit-content"));
        ns_measured_kind need = use_max ? NS_MEASURED_MAX_CONTENT : NS_MEASURED_MIN_CONTENT;
        double m = need == measured ? content
                 : use_max ? measure_max_content_width(box, parent_style)
                           : min_content_width_of(box, parent_style);
        if (m >= 0 && w > m) w = m;
    }
    if (size_keyword_is_intrinsic(mnw)) {
        gboolean use_max = keyword_is(mnw, "max-content") ||
                           (max_content && keyword_is(mnw, "fit-content"));
        ns_measured_kind need = use_max ? NS_MEASURED_MAX_CONTENT : NS_MEASURED_MIN_CONTENT;
        double m = need == measured ? content
                 : use_max ? measure_max_content_width(box, parent_style)
                           : min_content_width_of(box, parent_style);
        if (m >= 0 && w < m) w = m;
    }
    return w;
}

static gboolean
box_content_independent_width(const ns_box *box, double *out)
{
    if (style_inline_size_contained(box->style, out)) return TRUE;
    *out = text_input_intrinsic_width(box);
    return *out > 0;
}

static double
atomic_inline_fit_width(ns_box *box, const ns_style *style, double avail)
{
    double input_width = text_input_intrinsic_width(box);
    if (input_width > 0) return input_width;
    double natural = measure_natural_width(box, style);
    if (natural <= avail) return natural;
    double min_content = min_content_width_of(box, style);
    if (min_content <= avail) return avail;
    return min_content < natural ? min_content : natural;
}

static double
measure_max_content_width(ns_box *box, const ns_style *parent_style)
{
    if (box->kind == NS_BOX_INLINE || box->kind == NS_BOX_IMAGE ||
        box->kind == NS_BOX_VIDEO || box->kind == NS_BOX_SVG ||
        box->kind == NS_BOX_TEXT)
        return measure_natural_width(box, parent_style);
    double contained_w;
    if (box_content_independent_width(box, &contained_w)) return contained_w;
    const ns_style *child_style = box->style ? box->style : parent_style;
    if (box->kind == NS_BOX_TABLE)
        return table_intrinsic_width(box, child_style, FALSE);
    if (box->style && box_is_grid_container(box)) {
        double gw = grid_natural_width(box, child_style);
        if (gw > 0) return gw;
    }
    gboolean flex_row = box_is_flex_container(box) &&
        strncmp(flex_direction_of(box->style), "row", 3) == 0;
    double max_child = 0;
    double float_row = 0;
    double row_sum = 0;
    int flex_items = 0;
    for (ns_box *c = box->first_child; c; c = c->next_sibling) {
        double w = measure_natural_width(c, child_style);
        int fside = float_side_of(c->style);
        double outer = w;
        if (c->style) {
            ns_edges m = {0}, pd = {0}, bd = {0};
            edges_from_style(c->style, 0, &m, &pd, &bd);
            outer += m.left + m.right + pd.left + pd.right + bd.left + bd.right;
        }
        if (flex_row) {
            row_sum += outer;
            flex_items++;
        } else if (box->kind == NS_BOX_BLOCK ||
                   box->kind == NS_BOX_TABLE_CELL ||
                   box->kind == NS_BOX_TABLE_CAPTION) {
            if (clear_kind_of(c->style)) float_row = 0;
            if (fside >= 0 || c->kind == NS_BOX_INLINE) {
                float_row += outer;
                if (float_row > max_child) max_child = float_row;
            } else {
                if (float_row + outer > max_child) max_child = float_row + outer;
                float_row = 0;
            }
        } else {
            max_child += w;
        }
    }
    if (flex_row && flex_items > 1)
        row_sum += flex_gap_of(box->style, 0) * (flex_items - 1);
    return flex_row ? row_sum : max_child;
}

static int g_min_measure_depth;

static gboolean
box_is_compressible_control(const ns_box *box)
{
    const ns_node *n = box->dom;
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    return strcmp(n->name, "input") == 0 || strcmp(n->name, "textarea") == 0 ||
           strcmp(n->name, "select") == 0;
}

static double measure_min_width(ns_box *box, const ns_style *parent_style);

static double
min_width_of(ns_box *box, const ns_style *parent_style)
{
    g_min_measure_depth++;
    double w = measure_min_width(box, parent_style);
    g_min_measure_depth--;
    return w;
}

static double
measure_min_width(ns_box *box, const ns_style *parent_style)
{
    if (!box) return 0;
    if (box->kind == NS_BOX_INLINE) {
        if (!box->text || !*box->text) return 0;
        if (ns_css_writing_mode(parent_style) &&
            !(box->inline_atomics && box->inline_atomics->len > 0)) {
            double thickness = 0, length = 0;
            ns_vertical_measure(box, parent_style, &thickness, &length);
            return thickness;
        }
        const ns_css_value *ws = parent_style
            ? parent_style->values[NS_CSS_WHITE_SPACE] : NULL;
        if (keyword_is(ws, "nowrap") || keyword_is(ws, "pre"))
            return measure_natural_width(box, parent_style);
        gboolean cacheable = inline_box_measure_cacheable(box);
        if (cacheable && box->inline_min_cache_valid &&
            box->inline_min_cache_style == parent_style)
            return box->inline_min_cache_width;
        double fast = measure_inline_ascii_min_width(box, parent_style);
        if (fast >= 0) {
            if (cacheable) {
                box->inline_min_cache_style = parent_style;
                box->inline_min_cache_width = fast;
                box->inline_min_cache_valid = TRUE;
            }
            return fast;
        }
        NsPangoLayout *layout = make_pango_layout(parent_style);
        ns_pango_layout_set_width(layout, 1);
        ns_pango_layout_set_wrap(layout, NS_PANGO_WRAP_WORD);
        GArray *saved = measure_inline_atomics_begin(box, parent_style, FALSE);
        ns_pango_layout_set_text(layout, box->text, -1);
        NsPangoAttrList *i18n = ns_pango_attr_list_new();
        ns_paint_apply_i18n(layout, i18n, box);
        ns_paint_apply_font_features(i18n, parent_style, 0, G_MAXUINT);
        ns_inline_apply_atomic_shapes(i18n, box);
        ns_inline_apply_spacing(i18n, parent_style, box->text);
        apply_inline_layout_attrs(i18n, box);
        ns_inline_layout_set_attrs(layout, i18n, box);
        ns_pango_attr_list_unref(i18n);
        int pw, ph;
        text_measure_pixel_size(layout, &pw, &ph);
        measure_inline_atomics_end(saved);
        g_object_unref(layout);
        if (cacheable) {
            box->inline_min_cache_style = parent_style;
            box->inline_min_cache_width = pw;
            box->inline_min_cache_valid = TRUE;
        }
        return pw;
    }
    if (box->kind == NS_BOX_IMAGE || box->kind == NS_BOX_VIDEO ||
        box->kind == NS_BOX_SVG) {
        const ns_css_value *max_width = box->style
            ? box->style->values[NS_CSS_MAX_WIDTH] : NULL;
        if (max_width && max_width->kind == NS_CSS_V_LENGTH &&
            max_width->u.length.unit == NS_CSS_UNIT_PERCENT)
            return 0;
        if (replaced_width_is_cyclic(box) ||
            (g_min_measure_depth > 1 && replaced_width_is_percent(box)))
            return replaced_intrinsic_contribution(box, FALSE);
        const ns_css_value *wv = box->style
            ? box->style->values[NS_CSS_WIDTH] : NULL;
        if (wv && !value_is_percent(wv) &&
            (wv->kind == NS_CSS_V_LENGTH || wv->kind == NS_CSS_V_CALC)) {
            double styled = length_resolve(wv, inline_atomic_measure_basis(box), -1);
            if (styled >= 0) return replaced_contribution_limits(box, styled);
        }
        return replaced_contribution_limits(
            box, box->content_width > 0 ? box->content_width : 200);
    }
    if (box->kind == NS_BOX_TEXT)
        return box->content_width > 0 ? box->content_width : 0;
    double contained_w;
    if (style_inline_size_contained(box->style, &contained_w)) return contained_w;
    if (box->style) {
        const ns_css_value *wv = box->style->values[NS_CSS_WIDTH];
        if (wv && (wv->kind == NS_CSS_V_LENGTH || wv->kind == NS_CSS_V_CALC)) {
            double w = value_is_percent(wv) && !box_is_compressible_control(box)
                ? -1 : length_resolve(wv, 0, -1);
            if (w >= 0) {
                if (ns_css_keyword_is(box->style->values[NS_CSS_BOX_SIZING],
                                      "border-box")) {
                    ns_edges m = {0}, pd = {0}, bd = {0};
                    edges_from_style(box->style, 0, &m, &pd, &bd);
                    w -= pd.left + pd.right + bd.left + bd.right;
                    if (w < 0) w = 0;
                }
                const ns_css_value *mxw = box->style->values[NS_CSS_MAX_WIDTH];
                if (mxw && (mxw->kind == NS_CSS_V_LENGTH ||
                            mxw->kind == NS_CSS_V_CALC)) {
                    double m = length_resolve(mxw, 0, -1);
                    if (m >= 0 && w > m) w = m;
                }
                const ns_css_value *mnw = box->style->values[NS_CSS_MIN_WIDTH];
                if (mnw && (mnw->kind == NS_CSS_V_LENGTH ||
                            mnw->kind == NS_CSS_V_CALC)) {
                    double m = length_resolve(mnw, 0, -1);
                    if (m >= 0 && w < m) w = m;
                }
                return width_contribution_keyword_limits(box, w, parent_style,
                                                         FALSE, NS_MEASURED_NONE);
            }
        }
        if (keyword_is(wv, "max-content"))
            return width_contribution_keyword_limits(
                box, measure_max_content_width(box, parent_style),
                parent_style, FALSE, NS_MEASURED_MAX_CONTENT);
        double transferred = ratio_transferred_width(box);
        if (transferred >= 0) {
            if (flex_box_is_border_box(box)) {
                ns_edges m = {0}, pd = {0}, bd = {0};
                edges_from_style(box->style, 0, &m, &pd, &bd);
                transferred -= pd.left + pd.right + bd.left + bd.right;
                if (transferred < 0) transferred = 0;
            }
            return width_contribution_keyword_limits(box, transferred,
                                                     parent_style, FALSE,
                                                     NS_MEASURED_NONE);
        }
    }
    return width_contribution_keyword_limits(
        box, min_content_width_of(box, parent_style), parent_style,
        FALSE, NS_MEASURED_MIN_CONTENT);
}

static double
flex_contribution_clamp_to_basis(ns_box *c, double contribution,
                                 double preferred)
{
    const ns_css_value *bv = c->style->values[NS_CSS_FLEX_BASIS];
    double base = definite_width_limit(c, bv);
    if (base < 0 && (!bv || keyword_is(bv, "auto"))) base = preferred;
    if (base < 0) return contribution;
    if (flex_grow_of(c) <= 0 && contribution > base) return base;
    if (flex_shrink_of(c) <= 0 && contribution < base) return base;
    return contribution;
}

static double
flex_row_item_min_contribution(ns_box *c, const ns_style *child_style)
{
    if (!c->style || c->kind == NS_BOX_INLINE || c->kind == NS_BOX_TEXT)
        return min_width_of(c, child_style);
    const ns_style *s = c->style;
    if (box_is_compressible_control(c) &&
        (value_is_percent(s->values[NS_CSS_WIDTH]) ||
         value_is_percent(s->values[NS_CSS_MAX_WIDTH])))
        return 0;
    double min_content = min_content_width_of(c, child_style);
    double preferred = definite_width_limit(c, s->values[NS_CSS_WIDTH]);
    double contribution = flex_contribution_clamp_to_basis(
        c, MAX(min_content, preferred), preferred);
    double max_main = definite_width_limit(c, s->values[NS_CSS_MAX_WIDTH]);
    if (max_main >= 0 && contribution > max_main) contribution = max_main;
    double min_main = definite_width_limit(c, s->values[NS_CSS_MIN_WIDTH]);
    if (min_main < 0 && !overflow_establishes_bfc(s))
        min_main = preferred >= 0 ? MIN(min_content, preferred) : min_content;
    if (min_main >= 0 && contribution < min_main) contribution = min_main;
    return contribution;
}

static double grid_min_content_width(ns_box *box,
                                     const ns_style *child_style);
static double measure_min_content_width(ns_box *box,
                                        const ns_style *parent_style);

static double
min_content_width_of(ns_box *box, const ns_style *parent_style)
{
    g_min_measure_depth++;
    double w = measure_min_content_width(box, parent_style);
    g_min_measure_depth--;
    return w;
}

static double
measure_min_content_width(ns_box *box, const ns_style *parent_style)
{
    if (box->kind == NS_BOX_INLINE || box->kind == NS_BOX_IMAGE ||
        box->kind == NS_BOX_VIDEO || box->kind == NS_BOX_SVG ||
        box->kind == NS_BOX_TEXT)
        return min_width_of(box, parent_style);
    double contained_w;
    if (box_content_independent_width(box, &contained_w)) return contained_w;
    if (box->kind == NS_BOX_TABLE)
        return table_intrinsic_width(
            box, box->style ? box->style : parent_style, TRUE);
    if (box->style && box_is_grid_container(box) &&
        grid_flows_by_column(box->style)) {
        double gw = grid_column_flow_width(box, box->style, TRUE);
        if (gw >= 0) return gw;
    }
    if (box->style && box_is_grid_container(box)) {
        double gw = grid_min_content_width(box, box->style);
        if (gw >= 0) return gw;
    }
    const ns_style *child_style = box->style ? box->style : parent_style;
    gboolean single_line_row = box_is_flex_container(box) &&
        strncmp(flex_direction_of(box->style), "row", 3) == 0 &&
        !flex_wraps(box->style);
    double max_child = 0;
    double row_sum = 0;
    int items = 0;
    for (ns_box *c = box->first_child; c; c = c->next_sibling) {
        double w = single_line_row
            ? flex_row_item_min_contribution(c, child_style)
            : min_width_of(c, child_style);
        double outer = w;
        if (c->style) {
            ns_edges m = {0}, pd = {0}, bd = {0};
            edges_from_style(c->style, 0, &m, &pd, &bd);
            outer += m.left + m.right + pd.left + pd.right + bd.left + bd.right;
        }
        if (outer > max_child) max_child = outer;
        row_sum += outer;
        items++;
    }
    if (!single_line_row)
        return max_child;
    if (items > 1)
        row_sum += flex_gap_of(box->style, 0) * (items - 1);
    return row_sum;
}

static gboolean
table_width_from_style(const ns_style *s, double basis, double *out)
{
    const ns_css_value *wv = s ? s->values[NS_CSS_WIDTH] : NULL;
    if (!wv || !(wv->kind == NS_CSS_V_LENGTH || wv->kind == NS_CSS_V_CALC))
        return FALSE;
    double w = length_resolve(wv, basis, -1);
    if (w < 0) return FALSE;
    *out = w;
    return TRUE;
}

static void
apply_fixed_table_width(double *col_widths, gboolean *col_fixed,
                        guint max_cols, guint col, int span, double w)
{
    if (span < 1) span = 1;
    if (col >= max_cols) return;
    double per = w / (double)span;
    if (per < 0) per = 0;
    for (int i = 0; i < span && col + (guint)i < max_cols; i++) {
        guint idx = col + (guint)i;
        if (per > col_widths[idx]) col_widths[idx] = per;
        col_fixed[idx] = TRUE;
    }
}

static ns_box *
table_first_row(ns_box *box)
{
    for (ns_box *row = box ? box->first_child : NULL; row; row = row->next_sibling)
        if (row->kind == NS_BOX_TABLE_ROW) return row;
    return NULL;
}

static double
layout_fixed_table_columns(ns_box *box, double cw, guint max_cols,
                           double *col_widths, gboolean *col_fixed)
{
    if (box->table_col_hints) {
        guint col = 0;
        for (guint i = 0; i < box->table_col_hints->len && col < max_cols; i++) {
            ns_table_col_hint *hint =
                &g_array_index(box->table_col_hints, ns_table_col_hint, i);
            double w = 0;
            if (table_width_from_style(hint->style, cw, &w))
                apply_fixed_table_width(col_widths, col_fixed, max_cols,
                                        col, hint->span, w);
            col += hint->span > 0 ? (guint)hint->span : 1;
        }
    }

    ns_box *first_row = table_first_row(box);
    if (first_row) {
        guint col = 0;
        for (ns_box *cell = first_row->first_child; cell; cell = cell->next_sibling) {
            int span = cell->colspan > 0 ? cell->colspan : 1;
            double w = 0;
            if (table_width_from_style(cell->style, cw, &w)) {
                ns_edges m = {0}, pd = {0}, bd = {0};
                edges_from_style(cell->style, cw, &m, &pd, &bd);
                w += m.left + m.right + pd.left + pd.right + bd.left + bd.right;
                apply_fixed_table_width(col_widths, col_fixed, max_cols,
                                        col, span, w);
            }
            col += (guint)span;
        }
    }

    double used = 0;
    guint unset = 0;
    for (guint i = 0; i < max_cols; i++) {
        used += col_widths[i];
        if (!col_fixed[i]) unset++;
    }

    double remaining = cw - used;
    if (remaining < 0) remaining = 0;
    if (unset > 0) {
        double per = remaining / (double)unset;
        for (guint i = 0; i < max_cols; i++)
            if (!col_fixed[i]) col_widths[i] = per;
    } else if (remaining > 0 && max_cols > 0) {
        double per = remaining / (double)max_cols;
        for (guint i = 0; i < max_cols; i++)
            col_widths[i] += per;
    }

    double sum = 0;
    for (guint i = 0; i < max_cols; i++) sum += col_widths[i];
    return sum;
}

static gboolean
table_caption_bottom(const ns_box *caption)
{
    const char *side = caption && caption->style
        ? ns_style_keyword(caption->style, NS_CSS_CAPTION_SIDE) : NULL;
    return side && (strcmp(side, "bottom") == 0 ||
                    strcmp(side, "block-end") == 0);
}

static double
table_child_outer_height(const ns_box *box)
{
    return box->content_height
         + box->margin.top + box->margin.bottom
         + box->padding.top + box->padding.bottom
         + box->border.top + box->border.bottom;
}

static double
table_border_box_width(const ns_box *box)
{
    return box->content_width + box->padding.left + box->padding.right +
           box->border.left + box->border.right;
}

static double
layout_table_captions(ns_box *box, gboolean bottom,
                      const ns_style *child_inherited, double y)
{
    double width = table_border_box_width(box);
    for (ns_box *caption = box->first_child; caption; caption = caption->next_sibling) {
        if (caption->kind != NS_BOX_TABLE_CAPTION) continue;
        if (table_caption_bottom(caption) != bottom) continue;
        caption->x = box->x + box->margin.left;
        caption->y = y;
        layout_box(caption, width, child_inherited);
        y += table_child_outer_height(caption);
    }
    return y;
}

void
ns_box_table_caption_extents(const ns_box *table, double *above, double *below)
{
    *above = 0;
    *below = 0;
    if (!table || table->kind != NS_BOX_TABLE) return;
    for (const ns_box *c = table->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_BOX_TABLE_CAPTION) continue;
        if (table_caption_bottom(c)) *below += table_child_outer_height(c);
        else                         *above += table_child_outer_height(c);
    }
}

static void
table_resolve_auto_margins(ns_box *box, double parent_content_width)
{
    gboolean ml_auto = length_is_auto(box->style ? box->style->values[NS_CSS_MARGIN_LEFT]  : NULL);
    gboolean mr_auto = length_is_auto(box->style ? box->style->values[NS_CSS_MARGIN_RIGHT] : NULL);
    if (!ml_auto && !mr_auto) return;
    double available = parent_content_width - table_border_box_width(box);
    if (available < 0) available = 0;
    if (ml_auto && mr_auto) {
        box->margin.left  = available / 2.0;
        box->margin.right = available / 2.0;
    } else if (ml_auto) {
        box->margin.left  = available - box->margin.right;
        if (box->margin.left < 0) box->margin.left = 0;
    } else {
        box->margin.right = available - box->margin.left;
        if (box->margin.right < 0) box->margin.right = 0;
    }
}

static void
table_grow_columns(double *col_widths, const gboolean *col_fixed,
                   guint max_cols, double extra)
{
    if (extra <= 0 || max_cols == 0) return;
    double elastic_natural = 0, natural_sum = 0;
    guint elastic_count = 0;
    for (guint i = 0; i < max_cols; i++) {
        natural_sum += col_widths[i];
        if (!col_fixed[i]) {
            elastic_natural += col_widths[i];
            elastic_count++;
        }
    }
    if (elastic_natural > 0) {
        for (guint i = 0; i < max_cols; i++)
            if (!col_fixed[i])
                col_widths[i] += extra * col_widths[i] / elastic_natural;
    } else if (elastic_count > 0) {
        double per = extra / (double)elastic_count;
        for (guint i = 0; i < max_cols; i++)
            if (!col_fixed[i]) col_widths[i] += per;
    } else if (natural_sum > 0) {
        for (guint i = 0; i < max_cols; i++)
            col_widths[i] += extra * col_widths[i] / natural_sum;
    } else {
        double per = extra / (double)max_cols;
        for (guint i = 0; i < max_cols; i++) col_widths[i] += per;
    }
}

static void
table_border_spacing(const ns_style *s, double *hsp, double *vsp)
{
    *hsp = 0;
    *vsp = 0;
    if (!s) return;
    const ns_css_value *bc = s->values[NS_CSS_BORDER_COLLAPSE];
    if (bc && bc->kind == NS_CSS_V_KEYWORD && bc->u.keyword &&
        g_ascii_strcasecmp(bc->u.keyword, "collapse") == 0)
        return;
    const ns_css_value *bs = s->values[NS_CSS_BORDER_SPACING];
    if (bs && bs->kind == NS_CSS_V_SIZE) {
        *hsp = bs->u.size.w;
        *vsp = bs->u.size.h;
    }
}

static gboolean
table_is_collapse(const ns_style *s)
{
    const ns_css_value *bc = s ? s->values[NS_CSS_BORDER_COLLAPSE] : NULL;
    return bc && bc->kind == NS_CSS_V_KEYWORD && bc->u.keyword &&
           g_ascii_strcasecmp(bc->u.keyword, "collapse") == 0;
}

typedef struct ns_cell_pos {
    ns_box *cell;
    guint   r0, c0, cov, rsp;
} ns_cell_pos;

static void
table_collapse_borders(ns_box *box, guint max_cols)
{
    if (max_cols == 0) return;
    guint R = 0;
    for (ns_box *row = box->first_child; row; row = row->next_sibling)
        if (row->kind == NS_BOX_TABLE_ROW) R++;
    if (R == 0) return;
    if ((gsize)R > G_MAXSIZE / sizeof(ns_box *) / max_cols) return;

    ns_box **grid = g_new0(ns_box *, (gsize)R * max_cols);
    GArray *cells = g_array_new(FALSE, FALSE, sizeof(ns_cell_pos));
    guint r = 0;
    for (ns_box *row = box->first_child; row; row = row->next_sibling) {
        if (row->kind != NS_BOX_TABLE_ROW) continue;
        guint col = 0;
        for (ns_box *cell = row->first_child; cell; cell = cell->next_sibling) {
            while (col < max_cols && grid[r * max_cols + col]) col++;
            if (col >= max_cols) break;
            guint cov = cell->colspan > 0 ? (guint)cell->colspan : 1;
            guint rsp = cell->rowspan > 0 ? (guint)cell->rowspan : 1;
            for (guint dr = 0; dr < rsp && r + dr < R; dr++)
                for (guint dc = 0; dc < cov && col + dc < max_cols; dc++)
                    grid[(r + dr) * max_cols + (col + dc)] = cell;
            ns_cell_pos cp = { cell, r, col, cov, rsp };
            g_array_append_val(cells, cp);
            col += cov;
        }
        r++;
    }

    for (guint i = 0; i < cells->len; i++) {
        ns_cell_pos *cp = &g_array_index(cells, ns_cell_pos, i);
        gboolean right_nb = FALSE, below_nb = FALSE;
        guint cr = cp->c0 + cp->cov;
        if (cr < max_cols)
            for (guint rr = cp->r0; rr < cp->r0 + cp->rsp && rr < R; rr++)
                if (grid[rr * max_cols + cr]) { right_nb = TRUE; break; }
        guint br = cp->r0 + cp->rsp;
        if (br < R)
            for (guint cc = cp->c0; cc < cp->c0 + cp->cov && cc < max_cols; cc++)
                if (grid[br * max_cols + cc]) { below_nb = TRUE; break; }
        if (right_nb) cp->cell->border.right = 0;
        if (below_nb) cp->cell->border.bottom = 0;
    }

    g_array_free(cells, TRUE);
    g_free(grid);
}

static double
table_initial_content_width(ns_box *box, gboolean explicit_width,
                            double cb_basis, double available,
                            double sizing_extras)
{
    if (box->has_flex_main) {
        box->has_flex_main = FALSE;
        return box->flex_main_size;
    }
    if (explicit_width)
        return length_resolve(box->style->values[NS_CSS_WIDTH], cb_basis, 0) -
               sizing_extras;
    return available;
}

static void
layout_table(ns_box *box, double parent_content_width, const ns_style *inherited_style)
{
    double cb_basis = box_cb_basis(box, parent_content_width);
    edges_from_style(box->style, cb_basis,
                     &box->margin, &box->padding, &box->border);
    double horiz_total = box->margin.left + box->margin.right +
                         box->padding.left + box->padding.right +
                         box->border.left + box->border.right;
    const ns_css_value *wv = box->style ? box->style->values[NS_CSS_WIDTH] : NULL;
    gboolean explicit_width = wv &&
        (wv->kind == NS_CSS_V_LENGTH || wv->kind == NS_CSS_V_CALC);
    double sizing_extras = flex_box_is_border_box(box)
        ? box->padding.left + box->padding.right +
          box->border.left + box->border.right
        : 0;
    double cw = table_initial_content_width(box, explicit_width, cb_basis,
                                            parent_content_width - horiz_total,
                                            sizing_extras);
    const ns_css_value *mxw = box->style ? box->style->values[NS_CSS_MAX_WIDTH] : NULL;
    if (mxw && (mxw->kind == NS_CSS_V_LENGTH || mxw->kind == NS_CSS_V_CALC)) {
        double m = length_resolve(mxw, cb_basis, -1);
        if (m >= 0 && cw > m - sizing_extras) cw = m - sizing_extras;
    }
    double min_cw = 0;
    const ns_css_value *mnw = box->style ? box->style->values[NS_CSS_MIN_WIDTH] : NULL;
    if (mnw && (mnw->kind == NS_CSS_V_LENGTH || mnw->kind == NS_CSS_V_CALC)) {
        double m = length_resolve(mnw, cb_basis, -1);
        if (m >= 0) min_cw = m - sizing_extras;
        if (cw < min_cw) cw = min_cw;
    }
    if (cw < 0) cw = 0;
    box->content_width = cw;

    const ns_style *child_inherited = box->style ? box->style : inherited_style;
    double caption_cw = table_caption_min_content_width(box, child_inherited);
    double top_edge = box->y + box->margin.top;

    guint max_cols = table_column_count(box);
    if (max_cols == 0) {
        if (!explicit_width) cw = MAX(caption_cw, min_cw);
        else if (cw < caption_cw) cw = caption_cw;
        box->content_width = cw;
        table_resolve_auto_margins(box, parent_content_width);
        double grid_top = layout_table_captions(box, FALSE, child_inherited,
                                                top_edge);
        double grid_bottom = grid_top + box->border.top + box->padding.top +
                             box->padding.bottom + box->border.bottom;
        double bottom = layout_table_captions(box, TRUE, child_inherited,
                                              grid_bottom);
        box->content_height = bottom - top_edge -
            box->border.top - box->padding.top -
            box->padding.bottom - box->border.bottom;
        return;
    }

    double hsp = 0, vsp = 0;
    table_border_spacing(box->style, &hsp, &vsp);
    double total_hsp = (double)(max_cols + 1) * hsp;
    double col_avail = cw - total_hsp;
    if (col_avail < 0) col_avail = 0;

    const ns_style *measure_inherited = box->style ? box->style : inherited_style;
    double *col_widths = g_new0(double, max_cols);
    double *col_min = g_new0(double, max_cols);
    gboolean *col_fixed = g_new0(gboolean, max_cols);
    double *col_explicit = g_new(double, max_cols);
    for (guint i = 0; i < max_cols; i++) col_explicit[i] = -1;
    gboolean has_explicit_cols = FALSE;
    gboolean fixed_layout = explicit_width &&
        keyword_is(box->style ? box->style->values[NS_CSS_TABLE_LAYOUT] : NULL, "fixed");
    if (fixed_layout) {
        double fixed_sum = layout_fixed_table_columns(box, col_avail, max_cols,
                                                      col_widths, col_fixed);
        if (fixed_sum > col_avail) {
            col_avail = fixed_sum;
            cw = col_avail + total_hsp;
            box->content_width = cw;
        }
    } else {
        for (ns_box *row = box->first_child; row; row = row->next_sibling) {
            if (row->kind != NS_BOX_TABLE_ROW) continue;
            guint col = 0;
            for (ns_box *cell = row->first_child; cell; cell = cell->next_sibling) {
                int span = cell->colspan > 0 ? cell->colspan : 1;
                const ns_style *cs = cell->style ? cell->style : measure_inherited;
                double natural = measure_natural_width(cell, cs);
                edges_from_style(cell->style, cw > 0 ? cw : 1000.0,
                                 &cell->margin, &cell->padding, &cell->border);
                double h_extra = cell->padding.left + cell->padding.right
                    + cell->border.left + cell->border.right
                    + cell->margin.left + cell->margin.right;
                double cell_outer =
                    table_cell_clamp(cell, h_extra, natural + h_extra);
                gboolean cell_fixed = FALSE;
                double cell_explicit = -1;
                if (cell->style && cell->style->values[NS_CSS_WIDTH]) {
                    const ns_css_value *cwv = cell->style->values[NS_CSS_WIDTH];
                    if (cwv->kind == NS_CSS_V_LENGTH || cwv->kind == NS_CSS_V_CALC) {
                        cell_fixed = TRUE;
                        double w = length_resolve(cwv, col_avail > 0 ? col_avail : 0, -1);
                        if (w >= 0)
                            cell_explicit =
                                table_cell_clamp(cell, h_extra, w + h_extra);
                    }
                }
                double per_col = cell_outer / (double)span;
                for (int i = 0; i < span && col + (guint)i < max_cols; i++) {
                    if (per_col > col_widths[col + i])
                        col_widths[col + i] = per_col;
                    if (cell_fixed && span == 1) {
                        col_fixed[col + i] = TRUE;
                        has_explicit_cols = TRUE;
                    }
                    if (cell_explicit >= 0 && span == 1 &&
                        cell_explicit > col_explicit[col + i]) {
                        col_explicit[col + i] = cell_explicit;
                        has_explicit_cols = TRUE;
                    }
                }
                col += (guint)span;
            }
        }
        if (box->table_col_hints) {
            guint col = 0;
            for (guint i = 0; i < box->table_col_hints->len && col < max_cols; i++) {
                ns_table_col_hint *hint =
                    &g_array_index(box->table_col_hints, ns_table_col_hint, i);
                int hspan = hint->span > 0 ? hint->span : 1;
                double w = 0;
                if (table_width_from_style(hint->style, col_avail, &w)) {
                    has_explicit_cols = TRUE;
                    double per = w / (double)hspan;
                    for (int k = 0; k < hspan && col + (guint)k < max_cols; k++)
                        if (per > col_explicit[col + k]) col_explicit[col + k] = per;
                }
                col += (guint)hspan;
            }
        }
        double natural_sum_pre = 0;
        for (guint i = 0; i < max_cols; i++) natural_sum_pre += col_widths[i];
        if (has_explicit_cols || natural_sum_pre > col_avail) {
            for (ns_box *row = box->first_child; row; row = row->next_sibling) {
                if (row->kind != NS_BOX_TABLE_ROW) continue;
                guint col = 0;
                for (ns_box *cell = row->first_child; cell; cell = cell->next_sibling) {
                    int span = cell->colspan > 0 ? cell->colspan : 1;
                    const ns_style *cs = cell->style ? cell->style : measure_inherited;
                    ns_edges m = {0}, pd = {0}, bd = {0};
                    edges_from_style(cell->style, cw > 0 ? cw : 1000.0,
                                     &m, &pd, &bd);
                    double h_extra = pd.left + pd.right + bd.left + bd.right +
                                     m.left + m.right;
                    double per_col_min =
                        table_cell_clamp(cell, h_extra,
                                         min_content_width_of(cell, cs)
                                         + h_extra) / (double)span;
                    for (int i = 0; i < span && col + (guint)i < max_cols; i++)
                        if (per_col_min > col_min[col + i])
                            col_min[col + i] = per_col_min;
                    col += (guint)span;
                }
            }
        }
        for (guint i = 0; i < max_cols; i++) {
            if (col_explicit[i] >= 0) {
                double e = col_explicit[i];
                if (e < col_min[i]) e = col_min[i];
                col_widths[i] = e;
                col_fixed[i] = TRUE;
            }
        }
        double natural_sum = 0;
        for (guint i = 0; i < max_cols; i++) natural_sum += col_widths[i];
        if (natural_sum > col_avail) {
            double min_sum = 0;
            for (guint i = 0; i < max_cols; i++) min_sum += col_min[i];
            if (min_sum >= col_avail) {
                if (min_sum > 0) {
                    for (guint i = 0; i < max_cols; i++)
                        col_widths[i] = col_min[i];
                    col_avail = min_sum;
                    cw = col_avail + total_hsp;
                    box->content_width = cw;
                } else {
                    double evenly = col_avail / (double)max_cols;
                    for (guint i = 0; i < max_cols; i++) col_widths[i] = evenly;
                }
            } else {
                double slack_sum = natural_sum - min_sum;
                double avail_extra = col_avail - min_sum;
                for (guint i = 0; i < max_cols; i++) {
                    double slack = col_widths[i] - col_min[i];
                    col_widths[i] = col_min[i] +
                        (slack_sum > 0 ? slack * (avail_extra / slack_sum) : 0);
                }
            }
        } else if (natural_sum == 0) {
            double evenly = col_avail / (double)max_cols;
            for (guint i = 0; i < max_cols; i++) col_widths[i] = evenly;
        } else if (explicit_width) {
            table_grow_columns(col_widths, col_fixed, max_cols,
                               col_avail - natural_sum);
        } else {
            cw = natural_sum + total_hsp;
            box->content_width = cw;
        }
    }
    if (cw < caption_cw) {
        table_grow_columns(col_widths, col_fixed, max_cols, caption_cw - cw);
        cw = caption_cw;
        box->content_width = cw;
    }
    g_free(col_explicit);
    g_free(col_fixed);
    g_free(col_min);
    double *col_x = g_new0(double, max_cols);
    {
        double cx = hsp;
        for (guint i = 0; i < max_cols; i++) {
            col_x[i] = cx;
            cx += col_widths[i] + hsp;
        }
    }

    table_resolve_auto_margins(box, parent_content_width);

    double inner_x = box->x + box->margin.left + box->border.left + box->padding.left;
    double grid_top = layout_table_captions(box, FALSE, child_inherited, top_edge);
    double inner_y = grid_top + box->border.top + box->padding.top;
    double cursor_y = inner_y + vsp;

    int *rs_remain = g_new0(int, max_cols);
    ns_box **rs_cell = g_new0(ns_box *, max_cols);
    for (ns_box *row = box->first_child; row; row = row->next_sibling) {
        if (row->kind != NS_BOX_TABLE_ROW) continue;
        row->x = inner_x + hsp;
        row->y = cursor_y;
        row->content_width = cw > 2 * hsp ? cw - 2 * hsp : 0;
        double row_height = 0;
        guint col = 0;
        for (ns_box *cell = row->first_child; cell; cell = cell->next_sibling) {
            while (col < max_cols && rs_remain[col] > 0) col++;
            int span = cell->colspan > 0 ? cell->colspan : 1;
            int rspan = cell->rowspan > 0 ? cell->rowspan : 1;
            double cell_outer_w = 0;
            int covered = 0;
            for (int i = 0; i < span && col + (guint)i < max_cols; i++) {
                cell_outer_w += col_widths[col + i];
                covered++;
            }
            if (covered > 1) cell_outer_w += hsp * (double)(covered - 1);
            if (rspan > 1) {
                for (int i = 0; i < span && col + (guint)i < max_cols; i++) {
                    rs_remain[col + i] = rspan;
                    rs_cell[col + i] = cell;
                }
            }
            cell->x = inner_x + (col < max_cols ? col_x[col] : 0);
            cell->y = cursor_y;
            edges_from_style(cell->style, cell_outer_w,
                             &cell->margin, &cell->padding, &cell->border);
            double cell_inner_w = cell_outer_w
                - cell->padding.left - cell->padding.right
                - cell->border.left - cell->border.right
                - cell->margin.left - cell->margin.right;
            if (cell_inner_w < 0) cell_inner_w = 0;
            cell->content_width = cell_inner_w;
            layout_block(cell, cell_outer_w, child_inherited);
            cell_inner_w = cell->content_width;
            double cell_h = cell->measured_content_height;
            const ns_css_value *cell_hv = cell->style
                ? cell->style->values[NS_CSS_HEIGHT] : NULL;
            const ns_css_value *cell_mnh = cell->style
                ? cell->style->values[NS_CSS_MIN_HEIGHT] : NULL;
            if (cell_hv &&
                (cell_hv->kind == NS_CSS_V_LENGTH || cell_hv->kind == NS_CSS_V_CALC)) {
                double eh = resolve_used_height(cell, cell_hv, cell_inner_w, -1);
                if (eh > cell_h) cell_h = eh;
            }
            if (cell_mnh &&
                (cell_mnh->kind == NS_CSS_V_LENGTH || cell_mnh->kind == NS_CSS_V_CALC)) {
                double mh = resolve_used_height(cell, cell_mnh, cell_inner_w, -1);
                if (mh > cell_h) cell_h = mh;
            }
            cell->content_height = cell_h;
            double cell_outer_h = cell_h
                + cell->margin.top + cell->margin.bottom
                + cell->padding.top + cell->padding.bottom
                + cell->border.top + cell->border.bottom;
            if (rspan <= 1 && cell_outer_h > row_height) row_height = cell_outer_h;
            col += (guint)span;
        }
        const ns_css_value *rhv = row->style ? row->style->values[NS_CSS_HEIGHT] : NULL;
        if (rhv && (rhv->kind == NS_CSS_V_LENGTH || rhv->kind == NS_CSS_V_CALC)) {
            double rh = length_resolve(rhv, 0, -1);
            if (rh > row_height) row_height = rh;
        }
        GHashTable *ending_rowspans = NULL;
        for (guint i = 0; i < max_cols; i++) {
            if (rs_remain[i] != 1 || !rs_cell[i]) continue;
            if (!ending_rowspans)
                ending_rowspans = g_hash_table_new(g_direct_hash,
                                                   g_direct_equal);
            ns_box *rc = rs_cell[i];
            if (g_hash_table_contains(ending_rowspans, rc)) continue;
            g_hash_table_add(ending_rowspans, rc);
            double span_bottom = cursor_y + row_height;
            double avail = span_bottom - rc->y
                         - rc->margin.top - rc->margin.bottom
                         - rc->padding.top - rc->padding.bottom
                         - rc->border.top - rc->border.bottom;
            if (rc->content_height > avail)
                row_height += rc->content_height - avail;
        }
        if (ending_rowspans) g_hash_table_destroy(ending_rowspans);
        for (ns_box *cell = row->first_child; cell; cell = cell->next_sibling) {
            if ((cell->rowspan > 0 ? cell->rowspan : 1) > 1) continue;
            double avail = row_height
                         - cell->margin.top - cell->margin.bottom
                         - cell->padding.top - cell->padding.bottom
                         - cell->border.top - cell->border.bottom;
            double extra = avail - cell->measured_content_height;
            if (extra > 0) {
                double factor = 0;
                const ns_css_value *va = cell->style
                    ? cell->style->values[NS_CSS_VERTICAL_ALIGN] : NULL;
                if (va && va->kind == NS_CSS_V_KEYWORD && va->u.keyword) {
                    if (g_ascii_strcasecmp(va->u.keyword, "middle") == 0)
                        factor = 0.5;
                    else if (g_ascii_strcasecmp(va->u.keyword, "bottom") == 0)
                        factor = 1.0;
                }
                if (factor > 0)
                    for (ns_box *ch = cell->first_child; ch; ch = ch->next_sibling)
                        shift_box_tree(ch, 0, extra * factor);
            }
            if (avail > cell->content_height) cell->content_height = avail;
        }
        row->content_height = row_height;
        cursor_y += row_height + vsp;
        for (guint i = 0; i < max_cols; i++) {
            if (rs_remain[i] > 0 && --rs_remain[i] == 0 && rs_cell[i]) {
                ns_box *rc = rs_cell[i];
                double h = cursor_y - vsp - rc->y
                    - rc->margin.top - rc->margin.bottom
                    - rc->padding.top - rc->padding.bottom
                    - rc->border.top - rc->border.bottom;
                if (h > rc->content_height) rc->content_height = h;
                rs_cell[i] = NULL;
            }
        }
    }
    if (table_is_collapse(box->style))
        table_collapse_borders(box, max_cols);
    g_free(rs_remain);
    g_free(rs_cell);
    g_free(col_widths);
    g_free(col_x);
    double grid_height = cursor_y - inner_y;

    const ns_css_value *thv = box->style ? box->style->values[NS_CSS_HEIGHT] : NULL;
    if (thv && (thv->kind == NS_CSS_V_LENGTH || thv->kind == NS_CSS_V_CALC)) {
        double target = resolve_used_height(box, thv, cw, -1);
        if (box->style && box->style->values[NS_CSS_BOX_SIZING] &&
            ns_css_keyword_is(box->style->values[NS_CSS_BOX_SIZING], "border-box"))
            target -= box->padding.top + box->padding.bottom +
                      box->border.top + box->border.bottom;
        int nrows = 0;
        for (ns_box *row = box->first_child; row; row = row->next_sibling)
            if (row->kind == NS_BOX_TABLE_ROW) nrows++;
        if (nrows > 0 && target > grid_height + 0.5) {
            double per = (target - grid_height) / nrows;
            double shift = 0;
            for (ns_box *row = box->first_child; row; row = row->next_sibling) {
                if (row->kind != NS_BOX_TABLE_ROW) continue;
                if (shift > 0) translate_subtree(row, 0, shift);
                row->content_height += per;
                for (ns_box *cell = row->first_child; cell;
                     cell = cell->next_sibling) {
                    if (cell->kind != NS_BOX_TABLE_CELL) continue;
                    double factor = 0;
                    const ns_css_value *va = cell->style
                        ? cell->style->values[NS_CSS_VERTICAL_ALIGN] : NULL;
                    if (va && va->kind == NS_CSS_V_KEYWORD && va->u.keyword) {
                        if (g_ascii_strcasecmp(va->u.keyword, "middle") == 0)
                            factor = 0.5;
                        else if (g_ascii_strcasecmp(va->u.keyword, "bottom") == 0)
                            factor = 1.0;
                    }
                    if (factor > 0)
                        for (ns_box *ch = cell->first_child; ch;
                             ch = ch->next_sibling)
                            shift_box_tree(ch, 0, per * factor);
                    cell->content_height += per;
                }
                shift += per;
            }
            grid_height = target;
        }
    }
    double grid_bottom = inner_y + grid_height +
                         box->padding.bottom + box->border.bottom;
    double bottom = layout_table_captions(box, TRUE, child_inherited, grid_bottom);
    box->content_height = bottom - top_edge -
        box->border.top - box->padding.top -
        box->padding.bottom - box->border.bottom;
}

static __thread gboolean g_cq_seen_container;

static gboolean
box_is_query_container(const ns_box *b)
{
    if (!b || !b->style) return FALSE;
    const ns_css_value *ct = b->style->values[NS_CSS_CONTAINER_TYPE];
    return ct && ct->kind == NS_CSS_V_KEYWORD && ct->u.keyword &&
           g_ascii_strcasecmp(ct->u.keyword, "normal") != 0;
}

static gboolean
box_is_size_query_container(const ns_box *b)
{
    if (!b || !b->style) return FALSE;
    const ns_css_value *ct = b->style->values[NS_CSS_CONTAINER_TYPE];
    return ct && ct->kind == NS_CSS_V_KEYWORD && ct->u.keyword &&
           g_ascii_strcasecmp(ct->u.keyword, "size") == 0;
}

static void
cq_set_dims_from_ancestors(const ns_box *box)
{
    double inline_size = 0;
    double block_size = 0;
    for (const ns_box *a = box->parent; a; a = a->parent) {
        if (inline_size <= 0 && box_is_query_container(a))
            inline_size = a->content_width;
        if (block_size <= 0 && box_is_size_query_container(a))
            block_size = a->content_height;
        if (inline_size > 0 && block_size > 0) break;
    }
    ns_css_set_container_dims(inline_size, block_size);
}

static void
inline_balance(ns_box *box, double width, const ns_style *style)
{
    if (!style || box->phantom_line || box->vertical_wm || width <= 0 ||
        !keyword_is(style->values[NS_CSS_TEXT_WRAP_STYLE], "balance") ||
        keyword_is(style->values[NS_CSS_WHITE_SPACE], "nowrap") ||
        keyword_is(style->values[NS_CSS_WHITE_SPACE], "pre"))
        return;
    double height = box->content_height;
    double line = inline_line_height(style);
    if (line <= 0 || height < line * 1.5 || height > line * 6.5) return;
    double lo = width * 0.4, hi = width;
    for (int i = 0; i < 10 && hi - lo > 0.5; i++) {
        double mid = (lo + hi) / 2.0;
        inline_layout(box, mid, style);
        if (box->content_height <= height + 0.5) hi = mid;
        else lo = mid;
    }
    inline_layout(box, hi, style);
    if (box->content_height > height + 0.5) {
        inline_layout(box, width, style);
        return;
    }
    const char *align = ns_style_keyword(style, NS_CSS_TEXT_ALIGN);
    gboolean rtl = keyword_is(style->values[NS_CSS_DIRECTION], "rtl");
    double slack = width - hi;
    double dx = 0;
    if (align && (strcmp(align, "center") == 0 ||
                  strcmp(align, "-webkit-center") == 0))
        dx = slack / 2.0;
    else if (align && (strcmp(align, "right") == 0 ||
                       strcmp(align, "-webkit-right") == 0 ||
                       (strcmp(align, "end") == 0 && !rtl) ||
                       (strcmp(align, "start") == 0 && rtl)))
        dx = slack;
    if (dx > 0) shift_box_tree(box, dx, 0);
}

static void
layout_box(ns_box *box, double parent_content_width, const ns_style *inherited_style)
{
    box->definite_height_read = FALSE;
    box->last_layout_width = parent_content_width;
    if (box_is_query_container(box)) g_cq_seen_container = TRUE;
    if (g_cq_seen_container) cq_set_dims_from_ancestors(box);
    if (box->kind == NS_BOX_BLOCK) {
        layout_block(box, parent_content_width, inherited_style);
    } else if (box->kind == NS_BOX_INLINE) {
        inline_layout(box, parent_content_width, inherited_style);
        inline_balance(box, parent_content_width, inherited_style);
    } else if (box->kind == NS_BOX_IMAGE) {
        layout_image(box, parent_content_width);
    } else if (box->kind == NS_BOX_VIDEO) {
        layout_image(box, parent_content_width);
    } else if (box->kind == NS_BOX_SVG) {
        layout_image(box, parent_content_width);
    } else if (box->kind == NS_BOX_TABLE) {
        layout_table(box, parent_content_width, inherited_style);
    } else if (box->kind == NS_BOX_TABLE_CAPTION) {
        layout_block(box, parent_content_width, inherited_style);
    } else if (box->kind == NS_BOX_MATH) {
        double fpx = length_or(box->style
                               ? box->style->values[NS_CSS_FONT_SIZE] : NULL, 16);
        double w = 0, asc = 0, desc = 0;
        ns_math_measure(box->dom, fpx, &w, &asc, &desc);
        box->content_width = w;
        box->content_height = asc + desc;
    } else {
        box->content_width = parent_content_width;
        box->content_height = 0;
    }
}

static gboolean
flex_box_is_border_box(const ns_box *c)
{
    const ns_css_value *bsv = c && c->style
        ? c->style->values[NS_CSS_BOX_SIZING] : NULL;
    return bsv && bsv->kind == NS_CSS_V_KEYWORD && bsv->u.keyword &&
           strcmp(bsv->u.keyword, "border-box") == 0;
}

static double
flex_main_axis_extras(const ns_box *c)
{
    if (!c) return 0;
    return c->padding.left + c->padding.right + c->border.left + c->border.right;
}

static double
flex_border_box_to_content(const ns_box *c, double v)
{
    if (flex_box_is_border_box(c)) {
        v -= flex_main_axis_extras(c);
        if (v < 0) v = 0;
    }
    return v;
}

static double
flex_item_keyword_width(ns_box *c, const ns_css_value *v, double cw,
                        const ns_style *inherited)
{
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return -1;
    double inner = cw - c->margin.left - c->margin.right
                 - c->padding.left - c->padding.right
                 - c->border.left - c->border.right;
    return intrinsic_keyword_width(c, v->u.keyword,
                                   inherited ? inherited : c->style, inner);
}

static gboolean
flex_main_basis_explicit(ns_box *c, double cw, const ns_style *inherited,
                         double *out)
{
    const ns_style *s = c->style;
    if (!s) return FALSE;
    const ns_css_value *b = s->values[NS_CSS_FLEX_BASIS];
    if (b && (b->kind == NS_CSS_V_LENGTH || b->kind == NS_CSS_V_CALC)) {
        *out = flex_border_box_to_content(c, length_resolve(b, cw, 0));
        return TRUE;
    }
    double keyword_basis = flex_item_keyword_width(c, b, cw, inherited);
    if (keyword_basis >= 0) {
        *out = keyword_basis;
        return TRUE;
    }
    const ns_css_value *w = s->values[NS_CSS_WIDTH];
    if (w && (w->kind == NS_CSS_V_LENGTH || w->kind == NS_CSS_V_CALC)) {
        *out = flex_border_box_to_content(c, length_resolve(w, cw, 0));
        return TRUE;
    }
    double keyword_width = flex_item_keyword_width(c, w, cw, inherited);
    if (keyword_width >= 0) {
        *out = keyword_width;
        return TRUE;
    }
    return FALSE;
}

static double
estimate_natural_width(const ns_box *b, double cap)
{
    double font_size = 16;
    if (b->style && b->style->values[NS_CSS_FONT_SIZE]) {
        const ns_css_value *fs = b->style->values[NS_CSS_FONT_SIZE];
        if (fs->kind == NS_CSS_V_LENGTH && fs->u.length.unit == NS_CSS_UNIT_PX)
            font_size = fs->u.length.v;
    } else {
        for (const ns_box *p = b->parent; p; p = p->parent) {
            if (p->style && p->style->values[NS_CSS_FONT_SIZE]) {
                const ns_css_value *fs = p->style->values[NS_CSS_FONT_SIZE];
                if (fs->kind == NS_CSS_V_LENGTH &&
                    fs->u.length.unit == NS_CSS_UNIT_PX) {
                    font_size = fs->u.length.v;
                    break;
                }
            }
        }
    }
    const ns_css_value *swv = b->style ? b->style->values[NS_CSS_WIDTH] : NULL;
    if (swv && swv->kind == NS_CSS_V_LENGTH && swv->u.length.v > 0) {
        double sw = -1;
        switch (swv->u.length.unit) {
        case NS_CSS_UNIT_PX:
        case NS_CSS_UNIT_NUMBER:
            sw = swv->u.length.v;
            break;
        case NS_CSS_UNIT_EM:
            sw = swv->u.length.v * font_size;
            break;
        case NS_CSS_UNIT_REM:
            sw = swv->u.length.v * 16.0;
            break;
        case NS_CSS_UNIT_VW:
            sw = swv->u.length.v * ns_css_viewport_w() / 100.0;
            break;
        default:
            break;
        }
        if (sw > 0) {
            const ns_css_value *bsv = b->style->values[NS_CSS_BOX_SIZING];
            gboolean border_box = bsv && bsv->kind == NS_CSS_V_KEYWORD &&
                                  bsv->u.keyword &&
                                  strcmp(bsv->u.keyword, "border-box") == 0;
            if (!border_box)
                sw += b->padding.left + b->padding.right +
                      b->border.left  + b->border.right;
            return sw > cap ? cap : sw;
        }
    }
    double contained_w;
    if (style_inline_size_contained(b->style, &contained_w)) return contained_w;
    double w = 0;
    if (b->kind == NS_BOX_INLINE && b->text) {
        double chars = 0;
        for (const char *p = b->text; *p; p = g_utf8_next_char(p))
            if (g_utf8_get_char(p) != 0xFFFC) chars += 1;
        w = chars * font_size * 0.65 + font_size * 0.5;
        if (b->inline_atomics)
            for (guint i = 0; i < b->inline_atomics->len; i++) {
                const ns_box *ab =
                    g_array_index(b->inline_atomics, ns_inline_atomic, i).box;
                if (!ab) continue;
                double aw = ab->content_width > 0
                    ? ab->content_width +
                      ab->padding.left + ab->padding.right +
                      ab->border.left  + ab->border.right
                    : estimate_natural_width(ab, cap);
                w += aw + ab->margin.left + ab->margin.right;
            }
    } else if (b->kind == NS_BOX_IMAGE || b->kind == NS_BOX_VIDEO ||
               b->kind == NS_BOX_SVG) {
        w = b->content_width > 0 ? b->content_width : 0;
    } else {
        int flow_children = 0;
        gboolean column_flex = b->style &&
            box_is_flex_container(b) &&
            (strcmp(flex_direction_of(b->style),
                    "column") == 0 ||
             strcmp(flex_direction_of(b->style),
                    "column-reverse") == 0);
        for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
            if (c->style && c->style != b->style &&
                style_is_absolute_or_fixed(c->style)) continue;
            double cw_child = estimate_natural_width(c, cap);
            if (c->style &&
                (c->style->values[NS_CSS_MARGIN_LEFT] ||
                 c->style->values[NS_CSS_MARGIN_RIGHT])) {
                ns_edges m = {0}, pd = {0}, bd = {0};
                edges_from_style(c->style, 0, &m, &pd, &bd);
                cw_child += m.left + m.right;
            }
            if (column_flex) {
                if (cw_child > w) w = cw_child;
            } else {
                w += cw_child;
            }
            flow_children++;
        }
        if (flow_children > 1 && box_is_flex_container(b) &&
            strcmp(flex_direction_of(b->style),
                   "column") != 0 &&
            strcmp(flex_direction_of(b->style),
                   "column-reverse") != 0) {
            w += (flow_children - 1) * flex_gap_of(b->style, 0);
        }
    }
    if (b->style) {
        if (b->style->values[NS_CSS_PADDING_LEFT] ||
            b->style->values[NS_CSS_PADDING_RIGHT] ||
            b->style->values[NS_CSS_BORDER_LEFT_WIDTH] ||
            b->style->values[NS_CSS_BORDER_RIGHT_WIDTH]) {
            ns_edges bm = {0}, bpd = {0}, bbd = {0};
            edges_from_style(b->style, cap, &bm, &bpd, &bbd);
            w += bpd.left + bpd.right + bbd.left + bbd.right;
        }
    } else {
        w += b->padding.left + b->padding.right +
             b->border.left  + b->border.right;
    }
    if (b->style) {
        const ns_css_value *bsv = b->style->values[NS_CSS_BOX_SIZING];
        gboolean border_box = bsv && bsv->kind == NS_CSS_V_KEYWORD &&
                              bsv->u.keyword &&
                              strcmp(bsv->u.keyword, "border-box") == 0;
        double box_extras = border_box ? 0 :
            b->padding.left + b->padding.right + b->border.left + b->border.right;
        const ns_css_value *mxw = b->style->values[NS_CSS_MAX_WIDTH];
        if (mxw && (mxw->kind == NS_CSS_V_LENGTH || mxw->kind == NS_CSS_V_CALC)) {
            double mx = length_resolve(mxw, cap, -1);
            if (mx >= 0 && w > mx + box_extras) w = mx + box_extras;
        }
        const ns_css_value *mnw = b->style->values[NS_CSS_MIN_WIDTH];
        if (mnw && (mnw->kind == NS_CSS_V_LENGTH || mnw->kind == NS_CSS_V_CALC)) {
            double mn = length_resolve(mnw, cap, -1);
            if (mn > 0 && w < mn + box_extras) w = mn + box_extras;
        }
    }
    if (w > cap) w = cap;
    return w;
}

static double
flex_content_basis_from_natural(ns_box *b, double cw, const ns_style *inherited)
{
    if (b->kind == NS_BOX_SVG) {
        layout_image(b, cw);
        return b->content_width > 0 ? b->content_width : 0;
    }
    double w = measure_natural_width(b, inherited ? inherited : b->style);
    return w > 0 ? w : 0;
}

static double
flex_item_max_main(ns_box *c, double cw, const ns_style *inherited)
{
    const ns_css_value *mxw = c->style ? c->style->values[NS_CSS_MAX_WIDTH] : NULL;
    if (mxw && mxw->kind == NS_CSS_V_KEYWORD)
        return flex_item_keyword_width(c, mxw, cw, inherited);
    if (!mxw || !(mxw->kind == NS_CSS_V_LENGTH || mxw->kind == NS_CSS_V_CALC))
        return -1;
    double mx = length_resolve(mxw, cw, -1);
    return mx < 0 ? -1 : flex_border_box_to_content(c, mx);
}

static gboolean
flex_item_is_replaced_like(const ns_box *c)
{
    if (c->kind == NS_BOX_IMAGE || c->kind == NS_BOX_VIDEO ||
        c->kind == NS_BOX_SVG)
        return TRUE;
    const ns_node *n = c->dom;
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "input") == 0) {
        const char *type = ns_element_get_attr(n, "type");
        return !type || (g_ascii_strcasecmp(type, "button") != 0 &&
                         g_ascii_strcasecmp(type, "submit") != 0 &&
                         g_ascii_strcasecmp(type, "reset") != 0);
    }
    return strcmp(n->name, "select") == 0 || strcmp(n->name, "textarea") == 0 ||
           strcmp(n->name, "meter") == 0 || strcmp(n->name, "progress") == 0;
}

static double
flex_item_min_main(ns_box *c, double cw, const ns_style *inherited)
{
    const ns_css_value *mnw = c->style ? c->style->values[NS_CSS_MIN_WIDTH] : NULL;
    if (mnw && (mnw->kind == NS_CSS_V_LENGTH || mnw->kind == NS_CSS_V_CALC)) {
        double mn = flex_border_box_to_content(c, length_resolve(mnw, cw, -1));
        return mn > 0 ? mn : 0;
    }
    if (mnw && mnw->kind == NS_CSS_V_KEYWORD && !keyword_is(mnw, "auto")) {
        double mn = flex_item_keyword_width(c, mnw, cw, inherited);
        return mn > 0 ? mn : 0;
    }
    if (box_is_scroll_container(c) || c->kind == NS_BOX_SVG) return 0;
    double mn = min_content_width_of(c, inherited ? inherited : c->style);
    if (mn < 0) mn = 0;
    const ns_css_value *wv = c->style ? c->style->values[NS_CSS_WIDTH] : NULL;
    if (wv && (wv->kind == NS_CSS_V_LENGTH || wv->kind == NS_CSS_V_CALC)) {
        double specified = value_is_percent(wv) && flex_item_is_replaced_like(c)
            ? flex_border_box_to_content(c, length_resolve(wv, 0, -1))
            : flex_border_box_to_content(c, length_resolve(wv, cw, -1));
        if (specified >= 0 && specified < mn) mn = specified;
    } else {
        double specified = flex_item_keyword_width(c, wv, cw, inherited);
        if (specified >= 0 && specified < mn) mn = specified;
    }
    double mx = flex_item_max_main(c, cw, inherited);
    if (mx >= 0 && mn > mx) mn = mx;
    return mn;
}

typedef struct {
    double basis;
    double min;
    double max;
    double grow;
    double shrink;
    double target;
    double violation;
    gboolean frozen;
    gboolean deferred;
    gboolean remeasured;
} ns_flex_len;

static double
flex_clamp_main(double v, double mn, double mx)
{
    if (mx >= 0 && v > mx) v = mx;
    if (v < mn) v = mn;
    return v < 0 ? 0 : v;
}

static void
flex_resolve_lengths(ns_flex_len *it, guint n, double available)
{
    double sum_hyp = 0;
    for (guint i = 0; i < n; i++)
        sum_hyp += flex_clamp_main(it[i].basis, it[i].min, it[i].max);
    gboolean growing = sum_hyp < available;
    double initial_free = available;
    for (guint i = 0; i < n; i++) {
        double hyp = flex_clamp_main(it[i].basis, it[i].min, it[i].max);
        double factor = growing ? it[i].grow : it[i].shrink;
        it[i].frozen = factor <= 0 ||
                       (growing && it[i].basis > hyp) ||
                       (!growing && it[i].basis < hyp);
        it[i].target = hyp;
        initial_free -= it[i].frozen ? hyp : it[i].basis;
    }
    for (guint iter = 0; iter <= n; iter++) {
        double sum_factor = 0, sum_scaled = 0, remaining = available;
        gboolean any = FALSE;
        for (guint i = 0; i < n; i++) {
            if (it[i].frozen) { remaining -= it[i].target; continue; }
            any = TRUE;
            remaining -= it[i].basis;
            sum_factor += growing ? it[i].grow : it[i].shrink;
            sum_scaled += it[i].shrink * it[i].basis;
        }
        if (!any) break;
        if (sum_factor < 1) {
            double product = initial_free * sum_factor;
            if (fabs(product) < fabs(remaining)) remaining = product;
        }
        double total_violation = 0;
        for (guint i = 0; i < n; i++) {
            if (it[i].frozen) continue;
            double t = it[i].basis;
            if (growing && remaining > 0 && sum_factor > 0)
                t += remaining * (it[i].grow / sum_factor);
            else if (!growing && remaining < 0 && sum_scaled > 0)
                t -= fabs(remaining) * (it[i].shrink * it[i].basis / sum_scaled);
            double clamped = flex_clamp_main(t, it[i].min, it[i].max);
            it[i].violation = clamped - t;
            it[i].target = clamped;
            total_violation += clamped - t;
        }
        for (guint i = 0; i < n; i++) {
            if (it[i].frozen) continue;
            if (total_violation > 0.0001 ? it[i].violation > 0 :
                total_violation < -0.0001 ? it[i].violation < 0 : TRUE)
                it[i].frozen = TRUE;
        }
    }
}

static gboolean
flex_container_scrolls(const ns_box *box)
{
    if (!box->style) return FALSE;
    return overflow_kw_scrolls(overflow_axis_keyword(box->style, NS_CSS_OVERFLOW_X)) ||
           overflow_kw_scrolls(overflow_axis_keyword(box->style, NS_CSS_OVERFLOW_Y));
}

static void
flex_justify_offsets(const ns_box *box, const char *justify, double free_main,
                     guint count, gboolean reverse,
                     double *leading, double *between)
{
    *leading = 0;
    *between = 0;
    if (count == 0) return;
    if (free_main < 0) {
        if (flex_container_scrolls(box)) {
            *leading = reverse ? free_main : 0;
            return;
        }
        if (strcmp(justify, "space-between") == 0 ||
            strcmp(justify, "space-around") == 0 ||
            strcmp(justify, "space-evenly") == 0)
            return;
    }
    if (strcmp(justify, "flex-end") == 0 || strcmp(justify, "end") == 0 ||
        strcmp(justify, "right") == 0)
        *leading = free_main;
    else if (strcmp(justify, "center") == 0)
        *leading = free_main / 2.0;
    else if (strcmp(justify, "space-between") == 0)
        *between = count > 1 ? free_main / (count - 1) : 0;
    else if (strcmp(justify, "space-around") == 0) {
        *between = free_main / count;
        *leading = *between / 2.0;
    } else if (strcmp(justify, "space-evenly") == 0) {
        *between = free_main / (count + 1);
        *leading = *between;
    }
}

static double
flex_grow_of(const ns_box *c)
{
    if (!c->style) return 0;
    return number_or(c->style->values[NS_CSS_FLEX_GROW], 0);
}

static double
flex_shrink_of(const ns_box *c)
{
    if (!c->style) return 1;
    double s = number_or(c->style->values[NS_CSS_FLEX_SHRINK], 1);
    return s < 0 ? 0 : s;
}

static void
shift_box_tree(ns_box *b, double dx, double dy)
{
    if (!b) return;
    b->x += dx;
    b->y += dy;
    if (b->grid_col_tracks)
        for (guint i = 0; i < b->grid_col_tracks->len; i++) {
            ns_grid_track_edges *e =
                &g_array_index(b->grid_col_tracks, ns_grid_track_edges, i);
            e->start += dx;
            e->end += dx;
        }
    if (b->grid_row_tracks)
        for (guint i = 0; i < b->grid_row_tracks->len; i++) {
            ns_grid_track_edges *e =
                &g_array_index(b->grid_row_tracks, ns_grid_track_edges, i);
            e->start += dy;
            e->end += dy;
        }
    for (ns_box *c = b->first_child; c; c = c->next_sibling)
        shift_box_tree(c, dx, dy);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++) {
            ns_box *ab =
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box;
            if (ab) shift_box_tree(ab, dx, dy);
        }
}

static double
gap_px(const ns_css_value *specific, const ns_css_value *shorthand, double basis)
{
    const ns_css_value *v = specific ? specific : shorthand;
    if (!v || (v->kind != NS_CSS_V_LENGTH && v->kind != NS_CSS_V_CALC))
        return 0;
    return length_resolve_nonnegative(v, basis, 0);
}

static double
flex_gap_of(const ns_style *s, double basis)
{
    if (!s) return 0;
    return gap_px(s->values[NS_CSS_COLUMN_GAP], s->values[NS_CSS_GAP], basis);
}

static double
flex_gap_row_of(const ns_style *s, double basis)
{
    if (!s) return 0;
    return gap_px(s->values[NS_CSS_ROW_GAP], s->values[NS_CSS_GAP], basis);
}

static gboolean
flex_wraps(const ns_style *s)
{
    if (!s) return FALSE;
    const ns_css_value *w = s->values[NS_CSS_FLEX_WRAP];
    if (!w || w->kind != NS_CSS_V_KEYWORD || !w->u.keyword) return FALSE;
    return strcmp(w->u.keyword, "wrap") == 0 ||
           strcmp(w->u.keyword, "wrap-reverse") == 0;
}

static double
flex_main_height_outer(const ns_box *c, const ns_css_value *v,
                       double cross_size, double container_main_size)
{
    double out = value_is_percent(v)
        ? resolve_height_with_basis(v, cross_size, container_main_size, 0)
        : length_resolve(v, cross_size, 0);
    if (!flex_box_is_border_box(c))
        out += c->padding.top + c->padding.bottom +
               c->border.top + c->border.bottom;
    return out;
}

static double
flex_basis_main_height(const ns_box *c, double cross_size,
                       double container_main_size, gboolean *out_explicit)
{
    *out_explicit = FALSE;
    const ns_style *s = c->style;
    if (!s) return 0;
    const ns_css_value *b = s->values[NS_CSS_FLEX_BASIS];
    if (b && (b->kind == NS_CSS_V_LENGTH || b->kind == NS_CSS_V_CALC)) {
        if (value_is_percent(b) && container_main_size < 0) return 0;
        *out_explicit = TRUE;
        return flex_main_height_outer(c, b, cross_size,
                                      container_main_size);
    }
    const ns_css_value *h = s->values[NS_CSS_HEIGHT];
    if (h && (h->kind == NS_CSS_V_LENGTH || h->kind == NS_CSS_V_CALC)) {
        if (value_is_percent(h) && container_main_size < 0) return 0;
        *out_explicit = TRUE;
        return flex_main_height_outer(c, h, cross_size,
                                      container_main_size);
    }
    return 0;
}

static gboolean
box_height_flexed_in_column(const ns_box *box)
{
    const ns_box *p = box->parent;
    if (!p || !box_is_flex_container(p) || style_is_absolute_or_fixed(box->style))
        return FALSE;
    if (strncmp(flex_direction_of(p->style), "column", 6) != 0) return FALSE;
    return box_read_definite_height(box) > 0;
}

static double
box_style_definite_height(const ns_box *box, double width_basis)
{
    const ns_css_value *hv = box->style ? box->style->values[NS_CSS_HEIGHT] : NULL;
    if (!hv || box->block_size_indefinite) return -1;
    if (hv->kind != NS_CSS_V_LENGTH && hv->kind != NS_CSS_V_CALC) return -1;
    return resolve_used_height(box, hv, width_basis, -1);
}

static double
flex_row_definite_cross(const ns_box *box, double explicit_cross)
{
    if (explicit_cross > 0 && !box_height_flexed_in_column(box))
        return explicit_cross;
    return box_read_definite_height(box) > 0 ? box->definite_height
                                             : explicit_cross;
}

static void
relayout_at_definite_height(ns_box *c, double layout_width, double target_h,
                            const ns_style *child_inherited)
{
    c->definite_height = target_h;
    double sx = c->x, sy = c->y;
    layout_box(c, layout_width, child_inherited);
    if (c->x != sx || c->y != sy)
        shift_box_tree(c, sx - c->x, sy - c->y);
    c->content_height = target_h;
}

static void
flex_relayout_after_cross_resize(ns_box *c, double layout_width,
                                 double main_size, double pre_h,
                                 const ns_style *child_inherited)
{
    if (!c->first_child) return;
    double target_h = c->content_height;
    if (fabs(target_h - pre_h) < 0.01) return;
    c->flex_main_size = main_size;
    c->has_flex_main = TRUE;
    relayout_at_definite_height(c, layout_width, target_h, child_inherited);
}

static gboolean
flex_align_stretches(const char *align)
{
    return strcmp(align, "stretch") == 0 || strcmp(align, "normal") == 0;
}

static gboolean
flex_item_cross_size_auto(const ns_box *c)
{
    const ns_css_value *h = c->style ? c->style->values[NS_CSS_HEIGHT] : NULL;
    if (!h || h->kind == NS_CSS_V_KEYWORD) return !size_keyword_is_intrinsic(h);
    return value_is_percent(h) && containing_block_definite_height(c) < 0;
}

static double
flex_item_stretched_height(const ns_box *c, double line_cross_size,
                           double width_basis)
{
    double vex = c->padding.top + c->padding.bottom +
                 c->border.top + c->border.bottom;
    double h = line_cross_size - c->margin.top - c->margin.bottom - vex;
    if (h < 0) h = 0;
    if (!c->style) return h;
    double sizing_extras = flex_box_is_border_box(c) ? vex : 0;
    double mx = resolve_used_height(c, c->style->values[NS_CSS_MAX_HEIGHT],
                                    width_basis, -1);
    if (mx >= 0 && h > mx - sizing_extras)
        h = mx > sizing_extras ? mx - sizing_extras : 0;
    double mn = resolve_used_height(c, c->style->values[NS_CSS_MIN_HEIGHT],
                                    width_basis, -1);
    if (mn >= 0 && h < mn - sizing_extras) h = mn - sizing_extras;
    return h;
}

static void
flex_item_fit_line_keyword_limits(ns_box *c, double line_cross_size,
                                  double width_basis)
{
    if (!c->style) return;
    const ns_css_value *mxh = c->style->values[NS_CSS_MAX_HEIGHT];
    const ns_css_value *mnh = c->style->values[NS_CSS_MIN_HEIGHT];
    gboolean max_stretches = height_keyword_stretches(mxh);
    gboolean min_stretches = height_keyword_stretches(mnh);
    if (!max_stretches && !min_stretches) return;
    double vex = c->padding.top + c->padding.bottom +
                 c->border.top + c->border.bottom;
    double line_inner = line_cross_size - c->margin.top - c->margin.bottom - vex;
    if (line_inner < 0) line_inner = 0;
    double min_h = line_inner;
    if (!min_stretches) {
        min_h = resolve_used_height(c, mnh, width_basis, -1);
        if (min_h >= 0 && flex_box_is_border_box(c)) min_h -= vex;
    }
    if (max_stretches && c->content_height > line_inner)
        c->content_height = line_inner;
    if (min_h >= 0 && c->content_height < min_h)
        c->content_height = min_h;
}

static gboolean
flex_preset_cross_size(ns_box *c, double line_cross_size, double width_basis)
{
    if (!c->first_child) return FALSE;
    double stretched = flex_item_stretched_height(c, line_cross_size,
                                                  width_basis);
    if (stretched <= 0) return FALSE;
    c->definite_height = stretched;
    return TRUE;
}

static double
flex_row_cross_size(double base, double min_cross, double max_cross_limit)
{
    double cross = base;
    if (min_cross > cross) cross = min_cross;
    if (max_cross_limit >= 0 && cross > max_cross_limit)
        cross = max_cross_limit < min_cross ? min_cross : max_cross_limit;
    return cross;
}

static gboolean
flex_row_item_stretches(const ns_box *c, const char *align)
{
    if (c->style && (keyword_is(c->style->values[NS_CSS_MARGIN_TOP], "auto") ||
                     keyword_is(c->style->values[NS_CSS_MARGIN_BOTTOM], "auto")))
        return FALSE;
    return flex_align_stretches(flex_item_align(c, align)) &&
           flex_item_cross_size_auto(c);
}

static void
flex_row_first_pass_height(ns_box *c, const char *align, double cross,
                           double width_basis)
{
    c->definite_height = 0;
    if (cross > 0 && flex_row_item_stretches(c, align))
        flex_preset_cross_size(c, cross, width_basis);
    c->definite_height_before_flex = c->definite_height;
}

static void
flex_item_keep_auto_margins(ns_box *c, double share)
{
    if (!c->style || share <= 0) return;
    if (keyword_is(c->style->values[NS_CSS_MARGIN_LEFT], "auto"))
        c->margin.left = share;
    if (keyword_is(c->style->values[NS_CSS_MARGIN_RIGHT], "auto"))
        c->margin.right = share;
}

static void
flex_row_item_edges(ns_box *c, double cw)
{
    edges_from_style(c->style, cw, &c->margin, &c->padding, &c->border);
    c->cb_width_override = cw;
    c->cb_width_set = TRUE;
}

static void
layout_flex_row(ns_box *box, double cw,
                double inner_x, double inner_y,
                const ns_style *child_inherited,
                gboolean reverse,
                double parent_content_width,
                double *cursor_y_out)
{
    const ns_css_value *hv_box = box->style ? box->style->values[NS_CSS_HEIGHT] : NULL;
    const ns_css_value *mnh_box = box->style ? box->style->values[NS_CSS_MIN_HEIGHT] : NULL;
    const ns_css_value *mxh_box = box->style ? box->style->values[NS_CSS_MAX_HEIGHT] : NULL;
    double explicit_cross = box_style_definite_height(box, parent_content_width);
    gboolean definite_cross = explicit_cross > 0;
    if (explicit_cross < 0) explicit_cross = 0;
    double min_cross = resolve_used_height(box, mnh_box, parent_content_width, -1);
    double max_cross_limit = resolve_used_height(box, mxh_box,
                                                 parent_content_width, -1);
    if (box->style && box->style->values[NS_CSS_BOX_SIZING] &&
        box->style->values[NS_CSS_BOX_SIZING]->kind == NS_CSS_V_KEYWORD &&
        strcmp(box->style->values[NS_CSS_BOX_SIZING]->u.keyword, "border-box") == 0) {
        double vex = box->border.top + box->border.bottom +
                     box->padding.top + box->padding.bottom;
        if (explicit_cross > 0) {
            explicit_cross -= vex;
            if (explicit_cross < 0) explicit_cross = 0;
        }
        if (min_cross > 0) {
            min_cross -= vex;
            if (min_cross < 0) min_cross = 0;
        }
        if (max_cross_limit > 0) {
            max_cross_limit -= vex;
            if (max_cross_limit < 0) max_cross_limit = 0;
        }
    }
    if (min_cross > explicit_cross) explicit_cross = min_cross;
    if (explicit_cross <= 0 && box->style &&
        style_is_absolute_or_fixed(box->style) &&
        box->content_height > 0 &&
        (hv_box || (box->style->values[NS_CSS_TOP] &&
                    box->style->values[NS_CSS_BOTTOM])))
        explicit_cross = box->content_height;
    explicit_cross = flex_row_definite_cross(box, explicit_cross);
    if (!definite_cross && explicit_cross > 0 && box->definite_height > 0)
        definite_cross = TRUE;

    GPtrArray *items = g_ptr_array_new();
    for (ns_box *c = box->first_child; c; c = c->next_sibling)
        if (!style_is_absolute_or_fixed(c->style))
            g_ptr_array_add(items, c);

    double gap = flex_gap_of(box->style, cw);
    double total_extras = 0;
    ns_flex_len *lens = g_new0(ns_flex_len, items->len + 1);
    for (guint i = 0; i < items->len; i++) {
        ns_box *c = items->pdata[i];
        flex_row_item_edges(c, cw);
        total_extras += c->margin.left + c->margin.right +
                        c->padding.left + c->padding.right +
                        c->border.left + c->border.right;
        double b = 0;
        if (!flex_main_basis_explicit(c, cw, child_inherited, &b))
            b = flex_content_basis_from_natural(c, cw, child_inherited);
        lens[i].basis = b;
        lens[i].min = flex_item_min_main(c, cw, child_inherited);
        lens[i].max = flex_item_max_main(c, cw, child_inherited);
        lens[i].grow = flex_grow_of(c);
        lens[i].shrink = flex_shrink_of(c);
    }
    if (items->len > 1) total_extras += gap * (items->len - 1);
    flex_resolve_lengths(lens, items->len, cw - total_extras);

    GArray *assigned_main = g_array_new(FALSE, FALSE, sizeof(double));
    GArray *measured_h    = g_array_new(FALSE, FALSE, sizeof(double));
    double max_cross = 0;
    double used_main = total_extras;
    for (guint i = 0; i < items->len; i++) {
        g_array_append_val(assigned_main, lens[i].target);
        used_main += lens[i].target;
    }
    g_free(lens);
    double free_main = cw - used_main;
    int auto_margins = 0;
    double auto_share = 0;
    for (guint i = 0; i < items->len; i++) {
        ns_box *c = items->pdata[i];
        if (!c->style) continue;
        if (keyword_is(c->style->values[NS_CSS_MARGIN_LEFT], "auto"))  auto_margins++;
        if (keyword_is(c->style->values[NS_CSS_MARGIN_RIGHT], "auto")) auto_margins++;
    }
    if (auto_margins > 0 && free_main > 0) {
        double share = free_main / auto_margins;
        auto_share = share;
        for (guint i = 0; i < items->len; i++) {
            ns_box *c = items->pdata[i];
            if (!c->style) continue;
            if (keyword_is(c->style->values[NS_CSS_MARGIN_LEFT], "auto"))
                c->margin.left += share;
            if (keyword_is(c->style->values[NS_CSS_MARGIN_RIGHT], "auto"))
                c->margin.right += share;
        }
        free_main = 0;
    }
    const char *justify = keyword_or(box->style, NS_CSS_JUSTIFY_CONTENT, "flex-start");
    double leading = 0;
    double between = 0;
    if (auto_margins == 0 || free_main < 0)
        flex_justify_offsets(box, justify, free_main, items->len, reverse,
                             &leading, &between);

    const char *align = keyword_or(box->style, NS_CSS_ALIGN_ITEMS, "stretch");
    double preset_cross = definite_cross
        ? flex_row_cross_size(explicit_cross, min_cross, max_cross_limit) : -1;
    for (guint i = 0; i < items->len; i++) {
        ns_box *c = items->pdata[i];
        double a = g_array_index(assigned_main, double, i);
        c->x = inner_x;
        c->y = inner_y;
        c->flex_main_size = a;
        c->has_flex_main = TRUE;
        flex_row_first_pass_height(c, align, preset_cross, cw);
        layout_box(c, a + c->margin.left + c->margin.right
                       + c->border.left + c->border.right
                       + c->padding.left + c->padding.right, child_inherited);
        flex_item_keep_auto_margins(c, auto_share);
        c->flex_pass_x = c->x;
        c->flex_pass_y = c->y;
        double item_h = c->content_height +
                        c->padding.top + c->padding.bottom +
                        c->border.top + c->border.bottom +
                        c->margin.top + c->margin.bottom;
        g_array_append_val(measured_h, item_h);
        if (item_h > max_cross) max_cross = item_h;
    }

    gboolean rtl = strcmp(keyword_or(box->style, NS_CSS_DIRECTION, "ltr"),
                          "rtl") == 0;
    gboolean main_reversed = reverse != rtl;
    double cursor_x = main_reversed ? inner_x + cw - leading : inner_x + leading;
    double cross_size = flex_row_cross_size(
        definite_cross || max_cross < explicit_cross ? explicit_cross : max_cross,
        min_cross, max_cross_limit);

    double cross_baseline = 0;
    double cross_below_baseline = 0;
    for (guint k = 0; k < items->len; k++) {
        ns_box *c = items->pdata[k];
        if (!flex_align_is_baseline(flex_item_align(c, align))) continue;
        double item_h_full = g_array_index(measured_h, double, k);
        double b = flex_item_baseline(c, item_h_full);
        if (b > cross_baseline) cross_baseline = b;
        if (item_h_full - b > cross_below_baseline)
            cross_below_baseline = item_h_full - b;
    }
    if (!definite_cross && cross_baseline + cross_below_baseline > cross_size)
        cross_size = cross_baseline + cross_below_baseline;

    for (guint k = 0; k < items->len; k++) {
        guint i = k;
        ns_box *c = items->pdata[i];
        const char *eff_align = flex_item_align(c, align);
        double item_h_full = g_array_index(measured_h, double, i);
        gboolean mt_auto = c->style &&
            keyword_is(c->style->values[NS_CSS_MARGIN_TOP], "auto");
        gboolean mb_auto = c->style &&
            keyword_is(c->style->values[NS_CSS_MARGIN_BOTTOM], "auto");
        double cy = inner_y;
        if (mt_auto || mb_auto) {
            double free_cross = cross_size - item_h_full;
            if (free_cross < 0) free_cross = 0;
            if (mt_auto && mb_auto) cy = inner_y + free_cross / 2.0;
            else if (mt_auto)       cy = inner_y + free_cross;
        } else if (strcmp(eff_align, "center") == 0) {
            cy = inner_y + (cross_size - item_h_full) / 2.0;
        } else if (strcmp(eff_align, "flex-end") == 0 || strcmp(eff_align, "end") == 0) {
            cy = inner_y + cross_size - item_h_full;
        } else if (flex_align_is_baseline(eff_align)) {
            cy = inner_y + cross_baseline - flex_item_baseline(c, item_h_full);
        }
        double a = g_array_index(assigned_main, double, i);
        double outer_main = a + c->margin.left + c->margin.right +
                            c->padding.left + c->padding.right +
                            c->border.left + c->border.right;
        if (main_reversed) cursor_x -= outer_main;
        c->x = cursor_x;
        c->y = cy;
        c->flex_main_size = a;
        c->has_flex_main = TRUE;
        double item_layout_width = a + c->margin.left + c->margin.right
                                     + c->border.left + c->border.right
                                     + c->padding.left + c->padding.right;
        gboolean stretches = !mt_auto && !mb_auto &&
                             flex_align_stretches(eff_align) &&
                             flex_item_cross_size_auto(c);
        gboolean cross_preset = stretches &&
                                flex_preset_cross_size(c, cross_size, cw);
        gboolean same_input = c->last_layout_width == item_layout_width &&
            (c->definite_height == c->definite_height_before_flex ||
             !c->definite_height_read);
        if (same_input && (!stretches || cross_preset || !c->first_child)) {
            c->x = c->flex_pass_x;
            c->y = c->flex_pass_y;
            shift_box_tree(c, cursor_x - inner_x, cy - inner_y);
        } else {
            layout_box(c, item_layout_width, child_inherited);
            flex_item_keep_auto_margins(c, auto_share);
        }
        if (!stretches)
            flex_item_fit_line_keyword_limits(c, cross_size, cw);
        if (stretches) {
            double pre_h = c->content_height;
            c->content_height = flex_item_stretched_height(c, cross_size, cw);
            if (c->content_height < pre_h - 0.01 && c->style &&
                overflow_kw_scrolls(overflow_axis_keyword(c->style,
                                                          NS_CSS_OVERFLOW_Y))) {
                c->scrolls = TRUE;
                if (pre_h - c->content_height > c->scroll_max_y)
                    c->scroll_max_y = pre_h - c->content_height;
            }
            if (c->definite_height <= 0)
                c->definite_height = c->content_height;
            if (!cross_preset)
                flex_relayout_after_cross_resize(c, item_layout_width, a,
                                                 pre_h, child_inherited);
        }
        if (main_reversed) cursor_x -= gap + between;
        else               cursor_x += outer_main + gap + between;
    }
    g_array_free(measured_h, TRUE);

    *cursor_y_out = inner_y + cross_size;
    g_array_free(assigned_main, TRUE);
    g_ptr_array_free(items, TRUE);
}

static void
flex_align_content_offsets(const ns_box *box, double free_cross, guint n,
                           double *lead, double *between, double *per_line)
{
    const char *acont = keyword_or(box->style, NS_CSS_ALIGN_CONTENT, "stretch");
    gboolean wrap_reverse =
        keyword_is(box->style ? box->style->values[NS_CSS_FLEX_WRAP] : NULL,
                   "wrap-reverse");
    if (strcmp(acont, "start") == 0 || strcmp(acont, "left") == 0 ||
        strcmp(acont, "self-start") == 0)
        acont = wrap_reverse ? "flex-end" : "flex-start";
    else if (strcmp(acont, "end") == 0 || strcmp(acont, "right") == 0 ||
             strcmp(acont, "self-end") == 0)
        acont = wrap_reverse ? "flex-start" : "flex-end";
    *lead = 0;
    *between = 0;
    *per_line = 0;
    if (n == 0) return;
    if (free_cross < 0) {
        if (flex_container_scrolls(box)) {
            *lead = wrap_reverse ? free_cross : 0;
            return;
        }
        if (strcmp(acont, "space-between") == 0 ||
            strcmp(acont, "space-around") == 0 ||
            strcmp(acont, "space-evenly") == 0 ||
            strcmp(acont, "stretch") == 0 || strcmp(acont, "normal") == 0)
            return;
    }
    if (strcmp(acont, "stretch") == 0 || strcmp(acont, "normal") == 0)
        *per_line = free_cross / n;
    else if (strcmp(acont, "center") == 0)
        *lead = free_cross / 2.0;
    else if (strcmp(acont, "flex-end") == 0 || strcmp(acont, "end") == 0)
        *lead = free_cross;
    else if (strcmp(acont, "space-between") == 0)
        *between = n > 1 ? free_cross / (n - 1) : 0;
    else if (strcmp(acont, "space-around") == 0) {
        *between = free_cross / n;
        *lead = *between / 2.0;
    } else if (strcmp(acont, "space-evenly") == 0) {
        *between = free_cross / (n + 1);
        *lead = *between;
    }
}

static void
layout_flex_row_wrap(ns_box *box, double cw,
                     double inner_x, double inner_y,
                     const ns_style *child_inherited,
                     gboolean reverse,
                     double *cursor_y_out)
{
    GPtrArray *items = g_ptr_array_new();
    for (ns_box *c = box->first_child; c; c = c->next_sibling)
        if (!style_is_absolute_or_fixed(c->style))
            g_ptr_array_add(items, c);
    double gap = flex_gap_of(box->style, cw);
    double row_gap = flex_gap_row_of(box->style,
                                     box_read_definite_height(box) > 0
                                         ? box->definite_height : 0);
    const char *align = keyword_or(box->style, NS_CSS_ALIGN_ITEMS, "stretch");
    const char *justify = keyword_or(box->style, NS_CSS_JUSTIFY_CONTENT, "flex-start");
    gboolean rtl = strcmp(keyword_or(box->style, NS_CSS_DIRECTION, "ltr"),
                          "rtl") == 0;
    gboolean main_reversed = reverse != rtl;
    typedef struct { double top, height; guint start, count; } flex_line;
    GArray *lines = g_array_new(FALSE, FALSE, sizeof(flex_line));

    GArray *extras_arr = g_array_new(FALSE, TRUE, sizeof(double));
    GArray *main_arr   = g_array_new(FALSE, TRUE, sizeof(double));
    g_array_set_size(extras_arr, items->len);
    g_array_set_size(main_arr, items->len);
    ns_flex_len *lens = g_new0(ns_flex_len, items->len + 1);
    for (guint n = 0; n < items->len; n++) {
        ns_box *c = items->pdata[n];
        flex_row_item_edges(c, cw);
        g_array_index(extras_arr, double, n) =
            c->margin.left + c->margin.right +
            c->padding.left + c->padding.right +
            c->border.left + c->border.right;
        double b = 0;
        if (!flex_main_basis_explicit(c, cw, child_inherited, &b))
            b = flex_content_basis_from_natural(c, cw, child_inherited);
        lens[n].basis = b;
        lens[n].min = flex_item_min_main(c, cw, child_inherited);
        lens[n].max = flex_item_max_main(c, cw, child_inherited);
        lens[n].grow = flex_grow_of(c);
        lens[n].shrink = flex_shrink_of(c);
    }

    double line_y = inner_y;
    guint i = 0;
    while (i < items->len) {
        guint line_start = i;
        double used = 0;
        double line_max_h = 0;
        guint line_count = 0;
        for (; i < items->len; i++) {
            double item_outer = flex_clamp_main(lens[i].basis, lens[i].min,
                                                lens[i].max) +
                                g_array_index(extras_arr, double, i);
            double try_used = used + (line_count > 0 ? gap : 0) + item_outer;
            if (try_used > cw + 0.5 && line_count > 0) break;
            used = try_used;
            line_count++;
        }

        double line_extras = line_count > 1 ? gap * (line_count - 1) : 0;
        for (guint k = 0; k < line_count; k++)
            line_extras += g_array_index(extras_arr, double, line_start + k);
        flex_resolve_lengths(lens + line_start, line_count, cw - line_extras);
        double remaining = cw - line_extras;
        for (guint k = 0; k < line_count; k++)
            remaining -= lens[line_start + k].target;

        int line_auto_margins = 0;
        double line_auto_share = 0;
        for (guint k = 0; k < line_count; k++) {
            ns_box *c = items->pdata[line_start + k];
            if (!c->style) continue;
            if (keyword_is(c->style->values[NS_CSS_MARGIN_LEFT], "auto"))
                line_auto_margins++;
            if (keyword_is(c->style->values[NS_CSS_MARGIN_RIGHT], "auto"))
                line_auto_margins++;
        }
        if (line_auto_margins > 0 && remaining > 0) {
            double share = remaining / line_auto_margins;
            line_auto_share = share;
            for (guint k = 0; k < line_count; k++) {
                ns_box *c = items->pdata[line_start + k];
                if (!c->style) continue;
                double *extras = &g_array_index(extras_arr, double,
                                                line_start + k);
                if (keyword_is(c->style->values[NS_CSS_MARGIN_LEFT], "auto")) {
                    c->margin.left += share;
                    *extras += share;
                }
                if (keyword_is(c->style->values[NS_CSS_MARGIN_RIGHT], "auto")) {
                    c->margin.right += share;
                    *extras += share;
                }
            }
            remaining = 0;
        }

        double leading = 0;
        double between = 0;
        if (line_auto_margins == 0 || remaining < 0)
            flex_justify_offsets(box, justify, remaining, line_count, reverse,
                                 &leading, &between);

        for (guint k = 0; k < line_count; k++) {
            guint gi = line_start + k;
            ns_box *c = items->pdata[gi];
            double a = lens[gi].target;
            g_array_index(main_arr, double, gi) = a;
            c->x = inner_x;
            c->y = line_y;
            layout_box(c, a + g_array_index(extras_arr, double, gi),
                       child_inherited);
            flex_item_keep_auto_margins(c, line_auto_share);
            double item_h = c->content_height +
                            c->padding.top + c->padding.bottom +
                            c->border.top + c->border.bottom +
                            c->margin.top + c->margin.bottom;
            if (item_h > line_max_h) line_max_h = item_h;
        }

        double line_baseline = 0;
        double line_below_baseline = 0;
        for (guint k = 0; k < line_count; k++) {
            ns_box *c = items->pdata[line_start + k];
            if (!flex_align_is_baseline(flex_item_align(c, align))) continue;
            double item_h_full = c->content_height +
                                 c->padding.top + c->padding.bottom +
                                 c->border.top + c->border.bottom +
                                 c->margin.top + c->margin.bottom;
            double b = flex_item_baseline(c, item_h_full);
            if (b > line_baseline) line_baseline = b;
            if (item_h_full - b > line_below_baseline)
                line_below_baseline = item_h_full - b;
        }
        if (line_baseline + line_below_baseline > line_max_h)
            line_max_h = line_baseline + line_below_baseline;

        double cursor_x = inner_x + leading;
        for (guint k = 0; k < line_count; k++) {
            guint idx = line_start + k;
            ns_box *c = items->pdata[idx];
            const char *eff_align = flex_item_align(c, align);
            double item_h_full = c->content_height +
                                 c->padding.top + c->padding.bottom +
                                 c->border.top + c->border.bottom +
                                 c->margin.top + c->margin.bottom;
            gboolean mt_auto = c->style &&
                keyword_is(c->style->values[NS_CSS_MARGIN_TOP], "auto");
            gboolean mb_auto = c->style &&
                keyword_is(c->style->values[NS_CSS_MARGIN_BOTTOM], "auto");
            double cy = line_y;
            if (mt_auto || mb_auto) {
                double free_line = line_max_h - item_h_full;
                if (free_line < 0) free_line = 0;
                if (mt_auto && mb_auto) cy = line_y + free_line / 2.0;
                else if (mt_auto)       cy = line_y + free_line;
            } else if (strcmp(eff_align, "center") == 0)
                cy = line_y + (line_max_h - item_h_full) / 2.0;
            else if (strcmp(eff_align, "flex-end") == 0 || strcmp(eff_align, "end") == 0)
                cy = line_y + line_max_h - item_h_full;
            else if (flex_align_is_baseline(eff_align))
                cy = line_y + line_baseline -
                     flex_item_baseline(c, item_h_full);
            c->x = cursor_x;
            c->y = cy;
            c->flex_main_size = g_array_index(main_arr, double, idx);
            c->has_flex_main = TRUE;
            double item_layout_width = g_array_index(main_arr, double, idx) +
                                       g_array_index(extras_arr, double, idx);
            gboolean stretches = !mt_auto && !mb_auto &&
                                 flex_item_cross_size_auto(c) &&
                                 flex_align_stretches(eff_align);
            gboolean cross_preset = stretches &&
                                    flex_preset_cross_size(c, line_max_h, cw);
            layout_box(c, item_layout_width, child_inherited);
            double outer = c->content_width
                + c->padding.left + c->padding.right
                + c->border.left + c->border.right;
            if (stretches) {
                double pre_h = c->content_height;
                double stretched = flex_item_stretched_height(c, line_max_h, cw);
                if (stretched > c->content_height) c->content_height = stretched;
                if (!cross_preset)
                    flex_relayout_after_cross_resize(
                        c, item_layout_width,
                        g_array_index(main_arr, double, idx),
                        pre_h, child_inherited);
            }
            cursor_x += outer + c->margin.left + c->margin.right + gap + between;
        }
        if (main_reversed) {
            for (guint k = 0; k < line_count; k++) {
                ns_box *c = items->pdata[line_start + k];
                double w = c->content_width
                         + c->padding.left + c->padding.right
                         + c->border.left + c->border.right
                         + c->margin.left + c->margin.right;
                double nx = inner_x + cw - (c->x - inner_x) - w;
                if (nx != c->x) shift_box_tree(c, nx - c->x, 0);
            }
        }
        flex_line fl = { .top = line_y, .height = line_max_h,
                         .start = line_start, .count = line_count };
        g_array_append_val(lines, fl);
        line_y += line_max_h + row_gap;
    }

    double measured = (line_y - (items->len > 0 ? row_gap : 0)) - inner_y;
    double free_cross = 0;
    double container_cross = -1;
    gboolean cross_definite = FALSE;
    {
        const ns_css_value *hv = box->style
            ? box->style->values[NS_CSS_HEIGHT] : NULL;
        double eh = -1;
        if (hv && (hv->kind == NS_CSS_V_LENGTH || hv->kind == NS_CSS_V_CALC) &&
            lines->len > 0) {
            eh = resolve_used_height(box, hv, cw, -1);
            if (eh >= 0 &&
                keyword_is(box->style->values[NS_CSS_BOX_SIZING], "border-box"))
                eh -= box->padding.top + box->padding.bottom +
                      box->border.top + box->border.bottom;
            if (eh >= 0) eh = flex_wrap_clamp_height(box, eh, cw);
        }
        gboolean flexed_item = box->parent &&
            box_is_flex_container(box->parent) &&
            box_read_definite_height(box) > 0;
        if ((eh < 0 || flexed_item) && box->definite_height > 0 &&
            lines->len > 0)
            eh = box->definite_height;
        if (eh >= 0) {
            cross_definite = TRUE;
            container_cross = eh;
            free_cross = eh - measured;
        }
    }
    if (cross_definite && fabs(free_cross) > 0.01) {
        guint n = lines->len;
        double lead = 0, between_lines = 0, per_line = 0;
        flex_align_content_offsets(box, free_cross, n,
                                   &lead, &between_lines, &per_line);
        for (guint li = 0; li < n; li++) {
            flex_line *fl = &g_array_index(lines, flex_line, li);
            double dy = lead + (between_lines + per_line) * li;
            double line_h = fl->height + per_line;
            fl->top += dy;
            fl->height = line_h;
            for (guint k = 0; k < fl->count; k++) {
                ns_box *c = items->pdata[fl->start + k];
                if (dy != 0) shift_box_tree(c, 0, dy);
                if (per_line > 0.5) {
                    const char *eff_align = align;
                    const char *as = c->style
                        ? ns_style_keyword(c->style, NS_CSS_ALIGN_SELF) : NULL;
                    if (as && strcmp(as, "auto") != 0) eff_align = as;
                    if (flex_align_stretches(eff_align) &&
                        flex_item_cross_size_auto(c)) {
                        guint idx = fl->start + k;
                        double pre_h = c->content_height;
                        double stretched =
                            flex_item_stretched_height(c, line_h, cw);
                        if (stretched > c->content_height)
                            c->content_height = stretched;
                        flex_relayout_after_cross_resize(
                            c, g_array_index(main_arr, double, idx)
                               + g_array_index(extras_arr, double, idx),
                            g_array_index(main_arr, double, idx),
                            pre_h, child_inherited);
                    } else if (strcmp(eff_align, "center") == 0) {
                        shift_box_tree(c, 0, per_line / 2.0);
                    } else if (strcmp(eff_align, "flex-end") == 0 ||
                               strcmp(eff_align, "end") == 0) {
                        shift_box_tree(c, 0, per_line);
                    }
                }
            }
        }
        line_y += free_cross > 0 ? free_cross : 0;
    }

    *cursor_y_out = line_y - (items->len > 0 ? row_gap : 0);

    if (keyword_is(box->style->values[NS_CSS_FLEX_WRAP], "wrap-reverse")) {
        double cross_total = container_cross >= 0 ? container_cross
                                                  : *cursor_y_out - inner_y;
        for (guint li = 0; li < lines->len; li++) {
            const flex_line *fl = &g_array_index(lines, flex_line, li);
            double mirrored_top = inner_y + cross_total -
                                  (fl->top - inner_y) - fl->height;
            for (guint k = 0; k < fl->count; k++) {
                ns_box *c = items->pdata[fl->start + k];
                double outer_h = c->content_height +
                    c->margin.top + c->margin.bottom +
                    c->padding.top + c->padding.bottom +
                    c->border.top + c->border.bottom;
                double within = c->y - fl->top;
                double target = mirrored_top + fl->height - within - outer_h;
                if (target != c->y) shift_box_tree(c, 0, target - c->y);
            }
        }
    }

    g_ptr_array_free(items, TRUE);
    g_array_free(lines, TRUE);
    g_free(lens);
    g_array_free(extras_arr, TRUE);
    g_array_free(main_arr, TRUE);
}

static double
flex_item_stretch_main_height(const ns_box *c, double container_main_size)
{
    if (container_main_size < 0) return -1;
    double h = container_main_size - c->margin.top - c->margin.bottom;
    return h > 0 ? h : 0;
}

static double
flex_item_min_main_height(ns_box *c, double cw, double pct_basis)
{
    double vextra = c->padding.top + c->padding.bottom +
                    c->border.top + c->border.bottom;
    const ns_css_value *mnh = c->style ? c->style->values[NS_CSS_MIN_HEIGHT] : NULL;
    if (mnh && (mnh->kind == NS_CSS_V_LENGTH || mnh->kind == NS_CSS_V_CALC)) {
        if (value_is_percent(mnh) && pct_basis < 0) return 0;
        double mn = flex_main_height_outer(c, mnh, cw, pct_basis);
        return mn > 0 ? mn : 0;
    }
    double content = (c->measured_content_height >= 0
                      ? c->measured_content_height : c->content_height) + vextra;
    if (mnh && mnh->kind == NS_CSS_V_KEYWORD && !keyword_is(mnh, "auto")) {
        if (size_keyword_is_intrinsic(mnh))
            return content > 0 ? content : 0;
        double stretch = flex_item_stretch_main_height(c, pct_basis);
        return stretch > 0 ? stretch : 0;
    }
    if (box_is_scroll_container(c) || c->kind == NS_BOX_SVG) return 0;
    const ns_css_value *hv = c->style ? c->style->values[NS_CSS_HEIGHT] : NULL;
    if (hv && (hv->kind == NS_CSS_V_LENGTH || hv->kind == NS_CSS_V_CALC) &&
        !(value_is_percent(hv) && pct_basis < 0)) {
        double specified = value_is_percent(hv) && flex_item_is_replaced_like(c)
            ? flex_main_height_outer(c, hv, cw, 0)
            : flex_main_height_outer(c, hv, cw, pct_basis);
        if (specified >= 0 && specified < content) content = specified;
    }
    return content > 0 ? content : 0;
}

static double
flex_item_max_main_height(ns_box *c, double cw, double pct_basis)
{
    const ns_css_value *mxh = c->style ? c->style->values[NS_CSS_MAX_HEIGHT] : NULL;
    if (size_keyword_is_intrinsic(mxh)) {
        double content = c->measured_content_height >= 0
            ? c->measured_content_height : c->content_height;
        return content + c->padding.top + c->padding.bottom +
               c->border.top + c->border.bottom;
    }
    if (height_keyword_stretches(mxh))
        return flex_item_stretch_main_height(c, pct_basis);
    if (!mxh || !(mxh->kind == NS_CSS_V_LENGTH || mxh->kind == NS_CSS_V_CALC))
        return -1;
    if (value_is_percent(mxh) && pct_basis < 0) return -1;
    return flex_main_height_outer(c, mxh, cw, pct_basis);
}

static const char *
flex_column_item_align(const ns_box *c, const char *align)
{
    const char *as = c->style ? ns_style_keyword(c->style, NS_CSS_ALIGN_SELF) : NULL;
    if (as && strcmp(as, "auto") != 0) return as;
    return align;
}

static gboolean
flex_column_item_shrinks_to_fit(const ns_box *c, const char *align)
{
    if (c->style && (length_is_auto(c->style->values[NS_CSS_MARGIN_LEFT]) ||
                     length_is_auto(c->style->values[NS_CSS_MARGIN_RIGHT])))
        return TRUE;
    const char *eff = flex_column_item_align(c, align);
    return strcmp(eff, "stretch") != 0 && strcmp(eff, "normal") != 0;
}

static void
flex_stretch_replaced_width(ns_box *c, double line_w)
{
    if (c->kind != NS_BOX_IMAGE && c->kind != NS_BOX_VIDEO &&
        c->kind != NS_BOX_SVG)
        return;
    const ns_style *s = c->style;
    if (!s || length_is_auto(s->values[NS_CSS_MARGIN_LEFT]) ||
        length_is_auto(s->values[NS_CSS_MARGIN_RIGHT]))
        return;
    double hextra = c->padding.left + c->padding.right +
                    c->border.left + c->border.right;
    double w = line_w - c->margin.left - c->margin.right - hextra;
    gboolean border_box =
        keyword_is(s->values[NS_CSS_BOX_SIZING], "border-box");
    double max_w = length_resolve(s->values[NS_CSS_MAX_WIDTH], line_w, -1);
    double min_w = length_resolve(s->values[NS_CSS_MIN_WIDTH], line_w, -1);
    if (border_box) {
        if (max_w >= 0) max_w = MAX(max_w - hextra, 0);
        if (min_w >= 0) min_w = MAX(min_w - hextra, 0);
    }
    if (max_w >= 0 && w > max_w) w = max_w;
    if (min_w >= 0 && w < min_w) w = min_w;
    if (w < 0) w = 0;
    const ns_css_value *hv = s->values[NS_CSS_HEIGHT];
    gboolean height_auto =
        !(hv && (hv->kind == NS_CSS_V_LENGTH || hv->kind == NS_CSS_V_CALC));
    if (height_auto && c->content_width > 0 && c->content_height > 0)
        c->content_height = w * c->content_height / c->content_width;
    c->content_width = w;
}

static void
flex_column_layout_item(ns_box *c, double cw, double line_w, gboolean fit,
                        const ns_style *child_inherited)
{
    const ns_css_value *wv = c->style ? c->style->values[NS_CSS_WIDTH] : NULL;
    gboolean width_explicit = wv &&
        (wv->kind == NS_CSS_V_LENGTH || wv->kind == NS_CSS_V_CALC ||
         size_keyword_is_intrinsic(wv));
    double parent_w = line_w;
    if (fit && !width_explicit) {
        double nat = measure_natural_width(c, child_inherited);
        double avail = line_w - c->margin.left - c->margin.right -
                       c->padding.left - c->padding.right -
                       c->border.left - c->border.right;
        if (nat > avail) nat = avail;
        if (nat < 0) nat = 0;
        parent_w = nat + c->margin.left + c->margin.right +
                   c->padding.left + c->padding.right +
                   c->border.left + c->border.right;
    }
    c->cb_width_override = cw;
    c->cb_width_set = TRUE;
    c->definite_height = 0;
    c->measured_content_height = -1;
    layout_box(c, parent_w, child_inherited);
    if (!fit && !width_explicit)
        flex_stretch_replaced_width(c, line_w);
}

static double
flex_item_outer_width(const ns_box *c)
{
    return c->content_width
        + c->padding.left + c->padding.right
        + c->border.left + c->border.right
        + c->margin.left + c->margin.right;
}

static double
definite_height_at_least(double h, double min_h)
{
    return h >= 0 && min_h > h ? min_h : h;
}

static double
flex_border_box_content_height(double h, double vex, gboolean is_content)
{
    return h > 0 && !is_content ? MAX(h - vex, 0) : h;
}

static gboolean
flex_item_min_height_is_content_based(const ns_box *c)
{
    const ns_css_value *mnh = c->style->values[NS_CSS_MIN_HEIGHT];
    if (mnh && (mnh->kind == NS_CSS_V_LENGTH || mnh->kind == NS_CSS_V_CALC))
        return FALSE;
    if (mnh && mnh->kind == NS_CSS_V_KEYWORD && !keyword_is(mnh, "auto"))
        return size_keyword_is_intrinsic(mnh);
    return !box_is_scroll_container(c);
}

static gboolean
flex_item_height_is_definite(const ns_box *c, double pct_basis)
{
    const ns_css_value *hv = c->style->values[NS_CSS_HEIGHT];
    if (height_keyword_stretches(hv)) return pct_basis >= 0;
    if (!hv || !(hv->kind == NS_CSS_V_LENGTH || hv->kind == NS_CSS_V_CALC))
        return FALSE;
    return !value_is_percent(hv) || pct_basis >= 0;
}

static gboolean
box_inside_indefinite_measure(const ns_box *c)
{
    for (const ns_box *p = c->parent; p; p = p->parent)
        if (p->block_size_indefinite) return TRUE;
    return FALSE;
}

static gboolean
flex_column_item_layout_deferrable(const ns_box *c, double pct_basis)
{
    if (c->kind != NS_BOX_BLOCK || !c->first_child || !c->style) return FALSE;
    if (!flex_item_height_is_definite(c, pct_basis)) return FALSE;
    if (size_keyword_is_intrinsic(c->style->values[NS_CSS_MAX_HEIGHT]))
        return FALSE;
    return !box_inside_indefinite_measure(c);
}

static double
flex_column_specified_outer(const ns_box *c, double cw, double pct_basis)
{
    const ns_css_value *hv = c->style->values[NS_CSS_HEIGHT];
    if (height_keyword_stretches(hv))
        return flex_item_stretch_main_height(c, pct_basis);
    return flex_main_height_outer(c, hv, cw, pct_basis);
}

static double
flex_column_deferred_basis(const ns_box *c, double cw, double pct_basis)
{
    gboolean exp = FALSE;
    double b = flex_basis_main_height(c, cw, pct_basis, &exp);
    if (exp) return b;
    if (height_keyword_stretches(c->style->values[NS_CSS_HEIGHT]))
        return flex_item_stretch_main_height(c, pct_basis);
    return -1;
}

static gboolean
flex_column_defer_item(ns_box *c, double cw, double pct_basis, ns_flex_len *l)
{
    if (!flex_column_item_layout_deferrable(c, pct_basis)) return FALSE;
    l->basis = flex_column_deferred_basis(c, cw, pct_basis);
    if (l->basis < 0) return FALSE;
    l->min = flex_item_min_height_is_content_based(c)
        ? 0 : flex_item_min_main_height(c, cw, pct_basis);
    l->max = flex_item_max_main_height(c, cw, pct_basis);
    l->grow = flex_grow_of(c);
    l->shrink = flex_shrink_of(c);
    l->deferred = TRUE;
    return TRUE;
}

static gboolean
flex_column_min_needs_content(const ns_box *c, const ns_flex_len *l,
                              double cw, double pct_basis)
{
    if (!flex_item_min_height_is_content_based(c)) return FALSE;
    return l->basis < flex_column_specified_outer(c, cw, pct_basis) - 0.01;
}

static double
flex_item_auto_min_within_max(const ns_box *c, double mn, double mx)
{
    if (mx < 0 || mn <= mx || !c->style) return mn;
    const ns_css_value *mnh = c->style->values[NS_CSS_MIN_HEIGHT];
    return !mnh || keyword_is(mnh, "auto") ? mx : mn;
}

static double
flex_column_item_lens(ns_box *c, double cw, double pct_basis, ns_flex_len *l)
{
    double vex = c->padding.top + c->padding.bottom +
                 c->border.top + c->border.bottom;
    gboolean exp = FALSE;
    double b = flex_basis_main_height(c, cw, pct_basis, &exp);
    if (!exp) b = c->content_height + vex;
    l->basis = b;
    l->max = flex_item_max_main_height(c, cw, pct_basis);
    l->min = flex_item_auto_min_within_max(
        c, flex_item_min_main_height(c, cw, pct_basis), l->max);
    l->grow = flex_grow_of(c);
    l->shrink = flex_shrink_of(c);
    double contrib = c->content_height + vex;
    return exp ? flex_clamp_main(b, l->min, l->max) : contrib;
}

static double
flex_column_items_used(const GPtrArray *items, const ns_flex_len *lens,
                       double row_gap)
{
    if (items->len == 0) return 0;
    double used = row_gap * (items->len - 1);
    for (guint i = 0; i < items->len; i++) {
        const ns_box *c = items->pdata[i];
        used += flex_clamp_main(lens[i].basis, lens[i].min, lens[i].max) +
                c->margin.top + c->margin.bottom;
    }
    return used;
}

static gboolean
flex_column_items_overflow(const GPtrArray *items, const ns_flex_len *lens,
                           double limit, double row_gap)
{
    if (limit < 0 || items->len == 0) return FALSE;
    return flex_column_items_used(items, lens, row_gap) > limit + 0.5;
}

static void
box_tree_forget_definite_heights(ns_box *b)
{
    for (ns_box *c = b->first_child; c; c = c->next_sibling) {
        c->definite_height = 0;
        box_tree_forget_definite_heights(c);
    }
    if (!b->inline_atomics) return;
    for (guint i = 0; i < b->inline_atomics->len; i++) {
        ns_box *ab = g_array_index(b->inline_atomics, ns_inline_atomic, i).box;
        if (!ab) continue;
        ab->definite_height = 0;
        box_tree_forget_definite_heights(ab);
    }
}

static void
flex_column_layout_indefinite(ns_box *c, double cw, gboolean fit,
                              const ns_style *child_inherited)
{
    box_tree_forget_definite_heights(c);
    c->block_size_indefinite = TRUE;
    flex_column_layout_item(c, cw, cw, fit, child_inherited);
    c->block_size_indefinite = FALSE;
    c->definite_height = -1;
    c->definite_height_read = TRUE;
}

typedef struct {
    double cw;
    double pct_basis;
    double limit;
    double row_gap;
    gboolean multi_line;
    const char *align;
    const ns_style *child_inherited;
} ns_flex_column_pass;

static void
flex_column_lay_out_deferred(ns_box *c, ns_flex_len *l, double *contrib,
                             gboolean indefinite, const ns_flex_column_pass *x)
{
    gboolean fit = x->multi_line || flex_column_item_shrinks_to_fit(c, x->align);
    if (indefinite)
        flex_column_layout_indefinite(c, x->cw, fit, x->child_inherited);
    else
        flex_column_layout_item(c, x->cw, x->cw, fit, x->child_inherited);
    double basis = l->basis;
    *contrib = flex_column_item_lens(c, x->cw, x->pct_basis, l);
    if (indefinite) l->basis = basis;
    l->deferred = FALSE;
    l->remeasured = indefinite;
}

static void
flex_column_lay_out_deferred_items(GPtrArray *items, ns_flex_len *lens,
                                   GArray *contribs,
                                   const ns_flex_column_pass *x)
{
    for (guint i = 0; i < items->len; i++) {
        ns_box *c = items->pdata[i];
        if (lens[i].deferred &&
            flex_column_min_needs_content(c, &lens[i], x->cw, x->pct_basis))
            flex_column_lay_out_deferred(c, &lens[i],
                                         &g_array_index(contribs, double, i),
                                         TRUE, x);
    }
    gboolean overflow = flex_column_items_overflow(items, lens, x->limit,
                                                   x->row_gap);
    for (guint i = 0; i < items->len; i++) {
        if (!lens[i].deferred) continue;
        flex_column_lay_out_deferred(items->pdata[i], &lens[i],
                                     &g_array_index(contribs, double, i),
                                     overflow && lens[i].shrink > 0, x);
    }
}

static double
flex_column_inset_height(const ns_box *box)
{
    const ns_style *s = box->style;
    if (!s || !style_is_absolute_or_fixed(s) || box->content_height <= 0)
        return -1;
    if (s->values[NS_CSS_HEIGHT] ||
        (s->values[NS_CSS_TOP] && s->values[NS_CSS_BOTTOM]))
        return box->content_height;
    return -1;
}

static double
box_aspect_ratio_height(const ns_box *box, double cw)
{
    if (!box->style || cw <= 0) return -1;
    double ratio = aspect_ratio_number(box->style->values[NS_CSS_ASPECT_RATIO],
                                       NULL);
    return ratio > 0 ? cw / ratio : -1;
}

static double
flex_column_own_height(ns_box *box, double cw, gboolean *is_content,
                       gboolean *from_ratio)
{
    *is_content = TRUE;
    *from_ratio = FALSE;
    if (box_height_flexed_in_column(box)) return box->definite_height;
    double h = box_style_definite_height(box, cw);
    if (h < 0) h = flex_column_inset_height(box);
    if (h < 0) {
        h = box_aspect_ratio_height(box, cw);
        *from_ratio = h >= 0;
    }
    if (h < 0 && box_read_definite_height(box) > 0) return box->definite_height;
    *is_content = FALSE;
    return h;
}

static gboolean
box_ratio_height_floors_at_content(const ns_box *box)
{
    const ns_css_value *mnh = box->style->values[NS_CSS_MIN_HEIGHT];
    if (mnh && (mnh->kind == NS_CSS_V_LENGTH || mnh->kind == NS_CSS_V_CALC))
        return FALSE;
    if (overflow_kw_scrolls(overflow_axis_keyword(box->style, NS_CSS_OVERFLOW_Y)))
        return FALSE;
    return !keyword_is(box->style->values[NS_CSS_OVERFLOW_Y], "hidden");
}

static double
flex_column_ratio_min_height(const ns_box *box, const GPtrArray *items,
                             const ns_flex_len *lens, double row_gap,
                             double min_h, double max_h)
{
    if (flex_wraps(box->style) || !box_ratio_height_floors_at_content(box))
        return min_h;
    double used = flex_column_items_used(items, lens, row_gap);
    if (max_h >= 0 && used > max_h) used = max_h;
    return used > min_h ? used : min_h;
}

static void
flex_column_relayout_remeasured(GPtrArray *items, const ns_flex_len *lens,
                                const ns_style *child_inherited)
{
    for (guint i = 0; i < items->len; i++) {
        ns_box *c = items->pdata[i];
        if (!lens[i].remeasured || c->definite_height >= 0) continue;
        double target_h = lens[i].target - c->padding.top -
                          c->padding.bottom - c->border.top - c->border.bottom;
        relayout_at_definite_height(c, flex_item_outer_width(c),
                                    MAX(target_h, 0), child_inherited);
    }
}

static void
layout_flex_column(ns_box *box, double cw,
                   double inner_x, double inner_y,
                   const ns_style *child_inherited,
                   gboolean reverse,
                   double parent_content_height,
                   double *cursor_y_out)
{
    GPtrArray *items = g_ptr_array_new();
    for (ns_box *c = box->first_child; c; c = c->next_sibling)
        if (!style_is_absolute_or_fixed(c->style))
            g_ptr_array_add(items, c);

    const char *align = keyword_or(box->style, NS_CSS_ALIGN_ITEMS, "stretch");
    const char *justify = keyword_or(box->style, NS_CSS_JUSTIFY_CONTENT, "flex-start");

    const ns_css_value *mnh = box->style ? box->style->values[NS_CSS_MIN_HEIGHT] : NULL;
    const ns_css_value *mxh = box->style ? box->style->values[NS_CSS_MAX_HEIGHT] : NULL;
    gboolean explicit_is_content = FALSE;
    gboolean explicit_from_ratio = FALSE;
    double explicit_h = flex_column_own_height(box, cw, &explicit_is_content,
                                               &explicit_from_ratio);
    double row_gap = flex_gap_row_of(box->style,
                                     explicit_h > 0 ? explicit_h : 0);
    double col_gap = flex_gap_of(box->style, cw);
    double min_h = resolve_used_height(box, mnh, parent_content_height, -1);
    double max_h = resolve_used_height(box, mxh, parent_content_height, -1);
    if (keyword_is(box->style ? box->style->values[NS_CSS_BOX_SIZING] : NULL,
                   "border-box")) {
        double vex = box->border.top + box->border.bottom +
                     box->padding.top + box->padding.bottom;
        explicit_h = flex_border_box_content_height(explicit_h, vex,
                                                    explicit_is_content);
        if (min_h > 0) min_h = MAX(min_h - vex, 0);
        if (max_h >= 0) max_h = MAX(max_h - vex, 0);
    }
    double percentage_basis_h = explicit_h;
    explicit_h = definite_height_at_least(explicit_h, min_h);
    if (max_h >= 0 && explicit_h > max_h) explicit_h = max_h;
    if (explicit_h > 0) box->definite_height = explicit_h;

    double line_limit = explicit_h > 0 ? explicit_h : max_h;
    gboolean multi_line = flex_wraps(box->style);
    gboolean wraps = multi_line && line_limit > 0;
    gboolean wrap_reverse =
        keyword_is(box->style ? box->style->values[NS_CSS_FLEX_WRAP] : NULL,
                   "wrap-reverse");
    gboolean rtl = strcmp(keyword_or(box->style, NS_CSS_DIRECTION, "ltr"),
                          "rtl") == 0;
    gboolean cross_start_right = wrap_reverse != rtl;

    ns_flex_len *lens = g_new0(ns_flex_len, items->len + 1);
    GArray *contribs = g_array_new(FALSE, TRUE, sizeof(double));
    g_array_set_size(contribs, items->len);
    for (guint i = 0; i < items->len; i++) {
        ns_box *c = items->pdata[i];
        edges_from_style(c->style, cw, &c->margin, &c->padding, &c->border);
        c->x = inner_x;
        c->y = inner_y;
        if (flex_column_defer_item(c, cw, percentage_basis_h, &lens[i]))
            continue;
        flex_column_layout_item(c, cw, cw,
                                multi_line ||
                                flex_column_item_shrinks_to_fit(c, align),
                                child_inherited);
        g_array_index(contribs, double, i) =
            flex_column_item_lens(c, cw, percentage_basis_h, &lens[i]);
    }
    ns_flex_column_pass pass = {
        .cw = cw, .pct_basis = percentage_basis_h, .limit = line_limit,
        .row_gap = row_gap, .multi_line = multi_line, .align = align,
        .child_inherited = child_inherited,
    };
    flex_column_lay_out_deferred_items(items, lens, contribs, &pass);
    if (explicit_from_ratio)
        min_h = flex_column_ratio_min_height(box, items, lens, row_gap,
                                             min_h, max_h);

    typedef struct { guint start, count; double cross, x; } col_line;
    GArray *lines = g_array_new(FALSE, FALSE, sizeof(col_line));
    guint i = 0;
    while (i < items->len) {
        col_line ln = { .start = i, .count = 0, .cross = 0, .x = inner_x };
        double used = 0;
        for (; i < items->len; i++) {
            ns_box *c = items->pdata[i];
            double outer = flex_clamp_main(lens[i].basis, lens[i].min, lens[i].max)
                         + c->margin.top + c->margin.bottom;
            double try_used = used + (ln.count > 0 ? row_gap : 0) + outer;
            if (wraps && try_used > line_limit + 0.5 && ln.count > 0) break;
            used = try_used;
            ln.count++;
            double w = flex_item_outer_width(c);
            if (w > ln.cross) ln.cross = w;
        }
        g_array_append_val(lines, ln);
        if (ln.count == 0) break;
    }

    double lines_cross = 0;
    for (guint li = 0; li < lines->len; li++)
        lines_cross += g_array_index(lines, col_line, li).cross;
    if (lines->len > 1) lines_cross += col_gap * (lines->len - 1);
    if (lines->len == 0) {
        col_line empty = { .start = 0, .count = 0, .cross = cw, .x = inner_x };
        g_array_append_val(lines, empty);
    }
    if (!multi_line) {
        g_array_index(lines, col_line, 0).cross = cw;
    } else {
        double lead, between_lines, per_line;
        flex_align_content_offsets(box, cw - lines_cross, lines->len,
                                   &lead, &between_lines, &per_line);
        double x = inner_x + lead;
        for (guint li = 0; li < lines->len; li++) {
            col_line *ln = &g_array_index(lines, col_line, li);
            ln->cross += per_line;
            ln->x = x;
            x += ln->cross + col_gap + between_lines;
        }
        if (cross_start_right)
            for (guint li = 0; li < lines->len; li++) {
                col_line *ln = &g_array_index(lines, col_line, li);
                ln->x = inner_x + cw - (ln->x - inner_x) - ln->cross;
            }
    }

    double main_extent = 0;
    for (guint li = 0; li < lines->len; li++) {
        col_line *ln = &g_array_index(lines, col_line, li);
        double gaps = ln->count > 1 ? row_gap * (ln->count - 1) : 0;
        double margins = 0;
        double sum_hyp = 0;
        int auto_margins = 0;
        for (guint k = 0; k < ln->count; k++) {
            ns_box *c = items->pdata[ln->start + k];
            margins += c->margin.top + c->margin.bottom;
            sum_hyp += flex_clamp_main(lens[ln->start + k].basis,
                                       lens[ln->start + k].min,
                                       lens[ln->start + k].max);
            if (c->style) {
                if (keyword_is(c->style->values[NS_CSS_MARGIN_TOP], "auto")) auto_margins++;
                if (keyword_is(c->style->values[NS_CSS_MARGIN_BOTTOM], "auto")) auto_margins++;
            }
        }
        double avail = explicit_h > 0 ? explicit_h : sum_hyp + margins + gaps;
        if (explicit_h <= 0) {
            double fraction = 0;
            for (guint k = 0; k < ln->count; k++) {
                const ns_flex_len *l = &lens[ln->start + k];
                double contrib = g_array_index(contribs, double, ln->start + k);
                double diff = contrib - l->basis;
                double f = diff > 0 ? diff / MAX(l->grow, 1.0)
                         : (l->shrink * l->basis > 0 ? diff / (l->shrink * l->basis) : 0);
                if (f > fraction) fraction = f;
            }
            double sum = margins + gaps;
            for (guint k = 0; k < ln->count; k++) {
                const ns_flex_len *l = &lens[ln->start + k];
                double size = l->basis + fraction * l->grow;
                sum += flex_clamp_main(size, l->min, l->max);
            }
            if (sum > avail) avail = sum;
        }
        if (max_h >= 0 && avail > max_h) avail = max_h;
        if (avail < min_h) avail = min_h;
        flex_resolve_lengths(lens + ln->start, ln->count, avail - margins - gaps);
        double free_main = avail - margins - gaps;
        for (guint k = 0; k < ln->count; k++)
            free_main -= lens[ln->start + k].target;
        double leading = 0, between = 0;
        if (auto_margins > 0 && free_main > 0) {
            double share = free_main / auto_margins;
            for (guint k = 0; k < ln->count; k++) {
                ns_box *c = items->pdata[ln->start + k];
                if (!c->style) continue;
                if (keyword_is(c->style->values[NS_CSS_MARGIN_TOP], "auto")) {
                    c->margin.top += share;
                    shift_box_tree(c, 0, share);
                    c->y -= share;
                }
                if (keyword_is(c->style->values[NS_CSS_MARGIN_BOTTOM], "auto"))
                    c->margin.bottom += share;
            }
            free_main = 0;
        } else {
            flex_justify_offsets(box, justify, free_main, ln->count, reverse,
                                 &leading, &between);
        }
        if (avail > main_extent) main_extent = avail;

        double cursor_y = reverse ? inner_y + avail - leading : inner_y + leading;
        for (guint k = 0; k < ln->count; k++) {
            guint idx = ln->start + k;
            ns_box *c = items->pdata[idx];
            double main_size = lens[idx].target;
            double vextra = c->padding.top + c->padding.bottom +
                            c->border.top + c->border.bottom;
            const ns_css_value *mlv = c->style ? c->style->values[NS_CSS_MARGIN_LEFT] : NULL;
            const ns_css_value *mrv = c->style ? c->style->values[NS_CSS_MARGIN_RIGHT] : NULL;
            const ns_css_value *wv = c->style ? c->style->values[NS_CSS_WIDTH] : NULL;
            gboolean ml_auto = length_is_auto(mlv);
            gboolean mr_auto = length_is_auto(mrv);
            gboolean width_explicit = wv &&
                (wv->kind == NS_CSS_V_LENGTH || wv->kind == NS_CSS_V_CALC ||
                 size_keyword_is_intrinsic(wv));
            const char *eff_align = flex_column_item_align(c, align);
            gboolean stretches = !ml_auto && !mr_auto && !width_explicit &&
                (strcmp(eff_align, "stretch") == 0 ||
                 strcmp(eff_align, "normal") == 0);
            gboolean at_right = FALSE;
            gboolean centered = strcmp(eff_align, "center") == 0;
            if (strcmp(eff_align, "flex-end") == 0)
                at_right = !cross_start_right;
            else if (strcmp(eff_align, "end") == 0 ||
                     strcmp(eff_align, "self-end") == 0)
                at_right = !rtl;
            else if (strcmp(eff_align, "start") == 0 ||
                     strcmp(eff_align, "self-start") == 0)
                at_right = rtl;
            else if (strcmp(eff_align, "right") == 0)
                at_right = TRUE;
            else if (strcmp(eff_align, "left") == 0)
                at_right = FALSE;
            else if (!centered)
                at_right = cross_start_right;
            if (stretches &&
                fabs(flex_item_outer_width(c) - ln->cross) > 0.01) {
                c->x = inner_x;
                c->y = inner_y;
                flex_column_layout_item(c, cw, ln->cross, FALSE, child_inherited);
            }
            double item_outer_w = flex_item_outer_width(c);
            double cx = ln->x;
            if (ml_auto || mr_auto) {
                double free_cross = MAX(ln->cross - item_outer_w, 0);
                cx = ln->x + (ml_auto && mr_auto ? free_cross / 2.0
                              : ml_auto      ? free_cross : 0);
            } else if (centered)
                cx = ln->x + (ln->cross - item_outer_w) / 2.0;
            else if (at_right)
                cx = ln->x + ln->cross - item_outer_w;
            double outer_main = main_size + c->margin.top + c->margin.bottom;
            if (reverse) cursor_y -= outer_main;
            double dx = cx - c->x;
            double dy = cursor_y - c->y;
            if (dx != 0 || dy != 0) shift_box_tree(c, dx, dy);
            c->x = cx;
            c->y = cursor_y;

            double target_h = main_size - vextra;
            if (target_h < 0) target_h = 0;
            if (fabs(target_h - c->content_height) > 0.01) {
                double natural_h = c->content_height;
                gboolean shrank = target_h < natural_h;
                c->content_height = target_h;
                const char *covy = c->style
                    ? overflow_axis_keyword(c->style, NS_CSS_OVERFLOW_Y) : NULL;
                if (shrank && overflow_kw_scrolls(covy)) {
                    c->scrolls = TRUE;
                    c->scroll_max_y = natural_h - target_h;
                    if (c->scroll_max_y < 0) c->scroll_max_y = 0;
                }
                if (c->first_child && c->definite_height != target_h) {
                    double relayout_w = stretches ? ln->cross : item_outer_w;
                    gboolean reuse = !c->definite_height_read &&
                                     c->last_layout_width == relayout_w;
                    if (reuse) {
                        c->definite_height = target_h;
                        c->content_height = target_h;
                    } else {
                        relayout_at_definite_height(c, relayout_w, target_h,
                                                    child_inherited);
                    }
                }
            }
            if (reverse) cursor_y -= row_gap + between;
            else         cursor_y += outer_main + row_gap + between;
        }
    }

    flex_column_relayout_remeasured(items, lens, child_inherited);
    *cursor_y_out = inner_y + main_extent;
    g_free(lens);
    g_array_free(contribs, TRUE);
    g_array_free(lines, TRUE);
    g_ptr_array_free(items, TRUE);
}

static double
track_min_px(const ns_css_track *t, double avail)
{
    if (!t->has_min) return 0;
    switch (t->min_kind) {
    case NS_CSS_TRACK_PX:      return t->min_v + t->min_pct * avail / 100.0;
    case NS_CSS_TRACK_PERCENT: return t->min_v * avail / 100.0;
    default: return 0;
    }
}

static gboolean
justify_kw_stretches_auto(const char *jc)
{
    return strcmp(jc, "normal") == 0 || strcmp(jc, "stretch") == 0;
}

static gboolean
track_is_intrinsic(ns_css_track_kind k)
{
    return k == NS_CSS_TRACK_AUTO || k == NS_CSS_TRACK_MIN_CONTENT ||
           k == NS_CSS_TRACK_MAX_CONTENT;
}

static gboolean
fr_track_min_is_auto(const ns_css_track *t)
{
    return t->kind == NS_CSS_TRACK_FR &&
           (!t->has_min || track_is_intrinsic(t->min_kind));
}

static double
grid_flex_track_sizes(const ns_css_tracks *tr, double space,
                      double available_main, const double *content_min,
                      double *sizes)
{
    gboolean inflexible[NS_CSS_TRACKS_MAX] = {0};
    double base[NS_CSS_TRACKS_MAX] = {0};
    for (int i = 0; i < tr->n; i++) {
        if (tr->tracks[i].kind != NS_CSS_TRACK_FR) continue;
        base[i] = track_min_px(&tr->tracks[i], available_main);
        if (content_min && fr_track_min_is_auto(&tr->tracks[i]))
            base[i] = MAX(base[i], content_min[i]);
    }
    double fr = 0;
    for (int pass = 0; pass <= tr->n; pass++) {
        double leftover = space, flex_sum = 0;
        for (int i = 0; i < tr->n; i++) {
            if (tr->tracks[i].kind != NS_CSS_TRACK_FR) continue;
            if (inflexible[i]) leftover -= base[i];
            else flex_sum += MAX(tr->tracks[i].v, 0);
        }
        fr = flex_sum > 0 ? leftover / MAX(flex_sum, 1.0) : 0;
        gboolean changed = FALSE;
        for (int i = 0; i < tr->n; i++) {
            if (tr->tracks[i].kind != NS_CSS_TRACK_FR || inflexible[i]) continue;
            if (fr * MAX(tr->tracks[i].v, 0) < base[i]) {
                inflexible[i] = TRUE;
                changed = TRUE;
            }
        }
        if (!changed) break;
    }
    double used = 0;
    for (int i = 0; i < tr->n; i++) {
        if (tr->tracks[i].kind != NS_CSS_TRACK_FR) continue;
        sizes[i] = inflexible[i] ? base[i]
                                 : MAX(base[i], fr * MAX(tr->tracks[i].v, 0));
        used += sizes[i];
    }
    return used;
}

static void
resolve_track_sizes_full(const ns_css_tracks *tr, double available_main,
                         double pct_basis,
                         const double *content_min, const double *content_max,
                         double *out_sizes, gboolean stretch_auto)
{
    double total_fixed = 0;
    double total_fr    = 0;
    double total_shrink = 0;
    double fixed_px[NS_CSS_TRACKS_MAX] = {0};
    double shrink_px[NS_CSS_TRACKS_MAX] = {0};
    int    n_auto      = 0;
    for (int i = 0; i < tr->n; i++) {
        const ns_css_track *t = &tr->tracks[i];
        double fixed = 0;
        switch (t->kind) {
        case NS_CSS_TRACK_PX:
        case NS_CSS_TRACK_PERCENT:
            fixed = t->kind == NS_CSS_TRACK_PX
                ? t->v + t->pct * pct_basis / 100.0
                : t->v * pct_basis / 100.0;
            if (t->fit_content)
                fixed = MIN(fixed, content_max ? content_max[i] : 0);
            if (t->has_min && track_is_intrinsic(t->min_kind) &&
                content_min && content_min[i] > fixed)
                fixed = content_min[i];
            fixed_px[i] = fixed;
            total_fixed += fixed;
            break;
        case NS_CSS_TRACK_FR:      total_fr += t->v > 0 ? t->v : 0; break;
        case NS_CSS_TRACK_AUTO:    n_auto++; break;
        case NS_CSS_TRACK_MIN_CONTENT:
        case NS_CSS_TRACK_MAX_CONTENT:
            break;
        }
        if ((t->kind == NS_CSS_TRACK_PX || t->kind == NS_CSS_TRACK_PERCENT) &&
            t->has_min) {
            double mn = track_min_px(t, pct_basis);
            if (fixed > mn) {
                shrink_px[i] = fixed - mn;
                total_shrink += shrink_px[i];
            }
        }
    }
    double auto_base[NS_CSS_TRACKS_MAX] = {0};
    double auto_lim[NS_CSS_TRACKS_MAX]  = {0};
    if (content_min) {
        double base_sum = 0, auto_sum = 0, content_sum = 0;
        for (int i = 0; i < tr->n; i++) {
            if (!track_is_intrinsic(tr->tracks[i].kind)) continue;
            auto_base[i] = content_min[i] > 0 ? content_min[i] : 0;
            double lim = content_max ? content_max[i] : auto_base[i];
            auto_lim[i] = lim > auto_base[i] ? lim : auto_base[i];
            if (tr->tracks[i].kind == NS_CSS_TRACK_MAX_CONTENT)
                auto_base[i] = auto_lim[i];
            base_sum += auto_base[i];
            if (tr->tracks[i].kind == NS_CSS_TRACK_AUTO)
                auto_sum += auto_base[i];
            else
                content_sum += auto_base[i];
        }
        double free_for_auto = available_main - (total_fixed - total_shrink);
        if (free_for_auto < 0) free_for_auto = 0;
        double room = free_for_auto - content_sum;
        if (room < 0) room = 0;
        if (auto_sum > room && auto_sum > 0) {
            double scale = room / auto_sum;
            for (int i = 0; i < tr->n; i++) {
                if (tr->tracks[i].kind != NS_CSS_TRACK_AUTO) continue;
                auto_base[i] *= scale;
                if (auto_lim[i] < auto_base[i]) auto_lim[i] = auto_base[i];
            }
            base_sum = content_sum + room;
        }
        total_fixed += base_sum;
    }

    double fr_min_total = 0;
    for (int i = 0; i < tr->n; i++) {
        if (tr->tracks[i].kind != NS_CSS_TRACK_FR) continue;
        double mn = track_min_px(&tr->tracks[i], pct_basis);
        if (content_min && fr_track_min_is_auto(&tr->tracks[i]))
            mn = MAX(mn, content_min[i]);
        fr_min_total += mn;
    }

    double shrink_used = 0;
    if (total_fixed + fr_min_total > available_main && total_shrink > 0) {
        shrink_used = total_fixed + fr_min_total - available_main;
        if (shrink_used > total_shrink) shrink_used = total_shrink;
        total_fixed -= shrink_used;
    }

    double remaining = available_main - total_fixed;
    if (remaining < 0) remaining = 0;

    double auto_grow[NS_CSS_TRACKS_MAX] = {0};
    if (n_auto > 0 && remaining > fr_min_total && content_min) {
        double room_total = 0;
        for (int i = 0; i < tr->n; i++) {
            if (tr->tracks[i].kind != NS_CSS_TRACK_AUTO) continue;
            double room = auto_lim[i] - auto_base[i];
            if (room > 0) room_total += room;
        }
        if (room_total > 0) {
            double free_space = remaining - fr_min_total;
            double give = free_space < room_total ? free_space : room_total;
            for (int i = 0; i < tr->n; i++) {
                if (tr->tracks[i].kind != NS_CSS_TRACK_AUTO) continue;
                double room = auto_lim[i] - auto_base[i];
                if (room > 0) auto_grow[i] = give * (room / room_total);
            }
            remaining -= give;
        }
    }

    double fr_sizes[NS_CSS_TRACKS_MAX] = {0};
    double fr_used = total_fr > 0
        ? grid_flex_track_sizes(tr, remaining, pct_basis, content_min,
                                fr_sizes) : 0;
    double per_auto = 0;
    if (n_auto > 0 && stretch_auto && remaining - fr_used > 0)
        per_auto = (remaining - fr_used) / n_auto;

    for (int i = 0; i < tr->n; i++) {
        const ns_css_track *t = &tr->tracks[i];
        switch (t->kind) {
        case NS_CSS_TRACK_PX:
        case NS_CSS_TRACK_PERCENT:
            out_sizes[i] = fixed_px[i];
            if (shrink_used > 0 && shrink_px[i] > 0)
                out_sizes[i] -= shrink_used * (shrink_px[i] / total_shrink);
            break;
        case NS_CSS_TRACK_FR:      out_sizes[i] = fr_sizes[i]; break;
        case NS_CSS_TRACK_AUTO:
            out_sizes[i] = auto_base[i] + auto_grow[i] + per_auto;
            break;
        case NS_CSS_TRACK_MIN_CONTENT:
            out_sizes[i] = auto_base[i];
            break;
        case NS_CSS_TRACK_MAX_CONTENT:
            out_sizes[i] = auto_lim[i] > auto_base[i] ? auto_lim[i] : auto_base[i];
            break;
        }
        double mn = track_min_px(t, pct_basis);
        if (out_sizes[i] < mn) out_sizes[i] = mn;
        if (out_sizes[i] < 0) out_sizes[i] = 0;
    }
}

static double
grid_track_repeat_px(const ns_css_track *t, double available_main)
{
    double min_px = track_min_px(t, available_main);
    if (t->kind == NS_CSS_TRACK_PX || t->kind == NS_CSS_TRACK_PERCENT) {
        double max_px = t->kind == NS_CSS_TRACK_PX
            ? t->v + t->pct * available_main / 100.0
            : t->v * available_main / 100.0;
        return max_px > min_px ? max_px : min_px;
    }
    return min_px;
}

static void
grid_line_name_copy(ns_css_tracks *out, const ns_css_line_name *ln, int line)
{
    if (out->n_line_names >= NS_CSS_LINE_NAMES_MAX) return;
    ns_css_line_name *dst = &out->line_names[out->n_line_names++];
    *dst = *ln;
    dst->line = line;
}

static void
grid_expand_repeat_names(const ns_css_tracks *tr, int repeats,
                         ns_css_tracks *out)
{
    int first = tr->auto_repeat_names_start;
    int last = tr->auto_repeat_names_end;
    int shift = (repeats - 1) * tr->auto_repeat_count;
    out->n_line_names = 0;
    for (int i = 0; i < first; i++)
        grid_line_name_copy(out, &tr->line_names[i], tr->line_names[i].line);
    for (int r = 0; r < repeats; r++)
        for (int i = first; i < last; i++)
            grid_line_name_copy(out, &tr->line_names[i],
                                tr->line_names[i].line +
                                r * tr->auto_repeat_count);
    for (int i = last; i < tr->n_line_names; i++)
        grid_line_name_copy(out, &tr->line_names[i],
                            tr->line_names[i].line + shift);
}

static ns_css_tracks
expand_auto_repeat_ex(const ns_css_tracks *tr, double available_main, double gap,
                      int *fit_start, int *fit_count)
{
    ns_css_tracks out = *tr;
    if (fit_start) *fit_start = 0;
    if (fit_count) *fit_count = 0;
    if (tr->auto_repeat == NS_CSS_AUTO_REPEAT_NONE) return out;
    if (tr->auto_repeat_count <= 0) return out;
    if (tr->auto_repeat_start < 0 || tr->auto_repeat_start >= tr->n) return out;
    ns_css_tracks clamped;
    if (tr->auto_repeat_count > tr->n - tr->auto_repeat_start) {
        clamped = *tr;
        clamped.auto_repeat_count = tr->n - tr->auto_repeat_start;
        tr = &clamped;
    }

    double base_min = 0;
    for (int i = 0; i < tr->auto_repeat_count; i++) {
        const ns_css_track *t = &tr->tracks[tr->auto_repeat_start + i];
        double m = grid_track_repeat_px(t, available_main);
        if (m <= 0) {
            memset(&out, 0, sizeof(out));
            out.n = 1;
            out.tracks[0].kind = NS_CSS_TRACK_AUTO;
            return out;
        }
        base_min += m;
    }
    if (base_min <= 0) return out;
    double others = 0;
    int n_others = tr->n - tr->auto_repeat_count;
    for (int i = 0; i < tr->n; i++) {
        if (i >= tr->auto_repeat_start &&
            i < tr->auto_repeat_start + tr->auto_repeat_count)
            continue;
        double m = grid_track_repeat_px(&tr->tracks[i], available_main);
        if (m > 0) others += m;
    }
    double pattern_with_gap = base_min + gap * tr->auto_repeat_count;
    double room = available_main - others - (n_others - 1) * gap;
    int repeats = 1;
    if (pattern_with_gap > 0)
        repeats = (int)(room / pattern_with_gap);
    if (repeats < 1) repeats = 1;
    if (repeats > NS_CSS_TRACKS_MAX) repeats = NS_CSS_TRACKS_MAX;

    int prefix = tr->auto_repeat_start;
    int suffix_start = tr->auto_repeat_start + tr->auto_repeat_count;
    int suffix_count = tr->n - suffix_start;
    int total = prefix + repeats * tr->auto_repeat_count + suffix_count;
    if (total > NS_CSS_TRACKS_MAX)
        repeats = (NS_CSS_TRACKS_MAX - prefix - suffix_count) /
                  tr->auto_repeat_count;
    if (repeats < 1) repeats = 1;

    out.n = 0;
    for (int i = 0; i < prefix && out.n < NS_CSS_TRACKS_MAX; i++)
        out.tracks[out.n++] = tr->tracks[i];
    for (int r = 0; r < repeats; r++) {
        for (int i = 0; i < tr->auto_repeat_count &&
                        out.n < NS_CSS_TRACKS_MAX; i++)
            out.tracks[out.n++] = tr->tracks[tr->auto_repeat_start + i];
    }
    for (int i = 0; i < suffix_count && out.n < NS_CSS_TRACKS_MAX; i++)
        out.tracks[out.n++] = tr->tracks[suffix_start + i];
    grid_expand_repeat_names(tr, repeats, &out);
    out.auto_repeat = NS_CSS_AUTO_REPEAT_NONE;
    if (tr->auto_repeat == NS_CSS_AUTO_REPEAT_FIT) {
        if (fit_start) *fit_start = prefix;
        if (fit_count) *fit_count = repeats * tr->auto_repeat_count;
    }
    return out;
}

typedef struct grid_lines {
    const ns_css_tracks *tracks;
    const ns_css_areas  *areas;
    gboolean row_axis;
} grid_lines;

static gint
grid_span_order(gconstpointer a, gconstpointer b, gpointer data)
{
    const GArray *spans = data;
    guint ia = *(const guint *)a, ib = *(const guint *)b;
    int sa = MAX(g_array_index(spans, int, ia), 1);
    int sb = MAX(g_array_index(spans, int, ib), 1);
    if (sa != sb) return sa < sb ? -1 : 1;
    return ia < ib ? -1 : ia > ib ? 1 : 0;
}

static gboolean
grid_row_below_limit(const double *height, const double *limit, int k)
{
    return limit[k] < 0 || height[k] < limit[k] - 0.01;
}

static double
grid_spread_to_limits(double *height, const gboolean *target,
                      const double *limit, int n, double extra)
{
    for (int round = 0; round < n && extra > 0.01; round++) {
        int open = 0;
        for (int k = 0; k < n; k++)
            if (target[k] && grid_row_below_limit(height, limit, k)) open++;
        if (!open) break;
        double share = extra / open;
        for (int k = 0; k < n; k++) {
            if (!target[k] || !grid_row_below_limit(height, limit, k)) continue;
            double add = limit[k] < 0 ? share : MIN(share, limit[k] - height[k]);
            height[k] += add;
            extra -= add;
        }
    }
    return extra;
}

static void
grid_spread_beyond_limits(double *height, const gboolean *target,
                          const gboolean *max_intrinsic, int n, double extra)
{
    gboolean any_max = FALSE;
    for (int k = 0; k < n; k++)
        if (target[k] && max_intrinsic[k]) any_max = TRUE;
    int beyond = 0;
    for (int k = 0; k < n; k++)
        if (target[k] && (max_intrinsic[k] || !any_max)) beyond++;
    for (int k = 0; k < n && beyond; k++)
        if (target[k] && (max_intrinsic[k] || !any_max))
            height[k] += extra / beyond;
}

static void
grid_distribute_span(double *height, const gboolean *fixed,
                     const double *limit, const gboolean *min_intrinsic,
                     const gboolean *max_intrinsic, int n, double extra)
{
    gboolean any_min = FALSE;
    for (int k = 0; k < n; k++)
        if (!fixed[k] && min_intrinsic[k]) any_min = TRUE;
    gboolean *target = g_new(gboolean, n);
    for (int k = 0; k < n; k++)
        target[k] = !fixed[k] && (min_intrinsic[k] || !any_min);
    extra = grid_spread_to_limits(height, target, limit, n, extra);
    if (extra > 0.01)
        grid_spread_beyond_limits(height, target, max_intrinsic, n, extra);
    g_free(target);
}

static GArray *
grid_items_by_span(GArray *spans, guint n)
{
    GArray *order = g_array_sized_new(FALSE, FALSE, sizeof(guint), n);
    for (guint i = 0; i < n; i++) g_array_append_val(order, i);
    g_array_sort_with_data(order, grid_span_order, spans);
    return order;
}

static const grid_lines *g_grid_lines;

static int
grid_line_name_with_suffix(const ns_css_tracks *tracks, const char *name,
                           gsize len, const char *suffix)
{
    gsize slen = strlen(suffix);
    int best = 0;
    for (int i = 0; tracks && i < tracks->n_line_names; i++) {
        const ns_css_line_name *ln = &tracks->line_names[i];
        if (strlen(ln->name) != len + slen) continue;
        if (strncmp(ln->name, name, len) != 0) continue;
        if (strcmp(ln->name + len, suffix) != 0) continue;
        if (!best || ln->line < best) best = ln->line;
    }
    return best;
}

static int
grid_area_rect_line(const grid_lines *gl, const char *name, gsize len,
                    gboolean end_side)
{
    int best = 0;
    for (int i = 0; gl->areas && i < gl->areas->n_rects; i++) {
        const ns_css_area_rect *a = &gl->areas->rects[i];
        if (!a->name || strlen(a->name) != len ||
            strncmp(a->name, name, len) != 0)
            continue;
        int line = gl->row_axis ? (end_side ? a->r1 + 2 : a->r0 + 1)
                                : (end_side ? a->c1 + 2 : a->c0 + 1);
        if (!best || line < best) best = line;
    }
    return best;
}

static int
grid_area_edge_line(const grid_lines *gl, const char *name, gsize len,
                    gboolean end_side)
{
    int named = grid_line_name_with_suffix(gl->tracks, name, len,
                                           end_side ? "-end" : "-start");
    int area = grid_area_rect_line(gl, name, len, end_side);
    if (!named) return area;
    if (!area) return named;
    return MIN(named, area);
}

static int
grid_named_line(const char *name, gsize len, int after, gboolean end_side)
{
    const grid_lines *gl = g_grid_lines;
    if (!gl || !name || !len) return 0;
    int edge = grid_area_edge_line(gl, name, len, end_side);
    if (edge > 0) return edge;
    if (gl->tracks) {
        for (int i = 0; i < gl->tracks->n_line_names; i++) {
            const ns_css_line_name *ln = &gl->tracks->line_names[i];
            if (ln->line > after && strlen(ln->name) == len &&
                strncmp(ln->name, name, len) == 0)
                return ln->line;
        }
    }
    if (gl->areas && len > 4) {
        gsize base = 0;
        gboolean want_end = FALSE;
        if (len > 6 && strncmp(name + len - 6, "-start", 6) == 0)
            base = len - 6;
        else if (len > 4 && strncmp(name + len - 4, "-end", 4) == 0) {
            base = len - 4;
            want_end = TRUE;
        }
        if (base) {
            for (int i = 0; i < gl->areas->n_rects; i++) {
                const ns_css_area_rect *a = &gl->areas->rects[i];
                if (!a->name || strlen(a->name) != base ||
                    strncmp(a->name, name, base) != 0)
                    continue;
                int line = gl->row_axis ? (want_end ? a->r1 + 2 : a->r0 + 1)
                                        : (want_end ? a->c1 + 2 : a->c0 + 1);
                if (line > after) return line;
            }
        }
    }
    if (gl->areas) {
        for (int i = 0; i < gl->areas->n_rects; i++) {
            const ns_css_area_rect *a = &gl->areas->rects[i];
            if (!a->name || strlen(a->name) != len ||
                strncmp(a->name, name, len) != 0)
                continue;
            int start = gl->row_axis ? a->r0 + 1 : a->c0 + 1;
            int end = gl->row_axis ? a->r1 + 2 : a->c1 + 2;
            if (start > after) return start;
            if (end > after) return end;
        }
    }
    return 0;
}

static int
grid_resolve_line_from(const char *s, int n_tracks, int after,
                       gboolean end_side)
{
    if (!s) return 0;
    while (*s == ' ') s++;
    gsize len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') len--;
    char *end = NULL;
    long n = strtol(s, &end, 10);
    if (end == s) {
        int named = grid_named_line(s, len, after, end_side);
        if (named > 0) return named;
        return 0;
    }
    while (end && *end == ' ') end++;
    if (!end || *end != '\0' || n == 0) return 0;
    if (n < 0) n = n_tracks + 2 + n;
    if (n < 1 || n > NS_CSS_TRACKS_MAX + 1) return 0;
    return (int)n;
}

static int
grid_resolve_line_number(const char *s, int n_tracks)
{
    return grid_resolve_line_from(s, n_tracks, 0, FALSE);
}

#define NS_GRID_ROWS_MAX 4096

static gboolean
grid_span_is_count(const char *s)
{
    while (*s == ' ') s++;
    char *end = NULL;
    long n = strtol(s, &end, 10);
    if (end == s || n < 1) return FALSE;
    while (*end == ' ') end++;
    return *end == '\0';
}

static int
grid_parse_span(const char *s)
{
    return ns_parse_int(s, 1, 1, NS_GRID_ROWS_MAX);
}

static int
grid_start_span_count(char *a)
{
    if (!g_str_has_prefix(g_strstrip(a), "span ")) return 1;
    return grid_span_is_count(a + 5) ? grid_parse_span(a + 5) : -1;
}

static int
grid_pos_span_pair(const char *s, const char *slash, int n_tracks,
                   int *out_start, int *out_span)
{
    char *a = g_strndup(s, slash - s);
    const char *b = slash + 1;
    while (*b == ' ') b++;
    int n = grid_resolve_line_number(a, n_tracks);
    *out_start = n > 0 ? n - 1 : 0;
    int start_span = grid_start_span_count(a);
    g_free(a);
    if (g_str_has_prefix(b, "span ")) {
        *out_span = grid_parse_span(b + 5);
        return n > 0;
    }
    int e = grid_resolve_line_from(b, n_tracks, n > 0 ? n : 0, TRUE);
    if (n > 0 && e > n) *out_span = e - n;
    if (n <= 0 && start_span > 0 && e - start_span >= 1) {
        *out_start = e - start_span - 1;
        *out_span = start_span;
        return 1;
    }
    return n > 0;
}

static int
grid_pos_span(const ns_css_value *v, int n_tracks,
              int *out_start, int *out_span)
{
    *out_start = 0;
    *out_span  = 1;
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return 0;
    const char *s = v->u.keyword;
    if (g_str_has_prefix(s, "span ")) {
        *out_span = grid_parse_span(s + 5);
        return 0;
    }
    const char *slash = strchr(s, '/');
    if (slash) return grid_pos_span_pair(s, slash, n_tracks, out_start, out_span);
    int n = grid_resolve_line_number(s, n_tracks);
    if (n > 0) { *out_start = n - 1; return 1; }
    return 0;
}

static int
grid_line_num(const ns_css_value *v, int n_tracks, int after,
              gboolean end_side)
{
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return 0;
    if (g_str_has_prefix(v->u.keyword, "span ")) return 0;
    return grid_resolve_line_from(v->u.keyword, n_tracks, after, end_side);
}

static int
grid_area_axis_pos(const ns_style *st, gboolean row_axis, int n_tracks,
                   int *out_start, int *out_span)
{
    const ns_css_value *v = st ? st->values[NS_CSS_GRID_AREA] : NULL;
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return 0;
    char **parts = g_strsplit(v->u.keyword, "/", -1);
    int n = 0;
    while (parts[n]) n++;
    char *sstr = row_axis ? (n > 0 ? parts[0] : NULL)
                          : (n > 1 ? parts[1] : NULL);
    char *estr = row_axis ? (n > 2 ? parts[2] : NULL)
                          : (n > 3 ? parts[3] : NULL);
    int got = 0;
    if (sstr) {
        char *ss = g_strstrip(sstr);
        int s = grid_resolve_line_number(ss, n_tracks);
        if (g_str_has_prefix(ss, "span "))
            *out_span = grid_parse_span(ss + 5);
        if (s > 0) {
            *out_start = s - 1;
            *out_span = 1;
            got = 1;
        }
    }
    if (!estr && got && sstr) {
        char *ss = g_strstrip(sstr);
        char *tail = NULL;
        strtol(ss, &tail, 10);
        if (tail == ss) {
            int e = grid_resolve_line_from(ss, n_tracks, *out_start + 1, TRUE);
            if (e > *out_start + 1) *out_span = e - (*out_start + 1);
        }
    }
    if (estr) {
        char *es = g_strstrip(estr);
        if (g_str_has_prefix(es, "span ")) {
            *out_span = grid_parse_span(es + 5);
        } else if (got) {
            int e = grid_resolve_line_from(es, n_tracks, *out_start + 1, TRUE);
            if (e > *out_start + 1) *out_span = e - (*out_start + 1);
        }
    }
    g_strfreev(parts);
    return got;
}

static int
grid_resolve_pos(const ns_style *st, ns_css_prop shorthand,
                 ns_css_prop start_prop, ns_css_prop end_prop,
                 int n_tracks,
                 int *out_start, int *out_span)
{
    int got = grid_pos_span(st ? st->values[shorthand] : NULL, n_tracks,
                            out_start, out_span);
    if (got) return 1;
    if (!st) return 0;
    if (grid_area_axis_pos(st, start_prop == NS_CSS_GRID_ROW_START, n_tracks,
                           out_start, out_span))
        return 1;
    int sl = grid_line_num(st->values[start_prop], n_tracks, 0, FALSE);
    int el = grid_line_num(st->values[end_prop], n_tracks, sl, TRUE);
    if (sl > 0) {
        *out_start = sl - 1;
        const ns_css_value *ev = st->values[end_prop];
        if (el > sl)
            *out_span = el - sl;
        else if (ev && ev->kind == NS_CSS_V_KEYWORD && ev->u.keyword &&
                 g_str_has_prefix(ev->u.keyword, "span "))
            *out_span = grid_parse_span(ev->u.keyword + 5);
        return 1;
    }
    const ns_css_value *sv = st->values[start_prop];
    gboolean start_span = sv && sv->kind == NS_CSS_V_KEYWORD && sv->u.keyword &&
                          g_str_has_prefix(sv->u.keyword, "span ");
    if (el > 0 && (!start_span || grid_span_is_count(sv->u.keyword + 5))) {
        int span = start_span ? grid_parse_span(sv->u.keyword + 5) : 1;
        if (el - span >= 1) {
            *out_start = el - span - 1;
            *out_span = span;
            return 1;
        }
    }
    if (sv && sv->kind == NS_CSS_V_KEYWORD && sv->u.keyword &&
        g_str_has_prefix(sv->u.keyword, "span ")) {
        *out_span = grid_parse_span(sv->u.keyword + 5);
        return 0;
    }
    const ns_css_value *ev = st->values[end_prop];
    if (ev && ev->kind == NS_CSS_V_KEYWORD && ev->u.keyword &&
        g_str_has_prefix(ev->u.keyword, "span "))
        *out_span = grid_parse_span(ev->u.keyword + 5);
    return 0;
}

#define NS_GRID_NESTING_MAX 64

static int g_grid_nesting;

typedef gboolean grid_occupancy_row[NS_CSS_TRACKS_MAX];

static GArray *
grid_occupancy_new(void)
{
    return g_array_new(FALSE, TRUE, sizeof(grid_occupancy_row));
}

static gboolean
grid_occupied(const GArray *occupied, int row, int col)
{
    if (row < 0 || col < 0 || col >= NS_CSS_TRACKS_MAX) return TRUE;
    if ((guint)row >= occupied->len) return FALSE;
    return g_array_index(occupied, grid_occupancy_row, row)[col];
}

static void
grid_occupy(GArray *occupied, int row, int col)
{
    if (row < 0 || row >= NS_GRID_ROWS_MAX ||
        col < 0 || col >= NS_CSS_TRACKS_MAX)
        return;
    if ((guint)row >= occupied->len) g_array_set_size(occupied, row + 1);
    g_array_index(occupied, grid_occupancy_row, row)[col] = TRUE;
}

static gboolean
grid_slot_available(const GArray *occupied,
                    int row, int col, int col_span, int row_span,
                    int n_cols)
{
    if (row < 0 || col < 0 || col_span < 1 || row_span < 1)
        return FALSE;
    if (col + col_span > n_cols || row + row_span > NS_GRID_ROWS_MAX)
        return FALSE;
    for (int r = 0; r < row_span; r++) {
        for (int c = 0; c < col_span; c++) {
            if (grid_occupied(occupied, row + r, col + c))
                return FALSE;
        }
    }
    return TRUE;
}

static void
grid_slot_mark(GArray *occupied,
               int row, int col, int col_span, int row_span,
               int n_cols)
{
    if (row < 0 || col < 0 || col_span < 1 || row_span < 1)
        return;
    if (col + col_span > n_cols) col_span = n_cols - col;
    if (row + row_span > NS_GRID_ROWS_MAX)
        row_span = NS_GRID_ROWS_MAX - row;
    for (int r = 0; r < row_span; r++) {
        for (int c = 0; c < col_span; c++)
            grid_occupy(occupied, row + r, col + c);
    }
}

static gboolean
grid_find_slot(const GArray *occupied,
               int *out_row, int *out_col,
               int col_span, int row_span, int n_cols,
               int start_row, int start_col,
               gboolean fixed_row, gboolean fixed_col)
{
    int r0 = start_row >= 0 ? start_row : 0;
    int c0 = start_col >= 0 ? start_col : 0;
    if (fixed_row && fixed_col) {
        *out_row = r0;
        *out_col = c0;
        return r0 < NS_GRID_ROWS_MAX && c0 < n_cols;
    }
    for (int r = r0; r < NS_GRID_ROWS_MAX; r++) {
        int first_col = fixed_col ? c0 : (r == r0 ? c0 : 0);
        int last_col = fixed_col ? c0 : n_cols - col_span;
        if (last_col < first_col) last_col = first_col;
        for (int c = first_col; c <= last_col; c++) {
            if (grid_slot_available(occupied, r, c, col_span, row_span, n_cols)) {
                *out_row = r;
                *out_col = c;
                return TRUE;
            }
        }
        if (fixed_row) break;
    }
    return FALSE;
}

static void
grid_advance_cursor(const GArray *occupied,
                    int *row, int *col, int n_cols)
{
    if (*col >= n_cols) {
        *col = 0;
        (*row)++;
    }
    while (*row < NS_GRID_ROWS_MAX && grid_occupied(occupied, *row, *col)) {
        (*col)++;
        if (*col >= n_cols) {
            *col = 0;
            (*row)++;
        }
    }
}

static double
grid_track_px(const ns_css_track *t, double basis)
{
    if (!t) return 0;
    if (t->kind == NS_CSS_TRACK_PX)
        return t->v + (basis >= 0 ? t->pct * basis / 100.0 : 0);
    if (t->kind == NS_CSS_TRACK_PERCENT && basis >= 0)
        return t->v * basis / 100.0;
    return 0;
}

static gboolean
grid_track_is_fixed(const ns_css_track *t, double basis)
{
    if (!t) return FALSE;
    if (t->kind == NS_CSS_TRACK_PX) {
        if (t->pct != 0 && basis < 0) return FALSE;
    } else if (t->kind != NS_CSS_TRACK_PERCENT || basis < 0) {
        return FALSE;
    }
    if (!t->has_min) return TRUE;
    return t->min_kind == NS_CSS_TRACK_PX ||
           (t->min_kind == NS_CSS_TRACK_PERCENT && basis >= 0);
}

static void
grid_distribute_extra(double *sizes, const double *caps,
                      const gboolean *affected, const gboolean *beyond,
                      int count, double extra)
{
    double grow[NS_CSS_TRACKS_MAX] = {0};
    gboolean frozen[NS_CSS_TRACKS_MAX] = {0};
    for (int i = 0; i < count; i++) frozen[i] = !affected[i];
    while (extra > 1e-9) {
        int open = 0;
        for (int i = 0; i < count; i++) if (!frozen[i]) open++;
        if (!open) break;
        double share = extra / open;
        gboolean capped = FALSE;
        for (int i = 0; i < count; i++) {
            if (frozen[i]) continue;
            double room = caps[i] - (sizes[i] + grow[i]);
            if (room > share + 1e-9) continue;
            if (room < 0) room = 0;
            grow[i] += room;
            extra -= room;
            frozen[i] = TRUE;
            capped = TRUE;
        }
        if (capped) continue;
        for (int i = 0; i < count; i++) if (!frozen[i]) grow[i] += share;
        extra = 0;
    }
    if (extra > 1e-9) {
        int n = 0;
        for (int i = 0; i < count; i++) if (affected[i] && beyond[i]) n++;
        gboolean only_beyond = n > 0;
        if (!only_beyond)
            for (int i = 0; i < count; i++) if (affected[i]) n++;
        for (int i = 0; n > 0 && i < count; i++)
            if (affected[i] && (!only_beyond || beyond[i]))
                grow[i] += extra / n;
    }
    for (int i = 0; i < count; i++) sizes[i] += grow[i];
}

static gboolean
grid_track_max_is_intrinsic(const ns_css_track *t)
{
    return track_is_intrinsic(t->kind);
}

static gboolean
grid_track_min_is_intrinsic(const ns_css_track *t)
{
    return t->has_min ? track_is_intrinsic(t->min_kind)
                      : track_is_intrinsic(t->kind);
}

static double
grid_track_fixed_px(const ns_css_track *t, double avail)
{
    if (t->kind == NS_CSS_TRACK_PX) return t->v + t->pct * avail / 100.0;
    if (t->kind == NS_CSS_TRACK_PERCENT) return t->v * avail / 100.0;
    return 0;
}

static gboolean
grid_span_accommodate(const ns_css_tracks *cols, int c0, int span,
                      const double *gap_after, double avail,
                      double min_contribution, double max_contribution,
                      double *col_min, double *col_content)
{
    double gaps = 0;
    for (int i = 0; i < span; i++) {
        const ns_css_track *t = &cols->tracks[c0 + i];
        if (t->kind == NS_CSS_TRACK_FR) return FALSE;
        if (i + 1 < span) gaps += gap_after[c0 + i];
    }
    double base[NS_CSS_TRACKS_MAX], limit[NS_CSS_TRACKS_MAX];
    double caps[NS_CSS_TRACKS_MAX];
    gboolean affected[NS_CSS_TRACKS_MAX], beyond[NS_CSS_TRACKS_MAX];
    gboolean any = FALSE;
    double sum = gaps;
    for (int i = 0; i < span; i++) {
        const ns_css_track *t = &cols->tracks[c0 + i];
        gboolean min_intrinsic = grid_track_min_is_intrinsic(t);
        gboolean max_intrinsic = grid_track_max_is_intrinsic(t);
        base[i] = min_intrinsic ? col_min[c0 + i]
                : t->has_min ? track_min_px(t, avail)
                             : grid_track_fixed_px(t, avail);
        caps[i] = max_intrinsic ? INFINITY : grid_track_fixed_px(t, avail);
        affected[i] = min_intrinsic;
        beyond[i] = max_intrinsic;
        any = any || min_intrinsic || max_intrinsic;
        sum += base[i];
    }
    if (!any) return FALSE;
    if (min_contribution > sum) {
        grid_distribute_extra(base, caps, affected, beyond, span,
                              min_contribution - sum);
        for (int i = 0; i < span; i++)
            if (affected[i]) col_min[c0 + i] = base[i];
    }
    sum = gaps;
    gboolean any_max_min = FALSE;
    for (int i = 0; i < span; i++) {
        const ns_css_track *t = &cols->tracks[c0 + i];
        ns_css_track_kind min_kind = t->has_min ? t->min_kind : t->kind;
        affected[i] = min_kind == NS_CSS_TRACK_MAX_CONTENT;
        beyond[i] = affected[i];
        any_max_min = any_max_min || affected[i];
        sum += base[i];
    }
    if (any_max_min && max_contribution > sum) {
        grid_distribute_extra(base, caps, affected, beyond, span,
                              max_contribution - sum);
        for (int i = 0; i < span; i++)
            if (affected[i]) col_min[c0 + i] = base[i];
    }
    for (int pass = 0; pass < 2; pass++) {
        double contribution = pass == 0 ? min_contribution : max_contribution;
        sum = gaps;
        for (int i = 0; i < span; i++) {
            const ns_css_track *t = &cols->tracks[c0 + i];
            limit[i] = grid_track_max_is_intrinsic(t)
                ? MAX(col_content[c0 + i], base[i])
                : MAX(grid_track_fixed_px(t, avail), base[i]);
            caps[i] = INFINITY;
            affected[i] = pass == 0 ? grid_track_max_is_intrinsic(t)
                                    : t->kind == NS_CSS_TRACK_AUTO ||
                                      t->kind == NS_CSS_TRACK_MAX_CONTENT;
            beyond[i] = affected[i];
            sum += limit[i];
        }
        if (contribution <= sum) continue;
        grid_distribute_extra(limit, caps, affected, beyond, span,
                              contribution - sum);
        for (int i = 0; i < span; i++)
            if (affected[i]) col_content[c0 + i] = limit[i];
    }
    return TRUE;
}

static void
grid_expand_flexible_rows(double *row_height, int n_rows,
                          const ns_css_tracks *rows_tracks,
                          const ns_css_tracks *auto_rows_tracks,
                          int explicit_rows, double space)
{
    double *fr_factor = g_new0(double, n_rows + 1);
    gboolean any_fr = FALSE;
    for (int r = 0; r < n_rows; r++) {
        const ns_css_track *tk = NULL;
        if (rows_tracks && r < rows_tracks->n) tk = &rows_tracks->tracks[r];
        else if (auto_rows_tracks && auto_rows_tracks->n > 0)
            tk = &auto_rows_tracks->tracks[(r - explicit_rows) % auto_rows_tracks->n];
        if (tk && tk->kind == NS_CSS_TRACK_FR && tk->v > 0) {
            fr_factor[r] = tk->v;
            any_fr = TRUE;
        }
    }
    if (any_fr) {
        for (int r = 0; r < n_rows; r++)
            if (fr_factor[r] <= 0) space -= row_height[r];
        gboolean *inflexible = g_new0(gboolean, n_rows + 1);
        for (int pass = 0; pass <= n_rows; pass++) {
            double sum_fr = 0, leftover = space;
            for (int r = 0; r < n_rows; r++) {
                if (fr_factor[r] <= 0) continue;
                if (inflexible[r]) leftover -= row_height[r];
                else sum_fr += fr_factor[r];
            }
            if (sum_fr <= 0) break;
            double unit = leftover > 0 ? leftover / MAX(sum_fr, 1.0) : 0;
            gboolean changed = FALSE;
            for (int r = 0; r < n_rows; r++) {
                if (fr_factor[r] <= 0 || inflexible[r]) continue;
                if (row_height[r] > unit * fr_factor[r] + 0.01) {
                    inflexible[r] = TRUE;
                    changed = TRUE;
                }
            }
            if (changed) continue;
            for (int r = 0; r < n_rows; r++)
                if (fr_factor[r] > 0 && !inflexible[r])
                    row_height[r] = unit * fr_factor[r];
            break;
        }
        g_free(inflexible);
    }
    g_free(fr_factor);
}

static double
grid_row_fr_factor(const ns_css_tracks *rows_tracks,
                   const ns_css_tracks *auto_rows_tracks, int explicit_rows,
                   int r)
{
    const ns_css_track *tk = NULL;
    if (rows_tracks && r < rows_tracks->n) tk = &rows_tracks->tracks[r];
    else if (auto_rows_tracks && auto_rows_tracks->n > 0)
        tk = &auto_rows_tracks->tracks[(r - explicit_rows) % auto_rows_tracks->n];
    return tk && tk->kind == NS_CSS_TRACK_FR && tk->v > 0 ? tk->v : 0;
}

static void
grid_size_flexible_rows_to_content(double *row_height, int n_rows,
                                   const ns_css_tracks *rows_tracks,
                                   const ns_css_tracks *auto_rows_tracks,
                                   int explicit_rows)
{
    double fraction = 0;
    for (int r = 0; r < n_rows; r++) {
        double fr = grid_row_fr_factor(rows_tracks, auto_rows_tracks,
                                       explicit_rows, r);
        if (fr > 0)
            fraction = MAX(fraction, fr > 1 ? row_height[r] / fr
                                            : row_height[r]);
    }
    for (int r = 0; r < n_rows; r++) {
        double fr = grid_row_fr_factor(rows_tracks, auto_rows_tracks,
                                       explicit_rows, r);
        if (fr > 0 && fraction * fr > row_height[r])
            row_height[r] = fraction * fr;
    }
}

static double
grid_auto_repeat_height(const ns_box *box, double row_basis, double cw)
{
    if (row_basis > 0) return row_basis;
    if (!box->style) return 0;
    const ns_css_value *mx = box->style->values[NS_CSS_MAX_HEIGHT];
    if (mx && (mx->kind == NS_CSS_V_LENGTH || mx->kind == NS_CSS_V_CALC)) {
        double h = specified_height_to_content(box,
                                               resolve_used_height(box, mx, cw, -1));
        if (h > 0) return h;
    }
    const ns_css_value *mn = box->style->values[NS_CSS_MIN_HEIGHT];
    if (mn && (mn->kind == NS_CSS_V_LENGTH || mn->kind == NS_CSS_V_CALC)) {
        double h = specified_height_to_content(box,
                                               resolve_used_height(box, mn, cw, -1));
        if (h > 0) return h;
    }
    return 0;
}

static void
grid_extend_with_auto_tracks(ns_css_tracks *tracks, int from, int to,
                             const ns_css_value *auto_v)
{
    const ns_css_tracks *pattern =
        auto_v && auto_v->kind == NS_CSS_V_TRACKS && auto_v->u.tracks.n > 0 &&
        !auto_v->u.tracks.subgrid ? &auto_v->u.tracks : NULL;
    if (to > NS_CSS_TRACKS_MAX) to = NS_CSS_TRACKS_MAX;
    for (int i = from; i < to; i++) {
        ns_css_track t = { .kind = NS_CSS_TRACK_AUTO };
        if (pattern) t = pattern->tracks[(i - from) % pattern->n];
        tracks->tracks[i] = t;
    }
    if (to > from) tracks->n = to;
}

static double
box_inset_definite_height(const ns_box *box)
{
    if (!box->style || !style_is_absolute_or_fixed(box->style) ||
        box->content_height <= 0)
        return -1;
    const ns_css_value *top = box->style->values[NS_CSS_TOP];
    const ns_css_value *bottom = box->style->values[NS_CSS_BOTTOM];
    if (!top || length_is_auto(top) || !bottom || length_is_auto(bottom))
        return -1;
    return box->content_height;
}

static double
grid_row_basis(const ns_box *box, double cw)
{
    if (box_height_flexed_in_column(box)) return box->definite_height;
    const ns_css_value *hv = box->style ? box->style->values[NS_CSS_HEIGHT] : NULL;
    if (hv && !box->block_size_indefinite &&
        (hv->kind == NS_CSS_V_LENGTH || hv->kind == NS_CSS_V_CALC))
        return clamp_height_minmax_px(box->style,
                                      resolve_used_height(box, hv, cw, -1));
    double inset = box_inset_definite_height(box);
    if (inset > 0) return inset;
    return box_read_definite_height(box) > 0 ? box->definite_height : -1;
}

static double
grid_used_content_height(const ns_box *box, double cw)
{
    if (box_height_flexed_in_column(box)) return box->definite_height;
    double eh = box_style_definite_height(box, cw);
    if (eh < 0) return -1;
    eh = clamp_height_minmax_px(box->style, eh);
    if (keyword_is(box->style->values[NS_CSS_BOX_SIZING], "border-box"))
        eh -= box->padding.top + box->padding.bottom +
              box->border.top + box->border.bottom;
    return eh;
}

static gboolean
grid_item_has_auto_minimum(const ns_box *c)
{
    const ns_css_value *mnw = c->style ? c->style->values[NS_CSS_MIN_WIDTH]
                                       : NULL;
    if (mnw && !length_is_auto(mnw)) return FALSE;
    return !box_is_scroll_container(c);
}

typedef struct {
    int col, span;
    double contribution, minimum;
    gboolean auto_minimum;
} grid_intrinsic_item;

static double
grid_item_outer_extras(const ns_box *c)
{
    if (!c->style) return 0;
    ns_edges m = {0}, pd = {0}, bd = {0};
    edges_from_style(c->style, 0, &m, &pd, &bd);
    return m.left + m.right + pd.left + pd.right + bd.left + bd.right;
}

static double
grid_item_minimum_contribution(ns_box *c, double min_content,
                               gboolean *auto_minimum)
{
    const ns_style *s = c->style;
    *auto_minimum = FALSE;
    if (!s || c->kind == NS_BOX_INLINE || c->kind == NS_BOX_TEXT)
        return min_content;
    const ns_css_value *wv = s->values[NS_CSS_WIDTH];
    gboolean preferred_fixed = wv && !value_is_percent(wv) &&
        (wv->kind == NS_CSS_V_LENGTH || wv->kind == NS_CSS_V_CALC);
    if (preferred_fixed) return min_content;
    double extras = grid_item_outer_extras(c);
    double min_w = definite_width_limit(c, s->values[NS_CSS_MIN_WIDTH]);
    if (min_w >= 0) return min_w + extras;
    if (!grid_item_has_auto_minimum(c)) return extras;
    *auto_minimum = TRUE;
    return min_content;
}

static gboolean
grid_min_track_is_fixed(const ns_css_track *t, double *px)
{
    ns_css_track_kind k = t->has_min ? t->min_kind : t->kind;
    double v = t->has_min ? t->min_v : t->v;
    double pct = t->has_min ? t->min_pct : t->pct;
    if (!t->has_min && t->kind == NS_CSS_TRACK_FR) return FALSE;
    if (k != NS_CSS_TRACK_PX || pct != 0) return FALSE;
    *px = v > 0 ? v : 0;
    return TRUE;
}

static double
grid_max_track_limit(const ns_css_track *t)
{
    if (t->kind == NS_CSS_TRACK_PX && t->pct == 0) return t->v > 0 ? t->v : 0;
    return -1;
}

static void
grid_grow_span(double *size, const gboolean *open, int c0, int span,
               double gaps, double contribution)
{
    double sum = gaps;
    int n_open = 0;
    for (int i = c0; i < c0 + span; i++) {
        sum += size[i];
        if (open[i]) n_open++;
    }
    if (contribution <= sum || n_open == 0) return;
    double share = (contribution - sum) / n_open;
    for (int i = c0; i < c0 + span; i++)
        if (open[i]) size[i] += share;
}

static GArray *
grid_intrinsic_items(ns_box *box, const ns_style *child_style,
                     gboolean max_content, ns_css_tracks *cols, int *n_cols_out,
                     int *max_span_out)
{
    const ns_css_value *cv = box->style->values[NS_CSS_GRID_TEMPLATE_COLUMNS];
    if (cv && cv->kind == NS_CSS_V_TRACKS &&
        (cv->u.tracks.subgrid || cv->u.tracks.auto_repeat != NS_CSS_AUTO_REPEAT_NONE))
        return NULL;
    const ns_css_value *areas_v = box->style->values[NS_CSS_GRID_TEMPLATE_AREAS];
    const ns_css_areas *areas = areas_v && areas_v->kind == NS_CSS_V_AREAS
        ? &areas_v->u.areas : NULL;
    memset(cols, 0, sizeof(*cols));
    if (cv && cv->kind == NS_CSS_V_TRACKS) *cols = cv->u.tracks;
    int explicit_cols = cols->n;
    if (areas && areas->n_cols > explicit_cols) {
        grid_extend_with_auto_tracks(cols, explicit_cols, areas->n_cols,
            box->style->values[NS_CSS_GRID_AUTO_COLUMNS]);
        explicit_cols = cols->n;
    }
    int flow_cols = explicit_cols > 0 ? explicit_cols : 1;
    int max_span = 1;

    GArray *items = g_array_new(FALSE, FALSE, sizeof(grid_intrinsic_item));
    grid_lines col_lines = { cols, areas, FALSE };
    for (ns_box *c = box->first_child; c; c = c->next_sibling) {
        if (style_is_absolute_or_fixed(c->style)) continue;
        int s = -1, sp = 1;
        if (c->style) {
            g_grid_lines = &col_lines;
            if (!grid_resolve_pos(c->style, NS_CSS_GRID_COLUMN,
                                  NS_CSS_GRID_COLUMN_START,
                                  NS_CSS_GRID_COLUMN_END, explicit_cols,
                                  &s, &sp))
                s = -1;
            g_grid_lines = NULL;
        }
        if (sp < 1) sp = 1;
        if (sp > NS_CSS_TRACKS_MAX) sp = NS_CSS_TRACKS_MAX;
        if (s >= 0 && s + sp > NS_CSS_TRACKS_MAX) s = NS_CSS_TRACKS_MAX - sp;
        if (s + sp > flow_cols) flow_cols = s + sp;
        if (sp > max_span) max_span = sp;
        grid_intrinsic_item it = { s, sp, 0, 0, FALSE };
        double extras = grid_item_outer_extras(c);
        if (max_content) {
            it.contribution = measure_natural_width(c, child_style) + extras;
        } else {
            it.contribution = min_width_of(c, child_style) + extras;
            it.minimum = grid_item_minimum_contribution(c, it.contribution,
                                                        &it.auto_minimum);
        }
        g_array_append_val(items, it);
    }
    int cursor = 0;
    for (guint k = 0; k < items->len; k++) {
        grid_intrinsic_item *it = &g_array_index(items, grid_intrinsic_item, k);
        if (it->col < 0) {
            if (cursor > 0 && cursor + it->span > flow_cols) cursor = 0;
            it->col = cursor;
        }
        cursor = it->col + it->span;
    }
    if (flow_cols > explicit_cols)
        grid_extend_with_auto_tracks(cols, explicit_cols, flow_cols,
            box->style->values[NS_CSS_GRID_AUTO_COLUMNS]);
    *n_cols_out = flow_cols;
    *max_span_out = max_span;
    return items;
}

static gboolean
grid_item_crosses(const grid_intrinsic_item *it, const gboolean *tracks)
{
    for (int i = it->col; i < it->col + it->span; i++)
        if (tracks[i]) return TRUE;
    return FALSE;
}

static double
grid_column_gap_px(const ns_box *box)
{
    double gap = gap_px(box->style->values[NS_CSS_COLUMN_GAP],
                        box->style->values[NS_CSS_GAP], 0);
    return gap > 0 ? gap : 0;
}

static double
grid_min_content_width(ns_box *box, const ns_style *child_style)
{
    ns_css_tracks cols;
    int n_cols = 0, max_span = 1;
    GArray *items = grid_intrinsic_items(box, child_style, FALSE, &cols,
                                         &n_cols, &max_span);
    if (!items) return -1;
    double base[NS_CSS_TRACKS_MAX] = {0};
    gboolean flexible[NS_CSS_TRACKS_MAX] = {0};
    gboolean grows[NS_CSS_TRACKS_MAX] = {0};
    gboolean grows_flexible[NS_CSS_TRACKS_MAX] = {0};
    for (int i = 0; i < n_cols; i++) {
        const ns_css_track *tk = &cols.tracks[i];
        double px = 0;
        flexible[i] = tk->kind == NS_CSS_TRACK_FR;
        if (grid_min_track_is_fixed(tk, &px)) {
            base[i] = px;
        } else {
            grows[i] = !flexible[i];
            grows_flexible[i] = flexible[i];
        }
    }
    double gap = grid_column_gap_px(box);
    for (int span = 1; span <= max_span; span++) {
        for (guint k = 0; k < items->len; k++) {
            grid_intrinsic_item *it = &g_array_index(items, grid_intrinsic_item, k);
            if (it->span != span) continue;
            double gaps = gap * (span - 1);
            if (grid_item_crosses(it, flexible)) {
                grid_grow_span(base, grows_flexible, it->col, span, gaps,
                               it->minimum);
                continue;
            }
            double contribution = it->contribution;
            if (span == 1) {
                const ns_css_track *tk = &cols.tracks[it->col];
                double limit = grid_max_track_limit(tk);
                double minimum = it->minimum;
                if (limit >= 0 && it->auto_minimum && !tk->fit_content)
                    minimum = MIN(minimum, limit);
                if (limit >= 0 && contribution > limit)
                    contribution = MAX(limit, minimum);
            }
            grid_grow_span(base, grows, it->col, span, gaps, contribution);
        }
    }
    g_array_free(items, TRUE);
    double sum = gap * (n_cols - 1);
    for (int i = 0; i < n_cols; i++) sum += base[i];
    return sum;
}

static double
grid_max_content_width(ns_box *box, const ns_style *child_style)
{
    ns_css_tracks cols;
    int n_cols = 0, max_span = 1;
    GArray *items = grid_intrinsic_items(box, child_style, TRUE, &cols,
                                         &n_cols, &max_span);
    if (!items) return -1;
    double size[NS_CSS_TRACKS_MAX] = {0};
    double factor[NS_CSS_TRACKS_MAX] = {0};
    gboolean flexible[NS_CSS_TRACKS_MAX] = {0};
    gboolean grows[NS_CSS_TRACKS_MAX] = {0};
    for (int i = 0; i < n_cols; i++) {
        const ns_css_track *tk = &cols.tracks[i];
        double px = 0;
        if (grid_min_track_is_fixed(tk, &px)) size[i] = px;
        if (tk->kind == NS_CSS_TRACK_FR) {
            flexible[i] = TRUE;
            factor[i] = tk->v > 0 ? tk->v : 0;
            continue;
        }
        double limit = tk->fit_content ? -1 : grid_max_track_limit(tk);
        if (limit >= 0) size[i] = MAX(size[i], limit);
        else grows[i] = TRUE;
    }
    double gap = grid_column_gap_px(box);
    for (int span = 1; span <= max_span; span++) {
        for (guint k = 0; k < items->len; k++) {
            grid_intrinsic_item *it = &g_array_index(items, grid_intrinsic_item, k);
            if (it->span != span || grid_item_crosses(it, flexible)) continue;
            double contribution = it->contribution;
            if (span == 1 && cols.tracks[it->col].fit_content) {
                double limit = grid_max_track_limit(&cols.tracks[it->col]);
                if (limit >= 0 && contribution > limit)
                    contribution = MAX(limit, size[it->col]);
            }
            grid_grow_span(size, grows, it->col, span, gap * (span - 1),
                           contribution);
        }
    }
    double fr = 0;
    for (int i = 0; i < n_cols; i++)
        if (flexible[i] && factor[i] > 0)
            fr = MAX(fr, factor[i] > 1 ? size[i] / factor[i] : size[i]);
    for (guint k = 0; k < items->len; k++) {
        grid_intrinsic_item *it = &g_array_index(items, grid_intrinsic_item, k);
        if (!grid_item_crosses(it, flexible)) continue;
        double leftover = it->contribution - gap * (it->span - 1);
        double factors = 0;
        for (int i = it->col; i < it->col + it->span; i++) {
            if (flexible[i]) factors += factor[i];
            else leftover -= size[i];
        }
        if (factors <= 0) continue;
        fr = MAX(fr, leftover / MAX(factors, 1.0));
    }
    for (int i = 0; i < n_cols; i++)
        if (flexible[i]) size[i] = MAX(size[i], fr * factor[i]);
    g_array_free(items, TRUE);
    double sum = gap * (n_cols - 1);
    for (int i = 0; i < n_cols; i++) sum += size[i];
    return sum;
}

typedef struct {
    ns_box *box;
    const ns_style *inherited;
    int col, span;
    double extra;
} grid_col_item;

static double
grid_item_edge_extra(const ns_box *c, gboolean leading)
{
    if (!c->style) return 0;
    ns_edges m = {0}, pd = {0}, bd = {0};
    edges_from_style(c->style, 0, &m, &pd, &bd);
    return leading ? m.left + pd.left + bd.left : m.right + pd.right + bd.right;
}

static void
grid_collect_col_items(GArray *out, ns_box *c, const ns_style *inherited,
                       int col, int span, double extra, int depth)
{
    if (depth >= 8 || span < 1 || !box_is_grid_container(c) ||
        !style_columns_are_subgrid(c->style)) {
        grid_col_item it = { c, inherited, col, span, extra };
        g_array_append_val(out, it);
        return;
    }
    double lead = grid_item_edge_extra(c, TRUE);
    double trail = grid_item_edge_extra(c, FALSE);
    int cursor = 0;
    for (ns_box *gc = c->first_child; gc; gc = gc->next_sibling) {
        if (style_is_absolute_or_fixed(gc->style)) continue;
        int s = -1, sp = 1;
        if (gc->style &&
            !grid_resolve_pos(gc->style, NS_CSS_GRID_COLUMN,
                              NS_CSS_GRID_COLUMN_START, NS_CSS_GRID_COLUMN_END,
                              span, &s, &sp))
            s = -1;
        if (sp < 1) sp = 1;
        if (sp > span) sp = span;
        if (s < 0) {
            if (cursor > 0 && cursor + sp > span) cursor = 0;
            s = cursor;
        }
        if (s + sp > span) s = span - sp;
        cursor = s + sp;
        double edge = (s == 0 ? lead : 0) + (s + sp == span ? trail : 0);
        grid_collect_col_items(out, gc, c->style, col + s, sp, extra + edge,
                               depth + 1);
    }
}

static GArray *
grid_col_items(GPtrArray *items, GArray *placed_cols, GArray *col_spans,
               const ns_style *child_inherited)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(grid_col_item));
    for (guint k = 0; k < items->len; k++) {
        int col = k < placed_cols->len ? g_array_index(placed_cols, int, k) : -1;
        int span = g_array_index(col_spans, int, k);
        if (col < 0) continue;
        grid_collect_col_items(out, items->pdata[k], child_inherited, col,
                               span < 1 ? 1 : span, 0, 0);
    }
    return out;
}

static double
grid_item_fit_border_width(ns_box *c, const ns_style *child_inherited,
                           double area)
{
    double nat = measure_natural_width(c, child_inherited);
    if (nat < 0) return area;
    double fit = nat + c->padding.left + c->padding.right
        + c->border.left + c->border.right;
    return MIN(fit, area);
}

static double
grid_stretched_height(ns_box *c, double area_h)
{
    double saved = c->cb_height_override;
    c->cb_height_override = area_h;
    double h = flex_item_stretched_height(c, area_h, 0);
    c->cb_height_override = saved;
    return h;
}

static gboolean
grid_item_behaves_as_start(const ns_box *c)
{
    const ns_css_value *h = c->style ? c->style->values[NS_CSS_HEIGHT] : NULL;
    return c->style &&
           ((h && h->kind == NS_CSS_V_KEYWORD && size_keyword_is_intrinsic(h)) ||
            aspect_ratio_number(c->style->values[NS_CSS_ASPECT_RATIO],
                                NULL) > 0);
}

static gboolean
grid_item_stretches(const ns_box *grid, const ns_box *c)
{
    const char *a = flex_item_align(
        c, keyword_or(grid->style, NS_CSS_ALIGN_ITEMS, "normal"));
    if (!a) return TRUE;
    if (strcmp(a, "normal") == 0) return !grid_item_behaves_as_start(c);
    return flex_align_stretches(a);
}

static gboolean
grid_item_height_is_auto(const ns_box *c)
{
    const ns_css_value *h = c->style ? c->style->values[NS_CSS_HEIGHT] : NULL;
    return !(h && (h->kind == NS_CSS_V_LENGTH || h->kind == NS_CSS_V_CALC));
}

static gboolean
grid_item_presets_stretch(const ns_box *grid, const ns_box *c)
{
    return c->kind == NS_BOX_BLOCK && c->style && grid_item_height_is_auto(c) &&
           !keyword_is(c->style->values[NS_CSS_MARGIN_TOP], "auto") &&
           !keyword_is(c->style->values[NS_CSS_MARGIN_BOTTOM], "auto") &&
           grid_item_stretches(grid, c);
}

static gboolean
grid_row_track_definite(const ns_css_tracks *rows_tracks, gboolean rows_subgrid,
                        int r, double row_basis, gboolean allow_fr)
{
    if (rows_subgrid || !rows_tracks || r >= rows_tracks->n) return FALSE;
    ns_css_track_kind rk = rows_tracks->tracks[r].kind;
    return rk == NS_CSS_TRACK_PX || rk == NS_CSS_TRACK_PERCENT ||
           (allow_fr && rk == NS_CSS_TRACK_FR && row_basis > 0);
}

static gboolean
grid_row_track_fixed(const ns_css_tracks *rows_tracks, gboolean rows_subgrid,
                     int r, double row_basis)
{
    if (rows_subgrid || !rows_tracks || r >= rows_tracks->n) return FALSE;
    const ns_css_track *tk = &rows_tracks->tracks[r];
    return !tk->fit_content && grid_track_is_fixed(tk, row_basis);
}

static void
grid_preset_stretch_height(ns_box *c, const ns_box *grid, int row, int rs,
                           double area_h, double row_basis,
                           const ns_css_tracks *rows_tracks,
                           gboolean rows_subgrid)
{
    c->definite_height = 0;
    if (rs != 1 || area_h <= 0 || !grid_item_presets_stretch(grid, c) ||
        !grid_row_track_fixed(rows_tracks, rows_subgrid, row, row_basis))
        return;
    c->definite_height = grid_stretched_height(c, area_h);
}

static void
grid_relayout_stretched_item(ns_box *c, double pre_h,
                             const ns_style *child_inherited)
{
    if (!c->first_child || !c->definite_height_read) return;
    if (box_is_grid_container(c) && (style_columns_are_subgrid(c->style) ||
                                     style_rows_are_subgrid(c->style)))
        return;
    double target_h = c->content_height;
    double laid_out_h = c->definite_height > 0 ? c->definite_height : pre_h;
    if (fabs(target_h - laid_out_h) < 0.01) return;
    relayout_at_definite_height(c, c->last_layout_width, target_h,
                                child_inherited);
}

static void
grid_stretch_item(ns_box *c, double row_h, double item_outer, double row_basis,
                  int span, gboolean row_definite,
                  const ns_style *child_inherited)
{
    double free_h = row_h - item_outer;
    if (c->kind != NS_BOX_BLOCK || !grid_item_height_is_auto(c)) return;
    gboolean shrinks_to_row = span == 1 && free_h < -0.5 &&
        (row_definite || grid_item_min_block_contribution(
                             c, item_outer, row_basis) < item_outer);
    if (free_h <= 0.5 && !shrinks_to_row) return;
    double pre_h = c->content_height;
    c->content_height = grid_stretched_height(c, row_h);
    grid_relayout_stretched_item(c, pre_h, child_inherited);
}

static void
layout_grid(ns_box *box, double cw,
            double inner_x, double inner_y,
            const ns_style *child_inherited,
            double *cursor_y_out)
{
    const ns_subgrid_cols *sg = g_pending_subgrid_cols;
    const ns_subgrid_rows *sgr = g_pending_subgrid_rows;
    g_pending_subgrid_cols = NULL;
    g_pending_subgrid_rows = NULL;

    const ns_css_value *areas_v = box->style ? box->style->values[NS_CSS_GRID_TEMPLATE_AREAS] : NULL;
    const ns_css_areas *areas = areas_v && areas_v->kind == NS_CSS_V_AREAS
        ? &areas_v->u.areas : NULL;
    const ns_css_value *cols_v = box->style ? box->style->values[NS_CSS_GRID_TEMPLATE_COLUMNS] : NULL;
    gboolean cols_subgrid = cols_v && cols_v->kind == NS_CSS_V_TRACKS &&
                            cols_v->u.tracks.subgrid && sg && sg->n > 0;
    const ns_css_value *rows_v = box->style ? box->style->values[NS_CSS_GRID_TEMPLATE_ROWS]    : NULL;
    gboolean rows_subgrid = rows_v && rows_v->kind == NS_CSS_V_TRACKS &&
                            rows_v->u.tracks.subgrid && sgr && sgr->n > 0;
    ns_css_tracks default_cols = { .n = 1, .tracks = { { .kind = NS_CSS_TRACK_AUTO } } };
    const ns_css_tracks *cols_src =
        (cols_v && cols_v->kind == NS_CSS_V_TRACKS && !cols_v->u.tracks.subgrid) ?
        &cols_v->u.tracks : &default_cols;

    double col_gap = gap_px(box->style ? box->style->values[NS_CSS_COLUMN_GAP] : NULL,
                            box->style ? box->style->values[NS_CSS_GAP] : NULL, cw);
    double row_basis = grid_row_basis(box, cw);
    double row_gap = gap_px(
        box->style ? box->style->values[NS_CSS_ROW_GAP] : NULL,
        box->style ? box->style->values[NS_CSS_GAP] : NULL,
        row_basis > 0 ? row_basis : 0);
    if (rows_subgrid) row_gap = sgr->gap;

    int fit_start = 0, fit_count = 0;
    ns_css_tracks cols_buf = expand_auto_repeat_ex(cols_src, cw, col_gap,
                                                   &fit_start, &fit_count);
    if (cols_subgrid)
        grid_extend_with_auto_tracks(&cols_buf, 0, sg->n, NULL);
    if (areas && !cols_subgrid) {
        int templated = cols_src == &default_cols ? 0 : cols_buf.n;
        if (areas->n_cols > templated)
            grid_extend_with_auto_tracks(&cols_buf, templated, areas->n_cols,
                box->style->values[NS_CSS_GRID_AUTO_COLUMNS]);
    }
    const ns_css_tracks *cols = &cols_buf;
    int n_cols = cols->n > 0 ? cols->n : 1;
    int explicit_cols = n_cols;
    ns_css_tracks rows_buf = { 0 };
    const ns_css_tracks *rows_template = NULL;
    int row_fit_start = 0, row_fit_count = 0;
    if (!rows_subgrid && rows_v && rows_v->kind == NS_CSS_V_TRACKS &&
        !rows_v->u.tracks.subgrid) {
        rows_buf = expand_auto_repeat_ex(&rows_v->u.tracks,
                                         grid_auto_repeat_height(box, row_basis, cw),
                                         row_gap, &row_fit_start, &row_fit_count);
        rows_template = &rows_buf;
    }
    int row_line_tracks = rows_subgrid ? sgr->n :
        (rows_template ? rows_template->n : 1);
    if (areas && !rows_subgrid && areas->n_rows > row_line_tracks)
        row_line_tracks = areas->n_rows;
    if (row_line_tracks < 1) row_line_tracks = 1;

    double col_sizes[NS_CSS_TRACKS_MAX] = {0};
    double avail = cw - (n_cols > 1 ? col_gap * (n_cols - 1) : 0);
    if (avail < 0) avail = 0;

    GPtrArray *items = g_ptr_array_new();
    GArray *col_starts = g_array_new(FALSE, FALSE, sizeof(int));
    GArray *col_spans  = g_array_new(FALSE, FALSE, sizeof(int));
    GArray *row_starts = g_array_new(FALSE, FALSE, sizeof(int));
    GArray *row_spans  = g_array_new(FALSE, FALSE, sizeof(int));
    GArray *placed_cols = g_array_new(FALSE, FALSE, sizeof(int));
    GArray *placed_rows = g_array_new(FALSE, FALSE, sizeof(int));
    GArray *item_heights = g_array_new(FALSE, FALSE, sizeof(double));
    typedef struct { double top, height; } grid_row;
    GArray *grid_rows = g_array_new(FALSE, FALSE, sizeof(grid_row));
    for (ns_box *c = box->first_child; c; c = c->next_sibling) {
        int s = -1, sp = 1;
        int rs_start = -1, rs = 1;
        if (c->style) {
            grid_lines col_lines = { &cols_buf, areas, FALSE };
            grid_lines row_lines = { rows_template, areas, TRUE };
            g_grid_lines = &col_lines;
            int got = grid_resolve_pos(c->style, NS_CSS_GRID_COLUMN,
                                       NS_CSS_GRID_COLUMN_START,
                                       NS_CSS_GRID_COLUMN_END, n_cols,
                                       &s, &sp);
            if (!got) s = -1;
            if (sp > NS_CSS_TRACKS_MAX) sp = NS_CSS_TRACKS_MAX;
            g_grid_lines = &row_lines;
            got = grid_resolve_pos(c->style, NS_CSS_GRID_ROW,
                                   NS_CSS_GRID_ROW_START,
                                   NS_CSS_GRID_ROW_END, row_line_tracks,
                                   &rs_start, &rs);
            g_grid_lines = NULL;
            if (!got) rs_start = -1;
            if (rs < 1) rs = 1;
        }
        g_ptr_array_add(items, c);
        g_array_append_val(col_starts, s);
        g_array_append_val(col_spans, sp);
        g_array_append_val(row_starts, rs_start);
        g_array_append_val(row_spans, rs);
    }

    if (!cols_subgrid) {
        int max_end = 0;
        for (guint i = 0; i < items->len; i++) {
            int s = g_array_index(col_starts, int, i);
            int sp = g_array_index(col_spans, int, i);
            if (sp < 1) sp = 1;
            int e = (s >= 0 ? s : 0) + sp;
            if (e > max_end) max_end = e;
        }
        if (max_end > NS_CSS_TRACKS_MAX) max_end = NS_CSS_TRACKS_MAX;
        if (max_end > n_cols) {
            grid_extend_with_auto_tracks(&cols_buf, n_cols, max_end,
                box->style->values[NS_CSS_GRID_AUTO_COLUMNS]);
            n_cols = max_end;
            avail = cw - (n_cols > 1 ? col_gap * (n_cols - 1) : 0);
            if (avail < 0) avail = 0;
        }
    }

    const char *auto_flow = box->style
        ? ns_style_keyword(box->style, NS_CSS_GRID_AUTO_FLOW) : NULL;
    gboolean dense = auto_flow && strstr(auto_flow, "dense") != NULL;
    gboolean col_flow = auto_flow && strstr(auto_flow, "column") != NULL &&
                        !cols_subgrid && !rows_subgrid;
    if (col_flow) {
        int flow_rows = rows_template ? rows_template->n : 0;
        if (flow_rows <= 0) flow_rows = 1;
        if (flow_rows > NS_CSS_TRACKS_MAX) flow_rows = NS_CSS_TRACKS_MAX;
        int tmpl_cols = (cols_v && cols_v->kind == NS_CSS_V_TRACKS &&
                         !cols_v->u.tracks.subgrid)
                        ? cols->n : 0;
        gboolean occ[NS_CSS_TRACKS_MAX][NS_CSS_TRACKS_MAX] = {{FALSE}};
        int cur_col = 0, cur_row = 0;
        int used_cols = tmpl_cols;
        for (guint i = 0; i < items->len; i++) {
            int s = g_array_index(col_starts, int, i);
            int sp = g_array_index(col_spans, int, i);
            int rs_start = g_array_index(row_starts, int, i);
            int rs = g_array_index(row_spans, int, i);
            if (sp < 1) sp = 1;
            if (sp > NS_CSS_TRACKS_MAX) sp = NS_CSS_TRACKS_MAX;
            if (rs < 1) rs = 1;
            if (rs > flow_rows) rs = flow_rows;
            gboolean fixed_col = s >= 0 && s + sp <= NS_CSS_TRACKS_MAX;
            gboolean fixed_row = rs_start >= 0 && rs_start + rs <= flow_rows;
            int pc = -1, pr = -1;
            int c0 = fixed_col ? s : (dense ? 0 : cur_col);
            for (int c = c0; c + sp <= NS_CSS_TRACKS_MAX && pc < 0; c++) {
                if (fixed_col && c != s) break;
                int r0 = fixed_row ? rs_start
                       : (!fixed_col && !dense && c == c0) ? cur_row : 0;
                for (int r = r0; r + rs <= flow_rows; r++) {
                    if (fixed_row && r != rs_start) break;
                    gboolean is_free = TRUE;
                    for (int rr = r; rr < r + rs && is_free; rr++)
                        for (int cc = c; cc < c + sp && is_free; cc++)
                            if (occ[rr][cc]) is_free = FALSE;
                    if (is_free) { pc = c; pr = r; break; }
                }
            }
            if (pc < 0) { pc = NS_CSS_TRACKS_MAX - sp; pr = 0; }
            for (int rr = pr; rr < pr + rs; rr++)
                for (int cc = pc; cc < pc + sp; cc++)
                    occ[rr][cc] = TRUE;
            g_array_append_val(placed_cols, pc);
            g_array_append_val(placed_rows, pr);
            if (pc + sp > used_cols) used_cols = pc + sp;
            if (!fixed_col || !fixed_row) {
                cur_col = pc;
                cur_row = pr + rs;
                if (cur_row >= flow_rows) { cur_col = pc + sp; cur_row = 0; }
            }
        }
        if (used_cols < 1) used_cols = 1;
        if (used_cols > NS_CSS_TRACKS_MAX) used_cols = NS_CSS_TRACKS_MAX;
        const ns_css_value *acv =
            box->style->values[NS_CSS_GRID_AUTO_COLUMNS];
        const ns_css_tracks *auto_cols =
            (acv && acv->kind == NS_CSS_V_TRACKS && acv->u.tracks.n > 0 &&
             !acv->u.tracks.subgrid)
            ? &acv->u.tracks : NULL;
        ns_css_tracks flow_cols = {0};
        flow_cols.n = used_cols;
        for (int c = 0; c < used_cols; c++) {
            if (c < tmpl_cols)
                flow_cols.tracks[c] = cols->tracks[c];
            else if (auto_cols)
                flow_cols.tracks[c] =
                    auto_cols->tracks[(c - tmpl_cols) % auto_cols->n];
            else
                flow_cols.tracks[c].kind = NS_CSS_TRACK_AUTO;
        }
        cols_buf = flow_cols;
        n_cols = used_cols;
        avail = cw - (n_cols > 1 ? col_gap * (n_cols - 1) : 0);
        if (avail < 0) avail = 0;
    }

    const ns_css_value *auto_rows_v = box->style
        ? box->style->values[NS_CSS_GRID_AUTO_ROWS] : NULL;
    const ns_css_tracks *rows_tracks = rows_template;
    const ns_css_tracks *auto_rows_tracks =
        (!rows_subgrid && auto_rows_v && auto_rows_v->kind == NS_CSS_V_TRACKS &&
         !auto_rows_v->u.tracks.subgrid && auto_rows_v->u.tracks.n > 0)
        ? &auto_rows_v->u.tracks : NULL;
    int explicit_rows = rows_subgrid ? sgr->n : (rows_tracks ? rows_tracks->n : 0);
    GArray *occupied = grid_occupancy_new();
    int auto_row = 0;
    int auto_col = 0;
    int n_rows = explicit_rows;
    if (areas && !rows_subgrid && areas->n_rows > n_rows)
        n_rows = MIN(areas->n_rows, NS_GRID_ROWS_MAX);
    if (col_flow) {
        for (guint i = 0; i < items->len; i++) {
            int pr = g_array_index(placed_rows, int, i);
            int rs = g_array_index(row_spans, int, i);
            if (rs < 1) rs = 1;
            if (pr + rs > n_rows) n_rows = pr + rs;
        }
        if (n_rows < 1) n_rows = 1;
    }
    if (!col_flow) {
        g_array_set_size(placed_rows, items->len);
        g_array_set_size(placed_cols, items->len);
    }
    for (guint step = 0; !col_flow && step < 3 * items->len; step++) {
        guint i = step % items->len;
        guint phase = step / items->len;
        int s = g_array_index(col_starts, int, i);
        int sp = g_array_index(col_spans, int, i);
        int rs_start = g_array_index(row_starts, int, i);
        int rs = g_array_index(row_spans, int, i);
        if (sp < 1) sp = 1;
        if (sp > n_cols) sp = n_cols;
        if (rs < 1) rs = 1;
        if (rs > NS_GRID_ROWS_MAX) rs = NS_GRID_ROWS_MAX;
        gboolean fixed_col = s >= 0 && s + sp <= n_cols;
        gboolean fixed_row = rs_start >= 0 && rs_start < NS_GRID_ROWS_MAX;
        guint item_phase = fixed_row ? (fixed_col ? 0 : 1) : 2;
        if (phase != item_phase) continue;
        int start_row = fixed_row ? rs_start : (dense ? 0 : auto_row);
        int start_col = fixed_col ? s : (fixed_row || dense ? 0 : auto_col);
        int placed_row = start_row;
        int placed_col = fixed_col ? s : 0;
        if (!grid_find_slot(occupied, &placed_row, &placed_col,
                            sp, rs, n_cols, start_row, start_col,
                            fixed_row, fixed_col)) {
            if (placed_row < 0) placed_row = 0;
            if (placed_col < 0) placed_col = 0;
            if (placed_col + sp > n_cols) placed_col = n_cols - sp;
            if (placed_row + rs > NS_GRID_ROWS_MAX)
                placed_row = NS_GRID_ROWS_MAX - rs;
            if (placed_row < 0) placed_row = 0;
        }
        grid_slot_mark(occupied, placed_row, placed_col, sp, rs, n_cols);
        g_array_index(placed_rows, int, i) = placed_row;
        g_array_index(placed_cols, int, i) = placed_col;
        if (placed_row + rs > n_rows) n_rows = placed_row + rs;
        if (phase < 2) continue;
        auto_row = placed_row;
        auto_col = placed_col + sp;
        grid_advance_cursor(occupied, &auto_row, &auto_col, n_cols);
    }
    g_array_free(occupied, TRUE);
    if (n_rows > NS_GRID_ROWS_MAX) n_rows = NS_GRID_ROWS_MAX;
    if (rows_subgrid && n_rows > sgr->n) n_rows = sgr->n;

    if (row_fit_count > 0 && rows_template == &rows_buf) {
        gboolean used[NS_CSS_TRACKS_MAX] = {0};
        for (guint k = 0; k < placed_rows->len; k++) {
            int r0 = g_array_index(placed_rows, int, k);
            int rs = k < row_spans->len ? g_array_index(row_spans, int, k) : 1;
            for (int j = 0; j < rs && r0 + j < rows_buf.n; j++)
                if (r0 + j >= 0) used[r0 + j] = TRUE;
        }
        for (int t = row_fit_start;
             t < row_fit_start + row_fit_count && t < rows_buf.n; t++) {
            if (used[t]) continue;
            rows_buf.tracks[t].kind = NS_CSS_TRACK_PX;
            rows_buf.tracks[t].v = 0;
            rows_buf.tracks[t].pct = 0;
            rows_buf.tracks[t].has_min = FALSE;
        }
    }
    gboolean col_collapsed[NS_CSS_TRACKS_MAX] = {0};
    double col_gap_after[NS_CSS_TRACKS_MAX + 1] = {0};
    if (fit_count > 0 && !cols_subgrid) {
        gboolean used[NS_CSS_TRACKS_MAX] = {0};
        for (guint k = 0; k < placed_cols->len; k++) {
            int c0 = g_array_index(placed_cols, int, k);
            int sp = k < col_spans->len ? g_array_index(col_spans, int, k) : 1;
            for (int j = 0; j < sp && c0 + j < n_cols; j++)
                if (c0 + j >= 0) used[c0 + j] = TRUE;
        }
        for (int t = fit_start; t < fit_start + fit_count && t < n_cols; t++) {
            if (used[t]) continue;
            col_collapsed[t] = TRUE;
            cols_buf.tracks[t].kind = NS_CSS_TRACK_PX;
            cols_buf.tracks[t].v = 0;
            cols_buf.tracks[t].has_min = FALSE;
        }
    }
    {
        double gaps_total = 0;
        for (int t = 0; t < n_cols; t++) {
            gboolean later = FALSE;
            for (int u = t + 1; u < n_cols; u++)
                if (!col_collapsed[u]) { later = TRUE; break; }
            col_gap_after[t] = (!col_collapsed[t] && later) ? col_gap : 0;
            gaps_total += col_gap_after[t];
        }
        avail = cw - gaps_total;
        if (avail < 0) avail = 0;
    }

    double col_content[NS_CSS_TRACKS_MAX] = {0};
    double col_min[NS_CSS_TRACKS_MAX] = {0};
    gboolean any_auto_content = FALSE;
    GArray *col_items = grid_col_items(items, placed_cols, col_spans,
                                       child_inherited);
    for (int t = 0; t < n_cols; t++) {
        gboolean fr_auto = fr_track_min_is_auto(&cols->tracks[t]);
        if (!track_is_intrinsic(cols->tracks[t].kind) &&
            !(cols->tracks[t].has_min &&
              track_is_intrinsic(cols->tracks[t].min_kind)) && !fr_auto)
            continue;
        for (guint k = 0; k < col_items->len; k++) {
            const grid_col_item *ci = &g_array_index(col_items, grid_col_item, k);
            if (ci->col != t || ci->span != 1) continue;
            ns_box *c = ci->box;
            if (fr_auto && !grid_item_has_auto_minimum(c)) {
                any_auto_content = TRUE;
                continue;
            }
            double nw = measure_natural_width(c, ci->inherited);
            double mw = min_width_of(c, ci->inherited);
            if (c->style) {
                ns_edges m = {0}, pd = {0}, bd = {0};
                edges_from_style(c->style, mw, &m, &pd, &bd);
                double extra = m.left + m.right + pd.left + pd.right +
                               bd.left + bd.right;
                mw += extra;
                nw += extra;
            }
            mw += ci->extra;
            nw += ci->extra;
            if (nw > avail) nw = avail;
            if (nw > col_content[t]) col_content[t] = nw;
            if (mw > col_min[t]) col_min[t] = mw;
            any_auto_content = TRUE;
        }
    }
    for (int span = 2; span <= n_cols; span++) {
        for (guint k = 0; k < col_items->len; k++) {
            const grid_col_item *ci = &g_array_index(col_items, grid_col_item, k);
            int c0 = ci->col;
            if (c0 < 0 || ci->span != span || c0 + span > n_cols)
                continue;
            ns_box *c = ci->box;
            double nw = measure_natural_width(c, ci->inherited);
            double mw = min_width_of(c, ci->inherited);
            if (c->style) {
                ns_edges m = {0}, pd = {0}, bd = {0};
                edges_from_style(c->style, mw, &m, &pd, &bd);
                double extra = m.left + m.right + pd.left + pd.right +
                               bd.left + bd.right;
                mw += extra;
                nw += extra;
            }
            mw += ci->extra;
            nw += ci->extra;
            if (nw > avail) nw = avail;
            if (grid_span_accommodate(cols, c0, span, col_gap_after, avail,
                                      mw, nw, col_min, col_content))
                any_auto_content = TRUE;
        }
    }
    g_array_free(col_items, TRUE);
    resolve_track_sizes_full(cols, avail, cw,
                             any_auto_content ? col_min : NULL,
                             any_auto_content ? col_content : NULL, col_sizes,
                             justify_kw_stretches_auto(
                                 keyword_or(box->style, NS_CSS_JUSTIFY_CONTENT,
                                            "normal")));

    double col_x[NS_CSS_TRACKS_MAX + 1];
    col_x[0] = inner_x;
    for (int i = 0; i < n_cols; i++)
        col_x[i + 1] = col_x[i] + col_sizes[i] + col_gap_after[i];

    if (cols_subgrid) {
        n_cols = sg->n;
        col_gap = sg->gap;
        for (int i = 0; i < n_cols; i++) {
            col_sizes[i] = sg->sizes[i];
            col_x[i] = sg->x[i];
        }
        col_x[n_cols] = sg->x[n_cols];
    } else {
        double used_w = 0;
        for (int t = 0; t < n_cols; t++) used_w += col_sizes[t];
        for (int t = 0; t < n_cols; t++) used_w += col_gap_after[t];
        double free_w = cw - used_w;
        if (free_w > 0.5) {
            const char *jc = keyword_or(box->style, NS_CSS_JUSTIFY_CONTENT,
                                        "start");
            double off = 0, extra_gap = 0;
            if (strcmp(jc, "center") == 0) {
                off = free_w / 2.0;
            } else if (strcmp(jc, "end") == 0 || strcmp(jc, "flex-end") == 0 ||
                       strcmp(jc, "right") == 0) {
                off = free_w;
            } else if (strcmp(jc, "space-between") == 0 && n_cols > 1) {
                extra_gap = free_w / (n_cols - 1);
            } else if (strcmp(jc, "space-around") == 0 && n_cols > 0) {
                extra_gap = free_w / n_cols;
                off = extra_gap / 2.0;
            } else if (strcmp(jc, "space-evenly") == 0 && n_cols > 0) {
                extra_gap = free_w / (n_cols + 1);
                off = extra_gap;
            }
            if (off != 0 || extra_gap != 0) {
                col_x[0] = inner_x + off;
                for (int t = 0; t < n_cols; t++)
                    col_x[t + 1] = col_x[t] + col_sizes[t] + col_gap_after[t] + extra_gap;
            }
        }
    }
    gboolean grid_rtl = !cols_subgrid &&
        strcmp(keyword_or(box->style, NS_CSS_DIRECTION, "ltr"), "rtl") == 0;
    if (grid_rtl) {
        double right = inner_x + cw;
        for (int t = 0; t < n_cols; t++)
            col_x[t] = right - (col_x[t] - inner_x) - col_sizes[t];
    }

    double *base_row_height = g_new0(double, n_rows + 1);
    for (int r = 0; r < n_rows; r++) {
        if (rows_subgrid) {
            base_row_height[r] = sgr->sizes[r];
        } else if (rows_tracks && r < rows_tracks->n) {
            base_row_height[r] = grid_track_px(&rows_tracks->tracks[r], row_basis);
        } else if (auto_rows_tracks) {
            int ar = (r - explicit_rows) % auto_rows_tracks->n;
            if (ar < 0) ar = 0;
            base_row_height[r] = grid_track_px(&auto_rows_tracks->tracks[ar], row_basis);
        }
    }
    double *base_row_y = g_new0(double, n_rows + 1);
    base_row_y[0] = rows_subgrid ? sgr->y[0] : inner_y;
    double base_gap = rows_subgrid ? sgr->gap : row_gap;
    for (int r = 0; r < n_rows; r++)
        base_row_y[r + 1] = base_row_y[r] + base_row_height[r] + base_gap;

    for (guint i = 0; i < items->len; i++) {
        ns_box *c = items->pdata[i];
        int chosen = g_array_index(placed_cols, int, i);
        int placed_row = g_array_index(placed_rows, int, i);
        int sp = g_array_index(col_spans, int, i);
        int rs = g_array_index(row_spans, int, i);
        if (sp < 1) sp = 1;
        if (sp > n_cols) sp = n_cols;
        if (rs < 1) rs = 1;
        if (placed_row < 0) placed_row = 0;
        if (placed_row + rs > n_rows) rs = n_rows - placed_row;
        if (chosen < 0) chosen = 0;
        if (chosen + sp > n_cols) chosen = n_cols - sp;

        double w = 0;
        for (int k = 0; k < sp; k++)
            w += col_sizes[chosen + k] + (k > 0 ? col_gap_after[chosen + k - 1] : 0);
        edges_from_style(c->style, w, &c->margin, &c->padding, &c->border);
        double cw_for_item = w - c->margin.left - c->margin.right;
        if (cw_for_item < 0) cw_for_item = 0;
        c->x = grid_rtl ? col_x[chosen + sp - 1] : col_x[chosen];
        c->y = inner_y;

        const char *jself = c->style
            ? ns_style_keyword(c->style, NS_CSS_JUSTIFY_SELF) : NULL;
        const char *j_eff = (jself && strcmp(jself, "auto") != 0)
            ? jself : keyword_or(box->style, NS_CSS_JUSTIFY_ITEMS, "stretch");
        gboolean j_stretch = !j_eff || strcmp(j_eff, "stretch") == 0 ||
                             strcmp(j_eff, "normal") == 0 ||
                             strcmp(j_eff, "legacy") == 0;
        const ns_css_value *iwv = c->style ? c->style->values[NS_CSS_WIDTH] : NULL;
        gboolean i_has_w = iwv && (iwv->kind == NS_CSS_V_LENGTH ||
                                   iwv->kind == NS_CSS_V_CALC);
        double item_w = cw_for_item;
        if (!j_stretch && !i_has_w) {
            item_w = grid_item_fit_border_width(c, child_inherited, item_w);
        }
        ns_subgrid_cols subctx = {0};
        if (sp >= 1 && box_is_grid_container(c) &&
            style_columns_are_subgrid(c->style)) {
            subctx.n = sp;
            subctx.gap = col_gap;
            for (int k = 0; k <= sp; k++)
                subctx.x[k] = col_x[chosen + k];
            for (int k = 0; k < sp; k++)
                subctx.sizes[k] = col_sizes[chosen + k];
            g_pending_subgrid_cols = &subctx;
        }
        ns_subgrid_rows subrowctx = {0};
        int sub_rs = MIN(rs, NS_CSS_TRACKS_MAX);
        if (sub_rs >= 1 && placed_row + sub_rs <= n_rows &&
            box_is_grid_container(c) &&
            style_rows_are_subgrid(c->style)) {
            gboolean usable = TRUE;
            for (int k = 0; k < sub_rs; k++) {
                if (base_row_height[placed_row + k] <= 0) {
                    usable = FALSE;
                    break;
                }
            }
            if (usable) {
                double child_inner_y = c->y + c->margin.top +
                                       c->border.top + c->padding.top;
                double parent_row_y = base_row_y[placed_row];
                subrowctx.n = sub_rs;
                subrowctx.gap = base_gap;
                for (int k = 0; k <= sub_rs; k++)
                    subrowctx.y[k] = child_inner_y +
                        (base_row_y[placed_row + k] - parent_row_y);
                for (int k = 0; k < sub_rs; k++)
                    subrowctx.sizes[k] = base_row_height[placed_row + k];
                g_pending_subgrid_rows = &subrowctx;
            }
        }
        double area_h = 0;
        for (int k = 0; k < rs && placed_row + k < n_rows; k++) {
            if (base_row_height[placed_row + k] <= 0) { area_h = 0; break; }
            area_h += base_row_height[placed_row + k] + (k > 0 ? base_gap : 0);
        }
        c->cb_height_override = area_h;
        c->cb_width_override = w;
        c->cb_width_set = TRUE;
        grid_preset_stretch_height(c, box, placed_row, rs, area_h, row_basis,
                                   rows_tracks, rows_subgrid);
        layout_box(c, item_w + c->margin.left + c->margin.right, child_inherited);
        g_pending_subgrid_cols = NULL;
        g_pending_subgrid_rows = NULL;
        gboolean auto_h_margin = c->style &&
            (keyword_is(c->style->values[NS_CSS_MARGIN_LEFT], "auto") ||
             keyword_is(c->style->values[NS_CSS_MARGIN_RIGHT], "auto"));
        if ((!j_stretch || i_has_w) && !auto_h_margin) {
            double used_w = c->content_width +
                            c->padding.left + c->padding.right +
                            c->border.left + c->border.right;
            double free_w = cw_for_item - used_w;
            double dx = 0;
            const ns_css_value *jraw = c->style
                ? c->style->values[NS_CSS_JUSTIFY_SELF] : NULL;
            gboolean j_safe = jraw && jraw->kind == NS_CSS_V_KEYWORD &&
                jraw->u.keyword && g_str_has_prefix(jraw->u.keyword, "safe ");
            const char *j_kw = j_stretch || (j_safe && free_w < 0)
                ? "start" : j_eff;
            gboolean item_far = self_start_is_far_side(c->style, TRUE);
            gboolean at_right = FALSE;
            if (strcmp(j_kw, "center") == 0) {
                if (free_w > 0) dx = free_w / 2.0;
            } else if (strcmp(j_kw, "end") == 0 || strcmp(j_kw, "flex-end") == 0) {
                at_right = !grid_rtl;
            } else if (strcmp(j_kw, "start") == 0 || strcmp(j_kw, "flex-start") == 0) {
                at_right = grid_rtl;
            } else if (strcmp(j_kw, "self-end") == 0) {
                at_right = !item_far;
            } else if (strcmp(j_kw, "self-start") == 0) {
                at_right = item_far;
            } else if (strcmp(j_kw, "right") == 0) {
                at_right = TRUE;
            }
            if (at_right && free_w != 0) dx = free_w;
            if (dx != 0) shift_box_tree(c, dx, 0);
        }
        double item_outer = c->content_height +
                            c->padding.top + c->padding.bottom +
                            c->border.top + c->border.bottom +
                            c->margin.top + c->margin.bottom;
        g_array_append_val(item_heights, item_outer);
    }

    gboolean definite_rows = !rows_subgrid && row_basis > 0;
    double *row_height = g_new0(double, n_rows + 1);
    gboolean *row_fixed = g_new0(gboolean, n_rows + 1);
    gboolean *row_flex = g_new0(gboolean, n_rows + 1);
    double *row_fr = g_new0(double, n_rows + 1);
    double *row_flex_factor = g_new0(double, n_rows + 1);
    double *row_limit = g_new(double, n_rows + 1);
    gboolean *row_min_intrinsic = g_new0(gboolean, n_rows + 1);
    gboolean *row_max_intrinsic = g_new0(gboolean, n_rows + 1);
    for (int r = 0; r <= n_rows; r++) row_limit[r] = -1;
    for (int r = 0; r < n_rows; r++) {
        row_flex_factor[r] = -1;
        double fixed = 0;
        const ns_css_track *tk = NULL;
        if (rows_subgrid) {
            fixed = sgr->sizes[r];
        } else if (rows_tracks && r < rows_tracks->n) {
            tk = &rows_tracks->tracks[r];
        } else if (auto_rows_tracks) {
            int ar = (r - explicit_rows) % auto_rows_tracks->n;
            if (ar < 0) ar = 0;
            tk = &auto_rows_tracks->tracks[ar];
        }
        row_min_intrinsic[r] = TRUE;
        row_max_intrinsic[r] = TRUE;
        if (tk) {
            gboolean flex = tk->kind == NS_CSS_TRACK_FR;
            fixed = flex ? track_min_px(tk, row_basis > 0 ? row_basis : 0)
                  : tk->fit_content ? 0
                  : grid_track_px(tk, row_basis);
            if (!flex && !tk->fit_content && tk->has_min &&
                !track_is_intrinsic(tk->min_kind)) {
                row_min_intrinsic[r] = FALSE;
                fixed = MAX(fixed, track_min_px(tk, row_basis > 0 ? row_basis : 0));
            }
            if (!flex && !tk->fit_content && !track_is_intrinsic(tk->kind) &&
                tk->has_min && track_is_intrinsic(tk->min_kind)) {
                row_max_intrinsic[r] = FALSE;
                row_limit[r] = grid_track_px(tk, row_basis);
            }
            row_fixed[r] = grid_track_is_fixed(tk, row_basis) ||
                           (definite_rows && flex && tk->has_min &&
                            !track_is_intrinsic(tk->min_kind));
            row_flex[r] = definite_rows && flex;
            if (flex) {
                row_fr[r] = tk->v > 0 ? tk->v : 1;
                row_flex_factor[r] = tk->v > 0 ? tk->v : 0;
            }
        }
        if (fixed > row_height[r]) row_height[r] = fixed;
    }
    GArray *by_span = grid_items_by_span(row_spans, items->len);
    for (guint bi = 0; bi < by_span->len; bi++) {
        guint i = g_array_index(by_span, guint, bi);
        int row = g_array_index(placed_rows, int, i);
        int rs = g_array_index(row_spans, int, i);
        if (row < 0 || row >= n_rows) continue;
        if (rs < 1) rs = 1;
        if (row + rs > n_rows) rs = n_rows - row;
        double item_outer = g_array_index(item_heights, double, i);
        if (rs == 1 && !definite_rows && row_flex_factor[row] >= 0 &&
            row_flex_factor[row] < 1)
            item_outer = MAX(item_outer * row_flex_factor[row],
                             grid_item_min_block_contribution(
                                 items->pdata[i], item_outer, row_basis));
        double used = row_gap * (rs - 1);
        int growable = 0;
        gboolean crosses_flex = FALSE;
        for (int k = 0; k < rs; k++) {
            used += row_height[row + k];
            if (!row_fixed[row + k]) growable++;
            if (row_flex[row + k] || row_fr[row + k] > 0) crosses_flex = TRUE;
        }
        if (crosses_flex && rs > 1) continue;
        if (item_outer > used && growable > 0) {
            if (rs == 1) {
                if (!row_fixed[row]) row_height[row] += item_outer - used;
            } else {
                grid_distribute_span(row_height + row, row_fixed + row,
                                     row_limit + row, row_min_intrinsic + row,
                                     row_max_intrinsic + row, rs,
                                     item_outer - used);
            }
        }
        if (rs == 1 && !row_fixed[row] && item_outer > row_limit[row])
            row_limit[row] = item_outer;
    }
    g_array_free(by_span, TRUE);
    g_free(row_limit);
    g_free(row_min_intrinsic);
    g_free(row_max_intrinsic);
    for (guint i = 0; i < items->len && !definite_rows; i++) {
        int row = g_array_index(placed_rows, int, i);
        int rs = g_array_index(row_spans, int, i);
        if (row < 0 || row >= n_rows || rs < 2) continue;
        if (row + rs > n_rows) rs = n_rows - row;
        double item_outer = g_array_index(item_heights, double, i);
        double used = row_gap * (rs - 1);
        double fr_total = 0;
        for (int k = 0; k < rs; k++) {
            used += row_height[row + k];
            fr_total += row_fr[row + k];
        }
        if (fr_total <= 0 || item_outer <= used) continue;
        for (int k = 0; k < rs; k++)
            row_height[row + k] += (item_outer - used) * row_fr[row + k] / fr_total;
    }
    g_free(row_fixed);
    g_free(row_flex);
    g_free(row_fr);
    g_free(row_flex_factor);

    if (!rows_subgrid && row_basis > 0 && n_rows > 0) {
        double over = (n_rows > 1 ? row_gap * (n_rows - 1) : 0) - row_basis;
        double *shrinkable = g_new0(double, n_rows + 1);
        double shrink_total = 0;
        for (int r = 0; r < n_rows; r++) {
            over += row_height[r];
            const ns_css_track *tk = NULL;
            if (rows_tracks && r < rows_tracks->n) tk = &rows_tracks->tracks[r];
            else if (auto_rows_tracks && auto_rows_tracks->n > 0)
                tk = &auto_rows_tracks->tracks[(r - explicit_rows) % auto_rows_tracks->n];
            if (tk && tk->has_min &&
                (tk->kind == NS_CSS_TRACK_PX || tk->kind == NS_CSS_TRACK_PERCENT)) {
                double mn = track_min_px(tk, row_basis);
                if (row_height[r] > mn) {
                    shrinkable[r] = row_height[r] - mn;
                    shrink_total += shrinkable[r];
                }
            }
        }
        if (over > 0 && shrink_total > 0) {
            double take = MIN(over, shrink_total);
            for (int r = 0; r < n_rows; r++)
                if (shrinkable[r] > 0)
                    row_height[r] -= take * shrinkable[r] / shrink_total;
        }
        g_free(shrinkable);
        grid_expand_flexible_rows(row_height, n_rows, rows_tracks,
                                  auto_rows_tracks, explicit_rows,
                                  row_basis - (n_rows > 1 ? row_gap * (n_rows - 1) : 0));
    } else if (!rows_subgrid && n_rows > 0) {
        grid_size_flexible_rows_to_content(row_height, n_rows, rows_tracks,
                                           auto_rows_tracks, explicit_rows);
        const ns_css_value *mnv = box->style
            ? box->style->values[NS_CSS_MIN_HEIGHT] : NULL;
        double min_h = mnv && (mnv->kind == NS_CSS_V_LENGTH ||
                               mnv->kind == NS_CSS_V_CALC)
            ? specified_height_to_content(box,
                                          resolve_used_height(box, mnv, cw, -1))
            : -1;
        double gaps = n_rows > 1 ? row_gap * (n_rows - 1) : 0;
        double used = gaps;
        for (int r = 0; r < n_rows; r++) used += row_height[r];
        if (min_h > used)
            grid_expand_flexible_rows(row_height, n_rows, rows_tracks,
                                      auto_rows_tracks, explicit_rows,
                                      min_h - gaps);
    }

    double cursor_y = rows_subgrid ? sgr->y[0] : inner_y;
    for (int r = 0; r < n_rows; r++) {
        grid_row gr = { .top = cursor_y, .height = row_height[r] };
        g_array_append_val(grid_rows, gr);
        cursor_y += row_height[r] + row_gap;
    }
    if (grid_rows->len > 0) cursor_y -= row_gap;

    double measured = cursor_y - inner_y;
    if (!rows_subgrid && row_basis <= 0 && rows_tracks && measured > 0) {
        gboolean changed = FALSE;
        for (int r = 0; r < n_rows && r < rows_tracks->n; r++) {
            const ns_css_track *tk = &rows_tracks->tracks[r];
            if (tk->kind != NS_CSS_TRACK_PERCENT) continue;
            double resolved = tk->v * measured / 100.0;
            if (fabs(resolved - row_height[r]) > 0.01) {
                row_height[r] = resolved;
                changed = TRUE;
            }
        }
        if (changed) {
            double y = inner_y;
            for (guint r = 0; r < grid_rows->len; r++) {
                grid_row *gr = &g_array_index(grid_rows, grid_row, r);
                gr->top = y;
                gr->height = row_height[r];
                y += row_height[r] + row_gap;
            }
        }
    }
    double total_extra = 0;
    {
        double eh = grid_rows->len > 0 ? grid_used_content_height(box, cw) : -1;
        if (eh > measured)
            total_extra = eh - measured;
    }
    const char *acont = keyword_or(box->style, NS_CSS_ALIGN_CONTENT, "stretch");
    gboolean ac_stretch = !acont || strcmp(acont, "stretch") == 0 ||
                          strcmp(acont, "normal") == 0;
    double *row_extra = g_new0(double, grid_rows->len + 1);
    double *row_extra_before = g_new0(double, grid_rows->len + 1);
    if (ac_stretch && grid_rows->len > 0 && total_extra > 0) {
        int stretchable = 0;
        gboolean *row_auto = g_new0(gboolean, grid_rows->len + 1);
        for (guint r = 0; r < grid_rows->len; r++) {
            const ns_css_track *tk = NULL;
            if (rows_subgrid) tk = NULL;
            else if (rows_tracks && (int)r < rows_tracks->n) tk = &rows_tracks->tracks[r];
            else if (auto_rows_tracks && auto_rows_tracks->n > 0)
                tk = &auto_rows_tracks->tracks[((int)r - explicit_rows) % auto_rows_tracks->n];
            row_auto[r] = rows_subgrid ? FALSE
                        : (!tk || tk->kind == NS_CSS_TRACK_AUTO);
            if (row_auto[r]) stretchable++;
        }
        if (stretchable > 0) {
            double share = total_extra / stretchable;
            double before = 0;
            for (guint r = 0; r < grid_rows->len; r++) {
                row_extra_before[r] = before;
                row_extra[r] = row_auto[r] ? share : 0;
                before += row_extra[r];
            }
        }
        g_free(row_auto);
    }
    double group_off = 0, row_between = 0;
    if (!ac_stretch && total_extra > 0) {
        guint n = grid_rows->len;
        if (strcmp(acont, "center") == 0)
            group_off = total_extra / 2.0;
        else if (strcmp(acont, "end") == 0 || strcmp(acont, "flex-end") == 0)
            group_off = total_extra;
        else if (strcmp(acont, "space-between") == 0 && n > 1)
            row_between = total_extra / (n - 1);
        else if (strcmp(acont, "space-around") == 0 && n > 0) {
            row_between = total_extra / n;
            group_off = row_between / 2.0;
        } else if (strcmp(acont, "space-evenly") == 0 && n > 0) {
            row_between = total_extra / (n + 1);
            group_off = row_between;
        }
    }
    for (guint j = 0; j < items->len; j++) {
        ns_box *c = items->pdata[j];
        int r = g_array_index(placed_rows, int, j);
        if (r < 0 || r >= (int)grid_rows->len) continue;
        int span = g_array_index(row_spans, int, j);
        if (span < 1) span = 1;
        double row_h = 0;
        for (int k = 0; k < span && r + k < (int)grid_rows->len; k++) {
            grid_row *gk = &g_array_index(grid_rows, grid_row, r + k);
            row_h += gk->height + row_extra[r + k];
            if (k > 0) row_h += row_gap + row_between;
        }
        const char *a_eff = flex_item_align(
            c, keyword_or(box->style, NS_CSS_ALIGN_ITEMS, "stretch"));
        double item_outer = c->content_height +
                            c->padding.top + c->padding.bottom +
                            c->border.top + c->border.bottom +
                            c->margin.top + c->margin.bottom;
        double free_h = row_h - item_outer;
        double dy_align = 0;
        gboolean mt_auto = c->style &&
            keyword_is(c->style->values[NS_CSS_MARGIN_TOP], "auto");
        gboolean mb_auto = c->style &&
            keyword_is(c->style->values[NS_CSS_MARGIN_BOTTOM], "auto");
        if ((mt_auto || mb_auto) && free_h > 0.5) {
            dy_align = mt_auto && mb_auto ? free_h / 2.0
                     : mt_auto           ? free_h
                                         : 0;
        } else if (grid_item_stretches(box, c)) {
            grid_stretch_item(c, row_h, item_outer, row_basis, span,
                              grid_row_track_definite(rows_tracks, rows_subgrid,
                                                      r, row_basis, TRUE),
                              child_inherited);
        } else if (free_h > 0.5 && strcmp(a_eff, "center") == 0) {
            dy_align = free_h / 2.0;
        } else if (free_h > 0.5 && (strcmp(a_eff, "end") == 0 ||
                                    strcmp(a_eff, "flex-end") == 0 ||
                                    strcmp(a_eff, "last baseline") == 0)) {
            dy_align = free_h;
        } else if (free_h > 0.5 && (strcmp(a_eff, "self-end") == 0 ||
                                    strcmp(a_eff, "self-start") == 0)) {
            gboolean far = self_start_is_far_side(c->style, FALSE);
            if (far == (strcmp(a_eff, "self-start") == 0)) dy_align = free_h;
        }
        grid_row *gr = &g_array_index(grid_rows, grid_row, r);
        double row_top = gr->top + row_extra_before[r] +
                         group_off + row_between * r;
        double target_y = row_top + dy_align;
        double dy = target_y - c->y;
        if (dy != 0) shift_box_tree(c, 0, dy);
    }
    if (total_extra > 0) cursor_y += total_extra;

    if (box->grid_col_tracks) g_array_free(box->grid_col_tracks, TRUE);
    if (box->grid_row_tracks) g_array_free(box->grid_row_tracks, TRUE);
    box->grid_col_tracks = g_array_new(FALSE, FALSE, sizeof(ns_grid_track_edges));
    box->grid_row_tracks = g_array_new(FALSE, FALSE, sizeof(ns_grid_track_edges));
    for (int t = 0; t < n_cols; t++) {
        ns_grid_track_edges e = { col_x[t], col_x[t] + col_sizes[t] };
        g_array_append_val(box->grid_col_tracks, e);
    }
    for (guint r = 0; r < grid_rows->len; r++) {
        const grid_row *gr = &g_array_index(grid_rows, grid_row, r);
        double top = gr->top + row_extra_before[r] + group_off + row_between * r;
        ns_grid_track_edges e = { top, top + gr->height + row_extra[r] };
        g_array_append_val(box->grid_row_tracks, e);
    }
    box->grid_explicit_cols = explicit_cols;
    box->grid_explicit_rows = row_line_tracks;

    *cursor_y_out = cursor_y;
    g_free(base_row_height);
    g_free(base_row_y);
    g_free(row_height);
    g_free(row_extra);
    g_free(row_extra_before);
    g_ptr_array_free(items, TRUE);
    g_array_free(col_starts, TRUE);
    g_array_free(col_spans, TRUE);
    g_array_free(row_starts, TRUE);
    g_array_free(row_spans, TRUE);
    g_array_free(placed_cols, TRUE);
    g_array_free(placed_rows, TRUE);
    g_array_free(item_heights, TRUE);
    g_array_free(grid_rows, TRUE);
}

static gboolean
box_is_block_level_replaced(const ns_box *c)
{
    if (!c || (c->kind != NS_BOX_IMAGE && c->kind != NS_BOX_VIDEO &&
               c->kind != NS_BOX_SVG)) return FALSE;
    return ns_display_is_block_level(ns_css_display_of(c->style));
}

static double
collapsed_margin(double a, double b)
{
    double positive = MAX(a > 0 ? a : 0, b > 0 ? b : 0);
    double negative = MIN(a < 0 ? a : 0, b < 0 ? b : 0);
    return positive + negative;
}

static void
phantom_lines_move_to(ns_box *first, const ns_box *stop, double y)
{
    for (ns_box *c = first; c && c != stop; c = c->next_sibling)
        if (c->phantom_line && c->y != y) shift_box_tree(c, 0, y - c->y);
}

static gboolean
empty_block_collapses_through(const ns_box *box, int clear)
{
    return box && box->kind == NS_BOX_BLOCK && clear == 0 &&
           box->content_height <= 0 &&
           box->padding.top <= 0 && box->padding.bottom <= 0 &&
           box->border.top <= 0 && box->border.bottom <= 0 &&
           !box_establishes_bfc(box);
}

static double
intrinsic_keyword_width(ns_box *box, const char *kw, const ns_style *mi,
                        double avail)
{
    if (!kw) return -1;
    if (avail < 0) avail = 0;
    if (strcmp(kw, "min-content") == 0)
        return min_content_width_of(box, mi);
    if (strcmp(kw, "max-content") == 0)
        return measure_max_content_width(box, mi);
    if (strcmp(kw, "fit-content") == 0) {
        double mn = min_content_width_of(box, mi);
        double mx = measure_max_content_width(box, mi);
        double w = mx < avail ? mx : avail;
        return w < mn ? mn : w;
    }
    if (strcmp(kw, "stretch") == 0 ||
        strcmp(kw, "-webkit-fill-available") == 0 ||
        strcmp(kw, "-moz-available") == 0)
        return avail;
    return -1;
}

static gboolean
size_keyword_is_intrinsic(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           (strcmp(v->u.keyword, "min-content") == 0 ||
            strcmp(v->u.keyword, "max-content") == 0 ||
            strcmp(v->u.keyword, "fit-content") == 0);
}

static gboolean
height_keyword_stretches(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           (strcmp(v->u.keyword, "stretch") == 0 ||
            strcmp(v->u.keyword, "-webkit-fill-available") == 0 ||
            strcmp(v->u.keyword, "-moz-available") == 0);
}

static const char *
legacy_block_align(const ns_box *c, const ns_style *inherited)
{
    const char *ta = inherited ? ns_style_keyword(inherited, NS_CSS_TEXT_ALIGN) : NULL;
    if (!ta) return NULL;
    if (strcmp(ta, "-webkit-center") == 0 || strcmp(ta, "-moz-center") == 0)
        return "center";
    if (strcmp(ta, "-webkit-right") == 0 || strcmp(ta, "-moz-right") == 0)
        return "right";
    if (strcmp(ta, "center") != 0 && strcmp(ta, "right") != 0) return NULL;
    for (const ns_box *p = c->parent; p; p = p->parent) {
        if (p->style) {
            const char *pt = ns_style_keyword(p->style, NS_CSS_TEXT_ALIGN);
            if (!pt || strcmp(pt, ta) != 0) return NULL;
        }
        if (!p->dom || p->dom->kind != NS_NODE_ELEMENT) continue;
        if (ns_node_is_element_named(p->dom, "center")) return ta;
        const char *al = ns_element_get_attr(p->dom, "align");
        if (al && g_ascii_strcasecmp(al, ta) == 0) return ta;
    }
    return NULL;
}

static void
legacy_align_block_child(ns_box *c, double avail_x, double avail_w,
                         const ns_style *inherited)
{
    if (c->kind != NS_BOX_BLOCK && c->kind != NS_BOX_TABLE) return;
    if (style_is_absolute_or_fixed(c->style) || float_side_of(c->style) >= 0)
        return;
    const ns_css_value *ml = c->style ? c->style->values[NS_CSS_MARGIN_LEFT] : NULL;
    const ns_css_value *mr = c->style ? c->style->values[NS_CSS_MARGIN_RIGHT] : NULL;
    if (length_is_auto(ml) || length_is_auto(mr) ||
        c->margin.left != 0 || c->margin.right != 0)
        return;
    double outer = c->content_width + c->padding.left + c->padding.right +
                   c->border.left + c->border.right;
    if (outer >= avail_w - 0.5) return;
    const char *legacy = legacy_block_align(c, inherited);
    if (!legacy) return;
    double target_x;
    if (strcmp(legacy, "center") == 0)
        target_x = avail_x + (avail_w - outer) / 2.0;
    else if (strcmp(legacy, "right") == 0)
        target_x = avail_x + avail_w - outer;
    else
        return;
    shift_box_tree(c, target_x - c->x, 0);
}

static gboolean
block_height_is_auto(const ns_box *box, double width_basis)
{
    if (!box->style) return TRUE;
    if (resolve_used_height(box, box->style->values[NS_CSS_MIN_HEIGHT],
                            width_basis, -1) > 0)
        return FALSE;
    const ns_css_value *hv = box->style->values[NS_CSS_HEIGHT];
    if (height_keyword_stretches(hv))
        return containing_block_definite_height(box) < 0;
    if (hv && (hv->kind == NS_CSS_V_LENGTH || hv->kind == NS_CSS_V_CALC))
        return resolve_used_height(box, hv, width_basis, -1) < 0;
    return TRUE;
}

static ns_box *
fieldset_rendered_legend(const ns_box *box)
{
    for (ns_box *c = box->first_child; c; c = c->next_sibling)
        if (c->is_rendered_legend) return c;
    return NULL;
}

static void
box_detach_child(ns_box *parent, ns_box *child)
{
    ns_box *prev = NULL;
    for (ns_box *c = parent->first_child; c && c != child; c = c->next_sibling)
        prev = c;
    if (prev) prev->next_sibling = child->next_sibling;
    else parent->first_child = child->next_sibling;
    if (parent->last_child == child) parent->last_child = prev;
    child->next_sibling = NULL;
}

static void
box_prepend_child(ns_box *parent, ns_box *child)
{
    child->next_sibling = parent->first_child;
    parent->first_child = child;
    if (!parent->last_child) parent->last_child = child;
}

static double
legend_inline_offset(const ns_box *fieldset, const ns_box *legend,
                     double free_w)
{
    const ns_style *ls = legend->style;
    gboolean ml_auto = ls && length_is_auto(ls->values[NS_CSS_MARGIN_LEFT]);
    gboolean mr_auto = ls && length_is_auto(ls->values[NS_CSS_MARGIN_RIGHT]);
    if (ml_auto && mr_auto) return free_w / 2.0;
    if (ml_auto) return free_w;
    if (mr_auto) return 0;
    const char *js = ls ? ns_style_keyword(ls, NS_CSS_JUSTIFY_SELF) : NULL;
    if (js && g_str_has_prefix(js, "safe ")) js += 5;
    else if (js && g_str_has_prefix(js, "unsafe ")) js += 7;
    gboolean fieldset_rtl = fieldset->style &&
        keyword_is(fieldset->style->values[NS_CSS_DIRECTION], "rtl");
    gboolean legend_rtl = ls && keyword_is(ls->values[NS_CSS_DIRECTION], "rtl");
    gboolean at_end = fieldset_rtl;
    if (!js) return at_end ? free_w : 0;
    if (strcmp(js, "center") == 0) return free_w / 2.0;
    if (strcmp(js, "left") == 0) at_end = FALSE;
    else if (strcmp(js, "right") == 0) at_end = TRUE;
    else if (strcmp(js, "end") == 0 || strcmp(js, "flex-end") == 0)
        at_end = !fieldset_rtl;
    else if (strcmp(js, "self-start") == 0) at_end = legend_rtl;
    else if (strcmp(js, "self-end") == 0) at_end = !legend_rtl;
    return at_end ? free_w : 0;
}

static double
layout_rendered_legend(ns_box *fieldset, ns_box *legend, double cw,
                       double inner_x, const ns_style *inherited)
{
    edges_from_style(legend->style, cw,
                     &legend->margin, &legend->padding, &legend->border);
    double outer_extras = legend->padding.left + legend->padding.right +
                          legend->border.left + legend->border.right +
                          legend->margin.left + legend->margin.right;
    const ns_css_value *wv = legend->style
        ? legend->style->values[NS_CSS_WIDTH] : NULL;
    double layout_w = cw;
    if (!wv || length_is_auto(wv)) {
        double avail = MAX(cw - outer_extras, 0);
        double fit = measure_natural_width(legend, inherited);
        if (fit > avail) fit = avail;
        double floor_w = min_width_of(legend, inherited);
        if (fit < floor_w) fit = floor_w;
        layout_w = fit + outer_extras;
    }
    double top_edge = fieldset->y + fieldset->margin.top;
    legend->x = inner_x;
    legend->y = top_edge;
    layout_box(legend, layout_w, inherited);
    double outer_w = legend->content_width +
                     legend->padding.left + legend->padding.right +
                     legend->border.left + legend->border.right +
                     legend->margin.left + legend->margin.right;
    double border_box_h = legend->content_height +
                          legend->padding.top + legend->padding.bottom +
                          legend->border.top + legend->border.bottom;
    double border_top = fieldset->border.top;
    double legend_top = legend->margin.top +
                        MAX((border_top - border_box_h) / 2.0, 0);
    double target_x = inner_x + legend_inline_offset(fieldset, legend,
                                                     cw - outer_w);
    double target_y = top_edge + legend_top - legend->margin.top;
    shift_box_tree(legend, target_x - legend->x, target_y - legend->y);
    double painted_border_top =
        MAX(legend_top + border_box_h / 2.0 - border_top / 2.0, 0);
    double legend_bottom = legend_top + border_box_h + legend->margin.bottom;
    double extra = MAX(painted_border_top, legend_bottom - border_top);
    return MAX(extra, 0);
}

static void
fieldset_set_content_block_size(ns_box *box, double width_basis,
                                double sizing_extras, double legend_extra)
{
    const ns_css_value *hv = box->style ? box->style->values[NS_CSS_HEIGHT] : NULL;
    if (!hv || (hv->kind != NS_CSS_V_LENGTH && hv->kind != NS_CSS_V_CALC))
        return;
    double h = resolve_used_height(box, hv, width_basis, -1);
    if (h < 0) return;
    h = MAX(h - sizing_extras - legend_extra, 0);
    for (ns_box *c = box->first_child; c; c = c->next_sibling)
        c->cb_height_override = h;
}

gboolean
ns_box_fieldset_legend_gap(const ns_box *fieldset, double *border_inset,
                           double *gap_x0, double *gap_x1,
                           double *gap_y0, double *gap_y1)
{
    const ns_box *legend = fieldset ? fieldset_rendered_legend(fieldset) : NULL;
    if (!legend) return FALSE;
    double top_edge = fieldset->y + fieldset->margin.top;
    double legend_border_top = legend->y + legend->margin.top;
    double border_box_w = legend->content_width +
                          legend->padding.left + legend->padding.right +
                          legend->border.left + legend->border.right;
    double border_box_h = legend->content_height +
                          legend->padding.top + legend->padding.bottom +
                          legend->border.top + legend->border.bottom;
    double inset = MAX(legend_border_top - top_edge + border_box_h / 2.0 -
                       fieldset->border.top / 2.0, 0);
    *border_inset = inset;
    *gap_x0 = legend->x + legend->margin.left;
    *gap_x1 = *gap_x0 + border_box_w;
    *gap_y0 = MIN(legend->y, top_edge + inset);
    *gap_y1 = MAX(legend_border_top + border_box_h + legend->margin.bottom,
                  top_edge + inset + fieldset->border.top);
    return TRUE;
}

static double
block_align_content_shift(const ns_box *box, double free_space)
{
    if (!box->style || free_space == 0) return 0;
    if (box_is_flex_container(box) ||
        box_is_grid_container(box))
        return 0;
    const char *acont = keyword_or(box->style, NS_CSS_ALIGN_CONTENT, "normal");
    gboolean unsafe = g_str_has_prefix(acont, "unsafe ");
    if (unsafe) acont += strlen("unsafe ");
    else if (g_str_has_prefix(acont, "safe ")) acont += strlen("safe ");
    if (free_space < 0 &&
        (!unsafe ||
         overflow_kw_scrolls(overflow_axis_keyword(box->style, NS_CSS_OVERFLOW_Y))))
        return 0;
    if (strcmp(acont, "center") == 0 || strcmp(acont, "space-around") == 0 ||
        strcmp(acont, "space-evenly") == 0)
        return free_space / 2.0;
    if (strcmp(acont, "end") == 0 || strcmp(acont, "flex-end") == 0)
        return free_space;
    return 0;
}

static double
multicol_child_height(const ns_box *c)
{
    if (c->kind == NS_BOX_BLOCK || c->kind == NS_BOX_TABLE)
        return c->content_height + c->padding.top + c->padding.bottom +
               c->border.top + c->border.bottom;
    return c->content_height;
}

static double
multicol_balanced_height(const ns_box *host, int n_cols, double total_h)
{
    double h = total_h / n_cols;
    for (int iter = 0; iter < 256; iter++) {
        int col = 0;
        double y = 0, min_excess = -1;
        for (const ns_box *c = host->first_child; c; c = c->next_sibling) {
            double ch = multicol_child_height(c);
            if (y > 0 && y + ch > h + 0.01) {
                double excess = y + ch - h;
                if (min_excess < 0 || excess < min_excess) min_excess = excess;
                col++;
                y = 0;
            }
            y += ch;
        }
        if (col < n_cols || min_excess <= 0) return h;
        h += min_excess;
    }
    return h;
}

static gboolean
box_is_menulist_select(const ns_box *box)
{
    if (!box || box->kind != NS_BOX_BLOCK ||
        !ns_node_is_element_named(box->dom, "select") ||
        ns_element_get_attr(box->dom, "multiple") ||
        box->dom == g_open_select_for_layout ||
        ns_css_writing_mode(box->style) != 0)
        return FALSE;
    return ns_parse_int(ns_element_get_attr(box->dom, "size"), 0, 0, 1000) <= 1;
}

static gboolean
menulist_themed(const ns_style *s)
{
    return !keyword_is(s ? s->values[NS_CSS_APPEARANCE] : NULL, "none");
}

static double
menulist_content_height(const ns_style *s)
{
    const ns_css_value *lh = s ? s->values[NS_CSS_LINE_HEIGHT] : NULL;
    double line = -1;
    if (!menulist_themed(s) && lh && !keyword_is(lh, "normal"))
        line = ns_paint_css_line_height_px(s);
    ns_css_font_metrics fm = { 0 };
    if (line < 0)
        line = ns_paint_style_font_metrics(s, &fm)
            ? fm.ascent_px + fm.descent_px : 0;
    return line + (menulist_themed(s) ? 2.0 : 0.0);
}

static gboolean
box_content_independent_height(const ns_box *box, double *out)
{
    if (style_block_size_contained(box->style, out)) return TRUE;
    if (!box_is_menulist_select(box)) return FALSE;
    *out = menulist_content_height(box->style);
    return TRUE;
}

static double
menulist_baseline_target(const ns_box *box)
{
    ns_css_font_metrics fm = { 0 };
    ns_paint_style_font_metrics(box->style, &fm);
    double inner = menulist_content_height(box->style);
    double pad = menulist_themed(box->style) ? 1.0 : 0.0;
    double leading = floor((inner - 2 * pad - fm.ascent_px - fm.descent_px) / 2.0);
    return box->margin.top + box->border.top + box->padding.top +
           MAX((box->content_height - inner) / 2.0, 0) + pad + leading +
           fm.ascent_px;
}

static void
center_form_control_content(ns_box *box, double measured)
{
    double baseline;
    if (box_is_menulist_select(box) && box_first_baseline(box, &baseline)) {
        double dy = menulist_baseline_target(box) - baseline;
        for (ns_box *c = box->first_child; c; c = c->next_sibling)
            if (!style_is_absolute_or_fixed(c->style)) shift_box_tree(c, 0, dy);
        return;
    }
    if (!ns_node_is_element_named(box->dom, "input") ||
        box->content_height <= measured + 0.5)
        return;
    double shift = (box->content_height - measured) * 0.5;
    for (ns_box *c = box->first_child; c; c = c->next_sibling)
        c->y += shift;
}

static gboolean
inline_text_ends_with_break(const char *text)
{
    if (!text) return FALSE;
    gsize len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                       text[len - 1] == '\n' || text[len - 1] == '\r'))
        len--;
    return len >= 3 && memcmp(text + len - 3, "\xe2\x80\xa8", 3) == 0;
}

static void
layout_block(ns_box *box, double parent_content_width, const ns_style *inherited_style)
{
    inline_runs_join_splits(box);
    double cb_pct_basis = box_cb_basis(box, parent_content_width);
    edges_from_style(box->style, cb_pct_basis,
                     &box->margin, &box->padding, &box->border);
    box->margin_top_through = 0;

    const ns_css_value *wv  = box->style ? box->style->values[NS_CSS_WIDTH]     : NULL;
    const ns_css_value *mxw = box->style ? box->style->values[NS_CSS_MAX_WIDTH] : NULL;
    const ns_css_value *mnw = box->style ? box->style->values[NS_CSS_MIN_WIDTH] : NULL;
    double horiz_extras = box->padding.left + box->padding.right +
                          box->border.left + box->border.right;
    double horiz_total  = horiz_extras + box->margin.left + box->margin.right;
    double vert_extras = box->padding.top + box->padding.bottom +
                         box->border.top + box->border.bottom;
    double cw;
    gboolean explicit_width = FALSE;
    gboolean intrinsic_width = FALSE;
    gboolean flex_grow_filled = FALSE;
    const char *parent_flex_dir = box->parent
        ? flex_direction_of(box->parent->style) : "row";
    gboolean flex_item = box->parent && box_is_flex_container(box->parent) &&
        !style_is_absolute_or_fixed(box->style);
    gboolean flex_row_item = flex_item &&
        (strcmp(parent_flex_dir, "row") == 0 ||
         strcmp(parent_flex_dir, "row-reverse") == 0);
    gboolean flex_col_item_stretch = flex_item &&
        (strcmp(parent_flex_dir, "column") == 0 ||
         strcmp(parent_flex_dir, "column-reverse") == 0);
    if (flex_col_item_stretch) {
        const char *eff = box->style
            ? ns_style_keyword(box->style, NS_CSS_ALIGN_SELF) : NULL;
        if (!eff || strcmp(eff, "auto") == 0)
            eff = keyword_or(box->parent->style, NS_CSS_ALIGN_ITEMS, "stretch");
        if (strcmp(eff, "stretch") != 0 && strcmp(eff, "normal") != 0)
            flex_col_item_stretch = FALSE;
        if (length_is_auto(box->style ? box->style->values[NS_CSS_MARGIN_LEFT] : NULL) ||
            length_is_auto(box->style ? box->style->values[NS_CSS_MARGIN_RIGHT] : NULL))
            flex_col_item_stretch = FALSE;
    }
    double pct_width_base = cb_pct_basis;
    if (flex_row_item && box->parent->content_width > 0)
        pct_width_base = box->parent->content_width;
    gboolean table_cell = box->kind == NS_BOX_TABLE_CELL;
    if (table_cell) {
        cw = parent_content_width - horiz_total;
        if (cw < 0) cw = 0;
    } else if (box->has_flex_main) {
        box->has_flex_main = FALSE;
        cw = box->flex_main_size;
        if (cw < 0) cw = 0;
        explicit_width = TRUE;
        flex_grow_filled = TRUE;
    } else if (flex_row_item && flex_grow_of(box) > 0) {
        cw = parent_content_width - horiz_total;
        if (cw < 0) cw = 0;
        explicit_width = TRUE;
        flex_grow_filled = TRUE;
    } else if (wv && wv->kind == NS_CSS_V_LENGTH) {
        cw = length_resolve(wv, pct_width_base, 0);
        explicit_width = TRUE;
    } else if (wv && wv->kind == NS_CSS_V_CALC) {
        cw = length_resolve(wv, pct_width_base, 0);
        explicit_width = TRUE;
    } else if (wv && wv->kind == NS_CSS_V_KEYWORD && wv->u.keyword &&
               (strcmp(wv->u.keyword, "stretch") == 0 ||
                strcmp(wv->u.keyword, "-webkit-fill-available") == 0 ||
                strcmp(wv->u.keyword, "-moz-available") == 0)) {
        cw = parent_content_width - horiz_total;
        if (cw < 0) cw = 0;
        explicit_width = TRUE;
        intrinsic_width = TRUE;
    } else if (wv && wv->kind == NS_CSS_V_KEYWORD && wv->u.keyword &&
               (strcmp(wv->u.keyword, "max-content") == 0 ||
                strcmp(wv->u.keyword, "min-content") == 0 ||
                strcmp(wv->u.keyword, "fit-content") == 0)) {
        const ns_style *mi = inherited_style ? inherited_style : box->style;
        double avail = parent_content_width - horiz_total;
        if (avail < 0) avail = 0;
        if (strcmp(wv->u.keyword, "min-content") == 0) {
            cw = min_width_of(box, mi);
        } else if (strcmp(wv->u.keyword, "max-content") == 0) {
            cw = measure_natural_width(box, mi);
        } else {
            double mn = min_width_of(box, mi);
            double mx = measure_natural_width(box, mi);
            cw = mx < avail ? mx : avail;
            if (cw < mn) cw = mn;
        }
        if (cw < 0) cw = 0;
        explicit_width = TRUE;
        intrinsic_width = TRUE;
    } else if (!flex_col_item_stretch &&
               display_is_atomic_inline_container(
                   ns_css_display_of(box->style))) {
        double avail = parent_content_width - horiz_total;
        if (avail < 0) avail = 0;
        if (height_keyword_stretches(wv)) {
            cw = avail;
            explicit_width = TRUE;
            intrinsic_width = TRUE;
        } else {
            cw = atomic_inline_fit_width(box,
                                         inherited_style ? inherited_style : box->style,
                                         avail);
        }
        if (cw < 0) cw = 0;
    } else if (!flex_col_item_stretch && ratio_transferred_width(box) >= 0) {
        cw = ratio_transferred_width(box);
        explicit_width = TRUE;
    } else {
        cw = parent_content_width - horiz_total;
        if (cw < 0) cw = 0;
    }
    gboolean border_box = FALSE;
    if (box->style && box->style->values[NS_CSS_BOX_SIZING] &&
        box->style->values[NS_CSS_BOX_SIZING]->kind == NS_CSS_V_KEYWORD &&
        strcmp(box->style->values[NS_CSS_BOX_SIZING]->u.keyword, "border-box") == 0)
        border_box = TRUE;
    if (border_box && explicit_width && !intrinsic_width && !flex_grow_filled) {
        cw -= horiz_extras;
        if (cw < 0) cw = 0;
    }
    double max_cw = table_cell ? -1 : length_resolve(mxw, pct_width_base, -1);
    if (!table_cell && max_cw < 0 && mxw && mxw->kind == NS_CSS_V_KEYWORD) {
        max_cw = intrinsic_keyword_width(box, mxw->u.keyword,
                                         inherited_style ? inherited_style : box->style,
                                         parent_content_width - horiz_total);
        if (max_cw >= 0 && border_box) max_cw += horiz_extras;
    }
    if (max_cw >= 0) {
        if (border_box) max_cw -= horiz_extras;
        if (max_cw >= 0 && cw > max_cw) { cw = max_cw; explicit_width = TRUE; }
    }
    double min_cw = table_cell ? -1 : length_resolve(mnw, pct_width_base, -1);
    if (!table_cell && min_cw < 0 && mnw && mnw->kind == NS_CSS_V_KEYWORD) {
        min_cw = intrinsic_keyword_width(box, mnw->u.keyword,
                                         inherited_style ? inherited_style : box->style,
                                         parent_content_width - horiz_total);
        if (min_cw >= 0 && border_box) min_cw += horiz_extras;
    }
    if (min_cw >= 0) {
        if (border_box) min_cw -= horiz_extras;
        if (min_cw >= 0 && cw < min_cw) { cw = min_cw; explicit_width = TRUE; }
    }
    box->content_width = cw;

    if (explicit_width && !style_is_absolute_or_fixed(box->style) &&
        ns_css_display_of(box->style).outer != NS_DISPLAY_OUTER_INLINE) {
        gboolean ml_auto = length_is_auto(box->style ? box->style->values[NS_CSS_MARGIN_LEFT]  : NULL);
        gboolean mr_auto = length_is_auto(box->style ? box->style->values[NS_CSS_MARGIN_RIGHT] : NULL);
        double available = parent_content_width - cw - horiz_extras;
        if (available < 0) available = 0;
        if (ml_auto && mr_auto) {
            box->margin.left  = available / 2.0;
            box->margin.right = available / 2.0;
        } else if (ml_auto) {
            box->margin.left  = available - box->margin.right;
            if (box->margin.left < 0) box->margin.left = 0;
        } else if (mr_auto) {
            box->margin.right = available - box->margin.left;
            if (box->margin.right < 0) box->margin.right = 0;
        }
    }

    double inner_x = box->x + box->margin.left + box->border.left + box->padding.left;
    double inner_y = box->y + box->margin.top  + box->border.top  + box->padding.top;
    const ns_style *child_inherited = box->style ? box->style : inherited_style;
    ns_box *legend = fieldset_rendered_legend(box);
    double legend_extra = 0;
    if (legend) {
        legend_extra = layout_rendered_legend(box, legend, cw, inner_x,
                                              child_inherited);
        inner_y += legend_extra;
        box_detach_child(box, legend);
        fieldset_set_content_block_size(box, parent_content_width,
                                        border_box ? vert_extras : 0,
                                        legend_extra);
    }
    double cursor_y = inner_y;
    double prev_margin_bottom = 0;
    double top_through = 0;
    double float_shift = 0;
    double outer_margin = g_margin_above.box == box ? g_margin_above.collapsed
                                                    : box->margin.top;
    gboolean collapse_top_with_parent =
        box->padding.top == 0 && box->border.top == 0 &&
        !box_establishes_bfc(box);
    gboolean collapse_bottom_with_parent =
        box->padding.bottom == 0 && box->border.bottom == 0 &&
        box->parent && !box_establishes_bfc(box) &&
        block_height_is_auto(box, parent_content_width);

    if (box_is_flex_container(box) || box_is_grid_container(box))
        reorder_children_by_order(box);

    if (box_is_flex_container(box)) {
        const char *dir = flex_direction_of(box->style);
        gboolean is_row = strcmp(dir, "row") == 0 || strcmp(dir, "row-reverse") == 0;
        gboolean is_col = strcmp(dir, "column") == 0 || strcmp(dir, "column-reverse") == 0;
        if (is_row) {
            if (flex_wraps(box->style))
                layout_flex_row_wrap(box, cw, inner_x, inner_y, child_inherited,
                                     strcmp(dir, "row-reverse") == 0, &cursor_y);
            else
                layout_flex_row(box, cw, inner_x, inner_y, child_inherited,
                                strcmp(dir, "row-reverse") == 0,
                                parent_content_width, &cursor_y);
            goto flex_done;
        }
        if (is_col) {
            layout_flex_column(box, cw, inner_x, inner_y, child_inherited,
                               strcmp(dir, "column-reverse") == 0,
                               parent_content_width, &cursor_y);
            goto flex_done;
        }
    }

    if (box_is_grid_container(box) &&
        g_grid_nesting < NS_GRID_NESTING_MAX) {
        g_grid_nesting++;
        layout_grid(box, cw, inner_x, inner_y, child_inherited, &cursor_y);
        g_grid_nesting--;
        goto flex_done;
    }

    double col_gap = 16;
    int n_cols = box->style ? ns_css_used_column_count(box->style, cw, &col_gap)
                            : 1;
    ns_box *column_host = NULL;
    if (n_cols > 1) {
        int distributable = multicol_distributable_children(box);
        gboolean single_fragmentable_inline =
            box->first_child && !box->first_child->next_sibling &&
            inline_box_can_fragment_multicol(box->first_child,
                                             child_inherited, cw);
        if (distributable < 2 && !single_fragmentable_inline) {
            column_host = multicol_column_host(box);
            if (!column_host) n_cols = 1;
        }
    }
    if (n_cols > 1) {
        double col_w = (cw - col_gap * (n_cols - 1)) / n_cols;
        if (col_w > 1) cw = col_w;
        else n_cols = 1;
    }
    box->columns = n_cols;

    GArray *floats = g_array_new(FALSE, FALSE, sizeof(float_ref));
    if (g_inherited_floats.box == box)
        floats_inherit(floats, &g_inherited_floats, inner_x, cw, inner_y);
    double inline_line_top = -1;
    const ns_box *top_collapse_child = NULL;

    for (ns_box *c = box->first_child; c; c = c->next_sibling) {
        c->x = inner_x;
        int fside = float_side_of(c->style);
        int clr = clear_kind_of(c->style);
        if (fside >= 0 && (c->kind == NS_BOX_BLOCK || c->kind == NS_BOX_TABLE ||
                           c->kind == NS_BOX_IMAGE || c->kind == NS_BOX_VIDEO ||
                           c->kind == NS_BOX_SVG)) {
            edges_from_style(c->style, cw,
                             &c->margin, &c->padding, &c->border);
            double float_max_w = cw;
            double float_floor_w = 0;
            double cw_for_float;
            const ns_css_value *wv2 = c->style ? c->style->values[NS_CSS_WIDTH] : NULL;
            const ns_css_value *mxw2 = c->style ? c->style->values[NS_CSS_MAX_WIDTH] : NULL;
            const ns_css_value *mnw2 = c->style ? c->style->values[NS_CSS_MIN_WIDTH] : NULL;
            double float_sizing_extras = flex_box_is_border_box(c)
                ? c->padding.left + c->padding.right
                  + c->border.left + c->border.right
                : 0;
            if (wv2 && (wv2->kind == NS_CSS_V_LENGTH || wv2->kind == NS_CSS_V_CALC)) {
                cw_for_float = length_resolve(wv2, cw, 0) - float_sizing_extras;
                if (cw_for_float < 0) cw_for_float = 0;
            } else {
                double cap = float_max_w
                    - c->padding.left - c->padding.right
                    - c->border.left - c->border.right
                    - c->margin.left - c->margin.right;
                if (cap < 0) cap = 0;
                cw_for_float = height_keyword_stretches(wv2)
                    ? cap : measure_natural_width(c, child_inherited);
                if (cw_for_float > cap) cw_for_float = cap;
                if (!height_keyword_stretches(wv2)) {
                    float_floor_w = min_width_of(c, child_inherited);
                    if (cw_for_float < float_floor_w) cw_for_float = float_floor_w;
                }
                if (cw_for_float < 0) cw_for_float = 0;
            }
            if (mxw2 && (mxw2->kind == NS_CSS_V_LENGTH || mxw2->kind == NS_CSS_V_CALC)) {
                double mx = length_resolve(mxw2, cw, -1);
                if (mx >= 0 && cw_for_float > mx - float_sizing_extras)
                    cw_for_float = MAX(mx - float_sizing_extras, 0);
            }
            if (mnw2 && (mnw2->kind == NS_CSS_V_LENGTH || mnw2->kind == NS_CSS_V_CALC)) {
                double mn = length_resolve(mnw2, cw, 0);
                if (mn > 0 && cw_for_float < mn - float_sizing_extras)
                    cw_for_float = mn - float_sizing_extras;
            }
            double avail = cw_for_float
                + c->padding.left + c->padding.right
                + c->border.left + c->border.right
                + c->margin.left + c->margin.right;
            double float_y = cursor_y + prev_margin_bottom;
            if (!clr && inline_line_top >= 0 && inline_line_top < cursor_y)
                float_y = inline_line_top;
            if (clr) {
                double y_after_clear = floats_clear_y(floats, float_y, clr);
                if (y_after_clear > float_y) float_y = y_after_clear;
            }
            double left_off = 0, right_off = 0;
            floats_offsets_at(floats, float_y, &left_off, &right_off);
            while ((avail > cw - left_off - right_off + NS_LAYOUT_UNIT) &&
                   floats->len > 0) {
                double next_y = float_y;
                gboolean advanced = FALSE;
                for (guint i = 0; i < floats->len; i++) {
                    const float_ref *f = &g_array_index(floats, float_ref, i);
                    if (f->bottom > float_y &&
                        (!advanced || f->bottom < next_y)) {
                        next_y = f->bottom;
                        advanced = TRUE;
                    }
                }
                if (!advanced) break;
                float_y = next_y;
                floats_offsets_at(floats, float_y, &left_off, &right_off);
            }
            double cw_capped = cw - left_off - right_off
                - c->margin.left - c->margin.right
                - c->padding.left - c->padding.right
                - c->border.left - c->border.right;
            if (cw_for_float > cw_capped && cw_capped > 0)
                cw_for_float = MAX(cw_capped, float_floor_w);
            double tentative_outer = cw_for_float
                + c->padding.left + c->padding.right
                + c->border.left + c->border.right
                + c->margin.left + c->margin.right;
            if (fside == 0)
                c->x = inner_x + left_off;
            else
                c->x = inner_x + cw - right_off - tentative_outer;
            c->y = float_y;
            c->cb_width_override = cw;
            c->cb_width_set = TRUE;
            c->content_width = cw_for_float;
            gboolean explicit_float_w = wv2 &&
                (wv2->kind == NS_CSS_V_LENGTH || wv2->kind == NS_CSS_V_CALC);
            layout_box(c, explicit_float_w
                       ? cw
                       : cw_for_float
                         + c->padding.left + c->padding.right
                         + c->border.left + c->border.right
                         + c->margin.left + c->margin.right,
                       child_inherited);
            double actual_outer = c->content_width
                + c->padding.left + c->padding.right
                + c->border.left + c->border.right
                + c->margin.left + c->margin.right;
            if (fside == 1) {
                double aligned_x = inner_x + cw - right_off - actual_outer;
                if (aligned_x != c->x) shift_box_tree(c, aligned_x - c->x, 0);
            }
            float_ref fr = {
                .box = c, .side = fside,
                .top = c->y,
                .bottom = c->y + c->margin.top + c->content_height
                    + c->padding.top + c->padding.bottom
                    + c->border.top + c->border.bottom
                    + c->margin.bottom,
                .outer_w = actual_outer,
            };
            g_array_append_val(floats, fr);
            continue;
        }
        if (c->kind == NS_BOX_BLOCK || c->kind == NS_BOX_TABLE ||
            box_is_block_level_replaced(c)) {
            edges_from_style(c->style, cw,
                             &c->margin, &c->padding, &c->border);
            double mt = c->margin.top;
            double gap = collapsed_margin(mt, prev_margin_bottom);
            double c_margin_above = gap;
            gboolean collapses_through_top =
                collapse_top_with_parent && cursor_y == inner_y;
            if (collapses_through_top) {
                c_margin_above = collapsed_margin(outer_margin, gap);
                gap = c_margin_above - outer_margin;
                floats_shift_placed(floats, gap - float_shift);
                float_shift = gap;
                top_through = gap;
            }
            cursor_y += gap;
            if (clr) {
                double y_after_clear = floats_clear_y(floats, cursor_y, clr);
                if (y_after_clear > cursor_y) cursor_y = y_after_clear;
            }
            double left_off = 0, right_off = 0;
            gboolean lines_wrap_floats = c->kind == NS_BOX_BLOCK &&
                                         floats_max_bottom(floats) > cursor_y &&
                                         !box_establishes_bfc(c);
            if (!lines_wrap_floats) {
                floats_offsets_at(floats, cursor_y, &left_off, &right_off);
                floats_advance_to_readable_width(floats, cw, &cursor_y,
                                                 &left_off, &right_off);
            }
            double cw_avail = cw - left_off - right_off;
            if (cw_avail < 0) cw_avail = 0;
            c->x = inner_x + left_off;
            c->y = cursor_y - mt;
            inherited_floats outer_floats = g_inherited_floats;
            margin_above outer_above = g_margin_above;
            if (lines_wrap_floats)
                g_inherited_floats = (inherited_floats){ c, floats, inner_x, cw };
            g_margin_above = (margin_above){ c, c_margin_above };
            layout_box(c, cw_avail, child_inherited);
            g_inherited_floats = outer_floats;
            g_margin_above = outer_above;
            if (box_establishes_bfc(c) && cw_avail > 0) {
                double span_l = 0, span_r = 0;
                double c_h = c->content_height +
                             c->padding.top + c->padding.bottom +
                             c->border.top + c->border.bottom;
                floats_offsets_over(floats, cursor_y, cursor_y + c_h,
                                    &span_l, &span_r);
                if (span_l > left_off || span_r > right_off) {
                    double retry = cw - MAX(span_l, left_off)
                                      - MAX(span_r, right_off);
                    if (retry > 0 && retry < cw_avail) {
                        left_off = MAX(span_l, left_off);
                        right_off = MAX(span_r, right_off);
                        cw_avail = retry;
                        c->x = inner_x + left_off;
                        c->y = cursor_y - mt;
                        layout_box(c, cw_avail, child_inherited);
                    }
                }
            }
            if (keyword_is(box->style ? box->style->values[NS_CSS_DIRECTION]
                                      : NULL, "rtl") &&
                !style_is_absolute_or_fixed(c->style)) {
                double outer = c->margin.left + c->margin.right +
                               c->content_width + c->padding.left +
                               c->padding.right + c->border.left +
                               c->border.right;
                if (fabs(outer - cw_avail) > 0.01)
                    shift_box_tree(c, cw_avail - outer, 0);
            }
            if (c->kind == NS_BOX_BLOCK)
                legacy_align_block_child(c, inner_x + left_off, cw_avail,
                                         child_inherited);
            collect_escaping_floats(c, floats, 0);
            if (empty_block_collapses_through(c, clr)) {
                cursor_y -= gap;
                if (collapses_through_top) top_through = 0;
                prev_margin_bottom = collapsed_margin(
                    collapsed_margin(prev_margin_bottom, mt),
                    c->margin.bottom);
            } else {
                if (collapses_through_top) top_collapse_child = c;
                collapse_top_with_parent = FALSE;
                cursor_y += c->margin_top_through;
                if (collapses_through_top) top_through += c->margin_top_through;
                cursor_y += c->content_height +
                            c->padding.top + c->padding.bottom +
                            c->border.top + c->border.bottom;
                prev_margin_bottom = c->margin.bottom;
            }
            inline_line_top = -1;
        } else {
            double line_y = cursor_y + prev_margin_bottom;
            double left_off = 0, right_off = 0;
            floats_offsets_at(floats, line_y, &left_off, &right_off);
            if (!style_lines_never_wrap(box->style) && left_off + right_off > 0)
                floats_advance_to_fit(floats, cw,
                                      inline_run_first_word_width(c, child_inherited),
                                      &line_y, &left_off, &right_off);
            double cw_avail = cw - left_off - right_off;
            if (cw_avail < 0) cw_avail = 0;
            c->x = inner_x + left_off;
            c->y = line_y;
            g_inline_atomic_cb_w = c->kind == NS_BOX_INLINE ? cw : 0;
            layout_box(c, cw_avail, child_inherited);
            if (cw_avail < cw &&
                inline_run_leading_atomic_outer_width(c) > cw_avail + 0.5 &&
                floats_advance_to_fit(floats, cw,
                                      inline_run_leading_atomic_outer_width(c),
                                      &line_y, &left_off, &right_off)) {
                cw_avail = cw - left_off - right_off;
                if (cw_avail < 0) cw_avail = 0;
                c->x = inner_x + left_off;
                c->y = line_y;
                g_inline_atomic_cb_w = cw;
                layout_box(c, cw_avail, child_inherited);
            }
            g_inline_atomic_cb_w = 0;
            if (c->phantom_line) {
                collect_escaping_floats(c, floats, 0);
                continue;
            }
            cursor_y = line_y;
            prev_margin_bottom = 0;
            if (c->kind == NS_BOX_INLINE && c->text && cw_avail < cw) {
                double band_bottom = floats_band_bottom(floats, cursor_y);
                if (band_bottom > cursor_y &&
                    c->y + c->content_height > band_bottom + 0.5) {
                    gsize split = inline_run_break_before(c,
                                                          band_bottom - c->y);
                    if (split > 0 && split < strlen(c->text)) {
                        inline_run_split(c, split);
                        g_inline_atomic_cb_w = cw;
                        layout_box(c, cw_avail, child_inherited);
                        g_inline_atomic_cb_w = 0;
                    }
                }
            }
            collect_escaping_floats(c, floats, 0);
            double line_height = inline_line_height(child_inherited);
            inline_line_top = inline_text_ends_with_break(c->text)
                ? -1 : c->y + MAX(0, c->content_height - line_height);
            cursor_y += c->content_height;
        }
        if ((c->kind == NS_BOX_IMAGE || c->kind == NS_BOX_VIDEO ||
             c->kind == NS_BOX_SVG || c->kind == NS_BOX_TABLE) &&
            c->content_width < cw) {
            double outer = c->content_width;
            if (c->kind == NS_BOX_TABLE)
                outer += c->padding.left + c->padding.right +
                         c->border.left + c->border.right +
                         c->margin.left + c->margin.right;
            const ns_css_value *ta = child_inherited
                ? child_inherited->values[NS_CSS_TEXT_ALIGN] : NULL;
            gboolean self_center = FALSE, self_right = FALSE;
            if (c->kind == NS_BOX_TABLE) {
                const char *al = c->dom ? ns_element_get_attr(c->dom, "align") : NULL;
                if (al && g_ascii_strcasecmp(al, "center") == 0) self_center = TRUE;
                else if (al && g_ascii_strcasecmp(al, "right") == 0) self_right = TRUE;
                const ns_css_value *ml = c->style ? c->style->values[NS_CSS_MARGIN_LEFT] : NULL;
                const ns_css_value *mr = c->style ? c->style->values[NS_CSS_MARGIN_RIGHT] : NULL;
                if (keyword_is(ml, "auto") && keyword_is(mr, "auto")) self_center = TRUE;
            }
            if ((keyword_is(ta, "center") || self_center) && outer < cw)
                shift_box_tree(c, inner_x + (cw - outer) / 2.0 - c->x, 0);
            else if ((keyword_is(ta, "right") || keyword_is(ta, "end") || self_right) && outer < cw)
                shift_box_tree(c, inner_x + (cw - outer) - c->x, 0);
        }
    }
    if (top_through != 0) {
        phantom_lines_move_to(box->first_child, top_collapse_child,
                              inner_y + top_through);
        box->margin.top += top_through;
        box->margin_top_through = top_through;
        inner_y += top_through;
    }
    if (collapse_bottom_with_parent) {
        box->margin.bottom = collapsed_margin(box->margin.bottom,
                                              prev_margin_bottom);
    } else {
        cursor_y += prev_margin_bottom;
    }

    if (box_establishes_bfc(box) && floats && floats->len > 0) {
        double fb = floats_max_bottom(floats);
        if (fb > cursor_y) cursor_y = fb;
    }
    g_array_free(floats, TRUE);

    if (n_cols > 1 &&
        !layout_multicol_single_inline(box, inner_x, inner_y, cw, col_gap,
                                       n_cols, child_inherited, &cursor_y)) {
        ns_box *host = column_host ? column_host : box;
        double flow_x = column_host && host->first_child
            ? host->first_child->x : inner_x;
        double flow_y = column_host && host->first_child
            ? host->first_child->y : inner_y;
        double total_h = cursor_y - inner_y;
        if (column_host) {
            total_h = 0;
            for (ns_box *c = host->first_child; c; c = c->next_sibling) {
                if (c->kind == NS_BOX_BLOCK || c->kind == NS_BOX_TABLE)
                    total_h += c->content_height +
                               c->padding.top + c->padding.bottom +
                               c->border.top + c->border.bottom;
                else
                    total_h += c->content_height;
            }
        }
        double target_h = multicol_balanced_height(host, n_cols, total_h);
        double cur_y = 0;
        double max_col_h = 0;
        int cur_col = 0;
        for (ns_box *c = host->first_child; c; c = c->next_sibling) {
            double c_full_h = multicol_child_height(c);
            if (cur_col < n_cols - 1 &&
                cur_y > 0 && cur_y + c_full_h > target_h + 0.01) {
                cur_col++;
                cur_y = 0;
            }
            double target_x = flow_x + cur_col * (cw + col_gap);
            double target_y = flow_y + cur_y;
            double dx = target_x - c->x;
            double dy = target_y - c->y;
            if (dx != 0 || dy != 0) shift_box_tree(c, dx, dy);
            cur_y += c_full_h;
            if (cur_y > max_col_h) max_col_h = cur_y;
        }
        box->content_width = cw * n_cols + col_gap * (n_cols - 1);
        if (column_host) {
            column_host->content_width = box->content_width;
            column_host->content_height = max_col_h;
            cursor_y = flow_y + max_col_h +
                       column_host->padding.bottom +
                       column_host->border.bottom +
                       column_host->margin.bottom;
        } else {
            cursor_y = inner_y + max_col_h;
        }
    }

flex_done: ;
    const ns_css_value *hv  = box->style ? box->style->values[NS_CSS_HEIGHT]     : NULL;
    const ns_css_value *mxh = box->style ? box->style->values[NS_CSS_MAX_HEIGHT] : NULL;
    const ns_css_value *mnh = box->style ? box->style->values[NS_CSS_MIN_HEIGHT] : NULL;
    const char *ovx = box->style
        ? overflow_axis_keyword(box->style, NS_CSS_OVERFLOW_X) : NULL;
    const char *ovy = box->style
        ? overflow_axis_keyword(box->style, NS_CSS_OVERFLOW_Y) : NULL;
    gboolean overflow_scrolls  = overflow_kw_scrolls(ovy);
    gboolean overflow_scrolls_x = overflow_kw_scrolls(ovx);
    if (legend) box_prepend_child(box, legend);
    double measured = cursor_y - inner_y + legend_extra;
    box->measured_content_height = measured;
    double contained_h;
    if (box_content_independent_height(box, &contained_h)) measured = contained_h;
    double explicit_h = -1;
    if (hv && (hv->kind == NS_CSS_V_LENGTH || hv->kind == NS_CSS_V_CALC))
        explicit_h = resolve_used_height(box, hv, parent_content_width, -1);
    double stretch_h = -1;
    if (height_keyword_stretches(hv) || height_keyword_stretches(mnh) ||
        height_keyword_stretches(mxh)) {
        double cb_h = containing_block_definite_height(box);
        if (cb_h >= 0) {
            stretch_h = cb_h - box->margin.top - box->margin.bottom
                      - box->border.top - box->border.bottom
                      - box->padding.top - box->padding.bottom;
            if (stretch_h < 0) stretch_h = 0;
        }
    }
    if (explicit_h < 0 && stretch_h >= 0 && height_keyword_stretches(hv)) {
        explicit_h = stretch_h;
        if (border_box) explicit_h += vert_extras;
    }
    if (explicit_h >= 0) {
        if (border_box) {
            explicit_h -= vert_extras;
            if (explicit_h < 0) explicit_h = 0;
        }
        box->content_height = explicit_h;
    } else {
        double ratio = box->style
            ? aspect_ratio_number(box->style->values[NS_CSS_ASPECT_RATIO], NULL) : -1;
        if (ratio > 0 && box->content_width > 0) {
            double aspect_h = box->content_width / ratio;
            gboolean content_floor =
                !(mnh && (mnh->kind == NS_CSS_V_LENGTH ||
                          mnh->kind == NS_CSS_V_CALC)) &&
                !overflow_scrolls &&
                !keyword_is(box->style->values[NS_CSS_OVERFLOW_Y], "hidden");
            box->content_height = content_floor && measured > aspect_h
                                ? measured : aspect_h;
        } else {
            box->content_height = measured;
        }
    }
    double max_h = resolve_used_height(box, mxh, parent_content_width, -1);
    if (border_box && max_h >= 0) {
        max_h -= vert_extras;
        if (max_h < 0) max_h = 0;
    }
    if (max_h < 0 && stretch_h >= 0 && height_keyword_stretches(mxh))
        max_h = stretch_h;
    if (max_h < 0 && size_keyword_is_intrinsic(mxh))
        max_h = measured;
    if (max_h >= 0 && box->content_height > max_h)
        box->content_height = max_h;
    double min_h = resolve_used_height(box, mnh, parent_content_width, -1);
    if (min_h < 0 && stretch_h >= 0 && height_keyword_stretches(mnh))
        min_h = stretch_h + (border_box ? vert_extras : 0);
    if (border_box && min_h >= 0) {
        min_h -= vert_extras;
        if (min_h < 0) min_h = 0;
    }
    if (min_h < 0 && size_keyword_is_intrinsic(mnh))
        min_h = measured;
    if (min_h >= 0 && box->content_height < min_h)
        box->content_height = min_h;
    double align_shift = block_align_content_shift(box, box->content_height - measured);
    if (align_shift != 0) {
        for (ns_box *c = box->first_child; c; c = c->next_sibling) {
            if (!style_is_absolute_or_fixed(c->style))
                shift_box_tree(c, 0, align_shift);
        }
    }
    if (align_shift == 0) center_form_control_content(box, measured);
    if (align_shift == 0 &&
        box->dom && box->dom->kind == NS_NODE_ELEMENT && box->dom->name &&
        strcmp(box->dom->name, "button") == 0 &&
        !keyword_is(box->style ? box->style->values[NS_CSS_APPEARANCE] : NULL,
                    "none") &&
        box->content_height > measured + 0.5) {
        double shift = (box->content_height - measured) * 0.5;
        for (ns_box *c = box->first_child; c; c = c->next_sibling) {
            if (!style_is_absolute_or_fixed(c->style))
                shift_box_tree(c, 0, shift);
        }
    }
    gboolean overflow_hidden_y = keyword_is_hidden_overflow(ovy);
    gboolean overflow_hidden_x = keyword_is_hidden_overflow(ovx);
    if (overflow_scrolls || overflow_hidden_y) {
        if (overflow_scrolls) box->scrolls = TRUE;
        if (box->measured_content_height > box->content_height)
            box->scroll_max_y = box->measured_content_height - box->content_height;
        if (box_is_flex_container(box) || box_is_grid_container(box)) {
            double content_bottom = inner_y + box->content_height;
            double max_bottom = content_bottom;
            for (const ns_box *c = box->first_child; c; c = c->next_sibling) {
                if (style_is_absolute_or_fixed(c->style)) continue;
                double bottom = c->y + c->margin.top + c->content_height +
                                c->padding.top + c->padding.bottom +
                                c->border.top + c->border.bottom +
                                c->margin.bottom;
                if (bottom > max_bottom) max_bottom = bottom;
            }
            double overflow = max_bottom - content_bottom;
            if (overflow > 0.5 && overflow > box->scroll_max_y)
                box->scroll_max_y = overflow;
        }
    }
    if (overflow_scrolls_x || overflow_hidden_x) {
        double content_right = inner_x + box->content_width;
        double padding_right = content_right + box->padding.right;
        double max_right = padding_right;
        for (const ns_box *c = box->first_child; c; c = c->next_sibling) {
            if (style_is_absolute_or_fixed(c->style)) continue;
            double right = c->x + c->margin.left + c->content_width +
                           c->padding.left + c->padding.right +
                           c->border.left + c->border.right;
            double flow_right = MIN(right + c->margin.right,
                                    MAX(right, content_right)) +
                                box->padding.right;
            if (right > max_right) max_right = right;
            if (flow_right > max_right) max_right = flow_right;
        }
        if (max_right > padding_right + 1.0) {
            if (overflow_scrolls_x) box->scrolls = TRUE;
            box->scroll_max_x = max_right - padding_right;
        }
    }
    if (style_content_visibility_hidden(box->style)) {
        double contained_height = explicit_h >= 0 ? explicit_h : 0;
        if (max_h >= 0 && contained_height > max_h)
            contained_height = max_h;
        if (min_h >= 0 && contained_height < min_h)
            contained_height = min_h;
        box->content_height = contained_height;
        box->scrolls = FALSE;
        box->scroll_max_x = 0;
        box->scroll_max_y = 0;
    }
}

typedef struct {
    double w, h;
} ns_frame_viewport;

static GHashTable *g_frame_viewports;

static void
record_frame_viewports_walk(const ns_box *b)
{
    if (!b) return;
    if (b->dom && b->dom->kind == NS_NODE_ELEMENT && b->dom->name &&
        (strcmp(b->dom->name, "iframe") == 0 ||
         strcmp(b->dom->name, "frame") == 0)) {
        ns_frame_viewport *v = g_new0(ns_frame_viewport, 1);
        v->w = b->content_width > 0 ? b->content_width : 0;
        v->h = b->content_height > 0 ? b->content_height : 0;
        g_hash_table_insert(g_frame_viewports, (gpointer)b->dom, v);
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        record_frame_viewports_walk(c);
}

static void
record_frame_viewports(const ns_box *root)
{
    if (!g_frame_viewports)
        g_frame_viewports = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                  NULL, g_free);
    else
        g_hash_table_remove_all(g_frame_viewports);
    record_frame_viewports_walk(root);
}

gboolean
ns_layout_frame_viewport(const ns_node *frame, double *w, double *h)
{
    if (!frame || !g_frame_viewports) return FALSE;
    const ns_frame_viewport *v = g_hash_table_lookup(g_frame_viewports, frame);
    if (!v) return FALSE;
    if (w) *w = v->w;
    if (h) *h = v->h;
    return TRUE;
}

static double
control_char_cell_px(const ns_style *s)
{
    NsPangoLayout *layout = make_pango_layout(s);
    if (!layout) return 0;
    NsPangoContext *ctx = ns_pango_layout_get_context(layout);
    const NsPangoFontDescription *fd =
        ns_pango_layout_get_font_description(layout);
    if (!fd) fd = ns_pango_context_get_font_description(ctx);
    NsPangoFontMetrics *fm = fd ? ns_pango_context_get_metrics(ctx, fd, NULL)
                                : NULL;
    double cell = 0;
    if (fm) {
        cell = (double)ns_pango_font_metrics_get_approximate_char_width(fm) /
               NS_PANGO_SCALE;
        ns_pango_font_metrics_unref(fm);
    }
    g_object_unref(layout);
    return cell;
}

static gboolean
node_is_windowed_text_input(const ns_node *n)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "input") != 0) return FALSE;
    const char *type = ns_element_get_attr(n, "type");
    return !type || !*type ||
        g_ascii_strcasecmp(type, "text") == 0 ||
        g_ascii_strcasecmp(type, "search") == 0 ||
        g_ascii_strcasecmp(type, "email") == 0 ||
        g_ascii_strcasecmp(type, "url") == 0 ||
        g_ascii_strcasecmp(type, "tel") == 0 ||
        g_ascii_strcasecmp(type, "number") == 0 ||
        g_ascii_strcasecmp(type, "password") == 0;
}

static glong
text_input_display_cps(const ns_node *n)
{
    const char *v = ns_input_used_value(n);
    if (!v || !*v) v = ns_element_get_attr(n, "placeholder");
    return (v && *v) ? g_utf8_strlen(v, -1) : 0;
}

static gboolean
control_width_is_definite(const ns_style *s)
{
    double fs = length_or(s->values[NS_CSS_FONT_SIZE], 16);
    return control_dim_px_clamped(s, NS_CSS_WIDTH, NS_CSS_MIN_WIDTH,
                                  NS_CSS_MAX_WIDTH, fs, 0) > 0;
}

static gboolean
collect_text_input_columns(const ns_box *b, GHashTable *cols)
{
    if (!b) return FALSE;
    gboolean changed = FALSE;
    if (b->style && b->content_width > 0 &&
        node_is_windowed_text_input(b->dom)) {
        int have = text_input_size_attr(b->dom);
        glong shown = text_input_display_cps(b->dom);
        double cell = shown > 0 ? control_char_cell_px(b->style) : 0;
        if (cell > 0) {
            int overhead = 2 + text_input_leading_spaces(b->style);
            double cells = floor(b->content_width / cell) - overhead;
            int fit = cells < 1 ? 1
                    : cells > G_MAXINT16 ? G_MAXINT16 : (int)cells;
            gboolean grow = fit > have && shown > have;
            gboolean shrink = fit < have && shown > fit &&
                              control_width_is_definite(b->style);
            if (grow || shrink) {
                g_hash_table_insert(cols, (gpointer)b->dom,
                                    GINT_TO_POINTER(fit));
                changed = TRUE;
            }
        }
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        if (collect_text_input_columns(c, cols)) changed = TRUE;
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            if (collect_text_input_columns(
                    g_array_index(b->inline_atomics, ns_inline_atomic, i).box,
                    cols))
                changed = TRUE;
    return changed;
}

ns_box *
ns_layout_build(const ns_node *doc, GHashTable *styles, double viewport_width,
                const ns_node *focused_input, gsize focused_caret_byte,
                gsize focused_sel_anchor_byte,
                struct ns_image_cache *image_cache, const char *base_url)
{
    g_focused_input_for_layout = focused_input;
    g_focused_is_contenteditable_for_layout =
        focused_input && focused_input->kind == NS_NODE_ELEMENT &&
        focused_input->name &&
        strcmp(focused_input->name, "input") != 0 &&
        strcmp(focused_input->name, "textarea") != 0 &&
        ns_ce_attr_enables(ns_element_get_attr(focused_input,
                                               "contenteditable"));
    g_focused_caret_byte_for_layout = focused_caret_byte;
    g_focused_sel_anchor_byte_for_layout = focused_sel_anchor_byte;
    g_image_cache_for_layout = image_cache;
    g_base_url_for_layout = base_url;
    ns_image_cache_begin_generation(image_cache);
    g_counters_for_layout = build_counter_snapshots(doc, styles);
    ns_paint_i18n_memo_begin();
    ns_box *root = ns_layout_build_(doc, styles, viewport_width);
    if (!g_input_columns_for_layout) {
        GHashTable *cols = g_hash_table_new(g_direct_hash, g_direct_equal);
        if (collect_text_input_columns(root, cols)) {
            g_input_columns_for_layout = cols;
            ns_box_free(root);
            ns_image_cache_begin_generation(image_cache);
            root = ns_layout_build_(doc, styles, viewport_width);
            g_input_columns_for_layout = NULL;
        }
        g_hash_table_destroy(cols);
    }
    ns_image_cache_collect(image_cache);
    g_focused_input_for_layout = NULL;
    g_focused_is_contenteditable_for_layout = FALSE;
    g_focused_caret_byte_for_layout = 0;
    g_focused_sel_anchor_byte_for_layout = 0;
    g_image_cache_for_layout = NULL;
    g_base_url_for_layout = NULL;
    if (g_counters_for_layout) {
        g_hash_table_destroy(g_counters_for_layout);
        g_counters_for_layout = NULL;
    }
    record_frame_viewports(root);
    ns_paint_i18n_memo_end();
    return root;
}

static gboolean
style_is_relative(const ns_style *s)
{
    if (!s || !s->values[NS_CSS_POSITION]) return FALSE;
    const ns_css_value *v = s->values[NS_CSS_POSITION];
    return v->kind == NS_CSS_V_KEYWORD &&
           strcmp(v->u.keyword, "relative") == 0;
}

static void
translate_subtree(ns_box *box, double dx, double dy)
{
    if (!box || (dx == 0 && dy == 0)) return;
    shift_box_tree(box, dx, dy);
}

static double
relative_pct_cb_height(const ns_box *box)
{
    const ns_box *p = box ? box->parent : NULL;
    while (p && !p->style) p = p->parent;
    if (!p) return -1;
    const ns_css_value *h = p->style->values[NS_CSS_HEIGHT];
    if (h && h->kind == NS_CSS_V_KEYWORD)
        return height_keyword_stretches(h) ? containing_block_definite_height(box) : -1;
    if (!h) return -1;
    if (value_is_percent(h)) {
        double base;
        if (p->dom && p->dom->name && strcmp(p->dom->name, "html") == 0)
            base = ns_css_viewport_h();
        else
            base = relative_pct_cb_height(p);
        if (base < 0) return -1;
        if (h->kind == NS_CSS_V_CALC)
            return h->u.calc.pct / 100.0 * base + h->u.calc.px;
        return h->u.length.v * base / 100.0;
    }
    if (p->content_height > 0) return p->content_height;
    return -1;
}

static double
relative_offset_y(const ns_style *s, double parent_h, const ns_box *pct_of)
{
    const ns_css_value *tv = s->values[NS_CSS_TOP];
    const ns_css_value *bv = s->values[NS_CSS_BOTTOM];
    gboolean from_top = tv && !length_is_auto(tv);
    const ns_css_value *v = from_top ? tv : bv;
    double sign = from_top ? 1 : -1;
    if (!v || length_is_auto(v)) return 0;
    if (!value_is_percent(v)) return sign * length_resolve(v, parent_h, 0);
    double cb_h = relative_pct_cb_height(pct_of);
    if (cb_h < 0) cb_h = containing_block_definite_height(pct_of);
    return cb_h < 0 ? 0 : sign * length_resolve(v, cb_h, 0);
}

static double
relative_offset_x(const ns_style *s, double parent_w)
{
    const ns_css_value *lv = s->values[NS_CSS_LEFT];
    const ns_css_value *rv = s->values[NS_CSS_RIGHT];
    if (lv && !length_is_auto(lv))
        return length_resolve(lv, parent_w, 0);
    if (rv && !length_is_auto(rv))
        return -length_resolve(rv, parent_w, 0);
    return 0;
}

static void
apply_relative_offset(ns_box *box, double parent_w, double parent_h)
{
    translate_subtree(box, relative_offset_x(box->style, parent_w),
                      relative_offset_y(box->style, parent_h, box));
}

static gboolean
inline_attr_holds(const ns_inline_attr *r, gsize byte)
{
    return byte >= r->start && byte < r->start + r->len;
}

void
ns_inline_offset_at(const ns_box *box, gsize byte, double *dx, double *dy)
{
    *dx = 0;
    *dy = 0;
    if (!box || !box->attrs) return;
    gsize innermost = G_MAXSIZE;
    for (guint i = 0; i < box->attrs->len; i++) {
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, i);
        if (r->kind != NS_INLINE_ELEMENT || r->len >= innermost ||
            !inline_attr_holds(r, byte))
            continue;
        innermost = r->len;
        *dx = r->rel_dx;
        *dy = r->rel_dy;
    }
}

gboolean
ns_inline_shift_bounds(const ns_box *box, double *x0, double *y0,
                       double *x1, double *y1)
{
    *x0 = *y0 = *x1 = *y1 = 0;
    if (!box || !box->attrs) return FALSE;
    gboolean any = FALSE;
    for (guint i = 0; i < box->attrs->len; i++) {
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, i);
        if (r->kind != NS_INLINE_ELEMENT || (r->rel_dx == 0 && r->rel_dy == 0))
            continue;
        any = TRUE;
        *x0 = MIN(*x0, r->rel_dx);
        *x1 = MAX(*x1, r->rel_dx);
        *y0 = MIN(*y0, r->rel_dy);
        *y1 = MAX(*y1, r->rel_dy);
    }
    return any;
}

static gboolean
inline_shifted_text_at(const ns_box *box, NsPangoLayout *layout,
                       const ns_inline_attr *r, double x, double y)
{
    int index = 0, trailing = 0;
    if (!ns_pango_layout_xy_to_index(layout,
                                     (int)((x - r->rel_dx) * NS_PANGO_SCALE),
                                     (int)((y - r->rel_dy) * NS_PANGO_SCALE),
                                     &index, &trailing) ||
        index < 0 || !inline_attr_holds(r, (gsize)index))
        return FALSE;
    double dx, dy;
    ns_inline_offset_at(box, (gsize)index, &dx, &dy);
    return dx == r->rel_dx && dy == r->rel_dy;
}

gboolean
ns_inline_unshift_point(const ns_box *box, NsPangoLayout *layout,
                        double *x, double *y)
{
    if (!box || !box->attrs || !layout) return FALSE;
    for (guint i = 0; i < box->attrs->len; i++) {
        const ns_inline_attr *r =
            &g_array_index(box->attrs, ns_inline_attr, i);
        if (r->kind != NS_INLINE_ELEMENT || (r->rel_dx == 0 && r->rel_dy == 0))
            continue;
        if (!inline_shifted_text_at(box, layout, r, *x, *y)) continue;
        *x -= r->rel_dx;
        *y -= r->rel_dy;
        return TRUE;
    }
    return FALSE;
}

typedef struct inline_rel_offset {
    const ns_node *dom;
    double dx, dy;
    double acc_dx, acc_dy;
    gboolean layered;
    int depth;
} inline_rel_offset;

typedef struct inline_rel_set {
    GArray *elements;
    GHashTable *index;
} inline_rel_set;

static gboolean
inline_element_is_relative(const ns_inline_attr *r)
{
    return r->kind == NS_INLINE_ELEMENT && r->dom &&
           style_is_relative(r->style) &&
           !ns_display_is_contents(ns_css_display_of(r->style));
}

static int
inline_node_depth(const ns_node *n)
{
    int depth = 0;
    for (; n; n = n->parent) depth++;
    return depth;
}

static void
inline_rel_set_add(inline_rel_set *set, const ns_inline_attr *r,
                   double cb_w, double cb_h, const ns_box *box)
{
    if (!set->elements) {
        set->elements = g_array_new(FALSE, FALSE, sizeof(inline_rel_offset));
        set->index = g_hash_table_new(NULL, NULL);
    }
    inline_rel_offset o = { r->dom, relative_offset_x(r->style, cb_w),
                            relative_offset_y(r->style, cb_h, box),
                            0, 0, FALSE, inline_node_depth(r->dom) };
    g_array_append_val(set->elements, o);
    g_hash_table_insert(set->index, (gpointer)r->dom,
                        GUINT_TO_POINTER(set->elements->len));
}

static void
inline_relative_elements(ns_box *box, double cb_w, double cb_h,
                         inline_rel_set *set)
{
    for (guint i = 0; i < box->attrs->len; i++) {
        ns_inline_attr *r = &g_array_index(box->attrs, ns_inline_attr, i);
        r->rel_dx = 0;
        r->rel_dy = 0;
        r->rel_layer = NULL;
        if (!inline_element_is_relative(r) ||
            (set->index && g_hash_table_contains(set->index, r->dom)))
            continue;
        inline_rel_set_add(set, r, cb_w, cb_h, box);
    }
}

static inline_rel_offset *
inline_rel_nearest(const inline_rel_set *set, const ns_node *n)
{
    for (; n; n = n->parent) {
        guint k = GPOINTER_TO_UINT(g_hash_table_lookup(set->index, n));
        if (k) return &g_array_index(set->elements, inline_rel_offset, k - 1);
    }
    return NULL;
}

static int
inline_rel_depth_cmp(gconstpointer a, gconstpointer b, gpointer data)
{
    const GArray *elements = data;
    int da = g_array_index(elements, inline_rel_offset, *(const guint *)a).depth;
    int db = g_array_index(elements, inline_rel_offset, *(const guint *)b).depth;
    return da < db ? -1 : (da > db ? 1 : 0);
}

static void
inline_rel_resolve(const inline_rel_set *set)
{
    GArray *order = g_array_sized_new(FALSE, FALSE, sizeof(guint),
                                      set->elements->len);
    for (guint i = 0; i < set->elements->len; i++)
        g_array_append_val(order, i);
    g_array_sort_with_data(order, inline_rel_depth_cmp, set->elements);
    for (guint k = 0; k < order->len; k++) {
        inline_rel_offset *o = &g_array_index(set->elements, inline_rel_offset,
                                              g_array_index(order, guint, k));
        const inline_rel_offset *up = inline_rel_nearest(set, o->dom->parent);
        o->acc_dx = o->dx + (up ? up->acc_dx : 0);
        o->acc_dy = o->dy + (up ? up->acc_dy : 0);
        o->layered = o->dx != 0 || o->dy != 0 || (up && up->layered);
    }
    g_array_free(order, TRUE);
}

static void
inline_rel_assign(ns_box *box, const inline_rel_set *set)
{
    for (guint i = 0; i < box->attrs->len; i++) {
        ns_inline_attr *r = &g_array_index(box->attrs, ns_inline_attr, i);
        if (r->kind != NS_INLINE_ELEMENT) continue;
        const inline_rel_offset *o = inline_rel_nearest(set, r->dom);
        if (!o) continue;
        r->rel_dx = o->acc_dx;
        r->rel_dy = o->acc_dy;
        r->rel_layer = o->layered ? o->dom : NULL;
    }
}

static void
apply_inline_relative_offsets(ns_box *box, double cb_w, double cb_h)
{
    if (box->kind != NS_BOX_INLINE || !box->attrs) return;
    inline_rel_set set = { NULL, NULL };
    inline_relative_elements(box, cb_w, cb_h, &set);
    if (!set.elements) return;
    if (!box->vertical_wm) {
        inline_rel_resolve(&set);
        inline_rel_assign(box, &set);
    }
    g_array_free(set.elements, TRUE);
    g_hash_table_destroy(set.index);
}

static void
apply_atomic_position_offsets(ns_box *box, double cb_w, double cb_h)
{
    double sx0, sy0, sx1, sy1;
    gboolean moved = ns_inline_shift_bounds(box, &sx0, &sy0, &sx1, &sy1);
    for (guint i = 0; i < box->inline_atomics->len; i++) {
        const ns_inline_atomic *atomic =
            &g_array_index(box->inline_atomics, ns_inline_atomic, i);
        ns_box *ab = atomic->box;
        if (!ab) continue;
        gboolean rel = style_is_relative(ab->style);
        ab->rel_dx = 0;
        ab->rel_dy = 0;
        if (moved)
            ns_inline_offset_at(box, atomic->byte_off, &ab->rel_dx,
                                &ab->rel_dy);
        ab->rel_dx += rel ? relative_offset_x(ab->style, cb_w) : 0;
        ab->rel_dy += rel ? relative_offset_y(ab->style, cb_h, box) : 0;
        translate_subtree(ab, ab->rel_dx, ab->rel_dy);
        for (ns_box *c = ab->first_child; c; c = c->next_sibling)
            apply_position_offsets(c, ab->content_width, ab->content_height);
        if (ab->inline_atomics)
            apply_atomic_position_offsets(ab, ab->content_width,
                                          ab->content_height);
    }
}

static void
apply_position_offsets(ns_box *box, double parent_w, double parent_h)
{
    if (!box) return;
    double child_w = box->content_width;
    double child_h = box->content_height;
    if (style_is_relative(box->style))
        apply_relative_offset(box, parent_w, parent_h);
    apply_inline_relative_offsets(box, parent_w, parent_h);
    if (box->inline_atomics)
        apply_atomic_position_offsets(box, parent_w, parent_h);
    for (ns_box *c = box->first_child; c; c = c->next_sibling)
        apply_position_offsets(c, child_w, child_h);
}

static void
abs_box_map_build(GHashTable *map, ns_box *root)
{
    if (!root) return;
    if (root->dom && !g_hash_table_contains(map, root->dom))
        g_hash_table_insert(map, (gpointer)root->dom, root);
    for (ns_box *c = root->first_child; c; c = c->next_sibling)
        abs_box_map_build(map, c);
    if (root->inline_atomics) {
        for (guint i = 0; i < root->inline_atomics->len; i++) {
            ns_box *ab = g_array_index(root->inline_atomics,
                                       ns_inline_atomic, i).box;
            abs_box_map_build(map, ab);
        }
    }
}

static gboolean
node_is_ancestor_of(const ns_node *a, const ns_node *n)
{
    if (!a || !n) return FALSE;
    for (const ns_node *p = n->parent; p; p = p->parent)
        if (p == a) return TRUE;
    return FALSE;
}

static GHashTable *g_node_order;
static GArray *g_node_order_end;
static guint g_node_order_epoch;

static void
node_order_build(const ns_node *root)
{
    g_node_order = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_node_order_end = g_array_new(FALSE, TRUE, sizeof(guint));
    g_array_set_size(g_node_order_end, 1);
    if (++g_node_order_epoch == 0) g_node_order_epoch = 1;
    GArray *open = g_array_new(FALSE, FALSE, sizeof(guint));
    guint rank = 0;
    const ns_node *n = root;
    for (;;) {
        g_hash_table_insert(g_node_order, (gpointer)n,
                            GUINT_TO_POINTER(++rank));
        g_array_set_size(g_node_order_end, rank + 1);
        g_array_append_val(open, rank);
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        for (;;) {
            guint closed = g_array_index(open, guint, open->len - 1);
            g_array_set_size(open, open->len - 1);
            g_array_index(g_node_order_end, guint, closed) = rank;
            if (n == root || !n->parent) goto done;
            if (n->next_sibling) {
                n = n->next_sibling;
                break;
            }
            n = n->parent;
        }
    }
done:
    g_array_free(open, TRUE);
}

static void
node_order_clear(void)
{
    g_clear_pointer(&g_node_order, g_hash_table_destroy);
    if (g_node_order_end) {
        g_array_free(g_node_order_end, TRUE);
        g_node_order_end = NULL;
    }
}

static void
box_order_load(ns_box *b)
{
    if (b->order_epoch == g_node_order_epoch) return;
    b->order_epoch = g_node_order_epoch;
    b->order_rank = g_node_order && b->dom ? GPOINTER_TO_UINT(
        g_hash_table_lookup(g_node_order, (gpointer)b->dom)) : 0;
    b->order_end = b->order_rank
        ? g_array_index(g_node_order_end, guint, b->order_rank) : 0;
}

static gboolean
box_is_absolute_or_fixed(ns_box *b)
{
    if (!b->order_abs_style_set || b->order_abs_style != b->style) {
        b->order_abs_style = b->style;
        b->order_abs_style_set = TRUE;
        b->order_abs = style_is_absolute_or_fixed(b->style);
    }
    return b->order_abs;
}

static gboolean
node_precedes(const ns_node *a, const ns_node *b)
{
    if (!a || !b || a == b) return FALSE;
    if (g_node_order) {
        guint ra = GPOINTER_TO_UINT(g_hash_table_lookup(g_node_order,
                                                        (gpointer)a));
        guint rb = GPOINTER_TO_UINT(g_hash_table_lookup(g_node_order,
                                                        (gpointer)b));
        if (ra && rb) return ra < rb;
    }
    int da = 0, db = 0;
    for (const ns_node *p = a; p; p = p->parent) da++;
    for (const ns_node *p = b; p; p = p->parent) db++;
    const ns_node *pa = a, *pb = b;
    while (da > db + 1) { pa = pa->parent; da--; }
    while (db > da + 1) { pb = pb->parent; db--; }
    if (da > db) { if (pa->parent == pb) return FALSE; pa = pa->parent; da--; }
    else if (db > da) { if (pb->parent == pa) return TRUE; pb = pb->parent; db--; }
    while (pa->parent != pb->parent) {
        pa = pa->parent;
        pb = pb->parent;
        if (!pa || !pb) return FALSE;
    }
    for (const ns_node *s = pa->next_sibling; s; s = s->next_sibling)
        if (s == pb) return TRUE;
    return FALSE;
}

static double
box_outer_bottom(const ns_box *b)
{
    if (!b) return 0;
    return b->y + b->margin.top + b->border.top + b->padding.top +
           b->content_height + b->padding.bottom + b->border.bottom +
           b->margin.bottom;
}

typedef struct static_abs_target {
    const ns_node *node;
    guint          rank;
    GHashTable    *ancestors;
} static_abs_target;

static gboolean
static_abs_target_inside(const ns_box *b, const static_abs_target *t)
{
    if (!t->rank) return g_hash_table_contains(t->ancestors, b->dom);
    return b->order_rank && b->order_rank < t->rank &&
           t->rank <= b->order_end;
}

static gboolean
static_abs_y_visit_other(ns_box *b, const static_abs_target *t, double *out)
{
    guint rb = b->order_rank;
    gboolean ranked = rb && t->rank;
    if (ranked ? t->rank < rb : node_precedes(t->node, b->dom)) return FALSE;
    if (box_is_absolute_or_fixed(b)) return FALSE;
    if (ranked ? rb < t->rank : node_precedes(b->dom, t->node)) {
        double bottom = box_outer_bottom(b);
        if (bottom > *out) *out = bottom;
    }
    return TRUE;
}

static gboolean
static_abs_y_visit(ns_box *b, const static_abs_target *t, double *out)
{
    if (!b->dom || b->dom == t->node) return TRUE;
    box_order_load(b);
    if (!static_abs_target_inside(b, t))
        return static_abs_y_visit_other(b, t, out);
    if (!box_is_absolute_or_fixed(b)) {
        double edge = b->y + b->margin.top + b->border.top + b->padding.top;
        if (edge > *out) *out = edge;
    }
    return TRUE;
}

static void
static_abs_y_walk_from(ns_box *b, const static_abs_target *t, double *out)
{
    if (!static_abs_y_visit(b, t, out)) return;
    for (ns_box *c = b->first_child; c; c = c->next_sibling)
        static_abs_y_walk_from(c, t, out);
}

static void
static_abs_y_walk(ns_box *b, const ns_node *target, double *out)
{
    if (!b || !target || !out) return;
    static_abs_target t = {
        .node = target,
        .rank = g_node_order ? GPOINTER_TO_UINT(
            g_hash_table_lookup(g_node_order, (gpointer)target)) : 0,
        .ancestors = NULL,
    };
    if (!t.rank) {
        t.ancestors = g_hash_table_new(g_direct_hash, g_direct_equal);
        for (const ns_node *p = target->parent; p; p = p->parent)
            g_hash_table_add(t.ancestors, (gpointer)p);
    }
    static_abs_y_walk_from(b, &t, out);
    if (t.ancestors) g_hash_table_destroy(t.ancestors);
}

static double
static_abs_x_from_ancestors(const ns_box *cb, const ns_node *target,
                            GHashTable *box_map, double fallback,
                            gboolean *out_rtl, double *out_right)
{
    double best = fallback;
    guint best_depth = 0;
    *out_rtl = cb && cb->style &&
        ns_css_keyword_is(cb->style->values[NS_CSS_DIRECTION], "rtl");
    *out_right = cb ? cb->x + cb->margin.left + cb->border.left +
                      cb->padding.left + cb->content_width : fallback;
    for (const ns_node *p = target ? target->parent : NULL; p; p = p->parent) {
        if (p->kind != NS_NODE_ELEMENT) continue;
        const ns_box *pb = g_hash_table_lookup(box_map, p);
        if (!pb || style_is_absolute_or_fixed(pb->style)) {
            if (pb && pb == cb) break;
            continue;
        }
        gboolean inside_cb = FALSE;
        for (const ns_box *a = pb; a; a = a->parent)
            if (a == cb) { inside_cb = TRUE; break; }
        if (!inside_cb) break;
        guint depth = 0;
        for (const ns_node *q = target; q && q != p; q = q->parent) depth++;
        if (best_depth == 0 || depth < best_depth) {
            best = pb->x + pb->margin.left + pb->border.left + pb->padding.left;
            *out_right = best + pb->content_width;
            *out_rtl = pb->style &&
                ns_css_keyword_is(pb->style->values[NS_CSS_DIRECTION], "rtl");
            best_depth = depth;
        }
        if (pb == cb) break;
    }
    return best;
}

typedef struct ns_abs_static_calc {
    guint entry_index;
    guint rank;
    double y;
    gboolean resolved;
} ns_abs_static_calc;

static int
abs_static_calc_rank_cmp(gconstpointer a, gconstpointer b)
{
    const ns_abs_static_calc *ca = a, *cb = b;
    if (ca->rank < cb->rank) return -1;
    if (ca->rank > cb->rank) return 1;
    return 0;
}

static void
static_abs_y_batch_walk(const ns_box *b, GArray *calcs, guint *next,
                        double *cur_max)
{
    if (!b || *next >= calcs->len) return;
    if (b->dom) {
        guint rb = GPOINTER_TO_UINT(
            g_hash_table_lookup(g_node_order, (gpointer)b->dom));
        if (rb) {
            while (*next < calcs->len &&
                   g_array_index(calcs, ns_abs_static_calc, *next).rank <= rb) {
                ns_abs_static_calc *c =
                    &g_array_index(calcs, ns_abs_static_calc, *next);
                c->y = *cur_max;
                c->resolved = TRUE;
                (*next)++;
            }
        }
    }
    if (b->dom && !style_is_absolute_or_fixed(b->style)) {
        double content_top = b->y + b->margin.top + b->border.top +
                             b->padding.top;
        if (content_top > *cur_max) *cur_max = content_top;
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        static_abs_y_batch_walk(c, calcs, next, cur_max);
    while (b->dom && *next < calcs->len) {
        ns_abs_static_calc *c = &g_array_index(calcs, ns_abs_static_calc, *next);
        const ns_abs_entry *e =
            &g_array_index(g_abs_pending, ns_abs_entry, c->entry_index);
        if (!node_is_ancestor_of(b->dom, e->dom)) break;
        c->y = *cur_max;
        c->resolved = TRUE;
        (*next)++;
    }
    if (b->dom && !style_is_absolute_or_fixed(b->style)) {
        double bottom = box_outer_bottom(b);
        if (bottom > *cur_max) *cur_max = bottom;
    }
}

static const ns_node *find_abs_containing_block_dom(const ns_node *n,
                                                    GHashTable *styles);
static gboolean style_creates_abs_cb(const ns_style *s, const ns_node *n);
static gboolean style_creates_fixed_cb(const ns_style *s, const ns_node *n);

static const ns_node *
abs_entry_cb_dom(const ns_abs_entry *e, GHashTable *styles)
{
    if (e->pseudo) {
        const ns_style *hs = g_hash_table_lookup(styles, e->dom);
        if (style_creates_abs_cb(hs, e->dom)) return e->dom;
    }
    return find_abs_containing_block_dom(e->dom, styles);
}

static const ns_node *
fixed_entry_cb_dom(const ns_abs_entry *e, GHashTable *styles)
{
    if (e->pseudo &&
        style_creates_fixed_cb(g_hash_table_lookup(styles, e->dom), e->dom))
        return e->dom;
    for (const ns_node *p = layout_flat_parent(e->dom); p;
         p = layout_flat_parent(p)) {
        const ns_node *frame = frame_document_host(p);
        if (frame) return frame;
        if (p->kind != NS_NODE_ELEMENT) continue;
        if (style_creates_fixed_cb(g_hash_table_lookup(styles, p), p)) return p;
    }
    return NULL;
}

static gboolean
inline_containing_block(const ns_box *root, const ns_node *cb_dom,
                        GHashTable *styles, ns_box *out)
{
    double x, y, w, h;
    if (!cb_dom || !ns_box_inline_rect_for_dom(root, cb_dom, &x, &y, &w, &h))
        return FALSE;
    memset(out, 0, sizeof *out);
    out->kind = NS_BOX_BLOCK;
    out->style = g_hash_table_lookup(styles, cb_dom);
    ns_edges m = {0}, pd = {0}, bd = {0};
    edges_from_style(out->style, 0, &m, &pd, &bd);
    out->x = x + bd.left;
    out->y = y + bd.top;
    out->content_width = MAX(0, w - bd.left - bd.right);
    out->content_height = MAX(0, h - bd.top - bd.bottom);
    return TRUE;
}

static ns_box *
nearest_ancestor_box(const ns_node *n, GHashTable *box_map, ns_box *root)
{
    for (const ns_node *p = n; p; p = layout_flat_parent(p)) {
        ns_box *b = g_hash_table_lookup(box_map, p);
        if (b) return b;
    }
    return root;
}

static gconstpointer
abs_entry_static_key(const ns_abs_entry *e)
{
    return e->pseudo ? (gconstpointer)e->pseudo : (gconstpointer)e->dom;
}

static const ns_node *
positioned_entry_cb_dom(const ns_abs_entry *e, GHashTable *styles)
{
    return e->fixed ? fixed_entry_cb_dom(e, styles) : abs_entry_cb_dom(e, styles);
}

static double
flex_static_main_offset(const char *justify, double free_space,
                        gboolean row, gboolean main_reverse, gboolean rtl)
{
    gboolean flipped = main_reverse != (rtl && row);
    gboolean phys_start_is_end = row && rtl;
    if (strcmp(justify, "start") == 0)
        return phys_start_is_end ? free_space : 0;
    if (strcmp(justify, "end") == 0)
        return phys_start_is_end ? 0 : free_space;
    if (strcmp(justify, "left") == 0)
        return row ? 0 : (phys_start_is_end ? free_space : 0);
    if (strcmp(justify, "right") == 0)
        return row ? free_space : (phys_start_is_end ? free_space : 0);
    double main = 0;
    if (strcmp(justify, "flex-end") == 0)
        main = free_space;
    else if (strcmp(justify, "center") == 0 ||
             strcmp(justify, "space-around") == 0 ||
             strcmp(justify, "space-evenly") == 0)
        main = free_space / 2;
    return flipped ? free_space - main : main;
}

static double
flex_static_cross_offset(const char *align, double free_space,
                         gboolean row, gboolean wrap_reverse, gboolean rtl,
                         gboolean self_rtl)
{
    gboolean flipped = wrap_reverse != (rtl && !row);
    gboolean phys_start_is_end = !row && rtl;
    gboolean self_start_is_end = !row && self_rtl;
    if (strcmp(align, "start") == 0 || flex_align_is_baseline(align))
        return phys_start_is_end ? free_space : 0;
    if (strcmp(align, "end") == 0 || strcmp(align, "last baseline") == 0)
        return phys_start_is_end ? 0 : free_space;
    if (strcmp(align, "self-start") == 0)
        return self_start_is_end ? free_space : 0;
    if (strcmp(align, "self-end") == 0)
        return self_start_is_end ? 0 : free_space;
    if (strcmp(align, "left") == 0)
        return row ? (phys_start_is_end ? free_space : 0) : 0;
    if (strcmp(align, "right") == 0)
        return row ? (phys_start_is_end ? free_space : 0) : free_space;
    double cross = 0;
    if (strcmp(align, "flex-end") == 0)
        cross = free_space;
    else if (strcmp(align, "center") == 0 || strcmp(align, "self-center") == 0)
        cross = free_space / 2;
    return flipped ? free_space - cross : cross;
}

static const ns_box *
abs_flex_parent_box(const ns_node *dom, GHashTable *box_map)
{
    const ns_node *p = dom ? dom->parent : NULL;
    while (p && p->kind != NS_NODE_ELEMENT) p = p->parent;
    return p ? g_hash_table_lookup(box_map, p) : NULL;
}

static gboolean
flex_static_position(ns_box *abox, const ns_box *fc, double *out_x,
                     double *out_y)
{
    if (!fc || !fc->style || !box_is_flex_container(fc)) return FALSE;

    double origin_x = fc->x + fc->margin.left + fc->border.left +
                      fc->padding.left;
    double origin_y = fc->y + fc->margin.top + fc->border.top +
                      fc->padding.top;
    double outer_w = abox->content_width + abox->padding.left +
                     abox->padding.right + abox->border.left +
                     abox->border.right + abox->margin.left +
                     abox->margin.right;
    double outer_h = abox->content_height + abox->padding.top +
                     abox->padding.bottom + abox->border.top +
                     abox->border.bottom + abox->margin.top +
                     abox->margin.bottom;

    const char *dir = flex_direction_of(fc->style);
    gboolean row = strncmp(dir, "row", 3) == 0;
    gboolean main_reverse = strstr(dir, "-reverse") != NULL;
    gboolean wrap_reverse = ns_css_keyword_is(
        fc->style->values[NS_CSS_FLEX_WRAP], "wrap-reverse");
    gboolean rtl = ns_css_keyword_is(fc->style->values[NS_CSS_DIRECTION],
                                     "rtl");

    double main_free = (row ? fc->content_width : fc->content_height) -
                       (row ? outer_w : outer_h);
    double cross_free = (row ? fc->content_height : fc->content_width) -
                        (row ? outer_h : outer_w);

    double main = flex_static_main_offset(
        keyword_or(fc->style, NS_CSS_JUSTIFY_CONTENT, "flex-start"), main_free,
        row, main_reverse, rtl);
    gboolean self_rtl = abox->style &&
        ns_css_keyword_is(abox->style->values[NS_CSS_DIRECTION], "rtl");
    double cross = flex_static_cross_offset(
        flex_item_align(abox, keyword_or(fc->style, NS_CSS_ALIGN_ITEMS,
                                         "stretch")), cross_free,
        row, wrap_reverse, rtl, self_rtl);

    *out_x = origin_x + (row ? main : cross);
    *out_y = origin_y + (row ? cross : main);
    return TRUE;
}

static void
abs_calc_array_free(gpointer a)
{
    g_array_free(a, TRUE);
}

static void
static_abs_y_precompute(ns_box *root, GHashTable *box_map, GHashTable *styles,
                        double *out_y, gboolean *out_resolved)
{
    GHashTable *by_cb = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, abs_calc_array_free);
    for (guint i = 0; i < g_abs_pending->len; i++) {
        ns_abs_entry e = g_array_index(g_abs_pending, ns_abs_entry, i);
        ns_abs_static *st = g_abs_static
            ? g_hash_table_lookup(g_abs_static, abs_entry_static_key(&e)) : NULL;
        if (st && st->run) continue;
        guint rank = GPOINTER_TO_UINT(
            g_hash_table_lookup(g_node_order, (gpointer)e.dom));
        if (!rank) continue;
        const ns_node *cb_dom = positioned_entry_cb_dom(&e, styles);
        ns_box *cb = cb_dom ? g_hash_table_lookup(box_map, cb_dom) : root;
        if (cb_dom && !cb) continue;
        if (!cb) cb = root;
        GArray *calcs = g_hash_table_lookup(by_cb, cb);
        if (!calcs) {
            calcs = g_array_new(FALSE, FALSE, sizeof(ns_abs_static_calc));
            g_hash_table_insert(by_cb, cb, calcs);
        }
        ns_abs_static_calc c = { i, rank, 0, FALSE };
        g_array_append_val(calcs, c);
    }

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, by_cb);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        ns_box *cb = key;
        GArray *calcs = value;
        g_array_sort(calcs, abs_static_calc_rank_cmp);
        double cur_max = cb->y + cb->margin.top + cb->border.top +
                         cb->padding.top;
        guint next = 0;
        static_abs_y_batch_walk(cb, calcs, &next, &cur_max);
        for (guint k = 0; k < calcs->len; k++) {
            const ns_abs_static_calc *c =
                &g_array_index(calcs, ns_abs_static_calc, k);
            if (!c->resolved) continue;
            out_y[c->entry_index] = c->y;
            out_resolved[c->entry_index] = TRUE;
        }
    }
    g_hash_table_destroy(by_cb);
}

static gboolean
style_creates_abs_cb(const ns_style *s, const ns_node *n)
{
    static const char *const will_change[] = { "position" };
    if (!s) return FALSE;
    const ns_css_value *v = s->values[NS_CSS_POSITION];
    if (v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword) {
        const char *kw = v->u.keyword;
        if (strcmp(kw, "relative") == 0 || strcmp(kw, "absolute") == 0 ||
            strcmp(kw, "fixed") == 0    || strcmp(kw, "sticky") == 0)
            return TRUE;
    }
    return keyword_list_has(s->values[NS_CSS_WILL_CHANGE], will_change,
                            G_N_ELEMENTS(will_change)) ||
           style_creates_fixed_cb(s, n);
}

static gboolean
style_is_non_atomic_inline(const ns_style *s)
{
    ns_display d = ns_css_display_of(s);
    return d.box == NS_DISPLAY_BOX_NORMAL &&
           d.internal == NS_DISPLAY_INTERNAL_NONE &&
           d.outer == NS_DISPLAY_OUTER_INLINE &&
           (d.inner == NS_DISPLAY_INNER_FLOW || d.inner == NS_DISPLAY_INNER_RUBY);
}

static gboolean
style_transforms_or_contains(const ns_style *s)
{
    static const char *const contain[] = {
        "layout", "paint", "strict", "content",
    };
    static const char *const will_change[] = {
        "transform", "translate", "rotate", "scale", "perspective",
        "transform-style", "offset-path", "contain",
    };
    const ns_css_value *pv = s->values[NS_CSS_PERSPECTIVE];
    return style_has_layer_transform(s) ||
           (pv && pv->kind == NS_CSS_V_LENGTH && pv->u.length.v > 0) ||
           keyword_is(s->values[NS_CSS_TRANSFORM_STYLE], "preserve-3d") ||
           style_differs_from(s, NS_CSS_OFFSET_PATH, "none") ||
           keyword_list_has(s->values[NS_CSS_CONTAIN], contain,
                            G_N_ELEMENTS(contain)) ||
           style_differs_from(s, NS_CSS_CONTENT_VISIBILITY, "visible") ||
           keyword_list_has(s->values[NS_CSS_WILL_CHANGE], will_change,
                            G_N_ELEMENTS(will_change));
}

static gboolean
style_has_filter_cb(const ns_style *s)
{
    static const char *const will_change[] = { "filter", "backdrop-filter" };
    return style_differs_from(s, NS_CSS_FILTER, "none") ||
           style_differs_from(s, NS_CSS_BACKDROP_FILTER, "none") ||
           keyword_list_has(s->values[NS_CSS_WILL_CHANGE], will_change,
                            G_N_ELEMENTS(will_change));
}

static gboolean
node_is_document_element(const ns_node *n)
{
    return n && n->parent && n->parent->kind == NS_NODE_DOCUMENT;
}

static gboolean
style_creates_fixed_cb(const ns_style *s, const ns_node *n)
{
    if (!s) return FALSE;
    if (style_has_filter_cb(s) && !node_is_document_element(n)) return TRUE;
    return !style_is_non_atomic_inline(s) && style_transforms_or_contains(s);
}

static const ns_node *
find_abs_containing_block_dom(const ns_node *n, GHashTable *styles)
{
    for (const ns_node *p = layout_flat_parent(n); p; p = layout_flat_parent(p)) {
        const ns_node *frame = frame_document_host(p);
        if (frame) return frame;
        if (p->kind != NS_NODE_ELEMENT) continue;
        const ns_style *ps = g_hash_table_lookup(styles, p);
        if (style_creates_abs_cb(ps, p)) return p;
    }
    return NULL;
}

static int
grid_abs_line(const char *tok, int explicit_tracks, gboolean end_side)
{
    if (!tok) return 0;
    while (*tok == ' ') tok++;
    if (!*tok || strcmp(tok, "auto") == 0 || g_str_has_prefix(tok, "span "))
        return 0;
    int before_start = -1;
    int past_end = explicit_tracks + 2;
    char *end = NULL;
    long n = strtol(tok, &end, 10);
    if (end != tok) {
        while (*end == ' ') end++;
        if (*end || n == 0) return 0;
        if (n < 0) n = explicit_tracks + 2 + n;
        if (n < 1) return before_start;
        if (n > explicit_tracks + 1) return past_end;
        return (int)n;
    }
    int named = grid_resolve_line_from(tok, explicit_tracks, 0, end_side);
    if (named < 1) return past_end;
    if (named > explicit_tracks + 1) return past_end;
    return named;
}

static void
grid_abs_axis_lines(const ns_style *st, gboolean row_axis, int explicit_tracks,
                    int *out_start, int *out_end)
{
    *out_start = 0;
    *out_end = 0;
    if (!st) return;
    char *start_tok = NULL, *end_tok = NULL;
    const ns_css_value *area = st->values[NS_CSS_GRID_AREA];
    if (area && area->kind == NS_CSS_V_KEYWORD && area->u.keyword) {
        char **parts = g_strsplit(area->u.keyword, "/", -1);
        int n = 0;
        while (parts[n]) n++;
        int si = row_axis ? 0 : 1, ei = row_axis ? 2 : 3;
        if (si < n) start_tok = g_strstrip(g_strdup(parts[si]));
        if (ei < n) end_tok = g_strstrip(g_strdup(parts[ei]));
        g_strfreev(parts);
    }
    const ns_css_value *sh =
        st->values[row_axis ? NS_CSS_GRID_ROW : NS_CSS_GRID_COLUMN];
    if (sh && sh->kind == NS_CSS_V_KEYWORD && sh->u.keyword) {
        g_free(start_tok);
        g_free(end_tok);
        start_tok = NULL;
        end_tok = NULL;
        const char *slash = strchr(sh->u.keyword, '/');
        if (slash) {
            start_tok = g_strstrip(g_strndup(sh->u.keyword, slash - sh->u.keyword));
            end_tok = g_strstrip(g_strdup(slash + 1));
        } else {
            start_tok = g_strstrip(g_strdup(sh->u.keyword));
        }
    }
    const ns_css_value *sv =
        st->values[row_axis ? NS_CSS_GRID_ROW_START : NS_CSS_GRID_COLUMN_START];
    const ns_css_value *ev =
        st->values[row_axis ? NS_CSS_GRID_ROW_END : NS_CSS_GRID_COLUMN_END];
    if (sv && sv->kind == NS_CSS_V_KEYWORD && sv->u.keyword) {
        g_free(start_tok);
        start_tok = g_strdup(sv->u.keyword);
    }
    if (ev && ev->kind == NS_CSS_V_KEYWORD && ev->u.keyword) {
        g_free(end_tok);
        end_tok = g_strdup(ev->u.keyword);
    }
    int s0 = grid_abs_line(start_tok, explicit_tracks, FALSE);
    int e0 = grid_abs_line(end_tok, explicit_tracks, TRUE);
    if (s0 && e0 && s0 > e0) { int t = s0; s0 = e0; e0 = t; }
    if (s0 && e0 && s0 == e0) e0 = 0;
    if (s0 == -1 || s0 >= explicit_tracks + 2) s0 = 0;
    if (e0 == -1 || e0 >= explicit_tracks + 2) e0 = 0;
    *out_start = s0;
    *out_end = e0;
    g_free(start_tok);
    g_free(end_tok);
}

static gboolean
grid_abs_containing_block(const ns_box *cb, const ns_style *st,
                          double *x, double *y, double *w, double *h)
{
    if (!cb || !cb->grid_col_tracks || !cb->grid_row_tracks || !st)
        return FALSE;
    int cs, ce, rs, re;
    const ns_css_value *cols_v = cb->style
        ? cb->style->values[NS_CSS_GRID_TEMPLATE_COLUMNS] : NULL;
    const ns_css_value *rows_v = cb->style
        ? cb->style->values[NS_CSS_GRID_TEMPLATE_ROWS] : NULL;
    grid_lines col_lines = { cols_v && cols_v->kind == NS_CSS_V_TRACKS
                             ? &cols_v->u.tracks : NULL, NULL, FALSE };
    grid_lines row_lines = { rows_v && rows_v->kind == NS_CSS_V_TRACKS
                             ? &rows_v->u.tracks : NULL, NULL, TRUE };
    g_grid_lines = &col_lines;
    grid_abs_axis_lines(st, FALSE, cb->grid_explicit_cols, &cs, &ce);
    g_grid_lines = &row_lines;
    grid_abs_axis_lines(st, TRUE, cb->grid_explicit_rows, &rs, &re);
    g_grid_lines = NULL;
    double pad_x0 = cb->x + cb->margin.left + cb->border.left;
    double pad_y0 = cb->y + cb->margin.top + cb->border.top;
    double pad_x1 = pad_x0 + cb->content_width + cb->padding.left + cb->padding.right;
    double pad_y1 = pad_y0 + cb->content_height + cb->padding.top + cb->padding.bottom;
    const GArray *ct = cb->grid_col_tracks, *rt = cb->grid_row_tracks;
    gboolean rtl = cb->style &&
        ns_css_keyword_is(cb->style->values[NS_CSS_DIRECTION], "rtl");
    double x0 = pad_x0, x1 = pad_x1, y0 = pad_y0, y1 = pad_y1;
    if (ct->len > 0 && (cs || ce)) {
        int n = (int)ct->len;
        double s_edge = rtl ? pad_x1 : pad_x0;
        double e_edge = rtl ? pad_x0 : pad_x1;
        if (cs) {
            const ns_grid_track_edges *t =
                &g_array_index(ct, ns_grid_track_edges, MIN(cs, n) - 1);
            s_edge = cs <= n ? (rtl ? t->end : t->start)
                             : (rtl ? t->start : t->end);
        }
        if (ce) {
            const ns_grid_track_edges *t =
                &g_array_index(ct, ns_grid_track_edges,
                               ce >= 2 ? MIN(ce - 2, n - 1) : 0);
            e_edge = ce >= 2 ? (rtl ? t->start : t->end)
                             : (rtl ? t->end : t->start);
        }
        x0 = MIN(s_edge, e_edge);
        x1 = MAX(s_edge, e_edge);
    }
    if (rs && rt->len > 0) {
        y0 = rs <= (int)rt->len
            ? g_array_index(rt, ns_grid_track_edges, rs - 1).start
            : g_array_index(rt, ns_grid_track_edges, rt->len - 1).end;
    }
    if (re && rt->len > 0) {
        y1 = re >= 2
            ? g_array_index(rt, ns_grid_track_edges, MIN(re - 2, (int)rt->len - 1)).end
            : g_array_index(rt, ns_grid_track_edges, 0).start;
    }
    *x = x0;
    *y = y0;
    *w = MAX(x1 - x0, 0);
    *h = MAX(y1 - y0, 0);
    return TRUE;
}

static void
position_absolute_box_in(ns_box *abox, double cb_inner_x, double cb_inner_y,
                         double cb_w, double cb_h);

static void
grid_append_line_names(GString *s, const ns_css_tracks *tk, int line)
{
    if (!tk) return;
    gboolean open = FALSE;
    for (int i = 0; i < tk->n_line_names; i++) {
        if (tk->line_names[i].line != line) continue;
        g_string_append(s, open ? " " : (s->len ? " [" : "["));
        g_string_append(s, tk->line_names[i].name);
        open = TRUE;
    }
    if (open) g_string_append_c(s, ']');
}

char *
ns_layout_grid_resolved_tracks(const ns_box *box, gboolean columns)
{
    if (!box) return NULL;
    const GArray *tr = columns ? box->grid_col_tracks : box->grid_row_tracks;
    if (!tr) return NULL;
    const ns_css_value *tv = box->style
        ? box->style->values[columns ? NS_CSS_GRID_TEMPLATE_COLUMNS
                                     : NS_CSS_GRID_TEMPLATE_ROWS]
        : NULL;
    const ns_css_tracks *tk = NULL;
    if (tv && tv->kind == NS_CSS_V_TRACKS && tv->u.tracks.subgrid) {
        const ns_box *p = box->parent;
        while (p && !p->style) p = p->parent;
        if (p && ns_display_is_grid_container(ns_css_display_of(p->style)))
            return NULL;
        tv = NULL;
    }
    ns_css_tracks expanded;
    if (tv && tv->kind == NS_CSS_V_TRACKS &&
        tv->u.tracks.auto_repeat == NS_CSS_AUTO_REPEAT_NONE) {
        tk = &tv->u.tracks;
    } else if (tv && tv->kind == NS_CSS_V_TRACKS &&
               tv->u.tracks.auto_repeat_count > 0) {
        int count = tv->u.tracks.auto_repeat_count;
        int others = tv->u.tracks.n - count;
        int explicit_n = columns ? box->grid_explicit_cols
                                 : box->grid_explicit_rows;
        if (explicit_n > others && (explicit_n - others) % count == 0) {
            expanded = tv->u.tracks;
            grid_expand_repeat_names(&tv->u.tracks,
                                     (explicit_n - others) / count, &expanded);
            tk = &expanded;
        }
    }
    if (tr->len == 0) return g_strdup("none");
    if (!tk && !(tv && tv->kind == NS_CSS_V_TRACKS)) {
        gboolean has_items = FALSE;
        for (const ns_box *c = box->first_child; c; c = c->next_sibling)
            if (!style_is_absolute_or_fixed(c->style)) { has_items = TRUE; break; }
        if (!has_items) return g_strdup("none");
    }
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < tr->len; i++) {
        const ns_grid_track_edges *e = &g_array_index(tr, ns_grid_track_edges, i);
        grid_append_line_names(s, tk, (int)i + 1);
        if (s->len) g_string_append_c(s, ' ');
        double size = e->end - e->start;
        if (size < 0) size = 0;
        g_string_append_printf(s, "%gpx", round(size * 100.0) / 100.0);
    }
    grid_append_line_names(s, tk, (int)tr->len + 1);
    return g_string_free(s, FALSE);
}

static double
grid_static_align_offset(const char *align, double free_space, gboolean flip)
{
    if (!align || strcmp(align, "auto") == 0) return 0;
    gboolean at_end = strcmp(align, "end") == 0 || strcmp(align, "flex-end") == 0 ||
                      strcmp(align, "self-end") == 0 || strcmp(align, "last baseline") == 0;
    gboolean at_start = strcmp(align, "start") == 0 || strcmp(align, "flex-start") == 0 ||
                        strcmp(align, "self-start") == 0 || strcmp(align, "baseline") == 0;
    if (strcmp(align, "center") == 0) return free_space / 2;
    if (strcmp(align, "left") == 0) return flip ? free_space : 0;
    if (strcmp(align, "right") == 0) return flip ? 0 : free_space;
    if (at_end) return flip ? 0 : free_space;
    if (at_start) return flip ? free_space : 0;
    return flip ? free_space : 0;
}

static void
grid_static_position(ns_box *abox, const ns_box *cb,
                     double area_x, double area_y, double area_w, double area_h,
                     gboolean static_x, gboolean static_y)
{
    gboolean rtl = cb->style &&
        ns_css_keyword_is(cb->style->values[NS_CSS_DIRECTION], "rtl");
    double outer_w = abox->content_width + abox->padding.left + abox->padding.right +
                     abox->border.left + abox->border.right +
                     abox->margin.left + abox->margin.right;
    double outer_h = abox->content_height + abox->padding.top + abox->padding.bottom +
                     abox->border.top + abox->border.bottom +
                     abox->margin.top + abox->margin.bottom;
    if (static_x) {
        const char *js = abox->style ? ns_style_keyword(abox->style, NS_CSS_JUSTIFY_SELF) : NULL;
        if (!js || strcmp(js, "auto") == 0)
            js = keyword_or(cb->style, NS_CSS_JUSTIFY_ITEMS, "normal");
        shift_box_tree(abox, area_x + grid_static_align_offset(js, area_w - outer_w, rtl)
                             - abox->x, 0);
    }
    if (static_y) {
        const char *as = abox->style ? ns_style_keyword(abox->style, NS_CSS_ALIGN_SELF) : NULL;
        if (!as || strcmp(as, "auto") == 0)
            as = keyword_or(cb->style, NS_CSS_ALIGN_ITEMS, "normal");
        shift_box_tree(abox, 0, area_y + grid_static_align_offset(as, area_h - outer_h, FALSE)
                                - abox->y);
    }
}

static void
position_absolute_box(ns_box *abox, ns_box *cb, gboolean cb_is_icb)
{
    if (!abox || !cb) return;
    double cb_w = cb_is_icb ? ns_css_viewport_w()
                            : cb->content_width + cb->padding.left + cb->padding.right;
    double cb_h = cb_is_icb ? ns_css_viewport_h()
                            : cb->content_height + cb->padding.top + cb->padding.bottom;
    double cb_inner_x = cb->x + cb->margin.left + cb->border.left;
    double cb_inner_y = cb->y + cb->margin.top  + cb->border.top;
    position_absolute_box_in(abox, cb_inner_x, cb_inner_y, cb_w, cb_h);
}

static void
position_absolute_box_in(ns_box *abox, double cb_inner_x, double cb_inner_y,
                         double cb_w, double cb_h)
{
    const ns_style *s = abox->style;

    const ns_css_value *lv = s ? s->values[NS_CSS_LEFT]   : NULL;
    const ns_css_value *rv = s ? s->values[NS_CSS_RIGHT]  : NULL;
    const ns_css_value *tv = s ? s->values[NS_CSS_TOP]    : NULL;
    const ns_css_value *bv = s ? s->values[NS_CSS_BOTTOM] : NULL;

    gboolean l_auto = !lv || length_is_auto(lv);
    gboolean r_auto = !rv || length_is_auto(rv);
    gboolean t_auto = !tv || length_is_auto(tv);
    gboolean b_auto = !bv || length_is_auto(bv);

    double left   = l_auto ? 0 : length_resolve(lv, cb_w, 0);
    double right  = r_auto ? 0 : length_resolve(rv, cb_w, 0);
    double top    = t_auto ? 0 : length_resolve(tv, cb_h, 0);
    double bottom = b_auto ? 0 : length_resolve(bv, cb_h, 0);

    double box_outer_w = abox->content_width
                       + abox->padding.left + abox->padding.right
                       + abox->border.left  + abox->border.right
                       + abox->margin.left  + abox->margin.right;
    double box_outer_h = abox->content_height
                       + abox->padding.top + abox->padding.bottom
                       + abox->border.top  + abox->border.bottom
                       + abox->margin.top  + abox->margin.bottom;

    gboolean ml_auto = length_is_auto(s ? s->values[NS_CSS_MARGIN_LEFT]   : NULL);
    gboolean mr_auto = length_is_auto(s ? s->values[NS_CSS_MARGIN_RIGHT]  : NULL);
    gboolean mt_auto = length_is_auto(s ? s->values[NS_CSS_MARGIN_TOP]    : NULL);
    gboolean mb_auto = length_is_auto(s ? s->values[NS_CSS_MARGIN_BOTTOM] : NULL);

    double final_x, final_y;
    if (!l_auto && !r_auto && (ml_auto || mr_auto)) {
        double remaining = cb_w - left - right - box_outer_w;
        double ml;
        if (ml_auto && mr_auto)
            ml = remaining > 0 ? remaining / 2 : 0;
        else if (ml_auto)
            ml = remaining;
        else
            ml = 0;
        final_x = cb_inner_x + left + ml;
    } else if (!l_auto && !r_auto) {
        double remaining = cb_w - left - right - box_outer_w;
        final_x = cb_inner_x + left +
            grid_static_align_offset(abs_self_alignment(abox, NS_CSS_JUSTIFY_SELF),
                                     remaining, FALSE);
    } else if (!l_auto) {
        final_x = cb_inner_x + left;
    } else if (!r_auto) {
        final_x = cb_inner_x + cb_w - right - box_outer_w;
    } else {
        final_x = abox->x;
    }
    if (!t_auto && !b_auto && (mt_auto || mb_auto)) {
        double remaining = cb_h - top - bottom - box_outer_h;
        double mt;
        if (mt_auto && mb_auto)
            mt = remaining / 2;
        else if (mt_auto)
            mt = remaining;
        else
            mt = 0;
        final_y = cb_inner_y + top + mt;
    } else if (!t_auto && !b_auto) {
        double remaining = cb_h - top - bottom - box_outer_h;
        final_y = cb_inner_y + top +
            grid_static_align_offset(abs_self_alignment(abox, NS_CSS_ALIGN_SELF),
                                     remaining, FALSE);
    } else if (!t_auto) {
        final_y = cb_inner_y + top;
    } else if (!b_auto) {
        final_y = cb_inner_y + cb_h - bottom - box_outer_h;
    } else {
        final_y = abox->y;
    }

    double dx = final_x - abox->x;
    double dy = final_y - abox->y;
    if (dx == 0 && dy == 0) return;
    shift_box_tree(abox, dx, dy);
}

static gboolean
box_has_transform_style(const ns_box *b)
{
    const ns_style *s = b ? b->style : NULL;
    if (!s) return FALSE;
    const ns_css_value *tv = s->values[NS_CSS_TRANSFORM];
    if (tv && tv->kind == NS_CSS_V_TRANSFORM && tv->u.transform.n_ops > 0)
        return TRUE;
    return s->values[NS_CSS_TRANSLATE] || s->values[NS_CSS_ROTATE] ||
           s->values[NS_CSS_SCALE] || s->values[NS_CSS_PERSPECTIVE];
}

static gboolean
box_covers_viewport(const ns_box *b)
{
    double x = b->x + b->margin.left;
    double y = b->y + b->margin.top;
    double w = b->content_width + b->padding.left + b->padding.right +
               b->border.left + b->border.right;
    double h = b->content_height + b->padding.top + b->padding.bottom +
               b->border.top + b->border.bottom;
    return x <= 0.5 && y <= 0.5 &&
           x + w >= ns_css_viewport_w() - 0.5 &&
           y + h >= ns_css_viewport_h() - 0.5;
}

static gboolean
box_can_host_fixed(const ns_box *anc)
{
    for (const ns_box *b = anc; b && b->parent; b = b->parent) {
        if (box_has_transform_style(b)) return FALSE;
        if (box_clips_children(b) && !box_covers_viewport(b)) return FALSE;
    }
    return TRUE;
}

static gboolean
box_local_transform(const ns_box *b, double dx, double dy, ns_mat4 *out)
{
    const ns_style *s = b->style;
    if (!s) return FALSE;
    if (!(s->values[NS_CSS_TRANSFORM] || s->values[NS_CSS_TRANSLATE] ||
          s->values[NS_CSS_ROTATE] || s->values[NS_CSS_SCALE]))
        return FALSE;
    ns_css_transform eff;
    eff.n_ops = 0;
    ns_css_style_effective_transform(s, NULL, &eff);
    if (eff.n_ops == 0) return FALSE;
    double bx = b->x + b->margin.left + dx;
    double by = b->y + b->margin.top + dy;
    double bw = b->content_width + b->padding.left + b->padding.right +
                b->border.left + b->border.right;
    double bh = b->content_height + b->padding.top + b->padding.bottom +
                b->border.top + b->border.bottom;
    double ox = bx + bw / 2.0;
    double oy = by + bh / 2.0;
    double oz = 0;
    const ns_css_value *origin = s->values[NS_CSS_TRANSFORM_ORIGIN];
    if (origin && origin->kind == NS_CSS_V_TRANSFORM &&
        origin->u.transform.n_ops > 0) {
        const ns_css_transform_op *o = &origin->u.transform.ops[0];
        ox = bx + (o->a_is_percent ? o->a / 100.0 * bw : o->a);
        oy = by + (o->b_is_percent ? o->b / 100.0 * bh : o->b);
        oz = o->c;
    }
    ns_mat4 m;
    ns_css_transform_to_mat4(&eff, bw, bh, &m);
    ns_mat4_identity(out);
    ns_mat4_translate(out, ox, oy, oz);
    ns_mat4_multiply(out, &m, out);
    ns_mat4_translate(out, -ox, -oy, -oz);
    return TRUE;
}

static gboolean
box_hit_untransform_point(const ns_box *b, double *x, double *y)
{
    ns_mat4 m;
    if (!box_local_transform(b, 0, 0, &m)) return TRUE;
    if (!ns_mat4_is_affine2d(&m)) return TRUE;
    cairo_matrix_t cm;
    cairo_matrix_init(&cm, m.m[0], m.m[4], m.m[1], m.m[5], m.m[3], m.m[7]);
    if (cairo_matrix_invert(&cm) != CAIRO_STATUS_SUCCESS) return FALSE;
    cairo_matrix_transform_point(&cm, x, y);
    return TRUE;
}

static double
abs_height_limit(const ns_box *abox, const ns_css_value *v, double width_basis,
                 double cb_h, double inset_h, double sizing_extras)
{
    double limit = resolve_height_with_basis(v, width_basis, cb_h, -1);
    if (limit < 0 && size_keyword_is_intrinsic(v) &&
        abox->measured_content_height >= 0)
        limit = abox->measured_content_height + sizing_extras;
    if (limit < 0 && height_keyword_stretches(v) && inset_h >= 0)
        limit = inset_h + sizing_extras;
    return limit;
}

static double
abs_height_within_limits(const ns_box *abox, double h, double width_basis,
                         double cb_h, double inset_h)
{
    const ns_style *s = abox->style;
    if (!s) return h;
    double sizing_extras = flex_box_is_border_box(abox)
        ? abox->padding.top + abox->padding.bottom +
          abox->border.top + abox->border.bottom
        : 0;
    double mx = abs_height_limit(abox, s->values[NS_CSS_MAX_HEIGHT],
                                 width_basis, cb_h, inset_h, sizing_extras);
    if (mx >= 0 && h > mx - sizing_extras)
        h = mx > sizing_extras ? mx - sizing_extras : 0;
    double mn = abs_height_limit(abox, s->values[NS_CSS_MIN_HEIGHT],
                                 width_basis, cb_h, inset_h, sizing_extras);
    if (mn >= 0 && h < mn - sizing_extras) h = mn - sizing_extras;
    return h;
}

static void
abs_place_at_static_run(ns_box *abox, const ns_abs_static *st)
{
    double dx, dy;
    ns_inline_offset_at(st->run, st->byte_off, &dx, &dy);
    abox->x = st->run->x + st->rel_x + dx;
    abox->y = st->run->y + st->rel_y + dy;
}

enum { ABS_AXIS_X = 1, ABS_AXIS_Y = 2 };

typedef struct {
    double right, bottom;
    double pad_right, pad_bottom;
    gboolean flip_x, flip_y;
} abs_overflow;

static gboolean
style_hides_overflow_contribution(const ns_style *s)
{
    const ns_css_value *cv = s ? s->values[NS_CSS_CONTENT_VISIBILITY] : NULL;
    return cv && (is_keyword(cv, "hidden") || is_keyword(cv, "auto"));
}

static int
box_overflow_clip_axes(const ns_box *c)
{
    if (!c->style || c->kind == NS_BOX_INLINE || c->kind == NS_BOX_TEXT)
        return 0;
    const char *ox = overflow_axis_keyword(c->style, NS_CSS_OVERFLOW_X);
    const char *oy = overflow_axis_keyword(c->style, NS_CSS_OVERFLOW_Y);
    if (overflow_kw_scroll_container(ox) || overflow_kw_scroll_container(oy))
        return ABS_AXIS_X | ABS_AXIS_Y;
    return (overflow_kw_clips(ox) ? ABS_AXIS_X : 0) |
           (overflow_kw_clips(oy) ? ABS_AXIS_Y : 0);
}

static gboolean
abs_overflow_unreachable(const abs_overflow *acc, double left, double top)
{
    return (acc->flip_x && left >= acc->pad_right) ||
           (acc->flip_y && top >= acc->pad_bottom);
}

static void
abs_overflow_add(const ns_box *c, abs_overflow *acc, int axes)
{
    double w = c->content_width + c->padding.left + c->padding.right +
               c->border.left + c->border.right;
    double h = c->content_height + c->padding.top + c->padding.bottom +
               c->border.top + c->border.bottom;
    if (w <= 0 || h <= 0) return;
    double left = c->x + c->margin.left;
    double top = c->y + c->margin.top;
    if (abs_overflow_unreachable(acc, left, top)) return;
    if ((axes & ABS_AXIS_X) && left + w > acc->right) acc->right = left + w;
    if ((axes & ABS_AXIS_Y) && top + h > acc->bottom) acc->bottom = top + h;
}

static void
abs_overflow_walk(const ns_box *b, gboolean in_abs, int axes,
                  abs_overflow *acc, int depth);

static void
abs_overflow_visit(const ns_box *c, gboolean in_abs, int axes,
                   abs_overflow *acc, int depth)
{
    if (ns_box_is_fixed(c) || box_has_transform_style(c)) return;
    gboolean c_abs = in_abs || style_is_absolute_or_fixed(c->style);
    if (c_abs) abs_overflow_add(c, acc, axes);
    if (style_hides_overflow_contribution(c->style)) return;
    int inner = axes & ~box_overflow_clip_axes(c);
    if (inner) abs_overflow_walk(c, c_abs, inner, acc, depth + 1);
}

static void
abs_overflow_walk(const ns_box *b, gboolean in_abs, int axes,
                  abs_overflow *acc, int depth)
{
    if (depth > 512) return;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        abs_overflow_visit(c, in_abs, axes, acc, depth);
    if (!b->inline_atomics) return;
    for (guint i = 0; i < b->inline_atomics->len; i++) {
        const ns_box *ab =
            g_array_index(b->inline_atomics, ns_inline_atomic, i).box;
        if (ab) abs_overflow_visit(ab, in_abs, axes, acc, depth);
    }
}

static void
scroll_axis_flips(const ns_box *b, gboolean *fx, gboolean *fy)
{
    const ns_style *s = b->style;
    *fx = self_start_is_far_side(s, TRUE);
    *fy = self_start_is_far_side(s, FALSE);
    if (!style_is_flex_container(s)) return;
    const char *dir = flex_direction_of(s);
    gboolean column = strncmp(dir, "column", 6) == 0;
    gboolean wrap_rev =
        strcmp(keyword_or(s, NS_CSS_FLEX_WRAP, "nowrap"), "wrap-reverse") == 0;
    if (strstr(dir, "-reverse")) {
        if (column) *fy = !*fy; else *fx = !*fx;
    }
    if (wrap_rev) {
        if (column) *fx = !*fx; else *fy = !*fy;
    }
}

static gboolean
scroll_owned_by_viewport(const ns_box *b)
{
    if (!box_is_doc_root(b)) return FALSE;
    if (strcmp(b->dom->name, "html") == 0) return TRUE;
    const ns_box *p = b->parent;
    if (!p || !p->style) return TRUE;
    return !overflow_kw_clips(overflow_axis_keyword(p->style, NS_CSS_OVERFLOW_X)) &&
           !overflow_kw_clips(overflow_axis_keyword(p->style, NS_CSS_OVERFLOW_Y));
}

static gboolean
scroll_range_grow(double *max, double extent)
{
    double whole = floor(extent + 0.5);
    if (whole < 1 || whole <= MAX(*max, 0)) return FALSE;
    *max = whole;
    return TRUE;
}

static gboolean
scroll_candidate_axes(const ns_box *b, gboolean *sx, gboolean *sy)
{
    if (!b->parent || !b->style) return FALSE;
    *sx = overflow_kw_scrolls(overflow_axis_keyword(b->style, NS_CSS_OVERFLOW_X));
    *sy = overflow_kw_scrolls(overflow_axis_keyword(b->style, NS_CSS_OVERFLOW_Y));
    return (*sx || *sy) && box_clips_children(b) &&
           !scroll_owned_by_viewport(b) &&
           !style_hides_overflow_contribution(b->style);
}

static void
extend_scroll_range_from_abs(ns_box *b)
{
    gboolean sx, sy;
    if (!scroll_candidate_axes(b, &sx, &sy)) return;
    abs_overflow acc = {0};
    scroll_axis_flips(b, &acc.flip_x, &acc.flip_y);
    acc.pad_right = b->x + b->margin.left + b->border.left + b->padding.left +
                    b->content_width + b->padding.right;
    acc.pad_bottom = b->y + b->margin.top + b->border.top + b->padding.top +
                     b->content_height + b->padding.bottom;
    acc.right = acc.pad_right;
    acc.bottom = acc.pad_bottom;
    abs_overflow_walk(b, FALSE, ABS_AXIS_X | ABS_AXIS_Y, &acc, 0);
    gboolean grew_x = sx && !acc.flip_x &&
        scroll_range_grow(&b->scroll_max_x, acc.right - acc.pad_right);
    gboolean grew_y = sy && !acc.flip_y &&
        scroll_range_grow(&b->scroll_max_y, acc.bottom - acc.pad_bottom);
    if (grew_x || grew_y) b->scrolls = TRUE;
}

static void
extend_scroll_ranges_from_abs(ns_box *b)
{
    for (ns_box *c = b->first_child; c; c = c->next_sibling)
        extend_scroll_ranges_from_abs(c);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++) {
            ns_box *ab = g_array_index(b->inline_atomics, ns_inline_atomic, i).box;
            if (ab) extend_scroll_ranges_from_abs(ab);
        }
    extend_scroll_range_from_abs(b);
}

static void
box_mark_abs_pseudo(ns_box *abox, GHashTable *styles, const ns_abs_entry *e)
{
    const ns_style *hs = g_hash_table_lookup(styles, e->dom);
    abox->pseudo_host = e->dom;
    abox->pseudo_after = hs && hs->after == e->pseudo;
}

typedef struct {
    GHashTable    *box_map;
    const ns_node *abs_dom;
    const char    *default_name;
} anchor_lookup_ud;

static gboolean
anchor_name_listed(const ns_style *s, const char *want)
{
    const ns_css_value *v = s ? s->values[NS_CSS_ANCHOR_NAME] : NULL;
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return FALSE;
    gsize wl = strlen(want);
    for (const char *p = v->u.keyword; (p = strstr(p, want)); p += wl) {
        gboolean start_ok = p == v->u.keyword || p[-1] == ' ' || p[-1] == ',';
        gboolean end_ok = p[wl] == '\0' || p[wl] == ',' || p[wl] == ' ';
        if (start_ok && end_ok) return TRUE;
    }
    return FALSE;
}

static void
anchor_box_rect(const ns_box *b, double r[4])
{
    r[0] = b->x + b->margin.left;
    r[1] = b->y + b->margin.top;
    r[2] = b->border.left + b->padding.left + b->content_width +
           b->padding.right + b->border.right;
    r[3] = b->border.top + b->padding.top + b->content_height +
           b->padding.bottom + b->border.bottom;
}

static gboolean
anchor_lookup_cb(void *user, const char *name, double rect[4])
{
    anchor_lookup_ud *u = user;
    const char *want = name ? name : u->default_name;
    if (!want || want[0] != '-') return FALSE;
    GHashTableIter it;
    gpointer k, val;
    const ns_box *best = NULL;
    guint best_rank = 0;
    g_hash_table_iter_init(&it, u->box_map);
    while (g_hash_table_iter_next(&it, &k, &val)) {
        const ns_node *dom = k;
        const ns_box *b = val;
        if (dom == u->abs_dom || node_is_ancestor_of(u->abs_dom, dom)) continue;
        if (!anchor_name_listed(b->style, want)) continue;
        guint rank = g_node_order
            ? GPOINTER_TO_UINT(g_hash_table_lookup(g_node_order, dom)) : 0;
        if (!best || rank >= best_rank) {
            best = b;
            best_rank = rank;
        }
    }
    if (!best) return FALSE;
    anchor_box_rect(best, rect);
    return TRUE;
}

static gboolean
value_has_anchor(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           ns_css_text_has_anchor_fn(v->u.keyword);
}

static gboolean
style_uses_anchors(const ns_style *s)
{
    static const ns_css_prop props[] = {
        NS_CSS_TOP, NS_CSS_RIGHT, NS_CSS_BOTTOM, NS_CSS_LEFT, NS_CSS_WIDTH,
        NS_CSS_HEIGHT, NS_CSS_MIN_WIDTH, NS_CSS_MIN_HEIGHT, NS_CSS_MAX_WIDTH,
        NS_CSS_MAX_HEIGHT, NS_CSS_MARGIN_TOP, NS_CSS_MARGIN_RIGHT,
        NS_CSS_MARGIN_BOTTOM, NS_CSS_MARGIN_LEFT,
    };
    if (!s) return FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(props); i++)
        if (value_has_anchor(s->values[props[i]])) return TRUE;
    const ns_css_value *pa = s->values[NS_CSS_POSITION_AREA];
    if (pa && pa->kind == NS_CSS_V_KEYWORD && pa->u.keyword &&
        strcmp(pa->u.keyword, "none") != 0)
        return TRUE;
    return keyword_is(s->values[NS_CSS_JUSTIFY_SELF], "anchor-center") ||
           keyword_is(s->values[NS_CSS_ALIGN_SELF], "anchor-center");
}

static void
style_put(ns_style *s, ns_css_prop prop, ns_css_value *v)
{
    ns_css_value_free(s->values[prop]);
    s->values[prop] = v;
}

static ns_css_value *
px_value(double px)
{
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_LENGTH;
    v->u.length.unit = NS_CSS_UNIT_PX;
    v->u.length.v = px;
    return v;
}

static ns_css_value *
keyword_value_take(char *kw)
{
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_KEYWORD;
    v->u.keyword = kw;
    return v;
}

static ns_css_value *
keyword_value(const char *kw)
{
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_KEYWORD;
    v->u.keyword = g_strdup(kw);
    return v;
}

static const char *
anchor_default_name(const ns_style *s)
{
    const ns_css_value *v = s->values[NS_CSS_POSITION_ANCHOR];
    if (v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
        g_str_has_prefix(v->u.keyword, "--"))
        return v->u.keyword;
    return NULL;
}

static gboolean
self_alignment_is_normal(const ns_style *s, ns_css_prop prop)
{
    const ns_css_value *v = s->values[prop];
    return !v || keyword_is(v, "normal") || keyword_is(v, "auto");
}

static gboolean
anchor_place_axis(ns_style *c, gboolean vertical, int lo, int hi,
                  const double ar[4], gboolean area, double imcb[4])
{
    double cb_start = vertical ? imcb[1] : imcb[0];
    double cb_size = vertical ? imcb[3] : imcb[2];
    double a_start = vertical ? ar[1] : ar[0];
    double a_size = vertical ? ar[3] : ar[2];
    double edges[4] = { cb_start, a_start, a_start + a_size, cb_start + cb_size };
    ns_css_prop near = vertical ? NS_CSS_TOP : NS_CSS_LEFT;
    ns_css_prop far = vertical ? NS_CSS_BOTTOM : NS_CSS_RIGHT;
    ns_css_prop align = vertical ? NS_CSS_ALIGN_SELF : NS_CSS_JUSTIFY_SELF;
    gboolean normal = self_alignment_is_normal(c, align);
    gboolean centered_area = (lo == 1 && hi == 1) || (lo == 0 && hi == 2);
    gboolean anchor_center = keyword_is(c->values[align], "anchor-center") ||
                             (area && normal && centered_area);
    gboolean near_auto = !c->values[near] || length_is_auto(c->values[near]);
    gboolean far_auto = !c->values[far] || length_is_auto(c->values[far]);
    double x0, x1;
    if (anchor_center && (area || (near_auto && far_auto))) {
        double mid = a_start + a_size / 2;
        double half = MIN(mid - cb_start, cb_start + cb_size - mid);
        if (half < 0) half = 0;
        x0 = mid - half;
        x1 = mid + half;
    } else if (area) {
        x0 = edges[lo];
        x1 = edges[hi + 1];
    } else {
        return FALSE;
    }
    if (x1 < x0) x1 = x0;
    if (near_auto) style_put(c, near, px_value(0));
    if (far_auto) style_put(c, far, px_value(0));
    if (anchor_center)
        style_put(c, align, keyword_value("center"));
    else if (normal)
        style_put(c, align, keyword_value(lo == 0 && hi <= 1 ? "end"
                                          : hi == 2 && lo >= 1 ? "start" : "center"));
    if (vertical) {
        imcb[1] = x0;
        imcb[3] = x1 - x0;
    } else {
        imcb[0] = x0;
        imcb[2] = x1 - x0;
    }
    return TRUE;
}

static ns_style *
anchor_resolve_style(const ns_box *abox, GHashTable *box_map, double cb_x,
                     double cb_y, double cb_w, double cb_h, double imcb[4],
                     gboolean *has_imcb)
{
    *has_imcb = FALSE;
    const ns_style *s = abox->style;
    if (!style_uses_anchors(s)) return NULL;
    ns_style *c = ns_style_clone(s);
    anchor_lookup_ud ud = { box_map, abox->dom, anchor_default_name(s) };
    ns_anchor_eval_ctx cx = {
        .cb_x = cb_x, .cb_y = cb_y, .cb_w = cb_w, .cb_h = cb_h,
        .font_px = 16, .root_font_px = 16,
        .viewport_w = ns_css_viewport_w(), .viewport_h = ns_css_viewport_h(),
        .lookup = anchor_lookup_cb, .user = &ud, .allow_anchor = TRUE,
    };
    const ns_css_value *fs = s->values[NS_CSS_FONT_SIZE];
    if (fs && fs->kind == NS_CSS_V_LENGTH && fs->u.length.unit == NS_CSS_UNIT_PX)
        cx.font_px = fs->u.length.v;
    static const struct { ns_css_prop prop; gboolean vertical, far, inset; const char *initial; }
        props[] = {
        { NS_CSS_TOP, TRUE, FALSE, TRUE, "auto" },
        { NS_CSS_BOTTOM, TRUE, TRUE, TRUE, "auto" },
        { NS_CSS_LEFT, FALSE, FALSE, TRUE, "auto" },
        { NS_CSS_RIGHT, FALSE, TRUE, TRUE, "auto" },
        { NS_CSS_WIDTH, FALSE, FALSE, FALSE, "auto" },
        { NS_CSS_MIN_WIDTH, FALSE, FALSE, FALSE, "auto" },
        { NS_CSS_MAX_WIDTH, FALSE, FALSE, FALSE, "none" },
        { NS_CSS_HEIGHT, TRUE, FALSE, FALSE, "auto" },
        { NS_CSS_MIN_HEIGHT, TRUE, FALSE, FALSE, "auto" },
        { NS_CSS_MAX_HEIGHT, TRUE, FALSE, FALSE, "none" },
        { NS_CSS_MARGIN_TOP, TRUE, FALSE, FALSE, NULL },
        { NS_CSS_MARGIN_BOTTOM, TRUE, FALSE, FALSE, NULL },
        { NS_CSS_MARGIN_LEFT, FALSE, FALSE, FALSE, NULL },
        { NS_CSS_MARGIN_RIGHT, FALSE, FALSE, FALSE, NULL },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(props); i++) {
        const ns_css_value *v = c->values[props[i].prop];
        if (!value_has_anchor(v)) continue;
        cx.vertical_axis = props[i].vertical;
        cx.far_edge = props[i].far;
        cx.allow_anchor = props[i].inset;
        cx.basis = props[i].vertical && props[i].inset ? cb_h : cb_w;
        double px = 0;
        if (ns_css_anchor_fn_eval(v->u.keyword, &cx, &px))
            style_put(c, props[i].prop, px_value(px));
        else
            style_put(c, props[i].prop, props[i].initial ? keyword_value(props[i].initial)
                                                         : px_value(0));
    }
    double ar[4];
    imcb[0] = cb_x;
    imcb[1] = cb_y;
    imcb[2] = cb_w;
    imcb[3] = cb_h;
    gboolean have_anchor = ud.default_name && anchor_lookup_cb(&ud, NULL, ar);
    const ns_css_value *pa = c->values[NS_CSS_POSITION_AREA];
    int col_lo, col_hi, row_lo, row_hi;
    if (have_anchor && pa && pa->kind == NS_CSS_V_KEYWORD &&
        ns_css_position_area_resolve(pa->u.keyword, &col_lo, &col_hi, &row_lo, &row_hi)) {
        *has_imcb |= anchor_place_axis(c, FALSE, col_lo, col_hi, ar, TRUE, imcb);
        *has_imcb |= anchor_place_axis(c, TRUE, row_lo, row_hi, ar, TRUE, imcb);
    } else if (have_anchor) {
        if (keyword_is(c->values[NS_CSS_JUSTIFY_SELF], "anchor-center"))
            *has_imcb |= anchor_place_axis(c, FALSE, 1, 1, ar, FALSE, imcb);
        if (keyword_is(c->values[NS_CSS_ALIGN_SELF], "anchor-center"))
            *has_imcb |= anchor_place_axis(c, TRUE, 1, 1, ar, FALSE, imcb);
    }
    return c;
}

static char *
swap_words(const char *text, const char *const (*pairs)[2], gsize n_pairs)
{
    GString *out = g_string_new(NULL);
    const char *p = text;
    while (*p) {
        gboolean word_start = (g_ascii_isalpha(*p)) &&
            (p == text || !(g_ascii_isalnum(p[-1]) || p[-1] == '-' || p[-1] == '_'));
        if (word_start) {
            const char *e = p;
            while (g_ascii_isalnum(*e) || *e == '-' || *e == '_') e++;
            gsize len = (gsize)(e - p);
            const char *repl = NULL;
            for (gsize i = 0; i < n_pairs && !repl; i++) {
                if (strlen(pairs[i][0]) == len && strncmp(p, pairs[i][0], len) == 0)
                    repl = pairs[i][1];
                else if (strlen(pairs[i][1]) == len && strncmp(p, pairs[i][1], len) == 0)
                    repl = pairs[i][0];
            }
            if (repl) g_string_append(out, repl);
            else g_string_append_len(out, p, (gssize)len);
            p = e;
            continue;
        }
        g_string_append_c(out, *p++);
    }
    return g_string_free(out, FALSE);
}

static void
style_swap_words(ns_style *c, ns_css_prop prop, const char *const (*pairs)[2],
                 gsize n_pairs)
{
    const ns_css_value *v = c->values[prop];
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return;
    char *swapped = ns_css_text_has_anchor_fn(v->u.keyword)
        ? ns_css_anchor_fn_flip(v->u.keyword, pairs, n_pairs, FALSE)
        : swap_words(v->u.keyword, pairs, n_pairs);
    if (!swapped) return;
    ns_css_value *nv = g_new0(ns_css_value, 1);
    nv->kind = NS_CSS_V_KEYWORD;
    nv->u.keyword = swapped;
    ns_css_value_free(c->values[prop]);
    c->values[prop] = nv;
}

static void
style_swap_props(ns_style *c, ns_css_prop a, ns_css_prop b)
{
    ns_css_value *t = c->values[a];
    c->values[a] = c->values[b];
    c->values[b] = t;
}

static void
apply_try_tactic(ns_style *c, const char *tactic)
{
    static const char *const block_words[][2] = {
        { "top", "bottom" }, { "span-top", "span-bottom" }, { "y-start", "y-end" },
        { "span-y-start", "span-y-end" }, { "self-y-start", "self-y-end" },
        { "span-self-y-start", "span-self-y-end" }, { "block-start", "block-end" },
        { "span-block-start", "span-block-end" },
        { "self-block-start", "self-block-end" },
        { "span-self-block-start", "span-self-block-end" },
        { "start", "end" }, { "self-start", "self-end" },
        { "flex-start", "flex-end" },
    };
    static const char *const inline_words[][2] = {
        { "left", "right" }, { "span-left", "span-right" }, { "x-start", "x-end" },
        { "span-x-start", "span-x-end" }, { "self-x-start", "self-x-end" },
        { "span-self-x-start", "span-self-x-end" }, { "inline-start", "inline-end" },
        { "span-inline-start", "span-inline-end" },
        { "self-inline-start", "self-inline-end" },
        { "span-self-inline-start", "span-self-inline-end" },
        { "start", "end" }, { "self-start", "self-end" },
        { "flex-start", "flex-end" },
    };
    static const char *const start_words[][2] = {
        { "top", "left" }, { "bottom", "right" }, { "span-top", "span-left" },
        { "span-bottom", "span-right" }, { "y-start", "x-start" }, { "y-end", "x-end" },
        { "block-start", "inline-start" }, { "block-end", "inline-end" },
        { "width", "height" }, { "block", "inline" },
        { "self-block", "self-inline" },
    };
    gboolean block = strcmp(tactic, "flip-block") == 0 || strcmp(tactic, "flip-y") == 0;
    gboolean inl = strcmp(tactic, "flip-inline") == 0 || strcmp(tactic, "flip-x") == 0;
    if (block || inl) {
        ns_css_prop near = block ? NS_CSS_TOP : NS_CSS_LEFT;
        ns_css_prop far = block ? NS_CSS_BOTTOM : NS_CSS_RIGHT;
        ns_css_prop mnear = block ? NS_CSS_MARGIN_TOP : NS_CSS_MARGIN_LEFT;
        ns_css_prop mfar = block ? NS_CSS_MARGIN_BOTTOM : NS_CSS_MARGIN_RIGHT;
        ns_css_prop align = block ? NS_CSS_ALIGN_SELF : NS_CSS_JUSTIFY_SELF;
        const char *const (*words)[2] = block ? block_words : inline_words;
        gsize nw = block ? G_N_ELEMENTS(block_words) : G_N_ELEMENTS(inline_words);
        style_swap_props(c, near, far);
        style_swap_props(c, mnear, mfar);
        ns_css_prop sides[2] = { near, far };
        for (int k = 0; k < 2; k++) {
            const ns_css_value *sv = c->values[sides[k]];
            if (sv && sv->kind == NS_CSS_V_KEYWORD && sv->u.keyword &&
                ns_css_text_has_anchor_fn(sv->u.keyword)) {
                char *flipped = ns_css_anchor_fn_flip(sv->u.keyword, words, nw, TRUE);
                if (flipped) style_put(c, sides[k], keyword_value_take(flipped));
            }
        }
        style_swap_words(c, align, words, nw);
        style_swap_words(c, NS_CSS_POSITION_AREA, words, nw - 3);
        return;
    }
    if (strcmp(tactic, "flip-start") == 0) {
        static const ns_css_prop swaps[][2] = {
            { NS_CSS_TOP, NS_CSS_LEFT }, { NS_CSS_BOTTOM, NS_CSS_RIGHT },
            { NS_CSS_WIDTH, NS_CSS_HEIGHT }, { NS_CSS_MIN_WIDTH, NS_CSS_MIN_HEIGHT },
            { NS_CSS_MAX_WIDTH, NS_CSS_MAX_HEIGHT },
            { NS_CSS_MARGIN_TOP, NS_CSS_MARGIN_LEFT },
            { NS_CSS_MARGIN_BOTTOM, NS_CSS_MARGIN_RIGHT },
            { NS_CSS_ALIGN_SELF, NS_CSS_JUSTIFY_SELF },
        };
        for (gsize i = 0; i < G_N_ELEMENTS(swaps); i++) {
            style_swap_props(c, swaps[i][0], swaps[i][1]);
            style_swap_words(c, swaps[i][0], start_words, G_N_ELEMENTS(start_words));
            style_swap_words(c, swaps[i][1], start_words, G_N_ELEMENTS(start_words));
        }
        style_swap_words(c, NS_CSS_POSITION_AREA, start_words, G_N_ELEMENTS(start_words));
    }
}

static gboolean
value_is_wide_keyword(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           (strcmp(v->u.keyword, "unset") == 0 || strcmp(v->u.keyword, "initial") == 0 ||
            strcmp(v->u.keyword, "revert") == 0 || strcmp(v->u.keyword, "revert-layer") == 0 ||
            strcmp(v->u.keyword, "inherit") == 0);
}

static gboolean
try_rule_prop_allowed(int prop)
{
    switch (prop) {
    case NS_CSS_TOP: case NS_CSS_RIGHT: case NS_CSS_BOTTOM: case NS_CSS_LEFT:
    case NS_CSS_MARGIN_TOP: case NS_CSS_MARGIN_RIGHT: case NS_CSS_MARGIN_BOTTOM:
    case NS_CSS_MARGIN_LEFT: case NS_CSS_WIDTH: case NS_CSS_HEIGHT:
    case NS_CSS_MIN_WIDTH: case NS_CSS_MIN_HEIGHT: case NS_CSS_MAX_WIDTH:
    case NS_CSS_MAX_HEIGHT: case NS_CSS_ALIGN_SELF: case NS_CSS_JUSTIFY_SELF:
    case NS_CSS_POSITION_ANCHOR: case NS_CSS_POSITION_AREA:
        return TRUE;
    default:
        return FALSE;
    }
}

static void
try_decls_free(GArray *d)
{
    for (guint i = 0; i < d->len; i++)
        ns_css_value_free(g_array_index(d, ns_css_decl, i).value);
    g_array_free(d, TRUE);
}

static ns_style *
anchor_try_style(const ns_style *base, const char *option)
{
    ns_style *c = ns_style_clone(base);
    char **words = g_strsplit(option, " ", -1);
    gboolean ok = TRUE;
    gboolean area = words[0] && !g_str_has_prefix(words[0], "flip-") &&
                    !g_str_has_prefix(words[0], "--");
    if (area) {
        char *decl = g_strdup_printf("position-area: %s", option);
        GArray *d = ns_css_parse_declarations(decl);
        g_free(decl);
        ok = d && d->len > 0;
        for (guint i = 0; ok && i < d->len; i++) {
            ns_css_decl *x = &g_array_index(d, ns_css_decl, i);
            if (x->prop != NS_CSS_POSITION_AREA) continue;
            ns_css_value_free(c->values[x->prop]);
            c->values[x->prop] = ns_css_value_dup(x->value);
        }
        if (d) try_decls_free(d);
    } else {
        for (int i = 0; ok && words[i]; i++) {
            if (g_str_has_prefix(words[i], "--")) {
                const char *body = ns_css_position_try_rule(words[i]);
                if (!body) {
                    ok = FALSE;
                    break;
                }
                GArray *d = ns_css_parse_declarations(body);
                for (guint k = 0; d && k < d->len; k++) {
                    ns_css_decl *x = &g_array_index(d, ns_css_decl, k);
                    if (!try_rule_prop_allowed((int)x->prop)) continue;
                    ns_css_value_free(c->values[x->prop]);
                    c->values[x->prop] = value_is_wide_keyword(x->value)
                        ? NULL : ns_css_value_dup(x->value);
                }
                if (d) try_decls_free(d);
            } else {
                apply_try_tactic(c, words[i]);
            }
        }
    }
    g_strfreev(words);
    if (!ok) {
        ns_style_free(c);
        return NULL;
    }
    return c;
}

typedef struct {
    char  *option;
    double size;
} try_option_rank;

static gint
try_option_rank_cmp(gconstpointer a, gconstpointer b)
{
    const try_option_rank *x = a, *y = b;
    return x->size < y->size ? 1 : x->size > y->size ? -1 : 0;
}

static void
anchor_try_order(char **options, ns_box *abox, GHashTable *box_map,
                 const double cb[4])
{
    const ns_css_value *ov = abox->style->values[NS_CSS_POSITION_TRY_ORDER];
    if (!ov || ov->kind != NS_CSS_V_KEYWORD || !ov->u.keyword ||
        strcmp(ov->u.keyword, "normal") == 0)
        return;
    gboolean by_width = strcmp(ov->u.keyword, "most-width") == 0 ||
                        strcmp(ov->u.keyword, "most-inline-size") == 0;
    int n = (int)g_strv_length(options);
    GArray *ranks = g_array_new(FALSE, FALSE, sizeof(try_option_rank));
    const ns_style *base = abox->style;
    for (int i = 0; i < n; i++) {
        try_option_rank r = { options[i], -1 };
        ns_style *opt = anchor_try_style(base, options[i]);
        if (opt) {
            abox->style = opt;
            double imcb[4];
            gboolean has = FALSE;
            ns_style *res = anchor_resolve_style(abox, box_map, cb[0], cb[1], cb[2],
                                                 cb[3], imcb, &has);
            const ns_style *eff = res ? res : opt;
            if (!has) memcpy(imcb, cb, sizeof imcb);
            double near = 0, far = 0;
            ns_css_prop pn = by_width ? NS_CSS_LEFT : NS_CSS_TOP;
            ns_css_prop pf = by_width ? NS_CSS_RIGHT : NS_CSS_BOTTOM;
            double basis = by_width ? imcb[2] : imcb[3];
            if (eff->values[pn] && eff->values[pn]->kind == NS_CSS_V_LENGTH)
                near = length_resolve(eff->values[pn], basis, 0);
            if (eff->values[pf] && eff->values[pf]->kind == NS_CSS_V_LENGTH)
                far = length_resolve(eff->values[pf], basis, 0);
            r.size = basis - near - far;
            abox->style = base;
            if (res) ns_style_free(res);
            ns_style_free(opt);
        }
        g_array_append_val(ranks, r);
    }
    g_array_sort(ranks, try_option_rank_cmp);
    for (int i = 0; i < n; i++) options[i] = g_array_index(ranks, try_option_rank, i).option;
    g_array_free(ranks, TRUE);
}

static char **
anchor_try_options(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_POSITION_TRY_FALLBACKS] : NULL;
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword ||
        strcmp(v->u.keyword, "none") == 0)
        return NULL;
    char **items = g_strsplit(v->u.keyword, ", ", -1);
    return items && items[0] ? items : (g_strfreev(items), NULL);
}

static gboolean
abs_box_fits(const ns_box *abox, const double imcb[4])
{
    const ns_style *s = abox->style;
    double l = imcb[0], t = imcb[1], r = imcb[0] + imcb[2], b = imcb[1] + imcb[3];
    const ns_css_value *v;
    if ((v = s->values[NS_CSS_LEFT]) && v->kind == NS_CSS_V_LENGTH) l += length_resolve(v, imcb[2], 0);
    if ((v = s->values[NS_CSS_RIGHT]) && v->kind == NS_CSS_V_LENGTH) r -= length_resolve(v, imcb[2], 0);
    if ((v = s->values[NS_CSS_TOP]) && v->kind == NS_CSS_V_LENGTH) t += length_resolve(v, imcb[3], 0);
    if ((v = s->values[NS_CSS_BOTTOM]) && v->kind == NS_CSS_V_LENGTH) b -= length_resolve(v, imcb[3], 0);
    double x0 = abox->x, y0 = abox->y;
    double x1 = x0 + abox->margin.left + abox->border.left + abox->padding.left +
                abox->content_width + abox->padding.right + abox->border.right +
                abox->margin.right;
    double y1 = y0 + abox->margin.top + abox->border.top + abox->padding.top +
                abox->content_height + abox->padding.bottom + abox->border.bottom +
                abox->margin.bottom;
    const double eps = 0.5;
    return x0 >= l - eps && y0 >= t - eps && x1 <= r + eps && y1 <= b + eps;
}

static void
process_absolute_boxes(ns_box *root, GHashTable *styles, double viewport_width)
{
    if (!g_abs_pending || g_abs_pending->len == 0) return;
    {
        const ns_node *order_root =
            g_array_index(g_abs_pending, ns_abs_entry, 0).dom;
        while (order_root->parent) order_root = order_root->parent;
        node_order_build(order_root);
    }
    GHashTable *box_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    abs_box_map_build(box_map, root);
    guint batch_len = g_abs_pending->len;
    double *batch_y = g_new0(double, batch_len);
    gboolean *batch_resolved = g_new0(gboolean, batch_len);
    static_abs_y_precompute(root, box_map, styles, batch_y, batch_resolved);
    for (guint i = 0; i < g_abs_pending->len; i++) {
        ns_abs_entry e = g_array_index(g_abs_pending, ns_abs_entry, i);
        const ns_node *cb_dom = positioned_entry_cb_dom(&e, styles);
        ns_box *cb = cb_dom ? g_hash_table_lookup(box_map, cb_dom) : root;
        ns_box *static_cb = cb;
        ns_box inline_cb;
        if (!cb && !e.fixed &&
            inline_containing_block(root, cb_dom, styles, &inline_cb)) {
            cb = &inline_cb;
            static_cb = nearest_ancestor_box(cb_dom, box_map, root);
        } else if (!cb) {
            cb = root;
            static_cb = root;
            if (e.fixed) cb_dom = NULL;
        }

        ns_box *paint_parent = static_cb;
        if (e.fixed && !cb_dom) {
            const ns_node *anc_dom = abs_entry_cb_dom(&e, styles);
            ns_box *anc = anc_dom ? g_hash_table_lookup(box_map, anc_dom) : NULL;
            for (ns_box *h = anc; h && h != root; h = h->parent) {
                if (h != anc && !style_is_positioned(h->style)) continue;
                if (box_can_host_fixed(h)) {
                    paint_parent = h;
                    break;
                }
            }
        }

        int pp_depth = 0;
        for (const ns_box *p = paint_parent; p; p = p->parent)
            if (++pp_depth >= NS_LAYOUT_MAX_DEPTH) break;
        if (pp_depth >= NS_LAYOUT_MAX_DEPTH) continue;

        ns_box *abox;
        if (e.pseudo) {
            abox = box_new(NS_BOX_BLOCK);
            abox->style = e.pseudo;
            box_mark_abs_pseudo(abox, styles, &e);
            collect_box_bg_image(abox, e.pseudo);
            ns_box *gen = build_pseudo_inline_for(e.pseudo, e.dom, NULL);
            if (gen && gen->kind == NS_BOX_INLINE && gen->text && !*gen->text &&
                !gen->inline_atomics) {
                ns_box_free(gen);
                gen = NULL;
            }
            if (gen) box_append_child(abox, gen);
        } else {
            g_abs_force_build = TRUE;
            abox = build_block(e.dom, styles);
            g_abs_force_build = FALSE;
        }
        if (!abox) continue;

        box_append_child(paint_parent, abox);
        abs_box_map_build(box_map, abox);
        gboolean cb_is_icb = (cb_dom == NULL);
        const ns_style *anchor_base_style = abox->style;
        char **try_options = anchor_try_options(anchor_base_style);
        if (try_options) {
            double ocb[4] = {
                cb_is_icb ? 0 : cb->x + cb->margin.left + cb->border.left,
                cb_is_icb ? 0 : cb->y + cb->margin.top + cb->border.top,
                cb_is_icb ? ns_css_viewport_w()
                          : cb->content_width + cb->padding.left + cb->padding.right,
                cb_is_icb ? ns_css_viewport_h()
                          : cb->content_height + cb->padding.top + cb->padding.bottom,
            };
            anchor_try_order(try_options, abox, box_map, ocb);
        }
        int try_index = -1;
        gboolean try_settled = try_options == NULL;
        double saved_cw = abox->content_width, saved_ch = abox->content_height;
        double saved_dh = abox->definite_height;
    anchor_retry:
        if (abox->owned_style) {
            ns_style_free(abox->owned_style);
            abox->owned_style = NULL;
        }
        abox->style = anchor_base_style;
        abox->content_width = saved_cw;
        abox->content_height = saved_ch;
        abox->definite_height = saved_dh;
        ns_style *try_style = try_index >= 0
            ? anchor_try_style(anchor_base_style, try_options[try_index]) : NULL;
        if (try_style) abox->style = try_style;
        double anchor_imcb[4] = { 0 };
        gboolean anchor_has_imcb = FALSE;
        {
            double acb_x = cb_is_icb ? 0 : cb->x + cb->margin.left + cb->border.left;
            double acb_y = cb_is_icb ? 0 : cb->y + cb->margin.top + cb->border.top;
            double acb_w = cb_is_icb ? ns_css_viewport_w()
                : cb->content_width + cb->padding.left + cb->padding.right;
            double acb_h = cb_is_icb ? ns_css_viewport_h()
                : cb->content_height + cb->padding.top + cb->padding.bottom;
            ns_style *resolved = anchor_resolve_style(abox, box_map, acb_x, acb_y,
                                                      acb_w, acb_h, anchor_imcb,
                                                      &anchor_has_imcb);
            if (resolved) {
                abox->owned_style = resolved;
                abox->style = resolved;
                if (try_style) ns_style_free(try_style);
            } else if (try_style) {
                abox->owned_style = try_style;
            }
            if (!anchor_has_imcb) {
                anchor_imcb[0] = acb_x;
                anchor_imcb[1] = acb_y;
                anchor_imcb[2] = acb_w;
                anchor_imcb[3] = acb_h;
            }
        }
        double cb_pad_w = cb->content_width + cb->padding.left + cb->padding.right;
        double cb_pad_h = cb->content_height + cb->padding.top + cb->padding.bottom;
        double avail = cb_is_icb ? viewport_width
                                 : (cb_pad_w > 0 ? cb_pad_w : viewport_width);
        double cb_h = cb_is_icb ? ns_css_viewport_h() : cb_pad_h;
        const ns_style *cs = cb->style;
        double grid_x = 0, grid_y = 0, grid_w = 0, grid_h = 0;
        gboolean grid_cb = !cb_is_icb && !e.pseudo &&
            grid_abs_containing_block(cb, abox->style,
                                      &grid_x, &grid_y, &grid_w, &grid_h);
        if (anchor_has_imcb) {
            grid_cb = TRUE;
            grid_x = anchor_imcb[0];
            grid_y = anchor_imcb[1];
            grid_w = anchor_imcb[2];
            grid_h = anchor_imcb[3];
        }
        if (grid_cb) {
            avail = grid_w;
            cb_h = grid_h;
        }
        ns_abs_static *st = g_abs_static
            ? g_hash_table_lookup(g_abs_static, abs_entry_static_key(&e)) : NULL;
        const ns_box *flex_parent = e.pseudo ? NULL
            : abs_flex_parent_box(e.dom, box_map);
        gboolean static_rtl = FALSE;
        double static_right = 0;
        if (st && st->run) {
            abs_place_at_static_run(abox, st);
        } else {
            double static_y = static_cb->y + static_cb->margin.top +
                              static_cb->border.top + static_cb->padding.top;
            if (i < batch_len && batch_resolved[i])
                static_y = batch_y[i];
            else
                static_abs_y_walk(static_cb, e.dom, &static_y);
            double base_x = static_cb->x + static_cb->margin.left +
                            static_cb->border.left + static_cb->padding.left;
            abox->x = static_abs_x_from_ancestors(static_cb, e.dom, box_map, base_x,
                                                   &static_rtl, &static_right);
            abox->y = static_y;
        }
        const ns_css_value *awv = abox->style
            ? abox->style->values[NS_CSS_WIDTH] : NULL;
        gboolean has_explicit_width = awv &&
            (awv->kind == NS_CSS_V_LENGTH || awv->kind == NS_CSS_V_CALC);
        const ns_css_value *alv = abox->style ? abox->style->values[NS_CSS_LEFT]   : NULL;
        const ns_css_value *arv = abox->style ? abox->style->values[NS_CSS_RIGHT]  : NULL;
        const ns_css_value *atv = abox->style ? abox->style->values[NS_CSS_TOP]    : NULL;
        const ns_css_value *abv = abox->style ? abox->style->values[NS_CSS_BOTTOM] : NULL;
        gboolean l_set = alv && !length_is_auto(alv) &&
            (alv->kind == NS_CSS_V_LENGTH || alv->kind == NS_CSS_V_CALC);
        gboolean r_set = arv && !length_is_auto(arv) &&
            (arv->kind == NS_CSS_V_LENGTH || arv->kind == NS_CSS_V_CALC);
        gboolean js_stretch = strcmp(abs_self_alignment(abox, NS_CSS_JUSTIFY_SELF),
                                     "stretch") == 0;
        gboolean as_stretch = strcmp(abs_self_alignment(abox, NS_CSS_ALIGN_SELF),
                                     "stretch") == 0;
        gboolean stretch_w = !has_explicit_width &&
            ((l_set && r_set && js_stretch) || height_keyword_stretches(awv));
        double layout_w = avail;
        double inset_w = avail;
        if (l_set) inset_w -= length_resolve(alv, avail, 0);
        if (r_set) inset_w -= length_resolve(arv, avail, 0);
        if (inset_w < 0) inset_w = 0;
        abox->cb_width_override = avail;
        abox->cb_width_set = TRUE;
        abox->cb_height_override = cb_h;
        if (stretch_w) {
            double l = l_set ? length_resolve(alv, avail, 0) : 0;
            double r = r_set ? length_resolve(arv, avail, 0) : 0;
            if (!l_set && !r_set && !grid_cb) {
                double origin = cb->x + cb->margin.left + cb->border.left;
                if (abox->x > origin) l = abox->x - origin;
            }
            layout_w = avail - l - r
                     - abox->margin.left - abox->margin.right
                     - abox->border.left - abox->border.right
                     - abox->padding.left - abox->padding.right;
            if (layout_w < 0) layout_w = 0;
        }
        const ns_css_value *ahv = abox->style
            ? abox->style->values[NS_CSS_HEIGHT] : NULL;
        gboolean has_explicit_height = ahv &&
            (ahv->kind == NS_CSS_V_LENGTH || ahv->kind == NS_CSS_V_CALC);
        if (has_explicit_height && value_is_percent(ahv) &&
            cb_h > 0) {
            double pre_h = resolve_height_with_basis(ahv, avail, cb_h, -1);
            if (pre_h > 0) {
                abox->content_height = pre_h;
                abox->definite_height = pre_h;
            }
        }
        layout_box(abox, layout_w, cs);
        if (!stretch_w && !has_explicit_width && abox->kind == NS_BOX_BLOCK) {
            ns_edges fm = {0}, fp = {0}, fb = {0};
            edges_from_style(abox->style, avail, &fm, &fp, &fb);
            double box_extras = fp.left + fp.right + fb.left + fb.right;
            double outer_extras = box_extras + fm.left + fm.right;
            double fit = measure_natural_width(abox, cs);
            if (!(fit > 0)) fit = estimate_natural_width(abox, inset_w) - box_extras;
            double floor_w = min_width_of(abox, cs);
            if (fit < floor_w) fit = floor_w;
            fit += outer_extras;
            floor_w += outer_extras;
            double fit_w = fit < inset_w ? fit
                         : floor_w > inset_w ? floor_w : inset_w;
            if (fit_w != layout_w) {
                layout_w = fit_w;
                layout_box(abox, layout_w, cs);
            }
        }
        gboolean t_set = atv && !length_is_auto(atv) &&
            (atv->kind == NS_CSS_V_LENGTH || atv->kind == NS_CSS_V_CALC);
        gboolean b_set = abv && !length_is_auto(abv) &&
            (abv->kind == NS_CSS_V_LENGTH || abv->kind == NS_CSS_V_CALC);
        double inset_h = -1;
        if (cb_h > 0) {
            inset_h = cb_h
                - (t_set ? length_resolve(atv, cb_h, 0) : 0)
                - (b_set ? length_resolve(abv, cb_h, 0) : 0)
                - abox->margin.top - abox->margin.bottom
                - abox->border.top - abox->border.bottom
                - abox->padding.top - abox->padding.bottom;
            if (inset_h < 0) inset_h = 0;
        }
        if (has_explicit_height) {
            double explicit_h = resolve_height_with_basis(ahv, avail,
                                                          cb_h,
                                                          -1);
            if (explicit_h >= 0) {
                if (flex_box_is_border_box(abox)) {
                    explicit_h -= abox->padding.top + abox->padding.bottom +
                                  abox->border.top + abox->border.bottom;
                    if (explicit_h < 0) explicit_h = 0;
                }
                abox->content_height = abs_height_within_limits(
                    abox, explicit_h, avail, cb_h, inset_h);
            }
        }
        gboolean intrinsic_height = ahv && ahv->kind == NS_CSS_V_KEYWORD &&
            ahv->u.keyword &&
            (strcmp(ahv->u.keyword, "fit-content") == 0 ||
             strcmp(ahv->u.keyword, "min-content") == 0 ||
             strcmp(ahv->u.keyword, "max-content") == 0);
        double stretched_h = -1;
        if (!has_explicit_height && !intrinsic_height &&
            t_set && b_set && cb_h > 0 && as_stretch) {
            double h = abs_height_within_limits(abox, inset_h, avail, cb_h,
                                                inset_h);
            abox->content_height = h;
            stretched_h = h;
        }
        if (stretched_h >= 0) {
            abox->definite_height = stretched_h;
            layout_box(abox, layout_w, cs);
            double natural_h = abox->content_height;
            abox->content_height = stretched_h;
            if (natural_h > stretched_h + 0.01 && abox->style &&
                overflow_kw_scrolls(overflow_axis_keyword(abox->style,
                                                          NS_CSS_OVERFLOW_Y))) {
                abox->scrolls = TRUE;
                if (natural_h - stretched_h > abox->scroll_max_y)
                    abox->scroll_max_y = natural_h - stretched_h;
            }
        }
        gboolean static_x = (!alv || length_is_auto(alv)) &&
                            (!arv || length_is_auto(arv));
        gboolean static_y = (!atv || length_is_auto(atv)) &&
                            (!abv || length_is_auto(abv));
        if (static_x && static_rtl && !(st && st->run))
            shift_box_tree(abox, static_right - (abox->margin.left + abox->border.left +
                                                 abox->padding.left + abox->content_width +
                                                 abox->padding.right + abox->border.right +
                                                 abox->margin.right) - abox->x, 0);
        double flex_x = 0, flex_y = 0;
        if (flex_parent && (static_x || static_y) &&
            flex_static_position(abox, flex_parent, &flex_x, &flex_y)) {
            shift_box_tree(abox, static_x ? flex_x - abox->x : 0,
                           static_y ? flex_y - abox->y : 0);
        }
        if (grid_cb && flex_parent == cb && (static_x || static_y))
            grid_static_position(abox, cb, grid_x, grid_y, grid_w, grid_h,
                                 static_x, static_y);
        apply_position_offsets(abox, avail, cb_h);
        if (grid_cb)
            position_absolute_box_in(abox, grid_x, grid_y, grid_w, grid_h);
        else
            position_absolute_box(abox, cb, cb_is_icb);
        if (!try_settled && !abs_box_fits(abox, anchor_imcb)) {
            if (try_options[try_index + 1]) {
                try_index++;
                goto anchor_retry;
            }
            try_settled = TRUE;
            try_index = -1;
            goto anchor_retry;
        }
        try_settled = TRUE;
        g_strfreev(try_options);
    }
    g_free(batch_y);
    g_free(batch_resolved);
    g_hash_table_destroy(box_map);
    g_array_set_size(g_abs_pending, 0);
    node_order_clear();
}

static gboolean
box_paint_unbounded(const ns_box *b)
{
    const ns_style *s = b->style;
    if (!s) return FALSE;
    const ns_css_value *tv = s->values[NS_CSS_TRANSFORM];
    if (tv && tv->kind == NS_CSS_V_TRANSFORM && tv->u.transform.n_ops > 0)
        return TRUE;
    if (s->values[NS_CSS_ANIMATION] || s->values[NS_CSS_TRANSITION])
        return TRUE;
    const ns_css_value *pv = s->values[NS_CSS_POSITION];
    if (pv && pv->kind == NS_CSS_V_KEYWORD && pv->u.keyword &&
        (strcmp(pv->u.keyword, "fixed") == 0 ||
         strcmp(pv->u.keyword, "sticky") == 0))
        return TRUE;
    return FALSE;
}

static void
inline_shift_paint_bounds(const ns_box *b, double *top, double *bottom)
{
    double x0, y0, x1, y1;
    if (!ns_inline_shift_bounds(b, &x0, &y0, &x1, &y1)) return;
    *top = MIN(*top, b->y + y0);
    *bottom = MAX(*bottom, b->y + b->content_height + y1);
}

static double
style_filter_blur_spread(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_FILTER] : NULL;
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return 0;
    double spread = 0;
    for (const char *p = strstr(v->u.keyword, "blur("); p;
         p = strstr(p + 5, "blur(")) {
        double sigma = g_ascii_strtod(p + 5, NULL);
        if (sigma * 3.0 > spread) spread = sigma * 3.0;
    }
    return spread;
}

static void
compute_paint_bounds(ns_box *b)
{
    double top = b->y;
    double bottom = b->y + b->margin.top + b->border.top + b->padding.top +
                    b->content_height +
                    b->padding.bottom + b->border.bottom + b->margin.bottom;
    inline_shift_paint_bounds(b, &top, &bottom);
    if (box_paint_unbounded(b)) {
        top = -G_MAXDOUBLE;
        bottom = G_MAXDOUBLE;
    }
    for (ns_box *c = b->first_child; c; c = c->next_sibling) {
        compute_paint_bounds(c);
        if (c->paint_top < top) top = c->paint_top;
        if (c->paint_bottom > bottom) bottom = c->paint_bottom;
    }
    if (b->inline_atomics) {
        for (guint i = 0; i < b->inline_atomics->len; i++) {
            const ns_inline_atomic *atomic =
                &g_array_index(b->inline_atomics, ns_inline_atomic, i);
            ns_box *ab = atomic->box;
            if (!ab) continue;
            compute_paint_bounds(ab);
            double dy = b->y + atomic->owner_offset_y + ab->rel_dy - ab->y;
            if (ab->paint_top + dy < top) top = ab->paint_top + dy;
            if (ab->paint_bottom + dy > bottom) bottom = ab->paint_bottom + dy;
        }
    }
    double spread = style_filter_blur_spread(b->style);
    b->paint_top = top - spread;
    b->paint_bottom = bottom + spread;
}

static ns_box *
ns_layout_build_(const ns_node *doc, GHashTable *styles, double viewport_width)
{
    g_abs_pending = g_array_new(FALSE, FALSE, sizeof(ns_abs_entry));
    g_abs_seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_abs_ph_set = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_abs_static = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, g_free);
    g_contains_block_media_cache = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_select_width_cache = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                 NULL, g_free);
    g_cq_seen_container = FALSE;
    ns_css_set_container_dims(0, 0);
    ns_box *root = build_block(doc, styles);
    if (!root) {
        g_array_free(g_abs_pending, TRUE);
        g_abs_pending = NULL;
        g_clear_pointer(&g_abs_seen, g_hash_table_destroy);
        g_clear_pointer(&g_abs_ph_set, g_hash_table_destroy);
        g_clear_pointer(&g_abs_static, g_hash_table_destroy);
        g_hash_table_destroy(g_contains_block_media_cache);
        g_contains_block_media_cache = NULL;
        g_clear_pointer(&g_select_width_cache, g_hash_table_destroy);
        return NULL;
    }
    root->x = 0;
    root->y = 0;

    layout_block(root, viewport_width, NULL);
    apply_position_offsets(root, viewport_width, root->content_height);
    process_absolute_boxes(root, styles, viewport_width);
    ns_paint_sync_inline_atomic_offsets(root);
    extend_scroll_ranges_from_abs(root);
    compute_paint_bounds(root);
    negative_layers_scan_root(root);

    g_array_free(g_abs_pending, TRUE);
    g_abs_pending = NULL;
    g_clear_pointer(&g_abs_seen, g_hash_table_destroy);
    g_clear_pointer(&g_abs_ph_set, g_hash_table_destroy);
    g_clear_pointer(&g_abs_static, g_hash_table_destroy);
    g_hash_table_destroy(g_contains_block_media_cache);
    g_contains_block_media_cache = NULL;
    g_clear_pointer(&g_select_width_cache, g_hash_table_destroy);
    return root;
}

const char *
ns_box_kind_name(ns_box_kind k)
{
    switch (k) {
    case NS_BOX_BLOCK:      return "block";
    case NS_BOX_INLINE:     return "inline";
    case NS_BOX_TEXT:       return "text";
    case NS_BOX_IMAGE:      return "image";
    case NS_BOX_TABLE:      return "table";
    case NS_BOX_TABLE_CAPTION: return "caption";
    case NS_BOX_TABLE_ROW:  return "row";
    case NS_BOX_TABLE_CELL: return "cell";
    case NS_BOX_VIDEO:      return "video";
    case NS_BOX_MATH:       return "math";
    case NS_BOX_SVG:        return "svg";
    }
    return "?";
}

static void
collect_images_walk(const ns_box *b, GPtrArray *out)
{
    if (!b) return;
    if (b->kind == NS_BOX_IMAGE) g_ptr_array_add(out, (gpointer)b);
    if (b->media && (b->media->bg_image_src || b->media->marker_image_src ||
                     b->media->border_image_src || b->media->mask_image_src))
        g_ptr_array_add(out, (gpointer)b);
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        collect_images_walk(c, out);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            collect_images_walk(
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box, out);
    if (b->media && b->media->svg_foreign)
        for (guint i = 0; i < b->media->svg_foreign->len; i++)
            collect_images_walk(g_ptr_array_index(b->media->svg_foreign, i), out);
}

static void
collect_videos_walk(const ns_box *b, GPtrArray *out)
{
    if (!b) return;
    if (b->kind == NS_BOX_VIDEO) g_ptr_array_add(out, (gpointer)b);
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        collect_videos_walk(c, out);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            collect_videos_walk(
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box, out);
}

void
ns_layout_collect_videos(const ns_box *root, GPtrArray *out_boxes)
{
    collect_videos_walk(root, out_boxes);
}

void
ns_layout_collect_images(const ns_box *root, GPtrArray *out_boxes)
{
    collect_images_walk(root, out_boxes);
}

static guint
count_matches_in_text(const char *text, const char *needle,
                      gboolean case_sensitive)
{
    if (!text || !needle || !*needle) return 0;
    gsize needle_len = strlen(needle);
    gsize text_len = strlen(text);
    guint hits = 0;
    for (gsize i = 0; i + needle_len <= text_len; ) {
        gboolean match = case_sensitive
            ? (strncmp(text + i, needle, needle_len) == 0)
            : (g_ascii_strncasecmp(text + i, needle, needle_len) == 0);
        if (match) {
            hits++;
            i += needle_len;
        } else {
            i++;
        }
    }
    return hits;
}

guint
ns_box_count_matches(const ns_box *root, const char *needle,
                     gboolean case_sensitive)
{
    if (!root || !needle || !*needle) return 0;
    guint sum = 0;
    if (root->kind == NS_BOX_INLINE && root->text)
        sum += count_matches_in_text(root->text, needle, case_sensitive);
    for (const ns_box *c = root->first_child; c; c = c->next_sibling)
        sum += ns_box_count_matches(c, needle, case_sensitive);
    return sum;
}

const ns_box *
ns_box_first_match_below(const ns_box *root, const char *needle,
                         double y_threshold, gboolean case_sensitive)
{
    if (!root || !needle || !*needle) return NULL;
    if (root->kind == NS_BOX_INLINE && root->text && root->y > y_threshold) {
        if (count_matches_in_text(root->text, needle, case_sensitive) > 0)
            return root;
    }
    for (const ns_box *c = root->first_child; c; c = c->next_sibling) {
        const ns_box *m = ns_box_first_match_below(c, needle, y_threshold,
                                                   case_sensitive);
        if (m) return m;
    }
    return NULL;
}

const ns_box *
ns_box_first_match_above(const ns_box *root, const char *needle,
                         double y_threshold, gboolean case_sensitive)
{
    if (!root || !needle || !*needle) return NULL;
    const ns_box *best = NULL;
    if (root->kind == NS_BOX_INLINE && root->text && root->y < y_threshold) {
        if (count_matches_in_text(root->text, needle, case_sensitive) > 0)
            best = root;
    }
    for (const ns_box *c = root->first_child; c; c = c->next_sibling) {
        const ns_box *m = ns_box_first_match_above(c, needle, y_threshold,
                                                   case_sensitive);
        if (m && (!best || m->y > best->y))
            best = m;
    }
    return best;
}

static gboolean
match_ordinal_walk(const ns_box *root, const char *needle,
                   const ns_box *target, gboolean case_sensitive,
                   guint *acc)
{
    if (!root) return FALSE;
    if (root->kind == NS_BOX_INLINE && root->text) {
        guint here = count_matches_in_text(root->text, needle, case_sensitive);
        if (root == target) {
            if (here > 0) { *acc += 1; return TRUE; }
            return FALSE;
        }
        *acc += here;
    }
    for (const ns_box *c = root->first_child; c; c = c->next_sibling)
        if (match_ordinal_walk(c, needle, target, case_sensitive, acc))
            return TRUE;
    return FALSE;
}

guint
ns_box_match_ordinal(const ns_box *root, const char *needle,
                     const ns_box *target, gboolean case_sensitive)
{
    if (!root || !needle || !*needle || !target) return 0;
    guint acc = 0;
    if (match_ordinal_walk(root, needle, target, case_sensitive, &acc))
        return acc;
    return 0;
}

const ns_node *
ns_box_hit_form_dom(const ns_box *root, double x, double y)
{
    return ns_form_hit_walk(root, x, y, NULL);
}

ns_box *
ns_box_hit_scrollable(ns_box *root, double x, double y)
{
    if (!root) return NULL;
    hit_enter_box(root, &x, &y);
    if (!box_hit_untransform_point(root, &x, &y)) return NULL;
    if (root->paint_bottom > root->paint_top &&
        (y < root->paint_top - 1.0 || y > root->paint_bottom + 1.0))
        return NULL;
    gboolean clipped = box_clips_children(root);
    if (clipped && !box_padding_contains(root, x, y))
        return NULL;
    double cx = x + root->scroll_x;
    double cy = y + root->scroll_y;
    for (ns_box *c = root->first_child; c; c = c->next_sibling) {
        ns_box *m = ns_box_hit_scrollable(c, cx, cy);
        if (m) return m;
    }
    if (root->inline_atomics)
        for (guint i = 0; i < root->inline_atomics->len; i++) {
            const ns_inline_atomic *atomic =
                &g_array_index(root->inline_atomics, ns_inline_atomic, i);
            if (!atomic->box) continue;
            double ax, ay;
            inline_atomic_hit_point(root, atomic, cx, cy, &ax, &ay);
            ns_box *m = ns_box_hit_scrollable(atomic->box, ax, ay);
            if (m) return m;
        }
    if (root->scrolls && (root->scroll_max_x > 0 || root->scroll_max_y > 0) &&
        box_padding_contains(root, x, y))
        return root;
    return NULL;
}

typedef struct {
    double   best;
    double   prev;
    gboolean any;
} snap_axis;

typedef struct {
    double left, top, right, bottom;
    double cur_x, cur_y;
    double prev_x, prev_y;
    double max_x, max_y;
} snap_port;

static double
snap_length(const ns_style *s, ns_css_prop p, double basis)
{
    const ns_css_value *v = s ? s->values[p] : NULL;
    if (!v || length_is_auto(v)) return 0;
    double px = length_resolve(v, basis, 0);
    return isfinite(px) ? px : 0;
}

static gboolean
snap_better(const snap_axis *axis, double candidate, double current)
{
    double travel = current - axis->prev;
    gboolean ahead = travel > 0.5 ? candidate > axis->prev + 0.5
                   : travel < -0.5 ? candidate < axis->prev - 0.5
                   : TRUE;
    gboolean best_ahead = travel > 0.5 ? axis->best > axis->prev + 0.5
                        : travel < -0.5 ? axis->best < axis->prev - 0.5
                        : TRUE;
    if (!axis->any) return TRUE;
    if (ahead != best_ahead) return ahead;
    return fabs(candidate - current) < fabs(axis->best - current);
}

static void
snap_consider(snap_axis *axis, double candidate, double current, double max)
{
    if (candidate < 0) candidate = 0;
    if (candidate > max) candidate = max;
    if (snap_better(axis, candidate, current)) {
        axis->best = candidate;
        axis->any = TRUE;
    }
}

static void
snap_collect(ns_box *b, const snap_port *port, snap_axis *ax, snap_axis *ay,
             gboolean want_x, gboolean want_y)
{
    double sp_top = port->top, sp_bottom = port->bottom;
    double sp_left = port->left, sp_right = port->right;
    for (ns_box *c = b->first_child; c; c = c->next_sibling) {
        const char *align = c->style
            ? ns_style_keyword(c->style, NS_CSS_SCROLL_SNAP_ALIGN) : NULL;
        if (align && strcmp(align, "none none") != 0) {
            char block[16] = {0}, inline_kw[16] = {0};
            sscanf(align, "%15s %15s", block, inline_kw);
            double port_w = sp_right - sp_left;
            double port_h = sp_bottom - sp_top;
            double top = c->y + c->margin.top -
                snap_length(c->style, NS_CSS_SCROLL_MARGIN_TOP, port_h);
            double bottom = c->y + c->margin.top + c->content_height +
                c->padding.top + c->padding.bottom +
                c->border.top + c->border.bottom +
                snap_length(c->style, NS_CSS_SCROLL_MARGIN_BOTTOM, port_h);
            double left = c->x + c->margin.left -
                snap_length(c->style, NS_CSS_SCROLL_MARGIN_LEFT, port_w);
            double right = c->x + c->margin.left + c->content_width +
                c->padding.left + c->padding.right +
                c->border.left + c->border.right +
                snap_length(c->style, NS_CSS_SCROLL_MARGIN_RIGHT, port_w);
            if (want_y && strcmp(block, "none") != 0)
                snap_consider(ay,
                    strcmp(block, "end") == 0 ? bottom - sp_bottom
                    : strcmp(block, "center") == 0
                        ? (top + bottom - sp_top - sp_bottom) / 2
                        : top - sp_top,
                    port->cur_y, port->max_y);
            if (want_x && strcmp(inline_kw, "none") != 0)
                snap_consider(ax,
                    strcmp(inline_kw, "end") == 0 ? right - sp_right
                    : strcmp(inline_kw, "center") == 0
                        ? (left + right - sp_left - sp_right) / 2
                        : left - sp_left,
                    port->cur_x, port->max_x);
        }
        if (c->scrolls) continue;
        snap_collect(c, port, ax, ay, want_x, want_y);
    }
}

static gboolean
snap_solve(ns_box *root, const char *type, const snap_port *port,
           double *out_x, double *out_y)
{
    char axis_kw[16] = {0}, strictness[16] = {0};
    sscanf(type, "%15s %15s", axis_kw, strictness);
    gboolean want_x = strcmp(axis_kw, "x") == 0 ||
                      strcmp(axis_kw, "inline") == 0 ||
                      strcmp(axis_kw, "both") == 0;
    gboolean want_y = strcmp(axis_kw, "y") == 0 ||
                      strcmp(axis_kw, "block") == 0 ||
                      strcmp(axis_kw, "both") == 0;
    if (!want_x && !want_y) return FALSE;

    snap_axis ax = { .prev = port->prev_x }, ay = { .prev = port->prev_y };
    snap_collect(root, port, &ax, &ay, want_x, want_y);

    gboolean mandatory = strcmp(strictness, "mandatory") == 0;
    double port_w = port->right - port->left;
    double port_h = port->bottom - port->top;
    gboolean snapped = FALSE;
    if (ay.any && (mandatory || fabs(ay.best - port->cur_y) <= port_h / 2)) {
        *out_y = ay.best;
        snapped = TRUE;
    }
    if (ax.any && (mandatory || fabs(ax.best - port->cur_x) <= port_w / 2)) {
        *out_x = ax.best;
        snapped = TRUE;
    }
    return snapped;
}

void
ns_box_scroll_snap_from(ns_box *scroller, double prev_x, double prev_y)
{
    ns_box_note_scroll();
    if (!scroller || !scroller->style) return;
    const char *type = ns_style_keyword(scroller->style,
                                        NS_CSS_SCROLL_SNAP_TYPE);
    if (!type || strcmp(type, "none") == 0) return;

    double port_top = scroller->y + scroller->margin.top + scroller->border.top;
    double port_left = scroller->x + scroller->margin.left +
                       scroller->border.left;
    double port_h = scroller->content_height + scroller->padding.top +
                    scroller->padding.bottom;
    double port_w = scroller->content_width + scroller->padding.left +
                    scroller->padding.right;
    const ns_style *s = scroller->style;
    snap_port port = {
        .top = port_top + snap_length(s, NS_CSS_SCROLL_PADDING_TOP, port_h),
        .bottom = port_top + port_h -
                  snap_length(s, NS_CSS_SCROLL_PADDING_BOTTOM, port_h),
        .left = port_left + snap_length(s, NS_CSS_SCROLL_PADDING_LEFT, port_w),
        .right = port_left + port_w -
                 snap_length(s, NS_CSS_SCROLL_PADDING_RIGHT, port_w),
        .cur_x = scroller->scroll_x, .cur_y = scroller->scroll_y,
        .prev_x = prev_x, .prev_y = prev_y,
        .max_x = scroller->scroll_max_x, .max_y = scroller->scroll_max_y,
    };

    double x = scroller->scroll_x, y = scroller->scroll_y;
    if (snap_solve(scroller, type, &port, &x, &y)) {
        scroller->scroll_x = x;
        scroller->scroll_y = y;
    }
}

gboolean
ns_box_scroll_snap_viewport(ns_box *root, const ns_style *s,
                            double viewport_w, double viewport_h,
                            double max_x, double max_y,
                            double prev_x, double prev_y,
                            double *x, double *y)
{
    if (!root || !s || !x || !y) return FALSE;
    if (!(viewport_w > 0) || !(viewport_h > 0)) return FALSE;
    const char *type = ns_style_keyword(s, NS_CSS_SCROLL_SNAP_TYPE);
    if (!type || strcmp(type, "none") == 0) return FALSE;

    snap_port port = {
        .top = snap_length(s, NS_CSS_SCROLL_PADDING_TOP, viewport_h),
        .bottom = viewport_h -
                  snap_length(s, NS_CSS_SCROLL_PADDING_BOTTOM, viewport_h),
        .left = snap_length(s, NS_CSS_SCROLL_PADDING_LEFT, viewport_w),
        .right = viewport_w -
                 snap_length(s, NS_CSS_SCROLL_PADDING_RIGHT, viewport_w),
        .cur_x = *x, .cur_y = *y,
        .prev_x = prev_x, .prev_y = prev_y,
        .max_x = max_x, .max_y = max_y,
    };
    return snap_solve(root, type, &port, x, y);
}

void
ns_box_scroll_snap(ns_box *scroller)
{
    if (scroller)
        ns_box_scroll_snap_from(scroller, scroller->scroll_x,
                                scroller->scroll_y);
}

ns_box *
ns_box_hit_scrollbar(ns_box *root, double x, double y, double *lx, double *ly)
{
    if (!root) return NULL;
    hit_enter_box(root, &x, &y);
    if (!box_hit_untransform_point(root, &x, &y)) return NULL;
    if (root->paint_bottom > root->paint_top &&
        (y < root->paint_top - 1.0 || y > root->paint_bottom + 1.0))
        return NULL;
    gboolean clipped = box_clips_children(root);
    if (clipped && !box_padding_contains(root, x, y))
        return NULL;
    double cx = x + root->scroll_x;
    double cy = y + root->scroll_y;
    for (ns_box *c = root->first_child; c; c = c->next_sibling) {
        ns_box *m = ns_box_hit_scrollbar(c, cx, cy, lx, ly);
        if (m) return m;
    }
    if (root->inline_atomics)
        for (guint i = 0; i < root->inline_atomics->len; i++) {
            const ns_inline_atomic *atomic =
                &g_array_index(root->inline_atomics, ns_inline_atomic, i);
            if (!atomic->box) continue;
            double ax, ay;
            inline_atomic_hit_point(root, atomic, cx, cy, &ax, &ay);
            ns_box *m = ns_box_hit_scrollbar(atomic->box, ax, ay, lx, ly);
            if (m) return m;
        }
    if (root->scrolls && (root->scroll_max_x > 0 || root->scroll_max_y > 0) &&
        box_padding_contains(root, x, y)) {
        if (lx) *lx = x;
        if (ly) *ly = y;
        return root;
    }
    return NULL;
}

typedef struct {
    const ns_box *box;
    double x, y;
    guint order;
    int   z;
} hit_deferred;

static __thread GArray       *g_hit_deferred;
static __thread int           g_hit_defer_depth;
static __thread const ns_box *g_hit_flush_box;
static __thread double        g_hit_local_x, g_hit_local_y;
static double g_hit_vp_x, g_hit_vp_y;
static guint g_box_scroll_epoch;

void
ns_box_note_scroll(void)
{
    g_box_scroll_epoch++;
}

guint
ns_box_scroll_epoch(void)
{
    return g_box_scroll_epoch;
}

void
ns_box_set_hit_viewport(double scroll_x, double scroll_y)
{
    double x = isfinite(scroll_x) ? scroll_x : 0;
    double y = isfinite(scroll_y) ? scroll_y : 0;
    g_box_scroll_epoch += (x != g_hit_vp_x) | (y != g_hit_vp_y);
    g_hit_vp_x = x;
    g_hit_vp_y = y;
}

void
ns_box_hit_viewport(double *scroll_x, double *scroll_y)
{
    *scroll_x = g_hit_vp_x;
    *scroll_y = g_hit_vp_y;
}

typedef struct { double x, y; } box_scroll_pos;

static void
box_collect_scroll(const ns_box *b, GHashTable *map)
{
    if (!b) return;
    if (b->dom && (b->scroll_y != 0.0 || b->scroll_x != 0.0)) {
        box_scroll_pos *p = g_new(box_scroll_pos, 1);
        p->x = b->scroll_x;
        p->y = b->scroll_y;
        g_hash_table_insert(map, (gpointer)b->dom, p);
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        box_collect_scroll(c, map);
    for (guint i = 0; b->inline_atomics && i < b->inline_atomics->len; i++)
        box_collect_scroll(g_array_index(b->inline_atomics, ns_inline_atomic, i).box,
                           map);
}

static void
box_restore_scroll(ns_box *b, GHashTable *map)
{
    if (!b) return;
    const box_scroll_pos *p = b->dom ? g_hash_table_lookup(map, b->dom) : NULL;
    if (p) {
        b->scroll_y = CLAMP(p->y, 0.0, MAX(b->scroll_max_y, 0.0));
        b->scroll_x = CLAMP(p->x, 0.0, MAX(b->scroll_max_x, 0.0));
    }
    for (ns_box *c = b->first_child; c; c = c->next_sibling)
        box_restore_scroll(c, map);
    for (guint i = 0; b->inline_atomics && i < b->inline_atomics->len; i++)
        box_restore_scroll(g_array_index(b->inline_atomics, ns_inline_atomic, i).box,
                           map);
}

GHashTable *
ns_box_scroll_save(const ns_box *root)
{
    GHashTable *map =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    box_collect_scroll(root, map);
    return map;
}

void
ns_box_scroll_restore(ns_box *root, GHashTable *map)
{
    if (g_hash_table_size(map) > 0) box_restore_scroll(root, map);
    g_hash_table_destroy(map);
}

gboolean
ns_box_is_fixed(const ns_box *b)
{
    if (!b || !b->style ||
        !keyword_is(b->style->values[NS_CSS_POSITION], "fixed"))
        return FALSE;
    for (const ns_box *p = b->parent; p; p = p->parent)
        if (style_creates_fixed_cb(p->style, p->dom) ||
            node_is_frame_element(p->dom))
            return FALSE;
    return TRUE;
}

static void
sticky_inset(const ns_css_value *v, double basis, gboolean *set, double *out)
{
    *set = FALSE;
    *out = 0;
    if (!v || length_is_auto(v)) return;
    if (v->kind != NS_CSS_V_LENGTH && v->kind != NS_CSS_V_CALC) return;
    *out = length_resolve(v, basis, 0);
    *set = isfinite(*out);
}

void
ns_box_sticky_offset_in(const ns_box *b, double sp_x0, double sp_y0,
                        double sp_x1, double sp_y1,
                        double *out_dx, double *out_dy)
{
    *out_dx = 0;
    *out_dy = 0;
    if (!b || !b->style ||
        !keyword_is(b->style->values[NS_CSS_POSITION], "sticky"))
        return;
    double box_top = b->y;
    double box_h = b->margin.top + b->border.top + b->padding.top +
                   b->content_height +
                   b->padding.bottom + b->border.bottom + b->margin.bottom;
    double box_left = b->x;
    double box_w = b->margin.left + b->border.left + b->padding.left +
                   b->content_width +
                   b->padding.right + b->border.right + b->margin.right;
    double cb_top, cb_bot, cb_left, cb_right;
    const ns_box *p = b->parent;
    if (p) {
        cb_left = p->x + p->margin.left + p->border.left + p->padding.left;
        cb_top  = p->y + p->margin.top  + p->border.top  + p->padding.top;
        cb_right = cb_left + p->content_width;
        cb_bot   = cb_top  + p->content_height;
    } else {
        cb_left = sp_x0; cb_top = 0;
        cb_right = sp_x1; cb_bot = G_MAXDOUBLE / 2;
    }
    double sp_w = sp_x1 - sp_x0, sp_h = sp_y1 - sp_y0;
    gboolean has_top, has_bot, has_left, has_right;
    double tval, bval, lval, rval;
    sticky_inset(b->style->values[NS_CSS_TOP],    sp_h, &has_top,   &tval);
    sticky_inset(b->style->values[NS_CSS_BOTTOM], sp_h, &has_bot,   &bval);
    sticky_inset(b->style->values[NS_CSS_LEFT],   sp_w, &has_left,  &lval);
    sticky_inset(b->style->values[NS_CSS_RIGHT],  sp_w, &has_right, &rval);
    if (has_top) {
        double target = sp_y0 + tval;
        if (box_top < target) {
            double want = target - box_top;
            double cap  = cb_bot - (box_top + box_h);
            if (cap < 0) cap = 0;
            *out_dy = want < cap ? want : cap;
        }
    }
    if (has_bot && *out_dy == 0) {
        double target = sp_y1 - bval;
        double box_bot = box_top + box_h;
        if (box_bot > target) {
            double want = target - box_bot;
            double cap  = cb_top - box_top;
            if (cap > 0) cap = 0;
            *out_dy = want > cap ? want : cap;
        }
    }
    if (has_left) {
        double target = sp_x0 + lval;
        if (box_left < target) {
            double want = target - box_left;
            double cap  = cb_right - (box_left + box_w);
            if (cap < 0) cap = 0;
            *out_dx = want < cap ? want : cap;
        }
    }
    if (has_right && *out_dx == 0) {
        double target = sp_x1 - rval;
        double box_right = box_left + box_w;
        if (box_right > target) {
            double want = target - box_right;
            double cap  = cb_left - box_left;
            if (cap > 0) cap = 0;
            *out_dx = want > cap ? want : cap;
        }
    }
    if (!isfinite(*out_dx)) *out_dx = 0;
    if (!isfinite(*out_dy)) *out_dy = 0;
}

static gboolean
box_scrollport_for(const ns_box *b, double *x0, double *y0,
                   double *x1, double *y1)
{
    for (const ns_box *a = b ? b->parent : NULL; a; a = a->parent) {
        if (!a->scrolls) continue;
        *x0 = a->x + a->margin.left + a->border.left + a->scroll_x;
        *y0 = a->y + a->margin.top + a->border.top + a->scroll_y;
        *x1 = *x0 + a->padding.left + a->content_width + a->padding.right;
        *y1 = *y0 + a->padding.top + a->content_height + a->padding.bottom;
        return TRUE;
    }
    return FALSE;
}

void
ns_box_sticky_offset(const ns_box *b, double vp_x0, double vp_y0,
                     double vp_x1, double vp_y1, double *dx, double *dy)
{
    double x0 = vp_x0, y0 = vp_y0, x1 = vp_x1, y1 = vp_y1;
    box_scrollport_for(b, &x0, &y0, &x1, &y1);
    ns_box_sticky_offset_in(b, x0, y0, x1, y1, dx, dy);
}

static gboolean
box_moves_when_painted(const ns_box *b)
{
    if (!b->style || !box_paint_unbounded(b)) return FALSE;
    const ns_css_value *pos = b->style->values[NS_CSS_POSITION];
    return !keyword_is(pos, "fixed") && !keyword_is(pos, "sticky");
}

static void
subtree_extent_y(const ns_box *b, double off, double *top, double *bottom,
                 gboolean *exact)
{
    if (box_moves_when_painted(b)) *exact = FALSE;
    double t = b->y + off;
    double bt = t + b->margin.top + b->border.top + b->padding.top +
                b->content_height + b->padding.bottom + b->border.bottom +
                b->margin.bottom;
    if (isfinite(t) && t < *top) *top = t;
    if (isfinite(bt) && bt > *bottom) *bottom = bt;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        subtree_extent_y(c, off, top, bottom, exact);
    if (!b->inline_atomics) return;
    for (guint i = 0; i < b->inline_atomics->len; i++) {
        const ns_inline_atomic *a =
            &g_array_index(b->inline_atomics, ns_inline_atomic, i);
        if (a->box)
            subtree_extent_y(a->box, off + b->y + a->owner_offset_y +
                                     a->box->rel_dy - a->box->y,
                             top, bottom, exact);
    }
}

gboolean
ns_box_subtree_extent_y(const ns_box *b, double *top, double *bottom)
{
    gboolean exact = TRUE;
    *top = G_MAXDOUBLE;
    *bottom = -G_MAXDOUBLE;
    if (b) subtree_extent_y(b, 0, top, bottom, &exact);
    if (*top > *bottom) *top = *bottom = 0;
    return exact;
}

gboolean
ns_box_in_scroller(const ns_box *b)
{
    double x0, y0, x1, y1;
    return box_scrollport_for(b, &x0, &y0, &x1, &y1);
}

gboolean
ns_box_sticky_y_model(const ns_box *b, double viewport_h, ns_sticky_y *out)
{
    memset(out, 0, sizeof *out);
    const ns_box *p = b ? b->parent : NULL;
    if (!p || !b->style ||
        !keyword_is(b->style->values[NS_CSS_POSITION], "sticky"))
        return FALSE;
    double box_top = b->y;
    double box_h = b->margin.top + b->border.top + b->padding.top +
                   b->content_height +
                   b->padding.bottom + b->border.bottom + b->margin.bottom;
    double cb_top = p->y + p->margin.top + p->border.top + p->padding.top;
    double cb_bot = cb_top + p->content_height;
    double tval, bval;
    sticky_inset(b->style->values[NS_CSS_TOP], viewport_h, &out->has_top,
                 &tval);
    sticky_inset(b->style->values[NS_CSS_BOTTOM], viewport_h,
                 &out->has_bottom, &bval);
    if (out->has_top) {
        out->top_start = box_top - tval;
        out->top_cap = MAX(cb_bot - (box_top + box_h), 0);
    }
    if (out->has_bottom) {
        out->bottom_start = box_top + box_h + bval - viewport_h;
        out->bottom_cap = MIN(cb_top - box_top, 0);
    }
    return isfinite(out->top_start) && isfinite(out->top_cap) &&
           isfinite(out->bottom_start) && isfinite(out->bottom_cap);
}

void
ns_box_hit_offset(const ns_box *b, double *dx, double *dy)
{
    *dx = 0;
    *dy = 0;
    if (!b || !b->style) return;
    const ns_css_value *pv = b->style->values[NS_CSS_POSITION];
    if (!pv || pv->kind != NS_CSS_V_KEYWORD || !pv->u.keyword) return;
    if (strcmp(pv->u.keyword, "fixed") == 0) {
        if (ns_box_is_fixed(b)) {
            *dx = g_hit_vp_x;
            *dy = g_hit_vp_y;
        }
    } else if (strcmp(pv->u.keyword, "sticky") == 0) {
        ns_box_sticky_offset(b, g_hit_vp_x, g_hit_vp_y,
                             g_hit_vp_x + ns_css_viewport_w(),
                             g_hit_vp_y + ns_css_viewport_h(), dx, dy);
    }
}

static void
hit_enter_box(const ns_box *b, double *x, double *y)
{
    double dx, dy;
    ns_box_hit_offset(b, &dx, &dy);
    *x -= dx;
    *y -= dy;
}

static gboolean
box_defers_hit_layer(const ns_box *b, int *out_z)
{
    if (!ns_box_is_upper_layer(b)) return FALSE;
    *out_z = ns_box_layer_z(b);
    return TRUE;
}

static gboolean
box_svg_yields_hit(const ns_box *b)
{
    if (!b || b->kind != NS_BOX_SVG) return FALSE;
    const ns_style *s = b->style;
    if (!s) return TRUE;
    const ns_css_value *pe = s->values[NS_CSS_POINTER_EVENTS];
    if (pe && pe->kind == NS_CSS_V_KEYWORD && pe->u.keyword &&
        strcmp(pe->u.keyword, "all") == 0)
        return FALSE;
    const ns_css_value *bg = s->values[NS_CSS_BACKGROUND_COLOR];
    if (bg && bg->kind == NS_CSS_V_COLOR && bg->u.color.a > 0) return FALSE;
    const ns_css_value *bi = s->values[NS_CSS_BACKGROUND_IMAGE];
    if (bi && (bi->kind == NS_CSS_V_URL || bi->kind == NS_CSS_V_GRADIENT))
        return FALSE;
    return b->border.top <= 0 && b->border.right <= 0 &&
           b->border.bottom <= 0 && b->border.left <= 0;
}

static int
hit_deferred_cmp(const void *a, const void *b)
{
    const hit_deferred *pa = a, *pb = b;
    if (pa->z != pb->z) return pa->z < pb->z ? -1 : 1;
    return hit_tree_order_cmp(pa->box, pb->box, pa->order, pb->order);
}

static const ns_box *box_hit_test_tree(const ns_box *root, double x, double y);

static const ns_box *box_hit_test_root(const ns_box *root, double x, double y);

static double
svg_foreign_coord(const ns_node *fo, const char *name)
{
    const char *a = ns_element_get_attr(fo, name);
    if (!a || !*a || strchr(a, '%')) return 0;
    return g_ascii_strtod(a, NULL);
}

const ns_box *
ns_layout_svg_foreign_root(const ns_box *svg_box, const ns_node *fo)
{
    if (!svg_box || !svg_box->media || !svg_box->media->svg_foreign)
        return NULL;
    GPtrArray *roots = svg_box->media->svg_foreign;
    for (guint i = 0; i < roots->len; i++) {
        const ns_box *root = g_ptr_array_index(roots, i);
        if (root->dom == fo) return root;
    }
    return NULL;
}

gboolean
ns_layout_svg_foreign_xform(const ns_box *svg_box, const ns_node *fo,
                            double xform[6])
{
    if (!svg_box || !svg_box->dom || !fo) return FALSE;
    ns_svg_geometry g;
    if (!ns_svg_node_geometry(svg_box->dom, fo, svg_box->content_width,
                              svg_box->content_height, svg_box->svg_styles,
                              svg_box->style, &g) || !g.found)
        return FALSE;
    cairo_matrix_t m = g.to_root;
    cairo_matrix_translate(&m, g.has_box ? g.x : svg_foreign_coord(fo, "x"),
                           g.has_box ? g.y : svg_foreign_coord(fo, "y"));
    xform[0] = m.xx;
    xform[1] = m.yx;
    xform[2] = m.xy;
    xform[3] = m.yy;
    xform[4] = m.x0;
    xform[5] = m.y0;
    return TRUE;
}

static const ns_box *
svg_foreign_hit(const ns_box *svg_box, double x, double y)
{
    const ns_box_media *m = svg_box->media;
    if (!m || !m->svg_foreign) return NULL;
    double lx = x - (svg_box->x + svg_box->margin.left + svg_box->border.left +
                     svg_box->padding.left);
    double ly = y - (svg_box->y + svg_box->margin.top + svg_box->border.top +
                     svg_box->padding.top);
    for (guint i = m->svg_foreign->len; i-- > 0;) {
        const ns_box *fo = g_ptr_array_index(m->svg_foreign, i);
        double computed[6];
        const double *t;
        if (m->svg_foreign_xform && m->svg_foreign_xform[i * 7] != 0)
            t = m->svg_foreign_xform + i * 7 + 1;
        else if (ns_layout_svg_foreign_xform(svg_box, fo->dom, computed))
            t = computed;
        else
            continue;
        double det = t[0] * t[3] - t[1] * t[2];
        if (fabs(det) < 1e-9) continue;
        double dx = lx - t[4], dy = ly - t[5];
        double fx = ( t[3] * dx - t[2] * dy) / det;
        double fy = (-t[1] * dx + t[0] * dy) / det;
        if (fx < 0 || fy < 0 || fx > fo->content_width ||
            fy > fo->content_height)
            continue;
        const ns_box *hit = box_hit_test_root(fo, fx, fy);
        if (hit && hit != fo) return hit;
    }
    return NULL;
}

static const ns_box *
hit_flush_deferred(GArray *list)
{
    if (!list || list->len == 0) return NULL;
    g_array_sort(list, hit_deferred_cmp);
    const ns_box *best = NULL;
    const ns_box *saved_flush = g_hit_flush_box;
    for (guint i = 0; i < list->len; i++) {
        const hit_deferred *d = &g_array_index(list, hit_deferred, i);
        g_hit_flush_box = d->box;
        const ns_box *m = box_hit_test_tree(d->box, d->x, d->y);
        if (m) best = m;
    }
    g_hit_flush_box = saved_flush;
    return best;
}

static gboolean
hit_box_unreachable(const ns_box *b)
{
    return !b || (b->negative_lifted && b != g_hit_flush_box) ||
           ns_paint_box_clip_hides(b);
}

static gboolean
hit_contents_unreachable(const ns_box *b, gboolean clipped,
                         double x, double y)
{
    if (b->style &&
        keyword_is(b->style->values[NS_CSS_CONTENT_VISIBILITY], "hidden"))
        return TRUE;
    return clipped && !box_padding_contains(b, x, y);
}

static gboolean
hit_layers_below_self(const ns_box *b)
{
    return b->negative_layers &&
           ns_node_is_element_named(b->dom, "body") &&
           !ns_box_is_stacking_context(b);
}

static gboolean
hit_point_in_box(const ns_box *b, double x, double y)
{
    gboolean block_edges = b->kind == NS_BOX_BLOCK ||
                           b->kind == NS_BOX_TABLE_CAPTION;
    double x1 = b->x + b->content_width
              + (block_edges ? b->padding.left + b->padding.right +
                               b->border.left + b->border.right +
                               b->margin.left + b->margin.right : 0);
    double y1 = b->y + b->content_height
              + (block_edges ? b->padding.top + b->padding.bottom +
                               b->border.top + b->border.bottom +
                               b->margin.top + b->margin.bottom : 0);
    return x >= b->x && x <= x1 && y >= b->y && y <= y1;
}

static const ns_box *
hit_self_test(const ns_box *root, double x, double y)
{
    if (root->kind == NS_BOX_SVG) {
        const ns_box *inner = svg_foreign_hit(root, x, y);
        if (inner) return inner;
    }
    if (box_blocks_hit_testing(root) || box_svg_yields_hit(root) ||
        !root->dom || !hit_point_in_box(root, x, y))
        return NULL;
    g_hit_local_x = x;
    g_hit_local_y = y;
    return root;
}

static gboolean
hit_owns_layer_scope(const ns_box *b, gboolean clipped)
{
    return b == g_hit_flush_box || clipped || ns_box_is_stacking_context(b);
}

static const ns_box *
hit_negative_layers(const ns_box *root, double x, double y,
                    const ns_box *best)
{
    const GPtrArray *layers = root->negative_layers;
    if (!layers) return best;
    const ns_box *saved_flush = g_hit_flush_box;
    for (guint i = 0; i < layers->len; i++) {
        g_hit_flush_box = g_ptr_array_index(layers, i);
        const ns_box *m = box_hit_test_tree(g_hit_flush_box, x, y);
        if (m) best = m;
    }
    g_hit_flush_box = saved_flush;
    return best;
}

static const ns_box *
hit_3d_root(const ns_box *root, double x, double y)
{
    const ns_box *m = ns_paint_3d_pick(root, x, y);
    if (!m && ns_paint_box_establishes_3d(root))
        m = hit_negative_layers(root, x + root->scroll_x, y + root->scroll_y,
                                NULL);
    return m ? m : hit_self_test(root, x, y);
}

static const ns_box *
hit_inline_atomic(const ns_box *ab, double x, double y)
{
    if (style_is_positioned(ab->style) || !ns_box_is_stacking_context(ab))
        return box_hit_test_tree(ab, x, y);
    const ns_box *saved_flush = g_hit_flush_box;
    g_hit_flush_box = ab;
    const ns_box *m = box_hit_test_tree(ab, x, y);
    g_hit_flush_box = saved_flush;
    return m;
}

static const ns_box *
hit_inline_atomics(const ns_box *root, double x, double y, const ns_box *best)
{
    for (guint i = 0; root->inline_atomics && i < root->inline_atomics->len;
         i++) {
        const ns_inline_atomic *atomic =
            &g_array_index(root->inline_atomics, ns_inline_atomic, i);
        if (!atomic->box) continue;
        double ax, ay;
        inline_atomic_hit_point(root, atomic, x, y, &ax, &ay);
        const ns_box *m = hit_inline_atomic(atomic->box, ax, ay);
        if (m) best = m;
    }
    return best;
}

static const ns_box *
box_hit_test_tree(const ns_box *root, double x, double y)
{
    if (hit_box_unreachable(root)) return NULL;
    double lx = x, ly = y;
    hit_enter_box(root, &lx, &ly);
    if (!box_hit_untransform_point(root, &lx, &ly)) return NULL;
    if (root->paint_bottom > root->paint_top &&
        (ly < root->paint_top - 1.0 || ly > root->paint_bottom + 1.0))
        return NULL;
    int defer_z = 0;
    if (g_hit_defer_depth > 0 && root != g_hit_flush_box &&
        box_defers_hit_layer(root, &defer_z)) {
        if (!g_hit_deferred)
            g_hit_deferred = g_array_new(FALSE, FALSE, sizeof(hit_deferred));
        hit_deferred d = { root, x, y, g_hit_deferred->len, defer_z };
        g_array_append_val(g_hit_deferred, d);
        return NULL;
    }
    x = lx;
    y = ly;
    gboolean clipped = box_clips_children(root);
    if (hit_contents_unreachable(root, clipped, x, y))
        return hit_self_test(root, x, y);
    if (ns_paint_3d_registered(root)) return hit_3d_root(root, x, y);
    double cx = x + root->scroll_x;
    double cy = y + root->scroll_y;
    gboolean own_scope = hit_owns_layer_scope(root, clipped);
    GArray *saved_deferred = NULL;
    if (own_scope) {
        saved_deferred = g_hit_deferred;
        g_hit_deferred = NULL;
        g_hit_defer_depth++;
    }
    const ns_box *under = hit_negative_layers(root, cx, cy, NULL);
    const ns_box *best = hit_layers_below_self(root) ? NULL : under;
    guint sn = 0;
    const ns_box **stacked = hit_children_stacked(root, &sn);
    if (stacked) {
        for (guint i = 0; i < sn; i++) {
            const ns_box *m = box_hit_test_tree(stacked[i], cx, cy);
            if (m) best = m;
        }
        g_free(stacked);
    } else {
        for (const ns_box *c = root->first_child; c; c = c->next_sibling) {
            const ns_box *m = box_hit_test_tree(c, cx, cy);
            if (m) best = m;
        }
    }
    best = hit_inline_atomics(root, cx, cy, best);
    if (own_scope) {
        GArray *mine = g_hit_deferred;
        g_hit_deferred = saved_deferred;
        g_hit_defer_depth--;
        const ns_box *m = hit_flush_deferred(mine);
        if (m) best = m;
        if (mine) g_array_free(mine, TRUE);
    }
    if (best) return best;
    const ns_box *self = hit_self_test(root, x, y);
    return self ? self : under;
}

static const ns_box *
box_hit_test_root(const ns_box *root, double x, double y)
{
    GArray *saved_list = g_hit_deferred;
    int saved_depth = g_hit_defer_depth;
    const ns_box *saved_flush = g_hit_flush_box;
    g_hit_deferred = NULL;
    g_hit_defer_depth = 0;
    g_hit_flush_box = root;
    const ns_box *m = box_hit_test_tree(root, x, y);
    g_hit_deferred = saved_list;
    g_hit_defer_depth = saved_depth;
    g_hit_flush_box = saved_flush;
    return m;
}

static const ns_box *
box_for_dom_node(const ns_box *root, const ns_node *node)
{
    if (!root) return NULL;
    if (root->dom == node) return root;
    for (const ns_box *c = root->first_child; c; c = c->next_sibling) {
        const ns_box *m = box_for_dom_node(c, node);
        if (m) return m;
    }
    return NULL;
}

const ns_box *
ns_box_hit_test(const ns_box *root, double x, double y)
{
    const ns_node *modal = ns_dom_active_modal();
    if (modal) {
        const ns_box *top = box_for_dom_node(root, modal);
        if (top && top != root) {
            const ns_box *any = box_hit_test_root(root, x, y);
            for (const ns_node *n = any ? any->dom : NULL; n; n = n->parent)
                if (n == modal) return any;
            const ns_box *m = box_hit_test_root(top, x, y);
            if (m) return m;
        }
    }
    return box_hit_test_root(root, x, y);
}

static const ns_node *
image_map_find(const ns_node *n, const char *name, int depth)
{
    if (!n || depth >= 512) return NULL;
    if (ns_node_is_element_named(n, "map")) {
        const char *id = ns_element_id_attr(n);
        const char *nm = ns_element_get_attr(n, "name");
        if ((id && strcmp(id, name) == 0) || (nm && strcmp(nm, name) == 0))
            return n;
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || ns_element_has_shadow_marker(c))
            continue;
        const ns_node *m = image_map_find(c, name, depth + 1);
        if (m) return m;
    }
    return NULL;
}

static const ns_node *
image_map_for(const ns_node *img)
{
    const char *usemap = ns_element_get_attr(img, "usemap");
    const char *hash = usemap ? strchr(usemap, '#') : NULL;
    if (!hash || !hash[1]) return NULL;
    const ns_node *scope = img;
    while (scope->parent && !ns_element_has_shadow_marker(scope))
        scope = scope->parent;
    return image_map_find(scope, hash + 1, 0);
}

static gboolean
image_map_is_delimiter(char c)
{
    return c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n' ||
           c == '\f' || c == '\r';
}

static double
image_map_number(const char *s, const char *end)
{
    const char *p = s;
    double sign = 1.0;
    if (p < end && (*p == '-' || *p == '+')) {
        if (*p == '-') sign = -1.0;
        p++;
    }
    const char *digits = p;
    while (p < end && g_ascii_isdigit(*p)) p++;
    if (p < end && *p == '.' && p + 1 < end && g_ascii_isdigit(p[1])) {
        p++;
        while (p < end && g_ascii_isdigit(*p)) p++;
    }
    if (p == digits) return 0;
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *e = p + 1;
        if (e < end && (*e == '-' || *e == '+')) e++;
        if (e < end && g_ascii_isdigit(*e)) {
            p = e;
            while (p < end && g_ascii_isdigit(*p)) p++;
        }
    }
    char *copy = g_strndup(digits, (gsize)(p - digits));
    double v = g_ascii_strtod(copy, NULL);
    g_free(copy);
    return isfinite(v) ? sign * v : 0;
}

static GArray *
image_map_coords(const char *coords)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(double));
    const char *p = coords ? coords : "";
    while (*p && image_map_is_delimiter(*p)) p++;
    while (*p) {
        while (*p && !image_map_is_delimiter(*p) && !g_ascii_isdigit(*p) &&
               *p != '.' && *p != '-')
            p++;
        const char *start = p;
        while (*p && !image_map_is_delimiter(*p)) p++;
        double v = image_map_number(start, p);
        g_array_append_val(out, v);
        while (*p && image_map_is_delimiter(*p)) p++;
    }
    return out;
}

static gboolean
image_map_polygon_contains(const double *c, guint n, double x, double y)
{
    gboolean inside = FALSE;
    guint pts = n / 2;
    for (guint i = 0, j = pts - 1; i < pts; j = i++) {
        double xi = c[2 * i], yi = c[2 * i + 1];
        double xj = c[2 * j], yj = c[2 * j + 1];
        double cross = (xj - xi) * (y - yi) - (yj - yi) * (x - xi);
        if (fabs(cross) < 1e-6 && x >= MIN(xi, xj) && x <= MAX(xi, xj) &&
            y >= MIN(yi, yj) && y <= MAX(yi, yj))
            return TRUE;
        if ((yi > y) != (yj > y) &&
            x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            inside = !inside;
    }
    return inside;
}

static gboolean
image_map_area_contains(const ns_node *area, double x, double y,
                        double width, double height)
{
    const char *shape = ns_element_get_attr(area, "shape");
    GArray *coords = image_map_coords(ns_element_get_attr(area, "coords"));
    const double *c = &g_array_index(coords, double, 0);
    guint n = coords->len;
    gboolean hit = FALSE;
    if (shape && (g_ascii_strcasecmp(shape, "circle") == 0 ||
                  g_ascii_strcasecmp(shape, "circ") == 0)) {
        if (n >= 3 && c[2] > 0)
            hit = (x - c[0]) * (x - c[0]) + (y - c[1]) * (y - c[1]) <=
                  c[2] * c[2];
    } else if (shape && g_ascii_strcasecmp(shape, "default") == 0) {
        hit = x >= 0 && y >= 0 && x < width && y < height;
    } else if (shape && (g_ascii_strcasecmp(shape, "poly") == 0 ||
                         g_ascii_strcasecmp(shape, "polygon") == 0)) {
        if (n >= 6) hit = image_map_polygon_contains(c, n & ~1u, x, y);
    } else if (n >= 4) {
        double x1 = MIN(c[0], c[2]), x2 = MAX(c[0], c[2]);
        double y1 = MIN(c[1], c[3]), y2 = MAX(c[1], c[3]);
        hit = x >= x1 && x < x2 && y >= y1 && y < y2;
    }
    g_array_free(coords, TRUE);
    return hit;
}

static const ns_node *
image_map_area_in(const ns_node *n, double x, double y, double width,
                  double height, int depth)
{
    if (!n || depth >= 512) return NULL;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT) continue;
        if (ns_node_is_element_named(c, "area") &&
            image_map_area_contains(c, x, y, width, height))
            return c;
        const ns_node *m = image_map_area_in(c, x, y, width, height, depth + 1);
        if (m) return m;
    }
    return NULL;
}

const ns_box *
ns_box_hit_test_local(const ns_box *root, double x, double y,
                      double *local_x, double *local_y)
{
    const ns_box *hit = ns_box_hit_test(root, x, y);
    *local_x = g_hit_local_x;
    *local_y = g_hit_local_y;
    return hit;
}

const ns_node *
ns_box_image_map_area(const ns_box *b, double local_x, double local_y)
{
    if (!b || b->kind != NS_BOX_IMAGE ||
        !ns_node_is_element_named(b->dom, "img") ||
        !ns_element_get_attr(b->dom, "usemap"))
        return NULL;
    const ns_node *map = image_map_for(b->dom);
    if (!map) return NULL;
    double x = local_x - (b->x + b->margin.left + b->border.left +
                          b->padding.left);
    double y = local_y - (b->y + b->margin.top + b->border.top +
                          b->padding.top);
    return image_map_area_in(map, x, y, b->content_width, b->content_height,
                             0);
}

const ns_node *
ns_box_hit_node(const ns_box *root, double x, double y)
{
    double local_x = 0, local_y = 0;
    const ns_box *hit = ns_box_hit_test_local(root, x, y, &local_x, &local_y);
    const ns_node *target = hit ? hit->dom : NULL;
    const ns_node *inline_target = ns_box_hit_inline_dom(root, x, y);
    if (inline_target) target = inline_target;
    const ns_node *form_target = ns_box_hit_form_dom(root, x, y);
    if (form_target) target = form_target;
    if (hit && target == hit->dom) {
        const ns_node *area = ns_box_image_map_area(hit, local_x, local_y);
        if (area) target = area;
    }
    return target;
}

static gboolean
inline_box_hit_contains(const ns_box *box, double x, double y)
{
    double lx = x - box->x;
    double ly = y - box->y;
    if (lx >= 0 && lx <= box->content_width &&
        ly >= 0 && ly <= box->content_height)
        return TRUE;
    return ns_paint_inline_on_shifted_text(box, lx, ly);
}

const ns_link_range *
ns_box_hit_link_range(const ns_box *root, double x, double y)
{
    if (!root) return NULL;
    if (!box_hit_untransform_point(root, &x, &y)) return NULL;
    if (root->inline_atomics)
        for (guint i = 0; i < root->inline_atomics->len; i++) {
            const ns_inline_atomic *atomic =
                &g_array_index(root->inline_atomics, ns_inline_atomic, i);
            const ns_box *ab = atomic->box;
            if (!ab) continue;
            double ax, ay;
            inline_atomic_hit_point(root, atomic,
                                    x + root->scroll_x,
                                    y + root->scroll_y, &ax, &ay);
            const ns_link_range *r = ns_box_hit_link_range(ab, ax, ay);
            if (r) return r;
        }
    if (!box_blocks_hit_testing(root) &&
        root->kind == NS_BOX_INLINE && root->links &&
        root->links->len > 0) {
        if (inline_box_hit_contains(root, x, y)) {
            gsize byte = 0;
            if (ns_paint_inline_xy_to_byte(root, x - root->x, y - root->y, &byte)) {
                for (guint i = 0; i < root->links->len; i++) {
                    const ns_link_range *r = &g_array_index(root->links, ns_link_range, i);
                    if (byte >= r->start && byte < r->start + r->len)
                        return r;
                }
            }
            return NULL;
        }
    }
    if (box_clips_children(root) && !box_padding_contains(root, x, y))
        return NULL;
    double cx = x + root->scroll_x;
    double cy = y + root->scroll_y;
    const ns_link_range *best = NULL;
    guint sn = 0;
    const ns_box **stacked = hit_children_stacked(root, &sn);
    if (stacked) {
        for (guint i = 0; i < sn; i++) {
            const ns_link_range *r = ns_box_hit_link_range(stacked[i], cx, cy);
            if (r) best = r;
        }
        g_free(stacked);
    } else {
        for (const ns_box *c = root->first_child; c; c = c->next_sibling) {
            const ns_link_range *r = ns_box_hit_link_range(c, cx, cy);
            if (r) best = r;
        }
    }
    return best;
}

const char *
ns_box_hit_link(const ns_box *root, double x, double y)
{
    const ns_link_range *r = ns_box_hit_link_range(root, x, y);
    return r ? r->href : NULL;
}

const ns_node *
ns_box_hit_inline_dom(const ns_box *root, double x, double y)
{
    if (!root) return NULL;
    hit_enter_box(root, &x, &y);
    if (!box_hit_untransform_point(root, &x, &y)) return NULL;
    if (root->paint_bottom > root->paint_top &&
        (y < root->paint_top - 1.0 || y > root->paint_bottom + 1.0))
        return NULL;
    if (root->inline_atomics)
        for (guint i = 0; i < root->inline_atomics->len; i++) {
            const ns_inline_atomic *atomic =
                &g_array_index(root->inline_atomics, ns_inline_atomic, i);
            const ns_box *ab = atomic->box;
            if (!ab) continue;
            double ax, ay;
            inline_atomic_hit_point(root, atomic,
                                    x + root->scroll_x,
                                    y + root->scroll_y, &ax, &ay);
            const ns_node *m = ns_box_hit_inline_dom(ab, ax, ay);
            if (m) return m;
        }
    if (!box_blocks_hit_testing(root) &&
        root->kind == NS_BOX_INLINE && root->attrs &&
        root->attrs->len > 0 && root->text && *root->text) {
        if (inline_box_hit_contains(root, x, y)) {
            gsize byte = 0;
            if (ns_paint_inline_xy_to_byte(root, x - root->x, y - root->y, &byte)) {
                const ns_node *best = NULL;
                gsize best_len = 0;
                for (guint i = 0; i < root->attrs->len; i++) {
                    const ns_inline_attr *r =
                        &g_array_index(root->attrs, ns_inline_attr, i);
                    if (r->kind != NS_INLINE_ELEMENT || !r->dom) continue;
                    if (byte < r->start || byte >= r->start + r->len) continue;
                    if (!best || r->len < best_len) {
                        best = r->dom;
                        best_len = r->len;
                    }
                }
                if (best) return best;
            }
            return NULL;
        }
    }
    if (box_clips_children(root) && !box_padding_contains(root, x, y))
        return NULL;
    if (ns_paint_3d_registered(root)) return NULL;
    double cx = x + root->scroll_x;
    double cy = y + root->scroll_y;
    const ns_node *best = NULL;
    guint sn = 0;
    const ns_box **stacked = hit_children_stacked(root, &sn);
    if (stacked) {
        for (guint i = 0; i < sn; i++) {
            const ns_node *m = ns_box_hit_inline_dom(stacked[i], cx, cy);
            if (m) best = m;
        }
        g_free(stacked);
    } else {
        for (const ns_box *c = root->first_child; c; c = c->next_sibling) {
            const ns_node *m = ns_box_hit_inline_dom(c, cx, cy);
            if (m) best = m;
        }
    }
    return best;
}

static const ns_inline_attr *
inline_attr_element(const ns_inline_attr *r)
{
    return r->kind == NS_INLINE_ELEMENT ? r : NULL;
}

static const ns_mat4 *
inline_rect_enter_box(const ns_box *b, double dx, double dy,
                      const ns_mat4 *outer, ns_mat4 *local)
{
    ns_mat4 m;
    if (!box_local_transform(b, dx, dy, &m)) return outer;
    if (outer)
        ns_mat4_multiply(outer, &m, local);
    else
        *local = m;
    return local;
}

static void
inline_rect_map(const ns_mat4 *xf, double *x, double *y, double *w, double *h)
{
    if (!xf) return;
    double cx[4] = { *x, *x + *w, *x, *x + *w };
    double cy[4] = { *y, *y, *y + *h, *y + *h };
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = 0; i < 4; i++) {
        double px, py, pz, pw;
        ns_mat4_apply(xf, cx[i], cy[i], 0, &px, &py, &pz, &pw);
        if (fabs(pw) > 1e-9) { px /= pw; py /= pw; }
        if (i == 0 || px < x0) x0 = px;
        if (i == 0 || py < y0) y0 = py;
        if (i == 0 || px > x1) x1 = px;
        if (i == 0 || py > y1) y1 = py;
    }
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
}

static void
box_inline_union_for_dom(const ns_box *root, const ns_node *target,
                         double dx, double dy, const ns_mat4 *outer_xf,
                         double *x0, double *y0, double *x1, double *y1,
                         gboolean *any)
{
    if (!root) return;
    ns_mat4 local_xf;
    const ns_mat4 *xf = inline_rect_enter_box(root, dx, dy, outer_xf,
                                              &local_xf);
    if (root->kind == NS_BOX_INLINE && root->attrs && root->text) {
        for (guint i = 0; i < root->attrs->len; i++) {
            const ns_inline_attr *r =
                &g_array_index(root->attrs, ns_inline_attr, i);
            if (r->dom != target) continue;
            double ex, ey, ew = 0, eh;
            if (r->kind == NS_INLINE_EMPTY_ELEMENT || root->phantom_line) {
                if (!ns_paint_inline_empty_rect(root, r, &ex, &ey, &eh))
                    continue;
            } else if (r->len == 0 ||
                       (!ns_paint_inline_toggle_rect(root, r, &ex, &ey,
                                                     &ew, &eh) &&
                        !ns_paint_inline_range_extents(root, r->start, r->len,
                                                       inline_attr_element(r),
                                                       &ex, &ey, &ew, &eh))) {
                continue;
            }
            double rx0 = root->x + dx + ex;
            double ry0 = root->y + dy + ey;
            inline_rect_map(xf, &rx0, &ry0, &ew, &eh);
            double rx1 = rx0 + ew;
            double ry1 = ry0 + eh;
            if (!*any) {
                *x0 = rx0; *y0 = ry0; *x1 = rx1; *y1 = ry1;
                *any = TRUE;
            } else {
                if (rx0 < *x0) *x0 = rx0;
                if (ry0 < *y0) *y0 = ry0;
                if (rx1 > *x1) *x1 = rx1;
                if (ry1 > *y1) *y1 = ry1;
            }
        }
    }
    double cdx = dx - root->scroll_x;
    double cdy = dy - root->scroll_y;
    for (const ns_box *c = root->first_child; c; c = c->next_sibling)
        box_inline_union_for_dom(c, target, cdx, cdy, xf,
                                 x0, y0, x1, y1, any);
    if (root->inline_atomics)
        for (guint i = 0; i < root->inline_atomics->len; i++) {
            const ns_inline_atomic *atomic =
                &g_array_index(root->inline_atomics, ns_inline_atomic, i);
            if (!atomic->box) continue;
            double adx = cdx + root->x + atomic->owner_offset_x +
                         atomic->box->rel_dx - atomic->box->x;
            double ady = cdy + root->y + atomic->owner_offset_y +
                         atomic->box->rel_dy - atomic->box->y;
            box_inline_union_for_dom(atomic->box, target, adx, ady, xf,
                                     x0, y0, x1, y1, any);
        }
}

static void
box_inline_rects_for_dom(const ns_box *root, const ns_node *target,
                         double dx, double dy, const ns_mat4 *outer_xf,
                         GArray *out)
{
    if (!root) return;
    ns_mat4 local_xf;
    const ns_mat4 *xf = inline_rect_enter_box(root, dx, dy, outer_xf,
                                              &local_xf);
    if (root->kind == NS_BOX_INLINE && root->attrs && root->text) {
        for (guint i = 0; i < root->attrs->len; i++) {
            const ns_inline_attr *r =
                &g_array_index(root->attrs, ns_inline_attr, i);
            if (r->dom != target || r->kind != NS_INLINE_ELEMENT || r->len == 0 ||
                root->phantom_line)
                continue;
            guint before = out->len;
            if (!ns_paint_inline_range_rects(root, r->start, r->len, r, out))
                continue;
            for (guint k = before; k + 3 < out->len; k += 4) {
                double *rect = &g_array_index(out, double, k);
                rect[0] += root->x + dx;
                rect[1] += root->y + dy;
                inline_rect_map(xf, &rect[0], &rect[1], &rect[2], &rect[3]);
            }
        }
    }
    double cdx = dx - root->scroll_x;
    double cdy = dy - root->scroll_y;
    for (const ns_box *c = root->first_child; c; c = c->next_sibling)
        box_inline_rects_for_dom(c, target, cdx, cdy, xf, out);
    if (root->inline_atomics)
        for (guint i = 0; i < root->inline_atomics->len; i++) {
            const ns_inline_atomic *atomic =
                &g_array_index(root->inline_atomics, ns_inline_atomic, i);
            if (!atomic->box) continue;
            double adx = cdx + root->x + atomic->owner_offset_x +
                         atomic->box->rel_dx - atomic->box->x;
            double ady = cdy + root->y + atomic->owner_offset_y +
                         atomic->box->rel_dy - atomic->box->y;
            box_inline_rects_for_dom(atomic->box, target, adx, ady, xf, out);
        }
}

GArray *
ns_box_inline_rects_for_dom(const ns_box *root, const ns_node *target)
{
    if (!root || !target) return NULL;
    GArray *out = g_array_new(FALSE, FALSE, sizeof(double));
    box_inline_rects_for_dom(root, target, 0, 0, NULL, out);
    if (out->len == 0) {
        g_array_free(out, TRUE);
        return NULL;
    }
    return out;
}

gboolean
ns_box_inline_rect_for_dom(const ns_box *root, const ns_node *target,
                           double *x, double *y, double *w, double *h)
{
    if (!root || !target) return FALSE;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    gboolean any = FALSE;
    box_inline_union_for_dom(root, target, 0, 0, NULL, &x0, &y0, &x1, &y1, &any);
    if (!any) return FALSE;
    if (x) *x = x0;
    if (y) *y = y0;
    if (w) *w = x1 - x0;
    if (h) *h = y1 - y0;
    return TRUE;
}
