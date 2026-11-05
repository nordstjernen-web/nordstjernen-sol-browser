/* Nordstjernen — experimental WebGPU (navigator.gpu) over wgpu-native.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "webgpu.h"
#include "js_classid.h"

#ifdef ND_HAVE_WEBGPU

#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

#include "webgpu/webgpu.h"
#include "webgpu/wgpu.h"
#include "js.h"
#include "webgl.h"
#include "config.h"

static WGPUInstance g_wg_instance;
static JSClassID g_adapter_class;
static JSClassID g_device_class;
static JSClassID g_queue_class;
static JSClassID g_buffer_class;
static JSClassID g_context_class;
static JSClassID g_texture_class;
static JSClassID g_view_class;
static JSClassID g_encoder_class;
static JSClassID g_pass_class;
static JSClassID g_cmdbuf_class;
static JSClassID g_shader_class;
static JSClassID g_pipeline_class;
static JSClassID g_bgl_class;
static JSClassID g_pllayout_class;
static JSClassID g_bindgroup_class;
static JSClassID g_sampler_class;
static JSClassID g_queryset_class;
static JSClassID g_compute_pipe_class;
static JSClassID g_compute_pass_class;
static JSClassID g_bundle_encoder_class;
static JSClassID g_bundle_class;

static GHashTable *g_webgpu_ctx_by_node;

#define NS_WG_MAX_COLOR_ATTACHMENTS 8

typedef struct { WGPUAdapter adapter; } ns_wg_adapter;
typedef struct ns_wg_error_sink {
    JSContext *ctx;
    JSValue    device_obj;
    GPtrArray *pending;
    gboolean   job_queued;
    guint      logged;
} ns_wg_error_sink;

typedef struct {
    WGPUDevice        device;
    WGPUQueue         queue;
    ns_wg_error_sink *sink;
    JSValue           lost_resolve;
    guint             error_scopes;
    GByteArray       *scope_internal;
    gboolean          destroyed;
} ns_wg_device;

typedef struct { WGPUErrorType type; char *message; } ns_wg_pending_error;
typedef struct { WGPUQueue queue; ns_wg_error_sink *sink; } ns_wg_queue;
typedef struct { WGPUBuffer buffer; uint64_t size; uint32_t usage; WGPUDevice device; GArray *mapped_ranges; gboolean range_escaped; gboolean mapped; uint64_t map_offset; uint64_t map_size; } ns_wg_buffer;
typedef struct { WGPUQuerySet qs; } ns_wg_queryset;
typedef struct { WGPUComputePipeline pipe; } ns_wg_compute_pipe;
typedef struct { WGPUComputePassEncoder pass; } ns_wg_compute_pass;
typedef struct { WGPUTexture texture; uint32_t w, h; WGPUTextureFormat format; } ns_wg_texture;
typedef struct { WGPUTextureView view; } ns_wg_view;
typedef struct {
    WGPUCommandEncoder enc;
    WGPUDevice         device;
    ns_wg_error_sink  *sink;
} ns_wg_encoder;
typedef struct { WGPURenderPassEncoder pass; } ns_wg_pass;
typedef struct { WGPUCommandBuffer cmd; } ns_wg_cmdbuf;
typedef struct { WGPUShaderModule mod; } ns_wg_shader;
typedef struct { WGPURenderPipeline pipe; } ns_wg_pipeline;
typedef struct { WGPUBindGroupLayout layout; } ns_wg_bgl;
typedef struct { WGPUPipelineLayout layout; } ns_wg_pllayout;
typedef struct { WGPUBindGroup group; } ns_wg_bindgroup;
typedef struct { WGPUSampler sampler; } ns_wg_sampler;
typedef struct { WGPURenderBundleEncoder enc; } ns_wg_bundle_encoder;
typedef struct { WGPURenderBundle bundle; } ns_wg_bundle;

typedef struct {
    const ns_node *canvas;
    JSValue        self;
    WGPUDevice     device;
    WGPUQueue      queue;
    WGPUTexture    target;
    WGPUTextureFormat format;
    WGPUTextureFormat view_formats[8];
    size_t         view_format_count;
    uint32_t       usage;
    int            w, h;
    gboolean       configured;
    gboolean       opaque;
    cairo_surface_t *surf;
    WGPUBuffer     readback;
    uint64_t       readback_size;
    gboolean       pending;
    JSRuntime     *rt;
    JSValue        current;
} ns_wg_context;

static JSValue wg_device_createCommandEncoder(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv);
static JSValue wg_device_createShaderModule(JSContext *ctx,
                                            JSValueConst this_val,
                                            int argc, JSValueConst *argv);
static JSValue wg_device_createRenderPipeline(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv);
static JSValue wg_device_createRenderPipelineAsync(JSContext *ctx,
                                                   JSValueConst this_val,
                                                   int argc, JSValueConst *argv);
static JSValue wg_device_createComputePipelineAsync(JSContext *ctx,
                                                    JSValueConst this_val,
                                                    int argc, JSValueConst *argv);
static JSValue wg_device_createBindGroupLayout(JSContext *ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst *argv);
static JSValue wg_device_createPipelineLayout(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv);
static JSValue wg_device_createBindGroup(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);
static JSValue wg_make_bgl(JSContext *ctx, WGPUBindGroupLayout layout);
static JSValue wg_pipeline_getBindGroupLayout(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv);
static JSValue wg_device_createSampler(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv);
static JSValue wg_device_createTexture(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv);
static JSValue wg_make_texture(JSContext *ctx, WGPUTexture texture);
static JSValue wg_array_from(JSContext *ctx, JSValueConst iterable);
static JSValue wg_queue_writeTexture(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);
static JSValue wg_queue_copyExternalImageToTexture(JSContext *ctx,
                                                   JSValueConst this_val,
                                                   int argc, JSValueConst *argv);
static JSValue wg_device_createQuerySet(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv);
static JSValue wg_encoder_resolveQuerySet(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv);
static JSValue wg_device_createComputePipeline(JSContext *ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst *argv);
static JSValue wg_encoder_beginComputePass(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv);
static JSValue wg_encoder_copyTextureToTexture(JSContext *ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst *argv);
static JSValue wg_encoder_copyBufferToBuffer(JSContext *ctx,
                                             JSValueConst this_val,
                                             int argc, JSValueConst *argv);
static JSValue wg_device_pushErrorScope(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv);
static JSValue wg_device_popErrorScope(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv);
static void wg_read_extent(JSContext *ctx, JSValueConst v, WGPUExtent3D *out);
static void wg_on_uncaptured_error(WGPUDevice const *device, WGPUErrorType type,
                                   WGPUStringView message, void *u1, void *u2);
static gboolean wg_pop_internal_scope(WGPUDevice device, WGPUErrorType *type,
                                      char **message);
static void wg_mark_canvases_pending(void);
static void wg_report_error(ns_wg_error_sink *sink, WGPUErrorType type,
                            const char *message);

static gboolean
ns_webgpu_allowed(void)
{
    const char *env = g_getenv("NS_WEBGPU_ALLOW");
    if (env && env[0] == '1') return TRUE;
    const ns_config *cfg = ns_config_get();
    return cfg && cfg->webgpu_enabled;
}

static WGPUInstance
ns_webgpu_instance(void)
{
    if (!g_wg_instance)
        g_wg_instance = wgpuCreateInstance(NULL);
    return g_wg_instance;
}

typedef struct { int done; WGPUMapAsyncStatus status; } wg_map_wait;

static void
wg_on_map(WGPUMapAsyncStatus status, WGPUStringView message, void *u1, void *u2)
{
    (void)message; (void)u2;
    wg_map_wait *w = u1;
    w->status = status;
    w->done = 1;
}

typedef struct {
    JSContext *ctx;
    GArray    *values;
} wg_hold;

static void *
wg_hold_opaque(wg_hold *h, JSValueConst v, JSClassID class_id)
{
    void *p = JS_GetOpaque(v, class_id);
    if (p) {
        if (!h->values)
            h->values = g_array_new(FALSE, FALSE, sizeof(JSValue));
        JSValue dup = JS_DupValue(h->ctx, v);
        g_array_append_val(h->values, dup);
    }
    return p;
}

static void
wg_hold_release(wg_hold *h)
{
    GArray *values = h->values;
    if (!values) return;
    h->values = NULL;
    for (guint i = 0; i < values->len; i++)
        JS_FreeValue(h->ctx, g_array_index(values, JSValue, i));
    g_array_free(values, TRUE);
}

static char *
wg_sv_dup(WGPUStringView sv)
{
    if (sv.data && sv.length > 0)
        return g_strndup(sv.data, sv.length);
    return g_strdup("");
}

static JSValue
wg_promise_resolved(JSContext *ctx, JSValue value)
{
    JSValue funcs[2];
    JSValue p = JS_NewPromiseCapability(ctx, funcs);
    JSValue r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst *)&value);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    JS_FreeValue(ctx, value);
    return p;
}

static JSValue
wg_promise_rejected(JSContext *ctx, const char *message)
{
    JSValue funcs[2];
    JSValue p = JS_NewPromiseCapability(ctx, funcs);
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, message));
    JSValue r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&err);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    JS_FreeValue(ctx, err);
    return p;
}

static JSValue
wg_promise_settled(JSContext *ctx, JSValue result)
{
    JSValue funcs[2];
    JSValue p = JS_NewPromiseCapability(ctx, funcs);
    gboolean failed = JS_IsException(result);
    JSValue value = failed ? JS_GetException(ctx) : result;
    JSValue r = JS_Call(ctx, funcs[failed ? 1 : 0], JS_UNDEFINED, 1,
                        (JSValueConst *)&value);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    JS_FreeValue(ctx, value);
    return p;
}

static void
wg_bind(JSContext *ctx, JSValueConst obj, const char *name,
        JSCFunction *fn, int argc)
{
    JS_SetPropertyStr(ctx, obj, name,
                      JS_NewCFunction(ctx, fn, name, argc));
}

static JSValue
wg_new_feature_set(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Set");
    JSValue set = JS_CallConstructor(ctx, ctor, 0, NULL);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    if (JS_IsException(set)) return JS_NewObject(ctx);
    return set;
}

#define WG_LIMIT32(name) { #name, offsetof(WGPULimits, name), FALSE }
#define WG_LIMIT64(name) { #name, offsetof(WGPULimits, name), TRUE }

static const struct { const char *name; size_t offset; gboolean wide; } wg_limit_fields[] = {
    WG_LIMIT32(maxTextureDimension1D),
    WG_LIMIT32(maxTextureDimension2D),
    WG_LIMIT32(maxTextureDimension3D),
    WG_LIMIT32(maxTextureArrayLayers),
    WG_LIMIT32(maxBindGroups),
    WG_LIMIT32(maxBindGroupsPlusVertexBuffers),
    WG_LIMIT32(maxBindingsPerBindGroup),
    WG_LIMIT32(maxDynamicUniformBuffersPerPipelineLayout),
    WG_LIMIT32(maxDynamicStorageBuffersPerPipelineLayout),
    WG_LIMIT32(maxSampledTexturesPerShaderStage),
    WG_LIMIT32(maxSamplersPerShaderStage),
    WG_LIMIT32(maxStorageBuffersPerShaderStage),
    WG_LIMIT32(maxStorageTexturesPerShaderStage),
    WG_LIMIT32(maxUniformBuffersPerShaderStage),
    WG_LIMIT64(maxUniformBufferBindingSize),
    WG_LIMIT64(maxStorageBufferBindingSize),
    WG_LIMIT32(minUniformBufferOffsetAlignment),
    WG_LIMIT32(minStorageBufferOffsetAlignment),
    WG_LIMIT32(maxVertexBuffers),
    WG_LIMIT64(maxBufferSize),
    WG_LIMIT32(maxVertexAttributes),
    WG_LIMIT32(maxVertexBufferArrayStride),
    WG_LIMIT32(maxInterStageShaderVariables),
    WG_LIMIT32(maxColorAttachments),
    WG_LIMIT32(maxColorAttachmentBytesPerSample),
    WG_LIMIT32(maxComputeWorkgroupStorageSize),
    WG_LIMIT32(maxComputeInvocationsPerWorkgroup),
    WG_LIMIT32(maxComputeWorkgroupSizeX),
    WG_LIMIT32(maxComputeWorkgroupSizeY),
    WG_LIMIT32(maxComputeWorkgroupSizeZ),
    WG_LIMIT32(maxComputeWorkgroupsPerDimension),
};

static JSValue
wg_limits_object(JSContext *ctx, const WGPULimits *l)
{
    JSValue o = JS_NewObject(ctx);
    for (size_t i = 0; i < G_N_ELEMENTS(wg_limit_fields); i++) {
        const char *field = (const char *)l + wg_limit_fields[i].offset;
        double v;
        if (wg_limit_fields[i].wide) {
            uint64_t u;
            memcpy(&u, field, sizeof u);
            v = (double)u;
        } else {
            uint32_t u;
            memcpy(&u, field, sizeof u);
            v = (double)u;
        }
        JS_DefinePropertyValueStr(ctx, o, wg_limit_fields[i].name,
                                  JS_NewFloat64(ctx, v), JS_PROP_ENUMERABLE);
    }
    JS_PreventExtensions(ctx, o);
    return o;
}

static gboolean
wg_read_required_limits(JSContext *ctx, JSValueConst v, WGPULimits *out)
{
    WGPULimits init = WGPU_LIMITS_INIT;
    *out = init;
    if (!JS_IsObject(v)) return FALSE;
    gboolean any = FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(wg_limit_fields); i++) {
        JSValue jv = JS_GetPropertyStr(ctx, v, wg_limit_fields[i].name);
        double d = -1;
        if (!JS_IsUndefined(jv)) JS_ToFloat64(ctx, &d, jv);
        JS_FreeValue(ctx, jv);
        if (!(d >= 0)) continue;
        char *field = (char *)out + wg_limit_fields[i].offset;
        if (wg_limit_fields[i].wide) {
            uint64_t u = d >= 1.8e19 ? UINT64_MAX - 1 : (uint64_t)d;
            memcpy(field, &u, sizeof u);
        } else {
            uint32_t u = d >= 4294967294.0 ? UINT32_MAX - 1 : (uint32_t)d;
            memcpy(field, &u, sizeof u);
        }
        any = TRUE;
    }
    return any;
}

static const struct { const char *name; WGPUFeatureName feature; } wg_feature_names[] = {
    { "core-features-and-limits", WGPUFeatureName_CoreFeaturesAndLimits },
    { "depth-clip-control", WGPUFeatureName_DepthClipControl },
    { "depth32float-stencil8", WGPUFeatureName_Depth32FloatStencil8 },
    { "texture-compression-bc", WGPUFeatureName_TextureCompressionBC },
    { "texture-compression-bc-sliced-3d", WGPUFeatureName_TextureCompressionBCSliced3D },
    { "texture-compression-etc2", WGPUFeatureName_TextureCompressionETC2 },
    { "texture-compression-astc", WGPUFeatureName_TextureCompressionASTC },
    { "texture-compression-astc-sliced-3d", WGPUFeatureName_TextureCompressionASTCSliced3D },
    { "timestamp-query", WGPUFeatureName_TimestampQuery },
    { "indirect-first-instance", WGPUFeatureName_IndirectFirstInstance },
    { "shader-f16", WGPUFeatureName_ShaderF16 },
    { "rg11b10ufloat-renderable", WGPUFeatureName_RG11B10UfloatRenderable },
    { "bgra8unorm-storage", WGPUFeatureName_BGRA8UnormStorage },
    { "float32-filterable", WGPUFeatureName_Float32Filterable },
    { "float32-blendable", WGPUFeatureName_Float32Blendable },
    { "clip-distances", WGPUFeatureName_ClipDistances },
    { "dual-source-blending", WGPUFeatureName_DualSourceBlending },
    { "subgroups", WGPUFeatureName_Subgroups },
    { "subgroups", (WGPUFeatureName)WGPUNativeFeature_Subgroup },
    { "texture-formats-tier1", WGPUFeatureName_TextureFormatsTier1 },
    { "texture-formats-tier2", WGPUFeatureName_TextureFormatsTier2 },
    { "primitive-index", WGPUFeatureName_PrimitiveIndex },
};

static gboolean
wg_adapter_is_core(WGPUAdapter adapter)
{
    WGPUAdapterInfo info; memset(&info, 0, sizeof info);
    if (wgpuAdapterGetInfo(adapter, &info) != WGPUStatus_Success) return FALSE;
    gboolean core = info.backendType != WGPUBackendType_OpenGL &&
                    info.backendType != WGPUBackendType_OpenGLES;
    wgpuAdapterInfoFreeMembers(info);
    return core;
}

static JSValue
wg_feature_set(JSContext *ctx, const WGPUSupportedFeatures *f, gboolean core)
{
    JSValue set = wg_new_feature_set(ctx);
    JSValue add = JS_GetPropertyStr(ctx, set, "add");
    if (core) {
        JSValue name = JS_NewString(ctx, "core-features-and-limits");
        JS_FreeValue(ctx, JS_Call(ctx, add, set, 1, (JSValueConst *)&name));
        JS_FreeValue(ctx, name);
    }
    for (size_t i = 0; f && i < f->featureCount; i++) {
        for (size_t k = 0; k < G_N_ELEMENTS(wg_feature_names); k++) {
            if (wg_feature_names[k].feature != f->features[i]) continue;
            JSValue name = JS_NewString(ctx, wg_feature_names[k].name);
            JS_FreeValue(ctx, JS_Call(ctx, add, set, 1, (JSValueConst *)&name));
            JS_FreeValue(ctx, name);
            break;
        }
    }
    JS_FreeValue(ctx, add);
    return set;
}

static const WGPUFeatureName *
wg_feature_from_name(WGPUAdapter adapter, const char *name)
{
    const WGPUFeatureName *known = NULL;
    for (size_t k = 0; name && k < G_N_ELEMENTS(wg_feature_names); k++) {
        if (strcmp(wg_feature_names[k].name, name) != 0) continue;
        if (!known) known = &wg_feature_names[k].feature;
        if (adapter && wgpuAdapterHasFeature(adapter, wg_feature_names[k].feature))
            return &wg_feature_names[k].feature;
    }
    return known;
}

static JSValue
wg_array_from(JSContext *ctx, JSValueConst iterable)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue array = JS_GetPropertyStr(ctx, global, "Array");
    JSValue from = JS_GetPropertyStr(ctx, array, "from");
    JSValue list = JS_Call(ctx, from, array, 1, &iterable);
    JS_FreeValue(ctx, from);
    JS_FreeValue(ctx, array);
    JS_FreeValue(ctx, global);
    return list;
}

static gboolean
wg_read_required_features(JSContext *ctx, WGPUAdapter adapter, JSValueConst v,
                          WGPUFeatureName *out, size_t cap, size_t *count)
{
    *count = 0;
    if (!JS_IsObject(v)) return TRUE;
    JSValue list = wg_array_from(ctx, v);
    if (JS_IsException(list)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return FALSE;
    }
    uint32_t n = 0;
    JSValue jl = JS_GetPropertyStr(ctx, list, "length");
    JS_ToUint32(ctx, &n, jl);
    JS_FreeValue(ctx, jl);
    gboolean ok = TRUE;
    for (uint32_t i = 0; i < n && ok; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        const char *name = JS_ToCString(ctx, e);
        const WGPUFeatureName *f = wg_feature_from_name(adapter, name);
        ok = f != NULL;
        if (f) {
            gboolean dup = FALSE;
            for (size_t k = 0; k < *count; k++) dup |= out[k] == *f;
            if (!dup && *count < cap) out[(*count)++] = *f;
        }
        if (name) JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, list);
    return ok;
}

static ns_wg_queue *
wg_queue_unwrap(JSValueConst v)
{
    return JS_GetOpaque(v, g_queue_class);
}

static JSValue
wg_queue_writeBuffer(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    ns_wg_queue *q = wg_queue_unwrap(this_val);
    if (!q || argc < 3) return JS_UNDEFINED;
    ns_wg_buffer *buf = JS_GetOpaque(argv[0], g_buffer_class);
    if (!buf) return JS_UNDEFINED;
    int64_t buffer_offset = 0;
    JS_ToInt64(ctx, &buffer_offset, argv[1]);
    int64_t data_offset = 0, size = -1;
    if (argc >= 4 && !JS_IsUndefined(argv[3])) JS_ToInt64(ctx, &data_offset, argv[3]);
    if (argc >= 5 && !JS_IsUndefined(argv[4])) JS_ToInt64(ctx, &size, argv[4]);

    size_t byte_len = 0;
    size_t view_off = 0, view_len = 0, bpe = 0;
    uint8_t *bytes = NULL;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[2], &view_off, &view_len, &bpe);
    if (!JS_IsException(abuf)) {
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, abuf);
        if (base && view_off <= total) {
            size_t avail = total - view_off;
            bytes = base + view_off;
            byte_len = view_len < avail ? view_len : avail;
        }
        JS_FreeValue(ctx, abuf);
    } else {
        JS_FreeValue(ctx, abuf);
        bytes = JS_GetArrayBuffer(ctx, &byte_len, argv[2]);
    }
    if (!bytes) return JS_UNDEFINED;

    size_t elem = bpe > 0 ? bpe : 1;
    if (buffer_offset < 0 || data_offset < 0 || size < -1) return JS_UNDEFINED;
    uint64_t view_elems = byte_len / elem;
    if ((uint64_t)data_offset > view_elems) return JS_UNDEFINED;
    uint64_t avail = view_elems - (uint64_t)data_offset;
    uint64_t write_elems = size < 0 ? avail : (uint64_t)size;
    if (write_elems > avail) return JS_UNDEFINED;
    size_t byte_off = (size_t)data_offset * elem;
    size_t byte_size = (size_t)write_elems * elem;

    wgpuQueueWriteBuffer(q->queue, buf->buffer, (uint64_t)buffer_offset,
                         bytes + byte_off, byte_size);
    return JS_UNDEFINED;
}

typedef struct { int done; } wg_work_wait;

static void
wg_on_work_done(WGPUQueueWorkDoneStatus status, WGPUStringView message,
                void *u1, void *u2)
{
    (void)status; (void)message; (void)u2;
    ((wg_work_wait *)u1)->done = 1;
}

static JSValue
wg_queue_onSubmittedWorkDone(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wg_queue *q = wg_queue_unwrap(this_val);
    if (!q) return wg_promise_rejected(ctx, "onSubmittedWorkDone: queue");
    wg_work_wait wait = { 0 };
    WGPUQueueWorkDoneCallbackInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = wg_on_work_done;
    ci.userdata1 = &wait;
    wgpuQueueOnSubmittedWorkDone(q->queue, ci);
    for (int i = 0; i < 4000 && !wait.done; i++)
        wgpuInstanceProcessEvents(ns_webgpu_instance());
    return wg_promise_resolved(ctx, JS_UNDEFINED);
}

static JSValue
wg_queue_submit(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    ns_wg_queue *q = wg_queue_unwrap(this_val);
    if (!q || argc < 1) return JS_UNDEFINED;
    uint32_t len = 0;
    JSValue jlen = JS_GetPropertyStr(ctx, argv[0], "length");
    JS_ToUint32(ctx, &len, jlen);
    JS_FreeValue(ctx, jlen);
    if (len == 0) return JS_UNDEFINED;
    if (len > 4096) len = 4096;

    JSValue *vals = g_new0(JSValue, len);
    for (uint32_t i = 0; i < len; i++)
        vals[i] = JS_GetPropertyUint32(ctx, argv[0], i);

    WGPUCommandBuffer *cmds = g_new0(WGPUCommandBuffer, len);
    GPtrArray *consumed = g_ptr_array_new();
    uint32_t n = 0;
    gboolean invalid = FALSE;
    for (uint32_t i = 0; i < len; i++) {
        ns_wg_cmdbuf *cb = JS_GetOpaque(vals[i], g_cmdbuf_class);
        if (!cb || !cb->cmd) { invalid = TRUE; continue; }
        gboolean dup = FALSE;
        for (guint k = 0; k < consumed->len && !dup; k++)
            dup = g_ptr_array_index(consumed, k) == cb;
        if (dup) { invalid = TRUE; continue; }
        cmds[n++] = cb->cmd;
        g_ptr_array_add(consumed, cb);
    }
    if (invalid) {
        if (q->sink)
            wg_report_error(q->sink, WGPUErrorType_Validation,
                            "queue.submit: a command buffer is invalid, was "
                            "already submitted, or appears more than once");
    } else if (n > 0) {
        wgpuQueueSubmit(q->queue, n, cmds);
        wg_mark_canvases_pending();
        for (guint i = 0; i < consumed->len; i++) {
            ns_wg_cmdbuf *cb = g_ptr_array_index(consumed, i);
            wgpuCommandBufferRelease(cb->cmd);
            cb->cmd = NULL;
        }
    }
    g_ptr_array_free(consumed, TRUE);
    for (uint32_t i = 0; i < len; i++) JS_FreeValue(ctx, vals[i]);
    g_free(vals);
    g_free(cmds);
    return JS_UNDEFINED;
}

static void
wg_queue_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_queue *q = JS_GetOpaque(val, g_queue_class);
    if (!q) return;
    if (q->queue) wgpuQueueRelease(q->queue);
    g_free(q);
}

static void
wg_mark_canvases_pending(void)
{
    if (!g_webgpu_ctx_by_node) return;
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, g_webgpu_ctx_by_node);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        ns_wg_context *c = value;
        if (c->configured && c->target) c->pending = TRUE;
    }
}

static void
wg_report_error(ns_wg_error_sink *sink, WGPUErrorType type, const char *message)
{
    WGPUStringView sv = { message, strlen(message) };
    wg_on_uncaptured_error(NULL, type, sv, sink, NULL);
}

static JSValue
wg_make_queue(JSContext *ctx, WGPUQueue queue, ns_wg_error_sink *sink)
{
    JSValue obj = JS_NewObjectClass(ctx, g_queue_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_queue *q = g_new0(ns_wg_queue, 1);
    q->queue = queue;
    q->sink = sink;
    JS_SetOpaque(obj, q);
    JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, ""));
    return obj;
}

static gboolean
wg_buffer_detach_ranges(JSContext *ctx, ns_wg_buffer *b)
{
    if (!b) return FALSE;
    if (b->mapped_ranges) {
        for (guint i = 0; i < b->mapped_ranges->len; i++) {
            JSValue ab = g_array_index(b->mapped_ranges, JSValue, i);
            size_t len = 0;
            if (!JS_GetArrayBuffer(ctx, &len, ab)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                b->range_escaped = TRUE;
            }
            JS_DetachArrayBuffer(ctx, ab);
            JS_FreeValue(ctx, ab);
        }
        g_array_set_size(b->mapped_ranges, 0);
    }
    return !b->range_escaped;
}

static void
wg_buffer_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wg_buffer *b = JS_GetOpaque(val, g_buffer_class);
    if (!b) return;
    if (b->mapped_ranges) {
        for (guint i = 0; i < b->mapped_ranges->len; i++)
            JS_FreeValueRT(rt, g_array_index(b->mapped_ranges, JSValue, i));
        g_array_free(b->mapped_ranges, TRUE);
    }
    if (b->buffer) wgpuBufferRelease(b->buffer);
    if (b->device) wgpuDeviceRelease(b->device);
    g_free(b);
}

static JSValue
wg_buffer_destroy(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wg_buffer *b = JS_GetOpaque(this_val, g_buffer_class);
    if (!b) return JS_UNDEFINED;
    b->mapped = FALSE;
    if (wg_buffer_detach_ranges(ctx, b) && b->buffer)
        wgpuBufferDestroy(b->buffer);
    return JS_UNDEFINED;
}

static void *
wg_ab_free(JSRuntime *rt, void *opaque, void *ptr, size_t size)
{
    (void)ptr;
    if (size != 0)
        return NULL;
    if (opaque) {
        JSValue *held = opaque;
        JS_FreeValueRT(rt, *held);
        g_free(held);
    }
    return NULL;
}

static JSValue
wg_buffer_getMappedRange(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    ns_wg_buffer *b = JS_GetOpaque(this_val, g_buffer_class);
    if (!b || !b->buffer) return JS_UNDEFINED;
    if (!b->mapped)
        return JS_ThrowTypeError(ctx,
            "OperationError: getMappedRange: buffer is not mapped");
    int64_t offset = 0, size = -1;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) JS_ToInt64(ctx, &offset, argv[0]);
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToInt64(ctx, &size, argv[1]);
    if (offset < 0 || (offset & 7))
        return JS_ThrowTypeError(ctx,
            "OperationError: getMappedRange: offset must be a non-negative "
            "multiple of 8");
    uint64_t map_end = b->map_offset + b->map_size;
    if ((uint64_t)offset < b->map_offset || (uint64_t)offset > map_end)
        return JS_ThrowTypeError(ctx,
            "OperationError: getMappedRange: offset is outside the mapped range");
    uint64_t sz = size < 0 ? map_end - (uint64_t)offset : (uint64_t)size;
    if ((sz & 3) || sz > map_end - (uint64_t)offset)
        return JS_ThrowTypeError(ctx,
            "OperationError: getMappedRange: size must be a multiple of 4 "
            "within the mapped range");
    void *p = wgpuBufferGetMappedRange(b->buffer, (size_t)offset, (size_t)sz);
    if (!p) return JS_ThrowRangeError(ctx, "getMappedRange failed");
    JSValue *held = g_new(JSValue, 1);
    *held = JS_DupValue(ctx, this_val);
    JSValue ab = JS_NewArrayBuffer(ctx, (uint8_t *)p, (size_t)sz, 0, wg_ab_free,
                                   held, false);
    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, *held);
        g_free(held);
        return ab;
    }
    if (!b->mapped_ranges)
        b->mapped_ranges = g_array_new(FALSE, FALSE, sizeof(JSValue));
    JSValue keep = JS_DupValue(ctx, ab);
    g_array_append_val(b->mapped_ranges, keep);
    return ab;
}

static JSValue
wg_buffer_unmap(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wg_buffer *b = JS_GetOpaque(this_val, g_buffer_class);
    if (!b) return JS_UNDEFINED;
    b->mapped = FALSE;
    if (wg_buffer_detach_ranges(ctx, b) && b->buffer)
        wgpuBufferUnmap(b->buffer);
    return JS_UNDEFINED;
}

static JSValue
wg_buffer_mapAsync(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    ns_wg_buffer *b = JS_GetOpaque(this_val, g_buffer_class);
    if (!b || !b->buffer) return wg_promise_rejected(ctx, "mapAsync: buffer");
    int64_t mode = 1, offset = 0, size = -1;
    if (argc >= 1) JS_ToInt64(ctx, &mode, argv[0]);
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToInt64(ctx, &offset, argv[1]);
    if (argc >= 3 && !JS_IsUndefined(argv[2])) JS_ToInt64(ctx, &size, argv[2]);
    if (offset < 0 || (uint64_t)offset > b->size)
        return wg_promise_rejected(ctx, "mapAsync: range");
    size_t sz = size < 0 ? (size_t)(b->size - (uint64_t)offset) : (size_t)size;

    wg_map_wait wait = { 0 };
    WGPUBufferMapCallbackInfo mci;
    memset(&mci, 0, sizeof mci);
    mci.mode = WGPUCallbackMode_AllowProcessEvents;
    mci.callback = wg_on_map;
    mci.userdata1 = &wait;
    if (mode != WGPUMapMode_Read && mode != WGPUMapMode_Write)
        return wg_promise_rejected(ctx, "OperationError: mapAsync: invalid mode");
    wgpuBufferMapAsync(b->buffer, (WGPUMapMode)mode, (size_t)offset, sz, mci);
    for (int i = 0; i < 4000 && !wait.done; i++) {
        if (b->device) wgpuDevicePoll(b->device, 1, NULL);
        wgpuInstanceProcessEvents(ns_webgpu_instance());
    }
    if (!wait.done || wait.status != WGPUMapAsyncStatus_Success)
        return wg_promise_rejected(ctx, "OperationError: mapAsync failed");
    b->mapped = TRUE;
    b->map_offset = (uint64_t)offset;
    b->map_size = sz;
    return wg_promise_resolved(ctx, JS_UNDEFINED);
}

static JSValue
wg_device_createBuffer(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d || argc < 1 || !JS_IsObject(argv[0]))
        return wg_promise_rejected(ctx, "createBuffer: descriptor required");

    JSValue jsize = JS_GetPropertyStr(ctx, argv[0], "size");
    JSValue jusage = JS_GetPropertyStr(ctx, argv[0], "usage");
    JSValue jmap = JS_GetPropertyStr(ctx, argv[0], "mappedAtCreation");
    int64_t size = 0; uint32_t usage = 0;
    JS_ToInt64(ctx, &size, jsize);
    JS_ToUint32(ctx, &usage, jusage);
    int mapped = JS_ToBool(ctx, jmap);
    JS_FreeValue(ctx, jsize);
    JS_FreeValue(ctx, jusage);
    JS_FreeValue(ctx, jmap);

    WGPUBufferDescriptor desc;
    memset(&desc, 0, sizeof desc);
    desc.size = (uint64_t)(size < 0 ? 0 : size);
    desc.usage = (WGPUBufferUsage)(usage & ~0x3FFu ? 0 : usage);
    desc.mappedAtCreation = mapped ? 1 : 0;
    WGPUBuffer wbuf = wgpuDeviceCreateBuffer(d->device, &desc);
    if (!wbuf)
        return JS_ThrowTypeError(ctx, "createBuffer: wgpu returned null");

    JSValue obj = JS_NewObjectClass(ctx, g_buffer_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_buffer *b = g_new0(ns_wg_buffer, 1);
    b->buffer = wbuf;
    b->size = desc.size;
    b->usage = usage;
    wgpuDeviceAddRef(d->device);
    b->device = d->device;
    if (desc.mappedAtCreation) {
        b->mapped = TRUE;
        b->map_offset = 0;
        b->map_size = desc.size;
    }
    JS_SetOpaque(obj, b);
    JS_SetPropertyStr(ctx, obj, "size", JS_NewFloat64(ctx, (double)desc.size));
    JS_SetPropertyStr(ctx, obj, "usage", JS_NewUint32(ctx, usage));
    JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, ""));
    return obj;
}

static JSValue
wg_device_getQueue(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d) return JS_UNDEFINED;
    wgpuQueueAddRef(d->queue);
    return wg_make_queue(ctx, d->queue, d->sink);
}

static void
wg_device_resolve_lost(JSContext *ctx, ns_wg_device *d, const char *reason,
                       const char *message)
{
    if (!JS_IsFunction(ctx, d->lost_resolve)) return;
    JSValue info = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, info, "reason", JS_NewString(ctx, reason));
    JS_SetPropertyStr(ctx, info, "message", JS_NewString(ctx, message));
    JSValue resolve = d->lost_resolve;
    d->lost_resolve = JS_UNDEFINED;
    JS_FreeValue(ctx, JS_Call(ctx, resolve, JS_UNDEFINED, 1, (JSValueConst *)&info));
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, info);
}

static JSValue
wg_device_destroy(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d || d->destroyed) return JS_UNDEFINED;
    d->destroyed = TRUE;
    wgpuDeviceDestroy(d->device);
    wg_device_resolve_lost(ctx, d, "destroyed", "Device was destroyed.");
    return JS_UNDEFINED;
}

static void
wg_device_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wg_device *d = JS_GetOpaque(val, g_device_class);
    if (!d) return;
    if (d->sink) {
        d->sink->ctx = NULL;
        d->sink->device_obj = JS_UNDEFINED;
    }
    JS_FreeValueRT(rt, d->lost_resolve);
    if (d->scope_internal) g_byte_array_free(d->scope_internal, TRUE);
    if (d->queue) wgpuQueueRelease(d->queue);
    if (d->device) wgpuDeviceRelease(d->device);
    g_free(d);
}

static void
wg_link_event_target(JSContext *ctx, JSValueConst device)
{
    JSValue proto = JS_GetPrototype(ctx, device);
    JSValue parent = JS_GetPrototype(ctx, proto);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue et = JS_GetPropertyStr(ctx, global, "EventTarget");
    JSValue et_proto = JS_IsFunction(ctx, et)
        ? JS_GetPropertyStr(ctx, et, "prototype") : JS_UNDEFINED;
    if (JS_IsObject(et_proto) && JS_VALUE_GET_PTR(parent) != JS_VALUE_GET_PTR(et_proto))
        JS_SetPrototype(ctx, proto, et_proto);
    JS_FreeValue(ctx, et_proto);
    JS_FreeValue(ctx, et);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, parent);
    JS_FreeValue(ctx, proto);
}

static JSValue
wg_make_device(JSContext *ctx, WGPUDevice device, ns_wg_error_sink *sink,
               gboolean core)
{
    JSValue obj = JS_NewObjectClass(ctx, g_device_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_device *d = g_new0(ns_wg_device, 1);
    d->device = device;
    d->queue = wgpuDeviceGetQueue(device);
    d->sink = sink;
    d->lost_resolve = JS_UNDEFINED;
    sink->ctx = ctx;
    sink->device_obj = obj;
    JS_SetOpaque(obj, d);
    wg_link_event_target(ctx, obj);

    wgpuQueueAddRef(d->queue);
    JS_SetPropertyStr(ctx, obj, "queue", wg_make_queue(ctx, d->queue, sink));
    {
        JSValue lost_funcs[2];
        JSValue lost = JS_NewPromiseCapability(ctx, lost_funcs);
        d->lost_resolve = lost_funcs[0];
        JS_FreeValue(ctx, lost_funcs[1]);
        JS_SetPropertyStr(ctx, obj, "lost", lost);
    }
    WGPUSupportedFeatures features; memset(&features, 0, sizeof features);
    wgpuDeviceGetFeatures(device, &features);
    JS_SetPropertyStr(ctx, obj, "features", wg_feature_set(ctx, &features, core));
    wgpuSupportedFeaturesFreeMembers(features);
    WGPULimits limits = WGPU_LIMITS_INIT;
    wgpuDeviceGetLimits(device, &limits);
    JS_SetPropertyStr(ctx, obj, "limits", wg_limits_object(ctx, &limits));
    JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, ""));
    return obj;
}

typedef struct { WGPUDevice device; int done; } wg_device_wait;

static void
wg_on_device(WGPURequestDeviceStatus status, WGPUDevice device,
             WGPUStringView message, void *u1, void *u2)
{
    (void)message; (void)u2;
    wg_device_wait *w = u1;
    w->device = (status == WGPURequestDeviceStatus_Success) ? device : NULL;
    w->done = 1;
}

static JSValue
wg_new_error(JSContext *ctx, WGPUErrorType type, const char *message)
{
    const char *cls = type == WGPUErrorType_OutOfMemory ? "GPUOutOfMemoryError"
                    : type == WGPUErrorType_Validation ? "GPUValidationError"
                    : "GPUInternalError";
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, cls);
    JSValue msg = JS_NewString(ctx, message ? message : "");
    JSValue err = JS_IsFunction(ctx, ctor)
        ? JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)&msg) : JS_NewError(ctx);
    if (JS_IsException(err)) err = JS_GetException(ctx);
    JS_FreeValue(ctx, msg);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    return err;
}

static GPtrArray *g_wg_signalled_sinks;

static void
wg_dispatch_sink(JSContext *ctx, ns_wg_error_sink *sink)
{
    sink->job_queued = FALSE;
    GPtrArray *pending = sink->pending;
    sink->pending = NULL;
    if (!pending) return;
    for (guint i = 0; i < pending->len; i++) {
        ns_wg_pending_error *pe = g_ptr_array_index(pending, i);
        if (sink->ctx == ctx && JS_IsObject(sink->device_obj)) {
            JSValue dev = JS_DupValue(ctx, sink->device_obj);
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue ctor = JS_GetPropertyStr(ctx, global, "GPUUncapturedErrorEvent");
            JSValue init = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, init, "error", wg_new_error(ctx, pe->type, pe->message));
            JS_SetPropertyStr(ctx, init, "cancelable", JS_TRUE);
            JSValue args[2] = { JS_NewString(ctx, "uncapturederror"), init };
            JSValue ev = JS_CallConstructor(ctx, ctor, 2, args);
            gboolean handled = FALSE;
            if (!JS_IsException(ev)) {
                JSValue dispatch = JS_GetPropertyStr(ctx, dev, "dispatchEvent");
                JSValue r = JS_IsFunction(ctx, dispatch)
                    ? JS_Call(ctx, dispatch, dev, 1, (JSValueConst *)&ev) : JS_TRUE;
                handled = JS_IsBool(r) && !JS_ToBool(ctx, r);
                JSValue dp = JS_GetPropertyStr(ctx, ev, "defaultPrevented");
                handled |= JS_ToBool(ctx, dp);
                JS_FreeValue(ctx, dp);
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, dispatch);
            }
            if (JS_IsException(ev) || JS_HasException(ctx))
                JS_FreeValue(ctx, JS_GetException(ctx));
            if (!handled && sink->logged++ < 20)
                g_warning("[webgpu] %s", pe->message);
            JS_FreeValue(ctx, ev);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, init);
            JS_FreeValue(ctx, ctor);
            JS_FreeValue(ctx, global);
            JS_FreeValue(ctx, dev);
        }
        g_free(pe->message);
        g_free(pe);
    }
    g_ptr_array_free(pending, TRUE);
}

static JSValue
wg_dispatch_uncaptured(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    if (!g_wg_signalled_sinks) return JS_UNDEFINED;
    GPtrArray *sinks = g_wg_signalled_sinks;
    g_wg_signalled_sinks = NULL;
    for (guint i = 0; i < sinks->len; i++)
        wg_dispatch_sink(ctx, g_ptr_array_index(sinks, i));
    g_ptr_array_free(sinks, TRUE);
    return JS_UNDEFINED;
}

static void
wg_on_uncaptured_error(WGPUDevice const *device, WGPUErrorType type,
                       WGPUStringView message, void *u1, void *u2)
{
    (void)device; (void)u2;
    ns_wg_error_sink *sink = u1;
    if (!sink || !sink->ctx) return;
    if (!sink->pending) sink->pending = g_ptr_array_new();
    if (sink->pending->len >= 64) return;
    ns_wg_pending_error *pe = g_new0(ns_wg_pending_error, 1);
    pe->type = type;
    pe->message = wg_sv_dup(message);
    g_ptr_array_add(sink->pending, pe);
    if (!sink->job_queued) {
        sink->job_queued = TRUE;
        if (!g_wg_signalled_sinks) {
            g_wg_signalled_sinks = g_ptr_array_new();
            JS_EnqueueJob(sink->ctx, wg_dispatch_uncaptured, 0, NULL);
        }
        g_ptr_array_add(g_wg_signalled_sinks, sink);
    }
}

static JSValue
wg_adapter_requestDevice(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    ns_wg_adapter *a = JS_GetOpaque(this_val, g_adapter_class);
    if (!a) return wg_promise_rejected(ctx, "requestDevice: invalid adapter");

    WGPUFeatureName required[G_N_ELEMENTS(wg_feature_names)];
    size_t required_count = 0;
    WGPULimits limits;
    gboolean have_limits = FALSE;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue jfeat = JS_GetPropertyStr(ctx, argv[0], "requiredFeatures");
        gboolean ok = wg_read_required_features(ctx, a->adapter, jfeat, required,
                                                G_N_ELEMENTS(required),
                                                &required_count);
        JS_FreeValue(ctx, jfeat);
        if (!ok)
            return wg_promise_rejected(ctx, "requestDevice: unsupported feature");
        size_t kept = 0;
        for (size_t i = 0; i < required_count; i++) {
            if (required[i] != WGPUFeatureName_CoreFeaturesAndLimits)
                required[kept++] = required[i];
            else if (!wg_adapter_is_core(a->adapter))
                return wg_promise_rejected(ctx, "requestDevice: unsupported feature");
        }
        required_count = kept;
        JSValue jlim = JS_GetPropertyStr(ctx, argv[0], "requiredLimits");
        have_limits = wg_read_required_limits(ctx, jlim, &limits);
        JS_FreeValue(ctx, jlim);
    }

    wg_device_wait wait; memset(&wait, 0, sizeof wait);
    WGPURequestDeviceCallbackInfo ci; memset(&ci, 0, sizeof ci);
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = wg_on_device;
    ci.userdata1 = &wait;
    WGPUDeviceDescriptor dd; memset(&dd, 0, sizeof dd);
    ns_wg_error_sink *sink = g_new0(ns_wg_error_sink, 1);
    sink->device_obj = JS_UNDEFINED;
    dd.uncapturedErrorCallbackInfo.callback = wg_on_uncaptured_error;
    dd.uncapturedErrorCallbackInfo.userdata1 = sink;
    dd.requiredFeatureCount = required_count;
    dd.requiredFeatures = required_count ? required : NULL;
    dd.requiredLimits = have_limits ? &limits : NULL;
    wgpuAdapterRequestDevice(a->adapter, &dd, ci);
    for (int i = 0; i < 2000 && !wait.done; i++)
        wgpuInstanceProcessEvents(ns_webgpu_instance());
    if (!wait.device)
        return wg_promise_rejected(ctx, "requestDevice: no device");
    return wg_promise_resolved(ctx, wg_make_device(ctx, wait.device, sink,
                                                   wg_adapter_is_core(a->adapter)));
}

static JSValue
wg_adapter_info(JSContext *ctx, WGPUAdapter adapter)
{
    WGPUAdapterInfo info; memset(&info, 0, sizeof info);
    JSValue o = JS_NewObject(ctx);
    if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
        char *vendor = wg_sv_dup(info.vendor);
        char *arch = wg_sv_dup(info.architecture);
        char *dev = wg_sv_dup(info.device);
        char *descr = wg_sv_dup(info.description);
        JS_SetPropertyStr(ctx, o, "vendor", JS_NewString(ctx, vendor));
        JS_SetPropertyStr(ctx, o, "architecture", JS_NewString(ctx, arch));
        JS_SetPropertyStr(ctx, o, "device", JS_NewString(ctx, dev));
        JS_SetPropertyStr(ctx, o, "description", JS_NewString(ctx, descr));
        g_free(vendor); g_free(arch); g_free(dev); g_free(descr);
        wgpuAdapterInfoFreeMembers(info);
    }
    return o;
}

static void
wg_adapter_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_adapter *a = JS_GetOpaque(val, g_adapter_class);
    if (!a) return;
    if (a->adapter) wgpuAdapterRelease(a->adapter);
    g_free(a);
}

static JSValue
wg_make_adapter(JSContext *ctx, WGPUAdapter adapter)
{
    JSValue obj = JS_NewObjectClass(ctx, g_adapter_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_adapter *a = g_new0(ns_wg_adapter, 1);
    a->adapter = adapter;
    JS_SetOpaque(obj, a);

    JS_SetPropertyStr(ctx, obj, "info", wg_adapter_info(ctx, adapter));
    WGPUSupportedFeatures features; memset(&features, 0, sizeof features);
    wgpuAdapterGetFeatures(adapter, &features);
    JS_SetPropertyStr(ctx, obj, "features",
                      wg_feature_set(ctx, &features, wg_adapter_is_core(adapter)));
    wgpuSupportedFeaturesFreeMembers(features);
    WGPULimits limits = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(adapter, &limits);
    JS_SetPropertyStr(ctx, obj, "limits", wg_limits_object(ctx, &limits));
    JS_SetPropertyStr(ctx, obj, "isFallbackAdapter", JS_FALSE);
    return obj;
}

typedef struct { WGPUAdapter adapter; int done; } wg_adapter_wait;

static void
wg_on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
              WGPUStringView message, void *u1, void *u2)
{
    (void)message; (void)u2;
    wg_adapter_wait *w = u1;
    w->adapter = (status == WGPURequestAdapterStatus_Success) ? adapter : NULL;
    w->done = 1;
}

static JSValue
wg_gpu_requestAdapter(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (!ns_webgpu_allowed())
        return wg_promise_resolved(ctx, JS_NULL);
    WGPUInstance inst = ns_webgpu_instance();
    if (!inst)
        return wg_promise_resolved(ctx, JS_NULL);

    wg_adapter_wait wait; memset(&wait, 0, sizeof wait);
    WGPURequestAdapterCallbackInfo ci; memset(&ci, 0, sizeof ci);
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = wg_on_adapter;
    ci.userdata1 = &wait;
    wgpuInstanceRequestAdapter(inst, NULL, ci);
    for (int i = 0; i < 2000 && !wait.done; i++)
        wgpuInstanceProcessEvents(inst);
    if (!wait.adapter)
        return wg_promise_resolved(ctx, JS_NULL);
    return wg_promise_resolved(ctx, wg_make_adapter(ctx, wait.adapter));
}

static JSValue
wg_gpu_getPreferredCanvasFormat(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewString(ctx, "bgra8unorm");
}

static int
wg_canvas_dim(const ns_node *canvas, const char *name, int defv)
{
    const char *s = ns_element_get_attr(canvas, name);
    if (!s || !*s) return defv;
    long v = strtol(s, NULL, 10);
    if (v <= 0) return defv;
    if (v > 8192) v = 8192;
    return (int)v;
}

static const struct { const char *name; WGPUTextureFormat fmt; } wg_texture_formats[] = {
    { "r8unorm", WGPUTextureFormat_R8Unorm },
    { "r8snorm", WGPUTextureFormat_R8Snorm },
    { "r8uint", WGPUTextureFormat_R8Uint },
    { "r8sint", WGPUTextureFormat_R8Sint },
    { "r16unorm", WGPUTextureFormat_R16Unorm },
    { "r16snorm", WGPUTextureFormat_R16Snorm },
    { "r16uint", WGPUTextureFormat_R16Uint },
    { "r16sint", WGPUTextureFormat_R16Sint },
    { "r16float", WGPUTextureFormat_R16Float },
    { "rg8unorm", WGPUTextureFormat_RG8Unorm },
    { "rg8snorm", WGPUTextureFormat_RG8Snorm },
    { "rg8uint", WGPUTextureFormat_RG8Uint },
    { "rg8sint", WGPUTextureFormat_RG8Sint },
    { "r32float", WGPUTextureFormat_R32Float },
    { "r32uint", WGPUTextureFormat_R32Uint },
    { "r32sint", WGPUTextureFormat_R32Sint },
    { "rg16unorm", WGPUTextureFormat_RG16Unorm },
    { "rg16snorm", WGPUTextureFormat_RG16Snorm },
    { "rg16uint", WGPUTextureFormat_RG16Uint },
    { "rg16sint", WGPUTextureFormat_RG16Sint },
    { "rg16float", WGPUTextureFormat_RG16Float },
    { "rgba8unorm", WGPUTextureFormat_RGBA8Unorm },
    { "rgba8unorm-srgb", WGPUTextureFormat_RGBA8UnormSrgb },
    { "rgba8snorm", WGPUTextureFormat_RGBA8Snorm },
    { "rgba8uint", WGPUTextureFormat_RGBA8Uint },
    { "rgba8sint", WGPUTextureFormat_RGBA8Sint },
    { "bgra8unorm", WGPUTextureFormat_BGRA8Unorm },
    { "bgra8unorm-srgb", WGPUTextureFormat_BGRA8UnormSrgb },
    { "rgb10a2uint", WGPUTextureFormat_RGB10A2Uint },
    { "rgb10a2unorm", WGPUTextureFormat_RGB10A2Unorm },
    { "rg11b10ufloat", WGPUTextureFormat_RG11B10Ufloat },
    { "rgb9e5ufloat", WGPUTextureFormat_RGB9E5Ufloat },
    { "rg32float", WGPUTextureFormat_RG32Float },
    { "rg32uint", WGPUTextureFormat_RG32Uint },
    { "rg32sint", WGPUTextureFormat_RG32Sint },
    { "rgba16unorm", WGPUTextureFormat_RGBA16Unorm },
    { "rgba16snorm", WGPUTextureFormat_RGBA16Snorm },
    { "rgba16uint", WGPUTextureFormat_RGBA16Uint },
    { "rgba16sint", WGPUTextureFormat_RGBA16Sint },
    { "rgba16float", WGPUTextureFormat_RGBA16Float },
    { "rgba32float", WGPUTextureFormat_RGBA32Float },
    { "rgba32uint", WGPUTextureFormat_RGBA32Uint },
    { "rgba32sint", WGPUTextureFormat_RGBA32Sint },
    { "stencil8", WGPUTextureFormat_Stencil8 },
    { "depth16unorm", WGPUTextureFormat_Depth16Unorm },
    { "depth24plus", WGPUTextureFormat_Depth24Plus },
    { "depth24plus-stencil8", WGPUTextureFormat_Depth24PlusStencil8 },
    { "depth32float", WGPUTextureFormat_Depth32Float },
    { "depth32float-stencil8", WGPUTextureFormat_Depth32FloatStencil8 },
    { "bc1-rgba-unorm", WGPUTextureFormat_BC1RGBAUnorm },
    { "bc1-rgba-unorm-srgb", WGPUTextureFormat_BC1RGBAUnormSrgb },
    { "bc2-rgba-unorm", WGPUTextureFormat_BC2RGBAUnorm },
    { "bc2-rgba-unorm-srgb", WGPUTextureFormat_BC2RGBAUnormSrgb },
    { "bc3-rgba-unorm", WGPUTextureFormat_BC3RGBAUnorm },
    { "bc3-rgba-unorm-srgb", WGPUTextureFormat_BC3RGBAUnormSrgb },
    { "bc4-r-unorm", WGPUTextureFormat_BC4RUnorm },
    { "bc4-r-snorm", WGPUTextureFormat_BC4RSnorm },
    { "bc5-rg-unorm", WGPUTextureFormat_BC5RGUnorm },
    { "bc5-rg-snorm", WGPUTextureFormat_BC5RGSnorm },
    { "bc6h-rgb-ufloat", WGPUTextureFormat_BC6HRGBUfloat },
    { "bc6h-rgb-float", WGPUTextureFormat_BC6HRGBFloat },
    { "bc7-rgba-unorm", WGPUTextureFormat_BC7RGBAUnorm },
    { "bc7-rgba-unorm-srgb", WGPUTextureFormat_BC7RGBAUnormSrgb },
    { "etc2-rgb8unorm", WGPUTextureFormat_ETC2RGB8Unorm },
    { "etc2-rgb8unorm-srgb", WGPUTextureFormat_ETC2RGB8UnormSrgb },
    { "etc2-rgb8a1unorm", WGPUTextureFormat_ETC2RGB8A1Unorm },
    { "etc2-rgb8a1unorm-srgb", WGPUTextureFormat_ETC2RGB8A1UnormSrgb },
    { "etc2-rgba8unorm", WGPUTextureFormat_ETC2RGBA8Unorm },
    { "etc2-rgba8unorm-srgb", WGPUTextureFormat_ETC2RGBA8UnormSrgb },
    { "eac-r11unorm", WGPUTextureFormat_EACR11Unorm },
    { "eac-r11snorm", WGPUTextureFormat_EACR11Snorm },
    { "eac-rg11unorm", WGPUTextureFormat_EACRG11Unorm },
    { "eac-rg11snorm", WGPUTextureFormat_EACRG11Snorm },
    { "astc-4x4-unorm", WGPUTextureFormat_ASTC4x4Unorm },
    { "astc-4x4-unorm-srgb", WGPUTextureFormat_ASTC4x4UnormSrgb },
    { "astc-5x4-unorm", WGPUTextureFormat_ASTC5x4Unorm },
    { "astc-5x4-unorm-srgb", WGPUTextureFormat_ASTC5x4UnormSrgb },
    { "astc-5x5-unorm", WGPUTextureFormat_ASTC5x5Unorm },
    { "astc-5x5-unorm-srgb", WGPUTextureFormat_ASTC5x5UnormSrgb },
    { "astc-6x5-unorm", WGPUTextureFormat_ASTC6x5Unorm },
    { "astc-6x5-unorm-srgb", WGPUTextureFormat_ASTC6x5UnormSrgb },
    { "astc-6x6-unorm", WGPUTextureFormat_ASTC6x6Unorm },
    { "astc-6x6-unorm-srgb", WGPUTextureFormat_ASTC6x6UnormSrgb },
    { "astc-8x5-unorm", WGPUTextureFormat_ASTC8x5Unorm },
    { "astc-8x5-unorm-srgb", WGPUTextureFormat_ASTC8x5UnormSrgb },
    { "astc-8x6-unorm", WGPUTextureFormat_ASTC8x6Unorm },
    { "astc-8x6-unorm-srgb", WGPUTextureFormat_ASTC8x6UnormSrgb },
    { "astc-8x8-unorm", WGPUTextureFormat_ASTC8x8Unorm },
    { "astc-8x8-unorm-srgb", WGPUTextureFormat_ASTC8x8UnormSrgb },
    { "astc-10x5-unorm", WGPUTextureFormat_ASTC10x5Unorm },
    { "astc-10x5-unorm-srgb", WGPUTextureFormat_ASTC10x5UnormSrgb },
    { "astc-10x6-unorm", WGPUTextureFormat_ASTC10x6Unorm },
    { "astc-10x6-unorm-srgb", WGPUTextureFormat_ASTC10x6UnormSrgb },
    { "astc-10x8-unorm", WGPUTextureFormat_ASTC10x8Unorm },
    { "astc-10x8-unorm-srgb", WGPUTextureFormat_ASTC10x8UnormSrgb },
    { "astc-10x10-unorm", WGPUTextureFormat_ASTC10x10Unorm },
    { "astc-10x10-unorm-srgb", WGPUTextureFormat_ASTC10x10UnormSrgb },
    { "astc-12x10-unorm", WGPUTextureFormat_ASTC12x10Unorm },
    { "astc-12x10-unorm-srgb", WGPUTextureFormat_ASTC12x10UnormSrgb },
    { "astc-12x12-unorm", WGPUTextureFormat_ASTC12x12Unorm },
    { "astc-12x12-unorm-srgb", WGPUTextureFormat_ASTC12x12UnormSrgb },
};

static WGPUTextureFormat
wg_format_from_str(const char *s)
{
    for (size_t i = 0; s && i < G_N_ELEMENTS(wg_texture_formats); i++)
        if (strcmp(s, wg_texture_formats[i].name) == 0)
            return wg_texture_formats[i].fmt;
    return WGPUTextureFormat_Undefined;
}

static const char *
wg_format_name(WGPUTextureFormat fmt)
{
    for (size_t i = 0; i < G_N_ELEMENTS(wg_texture_formats); i++)
        if (wg_texture_formats[i].fmt == fmt) return wg_texture_formats[i].name;
    return "";
}

typedef struct { const char *name; int value; } wg_enum_name;

static const wg_enum_name wg_view_dimension_names[] = {
    { "1d", WGPUTextureViewDimension_1D },
    { "2d", WGPUTextureViewDimension_2D },
    { "2d-array", WGPUTextureViewDimension_2DArray },
    { "cube", WGPUTextureViewDimension_Cube },
    { "cube-array", WGPUTextureViewDimension_CubeArray },
    { "3d", WGPUTextureViewDimension_3D },
};

static const wg_enum_name wg_texture_dimension_names[] = {
    { "1d", WGPUTextureDimension_1D },
    { "2d", WGPUTextureDimension_2D },
    { "3d", WGPUTextureDimension_3D },
};

static const wg_enum_name wg_aspect_names[] = {
    { "all", WGPUTextureAspect_All },
    { "stencil-only", WGPUTextureAspect_StencilOnly },
    { "depth-only", WGPUTextureAspect_DepthOnly },
};

static JSValue
wg_throw_enum(JSContext *ctx, const char *value, const char *type)
{
    return JS_ThrowTypeError(ctx, "The provided value '%s' is not a valid enum value of type %s.",
                             value, type);
}

static gboolean
wg_enum_value(JSContext *ctx, JSValueConst v, const char *type,
              const wg_enum_name *names, size_t count, int *out)
{
    const char *s = JS_ToCString(ctx, v);
    if (!s) return FALSE;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(s, names[i].name) == 0) {
            *out = names[i].value;
            JS_FreeCString(ctx, s);
            return TRUE;
        }
    }
    wg_throw_enum(ctx, s, type);
    JS_FreeCString(ctx, s);
    return FALSE;
}

static gboolean
wg_enum_member(JSContext *ctx, JSValueConst dict, const char *key,
               const char *type, const wg_enum_name *names, size_t count,
               int *out)
{
    JSValue v = JS_GetPropertyStr(ctx, dict, key);
    if (JS_IsException(v)) return FALSE;
    gboolean ok = JS_IsUndefined(v) ||
                  wg_enum_value(ctx, v, type, names, count, out);
    JS_FreeValue(ctx, v);
    return ok;
}

static gboolean
wg_format_value(JSContext *ctx, JSValueConst v, WGPUTextureFormat *out)
{
    const char *s = JS_ToCString(ctx, v);
    if (!s) return FALSE;
    *out = wg_format_from_str(s);
    if (*out == WGPUTextureFormat_Undefined)
        wg_throw_enum(ctx, s, "GPUTextureFormat");
    JS_FreeCString(ctx, s);
    return *out != WGPUTextureFormat_Undefined;
}

static gboolean
wg_format_member(JSContext *ctx, JSValueConst dict, const char *key,
                 gboolean required, WGPUTextureFormat *out)
{
    *out = WGPUTextureFormat_Undefined;
    JSValue v = JS_GetPropertyStr(ctx, dict, key);
    if (JS_IsException(v)) return FALSE;
    gboolean ok;
    if (JS_IsUndefined(v)) {
        ok = !required;
        if (required)
            JS_ThrowTypeError(ctx, "Required member '%s' is undefined.", key);
    } else {
        ok = wg_format_value(ctx, v, out);
    }
    JS_FreeValue(ctx, v);
    return ok;
}

#define NS_WG_MAX_VIEW_FORMATS 8

static gboolean
wg_read_view_formats(JSContext *ctx, JSValueConst desc, WGPUTextureFormat *out,
                     size_t *count)
{
    *count = 0;
    JSValue v = JS_GetPropertyStr(ctx, desc, "viewFormats");
    if (JS_IsException(v)) return FALSE;
    gboolean ok = TRUE;
    if (JS_IsObject(v)) {
        JSValue list = wg_array_from(ctx, v);
        uint32_t len = 0;
        if (JS_IsException(list)) {
            ok = FALSE;
        } else {
            JSValue jl = JS_GetPropertyStr(ctx, list, "length");
            JS_ToUint32(ctx, &len, jl);
            JS_FreeValue(ctx, jl);
        }
        for (uint32_t i = 0; ok && i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, list, i);
            WGPUTextureFormat f = WGPUTextureFormat_Undefined;
            ok = wg_format_value(ctx, e, &f);
            if (ok && *count < NS_WG_MAX_VIEW_FORMATS) out[(*count)++] = f;
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, list);
    }
    JS_FreeValue(ctx, v);
    return ok;
}

static gboolean
wg_format_has_depth(WGPUTextureFormat f)
{
    return f == WGPUTextureFormat_Depth16Unorm ||
           f == WGPUTextureFormat_Depth24Plus ||
           f == WGPUTextureFormat_Depth24PlusStencil8 ||
           f == WGPUTextureFormat_Depth32Float ||
           f == WGPUTextureFormat_Depth32FloatStencil8;
}

static WGPUCompareFunction
wg_compare_func(const char *s)
{
    if (!s) return WGPUCompareFunction_Undefined;
    if (strcmp(s, "never") == 0) return WGPUCompareFunction_Never;
    if (strcmp(s, "less") == 0) return WGPUCompareFunction_Less;
    if (strcmp(s, "equal") == 0) return WGPUCompareFunction_Equal;
    if (strcmp(s, "less-equal") == 0) return WGPUCompareFunction_LessEqual;
    if (strcmp(s, "greater") == 0) return WGPUCompareFunction_Greater;
    if (strcmp(s, "greater-equal") == 0) return WGPUCompareFunction_GreaterEqual;
    if (strcmp(s, "always") == 0) return WGPUCompareFunction_Always;
    return WGPUCompareFunction_Undefined;
}

static WGPUAddressMode
wg_address_mode(const char *s)
{
    if (s && strcmp(s, "repeat") == 0) return WGPUAddressMode_Repeat;
    if (s && strcmp(s, "mirror-repeat") == 0) return WGPUAddressMode_MirrorRepeat;
    return WGPUAddressMode_ClampToEdge;
}

static WGPUBlendFactor
wg_blend_factor(const char *s)
{
    if (!s) return WGPUBlendFactor_One;
    static const struct { const char *n; WGPUBlendFactor f; } m[] = {
        { "zero", WGPUBlendFactor_Zero },
        { "one", WGPUBlendFactor_One },
        { "src", WGPUBlendFactor_Src },
        { "one-minus-src", WGPUBlendFactor_OneMinusSrc },
        { "src-alpha", WGPUBlendFactor_SrcAlpha },
        { "one-minus-src-alpha", WGPUBlendFactor_OneMinusSrcAlpha },
        { "dst", WGPUBlendFactor_Dst },
        { "one-minus-dst", WGPUBlendFactor_OneMinusDst },
        { "dst-alpha", WGPUBlendFactor_DstAlpha },
        { "one-minus-dst-alpha", WGPUBlendFactor_OneMinusDstAlpha },
        { "src-alpha-saturated", WGPUBlendFactor_SrcAlphaSaturated },
        { "constant", WGPUBlendFactor_Constant },
        { "one-minus-constant", WGPUBlendFactor_OneMinusConstant },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(m); i++)
        if (strcmp(s, m[i].n) == 0) return m[i].f;
    return WGPUBlendFactor_One;
}

static WGPUBlendOperation
wg_blend_op(const char *s)
{
    if (!s) return WGPUBlendOperation_Add;
    if (strcmp(s, "subtract") == 0) return WGPUBlendOperation_Subtract;
    if (strcmp(s, "reverse-subtract") == 0) return WGPUBlendOperation_ReverseSubtract;
    if (strcmp(s, "min") == 0) return WGPUBlendOperation_Min;
    if (strcmp(s, "max") == 0) return WGPUBlendOperation_Max;
    return WGPUBlendOperation_Add;
}

static void
wg_read_blend_component(JSContext *ctx, JSValueConst v, WGPUBlendComponent *out)
{
    out->operation = WGPUBlendOperation_Add;
    out->srcFactor = WGPUBlendFactor_One;
    out->dstFactor = WGPUBlendFactor_Zero;
    if (!JS_IsObject(v)) return;
    JSValue jo = JS_GetPropertyStr(ctx, v, "operation");
    const char *os = JS_IsString(jo) ? JS_ToCString(ctx, jo) : NULL;
    out->operation = wg_blend_op(os);
    if (os) JS_FreeCString(ctx, os);
    JS_FreeValue(ctx, jo);
    JSValue js = JS_GetPropertyStr(ctx, v, "srcFactor");
    const char *ss = JS_IsString(js) ? JS_ToCString(ctx, js) : NULL;
    out->srcFactor = wg_blend_factor(ss);
    if (ss) JS_FreeCString(ctx, ss);
    JS_FreeValue(ctx, js);
    JSValue jd = JS_GetPropertyStr(ctx, v, "dstFactor");
    const char *ds = JS_IsString(jd) ? JS_ToCString(ctx, jd) : NULL;
    out->dstFactor = ds ? wg_blend_factor(ds) : WGPUBlendFactor_Zero;
    if (ds) JS_FreeCString(ctx, ds);
    JS_FreeValue(ctx, jd);
}

static const char *
wg_view_dimension_name(WGPUTextureViewDimension d)
{
    switch (d) {
    case WGPUTextureViewDimension_1D: return "1d";
    case WGPUTextureViewDimension_2DArray: return "2d-array";
    case WGPUTextureViewDimension_Cube: return "cube";
    case WGPUTextureViewDimension_CubeArray: return "cube-array";
    case WGPUTextureViewDimension_3D: return "3d";
    default: return "2d";
    }
}

static void
wg_view_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_view *v = JS_GetOpaque(val, g_view_class);
    if (!v) return;
    if (v->view) wgpuTextureViewRelease(v->view);
    g_free(v);
}

static JSValue
wg_make_view(JSContext *ctx, WGPUTextureView view)
{
    JSValue obj = JS_NewObjectClass(ctx, g_view_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_view *v = g_new0(ns_wg_view, 1);
    v->view = view;
    JS_SetOpaque(obj, v);
    return obj;
}

static JSValue
wg_texture_createView(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    ns_wg_texture *t = JS_GetOpaque(this_val, g_texture_class);
    if (!t || !t->texture) return JS_UNDEFINED;

    WGPUTextureView view;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        WGPUTextureViewDescriptor desc;
        memset(&desc, 0, sizeof desc);
        desc.mipLevelCount = WGPU_MIP_LEVEL_COUNT_UNDEFINED;
        desc.arrayLayerCount = WGPU_ARRAY_LAYER_COUNT_UNDEFINED;
        int dimension = WGPUTextureViewDimension_Undefined;
        int aspect = WGPUTextureAspect_All;
        if (!wg_format_member(ctx, argv[0], "format", FALSE, &desc.format) ||
            !wg_enum_member(ctx, argv[0], "dimension", "GPUTextureViewDimension",
                            wg_view_dimension_names,
                            G_N_ELEMENTS(wg_view_dimension_names), &dimension) ||
            !wg_enum_member(ctx, argv[0], "aspect", "GPUTextureAspect",
                            wg_aspect_names, G_N_ELEMENTS(wg_aspect_names),
                            &aspect))
            return JS_EXCEPTION;
        desc.dimension = (WGPUTextureViewDimension)dimension;
        desc.aspect = (WGPUTextureAspect)aspect;
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[0], "baseMipLevel");
        if (!JS_IsUndefined(v)) JS_ToUint32(ctx, &desc.baseMipLevel, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "mipLevelCount");
        if (!JS_IsUndefined(v)) JS_ToUint32(ctx, &desc.mipLevelCount, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "baseArrayLayer");
        if (!JS_IsUndefined(v)) JS_ToUint32(ctx, &desc.baseArrayLayer, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "arrayLayerCount");
        if (!JS_IsUndefined(v)) JS_ToUint32(ctx, &desc.arrayLayerCount, v);
        JS_FreeValue(ctx, v);
        if (desc.mipLevelCount == 0 || desc.arrayLayerCount == 0)
            return JS_ThrowTypeError(ctx, "createView: mipLevelCount and arrayLayerCount must not be 0");
        view = wgpuTextureCreateView(t->texture, &desc);
    } else {
        view = wgpuTextureCreateView(t->texture, NULL);
    }
    if (!view) return JS_UNDEFINED;
    return wg_make_view(ctx, view);
}

static JSValue
wg_texture_destroy(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    ns_wg_texture *t = JS_GetOpaque(this_val, g_texture_class);
    if (t && t->texture) wgpuTextureDestroy(t->texture);
    return JS_UNDEFINED;
}

static void
wg_texture_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_texture *t = JS_GetOpaque(val, g_texture_class);
    if (!t) return;
    if (t->texture) wgpuTextureRelease(t->texture);
    g_free(t);
}

static JSValue
wg_make_texture(JSContext *ctx, WGPUTexture texture)
{
    JSValue obj = JS_NewObjectClass(ctx, g_texture_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_texture *t = g_new0(ns_wg_texture, 1);
    t->texture = texture;
    t->w = wgpuTextureGetWidth(texture);
    t->h = wgpuTextureGetHeight(texture);
    t->format = wgpuTextureGetFormat(texture);
    JS_SetOpaque(obj, t);
    WGPUTextureDimension dim = wgpuTextureGetDimension(texture);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewUint32(ctx, t->w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewUint32(ctx, t->h));
    JS_SetPropertyStr(ctx, obj, "depthOrArrayLayers",
                      JS_NewUint32(ctx, wgpuTextureGetDepthOrArrayLayers(texture)));
    JS_SetPropertyStr(ctx, obj, "mipLevelCount",
                      JS_NewUint32(ctx, wgpuTextureGetMipLevelCount(texture)));
    JS_SetPropertyStr(ctx, obj, "sampleCount",
                      JS_NewUint32(ctx, wgpuTextureGetSampleCount(texture)));
    JS_SetPropertyStr(ctx, obj, "dimension",
                      JS_NewString(ctx, dim == WGPUTextureDimension_1D ? "1d"
                                      : dim == WGPUTextureDimension_3D ? "3d" : "2d"));
    JS_SetPropertyStr(ctx, obj, "usage",
                      JS_NewUint32(ctx, (uint32_t)wgpuTextureGetUsage(texture)));
    uint32_t layers = wgpuTextureGetDepthOrArrayLayers(texture);
    WGPUTextureViewDimension bvd = dim == WGPUTextureDimension_1D
        ? WGPUTextureViewDimension_1D
        : dim == WGPUTextureDimension_3D ? WGPUTextureViewDimension_3D
        : layers > 1 ? WGPUTextureViewDimension_2DArray
                     : WGPUTextureViewDimension_2D;
    JS_SetPropertyStr(ctx, obj, "textureBindingViewDimension",
                      JS_NewString(ctx, wg_view_dimension_name(bvd)));
    JS_SetPropertyStr(ctx, obj, "format",
                      JS_NewString(ctx, wg_format_name(t->format)));
    JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, ""));
    return obj;
}

static void
wg_pass_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_pass *p = JS_GetOpaque(val, g_pass_class);
    if (!p) return;
    if (p->pass) wgpuRenderPassEncoderRelease(p->pass);
    g_free(p);
}

static JSValue
wg_pass_end(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (p && p->pass) wgpuRenderPassEncoderEnd(p->pass);
    return JS_UNDEFINED;
}

static double
wg_arg_f64(JSContext *ctx, int argc, JSValueConst *argv, int i, double defv)
{
    double v = defv;
    if (i < argc && !JS_IsUndefined(argv[i])) JS_ToFloat64(ctx, &v, argv[i]);
    return v;
}

static uint32_t
wg_arg_u32(JSContext *ctx, int argc, JSValueConst *argv, int i, uint32_t defv)
{
    uint32_t v = defv;
    if (i < argc && !JS_IsUndefined(argv[i])) JS_ToUint32(ctx, &v, argv[i]);
    return v;
}

static JSValue
wg_pass_setViewport(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass || argc < 6) return JS_UNDEFINED;
    float v[6];
    for (int i = 0; i < 6; i++) v[i] = (float)wg_arg_f64(ctx, argc, argv, i, 0);
    wgpuRenderPassEncoderSetViewport(p->pass, v[0], v[1], v[2], v[3], v[4], v[5]);
    return JS_UNDEFINED;
}

static JSValue
wg_pass_setScissorRect(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass || argc < 4) return JS_UNDEFINED;
    wgpuRenderPassEncoderSetScissorRect(p->pass,
        wg_arg_u32(ctx, argc, argv, 0, 0), wg_arg_u32(ctx, argc, argv, 1, 0),
        wg_arg_u32(ctx, argc, argv, 2, 0), wg_arg_u32(ctx, argc, argv, 3, 0));
    return JS_UNDEFINED;
}

static double wg_color_component(JSContext *ctx, JSValueConst color,
                                 const char *key, int idx);

static JSValue
wg_pass_beginOcclusionQuery(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass) return JS_UNDEFINED;
    wgpuRenderPassEncoderBeginOcclusionQuery(p->pass,
                                             wg_arg_u32(ctx, argc, argv, 0, 0));
    return JS_UNDEFINED;
}

static JSValue
wg_pass_endOcclusionQuery(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (p && p->pass) wgpuRenderPassEncoderEndOcclusionQuery(p->pass);
    return JS_UNDEFINED;
}

static JSValue
wg_debug_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static JSValue
wg_pass_setBlendConstant(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass || argc < 1) return JS_UNDEFINED;
    WGPUColor c = {
        wg_color_component(ctx, argv[0], "r", 0),
        wg_color_component(ctx, argv[0], "g", 1),
        wg_color_component(ctx, argv[0], "b", 2),
        wg_color_component(ctx, argv[0], "a", 3),
    };
    wgpuRenderPassEncoderSetBlendConstant(p->pass, &c);
    return JS_UNDEFINED;
}

static JSValue
wg_pass_setStencilReference(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass) return JS_UNDEFINED;
    wgpuRenderPassEncoderSetStencilReference(p->pass,
                                             wg_arg_u32(ctx, argc, argv, 0, 0));
    return JS_UNDEFINED;
}

static JSValue
wg_pass_setPipeline(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    (void)ctx;
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass || argc < 1) return JS_UNDEFINED;
    ns_wg_pipeline *pl = JS_GetOpaque(argv[0], g_pipeline_class);
    if (pl && pl->pipe) wgpuRenderPassEncoderSetPipeline(p->pass, pl->pipe);
    return JS_UNDEFINED;
}

static JSValue
wg_pass_setVertexBuffer(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass || argc < 2) return JS_UNDEFINED;
    int32_t slot = 0;
    JS_ToInt32(ctx, &slot, argv[0]);
    ns_wg_buffer *b = JS_GetOpaque(argv[1], g_buffer_class);
    if (!b || !b->buffer) return JS_UNDEFINED;
    int64_t offset = 0;
    if (argc >= 3) JS_ToInt64(ctx, &offset, argv[2]);
    uint64_t size = b->size > (uint64_t)offset ? b->size - (uint64_t)offset : 0;
    if (argc >= 4 && !JS_IsUndefined(argv[3])) {
        int64_t s = 0; JS_ToInt64(ctx, &s, argv[3]);
        if (s > 0) size = (uint64_t)s;
    }
    wgpuRenderPassEncoderSetVertexBuffer(p->pass, (uint32_t)slot, b->buffer,
                                         (uint64_t)offset, size);
    return JS_UNDEFINED;
}

static JSValue
wg_pass_setIndexBuffer(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass || argc < 2) return JS_UNDEFINED;
    ns_wg_buffer *b = JS_GetOpaque(argv[0], g_buffer_class);
    if (!b || !b->buffer) return JS_UNDEFINED;
    WGPUIndexFormat fmt = WGPUIndexFormat_Uint32;
    const char *fs = JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    if (fs && strcmp(fs, "uint16") == 0) fmt = WGPUIndexFormat_Uint16;
    if (fs) JS_FreeCString(ctx, fs);
    int64_t offset = 0;
    if (argc >= 3) JS_ToInt64(ctx, &offset, argv[2]);
    uint64_t size = b->size > (uint64_t)offset ? b->size - (uint64_t)offset : 0;
    if (argc >= 4 && !JS_IsUndefined(argv[3])) {
        int64_t s = 0; JS_ToInt64(ctx, &s, argv[3]);
        if (s > 0) size = (uint64_t)s;
    }
    wgpuRenderPassEncoderSetIndexBuffer(p->pass, b->buffer, fmt,
                                        (uint64_t)offset, size);
    return JS_UNDEFINED;
}

#define NS_WG_MAX_DYNAMIC_OFFSETS 32

static size_t
wg_read_dynamic_offsets(JSContext *ctx, int argc, JSValueConst *argv,
                        uint32_t *out)
{
    if (argc < 3 || !JS_IsObject(argv[2])) return 0;
    int64_t start = 0, count = -1;
    if (argc >= 4 && JS_ToInt64(ctx, &start, argv[3])) return 0;
    if (argc >= 5 && JS_ToInt64(ctx, &count, argv[4])) return 0;
    size_t view_off = 0, view_len = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[2], &view_off, &view_len, &bpe);
    if (!JS_IsException(abuf)) {
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, abuf);
        JS_FreeValue(ctx, abuf);
        if (!base || bpe != 4 || view_off > total || view_len > total - view_off)
            return 0;
        const uint8_t *data = base + view_off;
        size_t len = view_len / 4;
        if (argc < 5) count = (int64_t)len;
        if (start < 0 || count < 0 || (uint64_t)start + (uint64_t)count > len)
            return 0;
        if (count > NS_WG_MAX_DYNAMIC_OFFSETS) count = NS_WG_MAX_DYNAMIC_OFFSETS;
        memcpy(out, data + (size_t)start * 4, (size_t)count * 4);
        return (size_t)count;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    uint32_t len = 0;
    JSValue jl = JS_GetPropertyStr(ctx, argv[2], "length");
    JS_ToUint32(ctx, &len, jl);
    JS_FreeValue(ctx, jl);
    if (len > NS_WG_MAX_DYNAMIC_OFFSETS) len = NS_WG_MAX_DYNAMIC_OFFSETS;
    for (uint32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[2], i);
        out[i] = 0;
        JS_ToUint32(ctx, &out[i], e);
        JS_FreeValue(ctx, e);
    }
    return len;
}

static JSValue
wg_pass_setBindGroup(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass || argc < 2) return JS_UNDEFINED;
    uint32_t index = 0;
    JS_ToUint32(ctx, &index, argv[0]);
    ns_wg_bindgroup *bg = JS_GetOpaque(argv[1], g_bindgroup_class);
    uint32_t offsets[NS_WG_MAX_DYNAMIC_OFFSETS];
    size_t n = wg_read_dynamic_offsets(ctx, argc, argv, offsets);
    wgpuRenderPassEncoderSetBindGroup(p->pass, index,
                                      bg ? bg->group : NULL, n, offsets);
    return JS_UNDEFINED;
}

static JSValue
wg_pass_draw(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass) return JS_UNDEFINED;
    uint32_t vc = 0, ic = 1, fv = 0, fi = 0;
    if (argc >= 1) JS_ToUint32(ctx, &vc, argv[0]);
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToUint32(ctx, &ic, argv[1]);
    if (argc >= 3) JS_ToUint32(ctx, &fv, argv[2]);
    if (argc >= 4) JS_ToUint32(ctx, &fi, argv[3]);
    wgpuRenderPassEncoderDraw(p->pass, vc, ic, fv, fi);
    return JS_UNDEFINED;
}

static ns_wg_buffer *
wg_indirect_args(JSContext *ctx, int argc, JSValueConst *argv, uint64_t *offset)
{
    if (argc < 1) return NULL;
    ns_wg_buffer *b = JS_GetOpaque(argv[0], g_buffer_class);
    int64_t off = 0;
    if (argc >= 2) JS_ToInt64(ctx, &off, argv[1]);
    *offset = off > 0 ? (uint64_t)off : 0;
    return b && b->buffer ? b : NULL;
}

static JSValue
wg_pass_drawIndirect(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    uint64_t off = 0;
    ns_wg_buffer *b = wg_indirect_args(ctx, argc, argv, &off);
    if (p && p->pass && b) wgpuRenderPassEncoderDrawIndirect(p->pass, b->buffer, off);
    return JS_UNDEFINED;
}

static JSValue
wg_pass_drawIndexedIndirect(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    uint64_t off = 0;
    ns_wg_buffer *b = wg_indirect_args(ctx, argc, argv, &off);
    if (p && p->pass && b)
        wgpuRenderPassEncoderDrawIndexedIndirect(p->pass, b->buffer, off);
    return JS_UNDEFINED;
}

static JSValue
wg_pass_drawIndexed(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass) return JS_UNDEFINED;
    uint32_t icnt = 0, inst = 1, fi = 0, ff = 0;
    int32_t bv = 0;
    if (argc >= 1) JS_ToUint32(ctx, &icnt, argv[0]);
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToUint32(ctx, &inst, argv[1]);
    if (argc >= 3) JS_ToUint32(ctx, &fi, argv[2]);
    if (argc >= 4) JS_ToInt32(ctx, &bv, argv[3]);
    if (argc >= 5) JS_ToUint32(ctx, &ff, argv[4]);
    wgpuRenderPassEncoderDrawIndexed(p->pass, icnt, inst, fi, bv, ff);
    return JS_UNDEFINED;
}

static JSValue
wg_make_pass(JSContext *ctx, WGPURenderPassEncoder pass)
{
    JSValue obj = JS_NewObjectClass(ctx, g_pass_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_pass *p = g_new0(ns_wg_pass, 1);
    p->pass = pass;
    JS_SetOpaque(obj, p);
    return obj;
}

static void
wg_cmdbuf_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_cmdbuf *c = JS_GetOpaque(val, g_cmdbuf_class);
    if (!c) return;
    if (c->cmd) wgpuCommandBufferRelease(c->cmd);
    g_free(c);
}

static JSValue
wg_make_cmdbuf(JSContext *ctx, WGPUCommandBuffer cmd)
{
    JSValue obj = JS_NewObjectClass(ctx, g_cmdbuf_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_cmdbuf *c = g_new0(ns_wg_cmdbuf, 1);
    c->cmd = cmd;
    JS_SetOpaque(obj, c);
    return obj;
}

static double
wg_color_component(JSContext *ctx, JSValueConst color, const char *key, int idx)
{
    double out = 0;
    if (JS_IsArray(color)) {
        JSValue e = JS_GetPropertyUint32(ctx, color, (uint32_t)idx);
        JS_ToFloat64(ctx, &out, e);
        JS_FreeValue(ctx, e);
    } else if (JS_IsObject(color)) {
        JSValue e = JS_GetPropertyStr(ctx, color, key);
        JS_ToFloat64(ctx, &out, e);
        JS_FreeValue(ctx, e);
    }
    return out;
}

static WGPULoadOp
wg_load_op(JSContext *ctx, JSValueConst obj, const char *key)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    const char *s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    WGPULoadOp op = !s ? WGPULoadOp_Undefined
                  : strcmp(s, "load") == 0 ? WGPULoadOp_Load : WGPULoadOp_Clear;
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return op;
}

static WGPUStoreOp
wg_store_op(JSContext *ctx, JSValueConst obj, const char *key)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    const char *s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    WGPUStoreOp op = !s ? WGPUStoreOp_Undefined
                   : strcmp(s, "discard") == 0 ? WGPUStoreOp_Discard
                                               : WGPUStoreOp_Store;
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return op;
}

static gboolean
wg_read_color_attachment(JSContext *ctx, JSValueConst a,
                         WGPURenderPassColorAttachment *color, wg_hold *hold)
{
    color->depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    if (!JS_IsObject(a)) return FALSE;
    JSValue jview = JS_GetPropertyStr(ctx, a, "view");
    ns_wg_view *vw = wg_hold_opaque(hold, jview, g_view_class);
    JS_FreeValue(ctx, jview);
    if (!vw) return FALSE;
    color->view = vw->view;
    color->loadOp = wg_load_op(ctx, a, "loadOp");
    if (color->loadOp == WGPULoadOp_Undefined) color->loadOp = WGPULoadOp_Clear;
    color->storeOp = wg_store_op(ctx, a, "storeOp");
    if (color->storeOp == WGPUStoreOp_Undefined) color->storeOp = WGPUStoreOp_Store;
    JSValue jslice = JS_GetPropertyStr(ctx, a, "depthSlice");
    if (!JS_IsUndefined(jslice)) JS_ToUint32(ctx, &color->depthSlice, jslice);
    JS_FreeValue(ctx, jslice);
    JSValue jclear = JS_GetPropertyStr(ctx, a, "clearValue");
    color->clearValue.r = wg_color_component(ctx, jclear, "r", 0);
    color->clearValue.g = wg_color_component(ctx, jclear, "g", 1);
    color->clearValue.b = wg_color_component(ctx, jclear, "b", 2);
    color->clearValue.a = wg_color_component(ctx, jclear, "a", 3);
    JS_FreeValue(ctx, jclear);
    JSValue jresolve = JS_GetPropertyStr(ctx, a, "resolveTarget");
    ns_wg_view *rv = wg_hold_opaque(hold, jresolve, g_view_class);
    if (rv) color->resolveTarget = rv->view;
    JS_FreeValue(ctx, jresolve);
    return TRUE;
}

static gboolean
wg_read_depth_attachment(JSContext *ctx, JSValueConst jds,
                         WGPURenderPassDepthStencilAttachment *depth,
                         wg_hold *hold)
{
    if (!JS_IsObject(jds)) return FALSE;
    JSValue jview = JS_GetPropertyStr(ctx, jds, "view");
    ns_wg_view *dv = wg_hold_opaque(hold, jview, g_view_class);
    JS_FreeValue(ctx, jview);
    if (!dv) return FALSE;
    depth->view = dv->view;
    depth->depthLoadOp = wg_load_op(ctx, jds, "depthLoadOp");
    depth->depthStoreOp = wg_store_op(ctx, jds, "depthStoreOp");
    depth->stencilLoadOp = wg_load_op(ctx, jds, "stencilLoadOp");
    depth->stencilStoreOp = wg_store_op(ctx, jds, "stencilStoreOp");
    depth->depthClearValue = WGPU_DEPTH_CLEAR_VALUE_UNDEFINED;
    JSValue v = JS_GetPropertyStr(ctx, jds, "depthClearValue");
    if (!JS_IsUndefined(v)) {
        double dc = 1.0; JS_ToFloat64(ctx, &dc, v);
        depth->depthClearValue = (float)dc;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, jds, "stencilClearValue");
    if (!JS_IsUndefined(v)) JS_ToUint32(ctx, &depth->stencilClearValue, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, jds, "depthReadOnly");
    depth->depthReadOnly = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, jds, "stencilReadOnly");
    depth->stencilReadOnly = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return TRUE;
}

static JSValue
wg_encoder_beginRenderPass(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    ns_wg_encoder *e = JS_GetOpaque(this_val, g_encoder_class);
    if (!e || !e->enc || argc < 1 || !JS_IsObject(argv[0]))
        return JS_UNDEFINED;

    wg_hold hold = { ctx, NULL };
    WGPURenderPassColorAttachment colors[NS_WG_MAX_COLOR_ATTACHMENTS];
    memset(colors, 0, sizeof colors);
    uint32_t ncolors = 0;
    gboolean any_view = FALSE;
    JSValue atts = JS_GetPropertyStr(ctx, argv[0], "colorAttachments");
    if (JS_IsArray(atts)) {
        JSValue jl = JS_GetPropertyStr(ctx, atts, "length");
        JS_ToUint32(ctx, &ncolors, jl);
        JS_FreeValue(ctx, jl);
        if (ncolors > NS_WG_MAX_COLOR_ATTACHMENTS)
            ncolors = NS_WG_MAX_COLOR_ATTACHMENTS;
        for (uint32_t i = 0; i < ncolors; i++) {
            JSValue a = JS_GetPropertyUint32(ctx, atts, i);
            if (wg_read_color_attachment(ctx, a, &colors[i], &hold))
                any_view = TRUE;
            JS_FreeValue(ctx, a);
        }
    }
    JS_FreeValue(ctx, atts);

    WGPURenderPassDepthStencilAttachment depth;
    memset(&depth, 0, sizeof depth);
    JSValue jds = JS_GetPropertyStr(ctx, argv[0], "depthStencilAttachment");
    gboolean have_depth = wg_read_depth_attachment(ctx, jds, &depth, &hold);
    JS_FreeValue(ctx, jds);

    if (!any_view && !have_depth) {
        wg_hold_release(&hold);
        return JS_UNDEFINED;
    }

    WGPUQuerySet occlusion = NULL;
    JSValue jocc = JS_GetPropertyStr(ctx, argv[0], "occlusionQuerySet");
    ns_wg_queryset *oq = wg_hold_opaque(&hold, jocc, g_queryset_class);
    if (oq) occlusion = oq->qs;
    JS_FreeValue(ctx, jocc);

    WGPURenderPassDescriptor desc;
    memset(&desc, 0, sizeof desc);
    desc.colorAttachmentCount = ncolors;
    desc.colorAttachments = colors;
    if (have_depth) desc.depthStencilAttachment = &depth;
    desc.occlusionQuerySet = occlusion;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(e->enc, &desc);
    wg_hold_release(&hold);
    if (!pass) return JS_UNDEFINED;
    return wg_make_pass(ctx, pass);
}

static JSValue
wg_encoder_finish(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wg_encoder *e = JS_GetOpaque(this_val, g_encoder_class);
    if (!e || !e->enc) return JS_UNDEFINED;
    wgpuDevicePushErrorScope(e->device, WGPUErrorFilter_Validation);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(e->enc, NULL);
    WGPUErrorType type = WGPUErrorType_NoError;
    char *message = NULL;
    if (wg_pop_internal_scope(e->device, &type, &message) &&
        type != WGPUErrorType_NoError) {
        if (e->sink) wg_report_error(e->sink, type, message ? message : "");
        if (cmd) wgpuCommandBufferRelease(cmd);
        cmd = NULL;
    }
    g_free(message);
    return wg_make_cmdbuf(ctx, cmd);
}

static void
wg_encoder_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_encoder *e = JS_GetOpaque(val, g_encoder_class);
    if (!e) return;
    if (e->enc) wgpuCommandEncoderRelease(e->enc);
    if (e->device) wgpuDeviceRelease(e->device);
    g_free(e);
}

static JSValue
wg_make_encoder(JSContext *ctx, WGPUCommandEncoder enc, const ns_wg_device *d)
{
    JSValue obj = JS_NewObjectClass(ctx, g_encoder_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_encoder *e = g_new0(ns_wg_encoder, 1);
    e->enc = enc;
    wgpuDeviceAddRef(d->device);
    e->device = d->device;
    e->sink = d->sink;
    JS_SetOpaque(obj, e);
    return obj;
}

static JSValue
wg_device_createCommandEncoder(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d) return JS_UNDEFINED;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(d->device, NULL);
    if (!enc) return JS_UNDEFINED;
    return wg_make_encoder(ctx, enc, d);
}

static void
wg_ctx_release_gpu(ns_wg_context *c)
{
    if (c->target) { wgpuTextureRelease(c->target); c->target = NULL; }
    if (c->surf) { cairo_surface_destroy(c->surf); c->surf = NULL; }
    if (c->readback) { wgpuBufferRelease(c->readback); c->readback = NULL; }
    c->readback_size = 0;
    c->pending = FALSE;
    JSValue cur = c->current;
    c->current = JS_UNDEFINED;
    if (c->rt) JS_FreeValueRT(c->rt, cur);
}

static gboolean
wg_ctx_ensure_target(ns_wg_context *c)
{
    int w = wg_canvas_dim(c->canvas, "width", 300);
    int h = wg_canvas_dim(c->canvas, "height", 150);
    if (c->target && w == c->w && h == c->h) return TRUE;
    wg_ctx_release_gpu(c);
    c->w = w; c->h = h;

    WGPUTextureDescriptor td;
    memset(&td, 0, sizeof td);
    td.usage = c->usage | WGPUTextureUsage_RenderAttachment |
               WGPUTextureUsage_CopySrc | WGPUTextureUsage_TextureBinding;
    td.viewFormatCount = c->view_format_count;
    td.viewFormats = c->view_formats;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)w;
    td.size.height = (uint32_t)h;
    td.size.depthOrArrayLayers = 1;
    td.format = c->format;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    c->target = wgpuDeviceCreateTexture(c->device, &td);
    return c->target != NULL;
}

static JSValue
wg_ctx_configure(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    ns_wg_context *c = JS_GetOpaque(this_val, g_context_class);
    if (!c || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "configure: descriptor required");

    wg_hold hold = { ctx, NULL };
    JSValue jdev = JS_GetPropertyStr(ctx, argv[0], "device");
    ns_wg_device *d = wg_hold_opaque(&hold, jdev, g_device_class);
    JS_FreeValue(ctx, jdev);
    if (!d) return JS_ThrowTypeError(ctx, "configure: valid device required");

    static const wg_enum_name alpha_modes[] = {
        { "opaque", 0 }, { "premultiplied", 1 },
    };
    WGPUTextureFormat format;
    WGPUTextureFormat view_formats[NS_WG_MAX_VIEW_FORMATS];
    size_t view_format_count = 0;
    int premultiplied = 0;
    if (!wg_format_member(ctx, argv[0], "format", TRUE, &format) ||
        !wg_read_view_formats(ctx, argv[0], view_formats, &view_format_count) ||
        !wg_enum_member(ctx, argv[0], "alphaMode", "GPUCanvasAlphaMode",
                        alpha_modes, G_N_ELEMENTS(alpha_modes), &premultiplied)) {
        wg_hold_release(&hold);
        return JS_EXCEPTION;
    }
    if (format != WGPUTextureFormat_BGRA8Unorm &&
        format != WGPUTextureFormat_RGBA8Unorm &&
        format != WGPUTextureFormat_RGBA16Float) {
        wg_hold_release(&hold);
        return JS_ThrowTypeError(ctx, "configure: unsupported canvas format '%s'",
                                 wg_format_name(format));
    }

    if (c->device) wgpuDeviceRelease(c->device);
    if (c->queue) wgpuQueueRelease(c->queue);
    wgpuDeviceAddRef(d->device);
    c->device = d->device;
    c->queue = wgpuDeviceGetQueue(d->device);
    c->format = format;
    memcpy(c->view_formats, view_formats,
           view_format_count * sizeof view_formats[0]);
    c->view_format_count = view_format_count;
    JSValue jusage = JS_GetPropertyStr(ctx, argv[0], "usage");
    c->usage = WGPUTextureUsage_RenderAttachment;
    if (!JS_IsUndefined(jusage)) {
        uint32_t u = 0; JS_ToUint32(ctx, &u, jusage);
        c->usage = u & 0x1Fu;
    }
    JS_FreeValue(ctx, jusage);
    c->opaque = !premultiplied;
    c->configured = TRUE;
    wg_ctx_release_gpu(c);
    wg_hold_release(&hold);
    return JS_UNDEFINED;
}

static JSValue
wg_ctx_unconfigure(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    ns_wg_context *c = JS_GetOpaque(this_val, g_context_class);
    if (c) { c->configured = FALSE; wg_ctx_release_gpu(c); }
    return JS_UNDEFINED;
}

static JSValue
wg_ctx_getCurrentTexture(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wg_context *c = JS_GetOpaque(this_val, g_context_class);
    if (!c || !c->configured || !c->device)
        return JS_ThrowTypeError(ctx, "InvalidStateError: getCurrentTexture: not configured");
    if (!wg_ctx_ensure_target(c))
        return JS_ThrowTypeError(ctx, "getCurrentTexture: no target");
    c->pending = TRUE;
    if (JS_IsObject(c->current)) return JS_DupValue(ctx, c->current);
    wgpuTextureAddRef(c->target);
    JSValue tex = wg_make_texture(ctx, c->target);
    if (JS_IsObject(tex)) c->current = JS_DupValue(ctx, tex);
    return tex;
}

static JSValue
wg_ctx_getConfiguration(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wg_context *c = JS_GetOpaque(this_val, g_context_class);
    if (!c || !c->configured) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "format", JS_NewString(ctx, wg_format_name(c->format)));
    JS_SetPropertyStr(ctx, o, "usage", JS_NewUint32(ctx, c->usage));
    JS_SetPropertyStr(ctx, o, "alphaMode",
                      JS_NewString(ctx, c->opaque ? "opaque" : "premultiplied"));
    return o;
}

static void
wg_context_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_context *c = JS_GetOpaque(val, g_context_class);
    if (!c) return;
    if (g_webgpu_ctx_by_node)
        g_hash_table_remove(g_webgpu_ctx_by_node, c->canvas);
    wg_ctx_release_gpu(c);
    if (c->device) wgpuDeviceRelease(c->device);
    if (c->queue) wgpuQueueRelease(c->queue);
    g_free(c);
}

static WGPUStringView
wg_sv(const char *s)
{
    WGPUStringView v;
    v.data = s;
    v.length = s ? strlen(s) : WGPU_STRLEN;
    return v;
}

static char *
wg_get_string(JSContext *ctx, JSValueConst obj, const char *key)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    char *out = NULL;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) { out = g_strdup(s); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return out;
}

#define NS_WG_MAX_CONSTANTS 64

typedef struct {
    WGPUConstantEntry entries[NS_WG_MAX_CONSTANTS];
    char             *keys[NS_WG_MAX_CONSTANTS];
    size_t            count;
} wg_constants;

static void
wg_read_constants(JSContext *ctx, JSValueConst stage, wg_constants *out)
{
    memset(out, 0, sizeof *out);
    JSValue jc = JS_GetPropertyStr(ctx, stage, "constants");
    JSPropertyEnum *props = NULL;
    uint32_t n = 0;
    if (JS_IsObject(jc) &&
        JS_GetOwnPropertyNames(ctx, &props, &n, jc,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (out->count < NS_WG_MAX_CONSTANTS) {
                const char *key = JS_AtomToCString(ctx, props[i].atom);
                JSValue jv = JS_GetProperty(ctx, jc, props[i].atom);
                double d = 0;
                JS_ToFloat64(ctx, &d, jv);
                JS_FreeValue(ctx, jv);
                if (key) {
                    size_t k = out->count++;
                    out->keys[k] = g_strdup(key);
                    out->entries[k].key = wg_sv(out->keys[k]);
                    out->entries[k].value = d;
                    JS_FreeCString(ctx, key);
                }
            }
            JS_FreeAtom(ctx, props[i].atom);
        }
        js_free(ctx, props);
    }
    JS_FreeValue(ctx, jc);
}

static void
wg_constants_clear(wg_constants *c)
{
    for (size_t i = 0; i < c->count; i++) g_free(c->keys[i]);
    c->count = 0;
}

static WGPUStencilOperation
wg_stencil_op(const char *s)
{
    if (!s) return WGPUStencilOperation_Keep;
    static const struct { const char *n; WGPUStencilOperation op; } m[] = {
        { "keep", WGPUStencilOperation_Keep },
        { "zero", WGPUStencilOperation_Zero },
        { "replace", WGPUStencilOperation_Replace },
        { "invert", WGPUStencilOperation_Invert },
        { "increment-clamp", WGPUStencilOperation_IncrementClamp },
        { "decrement-clamp", WGPUStencilOperation_DecrementClamp },
        { "increment-wrap", WGPUStencilOperation_IncrementWrap },
        { "decrement-wrap", WGPUStencilOperation_DecrementWrap },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(m); i++)
        if (strcmp(s, m[i].n) == 0) return m[i].op;
    return WGPUStencilOperation_Keep;
}

static void
wg_read_stencil_face(JSContext *ctx, JSValueConst ds, const char *key,
                     WGPUStencilFaceState *out)
{
    out->compare = WGPUCompareFunction_Always;
    out->failOp = out->depthFailOp = out->passOp = WGPUStencilOperation_Keep;
    JSValue f = JS_GetPropertyStr(ctx, ds, key);
    if (JS_IsObject(f)) {
        char *s = wg_get_string(ctx, f, "compare");
        if (s) out->compare = wg_compare_func(s);
        if (out->compare == WGPUCompareFunction_Undefined)
            out->compare = WGPUCompareFunction_Always;
        g_free(s);
        s = wg_get_string(ctx, f, "failOp"); out->failOp = wg_stencil_op(s); g_free(s);
        s = wg_get_string(ctx, f, "depthFailOp"); out->depthFailOp = wg_stencil_op(s); g_free(s);
        s = wg_get_string(ctx, f, "passOp"); out->passOp = wg_stencil_op(s); g_free(s);
    }
    JS_FreeValue(ctx, f);
}

static const wg_enum_name wg_vertex_format_names[] = {
    { "uint8", WGPUVertexFormat_Uint8 }, { "uint8x2", WGPUVertexFormat_Uint8x2 },
    { "uint8x4", WGPUVertexFormat_Uint8x4 }, { "sint8", WGPUVertexFormat_Sint8 },
    { "sint8x2", WGPUVertexFormat_Sint8x2 }, { "sint8x4", WGPUVertexFormat_Sint8x4 },
    { "unorm8", WGPUVertexFormat_Unorm8 }, { "unorm8x2", WGPUVertexFormat_Unorm8x2 },
    { "unorm8x4", WGPUVertexFormat_Unorm8x4 }, { "snorm8", WGPUVertexFormat_Snorm8 },
    { "snorm8x2", WGPUVertexFormat_Snorm8x2 }, { "snorm8x4", WGPUVertexFormat_Snorm8x4 },
    { "uint16", WGPUVertexFormat_Uint16 }, { "uint16x2", WGPUVertexFormat_Uint16x2 },
    { "uint16x4", WGPUVertexFormat_Uint16x4 }, { "sint16", WGPUVertexFormat_Sint16 },
    { "sint16x2", WGPUVertexFormat_Sint16x2 }, { "sint16x4", WGPUVertexFormat_Sint16x4 },
    { "unorm16", WGPUVertexFormat_Unorm16 }, { "unorm16x2", WGPUVertexFormat_Unorm16x2 },
    { "unorm16x4", WGPUVertexFormat_Unorm16x4 }, { "snorm16", WGPUVertexFormat_Snorm16 },
    { "snorm16x2", WGPUVertexFormat_Snorm16x2 }, { "snorm16x4", WGPUVertexFormat_Snorm16x4 },
    { "float16", WGPUVertexFormat_Float16 }, { "float16x2", WGPUVertexFormat_Float16x2 },
    { "float16x4", WGPUVertexFormat_Float16x4 }, { "float32", WGPUVertexFormat_Float32 },
    { "float32x2", WGPUVertexFormat_Float32x2 }, { "float32x3", WGPUVertexFormat_Float32x3 },
    { "float32x4", WGPUVertexFormat_Float32x4 }, { "uint32", WGPUVertexFormat_Uint32 },
    { "uint32x2", WGPUVertexFormat_Uint32x2 }, { "uint32x3", WGPUVertexFormat_Uint32x3 },
    { "uint32x4", WGPUVertexFormat_Uint32x4 }, { "sint32", WGPUVertexFormat_Sint32 },
    { "sint32x2", WGPUVertexFormat_Sint32x2 }, { "sint32x3", WGPUVertexFormat_Sint32x3 },
    { "sint32x4", WGPUVertexFormat_Sint32x4 },
    { "unorm10-10-10-2", WGPUVertexFormat_Unorm10_10_10_2 },
    { "unorm8x4-bgra", WGPUVertexFormat_Unorm8x4BGRA },
};

static gboolean
wg_vertex_format_member(JSContext *ctx, JSValueConst attr, WGPUVertexFormat *out)
{
    JSValue v = JS_GetPropertyStr(ctx, attr, "format");
    if (JS_IsException(v)) return FALSE;
    int format = WGPUVertexFormat_Float32x3;
    gboolean ok;
    if (JS_IsUndefined(v)) {
        JS_ThrowTypeError(ctx, "Required member 'format' is undefined.");
        ok = FALSE;
    } else {
        ok = wg_enum_value(ctx, v, "GPUVertexFormat", wg_vertex_format_names,
                           G_N_ELEMENTS(wg_vertex_format_names), &format);
    }
    JS_FreeValue(ctx, v);
    if (ok && format == WGPUVertexFormat_Unorm8x4BGRA) {
        JS_ThrowTypeError(ctx, "createRenderPipeline: vertex format 'unorm8x4-bgra' is not supported");
        ok = FALSE;
    }
    *out = (WGPUVertexFormat)format;
    return ok;
}

static WGPUPrimitiveTopology
wg_topology(const char *s)
{
    if (!s) return WGPUPrimitiveTopology_TriangleList;
    if (strcmp(s, "triangle-strip") == 0) return WGPUPrimitiveTopology_TriangleStrip;
    if (strcmp(s, "line-list") == 0)  return WGPUPrimitiveTopology_LineList;
    if (strcmp(s, "line-strip") == 0) return WGPUPrimitiveTopology_LineStrip;
    if (strcmp(s, "point-list") == 0) return WGPUPrimitiveTopology_PointList;
    return WGPUPrimitiveTopology_TriangleList;
}

static void
wg_shader_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_shader *s = JS_GetOpaque(val, g_shader_class);
    if (!s) return;
    if (s->mod) wgpuShaderModuleRelease(s->mod);
    g_free(s);
}

static JSValue
wg_shader_compilationInfo(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue info = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, info, "messages", JS_NewArray(ctx));
    return wg_promise_resolved(ctx, info);
}

static gboolean
wg_wgsl_ident_char(char c)
{
    return g_ascii_isalnum(c) || c == '_';
}

static const char *
wg_wgsl_skip_trivia(const char *p)
{
    for (;;) {
        while (*p && g_ascii_isspace(*p)) p++;
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
        } else if (p[0] == '/' && p[1] == '*') {
            const char *end = strstr(p + 2, "*/");
            p = end ? end + 2 : p + strlen(p);
        } else {
            return p;
        }
    }
}

