/* Nordstjernen — Windows platform authenticator for Web Authentication, through webauthn.dll loaded at runtime. */

#include "webauthn_platform.h"

#include <windows.h>
#include <webauthn.h>
#include <string.h>

#ifndef WEBAUTHN_CTAP_TRANSPORT_HYBRID
#define WEBAUTHN_CTAP_TRANSPORT_HYBRID 0x00000020
#endif

typedef DWORD   (WINAPI *WaApiVersion)(void);
typedef HRESULT (WINAPI *WaIsUvpaa)(BOOL *);
typedef HRESULT (WINAPI *WaMakeCredential)(HWND, PCWEBAUTHN_RP_ENTITY_INFORMATION,
    PCWEBAUTHN_USER_ENTITY_INFORMATION, PCWEBAUTHN_COSE_CREDENTIAL_PARAMETERS,
    PCWEBAUTHN_CLIENT_DATA, PCWEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS,
    PWEBAUTHN_CREDENTIAL_ATTESTATION *);
typedef HRESULT (WINAPI *WaGetAssertion)(HWND, LPCWSTR, PCWEBAUTHN_CLIENT_DATA,
    PCWEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS, PWEBAUTHN_ASSERTION *);
typedef void    (WINAPI *WaFreeAttestation)(PWEBAUTHN_CREDENTIAL_ATTESTATION);
typedef void    (WINAPI *WaFreeAssertion)(PWEBAUTHN_ASSERTION);
typedef HRESULT (WINAPI *WaGetCancellationId)(GUID *);
typedef HRESULT (WINAPI *WaCancel)(const GUID *);
typedef PCWSTR  (WINAPI *WaErrorName)(HRESULT);

static struct {
    HMODULE             dll;
    DWORD               api;
    WaIsUvpaa           is_uvpaa;
    WaMakeCredential    make_credential;
    WaGetAssertion      get_assertion;
    WaFreeAttestation   free_attestation;
    WaFreeAssertion     free_assertion;
    WaGetCancellationId get_cancellation_id;
    WaCancel            cancel;
    WaErrorName         error_name;
} wa;

static gboolean
wa_load(void)
{
    static gsize once;
    if (g_once_init_enter(&once)) {
        HMODULE dll = LoadLibraryExW(L"webauthn.dll", NULL,
                                     LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dll) {
            WaApiVersion version =
                (WaApiVersion)(void *)GetProcAddress(dll, "WebAuthNGetApiVersionNumber");
            wa.api = version ? version() : 1;
            wa.is_uvpaa = (WaIsUvpaa)(void *)GetProcAddress(dll,
                "WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable");
            wa.make_credential = (WaMakeCredential)(void *)GetProcAddress(dll,
                "WebAuthNAuthenticatorMakeCredential");
            wa.get_assertion = (WaGetAssertion)(void *)GetProcAddress(dll,
                "WebAuthNAuthenticatorGetAssertion");
            wa.free_attestation = (WaFreeAttestation)(void *)GetProcAddress(dll,
                "WebAuthNFreeCredentialAttestation");
            wa.free_assertion = (WaFreeAssertion)(void *)GetProcAddress(dll,
                "WebAuthNFreeAssertion");
            wa.get_cancellation_id = (WaGetCancellationId)(void *)GetProcAddress(dll,
                "WebAuthNGetCancellationId");
            wa.cancel = (WaCancel)(void *)GetProcAddress(dll,
                "WebAuthNCancelCurrentOperation");
            wa.error_name = (WaErrorName)(void *)GetProcAddress(dll,
                "WebAuthNGetErrorName");
            if (wa.make_credential && wa.get_assertion && wa.free_attestation &&
                wa.free_assertion)
                wa.dll = dll;
            else
                FreeLibrary(dll);
        }
        g_once_init_leave(&once, 1);
    }
    return wa.dll != NULL;
}

gboolean
ns_webauthn_platform_available(void)
{
    return wa_load();
}

