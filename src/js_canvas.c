/* Nordstjernen — HTML canvas 2D, Path2D, ImageBitmap (QuickJS).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */
#include "js_internal.h"
#include "js_classid.h"

#include <math.h>
#include <string.h>

#include <gio/gio.h>
#include "ns_pango.h"

#include "css.h"
#include "net.h"
#include "paint.h"
#include "video.h"
#include "texture.h"
#include "image.h"
#include "webgl.h"
#ifdef ND_HAVE_WEBGPU
#include "webgpu.h"
#endif

static JSClassID ns_path2d_class_id;

void
ns_path2d_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_path2d *p = JS_GetOpaque(val, ns_path2d_class_id);
    if (!p) return;
    if (p->cr) cairo_destroy(p->cr);
    if (p->rs) cairo_surface_destroy(p->rs);
    g_free(p);
}

static JSClassDef ns_path2d_class = {
    "Path2D",
    .finalizer = ns_path2d_finalizer,
};

static JSClassID ns_image_bitmap_class_id;

void
ns_image_bitmap_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_image_bitmap *b = JS_GetOpaque(val, ns_image_bitmap_class_id);
    if (!b) return;
    if (b->surf) cairo_surface_destroy(b->surf);
    g_free(b);
}

static JSClassDef ns_image_bitmap_class = {
    "ImageBitmap",
    .finalizer = ns_image_bitmap_finalizer,
};

void
ns_canvas_state_free(gpointer data)
{
    ns_canvas_state *st = data;
    if (!st) return;
    if (st->fill_pattern)   cairo_pattern_destroy(st->fill_pattern);
    if (st->stroke_pattern) cairo_pattern_destroy(st->stroke_pattern);
    if (st->cr)   cairo_destroy(st->cr);
    if (st->surf) cairo_surface_destroy(st->surf);
    if (JS_IsObject(st->ctx2d)) JS_FreeValueRT(st->rt, st->ctx2d);
    if (st->owned_node) ns_node_free(st->owned_node);
    g_free(st->font);
    g_free(st);
}

static void
ns_canvas_state_reset(ns_canvas_state *st, int w, int h)
{
    if (st->fill_pattern)   { cairo_pattern_destroy(st->fill_pattern);   st->fill_pattern = NULL; }
    if (st->stroke_pattern) { cairo_pattern_destroy(st->stroke_pattern); st->stroke_pattern = NULL; }
    if (st->cr)   cairo_destroy(st->cr);
    if (st->surf) cairo_surface_destroy(st->surf);
    g_free(st->font);
    st->w = w;
    st->h = h;
    st->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    st->cr   = cairo_create(st->surf);
    st->fill_r = st->fill_g = st->fill_b = 0; st->fill_a = 1;
    st->stroke_r = st->stroke_g = st->stroke_b = 0; st->stroke_a = 1;
    st->line_width = 1;
    st->font = g_strdup("10px sans-serif");
    st->shadow_r = st->shadow_g = st->shadow_b = st->shadow_a = 0;
    st->shadow_blur = st->shadow_ox = st->shadow_oy = 0;
    st->origin_clean = TRUE;
    if (JS_IsObject(st->ctx2d)) ns_ctx2d_init_state(st->jsctx, st->ctx2d);
}

gboolean
ns_image_bitmap_is(JSValueConst v)
{
    return JS_GetOpaque(v, ns_image_bitmap_class_id) != NULL;
}

JSValue
ns_image_bitmap_close(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_image_bitmap *b = JS_GetOpaque(this_val, ns_image_bitmap_class_id);
    if (!b) return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (b->surf) {
        cairo_surface_destroy(b->surf);
        b->surf = NULL;
        b->w = b->h = 0;
    }
    return JS_UNDEFINED;
}

static JSValue
ns_image_bitmap_get_size(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv, int magic)
{
    (void)argc; (void)argv;
    ns_image_bitmap *b = JS_GetOpaque(this_val, ns_image_bitmap_class_id);
    if (!b) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return JS_NewInt32(ctx, magic ? b->h : b->w);
}

void
ns_image_bitmap_define_members(JSContext *ctx, JSValueConst global)
{
    JSValue proto = ns_api_proto(ctx, "ImageBitmap");
    static const char *const names[2] = { "width", "height" };
    for (int i = 0; i < 2; i++) {
        char *get_name = g_strconcat("get ", names[i], NULL);
        JSAtom atom = JS_NewAtom(ctx, names[i]);
        JS_DefinePropertyGetSet(ctx, proto, atom,
            JS_NewCFunctionMagic(ctx, ns_image_bitmap_get_size, get_name, 0,
                                 JS_CFUNC_generic_magic, i),
            JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
        g_free(get_name);
    }
    JS_FreeValue(ctx, proto);
    (void)global;
}

JSValue
ns_image_bitmap_make(JSContext *ctx, cairo_surface_t *surf, int w, int h,
                     gboolean origin_clean)
{
    if (!surf || w <= 0 || h <= 0) {
        if (surf) cairo_surface_destroy(surf);
        return JS_NULL;
    }
    ns_image_bitmap *b = g_new0(ns_image_bitmap, 1);
    b->surf = surf;
    b->w = w;
    b->h = h;
    b->origin_clean = origin_clean;
    JSValue proto = ns_api_proto(ctx, "ImageBitmap");
    JSValue obj = JS_IsObject(proto)
        ? JS_NewObjectProtoClass(ctx, proto, ns_image_bitmap_class_id)
        : JS_NewObjectClass(ctx, ns_image_bitmap_class_id);
    JS_FreeValue(ctx, proto);
    JS_SetOpaque(obj, b);
    return obj;
}

JSValue
ns_image_bitmap_clone(JSContext *ctx, JSValueConst v)
{
    ns_image_bitmap *b = JS_GetOpaque(v, ns_image_bitmap_class_id);
    if (!b || !b->surf)
        return ns_canvas_throw_dom(ctx, "DataCloneError", "The ImageBitmap is closed.");
    cairo_surface_t *copy = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, b->w, b->h);
    cairo_t *cr = cairo_create(copy);
    cairo_set_source_surface(cr, b->surf, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    return ns_image_bitmap_make(ctx, copy, b->w, b->h, b->origin_clean);
}

cairo_surface_t *
ns_image_bitmap_from_imagedata(JSContext *ctx, JSValueConst src,
                               int *out_w, int *out_h)
{
    JSValue wv = JS_GetPropertyStr(ctx, src, "width");
    JSValue hv = JS_GetPropertyStr(ctx, src, "height");
    JSValue dv = JS_GetPropertyStr(ctx, src, "data");
    int iw = 0, ih = 0;
    JS_ToInt32(ctx, &iw, wv); JS_ToInt32(ctx, &ih, hv);
    JS_FreeValue(ctx, wv); JS_FreeValue(ctx, hv);
    if (iw <= 0 || ih <= 0 || iw > 32767 || ih > 32767) {
        JS_FreeValue(ctx, dv); return NULL;
    }
    size_t byte_offset = 0, byte_len = 0, bpe = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, dv, &byte_offset, &byte_len, &bpe);
    if (JS_IsException(ab)) { JS_FreeValue(ctx, dv); return NULL; }
    size_t ab_len = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &ab_len, ab);
    if (!base || byte_offset + byte_len > ab_len ||
        byte_len < (size_t)iw * (size_t)ih * 4u) {
        JS_FreeValue(ctx, ab); JS_FreeValue(ctx, dv); return NULL;
    }
    const uint8_t *rgba = base + byte_offset;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        JS_FreeValue(ctx, ab); JS_FreeValue(ctx, dv); return NULL;
    }
    uint8_t *dst = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < ih; y++) {
        const uint8_t *s = rgba + (size_t)y * (size_t)iw * 4u;
        uint8_t *d = dst + (size_t)y * (size_t)stride;
        for (int x = 0; x < iw; x++) {
            uint8_t r = s[0], g = s[1], b = s[2], a = s[3];
            uint8_t pr, pg, pb;
            if (a == 0)        { pr = 0; pg = 0; pb = 0; }
            else if (a == 255) { pr = r; pg = g; pb = b; }
            else {
                pr = (uint8_t)((r * a + 127) / 255);
                pg = (uint8_t)((g * a + 127) / 255);
                pb = (uint8_t)((b * a + 127) / 255);
            }
            d[0] = pb; d[1] = pg; d[2] = pr; d[3] = a;
            s += 4; d += 4;
        }
    }
    cairo_surface_mark_dirty(surf);
    JS_FreeValue(ctx, ab); JS_FreeValue(ctx, dv);
    *out_w = iw;
    *out_h = ih;
    return surf;
}

cairo_surface_t *
ns_image_bitmap_crop(cairo_surface_t *src, int sw, int sh,
                     int sx, int sy, int rw, int rh)
{
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, rw, rh);
    if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(out);
        return NULL;
    }
    cairo_t *cr = cairo_create(out);
    cairo_set_source_surface(cr, src, -sx, -sy);
    cairo_paint(cr);
    cairo_destroy(cr);
    (void)sw; (void)sh;
    return out;
}

static guint8 *
ns_blob_byte_array(JSContext *ctx, JSValueConst src, gsize *out_len)
{
    *out_len = 0;
    JSValue b = JS_GetPropertyStr(ctx, src, "__ndBlobBytes");
    if (!JS_IsObject(b)) { JS_FreeValue(ctx, b); return NULL; }
    JSValue lv = JS_GetPropertyStr(ctx, b, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    if (len == 0 || len > 256u * 1024u * 1024u) { JS_FreeValue(ctx, b); return NULL; }
    guint8 *out = g_try_malloc(len);
    if (!out) { JS_FreeValue(ctx, b); return NULL; }
    for (uint32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, b, i);
        int32_t bv = 0;
        JS_ToInt32(ctx, &bv, v);
        JS_FreeValue(ctx, v);
        out[i] = (guint8)(bv & 0xff);
    }
    JS_FreeValue(ctx, b);
    *out_len = len;
    return out;
}

static cairo_surface_t *
ns_image_bitmap_from_blob(JSContext *ctx, JSValueConst src, int *out_w, int *out_h)
{
    gsize len = 0;
    guint8 *bytes = ns_blob_byte_array(ctx, src, &len);
    if (!bytes) return NULL;
    int w = 0, h = 0;
    gsize stride = 0, buf_len = 0;
    ns_texture_format fmt;
    guint8 *pix = ns_image_decode_bytes_to_pixels(bytes, len, &w, &h,
                                                  &stride, &buf_len, &fmt);
    g_free(bytes);
    if (!pix || w <= 0 || h <= 0 || w > 32767 || h > 32767) {
        g_free(pix);
        return NULL;
    }
    if (stride < (gsize)w * 4u || (gsize)h > buf_len / stride) {
        g_free(pix);
        return NULL;
    }
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        g_free(pix);
        return NULL;
    }
    uint8_t *dst = cairo_image_surface_get_data(surf);
    int dst_stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)y * (size_t)dst_stride,
               pix + (size_t)y * stride, (size_t)w * 4u);
    cairo_surface_mark_dirty(surf);
    g_free(pix);
    *out_w = w;
    *out_h = h;
    return surf;
}

static void
ns_reject_image_decode(JSContext *ctx, JSValue resolvers[2])
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, g, "DOMException");
    JSValue args[2] = {
        JS_NewString(ctx, "The source image could not be decoded."),
        JS_NewString(ctx, "InvalidStateError"),
    };
    JSValue exc = JS_CallConstructor(ctx, ctor, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(exc) || !JS_IsObject(exc)) {
        if (JS_IsException(exc)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, exc);
        exc = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, exc, "name",
                          JS_NewString(ctx, "InvalidStateError"));
    }
    JS_Call(ctx, resolvers[1], JS_UNDEFINED, 1, &exc);
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, resolvers[0]);
    JS_FreeValue(ctx, resolvers[1]);
}

JSValue
ns_canvas_throw_dom(JSContext *ctx, const char *name, const char *msg)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, g, "DOMException");
    JS_FreeValue(ctx, g);
    JSValue args[2] = { JS_NewString(ctx, msg), JS_NewString(ctx, name) };
    JSValue exc = JS_CallConstructor(ctx, ctor, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(exc) || !JS_IsObject(exc)) {
        if (JS_IsException(exc)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, exc);
        exc = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, exc, "name", JS_NewString(ctx, name));
        JS_SetPropertyStr(ctx, exc, "message", JS_NewString(ctx, msg));
    }
    return JS_Throw(ctx, exc);
}

JSValue
ns_window_create_image_bitmap(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue resolvers[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolvers);
    if (JS_IsException(promise)) return promise;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        ns_js_promise_reject(ctx, resolvers, "createImageBitmap: source required");
        return promise;
    }
    int sw = 0, sh = 0;
    gboolean origin_clean = TRUE;
    cairo_surface_t *surf = NULL;
    JSValue dv = JS_GetPropertyStr(ctx, argv[0], "data");
    gboolean is_imagedata = JS_IsObject(dv);
    JS_FreeValue(ctx, dv);
    gboolean is_blob = FALSE;
    if (!is_imagedata) {
        JSValue bv = JS_GetPropertyStr(ctx, argv[0], "__ndBlobBytes");
        is_blob = JS_IsObject(bv);
        JS_FreeValue(ctx, bv);
    }
    if (is_imagedata)
        surf = ns_image_bitmap_from_imagedata(ctx, argv[0], &sw, &sh);
    else if (is_blob)
        surf = ns_image_bitmap_from_blob(ctx, argv[0], &sw, &sh);
    else
        surf = ns_ctx_drawimage_source(ctx, argv[0], &sw, &sh, &origin_clean);
    if (!surf) {
        ns_reject_image_decode(ctx, resolvers);
        return promise;
    }
    int crop = 0;
    int sx = 0, sy = 0, rw = sw, rh = sh;
    if (argc >= 5) {
        JS_ToInt32(ctx, &sx, argv[1]);
        JS_ToInt32(ctx, &sy, argv[2]);
        JS_ToInt32(ctx, &rw, argv[3]);
        JS_ToInt32(ctx, &rh, argv[4]);
        crop = 1;
    }
    if (crop && (rw <= 0 || rh <= 0 || rw > 32767 || rh > 32767)) {
        cairo_surface_destroy(surf);
        ns_js_promise_reject(ctx, resolvers, "createImageBitmap: invalid crop size");
        return promise;
    }
    if (crop) {
        cairo_surface_t *out = ns_image_bitmap_crop(surf, sw, sh, sx, sy, rw, rh);
        cairo_surface_destroy(surf);
        if (!out) {
            ns_js_promise_reject(ctx, resolvers, "createImageBitmap: crop failed");
            return promise;
        }
        surf = out;
        sw = rw; sh = rh;
    }
    JSValue bm = ns_image_bitmap_make(ctx, surf, sw, sh, origin_clean);
    JS_Call(ctx, resolvers[0], JS_UNDEFINED, 1, &bm);
    JS_FreeValue(ctx, bm);
    JS_FreeValue(ctx, resolvers[0]);
    JS_FreeValue(ctx, resolvers[1]);
    return promise;
}

static cairo_surface_t *
ns_canvas_surface_copy(cairo_surface_t *src, int w, int h)
{
    cairo_surface_t *copy = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(copy) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(copy);
        return NULL;
    }
    cairo_t *cr = cairo_create(copy);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    return copy;
}

static void
ns_canvas_state_clear_surface(ns_canvas_state *st)
{
    if (!st || !st->surf) return;
    cairo_t *clear = cairo_create(st->surf);
    cairo_set_operator(clear, CAIRO_OPERATOR_CLEAR);
    cairo_paint(clear);
    cairo_destroy(clear);
}

JSValue
ns_offscreen_transferToImageBitmap(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_js *js = js_from_ctx(ctx);
    const ns_node *el = ns_offscreen_node(this_val);
    if (!js || !el) return JS_NULL;
    cairo_surface_t *src = ns_js_canvas_surface(js, el);
    if (!src) return JS_NULL;
    int w = cairo_image_surface_get_width(src);
    int h = cairo_image_surface_get_height(src);
    if (w <= 0 || h <= 0) return JS_NULL;
    cairo_surface_t *copy = ns_canvas_surface_copy(src, w, h);
    if (!copy) return JS_NULL;
    ns_canvas_state *st = js->canvas_states
        ? g_hash_table_lookup(js->canvas_states, el) : NULL;
    ns_canvas_state_clear_surface(st);
    return ns_image_bitmap_make(ctx, copy, w, h, !st || st->origin_clean);
}

JSValue
ns_dommatrix_make(JSContext *ctx, double a, double b, double c, double d,
                  double e, double f)
{
    const double v[6] = { a, b, c, d, e, f };
    JSValue init = JS_NewArray(ctx);
    for (uint32_t i = 0; i < 6; i++)
        JS_SetPropertyUint32(ctx, init, i, JS_NewFloat64(ctx, v[i]));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "DOMMatrix");
    JS_FreeValue(ctx, global);
    JSValue m = JS_IsFunction(ctx, ctor)
        ? JS_CallConstructor(ctx, ctor, 1, &init) : JS_UNDEFINED;
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, init);
    if (JS_IsException(m)) JS_FreeValue(ctx, JS_GetException(ctx));
    if (JS_IsObject(m)) return m;
    JS_FreeValue(ctx, m);
    static const char *const keys[6] = { "a", "b", "c", "d", "e", "f" };
    JSValue plain = JS_NewObject(ctx);
    for (int i = 0; i < 6; i++)
        JS_SetPropertyStr(ctx, plain, keys[i], JS_NewFloat64(ctx, v[i]));
    return plain;
}

int
ns_canvas_dim_from_attr(const ns_node *el, const char *name, int defv)
{
    const char *v = ns_element_get_attr(el, name);
    if (!v || !*v) return defv;
    int n = ns_parse_int(v, defv, 0, 8192);
    if (n < 1) return defv;
    return n;
}

static double
ns_srgb_encode(double v)
{
    if (v <= 0) return 0;
    if (v >= 1) return 1;
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
}

