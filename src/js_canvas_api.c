/* Nordstjernen — the WebIDL surface of the canvas objects: interfaces, hidden state, attributes (QuickJS).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */
#include "js_internal.h"
#include "js_classid.h"

#include <math.h>
#include <string.h>

#include "css.h"

static char *ns_canvas_font_string(const char *css);
static void ns_offscreen_sync_size(JSContext *ctx, JSValueConst obj);
static JSValue ns_imagedata_construct(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv);
static JSValue ns_offscreen_construct(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv);

static JSClassID ns_hidden_class_id;

typedef struct ns_hidden {
    int      kind;
    JSValue  state;
    gpointer ptr;
} ns_hidden;

static void
ns_hidden_finalizer(JSRuntime *rt, JSValue val)
{
    ns_hidden *h = JS_GetOpaque(val, ns_hidden_class_id);
    if (!h) return;
    JS_FreeValueRT(rt, h->state);
    g_free(h);
}

static void
ns_hidden_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_hidden *h = JS_GetOpaque(val, ns_hidden_class_id);
    if (h) JS_MarkValue(rt, h->state, mark_func);
}

static JSClassDef ns_hidden_class = {
    "CanvasObject",
    .finalizer = ns_hidden_finalizer,
    .gc_mark = ns_hidden_gc_mark,
};

static void
ns_canvas_register_hidden_class(JSRuntime *rt)
{
    ns_new_class_id(&ns_hidden_class_id);
    if (!JS_IsRegisteredClass(rt, ns_hidden_class_id))
        JS_NewClass(rt, ns_hidden_class_id, &ns_hidden_class);
}

JSValue
ns_hidden_new(JSContext *realm, int kind, JSValueConst proto)
{
    JSValue obj = JS_IsObject(proto)
        ? JS_NewObjectProtoClass(realm, proto, ns_hidden_class_id)
        : JS_NewObjectClass(realm, ns_hidden_class_id);
    if (JS_IsException(obj)) return obj;
    ns_hidden *h = g_new0(ns_hidden, 1);
    h->kind = kind;
    h->state = JS_NewObjectProto(realm, JS_NULL);
    JS_SetOpaque(obj, h);
    return obj;
}

gboolean
ns_hidden_is(JSValueConst v, int kind)
{
    ns_hidden *h = JS_GetOpaque(v, ns_hidden_class_id);
    return h && h->kind == kind;
}

gpointer
ns_hidden_ptr(JSValueConst v)
{
    ns_hidden *h = JS_GetOpaque(v, ns_hidden_class_id);
    return h ? h->ptr : NULL;
}

static void
ns_hidden_set_ptr(JSValueConst v, gpointer ptr)
{
    ns_hidden *h = JS_GetOpaque(v, ns_hidden_class_id);
    if (h) h->ptr = ptr;
}

JSValue
ns_hget(JSContext *ctx, JSValueConst obj, const char *name)
{
    ns_hidden *h = JS_GetOpaque(obj, ns_hidden_class_id);
    return h ? JS_GetPropertyStr(ctx, h->state, name) : JS_UNDEFINED;
}

void
ns_hset(JSContext *ctx, JSValueConst obj, const char *name, JSValue val)
{
    ns_hidden *h = JS_GetOpaque(obj, ns_hidden_class_id);
    if (h) JS_SetPropertyStr(ctx, h->state, name, val);
    else JS_FreeValue(ctx, val);
}

gboolean
ns_ctx2d_is(JSValueConst v)
{
    return ns_hidden_is(v, NS_HK_CTX2D) || ns_hidden_is(v, NS_HK_OFFSCREEN_CTX2D);
}

JSContext *
ns_canvas_realm(JSContext *ctx, const ns_node *el)
{
    JSContext *realm = js_from_ctx(ctx) && el
        ? ns_js_realm_for_node(js_from_ctx(ctx), el) : NULL;
    return realm ? realm : ctx;
}

JSValue
ns_api_proto(JSContext *realm, const char *iface)
{
    JSValue global = JS_GetGlobalObject(realm);
    JSValue ctor = JS_GetPropertyStr(realm, global, iface);
    JS_FreeValue(realm, global);
    JSValue proto = JS_IsObject(ctor) ? JS_GetPropertyStr(realm, ctor, "prototype")
                                      : JS_UNDEFINED;
    JS_FreeValue(realm, ctor);
    return proto;
}

JSValue
ns_api_proto_of_ctor(JSContext *ctx, JSValueConst new_target, const char *iface)
{
    if (JS_IsObject(new_target)) {
        JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsObject(proto)) return proto;
        JS_FreeValue(ctx, proto);
    }
    return ns_api_proto(ctx, iface);
}

JSValue
ns_api_throw_new_required(JSContext *ctx, const char *iface)
{
    return JS_ThrowTypeError(ctx,
        "Failed to construct '%s': Please use the 'new' operator, this DOM "
        "object constructor cannot be called as a function.", iface);
}

typedef struct ns_attr_def {
    const char *name;
    int         type;
    const char *values;
} ns_attr_def;

enum {
    NS_AT_READONLY,
    NS_AT_FINITE,
    NS_AT_POSITIVE,
    NS_AT_NONNEGATIVE,
    NS_AT_ALPHA,
    NS_AT_BOOL,
    NS_AT_ENUM,
    NS_AT_STYLE,
    NS_AT_COLOR,
    NS_AT_FONT,
    NS_AT_FILTER,
    NS_AT_LENGTH,
    NS_AT_STRING,
    NS_AT_SIZE,
    NS_AT_HANDLER,
};

typedef struct ns_api_method {
    const char  *name;
    JSCFunction *fn;
    int          length;
} ns_api_method;

typedef struct ns_api_table {
    const char          *iface;
    gboolean           (*brand)(JSValueConst v);
    const ns_attr_def   *attrs;
    guint                n_attrs;
    const ns_api_method *methods;
    guint                n_methods;
} ns_api_table;

static gboolean
ns_brand_gradient(JSValueConst v)
{
    return ns_hidden_is(v, NS_HK_GRADIENT);
}

static gboolean
ns_brand_pattern(JSValueConst v)
{
    return ns_hidden_is(v, NS_HK_PATTERN);
}

static gboolean
ns_brand_imagedata(JSValueConst v)
{
    return ns_hidden_is(v, NS_HK_IMAGEDATA);
}

static gboolean
ns_brand_textmetrics(JSValueConst v)
{
    return ns_hidden_is(v, NS_HK_TEXTMETRICS);
}

static gboolean
ns_brand_offscreen(JSValueConst v)
{
    return ns_hidden_is(v, NS_HK_OFFSCREEN);
}

static gboolean
ns_brand_window_ctx2d(JSValueConst v)
{
    return ns_hidden_is(v, NS_HK_CTX2D);
}

static gboolean
ns_brand_offscreen_ctx2d(JSValueConst v)
{
    return ns_hidden_is(v, NS_HK_OFFSCREEN_CTX2D);
}