gboolean
ns_webauthn_platform_uv_available(void)
{
    BOOL available = FALSE;
    return wa_load() && wa.is_uvpaa &&
           SUCCEEDED(wa.is_uvpaa(&available)) && available;
}

typedef struct {
    GPtrArray *strings;
    GPtrArray *blobs;
} WaArena;

static LPCWSTR
wa_wide(WaArena *arena, const char *utf8)
{
    if (!utf8) return NULL;
    gunichar2 *w = g_utf8_to_utf16(utf8, -1, NULL, NULL, NULL);
    if (w) g_ptr_array_add(arena->strings, w);
    return (LPCWSTR)w;
}

static GBytes *
wa_bytes(WaArena *arena, const char *b64url)
{
    GBytes *bytes = ns_webauthn_b64url_decode(b64url);
    if (bytes) g_ptr_array_add(arena->blobs, bytes);
    return bytes;
}

static DWORD
wa_transport_mask(const RjValue *list)
{
    DWORD mask = 0;
    for (guint i = 0; list && list->type == RJ_ARR && i < list->items->len; i++) {
        const RjValue *t = g_ptr_array_index(list->items, i);
        const char *s = t && t->type == RJ_STR ? t->str : "";
        if (strcmp(s, "usb") == 0) mask |= WEBAUTHN_CTAP_TRANSPORT_USB;
        else if (strcmp(s, "nfc") == 0) mask |= WEBAUTHN_CTAP_TRANSPORT_NFC;
        else if (strcmp(s, "ble") == 0) mask |= WEBAUTHN_CTAP_TRANSPORT_BLE;
        else if (strcmp(s, "internal") == 0) mask |= WEBAUTHN_CTAP_TRANSPORT_INTERNAL;
        else if (strcmp(s, "hybrid") == 0) mask |= WEBAUTHN_CTAP_TRANSPORT_HYBRID;
    }
    return mask;
}

typedef struct {
    WEBAUTHN_CREDENTIAL      *plain;
    WEBAUTHN_CREDENTIAL_EX   *ex;
    WEBAUTHN_CREDENTIAL_EX  **ex_ptrs;
    WEBAUTHN_CREDENTIALS      credentials;
    WEBAUTHN_CREDENTIAL_LIST  list;
} WaCredentials;

static void
wa_credentials(WaArena *arena, const RjValue *array, WaCredentials *out)
{
    memset(out, 0, sizeof *out);
    guint n = array && array->type == RJ_ARR ? array->items->len : 0;
    if (!n) return;
    out->plain = g_new0(WEBAUTHN_CREDENTIAL, n);
    out->ex = g_new0(WEBAUTHN_CREDENTIAL_EX, n);
    out->ex_ptrs = g_new0(WEBAUTHN_CREDENTIAL_EX *, n);
    DWORD count = 0;
    for (guint i = 0; i < n; i++) {
        const RjValue *c = g_ptr_array_index(array->items, i);
        GBytes *id = c && c->type == RJ_OBJ ? wa_bytes(arena, rj_get_str(c, "id")) : NULL;
        if (!id) continue;
        gsize len = 0;
        PBYTE data = (PBYTE)g_bytes_get_data(id, &len);
        out->plain[count] = (WEBAUTHN_CREDENTIAL){
            WEBAUTHN_CREDENTIAL_CURRENT_VERSION, (DWORD)len, data,
            WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY };
        out->ex[count] = (WEBAUTHN_CREDENTIAL_EX){
            WEBAUTHN_CREDENTIAL_EX_CURRENT_VERSION, (DWORD)len, data,
            WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY,
            wa_transport_mask(rj_get(c, "transports")) };
        out->ex_ptrs[count] = &out->ex[count];
        count++;
    }
    out->credentials.cCredentials = count;
    out->credentials.pCredentials = out->plain;
    out->list.cCredentials = count;
    out->list.ppCredentials = out->ex_ptrs;
}

static void
wa_credentials_clear(WaCredentials *c)
{
    g_free(c->plain);
    g_free(c->ex);
    g_free(c->ex_ptrs);
}

