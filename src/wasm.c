/* Nordstjernen — WebAssembly JS API implemented over the vendored WAMR interpreter.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "wasm.h"
#include "js_classid.h"
#include "js_cage.h"
#include "js.h"

#if !defined(NS_HAVE_WAMR)

void ns_wasm_install(JSContext *ctx, JSValueConst global)
{
    (void)ctx;
    (void)global;
}

JSValue
ns_wasm_wire_handle(JSContext *ctx, JSValueConst v, const char **kind)
{
    (void)ctx;
    (void)v;
    *kind = NULL;
    return JS_UNDEFINED;
}

JSValue
ns_wasm_wire_restore(JSContext *ctx, const char *kind, JSValueConst sab)
{
    (void)ctx;
    (void)kind;
    (void)sab;
    return JS_NULL;
}

#else

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include <wasm_export.h>

#include "ns_wamr.h"
#include "ns_wamr_eh.h"

#define NS_WASM_STACK_SIZE (1u << 20)
#define NS_WASM_RECLAIM_INTERVAL 64
#define NS_WASM_MAX_PARAMS 64
#define NS_WASM_MAX_RESULTS 16
#define NS_WASM_PAGE_SIZE 65536u
#define NS_WASM_MEMORY_MAX_BYTES (512u * 1024u * 1024u)
#define NS_WASM_SHARED_MAX_BYTES (1024u * 1024u * 1024u)

static JSClassID ns_wasm_module_class_id;
static JSClassID ns_wasm_instance_class_id;
static JSClassID ns_wasm_memory_class_id;
static JSClassID ns_wasm_table_class_id;
static JSClassID ns_wasm_global_class_id;
static JSClassID ns_wasm_func_class_id;

static GMutex ns_wasm_link_mutex;
static _Thread_local gint64 ns_wasm_entered_us;
static GMutex ns_wasm_buffers_mutex;
static GHashTable *ns_wasm_buffers;

typedef struct {
    JSContext *ctx;
    JSValue fn;
    guint8 param_kinds[NS_WASM_MAX_PARAMS];
    guint param_count;
    guint8 result_kind;
    guint index;
    gboolean has_result;
} ns_wasm_binding;

typedef struct {
    GPtrArray *bindings;
    GPtrArray *symbol_blocks;
    GPtrArray *strings;
    JSValue provided_memory;
    JSValue provided_table;
} ns_wasm_linkage;

typedef struct {
    guint8 *load_bytes;
    size_t len;
    wasm_module_t module;
    ns_wasm_linkage linkage;
    gboolean imports_linked;
    gint refs;
} ns_wasm_module;

typedef struct {
    JSContext *ctx;
    wasm_module_inst_t inst;
    wasm_exec_env_t exec_env;
    JSValue module_obj;
    JSValue exports;
    JSValue memory_obj;
    JSValue pending_exc;
    ns_wasm_linkage linkage;
    GHashTable *funcs;
    gboolean has_pending;
    guint call_depth;
    guint calls_since_reclaim;
    struct ns_wasm_eh *eh;
} ns_wasm_instance;

typedef struct {
    JSContext *ctx;
    JSValue instance;
    JSValue buffer;
    guint8 *base;
    size_t size;
    guint8 *staging;
    size_t staging_size;
    uint32_t max_pages;
    wasm_memory_inst_t shared;
} ns_wasm_memory;

typedef struct {
    JSContext *ctx;
    JSValue instance;
    char *name;
    wasm_valkind_t elem_kind;
} ns_wasm_table;

typedef struct {
    JSContext *ctx;
    JSValue instance;
    char *name;
    JSValue ref_value;
    wasm_val_t val;
    wasm_valkind_t kind;
    gboolean standalone;
    gboolean is_mutable;
} ns_wasm_global;

typedef struct {
    JSContext *ctx;
    JSValue instance;
    wasm_function_inst_t func;
} ns_wasm_func;

typedef struct {
    JSRuntime *rt;
    JSValue v;
} ns_wasm_ref;

static GPrivate ns_wasm_thread_env_done = G_PRIVATE_INIT(NULL);
static GPrivate ns_wasm_instantiating = G_PRIVATE_INIT(NULL);

static void ns_wasm_on_memory_grown(wasm_module_inst_t inst, void *user_data);

static void
ns_wasm_linkage_init(ns_wasm_linkage *linkage)
{
    linkage->bindings = g_ptr_array_new();
    linkage->symbol_blocks = g_ptr_array_new_with_free_func(g_free);
    linkage->strings = g_ptr_array_new_with_free_func(g_free);
    linkage->provided_memory = JS_UNDEFINED;
    linkage->provided_table = JS_UNDEFINED;
}

static void
ns_wasm_linkage_clear(JSRuntime *rt, ns_wasm_linkage *linkage)
{
    if (!linkage)
        return;
    if (linkage->bindings) {
        for (guint i = 0; i < linkage->bindings->len; i++) {
            ns_wasm_binding *b = g_ptr_array_index(linkage->bindings, i);
            JS_FreeValueRT(rt, b->fn);
            g_free(b);
        }
        g_ptr_array_free(linkage->bindings, TRUE);
        linkage->bindings = NULL;
    }
    if (linkage->symbol_blocks) {
        g_ptr_array_free(linkage->symbol_blocks, TRUE);
        linkage->symbol_blocks = NULL;
    }
    if (linkage->strings) {
        g_ptr_array_free(linkage->strings, TRUE);
        linkage->strings = NULL;
    }
    JS_FreeValueRT(rt, linkage->provided_memory);
    linkage->provided_memory = JS_UNDEFINED;
    JS_FreeValueRT(rt, linkage->provided_table);
    linkage->provided_table = JS_UNDEFINED;
}

static void
ns_wasm_linkage_gc_mark(JSRuntime *rt, const ns_wasm_linkage *linkage,
                        JS_MarkFunc *mark_func)
{
    if (!linkage || !linkage->bindings)
        return;
    for (guint i = 0; i < linkage->bindings->len; i++) {
        ns_wasm_binding *b = g_ptr_array_index(linkage->bindings, i);
        JS_MarkValue(rt, b->fn, mark_func);
    }
    JS_MarkValue(rt, linkage->provided_memory, mark_func);
    JS_MarkValue(rt, linkage->provided_table, mark_func);
}

static gboolean
ns_wasm_usage_outside_cage(mem_alloc_usage_t usage)
{
    return usage != Alloc_For_LinearMemory && ns_js_cage_enabled();
}

static gboolean
ns_wasm_block_outside_cage(const void *ptr)
{
    return ns_js_cage_enabled() && !ns_js_cage_contains(ptr);
}

static void *
ns_wasm_runtime_alloc(mem_alloc_usage_t usage, unsigned int size)
{
    if (ns_wasm_usage_outside_cage(usage))
        return malloc(size ? size : 1);
    void *p = ns_js_cage_shared_malloc(size);
    if (p && usage == Alloc_For_LinearMemory)
        memset(p, 0, ns_js_cage_shared_usable_size(p));
    return p;
}

static void *
ns_wasm_runtime_realloc(mem_alloc_usage_t usage, bool full_size_mmaped,
                        void *ptr, unsigned int size)
{
    (void)full_size_mmaped;
    if (ptr ? ns_wasm_block_outside_cage(ptr) : ns_wasm_usage_outside_cage(usage))
        return realloc(ptr, size ? size : 1);
    size_t old = ptr ? ns_js_cage_shared_usable_size(ptr) : 0;
    void *p = ns_js_cage_shared_realloc(ptr, size);
    if (p && usage == Alloc_For_LinearMemory) {
        size_t now = ns_js_cage_shared_usable_size(p);
        if (now > old) memset((char *)p + old, 0, now - old);
    }
    return p;
}

static void
ns_wasm_runtime_free(mem_alloc_usage_t usage, void *ptr)
{
    (void)usage;
    if (ptr && ns_wasm_block_outside_cage(ptr))
        free(ptr);
    else
        ns_js_cage_shared_free(ptr);
}

static GMutex ns_wasm_shared_mutex;
static GHashTable *ns_wasm_shared_by_data;

static void
ns_wasm_shared_dup_cb(void *opaque)
{
    ns_wamr_shared_memory_dup(opaque);
}

static void
ns_wasm_shared_release_cb(void *opaque)
{
    ns_wamr_shared_memory_release(opaque);
}

static void
ns_wasm_shared_register(wasm_memory_inst_t mem)
{
    void *data = wasm_memory_get_base_address(mem);
    if (!data)
        return;
    g_mutex_lock(&ns_wasm_shared_mutex);
    if (!ns_wasm_shared_by_data)
        ns_wasm_shared_by_data = g_hash_table_new(g_direct_hash,
                                                  g_direct_equal);
    gboolean fresh = !g_hash_table_contains(ns_wasm_shared_by_data, data);
    if (fresh)
        g_hash_table_insert(ns_wasm_shared_by_data, data, mem);
    g_mutex_unlock(&ns_wasm_shared_mutex);
    if (fresh)
        ns_js_sab_register_foreign(data, ns_wasm_shared_dup_cb,
                                   ns_wasm_shared_release_cb, mem);
}

static void
ns_wasm_shared_destroyed(void *data)
{
    g_mutex_lock(&ns_wasm_shared_mutex);
    if (ns_wasm_shared_by_data)
        g_hash_table_remove(ns_wasm_shared_by_data, data);
    g_mutex_unlock(&ns_wasm_shared_mutex);
    ns_js_sab_unregister_foreign(data);
}

static wasm_memory_inst_t
ns_wasm_shared_lookup(void *data)
{
    g_mutex_lock(&ns_wasm_shared_mutex);
    wasm_memory_inst_t mem = ns_wasm_shared_by_data
        ? g_hash_table_lookup(ns_wasm_shared_by_data, data) : NULL;
    g_mutex_unlock(&ns_wasm_shared_mutex);
    return mem;
}

static gpointer
ns_wasm_runtime_init_once(gpointer data)
{
    RuntimeInitArgs args;
    memset(&args, 0, sizeof(args));
    args.mem_alloc_type = Alloc_With_Allocator;
    args.mem_alloc_option.allocator.malloc_func = (void *)ns_wasm_runtime_alloc;
    args.mem_alloc_option.allocator.realloc_func = (void *)ns_wasm_runtime_realloc;
    args.mem_alloc_option.allocator.free_func = (void *)ns_wasm_runtime_free;
    if (!wasm_runtime_full_init(&args))
        return NULL;
    wasm_runtime_set_log_level(g_getenv("ND_WASM_LOG")
                                   ? WASM_LOG_LEVEL_VERBOSE
                                   : WASM_LOG_LEVEL_ERROR);
    ns_wamr_set_shared_memory_destroy_callback(ns_wasm_shared_destroyed);
    wasm_runtime_set_enlarge_mem_success_callback(ns_wasm_on_memory_grown,
                                                  NULL);
    (void)data;
    return GINT_TO_POINTER(1);
}

static gboolean
ns_wasm_runtime_ready(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, ns_wasm_runtime_init_once, NULL);
    if (!once.retval)
        return FALSE;
    if (!g_private_get(&ns_wasm_thread_env_done)) {
        wasm_runtime_init_thread_env();
        g_private_set(&ns_wasm_thread_env_done, GINT_TO_POINTER(1));
    }
    return TRUE;
}

static guint8 *
ns_wasm_memdup_bytes(const guint8 *bytes, size_t len)
{
    if (len == 0)
        return g_malloc0(1);
    return g_memdup2(bytes, len);
}

static JSValue
ns_wasm_throw_named(JSContext *ctx, const char *class_name, const char *msg)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ns = JS_GetPropertyStr(ctx, global, "WebAssembly");
    JSValue ctor = JS_IsObject(ns) ? JS_GetPropertyStr(ctx, ns, class_name)
                                   : JS_UNDEFINED;
    JS_FreeValue(ctx, ns);
    JS_FreeValue(ctx, global);
    if (JS_IsFunction(ctx, ctor)) {
        JSValue arg = JS_NewString(ctx, msg ? msg : "wasm error");
        JSValue err = JS_CallConstructor(ctx, ctor, 1, &arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, ctor);
        if (!JS_IsException(err))
            return JS_Throw(ctx, err);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, ctor);
    return JS_ThrowTypeError(ctx, "%s: %s", class_name,
                             msg ? msg : "wasm error");
}

static guint8 *
ns_wasm_copy_buffer_source(JSContext *ctx, JSValueConst v, size_t *out_len)
{
    *out_len = 0;
    size_t off = 0, blen = 0;
    JSValue buf = JS_GetArrayBufferViewBuffer(ctx, v, &off, &blen);
    if (!JS_IsException(buf)) {
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, buf);
        guint8 *out = NULL;
        if (base && off <= total && blen <= total - off) {
            if (blen > G_MAXUINT32) {
                JS_ThrowRangeError(ctx, "wasm module too large");
                JS_FreeValue(ctx, buf);
                return NULL;
            }
            out = ns_wasm_memdup_bytes(base + off, blen);
            *out_len = blen;
        }
        JS_FreeValue(ctx, buf);
        return out;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    size_t total = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &total, v);
    if (base) {
        if (total > G_MAXUINT32) {
            JS_ThrowRangeError(ctx, "wasm module too large");
            return NULL;
        }
        *out_len = total;
        return ns_wasm_memdup_bytes(base, total);
    }
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    return NULL;
}

static JSValue
ns_wasm_buffer_source_error(JSContext *ctx, const char *msg)
{
    if (JS_HasException(ctx))
        return JS_EXCEPTION;
    return JS_ThrowTypeError(ctx, "%s", msg);
}

static int
ns_wasm_to_u32_index(JSContext *ctx, JSValueConst v, uint32_t *out,
                     const char *what)
{
    uint64_t idx = 0;
    if (JS_ToIndex(ctx, &idx, v))
        return -1;
    if (idx > G_MAXUINT32) {
        JS_ThrowRangeError(ctx, "%s is too large", what);
        return -1;
    }
    *out = (uint32_t)idx;
    return 0;
}

static ns_wasm_ref *
ns_wasm_ref_box(JSContext *ctx, JSValueConst v)
{
    ns_wasm_ref *r = g_new0(ns_wasm_ref, 1);
    r->rt = JS_GetRuntime(ctx);
    r->v = JS_DupValue(ctx, v);
    return r;
}

static void
ns_wasm_ref_cleanup(void *p)
{
    ns_wasm_ref *r = p;
    JS_FreeValueRT(r->rt, r->v);
    g_free(r);
}

static gboolean
ns_wasm_ref_register(wasm_module_inst_t inst, ns_wasm_ref *r, uint32_t *out_idx)
{
    uint32_t idx = 0;
    if (!wasm_externref_obj2ref(inst, r, &idx)) {
        ns_wasm_ref_cleanup(r);
        return FALSE;
    }
    wasm_externref_set_cleanup(inst, r, ns_wasm_ref_cleanup);
    if (out_idx)
        *out_idx = idx;
    return TRUE;
}

static JSValue
ns_wasm_ref_to_js(JSContext *ctx, void *obj)
{
    ns_wasm_ref *r = obj;
    if (!r)
        return JS_NULL;
    return JS_DupValue(ctx, r->v);
}

static ns_wasm_instance *
ns_wasm_instance_opaque(JSValueConst v)
{
    return JS_GetOpaque(v, ns_wasm_instance_class_id);
}

static int
ns_wasm_js_to_val(JSContext *ctx, ns_wasm_instance *wi, JSValueConst v,
                  wasm_valkind_t kind, wasm_val_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    switch (kind) {
    case WASM_I32: {
        int32_t i = 0;
        if (JS_ToInt32(ctx, &i, v))
            return -1;
        out->of.i32 = i;
        return 0;
    }
    case WASM_I64: {
        int64_t i = 0;
        if (JS_ToBigInt64(ctx, &i, v))
            return -1;
        out->of.i64 = i;
        return 0;
    }
    case WASM_F32: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        out->of.f32 = (float)d;
        return 0;
    }
    case WASM_F64: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        out->of.f64 = d;
        return 0;
    }
    case WASM_EXTERNREF: {
        ns_wasm_ref *r = ns_wasm_ref_box(ctx, v);
        if (!ns_wasm_ref_register(wi->inst, r, NULL)) {
            JS_ThrowRangeError(ctx, "wasm externref registration failed");
            return -1;
        }
        out->of.foreign = (uintptr_t)r;
        return 0;
    }
    default:
        JS_ThrowTypeError(ctx, "unsupported wasm value kind %d", (int)kind);
        return -1;
    }
}

static JSValue
ns_wasm_val_to_js(JSContext *ctx, const wasm_val_t *val)
{
    switch (val->kind) {
    case WASM_I32:
        return JS_NewInt32(ctx, val->of.i32);
    case WASM_I64:
        return JS_NewBigInt64(ctx, val->of.i64);
    case WASM_F32:
        return JS_NewFloat64(ctx, val->of.f32);
    case WASM_F64:
        return JS_NewFloat64(ctx, val->of.f64);
    case WASM_EXTERNREF:
        return ns_wasm_ref_to_js(ctx, (void *)val->of.foreign);
    default:
        return JS_UNDEFINED;
    }
}

#define NS_WASM_KIND_TAG 4

static JSClassID ns_wasm_tag_class_id;
static JSClassID ns_wasm_exception_class_id;
static const char ns_wasm_js_exception_identity;

typedef struct {
    void *identity;
    JSValue owner;
    wasm_valkind_t kinds[NS_WASM_MAX_PARAMS];
    guint n_kinds;
} ns_wasm_tag;

typedef struct {
    JSValue tag;
    JSValue *args;
    guint n_args;
} ns_wasm_exception;

struct ns_wasm_eh {
    JSValue *tags;
    guint n_tags;
    GArray *pins;
};

static void
ns_wasm_tag_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_tag *t = JS_GetOpaque(val, ns_wasm_tag_class_id);
    if (!t)
        return;
    JS_FreeValueRT(rt, t->owner);
    g_free(t);
}

static void
ns_wasm_tag_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_wasm_tag *t = JS_GetOpaque(val, ns_wasm_tag_class_id);
    if (t)
        JS_MarkValue(rt, t->owner, mark_func);
}

static const JSClassDef ns_wasm_tag_class = {
    "Tag",
    .finalizer = ns_wasm_tag_finalizer,
    .gc_mark = ns_wasm_tag_gc_mark,
};

static void
ns_wasm_exception_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_exception *e = JS_GetOpaque(val, ns_wasm_exception_class_id);
    if (!e)
        return;
    JS_FreeValueRT(rt, e->tag);
    for (guint i = 0; i < e->n_args; i++)
        JS_FreeValueRT(rt, e->args[i]);
    g_free(e->args);
    g_free(e);
}

static void
ns_wasm_exception_gc_mark(JSRuntime *rt, JSValueConst val,
                          JS_MarkFunc *mark_func)
{
    ns_wasm_exception *e = JS_GetOpaque(val, ns_wasm_exception_class_id);
    if (!e)
        return;
    JS_MarkValue(rt, e->tag, mark_func);
    for (guint i = 0; i < e->n_args; i++)
        JS_MarkValue(rt, e->args[i], mark_func);
}

static const JSClassDef ns_wasm_exception_class = {
    "Exception",
    .finalizer = ns_wasm_exception_finalizer,
    .gc_mark = ns_wasm_exception_gc_mark,
};

static int
ns_wasm_eh_length(JSContext *ctx, JSValueConst obj, int64_t *out)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, "length");
    if (JS_IsException(v))
        return -1;
    int r = JS_ToInt64(ctx, out, v);
    JS_FreeValue(ctx, v);
    return r;
}

static guint
ns_wasm_kind_cells(wasm_valkind_t kind)
{
    switch (kind) {
    case WASM_I64:
    case WASM_F64:
        return 2;
    case WASM_V128:
        return 4;
    default:
        return 1;
    }
}

static JSValue
ns_wasm_tag_new(JSContext *ctx, JSValueConst owner, wasm_module_t module,
                wasm_module_inst_t inst, uint32_t tag_index)
{
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_tag_class_id);
    if (JS_IsException(obj))
        return obj;
    ns_wasm_tag *t = g_new0(ns_wasm_tag, 1);
    t->owner = JS_DupValue(ctx, owner);
    t->identity = ns_wamr_tag_identity(inst, tag_index);
    t->n_kinds = MIN(ns_wamr_tag_param_count(module, tag_index),
                     G_N_ELEMENTS(t->kinds));
    for (guint i = 0; i < t->n_kinds; i++)
        t->kinds[i] = ns_wamr_tag_param_kind(module, tag_index, i);
    JS_SetOpaque(obj, t);
    return obj;
}

static gboolean
ns_wasm_tag_kind_from_name(const char *name, wasm_valkind_t *out)
{
    static const struct {
        const char *name;
        wasm_valkind_t kind;
    } kinds[] = {
        { "i32", WASM_I32 },       { "i64", WASM_I64 },
        { "f32", WASM_F32 },       { "f64", WASM_F64 },
        { "externref", WASM_EXTERNREF },
    };
    for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
        if (!strcmp(name, kinds[i].name)) {
            *out = kinds[i].kind;
            return TRUE;
        }
    }
    return FALSE;
}

static JSValue
ns_wasm_tag_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                 JSValueConst *argv)
{
    (void)new_target;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "WebAssembly.Tag requires a descriptor");
    JSValue params = JS_GetPropertyStr(ctx, argv[0], "parameters");
    if (JS_IsException(params))
        return params;
    int64_t len = 0;
    if (!JS_IsObject(params) || ns_wasm_eh_length(ctx, params, &len) < 0 ||
        len < 0 || len > NS_WASM_MAX_PARAMS) {
        JS_FreeValue(ctx, params);
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        return JS_ThrowTypeError(ctx,
                                 "WebAssembly.Tag parameters must be a list");
    }
    ns_wasm_tag *t = g_new0(ns_wasm_tag, 1);
    t->owner = JS_UNDEFINED;
    t->identity = t;
    guint cells = 0;
    for (int64_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, params, (uint32_t)i);
        const char *name = JS_IsException(v) ? NULL : JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        gboolean ok = name && ns_wasm_tag_kind_from_name(name, &t->kinds[i]);
        if (name)
            JS_FreeCString(ctx, name);
        if (!ok) {
            JS_FreeValue(ctx, params);
            g_free(t);
            if (JS_HasException(ctx))
                return JS_EXCEPTION;
            return JS_ThrowTypeError(ctx, "invalid WebAssembly.Tag parameter");
        }
        cells += ns_wasm_kind_cells(t->kinds[i]);
    }
    JS_FreeValue(ctx, params);
    t->n_kinds = (guint)len;
    if (cells > NS_WAMR_EXN_MAX_CELLS) {
        g_free(t);
        return JS_ThrowRangeError(ctx, "WebAssembly.Tag has too many parameters");
    }
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_tag_class_id);
    if (JS_IsException(obj)) {
        g_free(t);
        return obj;
    }
    JS_SetOpaque(obj, t);
    return obj;
}

static JSValue
ns_wasm_exception_arg_from_js(JSContext *ctx, wasm_valkind_t kind,
                              JSValueConst v)
{
    switch (kind) {
    case WASM_I32: {
        int32_t i = 0;
        if (JS_ToInt32(ctx, &i, v))
            return JS_EXCEPTION;
        return JS_NewInt32(ctx, i);
    }
    case WASM_I64: {
        int64_t i = 0;
        if (JS_ToBigInt64(ctx, &i, v))
            return JS_EXCEPTION;
        return JS_NewBigInt64(ctx, i);
    }
    case WASM_F32: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v))
            return JS_EXCEPTION;
        return JS_NewFloat64(ctx, (float)d);
    }
    case WASM_F64: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v))
            return JS_EXCEPTION;
        return JS_NewFloat64(ctx, d);
    }
    case WASM_EXTERNREF:
        return JS_DupValue(ctx, v);
    default:
        return JS_ThrowTypeError(ctx, "unsupported WebAssembly.Exception "
                                      "argument type");
    }
}

static JSValue
ns_wasm_exception_new(JSContext *ctx, JSValueConst tag_obj, JSValue *args,
                      guint n_args)
{
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_exception_class_id);
    if (JS_IsException(obj)) {
        for (guint i = 0; i < n_args; i++)
            JS_FreeValue(ctx, args[i]);
        g_free(args);
        return obj;
    }
    ns_wasm_exception *e = g_new0(ns_wasm_exception, 1);
    e->tag = JS_DupValue(ctx, tag_obj);
    e->args = args;
    e->n_args = n_args;
    JS_SetOpaque(obj, e);
    return obj;
}

static JSValue
ns_wasm_exception_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                       JSValueConst *argv)
{
    (void)new_target;
    ns_wasm_tag *t =
        argc > 0 ? JS_GetOpaque(argv[0], ns_wasm_tag_class_id) : NULL;
    if (!t)
        return JS_ThrowTypeError(ctx, "WebAssembly.Exception requires a Tag");
    JSValueConst payload = argc > 1 ? argv[1] : JS_UNDEFINED;
    int64_t len = 0;
    if (!JS_IsObject(payload) || ns_wasm_eh_length(ctx, payload, &len) < 0 ||
        len != (int64_t)t->n_kinds) {
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        return JS_ThrowTypeError(ctx, "WebAssembly.Exception payload does not "
                                      "match the tag");
    }
    JSValue *args = g_new0(JSValue, t->n_kinds ? t->n_kinds : 1);
    for (guint i = 0; i < t->n_kinds; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, payload, i);
        args[i] = JS_IsException(v)
                      ? v
                      : ns_wasm_exception_arg_from_js(ctx, t->kinds[i], v);
        JS_FreeValue(ctx, v);
        if (JS_IsException(args[i])) {
            for (guint j = 0; j < i; j++)
                JS_FreeValue(ctx, args[j]);
            g_free(args);
            return JS_EXCEPTION;
        }
    }
    JSValue obj = ns_wasm_exception_new(ctx, argv[0], args, t->n_kinds);
    if (JS_IsException(obj) || argc < 3 || !JS_IsObject(argv[2]))
        return obj;
    JSValue trace = JS_GetPropertyStr(ctx, argv[2], "traceStack");
    gboolean want_stack = JS_ToBool(ctx, trace) > 0;
    JS_FreeValue(ctx, trace);
    if (want_stack) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue error_ctor = JS_GetPropertyStr(ctx, global, "Error");
        JS_FreeValue(ctx, global);
        JSValue err = JS_IsFunction(ctx, error_ctor)
                          ? JS_CallConstructor(ctx, error_ctor, 0, NULL)
                          : JS_NewError(ctx);
        JS_FreeValue(ctx, error_ctor);
        JSValue stack = JS_IsException(err)
                            ? JS_UNDEFINED
                            : JS_GetPropertyStr(ctx, err, "stack");
        JS_FreeValue(ctx, err);
        if (JS_IsException(stack))
            stack = JS_UNDEFINED;
        JS_DefinePropertyValueStr(ctx, obj, "stack", stack,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }
    return obj;
}

static ns_wasm_exception *
ns_wasm_exception_this(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv, ns_wasm_tag **out_tag)
{
    ns_wasm_exception *e = JS_GetOpaque(this_val, ns_wasm_exception_class_id);
    if (!e) {
        JS_ThrowTypeError(ctx, "not a WebAssembly.Exception");
        return NULL;
    }
    *out_tag = argc > 0 ? JS_GetOpaque(argv[0], ns_wasm_tag_class_id) : NULL;
    if (!*out_tag) {
        JS_ThrowTypeError(ctx, "argument is not a WebAssembly.Tag");
        return NULL;
    }
    return e;
}

static gboolean
ns_wasm_exception_has_tag(const ns_wasm_exception *e, const ns_wasm_tag *t)
{
    ns_wasm_tag *own = JS_GetOpaque(e->tag, ns_wasm_tag_class_id);
    return own && own->identity == t->identity;
}

static JSValue
ns_wasm_exception_is(JSContext *ctx, JSValueConst this_val, int argc,
                     JSValueConst *argv)
{
    ns_wasm_tag *t = NULL;
    ns_wasm_exception *e = ns_wasm_exception_this(ctx, this_val, argc, argv, &t);
    if (!e)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, ns_wasm_exception_has_tag(e, t));
}

static JSValue
ns_wasm_exception_get_arg(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv)
{
    ns_wasm_tag *t = NULL;
    ns_wasm_exception *e = ns_wasm_exception_this(ctx, this_val, argc, argv, &t);
    if (!e)
        return JS_EXCEPTION;
    if (!ns_wasm_exception_has_tag(e, t))
        return JS_ThrowTypeError(ctx, "WebAssembly.Exception has another tag");
    uint32_t index = 0;
    if (ns_wasm_to_u32_index(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &index,
                             "index"))
        return JS_EXCEPTION;
    if (index >= e->n_args)
        return JS_ThrowRangeError(ctx, "WebAssembly.Exception argument index "
                                       "out of range");
    return JS_DupValue(ctx, e->args[index]);
}

static const JSCFunctionListEntry ns_wasm_exception_proto_funcs[] = {
    JS_CFUNC_DEF("is", 1, ns_wasm_exception_is),
    JS_CFUNC_DEF("getArg", 2, ns_wasm_exception_get_arg),
};

static struct ns_wasm_eh *
ns_wasm_eh_state(ns_wasm_instance *wi)
{
    if (!wi->eh) {
        wi->eh = g_new0(struct ns_wasm_eh, 1);
        wi->eh->pins = g_array_new(FALSE, FALSE, sizeof(JSValue));
    }
    return wi->eh;
}

static void
ns_wasm_eh_release_pins(JSRuntime *rt, struct ns_wasm_eh *eh)
{
    for (guint i = 0; i < eh->pins->len; i++)
        JS_FreeValueRT(rt, g_array_index(eh->pins, JSValue, i));
    g_array_set_size(eh->pins, 0);
}

static void
ns_wasm_eh_free(JSRuntime *rt, ns_wasm_instance *wi)
{
    struct ns_wasm_eh *eh = wi->eh;
    if (!eh)
        return;
    ns_wasm_eh_release_pins(rt, eh);
    g_array_free(eh->pins, TRUE);
    for (guint i = 0; i < eh->n_tags; i++)
        JS_FreeValueRT(rt, eh->tags[i]);
    g_free(eh->tags);
    g_free(eh);
    wi->eh = NULL;
}

static void
ns_wasm_eh_mark(JSRuntime *rt, ns_wasm_instance *wi, JS_MarkFunc *mark_func)
{
    struct ns_wasm_eh *eh = wi->eh;
    if (!eh)
        return;
    for (guint i = 0; i < eh->pins->len; i++)
        JS_MarkValue(rt, g_array_index(eh->pins, JSValue, i), mark_func);
    for (guint i = 0; i < eh->n_tags; i++)
        JS_MarkValue(rt, eh->tags[i], mark_func);
}

static void
ns_wasm_eh_reset(JSContext *ctx, ns_wasm_instance *wi)
{
    ns_wamr_exn_clear(wi->inst);
    if (wi->eh)
        ns_wasm_eh_release_pins(JS_GetRuntime(ctx), wi->eh);
}

static gboolean
ns_wasm_eh_tags_match(const ns_wasm_tag *t, wasm_module_t module,
                      uint32_t tag_index)
{
    if (t->n_kinds != ns_wamr_tag_param_count(module, tag_index))
        return FALSE;
    for (guint i = 0; i < t->n_kinds; i++)
        if (t->kinds[i] != ns_wamr_tag_param_kind(module, tag_index, i))
            return FALSE;
    return TRUE;
}

static gboolean
ns_wasm_eh_setup(JSContext *ctx, JSValueConst instance_obj,
                 ns_wasm_instance *wi, ns_wasm_module *mod,
                 JSValueConst import_object)
{
    uint32_t n_tags = ns_wamr_tag_count(mod->module);
    uint32_t n_imports = ns_wamr_import_tag_count(mod->module);
    if (n_tags == 0)
        return TRUE;

    struct ns_wasm_eh *eh = ns_wasm_eh_state(wi);
    eh->tags = g_new(JSValue, n_tags);
    for (uint32_t i = 0; i < n_tags; i++)
        eh->tags[i] = JS_UNDEFINED;
    eh->n_tags = n_tags;

    for (uint32_t i = 0; i < n_imports; i++) {
        const char *module_name = NULL, *name = NULL;
        ns_wamr_import_tag_name(mod->module, i, &module_name, &name);
        JSValue ns = JS_IsObject(import_object)
                         ? JS_GetPropertyStr(ctx, import_object, module_name)
                         : JS_UNDEFINED;
        JSValue v = JS_IsObject(ns) ? JS_GetPropertyStr(ctx, ns, name)
                                    : JS_UNDEFINED;
        JS_FreeValue(ctx, ns);
        ns_wasm_tag *t = JS_GetOpaque(v, ns_wasm_tag_class_id);
        if (!t || !ns_wasm_eh_tags_match(t, mod->module, i) ||
            !ns_wamr_tag_link_import(wi->inst, i, t->identity)) {
            JS_FreeValue(ctx, v);
            if (JS_HasException(ctx))
                return FALSE;
            char *msg = g_strdup_printf(
                "tag import %s.%s is missing or has the wrong type",
                module_name, name);
            ns_wasm_throw_named(ctx, "LinkError", msg);
            g_free(msg);
            return FALSE;
        }
        eh->tags[i] = v;
    }
    for (uint32_t i = n_imports; i < n_tags; i++) {
        eh->tags[i] = ns_wasm_tag_new(ctx, instance_obj, mod->module, wi->inst,
                                      i);
        if (JS_IsException(eh->tags[i])) {
            eh->tags[i] = JS_UNDEFINED;
            return FALSE;
        }
    }

    int32_t n_exports = wasm_runtime_get_export_count(mod->module);
    for (int32_t i = 0; i < n_exports; i++) {
        uint32_t tag_index = 0;
        if (!ns_wamr_export_tag_index(mod->module, i, &tag_index) ||
            tag_index >= n_tags)
            continue;
        wasm_export_t exp;
        wasm_runtime_get_export_type(mod->module, i, &exp);
        if (JS_SetPropertyStr(ctx, wi->exports, exp.name,
                              JS_DupValue(ctx, eh->tags[tag_index])) < 0)
            return FALSE;
    }
    return TRUE;
}

static JSValue
ns_wasm_eh_tag_for_identity(ns_wasm_instance *wi, void *identity)
{
    struct ns_wasm_eh *eh = wi->eh;
    if (!eh)
        return JS_UNDEFINED;
    for (guint i = 0; i < eh->n_tags; i++) {
        ns_wasm_tag *t = JS_GetOpaque(eh->tags[i], ns_wasm_tag_class_id);
        if (t && t->identity == identity)
            return eh->tags[i];
    }
    for (guint i = 0; i < eh->pins->len; i++) {
        JSValue v = g_array_index(eh->pins, JSValue, i);
        ns_wasm_tag *t = JS_GetOpaque(v, ns_wasm_tag_class_id);
        if (t && t->identity == identity)
            return v;
    }
    return JS_UNDEFINED;
}

static JSValue
ns_wasm_eh_cell_to_js(JSContext *ctx, wasm_valkind_t kind,
                      const uint32_t *cell)
{
    switch (kind) {
    case WASM_I32:
        return JS_NewInt32(ctx, (int32_t)cell[0]);
    case WASM_I64: {
        int64_t v;
        memcpy(&v, cell, sizeof(v));
        return JS_NewBigInt64(ctx, v);
    }
    case WASM_F32: {
        float v;
        memcpy(&v, cell, sizeof(v));
        return JS_NewFloat64(ctx, v);
    }
    case WASM_F64: {
        double v;
        memcpy(&v, cell, sizeof(v));
        return JS_NewFloat64(ctx, v);
    }
    case WASM_EXTERNREF: {
        void *obj = NULL;
        if (cell[0] == NS_WAMR_NULL_REF ||
            !wasm_externref_ref2obj(cell[0], &obj))
            return JS_NULL;
        return ns_wasm_ref_to_js(ctx, obj);
    }
    default:
        return JS_UNDEFINED;
    }
}

static gboolean
ns_wasm_eh_take(JSContext *ctx, ns_wasm_instance *wi, JSValue *out)
{
    void *identity = NULL;
    const uint32_t *cells = NULL;
    uint32_t cell_num = 0;
    if (!wi->inst ||
        !ns_wamr_exn_pending(wi->inst, &identity, &cells, &cell_num))
        return FALSE;

    if (identity == &ns_wasm_js_exception_identity) {
        guint pin = cells[0];
        *out = wi->eh && pin < wi->eh->pins->len
                   ? JS_DupValue(ctx, g_array_index(wi->eh->pins, JSValue, pin))
                   : JS_UNDEFINED;
        ns_wamr_exn_clear(wi->inst);
        return TRUE;
    }

    JSValue tag_obj = ns_wasm_eh_tag_for_identity(wi, identity);
    ns_wasm_tag *t = JS_GetOpaque(tag_obj, ns_wasm_tag_class_id);
    if (!t)
        return FALSE;
    JSValue *args = g_new0(JSValue, t->n_kinds ? t->n_kinds : 1);
    guint pos = 0;
    for (guint i = 0; i < t->n_kinds; i++) {
        guint need = ns_wasm_kind_cells(t->kinds[i]);
        args[i] = pos + need <= cell_num
                      ? ns_wasm_eh_cell_to_js(ctx, t->kinds[i], cells + pos)
                      : JS_UNDEFINED;
        pos += need;
    }
    ns_wamr_exn_clear(wi->inst);
    *out = ns_wasm_exception_new(ctx, tag_obj, args, t->n_kinds);
    return TRUE;
}

static gboolean
ns_wasm_eh_js_to_cells(JSContext *ctx, ns_wasm_instance *wi,
                       const ns_wasm_tag *t, const ns_wasm_exception *e,
                       uint32_t *cells, uint32_t *out_cell_num)
{
    guint pos = 0;
    if (e->n_args != t->n_kinds)
        return FALSE;
    for (guint i = 0; i < t->n_kinds; i++) {
        guint need = ns_wasm_kind_cells(t->kinds[i]);
        if (pos + need > NS_WAMR_EXN_MAX_CELLS)
            return FALSE;
        if (t->kinds[i] == WASM_EXTERNREF) {
            uint32_t idx = 0;
            if (!ns_wasm_ref_register(wi->inst, ns_wasm_ref_box(ctx, e->args[i]),
                                      &idx))
                return FALSE;
            cells[pos] = idx;
        } else {
            wasm_val_t val;
            if (ns_wasm_js_to_val(ctx, wi, e->args[i], t->kinds[i], &val))
                return FALSE;
            memcpy(cells + pos, &val.of, need * sizeof(uint32_t));
        }
        pos += need;
    }
    *out_cell_num = pos;
    return TRUE;
}

static gboolean
ns_wasm_eh_raise(JSContext *ctx, ns_wasm_instance *wi,
                 wasm_module_inst_t inst, JSValue exc)
{
    if (!wi || !inst || inst != wi->inst) {
        JS_FreeValue(ctx, exc);
        return FALSE;
    }
    struct ns_wasm_eh *eh = ns_wasm_eh_state(wi);
    ns_wasm_exception *e = JS_GetOpaque(exc, ns_wasm_exception_class_id);
    ns_wasm_tag *t = e ? JS_GetOpaque(e->tag, ns_wasm_tag_class_id) : NULL;
    if (t) {
        uint32_t cells[NS_WAMR_EXN_MAX_CELLS];
        uint32_t cell_num = 0;
        if (ns_wasm_eh_js_to_cells(ctx, wi, t, e, cells, &cell_num)) {
            JSValue tag_obj = JS_DupValue(ctx, e->tag);
            g_array_append_val(eh->pins, tag_obj);
            JS_FreeValue(ctx, exc);
            return ns_wamr_exn_raise(inst, t->identity, cells, cell_num);
        }
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    uint32_t cells[2] = { eh->pins->len, 0 };
    g_array_append_val(eh->pins, exc);
    return ns_wamr_exn_raise(inst, (void *)&ns_wasm_js_exception_identity,
                             cells, 2);
}

static void
ns_wasm_eh_install(JSContext *ctx, JSValueConst ns)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    ns_new_class_id(&ns_wasm_tag_class_id);
    if (!JS_IsRegisteredClass(rt, ns_wasm_tag_class_id))
        JS_NewClass(rt, ns_wasm_tag_class_id, &ns_wasm_tag_class);
    ns_new_class_id(&ns_wasm_exception_class_id);
    if (!JS_IsRegisteredClass(rt, ns_wasm_exception_class_id))
        JS_NewClass(rt, ns_wasm_exception_class_id, &ns_wasm_exception_class);

    const int iface = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
    JSValue tag_proto = JS_NewObject(ctx);
    JS_SetClassProto(ctx, ns_wasm_tag_class_id, JS_DupValue(ctx, tag_proto));
    JSValue tag_ctor = JS_NewCFunction2(ctx, ns_wasm_tag_ctor, "Tag", 1,
                                        JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, tag_ctor, "prototype", tag_proto);
    JS_DefinePropertyValueStr(ctx, ns, "Tag", tag_ctor, iface);

    JSValue exn_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, exn_proto, ns_wasm_exception_proto_funcs,
                               G_N_ELEMENTS(ns_wasm_exception_proto_funcs));
    JS_SetClassProto(ctx, ns_wasm_exception_class_id,
                     JS_DupValue(ctx, exn_proto));
    JSValue exn_ctor = JS_NewCFunction2(ctx, ns_wasm_exception_ctor,
                                        "Exception", 2, JS_CFUNC_constructor,
                                        0);
    JS_SetPropertyStr(ctx, exn_ctor, "prototype", exn_proto);
    JS_DefinePropertyValueStr(ctx, ns, "Exception", exn_ctor, iface);
}

static JSValue
ns_wasm_call_result(JSContext *ctx, ns_wasm_instance *wi, gboolean ok,
                    guint n_results, const wasm_val_t *results)
{
    if (!ok) {
        JSValue thrown;
        if (ns_wasm_eh_take(ctx, wi, &thrown))
            return JS_IsException(thrown) ? thrown : JS_Throw(ctx, thrown);
        if (wi->has_pending) {
            JSValue exc = wi->pending_exc;
            wi->pending_exc = JS_UNDEFINED;
            wi->has_pending = FALSE;
            wasm_runtime_clear_exception(wi->inst);
            return JS_Throw(ctx, exc);
        }
        const char *msg = wasm_runtime_get_exception(wi->inst);
        JSValue ret = ns_wasm_throw_named(ctx, "RuntimeError",
                                          msg ? msg : "wasm trap");
        wasm_runtime_clear_exception(wi->inst);
        return ret;
    }

    if (n_results == 0)
        return JS_UNDEFINED;
    if (n_results == 1)
        return ns_wasm_val_to_js(ctx, &results[0]);
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (guint i = 0; i < n_results; i++) {
        JSValue v = ns_wasm_val_to_js(ctx, &results[i]);
        if (JS_IsException(v) ||
            JS_DefinePropertyValueUint32(ctx, arr, i, v, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue
ns_wasm_call_function(JSContext *ctx, ns_wasm_instance *wi,
                      wasm_function_inst_t func, int argc, JSValueConst *argv)
{
    if (!wi || !wi->inst || !wi->exec_env)
        return JS_ThrowTypeError(ctx, "wasm instance is gone");

    guint n_params = wasm_func_get_param_count(func, wi->inst);
    guint n_results = wasm_func_get_result_count(func, wi->inst);
    wasm_valkind_t param_kinds[NS_WASM_MAX_PARAMS];
    wasm_valkind_t result_kinds[NS_WASM_MAX_RESULTS];
    if (n_params > G_N_ELEMENTS(param_kinds) ||
        n_results > G_N_ELEMENTS(result_kinds))
        return JS_ThrowTypeError(ctx, "wasm function arity too large");
    wasm_func_get_param_types(func, wi->inst, param_kinds);
    wasm_func_get_result_types(func, wi->inst, result_kinds);

    wasm_val_t args[NS_WASM_MAX_PARAMS], results[NS_WASM_MAX_RESULTS];
    memset(results, 0, sizeof(results));
    for (guint pass = 0; pass < 2; pass++) {
        for (guint i = 0; i < n_params; i++) {
            if ((param_kinds[i] == WASM_EXTERNREF) != (pass == 1))
                continue;
            JSValueConst src = (int)i < argc ? argv[i] : JS_UNDEFINED;
            if (ns_wasm_js_to_val(ctx, wi, src, param_kinds[i], &args[i])) {
                if (wi->call_depth == 0)
                    ns_wamr_externref_reclaim(wi->inst);
                return JS_EXCEPTION;
            }
        }
    }

    if (wi->call_depth == 0) {
        wasm_runtime_clear_exception(wi->inst);
        ns_wasm_eh_reset(ctx, wi);
        if (wi->has_pending) {
            JS_FreeValue(ctx, wi->pending_exc);
            wi->pending_exc = JS_UNDEFINED;
            wi->has_pending = FALSE;
        }
    }

    wi->call_depth++;
    gint64 outer_mark = ns_wasm_entered_us;
    ns_wasm_entered_us = g_get_monotonic_time();
    gboolean ok = wasm_runtime_call_wasm_a(wi->exec_env, func, n_results,
                                           results, n_params, args);
    ns_js_credit_wasm_time(ctx, g_get_monotonic_time() - ns_wasm_entered_us);
    ns_wasm_entered_us = outer_mark;
    wi->call_depth--;

    JSValue ret = ns_wasm_call_result(ctx, wi, ok, n_results, results);

    if (wi->call_depth == 0 &&
        ++wi->calls_since_reclaim >= NS_WASM_RECLAIM_INTERVAL) {
        wi->calls_since_reclaim = 0;
        ns_wamr_externref_reclaim(wi->inst);
    }
    return ret;
}

static void
ns_wasm_func_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_func *f = JS_GetOpaque(val, ns_wasm_func_class_id);
    if (!f)
        return;
    JS_FreeValueRT(rt, f->instance);
    g_free(f);
}

static void
ns_wasm_func_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_wasm_func *f = JS_GetOpaque(val, ns_wasm_func_class_id);
    if (f)
        JS_MarkValue(rt, f->instance, mark_func);
}

static JSValue
ns_wasm_func_call(JSContext *ctx, JSValueConst func_obj, JSValueConst this_val,
                  int argc, JSValueConst *argv, int flags)
{
    ns_wasm_func *f = JS_GetOpaque(func_obj, ns_wasm_func_class_id);
    if (!f)
        return JS_ThrowTypeError(ctx, "not a wasm function");
    (void)this_val;
    (void)flags;
    return ns_wasm_call_function(ctx, ns_wasm_instance_opaque(f->instance),
                                 f->func, argc, argv);
}

static const JSClassDef ns_wasm_func_class = {
    "WasmFunction",
    .finalizer = ns_wasm_func_finalizer,
    .gc_mark = ns_wasm_func_gc_mark,
    .call = ns_wasm_func_call,
};

static JSValue
ns_wasm_make_func(JSContext *ctx, JSValueConst instance,
                  wasm_function_inst_t func)
{
    ns_wasm_instance *wi = ns_wasm_instance_opaque(instance);
    if (wi && wi->funcs) {
        JSValue *known = g_hash_table_lookup(wi->funcs, func);
        if (known)
            return JS_DupValue(ctx, *known);
    }
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_func_class_id);
    if (JS_IsException(obj))
        return obj;
    ns_wasm_func *f = g_new0(ns_wasm_func, 1);
    f->ctx = ctx;
    ns_js_pin_realm(ctx);
    f->instance = JS_DupValue(ctx, instance);
    f->func = func;
    JS_SetOpaque(obj, f);
    if (wi && wi->inst) {
        guint n_params = wasm_func_get_param_count(func, wi->inst);
        JS_DefinePropertyValueStr(ctx, obj, "length",
                                  JS_NewInt32(ctx, (int)n_params),
                                  JS_PROP_CONFIGURABLE);
        if (!wi->funcs)
            wi->funcs = g_hash_table_new(g_direct_hash, g_direct_equal);
        JSValue *slot = g_new(JSValue, 1);
        *slot = JS_DupValue(ctx, obj);
        g_hash_table_insert(wi->funcs, func, slot);
    }
    return obj;
}

static void
ns_wasm_buffer_track(JSValueConst buffer, gboolean live)
{
    g_mutex_lock(&ns_wasm_buffers_mutex);
    if (!ns_wasm_buffers)
        ns_wasm_buffers = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (live)
        g_hash_table_add(ns_wasm_buffers, JS_VALUE_GET_PTR(buffer));
    else
        g_hash_table_remove(ns_wasm_buffers, JS_VALUE_GET_PTR(buffer));
    g_mutex_unlock(&ns_wasm_buffers_mutex);
}

static gboolean
ns_wasm_buffer_is_memory(JSValueConst v)
{
    if (!JS_IsObject(v))
        return FALSE;
    g_mutex_lock(&ns_wasm_buffers_mutex);
    gboolean hit = ns_wasm_buffers &&
                   g_hash_table_contains(ns_wasm_buffers, JS_VALUE_GET_PTR(v));
    g_mutex_unlock(&ns_wasm_buffers_mutex);
    return hit;
}

static void
ns_wasm_memory_detach_buffer(ns_wasm_memory *m)
{
    if (!m || JS_IsUndefined(m->buffer))
        return;
    ns_wasm_buffer_track(m->buffer, FALSE);
    JS_DetachArrayBuffer(m->ctx, m->buffer);
    JS_FreeValue(m->ctx, m->buffer);
    m->buffer = JS_UNDEFINED;
    m->base = NULL;
    m->size = 0;
}

static void
ns_wasm_memory_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_memory *m = JS_GetOpaque(val, ns_wasm_memory_class_id);
    if (!m)
        return;
    if (m->shared) {
        JS_FreeValueRT(rt, m->buffer);
        JS_FreeValueRT(rt, m->instance);
        ns_wamr_shared_memory_release(m->shared);
        g_free(m);
        return;
    }
    ns_wasm_memory_detach_buffer(m);
    JS_FreeValueRT(rt, m->instance);
    ns_js_cage_shared_free(m->staging);
    g_free(m);
}

static void
ns_wasm_memory_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_wasm_memory *m = JS_GetOpaque(val, ns_wasm_memory_class_id);
    if (!m)
        return;
    JS_MarkValue(rt, m->instance, mark_func);
    JS_MarkValue(rt, m->buffer, mark_func);
}

static const JSClassDef ns_wasm_memory_class = {
    "Memory",
    .finalizer = ns_wasm_memory_finalizer,
    .gc_mark = ns_wasm_memory_gc_mark,
};

static JSValue
ns_wasm_shared_buffer(JSContext *ctx, ns_wasm_memory *m)
{
    guint8 *base = wasm_memory_get_base_address(m->shared);
    size_t size = (size_t)wasm_memory_get_cur_page_count(m->shared) *
                  wasm_memory_get_bytes_per_page(m->shared);
    if (!JS_IsUndefined(m->buffer) && m->base == base && m->size == size)
        return JS_DupValue(ctx, m->buffer);
    JSValue buffer = JS_NewArrayBuffer(ctx, base, size, 0, NULL, NULL, TRUE);
    if (JS_IsException(buffer))
        return buffer;
    JS_FreeValue(ctx, m->buffer);
    m->buffer = buffer;
    m->base = base;
    m->size = size;
    return JS_DupValue(ctx, m->buffer);
}

static JSValue
ns_wasm_memory_new_shared(JSContext *ctx, wasm_memory_inst_t mem)
{
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_memory_class_id);
    if (JS_IsException(obj)) {
        ns_wamr_shared_memory_release(mem);
        return obj;
    }
    ns_wasm_memory *m = g_new0(ns_wasm_memory, 1);
    m->ctx = ctx;
    ns_js_pin_realm(ctx);
    m->instance = JS_UNDEFINED;
    m->buffer = JS_UNDEFINED;
    m->shared = mem;
    m->max_pages = ns_wamr_memory_max_pages(mem);
    ns_wasm_shared_register(mem);
    JS_SetOpaque(obj, m);
    return obj;
}

static JSValue
ns_wasm_memory_buffer_get(JSContext *ctx, JSValueConst this_val)
{
    ns_wasm_memory *m = JS_GetOpaque2(ctx, this_val, ns_wasm_memory_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (m->shared)
        return ns_wasm_shared_buffer(ctx, m);
    guint8 *base;
    size_t size;
    if (m->staging) {
        base = m->staging;
        size = m->staging_size;
    } else {
        ns_wasm_instance *wi = ns_wasm_instance_opaque(m->instance);
        if (!wi || !wi->inst)
            return JS_ThrowTypeError(ctx, "wasm instance is gone");
        wasm_memory_inst_t mem = wasm_runtime_get_memory(wi->inst, 0);
        if (!mem)
            return JS_ThrowTypeError(ctx, "wasm memory is gone");
        base = wasm_memory_get_base_address(mem);
        size = (size_t)wasm_memory_get_cur_page_count(mem) *
               wasm_memory_get_bytes_per_page(mem);
    }
    if (!JS_IsUndefined(m->buffer) && m->base == base && m->size == size)
        return JS_DupValue(ctx, m->buffer);
    ns_wasm_memory_detach_buffer(m);
    JSValue buffer = JS_NewArrayBuffer(ctx, base, size, 0, NULL, NULL, FALSE);
    if (JS_IsException(buffer)) {
        m->buffer = JS_UNDEFINED;
        m->base = NULL;
        m->size = 0;
        return buffer;
    }
    m->buffer = buffer;
    m->base = base;
    m->size = size;
    ns_wasm_buffer_track(m->buffer, TRUE);
    return JS_DupValue(ctx, m->buffer);
}

static JSValue
ns_wasm_buffer_transfer_guard(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int magic,
                              JSValueConst *func_data)
{
    (void)magic;
    if (ns_wasm_buffer_is_memory(this_val))
        return JS_ThrowTypeError(ctx, "cannot transfer the buffer of a "
                                      "WebAssembly.Memory");
    return JS_Call(ctx, func_data[0], this_val, argc, argv);
}

static void
ns_wasm_guard_buffer_transfer(JSContext *ctx, JSValueConst global)
{
    static const char *const names[] = {
        "transfer", "transferToFixedLength", "transferToImmutable",
    };
    JSValue ctor = JS_GetPropertyStr(ctx, global, "ArrayBuffer");
    JSValue proto = JS_IsObject(ctor) ? JS_GetPropertyStr(ctx, ctor, "prototype")
                                      : JS_UNDEFINED;
    JS_FreeValue(ctx, ctor);
    for (gsize i = 0; JS_IsObject(proto) && i < G_N_ELEMENTS(names); i++) {
        JSValue orig = JS_GetPropertyStr(ctx, proto, names[i]);
        if (JS_IsFunction(ctx, orig)) {
            JSValue guard = JS_NewCFunctionData(ctx, ns_wasm_buffer_transfer_guard,
                                                0, 0, 1, &orig);
            JS_DefinePropertyValueStr(ctx, guard, "name",
                                      JS_NewString(ctx, names[i]),
                                      JS_PROP_CONFIGURABLE);
            JS_DefinePropertyValueStr(ctx, proto, names[i], guard,
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        }
        JS_FreeValue(ctx, orig);
    }
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, proto);
}

static JSValue
ns_wasm_memory_grow(JSContext *ctx, JSValueConst this_val, int argc,
                    JSValueConst *argv)
{
    ns_wasm_memory *m = JS_GetOpaque2(ctx, this_val, ns_wasm_memory_class_id);
    if (!m)
        return JS_EXCEPTION;
    uint32_t delta = 0;
    if (ns_wasm_to_u32_index(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                             &delta, "wasm memory.grow delta"))
        return JS_EXCEPTION;
    if (m->shared) {
        uint32_t old_pages =
            (uint32_t)wasm_memory_get_cur_page_count(m->shared);
        if (delta && !wasm_memory_enlarge(m->shared, delta))
            return JS_ThrowRangeError(ctx, "wasm memory.grow failed");
        return JS_NewUint32(ctx, old_pages);
    }
    if (m->staging) {
        uint32_t old_pages = (uint32_t)(m->staging_size / NS_WASM_PAGE_SIZE);
        if (delta) {
            guint64 new_pages = (guint64)old_pages + delta;
            if (m->max_pages && new_pages > m->max_pages)
                return JS_ThrowRangeError(ctx, "wasm memory.grow failed");
            guint64 new_size = new_pages * NS_WASM_PAGE_SIZE;
            if (new_size > NS_WASM_MEMORY_MAX_BYTES)
                return JS_ThrowRangeError(ctx, "wasm memory.grow failed");
            guint8 *grown = ns_js_cage_shared_realloc(m->staging, new_size);
            if (!grown)
                return JS_ThrowRangeError(ctx, "wasm memory.grow failed");
            m->staging = grown;
            memset(m->staging + m->staging_size, 0,
                   new_size - m->staging_size);
            m->staging_size = (size_t)new_size;
            if (!JS_IsUndefined(m->buffer) &&
                JS_RepointArrayBuffer(ctx, m->buffer, m->staging,
                                      m->staging_size) == 0) {
                m->base = m->staging;
                m->size = m->staging_size;
            } else {
                ns_wasm_memory_detach_buffer(m);
            }
        }
        return JS_NewUint32(ctx, old_pages);
    }
    ns_wasm_instance *wi = ns_wasm_instance_opaque(m->instance);
    if (!wi || !wi->inst)
        return JS_ThrowTypeError(ctx, "wasm instance is gone");
    wasm_memory_inst_t mem = wasm_runtime_get_memory(wi->inst, 0);
    if (!mem)
        return JS_ThrowTypeError(ctx, "wasm memory is gone");
    uint32_t old_pages = (uint32_t)wasm_memory_get_cur_page_count(mem);
    if (delta) {
        if (!wasm_memory_enlarge(mem, delta))
            return JS_ThrowRangeError(ctx, "wasm memory.grow failed");
        ns_wasm_memory_detach_buffer(m);
    }
    return JS_NewUint32(ctx, old_pages);
}

static const JSCFunctionListEntry ns_wasm_module_proto_funcs[] = {
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Module",
                       JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry ns_wasm_instance_proto_funcs[] = {
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Instance",
                       JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry ns_wasm_memory_proto_funcs[] = {
    JS_CGETSET_DEF("buffer", ns_wasm_memory_buffer_get, NULL),
    JS_CFUNC_DEF("grow", 1, ns_wasm_memory_grow),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Memory",
                       JS_PROP_CONFIGURABLE),
};

static void
ns_wasm_on_memory_grown(wasm_module_inst_t inst, void *user_data)
{
    (void)user_data;
    ns_wasm_instance *wi = wasm_runtime_get_custom_data(inst);
    if (!wi || JS_IsUndefined(wi->memory_obj))
        return;
    ns_wasm_memory *m = JS_GetOpaque(wi->memory_obj, ns_wasm_memory_class_id);
    if (m && !m->shared)
        ns_wasm_memory_detach_buffer(m);
}

static void
ns_wasm_table_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_table *t = JS_GetOpaque(val, ns_wasm_table_class_id);
    if (!t)
        return;
    JS_FreeValueRT(rt, t->instance);
    g_free(t->name);
    g_free(t);
}

static void
ns_wasm_table_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_wasm_table *t = JS_GetOpaque(val, ns_wasm_table_class_id);
    if (t)
        JS_MarkValue(rt, t->instance, mark_func);
}

static const JSClassDef ns_wasm_table_class = {
    "Table",
    .finalizer = ns_wasm_table_finalizer,
    .gc_mark = ns_wasm_table_gc_mark,
};

static void
ns_wasm_instance_detach_memory(ns_wasm_instance *wi)
{
    if (!wi || JS_IsUndefined(wi->memory_obj))
        return;
    ns_wasm_memory *m = JS_GetOpaque(wi->memory_obj,
                                     ns_wasm_memory_class_id);
    ns_wasm_memory_detach_buffer(m);
}

static ns_wasm_table *
ns_wasm_table_opaque(JSContext *ctx, JSValueConst this_val,
                     ns_wasm_instance **out_wi)
{
    ns_wasm_table *t = JS_GetOpaque2(ctx, this_val, ns_wasm_table_class_id);
    if (!t)
        return NULL;
    ns_wasm_instance *wi = ns_wasm_instance_opaque(t->instance);
    if (!wi || !wi->inst) {
        JS_ThrowTypeError(ctx, "wasm instance is gone");
        return NULL;
    }
    if (!t->name) {
        JS_ThrowTypeError(ctx, "wasm table is gone");
        return NULL;
    }
    *out_wi = wi;
    return t;
}

static JSValue
ns_wasm_table_length_get(JSContext *ctx, JSValueConst this_val)
{
    ns_wasm_instance *wi = NULL;
    ns_wasm_table *t = ns_wasm_table_opaque(ctx, this_val, &wi);
    if (!t)
        return JS_EXCEPTION;
    uint32_t size = 0;
    if (!ns_wamr_table_size(wi->inst, t->name, &size, NULL))
        return JS_ThrowTypeError(ctx, "wasm table is gone");
    return JS_NewUint32(ctx, size);
}

static JSValue
ns_wasm_table_get(JSContext *ctx, JSValueConst this_val, int argc,
                  JSValueConst *argv)
{
    ns_wasm_instance *wi = NULL;
    ns_wasm_table *t = ns_wasm_table_opaque(ctx, this_val, &wi);
    if (!t)
        return JS_EXCEPTION;
    uint32_t idx = 0;
    if (ns_wasm_to_u32_index(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                             &idx, "wasm table index"))
        return JS_EXCEPTION;
    if (t->elem_kind == WASM_FUNCREF) {
        wasm_table_inst_t tbl;
        if (!wasm_runtime_get_export_table_inst(wi->inst, t->name, &tbl))
            return JS_ThrowTypeError(ctx, "wasm table is gone");
        if (idx >= tbl.cur_size)
            return JS_ThrowRangeError(ctx, "table index out of bounds");
        wasm_function_inst_t func =
            wasm_table_get_func_inst(wi->inst, &tbl, idx);
        if (!func)
            return JS_NULL;
        return ns_wasm_make_func(ctx, t->instance, func);
    }
    uint32_t ref = NS_WAMR_NULL_REF;
    if (!ns_wamr_table_get_ref(wi->inst, t->name, idx, &ref))
        return JS_ThrowRangeError(ctx, "table index out of bounds");
    if (ref == NS_WAMR_NULL_REF)
        return JS_NULL;
    void *obj = NULL;
    if (!wasm_externref_ref2obj(ref, &obj))
        return JS_NULL;
    return ns_wasm_ref_to_js(ctx, obj);
}

static JSValue
ns_wasm_table_set(JSContext *ctx, JSValueConst this_val, int argc,
                  JSValueConst *argv)
{
    ns_wasm_instance *wi = NULL;
    ns_wasm_table *t = ns_wasm_table_opaque(ctx, this_val, &wi);
    if (!t)
        return JS_EXCEPTION;
    uint32_t idx = 0;
    if (ns_wasm_to_u32_index(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                             &idx, "wasm table index"))
        return JS_EXCEPTION;
    JSValueConst v = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (t->elem_kind == WASM_FUNCREF) {
        if (!JS_IsNull(v))
            return JS_ThrowTypeError(ctx,
                                     "setting funcref table entries from JS "
                                     "is not supported");
        if (!ns_wamr_table_set_ref(wi->inst, t->name, idx, NS_WAMR_NULL_REF))
            return JS_ThrowRangeError(ctx, "table index out of bounds");
        return JS_UNDEFINED;
    }
    uint32_t ref = NS_WAMR_NULL_REF;
    if (!JS_IsNull(v)) {
        uint32_t size = 0;
        if (!ns_wamr_table_size(wi->inst, t->name, &size, NULL))
            return JS_ThrowTypeError(ctx, "wasm table is gone");
        if (idx >= size)
            return JS_ThrowRangeError(ctx, "table index out of bounds");
        ns_wasm_ref *r = ns_wasm_ref_box(ctx, v);
        if (!ns_wasm_ref_register(wi->inst, r, &ref))
            return JS_ThrowRangeError(ctx,
                                         "wasm externref registration failed");
    }
    if (!ns_wamr_table_set_ref(wi->inst, t->name, idx, ref))
        return JS_ThrowRangeError(ctx, "table index out of bounds");
    return JS_UNDEFINED;
}

static JSValue
ns_wasm_table_grow(JSContext *ctx, JSValueConst this_val, int argc,
                   JSValueConst *argv)
{
    ns_wasm_instance *wi = NULL;
    ns_wasm_table *t = ns_wasm_table_opaque(ctx, this_val, &wi);
    if (!t)
        return JS_EXCEPTION;
    uint32_t delta = 0;
    if (ns_wasm_to_u32_index(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                             &delta, "wasm table.grow delta"))
        return JS_EXCEPTION;
    uint32_t ref = NS_WAMR_NULL_REF;
    gboolean fill_externref = FALSE;
    if (argc > 1) {
        if (t->elem_kind == WASM_FUNCREF && !JS_IsNull(argv[1]))
            return JS_ThrowTypeError(ctx,
                                     "growing funcref table entries from JS "
                                     "is not supported");
        if (t->elem_kind == WASM_EXTERNREF && !JS_IsNull(argv[1])) {
            ns_wasm_ref *r = ns_wasm_ref_box(ctx, argv[1]);
            if (!ns_wasm_ref_register(wi->inst, r, &ref))
                return JS_ThrowRangeError(
                    ctx, "wasm externref registration failed");
            fill_externref = TRUE;
        }
    }
    uint32_t old_size = 0;
    if (!ns_wamr_table_grow(wi->inst, t->name, delta, &old_size)) {
        if (fill_externref && wi->call_depth == 0)
            ns_wamr_externref_reclaim(wi->inst);
        return JS_ThrowRangeError(ctx, "wasm table.grow failed");
    }
    if (fill_externref) {
        for (uint32_t i = 0; i < delta; i++) {
            if (!ns_wamr_table_set_ref(wi->inst, t->name, old_size + i, ref)) {
                if (wi->call_depth == 0)
                    ns_wamr_externref_reclaim(wi->inst);
                return JS_ThrowRangeError(ctx, "table index out of bounds");
            }
        }
    }
    return JS_NewUint32(ctx, old_size);
}

static const JSCFunctionListEntry ns_wasm_table_proto_funcs[] = {
    JS_CGETSET_DEF("length", ns_wasm_table_length_get, NULL),
    JS_CFUNC_DEF("get", 1, ns_wasm_table_get),
    JS_CFUNC_DEF("set", 2, ns_wasm_table_set),
    JS_CFUNC_DEF("grow", 1, ns_wasm_table_grow),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Table",
                       JS_PROP_CONFIGURABLE),
};

static void
ns_wasm_global_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_global *g = JS_GetOpaque(val, ns_wasm_global_class_id);
    if (!g)
        return;
    JS_FreeValueRT(rt, g->instance);
    JS_FreeValueRT(rt, g->ref_value);
    g_free(g->name);
    g_free(g);
}

static void
ns_wasm_global_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_wasm_global *g = JS_GetOpaque(val, ns_wasm_global_class_id);
    if (!g)
        return;
    JS_MarkValue(rt, g->instance, mark_func);
    JS_MarkValue(rt, g->ref_value, mark_func);
}

static const JSClassDef ns_wasm_global_class = {
    "Global",
    .finalizer = ns_wasm_global_finalizer,
    .gc_mark = ns_wasm_global_gc_mark,
};

static gboolean
ns_wasm_global_resolve(JSContext *ctx, JSValueConst this_val,
                       wasm_global_inst_t *out)
{
    ns_wasm_global *g = JS_GetOpaque2(ctx, this_val, ns_wasm_global_class_id);
    if (!g) {
        JS_ThrowTypeError(ctx, "not a WebAssembly.Global");
        return FALSE;
    }
    ns_wasm_instance *wi = ns_wasm_instance_opaque(g->instance);
    if (!wi || !wi->inst) {
        JS_ThrowTypeError(ctx, "wasm instance is gone");
        return FALSE;
    }
    if (!wasm_runtime_get_export_global_inst(wi->inst, g->name, out) ||
        !out->global_data) {
        JS_ThrowTypeError(ctx, "wasm global is gone");
        return FALSE;
    }
    return TRUE;
}

static int
ns_wasm_global_convert(JSContext *ctx, wasm_valkind_t kind, JSValueConst v,
                       wasm_val_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (JS_IsUndefined(v))
        return 0;
    switch (kind) {
    case WASM_I32:
        return JS_ToInt32(ctx, &out->of.i32, v) ? -1 : 0;
    case WASM_I64:
        return JS_ToBigInt64(ctx, &out->of.i64, v) ? -1 : 0;
    case WASM_F32: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        out->of.f32 = (float)d;
        return 0;
    }
    case WASM_F64:
        return JS_ToFloat64(ctx, &out->of.f64, v) ? -1 : 0;
    default:
        JS_ThrowTypeError(ctx, "unsupported wasm global type");
        return -1;
    }
}

static JSValue
ns_wasm_global_standalone_get(JSContext *ctx, const ns_wasm_global *g)
{
    if (g->kind == WASM_EXTERNREF || g->kind == WASM_FUNCREF)
        return JS_DupValue(ctx, g->ref_value);
    return ns_wasm_val_to_js(ctx, &g->val);
}

static JSValue
ns_wasm_global_value_get(JSContext *ctx, JSValueConst this_val)
{
    ns_wasm_global *sg = JS_GetOpaque(this_val, ns_wasm_global_class_id);
    if (sg && sg->standalone)
        return ns_wasm_global_standalone_get(ctx, sg);
    wasm_global_inst_t gi;
    if (!ns_wasm_global_resolve(ctx, this_val, &gi))
        return JS_EXCEPTION;
    switch (gi.kind) {
    case WASM_I32:
        return JS_NewInt32(ctx, *(int32_t *)gi.global_data);
    case WASM_I64:
        return JS_NewBigInt64(ctx, *(int64_t *)gi.global_data);
    case WASM_F32:
        return JS_NewFloat64(ctx, *(float *)gi.global_data);
    case WASM_F64:
        return JS_NewFloat64(ctx, *(double *)gi.global_data);
    default:
        return JS_ThrowTypeError(ctx, "unsupported wasm global type");
    }
}

static JSValue
ns_wasm_global_value_set(JSContext *ctx, JSValueConst this_val,
                         JSValueConst val)
{
    ns_wasm_global *sg = JS_GetOpaque(this_val, ns_wasm_global_class_id);
    if (sg && sg->standalone) {
        if (!sg->is_mutable)
            return JS_ThrowTypeError(ctx, "WebAssembly.Global is immutable");
        if (sg->kind == WASM_EXTERNREF || sg->kind == WASM_FUNCREF) {
            JS_FreeValue(ctx, sg->ref_value);
            sg->ref_value = JS_DupValue(ctx, val);
            return JS_UNDEFINED;
        }
        wasm_val_t next;
        if (ns_wasm_global_convert(ctx, sg->kind, val, &next))
            return JS_EXCEPTION;
        sg->val = next;
        return JS_UNDEFINED;
    }
    wasm_global_inst_t gi;
    if (!ns_wasm_global_resolve(ctx, this_val, &gi))
        return JS_EXCEPTION;
    if (!gi.is_mutable)
        return JS_ThrowTypeError(ctx, "WebAssembly.Global is immutable");
    switch (gi.kind) {
    case WASM_I32: {
        int32_t i = 0;
        if (JS_ToInt32(ctx, &i, val))
            return JS_EXCEPTION;
        *(int32_t *)gi.global_data = i;
        break;
    }
    case WASM_I64: {
        int64_t i = 0;
        if (JS_ToBigInt64(ctx, &i, val))
            return JS_EXCEPTION;
        *(int64_t *)gi.global_data = i;
        break;
    }
    case WASM_F32: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, val))
            return JS_EXCEPTION;
        *(float *)gi.global_data = (float)d;
        break;
    }
    case WASM_F64: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, val))
            return JS_EXCEPTION;
        *(double *)gi.global_data = d;
        break;
    }
    default:
        return JS_ThrowTypeError(ctx, "unsupported wasm global type");
    }
    return JS_UNDEFINED;
}

static JSValue
ns_wasm_global_value_of(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    return ns_wasm_global_value_get(ctx, this_val);
}

static const JSCFunctionListEntry ns_wasm_global_proto_funcs[] = {
    JS_CGETSET_DEF("value", ns_wasm_global_value_get,
                   ns_wasm_global_value_set),
    JS_CFUNC_DEF("valueOf", 0, ns_wasm_global_value_of),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Global",
                       JS_PROP_CONFIGURABLE),
};

static void
ns_wasm_module_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_module *m = JS_GetOpaque(val, ns_wasm_module_class_id);
    if (!m || !g_atomic_int_dec_and_test(&m->refs))
        return;
    if (m->module)
        wasm_runtime_unload(m->module);
    ns_wasm_linkage_clear(rt, &m->linkage);
    g_free(m->load_bytes);
    g_free(m);
}

static void
ns_wasm_module_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    (void)rt;
    (void)val;
    (void)mark_func;
}

static const JSClassDef ns_wasm_module_class = {
    "Module",
    .finalizer = ns_wasm_module_finalizer,
    .gc_mark = ns_wasm_module_gc_mark,
};

static void
ns_wasm_instance_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_instance *wi = JS_GetOpaque(val, ns_wasm_instance_class_id);
    if (!wi)
        return;
    ns_wasm_instance_detach_memory(wi);
    if (wi->exec_env)
        wasm_runtime_destroy_exec_env(wi->exec_env);
    if (wi->inst) {
        wasm_runtime_set_custom_data(wi->inst, NULL);
        wasm_runtime_deinstantiate(wi->inst);
    }
    ns_wasm_linkage_clear(rt, &wi->linkage);
    if (wi->funcs) {
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, wi->funcs);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            JS_FreeValueRT(rt, *(JSValue *)value);
            g_free(value);
        }
        g_hash_table_destroy(wi->funcs);
    }
    JS_FreeValueRT(rt, wi->module_obj);
    JS_FreeValueRT(rt, wi->exports);
    JS_FreeValueRT(rt, wi->memory_obj);
    JS_FreeValueRT(rt, wi->pending_exc);
    ns_wasm_eh_free(rt, wi);
    g_free(wi);
}

static void
ns_wasm_instance_gc_mark(JSRuntime *rt, JSValueConst val,
                         JS_MarkFunc *mark_func)
{
    ns_wasm_instance *wi = JS_GetOpaque(val, ns_wasm_instance_class_id);
    if (!wi)
        return;
    JS_MarkValue(rt, wi->module_obj, mark_func);
    JS_MarkValue(rt, wi->exports, mark_func);
    JS_MarkValue(rt, wi->memory_obj, mark_func);
    JS_MarkValue(rt, wi->pending_exc, mark_func);
    ns_wasm_eh_mark(rt, wi, mark_func);
    ns_wasm_linkage_gc_mark(rt, &wi->linkage, mark_func);
    if (wi->funcs) {
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, wi->funcs);
        while (g_hash_table_iter_next(&it, &key, &value))
            JS_MarkValue(rt, *(JSValue *)value, mark_func);
    }
}

static const JSClassDef ns_wasm_instance_class = {
    "Instance",
    .finalizer = ns_wasm_instance_finalizer,
    .gc_mark = ns_wasm_instance_gc_mark,
};

static void
ns_wasm_native_fail(JSContext *ctx, ns_wasm_instance *wi,
                    wasm_module_inst_t inst, const char *msg)
{
    if (!JS_HasException(ctx))
        ns_wasm_throw_named(ctx, "RuntimeError", msg ? msg : "wasm import failed");
    JSValue exc = JS_GetException(ctx);
    if (wi && !wi->has_pending) {
        wi->pending_exc = exc;
        wi->has_pending = TRUE;
    } else {
        JS_FreeValue(ctx, exc);
    }
    if (inst)
        wasm_runtime_set_exception(inst, msg ? msg : "wasm import failed");
}

static void
ns_wasm_native_dispatch(wasm_exec_env_t exec_env, uint64_t *args)
{
    ns_wasm_binding *desc = wasm_runtime_get_function_attachment(exec_env);
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    ns_wasm_instance *wi = inst ? wasm_runtime_get_custom_data(inst) : NULL;
    if (!wi)
        wi = g_private_get(&ns_wasm_instantiating);
    ns_wasm_binding *b = NULL;
    if (wi && desc && wi->linkage.bindings &&
        desc->index < wi->linkage.bindings->len)
        b = g_ptr_array_index(wi->linkage.bindings, desc->index);
    if (!b || !b->ctx) {
        if (inst)
            wasm_runtime_set_exception(inst, "wasm import binding is gone");
        return;
    }
    JSContext *ctx = b->ctx;

    JSValue argv[NS_WASM_MAX_PARAMS];
    guint argc = b->param_count;
    for (guint i = 0; i < argc; i++) {
        uint64_t *slot = &args[i];
        switch (b->param_kinds[i]) {
        case WASM_I32:
            argv[i] = JS_NewInt32(ctx, *(int32_t *)slot);
            break;
        case WASM_I64:
            argv[i] = JS_NewBigInt64(ctx, *(int64_t *)slot);
            break;
        case WASM_F32:
            argv[i] = JS_NewFloat64(ctx, *(float *)slot);
            break;
        case WASM_F64:
            argv[i] = JS_NewFloat64(ctx, *(double *)slot);
            break;
        case WASM_EXTERNREF:
            argv[i] = ns_wasm_ref_to_js(ctx, *(void **)slot);
            break;
        default:
            argv[i] = JS_UNDEFINED;
            break;
        }
        if (JS_IsException(argv[i])) {
            for (guint j = 0; j < i; j++)
                JS_FreeValue(ctx, argv[j]);
            ns_wasm_native_fail(ctx, wi, inst,
                                "wasm import argument conversion failed");
            return;
        }
    }

    if (ns_wasm_entered_us)
        ns_js_credit_wasm_time(ctx,
                               g_get_monotonic_time() - ns_wasm_entered_us);
    JSValue ret = JS_Call(ctx, b->fn, JS_UNDEFINED, (int)argc, argv);
    ns_wasm_entered_us = g_get_monotonic_time();
    for (guint i = 0; i < argc; i++)
        JS_FreeValue(ctx, argv[i]);

    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        if (ns_wasm_eh_raise(ctx, wi, inst, JS_DupValue(ctx, exc))) {
            JS_FreeValue(ctx, exc);
            return;
        }
        if (wi && !wi->has_pending) {
            wi->pending_exc = exc;
            wi->has_pending = TRUE;
        } else {
            JS_FreeValue(ctx, exc);
        }
        if (inst)
            wasm_runtime_set_exception(inst, "uncaught JavaScript exception");
        return;
    }

    if (b->has_result) {
        switch (b->result_kind) {
        case WASM_I32: {
            int32_t i = 0;
            if (JS_ToInt32(ctx, &i, ret)) {
                ns_wasm_native_fail(ctx, wi, inst,
                                    "wasm import result conversion failed");
                JS_FreeValue(ctx, ret);
                return;
            }
            *(int32_t *)args = i;
            break;
        }
        case WASM_I64: {
            int64_t i = 0;
            if (JS_ToBigInt64(ctx, &i, ret)) {
                ns_wasm_native_fail(ctx, wi, inst,
                                    "wasm import result conversion failed");
                JS_FreeValue(ctx, ret);
                return;
            }
            *(int64_t *)args = i;
            break;
        }
        case WASM_F32:
        case WASM_F64: {
            double d = 0;
            if (JS_ToFloat64(ctx, &d, ret)) {
                ns_wasm_native_fail(ctx, wi, inst,
                                    "wasm import result conversion failed");
                JS_FreeValue(ctx, ret);
                return;
            }
            if (b->result_kind == WASM_F32)
                *(float *)args = (float)d;
            else
                *(double *)args = d;
            break;
        }
        case WASM_EXTERNREF: {
            ns_wasm_ref *r = ns_wasm_ref_box(ctx, ret);
            if (ns_wasm_ref_register(inst, r, NULL))
                *(void **)args = r;
            else {
                ns_wasm_native_fail(ctx, wi, inst,
                                    "wasm externref registration failed");
                JS_FreeValue(ctx, ret);
                return;
            }
            break;
        }
        default:
            break;
        }
    }
    JS_FreeValue(ctx, ret);
}

static char
ns_wasm_signature_char(wasm_valkind_t kind)
{
    switch (kind) {
    case WASM_I32:
        return 'i';
    case WASM_I64:
        return 'I';
    case WASM_F32:
        return 'f';
    case WASM_F64:
        return 'F';
    case WASM_EXTERNREF:
        return 'r';
    case WASM_FUNCREF:
        return 'i';
    default:
        return 'i';
    }
}

static gboolean
ns_wasm_kind_supported(wasm_valkind_t kind)
{
    return kind == WASM_I32 || kind == WASM_I64 || kind == WASM_F32 ||
           kind == WASM_F64 || kind == WASM_EXTERNREF;
}

static gboolean
ns_wasm_check_import_func_type(JSContext *ctx, wasm_func_type_t type)
{
    guint param_count = wasm_func_type_get_param_count(type);
    guint n_results = wasm_func_type_get_result_count(type);
    if (param_count > NS_WASM_MAX_PARAMS || n_results > 1) {
        ns_wasm_throw_named(ctx, "LinkError",
                            "unsupported wasm import function arity");
        return FALSE;
    }
    for (guint p = 0; p < param_count; p++) {
        wasm_valkind_t kind = wasm_func_type_get_param_valkind(type, p);
        if (!ns_wasm_kind_supported(kind)) {
            ns_wasm_throw_named(ctx, "LinkError",
                                "unsupported wasm import parameter type");
            return FALSE;
        }
    }
    if (n_results > 0 &&
        !ns_wasm_kind_supported(wasm_func_type_get_result_valkind(type, 0))) {
        ns_wasm_throw_named(ctx, "LinkError",
                            "unsupported wasm import result type");
        return FALSE;
    }
    return TRUE;
}

static gboolean
ns_wasm_global_current_value(JSValueConst v, wasm_val_t *out)
{
    ns_wasm_global *g = JS_GetOpaque(v, ns_wasm_global_class_id);
    if (!g)
        return FALSE;
    if (g->standalone) {
        if (g->kind == WASM_EXTERNREF || g->kind == WASM_FUNCREF)
            return FALSE;
        *out = g->val;
        return TRUE;
    }
    ns_wasm_instance *wi = ns_wasm_instance_opaque(g->instance);
    if (!wi || !wi->inst)
        return FALSE;
    wasm_global_inst_t gi;
    if (!wasm_runtime_get_export_global_inst(wi->inst, g->name, &gi) ||
        !gi.global_data)
        return FALSE;
    memset(out, 0, sizeof(*out));
    out->kind = gi.kind;
    switch (gi.kind) {
    case WASM_I32:
        out->of.i32 = *(int32_t *)gi.global_data;
        return TRUE;
    case WASM_I64:
        out->of.i64 = *(int64_t *)gi.global_data;
        return TRUE;
    case WASM_F32:
        out->of.f32 = *(float *)gi.global_data;
        return TRUE;
    case WASM_F64:
        out->of.f64 = *(double *)gi.global_data;
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean
ns_wasm_bind_imports(JSContext *ctx, wasm_module_t module,
                     ns_wasm_linkage *linkage, JSValueConst import_object)
{
    int32_t n = wasm_runtime_get_import_count(module);
    g_autofree wasm_val_t *global_vals = g_new0(wasm_val_t, n > 0 ? n : 1);
    g_autofree gboolean *global_set = g_new0(gboolean, n > 0 ? n : 1);
    for (int32_t i = 0; i < n; i++) {
        wasm_import_t imp;
        wasm_runtime_get_import_type(module, i, &imp);
        if (!JS_IsObject(import_object)) {
            ns_wasm_throw_named(ctx, "LinkError", "import object required");
            return FALSE;
        }
        if (imp.kind != WASM_IMPORT_EXPORT_KIND_FUNC) {
            JSValue ns = JS_GetPropertyStr(ctx, import_object,
                                           imp.module_name);
            JSValue v = JS_IsObject(ns) ? JS_GetPropertyStr(ctx, ns, imp.name)
                                        : JS_UNDEFINED;
            JS_FreeValue(ctx, ns);
            switch (imp.kind) {
            case WASM_IMPORT_EXPORT_KIND_MEMORY: {
                ns_wasm_memory *pm = JS_GetOpaque(v, ns_wasm_memory_class_id);
                gboolean want_shared =
                    wasm_memory_type_get_shared(imp.u.memory_type);
                if (want_shared != (pm && pm->shared)) {
                    JS_FreeValue(ctx, v);
                    ns_wasm_throw_named(ctx, "LinkError",
                        want_shared ? "imported memory must be a shared "
                                      "WebAssembly.Memory"
                                    : "imported memory must not be shared");
                    return FALSE;
                }
                if (pm && pm->shared &&
                    (wasm_memory_get_cur_page_count(pm->shared) <
                         wasm_memory_type_get_init_page_count(
                             imp.u.memory_type) ||
                     ns_wamr_memory_max_pages(pm->shared) >
                         wasm_memory_type_get_max_page_count(
                             imp.u.memory_type))) {
                    JS_FreeValue(ctx, v);
                    ns_wasm_throw_named(ctx, "LinkError",
                        "imported shared memory has incompatible limits");
                    return FALSE;
                }
                if (pm && JS_IsUndefined(linkage->provided_memory))
                    linkage->provided_memory = JS_DupValue(ctx, v);
                break;
            }
            case WASM_IMPORT_EXPORT_KIND_TABLE:
                if (JS_GetOpaque(v, ns_wasm_table_class_id) &&
                    JS_IsUndefined(linkage->provided_table))
                    linkage->provided_table = JS_DupValue(ctx, v);
                break;
            case WASM_IMPORT_EXPORT_KIND_GLOBAL: {
                wasm_valkind_t want =
                    wasm_global_type_get_valkind(imp.u.global_type);
                wasm_val_t val;
                memset(&val, 0, sizeof(val));
                gboolean ok = FALSE;
                if (ns_wasm_global_current_value(v, &val)) {
                    ok = val.kind == want;
                } else if (want == WASM_I64 && JS_IsBigInt(v)) {
                    val.kind = want;
                    ok = !JS_ToBigInt64(ctx, &val.of.i64, v);
                } else if (want != WASM_I64 && JS_IsNumber(v)) {
                    double d = 0;
                    val.kind = want;
                    switch (want) {
                    case WASM_I32:
                        ok = !JS_ToInt32(ctx, &val.of.i32, v);
                        break;
                    case WASM_F32:
                        ok = !JS_ToFloat64(ctx, &d, v);
                        val.of.f32 = (float)d;
                        break;
                    case WASM_F64:
                        ok = !JS_ToFloat64(ctx, &val.of.f64, v);
                        break;
                    default:
                        break;
                    }
                }
                if (!ok) {
                    JS_FreeValue(ctx, v);
                    if (!JS_HasException(ctx)) {
                        char *msg = g_strdup_printf(
                            "global import %s.%s is missing or has the wrong type",
                            imp.module_name, imp.name);
                        ns_wasm_throw_named(ctx, "LinkError", msg);
                        g_free(msg);
                    }
                    return FALSE;
                }
                global_vals[i] = val;
                global_set[i] = TRUE;
                break;
            }
            default:
                break;
            }
            JS_FreeValue(ctx, v);
            continue;
        }
        JSValue ns = JS_GetPropertyStr(ctx, import_object, imp.module_name);
        JSValue fn = JS_IsObject(ns) ? JS_GetPropertyStr(ctx, ns, imp.name)
                                     : JS_UNDEFINED;
        JS_FreeValue(ctx, ns);
        if (!JS_IsFunction(ctx, fn)) {
            JS_FreeValue(ctx, fn);
            char *msg = g_strdup_printf("import %s.%s is not a function",
                                        imp.module_name, imp.name);
            ns_wasm_throw_named(ctx, "LinkError", msg);
            g_free(msg);
            return FALSE;
        }

        if (!ns_wasm_check_import_func_type(ctx, imp.u.func_type)) {
            JS_FreeValue(ctx, fn);
            return FALSE;
        }

        ns_wasm_binding *b = g_new0(ns_wasm_binding, 1);
        b->ctx = ctx;
        ns_js_pin_realm(ctx);
        b->fn = fn;
        b->index = linkage->bindings->len;
        b->param_count = wasm_func_type_get_param_count(imp.u.func_type);
        for (guint p = 0; p < b->param_count; p++)
            b->param_kinds[p] =
                wasm_func_type_get_param_valkind(imp.u.func_type, p);
        b->has_result = wasm_func_type_get_result_count(imp.u.func_type) > 0;
        if (b->has_result)
            b->result_kind =
                wasm_func_type_get_result_valkind(imp.u.func_type, 0);
        g_ptr_array_add(linkage->bindings, b);
    }
    for (int32_t i = 0; i < n; i++)
        if (global_set[i])
            wasm_runtime_link_import_global(module, i, &global_vals[i]);
    return TRUE;
}

static gboolean
ns_wasm_link_import_symbols(JSContext *ctx, ns_wasm_module *mod)
{
    if (mod->imports_linked)
        return TRUE;

    int32_t n = wasm_runtime_get_import_count(mod->module);
    ns_wasm_linkage symbols;
    ns_wasm_linkage_init(&symbols);
    GHashTable *groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify)g_array_unref);

    for (int32_t i = 0; i < n; i++) {
        wasm_import_t imp;
        wasm_runtime_get_import_type(mod->module, i, &imp);
        if (imp.kind != WASM_IMPORT_EXPORT_KIND_FUNC)
            continue;
        if (!ns_wasm_check_import_func_type(ctx, imp.u.func_type)) {
            g_hash_table_destroy(groups);
            ns_wasm_linkage_clear(JS_GetRuntime(ctx), &symbols);
            return FALSE;
        }

        ns_wasm_binding *b = g_new0(ns_wasm_binding, 1);
        b->fn = JS_UNDEFINED;
        b->index = symbols.bindings->len;
        b->param_count = wasm_func_type_get_param_count(imp.u.func_type);
        for (guint p = 0; p < b->param_count; p++)
            b->param_kinds[p] =
                wasm_func_type_get_param_valkind(imp.u.func_type, p);
        b->has_result = wasm_func_type_get_result_count(imp.u.func_type) > 0;
        if (b->has_result)
            b->result_kind =
                wasm_func_type_get_result_valkind(imp.u.func_type, 0);
        g_ptr_array_add(symbols.bindings, b);

        GString *sig = g_string_new("(");
        for (guint p = 0; p < b->param_count; p++)
            g_string_append_c(sig, ns_wasm_signature_char(b->param_kinds[p]));
        g_string_append_c(sig, ')');
        if (b->has_result)
            g_string_append_c(sig, ns_wasm_signature_char(b->result_kind));
        char *sig_str = g_string_free(sig, FALSE);
        g_ptr_array_add(symbols.strings, sig_str);
        char *name_str = g_strdup(imp.name);
        g_ptr_array_add(symbols.strings, name_str);

        GArray *group = g_hash_table_lookup(groups, imp.module_name);
        if (!group) {
            group = g_array_new(FALSE, TRUE, sizeof(NativeSymbol));
            g_hash_table_insert(groups, g_strdup(imp.module_name), group);
        }
        NativeSymbol sym = {
            .symbol = name_str,
            .func_ptr = (void *)ns_wasm_native_dispatch,
            .signature = sig_str,
            .attachment = b,
        };
        g_array_append_val(group, sym);
    }

    GHashTableIter it;
    gpointer key, value;
    GPtrArray *reg_names = g_ptr_array_new();
    GPtrArray *reg_blocks = g_ptr_array_new();
    g_hash_table_iter_init(&it, groups);

    g_mutex_lock(&ns_wasm_link_mutex);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        GArray *group = value;
        NativeSymbol *syms =
            g_memdup2(group->data, group->len * sizeof(NativeSymbol));
        g_ptr_array_add(symbols.symbol_blocks, syms);
        char *reg_name = g_strdup(key);
        g_ptr_array_add(symbols.strings, reg_name);
        wasm_runtime_register_natives_raw(reg_name, syms, group->len);
        g_ptr_array_add(reg_names, reg_name);
        g_ptr_array_add(reg_blocks, syms);
    }

    gboolean ok = wasm_runtime_resolve_symbols(mod->module);

    for (guint i = 0; i < reg_names->len; i++)
        wasm_runtime_unregister_natives(g_ptr_array_index(reg_names, i),
                                        g_ptr_array_index(reg_blocks, i));
    g_mutex_unlock(&ns_wasm_link_mutex);

    g_ptr_array_free(reg_names, TRUE);
    g_ptr_array_free(reg_blocks, TRUE);
    g_hash_table_destroy(groups);

    if (!ok) {
        GString *msg = g_string_new("unresolved wasm imports:");
        for (int32_t i = 0; i < n; i++) {
            wasm_import_t imp;
            wasm_runtime_get_import_type(mod->module, i, &imp);
            if (imp.kind == WASM_IMPORT_EXPORT_KIND_FUNC && !imp.linked)
                g_string_append_printf(msg, " %s.%s", imp.module_name,
                                       imp.name);
        }
        ns_wasm_throw_named(ctx, "LinkError", msg->str);
        g_string_free(msg, TRUE);
        ns_wasm_linkage_clear(JS_GetRuntime(ctx), &symbols);
        return FALSE;
    }
    ns_wasm_linkage_clear(JS_GetRuntime(ctx), &mod->linkage);
    mod->linkage = symbols;
    mod->imports_linked = TRUE;
    return TRUE;
}

static JSValue
ns_wasm_build_exports(JSContext *ctx, JSValueConst instance_obj,
                      ns_wasm_instance *wi, ns_wasm_module *mod)
{
    JSValue exports = JS_NewObject(ctx);
    if (JS_IsException(exports))
        return exports;
    int32_t n = wasm_runtime_get_export_count(mod->module);
    for (int32_t i = 0; i < n; i++) {
        wasm_export_t exp;
        wasm_runtime_get_export_type(mod->module, i, &exp);
        switch (exp.kind) {
        case WASM_IMPORT_EXPORT_KIND_FUNC: {
            wasm_function_inst_t func =
                wasm_runtime_lookup_function(wi->inst, exp.name);
            if (func) {
                JSValue fn = ns_wasm_make_func(ctx, instance_obj, func);
                if (JS_IsException(fn)) {
                    JS_FreeValue(ctx, exports);
                    return fn;
                }
                JS_DefinePropertyValueStr(ctx, fn, "name",
                                          JS_NewString(ctx, exp.name),
                                          JS_PROP_CONFIGURABLE);
                if (JS_SetPropertyStr(ctx, exports, exp.name, fn) < 0) {
                    JS_FreeValue(ctx, exports);
                    return JS_EXCEPTION;
                }
            }
            break;
        }
        case WASM_IMPORT_EXPORT_KIND_MEMORY: {
            JSValue mem_obj;
            if (!JS_IsUndefined(wi->memory_obj)) {
                mem_obj = JS_DupValue(ctx, wi->memory_obj);
            } else {
                mem_obj = JS_NewObjectClass(ctx, ns_wasm_memory_class_id);
                if (JS_IsException(mem_obj)) {
                    JS_FreeValue(ctx, exports);
                    return mem_obj;
                }
                ns_wasm_memory *m = g_new0(ns_wasm_memory, 1);
                m->ctx = ctx;
                ns_js_pin_realm(ctx);
                m->instance = JS_DupValue(ctx, instance_obj);
                m->buffer = JS_UNDEFINED;
                wasm_memory_inst_t own = wasm_runtime_get_memory(wi->inst, 0);
                if (ns_wamr_memory_is_shared(own)) {
                    ns_wamr_shared_memory_dup(own);
                    m->shared = own;
                    m->max_pages = ns_wamr_memory_max_pages(own);
                    ns_wasm_shared_register(own);
                }
                JS_SetOpaque(mem_obj, m);
                wi->memory_obj = JS_DupValue(ctx, mem_obj);
            }
            if (JS_SetPropertyStr(ctx, exports, exp.name, mem_obj) < 0) {
                JS_FreeValue(ctx, exports);
                return JS_EXCEPTION;
            }
            break;
        }
        case WASM_IMPORT_EXPORT_KIND_TABLE: {
            JSValue tbl_obj = JS_NewObjectClass(ctx, ns_wasm_table_class_id);
            if (JS_IsException(tbl_obj)) {
                JS_FreeValue(ctx, exports);
                return tbl_obj;
            }
            ns_wasm_table *t = g_new0(ns_wasm_table, 1);
            t->ctx = ctx;
            ns_js_pin_realm(ctx);
            t->instance = JS_DupValue(ctx, instance_obj);
            t->name = g_strdup(exp.name);
            t->elem_kind = wasm_table_type_get_elem_kind(exp.u.table_type);
            JS_SetOpaque(tbl_obj, t);
            if (JS_SetPropertyStr(ctx, exports, exp.name, tbl_obj) < 0) {
                JS_FreeValue(ctx, exports);
                return JS_EXCEPTION;
            }
            break;
        }
        case WASM_IMPORT_EXPORT_KIND_GLOBAL: {
            wasm_global_inst_t gi;
            if (!wasm_runtime_get_export_global_inst(wi->inst, exp.name, &gi))
                break;
            JSValue g_obj = JS_NewObjectClass(ctx, ns_wasm_global_class_id);
            if (JS_IsException(g_obj)) {
                JS_FreeValue(ctx, exports);
                return g_obj;
            }
            ns_wasm_global *g = g_new0(ns_wasm_global, 1);
            g->ctx = ctx;
            ns_js_pin_realm(ctx);
            g->instance = JS_DupValue(ctx, instance_obj);
            g->name = g_strdup(exp.name);
            g->ref_value = JS_UNDEFINED;
            JS_SetOpaque(g_obj, g);
            if (JS_SetPropertyStr(ctx, exports, exp.name, g_obj) < 0) {
                JS_FreeValue(ctx, exports);
                return JS_EXCEPTION;
            }
            break;
        }
        default:
            break;
        }
    }
    return exports;
}

static JSValue
ns_wasm_module_from_bytes(JSContext *ctx, const guint8 *bytes, size_t len)
{
    if (!ns_wasm_runtime_ready())
        return JS_ThrowRangeError(ctx, "wasm runtime init failed");
    if (!ns_wasm_compile_allowed(ctx))
        return ns_wasm_throw_named(ctx, "CompileError",
                                   "Content Security Policy refused to compile "
                                   "WebAssembly ('wasm-unsafe-eval' is not "
                                   "allowed)");
    if (len > G_MAXUINT32)
        return ns_wasm_throw_named(ctx, "CompileError",
                                   "wasm module too large");

    ns_wasm_module *mod = g_new0(ns_wasm_module, 1);
    mod->refs = 1;
    ns_wasm_linkage_init(&mod->linkage);
    mod->load_bytes = ns_wasm_memdup_bytes(bytes, len);
    mod->len = len;

    char error_buf[192] = "";
    LoadArgs load_args;
    memset(&load_args, 0, sizeof(load_args));
    load_args.name = (char *)"";
    load_args.no_resolve = true;
    mod->module = wasm_runtime_load_ex(mod->load_bytes, (uint32_t)len,
                                       &load_args, error_buf,
                                       sizeof(error_buf));
    JSValue obj = mod->module
                      ? JS_NewObjectClass(ctx, ns_wasm_module_class_id)
                      : JS_EXCEPTION;
    if (JS_IsException(obj)) {
        if (mod->module)
            wasm_runtime_unload(mod->module);
        ns_wasm_linkage_clear(JS_GetRuntime(ctx), &mod->linkage);
        g_free(mod->load_bytes);
        g_free(mod);
        if (!JS_HasException(ctx))
            return ns_wasm_throw_named(ctx, "CompileError", error_buf);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, mod);
    return obj;
}

static int
ns_wasm_segment_range_cmp(gconstpointer a, gconstpointer b)
{
    const guint64 *ra = a, *rb = b;
    if (ra[0] < rb[0]) return -1;
    if (ra[0] > rb[0]) return 1;
    return 0;
}

static gboolean
ns_wasm_copy_staging_around_segments(ns_wasm_instance *wi, guint8 *base,
                                     size_t size, const guint8 *staging,
                                     size_t staging_size)
{
    wasm_module_t module = wasm_runtime_get_module(wi->inst);
    int32_t n = wasm_runtime_get_data_segment_count(module);
    if (n < 0)
        return FALSE;

    guint64 copy_len = MIN(size, staging_size);
    GArray *ranges = g_array_new(FALSE, FALSE, sizeof(guint64) * 2);
    for (int32_t i = 0; i < n; i++) {
        uint64_t off = 0, range[2];
        uint32_t len = 0;
        if (!wasm_runtime_get_data_segment_range(module, i, &off, &len)) {
            g_array_free(ranges, TRUE);
            return FALSE;
        }
        if (len == 0 || off >= copy_len)
            continue;
        range[0] = off;
        range[1] = MIN(off + len, copy_len);
        g_array_append_val(ranges, range);
    }
    g_array_sort(ranges, ns_wasm_segment_range_cmp);

    guint64 pos = 0;
    for (guint i = 0; i < ranges->len; i++) {
        const guint64 *range =
            &g_array_index(ranges, guint64, (gsize)i * 2);
        if (range[0] > pos)
            memcpy(base + pos, staging + pos, range[0] - pos);
        if (range[1] > pos)
            pos = range[1];
    }
    if (copy_len > pos)
        memcpy(base + pos, staging + pos, copy_len - pos);
    g_array_free(ranges, TRUE);
    return TRUE;
}

static void
ns_wasm_adopt_provided_imports(JSContext *ctx, JSValueConst instance_obj,
                               ns_wasm_instance *wi)
{
    if (!JS_IsUndefined(wi->linkage.provided_memory)) {
        ns_wasm_memory *m = JS_GetOpaque(wi->linkage.provided_memory,
                                         ns_wasm_memory_class_id);
        if (m && m->shared && JS_IsUndefined(wi->memory_obj))
            wi->memory_obj = JS_DupValue(ctx, wi->linkage.provided_memory);
        wasm_memory_inst_t mem =
            m && m->staging ? wasm_runtime_get_memory(wi->inst, 0) : NULL;
        if (mem) {
            guint8 *base = wasm_memory_get_base_address(mem);
            size_t size = (size_t)wasm_memory_get_cur_page_count(mem) *
                          wasm_memory_get_bytes_per_page(mem);
            ns_wasm_copy_staging_around_segments(wi, base, size, m->staging,
                                                 m->staging_size);
            if (!JS_IsUndefined(m->buffer) &&
                JS_RepointArrayBuffer(ctx, m->buffer, base, size) == 0) {
                m->base = base;
                m->size = size;
            } else {
                ns_wasm_memory_detach_buffer(m);
            }
            ns_js_cage_shared_free(m->staging);
            m->staging = NULL;
            m->staging_size = 0;
            JS_FreeValue(ctx, m->instance);
            m->instance = JS_DupValue(ctx, instance_obj);
            if (JS_IsUndefined(wi->memory_obj))
                wi->memory_obj =
                    JS_DupValue(ctx, wi->linkage.provided_memory);
        }
    }
    if (!JS_IsUndefined(wi->linkage.provided_table)) {
        ns_wasm_table *t = JS_GetOpaque(wi->linkage.provided_table,
                                        ns_wasm_table_class_id);
        if (t && JS_IsUndefined(t->instance))
            t->instance = JS_DupValue(ctx, instance_obj);
    }
}

static JSValue
ns_wasm_instance_create(JSContext *ctx, JSValueConst module_obj,
                        JSValueConst import_object)
{
    ns_wasm_module *mod =
        JS_GetOpaque2(ctx, module_obj, ns_wasm_module_class_id);
    if (!mod || !mod->module)
        return JS_EXCEPTION;
    if (!ns_wasm_runtime_ready())
        return JS_ThrowRangeError(ctx, "wasm runtime init failed");

    JSRuntime *rt = JS_GetRuntime(ctx);
    ns_wasm_linkage linkage;
    ns_wasm_linkage_init(&linkage);

    char error_buf[192] = "";
    wasm_module_t inst_module = mod->module;
    int32_t import_count = wasm_runtime_get_import_count(mod->module);
    if (import_count > 0) {
        if (!ns_wasm_bind_imports(ctx, inst_module, &linkage,
                                  import_object)) {
            ns_wasm_linkage_clear(rt, &linkage);
            return JS_EXCEPTION;
        }
        if (!ns_wasm_link_import_symbols(ctx, mod)) {
            ns_wasm_linkage_clear(rt, &linkage);
            return JS_EXCEPTION;
        }
    }

    if (import_count == 0 &&
        !ns_wasm_bind_imports(ctx, inst_module, &linkage, import_object)) {
        ns_wasm_linkage_clear(rt, &linkage);
        return JS_EXCEPTION;
    }

    ns_wasm_instance *wi = g_new0(ns_wasm_instance, 1);
    wi->ctx = ctx;
    ns_js_pin_realm(ctx);
    wi->module_obj = JS_UNDEFINED;
    wi->exports = JS_UNDEFINED;
    wi->memory_obj = JS_UNDEFINED;
    wi->pending_exc = JS_UNDEFINED;
    wi->linkage = linkage;

    ns_wasm_memory *provided =
        JS_IsUndefined(wi->linkage.provided_memory) ? NULL
        : JS_GetOpaque(wi->linkage.provided_memory, ns_wasm_memory_class_id);
    g_private_set(&ns_wasm_instantiating, wi);
    ns_wamr_set_adopted_memory(provided ? provided->shared : NULL);
    wasm_module_inst_t inst =
        wasm_runtime_instantiate(inst_module, NS_WASM_STACK_SIZE, 0, error_buf,
                                 sizeof(error_buf));
    ns_wamr_set_adopted_memory(NULL);
    g_private_set(&ns_wasm_instantiating, NULL);
    if (!inst) {
        JSValue exc = JS_UNDEFINED;
        if (wi->has_pending) {
            exc = wi->pending_exc;
            wi->pending_exc = JS_UNDEFINED;
            wi->has_pending = FALSE;
        }
        ns_wasm_linkage_clear(rt, &wi->linkage);
        JS_FreeValueRT(rt, wi->pending_exc);
        g_free(wi);
        if (!JS_IsUndefined(exc))
            return JS_Throw(ctx, exc);
        return ns_wasm_throw_named(ctx, "LinkError", error_buf);
    }

    wasm_exec_env_t exec_env =
        wasm_runtime_create_exec_env(inst, NS_WASM_STACK_SIZE);
    if (!exec_env) {
        wasm_runtime_deinstantiate(inst);
        ns_wasm_linkage_clear(rt, &wi->linkage);
        JS_FreeValueRT(rt, wi->pending_exc);
        g_free(wi);
        return JS_ThrowRangeError(ctx, "wasm exec env creation failed");
    }

    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_instance_class_id);
    if (JS_IsException(obj)) {
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(inst);
        ns_wasm_linkage_clear(rt, &wi->linkage);
        JS_FreeValueRT(rt, wi->pending_exc);
        g_free(wi);
        return obj;
    }

    wi->inst = inst;
    wi->exec_env = exec_env;
    wi->module_obj = JS_DupValue(ctx, module_obj);
    JS_SetOpaque(obj, wi);
    wasm_runtime_set_custom_data(inst, wi);

    ns_wasm_adopt_provided_imports(ctx, obj, wi);

    wi->exports = ns_wasm_build_exports(ctx, obj, wi, mod);
    if (JS_IsException(wi->exports)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (!ns_wasm_eh_setup(ctx, obj, wi, mod, import_object)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, obj, "exports",
                                  JS_DupValue(ctx, wi->exports),
                                  JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue
ns_wasm_module_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                    JSValueConst *argv)
{
    (void)new_target;
    size_t len = 0;
    guint8 *bytes =
        argc > 0 ? ns_wasm_copy_buffer_source(ctx, argv[0], &len) : NULL;
    if (!bytes)
        return ns_wasm_buffer_source_error(ctx, "WebAssembly.Module requires a "
                                                "BufferSource");
    JSValue obj = ns_wasm_module_from_bytes(ctx, bytes, len);
    g_free(bytes);
    return obj;
}

static JSValue
ns_wasm_instance_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                      JSValueConst *argv)
{
    (void)new_target;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "WebAssembly.Instance requires a "
                                      "Module");
    return ns_wasm_instance_create(ctx, argv[0],
                                   argc > 1 ? argv[1] : JS_UNDEFINED);
}

static JSValue
ns_wasm_global_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                    JSValueConst *argv)
{
    (void)new_target;
    JSValueConst desc = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsObject(desc))
        return JS_ThrowTypeError(ctx, "wasm global descriptor required");
    JSValue tv = JS_GetPropertyStr(ctx, desc, "value");
    const char *type = JS_ToCString(ctx, tv);
    JS_FreeValue(ctx, tv);
    if (!type)
        return JS_EXCEPTION;
    wasm_valkind_t kind;
    gboolean is_ref = FALSE;
    if (strcmp(type, "i32") == 0) {
        kind = WASM_I32;
    } else if (strcmp(type, "i64") == 0) {
        kind = WASM_I64;
    } else if (strcmp(type, "f32") == 0) {
        kind = WASM_F32;
    } else if (strcmp(type, "f64") == 0) {
        kind = WASM_F64;
    } else if (strcmp(type, "externref") == 0) {
        kind = WASM_EXTERNREF;
        is_ref = TRUE;
    } else if (strcmp(type, "anyfunc") == 0 || strcmp(type, "funcref") == 0) {
        kind = WASM_FUNCREF;
        is_ref = TRUE;
    } else {
        JS_FreeCString(ctx, type);
        return JS_ThrowTypeError(ctx, "unsupported wasm global type");
    }
    JS_FreeCString(ctx, type);
    JSValue mv = JS_GetPropertyStr(ctx, desc, "mutable");
    int is_mutable = JS_ToBool(ctx, mv);
    JS_FreeValue(ctx, mv);
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    wasm_val_t val;
    memset(&val, 0, sizeof(val));
    val.kind = kind;
    if (!is_ref && ns_wasm_global_convert(ctx, kind, init, &val))
        return JS_EXCEPTION;
    if (kind == WASM_FUNCREF && !JS_IsUndefined(init) && !JS_IsNull(init) &&
        !JS_GetOpaque(init, ns_wasm_func_class_id))
        return JS_ThrowTypeError(ctx, "funcref global requires an exported "
                                      "wasm function or null");
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_global_class_id);
    if (JS_IsException(obj))
        return obj;
    ns_wasm_global *g = g_new0(ns_wasm_global, 1);
    g->ctx = ctx;
    ns_js_pin_realm(ctx);
    g->instance = JS_UNDEFINED;
    if (kind == WASM_FUNCREF && JS_IsUndefined(init))
        g->ref_value = JS_NULL;
    else
        g->ref_value = is_ref ? JS_DupValue(ctx, init) : JS_UNDEFINED;
    g->val = val;
    g->kind = kind;
    g->standalone = TRUE;
    g->is_mutable = is_mutable != 0;
    JS_SetOpaque(obj, g);
    return obj;
}

static JSValue
ns_wasm_memory_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                    JSValueConst *argv)
{
    (void)new_target;
    JSValueConst desc = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsObject(desc))
        return JS_ThrowTypeError(ctx, "wasm memory descriptor required");
    JSValue iv = JS_GetPropertyStr(ctx, desc, "initial");
    if (JS_IsUndefined(iv)) {
        JS_FreeValue(ctx, iv);
        iv = JS_GetPropertyStr(ctx, desc, "minimum");
    }
    uint32_t initial = 0;
    int rc = ns_wasm_to_u32_index(ctx, iv, &initial, "wasm memory initial");
    JS_FreeValue(ctx, iv);
    if (rc)
        return JS_EXCEPTION;
    JSValue maxv = JS_GetPropertyStr(ctx, desc, "maximum");
    uint32_t maximum = 0;
    if (!JS_IsUndefined(maxv))
        rc = ns_wasm_to_u32_index(ctx, maxv, &maximum, "wasm memory maximum");
    JS_FreeValue(ctx, maxv);
    if (rc)
        return JS_EXCEPTION;
    if (maximum && maximum < initial)
        return JS_ThrowRangeError(ctx, "wasm memory maximum is less than initial");
    JSValue sv = JS_GetPropertyStr(ctx, desc, "shared");
    int shared = JS_ToBool(ctx, sv);
    JS_FreeValue(ctx, sv);
    if (shared) {
        if (!maximum)
            return JS_ThrowTypeError(ctx, "shared wasm memory requires a "
                                          "maximum");
        if (!ns_wasm_runtime_ready())
            return JS_ThrowRangeError(ctx, "wasm runtime init failed");
        uint32_t cap = NS_WASM_SHARED_MAX_BYTES / NS_WASM_PAGE_SIZE;
        if (initial > cap)
            return JS_ThrowRangeError(ctx, "wasm memory initial is too large");
        wasm_memory_inst_t mem =
            ns_wamr_shared_memory_new(initial, MIN(maximum, cap));
        if (!mem)
            return JS_ThrowRangeError(ctx, "wasm memory could not be "
                                           "allocated");
        return ns_wasm_memory_new_shared(ctx, mem);
    }
    guint64 bytes = (guint64)initial * NS_WASM_PAGE_SIZE;
    if (bytes > NS_WASM_MEMORY_MAX_BYTES)
        return JS_ThrowRangeError(ctx, "wasm memory initial is too large");
    guint8 *staging = ns_js_cage_shared_malloc(bytes ? (size_t)bytes : 1);
    if (!staging)
        return JS_ThrowRangeError(ctx, "wasm memory could not be allocated");
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_memory_class_id);
    if (JS_IsException(obj)) {
        ns_js_cage_shared_free(staging);
        return obj;
    }
    ns_wasm_memory *m = g_new0(ns_wasm_memory, 1);
    m->ctx = ctx;
    ns_js_pin_realm(ctx);
    m->instance = JS_UNDEFINED;
    m->buffer = JS_UNDEFINED;
    m->staging = staging;
    memset(m->staging, 0, bytes ? (size_t)bytes : 1);
    m->staging_size = (size_t)bytes;
    m->max_pages = maximum;
    JS_SetOpaque(obj, m);
    return obj;
}

static JSValue
ns_wasm_table_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                   JSValueConst *argv)
{
    (void)new_target;
    JSValueConst desc = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsObject(desc))
        return JS_ThrowTypeError(ctx, "wasm table descriptor required");
    JSValue ev = JS_GetPropertyStr(ctx, desc, "element");
    const char *elem = JS_ToCString(ctx, ev);
    wasm_valkind_t elem_kind =
        elem && strcmp(elem, "externref") == 0 ? WASM_EXTERNREF : WASM_FUNCREF;
    if (elem)
        JS_FreeCString(ctx, elem);
    JS_FreeValue(ctx, ev);
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_table_class_id);
    if (JS_IsException(obj))
        return obj;
    ns_wasm_table *t = g_new0(ns_wasm_table, 1);
    t->ctx = ctx;
    ns_js_pin_realm(ctx);
    t->instance = JS_UNDEFINED;
    t->elem_kind = elem_kind;
    JS_SetOpaque(obj, t);
    return obj;
}

static JSValue
ns_wasm_resolved_promise(JSContext *ctx, JSValue value)
{
    JSValue resolvers[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolvers);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, value);
        return promise;
    }
    if (JS_IsException(value)) {
        JSValue exc = JS_GetException(ctx);
        JSValue r = JS_Call(ctx, resolvers[1], JS_UNDEFINED, 1,
                            (JSValueConst *)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
    } else {
        JSValue r = JS_Call(ctx, resolvers[0], JS_UNDEFINED, 1,
                            (JSValueConst *)&value);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, value);
    }
    JS_FreeValue(ctx, resolvers[0]);
    JS_FreeValue(ctx, resolvers[1]);
    return promise;
}

static JSValue
ns_wasm_compile(JSContext *ctx, JSValueConst this_val, int argc,
                JSValueConst *argv)
{
    (void)this_val;
    size_t len = 0;
    guint8 *bytes =
        argc > 0 ? ns_wasm_copy_buffer_source(ctx, argv[0], &len) : NULL;
    if (!bytes)
        return ns_wasm_resolved_promise(
            ctx, ns_wasm_buffer_source_error(ctx,
                                             "WebAssembly.compile requires a "
                                             "BufferSource"));
    JSValue module = ns_wasm_module_from_bytes(ctx, bytes, len);
    g_free(bytes);
    return ns_wasm_resolved_promise(ctx, module);
}

static JSValue
ns_wasm_instantiate(JSContext *ctx, JSValueConst this_val, int argc,
                    JSValueConst *argv)
{
    (void)this_val;
    JSValueConst source = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst imports = argc > 1 ? argv[1] : JS_UNDEFINED;

    if (JS_GetOpaque(source, ns_wasm_module_class_id))
        return ns_wasm_resolved_promise(
            ctx, ns_wasm_instance_create(ctx, source, imports));

    size_t len = 0;
    guint8 *bytes = ns_wasm_copy_buffer_source(ctx, source, &len);
    if (!bytes)
        return ns_wasm_resolved_promise(
            ctx, ns_wasm_buffer_source_error(
                     ctx, "WebAssembly.instantiate requires a BufferSource or "
                          "Module"));
    JSValue module = ns_wasm_module_from_bytes(ctx, bytes, len);
    g_free(bytes);
    if (JS_IsException(module))
        return ns_wasm_resolved_promise(ctx, module);

    JSValue instance = ns_wasm_instance_create(ctx, module, imports);
    if (JS_IsException(instance)) {
        JS_FreeValue(ctx, module);
        return ns_wasm_resolved_promise(ctx, instance);
    }
    JSValue pair = JS_NewObject(ctx);
    if (JS_IsException(pair)) {
        JS_FreeValue(ctx, module);
        JS_FreeValue(ctx, instance);
        return ns_wasm_resolved_promise(ctx, pair);
    }
    if (JS_SetPropertyStr(ctx, pair, "module", module) < 0) {
        JS_FreeValue(ctx, pair);
        JS_FreeValue(ctx, instance);
        return ns_wasm_resolved_promise(ctx, JS_EXCEPTION);
    }
    if (JS_SetPropertyStr(ctx, pair, "instance", instance) < 0) {
        JS_FreeValue(ctx, pair);
        return ns_wasm_resolved_promise(ctx, JS_EXCEPTION);
    }
    return ns_wasm_resolved_promise(ctx, pair);
}

static JSValue
ns_wasm_validate(JSContext *ctx, JSValueConst this_val, int argc,
                 JSValueConst *argv)
{
    (void)this_val;
    if (!ns_wasm_runtime_ready())
        return JS_NewBool(ctx, FALSE);
    size_t len = 0;
    guint8 *bytes =
        argc > 0 ? ns_wasm_copy_buffer_source(ctx, argv[0], &len) : NULL;
    if (!bytes)
        return ns_wasm_buffer_source_error(ctx,
                                           "WebAssembly.validate requires a "
                                           "BufferSource");
    char error_buf[128];
    LoadArgs load_args;
    memset(&load_args, 0, sizeof(load_args));
    load_args.name = (char *)"";
    load_args.no_resolve = true;
    wasm_module_t module = wasm_runtime_load_ex(bytes, (uint32_t)len,
                                                &load_args, error_buf,
                                                sizeof(error_buf));
    gboolean ok = module != NULL;
    if (module)
        wasm_runtime_unload(module);
    g_free(bytes);
    return JS_NewBool(ctx, ok);
}

static const char *
ns_wasm_kind_name(uint8_t kind)
{
    switch (kind) {
    case WASM_IMPORT_EXPORT_KIND_FUNC:
        return "function";
    case WASM_IMPORT_EXPORT_KIND_TABLE:
        return "table";
    case WASM_IMPORT_EXPORT_KIND_MEMORY:
        return "memory";
    case WASM_IMPORT_EXPORT_KIND_GLOBAL:
        return "global";
    case NS_WASM_KIND_TAG:
        return "tag";
    default:
        return "";
    }
}

static JSValue
ns_wasm_module_exports_static(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)this_val;
    ns_wasm_module *mod =
        argc > 0 ? JS_GetOpaque2(ctx, argv[0], ns_wasm_module_class_id) : NULL;
    if (!mod || !mod->module)
        return JS_ThrowTypeError(ctx,
                                 "WebAssembly.Module.exports requires a Module");
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    int32_t n = wasm_runtime_get_export_count(mod->module);
    for (int32_t i = 0; i < n; i++) {
        wasm_export_t exp;
        wasm_runtime_get_export_type(mod->module, i, &exp);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, exp.name));
        JS_SetPropertyStr(ctx, o, "kind",
                          JS_NewString(ctx, ns_wasm_kind_name(exp.kind)));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
    }
    return arr;
}

static JSValue
ns_wasm_module_imports_static(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)this_val;
    ns_wasm_module *mod =
        argc > 0 ? JS_GetOpaque2(ctx, argv[0], ns_wasm_module_class_id) : NULL;
    if (!mod || !mod->module)
        return JS_ThrowTypeError(ctx,
                                 "WebAssembly.Module.imports requires a Module");
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    int32_t n = wasm_runtime_get_import_count(mod->module);
    for (int32_t i = 0; i < n; i++) {
        wasm_import_t imp;
        wasm_runtime_get_import_type(mod->module, i, &imp);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "module",
                          JS_NewString(ctx, imp.module_name));
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, imp.name));
        JS_SetPropertyStr(ctx, o, "kind",
                          JS_NewString(ctx, ns_wasm_kind_name(imp.kind)));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
    }
    return arr;
}

static const char ns_wasm_bootstrap_js[] =
    "(() => {"
    "  const W = WebAssembly, dp = Object.defineProperty;"
    "  const error = (name) => {"
    "    const C = ({ [name]: class extends Error {"
    "      constructor(message) { super(message, arguments[1]); } } })[name];"
    "    dp(C.prototype, 'name', { value: name, writable: true, configurable: true });"
    "    dp(C.prototype, 'message', { value: '', writable: true, configurable: true });"
    "    dp(W, name, { value: C, writable: true, configurable: true });"
    "  };"
    "  error('CompileError'); error('LinkError'); error('RuntimeError');"
    "  const op = (f) => dp(W, f.name, { value: f, writable: true, enumerable: true, configurable: true });"
    "  op(async function compileStreaming(source) {"
    "    return W.compile(await (await source).arrayBuffer()); });"
    "  op(async function instantiateStreaming(source) {"
    "    const importObject = arguments[1];"
    "    return W.instantiate(await (await source).arrayBuffer(), importObject); });"
    "})();";

static void
ns_wasm_register_class(JSRuntime *rt, JSClassID *class_id,
                       const JSClassDef *def)
{
    ns_new_class_id(class_id);
    if (!JS_IsRegisteredClass(rt, *class_id))
        JS_NewClass(rt, *class_id, def);
}

void
ns_wasm_install(JSContext *ctx, JSValueConst global)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    ns_wamr_set_host_calls_ctors(true);
    ns_wasm_register_class(rt, &ns_wasm_module_class_id, &ns_wasm_module_class);
    ns_wasm_register_class(rt, &ns_wasm_instance_class_id,
                           &ns_wasm_instance_class);
    ns_wasm_register_class(rt, &ns_wasm_memory_class_id, &ns_wasm_memory_class);
    ns_wasm_register_class(rt, &ns_wasm_table_class_id, &ns_wasm_table_class);
    ns_wasm_register_class(rt, &ns_wasm_global_class_id, &ns_wasm_global_class);
    ns_wasm_register_class(rt, &ns_wasm_func_class_id, &ns_wasm_func_class);

    JSValue memory_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, memory_proto, ns_wasm_memory_proto_funcs,
                               G_N_ELEMENTS(ns_wasm_memory_proto_funcs));
    JS_SetClassProto(ctx, ns_wasm_memory_class_id, memory_proto);

    JSValue table_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, table_proto, ns_wasm_table_proto_funcs,
                               G_N_ELEMENTS(ns_wasm_table_proto_funcs));
    JS_SetClassProto(ctx, ns_wasm_table_class_id, table_proto);

    JSValue global_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, global_proto, ns_wasm_global_proto_funcs,
                               G_N_ELEMENTS(ns_wasm_global_proto_funcs));
    JS_SetClassProto(ctx, ns_wasm_global_class_id, global_proto);

    JSValue module_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, module_proto, ns_wasm_module_proto_funcs,
                               G_N_ELEMENTS(ns_wasm_module_proto_funcs));
    JS_SetClassProto(ctx, ns_wasm_module_class_id,
                     JS_DupValue(ctx, module_proto));
    JSValue instance_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, instance_proto,
                               ns_wasm_instance_proto_funcs,
                               G_N_ELEMENTS(ns_wasm_instance_proto_funcs));
    JS_SetClassProto(ctx, ns_wasm_instance_class_id,
                     JS_DupValue(ctx, instance_proto));

    JSValue function_ctor = JS_GetPropertyStr(ctx, global, "Function");
    JSValue function_proto = JS_GetPropertyStr(ctx, function_ctor,
                                               "prototype");
    JS_FreeValue(ctx, function_ctor);
    JS_SetClassProto(ctx, ns_wasm_func_class_id, function_proto);

    JSValue ns = JS_NewObject(ctx);
    JSValue module_ctor = JS_NewCFunction2(ctx, ns_wasm_module_ctor, "Module",
                                           1, JS_CFUNC_constructor, 0);
    JSValue instance_ctor = JS_NewCFunction2(ctx, ns_wasm_instance_ctor,
                                             "Instance", 2,
                                             JS_CFUNC_constructor, 0);
    JSValue memory_ctor = JS_NewCFunction2(ctx, ns_wasm_memory_ctor,
                                           "Memory", 1, JS_CFUNC_constructor,
                                           0);
    JSValue table_ctor = JS_NewCFunction2(ctx, ns_wasm_table_ctor,
                                          "Table", 1, JS_CFUNC_constructor, 0);
    JSValue global_ctor = JS_NewCFunction2(ctx, ns_wasm_global_ctor,
                                           "Global", 1, JS_CFUNC_constructor,
                                           0);
    JS_SetPropertyStr(ctx, memory_ctor, "prototype",
                      JS_DupValue(ctx, memory_proto));
    JS_SetPropertyStr(ctx, table_ctor, "prototype",
                      JS_DupValue(ctx, table_proto));
    JS_SetPropertyStr(ctx, global_ctor, "prototype",
                      JS_DupValue(ctx, global_proto));
    JS_SetPropertyStr(ctx, module_ctor, "exports",
                      JS_NewCFunction(ctx, ns_wasm_module_exports_static,
                                      "exports", 1));
    JS_SetPropertyStr(ctx, module_ctor, "imports",
                      JS_NewCFunction(ctx, ns_wasm_module_imports_static,
                                      "imports", 1));
    JS_SetPropertyStr(ctx, module_ctor, "prototype", module_proto);
    JS_SetPropertyStr(ctx, instance_ctor, "prototype", instance_proto);
    const int iface = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
    JS_DefinePropertyValueStr(ctx, instance_ctor, "length", JS_NewInt32(ctx, 1),
                              JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, ns, "Module", module_ctor, iface);
    JS_DefinePropertyValueStr(ctx, ns, "Instance", instance_ctor, iface);
    JS_DefinePropertyValueStr(ctx, ns, "Memory", memory_ctor, iface);
    JS_DefinePropertyValueStr(ctx, ns, "Table", table_ctor, iface);
    JS_DefinePropertyValueStr(ctx, ns, "Global", global_ctor, iface);
    JSValue compile_fn = JS_NewCFunction(ctx, ns_wasm_compile, "compile", 1);
    JS_SetPropertyStr(ctx, ns, "compile", compile_fn);
    JSValue validate_fn = JS_NewCFunction(ctx, ns_wasm_validate, "validate",
                                          1);
    JS_SetPropertyStr(ctx, ns, "validate", validate_fn);
    JSValue inst_fn = JS_NewCFunction(ctx, ns_wasm_instantiate, "instantiate",
                                      2);
    JS_DefinePropertyValueStr(ctx, inst_fn, "length", JS_NewInt32(ctx, 1),
                              JS_PROP_CONFIGURABLE);
    JS_SetPropertyStr(ctx, ns, "instantiate", inst_fn);
    JSValue sym_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
    JSValue tag_sym = JS_GetPropertyStr(ctx, sym_ctor, "toStringTag");
    JSAtom tag = JS_ValueToAtom(ctx, tag_sym);
    if (tag != JS_ATOM_NULL) {
        JS_DefinePropertyValue(ctx, ns, tag, JS_NewString(ctx, "WebAssembly"),
                               JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, tag);
    }
    JS_FreeValue(ctx, tag_sym);
    JS_FreeValue(ctx, sym_ctor);
    ns_wasm_eh_install(ctx, ns);
    JS_DefinePropertyValueStr(ctx, global, "WebAssembly", ns, iface);
    ns_wasm_guard_buffer_transfer(ctx, global);

    JSValue boot = JS_Eval(ctx, ns_wasm_bootstrap_js,
                           sizeof(ns_wasm_bootstrap_js) - 1,
                           "<wasm-bootstrap>",
                           JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_HIDE_SOURCE);
    if (JS_IsException(boot))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, boot);
}

typedef struct {
    gint refs;
    ns_wasm_module *mod;
    void *block;
} ns_wasm_module_handle;

static GMutex ns_wasm_handles_mutex;
static GHashTable *ns_wasm_handles;

static void
ns_wasm_module_unref(ns_wasm_module *m)
{
    if (!m || !g_atomic_int_dec_and_test(&m->refs))
        return;
    if (m->module)
        wasm_runtime_unload(m->module);
    ns_wasm_linkage_clear(NULL, &m->linkage);
    g_free(m->load_bytes);
    g_free(m);
}

static void
ns_wasm_module_handle_dup(void *opaque)
{
    ns_wasm_module_handle *h = opaque;
    g_atomic_int_inc(&h->refs);
}

static void
ns_wasm_module_handle_release(void *opaque)
{
    ns_wasm_module_handle *h = opaque;
    if (!g_atomic_int_dec_and_test(&h->refs))
        return;
    g_mutex_lock(&ns_wasm_handles_mutex);
    g_hash_table_remove(ns_wasm_handles, h->block);
    g_mutex_unlock(&ns_wasm_handles_mutex);
    ns_js_sab_unregister_foreign(h->block);
    ns_js_cage_shared_free(h->block);
    ns_wasm_module_unref(h->mod);
    g_free(h);
}

static JSValue
ns_wasm_module_handle_new(JSContext *ctx, ns_wasm_module *mod)
{
    ns_wasm_module_handle *h = g_new0(ns_wasm_module_handle, 1);
    h->block = ns_js_cage_shared_malloc(16);
    if (!h->block) {
        g_free(h);
        return JS_ThrowOutOfMemory(ctx);
    }
    memset(h->block, 0, 16);
    g_atomic_int_inc(&mod->refs);
    h->mod = mod;
    g_mutex_lock(&ns_wasm_handles_mutex);
    if (!ns_wasm_handles)
        ns_wasm_handles = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(ns_wasm_handles, h->block, h);
    g_mutex_unlock(&ns_wasm_handles_mutex);
    ns_js_sab_register_foreign(h->block, ns_wasm_module_handle_dup,
                               ns_wasm_module_handle_release, h);
    g_atomic_int_inc(&h->refs);
    JSValue sab = JS_NewArrayBuffer(ctx, h->block, 16, 0, NULL, NULL, TRUE);
    ns_wasm_module_handle_release(h);
    return sab;
}

static ns_wasm_module *
ns_wasm_module_from_block(void *block)
{
    g_mutex_lock(&ns_wasm_handles_mutex);
    ns_wasm_module_handle *h = ns_wasm_handles
        ? g_hash_table_lookup(ns_wasm_handles, block) : NULL;
    ns_wasm_module *mod = h ? h->mod : NULL;
    if (mod)
        g_atomic_int_inc(&mod->refs);
    g_mutex_unlock(&ns_wasm_handles_mutex);
    return mod;
}

JSValue
ns_wasm_wire_handle(JSContext *ctx, JSValueConst v, const char **kind)
{
    *kind = NULL;
    ns_wasm_memory *m = JS_GetOpaque(v, ns_wasm_memory_class_id);
    if (m && m->shared) {
        *kind = "WMEM";
        return ns_wasm_shared_buffer(ctx, m);
    }
    ns_wasm_module *mod = JS_GetOpaque(v, ns_wasm_module_class_id);
    if (mod) {
        *kind = "WMOD";
        return ns_wasm_module_handle_new(ctx, mod);
    }
    return JS_UNDEFINED;
}

JSValue
ns_wasm_wire_restore(JSContext *ctx, const char *kind, JSValueConst sab)
{
    size_t len = 0;
    uint8_t *data = JS_IsSharedArrayBuffer(sab)
        ? JS_GetArrayBuffer(ctx, &len, sab) : NULL;
    if (!data)
        return JS_NULL;
    if (strcmp(kind, "WMEM") == 0) {
        wasm_memory_inst_t mem = ns_wasm_shared_lookup(data);
        if (!mem)
            return JS_NULL;
        ns_wamr_shared_memory_dup(mem);
        return ns_wasm_memory_new_shared(ctx, mem);
    }
    ns_wasm_module *mod = ns_wasm_module_from_block(data);
    if (!mod)
        return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_module_class_id);
    if (JS_IsException(obj)) {
        ns_wasm_module_unref(mod);
        return obj;
    }
    JS_SetOpaque(obj, mod);
    return obj;
}

#endif