static double
ns_srgb_decode(double v)
{
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static gboolean
ns_color_component(const char *tok, double *out)
{
    if (strcmp(tok, "none") == 0) {
        *out = 0;
        return TRUE;
    }
    char *end = NULL;
    double v = g_ascii_strtod(tok, &end);
    if (end == tok) return FALSE;
    if (strcmp(end, "%") == 0) v /= 100.0;
    else if (*end) return FALSE;
    *out = v;
    return TRUE;
}

static char **
ns_color_fn_split(const char *body, gsize len)
{
    char *copy = g_strndup(body, len);
    GString *spaced = g_string_new(NULL);
    for (const char *q = copy; *q; q++) {
        if (*q == '/') g_string_append(spaced, " / ");
        else g_string_append_c(spaced, g_ascii_isspace(*q) ? ' ' : g_ascii_tolower(*q));
    }
    g_free(copy);
    char **tok = g_strsplit_set(spaced->str, " ", -1);
    g_string_free(spaced, TRUE);
    return tok;
}

static GPtrArray *
ns_color_fn_parts(char **tok)
{
    GPtrArray *parts = g_ptr_array_new();
    for (char **t = tok; *t; t++)
        if (**t) g_ptr_array_add(parts, *t);
    return parts;
}

static gboolean
ns_color_fn_shape(GPtrArray *parts, gboolean *p3)
{
    if (parts->len < 4) return FALSE;
    const char *space = g_ptr_array_index(parts, 0);
    *p3 = strcmp(space, "display-p3") == 0;
    if (!*p3 && strcmp(space, "srgb") != 0) return FALSE;
    return parts->len == 4 ||
           (parts->len == 6 && strcmp(g_ptr_array_index(parts, 4), "/") == 0);
}

static gboolean
ns_color_fn_values(GPtrArray *parts, double *c, double *alpha)
{
    for (int i = 0; i < 3; i++)
        if (!ns_color_component(g_ptr_array_index(parts, (guint)(i + 1)), &c[i]))
            return FALSE;
    return parts->len != 6 || ns_color_component(g_ptr_array_index(parts, 5), alpha);
}

static void
ns_display_p3_to_srgb(double *c)
{
    double lr = ns_srgb_decode(c[0]), lg = ns_srgb_decode(c[1]), lb = ns_srgb_decode(c[2]);
    c[0] = ns_srgb_encode(1.2249401 * lr - 0.2249404 * lg);
    c[1] = ns_srgb_encode(-0.0420569 * lr + 1.0420571 * lg);
    c[2] = ns_srgb_encode(-0.0196376 * lr - 0.0786361 * lg + 1.0982735 * lb);
}

static gboolean
ns_canvas_parse_color_function(const char *s, double *r, double *g, double *b, double *a)
{
    while (g_ascii_isspace(*s)) s++;
    const char *close = strrchr(s, ')');
    if (g_ascii_strncasecmp(s, "color(", 6) != 0 || !close) return FALSE;
    char **tok = ns_color_fn_split(s + 6, (gsize)(close - s - 6));
    GPtrArray *parts = ns_color_fn_parts(tok);
    gboolean p3 = FALSE;
    double c[3] = { 0, 0, 0 }, alpha = 1;
    gboolean ok = ns_color_fn_shape(parts, &p3) && ns_color_fn_values(parts, c, &alpha);
    if (ok && p3) ns_display_p3_to_srgb(c);
    g_ptr_array_free(parts, TRUE);
    g_strfreev(tok);
    if (!ok) return FALSE;
    *r = CLAMP(c[0], 0, 1);
    *g = CLAMP(c[1], 0, 1);
    *b = CLAMP(c[2], 0, 1);
    *a = CLAMP(alpha, 0, 1);
    return TRUE;
}

gboolean
ns_canvas_color_is_literal(const char *s)
{
    const char *p;
    if (s[0] == '#') {
        for (p = s + 1; *p; p++)
            if (!g_ascii_isxdigit(*p)) return FALSE;
        return p - s > 1;
    }
    if (g_ascii_strncasecmp(s, "rgb", 3) != 0) return FALSE;
    p = s + 3;
    if (*p == 'a' || *p == 'A') p++;
    if (*p != '(') return FALSE;
    for (p++; *p && *p != ')'; p++)
        if (!g_ascii_isdigit(*p) && !strchr(" .,%/+-eE", *p)) return FALSE;
    return *p == ')' && p[1] == '\0';
}

typedef struct {
    char css[48];
    gboolean ok;
    double r, g, b, a;
} ns_canvas_color_memo;

static __thread ns_canvas_color_memo g_canvas_color_memo[16];
static __thread guint g_canvas_color_memo_next;

static gboolean
ns_canvas_parse_color_uncached(const char *s, double *r, double *g, double *b,
                               double *a)
{
    guint8 cr, cg, cb, ca;
    if (!ns_css_parse_color(s, &cr, &cg, &cb, &ca))
        return ns_canvas_parse_color_function(s, r, g, b, a);
    *r = cr / 255.0;
    *g = cg / 255.0;
    *b = cb / 255.0;
    *a = ca / 255.0;
    return TRUE;
}

gboolean
ns_canvas_parse_color(const char *s, double *r, double *g, double *b, double *a)
{
    gboolean memo = strlen(s) < sizeof g_canvas_color_memo[0].css &&
                    ns_canvas_color_is_literal(s);
    if (memo) {
        for (guint i = 0; i < G_N_ELEMENTS(g_canvas_color_memo); i++) {
            const ns_canvas_color_memo *m = &g_canvas_color_memo[i];
            if (strcmp(m->css, s) != 0) continue;
            if (!m->ok) return FALSE;
            *r = m->r;
            *g = m->g;
            *b = m->b;
            *a = m->a;
            return TRUE;
        }
    }
    double pr = 0, pg = 0, pb = 0, pa = 0;
    gboolean ok = ns_canvas_parse_color_uncached(s, &pr, &pg, &pb, &pa);
    if (memo) {
        ns_canvas_color_memo *m = &g_canvas_color_memo[
            g_canvas_color_memo_next++ % G_N_ELEMENTS(g_canvas_color_memo)];
        g_strlcpy(m->css, s, sizeof m->css);
        m->ok = ok;
        m->r = pr;
        m->g = pg;
        m->b = pb;
        m->a = pa;
    }
    if (!ok) return FALSE;
    *r = pr;
    *g = pg;
    *b = pb;
    *a = pa;
    return TRUE;
}

ns_canvas_state *
ns_canvas_state_for(ns_js *js, const ns_node *el)
{
    if (!js || !el) return NULL;
    if (!js->canvas_states)
        js->canvas_states = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                  NULL, ns_canvas_state_free);
    ns_canvas_state *st = g_hash_table_lookup(js->canvas_states, el);
    int w = ns_canvas_dim_from_attr(el, "width",  300);
    int h = ns_canvas_dim_from_attr(el, "height", 150);
    if (st && (st->w != w || st->h != h)) {
        ns_canvas_state_reset(st, w, h);
    } else if (!st) {
        st = g_new0(ns_canvas_state, 1);
        ns_canvas_state_reset(st, w, h);
        g_hash_table_insert(js->canvas_states, (gpointer)el, st);
    }
    return st;
}

void
ns_canvas_state_forget_node(ns_js *js, const ns_node *n)
{
    if (!js || !js->canvas_states || !n) return;
    ns_canvas_state *st = g_hash_table_lookup(js->canvas_states, n);
    if (!st || st->owned_node == n) return;
    g_hash_table_remove(js->canvas_states, n);
}

cairo_surface_t *
ns_js_canvas_surface(ns_js *js, const ns_node *n)
{
    if (!n) return NULL;
    cairo_surface_t *gl = ns_webgl_canvas_surface(n);
    if (gl) return gl;
#ifdef ND_HAVE_WEBGPU
    cairo_surface_t *gpu = ns_webgpu_canvas_surface(n);
    if (gpu) return gpu;
#endif
    if (!js || !js->canvas_states) return NULL;
    ns_canvas_state *st = g_hash_table_lookup(js->canvas_states, n);
    return st ? st->surf : NULL;
}

ns_canvas_state *
ns_ctx_state(JSContext *ctx, JSValueConst this_val)
{
    if (!js_from_ctx(ctx) || !ns_ctx2d_is(this_val)) return NULL;
    return ns_canvas_state_for(js_from_ctx(ctx), ns_hidden_ptr(this_val));
}

typedef struct { double pos, r, g, b, a; } ns_conic_stop;

static int
ns_conic_stop_cmp(const void *pa, const void *pb)
{
    double da = ((const ns_conic_stop *)pa)->pos;
    double db = ((const ns_conic_stop *)pb)->pos;
    return da < db ? -1 : da > db ? 1 : 0;
}

static void
ns_conic_color_at(const ns_conic_stop *stops, guint n, double t,
                  double *r, double *g, double *b, double *a)
{
    if (n == 0) { *r = *g = *b = 0; *a = 0; return; }
    if (t <= stops[0].pos) {
        *r = stops[0].r; *g = stops[0].g; *b = stops[0].b; *a = stops[0].a;
        return;
    }
    if (t >= stops[n - 1].pos) {
        *r = stops[n - 1].r; *g = stops[n - 1].g;
        *b = stops[n - 1].b; *a = stops[n - 1].a;
        return;
    }
    for (guint i = 1; i < n; i++) {
        if (t <= stops[i].pos) {
            double span = stops[i].pos - stops[i - 1].pos;
            double f = span > 0 ? (t - stops[i - 1].pos) / span : 0;
            *r = stops[i - 1].r + (stops[i].r - stops[i - 1].r) * f;
            *g = stops[i - 1].g + (stops[i].g - stops[i - 1].g) * f;
            *b = stops[i - 1].b + (stops[i].b - stops[i - 1].b) * f;
            *a = stops[i - 1].a + (stops[i].a - stops[i - 1].a) * f;
            return;
        }
    }
}

static cairo_pattern_t *
ns_ctx_build_conic_pattern(JSContext *ctx, JSValueConst obj)
{
    double cx = 0, cy = 0, angle = 0;
    JSValue v;
    v = ns_hget(ctx, obj, "_x0"); JS_ToFloat64(ctx, &cx, v); JS_FreeValue(ctx, v);
    v = ns_hget(ctx, obj, "_y0"); JS_ToFloat64(ctx, &cy, v); JS_FreeValue(ctx, v);
    v = ns_hget(ctx, obj, "_angle"); JS_ToFloat64(ctx, &angle, v); JS_FreeValue(ctx, v);

    GArray *sa = g_array_new(FALSE, FALSE, sizeof(ns_conic_stop));
    JSValue stops = ns_hget(ctx, obj, "_stops");
    if (JS_IsArray(stops)) {
        JSValue lenv = JS_GetPropertyStr(ctx, stops, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenv); JS_FreeValue(ctx, lenv);
        for (uint32_t i = 0; i < n; i++) {
            JSValue s = JS_GetPropertyUint32(ctx, stops, i);
            if (JS_IsObject(s)) {
                ns_conic_stop cs = { 0, 0, 0, 0, 1 };
                JSValue f;
                f = JS_GetPropertyStr(ctx, s, "pos"); JS_ToFloat64(ctx, &cs.pos, f); JS_FreeValue(ctx, f);
                f = JS_GetPropertyStr(ctx, s, "r");   JS_ToFloat64(ctx, &cs.r,   f); JS_FreeValue(ctx, f);
                f = JS_GetPropertyStr(ctx, s, "g");   JS_ToFloat64(ctx, &cs.g,   f); JS_FreeValue(ctx, f);
                f = JS_GetPropertyStr(ctx, s, "b");   JS_ToFloat64(ctx, &cs.b,   f); JS_FreeValue(ctx, f);
                f = JS_GetPropertyStr(ctx, s, "a");   JS_ToFloat64(ctx, &cs.a,   f); JS_FreeValue(ctx, f);
                g_array_append_val(sa, cs);
            }
            JS_FreeValue(ctx, s);
        }
    }
    JS_FreeValue(ctx, stops);
    if (sa->len == 0) { g_array_free(sa, TRUE); return NULL; }
    g_array_sort(sa, ns_conic_stop_cmp);
    const ns_conic_stop *cs = &g_array_index(sa, ns_conic_stop, 0);

    cairo_pattern_t *pat = cairo_pattern_create_mesh();
    const int sectors = 256;
    const double radius = 1e5;
    for (int i = 0; i < sectors; i++) {
        double t0 = (double)i / sectors;
        double t1 = (double)(i + 1) / sectors;
        double a0 = angle + t0 * 2.0 * G_PI;
        double a1 = angle + t1 * 2.0 * G_PI;
        double r0, g0, b0, al0, r1, g1, b1, al1;
        ns_conic_color_at(cs, sa->len, t0, &r0, &g0, &b0, &al0);
        ns_conic_color_at(cs, sa->len, t1, &r1, &g1, &b1, &al1);
        cairo_mesh_pattern_begin_patch(pat);
        cairo_mesh_pattern_move_to(pat, cx, cy);
        cairo_mesh_pattern_line_to(pat, cx + radius * cos(a0), cy + radius * sin(a0));
        cairo_mesh_pattern_line_to(pat, cx + radius * cos(a1), cy + radius * sin(a1));
        cairo_mesh_pattern_line_to(pat, cx, cy);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 0, r0, g0, b0, al0);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 1, r0, g0, b0, al0);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 2, r1, g1, b1, al1);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 3, r1, g1, b1, al1);
        cairo_mesh_pattern_end_patch(pat);
    }
    g_array_free(sa, TRUE);
    return pat;
}

cairo_pattern_t *
ns_ctx_build_pattern(JSContext *ctx, JSValueConst obj, gboolean *origin_clean)
{
    *origin_clean = TRUE;
    if (!JS_IsObject(obj)) return NULL;
    JSValue t = ns_hget(ctx, obj, "_type");
    if (!JS_IsString(t)) { JS_FreeValue(ctx, t); return NULL; }
    const char *type = JS_ToCString(ctx, t);
    JS_FreeValue(ctx, t);
    if (!type) return NULL;
    cairo_pattern_t *pat = NULL;
    if (strcmp(type, "linear") == 0) {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        JSValue v;
        v = ns_hget(ctx, obj, "_x0"); JS_ToFloat64(ctx, &x0, v); JS_FreeValue(ctx, v);
        v = ns_hget(ctx, obj, "_y0"); JS_ToFloat64(ctx, &y0, v); JS_FreeValue(ctx, v);
        v = ns_hget(ctx, obj, "_x1"); JS_ToFloat64(ctx, &x1, v); JS_FreeValue(ctx, v);
        v = ns_hget(ctx, obj, "_y1"); JS_ToFloat64(ctx, &y1, v); JS_FreeValue(ctx, v);
        pat = cairo_pattern_create_linear(x0, y0, x1, y1);
    } else if (strcmp(type, "pattern") == 0) {
        JS_FreeCString(ctx, type);
        JSValue node_v = ns_hget(ctx, obj, "_node");
        int iw = 0, ih = 0;
        cairo_surface_t *img =
            ns_ctx_drawimage_source(ctx, node_v, &iw, &ih, origin_clean);
        JS_FreeValue(ctx, node_v);
        if (!img) return NULL;
        pat = cairo_pattern_create_for_surface(img);
        cairo_surface_destroy(img);
        cairo_extend_t ext = CAIRO_EXTEND_REPEAT;
        JSValue rep_v = ns_hget(ctx, obj, "_rep");
        if (JS_IsString(rep_v)) {
            const char *r = JS_ToCString(ctx, rep_v);
            if (r) {
                if      (!strcmp(r, "repeat-x"))  ext = CAIRO_EXTEND_REPEAT;
                else if (!strcmp(r, "repeat-y"))  ext = CAIRO_EXTEND_REPEAT;
                else if (!strcmp(r, "no-repeat")) ext = CAIRO_EXTEND_NONE;
                else                              ext = CAIRO_EXTEND_REPEAT;
                JS_FreeCString(ctx, r);
            }
        }
        JS_FreeValue(ctx, rep_v);
        cairo_pattern_set_extend(pat, ext);
        JSValue m = ns_hget(ctx, obj, "_matrix");
        if (JS_IsArray(m)) {
            double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
            JSValue vv;
            vv = JS_GetPropertyUint32(ctx, m, 0); JS_ToFloat64(ctx, &a, vv); JS_FreeValue(ctx, vv);
            vv = JS_GetPropertyUint32(ctx, m, 1); JS_ToFloat64(ctx, &b, vv); JS_FreeValue(ctx, vv);
            vv = JS_GetPropertyUint32(ctx, m, 2); JS_ToFloat64(ctx, &c, vv); JS_FreeValue(ctx, vv);
            vv = JS_GetPropertyUint32(ctx, m, 3); JS_ToFloat64(ctx, &d, vv); JS_FreeValue(ctx, vv);
            vv = JS_GetPropertyUint32(ctx, m, 4); JS_ToFloat64(ctx, &e, vv); JS_FreeValue(ctx, vv);
            vv = JS_GetPropertyUint32(ctx, m, 5); JS_ToFloat64(ctx, &f, vv); JS_FreeValue(ctx, vv);
            cairo_matrix_t cm;
            cairo_matrix_init(&cm, a, b, c, d, e, f);
            cairo_matrix_t inv = cm;
            if (cairo_matrix_invert(&inv) == CAIRO_STATUS_SUCCESS)
                cairo_pattern_set_matrix(pat, &inv);
        }
        JS_FreeValue(ctx, m);
        return pat;
    } else if (strcmp(type, "radial") == 0) {
        double x0 = 0, y0 = 0, r0 = 0, x1 = 0, y1 = 0, r1 = 0;
        JSValue v;
        v = ns_hget(ctx, obj, "_x0"); JS_ToFloat64(ctx, &x0, v); JS_FreeValue(ctx, v);
        v = ns_hget(ctx, obj, "_y0"); JS_ToFloat64(ctx, &y0, v); JS_FreeValue(ctx, v);
        v = ns_hget(ctx, obj, "_r0"); JS_ToFloat64(ctx, &r0, v); JS_FreeValue(ctx, v);
        v = ns_hget(ctx, obj, "_x1"); JS_ToFloat64(ctx, &x1, v); JS_FreeValue(ctx, v);
        v = ns_hget(ctx, obj, "_y1"); JS_ToFloat64(ctx, &y1, v); JS_FreeValue(ctx, v);
        v = ns_hget(ctx, obj, "_r1"); JS_ToFloat64(ctx, &r1, v); JS_FreeValue(ctx, v);
        pat = cairo_pattern_create_radial(x0, y0, r0, x1, y1, r1);
    } else if (strcmp(type, "conic") == 0) {
        JS_FreeCString(ctx, type);
        return ns_ctx_build_conic_pattern(ctx, obj);
    }
    JS_FreeCString(ctx, type);
    if (!pat) return NULL;
    JSValue stops = ns_hget(ctx, obj, "_stops");
    if (JS_IsArray(stops)) {
        JSValue lenv = JS_GetPropertyStr(ctx, stops, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenv); JS_FreeValue(ctx, lenv);
        for (uint32_t i = 0; i < n; i++) {
            JSValue s = JS_GetPropertyUint32(ctx, stops, i);
            if (JS_IsObject(s)) {
                double pos = 0, r = 0, g = 0, b = 0, a = 1;
                JSValue f;
                f = JS_GetPropertyStr(ctx, s, "pos"); JS_ToFloat64(ctx, &pos, f); JS_FreeValue(ctx, f);
                f = JS_GetPropertyStr(ctx, s, "r");   JS_ToFloat64(ctx, &r,   f); JS_FreeValue(ctx, f);
                f = JS_GetPropertyStr(ctx, s, "g");   JS_ToFloat64(ctx, &g,   f); JS_FreeValue(ctx, f);
                f = JS_GetPropertyStr(ctx, s, "b");   JS_ToFloat64(ctx, &b,   f); JS_FreeValue(ctx, f);
                f = JS_GetPropertyStr(ctx, s, "a");   JS_ToFloat64(ctx, &a,   f); JS_FreeValue(ctx, f);
                cairo_pattern_add_color_stop_rgba(pat, pos, r, g, b, a);
            }
            JS_FreeValue(ctx, s);
        }
    }
    JS_FreeValue(ctx, stops);
    return pat;
}

