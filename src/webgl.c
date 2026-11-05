/* Nordstjernen — minimalist WebGL: canvas.getContext mapped onto GLES via GTK.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "webgl.h"
#include "js_classid.h"
#include "js_internal.h"

#if defined(NS_ENABLE_WEBGL)

#include <stdint.h>
#include <string.h>

#include <epoxy/gl.h>

#include "config.h"
#include "glctx.h"
#include "js.h"
#include "net.h"

#define NS_UNPACK_FLIP_Y_WEBGL              0x9240
#define NS_UNPACK_PREMULTIPLY_ALPHA_WEBGL   0x9241
#define NS_UNPACK_COLORSPACE_CONVERSION_WEBGL 0x9243
#define NS_UNMASKED_VENDOR_WEBGL           0x9245
#define NS_UNMASKED_RENDERER_WEBGL         0x9246
#define NS_MAX_TEXTURE_MAX_ANISOTROPY_EXT  0x84FF
#define NS_TEXTURE_MAX_ANISOTROPY_EXT      0x84FE

typedef struct ns_webgl {
    ns_js         *js;
    JSContext     *ctx;
    JSValue        js_obj;
    JSValue        canvas_obj;
    JSValue        extensions;
    gboolean       lost;
    const ns_node *canvas;
    int            version;
    ns_gl_context *gl;
    GLuint         fbo, color_tex, depth_rb;
    GLuint         draw_fbo, msaa_color_rb, msaa_depth_rb;
    GLuint         user_draw_fbo, user_read_fbo;
    GLuint         bound_draw_fbo, bound_read_fbo;
    GLuint         bound_array_buffer, bound_element_array_buffer;
    int            samples;
    int            w, h;
    guint32        size_attr_gen;
    gboolean       size_synced;
    cairo_surface_t *surf;
    uint8_t       *readback;
    size_t         readback_len;
    GLuint         flip_fbo, flip_rb;
    int            flip_w, flip_h;
    GLuint         pbo[2];
    GLsync         pbo_fence[2];
    gboolean       pbo_pending[2];
    int            pbo_next;
    size_t         pbo_size;
    gint64         last_readback_us;
    GSource       *pbo_timer;
    gboolean       dirty;
    gboolean       repaint_queued;
    GLenum         injected_error;
    GHashTable    *attrib_masks;
    gboolean       drawing_p3, unpack_p3;
    gboolean       unpack_flip_y;
    gboolean       premultiply;
    gboolean       premultiplied_alpha;
    gboolean       depth, stencil, alpha, antialias, preserve;
    GHashTable    *syncs;
    GHashTable    *bound_buffers;
    GHashTable    *buffer_sizes;
    GHashTable    *elem_data;
    GHashTable    *uncleared_rbs;
    int            next_sync;
} ns_webgl;

static JSClassID ns_webgl_class_id;
static GHashTable *g_webgl_by_node;
static ns_webgl *wgl_active;

static GHashTable *g_webgl_decisions;
static char *g_webgl_pending;

static void
ns_webgl_record_decision(const char *origin, gboolean allow)
{
    if (!g_webgl_decisions)
        g_webgl_decisions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
    g_hash_table_insert(g_webgl_decisions, g_strdup(origin),
                        GINT_TO_POINTER(allow ? 1 : 2));
}

char *
ns_webgl_take_pending_origin(void)
{
    char *origin = g_webgl_pending;
    g_webgl_pending = NULL;
    return origin;
}

void
ns_webgl_set_decision(const char *origin, int allow)
{
    ns_webgl_set_state(origin, allow ? 1 : 2);
}

void
ns_webgl_set_state(const char *origin, int state)
{
    if (!origin || !*origin) return;
    if (state == 1 || state == 2)
        ns_webgl_record_decision(origin, state == 1);
    else if (g_webgl_decisions)
        g_hash_table_remove(g_webgl_decisions, origin);
    if (g_webgl_pending && g_strcmp0(g_webgl_pending, origin) == 0)
        g_clear_pointer(&g_webgl_pending, g_free);
}

void
ns_webgl_clear_decisions(void)
{
    if (g_webgl_decisions)
        g_hash_table_remove_all(g_webgl_decisions);
}

static gboolean
ns_webgl_permission(ns_js *js)
{
    const char *url = ns_js_current_url(js);
    char *origin = ns_url_origin_from(url);
    if (!origin || !*origin) {
        g_free(origin);
        origin = g_strdup(url && *url ? url : "this page");
    }

    const ns_config *cfg = ns_config_get();
    if (cfg && !cfg->webgl_enabled) {
        g_free(origin);
        return FALSE;
    }

    if (!g_webgl_decisions)
        g_webgl_decisions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
    gpointer recorded = NULL;
    if (g_hash_table_lookup_extended(g_webgl_decisions, origin, NULL,
                                     &recorded)) {
        g_free(origin);
        return GPOINTER_TO_INT(recorded) != 2;
    }

    g_hash_table_insert(g_webgl_decisions, g_strdup(origin),
                        GINT_TO_POINTER(1));
    g_free(g_webgl_pending);
    g_webgl_pending = g_strdup(origin);
    g_free(origin);
    return TRUE;
}

static int
ns_webgl_dim(const ns_node *el, const char *name, int defv)
{
    const char *s = ns_element_get_attr(el, name);
    if (!s || !*s) return defv;
    long v = strtol(s, NULL, 10);
    if (v <= 0) return defv;
    if (v > 8192) v = 8192;
    return (int)v;
}

static GLuint
ns_webgl_draw_target(ns_webgl *g)
{
    return g->samples > 1 ? g->draw_fbo : g->fbo;
}

static void
wgl_bind_framebuffer(ns_webgl *g, GLenum target, GLuint fbo)
{
    if (!g) return;
    if (target == GL_FRAMEBUFFER) {
        if (g->bound_draw_fbo == fbo && g->bound_read_fbo == fbo)
            return;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        g->bound_draw_fbo = fbo;
        g->bound_read_fbo = fbo;
    } else if (target == GL_DRAW_FRAMEBUFFER) {
        if (g->bound_draw_fbo == fbo)
            return;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        g->bound_draw_fbo = fbo;
    } else if (target == GL_READ_FRAMEBUFFER) {
        if (g->bound_read_fbo == fbo)
            return;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        g->bound_read_fbo = fbo;
    } else {
        glBindFramebuffer(target, fbo);
    }
}

static void
wgl_bind_current_targets(ns_webgl *g)
{
    GLuint dt = ns_webgl_draw_target(g);
    GLuint draw = g->user_draw_fbo ? g->user_draw_fbo : dt;
    GLuint read = g->user_read_fbo ? g->user_read_fbo : dt;
    if (draw == read) {
        wgl_bind_framebuffer(g, GL_FRAMEBUFFER, draw);
    } else {
        wgl_bind_framebuffer(g, GL_DRAW_FRAMEBUFFER, draw);
        wgl_bind_framebuffer(g, GL_READ_FRAMEBUFFER, read);
    }
}

static void
wgl_mark_dirty(ns_webgl *g)
{
    if (!g) return;
    g->dirty = TRUE;
    if (!g->repaint_queued) {
        ns_js_request_repaint(g->js);
        g->repaint_queued = TRUE;
    }
}

static uint8_t *
wgl_readback_buffer(ns_webgl *g, size_t need)
{
    if (!g || need == 0) return NULL;
    if (need <= g->readback_len) return g->readback;
    uint8_t *p = g_try_realloc(g->readback, need);
    if (!p) return NULL;
    memset(p + g->readback_len, 0, need - g->readback_len);
    g->readback = p;
    g->readback_len = need;
    return p;
}

static void
wgl_copy_opaque_rgba_row(uint8_t *dst, const uint8_t *src, int w,
                         gboolean force_alpha)
{
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    for (int x = 0; x < w; x++) {
        uint32_t v;
        memcpy(&v, src + (size_t)x * 4, sizeof v);
        uint32_t a = force_alpha ? 0xff000000u : (v & 0xff000000u);
        v = a | (v & 0x0000ff00u) |
            ((v & 0x000000ffu) << 16) |
            ((v & 0x00ff0000u) >> 16);
        memcpy(dst + (size_t)x * 4, &v, sizeof v);
    }
#else
    for (int x = 0; x < w; x++) {
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = force_alpha ? 255u : src[x * 4 + 3];
    }
#endif
}

static gboolean
ns_webgl_alloc_storage(ns_webgl *g, int w, int h)
{
    GLenum ds_format = (g->depth && g->stencil) ? GL_DEPTH24_STENCIL8
                     : g->stencil               ? GL_STENCIL_INDEX8
                     : g->depth                  ? GL_DEPTH_COMPONENT16
                                                 : 0;
    GLenum ds_attach = (g->depth && g->stencil) ? GL_DEPTH_STENCIL_ATTACHMENT
                     : g->stencil               ? GL_STENCIL_ATTACHMENT
                                                 : GL_DEPTH_ATTACHMENT;

    wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->fbo);
    glBindTexture(GL_TEXTURE_2D, g->color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g->color_tex, 0);

    if (g->samples > 1) {
        wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->draw_fbo);
        glBindRenderbuffer(GL_RENDERBUFFER, g->msaa_color_rb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, g->samples,
                                         GL_RGBA8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, g->msaa_color_rb);
        if (ds_format) {
            glBindRenderbuffer(GL_RENDERBUFFER, g->msaa_depth_rb);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, g->samples,
                                             ds_format, w, h);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, ds_attach,
                                      GL_RENDERBUFFER, g->msaa_depth_rb);
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            g->samples = 1;
    }

    if (g->samples <= 1 && ds_format) {
        wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->fbo);
        glBindRenderbuffer(GL_RENDERBUFFER, g->depth_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, ds_format, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, ds_attach,
                                  GL_RENDERBUFFER, g->depth_rb);
    }

    wgl_bind_framebuffer(g, GL_FRAMEBUFFER, ns_webgl_draw_target(g));
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void
wgl_resolve_blit(int w, int h)
{
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

static void
ns_webgl_sync_size(ns_webgl *g)
{
    guint32 gen = g->canvas ? g->canvas->attr_gen : 0;
    if (g->size_synced && gen == g->size_attr_gen) return;
    g->size_attr_gen = gen;
    g->size_synced = TRUE;

    int w = ns_webgl_dim(g->canvas, "width", 300);
    int h = ns_webgl_dim(g->canvas, "height", 150);
    if (w == g->w && h == g->h) return;
    g->w = w;
    g->h = h;
    ns_webgl_alloc_storage(g, w, h);
    glViewport(0, 0, w, h);
    wgl_mark_dirty(g);
    if (g->surf) { cairo_surface_destroy(g->surf); g->surf = NULL; }
}

static gboolean
ns_webgl_attr(JSContext *ctx, JSValueConst attrs, const char *name, gboolean defv)
{
    if (!JS_IsObject(attrs)) return defv;
    JSValue v = JS_GetPropertyStr(ctx, attrs, name);
    gboolean r = defv;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
        r = JS_ToBool(ctx, v) ? TRUE : FALSE;
    JS_FreeValue(ctx, v);
    return r;
}

static void ns_webgl_free(ns_webgl *g);

static void
wgl_pbo_release(ns_webgl *g)
{
    for (int i = 0; i < 2; i++) {
        if (g->pbo_fence[i]) glDeleteSync(g->pbo_fence[i]);
        g->pbo_fence[i] = NULL;
        g->pbo_pending[i] = FALSE;
    }
    if (g->pbo[0]) glDeleteBuffers(2, g->pbo);
    g->pbo[0] = g->pbo[1] = 0;
    g->pbo_size = 0;
}

static void
wgl_enable_shader_point_size(void)
{
    if (!epoxy_is_desktop_gl()) return;
    glEnable(GL_PROGRAM_POINT_SIZE);
    GLint profile = 0;
    if (epoxy_gl_version() >= 32)
        glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    if (epoxy_gl_version() < 32 || (profile & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT))
        glEnable(GL_POINT_SPRITE);
    while (glGetError() != GL_NO_ERROR) {}
}

static ns_webgl *
ns_webgl_make(JSContext *ctx, ns_js *js, const ns_node *canvas, int version,
              JSValueConst attrs)
{
    ns_gl_context *gl = ns_gl_context_create();
    if (!gl) return NULL;
    if (!ns_gl_context_make_current(gl)) {
        ns_gl_context_destroy(gl);
        return NULL;
    }

    ns_webgl *g = g_new0(ns_webgl, 1);
    g->js = js;
    g->canvas = canvas;
    g->version = version;
    g->gl = gl;
    g->js_obj = JS_UNDEFINED;
    g->canvas_obj = JS_UNDEFINED;
    g->extensions = JS_UNDEFINED;
    g->alpha     = ns_webgl_attr(ctx, attrs, "alpha", TRUE);
    g->depth     = ns_webgl_attr(ctx, attrs, "depth", TRUE);
    g->stencil   = ns_webgl_attr(ctx, attrs, "stencil", FALSE);
    g->antialias = ns_webgl_attr(ctx, attrs, "antialias", TRUE);
    g->preserve  = ns_webgl_attr(ctx, attrs, "preserveDrawingBuffer", FALSE);
    g->premultiplied_alpha = ns_webgl_attr(ctx, attrs, "premultipliedAlpha", TRUE);
    g->w = ns_webgl_dim(canvas, "width", 300);
    g->h = ns_webgl_dim(canvas, "height", 150);
    g->dirty = TRUE;

    g->samples = 1;
    if (g->antialias) {
        GLint max_samples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
        g->samples = max_samples >= 4 ? 4 : (max_samples > 1 ? max_samples : 1);
    }
    g->antialias = g->samples > 1;

    glGenFramebuffers(1, &g->fbo);
    glGenTextures(1, &g->color_tex);
    glGenRenderbuffers(1, &g->depth_rb);
    if (g->samples > 1) {
        glGenFramebuffers(1, &g->draw_fbo);
        glGenRenderbuffers(1, &g->msaa_color_rb);
        glGenRenderbuffers(1, &g->msaa_depth_rb);
    }
    if (!ns_webgl_alloc_storage(g, g->w, g->h)) {
        ns_webgl_free(g);
        return NULL;
    }
    glViewport(0, 0, g->w, g->h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    wgl_enable_shader_point_size();
    return g;
}

static void
ns_webgl_free(ns_webgl *g)
{
    if (!g) return;
    if (g->gl) {
        ns_gl_context_make_current(g->gl);
        glDeleteFramebuffers(1, &g->fbo);
        glDeleteTextures(1, &g->color_tex);
        glDeleteRenderbuffers(1, &g->depth_rb);
        if (g->draw_fbo) glDeleteFramebuffers(1, &g->draw_fbo);
        if (g->msaa_color_rb) glDeleteRenderbuffers(1, &g->msaa_color_rb);
        if (g->msaa_depth_rb) glDeleteRenderbuffers(1, &g->msaa_depth_rb);
        if (g->flip_fbo) glDeleteFramebuffers(1, &g->flip_fbo);
        if (g->flip_rb) glDeleteRenderbuffers(1, &g->flip_rb);
        wgl_pbo_release(g);
        if (g->syncs) {
            GHashTableIter it;
            gpointer k, v;
            g_hash_table_iter_init(&it, g->syncs);
            while (g_hash_table_iter_next(&it, &k, &v))
                glDeleteSync((GLsync)v);
        }
        ns_gl_context_release(g->gl);
        ns_gl_context_destroy(g->gl);
    }
    if (wgl_active == g)
        wgl_active = NULL;
    else if (wgl_active && wgl_active->gl)
        ns_gl_context_make_current(wgl_active->gl);
    if (g->syncs) g_hash_table_destroy(g->syncs);
    if (g->bound_buffers) g_hash_table_destroy(g->bound_buffers);
    if (g->buffer_sizes) g_hash_table_destroy(g->buffer_sizes);
    if (g->attrib_masks) g_hash_table_destroy(g->attrib_masks);
    if (g->elem_data) g_hash_table_destroy(g->elem_data);
    if (g->uncleared_rbs) g_hash_table_destroy(g->uncleared_rbs);
    if (g->pbo_timer) {
        g_source_destroy(g->pbo_timer);
        g_source_unref(g->pbo_timer);
    }
    if (g->surf) cairo_surface_destroy(g->surf);
    g_free(g->readback);
    g_free(g);
}

static void
ns_webgl_finalizer(JSRuntime *rt, JSValue val)
{
    ns_webgl *g = JS_GetOpaque(val, ns_webgl_class_id);
    if (!g) return;
    if (g_webgl_by_node)
        g_hash_table_remove(g_webgl_by_node, g->canvas);
    JS_FreeValueRT(rt, g->canvas_obj);
    JS_FreeValueRT(rt, g->extensions);
    ns_webgl_free(g);
}

static void
ns_webgl_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_webgl *g = JS_GetOpaque(val, ns_webgl_class_id);
    if (g) {
        JS_MarkValue(rt, g->canvas_obj, mark_func);
        JS_MarkValue(rt, g->extensions, mark_func);
    }
}

static JSClassDef ns_webgl_class = {
    "WebGLRenderingContext",
    .finalizer = ns_webgl_finalizer,
    .gc_mark = ns_webgl_gc_mark,
};

static ns_webgl *
wgl_cur(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    ns_webgl *g = JS_GetOpaque(this_val, ns_webgl_class_id);
    if (!g || !g->gl) return NULL;
    ns_gl_context_make_current(g->gl);
    wgl_active = g;
    ns_webgl_sync_size(g);
    wgl_bind_current_targets(g);
    return g;
}

static ns_webgl *
wgl_brand(JSContext *ctx, JSValueConst this_val)
{
    ns_webgl *g = JS_GetOpaque(this_val, ns_webgl_class_id);
    if (!g) JS_ThrowTypeError(ctx, "Illegal invocation");
    return g;
}

static JSValue
wgl_no_context(JSContext *ctx, JSValueConst this_val)
{
    return wgl_brand(ctx, this_val) ? JS_UNDEFINED : JS_EXCEPTION;
}

static void
wgl_reassert(ns_webgl *keep)
{
    if (keep && keep->gl) {
        ns_gl_context_make_current(keep->gl);
        wgl_active = keep;
    }
}

static int
argi(JSContext *ctx, int argc, JSValueConst *argv, int i)
{
    int32_t v = 0;
    if (i < argc) {
        ns_webgl *keep = wgl_active;
        JS_ToInt32(ctx, &v, argv[i]);
        if (JS_IsObject(argv[i])) wgl_reassert(keep);
    }
    return v;
}

static double
argd(JSContext *ctx, int argc, JSValueConst *argv, int i)
{
    double v = 0;
    if (i < argc) {
        ns_webgl *keep = wgl_active;
        JS_ToFloat64(ctx, &v, argv[i]);
        if (JS_IsObject(argv[i])) wgl_reassert(keep);
    }
    return v;
}

static gboolean
argbool(JSContext *ctx, int argc, JSValueConst *argv, int i)
{
    return (i < argc) ? (JS_ToBool(ctx, argv[i]) ? TRUE : FALSE) : FALSE;
}

static JSClassID ns_webgl_obj_class_id;

typedef struct ns_webgl_obj {
    int    kind;
    GLuint name;
} ns_webgl_obj;

static const struct { const char *kind; const char *iface; } wgl_object_kinds[] = {
    { "buffer", "WebGLBuffer" }, { "framebuffer", "WebGLFramebuffer" },
    { "program", "WebGLProgram" }, { "renderbuffer", "WebGLRenderbuffer" },
    { "shader", "WebGLShader" }, { "texture", "WebGLTexture" },
    { "query", "WebGLQuery" }, { "sampler", "WebGLSampler" },
    { "sync", "WebGLSync" }, { "transformfeedback", "WebGLTransformFeedback" },
    { "vertexarray", "WebGLVertexArrayObject" },
    { "location", "WebGLUniformLocation" },
};

static void
ns_webgl_obj_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    g_free(JS_GetOpaque(val, ns_webgl_obj_class_id));
}

static JSClassDef ns_webgl_obj_class = {
    "WebGLObject",
    .finalizer = ns_webgl_obj_finalizer,
};

static int
wgl_name(JSContext *ctx, JSValueConst v)
{
    (void)ctx;
    ns_webgl_obj *o = JS_GetOpaque(v, ns_webgl_obj_class_id);
    return o ? (int)o->name : 0;
}

static int
wgl_loc(JSContext *ctx, JSValueConst v)
{
    (void)ctx;
    ns_webgl_obj *o = JS_GetOpaque(v, ns_webgl_obj_class_id);
    return o && o->kind == 11 ? (int)o->name : -1;
}

static JSValue
wgl_new_object(ns_webgl *g, JSContext *ctx, GLuint name, const char *kind)
{
    int k = 0;
    while (k < 11 && strcmp(wgl_object_kinds[k].kind, kind) != 0) k++;
    JSValue proto = ns_api_proto(ns_canvas_realm(ctx, g->canvas), wgl_object_kinds[k].iface);
    JSValue o = JS_IsObject(proto)
        ? JS_NewObjectProtoClass(ctx, proto, ns_webgl_obj_class_id)
        : JS_NewObjectClass(ctx, ns_webgl_obj_class_id);
    JS_FreeValue(ctx, proto);
    ns_webgl_obj *d = g_new0(ns_webgl_obj, 1);
    d->kind = k;
    d->name = name;
    JS_SetOpaque(o, d);
    return o;
}

static JSValue
wgl_wrap(ns_webgl *g, JSContext *ctx, GLuint name, const char *kind)
{
    if (!name) return JS_NULL;
    return wgl_new_object(g, ctx, name, kind);
}

static JSValue
wgl_typed_array(JSContext *ctx, JSValueConst buf, JSTypedArrayEnum type)
{
    JSValueConst args[3] = { buf, JS_UNDEFINED, JS_UNDEFINED };
    return JS_NewTypedArray(ctx, 3, args, type);
}

static const uint8_t *
view_bytes(JSContext *ctx, JSValueConst v, size_t *out_len, JSValue *hold)
{
    *hold = JS_UNDEFINED;
    *out_len = 0;
    if (JS_IsArrayBuffer(v)) {
        size_t n = 0;
        uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
        *out_len = n;
        return p;
    }
    size_t off = 0, len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NULL;
    }
    size_t tot = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &tot, buf);
    if (!base || off + len > tot) {
        JS_FreeValue(ctx, buf);
        return NULL;
    }
    *hold = buf;
    *out_len = len;
    return base + off;
}

static GLenum
wgl_buffer_binding_query(GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER:              return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER:      return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    case GL_COPY_READ_BUFFER:          return GL_COPY_READ_BUFFER_BINDING;
    case GL_COPY_WRITE_BUFFER:         return GL_COPY_WRITE_BUFFER_BINDING;
    case GL_PIXEL_PACK_BUFFER:         return GL_PIXEL_PACK_BUFFER_BINDING;
    case GL_PIXEL_UNPACK_BUFFER:       return GL_PIXEL_UNPACK_BUFFER_BINDING;
    case GL_TRANSFORM_FEEDBACK_BUFFER: return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
    case GL_UNIFORM_BUFFER:            return GL_UNIFORM_BUFFER_BINDING;
    default:                           return 0;
    }
}

static GLuint
wgl_bound_buffer(ns_webgl *g, GLenum target)
{
    if (!g) return 0;
    GLenum query = wgl_buffer_binding_query(target);
    if (!query) return 0;
    GLint name = 0;
    glGetIntegerv(query, &name);
    return name > 0 ? (GLuint)name : 0;
}

static void
wgl_set_bound_buffer(ns_webgl *g, GLenum target, GLuint name)
{
    if (!g) return;
    if (target == GL_ARRAY_BUFFER) {
        g->bound_array_buffer = name;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        g->bound_element_array_buffer = name;
    } else {
        if (!g->bound_buffers)
            g->bound_buffers = g_hash_table_new(g_direct_hash, g_direct_equal);
        if (name)
            g_hash_table_insert(g->bound_buffers, GUINT_TO_POINTER(target),
                                GUINT_TO_POINTER(name));
        else
            g_hash_table_remove(g->bound_buffers, GUINT_TO_POINTER(target));
    }
}

static void
wgl_set_buffer_size(ns_webgl *g, GLuint name, size_t size)
{
    if (!g || !name) return;
    if (!g->buffer_sizes)
        g->buffer_sizes = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(g->buffer_sizes, GUINT_TO_POINTER(name),
                        GSIZE_TO_POINTER(size));
}

static size_t
wgl_buffer_size(ns_webgl *g, GLuint name)
{
    if (!g || !name || !g->buffer_sizes) return 0;
    return GPOINTER_TO_SIZE(g_hash_table_lookup(g->buffer_sizes,
                                                GUINT_TO_POINTER(name)));
}

static GByteArray *
wgl_elem_shadow_get(ns_webgl *g, GLuint name)
{
    if (!g || !g->elem_data || !name) return NULL;
    return g_hash_table_lookup(g->elem_data, GUINT_TO_POINTER(name));
}

static void
wgl_elem_shadow_clear(ns_webgl *g, GLuint name)
{
    if (g && g->elem_data && name)
        g_hash_table_remove(g->elem_data, GUINT_TO_POINTER(name));
}

static void
wgl_elem_shadow_set(ns_webgl *g, GLuint name, const uint8_t *p, size_t len)
{
    if (!g || !name) return;
    if (!g->elem_data)
        g->elem_data = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                             NULL, (GDestroyNotify)g_byte_array_unref);
    GByteArray *a = g_byte_array_sized_new(len);
    g_byte_array_set_size(a, (guint)len);
    if (p) memcpy(a->data, p, len);
    else if (len) memset(a->data, 0, len);
    g_hash_table_insert(g->elem_data, GUINT_TO_POINTER(name), a);
}

static void
wgl_elem_shadow_patch(ns_webgl *g, GLuint name, size_t off,
                      const uint8_t *p, size_t len)
{
    GByteArray *a = wgl_elem_shadow_get(g, name);
    if (a && p && off + len <= a->len) memcpy(a->data + off, p, len);
}

static int
wgl_attr_elem_bytes(GLenum type, GLint size)
{
    switch (type) {
    case GL_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_2_10_10_10_REV:
        return 4;
    default:
        break;
    }
    int comp;
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:                     comp = 1; break;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: comp = 2; break;
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: case GL_FIXED: comp = 4; break;
    default: return 0;
    }
    if (size < 1 || size > 4) return 0;
    return comp * size;
}

static int
wgl_attr_type_cols(GLenum t)
{
    switch (t) {
    case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: return 2;
    case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: return 3;
    case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: return 4;
    default: return 1;
    }
}

static uint64_t
wgl_program_attrib_mask_query(GLint prog)
{
    GLint nattr = 0;
    glGetProgramiv((GLuint)prog, GL_ACTIVE_ATTRIBUTES, &nattr);
    if (nattr <= 0) return 0;
    GLint maxlen = 0;
    glGetProgramiv((GLuint)prog, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &maxlen);
    if (maxlen <= 0) maxlen = 64;
    char *name = g_malloc((size_t)maxlen + 1);
    uint64_t mask = 0;
    for (GLint i = 0; i < nattr; i++) {
        GLint asize = 0; GLenum atype = 0; GLsizei wr = 0;
        name[0] = 0;
        glGetActiveAttrib((GLuint)prog, (GLuint)i, maxlen, &wr, &asize, &atype, name);
        if (strncmp(name, "gl_", 3) == 0) continue;
        GLint loc = glGetAttribLocation((GLuint)prog, name);
        if (loc < 0) continue;
        int slots = wgl_attr_type_cols(atype) * (asize > 0 ? asize : 1);
        for (int s = 0; s < slots; s++) {
            int l = loc + s;
            if (l >= 0 && l < 64) mask |= (uint64_t)1 << l;
        }
    }
    g_free(name);
    return mask;
}

static uint64_t
wgl_program_attrib_mask(ns_webgl *g)
{
    GLint prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    if (prog <= 0) return 0;
    if (!g->attrib_masks)
        g->attrib_masks = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                NULL, g_free);
    guint64 *cached = g_hash_table_lookup(g->attrib_masks,
                                          GINT_TO_POINTER(prog));
    if (cached) return *cached;
    guint64 *mask = g_new(guint64, 1);
    *mask = wgl_program_attrib_mask_query(prog);
    g_hash_table_insert(g->attrib_masks, GINT_TO_POINTER(prog), mask);
    return *mask;
}

static void
wgl_forget_program(ns_webgl *g, GLuint prog)
{
    if (g->attrib_masks)
        g_hash_table_remove(g->attrib_masks, GUINT_TO_POINTER(prog));
}

static void
wgl_check_all_attribs_for(ns_webgl *g, GLuint prog)
{
    if (prog == 0) return;
    if (!g->attrib_masks)
        g->attrib_masks = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                NULL, g_free);
    GLint max_attribs = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attribs);
    if (max_attribs <= 0) max_attribs = 16;
    guint64 *mask = g_new(guint64, 1);
    *mask = max_attribs >= 64 ? G_MAXUINT64
                              : (((guint64)1 << max_attribs) - 1);
    g_hash_table_insert(g->attrib_masks, GUINT_TO_POINTER(prog), mask);
}

static gboolean
wgl_elem_max_index(const uint8_t *data, size_t len, GLintptr offset,
                   GLsizei count, int isz, int version, uint64_t *out_max)
{
    if (!data || count <= 0 || isz <= 0) return FALSE;
    uint64_t span;
    if (__builtin_mul_overflow((uint64_t)count, (uint64_t)isz, &span) ||
        __builtin_add_overflow(span, (uint64_t)offset, &span) || span > len)
        return FALSE;
    uint64_t restart = isz == 1 ? 0xFFu : isz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    gboolean skip_restart = version >= 2;
    const uint8_t *p = data + offset;
    uint64_t mx = 0;
    gboolean any = FALSE;
    for (GLsizei i = 0; i < count; i++) {
        uint64_t v;
        if (isz == 1) {
            v = p[i];
        } else if (isz == 2) {
            uint16_t t;
            memcpy(&t, p + (size_t)i * 2, 2);
            v = t;
        } else {
            uint32_t t;
            memcpy(&t, p + (size_t)i * 4, 4);
            v = t;
        }
        if (skip_restart && v == restart) continue;
        if (!any || v > mx) { mx = v; any = TRUE; }
    }
    *out_max = any ? mx : 0;
    return TRUE;
}

static gboolean
wgl_attribs_cover(ns_webgl *g, int64_t vertex_last, int64_t instances)
{
    if (!g || vertex_last < 0) return TRUE;
    uint64_t used = wgl_program_attrib_mask(g);
    for (int i = 0; i < 64; i++) {
        if (!(used & ((uint64_t)1 << i))) continue;
        GLuint index = (GLuint)i;
        GLint enabled = 0, buffer = 0, size = 0, type = 0, stride = 0;
        GLint divisor = 0;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        if (!enabled) continue;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
        if (buffer <= 0) return FALSE;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        if (g->version >= 2)
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &divisor);
        void *pointer = NULL;
        glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
        int ebytes = wgl_attr_elem_bytes((GLenum)type, size);
        if (ebytes <= 0 || stride < 0) return FALSE;
        int64_t last = divisor == 0 ? vertex_last
                                    : (instances - 1) / (int64_t)(GLuint)divisor;
        if (last < 0) continue;
        uint64_t eff = stride ? (uint64_t)stride : (uint64_t)ebytes;
        uint64_t need;
        if (__builtin_mul_overflow(eff, (uint64_t)last, &need) ||
            __builtin_add_overflow(need, (uint64_t)(uintptr_t)pointer, &need) ||
            __builtin_add_overflow(need, (uint64_t)ebytes, &need))
            return FALSE;
        if (need > wgl_buffer_size(g, (GLuint)buffer)) return FALSE;
    }
    return TRUE;
}

static int
wgl_floats(JSContext *ctx, JSValueConst v, float *out, int max)
{
    if (JS_GetTypedArrayType(v) == JS_TYPED_ARRAY_FLOAT32) {
        JSValue hold;
        size_t n = 0;
        const uint8_t *b = view_bytes(ctx, v, &n, &hold);
        int cnt = b ? (int)(n / sizeof(float)) : 0;
        if (cnt > max) cnt = max;
        if (b) memcpy(out, b, (size_t)cnt * sizeof(float));
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return cnt;
    }
    ns_webgl *keep = wgl_active;
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    int cnt = (int)len;
    if (cnt > max) cnt = max;
    for (int i = 0; i < cnt; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        double d = 0;
        JS_ToFloat64(ctx, &d, e);
        JS_FreeValue(ctx, e);
        out[i] = (float)d;
    }
    wgl_reassert(keep);
    return cnt;
}

static int
wgl_ints(JSContext *ctx, JSValueConst v, GLint *out, int max)
{
    int t = JS_GetTypedArrayType(v);
    if (t == JS_TYPED_ARRAY_INT32 || t == JS_TYPED_ARRAY_UINT32) {
        JSValue hold;
        size_t n = 0;
        const uint8_t *b = view_bytes(ctx, v, &n, &hold);
        int cnt = b ? (int)(n / sizeof(GLint)) : 0;
        if (cnt > max) cnt = max;
        if (b) memcpy(out, b, (size_t)cnt * sizeof(GLint));
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return cnt;
    }
    ns_webgl *keep = wgl_active;
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    int cnt = (int)len;
    if (cnt > max) cnt = max;
    for (int i = 0; i < cnt; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        int32_t d = 0;
        JS_ToInt32(ctx, &d, e);
        JS_FreeValue(ctx, e);
        out[i] = d;
    }
    wgl_reassert(keep);
    return cnt;
}

#define NS_WEBGL_MAX_ALLOC    (1024u * 1024u * 1024u)
#define NS_WEBGL_MAX_CONTEXTS 32
#define NS_WEBGL_MAX_SHADER   (4u * 1024u * 1024u)

static int
wgl_components(int format)
{
    switch (format) {
    case GL_RED:  case GL_RED_INTEGER:
    case GL_ALPHA: case GL_LUMINANCE:
    case GL_DEPTH_COMPONENT:                       return 1;
    case GL_RG:   case GL_RG_INTEGER:
    case GL_LUMINANCE_ALPHA: case GL_DEPTH_STENCIL: return 2;
    case GL_RGB:  case GL_RGB_INTEGER:
    case GL_SRGB_EXT:                              return 3;
    default:                                       return 4;
    }
}

static int
wgl_type_bytes(int type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:                return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES:         return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT:
    default:                                            return 4;
    }
}

static size_t
wgl_pixel_bytes(int format, int type)
{
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    default:
        return (size_t)wgl_components(format) * (size_t)wgl_type_bytes(type);
    }
}

static size_t
wgl_transfer_bytes(ns_webgl *g, int w, int h, int depth,
                   int format, int type, gboolean pack)
{
    if (w <= 0 || h <= 0 || depth <= 0) return 0;
    size_t pix = wgl_pixel_bytes(format, type);
    GLint align = 4, rowlen = 0, skiprows = 0, skippix = 0;
    GLint imgh = 0, skipimg = 0;
    glGetIntegerv(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT, &align);
    if (g->version >= 2) {
        glGetIntegerv(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH, &rowlen);
        glGetIntegerv(pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS, &skiprows);
        glGetIntegerv(pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS, &skippix);
        if (!pack) {
            glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &imgh);
            glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skipimg);
        }
    }
    if (align < 1) align = 1;
    if (rowlen < 0) rowlen = 0;
    if (skiprows < 0) skiprows = 0;
    if (skippix < 0) skippix = 0;
    if (imgh < 0) imgh = 0;
    if (skipimg < 0) skipimg = 0;

    size_t row, ih, full_rows, last, total;
    if (__builtin_mul_overflow(pix, (size_t)(rowlen > 0 ? rowlen : w), &row))
        return SIZE_MAX;
    size_t rem = row % (size_t)align;
    if (rem) row += (size_t)align - rem;
    ih = (size_t)(imgh > 0 ? imgh : h);
    if (__builtin_add_overflow((size_t)skipimg, (size_t)(depth - 1), &full_rows) ||
        __builtin_mul_overflow(ih, full_rows, &full_rows) ||
        __builtin_add_overflow(full_rows, (size_t)skiprows, &full_rows) ||
        __builtin_add_overflow(full_rows, (size_t)(h - 1), &full_rows))
        return SIZE_MAX;
    if (__builtin_add_overflow((size_t)skippix, (size_t)w, &last) ||
        __builtin_mul_overflow(last, pix, &last) ||
        __builtin_mul_overflow(row, full_rows, &total) ||
        __builtin_add_overflow(total, last, &total))
        return SIZE_MAX;
    return total;
}

static gboolean
wgl_flip_fits(int w, int h, int bpp, size_t len)
{
    size_t row, total;
    if (w <= 0 || h <= 0 || bpp <= 0) return FALSE;
    if (__builtin_mul_overflow((size_t)w, (size_t)bpp, &row) ||
        __builtin_mul_overflow(row, (size_t)h, &total))
        return FALSE;
    return total <= len;
}

static uint8_t *
wgl_flip_rows(const uint8_t *src, int w, int h, int bpp)
{
    size_t row = (size_t)w * (size_t)bpp;
    uint8_t *dst = g_try_malloc(row * (size_t)h);
    if (!dst) return NULL;
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)y * row,
               src + (size_t)(h - 1 - y) * row, row);
    return dst;
}

static gboolean
wgl_flip_safe(ns_webgl *g, int w, int h, int format, int type,
              size_t need, size_t len)
{
    if (!g->unpack_flip_y || type != GL_UNSIGNED_BYTE) return FALSE;
    int bpp = wgl_components(format);
    size_t tight;
    if (__builtin_mul_overflow((size_t)w, (size_t)h, &tight) ||
        __builtin_mul_overflow(tight, (size_t)bpp, &tight))
        return FALSE;
    if (need != tight) return FALSE;
    return wgl_flip_fits(w, h, bpp, len);
}

#define WGL_GET(name) \
    ns_webgl *g = wgl_cur(ctx, this_val); \
    if (!g) return wgl_no_context(ctx, this_val); \
    (void)g; (void)name; (void)argc; (void)argv

static JSValue
wgl_clearColor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glClearColor((float)argd(ctx, argc, argv, 0), (float)argd(ctx, argc, argv, 1),
                 (float)argd(ctx, argc, argv, 2), (float)argd(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_clearDepth(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glClearDepthf((float)argd(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_clearStencil(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glClearStencil(argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glClear((GLbitfield)argi(ctx, argc, argv, 0));
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_viewport(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glViewport(argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
               argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_scissor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glScissor(argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
              argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_enable(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glEnable((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_disable(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDisable((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_isEnabled(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    return JS_NewBool(ctx, glIsEnabled((GLenum)argi(ctx, argc, argv, 0)));
}

static JSValue
wgl_depthFunc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDepthFunc((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_depthMask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDepthMask(argbool(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_depthRange(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDepthRangef((float)argd(ctx, argc, argv, 0), (float)argd(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_colorMask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glColorMask(argbool(ctx, argc, argv, 0), argbool(ctx, argc, argv, 1),
                argbool(ctx, argc, argv, 2), argbool(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_stencilMask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glStencilMask((GLuint)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_stencilFunc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glStencilFunc((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                  (GLuint)argi(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_stencilOp(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glStencilOp((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                (GLenum)argi(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_blendFunc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlendFunc((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_blendFuncSeparate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlendFuncSeparate((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                        (GLenum)argi(ctx, argc, argv, 2), (GLenum)argi(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_blendEquation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlendEquation((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_blendEquationSeparate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlendEquationSeparate((GLenum)argi(ctx, argc, argv, 0),
                            (GLenum)argi(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_blendColor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlendColor((float)argd(ctx, argc, argv, 0), (float)argd(ctx, argc, argv, 1),
                 (float)argd(ctx, argc, argv, 2), (float)argd(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_cullFace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glCullFace((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_frontFace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glFrontFace((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_lineWidth(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glLineWidth((float)argd(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_polygonOffset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glPolygonOffset((float)argd(ctx, argc, argv, 0), (float)argd(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_hint(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glHint((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_finish(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    glFinish();
    return JS_UNDEFINED;
}

static JSValue
wgl_flush(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    glFlush();
    return JS_UNDEFINED;
}

static JSValue
wgl_pixelStorei(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    int pname = argi(ctx, argc, argv, 0);
    int param = argi(ctx, argc, argv, 1);
    if (pname == NS_UNPACK_FLIP_Y_WEBGL) {
        g->unpack_flip_y = param ? TRUE : FALSE;
    } else if (pname == NS_UNPACK_PREMULTIPLY_ALPHA_WEBGL) {
        g->premultiply = param ? TRUE : FALSE;
    } else if (pname == NS_UNPACK_COLORSPACE_CONVERSION_WEBGL) {
    } else if (g->version < 2 &&
               pname != GL_PACK_ALIGNMENT && pname != GL_UNPACK_ALIGNMENT) {
        glPixelStorei(0, 0);
    } else {
        glPixelStorei((GLenum)pname, param);
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_getError(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    GLenum err = glGetError();
    if (g->injected_error) {
        if (err == GL_NO_ERROR) err = g->injected_error;
        g->injected_error = GL_NO_ERROR;
    }
    return JS_NewInt32(ctx, (int)err);
}

static GLint
wgl_param_cap(GLenum pname)
{
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE:                 return 16384;
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE:        return 16384;
    case GL_MAX_RENDERBUFFER_SIZE:            return 16384;
    case GL_MAX_VERTEX_ATTRIBS:               return 16;
    case GL_MAX_VERTEX_UNIFORM_VECTORS:       return 1024;
    case GL_MAX_VARYING_VECTORS:              return 30;
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS:     return 1024;
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:   return 16;
    case GL_MAX_TEXTURE_IMAGE_UNITS:          return 16;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: return 32;
#ifdef GL_MAX_3D_TEXTURE_SIZE
    case GL_MAX_3D_TEXTURE_SIZE:              return 2048;
#endif
#ifdef GL_MAX_ARRAY_TEXTURE_LAYERS
    case GL_MAX_ARRAY_TEXTURE_LAYERS:         return 2048;
#endif
#ifdef GL_MAX_DRAW_BUFFERS
    case GL_MAX_DRAW_BUFFERS:                 return 8;
#endif
#ifdef GL_MAX_COLOR_ATTACHMENTS
    case GL_MAX_COLOR_ATTACHMENTS:            return 8;
#endif
#ifdef GL_MAX_SAMPLES
    case GL_MAX_SAMPLES:                      return 4;
#endif
#ifdef GL_MAX_VERTEX_UNIFORM_COMPONENTS
    case GL_MAX_VERTEX_UNIFORM_COMPONENTS:    return 4096;
#endif
#ifdef GL_MAX_FRAGMENT_UNIFORM_COMPONENTS
    case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:  return 4096;
#endif
#ifdef GL_MAX_VERTEX_OUTPUT_COMPONENTS
    case GL_MAX_VERTEX_OUTPUT_COMPONENTS:     return 64;
#endif
#ifdef GL_MAX_FRAGMENT_INPUT_COMPONENTS
    case GL_MAX_FRAGMENT_INPUT_COMPONENTS:    return 120;
#endif
#ifdef GL_MAX_VARYING_COMPONENTS
    case GL_MAX_VARYING_COMPONENTS:           return 120;
#endif
#ifdef GL_MAX_VERTEX_UNIFORM_BLOCKS
    case GL_MAX_VERTEX_UNIFORM_BLOCKS:        return 12;
#endif
#ifdef GL_MAX_FRAGMENT_UNIFORM_BLOCKS
    case GL_MAX_FRAGMENT_UNIFORM_BLOCKS:      return 12;
#endif
#ifdef GL_MAX_COMBINED_UNIFORM_BLOCKS
    case GL_MAX_COMBINED_UNIFORM_BLOCKS:      return 24;
#endif
#ifdef GL_MAX_UNIFORM_BUFFER_BINDINGS
    case GL_MAX_UNIFORM_BUFFER_BINDINGS:      return 24;
#endif
#ifdef GL_MAX_UNIFORM_BLOCK_SIZE
    case GL_MAX_UNIFORM_BLOCK_SIZE:           return 16384;
#endif
    default:                                  return 0;
    }
}

static int wgl_enabled_compressed_formats(JSContext *ctx, ns_webgl *g,
                                          GLint *out, int cap);

static void
wgl_read_format(ns_webgl *g, GLint *format, GLint *type)
{
    *format = GL_RGBA;
    *type = GL_UNSIGNED_BYTE;
    if (!g->user_read_fbo) return;
    GLint buffer = GL_NONE, object = GL_NONE, component = GL_NONE;
    glGetIntegerv(GL_READ_BUFFER, &buffer);
    if (buffer == GL_NONE) return;
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, (GLenum)buffer,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &object);
    if (object == GL_NONE) return;
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, (GLenum)buffer,
        GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component);
    if (component == GL_INT) {
        *format = GL_RGBA_INTEGER;
        *type = GL_INT;
    } else if (component == GL_UNSIGNED_INT) {
        *format = GL_RGBA_INTEGER;
        *type = GL_UNSIGNED_INT;
    } else if (component == GL_FLOAT) {
        *type = GL_FLOAT;
    }
}

static JSValue
wgl_getParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 0);
    switch (pname) {
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT:
    case GL_IMPLEMENTATION_COLOR_READ_TYPE: {
        GLint format, type;
        wgl_read_format(g, &format, &type);
        return JS_NewInt32(ctx, pname == GL_IMPLEMENTATION_COLOR_READ_FORMAT
                                ? format : type);
    }
    case GL_VENDOR:
        return JS_NewString(ctx, "WebKit");
    case NS_UNMASKED_VENDOR_WEBGL:
        return JS_NewString(ctx, "Google Inc. (Intel)");
    case GL_RENDERER:
        return JS_NewString(ctx, "WebKit WebGL");
    case NS_UNMASKED_RENDERER_WEBGL:
        return JS_NewString(ctx,
            "ANGLE (Intel, Mesa Intel(R) UHD Graphics (CML GT2), OpenGL 4.6)");
    case GL_VERSION:
        return JS_NewString(ctx, g->version >= 2
            ? "WebGL 2.0 (OpenGL ES 3.0 Chromium)"
            : "WebGL 1.0 (OpenGL ES 2.0 Chromium)");
    case GL_SHADING_LANGUAGE_VERSION:
        return JS_NewString(ctx, g->version >= 2
            ? "WebGL GLSL ES 3.00 (OpenGL ES GLSL ES 3.0 Chromium)"
            : "WebGL GLSL ES 1.0 (OpenGL ES GLSL ES 1.0 Chromium)");
    case GL_VIEWPORT:
    case GL_SCISSOR_BOX:
    case GL_MAX_VIEWPORT_DIMS: {
        GLint v[4] = { 0, 0, 0, 0 };
        glGetIntegerv(pname, v);
        int n = (pname == GL_MAX_VIEWPORT_DIMS) ? 2 : 4;
        if (pname == GL_MAX_VIEWPORT_DIMS) {
            if (v[0] > 16384) v[0] = 16384;
            if (v[1] > 16384) v[1] = 16384;
        }
        JSValue a = JS_NewArrayBufferCopy(ctx, (const uint8_t *)v,
                                          (size_t)n * sizeof(GLint));
        JSValue ta = wgl_typed_array(ctx, a, JS_TYPED_ARRAY_INT32);
        JS_FreeValue(ctx, a);
        return ta;
    }
    case GL_COLOR_CLEAR_VALUE:
    case GL_DEPTH_CLEAR_VALUE:
    case GL_BLEND_COLOR:
    case GL_DEPTH_RANGE:
    case GL_ALIASED_LINE_WIDTH_RANGE:
    case GL_ALIASED_POINT_SIZE_RANGE: {
        GLfloat v[4] = { 0, 0, 0, 0 };
        glGetFloatv(pname, v);
        int n = (pname == GL_DEPTH_CLEAR_VALUE)                 ? 1
              : (pname == GL_COLOR_CLEAR_VALUE ||
                 pname == GL_BLEND_COLOR)                       ? 4
                                                               : 2;
        if (pname == GL_ALIASED_LINE_WIDTH_RANGE) {
            v[0] = 1.0f; v[1] = 1.0f;
        } else if (pname == GL_ALIASED_POINT_SIZE_RANGE) {
            v[0] = 1.0f;
            if (v[1] > 1024.0f) v[1] = 1024.0f;
        }
        JSValue a = JS_NewArrayBufferCopy(ctx, (const uint8_t *)v,
                                          (size_t)n * sizeof(GLfloat));
        JSValue ta = wgl_typed_array(ctx, a, JS_TYPED_ARRAY_FLOAT32);
        JS_FreeValue(ctx, a);
        return ta;
    }
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS: {
        GLint formats[64];
        return JS_NewInt32(ctx, wgl_enabled_compressed_formats(ctx, g, formats, 64));
    }
    case GL_STENCIL_WRITEMASK:
    case GL_STENCIL_BACK_WRITEMASK:
    case GL_STENCIL_VALUE_MASK:
    case GL_STENCIL_BACK_VALUE_MASK: {
        GLint mask = 0;
        glGetIntegerv(pname, &mask);
        return JS_NewUint32(ctx, (uint32_t)mask);
    }
    case NS_UNPACK_FLIP_Y_WEBGL:
        return JS_NewBool(ctx, g->unpack_flip_y);
    case NS_UNPACK_PREMULTIPLY_ALPHA_WEBGL:
        return JS_NewBool(ctx, g->premultiply);
    case GL_DEPTH_TEST:
    case GL_BLEND:
    case GL_CULL_FACE:
    case GL_STENCIL_TEST:
    case GL_SCISSOR_TEST:
    case GL_DITHER:
        return JS_NewBool(ctx, glIsEnabled(pname));
    case GL_COMPRESSED_TEXTURE_FORMATS: {
        GLint formats[64];
        int n = wgl_enabled_compressed_formats(ctx, g, formats, 64);
        JSValue a = JS_NewArrayBufferCopy(ctx, (const uint8_t *)formats,
                                          (size_t)n * sizeof(GLint));
        JSValue ta = wgl_typed_array(ctx, a, JS_TYPED_ARRAY_UINT32);
        JS_FreeValue(ctx, a);
        return ta;
    }
    default: {
        GLint v[32] = { 0 };
        glGetIntegerv(pname, v);
        GLint cap = wgl_param_cap(pname);
        if (cap && v[0] > cap) v[0] = cap;
        return JS_NewInt32(ctx, v[0]);
    }
    }
}

static JSValue
wgl_getContextAttributes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_webgl *g = wgl_brand(ctx, this_val);
    if (!g) return JS_EXCEPTION;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "alpha", JS_NewBool(ctx, g->alpha));
    JS_SetPropertyStr(ctx, o, "antialias", JS_NewBool(ctx, g->antialias));
    JS_SetPropertyStr(ctx, o, "depth", JS_NewBool(ctx, g->depth));
    JS_SetPropertyStr(ctx, o, "desynchronized", JS_FALSE);
    JS_SetPropertyStr(ctx, o, "failIfMajorPerformanceCaveat", JS_FALSE);
    JS_SetPropertyStr(ctx, o, "powerPreference", JS_NewString(ctx, "default"));
    JS_SetPropertyStr(ctx, o, "premultipliedAlpha",
                      JS_NewBool(ctx, g->premultiplied_alpha));
    JS_SetPropertyStr(ctx, o, "preserveDrawingBuffer", JS_NewBool(ctx, g->preserve));
    JS_SetPropertyStr(ctx, o, "stencil", JS_NewBool(ctx, g->stencil));
    JS_SetPropertyStr(ctx, o, "xrCompatible", JS_FALSE);
    return o;
}

static JSValue
wgl_isContextLost(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_webgl *g = wgl_brand(ctx, this_val);
    if (!g) return JS_EXCEPTION;
    return JS_NewBool(ctx, g->lost);
}

static void set_const(JSContext *ctx, JSValueConst obj, const char *name,
                      int value);

static JSValue wgl_drawArrays(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue wgl_drawElements(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue wgl_drawArraysInstanced(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue wgl_drawElementsInstanced(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue wgl_vertexAttribDivisor(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue wgl_createVertexArray(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue wgl_deleteVertexArray(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue wgl_isVertexArray(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue wgl_bindVertexArray(JSContext *, JSValueConst, int, JSValueConst *);

static gboolean
wgl_has_any_ext(const char *a, const char *b)
{
    return epoxy_is_desktop_gl() || epoxy_has_gl_extension(a) ||
           (b && epoxy_has_gl_extension(b));
}

static gboolean wgl_ext_always(void) { return TRUE; }
static gboolean wgl_ext_float_rt(void)
{ return wgl_has_any_ext("GL_EXT_color_buffer_float", NULL); }
static gboolean wgl_ext_half_float_rt(void)
{ return wgl_has_any_ext("GL_EXT_color_buffer_half_float", "GL_EXT_color_buffer_float"); }
static gboolean wgl_ext_float_linear(void)
{ return wgl_has_any_ext("GL_OES_texture_float_linear", NULL); }
static gboolean wgl_ext_float_blend(void)
{ return wgl_has_any_ext("GL_EXT_float_blend", NULL); }
static gboolean wgl_ext_derivatives(void)
{ return wgl_has_any_ext("GL_OES_standard_derivatives", NULL); }
static gboolean wgl_ext_s3tc(void)
{
    return epoxy_has_gl_extension("GL_EXT_texture_compression_s3tc") ||
           epoxy_has_gl_extension("GL_ANGLE_texture_compression_dxt5");
}
static gboolean wgl_ext_s3tc_srgb(void)
{
    return wgl_ext_s3tc() &&
           (epoxy_has_gl_extension("GL_EXT_texture_compression_s3tc_srgb") ||
            (epoxy_is_desktop_gl() && epoxy_has_gl_extension("GL_EXT_texture_sRGB")) ||
            (epoxy_is_desktop_gl() && epoxy_gl_version() >= 21));
}
static gboolean wgl_ext_rgtc(void)
{
    return (epoxy_is_desktop_gl() && epoxy_gl_version() >= 30) ||
           epoxy_has_gl_extension("GL_EXT_texture_compression_rgtc") ||
           epoxy_has_gl_extension("GL_ARB_texture_compression_rgtc");
}
static gboolean wgl_ext_bptc(void)
{
    return (epoxy_is_desktop_gl() && epoxy_gl_version() >= 42) ||
           epoxy_has_gl_extension("GL_EXT_texture_compression_bptc") ||
           epoxy_has_gl_extension("GL_ARB_texture_compression_bptc");
}
static gboolean wgl_ext_etc(void)
{
    return !epoxy_is_desktop_gl() || epoxy_gl_version() >= 43 ||
           epoxy_has_gl_extension("GL_ARB_ES3_compatibility");
}
static gboolean wgl_ext_astc(void)
{
    return epoxy_has_gl_extension("GL_KHR_texture_compression_astc_ldr");
}
static gboolean wgl_ext_vao(void)
{
#ifdef NS_HAVE_CGL
    return FALSE;
#else
    return TRUE;
#endif
}

static int32_t *
wgl_int_list(JSContext *ctx, JSValueConst list, int64_t offset, int64_t count)
{
    if (offset < 0 || count < 0 || count > (1 << 24)) return NULL;
    int32_t *out = g_try_new0(int32_t, count ? (gsize)count : 1);
    if (!out) return NULL;
    size_t len = 0;
    JSValue hold;
    const uint8_t *bytes = view_bytes(ctx, list, &len, &hold);
    if (bytes) {
        gboolean ok = (uint64_t)offset <= len / 4u &&
                      (uint64_t)count <= len / 4u - (uint64_t)offset;
        if (ok) memcpy(out, bytes + offset * 4, (size_t)count * 4u);
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        if (!ok) { g_free(out); return NULL; }
        return out;
    }
    if (!JS_IsArray(list)) { g_free(out); return NULL; }
    for (int64_t i = 0; i < count; i++) {
        JSValue v = JS_GetPropertyInt64(ctx, list, offset + i);
        if (JS_IsUndefined(v) || JS_ToInt32(ctx, &out[i], v) < 0) {
            JS_FreeValue(ctx, v);
            g_free(out);
            return NULL;
        }
        JS_FreeValue(ctx, v);
    }
    return out;
}

static GLint
wgl_draw_id_location(void)
{
    GLint prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    return prog ? glGetUniformLocation((GLuint)prog, "ns_webgl_DrawID") : -1;
}

static gboolean wgl_elements_in_range(ns_webgl *g, GLsizei count, GLenum type,
                                      GLintptr offset);
static int wgl_index_bytes(GLenum type);
static GByteArray *wgl_elem_shadow_load(ns_webgl *g, GLuint name);
static gboolean wgl_transform_feedback_active(ns_webgl *g);

static JSValue
wgl_multi_draw(JSContext *ctx, JSValueConst gl_obj, int argc, JSValueConst *argv,
               gboolean elements, gboolean instanced)
{
    ns_webgl *g = wgl_cur(ctx, gl_obj);
    if (!g) return wgl_no_context(ctx, gl_obj);
    int list_a = 1, list_b = elements ? 4 : 3, list_c = elements ? 6 : 5;
    int count_at = instanced ? (elements ? 8 : 7) : (elements ? 6 : 5);
    if (argc <= count_at) return JS_UNDEFINED;
    int32_t drawcount = 0;
    if (JS_ToInt32(ctx, &drawcount, argv[count_at]) < 0 || drawcount <= 0)
        return JS_UNDEFINED;
    GLenum mode = (GLenum)argi(ctx, argc, argv, 0);
    GLenum type = elements ? (GLenum)argi(ctx, argc, argv, 3) : 0;
    int64_t off_a = 0, off_b = 0, off_c = 0;
    JS_ToInt64(ctx, &off_a, argv[list_a + 1]);
    JS_ToInt64(ctx, &off_b, argv[list_b + 1]);
    if (instanced) JS_ToInt64(ctx, &off_c, argv[list_c + 1]);
    int32_t *a = wgl_int_list(ctx, argv[list_a], off_a, drawcount);
    int32_t *b = wgl_int_list(ctx, argv[list_b], off_b, drawcount);
    int32_t *c = instanced ? wgl_int_list(ctx, argv[list_c], off_c, drawcount) : NULL;
    if (!a || !b || (instanced && !c)) {
        g_free(a); g_free(b); g_free(c);
        g->injected_error = GL_INVALID_OPERATION;
        return JS_UNDEFINED;
    }
    int64_t last = -1, inst_max = 1;
    gboolean ok = TRUE;
    GByteArray *shadow = NULL;
    if (elements) {
        GLuint ebuf = wgl_bound_buffer(g, GL_ELEMENT_ARRAY_BUFFER);
        shadow = wgl_elem_shadow_get(g, ebuf);
        if (!shadow || wgl_transform_feedback_active(g))
            shadow = wgl_elem_shadow_load(g, ebuf);
    }
    for (int32_t i = 0; i < drawcount && ok; i++) {
        int64_t inst = instanced ? c[i] : 1;
        if (inst < 0) { ok = FALSE; break; }
        if (inst > inst_max) inst_max = inst;
        if (elements) {
            if (!wgl_elements_in_range(g, a[i], type, b[i])) { ok = FALSE; break; }
            if (a[i] == 0 || inst == 0) continue;
            uint64_t mx = 0;
            if (!shadow || !wgl_elem_max_index(shadow->data, shadow->len, b[i],
                                               a[i], wgl_index_bytes(type),
                                               g->version, &mx)) {
                ok = FALSE;
                break;
            }
            if ((int64_t)mx > last) last = (int64_t)mx;
        } else {
            if (a[i] < 0 || b[i] < 0) { ok = FALSE; break; }
            if (b[i] > 0 && inst > 0 && (int64_t)a[i] + b[i] - 1 > last)
                last = (int64_t)a[i] + b[i] - 1;
        }
    }
    if (ok && last >= 0 && !wgl_attribs_cover(g, last, inst_max)) ok = FALSE;
    if (ok) {
        GLint draw_id = wgl_draw_id_location();
        for (int32_t i = 0; i < drawcount; i++) {
            if (draw_id >= 0) glUniform1i(draw_id, i);
            if (elements && instanced)
                glDrawElementsInstanced(mode, a[i], type,
                                        (const void *)(intptr_t)b[i], c[i]);
            else if (elements)
                glDrawElements(mode, a[i], type, (const void *)(intptr_t)b[i]);
            else if (instanced)
                glDrawArraysInstanced(mode, a[i], b[i], c[i]);
            else
                glDrawArrays(mode, a[i], b[i]);
        }
        if (draw_id >= 0) glUniform1i(draw_id, 0);
        wgl_mark_dirty(g);
    } else {
        g->injected_error = GL_INVALID_OPERATION;
    }
    g_free(a);
    g_free(b);
    g_free(c);
    return JS_UNDEFINED;
}

static JSValue
wgl_multiDrawArrays(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return wgl_multi_draw(ctx, this_val, argc, argv, FALSE, FALSE);
}

static JSValue
wgl_multiDrawElements(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return wgl_multi_draw(ctx, this_val, argc, argv, TRUE, FALSE);
}

static JSValue
wgl_multiDrawArraysInstanced(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return wgl_multi_draw(ctx, this_val, argc, argv, FALSE, TRUE);
}

static JSValue
wgl_multiDrawElementsInstanced(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return wgl_multi_draw(ctx, this_val, argc, argv, TRUE, TRUE);
}

static JSValue
wgl_ext_trampoline(JSContext *ctx, JSValueConst this_val, int argc,
                   JSValueConst *argv, int magic, JSValueConst *data)
{
    (void)this_val;
    static JSCFunction *const fns[] = {
        wgl_drawArraysInstanced, wgl_drawElementsInstanced,
        wgl_vertexAttribDivisor, wgl_createVertexArray, wgl_deleteVertexArray,
        wgl_isVertexArray, wgl_bindVertexArray,
        wgl_multiDrawArrays, wgl_multiDrawElements,
        wgl_multiDrawArraysInstanced, wgl_multiDrawElementsInstanced,
    };
    if (magic < 0 || magic >= (int)G_N_ELEMENTS(fns)) return JS_UNDEFINED;
    return fns[magic](ctx, data[0], argc, argv);
}

static void
wgl_dispatch_context_event(JSContext *ctx, ns_webgl *g, const char *type)
{
    if (!JS_IsObject(g->canvas_obj)) return;
    JSValue ev = ns_js_realm_event(ctx, g->canvas_obj, type);
    if (!JS_IsException(ev)) {
        JSValue dispatch = JS_GetPropertyStr(ctx, g->canvas_obj, "dispatchEvent");
        JS_FreeValue(ctx, JS_Call(ctx, dispatch, g->canvas_obj, 1,
                                  (JSValueConst *)&ev));
        JS_FreeValue(ctx, dispatch);
    }
    if (JS_IsException(ev)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, ev);
}

static JSValue
wgl_lose_context(JSContext *ctx, JSValueConst this_val, int argc,
                 JSValueConst *argv, int magic, JSValueConst *data)
{
    (void)this_val; (void)argc; (void)argv;
    ns_webgl *g = JS_GetOpaque(data[0], ns_webgl_class_id);
    if (!g || g->lost == !magic) return JS_UNDEFINED;
    g->lost = !magic;
    wgl_dispatch_context_event(ctx, g, magic ? "webglcontextrestored"
                                             : "webglcontextlost");
    return JS_UNDEFINED;
}

typedef struct { const char *name; int value; } wgl_ext_const;
typedef struct { const char *name; int argc; int magic; } wgl_ext_method;

typedef struct {
    const char           *name;
    int                   versions;
    gboolean            (*available)(void);
    const wgl_ext_const  *consts;
    const wgl_ext_method *methods;
} wgl_extension;

static const wgl_ext_const wgl_c_debug_info[] = {
    { "UNMASKED_VENDOR_WEBGL", NS_UNMASKED_VENDOR_WEBGL },
    { "UNMASKED_RENDERER_WEBGL", NS_UNMASKED_RENDERER_WEBGL }, { NULL, 0 } };
static const wgl_ext_const wgl_c_aniso[] = {
    { "MAX_TEXTURE_MAX_ANISOTROPY_EXT", NS_MAX_TEXTURE_MAX_ANISOTROPY_EXT },
    { "TEXTURE_MAX_ANISOTROPY_EXT", NS_TEXTURE_MAX_ANISOTROPY_EXT }, { NULL, 0 } };
static const wgl_ext_const wgl_c_instanced[] = {
    { "VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE", 0x88FE }, { NULL, 0 } };
static const wgl_ext_method wgl_m_instanced[] = {
    { "drawArraysInstancedANGLE", 4, 0 }, { "drawElementsInstancedANGLE", 5, 1 },
    { "vertexAttribDivisorANGLE", 2, 2 }, { NULL, 0, 0 } };
static const wgl_ext_const wgl_c_vao[] = {
    { "VERTEX_ARRAY_BINDING_OES", 0x85B5 }, { NULL, 0 } };
static const wgl_ext_method wgl_m_vao[] = {
    { "createVertexArrayOES", 0, 3 }, { "deleteVertexArrayOES", 1, 4 },
    { "isVertexArrayOES", 1, 5 }, { "bindVertexArrayOES", 1, 6 }, { NULL, 0, 0 } };
static const wgl_ext_const wgl_c_derivatives[] = {
    { "FRAGMENT_SHADER_DERIVATIVE_HINT_OES", 0x8B8B }, { NULL, 0 } };
static const wgl_ext_const wgl_c_half_float[] = {
    { "HALF_FLOAT_OES", 0x8D61 }, { NULL, 0 } };
static const wgl_ext_const wgl_c_depth_texture[] = {
    { "UNSIGNED_INT_24_8_WEBGL", 0x84FA }, { NULL, 0 } };
static const wgl_ext_const wgl_c_minmax[] = {
    { "MIN_EXT", 0x8007 }, { "MAX_EXT", 0x8008 }, { NULL, 0 } };
static const wgl_ext_const wgl_c_color_float[] = {
    { "RGBA32F_EXT", 0x8814 },
    { "FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT", 0x8211 },
    { "UNSIGNED_NORMALIZED_EXT", 0x8C17 }, { NULL, 0 } };
static const wgl_ext_const wgl_c_color_half_float[] = {
    { "RGBA16F_EXT", 0x881A }, { "RGB16F_EXT", 0x881B },
    { "FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT", 0x8211 },
    { "UNSIGNED_NORMALIZED_EXT", 0x8C17 }, { NULL, 0 } };
static const wgl_ext_const wgl_c_s3tc[] = {
    { "COMPRESSED_RGB_S3TC_DXT1_EXT", 0x83F0 },
    { "COMPRESSED_RGBA_S3TC_DXT1_EXT", 0x83F1 },
    { "COMPRESSED_RGBA_S3TC_DXT3_EXT", 0x83F2 },
    { "COMPRESSED_RGBA_S3TC_DXT5_EXT", 0x83F3 }, { NULL, 0 } };
static const wgl_ext_const wgl_c_s3tc_srgb[] = {
    { "COMPRESSED_SRGB_S3TC_DXT1_EXT", 0x8C4C },
    { "COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT", 0x8C4D },
    { "COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT", 0x8C4E },
    { "COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT", 0x8C4F }, { NULL, 0 } };
static const wgl_ext_const wgl_c_rgtc[] = {
    { "COMPRESSED_RED_RGTC1_EXT", 0x8DBB },
    { "COMPRESSED_SIGNED_RED_RGTC1_EXT", 0x8DBC },
    { "COMPRESSED_RED_GREEN_RGTC2_EXT", 0x8DBD },
    { "COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT", 0x8DBE }, { NULL, 0 } };
static const wgl_ext_const wgl_c_bptc[] = {
    { "COMPRESSED_RGBA_BPTC_UNORM_EXT", 0x8E8C },
    { "COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT", 0x8E8D },
    { "COMPRESSED_RGB_BPTC_SIGNED_FLOAT_EXT", 0x8E8E },
    { "COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT", 0x8E8F }, { NULL, 0 } };
static const wgl_ext_const wgl_c_etc[] = {
    { "COMPRESSED_R11_EAC", 0x9270 }, { "COMPRESSED_SIGNED_R11_EAC", 0x9271 },
    { "COMPRESSED_RG11_EAC", 0x9272 }, { "COMPRESSED_SIGNED_RG11_EAC", 0x9273 },
    { "COMPRESSED_RGB8_ETC2", 0x9274 }, { "COMPRESSED_SRGB8_ETC2", 0x9275 },
    { "COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2", 0x9276 },
    { "COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2", 0x9277 },
    { "COMPRESSED_RGBA8_ETC2_EAC", 0x9278 },
    { "COMPRESSED_SRGB8_ALPHA8_ETC2_EAC", 0x9279 }, { NULL, 0 } };
static const wgl_ext_const wgl_c_astc[] = {
    { "COMPRESSED_RGBA_ASTC_4x4_KHR", 0x93B0 }, { "COMPRESSED_RGBA_ASTC_5x4_KHR", 0x93B1 },
    { "COMPRESSED_RGBA_ASTC_5x5_KHR", 0x93B2 }, { "COMPRESSED_RGBA_ASTC_6x5_KHR", 0x93B3 },
    { "COMPRESSED_RGBA_ASTC_6x6_KHR", 0x93B4 }, { "COMPRESSED_RGBA_ASTC_8x5_KHR", 0x93B5 },
    { "COMPRESSED_RGBA_ASTC_8x6_KHR", 0x93B6 }, { "COMPRESSED_RGBA_ASTC_8x8_KHR", 0x93B7 },
    { "COMPRESSED_RGBA_ASTC_10x5_KHR", 0x93B8 }, { "COMPRESSED_RGBA_ASTC_10x6_KHR", 0x93B9 },
    { "COMPRESSED_RGBA_ASTC_10x8_KHR", 0x93BA }, { "COMPRESSED_RGBA_ASTC_10x10_KHR", 0x93BB },
    { "COMPRESSED_RGBA_ASTC_12x10_KHR", 0x93BC }, { "COMPRESSED_RGBA_ASTC_12x12_KHR", 0x93BD },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR", 0x93D0 },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR", 0x93D1 },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR", 0x93D2 },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR", 0x93D3 },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR", 0x93D4 },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR", 0x93D5 },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR", 0x93D6 },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR", 0x93D7 },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR", 0x93D8 },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR", 0x93D9 },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR", 0x93DA },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR", 0x93DB },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR", 0x93DC },
    { "COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR", 0x93DD }, { NULL, 0 } };
static const wgl_ext_method wgl_m_multi_draw[] = {
    { "multiDrawArraysWEBGL", 6, 7 }, { "multiDrawElementsWEBGL", 7, 8 },
    { "multiDrawArraysInstancedWEBGL", 8, 9 },
    { "multiDrawElementsInstancedWEBGL", 9, 10 }, { NULL, 0, 0 } };
static const wgl_ext_const wgl_c_parallel[] = {
    { "COMPLETION_STATUS_KHR", 0x91B1 }, { NULL, 0 } };

#define WGL_V1 1
#define WGL_V2 2

static const wgl_extension wgl_extensions[] = {
    { "ANGLE_instanced_arrays", WGL_V1, wgl_ext_always, wgl_c_instanced, wgl_m_instanced },
    { "EXT_blend_minmax", WGL_V1, wgl_ext_always, wgl_c_minmax, NULL },
    { "EXT_color_buffer_float", WGL_V2, wgl_ext_float_rt, NULL, NULL },
    { "EXT_color_buffer_half_float", WGL_V1 | WGL_V2, wgl_ext_half_float_rt,
      wgl_c_color_half_float, NULL },
    { "EXT_float_blend", WGL_V1 | WGL_V2, wgl_ext_float_blend, NULL, NULL },
    { "EXT_texture_filter_anisotropic", WGL_V1 | WGL_V2, wgl_ext_always, wgl_c_aniso, NULL },
    { "KHR_parallel_shader_compile", WGL_V1 | WGL_V2, wgl_ext_always, wgl_c_parallel, NULL },
    { "OES_element_index_uint", WGL_V1, wgl_ext_always, NULL, NULL },
    { "OES_standard_derivatives", WGL_V1, wgl_ext_derivatives, wgl_c_derivatives, NULL },
    { "OES_texture_float", WGL_V1, wgl_ext_always, NULL, NULL },
    { "OES_texture_float_linear", WGL_V1 | WGL_V2, wgl_ext_float_linear, NULL, NULL },
    { "OES_texture_half_float", WGL_V1, wgl_ext_always, wgl_c_half_float, NULL },
    { "OES_texture_half_float_linear", WGL_V1, wgl_ext_always, NULL, NULL },
    { "OES_vertex_array_object", WGL_V1, wgl_ext_vao, wgl_c_vao, wgl_m_vao },
    { "WEBGL_color_buffer_float", WGL_V1, wgl_ext_float_rt, wgl_c_color_float, NULL },
    { "WEBGL_debug_renderer_info", WGL_V1 | WGL_V2, wgl_ext_always, wgl_c_debug_info, NULL },
    { "WEBGL_depth_texture", WGL_V1, wgl_ext_always, wgl_c_depth_texture, NULL },
    { "WEBGL_lose_context", WGL_V1 | WGL_V2, wgl_ext_always, NULL, NULL },
    { "WEBGL_multi_draw", WGL_V1 | WGL_V2, wgl_ext_always, NULL, wgl_m_multi_draw },
    { "WEBGL_compressed_texture_s3tc", WGL_V1 | WGL_V2, wgl_ext_s3tc, wgl_c_s3tc, NULL },
    { "WEBGL_compressed_texture_s3tc_srgb", WGL_V1 | WGL_V2, wgl_ext_s3tc_srgb,
      wgl_c_s3tc_srgb, NULL },
    { "EXT_texture_compression_rgtc", WGL_V1 | WGL_V2, wgl_ext_rgtc, wgl_c_rgtc, NULL },
    { "EXT_texture_compression_bptc", WGL_V1 | WGL_V2, wgl_ext_bptc, wgl_c_bptc, NULL },
    { "WEBGL_compressed_texture_etc", WGL_V1 | WGL_V2, wgl_ext_etc, wgl_c_etc, NULL },
    { "WEBGL_compressed_texture_astc", WGL_V1 | WGL_V2, wgl_ext_astc, wgl_c_astc, NULL },
};

static gboolean
wgl_extension_enabled(const ns_webgl *g, const wgl_extension *e)
{
    return (e->versions & (g->version >= 2 ? WGL_V2 : WGL_V1)) && e->available();
}

static JSValue
wgl_getSupportedExtensions(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    if (!wgl_brand(ctx, this_val)) return JS_EXCEPTION;
    ns_webgl *g = wgl_cur(ctx, this_val);
    if (!g) return JS_NULL;
    JSValue arr = JS_NewArray(ctx);
    uint32_t n = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(wgl_extensions); i++)
        if (wgl_extension_enabled(g, &wgl_extensions[i]))
            JS_SetPropertyUint32(ctx, arr, n++,
                                 JS_NewString(ctx, wgl_extensions[i].name));
    return arr;
}

static int
wgl_enabled_compressed_formats(JSContext *ctx, ns_webgl *g, GLint *out, int cap)
{
    int n = 0;
    if (!JS_IsObject(g->extensions)) return 0;
    for (size_t i = 0; i < G_N_ELEMENTS(wgl_extensions); i++) {
        const wgl_extension *e = &wgl_extensions[i];
        if (!strstr(e->name, "compress")) continue;
        JSValue v = JS_GetPropertyStr(ctx, g->extensions, e->name);
        gboolean on = JS_IsObject(v);
        JS_FreeValue(ctx, v);
        for (const wgl_ext_const *c = e->consts; on && c && c->name && n < cap; c++)
            out[n++] = c->value;
    }
    return n;
}

static JSValue
wgl_astc_profiles(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, "ldr"));
    return arr;
}

static JSValue
wgl_make_extension(JSContext *ctx, JSValueConst gl, const wgl_extension *e)
{
    JSValue ext = JS_NewObject(ctx);
    for (const wgl_ext_const *c = e->consts; c && c->name; c++)
        set_const(ctx, ext, c->name, c->value);
    for (const wgl_ext_method *m = e->methods; m && m->name; m++)
        JS_SetPropertyStr(ctx, ext, m->name,
                          JS_NewCFunctionData(ctx, wgl_ext_trampoline, m->argc,
                                              m->magic, 1, &gl));
    if (strcmp(e->name, "WEBGL_compressed_texture_astc") == 0)
        JS_SetPropertyStr(ctx, ext, "getSupportedProfiles",
                          JS_NewCFunction(ctx, wgl_astc_profiles,
                                          "getSupportedProfiles", 0));
    if (strcmp(e->name, "WEBGL_lose_context") == 0) {
        JS_SetPropertyStr(ctx, ext, "loseContext",
                          JS_NewCFunctionData(ctx, wgl_lose_context, 0, 0, 1, &gl));
        JS_SetPropertyStr(ctx, ext, "restoreContext",
                          JS_NewCFunctionData(ctx, wgl_lose_context, 0, 1, 1, &gl));
    }
    return ext;
}

static JSValue
wgl_getExtension(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (!wgl_brand(ctx, this_val)) return JS_EXCEPTION;
    if (argc < 1) return JS_NULL;
    ns_webgl *g = wgl_cur(ctx, this_val);
    if (!g) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    const wgl_extension *e = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(wgl_extensions) && !e; i++)
        if (g_ascii_strcasecmp(name, wgl_extensions[i].name) == 0 &&
            wgl_extension_enabled(g, &wgl_extensions[i]))
            e = &wgl_extensions[i];
    JS_FreeCString(ctx, name);
    if (!e) return JS_NULL;
    if (!JS_IsObject(g->extensions)) g->extensions = JS_NewObject(ctx);
    JSValue ext = JS_GetPropertyStr(ctx, g->extensions, e->name);
    if (JS_IsObject(ext)) return ext;
    JS_FreeValue(ctx, ext);
    ext = wgl_make_extension(ctx, this_val, e);
    JS_SetPropertyStr(ctx, g->extensions, e->name, JS_DupValue(ctx, ext));
    return ext;
}

static JSValue
wgl_activeTexture(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glActiveTexture((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_createShader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint s = glCreateShader((GLenum)argi(ctx, argc, argv, 0));
    return wgl_wrap(g, ctx, s, "shader");
}

static JSValue
wgl_deleteShader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDeleteShader((GLuint)wgl_name(ctx, argv[0]));
    return JS_UNDEFINED;
}

static char *
wgl_rewrite_draw_id(const char *src)
{
    if (!strstr(src, "GL_ANGLE_multi_draw") && !strstr(src, "gl_DrawID"))
        return NULL;
    GRegex *ext = g_regex_new("^[ \\t]*#[ \\t]*extension[ \\t]+GL_ANGLE_multi_draw[^\\n]*$",
                              G_REGEX_MULTILINE, 0, NULL);
    GRegex *ifdef = g_regex_new("#[ \\t]*ifdef[ \\t]+GL_ANGLE_multi_draw\\b", 0, 0, NULL);
    GRegex *ifndef = g_regex_new("#[ \\t]*ifndef[ \\t]+GL_ANGLE_multi_draw\\b", 0, 0, NULL);
    GRegex *defined = g_regex_new("defined[ \\t]*\\([ \\t]*GL_ANGLE_multi_draw[ \\t]*\\)|"
                                  "defined[ \\t]+GL_ANGLE_multi_draw\\b", 0, 0, NULL);
    GRegex *draw_id = g_regex_new("\\bgl_DrawID\\b", 0, 0, NULL);
    char *a = g_regex_replace_literal(ext, src, -1, 0, "", 0, NULL);
    char *b = g_regex_replace_literal(ifdef, a, -1, 0, "#if 1", 0, NULL);
    char *c = g_regex_replace_literal(ifndef, b, -1, 0, "#if 0", 0, NULL);
    char *d = g_regex_replace_literal(defined, c, -1, 0, "1", 0, NULL);
    gboolean uses = g_regex_match(draw_id, d, 0, NULL);
    char *e = g_regex_replace_literal(draw_id, d, -1, 0, "ns_webgl_DrawID", 0, NULL);
    g_regex_unref(ext);
    g_regex_unref(ifdef);
    g_regex_unref(ifndef);
    g_regex_unref(defined);
    g_regex_unref(draw_id);
    g_free(a);
    g_free(b);
    g_free(c);
    g_free(d);
    if (!uses) return e;
    const char *at = e;
    for (const char *line = e; *line; ) {
        const char *q = line;
        while (*q == ' ' || *q == '\t' || *q == '\r') q++;
        const char *nl = strchr(line, '\n');
        if (*q != '#' && *q != '\n' && *q != '\0' &&
            !(q[0] == '/' && q[1] == '/')) {
            at = line;
            break;
        }
        if (!nl) { at = line + strlen(line); break; }
        line = nl + 1;
        at = line;
    }
    GString *out = g_string_new_len(e, at - e);
    g_string_append(out, "uniform highp int ns_webgl_DrawID;\n");
    g_string_append(out, at);
    g_free(e);
    return g_string_free(out, FALSE);
}

static JSValue
wgl_shaderSource(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    if (argc < 2) return JS_UNDEFINED;
    size_t slen = 0;
    const char *src = JS_ToCStringLen(ctx, &slen, argv[1]);
    if (src) {
        if (slen <= NS_WEBGL_MAX_SHADER) {
            char *rewritten = wgl_rewrite_draw_id(src);
            const GLchar *p = rewritten ? rewritten : src;
            GLint plen = rewritten ? (GLint)strlen(rewritten) : (GLint)slen;
            char *prefixed = NULL;
            if (epoxy_is_desktop_gl() && !strstr(p, "#version")) {
                prefixed = g_strconcat("#version 100\n", p, NULL);
                p = prefixed;
                plen = (GLint)strlen(prefixed);
            }
            glShaderSource((GLuint)wgl_name(ctx, argv[0]), 1, &p, &plen);
            g_free(prefixed);
            g_free(rewritten);
        }
        JS_FreeCString(ctx, src);
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_compileShader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glCompileShader((GLuint)wgl_name(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue
wgl_getShaderParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 1);
    GLint v = 0;
    if (pname == 0x91B1) return JS_TRUE;
    glGetShaderiv((GLuint)wgl_name(ctx, argv[0]), pname, &v);
    if (pname == GL_COMPILE_STATUS || pname == GL_DELETE_STATUS)
        return JS_NewBool(ctx, v);
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_gl_log(JSContext *ctx, GLuint name,
           void (*get_iv)(GLuint, GLenum, GLint *), GLenum len_pname,
           void (*get_str)(GLuint, GLsizei, GLsizei *, GLchar *))
{
    GLint len = 0;
    get_iv(name, len_pname, &len);
    if (len <= 0) return JS_NewString(ctx, "");
    char *buf = g_malloc((size_t)len + 1);
    get_str(name, len, NULL, buf);
    buf[len] = 0;
    JSValue r = JS_NewString(ctx, buf);
    g_free(buf);
    return r;
}

static JSValue
wgl_getShaderInfoLog(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    return wgl_gl_log(ctx, (GLuint)wgl_name(ctx, argv[0]),
                      glGetShaderiv, GL_INFO_LOG_LENGTH, glGetShaderInfoLog);
}

static JSValue
wgl_getShaderSource(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    return wgl_gl_log(ctx, (GLuint)wgl_name(ctx, argv[0]),
                      glGetShaderiv, GL_SHADER_SOURCE_LENGTH, glGetShaderSource);
}

static JSValue
wgl_createProgram(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    return wgl_wrap(g, ctx, glCreateProgram(), "program");
}

static JSValue
wgl_deleteProgram(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    wgl_forget_program(g, (GLuint)wgl_name(ctx, argv[0]));
    glDeleteProgram((GLuint)wgl_name(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue
wgl_attachShader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glAttachShader((GLuint)wgl_name(ctx, argv[0]), (GLuint)wgl_name(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
wgl_detachShader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDetachShader((GLuint)wgl_name(ctx, argv[0]), (GLuint)wgl_name(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
wgl_linkProgram(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint prog = (GLuint)wgl_name(ctx, argv[0]);
    glLinkProgram(prog);
    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked)
        wgl_forget_program(g, prog);
    else
        wgl_check_all_attribs_for(g, prog);
    return JS_UNDEFINED;
}

static JSValue
wgl_validateProgram(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glValidateProgram((GLuint)wgl_name(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue
wgl_useProgram(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glUseProgram((GLuint)wgl_name(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue
wgl_getProgramParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 1);
    GLint v = 0;
    if (pname == 0x91B1) return JS_TRUE;
    glGetProgramiv((GLuint)wgl_name(ctx, argv[0]), pname, &v);
    if (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS ||
        pname == GL_DELETE_STATUS)
        return JS_NewBool(ctx, v);
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_getProgramInfoLog(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    return wgl_gl_log(ctx, (GLuint)wgl_name(ctx, argv[0]),
                      glGetProgramiv, GL_INFO_LOG_LENGTH, glGetProgramInfoLog);
}

static JSValue
wgl_bindAttribLocation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    const char *name = argc >= 3 ? JS_ToCString(ctx, argv[2]) : NULL;
    if (name) {
        glBindAttribLocation((GLuint)wgl_name(ctx, argv[0]),
                             (GLuint)argi(ctx, argc, argv, 1), name);
        JS_FreeCString(ctx, name);
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_getAttribLocation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    const char *name = argc >= 2 ? JS_ToCString(ctx, argv[1]) : NULL;
    GLint loc = -1;
    if (name) {
        loc = glGetAttribLocation((GLuint)wgl_name(ctx, argv[0]), name);
        JS_FreeCString(ctx, name);
    }
    return JS_NewInt32(ctx, loc);
}

static JSValue
wgl_getUniformLocation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    const char *name = argc >= 2 ? JS_ToCString(ctx, argv[1]) : NULL;
    GLint loc = -1;
    if (name) {
        loc = glGetUniformLocation((GLuint)wgl_name(ctx, argv[0]), name);
        JS_FreeCString(ctx, name);
    }
    if (loc < 0) return JS_NULL;
    return wgl_new_object(g, ctx, (GLuint)loc, "location");
}

static JSValue
wgl_new_info(ns_webgl *g, JSContext *ctx, int kind, const char *iface)
{
    JSContext *realm = ns_canvas_realm(ctx, g->canvas);
    JSValue proto = ns_api_proto(realm, iface);
    JSValue o = ns_hidden_new(realm, kind, proto);
    JS_FreeValue(realm, proto);
    return o;
}

static JSValue
wgl_active_info(ns_webgl *g, JSContext *ctx, GLint size, GLenum type, const char *name)
{
    JSValue o = wgl_new_info(g, ctx, NS_HK_ACTIVEINFO, "WebGLActiveInfo");
    ns_hset(ctx, o, "size", JS_NewInt32(ctx, size));
    ns_hset(ctx, o, "type", JS_NewInt32(ctx, (int)type));
    ns_hset(ctx, o, "name", JS_NewString(ctx, name));
    return o;
}

static JSValue
wgl_active_var(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
               void (*fn)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *))
{
    WGL_GET(0);
    char name[256] = { 0 };
    GLint size = 0;
    GLenum type = 0;
    fn((GLuint)wgl_name(ctx, argv[0]), (GLuint)argi(ctx, argc, argv, 1),
       sizeof(name) - 1, NULL, &size, &type, name);
    return wgl_active_info(g, ctx, size, type, name);
}

static JSValue wgl_getActiveAttrib(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_active_var(c, t, a, v, glGetActiveAttrib); }
static JSValue wgl_getActiveUniform(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_active_var(c, t, a, v, glGetActiveUniform); }

static JSValue
wgl_getShaderPrecisionFormat(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum shader_type = (GLenum)argi(ctx, argc, argv, 0);
    GLenum precision_type = (GLenum)argi(ctx, argc, argv, 1);
    GLint range[2] = { 0, 0 };
    GLint precision = 0;
    glGetShaderPrecisionFormat(shader_type, precision_type, range, &precision);
    JSValue o = wgl_new_info(g, ctx, NS_HK_PRECISION, "WebGLShaderPrecisionFormat");
    ns_hset(ctx, o, "rangeMin", JS_NewInt32(ctx, range[0]));
    ns_hset(ctx, o, "rangeMax", JS_NewInt32(ctx, range[1]));
    ns_hset(ctx, o, "precision", JS_NewInt32(ctx, precision));
    return o;
}

static JSValue
wgl_gen_obj(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
            const char *kind, void (*gen)(GLsizei, GLuint *))
{
    WGL_GET(0);
    GLuint n = 0;
    gen(1, &n);
    return wgl_wrap(g, ctx, n, kind);
}

static JSValue
wgl_del_obj(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
            void (*del)(GLsizei, const GLuint *))
{
    WGL_GET(0);
    GLuint n = (GLuint)wgl_name(ctx, argv[0]);
    del(1, &n);
    return JS_UNDEFINED;
}

static JSValue wgl_createBuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "buffer", glGenBuffers); }
static JSValue
wgl_deleteBuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint n = argc >= 1 ? (GLuint)wgl_name(ctx, argv[0]) : 0;
    if (n) {
        if (g->bound_array_buffer == n) g->bound_array_buffer = 0;
        if (g->bound_element_array_buffer == n) g->bound_element_array_buffer = 0;
        if (g->bound_buffers) {
            GHashTableIter it;
            gpointer key, value;
            g_hash_table_iter_init(&it, g->bound_buffers);
            while (g_hash_table_iter_next(&it, &key, &value)) {
                if (GPOINTER_TO_UINT(value) == n)
                    g_hash_table_iter_remove(&it);
            }
        }
        if (g->buffer_sizes)
            g_hash_table_remove(g->buffer_sizes, GUINT_TO_POINTER(n));
        wgl_elem_shadow_clear(g, n);
        glDeleteBuffers(1, &n);
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_bindBuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLuint n = (GLuint)wgl_name(ctx, argv[1]);
    glBindBuffer(target, n);
    wgl_set_bound_buffer(g, target, n);
    return JS_UNDEFINED;
}

static gboolean
wgl_buffer_range_ok(ns_webgl *g, GLenum target, GLintptr offset, size_t len)
{
    if (offset < 0) return FALSE;
    size_t size = wgl_buffer_size(g, wgl_bound_buffer(g, target));
    if (size == 0) return FALSE;
    uint64_t end;
    if (__builtin_add_overflow((uint64_t)offset, (uint64_t)len, &end))
        return FALSE;
    return end <= size;
}

static JSValue
wgl_bufferData(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLenum usage = (GLenum)argi(ctx, argc, argv, 2);
    GLuint bn = wgl_bound_buffer(g, target);
    gboolean is_elem = target == GL_ELEMENT_ARRAY_BUFFER;
    wgl_elem_shadow_clear(g, bn);
    if (argc >= 2 && JS_IsNumber(argv[1])) {
        int64_t size = 0;
        JS_ToInt64(ctx, &size, argv[1]);
        if (size < 0 || (uint64_t)size > NS_WEBGL_MAX_ALLOC) return JS_UNDEFINED;
        if (is_elem) {
            uint8_t *z = size ? g_malloc0((size_t)size) : NULL;
            glBufferData(target, (GLsizeiptr)size, z, usage);
            wgl_elem_shadow_set(g, bn, NULL, (size_t)size);
            g_free(z);
        } else {
            glBufferData(target, (GLsizeiptr)size, NULL, usage);
        }
        wgl_set_buffer_size(g, bn, (size_t)size);
        return JS_UNDEFINED;
    }
    JSValue hold;
    size_t len = 0;
    const uint8_t *p = (argc >= 2) ? view_bytes(ctx, argv[1], &len, &hold) : NULL;
    if (len > NS_WEBGL_MAX_ALLOC) {
        if (p && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return JS_UNDEFINED;
    }
    glBufferData(target, (GLsizeiptr)len, p, usage);
    wgl_set_buffer_size(g, bn, len);
    if (is_elem) wgl_elem_shadow_set(g, bn, p, len);
    if (p && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    return JS_UNDEFINED;
}

static JSValue
wgl_bufferSubData(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 1);
    JSValue hold;
    size_t len = 0;
    const uint8_t *p = (argc >= 3) ? view_bytes(ctx, argv[2], &len, &hold) : NULL;
    if (p) {
        if (wgl_buffer_range_ok(g, target, offset, len)) {
            glBufferSubData(target, offset, (GLsizeiptr)len, p);
            GLuint bn = wgl_bound_buffer(g, target);
            if (target == GL_ELEMENT_ARRAY_BUFFER)
                wgl_elem_shadow_patch(g, bn, (size_t)offset, p, len);
            else
                wgl_elem_shadow_clear(g, bn);
        }
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_enableVertexAttribArray(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 0);
    glEnableVertexAttribArray(index);
    return JS_UNDEFINED;
}

static JSValue
wgl_disableVertexAttribArray(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 0);
    glDisableVertexAttribArray(index);
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribPointer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 0);
    GLint size = argi(ctx, argc, argv, 1);
    GLenum type = (GLenum)argi(ctx, argc, argv, 2);
    GLboolean norm = argbool(ctx, argc, argv, 3);
    GLsizei stride = argi(ctx, argc, argv, 4);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 5);
    if (size < 0 || stride < 0 || offset < 0) return JS_UNDEFINED;
    glVertexAttribPointer(index, size, type, norm, stride,
                          (const void *)offset);
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttrib_f(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 0);
    float v[4] = { 0, 0, 0, 1 };
    for (int i = 0; i < n; i++) v[i] = (float)argd(ctx, argc, argv, i + 1);
    switch (n) {
    case 1: glVertexAttrib1f(index, v[0]); break;
    case 2: glVertexAttrib2f(index, v[0], v[1]); break;
    case 3: glVertexAttrib3f(index, v[0], v[1], v[2]); break;
    default: glVertexAttrib4f(index, v[0], v[1], v[2], v[3]); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_vertexAttrib1f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_f(c, t, a, v, 1); }
static JSValue wgl_vertexAttrib2f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_f(c, t, a, v, 2); }
static JSValue wgl_vertexAttrib3f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_f(c, t, a, v, 3); }
static JSValue wgl_vertexAttrib4f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_f(c, t, a, v, 4); }

static JSValue
wgl_uniform_f(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    float v[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) v[i] = (float)argd(ctx, argc, argv, i + 1);
    switch (n) {
    case 1: glUniform1f(loc, v[0]); break;
    case 2: glUniform2f(loc, v[0], v[1]); break;
    case 3: glUniform3f(loc, v[0], v[1], v[2]); break;
    default: glUniform4f(loc, v[0], v[1], v[2], v[3]); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_f(c, t, a, v, 1); }
static JSValue wgl_uniform2f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_f(c, t, a, v, 2); }
static JSValue wgl_uniform3f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_f(c, t, a, v, 3); }
static JSValue wgl_uniform4f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_f(c, t, a, v, 4); }

static JSValue
wgl_uniform_i(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    GLint v[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) v[i] = argi(ctx, argc, argv, i + 1);
    switch (n) {
    case 1: glUniform1i(loc, v[0]); break;
    case 2: glUniform2i(loc, v[0], v[1]); break;
    case 3: glUniform3i(loc, v[0], v[1], v[2]); break;
    default: glUniform4i(loc, v[0], v[1], v[2], v[3]); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1i(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_i(c, t, a, v, 1); }
static JSValue wgl_uniform2i(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_i(c, t, a, v, 2); }
static JSValue wgl_uniform3i(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_i(c, t, a, v, 3); }
static JSValue wgl_uniform4i(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_i(c, t, a, v, 4); }

static JSValue
wgl_uniform_fv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    float buf[4096];
    int cnt = (argc >= 2) ? wgl_floats(ctx, argv[1], buf, 4096) : 0;
    GLsizei count = cnt / n;
    switch (n) {
    case 1: glUniform1fv(loc, count, buf); break;
    case 2: glUniform2fv(loc, count, buf); break;
    case 3: glUniform3fv(loc, count, buf); break;
    default: glUniform4fv(loc, count, buf); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_fv(c, t, a, v, 1); }
static JSValue wgl_uniform2fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_fv(c, t, a, v, 2); }
static JSValue wgl_uniform3fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_fv(c, t, a, v, 3); }
static JSValue wgl_uniform4fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_fv(c, t, a, v, 4); }

static JSValue
wgl_uniform_iv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    GLint buf[4096];
    int cnt = (argc >= 2) ? wgl_ints(ctx, argv[1], buf, 4096) : 0;
    GLsizei count = cnt / n;
    switch (n) {
    case 1: glUniform1iv(loc, count, buf); break;
    case 2: glUniform2iv(loc, count, buf); break;
    case 3: glUniform3iv(loc, count, buf); break;
    default: glUniform4iv(loc, count, buf); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1iv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_iv(c, t, a, v, 1); }
static JSValue wgl_uniform2iv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_iv(c, t, a, v, 2); }
static JSValue wgl_uniform3iv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_iv(c, t, a, v, 3); }
static JSValue wgl_uniform4iv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_iv(c, t, a, v, 4); }

static JSValue
wgl_uniformMatrix_fv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    float buf[4096];
    int cnt = (argc >= 3) ? wgl_floats(ctx, argv[2], buf, 4096) : 0;
    GLsizei count = cnt / (n * n);
    switch (n) {
    case 2: glUniformMatrix2fv(loc, count, GL_FALSE, buf); break;
    case 3: glUniformMatrix3fv(loc, count, GL_FALSE, buf); break;
    default: glUniformMatrix4fv(loc, count, GL_FALSE, buf); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniformMatrix2fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_fv(c, t, a, v, 2); }
static JSValue wgl_uniformMatrix3fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_fv(c, t, a, v, 3); }
static JSValue wgl_uniformMatrix4fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_fv(c, t, a, v, 4); }

static int
wgl_index_bytes(GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:  return 1;
    case GL_UNSIGNED_SHORT: return 2;
    case GL_UNSIGNED_INT:   return 4;
    default:                return 0;
    }
}

static gboolean
wgl_elements_in_range(ns_webgl *g, GLsizei count, GLenum type, GLintptr offset)
{
    if (count < 0 || offset < 0) return FALSE;
    int isz = wgl_index_bytes(type);
    if (!isz) return FALSE;
    size_t size = wgl_buffer_size(g, wgl_bound_buffer(g, GL_ELEMENT_ARRAY_BUFFER));
    if (size == 0) return FALSE;
    uint64_t span;
    if (__builtin_mul_overflow((uint64_t)count, (uint64_t)isz, &span) ||
        __builtin_add_overflow(span, (uint64_t)offset, &span))
        return FALSE;
    return span <= size;
}

static gboolean
wgl_transform_feedback_active(ns_webgl *g)
{
    if (g->version < 2) return FALSE;
    GLint active = 0;
    glGetIntegerv(GL_TRANSFORM_FEEDBACK_ACTIVE, &active);
    return active != 0;
}

static GByteArray *
wgl_elem_shadow_load(ns_webgl *g, GLuint name)
{
    wgl_elem_shadow_clear(g, name);
    size_t size = wgl_buffer_size(g, name);
    if (size == 0 || g->version < 2) return NULL;
    const uint8_t *p = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0,
                                        (GLsizeiptr)size, GL_MAP_READ_BIT);
    if (!p) return NULL;
    wgl_elem_shadow_set(g, name, p, size);
    glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    return wgl_elem_shadow_get(g, name);
}

static gboolean
wgl_draw_elements_ok(ns_webgl *g, GLsizei count, GLenum type, GLintptr offset,
                     int64_t instances)
{
    if (!wgl_elements_in_range(g, count, type, offset)) return FALSE;
    if (count <= 0 || instances <= 0) return TRUE;
    GLuint ebuf = wgl_bound_buffer(g, GL_ELEMENT_ARRAY_BUFFER);
    GByteArray *sh = wgl_elem_shadow_get(g, ebuf);
    if (!sh || wgl_transform_feedback_active(g))
        sh = wgl_elem_shadow_load(g, ebuf);
    if (!sh) return FALSE;
    uint64_t mx;
    if (!wgl_elem_max_index(sh->data, sh->len, offset, count,
                            wgl_index_bytes(type), g->version, &mx))
        return FALSE;
    return wgl_attribs_cover(g, (int64_t)mx, instances);
}

static JSValue
wgl_drawArrays(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum mode = (GLenum)argi(ctx, argc, argv, 0);
    GLint first = argi(ctx, argc, argv, 1);
    GLsizei count = argi(ctx, argc, argv, 2);
    if (first < 0 || count < 0) return JS_UNDEFINED;
    if (count > 0 && !wgl_attribs_cover(g, (int64_t)first + count - 1, 1))
        return JS_UNDEFINED;
    glDrawArrays(mode, first, count);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_drawElements(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum mode = (GLenum)argi(ctx, argc, argv, 0);
    GLsizei count = argi(ctx, argc, argv, 1);
    GLenum type = (GLenum)argi(ctx, argc, argv, 2);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 3);
    if (!wgl_draw_elements_ok(g, count, type, offset, 1)) return JS_UNDEFINED;
    glDrawElements(mode, count, type, (const void *)offset);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue wgl_createTexture(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "texture", glGenTextures); }
static JSValue wgl_deleteTexture(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteTextures); }

static JSValue
wgl_bindTexture(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindTexture((GLenum)argi(ctx, argc, argv, 0), (GLuint)wgl_name(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
wgl_texParameteri(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glTexParameteri((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                    argi(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_texParameterf(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glTexParameterf((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                    (float)argd(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_generateMipmap(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glGenerateMipmap((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static const uint8_t *
wgl_imagedata_bytes(JSContext *ctx, JSValueConst src, int *w, int *h,
                    size_t *out_len, JSValue *hold)
{
    *hold = JS_UNDEFINED;
    *out_len = 0;
    JSValue wv = JS_GetPropertyStr(ctx, src, "width");
    JSValue hv = JS_GetPropertyStr(ctx, src, "height");
    JSValue dv = JS_GetPropertyStr(ctx, src, "data");
    int32_t iw = 0, ih = 0;
    JS_ToInt32(ctx, &iw, wv);
    JS_ToInt32(ctx, &ih, hv);
    JS_FreeValue(ctx, wv);
    JS_FreeValue(ctx, hv);
    *w = iw;
    *h = ih;
    if (iw <= 0 || ih <= 0 || !JS_IsObject(dv)) {
        JS_FreeValue(ctx, dv);
        return NULL;
    }
    size_t len = 0;
    const uint8_t *p = view_bytes(ctx, dv, &len, hold);
    JS_FreeValue(ctx, dv);
    *out_len = len;
    return p;
}

static uint8_t *
wgl_source_rgba(JSContext *ctx, JSValueConst src, int format,
                gboolean flip_y, gboolean premultiply, int *out_w, int *out_h,
                gboolean *threw)
{
    int w = 0, h = 0;
    cairo_surface_t *s = ns_js_drawimage_source_surface(ctx, src, &w, &h, threw);
    if (!s) return NULL;
    const unsigned char *data = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    if (w <= 0 || h <= 0 || !data) {
        cairo_surface_destroy(s);
        return NULL;
    }
    int comps = wgl_components(format);
    guint64 total = (guint64)w * (guint64)h * (guint64)comps;
    if (total == 0 || total > NS_WEBGL_MAX_ALLOC) {
        cairo_surface_destroy(s);
        return NULL;
    }
    uint8_t *out = g_try_malloc0((size_t)total);
    if (!out) {
        cairo_surface_destroy(s);
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        const unsigned char *srow = data + (size_t)(flip_y ? h - 1 - y : y) * stride;
        uint8_t *orow = out + (size_t)y * (size_t)w * (size_t)comps;
        for (int x = 0; x < w; x++) {
            const unsigned char *p = srow + x * 4;
            unsigned b = p[0], gg = p[1], r = p[2], a = p[3];
            if (!premultiply && a > 0 && a < 255) {
                r = (r * 255u + a / 2) / a;
                gg = (gg * 255u + a / 2) / a;
                b = (b * 255u + a / 2) / a;
                if (r > 255) r = 255;
                if (gg > 255) gg = 255;
                if (b > 255) b = 255;
            }
            uint8_t *o = orow + x * comps;
            switch (format) {
            case GL_RGBA: o[0] = (uint8_t)r; o[1] = (uint8_t)gg; o[2] = (uint8_t)b; o[3] = (uint8_t)a; break;
            case GL_RGB:  o[0] = (uint8_t)r; o[1] = (uint8_t)gg; o[2] = (uint8_t)b; break;
            case GL_LUMINANCE_ALPHA:
                o[0] = (uint8_t)((r * 77 + gg * 150 + b * 29) >> 8); o[1] = (uint8_t)a; break;
            case GL_ALPHA: o[0] = (uint8_t)a; break;
            default: o[0] = (uint8_t)((r * 77 + gg * 150 + b * 29) >> 8); break;
            }
        }
    }
    cairo_surface_destroy(s);
    *out_w = w;
    *out_h = h;
    return out;
}

typedef struct {
    GLint align, row_length, skip_rows, skip_pixels, image_height, skip_images;
} wgl_unpack_state;

static void
wgl_unpack_tight(ns_webgl *g, wgl_unpack_state *saved)
{
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &saved->align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (g->version < 2) return;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &saved->row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &saved->skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &saved->skip_pixels);
    glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &saved->image_height);
    glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &saved->skip_images);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
}

static void
wgl_unpack_restore(ns_webgl *g, const wgl_unpack_state *saved)
{
    glPixelStorei(GL_UNPACK_ALIGNMENT, saved->align);
    if (g->version < 2) return;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, saved->row_length);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, saved->skip_rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, saved->skip_pixels);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, saved->image_height);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, saved->skip_images);
}

static GLenum
wgl_upload_type(const ns_webgl *g, GLenum type)
{
    return g->version < 2 && type == GL_HALF_FLOAT_OES ? GL_HALF_FLOAT : type;
}

static void
wgl_webgl1_sized_format(const ns_webgl *g, GLint *internalformat, GLenum format,
                        GLenum *type)
{
    if (g->version >= 2) return;
    if (*type == GL_HALF_FLOAT_OES) {
        *type = GL_HALF_FLOAT;
        if (*internalformat == GL_RGBA) *internalformat = GL_RGBA16F;
        else if (*internalformat == GL_RGB) *internalformat = GL_RGB16F;
    } else if (*type == GL_FLOAT) {
        if (*internalformat == GL_RGBA) *internalformat = GL_RGBA32F;
        else if (*internalformat == GL_RGB) *internalformat = GL_RGB32F;
    } else if (format == GL_DEPTH_COMPONENT && *internalformat == GL_DEPTH_COMPONENT) {
        *internalformat = *type == GL_UNSIGNED_INT ? GL_DEPTH_COMPONENT24
                                                   : GL_DEPTH_COMPONENT16;
    } else if (format == GL_DEPTH_STENCIL && *internalformat == GL_DEPTH_STENCIL) {
        *internalformat = GL_DEPTH24_STENCIL8;
    }
}

static uint8_t *
wgl_tex_source_pixels(JSContext *ctx, ns_webgl *g, JSValueConst src,
                      GLenum format, GLenum type, int want_w, int want_h,
                      gboolean *threw)
{
    *threw = FALSE;
    if (type != GL_UNSIGNED_BYTE || want_w <= 0 || want_h <= 0) return NULL;
    int comps = wgl_components(format);
    int sw = 0, sh = 0;
    uint8_t *px = NULL;
    size_t len = 0;
    JSValue hold;
    const uint8_t *idata = wgl_imagedata_bytes(ctx, src, &sw, &sh, &len, &hold);
    if (idata) {
        size_t need = (size_t)sw * (size_t)sh * 4u;
        if (format == GL_RGBA && len >= need)
            px = g->unpack_flip_y ? wgl_flip_rows(idata, sw, sh, 4)
                                  : g_memdup2(idata, need);
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    } else {
        px = wgl_source_rgba(ctx, src, format, g->unpack_flip_y, g->premultiply,
                             &sw, &sh, threw);
    }
    if (!px) return NULL;
    if (sw == want_w && sh == want_h) return px;
    guint64 total = (guint64)want_w * (guint64)want_h * (guint64)comps;
    uint8_t *out = total <= NS_WEBGL_MAX_ALLOC ? g_try_malloc0((size_t)total)
                                               : NULL;
    if (out) {
        int cw = MIN(sw, want_w), ch = MIN(sh, want_h);
        for (int y = 0; y < ch; y++)
            memcpy(out + (size_t)y * (size_t)want_w * (size_t)comps,
                   px + (size_t)y * (size_t)sw * (size_t)comps,
                   (size_t)cw * (size_t)comps);
    }
    g_free(px);
    return out;
}

static int
wgl_unpack_pbo_offset(JSContext *ctx, ns_webgl *g, JSValueConst v,
                      const void **out)
{
    if (g->version < 2 || !JS_IsNumber(v)) return 0;
    GLint unpack_buffer = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
    int64_t off = 0;
    if (!unpack_buffer || JS_ToInt64(ctx, &off, v) < 0 || off < 0) {
        g->injected_error = GL_INVALID_OPERATION;
        return -1;
    }
    *out = (const void *)(intptr_t)off;
    return 1;
}

static JSValue
wgl_texImage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLint level = argi(ctx, argc, argv, 1);
    GLint internalformat = argi(ctx, argc, argv, 2);

    if (argc >= 9 && JS_IsNumber(argv[3])) {
        GLsizei w = argi(ctx, argc, argv, 3);
        GLsizei h = argi(ctx, argc, argv, 4);
        GLint border = argi(ctx, argc, argv, 5);
        GLenum format = (GLenum)argi(ctx, argc, argv, 6);
        GLenum type = (GLenum)argi(ctx, argc, argv, 7);
        JSValue hold = JS_UNDEFINED;
        size_t len = 0;
        const uint8_t *px = NULL;
        const void *pbo = NULL;
        int pbo_state = wgl_unpack_pbo_offset(ctx, g, argv[8], &pbo);
        if (pbo_state) {
            if (pbo_state > 0)
                glTexImage2D(target, level, internalformat, w, h, border, format,
                             type, pbo);
            return JS_UNDEFINED;
        }
        if (!JS_IsNull(argv[8]) && !JS_IsUndefined(argv[8]))
            px = view_bytes(ctx, argv[8], &len, &hold);
        if (!px && JS_IsObject(argv[8])) {
            gboolean threw = FALSE;
            uint8_t *rgba = wgl_tex_source_pixels(ctx, g, argv[8], format, type,
                                                  w, h, &threw);
            if (threw) return JS_EXCEPTION;
            if (rgba) {
                wgl_unpack_state saved;
                wgl_unpack_tight(g, &saved);
                glTexImage2D(target, level, internalformat, w, h, border, format,
                             type, rgba);
                wgl_unpack_restore(g, &saved);
                g_free(rgba);
            }
            return JS_UNDEFINED;
        }
        size_t need = wgl_transfer_bytes(g, w, h, 1, format, type, FALSE);
        if (need > NS_WEBGL_MAX_ALLOC || (px && len < need)) {
            if (px && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
            return JS_UNDEFINED;
        }
        wgl_webgl1_sized_format(g, &internalformat, format, &type);
        uint8_t *flipped = NULL;
        uint8_t *zero = NULL;
        if (!px) {
            zero = need ? g_try_malloc0(need) : NULL;
            px = zero;
        } else if (wgl_flip_safe(g, w, h, format, type, need, len)) {
            flipped = wgl_flip_rows(px, w, h, wgl_components(format));
            if (flipped) px = flipped;
        }
        glTexImage2D(target, level, internalformat, w, h, border, format, type, px);
        g_free(flipped);
        g_free(zero);
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return JS_UNDEFINED;
    }

    GLenum format = (GLenum)argi(ctx, argc, argv, 3);
    GLenum type = (GLenum)argi(ctx, argc, argv, 4);
    if (argc >= 6 && JS_IsObject(argv[5])) {
        int w = 0, h = 0;
        size_t len = 0;
        JSValue hold;
        const uint8_t *px = wgl_imagedata_bytes(ctx, argv[5], &w, &h, &len, &hold);
        size_t need = (px && w > 0 && h > 0)
            ? wgl_transfer_bytes(g, w, h, 1, format, type, FALSE) : 0;
        if (px && w > 0 && h > 0 && need > 0 && need <= NS_WEBGL_MAX_ALLOC &&
            len >= need) {
            wgl_webgl1_sized_format(g, &internalformat, format, &type);
            uint8_t *flipped = NULL;
            if (wgl_flip_safe(g, w, h, format, type, need, len)) {
                flipped = wgl_flip_rows(px, w, h, wgl_components(format));
                if (flipped) px = flipped;
            }
            glTexImage2D(target, level, internalformat, w, h, 0, format, type, px);
            g_free(flipped);
            if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        } else if (px) {
            if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        } else if (type == GL_UNSIGNED_BYTE) {
            gboolean threw = FALSE;
            uint8_t *rgba = wgl_source_rgba(ctx, argv[5], format,
                                            g->unpack_flip_y, g->premultiply,
                                            &w, &h, &threw);
            if (threw) return JS_EXCEPTION;
            if (rgba) {
                wgl_unpack_state saved;
                wgl_unpack_tight(g, &saved);
                glTexImage2D(target, level, internalformat, w, h, 0, format, type, rgba);
                wgl_unpack_restore(g, &saved);
                g_free(rgba);
            }
        }
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_texSubImage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLint level = argi(ctx, argc, argv, 1);
    GLint xoff = argi(ctx, argc, argv, 2);
    GLint yoff = argi(ctx, argc, argv, 3);

    if (argc >= 9 && JS_IsNumber(argv[4])) {
        GLsizei w = argi(ctx, argc, argv, 4);
        GLsizei h = argi(ctx, argc, argv, 5);
        GLenum format = (GLenum)argi(ctx, argc, argv, 6);
        GLenum type = (GLenum)argi(ctx, argc, argv, 7);
        JSValue hold = JS_UNDEFINED;
        size_t len = 0;
        const uint8_t *px = NULL;
        const void *pbo = NULL;
        int pbo_state = wgl_unpack_pbo_offset(ctx, g, argv[8], &pbo);
        if (pbo_state) {
            if (pbo_state > 0)
                glTexSubImage2D(target, level, xoff, yoff, w, h, format,
                                wgl_upload_type(g, type), pbo);
            return JS_UNDEFINED;
        }
        if (!JS_IsNull(argv[8]) && !JS_IsUndefined(argv[8]))
            px = view_bytes(ctx, argv[8], &len, &hold);
        if (!px && JS_IsObject(argv[8])) {
            gboolean threw = FALSE;
            uint8_t *rgba = wgl_tex_source_pixels(ctx, g, argv[8], format, type,
                                                  w, h, &threw);
            if (threw) return JS_EXCEPTION;
            if (rgba) {
                wgl_unpack_state saved;
                wgl_unpack_tight(g, &saved);
                glTexSubImage2D(target, level, xoff, yoff, w, h, format, type,
                                rgba);
                wgl_unpack_restore(g, &saved);
                g_free(rgba);
            }
            return JS_UNDEFINED;
        }
        size_t need = wgl_transfer_bytes(g, w, h, 1, format, type, FALSE);
        if (need > NS_WEBGL_MAX_ALLOC || (px && len < need)) {
            if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
            return JS_UNDEFINED;
        }
        uint8_t *flipped = NULL;
        if (px && wgl_flip_safe(g, w, h, format, type, need, len)) {
            flipped = wgl_flip_rows(px, w, h, wgl_components(format));
            if (flipped) px = flipped;
        }
        if (px)
            glTexSubImage2D(target, level, xoff, yoff, w, h, format,
                            wgl_upload_type(g, type), px);
        g_free(flipped);
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return JS_UNDEFINED;
    }

    GLenum format = (GLenum)argi(ctx, argc, argv, 4);
    GLenum type = (GLenum)argi(ctx, argc, argv, 5);
    if (argc >= 7 && JS_IsObject(argv[6])) {
        int w = 0, h = 0;
        size_t len = 0;
        JSValue hold;
        const uint8_t *px = wgl_imagedata_bytes(ctx, argv[6], &w, &h, &len, &hold);
        size_t need = (px && w > 0 && h > 0)
            ? wgl_transfer_bytes(g, w, h, 1, format, type, FALSE) : 0;
        if (px && w > 0 && h > 0 && need > 0 && need <= NS_WEBGL_MAX_ALLOC &&
            len >= need) {
            uint8_t *flipped = NULL;
            if (wgl_flip_safe(g, w, h, format, type, need, len)) {
                flipped = wgl_flip_rows(px, w, h, wgl_components(format));
                if (flipped) px = flipped;
            }
            glTexSubImage2D(target, level, xoff, yoff, w, h, format,
                            wgl_upload_type(g, type), px);
            g_free(flipped);
            if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        } else if (px) {
            if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        } else if (type == GL_UNSIGNED_BYTE) {
            gboolean threw = FALSE;
            uint8_t *rgba = wgl_source_rgba(ctx, argv[6], format,
                                            g->unpack_flip_y, g->premultiply,
                                            &w, &h, &threw);
            if (threw) return JS_EXCEPTION;
            if (rgba) {
                wgl_unpack_state saved;
                wgl_unpack_tight(g, &saved);
                glTexSubImage2D(target, level, xoff, yoff, w, h, format, type, rgba);
                wgl_unpack_restore(g, &saved);
                g_free(rgba);
            }
        }
    }
    return JS_UNDEFINED;
}

static JSValue wgl_createFramebuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "framebuffer", glGenFramebuffers); }
static JSValue wgl_deleteFramebuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{
    ns_webgl *g = wgl_cur(c, t);
    if (g && a >= 1) {
        GLuint n = (GLuint)wgl_name(c, v[0]);
        if (n && g->user_draw_fbo == n) g->user_draw_fbo = 0;
        if (n && g->user_read_fbo == n) g->user_read_fbo = 0;
        if (n && g->bound_draw_fbo == n) g->bound_draw_fbo = 0;
        if (n && g->bound_read_fbo == n) g->bound_read_fbo = 0;
    }
    return wgl_del_obj(c, t, a, v, glDeleteFramebuffers);
}

static JSValue
wgl_bindFramebuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLuint f = (argc >= 2) ? (GLuint)wgl_name(ctx, argv[1]) : 0;
    if (target == GL_FRAMEBUFFER) {
        g->user_draw_fbo = f;
        g->user_read_fbo = f;
    } else if (target == GL_DRAW_FRAMEBUFFER) {
        g->user_draw_fbo = f;
    } else if (target == GL_READ_FRAMEBUFFER) {
        g->user_read_fbo = f;
    }
    wgl_bind_framebuffer(g, target, f ? f : ns_webgl_draw_target(g));
    return JS_UNDEFINED;
}

static JSValue
wgl_framebufferTexture2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glFramebufferTexture2D((GLenum)argi(ctx, argc, argv, 0),
                           (GLenum)argi(ctx, argc, argv, 1),
                           (GLenum)argi(ctx, argc, argv, 2),
                           (GLuint)wgl_name(ctx, argv[3]),
                           argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static void
wgl_clear_attachment(GLenum target, GLenum attachment)
{
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean discard = glIsEnabled(GL_RASTERIZER_DISCARD);
    GLboolean cmask[4], dmask;
    GLint smask_front = 0, smask_back = 0;
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &smask_front);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &smask_back);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_RASTERIZER_DISCARD);

    GLint prev_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint fbo = prev_fbo;
    if (target == GL_READ_FRAMEBUFFER) {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)fbo);
    }

    if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_STENCIL_ATTACHMENT ||
        attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFFFFFFFu);
        glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
        glDepthMask(dmask);
        glStencilMaskSeparate(GL_FRONT, (GLuint)smask_front);
        glStencilMaskSeparate(GL_BACK, (GLuint)smask_back);
    } else {
        GLint draw_bufs[8], max_bufs = 1;
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_bufs);
        max_bufs = CLAMP(max_bufs, 1, 8);
        for (int i = 0; i < max_bufs; i++) {
            draw_bufs[i] = GL_NONE;
            glGetIntegerv(GL_DRAW_BUFFER0 + i, &draw_bufs[i]);
        }
        GLenum only = attachment;
        glDrawBuffers(1, &only);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        static const GLfloat zero[4] = { 0, 0, 0, 0 };
        glClearBufferfv(GL_COLOR, 0, zero);
        glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
        GLenum restore[8];
        for (int i = 0; i < max_bufs; i++) restore[i] = (GLenum)draw_bufs[i];
        glDrawBuffers(max_bufs, restore);
    }

    if (target == GL_READ_FRAMEBUFFER)
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_fbo);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (discard) glEnable(GL_RASTERIZER_DISCARD);
}

static void
wgl_clear_new_renderbuffer(ns_webgl *g, GLenum target, GLenum attachment, GLuint rb)
{
    if (!rb || !g->uncleared_rbs ||
        !g_hash_table_contains(g->uncleared_rbs, GUINT_TO_POINTER(rb)))
        return;
    if (glCheckFramebufferStatus(target) != GL_FRAMEBUFFER_COMPLETE) return;
    g_hash_table_remove(g->uncleared_rbs, GUINT_TO_POINTER(rb));
    wgl_clear_attachment(target, attachment);
}

static void
wgl_mark_renderbuffer_uncleared(ns_webgl *g)
{
    GLint rb = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    if (!rb) return;
    if (!g->uncleared_rbs)
        g->uncleared_rbs = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_add(g->uncleared_rbs, GUINT_TO_POINTER((GLuint)rb));
}

static JSValue
wgl_framebufferRenderbuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLenum attachment = (GLenum)argi(ctx, argc, argv, 1);
    GLuint rb = (GLuint)wgl_name(ctx, argv[3]);
    glFramebufferRenderbuffer(target, attachment,
                              (GLenum)argi(ctx, argc, argv, 2), rb);
    wgl_clear_new_renderbuffer(g, target, attachment, rb);
    return JS_UNDEFINED;
}

static JSValue
wgl_checkFramebufferStatus(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    return JS_NewInt32(ctx, (int)glCheckFramebufferStatus((GLenum)argi(ctx, argc, argv, 0)));
}

static JSValue wgl_createRenderbuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "renderbuffer", glGenRenderbuffers); }
static JSValue wgl_deleteRenderbuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteRenderbuffers); }

static JSValue
wgl_bindRenderbuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindRenderbuffer((GLenum)argi(ctx, argc, argv, 0), (GLuint)wgl_name(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
wgl_renderbufferStorage(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glRenderbufferStorage((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                          argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3));
    wgl_mark_renderbuffer_uncleared(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_readPixels(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint x = argi(ctx, argc, argv, 0);
    GLint y = argi(ctx, argc, argv, 1);
    GLsizei w = argi(ctx, argc, argv, 2);
    GLsizei h = argi(ctx, argc, argv, 3);
    GLenum format = (GLenum)argi(ctx, argc, argv, 4);
    GLenum type = (GLenum)argi(ctx, argc, argv, 5);
    if (argc < 7) return JS_UNDEFINED;
    GLint pack_buffer = 0;
    if (g->version >= 2) glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    if (pack_buffer) {
        int64_t offset = 0;
        if (JS_IsObject(argv[6]) || JS_ToInt64(ctx, &offset, argv[6]) || offset < 0) {
            g->injected_error = GL_INVALID_OPERATION;
            return JS_UNDEFINED;
        }
        glReadPixels(x, y, w, h, format, type, (void *)(intptr_t)offset);
        return JS_UNDEFINED;
    }
    if (!JS_IsObject(argv[6])) return JS_UNDEFINED;
    int64_t dst_off = 0;
    if (argc >= 8 && JS_ToInt64(ctx, &dst_off, argv[7]))
        return JS_EXCEPTION;
    JSValue hold;
    size_t len = 0;
    uint8_t *p = (uint8_t *)view_bytes(ctx, argv[6], &len, &hold);
    if (p && argc >= 8) {
        size_t view_off = 0, view_len = 0, bpe = 1;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[6], &view_off, &view_len, &bpe);
        if (JS_IsException(ab)) JS_FreeValue(ctx, JS_GetException(ctx));
        else JS_FreeValue(ctx, ab);
        size_t skip = dst_off > 0 ? (size_t)dst_off * bpe : 0;
        if (skip > len) {
            g->injected_error = GL_INVALID_VALUE;
            if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
            return JS_UNDEFINED;
        }
        p += skip;
        len -= skip;
    }
    size_t need = wgl_transfer_bytes(g, w, h, 1, format, type, TRUE);
    if (p && len >= need && need > 0) {
        GLint bound = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
        gboolean resolved = FALSE;
        if (g->samples > 1 && (GLuint)bound == g->draw_fbo) {
            wgl_bind_framebuffer(g, GL_READ_FRAMEBUFFER, g->draw_fbo);
            wgl_bind_framebuffer(g, GL_DRAW_FRAMEBUFFER, g->fbo);
            wgl_resolve_blit(g->w, g->h);
            wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->fbo);
            resolved = TRUE;
        }
        glReadPixels(x, y, w, h, format, type, (void *)p);
        if (resolved)
            wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->draw_fbo);
    }
    if (p && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    return JS_UNDEFINED;
}

static int
wgl_uints(JSContext *ctx, JSValueConst v, GLuint *out, int max)
{
    return wgl_ints(ctx, v, (GLint *)out, max);
}

static JSValue
wgl_is_obj(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
           GLboolean (*fn)(GLuint))
{
    WGL_GET(0);
    GLuint n = (argc >= 1) ? (GLuint)wgl_name(ctx, argv[0]) : 0;
    return JS_NewBool(ctx, n && fn(n));
}

static JSValue wgl_isBuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsBuffer); }
static JSValue wgl_isProgram(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsProgram); }
static JSValue wgl_isShader(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsShader); }
static JSValue wgl_isTexture(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsTexture); }
static JSValue wgl_isFramebuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsFramebuffer); }
static JSValue wgl_isRenderbuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsRenderbuffer); }

static JSValue
wgl_get_target_iv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                  void (*fn)(GLenum, GLenum, GLint *))
{
    WGL_GET(0);
    GLint v = 0;
    fn((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1), &v);
    return JS_NewInt32(ctx, v);
}

static JSValue wgl_getBufferParameter(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_get_target_iv(c, t, a, v, glGetBufferParameteriv); }
static JSValue wgl_getTexParameter(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_get_target_iv(c, t, a, v, glGetTexParameteriv); }
static JSValue wgl_getRenderbufferParameter(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_get_target_iv(c, t, a, v, glGetRenderbufferParameteriv); }

static JSValue
wgl_getFramebufferAttachmentParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint v = 0;
    glGetFramebufferAttachmentParameteriv((GLenum)argi(ctx, argc, argv, 0),
                                          (GLenum)argi(ctx, argc, argv, 1),
                                          (GLenum)argi(ctx, argc, argv, 2), &v);
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_getVertexAttrib(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 0);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 1);
    if (pname == GL_CURRENT_VERTEX_ATTRIB) {
        GLfloat v[4] = { 0, 0, 0, 0 };
        glGetVertexAttribfv(index, pname, v);
        JSValue a = JS_NewArrayBufferCopy(ctx, (const uint8_t *)v, sizeof(v));
        JSValue ta = wgl_typed_array(ctx, a, JS_TYPED_ARRAY_FLOAT32);
        JS_FreeValue(ctx, a);
        return ta;
    }
    GLint v = 0;
    glGetVertexAttribiv(index, pname, &v);
    if (pname == GL_VERTEX_ATTRIB_ARRAY_ENABLED ||
        pname == GL_VERTEX_ATTRIB_ARRAY_NORMALIZED)
        return JS_NewBool(ctx, v);
    if (pname == GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING)
        return v ? wgl_wrap(g, ctx, (GLuint)v, "buffer") : JS_NULL;
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_getVertexAttribOffset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    void *p = NULL;
    glGetVertexAttribPointerv((GLuint)argi(ctx, argc, argv, 0),
                              (GLenum)argi(ctx, argc, argv, 1), &p);
    return JS_NewInt32(ctx, (int)(intptr_t)p);
}

static JSValue
wgl_getUniform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    return JS_NULL;
}

static JSValue
wgl_vertexAttrib_fv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 0);
    float v[4] = { 0, 0, 0, 1 };
    if (argc >= 2) wgl_floats(ctx, argv[1], v, n);
    switch (n) {
    case 1: glVertexAttrib1fv(index, v); break;
    case 2: glVertexAttrib2fv(index, v); break;
    case 3: glVertexAttrib3fv(index, v); break;
    default: glVertexAttrib4fv(index, v); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_vertexAttrib1fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_fv(c, t, a, v, 1); }
static JSValue wgl_vertexAttrib2fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_fv(c, t, a, v, 2); }
static JSValue wgl_vertexAttrib3fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_fv(c, t, a, v, 3); }
static JSValue wgl_vertexAttrib4fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_fv(c, t, a, v, 4); }

static JSValue wgl_createVertexArray(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "vertexarray", glGenVertexArrays); }
static JSValue wgl_deleteVertexArray(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteVertexArrays); }

static JSValue
wgl_bindVertexArray(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindVertexArray(argc >= 1 ? (GLuint)wgl_name(ctx, argv[0]) : 0);
    return JS_UNDEFINED;
}

static JSValue
wgl_isVertexArray(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsVertexArray); }

static JSValue
wgl_drawArraysInstanced(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum mode = (GLenum)argi(ctx, argc, argv, 0);
    GLint first = argi(ctx, argc, argv, 1);
    GLsizei count = argi(ctx, argc, argv, 2);
    GLsizei instances = argi(ctx, argc, argv, 3);
    if (first < 0 || count < 0 || instances < 0) return JS_UNDEFINED;
    if (count > 0 && instances > 0 &&
        !wgl_attribs_cover(g, (int64_t)first + count - 1, instances))
        return JS_UNDEFINED;
    glDrawArraysInstanced(mode, first, count, instances);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_drawElementsInstanced(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum mode = (GLenum)argi(ctx, argc, argv, 0);
    GLsizei count = argi(ctx, argc, argv, 1);
    GLenum type = (GLenum)argi(ctx, argc, argv, 2);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 3);
    GLsizei instances = argi(ctx, argc, argv, 4);
    if (instances < 0 || !wgl_draw_elements_ok(g, count, type, offset, instances))
        return JS_UNDEFINED;
    glDrawElementsInstanced(mode, count, type, (const void *)offset, instances);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribDivisor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 0);
    GLuint divisor = (GLuint)argi(ctx, argc, argv, 1);
    glVertexAttribDivisor(index, divisor);
    return JS_UNDEFINED;
}

static JSValue
wgl_drawBuffers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint bufs[16];
    int n = (argc >= 1) ? wgl_ints(ctx, argv[0], bufs, 16) : 0;
    if (!g->user_draw_fbo)
        for (int i = 0; i < n; i++)
            if (bufs[i] == GL_BACK) bufs[i] = GL_COLOR_ATTACHMENT0;
    if (n > 0) glDrawBuffers(n, (const GLenum *)bufs);
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribIPointer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint size = argi(ctx, argc, argv, 1);
    GLsizei stride = argi(ctx, argc, argv, 3);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 4);
    if (size < 0 || stride < 0 || offset < 0) return JS_UNDEFINED;
    glVertexAttribIPointer((GLuint)argi(ctx, argc, argv, 0), size,
                           (GLenum)argi(ctx, argc, argv, 2), stride,
                           (const void *)offset);
    return JS_UNDEFINED;
}

static JSValue
wgl_uniform_ui(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    GLuint v[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) v[i] = (GLuint)argi(ctx, argc, argv, i + 1);
    switch (n) {
    case 1: glUniform1ui(loc, v[0]); break;
    case 2: glUniform2ui(loc, v[0], v[1]); break;
    case 3: glUniform3ui(loc, v[0], v[1], v[2]); break;
    default: glUniform4ui(loc, v[0], v[1], v[2], v[3]); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1ui(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_ui(c, t, a, v, 1); }
static JSValue wgl_uniform2ui(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_ui(c, t, a, v, 2); }
static JSValue wgl_uniform3ui(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_ui(c, t, a, v, 3); }
static JSValue wgl_uniform4ui(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_ui(c, t, a, v, 4); }

static JSValue
wgl_uniform_uiv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    GLuint buf[4096];
    int cnt = (argc >= 2) ? wgl_uints(ctx, argv[1], buf, 4096) : 0;
    GLsizei count = cnt / n;
    switch (n) {
    case 1: glUniform1uiv(loc, count, buf); break;
    case 2: glUniform2uiv(loc, count, buf); break;
    case 3: glUniform3uiv(loc, count, buf); break;
    default: glUniform4uiv(loc, count, buf); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1uiv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_uiv(c, t, a, v, 1); }
static JSValue wgl_uniform2uiv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_uiv(c, t, a, v, 2); }
static JSValue wgl_uniform3uiv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_uiv(c, t, a, v, 3); }
static JSValue wgl_uniform4uiv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_uiv(c, t, a, v, 4); }

static JSValue
wgl_uniformMatrix_nxm(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                      int rows, int cols)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    float buf[4096];
    int cnt = (argc >= 3) ? wgl_floats(ctx, argv[2], buf, 4096) : 0;
    GLsizei count = cnt / (rows * cols);
    if (rows == 2 && cols == 3) glUniformMatrix2x3fv(loc, count, GL_FALSE, buf);
    else if (rows == 3 && cols == 2) glUniformMatrix3x2fv(loc, count, GL_FALSE, buf);
    else if (rows == 2 && cols == 4) glUniformMatrix2x4fv(loc, count, GL_FALSE, buf);
    else if (rows == 4 && cols == 2) glUniformMatrix4x2fv(loc, count, GL_FALSE, buf);
    else if (rows == 3 && cols == 4) glUniformMatrix3x4fv(loc, count, GL_FALSE, buf);
    else glUniformMatrix4x3fv(loc, count, GL_FALSE, buf);
    return JS_UNDEFINED;
}

static JSValue wgl_uniformMatrix2x3fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 2, 3); }
static JSValue wgl_uniformMatrix3x2fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 3, 2); }
static JSValue wgl_uniformMatrix2x4fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 2, 4); }
static JSValue wgl_uniformMatrix4x2fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 4, 2); }
static JSValue wgl_uniformMatrix3x4fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 3, 4); }
static JSValue wgl_uniformMatrix4x3fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 4, 3); }

static JSValue
wgl_texStorage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glTexStorage2D((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                   (GLenum)argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                   argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_renderbufferStorageMultisample(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glRenderbufferStorageMultisample((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                                     (GLenum)argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                                     argi(ctx, argc, argv, 4));
    wgl_mark_renderbuffer_uncleared(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_blitFramebuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlitFramebuffer(argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                      argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                      argi(ctx, argc, argv, 4), argi(ctx, argc, argv, 5),
                      argi(ctx, argc, argv, 6), argi(ctx, argc, argv, 7),
                      (GLbitfield)argi(ctx, argc, argv, 8),
                      (GLenum)argi(ctx, argc, argv, 9));
    return JS_UNDEFINED;
}

static JSValue
wgl_framebufferTextureLayer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glFramebufferTextureLayer((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                              (GLuint)wgl_name(ctx, argv[2]), argi(ctx, argc, argv, 3),
                              argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_invalidateFramebuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint att[16];
    int n = (argc >= 2) ? wgl_ints(ctx, argv[1], att, 16) : 0;
    if (n > 0)
        glInvalidateFramebuffer((GLenum)argi(ctx, argc, argv, 0), n, (const GLenum *)att);
    return JS_UNDEFINED;
}

static JSValue
wgl_readBuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum mode = (GLenum)argi(ctx, argc, argv, 0);
    if (!g->user_read_fbo && mode == GL_BACK) mode = GL_COLOR_ATTACHMENT0;
    glReadBuffer(mode);
    return JS_UNDEFINED;
}

static JSValue
wgl_copyBufferSubData(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum read_target = (GLenum)argi(ctx, argc, argv, 0);
    GLenum write_target = (GLenum)argi(ctx, argc, argv, 1);
    GLintptr read_offset = (GLintptr)argi(ctx, argc, argv, 2);
    GLintptr write_offset = (GLintptr)argi(ctx, argc, argv, 3);
    GLsizeiptr size = (GLsizeiptr)argi(ctx, argc, argv, 4);
    glCopyBufferSubData(read_target, write_target, read_offset, write_offset, size);
    wgl_elem_shadow_clear(g, wgl_bound_buffer(g, write_target));
    return JS_UNDEFINED;
}

static JSValue
wgl_getBufferSubData(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 1);
    if (argc < 3 || !JS_IsObject(argv[2])) return JS_UNDEFINED;
    JSValue hold;
    size_t len = 0;
    const uint8_t *dst = view_bytes(ctx, argv[2], &len, &hold);
    if (dst && len > 0 && wgl_buffer_range_ok(g, target, offset, len)) {
        void *src = glMapBufferRange(target, offset, (GLsizeiptr)len, GL_MAP_READ_BIT);
        if (src) {
            memcpy((void *)dst, src, len);
            glUnmapBuffer(target);
        }
    }
    if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    return JS_UNDEFINED;
}

static JSValue
wgl_clearBuffer_fv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    float v[4] = { 0, 0, 0, 0 };
    if (argc >= 3) wgl_floats(ctx, argv[2], v, 4);
    glClearBufferfv((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1), v);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_clearBuffer_iv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint v[4] = { 0, 0, 0, 0 };
    if (argc >= 3) wgl_ints(ctx, argv[2], v, 4);
    glClearBufferiv((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1), v);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_clearBuffer_uiv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint v[4] = { 0, 0, 0, 0 };
    if (argc >= 3) wgl_uints(ctx, argv[2], v, 4);
    glClearBufferuiv((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1), v);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_clearBufferfi(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glClearBufferfi((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                    (float)argd(ctx, argc, argv, 2), argi(ctx, argc, argv, 3));
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue wgl_createSampler(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "sampler", glGenSamplers); }
static JSValue wgl_deleteSampler(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteSamplers); }

static JSValue
wgl_bindSampler(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindSampler((GLuint)argi(ctx, argc, argv, 0),
                  argc >= 2 ? (GLuint)wgl_name(ctx, argv[1]) : 0);
    return JS_UNDEFINED;
}

static JSValue
wgl_samplerParameteri(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glSamplerParameteri((GLuint)wgl_name(ctx, argv[0]), (GLenum)argi(ctx, argc, argv, 1),
                        argi(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_samplerParameterf(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glSamplerParameterf((GLuint)wgl_name(ctx, argv[0]), (GLenum)argi(ctx, argc, argv, 1),
                        (float)argd(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_isSampler(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsSampler); }

static JSValue
wgl_getUniformBlockIndex(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    const char *name = argc >= 2 ? JS_ToCString(ctx, argv[1]) : NULL;
    GLuint idx = GL_INVALID_INDEX;
    if (name) {
        idx = glGetUniformBlockIndex((GLuint)wgl_name(ctx, argv[0]), name);
        JS_FreeCString(ctx, name);
    }
    return JS_NewUint32(ctx, idx);
}

static JSValue
wgl_uniformBlockBinding(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glUniformBlockBinding((GLuint)wgl_name(ctx, argv[0]), (GLuint)argi(ctx, argc, argv, 1),
                          (GLuint)argi(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_bindBufferBase(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindBufferBase((GLenum)argi(ctx, argc, argv, 0), (GLuint)argi(ctx, argc, argv, 1),
                     argc >= 3 ? (GLuint)wgl_name(ctx, argv[2]) : 0);
    return JS_UNDEFINED;
}

static JSValue
wgl_bindBufferRange(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindBufferRange((GLenum)argi(ctx, argc, argv, 0), (GLuint)argi(ctx, argc, argv, 1),
                      argc >= 3 ? (GLuint)wgl_name(ctx, argv[2]) : 0,
                      (GLintptr)argi(ctx, argc, argv, 3), (GLsizeiptr)argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_copyTexImage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glCopyTexImage2D((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                     (GLenum)argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                     argi(ctx, argc, argv, 4), argi(ctx, argc, argv, 5),
                     argi(ctx, argc, argv, 6), argi(ctx, argc, argv, 7));
    return JS_UNDEFINED;
}

static JSValue
wgl_copyTexSubImage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glCopyTexSubImage2D((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                        argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                        argi(ctx, argc, argv, 4), argi(ctx, argc, argv, 5),
                        argi(ctx, argc, argv, 6), argi(ctx, argc, argv, 7));
    return JS_UNDEFINED;
}

static JSValue
wgl_copyTexSubImage3D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glCopyTexSubImage3D((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                        argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                        argi(ctx, argc, argv, 4), argi(ctx, argc, argv, 5),
                        argi(ctx, argc, argv, 6), argi(ctx, argc, argv, 7),
                        argi(ctx, argc, argv, 8));
    return JS_UNDEFINED;
}

static JSValue
wgl_drawRangeElements(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum mode = (GLenum)argi(ctx, argc, argv, 0);
    GLuint start = (GLuint)argi(ctx, argc, argv, 1);
    GLuint end = (GLuint)argi(ctx, argc, argv, 2);
    GLsizei count = argi(ctx, argc, argv, 3);
    GLenum type = (GLenum)argi(ctx, argc, argv, 4);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 5);
    if (!wgl_draw_elements_ok(g, count, type, offset, 1)) return JS_UNDEFINED;
    glDrawRangeElements(mode, start, end, count, type, (const void *)offset);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribI4i(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glVertexAttribI4i((GLuint)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                      argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                      argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribI4ui(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glVertexAttribI4ui((GLuint)argi(ctx, argc, argv, 0), (GLuint)argi(ctx, argc, argv, 1),
                       (GLuint)argi(ctx, argc, argv, 2), (GLuint)argi(ctx, argc, argv, 3),
                       (GLuint)argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribI4iv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint v[4] = { 0, 0, 0, 0 };
    if (argc >= 2) wgl_ints(ctx, argv[1], v, 4);
    glVertexAttribI4iv((GLuint)argi(ctx, argc, argv, 0), v);
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribI4uiv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint v[4] = { 0, 0, 0, 0 };
    if (argc >= 2) wgl_uints(ctx, argv[1], v, 4);
    glVertexAttribI4uiv((GLuint)argi(ctx, argc, argv, 0), v);
    return JS_UNDEFINED;
}

static JSValue
wgl_getFragDataLocation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    const char *name = argc >= 2 ? JS_ToCString(ctx, argv[1]) : NULL;
    GLint loc = -1;
    if (name) {
        loc = glGetFragDataLocation((GLuint)wgl_name(ctx, argv[0]), name);
        JS_FreeCString(ctx, name);
    }
    return JS_NewInt32(ctx, loc);
}

static JSValue
wgl_getInternalformatParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLenum iformat = (GLenum)argi(ctx, argc, argv, 1);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 2);
    GLint count = 0;
    glGetInternalformativ(target, iformat, GL_NUM_SAMPLE_COUNTS, 1, &count);
    if (count <= 0 || count > 64) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, NULL, 0);
        JSValue ta = wgl_typed_array(ctx, ab, JS_TYPED_ARRAY_INT32);
        JS_FreeValue(ctx, ab);
        return ta;
    }
    GLint *vals = g_new0(GLint, count);
    glGetInternalformativ(target, iformat, pname, count, vals);
    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)vals,
                                       (size_t)count * sizeof(GLint));
    JSValue ta = wgl_typed_array(ctx, ab, JS_TYPED_ARRAY_INT32);
    JS_FreeValue(ctx, ab);
    g_free(vals);
    return ta;
}

static JSValue
wgl_texImage3D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLint level = argi(ctx, argc, argv, 1);
    GLint internalformat = argi(ctx, argc, argv, 2);
    GLsizei w = argi(ctx, argc, argv, 3);
    GLsizei h = argi(ctx, argc, argv, 4);
    GLsizei d = argi(ctx, argc, argv, 5);
    GLint border = argi(ctx, argc, argv, 6);
    GLenum format = (GLenum)argi(ctx, argc, argv, 7);
    GLenum type = (GLenum)argi(ctx, argc, argv, 8);
    JSValue hold = JS_UNDEFINED;
    size_t len = 0;
    const uint8_t *px = NULL;
    const void *pbo = NULL;
    int pbo_state = argc >= 10 ? wgl_unpack_pbo_offset(ctx, g, argv[9], &pbo) : 0;
    if (pbo_state) {
        if (pbo_state > 0)
            glTexImage3D(target, level, internalformat, w, h, d, border, format,
                         type, pbo);
        return JS_UNDEFINED;
    }
    if (argc >= 10 && JS_IsObject(argv[9]))
        px = view_bytes(ctx, argv[9], &len, &hold);
    if (!px && argc >= 10 && JS_IsObject(argv[9]) && d > 0) {
        gboolean threw = FALSE;
        uint8_t *rgba = wgl_tex_source_pixels(ctx, g, argv[9], format, type,
                                              w, h * d, &threw);
        if (threw) return JS_EXCEPTION;
        if (rgba) {
            wgl_unpack_state saved;
            wgl_unpack_tight(g, &saved);
            glTexImage3D(target, level, internalformat, w, h, d, border, format,
                         type, rgba);
            wgl_unpack_restore(g, &saved);
            g_free(rgba);
        }
        return JS_UNDEFINED;
    }
    size_t need = wgl_transfer_bytes(g, w, h, d, format, type, FALSE);
    if (need > NS_WEBGL_MAX_ALLOC || (px && len < need)) {
        if (px && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return JS_UNDEFINED;
    }
    uint8_t *zero = NULL;
    if (!px) { zero = need ? g_try_malloc0(need) : NULL; px = zero; }
    glTexImage3D(target, level, internalformat, w, h, d, border, format, type, px);
    g_free(zero);
    if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    return JS_UNDEFINED;
}

static JSValue
wgl_texSubImage3D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLint level = argi(ctx, argc, argv, 1);
    GLint xoff = argi(ctx, argc, argv, 2);
    GLint yoff = argi(ctx, argc, argv, 3);
    GLint zoff = argi(ctx, argc, argv, 4);
    GLsizei w = argi(ctx, argc, argv, 5);
    GLsizei h = argi(ctx, argc, argv, 6);
    GLsizei d = argi(ctx, argc, argv, 7);
    GLenum format = (GLenum)argi(ctx, argc, argv, 8);
    GLenum type = (GLenum)argi(ctx, argc, argv, 9);
    JSValue hold = JS_UNDEFINED;
    size_t len = 0;
    const uint8_t *px = NULL;
    const void *pbo = NULL;
    int pbo_state = argc >= 11 ? wgl_unpack_pbo_offset(ctx, g, argv[10], &pbo) : 0;
    if (pbo_state) {
        if (pbo_state > 0)
            glTexSubImage3D(target, level, xoff, yoff, zoff, w, h, d, format,
                            type, pbo);
        return JS_UNDEFINED;
    }
    if (argc >= 11 && JS_IsObject(argv[10]))
        px = view_bytes(ctx, argv[10], &len, &hold);
    if (!px && argc >= 11 && JS_IsObject(argv[10]) && d > 0) {
        gboolean threw = FALSE;
        uint8_t *rgba = wgl_tex_source_pixels(ctx, g, argv[10], format, type,
                                              w, h * d, &threw);
        if (threw) return JS_EXCEPTION;
        if (rgba) {
            wgl_unpack_state saved;
            wgl_unpack_tight(g, &saved);
            glTexSubImage3D(target, level, xoff, yoff, zoff, w, h, d, format,
                            type, rgba);
            wgl_unpack_restore(g, &saved);
            g_free(rgba);
        }
        return JS_UNDEFINED;
    }
    size_t need = wgl_transfer_bytes(g, w, h, d, format, type, FALSE);
    if (px && need > 0 && need <= NS_WEBGL_MAX_ALLOC && len >= need)
        glTexSubImage3D(target, level, xoff, yoff, zoff, w, h, d, format, type, px);
    if (px && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    return JS_UNDEFINED;
}

static JSValue
wgl_texStorage3D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glTexStorage3D((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                   (GLenum)argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                   argi(ctx, argc, argv, 4), argi(ctx, argc, argv, 5));
    return JS_UNDEFINED;
}

static JSValue wgl_createQuery(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "query", glGenQueries); }
static JSValue wgl_deleteQuery(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteQueries); }

static JSValue
wgl_isQuery(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsQuery); }

static JSValue
wgl_beginQuery(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBeginQuery((GLenum)argi(ctx, argc, argv, 0), (GLuint)wgl_name(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
wgl_endQuery(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glEndQuery((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_getQueryParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 1);
    GLuint v = 0;
    glGetQueryObjectuiv((GLuint)wgl_name(ctx, argv[0]), pname, &v);
    if (pname == GL_QUERY_RESULT_AVAILABLE)
        return JS_NewBool(ctx, v);
    return JS_NewUint32(ctx, v);
}

static JSValue
wgl_getQuery(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint v = 0;
    glGetQueryiv((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1), &v);
    return v ? wgl_wrap(g, ctx, (GLuint)v, "query") : JS_NULL;
}

static JSValue wgl_createTransformFeedback(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "transformfeedback", glGenTransformFeedbacks); }
static JSValue wgl_deleteTransformFeedback(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteTransformFeedbacks); }

static JSValue
wgl_isTransformFeedback(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsTransformFeedback); }

static JSValue
wgl_bindTransformFeedback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindTransformFeedback((GLenum)argi(ctx, argc, argv, 0),
                            argc >= 2 ? (GLuint)wgl_name(ctx, argv[1]) : 0);
    return JS_UNDEFINED;
}

static JSValue
wgl_beginTransformFeedback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBeginTransformFeedback((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_endTransformFeedback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    glEndTransformFeedback();
    if (g->elem_data) g_hash_table_remove_all(g->elem_data);
    return JS_UNDEFINED;
}

static JSValue
wgl_pauseTransformFeedback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    glPauseTransformFeedback();
    return JS_UNDEFINED;
}

static JSValue
wgl_resumeTransformFeedback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    glResumeTransformFeedback();
    return JS_UNDEFINED;
}

static JSValue
wgl_transformFeedbackVaryings(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    if (argc < 3 || !JS_IsObject(argv[1])) return JS_UNDEFINED;
    JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    if (n == 0 || n > 256) return JS_UNDEFINED;
    char **names = g_new0(char *, n);
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], i);
        const char *s = JS_ToCString(ctx, e);
        names[i] = g_strdup(s ? s : "");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
    }
    glTransformFeedbackVaryings((GLuint)wgl_name(ctx, argv[0]), (GLsizei)n,
                                (const GLchar *const *)names,
                                (GLenum)argi(ctx, argc, argv, 2));
    for (uint32_t i = 0; i < n; i++) g_free(names[i]);
    g_free(names);
    return JS_UNDEFINED;
}

static JSValue
wgl_getActiveUniforms(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    if (argc < 3 || !JS_IsObject(argv[1])) return JS_NULL;
    JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    if (n == 0 || n > 4096) return JS_NewArray(ctx);
    GLuint *indices = g_new0(GLuint, n);
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], i);
        int32_t v = 0;
        JS_ToInt32(ctx, &v, e);
        JS_FreeValue(ctx, e);
        indices[i] = (GLuint)v;
    }
    GLint *out = g_new0(GLint, n);
    glGetActiveUniformsiv((GLuint)wgl_name(ctx, argv[0]), (GLsizei)n, indices,
                          (GLenum)argi(ctx, argc, argv, 2), out);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, out[i]));
    g_free(indices);
    g_free(out);
    return arr;
}

static JSValue
wgl_getActiveUniformBlockParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint pr = (GLuint)wgl_name(ctx, argv[0]);
    GLuint idx = (GLuint)argi(ctx, argc, argv, 1);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 2);
    if (pname == GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES) {
        GLint count = 0;
        glGetActiveUniformBlockiv(pr, idx, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count);
        if (count <= 0 || count > 4096) count = 0;
        GLint *vals = g_new0(GLint, count > 0 ? count : 1);
        if (count > 0)
            glGetActiveUniformBlockiv(pr, idx, pname, vals);
        JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)vals,
                                           (size_t)count * sizeof(GLint));
        JSValue ta = wgl_typed_array(ctx, ab, JS_TYPED_ARRAY_UINT32);
        JS_FreeValue(ctx, ab);
        g_free(vals);
        return ta;
    }
    GLint v = 0;
    glGetActiveUniformBlockiv(pr, idx, pname, &v);
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_getActiveUniformBlockName(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    char name[256] = { 0 };
    glGetActiveUniformBlockName((GLuint)wgl_name(ctx, argv[0]),
                                (GLuint)argi(ctx, argc, argv, 1),
                                sizeof(name) - 1, NULL, name);
    return JS_NewString(ctx, name);
}

static JSValue
wgl_fenceSync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLsync s = glFenceSync((GLenum)argi(ctx, argc, argv, 0),
                           (GLbitfield)argi(ctx, argc, argv, 1));
    if (!s) return JS_NULL;
    if (!g->syncs)
        g->syncs = g_hash_table_new(g_direct_hash, g_direct_equal);
    int id = ++g->next_sync;
    g_hash_table_insert(g->syncs, GINT_TO_POINTER(id), (gpointer)s);
    return wgl_new_object(g, ctx, (GLuint)id, "sync");
}

static GLsync
wgl_sync_lookup(JSContext *ctx, ns_webgl *g, JSValueConst v)
{
    (void)ctx;
    ns_webgl_obj *o = JS_GetOpaque(v, ns_webgl_obj_class_id);
    if (!g->syncs || !o || o->kind != 8) return NULL;
    return (GLsync)g_hash_table_lookup(g->syncs, GINT_TO_POINTER((int)o->name));
}

static JSValue
wgl_isSync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLsync s = argc >= 1 ? wgl_sync_lookup(ctx, g, argv[0]) : NULL;
    return JS_NewBool(ctx, s && glIsSync(s));
}

static JSValue
wgl_deleteSync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    if (argc < 1 || !g->syncs) return JS_UNDEFINED;
    GLsync s = wgl_sync_lookup(ctx, g, argv[0]);
    if (s) {
        glDeleteSync(s);
        g_hash_table_remove(g->syncs, GINT_TO_POINTER(wgl_name(ctx, argv[0])));
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_clientWaitSync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLbitfield flags = (GLbitfield)argi(ctx, argc, argv, 1);
    double timeout = argd(ctx, argc, argv, 2);
    GLuint64 ns = !(timeout > 0) ? 0
                : timeout >= 18446744073709551616.0 ? UINT64_MAX
                : (GLuint64)timeout;
    GLsync s = wgl_sync_lookup(ctx, g, argv[0]);
    if (!s) return JS_NewInt32(ctx, (int)GL_WAIT_FAILED);
    GLenum r = glClientWaitSync(s, flags, ns);
    return JS_NewInt32(ctx, (int)r);
}

static JSValue
wgl_waitSync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLbitfield flags = (GLbitfield)argi(ctx, argc, argv, 1);
    GLsync s = wgl_sync_lookup(ctx, g, argv[0]);
    if (s)
        glWaitSync(s, flags, GL_TIMEOUT_IGNORED);
    return JS_UNDEFINED;
}

static JSValue
wgl_getSyncParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 1);
    GLsync s = wgl_sync_lookup(ctx, g, argv[0]);
    if (!s) return JS_NULL;
    GLint v = 0;
    GLsizei len = 0;
    glGetSynciv(s, pname, 1, &len, &v);
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_sampleCoverage(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glSampleCoverage((float)argd(ctx, argc, argv, 0),
                     argbool(ctx, argc, argv, 1) ? GL_TRUE : GL_FALSE);
    return JS_UNDEFINED;
}

static JSValue
wgl_stencilFuncSeparate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glStencilFuncSeparate((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                          argi(ctx, argc, argv, 2), (GLuint)argi(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_stencilOpSeparate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glStencilOpSeparate((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                        (GLenum)argi(ctx, argc, argv, 2), (GLenum)argi(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_stencilMaskSeparate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glStencilMaskSeparate((GLenum)argi(ctx, argc, argv, 0), (GLuint)argi(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_getAttachedShaders(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_NULL;
    GLuint shaders[16];
    GLsizei count = 0;
    glGetAttachedShaders((GLuint)wgl_name(ctx, argv[0]), 16, &count, shaders);
    JSValue arr = JS_NewArray(ctx);
    for (GLsizei i = 0; i < count && i < 16; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, wgl_wrap(g, ctx, shaders[i], "shader"));
    return arr;
}

static gboolean
wgl_compressed_block(GLenum fmt, int *bw, int *bh, int *bytes)
{
    *bw = *bh = 4;
    switch (fmt) {
    case 0x83F0: case 0x83F1: case 0x8C4C: case 0x8C4D:
    case 0x8DBB: case 0x8DBC:
    case 0x9270: case 0x9271: case 0x9274: case 0x9275: case 0x9276: case 0x9277:
        *bytes = 8; return TRUE;
    case 0x83F2: case 0x83F3: case 0x8C4E: case 0x8C4F:
    case 0x8DBD: case 0x8DBE:
    case 0x8E8C: case 0x8E8D: case 0x8E8E: case 0x8E8F:
    case 0x9272: case 0x9273: case 0x9278: case 0x9279:
        *bytes = 16; return TRUE;
    }
    static const unsigned char astc[14][2] = {
        { 4, 4 }, { 5, 4 }, { 5, 5 }, { 6, 5 }, { 6, 6 }, { 8, 5 }, { 8, 6 },
        { 8, 8 }, { 10, 5 }, { 10, 6 }, { 10, 8 }, { 10, 10 }, { 12, 10 }, { 12, 12 },
    };
    int idx = fmt >= 0x93B0 && fmt <= 0x93BD ? (int)(fmt - 0x93B0)
            : fmt >= 0x93D0 && fmt <= 0x93DD ? (int)(fmt - 0x93D0) : -1;
    if (idx < 0) return FALSE;
    *bw = astc[idx][0];
    *bh = astc[idx][1];
    *bytes = 16;
    return TRUE;
}

static gboolean
wgl_compressed_size(GLenum fmt, GLsizei w, GLsizei h, GLsizei d, size_t *out)
{
    int bw, bh, bytes;
    if (w < 0 || h < 0 || d < 0 || !wgl_compressed_block(fmt, &bw, &bh, &bytes))
        return FALSE;
    size_t blocks_x = ((size_t)w + (size_t)bw - 1) / (size_t)bw;
    size_t blocks_y = ((size_t)h + (size_t)bh - 1) / (size_t)bh;
    size_t n;
    if (__builtin_mul_overflow(blocks_x, blocks_y, &n) ||
        __builtin_mul_overflow(n, (size_t)bytes, &n) ||
        __builtin_mul_overflow(n, (size_t)(d ? d : 1), &n))
        return FALSE;
    *out = n;
    return TRUE;
}

static const uint8_t *
wgl_compressed_source(JSContext *ctx, int argc, JSValueConst *argv, int i,
                      size_t *len, JSValue *hold)
{
    *hold = JS_UNDEFINED;
    if (i >= argc || !JS_IsObject(argv[i])) return NULL;
    int64_t off = 0, override = 0;
    if (argc > i + 1 && JS_ToInt64(ctx, &off, argv[i + 1])) return NULL;
    if (argc > i + 2 && JS_ToInt64(ctx, &override, argv[i + 2])) return NULL;
    size_t total = 0;
    const uint8_t *px = view_bytes(ctx, argv[i], &total, hold);
    if (!px) return NULL;
    size_t view_off = 0, view_len = 0, bpe = 1;
    JSValue tb = JS_GetTypedArrayBuffer(ctx, argv[i], &view_off, &view_len, &bpe);
    if (JS_IsException(tb)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, tb);
    if (bpe == 0) bpe = 1;
    uint64_t elems = total / bpe;
    if (off < 0 || override < 0 || (uint64_t)off > elems) return NULL;
    uint64_t count = override ? (uint64_t)override : elems - (uint64_t)off;
    if ((uint64_t)off + count > elems) return NULL;
    *len = (size_t)(count * bpe);
    return px + (size_t)off * bpe;
}

static JSValue
wgl_compressed_upload(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv, gboolean sub, gboolean three_d)
{
    WGL_GET(0);
    int dims = three_d ? 3 : 2;
    int base = sub ? 2 + dims : 2;
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLint level = argi(ctx, argc, argv, 1);
    GLint off[3] = { 0, 0, 0 };
    if (sub)
        for (int k = 0; k < dims; k++) off[k] = argi(ctx, argc, argv, 2 + k);
    GLenum fmt = (GLenum)argi(ctx, argc, argv, sub ? base + dims : 2);
    int size_at = sub ? base : 3;
    GLsizei w = argi(ctx, argc, argv, size_at);
    GLsizei h = argi(ctx, argc, argv, size_at + 1);
    GLsizei d = three_d ? argi(ctx, argc, argv, size_at + 2) : 1;
    int data_at = sub ? base + dims + 1 : 3 + dims + 1;
    size_t need = 0;
    if (!wgl_compressed_size(fmt, w, h, d, &need)) {
        g->injected_error = GL_INVALID_ENUM;
        return JS_UNDEFINED;
    }
    if (data_at < argc && JS_IsNumber(argv[data_at])) {
        GLsizei image_size = argi(ctx, argc, argv, data_at);
        GLintptr pbo_off = (GLintptr)argi(ctx, argc, argv, data_at + 1);
        GLint unpack_buffer = 0;
        if (g->version >= 2)
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
        if (!unpack_buffer || image_size < 0 || pbo_off < 0) {
            g->injected_error = GL_INVALID_OPERATION;
            return JS_UNDEFINED;
        }
        const void *ptr = (const void *)pbo_off;
        if (three_d && sub)
            glCompressedTexSubImage3D(target, level, off[0], off[1], off[2], w, h, d,
                                      fmt, image_size, ptr);
        else if (three_d)
            glCompressedTexImage3D(target, level, fmt, w, h, d, 0, image_size, ptr);
        else if (sub)
            glCompressedTexSubImage2D(target, level, off[0], off[1], w, h, fmt,
                                      image_size, ptr);
        else
            glCompressedTexImage2D(target, level, fmt, w, h, 0, image_size, ptr);
        return JS_UNDEFINED;
    }
    size_t len = 0;
    JSValue hold;
    const uint8_t *px = wgl_compressed_source(ctx, argc, argv, data_at, &len, &hold);
    if (!px || len != need || len > NS_WEBGL_MAX_ALLOC) {
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        g->injected_error = GL_INVALID_VALUE;
        return JS_UNDEFINED;
    }
    if (three_d && sub)
        glCompressedTexSubImage3D(target, level, off[0], off[1], off[2], w, h, d,
                                  fmt, (GLsizei)len, px);
    else if (three_d)
        glCompressedTexImage3D(target, level, fmt, w, h, d, 0, (GLsizei)len, px);
    else if (sub)
        glCompressedTexSubImage2D(target, level, off[0], off[1], w, h, fmt,
                                  (GLsizei)len, px);
    else
        glCompressedTexImage2D(target, level, fmt, w, h, 0, (GLsizei)len, px);
    if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    return JS_UNDEFINED;
}

static JSValue
wgl_compressedTexImage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return wgl_compressed_upload(ctx, this_val, argc, argv, FALSE, FALSE); }
static JSValue
wgl_compressedTexSubImage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return wgl_compressed_upload(ctx, this_val, argc, argv, TRUE, FALSE); }
static JSValue
wgl_compressedTexImage3D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return wgl_compressed_upload(ctx, this_val, argc, argv, FALSE, TRUE); }
static JSValue
wgl_compressedTexSubImage3D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return wgl_compressed_upload(ctx, this_val, argc, argv, TRUE, TRUE); }

static JSValue
wgl_getIndexedParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 1);
    switch (target) {
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
    case GL_UNIFORM_BUFFER_BINDING: {
        GLint name = 0;
        glGetIntegeri_v(target, index, &name);
        return wgl_wrap(g, ctx, (GLuint)name, "buffer");
    }
    case GL_TRANSFORM_FEEDBACK_BUFFER_START:
    case GL_TRANSFORM_FEEDBACK_BUFFER_SIZE:
    case GL_UNIFORM_BUFFER_START:
    case GL_UNIFORM_BUFFER_SIZE: {
        GLint64 v = 0;
        glGetInteger64i_v(target, index, &v);
        return JS_NewInt64(ctx, v);
    }
    default:
        g->injected_error = GL_INVALID_ENUM;
        return JS_NULL;
    }
}

static JSValue
wgl_getSamplerParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint sampler = (GLuint)wgl_name(ctx, argv[0]);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 1);
    switch (pname) {
    case GL_TEXTURE_MAX_LOD:
    case GL_TEXTURE_MIN_LOD: {
        GLfloat f = 0;
        glGetSamplerParameterfv(sampler, pname, &f);
        return JS_NewFloat64(ctx, f);
    }
    case GL_TEXTURE_COMPARE_FUNC:
    case GL_TEXTURE_COMPARE_MODE:
    case GL_TEXTURE_MAG_FILTER:
    case GL_TEXTURE_MIN_FILTER:
    case GL_TEXTURE_WRAP_R:
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T: {
        GLint v = 0;
        glGetSamplerParameteriv(sampler, pname, &v);
        return JS_NewInt32(ctx, v);
    }
    default:
        g->injected_error = GL_INVALID_ENUM;
        return JS_NULL;
    }
}

static JSValue
wgl_getTransformFeedbackVarying(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    char name[256] = { 0 };
    GLint size = 0;
    GLenum type = 0;
    glGetTransformFeedbackVarying((GLuint)wgl_name(ctx, argv[0]),
                                  (GLuint)argi(ctx, argc, argv, 1),
                                  sizeof(name) - 1, NULL, &size, &type, name);
    return wgl_active_info(g, ctx, size, type, name);
}

static JSValue
wgl_getUniformIndices(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    if (argc < 2 || !JS_IsObject(argv[1])) return JS_NULL;
    JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    if (n == 0 || n > 4096) return JS_NewArray(ctx);
    char **names = g_new0(char *, n);
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], i);
        const char *str = JS_ToCString(ctx, e);
        names[i] = g_strdup(str ? str : "");
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, e);
    }
    GLuint *indices = g_new0(GLuint, n);
    glGetUniformIndices((GLuint)wgl_name(ctx, argv[0]), (GLsizei)n,
                        (const GLchar *const *)names, indices);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < n; i++) {
        JS_SetPropertyUint32(ctx, arr, i, JS_NewUint32(ctx, indices[i]));
        g_free(names[i]);
    }
    g_free(names);
    g_free(indices);
    return arr;
}

static JSValue
wgl_invalidateSubFramebuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint att[16];
    int n = (argc >= 2) ? wgl_ints(ctx, argv[1], att, 16) : 0;
    if (n > 0)
        glInvalidateSubFramebuffer((GLenum)argi(ctx, argc, argv, 0), n, (const GLenum *)att,
                                   argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                                   argi(ctx, argc, argv, 4), argi(ctx, argc, argv, 5));
    return JS_UNDEFINED;
}

static JSValue
wgl_drawingBufferStorage(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum format = (GLenum)argi(ctx, argc, argv, 0);
    int w = argi(ctx, argc, argv, 1);
    int h = argi(ctx, argc, argv, 2);
    if (format != GL_RGBA8 && format != GL_RGB8) {
        g->injected_error = GL_INVALID_ENUM;
        return JS_UNDEFINED;
    }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        g->injected_error = GL_INVALID_VALUE;
        return JS_UNDEFINED;
    }
    g->w = w;
    g->h = h;
    ns_webgl_alloc_storage(g, w, h);
    glViewport(0, 0, w, h);
    wgl_mark_dirty(g);
    if (g->surf) { cairo_surface_destroy(g->surf); g->surf = NULL; }
    return JS_UNDEFINED;
}

static JSValue
wgl_makeXRCompatible(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    if (!wgl_brand(ctx, this_val)) return JS_EXCEPTION;
    JSValue resolvers[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolvers);
    if (JS_IsException(promise)) return promise;
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "InvalidStateError"));
    JS_SetPropertyStr(ctx, err, "message",
                      JS_NewString(ctx, "No XR device is available."));
    JS_FreeValue(ctx, JS_Call(ctx, resolvers[1], JS_UNDEFINED, 1, &err));
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, resolvers[0]);
    JS_FreeValue(ctx, resolvers[1]);
    return promise;
}

static void
set_const(JSContext *ctx, JSValueConst obj, const char *name, int value)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewInt32(ctx, value));
}

typedef struct ns_gl_constant {
    const char *name;
    int64_t     value;
} ns_gl_constant;

static const ns_gl_constant wgl_constants[] = {
    { "ACTIVE_ATTRIBUTES", 0x8B89 }, { "ACTIVE_TEXTURE", 0x84E0 },
    { "ACTIVE_UNIFORMS", 0x8B86 }, { "ALIASED_LINE_WIDTH_RANGE", 0x846E },
    { "ALIASED_POINT_SIZE_RANGE", 0x846D }, { "ALPHA", 0x1906 },
    { "ALPHA_BITS", 0xD55 }, { "ALWAYS", 0x207 }, { "ARRAY_BUFFER", 0x8892 },
    { "ARRAY_BUFFER_BINDING", 0x8894 }, { "ATTACHED_SHADERS", 0x8B85 },
    { "BACK", 0x405 }, { "BLEND", 0xBE2 }, { "BLEND_COLOR", 0x8005 },
    { "BLEND_DST_ALPHA", 0x80CA }, { "BLEND_DST_RGB", 0x80C8 },
    { "BLEND_EQUATION", 0x8009 }, { "BLEND_EQUATION_ALPHA", 0x883D },
    { "BLEND_EQUATION_RGB", 0x8009 }, { "BLEND_SRC_ALPHA", 0x80CB },
    { "BLEND_SRC_RGB", 0x80C9 }, { "BLUE_BITS", 0xD54 }, { "BOOL", 0x8B56 },
    { "BOOL_VEC2", 0x8B57 }, { "BOOL_VEC3", 0x8B58 }, { "BOOL_VEC4", 0x8B59 },
    { "BROWSER_DEFAULT_WEBGL", 0x9244 }, { "BUFFER_SIZE", 0x8764 },
    { "BUFFER_USAGE", 0x8765 }, { "BYTE", 0x1400 }, { "CCW", 0x901 },
    { "CLAMP_TO_EDGE", 0x812F }, { "COLOR_ATTACHMENT0", 0x8CE0 },
    { "COLOR_BUFFER_BIT", 0x4000 }, { "COLOR_CLEAR_VALUE", 0xC22 },
    { "COLOR_WRITEMASK", 0xC23 }, { "COMPILE_STATUS", 0x8B81 },
    { "COMPRESSED_TEXTURE_FORMATS", 0x86A3 }, { "CONSTANT_ALPHA", 0x8003 },
    { "CONSTANT_COLOR", 0x8001 }, { "CONTEXT_LOST_WEBGL", 0x9242 },
    { "CULL_FACE", 0xB44 }, { "CULL_FACE_MODE", 0xB45 },
    { "CURRENT_PROGRAM", 0x8B8D }, { "CURRENT_VERTEX_ATTRIB", 0x8626 },
    { "CW", 0x900 }, { "DECR", 0x1E03 }, { "DECR_WRAP", 0x8508 },
    { "DELETE_STATUS", 0x8B80 }, { "DEPTH_ATTACHMENT", 0x8D00 },
    { "DEPTH_BITS", 0xD56 }, { "DEPTH_BUFFER_BIT", 0x100 },
    { "DEPTH_CLEAR_VALUE", 0xB73 }, { "DEPTH_COMPONENT", 0x1902 },
    { "DEPTH_COMPONENT16", 0x81A5 }, { "DEPTH_FUNC", 0xB74 },
    { "DEPTH_RANGE", 0xB70 }, { "DEPTH_STENCIL", 0x84F9 },
    { "DEPTH_STENCIL_ATTACHMENT", 0x821A }, { "DEPTH_TEST", 0xB71 },
    { "DEPTH_WRITEMASK", 0xB72 }, { "DITHER", 0xBD0 },
    { "DONT_CARE", 0x1100 }, { "DST_ALPHA", 0x304 }, { "DST_COLOR", 0x306 },
    { "DYNAMIC_DRAW", 0x88E8 }, { "ELEMENT_ARRAY_BUFFER", 0x8893 },
    { "ELEMENT_ARRAY_BUFFER_BINDING", 0x8895 }, { "EQUAL", 0x202 },
    { "FASTEST", 0x1101 }, { "FLOAT", 0x1406 }, { "FLOAT_MAT2", 0x8B5A },
    { "FLOAT_MAT3", 0x8B5B }, { "FLOAT_MAT4", 0x8B5C },
    { "FLOAT_VEC2", 0x8B50 }, { "FLOAT_VEC3", 0x8B51 },
    { "FLOAT_VEC4", 0x8B52 }, { "FRAGMENT_SHADER", 0x8B30 },
    { "FRAMEBUFFER", 0x8D40 },
    { "FRAMEBUFFER_ATTACHMENT_OBJECT_NAME", 0x8CD1 },
    { "FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE", 0x8CD0 },
    { "FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE", 0x8CD3 },
    { "FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL", 0x8CD2 },
    { "FRAMEBUFFER_BINDING", 0x8CA6 }, { "FRAMEBUFFER_COMPLETE", 0x8CD5 },
    { "FRAMEBUFFER_INCOMPLETE_ATTACHMENT", 0x8CD6 },
    { "FRAMEBUFFER_INCOMPLETE_DIMENSIONS", 0x8CD9 },
    { "FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT", 0x8CD7 },
    { "FRAMEBUFFER_UNSUPPORTED", 0x8CDD }, { "FRONT", 0x404 },
    { "FRONT_AND_BACK", 0x408 }, { "FRONT_FACE", 0xB46 },
    { "FUNC_ADD", 0x8006 }, { "FUNC_REVERSE_SUBTRACT", 0x800B },
    { "FUNC_SUBTRACT", 0x800A }, { "GENERATE_MIPMAP_HINT", 0x8192 },
    { "GEQUAL", 0x206 }, { "GREATER", 0x204 }, { "GREEN_BITS", 0xD53 },
    { "HIGH_FLOAT", 0x8DF2 }, { "HIGH_INT", 0x8DF5 },
    { "IMPLEMENTATION_COLOR_READ_FORMAT", 0x8B9B },
    { "IMPLEMENTATION_COLOR_READ_TYPE", 0x8B9A }, { "INCR", 0x1E02 },
    { "INCR_WRAP", 0x8507 }, { "INT", 0x1404 }, { "INT_VEC2", 0x8B53 },
    { "INT_VEC3", 0x8B54 }, { "INT_VEC4", 0x8B55 }, { "INVALID_ENUM", 0x500 },
    { "INVALID_FRAMEBUFFER_OPERATION", 0x506 },
    { "INVALID_OPERATION", 0x502 }, { "INVALID_VALUE", 0x501 },
    { "INVERT", 0x150A }, { "KEEP", 0x1E00 }, { "LEQUAL", 0x203 },
    { "LESS", 0x201 }, { "LINEAR", 0x2601 },
    { "LINEAR_MIPMAP_LINEAR", 0x2703 }, { "LINEAR_MIPMAP_NEAREST", 0x2701 },
    { "LINES", 1 }, { "LINE_LOOP", 2 }, { "LINE_STRIP", 3 },
    { "LINE_WIDTH", 0xB21 }, { "LINK_STATUS", 0x8B82 },
    { "LOW_FLOAT", 0x8DF0 }, { "LOW_INT", 0x8DF3 }, { "LUMINANCE", 0x1909 },
    { "LUMINANCE_ALPHA", 0x190A },
    { "MAX_COMBINED_TEXTURE_IMAGE_UNITS", 0x8B4D },
    { "MAX_CUBE_MAP_TEXTURE_SIZE", 0x851C },
    { "MAX_FRAGMENT_UNIFORM_VECTORS", 0x8DFD },
    { "MAX_RENDERBUFFER_SIZE", 0x84E8 },
    { "MAX_TEXTURE_IMAGE_UNITS", 0x8872 }, { "MAX_TEXTURE_SIZE", 0xD33 },
    { "MAX_VARYING_VECTORS", 0x8DFC }, { "MAX_VERTEX_ATTRIBS", 0x8869 },
    { "MAX_VERTEX_TEXTURE_IMAGE_UNITS", 0x8B4C },
    { "MAX_VERTEX_UNIFORM_VECTORS", 0x8DFB }, { "MAX_VIEWPORT_DIMS", 0xD3A },
    { "MEDIUM_FLOAT", 0x8DF1 }, { "MEDIUM_INT", 0x8DF4 },
    { "MIRRORED_REPEAT", 0x8370 }, { "NEAREST", 0x2600 },
    { "NEAREST_MIPMAP_LINEAR", 0x2702 }, { "NEAREST_MIPMAP_NEAREST", 0x2700 },
    { "NEVER", 0x200 }, { "NICEST", 0x1102 }, { "NONE", 0 },
    { "NOTEQUAL", 0x205 }, { "NO_ERROR", 0 }, { "ONE", 1 },
    { "ONE_MINUS_CONSTANT_ALPHA", 0x8004 },
    { "ONE_MINUS_CONSTANT_COLOR", 0x8002 }, { "ONE_MINUS_DST_ALPHA", 0x305 },
    { "ONE_MINUS_DST_COLOR", 0x307 }, { "ONE_MINUS_SRC_ALPHA", 0x303 },
    { "ONE_MINUS_SRC_COLOR", 0x301 }, { "OUT_OF_MEMORY", 0x505 },
    { "PACK_ALIGNMENT", 0xD05 }, { "POINTS", 0 },
    { "POLYGON_OFFSET_FACTOR", 0x8038 }, { "POLYGON_OFFSET_FILL", 0x8037 },
    { "POLYGON_OFFSET_UNITS", 0x2A00 }, { "RED_BITS", 0xD52 },
    { "RENDERBUFFER", 0x8D41 }, { "RENDERBUFFER_ALPHA_SIZE", 0x8D53 },
    { "RENDERBUFFER_BINDING", 0x8CA7 }, { "RENDERBUFFER_BLUE_SIZE", 0x8D52 },
    { "RENDERBUFFER_DEPTH_SIZE", 0x8D54 },
    { "RENDERBUFFER_GREEN_SIZE", 0x8D51 }, { "RENDERBUFFER_HEIGHT", 0x8D43 },
    { "RENDERBUFFER_INTERNAL_FORMAT", 0x8D44 },
    { "RENDERBUFFER_RED_SIZE", 0x8D50 },
    { "RENDERBUFFER_STENCIL_SIZE", 0x8D55 }, { "RENDERBUFFER_WIDTH", 0x8D42 },
    { "RENDERER", 0x1F01 }, { "REPEAT", 0x2901 }, { "REPLACE", 0x1E01 },
    { "RGB", 0x1907 }, { "RGB565", 0x8D62 }, { "RGB5_A1", 0x8057 },
    { "RGB8", 0x8051 }, { "RGBA", 0x1908 }, { "RGBA4", 0x8056 },
    { "RGBA8", 0x8058 }, { "SAMPLER_2D", 0x8B5E }, { "SAMPLER_CUBE", 0x8B60 },
    { "SAMPLES", 0x80A9 }, { "SAMPLE_ALPHA_TO_COVERAGE", 0x809E },
    { "SAMPLE_BUFFERS", 0x80A8 }, { "SAMPLE_COVERAGE", 0x80A0 },
    { "SAMPLE_COVERAGE_INVERT", 0x80AB }, { "SAMPLE_COVERAGE_VALUE", 0x80AA },
    { "SCISSOR_BOX", 0xC10 }, { "SCISSOR_TEST", 0xC11 },
    { "SHADER_TYPE", 0x8B4F }, { "SHADING_LANGUAGE_VERSION", 0x8B8C },
    { "SHORT", 0x1402 }, { "SRC_ALPHA", 0x302 },
    { "SRC_ALPHA_SATURATE", 0x308 }, { "SRC_COLOR", 0x300 },
    { "STATIC_DRAW", 0x88E4 }, { "STENCIL_ATTACHMENT", 0x8D20 },
    { "STENCIL_BACK_FAIL", 0x8801 }, { "STENCIL_BACK_FUNC", 0x8800 },
    { "STENCIL_BACK_PASS_DEPTH_FAIL", 0x8802 },
    { "STENCIL_BACK_PASS_DEPTH_PASS", 0x8803 },
    { "STENCIL_BACK_REF", 0x8CA3 }, { "STENCIL_BACK_VALUE_MASK", 0x8CA4 },
    { "STENCIL_BACK_WRITEMASK", 0x8CA5 }, { "STENCIL_BITS", 0xD57 },
    { "STENCIL_BUFFER_BIT", 0x400 }, { "STENCIL_CLEAR_VALUE", 0xB91 },
    { "STENCIL_FAIL", 0xB94 }, { "STENCIL_FUNC", 0xB92 },
    { "STENCIL_INDEX8", 0x8D48 }, { "STENCIL_PASS_DEPTH_FAIL", 0xB95 },
    { "STENCIL_PASS_DEPTH_PASS", 0xB96 }, { "STENCIL_REF", 0xB97 },
    { "STENCIL_TEST", 0xB90 }, { "STENCIL_VALUE_MASK", 0xB93 },
    { "STENCIL_WRITEMASK", 0xB98 }, { "STREAM_DRAW", 0x88E0 },
    { "SUBPIXEL_BITS", 0xD50 }, { "TEXTURE", 0x1702 }, { "TEXTURE0", 0x84C0 },
    { "TEXTURE1", 0x84C1 }, { "TEXTURE10", 0x84CA }, { "TEXTURE11", 0x84CB },
    { "TEXTURE12", 0x84CC }, { "TEXTURE13", 0x84CD }, { "TEXTURE14", 0x84CE },
    { "TEXTURE15", 0x84CF }, { "TEXTURE16", 0x84D0 }, { "TEXTURE17", 0x84D1 },
    { "TEXTURE18", 0x84D2 }, { "TEXTURE19", 0x84D3 }, { "TEXTURE2", 0x84C2 },
    { "TEXTURE20", 0x84D4 }, { "TEXTURE21", 0x84D5 }, { "TEXTURE22", 0x84D6 },
    { "TEXTURE23", 0x84D7 }, { "TEXTURE24", 0x84D8 }, { "TEXTURE25", 0x84D9 },
    { "TEXTURE26", 0x84DA }, { "TEXTURE27", 0x84DB }, { "TEXTURE28", 0x84DC },
    { "TEXTURE29", 0x84DD }, { "TEXTURE3", 0x84C3 }, { "TEXTURE30", 0x84DE },
    { "TEXTURE31", 0x84DF }, { "TEXTURE4", 0x84C4 }, { "TEXTURE5", 0x84C5 },
    { "TEXTURE6", 0x84C6 }, { "TEXTURE7", 0x84C7 }, { "TEXTURE8", 0x84C8 },
    { "TEXTURE9", 0x84C9 }, { "TEXTURE_2D", 0xDE1 },
    { "TEXTURE_BINDING_2D", 0x8069 }, { "TEXTURE_BINDING_CUBE_MAP", 0x8514 },
    { "TEXTURE_CUBE_MAP", 0x8513 }, { "TEXTURE_CUBE_MAP_NEGATIVE_X", 0x8516 },
    { "TEXTURE_CUBE_MAP_NEGATIVE_Y", 0x8518 },
    { "TEXTURE_CUBE_MAP_NEGATIVE_Z", 0x851A },
    { "TEXTURE_CUBE_MAP_POSITIVE_X", 0x8515 },
    { "TEXTURE_CUBE_MAP_POSITIVE_Y", 0x8517 },
    { "TEXTURE_CUBE_MAP_POSITIVE_Z", 0x8519 },
    { "TEXTURE_MAG_FILTER", 0x2800 }, { "TEXTURE_MIN_FILTER", 0x2801 },
    { "TEXTURE_WRAP_S", 0x2802 }, { "TEXTURE_WRAP_T", 0x2803 },
    { "TRIANGLES", 4 }, { "TRIANGLE_FAN", 6 }, { "TRIANGLE_STRIP", 5 },
    { "UNPACK_ALIGNMENT", 0xCF5 },
    { "UNPACK_COLORSPACE_CONVERSION_WEBGL", 0x9243 },
    { "UNPACK_FLIP_Y_WEBGL", 0x9240 },
    { "UNPACK_PREMULTIPLY_ALPHA_WEBGL", 0x9241 }, { "UNSIGNED_BYTE", 0x1401 },
    { "UNSIGNED_INT", 0x1405 }, { "UNSIGNED_SHORT", 0x1403 },
    { "UNSIGNED_SHORT_4_4_4_4", 0x8033 },
    { "UNSIGNED_SHORT_5_5_5_1", 0x8034 }, { "UNSIGNED_SHORT_5_6_5", 0x8363 },
    { "VALIDATE_STATUS", 0x8B83 }, { "VENDOR", 0x1F00 },
    { "VERSION", 0x1F02 }, { "VERTEX_ATTRIB_ARRAY_BUFFER_BINDING", 0x889F },
    { "VERTEX_ATTRIB_ARRAY_ENABLED", 0x8622 },
    { "VERTEX_ATTRIB_ARRAY_NORMALIZED", 0x886A },
    { "VERTEX_ATTRIB_ARRAY_POINTER", 0x8645 },
    { "VERTEX_ATTRIB_ARRAY_SIZE", 0x8623 },
    { "VERTEX_ATTRIB_ARRAY_STRIDE", 0x8624 },
    { "VERTEX_ATTRIB_ARRAY_TYPE", 0x8625 }, { "VERTEX_SHADER", 0x8B31 },
    { "VIEWPORT", 0xBA2 }, { "ZERO", 0 },
};

static const ns_gl_constant wgl2_constants[] = {
    { "ACTIVE_UNIFORM_BLOCKS", 0x8A36 }, { "ALREADY_SIGNALED", 0x911A },
    { "ANY_SAMPLES_PASSED", 0x8C2F },
    { "ANY_SAMPLES_PASSED_CONSERVATIVE", 0x8D6A }, { "COLOR", 0x1800 },
    { "COLOR_ATTACHMENT1", 0x8CE1 }, { "COLOR_ATTACHMENT10", 0x8CEA },
    { "COLOR_ATTACHMENT11", 0x8CEB }, { "COLOR_ATTACHMENT12", 0x8CEC },
    { "COLOR_ATTACHMENT13", 0x8CED }, { "COLOR_ATTACHMENT14", 0x8CEE },
    { "COLOR_ATTACHMENT15", 0x8CEF }, { "COLOR_ATTACHMENT2", 0x8CE2 },
    { "COLOR_ATTACHMENT3", 0x8CE3 }, { "COLOR_ATTACHMENT4", 0x8CE4 },
    { "COLOR_ATTACHMENT5", 0x8CE5 }, { "COLOR_ATTACHMENT6", 0x8CE6 },
    { "COLOR_ATTACHMENT7", 0x8CE7 }, { "COLOR_ATTACHMENT8", 0x8CE8 },
    { "COLOR_ATTACHMENT9", 0x8CE9 }, { "COMPARE_REF_TO_TEXTURE", 0x884E },
    { "CONDITION_SATISFIED", 0x911C }, { "COPY_READ_BUFFER", 0x8F36 },
    { "COPY_READ_BUFFER_BINDING", 0x8F36 }, { "COPY_WRITE_BUFFER", 0x8F37 },
    { "COPY_WRITE_BUFFER_BINDING", 0x8F37 }, { "CURRENT_QUERY", 0x8865 },
    { "DEPTH", 0x1801 }, { "DEPTH24_STENCIL8", 0x88F0 },
    { "DEPTH32F_STENCIL8", 0x8CAD }, { "DEPTH_COMPONENT24", 0x81A6 },
    { "DEPTH_COMPONENT32F", 0x8CAC }, { "DRAW_BUFFER0", 0x8825 },
    { "DRAW_BUFFER1", 0x8826 }, { "DRAW_BUFFER10", 0x882F },
    { "DRAW_BUFFER11", 0x8830 }, { "DRAW_BUFFER12", 0x8831 },
    { "DRAW_BUFFER13", 0x8832 }, { "DRAW_BUFFER14", 0x8833 },
    { "DRAW_BUFFER15", 0x8834 }, { "DRAW_BUFFER2", 0x8827 },
    { "DRAW_BUFFER3", 0x8828 }, { "DRAW_BUFFER4", 0x8829 },
    { "DRAW_BUFFER5", 0x882A }, { "DRAW_BUFFER6", 0x882B },
    { "DRAW_BUFFER7", 0x882C }, { "DRAW_BUFFER8", 0x882D },
    { "DRAW_BUFFER9", 0x882E }, { "DRAW_FRAMEBUFFER", 0x8CA9 },
    { "DRAW_FRAMEBUFFER_BINDING", 0x8CA6 }, { "DYNAMIC_COPY", 0x88EA },
    { "DYNAMIC_READ", 0x88E9 }, { "FLOAT_32_UNSIGNED_INT_24_8_REV", 0x8DAD },
    { "FLOAT_MAT2x3", 0x8B65 }, { "FLOAT_MAT2x4", 0x8B66 },
    { "FLOAT_MAT3x2", 0x8B67 }, { "FLOAT_MAT3x4", 0x8B68 },
    { "FLOAT_MAT4x2", 0x8B69 }, { "FLOAT_MAT4x3", 0x8B6A },
    { "FRAGMENT_SHADER_DERIVATIVE_HINT", 0x8B8B },
    { "FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE", 0x8215 },
    { "FRAMEBUFFER_ATTACHMENT_BLUE_SIZE", 0x8214 },
    { "FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING", 0x8210 },
    { "FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE", 0x8211 },
    { "FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE", 0x8216 },
    { "FRAMEBUFFER_ATTACHMENT_GREEN_SIZE", 0x8213 },
    { "FRAMEBUFFER_ATTACHMENT_RED_SIZE", 0x8212 },
    { "FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE", 0x8217 },
    { "FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER", 0x8CD4 },
    { "FRAMEBUFFER_DEFAULT", 0x8218 },
    { "FRAMEBUFFER_INCOMPLETE_MULTISAMPLE", 0x8D56 },
    { "HALF_FLOAT", 0x140B }, { "INTERLEAVED_ATTRIBS", 0x8C8C },
    { "INT_2_10_10_10_REV", 0x8D9F }, { "INT_SAMPLER_2D", 0x8DCA },
    { "INT_SAMPLER_2D_ARRAY", 0x8DCF }, { "INT_SAMPLER_3D", 0x8DCB },
    { "INT_SAMPLER_CUBE", 0x8DCC }, { "INVALID_INDEX", 0xFFFFFFFF },
    { "MAX", 0x8008 }, { "MAX_3D_TEXTURE_SIZE", 0x8073 },
    { "MAX_ARRAY_TEXTURE_LAYERS", 0x88FF },
    { "MAX_CLIENT_WAIT_TIMEOUT_WEBGL", 0x9247 },
    { "MAX_COLOR_ATTACHMENTS", 0x8CDF },
    { "MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS", 0x8A33 },
    { "MAX_COMBINED_UNIFORM_BLOCKS", 0x8A2E },
    { "MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS", 0x8A31 },
    { "MAX_DRAW_BUFFERS", 0x8824 }, { "MAX_ELEMENTS_INDICES", 0x80E9 },
    { "MAX_ELEMENTS_VERTICES", 0x80E8 }, { "MAX_ELEMENT_INDEX", 0x8D6B },
    { "MAX_FRAGMENT_INPUT_COMPONENTS", 0x9125 },
    { "MAX_FRAGMENT_UNIFORM_BLOCKS", 0x8A2D },
    { "MAX_FRAGMENT_UNIFORM_COMPONENTS", 0x8B49 },
    { "MAX_PROGRAM_TEXEL_OFFSET", 0x8905 }, { "MAX_SAMPLES", 0x8D57 },
    { "MAX_SERVER_WAIT_TIMEOUT", 0x9111 }, { "MAX_TEXTURE_LOD_BIAS", 0x84FD },
    { "MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS", 0x8C8A },
    { "MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS", 0x8C8B },
    { "MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS", 0x8C80 },
    { "MAX_UNIFORM_BLOCK_SIZE", 0x8A30 },
    { "MAX_UNIFORM_BUFFER_BINDINGS", 0x8A2F },
    { "MAX_VARYING_COMPONENTS", 0x8B4B },
    { "MAX_VERTEX_OUTPUT_COMPONENTS", 0x9122 },
    { "MAX_VERTEX_UNIFORM_BLOCKS", 0x8A2B },
    { "MAX_VERTEX_UNIFORM_COMPONENTS", 0x8B4A }, { "MIN", 0x8007 },
    { "MIN_PROGRAM_TEXEL_OFFSET", 0x8904 }, { "OBJECT_TYPE", 0x9112 },
    { "PACK_ROW_LENGTH", 0xD02 }, { "PACK_SKIP_PIXELS", 0xD04 },
    { "PACK_SKIP_ROWS", 0xD03 }, { "PIXEL_PACK_BUFFER", 0x88EB },
    { "PIXEL_PACK_BUFFER_BINDING", 0x88ED },
    { "PIXEL_UNPACK_BUFFER", 0x88EC },
    { "PIXEL_UNPACK_BUFFER_BINDING", 0x88EF }, { "QUERY_RESULT", 0x8866 },
    { "QUERY_RESULT_AVAILABLE", 0x8867 }, { "R11F_G11F_B10F", 0x8C3A },
    { "R16F", 0x822D }, { "R16I", 0x8233 }, { "R16UI", 0x8234 },
    { "R32F", 0x822E }, { "R32I", 0x8235 }, { "R32UI", 0x8236 },
    { "R8", 0x8229 }, { "R8I", 0x8231 }, { "R8UI", 0x8232 },
    { "R8_SNORM", 0x8F94 }, { "RASTERIZER_DISCARD", 0x8C89 },
    { "READ_BUFFER", 0xC02 }, { "READ_FRAMEBUFFER", 0x8CA8 },
    { "READ_FRAMEBUFFER_BINDING", 0x8CAA }, { "RED", 0x1903 },
    { "RED_INTEGER", 0x8D94 }, { "RENDERBUFFER_SAMPLES", 0x8CAB },
    { "RG", 0x8227 }, { "RG16F", 0x822F }, { "RG16I", 0x8239 },
    { "RG16UI", 0x823A }, { "RG32F", 0x8230 }, { "RG32I", 0x823B },
    { "RG32UI", 0x823C }, { "RG8", 0x822B }, { "RG8I", 0x8237 },
    { "RG8UI", 0x8238 }, { "RG8_SNORM", 0x8F95 }, { "RGB10_A2", 0x8059 },
    { "RGB10_A2UI", 0x906F }, { "RGB16F", 0x881B }, { "RGB16I", 0x8D89 },
    { "RGB16UI", 0x8D77 }, { "RGB32F", 0x8815 }, { "RGB32I", 0x8D83 },
    { "RGB32UI", 0x8D71 }, { "RGB8I", 0x8D8F }, { "RGB8UI", 0x8D7D },
    { "RGB8_SNORM", 0x8F96 }, { "RGB9_E5", 0x8C3D }, { "RGBA16F", 0x881A },
    { "RGBA16I", 0x8D88 }, { "RGBA16UI", 0x8D76 }, { "RGBA32F", 0x8814 },
    { "RGBA32I", 0x8D82 }, { "RGBA32UI", 0x8D70 }, { "RGBA8I", 0x8D8E },
    { "RGBA8UI", 0x8D7C }, { "RGBA8_SNORM", 0x8F97 },
    { "RGBA_INTEGER", 0x8D99 }, { "RGB_INTEGER", 0x8D98 },
    { "RG_INTEGER", 0x8228 }, { "SAMPLER_2D_ARRAY", 0x8DC1 },
    { "SAMPLER_2D_ARRAY_SHADOW", 0x8DC4 }, { "SAMPLER_2D_SHADOW", 0x8B62 },
    { "SAMPLER_3D", 0x8B5F }, { "SAMPLER_BINDING", 0x8919 },
    { "SAMPLER_CUBE_SHADOW", 0x8DC5 }, { "SEPARATE_ATTRIBS", 0x8C8D },
    { "SIGNALED", 0x9119 }, { "SIGNED_NORMALIZED", 0x8F9C },
    { "SRGB", 0x8C40 }, { "SRGB8", 0x8C41 }, { "SRGB8_ALPHA8", 0x8C43 },
    { "STATIC_COPY", 0x88E6 }, { "STATIC_READ", 0x88E5 },
    { "STENCIL", 0x1802 }, { "STREAM_COPY", 0x88E2 },
    { "STREAM_READ", 0x88E1 }, { "SYNC_CONDITION", 0x9113 },
    { "SYNC_FENCE", 0x9116 }, { "SYNC_FLAGS", 0x9115 },
    { "SYNC_FLUSH_COMMANDS_BIT", 1 },
    { "SYNC_GPU_COMMANDS_COMPLETE", 0x9117 }, { "SYNC_STATUS", 0x9114 },
    { "TEXTURE_2D_ARRAY", 0x8C1A }, { "TEXTURE_3D", 0x806F },
    { "TEXTURE_BASE_LEVEL", 0x813C }, { "TEXTURE_BINDING_2D_ARRAY", 0x8C1D },
    { "TEXTURE_BINDING_3D", 0x806A }, { "TEXTURE_COMPARE_FUNC", 0x884D },
    { "TEXTURE_COMPARE_MODE", 0x884C },
    { "TEXTURE_IMMUTABLE_FORMAT", 0x912F },
    { "TEXTURE_IMMUTABLE_LEVELS", 0x82DF }, { "TEXTURE_MAX_LEVEL", 0x813D },
    { "TEXTURE_MAX_LOD", 0x813B }, { "TEXTURE_MIN_LOD", 0x813A },
    { "TEXTURE_WRAP_R", 0x8072 }, { "TIMEOUT_EXPIRED", 0x911B },
    { "TIMEOUT_IGNORED", -1 }, { "TRANSFORM_FEEDBACK", 0x8E22 },
    { "TRANSFORM_FEEDBACK_ACTIVE", 0x8E24 },
    { "TRANSFORM_FEEDBACK_BINDING", 0x8E25 },
    { "TRANSFORM_FEEDBACK_BUFFER", 0x8C8E },
    { "TRANSFORM_FEEDBACK_BUFFER_BINDING", 0x8C8F },
    { "TRANSFORM_FEEDBACK_BUFFER_MODE", 0x8C7F },
    { "TRANSFORM_FEEDBACK_BUFFER_SIZE", 0x8C85 },
    { "TRANSFORM_FEEDBACK_BUFFER_START", 0x8C84 },
    { "TRANSFORM_FEEDBACK_PAUSED", 0x8E23 },
    { "TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN", 0x8C88 },
    { "TRANSFORM_FEEDBACK_VARYINGS", 0x8C83 },
    { "UNIFORM_ARRAY_STRIDE", 0x8A3C },
    { "UNIFORM_BLOCK_ACTIVE_UNIFORMS", 0x8A42 },
    { "UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES", 0x8A43 },
    { "UNIFORM_BLOCK_BINDING", 0x8A3F },
    { "UNIFORM_BLOCK_DATA_SIZE", 0x8A40 }, { "UNIFORM_BLOCK_INDEX", 0x8A3A },
    { "UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER", 0x8A46 },
    { "UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER", 0x8A44 },
    { "UNIFORM_BUFFER", 0x8A11 }, { "UNIFORM_BUFFER_BINDING", 0x8A28 },
    { "UNIFORM_BUFFER_OFFSET_ALIGNMENT", 0x8A34 },
    { "UNIFORM_BUFFER_SIZE", 0x8A2A }, { "UNIFORM_BUFFER_START", 0x8A29 },
    { "UNIFORM_IS_ROW_MAJOR", 0x8A3E }, { "UNIFORM_MATRIX_STRIDE", 0x8A3D },
    { "UNIFORM_OFFSET", 0x8A3B }, { "UNIFORM_SIZE", 0x8A38 },
    { "UNIFORM_TYPE", 0x8A37 }, { "UNPACK_IMAGE_HEIGHT", 0x806E },
    { "UNPACK_ROW_LENGTH", 0xCF2 }, { "UNPACK_SKIP_IMAGES", 0x806D },
    { "UNPACK_SKIP_PIXELS", 0xCF4 }, { "UNPACK_SKIP_ROWS", 0xCF3 },
    { "UNSIGNALED", 0x9118 }, { "UNSIGNED_INT_10F_11F_11F_REV", 0x8C3B },
    { "UNSIGNED_INT_24_8", 0x84FA },
    { "UNSIGNED_INT_2_10_10_10_REV", 0x8368 },
    { "UNSIGNED_INT_5_9_9_9_REV", 0x8C3E },
    { "UNSIGNED_INT_SAMPLER_2D", 0x8DD2 },
    { "UNSIGNED_INT_SAMPLER_2D_ARRAY", 0x8DD7 },
    { "UNSIGNED_INT_SAMPLER_3D", 0x8DD3 },
    { "UNSIGNED_INT_SAMPLER_CUBE", 0x8DD4 }, { "UNSIGNED_INT_VEC2", 0x8DC6 },
    { "UNSIGNED_INT_VEC3", 0x8DC7 }, { "UNSIGNED_INT_VEC4", 0x8DC8 },
    { "UNSIGNED_NORMALIZED", 0x8C17 }, { "VERTEX_ARRAY_BINDING", 0x85B5 },
    { "VERTEX_ATTRIB_ARRAY_DIVISOR", 0x88FE },
    { "VERTEX_ATTRIB_ARRAY_INTEGER", 0x88FD }, { "WAIT_FAILED", 0x911D },
};

static void
wgl_define_constants(JSContext *ctx, JSValueConst obj,
                     const ns_gl_constant *table, size_t count)
{
    for (size_t i = 0; i < count; i++)
        JS_DefinePropertyValueStr(ctx, obj, table[i].name,
                                  JS_NewInt64(ctx, table[i].value),
                                  JS_PROP_ENUMERABLE);
}

typedef struct ns_gl_method {
    const char  *name;
    JSCFunction *fn;
    int          length;
} ns_gl_method;

static const ns_gl_method wgl_methods[] = {
    { "getContextAttributes", wgl_getContextAttributes, 0 },
    { "isContextLost", wgl_isContextLost, 0 },
    { "getSupportedExtensions", wgl_getSupportedExtensions, 0 },
    { "getExtension", wgl_getExtension, 1 },
    { "getParameter", wgl_getParameter, 1 },
    { "getError", wgl_getError, 0 },
    { "clearColor", wgl_clearColor, 4 },
    { "clearDepth", wgl_clearDepth, 1 },
    { "clearStencil", wgl_clearStencil, 1 },
    { "clear", wgl_clear, 1 },
    { "viewport", wgl_viewport, 4 },
    { "scissor", wgl_scissor, 4 },
    { "enable", wgl_enable, 1 },
    { "disable", wgl_disable, 1 },
    { "isEnabled", wgl_isEnabled, 1 },
    { "depthFunc", wgl_depthFunc, 1 },
    { "depthMask", wgl_depthMask, 1 },
    { "depthRange", wgl_depthRange, 2 },
    { "colorMask", wgl_colorMask, 4 },
    { "stencilMask", wgl_stencilMask, 1 },
    { "stencilFunc", wgl_stencilFunc, 3 },
    { "stencilOp", wgl_stencilOp, 3 },
    { "blendFunc", wgl_blendFunc, 2 },
    { "blendFuncSeparate", wgl_blendFuncSeparate, 4 },
    { "blendEquation", wgl_blendEquation, 1 },
    { "blendEquationSeparate", wgl_blendEquationSeparate, 2 },
    { "blendColor", wgl_blendColor, 4 },
    { "cullFace", wgl_cullFace, 1 },
    { "frontFace", wgl_frontFace, 1 },
    { "lineWidth", wgl_lineWidth, 1 },
    { "polygonOffset", wgl_polygonOffset, 2 },
    { "hint", wgl_hint, 2 },
    { "finish", wgl_finish, 0 },
    { "flush", wgl_flush, 0 },
    { "pixelStorei", wgl_pixelStorei, 2 },
    { "sampleCoverage", wgl_sampleCoverage, 2 },
    { "stencilFuncSeparate", wgl_stencilFuncSeparate, 4 },
    { "stencilOpSeparate", wgl_stencilOpSeparate, 4 },
    { "stencilMaskSeparate", wgl_stencilMaskSeparate, 2 },
    { "activeTexture", wgl_activeTexture, 1 },
    { "createShader", wgl_createShader, 1 },
    { "deleteShader", wgl_deleteShader, 1 },
    { "shaderSource", wgl_shaderSource, 2 },
    { "compileShader", wgl_compileShader, 1 },
    { "getShaderParameter", wgl_getShaderParameter, 2 },
    { "getShaderInfoLog", wgl_getShaderInfoLog, 1 },
    { "getShaderSource", wgl_getShaderSource, 1 },
    { "createProgram", wgl_createProgram, 0 },
    { "deleteProgram", wgl_deleteProgram, 1 },
    { "attachShader", wgl_attachShader, 2 },
    { "detachShader", wgl_detachShader, 2 },
    { "linkProgram", wgl_linkProgram, 1 },
    { "validateProgram", wgl_validateProgram, 1 },
    { "useProgram", wgl_useProgram, 1 },
    { "getProgramParameter", wgl_getProgramParameter, 2 },
    { "getProgramInfoLog", wgl_getProgramInfoLog, 1 },
    { "bindAttribLocation", wgl_bindAttribLocation, 3 },
    { "getAttribLocation", wgl_getAttribLocation, 2 },
    { "getUniformLocation", wgl_getUniformLocation, 2 },
    { "getActiveAttrib", wgl_getActiveAttrib, 2 },
    { "getShaderPrecisionFormat", wgl_getShaderPrecisionFormat, 2 },
    { "getActiveUniform", wgl_getActiveUniform, 2 },
    { "createBuffer", wgl_createBuffer, 0 },
    { "deleteBuffer", wgl_deleteBuffer, 1 },
    { "bindBuffer", wgl_bindBuffer, 2 },
    { "bufferData", wgl_bufferData, 3 },
    { "bufferSubData", wgl_bufferSubData, 3 },
    { "enableVertexAttribArray", wgl_enableVertexAttribArray, 1 },
    { "disableVertexAttribArray", wgl_disableVertexAttribArray, 1 },
    { "vertexAttribPointer", wgl_vertexAttribPointer, 6 },
    { "vertexAttrib1f", wgl_vertexAttrib1f, 2 },
    { "vertexAttrib2f", wgl_vertexAttrib2f, 3 },
    { "vertexAttrib3f", wgl_vertexAttrib3f, 4 },
    { "vertexAttrib4f", wgl_vertexAttrib4f, 5 },
    { "uniform1f", wgl_uniform1f, 2 },
    { "uniform2f", wgl_uniform2f, 3 },
    { "uniform3f", wgl_uniform3f, 4 },
    { "uniform4f", wgl_uniform4f, 5 },
    { "uniform1i", wgl_uniform1i, 2 },
    { "uniform2i", wgl_uniform2i, 3 },
    { "uniform3i", wgl_uniform3i, 4 },
    { "uniform4i", wgl_uniform4i, 5 },
    { "uniform1fv", wgl_uniform1fv, 2 },
    { "uniform2fv", wgl_uniform2fv, 2 },
    { "uniform3fv", wgl_uniform3fv, 2 },
    { "uniform4fv", wgl_uniform4fv, 2 },
    { "uniform1iv", wgl_uniform1iv, 2 },
    { "uniform2iv", wgl_uniform2iv, 2 },
    { "uniform3iv", wgl_uniform3iv, 2 },
    { "uniform4iv", wgl_uniform4iv, 2 },
    { "uniformMatrix2fv", wgl_uniformMatrix2fv, 3 },
    { "uniformMatrix3fv", wgl_uniformMatrix3fv, 3 },
    { "uniformMatrix4fv", wgl_uniformMatrix4fv, 3 },
    { "drawArrays", wgl_drawArrays, 3 },
    { "drawElements", wgl_drawElements, 4 },
    { "createTexture", wgl_createTexture, 0 },
    { "deleteTexture", wgl_deleteTexture, 1 },
    { "bindTexture", wgl_bindTexture, 2 },
    { "texParameteri", wgl_texParameteri, 3 },
    { "texParameterf", wgl_texParameterf, 3 },
    { "generateMipmap", wgl_generateMipmap, 1 },
    { "texImage2D", wgl_texImage2D, 6 },
    { "texSubImage2D", wgl_texSubImage2D, 7 },
    { "createFramebuffer", wgl_createFramebuffer, 0 },
    { "deleteFramebuffer", wgl_deleteFramebuffer, 1 },
    { "bindFramebuffer", wgl_bindFramebuffer, 2 },
    { "framebufferTexture2D", wgl_framebufferTexture2D, 5 },
    { "framebufferRenderbuffer", wgl_framebufferRenderbuffer, 4 },
    { "checkFramebufferStatus", wgl_checkFramebufferStatus, 1 },
    { "createRenderbuffer", wgl_createRenderbuffer, 0 },
    { "deleteRenderbuffer", wgl_deleteRenderbuffer, 1 },
    { "bindRenderbuffer", wgl_bindRenderbuffer, 2 },
    { "renderbufferStorage", wgl_renderbufferStorage, 4 },
    { "readPixels", wgl_readPixels, 7 },
    { "isBuffer", wgl_isBuffer, 1 },
    { "isProgram", wgl_isProgram, 1 },
    { "isShader", wgl_isShader, 1 },
    { "isTexture", wgl_isTexture, 1 },
    { "isFramebuffer", wgl_isFramebuffer, 1 },
    { "isRenderbuffer", wgl_isRenderbuffer, 1 },
    { "getBufferParameter", wgl_getBufferParameter, 2 },
    { "getTexParameter", wgl_getTexParameter, 2 },
    { "getRenderbufferParameter", wgl_getRenderbufferParameter, 2 },
    { "getFramebufferAttachmentParameter", wgl_getFramebufferAttachmentParameter, 3 },
    { "getVertexAttrib", wgl_getVertexAttrib, 2 },
    { "getVertexAttribOffset", wgl_getVertexAttribOffset, 2 },
    { "getUniform", wgl_getUniform, 2 },
    { "vertexAttrib1fv", wgl_vertexAttrib1fv, 2 },
    { "vertexAttrib2fv", wgl_vertexAttrib2fv, 2 },
    { "vertexAttrib3fv", wgl_vertexAttrib3fv, 2 },
    { "vertexAttrib4fv", wgl_vertexAttrib4fv, 2 },
    { "compressedTexImage2D", wgl_compressedTexImage2D, 7 },
    { "compressedTexSubImage2D", wgl_compressedTexSubImage2D, 8 },
    { "getAttachedShaders", wgl_getAttachedShaders, 1 },
    { "drawingBufferStorage", wgl_drawingBufferStorage, 3 },
    { "makeXRCompatible", wgl_makeXRCompatible, 0 },
    { "copyTexImage2D", wgl_copyTexImage2D, 8 },
    { "copyTexSubImage2D", wgl_copyTexSubImage2D, 8 },
};

static const ns_gl_method wgl2_methods[] = {
    { "createVertexArray", wgl_createVertexArray, 0 },
    { "deleteVertexArray", wgl_deleteVertexArray, 1 },
    { "bindVertexArray", wgl_bindVertexArray, 1 },
    { "isVertexArray", wgl_isVertexArray, 1 },
    { "drawArraysInstanced", wgl_drawArraysInstanced, 4 },
    { "drawElementsInstanced", wgl_drawElementsInstanced, 5 },
    { "vertexAttribDivisor", wgl_vertexAttribDivisor, 2 },
    { "drawBuffers", wgl_drawBuffers, 1 },
    { "vertexAttribIPointer", wgl_vertexAttribIPointer, 5 },
    { "uniform1ui", wgl_uniform1ui, 2 },
    { "uniform2ui", wgl_uniform2ui, 3 },
    { "uniform3ui", wgl_uniform3ui, 4 },
    { "uniform4ui", wgl_uniform4ui, 5 },
    { "uniform1uiv", wgl_uniform1uiv, 2 },
    { "uniform2uiv", wgl_uniform2uiv, 2 },
    { "uniform3uiv", wgl_uniform3uiv, 2 },
    { "uniform4uiv", wgl_uniform4uiv, 2 },
    { "uniformMatrix2x3fv", wgl_uniformMatrix2x3fv, 3 },
    { "uniformMatrix3x2fv", wgl_uniformMatrix3x2fv, 3 },
    { "uniformMatrix2x4fv", wgl_uniformMatrix2x4fv, 3 },
    { "uniformMatrix4x2fv", wgl_uniformMatrix4x2fv, 3 },
    { "uniformMatrix3x4fv", wgl_uniformMatrix3x4fv, 3 },
    { "uniformMatrix4x3fv", wgl_uniformMatrix4x3fv, 3 },
    { "texStorage2D", wgl_texStorage2D, 5 },
    { "renderbufferStorageMultisample", wgl_renderbufferStorageMultisample, 5 },
    { "blitFramebuffer", wgl_blitFramebuffer, 10 },
    { "framebufferTextureLayer", wgl_framebufferTextureLayer, 5 },
    { "invalidateFramebuffer", wgl_invalidateFramebuffer, 2 },
    { "readBuffer", wgl_readBuffer, 1 },
    { "copyBufferSubData", wgl_copyBufferSubData, 5 },
    { "getBufferSubData", wgl_getBufferSubData, 3 },
    { "clearBufferfv", wgl_clearBuffer_fv, 3 },
    { "clearBufferiv", wgl_clearBuffer_iv, 3 },
    { "clearBufferuiv", wgl_clearBuffer_uiv, 3 },
    { "clearBufferfi", wgl_clearBufferfi, 4 },
    { "createSampler", wgl_createSampler, 0 },
    { "deleteSampler", wgl_deleteSampler, 1 },
    { "bindSampler", wgl_bindSampler, 2 },
    { "samplerParameteri", wgl_samplerParameteri, 3 },
    { "samplerParameterf", wgl_samplerParameterf, 3 },
    { "isSampler", wgl_isSampler, 1 },
    { "getUniformBlockIndex", wgl_getUniformBlockIndex, 2 },
    { "uniformBlockBinding", wgl_uniformBlockBinding, 3 },
    { "bindBufferBase", wgl_bindBufferBase, 3 },
    { "bindBufferRange", wgl_bindBufferRange, 5 },
    { "copyTexSubImage3D", wgl_copyTexSubImage3D, 9 },
    { "drawRangeElements", wgl_drawRangeElements, 6 },
    { "vertexAttribI4i", wgl_vertexAttribI4i, 5 },
    { "vertexAttribI4ui", wgl_vertexAttribI4ui, 5 },
    { "vertexAttribI4iv", wgl_vertexAttribI4iv, 2 },
    { "vertexAttribI4uiv", wgl_vertexAttribI4uiv, 2 },
    { "getFragDataLocation", wgl_getFragDataLocation, 2 },
    { "getInternalformatParameter", wgl_getInternalformatParameter, 3 },
    { "texImage3D", wgl_texImage3D, 10 },
    { "texSubImage3D", wgl_texSubImage3D, 11 },
    { "texStorage3D", wgl_texStorage3D, 6 },
    { "createQuery", wgl_createQuery, 0 },
    { "deleteQuery", wgl_deleteQuery, 1 },
    { "isQuery", wgl_isQuery, 1 },
    { "beginQuery", wgl_beginQuery, 2 },
    { "endQuery", wgl_endQuery, 1 },
    { "getQuery", wgl_getQuery, 2 },
    { "getQueryParameter", wgl_getQueryParameter, 2 },
    { "createTransformFeedback", wgl_createTransformFeedback, 0 },
    { "deleteTransformFeedback", wgl_deleteTransformFeedback, 1 },
    { "isTransformFeedback", wgl_isTransformFeedback, 1 },
    { "bindTransformFeedback", wgl_bindTransformFeedback, 2 },
    { "beginTransformFeedback", wgl_beginTransformFeedback, 1 },
    { "endTransformFeedback", wgl_endTransformFeedback, 0 },
    { "pauseTransformFeedback", wgl_pauseTransformFeedback, 0 },
    { "resumeTransformFeedback", wgl_resumeTransformFeedback, 0 },
    { "transformFeedbackVaryings", wgl_transformFeedbackVaryings, 3 },
    { "getActiveUniforms", wgl_getActiveUniforms, 3 },
    { "getActiveUniformBlockParameter", wgl_getActiveUniformBlockParameter, 3 },
    { "getActiveUniformBlockName", wgl_getActiveUniformBlockName, 2 },
    { "fenceSync", wgl_fenceSync, 2 },
    { "isSync", wgl_isSync, 1 },
    { "deleteSync", wgl_deleteSync, 1 },
    { "clientWaitSync", wgl_clientWaitSync, 3 },
    { "waitSync", wgl_waitSync, 3 },
    { "getSyncParameter", wgl_getSyncParameter, 2 },
    { "compressedTexImage3D", wgl_compressedTexImage3D, 8 },
    { "compressedTexSubImage3D", wgl_compressedTexSubImage3D, 10 },
    { "getIndexedParameter", wgl_getIndexedParameter, 2 },
    { "getSamplerParameter", wgl_getSamplerParameter, 2 },
    { "getTransformFeedbackVarying", wgl_getTransformFeedbackVarying, 2 },
    { "getUniformIndices", wgl_getUniformIndices, 2 },
    { "invalidateSubFramebuffer", wgl_invalidateSubFramebuffer, 6 },
};

#define WGL_V2_METHODS 0x4000

static JSValue
wgl_dispatch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
             int magic)
{
    const ns_gl_method *m = magic >= WGL_V2_METHODS
        ? &wgl2_methods[magic - WGL_V2_METHODS] : &wgl_methods[magic];
    if (argc < m->length) {
        ns_webgl *g = wgl_brand(ctx, this_val);
        if (!g) return JS_EXCEPTION;
        return JS_ThrowTypeError(ctx,
            "Failed to execute '%s' on '%s': %d argument%s required, but only %d present.",
            m->name, g->version >= 2 ? "WebGL2RenderingContext" : "WebGLRenderingContext",
            m->length, m->length == 1 ? "" : "s", argc);
    }
    return m->fn(ctx, this_val, argc, argv);
}

static void
wgl_bind_methods(JSContext *ctx, JSValueConst proto, const ns_gl_method *table,
                 size_t count, int magic_base)
{
    for (size_t i = 0; i < count; i++)
        JS_SetPropertyStr(ctx, proto, table[i].name,
            JS_NewCFunctionMagic(ctx, wgl_dispatch, table[i].name, table[i].length,
                                 JS_CFUNC_generic_magic, magic_base + (int)i));
}

static JSValue
wgl_get_canvas(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_webgl *g = wgl_brand(ctx, this_val);
    return g ? JS_DupValue(ctx, g->canvas_obj) : JS_EXCEPTION;
}

static ns_webgl *
wgl_synced(JSContext *ctx, JSValueConst this_val)
{
    ns_webgl *g = wgl_brand(ctx, this_val);
    if (g && g->gl) {
        ns_gl_context_make_current(g->gl);
        ns_webgl_sync_size(g);
    }
    return g;
}

static JSValue
wgl_get_drawingBufferWidth(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_webgl *g = wgl_synced(ctx, this_val);
    return g ? JS_NewInt32(ctx, g->w) : JS_EXCEPTION;
}

static JSValue
wgl_get_drawingBufferHeight(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_webgl *g = wgl_synced(ctx, this_val);
    return g ? JS_NewInt32(ctx, g->h) : JS_EXCEPTION;
}

static JSValue
wgl_get_drawingBufferFormat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_webgl *g = wgl_brand(ctx, this_val);
    return g ? JS_NewInt32(ctx, g->alpha ? GL_RGBA8 : GL_RGB8) : JS_EXCEPTION;
}

static JSValue
wgl_get_drawingBufferColorSpace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_webgl *g = wgl_brand(ctx, this_val);
    if (!g) return JS_EXCEPTION;
    return JS_NewString(ctx, g->drawing_p3 ? "display-p3" : "srgb");
}

static JSValue
wgl_get_unpackColorSpace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_webgl *g = wgl_brand(ctx, this_val);
    if (!g) return JS_EXCEPTION;
    return JS_NewString(ctx, g->unpack_p3 ? "display-p3" : "srgb");
}

static JSValue
wgl_set_color_space(JSContext *ctx, JSValueConst this_val, JSValueConst v,
                    gboolean drawing)
{
    ns_webgl *g = wgl_brand(ctx, this_val);
    if (!g) return JS_EXCEPTION;
    const char *str = JS_ToCString(ctx, v);
    if (!str) return JS_EXCEPTION;
    gboolean *slot = drawing ? &g->drawing_p3 : &g->unpack_p3;
    if (strcmp(str, "srgb") == 0) *slot = FALSE;
    else if (strcmp(str, "display-p3") == 0) *slot = TRUE;
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static JSValue
wgl_set_drawingBufferColorSpace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return wgl_set_color_space(ctx, this_val, argc > 0 ? argv[0] : JS_UNDEFINED, TRUE);
}

static JSValue
wgl_set_unpackColorSpace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return wgl_set_color_space(ctx, this_val, argc > 0 ? argv[0] : JS_UNDEFINED, FALSE);
}

static void
wgl_define_accessor(JSContext *ctx, JSValueConst proto, const char *name,
                    JSCFunction *getter, JSCFunction *setter)
{
    char *get_name = g_strconcat("get ", name, NULL);
    char *set_name = g_strconcat("set ", name, NULL);
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, proto, atom,
        JS_NewCFunction2(ctx, getter, get_name, 0, JS_CFUNC_generic, 0),
        setter ? JS_NewCFunction2(ctx, setter, set_name, 1, JS_CFUNC_generic, 0)
               : JS_UNDEFINED,
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
    g_free(get_name);
    g_free(set_name);
}

static void
wgl_bind_accessors(JSContext *ctx, JSValueConst proto)
{
    wgl_define_accessor(ctx, proto, "canvas", wgl_get_canvas, NULL);
    wgl_define_accessor(ctx, proto, "drawingBufferWidth",
                        wgl_get_drawingBufferWidth, NULL);
    wgl_define_accessor(ctx, proto, "drawingBufferHeight",
                        wgl_get_drawingBufferHeight, NULL);
    wgl_define_accessor(ctx, proto, "drawingBufferFormat",
                        wgl_get_drawingBufferFormat, NULL);
    wgl_define_accessor(ctx, proto, "drawingBufferColorSpace",
                        wgl_get_drawingBufferColorSpace,
                        wgl_set_drawingBufferColorSpace);
    wgl_define_accessor(ctx, proto, "unpackColorSpace",
                        wgl_get_unpackColorSpace, wgl_set_unpackColorSpace);
}

static void
ns_webgl_install_interface(JSContext *ctx, JSValueConst ctor, JSValueConst proto,
                           int version)
{
    wgl_define_constants(ctx, ctor, wgl_constants, G_N_ELEMENTS(wgl_constants));
    wgl_define_constants(ctx, proto, wgl_constants, G_N_ELEMENTS(wgl_constants));
    wgl_bind_methods(ctx, proto, wgl_methods, G_N_ELEMENTS(wgl_methods), 0);
    if (version >= 2) {
        wgl_define_constants(ctx, ctor, wgl2_constants,
                             G_N_ELEMENTS(wgl2_constants));
        wgl_define_constants(ctx, proto, wgl2_constants,
                             G_N_ELEMENTS(wgl2_constants));
        wgl_bind_methods(ctx, proto, wgl2_methods, G_N_ELEMENTS(wgl2_methods),
                         WGL_V2_METHODS);
    }
    wgl_bind_accessors(ctx, proto);
}

static JSValue
wgl_new_context_object(JSContext *ctx, const ns_node *canvas, int version)
{
    JSContext *realm = ns_canvas_realm(ctx, canvas);
    JSValue proto = ns_api_proto(realm, version >= 2 ? "WebGL2RenderingContext"
                                                     : "WebGLRenderingContext");
    JSValue obj = JS_IsObject(proto)
        ? JS_NewObjectProtoClass(realm, proto, ns_webgl_class_id)
        : JS_NewObjectClass(realm, ns_webgl_class_id);
    JS_FreeValue(realm, proto);
    return obj;
}

static const char *const wgl_interface_names[] = {
    "WebGLObject", "WebGLBuffer", "WebGLFramebuffer", "WebGLProgram",
    "WebGLRenderbuffer", "WebGLShader", "WebGLTexture", "WebGLQuery",
    "WebGLSampler", "WebGLSync", "WebGLTransformFeedback",
    "WebGLVertexArrayObject", "WebGLUniformLocation", "WebGLActiveInfo",
    "WebGLShaderPrecisionFormat", "WebGLRenderingContext",
    "WebGL2RenderingContext",
};

static JSValue
wgl_illegal_constructor(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv, int magic)
{
    (void)argc; (void)argv;
    if (JS_IsUndefined(this_val)) return JS_ThrowTypeError(ctx, "Illegal constructor");
    return JS_ThrowTypeError(ctx, "Failed to construct '%s': Illegal constructor",
                             wgl_interface_names[magic]);
}

static const struct { int kind; const char *names[3]; } wgl_info_fields[] = {
    { NS_HK_ACTIVEINFO, { "name", "size", "type" } },
    { NS_HK_PRECISION, { "precision", "rangeMax", "rangeMin" } },
};

static JSValue
wgl_info_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
             int magic)
{
    (void)argc; (void)argv;
    int iface = magic >> 4;
    if (!ns_hidden_is(this_val, wgl_info_fields[iface].kind))
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    return ns_hget(ctx, this_val, wgl_info_fields[iface].names[magic & 15]);
}

static void
wgl_define_info_getters(JSContext *ctx, JSValueConst proto, int iface)
{
    for (int i = 0; i < 3; i++) {
        const char *name = wgl_info_fields[iface].names[i];
        char *get_name = g_strconcat("get ", name, NULL);
        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DefinePropertyGetSet(ctx, proto, atom,
            JS_NewCFunctionMagic(ctx, wgl_info_get, get_name, 0, JS_CFUNC_generic_magic,
                                 (iface << 4) | i),
            JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
        g_free(get_name);
    }
}

static JSValue
wgl_install_one(JSContext *ctx, JSValueConst global, int index, const char *parent,
                JSValue *proto_out)
{
    const char *name = wgl_interface_names[index];
    JSValue ctor = JS_NewCFunctionMagic(ctx, wgl_illegal_constructor, name, 0,
                                        JS_CFUNC_constructor_or_func_magic, index);
    *proto_out = ns_api_interface(ctx, global, name, JS_DupValue(ctx, ctor), parent);
    return ctor;
}

void
ns_webgl_install(JSContext *ctx, JSValueConst global)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    ns_new_class_id(&ns_webgl_obj_class_id);
    if (!JS_IsRegisteredClass(rt, ns_webgl_obj_class_id))
        JS_NewClass(rt, ns_webgl_obj_class_id, &ns_webgl_obj_class);
    JSValue proto;
    for (int i = 0; i <= 12; i++) {
        JS_FreeValue(ctx, wgl_install_one(ctx, global, i, i >= 1 && i <= 11 ? "WebGLObject" : NULL,
                                          &proto));
        JS_FreeValue(ctx, proto);
    }
    for (int i = 0; i < 2; i++) {
        JS_FreeValue(ctx, wgl_install_one(ctx, global, 13 + i, NULL, &proto));
        wgl_define_info_getters(ctx, proto, i);
        JS_FreeValue(ctx, proto);
    }
    for (int version = 1; version <= 2; version++) {
        JSValue ctor = wgl_install_one(ctx, global, 14 + version, NULL, &proto);
        ns_webgl_install_interface(ctx, ctor, proto, version);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, proto);
    }
}

JSValue
ns_webgl_get_context(JSContext *ctx, ns_js *js, JSValueConst canvas_obj,
                     const ns_node *canvas, int version, JSValueConst attrs)
{
    if (!g_webgl_by_node)
        g_webgl_by_node = g_hash_table_new(g_direct_hash, g_direct_equal);

    ns_webgl *existing = g_hash_table_lookup(g_webgl_by_node, canvas);
    if (existing)
        return JS_DupValue(ctx, existing->js_obj);

    if (g_hash_table_size(g_webgl_by_node) >= NS_WEBGL_MAX_CONTEXTS)
        return JS_NULL;

    if (!ns_webgl_permission(js))
        return JS_NULL;

    ns_new_class_id(&ns_webgl_class_id);
    if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), ns_webgl_class_id))
        JS_NewClass(JS_GetRuntime(ctx), ns_webgl_class_id, &ns_webgl_class);

    ns_webgl *g = ns_webgl_make(ctx, js, canvas, version, attrs);
    if (!g) return JS_NULL;

    JSValue obj = wgl_new_context_object(ctx, canvas, version);
    if (JS_IsException(obj)) {
        ns_webgl_free(g);
        return JS_NULL;
    }
    JS_SetOpaque(obj, g);
    g->ctx = ctx;
    g->js_obj = obj;
    g->canvas_obj = JS_DupValue(ctx, canvas_obj);

    g_hash_table_insert(g_webgl_by_node, (gpointer)canvas, g);
    return obj;
}

typedef struct wgl_pack_state {
    gboolean extended;
    GLint    align, row_length, skip_rows, skip_pixels, pack_buffer;
} wgl_pack_state;

static void
wgl_pack_tight(wgl_pack_state *s)
{
    s->extended = epoxy_is_desktop_gl() || epoxy_gl_version() >= 30;
    s->align = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &s->align);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (!s->extended) return;
    s->row_length = s->skip_rows = s->skip_pixels = s->pack_buffer = 0;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &s->row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &s->skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &s->skip_pixels);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &s->pack_buffer);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    if (s->pack_buffer) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

static void
wgl_pack_restore(const wgl_pack_state *s)
{
    glPixelStorei(GL_PACK_ALIGNMENT, s->align);
    if (!s->extended) return;
    glPixelStorei(GL_PACK_ROW_LENGTH, s->row_length);
    glPixelStorei(GL_PACK_SKIP_ROWS, s->skip_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, s->skip_pixels);
    if (s->pack_buffer) glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)s->pack_buffer);
}

static gboolean
wgl_flip_target(ns_webgl *g, int w, int h)
{
    if (g->flip_fbo && g->flip_w == w && g->flip_h == h) return TRUE;
    GLint saved_rb = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &saved_rb);
    if (!g->flip_rb) glGenRenderbuffers(1, &g->flip_rb);
    if (!g->flip_fbo) glGenFramebuffers(1, &g->flip_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, g->flip_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)saved_rb);
    wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->flip_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, g->flip_rb);
    gboolean ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                  GL_FRAMEBUFFER_COMPLETE;
    g->flip_w = ok ? w : 0;
    g->flip_h = ok ? h : 0;
    return ok;
}

static gboolean
wgl_readback_bgra(ns_webgl *g, int w, int h, unsigned char *dst, int stride)
{
    if (!epoxy_is_desktop_gl() || epoxy_gl_version() < 30) return FALSE;
    if (stride % 4 != 0) return FALSE;
    if (g->alpha && !g->premultiplied_alpha) return FALSE;
    if (!wgl_flip_target(g, w, h)) return FALSE;
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    wgl_bind_framebuffer(g, GL_READ_FRAMEBUFFER, g->fbo);
    wgl_bind_framebuffer(g, GL_DRAW_FRAMEBUFFER, g->flip_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (!g->alpha) {
        GLboolean mask[4];
        GLfloat clear[4];
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
    }
    if (scissor) glEnable(GL_SCISSOR_TEST);
    wgl_bind_framebuffer(g, GL_READ_FRAMEBUFFER, g->flip_fbo);
    wgl_pack_state pack;
    wgl_pack_tight(&pack);
    glPixelStorei(GL_PACK_ROW_LENGTH, stride / 4);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dst);
    wgl_pack_restore(&pack);
    wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->fbo);
    return TRUE;
}

#define NS_WEBGL_STREAM_GAP_US 50000

static gboolean
wgl_readback_streams(ns_webgl *g, int stride)
{
    return epoxy_is_desktop_gl() && epoxy_gl_version() >= 30 &&
           stride % 4 == 0 && (!g->alpha || g->premultiplied_alpha);
}

static gboolean
wgl_pbo_ensure(ns_webgl *g, size_t size)
{
    if (g->pbo[0] && g->pbo_size == size) return TRUE;
    wgl_pbo_release(g);
    GLint saved = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &saved);
    glGenBuffers(2, g->pbo);
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g->pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)size, NULL,
                     GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)saved);
    g->pbo_size = size;
    g->pbo_next = 0;
    return glGetError() == GL_NO_ERROR;
}

static gboolean
wgl_pbo_issue(ns_webgl *g, int w, int h, int stride)
{
    if (!wgl_flip_target(g, w, h)) return FALSE;
    if (!wgl_pbo_ensure(g, (size_t)stride * (size_t)h)) return FALSE;
    int slot = g->pbo_next;
    if (g->pbo_pending[slot]) {
        glDeleteSync(g->pbo_fence[slot]);
        g->pbo_fence[slot] = NULL;
        g->pbo_pending[slot] = FALSE;
    }
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    wgl_bind_framebuffer(g, GL_READ_FRAMEBUFFER, g->fbo);
    wgl_bind_framebuffer(g, GL_DRAW_FRAMEBUFFER, g->flip_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (!g->alpha) {
        GLboolean mask[4];
        GLfloat clear[4];
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
    }
    if (scissor) glEnable(GL_SCISSOR_TEST);
    wgl_bind_framebuffer(g, GL_READ_FRAMEBUFFER, g->flip_fbo);
    wgl_pack_state pack;
    wgl_pack_tight(&pack);
    glPixelStorei(GL_PACK_ROW_LENGTH, stride / 4);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g->pbo[slot]);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    wgl_pack_restore(&pack);
    wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->fbo);
    g->pbo_fence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    g->pbo_pending[slot] = g->pbo_fence[slot] != NULL;
    g->pbo_next = slot ^ 1;
    glFlush();
    return g->pbo_pending[slot];
}

static gboolean
wgl_pbo_collect(ns_webgl *g, int slot, unsigned char *dst, gboolean wait)
{
    if (!g->pbo_pending[slot]) return FALSE;
    GLenum st = glClientWaitSync(g->pbo_fence[slot], GL_SYNC_FLUSH_COMMANDS_BIT,
                                 wait ? (GLuint64)1000000000 : 0);
    if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED)
        return FALSE;
    glDeleteSync(g->pbo_fence[slot]);
    g->pbo_fence[slot] = NULL;
    g->pbo_pending[slot] = FALSE;
    GLint saved = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &saved);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g->pbo[slot]);
    const void *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                       (GLsizeiptr)g->pbo_size,
                                       GL_MAP_READ_BIT);
    if (src) {
        memcpy(dst, src, g->pbo_size);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)saved);
    return src != NULL;
}

static gboolean
wgl_pbo_timer_cb(gpointer user_data)
{
    ns_webgl *g = user_data;
    g_source_unref(g->pbo_timer);
    g->pbo_timer = NULL;
    ns_js_request_repaint(g->js);
    return G_SOURCE_REMOVE;
}

static void
wgl_pbo_schedule_collect(ns_webgl *g)
{
    if (g->pbo_timer) return;
    g->pbo_timer = g_timeout_source_new(4);
    g_source_set_callback(g->pbo_timer, wgl_pbo_timer_cb, g, NULL);
    g_source_attach(g->pbo_timer, g_main_context_get_thread_default());
}

static gboolean
wgl_readback_streaming(ns_webgl *g, int w, int h, unsigned char *dst,
                       int stride)
{
    gint64 now = g_get_monotonic_time();
    gboolean streaming = g->dirty && g->last_readback_us > 0 &&
                         now - g->last_readback_us < NS_WEBGL_STREAM_GAP_US;
    if (g->dirty) g->last_readback_us = now;
    if (!g->pbo_pending[0] && !g->pbo_pending[1] && !streaming)
        return FALSE;
    gboolean issued = FALSE;
    if (g->dirty) {
        if (!streaming) {
            for (int i = 0; i < 2; i++) wgl_pbo_collect(g, i, dst, TRUE);
            return FALSE;
        }
        issued = wgl_pbo_issue(g, w, h, stride);
        if (!issued) return FALSE;
        g->dirty = FALSE;
        g->repaint_queued = FALSE;
    }
    int newest = g->pbo_next ^ 1;
    int oldest = g->pbo_next;
    gboolean copied = wgl_pbo_collect(g, oldest, dst, FALSE);
    copied = wgl_pbo_collect(g, newest, dst, FALSE) || copied;
    if (g->pbo_pending[0] || g->pbo_pending[1])
        wgl_pbo_schedule_collect(g);
    return copied || issued;
}

cairo_surface_t *
ns_webgl_canvas_surface(const ns_node *canvas)
{
    if (!g_webgl_by_node) return NULL;
    ns_webgl *g = g_hash_table_lookup(g_webgl_by_node, canvas);
    if (!g || !g->gl) return NULL;

    ns_gl_context_make_current(g->gl);
    ns_webgl_sync_size(g);

    int w = g->w, h = g->h;
    if (w <= 0 || h <= 0) return NULL;

    if (!g->dirty && g->surf && !g->pbo_pending[0] && !g->pbo_pending[1])
        return g->surf;

    if (g->samples > 1 && g->dirty) {
        wgl_bind_framebuffer(g, GL_READ_FRAMEBUFFER, g->draw_fbo);
        wgl_bind_framebuffer(g, GL_DRAW_FRAMEBUFFER, g->fbo);
        wgl_resolve_blit(w, h);
    }
    wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->fbo);

    if (!g->surf) {
        g->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (cairo_surface_status(g->surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(g->surf);
            g->surf = NULL;
            return NULL;
        }
    }

    cairo_surface_flush(g->surf);
    int stride = cairo_image_surface_get_stride(g->surf);
    unsigned char *dst = cairo_image_surface_get_data(g->surf);

    if (wgl_readback_streams(g, stride) &&
        wgl_readback_streaming(g, w, h, dst, stride)) {
        cairo_surface_mark_dirty(g->surf);
        return g->surf;
    }
    if (!g->dirty) return g->surf;

    if (wgl_readback_bgra(g, w, h, dst, stride)) {
        cairo_surface_mark_dirty(g->surf);
        g->dirty = FALSE;
        g->repaint_queued = FALSE;
        return g->surf;
    }

    uint8_t *rgba = wgl_readback_buffer(g, (size_t)w * (size_t)h * 4);
    if (!rgba) return g->surf;
    wgl_pack_state pack;
    wgl_pack_tight(&pack);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    wgl_pack_restore(&pack);

    for (int y = 0; y < h; y++) {
        const uint8_t *src = rgba + (size_t)(h - 1 - y) * (size_t)w * 4;
        uint8_t *row = dst + (size_t)y * stride;
        if (!g->alpha) {
            wgl_copy_opaque_rgba_row(row, src, w, TRUE);
            continue;
        }
        {
            gboolean opaque = TRUE;
            for (int x = 0; x < w; x++) {
                if (src[x * 4 + 3] != 255u) {
                    opaque = FALSE;
                    break;
                }
            }
            if (opaque) {
                wgl_copy_opaque_rgba_row(row, src, w, FALSE);
                continue;
            }
        }
        for (int x = 0; x < w; x++) {
            unsigned r = src[x * 4 + 0];
            unsigned gg = src[x * 4 + 1];
            unsigned b = src[x * 4 + 2];
            unsigned a = src[x * 4 + 3];
            if (a == 255u || g->premultiplied_alpha) {
                row[x * 4 + 0] = (uint8_t)b;
                row[x * 4 + 1] = (uint8_t)gg;
                row[x * 4 + 2] = (uint8_t)r;
            } else {
                row[x * 4 + 0] = (uint8_t)((b * a + 127) / 255);
                row[x * 4 + 1] = (uint8_t)((gg * a + 127) / 255);
                row[x * 4 + 2] = (uint8_t)((r * a + 127) / 255);
            }
            row[x * 4 + 3] = (uint8_t)a;
        }
    }
    cairo_surface_mark_dirty(g->surf);
    g->dirty = FALSE;
    g->repaint_queued = FALSE;
    return g->surf;
}

#else

static JSValue
wgl_stub_constructor(JSContext *ctx, JSValueConst this_val, int argc,
                     JSValueConst *argv, int magic)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

JSValue
ns_webgl_get_context(JSContext *ctx, ns_js *js, JSValueConst canvas_obj,
                     const ns_node *canvas, int version, JSValueConst attrs)
{
    (void)ctx; (void)js; (void)canvas_obj; (void)canvas; (void)version;
    (void)attrs;
    return JS_NULL;
}

void
ns_webgl_install(JSContext *ctx, JSValueConst global)
{
    static const char *const names[2] = { "WebGLRenderingContext", "WebGL2RenderingContext" };
    for (int i = 0; i < 2; i++) {
        JSValue ctor = JS_NewCFunctionMagic(ctx, wgl_stub_constructor, names[i], 0,
                                            JS_CFUNC_constructor_or_func_magic, i);
        JS_FreeValue(ctx, ns_api_interface(ctx, global, names[i], ctor, NULL));
    }
}

cairo_surface_t *
ns_webgl_canvas_surface(const ns_node *canvas)
{
    (void)canvas;
    return NULL;
}

char *
ns_webgl_take_pending_origin(void)
{
    return NULL;
}

void
ns_webgl_set_decision(const char *origin, int allow)
{
    (void)origin; (void)allow;
}

void
ns_webgl_set_state(const char *origin, int state)
{
    (void)origin; (void)state;
}

void
ns_webgl_clear_decisions(void)
{
}

#endif