static DWORD
wa_user_verification(const char *uv)
{
    if (g_strcmp0(uv, "required") == 0)
        return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED;
    if (g_strcmp0(uv, "discouraged") == 0)
        return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_DISCOURAGED;
    return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED;
}

static DWORD
wa_timeout(const RjValue *options)
{
    double ms = 0;
    if (!rj_get_num(options, "timeout", &ms) || ms <= 0) ms = 300000;
    return (DWORD)CLAMP(ms, 1000, 600000);
}

static void
wa_fail_hr(NsWebAuthnCall *call, HRESULT hr)
{
    static const char *const names[] = {
        "NotAllowedError", "InvalidStateError", "ConstraintError",
        "NotSupportedError", "AbortError", "SecurityError", "UnknownError",
    };
    char *name = NULL;
    PCWSTR wide = wa.error_name ? wa.error_name(hr) : NULL;
    if (wide) name = g_utf16_to_utf8((const gunichar2 *)wide, -1, NULL, NULL, NULL);
    const char *error = "NotAllowedError";
    for (gsize i = 0; name && i < G_N_ELEMENTS(names); i++)
        if (strcmp(name, names[i]) == 0) error = names[i];
    g_free(name);
    const char *message = strcmp(error, "InvalidStateError") == 0
        ? "The authenticator already holds a credential for this account."
        : "The operation either timed out or was not allowed.";
    ns_webauthn_call_fail(call, error, message);
}

static gboolean
wa_begin(NsWebAuthnCall *call, GUID *guid, GUID **pguid)
{
    *pguid = NULL;
    g_mutex_lock(&call->lock);
    gboolean cancelled = call->cancelled;
    if (!cancelled && wa.get_cancellation_id &&
        SUCCEEDED(wa.get_cancellation_id(guid))) {
        call->cancel_token = guid;
        *pguid = guid;
    }
    g_mutex_unlock(&call->lock);
    if (cancelled)
        ns_webauthn_call_fail(call, "AbortError", "The operation was aborted.");
    return !cancelled;
}

static void
wa_end(NsWebAuthnCall *call)
{
    g_mutex_lock(&call->lock);
    call->cancel_token = NULL;
    g_mutex_unlock(&call->lock);
}

static void
wa_note_transport(NsWebAuthnCall *call, DWORD transport)
{
    const char *name = NULL;
    if (transport & WEBAUTHN_CTAP_TRANSPORT_INTERNAL) name = "internal";
    else if (transport & WEBAUTHN_CTAP_TRANSPORT_HYBRID) name = "hybrid";
    else if (transport & WEBAUTHN_CTAP_TRANSPORT_USB) name = "usb";
    else if (transport & WEBAUTHN_CTAP_TRANSPORT_NFC) name = "nfc";
    else if (transport & WEBAUTHN_CTAP_TRANSPORT_BLE) name = "ble";
    if (!name) return;
    call->attachment = strcmp(name, "internal") == 0 ? "platform" : "cross-platform";
    call->transports = g_ptr_array_new();
    g_ptr_array_add(call->transports, (gpointer)name);
}