static void
wg_wgsl_drop_subgroups_enable(char *code)
{
    char *p = (char *)wg_wgsl_skip_trivia(code);
    while (strncmp(p, "enable", 6) == 0 && !wg_wgsl_ident_char(p[6])) {
        char *semi = strchr(p, ';');
        if (!semi) return;
        char *list = p + 6;
        gboolean kept = FALSE;
        char *q = list;
        while (q < semi) {
            while (q < semi && !wg_wgsl_ident_char(*q)) q++;
            char *start = q;
            while (q < semi && wg_wgsl_ident_char(*q)) q++;
            if (q == start) break;
            if (q - start == 9 && strncmp(start, "subgroups", 9) == 0) {
                memset(start, ' ', 9);
                char *comma = q;
                while (comma < semi && g_ascii_isspace(*comma)) comma++;
                if (comma < semi && *comma == ',') *comma = ' ';
                else {
                    char *prev = start;
                    while (prev > list && g_ascii_isspace(prev[-1])) prev--;
                    if (prev > list && prev[-1] == ',') prev[-1] = ' ';
                }
            } else {
                kept = TRUE;
            }
        }
        if (!kept)
            for (char *c = p; c <= semi; c++)
                if (*c != '\n') *c = ' ';
        p = (char *)wg_wgsl_skip_trivia(semi + 1);
    }
}

