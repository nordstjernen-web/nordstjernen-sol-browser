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
JSValue ns_quickjs_new_array_buffer(JSContext *ctx, uint8_t *buf, size_t len,
                                    size_t max_len,
                                    JSReallocArrayBufferDataFunc *realloc_func,
                                    void *opaque, bool is_shared);
JSValue ns_quickjs_new_typed_array(JSContext *ctx, int argc, JSValueConst *argv,
                                   JSTypedArrayEnum type);


bool JS_IsRunningScript(JSContext *ctx);
int JS_FreezeObject(JSContext *ctx, JSValueConst obj);
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

static inline JSValue
JS_GetArrayBufferViewBuffer(JSContext *ctx, JSValueConst obj,
                            size_t *pbyte_offset, size_t *pbyte_length)
{
    return JS_GetTypedArrayBuffer(ctx, obj, pbyte_offset, pbyte_length, NULL);
}

#define JS_NewContext(rt) ns_quickjs_new_context(rt)
#define JS_IsArray(val)   JS_IsArrayObject(val)
#define JS_IsError(val)   JS_IsErrorObject(val)
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