static void
wa_make_credential(NsWebAuthnCall *call, WaArena *arena)
{
    const RjValue *o = call->options;
    const RjValue *rp = rj_get(o, "rp");
    const RjValue *user = rj_get(o, "user");
    GBytes *user_id = user ? wa_bytes(arena, rj_get_str(user, "id")) : NULL;
    if (!rp || !user || !user_id) {
        ns_webauthn_call_fail(call, "NotSupportedError", "Malformed request.");
        return;
    }
    WEBAUTHN_RP_ENTITY_INFORMATION rp_info = {
        WEBAUTHN_RP_ENTITY_INFORMATION_CURRENT_VERSION,
        wa_wide(arena, call->rp_id), wa_wide(arena, rj_get_str(rp, "name")), NULL };
    gsize uid_len = 0;
    PBYTE uid = (PBYTE)g_bytes_get_data(user_id, &uid_len);
    WEBAUTHN_USER_ENTITY_INFORMATION user_info = {
        WEBAUTHN_USER_ENTITY_INFORMATION_CURRENT_VERSION, (DWORD)uid_len, uid,
        wa_wide(arena, rj_get_str(user, "name")), NULL,
        wa_wide(arena, rj_get_str(user, "displayName")) };

    const RjValue *params = rj_get(o, "pubKeyCredParams");
    guint np = params && params->type == RJ_ARR ? params->items->len : 0;
    WEBAUTHN_COSE_CREDENTIAL_PARAMETER *param = g_new0(WEBAUTHN_COSE_CREDENTIAL_PARAMETER,
                                                       MAX(np, 1));
    DWORD count = 0;
    for (guint i = 0; i < np; i++) {
        const RjValue *p = g_ptr_array_index(params->items, i);
        double alg = 0;
        if (!p || p->type != RJ_OBJ || !rj_get_num(p, "alg", &alg)) continue;
        param[count++] = (WEBAUTHN_COSE_CREDENTIAL_PARAMETER){
            WEBAUTHN_COSE_CREDENTIAL_PARAMETER_CURRENT_VERSION,
            WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY, (LONG)alg };
    }
    WEBAUTHN_COSE_CREDENTIAL_PARAMETERS cose = { count, param };

    gsize cd_len = 0;
    PBYTE cd = (PBYTE)g_bytes_get_data(call->client_data, &cd_len);
    WEBAUTHN_CLIENT_DATA client = { WEBAUTHN_CLIENT_DATA_CURRENT_VERSION,
        (DWORD)cd_len, cd, WEBAUTHN_HASH_ALGORITHM_SHA_256 };

    WaCredentials exclude;
    wa_credentials(arena, rj_get(o, "excludeCredentials"), &exclude);
    const RjValue *sel = rj_get(o, "authenticatorSelection");
    const char *attach = sel ? rj_get_str(sel, "authenticatorAttachment") : NULL;
    const char *resident = sel ? rj_get_str(sel, "residentKey") : NULL;
    const char *attestation = rj_get_str(o, "attestation");

    WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS opts;
    memset(&opts, 0, sizeof opts);
    opts.dwVersion = wa.api >= 3 ? 4 : wa.api == 2 ? 3 : 2;
    opts.dwTimeoutMilliseconds = wa_timeout(o);
    opts.CredentialList = exclude.credentials;
    opts.dwAuthenticatorAttachment =
        g_strcmp0(attach, "platform") == 0 ? WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM :
        g_strcmp0(attach, "cross-platform") == 0 ? WEBAUTHN_AUTHENTICATOR_ATTACHMENT_CROSS_PLATFORM :
        WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
    opts.bRequireResidentKey = g_strcmp0(resident, "required") == 0;
    opts.bPreferResidentKey = g_strcmp0(resident, "preferred") == 0;
    opts.dwUserVerificationRequirement =
        wa_user_verification(sel ? rj_get_str(sel, "userVerification") : NULL);
    opts.dwAttestationConveyancePreference =
        g_strcmp0(attestation, "direct") == 0 || g_strcmp0(attestation, "enterprise") == 0
            ? WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_DIRECT :
        g_strcmp0(attestation, "indirect") == 0
            ? WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_INDIRECT :
        WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE;
    if (opts.dwVersion >= 3) opts.pExcludeCredentialList = &exclude.list;

    GUID guid;
    if (count == 0) {
        ns_webauthn_call_fail(call, "NotSupportedError",
                              "None of the requested algorithms is supported.");
    } else if (wa_begin(call, &guid, &opts.pCancellationId)) {
        PWEBAUTHN_CREDENTIAL_ATTESTATION att = NULL;
        HRESULT hr = wa.make_credential((HWND)call->window, &rp_info, &user_info,
                                        &cose, &client, &opts, &att);
        wa_end(call);
        if (SUCCEEDED(hr) && att && att->pbCredentialId && att->pbAttestationObject) {
            call->raw_id = g_bytes_new(att->pbCredentialId, att->cbCredentialId);
            call->attestation_object = g_bytes_new(att->pbAttestationObject,
                                                   att->cbAttestationObject);
            if (att->dwVersion >= WEBAUTHN_CREDENTIAL_ATTESTATION_VERSION_3)
                wa_note_transport(call, att->dwUsedTransport);
            if (att->dwVersion >= WEBAUTHN_CREDENTIAL_ATTESTATION_VERSION_4) {
                call->resident_key_known = TRUE;
                call->resident_key = att->bResidentKey != FALSE;
            }
        } else {
            wa_fail_hr(call, SUCCEEDED(hr) ? NTE_INVALID_PARAMETER : hr);
        }
        if (att) wa.free_attestation(att);
    }
    wa_credentials_clear(&exclude);
    g_free(param);
}