static const ns_attr_def ns_ctx2d_attrs[] = {
    { "canvas", NS_AT_READONLY, NULL },
    { "direction", NS_AT_ENUM, "inherit ltr rtl" },
    { "fillStyle", NS_AT_STYLE, NULL },
    { "filter", NS_AT_FILTER, NULL },
    { "font", NS_AT_FONT, NULL },
    { "fontKerning", NS_AT_ENUM, "auto normal none" },
    { "fontStretch", NS_AT_ENUM,
      "ultra-condensed extra-condensed condensed semi-condensed normal "
      "semi-expanded expanded extra-expanded ultra-expanded" },
    { "fontVariantCaps", NS_AT_ENUM,
      "normal small-caps all-small-caps petite-caps all-petite-caps unicase "
      "titling-caps" },
    { "globalAlpha", NS_AT_ALPHA, NULL },
    { "globalCompositeOperation", NS_AT_ENUM,
      "source-over source-in source-out source-atop destination-over "
      "destination-in destination-out destination-atop lighter copy xor clear "
      "multiply screen overlay darken lighten color-dodge color-burn "
      "hard-light soft-light difference exclusion hue saturation color "
      "luminosity" },
    { "imageSmoothingEnabled", NS_AT_BOOL, NULL },
    { "imageSmoothingQuality", NS_AT_ENUM, "low medium high" },
    { "lang", NS_AT_STRING, NULL },
    { "letterSpacing", NS_AT_LENGTH, NULL },
    { "lineCap", NS_AT_ENUM, "butt round square" },
    { "lineDashOffset", NS_AT_FINITE, NULL },
    { "lineJoin", NS_AT_ENUM, "round bevel miter" },
    { "lineWidth", NS_AT_POSITIVE, NULL },
    { "miterLimit", NS_AT_POSITIVE, NULL },
    { "shadowBlur", NS_AT_NONNEGATIVE, NULL },
    { "shadowColor", NS_AT_COLOR, NULL },
    { "shadowOffsetX", NS_AT_FINITE, NULL },
    { "shadowOffsetY", NS_AT_FINITE, NULL },
    { "strokeStyle", NS_AT_STYLE, NULL },
    { "textAlign", NS_AT_ENUM, "start end left right center" },
    { "textBaseline", NS_AT_ENUM,
      "top hanging middle alphabetic ideographic bottom" },
    { "textRendering", NS_AT_ENUM,
      "auto optimizeSpeed optimizeLegibility geometricPrecision" },
    { "wordSpacing", NS_AT_LENGTH, NULL },
};

static const ns_api_method ns_ctx2d_methods[] = {
    { "arc", ns_ctx_arc, 5 },
    { "arcTo", ns_ctx_arcTo, 5 },
    { "beginLayer", ns_ctx_beginLayer, 0 },
    { "beginPath", ns_ctx_beginPath, 0 },
    { "bezierCurveTo", ns_ctx_bezierCurveTo, 6 },
    { "clearRect", ns_ctx_clearRect, 4 },
    { "clip", ns_ctx_clip, 0 },
    { "closePath", ns_ctx_closePath, 0 },
    { "createConicGradient", ns_ctx_createConicGradient, 3 },
    { "createImageData", ns_ctx_createImageData, 1 },
    { "createLinearGradient", ns_ctx_createLinearGradient, 4 },
    { "createPattern", ns_ctx_createPattern, 2 },
    { "createRadialGradient", ns_ctx_createRadialGradient, 6 },
    { "drawFocusIfNeeded", ns_ctx_draw_focus_if_needed, 1 },
    { "drawImage", ns_ctx_drawImage, 3 },
    { "ellipse", ns_ctx_ellipse, 7 },
    { "endLayer", ns_ctx_endLayer, 0 },
    { "fill", ns_ctx_fill, 0 },
    { "fillRect", ns_ctx_fillRect, 4 },
    { "fillText", ns_ctx_fillText, 3 },
    { "getContextAttributes", ns_ctx_get_attrs, 0 },
    { "getImageData", ns_ctx_getImageData, 4 },
    { "getLineDash", ns_ctx_getLineDash, 0 },
    { "getTransform", ns_ctx_getTransform, 0 },
    { "isContextLost", ns_ctx_is_context_lost, 0 },
    { "isPointInPath", ns_ctx_isPointInPath, 2 },
    { "isPointInStroke", ns_ctx_isPointInStroke, 2 },
    { "lineTo", ns_ctx_lineTo, 2 },
    { "measureText", ns_ctx_measureText, 1 },
    { "moveTo", ns_ctx_moveTo, 2 },
    { "putImageData", ns_ctx_putImageData, 3 },
    { "quadraticCurveTo", ns_ctx_quadraticCurveTo, 4 },
    { "rect", ns_ctx_rect, 4 },
    { "reset", ns_ctx_reset, 0 },
    { "resetTransform", ns_ctx_resetTransform, 0 },
    { "restore", ns_ctx_restore, 0 },
    { "rotate", ns_ctx_rotate, 1 },
    { "roundRect", ns_ctx_roundRect, 4 },
    { "save", ns_ctx_save, 0 },
    { "scale", ns_ctx_scale, 2 },
    { "setLineDash", ns_ctx_setLineDash, 1 },
    { "setTransform", ns_ctx_setTransform, 0 },
    { "stroke", ns_ctx_stroke, 0 },
    { "strokeRect", ns_ctx_strokeRect, 4 },
    { "strokeText", ns_ctx_strokeText, 3 },
    { "transform", ns_ctx_transform, 6 },
    { "translate", ns_ctx_translate, 2 },
};

static const ns_api_method ns_gradient_methods[] = {
    { "addColorStop", ns_ctx_gradient_addColorStop, 2 },
};

static gboolean
ns_matrix_member_equal(double a, double b)
{
    return a == b || (a != a && b != b);
}

static int
ns_matrix_member_value(JSContext *ctx, JSValueConst alias, JSValueConst field,
                       const char *alias_name, const char *field_name, double *out)
{
    double da = 0, df = 0;
    gboolean has_alias = !JS_IsUndefined(alias), has_field = !JS_IsUndefined(field);
    if (has_alias && JS_ToFloat64(ctx, &da, alias) < 0) return -1;
    if (has_field && JS_ToFloat64(ctx, &df, field) < 0) return -1;
    if (has_alias && has_field && !ns_matrix_member_equal(da, df)) {
        JS_ThrowTypeError(ctx, "The '%s' and '%s' members must be equal.",
                          alias_name, field_name);
        return -1;
    }
    if (has_alias) *out = da;
    else if (has_field) *out = df;
    return 0;
}

static int
ns_matrix_init_member(JSContext *ctx, JSValueConst init, int i, double *out)
{
    static const char *const aliases[6] = { "a", "b", "c", "d", "e", "f" };
    static const char *const fields[6] = { "m11", "m12", "m21", "m22", "m41", "m42" };
    JSValue alias = JS_GetPropertyStr(ctx, init, aliases[i]);
    JSValue field = JS_GetPropertyStr(ctx, init, fields[i]);
    int ret = ns_matrix_member_value(ctx, alias, field, aliases[i], fields[i], out);
    JS_FreeValue(ctx, alias);
    JS_FreeValue(ctx, field);
    return ret;
}

static JSValue
ns_pattern_setTransform(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    double m[6] = { 1, 0, 0, 1, 0, 0 };
    if (argc >= 1 && JS_IsObject(argv[0])) {
        for (int i = 0; i < 6; i++)
            if (ns_matrix_init_member(ctx, argv[0], i, &m[i]) < 0) return JS_EXCEPTION;
    }
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < 6; i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewFloat64(ctx, m[i]));
    ns_hset(ctx, this_val, "_matrix", arr);
    return JS_UNDEFINED;
}

static const ns_api_method ns_pattern_methods[] = {
    { "setTransform", ns_pattern_setTransform, 0 },
};

static const ns_attr_def ns_imagedata_attrs[] = {
    { "colorSpace", NS_AT_READONLY, NULL },
    { "data", NS_AT_READONLY, NULL },
    { "height", NS_AT_READONLY, NULL },
    { "pixelFormat", NS_AT_READONLY, NULL },
    { "width", NS_AT_READONLY, NULL },
};

static const ns_attr_def ns_textmetrics_attrs[] = {
    { "actualBoundingBoxAscent", NS_AT_READONLY, NULL },
    { "actualBoundingBoxDescent", NS_AT_READONLY, NULL },
    { "actualBoundingBoxLeft", NS_AT_READONLY, NULL },
    { "actualBoundingBoxRight", NS_AT_READONLY, NULL },
    { "alphabeticBaseline", NS_AT_READONLY, NULL },
    { "fontBoundingBoxAscent", NS_AT_READONLY, NULL },
    { "fontBoundingBoxDescent", NS_AT_READONLY, NULL },
    { "hangingBaseline", NS_AT_READONLY, NULL },
    { "ideographicBaseline", NS_AT_READONLY, NULL },
    { "width", NS_AT_READONLY, NULL },
};

static const ns_attr_def ns_offscreen_attrs[] = {
    { "height", NS_AT_SIZE, NULL },
    { "oncontextlost", NS_AT_HANDLER, NULL },
    { "oncontextrestored", NS_AT_HANDLER, NULL },
    { "width", NS_AT_SIZE, NULL },
};

