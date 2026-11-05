/* Nordstjernen — Web Authentication requests from a tab, validated in the shell and run on the platform authenticator. */

#include "ns_webauthn.h"
#include "webauthn_platform.h"
#include "procview.h"
#include "rjson.h"

#include <libpsl.h>
#include <string.h>

#ifdef GDK_WINDOWING_WIN32
#include <gdk/win32/gdkwin32.h>
#endif

typedef struct NsWebAuthnOp {
    gint            ref;
    NsWebAuthn     *owner;
    guint           id;
    RjValue        *request;
    NsWebAuthnCall  call;
    gboolean        cred_props;
} NsWebAuthnOp;

struct NsWebAuthn {
    GtkWidget         *anchor;
    NsWebAuthnDeliver  deliver;
    gpointer           owner;
    NsWebAuthnOp      *active;
};

GBytes *
ns_webauthn_b64url_decode(const char *text)
{
    if (!text) return NULL;
    gsize n = strlen(text);
    GString *s = g_string_sized_new(n + 4);
    for (gsize i = 0; i < n; i++) {
        char c = text[i];
        if (g_ascii_isalnum(c)) g_string_append_c(s, c);
        else if (c == '-') g_string_append_c(s, '+');
        else if (c == '_') g_string_append_c(s, '/');
        else {
            g_string_free(s, TRUE);
            return NULL;
        }
    }
    if (s->len % 4 == 1) {
        g_string_free(s, TRUE);
        return NULL;
    }
    while (s->len % 4) g_string_append_c(s, '=');
    gsize len = 0;
    guchar *data = s->len ? g_base64_decode(s->str, &len) : g_malloc0(1);
    g_string_free(s, TRUE);
    return g_bytes_new_take(data, len);
}

char *
ns_webauthn_b64url_encode(const guint8 *data, gsize len)
{
    if (!len) return g_strdup("");
    char *b64 = g_base64_encode(data, len);
    char *w = b64;
    for (char *r = b64; *r; r++) {
        if (*r == '=') continue;
        *w++ = *r == '+' ? '-' : *r == '/' ? '_' : *r;
    }
    *w = '\0';
    return b64;
}

void
ns_webauthn_call_fail(NsWebAuthnCall *call, const char *error,
                      const char *message)
{
    g_free(call->error);
    g_free(call->message);
    call->error = g_strdup(error);
    call->message = g_strdup(message);
}

int
ns_webauthn_support_flags(void)
{
    return ns_webauthn_platform_uv_available() ? 1 : 0;
}

static void
op_unref(NsWebAuthnOp *op)
{
    if (!g_atomic_int_dec_and_test(&op->ref)) return;
    rj_free(op->request);
    g_free(op->call.rp_id);
    g_clear_pointer(&op->call.client_data, g_bytes_unref);
    g_free(op->call.error);
    g_free(op->call.message);
    g_clear_pointer(&op->call.raw_id, g_bytes_unref);
    g_clear_pointer(&op->call.transports, g_ptr_array_unref);
    g_clear_pointer(&op->call.attestation_object, g_bytes_unref);
    g_clear_pointer(&op->call.authenticator_data, g_bytes_unref);
    g_clear_pointer(&op->call.signature, g_bytes_unref);
    g_clear_pointer(&op->call.user_handle, g_bytes_unref);
    g_mutex_clear(&op->call.lock);
    g_free(op);
}

static void
deliver_error(NsWebAuthn *w, guint id, const char *error, const char *message)
{
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "[{\"id\":%u,\"error\":", id);
    rj_append_str(out, error);
    g_string_append(out, ",\"message\":");
    rj_append_str(out, message);
    g_string_append(out, "}]");
    w->deliver(w->owner, out->str);
    g_string_free(out, TRUE);
}

static void
append_b64(GString *out, const char *key, GBytes *bytes)
{
    gsize len = 0;
    const guint8 *data = bytes ? g_bytes_get_data(bytes, &len) : NULL;
    char *text = ns_webauthn_b64url_encode(data, len);
    g_string_append_printf(out, "\"%s\":", key);
    rj_append_str(out, text);
    g_free(text);
}

