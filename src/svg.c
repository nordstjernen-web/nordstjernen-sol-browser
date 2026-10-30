/* Northstar — SVG rendering onto Cairo, sharing the CSS cascade with HTML.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "svg.h"
#include "svg_external.h"

#include <math.h>
#include <string.h>

#include "ns_pango.h"

#include "css.h"
#include "html.h"
#include "image.h"
#include "paint.h"
#include "net.h"

enum {
    NS_SVG_MAX_DEPTH        = 24,
    NS_SVG_MAX_NESTING      = 256,
    NS_SVG_MAX_NODES        = 60000,
    NS_SVG_MAX_INPUT_BYTES  = 8 * 1024 * 1024,
    NS_SVG_MAX_DIM_PX       = 8192,
    NS_SVG_MAX_PIXELS       = 4096 * 4096,
    NS_SVG_DEFAULT_DIM_PX   = 512,
    NS_SVG_DEFAULT_OBJECT_W = 300,
    NS_SVG_DEFAULT_OBJECT_H = 150,
    NS_SVG_MAX_HREF_CHAIN   = 16,
    NS_SVG_MAX_DASHES       = 256,
    NS_SVG_MAX_PATTERNS     = 4,
    NS_SVG_MAX_TILE_PIXELS  = 2048 * 2048,
    NS_SVG_MAX_IMAGE_NEST   = 4,
    NS_SVG_MAX_IMAGE_BYTES  = 32 * 1024 * 1024,
    NS_SVG_IMAGE_CACHE_LEN  = 32,
    NS_SVG_IMAGE_CACHE_SIZE = 48 * 1024 * 1024,
    NS_SVG_TILE_CACHE_LEN   = 64,
    NS_SVG_TILE_CACHE_SIZE  = 64 * 1024 * 1024,
    NS_SVG_MAX_SOURCE_SCALE = 4096,
    NS_SVG_PROP_WORDS       = (NS_CSS_PROP_COUNT + 63) / 64,
};

typedef enum {
    SVG_PAINT_NONE,
    SVG_PAINT_COLOR,
    SVG_PAINT_REF,
} svg_paint_kind;

typedef struct {
    svg_paint_kind kind;
    double         r, g, b, a;
    char          *ref;
    gboolean       have_fallback;
    double         fr, fg, fb, fa;
} svg_paint;

typedef struct {
    svg_paint         fill;
    svg_paint         stroke;
    double            fill_opacity;
    double            stroke_opacity;
    double            stroke_width;
    double            dash_offset;
    double            miter_limit;
    cairo_line_cap_t  line_cap;
    cairo_line_join_t line_join;
    cairo_fill_rule_t fill_rule;
    cairo_fill_rule_t clip_rule;
    GArray           *dashes;
    double            color_r, color_g, color_b, color_a;
    char             *font_family;
    double            font_size;
    int               font_weight;
    gboolean          font_italic;
    int               text_anchor;
    gboolean          hidden;
    gboolean          stroke_first;
    gboolean          non_scaling_stroke;
    gboolean          instanced;
} svg_state;

typedef struct {
    cairo_t       *cr;
    const ns_node *root;
    GHashTable    *styles;
    GHashTable    *ids;
    int            depth;
    int            nesting;
    int            nodes;
    int            pattern_nodes;
    double         vw, vh;
    double         icb_w, icb_h;
    double         unit_vw, unit_vh;
    double         root_font_size;
    gboolean       declared;
    struct svg_measure *measure;
    const struct ns_style *inherited;
    const ns_node *patterns[NS_SVG_MAX_PATTERNS];
    int            n_patterns;
    GPtrArray     *tiles;
    gsize          tile_bytes;
} svg_ctx;

typedef struct {
    double x, y, w, h;
} svg_rect;

static void svg_render_node(svg_ctx *ctx, const ns_node *n,
                            const svg_state *parent);
static void svg_state_init_inherited(svg_state *st,
                                     const struct ns_style *inherited);

static gboolean
svg_is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static void
svg_skip_sep(const char **pp)
{
    const char *p = *pp;
    while (*p && (svg_is_ws(*p) || *p == ',')) p++;
    *pp = p;
}

static gboolean
svg_num(const char **pp, double *out)
{
    svg_skip_sep(pp);
    const char *p = *pp;
    if (!*p) return FALSE;
    char *end = NULL;
    double v = g_ascii_strtod(p, &end);
    if (!end || end == p) return FALSE;
    if (!isfinite(v)) return FALSE;
    *out = v;
    *pp = end;
    return TRUE;
}

static gboolean
svg_flag(const char **pp, gboolean *out)
{
    svg_skip_sep(pp);
    const char *p = *pp;
    if (*p == '0') { *out = FALSE; *pp = p + 1; return TRUE; }
    if (*p == '1') { *out = TRUE;  *pp = p + 1; return TRUE; }
    return FALSE;
}

static double
svg_length_str(const char *s, double pct_basis, double font_size, double fallback)
{
    if (!s) return fallback;
    while (*s && svg_is_ws(*s)) s++;
    if (!*s) return fallback;
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (!end || end == s || !isfinite(v)) return fallback;
    while (*end && svg_is_ws(*end)) end++;
    if (*end == '%')                        return v / 100.0 * pct_basis;
    if (g_ascii_strncasecmp(end, "px", 2) == 0) return v;
    if (g_ascii_strncasecmp(end, "pt", 2) == 0) return v * 4.0 / 3.0;
    if (g_ascii_strncasecmp(end, "pc", 2) == 0) return v * 16.0;
    if (g_ascii_strncasecmp(end, "mm", 2) == 0) return v * 96.0 / 25.4;
    if (g_ascii_strncasecmp(end, "cm", 2) == 0) return v * 96.0 / 2.54;
    if (g_ascii_strncasecmp(end, "in", 2) == 0) return v * 96.0;
    if (g_ascii_strncasecmp(end, "em", 2) == 0) return v * font_size;
    if (g_ascii_strncasecmp(end, "ex", 2) == 0) return v * font_size * 0.5;
    if (g_ascii_strncasecmp(end, "rem", 3) == 0) return v * 16.0;
    return v;
}

static double
svg_attr_length(const ns_node *n, const char *name, double pct_basis,
                double font_size, double fallback)
{
    const char *s = ns_element_get_attr(n, name);
    return s ? svg_length_str(s, pct_basis, font_size, fallback) : fallback;
}

static double
svg_diag_basis(double w, double h)
{
    return sqrt(w * w + h * h) / G_SQRT2;
}

static void
svg_paint_clear(svg_paint *p)
{
    g_clear_pointer(&p->ref, g_free);
}

static char *
svg_url_id(const char *s, const char **rest)
{
    if (!s) return NULL;
    while (*s && svg_is_ws(*s)) s++;
    if (g_ascii_strncasecmp(s, "url(", 4) != 0) return NULL;
    const char *p = s + 4;
    while (*p && svg_is_ws(*p)) p++;
    char quote = 0;
    if (*p == '"' || *p == '\'') { quote = *p; p++; }
    const char *start = p;
    while (*p && *p != ')' && (!quote || *p != quote)) p++;
    const char *end = p;
    if (quote && *p == quote) p++;
    while (*p && *p != ')') p++;
    if (*p == ')') p++;
    if (rest) *rest = p;
    while (end > start && svg_is_ws(end[-1])) end--;
    if (end > start && *start == '#') start++;
    if (end <= start) return NULL;
    char *raw = g_strndup(start, (gsize)(end - start));
    if (!strchr(raw, '%')) return raw;
    char *dec = g_uri_unescape_string(raw, NULL);
    if (!dec) return raw;
    g_free(raw);
    return dec;
}

static gboolean
svg_parse_paint(const char *text, const svg_state *st, svg_paint *out)
{
    if (!text) return FALSE;
    while (*text && svg_is_ws(*text)) text++;
    if (!*text) return FALSE;

    if (g_ascii_strncasecmp(text, "url(", 4) == 0) {
        const char *rest = NULL;
        char *id = svg_url_id(text, &rest);
        if (!id) return FALSE;
        svg_paint_clear(out);
        out->kind = SVG_PAINT_REF;
        out->ref = id;
        out->have_fallback = FALSE;
        if (rest) {
            while (*rest && svg_is_ws(*rest)) rest++;
            if (*rest) {
                guint8 r, g, b, a;
                if (g_ascii_strncasecmp(rest, "none", 4) == 0) {
                    out->have_fallback = TRUE;
                    out->fa = 0;
                } else if (g_ascii_strncasecmp(rest, "currentcolor", 12) == 0) {
                    out->have_fallback = TRUE;
                    out->fr = st->color_r; out->fg = st->color_g;
                    out->fb = st->color_b; out->fa = st->color_a;
                } else if (ns_css_parse_color(rest, &r, &g, &b, &a)) {
                    out->have_fallback = TRUE;
                    out->fr = r / 255.0; out->fg = g / 255.0;
                    out->fb = b / 255.0; out->fa = a / 255.0;
                }
            }
        }
        return TRUE;
    }
    if (g_ascii_strcasecmp(text, "none") == 0) {
        svg_paint_clear(out);
        out->kind = SVG_PAINT_NONE;
        return TRUE;
    }
    if (g_ascii_strcasecmp(text, "currentcolor") == 0) {
        svg_paint_clear(out);
        out->kind = SVG_PAINT_COLOR;
        out->r = st->color_r; out->g = st->color_g;
        out->b = st->color_b; out->a = st->color_a;
        return TRUE;
    }
    guint8 r, g, b, a;
    if (ns_css_parse_color(text, &r, &g, &b, &a)) {
        svg_paint_clear(out);
        out->kind = SVG_PAINT_COLOR;
        out->r = r / 255.0; out->g = g / 255.0;
        out->b = b / 255.0; out->a = a / 255.0;
        return TRUE;
    }
    return FALSE;
}

static void
svg_state_init(svg_state *st)
{
    memset(st, 0, sizeof *st);
    st->fill.kind = SVG_PAINT_COLOR;
    st->fill.a = 1.0;
    st->stroke.kind = SVG_PAINT_NONE;
    st->fill_opacity = 1.0;
    st->stroke_opacity = 1.0;
    st->stroke_width = 1.0;
    st->miter_limit = 4.0;
    st->line_cap = CAIRO_LINE_CAP_BUTT;
    st->line_join = CAIRO_LINE_JOIN_MITER;
    st->fill_rule = CAIRO_FILL_RULE_WINDING;
    st->clip_rule = CAIRO_FILL_RULE_WINDING;
    st->color_a = 1.0;
    st->font_size = 16.0;
    st->font_weight = 400;
}

static void
svg_state_copy(svg_state *dst, const svg_state *src)
{
    *dst = *src;
    dst->fill.ref = src->fill.ref ? g_strdup(src->fill.ref) : NULL;
    dst->stroke.ref = src->stroke.ref ? g_strdup(src->stroke.ref) : NULL;
    dst->font_family = src->font_family ? g_strdup(src->font_family) : NULL;
    if (src->dashes) {
        dst->dashes = g_array_sized_new(FALSE, FALSE, sizeof(double),
                                        src->dashes->len);
        g_array_append_vals(dst->dashes, src->dashes->data, src->dashes->len);
    }
}

static void
svg_state_clear(svg_state *st)
{
    svg_paint_clear(&st->fill);
    svg_paint_clear(&st->stroke);
    g_clear_pointer(&st->font_family, g_free);
    if (st->dashes) { g_array_free(st->dashes, TRUE); st->dashes = NULL; }
}

static gboolean
svg_dash_next(const char **pp, double *out, double basis, double font_size)
{
    svg_skip_sep(pp);
    const char *token = *pp;
    double v;
    if (!svg_num(pp, &v)) return FALSE;
    const char *p = *pp;
    while (*p && !svg_is_ws(*p) && *p != ',' && *p != '-' && *p != '+' &&
           !g_ascii_isdigit(*p) && *p != '.')
        p++;
    *pp = p;
    *out = svg_length_str(token, basis, font_size, v);
    return TRUE;
}

static void
svg_set_dashes(svg_state *st, const char *text, double basis, double font_size)
{
    if (st->dashes) { g_array_free(st->dashes, TRUE); st->dashes = NULL; }
    if (!text || g_ascii_strcasecmp(text, "none") == 0) return;
    GArray *out = g_array_new(FALSE, FALSE, sizeof(double));
    const char *p = text;
    double v;
    gboolean any_positive = FALSE;
    while (out->len < NS_SVG_MAX_DASHES &&
           svg_dash_next(&p, &v, basis, font_size)) {
        if (v < 0) { g_array_free(out, TRUE); return; }
        if (v > 0) any_positive = TRUE;
        g_array_append_val(out, v);
    }
    if (out->len == 0 || !any_positive) { g_array_free(out, TRUE); return; }
    if (out->len % 2) {
        guint n = out->len;
        for (guint i = 0; i < n; i++) {
            double d = g_array_index(out, double, i);
            g_array_append_val(out, d);
        }
    }
    st->dashes = out;
}

static cairo_line_cap_t
svg_cap_of(const char *s)
{
    if (!s) return CAIRO_LINE_CAP_BUTT;
    if (g_ascii_strcasecmp(s, "round") == 0)  return CAIRO_LINE_CAP_ROUND;
    if (g_ascii_strcasecmp(s, "square") == 0) return CAIRO_LINE_CAP_SQUARE;
    return CAIRO_LINE_CAP_BUTT;
}

static cairo_line_join_t
svg_join_of(const char *s)
{
    if (!s) return CAIRO_LINE_JOIN_MITER;
    if (g_ascii_strcasecmp(s, "round") == 0) return CAIRO_LINE_JOIN_ROUND;
    if (g_ascii_strcasecmp(s, "bevel") == 0) return CAIRO_LINE_JOIN_BEVEL;
    return CAIRO_LINE_JOIN_MITER;
}

static int
svg_anchor_of(const char *s)
{
    if (!s) return 0;
    if (g_ascii_strcasecmp(s, "middle") == 0) return 1;
    if (g_ascii_strcasecmp(s, "end") == 0)    return 2;
    return 0;
}

static gboolean
svg_strip_important(const char *val, gsize *len)
{
    static const char kw[] = "important";
    const gsize kw_len = sizeof kw - 1;
    if (!val || *len < kw_len + 1 ||
        g_ascii_strncasecmp(val + *len - kw_len, kw, kw_len) != 0)
        return FALSE;
    gsize end = *len - kw_len;
    while (end > 0 && svg_is_ws(val[end - 1])) end--;
    if (end == 0 || val[end - 1] != '!') return FALSE;
    end--;
    while (end > 0 && svg_is_ws(val[end - 1])) end--;
    *len = end;
    return TRUE;
}

static const char *
svg_style_decl(const ns_node *n, const char *prop, gboolean *important)
{
    static __thread char buf[256];
    *important = FALSE;
    const char *style = ns_element_get_attr(n, "style");
    if (!style) return NULL;
    gsize plen = strlen(prop);
    const char *p = style;
    const char *found = NULL;
    gsize found_len = 0;
    while (*p) {
        while (*p && (svg_is_ws(*p) || *p == ';')) p++;
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { while (*p && *p != ';') p++; continue; }
        const char *name_end = p;
        while (name_end > name && svg_is_ws(name_end[-1])) name_end--;
        p++;
        while (*p && svg_is_ws(*p)) p++;
        const char *val = p;
        while (*p && *p != ';') p++;
        const char *val_end = p;
        while (val_end > val && svg_is_ws(val_end[-1])) val_end--;
        if ((gsize)(name_end - name) == plen &&
            g_ascii_strncasecmp(name, prop, plen) == 0) {
            found = val;
            found_len = (gsize)(val_end - val);
        }
    }
    *important = svg_strip_important(found, &found_len);
    if (!found || found_len == 0 || found_len >= sizeof buf) return NULL;
    memcpy(buf, found, found_len);
    buf[found_len] = '\0';
    return buf;
}

static __thread GHashTable *g_svg_var_styles;

static const char *
svg_prop(const ns_node *n, const char *name)
{
    gboolean important;
    const char *v = svg_style_decl(n, name, &important);
    if (!v) v = ns_element_get_attr(n, name);
    if (!v || !strstr(v, "var(")) return v;

    const ns_style *st = g_svg_var_styles
        ? g_hash_table_lookup(g_svg_var_styles, (gpointer)n) : NULL;
    if (!st) return v;
    static __thread char buf[256];
    char *resolved = ns_css_resolve_style_vars(v, st);
    if (!resolved) return v;
    if (strlen(resolved) >= sizeof buf) {
        g_free(resolved);
        return v;
    }
    strcpy(buf, resolved);
    g_free(resolved);
    return buf;
}

static void
svg_apply_css_paint(svg_state *st, const ns_style *s, ns_css_prop prop,
                    svg_paint *out)
{
    const ns_css_value *v = s->values[prop];
    if (!v) return;
    if (v->kind == NS_CSS_V_COLOR && ns_style_prop_from_currentcolor(s, prop)) {
        svg_paint_clear(out);
        out->kind = SVG_PAINT_COLOR;
        out->r = st->color_r;
        out->g = st->color_g;
        out->b = st->color_b;
        out->a = st->color_a;
    } else if (v->kind == NS_CSS_V_COLOR) {
        svg_paint_clear(out);
        out->kind = SVG_PAINT_COLOR;
        out->r = v->u.color.r / 255.0;
        out->g = v->u.color.g / 255.0;
        out->b = v->u.color.b / 255.0;
        out->a = v->u.color.a / 255.0;
    } else if (v->kind == NS_CSS_V_KEYWORD && v->u.keyword) {
        svg_parse_paint(v->u.keyword, st, out);
    }
}

typedef enum {
    SVG_UNIT_ONE,
    SVG_UNIT_PCT,
    SVG_UNIT_EM,
    SVG_UNIT_REM,
    SVG_UNIT_VW,
    SVG_UNIT_VH,
    SVG_UNIT_VMIN,
    SVG_UNIT_VMAX,
    SVG_UNIT_BASES,
} svg_unit_base;

static const struct {
    svg_unit_base base;
    double        k;
} svg_units[] = {
    [NS_CSS_UNIT_PX]      = { SVG_UNIT_ONE,  1.0 },
    [NS_CSS_UNIT_EM]      = { SVG_UNIT_EM,   1.0 },
    [NS_CSS_UNIT_REM]     = { SVG_UNIT_REM,  1.0 },
    [NS_CSS_UNIT_PERCENT] = { SVG_UNIT_PCT,  0.01 },
    [NS_CSS_UNIT_NUMBER]  = { SVG_UNIT_ONE,  1.0 },
    [NS_CSS_UNIT_VW]      = { SVG_UNIT_VW,   0.01 },
    [NS_CSS_UNIT_VH]      = { SVG_UNIT_VH,   0.01 },
    [NS_CSS_UNIT_VMIN]    = { SVG_UNIT_VMIN, 0.01 },
    [NS_CSS_UNIT_VMAX]    = { SVG_UNIT_VMAX, 0.01 },
    [NS_CSS_UNIT_CQW]     = { SVG_UNIT_VW,   0.01 },
    [NS_CSS_UNIT_CQH]     = { SVG_UNIT_VH,   0.01 },
    [NS_CSS_UNIT_CQMIN]   = { SVG_UNIT_VMIN, 0.01 },
    [NS_CSS_UNIT_CQMAX]   = { SVG_UNIT_VMAX, 0.01 },
    [NS_CSS_UNIT_EX]      = { SVG_UNIT_EM,   0.5 },
    [NS_CSS_UNIT_CH]      = { SVG_UNIT_EM,   0.5 },
    [NS_CSS_UNIT_CAP]     = { SVG_UNIT_EM,   0.7 },
    [NS_CSS_UNIT_IC]      = { SVG_UNIT_EM,   1.0 },
    [NS_CSS_UNIT_LH]      = { SVG_UNIT_EM,   1.5 },
    [NS_CSS_UNIT_RLH]     = { SVG_UNIT_REM,  1.5 },
    [NS_CSS_UNIT_REX]     = { SVG_UNIT_REM,  0.5 },
    [NS_CSS_UNIT_RCH]     = { SVG_UNIT_REM,  0.5 },
    [NS_CSS_UNIT_RCAP]    = { SVG_UNIT_REM,  0.7 },
    [NS_CSS_UNIT_RIC]     = { SVG_UNIT_REM,  1.0 },
};

static void
svg_unit_bases(const svg_ctx *ctx, double font_size, double basis, double *b)
{
    b[SVG_UNIT_ONE] = 1.0;
    b[SVG_UNIT_PCT] = basis;
    b[SVG_UNIT_EM] = font_size;
    b[SVG_UNIT_REM] = ctx->root_font_size;
    b[SVG_UNIT_VW] = ctx->unit_vw;
    b[SVG_UNIT_VH] = ctx->unit_vh;
    b[SVG_UNIT_VMIN] = MIN(ctx->unit_vw, ctx->unit_vh);
    b[SVG_UNIT_VMAX] = MAX(ctx->unit_vw, ctx->unit_vh);
}

static double
svg_calc_viewport_px(const ns_css_value *v, double w, double h)
{
    return (v->u.calc.vw * w + v->u.calc.vh * h + v->u.calc.vmin * MIN(w, h) +
            v->u.calc.vmax * MAX(w, h)) / 100.0;
}

static double
svg_calc_px(const ns_css_value *v, const double *b)
{
    if (ns_css_calc_is_math_fn(v))
        return ns_css_calc_math_fn_px(v, b[SVG_UNIT_PCT]);
    double page = svg_calc_viewport_px(v, v->u.calc.parsed_vw,
                                       v->u.calc.parsed_vh);
    double own = svg_calc_viewport_px(v, b[SVG_UNIT_VW], b[SVG_UNIT_VH]);
    return v->u.calc.px - page + own +
           v->u.calc.pct / 100.0 * b[SVG_UNIT_PCT] +
           v->u.calc.em * b[SVG_UNIT_EM] + v->u.calc.rem * b[SVG_UNIT_REM];
}

static double
svg_css_length_px(const svg_ctx *ctx, const ns_css_value *v, double font_size,
                  double basis)
{
    double b[SVG_UNIT_BASES];
    svg_unit_bases(ctx, font_size, basis, b);
    if (v->kind == NS_CSS_V_CALC) return svg_calc_px(v, b);
    guint unit = (guint)v->u.length.unit;
    if (unit >= G_N_ELEMENTS(svg_units)) return v->u.length.v;
    return v->u.length.v * svg_units[unit].k * b[svg_units[unit].base];
}

static double
svg_css_number(const svg_ctx *ctx, const ns_style *s, ns_css_prop prop,
               double font_size, double basis, double fallback)
{
    const ns_css_value *v = s->values[prop];
    if (!v || (v->kind != NS_CSS_V_LENGTH && v->kind != NS_CSS_V_CALC))
        return fallback;
    return svg_css_length_px(ctx, v, font_size, basis);
}

static const ns_css_prop svg_inherited_props[] = {
    NS_CSS_COLOR, NS_CSS_FILL, NS_CSS_STROKE, NS_CSS_FILL_OPACITY,
    NS_CSS_STROKE_OPACITY, NS_CSS_STROKE_WIDTH, NS_CSS_STROKE_MITERLIMIT,
    NS_CSS_STROKE_DASHOFFSET, NS_CSS_STROKE_DASHARRAY, NS_CSS_STROKE_LINECAP,
    NS_CSS_STROKE_LINEJOIN, NS_CSS_FILL_RULE, NS_CSS_CLIP_RULE,
    NS_CSS_TEXT_ANCHOR, NS_CSS_PAINT_ORDER, NS_CSS_VISIBILITY,
    NS_CSS_FONT_SIZE, NS_CSS_FONT_WEIGHT, NS_CSS_FONT_STYLE,
    NS_CSS_FONT_FAMILY,
};

static const ns_style *
svg_node_style(svg_ctx *ctx, const svg_state *st, const ns_node *n,
               ns_style *own)
{
    const ns_style *s = ctx->styles
        ? g_hash_table_lookup(ctx->styles, (gpointer)n) : NULL;
    if (!s || !st->instanced) return s;
    *own = *s;
    for (gsize i = 0; i < G_N_ELEMENTS(svg_inherited_props); i++) {
        ns_css_prop prop = svg_inherited_props[i];
        if (ns_style_prop_inherited(s, prop))
            own->values[prop] = NULL;
    }
    return own;
}

static gboolean
svg_css_font_size(const svg_ctx *ctx, const ns_style *s, double inherited,
                  double *out)
{
    const ns_css_value *v = s ? s->values[NS_CSS_FONT_SIZE] : NULL;
    if (!v || (v->kind != NS_CSS_V_LENGTH && v->kind != NS_CSS_V_CALC))
        return FALSE;
    *out = svg_css_length_px(ctx, v, inherited, inherited);
    return TRUE;
}

static void
svg_state_apply_font_size(svg_ctx *ctx, svg_state *st, const ns_style *s,
                          const ns_node *n)
{
    double inherited = st->font_size;
    const char *v = svg_prop(n, "font-size");
    if (v) st->font_size = svg_length_str(v, inherited, inherited, 16.0);
    double css;
    if (svg_css_font_size(ctx, s, inherited, &css) &&
        (ctx->declared || !v || fabs(css - inherited) > 1e-6))
        st->font_size = css;
    st->font_size = MAX(0.0, st->font_size);
    if (n == ctx->root && ctx->declared) ctx->root_font_size = st->font_size;
}

static void
svg_state_apply_node(svg_ctx *ctx, svg_state *st, const ns_node *n)
{
    ns_style own;
    const ns_style *s = svg_node_style(ctx, st, n, &own);
    double basis = svg_diag_basis(ctx->vw, ctx->vh);
    svg_state_apply_font_size(ctx, st, s, n);

    if (s && s->values[NS_CSS_COLOR] &&
        s->values[NS_CSS_COLOR]->kind == NS_CSS_V_COLOR) {
        st->color_r = s->values[NS_CSS_COLOR]->u.color.r / 255.0;
        st->color_g = s->values[NS_CSS_COLOR]->u.color.g / 255.0;
        st->color_b = s->values[NS_CSS_COLOR]->u.color.b / 255.0;
        st->color_a = s->values[NS_CSS_COLOR]->u.color.a / 255.0;
    } else {
        const char *col = svg_prop(n, "color");
        guint8 r, g, b, a;
        if (col && ns_css_parse_color(col, &r, &g, &b, &a)) {
            st->color_r = r / 255.0; st->color_g = g / 255.0;
            st->color_b = b / 255.0; st->color_a = a / 255.0;
        }
    }

    const char *v;
    if ((v = svg_prop(n, "fill")))   svg_parse_paint(v, st, &st->fill);
    if ((v = svg_prop(n, "stroke"))) svg_parse_paint(v, st, &st->stroke);
    if ((v = svg_prop(n, "fill-opacity")))
        st->fill_opacity = CLAMP(svg_length_str(v, 1.0, st->font_size, 1.0), 0.0, 1.0);
    if ((v = svg_prop(n, "stroke-opacity")))
        st->stroke_opacity = CLAMP(svg_length_str(v, 1.0, st->font_size, 1.0), 0.0, 1.0);
    if ((v = svg_prop(n, "stroke-width")))
        st->stroke_width = MAX(0.0, svg_length_str(v, basis, st->font_size, 1.0));
    if ((v = svg_prop(n, "stroke-linecap")))    st->line_cap = svg_cap_of(v);
    if ((v = svg_prop(n, "stroke-linejoin")))   st->line_join = svg_join_of(v);
    if ((v = svg_prop(n, "stroke-miterlimit")))
        st->miter_limit = MAX(1.0, svg_length_str(v, 1.0, st->font_size, 4.0));
    if ((v = svg_prop(n, "stroke-dashoffset")))
        st->dash_offset = svg_length_str(v, basis, st->font_size, 0.0);
    if ((v = svg_prop(n, "stroke-dasharray")))
        svg_set_dashes(st, v, basis, st->font_size);
    if ((v = svg_prop(n, "fill-rule")))
        st->fill_rule = (g_ascii_strcasecmp(v, "evenodd") == 0)
            ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING;
    if ((v = svg_prop(n, "clip-rule")))
        st->clip_rule = (g_ascii_strcasecmp(v, "evenodd") == 0)
            ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING;
    if ((v = svg_prop(n, "text-anchor"))) st->text_anchor = svg_anchor_of(v);
    if ((v = svg_prop(n, "font-family"))) {
        g_free(st->font_family);
        st->font_family = g_strdup(v);
    }
    if ((v = svg_prop(n, "font-weight"))) {
        if (g_ascii_strcasecmp(v, "bold") == 0) st->font_weight = 700;
        else if (g_ascii_strcasecmp(v, "normal") == 0) st->font_weight = 400;
        else {
            int w = atoi(v);
            if (w >= 1 && w <= 1000) st->font_weight = w;
        }
    }
    if ((v = svg_prop(n, "font-style")))
        st->font_italic = g_ascii_strcasecmp(v, "normal") != 0;
    if ((v = svg_prop(n, "paint-order")))
        st->stroke_first = g_ascii_strncasecmp(v, "stroke", 6) == 0;
    if ((v = svg_prop(n, "visibility")))
        st->hidden = (g_ascii_strcasecmp(v, "hidden") == 0 ||
                      g_ascii_strcasecmp(v, "collapse") == 0);
    if ((v = svg_prop(n, "vector-effect")))
        st->non_scaling_stroke = g_ascii_strcasecmp(v, "non-scaling-stroke") == 0;

    if (!s) return;

    svg_apply_css_paint(st, s, NS_CSS_FILL, &st->fill);
    svg_apply_css_paint(st, s, NS_CSS_STROKE, &st->stroke);
    double fs = st->font_size;
    if (s->values[NS_CSS_FILL_OPACITY])
        st->fill_opacity = CLAMP(svg_css_number(ctx, s, NS_CSS_FILL_OPACITY, fs, 1.0, 1.0), 0.0, 1.0);
    if (s->values[NS_CSS_STROKE_OPACITY])
        st->stroke_opacity = CLAMP(svg_css_number(ctx, s, NS_CSS_STROKE_OPACITY, fs, 1.0, 1.0), 0.0, 1.0);
    if (s->values[NS_CSS_STROKE_WIDTH])
        st->stroke_width = MAX(0.0, svg_css_number(ctx, s, NS_CSS_STROKE_WIDTH, fs, basis, 1.0));
    if (s->values[NS_CSS_STROKE_MITERLIMIT])
        st->miter_limit = MAX(1.0, svg_css_number(ctx, s, NS_CSS_STROKE_MITERLIMIT, fs, 1.0, 4.0));
    if (s->values[NS_CSS_STROKE_DASHOFFSET])
        st->dash_offset = svg_css_number(ctx, s, NS_CSS_STROKE_DASHOFFSET, fs, basis, 0.0);
    const char *kw;
    if ((kw = ns_style_keyword(s, NS_CSS_STROKE_LINECAP)))  st->line_cap = svg_cap_of(kw);
    if ((kw = ns_style_keyword(s, NS_CSS_STROKE_LINEJOIN))) st->line_join = svg_join_of(kw);
    if ((kw = ns_style_keyword(s, NS_CSS_FILL_RULE)))
        st->fill_rule = (g_ascii_strcasecmp(kw, "evenodd") == 0)
            ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING;
    if ((kw = ns_style_keyword(s, NS_CSS_CLIP_RULE)))
        st->clip_rule = (g_ascii_strcasecmp(kw, "evenodd") == 0)
            ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING;
    if ((kw = ns_style_keyword(s, NS_CSS_TEXT_ANCHOR)))  st->text_anchor = svg_anchor_of(kw);
    if ((kw = ns_style_keyword(s, NS_CSS_PAINT_ORDER)))
        st->stroke_first = g_ascii_strncasecmp(kw, "stroke", 6) == 0;
    if ((kw = ns_style_keyword(s, NS_CSS_STROKE_DASHARRAY)))
        svg_set_dashes(st, kw, basis, st->font_size);
    if ((kw = ns_style_keyword(s, NS_CSS_VECTOR_EFFECT)))
        st->non_scaling_stroke = g_ascii_strcasecmp(kw, "non-scaling-stroke") == 0;
    if ((kw = ns_style_keyword(s, NS_CSS_VISIBILITY)))
        st->hidden = (g_ascii_strcasecmp(kw, "hidden") == 0 ||
                      g_ascii_strcasecmp(kw, "collapse") == 0);
    if (s->values[NS_CSS_FONT_WEIGHT])
        st->font_weight = ns_css_font_weight_number(s->values[NS_CSS_FONT_WEIGHT],
                                                    st->font_weight);
    if ((kw = ns_style_keyword(s, NS_CSS_FONT_STYLE)))
        st->font_italic = g_ascii_strcasecmp(kw, "normal") != 0;
    if (s->values[NS_CSS_FONT_FAMILY] &&
        s->values[NS_CSS_FONT_FAMILY]->kind == NS_CSS_V_KEYWORD &&
        s->values[NS_CSS_FONT_FAMILY]->u.keyword) {
        g_free(st->font_family);
        st->font_family = g_strdup(s->values[NS_CSS_FONT_FAMILY]->u.keyword);
    }
}

static gboolean
svg_parse_transform(const char *s, cairo_matrix_t *out)
{
    cairo_matrix_init_identity(out);
    if (!s) return FALSE;
    gboolean any = FALSE;
    const char *p = s;
    while (*p) {
        while (*p && (svg_is_ws(*p) || *p == ',')) p++;
        if (!*p) break;
        const char *name = p;
        while (g_ascii_isalpha(*p)) p++;
        gsize nlen = (gsize)(p - name);
        if (nlen == 0) break;
        while (*p && svg_is_ws(*p)) p++;
        if (*p != '(') break;
        p++;
        double a[6] = {0};
        int n = 0;
        while (n < 6 && svg_num(&p, &a[n])) n++;
        while (*p && *p != ')') p++;
        if (*p == ')') p++;

        cairo_matrix_t m;
        cairo_matrix_init_identity(&m);
        if (nlen == 6 && g_ascii_strncasecmp(name, "matrix", 6) == 0 && n == 6) {
            cairo_matrix_init(&m, a[0], a[1], a[2], a[3], a[4], a[5]);
        } else if (nlen == 9 && g_ascii_strncasecmp(name, "translate", 9) == 0 && n >= 1) {
            cairo_matrix_init_translate(&m, a[0], n >= 2 ? a[1] : 0.0);
        } else if (nlen == 5 && g_ascii_strncasecmp(name, "scale", 5) == 0 && n >= 1) {
            cairo_matrix_init_scale(&m, a[0], n >= 2 ? a[1] : a[0]);
        } else if (nlen == 6 && g_ascii_strncasecmp(name, "rotate", 6) == 0 && n >= 1) {
            if (n >= 3) {
                cairo_matrix_init_translate(&m, a[1], a[2]);
                cairo_matrix_rotate(&m, a[0] * G_PI / 180.0);
                cairo_matrix_translate(&m, -a[1], -a[2]);
            } else {
                cairo_matrix_init_rotate(&m, a[0] * G_PI / 180.0);
            }
        } else if (nlen == 5 && g_ascii_strncasecmp(name, "skewX", 5) == 0 && n >= 1) {
            cairo_matrix_init(&m, 1, 0, tan(a[0] * G_PI / 180.0), 1, 0, 0);
        } else if (nlen == 5 && g_ascii_strncasecmp(name, "skewY", 5) == 0 && n >= 1) {
            cairo_matrix_init(&m, 1, tan(a[0] * G_PI / 180.0), 0, 1, 0, 0);
        } else {
            continue;
        }
        cairo_matrix_multiply(out, &m, out);
        any = TRUE;
    }
    return any;
}

static void
svg_apply_transform_attr(svg_ctx *ctx, const ns_node *n, const char *attr)
{
    const char *t = ns_element_get_attr(n, attr);
    if (!t) return;
    cairo_matrix_t m;
    if (!svg_parse_transform(t, &m)) return;
    cairo_transform(ctx->cr, &m);
}

static void
svg_arc_to(cairo_t *cr, double x1, double y1, double rx, double ry,
           double phi_deg, gboolean large_arc, gboolean sweep,
           double x2, double y2)
{
    if (x1 == x2 && y1 == y2) return;
    if (rx == 0.0 || ry == 0.0) { cairo_line_to(cr, x2, y2); return; }
    rx = fabs(rx);
    ry = fabs(ry);
    double phi = phi_deg * G_PI / 180.0;
    double cosp = cos(phi), sinp = sin(phi);
    double dx2 = (x1 - x2) / 2.0, dy2 = (y1 - y2) / 2.0;
    double x1p =  cosp * dx2 + sinp * dy2;
    double y1p = -sinp * dx2 + cosp * dy2;

    double lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1.0) {
        double k = sqrt(lam);
        rx *= k;
        ry *= k;
    }

    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double num = rx * rx * ry * ry - den;
    double co = 0.0;
    if (den > 0.0 && num > 0.0) co = sqrt(num / den);
    if (large_arc == sweep) co = -co;

    double cxp =  co * rx * y1p / ry;
    double cyp = -co * ry * x1p / rx;
    double cx = cosp * cxp - sinp * cyp + (x1 + x2) / 2.0;
    double cy = sinp * cxp + cosp * cyp + (y1 + y2) / 2.0;

    double t1 = atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    double t2 = atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    double dt = t2 - t1;
    if (!sweep && dt > 0.0)      dt -= 2.0 * G_PI;
    else if (sweep && dt < 0.0)  dt += 2.0 * G_PI;

    double quarters = ceil(fabs(dt) / (G_PI / 2.0) - 1e-9);
    int segs = quarters > 1.0 ? (int)MIN(quarters, 8.0) : 1;
    double seg = dt / segs;
    double alpha = 4.0 / 3.0 * tan(seg / 4.0);

    for (int i = 0; i < segs; i++) {
        double a1 = t1 + i * seg;
        double a2 = a1 + seg;
        double ca1 = cos(a1), sa1 = sin(a1);
        double ca2 = cos(a2), sa2 = sin(a2);
        double px1 = cx + rx * cosp * ca1 - ry * sinp * sa1;
        double py1 = cy + rx * sinp * ca1 + ry * cosp * sa1;
        double px2 = cx + rx * cosp * ca2 - ry * sinp * sa2;
        double py2 = cy + rx * sinp * ca2 + ry * cosp * sa2;
        double d1x = -rx * cosp * sa1 - ry * sinp * ca1;
        double d1y = -rx * sinp * sa1 + ry * cosp * ca1;
        double d2x = -rx * cosp * sa2 - ry * sinp * ca2;
        double d2y = -rx * sinp * sa2 + ry * cosp * ca2;
        cairo_curve_to(cr,
                       px1 + alpha * d1x, py1 + alpha * d1y,
                       px2 - alpha * d2x, py2 - alpha * d2y,
                       px2, py2);
    }
}

static void
svg_path_data(cairo_t *cr, const char *d)
{
    if (!d) return;
    const char *p = d;
    double cx = 0, cy = 0, sx = 0, sy = 0;
    double ctrl_x = 0, ctrl_y = 0;
    char cmd = 0, prev = 0;
    gboolean started = FALSE;

    for (;;) {
        svg_skip_sep(&p);
        if (!*p) break;
        if (g_ascii_isalpha(*p)) {
            cmd = *p;
            p++;
        } else if (cmd == 'M') {
            cmd = 'L';
        } else if (cmd == 'm') {
            cmd = 'l';
        } else if (!cmd || cmd == 'Z' || cmd == 'z') {
            break;
        }

        gboolean rel = g_ascii_islower(cmd);
        char c = g_ascii_toupper(cmd);
        double a[7];

        switch (c) {
        case 'M':
            if (!svg_num(&p, &a[0]) || !svg_num(&p, &a[1])) return;
            if (rel && started) { a[0] += cx; a[1] += cy; }
            cx = a[0]; cy = a[1];
            sx = cx; sy = cy;
            cairo_move_to(cr, cx, cy);
            started = TRUE;
            break;
        case 'L':
            if (!svg_num(&p, &a[0]) || !svg_num(&p, &a[1])) return;
            if (rel) { a[0] += cx; a[1] += cy; }
            cx = a[0]; cy = a[1];
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            else cairo_line_to(cr, cx, cy);
            break;
        case 'H':
            if (!svg_num(&p, &a[0])) return;
            if (rel) a[0] += cx;
            cx = a[0];
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            else cairo_line_to(cr, cx, cy);
            break;
        case 'V':
            if (!svg_num(&p, &a[0])) return;
            if (rel) a[0] += cy;
            cy = a[0];
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            else cairo_line_to(cr, cx, cy);
            break;
        case 'C':
            for (int i = 0; i < 6; i++) if (!svg_num(&p, &a[i])) return;
            if (rel) { a[0] += cx; a[1] += cy; a[2] += cx; a[3] += cy; a[4] += cx; a[5] += cy; }
            if (!started) { cairo_move_to(cr, a[0], a[1]); sx = a[0]; sy = a[1]; started = TRUE; }
            cairo_curve_to(cr, a[0], a[1], a[2], a[3], a[4], a[5]);
            ctrl_x = a[2]; ctrl_y = a[3];
            cx = a[4]; cy = a[5];
            break;
        case 'S': {
            for (int i = 0; i < 4; i++) if (!svg_num(&p, &a[i])) return;
            if (rel) { a[0] += cx; a[1] += cy; a[2] += cx; a[3] += cy; }
            char pc = g_ascii_toupper(prev);
            double rx = (pc == 'C' || pc == 'S') ? 2 * cx - ctrl_x : cx;
            double ry = (pc == 'C' || pc == 'S') ? 2 * cy - ctrl_y : cy;
            if (!started) { cairo_move_to(cr, rx, ry); sx = rx; sy = ry; started = TRUE; }
            cairo_curve_to(cr, rx, ry, a[0], a[1], a[2], a[3]);
            ctrl_x = a[0]; ctrl_y = a[1];
            cx = a[2]; cy = a[3];
            break;
        }
        case 'Q': {
            for (int i = 0; i < 4; i++) if (!svg_num(&p, &a[i])) return;
            if (rel) { a[0] += cx; a[1] += cy; a[2] += cx; a[3] += cy; }
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            double c1x = cx + 2.0 / 3.0 * (a[0] - cx);
            double c1y = cy + 2.0 / 3.0 * (a[1] - cy);
            double c2x = a[2] + 2.0 / 3.0 * (a[0] - a[2]);
            double c2y = a[3] + 2.0 / 3.0 * (a[1] - a[3]);
            cairo_curve_to(cr, c1x, c1y, c2x, c2y, a[2], a[3]);
            ctrl_x = a[0]; ctrl_y = a[1];
            cx = a[2]; cy = a[3];
            break;
        }
        case 'T': {
            for (int i = 0; i < 2; i++) if (!svg_num(&p, &a[i])) return;
            if (rel) { a[0] += cx; a[1] += cy; }
            char pc = g_ascii_toupper(prev);
            double qx = (pc == 'Q' || pc == 'T') ? 2 * cx - ctrl_x : cx;
            double qy = (pc == 'Q' || pc == 'T') ? 2 * cy - ctrl_y : cy;
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            double c1x = cx + 2.0 / 3.0 * (qx - cx);
            double c1y = cy + 2.0 / 3.0 * (qy - cy);
            double c2x = a[0] + 2.0 / 3.0 * (qx - a[0]);
            double c2y = a[1] + 2.0 / 3.0 * (qy - a[1]);
            cairo_curve_to(cr, c1x, c1y, c2x, c2y, a[0], a[1]);
            ctrl_x = qx; ctrl_y = qy;
            cx = a[0]; cy = a[1];
            break;
        }
        case 'A': {
            gboolean large = FALSE, sweep = FALSE;
            if (!svg_num(&p, &a[0]) || !svg_num(&p, &a[1]) || !svg_num(&p, &a[2]))
                return;
            if (!svg_flag(&p, &large) || !svg_flag(&p, &sweep)) return;
            if (!svg_num(&p, &a[5]) || !svg_num(&p, &a[6])) return;
            if (rel) { a[5] += cx; a[6] += cy; }
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            svg_arc_to(cr, cx, cy, a[0], a[1], a[2], large, sweep, a[5], a[6]);
            cx = a[5]; cy = a[6];
            break;
        }
        case 'Z':
            if (started) cairo_close_path(cr);
            cx = sx; cy = sy;
            break;
        default:
            return;
        }
        prev = cmd;
    }
}

static double
svg_geom(svg_ctx *ctx, const ns_node *n, const char *attr, ns_css_prop prop,
         double basis, double fs, double fallback, gboolean *out_auto)
{
    if (out_auto) *out_auto = FALSE;
    const ns_style *s = ctx->styles
        ? g_hash_table_lookup(ctx->styles, (gpointer)n) : NULL;
    const ns_css_value *v = s ? s->values[prop] : NULL;
    if (v) {
        if (v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
            g_ascii_strcasecmp(v->u.keyword, "auto") == 0) {
            if (out_auto) *out_auto = TRUE;
            return fallback;
        }
        if (v->kind == NS_CSS_V_LENGTH || v->kind == NS_CSS_V_CALC)
            return svg_css_number(ctx, s, prop, fs, basis, fallback);
    }
    const char *a = svg_prop(n, attr);
    if (!a) return fallback;
    if (g_ascii_strcasecmp(a, "auto") == 0) {
        if (out_auto) *out_auto = TRUE;
        return fallback;
    }
    return svg_length_str(a, basis, fs, fallback);
}

static gboolean
svg_shape_path(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    cairo_t *cr = ctx->cr;
    const char *tag = n->name;
    double fs = st->font_size;

    if (strcmp(tag, "path") == 0) {
        const char *d = ns_element_get_attr(n, "d");
        if (!d || !*d) return FALSE;
        svg_path_data(cr, d);
        return TRUE;
    }
    if (strcmp(tag, "rect") == 0) {
        double x = svg_geom(ctx, n, "x", NS_CSS_SVG_X, ctx->vw, fs, 0, NULL);
        double y = svg_geom(ctx, n, "y", NS_CSS_SVG_Y, ctx->vh, fs, 0, NULL);
        double w = svg_geom(ctx, n, "width", NS_CSS_WIDTH, ctx->vw, fs, 0, NULL);
        double h = svg_geom(ctx, n, "height", NS_CSS_HEIGHT, ctx->vh, fs, 0, NULL);
        if (w <= 0 || h <= 0) return FALSE;
        gboolean rx_auto = FALSE, ry_auto = FALSE;
        double rx = svg_geom(ctx, n, "rx", NS_CSS_RX, ctx->vw, fs, -1, &rx_auto);
        double ry = svg_geom(ctx, n, "ry", NS_CSS_RY, ctx->vh, fs, -1, &ry_auto);
        if (rx_auto) rx = -1;
        if (ry_auto) ry = -1;
        if (rx < 0 && ry < 0) { rx = ry = 0; }
        else if (rx < 0) rx = ry;
        else if (ry < 0) ry = rx;
        rx = CLAMP(rx, 0, w / 2);
        ry = CLAMP(ry, 0, h / 2);
        if (rx <= 0 || ry <= 0) {
            cairo_rectangle(cr, x, y, w, h);
            return TRUE;
        }
        cairo_save(cr);
        cairo_new_sub_path(cr);
        cairo_translate(cr, x + rx, y + ry);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1.0, G_PI, 1.5 * G_PI);
        cairo_restore(cr);
        cairo_save(cr);
        cairo_translate(cr, x + w - rx, y + ry);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1.0, 1.5 * G_PI, 2.0 * G_PI);
        cairo_restore(cr);
        cairo_save(cr);
        cairo_translate(cr, x + w - rx, y + h - ry);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1.0, 0, 0.5 * G_PI);
        cairo_restore(cr);
        cairo_save(cr);
        cairo_translate(cr, x + rx, y + h - ry);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1.0, 0.5 * G_PI, G_PI);
        cairo_restore(cr);
        cairo_close_path(cr);
        return TRUE;
    }
    if (strcmp(tag, "circle") == 0) {
        double cx = svg_geom(ctx, n, "cx", NS_CSS_CX, ctx->vw, fs, 0, NULL);
        double cy = svg_geom(ctx, n, "cy", NS_CSS_CY, ctx->vh, fs, 0, NULL);
        double r  = svg_geom(ctx, n, "r", NS_CSS_R,
                             svg_diag_basis(ctx->vw, ctx->vh), fs, 0, NULL);
        if (r <= 0) return FALSE;
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
        cairo_close_path(cr);
        return TRUE;
    }
    if (strcmp(tag, "ellipse") == 0) {
        double cx = svg_geom(ctx, n, "cx", NS_CSS_CX, ctx->vw, fs, 0, NULL);
        double cy = svg_geom(ctx, n, "cy", NS_CSS_CY, ctx->vh, fs, 0, NULL);
        gboolean rx_auto = FALSE, ry_auto = FALSE;
        double rx = svg_geom(ctx, n, "rx", NS_CSS_RX, ctx->vw, fs, -1, &rx_auto);
        double ry = svg_geom(ctx, n, "ry", NS_CSS_RY, ctx->vh, fs, -1, &ry_auto);
        if (rx_auto) rx = -1;
        if (ry_auto) ry = -1;
        if (rx < 0 && ry < 0) return FALSE;
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        if (rx <= 0 || ry <= 0) return FALSE;
        cairo_save(cr);
        cairo_new_sub_path(cr);
        cairo_translate(cr, cx, cy);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1.0, 0, 2 * G_PI);
        cairo_restore(cr);
        cairo_close_path(cr);
        return TRUE;
    }
    if (strcmp(tag, "line") == 0) {
        double x1 = svg_attr_length(n, "x1", ctx->vw, fs, 0);
        double y1 = svg_attr_length(n, "y1", ctx->vh, fs, 0);
        double x2 = svg_attr_length(n, "x2", ctx->vw, fs, 0);
        double y2 = svg_attr_length(n, "y2", ctx->vh, fs, 0);
        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
        return TRUE;
    }
    if (strcmp(tag, "polyline") == 0 || strcmp(tag, "polygon") == 0) {
        const char *pts = ns_element_get_attr(n, "points");
        if (!pts) return FALSE;
        const char *p = pts;
        double x, y;
        gboolean first = TRUE;
        while (svg_num(&p, &x) && svg_num(&p, &y)) {
            if (first) { cairo_move_to(cr, x, y); first = FALSE; }
            else cairo_line_to(cr, x, y);
        }
        if (first) return FALSE;
        if (strcmp(tag, "polygon") == 0) cairo_close_path(cr);
        return TRUE;
    }
    return FALSE;
}

static void
svg_index_ids(svg_ctx *ctx, const ns_node *n)
{
    for (const ns_node *c = n->first_child; c;
         c = ns_node_next_in_subtree(c, n, c->kind == NS_NODE_ELEMENT)) {
        if (c->kind != NS_NODE_ELEMENT) continue;
        const char *id = ns_element_get_attr(c, "id");
        if (id && *id && !g_hash_table_contains(ctx->ids, id))
            g_hash_table_insert(ctx->ids, (gpointer)id, (gpointer)c);
    }
}

static const ns_node *
svg_by_id(svg_ctx *ctx, const char *id)
{
    if (!id || !*id) return NULL;
    if (!ctx->ids) {
        ctx->ids = g_hash_table_new(g_str_hash, g_str_equal);
        const char *rid = ns_element_get_attr(ctx->root, "id");
        if (rid && *rid) g_hash_table_insert(ctx->ids, (gpointer)rid,
                                             (gpointer)ctx->root);
        svg_index_ids(ctx, ctx->root);
    }
    const ns_node *hit = g_hash_table_lookup(ctx->ids, id);
    if (hit) return hit;
    const ns_node *doc = ctx->root;
    while (doc && doc->parent) doc = doc->parent;
    if (!doc || doc == ctx->root || doc->kind != NS_NODE_DOCUMENT) return NULL;
    return ns_node_find_by_id(doc, id);
}

static const char *
svg_href(const ns_node *n)
{
    const char *h = ns_element_get_attr(n, "href");
    if (!h) h = ns_element_get_attr(n, "xlink:href");
    return h;
}

static const ns_node *
svg_href_target(svg_ctx *ctx, const ns_node *n)
{
    const char *h = svg_href(n);
    if (!h) return NULL;
    while (*h && svg_is_ws(*h)) h++;
    if (*h != '#') return ns_svg_external_target(n, h);
    return svg_by_id(ctx, h + 1);
}

static const ns_node *
svg_attr_owner(svg_ctx *ctx, const ns_node *n, const char *name)
{
    const ns_node *cur = n;
    for (int i = 0; cur && i < NS_SVG_MAX_HREF_CHAIN; i++) {
        if (ns_element_get_attr(cur, name)) return cur;
        cur = svg_href_target(ctx, cur);
    }
    return NULL;
}

static const char *
svg_inherited_attr(svg_ctx *ctx, const ns_node *n, const char *name)
{
    const ns_node *owner = svg_attr_owner(ctx, n, name);
    return owner ? ns_element_get_attr(owner, name) : NULL;
}

static const ns_node *
svg_stops_owner(svg_ctx *ctx, const ns_node *n)
{
    const ns_node *cur = n;
    for (int i = 0; cur && i < NS_SVG_MAX_HREF_CHAIN; i++) {
        for (const ns_node *c = cur->first_child; c; c = c->next_sibling)
            if (c->kind == NS_NODE_ELEMENT && c->name &&
                strcmp(c->name, "stop") == 0)
                return cur;
        cur = svg_href_target(ctx, cur);
    }
    return NULL;
}

static void
svg_add_stops(svg_ctx *ctx, cairo_pattern_t *pat, const ns_node *owner,
              const svg_state *st, double alpha)
{
    double last = 0.0;
    for (const ns_node *c = owner->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || !c->name || strcmp(c->name, "stop") != 0)
            continue;
        const char *os = svg_prop(c, "offset");
        double off = os ? svg_length_str(os, 1.0, st->font_size, 0.0) : 0.0;
        if (os && strchr(os, '%') == NULL && off > 1.0) off = 1.0;
        off = CLAMP(off, 0.0, 1.0);
        if (off < last) off = last;
        last = off;

        double r = 0, g = 0, b = 0, a = 1.0;
        const ns_style *cs = ctx->styles
            ? g_hash_table_lookup(ctx->styles, (gpointer)c) : NULL;
        const ns_css_value *sv = cs ? cs->values[NS_CSS_STOP_COLOR] : NULL;
        gboolean got = FALSE;
        if (sv && sv->kind == NS_CSS_V_COLOR) {
            r = sv->u.color.r / 255.0; g = sv->u.color.g / 255.0;
            b = sv->u.color.b / 255.0; a = sv->u.color.a / 255.0;
            got = TRUE;
        }
        if (!got) {
            const char *sc = svg_prop(c, "stop-color");
            guint8 cr8, cg8, cb8, ca8;
            if (sc && g_ascii_strcasecmp(sc, "currentcolor") == 0) {
                r = st->color_r; g = st->color_g; b = st->color_b; a = st->color_a;
            } else if (sc && ns_css_parse_color(sc, &cr8, &cg8, &cb8, &ca8)) {
                r = cr8 / 255.0; g = cg8 / 255.0; b = cb8 / 255.0; a = ca8 / 255.0;
            }
        }
        double so = 1.0;
        const ns_css_value *ov = cs ? cs->values[NS_CSS_STOP_OPACITY] : NULL;
        if (ov) so = CLAMP(svg_css_number(ctx, cs, NS_CSS_STOP_OPACITY, st->font_size, 1.0, 1.0), 0.0, 1.0);
        else {
            const char *sos = svg_prop(c, "stop-opacity");
            if (sos) so = CLAMP(svg_length_str(sos, 1.0, st->font_size, 1.0), 0.0, 1.0);
        }
        cairo_pattern_add_color_stop_rgba(pat, off, r, g, b, a * so * alpha);
    }
}

typedef struct {
    gboolean none;
    gboolean slice;
    double   ax, ay;
} svg_aspect;

static void
svg_aspect_align(const char *token, svg_aspect *out)
{
    if (strstr(token, "xMin")) out->ax = 0.0;
    else if (strstr(token, "xMax")) out->ax = 1.0;
    if (strstr(token, "YMin")) out->ay = 0.0;
    else if (strstr(token, "YMax")) out->ay = 1.0;
}

static void
svg_aspect_parse(const char *par, svg_aspect *out)
{
    out->none = FALSE;
    out->slice = FALSE;
    out->ax = 0.5;
    out->ay = 0.5;
    if (!par) return;
    char **parts = g_strsplit_set(par, " \t\r\n", -1);
    for (int i = 0; parts[i]; i++) {
        if (!*parts[i]) continue;
        if (g_ascii_strcasecmp(parts[i], "none") == 0) out->none = TRUE;
        else if (g_ascii_strcasecmp(parts[i], "slice") == 0) out->slice = TRUE;
        else if (g_ascii_strncasecmp(parts[i], "x", 1) == 0)
            svg_aspect_align(parts[i], out);
    }
    g_strfreev(parts);
}

static void
svg_fit_matrix(const svg_rect *view, const char *par, double vw, double vh,
               cairo_matrix_t *out)
{
    cairo_matrix_init_identity(out);
    if (view->w <= 0 || view->h <= 0 || vw <= 0 || vh <= 0) return;
    svg_aspect aspect;
    svg_aspect_parse(par, &aspect);
    double sx = vw / view->w, sy = vh / view->h;
    if (!aspect.none) {
        double s = aspect.slice ? MAX(sx, sy) : MIN(sx, sy);
        sx = sy = s;
    }
    double tx = (vw - view->w * sx) * aspect.ax;
    double ty = (vh - view->h * sy) * aspect.ay;
    cairo_matrix_init_translate(out, tx, ty);
    cairo_matrix_scale(out, sx, sy);
    cairo_matrix_translate(out, -view->x, -view->y);
}

static void
svg_viewbox_matrix(const ns_node *n, double vw, double vh, cairo_matrix_t *out)
{
    cairo_matrix_init_identity(out);
    const char *vb = ns_element_get_attr(n, "viewBox");
    if (!vb) return;
    const char *p = vb;
    svg_rect view;
    if (!svg_num(&p, &view.x) || !svg_num(&p, &view.y) ||
        !svg_num(&p, &view.w) || !svg_num(&p, &view.h)) return;
    svg_fit_matrix(&view, ns_element_get_attr(n, "preserveAspectRatio"),
                   vw, vh, out);
}

static void
svg_viewbox_size(const ns_node *n, double w, double h, double *vw, double *vh)
{
    *vw = w;
    *vh = h;
    const char *p = ns_element_get_attr(n, "viewBox");
    double x, y, box_w, box_h;
    if (p && svg_num(&p, &x) && svg_num(&p, &y) &&
        svg_num(&p, &box_w) && svg_num(&p, &box_h) && box_w > 0 && box_h > 0) {
        *vw = box_w;
        *vh = box_h;
    }
}

static cairo_extend_t
svg_extend_of(const char *s)
{
    if (!s) return CAIRO_EXTEND_PAD;
    if (g_ascii_strcasecmp(s, "reflect") == 0) return CAIRO_EXTEND_REFLECT;
    if (g_ascii_strcasecmp(s, "repeat") == 0)  return CAIRO_EXTEND_REPEAT;
    return CAIRO_EXTEND_PAD;
}

static cairo_pattern_t *
svg_gradient_pattern(svg_ctx *ctx, const ns_node *g, const svg_state *st,
                     double alpha, double bx, double by, double bw, double bh)
{
    gboolean linear = strcmp(g->name, "linearGradient") == 0;
    const char *units = svg_inherited_attr(ctx, g, "gradientUnits");
    gboolean obb = !units || g_ascii_strcasecmp(units, "userSpaceOnUse") != 0;
    double bw_ref = obb ? 1.0 : ctx->vw;
    double bh_ref = obb ? 1.0 : ctx->vh;
    double dref   = obb ? 1.0 : svg_diag_basis(ctx->vw, ctx->vh);

    cairo_pattern_t *pat = NULL;
    if (linear) {
        const char *s;
        double x1 = (s = svg_inherited_attr(ctx, g, "x1"))
            ? svg_length_str(s, bw_ref, st->font_size, 0) : 0.0;
        double y1 = (s = svg_inherited_attr(ctx, g, "y1"))
            ? svg_length_str(s, bh_ref, st->font_size, 0) : 0.0;
        double x2 = (s = svg_inherited_attr(ctx, g, "x2"))
            ? svg_length_str(s, bw_ref, st->font_size, bw_ref) : bw_ref;
        double y2 = (s = svg_inherited_attr(ctx, g, "y2"))
            ? svg_length_str(s, bh_ref, st->font_size, 0) : 0.0;
        pat = cairo_pattern_create_linear(x1, y1, x2, y2);
    } else {
        const char *s;
        double cx = (s = svg_inherited_attr(ctx, g, "cx"))
            ? svg_length_str(s, bw_ref, st->font_size, bw_ref * 0.5) : bw_ref * 0.5;
        double cy = (s = svg_inherited_attr(ctx, g, "cy"))
            ? svg_length_str(s, bh_ref, st->font_size, bh_ref * 0.5) : bh_ref * 0.5;
        double r  = (s = svg_inherited_attr(ctx, g, "r"))
            ? svg_length_str(s, dref, st->font_size, dref * 0.5) : dref * 0.5;
        double fx = (s = svg_inherited_attr(ctx, g, "fx"))
            ? svg_length_str(s, bw_ref, st->font_size, cx) : cx;
        double fy = (s = svg_inherited_attr(ctx, g, "fy"))
            ? svg_length_str(s, bh_ref, st->font_size, cy) : cy;
        if (r <= 0) return NULL;
        double dx = fx - cx, dy = fy - cy;
        double dist = sqrt(dx * dx + dy * dy);
        if (dist > r * 0.999) {
            double k = r * 0.999 / dist;
            fx = cx + dx * k;
            fy = cy + dy * k;
        }
        pat = cairo_pattern_create_radial(fx, fy, 0.0, cx, cy, r);
    }
    if (!pat) return NULL;

    const ns_node *owner = svg_stops_owner(ctx, g);
    if (!owner) {
        cairo_pattern_destroy(pat);
        return NULL;
    }
    svg_add_stops(ctx, pat, owner, st, alpha);
    cairo_pattern_set_extend(pat, svg_extend_of(
        svg_inherited_attr(ctx, g, "spreadMethod")));

    cairo_matrix_t m;
    cairo_matrix_init_identity(&m);
    if (obb) {
        if (bw <= 0 || bh <= 0) { cairo_pattern_destroy(pat); return NULL; }
        cairo_matrix_init_translate(&m, bx, by);
        cairo_matrix_scale(&m, bw, bh);
    }
    const char *gt = svg_inherited_attr(ctx, g, "gradientTransform");
    if (gt) {
        cairo_matrix_t t;
        if (svg_parse_transform(gt, &t)) cairo_matrix_multiply(&m, &t, &m);
    }
    if (cairo_matrix_invert(&m) != CAIRO_STATUS_SUCCESS) {
        cairo_pattern_destroy(pat);
        return NULL;
    }
    cairo_pattern_set_matrix(pat, &m);
    return pat;
}

static gboolean
svg_units_obb(svg_ctx *ctx, const ns_node *n, const char *attr, gboolean dflt)
{
    const char *units = svg_inherited_attr(ctx, n, attr);
    if (!units) return dflt;
    if (g_ascii_strcasecmp(units, "userSpaceOnUse") == 0) return FALSE;
    if (g_ascii_strcasecmp(units, "objectBoundingBox") == 0) return TRUE;
    return dflt;
}

static double
svg_pattern_length(svg_ctx *ctx, const ns_node *p, const char *attr,
                   double basis, double font_size)
{
    const char *v = svg_inherited_attr(ctx, p, attr);
    return v ? svg_length_str(v, basis, font_size, 0) : 0;
}

static gboolean
svg_pattern_tile(svg_ctx *ctx, const ns_node *p, const svg_state *st,
                 const svg_rect *bbox, svg_rect *tile)
{
    gboolean obb = svg_units_obb(ctx, p, "patternUnits", TRUE);
    double bw = obb ? 1.0 : ctx->vw;
    double bh = obb ? 1.0 : ctx->vh;
    tile->x = svg_pattern_length(ctx, p, "x", bw, st->font_size);
    tile->y = svg_pattern_length(ctx, p, "y", bh, st->font_size);
    tile->w = svg_pattern_length(ctx, p, "width", bw, st->font_size);
    tile->h = svg_pattern_length(ctx, p, "height", bh, st->font_size);
    if (obb) {
        tile->x = bbox->x + tile->x * bbox->w;
        tile->y = bbox->y + tile->y * bbox->h;
        tile->w *= bbox->w;
        tile->h *= bbox->h;
    }
    return tile->w > 0 && tile->h > 0 && isfinite(tile->w) && isfinite(tile->h);
}

static void
svg_pattern_to_user(svg_ctx *ctx, const ns_node *p, const svg_rect *tile,
                    cairo_matrix_t *out)
{
    cairo_matrix_init_translate(out, tile->x, tile->y);
    const char *pt = svg_inherited_attr(ctx, p, "patternTransform");
    cairo_matrix_t t;
    if (pt && svg_parse_transform(pt, &t)) cairo_matrix_multiply(out, out, &t);
}

static void
svg_pattern_content_matrix(svg_ctx *ctx, const ns_node *p,
                           const svg_rect *bbox, const svg_rect *tile,
                           cairo_matrix_t *out)
{
    const ns_node *vb = svg_attr_owner(ctx, p, "viewBox");
    if (vb) {
        svg_viewbox_matrix(vb, tile->w, tile->h, out);
        return;
    }
    cairo_matrix_init_identity(out);
    if (svg_units_obb(ctx, p, "patternContentUnits", FALSE))
        cairo_matrix_scale(out, bbox->w, bbox->h);
}

static gboolean
svg_tile_pixels(cairo_t *cr, const cairo_matrix_t *to_user,
                const svg_rect *tile, int *pw, int *ph)
{
    cairo_matrix_t dev;
    cairo_get_matrix(cr, &dev);
    cairo_matrix_multiply(&dev, to_user, &dev);
    double w = ceil(tile->w * hypot(dev.xx, dev.yx) - 0.001);
    double h = ceil(tile->h * hypot(dev.xy, dev.yy) - 0.001);
    if (!isfinite(w) || !isfinite(h)) return FALSE;
    w = CLAMP(w, 1.0, (double)NS_SVG_MAX_DIM_PX);
    h = CLAMP(h, 1.0, (double)NS_SVG_MAX_DIM_PX);
    double k = MIN(1.0, sqrt((double)NS_SVG_MAX_TILE_PIXELS / (w * h)));
    *pw = (int)CLAMP(ceil(w * k), 1.0, (double)NS_SVG_MAX_DIM_PX);
    *ph = (int)CLAMP(ceil(h * k), 1.0, (double)NS_SVG_MAX_DIM_PX);
    return TRUE;
}

static const ns_node *
svg_pattern_content(svg_ctx *ctx, const ns_node *p)
{
    const ns_node *cur = p;
    for (int i = 0; cur && i < NS_SVG_MAX_HREF_CHAIN; i++) {
        for (const ns_node *c = cur->first_child; c; c = c->next_sibling)
            if (c->kind == NS_NODE_ELEMENT) return cur;
        cur = svg_href_target(ctx, cur);
    }
    return NULL;
}

static void
svg_state_for_element(svg_ctx *ctx, const ns_node *el, svg_state *st)
{
    GPtrArray *chain = g_ptr_array_new();
    for (const ns_node *n = el; n && n->kind == NS_NODE_ELEMENT &&
         chain->len < NS_DOM_MAX_DEPTH; n = n->parent) {
        g_ptr_array_add(chain, (gpointer)n);
        if (n == ctx->root) break;
    }
    svg_state_init_inherited(st, ctx->inherited);
    for (guint i = chain->len; i > 0; i--)
        svg_state_apply_node(ctx, st, g_ptr_array_index(chain, i - 1));
    g_ptr_array_free(chain, TRUE);
}

typedef struct {
    const ns_node   *pattern;
    int              pw, ph;
    double           opacity;
    double           vw, vh;
    cairo_matrix_t   m;
    cairo_surface_t *surface;
} svg_tile;

static void
svg_tile_free(gpointer data)
{
    svg_tile *t = data;
    if (t->surface) cairo_surface_destroy(t->surface);
    g_free(t);
}

static gboolean
svg_tile_same(const svg_tile *a, const svg_tile *b)
{
    return a->pattern == b->pattern && a->pw == b->pw && a->ph == b->ph &&
           memcmp(&a->opacity, &b->opacity, sizeof a->opacity) == 0 &&
           memcmp(&a->vw, &b->vw, sizeof a->vw) == 0 &&
           memcmp(&a->vh, &b->vh, sizeof a->vh) == 0 &&
           memcmp(&a->m, &b->m, sizeof a->m) == 0;
}

static cairo_surface_t *
svg_tile_lookup(const svg_ctx *ctx, const svg_tile *key)
{
    for (guint i = 0; ctx->tiles && i < ctx->tiles->len; i++) {
        const svg_tile *t = g_ptr_array_index(ctx->tiles, i);
        if (svg_tile_same(t, key)) return cairo_surface_reference(t->surface);
    }
    return NULL;
}

static void
svg_tile_store(svg_ctx *ctx, const svg_tile *key, cairo_surface_t *surface)
{
    gsize bytes = (gsize)cairo_image_surface_get_stride(surface) *
                  (gsize)cairo_image_surface_get_height(surface);
    if (ctx->tile_bytes + bytes > NS_SVG_TILE_CACHE_SIZE) return;
    if (!ctx->tiles) ctx->tiles = g_ptr_array_new_with_free_func(svg_tile_free);
    if (ctx->tiles->len >= NS_SVG_TILE_CACHE_LEN) return;
    svg_tile *t = g_new(svg_tile, 1);
    *t = *key;
    t->surface = cairo_surface_reference(surface);
    g_ptr_array_add(ctx->tiles, t);
    ctx->tile_bytes += bytes;
}

static void
svg_element_viewport(const svg_ctx *ctx, const ns_node *el, double *vw,
                     double *vh)
{
    const ns_node *svg = el->parent;
    while (svg && svg != ctx->root && !ns_node_is_element_named(svg, "svg"))
        svg = svg->parent;
    svg_viewbox_size(ctx->root, ctx->icb_w, ctx->icb_h, vw, vh);
    if (!svg || svg == ctx->root) return;
    double w = svg_attr_length(svg, "width", *vw, 16.0, *vw);
    double h = svg_attr_length(svg, "height", *vh, 16.0, *vh);
    svg_viewbox_size(svg, w, h, vw, vh);
}

static void
svg_pattern_render_content(svg_ctx *ctx, cairo_t *cr, const ns_node *content,
                           const svg_tile *key)
{
    gboolean outermost = ctx->n_patterns == 1;
    int document_nodes = ctx->nodes;
    if (outermost) ctx->nodes = ctx->pattern_nodes;
    svg_state st;
    svg_state_for_element(ctx, content, &st);
    cairo_t *outer = ctx->cr;
    double outer_vw = ctx->vw, outer_vh = ctx->vh;
    ctx->cr = cr;
    ctx->vw = key->vw;
    ctx->vh = key->vh;
    for (const ns_node *c = content->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_ELEMENT) svg_render_node(ctx, c, &st);
    ctx->cr = outer;
    ctx->vw = outer_vw;
    ctx->vh = outer_vh;
    svg_state_clear(&st);
    if (!outermost) return;
    ctx->pattern_nodes = ctx->nodes;
    ctx->nodes = document_nodes;
}

static cairo_surface_t *
svg_pattern_tile_surface(svg_ctx *ctx, const svg_tile *key)
{
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       key->pw, key->ph);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    const ns_node *content = svg_pattern_content(ctx, key->pattern);
    if (!content) return surf;
    cairo_t *cr = cairo_create(surf);
    cairo_set_matrix(cr, &key->m);
    gboolean faded = key->opacity < 1.0;
    if (faded) cairo_push_group(cr);
    svg_pattern_render_content(ctx, cr, content, key);
    if (faded) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, key->opacity);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) return surf;
    cairo_surface_destroy(surf);
    return NULL;
}

static gboolean
svg_pattern_blocked(const svg_ctx *ctx, const ns_node *p)
{
    for (int i = 0; i < ctx->n_patterns; i++)
        if (ctx->patterns[i] == p) return TRUE;
    return ctx->n_patterns >= NS_SVG_MAX_PATTERNS;
}

static cairo_surface_t *
svg_pattern_tile_cached(svg_ctx *ctx, const svg_tile *key)
{
    cairo_surface_t *surf = svg_tile_lookup(ctx, key);
    if (surf) return surf;
    ctx->patterns[ctx->n_patterns++] = key->pattern;
    surf = svg_pattern_tile_surface(ctx, key);
    ctx->n_patterns--;
    if (surf) svg_tile_store(ctx, key, surf);
    return surf;
}

static void
svg_pattern_tile_key(svg_ctx *ctx, const ns_node *p, const svg_rect *bbox,
                     const svg_rect *tile, svg_tile *key)
{
    cairo_matrix_t content, scale;
    svg_pattern_content_matrix(ctx, p, bbox, tile, &content);
    cairo_matrix_init_scale(&scale, key->pw / tile->w, key->ph / tile->h);
    cairo_matrix_multiply(&key->m, &content, &scale);
    const ns_node *own = svg_pattern_content(ctx, p);
    svg_element_viewport(ctx, own ? own : p, &key->vw, &key->vh);
    key->pattern = p;
    key->surface = NULL;
}

static void
svg_surface_sums(cairo_surface_t *surf, guint64 sums[4])
{
    const guchar *data = cairo_image_surface_get_data(surf);
    int w = cairo_image_surface_get_width(surf);
    int h = cairo_image_surface_get_height(surf);
    gsize stride = (gsize)cairo_image_surface_get_stride(surf);
    for (int y = 0; data && y < h; y++) {
        for (int x = 0; x < w; x++) {
            guint32 px;
            memcpy(&px, data + (gsize)y * stride + (gsize)x * 4, sizeof px);
            for (int c = 0; c < 4; c++) sums[c] += (px >> (24 - 8 * c)) & 0xff;
        }
    }
}

static cairo_pattern_t *
svg_mean_color_pattern(cairo_surface_t *surf)
{
    guint64 sums[4] = { 0, 0, 0, 0 };
    cairo_surface_flush(surf);
    svg_surface_sums(surf, sums);
    double n = 255.0 * MAX(1, cairo_image_surface_get_width(surf)) *
               MAX(1, cairo_image_surface_get_height(surf));
    double a = sums[0] / n;
    if (!(a > 0)) return cairo_pattern_create_rgba(0, 0, 0, 0);
    return cairo_pattern_create_rgba(MIN(1.0, sums[1] / n / a),
                                     MIN(1.0, sums[2] / n / a),
                                     MIN(1.0, sums[3] / n / a), a);
}

static gboolean
svg_source_scale_ok(cairo_t *cr, const cairo_matrix_t *user_to_source)
{
    cairo_matrix_t device_to_source;
    cairo_get_matrix(cr, &device_to_source);
    if (cairo_matrix_invert(&device_to_source) != CAIRO_STATUS_SUCCESS)
        return FALSE;
    cairo_matrix_multiply(&device_to_source, &device_to_source, user_to_source);
    double k = MAX(MAX(fabs(device_to_source.xx), fabs(device_to_source.xy)),
                   MAX(fabs(device_to_source.yx), fabs(device_to_source.yy)));
    return isfinite(k) && k <= NS_SVG_MAX_SOURCE_SCALE;
}

static cairo_pattern_t *
svg_surface_pattern(cairo_t *cr, cairo_surface_t *surf,
                    const cairo_matrix_t *user_to_source, cairo_extend_t extend)
{
    if (!svg_source_scale_ok(cr, user_to_source))
        return svg_mean_color_pattern(surf);
    cairo_pattern_t *pat = cairo_pattern_create_for_surface(surf);
    cairo_pattern_set_extend(pat, extend);
    cairo_pattern_set_matrix(pat, user_to_source);
    if (cairo_pattern_status(pat) == CAIRO_STATUS_SUCCESS) return pat;
    cairo_pattern_destroy(pat);
    return NULL;
}

static cairo_pattern_t *
svg_pattern_paint(svg_ctx *ctx, const ns_node *p, const svg_state *st,
                  double opacity, const svg_rect *bbox)
{
    svg_rect tile;
    if (svg_pattern_blocked(ctx, p) ||
        !svg_pattern_tile(ctx, p, st, bbox, &tile)) return NULL;
    cairo_matrix_t to_user;
    svg_pattern_to_user(ctx, p, &tile, &to_user);
    svg_tile key = { .opacity = opacity };
    if (!svg_tile_pixels(ctx->cr, &to_user, &tile, &key.pw, &key.ph))
        return NULL;
    cairo_matrix_t to_tile;
    cairo_matrix_init_scale(&to_tile, tile.w / key.pw, tile.h / key.ph);
    cairo_matrix_multiply(&to_tile, &to_tile, &to_user);
    if (cairo_matrix_invert(&to_tile) != CAIRO_STATUS_SUCCESS) return NULL;
    svg_pattern_tile_key(ctx, p, bbox, &tile, &key);
    cairo_surface_t *surf = svg_pattern_tile_cached(ctx, &key);
    if (!surf) return NULL;
    cairo_pattern_t *pat = svg_surface_pattern(ctx->cr, surf, &to_tile,
                                               CAIRO_EXTEND_REPEAT);
    cairo_surface_destroy(surf);
    return pat;
}

static cairo_pattern_t *
svg_paint_server(svg_ctx *ctx, const ns_node *server, const svg_state *st,
                 double opacity)
{
    if (!server || !server->name) return NULL;
    double x1, y1, x2, y2;
    cairo_path_extents(ctx->cr, &x1, &y1, &x2, &y2);
    svg_rect bbox = { x1, y1, x2 - x1, y2 - y1 };
    if (strcmp(server->name, "pattern") == 0)
        return svg_pattern_paint(ctx, server, st, opacity, &bbox);
    if (strcmp(server->name, "linearGradient") == 0 ||
        strcmp(server->name, "radialGradient") == 0)
        return svg_gradient_pattern(ctx, server, st, opacity,
                                    bbox.x, bbox.y, bbox.w, bbox.h);
    return NULL;
}

static gboolean
svg_set_paint(svg_ctx *ctx, const svg_paint *paint, const svg_state *st,
              double opacity)
{
    cairo_t *cr = ctx->cr;
    if (paint->kind == SVG_PAINT_NONE) return FALSE;
    if (paint->kind == SVG_PAINT_COLOR) {
        if (paint->a * opacity <= 0.0) return FALSE;
        cairo_set_source_rgba(cr, paint->r, paint->g, paint->b,
                              paint->a * opacity);
        return TRUE;
    }

    cairo_pattern_t *pat = svg_paint_server(ctx, svg_by_id(ctx, paint->ref),
                                            st, opacity);
    if (pat) {
        cairo_set_source(cr, pat);
        cairo_pattern_destroy(pat);
        return TRUE;
    }
    if (paint->have_fallback) {
        if (paint->fa * opacity <= 0.0) return FALSE;
        cairo_set_source_rgba(cr, paint->fr, paint->fg, paint->fb,
                              paint->fa * opacity);
        return TRUE;
    }
    return FALSE;
}

static void
svg_apply_stroke_params(svg_ctx *ctx, const svg_state *st)
{
    cairo_t *cr = ctx->cr;
    cairo_set_line_width(cr, st->stroke_width);
    cairo_set_line_cap(cr, st->line_cap);
    cairo_set_line_join(cr, st->line_join);
    cairo_set_miter_limit(cr, st->miter_limit);
    if (st->dashes && st->dashes->len > 0)
        cairo_set_dash(cr, &g_array_index(st->dashes, double, 0),
                       (int)st->dashes->len, st->dash_offset);
    else
        cairo_set_dash(cr, NULL, 0, 0.0);
}

static void
svg_paint_current_path(svg_ctx *ctx, const svg_state *st)
{
    cairo_t *cr = ctx->cr;
    gboolean do_stroke = st->stroke.kind != SVG_PAINT_NONE && st->stroke_width > 0;

    for (int pass = 0; pass < 2; pass++) {
        gboolean stroking = st->stroke_first ? (pass == 0) : (pass == 1);
        if (stroking) {
            if (!do_stroke) continue;
            if (!svg_set_paint(ctx, &st->stroke, st, st->stroke_opacity)) continue;
            if (st->non_scaling_stroke) {
                cairo_matrix_t ctm;
                cairo_get_matrix(cr, &ctm);
                cairo_identity_matrix(cr);
                svg_apply_stroke_params(ctx, st);
                cairo_stroke_preserve(cr);
                cairo_set_matrix(cr, &ctm);
            } else {
                svg_apply_stroke_params(ctx, st);
                cairo_stroke_preserve(cr);
            }
        } else {
            cairo_set_fill_rule(cr, st->fill_rule);
            if (!svg_set_paint(ctx, &st->fill, st, st->fill_opacity)) continue;
            cairo_fill_preserve(cr);
        }
    }
    cairo_new_path(cr);
}

static void
svg_clip_path_children(svg_ctx *ctx, const ns_node *clip, const svg_state *st)
{
    for (const ns_node *c = clip->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "use") == 0) {
            const ns_node *t = svg_href_target(ctx, c);
            if (t && ctx->depth < NS_SVG_MAX_DEPTH) {
                ctx->depth++;
                svg_clip_path_children(ctx, c, st);
                cairo_save(ctx->cr);
                svg_apply_transform_attr(ctx, c, "transform");
                if (svg_shape_path(ctx, t, st)) { }
                cairo_restore(ctx->cr);
                ctx->depth--;
            }
            continue;
        }
        cairo_save(ctx->cr);
        svg_apply_transform_attr(ctx, c, "transform");
        svg_shape_path(ctx, c, st);
        cairo_restore(ctx->cr);
    }
}

static cairo_surface_t *
svg_mask_surface(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    const char *mv = svg_prop(n, "mask");
    if (!mv) return NULL;
    char *id = svg_url_id(mv, NULL);
    if (!id) return NULL;
    const ns_node *mask = svg_by_id(ctx, id);
    g_free(id);
    if (!mask || !mask->name || strcmp(mask->name, "mask") != 0) return NULL;
    if (ctx->depth >= NS_SVG_MAX_DEPTH) return NULL;

    cairo_surface_t *target = cairo_get_target(ctx->cr);
    double ox = 0, oy = 0;
    cairo_surface_get_device_offset(target, &ox, &oy);
    double cx1, cy1, cx2, cy2;
    cairo_clip_extents(ctx->cr, &cx1, &cy1, &cx2, &cy2);
    cairo_matrix_t ctm;
    cairo_get_matrix(ctx->cr, &ctm);
    cairo_matrix_transform_point(&ctm, &cx1, &cy1);
    cairo_matrix_transform_point(&ctm, &cx2, &cy2);
    int w = (int)ceil(MAX(cx1, cx2) + ox);
    int h = (int)ceil(MAX(cy1, cy2) + oy);
    if (w <= 0 || h <= 0 || (double)w * h > (double)NS_SVG_MAX_PIXELS) return NULL;

    cairo_surface_t *rgb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(rgb) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(rgb);
        return NULL;
    }
    cairo_t *mcr = cairo_create(rgb);
    cairo_set_matrix(mcr, &ctm);
    const char *content_units = ns_element_get_attr(mask, "maskContentUnits");
    if (content_units &&
        g_ascii_strcasecmp(content_units, "objectBoundingBox") == 0) {
        double bx = svg_attr_length(n, "x", ctx->vw, st->font_size, 0);
        double by = svg_attr_length(n, "y", ctx->vh, st->font_size, 0);
        double bw = svg_attr_length(n, "width", ctx->vw, st->font_size, ctx->vw);
        double bh = svg_attr_length(n, "height", ctx->vh, st->font_size, ctx->vh);
        cairo_translate(mcr, bx, by);
        cairo_scale(mcr, bw > 0 ? bw : 1, bh > 0 ? bh : 1);
    }

    cairo_t *saved = ctx->cr;
    ctx->cr = mcr;
    ctx->depth++;
    svg_state ms;
    svg_state_copy(&ms, st);
    ms.instanced = FALSE;
    svg_paint_clear(&ms.fill);
    ms.fill.kind = SVG_PAINT_COLOR;
    ms.fill.r = ms.fill.g = ms.fill.b = ms.fill.a = 1.0;
    for (const ns_node *c = mask->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_ELEMENT) svg_render_node(ctx, c, &ms);
    svg_state_clear(&ms);
    ctx->depth--;
    ctx->cr = saved;
    cairo_destroy(mcr);
    cairo_surface_flush(rgb);

    cairo_surface_t *a8 = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
    if (cairo_surface_status(a8) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(rgb);
        cairo_surface_destroy(a8);
        return NULL;
    }
    const unsigned char *src = cairo_image_surface_get_data(rgb);
    unsigned char *dst = cairo_image_surface_get_data(a8);
    int sstride = cairo_image_surface_get_stride(rgb);
    int dstride = cairo_image_surface_get_stride(a8);
    for (int y = 0; y < h; y++) {
        const guint32 *srow = (const guint32 *)(gconstpointer)(src + (gsize)y * sstride);
        unsigned char *drow = dst + (gsize)y * dstride;
        for (int x = 0; x < w; x++) {
            guint32 px = srow[x];
            double r = ((px >> 16) & 0xff) / 255.0;
            double g = ((px >> 8) & 0xff) / 255.0;
            double b = (px & 0xff) / 255.0;
            double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            drow[x] = (unsigned char)CLAMP(lum * 255.0, 0.0, 255.0);
        }
    }
    cairo_surface_mark_dirty(a8);
    cairo_surface_destroy(rgb);
    return a8;
}

static double
svg_filter_blur(svg_ctx *ctx, const ns_node *n)
{
    const char *fv = svg_prop(n, "filter");
    if (!fv) return 0;
    char *id = svg_url_id(fv, NULL);
    if (!id) return 0;
    const ns_node *f = svg_by_id(ctx, id);
    g_free(id);
    if (!f || !f->name || strcmp(f->name, "filter") != 0) return 0;
    double variance = 0;
    for (const ns_node *c = f->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "feGaussianBlur") != 0) return 0;
        const char *in = ns_element_get_attr(c, "in");
        if (in && *in && strcmp(in, "SourceGraphic") != 0) return 0;
        const char *p = ns_element_get_attr(c, "stdDeviation");
        double sx = 0, sy;
        if (!p || !svg_num(&p, &sx)) continue;
        if (svg_num(&p, &sy)) sx = MAX(sx, sy);
        if (sx > 0) variance += sx * sx;
    }
    return sqrt(variance);
}

static void
svg_blur_source(cairo_t *cr, double sigma)
{
    cairo_surface_t *group = NULL;
    if (cairo_pattern_get_surface(cairo_get_source(cr), &group) !=
            CAIRO_STATUS_SUCCESS || !group)
        return;
    cairo_matrix_t m;
    cairo_get_matrix(cr, &m);
    double dsx = 1, dsy = 1;
    cairo_surface_get_device_scale(group, &dsx, &dsy);
    double scale = sqrt(fabs(m.xx * m.yy - m.xy * m.yx)) * MAX(dsx, dsy);
    cairo_surface_t *img = cairo_surface_map_to_image(group, NULL);
    if (cairo_surface_status(img) == CAIRO_STATUS_SUCCESS &&
        cairo_image_surface_get_format(img) == CAIRO_FORMAT_ARGB32) {
        cairo_surface_flush(img);
        ns_paint_blur_argb(cairo_image_surface_get_data(img),
                           cairo_image_surface_get_stride(img),
                           cairo_image_surface_get_width(img),
                           cairo_image_surface_get_height(img),
                           sigma * scale);
        cairo_surface_mark_dirty(img);
    }
    cairo_surface_unmap_image(group, img);
}

static void
svg_apply_clip(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    const char *cp = svg_prop(n, "clip-path");
    if (!cp) return;
    char *id = svg_url_id(cp, NULL);
    if (!id) return;
    const ns_node *clip = svg_by_id(ctx, id);
    g_free(id);
    if (!clip || !clip->name || strcmp(clip->name, "clipPath") != 0) return;
    if (ctx->depth >= NS_SVG_MAX_DEPTH) return;

    ctx->depth++;
    cairo_save(ctx->cr);
    const char *units = ns_element_get_attr(clip, "clipPathUnits");
    gboolean obb = units && g_ascii_strcasecmp(units, "objectBoundingBox") == 0;
    if (obb) {
        cairo_matrix_t m;
        cairo_matrix_init_scale(&m, ctx->vw, ctx->vh);
        cairo_transform(ctx->cr, &m);
    }
    cairo_new_path(ctx->cr);
    svg_clip_path_children(ctx, clip, st);
    cairo_restore(ctx->cr);
    cairo_set_fill_rule(ctx->cr, st->clip_rule);
    cairo_clip(ctx->cr);
    cairo_new_path(ctx->cr);
    ctx->depth--;
}

static NsPangoLayout *
svg_text_layout(svg_ctx *ctx, const ns_node *n, const svg_state *st,
                double *out_x, double *out_top, double *out_w, double *out_h)
{
    char *text = ns_node_collect_text(n);
    if (!text || !*text) { g_free(text); return NULL; }

    gsize len = strlen(text);
    GString *flat = g_string_sized_new(len);
    gboolean prev_ws = TRUE;
    for (gsize i = 0; i < len; i++) {
        char c = text[i];
        if (svg_is_ws(c)) {
            if (!prev_ws) g_string_append_c(flat, ' ');
            prev_ws = TRUE;
        } else {
            g_string_append_c(flat, c);
            prev_ws = FALSE;
        }
    }
    g_free(text);
    while (flat->len > 0 && flat->str[flat->len - 1] == ' ')
        g_string_truncate(flat, flat->len - 1);
    if (flat->len == 0) { g_string_free(flat, TRUE); return NULL; }

    double x = svg_attr_length(n, "x", ctx->vw, st->font_size, 0);
    double y = svg_attr_length(n, "y", ctx->vh, st->font_size, 0);
    x += svg_attr_length(n, "dx", ctx->vw, st->font_size, 0);
    y += svg_attr_length(n, "dy", ctx->vh, st->font_size, 0);

    NsPangoLayout *layout = ns_pango_cairo_create_layout(ctx->cr);
    NsPangoFontDescription *desc = ns_pango_font_description_new();
    char *fam = ns_css_font_family_for_pango(st->font_family ? st->font_family
                                                              : "serif");
    ns_pango_font_description_set_family(desc, fam ? fam : "serif");
    g_free(fam);
    ns_pango_font_description_set_absolute_size(
        desc, ns_paint_pango_font_size(st->font_size));
    ns_pango_font_description_set_weight(desc, (NsPangoWeight)st->font_weight);
    if (st->font_italic)
        ns_pango_font_description_set_style(desc, NS_PANGO_STYLE_ITALIC);
    ns_pango_layout_set_font_description(layout, desc);
    ns_pango_font_description_free(desc);
    ns_pango_layout_set_text(layout, flat->str, (int)flat->len);
    g_string_free(flat, TRUE);

    int wpx = 0, hpx = 0;
    ns_pango_layout_get_pixel_size(layout, &wpx, &hpx);
    if (st->text_anchor == 1)      x -= wpx / 2.0;
    else if (st->text_anchor == 2) x -= wpx;
    int baseline = ns_pango_layout_get_baseline(layout) / NS_PANGO_SCALE;

    *out_x = x;
    *out_top = y - baseline;
    *out_w = wpx;
    *out_h = hpx;
    return layout;
}

static void
svg_render_text(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    double x, top, w, h;
    NsPangoLayout *layout = svg_text_layout(ctx, n, st, &x, &top, &w, &h);
    if (!layout) return;
    cairo_move_to(ctx->cr, x, top);
    ns_pango_cairo_layout_path(ctx->cr, layout);
    g_object_unref(layout);
    svg_paint_current_path(ctx, st);
}

typedef struct {
    double x, y;
    double in_angle, out_angle;
    gboolean has_in, has_out;
} svg_vertex;

static void
svg_vertex_add(GArray *out, double x, double y)
{
    svg_vertex v = { .x = x, .y = y };
    g_array_append_val(out, v);
}

static void
svg_vertex_link(GArray *out, double fromx, double fromy, double tox, double toy)
{
    if (out->len == 0) return;
    svg_vertex *prev = &g_array_index(out, svg_vertex, out->len - 1);
    double dx = tox - fromx, dy = toy - fromy;
    if (dx == 0 && dy == 0) return;
    double a = atan2(dy, dx);
    if (!prev->has_out) { prev->out_angle = a; prev->has_out = TRUE; }
}

static GArray *
svg_path_vertices(cairo_t *cr)
{
    cairo_path_t *path = cairo_copy_path(cr);
    if (!path) return NULL;
    if (path->status != CAIRO_STATUS_SUCCESS) {
        cairo_path_destroy(path);
        return NULL;
    }
    GArray *out = g_array_new(FALSE, TRUE, sizeof(svg_vertex));
    double cx = 0, cy = 0, sx = 0, sy = 0;
    for (int i = 0; i < path->num_data; i += path->data[i].header.length) {
        cairo_path_data_t *d = &path->data[i];
        switch (d->header.type) {
        case CAIRO_PATH_MOVE_TO:
            cx = sx = d[1].point.x;
            cy = sy = d[1].point.y;
            svg_vertex_add(out, cx, cy);
            break;
        case CAIRO_PATH_LINE_TO:
            svg_vertex_link(out, cx, cy, d[1].point.x, d[1].point.y);
            svg_vertex_add(out, d[1].point.x, d[1].point.y);
            if (out->len >= 2) {
                svg_vertex *v = &g_array_index(out, svg_vertex, out->len - 1);
                if (d[1].point.x != cx || d[1].point.y != cy) {
                    v->in_angle = atan2(d[1].point.y - cy, d[1].point.x - cx);
                    v->has_in = TRUE;
                }
            }
            cx = d[1].point.x; cy = d[1].point.y;
            break;
        case CAIRO_PATH_CURVE_TO: {
            double c1x = d[1].point.x, c1y = d[1].point.y;
            double c2x = d[2].point.x, c2y = d[2].point.y;
            double ex = d[3].point.x, ey = d[3].point.y;
            svg_vertex_link(out, cx, cy, c1x, c1y);
            svg_vertex_add(out, ex, ey);
            svg_vertex *v = &g_array_index(out, svg_vertex, out->len - 1);
            double tx = ex - c2x, ty = ey - c2y;
            if (tx == 0 && ty == 0) { tx = ex - c1x; ty = ey - c1y; }
            if (tx == 0 && ty == 0) { tx = ex - cx;  ty = ey - cy;  }
            if (tx != 0 || ty != 0) { v->in_angle = atan2(ty, tx); v->has_in = TRUE; }
            cx = ex; cy = ey;
            break;
        }
        case CAIRO_PATH_CLOSE_PATH:
            svg_vertex_link(out, cx, cy, sx, sy);
            svg_vertex_add(out, sx, sy);
            if (sx != cx || sy != cy) {
                svg_vertex *v = &g_array_index(out, svg_vertex, out->len - 1);
                v->in_angle = atan2(sy - cy, sx - cx);
                v->has_in = TRUE;
            }
            cx = sx; cy = sy;
            break;
        default:
            break;
        }
    }
    cairo_path_destroy(path);
    return out;
}

static const ns_node *
svg_marker_ref(svg_ctx *ctx, const ns_node *n, const char *prop)
{
    const char *v = svg_prop(n, prop);
    if (!v) v = svg_prop(n, "marker");
    if (!v) return NULL;
    char *id = svg_url_id(v, NULL);
    if (!id) return NULL;
    const ns_node *m = svg_by_id(ctx, id);
    g_free(id);
    if (!m || !m->name || strcmp(m->name, "marker") != 0) return NULL;
    return m;
}

static void
svg_draw_marker(svg_ctx *ctx, const ns_node *marker, const svg_state *st,
                double x, double y, double angle)
{
    if (ctx->depth >= NS_SVG_MAX_DEPTH) return;
    cairo_t *cr = ctx->cr;
    double fs = st->font_size;
    double mw = svg_attr_length(marker, "markerWidth", ctx->vw, fs, 3);
    double mh = svg_attr_length(marker, "markerHeight", ctx->vh, fs, 3);
    if (mw <= 0 || mh <= 0) return;

    const char *units = ns_element_get_attr(marker, "markerUnits");
    gboolean stroke_units = !units || g_ascii_strcasecmp(units, "strokeWidth") == 0;

    cairo_save(cr);
    cairo_translate(cr, x, y);
    const char *orient = ns_element_get_attr(marker, "orient");
    if (!orient || g_ascii_strcasecmp(orient, "auto") == 0 ||
        g_ascii_strcasecmp(orient, "auto-start-reverse") == 0)
        cairo_rotate(cr, angle);
    else
        cairo_rotate(cr, svg_length_str(orient, 0, fs, 0) * G_PI / 180.0);
    if (stroke_units && st->stroke_width > 0)
        cairo_scale(cr, st->stroke_width, st->stroke_width);

    cairo_matrix_t vm;
    svg_viewbox_matrix(marker, mw, mh, &vm);

    double rx = svg_attr_length(marker, "refX", ctx->vw, fs, 0);
    double ry = svg_attr_length(marker, "refY", ctx->vh, fs, 0);
    cairo_matrix_transform_point(&vm, &rx, &ry);
    cairo_translate(cr, -rx, -ry);

    const char *ov = svg_prop(marker, "overflow");
    gboolean clip = !ov || (g_ascii_strcasecmp(ov, "visible") != 0 &&
                            g_ascii_strcasecmp(ov, "auto") != 0);
    if (clip) {
        cairo_rectangle(cr, 0, 0, mw, mh);
        cairo_clip(cr);
        cairo_new_path(cr);
    }

    cairo_transform(cr, &vm);

    double ow = ctx->vw, oh = ctx->vh;
    const char *vb = ns_element_get_attr(marker, "viewBox");
    if (vb) {
        const char *q = vb;
        double a, b, c, dd;
        if (svg_num(&q, &a) && svg_num(&q, &b) &&
            svg_num(&q, &c) && svg_num(&q, &dd) && c > 0 && dd > 0) {
            ctx->vw = c; ctx->vh = dd;
        }
    }

    svg_state ms;
    svg_state_init(&ms);
    ms.color_r = st->color_r; ms.color_g = st->color_g;
    ms.color_b = st->color_b; ms.color_a = st->color_a;
    ms.font_size = st->font_size;
    ctx->depth++;
    for (const ns_node *c = marker->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_ELEMENT) svg_render_node(ctx, c, &ms);
    ctx->depth--;
    svg_state_clear(&ms);

    ctx->vw = ow; ctx->vh = oh;
    cairo_restore(cr);
}

static void
svg_render_markers(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    const ns_node *ms = svg_marker_ref(ctx, n, "marker-start");
    const ns_node *mm = svg_marker_ref(ctx, n, "marker-mid");
    const ns_node *me = svg_marker_ref(ctx, n, "marker-end");
    if (!ms && !mm && !me) return;

    GArray *verts = svg_path_vertices(ctx->cr);
    if (!verts) return;
    if (verts->len == 0) { g_array_free(verts, TRUE); return; }

    for (guint i = 0; i < verts->len; i++) {
        const svg_vertex *v = &g_array_index(verts, svg_vertex, i);
        const ns_node *m = (i == 0) ? ms
                         : (i == verts->len - 1) ? me : mm;
        if (!m) continue;
        double a;
        if (v->has_in && v->has_out) {
            double dx = cos(v->in_angle) + cos(v->out_angle);
            double dy = sin(v->in_angle) + sin(v->out_angle);
            a = (dx == 0 && dy == 0) ? v->in_angle : atan2(dy, dx);
        } else if (v->has_in) {
            a = v->in_angle;
        } else if (v->has_out) {
            a = v->out_angle;
        } else {
            a = 0;
        }
        if (i == 0 && m == ms) {
            const char *o = ns_element_get_attr(m, "orient");
            if (o && g_ascii_strcasecmp(o, "auto-start-reverse") == 0)
                a += G_PI;
        }
        svg_draw_marker(ctx, m, st, v->x, v->y, a);
    }
    g_array_free(verts, TRUE);
}

static void
svg_render_children(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT) continue;
        svg_render_node(ctx, c, st);
    }
}

static gboolean
svg_language_preferred(const char *tag, char **preferred)
{
    gsize tag_len = strlen(tag);
    for (guint i = 0; preferred && preferred[i]; i++) {
        gsize len = strlen(preferred[i]);
        if (len == 0 || len > tag_len) continue;
        if (g_ascii_strncasecmp(tag, preferred[i], len) != 0) continue;
        if (tag[len] == '\0' || tag[len] == '-') return TRUE;
    }
    return FALSE;
}

static gboolean
svg_system_language_allows(const ns_node *n)
{
    const char *value = ns_element_get_attr(n, "systemLanguage");
    if (!value) return TRUE;
    char **preferred = ns_net_navigator_languages();
    char **tags = g_strsplit(value, ",", -1);
    gboolean allowed = FALSE;
    for (guint i = 0; tags[i] && !allowed; i++)
        allowed = svg_language_preferred(g_strstrip(tags[i]), preferred);
    g_strfreev(tags);
    g_strfreev(preferred);
    return allowed;
}

static gboolean
svg_is_hidden(svg_ctx *ctx, const ns_node *n)
{
    if (!svg_system_language_allows(n)) return TRUE;
    const ns_style *s = ctx->styles
        ? g_hash_table_lookup(ctx->styles, (gpointer)n) : NULL;
    if (s) {
        const char *d = ns_style_keyword(s, NS_CSS_DISPLAY);
        if (d && strcmp(d, "none") == 0) return TRUE;
    }
    const char *d = svg_prop(n, "display");
    return d && g_ascii_strcasecmp(d, "none") == 0;
}

static ns_svg_foreign_painter g_foreign_painter;
static void *g_foreign_painter_data;

void
ns_svg_set_foreign_painter(ns_svg_foreign_painter painter, void *user_data)
{
    g_foreign_painter = painter;
    g_foreign_painter_data = user_data;
}

static gboolean
svg_renderable(const char *tag)
{
    static const char *const tags[] = {
        "a", "circle", "ellipse", "foreignObject", "g", "image", "line",
        "path", "polygon", "polyline", "rect", "svg", "switch", "text",
        "textPath", "tspan", "use",
    };
    const char *colon = strchr(tag, ':');
    if (colon) tag = colon + 1;
    for (gsize i = 0; i < G_N_ELEMENTS(tags); i++)
        if (strcmp(tag, tags[i]) == 0) return TRUE;
    return FALSE;
}

static const ns_node *
svg_switch_choice(const ns_node *n)
{
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT) continue;
        if (ns_element_get_attr(c, "requiredExtensions") ||
            ns_element_get_attr(c, "requiredFeatures")) continue;
        if (!svg_system_language_allows(c)) continue;
        return c;
    }
    return NULL;
}

static gboolean
svg_shape_markable(const char *tag)
{
    return strcmp(tag, "path") == 0 || strcmp(tag, "line") == 0 ||
           strcmp(tag, "polyline") == 0 || strcmp(tag, "polygon") == 0;
}

static void
svg_render_shape(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    cairo_t *cr = ctx->cr;
    cairo_new_path(cr);
    if (svg_shape_path(ctx, n, st)) {
        cairo_path_t *kept = svg_shape_markable(n->name)
            ? cairo_copy_path(cr) : NULL;
        svg_paint_current_path(ctx, st);
        if (kept) {
            cairo_new_path(cr);
            cairo_append_path(cr, kept);
            cairo_path_destroy(kept);
            svg_render_markers(ctx, n, st);
        }
    }
    cairo_new_path(cr);
}

typedef struct {
    char            *url;
    cairo_surface_t *surface;
    gsize            bytes;
} svg_image_entry;

static GMutex       g_svg_image_lock;
static GPtrArray   *g_svg_images;
static gsize        g_svg_image_bytes;
static __thread int g_svg_image_nest;

static void
svg_image_entry_free(gpointer data)
{
    svg_image_entry *e = data;
    if (e->surface) cairo_surface_destroy(e->surface);
    g_free(e->url);
    g_free(e);
}

static cairo_surface_t *
svg_image_cache_lookup(const char *url, gboolean *found)
{
    cairo_surface_t *hit = NULL;
    *found = FALSE;
    g_mutex_lock(&g_svg_image_lock);
    for (guint i = 0; g_svg_images && i < g_svg_images->len; i++) {
        svg_image_entry *e = g_ptr_array_index(g_svg_images, i);
        if (strcmp(e->url, url) != 0) continue;
        *found = TRUE;
        hit = e->surface ? cairo_surface_reference(e->surface) : NULL;
        g_ptr_array_add(g_svg_images, g_ptr_array_steal_index(g_svg_images, i));
        break;
    }
    g_mutex_unlock(&g_svg_image_lock);
    return hit;
}

static void
svg_image_cache_store(const char *url, cairo_surface_t *surface)
{
    svg_image_entry *e = g_new0(svg_image_entry, 1);
    e->url = g_strdup(url);
    e->bytes = strlen(url);
    if (surface) {
        e->surface = cairo_surface_reference(surface);
        e->bytes += (gsize)cairo_image_surface_get_stride(surface) *
                    (gsize)cairo_image_surface_get_height(surface);
    }
    if (e->bytes > NS_SVG_IMAGE_CACHE_SIZE) {
        svg_image_entry_free(e);
        return;
    }
    g_mutex_lock(&g_svg_image_lock);
    if (!g_svg_images)
        g_svg_images = g_ptr_array_new_with_free_func(svg_image_entry_free);
    g_ptr_array_add(g_svg_images, e);
    g_svg_image_bytes += e->bytes;
    while (g_svg_images->len > 0 &&
           (g_svg_images->len > NS_SVG_IMAGE_CACHE_LEN ||
            g_svg_image_bytes > NS_SVG_IMAGE_CACHE_SIZE)) {
        svg_image_entry *old = g_ptr_array_index(g_svg_images, 0);
        g_svg_image_bytes -= old->bytes;
        g_ptr_array_remove_index(g_svg_images, 0);
    }
    g_mutex_unlock(&g_svg_image_lock);
}

static cairo_surface_t *
svg_texture_surface(ns_texture *tex, int w, int h)
{
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    ns_texture_download(tex, cairo_image_surface_get_data(surf),
                        (gsize)cairo_image_surface_get_stride(surf));
    cairo_surface_mark_dirty(surf);
    return surf;
}

static cairo_surface_t *
svg_image_decode(const char *url)
{
    GByteArray *bytes = g_byte_array_new();
    ns_texture *tex = NULL;
    int w = 0, h = 0;
    if (ns_data_url_decode(url, bytes, NS_SVG_MAX_IMAGE_BYTES, NULL, NULL) &&
        bytes->len > 0) {
        g_svg_image_nest++;
        tex = ns_image_decode_bytes(bytes->data, bytes->len, &w, &h);
        g_svg_image_nest--;
    }
    g_byte_array_free(bytes, TRUE);
    if (!tex) return NULL;
    cairo_surface_t *surf = w > 0 && h > 0 ? svg_texture_surface(tex, w, h)
                                           : NULL;
    ns_texture_unref(tex);
    return surf;
}

static cairo_surface_t *
svg_image_view(cairo_surface_t *master)
{
    static const cairo_user_data_key_t key;
    cairo_surface_t *view = cairo_image_surface_create_for_data(
        cairo_image_surface_get_data(master), CAIRO_FORMAT_ARGB32,
        cairo_image_surface_get_width(master),
        cairo_image_surface_get_height(master),
        cairo_image_surface_get_stride(master));
    if (cairo_surface_set_user_data(view, &key, master,
            (cairo_destroy_func_t)cairo_surface_destroy) !=
        CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(view);
        cairo_surface_destroy(master);
        return NULL;
    }
    return view;
}

static cairo_surface_t *
svg_image_source(const char *href)
{
    while (*href && svg_is_ws(*href)) href++;
    if (!g_str_has_prefix(href, "data:")) return NULL;
    if (g_svg_image_nest >= NS_SVG_MAX_IMAGE_NEST) return NULL;
    gboolean found = FALSE;
    cairo_surface_t *master = svg_image_cache_lookup(href, &found);
    if (!found) {
        master = svg_image_decode(href);
        svg_image_cache_store(href, master);
    }
    return master ? svg_image_view(master) : NULL;
}

static void
svg_image_box(svg_ctx *ctx, const ns_node *n, double fs,
              const svg_rect *natural, svg_rect *box)
{
    box->x = svg_geom(ctx, n, "x", NS_CSS_SVG_X, ctx->vw, fs, 0, NULL);
    box->y = svg_geom(ctx, n, "y", NS_CSS_SVG_Y, ctx->vh, fs, 0, NULL);
    box->w = svg_geom(ctx, n, "width", NS_CSS_WIDTH, ctx->vw, fs, -1, NULL);
    box->h = svg_geom(ctx, n, "height", NS_CSS_HEIGHT, ctx->vh, fs, -1, NULL);
    if (box->w < 0 && box->h < 0) {
        box->w = natural->w;
        box->h = natural->h;
    } else if (box->w < 0) {
        box->w = box->h * natural->w / natural->h;
    } else if (box->h < 0) {
        box->h = box->w * natural->h / natural->w;
    }
}

static void
svg_draw_image(cairo_t *cr, cairo_surface_t *img, const svg_rect *natural,
               const svg_rect *box, const char *par)
{
    cairo_matrix_t to_user, shift;
    svg_fit_matrix(natural, par, box->w, box->h, &to_user);
    cairo_matrix_init_translate(&shift, box->x, box->y);
    cairo_matrix_multiply(&to_user, &to_user, &shift);
    cairo_matrix_t to_image = to_user;
    if (cairo_matrix_invert(&to_image) != CAIRO_STATUS_SUCCESS) return;
    cairo_pattern_t *pat = svg_surface_pattern(cr, img, &to_image,
                                               CAIRO_EXTEND_PAD);
    if (!pat) return;
    double x0 = 0, y0 = 0, x1 = natural->w, y1 = natural->h;
    cairo_matrix_transform_point(&to_user, &x0, &y0);
    cairo_matrix_transform_point(&to_user, &x1, &y1);
    cairo_save(cr);
    cairo_rectangle(cr, box->x, box->y, box->w, box->h);
    cairo_clip(cr);
    cairo_rectangle(cr, MIN(x0, x1), MIN(y0, y1), fabs(x1 - x0), fabs(y1 - y0));
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_pattern_destroy(pat);
}

static void
svg_render_image(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    const char *href = svg_href(n);
    cairo_surface_t *img = href ? svg_image_source(href) : NULL;
    if (!img) return;
    svg_rect natural = { 0, 0, cairo_image_surface_get_width(img),
                         cairo_image_surface_get_height(img) };
    svg_rect box;
    svg_image_box(ctx, n, st->font_size, &natural, &box);
    if (box.w > 0 && box.h > 0)
        svg_draw_image(ctx->cr, img, &natural, &box,
                       ns_element_get_attr(n, "preserveAspectRatio"));
    cairo_surface_destroy(img);
}

static void
svg_render_node_nested(svg_ctx *ctx, const ns_node *n,
                       const svg_state *parent)
{
    const char *tag = n->name;
    if (!svg_renderable(tag)) return;

    if (svg_is_hidden(ctx, n)) return;

    svg_state st;
    svg_state_copy(&st, parent);
    svg_state_apply_node(ctx, &st, n);

    double opacity = 1.0;
    const ns_style *s = ctx->styles
        ? g_hash_table_lookup(ctx->styles, (gpointer)n) : NULL;
    if (s && s->values[NS_CSS_OPACITY])
        opacity = CLAMP(svg_css_number(ctx, s, NS_CSS_OPACITY, st.font_size, 1.0, 1.0), 0.0, 1.0);
    else {
        const char *o = svg_prop(n, "opacity");
        if (o) opacity = CLAMP(svg_length_str(o, 1.0, st.font_size, 1.0), 0.0, 1.0);
    }

    if (st.hidden && strcmp(tag, "g") != 0 && strcmp(tag, "svg") != 0) {
        svg_state_clear(&st);
        return;
    }
    if (opacity <= 0.0) {
        svg_state_clear(&st);
        return;
    }

    cairo_t *cr = ctx->cr;
    cairo_save(cr);
    svg_apply_transform_attr(ctx, n, "transform");

    cairo_surface_t *mask = svg_mask_surface(ctx, n, &st);
    double blur = svg_filter_blur(ctx, n);
    gboolean grouped = opacity < 1.0 || mask != NULL || blur > 0;
    if (grouped) cairo_push_group(cr);

    svg_apply_clip(ctx, n, &st);

    if (strcmp(tag, "g") == 0 || strcmp(tag, "a") == 0) {
        svg_render_children(ctx, n, &st);
    } else if (strcmp(tag, "switch") == 0) {
        const ns_node *choice = svg_switch_choice(n);
        if (choice) svg_render_node(ctx, choice, &st);
    } else if (strcmp(tag, "svg") == 0) {
        double x = svg_attr_length(n, "x", ctx->vw, st.font_size, 0);
        double y = svg_attr_length(n, "y", ctx->vh, st.font_size, 0);
        double w = svg_attr_length(n, "width", ctx->vw, st.font_size, ctx->vw);
        double h = svg_attr_length(n, "height", ctx->vh, st.font_size, ctx->vh);
        cairo_translate(cr, x, y);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_clip(cr);
        cairo_new_path(cr);
        cairo_matrix_t vm;
        svg_viewbox_matrix(n, w, h, &vm);
        cairo_transform(cr, &vm);
        double ow = ctx->vw, oh = ctx->vh;
        const char *vb = ns_element_get_attr(n, "viewBox");
        if (vb) {
            const char *p = vb;
            double a, b, c, d;
            if (svg_num(&p, &a) && svg_num(&p, &b) &&
                svg_num(&p, &c) && svg_num(&p, &d) && c > 0 && d > 0) {
                ctx->vw = c; ctx->vh = d;
            }
        } else {
            ctx->vw = w; ctx->vh = h;
        }
        svg_render_children(ctx, n, &st);
        ctx->vw = ow; ctx->vh = oh;
    } else if (strcmp(tag, "use") == 0) {
        const ns_node *t = svg_href_target(ctx, n);
        if (t && t != n) {
            double x = svg_attr_length(n, "x", ctx->vw, st.font_size, 0);
            double y = svg_attr_length(n, "y", ctx->vh, st.font_size, 0);
            cairo_translate(cr, x, y);
            ctx->depth++;
            st.instanced = TRUE;
            if (t->name && strcmp(t->name, "symbol") == 0) {
                double w = svg_attr_length(n, "width", ctx->vw, st.font_size, ctx->vw);
                double h = svg_attr_length(n, "height", ctx->vh, st.font_size, ctx->vh);
                cairo_rectangle(cr, 0, 0, w, h);
                cairo_clip(cr);
                cairo_new_path(cr);
                cairo_matrix_t vm;
                svg_viewbox_matrix(t, w, h, &vm);
                cairo_transform(cr, &vm);
                svg_state_apply_node(ctx, &st, t);
                svg_render_children(ctx, t, &st);
            } else {
                svg_render_node(ctx, t, &st);
            }
            ctx->depth--;
        }
    } else if (strcmp(tag, "foreignObject") == 0) {
        double x = svg_attr_length(n, "x", ctx->vw, st.font_size, 0);
        double y = svg_attr_length(n, "y", ctx->vh, st.font_size, 0);
        double w = svg_attr_length(n, "width", ctx->vw, st.font_size, ctx->vw);
        double h = svg_attr_length(n, "height", ctx->vh, st.font_size, ctx->vh);
        if (g_foreign_painter && w > 0 && h > 0) {
            cairo_translate(cr, x, y);
            cairo_rectangle(cr, 0, 0, w, h);
            cairo_clip(cr);
            cairo_new_path(cr);
            g_foreign_painter(cr, n, w, h, g_foreign_painter_data);
        }
    } else if (strcmp(tag, "text") == 0) {
        svg_render_text(ctx, n, &st);
    } else if (strcmp(tag, "tspan") == 0) {
        svg_render_text(ctx, n, &st);
    } else if (strcmp(tag, "image") == 0) {
        svg_render_image(ctx, n, &st);
    } else {
        svg_render_shape(ctx, n, &st);
    }

    if (grouped) {
        cairo_pop_group_to_source(cr);
        if (blur > 0) svg_blur_source(cr, blur);
        if (mask) {
            cairo_save(cr);
            cairo_identity_matrix(cr);
            if (opacity < 1.0) {
                cairo_push_group(cr);
                cairo_mask_surface(cr, mask, 0, 0);
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, opacity);
            } else {
                cairo_mask_surface(cr, mask, 0, 0);
            }
            cairo_restore(cr);
        } else {
            cairo_paint_with_alpha(cr, opacity);
        }
    }
    if (mask) cairo_surface_destroy(mask);
    cairo_restore(cr);
    svg_state_clear(&st);
}

static void
svg_render_node(svg_ctx *ctx, const ns_node *n, const svg_state *parent)
{
    if (!n->name) return;
    if (ctx->depth >= NS_SVG_MAX_DEPTH) return;
    if (ctx->nesting >= NS_SVG_MAX_NESTING) return;
    if (++ctx->nodes > NS_SVG_MAX_NODES) return;
    ctx->nesting++;
    svg_render_node_nested(ctx, n, parent);
    ctx->nesting--;
}

static void
svg_state_init_inherited(svg_state *st, const struct ns_style *inherited)
{
    svg_state_init(st);
    if (!inherited) return;
    const ns_css_value *c = inherited->values[NS_CSS_COLOR];
    if (c && c->kind == NS_CSS_V_COLOR) {
        st->color_r = c->u.color.r / 255.0;
        st->color_g = c->u.color.g / 255.0;
        st->color_b = c->u.color.b / 255.0;
        st->color_a = c->u.color.a / 255.0;
    }
    if (inherited->values[NS_CSS_FONT_SIZE])
        st->font_size = MAX(1.0, ns_css_length_or(
            inherited->values[NS_CSS_FONT_SIZE], 16.0));
}

typedef struct {
    double   x0, y0, x1, y1;
    gboolean any;
} svg_extent;

typedef struct {
    cairo_matrix_t to_root;
    cairo_matrix_t to_viewport;
} svg_frame;

typedef struct svg_measure {
    const ns_node   *target;
    GHashTable      *path;
    gboolean         inside;
    gboolean         boxless;
    ns_svg_geometry *out;
} svg_measure;

static void svg_measure_node(svg_ctx *ctx, const ns_node *n,
                             const svg_state *parent, const svg_frame *outer,
                             svg_extent *outer_extent);

static void
svg_extent_add_point(svg_extent *e, double x, double y)
{
    if (!e->any) {
        e->x0 = e->x1 = x;
        e->y0 = e->y1 = y;
        e->any = TRUE;
        return;
    }
    e->x0 = MIN(e->x0, x);
    e->x1 = MAX(e->x1, x);
    e->y0 = MIN(e->y0, y);
    e->y1 = MAX(e->y1, y);
}

static void
svg_extent_add_mapped(svg_extent *into, const svg_extent *from,
                      const cairo_matrix_t *to_outer)
{
    if (!from->any) return;
    const double xs[2] = { from->x0, from->x1 };
    const double ys[2] = { from->y0, from->y1 };
    for (int corner = 0; corner < 4; corner++) {
        double x = xs[corner & 1], y = ys[corner >> 1];
        cairo_matrix_transform_point(to_outer, &x, &y);
        svg_extent_add_point(into, x, y);
    }
}

static svg_frame
svg_frame_inside(const svg_frame *outer, const cairo_matrix_t *local)
{
    svg_frame frame;
    cairo_matrix_multiply(&frame.to_root, local, &outer->to_root);
    cairo_matrix_multiply(&frame.to_viewport, local, &outer->to_viewport);
    return frame;
}


static void
svg_measure_children(svg_ctx *ctx, const ns_node *n, const svg_state *st,
                     const svg_frame *frame, svg_extent *own)
{
    svg_measure *m = ctx->measure;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (m->out->found) return;
        if (c->kind != NS_NODE_ELEMENT) continue;
        if (!m->inside && !g_hash_table_contains(m->path, c)) continue;
        svg_measure_node(ctx, c, st, frame, own);
    }
}

static gboolean
svg_path_is_lone_point(cairo_t *cr, double *x, double *y)
{
    cairo_path_t *path = cairo_copy_path(cr);
    gboolean lone = path->num_data > 0;
    double px = 0, py = 0;
    for (int i = 0; lone && i < path->num_data;
         i += path->data[i].header.length) {
        lone = path->data[i].header.type == CAIRO_PATH_MOVE_TO;
        if (!lone) break;
        px = path->data[i + 1].point.x;
        py = path->data[i + 1].point.y;
    }
    cairo_path_destroy(path);
    if (!lone) return FALSE;
    *x = px;
    *y = py;
    return TRUE;
}

static void
svg_measure_shape(svg_ctx *ctx, const ns_node *n, const svg_state *st,
                  svg_extent *own)
{
    cairo_t *cr = ctx->cr;
    cairo_new_path(cr);
    if (svg_shape_path(ctx, n, st) && cairo_has_current_point(cr)) {
        double x0, y0, x1, y1;
        cairo_path_extents(cr, &x0, &y0, &x1, &y1);
        if (x0 == x1 && y0 == y1 && svg_path_is_lone_point(cr, &x0, &y0)) {
            x1 = x0;
            y1 = y0;
        }
        svg_extent_add_point(own, x0, y0);
        svg_extent_add_point(own, x1, y1);
    }
    cairo_new_path(cr);
}

static void
svg_measure_positioned_box(svg_ctx *ctx, const ns_node *n, const svg_state *st,
                           svg_extent *own)
{
    double fs = st->font_size;
    double x = svg_geom(ctx, n, "x", NS_CSS_SVG_X, ctx->vw, fs, 0, NULL);
    double y = svg_geom(ctx, n, "y", NS_CSS_SVG_Y, ctx->vh, fs, 0, NULL);
    double w = svg_geom(ctx, n, "width", NS_CSS_WIDTH, ctx->vw, fs, 0, NULL);
    double h = svg_geom(ctx, n, "height", NS_CSS_HEIGHT, ctx->vh, fs, 0, NULL);
    if (w <= 0 || h <= 0) return;
    svg_extent_add_point(own, x, y);
    svg_extent_add_point(own, x + w, y + h);
}

static void
svg_measure_text(svg_ctx *ctx, const ns_node *n, const svg_state *st,
                 svg_extent *own)
{
    double x, top, w, h;
    NsPangoLayout *layout = svg_text_layout(ctx, n, st, &x, &top, &w, &h);
    if (!layout) return;
    g_object_unref(layout);
    svg_extent_add_point(own, x, top);
    svg_extent_add_point(own, x + w, top + h);
}

static void
svg_measure_use(svg_ctx *ctx, const ns_node *n, const svg_state *st,
                const svg_frame *frame, svg_extent *own)
{
    const ns_node *referenced = svg_href_target(ctx, n);
    if (!referenced || referenced == n) return;
    double fs = st->font_size;
    cairo_matrix_t placed;
    cairo_matrix_init_translate(&placed,
                                svg_attr_length(n, "x", ctx->vw, fs, 0),
                                svg_attr_length(n, "y", ctx->vh, fs, 0));
    svg_extent content = { 0 };
    svg_state instance;
    svg_state_copy(&instance, st);
    instance.instanced = TRUE;
    ctx->depth++;
    if (referenced->name && strcmp(referenced->name, "symbol") == 0) {
        double w = svg_attr_length(n, "width", ctx->vw, fs, ctx->vw);
        double h = svg_attr_length(n, "height", ctx->vh, fs, ctx->vh);
        cairo_matrix_t fitted;
        svg_viewbox_matrix(referenced, w, h, &fitted);
        cairo_matrix_multiply(&placed, &fitted, &placed);
        svg_frame inner = svg_frame_inside(frame, &placed);
        svg_state_apply_node(ctx, &instance, referenced);
        svg_measure_children(ctx, referenced, &instance, &inner, &content);
    } else {
        svg_frame inner = svg_frame_inside(frame, &placed);
        svg_measure_node(ctx, referenced, &instance, &inner, &content);
    }
    ctx->depth--;
    svg_state_clear(&instance);
    svg_extent_add_mapped(own, &content, &placed);
}

static void
svg_measure_content(svg_ctx *ctx, const ns_node *n, const svg_state *st,
                    const svg_frame *frame, svg_extent *own)
{
    const char *tag = n->name;
    if (strcmp(tag, "switch") == 0) {
        const ns_node *choice = svg_switch_choice(n);
        if (choice) svg_measure_node(ctx, choice, st, frame, own);
    } else if (strcmp(tag, "use") == 0) {
        svg_measure_use(ctx, n, st, frame, own);
    } else if (strcmp(tag, "text") == 0 || strcmp(tag, "tspan") == 0) {
        svg_measure_text(ctx, n, st, own);
    } else if (strcmp(tag, "rect") == 0 || strcmp(tag, "image") == 0 ||
               strcmp(tag, "foreignObject") == 0) {
        svg_measure_positioned_box(ctx, n, st, own);
    } else if (strcmp(tag, "g") == 0 || strcmp(tag, "a") == 0 ||
               strcmp(tag, "svg") == 0 || !svg_renderable(tag)) {
        svg_measure_children(ctx, n, st, frame, own);
    } else {
        svg_measure_shape(ctx, n, st, own);
    }
}

static void
svg_measure_toward_target(svg_ctx *ctx, const ns_node *n, const svg_state *st,
                          const svg_frame *frame, svg_extent *own)
{
    svg_measure *m = ctx->measure;
    if (strcmp(n->name, "switch") == 0) {
        const ns_node *choice = svg_switch_choice(n);
        if (!choice || !g_hash_table_contains(m->path, choice)) {
            m->out->rendered = FALSE;
            m->boxless = TRUE;
        }
    }
    svg_measure_children(ctx, n, st, frame, own);
}

static void
svg_measure_report(svg_measure *m, const svg_frame *frame,
                   const svg_extent *own)
{
    ns_svg_geometry *geometry = m->out;
    geometry->found = TRUE;
    geometry->to_root = frame->to_root;
    geometry->to_viewport = frame->to_viewport;
    geometry->has_box = own->any;
    if (!own->any) return;
    geometry->x = own->x0;
    geometry->y = own->y0;
    geometry->width = own->x1 - own->x0;
    geometry->height = own->y1 - own->y0;
}

static void
svg_measure_local_matrix(svg_ctx *ctx, const ns_node *n, const svg_state *st,
                         cairo_matrix_t *local, cairo_matrix_t *viewport)
{
    svg_parse_transform(ns_element_get_attr(n, "transform"), local);
    if (strcmp(n->name, "svg") != 0) return;
    double fs = st->font_size;
    double w = svg_attr_length(n, "width", ctx->vw, fs, ctx->vw);
    double h = svg_attr_length(n, "height", ctx->vh, fs, ctx->vh);
    cairo_matrix_t placed;
    cairo_matrix_init_translate(&placed,
                                svg_attr_length(n, "x", ctx->vw, fs, 0),
                                svg_attr_length(n, "y", ctx->vh, fs, 0));
    svg_viewbox_matrix(n, w, h, viewport);
    cairo_matrix_multiply(viewport, viewport, &placed);
    cairo_matrix_multiply(local, viewport, local);
    svg_viewbox_size(n, w, h, &ctx->vw, &ctx->vh);
}

static void
svg_measure_node_nested(svg_ctx *ctx, const ns_node *n,
                        const svg_state *parent, const svg_frame *outer,
                        svg_extent *outer_extent)
{
    svg_measure *m = ctx->measure;
    if (m->inside && (!svg_renderable(n->name) || svg_is_hidden(ctx, n)))
        return;

    svg_state st;
    svg_state_copy(&st, parent);
    svg_state_apply_node(ctx, &st, n);

    double outer_vw = ctx->vw, outer_vh = ctx->vh;
    cairo_matrix_t local, viewport;
    cairo_matrix_init_identity(&viewport);
    svg_measure_local_matrix(ctx, n, &st, &local, &viewport);
    svg_frame frame = svg_frame_inside(outer, &local);
    svg_frame child_frame = frame;
    if (strcmp(n->name, "svg") == 0) child_frame.to_viewport = viewport;

    svg_extent own = { 0 };
    gboolean reached = !m->inside &&
        (n == m->target || strcmp(n->name, "text") == 0);
    if (reached) m->inside = TRUE;
    if (!m->inside)
        svg_measure_toward_target(ctx, n, &st, &child_frame, &own);
    else if (!m->boxless)
        svg_measure_content(ctx, n, &st, &child_frame, &own);
    if (reached) {
        m->inside = FALSE;
        svg_measure_report(m, &frame, &own);
    } else if (m->inside) {
        svg_extent_add_mapped(outer_extent, &own, &local);
    }

    ctx->vw = outer_vw;
    ctx->vh = outer_vh;
    svg_state_clear(&st);
}

static void
svg_measure_node(svg_ctx *ctx, const ns_node *n, const svg_state *parent,
                 const svg_frame *outer, svg_extent *outer_extent)
{
    if (!n->name) return;
    if (ctx->depth >= NS_SVG_MAX_DEPTH) return;
    if (ctx->nesting >= NS_SVG_MAX_NESTING) return;
    if (++ctx->nodes > NS_SVG_MAX_NODES) return;
    ctx->nesting++;
    svg_measure_node_nested(ctx, n, parent, outer, outer_extent);
    ctx->nesting--;
}

gboolean
ns_svg_node_is_root(const ns_node *n)
{
    return n && n->kind == NS_NODE_ELEMENT && n->name &&
           strcmp(n->name, "svg") == 0;
}

void
ns_svg_intrinsic_size(const ns_node *svg, ns_svg_size *out)
{
    memset(out, 0, sizeof *out);
    if (!svg) return;

    const char *ws = ns_element_get_attr(svg, "width");
    const char *hs = ns_element_get_attr(svg, "height");
    if (ws && !strchr(ws, '%')) {
        double w = svg_length_str(ws, 0, 16.0, 0);
        if (w > 0) { out->width = w; out->has_width = TRUE; }
    }
    if (hs && !strchr(hs, '%')) {
        double h = svg_length_str(hs, 0, 16.0, 0);
        if (h > 0) { out->height = h; out->has_height = TRUE; }
    }

    const char *vb = ns_element_get_attr(svg, "viewBox");
    if (vb) {
        const char *p = vb;
        double x, y, w, h;
        if (svg_num(&p, &x) && svg_num(&p, &y) &&
            svg_num(&p, &w) && svg_num(&p, &h) && w > 0 && h > 0) {
            out->ratio = w / h;
            out->has_ratio = TRUE;
        }
    }
    if (out->has_width && out->has_height && !out->has_ratio && out->height > 0) {
        out->ratio = out->width / out->height;
        out->has_ratio = TRUE;
    }
    if (out->has_width && !out->has_height && out->has_ratio) {
        out->height = out->width / out->ratio;
        out->has_height = TRUE;
    } else if (out->has_height && !out->has_width && out->has_ratio) {
        out->width = out->height * out->ratio;
        out->has_width = TRUE;
    }
}

static void
svg_ctx_clear(svg_ctx *ctx)
{
    if (ctx->ids) g_hash_table_destroy(ctx->ids);
    if (ctx->tiles) g_ptr_array_free(ctx->tiles, TRUE);
}

static void
svg_render_root(cairo_t *cr, const ns_node *svg, double width, double height,
                GHashTable *styles, const struct ns_style *inherited,
                gboolean declared)
{
    svg_ctx ctx = {
        .cr = cr,
        .root = svg,
        .styles = styles,
        .icb_w = width,
        .icb_h = height,
        .unit_vw = declared ? width : ns_css_viewport_w(),
        .unit_vh = declared ? height : ns_css_viewport_h(),
        .root_font_size = 16.0,
        .declared = declared,
        .inherited = inherited,
    };

    svg_state st;
    svg_state_init_inherited(&st, inherited);

    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    cairo_new_path(cr);

    cairo_matrix_t vm;
    svg_viewbox_matrix(svg, width, height, &vm);
    cairo_transform(cr, &vm);
    svg_viewbox_size(svg, width, height, &ctx.vw, &ctx.vh);

    GHashTable *prev_var_styles = g_svg_var_styles;
    g_svg_var_styles = styles;
    svg_state_apply_node(&ctx, &st, svg);
    svg_render_children(&ctx, svg, &st);
    g_svg_var_styles = prev_var_styles;

    cairo_restore(cr);
    svg_state_clear(&st);
    svg_ctx_clear(&ctx);
}

void
ns_svg_render_node(cairo_t *cr, const ns_node *svg, double width, double height,
                   GHashTable *styles, const struct ns_style *inherited)
{
    if (!cr || !svg || width <= 0 || height <= 0) return;
    svg_render_root(cr, svg, width, height, styles, inherited, FALSE);
}

static GHashTable *
svg_path_to(const ns_node *svg, const ns_node *target)
{
    GHashTable *path = g_hash_table_new(NULL, NULL);
    const ns_node *n = target;
    for (; n && n != svg; n = n->parent)
        g_hash_table_add(path, (gpointer)n);
    if (n) return path;
    g_hash_table_destroy(path);
    return NULL;
}

static void
svg_measure_target_rendering(svg_ctx *ctx, const ns_node *svg)
{
    svg_measure *m = ctx->measure;
    for (const ns_node *n = m->target; n && n != svg; n = n->parent) {
        gboolean hidden = svg_is_hidden(ctx, n);
        gboolean foreign = !(n->flags & NS_NODE_SVG_NS);
        if (hidden) m->boxless = TRUE;
        if (hidden || !(foreign || svg_renderable(n->name)))
            m->out->rendered = FALSE;
    }
}

gboolean
ns_svg_node_geometry(const ns_node *svg, const ns_node *target,
                     double width, double height, GHashTable *styles,
                     const struct ns_style *inherited, ns_svg_geometry *out)
{
    memset(out, 0, sizeof *out);
    cairo_matrix_init_identity(&out->to_root);
    cairo_matrix_init_identity(&out->to_viewport);
    if (!svg || !target) return FALSE;
    GHashTable *path = svg_path_to(svg, target);
    if (!path) return FALSE;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cr = cairo_create(surface);
    svg_measure measure = {
        .target = target,
        .path = path,
        .inside = target == svg,
        .out = out,
    };
    svg_ctx ctx = {
        .cr = cr,
        .root = svg,
        .styles = styles,
        .icb_w = width,
        .icb_h = height,
        .unit_vw = ns_css_viewport_w(),
        .unit_vh = ns_css_viewport_h(),
        .root_font_size = 16.0,
        .measure = &measure,
    };
    out->rendered = TRUE;

    svg_frame frame;
    svg_viewbox_matrix(svg, width, height, &frame.to_root);
    frame.to_viewport = frame.to_root;
    svg_viewbox_size(svg, width, height, &ctx.vw, &ctx.vh);

    svg_state st;
    svg_state_init_inherited(&st, inherited);
    GHashTable *prev_var_styles = g_svg_var_styles;
    g_svg_var_styles = styles;
    svg_measure_target_rendering(&ctx, svg);
    svg_state_apply_node(&ctx, &st, svg);
    svg_extent own = { 0 };
    svg_measure_children(&ctx, svg, &st, &frame, &own);
    if (target == svg) svg_measure_report(&measure, &frame, &own);
    g_svg_var_styles = prev_var_styles;

    svg_state_clear(&st);
    svg_ctx_clear(&ctx);
    g_hash_table_destroy(path);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return out->found;
}

gboolean
ns_svg_bytes_look_like_svg(const guchar *data, gsize len)
{
    if (!data || len < 4) return FALSE;
    gsize scan = MIN(len, (gsize)1024);
    for (gsize i = 0; i + 4 < scan; i++) {
        if (data[i] == '<' &&
            g_ascii_strncasecmp((const char *)data + i, "<svg", 4) == 0)
            return TRUE;
    }
    return FALSE;
}

static const ns_node *
svg_find_root(const ns_node *doc)
{
    for (const ns_node *n = doc; n; n = ns_node_next_in_subtree(n, doc, TRUE))
        if (ns_svg_node_is_root(n)) return n;
    return NULL;
}

const ns_node *
ns_svg_document_root(const ns_node *doc)
{
    return svg_find_root(doc);
}

typedef struct {
    const ns_css_rule *rule;
    gint64             specificity;
    int                layer;
    guint              order;
} svg_rule_match;

typedef struct {
    ns_style *style;
    guint64   important[NS_SVG_PROP_WORDS];
    guint64   animated[NS_SVG_PROP_WORDS];
} svg_cascaded;

typedef struct {
    GPtrArray  *sheets;
    GHashTable *ranks;
    guint      *bases;
    guint      *stamps;
    guint      *slots;
    guint       epoch;
    guint       sheet;
    GArray     *matches;
    GHashTable *shared;
    GPtrArray  *owned;
    GPtrArray  *keyframe_decls;
    GHashTable *styles;
} svg_sheets;

static gboolean
svg_is_style_element(const ns_node *n)
{
    if (n->kind != NS_NODE_ELEMENT || !n->name ||
        strcmp(n->name, "style") != 0) return FALSE;
    const char *type = ns_element_get_attr(n, "type");
    return !type || !*type || g_ascii_strcasecmp(type, "text/css") == 0;
}

static gboolean
svg_style_media_matches(const ns_node *style)
{
    const char *media = ns_element_get_attr(style, "media");
    return !media || !*media || ns_css_media_query_matches(media);
}

static ns_css_stylesheet *
svg_style_element_sheet(const ns_node *style)
{
    GString *css = g_string_new(NULL);
    for (const ns_node *c = style->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_TEXT && c->text) g_string_append(css, c->text);
    ns_css_stylesheet *sheet = css->len > 0
        ? ns_css_stylesheet_parse_detached(css->str, (gssize)css->len) : NULL;
    g_string_free(css, TRUE);
    return sheet;
}

static GPtrArray *
svg_parse_sheets(const ns_node *root, double width, double height)
{
    GPtrArray *sheets = NULL;
    ns_css_media_viewport_push(width, height);
    for (const ns_node *n = root; n; n = ns_node_next_in_subtree(n, root, TRUE)) {
        if (!svg_is_style_element(n) || !svg_style_media_matches(n)) continue;
        ns_css_stylesheet *sheet = svg_style_element_sheet(n);
        if (!sheet) continue;
        if (!sheets)
            sheets = g_ptr_array_new_with_free_func(
                (GDestroyNotify)ns_css_stylesheet_free);
        g_ptr_array_add(sheets, sheet);
    }
    ns_css_media_viewport_pop();
    return sheets;
}

static void
svg_note_match(const ns_css_rule *rule, guint rule_index,
               const ns_css_selector *sel, gpointer data)
{
    svg_sheets *sh = data;
    guint order = sh->bases[sh->sheet] + rule_index;
    gint64 spec = ((gint64)sel->spec_a << 40) |
                  ((gint64)sel->spec_b << 20) | (gint64)sel->spec_c;
    if (sh->stamps[order] == sh->epoch) {
        svg_rule_match *m = &g_array_index(sh->matches, svg_rule_match,
                                           sh->slots[order]);
        m->specificity = MAX(m->specificity, spec);
        return;
    }
    sh->stamps[order] = sh->epoch;
    sh->slots[order] = sh->matches->len;
    svg_rule_match m = {
        rule, spec, ns_css_layer_rank(sh->ranks, rule->layer_name), order,
    };
    g_array_append_val(sh->matches, m);
}

static gint
svg_match_order_cmp(const svg_rule_match *a, const svg_rule_match *b)
{
    if (a->specificity != b->specificity)
        return a->specificity < b->specificity ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

static gint
svg_match_cmp(gconstpointer pa, gconstpointer pb)
{
    const svg_rule_match *a = pa, *b = pb;
    if (a->layer != b->layer) return a->layer < b->layer ? -1 : 1;
    return svg_match_order_cmp(a, b);
}

static gint
svg_match_important_cmp(gconstpointer pa, gconstpointer pb)
{
    const svg_rule_match *a = pa, *b = pb;
    if (a->layer != b->layer) return a->layer > b->layer ? -1 : 1;
    return svg_match_order_cmp(a, b);
}

static void
svg_apply_rule_decls(svg_cascaded *out, const ns_css_rule *rule,
                     gboolean important)
{
    for (guint i = 0; rule->decls && i < rule->decls->len; i++) {
        const ns_css_decl *d = &g_array_index(rule->decls, ns_css_decl, i);
        gboolean decl_important = d->important != 0;
        if (!d->value || decl_important != important) continue;
        out->style->values[d->prop] = d->value;
        if (important)
            out->important[d->prop / 64] |= G_GUINT64_CONSTANT(1) << (d->prop % 64);
    }
}

static const ns_css_keyframes *
svg_find_keyframes(const svg_sheets *sh, const char *name)
{
    for (guint s = sh->sheets->len; s > 0; s--) {
        const GArray *all = ((const ns_css_stylesheet *)
                             g_ptr_array_index(sh->sheets, s - 1))->keyframes;
        for (guint k = all ? all->len : 0; k > 0; k--) {
            const ns_css_keyframes *kf =
                &g_array_index(all, ns_css_keyframes, k - 1);
            if (kf->name && strcmp(kf->name, name) == 0) return kf;
        }
    }
    return NULL;
}

static double
svg_anim_directed_pct(const ns_css_anim_entry *e, double index, double progress)
{
    gboolean odd = fmod(index, 2.0) >= 1.0;
    gboolean reverse = e->direction == NS_CSS_ANIM_DIR_REVERSE ||
                       (e->direction == NS_CSS_ANIM_DIR_ALTERNATE && odd) ||
                       (e->direction == NS_CSS_ANIM_DIR_ALTERNATE_REVERSE && !odd);
    double p = reverse ? 1.0 - progress : progress;
    return p >= 0.5 ? 100.0 : 0.0;
}

static gboolean
svg_anim_start_pct(const ns_css_anim_entry *e, double *pct)
{
    if (e->delay_ms > 0 && e->fill != NS_CSS_ANIM_FILL_BACKWARDS &&
        e->fill != NS_CSS_ANIM_FILL_BOTH)
        return FALSE;
    *pct = svg_anim_directed_pct(e, 0, 0);
    return TRUE;
}

static gboolean
svg_anim_settled_pct(const ns_css_anim_entry *e, double *pct)
{
    if (e->paused || (isinf(e->iterations) && e->duration_ms > 0))
        return svg_anim_start_pct(e, pct);
    if (e->fill != NS_CSS_ANIM_FILL_FORWARDS && e->fill != NS_CSS_ANIM_FILL_BOTH)
        return FALSE;
    double n = isfinite(e->iterations) ? e->iterations : 1.0;
    double index = n > 0 ? ceil(n) - 1.0 : 0.0;
    *pct = svg_anim_directed_pct(e, index, n > 0 ? n - index : 0.0);
    return TRUE;
}

static void
svg_apply_keyframe_decls(svg_cascaded *c, const GArray *decls)
{
    for (guint i = 0; i < decls->len; i++) {
        const ns_css_decl *d = &g_array_index(decls, ns_css_decl, i);
        if (!d->value || d->important) continue;
        c->style->values[d->prop] = d->value;
        c->animated[d->prop / 64] |= G_GUINT64_CONSTANT(1) << (d->prop % 64);
    }
}

static void
svg_apply_keyframe(svg_sheets *sh, svg_cascaded *c, const ns_css_keyframes *kf,
                   double pct)
{
    for (int i = 0; i < kf->n_stops; i++) {
        const ns_css_keyframe_stop *stop = &kf->stops[i];
        if (fabs(stop->pct - pct) > 1e-9 || !stop->raw_props) continue;
        GArray *decls = ns_css_parse_declarations(stop->raw_props);
        if (!sh->keyframe_decls)
            sh->keyframe_decls = g_ptr_array_new_with_free_func(
                (GDestroyNotify)ns_css_declarations_free);
        g_ptr_array_add(sh->keyframe_decls, decls);
        svg_apply_keyframe_decls(c, decls);
    }
}

static void
svg_apply_animations(svg_sheets *sh, svg_cascaded *c)
{
    if (!c->style->values[NS_CSS_ANIMATION_NAME]) return;
    ns_css_anim_list list;
    ns_css_anim_effective(c->style, TRUE, &list);
    for (int i = 0; i < list.n; i++) {
        const ns_css_anim_entry *e = &list.entries[i];
        const ns_css_keyframes *kf = e->name ? svg_find_keyframes(sh, e->name)
                                             : NULL;
        double pct;
        if (kf && svg_anim_settled_pct(e, &pct)) svg_apply_keyframe(sh, c, kf, pct);
    }
    ns_css_anim_list_clear(&list);
}

static svg_cascaded *
svg_cascaded_build(svg_sheets *sh, GArray *matches)
{
    svg_cascaded *c = g_new0(svg_cascaded, 1);
    c->style = g_new0(ns_style, 1);
    for (guint i = 0; i < matches->len; i++)
        svg_apply_rule_decls(c, g_array_index(matches, svg_rule_match, i).rule,
                             FALSE);
    svg_apply_animations(sh, c);
    GArray *important = g_array_copy(matches);
    g_array_sort(important, svg_match_important_cmp);
    for (guint i = 0; i < important->len; i++)
        svg_apply_rule_decls(c, g_array_index(important, svg_rule_match, i).rule,
                             TRUE);
    g_array_free(important, TRUE);
    return c;
}

static const svg_cascaded *
svg_shared_style(svg_sheets *sh)
{
    guint n = sh->matches->len;
    const ns_css_rule **rules = g_new(const ns_css_rule *, n);
    for (guint i = 0; i < n; i++)
        rules[i] = g_array_index(sh->matches, svg_rule_match, i).rule;
    GBytes *key = g_bytes_new_take(rules, n * sizeof *rules);
    svg_cascaded *c = g_hash_table_lookup(sh->shared, key);
    if (c) {
        g_bytes_unref(key);
        return c;
    }
    c = svg_cascaded_build(sh, sh->matches);
    g_ptr_array_add(sh->owned, c->style);
    g_hash_table_insert(sh->shared, key, c);
    return c;
}

static gboolean
svg_inline_shadows(const ns_node *el, const svg_cascaded *c, ns_css_prop prop)
{
    gboolean inline_important;
    if (!c->style->values[prop] ||
        !svg_style_decl(el, ns_css_prop_name(prop), &inline_important))
        return FALSE;
    guint64 bit = G_GUINT64_CONSTANT(1) << (prop % 64);
    gboolean outranks = ((c->important[prop / 64] | c->animated[prop / 64]) &
                         bit) != 0;
    return inline_important || !outranks;
}

static ns_style *
svg_unshadowed_style(svg_sheets *sh, const ns_node *el, const svg_cascaded *c)
{
    ns_style *own = NULL;
    for (int prop = 0; prop < NS_CSS_PROP_COUNT; prop++) {
        if (!svg_inline_shadows(el, c, (ns_css_prop)prop)) continue;
        if (!own) {
            own = g_memdup2(c->style, sizeof *own);
            g_ptr_array_add(sh->owned, own);
        }
        own->values[prop] = NULL;
    }
    return own ? own : c->style;
}

static void
svg_cascade_element(svg_sheets *sh, const ns_node *el)
{
    sh->epoch++;
    g_array_set_size(sh->matches, 0);
    for (sh->sheet = 0; sh->sheet < sh->sheets->len; sh->sheet++)
        ns_css_stylesheet_each_match(g_ptr_array_index(sh->sheets, sh->sheet),
                                     el, svg_note_match, sh);
    if (sh->matches->len == 0) return;
    g_array_sort(sh->matches, svg_match_cmp);
    const svg_cascaded *c = svg_shared_style(sh);
    ns_style *style = ns_element_get_attr(el, "style")
        ? svg_unshadowed_style(sh, el, c) : c->style;
    g_hash_table_insert(sh->styles, (gpointer)el, style);
}

static void
svg_sheets_index(svg_sheets *sh)
{
    guint n = sh->sheets->len, total = 0;
    sh->ranks = ns_css_layer_ranks_new(
        (const ns_css_stylesheet *const *)sh->sheets->pdata, n);
    sh->bases = g_new(guint, n);
    for (guint i = 0; i < n; i++) {
        const ns_css_stylesheet *sheet = g_ptr_array_index(sh->sheets, i);
        sh->bases[i] = total;
        total += sheet->rules ? sheet->rules->len : 0;
    }
    sh->stamps = g_new0(guint, total + 1);
    sh->slots = g_new0(guint, total + 1);
    sh->matches = g_array_new(FALSE, FALSE, sizeof(svg_rule_match));
    sh->shared = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                       (GDestroyNotify)g_bytes_unref, g_free);
    sh->owned = g_ptr_array_new_with_free_func(g_free);
    sh->styles = g_hash_table_new(NULL, NULL);
}

static void
svg_sheets_build(svg_sheets *sh, const ns_node *root, double width,
                 double height)
{
    memset(sh, 0, sizeof *sh);
    sh->sheets = svg_parse_sheets(root, width, height);
    if (!sh->sheets) return;
    svg_sheets_index(sh);
    ns_css_selector_batch_begin();
    int count = 0;
    for (const ns_node *n = root; n && count < NS_SVG_MAX_NODES;
         n = ns_node_next_in_subtree(n, root, TRUE), count++)
        if (n->kind == NS_NODE_ELEMENT) svg_cascade_element(sh, n);
    ns_css_selector_batch_end();
    g_clear_pointer(&sh->shared, g_hash_table_destroy);
    g_clear_pointer(&sh->matches, g_array_unref);
    g_clear_pointer(&sh->stamps, g_free);
    g_clear_pointer(&sh->slots, g_free);
    g_clear_pointer(&sh->bases, g_free);
    g_clear_pointer(&sh->ranks, g_hash_table_destroy);
}

static void
svg_sheets_free(svg_sheets *sh)
{
    if (sh->styles) g_hash_table_destroy(sh->styles);
    if (sh->owned) g_ptr_array_free(sh->owned, TRUE);
    if (sh->keyframe_decls) g_ptr_array_free(sh->keyframe_decls, TRUE);
    if (sh->sheets) g_ptr_array_free(sh->sheets, TRUE);
}

static const ns_node *
svg_document_element(ns_node *doc, const ns_node *root)
{
    if (root->parent == doc) return root;
    ns_node *el = (ns_node *)root;
    ns_node_remove(el);
    ns_node_append_child(doc, el);
    return el;
}

static void
svg_render_document(cairo_t *cr, ns_node *doc, const ns_node *root,
                    double width, double height)
{
    root = svg_document_element(doc, root);
    svg_sheets sheets;
    svg_sheets_build(&sheets, root, width, height);
    svg_render_root(cr, root, width, height, sheets.styles, NULL, TRUE);
    svg_sheets_free(&sheets);
}

struct ns_svg_doc {
    ns_node       *doc;
    const ns_node *root;
};

ns_svg_doc *
ns_svg_doc_parse(const guchar *data, gsize len)
{
    if (!data || len == 0 || len > NS_SVG_MAX_INPUT_BYTES) return NULL;
    if (!ns_svg_bytes_look_like_svg(data, len)) return NULL;
    ns_node *doc = ns_html_parse((const char *)data, (gssize)len);
    if (!doc) return NULL;
    const ns_node *root = svg_find_root(doc);
    if (!root) {
        ns_node_free(doc);
        return NULL;
    }
    ns_svg_doc *d = g_new(ns_svg_doc, 1);
    d->doc = doc;
    d->root = root;
    return d;
}

gboolean
ns_svg_doc_render(cairo_t *cr, ns_svg_doc *d, double width, double height)
{
    if (!cr || !d || width <= 0 || height <= 0) return FALSE;
    svg_render_document(cr, d->doc, d->root, width, height);
    return TRUE;
}

void
ns_svg_doc_free(ns_svg_doc *d)
{
    if (!d) return;
    ns_node_free(d->doc);
    g_free(d);
}

gboolean
ns_svg_render_bytes(cairo_t *cr, const guchar *data, gsize len,
                    double width, double height)
{
    if (!cr || width <= 0 || height <= 0) return FALSE;
    ns_svg_doc *d = ns_svg_doc_parse(data, len);
    gboolean drawn = ns_svg_doc_render(cr, d, width, height);
    ns_svg_doc_free(d);
    return drawn;
}

ns_texture *
ns_svg_decode_bytes(const guchar *data, gsize len, int *out_w, int *out_h)
{
    if (!data || len == 0 || len > NS_SVG_MAX_INPUT_BYTES) return NULL;
    if (!ns_svg_bytes_look_like_svg(data, len)) return NULL;

    ns_node *doc = ns_html_parse((const char *)data, (gssize)len);
    if (!doc) return NULL;
    const ns_node *root = svg_find_root(doc);
    if (!root) {
        ns_node_free(doc);
        return NULL;
    }

    ns_svg_size size;
    ns_svg_intrinsic_size(root, &size);
    double w = size.has_width  ? size.width  : NS_SVG_DEFAULT_OBJECT_W;
    double h = size.has_height ? size.height : NS_SVG_DEFAULT_OBJECT_H;
    if (!size.has_width && !size.has_height &&
        size.has_ratio && size.ratio > 0) {
        double k = MIN(NS_SVG_DEFAULT_OBJECT_W / size.ratio,
                       NS_SVG_DEFAULT_OBJECT_H);
        w = size.ratio * k;
        h = k;
    }
    if (!isfinite(w) || w <= 0) w = NS_SVG_DEFAULT_DIM_PX;
    if (!isfinite(h) || h <= 0) h = NS_SVG_DEFAULT_DIM_PX;
    if (w > NS_SVG_MAX_DIM_PX || h > NS_SVG_MAX_DIM_PX) {
        double s = (double)NS_SVG_MAX_DIM_PX / MAX(w, h);
        w *= s; h *= s;
    }
    if (w * h > (double)NS_SVG_MAX_PIXELS) {
        double s = sqrt((double)NS_SVG_MAX_PIXELS / (w * h));
        w *= s; h *= s;
    }
    int iw = (int)CLAMP(ceil(w - 0.001), 1.0, (double)NS_SVG_MAX_DIM_PX);
    int ih = (int)CLAMP(ceil(h - 0.001), 1.0, (double)NS_SVG_MAX_DIM_PX);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        ns_node_free(doc);
        return NULL;
    }
    cairo_t *cr = cairo_create(surf);
    svg_render_document(cr, doc, root, iw, ih);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    ns_node_free(doc);

    int stride = cairo_image_surface_get_stride(surf);
    const guchar *pixels = cairo_image_surface_get_data(surf);
    if (!pixels) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    GBytes *bytes = g_bytes_new_with_free_func(
        pixels, (gsize)stride * (gsize)ih,
        (GDestroyNotify)cairo_surface_destroy, surf);
    ns_texture *tex = ns_texture_new(iw, ih, NS_TEXTURE_DEFAULT, bytes,
                                     (gsize)stride);
    g_bytes_unref(bytes);
    if (!tex) return NULL;
    if (out_w) *out_w = iw;
    if (out_h) *out_h = ih;
    return tex;
}
