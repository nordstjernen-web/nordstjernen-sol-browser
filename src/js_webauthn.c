/* Nordstjernen — WebAuthn requests from the top-level document to the shell's authenticators.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "js_internal.h"

#include <string.h>

typedef struct ns_webauthn_pending {
    JSContext *ctx;
    JSValue    resolve;
    JSValue    reject;
} ns_webauthn_pending;

static GString *g_webauthn_outbox;
static int      g_webauthn_support;

void
ns_js_webauthn_set_support(int flags)
{
    g_webauthn_support = flags;
}

gboolean
ns_js_webauthn_pending(void)
{
    return g_webauthn_outbox && g_webauthn_outbox->len > 0;
}

char *
ns_js_webauthn_take_outbox(void)
{
    if (!ns_js_webauthn_pending()) return NULL;
    char *out = g_strdup_printf("[%s]", g_webauthn_outbox->str);
    g_string_truncate(g_webauthn_outbox, 0);
    return out;
}

static void
ns_webauthn_outbox_append(const char *json)
{
    if (!g_webauthn_outbox) g_webauthn_outbox = g_string_new(NULL);
    if (g_webauthn_outbox->len) g_string_append_c(g_webauthn_outbox, ',');
    g_string_append(g_webauthn_outbox, json);
}

static void
ns_webauthn_outbox_cancel(guint id)
{
    char line[64];
    g_snprintf(line, sizeof line, "{\"id\":%u,\"cancel\":true}", id);
    ns_webauthn_outbox_append(line);
}

static void
ns_webauthn_pending_free(gpointer data)
{
    ns_webauthn_pending *p = data;
    JS_FreeValue(p->ctx, p->resolve);
    JS_FreeValue(p->ctx, p->reject);
    g_free(p);
}

static GHashTable *
ns_webauthn_table(ns_js *js)
{
    if (!js->webauthn_pending)
        js->webauthn_pending = g_hash_table_new_full(
            g_direct_hash, g_direct_equal, NULL, ns_webauthn_pending_free);
    return js->webauthn_pending;
}

static JSValue
ns_webauthn_dom_error(JSContext *ctx, const char *name, const char *message)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "DOMException");
    JSValue args[2] = { JS_NewString(ctx, message), JS_NewString(ctx, name) };
    JSValue err = JS_IsConstructor(ctx, ctor)
        ? JS_CallConstructor(ctx, ctor, 2, args)
        : JS_NewError(ctx);
    if (JS_IsException(err)) err = JS_GetException(ctx);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    return err;
}

static JSValue
ns_webauthn_rejected(JSContext *ctx, const char *name, const char *message)
{
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;
    JSValue err = ns_webauthn_dom_error(ctx, name, message);
    JSValue r = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

static JSValue
ns_webauthn_ticket(JSContext *ctx, guint id, JSValue promise)
{
    JSValue ticket = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ticket, "id", JS_NewUint32(ctx, id));
    JS_SetPropertyStr(ctx, ticket, "promise", promise);
    return ticket;
}

static gboolean
ns_webauthn_top_level_realm(ns_js *js, JSContext *ctx)
{
    return js->main_realm_ctx ? ctx == js->main_realm_ctx : ctx == js->ctx;
}

static JSValue
ns_window_webauthn_request(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    ns_js *js = js_from_ctx(ctx);
    if (!js || argc < 2) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    gboolean known = type && (strcmp(type, "create") == 0 ||
                              strcmp(type, "get") == 0);
    char *kind = known ? g_strdup(type) : NULL;
    JS_FreeCString(ctx, type);
    if (!kind) return JS_ThrowTypeError(ctx, "unknown WebAuthn request");

    JSValue promise;
    guint id = ++js->webauthn_next_id;
    const char *origin = js->document_origin;
    if (!ns_webauthn_top_level_realm(js, ctx)) {
        promise = ns_webauthn_rejected(ctx, "NotAllowedError",
            "Web Authentication is not available in frames.");
    } else if (!origin || !*origin || strcmp(origin, "null") == 0) {
        promise = ns_webauthn_rejected(ctx, "NotAllowedError",
            "The document's origin is opaque.");
    } else {
        const char *text = JS_ToCString(ctx, argv[1]);
        JSValue options = text
            ? JS_ParseJSON(ctx, text, strlen(text), "<webauthn>")
            : JS_EXCEPTION;
        JS_FreeCString(ctx, text);
        if (JS_IsException(options) || !JS_IsObject(options)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, options);
            g_free(kind);
            return JS_ThrowTypeError(ctx, "invalid WebAuthn options");
        }
        JSValue envelope = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, envelope, "id", JS_NewUint32(ctx, id));
        JS_SetPropertyStr(ctx, envelope, "type", JS_NewString(ctx, kind));
        JS_SetPropertyStr(ctx, envelope, "origin", JS_NewString(ctx, origin));
        JS_SetPropertyStr(ctx, envelope, "options", options);
        JSValue json = JS_JSONStringify(ctx, envelope, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue(ctx, envelope);
        const char *line = JS_IsString(json) ? JS_ToCString(ctx, json) : NULL;
        JS_FreeValue(ctx, json);
        if (!line) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            g_free(kind);
            return JS_ThrowTypeError(ctx, "invalid WebAuthn options");
        }
        JSValue resolving[2];
        promise = JS_NewPromiseCapability(ctx, resolving);
        if (JS_IsException(promise)) {
            JS_FreeCString(ctx, line);
            g_free(kind);
            return promise;
        }
        ns_webauthn_pending *p = g_new0(ns_webauthn_pending, 1);
        p->ctx = ctx;
        p->resolve = resolving[0];
        p->reject = resolving[1];
        g_hash_table_insert(ns_webauthn_table(js), GUINT_TO_POINTER(id), p);
        ns_webauthn_outbox_append(line);
        JS_FreeCString(ctx, line);
    }
    g_free(kind);
    return ns_webauthn_ticket(ctx, id, promise);
}

static JSValue
ns_window_webauthn_cancel(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    ns_js *js = js_from_ctx(ctx);
    uint32_t id = 0;
    if (!js || argc < 1 || JS_ToUint32(ctx, &id, argv[0]) < 0)
        return JS_UNDEFINED;
    if (js->webauthn_pending &&
        g_hash_table_remove(js->webauthn_pending, GUINT_TO_POINTER(id)))
        ns_webauthn_outbox_cancel(id);
    return JS_UNDEFINED;
}

static JSValue
ns_window_webauthn_platform(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NewBool(ctx, (g_webauthn_support & NS_WEBAUTHN_PLATFORM) != 0);
}

void
ns_js_webauthn_install(JSContext *ctx, JSValueConst global)
{
    ns_bind_fn(ctx, global, "__ndWebAuthnRequest", ns_window_webauthn_request, 2);
    ns_bind_fn(ctx, global, "__ndWebAuthnCancel", ns_window_webauthn_cancel, 1);
    ns_bind_fn(ctx, global, "__ndWebAuthnPlatform", ns_window_webauthn_platform, 0);
}

static void
ns_webauthn_settle(ns_js *js, JSContext *ctx, JSValueConst item)
{
    uint32_t id = 0;
    JSValue idv = JS_GetPropertyStr(ctx, item, "id");
    gboolean have_id = JS_ToUint32(ctx, &id, idv) == 0;
    JS_FreeValue(ctx, idv);
    ns_webauthn_pending *p = have_id && js->webauthn_pending
        ? g_hash_table_lookup(js->webauthn_pending, GUINT_TO_POINTER(id))
        : NULL;
    if (!p) return;
    JSContext *pctx = p->ctx;
    JSValue credential = JS_GetPropertyStr(ctx, item, "credential");
    JSValue arg;
    JSValue settle;
    if (pctx != ctx) {
        JS_FreeValue(ctx, credential);
        g_hash_table_remove(js->webauthn_pending, GUINT_TO_POINTER(id));
        return;
    }
    if (JS_IsObject(credential)) {
        arg = JS_DupValue(ctx, credential);
        settle = JS_DupValue(pctx, p->resolve);
    } else {
        JSValue name = JS_GetPropertyStr(ctx, item, "error");
        JSValue message = JS_GetPropertyStr(ctx, item, "message");
        const char *n = JS_IsString(name) ? JS_ToCString(ctx, name) : NULL;
        const char *m = JS_IsString(message) ? JS_ToCString(ctx, message) : NULL;
        arg = ns_webauthn_dom_error(pctx, n ? n : "NotAllowedError",
            m ? m : "The operation either timed out or was not allowed.");
        if (n) JS_FreeCString(ctx, n);
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, message);
        settle = JS_DupValue(pctx, p->reject);
    }
    JS_FreeValue(ctx, credential);
    if (JS_IsException(arg)) arg = JS_GetException(pctx);
    g_hash_table_remove(js->webauthn_pending, GUINT_TO_POINTER(id));
    JSValue r = JS_Call(pctx, settle, JS_UNDEFINED, 1, &arg);
    if (JS_IsException(r)) JS_FreeValue(pctx, JS_GetException(pctx));
    JS_FreeValue(pctx, r);
    JS_FreeValue(pctx, arg);
    JS_FreeValue(pctx, settle);
}

void
ns_js_webauthn_deliver(ns_js *js, const char *json)
{
    if (!js || !json || !js->webauthn_pending) return;
    JSContext *ctx = js->main_realm_ctx ? js->main_realm_ctx : js->ctx;
    JSValue items = JS_ParseJSON(ctx, json, strlen(json), "<webauthn>");
    if (JS_IsException(items)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    uint32_t n = 0;
    if (JS_IsArray(items)) {
        JSValue len = JS_GetPropertyStr(ctx, items, "length");
        JS_ToUint32(ctx, &n, len);
        JS_FreeValue(ctx, len);
    }
    for (uint32_t i = 0; i < n; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, items, i);
        if (JS_IsObject(item)) ns_webauthn_settle(js, ctx, item);
        JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, items);
    ns_js_microtask_checkpoint(js);
}

void
ns_js_webauthn_reset(ns_js *js)
{
    if (!js || !js->webauthn_pending) return;
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, js->webauthn_pending);
    while (g_hash_table_iter_next(&it, &key, NULL))
        ns_webauthn_outbox_cancel(GPOINTER_TO_UINT(key));
    g_clear_pointer(&js->webauthn_pending, g_hash_table_destroy);
}
