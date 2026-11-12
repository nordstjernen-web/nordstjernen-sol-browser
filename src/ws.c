/* Nordstjernen — WebSocket client over libcurl, framing in-tree when libcurl has no WebSocket support.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "ws.h"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <openssl/rand.h>

#include <string.h>

#include "net.h"

#define NS_WS_RECV_BUF       8192
#define NS_WS_MAX_MESSAGE    (8 * 1024 * 1024)
#define NS_WS_POLL_MSEC      10
#define NS_WS_MAX_PENDING    256
#define NS_WS_MAX_HANDSHAKE  16384
#define NS_WS_HANDSHAKE_USEC (15 * G_USEC_PER_SEC)
#define NS_WS_ACCEPT_GUID    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

typedef enum {
    NS_WS_OUT_TEXT,
    NS_WS_OUT_BINARY,
    NS_WS_OUT_CLOSE,
} ns_ws_out_kind;

typedef struct {
    ns_ws_out_kind kind;
    guint8        *data;
    gsize          len;
    int            close_code;
} ns_ws_out_msg;

struct ns_ws {
    volatile gint     refcount;

    char             *url;
    char             *origin;
    char             *top_url;
    GPtrArray        *protocols;
    char             *protocol;

    ns_ws_callbacks   cbs;
    gpointer          user_data;

    GMutex            lock;
    GCond             cond;
    GQueue            out_queue;
    volatile gint     state;
    volatile gint     exit_requested;
    volatile gint     detached;
    volatile gint     pending;

    GThread          *thread;

    GByteArray       *recv_assembly;
    int               recv_assembly_kind;
    gboolean          recv_in_message;

    gboolean          raw;
    GByteArray       *raw_in;
    gboolean          raw_in_frame;
    gboolean          raw_control;
    int               raw_flags;
    int               raw_message_kind;
    guint64           raw_len;
    guint64           raw_off;
    struct curl_ws_frame raw_meta;
};

static ns_ws *
ns_ws_ref(ns_ws *ws)
{
    if (ws) g_atomic_int_inc(&ws->refcount);
    return ws;
}

static void
ns_ws_destroy(ns_ws *ws)
{
    if (!ws) return;
    g_free(ws->url);
    g_free(ws->origin);
    g_free(ws->top_url);
    if (ws->protocols) g_ptr_array_free(ws->protocols, TRUE);
    g_free(ws->protocol);
    while (!g_queue_is_empty(&ws->out_queue)) {
        ns_ws_out_msg *m = g_queue_pop_head(&ws->out_queue);
        g_free(m->data);
        g_free(m);
    }
    if (ws->recv_assembly) g_byte_array_free(ws->recv_assembly, TRUE);
    if (ws->raw_in) g_byte_array_free(ws->raw_in, TRUE);
    g_mutex_clear(&ws->lock);
    g_cond_clear(&ws->cond);
    g_free(ws);
}

static void
ns_ws_unref(ns_ws *ws)
{
    if (!ws) return;
    if (g_atomic_int_dec_and_test(&ws->refcount))
        ns_ws_destroy(ws);
}

typedef struct {
    ns_ws  *ws;
    void  (*invoke)(struct ns_ws *ws, gpointer payload);
    gpointer payload;
    void   (*payload_free)(gpointer);
} ns_ws_dispatch;

static gboolean
ns_ws_dispatch_run(gpointer data)
{
    ns_ws_dispatch *d = data;
    ns_ws *ws = d->ws;
    if (!g_atomic_int_get(&ws->detached) && ws->cbs.busy &&
        ws->cbs.busy(ws->user_data)) {
        g_timeout_add(4, ns_ws_dispatch_run, d);
        return G_SOURCE_REMOVE;
    }
    if (!g_atomic_int_get(&ws->detached) && d->invoke)
        d->invoke(ws, d->payload);
    if (d->payload && d->payload_free) d->payload_free(d->payload);
    g_atomic_int_add(&ws->pending, -1);
    ns_ws_unref(ws);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static void
ns_ws_post(ns_ws *ws,
           void (*invoke)(ns_ws *, gpointer),
           gpointer payload,
           void   (*payload_free)(gpointer),
           gboolean droppable)
{
    if (g_atomic_int_get(&ws->detached)) {
        if (payload && payload_free) payload_free(payload);
        return;
    }
    if (droppable && g_atomic_int_get(&ws->pending) >= NS_WS_MAX_PENDING) {
        if (payload && payload_free) payload_free(payload);
        return;
    }
    g_atomic_int_inc(&ws->pending);
    ns_ws_dispatch *d = g_new0(ns_ws_dispatch, 1);
    d->ws = ns_ws_ref(ws);
    d->invoke = invoke;
    d->payload = payload;
    d->payload_free = payload_free;
    g_idle_add(ns_ws_dispatch_run, d);
}

typedef struct {
    GByteArray *data;
    gboolean    is_text;
} ns_ws_msg_payload;

typedef struct {
    int   code;
    char *reason;
    gboolean clean;
} ns_ws_close_payload;

static void
ns_ws_msg_payload_free(gpointer p)
{
    ns_ws_msg_payload *m = p;
    if (m->data) g_byte_array_free(m->data, TRUE);
    g_free(m);
}

static void
ns_ws_close_payload_free(gpointer p)
{
    ns_ws_close_payload *c = p;
    g_free(c->reason);
    g_free(c);
}

static void
ns_ws_invoke_open(ns_ws *ws, gpointer payload)
{
    (void)payload;
    if (ws->cbs.on_open) ws->cbs.on_open(ws->user_data);
}

static void
ns_ws_invoke_msg(ns_ws *ws, gpointer payload)
{
    ns_ws_msg_payload *m = payload;
    if (m->is_text) {
        if (ws->cbs.on_text) {
            const char *text = m->data->len ? (const char *)m->data->data : "";
            ws->cbs.on_text(text, m->data->len, ws->user_data);
        }
    } else {
        if (ws->cbs.on_binary) {
            const guint8 *bytes = m->data->len
                ? m->data->data : (const guint8 *)"";
            ws->cbs.on_binary(bytes, m->data->len, ws->user_data);
        }
    }
}

static void
ns_ws_invoke_close(ns_ws *ws, gpointer payload)
{
    ns_ws_close_payload *c = payload;
    if (ws->cbs.on_close)
        ws->cbs.on_close(c->code, c->reason ? c->reason : "", c->clean, ws->user_data);
}

static void
ns_ws_invoke_error(ns_ws *ws, gpointer payload)
{
    const char *msg = payload;
    if (ws->cbs.on_error) ws->cbs.on_error(msg, ws->user_data);
}

static void
ns_ws_dispatch_open(ns_ws *ws)
{
    g_atomic_int_set(&ws->state, NS_WS_STATE_OPEN);
    ns_ws_post(ws, ns_ws_invoke_open, NULL, NULL, FALSE);
}

static void
ns_ws_dispatch_message(ns_ws *ws, gboolean is_text,
                       const guint8 *data, gsize len)
{
    ns_ws_msg_payload *m = g_new0(ns_ws_msg_payload, 1);
    m->is_text = is_text;
    m->data = g_byte_array_sized_new(len);
    if (len) g_byte_array_append(m->data, data, len);
    ns_ws_post(ws, ns_ws_invoke_msg, m, ns_ws_msg_payload_free, TRUE);
}

static void
ns_ws_dispatch_close(ns_ws *ws, int code, const char *reason, gboolean clean)
{
    g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSED);
    ns_ws_close_payload *c = g_new0(ns_ws_close_payload, 1);
    c->code = code;
    c->reason = reason ? g_strdup(reason) : NULL;
    c->clean = clean;
    ns_ws_post(ws, ns_ws_invoke_close, c, ns_ws_close_payload_free, FALSE);
}

static void
ns_ws_dispatch_error(ns_ws *ws, const char *msg)
{
    ns_ws_post(ws, ns_ws_invoke_error, g_strdup(msg ? msg : ""), g_free, FALSE);
}

static gboolean
ns_ws_raw_write(ns_ws *ws, CURL *curl, const guint8 *data, gsize len)
{
    gsize off = 0;
    int stalls = 0;
    while (off < len) {
        if (g_atomic_int_get(&ws->exit_requested)) return FALSE;
        size_t sent = 0;
        CURLcode rc = curl_easy_send(curl, data + off, len - off, &sent);
        if (rc != CURLE_OK && rc != CURLE_AGAIN) return FALSE;
        off += sent;
        if (sent == 0) {
            if (++stalls > 5000) return FALSE;
            g_usleep(2000);
        } else {
            stalls = 0;
        }
    }
    return TRUE;
}

static guint8
ns_ws_raw_opcode(unsigned int flags)
{
    if (flags & CURLWS_CLOSE) return 0x8;
    if (flags & CURLWS_PING) return 0x9;
    if (flags & CURLWS_PONG) return 0xa;
    return flags & CURLWS_BINARY ? 0x2 : 0x1;
}

static gboolean
ns_ws_raw_send_frame(ns_ws *ws, CURL *curl, const guint8 *data, gsize len,
                     unsigned int flags)
{
    guint8 mask[4];
    if (RAND_bytes(mask, sizeof mask) != 1) return FALSE;
    GByteArray *frame = g_byte_array_sized_new((guint)len + 14);
    guint8 head[10] = { (guint8)(0x80 | ns_ws_raw_opcode(flags)) };
    gsize head_len = 2;
    if (len < 126) {
        head[1] = (guint8)(0x80 | len);
    } else if (len <= 0xffff) {
        head[1] = 0x80 | 126;
        head[2] = (guint8)(len >> 8);
        head[3] = (guint8)len;
        head_len = 4;
    } else {
        head[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++)
            head[2 + i] = (guint8)((guint64)len >> (8 * (7 - i)));
        head_len = 10;
    }
    g_byte_array_append(frame, head, (guint)head_len);
    g_byte_array_append(frame, mask, sizeof mask);
    guint payload_at = frame->len;
    if (len) g_byte_array_append(frame, data, (guint)len);
    for (gsize i = 0; i < len; i++)
        frame->data[payload_at + i] ^= mask[i & 3];
    gboolean ok = ns_ws_raw_write(ws, curl, frame->data, frame->len);
    g_byte_array_unref(frame);
    return ok;
}

static gboolean
ns_ws_raw_parse_header(ns_ws *ws)
{
    const guint8 *p = ws->raw_in->data;
    gsize have = ws->raw_in->len;
    if (have < 2) return TRUE;
    gboolean fin = (p[0] & 0x80) != 0;
    int opcode = p[0] & 0x0f;
    guint64 len = p[1] & 0x7f;
    gsize head_len = len == 126 ? 4 : len == 127 ? 10 : 2;
    if ((p[0] & 0x70) || (p[1] & 0x80)) return FALSE;
    if (have < head_len) return TRUE;
    if (head_len > 2) {
        len = 0;
        for (gsize i = 2; i < head_len; i++) len = (len << 8) | p[i];
    }
    gboolean control = opcode >= 0x8;
    if (control && (!fin || len > 125)) return FALSE;
    int flags;
    switch (opcode) {
    case 0x0:
        if (!ws->raw_message_kind) return FALSE;
        flags = ws->raw_message_kind;
        break;
    case 0x1: flags = CURLWS_TEXT; break;
    case 0x2: flags = CURLWS_BINARY; break;
    case 0x8: flags = CURLWS_CLOSE; break;
    case 0x9: flags = CURLWS_PING; break;
    case 0xa: flags = CURLWS_PONG; break;
    default: return FALSE;
    }
    if (!control) {
        if (opcode != 0x0 && ws->raw_message_kind) return FALSE;
        ws->raw_message_kind = fin ? 0 : flags;
        if (!fin) flags |= CURLWS_CONT;
    }
    g_byte_array_remove_range(ws->raw_in, 0, (guint)head_len);
    ws->raw_in_frame = TRUE;
    ws->raw_control = control;
    ws->raw_flags = flags;
    ws->raw_len = len;
    ws->raw_off = 0;
    return TRUE;
}

static gboolean
ns_ws_raw_take_payload(ns_ws *ws, guint8 *buf, gsize buflen, size_t *got)
{
    guint64 left = ws->raw_len - ws->raw_off;
    if (ws->raw_control && ws->raw_in->len < left) return FALSE;
    gsize n = (gsize)MIN(MIN((guint64)ws->raw_in->len, left), (guint64)buflen);
    if (n == 0 && left > 0) return FALSE;
    if (n) {
        memcpy(buf, ws->raw_in->data, n);
        g_byte_array_remove_range(ws->raw_in, 0, (guint)n);
    }
    ws->raw_meta.flags = ws->raw_flags;
    ws->raw_meta.offset = (curl_off_t)ws->raw_off;
    ws->raw_off += n;
    ws->raw_meta.bytesleft = (curl_off_t)(ws->raw_len - ws->raw_off);
    if (ws->raw_off == ws->raw_len) ws->raw_in_frame = FALSE;
    *got = n;
    return TRUE;
}

static CURLcode
ns_ws_raw_recv(ns_ws *ws, CURL *curl, guint8 *buf, gsize buflen, size_t *got,
               const struct curl_ws_frame **meta)
{
    for (;;) {
        if (!ws->raw_in_frame && !ns_ws_raw_parse_header(ws))
            return CURLE_RECV_ERROR;
        if (ws->raw_in_frame && ns_ws_raw_take_payload(ws, buf, buflen, got)) {
            *meta = &ws->raw_meta;
            return CURLE_OK;
        }
        guint8 chunk[NS_WS_RECV_BUF];
        size_t n = 0;
        CURLcode rc = curl_easy_recv(curl, chunk, sizeof chunk, &n);
        if (rc != CURLE_OK) return rc;
        if (n == 0) return CURLE_RECV_ERROR;
        g_byte_array_append(ws->raw_in, chunk, (guint)n);
    }
}

static char *
ns_ws_raw_request(ns_ws *ws, const char *key)
{
    GUri *uri = g_uri_parse(ws->url, G_URI_FLAGS_ENCODED, NULL);
    if (!uri || !g_uri_get_host(uri)) {
        if (uri) g_uri_unref(uri);
        return NULL;
    }
    const char *host = g_uri_get_host(uri);
    const char *path = g_uri_get_path(uri);
    const char *query = g_uri_get_query(uri);
    gboolean v6 = strchr(host, ':') != NULL;
    GString *req = g_string_new("GET ");
    g_string_append(req, path && *path ? path : "/");
    if (query) g_string_append_printf(req, "?%s", query);
    g_string_append_printf(req, " HTTP/1.1\r\nHost: %s%s%s", v6 ? "[" : "",
                           host, v6 ? "]" : "");
    if (g_uri_get_port(uri) > 0)
        g_string_append_printf(req, ":%d", g_uri_get_port(uri));
    g_string_append_printf(req,
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
        "User-Agent: %s\r\n", key, NS_USER_AGENT);
    if (ws->origin && *ws->origin)
        g_string_append_printf(req, "Origin: %s\r\n", ws->origin);
    for (guint i = 0; ws->protocols && i < ws->protocols->len; i++)
        g_string_append_printf(req, "%s%s%s",
            i ? ", " : "Sec-WebSocket-Protocol: ",
            (const char *)g_ptr_array_index(ws->protocols, i),
            i + 1 == ws->protocols->len ? "\r\n" : "");
    char *cookie = ns_net_cookie_header(ws->url, ws->origin, ws->top_url);
    if (cookie && *cookie)
        g_string_append_printf(req, "Cookie: %s\r\n", cookie);
    g_free(cookie);
    g_string_append(req, "\r\n");
    g_uri_unref(uri);
    return g_string_free(req, FALSE);
}

static char *
ns_ws_raw_header_value(const char *head, const char *name)
{
    gsize name_len = strlen(name);
    for (const char *line = strstr(head, "\r\n"); line; line = strstr(line, "\r\n")) {
        line += 2;
        if (g_ascii_strncasecmp(line, name, name_len) == 0 &&
            line[name_len] == ':') {
            const char *end = strstr(line, "\r\n");
            const char *value = line + name_len + 1;
            return g_strstrip(g_strndup(value, end ? (gsize)(end - value)
                                                   : strlen(value)));
        }
    }
    return NULL;
}

static size_t
ns_ws_native_header(char *line, size_t size, size_t nitems, void *userdata)
{
    ns_ws *ws = userdata;
    size_t len = size * nitems;
    g_autofree char *text = g_strndup(line, len);
    g_autofree char *framed = g_strconcat("\r\n", text, NULL);
    char *value = ns_ws_raw_header_value(framed, "Sec-WebSocket-Protocol");
    if (value) {
        g_free(ws->protocol);
        ws->protocol = value;
    }
    return len;
}

static char *
ns_ws_raw_read_head(ns_ws *ws, CURL *curl)
{
    gint64 deadline = g_get_monotonic_time() + NS_WS_HANDSHAKE_USEC;
    for (;;) {
        const char *end = ws->raw_in->len
            ? g_strstr_len((const char *)ws->raw_in->data, ws->raw_in->len,
                           "\r\n\r\n")
            : NULL;
        if (end) {
            gsize head_len = (gsize)(end - (const char *)ws->raw_in->data) + 4;
            char *head = g_strndup((const char *)ws->raw_in->data, head_len);
            g_byte_array_remove_range(ws->raw_in, 0, (guint)head_len);
            return head;
        }
        if (ws->raw_in->len > NS_WS_MAX_HANDSHAKE ||
            g_atomic_int_get(&ws->exit_requested) ||
            g_get_monotonic_time() > deadline)
            return NULL;
        guint8 chunk[1024];
        size_t n = 0;
        CURLcode rc = curl_easy_recv(curl, chunk, sizeof chunk, &n);
        if (rc == CURLE_AGAIN) {
            g_usleep(2000);
            continue;
        }
        if (rc != CURLE_OK || n == 0) return NULL;
        g_byte_array_append(ws->raw_in, chunk, (guint)n);
    }
}

static char *
ns_ws_raw_handshake(ns_ws *ws, CURL *curl)
{
    guint8 nonce[16];
    if (RAND_bytes(nonce, sizeof nonce) != 1)
        return g_strdup("WebSocket handshake failed (no entropy)");
    g_autofree char *key = g_base64_encode(nonce, sizeof nonce);
    g_autofree char *request = ns_ws_raw_request(ws, key);
    if (!request || !ns_ws_raw_write(ws, curl, (const guint8 *)request,
                                     strlen(request)))
        return g_strdup("WebSocket handshake failed (send)");
    g_autofree char *head = ns_ws_raw_read_head(ws, curl);
    if (!head)
        return g_strdup("WebSocket handshake failed (no response)");
    if (!g_str_has_prefix(head, "HTTP/1.1 101"))
        return g_strdup_printf("WebSocket handshake failed (HTTP %.3s)",
                               strlen(head) >= 12 ? head + 9 : "");
    g_autofree char *accept = ns_ws_raw_header_value(head, "Sec-WebSocket-Accept");
    g_autofree char *upgrade = ns_ws_raw_header_value(head, "Upgrade");
    g_autofree char *challenge = g_strconcat(key, NS_WS_ACCEPT_GUID, NULL);
    guint8 digest[20];
    gsize digest_len = sizeof digest;
    GChecksum *sha1 = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(sha1, (const guchar *)challenge, -1);
    g_checksum_get_digest(sha1, digest, &digest_len);
    g_checksum_free(sha1);
    g_autofree char *expected = g_base64_encode(digest, digest_len);
    if (!upgrade || g_ascii_strcasecmp(upgrade, "websocket") != 0 ||
        g_strcmp0(accept, expected) != 0)
        return g_strdup("WebSocket handshake failed (bad upgrade response)");
    ws->protocol = ns_ws_raw_header_value(head, "Sec-WebSocket-Protocol");
    return NULL;
}

static CURLcode
ns_ws_io_send(ns_ws *ws, CURL *curl, const void *data, size_t len,
              size_t *sent, unsigned int flags)
{
    if (!ws->raw)
        return curl_ws_send(curl, data, len, sent, 0, flags);
    if (!ns_ws_raw_send_frame(ws, curl, data, len, flags))
        return CURLE_SEND_ERROR;
    *sent = len;
    return CURLE_OK;
}

static CURLcode
ns_ws_io_recv(ns_ws *ws, CURL *curl, guint8 *buf, gsize buflen, size_t *got,
              const struct curl_ws_frame **meta)
{
    if (!ws->raw)
        return curl_ws_recv(curl, buf, buflen, got, meta);
    return ns_ws_raw_recv(ws, curl, buf, buflen, got, meta);
}

static gboolean
ns_ws_send_curl(ns_ws *ws, CURL *curl, const guint8 *data, gsize len,
                unsigned int flags)
{
    gsize off = 0;
    int stalls = 0;
    for (;;) {
        if (g_atomic_int_get(&ws->exit_requested)) return FALSE;
        gsize remain = len - off;
        size_t sent = 0;
        CURLcode rc = ns_ws_io_send(ws, curl,
                                    remain ? (const void *)(data + off) : "",
                                    remain, &sent, flags);
        if (rc == CURLE_AGAIN) {
            if (++stalls > 5000) return FALSE;
            g_usleep(2000);
            continue;
        }
        if (rc != CURLE_OK) return FALSE;
        off += sent;
        if (off >= len) return TRUE;
        if (sent == 0) {
            if (++stalls > 5000) return FALSE;
            g_usleep(2000);
        } else {
            stalls = 0;
        }
    }
}

static int
ns_ws_echo_close_code(int code)
{
    if (code == 1000 || code == 1001 || code == 1002 || code == 1003 ||
        (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999))
        return code;
    return 0;
}

static gboolean
ns_ws_send_close_frame(ns_ws *ws, CURL *curl, int code, const char *reason)
{
    guint8 buf[125];
    gsize  len = 0;
    if (code > 0) {
        buf[0] = (guint8)((code >> 8) & 0xff);
        buf[1] = (guint8)(code & 0xff);
        len = 2;
        if (reason && *reason) {
            gsize rlen = strlen(reason);
            if (rlen > sizeof buf - 2) rlen = sizeof buf - 2;
            memcpy(buf + 2, reason, rlen);
            len += rlen;
        }
    }
    size_t sent = 0;
    CURLcode rc = ns_ws_io_send(ws, curl, len ? buf : (const void *)"", len,
                                &sent, CURLWS_CLOSE);
    return rc == CURLE_OK;
}

static void
ns_ws_drain_outgoing(ns_ws *ws, CURL *curl, gboolean *want_close,
                     int *close_code, char **close_reason)
{
    for (;;) {
        g_mutex_lock(&ws->lock);
        ns_ws_out_msg *m = g_queue_pop_head(&ws->out_queue);
        g_mutex_unlock(&ws->lock);
        if (!m) return;
        switch (m->kind) {
        case NS_WS_OUT_TEXT:
            ns_ws_send_curl(ws, curl, m->data, m->len, CURLWS_TEXT);
            break;
        case NS_WS_OUT_BINARY:
            ns_ws_send_curl(ws, curl, m->data, m->len, CURLWS_BINARY);
            break;
        case NS_WS_OUT_CLOSE:
            *want_close = TRUE;
            *close_code = m->close_code;
            g_free(*close_reason);
            *close_reason = m->data ? g_strndup((const char *)m->data, m->len) : NULL;
            break;
        }
        g_free(m->data);
        g_free(m);
        if (*want_close) return;
    }
}

static void
ns_ws_handle_frame(ns_ws *ws, const guint8 *data, gsize len,
                   const struct curl_ws_frame *meta,
                   CURL *curl,
                   gboolean *peer_closed,
                   int *peer_code, char **peer_reason,
                   gboolean *bad_utf8, gboolean *too_big)
{
    int flags = meta->flags;

    if (flags & CURLWS_CLOSE) {
        int code = 1005;
        char *reason = NULL;
        if (data && len >= 2 && len <= 125) {
            code = (data[0] << 8) | data[1];
            if (len > 2)
                reason = g_strndup((const char *)data + 2, len - 2);
        } else if (data && len == 1) {
            code = 1002;
        }
        *peer_closed = TRUE;
        *peer_code = code;
        *peer_reason = reason;
        return;
    }

    if (flags & CURLWS_PING) {
        size_t sent = 0;
        size_t pong_len = (data && len > 0) ? (len > 125 ? 125 : len) : 0;
        ns_ws_io_send(ws, curl, pong_len ? data : (const void *)"", pong_len,
                      &sent, CURLWS_PONG);
        return;
    }

    if (!(flags & (CURLWS_TEXT | CURLWS_BINARY | CURLWS_CONT))) return;

    if (!ws->recv_assembly) ws->recv_assembly = g_byte_array_new();
    if (meta->offset == 0 && !ws->recv_in_message &&
        (flags & (CURLWS_TEXT | CURLWS_BINARY))) {
        g_byte_array_set_size(ws->recv_assembly, 0);
        ws->recv_assembly_kind = (flags & CURLWS_BINARY)
            ? CURLWS_BINARY : CURLWS_TEXT;
        ws->recv_in_message = TRUE;
    } else if (!ws->recv_in_message) {
        return;
    }
    if (len) {
        if (ws->recv_assembly->len + len > NS_WS_MAX_MESSAGE) {
            *too_big = TRUE;
            g_byte_array_set_size(ws->recv_assembly, 0);
            ws->recv_in_message = FALSE;
            return;
        }
        g_byte_array_append(ws->recv_assembly, data, len);
    }

    if (meta->bytesleft == 0 && !(flags & CURLWS_CONT)) {
        gboolean is_text = (ws->recv_assembly_kind != CURLWS_BINARY);
        if (is_text && ws->recv_assembly->len > 0 &&
            !g_utf8_validate((const char *)ws->recv_assembly->data,
                             ws->recv_assembly->len, NULL)) {
            *bad_utf8 = TRUE;
        } else {
            ns_ws_dispatch_message(ws, is_text,
                                   ws->recv_assembly->data,
                                   ws->recv_assembly->len);
        }
        g_byte_array_set_size(ws->recv_assembly, 0);
        ws->recv_in_message = FALSE;
    }
}

static void
ns_ws_worker_wait(ns_ws *ws)
{
    g_mutex_lock(&ws->lock);
    if (g_queue_is_empty(&ws->out_queue) &&
        !g_atomic_int_get(&ws->exit_requested)) {
        gint64 deadline = g_get_monotonic_time() +
                          (gint64)NS_WS_POLL_MSEC * G_TIME_SPAN_MILLISECOND;
        g_cond_wait_until(&ws->cond, &ws->lock, deadline);
    }
    g_mutex_unlock(&ws->lock);
}

static int
ns_ws_handshake_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                         curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    const ns_ws *ws = clientp;
    return g_atomic_int_get(&ws->exit_requested) ? 1 : 0;
}

static gboolean
ns_ws_curl_native(void)
{
    const curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    if (!v || !v->protocols) return FALSE;
    for (const char *const *p = v->protocols; *p; p++) {
        if (g_ascii_strcasecmp(*p, "ws") == 0) return TRUE;
        if (g_ascii_strcasecmp(*p, "wss") == 0) return TRUE;
    }
    return FALSE;
}

static gboolean
ns_ws_protocol_requested(const ns_ws *ws)
{
    for (guint i = 0; ws->protocols && i < ws->protocols->len; i++)
        if (strcmp(g_ptr_array_index(ws->protocols, i), ws->protocol) == 0)
            return TRUE;
    return FALSE;
}

static gpointer
ns_ws_worker_curl(gpointer data)
{
    ns_ws *ws = data;
    if (!ns_net_policy_allows(ws->url)) {
        ns_ws_dispatch_error(ws, ns_net_policy_message());
        ns_ws_dispatch_close(ws, 1006, ns_net_policy_message(), FALSE);
        ns_ws_unref(ws);
        return NULL;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        ns_ws_dispatch_error(ws, "curl init failed");
        ns_ws_dispatch_close(ws, 1006, "init failed", FALSE);
        ns_ws_unref(ws);
        return NULL;
    }

    ws->raw = !ns_ws_curl_native();
    if (ws->raw) {
        g_autofree char *http_url = g_strconcat(
            g_str_has_prefix(ws->url, "wss://") ? "https://" : "http://",
            strstr(ws->url, "://") + 3, NULL);
        ws->raw_in = g_byte_array_new();
        curl_easy_setopt(curl, CURLOPT_URL, http_url);
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_URL, ws->url);
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ns_ws_native_header);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, ws);
    }
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                     (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, NS_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    ns_net_apply_curl_tls(curl);
    ns_net_apply_curl_proxy(curl, ws->url);
    ns_net_address_guard address_guard;
    ns_net_address_guard_init(&address_guard,
        ws->origin && strcmp(ws->origin, "null") != 0 ? ws->origin
                                                      : ws->top_url,
        ws->url);
    ns_net_apply_curl_address_guard(curl, &address_guard);
    ns_net_curl_apply_cookies(curl, ws->url, ws->origin, ws->top_url);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ns_ws_handshake_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, ws);

    struct curl_slist *headers = NULL;
    if (ws->origin && *ws->origin) {
        char *h = g_strconcat("Origin: ", ws->origin, NULL);
        headers = curl_slist_append(headers, h);
        g_free(h);
    }
    if (ws->protocols && ws->protocols->len > 0) {
        GString *s = g_string_new("Sec-WebSocket-Protocol: ");
        for (guint i = 0; i < ws->protocols->len; i++) {
            if (i > 0) g_string_append(s, ", ");
            g_string_append(s, g_ptr_array_index(ws->protocols, i));
        }
        headers = curl_slist_append(headers, s->str);
        g_string_free(s, TRUE);
    }
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode rc = curl_easy_perform(curl);

    if (rc != CURLE_OK || g_atomic_int_get(&ws->exit_requested)) {
        const char *msg = address_guard.blocked
            ? ns_net_address_guard_message(&address_guard)
            : rc != CURLE_OK
            ? (errbuf[0] ? errbuf : curl_easy_strerror(rc))
            : "aborted";
        if (rc != CURLE_OK) ns_ws_dispatch_error(ws, msg);
        ns_ws_dispatch_close(ws, 1006, msg, FALSE);
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        ns_ws_unref(ws);
        return NULL;
    }

    {
        long code = 0;
        char *msg = NULL;
        if (ws->raw)
            msg = ns_ws_raw_handshake(ws, curl);
        else
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (code != 0 && code != 101)
            msg = g_strdup_printf(
                "WebSocket handshake failed (HTTP %ld)", code);
        if (!msg && ws->protocol && !ns_ws_protocol_requested(ws))
            msg = g_strdup("WebSocket handshake failed (unrequested subprotocol)");
        if (msg) {
            ns_ws_dispatch_error(ws, msg);
            ns_ws_dispatch_close(ws, 1006, msg, FALSE);
            g_free(msg);
            if (headers) curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            ns_ws_unref(ws);
            return NULL;
        }
    }
    ns_ws_dispatch_open(ws);

    gboolean clean_close = FALSE;
    int close_code = 1006;
    char *close_reason = NULL;
    gboolean peer_closed = FALSE;
    int peer_code = 1005;
    char *peer_reason = NULL;
    gboolean want_close = FALSE;
    gboolean bad_utf8 = FALSE;
    gboolean too_big = FALSE;

    guint8 buf[NS_WS_RECV_BUF];

    while (!g_atomic_int_get(&ws->exit_requested)) {
        ns_ws_drain_outgoing(ws, curl, &want_close, &close_code, &close_reason);
        if (want_close) {
            g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSING);
            ns_ws_send_close_frame(ws, curl, close_code, close_reason);
            clean_close = TRUE;
            break;
        }

        size_t got = 0;
        const struct curl_ws_frame *meta = NULL;
        rc = ns_ws_io_recv(ws, curl, buf, sizeof buf, &got, &meta);
        if (rc == CURLE_AGAIN) {
            ns_ws_worker_wait(ws);
            continue;
        }
        if (rc != CURLE_OK) {
            const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
            ns_ws_dispatch_error(ws, msg);
            close_code = 1006;
            g_free(close_reason);
            close_reason = g_strdup(msg);
            break;
        }
        if (meta) {
            ns_ws_handle_frame(ws, buf, got, meta, curl,
                               &peer_closed, &peer_code, &peer_reason,
                               &bad_utf8, &too_big);
            if (too_big) {
                g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSING);
                ns_ws_send_close_frame(ws, curl, 1009, "message too big");
                clean_close = FALSE;
                close_code = 1009;
                g_free(close_reason);
                close_reason = g_strdup("message too big");
                break;
            }
            if (bad_utf8) {
                g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSING);
                ns_ws_send_close_frame(ws, curl, 1007, "invalid utf-8");
                clean_close = FALSE;
                close_code = 1007;
                g_free(close_reason);
                close_reason = g_strdup("invalid utf-8");
                break;
            }
            if (peer_closed) {
                g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSING);
                ns_ws_send_close_frame(ws, curl, ns_ws_echo_close_code(peer_code),
                                       NULL);
                clean_close = TRUE;
                close_code = peer_code;
                g_free(close_reason);
                close_reason = peer_reason ? g_strdup(peer_reason) : NULL;
                break;
            }
        }
    }

    ns_ws_dispatch_close(ws, close_code,
                         close_reason ? close_reason
                                      : (peer_reason ? peer_reason : ""),
                         clean_close);

    g_free(close_reason);
    g_free(peer_reason);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    ns_ws_unref(ws);
    return NULL;
}

ns_ws *
ns_ws_new(const char        *url,
          const char        *origin,
          const char        *top_url,
          const char *const *protocols,
          const ns_ws_callbacks *cbs,
          gpointer           user_data)
{
    g_return_val_if_fail(url != NULL, NULL);

    ns_ws *ws = g_new0(ns_ws, 1);
    ws->refcount = 1;
    ws->url    = g_strdup(url);
    ws->origin = origin ? g_strdup(origin) : NULL;
    ws->top_url = top_url ? g_strdup(top_url) : NULL;
    if (protocols && protocols[0]) {
        ws->protocols = g_ptr_array_new_with_free_func(g_free);
        for (int i = 0; protocols[i]; i++)
            g_ptr_array_add(ws->protocols, g_strdup(protocols[i]));
    }
    if (cbs) ws->cbs = *cbs;
    ws->user_data = user_data;
    g_mutex_init(&ws->lock);
    g_cond_init(&ws->cond);
    g_queue_init(&ws->out_queue);
    g_atomic_int_set(&ws->state, NS_WS_STATE_CONNECTING);

    ns_ws_ref(ws);
    ws->thread = g_thread_new("nd-ws", ns_ws_worker_curl, ws);
    return ws;
}

static gboolean
ns_ws_enqueue(ns_ws *ws, ns_ws_out_kind kind,
              const guint8 *data, gsize len, int close_code)
{
    if (!ws) return FALSE;
    int s = g_atomic_int_get(&ws->state);
    if (s == NS_WS_STATE_CLOSED) return FALSE;
    if (kind != NS_WS_OUT_CLOSE && s == NS_WS_STATE_CLOSING) return FALSE;
    if (kind != NS_WS_OUT_CLOSE && len > NS_WS_MAX_MESSAGE) return FALSE;

    ns_ws_out_msg *m = g_new0(ns_ws_out_msg, 1);
    m->kind = kind;
    m->close_code = close_code;
    if (data && len > 0) {
        m->data = g_memdup2(data, len);
        m->len = len;
    }
    g_mutex_lock(&ws->lock);
    g_queue_push_tail(&ws->out_queue, m);
    g_cond_signal(&ws->cond);
    g_mutex_unlock(&ws->lock);
    return TRUE;
}

gboolean
ns_ws_send_text(ns_ws *ws, const char *text, gsize len)
{
    return ns_ws_enqueue(ws, NS_WS_OUT_TEXT,
                         (const guint8 *)text, len, 0);
}

gboolean
ns_ws_send_binary(ns_ws *ws, const guint8 *data, gsize len)
{
    return ns_ws_enqueue(ws, NS_WS_OUT_BINARY, data, len, 0);
}

void
ns_ws_close(ns_ws *ws, int code, const char *reason)
{
    if (!ws) return;
    int s = g_atomic_int_get(&ws->state);
    if (s == NS_WS_STATE_CLOSING || s == NS_WS_STATE_CLOSED) return;
    if (code <= 0) code = 1000;
    gsize rlen = reason ? strlen(reason) : 0;
    ns_ws_enqueue(ws, NS_WS_OUT_CLOSE, (const guint8 *)reason, rlen, code);
    g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSING);
}

const char *
ns_ws_protocol(ns_ws *ws)
{
    return ws && ws->protocol &&
           g_atomic_int_get(&ws->state) != NS_WS_STATE_CONNECTING
        ? ws->protocol : "";
}

int
ns_ws_state_get(ns_ws *ws)
{
    if (!ws) return NS_WS_STATE_CLOSED;
    return g_atomic_int_get(&ws->state);
}

void
ns_ws_free(ns_ws *ws)
{
    if (!ws) return;
    g_atomic_int_set(&ws->detached, 1);
    g_atomic_int_set(&ws->exit_requested, 1);
    g_mutex_lock(&ws->lock);
    g_cond_signal(&ws->cond);
    g_mutex_unlock(&ws->lock);
    if (ws->thread) {
        g_thread_join(ws->thread);
        ws->thread = NULL;
    }
    ns_ws_unref(ws);
}