static JSValue
wg_device_createShaderModule(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createShaderModule: descriptor required");
    JSValue jcode = JS_GetPropertyStr(ctx, argv[0], "code");
    const char *code = JS_IsString(jcode) ? JS_ToCString(ctx, jcode) : NULL;
    JS_FreeValue(ctx, jcode);
    if (!code) return JS_ThrowTypeError(ctx, "createShaderModule: code required");
    char *wgsl = g_strdup(code);
    JS_FreeCString(ctx, code);
    wg_wgsl_drop_subgroups_enable(wgsl);

    WGPUShaderSourceWGSL src;
    memset(&src, 0, sizeof src);
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = wg_sv(wgsl);
    WGPUShaderModuleDescriptor desc;
    memset(&desc, 0, sizeof desc);
    desc.nextInChain = (WGPUChainedStruct *)&src;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(d->device, &desc);
    g_free(wgsl);
    if (!mod) return JS_ThrowTypeError(ctx, "createShaderModule failed");

    JSValue obj = JS_NewObjectClass(ctx, g_shader_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_shader *s = g_new0(ns_wg_shader, 1);
    s->mod = mod;
    JS_SetOpaque(obj, s);
    return obj;
}

static void
wg_pipeline_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_pipeline *p = JS_GetOpaque(val, g_pipeline_class);
    if (!p) return;
    if (p->pipe) wgpuRenderPipelineRelease(p->pipe);
    g_free(p);
}