double
ns_ctx_global_alpha(JSContext *ctx, JSValueConst this_val)
{
    JSValue v = ns_hget(ctx, this_val, "globalAlpha");
    double ga = 1.0;
    JS_ToFloat64(ctx, &ga, v);
    JS_FreeValue(ctx, v);
    if (ga < 0) ga = 0;
    if (ga > 1) ga = 1;
    return ga;
}

cairo_operator_t
ns_ctx_parse_composite(const char *s)
{
    if (!s)                                    return CAIRO_OPERATOR_OVER;
    if (!strcmp(s, "source-over"))             return CAIRO_OPERATOR_OVER;
    if (!strcmp(s, "source-in"))               return CAIRO_OPERATOR_IN;
    if (!strcmp(s, "source-out"))              return CAIRO_OPERATOR_OUT;
    if (!strcmp(s, "source-atop"))             return CAIRO_OPERATOR_ATOP;
    if (!strcmp(s, "destination-over"))        return CAIRO_OPERATOR_DEST_OVER;
    if (!strcmp(s, "destination-in"))          return CAIRO_OPERATOR_DEST_IN;
    if (!strcmp(s, "destination-out"))         return CAIRO_OPERATOR_DEST_OUT;
    if (!strcmp(s, "destination-atop"))        return CAIRO_OPERATOR_DEST_ATOP;
    if (!strcmp(s, "lighter"))                 return CAIRO_OPERATOR_ADD;
    if (!strcmp(s, "copy"))                    return CAIRO_OPERATOR_SOURCE;
    if (!strcmp(s, "xor"))                     return CAIRO_OPERATOR_XOR;
    if (!strcmp(s, "clear"))                   return CAIRO_OPERATOR_CLEAR;
    if (!strcmp(s, "multiply"))                return CAIRO_OPERATOR_MULTIPLY;
    if (!strcmp(s, "screen"))                  return CAIRO_OPERATOR_SCREEN;
    if (!strcmp(s, "overlay"))                 return CAIRO_OPERATOR_OVERLAY;
    if (!strcmp(s, "darken"))                  return CAIRO_OPERATOR_DARKEN;
    if (!strcmp(s, "lighten"))                 return CAIRO_OPERATOR_LIGHTEN;
    if (!strcmp(s, "color-dodge"))             return CAIRO_OPERATOR_COLOR_DODGE;
    if (!strcmp(s, "color-burn"))              return CAIRO_OPERATOR_COLOR_BURN;
    if (!strcmp(s, "hard-light"))              return CAIRO_OPERATOR_HARD_LIGHT;
    if (!strcmp(s, "soft-light"))              return CAIRO_OPERATOR_SOFT_LIGHT;
    if (!strcmp(s, "difference"))              return CAIRO_OPERATOR_DIFFERENCE;
    if (!strcmp(s, "exclusion"))               return CAIRO_OPERATOR_EXCLUSION;
    if (!strcmp(s, "hue"))                     return CAIRO_OPERATOR_HSL_HUE;
    if (!strcmp(s, "saturation"))              return CAIRO_OPERATOR_HSL_SATURATION;
    if (!strcmp(s, "color"))                   return CAIRO_OPERATOR_HSL_COLOR;
    if (!strcmp(s, "luminosity"))              return CAIRO_OPERATOR_HSL_LUMINOSITY;
    return CAIRO_OPERATOR_OVER;
}

void
ns_ctx_apply_composite(JSContext *ctx, JSValueConst this_val, cairo_t *cr)
{
    JSValue v = ns_hget(ctx, this_val, "globalCompositeOperation");
    cairo_operator_t op = CAIRO_OPERATOR_OVER;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) { op = ns_ctx_parse_composite(s); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    cairo_set_operator(cr, op);
}

gboolean
ns_ctx_image_smoothing(JSContext *ctx, JSValueConst this_val)
{
    JSValue v = ns_hget(ctx, this_val, "imageSmoothingEnabled");
    gboolean on = TRUE;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
        on = JS_ToBool(ctx, v) ? TRUE : FALSE;
    JS_FreeValue(ctx, v);
    return on;
}

void
ns_ctx_sync_styles(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st)
{
    JSValue v;
    if (st->fill_pattern) { cairo_pattern_destroy(st->fill_pattern); st->fill_pattern = NULL; }
    if (st->stroke_pattern) { cairo_pattern_destroy(st->stroke_pattern); st->stroke_pattern = NULL; }
    v = ns_hget(ctx, this_val, "fillStyle");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            double r, g, b, a;
            if (ns_canvas_parse_color(s, &r, &g, &b, &a)) {
                st->fill_r = r; st->fill_g = g; st->fill_b = b; st->fill_a = a;
            }
            JS_FreeCString(ctx, s);
        }
    } else if (JS_IsObject(v)) {
        gboolean clean = TRUE;
        st->fill_pattern = ns_ctx_build_pattern(ctx, v, &clean);
        if (!clean) st->origin_clean = FALSE;
    }
    JS_FreeValue(ctx, v);
    v = ns_hget(ctx, this_val, "strokeStyle");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            double r, g, b, a;
            if (ns_canvas_parse_color(s, &r, &g, &b, &a)) {
                st->stroke_r = r; st->stroke_g = g; st->stroke_b = b; st->stroke_a = a;
            }
            JS_FreeCString(ctx, s);
        }
    } else if (JS_IsObject(v)) {
        gboolean clean = TRUE;
        st->stroke_pattern = ns_ctx_build_pattern(ctx, v, &clean);
        if (!clean) st->origin_clean = FALSE;
    }
    JS_FreeValue(ctx, v);
    v = ns_hget(ctx, this_val, "lineWidth");
    double lw;
    if (JS_ToFloat64(ctx, &lw, v) == 0 && lw > 0) st->line_width = lw;
    JS_FreeValue(ctx, v);
    v = ns_hget(ctx, this_val, "font");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) { g_free(st->font); st->font = g_strdup(s); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    v = ns_hget(ctx, this_val, "lineCap");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            if      (strcmp(s, "round")  == 0) cairo_set_line_cap(st->cr, CAIRO_LINE_CAP_ROUND);
            else if (strcmp(s, "square") == 0) cairo_set_line_cap(st->cr, CAIRO_LINE_CAP_SQUARE);
            else                                cairo_set_line_cap(st->cr, CAIRO_LINE_CAP_BUTT);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);
    v = ns_hget(ctx, this_val, "lineJoin");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            if      (strcmp(s, "round") == 0) cairo_set_line_join(st->cr, CAIRO_LINE_JOIN_ROUND);
            else if (strcmp(s, "bevel") == 0) cairo_set_line_join(st->cr, CAIRO_LINE_JOIN_BEVEL);
            else                              cairo_set_line_join(st->cr, CAIRO_LINE_JOIN_MITER);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);
    v = ns_hget(ctx, this_val, "miterLimit");
    double ml;
    if (JS_ToFloat64(ctx, &ml, v) == 0 && ml > 0) cairo_set_miter_limit(st->cr, ml);
    JS_FreeValue(ctx, v);
    st->shadow_r = st->shadow_g = st->shadow_b = 0;
    st->shadow_a = 0;
    st->shadow_blur = st->shadow_ox = st->shadow_oy = 0;
    v = ns_hget(ctx, this_val, "shadowColor");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            double r, g, b, a;
            if (ns_canvas_parse_color(s, &r, &g, &b, &a)) {
                st->shadow_r = r; st->shadow_g = g;
                st->shadow_b = b; st->shadow_a = a;
            }
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);
    v = ns_hget(ctx, this_val, "shadowBlur");
    double sb = 0;
    if (JS_ToFloat64(ctx, &sb, v) == 0 && sb >= 0) st->shadow_blur = sb;
    JS_FreeValue(ctx, v);
    v = ns_hget(ctx, this_val, "shadowOffsetX");
    double sox = 0;
    if (JS_ToFloat64(ctx, &sox, v) == 0) st->shadow_ox = sox;
    JS_FreeValue(ctx, v);
    v = ns_hget(ctx, this_val, "shadowOffsetY");
    double soy = 0;
    if (JS_ToFloat64(ctx, &soy, v) == 0) st->shadow_oy = soy;
    JS_FreeValue(ctx, v);
    double dash_offset = 0;
    v = ns_hget(ctx, this_val, "lineDashOffset");
    JS_ToFloat64(ctx, &dash_offset, v);
    JS_FreeValue(ctx, v);
    v = ns_hget(ctx, this_val, "_dashes");
    if (JS_IsArray(v)) {
        uint32_t n = ns_js_array_length(ctx, v);
        if (n == 0) {
            cairo_set_dash(st->cr, NULL, 0, 0);
        } else {
            if (n > 64) n = 64;
            double dashes[64];
            double dash_sum = 0;
            for (uint32_t i = 0; i < n; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, v, i);
                double d = 0; JS_ToFloat64(ctx, &d, e); JS_FreeValue(ctx, e);
                if (d < 0) d = 0;
                dashes[i] = d;
                dash_sum += d;
            }
            if (dash_sum > 0)
                cairo_set_dash(st->cr, dashes, (int)n, dash_offset);
            else
                cairo_set_dash(st->cr, NULL, 0, 0);
        }
    } else {
        cairo_set_dash(st->cr, NULL, 0, 0);
    }
    JS_FreeValue(ctx, v);
}

gboolean
ns_ctx_has_shadow(const ns_canvas_state *st)
{
    if (!st || st->shadow_a <= 0) return FALSE;
    return st->shadow_ox != 0 || st->shadow_oy != 0 || st->shadow_blur > 0;
}

void
ns_box_blur_argb(uint8_t *data, int w, int h, int stride, int radius)
{
    if (radius <= 0 || w <= 0 || h <= 0) return;
    if (radius > 64) radius = 64;
    int span = radius * 2 + 1;
    uint8_t *tmp = g_try_malloc((size_t)w * (size_t)h * 4u);
    if (!tmp) return;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sa = 0, sr = 0, sg = 0, sb = 0, n = 0;
            for (int k = -radius; k <= radius; k++) {
                int xx = x + k;
                if (xx < 0 || xx >= w) continue;
                const uint8_t *p = data + y * stride + xx * 4;
                sb += p[0]; sg += p[1]; sr += p[2]; sa += p[3];
                n++;
            }
            uint8_t *q = tmp + (y * w + x) * 4;
            q[0] = (uint8_t)(sb / (n ? n : 1));
            q[1] = (uint8_t)(sg / (n ? n : 1));
            q[2] = (uint8_t)(sr / (n ? n : 1));
            q[3] = (uint8_t)(sa / (n ? n : 1));
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sa = 0, sr = 0, sg = 0, sb = 0, n = 0;
            for (int k = -radius; k <= radius; k++) {
                int yy = y + k;
                if (yy < 0 || yy >= h) continue;
                const uint8_t *p = tmp + (yy * w + x) * 4;
                sb += p[0]; sg += p[1]; sr += p[2]; sa += p[3];
                n++;
            }
            uint8_t *q = data + y * stride + x * 4;
            q[0] = (uint8_t)(sb / (n ? n : 1));
            q[1] = (uint8_t)(sg / (n ? n : 1));
            q[2] = (uint8_t)(sr / (n ? n : 1));
            q[3] = (uint8_t)(sa / (n ? n : 1));
        }
    }
    g_free(tmp);
    (void)span;
}

void
ns_ctx_with_shadow(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st,
                   ns_ctx_drawfn draw, void *ud)
{
    if (!ns_ctx_has_shadow(st)) {
        draw(st->cr, ud);
        return;
    }
    int w = st->w, h = st->h;
    if (w <= 0 || h <= 0) { draw(st->cr, ud); return; }
    cairo_surface_t *off = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(off) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(off);
        draw(st->cr, ud);
        return;
    }
    cairo_t *ocr = cairo_create(off);
    cairo_matrix_t m;
    cairo_get_matrix(st->cr, &m);
    cairo_set_matrix(ocr, &m);
    draw(ocr, ud);
    cairo_destroy(ocr);
    cairo_surface_flush(off);
    uint8_t *data = cairo_image_surface_get_data(off);
    int stride = cairo_image_surface_get_stride(off);
    int sw = cairo_image_surface_get_width(off);
    int sh = cairo_image_surface_get_height(off);
    if (data) {
        for (int y = 0; y < sh; y++) {
            uint8_t *row = data + y * stride;
            for (int x = 0; x < sw; x++) {
                uint8_t a = row[x * 4 + 3];
                uint8_t na = (uint8_t)(a * st->shadow_a);
                row[x * 4 + 0] = (uint8_t)(st->shadow_b * na);
                row[x * 4 + 1] = (uint8_t)(st->shadow_g * na);
                row[x * 4 + 2] = (uint8_t)(st->shadow_r * na);
                row[x * 4 + 3] = na;
            }
        }
        double half_blur = st->shadow_blur * 0.5 + 0.5;
        int radius = half_blur < 64 ? (int)half_blur : 64;
        if (radius > 0)
            ns_box_blur_argb(data, sw, sh, stride, radius);
        cairo_surface_mark_dirty(off);
    }
    cairo_save(st->cr);
    cairo_identity_matrix(st->cr);
    cairo_set_source_surface(st->cr, off, st->shadow_ox, st->shadow_oy);
    cairo_paint(st->cr);
    cairo_restore(st->cr);
    cairo_surface_destroy(off);
    draw(st->cr, ud);
    (void)ctx; (void)this_val;
}

void
ns_ctx_set_fill_source(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st)
{
    double ga = ns_ctx_global_alpha(ctx, this_val);
    if (st->fill_pattern) cairo_set_source(st->cr, st->fill_pattern);
    else cairo_set_source_rgba(st->cr, st->fill_r, st->fill_g, st->fill_b,
                               st->fill_a * ga);
}

void
ns_ctx_set_stroke_source(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st)
{
    double ga = ns_ctx_global_alpha(ctx, this_val);
    if (st->stroke_pattern) cairo_set_source(st->cr, st->stroke_pattern);
    else cairo_set_source_rgba(st->cr, st->stroke_r, st->stroke_g, st->stroke_b,
                               st->stroke_a * ga);
}