static char *
credential_json(NsWebAuthnOp *op)
{
    NsWebAuthnCall *c = &op->call;
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "[{\"id\":%u,\"credential\":{", op->id);
    append_b64(out, "id", c->raw_id);
    g_string_append_c(out, ',');
    append_b64(out, "rawId", c->raw_id);
    g_string_append(out, ",\"type\":\"public-key\",\"authenticatorAttachment\":");
    if (c->attachment) rj_append_str(out, c->attachment);
    else g_string_append(out, "null");
    g_string_append(out, ",\"response\":{");
    append_b64(out, "clientDataJSON", c->client_data);
    if (c->create) {
        g_string_append_c(out, ',');
        append_b64(out, "attestationObject", c->attestation_object);
        g_string_append(out, ",\"transports\":[");
        for (guint i = 0; c->transports && i < c->transports->len; i++) {
            if (i) g_string_append_c(out, ',');
            rj_append_str(out, g_ptr_array_index(c->transports, i));
        }
        g_string_append_c(out, ']');
    } else {
        g_string_append_c(out, ',');
        append_b64(out, "authenticatorData", c->authenticator_data);
        g_string_append_c(out, ',');
        append_b64(out, "signature", c->signature);
        g_string_append(out, ",\"userHandle\":");
        if (c->user_handle && g_bytes_get_size(c->user_handle)) {
            gsize len = 0;
            const guint8 *d = g_bytes_get_data(c->user_handle, &len);
            char *text = ns_webauthn_b64url_encode(d, len);
            rj_append_str(out, text);
            g_free(text);
        } else {
            g_string_append(out, "null");
        }
    }
    g_string_append(out, "},\"clientExtensionResults\":{");
    if (c->create && op->cred_props) {
        g_string_append(out, "\"credProps\":{");
        if (c->resident_key_known)
            g_string_append_printf(out, "\"rk\":%s",
                                   c->resident_key ? "true" : "false");
        g_string_append_c(out, '}');
    }
    g_string_append(out, "}}}]");
    return g_string_free(out, FALSE);
}

static gboolean
op_finished(gpointer data)
{
    NsWebAuthnOp *op = data;
    NsWebAuthn *w = op->owner;
    if (w && w->active == op) {
        w->active = NULL;
        if (op->call.error || !op->call.raw_id) {
            deliver_error(w, op->id,
                          op->call.error ? op->call.error : "NotAllowedError",
                          op->call.message ? op->call.message
                              : "The operation either timed out or was not allowed.");
        } else {
            char *json = credential_json(op);
            w->deliver(w->owner, json);
            g_free(json);
        }
        op_unref(op);
    }
    op_unref(op);
    return G_SOURCE_REMOVE;
}

static gpointer
op_thread(gpointer data)
{
    NsWebAuthnOp *op = data;
    ns_webauthn_platform_run(&op->call);
    g_idle_add(op_finished, op);
    return NULL;
}

static gboolean
origin_is_trustworthy(const char *scheme, const char *host)
{
    if (g_ascii_strcasecmp(scheme, "https") == 0) return TRUE;
    return g_ascii_strcasecmp(scheme, "http") == 0 &&
           (g_ascii_strcasecmp(host, "localhost") == 0 ||
            g_str_has_suffix(host, ".localhost"));
}

static gboolean
rp_id_allowed(const char *host, const char *rp_id)
{
    if (!rp_id || !*rp_id) return FALSE;
    if (g_ascii_strcasecmp(host, rp_id) == 0) return TRUE;
    gsize hl = strlen(host), rl = strlen(rp_id);
    if (rl >= hl || host[hl - rl - 1] != '.' ||
        g_ascii_strcasecmp(host + hl - rl, rp_id) != 0)
        return FALSE;
    static const psl_ctx_t *psl;
    if (!psl) psl = psl_latest(NULL);
    if (!psl) psl = psl_builtin();
    return psl && !psl_is_public_suffix2(psl, rp_id,
                                         PSL_TYPE_ANY | PSL_TYPE_NO_STAR_RULE);
}

