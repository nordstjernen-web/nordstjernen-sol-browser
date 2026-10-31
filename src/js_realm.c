/* Nordstjernen — native ShadowRealm over the QuickJS C API. */

#include "js_realm.h"
#include "js_classid.h"

#include <string.h>
#include <glib.h>

static JSClassID ns_shadowrealm_class_id;
static JSClassID ns_shadowrealm_handle_class_id;

typedef struct {
    JSContext *outer;
    JSContext *child;
    void      *realm_obj;
    int        refs;
} ns_shadowrealm;

static void
realm_unref(ns_shadowrealm *r)
{
    if (r && --r->refs == 0) g_free(r);
}

static void
ns_shadowrealm_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_shadowrealm *r = JS_GetOpaque(val, ns_shadowrealm_class_id);
    if (!r) return;
    r->realm_obj = NULL;
    JSContext *child = r->child;
    r->child = NULL;
    if (child) JS_FreeContext(child);
    realm_unref(r);
}

static void
ns_shadowrealm_handle_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    realm_unref(JS_GetOpaque(val, ns_shadowrealm_handle_class_id));
}

static JSClassDef ns_shadowrealm_class = {
    "ShadowRealm", .finalizer = ns_shadowrealm_finalizer,
};

static JSClassDef ns_shadowrealm_handle_class = {
    "ShadowRealmHandle", .finalizer = ns_shadowrealm_handle_finalizer,
};

static ns_shadowrealm *
realm_from_data(JSValueConst v)
{
    ns_shadowrealm *r = JS_GetOpaque(v, ns_shadowrealm_class_id);
    return r ? r : JS_GetOpaque(v, ns_shadowrealm_handle_class_id);
}

static JSValue
realm_owner_for(JSContext *into, ns_shadowrealm *r, gboolean from_inner)
{
    if (from_inner)
        return JS_DupValue(into, JS_MKPTR(JS_TAG_OBJECT, r->realm_obj));
    JSValue h = JS_NewObjectClass(into, ns_shadowrealm_handle_class_id);
    if (JS_IsException(h)) return h;
    r->refs++;
    JS_SetOpaque(h, r);
    return h;
}

static JSValue realm_wrap_value(JSContext *into, ns_shadowrealm *r,
                                JSValue v, gboolean from_inner);

static JSValue
realm_wrapped_call(JSContext *ctx, JSValueConst this_val, int argc,
                   JSValueConst *argv, int magic, JSValueConst *data)
{
    (void)this_val;
    JSValueConst target = data[0];
    gboolean target_inner = magic != 0;
    ns_shadowrealm *r = realm_from_data(data[1]);
    if (!r || !r->child || !r->outer || !r->realm_obj)
        return JS_ThrowTypeError(ctx, "ShadowRealm is gone");
    JSContext *tctx = target_inner ? r->child : r->outer;
    for (int i = 0; i < argc; i++) {
        if (JS_IsObject(argv[i]) && !JS_IsFunction(ctx, argv[i]))
            return JS_ThrowTypeError(ctx,
                "ShadowRealm wrapped function: only primitives and callables "
                "may cross the realm boundary");
    }
    JSValue *args = argc > 0 ? g_new0(JSValue, argc) : NULL;
    for (int i = 0; i < argc; i++) {
        args[i] = realm_wrap_value(tctx, r, JS_DupValue(ctx, argv[i]),
                                   !target_inner);
        if (JS_IsException(args[i])) {
            for (int j = 0; j < i; j++) JS_FreeValue(tctx, args[j]);
            g_free(args);
            JS_FreeValue(tctx, JS_GetException(tctx));
            return JS_ThrowTypeError(ctx, "ShadowRealm: argument cannot cross");
        }
    }
    JSValue res = JS_Call(tctx, target, JS_UNDEFINED, argc,
                          (JSValueConst *)args);
    for (int i = 0; i < argc; i++) JS_FreeValue(tctx, args[i]);
    g_free(args);
    if (JS_IsException(res)) {
        JSValue exc = JS_GetException(tctx);
        const char *msg = JS_ToCString(tctx, exc);
        JSValue te = JS_ThrowTypeError(ctx, "%s",
                                       msg ? msg : "ShadowRealm callable threw");
        if (msg) JS_FreeCString(tctx, msg);
        JS_FreeValue(tctx, exc);
        return te;
    }
    return realm_wrap_value(ctx, r, res, target_inner);
}

