/* Nordstjernen — quickjs-ng API entry points built over Bellard's original QuickJS.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "ns_quickjs.h"
#include "js_classid.h"

#include <glib.h>
#include <stdarg.h>
#include <string.h>

typedef struct ns_quickjs_array_buffer_owner {
    JSReallocArrayBufferDataFunc *realloc_func;
    void *opaque;
} ns_quickjs_array_buffer_owner;

typedef struct ns_quickjs_forwarder {
    JSValue target;
} ns_quickjs_forwarder;

static JSClassID ns_quickjs_function_class_id;
static JSClassID ns_quickjs_forwarder_class_id;

static JSClassID
ns_quickjs_class_of(JSContext *ctx, JSValue val)
{
    JSClassID id = JS_GetClassID(val);
    if (JS_IsException(val))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, val);
    return id;
}

static void
ns_quickjs_learn_class_ids(JSContext *ctx)
{
    static const char probe[] = "(function () {})";
    ns_quickjs_function_class_id = ns_quickjs_class_of(ctx,
        JS_Eval(ctx, probe, sizeof(probe) - 1, "<ns_quickjs>",
                JS_EVAL_TYPE_GLOBAL));
}

JSContext *
ns_quickjs_new_context(JSRuntime *rt)
{
    static gsize learned;
    JSContext *ctx = (JS_NewContext)(rt);
    if (ctx && (JS_AddIntrinsicTemporal(ctx) < 0 || JS_AddIntrinsicIntl(ctx) < 0)) {
        JS_FreeContext(ctx);
        return NULL;
    }
    if (ctx && g_once_init_enter(&learned)) {
        ns_quickjs_learn_class_ids(ctx);
        g_once_init_leave(&learned, 1);
    }
    return ctx;
}

JSValue
JS_GetErrorFrames(JSContext *ctx, JSValueConst val)
{
    if (!JS_IsErrorObject(val))
        return JS_UNDEFINED;
    JSAtom atom = JS_NewAtom(ctx, "stack");
    JSPropertyDescriptor desc;
    int has = JS_GetOwnProperty(ctx, &desc, val, atom);
    JS_FreeAtom(ctx, atom);
    if (has < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    if (!has)
        return JS_UNDEFINED;
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    if ((desc.flags & JS_PROP_GETSET) || !JS_IsString(desc.value)) {
        JS_FreeValue(ctx, desc.value);
        return JS_UNDEFINED;
    }
    return desc.value;
}

static void
ns_quickjs_array_buffer_free(JSRuntime *rt, void *opaque, void *ptr)
{
    ns_quickjs_array_buffer_owner *owner = opaque;
    owner->realloc_func(rt, owner->opaque, ptr, 0);
    g_free(owner);
}

JSValue
ns_quickjs_new_array_buffer(JSContext *ctx, uint8_t *buf, size_t len,
                            size_t max_len,
                            JSReallocArrayBufferDataFunc *realloc_func,
                            void *opaque, bool is_shared)
{
    if (max_len != 0)
        return JS_ThrowRangeError(ctx, "resizable external ArrayBuffer is not supported");
    if (!realloc_func)
        return (JS_NewArrayBuffer)(ctx, buf, len, NULL, opaque, is_shared);
    ns_quickjs_array_buffer_owner *owner = g_new(ns_quickjs_array_buffer_owner, 1);
    owner->realloc_func = realloc_func;
    owner->opaque = opaque;
    JSValue buffer = (JS_NewArrayBuffer)(ctx, buf, len,
                                         ns_quickjs_array_buffer_free, owner,
                                         is_shared);
    if (JS_IsException(buffer))
        g_free(owner);
    return buffer;
}

JSValue
ns_quickjs_new_typed_array(JSContext *ctx, int argc, JSValueConst *argv,
                           JSTypedArrayEnum type)
{
    JSValueConst padded[3] = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED };
    if (argc >= 3)
        return (JS_NewTypedArray)(ctx, argc, argv, type);
    for (int i = 0; i < argc; i++)
        padded[i] = argv[i];
    return (JS_NewTypedArray)(ctx, 3, padded, type);
}

JSValue
JS_EvalThis2(JSContext *ctx, JSValueConst this_obj, const char *input,
             size_t input_len, JSEvalOptions *options)
{
    const char *filename = options->filename ? options->filename : "<unnamed>";
    size_t lines = options->line_num > 1 ? (size_t)options->line_num - 1 : 0;
    size_t columns = options->col_num > 1 ? (size_t)options->col_num - 1 : 0;
    gboolean hashbang = input_len >= 2 && input[0] == '#' && input[1] == '!';
    if ((lines == 0 && columns == 0) || hashbang ||
        input_len > G_MAXSIZE - lines - columns - 1)
        return JS_EvalThis(ctx, this_obj, input, input_len, filename,
                           options->eval_flags);
    size_t padded_len = lines + columns + input_len;
    char *padded = g_try_malloc(padded_len + 1);
    if (!padded)
        return JS_ThrowOutOfMemory(ctx);
    memset(padded, '\n', lines);
    memset(padded + lines, ' ', columns);
    memcpy(padded + lines + columns, input, input_len);
    padded[padded_len] = '\0';
    JSValue result = JS_EvalThis(ctx, this_obj, padded, padded_len,
                                 filename, options->eval_flags);
    g_free(padded);
    return result;
}

static size_t
ns_quickjs_put_wtf8(char *out, uint32_t c)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

JSValue
JS_NewStringUTF16(JSContext *ctx, const uint16_t *buf, size_t len)
{
    if (len > (G_MAXSIZE - 1) / 3)
        return JS_ThrowRangeError(ctx, "invalid string length");
    char *wtf8 = g_try_malloc(len * 3 + 1);
    if (!wtf8)
        return JS_ThrowOutOfMemory(ctx);
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t c = buf[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len &&
            buf[i + 1] >= 0xDC00 && buf[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (uint32_t)(buf[i + 1] - 0xDC00);
            i++;
        }
        n += ns_quickjs_put_wtf8(wtf8 + n, c);
    }
    JSValue str = JS_NewStringLen(ctx, wtf8, n);
    g_free(wtf8);
    return str;
}

JSValue
JS_ThrowDOMException(JSContext *ctx, const char *name, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *message = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "DOMException");
    JS_FreeValue(ctx, global);
    JSValue error;
    if (JS_IsConstructor(ctx, ctor)) {
        JSValue args[2] = { JS_NewString(ctx, message), JS_NewString(ctx, name) };
        error = JS_CallConstructor(ctx, ctor, 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
    } else {
        error = JS_NewError(ctx);
        if (!JS_IsException(error)) {
            JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, name));
            JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message));
        }
    }
    JS_FreeValue(ctx, ctor);
    g_free(message);
    if (JS_IsException(error))
        return JS_EXCEPTION;
    return JS_Throw(ctx, error);
}

bool
JS_IsRunningScript(JSContext *ctx)
{
    for (int level = 0; level < 8; level++) {
        JSAtom name = JS_GetScriptOrModuleName(ctx, level);
        if (name != JS_ATOM_NULL) {
            JS_FreeAtom(ctx, name);
            return true;
        }
    }
    return false;
}

int
JS_FreezeObject(JSContext *ctx, JSValueConst obj)
{
    JSPropertyEnum *props = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &count, obj,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return -1;
    int ret = 0;
    for (uint32_t i = 0; i < count; i++) {
        JSPropertyDescriptor desc;
        int has = JS_GetOwnProperty(ctx, &desc, obj, props[i].atom);
        int flags = JS_PROP_HAS_CONFIGURABLE;
        if (has > 0) {
            if (!(desc.flags & JS_PROP_GETSET)) flags |= JS_PROP_HAS_WRITABLE;
            JS_FreeValue(ctx, desc.value);
            JS_FreeValue(ctx, desc.getter);
            JS_FreeValue(ctx, desc.setter);
        }
        if (has < 0 ||
            JS_DefineProperty(ctx, obj, props[i].atom, JS_UNDEFINED,
                              JS_UNDEFINED, JS_UNDEFINED, flags) < 0)
            ret = -1;
    }
    for (uint32_t i = 0; i < count; i++)
        JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props);
    if (ret == 0 && JS_PreventExtensions(ctx, obj) < 0)
        ret = -1;
    return ret;
}

const char *
JS_GetVersion(void)
{
    return NS_QUICKJS_ORIGINAL_VERSION;
}

JSValue
JS_ToNumber(JSContext *ctx, JSValueConst val)
{
    double d;
    if (JS_ToFloat64(ctx, &d, val) < 0)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, d);
}

JSValue
JS_GetPropertyInt64(JSContext *ctx, JSValueConst obj, int64_t idx)
{
    if (idx >= 0 && idx <= UINT32_MAX - 1)
        return JS_GetPropertyUint32(ctx, obj, (uint32_t)idx);
    JSValue key = JS_NewInt64(ctx, idx);
    JSAtom atom = JS_ValueToAtom(ctx, key);
    JS_FreeValue(ctx, key);
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    JSValue v = JS_GetProperty(ctx, obj, atom);
    JS_FreeAtom(ctx, atom);
    return v;
}

int
JS_GetClassCount(JSRuntime *rt)
{
    JSClassID id = (1 << 16) - 1;
    while (id > 0 && !JS_IsRegisteredClass(rt, id))
        id--;
    return (int)id + 1;
}

static void
ns_quickjs_forwarder_finalize(JSRuntime *rt, JSValue obj)
{
    ns_quickjs_forwarder *fw = JS_GetOpaque(obj, ns_quickjs_forwarder_class_id);
    if (!fw)
        return;
    JS_FreeValueRT(rt, fw->target);
    g_free(fw);
}

static void
ns_quickjs_forwarder_mark(JSRuntime *rt, JSValueConst obj,
                          JS_MarkFunc *mark_func)
{
    ns_quickjs_forwarder *fw = JS_GetOpaque(obj, ns_quickjs_forwarder_class_id);
    if (fw)
        JS_MarkValue(rt, fw->target, mark_func);
}

static JSValue
ns_quickjs_forwarder_call(JSContext *ctx, JSValueConst func_obj,
                          JSValueConst this_val, int argc, JSValueConst *argv,
                          int flags)
{
    ns_quickjs_forwarder *fw = JS_GetOpaque(func_obj,
                                            ns_quickjs_forwarder_class_id);
    if (!fw)
        return JS_ThrowTypeError(ctx, "not a function");
    if (flags & JS_CALL_FLAG_CONSTRUCTOR)
        return JS_CallConstructor2(ctx, fw->target, this_val, argc, argv);
    if (!JS_IsUndefined(this_val) && !JS_IsNull(this_val))
        return JS_Call(ctx, fw->target, this_val, argc, argv);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ret = JS_Call(ctx, fw->target, global, argc, argv);
    JS_FreeValue(ctx, global);
    return ret;
}

static const JSClassDef ns_quickjs_forwarder_class = {
    .class_name = "Function",
    .finalizer = ns_quickjs_forwarder_finalize,
    .gc_mark = ns_quickjs_forwarder_mark,
    .call = ns_quickjs_forwarder_call,
};

static JSClassID
ns_quickjs_forwarder_class_for(JSRuntime *rt)
{
    JSClassID class_id = ns_new_class_id(&ns_quickjs_forwarder_class_id);
    if (!JS_IsRegisteredClass(rt, class_id) &&
        JS_NewClass(rt, class_id, &ns_quickjs_forwarder_class) < 0)
        return JS_INVALID_CLASS_ID;
    return class_id;
}

JSValue
JS_NewForwarder(JSContext *ctx, JSValueConst target, const char *name,
                int length, bool constructor)
{
    JSClassID class_id = ns_quickjs_forwarder_class_for(JS_GetRuntime(ctx));
    if (class_id == JS_INVALID_CLASS_ID)
        return JS_ThrowInternalError(ctx, "cannot register the forwarder class");
    JSValue proto = JS_GetClassProto(ctx, ns_quickjs_function_class_id);
    JSValue func = JS_NewObjectProtoClass(ctx, proto, class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(func))
        return func;
    ns_quickjs_forwarder *fw = g_new(ns_quickjs_forwarder, 1);
    fw->target = JS_DupValue(ctx, target);
    JS_SetOpaque(func, fw);
    JS_SetConstructorBit(ctx, func, constructor);
    JS_DefinePropertyValueStr(ctx, func, "length", JS_NewInt32(ctx, length),
                              JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, func, "name", JS_NewString(ctx, name),
                              JS_PROP_CONFIGURABLE);
    return func;
}