static gboolean
challenge_valid(const char *challenge)
{
    if (!challenge || !*challenge) return FALSE;
    for (const char *p = challenge; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') return FALSE;
    return TRUE;
}

static GBytes *
client_data_json(gboolean create, const char *challenge, const char *origin)
{
    GString *out = g_string_new("{\"type\":");
    rj_append_str(out, create ? "webauthn.create" : "webauthn.get");
    g_string_append(out, ",\"challenge\":");
    rj_append_str(out, challenge);
    g_string_append(out, ",\"origin\":");
    rj_append_str(out, origin);
    g_string_append(out, ",\"crossOrigin\":false}");
    gsize len = out->len;
    return g_bytes_new_take(g_string_free(out, FALSE), len);
}

static gpointer
anchor_native_window(GtkWidget *anchor)
{
#ifdef GDK_WINDOWING_WIN32
    GtkNative *native = anchor ? gtk_widget_get_native(anchor) : NULL;
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    if (surface && GDK_IS_WIN32_SURFACE(surface))
        return gdk_win32_surface_get_handle(surface);
#else
    (void)anchor;
#endif
    return NULL;
}

static void
start_request(NsWebAuthn *w, const char *page_url, RjValue *req, guint id)
{
    const char *type = rj_get_str(req, "type");
    const char *origin = rj_get_str(req, "origin");
    const RjValue *options = rj_get(req, "options");
    gboolean create = g_strcmp0(type, "create") == 0;
    if ((!create && g_strcmp0(type, "get") != 0) || !origin || !options ||
        options->type != RJ_OBJ) {
        deliver_error(w, id, "NotSupportedError", "Malformed request.");
        rj_free(req);
        return;
    }
    char *page_origin = ns_proc_url_origin(page_url);
    gboolean same = page_origin && g_ascii_strcasecmp(page_origin, origin) == 0;
    g_free(page_origin);
    GUri *uri = same ? g_uri_parse(origin, G_URI_FLAGS_NONE, NULL) : NULL;
    const char *scheme = uri ? g_uri_get_scheme(uri) : NULL;
    const char *host = uri ? g_uri_get_host(uri) : NULL;
    const char *error = NULL, *message = NULL;
    if (!same || !scheme || !host || !*host) {
        error = "NotAllowedError";
        message = "The request does not come from the page shown in this tab.";
    } else if (!origin_is_trustworthy(scheme, host)) {
        error = "NotAllowedError";
        message = "Web Authentication needs a secure context.";
    } else if (g_hostname_is_ip_address(host)) {
        error = "SecurityError";
        message = "This is an invalid domain.";
    } else if (w->active) {
        error = "NotAllowedError";
        message = "Another Web Authentication request is in progress.";
    }
    const char *rp_id = NULL;
    if (!error) {
        const RjValue *rp = create ? rj_get(options, "rp") : NULL;
        rp_id = create ? (rp ? rj_get_str(rp, "id") : NULL)
                       : rj_get_str(options, "rpId");
        if (!rp_id) rp_id = host;
        if (!rp_id_allowed(host, rp_id)) {
            error = "SecurityError";
            message = "The relying party ID is not a registrable domain suffix "
                      "of, nor equal to the current domain.";
        } else if (!challenge_valid(rj_get_str(options, "challenge"))) {
            error = "NotAllowedError";
            message = "The challenge is missing or malformed.";
        }
    }
    if (error) {
        deliver_error(w, id, error, message);
        if (uri) g_uri_unref(uri);
        rj_free(req);
        return;
    }
    NsWebAuthnOp *op = g_new0(NsWebAuthnOp, 1);
    op->ref = 2;
    op->owner = w;
    op->id = id;
    op->request = req;
    g_mutex_init(&op->call.lock);
    op->call.create = create;
    op->call.rp_id = g_ascii_strdown(rp_id, -1);
    op->call.options = options;
    op->call.client_data = client_data_json(create,
                                            rj_get_str(options, "challenge"),
                                            origin);
    op->call.window = anchor_native_window(w->anchor);
    const RjValue *ext = rj_get(options, "extensions");
    op->cred_props = ext && ext->type == RJ_OBJ &&
                     rj_get_bool(ext, "credProps", FALSE);
    g_uri_unref(uri);
    w->active = op;
    GThread *t = g_thread_try_new("webauthn", op_thread, op, NULL);
    if (t) {
        g_thread_unref(t);
    } else {
        ns_webauthn_call_fail(&op->call, "NotAllowedError",
                              "The authenticator could not be reached.");
        g_idle_add(op_finished, op);
    }
}

void
ns_webauthn_handle(NsWebAuthn *w, const char *page_url,
                   const char *requests_json)
{
    if (!w || !requests_json) return;
    RjValue *list = rj_parse(requests_json, strlen(requests_json));
    if (!list || list->type != RJ_ARR) {
        rj_free(list);
        return;
    }
    for (guint i = 0; i < list->items->len; i++) {
        RjValue *item = g_ptr_array_index(list->items, i);
        double num = 0;
        if (!item || item->type != RJ_OBJ || !rj_get_num(item, "id", &num) ||
            num < 1 || num > G_MAXUINT)
            continue;
        guint id = (guint)num;
        if (rj_get_bool(item, "cancel", FALSE)) {
            if (w->active && w->active->id == id)
                ns_webauthn_platform_cancel(&w->active->call);
            continue;
        }
        g_ptr_array_index(list->items, i) = NULL;
        start_request(w, page_url, item, id);
    }
    rj_free(list);
}

void
ns_webauthn_cancel_all(NsWebAuthn *w)
{
    if (w && w->active)
        ns_webauthn_platform_cancel(&w->active->call);
}

NsWebAuthn *
ns_webauthn_new(GtkWidget *anchor, NsWebAuthnDeliver deliver, gpointer owner)
{
    NsWebAuthn *w = g_new0(NsWebAuthn, 1);
    w->anchor = anchor;
    w->deliver = deliver;
    w->owner = owner;
    return w;
}

void
ns_webauthn_free(NsWebAuthn *w)
{
    if (!w) return;
    if (w->active) {
        NsWebAuthnOp *op = w->active;
        w->active = NULL;
        op->owner = NULL;
        ns_webauthn_platform_cancel(&op->call);
        op_unref(op);
    }
    g_free(w);
}

#if !defined(G_OS_WIN32)
gboolean
ns_webauthn_platform_available(void)
{
    return FALSE;
}

gboolean
ns_webauthn_platform_uv_available(void)
{
    return FALSE;
}

void
ns_webauthn_platform_run(NsWebAuthnCall *call)
{
    ns_webauthn_call_fail(call, "NotAllowedError",
                          "No authenticator is available.");
}

void
ns_webauthn_platform_cancel(NsWebAuthnCall *call)
{
    (void)call;
}
#endif