#define NS_WG_MAX_VBUF 16
#define NS_WG_MAX_ATTR 16
#define NS_WG_MAX_TARGET 8

static JSValue
wg_device_createRenderPipeline(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createRenderPipeline: descriptor required");

    WGPURenderPipelineDescriptor desc;
    memset(&desc, 0, sizeof desc);
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.multisample.count = 1;
    desc.multisample.mask = 0xFFFFFFFFu;

    wg_hold hold = { ctx, NULL };
    JSValue jlayout = JS_GetPropertyStr(ctx, argv[0], "layout");
    ns_wg_pllayout *pll = wg_hold_opaque(&hold, jlayout, g_pllayout_class);
    if (pll) desc.layout = pll->layout;
    JS_FreeValue(ctx, jlayout);

    char *vs_entry = NULL, *fs_entry = NULL;
    wg_constants vs_consts, fs_consts;
    memset(&vs_consts, 0, sizeof vs_consts);
    memset(&fs_consts, 0, sizeof fs_consts);
    WGPUVertexBufferLayout vbl[NS_WG_MAX_VBUF];
    WGPUVertexAttribute attrs[NS_WG_MAX_VBUF][NS_WG_MAX_ATTR];
    WGPUColorTargetState targets[NS_WG_MAX_TARGET];
    WGPUBlendState blends[NS_WG_MAX_TARGET];
    WGPUFragmentState frag;
    memset(vbl, 0, sizeof vbl);
    memset(attrs, 0, sizeof attrs);
    memset(targets, 0, sizeof targets);
    memset(blends, 0, sizeof blends);
    memset(&frag, 0, sizeof frag);
    gboolean failed = FALSE;

    JSValue jvertex = JS_GetPropertyStr(ctx, argv[0], "vertex");
    if (!JS_IsObject(jvertex)) {
        JS_ThrowTypeError(ctx, "createRenderPipeline: vertex state required");
        failed = TRUE;
    } else {
        JSValue jmod = JS_GetPropertyStr(ctx, jvertex, "module");
        ns_wg_shader *vm = wg_hold_opaque(&hold, jmod, g_shader_class);
        if (vm) desc.vertex.module = vm->mod;
        JS_FreeValue(ctx, jmod);
        if (!vm || !vm->mod) {
            JS_ThrowTypeError(ctx, "createRenderPipeline: vertex.module must be a GPUShaderModule");
            failed = TRUE;
        }
        JSValue jentry = JS_GetPropertyStr(ctx, jvertex, "entryPoint");
        if (JS_IsString(jentry)) vs_entry = (char *)JS_ToCString(ctx, jentry);
        JS_FreeValue(ctx, jentry);
        desc.vertex.entryPoint = wg_sv(vs_entry);
        wg_read_constants(ctx, jvertex, &vs_consts);
        desc.vertex.constantCount = vs_consts.count;
        desc.vertex.constants = vs_consts.entries;

        static const wg_enum_name step_modes[] = {
            { "vertex", WGPUVertexStepMode_Vertex },
            { "instance", WGPUVertexStepMode_Instance },
        };
        JSValue jbufs = JS_GetPropertyStr(ctx, jvertex, "buffers");
        if (!failed && JS_IsArray(jbufs)) {
            uint32_t nb = 0;
            JSValue jbl = JS_GetPropertyStr(ctx, jbufs, "length");
            JS_ToUint32(ctx, &nb, jbl);
            JS_FreeValue(ctx, jbl);
            if (nb > NS_WG_MAX_VBUF) nb = NS_WG_MAX_VBUF;
            for (uint32_t i = 0; i < nb && !failed; i++) {
                vbl[i].stepMode = WGPUVertexStepMode_Vertex;
                JSValue jb = JS_GetPropertyUint32(ctx, jbufs, i);
                if (!JS_IsObject(jb)) { JS_FreeValue(ctx, jb); continue; }
                int64_t stride = 0;
                JSValue jstride = JS_GetPropertyStr(ctx, jb, "arrayStride");
                JS_ToInt64(ctx, &stride, jstride);
                JS_FreeValue(ctx, jstride);
                vbl[i].arrayStride = (uint64_t)stride;
                int step = WGPUVertexStepMode_Vertex;
                if (!wg_enum_member(ctx, jb, "stepMode", "GPUVertexStepMode",
                                    step_modes, G_N_ELEMENTS(step_modes), &step))
                    failed = TRUE;
                vbl[i].stepMode = (WGPUVertexStepMode)step;

                JSValue jattrs = JS_GetPropertyStr(ctx, jb, "attributes");
                uint32_t na = 0;
                if (!failed && JS_IsArray(jattrs)) {
                    JSValue jal = JS_GetPropertyStr(ctx, jattrs, "length");
                    JS_ToUint32(ctx, &na, jal);
                    JS_FreeValue(ctx, jal);
                    if (na > NS_WG_MAX_ATTR) na = NS_WG_MAX_ATTR;
                    for (uint32_t k = 0; k < na && !failed; k++) {
                        JSValue ja = JS_GetPropertyUint32(ctx, jattrs, k);
                        if (!JS_IsObject(ja)) {
                            JS_FreeValue(ctx, ja);
                            JS_ThrowTypeError(ctx, "createRenderPipeline: vertex attribute must be an object");
                            failed = TRUE;
                            break;
                        }
                        if (!wg_vertex_format_member(ctx, ja, &attrs[i][k].format))
                            failed = TRUE;
                        int64_t off = 0; uint32_t loc = 0;
                        JSValue jo = JS_GetPropertyStr(ctx, ja, "offset");
                        JS_ToInt64(ctx, &off, jo); JS_FreeValue(ctx, jo);
                        JSValue jl = JS_GetPropertyStr(ctx, ja, "shaderLocation");
                        JS_ToUint32(ctx, &loc, jl); JS_FreeValue(ctx, jl);
                        attrs[i][k].offset = (uint64_t)off;
                        attrs[i][k].shaderLocation = loc;
                        JS_FreeValue(ctx, ja);
                    }
                }
                JS_FreeValue(ctx, jattrs);
                vbl[i].attributeCount = na;
                vbl[i].attributes = attrs[i];
                JS_FreeValue(ctx, jb);
            }
            desc.vertex.bufferCount = nb;
            desc.vertex.buffers = vbl;
        }
        JS_FreeValue(ctx, jbufs);
    }
    JS_FreeValue(ctx, jvertex);

    JSValue jprim = JS_GetPropertyStr(ctx, argv[0], "primitive");
    if (JS_IsObject(jprim)) {
        JSValue jt = JS_GetPropertyStr(ctx, jprim, "topology");
        const char *ts = JS_IsString(jt) ? JS_ToCString(ctx, jt) : NULL;
        desc.primitive.topology = wg_topology(ts);
        if (ts) JS_FreeCString(ctx, ts);
        JS_FreeValue(ctx, jt);
        JSValue jcull = JS_GetPropertyStr(ctx, jprim, "cullMode");
        const char *cs = JS_IsString(jcull) ? JS_ToCString(ctx, jcull) : NULL;
        if (cs && strcmp(cs, "back") == 0) desc.primitive.cullMode = WGPUCullMode_Back;
        else if (cs && strcmp(cs, "front") == 0) desc.primitive.cullMode = WGPUCullMode_Front;
        if (cs) JS_FreeCString(ctx, cs);
        JS_FreeValue(ctx, jcull);
        char *ff = wg_get_string(ctx, jprim, "frontFace");
        desc.primitive.frontFace = (ff && strcmp(ff, "cw") == 0)
            ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
        g_free(ff);
        char *sif = wg_get_string(ctx, jprim, "stripIndexFormat");
        if (sif && strcmp(sif, "uint16") == 0)
            desc.primitive.stripIndexFormat = WGPUIndexFormat_Uint16;
        else if (sif && strcmp(sif, "uint32") == 0)
            desc.primitive.stripIndexFormat = WGPUIndexFormat_Uint32;
        g_free(sif);
        JSValue juc = JS_GetPropertyStr(ctx, jprim, "unclippedDepth");
        desc.primitive.unclippedDepth = JS_ToBool(ctx, juc);
        JS_FreeValue(ctx, juc);
    }
    JS_FreeValue(ctx, jprim);

    WGPUDepthStencilState ds;
    memset(&ds, 0, sizeof ds);
    JSValue jds = JS_GetPropertyStr(ctx, argv[0], "depthStencil");
    if (!failed && JS_IsObject(jds)) {
        if (!wg_format_member(ctx, jds, "format", TRUE, &ds.format))
            failed = TRUE;
        JSValue jdw = JS_GetPropertyStr(ctx, jds, "depthWriteEnabled");
        ds.depthWriteEnabled = JS_ToBool(ctx, jdw)
            ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        JS_FreeValue(ctx, jdw);
        if (!failed && ds.depthWriteEnabled == WGPUOptionalBool_True &&
            !wg_format_has_depth(ds.format)) {
            JS_ThrowTypeError(ctx, "createRenderPipeline: depthWriteEnabled needs a depth format, not '%s'",
                              wg_format_name(ds.format));
            failed = TRUE;
        }
        JSValue jdc = JS_GetPropertyStr(ctx, jds, "depthCompare");
        const char *dcs = JS_IsString(jdc) ? JS_ToCString(ctx, jdc) : NULL;
        ds.depthCompare = wg_compare_func(dcs);
        if (ds.depthCompare == WGPUCompareFunction_Undefined)
            ds.depthCompare = WGPUCompareFunction_Always;
        if (dcs) JS_FreeCString(ctx, dcs);
        JS_FreeValue(ctx, jdc);
        wg_read_stencil_face(ctx, jds, "stencilFront", &ds.stencilFront);
        wg_read_stencil_face(ctx, jds, "stencilBack", &ds.stencilBack);
        ds.stencilReadMask = ds.stencilWriteMask = 0xFFFFFFFFu;
        JSValue jm = JS_GetPropertyStr(ctx, jds, "stencilReadMask");
        if (!JS_IsUndefined(jm)) JS_ToUint32(ctx, &ds.stencilReadMask, jm);
        JS_FreeValue(ctx, jm);
        jm = JS_GetPropertyStr(ctx, jds, "stencilWriteMask");
        if (!JS_IsUndefined(jm)) JS_ToUint32(ctx, &ds.stencilWriteMask, jm);
        JS_FreeValue(ctx, jm);
        JSValue jb = JS_GetPropertyStr(ctx, jds, "depthBias");
        if (!JS_IsUndefined(jb)) JS_ToInt32(ctx, &ds.depthBias, jb);
        JS_FreeValue(ctx, jb);
        double bias = 0;
        jb = JS_GetPropertyStr(ctx, jds, "depthBiasSlopeScale");
        if (!JS_IsUndefined(jb)) JS_ToFloat64(ctx, &bias, jb);
        ds.depthBiasSlopeScale = (float)bias;
        JS_FreeValue(ctx, jb);
        bias = 0;
        jb = JS_GetPropertyStr(ctx, jds, "depthBiasClamp");
        if (!JS_IsUndefined(jb)) JS_ToFloat64(ctx, &bias, jb);
        ds.depthBiasClamp = (float)bias;
        JS_FreeValue(ctx, jb);
        desc.depthStencil = &ds;
    }
    JS_FreeValue(ctx, jds);

    JSValue jms = JS_GetPropertyStr(ctx, argv[0], "multisample");
    if (JS_IsObject(jms)) {
        JSValue jc = JS_GetPropertyStr(ctx, jms, "count");
        if (!JS_IsUndefined(jc)) {
            uint32_t c = 1; JS_ToUint32(ctx, &c, jc);
            if (c >= 1) desc.multisample.count = c;
        }
        JS_FreeValue(ctx, jc);
        JSValue jmask = JS_GetPropertyStr(ctx, jms, "mask");
        if (!JS_IsUndefined(jmask)) JS_ToUint32(ctx, &desc.multisample.mask, jmask);
        JS_FreeValue(ctx, jmask);
        JSValue jatc = JS_GetPropertyStr(ctx, jms, "alphaToCoverageEnabled");
        desc.multisample.alphaToCoverageEnabled = JS_ToBool(ctx, jatc);
        JS_FreeValue(ctx, jatc);
    }
    JS_FreeValue(ctx, jms);

    JSValue jfrag = JS_GetPropertyStr(ctx, argv[0], "fragment");
    if (!failed && JS_IsObject(jfrag)) {
        JSValue jmod = JS_GetPropertyStr(ctx, jfrag, "module");
        ns_wg_shader *fm = wg_hold_opaque(&hold, jmod, g_shader_class);
        if (fm) frag.module = fm->mod;
        JS_FreeValue(ctx, jmod);
        if (!fm || !fm->mod) {
            JS_ThrowTypeError(ctx, "createRenderPipeline: fragment.module must be a GPUShaderModule");
            failed = TRUE;
        }
        JSValue jentry = JS_GetPropertyStr(ctx, jfrag, "entryPoint");
        if (JS_IsString(jentry)) fs_entry = (char *)JS_ToCString(ctx, jentry);
        JS_FreeValue(ctx, jentry);
        frag.entryPoint = wg_sv(fs_entry);
        wg_read_constants(ctx, jfrag, &fs_consts);
        frag.constantCount = fs_consts.count;
        frag.constants = fs_consts.entries;

        JSValue jtargets = JS_GetPropertyStr(ctx, jfrag, "targets");
        uint32_t nt = 0;
        if (!failed && JS_IsArray(jtargets)) {
            JSValue jtl = JS_GetPropertyStr(ctx, jtargets, "length");
            JS_ToUint32(ctx, &nt, jtl);
            JS_FreeValue(ctx, jtl);
            if (nt > NS_WG_MAX_TARGET) nt = NS_WG_MAX_TARGET;
            for (uint32_t i = 0; i < nt && !failed; i++) {
                JSValue jtg = JS_GetPropertyUint32(ctx, jtargets, i);
                if (!JS_IsObject(jtg)) { JS_FreeValue(ctx, jtg); continue; }
                if (!wg_format_member(ctx, jtg, "format", TRUE, &targets[i].format))
                    failed = TRUE;
                targets[i].writeMask = WGPUColorWriteMask_All;
                JSValue jwm = JS_GetPropertyStr(ctx, jtg, "writeMask");
                if (!JS_IsUndefined(jwm)) {
                    uint32_t wm = 0xF; JS_ToUint32(ctx, &wm, jwm);
                    targets[i].writeMask = wm & 0xFu;
                }
                JS_FreeValue(ctx, jwm);
                JSValue jblend = JS_GetPropertyStr(ctx, jtg, "blend");
                if (JS_IsObject(jblend)) {
                    JSValue jc = JS_GetPropertyStr(ctx, jblend, "color");
                    wg_read_blend_component(ctx, jc, &blends[i].color);
                    JS_FreeValue(ctx, jc);
                    JSValue ja = JS_GetPropertyStr(ctx, jblend, "alpha");
                    wg_read_blend_component(ctx, ja, &blends[i].alpha);
                    JS_FreeValue(ctx, ja);
                    targets[i].blend = &blends[i];
                }
                JS_FreeValue(ctx, jblend);
                JS_FreeValue(ctx, jtg);
            }
        }
        JS_FreeValue(ctx, jtargets);
        frag.targetCount = nt;
        frag.targets = targets;
        desc.fragment = &frag;
    }
    JS_FreeValue(ctx, jfrag);

    WGPURenderPipeline pipe = failed ? NULL
        : wgpuDeviceCreateRenderPipeline(d->device, &desc);
    wg_hold_release(&hold);
    wg_constants_clear(&vs_consts);
    wg_constants_clear(&fs_consts);
    if (vs_entry) JS_FreeCString(ctx, vs_entry);
    if (fs_entry) JS_FreeCString(ctx, fs_entry);
    if (failed) return JS_EXCEPTION;
    if (!pipe) return JS_ThrowTypeError(ctx, "createRenderPipeline failed");

    JSValue obj = JS_NewObjectClass(ctx, g_pipeline_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_pipeline *p = g_new0(ns_wg_pipeline, 1);
    p->pipe = pipe;
    JS_SetOpaque(obj, p);
    return obj;
}

static JSValue
wg_device_createRenderPipelineAsync(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    return wg_promise_settled(ctx,
        wg_device_createRenderPipeline(ctx, this_val, argc, argv));
}

static JSValue
wg_device_createComputePipelineAsync(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    return wg_promise_settled(ctx,
        wg_device_createComputePipeline(ctx, this_val, argc, argv));
}

static JSValue
wg_pipeline_getBindGroupLayout(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    ns_wg_pipeline *p = JS_GetOpaque(this_val, g_pipeline_class);
    if (!p || !p->pipe || argc < 1) return JS_UNDEFINED;
    uint32_t index = 0;
    JS_ToUint32(ctx, &index, argv[0]);
    WGPUBindGroupLayout l = wgpuRenderPipelineGetBindGroupLayout(p->pipe, index);
    if (!l) return JS_UNDEFINED;
    return wg_make_bgl(ctx, l);
}

static void
wg_bgl_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_bgl *b = JS_GetOpaque(val, g_bgl_class);
    if (!b) return;
    if (b->layout) wgpuBindGroupLayoutRelease(b->layout);
    g_free(b);
}