static JSValue
realm_wrap_value(JSContext *into, ns_shadowrealm *r, JSValue v,
                 gboolean from_inner)
{
    if (!JS_IsObject(v))
        return v;
    if (JS_IsFunction(into, v)) {
        JSValue owner = realm_owner_for(into, r, from_inner);
        if (JS_IsException(owner)) {
            JS_FreeValue(into, v);
            return owner;
        }
        JSValueConst data[2] = { v, owner };
        JSValue fn = JS_NewCFunctionData(into, realm_wrapped_call, 0,
                                         from_inner ? 1 : 0, 2, data);
        JS_FreeValue(into, owner);
        JS_FreeValue(into, v);
        return fn;
    }
    JS_FreeValue(into, v);
    return JS_ThrowTypeError(into,
        "ShadowRealm: evaluation result must be a primitive or a callable");
}

static JSValue
ns_shadowrealm_ctor(JSContext *ctx, JSValueConst new_target,
                    int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSContext *child = JS_NewContext(JS_GetRuntime(ctx));
    if (!child) return JS_ThrowOutOfMemory(ctx);
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_IsObject(proto)
        ? JS_NewObjectProtoClass(ctx, proto, ns_shadowrealm_class_id)
        : JS_NewObjectClass(ctx, ns_shadowrealm_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) { JS_FreeContext(child); return obj; }
    ns_shadowrealm *r = g_new0(ns_shadowrealm, 1);
    JS_SetContextOpaque(child, JS_GetContextOpaque(ctx));
    r->outer = ctx;
    r->child = child;
    r->realm_obj = JS_VALUE_GET_PTR(obj);
    r->refs = 1;
    JS_SetOpaque(obj, r);
    return obj;
}

static JSValue
ns_shadowrealm_evaluate(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    ns_shadowrealm *r = JS_GetOpaque2(ctx, this_val, ns_shadowrealm_class_id);
    if (!r) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "ShadowRealm.prototype.evaluate expects a string");
    if (!ns_js_eval_compile_allowed(ctx))
        return JS_ThrowTypeError(ctx,
            "ShadowRealm.prototype.evaluate: refused to evaluate a string "
            "(Content-Security-Policy)");
    size_t len = 0;
    const char *src = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!src) return JS_EXCEPTION;
    JSValue res = JS_Eval(r->child, src, len, "<shadowrealm>",
                          JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(ctx, src);
    if (JS_IsException(res)) {
        JSValue exc = JS_GetException(r->child);
        const char *msg = JS_ToCString(r->child, exc);
        JSValue te = JS_ThrowTypeError(ctx, "%s",
                                       msg ? msg : "ShadowRealm evaluate threw");
        if (msg) JS_FreeCString(r->child, msg);
        JS_FreeValue(r->child, exc);
        return te;
    }
    return realm_wrap_value(ctx, r, res, TRUE);
}

static JSValue
ns_shadowrealm_importValue(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    ns_shadowrealm *r = JS_GetOpaque2(ctx, this_val, ns_shadowrealm_class_id);
    if (!r) return JS_EXCEPTION;
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message",
        JS_NewString(ctx, "ShadowRealm.prototype.importValue is not supported"));
    JSValue ret = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, (JSValueConst *)&err);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

static void
realm_bind(JSContext *ctx, JSValueConst obj, const char *name,
           JSCFunction *fn, int argc)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, argc));
}

void
ns_js_realm_install(JSContext *ctx, JSValueConst global)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    JSAtom sr_atom = JS_NewAtom(ctx, "ShadowRealm");
    int has_sr = JS_HasProperty(ctx, global, sr_atom);
    JS_FreeAtom(ctx, sr_atom);
    if (has_sr <= 0) {
        ns_new_class_id(&ns_shadowrealm_class_id);
        if (!JS_IsRegisteredClass(rt, ns_shadowrealm_class_id))
            JS_NewClass(rt, ns_shadowrealm_class_id, &ns_shadowrealm_class);
        ns_new_class_id(&ns_shadowrealm_handle_class_id);
        if (!JS_IsRegisteredClass(rt, ns_shadowrealm_handle_class_id))
            JS_NewClass(rt, ns_shadowrealm_handle_class_id,
                        &ns_shadowrealm_handle_class);
        JSValue ctor = JS_NewCFunction2(ctx, ns_shadowrealm_ctor, "ShadowRealm",
                                        0, JS_CFUNC_constructor, 0);
        JSValue proto = JS_NewObject(ctx);
        realm_bind(ctx, proto, "evaluate", ns_shadowrealm_evaluate, 1);
        realm_bind(ctx, proto, "importValue", ns_shadowrealm_importValue, 2);
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
        JS_SetPropertyStr(ctx, global, "ShadowRealm", ctor);
    }
}