static const ns_api_method ns_path2d_methods[] = {
    { "addPath", ns_path2d_addPath, 1 },
    { "arc", ns_path2d_arc, 5 },
    { "arcTo", ns_path2d_arcTo, 5 },
    { "bezierCurveTo", ns_path2d_bezierCurveTo, 6 },
    { "closePath", ns_path2d_closePath, 0 },
    { "ellipse", ns_path2d_ellipse, 7 },
    { "lineTo", ns_path2d_lineTo, 2 },
    { "moveTo", ns_path2d_moveTo, 2 },
    { "quadraticCurveTo", ns_path2d_quadraticCurveTo, 4 },
    { "rect", ns_path2d_rect, 4 },
    { "roundRect", ns_path2d_roundRect, 4 },
};

static const ns_api_method ns_image_bitmap_methods[] = {
    { "close", ns_image_bitmap_close, 0 },
};

static const ns_api_method ns_offscreen_methods[] = {
    { "convertToBlob", ns_offscreen_convertToBlob, 0 },
    { "getContext", ns_offscreen_getContext, 1 },
    { "transferToImageBitmap", ns_offscreen_transferToImageBitmap, 0 },
};

enum {
    NS_TBL_CTX2D,
    NS_TBL_OFFSCREEN_CTX2D,
    NS_TBL_GRADIENT,
    NS_TBL_PATTERN,
    NS_TBL_IMAGEDATA,
    NS_TBL_TEXTMETRICS,
    NS_TBL_OFFSCREEN,
    NS_TBL_PATH2D,
    NS_TBL_IMAGEBITMAP,
    NS_TBL_COUNT,
};

static const ns_api_table ns_api_tables[NS_TBL_COUNT] = {
    [NS_TBL_CTX2D] = { "CanvasRenderingContext2D", ns_brand_window_ctx2d,
        ns_ctx2d_attrs, G_N_ELEMENTS(ns_ctx2d_attrs),
        ns_ctx2d_methods, G_N_ELEMENTS(ns_ctx2d_methods) },
    [NS_TBL_OFFSCREEN_CTX2D] = { "OffscreenCanvasRenderingContext2D",
        ns_brand_offscreen_ctx2d,
        ns_ctx2d_attrs, G_N_ELEMENTS(ns_ctx2d_attrs),
        ns_ctx2d_methods, G_N_ELEMENTS(ns_ctx2d_methods) },
    [NS_TBL_GRADIENT] = { "CanvasGradient", ns_brand_gradient, NULL, 0,
        ns_gradient_methods, G_N_ELEMENTS(ns_gradient_methods) },
    [NS_TBL_PATTERN] = { "CanvasPattern", ns_brand_pattern, NULL, 0,
        ns_pattern_methods, G_N_ELEMENTS(ns_pattern_methods) },
    [NS_TBL_IMAGEDATA] = { "ImageData", ns_brand_imagedata,
        ns_imagedata_attrs, G_N_ELEMENTS(ns_imagedata_attrs), NULL, 0 },
    [NS_TBL_TEXTMETRICS] = { "TextMetrics", ns_brand_textmetrics,
        ns_textmetrics_attrs, G_N_ELEMENTS(ns_textmetrics_attrs), NULL, 0 },
    [NS_TBL_OFFSCREEN] = { "OffscreenCanvas", ns_brand_offscreen,
        ns_offscreen_attrs, G_N_ELEMENTS(ns_offscreen_attrs),
        ns_offscreen_methods, G_N_ELEMENTS(ns_offscreen_methods) },
    [NS_TBL_PATH2D] = { "Path2D", ns_value_is_path2d, NULL, 0,
        ns_path2d_methods, G_N_ELEMENTS(ns_path2d_methods) },
    [NS_TBL_IMAGEBITMAP] = { "ImageBitmap", ns_image_bitmap_is, NULL, 0,
        ns_image_bitmap_methods, G_N_ELEMENTS(ns_image_bitmap_methods) },
};

#define NS_MAGIC_METHOD 0
#define NS_MAGIC_GETTER 1
#define NS_MAGIC_SETTER 2
#define NS_MAGIC(table, kind, index) (((table) << 10) | ((kind) << 8) | (index))
#define NS_MAGIC_TABLE(magic) (&ns_api_tables[(magic) >> 10])
#define NS_MAGIC_INDEX(magic) ((magic) & 0xff)

static JSValue
ns_api_call(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
            int magic)
{
    const ns_api_table *t = NS_MAGIC_TABLE(magic);
    const ns_api_method *m = &t->methods[NS_MAGIC_INDEX(magic)];
    if (!t->brand(this_val)) return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (argc < m->length)
        return JS_ThrowTypeError(ctx,
            "Failed to execute '%s' on '%s': %d argument%s required, but only %d present.",
            m->name, t->iface, m->length, m->length == 1 ? "" : "s", argc);
    return m->fn(ctx, this_val, argc, argv);
}

static void
ns_attr_sync_canvas(JSContext *ctx, JSValueConst this_val)
{
    if (ns_ctx2d_is(this_val) && js_from_ctx(ctx))
        ns_canvas_state_for(js_from_ctx(ctx), ns_hidden_ptr(this_val));
}

static JSValue
ns_attr_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
            int magic)
{
    (void)argc; (void)argv;
    const ns_api_table *t = NS_MAGIC_TABLE(magic);
    if (!t->brand(this_val)) return JS_ThrowTypeError(ctx, "Illegal invocation");
    ns_attr_sync_canvas(ctx, this_val);
    return ns_hget(ctx, this_val, t->attrs[NS_MAGIC_INDEX(magic)].name);
}

static gboolean
ns_enum_has(const char *values, const char *s)
{
    gsize n = strlen(s);
    for (const char *p = values; *p; ) {
        const char *end = strchr(p, ' ');
        gsize len = end ? (gsize)(end - p) : strlen(p);
        if (len == n && memcmp(p, s, n) == 0) return TRUE;
        if (!end) break;
        p = end + 1;
    }
    return FALSE;
}

static char *
ns_color_serialize(guint8 r, guint8 g, guint8 b, guint8 a)
{
    if (a == 255) return g_strdup_printf("#%02x%02x%02x", r, g, b);
    double alpha = round(a / 255.0 * 100.0) / 100.0;
    if ((int)round(alpha * 255.0) != a) alpha = round(a / 255.0 * 1000.0) / 1000.0;
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof buf, "%g", alpha);
    return g_strdup_printf("rgba(%u, %u, %u, %s)", r, g, b, buf);
}

static const char *const ns_color_function_names[] = {
    "color", "lab", "lch", "oklab", "oklch",
};

static int
ns_color_function_index(const char *name, gsize name_len)
{
    for (gsize i = 0; i < G_N_ELEMENTS(ns_color_function_names); i++)
        if (strlen(ns_color_function_names[i]) == name_len &&
            g_ascii_strncasecmp(name, ns_color_function_names[i], name_len) == 0)
            return (int)i;
    return -1;
}

static char **
ns_color_function_tokens(const char *open, const char *close)
{
    char *body = g_strndup(open + 1, (gsize)(close - open - 1));
    g_strdelimit(body, "\t\n\r", ' ');
    GString *spaced = g_string_new(NULL);
    for (const char *q = body; *q; q++) {
        if (*q == '/') g_string_append(spaced, " / ");
        else g_string_append_c(spaced, *q);
    }
    g_free(body);
    char **tokens = g_strsplit_set(spaced->str, " ", -1);
    g_string_free(spaced, TRUE);
    return tokens;
}

static void
ns_color_append_number(GString *out, double v)
{
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof buf, "%g", v);
    g_string_append(out, buf);
}

static void
ns_color_function_append_component(GString *out, const char *token, int fn,
                                   gboolean first)
{
    if (fn >= 1 && first && g_str_has_suffix(token, "%"))
        ns_color_append_number(out, g_ascii_strtod(token, NULL) * (fn >= 3 ? 0.01 : 1.0));
    else
        g_string_append(out, token);
}

