/* Nordstjernen — minimal WebSocket client (libcurl-backed).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_WS_H
#define NS_WS_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct ns_ws ns_ws;

typedef enum {
    NS_WS_STATE_CONNECTING = 0,
    NS_WS_STATE_OPEN       = 1,
    NS_WS_STATE_CLOSING    = 2,
    NS_WS_STATE_CLOSED     = 3,
} ns_ws_state;

typedef struct ns_ws_callbacks {
    void (*on_open)   (gpointer user_data);
    void (*on_text)   (const char *text, gsize len, gpointer user_data);
    void (*on_binary) (const guint8 *data, gsize len, gpointer user_data);
    void (*on_close)  (int code, const char *reason, gboolean clean, gpointer user_data);
    void (*on_error)  (const char *message, gpointer user_data);
    gboolean (*busy)  (gpointer user_data);
} ns_ws_callbacks;

ns_ws *ns_ws_new(const char        *url,
                 const char        *origin,
                 const char        *top_url,
                 const char *const *protocols,
                 const ns_ws_callbacks *cbs,
                 gpointer           user_data);

gboolean ns_ws_send_text(ns_ws *ws, const char *text, gsize len);
gboolean ns_ws_send_binary(ns_ws *ws, const guint8 *data, gsize len);

void     ns_ws_close(ns_ws *ws, int code, const char *reason);

int      ns_ws_state_get(ns_ws *ws);

const char *ns_ws_protocol(ns_ws *ws);

void     ns_ws_free(ns_ws *ws);

G_END_DECLS

#endif
