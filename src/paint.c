/* Nordstjernen — Cairo paint.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "paint.h"

static int g_dbg_paint_x = -2, g_dbg_paint_y = -2;

#include <math.h>
#include "ns_pango.h"
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "css.h"
#include "css_shape.h"
#include "dom.h"
#include "spellcheck.h"
#include "font.h"
#include "image.h"
#include "mathml.h"
#include "selection.h"
#include "svg.h"
#include "video.h"

typedef struct rgba {
    double r, g, b, a;
} rgba;

static gboolean       g_caret_visible = TRUE;
static int            g_paint_no_cull;
static GPtrArray     *g_paint_deferred_list;

static int            g_paint_defer_depth;
static const ns_box  *g_paint_flush_box;
static ns_js         *g_paint_js;
static ns_anim       *g_paint_anim;
static gboolean       g_search_case_sensitive;
static const ns_box  *g_search_active_box;

enum {
    PAINT_LAYERS_OFF,
    PAINT_LAYERS_PLAN,
    PAINT_LAYERS_DOC,
};

typedef struct paint_layers_state {
    int mode;
    const ns_box *root;
    const ns_box *owner;
    gboolean flush_layered;
    int vp_seen;
    cairo_matrix_t base;
    cairo_t *doc;
    GHashTable *kinds;
    GArray *found;
    ns_paint_upper_fn upper;
    gpointer upper_data;
    int n_upper;
} paint_layers_state;

static paint_layers_state g_layers;
static const ns_box *g_paint_sticky_static_box;
static guint g_paint_video_layer_gen;
static double g_paint_video_tile_y;

void
ns_paint_set_video_layer(guint gen, double tile_y)
{
    g_paint_video_layer_gen = gen;
    g_paint_video_tile_y = tile_y;
}

void
ns_paint_set_caret_visible(gboolean visible)
{
    g_caret_visible = visible;
}
static cairo_surface_t *texture_surface_cached(ns_texture *tex,
                                               const char *filter_kw);
static cairo_surface_t *svg_raster_cached(ns_texture *tex, GBytes *svg,
                                          double vw, double vh, int pw, int ph,
                                          const char *filter_kw);
static gboolean svg_raster_size(cairo_t *cr, double vw, double vh, int iw,
                                int ih, int *out_pw, int *out_ph);

static void ns_paint_font_metrics(const char *family, double size_px,
                                  int weight, gboolean italic,
                                  ns_css_font_metrics *out);

NsPangoContext *
ns_paint_text_context(void)
{
    static NsPangoContext *cached_ctx;
    if (!cached_ctx) {
        NsPangoFontMap *fm = ns_pango_cairo_font_map_get_default();
        cached_ctx = ns_pango_font_map_create_context(fm);
        cairo_font_options_t *fo = cairo_font_options_create();
        const cairo_font_options_t *base =
            ns_pango_cairo_context_get_font_options(cached_ctx);
        if (base) cairo_font_options_merge(fo, base);
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_subpixel_order(fo, CAIRO_SUBPIXEL_ORDER_DEFAULT);
        cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
#ifdef __APPLE__
        cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE);
#endif
        ns_pango_cairo_context_set_font_options(cached_ctx, fo);
        cairo_font_options_destroy(fo);
        ns_pango_context_set_round_glyph_positions(cached_ctx, FALSE);
        ns_pango_cairo_context_set_resolution(cached_ctx, 72.0);
    }
    return cached_ctx;
}

static NsPangoLayout *
paint_create_layout(void)
{
    return ns_pango_layout_new(ns_paint_text_context());
}

static NsPangoWeight
ns_pango_weight_from_css(int weight)
{
    if (weight <= 100) return NS_PANGO_WEIGHT_THIN;
    if (weight <= 200) return NS_PANGO_WEIGHT_ULTRALIGHT;
    if (weight <= 300) return NS_PANGO_WEIGHT_LIGHT;
    if (weight <= 400) return NS_PANGO_WEIGHT_NORMAL;
    if (weight <= 500) return NS_PANGO_WEIGHT_MEDIUM;
    if (weight <= 600) return NS_PANGO_WEIGHT_SEMIBOLD;
    if (weight <= 700) return NS_PANGO_WEIGHT_BOLD;
    if (weight <= 800) return NS_PANGO_WEIGHT_ULTRABOLD;
    if (weight <= 900) return NS_PANGO_WEIGHT_HEAVY;
    return (NsPangoWeight)weight;
}

int
ns_paint_pango_font_size(double size_px)
{
    if (!(size_px > 0)) return 0;
    if (size_px > 65535) size_px = 65535;
    return (int)(size_px * NS_PANGO_SCALE);
}

static NsPangoStretch
ns_pango_stretch_from_css(int rank)
{
    static const NsPangoStretch map[] = {
        NS_PANGO_STRETCH_ULTRA_CONDENSED,
        NS_PANGO_STRETCH_EXTRA_CONDENSED,
        NS_PANGO_STRETCH_CONDENSED,
        NS_PANGO_STRETCH_SEMI_CONDENSED,
        NS_PANGO_STRETCH_NORMAL,
        NS_PANGO_STRETCH_SEMI_EXPANDED,
        NS_PANGO_STRETCH_EXPANDED,
        NS_PANGO_STRETCH_EXTRA_EXPANDED,
        NS_PANGO_STRETCH_ULTRA_EXPANDED,
    };
    if (rank < 0) rank = 0;
    if (rank > 8) rank = 8;
    return map[rank];
}

gboolean
ns_paint_inline_attr_nowrap(const ns_inline_attr *r)
{
    if (r->kind != NS_INLINE_ELEMENT || !r->style) return FALSE;
    const char *ws = ns_style_keyword(r->style, NS_CSS_WHITE_SPACE);
    return ws && (strcmp(ws, "nowrap") == 0 || strcmp(ws, "pre") == 0);
}

NsPangoAttribute *
ns_paint_inline_font_attr(const ns_inline_attr *r)
{
    switch (r->kind) {
    case NS_INLINE_ELEMENT:
        return ns_paint_inline_attr_nowrap(r)
            ? ns_pango_attr_allow_breaks_new(FALSE) : NULL;
    case NS_INLINE_BOLD:
        return ns_pango_attr_weight_new(NS_PANGO_WEIGHT_BOLD);
    case NS_INLINE_FONT_WEIGHT:
        return ns_pango_attr_weight_new(ns_pango_weight_from_css(r->font_weight));
    case NS_INLINE_FONT_STRETCH:
        return ns_pango_attr_stretch_new(
            ns_pango_stretch_from_css(r->font_stretch));
    case NS_INLINE_FONT_FEATURES:
        return ns_paint_font_features_attr_from_values(
            r->font_kerning, r->font_ligatures, r->font_features);
    case NS_INLINE_FONT_VARIATIONS:
        return ns_paint_font_variations_attr_from_values(r->font_variations);
    case NS_INLINE_ITALIC:
        return ns_pango_attr_style_new(NS_PANGO_STYLE_ITALIC);
    case NS_INLINE_MONOSPACE:
        return ns_pango_attr_family_new("monospace");
    case NS_INLINE_FONT_SIZE:
        return ns_pango_attr_size_new_absolute(
            ns_paint_pango_font_size(r->font_size_px));
    case NS_INLINE_LETTER_SPACING:
        return ns_pango_attr_letter_spacing_new(
            (int)lround(r->letter_spacing_px * NS_PANGO_SCALE));
    case NS_INLINE_FONT_FAMILY: {
        if (!r->family) return NULL;
        char *family = ns_css_font_family_for_pango(r->family);
        NsPangoAttribute *a = ns_pango_attr_family_new(family);
        g_free(family);
        return a;
    }
    case NS_INLINE_RISE:
        return ns_pango_attr_rise_new((int)lround(r->rise_px * NS_PANGO_SCALE));
    case NS_INLINE_SMALL_CAPS:
        return ns_pango_attr_variant_new(NS_PANGO_VARIANT_SMALL_CAPS);
    case NS_INLINE_SPACER: {
        NsPangoRectangle rect = { 0, 0, (int)(r->box_w * NS_PANGO_SCALE), 0 };
        return ns_pango_attr_shape_new(&rect, &rect);
    }
    default:
        return NULL;
    }
}

void
ns_paint_set_search(gboolean case_sensitive, const ns_box *active)
{
    g_search_case_sensitive = case_sensitive;
    g_search_active_box = active;
}

void
ns_paint_set_js(ns_js *js)
{
    g_paint_js = js;
}

void
ns_paint_set_anim(ns_anim *anim)
{
    g_paint_anim = anim;
}

static rgba
rgba_of(const ns_css_value *v, double dr, double dg, double db, double da)
{
    rgba c = { dr, dg, db, da };
    if (!v || v->kind != NS_CSS_V_COLOR) return c;
    c.r = v->u.color.r / 255.0;
    c.g = v->u.color.g / 255.0;
    c.b = v->u.color.b / 255.0;
    c.a = v->u.color.a / 255.0;
    return c;
}

static rgba
rgba_anim(const ns_box *b, ns_css_anim_target which,
          const ns_css_value *v, double dr, double dg, double db, double da)
{
    if (b && b->dom && g_paint_anim) {
        guint8 c[4];
        if (ns_anim_get_color(g_paint_anim, b->dom, which, c)) {
            rgba r = { c[0] / 255.0, c[1] / 255.0, c[2] / 255.0, c[3] / 255.0 };
            return r;
        }
    }
    return rgba_of(v, dr, dg, db, da);
}

static inline void
set_source_rgba(cairo_t *cr, rgba c)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

static gboolean
overflow_kw_clips(const char *ov)
{
    return ov && (g_ascii_strcasecmp(ov, "hidden") == 0 ||
                  g_ascii_strcasecmp(ov, "clip")   == 0 ||
                  g_ascii_strcasecmp(ov, "auto")   == 0 ||
                  g_ascii_strcasecmp(ov, "scroll") == 0);
}

#define length_or ns_css_length_or

#define keyword_is ns_css_keyword_is

static double
bg_size_px(double v, ns_css_unit unit, double basis)
{
    switch (unit) {
    case NS_CSS_UNIT_PERCENT: return v * basis / 100.0;
    case NS_CSS_UNIT_EM:
    case NS_CSS_UNIT_REM:     return v * 16.0;
    case NS_CSS_UNIT_VW:      return v * ns_css_viewport_w() / 100.0;
    case NS_CSS_UNIT_VH:      return v * ns_css_viewport_h() / 100.0;
    case NS_CSS_UNIT_VMIN: {
        double m = MIN(ns_css_viewport_w(), ns_css_viewport_h());
        return v * m / 100.0;
    }
    case NS_CSS_UNIT_VMAX: {
        double m = MAX(ns_css_viewport_w(), ns_css_viewport_h());
        return v * m / 100.0;
    }
    case NS_CSS_UNIT_NUMBER:
    case NS_CSS_UNIT_PX:
    default:                  return v;
    }
}

typedef struct corner_radii {
    double tl, tr, br, bl;
    double tlv, trv, brv, blv;
} corner_radii;

static corner_radii
corner_radii_uniform(double r)
{
    corner_radii c = { r, r, r, r, r, r, r, r };
    return c;
}

static void
corner_radius_px(const ns_css_value *v, double basis_w, double basis_h,
                 double *rh, double *rv)
{
    *rh = -1;
    *rv = -1;
    if (!v) return;
    if (v->kind == NS_CSS_V_LENGTH) {
        double r = v->u.length.v;
        if (v->u.length.unit == NS_CSS_UNIT_PERCENT) {
            *rh = r * basis_w / 100.0;
            *rv = r * basis_h / 100.0;
        } else {
            *rh = r;
            *rv = r;
        }
    } else if (v->kind == NS_CSS_V_SIZE) {
        *rh = v->u.size.w_unit == NS_CSS_UNIT_PERCENT
            ? v->u.size.w * basis_w / 100.0 : v->u.size.w;
        *rv = v->u.size.h_unit == NS_CSS_UNIT_PERCENT
            ? v->u.size.h * basis_h / 100.0 : v->u.size.h;
    } else if (v->kind == NS_CSS_V_CALC) {
        *rh = v->u.calc.px + v->u.calc.pct * basis_w / 100.0;
        *rv = v->u.calc.px + v->u.calc.pct * basis_h / 100.0;
    } else {
        return;
    }
    if (!(*rh > 0)) *rh = 0;
    if (!(*rv > 0)) *rv = 0;
}

static corner_radii
style_border_radii(const ns_style *s, double w, double h)
{
    corner_radii c = {0};
    if (!s) return c;
    double base_h, base_v;
    corner_radius_px(s->values[NS_CSS_BORDER_RADIUS], w, h, &base_h, &base_v);
    if (base_h < 0) base_h = 0;
    if (base_v < 0) base_v = 0;
    static const ns_css_prop corners[4] = {
        NS_CSS_BORDER_TOP_LEFT_RADIUS, NS_CSS_BORDER_TOP_RIGHT_RADIUS,
        NS_CSS_BORDER_BOTTOM_RIGHT_RADIUS, NS_CSS_BORDER_BOTTOM_LEFT_RADIUS,
    };
    double *hs[4] = { &c.tl, &c.tr, &c.br, &c.bl };
    double *vs[4] = { &c.tlv, &c.trv, &c.brv, &c.blv };
    for (int i = 0; i < 4; i++) {
        double rh, rv;
        corner_radius_px(s->values[corners[i]], w, h, &rh, &rv);
        *hs[i] = rh >= 0 ? rh : base_h;
        *vs[i] = rv >= 0 ? rv : base_v;
    }
    return c;
}

static corner_radii
box_border_radii(const ns_box *b)
{
    if (!b) return style_border_radii(NULL, 0, 0);
    double w = b->content_width + b->padding.left + b->padding.right +
               b->border.left + b->border.right;
    double h = b->content_height + b->padding.top + b->padding.bottom +
               b->border.top + b->border.bottom;
    return style_border_radii(b->style, w, h);
}

static gboolean
corner_radii_zero(corner_radii c)
{
    return (c.tl <= 0 || c.tlv <= 0) && (c.tr <= 0 || c.trv <= 0) &&
           (c.br <= 0 || c.brv <= 0) && (c.bl <= 0 || c.blv <= 0);
}

static void
corner_arc(cairo_t *cr, double cx, double cy, double rx, double ry,
           double a0, double a1)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, rx, ry);
    cairo_arc(cr, 0, 0, 1, a0, a1);
    cairo_restore(cr);
}

static void
rounded_rect_path(cairo_t *cr, double x, double y, double w, double h,
                  corner_radii c)
{
    if (corner_radii_zero(c) || !(w > 0) || !(h > 0)) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    double f = 1.0;
    const double sums[4] = { c.tl + c.tr, c.trv + c.brv, c.br + c.bl, c.tlv + c.blv };
    const double lens[4] = { w, h, w, h };
    for (int i = 0; i < 4; i++)
        if (sums[i] > 0 && lens[i] / sums[i] < f) f = lens[i] / sums[i];
    if (f < 1.0) {
        c.tl *= f; c.tr *= f; c.br *= f; c.bl *= f;
        c.tlv *= f; c.trv *= f; c.brv *= f; c.blv *= f;
    }
    cairo_new_sub_path(cr);
    if (c.tr > 0 && c.trv > 0)
        corner_arc(cr, x + w - c.tr, y + c.trv, c.tr, c.trv, -G_PI_2, 0);
    else
        cairo_move_to(cr, x + w, y);
    if (c.br > 0 && c.brv > 0)
        corner_arc(cr, x + w - c.br, y + h - c.brv, c.br, c.brv, 0, G_PI_2);
    else
        cairo_line_to(cr, x + w, y + h);
    if (c.bl > 0 && c.blv > 0)
        corner_arc(cr, x + c.bl, y + h - c.blv, c.bl, c.blv, G_PI_2, G_PI);
    else
        cairo_line_to(cr, x, y + h);
    if (c.tl > 0 && c.tlv > 0)
        corner_arc(cr, x + c.tl, y + c.tlv, c.tl, c.tlv, G_PI, 1.5 * G_PI);
    else
        cairo_line_to(cr, x, y);
    cairo_close_path(cr);
}

static void
fill_outer_shadow(cairo_t *cr, double ox, double oy, double ow, double oh,
                  double ix, double iy, double iw, double ih, corner_radii radii)
{
    double x0 = MIN(ox, ix) - 1, y0 = MIN(oy, iy) - 1;
    double x1 = MAX(ox + ow, ix + iw) + 1, y1 = MAX(oy + oh, iy + ih) + 1;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    rounded_rect_path(cr, ix, iy, iw, ih, radii);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    rounded_rect_path(cr, ox, oy, ow, oh, radii);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void box_blur_argb(guchar *data, int stride, int w, int h, int radius);

typedef struct shadow_blur_key {
    double       sw, sh;
    corner_radii radii;
    double       r, g, b, a;
    int          radius;
    int          inset;
    double       hole_x, hole_y, hole_w, hole_h;
} shadow_blur_key;

#define NS_SHADOW_BLUR_CACHE_BYTES (32u << 20)
#define NS_SHADOW_BLUR_ENTRY_BYTES (4u << 20)

static GHashTable *g_shadow_blur_cache;
static gsize       g_shadow_blur_cache_bytes;

static guint
shadow_blur_key_hash(gconstpointer p)
{
    const guchar *bytes = p;
    guint h = 5381;
    for (gsize i = 0; i < sizeof(shadow_blur_key); i++)
        h = h * 33 + bytes[i];
    return h;
}

static gboolean
shadow_blur_key_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(shadow_blur_key)) == 0;
}

static cairo_surface_t *
blurred_shadow_surface_new(const shadow_blur_key *k, int pad,
                           int surf_w, int surf_h)
{
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, surf_w, surf_h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    cairo_t *scr = cairo_create(surf);
    if (k->inset) {
        cairo_rectangle(scr, 0, 0, surf_w, surf_h);
        if (k->hole_w > 0 && k->hole_h > 0)
            rounded_rect_path(scr, pad + k->hole_x, pad + k->hole_y,
                              k->hole_w, k->hole_h, k->radii);
        cairo_set_fill_rule(scr, CAIRO_FILL_RULE_EVEN_ODD);
    } else {
        rounded_rect_path(scr, pad, pad, k->sw, k->sh, k->radii);
    }
    cairo_set_source_rgba(scr, k->r, k->g, k->b, k->a);
    cairo_fill(scr);
    cairo_destroy(scr);
    cairo_surface_flush(surf);
    guchar *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    box_blur_argb(data, stride, surf_w, surf_h, k->radius);
    box_blur_argb(data, stride, surf_w, surf_h, k->radius);
    box_blur_argb(data, stride, surf_w, surf_h, k->radius);
    cairo_surface_mark_dirty(surf);
    return surf;
}

static cairo_surface_t *
blurred_shadow_cached(const shadow_blur_key *k, int pad, int surf_w, int surf_h,
                      gboolean keep_large)
{
    if (g_shadow_blur_cache) {
        cairo_surface_t *hit = g_hash_table_lookup(g_shadow_blur_cache, k);
        if (hit) return cairo_surface_reference(hit);
    }
    cairo_surface_t *surf = blurred_shadow_surface_new(k, pad, surf_w, surf_h);
    if (!surf) return NULL;
    gsize bytes = (gsize)cairo_image_surface_get_stride(surf) *
                  (gsize)cairo_image_surface_get_height(surf);
    if (bytes > NS_SHADOW_BLUR_ENTRY_BYTES && !keep_large) return surf;
    if (bytes > NS_SHADOW_BLUR_CACHE_BYTES) return surf;
    if (!g_shadow_blur_cache)
        g_shadow_blur_cache = g_hash_table_new_full(
            shadow_blur_key_hash, shadow_blur_key_equal, g_free,
            (GDestroyNotify)cairo_surface_destroy);
    if (g_shadow_blur_cache_bytes + bytes > NS_SHADOW_BLUR_CACHE_BYTES) {
        g_hash_table_remove_all(g_shadow_blur_cache);
        g_shadow_blur_cache_bytes = 0;
    }
    g_hash_table_insert(g_shadow_blur_cache, g_memdup2(k, sizeof *k),
                        cairo_surface_reference(surf));
    g_shadow_blur_cache_bytes += bytes;
    return surf;
}

static int
shadow_band_first(double start, int pad, int radius, double start_r)
{
    return MAX((int)ceil(pad + start + start_r) + radius * 3 + 1, pad);
}

static int
shadow_band_excess(double start, double len, int surf_len, int pad, int radius,
                   double start_r, double end_r)
{
    int first = shadow_band_first(start, pad, radius, start_r);
    int last = MIN((int)floor(pad + start + len - end_r) - radius * 3 - 1,
                   surf_len - pad - 1);
    int uniform = last - first;
    return uniform > 1 ? uniform - 1 : 0;
}

static gboolean
shadow_radii_fit(const corner_radii *c, double w, double h)
{
    const double sums[4] = { c->tl + c->tr, c->trv + c->brv,
                             c->br + c->bl, c->tlv + c->blv };
    const double lens[4] = { w, h, w, h };
    for (int i = 0; i < 4; i++)
        if (sums[i] > 0 && lens[i] / sums[i] < 1.0) return FALSE;
    return TRUE;
}

static double
shadow_corner_w(double w, double h)
{
    return w > 0 && h > 0 ? w : 0;
}

static double
shadow_corner_h(double w, double h)
{
    return w > 0 && h > 0 ? h : 0;
}

static gboolean
shadow_bands(const shadow_blur_key *k, int pad, int surf_w, int surf_h,
             int *dx, int *dy, int *mid_x, int *mid_y)
{
    *dx = *dy = 0;
    double x0 = k->inset ? k->hole_x : 0, y0 = k->inset ? k->hole_y : 0;
    double w = k->inset ? k->hole_w : k->sw, h = k->inset ? k->hole_h : k->sh;
    if (!(w > 0) || !(h > 0) || !shadow_radii_fit(&k->radii, w, h))
        return FALSE;
    const corner_radii *c = &k->radii;
    double left_r = MAX(shadow_corner_w(c->tl, c->tlv),
                        shadow_corner_w(c->bl, c->blv));
    double right_r = MAX(shadow_corner_w(c->tr, c->trv),
                         shadow_corner_w(c->br, c->brv));
    double top_r = MAX(shadow_corner_h(c->tl, c->tlv),
                       shadow_corner_h(c->tr, c->trv));
    double bottom_r = MAX(shadow_corner_h(c->bl, c->blv),
                          shadow_corner_h(c->br, c->brv));
    *dx = shadow_band_excess(x0, w, surf_w, pad, k->radius, left_r, right_r);
    *dy = shadow_band_excess(y0, h, surf_h, pad, k->radius, top_r, bottom_r);
    *mid_x = *dx > 0 ? shadow_band_first(x0, pad, k->radius, left_r) : 0;
    *mid_y = *dy > 0 ? shadow_band_first(y0, pad, k->radius, top_r) : 0;
    return *dx > 0 || *dy > 0;
}

static cairo_surface_t *
shadow_expand_bands(cairo_surface_t *small, int surf_w, int surf_h,
                    int dx, int dy, int mid_x, int mid_y)
{
    cairo_surface_t *full =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, surf_w, surf_h);
    if (cairo_surface_status(full) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(full);
        return NULL;
    }
    cairo_surface_flush(full);
    const guchar *src = cairo_image_surface_get_data(small);
    int src_stride = cairo_image_surface_get_stride(small);
    guchar *dst = cairo_image_surface_get_data(full);
    int dst_stride = cairo_image_surface_get_stride(full);
    gsize head = (gsize)(mid_x + 1) * 4;
    gsize tail = (gsize)(surf_w - mid_x - 1 - dx) * 4;
    for (int y = 0; y < surf_h; y++) {
        int sy = y <= mid_y ? y : (y <= mid_y + dy ? mid_y : y - dy);
        guchar *row = dst + (gsize)y * dst_stride;
        if (y > mid_y && y <= mid_y + dy) {
            memcpy(row, dst + (gsize)mid_y * dst_stride, (gsize)surf_w * 4);
            continue;
        }
        const guchar *srow = src + (gsize)sy * src_stride;
        memcpy(row, srow, head);
        const guchar *mid = srow + (gsize)mid_x * 4;
        for (int x = 0; x < dx; x++) memcpy(row + head + (gsize)x * 4, mid, 4);
        memcpy(row + head + (gsize)dx * 4, srow + head, tail);
    }
    cairo_surface_mark_dirty(full);
    return full;
}

static cairo_surface_t *
blurred_shadow_surface(const shadow_blur_key *k, int pad, int surf_w, int surf_h)
{
    int dx, dy, mid_x, mid_y;
    if (!shadow_bands(k, pad, surf_w, surf_h, &dx, &dy, &mid_x, &mid_y))
        return blurred_shadow_cached(k, pad, surf_w, surf_h, FALSE);
    if (g_shadow_blur_cache) {
        cairo_surface_t *hit = g_hash_table_lookup(g_shadow_blur_cache, k);
        if (hit) return cairo_surface_reference(hit);
    }
    shadow_blur_key small_key = *k;
    small_key.sw -= dx;
    small_key.sh -= dy;
    if (k->inset) {
        small_key.hole_w -= dx;
        small_key.hole_h -= dy;
    }
    cairo_surface_t *small = blurred_shadow_cached(&small_key, pad, surf_w - dx,
                                                   surf_h - dy, TRUE);
    if (!small) return NULL;
    cairo_surface_t *full = shadow_expand_bands(small, surf_w, surf_h,
                                                dx, dy, mid_x, mid_y);
    cairo_surface_destroy(small);
    if (!full) return NULL;
    gsize bytes = (gsize)cairo_image_surface_get_stride(full) * (gsize)surf_h;
    if (bytes > NS_SHADOW_BLUR_ENTRY_BYTES) return full;
    if (g_shadow_blur_cache_bytes + bytes > NS_SHADOW_BLUR_CACHE_BYTES) {
        g_hash_table_remove_all(g_shadow_blur_cache);
        g_shadow_blur_cache_bytes = 0;
    }
    g_hash_table_insert(g_shadow_blur_cache, g_memdup2(k, sizeof *k),
                        cairo_surface_reference(full));
    g_shadow_blur_cache_bytes += bytes;
    return full;
}

static void
paint_blurred_box_shadow(cairo_t *cr, double sx, double sy, double sw, double sh_h,
                         corner_radii radii, double blur,
                         double br, double bg, double bb, double ba,
                         double clip_x, double clip_y, double clip_w, double clip_h,
                         corner_radii clip_radii)
{
    int radius = (int)(blur * 0.5 + 0.5);
    if (radius < 1) radius = 1;
    if (radius > 256) radius = 256;
    int pad = radius * 3 + 2;
    int isw = (int)ceil(sw), ish = (int)ceil(sh_h);
    if (isw < 1) isw = 1;
    if (ish < 1) ish = 1;
    int surf_w = isw + pad * 2, surf_h = ish + pad * 2;
    if (surf_w > 8192 || surf_h > 8192) return;
    shadow_blur_key key;
    memset(&key, 0, sizeof key);
    key.sw = sw;
    key.sh = sh_h;
    key.radii = radii;
    key.r = br;
    key.g = bg;
    key.b = bb;
    key.a = ba;
    key.radius = radius;
    cairo_surface_t *surf = blurred_shadow_surface(&key, pad, surf_w, surf_h);
    if (!surf) return;

    cairo_save(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, sx - pad, sy - pad, surf_w, surf_h);
    rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, clip_radii);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    cairo_set_source_surface(cr, surf, sx - pad, sy - pad);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(surf);
}

static gboolean
box_is_input_block(const ns_box *b)
{
    return b && b->style && b->kind == NS_BOX_BLOCK && b->dom &&
           b->dom->kind == NS_NODE_ELEMENT && b->dom->name &&
           strcmp(b->dom->name, "input") == 0;
}

static gboolean
box_has_native_field_border(const ns_box *b)
{
    static const ns_css_prop side_styles[4] = {
        NS_CSS_BORDER_TOP_STYLE, NS_CSS_BORDER_RIGHT_STYLE,
        NS_CSS_BORDER_BOTTOM_STYLE, NS_CSS_BORDER_LEFT_STYLE,
    };
    for (int i = 0; i < 4; i++)
        if (!keyword_is(b->style->values[side_styles[i]], "inset"))
            return FALSE;
    const double widths[4] = {
        b->border.top, b->border.right, b->border.bottom, b->border.left,
    };
    for (int i = 0; i < 4; i++)
        if (widths[i] != 2) return FALSE;
    return TRUE;
}

static gboolean
input_type_is_text_field(const char *type)
{
    static const char *const text_types[] = {
        "text", "search", "email", "url", "tel", "number", "password",
    };
    if (!type || !*type) return TRUE;
    for (gsize i = 0; i < G_N_ELEMENTS(text_types); i++)
        if (g_ascii_strcasecmp(type, text_types[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean
box_paints_native_text_field(const ns_box *b)
{
    if (!box_is_input_block(b)) return FALSE;
    if (keyword_is(b->style->values[NS_CSS_APPEARANCE], "none")) return FALSE;
    if (!box_has_native_field_border(b)) return FALSE;
    return input_type_is_text_field(ns_element_get_attr(b->dom, "type"));
}

static gboolean
style_side_visible(const ns_style *s, ns_css_prop wp, ns_css_prop sp)
{
    if (!s) return FALSE;
    double w = length_or(s->values[wp], 0);
    if (w <= 0) return FALSE;
    const ns_css_value *st = s->values[sp];
    return st && !keyword_is(st, "none") && !keyword_is(st, "hidden");
}

static gboolean
style_has_inline_box_paint(const ns_style *s)
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
    if (style_side_visible(s, NS_CSS_BORDER_TOP_WIDTH, NS_CSS_BORDER_TOP_STYLE) ||
        style_side_visible(s, NS_CSS_BORDER_RIGHT_WIDTH, NS_CSS_BORDER_RIGHT_STYLE) ||
        style_side_visible(s, NS_CSS_BORDER_BOTTOM_WIDTH, NS_CSS_BORDER_BOTTOM_STYLE) ||
        style_side_visible(s, NS_CSS_BORDER_LEFT_WIDTH, NS_CSS_BORDER_LEFT_STYLE))
        return TRUE;
    return FALSE;
}

static corner_radii
corner_radii_fit(corner_radii c, double w, double h)
{
    double f = 1.0;
    const double sums[4] = { c.tl + c.tr, c.trv + c.brv, c.br + c.bl, c.tlv + c.blv };
    const double lens[4] = { w, h, w, h };
    for (int i = 0; i < 4; i++)
        if (sums[i] > 0 && lens[i] / sums[i] < f) f = lens[i] / sums[i];
    if (f < 1.0) {
        c.tl *= f; c.tr *= f; c.br *= f; c.bl *= f;
        c.tlv *= f; c.trv *= f; c.brv *= f; c.blv *= f;
    }
    return c;
}

static corner_radii
corner_radii_inset(corner_radii c, double top, double right, double bottom,
                   double left)
{
    corner_radii in = {
        MAX(0, c.tl - left), MAX(0, c.tr - right),
        MAX(0, c.br - right), MAX(0, c.bl - left),
        MAX(0, c.tlv - top), MAX(0, c.trv - top),
        MAX(0, c.brv - bottom), MAX(0, c.blv - bottom),
    };
    return in;
}

static corner_radii
outline_radii(corner_radii c, double w, double h, double grow)
{
    c = corner_radii_fit(c, w, h);
    double *r[8] = { &c.tl, &c.tr, &c.br, &c.bl, &c.tlv, &c.trv, &c.brv, &c.blv };
    for (int i = 0; i < 8; i++)
        if (*r[i] > 0) *r[i] = MAX(0, *r[i] + grow);
    return c;
}

static void
paint_inset_box_shadow(cairo_t *cr, const ns_css_shadow *sh,
                       double px, double py, double pw, double ph,
                       corner_radii pad_radii)
{
    if (!(pw > 0) || !(ph > 0)) return;
    double hole_x = sh->x + sh->spread;
    double hole_y = sh->y + sh->spread;
    double hole_w = pw - 2 * sh->spread;
    double hole_h = ph - 2 * sh->spread;
    corner_radii hole_radii = outline_radii(pad_radii, pw, ph, -sh->spread);
    cairo_save(cr);
    rounded_rect_path(cr, px, py, pw, ph, pad_radii);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, sh->r / 255.0, sh->g / 255.0, sh->b / 255.0,
                          sh->a / 255.0);
    int radius = (int)(sh->blur * 0.5 + 0.5);
    int pad = radius * 3 + 2;
    int surf_w = (int)ceil(pw) + pad * 2, surf_h = (int)ceil(ph) + pad * 2;
    if (sh->blur > 0 && radius <= 256 && surf_w <= 4096 && surf_h <= 4096) {
        shadow_blur_key key;
        memset(&key, 0, sizeof key);
        key.sw = pw;
        key.sh = ph;
        key.radii = hole_radii;
        key.r = sh->r / 255.0;
        key.g = sh->g / 255.0;
        key.b = sh->b / 255.0;
        key.a = sh->a / 255.0;
        key.radius = radius;
        key.inset = 1;
        key.hole_x = hole_x;
        key.hole_y = hole_y;
        key.hole_w = hole_w;
        key.hole_h = hole_h;
        cairo_surface_t *surf =
            blurred_shadow_surface(&key, pad, surf_w, surf_h);
        if (surf) {
            cairo_set_source_surface(cr, surf, px - pad, py - pad);
            cairo_paint(cr);
            cairo_surface_destroy(surf);
        }
    } else if (hole_w > 0 && hole_h > 0) {
        cairo_new_path(cr);
        cairo_rectangle(cr, px - 1, py - 1, pw + 2, ph + 2);
        rounded_rect_path(cr, px + hole_x, py + hole_y, hole_w, hole_h,
                          hole_radii);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_fill(cr);
    } else {
        cairo_paint(cr);
    }
    cairo_restore(cr);
}

static gboolean
border_style_is_solid(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           strcmp(v->u.keyword, "solid") == 0;
}

static gboolean
box_is_native_control(const ns_box *b)
{
    if (!b->style || keyword_is(b->style->values[NS_CSS_APPEARANCE], "none"))
        return FALSE;
    return ns_node_is_element_named(b->dom, "button") ||
           ns_node_is_element_named(b->dom, "input") ||
           ns_node_is_element_named(b->dom, "select") ||
           ns_node_is_element_named(b->dom, "textarea");
}

static gboolean
border_style_is_3d(const char *kw)
{
    return strcmp(kw, "inset") == 0 || strcmp(kw, "outset") == 0 ||
           strcmp(kw, "groove") == 0 || strcmp(kw, "ridge") == 0;
}

static void
border_band_point(const double outer[2], const double inner[2], double t,
                  double out[2])
{
    out[0] = outer[0] + (inner[0] - outer[0]) * t;
    out[1] = outer[1] + (inner[1] - outer[1]) * t;
}

static double
color_channel_quantize(double v)
{
    return floor(CLAMP(v, 0.0, 1.0) * 255.99) / 255.0;
}

static rgba
color_scaled(rgba c, double m)
{
    rgba out = { color_channel_quantize(c.r * m), color_channel_quantize(c.g * m),
                 color_channel_quantize(c.b * m), c.a };
    return out;
}

static rgba
color_darker(rgba c)
{
    if (c.r >= 1 && c.g >= 1 && c.b >= 1) {
        rgba w = { 171 / 255.0, 171 / 255.0, 171 / 255.0, c.a };
        return w;
    }
    double v = MAX(c.r, MAX(c.g, c.b));
    return color_scaled(c, v > 0 ? MAX(0.0, (v - 0.33) / v) : 0);
}

static rgba
color_lighter(rgba c)
{
    double v = MAX(c.r, MAX(c.g, c.b));
    if (v <= 0) {
        rgba k = { 84 / 255.0, 84 / 255.0, 84 / 255.0, c.a };
        return k;
    }
    return color_scaled(c, MIN(1.0, v + 0.33) / v);
}

static double
color_distance2(rgba c, double gray)
{
    return (c.r - gray) * (c.r - gray) + (c.g - gray) * (c.g - gray) +
           (c.b - gray) * (c.b - gray);
}

static rgba
border_shade(rgba c, gboolean current, gboolean darker)
{
    if (current) {
        double g = (darker ? 154 : 238) / 255.0;
        rgba fixed = { g, g, g, c.a };
        return fixed;
    }
    gboolean too_dark = color_distance2(c, 0) <= color_distance2(c, 32 / 255.0);
    if (darker) return too_dark ? color_lighter(c) : color_darker(c);
    if (too_dark) return color_lighter(color_lighter(c));
    gboolean too_light = color_distance2(c, 1) <= color_distance2(c, 235 / 255.0);
    return too_light ? c : color_lighter(c);
}

static void
fill_border_band(cairo_t *cr, const double a[2], const double b[2],
                 const double c[2], const double d[2], rgba color)
{
    set_source_rgba(cr, color);
    cairo_new_path(cr);
    cairo_move_to(cr, a[0], a[1]);
    cairo_line_to(cr, b[0], b[1]);
    cairo_line_to(cr, c[0], c[1]);
    cairo_line_to(cr, d[0], d[1]);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void
paint_3d_border_side(cairo_t *cr, int side, const char *kw, rgba c,
                     gboolean current, const double outer[4][2],
                     const double inner[4][2])
{
    int next = (side + 1) % 4;
    gboolean top_left = side == 0 || side == 3;
    gboolean groove = strcmp(kw, "groove") == 0;
    if (!groove && strcmp(kw, "ridge") != 0) {
        gboolean darker = top_left == (strcmp(kw, "inset") == 0);
        fill_border_band(cr, outer[side], outer[next], inner[next],
                         inner[side], border_shade(c, current, darker));
        return;
    }
    double mid0[2], mid1[2];
    border_band_point(outer[side], inner[side], 0.5, mid0);
    border_band_point(outer[next], inner[next], 0.5, mid1);
    gboolean outer_darker = top_left == groove;
    fill_border_band(cr, outer[side], outer[next], mid1, mid0,
                     border_shade(c, current, outer_darker));
    fill_border_band(cr, mid0, mid1, inner[next], inner[side],
                     border_shade(c, current, !outer_darker));
}

static void
paint_double_border_side(cairo_t *cr, int side, double width, rgba c,
                         const double outer[4][2], const double inner[4][2])
{
    int next = (side + 1) % 4;
    double stripe = width >= 3 ? floor((width + 1) / 3.0) / width : 0.5;
    double o0[2], o1[2], i0[2], i1[2];
    border_band_point(outer[side], inner[side], stripe, o0);
    border_band_point(outer[next], inner[next], stripe, o1);
    border_band_point(outer[side], inner[side], 1.0 - stripe, i0);
    border_band_point(outer[next], inner[next], 1.0 - stripe, i1);
    fill_border_band(cr, outer[side], outer[next], o1, o0, c);
    fill_border_band(cr, i0, i1, inner[next], inner[side], c);
}

static void
border_wedge_apex(const double a0[2], const double a1[2],
                  const double b0[2], const double b1[2],
                  double cx, double cy, double *out_x, double *out_y)
{
    double dax = a1[0] - a0[0], day = a1[1] - a0[1];
    double dbx = b1[0] - b0[0], dby = b1[1] - b0[1];
    double den = dax * dby - day * dbx;
    if (fabs(den) < 1e-9) {
        *out_x = cx;
        *out_y = cy;
        return;
    }
    double t = ((b0[0] - a0[0]) * dby - (b0[1] - a0[1]) * dbx) / den;
    *out_x = a0[0] + t * dax;
    *out_y = a0[1] + t * day;
}

static double
snap_device_x(cairo_t *cr, double x)
{
    double y = 0;
    cairo_user_to_device(cr, &x, &y);
    x = round(x);
    cairo_device_to_user(cr, &x, &y);
    return x;
}

static double
snap_device_y(cairo_t *cr, double y)
{
    double x = 0;
    cairo_user_to_device(cr, &x, &y);
    y = round(y);
    cairo_device_to_user(cr, &x, &y);
    return y;
}

static double
snap_inner_edge(cairo_t *cr, double outer, double inner, double width,
                double inward, gboolean vertical)
{
    if (width <= 0) return outer;
    double snapped = vertical ? snap_device_y(cr, inner) : snap_device_x(cr, inner);
    double dx = 1, dy = 1;
    cairo_device_to_user_distance(cr, &dx, &dy);
    double device_px = fabs(vertical ? dy : dx);
    if ((snapped - outer) * inward < device_px * 0.5)
        snapped = outer + inward * device_px;
    return snapped;
}

static void
snap_border_edges(cairo_t *cr, double *l, double *t, double *r, double *b,
                  double *il, double *it, double *ir, double *ib,
                  const ns_box *box)
{
    cairo_matrix_t m;
    cairo_get_matrix(cr, &m);
    if (m.xy != 0 || m.yx != 0) return;
    *l = snap_device_x(cr, *l);
    *r = snap_device_x(cr, *r);
    *t = snap_device_y(cr, *t);
    *b = snap_device_y(cr, *b);
    *il = snap_inner_edge(cr, *l, *il, box->border.left, 1, FALSE);
    *ir = snap_inner_edge(cr, *r, *ir, box->border.right, -1, FALSE);
    *it = snap_inner_edge(cr, *t, *it, box->border.top, 1, TRUE);
    *ib = snap_inner_edge(cr, *b, *ib, box->border.bottom, -1, TRUE);
    if (*ir < *il) *ir = *il;
    if (*ib < *it) *ib = *it;
}

static gboolean
paint_rounded_mixed_border(cairo_t *cr, const ns_box *b, const ns_style *s,
                           double x, double y, double w, double h,
                           corner_radii radii)
{
    const double bw[4] = { b->border.top, b->border.right,
                           b->border.bottom, b->border.left };
    const int style_props[4] = { NS_CSS_BORDER_TOP_STYLE, NS_CSS_BORDER_RIGHT_STYLE,
                                 NS_CSS_BORDER_BOTTOM_STYLE, NS_CSS_BORDER_LEFT_STYLE };
    const int color_props[4] = { NS_CSS_BORDER_TOP_COLOR, NS_CSS_BORDER_RIGHT_COLOR,
                                 NS_CSS_BORDER_BOTTOM_COLOR, NS_CSS_BORDER_LEFT_COLOR };
    for (int i = 0; i < 4; i++)
        if (bw[i] > 0 && !border_style_is_solid(s->values[style_props[i]]))
            return FALSE;
    corner_radii outer = corner_radii_fit(radii, w, h);
    double ix = x + bw[3], iy = y + bw[0];
    double iw = w - bw[1] - bw[3], ih = h - bw[0] - bw[2];
    corner_radii inner = corner_radii_inset(outer, bw[0], bw[1], bw[2], bw[3]);
    const double oc[4][2] = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
    const double ic[4][2] = { { ix, iy }, { ix + iw, iy }, { ix + iw, iy + ih }, { ix, iy + ih } };
    for (int i = 0; i < 4; i++) {
        if (bw[i] <= 0) continue;
        rgba c = rgba_of(s->values[color_props[i]] ? s->values[color_props[i]]
                                                   : s->values[NS_CSS_COLOR],
                         0, 0, 0, 1);
        if (c.a <= 0) continue;
        cairo_save(cr);
        cairo_new_path(cr);
        int a = i, bidx = (i + 1) % 4;
        double apex_x, apex_y;
        border_wedge_apex(oc[a], ic[a], oc[bidx], ic[bidx],
                          x + w / 2.0, y + h / 2.0, &apex_x, &apex_y);
        cairo_move_to(cr, oc[a][0], oc[a][1]);
        cairo_line_to(cr, oc[bidx][0], oc[bidx][1]);
        cairo_line_to(cr, apex_x, apex_y);
        cairo_close_path(cr);
        cairo_clip(cr);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        rounded_rect_path(cr, x, y, w, h, outer);
        if (iw > 0 && ih > 0)
            rounded_rect_path(cr, ix, iy, iw, ih, inner);
        set_source_rgba(cr, c);
        cairo_fill(cr);
        cairo_restore(cr);
    }
    return TRUE;
}

static gboolean
style_uniform_solid_border(const ns_style *s, double *out_w, rgba *out_color)
{
    if (!s) return FALSE;
    const ns_css_prop widths[4] = {
        NS_CSS_BORDER_TOP_WIDTH,
        NS_CSS_BORDER_RIGHT_WIDTH,
        NS_CSS_BORDER_BOTTOM_WIDTH,
        NS_CSS_BORDER_LEFT_WIDTH,
    };
    const ns_css_prop styles[4] = {
        NS_CSS_BORDER_TOP_STYLE,
        NS_CSS_BORDER_RIGHT_STYLE,
        NS_CSS_BORDER_BOTTOM_STYLE,
        NS_CSS_BORDER_LEFT_STYLE,
    };
    const ns_css_prop colors[4] = {
        NS_CSS_BORDER_TOP_COLOR,
        NS_CSS_BORDER_RIGHT_COLOR,
        NS_CSS_BORDER_BOTTOM_COLOR,
        NS_CSS_BORDER_LEFT_COLOR,
    };
    double bw = 0;
    rgba bc = {0};
    for (int i = 0; i < 4; i++) {
        if (!style_side_visible(s, widths[i], styles[i])) return FALSE;
        const ns_css_value *st = s->values[styles[i]];
        if (st && st->kind == NS_CSS_V_KEYWORD && st->u.keyword &&
            strcmp(st->u.keyword, "solid") != 0)
            return FALSE;
        double w = length_or(s->values[widths[i]], 0);
        rgba c = rgba_of(s->values[colors[i]] ? s->values[colors[i]]
                                              : s->values[NS_CSS_COLOR], 0, 0, 0, 1);
        if (i == 0) {
            bw = w;
            bc = c;
        } else {
            if (fabs(w - bw) > 0.01) return FALSE;
            if (fabs(c.r - bc.r) > 0.001 ||
                fabs(c.g - bc.g) > 0.001 ||
                fabs(c.b - bc.b) > 0.001 ||
                fabs(c.a - bc.a) > 0.001)
                return FALSE;
        }
    }
    if (out_w) *out_w = bw;
    if (out_color) *out_color = bc;
    return bw > 0;
}

static double
inline_control_css_min_width(const ns_inline_attr *r, const ns_box *b)
{
    if (!r || !r->style) return 0;
    double fs = length_or(r->style->values[NS_CSS_FONT_SIZE], 16);
    double mn = ns_control_dim_px(r->style->values[NS_CSS_MIN_WIDTH], fs,
                                  b ? b->content_width : 0);
    if (mn > 0) mn += ns_control_css_extra_w(r->dom, r->style);
    return mn;
}

static void
paint_inline_box_shadow(cairo_t *cr, const ns_style *s, double x, double y,
                        double w, double h, corner_radii radii)
{
    if (!s || !s->values[NS_CSS_BOX_SHADOW] ||
        s->values[NS_CSS_BOX_SHADOW]->kind != NS_CSS_V_SHADOW)
        return;
    const ns_css_shadow_list *sl = &s->values[NS_CSS_BOX_SHADOW]->u.shadow;
    for (int si = sl->n - 1; si >= 0; si--) {
        const ns_css_shadow *sh = &sl->s[si];
        if (sh->inset) continue;
        double sx = x + sh->x - sh->spread;
        double sy = y + sh->y - sh->spread;
        double sw = w + sh->spread * 2;
        double sh_h = h + sh->spread * 2;
        int blur = (int)sh->blur;
        if (blur > 0) {
            int steps = blur > 12 ? 12 : blur;
            if (steps < 1) steps = 1;
            for (int i = steps; i >= 1; i--) {
                double t = (double)i / steps;
                double pad = sh->blur * t;
                double alpha = (sh->a / 255.0) * (1.0 - t) * 0.7;
                cairo_set_source_rgba(cr,
                    sh->r / 255.0, sh->g / 255.0, sh->b / 255.0, alpha);
                fill_outer_shadow(cr, sx - pad, sy - pad,
                                  sw + pad * 2, sh_h + pad * 2,
                                  x, y, w, h, radii);
            }
        } else {
            cairo_set_source_rgba(cr,
                sh->r / 255.0, sh->g / 255.0, sh->b / 255.0,
                sh->a / 255.0);
            fill_outer_shadow(cr, sx, sy, sw, sh_h, x, y, w, h, radii);
        }
    }
}

static gboolean
style_pixelated(const ns_style *s)
{
    const ns_css_value *ir = s ? s->values[NS_CSS_IMAGE_RENDERING] : NULL;
    return ir && ir->kind == NS_CSS_V_KEYWORD && ir->u.keyword &&
           (strcmp(ir->u.keyword, "pixelated") == 0 ||
            strcmp(ir->u.keyword, "crisp-edges") == 0);
}

static void
paint_bg_image_core(cairo_t *cr, ns_image *img,
                    const ns_css_value *rep_v, const ns_css_value *sz,
                    const ns_css_value *px, const ns_css_value *py,
                    gboolean pixelated,
                    double x, double y, double w, double h,
                    double clip_x, double clip_y, double clip_w, double clip_h,
                    corner_radii radii)
{
    if (!img || !img->loaded || !img->texture) return;
    int iw = ns_texture_get_width(img->texture);
    int ih = ns_texture_get_height(img->texture);
    if (iw <= 0 || ih <= 0) return;
    gboolean tile_x = TRUE, tile_y = TRUE;
    const char *repeat = (rep_v && rep_v->kind == NS_CSS_V_KEYWORD)
                         ? rep_v->u.keyword : NULL;
    if (repeat) {
        if (strcmp(repeat, "no-repeat") == 0) { tile_x = tile_y = FALSE; }
        else if (strcmp(repeat, "repeat-x") == 0) { tile_y = FALSE; }
        else if (strcmp(repeat, "repeat-y") == 0) { tile_x = FALSE; }
    }
    double draw_w = iw, draw_h = ih;
    if (sz && sz->kind == NS_CSS_V_KEYWORD && sz->u.keyword) {
        if (strcmp(sz->u.keyword, "cover") == 0) {
            double sx = w / (double)iw;
            double sy = h / (double)ih;
            double sc = sx > sy ? sx : sy;
            draw_w = iw * sc;
            draw_h = ih * sc;
        } else if (strcmp(sz->u.keyword, "contain") == 0) {
            double sx = w / (double)iw;
            double sy = h / (double)ih;
            double sc = sx < sy ? sx : sy;
            draw_w = iw * sc;
            draw_h = ih * sc;
        }
    } else if (sz && sz->kind == NS_CSS_V_LENGTH) {
        draw_w = bg_size_px(sz->u.length.v, sz->u.length.unit, w);
        draw_h = draw_w * ((double)ih / (double)iw);
    } else if (sz && sz->kind == NS_CSS_V_SIZE) {
        gboolean wa = sz->u.size.w_auto;
        gboolean ha = sz->u.size.h_auto;
        if (!wa) draw_w = bg_size_px(sz->u.size.w, sz->u.size.w_unit, w);
        if (!ha) draw_h = bg_size_px(sz->u.size.h, sz->u.size.h_unit, h);
        if (wa && !ha) draw_w = draw_h * ((double)iw / (double)ih);
        else if (!wa && ha) draw_h = draw_w * ((double)ih / (double)iw);
        else if (wa && ha) { draw_w = iw; draw_h = ih; }
    }
    if (draw_w < 1) draw_w = 1;
    if (draw_h < 1) draw_h = 1;
    double off_x = 0, off_y = 0;
    if (px && px->kind == NS_CSS_V_LENGTH) {
        if (px->u.length.unit == NS_CSS_UNIT_PERCENT)
            off_x = (w - draw_w) * (px->u.length.v / 100.0);
        else
            off_x = px->u.length.v;
    } else if (px && px->kind == NS_CSS_V_CALC) {
        off_x = (w - draw_w) * (px->u.calc.pct / 100.0) + px->u.calc.px;
    }
    if (py && py->kind == NS_CSS_V_LENGTH) {
        if (py->u.length.unit == NS_CSS_UNIT_PERCENT)
            off_y = (h - draw_h) * (py->u.length.v / 100.0);
        else
            off_y = py->u.length.v;
    } else if (py && py->kind == NS_CSS_V_CALC) {
        off_y = (h - draw_h) * (py->u.calc.pct / 100.0) + py->u.calc.px;
    }
    int pw = 0, ph = 0;
    cairo_surface_t *surf =
        img->svg_source && svg_raster_size(cr, draw_w, draw_h, iw, ih, &pw, &ph)
        ? svg_raster_cached(img->texture, img->svg_source, draw_w, draw_h,
                            pw, ph, NULL)
        : NULL;
    if (surf) {
        iw = cairo_image_surface_get_width(surf);
        ih = cairo_image_surface_get_height(surf);
    } else {
        surf = texture_surface_cached(img->texture, NULL);
    }
    if (!surf) return;
    cairo_save(cr);
    rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, radii);
    cairo_clip(cr);
    cairo_pattern_t *pat = cairo_pattern_create_for_surface(surf);
    cairo_pattern_set_extend(pat,
        (tile_x || tile_y) ? CAIRO_EXTEND_REPEAT : CAIRO_EXTEND_NONE);
    if (pixelated)
        cairo_pattern_set_filter(pat, CAIRO_FILTER_NEAREST);
    cairo_matrix_t m;
    cairo_matrix_init_identity(&m);
    cairo_matrix_scale(&m, (double)iw / draw_w, (double)ih / draw_h);
    cairo_matrix_translate(&m, -(x + off_x), -(y + off_y));
    cairo_pattern_set_matrix(pat, &m);
    cairo_set_source(cr, pat);
    if (tile_x && tile_y) {
        cairo_paint(cr);
    } else if (!tile_x && !tile_y) {
        cairo_rectangle(cr, x + off_x, y + off_y, draw_w, draw_h);
        cairo_fill(cr);
    } else if (tile_x) {
        cairo_rectangle(cr, x, y + off_y, w, draw_h);
        cairo_fill(cr);
    } else {
        cairo_rectangle(cr, x + off_x, y, draw_w, h);
        cairo_fill(cr);
    }
    cairo_pattern_destroy(pat);
    cairo_restore(cr);
}

static void
gradient_stop_color(const ns_css_gradient_stop *st, double c[4])
{
    c[0] = st->r / 255.0;
    c[1] = st->g / 255.0;
    c[2] = st->b / 255.0;
    c[3] = st->a / 255.0;
}

static void
gradient_lerp_premultiplied(const double a[4], const double b[4], double t,
                            double out[4])
{
    double al = a[3] + (b[3] - a[3]) * t;
    for (int ch = 0; ch < 3; ch++) {
        double pm = a[ch] * a[3] + (b[ch] * b[3] - a[ch] * a[3]) * t;
        out[ch] = al > 0 ? pm / al : a[ch] + (b[ch] - a[ch]) * t;
    }
    out[3] = al;
}

static void
conic_color_at(const ns_css_gradient *gr, double frac,
               double *r, double *g, double *b, double *a)
{
    double pos = frac + gr->from_deg / 360.0;
    pos -= floor(pos);
    if (!(pos >= 0.0 && pos < 1.0)) pos = 0.0;
    if (gr->repeating) {
        double cper = gr->stops[gr->n_stops - 1].pos;
        if (cper > 0) pos = fmod(pos, cper);
    }
    int lo = 0;
    while (lo + 1 < gr->n_stops && gr->stops[lo + 1].pos < pos) lo++;
    int hi = lo + 1;
    if (hi >= gr->n_stops) hi = gr->n_stops - 1;
    double t = 0.0;
    if (gr->stops[hi].pos > gr->stops[lo].pos)
        t = (pos - gr->stops[lo].pos) /
            (gr->stops[hi].pos - gr->stops[lo].pos);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double c0[4], c1[4], c[4];
    gradient_stop_color(&gr->stops[lo], c0);
    gradient_stop_color(&gr->stops[hi], c1);
    gradient_lerp_premultiplied(c0, c1, t, c);
    *r = c[0];
    *g = c[1];
    *b = c[2];
    *a = c[3];
}

static gboolean
gradient_segment_blends(double o0, double o1, const double a[4],
                        const double b[4])
{
    return o1 > o0 && a[3] != b[3] && a[3] > 0 && b[3] > 0 &&
           (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]);
}

static void
gradient_add_premultiplied_stop(cairo_pattern_t *pat, double offset,
                                const double a[4], const double b[4],
                                double t)
{
    double c[4];
    gradient_lerp_premultiplied(a, b, t, c);
    cairo_pattern_add_color_stop_rgba(pat, offset, c[0], c[1], c[2], c[3]);
}

static void
gradient_add_segment(cairo_pattern_t *pat, double o0, double o1,
                     const double a[4], const double b[4])
{
    int steps = gradient_segment_blends(o0, o1, a, b) ? 8 : 1;
    for (int k = 0; k <= steps; k++) {
        double t = (double)k / steps;
        gradient_add_premultiplied_stop(pat, o0 + (o1 - o0) * t, a, b, t);
    }
}

static void
gradient_add_stops(cairo_pattern_t *pat, const ns_css_gradient *gr,
                   const double *frac, double period)
{
    double a[4], b[4];
    if (gr->n_stops == 1) {
        gradient_stop_color(&gr->stops[0], a);
        cairo_pattern_add_color_stop_rgba(pat, frac[0] / period,
                                          a[0], a[1], a[2], a[3]);
        return;
    }
    for (int i = 0; i + 1 < gr->n_stops; i++) {
        gradient_stop_color(&gr->stops[i], a);
        gradient_stop_color(&gr->stops[i + 1], b);
        if (a[3] <= 0) memcpy(a, b, 3 * sizeof(double));
        if (b[3] <= 0) memcpy(b, a, 3 * sizeof(double));
        gradient_add_segment(pat, frac[i] / period, frac[i + 1] / period, a, b);
    }
}

static void
gradient_stop_fractions(const ns_css_gradient *gr, double line_len,
                        double *frac)
{
    for (int i = 0; i < gr->n_stops; i++) {
        frac[i] = gr->stops[i].pos + gr->stops[i].pos_px / line_len;
        if (i > 0 && frac[i] < frac[i - 1]) frac[i] = frac[i - 1];
    }
}

static void
paint_bg_gradient_core(cairo_t *cr, const ns_css_gradient *gr,
                       double border_x, double border_y,
                       double border_w, double border_h,
                       double clip_x, double clip_y,
                       double clip_w, double clip_h,
                       corner_radii radii)
{
    double cx = border_x + gr->center_x * border_w + gr->center_x_px;
    double cy = border_y + gr->center_y * border_h + gr->center_y_px;
    if (gr->conic && gr->n_stops > 0) {
        enum { CONIC_BASE = 24, CONIC_CAP = 96 };
        double bnd[CONIC_CAP];
        int nb = 0;
        for (int k = 0; k <= CONIC_BASE; k++)
            bnd[nb++] = k / (double)CONIC_BASE;
        double off = gr->from_deg / 360.0;
        for (int s = 0; s < gr->n_stops && nb < CONIC_CAP; s++) {
            double rel_pos = gr->stops[s].pos - off;
            rel_pos -= floor(rel_pos);
            bnd[nb++] = rel_pos;
            if (nb < CONIC_CAP) bnd[nb++] = rel_pos;
        }
        for (int i = 1; i < nb; i++) {
            double key = bnd[i];
            int j = i - 1;
            while (j >= 0 && bnd[j] > key) { bnd[j + 1] = bnd[j]; j--; }
            bnd[j + 1] = key;
        }
        double r_outer = sqrt(border_w * border_w + border_h * border_h) /
                         cos(G_PI / CONIC_BASE);
        cairo_save(cr);
        rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, radii);
        cairo_clip(cr);
        cairo_pattern_t *mesh = cairo_pattern_create_mesh();
        for (int i = 0; i + 1 < nb; i++) {
            double f1 = bnd[i], f2 = bnd[i + 1];
            double span = f2 - f1;
            if (span < 1e-6) continue;
            double eps = span * 1e-3;
            double a1 = f1 * 2 * G_PI - G_PI / 2;
            double a2 = f2 * 2 * G_PI - G_PI / 2;
            double r1, g1, b1, al1, r2, g2, b2, al2;
            conic_color_at(gr, f1 + eps, &r1, &g1, &b1, &al1);
            conic_color_at(gr, f2 - eps, &r2, &g2, &b2, &al2);
            cairo_mesh_pattern_begin_patch(mesh);
            cairo_mesh_pattern_move_to(mesh, cx, cy);
            cairo_mesh_pattern_line_to(mesh, cx + r_outer * cos(a1),
                                       cy + r_outer * sin(a1));
            cairo_mesh_pattern_line_to(mesh, cx + r_outer * cos(a2),
                                       cy + r_outer * sin(a2));
            cairo_mesh_pattern_line_to(mesh, cx, cy);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 0, r1, g1, b1, al1);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, r1, g1, b1, al1);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, r2, g2, b2, al2);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, r2, g2, b2, al2);
            cairo_mesh_pattern_end_patch(mesh);
        }
        cairo_set_source(cr, mesh);
        cairo_paint(cr);
        cairo_pattern_destroy(mesh);
        cairo_restore(cr);
    } else {
        cairo_pattern_t *pat;
        double dxh = 0, dyh = 0, r_outer = 1, r_outer_y = 1, line_len;
        if (gr->radial) {
            ns_css_gradient_radii(gr, border_w, border_h, cx - border_x,
                                  cy - border_y, &r_outer, &r_outer_y);
            line_len = r_outer;
        } else {
            double rad = ns_css_gradient_angle(gr, border_w, border_h) *
                         G_PI / 180.0;
            double dx = sin(rad), dy = -cos(rad);
            double half = (fabs(dx) * border_w + fabs(dy) * border_h) / 2.0;
            dxh = dx * half;
            dyh = dy * half;
            line_len = 2.0 * half;
        }
        if (line_len <= 0) line_len = 1;
        double frac[NS_CSS_GRADIENT_STOPS_MAX];
        gradient_stop_fractions(gr, line_len, frac);
        double period = (gr->repeating && gr->n_stops > 0)
            ? frac[gr->n_stops - 1] : 1.0;
        if (period <= 0) period = 1.0;
        if (gr->radial) {
            pat = cairo_pattern_create_radial(0, 0, 0, 0, 0, period);
            cairo_matrix_t m;
            cairo_matrix_init_scale(&m, 1.0 / r_outer, 1.0 / r_outer_y);
            cairo_matrix_translate(&m, -cx, -cy);
            cairo_pattern_set_matrix(pat, &m);
        } else {
            double x0 = cx - dxh, y0 = cy - dyh;
            double x1 = cx + dxh, y1 = cy + dyh;
            pat = cairo_pattern_create_linear(
                x0, y0,
                x0 + (x1 - x0) * period, y0 + (y1 - y0) * period);
        }
        gradient_add_stops(pat, gr, frac, period);
        if (gr->repeating)
            cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
        cairo_save(cr);
        rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, radii);
        cairo_clip(cr);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
        cairo_restore(cr);
    }
}

static gboolean
bg_repeat_word_tiles(const char *word, size_t len)
{
    return len != strlen("no-repeat") || strncmp(word, "no-repeat", len) != 0;
}

static void
bg_repeat_axes(const ns_css_value *rep_v, gboolean *tile_x, gboolean *tile_y)
{
    *tile_x = *tile_y = TRUE;
    const char *k = rep_v && rep_v->kind == NS_CSS_V_KEYWORD
                    ? rep_v->u.keyword : NULL;
    if (!k) return;
    if (strcmp(k, "repeat-x") == 0) { *tile_y = FALSE; return; }
    if (strcmp(k, "repeat-y") == 0) { *tile_x = FALSE; return; }
    const char *space = strchr(k, ' ');
    *tile_x = bg_repeat_word_tiles(k, space ? (size_t)(space - k) : strlen(k));
    *tile_y = space ? bg_repeat_word_tiles(space + 1, strlen(space + 1))
                    : *tile_x;
}

static double
bg_offset_px(const ns_css_value *p, double avail)
{
    if (p && p->kind == NS_CSS_V_LENGTH)
        return p->u.length.unit == NS_CSS_UNIT_PERCENT
               ? avail * (p->u.length.v / 100.0) : p->u.length.v;
    if (p && p->kind == NS_CSS_V_CALC)
        return avail * (p->u.calc.pct / 100.0) + p->u.calc.px;
    return 0;
}

static void
bg_gradient_tile_size(const ns_css_value *sz, double w, double h,
                      double *tw, double *th)
{
    *tw = w;
    *th = h;
    if (sz && sz->kind == NS_CSS_V_LENGTH) {
        *tw = bg_size_px(sz->u.length.v, sz->u.length.unit, w);
    } else if (sz && sz->kind == NS_CSS_V_SIZE) {
        if (!sz->u.size.w_auto)
            *tw = bg_size_px(sz->u.size.w, sz->u.size.w_unit, w);
        if (!sz->u.size.h_auto)
            *th = bg_size_px(sz->u.size.h, sz->u.size.h_unit, h);
    }
}

static cairo_pattern_t *
bg_gradient_tile_pattern(cairo_t *cr, const ns_css_gradient *gr,
                         double tx, double ty, double tw, double th)
{
    double dx = 1, dy = 1;
    cairo_user_to_device_distance(cr, &dx, &dy);
    int pw = MAX((int)ceil(MIN(tw * MAX(fabs(dx), 1.0), 4096.0)), 1);
    int ph = MAX((int)ceil(MIN(th * MAX(fabs(dy), 1.0), 4096.0)), 1);
    cairo_surface_t *tile = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       pw, ph);
    if (cairo_surface_status(tile) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(tile);
        return NULL;
    }
    cairo_t *gc = cairo_create(tile);
    cairo_scale(gc, pw / tw, ph / th);
    corner_radii square = {0};
    paint_bg_gradient_core(gc, gr, 0, 0, tw, th, 0, 0, tw, th, square);
    cairo_destroy(gc);
    cairo_pattern_t *pat = cairo_pattern_create_for_surface(tile);
    cairo_surface_destroy(tile);
    cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
    cairo_matrix_t m;
    cairo_matrix_init_scale(&m, pw / tw, ph / th);
    cairo_matrix_translate(&m, -tx, -ty);
    cairo_pattern_set_matrix(pat, &m);
    return pat;
}

static void
paint_bg_gradient_layer(cairo_t *cr, const ns_css_gradient *gr,
                        const ns_css_value *rep_v, const ns_css_value *sz,
                        const ns_css_value *px, const ns_css_value *py,
                        double x, double y, double w, double h,
                        double clip_x, double clip_y,
                        double clip_w, double clip_h,
                        corner_radii radii)
{
    double tw, th;
    bg_gradient_tile_size(sz, w, h, &tw, &th);
    if (tw <= 0 || th <= 0 || clip_w <= 0 || clip_h <= 0) return;
    double tx = x + bg_offset_px(px, w - tw);
    double ty = y + bg_offset_px(py, h - th);
    gboolean tile_x, tile_y;
    bg_repeat_axes(rep_v, &tile_x, &tile_y);
    gboolean covers_x = tx <= clip_x && tx + tw >= clip_x + clip_w;
    gboolean covers_y = ty <= clip_y && ty + th >= clip_y + clip_h;
    cairo_save(cr);
    rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, radii);
    cairo_clip(cr);
    if ((covers_x || !tile_x) && (covers_y || !tile_y)) {
        corner_radii square = {0};
        paint_bg_gradient_core(cr, gr, tx, ty, tw, th, tx, ty, tw, th, square);
    } else {
        cairo_pattern_t *pat = bg_gradient_tile_pattern(cr, gr, tx, ty, tw, th);
        if (pat) {
            cairo_rectangle(cr, tile_x ? clip_x : tx, tile_y ? clip_y : ty,
                            tile_x ? clip_w : tw, tile_y ? clip_h : th);
            cairo_set_source(cr, pat);
            cairo_fill(cr);
            cairo_pattern_destroy(pat);
        }
    }
    cairo_restore(cr);
}

static void
paint_inline_background_image(cairo_t *cr, const ns_inline_attr *r,
                              const ns_style *s, double x, double y,
                              double w, double h, corner_radii radii)
{
    const ns_css_value *head = s ? s->values[NS_CSS_BACKGROUND_IMAGE] : NULL;
    for (int li = ns_css_value_layer_count(head) - 1; li >= 0; li--) {
        const ns_css_value *lv = ns_css_value_layer(head, li);
        if (lv->kind != NS_CSS_V_GRADIENT) continue;
        paint_bg_gradient_layer(cr, &lv->u.gradient,
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_REPEAT], li),
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_SIZE], li),
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_POSITION_X], li),
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_POSITION_Y], li),
            x, y, w, h, x, y, w, h, radii);
    }
    ns_image *img = r && r->bg_image ? r->bg_image : NULL;
    if (!img) return;
    paint_bg_image_core(cr, img,
        s ? s->values[NS_CSS_BACKGROUND_REPEAT] : NULL,
        s ? s->values[NS_CSS_BACKGROUND_SIZE] : NULL,
        s ? s->values[NS_CSS_BACKGROUND_POSITION_X] : NULL,
        s ? s->values[NS_CSS_BACKGROUND_POSITION_Y] : NULL,
        style_pixelated(s),
        x, y, w, h, x, y, w, h, radii);
}

static void
paint_inline_css_chrome(cairo_t *cr, const ns_inline_attr *r, double x, double y,
                        double w, double h)
{
    const ns_style *s = r ? r->style : NULL;
    if (!style_has_inline_box_paint(s) || w <= 0 || h <= 0) return;
    corner_radii radii = style_border_radii(s, w, h);
    cairo_save(cr);
    paint_inline_box_shadow(cr, s, x, y, w, h, radii);
    rgba bg = rgba_of(s->values[NS_CSS_BACKGROUND_COLOR], 0, 0, 0, 0);
    if (bg.a > 0) {
        set_source_rgba(cr, bg);
        rounded_rect_path(cr, x, y, w, h, radii);
        cairo_fill(cr);
    }
    paint_inline_background_image(cr, r, s, x, y, w, h, radii);
    double uniform_bw = 0;
    rgba uniform_color = {0};
    if (!corner_radii_zero(radii) &&
        style_uniform_solid_border(s, &uniform_bw, &uniform_color)) {
        set_source_rgba(cr, uniform_color);
        cairo_set_line_width(cr, uniform_bw);
        rounded_rect_path(cr, x + uniform_bw / 2.0, y + uniform_bw / 2.0,
                          w - uniform_bw, h - uniform_bw, radii);
        cairo_stroke(cr);
        cairo_restore(cr);
        return;
    }
    const struct {
        ns_css_prop width;
        ns_css_prop style;
        ns_css_prop color;
        double x1, y1, x2, y2;
    } sides[4] = {
        { NS_CSS_BORDER_TOP_WIDTH, NS_CSS_BORDER_TOP_STYLE,
          NS_CSS_BORDER_TOP_COLOR, x, y, x + w, y },
        { NS_CSS_BORDER_RIGHT_WIDTH, NS_CSS_BORDER_RIGHT_STYLE,
          NS_CSS_BORDER_RIGHT_COLOR, x + w, y, x + w, y + h },
        { NS_CSS_BORDER_BOTTOM_WIDTH, NS_CSS_BORDER_BOTTOM_STYLE,
          NS_CSS_BORDER_BOTTOM_COLOR, x, y + h, x + w, y + h },
        { NS_CSS_BORDER_LEFT_WIDTH, NS_CSS_BORDER_LEFT_STYLE,
          NS_CSS_BORDER_LEFT_COLOR, x, y, x, y + h },
    };
    for (int i = 0; i < 4; i++) {
        double bw = length_or(s->values[sides[i].width], 0);
        if (bw <= 0 || !style_side_visible(s, sides[i].width, sides[i].style))
            continue;
        rgba c = rgba_of(s->values[sides[i].color] ? s->values[sides[i].color]
                                                   : s->values[NS_CSS_COLOR], 0, 0, 0, 1);
        set_source_rgba(cr, c);
        cairo_set_line_width(cr, bw);
        cairo_move_to(cr, sides[i].x1, sides[i].y1);
        cairo_line_to(cr, sides[i].x2, sides[i].y2);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

typedef struct border_image_run {
    double size;
    double start;
    double step;
    int    count;
} border_image_run;

static border_image_run
border_image_stretch(double dest)
{
    border_image_run r = { dest, 0, dest, 1 };
    return r;
}

static border_image_run
border_image_tiling(double dest, double natural, ns_border_image_tile tile)
{
    border_image_run r = border_image_stretch(dest);
    if (dest <= 0 || natural <= 0 || tile == NS_BORDER_IMAGE_STRETCH)
        return r;
    if (tile == NS_BORDER_IMAGE_ROUND) {
        int n = (int)floor(dest / natural + 0.5);
        if (n < 1) n = 1;
        r.size = dest / n;
        r.step = r.size;
        r.count = n;
        return r;
    }
    if (tile == NS_BORDER_IMAGE_SPACE) {
        int n = (int)floor(dest / natural);
        if (n < 1) { r.count = 0; return r; }
        double gap = (dest - n * natural) / (n + 1);
        r.size = natural;
        r.start = gap;
        r.step = natural + gap;
        r.count = n;
        return r;
    }
    int n = (int)ceil(dest / natural) + 1;
    r.size = natural;
    r.step = natural;
    r.count = n;
    r.start = (dest - n * natural) / 2.0;
    return r;
}

static void
paint_border_image_part(cairo_t *cr, cairo_surface_t *surf,
                        double sx, double sy, double sw, double sh,
                        double dx, double dy, double dw, double dh,
                        border_image_run rx, border_image_run ry)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (rx.count <= 0 || ry.count <= 0 || rx.size <= 0 || ry.size <= 0) return;
    cairo_save(cr);
    cairo_rectangle(cr, dx, dy, dw, dh);
    cairo_clip(cr);
    for (int iy = 0; iy < ry.count; iy++) {
        for (int ix = 0; ix < rx.count; ix++) {
            cairo_save(cr);
            cairo_translate(cr, dx + rx.start + ix * rx.step,
                                dy + ry.start + iy * ry.step);
            cairo_rectangle(cr, 0, 0, rx.size, ry.size);
            cairo_clip(cr);
            cairo_scale(cr, rx.size / sw, ry.size / sh);
            cairo_set_source_surface(cr, surf, -sx, -sy);
            cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }
    cairo_restore(cr);
}

static double
border_image_edge_px(double v, ns_css_unit unit, double border_px,
                     double pct_basis, double font_size)
{
    if (unit == NS_CSS_UNIT_NUMBER) return v * border_px;
    if (unit == NS_CSS_UNIT_EM) return v * font_size;
    return bg_size_px(v, unit, pct_basis);
}

static gboolean
paint_border_image(cairo_t *cr, const ns_box *b, const ns_style *s,
                   double border_x, double border_y,
                   double border_w, double border_h)
{
    const ns_css_value *src = ns_css_border_image_source(s);
    if (!src) return FALSE;

    ns_border_image bi;
    ns_css_border_image_params(s, &bi);
    double font_size = length_or(s->values[NS_CSS_FONT_SIZE], 16);
    const double side[4] = { b->border.top, b->border.right,
                             b->border.bottom, b->border.left };
    double outset[4];
    for (int i = 0; i < 4; i++)
        outset[i] = border_image_edge_px(bi.outset[i], bi.outset_unit[i],
                                         side[i], 0, font_size);

    double area_x = border_x - outset[3];
    double area_y = border_y - outset[0];
    double area_w = border_w + outset[1] + outset[3];
    double area_h = border_h + outset[0] + outset[2];
    if (area_w <= 0 || area_h <= 0) return FALSE;

    cairo_surface_t *owned = NULL;
    cairo_surface_t *surf = NULL;
    double iw = 0, ih = 0;
    if (src->kind == NS_CSS_V_URL) {
        ns_image *img = b->media ? b->media->border_image : NULL;
        if (!img || !img->loaded || !img->texture) return FALSE;
        iw = ns_texture_get_width(img->texture);
        ih = ns_texture_get_height(img->texture);
        if (iw <= 0 || ih <= 0) return FALSE;
        surf = texture_surface_cached(img->texture, NULL);
        if (!surf) return FALSE;
    } else {
        iw = ceil(area_w);
        ih = ceil(area_h);
        owned = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                           (int)iw, (int)ih);
        cairo_t *gc = cairo_create(owned);
        corner_radii square = {0};
        paint_bg_gradient_core(gc, &src->u.gradient, 0, 0, iw, ih,
                               0, 0, iw, ih, square);
        cairo_destroy(gc);
        surf = owned;
    }

    double slice[4];
    for (int i = 0; i < 4; i++) {
        double basis = (i % 2 == 0) ? ih : iw;
        slice[i] = bi.slice_percent[i] ? bi.slice[i] / 100.0 * basis
                                       : bi.slice[i];
        if (slice[i] < 0) slice[i] = 0;
    }
    if (slice[0] + slice[2] > ih) {
        double f = ih / (slice[0] + slice[2]);
        slice[0] *= f;
        slice[2] *= f;
    }
    if (slice[3] + slice[1] > iw) {
        double f = iw / (slice[3] + slice[1]);
        slice[3] *= f;
        slice[1] *= f;
    }

    double width[4];
    for (int i = 0; i < 4; i++) {
        double pct_basis = (i % 2 == 0) ? area_h : area_w;
        width[i] = bi.width_auto[i]
            ? slice[i]
            : border_image_edge_px(bi.width[i], bi.width_unit[i], side[i],
                                   pct_basis, font_size);
        if (width[i] < 0) width[i] = 0;
    }
    double shrink = 1.0;
    if (width[3] + width[1] > area_w)
        shrink = MIN(shrink, area_w / (width[3] + width[1]));
    if (width[0] + width[2] > area_h)
        shrink = MIN(shrink, area_h / (width[0] + width[2]));
    if (shrink < 1.0)
        for (int i = 0; i < 4; i++) width[i] *= shrink;

    double mid_sw = iw - slice[3] - slice[1];
    double mid_sh = ih - slice[0] - slice[2];
    double mid_dw = area_w - width[3] - width[1];
    double mid_dh = area_h - width[0] - width[2];
    double scale_y = slice[0] > 0 ? width[0] / slice[0]
                   : slice[2] > 0 ? width[2] / slice[2] : 1.0;
    double scale_x = slice[3] > 0 ? width[3] / slice[3]
                   : slice[1] > 0 ? width[1] / slice[1] : 1.0;

    paint_border_image_part(cr, surf, 0, 0, slice[3], slice[0],
        area_x, area_y, width[3], width[0],
        border_image_stretch(width[3]), border_image_stretch(width[0]));
    paint_border_image_part(cr, surf, iw - slice[1], 0, slice[1], slice[0],
        area_x + area_w - width[1], area_y, width[1], width[0],
        border_image_stretch(width[1]), border_image_stretch(width[0]));
    paint_border_image_part(cr, surf, 0, ih - slice[2], slice[3], slice[2],
        area_x, area_y + area_h - width[2], width[3], width[2],
        border_image_stretch(width[3]), border_image_stretch(width[2]));
    paint_border_image_part(cr, surf, iw - slice[1], ih - slice[2],
        slice[1], slice[2],
        area_x + area_w - width[1], area_y + area_h - width[2],
        width[1], width[2],
        border_image_stretch(width[1]), border_image_stretch(width[2]));

    paint_border_image_part(cr, surf, slice[3], 0, mid_sw, slice[0],
        area_x + width[3], area_y, mid_dw, width[0],
        border_image_tiling(mid_dw, mid_sw * scale_y, bi.tile[0]),
        border_image_stretch(width[0]));
    paint_border_image_part(cr, surf, slice[3], ih - slice[2], mid_sw, slice[2],
        area_x + width[3], area_y + area_h - width[2], mid_dw, width[2],
        border_image_tiling(mid_dw, mid_sw * scale_y, bi.tile[0]),
        border_image_stretch(width[2]));
    paint_border_image_part(cr, surf, 0, slice[0], slice[3], mid_sh,
        area_x, area_y + width[0], width[3], mid_dh,
        border_image_stretch(width[3]),
        border_image_tiling(mid_dh, mid_sh * scale_x, bi.tile[1]));
    paint_border_image_part(cr, surf, iw - slice[1], slice[0], slice[1], mid_sh,
        area_x + area_w - width[1], area_y + width[0], width[1], mid_dh,
        border_image_stretch(width[1]),
        border_image_tiling(mid_dh, mid_sh * scale_x, bi.tile[1]));

    if (bi.fill)
        paint_border_image_part(cr, surf, slice[3], slice[0], mid_sw, mid_sh,
            area_x + width[3], area_y + width[0], mid_dw, mid_dh,
            border_image_tiling(mid_dw, mid_sw * scale_x, bi.tile[0]),
            border_image_tiling(mid_dh, mid_sh * scale_y, bi.tile[1]));

    if (owned) cairo_surface_destroy(owned);
    return TRUE;
}

static gboolean g_paint_have_viewport;
static double g_paint_vp_x0, g_paint_vp_y0;

static const char *
bg_layer_keyword(const ns_style *s, ns_css_prop prop, int li)
{
    const ns_css_value *v = s ? ns_css_value_layer(s->values[prop], li) : NULL;
    return v && v->kind == NS_CSS_V_KEYWORD ? v->u.keyword : NULL;
}

static void paint_box_text_path(cairo_t *cr, const ns_box *b, int depth);

static gboolean
style_background_clips_to_text(const ns_style *s)
{
    const ns_css_value *head = s ? s->values[NS_CSS_BACKGROUND_IMAGE] : NULL;
    int layers = ns_css_value_layer_count(head);
    const char *clip = bg_layer_keyword(s, NS_CSS_BACKGROUND_CLIP,
                                        layers > 0 ? layers - 1 : 0);
    return clip && strcmp(clip, "text") == 0;
}

static void
paint_background_clipped_to_text(cairo_t *cr, const ns_box *b,
                                 cairo_pattern_t *background)
{
    cairo_save(cr);
    cairo_new_path(cr);
    paint_box_text_path(cr, b, 0);
    cairo_clip(cr);
    cairo_set_source(cr, background);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_pattern_destroy(background);
}

static void
bg_layer_clip_area(const ns_box *b, int li,
                   double bx, double by, double bw, double bh,
                   double *x, double *y, double *w, double *h)
{
    const char *k = bg_layer_keyword(b->style, NS_CSS_BACKGROUND_CLIP, li);
    *x = bx; *y = by; *w = bw; *h = bh;
    if (k && strcmp(k, "padding-box") == 0) {
        *x += b->border.left; *y += b->border.top;
        *w -= b->border.left + b->border.right;
        *h -= b->border.top + b->border.bottom;
    } else if (k && strcmp(k, "content-box") == 0) {
        *x += b->border.left + b->padding.left;
        *y += b->border.top + b->padding.top;
        *w -= b->border.left + b->border.right + b->padding.left + b->padding.right;
        *h -= b->border.top + b->border.bottom + b->padding.top + b->padding.bottom;
    }
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

static void
bg_layer_origin_area(const ns_box *b, int li,
                     double bx, double by, double bw, double bh,
                     double *x, double *y, double *w, double *h)
{
    const char *att = bg_layer_keyword(b->style, NS_CSS_BACKGROUND_ATTACHMENT, li);
    if (att && strcmp(att, "fixed") == 0) {
        *x = g_paint_have_viewport ? g_paint_vp_x0 : 0;
        *y = g_paint_have_viewport ? g_paint_vp_y0 : 0;
        *w = MAX(ns_css_viewport_w(), 1);
        *h = MAX(ns_css_viewport_h(), 1);
        return;
    }
    const char *k = bg_layer_keyword(b->style, NS_CSS_BACKGROUND_ORIGIN, li);
    *x = bx + b->border.left;
    *y = by + b->border.top;
    *w = bw - b->border.left - b->border.right;
    *h = bh - b->border.top - b->border.bottom;
    if (k && strcmp(k, "border-box") == 0) {
        *x = bx; *y = by; *w = bw; *h = bh;
    } else if (k && strcmp(k, "content-box") == 0) {
        *x += b->padding.left;
        *y += b->padding.top;
        *w -= b->padding.left + b->padding.right;
        *h -= b->padding.top + b->padding.bottom;
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static gboolean
cell_hides_empty(const ns_box *b)
{
    if (b->kind != NS_BOX_TABLE_CELL || !b->style || !b->dom ||
        !ns_css_keyword_is(b->style->values[NS_CSS_EMPTY_CELLS], "hide") ||
        ns_css_keyword_is(b->style->values[NS_CSS_BORDER_COLLAPSE], "collapse"))
        return FALSE;
    for (const ns_node *c = b->dom->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT) return FALSE;
        if (c->kind != NS_NODE_TEXT || !c->text) continue;
        for (const char *p = c->text; *p; p++)
            if (!g_ascii_isspace(*p)) return FALSE;
    }
    return TRUE;
}

static void
paint_block(cairo_t *cr, const ns_box *b)
{
    if (cell_hides_empty(b)) return;
    double border_x = b->x + b->margin.left;
    double border_y = b->y + b->margin.top;
    double border_w = b->content_width + b->padding.left + b->padding.right +
                      b->border.left + b->border.right;
    double border_h = b->content_height + b->padding.top + b->padding.bottom +
                      b->border.top + b->border.bottom;
    double legend_inset = 0;
    double gap_x0 = 0, gap_x1 = 0, gap_y0 = 0, gap_y1 = 0;
    gboolean legend_gap = ns_box_fieldset_legend_gap(b, &legend_inset,
                                                     &gap_x0, &gap_x1,
                                                     &gap_y0, &gap_y1);
    if (legend_gap) {
        border_y += legend_inset;
        border_h -= legend_inset;
    }
    double captions_above = 0, captions_below = 0;
    ns_box_table_caption_extents(b, &captions_above, &captions_below);
    border_y += captions_above;
    border_h -= captions_above + captions_below;

    if (border_w <= 0 || border_h <= 0) return;

    const ns_style *s = b->style;
    corner_radii radii = box_border_radii(b);

    const ns_css_value *bg_head = s ? s->values[NS_CSS_BACKGROUND_IMAGE] : NULL;
    int n_bg_layers = ns_css_value_layer_count(bg_head);
    int last_bg_layer = n_bg_layers > 0 ? n_bg_layers - 1 : 0;
    double clip_x, clip_y, clip_w, clip_h;
    bg_layer_clip_area(b, last_bg_layer, border_x, border_y, border_w, border_h,
                       &clip_x, &clip_y, &clip_w, &clip_h);
    double pos_x, pos_y, pos_w, pos_h;
    bg_layer_origin_area(b, last_bg_layer, border_x, border_y, border_w, border_h,
                         &pos_x, &pos_y, &pos_w, &pos_h);

    if (s && s->values[NS_CSS_BOX_SHADOW] &&
        s->values[NS_CSS_BOX_SHADOW]->kind == NS_CSS_V_SHADOW) {
        const ns_css_shadow_list *sl = &s->values[NS_CSS_BOX_SHADOW]->u.shadow;
        for (int si = sl->n - 1; si >= 0; si--) {
            const ns_css_shadow *sh = &sl->s[si];
            if (sh->inset) continue;
            double sx = border_x + sh->x - sh->spread;
            double sy = border_y + sh->y - sh->spread;
            double sw = border_w + sh->spread * 2;
            double sh_h = border_h + sh->spread * 2;
            cairo_save(cr);
            if (sh->blur > 0) {
                paint_blurred_box_shadow(cr, sx, sy, sw, sh_h, radii, sh->blur,
                    sh->r / 255.0, sh->g / 255.0, sh->b / 255.0, sh->a / 255.0,
                    border_x, border_y, border_w, border_h, radii);
            } else {
                cairo_set_source_rgba(cr,
                    sh->r / 255.0, sh->g / 255.0, sh->b / 255.0,
                    sh->a / 255.0);
                fill_outer_shadow(cr, sx, sy, sw, sh_h,
                                  border_x, border_y, border_w, border_h,
                                  radii);
            }
            cairo_restore(cr);
        }
    }

    gboolean clip_to_text = style_background_clips_to_text(s);
    if (clip_to_text) cairo_push_group(cr);
    rgba bg = rgba_anim(b, NS_CSS_ANIM_TARGET_BG_COLOR,
                        s ? s->values[NS_CSS_BACKGROUND_COLOR] : NULL,
                        0, 0, 0, 0);
    if (bg.a > 0) {
        set_source_rgba(cr, bg);
        rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, radii);
        cairo_fill(cr);
    }

    gboolean bg_has_url = FALSE;
    for (const ns_css_value *l = bg_head; l; l = l->next_layer)
        if (l->kind == NS_CSS_V_URL) { bg_has_url = TRUE; break; }

    if (b->media && b->media->bg_image && !bg_has_url) {
        paint_bg_image_core(cr, b->media->bg_image,
            s ? s->values[NS_CSS_BACKGROUND_REPEAT] : NULL,
            s ? s->values[NS_CSS_BACKGROUND_SIZE] : NULL,
            s ? s->values[NS_CSS_BACKGROUND_POSITION_X] : NULL,
            s ? s->values[NS_CSS_BACKGROUND_POSITION_Y] : NULL,
            style_pixelated(s),
            pos_x, pos_y, pos_w, pos_h,
            clip_x, clip_y, clip_w, clip_h, radii);
    }

    for (int li = n_bg_layers - 1; li >= 0; li--) {
        const ns_css_value *lv = ns_css_value_layer(bg_head, li);
        double lclip_x, lclip_y, lclip_w, lclip_h;
        double lpos_x, lpos_y, lpos_w, lpos_h;
        bg_layer_clip_area(b, li, border_x, border_y, border_w, border_h,
                           &lclip_x, &lclip_y, &lclip_w, &lclip_h);
        bg_layer_origin_area(b, li, border_x, border_y, border_w, border_h,
                             &lpos_x, &lpos_y, &lpos_w, &lpos_h);
        if (lv->kind == NS_CSS_V_GRADIENT) {
            paint_bg_gradient_layer(cr, &lv->u.gradient,
                ns_css_value_layer(s->values[NS_CSS_BACKGROUND_REPEAT], li),
                ns_css_value_layer(s->values[NS_CSS_BACKGROUND_SIZE], li),
                ns_css_value_layer(s->values[NS_CSS_BACKGROUND_POSITION_X], li),
                ns_css_value_layer(s->values[NS_CSS_BACKGROUND_POSITION_Y], li),
                lpos_x, lpos_y, lpos_w, lpos_h,
                lclip_x, lclip_y, lclip_w, lclip_h, radii);
            continue;
        }
        if (lv->kind != NS_CSS_V_URL) continue;
        ns_image *img = NULL;
        if (b->media && b->media->bg_layer_images &&
            li < (int)b->media->bg_layer_images->len)
            img = g_ptr_array_index(b->media->bg_layer_images, li);
        else if (b->media)
            img = b->media->bg_image;
        if (!img) continue;
        paint_bg_image_core(cr, img,
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_REPEAT], li),
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_SIZE], li),
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_POSITION_X], li),
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_POSITION_Y], li),
            style_pixelated(s),
            lpos_x, lpos_y, lpos_w, lpos_h,
            lclip_x, lclip_y, lclip_w, lclip_h, radii);
    }
    if (clip_to_text) paint_background_clipped_to_text(cr, b, cairo_pop_group(cr));

    if (s && s->values[NS_CSS_BOX_SHADOW] &&
        s->values[NS_CSS_BOX_SHADOW]->kind == NS_CSS_V_SHADOW) {
        const ns_css_shadow_list *sl = &s->values[NS_CSS_BOX_SHADOW]->u.shadow;
        for (int si = sl->n - 1; si >= 0; si--) {
            const ns_css_shadow *sh = &sl->s[si];
            if (!sh->inset) continue;
            paint_inset_box_shadow(cr, sh,
                border_x + b->border.left, border_y + b->border.top,
                border_w - b->border.left - b->border.right,
                border_h - b->border.top - b->border.bottom,
                corner_radii_inset(radii, b->border.top, b->border.right,
                                   b->border.bottom, b->border.left));
        }
    }

    if (legend_gap) {
        double cx0, cy0, cx1, cy1;
        cairo_save(cr);
        cairo_clip_extents(cr, &cx0, &cy0, &cx1, &cy1);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_rectangle(cr, cx0, cy0, cx1 - cx0, cy1 - cy0);
        cairo_rectangle(cr, gap_x0, gap_y0, gap_x1 - gap_x0, gap_y1 - gap_y0);
        cairo_clip(cr);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    }
    if (s) {
        double uniform_bw = 0;
        rgba uniform_color = {0};
        gboolean drew_uniform = paint_border_image(cr, b, s, border_x, border_y,
                                                   border_w, border_h);
        if (!drew_uniform && box_paints_native_text_field(b)) {
            const ns_css_value *col = s->values[NS_CSS_BORDER_TOP_COLOR];
            set_source_rgba(cr, rgba_of(col ? col : s->values[NS_CSS_COLOR],
                                        0, 0, 0, 1));
            cairo_set_line_width(cr, 1.0);
            rounded_rect_path(cr, border_x + 0.5, border_y + 0.5,
                              border_w - 1, border_h - 1,
                              corner_radii_uniform(2));
            cairo_stroke(cr);
            drew_uniform = TRUE;
        }
        if (!drew_uniform && !corner_radii_zero(radii) &&
            style_uniform_solid_border(s, &uniform_bw, &uniform_color)) {
            set_source_rgba(cr, uniform_color);
            cairo_set_line_width(cr, uniform_bw);
            rounded_rect_path(cr,
                              border_x + uniform_bw / 2.0,
                              border_y + uniform_bw / 2.0,
                              border_w - uniform_bw,
                              border_h - uniform_bw,
                              radii);
            cairo_stroke(cr);
            drew_uniform = TRUE;
        }
        if (!drew_uniform && !corner_radii_zero(radii))
            drew_uniform = paint_rounded_mixed_border(cr, b, s, border_x, border_y,
                                                      border_w, border_h, radii);
        const struct {
            double w;
            const ns_css_value *col;
            const ns_css_value *style;
            double x1, y1, x2, y2;
        } sides[4] = {
            { b->border.top,
              s->values[NS_CSS_BORDER_TOP_COLOR],
              s->values[NS_CSS_BORDER_TOP_STYLE],
              border_x, border_y + b->border.top / 2.0,
              border_x + border_w, border_y + b->border.top / 2.0 },
            { b->border.right,
              s->values[NS_CSS_BORDER_RIGHT_COLOR],
              s->values[NS_CSS_BORDER_RIGHT_STYLE],
              border_x + border_w - b->border.right / 2.0, border_y,
              border_x + border_w - b->border.right / 2.0, border_y + border_h },
            { b->border.bottom,
              s->values[NS_CSS_BORDER_BOTTOM_COLOR],
              s->values[NS_CSS_BORDER_BOTTOM_STYLE],
              border_x, border_y + border_h - b->border.bottom / 2.0,
              border_x + border_w, border_y + border_h - b->border.bottom / 2.0 },
            { b->border.left,
              s->values[NS_CSS_BORDER_LEFT_COLOR],
              s->values[NS_CSS_BORDER_LEFT_STYLE],
              border_x + b->border.left / 2.0, border_y,
              border_x + b->border.left / 2.0, border_y + border_h },
        };
        double edge_l = border_x, edge_t = border_y;
        double edge_r = border_x + border_w, edge_b = border_y + border_h;
        double inner_x = edge_l + b->border.left;
        double inner_y = edge_t + b->border.top;
        double inner_r = MAX(inner_x, edge_r - b->border.right);
        double inner_b = MAX(inner_y, edge_b - b->border.bottom);
        snap_border_edges(cr, &edge_l, &edge_t, &edge_r, &edge_b,
                          &inner_x, &inner_y, &inner_r, &inner_b, b);
        const double outer_corner[4][2] = {
            { edge_l, edge_t }, { edge_r, edge_t },
            { edge_r, edge_b }, { edge_l, edge_b },
        };
        const double inner_corner[4][2] = {
            { inner_x, inner_y }, { inner_r, inner_y },
            { inner_r, inner_b }, { inner_x, inner_b },
        };
        for (int i = 0; !drew_uniform && i < 4; i++) {
            if (sides[i].w <= 0) continue;
            const ns_css_value *bs = sides[i].style;
            if (!bs || bs->kind != NS_CSS_V_KEYWORD || !bs->u.keyword ||
                strcmp(bs->u.keyword, "none") == 0 ||
                strcmp(bs->u.keyword, "hidden") == 0)
                continue;
            rgba c = rgba_of(sides[i].col ? sides[i].col
                                          : (s ? s->values[NS_CSS_COLOR] : NULL),
                             0, 0, 0, 1);
            if (c.a <= 0) continue;
            if (border_style_is_3d(bs->u.keyword) && !box_is_native_control(b)) {
                static const ns_css_prop side_colors[4] = {
                    NS_CSS_BORDER_TOP_COLOR, NS_CSS_BORDER_RIGHT_COLOR,
                    NS_CSS_BORDER_BOTTOM_COLOR, NS_CSS_BORDER_LEFT_COLOR,
                };
                gboolean current = !sides[i].col ||
                    ns_style_prop_from_currentcolor(s, side_colors[i]);
                paint_3d_border_side(cr, i, bs->u.keyword, c, current,
                                     outer_corner, inner_corner);
                continue;
            }
            if (strcmp(bs->u.keyword, "double") == 0) {
                paint_double_border_side(cr, i, sides[i].w, c,
                                         outer_corner, inner_corner);
                continue;
            }
            set_source_rgba(cr, c);
            if (strcmp(bs->u.keyword, "solid") == 0) {
                int next = (i + 1) % 4;
                cairo_new_path(cr);
                cairo_move_to(cr, outer_corner[i][0], outer_corner[i][1]);
                cairo_line_to(cr, outer_corner[next][0], outer_corner[next][1]);
                cairo_line_to(cr, inner_corner[next][0], inner_corner[next][1]);
                cairo_line_to(cr, inner_corner[i][0], inner_corner[i][1]);
                cairo_close_path(cr);
                cairo_fill(cr);
                continue;
            }
            cairo_set_line_width(cr, sides[i].w);
            cairo_save(cr);
            if (strcmp(bs->u.keyword, "dashed") == 0) {
                double dashes[] = { sides[i].w * 3, sides[i].w * 2 };
                cairo_set_dash(cr, dashes, 2, 0);
            } else if (strcmp(bs->u.keyword, "dotted") == 0) {
                double dashes[] = { sides[i].w, sides[i].w };
                cairo_set_dash(cr, dashes, 2, 0);
            }
            double x1 = sides[i].x1, y1 = sides[i].y1;
            double x2 = sides[i].x2, y2 = sides[i].y2;
            if (sides[i].w < 1.5) {
                if (x1 == x2) { x1 = floor(x1) + 0.5; x2 = x1; }
                if (y1 == y2) { y1 = floor(y1) + 0.5; y2 = y1; }
            }
            cairo_move_to(cr, x1, y1);
            cairo_line_to(cr, x2, y2);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
        if (legend_gap) {
            cairo_restore(cr);
            legend_gap = FALSE;
        }
        double ow = length_or(s->values[NS_CSS_OUTLINE_WIDTH], 0);
        const ns_css_value *ostyle = s->values[NS_CSS_OUTLINE_STYLE];
        gboolean ostyle_drawable = ostyle && ostyle->kind == NS_CSS_V_KEYWORD &&
            ostyle->u.keyword && strcmp(ostyle->u.keyword, "none") != 0 &&
            strcmp(ostyle->u.keyword, "hidden") != 0;
        if (ow > 0 && ostyle_drawable) {
            double off = length_or(s->values[NS_CSS_OUTLINE_OFFSET], 0);
            rgba oc = rgba_of(s->values[NS_CSS_OUTLINE_COLOR], 0, 0, 0, 1);
            cairo_save(cr);
            set_source_rgba(cr, oc);
            cairo_set_line_width(cr, ow);
            if (strcmp(ostyle->u.keyword, "dashed") == 0) {
                double dashes[] = { ow * 3, ow * 2 };
                cairo_set_dash(cr, dashes, 2, 0);
            } else if (strcmp(ostyle->u.keyword, "dotted") == 0) {
                double dashes[] = { ow, ow };
                cairo_set_dash(cr, dashes, 2, 0);
            }
            double grow = off + ow / 2.0;
            rounded_rect_path(cr, border_x - grow, border_y - grow,
                              border_w + grow * 2, border_h + grow * 2,
                              outline_radii(radii, border_w, border_h, grow));
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }
    if (legend_gap) cairo_restore(cr);
}

static void
paint_table_row(cairo_t *cr, const ns_box *row)
{
    cairo_save(cr);
    cairo_new_path(cr);
    for (const ns_box *c = row->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_BOX_TABLE_CELL) continue;
        cairo_rectangle(cr, c->x + c->margin.left, c->y + c->margin.top,
                        c->content_width + c->padding.left + c->padding.right +
                        c->border.left + c->border.right,
                        c->content_height + c->padding.top + c->padding.bottom +
                        c->border.top + c->border.bottom);
    }
    cairo_clip(cr);
    paint_block(cr, row);
    cairo_restore(cr);
}

static const ns_style *
inherited_style(const ns_box *b)
{
    for (const ns_box *p = b->parent; p; p = p->parent)
        if (p->style) return p->style;
    return NULL;
}

static void
attr_insert_range(NsPangoAttrList *attrs, NsPangoAttribute *a,
                  gsize start, gsize len)
{
    if (!a) return;
    a->start_index = (guint)start;
    a->end_index   = (guint)(start + len);
    ns_pango_attr_list_insert(attrs, a);
}

static void
decoration_insert_around_atomics(NsPangoAttrList *attrs, NsPangoAttribute *a,
                                 const char *text, gsize start, gsize len)
{
    static const char placeholder[] = "\xef\xbf\xbc";
    if (!a) return;
    gsize text_len = text ? strlen(text) : 0;
    gsize end = MIN(start + len, text_len);
    gsize seg = start;
    for (gsize p = start; p + 3 <= end; ) {
        if (memcmp(text + p, placeholder, 3) != 0) {
            p++;
            continue;
        }
        if (p > seg)
            attr_insert_range(attrs, ns_pango_attribute_copy(a), seg, p - seg);
        p += 3;
        seg = p;
    }
    if (seg == start && end - start == len) {
        attr_insert_range(attrs, a, start, len);
        return;
    }
    if (end > seg) attr_insert_range(attrs, ns_pango_attribute_copy(a), seg,
                                     end - seg);
    ns_pango_attribute_destroy(a);
}

static gboolean
inline_box_is_hidden(const ns_style *s)
{
    const char *vis = ns_style_keyword(s, NS_CSS_VISIBILITY);
    return vis && (strcmp(vis, "hidden") == 0 || strcmp(vis, "collapse") == 0);
}

static gboolean
inline_attr_paints_box(const ns_inline_attr *r)
{
    return r->kind == NS_INLINE_ELEMENT && r->len > 0 &&
           style_has_inline_box_paint(r->style) &&
           !inline_box_is_hidden(r->style);
}

typedef struct inline_layer_line {
    NsPangoLayoutLine *line;
    int x, baseline, top, height;
    guint first_seg, n_segs;
} inline_layer_line;

typedef struct inline_layer_seg {
    guint layer;
    guint line;
    GSList *first;
    GSList *last;
    int x, width;
    gsize lo, hi;
} inline_layer_seg;

typedef struct inline_bucket {
    guint *order;
    guint *start;
} inline_bucket;

typedef struct inline_bucket_item {
    guint value;
    guint layer;
} inline_bucket_item;

typedef struct inline_layers {
    GArray *owners;
    GHashTable *index;
    guint *byte_layer;
    gsize n_bytes;
    GArray *lines;
    GArray *segs;
    inline_bucket seg_order;
    inline_bucket box_order;
    inline_bucket atomic_order;
} inline_layers;

static const char k_inline_layers[] = "ns-inline-layers";

static void
inline_bucket_free(inline_bucket *bk)
{
    g_free(bk->order);
    g_free(bk->start);
}

static void
inline_layers_free(gpointer data)
{
    inline_layers *L = data;
    g_array_free(L->owners, TRUE);
    g_hash_table_destroy(L->index);
    g_free(L->byte_layer);
    if (L->lines) g_array_free(L->lines, TRUE);
    if (L->segs) g_array_free(L->segs, TRUE);
    inline_bucket_free(&L->seg_order);
    inline_bucket_free(&L->box_order);
    inline_bucket_free(&L->atomic_order);
    g_free(L);
}

static gboolean
inline_attr_owns_layer(const ns_inline_attr *r)
{
    return r->kind == NS_INLINE_ELEMENT && r->rel_layer &&
           r->rel_layer == r->dom;
}

static gboolean
inline_box_has_layers(const ns_box *b)
{
    for (guint i = 0; b->attrs && i < b->attrs->len; i++)
        if (inline_attr_owns_layer(&g_array_index(b->attrs, ns_inline_attr, i)))
            return TRUE;
    return FALSE;
}

static int dom_tree_order_cmp(const ns_node *a, const ns_node *b);

static int
inline_owner_cmp(gconstpointer a, gconstpointer b, gpointer data)
{
    const GArray *attrs = data;
    const ns_inline_attr *x =
        &g_array_index(attrs, ns_inline_attr, *(const guint *)a);
    const ns_inline_attr *y =
        &g_array_index(attrs, ns_inline_attr, *(const guint *)b);
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    if (x->len != y->len) return x->len > y->len ? -1 : 1;
    return dom_tree_order_cmp(x->dom, y->dom);
}

static void
inline_owner_note(GHashTable *first, const GArray *attrs, guint i)
{
    const ns_inline_attr *r = &g_array_index(attrs, ns_inline_attr, i);
    gpointer seen = NULL;
    if (g_hash_table_lookup_extended(first, r->dom, NULL, &seen) &&
        g_array_index(attrs, ns_inline_attr,
                      GPOINTER_TO_UINT(seen)).start <= r->start)
        return;
    g_hash_table_insert(first, (gpointer)r->dom, GUINT_TO_POINTER(i));
}

static GArray *
inline_layer_owners(const ns_box *b, GHashTable *index)
{
    GHashTable *first = g_hash_table_new(NULL, NULL);
    for (guint i = 0; i < b->attrs->len; i++)
        if (inline_attr_owns_layer(&g_array_index(b->attrs, ns_inline_attr, i)))
            inline_owner_note(first, b->attrs, i);
    GArray *owners = g_array_new(FALSE, FALSE, sizeof(guint));
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, first);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        guint i = GPOINTER_TO_UINT(value);
        g_array_append_val(owners, i);
    }
    g_hash_table_destroy(first);
    g_array_sort_with_data(owners, inline_owner_cmp, b->attrs);
    guint none = G_MAXUINT;
    g_array_prepend_val(owners, none);
    for (guint k = 1; k < owners->len; k++) {
        const ns_inline_attr *r = &g_array_index(
            b->attrs, ns_inline_attr, g_array_index(owners, guint, k));
        g_hash_table_insert(index, (gpointer)r->dom, GUINT_TO_POINTER(k));
    }
    return owners;
}

static guint
inline_layer_of_attr(GHashTable *index, const ns_inline_attr *r)
{
    return r->rel_layer
        ? GPOINTER_TO_UINT(g_hash_table_lookup(index, r->rel_layer)) : 0;
}

static int
inline_element_tree_order_cmp(gconstpointer a, gconstpointer b, gpointer data)
{
    const GArray *attrs = data;
    guint ia = *(const guint *)a, ib = *(const guint *)b;
    const ns_inline_attr *ra = &g_array_index(attrs, ns_inline_attr, ia);
    const ns_inline_attr *rb = &g_array_index(attrs, ns_inline_attr, ib);
    if (ra->start != rb->start) return ra->start < rb->start ? -1 : 1;
    if (ra->len != rb->len) return ra->len > rb->len ? -1 : 1;
    return ia > ib ? -1 : (ia < ib ? 1 : 0);
}

static GArray *
inline_elements_in_tree_order(const ns_box *b)
{
    GArray *order = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; i < b->attrs->len; i++)
        if (g_array_index(b->attrs, ns_inline_attr, i).kind ==
            NS_INLINE_ELEMENT)
            g_array_append_val(order, i);
    g_array_sort_with_data(order, inline_element_tree_order_cmp, b->attrs);
    return order;
}

static guint *
inline_layer_byte_map(const ns_box *b, gsize n, GHashTable *index)
{
    guint *map = g_new0(guint, n + 1);
    GArray *order = inline_elements_in_tree_order(b);
    for (guint k = 0; k < order->len; k++) {
        const ns_inline_attr *r = &g_array_index(
            b->attrs, ns_inline_attr, g_array_index(order, guint, k));
        guint layer = inline_layer_of_attr(index, r);
        gsize end = MIN(n, r->start + r->len);
        for (gsize i = r->start; i < end; i++) map[i] = layer;
    }
    g_array_free(order, TRUE);
    return map;
}

static void
inline_layers_attach(const ns_box *b, NsPangoLayout *layout)
{
    if (!b->text || !inline_box_has_layers(b)) return;
    inline_layers *L = g_new0(inline_layers, 1);
    L->index = g_hash_table_new(NULL, NULL);
    L->owners = inline_layer_owners(b, L->index);
    L->n_bytes = strlen(b->text);
    L->byte_layer = inline_layer_byte_map(b, L->n_bytes, L->index);
    g_object_set_data_full(G_OBJECT(layout), k_inline_layers, L,
                           inline_layers_free);
}

static void
inline_bucket_build(inline_bucket *bk, guint n_layers, const GArray *items)
{
    bk->start = g_new0(guint, n_layers + 2);
    bk->order = g_new(guint, items->len + 1);
    for (guint i = 0; i < items->len; i++)
        bk->start[g_array_index(items, inline_bucket_item, i).layer + 2]++;
    for (guint k = 2; k < n_layers + 2; k++)
        bk->start[k] += bk->start[k - 1];
    for (guint i = 0; i < items->len; i++) {
        const inline_bucket_item *it =
            &g_array_index(items, inline_bucket_item, i);
        bk->order[bk->start[it->layer + 1]++] = it->value;
    }
}

static gboolean
inline_run_has_cluster_at(const NsPangoLayoutRun *run, int at)
{
    for (int i = 0; i < run->glyphs->num_glyphs; i++)
        if (run->glyphs->log_clusters[i] == at) return TRUE;
    return FALSE;
}

static int
inline_run_layer_break(const inline_layers *L, const NsPangoLayoutRun *run)
{
    gsize start = (gsize)run->item->offset;
    gsize end = MIN(L->n_bytes, start + (gsize)run->item->length);
    if (start >= end ||
        (run->item->analysis.flags & NS_PANGO_ANALYSIS_FLAG_IS_ELLIPSIS))
        return 0;
    for (gsize i = start + 1; i < end; i++)
        if (L->byte_layer[i] != L->byte_layer[start] &&
            inline_run_has_cluster_at(run, (int)(i - start)))
            return (int)(i - start);
    return 0;
}

static gboolean
inline_split_run(const inline_layers *L, GSList *l, const char *text)
{
    NsPangoLayoutRun *run = l->data;
    int at = inline_run_layer_break(L, run);
    NsPangoLayoutRun *head =
        at > 0 ? ns_pango_glyph_item_split(run, text, at) : NULL;
    if (!head) return FALSE;
    gboolean rtl = run->item->analysis.level % 2 != 0;
    l->data = rtl ? run : head;
    l->next = g_slist_prepend(l->next, rtl ? head : run);
    return TRUE;
}

static void
inline_layers_split_line(const inline_layers *L, NsPangoLayoutLine *line,
                         const char *text)
{
    GSList *l = line->runs;
    while (l)
        if (!inline_split_run(L, l, text)) l = l->next;
}

static guint
inline_run_layer(const inline_layers *L, const NsPangoLayoutRun *run)
{
    gsize at = (gsize)run->item->offset;
    if ((run->item->analysis.flags & NS_PANGO_ANALYSIS_FLAG_IS_ELLIPSIS) ||
        at >= L->n_bytes)
        return 0;
    return L->byte_layer[at];
}

static int
inline_run_advance(const NsPangoLayoutRun *run)
{
    int width = ns_pango_glyph_string_get_width(run->glyphs);
    int n = run->glyphs->num_glyphs;
    for (const GSList *l = run->item->analysis.extra_attrs; l; l = l->next) {
        const NsPangoAttribute *attr = l->data;
        if (attr->klass->type != NS_PANGO_ATTR_SHAPE) continue;
        int w = ((const NsPangoAttrShape *)attr)->logical_rect.width;
        width = n > 0 ? MAX(w, w + w * (n - 1)) : 0;
    }
    return run->start_x_offset + width + run->end_x_offset;
}

static void
glyph_range_path(cairo_t *cr, const NsPangoLayoutRun *run, double x, double y,
                 int start, int end)
{
    const NsPangoGlyphString *glyphs = run->glyphs;
    int offset = run->item->offset;
    if (offset >= start && offset + run->item->length <= end) {
        cairo_move_to(cr, x, y);
        ns_pango_cairo_glyph_string_path(cr, run->item->analysis.font,
                                         (NsPangoGlyphString *)glyphs);
        return;
    }
    NsPangoGlyphString *one = ns_pango_glyph_string_new();
    ns_pango_glyph_string_set_size(one, 1);
    for (int g = 0; g < glyphs->num_glyphs; g++) {
        int byte = offset + glyphs->log_clusters[g];
        if (byte >= start && byte < end) {
            one->glyphs[0] = glyphs->glyphs[g];
            one->log_clusters[0] = 0;
            cairo_move_to(cr, x, y);
            ns_pango_cairo_glyph_string_path(cr, run->item->analysis.font, one);
        }
        x += (double)glyphs->glyphs[g].geometry.width / NS_PANGO_SCALE;
    }
    ns_pango_glyph_string_free(one);
}

static void
layout_range_text_path(cairo_t *cr, NsPangoLayout *layout, double x0,
                       double y0, int start, int end)
{
    NsPangoLayoutIter *it = ns_pango_layout_get_iter(layout);
    do {
        NsPangoLayoutLine *line = ns_pango_layout_iter_get_line_readonly(it);
        NsPangoRectangle logical;
        ns_pango_layout_iter_get_line_extents(it, NULL, &logical);
        double y = y0 + (double)ns_pango_layout_iter_get_baseline(it) / NS_PANGO_SCALE;
        int x = logical.x;
        for (GSList *l = line ? line->runs : NULL; l; l = l->next) {
            const NsPangoLayoutRun *run = l->data;
            int item_end = run->item->offset + run->item->length;
            if (run->item->offset < end && item_end > start)
                glyph_range_path(cr, run,
                                 x0 + (double)(x + run->start_x_offset) / NS_PANGO_SCALE,
                                 y, start, end);
            x += inline_run_advance(run);
        }
    } while (ns_pango_layout_iter_next_line(it));
    ns_pango_layout_iter_free(it);
}

static void
inline_layers_index_line(inline_layers *L, NsPangoLayoutLine *line,
                         guint line_no)
{
    inline_layer_seg seg = { 0 };
    int x = 0;
    for (GSList *l = line->runs; l; l = l->next) {
        const NsPangoLayoutRun *run = l->data;
        guint layer = inline_run_layer(L, run);
        if (!seg.first || layer != seg.layer) {
            if (seg.first) g_array_append_val(L->segs, seg);
            seg = (inline_layer_seg){ .layer = layer, .line = line_no,
                                      .first = l, .x = x, .lo = G_MAXSIZE };
        }
        int advance = inline_run_advance(run);
        seg.last = l;
        seg.width += advance;
        seg.lo = MIN(seg.lo, (gsize)run->item->offset);
        seg.hi = MAX(seg.hi, (gsize)(run->item->offset + run->item->length));
        x += advance;
    }
    if (seg.first) g_array_append_val(L->segs, seg);
}

static void
inline_layers_index_lines(inline_layers *L, NsPangoLayout *layout)
{
    const char *text = ns_pango_layout_get_text(layout);
    NsPangoLayoutIter *it = ns_pango_layout_get_iter(layout);
    do {
        NsPangoLayoutLine *line = ns_pango_layout_iter_get_line_readonly(it);
        NsPangoRectangle ink, logical;
        ns_pango_layout_line_get_extents(line, &ink, &logical);
        inline_layers_split_line(L, line, text);
        ns_pango_layout_iter_get_line_extents(it, NULL, &logical);
        inline_layer_line g = { line, logical.x,
                                ns_pango_layout_iter_get_baseline(it),
                                logical.y, logical.height, L->segs->len, 0 };
        inline_layers_index_line(L, line, L->lines->len);
        g.n_segs = L->segs->len - g.first_seg;
        g_array_append_val(L->lines, g);
    } while (ns_pango_layout_iter_next_line(it));
    ns_pango_layout_iter_free(it);
}

static void
inline_layers_bucket_segs(inline_layers *L)
{
    GArray *items = g_array_sized_new(FALSE, FALSE, sizeof(inline_bucket_item),
                                      L->segs->len);
    for (guint i = 0; i < L->segs->len; i++) {
        inline_bucket_item it = {
            i, g_array_index(L->segs, inline_layer_seg, i).layer };
        g_array_append_val(items, it);
    }
    inline_bucket_build(&L->seg_order, L->owners->len, items);
    g_array_free(items, TRUE);
}

static void
inline_layers_bucket_boxes(inline_layers *L, const ns_box *b)
{
    GArray *items = g_array_new(FALSE, FALSE, sizeof(inline_bucket_item));
    for (guint i = b->attrs->len; i-- > 0;) {
        const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
        if (!inline_attr_paints_box(r)) continue;
        inline_bucket_item it = { i, inline_layer_of_attr(L->index, r) };
        g_array_append_val(items, it);
    }
    inline_bucket_build(&L->box_order, L->owners->len, items);
    g_array_free(items, TRUE);
}

static void
inline_layers_bucket_atomics(inline_layers *L, const ns_box *b)
{
    const GArray *atomics = b->inline_atomics;
    GArray *items = g_array_new(FALSE, FALSE, sizeof(inline_bucket_item));
    for (guint i = 0; atomics && i < atomics->len; i++) {
        gsize at = g_array_index(atomics, ns_inline_atomic, i).byte_off;
        inline_bucket_item it = {
            i, at < L->n_bytes ? L->byte_layer[at] : 0 };
        g_array_append_val(items, it);
    }
    inline_bucket_build(&L->atomic_order, L->owners->len, items);
    g_array_free(items, TRUE);
}

static const inline_layers *
inline_layers_of(const ns_box *b, NsPangoLayout *layout)
{
    inline_layers *L = g_object_get_data(G_OBJECT(layout), k_inline_layers);
    if (!L || L->lines) return L;
    L->lines = g_array_new(FALSE, FALSE, sizeof(inline_layer_line));
    L->segs = g_array_new(FALSE, FALSE, sizeof(inline_layer_seg));
    inline_layers_index_lines(L, layout);
    inline_layers_bucket_segs(L);
    inline_layers_bucket_boxes(L, b);
    inline_layers_bucket_atomics(L, b);
    g_clear_pointer(&L->byte_layer, g_free);
    return L;
}

static const ns_inline_attr *
inline_layer_owner(const ns_box *b, const inline_layers *L, guint layer)
{
    if (layer == 0 || layer >= L->owners->len) return NULL;
    return &g_array_index(b->attrs, ns_inline_attr,
                          g_array_index(L->owners, guint, layer));
}

static void
inline_layer_offset(const ns_box *b, const inline_layers *L, guint layer,
                    double *dx, double *dy)
{
    const ns_inline_attr *owner = inline_layer_owner(b, L, layer);
    *dx = owner ? owner->rel_dx : 0;
    *dy = owner ? owner->rel_dy : 0;
}

static void
inline_layer_draw_seg(cairo_t *cr, const inline_layer_seg *seg,
                      NsPangoLayoutLine *line, double x, double y)
{
    GSList *runs = line->runs;
    GSList *after = seg->last->next;
    seg->last->next = NULL;
    line->runs = seg->first;
    cairo_move_to(cr, x + (double)seg->x / NS_PANGO_SCALE, y);
    ns_pango_cairo_show_layout_line(cr, line);
    seg->last->next = after;
    line->runs = runs;
}

static void
inline_layer_show(cairo_t *cr, const inline_layers *L, guint layer,
                  double ox, double oy, const double *baselines)
{
    const inline_bucket *bk = &L->seg_order;
    for (guint k = bk->start[layer]; k < bk->start[layer + 1]; k++) {
        const inline_layer_seg *seg =
            &g_array_index(L->segs, inline_layer_seg, bk->order[k]);
        const inline_layer_line *g =
            &g_array_index(L->lines, inline_layer_line, seg->line);
        double y = baselines ? baselines[seg->line]
                             : oy + (double)g->baseline / NS_PANGO_SCALE;
        inline_layer_draw_seg(cr, seg, g->line,
                              ox + (double)g->x / NS_PANGO_SCALE, y);
    }
}

static const guint k_inline_all_layers = G_MAXUINT;

typedef struct inline_text_src {
    const ns_box *box;
    NsPangoLayout *layout;
    const inline_layers *layers;
    guint layer;
} inline_text_src;

static inline_text_src
inline_text_src_of(const ns_box *b, NsPangoLayout *layout, guint layer)
{
    inline_text_src src = { b, layout, inline_layers_of(b, layout), layer };
    return src;
}

static void
inline_text_src_show(cairo_t *cr, const inline_text_src *src)
{
    if (!src->layers) {
        ns_pango_cairo_show_layout(cr, src->layout);
        return;
    }
    double x = 0, y = 0;
    cairo_get_current_point(cr, &x, &y);
    if (src->layer != k_inline_all_layers) {
        inline_layer_show(cr, src->layers, src->layer, x, y, NULL);
        return;
    }
    for (guint k = 0; k < src->layers->owners->len; k++) {
        double dx, dy;
        inline_layer_offset(src->box, src->layers, k, &dx, &dy);
        inline_layer_show(cr, src->layers, k, x + dx, y + dy, NULL);
    }
}

static gboolean
inline_text_src_extents(const inline_text_src *src, double *x0, double *y0,
                        double *w, double *h)
{
    int lw = 0, lh = 0;
    ns_pango_layout_get_pixel_size(src->layout, &lw, &lh);
    if (lw <= 0 || lh <= 0) return FALSE;
    double sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    if (src->layers && src->layer == k_inline_all_layers)
        ns_inline_shift_bounds(src->box, &sx0, &sy0, &sx1, &sy1);
    *x0 = sx0;
    *y0 = sy0;
    *w = lw + sx1 - sx0;
    *h = lh + sy1 - sy0;
    return TRUE;
}

static gsize
find_ci_substring(const char *hay, gsize hay_len,
                  const char *needle, gsize needle_len,
                  gsize start)
{
    if (needle_len == 0 || start >= hay_len) return (gsize)-1;
    for (gsize i = start; i + needle_len <= hay_len; i++) {
        gboolean match = g_search_case_sensitive
            ? (strncmp(hay + i, needle, needle_len) == 0)
            : (g_ascii_strncasecmp(hay + i, needle, needle_len) == 0);
        if (match)
            return i;
    }
    return (gsize)-1;
}

typedef struct {
    const char *lang;
    const char *xml_lang;
    const char *dir;
} paint_i18n_attrs;

static __thread GHashTable *g_i18n_memo;

void
ns_paint_i18n_memo_begin(void)
{
    if (g_i18n_memo) g_hash_table_remove_all(g_i18n_memo);
    else g_i18n_memo = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                             NULL, g_free);
}

void
ns_paint_i18n_memo_end(void)
{
    g_clear_pointer(&g_i18n_memo, g_hash_table_destroy);
}

static const char *
own_attr_nonempty(const ns_node *n, const char *attr)
{
    const char *v = ns_element_get_attr(n, attr);
    return v && *v ? v : NULL;
}

static const paint_i18n_attrs *
paint_i18n_attrs_for(const ns_node *n)
{
    paint_i18n_attrs *hit = g_hash_table_lookup(g_i18n_memo, n);
    if (hit) return hit;
    const paint_i18n_attrs *up = NULL;
    for (const ns_node *p = n->parent; p && !up; p = p->parent)
        if (p->kind == NS_NODE_ELEMENT) up = paint_i18n_attrs_for(p);
    paint_i18n_attrs *a = g_new0(paint_i18n_attrs, 1);
    if (n->kind == NS_NODE_ELEMENT) {
        a->lang = own_attr_nonempty(n, "lang");
        a->xml_lang = own_attr_nonempty(n, "xml:lang");
        a->dir = own_attr_nonempty(n, "dir");
    }
    if (up) {
        if (!a->lang) a->lang = up->lang;
        if (!a->xml_lang) a->xml_lang = up->xml_lang;
        if (!a->dir) a->dir = up->dir;
    }
    g_hash_table_insert(g_i18n_memo, (gpointer)n, a);
    return a;
}

static const char *
nearest_node_attr(const ns_node *n, const char *attr)
{
    for (const ns_node *p = n; p; p = p->parent) {
        if (p->kind != NS_NODE_ELEMENT) continue;
        const char *v = ns_element_get_attr(p, attr);
        if (v && *v) return v;
    }
    return NULL;
}

NsPangoWrapMode
ns_paint_wrap_mode_for(const ns_style *style)
{
    if (style) {
        const ns_css_value *wb = style->values[NS_CSS_WORD_BREAK];
        if (wb && wb->kind == NS_CSS_V_KEYWORD && wb->u.keyword) {
            if (strcmp(wb->u.keyword, "break-all") == 0)
                return NS_PANGO_WRAP_CHAR;
            if (strcmp(wb->u.keyword, "keep-all") == 0)
                return NS_PANGO_WRAP_WORD;
        }
        const ns_css_value *ow = style->values[NS_CSS_OVERFLOW_WRAP];
        if (ow && ow->kind == NS_CSS_V_KEYWORD && ow->u.keyword) {
            if (strcmp(ow->u.keyword, "normal") == 0)
                return NS_PANGO_WRAP_WORD;
            if (strcmp(ow->u.keyword, "break-word") == 0 ||
                strcmp(ow->u.keyword, "anywhere") == 0)
                return NS_PANGO_WRAP_WORD_CHAR;
        }
    }
    return NS_PANGO_WRAP_WORD;
}

static gboolean
ns_style_is_nowrap(const ns_style *style)
{
    if (!style) return FALSE;
    const ns_css_value *ws = style->values[NS_CSS_WHITE_SPACE];
    return ws && ws->kind == NS_CSS_V_KEYWORD && ws->u.keyword &&
           (strcmp(ws->u.keyword, "nowrap") == 0 ||
            strcmp(ws->u.keyword, "pre") == 0);
}

gboolean
ns_paint_text_ellipsizes(const ns_style *style)
{
    return style && ns_style_is_nowrap(style) &&
           keyword_is(style->values[NS_CSS_TEXT_OVERFLOW], "ellipsis");
}

void
ns_paint_layout_set_inline_text(NsPangoLayout *layout, const char *text)
{
    static const char line_sep[] = "\xe2\x80\xa8";
    if (!text || ns_pango_layout_get_ellipsize(layout) == NS_PANGO_ELLIPSIZE_NONE ||
        ns_pango_layout_get_height(layout) != -1 || !strstr(text, line_sep)) {
        ns_pango_layout_set_text(layout, text, -1);
        return;
    }
    char *paragraphs = g_strdup(text);
    for (char *p = strstr(paragraphs, line_sep); p; p = strstr(p + 3, line_sep))
        p[2] = '\xa9';
    ns_pango_layout_set_text(layout, paragraphs, -1);
    g_free(paragraphs);
}

static double
normal_line_height_from_metrics(const ns_style *s, const char *family,
                                double font_size)
{
    ns_css_font_metrics m = { 0 };
    gboolean italic = s &&
        (keyword_is(s->values[NS_CSS_FONT_STYLE], "italic") ||
         keyword_is(s->values[NS_CSS_FONT_STYLE], "oblique"));
    int weight = ns_css_font_weight_number(
        s ? s->values[NS_CSS_FONT_WEIGHT] : NULL, 400);
    ns_paint_font_metrics(family, font_size, weight, italic, &m);
    return m.line_px;
}

static double
normal_line_height_fallback(const char *family, double font_size)
{
    static const struct {
        const char *name;
        double factor;
        gboolean rounded;
    } known[] = {
        { "Arial", 1.1, FALSE },
        { "Helvetica", 1.1, FALSE },
        { "Times New Roman", 1.125, FALSE },
        { "Times", 1.125, FALSE },
        { "serif", 1.125, FALSE },
        { "Menlo", 1.164, TRUE },
        { "System Font", 1.19, TRUE },
    };
    char *resolved = ns_css_font_family_first_for_pango(family);
    double factor = 1.2;
    gboolean rounded = FALSE;
    for (gsize i = 0; resolved && i < G_N_ELEMENTS(known); i++) {
        if (g_ascii_strcasecmp(resolved, known[i].name) != 0) continue;
        factor = known[i].factor;
        rounded = known[i].rounded;
        break;
    }
    g_free(resolved);
    return rounded ? round(font_size * factor) : ceil(font_size * factor);
}

double
ns_paint_normal_line_height_px(const ns_style *s)
{
    double font_size = length_or(s ? s->values[NS_CSS_FONT_SIZE] : NULL, 16);
    const ns_css_value *fv = s ? s->values[NS_CSS_FONT_FAMILY] : NULL;
    const char *family = fv && fv->kind == NS_CSS_V_KEYWORD ? fv->u.keyword
                                                            : NULL;
    if (font_size > 0) {
        double line = normal_line_height_from_metrics(
            s, family ? family : "sans-serif", font_size);
        if (line > 0) return line;
    }
    return normal_line_height_fallback(family, font_size);
}

double
ns_paint_css_line_height_px(const ns_style *s)
{
    if (!s) return -1;
    const ns_css_value *lh = s->values[NS_CSS_LINE_HEIGHT];
    double font_size = length_or(s->values[NS_CSS_FONT_SIZE], 16);
    if (!lh || keyword_is(lh, "normal"))
        return ns_paint_normal_line_height_px(s);
    if (ns_css_calc_is_math_fn(lh))
        return ns_css_calc_math_fn_px(lh, font_size);
    if (lh->kind == NS_CSS_V_CALC)
        return lh->u.calc.px + lh->u.calc.pct / 100.0 * font_size;
    if (lh->kind != NS_CSS_V_LENGTH) return -1;
    switch (lh->u.length.unit) {
    case NS_CSS_UNIT_PX:      return lh->u.length.v;
    case NS_CSS_UNIT_NUMBER:
    case NS_CSS_UNIT_EM:      return lh->u.length.v * font_size;
    case NS_CSS_UNIT_PERCENT: return lh->u.length.v / 100.0 * font_size;
    case NS_CSS_UNIT_REM:     return lh->u.length.v * 16.0;
    case NS_CSS_UNIT_LH:      return lh->u.length.v * font_size * 1.5;
    case NS_CSS_UNIT_RLH:     return lh->u.length.v * 24.0;
    case NS_CSS_UNIT_EX:      return lh->u.length.v * font_size * 0.5;
    case NS_CSS_UNIT_REX:     return lh->u.length.v * 8.0;
    case NS_CSS_UNIT_CH:      return lh->u.length.v * font_size * 0.5;
    case NS_CSS_UNIT_RCH:     return lh->u.length.v * 8.0;
    case NS_CSS_UNIT_CAP:     return lh->u.length.v * font_size * 0.7;
    case NS_CSS_UNIT_RCAP:    return lh->u.length.v * 11.2;
    case NS_CSS_UNIT_IC:      return lh->u.length.v * font_size;
    case NS_CSS_UNIT_RIC:     return lh->u.length.v * 16.0;
    case NS_CSS_UNIT_VH:
    case NS_CSS_UNIT_CQH:     return lh->u.length.v * ns_css_viewport_h() / 100.0;
    case NS_CSS_UNIT_VW:
    case NS_CSS_UNIT_CQW:     return lh->u.length.v * ns_css_viewport_w() / 100.0;
    case NS_CSS_UNIT_VMIN:
    case NS_CSS_UNIT_CQMIN: {
        double m = MIN(ns_css_viewport_w(), ns_css_viewport_h());
        return lh->u.length.v * m / 100.0;
    }
    case NS_CSS_UNIT_VMAX:
    case NS_CSS_UNIT_CQMAX: {
        double m = MAX(ns_css_viewport_w(), ns_css_viewport_h());
        return lh->u.length.v * m / 100.0;
    }
    default:                  return -1;
    }
}

gboolean
ns_paint_css_line_height_is_normal(const ns_style *s)
{
    if (!s) return FALSE;
    const ns_css_value *lh = s->values[NS_CSS_LINE_HEIGHT];
    return !lh || keyword_is(lh, "normal");
}

void
ns_paint_apply_css_line_spacing(NsPangoLayout *layout, const ns_style *s)
{
    double lh_px = ns_paint_css_line_height_px(s);
    if (!layout || lh_px < 0) return;
    ns_css_line_height *stored = g_new(ns_css_line_height, 1);
    stored->px = lh_px;
    stored->normal = ns_paint_css_line_height_is_normal(s);
    g_object_set_data_full(G_OBJECT(layout), NS_CSS_LINE_HEIGHT_KEY, stored,
                           g_free);
}

void
ns_paint_apply_i18n(NsPangoLayout *layout, NsPangoAttrList *attrs,
                    const ns_box *b)
{
    if (!b) return;
    const ns_node  *dn = b->dom;
    const ns_style *st = b->style;
    for (const ns_box *p = b->parent; (!dn || !st) && p; p = p->parent) {
        if (!dn && p->dom)   dn = p->dom;
        if (!st && p->style) st = p->style;
    }
    if (!dn && !st) return;
    if (attrs) {
        gboolean auto_hyphens =
            st && keyword_is(st->values[NS_CSS_HYPHENS], "auto");
        NsPangoAttribute *ih = ns_pango_attr_insert_hyphens_new(auto_hyphens);
        ih->start_index = 0;
        ih->end_index   = G_MAXUINT;
        ns_pango_attr_list_insert(attrs, ih);
    }
    const paint_i18n_attrs *memo = dn && g_i18n_memo
        ? paint_i18n_attrs_for(dn) : NULL;
    const char *lang = memo ? memo->lang
                     : dn ? nearest_node_attr(dn, "lang") : NULL;
    if (!lang && dn)
        lang = memo ? memo->xml_lang : nearest_node_attr(dn, "xml:lang");
    if (lang && attrs) {
        NsPangoAttribute *a = ns_pango_attr_language_new(
            ns_pango_language_from_string(lang));
        a->start_index = 0;
        a->end_index   = G_MAXUINT;
        ns_pango_attr_list_insert(attrs, a);
    }
    const char *dir = memo ? memo->dir
                    : dn ? nearest_node_attr(dn, "dir") : NULL;
    NsPangoDirection bd = NS_PANGO_DIRECTION_NEUTRAL;
    if (dir) {
        if (g_ascii_strcasecmp(dir, "rtl") == 0) bd = NS_PANGO_DIRECTION_RTL;
        else if (g_ascii_strcasecmp(dir, "ltr") == 0) bd = NS_PANGO_DIRECTION_LTR;
    }
    if (bd == NS_PANGO_DIRECTION_NEUTRAL && st &&
        keyword_is(st->values[NS_CSS_DIRECTION], "rtl"))
        bd = NS_PANGO_DIRECTION_RTL;
    if (bd == NS_PANGO_DIRECTION_NEUTRAL &&
        !(st && keyword_is(st->values[NS_CSS_UNICODE_BIDI], "plaintext")))
        bd = NS_PANGO_DIRECTION_LTR;
    if (bd != NS_PANGO_DIRECTION_NEUTRAL && layout) {
        ns_pango_layout_set_auto_dir(layout, FALSE);
        ns_pango_context_set_base_dir(ns_pango_layout_get_context(layout), bd);
    }
}

static void
append_font_feature(GString *out, const char *feature)
{
    if (!feature || !*feature) return;
    if (out->len > 0) g_string_append(out, ", ");
    g_string_append(out, feature);
}

static void
append_font_kerning_features(GString *out, const ns_css_value *v)
{
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return;
    if (strcmp(v->u.keyword, "none") == 0)
        append_font_feature(out, "kern=0");
    else if (strcmp(v->u.keyword, "normal") == 0 ||
             strcmp(v->u.keyword, "auto") == 0)
        append_font_feature(out, "kern=1");
}

static void
append_font_ligature_token(GString *out, const char *token)
{
    if (strcmp(token, "none") == 0) {
        append_font_feature(out, "liga=0");
        append_font_feature(out, "clig=0");
        append_font_feature(out, "dlig=0");
        append_font_feature(out, "hlig=0");
        append_font_feature(out, "calt=0");
    } else if (strcmp(token, "normal") == 0) {
        append_font_feature(out, "liga=1");
        append_font_feature(out, "clig=1");
        append_font_feature(out, "dlig=0");
        append_font_feature(out, "hlig=0");
        append_font_feature(out, "calt=1");
    } else if (strcmp(token, "common-ligatures") == 0) {
        append_font_feature(out, "liga=1");
        append_font_feature(out, "clig=1");
    } else if (strcmp(token, "no-common-ligatures") == 0) {
        append_font_feature(out, "liga=0");
        append_font_feature(out, "clig=0");
    } else if (strcmp(token, "discretionary-ligatures") == 0) {
        append_font_feature(out, "dlig=1");
    } else if (strcmp(token, "no-discretionary-ligatures") == 0) {
        append_font_feature(out, "dlig=0");
    } else if (strcmp(token, "historical-ligatures") == 0) {
        append_font_feature(out, "hlig=1");
    } else if (strcmp(token, "no-historical-ligatures") == 0) {
        append_font_feature(out, "hlig=0");
    } else if (strcmp(token, "contextual") == 0) {
        append_font_feature(out, "calt=1");
    } else if (strcmp(token, "no-contextual") == 0) {
        append_font_feature(out, "calt=0");
    }
}

static void
append_font_ligature_features(GString *out, const char *ligatures)
{
    if (!ligatures || !*ligatures) return;
    char **tokens = g_strsplit_set(ligatures, " \t\r\n\f", -1);
    for (int i = 0; tokens[i]; i++) {
        if (*tokens[i])
            append_font_ligature_token(out, tokens[i]);
    }
    g_strfreev(tokens);
}

static const char *
paint_font_feature_skip_ws(const char *p)
{
    while (*p && g_ascii_isspace((unsigned char)*p)) p++;
    return p;
}

static gboolean
paint_font_feature_read_tag(const char **pp, char tag[5])
{
    const char *p = paint_font_feature_skip_ws(*pp);
    if (*p != '"' && *p != '\'') return FALSE;
    char quote = *p++;
    const char *s = p;
    while (*p && *p != quote) p++;
    if (*p != quote || p - s != 4) return FALSE;
    memcpy(tag, s, 4);
    tag[4] = '\0';
    *pp = p + 1;
    return TRUE;
}

static gboolean
paint_font_feature_read_value(const char **pp, int *out)
{
    const char *p = paint_font_feature_skip_ws(*pp);
    *out = 1;
    if (!*p || *p == ',') {
        *pp = p;
        return TRUE;
    }
    if (g_ascii_isalpha((unsigned char)*p)) {
        const char *s = p;
        while (g_ascii_isalpha((unsigned char)*p) || *p == '-') p++;
        char *kw = g_ascii_strdown(s, (gssize)(p - s));
        if (strcmp(kw, "on") == 0) *out = 1;
        else if (strcmp(kw, "off") == 0) *out = 0;
        else {
            g_free(kw);
            return FALSE;
        }
        g_free(kw);
        *pp = paint_font_feature_skip_ws(p);
        return TRUE;
    }
    if (!g_ascii_isdigit((unsigned char)*p)) return FALSE;
    char *endp = NULL;
    gint64 v = g_ascii_strtoll(p, &endp, 10);
    if (!endp || endp == p) return FALSE;
    if (v < 0) v = 0;
    if (v > G_MAXINT) v = G_MAXINT;
    *out = (int)v;
    *pp = paint_font_feature_skip_ws(endp);
    return TRUE;
}

static void
append_font_feature_settings_features(GString *out, const char *settings)
{
    if (!settings || !*settings || strcmp(settings, "normal") == 0) return;
    const char *p = paint_font_feature_skip_ws(settings);
    while (*p) {
        char tag[5];
        int value = 1;
        if (!paint_font_feature_read_tag(&p, tag)) return;
        if (!paint_font_feature_read_value(&p, &value)) return;
        char feature[32];
        g_snprintf(feature, sizeof(feature), "%s=%d", tag, value);
        append_font_feature(out, feature);
        if (*p != ',') break;
        p = paint_font_feature_skip_ws(p + 1);
    }
}

static gboolean
paint_font_variation_read_value(const char **pp, char value[32])
{
    const char *p = paint_font_feature_skip_ws(*pp);
    if (!*p || *p == ',') return FALSE;
    char *endp = NULL;
    double v = g_ascii_strtod(p, &endp);
    if (!endp || endp == p || !isfinite(v)) return FALSE;
    g_ascii_formatd(value, 32, "%.8g", v);
    *pp = paint_font_feature_skip_ws(endp);
    return TRUE;
}

static char *
paint_font_variations_from_css(const char *settings)
{
    if (!settings || !*settings) return NULL;
    if (strcmp(settings, "normal") == 0) return g_strdup("");
    GString *out = g_string_new(NULL);
    const char *p = paint_font_feature_skip_ws(settings);
    while (*p) {
        char tag[5];
        char value[32];
        if (!paint_font_feature_read_tag(&p, tag) ||
            !paint_font_variation_read_value(&p, value)) {
            g_string_free(out, TRUE);
            return NULL;
        }
        if (out->len > 0) g_string_append_c(out, ',');
        g_string_append_printf(out, "%s=%s", tag, value);
        if (*p != ',') break;
        p = paint_font_feature_skip_ws(p + 1);
    }
    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

NsPangoAttribute *
ns_paint_font_features_attr_from_values(int kerning, const char *ligatures,
                                        const char *settings)
{
    GString *s = g_string_new(NULL);
    if (kerning == 0)
        append_font_feature(s, "kern=0");
    else if (kerning > 0)
        append_font_feature(s, "kern=1");
    append_font_ligature_features(s, ligatures);
    append_font_feature_settings_features(s, settings);
    if (s->len == 0) {
        g_string_free(s, TRUE);
        return NULL;
    }
    char *features = g_string_free(s, FALSE);
    NsPangoAttribute *a = ns_pango_attr_font_features_new(features);
    g_free(features);
    return a;
}

NsPangoAttribute *
ns_paint_font_variations_attr_from_values(const char *settings)
{
    char *variations = paint_font_variations_from_css(settings);
    if (!variations) return NULL;
    NsPangoFontDescription *desc = ns_pango_font_description_new();
    ns_pango_font_description_set_variations(desc, variations);
    NsPangoAttribute *a = ns_pango_attr_font_desc_new(desc);
    ns_pango_font_description_free(desc);
    g_free(variations);
    return a;
}

void
ns_paint_apply_font_features(NsPangoAttrList *attrs, const ns_style *s,
                             guint start, guint end)
{
    if (!attrs || !s) return;
    GString *features = g_string_new(NULL);
    append_font_kerning_features(features, s->values[NS_CSS_FONT_KERNING]);
    const ns_css_value *lig = s->values[NS_CSS_FONT_VARIANT_LIGATURES];
    if (lig && lig->kind == NS_CSS_V_KEYWORD)
        append_font_ligature_features(features, lig->u.keyword);
    const ns_css_value *settings = s->values[NS_CSS_FONT_FEATURE_SETTINGS];
    if (settings && settings->kind == NS_CSS_V_KEYWORD)
        append_font_feature_settings_features(features, settings->u.keyword);
    if (features->len == 0) {
        g_string_free(features, TRUE);
        return;
    }
    char *str = g_string_free(features, FALSE);
    NsPangoAttribute *a = ns_pango_attr_font_features_new(str);
    g_free(str);
    a->start_index = start;
    a->end_index = end;
    ns_pango_attr_list_insert(attrs, a);
}

static gboolean
ns_paint_font_available(const char *family)
{
    static GHashTable *avail;
    static guint cached_serial;
    if (!family || !*family) return TRUE;
    NsPangoFontMap *fm = ns_pango_cairo_font_map_get_default();
    guint serial = fm ? ns_pango_font_map_get_serial(fm) : 0;
    if (!avail || serial != cached_serial) {
        if (avail) g_hash_table_remove_all(avail);
        else avail = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        cached_serial = serial;
        if (fm) {
            NsPangoFontFamily **fams = NULL;
            int n = 0;
            ns_pango_font_map_list_families(fm, &fams, &n);
            for (int i = 0; i < n; i++) {
                const char *nm = ns_pango_font_family_get_name(fams[i]);
                if (nm) g_hash_table_add(avail, g_ascii_strdown(nm, -1));
            }
            g_free(fams);
        }
    }
    char *lo = g_ascii_strdown(family, -1);
    gboolean has = g_hash_table_contains(avail, lo);
    g_free(lo);
    if (!has) has = ns_font_family_loaded(family);
    return has;
}

#define NS_FONT_ALIAS_CACHE_MAX 1024

static char *
ns_paint_font_alias(const char *family)
{
    static GMutex lock;
    static GHashTable *aliases;
    if (!family || !*family) return NULL;
    char *key = g_ascii_strdown(family, -1);
    g_mutex_lock(&lock);
    if (!aliases)
        aliases = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    gpointer cached = NULL;
    gboolean known = g_hash_table_lookup_extended(aliases, key, NULL, &cached);
    g_mutex_unlock(&lock);
    if (known) {
        g_free(key);
        return cached ? g_strdup(cached) : NULL;
    }
    char *alias = ns_font_metric_alias(family);
    if (alias && !ns_paint_font_available(alias)) {
        g_free(alias);
        alias = NULL;
    }
    g_mutex_lock(&lock);
    if (g_hash_table_size(aliases) >= NS_FONT_ALIAS_CACHE_MAX)
        g_hash_table_remove_all(aliases);
    g_hash_table_replace(aliases, key, g_strdup(alias));
    g_mutex_unlock(&lock);
    return alias;
}

static gboolean
ns_paint_text_family_available(const char *family)
{
    if (!ns_paint_font_available(family)) return FALSE;
    return !ns_font_family_is_emoji(family) || ns_font_family_loaded(family);
}

typedef struct {
    char    *family;
    double   size_px;
    int      weight;
    gboolean italic;
} ns_font_metrics_key;

static guint
font_metrics_key_hash(gconstpointer v)
{
    const ns_font_metrics_key *k = v;
    return (k->family ? g_str_hash(k->family) : 0) ^
           g_double_hash(&k->size_px) ^
           (guint)(k->weight * 31 + (k->italic ? 1 : 0));
}

static gboolean
font_metrics_key_equal(gconstpointer a, gconstpointer b)
{
    const ns_font_metrics_key *ka = a, *kb = b;
    return ka->size_px == kb->size_px && ka->weight == kb->weight &&
           ka->italic == kb->italic && g_strcmp0(ka->family, kb->family) == 0;
}

static void
font_metrics_key_free(gpointer v)
{
    ns_font_metrics_key *k = v;
    g_free(k->family);
    g_free(k);
}

static gboolean
font_has_legacy_mac_ascent(const char *family)
{
#ifdef __APPLE__
    return family && (g_ascii_strcasecmp(family, "Times") == 0 ||
                      g_ascii_strcasecmp(family, "Helvetica") == 0 ||
                      g_ascii_strcasecmp(family, "Courier") == 0);
#else
    (void)family;
    return FALSE;
#endif
}

static void
font_vertical_metrics(NsPangoContext *ctx, const NsPangoFontDescription *fd,
                      const char *family, ns_css_font_metrics *out)
{
    NsPangoFontMetrics *fm = ns_pango_context_get_metrics(ctx, fd, NULL);
    if (!fm) return;
    double raw_ascent = ns_pango_font_metrics_get_ascent(fm) / (double)NS_PANGO_SCALE;
    double raw_descent = ns_pango_font_metrics_get_descent(fm) / (double)NS_PANGO_SCALE;
    double height = ns_pango_font_metrics_get_height(fm) / (double)NS_PANGO_SCALE;
    ns_pango_font_metrics_unref(fm);
    double ascent = round(raw_ascent);
    double descent = round(raw_descent);
    double gap = height > raw_ascent + raw_descent
        ? round(height - raw_ascent - raw_descent) : 0;
    if (font_has_legacy_mac_ascent(family))
        ascent += floor((ascent + descent) * 0.15 + 0.5);
    double line = ascent + descent + gap;
    if (!(line > 0)) return;
    out->line_px = line;
    out->ascent_px = ascent;
    out->descent_px = descent;
}

static void
font_metrics_measure(const char *family, double size_px, int weight,
                     gboolean italic, ns_css_font_metrics *out)
{
    NsPangoLayout *l = paint_create_layout();
    if (!l) return;
    NsPangoFontDescription *fd = ns_pango_font_description_new();
    char *ns_pango_family = family ? ns_css_font_family_for_pango(family) : NULL;
    if (ns_pango_family && *ns_pango_family)
        ns_pango_font_description_set_family(fd, ns_pango_family);
    if (weight > 0) ns_pango_font_description_set_weight(fd, (NsPangoWeight)weight);
    if (italic) ns_pango_font_description_set_style(fd, NS_PANGO_STYLE_ITALIC);
    ns_pango_font_description_set_absolute_size(
        fd, ns_paint_pango_font_size(size_px));
    ns_pango_layout_set_font_description(l, fd);
    g_free(ns_pango_family);
    char *first_family = ns_css_font_family_first_for_pango(family);
    font_vertical_metrics(ns_pango_layout_get_context(l), fd, first_family,
                          out);
    g_free(first_family);

    NsPangoRectangle ink;
    ns_pango_layout_set_text(l, "x", -1);
    ns_pango_layout_get_pixel_extents(l, &ink, NULL);
    if (ink.height > 0) out->ex_px = ink.height;

    ns_pango_layout_set_text(l, "H", -1);
    ns_pango_layout_get_pixel_extents(l, &ink, NULL);
    if (ink.height > 0) out->cap_px = ink.height;

    int w = 0;
    ns_pango_layout_set_text(l, "0", -1);
    ns_pango_layout_get_pixel_size(l, &w, NULL);
    if (w > 0) out->ch_px = w;

    w = 0;
    ns_pango_layout_set_text(l, "\xe6\xb0\xb4", -1);
    ns_pango_layout_get_pixel_size(l, &w, NULL);
    if (w > 0) out->ic_px = w;

    ns_pango_font_description_free(fd);
    g_object_unref(l);
}

static void
ns_paint_font_metrics(const char *family, double size_px, int weight,
                      gboolean italic, ns_css_font_metrics *out)
{
    static GHashTable *cache;
    static guint cached_serial;

    if (size_px <= 0) return;

    NsPangoFontMap *fm = ns_pango_cairo_font_map_get_default();
    guint serial = fm ? ns_pango_font_map_get_serial(fm) : 0;
    if (!cache) {
        cache = g_hash_table_new_full(font_metrics_key_hash,
                                      font_metrics_key_equal,
                                      font_metrics_key_free, g_free);
        cached_serial = serial;
    } else if (serial != cached_serial) {
        g_hash_table_remove_all(cache);
        cached_serial = serial;
    }

    ns_font_metrics_key probe = {
        .family = (char *)family, .size_px = size_px,
        .weight = weight, .italic = italic,
    };
    const ns_css_font_metrics *hit = g_hash_table_lookup(cache, &probe);
    if (hit) {
        *out = *hit;
        return;
    }

    ns_css_font_metrics m = {
        .ex_px  = size_px * 0.5,
        .ch_px  = size_px * 0.5,
        .cap_px = size_px * 0.7,
        .ic_px  = size_px,
    };
    font_metrics_measure(family, size_px, weight, italic, &m);

    ns_font_metrics_key *key = g_new(ns_font_metrics_key, 1);
    key->family = g_strdup(family);
    key->size_px = size_px;
    key->weight = weight;
    key->italic = italic;
    g_hash_table_insert(cache, key, g_memdup2(&m, sizeof m));

    *out = m;
}

static guint64
ns_paint_font_generation(void)
{
    NsPangoFontMap *fm = ns_pango_cairo_font_map_get_default();
    guint serial = fm ? ns_pango_font_map_get_serial(fm) : 0;
    return ((guint64)serial << 32) | ns_font_generation();
}

void
ns_paint_register_font_oracle(void)
{
    ns_css_set_font_available_cb(ns_paint_text_family_available);
    ns_css_set_font_alias_cb(ns_paint_font_alias);
    ns_css_set_font_family_name_cb(ns_font_family_for_text);
    ns_css_set_font_generation_cb(ns_paint_font_generation);
    ns_css_set_font_metrics_cb(ns_paint_font_metrics);
}

void
ns_paint_apply_inline_font(NsPangoLayout *layout, const ns_style *s)
{
    NsPangoFontDescription *desc = ns_pango_font_description_new();
    double font_size = length_or(s ? s->values[NS_CSS_FONT_SIZE] : NULL, 16);
    const char *family = "sans-serif";
    const ns_css_value *fam = s ? s->values[NS_CSS_FONT_FAMILY] : NULL;
    if (fam && fam->kind == NS_CSS_V_KEYWORD) family = fam->u.keyword;
    char *ns_pango_family = ns_css_font_family_for_pango(family);
    ns_pango_font_description_set_family(desc, ns_pango_family);
    g_free(ns_pango_family);
    ns_pango_font_description_set_absolute_size(
        desc, ns_paint_pango_font_size(font_size));
    const ns_css_value *fw = s ? s->values[NS_CSS_FONT_WEIGHT] : NULL;
    int font_weight = ns_css_font_weight_number(fw, -1);
    if (font_weight > 0)
        ns_pango_font_description_set_weight(desc, ns_pango_weight_from_css(font_weight));
    ns_pango_font_description_set_stretch(desc,
        ns_pango_stretch_from_css(ns_css_font_stretch_rank(
            s ? s->values[NS_CSS_FONT_STRETCH] : NULL)));
    if (keyword_is(s ? s->values[NS_CSS_FONT_STYLE] : NULL, "italic"))
        ns_pango_font_description_set_style(desc, NS_PANGO_STYLE_ITALIC);
    else if (keyword_is(s ? s->values[NS_CSS_FONT_STYLE] : NULL, "oblique"))
        ns_pango_font_description_set_style(desc, NS_PANGO_STYLE_OBLIQUE);
    char *variations = paint_font_variations_from_css(
        ns_style_keyword(s, NS_CSS_FONT_VARIATION_SETTINGS));
    if (variations) {
        ns_pango_font_description_set_variations(desc, variations);
        g_free(variations);
    }
    ns_pango_layout_set_font_description(layout, desc);
    ns_pango_font_description_free(desc);

    const ns_css_value *tsv = s ? s->values[NS_CSS_TAB_SIZE] : NULL;
    if (tsv && tsv->kind == NS_CSS_V_LENGTH && tsv->u.length.v > 0) {
        double tab_w;
        if (tsv->u.length.unit == NS_CSS_UNIT_NUMBER) {
            NsPangoContext *ctx = ns_pango_layout_get_context(layout);
            NsPangoLayout *probe = ns_pango_layout_new(ctx);
            ns_pango_layout_set_font_description(probe,
                ns_pango_layout_get_font_description(layout));
            ns_pango_layout_set_text(probe, " ", 1);
            int sw = 0, sh = 0;
            ns_pango_layout_get_size(probe, &sw, &sh);
            g_object_unref(probe);
            tab_w = tsv->u.length.v * (sw / (double)NS_PANGO_SCALE);
        } else {
            tab_w = tsv->u.length.v;
        }
        if (tab_w > 0) {
            int n = 32;
            NsPangoTabArray *tabs = ns_pango_tab_array_new(n, TRUE);
            for (int i = 0; i < n; i++)
                ns_pango_tab_array_set_tab(tabs, i, NS_PANGO_TAB_LEFT,
                                        (int)((i + 1) * tab_w + 0.5));
            ns_pango_layout_set_tabs(layout, tabs);
            ns_pango_tab_array_free(tabs);
        }
    }
}

static void
ns_paint_apply_justify(NsPangoLayout *layout, const ns_style *s, gboolean rtl)
{
    const ns_css_value *last = s->values[NS_CSS_TEXT_ALIGN_LAST];
    ns_pango_layout_set_justify(layout, TRUE);
    if (keyword_is(last, "justify"))
        ns_pango_layout_set_justify_last_line(layout, TRUE);
    else if (keyword_is(last, "center"))
        ns_pango_layout_set_alignment(layout, NS_PANGO_ALIGN_CENTER);
    else if (keyword_is(last, "right") || (keyword_is(last, "end") && !rtl) ||
             (keyword_is(last, "start") && rtl) ||
             ((!last || keyword_is(last, "auto")) && rtl))
        ns_pango_layout_set_alignment(layout, NS_PANGO_ALIGN_RIGHT);
    else
        ns_pango_layout_set_alignment(layout, NS_PANGO_ALIGN_LEFT);
}

void
ns_paint_apply_text_align(NsPangoLayout *layout, const ns_style *s)
{
    const ns_css_value *ta = s ? s->values[NS_CSS_TEXT_ALIGN] : NULL;
    gboolean rtl = ns_pango_context_get_base_dir(
        ns_pango_layout_get_context(layout)) == NS_PANGO_DIRECTION_RTL;
    if (keyword_is(ta, "center"))
        ns_pango_layout_set_alignment(layout, NS_PANGO_ALIGN_CENTER);
    else if (keyword_is(ta, "right") ||
             (keyword_is(ta, "end") && !rtl) ||
             (keyword_is(ta, "start") && rtl) ||
             (!ta && rtl))
        ns_pango_layout_set_alignment(layout, NS_PANGO_ALIGN_RIGHT);
    else if (keyword_is(ta, "justify") &&
             !keyword_is(s->values[NS_CSS_TEXT_JUSTIFY], "none"))
        ns_paint_apply_justify(layout, s, rtl);
    else
        ns_pango_layout_set_alignment(layout, NS_PANGO_ALIGN_LEFT);
}

void
ns_paint_start_align_overflow(NsPangoLayout *layout)
{
    int width = ns_pango_layout_get_width(layout);
    if (width < 0) return;
    gboolean rtl = ns_pango_context_get_base_dir(
        ns_pango_layout_get_context(layout)) == NS_PANGO_DIRECTION_RTL;
    NsPangoAlignment start = rtl ? NS_PANGO_ALIGN_RIGHT : NS_PANGO_ALIGN_LEFT;
    if (ns_pango_layout_get_alignment(layout) == start) return;
    gboolean all_overflow = TRUE;
    NsPangoLayoutIter *iter = ns_pango_layout_get_iter(layout);
    do {
        NsPangoRectangle logical;
        ns_pango_layout_iter_get_line_extents(iter, NULL, &logical);
        if (logical.width <= width) all_overflow = FALSE;
    } while (all_overflow && ns_pango_layout_iter_next_line(iter));
    ns_pango_layout_iter_free(iter);
    if (all_overflow)
        ns_pango_layout_set_alignment(layout, start);
}

static void
apply_nowrap_align_width(NsPangoLayout *layout, const ns_box *b)
{
    if (ns_pango_layout_get_width(layout) >= 0) return;
    NsPangoAlignment al = ns_pango_layout_get_alignment(layout);
    if (al == NS_PANGO_ALIGN_LEFT) return;
    int pw, ph;
    ns_pango_layout_get_pixel_size(layout, &pw, &ph);
    if (pw <= b->content_width)
        ns_pango_layout_set_width(layout, (int)(b->content_width * NS_PANGO_SCALE));
}

static void paint_walk(cairo_t *cr, const ns_box *b, const char *highlight);

static double
layout_baseline_at_index(NsPangoLayout *layout, int index)
{
    double baseline = 0;
    NsPangoLayoutIter *iter = ns_pango_layout_get_iter(layout);
    do {
        NsPangoLayoutLine *line = ns_pango_layout_iter_get_line_readonly(iter);
        baseline = (double)ns_pango_layout_iter_get_baseline(iter) / NS_PANGO_SCALE;
        if (line && index < line->start_index + line->length) break;
    } while (ns_pango_layout_iter_next_line(iter));
    ns_pango_layout_iter_free(iter);
    return baseline;
}

static gboolean
inline_has_form_controls(const ns_box *b)
{
    if (!b || !b->attrs) return FALSE;
    for (guint i = 0; i < b->attrs->len; i++) {
        const ns_inline_attr *r =
            &g_array_index(b->attrs, ns_inline_attr, i);
        if (r->kind == NS_INLINE_INPUT_FIELD ||
            r->kind == NS_INLINE_INPUT_FIELD_FOCUSED ||
            r->kind == NS_INLINE_BUTTON)
            return TRUE;
    }
    return FALSE;
}

double
ns_paint_inline_y_offset_for_layout(const ns_box *b, NsPangoLayout *layout)
{
    if (!b || !layout) return 0;
    int ph;
    ns_pango_layout_get_size(layout, NULL, &ph);
    double y_offset = (b->content_height - (double)ph / NS_PANGO_SCALE) * 0.5;
    if (b->atomic_line_offsets && b->atomic_line_offsets->len == 1)
        y_offset = g_array_index(b->atomic_line_offsets, double, 0);
    if (inline_has_form_controls(b)) y_offset = 0;
    if (y_offset < 0 &&
        ns_paint_css_line_height_px(inherited_style(b)) <= 0)
        y_offset = 0;
    return y_offset;
}

static void
apply_first_line_attrs(NsPangoAttrList *attrs, const ns_style *fl,
                       guint start, guint end)
{
    if (!fl || end <= start) return;
    guint len = end - start;
    const ns_css_value *fs = fl->values[NS_CSS_FONT_SIZE];
    if (fs && fs->kind == NS_CSS_V_LENGTH && fs->u.length.unit == NS_CSS_UNIT_PX)
        attr_insert_range(attrs,
            ns_pango_attr_size_new_absolute(
                ns_paint_pango_font_size(fs->u.length.v)),
            start, len);
    const ns_css_value *col = fl->values[NS_CSS_COLOR];
    if (col && col->kind == NS_CSS_V_COLOR) {
        attr_insert_range(attrs,
            ns_pango_attr_foreground_new((guint16)(col->u.color.r * 0x101),
                                      (guint16)(col->u.color.g * 0x101),
                                      (guint16)(col->u.color.b * 0x101)),
            start, len);
        if (col->u.color.a < 255)
            attr_insert_range(attrs,
                ns_pango_attr_foreground_alpha_new(
                    col->u.color.a ? (guint16)(col->u.color.a * 0x101) : 1),
                start, len);
    }
    const ns_css_value *bg = fl->values[NS_CSS_BACKGROUND_COLOR];
    if (bg && bg->kind == NS_CSS_V_COLOR && bg->u.color.a > 0) {
        attr_insert_range(attrs,
            ns_pango_attr_background_new((guint16)(bg->u.color.r * 0x101),
                                      (guint16)(bg->u.color.g * 0x101),
                                      (guint16)(bg->u.color.b * 0x101)),
            start, len);
        if (bg->u.color.a < 255)
            attr_insert_range(attrs,
                ns_pango_attr_background_alpha_new(
                    (guint16)(bg->u.color.a * 0x101)),
                start, len);
    }
    int fw = ns_css_font_weight_number(fl->values[NS_CSS_FONT_WEIGHT], -1);
    if (fw > 0)
        attr_insert_range(attrs, ns_pango_attr_weight_new(ns_pango_weight_from_css(fw)),
                          start, len);
    if (fl->values[NS_CSS_FONT_STRETCH])
        attr_insert_range(attrs,
            ns_pango_attr_stretch_new(ns_pango_stretch_from_css(
                ns_css_font_stretch_rank(fl->values[NS_CSS_FONT_STRETCH]))),
            start, len);
    if (keyword_is(fl->values[NS_CSS_FONT_STYLE], "italic") ||
        keyword_is(fl->values[NS_CSS_FONT_STYLE], "oblique"))
        attr_insert_range(attrs, ns_pango_attr_style_new(NS_PANGO_STYLE_ITALIC),
                          start, len);
    const ns_css_value *ff = fl->values[NS_CSS_FONT_FAMILY];
    if (ff && ff->kind == NS_CSS_V_KEYWORD && ff->u.keyword) {
        char *pf = ns_css_font_family_for_pango(ff->u.keyword);
        attr_insert_range(attrs, ns_pango_attr_family_new(pf), start, len);
        g_free(pf);
    }
    if (keyword_is(fl->values[NS_CSS_FONT_VARIANT], "small-caps"))
        attr_insert_range(attrs, ns_pango_attr_variant_new(NS_PANGO_VARIANT_SMALL_CAPS),
                          start, len);
    ns_paint_apply_font_features(attrs, fl, start, end);
    attr_insert_range(attrs,
        ns_paint_font_variations_attr_from_values(
            ns_style_keyword(fl, NS_CSS_FONT_VARIATION_SETTINGS)),
        start, len);
    const ns_css_value *td = fl->values[NS_CSS_TEXT_DECORATION];
    const ns_css_value *tdc = fl->values[NS_CSS_TEXT_DECORATION_COLOR];
    gboolean invisible = tdc && tdc->kind == NS_CSS_V_COLOR &&
                         tdc->u.color.a == 0;
    if (td && td->kind == NS_CSS_V_KEYWORD && td->u.keyword && !invisible &&
        strstr(td->u.keyword, "underline") && !strstr(td->u.keyword, "none"))
        attr_insert_range(attrs, ns_pango_attr_underline_new(NS_PANGO_UNDERLINE_SINGLE),
                          start, len);
}

static const ns_css_value *
decoration_value_of(const ns_inline_attr *r, const ns_style *s, ns_css_prop prop)
{
    const ns_css_value *v = r && r->style ? r->style->values[prop] : NULL;
    if (!v && s) v = s->values[prop];
    return v;
}

static gboolean
decoration_length_px(const ns_css_value *v, double em, double *px)
{
    if (!v) return FALSE;
    if (v->kind == NS_CSS_V_LENGTH && v->u.length.unit == NS_CSS_UNIT_PERCENT) {
        *px = v->u.length.v * em / 100.0;
        return TRUE;
    }
    if (v->kind != NS_CSS_V_LENGTH && v->kind != NS_CSS_V_CALC) return FALSE;
    *px = ns_css_dimension_px(v, em, em);
    return TRUE;
}

static double
decoration_thickness_px(const ns_inline_attr *r, const ns_style *s, double em)
{
    double px;
    if (decoration_length_px(decoration_value_of(r, s, NS_CSS_TEXT_DECORATION_THICKNESS),
                             em, &px))
        return MAX(px, 1.0);
    return MAX(em / 16.0, 1.0);
}

static gboolean
decoration_has_custom_metrics(const ns_inline_attr *r, const ns_style *s)
{
    double px;
    double em = r->font_size_px > 0 ? r->font_size_px : 16.0;
    if (decoration_length_px(decoration_value_of(r, s, NS_CSS_TEXT_DECORATION_THICKNESS),
                             em, &px))
        return TRUE;
    return r->kind == NS_INLINE_UNDERLINE &&
           decoration_length_px(decoration_value_of(r, s, NS_CSS_TEXT_UNDERLINE_OFFSET),
                                em, &px);
}

static const char *
underline_dash_style(const ns_inline_attr *r, const ns_style *s)
{
    const ns_css_value *dv = NULL;
    if (r->style && r->style->values[NS_CSS_TEXT_DECORATION_STYLE])
        dv = r->style->values[NS_CSS_TEXT_DECORATION_STYLE];
    else if (s && s->values[NS_CSS_TEXT_DECORATION_STYLE])
        dv = s->values[NS_CSS_TEXT_DECORATION_STYLE];
    const char *kw = dv && dv->kind == NS_CSS_V_KEYWORD ? dv->u.keyword : NULL;
    if (kw && (strcmp(kw, "dotted") == 0 || strcmp(kw, "dashed") == 0))
        return kw;
    if ((!kw || strcmp(kw, "solid") == 0) && decoration_has_custom_metrics(r, s))
        return "solid";
    return NULL;
}

static const ns_css_value *
decoration_color_of(const ns_inline_attr *r, const ns_style *s)
{
    const ns_css_value *cv = r && r->style
        ? r->style->values[NS_CSS_TEXT_DECORATION_COLOR] : NULL;
    if (!cv && s) cv = s->values[NS_CSS_TEXT_DECORATION_COLOR];
    return cv && cv->kind == NS_CSS_V_COLOR ? cv : NULL;
}

static const char *
decoration_style_of(const ns_inline_attr *r, const ns_style *s)
{
    const ns_css_value *dv = r && r->style
        ? r->style->values[NS_CSS_TEXT_DECORATION_STYLE] : NULL;
    if (!dv && s) dv = s->values[NS_CSS_TEXT_DECORATION_STYLE];
    return dv && dv->kind == NS_CSS_V_KEYWORD ? dv->u.keyword : NULL;
}

typedef struct dashed_decoration {
    const ns_box *box;
    const ns_inline_attr *attr;
    const char *keyword;
    double text_x, base_y;
    rgba base;
} dashed_decoration;

static double
dashed_decoration_y(const ns_inline_attr *r, const ns_style *s, double base_y,
                    double em, double thick)
{
    if (r->kind == NS_INLINE_STRIKETHROUGH)
        return base_y - em * 0.28;
    if (r->kind == NS_INLINE_OVERLINE)
        return base_y - em * 0.78;
    double offset;
    if (decoration_length_px(decoration_value_of(r, s, NS_CSS_TEXT_UNDERLINE_OFFSET),
                             em, &offset))
        return base_y + offset + thick / 2.0;
    return base_y + thick * 1.5;
}

static void
paint_dashed_decoration_piece(cairo_t *cr, const dashed_decoration *d,
                              NsPangoLayoutLine *line, int seg0, int seg1,
                              double dx, double dy)
{
    const ns_inline_attr *r = d->attr;
    gboolean dotted = strcmp(d->keyword, "dotted") == 0;
    gboolean solid = strcmp(d->keyword, "solid") == 0;
    int xa = 0, xb = 0;
    ns_pango_layout_line_index_to_x(line, seg0, FALSE, &xa);
    ns_pango_layout_line_index_to_x(line, seg1, FALSE, &xb);
    double x0 = dx + d->text_x + (double)MIN(xa, xb) / NS_PANGO_SCALE;
    double x1 = dx + d->text_x + (double)MAX(xa, xb) / NS_PANGO_SCALE;
    double em = r->font_size_px > 0 ? r->font_size_px : 16.0;
    double thick = decoration_thickness_px(r, d->box->style, em);
    double uy = dy + dashed_decoration_y(r, d->box->style, d->base_y, em, thick);
    const ns_css_value *dc =
        r->style ? r->style->values[NS_CSS_TEXT_DECORATION_COLOR] : NULL;
    if (!dc && r->style) dc = r->style->values[NS_CSS_COLOR];
    cairo_save(cr);
    if (dc && dc->kind == NS_CSS_V_COLOR)
        cairo_set_source_rgba(cr, dc->u.color.r / 255.0,
                              dc->u.color.g / 255.0,
                              dc->u.color.b / 255.0,
                              dc->u.color.a / 255.0);
    else
        cairo_set_source_rgba(cr, d->base.r, d->base.g, d->base.b, d->base.a);
    cairo_set_line_width(cr, thick);
    double dash[2] = { dotted ? thick : thick * 3.0,
                       dotted ? thick * 1.6 : thick * 2.5 };
    cairo_set_dash(cr, dash, solid ? 0 : 2, 0);
    cairo_set_line_cap(cr, dotted ? CAIRO_LINE_CAP_ROUND
                                  : CAIRO_LINE_CAP_BUTT);
    cairo_move_to(cr, x0, uy);
    cairo_line_to(cr, x1, uy);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void
paint_dashed_decoration(cairo_t *cr, const dashed_decoration *d,
                        const inline_layers *L, NsPangoLayoutLine *line,
                        guint line_no, int seg0, int seg1)
{
    if (!L || line_no >= L->lines->len) {
        paint_dashed_decoration_piece(cr, d, line, seg0, seg1, 0, 0);
        return;
    }
    const inline_layer_line *g =
        &g_array_index(L->lines, inline_layer_line, line_no);
    for (guint k = g->first_seg; k < g->first_seg + g->n_segs; k++) {
        const inline_layer_seg *seg =
            &g_array_index(L->segs, inline_layer_seg, k);
        int lo = MAX(seg0, (int)seg->lo), hi = MIN(seg1, (int)seg->hi);
        if (lo >= hi) continue;
        double dx, dy;
        inline_layer_offset(d->box, L, seg->layer, &dx, &dy);
        paint_dashed_decoration_piece(cr, d, line, lo, hi, dx, dy);
    }
}

static void
paint_inline_dashed_decorations(cairo_t *cr, const ns_box *b, NsPangoLayout *layout,
                               double text_x, double y_origin,
                               const ns_style *s, rgba base)
{
    if (!b->attrs)
        return;
    gboolean any = FALSE;
    for (guint i = 0; i < b->attrs->len; i++) {
        const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
        if ((r->kind == NS_INLINE_UNDERLINE ||
             r->kind == NS_INLINE_STRIKETHROUGH ||
             r->kind == NS_INLINE_OVERLINE) && underline_dash_style(r, s)) {
            any = TRUE;
            break;
        }
    }
    if (!any)
        return;
    NsPangoLayoutIter *iter = ns_pango_layout_get_iter(layout);
    if (!iter)
        return;
    const inline_layers *L = inline_layers_of(b, layout);
    int line_no = -1;
    do {
        line_no++;
        NsPangoLayoutLine *line = ns_pango_layout_iter_get_line_readonly(iter);
        if (!line)
            continue;
        double base_y = y_origin +
            (double)ns_pango_layout_iter_get_baseline(iter) / NS_PANGO_SCALE;
        int line_start = line->start_index;
        int line_end = line->start_index + line->length;
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *r =
                &g_array_index(b->attrs, ns_inline_attr, i);
            if (r->kind != NS_INLINE_UNDERLINE &&
                r->kind != NS_INLINE_STRIKETHROUGH &&
                r->kind != NS_INLINE_OVERLINE)
                continue;
            const char *kw = underline_dash_style(r, s);
            if (!kw)
                continue;
            int rstart = (int)r->start, rend = (int)(r->start + r->len);
            int seg0 = rstart > line_start ? rstart : line_start;
            int seg1 = rend < line_end ? rend : line_end;
            if (seg0 >= seg1)
                continue;
            dashed_decoration d = { b, r, kw, text_x, base_y, base };
            paint_dashed_decoration(cr, &d, L, line, (guint)line_no, seg0,
                                    seg1);
        }
    } while (ns_pango_layout_iter_next_line(iter));
    ns_pango_layout_iter_free(iter);
}

static void
ns_box_blur_a8(unsigned char *data, int w, int h, int stride, int radius)
{
    if (radius < 1 || w <= 0 || h <= 0) return;
    int win = 2 * radius + 1;
    unsigned char *tmp = g_malloc((gsize)stride * h);
    for (int y = 0; y < h; y++) {
        unsigned char *row = data + (gsize)y * stride;
        unsigned char *out = tmp + (gsize)y * stride;
        int sum = 0;
        for (int k = -radius; k <= radius; k++) {
            int xi = k < 0 ? 0 : (k >= w ? w - 1 : k);
            sum += row[xi];
        }
        for (int x = 0; x < w; x++) {
            out[x] = (unsigned char)(sum / win);
            int xo = x - radius; xo = xo < 0 ? 0 : (xo >= w ? w - 1 : xo);
            int xi = x + radius + 1; xi = xi < 0 ? 0 : (xi >= w ? w - 1 : xi);
            sum += row[xi] - row[xo];
        }
    }
    for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int k = -radius; k <= radius; k++) {
            int yi = k < 0 ? 0 : (k >= h ? h - 1 : k);
            sum += tmp[(gsize)yi * stride + x];
        }
        for (int y = 0; y < h; y++) {
            data[(gsize)y * stride + x] = (unsigned char)(sum / win);
            int yo = y - radius; yo = yo < 0 ? 0 : (yo >= h ? h - 1 : yo);
            int yi = y + radius + 1; yi = yi < 0 ? 0 : (yi >= h ? h - 1 : yi);
            sum += tmp[(gsize)yi * stride + x] - tmp[(gsize)yo * stride + x];
        }
    }
    g_free(tmp);
}

typedef struct text_shadow_geom {
    int blur, blur_s, pad, mw, mh;
    double ds;
} text_shadow_geom;

static void
text_shadow_geom_for(const ns_css_shadow *sh, double w, double h,
                     text_shadow_geom *g)
{
    g->blur = MAX((int)(sh->blur + 0.5), 0);
    g->ds = CLAMP(g->blur / 3.0, 1.0, 4.0);
    g->blur_s = (int)(g->blur / g->ds + 0.5);
    if (g->blur > 0 && g->blur_s < 1) g->blur_s = 1;
    g->pad = g->blur_s * 3 + 2;
    g->mw = (int)ceil(w / g->ds) + 2 * g->pad;
    g->mh = (int)ceil(h / g->ds) + 2 * g->pad;
}

static cairo_surface_t *
text_shadow_mask(const inline_text_src *src, const text_shadow_geom *g,
                 double x0, double y0)
{
    cairo_surface_t *mask =
        cairo_image_surface_create(CAIRO_FORMAT_A8, g->mw, g->mh);
    if (cairo_surface_status(mask) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(mask);
        return NULL;
    }
    cairo_t *mcr = cairo_create(mask);
    cairo_scale(mcr, 1.0 / g->ds, 1.0 / g->ds);
    cairo_move_to(mcr, g->pad * g->ds - x0, g->pad * g->ds - y0);
    inline_text_src_show(mcr, src);
    cairo_destroy(mcr);
    cairo_surface_flush(mask);
    if (g->blur > 0) {
        unsigned char *data = cairo_image_surface_get_data(mask);
        int stride = cairo_image_surface_get_stride(mask);
        ns_box_blur_a8(data, g->mw, g->mh, stride, g->blur_s);
        ns_box_blur_a8(data, g->mw, g->mh, stride, g->blur_s);
        cairo_surface_mark_dirty(mask);
    }
    return mask;
}

static void
paint_text_shadow_mask(cairo_t *cr, cairo_surface_t *mask, double ds,
                       double ox, double oy)
{
    if (ds == 1.0) {
        cairo_mask_surface(cr, mask, ox, oy);
        return;
    }
    cairo_pattern_t *mp = cairo_pattern_create_for_surface(mask);
    cairo_matrix_t m;
    cairo_matrix_init_scale(&m, 1.0 / ds, 1.0 / ds);
    cairo_matrix_translate(&m, -ox, -oy);
    cairo_pattern_set_matrix(mp, &m);
    cairo_mask(cr, mp);
    cairo_pattern_destroy(mp);
}

static void
paint_text_shadow_layer(cairo_t *cr, const inline_text_src *src, double x,
                        double y, const ns_css_shadow *sh)
{
    double x0, y0, w, h;
    if (!inline_text_src_extents(src, &x0, &y0, &w, &h)) return;
    text_shadow_geom g;
    text_shadow_geom_for(sh, w, h, &g);
    gboolean direct = g.mw > 4096 || g.mh > 4096;
    cairo_surface_t *mask = direct ? NULL : text_shadow_mask(src, &g, x0, y0);
    if (!direct && !mask) return;
    cairo_save(cr);
    cairo_set_source_rgba(cr, sh->r / 255.0, sh->g / 255.0,
                          sh->b / 255.0, sh->a / 255.0);
    if (mask) {
        paint_text_shadow_mask(cr, mask, g.ds,
                               x + sh->x - g.pad * g.ds + x0,
                               y + sh->y - g.pad * g.ds + y0);
        cairo_surface_destroy(mask);
    } else {
        cairo_move_to(cr, x + sh->x, y + sh->y);
        inline_text_src_show(cr, src);
    }
    cairo_restore(cr);
}

static void
spell_underline_range(NsPangoAttrList *attrs, const char *t,
                      gsize rstart, gsize rend)
{
    const char *p = t + rstart;
    const char *end = t + rend;
    while (p < end) {
        if (!g_unichar_isalpha(g_utf8_get_char(p))) {
            p = g_utf8_next_char(p);
            continue;
        }
        const char *start = p;
        const char *q = p;
        int alpha = 0;
        while (q < end) {
            gunichar c = g_utf8_get_char(q);
            const char *nx = g_utf8_next_char(q);
            if (g_unichar_isalpha(c)) { alpha++; q = nx; continue; }
            if ((c == '\'' || c == 0x2019) && nx < end &&
                g_unichar_isalpha(g_utf8_get_char(nx))) { q = nx; continue; }
            break;
        }
        gsize bstart = (gsize)(start - t);
        gsize blen = (gsize)(q - start);
        if (alpha >= 2 && !ns_spell_word_ok(start, (gssize)blen, NULL)) {
            NsPangoAttribute *u = ns_pango_attr_underline_new(NS_PANGO_UNDERLINE_ERROR);
            u->start_index = (guint)bstart;
            u->end_index = (guint)(bstart + blen);
            ns_pango_attr_list_insert(attrs, u);
            NsPangoAttribute *col = ns_pango_attr_underline_color_new(0xffff, 0x1000, 0x1000);
            col->start_index = (guint)bstart;
            col->end_index = (guint)(bstart + blen);
            ns_pango_attr_list_insert(attrs, col);
        }
        p = q;
    }
}

static void
paint_spell_underlines(NsPangoAttrList *attrs, const ns_box *b)
{
    if (!attrs || !b || !b->text) return;
    if (!ns_spell_available()) return;
    gsize tlen = strlen(b->text);
    gboolean ranges_known = FALSE;
    if (b->attrs) {
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *a =
                &g_array_index(b->attrs, ns_inline_attr, i);
            if (a->kind == NS_INLINE_INPUT_FIELD ||
                a->kind == NS_INLINE_INPUT_FIELD_FOCUSED)
                ranges_known = TRUE;
            if (a->kind != NS_INLINE_SPELLCHECK) continue;
            gsize s = a->start, e = a->start + a->len;
            if (e > tlen) e = tlen;
            if (s < e) {
                spell_underline_range(attrs, b->text, s, e);
                ranges_known = TRUE;
            }
        }
    }
    if (ranges_known) return;
    const ns_node *owner = NULL;
    for (const ns_box *bx = b; bx && !owner; bx = bx->parent)
        owner = bx->dom;
    const ns_node *host = owner ? ns_node_spellcheck_host(owner) : NULL;
    if (host && ns_node_spellcheck_armed(host))
        spell_underline_range(attrs, b->text, 0, tlen);
}

void
ns_paint_drop_box_cache(ns_box *box)
{
    if (box && box->paint_layout) {
        g_object_unref(box->paint_layout);
        box->paint_layout = NULL;
    }
}

static NsPangoLayout *
inline_layout_begin(const ns_box *b, const ns_style *s,
                    NsPangoAttrList **attrs_out)
{
    NsPangoLayout *layout = paint_create_layout();
    ns_paint_apply_inline_font(layout, s);

    if (ns_style_is_nowrap(s) && !ns_paint_text_ellipsizes(s))
        ns_pango_layout_set_width(layout, -1);
    else
        ns_pango_layout_set_width(layout, (int)(b->content_width * NS_PANGO_SCALE));
    ns_pango_layout_set_wrap(layout, ns_paint_wrap_mode_for(s));
    if (!(b->inline_atomics && b->inline_atomics->len > 0))
        ns_paint_apply_css_line_spacing(layout, s);
    {
        double ti = ns_inline_text_indent_px(b, s, b->content_width);
        if (ti > 0)
            ns_pango_layout_set_indent(layout, (int)(ti * NS_PANGO_SCALE));
    }
    if (ns_paint_text_ellipsizes(s))
        ns_pango_layout_set_ellipsize(layout, NS_PANGO_ELLIPSIZE_END);
    {
        const ns_css_value *lc = s ? s->values[NS_CSS_LINE_CLAMP] : NULL;
        if (lc && lc->kind == NS_CSS_V_LENGTH && lc->u.length.v >= 1) {
            ns_pango_layout_set_height(layout, -(int)lc->u.length.v);
            ns_pango_layout_set_ellipsize(layout, NS_PANGO_ELLIPSIZE_END);
        }
    }
    ns_paint_layout_set_inline_text(layout, b->text);

    NsPangoAttrList *attrs = ns_pango_attr_list_new();
    ns_paint_apply_i18n(layout, attrs, b);
    ns_paint_apply_font_features(attrs, s, 0, G_MAXUINT);
    ns_inline_apply_atomic_shapes(attrs, b);
    *attrs_out = attrs;
    return layout;
}

static NsPangoLayout *
paint_inline_make_layout(const ns_box *b, const ns_style *s,
                         const char *highlight)
{
    NsPangoAttrList *attrs = NULL;
    NsPangoLayout *layout = inline_layout_begin(b, s, &attrs);
    ns_inline_apply_spacing(attrs, s, b->text);
    if (b->attrs) {
        for (gint ii = (gint)b->attrs->len - 1; ii >= 0; ii--) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, (guint)ii);
            NsPangoAttribute *a = ns_paint_inline_font_attr(r);
            switch (r->kind) {
            case NS_INLINE_UNDERLINE: {
                if (underline_dash_style(r, s))
                    break;
                const ns_css_value *cv = decoration_color_of(r, s);
                if (cv && cv->u.color.a == 0)
                    break;
                NsPangoUnderline ul = NS_PANGO_UNDERLINE_SINGLE;
                const char *kw = decoration_style_of(r, s);
                if (kw) {
                    if (strcmp(kw, "double") == 0) ul = NS_PANGO_UNDERLINE_DOUBLE;
                    else if (strcmp(kw, "wavy") == 0) ul = NS_PANGO_UNDERLINE_ERROR;
                }
                a = ns_pango_attr_underline_new(ul);
                if (cv)
                    attr_insert_range(attrs, ns_pango_attr_underline_color_new(
                        (guint16)(cv->u.color.r * 0x101),
                        (guint16)(cv->u.color.g * 0x101),
                        (guint16)(cv->u.color.b * 0x101)), r->start, r->len);
                break;
            }
            case NS_INLINE_OVERLINE: {
                if (underline_dash_style(r, s))
                    break;
                const ns_css_value *cv = decoration_color_of(r, s);
                if (cv && cv->u.color.a == 0)
                    break;
                a = ns_pango_attr_overline_new(NS_PANGO_OVERLINE_SINGLE);
                if (cv)
                    attr_insert_range(attrs, ns_pango_attr_overline_color_new(
                        (guint16)(cv->u.color.r * 0x101),
                        (guint16)(cv->u.color.g * 0x101),
                        (guint16)(cv->u.color.b * 0x101)), r->start, r->len);
                break;
            }
            case NS_INLINE_STRIKETHROUGH: {
                if (underline_dash_style(r, s))
                    break;
                const ns_css_value *cv = decoration_color_of(r, s);
                if (cv && cv->u.color.a == 0)
                    break;
                a = ns_pango_attr_strikethrough_new(TRUE);
                if (cv)
                    attr_insert_range(attrs, ns_pango_attr_strikethrough_color_new(
                        (guint16)(cv->u.color.r * 0x101),
                        (guint16)(cv->u.color.g * 0x101),
                        (guint16)(cv->u.color.b * 0x101)), r->start, r->len);
                break;
            }
            case NS_INLINE_INPUT_FIELD:
            case NS_INLINE_INPUT_FIELD_FOCUSED:
            case NS_INLINE_BUTTON: {
                gboolean ta = r->dom && r->dom->name &&
                              strcmp(r->dom->name, "textarea") == 0;
                if (!ta)
                    attr_insert_range(attrs,
                        ns_pango_attr_allow_breaks_new(FALSE),
                        r->start, r->len);
                break;
            }
            case NS_INLINE_CHECKBOX:
            case NS_INLINE_CHECKBOX_CHECKED:
            case NS_INLINE_RADIO:
            case NS_INLINE_RADIO_CHECKED:
                attr_insert_range(attrs,
                    ns_pango_attr_foreground_alpha_new(1),
                    r->start, r->len);
                break;
            case NS_INLINE_COLOR:
                a = ns_pango_attr_foreground_new(
                    (guint16)(r->r * 0x101),
                    (guint16)(r->g * 0x101),
                    (guint16)(r->b * 0x101));
                if (r->a < 255)
                    attr_insert_range(attrs,
                        ns_pango_attr_foreground_alpha_new(
                            r->a ? (guint16)(r->a * 0x101) : 1),
                        r->start, r->len);
                break;
            case NS_INLINE_BG_COLOR:
                if (r->a == 0) break;
                a = ns_pango_attr_background_new(
                    (guint16)(r->r * 0x101),
                    (guint16)(r->g * 0x101),
                    (guint16)(r->b * 0x101));
                if (r->a < 255)
                    attr_insert_range(attrs,
                        ns_pango_attr_background_alpha_new(
                            (guint16)(r->a * 0x101)),
                        r->start, r->len);
                break;
            case NS_INLINE_SELECTION:
                attr_insert_range(attrs,
                    ns_pango_attr_background_new(0xb400, 0xd500, 0xfe00),
                    r->start, r->len);
                attr_insert_range(attrs,
                    ns_pango_attr_foreground_new(0x0000, 0x0000, 0x0000),
                    r->start, r->len);
                break;
            default:
                break;
            }
            if (r->kind == NS_INLINE_UNDERLINE ||
                r->kind == NS_INLINE_OVERLINE ||
                r->kind == NS_INLINE_STRIKETHROUGH)
                decoration_insert_around_atomics(attrs, a, b->text,
                                                 r->start, r->len);
            else
                attr_insert_range(attrs, a, r->start, r->len);
        }
    }
    if (highlight && *highlight) {
        gsize text_len = strlen(b->text);
        gsize needle_len = strlen(highlight);
        gsize pos = 0;
        gboolean is_active = (b == g_search_active_box);
        guint16 br = 0xffff;
        guint16 bg = is_active ? 0xff00 : 0xee00;
        guint16 bb = is_active ? 0x6600 : 0xb000;
        while ((pos = find_ci_substring(b->text, text_len,
                                        highlight, needle_len, pos)) != (gsize)-1) {
            attr_insert_range(attrs,
                ns_pango_attr_background_new(br, bg, bb),
                pos, needle_len);
            pos += needle_len > 0 ? needle_len : 1;
        }
    }
    if (s && s->first_line && b->parent && b->parent->first_child == b) {
        NsPangoLayoutLine *line0 = ns_pango_layout_get_line_readonly(layout, 0);
        if (line0 && line0->length > 0)
            apply_first_line_attrs(attrs, s->first_line,
                                   (guint)line0->start_index,
                                   (guint)(line0->start_index + line0->length));
    }
    paint_spell_underlines(attrs, b);
    ns_inline_layout_set_attrs(layout, attrs, b);
    inline_layers_attach(b, layout);
    ns_pango_attr_list_unref(attrs);

    ns_paint_apply_text_align(layout, s);
    apply_nowrap_align_width(layout, b);
    ns_paint_start_align_overflow(layout);
    return layout;
}

static GHashTable *g_paint_sel_runs;

static const ns_selection_run *
paint_selection_run(const ns_box *b)
{
    if (!g_paint_sel_runs) return NULL;
    return g_hash_table_lookup(g_paint_sel_runs, (gpointer)b);
}

typedef struct sel_rect { double x, y, w, h; } sel_rect;

static GArray *
paint_selection_rects(NsPangoLayout *layout, double ox, double oy,
                      const ns_selection_run *run)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(sel_rect));
    NsPangoLayoutIter *iter = ns_pango_layout_get_iter(layout);
    do {
        NsPangoLayoutLine *line = ns_pango_layout_iter_get_line_readonly(iter);
        if (!line) continue;
        int line_start = line->start_index;
        int line_end   = line_start + line->length;
        int s = (int)run->start > line_start ? (int)run->start : line_start;
        int e = (int)run->end   < line_end   ? (int)run->end   : line_end;
        if (s >= e) continue;
        NsPangoRectangle ext;
        ns_pango_layout_iter_get_line_extents(iter, NULL, &ext);
        int *ranges = NULL;
        int n_ranges = 0;
        ns_pango_layout_line_get_x_ranges(line, s, e, &ranges, &n_ranges);
        for (int i = 0; i < n_ranges; i++) {
            int x0 = ranges[i * 2], x1 = ranges[i * 2 + 1];
            if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
            sel_rect r;
            r.x = ox + (double)x0 / NS_PANGO_SCALE;
            r.y = oy + (double)ext.y / NS_PANGO_SCALE;
            r.w = (double)(x1 - x0) / NS_PANGO_SCALE;
            r.h = (double)ext.height / NS_PANGO_SCALE;
            if (r.w < 1.0) r.w = 1.0;
            if (r.h < 1.0) r.h = 1.0;
            g_array_append_val(out, r);
        }
        g_free(ranges);
    } while (ns_pango_layout_iter_next_line(iter));
    ns_pango_layout_iter_free(iter);
    if (out->len == 0) {
        g_array_free(out, TRUE);
        return NULL;
    }
    return out;
}

static gboolean
selection_pseudo_color(const ns_box *b, ns_css_prop prop, rgba *out)
{
    const ns_style *ssel = NULL;
    for (const ns_box *p = b; p && !ssel; p = p->parent)
        if (p->style) ssel = p->style->selection;
    if (!ssel) return FALSE;
    const ns_css_value *v = ssel->values[prop];
    if (!v || v->kind != NS_CSS_V_COLOR) return FALSE;
    out->r = v->u.color.r / 255.0;
    out->g = v->u.color.g / 255.0;
    out->b = v->u.color.b / 255.0;
    out->a = v->u.color.a / 255.0;
    return TRUE;
}

static void
inline_seg_selection_rects(GArray *out, const inline_layers *L,
                           const inline_layer_seg *seg, double ox, double oy,
                           const ns_selection_run *run)
{
    gsize s = MAX(seg->lo, (gsize)run->start);
    gsize e = MIN(seg->hi, (gsize)run->end);
    if (s >= e) return;
    const inline_layer_line *g =
        &g_array_index(L->lines, inline_layer_line, seg->line);
    int *ranges = NULL;
    int n_ranges = 0;
    ns_pango_layout_line_get_x_ranges(g->line, (int)s, (int)e, &ranges,
                                      &n_ranges);
    for (int i = 0; i < n_ranges; i++) {
        int x0 = MIN(ranges[i * 2], ranges[i * 2 + 1]);
        int x1 = MAX(ranges[i * 2], ranges[i * 2 + 1]);
        sel_rect r = { ox + (double)x0 / NS_PANGO_SCALE,
                       oy + (double)g->top / NS_PANGO_SCALE,
                       MAX((double)(x1 - x0) / NS_PANGO_SCALE, 1.0),
                       MAX((double)g->height / NS_PANGO_SCALE, 1.0) };
        g_array_append_val(out, r);
    }
    g_free(ranges);
}

static GArray *
inline_selection_rects(const inline_text_src *src, double ox, double oy,
                       const ns_selection_run *run)
{
    const inline_layers *L = src->layers;
    if (!L) return paint_selection_rects(src->layout, ox, oy, run);
    GArray *out = g_array_new(FALSE, FALSE, sizeof(sel_rect));
    const inline_bucket *bk = &L->seg_order;
    for (guint k = bk->start[src->layer]; k < bk->start[src->layer + 1]; k++)
        inline_seg_selection_rects(
            out, L, &g_array_index(L->segs, inline_layer_seg, bk->order[k]),
            ox, oy, run);
    if (out->len > 0) return out;
    g_array_free(out, TRUE);
    return NULL;
}

static void
paint_selection_background(cairo_t *cr, const ns_box *b,
                           const inline_text_src *src, double ox, double oy,
                           const ns_selection_run *run)
{
    GArray *rects = inline_selection_rects(src, ox, oy, run);
    if (!rects) return;
    rgba bg = { 0.20, 0.40, 0.85, 0.30 };
    selection_pseudo_color(b, NS_CSS_BACKGROUND_COLOR, &bg);
    cairo_save(cr);
    set_source_rgba(cr, bg);
    for (guint i = 0; i < rects->len; i++) {
        const sel_rect *r = &g_array_index(rects, sel_rect, i);
        cairo_rectangle(cr, r->x, r->y, r->w, r->h);
    }
    cairo_fill(cr);
    cairo_restore(cr);
    g_array_free(rects, TRUE);
}

static void
paint_selection_foreground(cairo_t *cr, const ns_box *b,
                           const inline_text_src *src, double ox, double oy,
                           const ns_selection_run *run)
{
    rgba fg;
    if (!selection_pseudo_color(b, NS_CSS_COLOR, &fg)) return;
    GArray *rects = inline_selection_rects(src, ox, oy, run);
    if (!rects) return;
    cairo_save(cr);
    for (guint i = 0; i < rects->len; i++) {
        const sel_rect *r = &g_array_index(rects, sel_rect, i);
        cairo_rectangle(cr, r->x, r->y, r->w, r->h);
    }
    cairo_clip(cr);
    set_source_rgba(cr, fg);
    cairo_move_to(cr, ox, oy);
    inline_text_src_show(cr, src);
    cairo_restore(cr);
    g_array_free(rects, TRUE);
}

static double
inline_line_offset(const ns_box *b, guint i, double line_h, double pango_h)
{
    if (b->atomic_line_offsets && i < b->atomic_line_offsets->len)
        return g_array_index(b->atomic_line_offsets, double, i);
    return (line_h - pango_h) / 2.0;
}

static gboolean
paint_inline_lines_at_layout_heights(cairo_t *cr, const ns_box *b,
                                     NsPangoLayout *layout, double text_x)
{
    const GArray *heights = b->atomic_line_heights;
    if (!heights || heights->len < 2 ||
        (guint)ns_pango_layout_get_line_count(layout) != heights->len)
        return FALSE;
    NsPangoLayoutIter *it = ns_pango_layout_get_iter(layout);
    double line_top = b->y;
    guint i = 0;
    do {
        NsPangoLayoutLine *line = ns_pango_layout_iter_get_line_readonly(it);
        NsPangoRectangle logical;
        ns_pango_layout_iter_get_line_extents(it, NULL, &logical);
        double line_h = g_array_index(heights, double, i);
        double pango_h = (double)logical.height / NS_PANGO_SCALE;
        double baseline = (double)(ns_pango_layout_iter_get_baseline(it) -
                                   logical.y) / NS_PANGO_SCALE;
        cairo_move_to(cr, text_x + (double)logical.x / NS_PANGO_SCALE,
                      line_top + inline_line_offset(b, i, line_h, pango_h) +
                      baseline);
        ns_pango_cairo_show_layout_line(cr, line);
        line_top += line_h;
        i++;
    } while (i < heights->len && ns_pango_layout_iter_next_line(it));
    ns_pango_layout_iter_free(it);
    return TRUE;
}

static double *
paint_inline_line_baselines(const ns_box *b, NsPangoLayout *layout,
                            double y_origin)
{
    int n = ns_pango_layout_get_line_count(layout);
    double *out = g_new0(double, n > 0 ? n : 1);
    const GArray *heights = b->atomic_line_heights;
    gboolean by_layout = heights && heights->len >= 2 &&
                         heights->len == (guint)n;
    NsPangoLayoutIter *it = ns_pango_layout_get_iter(layout);
    double line_top = b->y;
    int i = 0;
    do {
        NsPangoRectangle logical;
        ns_pango_layout_iter_get_line_extents(it, NULL, &logical);
        int baseline = ns_pango_layout_iter_get_baseline(it);
        if (by_layout) {
            double line_h = g_array_index(heights, double, i);
            out[i] = line_top +
                inline_line_offset(b, (guint)i, line_h,
                                   (double)logical.height / NS_PANGO_SCALE) +
                (double)(baseline - logical.y) / NS_PANGO_SCALE;
            line_top += line_h;
        } else {
            out[i] = y_origin + (double)baseline / NS_PANGO_SCALE;
        }
        i++;
    } while (i < n && ns_pango_layout_iter_next_line(it));
    ns_pango_layout_iter_free(it);
    return out;
}

static void
paint_inline_element_fragment(cairo_t *cr, const ns_inline_attr *r,
                              double x0, double x1, double y0, double y1,
                              gboolean open_start, gboolean open_end)
{
    double reach = (y1 - y0) + 64.0;
    double bx0 = open_start ? x0 - reach : x0;
    double bx1 = open_end ? x1 + reach : x1;
    cairo_save(cr);
    if (open_start || open_end) {
        cairo_rectangle(cr, x0, y0 - reach, x1 - x0, (y1 - y0) + 2 * reach);
        cairo_clip(cr);
    }
    paint_inline_css_chrome(cr, r, bx0, y0, bx1 - bx0, y1 - y0);
    cairo_restore(cr);
}

typedef struct inline_fragment {
    double x0, y0, x1, y1;
    gboolean open_left;
    gboolean open_right;
} inline_fragment;

typedef void (*inline_fragment_fn)(const inline_fragment *f, gpointer data);

static double
inline_border_px(const ns_style *s, ns_css_prop width, ns_css_prop style)
{
    if (!style_side_visible(s, width, style)) return 0;
    return length_or(s->values[width], 0);
}

gboolean
ns_paint_style_font_metrics(const ns_style *s, ns_css_font_metrics *m)
{
    double font_size = s ? length_or(s->values[NS_CSS_FONT_SIZE], 16) : 16;
    if (!s || font_size <= 0) return FALSE;
    const ns_css_value *fv = s->values[NS_CSS_FONT_FAMILY];
    const char *family = fv && fv->kind == NS_CSS_V_KEYWORD && fv->u.keyword
        ? fv->u.keyword : "sans-serif";
    gboolean italic = keyword_is(s->values[NS_CSS_FONT_STYLE], "italic") ||
                      keyword_is(s->values[NS_CSS_FONT_STYLE], "oblique");
    int weight = ns_css_font_weight_number(s->values[NS_CSS_FONT_WEIGHT], 400);
    ns_paint_font_metrics(family, font_size, weight, italic, m);
    return m->ascent_px + m->descent_px > 0;
}

static gboolean
inline_box_vertical_reach(const ns_style *s, double *above, double *below)
{
    if (!s) return FALSE;
    double ascent = 0, descent = 0;
    if (length_or(s->values[NS_CSS_FONT_SIZE], 16) > 0) {
        ns_css_font_metrics m = { 0 };
        if (!ns_paint_style_font_metrics(s, &m)) return FALSE;
        ascent = m.ascent_px;
        descent = m.descent_px;
    }
    *above = ascent + length_or(s->values[NS_CSS_PADDING_TOP], 0) +
             inline_border_px(s, NS_CSS_BORDER_TOP_WIDTH, NS_CSS_BORDER_TOP_STYLE);
    *below = descent + length_or(s->values[NS_CSS_PADDING_BOTTOM], 0) +
             inline_border_px(s, NS_CSS_BORDER_BOTTOM_WIDTH,
                              NS_CSS_BORDER_BOTTOM_STYLE);
    return TRUE;
}

static gboolean
inline_line_x_extent(NsPangoLayoutLine *line, gsize lo, gsize hi,
                     double *x0, double *x1)
{
    int *ranges = NULL;
    int n_ranges = 0;
    ns_pango_layout_line_get_x_ranges(line, (int)lo, (int)hi, &ranges,
                                      &n_ranges);
    if (n_ranges <= 0) {
        g_free(ranges);
        return FALSE;
    }
    int left = MIN(ranges[0], ranges[1]);
    int right = MAX(ranges[0], ranges[1]);
    for (int i = 1; i < n_ranges; i++) {
        left = MIN(left, MIN(ranges[2 * i], ranges[2 * i + 1]));
        right = MAX(right, MAX(ranges[2 * i], ranges[2 * i + 1]));
    }
    g_free(ranges);
    *x0 = (double)left / NS_PANGO_SCALE;
    *x1 = (double)right / NS_PANGO_SCALE;
    return TRUE;
}

typedef struct inline_range {
    gsize start;
    gsize len;
    const ns_style *box_style;
    double rise;
    double dx, dy;
} inline_range;

static inline_range
inline_range_for(const ns_box *b, gsize start, gsize len,
                 const ns_inline_attr *element)
{
    inline_range range = { start, len, NULL, 0, 0, 0 };
    if (!element) {
        ns_inline_offset_at(b, start, &range.dx, &range.dy);
        return range;
    }
    range.box_style = element->style;
    range.rise = element->rise_px;
    range.dx = element->rel_dx;
    range.dy = element->rel_dy;
    return range;
}

static void
inline_range_fragments(const ns_box *b, NsPangoLayout *layout, double y_origin,
                       const inline_range *range, inline_fragment_fn fn,
                       gpointer data)
{
    double above = 0, below = 0;
    gboolean own_box =
        inline_box_vertical_reach(range->box_style, &above, &below);
    gboolean rtl = range->box_style &&
        keyword_is(range->box_style->values[NS_CSS_DIRECTION], "rtl");
    double *baselines =
        own_box ? paint_inline_line_baselines(b, layout, y_origin) : NULL;
    gsize end = range->start + range->len;
    NsPangoLayoutIter *it = ns_pango_layout_get_iter(layout);
    int line_index = -1;
    do {
        line_index++;
        NsPangoLayoutLine *line = ns_pango_layout_iter_get_line_readonly(it);
        gsize line_start = (gsize)line->start_index;
        gsize line_end = line_start + (gsize)line->length;
        gsize lo = MAX(range->start, line_start);
        gsize hi = MIN(end, line_end);
        if (lo >= hi) continue;
        inline_fragment f;
        if (!inline_line_x_extent(line, lo, hi, &f.x0, &f.x1)) continue;
        if (own_box) {
            double baseline = baselines[line_index] - range->rise;
            f.y0 = baseline - above;
            f.y1 = baseline + below;
        } else {
            NsPangoRectangle lrect;
            ns_pango_layout_iter_get_line_extents(it, NULL, &lrect);
            f.y0 = y_origin + (double)lrect.y / NS_PANGO_SCALE;
            f.y1 = y_origin +
                (double)(lrect.y + lrect.height) / NS_PANGO_SCALE;
        }
        f.open_left = rtl ? hi < end : lo > range->start;
        f.open_right = rtl ? lo > range->start : hi < end;
        f.x0 += range->dx;
        f.x1 += range->dx;
        f.y0 += range->dy;
        f.y1 += range->dy;
        fn(&f, data);
    } while (ns_pango_layout_iter_next_line(it));
    ns_pango_layout_iter_free(it);
    g_free(baselines);
}

typedef struct inline_box_paint {
    cairo_t *cr;
    const ns_inline_attr *attr;
    double text_x;
} inline_box_paint;

static void
paint_inline_box_fragment(const inline_fragment *f, gpointer data)
{
    const inline_box_paint *p = data;
    paint_inline_element_fragment(p->cr, p->attr,
        snap_device_x(p->cr, p->text_x + f->x0),
        snap_device_x(p->cr, p->text_x + f->x1),
        snap_device_y(p->cr, f->y0), snap_device_y(p->cr, f->y1),
        f->open_left, f->open_right);
}

static void
paint_inline_element_box(cairo_t *cr, const ns_box *b, NsPangoLayout *layout,
                         double text_x, double y_origin,
                         const ns_inline_attr *r)
{
    gboolean clip_to_text = style_background_clips_to_text(r->style);
    if (clip_to_text) cairo_push_group(cr);
    inline_box_paint p = { cr, r, text_x };
    inline_range range = inline_range_for(b, r->start, r->len, r);
    inline_range_fragments(b, layout, y_origin, &range,
                           paint_inline_box_fragment, &p);
    if (!clip_to_text) return;
    cairo_pattern_t *background = cairo_pop_group(cr);
    cairo_save(cr);
    cairo_new_path(cr);
    layout_range_text_path(cr, layout, text_x, y_origin,
                           (int)r->start, (int)(r->start + r->len));
    cairo_clip(cr);
    cairo_set_source(cr, background);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_pattern_destroy(background);
}

static void
paint_inline_element_boxes(cairo_t *cr, const ns_box *b, NsPangoLayout *layout,
                           double text_x, double y_origin, guint layer)
{
    if (!b->attrs) return;
    const inline_layers *L = inline_layers_of(b, layout);
    if (!L) {
        for (guint i = b->attrs->len; i-- > 0;) {
            const ns_inline_attr *r =
                &g_array_index(b->attrs, ns_inline_attr, i);
            if (inline_attr_paints_box(r))
                paint_inline_element_box(cr, b, layout, text_x, y_origin, r);
        }
        return;
    }
    const inline_bucket *bk = &L->box_order;
    for (guint k = bk->start[layer]; k < bk->start[layer + 1]; k++)
        paint_inline_element_box(cr, b, layout, text_x, y_origin,
            &g_array_index(b->attrs, ns_inline_attr, bk->order[k]));
}

static double *
inline_layer_baselines(const ns_box *b, const inline_layers *L)
{
    const GArray *heights = b->atomic_line_heights;
    if (!heights || heights->len < 2 || heights->len != L->lines->len)
        return NULL;
    double *out = g_new(double, heights->len);
    double top = b->y;
    for (guint i = 0; i < heights->len; i++) {
        const inline_layer_line *g =
            &g_array_index(L->lines, inline_layer_line, i);
        double line_h = g_array_index(heights, double, i);
        out[i] = top + inline_line_offset(b, i, line_h,
                                          (double)g->height / NS_PANGO_SCALE) +
                 (double)(g->baseline - g->top) / NS_PANGO_SCALE;
        top += line_h;
    }
    return out;
}

typedef struct text_stroke {
    double width;
    rgba color;
} text_stroke;

static gboolean
style_text_stroke(const ns_style *s, text_stroke *out)
{
    double w = s ? length_or(s->values[NS_CSS_WEBKIT_TEXT_STROKE_WIDTH], 0) : 0;
    if (w <= 0 || inline_box_is_hidden(s)) return FALSE;
    const ns_css_value *c = s->values[NS_CSS_WEBKIT_TEXT_STROKE_COLOR];
    if (!c || c->kind != NS_CSS_V_COLOR) c = s->values[NS_CSS_COLOR];
    out->width = w;
    out->color = rgba_of(c, 0, 0, 0, 1);
    return out->color.a > 0;
}

static gsize
text_style_attr_rank(const ns_inline_attr *r)
{
    if (!r->style) return G_MAXSIZE;
    if (r->kind == NS_INLINE_GENERATED) return r->len * 2;
    return r->kind == NS_INLINE_ELEMENT ? r->len * 2 + 1 : G_MAXSIZE;
}

static gboolean
inline_text_has_stroke(const ns_box *b, const ns_style *base)
{
    text_stroke st;
    if (style_text_stroke(base, &st)) return TRUE;
    for (guint i = 0; b->attrs && i < b->attrs->len; i++) {
        const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
        if (text_style_attr_rank(r) != G_MAXSIZE &&
            style_text_stroke(r->style, &st))
            return TRUE;
    }
    return FALSE;
}

static const ns_style **
inline_byte_styles(const ns_box *b, const ns_style *base, gsize n)
{
    const ns_style **owner = g_new(const ns_style *, n + 1);
    gsize *span = g_new(gsize, n + 1);
    for (gsize k = 0; k <= n; k++) {
        owner[k] = base;
        span[k] = G_MAXSIZE;
    }
    for (guint i = 0; b->attrs && i < b->attrs->len; i++) {
        const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
        gsize rank = text_style_attr_rank(r);
        if (rank == G_MAXSIZE) continue;
        gsize end = MIN(n, r->start + r->len);
        for (gsize k = r->start; k < end; k++) {
            if (rank >= span[k]) continue;
            owner[k] = r->style;
            span[k] = rank;
        }
    }
    g_free(span);
    return owner;
}

typedef struct text_stroke_walk {
    cairo_t *cr;
    const ns_style **owner;
    gsize n;
    NsPangoGlyphString *one;
    const ns_style *style;
    gboolean active;
    text_stroke cur;
} text_stroke_walk;

static void
text_stroke_flush(text_stroke_walk *w)
{
    if (!w->active) return;
    set_source_rgba(w->cr, w->cur.color);
    cairo_set_line_width(w->cr, w->cur.width);
    cairo_set_line_join(w->cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_miter_limit(w->cr, 4.0);
    cairo_stroke(w->cr);
}

static gboolean
glyph_has_outline(const NsPangoGlyphInfo *gi)
{
    if (gi->attr.is_color || gi->glyph == NS_PANGO_GLYPH_EMPTY) return FALSE;
    return (gi->glyph & NS_PANGO_GLYPH_UNKNOWN_FLAG) == 0;
}

static void
text_stroke_glyph(text_stroke_walk *w, const NsPangoLayoutRun *run, int g,
                  double x, double y)
{
    if (!glyph_has_outline(&run->glyphs->glyphs[g])) return;
    gsize byte = (gsize)(run->item->offset + run->glyphs->log_clusters[g]);
    const ns_style *s = w->owner[MIN(byte, w->n)];
    if (s != w->style) {
        text_stroke_flush(w);
        w->style = s;
        w->active = style_text_stroke(s, &w->cur);
    }
    if (!w->active) return;
    w->one->glyphs[0] = run->glyphs->glyphs[g];
    w->one->log_clusters[0] = 0;
    cairo_move_to(w->cr, x, y);
    ns_pango_cairo_glyph_string_path(w->cr, run->item->analysis.font, w->one);
}

static void
text_stroke_run(const NsPangoLayoutRun *run, double x, double y,
                gpointer data)
{
    const NsPangoGlyphString *gs = run->glyphs;
    for (int g = 0; g < gs->num_glyphs; g++) {
        text_stroke_glyph(data, run, g, x, y);
        x += (double)gs->glyphs[g].geometry.width / NS_PANGO_SCALE;
    }
}

typedef void (*inline_run_visit)(const NsPangoLayoutRun *run, double x,
                                 double y, gpointer data);

static void
inline_runs_visit(GSList *first, const GSList *stop, double x, double y,
                  inline_run_visit fn, gpointer data)
{
    int off = 0;
    for (GSList *l = first; l && l != stop; l = l->next) {
        const NsPangoLayoutRun *run = l->data;
        fn(run, x + (double)(off + run->start_x_offset) / NS_PANGO_SCALE,
           y - (double)run->y_offset / NS_PANGO_SCALE, data);
        off += inline_run_advance(run);
    }
}

static void
inline_layer_runs_visit(const ns_box *b, const inline_text_src *src,
                        double ox, double oy, inline_run_visit fn,
                        gpointer data)
{
    const inline_layers *L = src->layers;
    double *baselines = inline_layer_baselines(b, L);
    const inline_bucket *bk = &L->seg_order;
    for (guint k = bk->start[src->layer]; k < bk->start[src->layer + 1]; k++) {
        const inline_layer_seg *seg =
            &g_array_index(L->segs, inline_layer_seg, bk->order[k]);
        const inline_layer_line *g =
            &g_array_index(L->lines, inline_layer_line, seg->line);
        double y = baselines ? baselines[seg->line]
                             : oy + (double)g->baseline / NS_PANGO_SCALE;
        inline_runs_visit(seg->first, seg->last->next,
                          ox + (double)(g->x + seg->x) / NS_PANGO_SCALE, y,
                          fn, data);
    }
    g_free(baselines);
}

static void
inline_lines_runs_visit(const ns_box *b, NsPangoLayout *layout,
                        double text_x, double y_origin, inline_run_visit fn,
                        gpointer data)
{
    double *baselines = paint_inline_line_baselines(b, layout, y_origin);
    int n = ns_pango_layout_get_line_count(layout);
    NsPangoLayoutIter *it = ns_pango_layout_get_iter(layout);
    int i = 0;
    do {
        NsPangoLayoutLine *line = ns_pango_layout_iter_get_line_readonly(it);
        NsPangoRectangle logical;
        ns_pango_layout_iter_get_line_extents(it, NULL, &logical);
        if (line)
            inline_runs_visit(line->runs, NULL,
                              text_x + (double)logical.x / NS_PANGO_SCALE,
                              baselines[i], fn, data);
        i++;
    } while (i < n && ns_pango_layout_iter_next_line(it));
    ns_pango_layout_iter_free(it);
    g_free(baselines);
}

static void
paint_inline_text_strokes(cairo_t *cr, const ns_box *b,
                          const inline_text_src *src, double text_x,
                          double y_origin)
{
    const ns_style *base = inherited_style(b);
    if (!inline_text_has_stroke(b, base)) return;
    gsize n = strlen(ns_pango_layout_get_text(src->layout));
    text_stroke_walk w = {
        .cr = cr, .owner = inline_byte_styles(b, base, n), .n = n,
        .one = ns_pango_glyph_string_new(),
    };
    ns_pango_glyph_string_set_size(w.one, 1);
    cairo_save(cr);
    cairo_new_path(cr);
    if (src->layers)
        inline_layer_runs_visit(b, src, text_x, y_origin, text_stroke_run, &w);
    else
        inline_lines_runs_visit(b, src->layout, text_x, y_origin,
                                text_stroke_run, &w);
    text_stroke_flush(&w);
    cairo_restore(cr);
    ns_pango_glyph_string_free(w.one);
    g_free(w.owner);
}

static void
paint_inline_text(cairo_t *cr, const ns_box *b, const inline_text_src *src,
                  double text_x, double y_origin, rgba color)
{
    cairo_save(cr);
    set_source_rgba(cr, color);
    if (src->layers) {
        double *baselines = inline_layer_baselines(b, src->layers);
        inline_layer_show(cr, src->layers, src->layer, text_x, y_origin,
                          baselines);
        g_free(baselines);
    } else if (!paint_inline_lines_at_layout_heights(cr, b, src->layout,
                                                     text_x)) {
        cairo_move_to(cr, text_x, y_origin);
        ns_pango_cairo_show_layout(cr, src->layout);
    }
    cairo_restore(cr);
    paint_inline_text_strokes(cr, b, src, text_x, y_origin);
}

static void
paint_inline_text_shadows(cairo_t *cr, const ns_box *b, const ns_style *s,
                          NsPangoLayout *layout, double text_x,
                          double y_origin)
{
    const ns_css_value *v = s ? s->values[NS_CSS_TEXT_SHADOW] : NULL;
    if (!v || v->kind != NS_CSS_V_SHADOW) return;
    inline_text_src src = inline_text_src_of(b, layout, k_inline_all_layers);
    for (int si = v->u.shadow.n - 1; si >= 0; si--)
        paint_text_shadow_layer(cr, &src, text_x, y_origin,
                                &v->u.shadow.s[si]);
}

static void
paint_inline_atomic(cairo_t *cr, const ns_box *b, NsPangoLayout *layout,
                    double text_x, ns_inline_atomic *a, const char *highlight)
{
    if (!a->box) return;
    NsPangoRectangle pos;
    ns_pango_layout_index_to_pos(layout, (int)a->byte_off, &pos);
    double sx = text_x + (double)pos.x / NS_PANGO_SCALE;
    double sy = b->atomic_line_heights
        ? a->box->y - a->box->rel_dy
        : b->y + (double)pos.y / NS_PANGO_SCALE;
    a->owner_offset_x = sx - b->x;
    a->owner_offset_y = sy - b->y;
    cairo_save(cr);
    cairo_translate(cr, sx + a->box->rel_dx - a->box->x,
                    sy + a->box->rel_dy - a->box->y);
    g_paint_no_cull++;
    const ns_box *saved_flush = g_paint_flush_box;
    if (g_paint_defer_depth == 0 || !ns_box_is_upper_layer(a->box))
        g_paint_flush_box = a->box;
    paint_walk(cr, a->box, highlight);
    g_paint_flush_box = saved_flush;
    g_paint_no_cull--;
    cairo_restore(cr);
}

static void
paint_inline_atomics(cairo_t *cr, const ns_box *b, NsPangoLayout *layout,
                     double text_x, const char *highlight, guint layer)
{
    GArray *atomics = b->inline_atomics;
    if (!atomics) return;
    const inline_layers *L = inline_layers_of(b, layout);
    if (!L) {
        for (guint i = 0; i < atomics->len; i++)
            paint_inline_atomic(cr, b, layout, text_x,
                &g_array_index(atomics, ns_inline_atomic, i), highlight);
        return;
    }
    const inline_bucket *bk = &L->atomic_order;
    for (guint k = bk->start[layer]; k < bk->start[layer + 1]; k++)
        paint_inline_atomic(cr, b, layout, text_x,
            &g_array_index(atomics, ns_inline_atomic, bk->order[k]),
            highlight);
}

static rgba
paint_inline_color(const ns_box *b, const ns_style *s)
{
    if (inline_box_is_hidden(s)) return (rgba){ 0, 0, 0, 0 };
    const ns_css_value *fill = s ? s->values[NS_CSS_WEBKIT_TEXT_FILL_COLOR] : NULL;
    if (fill && fill->kind == NS_CSS_V_COLOR)
        return rgba_of(fill, 0.07, 0.07, 0.07, 1);
    return rgba_anim(b, NS_CSS_ANIM_TARGET_COLOR,
                     s ? s->values[NS_CSS_COLOR] : NULL, 0.07, 0.07, 0.07, 1);
}

static double
paint_inline_text_x(const ns_box *b, const ns_style *s)
{
    double ti = ns_inline_text_indent_px(b, s, b->content_width);
    return b->x + (ti < 0 ? ti : 0);
}

static NsPangoLayout *
paint_inline_layout(const ns_box *b, const ns_style *s, const char *highlight)
{
    gboolean cacheable = !(highlight && *highlight);
    if (cacheable && b->paint_layout)
        return g_object_ref(b->paint_layout);
    NsPangoLayout *layout = paint_inline_make_layout(b, s, highlight);
    if (cacheable)
        ((ns_box *)b)->paint_layout = g_object_ref(layout);
    return layout;
}

static void
paint_box_text_path(cairo_t *cr, const ns_box *b, int depth)
{
    if (!b || depth > NS_DOM_MAX_DEPTH) return;
    if (b->kind == NS_BOX_INLINE && b->text && *b->text && !b->vertical_wm) {
        const ns_style *s = inherited_style(b);
        NsPangoLayout *layout = paint_inline_layout(b, s, NULL);
        cairo_move_to(cr, paint_inline_text_x(b, s),
                      b->y + ns_paint_inline_y_offset_for_layout(b, layout));
        ns_pango_cairo_layout_path(cr, layout);
        g_object_unref(layout);
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        paint_box_text_path(cr, c, depth + 1);
}

typedef struct inline_layer_place {
    double text_x, y_origin;
    rgba color;
} inline_layer_place;

static void
paint_inline_layer(cairo_t *cr, const ns_box *b, NsPangoLayout *layout,
                   guint layer, const inline_layer_place *at,
                   const char *highlight)
{
    const inline_layers *L = inline_layers_of(b, layout);
    const ns_inline_attr *owner = L ? inline_layer_owner(b, L, layer) : NULL;
    if (!owner) return;
    paint_inline_element_boxes(cr, b, layout, at->text_x, at->y_origin, layer);
    inline_text_src src = { b, layout, L, layer };
    const ns_selection_run *sel_run = paint_selection_run(b);
    cairo_save(cr);
    cairo_translate(cr, owner->rel_dx, owner->rel_dy);
    if (sel_run)
        paint_selection_background(cr, b, &src, at->text_x, at->y_origin,
                                   sel_run);
    paint_inline_text(cr, b, &src, at->text_x, at->y_origin, at->color);
    if (sel_run)
        paint_selection_foreground(cr, b, &src, at->text_x, at->y_origin,
                                   sel_run);
    cairo_restore(cr);
    paint_inline_atomics(cr, b, layout, at->text_x, highlight, layer);
}

static void paint_inline_defer_layers(cairo_t *cr, const ns_box *b,
                                      NsPangoLayout *layout,
                                      const inline_layer_place *at,
                                      const char *highlight);
static void paint_inline_negative_layers(cairo_t *cr, const ns_box *b,
                                         NsPangoLayout *layout,
                                         const inline_layer_place *at,
                                         const char *highlight);

static void
inline_shift_rect(const ns_box *b, gsize byte, double *x0, double *y0,
                  double *x1, double *y1)
{
    double dx, dy;
    ns_inline_offset_at(b, byte, &dx, &dy);
    *x0 += dx;
    *x1 += dx;
    *y0 += dy;
    *y1 += dy;
}

static void
paint_inline_ranges(cairo_t *cr, const ns_box *b, const ns_style *s,
                    NsPangoLayout *layout, double text_x, double y_origin)
{
    for (guint i = 0; i < b->attrs->len; i++) {
        const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
        double w, h, m[4];
        if (!ns_inline_range_geometry(r, b, &w, &h, m)) continue;
        const ns_style *rs = r->style ? r->style : s;
        const ns_css_value *ac = rs ? rs->values[NS_CSS_ACCENT_COLOR] : NULL;
        rgba accent = rgba_of(
            (ac && ac->kind == NS_CSS_V_COLOR) ? ac : NULL,
            0.0, 0.459, 1.0, 1);
        NsPangoRectangle r0;
        ns_pango_layout_index_to_pos(layout, (int)r->start, &r0);
        double x = text_x + (double)r0.x / NS_PANGO_SCALE + m[3];
        double y = y_origin +
            layout_baseline_at_index(layout, (int)r->start) - h;
        double dx, dy;
        ns_inline_offset_at(b, r->start, &dx, &dy);
        x += dx;
        y += dy;
        gboolean native = !(rs && keyword_is(rs->values[NS_CSS_APPEARANCE],
                                             "none"));
        double knob = 16.0;
        double frac = CLAMP(r->font_size_px, 0.0, 1.0);
        double usable = MAX(0.0, w - knob);
        double kx = x + knob / 2.0 + usable * frac;
        double cy = y + h / 2.0;
        cairo_save(cr);
        if (native) {
            double th = 4.0;
            corner_radii tr = corner_radii_uniform(th / 2.0);
            rounded_rect_path(cr, x + 1, cy - th / 2.0, MAX(0.0, w - 2), th, tr);
            cairo_set_source_rgb(cr, 0.80, 0.80, 0.82);
            cairo_fill(cr);
            rounded_rect_path(cr, x + 1, cy - th / 2.0, MAX(0.0, kx - x - 1), th, tr);
            cairo_set_source_rgba(cr, accent.r, accent.g, accent.b, accent.a);
            cairo_fill(cr);
        }
        cairo_new_sub_path(cr);
        cairo_arc(cr, kx, cy, knob / 2.0 - 0.5, 0, 2 * G_PI);
        cairo_set_source_rgba(cr, accent.r, accent.g, accent.b, accent.a);
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

static void
paint_inline(cairo_t *cr, const ns_box *b, const char *highlight)
{
    if (!b->text || !*b->text) return;
    const ns_style *s = inherited_style(b);
    rgba color = paint_inline_color(b, s);

    if (b->vertical_wm) {
        if (b->text_orient == 1) {
            char *stacked = ns_vertical_stack_text(b->text);
            NsPangoLayout *layout = paint_inline_make_layout(b, s, highlight);
            ns_pango_layout_set_attributes(layout, NULL);
            ns_pango_layout_set_width(layout, -1);
            ns_pango_layout_set_alignment(layout, NS_PANGO_ALIGN_CENTER);
            ns_pango_layout_set_text(layout, stacked, -1);
            g_free(stacked);
            cairo_save(cr);
            set_source_rgba(cr, color);
            cairo_move_to(cr, b->x, b->y);
            ns_pango_cairo_show_layout(cr, layout);
            cairo_restore(cr);
            g_object_unref(layout);
            return;
        }
        NsPangoLayout *layout = paint_inline_make_layout(b, s, highlight);
        ns_pango_layout_set_width(layout, -1);
        set_source_rgba(cr, color);
        NsPangoLayoutIter *it = ns_pango_layout_get_iter(layout);
        double acc = 0;
        do {
            NsPangoLayoutLine *line = ns_pango_layout_iter_get_line_readonly(it);
            NsPangoRectangle logical;
            ns_pango_layout_iter_get_line_extents(it, NULL, &logical);
            int baseline = ns_pango_layout_iter_get_baseline(it);
            double line_h = (double)logical.height / NS_PANGO_SCALE;
            double ascent = (double)(baseline - logical.y) / NS_PANGO_SCALE;
            double col_left = (b->vertical_wm == 2)
                ? b->x + acc
                : b->x + b->content_width - acc - line_h;
            cairo_save(cr);
            cairo_translate(cr, col_left + line_h, b->y);
            cairo_rotate(cr, G_PI / 2.0);
            cairo_move_to(cr, 0, ascent);
            ns_pango_cairo_show_layout_line(cr, line);
            cairo_restore(cr);
            acc += line_h;
        } while (ns_pango_layout_iter_next_line(it));
        ns_pango_layout_iter_free(it);
        g_object_unref(layout);
        return;
    }

    double text_x = paint_inline_text_x(b, s);
    NsPangoLayout *layout = paint_inline_layout(b, s, highlight);
    double y_offset = ns_paint_inline_y_offset_for_layout(b, layout);
    double y_origin = b->y + y_offset;
    inline_layer_place at = { text_x, y_origin, color };

    paint_inline_negative_layers(cr, b, layout, &at, highlight);
    paint_inline_element_boxes(cr, b, layout, text_x, y_origin, 0);

    if (b->attrs) {
        double opt_minx = 1e9, opt_maxx = -1e9, opt_miny = 1e9, opt_maxy = -1e9;
        int opt_count = 0, opt_nsel = 0;
        struct { double x0, y0, x1, y1; } opt_sel[64];
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
            if (r->kind != NS_INLINE_INPUT_FIELD &&
                r->kind != NS_INLINE_INPUT_FIELD_FOCUSED &&
                r->kind != NS_INLINE_BUTTON)
                continue;
            NsPangoRectangle r0, r1;
            ns_pango_layout_index_to_pos(layout, (int)r->start, &r0);
            ns_pango_layout_index_to_pos(layout,
                (int)(r->len > 0 ? r->start + r->len - 1 : r->start), &r1);
            if (r->dom && r->dom->name && strcmp(r->dom->name, "option") == 0) {
                double rx0 = text_x + (double)r0.x / NS_PANGO_SCALE;
                double rx1 = text_x + (double)(r1.x + r1.width) / NS_PANGO_SCALE;
                double ry0 = y_origin + (double)r0.y / NS_PANGO_SCALE;
                double ry1 = y_origin + (double)(r0.y + r0.height) / NS_PANGO_SCALE;
                if (rx0 < opt_minx) opt_minx = rx0;
                if (rx1 > opt_maxx) opt_maxx = rx1;
                if (ry0 < opt_miny) opt_miny = ry0;
                if (ry1 > opt_maxy) opt_maxy = ry1;
                opt_count++;
                if (ns_element_get_attr(r->dom, "selected") && opt_nsel < 64) {
                    opt_sel[opt_nsel].x0 = rx0; opt_sel[opt_nsel].y0 = ry0;
                    opt_sel[opt_nsel].x1 = rx1; opt_sel[opt_nsel].y1 = ry1;
                    opt_nsel++;
                }
                continue;
            }
            double bleed_x = r->box_w > 0 || r->box_h > 0 ? 0 : 10;
            double bleed_y = r->box_w > 0 || r->box_h > 0 ? 0 : 5;
            double x0 = text_x + (double)r0.x / NS_PANGO_SCALE - bleed_x;
            double y0 = y_origin + (double)r0.y / NS_PANGO_SCALE - bleed_y;
            double x1 = text_x + (double)(r1.x + r1.width) / NS_PANGO_SCALE + bleed_x;
            double y1 = y_origin + (double)(r0.y + r0.height) / NS_PANGO_SCALE + bleed_y;
            double css_w = ns_inline_attr_control_width(r, b);
            if ((r->kind == NS_INLINE_INPUT_FIELD ||
                 r->kind == NS_INLINE_INPUT_FIELD_FOCUSED) && r->dom) {
                const char *type = ns_element_get_attr(r->dom, "type");
                gboolean text_like = !type || !*type ||
                    g_ascii_strcasecmp(type, "text") == 0 ||
                    g_ascii_strcasecmp(type, "search") == 0 ||
                    g_ascii_strcasecmp(type, "email") == 0 ||
                    g_ascii_strcasecmp(type, "url") == 0 ||
                    g_ascii_strcasecmp(type, "tel") == 0 ||
                    g_ascii_strcasecmp(type, "number") == 0 ||
                    g_ascii_strcasecmp(type, "password") == 0;
                if (text_like && r->box_w <= 0) {
                    const char *sz = ns_element_get_attr(r->dom, "size");
                    int n = sz ? ns_parse_int(sz, 20, 4, 80) : 20;
                    NsPangoContext *pctx = ns_pango_layout_get_context(layout);
                    const NsPangoFontDescription *fd =
                        ns_pango_layout_get_font_description(layout);
                    if (!fd) fd = ns_pango_context_get_font_description(pctx);
                    NsPangoFontMetrics *fm =
                        ns_pango_context_get_metrics(pctx, fd, NULL);
                    int aw = ns_pango_font_metrics_get_approximate_char_width(fm);
                    ns_pango_font_metrics_unref(fm);
                    double cell = (double)aw / NS_PANGO_SCALE;
                    double want_w = cell * (double)n + 20.0;
                    double cur_w = x1 - x0;
                    if (want_w > cur_w) x1 = x0 + want_w;
                }
            }
            gboolean is_textarea = r->dom && r->dom->name &&
                                   strcmp(r->dom->name, "textarea") == 0;
            if (css_w <= 0 && r->kind == NS_INLINE_BUTTON) {
                double mnw = inline_control_css_min_width(r, b);
                if (mnw > 0 && x1 - x0 < mnw) {
                    double cx = (x0 + x1) / 2.0;
                    x0 = cx - mnw / 2.0;
                    x1 = cx + mnw / 2.0;
                }
            }
            if (css_w > 0) {
                x0 = text_x + (double)r0.x / NS_PANGO_SCALE;
                x1 = x0 + css_w;
            } else if ((r->kind == NS_INLINE_INPUT_FIELD ||
                        r->kind == NS_INLINE_INPUT_FIELD_FOCUSED) &&
                       b->content_width > 0 && b->parent &&
                       b->parent->dom == r->dom) {
                gsize tlen = b->text ? strlen(b->text) : 0;
                if (r->start <= 3 && r->start + r->len >= tlen) {
                    double fill_x1 = text_x + b->content_width;
                    if (fill_x1 > x1) x1 = fill_x1;
                }
            }
            if (r->box_h > 0) {
                if (is_textarea) {
                    y1 = y0 + r->box_h;
                    double text_bottom = y_origin +
                        (double)(r1.y + r1.height) / NS_PANGO_SCALE + 3.0;
                    if (text_bottom > y1) y1 = text_bottom;
                } else {
                    double cy = (y0 + y1) / 2.0;
                    y0 = cy - r->box_h / 2.0;
                    y1 = cy + r->box_h / 2.0;
                }
            }
            inline_shift_rect(b, r->start, &x0, &y0, &x1, &y1);
            if (x1 < x0) { double t = x0; x0 = x1; x1 = t; }
            const ns_box *field_box = NULL;
            for (const ns_box *p = b; p; p = p->parent)
                if (p->dom) { field_box = p; break; }
            gboolean block_chrome = field_box &&
                                    field_box->dom == r->dom &&
                                    style_has_inline_box_paint(r->style);
            gboolean draw_native_chrome = r->native_chrome && !block_chrome;
            if (r->kind == NS_INLINE_BUTTON && r->dom &&
                ns_element_get_attr(r->dom, "class") &&
                r->box_w <= 0 && r->box_h <= 0)
                draw_native_chrome = FALSE;
            if (!draw_native_chrome && !block_chrome)
                paint_inline_css_chrome(cr, r, x0, y0, x1 - x0, y1 - y0);
            if (draw_native_chrome) {
                cairo_save(cr);
                if (r->kind == NS_INLINE_BUTTON)
                    cairo_set_source_rgb(cr, 0.902, 0.902, 0.902);
                else
                    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
                cairo_fill(cr);
                cairo_set_source_rgb(cr, 0.722, 0.722, 0.722);
                cairo_set_line_width(cr, 1.0);
                cairo_rectangle(cr, x0 + 0.5, y0 + 0.5,
                                x1 - x0 - 1, y1 - y0 - 1);
                cairo_stroke(cr);
                cairo_restore(cr);
            }
            if (r->dom && ns_node_is_element_named(r->dom, "select") &&
                !keyword_is(r->style ? r->style->values[NS_CSS_APPEARANCE]
                                     : NULL, "none")) {
                const ns_css_value *cv = r->style
                    ? r->style->values[NS_CSS_COLOR] : NULL;
                rgba arrow = rgba_of(cv, 0.1, 0.1, 0.1, 1);
                double sz = MIN(5.0, (y1 - y0) / 5.0);
                double ax = x1 - 6.0 - sz;
                double ay = (y0 + y1) / 2.0;
                cairo_save(cr);
                set_source_rgba(cr, arrow);
                cairo_set_line_width(cr, 1.5);
                cairo_move_to(cr, ax - sz, ay - sz / 2.0);
                cairo_line_to(cr, ax, ay + sz / 2.0);
                cairo_line_to(cr, ax + sz, ay - sz / 2.0);
                cairo_stroke(cr);
                cairo_restore(cr);
            }
            if (r->kind == NS_INLINE_INPUT_FIELD_FOCUSED &&
                draw_native_chrome) {
                cairo_save(cr);
                cairo_set_source_rgb(cr, 0.13, 0.36, 0.80);
                cairo_set_line_width(cr, 2.0);
                cairo_rectangle(cr, x0 + 0.5, y0 + 0.5,
                                x1 - x0 - 1, y1 - y0 - 1);
                cairo_stroke(cr);
                cairo_restore(cr);
            }
        }
        if (opt_count > 0 && opt_maxx > opt_minx) {
            double px = opt_minx - 6.0;
            double pw = (opt_maxx - opt_minx) + 12.0;
            double py = opt_miny;
            double ph = opt_maxy - opt_miny;
            corner_radii pr = corner_radii_uniform(3);
            cairo_save(cr);
            rounded_rect_path(cr, px + 0.5, py + 1.5, pw, ph, pr);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
            cairo_fill(cr);
            rounded_rect_path(cr, px, py, pw, ph, pr);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_fill(cr);
            for (int k = 0; k < opt_nsel; k++) {
                cairo_rectangle(cr, px, opt_sel[k].y0, pw,
                                opt_sel[k].y1 - opt_sel[k].y0);
                cairo_set_source_rgb(cr, 0.816, 0.886, 0.988);
                cairo_fill(cr);
            }
            rounded_rect_path(cr, px + 0.5, py + 0.5, pw - 1, ph - 1, pr);
            cairo_set_source_rgb(cr, 0.70, 0.72, 0.75);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }

    paint_inline_text_shadows(cr, b, s, layout, text_x, y_origin);

    if (g_dbg_paint_x >= 0 && b->text) {
        double px0 = b->x, py0 = b->y;
        double px1 = b->x + b->content_width, py1 = b->y + b->content_height;
        cairo_user_to_device(cr, &px0, &py0);
        cairo_user_to_device(cr, &px1, &py1);
        if (g_dbg_paint_x >= px0 && g_dbg_paint_x <= px1 &&
            g_dbg_paint_y >= py0 && g_dbg_paint_y <= py1) {
            double cx0, cy0, cx1, cy1;
            cairo_clip_extents(cr, &cx0, &cy0, &cx1, &cy1);
            g_printerr("[paint-at] TEXT \"%.30s\" rgba(%.2f,%.2f,%.2f,%.2f) "
                       "at %.0f,%.0f clip=%.0f,%.0f..%.0f,%.0f grp=%d\n",
                       b->text, color.r, color.g, color.b, color.a,
                       text_x, y_origin, cx0, cy0, cx1, cy1,
                       cairo_get_group_target(cr) != cairo_get_target(cr));
        }
    }
    const ns_selection_run *sel_run = paint_selection_run(b);
    inline_text_src src = inline_text_src_of(b, layout, 0);
    if (sel_run)
        paint_selection_background(cr, b, &src, text_x, y_origin, sel_run);

    paint_inline_text(cr, b, &src, text_x, y_origin, color);

    if (sel_run)
        paint_selection_foreground(cr, b, &src, text_x, y_origin, sel_run);

    paint_inline_dashed_decorations(cr, b, layout, text_x, y_origin, s, color);

    if (b->attrs) {
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
            if (r->kind != NS_INLINE_CARET) continue;
            if (!g_caret_visible) continue;
            if (b->text && r->start >= strlen(b->text)) continue;
            NsPangoRectangle pos;
            ns_pango_layout_index_to_pos(layout, (int)r->start, &pos);
            double shift_x, shift_y;
            ns_inline_offset_at(b, r->start, &shift_x, &shift_y);
            double cx = text_x + shift_x + (double)pos.x / NS_PANGO_SCALE;
            double cy = y_origin + shift_y + (double)pos.y / NS_PANGO_SCALE;
            double ch = (double)pos.height / NS_PANGO_SCALE;
            if (ch < 1.0) ch = 14.0;
            cairo_save(cr);
            const ns_style *cstyle = s;
            for (guint j = 0; j < b->attrs->len; j++) {
                const ns_inline_attr *f = &g_array_index(b->attrs, ns_inline_attr, j);
                if ((f->kind == NS_INLINE_INPUT_FIELD ||
                     f->kind == NS_INLINE_INPUT_FIELD_FOCUSED) && f->style &&
                    f->start <= r->start && r->start <= f->start + f->len) {
                    cstyle = f->style;
                    break;
                }
            }
            const ns_css_value *cc = cstyle ? cstyle->values[NS_CSS_CARET_COLOR] : NULL;
            const ns_css_value *tc = cstyle ? cstyle->values[NS_CSS_COLOR] : NULL;
            if (cc && cc->kind == NS_CSS_V_COLOR)
                cairo_set_source_rgb(cr, cc->u.color.r / 255.0,
                                     cc->u.color.g / 255.0, cc->u.color.b / 255.0);
            else if (tc && tc->kind == NS_CSS_V_COLOR)
                cairo_set_source_rgb(cr, tc->u.color.r / 255.0,
                                     tc->u.color.g / 255.0, tc->u.color.b / 255.0);
            else
                cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, cx + 0.5, cy);
            cairo_line_to(cr, cx + 0.5, cy + ch);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }

    if (b->attrs) {
        const ns_css_value *ac = s ? s->values[NS_CSS_ACCENT_COLOR] : NULL;
        rgba accent = rgba_of(
            (ac && ac->kind == NS_CSS_V_COLOR) ? ac : NULL,
            0.0, 0.459, 1.0, 1);
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
            if (r->kind != NS_INLINE_CHECKBOX &&
                r->kind != NS_INLINE_CHECKBOX_CHECKED &&
                r->kind != NS_INLINE_RADIO &&
                r->kind != NS_INLINE_RADIO_CHECKED)
                continue;
            NsPangoRectangle r0;
            ns_pango_layout_index_to_pos(layout, (int)r->start, &r0);
            double side, margins[4];
            ns_inline_toggle_geometry(r, &side, margins);
            double bx = text_x + (double)r0.x / NS_PANGO_SCALE + margins[3];
            double by = y_origin +
                layout_baseline_at_index(layout, (int)r->start) - side;
            double dx, dy;
            ns_inline_offset_at(b, r->start, &dx, &dy);
            bx += dx;
            by += dy;
            gboolean radio = (r->kind == NS_INLINE_RADIO ||
                              r->kind == NS_INLINE_RADIO_CHECKED);
            gboolean checked = (r->kind == NS_INLINE_CHECKBOX_CHECKED ||
                                r->kind == NS_INLINE_RADIO_CHECKED);
            cairo_save(cr);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            corner_radii toggle_radii = corner_radii_uniform(side * 0.16);
            if (radio) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, bx + side / 2.0, by + side / 2.0,
                          side / 2.0 - 0.5, 0, 2 * G_PI);
            } else {
                rounded_rect_path(cr, bx + 0.5, by + 0.5, side - 1, side - 1,
                                  toggle_radii);
            }
            cairo_fill_preserve(cr);
            if (checked && radio)
                cairo_set_source_rgba(cr, accent.r, accent.g, accent.b, accent.a);
            else
                cairo_set_source_rgb(cr, 0.463, 0.463, 0.463);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            if (checked) {
                cairo_set_source_rgba(cr, accent.r, accent.g, accent.b, accent.a);
                if (radio) {
                    double rdot = side * 0.31;
                    cairo_new_sub_path(cr);
                    cairo_arc(cr, bx + side / 2.0, by + side / 2.0,
                              rdot, 0, 2 * G_PI);
                    cairo_fill(cr);
                } else {
                    rounded_rect_path(cr, bx, by, side, side, toggle_radii);
                    cairo_fill(cr);
                    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                    cairo_set_line_width(cr, side * 0.18);
                    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                    cairo_move_to(cr, bx + side * 0.20, by + side * 0.55);
                    cairo_line_to(cr, bx + side * 0.42, by + side * 0.78);
                    cairo_line_to(cr, bx + side * 0.80, by + side * 0.28);
                    cairo_stroke(cr);
                }
            }
            cairo_restore(cr);
        }
    }

    if (b->attrs) {
        const ns_css_value *ac = s ? s->values[NS_CSS_ACCENT_COLOR] : NULL;
        rgba accent = rgba_of(
            (ac && ac->kind == NS_CSS_V_COLOR) ? ac : NULL,
            0.0, 0.459, 1.0, 1);
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
            if (r->kind != NS_INLINE_PROGRESS &&
                r->kind != NS_INLINE_METER) continue;
            NsPangoRectangle r0, r1;
            ns_pango_layout_index_to_pos(layout, (int)r->start, &r0);
            ns_pango_layout_index_to_pos(layout,
                (int)(r->len > 0 ? r->start + r->len - 1 : r->start), &r1);
            double gx0 = text_x + (double)r0.x / NS_PANGO_SCALE;
            double gy0 = y_origin + (double)r0.y / NS_PANGO_SCALE;
            double gx1 = text_x + (double)(r1.x + r1.width) / NS_PANGO_SCALE;
            double gy1 = y_origin + (double)(r0.y + r0.height) / NS_PANGO_SCALE;
            inline_shift_rect(b, r->start, &gx0, &gy0, &gx1, &gy1);
            if (gx1 < gx0) { double t = gx0; gx0 = gx1; gx1 = t; }
            double pad_x = 2;
            double bx = gx0 + pad_x;
            double bw = gx1 - gx0 - pad_x * 2;
            if (bw < 4) bw = 4;
            double bh = (gy1 - gy0) * 0.55;
            if (bh < 6) bh = 6;
            double by = gy0 + ((gy1 - gy0) - bh) / 2.0;
            double radius = bh / 2.0;
            cairo_save(cr);
            cairo_new_sub_path(cr);
            cairo_arc(cr, bx + radius,      by + radius, radius,  G_PI / 2,  3 * G_PI / 2);
            cairo_arc(cr, bx + bw - radius, by + radius, radius, -G_PI / 2,      G_PI / 2);
            cairo_close_path(cr);
            cairo_set_source_rgb(cr, 0.88, 0.88, 0.90);
            cairo_fill_preserve(cr);
            cairo_clip(cr);
            double frac = r->font_size_px;
            if (frac > 1) frac = 1;
            rgba fill = accent;
            if (r->kind == NS_INLINE_METER && r->a)
                fill = (rgba){ r->r / 255.0, r->g / 255.0,
                               r->b / 255.0, r->a / 255.0 };
            cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, fill.a);
            if (r->kind == NS_INLINE_PROGRESS && frac < 0) {
                double iw = bw * 0.35;
                double ix = bx + (bw - iw) / 2.0;
                cairo_rectangle(cr, ix, by, iw, bh);
            } else {
                if (frac < 0) frac = 0;
                cairo_rectangle(cr, bx, by, bw * frac, bh);
            }
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }

    if (b->attrs)
        paint_inline_ranges(cr, b, s, layout, text_x, y_origin);

    paint_inline_atomics(cr, b, layout, text_x, highlight, 0);
    paint_inline_defer_layers(cr, b, layout, &at, highlight);
    g_object_unref(layout);
}

NsPangoLayout *
ns_paint_build_inline_layout(cairo_t *cr, const ns_box *b)
{
    (void)cr;
    if (!b || !b->text) return NULL;
    const ns_style *s = inherited_style(b);

    NsPangoAttrList *attrs = NULL;
    NsPangoLayout *layout = inline_layout_begin(b, s, &attrs);
    if (b->attrs) {
        for (gint ii = (gint)b->attrs->len - 1; ii >= 0; ii--) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, (guint)ii);
            attr_insert_range(attrs, ns_paint_inline_font_attr(r), r->start, r->len);
        }
    }
    ns_inline_layout_set_attrs(layout, attrs, b);
    ns_pango_attr_list_unref(attrs);

    ns_paint_apply_text_align(layout, s);
    apply_nowrap_align_width(layout, b);
    ns_paint_start_align_overflow(layout);
    return layout;
}

void
ns_paint_sync_inline_atomic_offsets(ns_box *root)
{
    if (!root) return;
    if (root->inline_atomics && root->text && *root->text) {
        const ns_style *s = inherited_style(root);
        NsPangoLayout *layout = paint_inline_make_layout(root, s, NULL);
        double text_x = 0;
        double ti = ns_inline_text_indent_px(root, s, root->content_width);
        if (ti < 0) text_x = ti;
        for (guint i = 0; i < root->inline_atomics->len; i++) {
            ns_inline_atomic *atomic =
                &g_array_index(root->inline_atomics, ns_inline_atomic, i);
            NsPangoRectangle pos;
            ns_pango_layout_index_to_pos(layout, (int)atomic->byte_off, &pos);
            atomic->owner_offset_x = text_x + (double)pos.x / NS_PANGO_SCALE;
            atomic->owner_offset_y = root->atomic_line_heights && atomic->box
                ? atomic->box->y - atomic->box->rel_dy - root->y
                : (double)pos.y / NS_PANGO_SCALE;
        }
        g_object_unref(layout);
    }
    for (ns_box *child = root->first_child; child; child = child->next_sibling)
        ns_paint_sync_inline_atomic_offsets(child);
    if (root->inline_atomics)
        for (guint i = 0; i < root->inline_atomics->len; i++)
            ns_paint_sync_inline_atomic_offsets(
                g_array_index(root->inline_atomics, ns_inline_atomic, i).box);
}

gboolean
ns_paint_inline_xy_to_byte(const ns_box *b, double rel_x, double rel_y,
                           gsize *out_byte)
{
    if (!b || !b->text || !*b->text) return FALSE;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cr = cairo_create(surf);
    NsPangoLayout *layout = ns_paint_build_inline_layout(cr, b);
    if (!layout) {
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        return FALSE;
    }

    int index = 0, trailing = 0;
    double y_offset = ns_paint_inline_y_offset_for_layout(b, layout);
    double layout_x = rel_x;
    double layout_y = rel_y - y_offset;
    ns_inline_unshift_point(b, layout, &layout_x, &layout_y);
    if (layout_y < 0) layout_y = 0;
    ns_pango_layout_xy_to_index(layout, (int)(layout_x * NS_PANGO_SCALE),
                             (int)(layout_y * NS_PANGO_SCALE),
                             &index, &trailing);
    if (out_byte) {
        gsize tlen = strlen(b->text);
        gsize bi = (gsize)index <= tlen ? (gsize)index : tlen;
        const char *p = g_utf8_offset_to_pointer(b->text + bi, trailing);
        gsize off = (gsize)(p - b->text);
        *out_byte = off <= tlen ? off : tlen;
    }

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return TRUE;
}

static gboolean
inline_point_near_shift(const ns_box *b, double rel_x, double rel_y)
{
    double x0, y0, x1, y1;
    if (!ns_inline_shift_bounds(b, &x0, &y0, &x1, &y1)) return FALSE;
    return rel_x >= x0 && rel_x <= b->content_width + x1 &&
           rel_y >= y0 && rel_y <= b->content_height + y1;
}

typedef struct inline_extent {
    gboolean any;
    double x0, y0, x1, y1;
} inline_extent;

static void
inline_extent_add(inline_extent *e, double x0, double y0, double x1,
                  double y1)
{
    if (!e->any) {
        *e = (inline_extent){ TRUE, x0, y0, x1, y1 };
        return;
    }
    e->x0 = MIN(e->x0, x0);
    e->y0 = MIN(e->y0, y0);
    e->x1 = MAX(e->x1, x1);
    e->y1 = MAX(e->y1, y1);
}

typedef struct inline_extent_walk {
    const inline_layers *layers;
    const double *baselines;
    double ox, oy;
    inline_extent *extent;
} inline_extent_walk;

static void
inline_run_moved_extent(inline_extent *e, const NsPangoLayoutRun *run,
                        double x0, double baseline)
{
    NsPangoRectangle logical;
    ns_pango_glyph_string_extents(run->glyphs, run->item->analysis.font,
                                  NULL, &logical);
    double base = baseline - (double)run->y_offset / NS_PANGO_SCALE;
    double ascent = round(-(double)logical.y / NS_PANGO_SCALE);
    double descent =
        round((double)(logical.height + logical.y) / NS_PANGO_SCALE);
    inline_extent_add(e, x0, base - ascent,
                      x0 + (double)inline_run_advance(run) / NS_PANGO_SCALE,
                      base + descent);
}

static void
inline_seg_moved_extent(const inline_extent_walk *w,
                        const inline_layer_seg *seg,
                        const ns_inline_attr *owner)
{
    const inline_layer_line *g =
        &g_array_index(w->layers->lines, inline_layer_line, seg->line);
    double baseline = owner->rel_dy + (w->baselines
        ? w->baselines[seg->line]
        : w->oy + (double)g->baseline / NS_PANGO_SCALE);
    double x0 = w->ox + owner->rel_dx +
                (double)(g->x + seg->x) / NS_PANGO_SCALE;
    double x = x0;
    for (const GSList *l = seg->first; l; l = l->next) {
        inline_run_moved_extent(w->extent, l->data, x, baseline);
        x += (double)inline_run_advance(l->data) / NS_PANGO_SCALE;
        if (l == seg->last) break;
    }
    double above, below;
    if (!inline_box_vertical_reach(owner->style, &above, &below)) return;
    const NsPangoLayoutRun *first = seg->first->data;
    double base = baseline - (double)first->y_offset / NS_PANGO_SCALE;
    inline_extent_add(w->extent, x0, base - above,
                      x0 + (double)seg->width / NS_PANGO_SCALE, base + below);
}

static void
inline_layers_moved_extent(const ns_box *b, const inline_layers *L,
                           double ox, double oy, inline_extent *e)
{
    double *baselines = inline_layer_baselines(b, L);
    inline_extent_walk w = { L, baselines, ox, oy, e };
    const inline_bucket *bk = &L->seg_order;
    for (guint k = bk->start[1]; k < bk->start[L->owners->len]; k++) {
        const inline_layer_seg *seg =
            &g_array_index(L->segs, inline_layer_seg, bk->order[k]);
        const ns_inline_attr *owner = inline_layer_owner(b, L, seg->layer);
        if (owner) inline_seg_moved_extent(&w, seg, owner);
    }
    g_free(baselines);
}

gboolean
ns_paint_inline_moved_extents(const ns_box *b, double *x0, double *y0,
                              double *x1, double *y1)
{
    if (!b || b->kind != NS_BOX_INLINE || !b->text || !*b->text ||
        !inline_box_has_layers(b))
        return FALSE;
    const ns_style *s = inherited_style(b);
    NsPangoLayout *layout = paint_inline_layout(b, s, NULL);
    const inline_layers *L = inline_layers_of(b, layout);
    inline_extent e = { FALSE, 0, 0, 0, 0 };
    if (L)
        inline_layers_moved_extent(b, L, paint_inline_text_x(b, s),
            b->y + ns_paint_inline_y_offset_for_layout(b, layout), &e);
    g_object_unref(layout);
    *x0 = e.x0;
    *y0 = e.y0;
    *x1 = e.x1;
    *y1 = e.y1;
    return e.any;
}

gboolean
ns_paint_inline_on_shifted_text(const ns_box *b, double rel_x, double rel_y)
{
    if (!b || !b->text || !*b->text || !inline_point_near_shift(b, rel_x, rel_y))
        return FALSE;
    NsPangoLayout *layout = ns_paint_build_inline_layout(NULL, b);
    if (!layout) return FALSE;
    double layout_y = rel_y - ns_paint_inline_y_offset_for_layout(b, layout);
    gboolean on = ns_inline_unshift_point(b, layout, &rel_x, &layout_y);
    g_object_unref(layout);
    return on;
}

gboolean
ns_paint_inline_word_range(const ns_box *b, gsize byte,
                           gsize *out_start, gsize *out_end)
{
    if (!b || !b->text || !*b->text) return FALSE;
    gsize tlen = strlen(b->text);
    if (byte > tlen) byte = tlen;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cr = cairo_create(surf);
    NsPangoLayout *layout = ns_paint_build_inline_layout(cr, b);
    if (!layout) {
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        return FALSE;
    }

    int n_attrs = 0;
    const NsPangoLogAttr *attrs =
        ns_pango_layout_get_log_attrs_readonly(layout, &n_attrs);
    const char *text = ns_pango_layout_get_text(layout);
    gboolean ok = FALSE;
    if (attrs && n_attrs > 1 && text && strlen(text) == tlen) {
        long here = g_utf8_pointer_to_offset(text, text + byte);
        if (here < 0) here = 0;
        if (here > n_attrs - 1) here = n_attrs - 1;
        int s = (int)here, e = (int)here;
        while (s > 0 && !attrs[s].is_word_start) s--;
        while (e < n_attrs - 1 && !attrs[e].is_word_end) e++;
        if (e > s) {
            const char *sp = g_utf8_offset_to_pointer(text, s);
            const char *ep = g_utf8_offset_to_pointer(text, e);
            if (out_start) *out_start = (gsize)(sp - text);
            if (out_end)   *out_end   = (gsize)(ep - text);
            ok = TRUE;
        }
    }

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return ok;
}

typedef struct inline_union {
    double x0, y0, x1, y1;
    gboolean any;
} inline_union;

static void
inline_union_add(const inline_fragment *f, gpointer data)
{
    inline_union *u = data;
    if (!u->any) {
        u->x0 = f->x0; u->y0 = f->y0; u->x1 = f->x1; u->y1 = f->y1;
        u->any = TRUE;
        return;
    }
    if (f->x0 < u->x0) u->x0 = f->x0;
    if (f->y0 < u->y0) u->y0 = f->y0;
    if (f->x1 > u->x1) u->x1 = f->x1;
    if (f->y1 > u->y1) u->y1 = f->y1;
}

static void
inline_rects_add(const inline_fragment *f, gpointer data)
{
    GArray *out = data;
    double rect[4] = { f->x0, f->y0, f->x1 - f->x0, f->y1 - f->y0 };
    g_array_append_vals(out, rect, 4);
}

gboolean
ns_paint_inline_range_rects(const ns_box *b, gsize start, gsize len,
                            const ns_inline_attr *element, GArray *out)
{
    if (!b || !b->text || !*b->text || len == 0 || !out) return FALSE;
    gsize text_len = strlen(b->text);
    if (start >= text_len) return FALSE;
    if (start + len > text_len) len = text_len - start;
    const ns_style *box_style =
        element && !b->vertical_wm ? element->style : NULL;
    NsPangoLayout *layout = box_style
        ? paint_inline_make_layout(b, inherited_style(b), NULL)
        : ns_paint_build_inline_layout(NULL, b);
    if (!layout) return FALSE;
    double y_origin = b->y + ns_paint_inline_y_offset_for_layout(b, layout);
    guint before = out->len;
    inline_range range =
        inline_range_for(b, start, len, box_style ? element : NULL);
    inline_range_fragments(b, layout, y_origin, &range, inline_rects_add, out);
    g_object_unref(layout);
    for (guint i = before; i + 3 < out->len; i += 4)
        g_array_index(out, double, i + 1) -= b->y;
    return out->len > before;
}

gboolean
ns_paint_inline_range_extents(const ns_box *b, gsize start, gsize len,
                              const ns_inline_attr *element,
                              double *out_x, double *out_y,
                              double *out_w, double *out_h)
{
    if (!b || !b->text || !*b->text || len == 0) return FALSE;
    gsize text_len = strlen(b->text);
    if (start >= text_len) return FALSE;
    if (start + len > text_len) len = text_len - start;

    const ns_style *box_style =
        element && !b->vertical_wm ? element->style : NULL;
    NsPangoLayout *layout = box_style
        ? paint_inline_make_layout(b, inherited_style(b), NULL)
        : ns_paint_build_inline_layout(NULL, b);
    if (!layout) return FALSE;
    double y_origin = b->y + ns_paint_inline_y_offset_for_layout(b, layout);
    inline_union u = { 0 };
    inline_range range =
        inline_range_for(b, start, len, box_style ? element : NULL);
    inline_range_fragments(b, layout, y_origin, &range, inline_union_add, &u);
    g_object_unref(layout);
    if (!u.any) return FALSE;
    if (out_x) *out_x = u.x0;
    if (out_y) *out_y = u.y0 - b->y;
    if (out_w) *out_w = u.x1 - u.x0;
    if (out_h) *out_h = u.y1 - u.y0;
    return TRUE;
}

gboolean
ns_paint_inline_empty_rect(const ns_box *b, const ns_inline_attr *r,
                           double *out_x, double *out_y, double *out_h)
{
    if (!b || !b->text || r->start > strlen(b->text) ||
        (!*b->text && !b->phantom_line))
        return FALSE;
    if (!*b->text)
        ns_pango_context_set_base_dir(ns_paint_text_context(),
                                      NS_PANGO_DIRECTION_LTR);
    const ns_inline_attr *element = b->vertical_wm ? NULL : r;
    NsPangoLayout *layout = element
        ? paint_inline_make_layout(b, inherited_style(b), NULL)
        : ns_paint_build_inline_layout(NULL, b);
    if (!layout) return FALSE;
    double y_origin = b->y + ns_paint_inline_y_offset_for_layout(b, layout);
    inline_range range = inline_range_for(b, r->start, 0, element);
    NsPangoRectangle pos;
    ns_pango_layout_index_to_pos(layout, (int)r->start, &pos);
    int line_no = 0, line_x = 0;
    ns_pango_layout_index_to_line_x(layout, (int)r->start, FALSE,
                                    &line_no, &line_x);
    double above = 0, below = 0, y0, y1;
    if (b->phantom_line) {
        y0 = y1 = b->y;
    } else if (inline_box_vertical_reach(range.box_style, &above, &below)) {
        double *baselines = paint_inline_line_baselines(b, layout, y_origin);
        int lines = ns_pango_layout_get_line_count(layout);
        double baseline =
            baselines[CLAMP(line_no, 0, MAX(lines - 1, 0))] - range.rise;
        g_free(baselines);
        y0 = baseline - above;
        y1 = baseline + below;
    } else {
        y0 = y_origin + (double)pos.y / NS_PANGO_SCALE;
        y1 = y0 + (double)pos.height / NS_PANGO_SCALE;
    }
    g_object_unref(layout);
    *out_x = (double)pos.x / NS_PANGO_SCALE + range.dx;
    *out_y = y0 + range.dy - b->y;
    *out_h = y1 - y0;
    return TRUE;
}

gboolean
ns_paint_inline_toggle_rect(const ns_box *b, const ns_inline_attr *r,
                            double *out_x, double *out_y,
                            double *out_w, double *out_h)
{
    double side, margins[4];
    if (!ns_inline_toggle_geometry(r, &side, margins)) return FALSE;
    double ex, ey, ew, eh;
    if (!ns_paint_inline_range_extents(b, r->start, r->len, NULL,
                                       &ex, &ey, &ew, &eh))
        return FALSE;
    NsPangoLayout *layout = ns_paint_build_inline_layout(NULL, b);
    if (!layout) return FALSE;
    double y_offset = ns_paint_inline_y_offset_for_layout(b, layout);
    double baseline = layout_baseline_at_index(layout, (int)r->start);
    g_object_unref(layout);
    double dx, dy;
    ns_inline_offset_at(b, r->start, &dx, &dy);
    *out_x = ex + margins[3] + dx;
    *out_y = y_offset + baseline - side + dy;
    *out_w = side;
    *out_h = side;
    return TRUE;
}

static double
parse_filter_amount(const char *p, const char **out_end)
{
    while (*p && (*p == ' ' || *p == '\t')) p++;
    char *endp = NULL;
    double v = g_ascii_strtod(p, &endp);
    if (!endp || endp == p) {
        if (out_end) *out_end = p;
        return -1;
    }
    if (*endp == '%') {
        v /= 100.0;
        endp++;
    }
    if (out_end) *out_end = endp;
    return v;
}

static int
clamp_i(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void
box_blur_argb(guchar *data, int stride, int w, int h, int radius)
{
    if (radius < 1 || w < 1 || h < 1) return;
    int win = radius * 2 + 1;
    guchar *tmp = g_malloc0((gsize)stride * h);
    for (int y = 0; y < h; y++) {
        guchar *s = data + y * stride;
        guchar *d = tmp + y * stride;
        for (int c = 0; c < 4; c++) {
            int sum = 0;
            for (int i = -radius; i <= radius; i++)
                sum += s[clamp_i(i, 0, w - 1) * 4 + c];
            for (int x = 0; x < w; x++) {
                d[x * 4 + c] = (guchar)(sum / win);
                sum += s[clamp_i(x + radius + 1, 0, w - 1) * 4 + c]
                     - s[clamp_i(x - radius, 0, w - 1) * 4 + c];
            }
        }
    }
    int row_bytes = w * 4;
    int *sums = g_new0(int, row_bytes);
    for (int i = -radius; i <= radius; i++) {
        const guchar *row = tmp + (gsize)clamp_i(i, 0, h - 1) * stride;
        for (int x = 0; x < row_bytes; x++) sums[x] += row[x];
    }
    for (int y = 0; y < h; y++) {
        guchar *d = data + (gsize)y * stride;
        const guchar *add = tmp + (gsize)clamp_i(y + radius + 1, 0, h - 1) * stride;
        const guchar *sub = tmp + (gsize)clamp_i(y - radius, 0, h - 1) * stride;
        for (int x = 0; x < row_bytes; x++) {
            d[x] = (guchar)(sums[x] / win);
            sums[x] += add[x] - sub[x];
        }
    }
    g_free(sums);
    g_free(tmp);
}

typedef struct image_drop_shadow {
    double x, y, blur;
    rgba color;
} image_drop_shadow;

static gboolean
filter_name_is(const char *name, gsize nlen, const char *want)
{
    gsize want_len = strlen(want);
    return nlen == want_len &&
           g_ascii_strncasecmp(name, want, want_len) == 0;
}

static const char *
filter_function_next(const char *p, const char **name_out, gsize *name_len_out,
                     const char **body_out, const char **body_end_out)
{
    while (*p && g_ascii_isspace((unsigned char)*p)) p++;
    if (!*p) return NULL;
    const char *name = p;
    while (*p && (g_ascii_isalpha((unsigned char)*p) || *p == '-')) p++;
    gsize name_len = (gsize)(p - name);
    while (*p && g_ascii_isspace((unsigned char)*p)) p++;
    if (!name_len || *p != '(') return NULL;
    p++;
    const char *body = p;
    int depth = 1;
    while (*p) {
        if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            depth--;
            if (depth == 0) {
                if (name_out) *name_out = name;
                if (name_len_out) *name_len_out = name_len;
                if (body_out) *body_out = body;
                if (body_end_out) *body_end_out = p;
                return p + 1;
            }
        }
        p++;
    }
    return NULL;
}

static gboolean
filter_has_bitmap_effect(const char *filter)
{
    if (!filter) return FALSE;
    const char *p = filter;
    while (*p) {
        const char *name = NULL;
        gsize nlen = 0;
        const char *next = filter_function_next(p, &name, &nlen, NULL, NULL);
        if (!next) break;
        if (filter_name_is(name, nlen, "grayscale") ||
            filter_name_is(name, nlen, "sepia") ||
            filter_name_is(name, nlen, "invert") ||
            filter_name_is(name, nlen, "brightness") ||
            filter_name_is(name, nlen, "contrast") ||
            filter_name_is(name, nlen, "saturate") ||
            filter_name_is(name, nlen, "hue-rotate") ||
            filter_name_is(name, nlen, "opacity") ||
            filter_name_is(name, nlen, "drop-shadow") ||
            filter_name_is(name, nlen, "blur"))
            return TRUE;
        p = next;
    }
    return FALSE;
}

static gboolean
parse_filter_length_px(const char *s, double *out)
{
    while (*s && g_ascii_isspace((unsigned char)*s)) s++;
    char *endp = NULL;
    double v = g_ascii_strtod(s, &endp);
    if (!endp || endp == s) return FALSE;
    while (*endp && g_ascii_isspace((unsigned char)*endp)) endp++;
    if (*endp == '\0' || g_ascii_strcasecmp(endp, "px") == 0) {
        *out = v;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "em") == 0 ||
        g_ascii_strcasecmp(endp, "rem") == 0 ||
        g_ascii_strcasecmp(endp, "lh") == 0 ||
        g_ascii_strcasecmp(endp, "rlh") == 0) {
        *out = v * 16.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "pt") == 0) {
        *out = v * (96.0 / 72.0);
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "pc") == 0) {
        *out = v * 16.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "cm") == 0) {
        *out = v * (96.0 / 2.54);
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "mm") == 0) {
        *out = v * (96.0 / 25.4);
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "q") == 0) {
        *out = v * (96.0 / 101.6);
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "in") == 0) {
        *out = v * 96.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "vw") == 0 ||
        g_ascii_strcasecmp(endp, "dvw") == 0 ||
        g_ascii_strcasecmp(endp, "svw") == 0 ||
        g_ascii_strcasecmp(endp, "lvw") == 0) {
        *out = v * ns_css_viewport_w() / 100.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "vh") == 0 ||
        g_ascii_strcasecmp(endp, "dvh") == 0 ||
        g_ascii_strcasecmp(endp, "svh") == 0 ||
        g_ascii_strcasecmp(endp, "lvh") == 0) {
        *out = v * ns_css_viewport_h() / 100.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "vmin") == 0 ||
        g_ascii_strcasecmp(endp, "dvmin") == 0 ||
        g_ascii_strcasecmp(endp, "svmin") == 0 ||
        g_ascii_strcasecmp(endp, "lvmin") == 0) {
        *out = v * MIN(ns_css_viewport_w(), ns_css_viewport_h()) / 100.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "vmax") == 0 ||
        g_ascii_strcasecmp(endp, "dvmax") == 0 ||
        g_ascii_strcasecmp(endp, "svmax") == 0 ||
        g_ascii_strcasecmp(endp, "lvmax") == 0) {
        *out = v * MAX(ns_css_viewport_w(), ns_css_viewport_h()) / 100.0;
        return TRUE;
    }
    return FALSE;
}

static int
filter_split_ws(const char *start, const char *end, char *tokens[], int max)
{
    int n = 0;
    int depth = 0;
    const char *tok = NULL;
    for (const char *p = start; p <= end; p++) {
        gboolean done = p == end;
        char c = done ? '\0' : *p;
        if (!done && !tok && !g_ascii_isspace((unsigned char)c))
            tok = p;
        if (!done && c == '(') depth++;
        else if (!done && c == ')' && depth > 0) depth--;
        if ((done || (g_ascii_isspace((unsigned char)c) && depth == 0)) && tok) {
            if (n < max)
                tokens[n++] = g_strndup(tok, (gsize)(p - tok));
            tok = NULL;
        }
    }
    return n;
}

static gboolean
parse_filter_shadow_body(const char *body, const char *body_end,
                         rgba current_color, image_drop_shadow *out)
{
    char *tokens[8] = {0};
    double lengths[3] = {0};
    int n_lengths = 0;
    rgba color = current_color;
    int n = filter_split_ws(body, body_end, tokens, G_N_ELEMENTS(tokens));
    gboolean ok = TRUE;
    for (int i = 0; i < n; i++) {
        double len = 0;
        guint8 r, g, b, a;
        char *token = g_strstrip(tokens[i]);
        if (parse_filter_length_px(token, &len)) {
            if (n_lengths < 3) lengths[n_lengths++] = len;
            else ok = FALSE;
        } else if (g_ascii_strcasecmp(token, "currentcolor") == 0) {
            color = current_color;
        } else if (ns_css_parse_color(token, &r, &g, &b, &a)) {
            color.r = r / 255.0;
            color.g = g / 255.0;
            color.b = b / 255.0;
            color.a = a / 255.0;
        } else {
            ok = FALSE;
        }
    }
    for (int i = 0; i < n; i++) g_free(tokens[i]);
    if (!ok || n_lengths < 2) return FALSE;
    out->x = lengths[0];
    out->y = lengths[1];
    out->blur = n_lengths >= 3 && lengths[2] > 0 ? lengths[2] : 0;
    out->color = color;
    return TRUE;
}

static int
parse_filter_drop_shadows(const char *filter, rgba current_color,
                          image_drop_shadow shadows[], int max)
{
    if (!filter || max <= 0) return 0;
    int n = 0;
    const char *p = filter;
    while (*p && n < max) {
        const char *name = NULL;
        const char *body = NULL;
        const char *body_end = NULL;
        gsize nlen = 0;
        const char *next =
            filter_function_next(p, &name, &nlen, &body, &body_end);
        if (!next) break;
        if (filter_name_is(name, nlen, "drop-shadow") &&
            parse_filter_shadow_body(body, body_end, current_color, &shadows[n]))
            n++;
        p = next;
    }
    return n;
}

static double
filter_drop_shadow_reach(const char *filter, rgba current_color)
{
    image_drop_shadow shadows[8];
    int n = parse_filter_drop_shadows(filter, current_color, shadows,
                                      G_N_ELEMENTS(shadows));
    double reach = 0;
    for (int i = 0; i < n; i++)
        reach += MAX(fabs(shadows[i].x), fabs(shadows[i].y)) +
                 ceil(shadows[i].blur * 3.0);
    return ceil(reach);
}

static cairo_surface_t *
drop_shadow_surface(cairo_surface_t *src, int iw, int ih, int cw, int ch,
                    double ox, double oy, double sx, double sy,
                    image_drop_shadow shadow, int *pad_out)
{
    if (cw <= 0 || ch <= 0 || sx <= 0 || sy <= 0) return NULL;
    if (cw > 4096) cw = 4096;
    if (ch > 4096) ch = 4096;
    double sigma = MIN(MAX(shadow.blur, 0), 512.0);
    int pad = (int)ceil(sigma * 3.0) + 2;
    int sw = cw + pad * 2;
    int sh = ch + pad * 2;
    cairo_surface_t *shadow_surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
    if (cairo_surface_status(shadow_surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(shadow_surf);
        return NULL;
    }

    cairo_t *s_cr = cairo_create(shadow_surf);
    cairo_set_operator(s_cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(s_cr);
    cairo_set_operator(s_cr, CAIRO_OPERATOR_OVER);
    cairo_rectangle(s_cr, pad, pad, cw, ch);
    cairo_clip(s_cr);
    cairo_translate(s_cr, pad + ox, pad + oy);
    cairo_scale(s_cr, sx, sy);
    cairo_set_source_surface(s_cr, src, 0, 0);
    cairo_paint(s_cr);
    cairo_destroy(s_cr);

    cairo_surface_flush(shadow_surf);
    guchar *data = cairo_image_surface_get_data(shadow_surf);
    int stride = cairo_image_surface_get_stride(shadow_surf);
    for (int y = 0; y < sh; y++) {
        guchar *row = data + y * stride;
        for (int x = 0; x < sw; x++) {
            guchar *px = row + x * 4;
            double a = px[3] / 255.0 * shadow.color.a;
            px[0] = (guchar)(shadow.color.b * a * 255.0 + 0.5);
            px[1] = (guchar)(shadow.color.g * a * 255.0 + 0.5);
            px[2] = (guchar)(shadow.color.r * a * 255.0 + 0.5);
            px[3] = (guchar)(a * 255.0 + 0.5);
        }
    }
    if (sigma > 0)
        ns_paint_blur_argb(data, stride, sw, sh, sigma);
    cairo_surface_mark_dirty(shadow_surf);
    if (pad_out) *pad_out = pad;
    (void)iw;
    (void)ih;
    return shadow_surf;
}

static void
paint_texture_drop_shadows(cairo_t *cr, cairo_surface_t *surf, const ns_box *b,
                           int iw, int ih, double sx, double sy,
                           double ox, double oy, const char *filter_kw)
{
    const ns_style *st = b ? b->style : NULL;
    rgba current = rgba_of(st ? st->values[NS_CSS_COLOR] : NULL, 0, 0, 0, 1);
    image_drop_shadow shadows[4];
    int n = parse_filter_drop_shadows(filter_kw, current, shadows,
                                      G_N_ELEMENTS(shadows));
    if (n <= 0) return;
    int cw = MAX(1, (int)ceil(b->content_width));
    int ch = MAX(1, (int)ceil(b->content_height));
    for (int i = 0; i < n; i++) {
        int pad = 0;
        cairo_surface_t *shadow =
            drop_shadow_surface(surf, iw, ih, cw, ch, ox, oy, sx, sy,
                                shadows[i], &pad);
        if (!shadow) continue;
        cairo_save(cr);
        cairo_set_source_surface(cr, shadow,
                                 b->x + b->margin.left + b->border.left +
                                     b->padding.left + shadows[i].x - pad,
                                 b->y + b->margin.top + b->border.top +
                                     b->padding.top + shadows[i].y - pad);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(shadow);
    }
}

void
ns_paint_blur_argb(guchar *data, int stride, int w, int h, double sigma)
{
    if (!(sigma >= 0.5)) return;
    if (sigma > 512.0) sigma = 512.0;
    double ideal = sqrt(12.0 * sigma * sigma / 3.0 + 1.0);
    int wl = (int)floor(ideal);
    if (wl % 2 == 0) wl--;
    int wu = wl + 2;
    double m_ideal = (12.0 * sigma * sigma - 3.0 * wl * wl - 12.0 * wl - 9.0) /
                     (-4.0 * wl - 4.0);
    int m = (int)lround(m_ideal);
    for (int i = 0; i < 3; i++) {
        int r = ((i < m ? wl : wu) - 1) / 2;
        box_blur_argb(data, stride, w, h, MIN(r, 512));
    }
}

typedef struct {
    int op;
    double amount;
} image_filter_op;

static void
apply_image_color_ops(guchar *data, int stride, int w, int h,
                      const image_filter_op *ops, int n_ops)
{
    if (n_ops <= 0) return;
    for (int y = 0; y < h; y++) {
        guchar *row = data + y * stride;
        for (int x = 0; x < w; x++) {
            guchar *px = row + x * 4;
            double a = px[3] / 255.0;
            double b = px[0] / 255.0;
            double g = px[1] / 255.0;
            double r = px[2] / 255.0;
            if (a > 0.0001) { r /= a; g /= a; b /= a; }
            for (int oi = 0; oi < n_ops; oi++) {
                double t = ops[oi].amount;
                switch (ops[oi].op) {
                case 1: {
                    double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    r = r * (1 - t) + lum * t;
                    g = g * (1 - t) + lum * t;
                    b = b * (1 - t) + lum * t;
                    break;
                }
                case 2: {
                    double sr = r * 0.393 + g * 0.769 + b * 0.189;
                    double sg = r * 0.349 + g * 0.686 + b * 0.168;
                    double sb = r * 0.272 + g * 0.534 + b * 0.131;
                    r = r * (1 - t) + sr * t;
                    g = g * (1 - t) + sg * t;
                    b = b * (1 - t) + sb * t;
                    break;
                }
                case 3: {
                    r = r * (1 - t) + (1 - r) * t;
                    g = g * (1 - t) + (1 - g) * t;
                    b = b * (1 - t) + (1 - b) * t;
                    break;
                }
                case 4: r *= t; g *= t; b *= t; break;
                case 5:
                    r = (r - 0.5) * t + 0.5;
                    g = (g - 0.5) * t + 0.5;
                    b = (b - 0.5) * t + 0.5;
                    break;
                case 6: {
                    double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    r = lum + (r - lum) * t;
                    g = lum + (g - lum) * t;
                    b = lum + (b - lum) * t;
                    break;
                }
                case 7: {
                    double c = cos(t * G_PI / 180.0);
                    double sn = sin(t * G_PI / 180.0);
                    double hr = r * (0.213 + c * 0.787 - sn * 0.213) +
                                g * (0.715 - c * 0.715 - sn * 0.715) +
                                b * (0.072 - c * 0.072 + sn * 0.928);
                    double hg = r * (0.213 - c * 0.213 + sn * 0.143) +
                                g * (0.715 + c * 0.285 + sn * 0.140) +
                                b * (0.072 - c * 0.072 - sn * 0.283);
                    double hb = r * (0.213 - c * 0.213 - sn * 0.787) +
                                g * (0.715 - c * 0.715 + sn * 0.715) +
                                b * (0.072 + c * 0.928 + sn * 0.072);
                    r = hr;
                    g = hg;
                    b = hb;
                    break;
                }
                case 8:
                    a *= CLAMP(t, 0.0, 1.0);
                    break;
                }
                r = CLAMP(r, 0.0, 1.0);
                g = CLAMP(g, 0.0, 1.0);
                b = CLAMP(b, 0.0, 1.0);
            }
            px[0] = (guchar)(b * a * 255.0 + 0.5);
            px[1] = (guchar)(g * a * 255.0 + 0.5);
            px[2] = (guchar)(r * a * 255.0 + 0.5);
            px[3] = (guchar)(a * 255.0 + 0.5);
        }
    }
}

static double
parse_filter_angle_deg(const char *p)
{
    while (*p && g_ascii_isspace((unsigned char)*p)) p++;
    char *endp = NULL;
    double v = g_ascii_strtod(p, &endp);
    if (!endp || endp == p) return 0;
    if (g_ascii_strncasecmp(endp, "grad", 4) == 0) return v * 0.9;
    if (g_ascii_strncasecmp(endp, "rad", 3) == 0) return v * 180.0 / G_PI;
    if (g_ascii_strncasecmp(endp, "turn", 4) == 0) return v * 360.0;
    return v;
}

static void
filter_apply_drop_shadow(guchar *data, int stride, int w, int h,
                         const image_drop_shadow *shadow, double scale)
{
    guchar *src = g_memdup2(data, (gsize)stride * h);
    double off_x = shadow->x * scale, off_y = shadow->y * scale;
    int dx = isfinite(off_x) ? (int)lround(CLAMP(off_x, -1e6, 1e6)) : 0;
    int dy = isfinite(off_y) ? (int)lround(CLAMP(off_y, -1e6, 1e6)) : 0;
    rgba c = shadow->color;
    for (int y = 0; y < h; y++) {
        guchar *row = data + y * stride;
        int sy = y - dy;
        for (int x = 0; x < w; x++) {
            int sx = x - dx;
            double a = sy >= 0 && sy < h && sx >= 0 && sx < w
                ? src[sy * stride + sx * 4 + 3] / 255.0 * c.a : 0;
            guchar *px = row + x * 4;
            px[0] = (guchar)(c.b * a * 255.0 + 0.5);
            px[1] = (guchar)(c.g * a * 255.0 + 0.5);
            px[2] = (guchar)(c.r * a * 255.0 + 0.5);
            px[3] = (guchar)(a * 255.0 + 0.5);
        }
    }
    if (shadow->blur > 0)
        ns_paint_blur_argb(data, stride, w, h, shadow->blur * scale);
    for (int y = 0; y < h; y++) {
        guchar *row = data + y * stride;
        const guchar *top = src + y * stride;
        for (int x = 0; x < w; x++) {
            double keep = 1.0 - top[x * 4 + 3] / 255.0;
            for (int i = 0; i < 4; i++)
                row[x * 4 + i] = (guchar)MIN(255.0,
                    top[x * 4 + i] + row[x * 4 + i] * keep + 0.5);
        }
    }
    g_free(src);
}

static void
apply_image_filter(guchar *data, int stride, int w, int h, const char *filter,
                   double blur_scale, const rgba *shadow_color)
{
    if (!filter || !*filter) return;
    image_filter_op ops[16];
    int n_ops = 0;
    const char *q = filter;
    while (*q) {
        int op = 0;
        const char *name = NULL;
        const char *body = NULL;
        const char *body_end = NULL;
        gsize nlen = 0;
        const char *next =
            filter_function_next(q, &name, &nlen, &body, &body_end);
        if (!next) break;
        double amt = parse_filter_amount(body, NULL);
        q = next;
        if (filter_name_is(name, nlen, "grayscale")) op = 1;
        else if (filter_name_is(name, nlen, "sepia")) op = 2;
        else if (filter_name_is(name, nlen, "invert")) op = 3;
        else if (filter_name_is(name, nlen, "brightness")) op = 4;
        else if (filter_name_is(name, nlen, "contrast")) op = 5;
        else if (filter_name_is(name, nlen, "saturate")) op = 6;
        else if (filter_name_is(name, nlen, "opacity")) op = 8;
        else if (filter_name_is(name, nlen, "hue-rotate")) {
            op = 7;
            amt = parse_filter_angle_deg(body);
        }
        if (op && (amt >= 0 || op == 7)) {
            if (n_ops == G_N_ELEMENTS(ops)) {
                apply_image_color_ops(data, stride, w, h, ops, n_ops);
                n_ops = 0;
            }
            ops[n_ops].op = op;
            ops[n_ops].amount = amt;
            n_ops++;
            continue;
        }
        image_drop_shadow shadow;
        if (filter_name_is(name, nlen, "blur") && amt > 0) {
            apply_image_color_ops(data, stride, w, h, ops, n_ops);
            n_ops = 0;
            ns_paint_blur_argb(data, stride, w, h, amt * blur_scale);
        } else if (shadow_color &&
                   filter_name_is(name, nlen, "drop-shadow") &&
                   parse_filter_shadow_body(body, body_end, *shadow_color,
                                            &shadow)) {
            apply_image_color_ops(data, stride, w, h, ops, n_ops);
            n_ops = 0;
            filter_apply_drop_shadow(data, stride, w, h, &shadow, blur_scale);
        }
    }
    apply_image_color_ops(data, stride, w, h, ops, n_ops);
}

static const char *
box_clip_path_text(const ns_box *b)
{
    const ns_style *st = b ? b->style : NULL;
    const ns_css_value *v = st ? st->values[NS_CSS_CLIP_PATH] : NULL;
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return NULL;
    const char *cp = v->u.keyword;
    if (!*cp || strcmp(cp, "none") == 0) return NULL;
    return cp;
}

static void
clip_path_reference_boxes(const ns_box *b, double boxes[NS_CLIP_BOX_COUNT][4])
{
    double bx = b->x + b->margin.left, by = b->y + b->margin.top;
    double bw = b->content_width + b->padding.left + b->padding.right +
                b->border.left + b->border.right;
    double bh = b->content_height + b->padding.top + b->padding.bottom +
                b->border.top + b->border.bottom;
    const double rects[NS_CLIP_BOX_COUNT][4] = {
        [NS_CLIP_BOX_MARGIN] = { b->x, b->y, bw + b->margin.left + b->margin.right,
                                 bh + b->margin.top + b->margin.bottom },
        [NS_CLIP_BOX_BORDER] = { bx, by, bw, bh },
        [NS_CLIP_BOX_PADDING] = { bx + b->border.left, by + b->border.top,
                                  bw - b->border.left - b->border.right,
                                  bh - b->border.top - b->border.bottom },
        [NS_CLIP_BOX_CONTENT] = { bx + b->border.left + b->padding.left,
                                  by + b->border.top + b->padding.top,
                                  b->content_width, b->content_height },
    };
    memcpy(boxes, rects, sizeof rects);
}

static void
clip_shape_path(cairo_t *cr, const ns_box *b, const ns_clip_shape *s)
{
    switch (s->kind) {
    case NS_CLIP_SHAPE_BOX: {
        corner_radii r = box_border_radii(b);
        if (s->box == NS_CLIP_BOX_PADDING)
            r = corner_radii_inset(r, b->border.top, b->border.right,
                                   b->border.bottom, b->border.left);
        else if (s->box == NS_CLIP_BOX_CONTENT)
            r = corner_radii_inset(r, b->border.top + b->padding.top,
                                   b->border.right + b->padding.right,
                                   b->border.bottom + b->padding.bottom,
                                   b->border.left + b->padding.left);
        rounded_rect_path(cr, s->x, s->y, s->w, s->h, r);
        break;
    }
    case NS_CLIP_SHAPE_INSET: {
        corner_radii r = { s->rh[0], s->rh[1], s->rh[2], s->rh[3],
                           s->rv[0], s->rv[1], s->rv[2], s->rv[3] };
        rounded_rect_path(cr, s->x, s->y, s->w, s->h, r);
        break;
    }
    case NS_CLIP_SHAPE_ELLIPSE:
        if (s->rx > 0 && s->ry > 0) {
            cairo_save(cr);
            cairo_translate(cr, s->cx, s->cy);
            cairo_scale(cr, s->rx, s->ry);
            cairo_new_sub_path(cr);
            cairo_arc(cr, 0, 0, 1.0, 0, 2 * G_PI);
            cairo_restore(cr);
        } else {
            cairo_rectangle(cr, s->cx, s->cy, 0, 0);
        }
        break;
    case NS_CLIP_SHAPE_POLYGON:
        cairo_set_fill_rule(cr, s->evenodd ? CAIRO_FILL_RULE_EVEN_ODD
                                           : CAIRO_FILL_RULE_WINDING);
        for (guint i = 0; i + 1 < s->points->len; i += 2) {
            double px = g_array_index(s->points, double, i);
            double py = g_array_index(s->points, double, i + 1);
            if (i == 0) cairo_move_to(cr, px, py);
            else        cairo_line_to(cr, px, py);
        }
        cairo_close_path(cr);
        break;
    }
}

static gboolean
apply_clip_path(cairo_t *cr, const ns_box *b)
{
    const char *cp = box_clip_path_text(b);
    if (!cp) return FALSE;
    char *computed = ns_css_shape_computed_serialize(b->style, NULL,
                                                     NS_CSS_CLIP_PATH);
    double boxes[NS_CLIP_BOX_COUNT][4];
    clip_path_reference_boxes(b, boxes);
    ns_clip_shape shape;
    gboolean ok = ns_css_clip_shape_resolve(computed ? computed : cp, boxes,
                                            &shape);
    g_free(computed);
    if (!ok) return FALSE;
    cairo_fill_rule_t rule = cairo_get_fill_rule(cr);
    cairo_new_path(cr);
    clip_shape_path(cr, b, &shape);
    cairo_clip(cr);
    cairo_set_fill_rule(cr, rule);
    ns_clip_shape_clear(&shape);
    return TRUE;
}

static gboolean
apply_box_content_clip(cairo_t *cr, const ns_box *b)
{
    if (!b) return FALSE;
    gboolean clipped = FALSE;
    if (isnan(b->x) || isnan(b->y) ||
        isnan(b->content_width) || isnan(b->content_height))
        return FALSE;
    corner_radii radii = box_border_radii(b);
    if (!corner_radii_zero(radii)) {
        rounded_rect_path(cr, b->x, b->y,
                          b->content_width, b->content_height, radii);
        cairo_clip(cr);
        clipped = TRUE;
    }
    return apply_clip_path(cr, b) || clipped;
}

#define SVG_RASTERS 4
#define SVG_RASTER_MAX_PIXELS (4096.0 * 4096.0)
#define SVG_RASTER_REUSE 1.5

typedef struct {
    cairo_surface_t *surf;
    char            *filter;
    int              pw, ph;
    double           vw, vh;
    guint            used;
} svg_raster;

typedef struct {
    cairo_surface_t *plain;
    char            *filter;
    cairo_surface_t *filtered;
    svg_raster       svg[SVG_RASTERS];
    guint            svg_clock;
} texture_surfaces;

static void
texture_surfaces_free(gpointer data)
{
    texture_surfaces *ts = data;
    if (ts->plain) cairo_surface_destroy(ts->plain);
    if (ts->filtered) cairo_surface_destroy(ts->filtered);
    g_free(ts->filter);
    for (int i = 0; i < SVG_RASTERS; i++) {
        if (ts->svg[i].surf) cairo_surface_destroy(ts->svg[i].surf);
        g_free(ts->svg[i].filter);
    }
    g_free(ts);
}

static texture_surfaces *
texture_surfaces_for(ns_texture *tex)
{
    texture_surfaces *ts = ns_texture_get_user_data(tex);
    if (!ts) {
        ts = g_new0(texture_surfaces, 1);
        ns_texture_set_user_data(tex, ts, texture_surfaces_free);
    }
    return ts;
}

static cairo_surface_t *
texture_surface_create(ns_texture *tex, int iw, int ih, const char *filter_kw)
{
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       iw, ih);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    guchar *dst = cairo_image_surface_get_data(surf);
    int dst_stride = cairo_image_surface_get_stride(surf);
    ns_texture_download(tex, dst, (gsize)dst_stride);
    if (filter_kw)
        apply_image_filter(dst, dst_stride, iw, ih, filter_kw, 1.0, NULL);
    cairo_surface_mark_dirty(surf);
    return surf;
}

static cairo_surface_t *
texture_surface_cached(ns_texture *tex, const char *filter_kw)
{
    int iw = ns_texture_get_width(tex);
    int ih = ns_texture_get_height(tex);
    if (iw <= 0 || ih <= 0) return NULL;
    texture_surfaces *ts = texture_surfaces_for(tex);
    if (!filter_kw) {
        if (!ts->plain) ts->plain = texture_surface_create(tex, iw, ih, NULL);
        return ts->plain;
    }
    if (ts->filtered && g_strcmp0(ts->filter, filter_kw) == 0)
        return ts->filtered;
    cairo_surface_t *surf = texture_surface_create(tex, iw, ih, filter_kw);
    if (!surf) return NULL;
    if (ts->filtered) cairo_surface_destroy(ts->filtered);
    g_free(ts->filter);
    ts->filtered = surf;
    ts->filter = g_strdup(filter_kw);
    return surf;
}

static double
object_position_offset(const ns_style *st, ns_css_prop prop,
                       double box_size, double object_size)
{
    double delta = box_size - object_size;
    const ns_css_value *v = st ? st->values[prop] : NULL;
    if (v && v->kind == NS_CSS_V_LENGTH) {
        if (v->u.length.unit == NS_CSS_UNIT_PERCENT)
            return delta * v->u.length.v / 100.0;
        return bg_size_px(v->u.length.v, v->u.length.unit, box_size);
    }
    return delta * 0.5;
}

static void
texture_fit(const ns_box *b, int iw, int ih, double *out_sx, double *out_sy,
            double *out_ox, double *out_oy)
{
    const ns_style *st = b->style;
    double cw = b->content_width, ch = b->content_height;
    double sx = cw / iw, sy = ch / ih;
    const char *fit = (st && st->values[NS_CSS_OBJECT_FIT] &&
                       st->values[NS_CSS_OBJECT_FIT]->kind == NS_CSS_V_KEYWORD)
                      ? st->values[NS_CSS_OBJECT_FIT]->u.keyword : NULL;
    if (!fit && b->kind == NS_BOX_VIDEO) fit = "contain";
    double ox = 0, oy = 0;
    if (fit && strcmp(fit, "fill") != 0) {
        double s;
        if (strcmp(fit, "contain") == 0)         s = MIN(sx, sy);
        else if (strcmp(fit, "cover") == 0)      s = MAX(sx, sy);
        else if (strcmp(fit, "none") == 0)       s = 1.0;
        else if (strcmp(fit, "scale-down") == 0) s = MIN(1.0, MIN(sx, sy));
        else                                     s = -1;
        if (s > 0) {
            sx = sy = s;
            ox = object_position_offset(st, NS_CSS_OBJECT_POSITION_X,
                                        cw, iw * s);
            oy = object_position_offset(st, NS_CSS_OBJECT_POSITION_Y,
                                        ch, ih * s);
        }
    }
    *out_sx = sx;
    *out_sy = sy;
    *out_ox = ox;
    *out_oy = oy;
}

static gboolean
video_layer_target(cairo_t *cr, const ns_box *b)
{
    if (!g_paint_video_layer_gen || g_layers.mode != PAINT_LAYERS_DOC ||
        cr != g_layers.doc ||
        cairo_get_group_target(cr) != cairo_get_target(cr))
        return FALSE;
    cairo_matrix_t m;
    cairo_get_matrix(cr, &m);
    if (m.xy != 0 || m.yx != 0 || m.xx <= 0 || m.yy <= 0) return FALSE;
    const ns_style *st = b->style;
    return !(st && st->values[NS_CSS_FILTER] &&
             st->values[NS_CSS_FILTER]->kind == NS_CSS_V_KEYWORD &&
             st->values[NS_CSS_FILTER]->u.keyword &&
             strcmp(st->values[NS_CSS_FILTER]->u.keyword, "none") != 0);
}

static gboolean
paint_video_layer_hole(cairo_t *cr, const ns_box *b, ns_video *v,
                       ns_texture *tex)
{
    int iw = ns_texture_get_width(tex);
    int ih = ns_texture_get_height(tex);
    if (iw <= 0 || ih <= 0 || b->content_width <= 0 ||
        b->content_height <= 0 || !video_layer_target(cr, b))
        return FALSE;
    double sx, sy, ox, oy;
    texture_fit(b, iw, ih, &sx, &sy, &ox, &oy);
    double cx = b->x + b->margin.left + b->border.left + b->padding.left;
    double cy = b->y + b->margin.top  + b->border.top  + b->padding.top;
    double x0 = cx + ox, y0 = cy + oy;
    double x1 = x0 + iw * sx, y1 = y0 + ih * sy;
    cairo_save(cr);
    apply_box_content_clip(cr, b);
    cairo_rectangle(cr, cx, cy, b->content_width, b->content_height);
    cairo_clip(cr);
    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_user_to_device(cr, &x0, &y0);
    cairo_user_to_device(cr, &x1, &y1);
    v->layer_x = x0;
    v->layer_y = y0 + g_paint_video_tile_y;
    v->layer_w = x1 - x0;
    v->layer_h = y1 - y0;
    v->layer_gen = g_paint_video_layer_gen;
    return TRUE;
}

static cairo_surface_t *
svg_raster_cached(ns_texture *tex, GBytes *svg, double vw, double vh,
                  int pw, int ph, const char *filter_kw)
{
    texture_surfaces *ts = texture_surfaces_for(tex);
    svg_raster *slot = &ts->svg[0], *hit = NULL;
    for (int i = 0; i < SVG_RASTERS; i++) {
        svg_raster *r = &ts->svg[i];
        if (r->surf && r->vw == vw && r->vh == vh &&
            r->pw >= pw && r->ph >= ph &&
            r->pw <= pw * SVG_RASTER_REUSE + 1 &&
            r->ph <= ph * SVG_RASTER_REUSE + 1 &&
            g_strcmp0(r->filter, filter_kw) == 0 &&
            (!hit || r->pw < hit->pw))
            hit = r;
        if (r->used < slot->used) slot = r;
    }
    if (hit) {
        hit->used = ++ts->svg_clock;
        return hit->surf;
    }
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    cairo_t *rc = cairo_create(surf);
    cairo_scale(rc, pw / vw, ph / vh);
    gsize len = 0;
    const guchar *data = g_bytes_get_data(svg, &len);
    gboolean drawn = ns_svg_render_bytes(rc, data, len, vw, vh);
    cairo_destroy(rc);
    if (!drawn) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    cairo_surface_flush(surf);
    if (filter_kw)
        apply_image_filter(cairo_image_surface_get_data(surf),
                           cairo_image_surface_get_stride(surf), pw, ph,
                           filter_kw, 1.0, NULL);
    cairo_surface_mark_dirty(surf);
    if (slot->surf) cairo_surface_destroy(slot->surf);
    g_free(slot->filter);
    *slot = (svg_raster){ .surf = surf, .filter = g_strdup(filter_kw),
                          .pw = pw, .ph = ph, .vw = vw, .vh = vh,
                          .used = ++ts->svg_clock };
    return surf;
}

static gboolean
svg_raster_size(cairo_t *cr, double vw, double vh, int iw, int ih,
                int *out_pw, int *out_ph)
{
    double ax = vw, ay = 0, bx = 0, by = vh;
    cairo_user_to_device_distance(cr, &ax, &ay);
    cairo_user_to_device_distance(cr, &bx, &by);
    double pw = hypot(ax, ay), ph = hypot(bx, by);
    if (!(pw >= 1) || !(ph >= 1)) return FALSE;
    if (pw * ph > SVG_RASTER_MAX_PIXELS) {
        double k = sqrt(SVG_RASTER_MAX_PIXELS / (pw * ph));
        pw *= k;
        ph *= k;
    }
    *out_pw = (int)ceil(pw - 0.01);
    *out_ph = (int)ceil(ph - 0.01);
    gboolean upscaled = *out_pw > iw || *out_ph > ih;
    gboolean reshaped = fabs(vw * ih - vh * iw) > 0.01 * vw * ih;
    return upscaled || reshaped;
}

static gboolean
paint_texture(cairo_t *cr, const ns_box *b, ns_texture *tex, GBytes *svg)
{
    int iw = ns_texture_get_width(tex);
    int ih = ns_texture_get_height(tex);
    if (iw <= 0 || ih <= 0) return FALSE;
    if (b->content_width <= 0 || b->content_height <= 0) return FALSE;
    const ns_style *st = b->style;
    const char *filter_kw = NULL;
    if (st && st->values[NS_CSS_FILTER] &&
        st->values[NS_CSS_FILTER]->kind == NS_CSS_V_KEYWORD &&
        st->values[NS_CSS_FILTER]->u.keyword) {
        filter_kw = st->values[NS_CSS_FILTER]->u.keyword;
    }
    const char *surface_filter =
        filter_has_bitmap_effect(filter_kw) ? filter_kw : NULL;
    cairo_surface_t *surf = texture_surface_cached(tex, surface_filter);
    if (!surf) return FALSE;
    double cw = b->content_width, ch = b->content_height;
    double sx, sy, ox, oy;
    texture_fit(b, iw, ih, &sx, &sy, &ox, &oy);
    double cx = b->x + b->margin.left + b->border.left + b->padding.left;
    double cy = b->y + b->margin.top  + b->border.top  + b->padding.top;
    paint_texture_drop_shadows(cr, surf, b, iw, ih, sx, sy, ox, oy, filter_kw);
    apply_box_content_clip(cr, b);
    cairo_rectangle(cr, cx, cy, cw, ch);
    cairo_clip(cr);
    double vw = iw * sx, vh = ih * sy;
    int pw = 0, ph = 0;
    cairo_surface_t *vector =
        svg && svg_raster_size(cr, vw, vh, iw, ih, &pw, &ph)
        ? svg_raster_cached(tex, svg, vw, vh, pw, ph, surface_filter) : NULL;
    cairo_translate(cr, cx + ox, cy + oy);
    if (vector) {
        surf = vector;
        cairo_scale(cr, vw / cairo_image_surface_get_width(vector),
                    vh / cairo_image_surface_get_height(vector));
    } else {
        cairo_scale(cr, sx, sy);
    }
    cairo_set_source_surface(cr, surf, 0, 0);
    ns_video *pv = b->media ? (ns_video *)b->media->video : NULL;
    if (pv && pv->playing && pv->frame_texture == tex)
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    const ns_css_value *ir = st ? st->values[NS_CSS_IMAGE_RENDERING] : NULL;
    if (ir && ir->kind == NS_CSS_V_KEYWORD && ir->u.keyword &&
        (strcmp(ir->u.keyword, "pixelated") == 0 ||
         strcmp(ir->u.keyword, "crisp-edges") == 0))
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    return TRUE;
}

#define NS_BROKEN_IMAGE_ICON 16.0

static void
broken_image_page_path(cairo_t *cr)
{
    cairo_new_path(cr);
    cairo_move_to(cr, 0.5, 0.5);
    cairo_line_to(cr, 11.5, 0.5);
    cairo_line_to(cr, 15.5, 4.5);
    cairo_line_to(cr, 15.5, 15.5);
    cairo_line_to(cr, 0.5, 15.5);
    cairo_close_path(cr);
}

static void
paint_broken_image_icon(cairo_t *cr, double x, double y)
{
    cairo_save(cr);
    cairo_translate(cr, x, y);
    broken_image_page_path(cr);
    cairo_set_source_rgb(cr, 0.79, 0.85, 0.96);
    cairo_fill_preserve(cr);
    cairo_save(cr);
    cairo_clip(cr);
    cairo_save(cr);
    cairo_translate(cr, 5.5, 16.5);
    cairo_scale(cr, 7.5, 6.0);
    cairo_arc(cr, 0, 0, 1, 0, 2 * G_PI);
    cairo_restore(cr);
    cairo_set_source_rgb(cr, 0.35, 0.68, 0.22);
    cairo_fill(cr);
    cairo_save(cr);
    cairo_translate(cr, 6.5, 4.5);
    cairo_scale(cr, 2.5, 1.5);
    cairo_arc(cr, 0, 0, 1, 0, 2 * G_PI);
    cairo_restore(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    cairo_restore(cr);
    broken_image_page_path(cr);
    cairo_set_source_rgb(cr, 0.58, 0.58, 0.59);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_move_to(cr, 11.5, 0.5);
    cairo_line_to(cr, 11.5, 4.5);
    cairo_line_to(cr, 15.5, 4.5);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.86, 0.86, 0.86);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.58, 0.58, 0.59);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static gboolean
image_extent_specified(const ns_box *b, ns_css_prop prop, const char *attr)
{
    const ns_css_value *v = b->style ? b->style->values[prop] : NULL;
    if (v && (v->kind == NS_CSS_V_LENGTH || v->kind == NS_CSS_V_CALC))
        return TRUE;
    return b->dom && ns_element_get_attr(b->dom, attr) != NULL;
}

static gboolean
image_request_errored(const ns_box *b, const ns_image *img)
{
    if (img && img->failed) return TRUE;
    if (!b->dom) return FALSE;
    if (ns_element_get_attr(b->dom, "src") ||
        ns_element_get_attr(b->dom, "srcset"))
        return TRUE;
    return ns_node_is_element_named(b->dom->parent, "picture");
}

static void
paint_image_alt_text(cairo_t *cr, const ns_box *b, const char *alt,
                     double x, double y, double indent)
{
    const ns_css_value *ws = b->style ? b->style->values[NS_CSS_WHITE_SPACE] : NULL;
    gboolean keep_spaces = keyword_is(ws, "pre") || keyword_is(ws, "pre-wrap") ||
                           keyword_is(ws, "break-spaces");
    gboolean wrap = !keyword_is(ws, "nowrap") && !keyword_is(ws, "pre");
    GString *text = g_string_new(NULL);
    for (const char *p = alt; *p; p++) {
        gboolean space = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r';
        if (!space || keep_spaces) g_string_append_c(text, *p);
        else if (text->len && text->str[text->len - 1] != ' ')
            g_string_append_c(text, ' ');
    }
    NsPangoLayout *layout = paint_create_layout();
    ns_paint_apply_inline_font(layout, b->style);
    ns_pango_layout_set_text(layout, text->str, -1);
    g_string_free(text, TRUE);
    if (wrap)
        ns_pango_layout_set_width(layout,
            (int)(b->content_width * NS_PANGO_SCALE));
    ns_pango_layout_set_indent(layout, (int)(indent * NS_PANGO_SCALE));
    rgba fg = rgba_anim(b, NS_CSS_ANIM_TARGET_COLOR,
                        b->style ? b->style->values[NS_CSS_COLOR] : NULL,
                        0, 0, 0, 1);
    set_source_rgba(cr, fg);
    cairo_move_to(cr, x, y);
    ns_pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static void
paint_image_fallback(cairo_t *cr, const ns_box *b, const ns_image *img)
{
    double w = b->content_width;
    double h = b->content_height;
    if (!(w > 0) || !(h > 0)) return;
    double cx = b->x + b->margin.left + b->border.left + b->padding.left;
    double cy = b->y + b->margin.top  + b->border.top  + b->padding.top;
    const char *alt = b->dom ? ns_element_get_attr(b->dom, "alt") : NULL;
    gboolean has_alt = alt && *alt;
    gboolean sized = image_extent_specified(b, NS_CSS_WIDTH, "width") &&
                     image_extent_specified(b, NS_CSS_HEIGHT, "height");
    gboolean icon_fits = w >= NS_BROKEN_IMAGE_ICON + 2 &&
                         h >= NS_BROKEN_IMAGE_ICON + 2;
    cairo_rectangle(cr, cx, cy, w, h);
    cairo_clip(cr);
    if (sized && !has_alt) {
        cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, cx + 0.5, cy + 0.5, w - 1, h - 1);
        cairo_stroke(cr);
        if (icon_fits && image_request_errored(b, img))
            paint_broken_image_icon(cr, cx + 2, cy + 2);
        return;
    }
    double indent = 0;
    if (icon_fits) {
        paint_broken_image_icon(cr, cx, cy);
        indent = NS_BROKEN_IMAGE_ICON;
    }
    if (has_alt) paint_image_alt_text(cr, b, alt, cx, cy, indent);
}

static const ns_box *g_svg_foreign_owner;
static cairo_matrix_t g_svg_foreign_base;

static void
record_svg_foreign_xform(const ns_box *svg_box, guint index, cairo_t *cr)
{
    ns_box_media *m = svg_box->media;
    if (!m->svg_foreign_xform)
        m->svg_foreign_xform = g_new0(double, (gsize)m->svg_foreign->len * 7);
    cairo_matrix_t inv = g_svg_foreign_base;
    if (cairo_matrix_invert(&inv) != CAIRO_STATUS_SUCCESS) return;
    cairo_matrix_t cur, local;
    cairo_get_matrix(cr, &cur);
    cairo_matrix_multiply(&local, &cur, &inv);
    double *t = m->svg_foreign_xform + index * 7;
    t[0] = 1;
    t[1] = local.xx;
    t[2] = local.yx;
    t[3] = local.xy;
    t[4] = local.yy;
    t[5] = local.x0;
    t[6] = local.y0;
}

static void
paint_svg_foreign(cairo_t *cr, const ns_node *fo, double width, double height,
                  void *user_data)
{
    (void)width;
    (void)height;
    (void)user_data;
    const ns_box *svg_box = g_svg_foreign_owner;
    GPtrArray *roots = svg_box && svg_box->media
        ? svg_box->media->svg_foreign : NULL;
    if (!roots) return;
    for (guint i = 0; i < roots->len; i++) {
        const ns_box *root = g_ptr_array_index(roots, i);
        if (root->dom == fo) {
            record_svg_foreign_xform(svg_box, i, cr);
            paint_walk(cr, root, NULL);
            return;
        }
    }
}

static void
paint_svg(cairo_t *cr, const ns_box *b)
{
    if (!b->dom) return;
    double w = b->content_width;
    double h = b->content_height;
    if (w <= 0 || h <= 0) return;
    double x = b->x + b->margin.left + b->border.left + b->padding.left;
    double y = b->y + b->margin.top  + b->border.top  + b->padding.top;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    const ns_box *outer = g_svg_foreign_owner;
    cairo_matrix_t outer_base = g_svg_foreign_base;
    g_svg_foreign_owner = b;
    cairo_get_matrix(cr, &g_svg_foreign_base);
    ns_svg_set_foreign_painter(paint_svg_foreign, NULL);
    ns_svg_render_node(cr, b->dom, w, h, b->svg_styles, b->style);
    g_svg_foreign_owner = outer;
    g_svg_foreign_base = outer_base;
    if (!outer) ns_svg_set_foreign_painter(NULL, NULL);
    cairo_restore(cr);
}

static void
paint_math(cairo_t *cr, const ns_box *b)
{
    if (!b->dom) return;
    const ns_style *s = b->style;
    double fpx = length_or(s ? s->values[NS_CSS_FONT_SIZE] : NULL, 16);
    double r = 0, g = 0, bl = 0, a = 1;
    const ns_css_value *col = s ? s->values[NS_CSS_COLOR] : NULL;
    if (col && col->kind == NS_CSS_V_COLOR) {
        r = col->u.color.r / 255.0;
        g = col->u.color.g / 255.0;
        bl = col->u.color.b / 255.0;
        a = col->u.color.a / 255.0;
    }
    double ox = b->x + b->margin.left + b->border.left + b->padding.left;
    double oy = b->y + b->margin.top + b->border.top + b->padding.top;
    ns_math_paint(cr, b->dom, ox, oy, fpx, r, g, bl, a);
}

static void
paint_image(cairo_t *cr, const ns_box *b)
{
    if (ns_node_is_element_named(b->dom, "canvas")) return;
    const ns_image *img = NULL;
    if (b->dom && g_paint_js)
        img = ns_js_image_for_node(g_paint_js, b->dom);
    if (!img && b->media)
        img = b->media->image;
    cairo_save(cr);
    if (img && img->loaded && img->texture) {
        paint_texture(cr, b, img->texture, img->svg_source);
    } else if ((img && img->failed) || !b->media || !b->media->image_src ||
               !*b->media->image_src) {
        paint_image_fallback(cr, b, img);
    }
    cairo_restore(cr);
}

static double
box_content_x(const ns_box *b)
{
    return b->x + b->margin.left + b->border.left + b->padding.left;
}

static double
box_content_y(const ns_box *b)
{
    return b->y + b->margin.top + b->border.top + b->padding.top;
}

static void
paint_video_caption(cairo_t *cr, const ns_box *b, const char *text)
{
    char **lines = g_strsplit(text, "\n", -1);
    guint nl = g_strv_length(lines);
    if (nl == 0) { g_strfreev(lines); return; }

    int fs = (int)(b->content_height * 0.07 + 0.5);
    if (fs < 11) fs = 11;
    if (fs > 26) fs = 26;
    NsPangoFontDescription *fd = ns_pango_font_description_from_string("sans");
    ns_pango_font_description_set_absolute_size(fd, ns_paint_pango_font_size(fs * 4.0 / 3.0));
    ns_pango_font_description_set_weight(fd, NS_PANGO_WEIGHT_MEDIUM);

    NsPangoLayout **lays = g_new0(NsPangoLayout *, nl);
    int *lw = g_new0(int, nl);
    int *lh = g_new0(int, nl);
    double pad = 3.0, gap = 1.0, total = 0;
    for (guint i = 0; i < nl; i++) {
        NsPangoLayout *layout = paint_create_layout();
        ns_pango_layout_set_font_description(layout, fd);
        ns_pango_layout_set_text(layout, lines[i][0] ? lines[i] : " ", -1);
        ns_pango_layout_get_pixel_size(layout, &lw[i], &lh[i]);
        lays[i] = layout;
        total += lh[i] + 2 * pad + (i ? gap : 0);
    }
    double x0 = box_content_x(b), y0 = box_content_y(b);
    double y = y0 + b->content_height - b->content_height * 0.05 - total;
    if (y < y0 + 2) y = y0 + 2;
    cairo_save(cr);
    for (guint i = 0; i < nl; i++) {
        double bw = lw[i] + 2 * pad;
        double bx = x0 + (b->content_width - bw) / 2.0;
        if (bx < x0) bx = x0;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_rectangle(cr, bx, y, bw, lh[i] + 2 * pad);
        cairo_fill(cr);
        cairo_move_to(cr, bx + pad, y + pad);
        cairo_set_source_rgb(cr, 1, 1, 1);
        ns_pango_cairo_show_layout(cr, lays[i]);
        y += lh[i] + 2 * pad + gap;
        g_object_unref(lays[i]);
    }
    cairo_restore(cr);
    ns_pango_font_description_free(fd);
    g_free(lays);
    g_free(lw);
    g_free(lh);
    g_strfreev(lines);
}

static void
paint_video(cairo_t *cr, const ns_box *b)
{
    if (b->media && b->media->video_audio_src && !b->media->video_src) {
        double x = box_content_x(b), y = box_content_y(b);
        double w = b->content_width, h = b->content_height;
        if (!(w > 0) || !(h > 0)) return;
        cairo_save(cr);
        corner_radii radii = corner_radii_uniform(4);
        rounded_rect_path(cr, x, y, w, h, radii);
        cairo_set_source_rgb(cr, 0.96, 0.97, 0.98);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.55, 0.58, 0.62);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        double cy = y + h / 2.0;
        double play_x = x + 13.0;
        double play_r = h * 0.28;
        if (play_r > 9) play_r = 9;
        if (play_r < 5) play_r = 5;
        cairo_arc(cr, play_x, cy, play_r, 0, 2 * G_PI);
        cairo_set_source_rgb(cr, 0.20, 0.23, 0.26);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, play_x - play_r * 0.28, cy - play_r * 0.45);
        cairo_line_to(cr, play_x + play_r * 0.45, cy);
        cairo_line_to(cr, play_x - play_r * 0.28, cy + play_r * 0.45);
        cairo_close_path(cr);
        cairo_fill(cr);

        const char *dur = b->dom ? ns_element_get_attr(b->dom, "data-durationhint") : NULL;
        char dtext[32] = "";
        if (dur && *dur) {
            char *end = NULL;
            double sec_d = g_ascii_strtod(dur, &end);
            if (end != dur && sec_d >= 0) {
                int sec = (int)(sec_d + 0.5);
                g_snprintf(dtext, sizeof dtext, "%d:%02d", sec / 60, sec % 60);
            }
        }
        double text_w = 0;
        if (dtext[0]) {
            NsPangoLayout *layout = paint_create_layout();
            NsPangoFontDescription *fd = ns_pango_font_description_from_string("sans");
            ns_pango_font_description_set_absolute_size(fd, ns_paint_pango_font_size(12));
            ns_pango_layout_set_font_description(layout, fd);
            ns_pango_layout_set_text(layout, dtext, -1);
            int tw = 0, th = 0;
            ns_pango_layout_get_pixel_size(layout, &tw, &th);
            text_w = tw + 10;
            cairo_move_to(cr, x + w - tw - 8, y + (h - th) / 2.0);
            cairo_set_source_rgb(cr, 0.18, 0.20, 0.23);
            ns_pango_cairo_show_layout(cr, layout);
            ns_pango_font_description_free(fd);
            g_object_unref(layout);
        }

        double tx0 = x + 31.0;
        double tx1 = x + w - (dtext[0] ? text_w + 8 : 12);
        if (tx1 > tx0 + 12) {
            cairo_set_source_rgb(cr, 0.72, 0.74, 0.77);
            cairo_set_line_width(cr, 3.0);
            cairo_move_to(cr, tx0, cy);
            cairo_line_to(cr, tx1, cy);
            cairo_stroke(cr);
            cairo_arc(cr, tx0, cy, 3.5, 0, 2 * G_PI);
            cairo_set_source_rgb(cr, 0.20, 0.23, 0.26);
            cairo_fill(cr);
        }
        cairo_restore(cr);
        return;
    }
    ns_video *v = b->media ? b->media->video : NULL;
    ns_texture *tex = v ? (v->frame_texture ? v->frame_texture
                                            : v->poster_texture)
                        : NULL;
    if (v && g_layers.mode == PAINT_LAYERS_OFF)
        ns_video_note_painted(v);
    ns_image *bgimg = b->media ? b->media->bg_image : NULL;
    gboolean bg_painted = bgimg && bgimg->loaded && bgimg->texture;
    if (!bg_painted && b->media && b->media->bg_layer_images) {
        for (guint li = 0; li < b->media->bg_layer_images->len; li++) {
            ns_image *limg = g_ptr_array_index(b->media->bg_layer_images, li);
            if (limg && limg->loaded && limg->texture) {
                bg_painted = TRUE;
                break;
            }
        }
    }
    cairo_save(cr);
    gboolean layered = tex && tex == v->frame_texture &&
                       paint_video_layer_hole(cr, b, v, tex);
    if (tex) {
        if (!layered) paint_texture(cr, b, tex, NULL);
    } else if (!bg_painted && b->dom &&
               ns_element_get_attr(b->dom, "controls")) {
        cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);
        cairo_rectangle(cr, box_content_x(b), box_content_y(b),
                        b->content_width, b->content_height);
        cairo_fill(cr);
    }
    cairo_restore(cr);

    const char *cue = ns_video_active_cue_text(v);
    if (cue && *cue && b->content_width > 24 && b->content_height > 24)
        paint_video_caption(cr, b, cue);
}

static void
roman_numeral(int n, gboolean upper, char *out, gsize sz)
{
    static const int vals[]  = {1000,900,500,400,100,90,50,40,10,9,5,4,1};
    static const char *upr[] = {"M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I"};
    static const char *lwr[] = {"m","cm","d","cd","c","xc","l","xl","x","ix","v","iv","i"};
    GString *s = g_string_new(NULL);
    if (n < 1 || n > 3999) {
        g_snprintf(out, sz, "%d", n);
        g_string_free(s, TRUE);
        return;
    }
    for (int i = 0; i < (int)(sizeof vals / sizeof vals[0]); i++) {
        while (n >= vals[i]) {
            g_string_append(s, upper ? upr[i] : lwr[i]);
            n -= vals[i];
        }
    }
    g_strlcpy(out, s->str, sz);
    g_string_free(s, TRUE);
}

static void
alpha_label(int n, gboolean upper, char *out, gsize sz)
{
    if (sz == 0) return;
    if (n < 1) { g_strlcpy(out, "?", sz); return; }
    char buf[16];
    int p = 0;
    while (n > 0 && p < (int)sizeof buf - 1) {
        n--;
        buf[p++] = (char)((upper ? 'A' : 'a') + (n % 26));
        n /= 26;
    }
    buf[p] = '\0';
    int len = p < (int)sz - 1 ? p : (int)sz - 1;
    for (int i = 0; i < len; i++) out[i] = buf[p - 1 - i];
    out[len] = '\0';
}

static const char *
ordered_marker_kind(const char *style_kw)
{
    static const char *const kinds[] = {
        "decimal",
        "decimal-leading-zero",
        "upper-alpha", "lower-alpha",
        "upper-latin", "lower-latin",
        "upper-roman", "lower-roman",
        "lower-greek",
    };
    if (!style_kw) return NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(kinds); i++)
        if (strcmp(style_kw, kinds[i]) == 0) return kinds[i];
    return NULL;
}

static gboolean
marker_is_ordered(const ns_node *li, const char *style_kw)
{
    if (!style_kw)
        return li->parent && li->parent->name &&
               strcmp(li->parent->name, "ol") == 0;
    return strcmp(style_kw, "disc") != 0 && strcmp(style_kw, "circle") != 0 &&
           strcmp(style_kw, "square") != 0 && strcmp(style_kw, "none") != 0 &&
           strncmp(style_kw, "disclosure-", 11) != 0;
}

static const char *
ordered_kind_from_type_attr(const char *type_attr)
{
    if (!type_attr || !*type_attr) return NULL;
    switch (type_attr[0]) {
    case 'A': return "upper-alpha";
    case 'a': return "lower-alpha";
    case 'I': return "upper-roman";
    case 'i': return "lower-roman";
    default:  return "decimal";
    }
}

static void format_ordered_label(const char *kind, int n,
                                 char *out, gsize out_sz);

static int
list_item_count(const ns_node *parent)
{
    int total = 0;
    for (const ns_node *p = parent ? parent->first_child : NULL;
         p; p = p->next_sibling) {
        if (ns_node_is_element_named(p, "li")) total++;
    }
    return total;
}

static GHashTable *g_list_ordinals;
static GHashTable *g_list_next_ordinal;
static int         g_list_ordinal_scope;

void
ns_paint_list_ordinals_begin(void)
{
    if (g_list_ordinal_scope++ > 0) return;
    g_list_ordinals = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_list_next_ordinal = g_hash_table_new(g_direct_hash, g_direct_equal);
}

void
ns_paint_list_ordinals_end(void)
{
    if (g_list_ordinal_scope == 0 || --g_list_ordinal_scope > 0) return;
    g_clear_pointer(&g_list_ordinals, g_hash_table_destroy);
    g_clear_pointer(&g_list_next_ordinal, g_hash_table_destroy);
}

static int
list_item_number_children(const ns_node *parent)
{
    const char *start_attr = ns_element_get_attr(parent, "start");
    int start = start_attr
        ? ns_parse_int(start_attr, 1, -1000000, 1000000)
        : 1;
    gboolean reversed = ns_element_get_attr(parent, "reversed") != NULL;
    int current = reversed && !start_attr ? list_item_count(parent) : start;
    for (const ns_node *p = parent->first_child; p; p = p->next_sibling) {
        if (!ns_node_is_element_named(p, "li")) continue;
        const char *val = ns_element_get_attr(p, "value");
        int ordinal = val ? ns_parse_int(val, current, -1000000, 1000000)
                          : current;
        g_hash_table_insert(g_list_ordinals, (gpointer)p,
                            GINT_TO_POINTER(ordinal));
        current = ordinal + (reversed ? -1 : 1);
    }
    return current;
}

static int
list_item_ordinal_cached(const ns_node *li, const ns_node *parent)
{
    gpointer next;
    if (!g_hash_table_lookup_extended(g_list_next_ordinal, parent, NULL,
                                      &next)) {
        next = GINT_TO_POINTER(list_item_number_children(parent));
        g_hash_table_insert(g_list_next_ordinal, (gpointer)parent, next);
    }
    gpointer ordinal;
    if (g_hash_table_lookup_extended(g_list_ordinals, li, NULL, &ordinal))
        return GPOINTER_TO_INT(ordinal);
    return GPOINTER_TO_INT(next);
}

static int
list_item_ordinal(const ns_node *li)
{
    const ns_node *parent = li ? li->parent : NULL;
    if (!parent || !parent->name) return 1;
    if (g_list_ordinals) return list_item_ordinal_cached(li, parent);
    const char *start_attr = ns_element_get_attr(parent, "start");
    int start = start_attr
        ? ns_parse_int(start_attr, 1, -1000000, 1000000)
        : 1;
    gboolean reversed = ns_element_get_attr(parent, "reversed") != NULL;
    int current = reversed && !start_attr ? list_item_count(parent) : start;
    for (const ns_node *p = parent->first_child; p; p = p->next_sibling) {
        if (!ns_node_is_element_named(p, "li")) continue;
        const char *val = ns_element_get_attr(p, "value");
        int ordinal = val ? ns_parse_int(val, current, -1000000, 1000000)
                          : current;
        if (p == li) return ordinal;
        current = ordinal + (reversed ? -1 : 1);
    }
    return current;
}

static const char *
marker_default_kind(const ns_node *li, const char *style_kw)
{
    if (style_kw && ordered_marker_kind(style_kw)) return style_kw;
    const ns_node *parent = li ? li->parent : NULL;
    if (parent && parent->name && strcmp(parent->name, "ol") == 0)
        return ordered_kind_from_type_attr(ns_element_get_attr(parent, "type"));
    return style_kw;
}

static void
marker_append_escaped_string(GString *out, const char *start, gsize len)
{
    char *raw = g_strndup(start, len);
    for (const char *r = raw; *r; )
        ns_css_append_unescaped(out, &r);
    g_free(raw);
}

static char *
marker_counter_text(const char *body, const ns_node *li, const char *style_kw)
{
    const char *p = body;
    while (*p && g_ascii_isspace(*p)) p++;
    const char *name_s = p;
    while (*p && *p != ',' && *p != ')' && !g_ascii_isspace(*p)) p++;
    if ((gsize)(p - name_s) != 9 ||
        g_ascii_strncasecmp(name_s, "list-item", 9) != 0)
        return g_strdup("");
    while (*p && g_ascii_isspace(*p)) p++;
    char *style = NULL;
    if (*p == ',') {
        p++;
        while (*p && g_ascii_isspace(*p)) p++;
        const char *style_s = p;
        while (*p && *p != ')' && !g_ascii_isspace(*p)) p++;
        if (p != style_s) style = g_ascii_strdown(style_s, p - style_s);
    }
    char buf[32];
    format_ordered_label(style ? style : marker_default_kind(li, style_kw),
                         list_item_ordinal(li), buf, sizeof buf);
    g_free(style);
    return g_strdup(buf);
}

static char *
marker_resolve_content_text(const char *raw, const ns_node *li,
                            const char *style_kw)
{
    gboolean has_func = strchr(raw, '(') != NULL;
    gboolean has_string = strchr(raw, '"') || strchr(raw, '\'');
    if (!has_func && !has_string) return g_strdup(raw);
    GString *out = g_string_new(NULL);
    const char *p = raw;
    while (*p) {
        while (*p && g_ascii_isspace(*p)) p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            const char *start = p;
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                p++;
            }
            marker_append_escaped_string(out, start, p - start);
            if (*p == q) p++;
        } else if (g_str_has_prefix(p, "attr(")) {
            p += 5;
            while (*p && g_ascii_isspace(*p)) p++;
            const char *start = p;
            while (*p && *p != ')' && *p != ',' && !g_ascii_isspace(*p)) p++;
            if (li && p != start) {
                char *attr_name = g_strndup(start, p - start);
                const char *val = ns_element_get_attr(li, attr_name);
                if (val) g_string_append(out, val);
                g_free(attr_name);
            }
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
        } else if (g_str_has_prefix(p, "counter(")) {
            p += 8;
            const char *body_s = p;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') { depth--; if (depth == 0) break; }
                p++;
            }
            char *body = g_strndup(body_s, p - body_s);
            if (*p == ')') p++;
            char *sub = marker_counter_text(body, li, style_kw);
            if (sub) g_string_append(out, sub);
            g_free(sub);
            g_free(body);
        } else {
            const char *start = p;
            while (*p && !g_ascii_isspace(*p) && *p != '"' && *p != '\'') p++;
            g_string_append_len(out, start, p - start);
        }
    }
    return g_string_free(out, FALSE);
}

static char *
marker_custom_text(const ns_node *li, const ns_style *li_style,
                   const char *style_kw, gboolean *suppress_default)
{
    if (suppress_default) *suppress_default = FALSE;
    const ns_style *ms = li_style ? li_style->marker : NULL;
    if (!ms) return NULL;
    if (ns_display_is_none(ns_css_display_of(ms))) {
        if (suppress_default) *suppress_default = TRUE;
        return NULL;
    }
    const ns_css_value *cv = ms->values[NS_CSS_CONTENT];
    if (!cv || cv->kind != NS_CSS_V_KEYWORD || !cv->u.keyword) return NULL;
    const char *raw = cv->u.keyword;
    if (strcmp(raw, "normal") == 0) return NULL;
    if (strcmp(raw, "none") == 0) {
        if (suppress_default) *suppress_default = TRUE;
        return NULL;
    }
    if (strcmp(raw, "open-quote") == 0) return g_strdup("\xe2\x80\x9c");
    if (strcmp(raw, "close-quote") == 0) return g_strdup("\xe2\x80\x9d");
    if (strcmp(raw, "no-open-quote") == 0 ||
        strcmp(raw, "no-close-quote") == 0) {
        if (suppress_default) *suppress_default = TRUE;
        return NULL;
    }
    return marker_resolve_content_text(raw, li, style_kw);
}

static void
paint_marker_label(cairo_t *cr, const char *label, double edge_x,
                   gboolean rtl, double baseline_y, double font_size)
{
    if (!label || !*label) return;
    cairo_set_font_size(cr, font_size);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, label, &ext);
    double x = rtl ? edge_x + font_size * 0.35 - ext.x_bearing
                   : edge_x - font_size * 0.35 - ext.width - ext.x_bearing;
    cairo_move_to(cr, x, baseline_y);
    cairo_show_text(cr, label);
}

static void
greek_label(int n, char *out, gsize out_sz)
{
    static const gunichar greek[24] = {
        0x3B1, 0x3B2, 0x3B3, 0x3B4, 0x3B5, 0x3B6, 0x3B7, 0x3B8,
        0x3B9, 0x3BA, 0x3BB, 0x3BC, 0x3BD, 0x3BE, 0x3BF, 0x3C0,
        0x3C1, 0x3C3, 0x3C4, 0x3C5, 0x3C6, 0x3C7, 0x3C8, 0x3C9,
    };
    if (n < 1) { g_snprintf(out, out_sz, "%d", n); return; }
    gunichar buf[16];
    int len = 0, v = n;
    while (v > 0 && len < 16) { v--; buf[len++] = greek[v % 24]; v /= 24; }
    GString *s = g_string_new(NULL);
    for (int i = len - 1; i >= 0; i--)
        g_string_append_unichar(s, buf[i]);
    g_strlcpy(out, s->str, out_sz);
    g_string_free(s, TRUE);
}

static void
format_ordered_label(const char *kind, int n, char *out, gsize out_sz)
{
    if (kind) {
        if (strcmp(kind, "lower-greek") == 0) {
            greek_label(n, out, out_sz); return;
        }
        if (strcmp(kind, "upper-alpha") == 0 || strcmp(kind, "upper-latin") == 0) {
            alpha_label(n, TRUE, out, out_sz); return;
        }
        if (strcmp(kind, "lower-alpha") == 0 || strcmp(kind, "lower-latin") == 0) {
            alpha_label(n, FALSE, out, out_sz); return;
        }
        if (strcmp(kind, "upper-roman") == 0) {
            roman_numeral(n, TRUE, out, out_sz); return;
        }
        if (strcmp(kind, "lower-roman") == 0) {
            roman_numeral(n, FALSE, out, out_sz); return;
        }
        if (strcmp(kind, "decimal-leading-zero") == 0) {
            g_snprintf(out, out_sz, "%02d", n); return;
        }
    }
    g_snprintf(out, out_sz, "%d", n);
}

static gboolean
li_generates_marker(const ns_node *li, const ns_style *li_style)
{
    if (!li || li->kind != NS_NODE_ELEMENT || !li->name) return FALSE;
    if (!li->parent || !li->parent->name) return FALSE;
    const ns_css_value *d = li_style ? li_style->values[NS_CSS_DISPLAY] : NULL;
    if (!d || d->kind != NS_CSS_V_KEYWORD || !d->u.keyword)
        return strcmp(li->name, "li") == 0;
    return strstr(d->u.keyword, "list-item") != NULL;
}

static const char *
disclosure_glyph(const char *style_kw, gboolean rtl)
{
    if (!style_kw || strncmp(style_kw, "disclosure-", 11) != 0) return NULL;
    if (strcmp(style_kw + 11, "open") == 0) return "\xe2\x96\xbe";
    return rtl ? "\xe2\x97\x82" : "\xe2\x96\xb8";
}

gboolean
ns_paint_li_is_inside(const ns_style *li_style)
{
    if (!li_style) return FALSE;
    const ns_css_value *v = li_style->values[NS_CSS_LIST_STYLE_POSITION];
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           strcmp(v->u.keyword, "inside") == 0;
}

gboolean
ns_paint_li_marker_text(const ns_node *li, const ns_style *li_style,
                        char *out, gsize out_sz)
{
    if (!li_generates_marker(li, li_style)) return FALSE;
    if (out_sz < 8) return FALSE;
    const ns_css_value *lst = li_style
        ? li_style->values[NS_CSS_LIST_STYLE_TYPE] : NULL;
    const char *style_kw = NULL;
    if (lst && lst->kind == NS_CSS_V_KEYWORD) style_kw = lst->u.keyword;
    gboolean suppress_default = FALSE;
    char *custom = marker_custom_text(li, li_style, style_kw,
                                      &suppress_default);
    if (custom || suppress_default) {
        g_strlcpy(out, custom ? custom : "", out_sz);
        g_free(custom);
        return TRUE;
    }
    if (style_kw && strcmp(style_kw, "none") == 0) {
        out[0] = '\0';
        return TRUE;
    }
    if (marker_is_ordered(li, style_kw)) {
        int n = list_item_ordinal(li);
        const char *kind = marker_default_kind(li, style_kw);
        char buf[32];
        format_ordered_label(kind, n, buf, sizeof buf);
        g_snprintf(out, out_sz, "%s. ", buf);
        return TRUE;
    }
    const char *glyph = disclosure_glyph(style_kw,
        li_style && keyword_is(li_style->values[NS_CSS_DIRECTION], "rtl"));
    if (!glyph && style_kw && strcmp(style_kw, "square") == 0)
        glyph = "\xe2\x96\xaa";
    else if (!glyph && style_kw && strcmp(style_kw, "circle") == 0)
        glyph = "\xe2\x97\x8b";
    else if (!glyph)
        glyph = "\xe2\x80\xa2";
    g_snprintf(out, out_sz, "%s ", glyph);
    return TRUE;
}

static gboolean
marker_first_baseline(const ns_box *b, int depth, double *out)
{
    if (!b || depth > 64) return FALSE;
    if (b->kind == NS_BOX_INLINE) {
        if (b->first_baseline <= 0) return FALSE;
        *out = b->y + b->margin.top + b->border.top + b->padding.top +
               b->first_baseline;
        return TRUE;
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
        const ns_css_value *pos = c->style ? c->style->values[NS_CSS_POSITION]
                                           : NULL;
        if (pos && pos->kind == NS_CSS_V_KEYWORD && pos->u.keyword &&
            (strcmp(pos->u.keyword, "absolute") == 0 ||
             strcmp(pos->u.keyword, "fixed") == 0))
            continue;
        if (marker_first_baseline(c, depth + 1, out)) return TRUE;
    }
    return FALSE;
}

static void
paint_marker(cairo_t *cr, const ns_box *b)
{
    const ns_style *s = b->style;
    if (!li_generates_marker(b->dom, s)) return;
    if (ns_paint_li_is_inside(s)) return;
    const ns_css_value *lst = s ? s->values[NS_CSS_LIST_STYLE_TYPE] : NULL;
    const char *style_kw = NULL;
    if (lst && lst->kind == NS_CSS_V_KEYWORD) style_kw = lst->u.keyword;

    gboolean suppress_default = FALSE;
    char *custom = marker_custom_text(b->dom, s, style_kw,
                                      &suppress_default);
    if (suppress_default) {
        g_free(custom);
        return;
    }
    ns_image *marker_img = NULL;
    if (!custom && b->media && b->media->marker_image &&
        s && s->values[NS_CSS_LIST_STYLE_IMAGE] &&
        s->values[NS_CSS_LIST_STYLE_IMAGE]->kind == NS_CSS_V_URL) {
        ns_image *mi = b->media->marker_image;
        if (mi->loaded && mi->texture &&
            ns_texture_get_width(mi->texture) > 0 &&
            ns_texture_get_height(mi->texture) > 0)
            marker_img = mi;
    }
    if (style_kw && strcmp(style_kw, "none") == 0 && !custom && !marker_img)
        return;

    const ns_style *ms = s ? s->marker : NULL;

    double font_size = length_or(s ? s->values[NS_CSS_FONT_SIZE] : NULL, 16);
    if (ms && ms->values[NS_CSS_FONT_SIZE])
        font_size = length_or(ms->values[NS_CSS_FONT_SIZE], font_size);
    double cy = b->y + b->margin.top + b->padding.top + font_size * 0.7;
    double baseline;
    if (marker_first_baseline(b, 0, &baseline) &&
        baseline < cy + font_size * 2)
        cy = baseline;
    gboolean rtl = strcmp(ns_css_node_dir(b->dom), "rtl") == 0;
    double content_x = b->x + b->margin.left + b->padding.left;
    double edge_x = rtl ? content_x + b->content_width : content_x;
    double cx = rtl ? edge_x + font_size * 0.8 : edge_x - font_size * 0.8;

    if (marker_img) {
        int iw = ns_texture_get_width(marker_img->texture);
        int ih = ns_texture_get_height(marker_img->texture);
        double dw = iw, dh = ih;
        double cap = font_size;
        if (dh > cap) { dw *= cap / dh; dh = cap; }
        cairo_surface_t *surf = texture_surface_cached(marker_img->texture,
                                                       NULL);
        if (surf) {
            double dx = rtl ? edge_x + font_size * 0.35
                            : edge_x - font_size * 0.35 - dw;
            double dy = cy - dh + font_size * 0.15;
            cairo_save(cr);
            cairo_translate(cr, dx, dy);
            cairo_scale(cr, dw / iw, dh / ih);
            cairo_set_source_surface(cr, surf, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            return;
        }
    }
    const ns_css_value *cval = (ms && ms->values[NS_CSS_COLOR])
        ? ms->values[NS_CSS_COLOR]
        : (s ? s->values[NS_CSS_COLOR] : NULL);
    rgba color = rgba_of(cval, 0.1, 0.1, 0.1, 1);
    set_source_rgba(cr, color);

    if (custom) {
        paint_marker_label(cr, custom, edge_x, rtl, cy, font_size);
        g_free(custom);
    } else if (marker_is_ordered(b->dom, style_kw)) {
        int n = list_item_ordinal(b->dom);
        const char *kind = marker_default_kind(b->dom, style_kw);
        char buf[32];
        format_ordered_label(kind, n, buf, sizeof buf);
        char with_dot[40];
        g_snprintf(with_dot, sizeof with_dot, "%s.", buf);
        paint_marker_label(cr, with_dot, edge_x, rtl, cy, font_size);
    } else if (disclosure_glyph(style_kw, rtl)) {
        gboolean open = strcmp(style_kw, "disclosure-open") == 0;
        double sz = font_size * 0.3;
        double ty = cy - font_size * 0.32;
        double dir = rtl ? -1 : 1;
        cairo_new_path(cr);
        if (open) {
            cairo_move_to(cr, cx - sz, ty - sz * 0.5);
            cairo_line_to(cr, cx + sz, ty - sz * 0.5);
            cairo_line_to(cr, cx, ty + sz * 0.7);
        } else {
            cairo_move_to(cr, cx - dir * sz * 0.5, ty - sz);
            cairo_line_to(cr, cx - dir * sz * 0.5, ty + sz);
            cairo_line_to(cr, cx + dir * sz * 0.7, ty);
        }
        cairo_close_path(cr);
        cairo_fill(cr);
    } else if (style_kw && strcmp(style_kw, "square") == 0) {
        double sz = font_size * 0.32;
        cairo_new_path(cr);
        cairo_rectangle(cr, cx - sz/2, cy - font_size * 0.32 - sz/2, sz, sz);
        cairo_fill(cr);
    } else if (style_kw && strcmp(style_kw, "circle") == 0) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy - font_size * 0.32, font_size * 0.18, 0, 2 * G_PI);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    } else {
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy - font_size * 0.32, font_size * 0.18, 0, 2 * G_PI);
        cairo_fill(cr);
    }
}

static gboolean
box_is_hidden(const ns_box *b)
{
    if (b && !b->style && b->kind == NS_BOX_INLINE) return FALSE;
    for (; b; b = b->parent) {
        const ns_style *s = b->style;
        if (!s) continue;
        const ns_css_value *v = s->values[NS_CSS_VISIBILITY];
        return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
               (strcmp(v->u.keyword, "hidden") == 0 ||
                strcmp(v->u.keyword, "collapse") == 0);
    }
    return FALSE;
}

static gboolean
box_has_visible_descendant(const ns_box *b)
{
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        if (!box_is_hidden(c) || box_has_visible_descendant(c)) return TRUE;
    for (guint i = 0; b->inline_atomics && i < b->inline_atomics->len; i++) {
        const ns_box *ab = g_array_index(b->inline_atomics,
                                         ns_inline_atomic, i).box;
        if (ab && (!box_is_hidden(ab) || box_has_visible_descendant(ab)))
            return TRUE;
    }
    return FALSE;
}

static gboolean
box_hides_subtree(const ns_box *b)
{
    return box_is_hidden(b) && !box_has_visible_descendant(b);
}

static gboolean
box_skips_contents(const ns_box *b)
{
    const ns_style *s = b ? b->style : NULL;
    const ns_css_value *v = s ? s->values[NS_CSS_CONTENT_VISIBILITY] : NULL;
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           strcmp(v->u.keyword, "hidden") == 0;
}

static double
box_opacity(const ns_box *b)
{
    if (b && g_paint_anim) {
        double anim_o;
        if (ns_anim_get_opacity(g_paint_anim, b->dom, &anim_o)) {
            if (anim_o < 0) anim_o = 0;
            if (anim_o > 1) anim_o = 1;
            return anim_o;
        }
    }
    const ns_style *s = b ? b->style : NULL;
    if (!s) return 1.0;
    const ns_css_value *v = s->values[NS_CSS_OPACITY];
    if (!v) return 1.0;
    if (v->kind == NS_CSS_V_LENGTH) {
        double o = v->u.length.v;
        if (o < 0) o = 0;
        if (o > 1) o = 1;
        return o;
    }
    return 1.0;
}

static gboolean
box_clip_hides(const ns_box *b)
{
    const ns_style *s = b ? b->style : NULL;
    if (!s) return FALSE;
    const ns_css_value *cv = s->values[NS_CSS_CLIP];
    if (!cv || cv->kind != NS_CSS_V_RECT) return FALSE;
    const ns_css_value *pv = s->values[NS_CSS_POSITION];
    if (!pv || pv->kind != NS_CSS_V_KEYWORD || !pv->u.keyword) return FALSE;
    if (strcmp(pv->u.keyword, "absolute") != 0 &&
        strcmp(pv->u.keyword, "fixed") != 0) return FALSE;
    double bw = b->content_width + b->padding.left + b->padding.right +
                b->border.left + b->border.right;
    double bh = b->content_height + b->padding.top + b->padding.bottom +
                b->border.top + b->border.bottom;
    double top    = cv->u.rect.is_auto[0] ? 0  : cv->u.rect.v[0];
    double right  = cv->u.rect.is_auto[1] ? bw : cv->u.rect.v[1];
    double bottom = cv->u.rect.is_auto[2] ? bh : cv->u.rect.v[2];
    double left   = cv->u.rect.is_auto[3] ? 0  : cv->u.rect.v[3];
    return (right - left) <= 0 || (bottom - top) <= 0;
}

static int
style_z_index(const ns_style *s)
{
    if (!s) return 0;
    const ns_css_value *v = s->values[NS_CSS_Z_INDEX];
    if (!v || v->kind != NS_CSS_V_LENGTH) return 0;
    return (int)v->u.length.v;
}

static int
box_z_index(const ns_box *b)
{
    return ns_box_layer_z(b);
}

typedef struct paint_entry {
    const ns_box *box;
    int key;
    guint order;
} paint_entry;

static int
paint_entry_cmp(const void *a, const void *b)
{
    const paint_entry *pa = a;
    const paint_entry *pb = b;
    if (pa->key != pb->key) return pa->key < pb->key ? -1 : 1;
    if (pa->order != pb->order) return pa->order < pb->order ? -1 : 1;
    return 0;
}

static int
dom_tree_order_cmp(const ns_node *a, const ns_node *b)
{
    if (!a || !b || a == b) return 0;
    const ns_node *pa[128], *pb[128];
    int na = 0, nb = 0;
    for (const ns_node *n = a; n && na < 128; n = n->parent) pa[na++] = n;
    for (const ns_node *n = b; n && nb < 128; n = n->parent) pb[nb++] = n;
    if (na >= 128 || nb >= 128) return 0;
    int ia = na - 1, ib = nb - 1;
    while (ia >= 0 && ib >= 0 && pa[ia] == pb[ib]) { ia--; ib--; }
    if (ia < 0) return -1;
    if (ib < 0) return 1;
    if (pa[ia]->parent != pb[ib]->parent) return 0;
    for (const ns_node *s = pa[ia]->next_sibling; s; s = s->next_sibling)
        if (s == pb[ib]) return -1;
    return 1;
}

typedef struct deferred_capture {
    const ns_box *box;
    double dev_x, dev_y;
    guint seq;
    NsPangoLayout *layout;
    guint layer;
    inline_layer_place at;
    gboolean no_cull;
    const ns_node *dom;
    int z;
    gboolean isolates;
    gboolean adopted;
} deferred_capture;

static const ns_box *g_paint_adopted_box;

static guint g_paint_capture_seq;

static void
deferred_capture_free(gpointer data)
{
    deferred_capture *cap = data;
    if (cap->layout) g_object_unref(cap->layout);
    g_free(cap);
}

static int
deferred_capture_z(const deferred_capture *cap)
{
    return cap->layout ? cap->z : box_z_index(cap->box);
}

static int
deferred_capture_tree_cmp(const deferred_capture *a, const deferred_capture *b)
{
    if (a->layout && b->layout && a->box == b->box)
        return a->layer < b->layer ? -1 : (a->layer > b->layer ? 1 : 0);
    return ns_layout_tree_order_cmp(a->box, a->layout ? a->dom : NULL,
                                    b->box, b->layout ? b->dom : NULL);
}

static int
deferred_capture_cmp(const void *va, const void *vb)
{
    const deferred_capture *a = *(deferred_capture *const *)va;
    const deferred_capture *b = *(deferred_capture *const *)vb;
    const ns_box *ab = a->box, *bb = b->box;
    if (!ab || !bb) return ab ? 1 : bb ? -1 : 0;
    int za = deferred_capture_z(a), zb = deferred_capture_z(b);
    if (za != zb) return za < zb ? -1 : 1;
    int c = deferred_capture_tree_cmp(a, b);
    if (c) return c;
    return a->seq < b->seq ? -1 : a->seq > b->seq ? 1 : 0;
}

static void
paint_defer_inline_layer(cairo_t *cr, const ns_box *b, NsPangoLayout *layout,
                         guint layer, const inline_layer_place *at,
                         const ns_inline_attr *owner)
{
    if (!g_paint_deferred_list)
        g_paint_deferred_list =
            g_ptr_array_new_with_free_func(deferred_capture_free);
    deferred_capture *cap = g_new0(deferred_capture, 1);
    cap->box = b;
    cap->seq = g_paint_capture_seq++;
    cap->layout = g_object_ref(layout);
    cap->layer = layer;
    cap->at = *at;
    cap->dom = owner->dom;
    cap->z = style_z_index(owner->style);
    cap->isolates = ns_style_is_stacking_context(owner->style);
    cairo_user_to_device(cr, &cap->dev_x, &cap->dev_y);
    g_ptr_array_add(g_paint_deferred_list, cap);
}

static void
paint_inline_defer_layers(cairo_t *cr, const ns_box *b, NsPangoLayout *layout,
                          const inline_layer_place *at, const char *highlight)
{
    const inline_layers *L = inline_layers_of(b, layout);
    for (guint k = 1; L && k < L->owners->len; k++) {
        const ns_inline_attr *owner = inline_layer_owner(b, L, k);
        if (style_z_index(owner->style) < 0)
            continue;
        if (g_paint_defer_depth > 0)
            paint_defer_inline_layer(cr, b, layout, k, at, owner);
        else
            paint_inline_layer(cr, b, layout, k, at, highlight);
    }
}

static void
paint_inline_negative_layers(cairo_t *cr, const ns_box *b,
                             NsPangoLayout *layout,
                             const inline_layer_place *at,
                             const char *highlight)
{
    const inline_layers *L = inline_layers_of(b, layout);
    for (guint k = 1; L && k < L->owners->len; k++)
        if (style_z_index(inline_layer_owner(b, L, k)->style) < 0)
            paint_inline_layer(cr, b, layout, k, at, highlight);
}

static void paint_flush_deferred(cairo_t *cr, GPtrArray *list,
                                 const char *highlight);

static void
paint_deferred_inline_layer(cairo_t *cr, const deferred_capture *cap,
                            const char *highlight)
{
    if (!cap->isolates) {
        paint_inline_layer(cr, cap->box, cap->layout, cap->layer, &cap->at,
                           highlight);
        return;
    }
    GPtrArray *saved_list = g_paint_deferred_list;
    g_paint_deferred_list = NULL;
    g_paint_defer_depth++;
    paint_inline_layer(cr, cap->box, cap->layout, cap->layer, &cap->at,
                       highlight);
    GPtrArray *mine = g_paint_deferred_list;
    g_paint_deferred_list = saved_list;
    g_paint_defer_depth--;
    if (!mine) return;
    paint_flush_deferred(cr, mine, highlight);
    g_ptr_array_free(mine, TRUE);
}

static void
paint_deferred_capture(cairo_t *cr, const deferred_capture *cap,
                       const char *highlight)
{
    if (cap->layout) {
        paint_deferred_inline_layer(cr, cap, highlight);
        return;
    }
    const ns_box *saved = g_paint_adopted_box;
    g_paint_adopted_box = cap->adopted ? cap->box : NULL;
    paint_walk(cr, cap->box, highlight);
    g_paint_adopted_box = saved;
}

static gboolean
deferred_capture_isolates(const deferred_capture *cap)
{
    return cap->layout ? cap->isolates
                       : ns_box_is_stacking_context(cap->box);
}

static void
layers_record(cairo_t *cr, const ns_box *box, int kind, double dx, double dy)
{
    ns_paint_vp_capture layer = { .box = box, .kind = kind };
    cairo_matrix_t m, inv = g_layers.base;
    cairo_get_matrix(cr, &m);
    cairo_matrix_translate(&m, dx, dy);
    if (cairo_matrix_invert(&inv) != CAIRO_STATUS_SUCCESS)
        cairo_matrix_init_identity(&inv);
    cairo_matrix_multiply(&layer.rel, &m, &inv);
    g_array_append_val(g_layers.found, layer);
}

static cairo_t *
layers_target(cairo_t *cr, const deferred_capture *cap, double dx, double dy)
{
    int kind = GPOINTER_TO_INT(g_hash_table_lookup(g_layers.kinds, cap->box));
    if (kind) {
        if (g_layers.mode == PAINT_LAYERS_PLAN)
            layers_record(cr, cap->box, kind, dx, dy);
        g_layers.vp_seen++;
        return NULL;
    }
    if (g_layers.mode == PAINT_LAYERS_PLAN) return cr;
    if (g_layers.vp_seen == 0 || g_layers.n_upper == 0) return g_layers.doc;
    int i = MIN(g_layers.vp_seen, g_layers.n_upper) - 1;
    cairo_t *upper = g_layers.upper(i, g_layers.upper_data);
    return upper ? upper : g_layers.doc;
}

static void
paint_flush_deferred(cairo_t *cr, GPtrArray *list, const char *highlight)
{
    gboolean layered = g_layers.mode != PAINT_LAYERS_OFF &&
                       g_layers.flush_layered;
    g_layers.flush_layered = FALSE;
    if (!list || list->len == 0) return;
    GPtrArray *queue = g_ptr_array_sized_new(list->len);
    GPtrArray *adopted =
        g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
    for (guint i = 0; i < list->len; i++)
        g_ptr_array_add(queue, g_ptr_array_index(list, i));
    qsort(queue->pdata, queue->len, sizeof(gpointer), deferred_capture_cmp);
    const ns_box *saved_flush = g_paint_flush_box;
    for (guint i = 0; i < queue->len; i++) {
        const deferred_capture *cap = g_ptr_array_index(queue, i);
        double cur_x = 0, cur_y = 0;
        cairo_user_to_device(cr, &cur_x, &cur_y);
        double dx = cap->dev_x - cur_x;
        double dy = cap->dev_y - cur_y;
        if (isnan(dx) || isnan(dy)) dx = dy = 0;
        cairo_t *target = layered ? layers_target(cr, cap, dx, dy) : cr;
        if (!target) continue;
        if (layered) g_layers.owner = cap->box;
        cairo_save(target);
        if (target != cr) {
            cairo_matrix_t m;
            cairo_get_matrix(cr, &m);
            cairo_set_matrix(target, &m);
        }
        if (dx != 0 || dy != 0) cairo_translate(target, dx, dy);
        g_paint_flush_box = cap->box;
        if (g_dbg_paint_x >= 0 && cap->box->dom) {
            double gx0, gy0, gx1, gy1;
            cairo_clip_extents(cr, &gx0, &gy0, &gx1, &gy1);
            g_printerr("[flush-one] <%s#%s y=%.0f h=%.0f> d=%.0f,%.0f "
                       "clip=%.0f,%.0f..%.0f,%.0f\n",
                       cap->box->dom->name ? cap->box->dom->name : "?",
                       ns_element_get_attr(cap->box->dom, "id")
                           ? ns_element_get_attr(cap->box->dom, "id")
                           : "",
                       cap->box->y, cap->box->content_height,
                       dx, dy, gx0, gy0, gx1, gy1);
        }
        gboolean flat = !deferred_capture_isolates(cap);
        GPtrArray *saved_list = g_paint_deferred_list;
        if (flat) {
            g_paint_deferred_list = NULL;
            g_paint_defer_depth++;
        }
        if (cap->no_cull) g_paint_no_cull++;
        paint_deferred_capture(target, cap, highlight);
        if (cap->no_cull) g_paint_no_cull--;
        if (flat) {
            GPtrArray *found = g_paint_deferred_list;
            g_paint_deferred_list = saved_list;
            g_paint_defer_depth--;
            if (found) {
                for (guint k = 0; k < found->len; k++)
                    g_ptr_array_add(queue, g_ptr_array_index(found, k));
                qsort(queue->pdata + i + 1, queue->len - i - 1,
                      sizeof(gpointer), deferred_capture_cmp);
                g_ptr_array_add(adopted, found);
            }
        }
        cairo_restore(target);
    }
    g_paint_flush_box = saved_flush;
    g_ptr_array_free(queue, TRUE);
    g_ptr_array_free(adopted, TRUE);
}

static double g_paint_anchor_dx, g_paint_anchor_dy;
static const ns_box *g_paint_skip_box;

static gboolean
paint_box_skipped(const ns_box *b)
{
    return b == g_paint_skip_box ||
           (b->negative_lifted && b != g_paint_flush_box);
}

static void
paint_negative_layers(cairo_t *cr, const ns_box *b, const char *highlight)
{
    const GPtrArray *layers = b->negative_layers;
    if (!layers) return;
    const ns_box *saved_flush = g_paint_flush_box;
    for (guint i = 0; i < layers->len; i++) {
        g_paint_flush_box = g_ptr_array_index(layers, i);
        paint_walk(cr, g_paint_flush_box, highlight);
    }
    g_paint_flush_box = saved_flush;
}

static void
paint_anchor_leave(cairo_t *cr, double saved_dx, double saved_dy)
{
    cairo_restore(cr);
    g_paint_anchor_dx = saved_dx;
    g_paint_anchor_dy = saved_dy;
}

static void
compute_sticky_offset(const ns_box *b, cairo_t *cr,
                      double *out_dx, double *out_dy)
{
    *out_dx = 0;
    *out_dy = 0;
    if (!b || !b->style || b == g_paint_sticky_static_box) return;
    if (ns_box_is_fixed(b)) {
        if (g_paint_have_viewport) {
            *out_dx = g_paint_vp_x0;
            *out_dy = g_paint_vp_y0;
        }
        return;
    }
    if (!keyword_is(b->style->values[NS_CSS_POSITION], "sticky")) return;
    double clip_x1, clip_y1, clip_x2, clip_y2;
    cairo_clip_extents(cr, &clip_x1, &clip_y1, &clip_x2, &clip_y2);
    ns_box_sticky_offset(b, clip_x1, clip_y1, clip_x2, clip_y2,
                         out_dx, out_dy);
}

static cairo_operator_t
blend_mode_operator(const ns_style *s)
{
    if (!s || !s->values[NS_CSS_MIX_BLEND_MODE]) return CAIRO_OPERATOR_OVER;
    const ns_css_value *v = s->values[NS_CSS_MIX_BLEND_MODE];
    if (v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return CAIRO_OPERATOR_OVER;
    const char *k = v->u.keyword;
    if (strcmp(k, "multiply")    == 0) return CAIRO_OPERATOR_MULTIPLY;
    if (strcmp(k, "screen")      == 0) return CAIRO_OPERATOR_SCREEN;
    if (strcmp(k, "overlay")     == 0) return CAIRO_OPERATOR_OVERLAY;
    if (strcmp(k, "darken")      == 0) return CAIRO_OPERATOR_DARKEN;
    if (strcmp(k, "lighten")     == 0) return CAIRO_OPERATOR_LIGHTEN;
    if (strcmp(k, "color-dodge") == 0) return CAIRO_OPERATOR_COLOR_DODGE;
    if (strcmp(k, "color-burn")  == 0) return CAIRO_OPERATOR_COLOR_BURN;
    if (strcmp(k, "hard-light")  == 0) return CAIRO_OPERATOR_HARD_LIGHT;
    if (strcmp(k, "soft-light")  == 0) return CAIRO_OPERATOR_SOFT_LIGHT;
    if (strcmp(k, "difference")  == 0) return CAIRO_OPERATOR_DIFFERENCE;
    if (strcmp(k, "exclusion")   == 0) return CAIRO_OPERATOR_EXCLUSION;
    if (strcmp(k, "hue")         == 0) return CAIRO_OPERATOR_HSL_HUE;
    if (strcmp(k, "saturation")  == 0) return CAIRO_OPERATOR_HSL_SATURATION;
    if (strcmp(k, "color")       == 0) return CAIRO_OPERATOR_HSL_COLOR;
    if (strcmp(k, "luminosity")  == 0) return CAIRO_OPERATOR_HSL_LUMINOSITY;
    return CAIRO_OPERATOR_OVER;
}

static ns_paint_stats g_paint_stats;
static gboolean g_paint_collect_stats;
static gboolean g_paint_have_clip;
static double g_paint_clip_y0, g_paint_clip_y1;
static double g_paint_cull_margin = 400.0;

static cairo_pattern_t *
mask_gradient_pattern(const ns_css_gradient *gr,
                      double bx, double by, double bw, double bh)
{
    if (!gr || gr->conic || gr->n_stops < 1) return NULL;
    double cx = bx + gr->center_x * bw + gr->center_x_px;
    double cy = by + gr->center_y * bh + gr->center_y_px;
    double dxh = 0, dyh = 0, r_outer = 1, r_outer_y = 1, line_len;
    if (gr->radial) {
        ns_css_gradient_radii(gr, bw, bh, cx - bx, cy - by, &r_outer, &r_outer_y);
        line_len = r_outer;
    } else {
        double rad = ns_css_gradient_angle(gr, bw, bh) * G_PI / 180.0;
        double dx = sin(rad), dy = -cos(rad);
        double half = (fabs(dx) * bw + fabs(dy) * bh) / 2.0;
        dxh = dx * half; dyh = dy * half;
        line_len = 2.0 * half;
    }
    if (line_len <= 0) line_len = 1;
    double frac[NS_CSS_GRADIENT_STOPS_MAX];
    gradient_stop_fractions(gr, line_len, frac);
    double period = (gr->repeating && gr->n_stops > 0) ? frac[gr->n_stops - 1] : 1.0;
    if (period <= 0) period = 1.0;
    cairo_pattern_t *pat;
    if (gr->radial) {
        pat = cairo_pattern_create_radial(0, 0, 0, 0, 0, period);
        cairo_matrix_t m;
        cairo_matrix_init_scale(&m, 1.0 / r_outer, 1.0 / r_outer_y);
        cairo_matrix_translate(&m, -cx, -cy);
        cairo_pattern_set_matrix(pat, &m);
    } else {
        double x0 = cx - dxh, y0 = cy - dyh;
        pat = cairo_pattern_create_linear(x0, y0,
            x0 + 2.0 * dxh * period, y0 + 2.0 * dyh * period);
    }
    gradient_add_stops(pat, gr, frac, period);
    if (gr->repeating) cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
    return pat;
}


static gboolean
mask_layer_is_gradient(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_GRADIENT && !v->u.gradient.conic;
}

static gboolean
mask_layer_is_none(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           strcmp(v->u.keyword, "none") == 0;
}

static gboolean
mask_layers_paintable(const ns_style *s)
{
    const ns_css_value *mask = s ? s->values[NS_CSS_MASK_IMAGE] : NULL;
    gboolean any = FALSE;
    for (const ns_css_value *l = mask; l; l = l->next_layer) {
        if (mask_layer_is_gradient(l) || (l->kind == NS_CSS_V_URL && l->u.url))
            any = TRUE;
        else if (!mask_layer_is_none(l)) return FALSE;
    }
    return any;
}

static ns_image *
mask_layer_image(const ns_box *b, const ns_css_value *layer)
{
    if (!b->media || !b->media->mask_image || !b->media->mask_image_src)
        return NULL;
    return strcmp(b->media->mask_image_src, layer->u.url) == 0
        ? b->media->mask_image : NULL;
}

static cairo_operator_t
mask_composite_operator(const ns_css_value *v)
{
    const char *op = v && v->kind == NS_CSS_V_KEYWORD ? v->u.keyword : NULL;
    if (!op) return CAIRO_OPERATOR_OVER;
    if (strcmp(op, "subtract") == 0) return CAIRO_OPERATOR_OUT;
    if (strcmp(op, "intersect") == 0) return CAIRO_OPERATOR_IN;
    if (strcmp(op, "exclude") == 0) return CAIRO_OPERATOR_XOR;
    return CAIRO_OPERATOR_OVER;
}

static void
mask_clip_box_path(cairo_t *cr, const ns_box *b, const ns_css_value *clip)
{
    const char *kw = clip && clip->kind == NS_CSS_V_KEYWORD
        ? clip->u.keyword : NULL;
    double x = b->x + b->margin.left, y = b->y + b->margin.top;
    double w = b->content_width + b->padding.left + b->padding.right +
               b->border.left + b->border.right;
    double h = b->content_height + b->padding.top + b->padding.bottom +
               b->border.top + b->border.bottom;
    corner_radii radii = box_border_radii(b);
    double t = 0, r = 0, bo = 0, l = 0;
    if (kw && (strcmp(kw, "padding-box") == 0 ||
               strcmp(kw, "content-box") == 0)) {
        t = b->border.top; r = b->border.right;
        bo = b->border.bottom; l = b->border.left;
    }
    if (kw && strcmp(kw, "content-box") == 0) {
        t += b->padding.top; r += b->padding.right;
        bo += b->padding.bottom; l += b->padding.left;
    }
    if (kw && strcmp(kw, "no-clip") == 0) {
        cairo_rectangle(cr, x - 1e5, y - 1e5, w + 2e5, h + 2e5);
        return;
    }
    rounded_rect_path(cr, x + l, y + t, MAX(0.0, w - l - r),
                      MAX(0.0, h - t - bo),
                      corner_radii_inset(radii, t, r, bo, l));
}

static cairo_pattern_t *
mask_layers_pattern(cairo_t *cr, const ns_box *b, const cairo_matrix_t *tf,
                    double alpha)
{
    const ns_style *s = b->style;
    const ns_css_value *mask = s->values[NS_CSS_MASK_IMAGE];
    int n = 0;
    for (const ns_css_value *l = mask; l; l = l->next_layer) n++;
    double bx = b->x + b->margin.left, by = b->y + b->margin.top;
    double bw = b->content_width + b->padding.left + b->padding.right +
                b->border.left + b->border.right;
    double bh = b->content_height + b->padding.top + b->padding.bottom +
                b->border.top + b->border.bottom;
    cairo_push_group_with_content(cr, CAIRO_CONTENT_ALPHA);
    if (tf) {
        cairo_save(cr);
        cairo_transform(cr, tf);
    }
    for (int i = n - 1; i >= 0; i--) {
        const ns_css_value *layer = ns_css_value_layer(mask, i);
        cairo_pattern_t *grad = mask_layer_is_gradient(layer)
            ? mask_gradient_pattern(&layer->u.gradient, bx, by, bw, bh) : NULL;
        cairo_save(cr);
        cairo_new_path(cr);
        mask_clip_box_path(cr, b, ns_css_value_layer(
            s->values[NS_CSS_MASK_CLIP], i));
        cairo_clip(cr);
        cairo_set_operator(cr, i == n - 1 ? CAIRO_OPERATOR_OVER
            : mask_composite_operator(ns_css_value_layer(
                  s->values[NS_CSS_MASK_COMPOSITE], i)));
        if (layer->kind == NS_CSS_V_URL) {
            ns_image *img = mask_layer_image(b, layer);
            corner_radii square = { 0 };
            if (img)
                paint_bg_image_core(cr, img,
                    ns_css_value_layer(s->values[NS_CSS_MASK_REPEAT], i),
                    ns_css_value_layer(s->values[NS_CSS_MASK_SIZE], i),
                    ns_css_value_layer(s->values[NS_CSS_MASK_POSITION_X], i),
                    ns_css_value_layer(s->values[NS_CSS_MASK_POSITION_Y], i),
                    FALSE, bx, by, bw, bh, bx, by, bw, bh, square);
        } else {
            if (grad) cairo_set_source(cr, grad);
            else cairo_set_source_rgba(cr, 0, 0, 0, 0);
            cairo_paint(cr);
        }
        cairo_restore(cr);
        if (grad) cairo_pattern_destroy(grad);
    }
    if (tf) cairo_restore(cr);
    if (alpha < 0.999) {
        cairo_set_operator(cr, CAIRO_OPERATOR_DEST_IN);
        cairo_set_source_rgba(cr, 0, 0, 0, 1);
        cairo_paint_with_alpha(cr, alpha);
    }
    return cairo_pop_group(cr);
}

static void
paint_cache_clip(cairo_t *cr)
{
    double x0, x1;
    cairo_clip_extents(cr, &x0, &g_paint_clip_y0, &x1, &g_paint_clip_y1);
    g_paint_have_clip = TRUE;
    g_paint_vp_x0 = x0;
    g_paint_vp_y0 = g_paint_clip_y0;
    g_paint_have_viewport = isfinite(x0) && isfinite(g_paint_clip_y0) &&
                            (x0 != 0 || g_paint_clip_y0 != 0);
    g_paint_anchor_dx = 0;
    g_paint_anchor_dy = 0;
}

static const ns_box *g_paint_tex_root;

typedef struct ns_quad3 {
    const ns_box *box;
    ns_mat4 m;
    double bx, by, bw, bh;
    double depth;
    guint seq;
    gboolean own_only;
} ns_quad3;

typedef struct quad_tex_entry {
    cairo_surface_t *surf;
    int tw, th;
} quad_tex_entry;

static GHashTable *g_paint_3d_tex;

static void
quad_tex_entry_free(gpointer p)
{
    quad_tex_entry *e = p;
    cairo_surface_destroy(e->surf);
    g_free(e);
}

static void
box_border_rect(const ns_box *b, double *bx, double *by, double *bw, double *bh)
{
    *bx = b->x + b->margin.left;
    *by = b->y + b->margin.top;
    *bw = b->content_width + b->padding.left + b->padding.right +
          b->border.left + b->border.right;
    *bh = b->content_height + b->padding.top + b->padding.bottom +
          b->border.top + b->border.bottom;
}

static double
filter_blur_sigma(const char *filter)
{
    double sigma = 0;
    const char *p = filter;
    while (p && *p) {
        const char *name = NULL;
        const char *body = NULL;
        gsize nlen = 0;
        const char *next = filter_function_next(p, &name, &nlen, &body, NULL);
        if (!next) break;
        if (filter_name_is(name, nlen, "blur")) {
            double amt = parse_filter_amount(body, NULL);
            if (amt > sigma) sigma = amt;
        }
        p = next;
    }
    return sigma > 512.0 ? 512.0 : sigma;
}

static const char *
box_filter_layer_keyword(const ns_box *b)
{
    if (!b->style || b == g_paint_tex_root) return NULL;
    if (b->kind != NS_BOX_BLOCK && b->kind != NS_BOX_TABLE &&
        b->kind != NS_BOX_TABLE_CAPTION && b->kind != NS_BOX_TABLE_CELL &&
        b->kind != NS_BOX_SVG)
        return NULL;
    const ns_css_value *v = b->style->values[NS_CSS_FILTER];
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return NULL;
    return filter_has_bitmap_effect(v->u.keyword) ? v->u.keyword : NULL;
}

static void
box_ink_extents_x(const ns_box *b, double *x0, double *x1, int depth)
{
    double bx, by, bw, bh;
    box_border_rect(b, &bx, &by, &bw, &bh);
    if (bw > 0) {
        *x0 = MIN(*x0, bx);
        *x1 = MAX(*x1, bx + bw);
    }
    if (depth >= 64) return;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        box_ink_extents_x(c, x0, x1, depth + 1);
}

#define NS_PAINT_GROUP_INK_MARGIN 64.0

static gboolean
paint_group_clip(cairo_t *cr, const ns_box *b)
{
    if (!(b->paint_bottom > b->paint_top) ||
        b->paint_top <= -G_MAXDOUBLE / 2 || b->paint_bottom >= G_MAXDOUBLE / 2)
        return FALSE;
    double bx, by, bw, bh;
    box_border_rect(b, &bx, &by, &bw, &bh);
    double x0 = bx, x1 = bx + bw;
    box_ink_extents_x(b, &x0, &x1, 0);
    double y0 = MIN(by, b->paint_top), y1 = MAX(by + bh, b->paint_bottom);
    if (!isfinite(x0) || !isfinite(x1)) return FALSE;
    cairo_save(cr);
    cairo_rectangle(cr, x0 - NS_PAINT_GROUP_INK_MARGIN,
                    y0 - NS_PAINT_GROUP_INK_MARGIN,
                    x1 - x0 + 2 * NS_PAINT_GROUP_INK_MARGIN,
                    y1 - y0 + 2 * NS_PAINT_GROUP_INK_MARGIN);
    cairo_clip(cr);
    return TRUE;
}

typedef enum {
    FILTER_LAYER_NONE,
    FILTER_LAYER_OPEN,
    FILTER_LAYER_INVISIBLE,
} filter_layer_state;

typedef struct {
    cairo_t         *outer;
    cairo_t         *cr;
    cairo_surface_t *surf;
    double           x, y, k;
    const char      *filter;
    rgba             color;
} filter_layer;

static filter_layer_state
filter_layer_begin(cairo_t *cr, const ns_box *b, const char *filter,
                   gboolean transformed, filter_layer *fl)
{
    double sigma = filter_blur_sigma(filter);
    rgba color = rgba_of(b->style ? b->style->values[NS_CSS_COLOR] : NULL,
                         0, 0, 0, 1);
    double spread = MAX(ceil(sigma * 3.0),
                        filter_drop_shadow_reach(filter, color));
    double cx0, cy0, cx1, cy1;
    cairo_clip_extents(cr, &cx0, &cy0, &cx1, &cy1);
    double bx, by, bw, bh;
    box_border_rect(b, &bx, &by, &bw, &bh);
    double x0 = bx, x1 = bx + bw;
    box_ink_extents_x(b, &x0, &x1, 0);
    double y0 = by, y1 = by + bh;
    if (b->paint_bottom > b->paint_top) {
        y0 = MIN(y0, b->paint_top);
        y1 = MAX(y1, b->paint_bottom);
    }
    if (transformed) {
        x0 = cx0 - spread;
        y0 = cy0 - spread;
        x1 = cx1 + spread;
        y1 = cy1 + spread;
    }
    x0 = MAX(x0, cx0 - spread) - spread;
    y0 = MAX(y0, cy0 - spread) - spread;
    x1 = MIN(x1, cx1 + spread) + spread;
    y1 = MIN(y1, cy1 + spread) + spread;
    if (x1 - x0 < 1 || y1 - y0 < 1 || x1 < cx0 || x0 > cx1 ||
        y1 < cy0 || y0 > cy1)
        return FILTER_LAYER_INVISIBLE;
    double ux = 1, uy = 0;
    cairo_user_to_device_distance(cr, &ux, &uy);
    double k = MAX(hypot(ux, uy), 0.05);
    if (sigma > 0) k = MIN(k, MAX(8.0 / sigma, 0.05));
    double maxdim = MAX(x1 - x0, y1 - y0);
    if (maxdim * k > 4096.0) k = 4096.0 / maxdim;
    int tw = (int)ceil((x1 - x0) * k);
    int th = (int)ceil((y1 - y0) * k);
    if (tw < 1 || th < 1) return FILTER_LAYER_INVISIBLE;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       tw, th);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return FILTER_LAYER_NONE;
    }
    cairo_t *tcr = cairo_create(surf);
    cairo_scale(tcr, k, k);
    cairo_translate(tcr, -x0, -y0);
    *fl = (filter_layer){
        .outer = cr, .cr = tcr, .surf = surf,
        .x = x0, .y = y0, .k = k, .filter = filter, .color = color,
    };
    return FILTER_LAYER_OPEN;
}

static void
filter_layer_discard(filter_layer *fl)
{
    cairo_destroy(fl->cr);
    cairo_surface_destroy(fl->surf);
    fl->cr = NULL;
    fl->surf = NULL;
}

static void
filter_layer_end(filter_layer *fl)
{
    cairo_destroy(fl->cr);
    fl->cr = NULL;
    cairo_surface_flush(fl->surf);
    apply_image_filter(cairo_image_surface_get_data(fl->surf),
                       cairo_image_surface_get_stride(fl->surf),
                       cairo_image_surface_get_width(fl->surf),
                       cairo_image_surface_get_height(fl->surf),
                       fl->filter, fl->k, &fl->color);
    cairo_surface_mark_dirty(fl->surf);
    cairo_t *cr = fl->outer;
    cairo_save(cr);
    cairo_translate(cr, fl->x, fl->y);
    cairo_scale(cr, 1.0 / fl->k, 1.0 / fl->k);
    cairo_set_source_surface(cr, fl->surf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(fl->surf);
    fl->surf = NULL;
}

static gboolean
style_keyword_set(const ns_style *s, ns_css_prop prop, const char *neutral)
{
    const ns_css_value *v = s->values[prop];
    if (!v) return FALSE;
    if (v->kind != NS_CSS_V_KEYWORD) return TRUE;
    return v->u.keyword && g_ascii_strcasecmp(v->u.keyword, neutral) != 0;
}

static gboolean
box_flattens_3d(const ns_box *b)
{
    const ns_style *s = b->style;
    if (box_opacity(b) < 1.0) return TRUE;
    const char *ovs = ns_style_keyword(s, NS_CSS_OVERFLOW);
    const char *ovx = ns_style_keyword(s, NS_CSS_OVERFLOW_X);
    const char *ovy = ns_style_keyword(s, NS_CSS_OVERFLOW_Y);
    if (!ovx) ovx = ovs;
    if (!ovy) ovy = ovs;
    if ((overflow_kw_clips(ovx) && g_ascii_strcasecmp(ovx, "clip") != 0) ||
        (overflow_kw_clips(ovy) && g_ascii_strcasecmp(ovy, "clip") != 0))
        return TRUE;
    return style_keyword_set(s, NS_CSS_FILTER, "none") ||
           style_keyword_set(s, NS_CSS_CLIP_PATH, "none") ||
           style_keyword_set(s, NS_CSS_MASK_IMAGE, "none") ||
           style_keyword_set(s, NS_CSS_MIX_BLEND_MODE, "normal");
}

static gboolean
box_preserve3d(const ns_box *b)
{
    if (!b->style || !b->style->values[NS_CSS_TRANSFORM_STYLE]) return FALSE;
    const char *kw = ns_style_keyword(b->style, NS_CSS_TRANSFORM_STYLE);
    if (!kw || strcmp(kw, "preserve-3d") != 0) return FALSE;
    return !box_flattens_3d(b);
}

static double
box_perspective_px(const ns_box *b)
{
    const ns_css_value *v =
        b->style ? b->style->values[NS_CSS_PERSPECTIVE] : NULL;
    if (v && v->kind == NS_CSS_V_LENGTH && v->u.length.v > 0)
        return v->u.length.v;
    return 0;
}

static gboolean
box_establishes_3d(const ns_box *b)
{
    if (!b->first_child) return FALSE;
    if (box_perspective_px(b) > 0) return TRUE;
    if (!box_preserve3d(b)) return FALSE;
    return !(b->parent && box_preserve3d(b->parent));
}

static gboolean
box_has_own_decor(const ns_box *b)
{
    const ns_style *st = b->style;
    if (!st) return FALSE;
    const ns_css_value *bg = st->values[NS_CSS_BACKGROUND_COLOR];
    if (bg && bg->kind == NS_CSS_V_COLOR && bg->u.color.a > 0) return TRUE;
    const ns_css_value *bi = st->values[NS_CSS_BACKGROUND_IMAGE];
    if (bi && (bi->kind == NS_CSS_V_URL || bi->kind == NS_CSS_V_GRADIENT))
        return TRUE;
    if (b->border.left > 0 || b->border.right > 0 ||
        b->border.top > 0 || b->border.bottom > 0)
        return TRUE;
    const ns_css_value *ow = st->values[NS_CSS_OUTLINE_WIDTH];
    const ns_css_value *os = st->values[NS_CSS_OUTLINE_STYLE];
    if (ow && os && os->kind == NS_CSS_V_KEYWORD && os->u.keyword &&
        strcmp(os->u.keyword, "none") != 0 && length_or(ow, 0) > 0)
        return TRUE;
    return FALSE;
}

static gboolean
box_subtree_paints(const ns_box *b)
{
    if (box_hides_subtree(b)) return FALSE;
    gboolean self_hidden = box_is_hidden(b);
    if (!self_hidden &&
        (b->kind == NS_BOX_IMAGE || b->kind == NS_BOX_VIDEO ||
         b->kind == NS_BOX_INLINE || b->kind == NS_BOX_TEXT))
        return TRUE;
    if (!self_hidden && box_has_own_decor(b)) return TRUE;
    if (box_skips_contents(b)) return FALSE;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        if (box_subtree_paints(c)) return TRUE;
    return FALSE;
}

static void
box_transform_origin(const ns_box *b, double bx, double by, double bw,
                     double bh, ns_css_prop prop,
                     double *ox, double *oy, double *oz)
{
    *ox = bx + bw / 2.0;
    *oy = by + bh / 2.0;
    *oz = 0;
    const ns_css_value *origin = b->style ? b->style->values[prop] : NULL;
    if (origin && origin->kind == NS_CSS_V_TRANSFORM &&
        origin->u.transform.n_ops > 0) {
        const ns_css_transform_op *o = &origin->u.transform.ops[0];
        *ox = bx + (o->a_is_percent ? o->a / 100.0 * bw : o->a);
        *oy = by + (o->b_is_percent ? o->b / 100.0 * bh : o->b);
        *oz = o->c;
    }
}

static gboolean
box_transform_mat4(const ns_box *b, ns_mat4 *out, gboolean *affine2d)
{
    const ns_css_transform *anim_tf =
        g_paint_anim ? ns_anim_get_transform(g_paint_anim, b->dom) : NULL;
    ns_css_transform eff;
    eff.n_ops = 0;
    if (anim_tf ||
        (b->style && (b->style->values[NS_CSS_TRANSFORM] ||
                      b->style->values[NS_CSS_TRANSLATE] ||
                      b->style->values[NS_CSS_ROTATE] ||
                      b->style->values[NS_CSS_SCALE])))
        ns_css_style_effective_transform(b->style, anim_tf, &eff);
    if (eff.n_ops == 0) return FALSE;
    double bx, by, bw, bh;
    box_border_rect(b, &bx, &by, &bw, &bh);
    double ox, oy, oz;
    box_transform_origin(b, bx, by, bw, bh, NS_CSS_TRANSFORM_ORIGIN,
                         &ox, &oy, &oz);
    ns_mat4 tm;
    ns_css_transform_to_mat4(&eff, bw, bh, &tm);
    if (affine2d) *affine2d = ns_mat4_is_affine2d(&tm);
    ns_mat4_identity(out);
    ns_mat4_translate(out, ox, oy, oz);
    ns_mat4_multiply(out, &tm, out);
    ns_mat4_translate(out, -ox, -oy, -oz);
    return TRUE;
}

static gboolean
box_3d_root_own_mat4(const ns_box *b, ns_mat4 *out)
{
    gboolean affine2d = TRUE;
    return box_establishes_3d(b) && b != g_paint_tex_root &&
           box_transform_mat4(b, out, &affine2d) && !affine2d;
}

static void
collect_3d_quads(const ns_box *b, const ns_mat4 *pm, GArray *quads)
{
    if (box_hides_subtree(b) || b == g_paint_skip_box) return;
    double bx, by, bw, bh;
    box_border_rect(b, &bx, &by, &bw, &bh);
    ns_mat4 m = *pm;
    ns_mat4 own;
    if (box_transform_mat4(b, &own, NULL))
        ns_mat4_multiply(&m, &own, &m);
    gboolean p3d = box_preserve3d(b);
    if (!p3d || !b->first_child) {
        if (box_subtree_paints(b)) {
            ns_quad3 q = { b, m, bx, by, bw, bh, 0, quads->len, FALSE };
            g_array_append_val(quads, q);
        }
        return;
    }
    if (box_has_own_decor(b)) {
        ns_quad3 q = { b, m, bx, by, bw, bh, 0, quads->len, TRUE };
        g_array_append_val(quads, q);
    }
    ns_mat4 cm = m;
    double d = box_perspective_px(b);
    if (d > 0) {
        double pox, poy, poz;
        box_transform_origin(b, bx, by, bw, bh, NS_CSS_PERSPECTIVE_ORIGIN,
                             &pox, &poy, &poz);
        ns_mat4_translate(&cm, pox, poy, 0);
        ns_mat4_perspective(&cm, d);
        ns_mat4_translate(&cm, -pox, -poy, 0);
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        collect_3d_quads(c, &cm, quads);
}

static void
paint_quad3(cairo_t *cr, const ns_quad3 *q, const char *highlight)
{
    if (q->bw < 0.5 || q->bh < 0.5) return;
    const double weps = 0.02;
    double px[4], py[4], pws[4];
    const double cxs[4] = { q->bx, q->bx + q->bw, q->bx + q->bw, q->bx };
    const double cys[4] = { q->by, q->by, q->by + q->bh, q->by + q->bh };
    int behind = 0;
    for (int i = 0; i < 4; i++) {
        double ox, oy, oz, ow;
        ns_mat4_apply(&q->m, cxs[i], cys[i], 0, &ox, &oy, &oz, &ow);
        pws[i] = ow;
        if (ow < weps) {
            behind++;
            px[i] = 0;
            py[i] = 0;
            continue;
        }
        px[i] = ox / ow;
        py[i] = oy / ow;
    }
    if (behind == 4) return;
    gboolean clipped = behind > 0;
    double clx0, cly0, clx1, cly1;
    cairo_clip_extents(cr, &clx0, &cly0, &clx1, &cly1);
    double minx = 0, maxx = 0, miny = 0, maxy = 0;
    if (!clipped) {
        double area2 = (px[1] - px[0]) * (py[3] - py[0]) -
                       (px[3] - px[0]) * (py[1] - py[0]);
        if (fabs(area2) < 0.01) return;
        if (area2 < 0 && q->box->style) {
            const char *bfv =
                ns_style_keyword(q->box->style, NS_CSS_BACKFACE_VISIBILITY);
            if (bfv && strcmp(bfv, "hidden") == 0) return;
        }
        minx = px[0]; maxx = px[0]; miny = py[0]; maxy = py[0];
        for (int i = 1; i < 4; i++) {
            minx = MIN(minx, px[i]);
            maxx = MAX(maxx, px[i]);
            miny = MIN(miny, py[i]);
            maxy = MAX(maxy, py[i]);
        }
        if (maxx < clx0 || minx > clx1 || maxy < cly0 || miny > cly1) return;
    } else {
        minx = clx0; maxx = clx1; miny = cly0; maxy = cly1;
    }

    double k = clipped ? 2.0
                       : MAX((maxx - minx) / MAX(q->bw, 1.0),
                             (maxy - miny) / MAX(q->bh, 1.0));
    k = CLAMP(k, 1.0, 3.0);
    double maxdim = MAX(q->bw, q->bh);
    if (maxdim > 0.0 && maxdim * k > 4096.0)
        k = 4096.0 / maxdim;
    int tw = (int)ceil(q->bw * k);
    int th = (int)ceil(q->bh * k);
    if (tw < 1 || th < 1) return;
    if (tw > 4096) { k *= 4096.0 / tw; tw = 4096; th = (int)ceil(q->bh * k); }
    if (th > 4096) { k *= 4096.0 / th; th = 4096; tw = (int)ceil(q->bw * k); }
    gboolean cacheable = q->box->dom && !q->own_only &&
                         !q->box->first_child &&
                         q->box->kind == NS_BOX_BLOCK &&
                         !(q->box->dom->name &&
                           strcmp(q->box->dom->name, "canvas") == 0);
    if (cacheable && g_paint_anim) {
        double anim_op;
        guint8 anim_col[4];
        if (ns_anim_get_opacity(g_paint_anim, q->box->dom, &anim_op) ||
            ns_anim_get_color(g_paint_anim, q->box->dom,
                              NS_CSS_ANIM_TARGET_COLOR, anim_col) ||
            ns_anim_get_color(g_paint_anim, q->box->dom,
                              NS_CSS_ANIM_TARGET_BG_COLOR, anim_col))
            cacheable = FALSE;
    }
    cairo_surface_t *tex = NULL;
    if (cacheable && g_paint_3d_tex) {
        quad_tex_entry *e =
            g_hash_table_lookup(g_paint_3d_tex, (gpointer)q->box);
        if (e && e->tw == tw && e->th == th)
            tex = cairo_surface_reference(e->surf);
    }
    if (!tex) {
        tex = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
        if (cairo_surface_status(tex) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(tex);
            return;
        }
        cairo_t *tcr = cairo_create(tex);
        cairo_scale(tcr, k, k);
        cairo_translate(tcr, -q->bx, -q->by);
        const ns_box *saved_root = g_paint_tex_root;
        const ns_box *saved_flush = g_paint_flush_box;
        g_paint_tex_root = q->box;
        g_paint_flush_box = q->box;
        g_paint_no_cull++;
        if (q->own_only)
            paint_block(tcr, q->box);
        else
            paint_walk(tcr, q->box, highlight);
        g_paint_no_cull--;
        g_paint_tex_root = saved_root;
        g_paint_flush_box = saved_flush;
        cairo_destroy(tcr);
        const ns_style *st = q->box->style;
        const char *filter_kw = st && st->values[NS_CSS_FILTER] &&
            st->values[NS_CSS_FILTER]->kind == NS_CSS_V_KEYWORD
            ? st->values[NS_CSS_FILTER]->u.keyword : NULL;
        if (filter_kw && filter_has_bitmap_effect(filter_kw)) {
            cairo_surface_flush(tex);
            apply_image_filter(cairo_image_surface_get_data(tex),
                               cairo_image_surface_get_stride(tex),
                               tw, th, filter_kw, 1.0, NULL);
            cairo_surface_mark_dirty(tex);
        }
        if (cacheable) {
            if (!g_paint_3d_tex)
                g_paint_3d_tex =
                    g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, quad_tex_entry_free);
            quad_tex_entry *e = g_new(quad_tex_entry, 1);
            e->surf = cairo_surface_reference(tex);
            e->tw = tw;
            e->th = th;
            g_hash_table_replace(g_paint_3d_tex, (gpointer)q->box, e);
        }
    }

    double wmin = pws[0], wmax = pws[0];
    for (int i = 1; i < 4; i++) {
        wmin = MIN(wmin, pws[i]);
        wmax = MAX(wmax, pws[i]);
    }
    int n = 1;
    if (clipped) {
        n = 32;
    } else if ((wmax - wmin) / MAX(wmin, 1e-9) > 0.02) {
        double dim = MAX(maxx - minx, maxy - miny);
        n = (int)(dim / 24.0);
        n = CLAMP(n, 2, 32);
    }
    int np = n + 1;
    double *gx = g_new(double, (gsize)np * np * 3);
    double *gy = gx + np * np;
    double *gw = gy + np * np;
    for (int j = 0; j <= n; j++)
        for (int i = 0; i <= n; i++) {
            double ox, oy, oz, ow;
            ns_mat4_apply(&q->m,
                          q->bx + q->bw * i / n,
                          q->by + q->bh * j / n, 0,
                          &ox, &oy, &oz, &ow);
            gw[j * np + i] = ow;
            if (ow < weps) {
                gx[j * np + i] = 0;
                gy[j * np + i] = 0;
            } else {
                gx[j * np + i] = ox / ow;
                gy[j * np + i] = oy / ow;
            }
        }
    cairo_pattern_t *pat = cairo_pattern_create_for_surface(tex);
    cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
    cairo_pattern_set_filter(pat, CAIRO_FILTER_GOOD);
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            double cws[4] = { gw[j * np + i], gw[j * np + i + 1],
                              gw[(j + 1) * np + i + 1], gw[(j + 1) * np + i] };
            int cell_behind = 0;
            for (int c2 = 0; c2 < 4; c2++)
                if (cws[c2] < weps) cell_behind++;
            if (cell_behind == 4) continue;
            double sx[8], sy[8], ss[8], st[8];
            int nv;
            if (cell_behind == 0) {
                sx[0] = gx[j * np + i];       sy[0] = gy[j * np + i];
                sx[1] = gx[j * np + i + 1];   sy[1] = gy[j * np + i + 1];
                sx[2] = gx[(j + 1) * np + i + 1];
                sy[2] = gy[(j + 1) * np + i + 1];
                sx[3] = gx[(j + 1) * np + i]; sy[3] = gy[(j + 1) * np + i];
                ss[0] = 0; st[0] = 0; ss[1] = 1; st[1] = 0;
                ss[2] = 1; st[2] = 1; ss[3] = 0; st[3] = 1;
                nv = 4;
            } else {
                const double cs[4] = { 0, 1, 1, 0 };
                const double ct[4] = { 0, 0, 1, 1 };
                nv = 0;
                for (int c2 = 0; c2 < 4; c2++) {
                    int c3 = (c2 + 1) & 3;
                    gboolean in_a = cws[c2] >= weps;
                    gboolean in_b = cws[c3] >= weps;
                    if (in_a) {
                        ss[nv] = cs[c2];
                        st[nv] = ct[c2];
                        nv++;
                    }
                    if (in_a != in_b) {
                        double t = (weps - cws[c2]) / (cws[c3] - cws[c2]);
                        ss[nv] = cs[c2] + (cs[c3] - cs[c2]) * t;
                        st[nv] = ct[c2] + (ct[c3] - ct[c2]) * t;
                        nv++;
                    }
                }
                if (nv < 3) continue;
                for (int v = 0; v < nv; v++) {
                    double ox, oy, oz, ow;
                    ns_mat4_apply(&q->m,
                                  q->bx + q->bw * (i + ss[v]) / n,
                                  q->by + q->bh * (j + st[v]) / n, 0,
                                  &ox, &oy, &oz, &ow);
                    if (ow < weps * 0.5) { nv = 0; break; }
                    sx[v] = ox / ow;
                    sy[v] = oy / ow;
                }
                if (nv < 3) continue;
            }
            double d_s1 = ss[1] - ss[0], d_t1 = st[1] - st[0];
            double d_s2 = ss[2] - ss[0], d_t2 = st[2] - st[0];
            double pdet = d_s1 * d_t2 - d_s2 * d_t1;
            if (fabs(pdet) < 1e-9) continue;
            double ma = ((sx[1] - sx[0]) * d_t2 - (sx[2] - sx[0]) * d_t1) / pdet;
            double mc = ((sx[2] - sx[0]) * d_s1 - (sx[1] - sx[0]) * d_s2) / pdet;
            double mb = ((sy[1] - sy[0]) * d_t2 - (sy[2] - sy[0]) * d_t1) / pdet;
            double md = ((sy[2] - sy[0]) * d_s1 - (sy[1] - sy[0]) * d_s2) / pdet;
            double me = sx[0] - ma * ss[0] - mc * st[0];
            double mf = sy[0] - mb * ss[0] - md * st[0];
            double ccx = 0, ccy = 0;
            for (int v = 0; v < nv; v++) { ccx += sx[v]; ccy += sy[v]; }
            ccx /= nv;
            ccy /= nv;
            double pad = n > 1 ? 0.35 : 0.0;
            cairo_save(cr);
            cairo_new_path(cr);
            for (int v = 0; v < nv; v++) {
                double dx2 = sx[v] - ccx, dy2 = sy[v] - ccy;
                double dlen = sqrt(dx2 * dx2 + dy2 * dy2);
                double ex = sx[v], ey = sy[v];
                if (dlen > 1e-9) {
                    ex += dx2 / dlen * pad;
                    ey += dy2 / dlen * pad;
                }
                if (v == 0) cairo_move_to(cr, ex, ey);
                else        cairo_line_to(cr, ex, ey);
            }
            cairo_close_path(cr);
            cairo_clip(cr);
            cairo_matrix_t cm2;
            cairo_matrix_init(&cm2, ma, mb, mc, md, me, mf);
            cairo_transform(cr, &cm2);
            cairo_matrix_t pm2;
            cairo_matrix_init(&pm2, (double)tw / n, 0, 0, (double)th / n,
                              (double)tw * i / n, (double)th * j / n);
            cairo_pattern_set_matrix(pat, &pm2);
            cairo_set_source(cr, pat);
            cairo_rectangle(cr, -0.5, -0.5, 2.0, 2.0);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }
    cairo_pattern_destroy(pat);
    cairo_surface_destroy(tex);
    g_free(gx);
}

static int
quad3_cmp(gconstpointer pa, gconstpointer pb)
{
    const ns_quad3 *a = pa, *b = pb;
    if (a->depth < b->depth) return -1;
    if (a->depth > b->depth) return 1;
    if (a->seq < b->seq) return -1;
    if (a->seq > b->seq) return 1;
    return 0;
}

static GHashTable *g_paint_3d_reg;

static void
paint_3d_reg_value_free(gpointer v)
{
    g_array_free(v, TRUE);
}

void
ns_paint_3d_invalidate(void)
{
    if (g_paint_3d_reg) g_hash_table_remove_all(g_paint_3d_reg);
    if (g_paint_3d_tex) g_hash_table_remove_all(g_paint_3d_tex);
}

gboolean
ns_paint_box_clip_hides(const ns_box *b)
{
    return box_clip_hides(b);
}

gboolean
ns_paint_box_paints_children(const ns_box *b)
{
    return !box_hides_subtree(b) && !box_clip_hides(b) &&
           !box_skips_contents(b);
}

gboolean
ns_paint_3d_registered(const ns_box *b)
{
    if (g_paint_3d_reg && g_hash_table_contains(g_paint_3d_reg, (gpointer)b))
        return TRUE;
    return box_establishes_3d(b);
}

static void
paint_3d_register(const ns_box *root, const GArray *quads)
{
    if (!g_paint_3d_reg)
        g_paint_3d_reg = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                               NULL, paint_3d_reg_value_free);
    GArray *snap = g_array_sized_new(FALSE, FALSE, sizeof(ns_quad3),
                                     quads->len);
    g_array_append_vals(snap, quads->data, quads->len);
    g_hash_table_replace(g_paint_3d_reg, (gpointer)root, snap);
}

static gboolean
quad3_unproject(const ns_quad3 *q, double sx, double sy,
                double *u_out, double *v_out, double *depth_out)
{
    const double *m = q->m.m;
    double a = m[0] * q->bw, b = m[1] * q->bh;
    double c = m[0] * q->bx + m[1] * q->by + m[3];
    double d = m[4] * q->bw, e = m[5] * q->bh;
    double f = m[4] * q->bx + m[5] * q->by + m[7];
    double g = m[12] * q->bw, h = m[13] * q->bh;
    double k = m[12] * q->bx + m[13] * q->by + m[15];
    double i0 = e * k - f * h;
    double i1 = c * h - b * k;
    double i2 = b * f - c * e;
    double i3 = f * g - d * k;
    double i4 = a * k - c * g;
    double i5 = c * d - a * f;
    double i6 = d * h - e * g;
    double i7 = b * g - a * h;
    double i8 = a * e - b * d;
    double det = a * i0 + b * i3 + c * i6;
    if (fabs(det) < 1e-12) return FALSE;
    double tu = i0 * sx + i1 * sy + i2;
    double tv = i3 * sx + i4 * sy + i5;
    double tw = i6 * sx + i7 * sy + i8;
    if (fabs(tw) < 1e-12) return FALSE;
    double u = tu / tw, v = tv / tw;
    if (u < -0.002 || u > 1.002 || v < -0.002 || v > 1.002) return FALSE;
    double px = q->bx + u * q->bw, py = q->by + v * q->bh;
    double w = m[12] * px + m[13] * py + m[15];
    if (w < 1e-6) return FALSE;
    double z = m[8] * px + m[9] * py + m[11];
    *u_out = u;
    *v_out = v;
    *depth_out = z / w;
    return TRUE;
}

typedef struct quad_pick_cand {
    guint idx;
    double depth, u, v;
} quad_pick_cand;

static int
quad_pick_cand_cmp(gconstpointer pa, gconstpointer pb)
{
    const quad_pick_cand *a = pa, *b = pb;
    if (a->depth > b->depth) return -1;
    if (a->depth < b->depth) return 1;
    if (a->idx > b->idx) return -1;
    if (a->idx < b->idx) return 1;
    return 0;
}

static GArray *collect_root_quads(const ns_box *b);

const ns_box *
ns_paint_3d_pick(const ns_box *root3d, double x, double y)
{
    GArray *quads = g_paint_3d_reg
        ? g_hash_table_lookup(g_paint_3d_reg, (gpointer)root3d) : NULL;
    if (!quads) {
        GArray *fresh = collect_root_quads(root3d);
        paint_3d_register(root3d, fresh);
        g_array_free(fresh, TRUE);
        quads = g_hash_table_lookup(g_paint_3d_reg, (gpointer)root3d);
        if (!quads) return NULL;
    }
    GArray *cands = g_array_new(FALSE, FALSE, sizeof(quad_pick_cand));
    for (guint i = 0; i < quads->len; i++) {
        const ns_quad3 *q = &g_array_index(quads, ns_quad3, i);
        quad_pick_cand c = { i, 0, 0, 0 };
        if (quad3_unproject(q, x, y, &c.u, &c.v, &c.depth))
            g_array_append_val(cands, c);
    }
    g_array_sort(cands, quad_pick_cand_cmp);
    const ns_box *result = NULL;
    for (guint i = 0; i < cands->len && !result; i++) {
        const quad_pick_cand *c = &g_array_index(cands, quad_pick_cand, i);
        const ns_quad3 *q = &g_array_index(quads, ns_quad3, c->idx);
        double lx = q->bx + c->u * q->bw;
        double ly = q->by + c->v * q->bh;
        result = ns_box_hit_test(q->box, lx, ly);
    }
    g_array_free(cands, TRUE);
    return result;
}

static GArray *
collect_root_quads(const ns_box *b)
{
    GArray *quads = g_array_new(FALSE, FALSE, sizeof(ns_quad3));
    ns_mat4 root;
    if (box_3d_root_own_mat4(b, &root)) {
        if (!box_hides_subtree(b) && box_has_own_decor(b)) {
            double bx, by, bw, bh;
            box_border_rect(b, &bx, &by, &bw, &bh);
            ns_quad3 q = { b, root, bx, by, bw, bh, 0, quads->len, TRUE };
            g_array_append_val(quads, q);
        }
    } else {
        ns_mat4_identity(&root);
    }
    if (box_establishes_3d(b)) {
        double d = box_perspective_px(b);
        if (d > 0) {
            double bx, by, bw, bh;
            box_border_rect(b, &bx, &by, &bw, &bh);
            double pox, poy, poz;
            box_transform_origin(b, bx, by, bw, bh,
                                 NS_CSS_PERSPECTIVE_ORIGIN,
                                 &pox, &poy, &poz);
            ns_mat4_translate(&root, pox, poy, 0);
            ns_mat4_perspective(&root, d);
            ns_mat4_translate(&root, -pox, -poy, 0);
        }
        for (const ns_box *c = b->first_child; c; c = c->next_sibling)
            collect_3d_quads(c, &root, quads);
    } else {
        collect_3d_quads(b, &root, quads);
    }
    for (guint i = 0; i < quads->len; i++) {
        ns_quad3 *q = &g_array_index(quads, ns_quad3, i);
        double ox, oy, oz, ow;
        ns_mat4_apply(&q->m, q->bx + q->bw / 2.0, q->by + q->bh / 2.0, 0,
                      &ox, &oy, &oz, &ow);
        if (ow > 1e-6) {
            q->depth = oz / ow;
        } else {
            q->depth = -1e30;
            const double cxs[4] = { q->bx, q->bx + q->bw,
                                    q->bx + q->bw, q->bx };
            const double cys[4] = { q->by, q->by,
                                    q->by + q->bh, q->by + q->bh };
            for (int c = 0; c < 4; c++) {
                ns_mat4_apply(&q->m, cxs[c], cys[c], 0, &ox, &oy, &oz, &ow);
                if (ow > 1e-6 && oz / ow > q->depth) q->depth = oz / ow;
            }
        }
    }
    g_array_sort(quads, quad3_cmp);
    return quads;
}

static void
paint_3d_root_below_quads(cairo_t *cr, const ns_box *b, const char *highlight)
{
    if (!box_establishes_3d(b)) return;
    ns_mat4 own;
    if ((b->kind == NS_BOX_BLOCK || b->kind == NS_BOX_TABLE ||
         b->kind == NS_BOX_TABLE_CAPTION || b->kind == NS_BOX_TABLE_CELL) &&
        !box_3d_root_own_mat4(b, &own))
        paint_block(cr, b);
    paint_negative_layers(cr, b, highlight);
}

gboolean
ns_paint_box_establishes_3d(const ns_box *b)
{
    return box_establishes_3d(b);
}

static gboolean
apply_3d_root_affine(cairo_t *cr, const ns_box *b, const ns_css_transform *tf)
{
    double bx, by, bw, bh;
    box_border_rect(b, &bx, &by, &bw, &bh);
    double ox, oy, oz;
    box_transform_origin(b, bx, by, bw, bh, NS_CSS_TRANSFORM_ORIGIN,
                         &ox, &oy, &oz);
    ns_mat4 m;
    ns_css_transform_to_mat4(tf, bw, bh, &m);
    if (!ns_mat4_is_affine2d(&m)) return FALSE;
    cairo_matrix_t cm;
    cairo_matrix_init(&cm, m.m[0], m.m[4], m.m[1], m.m[5], m.m[3], m.m[7]);
    cairo_matrix_t inverse = cm;
    if (cairo_matrix_invert(&inverse) != CAIRO_STATUS_SUCCESS) return FALSE;
    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_transform(cr, &cm);
    cairo_translate(cr, -ox, -oy);
    return TRUE;
}

static void
paint_3d_root(cairo_t *cr, const ns_box *b, const char *highlight)
{
    paint_3d_root_below_quads(cr, b, highlight);
    GArray *quads = collect_root_quads(b);
    paint_3d_register(b, quads);
    if (g_getenv("NS_3D_DEBUG")) {
        fprintf(stderr, "3d root <%s class=%s> %u quads\n",
                b->dom && b->dom->name ? b->dom->name : "?",
                b->dom ? (ns_element_get_attr(b->dom, "class") ?: "") : "",
                quads->len);
        for (guint i = 0; i < quads->len; i++) {
            const ns_quad3 *q = &g_array_index(quads, ns_quad3, i);
            double cxs[4] = { q->bx, q->bx + q->bw, q->bx + q->bw, q->bx };
            double cys[4] = { q->by, q->by, q->by + q->bh, q->by + q->bh };
            fprintf(stderr, "  quad <%s class=%s> rect %.0f,%.0f %gx%g depth %.2f corners",
                    q->box->dom && q->box->dom->name ? q->box->dom->name : "?",
                    q->box->dom ? (ns_element_get_attr(q->box->dom, "class") ?: "") : "",
                    q->bx, q->by, q->bw, q->bh, q->depth);
            for (int c = 0; c < 4; c++) {
                double ox, oy, oz, ow;
                ns_mat4_apply(&q->m, cxs[c], cys[c], 0, &ox, &oy, &oz, &ow);
                if (ow > 1e-6)
                    fprintf(stderr, " (%.1f,%.1f)", ox / ow, oy / ow);
                else
                    fprintf(stderr, " (w=%.3f!)", ow);
            }
            fprintf(stderr, "\n");
        }
    }
    g_paint_no_cull++;
    for (guint i = 0; i < quads->len; i++) {
        double w0 = 0, w1 = 0, w2 = 0, w3 = 0;
        if (g_dbg_paint_x >= 0)
            cairo_clip_extents(cr, &w0, &w1, &w2, &w3);
        paint_quad3(cr, &g_array_index(quads, ns_quad3, i), highlight);
        if (g_dbg_paint_x >= 0) {
            double v0, v1, v2, v3;
            cairo_clip_extents(cr, &v0, &v1, &v2, &v3);
            if (fabs(v0 - w0) > 0.5 || fabs(v1 - w1) > 0.5 ||
                fabs(v2 - w2) > 0.5 || fabs(v3 - w3) > 0.5) {
                const ns_quad3 *q = &g_array_index(quads, ns_quad3, i);
                g_printerr("[quad3-LEAK] <%s class=%s> rect %.0f,%.0f "
                           "%gx%g\n",
                           q->box->dom && q->box->dom->name
                               ? q->box->dom->name : "?",
                           q->box->dom
                               ? (ns_element_get_attr(q->box->dom, "class")
                                      ?: "")
                               : "",
                           q->bx, q->by, q->bw, q->bh);
            }
        }
    }
    g_paint_no_cull--;
    g_array_free(quads, TRUE);
}

static void
ns_dbg_paint_probe(cairo_t *cr, const ns_box *b)
{
    if (g_dbg_paint_x == -2) {
        const char *s = g_getenv("NS_DBG_PAINT_AT");
        g_dbg_paint_x = g_dbg_paint_y = -1;
        if (s) sscanf(s, "%d,%d", &g_dbg_paint_x, &g_dbg_paint_y);
    }
    if (g_dbg_paint_x < 0) return;
    if (isnan(b->x) || isnan(b->y) ||
        isnan(b->content_width) || isnan(b->content_height)) {
        GString *ch = g_string_new("[paint-NAN]");
        for (const ns_box *p2 = b; p2; p2 = p2->parent) {
            const char *nm = p2->dom && p2->dom->name ? p2->dom->name : "?";
            const char *id = p2->dom && p2->dom->kind == NS_NODE_ELEMENT
                           ? ns_element_get_attr(p2->dom, "id") : NULL;
            g_string_append_printf(ch, " <%s#%s%s>", nm, id ? id : "",
                                   isnan(p2->x) ? " NAN" : "");
        }
        g_printerr("%s\n", ch->str);
        g_string_free(ch, TRUE);
    }
    if (b->dom && b->dom->name &&
        strcmp(b->dom->name, "ytd-watch-metadata") == 0) {
        GString *chain = g_string_new("[paint-chain]");
        for (const ns_box *p2 = b; p2; p2 = p2->parent) {
            const char *nm = p2->dom && p2->dom->name ? p2->dom->name : "?";
            const char *id = p2->dom && p2->dom->kind == NS_NODE_ELEMENT
                           ? ns_element_get_attr(p2->dom, "id") : NULL;
            g_string_append_printf(chain, " <%s#%s y=%.0f h=%.0f>",
                                   nm, id ? id : "", p2->y,
                                   p2->content_height);
        }
        g_printerr("%s\n", chain->str);
        g_string_free(chain, TRUE);
    }
    double x0 = b->x, y0 = b->y;
    double x1 = b->x + b->content_width, y1 = b->y + b->content_height;
    cairo_user_to_device(cr, &x0, &y0);
    cairo_user_to_device(cr, &x1, &y1);
    if (isnan(x0) && !isnan(b->x)) {
        GString *ch = g_string_new("[paint-CTM-NAN]");
        for (const ns_box *p2 = b; p2; p2 = p2->parent) {
            const char *nm = p2->dom && p2->dom->name ? p2->dom->name : "?";
            const char *id = p2->dom && p2->dom->kind == NS_NODE_ELEMENT
                           ? ns_element_get_attr(p2->dom, "id") : NULL;
            g_string_append_printf(ch, " <%s#%s sx=%.0f sy=%.0f>",
                                   nm, id ? id : "",
                                   p2->scroll_x, p2->scroll_y);
        }
        g_printerr("%s\n", ch->str);
        g_string_free(ch, TRUE);
    }
    if (g_dbg_paint_x < x0 || g_dbg_paint_x > x1 ||
        g_dbg_paint_y < y0 || g_dbg_paint_y > y1)
        return;
    const ns_css_value *bgv =
        b->style ? b->style->values[NS_CSS_BACKGROUND_COLOR] : NULL;
    char bg[64] = "-";
    if (bgv && bgv->kind == NS_CSS_V_COLOR)
        g_snprintf(bg, sizeof bg, "rgba(%d,%d,%d,%d)",
                   (int)bgv->u.color.r, (int)bgv->u.color.g,
                   (int)bgv->u.color.b, (int)bgv->u.color.a);
    double kx0, ky0, kx1, ky1;
    cairo_clip_extents(cr, &kx0, &ky0, &kx1, &ky1);
    g_printerr("[paint-at] <%s> %.0f,%.0f %.0fx%.0f bg=%s clip=%.0f,%.0f..%.0f,%.0f\n",
               b->dom && b->dom->name ? b->dom->name : "?",
               x0, y0, x1 - x0, y1 - y0, bg, kx0, ky0, kx1, ky1);
}

static gboolean
paint_owns_layer_scope(const ns_box *b, gboolean grouped,
                       gboolean has_transform, gboolean clip_overflow,
                       gboolean has_path_clip)
{
    if (grouped || has_transform || clip_overflow || has_path_clip)
        return TRUE;
    return b == g_paint_tex_root || ns_box_is_stacking_context(b);
}

typedef enum {
    BOX_TF_OK,
    BOX_TF_INVALID,
    BOX_TF_DEGENERATE,
} box_tf_state;

static box_tf_state
box_transform_matrix(const ns_box *b, const ns_css_transform *tf,
                     cairo_matrix_t *out)
{
    double bx, by, bw, bh;
    box_border_rect(b, &bx, &by, &bw, &bh);
    double ox, oy, oz;
    box_transform_origin(b, bx, by, bw, bh, NS_CSS_TRANSFORM_ORIGIN,
                         &ox, &oy, &oz);
    ns_mat4 m;
    ns_css_transform_to_mat4(tf, bw, bh, &m);
    if (isnan(m.m[0]) || isnan(m.m[4]) || isnan(m.m[1]) ||
        isnan(m.m[5]) || isnan(m.m[3]) || isnan(m.m[7]) ||
        isnan(ox) || isnan(oy))
        return BOX_TF_INVALID;
    if (fabs(m.m[0] * m.m[5] - m.m[1] * m.m[4]) < 1e-6)
        return BOX_TF_DEGENERATE;
    cairo_matrix_t cm;
    cairo_matrix_init(&cm, m.m[0], m.m[4], m.m[1], m.m[5], m.m[3], m.m[7]);
    cairo_matrix_init_translate(out, ox, oy);
    cairo_matrix_multiply(out, &cm, out);
    cairo_matrix_translate(out, -ox, -oy);
    return BOX_TF_OK;
}

#define NS_BACKDROP_MAX_PIXELS (16 * 1024 * 1024)

typedef struct {
    cairo_surface_t *surf;
    double x, y;
    double scale_x, scale_y;
} backdrop_image;

static const char *
box_backdrop_filter(const ns_box *b)
{
    const ns_css_value *v = b->style
        ? b->style->values[NS_CSS_BACKDROP_FILTER] : NULL;
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword ||
        !*v->u.keyword || g_ascii_strcasecmp(v->u.keyword, "none") == 0)
        return NULL;
    return v->u.keyword;
}

static void
backdrop_device_bounds(cairo_t *cr, const ns_box *b, const cairo_matrix_t *tf,
                       double *x0, double *y0, double *x1, double *y1)
{
    double bx, by, bw, bh;
    box_border_rect(b, &bx, &by, &bw, &bh);
    cairo_save(cr);
    if (tf) cairo_transform(cr, tf);
    const double corners[4][2] = {
        { bx, by }, { bx + bw, by }, { bx, by + bh }, { bx + bw, by + bh },
    };
    *x0 = *y0 = G_MAXDOUBLE;
    *x1 = *y1 = -G_MAXDOUBLE;
    for (int i = 0; i < 4; i++) {
        double x = corners[i][0], y = corners[i][1];
        cairo_user_to_device(cr, &x, &y);
        *x0 = MIN(*x0, x);
        *y0 = MIN(*y0, y);
        *x1 = MAX(*x1, x);
        *y1 = MAX(*y1, y);
    }
    cairo_restore(cr);
}

static int
backdrop_mirror(int i, int n)
{
    if (n <= 1) return 0;
    int period = 2 * n;
    i %= period;
    if (i < 0) i += period;
    return i < n ? i : period - 1 - i;
}

static void
backdrop_capture(cairo_t *cr, const ns_box *b, const char *filter,
                 const cairo_matrix_t *tf, backdrop_image *out)
{
    cairo_surface_t *target = cairo_get_group_target(cr);
    if (!target || cairo_surface_get_type(target) != CAIRO_SURFACE_TYPE_IMAGE)
        return;
    cairo_format_t fmt = cairo_image_surface_get_format(target);
    if (fmt != CAIRO_FORMAT_ARGB32 && fmt != CAIRO_FORMAT_RGB24) return;
    int tw = cairo_image_surface_get_width(target);
    int th = cairo_image_surface_get_height(target);
    if (tw <= 0 || th <= 0) return;
    double off_x, off_y, scale_x, scale_y;
    cairo_surface_get_device_offset(target, &off_x, &off_y);
    cairo_surface_get_device_scale(target, &scale_x, &scale_y);
    double x0, y0, x1, y1;
    backdrop_device_bounds(cr, b, tf, &x0, &y0, &x1, &y1);
    int px0 = MAX(0, (int)floor(x0 * scale_x + off_x));
    int py0 = MAX(0, (int)floor(y0 * scale_y + off_y));
    int px1 = MIN(tw, (int)ceil(x1 * scale_x + off_x));
    int py1 = MIN(th, (int)ceil(y1 * scale_y + off_y));
    int iw = px1 - px0, ih = py1 - py0;
    double pad_px = filter_blur_sigma(filter) * 3.0 * MAX(scale_x, scale_y);
    int pad = pad_px >= 0.0 && pad_px <= 4096.0 ? (int)ceil(pad_px)
            : pad_px > 4096.0 ? 4096 : 0;
    if (iw <= 0 || ih <= 0) return;
    int w = iw + 2 * pad, h = ih + 2 * pad;
    if ((gint64)w * h > NS_BACKDROP_MAX_PIXELS) return;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return;
    }
    cairo_surface_flush(target);
    cairo_surface_flush(surf);
    const guchar *src = cairo_image_surface_get_data(target);
    int src_stride = cairo_image_surface_get_stride(target);
    guchar *dst = cairo_image_surface_get_data(surf);
    int dst_stride = cairo_image_surface_get_stride(surf);
    guint32 opaque = fmt == CAIRO_FORMAT_RGB24 ? 0xff000000u : 0;
    for (int y = 0; y < h; y++) {
        const guchar *srow =
            src + (py0 + backdrop_mirror(y - pad, ih)) * src_stride;
        guchar *drow = dst + y * dst_stride;
        for (int x = 0; x < w; x++) {
            guint32 px;
            memcpy(&px, srow + (px0 + backdrop_mirror(x - pad, iw)) * 4,
                   sizeof px);
            px |= opaque;
            memcpy(drow + x * 4, &px, sizeof px);
        }
    }
    apply_image_filter(dst, dst_stride, w, h, filter, MAX(scale_x, scale_y),
                       NULL);
    cairo_surface_mark_dirty(surf);
    out->surf = surf;
    out->x = px0 - pad - off_x;
    out->y = py0 - pad - off_y;
    out->scale_x = scale_x;
    out->scale_y = scale_y;
}

static void
backdrop_paint(cairo_t *cr, const ns_box *b, const backdrop_image *bd)
{
    double bx, by, bw, bh;
    box_border_rect(b, &bx, &by, &bw, &bh);
    cairo_save(cr);
    cairo_new_path(cr);
    rounded_rect_path(cr, bx, by, bw, bh, box_border_radii(b));
    cairo_clip(cr);
    cairo_identity_matrix(cr);
    cairo_scale(cr, 1.0 / bd->scale_x, 1.0 / bd->scale_y);
    cairo_set_source_surface(cr, bd->surf, bd->x, bd->y);
    cairo_paint(cr);
    cairo_restore(cr);
}

static gboolean
box_position_is(const ns_box *b, const char *kw)
{
    return b->style && keyword_is(b->style->values[NS_CSS_POSITION], kw);
}

static gboolean
box_is_positioned(const ns_box *b)
{
    const ns_css_value *p = b->style ? b->style->values[NS_CSS_POSITION] : NULL;
    return p && !keyword_is(p, "static");
}

static gboolean
box_moves_contents(const ns_box *b)
{
    const ns_style *s = b->style;
    if (!s) return FALSE;
    if (keyword_is(s->values[NS_CSS_POSITION], "sticky")) return TRUE;
    return s->values[NS_CSS_TRANSFORM] || s->values[NS_CSS_TRANSLATE] ||
           s->values[NS_CSS_ROTATE] || s->values[NS_CSS_SCALE];
}

static void
box_overflow_clip_axes(const ns_box *b, gboolean *clip_x, gboolean *clip_y)
{
    const char *ovs = ns_style_keyword(b->style, NS_CSS_OVERFLOW);
    const char *ovx = ns_style_keyword(b->style, NS_CSS_OVERFLOW_X);
    const char *ovy = ns_style_keyword(b->style, NS_CSS_OVERFLOW_Y);
    if (!ovx) ovx = ovs;
    if (!ovy) ovy = ovs;
    *clip_x = overflow_kw_clips(ovx);
    *clip_y = overflow_kw_clips(ovy);
    if (*clip_x != *clip_y &&
        g_ascii_strcasecmp(*clip_x ? ovx : ovy, "clip") != 0)
        *clip_x = *clip_y = TRUE;
}

static gboolean
box_overflow_clip_rect(const ns_box *b, double *px, double *py,
                       double *pw, double *ph)
{
    const ns_style *bs = b->style;
    if (!bs || !b->parent ||
        ns_node_is_element_named(b->dom, "html") ||
        ns_node_is_element_named(b->dom, "body"))
        return FALSE;
    gboolean clip_x, clip_y;
    box_overflow_clip_axes(b, &clip_x, &clip_y);
    if (!clip_x && !clip_y) return FALSE;
    if (b->kind != NS_BOX_BLOCK && b->kind != NS_BOX_TABLE_CAPTION &&
        b->kind != NS_BOX_TABLE_CELL)
        return FALSE;
    *px = b->x + b->margin.left + b->border.left;
    *py = b->y + b->margin.top  + b->border.top;
    *pw = MAX(0, b->content_width + b->padding.left + b->padding.right);
    *ph = MAX(0, b->content_height + b->padding.top + b->padding.bottom);
    if (isnan(*px) || isnan(*py) || isnan(*pw) || isnan(*ph)) {
        if (g_dbg_paint_x >= 0)
            g_printerr("[paint-nan-guard] overflow-clip <%s>\n",
                       b->dom && b->dom->name ? b->dom->name : "?");
        *px = *py = 0;
        *pw = *ph = 0;
    }
    gboolean explicit_h =
        (bs->values[NS_CSS_MAX_HEIGHT] &&
         (bs->values[NS_CSS_MAX_HEIGHT]->kind == NS_CSS_V_LENGTH ||
          bs->values[NS_CSS_MAX_HEIGHT]->kind == NS_CSS_V_CALC)) ||
        (bs->values[NS_CSS_HEIGHT] &&
         (bs->values[NS_CSS_HEIGHT]->kind == NS_CSS_V_LENGTH ||
          bs->values[NS_CSS_HEIGHT]->kind == NS_CSS_V_CALC));
    gboolean explicit_w =
        (bs->values[NS_CSS_MAX_WIDTH] &&
         (bs->values[NS_CSS_MAX_WIDTH]->kind == NS_CSS_V_LENGTH ||
          bs->values[NS_CSS_MAX_WIDTH]->kind == NS_CSS_V_CALC)) ||
        (bs->values[NS_CSS_WIDTH] &&
         (bs->values[NS_CSS_WIDTH]->kind == NS_CSS_V_LENGTH ||
          bs->values[NS_CSS_WIDTH]->kind == NS_CSS_V_CALC));
    gboolean sized_by_container = ns_box_is_flex_or_grid_item(b);
    return (!clip_x || *pw > 0 || explicit_w || sized_by_container) &&
           (!clip_y || *ph > 0 || explicit_h || sized_by_container);
}

static void
box_overflow_clip_path(cairo_t *cr, const ns_box *b, double px, double py,
                       double pw, double ph)
{
    gboolean clip_x, clip_y;
    box_overflow_clip_axes(b, &clip_x, &clip_y);
    if (!clip_x || !clip_y) {
        double ex0, ey0, ex1, ey1;
        cairo_clip_extents(cr, &ex0, &ey0, &ex1, &ey1);
        if (clip_x) cairo_rectangle(cr, px, ey0, pw, ey1 - ey0);
        else        cairo_rectangle(cr, ex0, py, ex1 - ex0, ph);
        return;
    }
    corner_radii radii = box_border_radii(b);
    if (!corner_radii_zero(radii))
        rounded_rect_path(cr, px, py, pw, ph, radii);
    else
        cairo_rectangle(cr, px, py, pw, ph);
}

static gboolean
box_scroll_offset(const ns_box *b, double *sx, double *sy)
{
    double px, py, pw, ph;
    if (!box_overflow_clip_rect(b, &px, &py, &pw, &ph)) return FALSE;
    gboolean scrolled = !isnan(b->scroll_x) && !isnan(b->scroll_y);
    *sx = scrolled ? b->scroll_x : 0;
    *sy = scrolled ? b->scroll_y : 0;
    return TRUE;
}

static gboolean
box_has_clip_path(const ns_box *b)
{
    const ns_css_value *cp = b->style ? b->style->values[NS_CSS_CLIP_PATH] : NULL;
    return (b->kind == NS_BOX_BLOCK || b->kind == NS_BOX_TABLE ||
            b->kind == NS_BOX_TABLE_CAPTION || b->kind == NS_BOX_TABLE_CELL) &&
           cp && cp->kind == NS_CSS_V_KEYWORD && cp->u.keyword &&
           strcmp(cp->u.keyword, "none") != 0;
}

static gboolean
box_groups_contents(const ns_box *b)
{
    const ns_style *s = b->style;
    if (!s || box_is_positioned(b)) return FALSE;
    const ns_css_value *cp = s->values[NS_CSS_CLIP_PATH];
    return box_opacity(b) < 0.999 ||
           blend_mode_operator(s) != CAIRO_OPERATOR_OVER ||
           mask_layers_paintable(s) ||
           (cp && !keyword_is(cp, "none")) || ns_box_layer_z(b) != 0;
}

static gboolean
box_outside_paint_clip(const ns_box *b)
{
    if (!g_paint_have_clip || !(b->paint_bottom > b->paint_top))
        return FALSE;
    double top = b->paint_top + g_paint_anchor_dy;
    double bottom = b->paint_bottom + g_paint_anchor_dy;
    return bottom < g_paint_clip_y0 - g_paint_cull_margin ||
           top > g_paint_clip_y1 + g_paint_cull_margin;
}

static const ns_box *
box_child_for_dom(const ns_box *parent, const ns_node *dom, int depth)
{
    for (const ns_box *c = parent->first_child; c; c = c->next_sibling) {
        if (c->dom == dom) return c;
        if (!c->dom && depth < 2) {
            const ns_box *hit = box_child_for_dom(c, dom, depth + 1);
            if (hit) return hit;
        }
    }
    return NULL;
}

#define NS_ESCAPE_MAX_DEPTH 32

static int
abs_box_dom_chain(const ns_box *c, const ns_box *cb,
                  const ns_node *chain[NS_ESCAPE_MAX_DEPTH])
{
    if (!box_position_is(c, "absolute") || !c->dom || !cb->dom ||
        !c->dom->parent || c->dom->parent == cb->dom)
        return 0;
    int n = 0;
    const ns_node *d = c->dom->parent;
    for (; d && d != cb->dom && n < NS_ESCAPE_MAX_DEPTH; d = d->parent)
        chain[n++] = d;
    return d == cb->dom ? n : 0;
}

typedef struct escape_step {
    const ns_box *box;
    double dx, dy;
} escape_step;

typedef struct abs_escape {
    const ns_box *owner;
    double owner_dx, owner_dy;
    int n_effects, n_clips;
    escape_step effects[NS_ESCAPE_MAX_DEPTH];
    escape_step clips[NS_ESCAPE_MAX_DEPTH];
} abs_escape;

static void
abs_box_escape(const ns_box *c, const ns_box *cb, abs_escape *out)
{
    const ns_node *chain[NS_ESCAPE_MAX_DEPTH];
    int n = abs_box_dom_chain(c, cb, chain);
    const ns_box *cur = cb;
    double dx = 0, dy = 0;
    int n_clips = 0;
    out->owner = NULL;
    out->owner_dx = out->owner_dy = 0;
    out->n_effects = out->n_clips = 0;
    for (int i = n - 1; i >= 0; i--) {
        cur = box_child_for_dom(cur, chain[i], 0);
        if (!cur || box_moves_contents(cur)) break;
        escape_step step = { cur, dx, dy };
        double sx, sy;
        if (box_scroll_offset(cur, &sx, &sy)) {
            out->clips[n_clips++] = step;
            dx -= sx;
            dy -= sy;
        }
        if (box_groups_contents(cur) && !box_hides_subtree(cur)) {
            out->effects[out->n_effects++] = step;
            out->owner = cur;
            out->owner_dx = -dx;
            out->owner_dy = -dy;
            out->n_clips = n_clips;
        }
    }
    if (out->owner && box_outside_paint_clip(out->owner)) {
        out->owner = NULL;
        out->n_clips = 0;
    }
}

static gboolean
abs_box_adopted(const ns_box *c, const ns_box *cb)
{
    abs_escape esc;
    abs_box_escape(c, cb, &esc);
    return esc.owner && esc.n_clips == 0;
}

static gboolean
paint_adopted_abs_boxes(cairo_t *cr, const ns_box *e, const char *highlight)
{
    if (!box_groups_contents(e)) return FALSE;
    gboolean any = FALSE;
    for (const ns_box *p = e->parent; p; p = p->parent) {
        for (const ns_box *c = p->first_child; c; c = c->next_sibling) {
            abs_escape esc;
            abs_box_escape(c, p, &esc);
            if (!esc.owner) continue;
            if (!cr) {
                for (int i = 0; i < esc.n_effects; i++)
                    if (esc.effects[i].box == e) return TRUE;
                continue;
            }
            if (esc.owner != e) continue;
            const ns_box *saved = g_paint_adopted_box;
            g_paint_adopted_box = c;
            cairo_save(cr);
            cairo_translate(cr, esc.owner_dx, esc.owner_dy);
            paint_walk(cr, c, highlight);
            cairo_restore(cr);
            g_paint_adopted_box = saved;
            any = TRUE;
        }
        if (box_is_positioned(p) || box_moves_contents(p)) break;
    }
    return any;
}

static const ns_box *g_paint_escaped_box;

static void
paint_in_escaped_effects(cairo_t *cr, const ns_box *b, const escape_step *fx,
                         int n, const char *highlight)
{
    if (n == 0) {
        const ns_box *saved = g_paint_escaped_box;
        g_paint_escaped_box = b;
        paint_walk(cr, b, highlight);
        g_paint_escaped_box = saved;
        return;
    }
    const ns_box *r = fx->box;
    double op = box_opacity(r);
    if (op <= 0.0) return;
    cairo_operator_t blend = blend_mode_operator(r->style);
    gboolean mask = mask_layers_paintable(r->style);
    gboolean grouped = op < 0.999 || blend != CAIRO_OPERATOR_OVER || mask;
    cairo_save(cr);
    if (grouped) cairo_push_group(cr);
    if (box_has_clip_path(r)) {
        cairo_translate(cr, fx->dx, fx->dy);
        apply_box_content_clip(cr, r);
        cairo_translate(cr, -fx->dx, -fx->dy);
    }
    paint_in_escaped_effects(cr, b, fx + 1, n - 1, highlight);
    if (grouped) {
        cairo_pop_group_to_source(cr);
        if (blend != CAIRO_OPERATOR_OVER) cairo_set_operator(cr, blend);
        cairo_pattern_t *mp = NULL;
        if (mask) {
            cairo_translate(cr, fx->dx, fx->dy);
            mp = mask_layers_pattern(cr, r, NULL, op);
        }
        if (mp) {
            cairo_mask(cr, mp);
            cairo_pattern_destroy(mp);
        } else {
            cairo_paint_with_alpha(cr, op);
        }
    }
    cairo_restore(cr);
}

static void
erase_escaped_clips(cairo_t *cr, const abs_escape *esc)
{
    cairo_save(cr);
    for (int i = 0; i < esc->n_clips; i++) {
        const escape_step *step = &esc->clips[i];
        double px, py, pw, ph;
        if (!box_overflow_clip_rect(step->box, &px, &py, &pw, &ph)) continue;
        cairo_translate(cr, step->dx, step->dy);
        box_overflow_clip_path(cr, step->box, px, py, pw, ph);
        cairo_translate(cr, -step->dx, -step->dy);
        cairo_clip(cr);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OUT);
    cairo_paint(cr);
    cairo_restore(cr);
}

static gboolean
paint_escaped_abs_box(cairo_t *cr, const ns_box *b, const char *highlight)
{
    if (b == g_paint_escaped_box || b == g_paint_adopted_box ||
        b == g_paint_tex_root || !b->parent ||
        !box_position_is(b, "absolute"))
        return FALSE;
    abs_escape esc;
    abs_box_escape(b, b->parent, &esc);
    if (esc.n_effects == 0 || (esc.owner && esc.n_clips == 0))
        return FALSE;
    gboolean erase = esc.owner && !b->negative_lifted;
    if (erase) {
        cairo_save(cr);
        cairo_push_group(cr);
    }
    paint_in_escaped_effects(cr, b, esc.effects, esc.n_effects, highlight);
    if (erase) {
        erase_escaped_clips(cr, &esc);
        cairo_pop_group_to_source(cr);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    return TRUE;
}

static void
paint_walk(cairo_t *cr, const ns_box *b, const char *highlight)
{
    if (!b) return;
    if (g_paint_collect_stats) g_paint_stats.boxes_seen++;
    if (box_hides_subtree(b)) {
        if (g_paint_collect_stats) g_paint_stats.hidden++;
        return;
    }
    gboolean self_hidden = box_is_hidden(b);
    ns_dbg_paint_probe(cr, b);
    if (box_clip_hides(b)) {
        if (g_paint_collect_stats) g_paint_stats.hidden++;
        return;
    }
    if (paint_box_skipped(b)) {
        if (g_paint_collect_stats) g_paint_stats.skipped_top++;
        return;
    }
    if (g_paint_defer_depth > 0 && b != g_paint_flush_box &&
        ns_box_is_upper_layer(b)) {
        if (!g_paint_deferred_list)
            g_paint_deferred_list =
                g_ptr_array_new_with_free_func(deferred_capture_free);
        deferred_capture *cap = g_new0(deferred_capture, 1);
        cap->box = b;
        cap->seq = g_paint_capture_seq++;
        cap->adopted = b == g_paint_adopted_box;
        cairo_user_to_device(cr, &cap->dev_x, &cap->dev_y);
        cap->no_cull = g_paint_no_cull > 0;
        g_ptr_array_add(g_paint_deferred_list, cap);
        if (g_dbg_paint_x >= 0 && b->dom && b->dom->name)
            g_printerr("[paint-defer] <%s#%s> y=%.0f h=%.0f\n",
                       b->dom->name,
                       ns_element_get_attr(b->dom, "id")
                           ? ns_element_get_attr(b->dom, "id") : "",
                       b->y, b->content_height);
        return;
    }
    if (!g_paint_no_cull && g_paint_have_clip &&
        b->paint_bottom > b->paint_top) {
        double top = b->paint_top + g_paint_anchor_dy;
        double bottom = b->paint_bottom + g_paint_anchor_dy;
        if (bottom < g_paint_clip_y0 - g_paint_cull_margin ||
            top > g_paint_clip_y1 + g_paint_cull_margin) {
            if (g_paint_collect_stats) g_paint_stats.culled_bounds++;
            return;
        }
    }
    if (paint_escaped_abs_box(cr, b, highlight)) return;
    double dbg_e0 = 0, dbg_e1 = 0, dbg_e2 = 0, dbg_e3 = 0;
    if (g_dbg_paint_x >= 0)
        cairo_clip_extents(cr, &dbg_e0, &dbg_e1, &dbg_e2, &dbg_e3);
    const ns_style *style = b->style;
    gboolean skip_contents = box_skips_contents(b);
    double op = box_opacity(b);
    if (op <= 0.0) {
        if (g_paint_collect_stats) g_paint_stats.hidden++;
        return;
    }
    cairo_operator_t blend = blend_mode_operator(style);
    gboolean mask_grad = mask_layers_paintable(style);
    const char *filter_kw = box_filter_layer_keyword(b);
    gboolean grouped = op < 0.999 || blend != CAIRO_OPERATOR_OVER ||
                       mask_grad || filter_kw;
    double sticky_dx = 0, sticky_dy = 0;
    compute_sticky_offset(b, cr, &sticky_dx, &sticky_dy);
    if (isnan(sticky_dx) || isnan(sticky_dy)) {
        if (g_dbg_paint_x >= 0)
            g_printerr("[paint-nan-guard] sticky <%s>\n",
                       b->dom && b->dom->name ? b->dom->name : "?");
        sticky_dx = sticky_dy = 0;
    }
    gboolean has_sticky = (sticky_dx != 0 || sticky_dy != 0);
    double saved_anchor_dx = g_paint_anchor_dx;
    double saved_anchor_dy = g_paint_anchor_dy;
    if (has_sticky) {
        cairo_save(cr);
        cairo_translate(cr, sticky_dx, sticky_dy);
        g_paint_anchor_dx += sticky_dx;
        g_paint_anchor_dy += sticky_dy;
    }
    const ns_css_transform *anim_tf =
        g_paint_anim ? ns_anim_get_transform(g_paint_anim, b->dom) : NULL;
    ns_css_transform eff_tf;
    eff_tf.n_ops = 0;
    if (anim_tf ||
        (style && (style->values[NS_CSS_TRANSFORM] ||
                   style->values[NS_CSS_TRANSLATE] ||
                   style->values[NS_CSS_ROTATE] ||
                   style->values[NS_CSS_SCALE])))
        ns_css_style_effective_transform(style, anim_tf, &eff_tf);
    gboolean has_transform = eff_tf.n_ops > 0;
    if (b == g_paint_tex_root) {
        has_transform = FALSE;
    } else if (box_establishes_3d(b) ||
               (has_transform && ns_css_transform_is_3d(&eff_tf))) {
        double u0 = 0, u1 = 0, u2 = 0, u3 = 0;
        if (g_dbg_paint_x >= 0)
            cairo_clip_extents(cr, &u0, &u1, &u2, &u3);
        gboolean root_tf = has_transform && box_establishes_3d(b) &&
                           apply_3d_root_affine(cr, b, &eff_tf);
        paint_3d_root(cr, b, highlight);
        if (root_tf) cairo_restore(cr);
        if (g_dbg_paint_x >= 0) {
            double z0, z1, z2, z3;
            cairo_clip_extents(cr, &z0, &z1, &z2, &z3);
            if (fabs(z0 - u0) > 0.5 || fabs(z1 - u1) > 0.5 ||
                fabs(z2 - u2) > 0.5 || fabs(z3 - u3) > 0.5)
                g_printerr("[3droot-LEAK] <%s class=%.60s>\n",
                           b->dom && b->dom->name ? b->dom->name : "?",
                           b->dom ? (ns_element_get_attr(b->dom, "class")
                                         ?: "")
                                  : "");
        }
        if (has_sticky) paint_anchor_leave(cr, saved_anchor_dx, saved_anchor_dy);
        return;
    }

    filter_layer fl = { 0 };
    if (filter_kw) {
        filter_layer_state fs = filter_layer_begin(cr, b, filter_kw,
                                                   has_transform, &fl);
        if (fs == FILTER_LAYER_INVISIBLE) {
            if (has_sticky)
                paint_anchor_leave(cr, saved_anchor_dx, saved_anchor_dy);
            return;
        }
        if (fs == FILTER_LAYER_OPEN) {
            cr = fl.cr;
            g_paint_no_cull++;
        }
    }
    gboolean box_offscreen = FALSE;
    if (!has_transform && !has_sticky && !g_paint_no_cull) {
        const ns_css_value *posv = style ? style->values[NS_CSS_POSITION] : NULL;
        gboolean is_fixed = posv && posv->kind == NS_CSS_V_KEYWORD &&
                            posv->u.keyword && strcmp(posv->u.keyword, "fixed") == 0;
        if (!is_fixed) {
            double by = b->y + b->margin.top + g_paint_anchor_dy;
            double bh = b->content_height + b->padding.top + b->padding.bottom +
                        b->border.top + b->border.bottom;
            if (g_paint_have_clip &&
                (by + bh < g_paint_clip_y0 - g_paint_cull_margin ||
                 by > g_paint_clip_y1 + g_paint_cull_margin))
                box_offscreen = TRUE;
        }
    }
    if (box_offscreen && g_paint_collect_stats) g_paint_stats.offscreen++;

    cairo_matrix_t box_tf;
    gboolean have_box_tf = FALSE;
    box_tf_state tf_state = has_transform
        ? box_transform_matrix(b, &eff_tf, &box_tf) : BOX_TF_INVALID;
    backdrop_image backdrop = { 0 };
    const char *backdrop_kw = box_backdrop_filter(b);
    if (backdrop_kw && !fl.cr && !box_offscreen && !self_hidden &&
        (b->kind == NS_BOX_BLOCK || b->kind == NS_BOX_TABLE ||
         b->kind == NS_BOX_TABLE_CAPTION || b->kind == NS_BOX_TABLE_CELL) &&
        (!has_transform || tf_state == BOX_TF_OK))
        backdrop_capture(cr, b, backdrop_kw, has_transform ? &box_tf : NULL,
                         &backdrop);

    gboolean group_clipped = FALSE;
    if (grouped) {
        if (g_paint_collect_stats) g_paint_stats.grouped++;
        if (!has_transform && !has_sticky && !filter_kw &&
            !paint_adopted_abs_boxes(NULL, b, NULL))
            group_clipped = paint_group_clip(cr, b);
        cairo_push_group(cr);
    }
    if (has_transform) {
        cairo_save(cr);
        if (tf_state == BOX_TF_INVALID) {
            if (g_dbg_paint_x >= 0)
                g_printerr("[paint-nan-guard] transform <%s>\n",
                           b->dom && b->dom->name ? b->dom->name : "?");
        } else if (tf_state == BOX_TF_DEGENERATE) {
            cairo_restore(cr);
            if (grouped)
                cairo_pattern_destroy(cairo_pop_group(cr));
            if (group_clipped) cairo_restore(cr);
            if (fl.cr) {
                g_paint_no_cull--;
                cr = fl.outer;
                filter_layer_discard(&fl);
            }
            if (has_sticky) paint_anchor_leave(cr, saved_anchor_dx, saved_anchor_dy);
            return;
        } else {
            have_box_tf = TRUE;
            cairo_transform(cr, &box_tf);
        }
    }
    gboolean has_path_clip = FALSE;
    gboolean clip_after_filter = FALSE;
    if ((b->kind == NS_BOX_BLOCK || b->kind == NS_BOX_TABLE ||
         b->kind == NS_BOX_TABLE_CAPTION || b->kind == NS_BOX_TABLE_CELL) &&
        box_clip_path_text(b)) {
        if (fl.cr) {
            clip_after_filter = TRUE;
        } else {
            cairo_save(cr);
            if (apply_clip_path(cr, b)) has_path_clip = TRUE;
            else                        cairo_restore(cr);
        }
    }
    if (!box_offscreen && !self_hidden) {
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        if (g_dbg_paint_x >= 0)
            cairo_clip_extents(cr, &s0, &s1, &s2, &s3);
        if (b->kind == NS_BOX_BLOCK || b->kind == NS_BOX_TABLE ||
            b->kind == NS_BOX_TABLE_CAPTION ||
            b->kind == NS_BOX_TABLE_ROW || b->kind == NS_BOX_TABLE_CELL ||
            b->kind == NS_BOX_IMAGE || b->kind == NS_BOX_VIDEO ||
            b->kind == NS_BOX_MATH || b->kind == NS_BOX_SVG) {
            if (g_paint_collect_stats) g_paint_stats.blocks++;
            if (backdrop.surf) backdrop_paint(cr, b, &backdrop);
            if (b->kind == NS_BOX_TABLE_ROW)
                paint_table_row(cr, b);
            else
                paint_block(cr, b);
            if (g_dbg_paint_x >= 0) {
                double t0, t1, t2, t3;
                cairo_clip_extents(cr, &t0, &t1, &t2, &t3);
                if (fabs(t0 - s0) > 0.5 || fabs(t1 - s1) > 0.5 ||
                    fabs(t2 - s2) > 0.5 || fabs(t3 - s3) > 0.5)
                    g_printerr("[block-LEAK] <%s class=%.60s>\n",
                               b->dom && b->dom->name ? b->dom->name : "?",
                               b->dom
                                   ? (ns_element_get_attr(b->dom, "class")
                                          ?: "")
                                   : "");
            }
        }
        if (b->kind == NS_BOX_BLOCK)
            paint_marker(cr, b);
        if (b->kind == NS_BOX_INLINE && !skip_contents) {
            if (g_paint_collect_stats) g_paint_stats.inlines++;
            paint_inline(cr, b, highlight);
            if (g_dbg_paint_x >= 0) {
                double t0, t1, t2, t3;
                cairo_clip_extents(cr, &t0, &t1, &t2, &t3);
                if (fabs(t0 - s0) > 0.5 || fabs(t1 - s1) > 0.5 ||
                    fabs(t2 - s2) > 0.5 || fabs(t3 - s3) > 0.5)
                    g_printerr("[inline-LEAK] <%s> text=%.30s\n",
                               b->dom && b->dom->name ? b->dom->name : "?",
                               b->text ? b->text : "");
            }
        }
        if (b->kind == NS_BOX_IMAGE && !skip_contents) {
            if (g_paint_collect_stats) g_paint_stats.images++;
            paint_image(cr, b);
        }
        if (b->kind == NS_BOX_VIDEO && !skip_contents) {
            if (g_paint_collect_stats) g_paint_stats.videos++;
            paint_video(cr, b);
        }
        if (b->kind == NS_BOX_MATH && !skip_contents)
            paint_math(cr, b);
        if (b->kind == NS_BOX_SVG)
            paint_svg(cr, b);
    }
    if (!skip_contents && !self_hidden &&
        ns_node_is_element_named(b->dom, "canvas") && g_paint_js) {
        if (g_paint_collect_stats) g_paint_stats.canvases++;
        cairo_surface_t *surf = ns_js_canvas_surface(g_paint_js, b->dom);
        if (surf) {
            int sw = cairo_image_surface_get_width(surf);
            int sh = cairo_image_surface_get_height(surf);
            if (sw > 0 && sh > 0) {
                double dx = b->x + b->margin.left + b->border.left + b->padding.left;
                double dy = b->y + b->margin.top  + b->border.top  + b->padding.top;
                double dw = b->content_width > 0 ? b->content_width : sw;
                double dh = b->content_height > 0 ? b->content_height : sh;
                cairo_save(cr);
                cairo_translate(cr, dx, dy);
                cairo_scale(cr, dw / sw, dh / sh);
                cairo_set_source_surface(cr, surf, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
            }
        }
    }

    guint n_children = 0;
    if (!skip_contents)
        for (const ns_box *c = b->first_child; c; c = c->next_sibling)
            n_children++;
    paint_entry entries_buf[64];
    paint_entry *entries = n_children <= G_N_ELEMENTS(entries_buf)
        ? entries_buf : g_new(paint_entry, n_children);
    guint order = 0;
    gboolean any_z = FALSE;
    if (!skip_contents) {
        for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
            paint_entry e;
            e.box = c;
            e.order = order++;
            e.key = ns_box_stack_key(c);
            if (e.key != 0) any_z = TRUE;
            entries[e.order] = e;
        }
    }
    if (any_z) {
        if (g_paint_collect_stats) {
            g_paint_stats.sorted_parents++;
            g_paint_stats.sorted_children += n_children;
        }
        qsort(entries, n_children, sizeof(paint_entry), paint_entry_cmp);
    }
    double clip_px, clip_py, clip_pw, clip_ph;
    gboolean clip_overflow =
        box_overflow_clip_rect(b, &clip_px, &clip_py, &clip_pw, &clip_ph);
    gboolean scroll_anchored = FALSE;
    if (clip_overflow) {
        cairo_save(cr);
        box_overflow_clip_path(cr, b, clip_px, clip_py, clip_pw, clip_ph);
        cairo_clip(cr);
        if (g_dbg_paint_x >= 0) {
            double ex0, ey0, ex1, ey1;
            cairo_clip_extents(cr, &ex0, &ey0, &ex1, &ey1);
            g_printerr("[paint-clip%s] <%s#%s> rect %.0f,%.0f %.0fx%.0f"
                       " -> clip %.0f,%.0f..%.0f,%.0f\n",
                       (ey1 - ey0 < 1 || ex1 - ex0 < 1) ? "-EMPTY" : "",
                       b->dom && b->dom->name ? b->dom->name : "?",
                       b->dom ? (ns_element_get_attr(b->dom, "id")
                                 ? ns_element_get_attr(b->dom, "id") : "")
                              : "",
                       clip_px, clip_py, clip_pw, clip_ph, ex0, ey0, ex1, ey1);
        }
        if ((b->scroll_x != 0 || b->scroll_y != 0) &&
            !isnan(b->scroll_x) && !isnan(b->scroll_y)) {
            cairo_translate(cr, -b->scroll_x, -b->scroll_y);
            g_paint_anchor_dx -= b->scroll_x;
            g_paint_anchor_dy -= b->scroll_y;
            scroll_anchored = TRUE;
        }
        if (g_paint_collect_stats) g_paint_stats.overflow_clips++;
    }
    gboolean own_layer_scope = paint_owns_layer_scope(
        b, grouped, has_transform, clip_overflow, has_path_clip);
    GPtrArray *saved_layer_list = NULL;
    if (own_layer_scope) {
        saved_layer_list = g_paint_deferred_list;
        g_paint_deferred_list = NULL;
        g_paint_defer_depth++;
    }
    if (has_transform || has_sticky) g_paint_no_cull++;
    paint_negative_layers(cr, b, highlight);
    for (guint i = 0; i < n_children; i++)
        if (!abs_box_adopted(entries[i].box, b))
            paint_walk(cr, entries[i].box, highlight);
    if (!skip_contents) paint_adopted_abs_boxes(cr, b, highlight);
    if (has_transform || has_sticky) g_paint_no_cull--;
    GPtrArray *deferred_mine = NULL;
    if (own_layer_scope) {
        deferred_mine = g_paint_deferred_list;
        g_paint_deferred_list = saved_layer_list;
        g_paint_defer_depth--;
        if (deferred_mine) {
            if (g_dbg_paint_x >= 0) {
                double fx0, fy0, fx1, fy1;
                cairo_clip_extents(cr, &fx0, &fy0, &fx1, &fy1);
                g_printerr("[paint-flush] owner=<%s#%s> n=%u "
                           "clip=%.0f,%.0f..%.0f,%.0f\n",
                           b->dom && b->dom->name ? b->dom->name : "?",
                           b->dom && ns_element_get_attr(b->dom, "id")
                               ? ns_element_get_attr(b->dom, "id") : "",
                           deferred_mine->len, fx0, fy0, fx1, fy1);
            }
            if (has_transform || has_sticky) g_paint_no_cull++;
            g_layers.flush_layered =
                b == g_layers.root ||
                (b == g_layers.owner && !grouped && !has_transform &&
                 !clip_overflow && !has_path_clip);
            paint_flush_deferred(cr, deferred_mine, highlight);
            if (has_transform || has_sticky) g_paint_no_cull--;
            g_ptr_array_free(deferred_mine, TRUE);
            deferred_mine = NULL;
        }
    }
    if (scroll_anchored) {
        g_paint_anchor_dx += b->scroll_x;
        g_paint_anchor_dy += b->scroll_y;
    }
    const char *sbw_kw = b->style && b->style->values[NS_CSS_SCROLLBAR_WIDTH] &&
        b->style->values[NS_CSS_SCROLLBAR_WIDTH]->kind == NS_CSS_V_KEYWORD
        ? b->style->values[NS_CSS_SCROLLBAR_WIDTH]->u.keyword : NULL;
    gboolean sb_hidden = sbw_kw && strcmp(sbw_kw, "none") == 0;
    double sb_size = (sbw_kw && strcmp(sbw_kw, "thin") == 0) ? 5.0 : 8.0;
    double th_r = 0, th_g = 0, th_b = 0, th_a = 0.40;
    double tk_r = 0, tk_g = 0, tk_b = 0, tk_a = 0.06;
    const char *sbc_kw = b->style && b->style->values[NS_CSS_SCROLLBAR_COLOR] &&
        b->style->values[NS_CSS_SCROLLBAR_COLOR]->kind == NS_CSS_V_KEYWORD
        ? b->style->values[NS_CSS_SCROLLBAR_COLOR]->u.keyword : NULL;
    if (sbc_kw && g_ascii_strcasecmp(sbc_kw, "auto") != 0) {
        char **ct = g_strsplit_set(sbc_kw, " \t", -1);
        int idx = 0;
        for (int i = 0; ct[i] && idx < 2; i++) {
            char *t = g_strstrip(ct[i]);
            guint8 r, g, bb, a;
            if (*t && ns_css_parse_color(t, &r, &g, &bb, &a)) {
                if (idx == 0) { th_r = r/255.0; th_g = g/255.0; th_b = bb/255.0; th_a = a/255.0; }
                else          { tk_r = r/255.0; tk_g = g/255.0; tk_b = bb/255.0; tk_a = a/255.0; }
                idx++;
            }
        }
        g_strfreev(ct);
    }
    if (clip_overflow && b->scrolls && !sb_hidden &&
        (b->scroll_max_x > 0 || b->scroll_max_y > 0)) {
        double px = b->x + b->margin.left + b->border.left;
        double py = b->y + b->margin.top  + b->border.top;
        double pw = b->content_width + b->padding.left + b->padding.right;
        double ph = b->content_height + b->padding.top + b->padding.bottom;
        if (b->scroll_x != 0 || b->scroll_y != 0)
            cairo_translate(cr, b->scroll_x, b->scroll_y);
        if (b->scroll_max_y > 0 && ph > 16) {
            double track_w = sb_size;
            double track_x = px + pw - track_w - 1.0;
            double track_y = py + 1.0;
            double track_h = ph - 2.0;
            double total_h = ph + b->scroll_max_y;
            double thumb_h = track_h * (ph / total_h);
            if (thumb_h < 16.0) thumb_h = 16.0;
            if (thumb_h > track_h) thumb_h = track_h;
            double thumb_y = track_y +
                (track_h - thumb_h) * (b->scroll_y / b->scroll_max_y);
            cairo_save(cr);
            cairo_set_source_rgba(cr, tk_r, tk_g, tk_b, tk_a);
            cairo_rectangle(cr, track_x, track_y, track_w, track_h);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, th_r, th_g, th_b, th_a);
            cairo_rectangle(cr, track_x + 1, thumb_y, track_w - 2, thumb_h);
            cairo_fill(cr);
            cairo_restore(cr);
        }
        if (b->scroll_max_x > 0 && pw > 16) {
            double track_h = sb_size;
            double track_x = px + 1.0;
            double track_y = py + ph - track_h - 1.0;
            double track_w = pw - 2.0 -
                (b->scroll_max_y > 0 ? sb_size : 0.0);
            double total_w = pw + b->scroll_max_x;
            double thumb_w = track_w * (pw / total_w);
            if (thumb_w < 16.0) thumb_w = 16.0;
            if (thumb_w > track_w) thumb_w = track_w;
            double thumb_x = track_x +
                (track_w - thumb_w) * (b->scroll_x / b->scroll_max_x);
            cairo_save(cr);
            cairo_set_source_rgba(cr, tk_r, tk_g, tk_b, tk_a);
            cairo_rectangle(cr, track_x, track_y, track_w, track_h);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, th_r, th_g, th_b, th_a);
            cairo_rectangle(cr, thumb_x, track_y + 1, thumb_w, track_h - 2);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }

    if (b->kind == NS_BOX_BLOCK && b->style && b->columns >= 2) {
        double col_gap = 16;
        int n_cols = b->columns;
        (void)ns_css_used_column_count(b->style, b->content_width, &col_gap);
        {
            double rule_w = length_or(b->style->values[NS_CSS_COLUMN_RULE_WIDTH], 0);
            const ns_css_value *rstyle =
                b->style->values[NS_CSS_COLUMN_RULE_STYLE];
            gboolean rdrawable = rstyle && rstyle->kind == NS_CSS_V_KEYWORD &&
                rstyle->u.keyword && strcmp(rstyle->u.keyword, "none") != 0 &&
                strcmp(rstyle->u.keyword, "hidden") != 0;
            if (rule_w > 0 && rdrawable) {
                rgba rc = rgba_of(b->style->values[NS_CSS_COLUMN_RULE_COLOR],
                                  0.50, 0.50, 0.50, 1.0);
                double inx = b->x + b->margin.left + b->border.left + b->padding.left;
                double iny = b->y + b->margin.top  + b->border.top  + b->padding.top;
                double cw = b->content_width;
                double col_w = (cw - col_gap * (n_cols - 1)) / n_cols;
                cairo_save(cr);
                set_source_rgba(cr, rc);
                cairo_set_line_width(cr, rule_w);
                if (strcmp(rstyle->u.keyword, "dashed") == 0) {
                    double dashes[] = { rule_w * 3, rule_w * 2 };
                    cairo_set_dash(cr, dashes, 2, 0);
                } else if (strcmp(rstyle->u.keyword, "dotted") == 0) {
                    double dashes[] = { rule_w, rule_w };
                    cairo_set_dash(cr, dashes, 2, 0);
                }
                for (int i = 0; i < n_cols - 1; i++) {
                    double rx = inx + col_w * (i + 1) + col_gap * i + col_gap / 2.0;
                    cairo_move_to(cr, rx, iny);
                    cairo_line_to(cr, rx, iny + b->content_height);
                    cairo_stroke(cr);
                }
                cairo_restore(cr);
            }
        }
    }
    if (clip_overflow) cairo_restore(cr);
    if (entries != entries_buf) g_free(entries);

    if (has_path_clip) cairo_restore(cr);
    if (deferred_mine) {
        if (g_dbg_paint_x >= 0) {
            double fx0, fy0, fx1, fy1;
            cairo_clip_extents(cr, &fx0, &fy0, &fx1, &fy1);
            g_printerr("[paint-flush-postclip] owner=<%s#%s> n=%u "
                       "clip=%.0f,%.0f..%.0f,%.0f\n",
                       b->dom && b->dom->name ? b->dom->name : "?",
                       b->dom && ns_element_get_attr(b->dom, "id")
                           ? ns_element_get_attr(b->dom, "id") : "",
                       deferred_mine->len, fx0, fy0, fx1, fy1);
        }
        if (has_transform || has_sticky) g_paint_no_cull++;
        g_layers.flush_layered =
            b == g_layers.root ||
            (b == g_layers.owner && !grouped && !has_transform &&
             !has_path_clip);
        paint_flush_deferred(cr, deferred_mine, highlight);
        if (has_transform || has_sticky) g_paint_no_cull--;
        g_ptr_array_free(deferred_mine, TRUE);
    }

    if (has_transform) cairo_restore(cr);

    if (grouped) {
        cairo_pop_group_to_source(cr);
        cairo_pattern_t *group_source =
            cairo_pattern_reference(cairo_get_source(cr));
        cairo_operator_t saved_op = cairo_get_operator(cr);
        if (blend != CAIRO_OPERATOR_OVER) cairo_set_operator(cr, blend);
        cairo_pattern_t *mp = NULL;
        if (mask_grad)
            mp = mask_layers_pattern(cr, b, have_box_tf ? &box_tf : NULL, op);
        if (mp) {
            cairo_mask(cr, mp);
        } else {
            cairo_paint_with_alpha(cr, op);
        }
        if (blend != CAIRO_OPERATOR_OVER) cairo_set_operator(cr, saved_op);
        if (mp) cairo_pattern_destroy(mp);
        cairo_pattern_destroy(group_source);
        if (group_clipped) cairo_restore(cr);
    }
    if (fl.cr) {
        g_paint_no_cull--;
        cr = fl.outer;
        if (clip_after_filter) {
            cairo_save(cr);
            cairo_matrix_t outer_m;
            cairo_get_matrix(cr, &outer_m);
            if (have_box_tf) cairo_transform(cr, &box_tf);
            apply_clip_path(cr, b);
            cairo_set_matrix(cr, &outer_m);
        }
        filter_layer_end(&fl);
        if (clip_after_filter) cairo_restore(cr);
    }
    if (backdrop.surf) cairo_surface_destroy(backdrop.surf);

    if (has_sticky) paint_anchor_leave(cr, saved_anchor_dx, saved_anchor_dy);
    if (g_dbg_paint_x >= 0) {
        double q0, q1, q2, q3;
        cairo_clip_extents(cr, &q0, &q1, &q2, &q3);
        if (fabs(q0 - dbg_e0) > 0.5 || fabs(q1 - dbg_e1) > 0.5 ||
            fabs(q2 - dbg_e2) > 0.5 || fabs(q3 - dbg_e3) > 0.5)
            g_printerr("[clip-LEAK] <%s#%s class=%.70s kind=%d grp=%d "
                       "tf=%d ov=%d pc=%d st=%d> "
                       "entry=%.0f,%.0f..%.0f,%.0f exit=%.0f,%.0f..%.0f,%.0f\n",
                       b->dom && b->dom->name ? b->dom->name : "?",
                       b->dom && b->dom->kind == NS_NODE_ELEMENT &&
                       ns_element_get_attr(b->dom, "id")
                           ? ns_element_get_attr(b->dom, "id") : "",
                       b->dom && b->dom->kind == NS_NODE_ELEMENT
                           ? (ns_element_get_attr(b->dom, "class") ?: "")
                           : "",
                       (int)b->kind, grouped, has_transform, clip_overflow,
                       has_path_clip, has_sticky,
                       dbg_e0, dbg_e1, dbg_e2, dbg_e3, q0, q1, q2, q3);
    }
}

static const ns_box *
find_element_box_named(const ns_box *b, const char *name)
{
    if (!b) return NULL;
    if (b->dom && b->dom->kind == NS_NODE_ELEMENT && b->dom->name &&
        strcmp(b->dom->name, name) == 0)
        return b;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
        const ns_box *hit = find_element_box_named(c, name);
        if (hit) return hit;
    }
    return NULL;
}

static gboolean
box_solid_background(const ns_box *b, rgba *out)
{
    const ns_style *s = b ? b->style : NULL;
    if (s && s->values[NS_CSS_BACKGROUND_COLOR] &&
        s->values[NS_CSS_BACKGROUND_COLOR]->kind == NS_CSS_V_COLOR &&
        s->values[NS_CSS_BACKGROUND_COLOR]->u.color.a > 0) {
        *out = rgba_of(s->values[NS_CSS_BACKGROUND_COLOR], 1, 1, 1, 1);
        return TRUE;
    }
    return FALSE;
}

static gboolean
canvas_background_of(const ns_box *root, rgba *out)
{
    if (!root) return FALSE;
    const ns_box *html = find_element_box_named(root, "html");
    if (box_solid_background(html, out)) return TRUE;
    const ns_box *body = find_element_box_named(root, "body");
    if (box_solid_background(body, out)) return TRUE;
    return FALSE;
}

static const ns_box *
find_box_for_node(const ns_box *b, const ns_node *node)
{
    if (!b) return NULL;
    if (b->dom == node) return b;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
        const ns_box *hit = find_box_for_node(c, node);
        if (hit) return hit;
    }
    return NULL;
}

static const ns_box *
top_layer_box(const ns_box *root)
{
    const ns_node *modal = ns_dom_active_modal();
    if (!modal) return NULL;
    return find_box_for_node(root, modal);
}

static void
paint_top_layer(cairo_t *cr, const ns_box *root, const char *highlight)
{
    const ns_box *top = top_layer_box(root);
    if (!top) return;
    const ns_style *s = top->style;
    const ns_style *bd = s ? s->backdrop : NULL;
    if (bd && bd->values[NS_CSS_BACKGROUND_COLOR]) {
        rgba c = rgba_of(bd->values[NS_CSS_BACKGROUND_COLOR], 0, 0, 0, 0);
        double x1, y1, x2, y2;
        cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
        cairo_save(cr);
        set_source_rgba(cr, c);
        cairo_rectangle(cr, x1, y1, x2 - x1, y2 - y1);
        cairo_fill(cr);
        cairo_restore(cr);
    }
    const ns_box *saved = g_paint_skip_box;
    const ns_box *saved_flush = g_paint_flush_box;
    g_paint_skip_box = NULL;
    g_paint_flush_box = top;
    paint_walk(cr, top, highlight);
    g_paint_flush_box = saved_flush;
    g_paint_skip_box = saved;
}

static void
paint_document(cairo_t *cr, const ns_box *root, const char *highlight_query,
               const struct ns_selection *sel)
{
    ns_paint_list_ordinals_begin();
    rgba bg = { 254.0 / 255, 254.0 / 255, 254.0 / 255, 1 };
    canvas_background_of(root, &bg);
    cairo_save(cr);
    set_source_rgba(cr, bg);
    cairo_paint(cr);
    cairo_restore(cr);
    paint_cache_clip(cr);
    if (sel)
        g_paint_sel_runs = ns_selection_ranges(root, sel);
    g_paint_skip_box = top_layer_box(root);
    paint_walk(cr, root, highlight_query);
    g_paint_skip_box = NULL;
    paint_top_layer(cr, root, highlight_query);
    g_clear_pointer(&g_paint_sel_runs, g_hash_table_destroy);
    g_paint_have_clip = FALSE;
    g_paint_have_viewport = FALSE;
    ns_paint_list_ordinals_end();
}

void
ns_paint(cairo_t *cr, const ns_box *root, const char *highlight_query)
{
    paint_document(cr, root, highlight_query, NULL);
}

void
ns_paint_with_selection(cairo_t *cr, const ns_box *root,
                        const char *highlight_query,
                        const struct ns_selection *sel)
{
    paint_document(cr, root, highlight_query, sel);
}

gboolean
ns_paint_canvas_color(const ns_box *root, double rgba_out[4])
{
    rgba bg = { 254.0 / 255, 254.0 / 255, 254.0 / 255, 1 };
    gboolean found = canvas_background_of(root, &bg);
    rgba_out[0] = bg.r;
    rgba_out[1] = bg.g;
    rgba_out[2] = bg.b;
    rgba_out[3] = bg.a;
    return found;
}

static gboolean
layers_box_bg_fixed(const ns_box *b)
{
    const ns_css_value *att = b->style
        ? b->style->values[NS_CSS_BACKGROUND_ATTACHMENT] : NULL;
    const ns_css_value *img = b->style
        ? b->style->values[NS_CSS_BACKGROUND_IMAGE] : NULL;
    if (!att || !img || keyword_is(img, "none")) return FALSE;
    int n = MAX(ns_css_value_layer_count(att), 1);
    for (int i = 0; i < n; i++)
        if (keyword_is(ns_css_value_layer(att, i), "fixed")) return TRUE;
    return FALSE;
}

static int
layers_vp_kind(const ns_box *b)
{
    const ns_css_value *pos = b->style ? b->style->values[NS_CSS_POSITION]
                                       : NULL;
    if (keyword_is(pos, "fixed"))
        return ns_box_is_fixed(b) ? NS_PAINT_VP_FIXED : 0;
    if (keyword_is(pos, "sticky") && !ns_box_in_scroller(b))
        return NS_PAINT_VP_STICKY;
    return 0;
}

static int
layers_scan_kind(const ns_box *b, int under, gboolean in_atomic)
{
    if (layers_box_bg_fixed(b)) return -1;
    int kind = layers_vp_kind(b);
    if (!kind) return 0;
    gboolean nested_sticky = under == NS_PAINT_VP_STICKY ||
                             (under && kind == NS_PAINT_VP_STICKY);
    if (in_atomic || nested_sticky || box_z_index(b) < 0) return -1;
    return under ? 0 : kind;
}

static gboolean
layers_scan(const ns_box *b, int under, gboolean in_atomic, GHashTable *kinds,
            int *roots);

static gboolean
layers_scan_children(const ns_box *b, int under, gboolean in_atomic,
                     GHashTable *kinds, int *roots)
{
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        if (!layers_scan(c, under, in_atomic, kinds, roots)) return FALSE;
    guint n = b->inline_atomics ? b->inline_atomics->len : 0;
    for (guint i = 0; i < n; i++) {
        const ns_box *ab =
            g_array_index(b->inline_atomics, ns_inline_atomic, i).box;
        if (ab && !layers_scan(ab, under, TRUE, kinds, roots)) return FALSE;
    }
    return TRUE;
}

static gboolean
layers_scan(const ns_box *b, int under, gboolean in_atomic, GHashTable *kinds,
            int *roots)
{
    if (box_hides_subtree(b) || box_clip_hides(b)) return TRUE;
    int kind = layers_scan_kind(b, under, in_atomic);
    if (kind < 0) return FALSE;
    if (kind > 0) {
        g_hash_table_insert(kinds, (gpointer)b, GINT_TO_POINTER(kind));
        (*roots)++;
        under = kind;
    }
    return box_skips_contents(b) ||
           layers_scan_children(b, under, in_atomic, kinds, roots);
}

void
ns_paint_layer_plan_init(ns_paint_layer_plan *plan)
{
    plan->dynamic = FALSE;
    plan->kinds = g_hash_table_new(g_direct_hash, g_direct_equal);
    plan->vp = g_array_new(FALSE, TRUE, sizeof(ns_paint_vp_capture));
}

void
ns_paint_layer_plan_clear(ns_paint_layer_plan *plan)
{
    g_clear_pointer(&plan->kinds, g_hash_table_destroy);
    if (plan->vp) g_array_free(plan->vp, TRUE);
    plan->vp = NULL;
}

void
ns_paint_plan_layers(cairo_t *cr, const ns_box *root,
                     ns_paint_layer_plan *plan)
{
    plan->dynamic = FALSE;
    g_hash_table_remove_all(plan->kinds);
    g_array_set_size(plan->vp, 0);
    if (!root) return;
    int roots = 0;
    if (top_layer_box(root) ||
        !layers_scan(root, 0, FALSE, plan->kinds, &roots)) {
        plan->dynamic = TRUE;
        return;
    }
    if (roots == 0) return;
    g_layers.mode = PAINT_LAYERS_PLAN;
    g_layers.root = root;
    g_layers.kinds = plan->kinds;
    g_layers.found = plan->vp;
    cairo_get_matrix(cr, &g_layers.base);
    paint_document(cr, root, NULL, NULL);
    memset(&g_layers, 0, sizeof g_layers);
    if ((int)plan->vp->len != roots) plan->dynamic = TRUE;
}

gboolean
ns_paint_doc_layers(cairo_t *cr, ns_paint_upper_fn upper, gpointer upper_data,
                    const ns_box *root, const char *highlight_query,
                    const struct ns_selection *sel,
                    const ns_paint_layer_plan *plan)
{
    g_layers.mode = PAINT_LAYERS_DOC;
    g_layers.root = root;
    g_layers.doc = cr;
    g_layers.kinds = plan->kinds;
    g_layers.upper = upper;
    g_layers.upper_data = upper_data;
    g_layers.n_upper = (int)plan->vp->len;
    paint_document(cr, root, highlight_query, sel);
    memset(&g_layers, 0, sizeof g_layers);
    return TRUE;
}

void
ns_paint_vp_layer(cairo_t *cr, const ns_box *root,
                  const ns_paint_vp_capture *layer, double vp_x, double vp_y,
                  const char *highlight_query,
                  const struct ns_selection *sel)
{
    ns_paint_list_ordinals_begin();
    paint_cache_clip(cr);
    g_paint_vp_x0 = vp_x;
    g_paint_vp_y0 = vp_y;
    g_paint_have_viewport = vp_x != 0 || vp_y != 0;
    if (sel)
        g_paint_sel_runs = ns_selection_ranges(root, sel);
    if (layer->kind == NS_PAINT_VP_STICKY)
        g_paint_sticky_static_box = layer->box;
    const ns_box *saved_flush = g_paint_flush_box;
    cairo_save(cr);
    cairo_matrix_t base, m;
    cairo_get_matrix(cr, &base);
    cairo_matrix_multiply(&m, &layer->rel, &base);
    cairo_set_matrix(cr, &m);
    g_paint_flush_box = layer->box;
    paint_walk(cr, layer->box, highlight_query);
    g_paint_flush_box = saved_flush;
    cairo_restore(cr);
    g_paint_sticky_static_box = NULL;
    g_clear_pointer(&g_paint_sel_runs, g_hash_table_destroy);
    g_paint_have_clip = FALSE;
    g_paint_have_viewport = FALSE;
    ns_paint_list_ordinals_end();
}