static char *
ns_color_function_append_tokens(GString *out, char **tokens, int fn)
{
    gboolean slash = FALSE, first = TRUE;
    for (char **t = tokens; *t; t++) {
        if (!**t) continue;
        if (strcmp(*t, "/") == 0) { slash = TRUE; continue; }
        if (slash) return g_strdup(*t);
        if (!first) g_string_append_c(out, ' ');
        ns_color_function_append_component(out, *t, fn, first);
        first = FALSE;
    }
    return NULL;
}

static void
ns_color_function_append_alpha(GString *out, const char *alpha)
{
    double v = g_ascii_strtod(alpha, NULL) * (g_str_has_suffix(alpha, "%") ? 0.01 : 1.0);
    if (v < 1) {
        g_string_append(out, " / ");
        ns_color_append_number(out, v < 0 ? 0 : v);
    }
}

static char *
ns_color_function_string(const char *css)
{
    const char *p = css;
    while (g_ascii_isspace(*p)) p++;
    const char *open = strchr(p, '(');
    const char *close = strrchr(p, ')');
    if (!open || !close || close < open) return NULL;
    int fn = ns_color_function_index(p, (gsize)(open - p));
    if (fn < 0) return NULL;
    char **tokens = ns_color_function_tokens(open, close);
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "%s(", ns_color_function_names[fn]);
    char *alpha = ns_color_function_append_tokens(out, tokens, fn);
    if (alpha) {
        ns_color_function_append_alpha(out, alpha);
        g_free(alpha);
    }
    g_string_append_c(out, ')');
    g_strfreev(tokens);
    return g_string_free(out, FALSE);
}

typedef struct {
    char css[48];
    char out[48];
} ns_canvas_color_string_memo;

static __thread ns_canvas_color_string_memo g_canvas_color_string_memo[16];
static __thread guint g_canvas_color_string_memo_next;

static char *
ns_canvas_color_string_uncached(const char *css)
{
    double r, g, b, a;
    if (!ns_canvas_parse_color(css, &r, &g, &b, &a)) return NULL;
    char *function = ns_color_function_string(css);
    return function ? function
        : ns_color_serialize((guint8)round(r * 255), (guint8)round(g * 255),
                             (guint8)round(b * 255), (guint8)round(a * 255));
}

static char *
ns_canvas_color_string(const char *css)
{
    if (!css) return NULL;
    gboolean memo = strlen(css) < sizeof g_canvas_color_string_memo[0].css &&
                    ns_canvas_color_is_literal(css);
    if (memo) {
        for (guint i = 0; i < G_N_ELEMENTS(g_canvas_color_string_memo); i++) {
            const ns_canvas_color_string_memo *m = &g_canvas_color_string_memo[i];
            if (strcmp(m->css, css) == 0) return g_strdup(m->out);
        }
    }
    char *out = ns_canvas_color_string_uncached(css);
    if (memo && out && strlen(out) < sizeof g_canvas_color_string_memo[0].out) {
        ns_canvas_color_string_memo *m = &g_canvas_color_string_memo[
            g_canvas_color_string_memo_next++ %
            G_N_ELEMENTS(g_canvas_color_string_memo)];
        g_strlcpy(m->css, css, sizeof m->css);
        g_strlcpy(m->out, out, sizeof m->out);
    }
    return out;
}

static gboolean
ns_length_valid(const char *s)
{
    static const char *const units[] = {
        "px", "em", "rem", "ex", "ch", "pt", "pc", "cm", "mm", "in", "q",
        "vw", "vh", "vmin", "vmax",
    };
    char *end = NULL;
    (void)g_ascii_strtod(s, &end);
    if (end == s || (*s != '+' && *s != '-' && *s != '.' && !g_ascii_isdigit(*s)))
        return FALSE;
    if (!*end) return strtod(s, NULL) == 0;
    for (gsize i = 0; i < G_N_ELEMENTS(units); i++)
        if (g_ascii_strcasecmp(end, units[i]) == 0) return TRUE;
    return FALSE;
}

static JSValue
ns_set_string(JSContext *ctx, ns_hidden *h, const char *name, const char *s)
{
    JS_SetPropertyStr(ctx, h->state, name, JS_NewString(ctx, s));
    return JS_UNDEFINED;
}

static char *
ns_canvas_color_for(JSContext *ctx, JSValueConst owner, const char *css)
{
    if (g_ascii_strcasecmp(css, "currentcolor") != 0) return ns_canvas_color_string(css);
    const ns_node *el = ns_hidden_ptr(owner);
    char *computed = el ? ns_js_computed_text(ctx, el, "color") : NULL;
    char *color = computed ? ns_canvas_color_string(computed) : NULL;
    g_free(computed);
    return color ? color : g_strdup("#000000");
}

static JSValue
ns_assign_style(JSContext *ctx, JSValueConst owner, ns_hidden *h, const ns_attr_def *a,
                JSValueConst v)
{
    if (ns_hidden_is(v, NS_HK_GRADIENT) || ns_hidden_is(v, NS_HK_PATTERN)) {
        JS_SetPropertyStr(ctx, h->state, a->name, JS_DupValue(ctx, v));
        return JS_UNDEFINED;
    }
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_EXCEPTION;
    char *color = ns_canvas_color_for(ctx, owner, s);
    JS_FreeCString(ctx, s);
    if (color) ns_set_string(ctx, h, a->name, color);
    g_free(color);
    return JS_UNDEFINED;
}

static char *
ns_assign_string_value(JSContext *ctx, JSValueConst owner, const ns_attr_def *a,
                       const char *s)
{
    switch (a->type) {
    case NS_AT_COLOR:  return ns_canvas_color_for(ctx, owner, s);
    case NS_AT_FONT:   return ns_canvas_font_string(s);
    case NS_AT_FILTER:
        return ns_css_filter_list_valid(s) ? g_strstrip(g_strdup(s)) : NULL;
    case NS_AT_LENGTH: return ns_length_valid(s) ? g_ascii_strdown(s, -1) : NULL;
    case NS_AT_ENUM:   return ns_enum_has(a->values, s) ? g_strdup(s) : NULL;
    default:           return g_strdup(s);
    }
}

static JSValue
ns_assign_string(JSContext *ctx, JSValueConst owner, ns_hidden *h, const ns_attr_def *a,
                 JSValueConst v)
{
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_EXCEPTION;
    char *value = ns_assign_string_value(ctx, owner, a, s);
    JS_FreeCString(ctx, s);
    if (value) ns_set_string(ctx, h, a->name, value);
    g_free(value);
    return JS_UNDEFINED;
}

static JSValue
ns_assign_number(JSContext *ctx, ns_hidden *h, const ns_attr_def *a, JSValueConst v)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v) < 0) return JS_EXCEPTION;
    if (!isfinite(d)) return JS_UNDEFINED;
    if (a->type == NS_AT_POSITIVE && d <= 0) return JS_UNDEFINED;
    if (a->type == NS_AT_NONNEGATIVE && d < 0) return JS_UNDEFINED;
    if (a->type == NS_AT_ALPHA && (d < 0 || d > 1)) return JS_UNDEFINED;
    JS_SetPropertyStr(ctx, h->state, a->name, JS_NewFloat64(ctx, d));
    return JS_UNDEFINED;
}

static JSValue
ns_assign_size(JSContext *ctx, JSValueConst obj, ns_hidden *h,
               const ns_attr_def *a, JSValueConst v)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v) < 0) return JS_EXCEPTION;
    if (!isfinite(d) || d < 0 || d > 18446744073709551615.0)
        return JS_ThrowTypeError(ctx,
            "Failed to set the '%s' property on 'OffscreenCanvas': Value is "
            "outside the 'unsigned long long' value range.", a->name);
    d = trunc(d);
    JS_SetPropertyStr(ctx, h->state, a->name, JS_NewFloat64(ctx, d));
    ns_offscreen_sync_size(ctx, obj);
    return JS_UNDEFINED;
}

