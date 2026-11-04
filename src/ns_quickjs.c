/* Nordstjernen — quickjs-ng API entry points built over Bellard's original QuickJS.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "ns_quickjs.h"
#include "js_classid.h"

#include <glib.h>
#include <stdarg.h>
#include <string.h>

typedef struct ns_quickjs_class_ids {
    JSClassID array;
    JSClassID error;
    JSClassID data_view;
    JSClassID date;
    JSClassID regexp;
    JSClassID map;
    JSClassID set;
    JSClassID proxy;
    JSClassID boxed[JS_BOXED_SYMBOL + 1];
    JSClassID bytecode_function;
    JSClassID c_function;
    JSClassID c_function_data;
} ns_quickjs_class_ids;

typedef struct ns_quickjs_array_buffer_owner {
    JSReallocArrayBufferDataFunc *realloc_func;
    void *opaque;
} ns_quickjs_array_buffer_owner;

typedef struct ns_quickjs_forwarder {
    JSValue target;
} ns_quickjs_forwarder;

static ns_quickjs_class_ids ns_quickjs_classes;
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

static JSValue
ns_quickjs_construct(JSContext *ctx, const char *name, int argc,
                     JSValueConst *argv)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, name);
    JSValue obj = JS_CallConstructor(ctx, ctor, argc, argv);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    return obj;
}

static JSClassID
ns_quickjs_boxed_class_of(JSContext *ctx, JSValue primitive)
{
    JSClassID id = ns_quickjs_class_of(ctx, JS_ToObject(ctx, primitive));
    JS_FreeValue(ctx, primitive);
    return id;
}

static JSValue
ns_quickjs_noop(JSContext *ctx, JSValueConst this_val, int argc,
                JSValueConst *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

static JSValue
ns_quickjs_noop_data(JSContext *ctx, JSValueConst this_val, int argc,
                     JSValueConst *argv, int magic, JSValue *func_data)
{
    (void)magic;
    (void)func_data;
    return ns_quickjs_noop(ctx, this_val, argc, argv);
}

static void
ns_quickjs_learn_function_class_ids(JSContext *ctx)
{
    static const char probe[] = "(function () {})";
    ns_quickjs_class_ids *ids = &ns_quickjs_classes;
    ids->bytecode_function = ns_quickjs_class_of(ctx,
        JS_Eval(ctx, probe, sizeof(probe) - 1, "<ns_quickjs>",
                JS_EVAL_TYPE_GLOBAL));
    ids->c_function = ns_quickjs_class_of(ctx,
        JS_NewCFunction(ctx, ns_quickjs_noop, "", 0));
    ids->c_function_data = ns_quickjs_class_of(ctx,
        JS_NewCFunctionData(ctx, ns_quickjs_noop_data, 0, 0, 0, NULL));
}

static void
ns_quickjs_learn_value_class_ids(JSContext *ctx)
{
    ns_quickjs_class_ids *ids = &ns_quickjs_classes;
    ids->date = ns_quickjs_class_of(ctx, ns_quickjs_construct(ctx, "Date", 0, NULL));
    JSValue pattern = JS_NewString(ctx, "a");
    ids->regexp = ns_quickjs_class_of(ctx,
        ns_quickjs_construct(ctx, "RegExp", 1, &pattern));
    JS_FreeValue(ctx, pattern);
    ids->map = ns_quickjs_class_of(ctx, ns_quickjs_construct(ctx, "Map", 0, NULL));
    ids->set = ns_quickjs_class_of(ctx, ns_quickjs_construct(ctx, "Set", 0, NULL));
    JSValue proxy_args[2] = { JS_NewObject(ctx), JS_NewObject(ctx) };
    ids->proxy = ns_quickjs_class_of(ctx,
        ns_quickjs_construct(ctx, "Proxy", 2, proxy_args));
    JS_FreeValue(ctx, proxy_args[0]);
    JS_FreeValue(ctx, proxy_args[1]);
    ids->boxed[JS_BOXED_NUMBER] = ns_quickjs_boxed_class_of(ctx, JS_NewInt32(ctx, 0));
    ids->boxed[JS_BOXED_STRING] = ns_quickjs_boxed_class_of(ctx, JS_NewString(ctx, ""));
    ids->boxed[JS_BOXED_BOOLEAN] = ns_quickjs_boxed_class_of(ctx, JS_FALSE);
    ids->boxed[JS_BOXED_BIGINT] = ns_quickjs_boxed_class_of(ctx, JS_NewBigInt64(ctx, 0));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue symbol_fn = JS_GetPropertyStr(ctx, global, "Symbol");
    ids->boxed[JS_BOXED_SYMBOL] = ns_quickjs_boxed_class_of(ctx,
        JS_Call(ctx, symbol_fn, JS_UNDEFINED, 0, NULL));
    JS_FreeValue(ctx, symbol_fn);
    JS_FreeValue(ctx, global);
}

static void
ns_quickjs_learn_class_ids(JSContext *ctx)
{
    static const uint8_t one_byte;
    ns_quickjs_class_ids *ids = &ns_quickjs_classes;
    ids->array = ns_quickjs_class_of(ctx, JS_NewArray(ctx));
    ids->error = ns_quickjs_class_of(ctx, JS_NewError(ctx));

    JSValue buffer = JS_NewArrayBufferCopy(ctx, &one_byte, 1);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue data_view = JS_GetPropertyStr(ctx, global, "DataView");
    ids->data_view = ns_quickjs_class_of(ctx,
        JS_CallConstructor(ctx, data_view, 1, &buffer));
    JS_FreeValue(ctx, data_view);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, buffer);
    ns_quickjs_learn_value_class_ids(ctx);
    ns_quickjs_learn_function_class_ids(ctx);
}

JSContext *
ns_quickjs_new_context(JSRuntime *rt)
{
    static gsize learned;
    JSContext *ctx = (JS_NewContext)(rt);
    if (ctx && g_once_init_enter(&learned)) {
        ns_quickjs_learn_class_ids(ctx);
        g_once_init_leave(&learned, 1);
    }
    return ctx;
}

static bool
ns_quickjs_has_class(JSValueConst val, JSClassID class_id)
{
    return class_id != JS_INVALID_CLASS_ID && JS_GetClassID(val) == class_id;
}

bool
ns_quickjs_is_array(JSValueConst val)
{
    return ns_quickjs_has_class(val, ns_quickjs_classes.array);
}

bool
ns_quickjs_is_error(JSValueConst val)
{
    return ns_quickjs_has_class(val, ns_quickjs_classes.error);
}

bool
JS_IsDataView(JSValueConst val)
{
    return ns_quickjs_has_class(val, ns_quickjs_classes.data_view);
}

bool
JS_IsDate(JSValueConst val)
{
    return ns_quickjs_has_class(val, ns_quickjs_classes.date);
}

bool
JS_IsRegExp(JSValueConst val)
{
    return ns_quickjs_has_class(val, ns_quickjs_classes.regexp);
}

bool
JS_IsMap(JSValueConst val)
{
    return ns_quickjs_has_class(val, ns_quickjs_classes.map);
}

bool
JS_IsSet(JSValueConst val)
{
    return ns_quickjs_has_class(val, ns_quickjs_classes.set);
}

bool
JS_IsProxy(JSValueConst val)
{
    return ns_quickjs_has_class(val, ns_quickjs_classes.proxy);
}

JSValue
JS_GetErrorFrames(JSContext *ctx, JSValueConst val)
{
    if (!ns_quickjs_is_error(val))
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

int
JS_GetBoxedPrimitiveKind(JSValueConst val)
{
    for (int kind = JS_BOXED_NUMBER; kind <= JS_BOXED_SYMBOL; kind++)
        if (ns_quickjs_has_class(val, ns_quickjs_classes.boxed[kind]))
            return kind;
    return JS_BOXED_NONE;
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

static bool
ns_quickjs_parse_array_index(const char *s, size_t len, uint32_t *pval)
{
    if (len == 0 || len > 10 || (len > 1 && s[0] == '0'))
        return false;
    uint64_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        value = value * 10 + (uint64_t)(s[i] - '0');
    }
    if (value > 0xFFFFFFFEu)
        return false;
    *pval = (uint32_t)value;
    return true;
}

bool
JS_AtomIsArrayIndex(JSContext *ctx, uint32_t *pval, JSAtom atom)
{
    *pval = 0;
    JSValue key = JS_AtomToValue(ctx, atom);
    bool is_index = false;
    if (JS_IsString(key)) {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, key);
        if (s) {
            is_index = ns_quickjs_parse_array_index(s, len, pval);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, key);
    return is_index;
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

JSContext *
JS_GetPendingJobRealm(JSRuntime *rt)
{
    (void)rt;
    return NULL;
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
    JSValue proto = JS_GetClassProto(ctx, ns_quickjs_classes.bytecode_function);
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

JSValue
JS_CloneCFunction(JSContext *ctx, JSValueConst func)
{
    JSClassID class_id = JS_GetClassID(func);
    if (class_id == JS_INVALID_CLASS_ID)
        return JS_UNDEFINED;
    bool constructor = JS_IsConstructor(ctx, func);
    if (class_id == ns_quickjs_forwarder_class_id) {
        ns_quickjs_forwarder *fw = JS_GetOpaque(func, class_id);
        if (!fw)
            return JS_UNDEFINED;
        return JS_NewForwarder(ctx, fw->target, "", 0, constructor);
    }
    if (class_id != ns_quickjs_classes.c_function &&
        class_id != ns_quickjs_classes.c_function_data)
        return JS_UNDEFINED;
    return JS_NewForwarder(ctx, func, "", 0, constructor);
}