void
ns_draw_fillrect(cairo_t *cr, void *vud)
{
    ns_draw_rect_ud *u = vud;
    if (cr == u->st->cr) ns_ctx_set_fill_source(u->ctx, u->this_val, u->st);
    else {
        double ga = ns_ctx_global_alpha(u->ctx, u->this_val);
        cairo_set_source_rgba(cr, u->st->fill_r, u->st->fill_g,
                              u->st->fill_b, u->st->fill_a * ga);
    }
    ns_ctx_apply_composite(u->ctx, u->this_val, cr);
    cairo_rectangle(cr, u->x, u->y, u->w, u->h);
    cairo_fill(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

void
ns_draw_strokerect(cairo_t *cr, void *vud)
{
    ns_draw_rect_ud *u = vud;
    if (cr == u->st->cr) ns_ctx_set_stroke_source(u->ctx, u->this_val, u->st);
    else {
        double ga = ns_ctx_global_alpha(u->ctx, u->this_val);
        cairo_set_source_rgba(cr, u->st->stroke_r, u->st->stroke_g,
                              u->st->stroke_b, u->st->stroke_a * ga);
    }
    ns_ctx_apply_composite(u->ctx, u->this_val, cr);
    cairo_set_line_width(cr, u->lw);
    cairo_rectangle(cr, u->x, u->y, u->w, u->h);
    cairo_stroke(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

JSValue
ns_ctx_fillRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    ns_ctx_sync_styles(ctx, this_val, st);
    ns_draw_rect_ud u = {
        .x = ns_arg_d(ctx, argv[0]), .y = ns_arg_d(ctx, argv[1]),
        .w = ns_arg_d(ctx, argv[2]), .h = ns_arg_d(ctx, argv[3]),
        .lw = st->line_width, .ctx = ctx, .this_val = this_val, .st = st,
    };
    ns_ctx_with_shadow(ctx, this_val, st, ns_draw_fillrect, &u);
    { ns_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

JSValue
ns_ctx_strokeRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    ns_ctx_sync_styles(ctx, this_val, st);
    ns_draw_rect_ud u = {
        .x = ns_arg_d(ctx, argv[0]), .y = ns_arg_d(ctx, argv[1]),
        .w = ns_arg_d(ctx, argv[2]), .h = ns_arg_d(ctx, argv[3]),
        .lw = st->line_width, .ctx = ctx, .this_val = this_val, .st = st,
    };
    ns_ctx_with_shadow(ctx, this_val, st, ns_draw_strokerect, &u);
    { ns_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

JSValue
ns_ctx_clearRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    double x = ns_arg_d(ctx, argv[0]), y = ns_arg_d(ctx, argv[1]);
    double w = ns_arg_d(ctx, argv[2]), h = ns_arg_d(ctx, argv[3]);
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    cairo_save(st->cr);
    cairo_set_operator(st->cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(st->cr, x, y, w, h);
    cairo_fill(st->cr);
    cairo_restore(st->cr);
    { ns_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

JSValue
ns_ctx_beginPath(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (st) cairo_new_path(st->cr);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_closePath(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (st) cairo_close_path(st->cr);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_moveTo(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    double x = ns_arg_d(ctx, argv[0]), y = ns_arg_d(ctx, argv[1]);
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (st) cairo_move_to(st->cr, x, y);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_lineTo(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    double x = ns_arg_d(ctx, argv[0]), y = ns_arg_d(ctx, argv[1]);
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (st) cairo_line_to(st->cr, x, y);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_arc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 5) return JS_UNDEFINED;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    double x = ns_arg_d(ctx, argv[0]);
    double y = ns_arg_d(ctx, argv[1]);
    double r = ns_arg_d(ctx, argv[2]);
    double a0 = ns_arg_d(ctx, argv[3]);
    double a1 = ns_arg_d(ctx, argv[4]);
    if (r < 0)
        return ns_canvas_throw_dom(ctx, "IndexSizeError",
                                   "arc radius must not be negative");
    gboolean ccw = argc >= 6 && JS_ToBool(ctx, argv[5]);
    if (ccw) cairo_arc_negative(st->cr, x, y, r, a0, a1);
    else     cairo_arc(st->cr, x, y, r, a0, a1);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    double x = ns_arg_d(ctx, argv[0]), y = ns_arg_d(ctx, argv[1]);
    double w = ns_arg_d(ctx, argv[2]), h = ns_arg_d(ctx, argv[3]);
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (st) cairo_rectangle(st->cr, x, y, w, h);
    return JS_UNDEFINED;
}

gboolean
ns_value_is_path2d(JSValueConst v)
{
    if (!JS_IsObject(v)) return FALSE;
    return JS_GetOpaque(v, ns_path2d_class_id) != NULL;
}

void
ns_replay_path2d(cairo_t *target, JSValueConst path_v)
{
    ns_path2d *p = JS_GetOpaque(path_v, ns_path2d_class_id);
    if (!p) return;
    cairo_path_t *cp = cairo_copy_path(p->cr);
    cairo_new_path(target);
    cairo_append_path(target, cp);
    cairo_path_destroy(cp);
}

cairo_fill_rule_t
ns_parse_fill_rule(const char *s)
{
    if (s && !strcmp(s, "evenodd")) return CAIRO_FILL_RULE_EVEN_ODD;
    return CAIRO_FILL_RULE_WINDING;
}

cairo_path_t *
ns_ctx_prepare_path_and_rule(JSContext *ctx, cairo_t *cr,
                             int argc, JSValueConst *argv)
{
    JSValueConst path_v = JS_UNDEFINED;
    const char *rule_s = NULL;
    if (argc >= 1 && ns_value_is_path2d(argv[0])) {
        path_v = argv[0];
        if (argc >= 2 && JS_IsString(argv[1]))
            rule_s = JS_ToCString(ctx, argv[1]);
    } else if (argc >= 1 && JS_IsString(argv[0])) {
        rule_s = JS_ToCString(ctx, argv[0]);
    }
    cairo_path_t *saved = NULL;
    if (!JS_IsUndefined(path_v)) {
        saved = cairo_copy_path(cr);
        ns_replay_path2d(cr, path_v);
    }
    cairo_set_fill_rule(cr, ns_parse_fill_rule(rule_s));
    if (rule_s) JS_FreeCString(ctx, rule_s);
    return saved;
}

void
ns_ctx_restore_path(cairo_t *cr, cairo_path_t *saved)
{
    if (!saved) return;
    cairo_new_path(cr);
    cairo_append_path(cr, saved);
    cairo_path_destroy(saved);
}

void
ns_draw_fillpath(cairo_t *cr, void *vud)
{
    ns_draw_path_ud *u = vud;
    if (cr != u->st->cr) {
        cairo_new_path(cr);
        if (u->snapshot) cairo_append_path(cr, u->snapshot);
        cairo_set_source_rgba(cr, u->st->fill_r, u->st->fill_g,
                              u->st->fill_b, u->st->fill_a *
                              ns_ctx_global_alpha(u->ctx, u->this_val));
    } else {
        ns_ctx_set_fill_source(u->ctx, u->this_val, u->st);
    }
    cairo_set_fill_rule(cr, u->fill_rule);
    ns_ctx_apply_composite(u->ctx, u->this_val, cr);
    cairo_fill_preserve(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

void
ns_draw_strokepath(cairo_t *cr, void *vud)
{
    ns_draw_path_ud *u = vud;
    if (cr != u->st->cr) {
        cairo_new_path(cr);
        if (u->snapshot) cairo_append_path(cr, u->snapshot);
        cairo_set_source_rgba(cr, u->st->stroke_r, u->st->stroke_g,
                              u->st->stroke_b, u->st->stroke_a *
                              ns_ctx_global_alpha(u->ctx, u->this_val));
    } else {
        ns_ctx_set_stroke_source(u->ctx, u->this_val, u->st);
    }
    ns_ctx_apply_composite(u->ctx, u->this_val, cr);
    cairo_set_line_width(cr, u->lw);
    cairo_stroke_preserve(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

JSValue
ns_ctx_fill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    ns_ctx_sync_styles(ctx, this_val, st);
    cairo_path_t *saved = ns_ctx_prepare_path_and_rule(ctx, st->cr, argc, argv);
    cairo_path_t *snap = ns_ctx_has_shadow(st) ? cairo_copy_path(st->cr) : NULL;
    ns_draw_path_ud u = {
        .ctx = ctx, .this_val = this_val, .st = st,
        .lw = st->line_width, .snapshot = snap,
        .fill_rule = cairo_get_fill_rule(st->cr),
    };
    ns_ctx_with_shadow(ctx, this_val, st, ns_draw_fillpath, &u);
    if (snap) cairo_path_destroy(snap);
    ns_ctx_restore_path(st->cr, saved);
    { ns_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

JSValue
ns_ctx_stroke(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    ns_ctx_sync_styles(ctx, this_val, st);
    cairo_path_t *saved = NULL;
    if (argc >= 1 && ns_value_is_path2d(argv[0])) {
        saved = cairo_copy_path(st->cr);
        ns_replay_path2d(st->cr, argv[0]);
    }
    cairo_path_t *snap = ns_ctx_has_shadow(st) ? cairo_copy_path(st->cr) : NULL;
    ns_draw_path_ud u = {
        .ctx = ctx, .this_val = this_val, .st = st,
        .lw = st->line_width, .snapshot = snap,
        .fill_rule = cairo_get_fill_rule(st->cr),
    };
    ns_ctx_with_shadow(ctx, this_val, st, ns_draw_strokepath, &u);
    if (snap) cairo_path_destroy(snap);
    ns_ctx_restore_path(st->cr, saved);
    { ns_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

static const char *ns_ctx_savable_props[] = {
    "fillStyle", "strokeStyle", "font", "textAlign", "textBaseline",
    "direction", "globalAlpha", "globalCompositeOperation",
    "shadowColor", "shadowBlur", "shadowOffsetX", "shadowOffsetY",
    "imageSmoothingEnabled", "imageSmoothingQuality",
    "lineWidth", "lineCap", "lineJoin", "miterLimit", "lineDashOffset",
    "filter", "letterSpacing", "wordSpacing",
    "fontKerning", "fontStretch", "fontVariantCaps", "textRendering",
    "_dashes",
};

JSValue
ns_ctx_save(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    cairo_save(st->cr);
    JSValue stack = ns_hget(ctx, this_val, "_stateStack");
    if (!JS_IsArray(stack)) {
        JS_FreeValue(ctx, stack);
        stack = JS_NewArray(ctx);
        ns_hset(ctx, this_val, "_stateStack", JS_DupValue(ctx, stack));
    }
    JSValue snap = JS_NewObjectProto(ctx, JS_NULL);
    for (gsize i = 0; i < G_N_ELEMENTS(ns_ctx_savable_props); i++) {
        JSValue v = ns_hget(ctx, this_val, ns_ctx_savable_props[i]);
        JS_DefinePropertyValueStr(ctx, snap, ns_ctx_savable_props[i], v,
                                  JS_PROP_C_W_E);
    }
    uint32_t n = ns_js_array_length(ctx, stack);
    JS_DefinePropertyValueUint32(ctx, stack, n, snap, JS_PROP_C_W_E);
    JS_FreeValue(ctx, stack);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_restore(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    JSValue stack = ns_hget(ctx, this_val, "_stateStack");
    if (!JS_IsArray(stack)) { JS_FreeValue(ctx, stack); return JS_UNDEFINED; }
    uint32_t n = ns_js_array_length(ctx, stack);
    if (n == 0) { JS_FreeValue(ctx, stack); return JS_UNDEFINED; }
    JSValue snap = JS_GetPropertyUint32(ctx, stack, n - 1);
    for (gsize i = 0; i < G_N_ELEMENTS(ns_ctx_savable_props); i++) {
        JSValue v = JS_GetPropertyStr(ctx, snap, ns_ctx_savable_props[i]);
        ns_hset(ctx, this_val, ns_ctx_savable_props[i], v);
    }
    JS_FreeValue(ctx, snap);
    JSAtom len_atom = JS_NewAtom(ctx, "length");
    JS_SetProperty(ctx, stack, len_atom, JS_NewUint32(ctx, n - 1));
    JS_FreeAtom(ctx, len_atom);
    JS_FreeValue(ctx, stack);
    cairo_restore(st->cr);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_translate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    double tx = ns_arg_d(ctx, argv[0]), ty = ns_arg_d(ctx, argv[1]);
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (st) cairo_translate(st->cr, tx, ty);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_scale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    double sx = ns_arg_d(ctx, argv[0]), sy = ns_arg_d(ctx, argv[1]);
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (st) cairo_scale(st->cr, sx, sy);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_rotate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    double angle = ns_arg_d(ctx, argv[0]);
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (st) cairo_rotate(st->cr, angle);
    return JS_UNDEFINED;
}

static gboolean
ns_canvas_font_size_token(const char *tok, double *size_px)
{
    static const struct { const char *name; double px; } keywords[] = {
        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 },
        { "medium", 16 }, { "large", 18 }, { "x-large", 24 },
        { "xx-large", 32 }, { "xxx-large", 48 },
        { "smaller", 13 }, { "larger", 19 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(keywords); i++) {
        gsize n = strlen(keywords[i].name);
        if (g_ascii_strncasecmp(tok, keywords[i].name, n) == 0 &&
            (tok[n] == '\0' || tok[n] == '/')) {
            *size_px = keywords[i].px;
            return TRUE;
        }
    }
    if (!g_ascii_isdigit(tok[0]) && tok[0] != '.') return FALSE;
    char *endp = NULL;
    double v = g_ascii_strtod(tok, &endp);
    if (!endp || endp == tok) return FALSE;
    static const struct { const char *unit; double scale; } units[] = {
        { "px", 1.0 }, { "pt", 96.0 / 72.0 }, { "pc", 16.0 },
        { "in", 96.0 }, { "cm", 96.0 / 2.54 }, { "mm", 96.0 / 25.4 },
        { "q", 96.0 / 101.6 }, { "rem", 16.0 }, { "em", 16.0 },
        { "ex", 8.0 }, { "ch", 8.0 }, { "%", 0.16 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(units); i++) {
        gsize n = strlen(units[i].unit);
        if (g_ascii_strncasecmp(endp, units[i].unit, n) == 0 &&
            (endp[n] == '\0' || endp[n] == '/')) {
            *size_px = v * units[i].scale;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
ns_canvas_font_prefix_token(const char *tok, NsPangoFontDescription *desc)
{
    if (g_ascii_strcasecmp(tok, "italic") == 0) {
        ns_pango_font_description_set_style(desc, NS_PANGO_STYLE_ITALIC);
        return TRUE;
    }
    if (g_ascii_strcasecmp(tok, "oblique") == 0) {
        ns_pango_font_description_set_style(desc, NS_PANGO_STYLE_OBLIQUE);
        return TRUE;
    }
    if (g_ascii_strcasecmp(tok, "bold") == 0 ||
        g_ascii_strcasecmp(tok, "bolder") == 0) {
        ns_pango_font_description_set_weight(desc, NS_PANGO_WEIGHT_BOLD);
        return TRUE;
    }
    if (g_ascii_strcasecmp(tok, "lighter") == 0) {
        ns_pango_font_description_set_weight(desc, NS_PANGO_WEIGHT_LIGHT);
        return TRUE;
    }
    if (g_ascii_isdigit(tok[0])) {
        char *endp = NULL;
        gint64 weight = g_ascii_strtoll(tok, &endp, 10);
        if (endp && *endp == '\0' && weight >= 1 && weight <= 1000) {
            ns_pango_font_description_set_weight(desc, (NsPangoWeight)weight);
            return TRUE;
        }
        return FALSE;
    }
    static const char *const ignored[] = {
        "normal", "small-caps", "ultra-condensed", "extra-condensed", "condensed",
        "semi-condensed", "semi-expanded", "expanded", "extra-expanded",
        "ultra-expanded",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(ignored); i++)
        if (g_ascii_strcasecmp(tok, ignored[i]) == 0) return TRUE;
    return FALSE;
}

NsPangoFontDescription *
ns_canvas_font_desc(const char *css_font)
{
    const char *src = css_font && *css_font ? css_font : "10px sans-serif";
    NsPangoFontDescription *desc = ns_pango_font_description_new();
    char **tokens = g_strsplit_set(src, " \t\n\r\f", -1);
    double size_px = 10.0;
    gboolean found_size = FALSE;
    gboolean skip_line_height = FALSE;
    GString *family = g_string_new(NULL);
    for (char **t = tokens; *t; t++) {
        const char *tok = *t;
        if (!*tok) continue;
        if (!found_size) {
            if (ns_canvas_font_size_token(tok, &size_px)) {
                found_size = TRUE;
                const char *slash = strchr(tok, '/');
                skip_line_height = slash && slash[1] == '\0';
                continue;
            }
            if (ns_canvas_font_prefix_token(tok, desc)) continue;
            found_size = TRUE;
        }
        if (skip_line_height) {
            skip_line_height = FALSE;
            continue;
        }
        if (!family->len && tok[0] == '/') {
            skip_line_height = tok[1] == '\0';
            continue;
        }
        if (family->len) g_string_append_c(family, ' ');
        g_string_append(family, tok);
    }
    g_strfreev(tokens);
    char *pango_family = ns_css_font_family_for_pango(
        family->len ? family->str : "sans-serif");
    ns_pango_font_description_set_family(desc,
        pango_family && *pango_family ? pango_family : "sans-serif");
    g_free(pango_family);
    g_string_free(family, TRUE);
    if (!(size_px > 0)) size_px = 10;
    ns_pango_font_description_set_absolute_size(
        desc, ns_paint_pango_font_size(size_px));
    return desc;
}

gboolean
ns_ctx_direction_is_rtl(JSContext *ctx, JSValueConst this_val)
{
    JSValue v = ns_hget(ctx, this_val, "direction");
    gboolean rtl = FALSE;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) { rtl = strcmp(s, "rtl") == 0; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return rtl;
}

void
ns_ctx_paint_text(JSContext *ctx, JSValueConst this_val,
                  ns_canvas_state *st, const char *text,
                  double x, double y, double max_width,
                  gboolean stroke)
{
    NsPangoLayout *layout = ns_pango_cairo_create_layout(st->cr);
    NsPangoFontDescription *desc = ns_canvas_font_desc(st->font);
    ns_pango_layout_set_font_description(layout, desc);
    ns_pango_layout_set_text(layout, text, -1);
    NsPangoRectangle ink, logical;
    ns_pango_layout_get_extents(layout, &ink, &logical);
    double baseline_offset =
        (double)ns_pango_layout_get_baseline(layout) / NS_PANGO_SCALE;
    JSValue baseline_v = ns_hget(ctx, this_val, "textBaseline");
    double dy = 0;
    if (JS_IsString(baseline_v)) {
        const char *bs = JS_ToCString(ctx, baseline_v);
        if (bs) {
            if      (!strcmp(bs, "top"))         dy = 0;
            else if (!strcmp(bs, "hanging"))     dy = -baseline_offset * 0.2;
            else if (!strcmp(bs, "middle"))      dy = -baseline_offset * 0.5;
            else if (!strcmp(bs, "ideographic")) dy = -(double)(logical.y + logical.height) / NS_PANGO_SCALE;
            else                                  dy = -baseline_offset;
            JS_FreeCString(ctx, bs);
        }
    } else {
        dy = -baseline_offset;
    }
    JS_FreeValue(ctx, baseline_v);
    gboolean rtl = ns_ctx_direction_is_rtl(ctx, this_val);
    JSValue align_v = ns_hget(ctx, this_val, "textAlign");
    double dx = 0;
    double tw = (double)logical.width / NS_PANGO_SCALE;
    const char *align = "start";
    char align_buf[16];
    if (JS_IsString(align_v)) {
        const char *as = JS_ToCString(ctx, align_v);
        if (as) {
            g_strlcpy(align_buf, as, sizeof align_buf);
            align = align_buf;
            JS_FreeCString(ctx, as);
        }
    }
    JS_FreeValue(ctx, align_v);
    if      (!strcmp(align, "center"))                       dx = -tw / 2;
    else if (!strcmp(align, "right"))                        dx = -tw;
    else if (!strcmp(align, "left"))                         dx = 0;
    else if (!strcmp(align, "end")   && !rtl)                dx = -tw;
    else if (!strcmp(align, "end")   &&  rtl)                dx = 0;
    else if (!strcmp(align, "start") &&  rtl)                dx = -tw;
    double xscale = 1.0;
    if (max_width > 0 && tw > max_width) xscale = max_width / tw;
    cairo_save(st->cr);
    ns_ctx_apply_composite(ctx, this_val, st->cr);
    cairo_translate(st->cr, x + dx * xscale, y + dy);
    if (xscale < 1.0) cairo_scale(st->cr, xscale, 1.0);
    if (stroke) {
        ns_ctx_set_stroke_source(ctx, this_val, st);
        cairo_set_line_width(st->cr, st->line_width);
        cairo_move_to(st->cr, 0, 0);
        ns_pango_cairo_layout_path(st->cr, layout);
        cairo_stroke(st->cr);
    } else {
        ns_ctx_set_fill_source(ctx, this_val, st);
        cairo_move_to(st->cr, 0, 0);
        ns_pango_cairo_show_layout(st->cr, layout);
    }
    cairo_restore(st->cr);
    ns_pango_font_description_free(desc);
    g_object_unref(layout);
}

JSValue
ns_ctx_fillText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 3) return JS_UNDEFINED;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    ns_ctx_sync_styles(ctx, this_val, st);
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_UNDEFINED;
    double x = ns_arg_d(ctx, argv[1]);
    double y = ns_arg_d(ctx, argv[2]);
    double mw = argc >= 4 ? ns_arg_d(ctx, argv[3]) : 0;
    ns_ctx_paint_text(ctx, this_val, st, text, x, y, mw, FALSE);
    JS_FreeCString(ctx, text);
    { ns_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

JSValue
ns_ctx_measureText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    double width = 0, ascent = 0, descent = 0;
    double font_ascent = 0, font_descent = 0;
    ns_canvas_state *st = argc >= 1 ? ns_ctx_state(ctx, this_val) : NULL;
    const char *text = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (text && st) {
        ns_ctx_sync_styles(ctx, this_val, st);
        NsPangoLayout *layout = ns_pango_cairo_create_layout(st->cr);
        NsPangoFontDescription *desc = ns_canvas_font_desc(st->font);
        ns_pango_layout_set_font_description(layout, desc);
        ns_pango_layout_set_text(layout, text, -1);
        NsPangoRectangle ink, logical;
        ns_pango_layout_get_extents(layout, &ink, &logical);
        double baseline_y = (double)ns_pango_layout_get_baseline(layout);
        width = (double)logical.width / NS_PANGO_SCALE;
        ascent  = (baseline_y - (double)ink.y) / NS_PANGO_SCALE;
        descent = ((double)(ink.y + ink.height) - baseline_y) / NS_PANGO_SCALE;
        if (ascent < 0) ascent = 0;
        if (descent < 0) descent = 0;
        NsPangoContext *pctx = ns_pango_layout_get_context(layout);
        NsPangoFontMetrics *fm = ns_pango_context_get_metrics(pctx, desc, NULL);
        if (fm) {
            font_ascent  = (double)ns_pango_font_metrics_get_ascent(fm)  / NS_PANGO_SCALE;
            font_descent = (double)ns_pango_font_metrics_get_descent(fm) / NS_PANGO_SCALE;
            ns_pango_font_metrics_unref(fm);
        }
        ns_pango_font_description_free(desc);
        g_object_unref(layout);
    }
    if (text) JS_FreeCString(ctx, text);
    const double metrics[10] = {
        width, 0, width, ascent, descent, font_ascent, font_descent,
        font_ascent * 0.8, 0, -font_descent,
    };
    return ns_textmetrics_new(ctx, ns_ctx_realm(ctx, this_val), metrics);
}

JSValue
ns_ctx_quadraticCurveTo(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    double cpx = ns_arg_d(ctx, argv[0]), cpy = ns_arg_d(ctx, argv[1]);
    double x   = ns_arg_d(ctx, argv[2]), y   = ns_arg_d(ctx, argv[3]);
    double x0, y0;
    if (!cairo_has_current_point(st->cr))
        cairo_move_to(st->cr, cpx, cpy);
    cairo_get_current_point(st->cr, &x0, &y0);
    cairo_curve_to(st->cr,
                   x0 + 2.0 / 3.0 * (cpx - x0), y0 + 2.0 / 3.0 * (cpy - y0),
                   x  + 2.0 / 3.0 * (cpx - x),  y  + 2.0 / 3.0 * (cpy - y),
                   x, y);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_bezierCurveTo(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    if (argc < 6) return JS_UNDEFINED;
    double x1 = ns_arg_d(ctx, argv[0]), y1 = ns_arg_d(ctx, argv[1]);
    double x2 = ns_arg_d(ctx, argv[2]), y2 = ns_arg_d(ctx, argv[3]);
    double x3 = ns_arg_d(ctx, argv[4]), y3 = ns_arg_d(ctx, argv[5]);
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (st) cairo_curve_to(st->cr, x1, y1, x2, y2, x3, y3);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_arcTo(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    if (argc < 5) return JS_UNDEFINED;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    double x1 = ns_arg_d(ctx, argv[0]), y1 = ns_arg_d(ctx, argv[1]);
    double x2 = ns_arg_d(ctx, argv[2]), y2 = ns_arg_d(ctx, argv[3]);
    double r  = ns_arg_d(ctx, argv[4]);
    if (r < 0)
        return ns_canvas_throw_dom(ctx, "IndexSizeError",
                                   "arcTo radius must not be negative");
    double x0, y0;
    if (!cairo_has_current_point(st->cr))
        cairo_move_to(st->cr, x1, y1);
    cairo_get_current_point(st->cr, &x0, &y0);
    double a1x = x0 - x1, a1y = y0 - y1;
    double a2x = x2 - x1, a2y = y2 - y1;
    double l1 = hypot(a1x, a1y), l2 = hypot(a2x, a2y);
    if (l1 == 0 || l2 == 0 || r == 0) { cairo_line_to(st->cr, x1, y1); return JS_UNDEFINED; }
    double u1x = a1x / l1, u1y = a1y / l1;
    double u2x = a2x / l2, u2y = a2y / l2;
    double cos_t = u1x * u2x + u1y * u2y;
    if (cos_t >= 1.0 || cos_t <= -1.0) {
        cairo_line_to(st->cr, x1, y1);
        return JS_UNDEFINED;
    }
    double tan_half = sqrt((1 - cos_t) / (1 + cos_t));
    double dist = r / tan_half;
    double t1x = x1 + u1x * dist, t1y = y1 + u1y * dist;
    double t2x = x1 + u2x * dist, t2y = y1 + u2y * dist;
    double bisx = (u1x + u2x), bisy = (u1y + u2y);
    double blen = hypot(bisx, bisy);
    if (blen == 0) { cairo_line_to(st->cr, t1x, t1y); return JS_UNDEFINED; }
    bisx /= blen; bisy /= blen;
    double cdist = sqrt(r * r + dist * dist);
    double cx = x1 + bisx * cdist, cy = y1 + bisy * cdist;
    double ang1 = atan2(t1y - cy, t1x - cx);
    double ang2 = atan2(t2y - cy, t2x - cx);
    double cross = u1x * u2y - u1y * u2x;
    cairo_line_to(st->cr, t1x, t1y);
    if (cross < 0) cairo_arc_negative(st->cr, cx, cy, r, ang1, ang2);
    else           cairo_arc(st->cr, cx, cy, r, ang1, ang2);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_ellipse(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    if (argc < 7) return JS_UNDEFINED;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    double x  = ns_arg_d(ctx, argv[0]);
    double y  = ns_arg_d(ctx, argv[1]);
    double rx = ns_arg_d(ctx, argv[2]);
    double ry = ns_arg_d(ctx, argv[3]);
    double rot = ns_arg_d(ctx, argv[4]);
    double a0 = ns_arg_d(ctx, argv[5]);
    double a1 = ns_arg_d(ctx, argv[6]);
    if (rx < 0 || ry < 0)
        return ns_canvas_throw_dom(ctx, "IndexSizeError",
                                   "ellipse radius must not be negative");
    gboolean ccw = argc >= 8 && JS_ToBool(ctx, argv[7]);
    cairo_save(st->cr);
    cairo_translate(st->cr, x, y);
    cairo_rotate(st->cr, rot);
    cairo_scale(st->cr, rx, ry);
    if (ccw) cairo_arc_negative(st->cr, 0, 0, 1.0, a0, a1);
    else     cairo_arc(st->cr, 0, 0, 1.0, a0, a1);
    cairo_restore(st->cr);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_clip(JSContext *ctx, JSValueConst this_val,
            int argc, JSValueConst *argv)
{
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    cairo_path_t *saved = ns_ctx_prepare_path_and_rule(ctx, st->cr, argc, argv);
    cairo_clip_preserve(st->cr);
    ns_ctx_restore_path(st->cr, saved);
    return JS_UNDEFINED;
}

gboolean
ns_matrix_from_obj(JSContext *ctx, JSValueConst v, cairo_matrix_t *m)
{
    if (!JS_IsObject(v)) return FALSE;
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    JSValue tmp;
    tmp = JS_GetPropertyStr(ctx, v, "a"); JS_ToFloat64(ctx, &a, tmp); JS_FreeValue(ctx, tmp);
    tmp = JS_GetPropertyStr(ctx, v, "b"); JS_ToFloat64(ctx, &b, tmp); JS_FreeValue(ctx, tmp);
    tmp = JS_GetPropertyStr(ctx, v, "c"); JS_ToFloat64(ctx, &c, tmp); JS_FreeValue(ctx, tmp);
    tmp = JS_GetPropertyStr(ctx, v, "d"); JS_ToFloat64(ctx, &d, tmp); JS_FreeValue(ctx, tmp);
    tmp = JS_GetPropertyStr(ctx, v, "e"); JS_ToFloat64(ctx, &e, tmp); JS_FreeValue(ctx, tmp);
    tmp = JS_GetPropertyStr(ctx, v, "f"); JS_ToFloat64(ctx, &f, tmp); JS_FreeValue(ctx, tmp);
    cairo_matrix_init(m, a, b, c, d, e, f);
    return TRUE;
}

JSValue
ns_ctx_setTransform(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    cairo_matrix_t m;
    if (argc == 0) {
        cairo_identity_matrix(st->cr);
        return JS_UNDEFINED;
    }
    if (argc == 1) {
        if (!ns_matrix_from_obj(ctx, argv[0], &m)) return JS_UNDEFINED;
        cairo_set_matrix(st->cr, &m);
        return JS_UNDEFINED;
    }
    if (argc < 6) return JS_UNDEFINED;
    cairo_matrix_init(&m,
        ns_arg_d(ctx, argv[0]), ns_arg_d(ctx, argv[1]),
        ns_arg_d(ctx, argv[2]), ns_arg_d(ctx, argv[3]),
        ns_arg_d(ctx, argv[4]), ns_arg_d(ctx, argv[5]));
    cairo_set_matrix(st->cr, &m);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_transform(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    if (argc < 6) return JS_UNDEFINED;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    cairo_matrix_t m;
    cairo_matrix_init(&m,
        ns_arg_d(ctx, argv[0]), ns_arg_d(ctx, argv[1]),
        ns_arg_d(ctx, argv[2]), ns_arg_d(ctx, argv[3]),
        ns_arg_d(ctx, argv[4]), ns_arg_d(ctx, argv[5]));
    cairo_transform(st->cr, &m);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_resetTransform(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (st) cairo_identity_matrix(st->cr);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_setLineDash(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    JSValue stored = JS_NewArray(ctx);
    if (argc >= 1 && JS_IsArray(argv[0])) {
        uint32_t n = ns_js_array_length(ctx, argv[0]);
        uint32_t out = 0;
        gboolean dup = (n % 2 == 1);
        for (uint32_t pass = 0; pass < (dup ? 2u : 1u); pass++) {
            for (uint32_t i = 0; i < n; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, argv[0], i);
                double d = 0;
                JS_ToFloat64(ctx, &d, e);
                JS_FreeValue(ctx, e);
                if (!isfinite(d) || d < 0) d = 0;
                JS_SetPropertyUint32(ctx, stored, out++, JS_NewFloat64(ctx, d));
            }
        }
    }
    ns_hset(ctx, this_val, "_dashes", stored);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_getLineDash(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue cur = ns_hget(ctx, this_val, "_dashes");
    if (!JS_IsArray(cur)) {
        JS_FreeValue(ctx, cur);
        return JS_NewArray(ctx);
    }
    uint32_t n = ns_js_array_length(ctx, cur);
    JSValue out = JS_NewArray(ctx);
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, cur, i);
        JS_SetPropertyUint32(ctx, out, i, e);
    }
    JS_FreeValue(ctx, cur);
    return out;
}

JSValue
ns_ctx_gradient_addColorStop(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    double pos = ns_arg_d(ctx, argv[0]);
    if (!(pos >= 0.0 && pos <= 1.0))
        return ns_canvas_throw_dom(ctx, "IndexSizeError",
            "addColorStop offset must be in the range [0, 1]");
    const char *col = JS_ToCString(ctx, argv[1]);
    if (!col)
        return ns_canvas_throw_dom(ctx, "SyntaxError",
            "addColorStop color could not be parsed");
    double r, g, b, a;
    gboolean ok = ns_canvas_parse_color(col, &r, &g, &b, &a);
    JS_FreeCString(ctx, col);
    if (!ok)
        return ns_canvas_throw_dom(ctx, "SyntaxError",
            "addColorStop color could not be parsed");
    JSValue stops = ns_hget(ctx, this_val, "_stops");
    if (!JS_IsArray(stops)) {
        JS_FreeValue(ctx, stops);
        stops = JS_NewArray(ctx);
        ns_hset(ctx, this_val, "_stops", JS_DupValue(ctx, stops));
    }
    JSValue lenv = JS_GetPropertyStr(ctx, stops, "length");
    uint32_t n = 0; JS_ToUint32(ctx, &n, lenv); JS_FreeValue(ctx, lenv);
    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "pos", JS_NewFloat64(ctx, pos));
    JS_SetPropertyStr(ctx, entry, "r",   JS_NewFloat64(ctx, r));
    JS_SetPropertyStr(ctx, entry, "g",   JS_NewFloat64(ctx, g));
    JS_SetPropertyStr(ctx, entry, "b",   JS_NewFloat64(ctx, b));
    JS_SetPropertyStr(ctx, entry, "a",   JS_NewFloat64(ctx, a));
    JS_SetPropertyUint32(ctx, stops, n, entry);
    JS_FreeValue(ctx, stops);
    return JS_UNDEFINED;
}

cairo_surface_t *
ns_ctx_drawimage_source(JSContext *ctx, JSValueConst src, int *out_w, int *out_h,
                        gboolean *origin_clean)
{
    *origin_clean = TRUE;
    if (!JS_IsObject(src)) return NULL;
    ns_image_bitmap *bm = JS_GetOpaque(src, ns_image_bitmap_class_id);
    if (bm && bm->surf) {
        *out_w = bm->w;
        *out_h = bm->h;
        *origin_clean = bm->origin_clean;
        return cairo_surface_reference(bm->surf);
    }
    const ns_node *n = ns_offscreen_node(src);
    if (!n) n = ns_unwrap_element(src);
    if (!n || !n->name) return NULL;
    ns_js *js = js_from_ctx(ctx);
    if (!js) return NULL;
    if (strcmp(n->name, "canvas") == 0) {
        ns_canvas_state *st = ns_canvas_state_for(js, n);
        if (st && st->surf) {
            *out_w = st->w;
            *out_h = st->h;
            *origin_clean = st->origin_clean;
            return cairo_surface_reference(st->surf);
        }
        return NULL;
    }
    ns_texture *tex = NULL;
    const char *source_url = NULL;
    const char *cors_allow_origin = NULL;
    gboolean cors_requested = ns_element_get_attr(n, "crossorigin") != NULL;
    if (js->layout_root) {
        const ns_box *b = ns_box_find_by_dom(js->layout_root, n);
        if (b && b->media) {
            if (strcmp(n->name, "img") == 0 && b->media->image) {
                const ns_image *im = (const ns_image *)b->media->image;
                if (im->texture) {
                    tex = im->texture;
                    source_url = im->final_url ? im->final_url : im->url;
                    cors_allow_origin = im->cors_allow_origin;
                }
            } else if (strcmp(n->name, "video") == 0 && b->media->video) {
                const ns_video *v = (const ns_video *)b->media->video;
                tex = v->poster_texture;
                source_url = v->poster_url;
            }
        }
    }
    if (strcmp(n->name, "video") == 0 && js->media_frame_cb) {
        const char *frame_url = NULL;
        const char *frame_cors = NULL;
        ns_texture *frame = js->media_frame_cb(n, &frame_url, &frame_cors,
                                               js->media_frame_user_data);
        if (frame) {
            tex = frame;
            source_url = frame_url;
            cors_allow_origin = frame_cors;
        }
    }
    ns_image *im_cache = NULL;
    if (!tex && strcmp(n->name, "img") == 0) {
        const ns_image *im = ns_js_image_for_node(js, n);
        if (im && im->texture) {
            tex = im->texture;
            source_url = im->final_url ? im->final_url : im->url;
            cors_allow_origin = im->cors_allow_origin;
            if (!im->anim_frames) im_cache = (ns_image *)im;
        }
    }
    if (!tex) return NULL;
    *origin_clean = ns_js_resource_origin_clean(js, ctx, source_url,
        cors_requested ? cors_allow_origin : NULL);
    if (im_cache && im_cache->render_surface) {
        cairo_surface_t *cached = im_cache->render_surface;
        *out_w = cairo_image_surface_get_width(cached);
        *out_h = cairo_image_surface_get_height(cached);
        return cairo_surface_reference(cached);
    }
    int iw = ns_texture_get_width(tex);
    int ih = ns_texture_get_height(tex);
    if (iw <= 0 || ih <= 0) return NULL;
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    guchar *dst = cairo_image_surface_get_data(surf);
    int dst_stride = cairo_image_surface_get_stride(surf);
    ns_texture_download(tex, dst, (gsize)dst_stride);
    cairo_surface_mark_dirty(surf);
    *out_w = iw;
    *out_h = ih;
    if (im_cache)
        im_cache->render_surface = cairo_surface_reference(surf);
    return surf;
}

cairo_surface_t *
ns_js_drawimage_source_surface(JSContext *ctx, JSValueConst src,
                               int *out_w, int *out_h, gboolean *threw)
{
    *out_w = 0;
    *out_h = 0;
    *threw = FALSE;
    gboolean origin_clean = TRUE;
    cairo_surface_t *surf = ns_ctx_drawimage_source(ctx, src, out_w, out_h,
                                                    &origin_clean);
    if (!surf || origin_clean) return surf;
    cairo_surface_destroy(surf);
    *out_w = 0;
    *out_h = 0;
    *threw = TRUE;
    ns_canvas_throw_dom(ctx, "SecurityError",
                        "The image source is not origin-clean.");
    return NULL;
}

JSValue
ns_ctx_drawImage(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "Failed to execute 'drawImage' on 'CanvasRenderingContext2D': "
            "argument 1 is not a valid image source.");
    if (argc < 3) return JS_UNDEFINED;
    int sw_total = 0, sh_total = 0;
    gboolean origin_clean = TRUE;
    cairo_surface_t *src = ns_ctx_drawimage_source(ctx, argv[0],
                                                   &sw_total, &sh_total,
                                                   &origin_clean);
    if (!src || sw_total <= 0 || sh_total <= 0) {
        if (src) cairo_surface_destroy(src);
        return JS_UNDEFINED;
    }
    double sx, sy, sw, sh, dx, dy, dw, dh;
    if (argc >= 9) {
        sx = ns_arg_d(ctx, argv[1]);
        sy = ns_arg_d(ctx, argv[2]);
        sw = ns_arg_d(ctx, argv[3]);
        sh = ns_arg_d(ctx, argv[4]);
        dx = ns_arg_d(ctx, argv[5]);
        dy = ns_arg_d(ctx, argv[6]);
        dw = ns_arg_d(ctx, argv[7]);
        dh = ns_arg_d(ctx, argv[8]);
    } else if (argc >= 5) {
        sx = 0; sy = 0; sw = sw_total; sh = sh_total;
        dx = ns_arg_d(ctx, argv[1]);
        dy = ns_arg_d(ctx, argv[2]);
        dw = ns_arg_d(ctx, argv[3]);
        dh = ns_arg_d(ctx, argv[4]);
    } else {
        sx = 0; sy = 0; sw = sw_total; sh = sh_total;
        dx = ns_arg_d(ctx, argv[1]);
        dy = ns_arg_d(ctx, argv[2]);
        dw = sw_total; dh = sh_total;
    }
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        cairo_surface_destroy(src);
        return JS_UNDEFINED;
    }
    double ga = ns_ctx_global_alpha(ctx, this_val);
    gboolean smooth = ns_ctx_image_smoothing(ctx, this_val);
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) {
        cairo_surface_destroy(src);
        return JS_UNDEFINED;
    }
    cairo_save(st->cr);
    ns_ctx_apply_composite(ctx, this_val, st->cr);
    cairo_translate(st->cr, dx, dy);
    cairo_scale(st->cr, dw / sw, dh / sh);
    cairo_translate(st->cr, -sx, -sy);
    cairo_rectangle(st->cr, sx, sy, sw, sh);
    cairo_clip(st->cr);
    cairo_set_source_surface(st->cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(st->cr),
                             smooth ? CAIRO_FILTER_BILINEAR
                                    : CAIRO_FILTER_NEAREST);
    if (ga < 1.0 - 1e-6) cairo_paint_with_alpha(st->cr, ga);
    else                  cairo_paint(st->cr);
    cairo_restore(st->cr);
    cairo_surface_destroy(src);
    if (!origin_clean) st->origin_clean = FALSE;
    { ns_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

JSValue
ns_ctx_createPattern(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_NULL;
    int iw = 0, ih = 0;
    gboolean origin_clean = TRUE;
    cairo_surface_t *probe = ns_ctx_drawimage_source(ctx, argv[0], &iw, &ih,
                                                     &origin_clean);
    if (!probe) return JS_NULL;
    cairo_surface_destroy(probe);
    const char *rep = "repeat";
    const char *r = argc >= 2 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    if (r && *r) rep = r;
    JSValue obj = ns_pattern_new(ctx, ns_ctx_realm(ctx, this_val), argv[0], rep);
    if (r) JS_FreeCString(ctx, r);
    return obj;
}

JSValue
ns_ctx_createLinearGradient(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_NULL;
    JSValue obj = ns_gradient_new(ctx, ns_ctx_realm(ctx, this_val), "linear");
    ns_hset(ctx, obj, "_x0", JS_NewFloat64(ctx, ns_arg_d(ctx, argv[0])));
    ns_hset(ctx, obj, "_y0", JS_NewFloat64(ctx, ns_arg_d(ctx, argv[1])));
    ns_hset(ctx, obj, "_x1", JS_NewFloat64(ctx, ns_arg_d(ctx, argv[2])));
    ns_hset(ctx, obj, "_y1", JS_NewFloat64(ctx, ns_arg_d(ctx, argv[3])));
    return obj;
}

JSValue
ns_ctx_createRadialGradient(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (argc < 6) return JS_NULL;
    double r0 = ns_arg_d(ctx, argv[2]);
    double r1 = ns_arg_d(ctx, argv[5]);
    if (r0 < 0.0 || r1 < 0.0)
        return ns_canvas_throw_dom(ctx, "IndexSizeError",
            "createRadialGradient radius must not be negative");
    JSValue obj = ns_gradient_new(ctx, ns_ctx_realm(ctx, this_val), "radial");
    ns_hset(ctx, obj, "_x0", JS_NewFloat64(ctx, ns_arg_d(ctx, argv[0])));
    ns_hset(ctx, obj, "_y0", JS_NewFloat64(ctx, ns_arg_d(ctx, argv[1])));
    ns_hset(ctx, obj, "_r0", JS_NewFloat64(ctx, r0));
    ns_hset(ctx, obj, "_x1", JS_NewFloat64(ctx, ns_arg_d(ctx, argv[3])));
    ns_hset(ctx, obj, "_y1", JS_NewFloat64(ctx, ns_arg_d(ctx, argv[4])));
    ns_hset(ctx, obj, "_r1", JS_NewFloat64(ctx, r1));
    return obj;
}

JSValue
ns_ctx_createConicGradient(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    if (argc < 3) return JS_NULL;
    JSValue obj = ns_gradient_new(ctx, ns_ctx_realm(ctx, this_val), "conic");
    ns_hset(ctx, obj, "_angle", JS_NewFloat64(ctx, ns_arg_d(ctx, argv[0])));
    ns_hset(ctx, obj, "_x0", JS_NewFloat64(ctx, ns_arg_d(ctx, argv[1])));
    ns_hset(ctx, obj, "_y0", JS_NewFloat64(ctx, ns_arg_d(ctx, argv[2])));
    return obj;
}

JSValue
ns_ctx_createImageData(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    int w = 0, h = 0;
    if (JS_IsObject(argv[0]) && !JS_IsNumber(argv[0])) {
        JSValue wv = JS_GetPropertyStr(ctx, argv[0], "width");
        JSValue hv = JS_GetPropertyStr(ctx, argv[0], "height");
        JS_ToInt32(ctx, &w, wv); JS_ToInt32(ctx, &h, hv);
        JS_FreeValue(ctx, wv); JS_FreeValue(ctx, hv);
    } else if (argc >= 2) {
        JS_ToInt32(ctx, &w, argv[0]);
        JS_ToInt32(ctx, &h, argv[1]);
    } else {
        return JS_ThrowTypeError(ctx,
            "Failed to execute 'createImageData' on 'CanvasRenderingContext2D': "
            "2 arguments required, but only 1 present.");
    }
    int64_t aw = w < 0 ? -(int64_t)w : (int64_t)w;
    int64_t ah = h < 0 ? -(int64_t)h : (int64_t)h;
    if (aw == 0 || ah == 0)
        return ns_canvas_throw_dom(ctx, "IndexSizeError", aw == 0
            ? "The source width is zero or not a number."
            : "The source height is zero or not a number.");
    if (aw > 32767 || ah > 32767) return JS_ThrowRangeError(ctx, "ImageData too large");
    return ns_imagedata_new(ctx, ns_ctx_realm(ctx, this_val), (int)aw, (int)ah, NULL);
}

JSValue
ns_ctx_getImageData(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    (void)argc;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    JS_ToInt32(ctx, &sx, argv[0]);
    JS_ToInt32(ctx, &sy, argv[1]);
    JS_ToInt32(ctx, &sw, argv[2]);
    JS_ToInt32(ctx, &sh, argv[3]);
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (st && !st->origin_clean)
        return ns_canvas_throw_dom(ctx, "SecurityError",
            "The canvas has been tainted by cross-origin data.");
    int64_t ox = sx, oy = sy, rw = sw, rh = sh;
    if (rw < 0) { ox += rw; rw = -rw; }
    if (rh < 0) { oy += rh; rh = -rh; }
    if (rw == 0 || rh == 0)
        return ns_canvas_throw_dom(ctx, "IndexSizeError", rw == 0
            ? "The source width is 0." : "The source height is 0.");
    if (rw > 32767 || rh > 32767)
        return JS_ThrowRangeError(ctx, "getImageData region too large");
    int dw = (int)rw, dh = (int)rh;
    uint8_t *out = g_try_malloc0((size_t)dw * (size_t)dh * 4u);
    if (!out) return JS_ThrowRangeError(ctx, "getImageData allocation failed");
    cairo_surface_t *surf = (st && st->surf) ? st->surf : NULL;
    const uint8_t *cd = NULL;
    int cw = 0, ch = 0, cs = 0;
    if (surf) {
        cw = cairo_image_surface_get_width(surf);
        ch = cairo_image_surface_get_height(surf);
        cs = cairo_image_surface_get_stride(surf);
        cairo_surface_flush(surf);
        cd = cairo_image_surface_get_data(surf);
    }
    if (cd)
    for (int y = 0; y < dh; y++) {
        int64_t srcy = oy + y;
        for (int x = 0; x < dw; x++) {
            int64_t srcx = ox + x;
            uint8_t *dst = out + ((size_t)y * (size_t)dw + (size_t)x) * 4u;
            if (srcx < 0 || srcy < 0 || srcx >= cw || srcy >= ch) continue;
            const uint8_t *p = cd + (size_t)srcy * (size_t)cs + (size_t)srcx * 4u;
            uint8_t b = p[0], g = p[1], r = p[2], a = p[3];
            if (a == 0) {
                dst[0] = 0; dst[1] = 0; dst[2] = 0; dst[3] = 0;
            } else if (a == 255) {
                dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = 255;
            } else {
                dst[0] = (uint8_t)((r * 255 + a / 2) / a);
                dst[1] = (uint8_t)((g * 255 + a / 2) / a);
                dst[2] = (uint8_t)((b * 255 + a / 2) / a);
                dst[3] = a;
            }
        }
    }
    JSValue result = ns_imagedata_new(ctx, ns_ctx_realm(ctx, this_val), dw, dh, out);
    g_free(out);
    return result;
}

JSValue
ns_ctx_putImageData(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    if (argc < 3 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    JSValue wv = JS_GetPropertyStr(ctx, argv[0], "width");
    JSValue hv = JS_GetPropertyStr(ctx, argv[0], "height");
    JSValue dv = JS_GetPropertyStr(ctx, argv[0], "data");
    int iw = 0, ih = 0;
    JS_ToInt32(ctx, &iw, wv); JS_ToInt32(ctx, &ih, hv);
    JS_FreeValue(ctx, wv); JS_FreeValue(ctx, hv);
    if (iw <= 0 || ih <= 0) { JS_FreeValue(ctx, dv); return JS_UNDEFINED; }
    if (iw > 32767 || ih > 32767) { JS_FreeValue(ctx, dv); return JS_UNDEFINED; }
    int dx = 0, dy = 0;
    JS_ToInt32(ctx, &dx, argv[1]);
    JS_ToInt32(ctx, &dy, argv[2]);
    int64_t rx = 0, ry = 0, rw = iw, rh = ih;
    if (argc >= 7) {
        int arx = 0, ary = 0, arw = 0, arh = 0;
        JS_ToInt32(ctx, &arx, argv[3]);
        JS_ToInt32(ctx, &ary, argv[4]);
        JS_ToInt32(ctx, &arw, argv[5]);
        JS_ToInt32(ctx, &arh, argv[6]);
        rx = arx; ry = ary; rw = arw; rh = arh;
    }
    if (rw < 0) { rx += rw; rw = -rw; }
    if (rh < 0) { ry += rh; rh = -rh; }
    if (rx < 0)        { rw += rx; rx = 0; }
    if (ry < 0)        { rh += ry; ry = 0; }
    if (rx + rw > iw)  { rw = iw - rx; }
    if (ry + rh > ih)  { rh = ih - ry; }
    if (rw <= 0 || rh <= 0) { JS_FreeValue(ctx, dv); return JS_UNDEFINED; }

    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st || !st->surf) { JS_FreeValue(ctx, dv); return JS_UNDEFINED; }
    int cw = cairo_image_surface_get_width(st->surf);
    int ch = cairo_image_surface_get_height(st->surf);
    int cs = cairo_image_surface_get_stride(st->surf);
    cairo_surface_flush(st->surf);
    uint8_t *cd = cairo_image_surface_get_data(st->surf);
    if (!cd) { JS_FreeValue(ctx, dv); return JS_UNDEFINED; }

    size_t byte_offset = 0, byte_len = 0, bpe = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, dv, &byte_offset, &byte_len, &bpe);
    if (JS_IsException(ab)) { JS_FreeValue(ctx, dv); return JS_UNDEFINED; }
    size_t ab_len = 0;
    uint8_t *src = JS_GetArrayBuffer(ctx, &ab_len, ab);
    if (!src || byte_len < (size_t)iw * (size_t)ih * 4u ||
        byte_offset + byte_len > ab_len) {
        JS_FreeValue(ctx, ab); JS_FreeValue(ctx, dv);
        return JS_UNDEFINED;
    }
    src += byte_offset;
    for (int y = 0; y < rh; y++) {
        int64_t dst_y = (int64_t)dy + y;
        if (dst_y < 0 || dst_y >= ch) continue;
        for (int x = 0; x < rw; x++) {
            int64_t dst_x = (int64_t)dx + x;
            if (dst_x < 0 || dst_x >= cw) continue;
            const uint8_t *s = src + ((size_t)(ry + y) * (size_t)iw +
                                      (size_t)(rx + x)) * 4u;
            uint8_t r = s[0], g = s[1], b = s[2], a = s[3];
            uint8_t pr, pg, pb;
            if (a == 0)        { pr = 0; pg = 0; pb = 0; }
            else if (a == 255) { pr = r; pg = g; pb = b; }
            else {
                pr = (uint8_t)((r * a + 127) / 255);
                pg = (uint8_t)((g * a + 127) / 255);
                pb = (uint8_t)((b * a + 127) / 255);
            }
            uint8_t *p = cd + (size_t)dst_y * (size_t)cs +
                              (size_t)dst_x * 4u;
            p[0] = pb; p[1] = pg; p[2] = pr; p[3] = a;
        }
    }
    cairo_surface_mark_dirty(st->surf);
    JS_FreeValue(ctx, ab);
    JS_FreeValue(ctx, dv);
    ns_js *_j = js_from_ctx(ctx);
    if (_j) _j->mutated = TRUE;
    return JS_UNDEFINED;
}

JSValue
ns_ctx_strokeText(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    if (argc < 3) return JS_UNDEFINED;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    ns_ctx_sync_styles(ctx, this_val, st);
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_UNDEFINED;
    double x = ns_arg_d(ctx, argv[1]);
    double y = ns_arg_d(ctx, argv[2]);
    double mw = argc >= 4 ? ns_arg_d(ctx, argv[3]) : 0;
    ns_ctx_paint_text(ctx, this_val, st, text, x, y, mw, TRUE);
    JS_FreeCString(ctx, text);
    { ns_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

void
ns_round_rect_subpath(cairo_t *cr, double x, double y, double w, double h,
                      double rtl, double rtr, double rbr, double rbl)
{
    double aw = fabs(w), ah = fabs(h);
    double max_r = (aw < ah ? aw : ah) / 2.0;
    if (rtl > max_r) rtl = max_r;
    if (rtr > max_r) rtr = max_r;
    if (rbr > max_r) rbr = max_r;
    if (rbl > max_r) rbl = max_r;
    double x2 = x + w, y2 = y + h;
    if (w < 0) { double t = x; x = x2; x2 = t; }
    if (h < 0) { double t = y; y = y2; y2 = t; }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x  + rtl, y  + rtl, rtl, G_PI,        1.5 * G_PI);
    cairo_arc(cr, x2 - rtr, y  + rtr, rtr, 1.5 * G_PI,  0);
    cairo_arc(cr, x2 - rbr, y2 - rbr, rbr, 0,           0.5 * G_PI);
    cairo_arc(cr, x  + rbl, y2 - rbl, rbl, 0.5 * G_PI,  G_PI);
    cairo_close_path(cr);
}

gboolean
ns_extract_radii(JSContext *ctx, JSValueConst v,
                 double *rtl, double *rtr, double *rbr, double *rbl)
{
    *rtl = *rtr = *rbr = *rbl = 0;
    if (JS_IsArray(v)) {
        uint32_t n = ns_js_array_length(ctx, v);
        double r[4] = { 0, 0, 0, 0 };
        for (uint32_t i = 0; i < n && i < 4; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            JS_ToFloat64(ctx, &r[i], e);
            JS_FreeValue(ctx, e);
        }
        if (n == 1) { *rtl = *rtr = *rbr = *rbl = r[0]; }
        else if (n == 2) { *rtl = *rbr = r[0]; *rtr = *rbl = r[1]; }
        else if (n == 3) { *rtl = r[0]; *rtr = *rbl = r[1]; *rbr = r[2]; }
        else { *rtl = r[0]; *rtr = r[1]; *rbr = r[2]; *rbl = r[3]; }
    } else {
        double r = 0;
        JS_ToFloat64(ctx, &r, v);
        *rtl = *rtr = *rbr = *rbl = r;
    }
    return !(*rtl < 0 || *rtr < 0 || *rbr < 0 || *rbl < 0);
}

JSValue
ns_ctx_roundRect(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    double x = ns_arg_d(ctx, argv[0]);
    double y = ns_arg_d(ctx, argv[1]);
    double w = ns_arg_d(ctx, argv[2]);
    double h = ns_arg_d(ctx, argv[3]);
    double rtl = 0, rtr = 0, rbr = 0, rbl = 0;
    if (argc >= 5 && !ns_extract_radii(ctx, argv[4], &rtl, &rtr, &rbr, &rbl))
        return JS_ThrowRangeError(ctx, "roundRect radius must be non-negative");
    ns_round_rect_subpath(st->cr, x, y, w, h, rtl, rtr, rbr, rbl);
    return JS_UNDEFINED;
}

JSValue
ns_ctx_reset(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    cairo_save(st->cr);
    cairo_identity_matrix(st->cr);
    cairo_set_operator(st->cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(st->cr);
    cairo_restore(st->cr);
    cairo_identity_matrix(st->cr);
    cairo_new_path(st->cr);
    cairo_reset_clip(st->cr);
    cairo_set_dash(st->cr, NULL, 0, 0);
    cairo_set_line_width(st->cr, 1);
    cairo_set_line_cap(st->cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(st->cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_miter_limit(st->cr, 10);
    ns_ctx2d_init_state(ctx, this_val);
    g_free(st->font);
    st->font = g_strdup("10px sans-serif");
    st->fill_r = st->fill_g = st->fill_b = 0; st->fill_a = 1;
    st->stroke_r = st->stroke_g = st->stroke_b = 0; st->stroke_a = 1;
    st->line_width = 1;
    if (st->fill_pattern)   { cairo_pattern_destroy(st->fill_pattern);   st->fill_pattern = NULL; }
    if (st->stroke_pattern) { cairo_pattern_destroy(st->stroke_pattern); st->stroke_pattern = NULL; }
    st->shadow_r = st->shadow_g = st->shadow_b = 0;
    st->shadow_a = 0; st->shadow_blur = 0;
    st->shadow_ox = st->shadow_oy = 0;
    { ns_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

JSValue
ns_ctx_getTransform(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    cairo_matrix_t m = { 1, 0, 0, 1, 0, 0 };
    if (st) cairo_get_matrix(st->cr, &m);
    return ns_dommatrix_make(ctx, m.xx, m.yx, m.xy, m.yy, m.x0, m.y0);
}

JSValue
ns_ctx_isPointInPath(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st || argc < 2) return JS_FALSE;
    int i = 0;
    cairo_path_t *saved = NULL;
    if (ns_value_is_path2d(argv[0])) {
        if (argc < 3) return JS_FALSE;
        saved = cairo_copy_path(st->cr);
        ns_replay_path2d(st->cr, argv[0]);
        i = 1;
    }
    double x = ns_arg_d(ctx, argv[i]);
    double y = ns_arg_d(ctx, argv[i + 1]);
    const char *rule_s = NULL;
    if (argc > i + 2 && JS_IsString(argv[i + 2]))
        rule_s = JS_ToCString(ctx, argv[i + 2]);
    cairo_fill_rule_t prev_rule = cairo_get_fill_rule(st->cr);
    cairo_set_fill_rule(st->cr, ns_parse_fill_rule(rule_s));
    if (rule_s) JS_FreeCString(ctx, rule_s);
    cairo_bool_t in = cairo_in_fill(st->cr, x, y);
    cairo_set_fill_rule(st->cr, prev_rule);
    ns_ctx_restore_path(st->cr, saved);
    return in ? JS_TRUE : JS_FALSE;
}

JSValue
ns_ctx_isPointInStroke(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    ns_canvas_state *st = ns_ctx_state(ctx, this_val);
    if (!st || argc < 2) return JS_FALSE;
    ns_ctx_sync_styles(ctx, this_val, st);
    int i = 0;
    cairo_path_t *saved = NULL;
    if (ns_value_is_path2d(argv[0])) {
        if (argc < 3) return JS_FALSE;
        saved = cairo_copy_path(st->cr);
        ns_replay_path2d(st->cr, argv[0]);
        i = 1;
    }
    double x = ns_arg_d(ctx, argv[i]);
    double y = ns_arg_d(ctx, argv[i + 1]);
    cairo_set_line_width(st->cr, st->line_width);
    cairo_bool_t in = cairo_in_stroke(st->cr, x, y);
    ns_ctx_restore_path(st->cr, saved);
    return in ? JS_TRUE : JS_FALSE;
}

JSValue
ns_path2d_get_cr(JSContext *ctx, JSValueConst this_val, cairo_t **out)
{
    ns_path2d *p = JS_GetOpaque(this_val, ns_path2d_class_id);
    if (!p) return JS_ThrowTypeError(ctx, "Path2D expected");
    *out = p->cr;
    return JS_UNDEFINED;
}

JSValue
ns_path2d_moveTo(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    cairo_t *cr = NULL;
    JSValue err = ns_path2d_get_cr(ctx, this_val, &cr);
    if (JS_IsException(err)) return err;
    if (argc < 2) return JS_UNDEFINED;
    cairo_move_to(cr, ns_arg_d(ctx, argv[0]), ns_arg_d(ctx, argv[1]));
    return JS_UNDEFINED;
}

JSValue
ns_path2d_lineTo(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    cairo_t *cr = NULL;
    JSValue err = ns_path2d_get_cr(ctx, this_val, &cr);
    if (JS_IsException(err)) return err;
    if (argc < 2) return JS_UNDEFINED;
    if (!cairo_has_current_point(cr))
        cairo_move_to(cr, ns_arg_d(ctx, argv[0]), ns_arg_d(ctx, argv[1]));
    cairo_line_to(cr, ns_arg_d(ctx, argv[0]), ns_arg_d(ctx, argv[1]));
    return JS_UNDEFINED;
}

JSValue
ns_path2d_closePath(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    cairo_t *cr = NULL;
    JSValue err = ns_path2d_get_cr(ctx, this_val, &cr);
    if (JS_IsException(err)) return err;
    cairo_close_path(cr);
    return JS_UNDEFINED;
}

JSValue
ns_path2d_bezierCurveTo(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    cairo_t *cr = NULL;
    JSValue err = ns_path2d_get_cr(ctx, this_val, &cr);
    if (JS_IsException(err)) return err;
    if (argc < 6) return JS_UNDEFINED;
    cairo_curve_to(cr,
        ns_arg_d(ctx, argv[0]), ns_arg_d(ctx, argv[1]),
        ns_arg_d(ctx, argv[2]), ns_arg_d(ctx, argv[3]),
        ns_arg_d(ctx, argv[4]), ns_arg_d(ctx, argv[5]));
    return JS_UNDEFINED;
}

JSValue
ns_path2d_quadraticCurveTo(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    cairo_t *cr = NULL;
    JSValue err = ns_path2d_get_cr(ctx, this_val, &cr);
    if (JS_IsException(err)) return err;
    if (argc < 4) return JS_UNDEFINED;
    double cpx = ns_arg_d(ctx, argv[0]), cpy = ns_arg_d(ctx, argv[1]);
    double x   = ns_arg_d(ctx, argv[2]), y   = ns_arg_d(ctx, argv[3]);
    double x0, y0;
    if (!cairo_has_current_point(cr)) cairo_move_to(cr, cpx, cpy);
    cairo_get_current_point(cr, &x0, &y0);
    cairo_curve_to(cr,
                   x0 + 2.0 / 3.0 * (cpx - x0), y0 + 2.0 / 3.0 * (cpy - y0),
                   x  + 2.0 / 3.0 * (cpx - x),  y  + 2.0 / 3.0 * (cpy - y),
                   x, y);
    return JS_UNDEFINED;
}

JSValue
ns_path2d_arc(JSContext *ctx, JSValueConst this_val,
              int argc, JSValueConst *argv)
{
    cairo_t *cr = NULL;
    JSValue err = ns_path2d_get_cr(ctx, this_val, &cr);
    if (JS_IsException(err)) return err;
    if (argc < 5) return JS_UNDEFINED;
    double x = ns_arg_d(ctx, argv[0]);
    double y = ns_arg_d(ctx, argv[1]);
    double r = ns_arg_d(ctx, argv[2]);
    double a0 = ns_arg_d(ctx, argv[3]);
    double a1 = ns_arg_d(ctx, argv[4]);
    gboolean ccw = argc >= 6 && JS_ToBool(ctx, argv[5]);
    if (ccw) cairo_arc_negative(cr, x, y, r, a0, a1);
    else     cairo_arc(cr, x, y, r, a0, a1);
    return JS_UNDEFINED;
}

JSValue
ns_path2d_arcTo(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    cairo_t *cr = NULL;
    JSValue err = ns_path2d_get_cr(ctx, this_val, &cr);
    if (JS_IsException(err)) return err;
    if (argc < 5) return JS_UNDEFINED;
    double x1 = ns_arg_d(ctx, argv[0]), y1 = ns_arg_d(ctx, argv[1]);
    double x2 = ns_arg_d(ctx, argv[2]), y2 = ns_arg_d(ctx, argv[3]);
    double r  = ns_arg_d(ctx, argv[4]);
    if (!cairo_has_current_point(cr)) cairo_move_to(cr, x1, y1);
    double x0, y0;
    cairo_get_current_point(cr, &x0, &y0);
    double a1x = x0 - x1, a1y = y0 - y1;
    double a2x = x2 - x1, a2y = y2 - y1;
    double l1 = hypot(a1x, a1y), l2 = hypot(a2x, a2y);
    if (l1 == 0 || l2 == 0 || r == 0) { cairo_line_to(cr, x1, y1); return JS_UNDEFINED; }
    double u1x = a1x / l1, u1y = a1y / l1;
    double u2x = a2x / l2, u2y = a2y / l2;
    double cos_t = u1x * u2x + u1y * u2y;
    if (cos_t >= 1.0 || cos_t <= -1.0) { cairo_line_to(cr, x1, y1); return JS_UNDEFINED; }
    double tan_half = sqrt((1 - cos_t) / (1 + cos_t));
    double dist = r / tan_half;
    double t1x = x1 + u1x * dist, t1y = y1 + u1y * dist;
    double t2x = x1 + u2x * dist, t2y = y1 + u2y * dist;
    double bisx = (u1x + u2x), bisy = (u1y + u2y);
    double blen = hypot(bisx, bisy);
    if (blen == 0) { cairo_line_to(cr, t1x, t1y); return JS_UNDEFINED; }
    bisx /= blen; bisy /= blen;
    double cdist = sqrt(r * r + dist * dist);
    double cx = x1 + bisx * cdist, cy = y1 + bisy * cdist;
    double ang1 = atan2(t1y - cy, t1x - cx);
    double ang2 = atan2(t2y - cy, t2x - cx);
    double cross = u1x * u2y - u1y * u2x;
    cairo_line_to(cr, t1x, t1y);
    if (cross < 0) cairo_arc_negative(cr, cx, cy, r, ang1, ang2);
    else           cairo_arc(cr, cx, cy, r, ang1, ang2);
    (void)t2x; (void)t2y;
    return JS_UNDEFINED;
}

JSValue
ns_path2d_ellipse(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    cairo_t *cr = NULL;
    JSValue err = ns_path2d_get_cr(ctx, this_val, &cr);
    if (JS_IsException(err)) return err;
    if (argc < 7) return JS_UNDEFINED;
    double x  = ns_arg_d(ctx, argv[0]);
    double y  = ns_arg_d(ctx, argv[1]);
    double rx = ns_arg_d(ctx, argv[2]);
    double ry = ns_arg_d(ctx, argv[3]);
    double rot = ns_arg_d(ctx, argv[4]);
    double a0 = ns_arg_d(ctx, argv[5]);
    double a1 = ns_arg_d(ctx, argv[6]);
    gboolean ccw = argc >= 8 && JS_ToBool(ctx, argv[7]);
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_rotate(cr, rot);
    cairo_scale(cr, rx, ry);
    if (ccw) cairo_arc_negative(cr, 0, 0, 1.0, a0, a1);
    else     cairo_arc(cr, 0, 0, 1.0, a0, a1);
    cairo_restore(cr);
    return JS_UNDEFINED;
}

JSValue
ns_path2d_rect(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    cairo_t *cr = NULL;
    JSValue err = ns_path2d_get_cr(ctx, this_val, &cr);
    if (JS_IsException(err)) return err;
    if (argc < 4) return JS_UNDEFINED;
    cairo_rectangle(cr,
        ns_arg_d(ctx, argv[0]), ns_arg_d(ctx, argv[1]),
        ns_arg_d(ctx, argv[2]), ns_arg_d(ctx, argv[3]));
    return JS_UNDEFINED;
}

JSValue
ns_path2d_roundRect(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    cairo_t *cr = NULL;
    JSValue err = ns_path2d_get_cr(ctx, this_val, &cr);
    if (JS_IsException(err)) return err;
    if (argc < 4) return JS_UNDEFINED;
    double x = ns_arg_d(ctx, argv[0]);
    double y = ns_arg_d(ctx, argv[1]);
    double w = ns_arg_d(ctx, argv[2]);
    double h = ns_arg_d(ctx, argv[3]);
    double rtl = 0, rtr = 0, rbr = 0, rbl = 0;
    if (argc >= 5 && !ns_extract_radii(ctx, argv[4], &rtl, &rtr, &rbr, &rbl))
        return JS_ThrowRangeError(ctx, "roundRect radius must be non-negative");
    ns_round_rect_subpath(cr, x, y, w, h, rtl, rtr, rbr, rbl);
    return JS_UNDEFINED;
}

JSValue
ns_path2d_addPath(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    cairo_t *cr = NULL;
    JSValue err = ns_path2d_get_cr(ctx, this_val, &cr);
    if (JS_IsException(err)) return err;
    if (argc < 1 || !ns_value_is_path2d(argv[0])) return JS_UNDEFINED;
    ns_path2d *src = JS_GetOpaque(argv[0], ns_path2d_class_id);
    if (!src) return JS_UNDEFINED;
    cairo_path_t *cp = cairo_copy_path(src->cr);
    cairo_save(cr);
    if (argc >= 2 && JS_IsObject(argv[1])) {
        double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[1], "a"); JS_ToFloat64(ctx, &a, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "b"); JS_ToFloat64(ctx, &b, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "c"); JS_ToFloat64(ctx, &c, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "d"); JS_ToFloat64(ctx, &d, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "e"); JS_ToFloat64(ctx, &e, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "f"); JS_ToFloat64(ctx, &f, v); JS_FreeValue(ctx, v);
        cairo_matrix_t m;
        cairo_matrix_init(&m, a, b, c, d, e, f);
        cairo_transform(cr, &m);
    }
    cairo_append_path(cr, cp);
    cairo_path_destroy(cp);
    cairo_restore(cr);
    return JS_UNDEFINED;
}

const char *
ns_svg_skip_ws(const char *p)
{
    while (*p && (g_ascii_isspace(*p) || *p == ',')) p++;
    return p;
}

gboolean
ns_svg_read_number(const char **pp, double *out)
{
    const char *p = ns_svg_skip_ws(*pp);
    char *end = NULL;
    double v = g_ascii_strtod(p, &end);
    if (end == p) return FALSE;
    *out = v;
    *pp = end;
    return TRUE;
}

void
ns_path2d_arc_svg(cairo_t *cr, double x1, double y1,
                  double rx, double ry, double phi_deg,
                  gboolean large_arc, gboolean sweep,
                  double x2, double y2)
{
    if (rx == 0 || ry == 0) { cairo_line_to(cr, x2, y2); return; }
    rx = fabs(rx); ry = fabs(ry);
    double phi = phi_deg * G_PI / 180.0;
    double cos_phi = cos(phi), sin_phi = sin(phi);
    double dx = (x1 - x2) / 2.0, dy = (y1 - y2) / 2.0;
    double x1p =  cos_phi * dx + sin_phi * dy;
    double y1p = -sin_phi * dx + cos_phi * dy;
    double rx2 = rx * rx, ry2 = ry * ry;
    double x1p2 = x1p * x1p, y1p2 = y1p * y1p;
    double radii_check = x1p2 / rx2 + y1p2 / ry2;
    if (radii_check > 1) {
        double s = sqrt(radii_check);
        rx *= s; ry *= s; rx2 = rx * rx; ry2 = ry * ry;
    }
    double sign = (large_arc == sweep) ? -1.0 : 1.0;
    double denom = rx2 * y1p2 + ry2 * x1p2;
    double frac = (rx2 * ry2 - denom) / denom;
    if (frac < 0) frac = 0;
    double factor = sign * sqrt(frac);
    double cxp = factor * (rx * y1p) / ry;
    double cyp = factor * -(ry * x1p) / rx;
    double cx = cos_phi * cxp - sin_phi * cyp + (x1 + x2) / 2.0;
    double cy = sin_phi * cxp + cos_phi * cyp + (y1 + y2) / 2.0;
    double a1 = atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    double a2 = atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, phi);
    cairo_scale(cr, rx, ry);
    if (sweep) {
        if (a2 < a1) a2 += 2 * G_PI;
        cairo_arc(cr, 0, 0, 1.0, a1, a2);
    } else {
        if (a2 > a1) a2 -= 2 * G_PI;
        cairo_arc_negative(cr, 0, 0, 1.0, a1, a2);
    }
    cairo_restore(cr);
}

void
ns_path2d_parse_svg(cairo_t *cr, const char *d)
{
    if (!d) return;
    const char *p = d;
    double cx = 0, cy = 0;
    double sx = 0, sy = 0;
    double last_cx = 0, last_cy = 0;
    double last_qx = 0, last_qy = 0;
    char prev_cmd = 0;
    char cmd = 0;
    while (*p) {
        p = ns_svg_skip_ws(p);
        if (!*p) break;
        if (g_ascii_isalpha(*p)) {
            cmd = *p++;
        } else if (cmd == 0) {
            break;
        } else if (cmd == 'M') {
            cmd = 'L';
        } else if (cmd == 'm') {
            cmd = 'l';
        } else if (cmd == 'Z' || cmd == 'z') {
            break;
        }
        double x, y, x1, y1, x2, y2, rx, ry, rot, large, sweep;
        gboolean rel = g_ascii_islower(cmd);
        switch (cmd) {
        case 'M': case 'm':
            if (!ns_svg_read_number(&p, &x) || !ns_svg_read_number(&p, &y)) return;
            if (rel) { x += cx; y += cy; }
            cairo_move_to(cr, x, y);
            cx = x; cy = y; sx = x; sy = y;
            break;
        case 'L': case 'l':
            if (!ns_svg_read_number(&p, &x) || !ns_svg_read_number(&p, &y)) return;
            if (rel) { x += cx; y += cy; }
            cairo_line_to(cr, x, y);
            cx = x; cy = y;
            break;
        case 'H': case 'h':
            if (!ns_svg_read_number(&p, &x)) return;
            if (rel) x += cx;
            cairo_line_to(cr, x, cy);
            cx = x;
            break;
        case 'V': case 'v':
            if (!ns_svg_read_number(&p, &y)) return;
            if (rel) y += cy;
            cairo_line_to(cr, cx, y);
            cy = y;
            break;
        case 'Z': case 'z':
            cairo_close_path(cr);
            cx = sx; cy = sy;
            break;
        case 'C': case 'c':
            if (!ns_svg_read_number(&p, &x1) || !ns_svg_read_number(&p, &y1) ||
                !ns_svg_read_number(&p, &x2) || !ns_svg_read_number(&p, &y2) ||
                !ns_svg_read_number(&p, &x)  || !ns_svg_read_number(&p, &y)) return;
            if (rel) { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }
            cairo_curve_to(cr, x1, y1, x2, y2, x, y);
            last_cx = x2; last_cy = y2;
            cx = x; cy = y;
            break;
        case 'S': case 's':
            if (!ns_svg_read_number(&p, &x2) || !ns_svg_read_number(&p, &y2) ||
                !ns_svg_read_number(&p, &x)  || !ns_svg_read_number(&p, &y)) return;
            if (rel) { x2 += cx; y2 += cy; x += cx; y += cy; }
            if (prev_cmd == 'C' || prev_cmd == 'c' ||
                prev_cmd == 'S' || prev_cmd == 's') {
                x1 = 2 * cx - last_cx; y1 = 2 * cy - last_cy;
            } else { x1 = cx; y1 = cy; }
            cairo_curve_to(cr, x1, y1, x2, y2, x, y);
            last_cx = x2; last_cy = y2;
            cx = x; cy = y;
            break;
        case 'Q': case 'q':
            if (!ns_svg_read_number(&p, &x1) || !ns_svg_read_number(&p, &y1) ||
                !ns_svg_read_number(&p, &x)  || !ns_svg_read_number(&p, &y)) return;
            if (rel) { x1 += cx; y1 += cy; x += cx; y += cy; }
            cairo_curve_to(cr,
                cx + 2.0 / 3.0 * (x1 - cx), cy + 2.0 / 3.0 * (y1 - cy),
                x  + 2.0 / 3.0 * (x1 - x),  y  + 2.0 / 3.0 * (y1 - y),
                x, y);
            last_qx = x1; last_qy = y1;
            cx = x; cy = y;
            break;
        case 'T': case 't':
            if (!ns_svg_read_number(&p, &x)  || !ns_svg_read_number(&p, &y)) return;
            if (rel) { x += cx; y += cy; }
            if (prev_cmd == 'Q' || prev_cmd == 'q' ||
                prev_cmd == 'T' || prev_cmd == 't') {
                x1 = 2 * cx - last_qx; y1 = 2 * cy - last_qy;
            } else { x1 = cx; y1 = cy; }
            cairo_curve_to(cr,
                cx + 2.0 / 3.0 * (x1 - cx), cy + 2.0 / 3.0 * (y1 - cy),
                x  + 2.0 / 3.0 * (x1 - x),  y  + 2.0 / 3.0 * (y1 - y),
                x, y);
            last_qx = x1; last_qy = y1;
            cx = x; cy = y;
            break;
        case 'A': case 'a':
            if (!ns_svg_read_number(&p, &rx)    || !ns_svg_read_number(&p, &ry) ||
                !ns_svg_read_number(&p, &rot)   ||
                !ns_svg_read_number(&p, &large) || !ns_svg_read_number(&p, &sweep) ||
                !ns_svg_read_number(&p, &x)     || !ns_svg_read_number(&p, &y)) return;
            if (rel) { x += cx; y += cy; }
            ns_path2d_arc_svg(cr, cx, cy, rx, ry, rot,
                              large != 0, sweep != 0, x, y);
            cx = x; cy = y;
            break;
        default:
            return;
        }
        prev_cmd = cmd;
    }
}

JSValue
ns_path2d_ctor(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    if (JS_IsUndefined(this_val)) return ns_api_throw_new_required(ctx, "Path2D");
    ns_path2d *p = g_new0(ns_path2d, 1);
    p->rs = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, NULL);
    p->cr = cairo_create(p->rs);
    JSValue proto = ns_api_proto_of_ctor(ctx, this_val, "Path2D");
    JSValue obj = JS_IsObject(proto)
        ? JS_NewObjectProtoClass(ctx, proto, ns_path2d_class_id)
        : JS_NewObjectClass(ctx, ns_path2d_class_id);
    JS_FreeValue(ctx, proto);
    JS_SetOpaque(obj, p);
    if (argc >= 1 && ns_value_is_path2d(argv[0])) {
        ns_path2d *src = JS_GetOpaque(argv[0], ns_path2d_class_id);
        if (src) {
            cairo_path_t *cp = cairo_copy_path(src->cr);
            cairo_append_path(p->cr, cp);
            cairo_path_destroy(cp);
        }
    } else if (argc >= 1 && JS_IsString(argv[0])) {
        const char *d = JS_ToCString(ctx, argv[0]);
        if (d) { ns_path2d_parse_svg(p->cr, d); JS_FreeCString(ctx, d); }
    }
    return obj;
}

JSValue
ns_ctx_get_attrs(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue saved = ns_hget(ctx, this_val, "_attrs");
    JSValue out = JS_NewObject(ctx);
    static const char *const keys[] = { "alpha", "colorSpace", "colorType",
                                        "desynchronized" };
    for (gsize i = 0; i < G_N_ELEMENTS(keys); i++)
        JS_SetPropertyStr(ctx, out, keys[i], JS_GetPropertyStr(ctx, saved, keys[i]));
    JSValue tone = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tone, "mode", JS_NewString(ctx, "standard"));
    JS_SetPropertyStr(ctx, out, "toneMapping", tone);
    JS_SetPropertyStr(ctx, out, "willReadFrequently",
                      JS_GetPropertyStr(ctx, saved, "willReadFrequently"));
    JS_FreeValue(ctx, saved);
    return out;
}

JSValue
ns_ctx_is_context_lost(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_FALSE;
}

JSValue
ns_ctx_draw_focus_if_needed(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

typedef enum {
    NS_CTX_UNKNOWN,
    NS_CTX_2D,
    NS_CTX_WEBGL,
    NS_CTX_WEBGL2,
    NS_CTX_WEBGPU,
    NS_CTX_BITMAP,
} ns_canvas_ctx_type;

static ns_canvas_ctx_type
ns_canvas_ctx_type_of(JSContext *ctx, JSValueConst v, gboolean *threw)
{
    *threw = FALSE;
    const char *t = JS_ToCString(ctx, v);
    if (!t) {
        *threw = TRUE;
        return NS_CTX_UNKNOWN;
    }
    ns_canvas_ctx_type type = NS_CTX_UNKNOWN;
    if (strcmp(t, "2d") == 0) type = NS_CTX_2D;
    else if (strcmp(t, "webgl") == 0 || strcmp(t, "experimental-webgl") == 0)
        type = NS_CTX_WEBGL;
#ifndef NS_HAVE_CGL
    else if (strcmp(t, "webgl2") == 0) type = NS_CTX_WEBGL2;
#endif
    else if (strcmp(t, "webgpu") == 0) type = NS_CTX_WEBGPU;
    else if (strcmp(t, "bitmaprenderer") == 0) type = NS_CTX_BITMAP;
    JS_FreeCString(ctx, t);
    return type;
}

static JSValue
ns_canvas_2d_attrs(JSContext *ctx, JSValueConst options, gboolean *opaque)
{
    JSValue attrs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, attrs, "alpha", JS_TRUE);
    JS_SetPropertyStr(ctx, attrs, "colorSpace", JS_NewString(ctx, "srgb"));
    JS_SetPropertyStr(ctx, attrs, "colorType", JS_NewString(ctx, "unorm8"));
    JS_SetPropertyStr(ctx, attrs, "desynchronized", JS_FALSE);
    JS_SetPropertyStr(ctx, attrs, "willReadFrequently", JS_FALSE);
    *opaque = FALSE;
    if (!JS_IsObject(options)) return attrs;
    JSValue av = JS_GetPropertyStr(ctx, options, "alpha");
    if (!JS_IsUndefined(av)) {
        gboolean alpha = JS_ToBool(ctx, av) ? TRUE : FALSE;
        *opaque = !alpha;
        JS_SetPropertyStr(ctx, attrs, "alpha", alpha ? JS_TRUE : JS_FALSE);
    }
    JS_FreeValue(ctx, av);
    JSValue wv = JS_GetPropertyStr(ctx, options, "willReadFrequently");
    if (JS_ToBool(ctx, wv))
        JS_SetPropertyStr(ctx, attrs, "willReadFrequently", JS_TRUE);
    JS_FreeValue(ctx, wv);
    JSValue cv = JS_GetPropertyStr(ctx, options, "colorSpace");
    if (JS_IsString(cv))
        JS_SetPropertyStr(ctx, attrs, "colorSpace", JS_DupValue(ctx, cv));
    JS_FreeValue(ctx, cv);
    return attrs;
}

static JSValue
ns_canvas_get_2d(JSContext *ctx, ns_js *js, ns_canvas_state *st, const ns_node *el,
                 JSValueConst canvas_obj, gboolean offscreen, JSValueConst options)
{
    if (JS_IsObject(st->ctx2d)) return JS_DupValue(ctx, st->ctx2d);
    if (st->context_kind) return JS_NULL;
    gboolean opaque = FALSE;
    JSValue attrs = ns_canvas_2d_attrs(ctx, options, &opaque);
    if (opaque && st->surf) {
        cairo_t *cr = cairo_create(st->surf);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 1);
        cairo_paint(cr);
        cairo_destroy(cr);
    }
    JSValue obj = ns_ctx2d_new(ctx, el, canvas_obj, offscreen, attrs);
    if (JS_IsException(obj)) return obj;
    st->ctx2d = JS_DupValue(ctx, obj);
    st->jsctx = js->main_realm_ctx ? js->main_realm_ctx : js->ctx;
    st->rt = JS_GetRuntime(ctx);
    st->context_kind = 1;
    return obj;
}

static void
ns_canvas_keep_context(JSContext *ctx, JSValueConst canvas_obj, JSValueConst context)
{
    if (!JS_IsObject(canvas_obj) || !JS_IsObject(context)) return;
    JS_DefinePropertyValueStr(ctx, canvas_obj, "__nd_context",
                              JS_DupValue(ctx, context), JS_PROP_CONFIGURABLE);
}

static JSValue
ns_canvas_get_webgl(JSContext *ctx, ns_js *js, ns_canvas_state *st, const ns_node *el,
                    JSValueConst canvas_obj, int version, JSValueConst options)
{
    if (st->context_kind == 1 || st->context_kind == 3 || js->worker_host)
        return JS_NULL;
    JSValue gl = ns_webgl_get_context(ctx, js, canvas_obj, el, version, options);
    if (!JS_IsNull(gl)) {
        st->context_kind = 2;
        ns_canvas_keep_context(ctx, canvas_obj, gl);
    }
    return gl;
}

static JSValue
ns_canvas_get_webgpu(JSContext *ctx, ns_js *js, ns_canvas_state *st, const ns_node *el,
                     JSValueConst canvas_obj)
{
#ifdef ND_HAVE_WEBGPU
    if (!st->context_kind || st->context_kind == 3) {
        JSValue gpu = ns_webgpu_get_context(ctx, js, canvas_obj, el);
        if (JS_IsObject(gpu)) {
            st->context_kind = 3;
            ns_canvas_keep_context(ctx, canvas_obj, gpu);
        }
        return gpu;
    }
#else
    (void)ctx; (void)js; (void)st; (void)el; (void)canvas_obj;
#endif
    return JS_NULL;
}

static JSValue
ns_canvas_get_unknown(JSContext *ctx, gboolean offscreen, ns_canvas_ctx_type type)
{
    if (!offscreen || type == NS_CTX_BITMAP) return JS_NULL;
    return JS_ThrowTypeError(ctx,
        "Failed to execute 'getContext' on 'OffscreenCanvas': The provided "
        "value is not a valid enum value of type OffscreenRenderingContextId.");
}

static JSValue
ns_canvas_get_by_type(JSContext *ctx, ns_js *js, ns_canvas_state *st, const ns_node *el,
                      JSValueConst canvas_obj, gboolean offscreen,
                      ns_canvas_ctx_type type, JSValueConst options)
{
    switch (type) {
    case NS_CTX_2D:
        return ns_canvas_get_2d(ctx, js, st, el, canvas_obj, offscreen, options);
    case NS_CTX_WEBGL:
        return ns_canvas_get_webgl(ctx, js, st, el, canvas_obj, 1, options);
    case NS_CTX_WEBGL2:
        return ns_canvas_get_webgl(ctx, js, st, el, canvas_obj, 2, options);
    case NS_CTX_WEBGPU:
        return ns_canvas_get_webgpu(ctx, js, st, el, canvas_obj);
    default:
        return ns_canvas_get_unknown(ctx, offscreen, type);
    }
}

static JSValue
ns_canvas_get_context(JSContext *ctx, const ns_node *el, JSValueConst canvas_obj,
                      gboolean offscreen, int argc, JSValueConst *argv)
{
    ns_js *js = js_from_ctx(ctx);
    ns_canvas_state *st = js && el ? ns_canvas_state_for(js, el) : NULL;
    if (!st) return JS_NULL;
    gboolean threw = FALSE;
    ns_canvas_ctx_type type = ns_canvas_ctx_type_of(ctx, argv[0], &threw);
    if (threw) return JS_EXCEPTION;
    JSValueConst options = argc >= 2 ? argv[1] : JS_UNDEFINED;
    return ns_canvas_get_by_type(ctx, js, st, el, canvas_obj, offscreen, type, options);
}

JSValue
ns_element_getContext(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    const ns_node *el = ns_unwrap_element(this_val);
    if (!el || !js_from_ctx(ctx)) return JS_NULL;
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "Failed to execute 'getContext' on 'HTMLCanvasElement': "
            "1 argument required, but only 0 present.");
    return ns_canvas_get_context(ctx, el, this_val, FALSE, argc, argv);
}

JSValue
ns_offscreen_getContext(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    return ns_canvas_get_context(ctx, ns_offscreen_node(this_val), this_val, TRUE,
                                 argc, argv);
}

cairo_status_t
ns_canvas_png_write(void *closure, const unsigned char *data, unsigned int length)
{
    g_byte_array_append((GByteArray *)closure, data, length);
    return CAIRO_STATUS_SUCCESS;
}

static JSValue
ns_canvas_make_blob(JSContext *ctx, const guint8 *data, gsize len, const char *type)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue blob_ctor = JS_GetPropertyStr(ctx, global, "Blob");
    JSValue u8_ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JS_FreeValue(ctx, global);
    JSValue ab = JS_NewArrayBufferCopy(ctx, data, len);
    JSValueConst u8_args[1] = { ab };
    JSValue u8 = JS_CallConstructor(ctx, u8_ctor, 1, u8_args);
    JSValue parts = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, parts, 0, u8);
    JSValue opts = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, opts, "type", JS_NewString(ctx, type));
    JSValueConst args[2] = { parts, opts };
    JSValue blob = JS_CallConstructor(ctx, blob_ctor, 2, args);
    JS_FreeValue(ctx, opts);
    JS_FreeValue(ctx, parts);
    JS_FreeValue(ctx, ab);
    JS_FreeValue(ctx, u8_ctor);
    JS_FreeValue(ctx, blob_ctor);
    return blob;
}

static JSValue
ns_canvas_png_blob(JSContext *ctx, ns_canvas_state *st)
{
    GByteArray *buf = g_byte_array_new();
    JSValue blob = JS_NULL;
    if (cairo_surface_write_to_png_stream(st->surf, ns_canvas_png_write, buf)
            == CAIRO_STATUS_SUCCESS)
        blob = ns_canvas_make_blob(ctx, buf->data, buf->len, "image/png");
    g_byte_array_free(buf, TRUE);
    return blob;
}

JSValue
ns_offscreen_convertToBlob(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue resolvers[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolvers);
    if (JS_IsException(promise)) return promise;
    const ns_node *el = ns_offscreen_node(this_val);
    JSValue result = JS_NULL;
    gboolean rejected = FALSE;
    if (el && js_from_ctx(ctx)) {
        ns_canvas_state *st = ns_canvas_state_for(js_from_ctx(ctx), el);
        if (st && !st->origin_clean) {
            ns_canvas_throw_dom(ctx, "SecurityError",
                                "Tainted canvases may not be exported.");
            result = JS_GetException(ctx);
            rejected = TRUE;
        } else if (st && st->surf) {
            result = ns_canvas_png_blob(ctx, st);
        }
    }
    JS_FreeValue(ctx, JS_Call(ctx, resolvers[rejected ? 1 : 0], JS_UNDEFINED, 1, &result));
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, resolvers[0]);
    JS_FreeValue(ctx, resolvers[1]);
    return promise;
}

void ns_canvas_register_image_bitmap_class(JSRuntime *rt)
{
    ns_new_class_id(&ns_image_bitmap_class_id);
    JS_NewClass(rt, ns_image_bitmap_class_id, &ns_image_bitmap_class);
}

void ns_canvas_register_path2d_class(JSRuntime *rt)
{
    ns_new_class_id(&ns_path2d_class_id);
    JS_NewClass(rt, ns_path2d_class_id, &ns_path2d_class);
}