static JSValue
ns_assign_handler(JSContext *ctx, ns_hidden *h, const ns_attr_def *a, JSValueConst v)
{
    JS_SetPropertyStr(ctx, h->state, a->name,
                      JS_IsFunction(ctx, v) ? JS_DupValue(ctx, v) : JS_NULL);
    return JS_UNDEFINED;
}

static JSValue
ns_attr_assign(JSContext *ctx, JSValueConst owner, ns_hidden *h, const ns_attr_def *a,
               JSValueConst v)
{
    switch (a->type) {
    case NS_AT_BOOL:
        JS_SetPropertyStr(ctx, h->state, a->name, JS_NewBool(ctx, JS_ToBool(ctx, v)));
        return JS_UNDEFINED;
    case NS_AT_FINITE: case NS_AT_POSITIVE: case NS_AT_NONNEGATIVE: case NS_AT_ALPHA:
        return ns_assign_number(ctx, h, a, v);
    case NS_AT_STYLE:
        return ns_assign_style(ctx, owner, h, a, v);
    case NS_AT_SIZE:
        return ns_assign_size(ctx, owner, h, a, v);
    case NS_AT_HANDLER:
        return ns_assign_handler(ctx, h, a, v);
    default:
        return ns_assign_string(ctx, owner, h, a, v);
    }
}

static JSValue
ns_attr_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
            int magic)
{
    const ns_api_table *t = NS_MAGIC_TABLE(magic);
    const ns_attr_def *a = &t->attrs[NS_MAGIC_INDEX(magic)];
    if (!t->brand(this_val)) return JS_ThrowTypeError(ctx, "Illegal invocation");
    ns_attr_sync_canvas(ctx, this_val);
    ns_hidden *h = JS_GetOpaque(this_val, ns_hidden_class_id);
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    return ns_attr_assign(ctx, this_val, h, a, v);
}

static void
ns_api_define_members(JSContext *ctx, JSValueConst proto, int table)
{
    const ns_api_table *t = &ns_api_tables[table];
    for (guint i = 0; i < t->n_methods; i++) {
        if (table == NS_TBL_OFFSCREEN_CTX2D &&
            t->methods[i].fn == ns_ctx_draw_focus_if_needed)
            continue;
        JS_SetPropertyStr(ctx, proto, t->methods[i].name,
            JS_NewCFunctionMagic(ctx, ns_api_call, t->methods[i].name,
                                 t->methods[i].length, JS_CFUNC_generic_magic,
                                 NS_MAGIC(table, NS_MAGIC_METHOD, i)));
    }
    for (guint i = 0; i < t->n_attrs; i++) {
        const ns_attr_def *a = &t->attrs[i];
        char *get_name = g_strconcat("get ", a->name, NULL);
        char *set_name = g_strconcat("set ", a->name, NULL);
        JSAtom atom = JS_NewAtom(ctx, a->name);
        JS_DefinePropertyGetSet(ctx, proto, atom,
            JS_NewCFunctionMagic(ctx, ns_attr_get, get_name, 0, JS_CFUNC_generic_magic,
                                 NS_MAGIC(table, NS_MAGIC_GETTER, i)),
            a->type == NS_AT_READONLY ? JS_UNDEFINED
                : JS_NewCFunctionMagic(ctx, ns_attr_set, set_name, 1,
                                       JS_CFUNC_generic_magic,
                                       NS_MAGIC(table, NS_MAGIC_SETTER, i)),
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
        g_free(get_name);
        g_free(set_name);
    }
}

static void
ns_api_set_tag(JSContext *ctx, JSValueConst obj, const char *tag)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue sym = JS_GetPropertyStr(ctx, g, "Symbol");
    JSValue tag_sym = JS_GetPropertyStr(ctx, sym, "toStringTag");
    JSAtom atom = JS_ValueToAtom(ctx, tag_sym);
    if (atom != JS_ATOM_NULL) {
        JS_DefinePropertyValue(ctx, obj, atom, JS_NewString(ctx, tag),
                               JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, tag_sym);
    JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, g);
}

JSValue
ns_api_interface(JSContext *ctx, JSValueConst global, const char *name,
                 JSValue ctor, const char *parent)
{
    JSValue parent_ctor = parent ? JS_GetPropertyStr(ctx, global, parent) : JS_UNDEFINED;
    JSValue parent_proto = JS_IsObject(parent_ctor)
        ? JS_GetPropertyStr(ctx, parent_ctor, "prototype") : JS_UNDEFINED;
    JSValue proto = JS_IsObject(parent_proto) ? JS_NewObjectProto(ctx, parent_proto)
                                              : JS_NewObject(ctx);
    JS_SetConstructor(ctx, ctor, proto);
    ns_api_set_tag(ctx, proto, name);
    if (JS_IsObject(parent_ctor)) JS_SetPrototype(ctx, ctor, parent_ctor);
    JS_DefinePropertyValueStr(ctx, global, name, ctor,
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, parent_proto);
    JS_FreeValue(ctx, parent_ctor);
    return proto;
}

static const char *const ns_illegal_ctor_names[] = {
    "CanvasRenderingContext2D", "OffscreenCanvasRenderingContext2D",
    "CanvasGradient", "CanvasPattern", "TextMetrics", "ImageBitmap",
};

static JSValue
ns_api_illegal_constructor(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv, int magic)
{
    (void)argc; (void)argv;
    if (JS_IsUndefined(this_val)) return JS_ThrowTypeError(ctx, "Illegal constructor");
    return JS_ThrowTypeError(ctx, "Failed to construct '%s': Illegal constructor",
                             ns_illegal_ctor_names[magic]);
}

static void
ns_api_install_illegal(JSContext *ctx, JSValueConst global, int index, int table)
{
    const char *name = ns_illegal_ctor_names[index];
    JSValue ctor = JS_NewCFunctionMagic(ctx, ns_api_illegal_constructor, name, 0,
                                        JS_CFUNC_constructor_or_func_magic, index);
    JSValue proto = ns_api_interface(ctx, global, name, ctor, NULL);
    if (table >= 0) ns_api_define_members(ctx, proto, table);
    JS_FreeValue(ctx, proto);
}

static JSValue
ns_image_data_ctor_fn(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv, int magic)
{
    (void)magic;
    return ns_imagedata_construct(ctx, this_val, argc, argv);
}

static JSValue
ns_offscreen_ctor_fn(JSContext *ctx, JSValueConst this_val, int argc,
                     JSValueConst *argv, int magic)
{
    (void)magic;
    return ns_offscreen_construct(ctx, this_val, argc, argv);
}

static JSValue
ns_path2d_ctor_fn(JSContext *ctx, JSValueConst this_val, int argc,
                  JSValueConst *argv, int magic)
{
    (void)magic;
    return ns_path2d_ctor(ctx, this_val, argc, argv);
}

void
ns_canvas_register_classes(JSRuntime *rt)
{
    ns_canvas_register_hidden_class(rt);
    ns_canvas_register_image_bitmap_class(rt);
    ns_canvas_register_path2d_class(rt);
}

void
ns_canvas_install(JSContext *ctx, JSValueConst global, gboolean window)
{
    JSValue proto;
    ns_bind_fn(ctx, global, "createImageBitmap", ns_window_create_image_bitmap, 1);
    if (window) ns_api_install_illegal(ctx, global, 0, NS_TBL_CTX2D);
    ns_api_install_illegal(ctx, global, 1, NS_TBL_OFFSCREEN_CTX2D);
    JSValue ctor = JS_NewCFunctionMagic(ctx, ns_offscreen_ctor_fn, "OffscreenCanvas",
                                        2, JS_CFUNC_constructor_or_func_magic, 0);
    proto = ns_api_interface(ctx, global, "OffscreenCanvas", ctor, "EventTarget");
    ns_api_define_members(ctx, proto, NS_TBL_OFFSCREEN);
    JS_FreeValue(ctx, proto);
    ns_api_install_illegal(ctx, global, 2, NS_TBL_GRADIENT);
    ns_api_install_illegal(ctx, global, 3, NS_TBL_PATTERN);
    ns_api_install_illegal(ctx, global, 4, NS_TBL_TEXTMETRICS);
    ns_api_install_illegal(ctx, global, 5, NS_TBL_IMAGEBITMAP);
    ns_image_bitmap_define_members(ctx, global);
    JSValue image_data = JS_NewCFunctionMagic(ctx, ns_image_data_ctor_fn, "ImageData", 2,
                                              JS_CFUNC_constructor_or_func_magic, 0);
    proto = ns_api_interface(ctx, global, "ImageData", image_data, NULL);
    ns_api_define_members(ctx, proto, NS_TBL_IMAGEDATA);
    JS_FreeValue(ctx, proto);
    JSValue path2d = JS_NewCFunctionMagic(ctx, ns_path2d_ctor_fn, "Path2D", 0,
                                          JS_CFUNC_constructor_or_func_magic, 0);
    proto = ns_api_interface(ctx, global, "Path2D", path2d, NULL);
    ns_api_define_members(ctx, proto, NS_TBL_PATH2D);
    JS_FreeValue(ctx, proto);
}