static JSValue
wg_make_bgl(JSContext *ctx, WGPUBindGroupLayout layout)
{
    JSValue obj = JS_NewObjectClass(ctx, g_bgl_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_bgl *b = g_new0(ns_wg_bgl, 1);
    b->layout = layout;
    JS_SetOpaque(obj, b);
    return obj;
}

static const wg_enum_name wg_buffer_binding_names[] = {
    { "uniform", WGPUBufferBindingType_Uniform },
    { "storage", WGPUBufferBindingType_Storage },
    { "read-only-storage", WGPUBufferBindingType_ReadOnlyStorage },
};

static const wg_enum_name wg_sampler_binding_names[] = {
    { "filtering", WGPUSamplerBindingType_Filtering },
    { "non-filtering", WGPUSamplerBindingType_NonFiltering },
    { "comparison", WGPUSamplerBindingType_Comparison },
};

static const wg_enum_name wg_sample_type_names[] = {
    { "float", WGPUTextureSampleType_Float },
    { "unfilterable-float", WGPUTextureSampleType_UnfilterableFloat },
    { "depth", WGPUTextureSampleType_Depth },
    { "sint", WGPUTextureSampleType_Sint },
    { "uint", WGPUTextureSampleType_Uint },
};

static const wg_enum_name wg_storage_access_names[] = {
    { "write-only", WGPUStorageTextureAccess_WriteOnly },
    { "read-only", WGPUStorageTextureAccess_ReadOnly },
    { "read-write", WGPUStorageTextureAccess_ReadWrite },
};

#define NS_WG_MAX_BGL_ENTRY 32

static gboolean
wg_read_bgl_entry(JSContext *ctx, JSValueConst e, WGPUBindGroupLayoutEntry *out)
{
    if (!JS_IsObject(e)) {
        JS_ThrowTypeError(ctx, "createBindGroupLayout: entry must be an object");
        return FALSE;
    }
    uint32_t binding = 0, vis = 0;
    JSValue jb = JS_GetPropertyStr(ctx, e, "binding");
    JS_ToUint32(ctx, &binding, jb); JS_FreeValue(ctx, jb);
    JSValue jv = JS_GetPropertyStr(ctx, e, "visibility");
    JS_ToUint32(ctx, &vis, jv); JS_FreeValue(ctx, jv);
    out->binding = binding;
    out->visibility = vis & 0x7u;
    JSValue jbuf = JS_GetPropertyStr(ctx, e, "buffer");
    JSValue jsamp = JS_GetPropertyStr(ctx, e, "sampler");
    JSValue jtex = JS_GetPropertyStr(ctx, e, "texture");
    JSValue jstore = JS_GetPropertyStr(ctx, e, "storageTexture");
    gboolean ok = TRUE;
    if (JS_IsObject(jbuf)) {
        int type = WGPUBufferBindingType_Uniform;
        ok = wg_enum_member(ctx, jbuf, "type", "GPUBufferBindingType",
                            wg_buffer_binding_names,
                            G_N_ELEMENTS(wg_buffer_binding_names), &type);
        out->buffer.type = (WGPUBufferBindingType)type;
        JSValue jd = JS_GetPropertyStr(ctx, jbuf, "hasDynamicOffset");
        out->buffer.hasDynamicOffset = JS_ToBool(ctx, jd);
        JS_FreeValue(ctx, jd);
        JSValue jm = JS_GetPropertyStr(ctx, jbuf, "minBindingSize");
        if (!JS_IsUndefined(jm)) {
            int64_t m = 0; JS_ToInt64(ctx, &m, jm);
            out->buffer.minBindingSize = m > 0 ? (uint64_t)m : 0;
        }
        JS_FreeValue(ctx, jm);
    } else if (JS_IsObject(jsamp)) {
        int type = WGPUSamplerBindingType_Filtering;
        ok = wg_enum_member(ctx, jsamp, "type", "GPUSamplerBindingType",
                            wg_sampler_binding_names,
                            G_N_ELEMENTS(wg_sampler_binding_names), &type);
        out->sampler.type = (WGPUSamplerBindingType)type;
    } else if (JS_IsObject(jtex)) {
        int sample = WGPUTextureSampleType_Float;
        int dim = WGPUTextureViewDimension_2D;
        ok = wg_enum_member(ctx, jtex, "sampleType", "GPUTextureSampleType",
                            wg_sample_type_names,
                            G_N_ELEMENTS(wg_sample_type_names), &sample) &&
             wg_enum_member(ctx, jtex, "viewDimension", "GPUTextureViewDimension",
                            wg_view_dimension_names,
                            G_N_ELEMENTS(wg_view_dimension_names), &dim);
        out->texture.sampleType = (WGPUTextureSampleType)sample;
        out->texture.viewDimension = (WGPUTextureViewDimension)dim;
        JSValue jms = JS_GetPropertyStr(ctx, jtex, "multisampled");
        out->texture.multisampled = JS_ToBool(ctx, jms);
        JS_FreeValue(ctx, jms);
    } else if (JS_IsObject(jstore)) {
        int access = WGPUStorageTextureAccess_WriteOnly;
        int dim = WGPUTextureViewDimension_2D;
        ok = wg_enum_member(ctx, jstore, "access", "GPUStorageTextureAccess",
                            wg_storage_access_names,
                            G_N_ELEMENTS(wg_storage_access_names), &access) &&
             wg_format_member(ctx, jstore, "format", TRUE,
                              &out->storageTexture.format) &&
             wg_enum_member(ctx, jstore, "viewDimension", "GPUTextureViewDimension",
                            wg_view_dimension_names,
                            G_N_ELEMENTS(wg_view_dimension_names), &dim);
        out->storageTexture.access = (WGPUStorageTextureAccess)access;
        out->storageTexture.viewDimension = (WGPUTextureViewDimension)dim;
    } else {
        JS_ThrowTypeError(ctx, "createBindGroupLayout: entry %u has no supported binding type",
                          binding);
        ok = FALSE;
    }
    JS_FreeValue(ctx, jstore);
    JS_FreeValue(ctx, jbuf);
    JS_FreeValue(ctx, jsamp);
    JS_FreeValue(ctx, jtex);
    return ok;
}

static JSValue
wg_device_createBindGroupLayout(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createBindGroupLayout: descriptor");

    WGPUBindGroupLayoutEntry entries[NS_WG_MAX_BGL_ENTRY];
    memset(entries, 0, sizeof entries);
    uint32_t n = 0;
    gboolean ok = TRUE;
    JSValue jentries = JS_GetPropertyStr(ctx, argv[0], "entries");
    if (JS_IsArray(jentries)) {
        JSValue jl = JS_GetPropertyStr(ctx, jentries, "length");
        JS_ToUint32(ctx, &n, jl);
        JS_FreeValue(ctx, jl);
        if (n > NS_WG_MAX_BGL_ENTRY) n = NS_WG_MAX_BGL_ENTRY;
        for (uint32_t i = 0; i < n && ok; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, jentries, i);
            ok = wg_read_bgl_entry(ctx, e, &entries[i]);
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, jentries);
    if (!ok) return JS_EXCEPTION;

    WGPUBindGroupLayoutDescriptor desc;
    memset(&desc, 0, sizeof desc);
    desc.entryCount = n;
    desc.entries = entries;
    WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(d->device, &desc);
    if (!layout)
        return JS_ThrowTypeError(ctx, "createBindGroupLayout failed");
    return wg_make_bgl(ctx, layout);
}

static void
wg_pllayout_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_pllayout *p = JS_GetOpaque(val, g_pllayout_class);
    if (!p) return;
    if (p->layout) wgpuPipelineLayoutRelease(p->layout);
    g_free(p);
}

