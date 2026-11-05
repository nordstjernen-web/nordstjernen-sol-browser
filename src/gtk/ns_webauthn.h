/* Nordstjernen — Web Authentication requests from a tab, validated in the shell and run on the platform authenticator. */

#ifndef NORDSTJERNEN_GTK_WEBAUTHN_H
#define NORDSTJERNEN_GTK_WEBAUTHN_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct NsWebAuthn NsWebAuthn;
typedef void (*NsWebAuthnDeliver)(gpointer owner, const char *json);

NsWebAuthn *ns_webauthn_new(GtkWidget *anchor, NsWebAuthnDeliver deliver,
                            gpointer owner);
void        ns_webauthn_free(NsWebAuthn *w);
void        ns_webauthn_handle(NsWebAuthn *w, const char *page_url,
                               const char *requests_json);
void        ns_webauthn_cancel_all(NsWebAuthn *w);
int         ns_webauthn_support_flags(void);

G_END_DECLS

#endif