JSContext *
ns_ctx_realm(JSContext *ctx, JSValueConst this_val)
{
    return ns_canvas_realm(ctx, ns_hidden_ptr(this_val));
}

void
ns_ctx2d_init_state(JSContext *ctx, JSValueConst obj)
{
    static const char *const strings[][2] = {
        { "fillStyle", "#000000" }, { "strokeStyle", "#000000" },
        { "font", "10px sans-serif" }, { "textBaseline", "alphabetic" },
        { "globalCompositeOperation", "source-over" },
        { "imageSmoothingQuality", "low" },
        { "shadowColor", "rgba(0, 0, 0, 0)" }, { "textAlign", "start" },
        { "direction", "ltr" }, { "filter", "none" },
        { "letterSpacing", "0px" }, { "wordSpacing", "0px" },
        { "fontKerning", "auto" }, { "fontStretch", "normal" },
        { "fontVariantCaps", "normal" }, { "textRendering", "auto" },
        { "lang", "inherit" }, { "lineCap", "butt" }, { "lineJoin", "miter" },
    };
    static const struct { const char *name; double value; } numbers[] = {
        { "lineWidth", 1 }, { "miterLimit", 10 }, { "globalAlpha", 1 },
        { "shadowBlur", 0 }, { "shadowOffsetX", 0 }, { "shadowOffsetY", 0 },
        { "lineDashOffset", 0 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(strings); i++)
        ns_hset(ctx, obj, strings[i][0], JS_NewString(ctx, strings[i][1]));
    for (gsize i = 0; i < G_N_ELEMENTS(numbers); i++)
        ns_hset(ctx, obj, numbers[i].name, JS_NewFloat64(ctx, numbers[i].value));
    ns_hset(ctx, obj, "imageSmoothingEnabled", JS_TRUE);
    ns_hset(ctx, obj, "_dashes", JS_NewArray(ctx));
    ns_hset(ctx, obj, "_stateStack", JS_NewArray(ctx));
}

JSValue
ns_ctx2d_new(JSContext *ctx, const ns_node *el, JSValueConst canvas_obj,
             gboolean offscreen, JSValue attrs)
{
    JSContext *realm = ns_canvas_realm(ctx, el);
    JSValue proto = ns_api_proto(realm, offscreen ? "OffscreenCanvasRenderingContext2D"
                                                  : "CanvasRenderingContext2D");
    JSValue obj = ns_hidden_new(realm, offscreen ? NS_HK_OFFSCREEN_CTX2D : NS_HK_CTX2D,
                                proto);
    JS_FreeValue(realm, proto);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, attrs);
        return obj;
    }
    ns_hidden_set_ptr(obj, (gpointer)el);
    ns_hset(ctx, obj, "canvas", JS_DupValue(ctx, canvas_obj));
    ns_hset(ctx, obj, "_attrs", attrs);
    ns_ctx2d_init_state(ctx, obj);
    return obj;
}

JSValue
ns_gradient_new(JSContext *ctx, JSContext *realm, const char *type)
{
    JSValue proto = ns_api_proto(realm, "CanvasGradient");
    JSValue obj = ns_hidden_new(realm, NS_HK_GRADIENT, proto);
    JS_FreeValue(realm, proto);
    if (JS_IsException(obj)) return obj;
    ns_hset(ctx, obj, "_type", JS_NewString(ctx, type));
    ns_hset(ctx, obj, "_stops", JS_NewArray(ctx));
    return obj;
}

JSValue
ns_pattern_new(JSContext *ctx, JSContext *realm, JSValueConst source,
               const char *repetition)
{
    JSValue proto = ns_api_proto(realm, "CanvasPattern");
    JSValue obj = ns_hidden_new(realm, NS_HK_PATTERN, proto);
    JS_FreeValue(realm, proto);
    if (JS_IsException(obj)) return obj;
    ns_hset(ctx, obj, "_type", JS_NewString(ctx, "pattern"));
    ns_hset(ctx, obj, "_node", JS_DupValue(ctx, source));
    ns_hset(ctx, obj, "_rep", JS_NewString(ctx, repetition));
    return obj;
}

JSValue
ns_textmetrics_new(JSContext *ctx, JSContext *realm, const double v[10])
{
    static const char *const names[10] = {
        "width", "actualBoundingBoxLeft", "actualBoundingBoxRight",
        "actualBoundingBoxAscent", "actualBoundingBoxDescent",
        "fontBoundingBoxAscent", "fontBoundingBoxDescent", "hangingBaseline",
        "alphabeticBaseline", "ideographicBaseline",
    };
    JSValue proto = ns_api_proto(realm, "TextMetrics");
    JSValue obj = ns_hidden_new(realm, NS_HK_TEXTMETRICS, proto);
    JS_FreeValue(realm, proto);
    if (JS_IsException(obj)) return obj;
    for (int i = 0; i < 10; i++)
        ns_hset(ctx, obj, names[i], JS_NewFloat64(ctx, v[i]));
    return obj;
}

static JSValue
ns_imagedata_wrap(JSContext *ctx, JSContext *realm, JSValueConst proto, int w,
                  int h, JSValue data, const char *color_space)
{
    JSValue own_proto = JS_UNDEFINED;
    if (!JS_IsObject(proto)) proto = own_proto = ns_api_proto(realm, "ImageData");
    JSValue obj = ns_hidden_new(realm, NS_HK_IMAGEDATA, proto);
    JS_FreeValue(realm, own_proto);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, data);
        return obj;
    }
    ns_hset(ctx, obj, "width", JS_NewInt32(ctx, w));
    ns_hset(ctx, obj, "height", JS_NewInt32(ctx, h));
    ns_hset(ctx, obj, "data", data);
    ns_hset(ctx, obj, "colorSpace", JS_NewString(ctx, color_space));
    ns_hset(ctx, obj, "pixelFormat", JS_NewString(ctx, "rgba-unorm8"));
    return obj;
}

static JSValue
ns_clamped_array(JSContext *realm, const uint8_t *rgba, size_t n)
{
    JSValue ab;
    if (rgba) {
        ab = JS_NewArrayBufferCopy(realm, rgba, n);
    } else {
        uint8_t *zeros = g_try_malloc0(n);
        if (!zeros) return JS_ThrowRangeError(realm, "ImageData allocation failed");
        ab = JS_NewArrayBufferCopy(realm, zeros, n);
        g_free(zeros);
    }
    if (JS_IsException(ab)) return ab;
    JSValue global = JS_GetGlobalObject(realm);
    JSValue ctor = JS_GetPropertyStr(realm, global, "Uint8ClampedArray");
    JS_FreeValue(realm, global);
    JSValueConst args[1] = { ab };
    JSValue data = JS_CallConstructor(realm, ctor, 1, args);
    JS_FreeValue(realm, ctor);
    JS_FreeValue(realm, ab);
    return data;
}

JSValue
ns_imagedata_new(JSContext *ctx, JSContext *realm, int w, int h, const uint8_t *rgba)
{
    if (w <= 0 || h <= 0) return JS_NULL;
    if (w > 32767 || h > 32767) return JS_ThrowRangeError(ctx, "ImageData too large");
    JSValue data = ns_clamped_array(realm, rgba, (size_t)w * (size_t)h * 4u);
    if (JS_IsException(data)) return data;
    return ns_imagedata_wrap(ctx, realm, JS_UNDEFINED, w, h, data, "srgb");
}