static JSValue
wg_device_createPipelineLayout(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createPipelineLayout: descriptor");

    WGPUBindGroupLayout layouts[NS_WG_MAX_BGL_ENTRY];
    memset(layouts, 0, sizeof layouts);
    wg_hold hold = { ctx, NULL };
    WGPUBindGroupLayout empty = NULL;
    gboolean ok = TRUE;
    uint32_t n = 0;
    JSValue jbgls = JS_GetPropertyStr(ctx, argv[0], "bindGroupLayouts");
    if (JS_IsArray(jbgls)) {
        JSValue jl = JS_GetPropertyStr(ctx, jbgls, "length");
        JS_ToUint32(ctx, &n, jl);
        JS_FreeValue(ctx, jl);
        if (n > NS_WG_MAX_BGL_ENTRY) n = NS_WG_MAX_BGL_ENTRY;
        for (uint32_t i = 0; i < n && ok; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, jbgls, i);
            ns_wg_bgl *b = wg_hold_opaque(&hold, e, g_bgl_class);
            if (b && b->layout) {
                layouts[i] = b->layout;
            } else if (JS_IsNull(e) || JS_IsUndefined(e)) {
                if (!empty) {
                    WGPUBindGroupLayoutDescriptor ed;
                    memset(&ed, 0, sizeof ed);
                    empty = wgpuDeviceCreateBindGroupLayout(d->device, &ed);
                }
                layouts[i] = empty;
                ok = empty != NULL;
                if (!ok) JS_ThrowTypeError(ctx, "createPipelineLayout failed");
            } else {
                JS_ThrowTypeError(ctx, "createPipelineLayout: bindGroupLayouts[%u] is not a GPUBindGroupLayout",
                                  i);
                ok = FALSE;
            }
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, jbgls);

    WGPUPipelineLayoutDescriptor desc;
    memset(&desc, 0, sizeof desc);
    desc.bindGroupLayoutCount = n;
    desc.bindGroupLayouts = layouts;
    WGPUPipelineLayout layout = ok
        ? wgpuDeviceCreatePipelineLayout(d->device, &desc) : NULL;
    wg_hold_release(&hold);
    if (empty) wgpuBindGroupLayoutRelease(empty);
    if (!ok) return JS_EXCEPTION;
    if (!layout)
        return JS_ThrowTypeError(ctx, "createPipelineLayout failed");

    JSValue obj = JS_NewObjectClass(ctx, g_pllayout_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_pllayout *p = g_new0(ns_wg_pllayout, 1);
    p->layout = layout;
    JS_SetOpaque(obj, p);
    return obj;
}

static void
wg_bindgroup_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_bindgroup *b = JS_GetOpaque(val, g_bindgroup_class);
    if (!b) return;
    if (b->group) wgpuBindGroupRelease(b->group);
    g_free(b);
}

static JSValue
wg_device_createBindGroup(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createBindGroup: descriptor");

    wg_hold hold = { ctx, NULL };
    JSValue jlayout = JS_GetPropertyStr(ctx, argv[0], "layout");
    ns_wg_bgl *bgl = wg_hold_opaque(&hold, jlayout, g_bgl_class);
    JS_FreeValue(ctx, jlayout);
    if (!bgl) return JS_ThrowTypeError(ctx, "createBindGroup: layout");

    WGPUBindGroupEntry entries[NS_WG_MAX_BGL_ENTRY];
    memset(entries, 0, sizeof entries);
    WGPUTextureView owned_views[NS_WG_MAX_BGL_ENTRY];
    size_t n_owned_views = 0;
    uint32_t n = 0;
    gboolean ok = TRUE;
    JSValue jentries = JS_GetPropertyStr(ctx, argv[0], "entries");
    if (JS_IsArray(jentries)) {
        JSValue jl = JS_GetPropertyStr(ctx, jentries, "length");
        JS_ToUint32(ctx, &n, jl);
        JS_FreeValue(ctx, jl);
        if (n > NS_WG_MAX_BGL_ENTRY) n = NS_WG_MAX_BGL_ENTRY;
        for (uint32_t i = 0; i < n && ok; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, jentries, i);
            if (!JS_IsObject(e)) {
                JS_FreeValue(ctx, e);
                JS_ThrowTypeError(ctx, "createBindGroup: entry must be an object");
                ok = FALSE;
                break;
            }
            uint32_t binding = 0;
            JSValue jb = JS_GetPropertyStr(ctx, e, "binding");
            JS_ToUint32(ctx, &binding, jb); JS_FreeValue(ctx, jb);
            entries[i].binding = binding;
            JSValue jres = JS_GetPropertyStr(ctx, e, "resource");
            ns_wg_view *vw = wg_hold_opaque(&hold, jres, g_view_class);
            ns_wg_sampler *smp = wg_hold_opaque(&hold, jres, g_sampler_class);
            ns_wg_texture *tex = wg_hold_opaque(&hold, jres, g_texture_class);
            ns_wg_buffer *whole = wg_hold_opaque(&hold, jres, g_buffer_class);
            entries[i].size = WGPU_WHOLE_SIZE;
            if (vw) {
                entries[i].textureView = vw->view;
            } else if (smp) {
                entries[i].sampler = smp->sampler;
            } else if (tex && tex->texture) {
                entries[i].textureView = wgpuTextureCreateView(tex->texture, NULL);
                if (n_owned_views < NS_WG_MAX_BGL_ENTRY)
                    owned_views[n_owned_views++] = entries[i].textureView;
            } else if (whole) {
                entries[i].buffer = whole->buffer;
            } else if (JS_IsObject(jres)) {
                JSValue jbuf = JS_GetPropertyStr(ctx, jres, "buffer");
                ns_wg_buffer *buf = wg_hold_opaque(&hold, jbuf, g_buffer_class);
                if (buf) {
                    entries[i].buffer = buf->buffer;
                    int64_t off = 0, sz = -1;
                    JSValue jo = JS_GetPropertyStr(ctx, jres, "offset");
                    if (!JS_IsUndefined(jo)) JS_ToInt64(ctx, &off, jo);
                    JS_FreeValue(ctx, jo);
                    JSValue js = JS_GetPropertyStr(ctx, jres, "size");
                    if (!JS_IsUndefined(js)) JS_ToInt64(ctx, &sz, js);
                    JS_FreeValue(ctx, js);
                    entries[i].offset = off > 0 ? (uint64_t)off : 0;
                    if (sz >= 0) entries[i].size = (uint64_t)sz;
                }
                JS_FreeValue(ctx, jbuf);
            }
            JS_FreeValue(ctx, jres);
            JS_FreeValue(ctx, e);
            if (!entries[i].buffer && !entries[i].sampler &&
                !entries[i].textureView) {
                JS_ThrowTypeError(ctx, "createBindGroup: entry %u has no valid resource",
                                  binding);
                ok = FALSE;
            } else if (entries[i].buffer && entries[i].size == 0) {
                JS_ThrowTypeError(ctx, "createBindGroup: entry %u binds 0 bytes",
                                  binding);
                ok = FALSE;
            }
        }
    }
    JS_FreeValue(ctx, jentries);

    WGPUBindGroupDescriptor desc;
    memset(&desc, 0, sizeof desc);
    desc.layout = bgl->layout;
    desc.entryCount = n;
    desc.entries = entries;
    WGPUBindGroup group = ok && bgl->layout
        ? wgpuDeviceCreateBindGroup(d->device, &desc) : NULL;
    wg_hold_release(&hold);
    for (size_t i = 0; i < n_owned_views; i++)
        if (owned_views[i]) wgpuTextureViewRelease(owned_views[i]);
    if (!ok) return JS_EXCEPTION;
    if (!group) return JS_ThrowTypeError(ctx, "createBindGroup failed");

    JSValue obj = JS_NewObjectClass(ctx, g_bindgroup_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_bindgroup *b = g_new0(ns_wg_bindgroup, 1);
    b->group = group;
    JS_SetOpaque(obj, b);
    return obj;
}

static uint32_t
wg_copy_stride(JSContext *ctx, JSValueConst obj, const char *key)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    uint32_t stride = WGPU_COPY_STRIDE_UNDEFINED;
    if (!JS_IsUndefined(v)) {
        uint32_t x = 0;
        JS_ToUint32(ctx, &x, v);
        if (x != 0) stride = x;
    }
    JS_FreeValue(ctx, v);
    return stride;
}

static void
wg_read_buffer_layout(JSContext *ctx, JSValueConst v,
                      WGPUTexelCopyBufferLayout *out)
{
    memset(out, 0, sizeof *out);
    out->bytesPerRow = WGPU_COPY_STRIDE_UNDEFINED;
    out->rowsPerImage = WGPU_COPY_STRIDE_UNDEFINED;
    if (!JS_IsObject(v)) return;
    JSValue jo = JS_GetPropertyStr(ctx, v, "offset");
    if (!JS_IsUndefined(jo)) {
        int64_t off = 0; JS_ToInt64(ctx, &off, jo);
        out->offset = off > 0 ? (uint64_t)off : 0;
    }
    JS_FreeValue(ctx, jo);
    out->bytesPerRow = wg_copy_stride(ctx, v, "bytesPerRow");
    out->rowsPerImage = wg_copy_stride(ctx, v, "rowsPerImage");
}

static ns_wg_texture *
wg_read_texcopy(JSContext *ctx, JSValueConst v, WGPUTexelCopyTextureInfo *out,
                wg_hold *hold)
{
    memset(out, 0, sizeof *out);
    out->aspect = WGPUTextureAspect_All;
    if (!JS_IsObject(v)) return NULL;
    JSValue jtex = JS_GetPropertyStr(ctx, v, "texture");
    ns_wg_texture *t = wg_hold_opaque(hold, jtex, g_texture_class);
    if (t) out->texture = t->texture;
    JS_FreeValue(ctx, jtex);
    JSValue jmip = JS_GetPropertyStr(ctx, v, "mipLevel");
    if (!JS_IsUndefined(jmip)) JS_ToUint32(ctx, &out->mipLevel, jmip);
    JS_FreeValue(ctx, jmip);
    char *aspect = wg_get_string(ctx, v, "aspect");
    if (aspect && strcmp(aspect, "depth-only") == 0)
        out->aspect = WGPUTextureAspect_DepthOnly;
    else if (aspect && strcmp(aspect, "stencil-only") == 0)
        out->aspect = WGPUTextureAspect_StencilOnly;
    g_free(aspect);
    JSValue jorigin = JS_GetPropertyStr(ctx, v, "origin");
    uint32_t xyz[3] = { 0, 0, 0 };
    static const char *const keys[3] = { "x", "y", "z" };
    for (uint32_t i = 0; i < 3 && JS_IsObject(jorigin); i++) {
        JSValue c = JS_IsArray(jorigin) ? JS_GetPropertyUint32(ctx, jorigin, i)
                                        : JS_GetPropertyStr(ctx, jorigin, keys[i]);
        if (!JS_IsUndefined(c)) JS_ToUint32(ctx, &xyz[i], c);
        JS_FreeValue(ctx, c);
    }
    JS_FreeValue(ctx, jorigin);
    out->origin.x = xyz[0]; out->origin.y = xyz[1]; out->origin.z = xyz[2];
    return t;
}

static JSValue
wg_encoder_copyTextureToTexture(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    ns_wg_encoder *e = JS_GetOpaque(this_val, g_encoder_class);
    if (!e || !e->enc || argc < 3) return JS_UNDEFINED;
    WGPUTexelCopyTextureInfo src, dst;
    wg_hold hold = { ctx, NULL };
    wg_read_texcopy(ctx, argv[0], &src, &hold);
    wg_read_texcopy(ctx, argv[1], &dst, &hold);
    if (src.texture && dst.texture) {
        WGPUExtent3D size;
        wg_read_extent(ctx, argv[2], &size);
        wgpuCommandEncoderCopyTextureToTexture(e->enc, &src, &dst, &size);
    }
    wg_hold_release(&hold);
    return JS_UNDEFINED;
}

static gboolean
wg_read_buffer_copy(JSContext *ctx, JSValueConst v, WGPUTexelCopyBufferInfo *out,
                    wg_hold *hold)
{
    memset(out, 0, sizeof *out);
    if (!JS_IsObject(v)) return FALSE;
    JSValue jbuf = JS_GetPropertyStr(ctx, v, "buffer");
    ns_wg_buffer *b = wg_hold_opaque(hold, jbuf, g_buffer_class);
    JS_FreeValue(ctx, jbuf);
    if (!b || !b->buffer) return FALSE;
    out->buffer = b->buffer;
    wg_read_buffer_layout(ctx, v, &out->layout);
    return TRUE;
}

static JSValue
wg_encoder_copyBufferToTexture(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    ns_wg_encoder *e = JS_GetOpaque(this_val, g_encoder_class);
    if (!e || !e->enc || argc < 3) return JS_UNDEFINED;
    wg_hold hold = { ctx, NULL };
    WGPUTexelCopyBufferInfo src;
    WGPUTexelCopyTextureInfo dst;
    gboolean have_src = wg_read_buffer_copy(ctx, argv[0], &src, &hold);
    wg_read_texcopy(ctx, argv[1], &dst, &hold);
    if (have_src && dst.texture) {
        WGPUExtent3D size;
        wg_read_extent(ctx, argv[2], &size);
        wgpuCommandEncoderCopyBufferToTexture(e->enc, &src, &dst, &size);
    }
    wg_hold_release(&hold);
    return JS_UNDEFINED;
}

static JSValue
wg_encoder_copyTextureToBuffer(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    ns_wg_encoder *e = JS_GetOpaque(this_val, g_encoder_class);
    if (!e || !e->enc || argc < 3) return JS_UNDEFINED;
    wg_hold hold = { ctx, NULL };
    WGPUTexelCopyTextureInfo src;
    WGPUTexelCopyBufferInfo dst;
    wg_read_texcopy(ctx, argv[0], &src, &hold);
    gboolean have_dst = wg_read_buffer_copy(ctx, argv[1], &dst, &hold);
    if (src.texture && have_dst) {
        WGPUExtent3D size;
        wg_read_extent(ctx, argv[2], &size);
        wgpuCommandEncoderCopyTextureToBuffer(e->enc, &src, &dst, &size);
    }
    wg_hold_release(&hold);
    return JS_UNDEFINED;
}

static JSValue
wg_encoder_clearBuffer(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    ns_wg_encoder *e = JS_GetOpaque(this_val, g_encoder_class);
    if (!e || !e->enc || argc < 1) return JS_UNDEFINED;
    ns_wg_buffer *b = JS_GetOpaque(argv[0], g_buffer_class);
    if (!b || !b->buffer) return JS_UNDEFINED;
    int64_t off = 0, size = -1;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToInt64(ctx, &off, argv[1]);
    if (argc >= 3 && !JS_IsUndefined(argv[2])) JS_ToInt64(ctx, &size, argv[2]);
    if (off < 0) off = 0;
    if (size == 0) return JS_UNDEFINED;
    wgpuCommandEncoderClearBuffer(e->enc, b->buffer, (uint64_t)off,
                                  size < 0 ? WGPU_WHOLE_SIZE : (uint64_t)size);
    return JS_UNDEFINED;
}

static JSValue
wg_encoder_copyBufferToBuffer(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    ns_wg_encoder *e = JS_GetOpaque(this_val, g_encoder_class);
    if (!e || !e->enc || argc < 4) return JS_UNDEFINED;
    if (argc < 5) {
        JSValueConst args[5] = { argv[0], argv[1], argv[2], argv[3], JS_UNDEFINED };
        if (argc == 3) {
            args[1] = JS_NewInt32(ctx, 0);
            args[2] = argv[1];
            args[3] = JS_NewInt32(ctx, 0);
            args[4] = argv[2];
        }
        return wg_encoder_copyBufferToBuffer(ctx, this_val, 5, args);
    }
    ns_wg_buffer *src = JS_GetOpaque(argv[0], g_buffer_class);
    ns_wg_buffer *dst = JS_GetOpaque(argv[2], g_buffer_class);
    if (!src || !dst) return JS_UNDEFINED;
    int64_t soff = 0, doff = 0, size = 0;
    JS_ToInt64(ctx, &soff, argv[1]);
    JS_ToInt64(ctx, &doff, argv[3]);
    if (JS_IsUndefined(argv[4])) size = (int64_t)src->size - soff;
    else JS_ToInt64(ctx, &size, argv[4]);
    if (soff < 0 || doff < 0 || size < 0) return JS_UNDEFINED;
    wgpuCommandEncoderCopyBufferToBuffer(e->enc, src->buffer, (uint64_t)soff,
                                         dst->buffer, (uint64_t)doff,
                                         (uint64_t)size);
    return JS_UNDEFINED;
}

static JSValue
wg_device_pushErrorScope(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d) return JS_UNDEFINED;
    char *f = argc >= 1 && JS_IsString(argv[0]) ? (char *)JS_ToCString(ctx, argv[0]) : NULL;
    WGPUErrorFilter filter;
    if (f && strcmp(f, "validation") == 0) filter = WGPUErrorFilter_Validation;
    else if (f && strcmp(f, "out-of-memory") == 0) filter = WGPUErrorFilter_OutOfMemory;
    else if (f && strcmp(f, "internal") == 0) filter = WGPUErrorFilter_Internal;
    else {
        if (f) JS_FreeCString(ctx, f);
        return JS_ThrowTypeError(ctx, "pushErrorScope: invalid filter");
    }
    JS_FreeCString(ctx, f);
    guint8 internal = filter == WGPUErrorFilter_Internal;
    if (!d->scope_internal) d->scope_internal = g_byte_array_new();
    g_byte_array_append(d->scope_internal, &internal, 1);
    if (!internal) wgpuDevicePushErrorScope(d->device, filter);
    d->error_scopes++;
    return JS_UNDEFINED;
}

typedef struct {
    int done;
    WGPUPopErrorScopeStatus status;
    WGPUErrorType type;
    char *message;
} wg_scope_wait;

static void
wg_on_pop_scope(WGPUPopErrorScopeStatus status, WGPUErrorType type,
                WGPUStringView message, void *u1, void *u2)
{
    (void)u2;
    wg_scope_wait *w = u1;
    w->status = status;
    w->type = type;
    w->message = wg_sv_dup(message);
    w->done = 1;
}

static gboolean
wg_pop_internal_scope(WGPUDevice device, WGPUErrorType *type, char **message)
{
    wg_scope_wait wait;
    memset(&wait, 0, sizeof wait);
    WGPUPopErrorScopeCallbackInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = wg_on_pop_scope;
    ci.userdata1 = &wait;
    wgpuDevicePopErrorScope(device, ci);
    for (int i = 0; i < 2000 && !wait.done; i++)
        wgpuInstanceProcessEvents(ns_webgpu_instance());
    *type = wait.type;
    *message = wait.message;
    return wait.done && wait.status == WGPUPopErrorScopeStatus_Success;
}

static JSValue
wg_device_popErrorScope(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d) return wg_promise_rejected(ctx, "popErrorScope: device");
    if (d->error_scopes == 0)
        return wg_promise_rejected(ctx, "OperationError: popErrorScope: the error scope stack is empty");
    d->error_scopes--;
    if (d->scope_internal && d->scope_internal->len > 0) {
        guint top = d->scope_internal->len - 1;
        gboolean internal = d->scope_internal->data[top] != 0;
        g_byte_array_set_size(d->scope_internal, top);
        if (internal) return wg_promise_resolved(ctx, JS_NULL);
    }
    wg_scope_wait wait;
    memset(&wait, 0, sizeof wait);
    WGPUPopErrorScopeCallbackInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = wg_on_pop_scope;
    ci.userdata1 = &wait;
    wgpuDevicePopErrorScope(d->device, ci);
    for (int i = 0; i < 2000 && !wait.done; i++)
        wgpuInstanceProcessEvents(ns_webgpu_instance());
    JSValue result;
    if (!wait.done || wait.status != WGPUPopErrorScopeStatus_Success)
        result = wg_promise_rejected(ctx, "OperationError: popErrorScope: the error scope stack is empty");
    else if (wait.type == WGPUErrorType_NoError)
        result = wg_promise_resolved(ctx, JS_NULL);
    else
        result = wg_promise_resolved(ctx, wg_new_error(ctx, wait.type, wait.message));
    g_free(wait.message);
    return result;
}

static void
wg_sampler_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_sampler *s = JS_GetOpaque(val, g_sampler_class);
    if (!s) return;
    if (s->sampler) wgpuSamplerRelease(s->sampler);
    g_free(s);
}

static JSValue
wg_device_createSampler(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d) return JS_UNDEFINED;

    WGPUSamplerDescriptor desc;
    memset(&desc, 0, sizeof desc);
    desc.addressModeU = WGPUAddressMode_ClampToEdge;
    desc.addressModeV = WGPUAddressMode_ClampToEdge;
    desc.addressModeW = WGPUAddressMode_ClampToEdge;
    desc.magFilter = WGPUFilterMode_Nearest;
    desc.minFilter = WGPUFilterMode_Nearest;
    desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    desc.lodMinClamp = 0.0f;
    desc.lodMaxClamp = 32.0f;
    desc.maxAnisotropy = 1;

    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue v;
        const char *s;
#define WG_STR(field) (v = JS_GetPropertyStr(ctx, argv[0], field), \
        s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL)
#define WG_STR_END() do { if (s) JS_FreeCString(ctx, s); JS_FreeValue(ctx, v); } while (0)
        WG_STR("addressModeU"); if (s) desc.addressModeU = wg_address_mode(s); WG_STR_END();
        WG_STR("addressModeV"); if (s) desc.addressModeV = wg_address_mode(s); WG_STR_END();
        WG_STR("addressModeW"); if (s) desc.addressModeW = wg_address_mode(s); WG_STR_END();
        WG_STR("magFilter"); if (s && strcmp(s, "linear") == 0) desc.magFilter = WGPUFilterMode_Linear; WG_STR_END();
        WG_STR("minFilter"); if (s && strcmp(s, "linear") == 0) desc.minFilter = WGPUFilterMode_Linear; WG_STR_END();
        WG_STR("mipmapFilter"); if (s && strcmp(s, "linear") == 0) desc.mipmapFilter = WGPUMipmapFilterMode_Linear; WG_STR_END();
        WG_STR("compare"); if (s) desc.compare = wg_compare_func(s); WG_STR_END();
#undef WG_STR
#undef WG_STR_END
        v = JS_GetPropertyStr(ctx, argv[0], "maxAnisotropy");
        if (!JS_IsUndefined(v)) {
            uint32_t a = 1; JS_ToUint32(ctx, &a, v);
            desc.maxAnisotropy = (uint16_t)(a < 1 ? 1 : a);
        }
        JS_FreeValue(ctx, v);
    }

    WGPUSampler sampler = wgpuDeviceCreateSampler(d->device, &desc);
    if (!sampler) return JS_ThrowTypeError(ctx, "createSampler failed");
    JSValue obj = JS_NewObjectClass(ctx, g_sampler_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_sampler *s = g_new0(ns_wg_sampler, 1);
    s->sampler = sampler;
    JS_SetOpaque(obj, s);
    return obj;
}

static void
wg_read_extent(JSContext *ctx, JSValueConst v, WGPUExtent3D *out)
{
    out->width = 1; out->height = 1; out->depthOrArrayLayers = 1;
    if (JS_IsArray(v)) {
        uint32_t n = 0;
        JSValue jl = JS_GetPropertyStr(ctx, v, "length");
        JS_ToUint32(ctx, &n, jl); JS_FreeValue(ctx, jl);
        uint32_t vals[3] = { 1, 1, 1 };
        for (uint32_t i = 0; i < n && i < 3; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            JS_ToUint32(ctx, &vals[i], e); JS_FreeValue(ctx, e);
        }
        out->width = vals[0]; out->height = vals[1];
        out->depthOrArrayLayers = vals[2];
    } else if (JS_IsObject(v)) {
        JSValue jw = JS_GetPropertyStr(ctx, v, "width");
        JSValue jh = JS_GetPropertyStr(ctx, v, "height");
        JSValue jd = JS_GetPropertyStr(ctx, v, "depthOrArrayLayers");
        JS_ToUint32(ctx, &out->width, jw);
        if (!JS_IsUndefined(jh)) JS_ToUint32(ctx, &out->height, jh);
        if (!JS_IsUndefined(jd)) JS_ToUint32(ctx, &out->depthOrArrayLayers, jd);
        JS_FreeValue(ctx, jw); JS_FreeValue(ctx, jh); JS_FreeValue(ctx, jd);
    }
}

static JSValue
wg_device_createTexture(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createTexture: descriptor required");

    WGPUTextureDescriptor desc;
    memset(&desc, 0, sizeof desc);
    desc.dimension = WGPUTextureDimension_2D;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;

    JSValue jsize = JS_GetPropertyStr(ctx, argv[0], "size");
    wg_read_extent(ctx, jsize, &desc.size);
    JS_FreeValue(ctx, jsize);

    JSValue jusage = JS_GetPropertyStr(ctx, argv[0], "usage");
    uint32_t usage = 0; JS_ToUint32(ctx, &usage, jusage); JS_FreeValue(ctx, jusage);
    desc.usage = (WGPUTextureUsage)(usage & ~0x1Fu ? 0 : usage);

    WGPUTextureFormat view_formats[NS_WG_MAX_VIEW_FORMATS];
    size_t view_format_count = 0;
    int dimension = WGPUTextureDimension_2D;
    if (!wg_format_member(ctx, argv[0], "format", TRUE, &desc.format) ||
        !wg_read_view_formats(ctx, argv[0], view_formats, &view_format_count) ||
        !wg_enum_member(ctx, argv[0], "dimension", "GPUTextureDimension",
                        wg_texture_dimension_names,
                        G_N_ELEMENTS(wg_texture_dimension_names), &dimension))
        return JS_EXCEPTION;
    desc.viewFormatCount = view_format_count;
    desc.viewFormats = view_formats;
    desc.dimension = (WGPUTextureDimension)dimension;

    JSValue jmip = JS_GetPropertyStr(ctx, argv[0], "mipLevelCount");
    if (!JS_IsUndefined(jmip)) { uint32_t m = 1; JS_ToUint32(ctx, &m, jmip); desc.mipLevelCount = m ? m : 1; }
    JS_FreeValue(ctx, jmip);
    JSValue jsamp = JS_GetPropertyStr(ctx, argv[0], "sampleCount");
    if (!JS_IsUndefined(jsamp)) { uint32_t m = 1; JS_ToUint32(ctx, &m, jsamp); desc.sampleCount = m ? m : 1; }
    JS_FreeValue(ctx, jsamp);

    WGPUTexture tex = wgpuDeviceCreateTexture(d->device, &desc);
    if (!tex) return JS_ThrowTypeError(ctx, "createTexture failed");
    JSValue obj = wg_make_texture(ctx, tex);
    JSValue jbvd = JS_GetPropertyStr(ctx, argv[0], "textureBindingViewDimension");
    if (JS_IsString(jbvd) && JS_IsObject(obj))
        JS_SetPropertyStr(ctx, obj, "textureBindingViewDimension", JS_DupValue(ctx, jbvd));
    JS_FreeValue(ctx, jbvd);
    return obj;
}

