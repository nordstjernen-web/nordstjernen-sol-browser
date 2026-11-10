/* Nordstjernen — the quickjs-ng API the engine is written against, on either QuickJS.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_QUICKJS_H
#define NS_QUICKJS_H

#include <quickjs.h>

#ifdef NS_QUICKJS_ORIGINAL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef JS_BOOL ns_js_bool;
typedef JSRuntime ns_js_interrupt_arg;

typedef enum JSBrandMode {
    JS_BRAND_THROW,
    JS_BRAND_REJECT,
    JS_BRAND_IGNORE,
} JSBrandMode;

static inline int
JS_NewCFunctionBrand(JSContext *ctx, const JSClassID *class_ids, int count)
{
    (void)ctx;
    (void)class_ids;
    (void)count;
    return 0;
}

static inline void
JS_SetCFunctionBrand(JSContext *ctx, JSValueConst func, int brand,
                     JSBrandMode mode)
{
    (void)ctx;
    (void)func;
    (void)brand;
    (void)mode;
}

static inline int
JS_SetDefaultLocale(JSRuntime *rt, const char *tag)
{
    (void)rt;
    (void)tag;
    return 0;
}

#define JS_EVAL_OPTIONS_VERSION 1

typedef struct JSEvalOptions {
    int version;
    int eval_flags;
    const char *filename;
    int line_num;
    int col_num;
} JSEvalOptions;

typedef void *JSReallocArrayBufferDataFunc(JSRuntime *rt, void *opaque,
                                           void *ptr, size_t size);

JSContext *ns_quickjs_new_context(JSRuntime *rt);
bool ns_quickjs_is_array(JSValueConst val);
bool ns_quickjs_is_error(JSValueConst val);
JSValue ns_quickjs_new_array_buffer(JSContext *ctx, uint8_t *buf, size_t len,
                                    size_t max_len,
                                    JSReallocArrayBufferDataFunc *realloc_func,
                                    void *opaque, bool is_shared);
JSValue ns_quickjs_new_typed_array(JSContext *ctx, int argc, JSValueConst *argv,
                                   JSTypedArrayEnum type);

#define JS_EVAL_FLAG_HIDE_SOURCE 0

enum {
    JS_BOXED_NONE, JS_BOXED_NUMBER, JS_BOXED_STRING, JS_BOXED_BOOLEAN,
    JS_BOXED_BIGINT, JS_BOXED_SYMBOL,
};

bool JS_IsDate(JSValueConst val);
bool JS_IsRegExp(JSValueConst val);
bool JS_IsMap(JSValueConst val);
bool JS_IsSet(JSValueConst val);
bool JS_IsProxy(JSValueConst val);
JSValue JS_GetErrorFrames(JSContext *ctx, JSValueConst val);
int JS_GetBoxedPrimitiveKind(JSValueConst val);
bool JS_IsRunningScript(JSContext *ctx);
int JS_FreezeObject(JSContext *ctx, JSValueConst obj);
JSContext *JS_GetPendingJobRealm(JSRuntime *rt);
bool JS_IsDataView(JSValueConst val);
bool JS_AtomIsArrayIndex(JSContext *ctx, uint32_t *pval, JSAtom atom);
JSValue JS_EvalThis2(JSContext *ctx, JSValueConst this_obj, const char *input,
                     size_t input_len, JSEvalOptions *options);
JSValue JS_NewStringUTF16(JSContext *ctx, const uint16_t *buf, size_t len);
JSValue __js_printf_like(3, 4) JS_ThrowDOMException(JSContext *ctx,
                                                    const char *name,
                                                    const char *fmt, ...);
const char *JS_GetVersion(void);
JSValue JS_ToNumber(JSContext *ctx, JSValueConst val);
JSValue JS_GetPropertyInt64(JSContext *ctx, JSValueConst obj, int64_t idx);
int JS_GetClassCount(JSRuntime *rt);
JSValue JS_NewForwarder(JSContext *ctx, JSValueConst target, const char *name,
                        int length, bool constructor);
JSValue JS_CloneCFunction(JSContext *ctx, JSValueConst func);

static inline bool
ns_quickjs_is_big_int(JSValueConst val)
{
    int tag = JS_VALUE_GET_TAG(val);
    return tag == JS_TAG_BIG_INT || tag == JS_TAG_SHORT_BIG_INT;
}

static inline bool
JS_IsStrictEqual(JSContext *ctx, JSValueConst op1, JSValueConst op2)
{
    return JS_StrictEq(ctx, op1, op2);
}

static inline JSContext *
JS_GetCallerRealm(JSContext *ctx)
{
    return ctx;
}

static inline JSContext *
JS_GetFunctionRealm(JSContext *ctx, JSValueConst func_obj)
{
    (void)func_obj;
    return ctx;
}

static inline int
JS_RepointArrayBuffer(JSContext *ctx, JSValueConst obj, uint8_t *data,
                      size_t byte_length)
{
    (void)ctx;
    (void)obj;
    (void)data;
    (void)byte_length;
    return -1;
}

static inline bool
JS_IsHostAccess(JSContext *ctx)
{
    (void)ctx;
    return true;
}

typedef bool JSCanCompileStrings(JSContext *ctx);

static inline void
JS_SetCanCompileStrings(JSRuntime *rt, JSCanCompileStrings *fn)
{
    (void)rt;
    (void)fn;
}

static inline void
JS_SetImmutablePrototype(JSContext *ctx, JSValueConst obj)
{
    (void)ctx;
    (void)obj;
}

static inline int
JS_AddEnginePrivateName(JSContext *ctx, const char *name)
{
    (void)ctx;
    (void)name;
    return 0;
}

static inline JSAtom
JS_GetEnginePrivateAtom(JSContext *ctx, const char *name)
{
    return JS_NewAtom(ctx, name);
}

static inline JSValue
JS_GetArrayBufferViewBuffer(JSContext *ctx, JSValueConst obj,
                            size_t *pbyte_offset, size_t *pbyte_length)
{
    return JS_GetTypedArrayBuffer(ctx, obj, pbyte_offset, pbyte_length, NULL);
}

static inline bool
JS_IsEngineFunction(JSValueConst fn)
{
    (void)fn;
    return true;
}

static inline bool
JS_IsHostCaller(JSContext *ctx)
{
    (void)ctx;
    return false;
}

static inline int
JS_SetPropertyReceiver(JSContext *ctx, JSValueConst obj, JSAtom prop,
                       JSValue val, JSValueConst receiver, int flags)
{
    (void)obj;
    (void)flags;
    return JS_SetProperty(ctx, receiver, prop, val);
}

#define JS_NewContext(rt) ns_quickjs_new_context(rt)
#define JS_IsArray(val)   ns_quickjs_is_array(val)
#define JS_IsError(val)   ns_quickjs_is_error(val)
#define JS_IsBigInt(val)  ns_quickjs_is_big_int(val)
#define JS_NewArrayBuffer(ctx, buf, len, max_len, realloc_func, opaque, shared) \
    ns_quickjs_new_array_buffer(ctx, buf, len, max_len, realloc_func, opaque, shared)
#define JS_NewTypedArray(ctx, argc, argv, type) \
    ns_quickjs_new_typed_array(ctx, argc, argv, type)

#else

typedef bool ns_js_bool;
typedef JSContext ns_js_interrupt_arg;

#endif

#endif