static JSValue
ns_imagedata_color_space(JSContext *ctx, JSValueConst settings, const char **out)
{
    *out = "srgb";
    if (!JS_IsObject(settings)) return JS_UNDEFINED;
    JSValue cs = JS_GetPropertyStr(ctx, settings, "colorSpace");
    if (JS_IsException(cs)) return cs;
    if (!JS_IsUndefined(cs)) {
        const char *s = JS_ToCString(ctx, cs);
        if (!s) {
            JS_FreeValue(ctx, cs);
            return JS_EXCEPTION;
        }
        if (strcmp(s, "srgb") == 0) *out = "srgb";
        else if (strcmp(s, "display-p3") == 0) *out = "display-p3";
        else JS_ThrowTypeError(ctx,
            "Failed to construct 'ImageData': The provided value '%s' is not a "
            "valid enum value of type PredefinedColorSpace.", s);
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, cs);
    return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue
ns_imagedata_array_size(JSContext *ctx, size_t blen, int argc, JSValueConst *argv,
                        uint32_t *sw, uint64_t *rows)
{
    if (blen == 0 || blen % 4)
        return ns_canvas_throw_dom(ctx, "InvalidStateError",
                                   "The input data length is not a nonzero multiple of 4.");
    if (JS_ToUint32(ctx, sw, argv[1]) < 0) return JS_EXCEPTION;
    if (*sw == 0)
        return ns_canvas_throw_dom(ctx, "IndexSizeError", "The source width is zero.");
    uint64_t pixels = blen / 4;
    if (pixels % *sw)
        return ns_canvas_throw_dom(ctx, "IndexSizeError",
            "The input data byte length is not a multiple of (4 * width).");
    *rows = pixels / *sw;
    if (argc < 3 || JS_IsUndefined(argv[2])) return JS_UNDEFINED;
    uint32_t sh = 0;
    if (JS_ToUint32(ctx, &sh, argv[2]) < 0) return JS_EXCEPTION;
    if (sh != *rows)
        return ns_canvas_throw_dom(ctx, "IndexSizeError",
            "The input data byte length is not equal to (4 * width * height).");
    return JS_UNDEFINED;
}

static JSValue
ns_imagedata_from_array(JSContext *ctx, JSValueConst proto, JSValueConst data,
                        int argc, JSValueConst *argv, const char *space_hint)
{
    (void)space_hint;
    size_t off = 0, blen = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, data, &off, &blen, &bpe);
    if (JS_IsException(buf)) return buf;
    JS_FreeValue(ctx, buf);
    uint32_t sw = 0;
    uint64_t rows = 0;
    JSValue bad = ns_imagedata_array_size(ctx, blen, argc, argv, &sw, &rows);
    if (JS_IsException(bad)) return bad;
    const char *space = "srgb";
    JSValue err = ns_imagedata_color_space(ctx, argc >= 4 ? argv[3] : JS_UNDEFINED, &space);
    if (JS_IsException(err)) return err;
    if (rows > 32767 || sw > 32767)
        return JS_ThrowRangeError(ctx, "Failed to construct 'ImageData': The requested image size exceeds the supported range.");
    return ns_imagedata_wrap(ctx, ctx, proto, (int)sw, (int)rows,
                             JS_DupValue(ctx, data), space);
}

static JSValue
ns_imagedata_check_size(JSContext *ctx, uint32_t sw, uint32_t sh)
{
    if (sw == 0 || sh == 0)
        return ns_canvas_throw_dom(ctx, "IndexSizeError", sw == 0
            ? "The source width is zero or not a number."
            : "The source height is zero or not a number.");
    if (sw > 32767 || sh > 32767)
        return JS_ThrowRangeError(ctx, "Failed to construct 'ImageData': The requested image size exceeds the supported range.");
    return JS_UNDEFINED;
}

static JSValue
ns_imagedata_from_size(JSContext *ctx, JSValueConst proto, int argc, JSValueConst *argv)
{
    uint32_t sw = 0, sh = 0;
    if (JS_ToUint32(ctx, &sw, argv[0]) < 0 || JS_ToUint32(ctx, &sh, argv[1]) < 0)
        return JS_EXCEPTION;
    const char *space = "srgb";
    JSValue err = ns_imagedata_color_space(ctx, argc >= 3 ? argv[2] : JS_UNDEFINED, &space);
    if (JS_IsException(err)) return err;
    JSValue bad = ns_imagedata_check_size(ctx, sw, sh);
    if (JS_IsException(bad)) return bad;
    JSValue data = ns_clamped_array(ctx, NULL, (size_t)sw * (size_t)sh * 4u);
    if (JS_IsException(data)) return data;
    return ns_imagedata_wrap(ctx, ctx, proto, (int)sw, (int)sh, data, space);
}

static JSValue
ns_imagedata_construct(JSContext *ctx, JSValueConst new_target, int argc,
                       JSValueConst *argv)
{
    if (JS_IsUndefined(new_target)) return ns_api_throw_new_required(ctx, "ImageData");
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "Failed to construct 'ImageData': 2 arguments required, but only %d present.",
            argc);
    JSValue proto = ns_api_proto_of_ctor(ctx, new_target, "ImageData");
    JSValue result;
    if (JS_IsObject(argv[0]) && JS_GetTypedArrayType(argv[0]) == JS_TYPED_ARRAY_UINT8C)
        result = ns_imagedata_from_array(ctx, proto, argv[0], argc, argv, NULL);
    else
        result = ns_imagedata_from_size(ctx, proto, argc, argv);
    JS_FreeValue(ctx, proto);
    return result;
}

const ns_node *
ns_offscreen_node(JSValueConst obj)
{
    return ns_hidden_is(obj, NS_HK_OFFSCREEN) ? ns_hidden_ptr(obj) : NULL;
}

static void
ns_offscreen_sync_size(JSContext *ctx, JSValueConst obj)
{
    ns_node *el = (ns_node *)ns_offscreen_node(obj);
    if (!el) return;
    static const char *const names[2] = { "width", "height" };
    for (int i = 0; i < 2; i++) {
        JSValue v = ns_hget(ctx, obj, names[i]);
        double d = 0;
        JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        char buf[24];
        g_snprintf(buf, sizeof buf, "%d", d > 8192 ? 8192 : (int)d);
        const char *cur = ns_element_get_attr(el, names[i]);
        if (!cur || strcmp(cur, buf) != 0) ns_element_set_attr(el, names[i], buf);
    }
}

static JSValue
ns_offscreen_construct(JSContext *ctx, JSValueConst new_target, int argc,
                       JSValueConst *argv)
{
    if (JS_IsUndefined(new_target)) return ns_api_throw_new_required(ctx, "OffscreenCanvas");
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "Failed to construct 'OffscreenCanvas': 2 arguments required, but only %d present.",
            argc);
    double dims[2];
    for (int i = 0; i < 2; i++) {
        if (JS_ToFloat64(ctx, &dims[i], argv[i]) < 0) return JS_EXCEPTION;
        if (!isfinite(dims[i]) || dims[i] < 0 || dims[i] > 18446744073709551615.0)
            return JS_ThrowTypeError(ctx,
                "Failed to construct 'OffscreenCanvas': Value is outside the "
                "'unsigned long long' value range.");
        dims[i] = trunc(dims[i]);
    }
    ns_js *js = js_from_ctx(ctx);
    if (!js) return JS_ThrowTypeError(ctx, "OffscreenCanvas is not available here");
    JSValue proto = ns_api_proto_of_ctor(ctx, new_target, "OffscreenCanvas");
    JSValue obj = ns_hidden_new(ctx, NS_HK_OFFSCREEN, proto);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    ns_node *el = ns_node_new_element(g_strdup("canvas"));
    ns_hidden_set_ptr(obj, el);
    ns_canvas_state_for(js, el)->owned_node = el;
    ns_hset(ctx, obj, "width", JS_NewFloat64(ctx, dims[0]));
    ns_hset(ctx, obj, "height", JS_NewFloat64(ctx, dims[1]));
    ns_hset(ctx, obj, "oncontextlost", JS_NULL);
    ns_hset(ctx, obj, "oncontextrestored", JS_NULL);
    ns_offscreen_sync_size(ctx, obj);
    return obj;
}

