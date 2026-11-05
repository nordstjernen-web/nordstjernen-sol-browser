/* Nordstjernen — the platform authenticator interface behind the shell's Web Authentication requests. */

#ifndef NORDSTJERNEN_GTK_WEBAUTHN_PLATFORM_H
#define NORDSTJERNEN_GTK_WEBAUTHN_PLATFORM_H

#include <glib.h>

#include "rjson.h"

G_BEGIN_DECLS

typedef struct NsWebAuthnCall {
    gboolean       create;
    char          *rp_id;
    const RjValue *options;
    GBytes        *client_data;
    gpointer       window;

    GMutex         lock;
    gboolean       cancelled;
    gpointer       cancel_token;

    char          *error;
    char          *message;
    GBytes        *raw_id;
    const char    *attachment;
    GPtrArray     *transports;
    GBytes        *attestation_object;
    GBytes        *authenticator_data;
    GBytes        *signature;
    GBytes        *user_handle;
    gboolean       resident_key_known;
    gboolean       resident_key;
} NsWebAuthnCall;

gboolean ns_webauthn_platform_available(void);
gboolean ns_webauthn_platform_uv_available(void);
void     ns_webauthn_platform_run(NsWebAuthnCall *call);
void     ns_webauthn_platform_cancel(NsWebAuthnCall *call);

void     ns_webauthn_call_fail(NsWebAuthnCall *call, const char *error,
                               const char *message);
GBytes  *ns_webauthn_b64url_decode(const char *text);
char    *ns_webauthn_b64url_encode(const guint8 *data, gsize len);

G_END_DECLS

#endif