static JSValue
wg_queue_writeTexture(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    ns_wg_queue *q = wg_queue_unwrap(this_val);
    if (!q || argc < 4 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    wg_hold hold = { ctx, NULL };
    WGPUTexelCopyTextureInfo dst;
    wg_read_texcopy(ctx, argv[0], &dst, &hold);
    if (!dst.texture) {
        wg_hold_release(&hold);
        return JS_UNDEFINED;
    }

    WGPUTexelCopyBufferLayout layout;
    wg_read_buffer_layout(ctx, argv[2], &layout);
    WGPUExtent3D ext;
    wg_read_extent(ctx, argv[3], &ext);

    size_t byte_len = 0;
    uint8_t *bytes = NULL;
    size_t view_off = 0, view_len = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[1], &view_off, &view_len, &bpe);
    if (!JS_IsException(abuf)) {
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, abuf);
        if (base && view_off <= total) {
            size_t avail = total - view_off;
            bytes = base + view_off;
            byte_len = view_len < avail ? view_len : avail;
        }
        JS_FreeValue(ctx, abuf);
    } else {
        JS_FreeValue(ctx, abuf);
        bytes = JS_GetArrayBuffer(ctx, &byte_len, argv[1]);
    }
    if (bytes)
        wgpuQueueWriteTexture(q->queue, &dst, bytes, byte_len, &layout, &ext);
    wg_hold_release(&hold);
    return JS_UNDEFINED;
}

static JSValue
wg_queue_copyExternalImageToTexture(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    ns_wg_queue *q = wg_queue_unwrap(this_val);
    if (!q || argc < 3 || !JS_IsObject(argv[0]) || !JS_IsObject(argv[1]))
        return JS_UNDEFINED;

    JSValue jsrc = JS_GetPropertyStr(ctx, argv[0], "source");
    JSValue jflip = JS_GetPropertyStr(ctx, argv[0], "flipY");
    gboolean flip_y = JS_ToBool(ctx, jflip);
    JS_FreeValue(ctx, jflip);
    int w = 0, h = 0;
    gboolean threw = FALSE;
    cairo_surface_t *s = ns_js_drawimage_source_surface(ctx, jsrc, &w, &h,
                                                        &threw);
    JS_FreeValue(ctx, jsrc);
    if (threw) return JS_EXCEPTION;
    if (!s) return JS_UNDEFINED;
    const unsigned char *data = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    if (w <= 0 || h <= 0 || !data) { cairo_surface_destroy(s); return JS_UNDEFINED; }

    WGPUTexelCopyTextureInfo dst;
    wg_hold hold = { ctx, NULL };
    ns_wg_texture *tw = wg_read_texcopy(ctx, argv[1], &dst, &hold);
    if (!dst.texture) {
        wg_hold_release(&hold);
        cairo_surface_destroy(s);
        return JS_UNDEFINED;
    }
    gboolean to_rgba = tw && tw->format != WGPUTextureFormat_BGRA8Unorm &&
                       tw->format != WGPUTextureFormat_BGRA8UnormSrgb;

    JSValue jpremul = JS_GetPropertyStr(ctx, argv[0], "premultipliedAlpha");
    gboolean premultiply = JS_ToBool(ctx, jpremul);
    JS_FreeValue(ctx, jpremul);

    size_t bpr = (size_t)w * 4u;
    uint8_t *out = g_try_malloc(bpr * (size_t)h);
    if (!out) {
        wg_hold_release(&hold);
        cairo_surface_destroy(s);
        return JS_UNDEFINED;
    }
    for (int y = 0; y < h; y++) {
        const unsigned char *srow = data + (size_t)(flip_y ? h - 1 - y : y) * stride;
        uint8_t *orow = out + (size_t)y * bpr;
        for (int x = 0; x < w; x++) {
            const unsigned char *p = srow + x * 4;
            unsigned b = p[0], g = p[1], r = p[2], a = p[3];
            if (!premultiply && a > 0 && a < 255) {
                r = (r * 255u + a / 2) / a; if (r > 255) r = 255;
                g = (g * 255u + a / 2) / a; if (g > 255) g = 255;
                b = (b * 255u + a / 2) / a; if (b > 255) b = 255;
            }
            uint8_t *o = orow + x * 4;
            if (to_rgba) { o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b; }
            else { o[0] = (uint8_t)b; o[1] = (uint8_t)g; o[2] = (uint8_t)r; }
            o[3] = (uint8_t)a;
        }
    }

    WGPUTexelCopyBufferLayout layout;
    memset(&layout, 0, sizeof layout);
    layout.bytesPerRow = (uint32_t)bpr;
    layout.rowsPerImage = (uint32_t)h;
    WGPUExtent3D ext = { (uint32_t)w, (uint32_t)h, 1 };
    wgpuQueueWriteTexture(q->queue, &dst, out, bpr * (size_t)h, &layout, &ext);
    wg_hold_release(&hold);

    g_free(out);
    cairo_surface_destroy(s);
    return JS_UNDEFINED;
}

static void
wg_queryset_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_queryset *q = JS_GetOpaque(val, g_queryset_class);
    if (!q) return;
    if (q->qs) wgpuQuerySetRelease(q->qs);
    g_free(q);
}

static JSValue
wg_queryset_destroy(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    ns_wg_queryset *q = JS_GetOpaque(this_val, g_queryset_class);
    if (q && q->qs) {
        wgpuQuerySetRelease(q->qs);
        q->qs = NULL;
    }
    return JS_UNDEFINED;
}

static JSValue
wg_device_createQuerySet(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    JSValue jtype = JS_GetPropertyStr(ctx, argv[0], "type");
    const char *ts = JS_IsString(jtype) ? JS_ToCString(ctx, jtype) : NULL;
    JSValue jcount = JS_GetPropertyStr(ctx, argv[0], "count");
    uint32_t count = 0; JS_ToUint32(ctx, &count, jcount);
    JS_FreeValue(ctx, jcount);

    gboolean occlusion = ts && strcmp(ts, "occlusion") == 0;
    if (ts) JS_FreeCString(ctx, ts);
    JS_FreeValue(ctx, jtype);

    WGPUQuerySet qs = NULL;
    if (occlusion) {
        WGPUQuerySetDescriptor desc;
        memset(&desc, 0, sizeof desc);
        desc.type = WGPUQueryType_Occlusion;
        desc.count = count;
        qs = wgpuDeviceCreateQuerySet(d->device, &desc);
    }
    JSValue obj = JS_NewObjectClass(ctx, g_queryset_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_queryset *q = g_new0(ns_wg_queryset, 1);
    q->qs = qs;
    JS_SetOpaque(obj, q);
    JS_SetPropertyStr(ctx, obj, "count", JS_NewUint32(ctx, count));
    return obj;
}

static JSValue
wg_encoder_resolveQuerySet(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    ns_wg_encoder *e = JS_GetOpaque(this_val, g_encoder_class);
    if (!e || !e->enc || argc < 5) return JS_UNDEFINED;
    ns_wg_queryset *q = JS_GetOpaque(argv[0], g_queryset_class);
    ns_wg_buffer *dst = JS_GetOpaque(argv[3], g_buffer_class);
    if (!q || !q->qs || !dst || !dst->buffer) return JS_UNDEFINED;
    uint32_t first = 0, count = 0;
    int64_t doff = 0;
    JS_ToUint32(ctx, &first, argv[1]);
    JS_ToUint32(ctx, &count, argv[2]);
    JS_ToInt64(ctx, &doff, argv[4]);
    wgpuCommandEncoderResolveQuerySet(e->enc, q->qs, first, count,
                                      dst->buffer, (uint64_t)doff);
    return JS_UNDEFINED;
}

static void
wg_compute_pipe_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_compute_pipe *p = JS_GetOpaque(val, g_compute_pipe_class);
    if (!p) return;
    if (p->pipe) wgpuComputePipelineRelease(p->pipe);
    g_free(p);
}

static JSValue
wg_compute_pipe_getBindGroupLayout(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    ns_wg_compute_pipe *p = JS_GetOpaque(this_val, g_compute_pipe_class);
    if (!p || !p->pipe || argc < 1) return JS_UNDEFINED;
    uint32_t index = 0;
    JS_ToUint32(ctx, &index, argv[0]);
    WGPUBindGroupLayout l = wgpuComputePipelineGetBindGroupLayout(p->pipe, index);
    if (!l) return JS_UNDEFINED;
    return wg_make_bgl(ctx, l);
}

static JSValue
wg_device_createComputePipeline(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createComputePipeline: descriptor");

    WGPUComputePipelineDescriptor desc;
    memset(&desc, 0, sizeof desc);
    wg_hold hold = { ctx, NULL };
    JSValue jlayout = JS_GetPropertyStr(ctx, argv[0], "layout");
    ns_wg_pllayout *pll = wg_hold_opaque(&hold, jlayout, g_pllayout_class);
    if (pll) desc.layout = pll->layout;
    JS_FreeValue(ctx, jlayout);

    char *entry = NULL;
    wg_constants consts;
    memset(&consts, 0, sizeof consts);
    JSValue jcompute = JS_GetPropertyStr(ctx, argv[0], "compute");
    if (JS_IsObject(jcompute)) {
        JSValue jmod = JS_GetPropertyStr(ctx, jcompute, "module");
        ns_wg_shader *m = wg_hold_opaque(&hold, jmod, g_shader_class);
        if (m) desc.compute.module = m->mod;
        JS_FreeValue(ctx, jmod);
        JSValue jentry = JS_GetPropertyStr(ctx, jcompute, "entryPoint");
        if (JS_IsString(jentry)) entry = (char *)JS_ToCString(ctx, jentry);
        JS_FreeValue(ctx, jentry);
        desc.compute.entryPoint = wg_sv(entry);
        wg_read_constants(ctx, jcompute, &consts);
        desc.compute.constantCount = consts.count;
        desc.compute.constants = consts.entries;
    }
    JS_FreeValue(ctx, jcompute);

    gboolean have_module = desc.compute.module != NULL;
    WGPUComputePipeline pipe = have_module
        ? wgpuDeviceCreateComputePipeline(d->device, &desc) : NULL;
    wg_hold_release(&hold);
    wg_constants_clear(&consts);
    if (entry) JS_FreeCString(ctx, entry);
    if (!have_module)
        return JS_ThrowTypeError(ctx, "createComputePipeline: compute.module must be a GPUShaderModule");
    if (!pipe) return JS_ThrowTypeError(ctx, "createComputePipeline failed");

    JSValue obj = JS_NewObjectClass(ctx, g_compute_pipe_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_compute_pipe *p = g_new0(ns_wg_compute_pipe, 1);
    p->pipe = pipe;
    JS_SetOpaque(obj, p);
    return obj;
}

static void
wg_compute_pass_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_compute_pass *p = JS_GetOpaque(val, g_compute_pass_class);
    if (!p) return;
    if (p->pass) wgpuComputePassEncoderRelease(p->pass);
    g_free(p);
}

static JSValue
wg_compute_pass_setPipeline(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)ctx;
    ns_wg_compute_pass *p = JS_GetOpaque(this_val, g_compute_pass_class);
    if (!p || !p->pass || argc < 1) return JS_UNDEFINED;
    ns_wg_compute_pipe *pl = JS_GetOpaque(argv[0], g_compute_pipe_class);
    if (pl && pl->pipe) wgpuComputePassEncoderSetPipeline(p->pass, pl->pipe);
    return JS_UNDEFINED;
}

static JSValue
wg_compute_pass_setBindGroup(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    ns_wg_compute_pass *p = JS_GetOpaque(this_val, g_compute_pass_class);
    if (!p || !p->pass || argc < 2) return JS_UNDEFINED;
    uint32_t index = 0;
    JS_ToUint32(ctx, &index, argv[0]);
    ns_wg_bindgroup *bg = JS_GetOpaque(argv[1], g_bindgroup_class);
    uint32_t offsets[NS_WG_MAX_DYNAMIC_OFFSETS];
    size_t n = wg_read_dynamic_offsets(ctx, argc, argv, offsets);
    wgpuComputePassEncoderSetBindGroup(p->pass, index,
                                       bg ? bg->group : NULL, n, offsets);
    return JS_UNDEFINED;
}

static JSValue
wg_compute_pass_dispatch(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    ns_wg_compute_pass *p = JS_GetOpaque(this_val, g_compute_pass_class);
    if (!p || !p->pass) return JS_UNDEFINED;
    uint32_t x = 1, y = 1, z = 1;
    if (argc >= 1) JS_ToUint32(ctx, &x, argv[0]);
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToUint32(ctx, &y, argv[1]);
    if (argc >= 3 && !JS_IsUndefined(argv[2])) JS_ToUint32(ctx, &z, argv[2]);
    wgpuComputePassEncoderDispatchWorkgroups(p->pass, x, y, z);
    return JS_UNDEFINED;
}

static JSValue
wg_compute_pass_dispatchIndirect(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    ns_wg_compute_pass *p = JS_GetOpaque(this_val, g_compute_pass_class);
    uint64_t off = 0;
    ns_wg_buffer *b = wg_indirect_args(ctx, argc, argv, &off);
    if (p && p->pass && b)
        wgpuComputePassEncoderDispatchWorkgroupsIndirect(p->pass, b->buffer, off);
    return JS_UNDEFINED;
}

static JSValue
wg_compute_pass_end(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    ns_wg_compute_pass *p = JS_GetOpaque(this_val, g_compute_pass_class);
    if (p && p->pass) wgpuComputePassEncoderEnd(p->pass);
    return JS_UNDEFINED;
}

static JSValue
wg_encoder_beginComputePass(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wg_encoder *e = JS_GetOpaque(this_val, g_encoder_class);
    if (!e || !e->enc) return JS_UNDEFINED;
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(e->enc, NULL);
    if (!pass) return JS_UNDEFINED;
    JSValue obj = JS_NewObjectClass(ctx, g_compute_pass_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_compute_pass *p = g_new0(ns_wg_compute_pass, 1);
    p->pass = pass;
    JS_SetOpaque(obj, p);
    return obj;
}

static void
wg_bundle_encoder_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_bundle_encoder *b = JS_GetOpaque(val, g_bundle_encoder_class);
    if (!b) return;
    if (b->enc) wgpuRenderBundleEncoderRelease(b->enc);
    g_free(b);
}

static void
wg_bundle_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_wg_bundle *b = JS_GetOpaque(val, g_bundle_class);
    if (!b) return;
    if (b->bundle) wgpuRenderBundleRelease(b->bundle);
    g_free(b);
}

static WGPURenderBundleEncoder
wg_bundle_enc(JSValueConst this_val)
{
    ns_wg_bundle_encoder *b = JS_GetOpaque(this_val, g_bundle_encoder_class);
    return b ? b->enc : NULL;
}

static JSValue
wg_device_createRenderBundleEncoder(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    ns_wg_device *d = JS_GetOpaque(this_val, g_device_class);
    if (!d || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createRenderBundleEncoder: descriptor required");
    WGPUTextureFormat formats[NS_WG_MAX_COLOR_ATTACHMENTS];
    WGPURenderBundleEncoderDescriptor desc;
    memset(&desc, 0, sizeof desc);
    gboolean ok = TRUE;
    JSValue jcf = JS_GetPropertyStr(ctx, argv[0], "colorFormats");
    if (JS_IsObject(jcf)) {
        JSValue list = wg_array_from(ctx, jcf);
        uint32_t n = 0;
        if (JS_IsException(list)) {
            ok = FALSE;
        } else {
            JSValue jl = JS_GetPropertyStr(ctx, list, "length");
            JS_ToUint32(ctx, &n, jl);
            JS_FreeValue(ctx, jl);
        }
        for (uint32_t i = 0; ok && i < n && i < NS_WG_MAX_COLOR_ATTACHMENTS; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, list, i);
            WGPUTextureFormat f = WGPUTextureFormat_Undefined;
            if (!JS_IsNull(e) && !JS_IsUndefined(e))
                ok = wg_format_value(ctx, e, &f);
            formats[desc.colorFormatCount++] = f;
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, list);
    }
    JS_FreeValue(ctx, jcf);
    desc.colorFormats = formats;
    if (!ok || !wg_format_member(ctx, argv[0], "depthStencilFormat", FALSE,
                                 &desc.depthStencilFormat))
        return JS_EXCEPTION;
    desc.sampleCount = 1;
    JSValue js = JS_GetPropertyStr(ctx, argv[0], "sampleCount");
    if (!JS_IsUndefined(js)) JS_ToUint32(ctx, &desc.sampleCount, js);
    JS_FreeValue(ctx, js);
    js = JS_GetPropertyStr(ctx, argv[0], "depthReadOnly");
    desc.depthReadOnly = JS_ToBool(ctx, js);
    JS_FreeValue(ctx, js);
    js = JS_GetPropertyStr(ctx, argv[0], "stencilReadOnly");
    desc.stencilReadOnly = JS_ToBool(ctx, js);
    JS_FreeValue(ctx, js);

    WGPURenderBundleEncoder enc = wgpuDeviceCreateRenderBundleEncoder(d->device, &desc);
    if (!enc) return JS_ThrowTypeError(ctx, "createRenderBundleEncoder failed");
    JSValue obj = JS_NewObjectClass(ctx, g_bundle_encoder_class);
    if (JS_IsException(obj)) { wgpuRenderBundleEncoderRelease(enc); return obj; }
    ns_wg_bundle_encoder *b = g_new0(ns_wg_bundle_encoder, 1);
    b->enc = enc;
    JS_SetOpaque(obj, b);
    JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, ""));
    return obj;
}

static JSValue
wg_bundle_setPipeline(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)ctx;
    WGPURenderBundleEncoder enc = wg_bundle_enc(this_val);
    if (!enc || argc < 1) return JS_UNDEFINED;
    ns_wg_pipeline *pl = JS_GetOpaque(argv[0], g_pipeline_class);
    if (pl && pl->pipe) wgpuRenderBundleEncoderSetPipeline(enc, pl->pipe);
    return JS_UNDEFINED;
}

static JSValue
wg_bundle_setBindGroup(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    WGPURenderBundleEncoder enc = wg_bundle_enc(this_val);
    if (!enc || argc < 2) return JS_UNDEFINED;
    uint32_t index = wg_arg_u32(ctx, argc, argv, 0, 0);
    ns_wg_bindgroup *bg = JS_GetOpaque(argv[1], g_bindgroup_class);
    uint32_t offsets[NS_WG_MAX_DYNAMIC_OFFSETS];
    size_t n = wg_read_dynamic_offsets(ctx, argc, argv, offsets);
    wgpuRenderBundleEncoderSetBindGroup(enc, index, bg ? bg->group : NULL, n, offsets);
    return JS_UNDEFINED;
}

static JSValue
wg_bundle_setVertexBuffer(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    WGPURenderBundleEncoder enc = wg_bundle_enc(this_val);
    if (!enc || argc < 2) return JS_UNDEFINED;
    ns_wg_buffer *b = JS_GetOpaque(argv[1], g_buffer_class);
    if (!b || !b->buffer) return JS_UNDEFINED;
    int64_t off = 0, size = -1;
    if (argc >= 3 && !JS_IsUndefined(argv[2])) JS_ToInt64(ctx, &off, argv[2]);
    if (argc >= 4 && !JS_IsUndefined(argv[3])) JS_ToInt64(ctx, &size, argv[3]);
    wgpuRenderBundleEncoderSetVertexBuffer(enc, wg_arg_u32(ctx, argc, argv, 0, 0),
        b->buffer, off > 0 ? (uint64_t)off : 0,
        size < 0 ? WGPU_WHOLE_SIZE : (uint64_t)size);
    return JS_UNDEFINED;
}

static JSValue
wg_bundle_setIndexBuffer(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    WGPURenderBundleEncoder enc = wg_bundle_enc(this_val);
    if (!enc || argc < 2) return JS_UNDEFINED;
    ns_wg_buffer *b = JS_GetOpaque(argv[0], g_buffer_class);
    if (!b || !b->buffer) return JS_UNDEFINED;
    const char *fs = JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    WGPUIndexFormat fmt = (fs && strcmp(fs, "uint16") == 0)
        ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
    if (fs) JS_FreeCString(ctx, fs);
    int64_t off = 0, size = -1;
    if (argc >= 3 && !JS_IsUndefined(argv[2])) JS_ToInt64(ctx, &off, argv[2]);
    if (argc >= 4 && !JS_IsUndefined(argv[3])) JS_ToInt64(ctx, &size, argv[3]);
    wgpuRenderBundleEncoderSetIndexBuffer(enc, b->buffer, fmt,
        off > 0 ? (uint64_t)off : 0, size < 0 ? WGPU_WHOLE_SIZE : (uint64_t)size);
    return JS_UNDEFINED;
}

static JSValue
wg_bundle_draw(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGPURenderBundleEncoder enc = wg_bundle_enc(this_val);
    if (enc)
        wgpuRenderBundleEncoderDraw(enc, wg_arg_u32(ctx, argc, argv, 0, 0),
                                    wg_arg_u32(ctx, argc, argv, 1, 1),
                                    wg_arg_u32(ctx, argc, argv, 2, 0),
                                    wg_arg_u32(ctx, argc, argv, 3, 0));
    return JS_UNDEFINED;
}

static JSValue
wg_bundle_drawIndexed(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    WGPURenderBundleEncoder enc = wg_bundle_enc(this_val);
    if (!enc) return JS_UNDEFINED;
    int32_t base = 0;
    if (argc >= 4) JS_ToInt32(ctx, &base, argv[3]);
    wgpuRenderBundleEncoderDrawIndexed(enc, wg_arg_u32(ctx, argc, argv, 0, 0),
                                       wg_arg_u32(ctx, argc, argv, 1, 1),
                                       wg_arg_u32(ctx, argc, argv, 2, 0), base,
                                       wg_arg_u32(ctx, argc, argv, 4, 0));
    return JS_UNDEFINED;
}

static JSValue
wg_bundle_drawIndirect(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    WGPURenderBundleEncoder enc = wg_bundle_enc(this_val);
    uint64_t off = 0;
    ns_wg_buffer *b = wg_indirect_args(ctx, argc, argv, &off);
    if (enc && b) wgpuRenderBundleEncoderDrawIndirect(enc, b->buffer, off);
    return JS_UNDEFINED;
}

static JSValue
wg_bundle_drawIndexedIndirect(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    WGPURenderBundleEncoder enc = wg_bundle_enc(this_val);
    uint64_t off = 0;
    ns_wg_buffer *b = wg_indirect_args(ctx, argc, argv, &off);
    if (enc && b) wgpuRenderBundleEncoderDrawIndexedIndirect(enc, b->buffer, off);
    return JS_UNDEFINED;
}

static JSValue
wg_bundle_finish(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGPURenderBundleEncoder enc = wg_bundle_enc(this_val);
    if (!enc) return JS_UNDEFINED;
    WGPURenderBundle bundle = wgpuRenderBundleEncoderFinish(enc, NULL);
    if (!bundle) return JS_UNDEFINED;
    JSValue obj = JS_NewObjectClass(ctx, g_bundle_class);
    if (JS_IsException(obj)) { wgpuRenderBundleRelease(bundle); return obj; }
    ns_wg_bundle *b = g_new0(ns_wg_bundle, 1);
    b->bundle = bundle;
    JS_SetOpaque(obj, b);
    JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, ""));
    return obj;
}