static void
wa_get_assertion(NsWebAuthnCall *call, WaArena *arena)
{
    const RjValue *o = call->options;
    gsize cd_len = 0;
    PBYTE cd = (PBYTE)g_bytes_get_data(call->client_data, &cd_len);
    WEBAUTHN_CLIENT_DATA client = { WEBAUTHN_CLIENT_DATA_CURRENT_VERSION,
        (DWORD)cd_len, cd, WEBAUTHN_HASH_ALGORITHM_SHA_256 };
    WaCredentials allow;
    wa_credentials(arena, rj_get(o, "allowCredentials"), &allow);

    WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS opts;
    memset(&opts, 0, sizeof opts);
    opts.dwVersion = wa.api >= 3 ? 5 : wa.api == 2 ? 4 : 3;
    opts.dwTimeoutMilliseconds = wa_timeout(o);
    opts.CredentialList = allow.credentials;
    opts.dwUserVerificationRequirement =
        wa_user_verification(rj_get_str(o, "userVerification"));
    if (opts.dwVersion >= 4) opts.pAllowCredentialList = &allow.list;

    GUID guid;
    if (wa_begin(call, &guid, &opts.pCancellationId)) {
        PWEBAUTHN_ASSERTION as = NULL;
        HRESULT hr = wa.get_assertion((HWND)call->window,
                                      wa_wide(arena, call->rp_id), &client,
                                      &opts, &as);
        wa_end(call);
        if (SUCCEEDED(hr) && as && as->Credential.pbId && as->pbAuthenticatorData &&
            as->pbSignature) {
            call->raw_id = g_bytes_new(as->Credential.pbId, as->Credential.cbId);
            call->authenticator_data = g_bytes_new(as->pbAuthenticatorData,
                                                   as->cbAuthenticatorData);
            call->signature = g_bytes_new(as->pbSignature, as->cbSignature);
            if (as->pbUserId && as->cbUserId)
                call->user_handle = g_bytes_new(as->pbUserId, as->cbUserId);
        } else {
            wa_fail_hr(call, SUCCEEDED(hr) ? NTE_INVALID_PARAMETER : hr);
        }
        if (as) wa.free_assertion(as);
    }
    wa_credentials_clear(&allow);
}

void
ns_webauthn_platform_run(NsWebAuthnCall *call)
{
    if (!wa_load()) {
        ns_webauthn_call_fail(call, "NotAllowedError",
                              "Windows Hello and security keys are not available.");
        return;
    }
    WaArena arena = {
        g_ptr_array_new_with_free_func(g_free),
        g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref),
    };
    if (call->create) wa_make_credential(call, &arena);
    else wa_get_assertion(call, &arena);
    g_ptr_array_unref(arena.strings);
    g_ptr_array_unref(arena.blobs);
}

void
ns_webauthn_platform_cancel(NsWebAuthnCall *call)
{
    g_mutex_lock(&call->lock);
    call->cancelled = TRUE;
    if (call->cancel_token && wa.cancel)
        wa.cancel(call->cancel_token);
    g_mutex_unlock(&call->lock);
}