static gboolean
ns_font_size_px(const char *size, double *px)
{
    static const struct { const char *name; double px; } keywords[] = {
        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 },
        { "large", 18 }, { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 },
        { "larger", 12 }, { "smaller", 25.0 / 3.0 },
    };
    static const struct { const char *unit; double factor; } units[] = {
        { "px", 1 }, { "pt", 4.0 / 3.0 }, { "pc", 16 }, { "in", 96 },
        { "cm", 96 / 2.54 }, { "mm", 96 / 25.4 }, { "q", 96 / 101.6 },
        { "em", 10 }, { "ex", 5 }, { "ch", 5 }, { "rem", 16 }, { "%", 0.1 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(keywords); i++)
        if (g_ascii_strcasecmp(size, keywords[i].name) == 0) {
            *px = keywords[i].px;
            return TRUE;
        }
    char *end = NULL;
    double v = g_ascii_strtod(size, &end);
    if (end == size) return FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(units); i++)
        if (g_ascii_strcasecmp(end, units[i].unit) == 0) {
            *px = v * units[i].factor;
            return TRUE;
        }
    return FALSE;
}

static const char *
ns_font_family_quoted(const char *p, const char **start, gsize *len)
{
    char quote = *p++;
    *start = p;
    while (*p && *p != quote) p += (*p == '\\' && p[1]) ? 2 : 1;
    *len = (gsize)(p - *start);
    if (*p) p++;
    return p;
}

static const char *
ns_font_family_bare(const char *p, const char **start, gsize *len)
{
    *start = p;
    while (*p && *p != ',') p++;
    *len = (gsize)(p - *start);
    while (*len && (*start)[*len - 1] == ' ') (*len)--;
    return p;
}

static void
ns_font_family_emit(GString *out, const char *start, gsize len)
{
    if (out->len && out->str[out->len - 1] != ' ') g_string_append(out, ", ");
    if (memchr(start, ' ', len)) {
        g_string_append_c(out, '"');
        g_string_append_len(out, start, (gssize)len);
        g_string_append_c(out, '"');
    } else {
        g_string_append_len(out, start, (gssize)len);
    }
}

static void
ns_font_family_append(GString *out, const char *family)
{
    const char *p = family;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *start;
        gsize len;
        p = (*p == '"' || *p == '\'') ? ns_font_family_quoted(p, &start, &len)
                                       : ns_font_family_bare(p, &start, &len);
        ns_font_family_emit(out, start, len);
    }
}

enum {
    NS_FONT_STYLE,
    NS_FONT_WEIGHT,
    NS_FONT_VARIANT,
    NS_FONT_STRETCH,
    NS_FONT_NORMAL,
    NS_FONT_SIZE,
};

static gboolean
ns_font_token_is_weight(const char *t)
{
    return g_ascii_strcasecmp(t, "bold") == 0 || g_ascii_strcasecmp(t, "bolder") == 0 ||
           g_ascii_strcasecmp(t, "lighter") == 0 ||
           (g_ascii_isdigit(t[0]) && strspn(t, "0123456789") == strlen(t));
}

static int
ns_font_token_slot(const char *t)
{
    if (g_ascii_strcasecmp(t, "italic") == 0 || g_ascii_strcasecmp(t, "oblique") == 0)
        return NS_FONT_STYLE;
    if (g_ascii_strcasecmp(t, "small-caps") == 0) return NS_FONT_VARIANT;
    if (ns_font_token_is_weight(t)) return NS_FONT_WEIGHT;
    if (g_str_has_suffix(t, "condensed") || g_str_has_suffix(t, "expanded"))
        return NS_FONT_STRETCH;
    return g_ascii_strcasecmp(t, "normal") == 0 ? NS_FONT_NORMAL : NS_FONT_SIZE;
}

static int
ns_font_collect_keywords(char **tokens, const char **parts)
{
    int i = 0;
    for (; tokens[i]; i++) {
        int slot = ns_font_token_slot(tokens[i]);
        if (slot == NS_FONT_SIZE) break;
        if (slot != NS_FONT_NORMAL) parts[slot] = tokens[i];
    }
    return i;
}

static void
ns_font_append_keywords(GString *out, const char *const *parts)
{
    for (int k = 0; k < 4; k++) {
        if (!parts[k]) continue;
        if (out->len) g_string_append_c(out, ' ');
        g_string_append(out, strcmp(parts[k], "700") == 0 ? "bold" : parts[k]);
    }
}

static void
ns_font_append_size(GString *out, const char *size)
{
    double px = 0;
    if (out->len) g_string_append_c(out, ' ');
    if (ns_font_size_px(size, &px)) {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(buf, sizeof buf, "%g", px);
        g_string_append_printf(out, "%spx", buf);
    } else {
        g_string_append(out, size);
    }
}

static void
ns_font_append_family(GString *out, char **tokens)
{
    char *family = g_strjoinv(" ", tokens);
    g_string_append_c(out, ' ');
    ns_font_family_append(out, family);
    g_free(family);
}

static char *
ns_canvas_font_string(const char *css)
{
    char *canon = ns_css_font_shorthand_canonical(css);
    if (!canon) return NULL;
    char **tokens = g_strsplit(canon, " ", -1);
    const char *parts[4] = { NULL, NULL, NULL, NULL };
    int i = ns_font_collect_keywords(tokens, parts);
    GString *out = g_string_new(NULL);
    ns_font_append_keywords(out, parts);
    if (tokens[i]) {
        ns_font_append_size(out, tokens[i]);
        i++;
    }
    if (tokens[i] && strcmp(tokens[i], "/") == 0 && tokens[i + 1]) i += 2;
    if (tokens[i]) ns_font_append_family(out, tokens + i);
    g_strfreev(tokens);
    g_free(canon);
    return g_string_free(out, FALSE);
}

static JSValue
ns_imagedata_clone(JSContext *ctx, JSValueConst v)
{
    JSValue data = ns_hget(ctx, v, "data");
    size_t off = 0, blen = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, data, &off, &blen, &bpe);
    JS_FreeValue(ctx, data);
    if (JS_IsException(buf)) return buf;
    size_t total = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &total, buf);
    JSValue copy = base && off + blen <= total
        ? ns_clamped_array(ctx, base + off, blen)
        : JS_ThrowTypeError(ctx, "The ImageData's pixel buffer is detached");
    JS_FreeValue(ctx, buf);
    if (JS_IsException(copy)) return copy;
    JSValue w = ns_hget(ctx, v, "width"), h = ns_hget(ctx, v, "height");
    JSValue space = ns_hget(ctx, v, "colorSpace");
    int32_t iw = 0, ih = 0;
    JS_ToInt32(ctx, &iw, w);
    JS_ToInt32(ctx, &ih, h);
    const char *cs = JS_ToCString(ctx, space);
    JSValue out = ns_imagedata_wrap(ctx, ctx, JS_UNDEFINED, iw, ih, copy, cs ? cs : "srgb");
    if (cs) JS_FreeCString(ctx, cs);
    JS_FreeValue(ctx, w);
    JS_FreeValue(ctx, h);
    JS_FreeValue(ctx, space);
    return out;
}

JSValue
ns_canvas_clone_object(JSContext *ctx, JSValueConst v)
{
    if (ns_hidden_is(v, NS_HK_IMAGEDATA)) return ns_imagedata_clone(ctx, v);
    if (ns_image_bitmap_is(v)) return ns_image_bitmap_clone(ctx, v);
    if (JS_GetOpaque(v, ns_hidden_class_id) || ns_value_is_path2d(v))
        return ns_canvas_throw_dom(ctx, "DataCloneError",
                                   "The object could not be cloned.");
    return JS_UNDEFINED;
}