static JSValue
wg_pass_executeBundles(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    ns_wg_pass *p = JS_GetOpaque(this_val, g_pass_class);
    if (!p || !p->pass || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    JSValue list = wg_array_from(ctx, argv[0]);
    if (JS_IsException(list)) return list;
    uint32_t n = 0;
    JSValue jl = JS_GetPropertyStr(ctx, list, "length");
    JS_ToUint32(ctx, &n, jl);
    JS_FreeValue(ctx, jl);
    WGPURenderBundle *bundles = g_new0(WGPURenderBundle, n ? n : 1);
    wg_hold hold = { ctx, NULL };
    size_t count = 0;
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        ns_wg_bundle *b = wg_hold_opaque(&hold, e, g_bundle_class);
        if (b && b->bundle) bundles[count++] = b->bundle;
        JS_FreeValue(ctx, e);
    }
    wgpuRenderPassEncoderExecuteBundles(p->pass, count, bundles);
    wg_hold_release(&hold);
    g_free(bundles);
    JS_FreeValue(ctx, list);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry wg_bundle_encoder_proto[] = {
    JS_CFUNC_DEF("setPipeline", 1, wg_bundle_setPipeline),
    JS_CFUNC_DEF("setBindGroup", 2, wg_bundle_setBindGroup),
    JS_CFUNC_DEF("setVertexBuffer", 2, wg_bundle_setVertexBuffer),
    JS_CFUNC_DEF("setIndexBuffer", 2, wg_bundle_setIndexBuffer),
    JS_CFUNC_DEF("draw", 4, wg_bundle_draw),
    JS_CFUNC_DEF("drawIndexed", 5, wg_bundle_drawIndexed),
    JS_CFUNC_DEF("drawIndirect", 2, wg_bundle_drawIndirect),
    JS_CFUNC_DEF("drawIndexedIndirect", 2, wg_bundle_drawIndexedIndirect),
    JS_CFUNC_DEF("finish", 1, wg_bundle_finish),
    JS_CFUNC_DEF("pushDebugGroup", 1, wg_debug_noop),
    JS_CFUNC_DEF("popDebugGroup", 0, wg_debug_noop),
    JS_CFUNC_DEF("insertDebugMarker", 1, wg_debug_noop),
};

static const JSCFunctionListEntry wg_queue_proto[] = {
    JS_CFUNC_DEF("writeBuffer", 5, wg_queue_writeBuffer),
    JS_CFUNC_DEF("writeTexture", 4, wg_queue_writeTexture),
    JS_CFUNC_DEF("copyExternalImageToTexture", 3, wg_queue_copyExternalImageToTexture),
    JS_CFUNC_DEF("submit", 1, wg_queue_submit),
    JS_CFUNC_DEF("onSubmittedWorkDone", 0, wg_queue_onSubmittedWorkDone),
};

static const JSCFunctionListEntry wg_buffer_proto[] = {
    JS_CFUNC_DEF("destroy", 0, wg_buffer_destroy),
    JS_CFUNC_DEF("getMappedRange", 2, wg_buffer_getMappedRange),
    JS_CFUNC_DEF("unmap", 0, wg_buffer_unmap),
    JS_CFUNC_DEF("mapAsync", 3, wg_buffer_mapAsync),
};

static const JSCFunctionListEntry wg_device_proto[] = {
    JS_CFUNC_DEF("createBuffer", 1, wg_device_createBuffer),
    JS_CFUNC_DEF("createCommandEncoder", 1, wg_device_createCommandEncoder),
    JS_CFUNC_DEF("createShaderModule", 1, wg_device_createShaderModule),
    JS_CFUNC_DEF("createRenderPipeline", 1, wg_device_createRenderPipeline),
    JS_CFUNC_DEF("createComputePipeline", 1, wg_device_createComputePipeline),
    JS_CFUNC_DEF("createRenderPipelineAsync", 1, wg_device_createRenderPipelineAsync),
    JS_CFUNC_DEF("createComputePipelineAsync", 1, wg_device_createComputePipelineAsync),
    JS_CFUNC_DEF("createRenderBundleEncoder", 1, wg_device_createRenderBundleEncoder),
    JS_CFUNC_DEF("createBindGroupLayout", 1, wg_device_createBindGroupLayout),
    JS_CFUNC_DEF("createPipelineLayout", 1, wg_device_createPipelineLayout),
    JS_CFUNC_DEF("createBindGroup", 1, wg_device_createBindGroup),
    JS_CFUNC_DEF("createSampler", 1, wg_device_createSampler),
    JS_CFUNC_DEF("createTexture", 1, wg_device_createTexture),
    JS_CFUNC_DEF("createQuerySet", 1, wg_device_createQuerySet),
    JS_CFUNC_DEF("pushErrorScope", 1, wg_device_pushErrorScope),
    JS_CFUNC_DEF("popErrorScope", 0, wg_device_popErrorScope),
    JS_CFUNC_DEF("getQueue", 0, wg_device_getQueue),
    JS_CFUNC_DEF("destroy", 0, wg_device_destroy),
};

static const JSCFunctionListEntry wg_adapter_proto[] = {
    JS_CFUNC_DEF("requestDevice", 1, wg_adapter_requestDevice),
};

static const JSCFunctionListEntry wg_texture_proto[] = {
    JS_CFUNC_DEF("createView", 1, wg_texture_createView),
    JS_CFUNC_DEF("destroy", 0, wg_texture_destroy),
};

static const JSCFunctionListEntry wg_pass_proto[] = {
    JS_CFUNC_DEF("end", 0, wg_pass_end),
    JS_CFUNC_DEF("setPipeline", 1, wg_pass_setPipeline),
    JS_CFUNC_DEF("setBindGroup", 2, wg_pass_setBindGroup),
    JS_CFUNC_DEF("setVertexBuffer", 2, wg_pass_setVertexBuffer),
    JS_CFUNC_DEF("setIndexBuffer", 2, wg_pass_setIndexBuffer),
    JS_CFUNC_DEF("setViewport", 6, wg_pass_setViewport),
    JS_CFUNC_DEF("setScissorRect", 4, wg_pass_setScissorRect),
    JS_CFUNC_DEF("setBlendConstant", 1, wg_pass_setBlendConstant),
    JS_CFUNC_DEF("setStencilReference", 1, wg_pass_setStencilReference),
    JS_CFUNC_DEF("beginOcclusionQuery", 1, wg_pass_beginOcclusionQuery),
    JS_CFUNC_DEF("endOcclusionQuery", 0, wg_pass_endOcclusionQuery),
    JS_CFUNC_DEF("pushDebugGroup", 1, wg_debug_noop),
    JS_CFUNC_DEF("popDebugGroup", 0, wg_debug_noop),
    JS_CFUNC_DEF("insertDebugMarker", 1, wg_debug_noop),
    JS_CFUNC_DEF("draw", 4, wg_pass_draw),
    JS_CFUNC_DEF("drawIndexed", 5, wg_pass_drawIndexed),
    JS_CFUNC_DEF("drawIndirect", 2, wg_pass_drawIndirect),
    JS_CFUNC_DEF("executeBundles", 1, wg_pass_executeBundles),
    JS_CFUNC_DEF("drawIndexedIndirect", 2, wg_pass_drawIndexedIndirect),
};

static const JSCFunctionListEntry wg_encoder_proto[] = {
    JS_CFUNC_DEF("beginRenderPass", 1, wg_encoder_beginRenderPass),
    JS_CFUNC_DEF("beginComputePass", 1, wg_encoder_beginComputePass),
    JS_CFUNC_DEF("copyTextureToTexture", 3, wg_encoder_copyTextureToTexture),
    JS_CFUNC_DEF("copyBufferToBuffer", 5, wg_encoder_copyBufferToBuffer),
    JS_CFUNC_DEF("copyBufferToTexture", 3, wg_encoder_copyBufferToTexture),
    JS_CFUNC_DEF("copyTextureToBuffer", 3, wg_encoder_copyTextureToBuffer),
    JS_CFUNC_DEF("clearBuffer", 3, wg_encoder_clearBuffer),
    JS_CFUNC_DEF("writeTimestamp", 2, wg_debug_noop),
    JS_CFUNC_DEF("pushDebugGroup", 1, wg_debug_noop),
    JS_CFUNC_DEF("popDebugGroup", 0, wg_debug_noop),
    JS_CFUNC_DEF("insertDebugMarker", 1, wg_debug_noop),
    JS_CFUNC_DEF("resolveQuerySet", 5, wg_encoder_resolveQuerySet),
    JS_CFUNC_DEF("finish", 0, wg_encoder_finish),
};

static const JSCFunctionListEntry wg_shader_proto[] = {
    JS_CFUNC_DEF("getCompilationInfo", 0, wg_shader_compilationInfo),
};

static const JSCFunctionListEntry wg_pipeline_proto[] = {
    JS_CFUNC_DEF("getBindGroupLayout", 1, wg_pipeline_getBindGroupLayout),
};

static const JSCFunctionListEntry wg_queryset_proto[] = {
    JS_CFUNC_DEF("destroy", 0, wg_queryset_destroy),
};

static const JSCFunctionListEntry wg_compute_pipe_proto[] = {
    JS_CFUNC_DEF("getBindGroupLayout", 1, wg_compute_pipe_getBindGroupLayout),
};

static const JSCFunctionListEntry wg_compute_pass_proto[] = {
    JS_CFUNC_DEF("setPipeline", 1, wg_compute_pass_setPipeline),
    JS_CFUNC_DEF("setBindGroup", 2, wg_compute_pass_setBindGroup),
    JS_CFUNC_DEF("dispatchWorkgroups", 3, wg_compute_pass_dispatch),
    JS_CFUNC_DEF("dispatchWorkgroupsIndirect", 2, wg_compute_pass_dispatchIndirect),
    JS_CFUNC_DEF("pushDebugGroup", 1, wg_debug_noop),
    JS_CFUNC_DEF("popDebugGroup", 0, wg_debug_noop),
    JS_CFUNC_DEF("insertDebugMarker", 1, wg_debug_noop),
    JS_CFUNC_DEF("end", 0, wg_compute_pass_end),
};

static const JSCFunctionListEntry wg_context_proto[] = {
    JS_CFUNC_DEF("configure", 1, wg_ctx_configure),
    JS_CFUNC_DEF("unconfigure", 0, wg_ctx_unconfigure),
    JS_CFUNC_DEF("getCurrentTexture", 0, wg_ctx_getCurrentTexture),
    JS_CFUNC_DEF("getConfiguration", 0, wg_ctx_getConfiguration),
};

typedef struct {
    JSClassID                  *id;
    const char                 *name;
    const JSCFunctionListEntry *methods;
    int                         count;
} wg_interface;

static const wg_interface wg_interfaces[] = {
    { &g_adapter_class, "GPUAdapter", wg_adapter_proto, G_N_ELEMENTS(wg_adapter_proto) },
    { &g_device_class, "GPUDevice", wg_device_proto, G_N_ELEMENTS(wg_device_proto) },
    { &g_queue_class, "GPUQueue", wg_queue_proto, G_N_ELEMENTS(wg_queue_proto) },
    { &g_buffer_class, "GPUBuffer", wg_buffer_proto, G_N_ELEMENTS(wg_buffer_proto) },
    { &g_context_class, "GPUCanvasContext", wg_context_proto, G_N_ELEMENTS(wg_context_proto) },
    { &g_texture_class, "GPUTexture", wg_texture_proto, G_N_ELEMENTS(wg_texture_proto) },
    { &g_view_class, "GPUTextureView", NULL, 0 },
    { &g_encoder_class, "GPUCommandEncoder", wg_encoder_proto, G_N_ELEMENTS(wg_encoder_proto) },
    { &g_pass_class, "GPURenderPassEncoder", wg_pass_proto, G_N_ELEMENTS(wg_pass_proto) },
    { &g_cmdbuf_class, "GPUCommandBuffer", NULL, 0 },
    { &g_shader_class, "GPUShaderModule", wg_shader_proto, G_N_ELEMENTS(wg_shader_proto) },
    { &g_pipeline_class, "GPURenderPipeline", wg_pipeline_proto, G_N_ELEMENTS(wg_pipeline_proto) },
    { &g_bgl_class, "GPUBindGroupLayout", NULL, 0 },
    { &g_pllayout_class, "GPUPipelineLayout", NULL, 0 },
    { &g_bindgroup_class, "GPUBindGroup", NULL, 0 },
    { &g_sampler_class, "GPUSampler", NULL, 0 },
    { &g_queryset_class, "GPUQuerySet", wg_queryset_proto, G_N_ELEMENTS(wg_queryset_proto) },
    { &g_compute_pipe_class, "GPUComputePipeline", wg_compute_pipe_proto, G_N_ELEMENTS(wg_compute_pipe_proto) },
    { &g_compute_pass_class, "GPUComputePassEncoder", wg_compute_pass_proto, G_N_ELEMENTS(wg_compute_pass_proto) },
    { &g_bundle_encoder_class, "GPURenderBundleEncoder", wg_bundle_encoder_proto, G_N_ELEMENTS(wg_bundle_encoder_proto) },
    { &g_bundle_class, "GPURenderBundle", NULL, 0 },
};

static const char *const wg_plain_interfaces[] = {
    "GPU", "GPUAdapterInfo", "GPUSupportedFeatures", "GPUSupportedLimits",
    "GPUCompilationInfo",
    "GPUCompilationMessage", "GPUDeviceLostInfo",
};

static const char wg_error_classes_js[] =
    "(() => {\n"
    "  const g = globalThis;\n"
    "  class GPUError {\n"
    "    #message;\n"
    "    constructor(message) { this.#message = String(message ?? ''); }\n"
    "    get message() { return this.#message; }\n"
    "  }\n"
    "  class GPUValidationError extends GPUError {}\n"
    "  class GPUOutOfMemoryError extends GPUError {}\n"
    "  class GPUInternalError extends GPUError {}\n"
    "  class GPUPipelineError extends Error {\n"
    "    constructor(message, options) {\n"
    "      super(message); this.name = 'GPUPipelineError';\n"
    "      this.reason = options && options.reason;\n"
    "    }\n"
    "  }\n"
    "  for (const c of [GPUError, GPUValidationError, GPUOutOfMemoryError,\n"
    "                   GPUInternalError, GPUPipelineError])\n"
    "    Object.defineProperty(g, c.name, { value: c, writable: true, configurable: true });\n"
    "  const settle = v => Object.defineProperty(g, 'GPUUncapturedErrorEvent',\n"
    "    { value: v, writable: true, configurable: true });\n"
    "  Object.defineProperty(g, 'GPUUncapturedErrorEvent', {\n"
    "    configurable: true,\n"
    "    get() {\n"
    "      const C = class GPUUncapturedErrorEvent extends g.Event {\n"
    "        #error;\n"
    "        constructor(type, init) { super(type, init); this.#error = init && init.error; }\n"
    "        get error() { return this.#error; }\n"
    "      };\n"
    "      settle(C);\n"
    "      return C;\n"
    "    },\n"
    "    set(v) { settle(v); }\n"
    "  });\n"
    "  if (g.GPUDevice) {\n"
    "    const handlers = new WeakMap();\n"
    "    Object.defineProperty(g.GPUDevice.prototype, 'onuncapturederror', {\n"
    "      configurable: true, enumerable: true,\n"
    "      get() { return handlers.get(this) ?? null; },\n"
    "      set(fn) {\n"
    "        const old = handlers.get(this);\n"
    "        if (old) this.removeEventListener('uncapturederror', old);\n"
    "        if (typeof fn === 'function') { handlers.set(this, fn); this.addEventListener('uncapturederror', fn); }\n"
    "        else handlers.delete(this);\n"
    "      }\n"
    "    });\n"
    "  }\n"
    "})();\n";

static JSValue
wg_illegal_constructor(JSContext *ctx, JSValueConst new_target,
                       int argc, JSValueConst *argv)
{
    (void)new_target; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

static void
wg_define_interface(JSContext *ctx, JSValueConst global, const char *name,
                    JSValue proto)
{
    JSValue ctor = JS_NewCFunction2(ctx, wg_illegal_constructor, name, 0,
                                    JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_DefinePropertyValueStr(ctx, global, name, ctor,
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

static void
wg_install_interfaces(JSContext *ctx, JSValueConst global)
{
    for (size_t i = 0; i < G_N_ELEMENTS(wg_interfaces); i++) {
        const wg_interface *w = &wg_interfaces[i];
        JSValue proto = JS_NewObject(ctx);
        if (w->methods)
            JS_SetPropertyFunctionList(ctx, proto, w->methods, w->count);
        wg_define_interface(ctx, global, w->name, proto);
        JS_SetClassProto(ctx, *w->id, proto);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(wg_plain_interfaces); i++) {
        JSValue proto = JS_NewObject(ctx);
        wg_define_interface(ctx, global, wg_plain_interfaces[i], proto);
        JS_FreeValue(ctx, proto);
    }
}

static void
wg_register_class(JSContext *ctx, JSClassID *id, const char *name,
                  JSClassFinalizer *finalizer)
{
    JSClassDef def;
    memset(&def, 0, sizeof def);
    def.class_name = name;
    def.finalizer = finalizer;
    ns_new_class_id(id);
    JS_NewClass(JS_GetRuntime(ctx), *id, &def);
}

void
ns_webgpu_install(JSContext *ctx, ns_js *js, JSValueConst navigator)
{
    (void)js;
    if (!g_adapter_class) {
        wg_register_class(ctx, &g_adapter_class, "GPUAdapter",
                          wg_adapter_finalizer);
        wg_register_class(ctx, &g_device_class, "GPUDevice",
                          wg_device_finalizer);
        wg_register_class(ctx, &g_queue_class, "GPUQueue",
                          wg_queue_finalizer);
        wg_register_class(ctx, &g_buffer_class, "GPUBuffer",
                          wg_buffer_finalizer);
        wg_register_class(ctx, &g_context_class, "GPUCanvasContext",
                          wg_context_finalizer);
        wg_register_class(ctx, &g_texture_class, "GPUTexture",
                          wg_texture_finalizer);
        wg_register_class(ctx, &g_view_class, "GPUTextureView",
                          wg_view_finalizer);
        wg_register_class(ctx, &g_encoder_class, "GPUCommandEncoder",
                          wg_encoder_finalizer);
        wg_register_class(ctx, &g_pass_class, "GPURenderPassEncoder",
                          wg_pass_finalizer);
        wg_register_class(ctx, &g_cmdbuf_class, "GPUCommandBuffer",
                          wg_cmdbuf_finalizer);
        wg_register_class(ctx, &g_shader_class, "GPUShaderModule",
                          wg_shader_finalizer);
        wg_register_class(ctx, &g_pipeline_class, "GPURenderPipeline",
                          wg_pipeline_finalizer);
        wg_register_class(ctx, &g_bgl_class, "GPUBindGroupLayout",
                          wg_bgl_finalizer);
        wg_register_class(ctx, &g_pllayout_class, "GPUPipelineLayout",
                          wg_pllayout_finalizer);
        wg_register_class(ctx, &g_bindgroup_class, "GPUBindGroup",
                          wg_bindgroup_finalizer);
        wg_register_class(ctx, &g_sampler_class, "GPUSampler",
                          wg_sampler_finalizer);
        wg_register_class(ctx, &g_queryset_class, "GPUQuerySet",
                          wg_queryset_finalizer);
        wg_register_class(ctx, &g_compute_pipe_class, "GPUComputePipeline",
                          wg_compute_pipe_finalizer);
        wg_register_class(ctx, &g_compute_pass_class, "GPUComputePassEncoder",
                          wg_compute_pass_finalizer);
        wg_register_class(ctx, &g_bundle_encoder_class, "GPURenderBundleEncoder",
                          wg_bundle_encoder_finalizer);
        wg_register_class(ctx, &g_bundle_class, "GPURenderBundle",
                          wg_bundle_finalizer);
    }

    JSValue gpu = JS_NewObject(ctx);
    wg_bind(ctx, gpu, "requestAdapter", wg_gpu_requestAdapter, 1);
    wg_bind(ctx, gpu, "getPreferredCanvasFormat",
            wg_gpu_getPreferredCanvasFormat, 0);
    JS_SetPropertyStr(ctx, gpu, "wgslLanguageFeatures", wg_new_feature_set(ctx));
    JS_SetPropertyStr(ctx, (JSValueConst)navigator, "gpu", gpu);

    JSValue global = JS_GetGlobalObject(ctx);
    wg_install_interfaces(ctx, global);
    JSValue r = JS_Eval(ctx, wg_error_classes_js, strlen(wg_error_classes_js),
                        "<webgpu>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);

    JSValue buf_usage = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, buf_usage, "MAP_READ", JS_NewInt32(ctx, 0x0001));
    JS_SetPropertyStr(ctx, buf_usage, "MAP_WRITE", JS_NewInt32(ctx, 0x0002));
    JS_SetPropertyStr(ctx, buf_usage, "COPY_SRC", JS_NewInt32(ctx, 0x0004));
    JS_SetPropertyStr(ctx, buf_usage, "COPY_DST", JS_NewInt32(ctx, 0x0008));
    JS_SetPropertyStr(ctx, buf_usage, "INDEX", JS_NewInt32(ctx, 0x0010));
    JS_SetPropertyStr(ctx, buf_usage, "VERTEX", JS_NewInt32(ctx, 0x0020));
    JS_SetPropertyStr(ctx, buf_usage, "UNIFORM", JS_NewInt32(ctx, 0x0040));
    JS_SetPropertyStr(ctx, buf_usage, "STORAGE", JS_NewInt32(ctx, 0x0080));
    JS_SetPropertyStr(ctx, buf_usage, "INDIRECT", JS_NewInt32(ctx, 0x0100));
    JS_SetPropertyStr(ctx, buf_usage, "QUERY_RESOLVE", JS_NewInt32(ctx, 0x0200));
    JS_SetPropertyStr(ctx, global, "GPUBufferUsage", buf_usage);

    JSValue tex_usage = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tex_usage, "COPY_SRC", JS_NewInt32(ctx, 0x01));
    JS_SetPropertyStr(ctx, tex_usage, "COPY_DST", JS_NewInt32(ctx, 0x02));
    JS_SetPropertyStr(ctx, tex_usage, "TEXTURE_BINDING", JS_NewInt32(ctx, 0x04));
    JS_SetPropertyStr(ctx, tex_usage, "STORAGE_BINDING", JS_NewInt32(ctx, 0x08));
    JS_SetPropertyStr(ctx, tex_usage, "RENDER_ATTACHMENT", JS_NewInt32(ctx, 0x10));
    JS_SetPropertyStr(ctx, global, "GPUTextureUsage", tex_usage);

    JSValue shader_stage = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, shader_stage, "VERTEX", JS_NewInt32(ctx, 0x1));
    JS_SetPropertyStr(ctx, shader_stage, "FRAGMENT", JS_NewInt32(ctx, 0x2));
    JS_SetPropertyStr(ctx, shader_stage, "COMPUTE", JS_NewInt32(ctx, 0x4));
    JS_SetPropertyStr(ctx, global, "GPUShaderStage", shader_stage);

    JSValue color_write = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, color_write, "RED", JS_NewInt32(ctx, 0x1));
    JS_SetPropertyStr(ctx, color_write, "GREEN", JS_NewInt32(ctx, 0x2));
    JS_SetPropertyStr(ctx, color_write, "BLUE", JS_NewInt32(ctx, 0x4));
    JS_SetPropertyStr(ctx, color_write, "ALPHA", JS_NewInt32(ctx, 0x8));
    JS_SetPropertyStr(ctx, color_write, "ALL", JS_NewInt32(ctx, 0xF));
    JS_SetPropertyStr(ctx, global, "GPUColorWrite", color_write);

    JSValue map_mode = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, map_mode, "READ", JS_NewInt32(ctx, 0x1));
    JS_SetPropertyStr(ctx, map_mode, "WRITE", JS_NewInt32(ctx, 0x2));
    JS_SetPropertyStr(ctx, global, "GPUMapMode", map_mode);

    JS_FreeValue(ctx, global);
}

JSValue
ns_webgpu_get_context(JSContext *ctx, ns_js *js, JSValueConst canvas_obj,
                      const ns_node *canvas)
{
    (void)js; (void)canvas_obj;
    if (!ns_webgpu_allowed()) return JS_NULL;
    if (!g_context_class) return JS_NULL;
    if (!g_webgpu_ctx_by_node)
        g_webgpu_ctx_by_node = g_hash_table_new(g_direct_hash, g_direct_equal);
    ns_wg_context *existing = g_hash_table_lookup(g_webgpu_ctx_by_node, canvas);
    if (existing)
        return JS_DupValue(ctx, existing->self);

    JSValue obj = JS_NewObjectClass(ctx, g_context_class);
    if (JS_IsException(obj)) return obj;
    ns_wg_context *c = g_new0(ns_wg_context, 1);
    c->canvas = canvas;
    c->self = obj;
    c->rt = JS_GetRuntime(ctx);
    c->current = JS_UNDEFINED;
    c->format = WGPUTextureFormat_BGRA8Unorm;
    c->opaque = TRUE;
    JS_SetOpaque(obj, c);
    g_hash_table_insert(g_webgpu_ctx_by_node, (gpointer)canvas, c);

    JS_SetPropertyStr(ctx, obj, "canvas", JS_DupValue(ctx, canvas_obj));
    return obj;
}

static uint8_t
wg_half_to_u8(uint16_t h)
{
    if (h & 0x8000u) return 0;
    uint32_t exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    double v = exp == 0 ? ldexp((double)mant, -24)
             : exp == 31 ? 1.0
             : ldexp((double)(mant | 0x400u), (int)exp - 25);
    if (v >= 1.0) return 255;
    return (uint8_t)(v * 255.0 + 0.5);
}

cairo_surface_t *
ns_webgpu_canvas_surface(const ns_node *canvas)
{
    if (!g_webgpu_ctx_by_node) return NULL;
    ns_wg_context *c = g_hash_table_lookup(g_webgpu_ctx_by_node, canvas);
    if (!c || !c->configured || !c->target || !c->device || !c->queue)
        return NULL;
    int w = c->w, h = c->h;
    if (w <= 0 || h <= 0) return NULL;
    if (c->surf && !c->pending) return c->surf;

    gboolean half = c->format == WGPUTextureFormat_RGBA16Float;
    uint32_t bpp = half ? 8u : 4u;
    uint32_t bytes_per_row = ((uint32_t)w * bpp + 255u) & ~255u;
    uint64_t buf_size = (uint64_t)bytes_per_row * (uint64_t)h;

    if (!c->readback || c->readback_size != buf_size) {
        if (c->readback) wgpuBufferRelease(c->readback);
        WGPUBufferDescriptor bd;
        memset(&bd, 0, sizeof bd);
        bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        bd.size = buf_size;
        c->readback = wgpuDeviceCreateBuffer(c->device, &bd);
        c->readback_size = c->readback ? buf_size : 0;
    }
    WGPUBuffer rb = c->readback;
    if (!rb) return c->surf;
    c->pending = FALSE;
    JSValue cur = c->current;
    c->current = JS_UNDEFINED;
    JS_FreeValueRT(c->rt, cur);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(c->device, NULL);
    WGPUTexelCopyTextureInfo src;
    memset(&src, 0, sizeof src);
    src.texture = c->target;
    src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst;
    memset(&dst, 0, sizeof dst);
    dst.buffer = rb;
    dst.layout.bytesPerRow = bytes_per_row;
    dst.layout.rowsPerImage = (uint32_t)h;
    WGPUExtent3D copy_size = { (uint32_t)w, (uint32_t)h, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &copy_size);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(c->queue, 1, &cmd);

    wg_map_wait wait = { 0 };
    WGPUBufferMapCallbackInfo mci;
    memset(&mci, 0, sizeof mci);
    mci.mode = WGPUCallbackMode_AllowProcessEvents;
    mci.callback = wg_on_map;
    mci.userdata1 = &wait;
    wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, (size_t)buf_size, mci);
    for (int i = 0; i < 4000 && !wait.done; i++) {
        wgpuDevicePoll(c->device, 1, NULL);
        wgpuInstanceProcessEvents(ns_webgpu_instance());
    }

    const uint8_t *map = wait.done && wait.status == WGPUMapAsyncStatus_Success
        ? wgpuBufferGetConstMappedRange(rb, 0, (size_t)buf_size) : NULL;
    if (map) {
        if (!c->surf)
            c->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (c->surf &&
            cairo_surface_status(c->surf) == CAIRO_STATUS_SUCCESS) {
            cairo_surface_flush(c->surf);
            int stride = cairo_image_surface_get_stride(c->surf);
            uint8_t *dstp = cairo_image_surface_get_data(c->surf);
            gboolean bgra = c->format == WGPUTextureFormat_BGRA8Unorm ||
                            c->format == WGPUTextureFormat_BGRA8UnormSrgb;
            for (int y = 0; y < h; y++) {
                const uint8_t *s = map + (size_t)y * bytes_per_row;
                uint8_t *d = dstp + (size_t)y * stride;
                for (int x = 0; x < w; x++) {
                    uint8_t px[4];
                    if (half) {
                        uint16_t hp[4];
                        memcpy(hp, s + (size_t)x * 8, sizeof hp);
                        for (int k = 0; k < 4; k++) px[k] = wg_half_to_u8(hp[k]);
                    } else if (bgra) {
                        px[0] = s[x * 4 + 2]; px[1] = s[x * 4 + 1];
                        px[2] = s[x * 4 + 0]; px[3] = s[x * 4 + 3];
                    } else {
                        memcpy(px, s + (size_t)x * 4, 4);
                    }
                    uint8_t a = c->opaque ? 255u : px[3];
                    d[x * 4 + 0] = MIN(px[2], a);
                    d[x * 4 + 1] = MIN(px[1], a);
                    d[x * 4 + 2] = MIN(px[0], a);
                    d[x * 4 + 3] = a;
                }
            }
            cairo_surface_mark_dirty(c->surf);
        }
        wgpuBufferUnmap(rb);
    }
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    return c->surf;
}

#endif
