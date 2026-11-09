/* Nordstjernen — W3C WebDriver server that drives sandboxed renderers over the engine protocol. */

#include "webdriver.h"

#include <cairo.h>
#include <gio/gio.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef G_OS_WIN32
#include <glib-unix.h>
#include <signal.h>
#endif

#include "../capability.h"
#include "../libnordstjernen.h"
#include "../proc_limits.h"
#include "../rproc_http.h"
#include "../version.h"
#include "procview.h"
#include "rjson.h"

#define WD_ELEMENT "element-6066-11e4-a52e-4f735466cecf"
#define WD_SHADOW  "shadow-6066-11e4-a52e-4f735466cecf"

#define WD_MAX_HEADER (64 * 1024)
#define WD_MAX_BODY   (64 * 1024 * 1024)
#define WD_MAX_WIDTH  2560
#define WD_MAX_HEIGHT 1600
#define WD_SCREEN_W   1920
#define WD_SCREEN_H   1080
#define WD_FRAME_US   16000
#define WD_SCROLL_SETTLE_MS 160
#define WD_MAX_SAFE_INT 9007199254740991.0

typedef struct wd_request {
    char    *method;
    char    *path;
    char    *body;
    gsize    body_len;
    int      status;
    char    *response;
    gboolean done;
    GMutex   lock;
    GCond    cond;
} wd_request;

typedef enum {
    WD_SRC_NONE,
    WD_SRC_KEY,
    WD_SRC_POINTER,
    WD_SRC_WHEEL,
} wd_source_type;

typedef struct wd_source {
    char           *id;
    wd_source_type  type;
    char           *pointer_type;
    GPtrArray      *pressed;
    guint           buttons;
    double          x;
    double          y;
    gboolean        moved;
    gint64          last_click_ms;
    int             clicks;
    double          click_x;
    double          click_y;
} wd_source;

typedef struct wd_window {
    char          *handle;
    ns_rproc_http *proc;
    GPtrArray     *history;
    int            hist_index;
    int            scroll_x;
    int            scroll_y;
    int            page_w;
    int            page_h;
    GPtrArray     *frame_path;
    gboolean       navigated;
} wd_window;

typedef struct wd_session {
    char       *id;
    GPtrArray  *windows;
    wd_window  *current;
    int         vw;
    int         vh;
    int         win_x;
    int         win_y;
    int         settle_ms;
    gint64      t_script;
    gint64      t_page_load;
    gint64      t_implicit;
    char       *load_strategy;
    char       *prompt_behavior;
    gboolean    strict_file;
    gboolean    private_mode;
    RjValue    *proxy;
    GHashTable *issued;
    GPtrArray  *sources;
    gint64      next_frame_us;
} wd_session;

typedef struct wd_call {
    const char *method;
    char      **seg;
    int         nseg;
    RjValue    *body;
    RjValue    *value;
    char       *error;
    char       *message;
    char       *stacktrace;
} wd_call;

typedef struct {
    gunichar    cp;
    const char *key;
    const char *code;
    int         keycode;
} wd_special_key;

typedef struct {
    char        key[32];
    const char *code;
    int         keycode;
    gboolean    printable;
    gboolean    shifted;
    gboolean    modifier;
    int         modifier_bit;
} wd_key;

typedef struct {
    GSocket    *sock;
    GByteArray *buf;
} wd_reader;

typedef struct {
    wd_source     *src;
    const RjValue *list;
} wd_sequence;

typedef void (*wd_fn)(wd_session *s, wd_call *c);

typedef struct {
    const char *method;
    const char *pattern;
    wd_fn       fn;
} wd_route;

static GAsyncQueue *g_wd_queue;
static gint g_wd_quit;
static gboolean g_wd_log;
static wd_session *g_session;
static ns_webdriver_opts g_opts;
static char *g_renderer_path;

static const wd_special_key wd_special_keys[] = {
    { 0xE000, "Unidentified", "", 0 },
    { 0xE001, "Cancel", "", 3 },
    { 0xE002, "Help", "Help", 6 },
    { 0xE003, "Backspace", "Backspace", 8 },
    { 0xE004, "Tab", "Tab", 9 },
    { 0xE005, "Clear", "NumpadClear", 12 },
    { 0xE006, "Enter", "Enter", 13 },
    { 0xE007, "Enter", "NumpadEnter", 13 },
    { 0xE008, "Shift", "ShiftLeft", 16 },
    { 0xE009, "Control", "ControlLeft", 17 },
    { 0xE00A, "Alt", "AltLeft", 18 },
    { 0xE00B, "Pause", "Pause", 19 },
    { 0xE00C, "Escape", "Escape", 27 },
    { 0xE00D, " ", "Space", 32 },
    { 0xE00E, "PageUp", "PageUp", 33 },
    { 0xE00F, "PageDown", "PageDown", 34 },
    { 0xE010, "End", "End", 35 },
    { 0xE011, "Home", "Home", 36 },
    { 0xE012, "ArrowLeft", "ArrowLeft", 37 },
    { 0xE013, "ArrowUp", "ArrowUp", 38 },
    { 0xE014, "ArrowRight", "ArrowRight", 39 },
    { 0xE015, "ArrowDown", "ArrowDown", 40 },
    { 0xE016, "Insert", "Insert", 45 },
    { 0xE017, "Delete", "Delete", 46 },
    { 0xE018, ";", "Semicolon", 186 },
    { 0xE019, "=", "Equal", 187 },
    { 0xE01A, "0", "Numpad0", 96 },
    { 0xE01B, "1", "Numpad1", 97 },
    { 0xE01C, "2", "Numpad2", 98 },
    { 0xE01D, "3", "Numpad3", 99 },
    { 0xE01E, "4", "Numpad4", 100 },
    { 0xE01F, "5", "Numpad5", 101 },
    { 0xE020, "6", "Numpad6", 102 },
    { 0xE021, "7", "Numpad7", 103 },
    { 0xE022, "8", "Numpad8", 104 },
    { 0xE023, "9", "Numpad9", 105 },
    { 0xE024, "*", "NumpadMultiply", 106 },
    { 0xE025, "+", "NumpadAdd", 107 },
    { 0xE026, ",", "NumpadComma", 108 },
    { 0xE027, "-", "NumpadSubtract", 109 },
    { 0xE028, ".", "NumpadDecimal", 110 },
    { 0xE029, "/", "NumpadDivide", 111 },
    { 0xE031, "F1", "F1", 112 },
    { 0xE032, "F2", "F2", 113 },
    { 0xE033, "F3", "F3", 114 },
    { 0xE034, "F4", "F4", 115 },
    { 0xE035, "F5", "F5", 116 },
    { 0xE036, "F6", "F6", 117 },
    { 0xE037, "F7", "F7", 118 },
    { 0xE038, "F8", "F8", 119 },
    { 0xE039, "F9", "F9", 120 },
    { 0xE03A, "F10", "F10", 121 },
    { 0xE03B, "F11", "F11", 122 },
    { 0xE03C, "F12", "F12", 123 },
    { 0xE03D, "Meta", "MetaLeft", 91 },
    { 0xE040, "ZenkakuHankaku", "Lang1", 0 },
    { 0xE050, "Shift", "ShiftRight", 16 },
    { 0xE051, "Control", "ControlRight", 17 },
    { 0xE052, "Alt", "AltRight", 18 },
    { 0xE053, "Meta", "MetaRight", 92 },
    { 0xE054, "PageUp", "Numpad9", 33 },
    { 0xE055, "PageDown", "Numpad3", 34 },
    { 0xE056, "End", "Numpad1", 35 },
    { 0xE057, "Home", "Numpad7", 36 },
    { 0xE058, "ArrowLeft", "Numpad4", 37 },
    { 0xE059, "ArrowUp", "Numpad8", 38 },
    { 0xE05A, "ArrowRight", "Numpad6", 39 },
    { 0xE05B, "ArrowDown", "Numpad2", 40 },
    { 0xE05C, "Insert", "Numpad0", 45 },
    { 0xE05D, "Delete", "NumpadDecimal", 46 },
};

static const struct {
    char        plain;
    char        shifted;
    const char *code;
    int         keycode;
} wd_punct_keys[] = {
    { '`', '~', "Backquote", 192 },    { '1', '!', "Digit1", 49 },
    { '2', '@', "Digit2", 50 },        { '3', '#', "Digit3", 51 },
    { '4', '$', "Digit4", 52 },        { '5', '%', "Digit5", 53 },
    { '6', '^', "Digit6", 54 },        { '7', '&', "Digit7", 55 },
    { '8', '*', "Digit8", 56 },        { '9', '(', "Digit9", 57 },
    { '0', ')', "Digit0", 48 },        { '-', '_', "Minus", 189 },
    { '=', '+', "Equal", 187 },        { '[', '{', "BracketLeft", 219 },
    { ']', '}', "BracketRight", 221 }, { '\\', '|', "Backslash", 220 },
    { ';', ':', "Semicolon", 186 },    { '\'', '"', "Quote", 222 },
    { ',', '<', "Comma", 188 },        { '.', '>', "Period", 190 },
    { '/', '?', "Slash", 191 },
};

static const char *const wd_prompt_behaviors[] = {
    "dismiss", "accept", "dismiss and notify", "accept and notify", "ignore",
};

static gboolean
wd_is(const RjValue *v, RjType type)
{
    return v && v->type == type;
}

static guint
wd_arr_len(const RjValue *v)
{
    return wd_is(v, RJ_ARR) ? v->items->len : 0;
}

static gboolean
wd_has(const RjValue *obj, const char *key)
{
    return rj_get(obj, key) != NULL;
}

static void
wd_request_free(wd_request *r)
{
    if (!r) return;
    g_free(r->method);
    g_free(r->path);
    g_free(r->body);
    g_free(r->response);
    g_mutex_clear(&r->lock);
    g_cond_clear(&r->cond);
    g_free(r);
}

static void
wd_request_complete(wd_request *r, int status, char *response)
{
    g_mutex_lock(&r->lock);
    r->status = status;
    r->response = response;
    r->done = TRUE;
    g_cond_signal(&r->cond);
    g_mutex_unlock(&r->lock);
}

static const char *
wd_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

static gboolean
wd_send_all(GSocket *sock, const char *buf, gsize len)
{
    while (len > 0) {
        gssize n = g_socket_send(sock, buf, len, NULL, NULL);
        if (n <= 0) return FALSE;
        buf += n;
        len -= (gsize)n;
    }
    return TRUE;
}

static gboolean
wd_send_response(GSocket *sock, int status, const char *body,
                 gboolean keep_alive)
{
    gsize blen = body ? strlen(body) : 0;
    char *head = g_strdup_printf(
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %" G_GSIZE_FORMAT "\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: %s\r\n\r\n",
        status, wd_reason(status), blen, keep_alive ? "keep-alive" : "close");
    gboolean ok = wd_send_all(sock, head, strlen(head)) &&
                  (blen == 0 || wd_send_all(sock, body, blen));
    g_free(head);
    return ok;
}

static char *
wd_error_body(const char *code, const char *message)
{
    GString *out = g_string_new("{\"value\":{\"error\":");
    rj_append_str(out, code);
    g_string_append(out, ",\"message\":");
    rj_append_str(out, message);
    g_string_append(out, ",\"stacktrace\":\"\"}}");
    return g_string_free(out, FALSE);
}

static gboolean
wd_read_more(wd_reader *rd)
{
    char tmp[16384];
    gssize n = g_socket_receive(rd->sock, tmp, sizeof tmp, NULL, NULL);
    if (n <= 0) return FALSE;
    g_byte_array_append(rd->buf, (const guint8 *)tmp, (guint)n);
    return TRUE;
}

static gssize
wd_find_header_end(GByteArray *buf)
{
    for (guint i = 0; i + 3 < buf->len; i++)
        if (buf->data[i] == '\r' && buf->data[i + 1] == '\n' &&
            buf->data[i + 2] == '\r' && buf->data[i + 3] == '\n')
            return (gssize)i + 4;
    return -1;
}

static gboolean
wd_read_at_least(wd_reader *rd, gsize n)
{
    while (rd->buf->len < n)
        if (!wd_read_more(rd)) return FALSE;
    return TRUE;
}

static char *
wd_read_line(wd_reader *rd)
{
    for (;;) {
        for (guint i = 0; i + 1 < rd->buf->len; i++) {
            if (rd->buf->data[i] == '\r' && rd->buf->data[i + 1] == '\n') {
                char *line = g_strndup((const char *)rd->buf->data, i);
                g_byte_array_remove_range(rd->buf, 0, i + 2);
                return line;
            }
        }
        if (rd->buf->len > WD_MAX_HEADER || !wd_read_more(rd)) return NULL;
    }
}

static GByteArray *
wd_read_chunked(wd_reader *rd)
{
    GByteArray *body = g_byte_array_new();
    for (;;) {
        char *line = wd_read_line(rd);
        if (!line) break;
        gchar *end = NULL;
        guint64 size = g_ascii_strtoull(line, &end, 16);
        gboolean bad = end == line;
        g_free(line);
        if (bad || size > WD_MAX_BODY || body->len + size > WD_MAX_BODY) break;
        if (size == 0) {
            for (;;) {
                char *trailer = wd_read_line(rd);
                if (!trailer) goto fail;
                gboolean last = !*trailer;
                g_free(trailer);
                if (last) return body;
            }
        }
        if (!wd_read_at_least(rd, (gsize)size + 2)) break;
        g_byte_array_append(body, rd->buf->data, (guint)size);
        g_byte_array_remove_range(rd->buf, 0, (guint)size + 2);
    }
fail:
    g_byte_array_free(body, TRUE);
    return NULL;
}

static gboolean
wd_host_allowed(const char *host)
{
    if (!host) return TRUE;
    g_autofree char *h = g_ascii_strdown(host, -1);
    char *name = g_strstrip(h);
    if (name[0] == '[') {
        char *close = strchr(name, ']');
        if (!close) return FALSE;
        *close = '\0';
        name++;
    } else {
        char *colon = strrchr(name, ':');
        if (colon && strchr(name, ':') == colon) *colon = '\0';
    }
    return strcmp(name, "localhost") == 0 || strcmp(name, "127.0.0.1") == 0 ||
           strcmp(name, "::1") == 0;
}

static gboolean
wd_content_type_allowed(const char *ct)
{
    if (!ct) return TRUE;
    g_autofree char *c = g_ascii_strdown(ct, -1);
    char *semi = strchr(c, ';');
    if (semi) *semi = '\0';
    g_strstrip(c);
    return strcmp(c, "application/x-www-form-urlencoded") != 0 &&
           strcmp(c, "multipart/form-data") != 0 &&
           strcmp(c, "text/plain") != 0;
}

static gpointer
wd_connection_thread(gpointer data)
{
    GSocketConnection *conn = data;
    GSocket *sock = g_socket_connection_get_socket(conn);
    g_socket_set_blocking(sock, TRUE);
    g_socket_set_timeout(sock, 0);
    wd_reader rd = { sock, g_byte_array_new() };
    gboolean keep_alive = TRUE;
    while (keep_alive && !g_atomic_int_get(&g_wd_quit)) {
        gssize hend;
        while ((hend = wd_find_header_end(rd.buf)) < 0) {
            if (rd.buf->len > WD_MAX_HEADER || !wd_read_more(&rd)) goto done;
        }
        char *head = g_strndup((const char *)rd.buf->data, (gsize)hend - 4);
        g_byte_array_remove_range(rd.buf, 0, (guint)hend);
        char **lines = g_strsplit(head, "\r\n", -1);
        g_free(head);
        char **req = lines[0] ? g_strsplit(lines[0], " ", 3) : NULL;
        if (!req || g_strv_length(req) != 3) {
            g_strfreev(req);
            g_strfreev(lines);
            char *b = wd_error_body("unknown error", "Malformed HTTP request line");
            wd_send_response(sock, 400, b, FALSE);
            g_free(b);
            goto done;
        }
        keep_alive = strcmp(req[2], "HTTP/1.0") != 0;
        const char *host = NULL, *origin = NULL, *ctype = NULL, *te = NULL,
                   *expect = NULL;
        gint64 clen = -1;
        for (int i = 1; lines[i]; i++) {
            char *colon = strchr(lines[i], ':');
            if (!colon) continue;
            *colon = '\0';
            char *name = g_strstrip(lines[i]);
            char *value = g_strstrip(colon + 1);
            if (g_ascii_strcasecmp(name, "Host") == 0) host = value;
            else if (g_ascii_strcasecmp(name, "Origin") == 0) origin = value;
            else if (g_ascii_strcasecmp(name, "Content-Type") == 0) ctype = value;
            else if (g_ascii_strcasecmp(name, "Transfer-Encoding") == 0) te = value;
            else if (g_ascii_strcasecmp(name, "Expect") == 0) expect = value;
            else if (g_ascii_strcasecmp(name, "Content-Length") == 0)
                clen = g_ascii_strtoll(value, NULL, 10);
            else if (g_ascii_strcasecmp(name, "Connection") == 0) {
                if (g_ascii_strcasecmp(value, "close") == 0) keep_alive = FALSE;
                else if (g_ascii_strcasecmp(value, "keep-alive") == 0)
                    keep_alive = TRUE;
            }
        }
        if (expect && g_ascii_strcasecmp(expect, "100-continue") == 0)
            wd_send_all(sock, "HTTP/1.1 100 Continue\r\n\r\n", 25);
        GByteArray *body = NULL;
        int early_status = 0;
        const char *early_msg = NULL;
        if (te && g_ascii_strcasecmp(te, "chunked") == 0) {
            body = wd_read_chunked(&rd);
            if (!body) {
                early_status = 400;
                early_msg = "Malformed chunked request body";
                keep_alive = FALSE;
            }
        } else if (clen > WD_MAX_BODY) {
            early_status = 413;
            early_msg = "Request body is too large";
            keep_alive = FALSE;
        } else if (clen > 0) {
            if (!wd_read_at_least(&rd, (gsize)clen)) {
                g_strfreev(req);
                g_strfreev(lines);
                goto done;
            }
            body = g_byte_array_new();
            g_byte_array_append(body, rd.buf->data, (guint)clen);
            g_byte_array_remove_range(rd.buf, 0, (guint)clen);
        }
        if (!early_status && !wd_host_allowed(host)) {
            early_status = 403;
            early_msg = "The Host header does not name a loopback address";
        } else if (!early_status && origin) {
            early_status = 403;
            early_msg = "Requests from web pages (with an Origin header) are refused";
        } else if (!early_status && strcmp(req[0], "POST") == 0 &&
                   !wd_content_type_allowed(ctype)) {
            early_status = 400;
            early_msg = "POST bodies must be application/json";
        }
        if (early_status) {
            char *b = wd_error_body(early_status == 403 ? "unknown error"
                                                        : "invalid argument",
                                    early_msg);
            gboolean ok = wd_send_response(sock, early_status, b, keep_alive);
            g_free(b);
            if (body) g_byte_array_free(body, TRUE);
            g_strfreev(req);
            g_strfreev(lines);
            if (!ok) goto done;
            continue;
        }
        wd_request *r = g_new0(wd_request, 1);
        g_mutex_init(&r->lock);
        g_cond_init(&r->cond);
        r->method = g_strdup(req[0]);
        char *q = strchr(req[1], '?');
        r->path = q ? g_strndup(req[1], (gsize)(q - req[1])) : g_strdup(req[1]);
        if (body) {
            r->body_len = body->len;
            g_byte_array_append(body, (const guint8 *)"", 1);
            r->body = (char *)g_byte_array_free(body, FALSE);
        }
        g_strfreev(req);
        g_strfreev(lines);
        g_async_queue_push(g_wd_queue, r);
        g_main_context_wakeup(NULL);
        g_mutex_lock(&r->lock);
        while (!r->done)
            g_cond_wait(&r->cond, &r->lock);
        g_mutex_unlock(&r->lock);
        gboolean ok = wd_send_response(sock, r->status, r->response,
                                       keep_alive);
        wd_request_free(r);
        if (!ok) break;
    }
done:
    g_byte_array_free(rd.buf, TRUE);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    g_object_unref(conn);
    return NULL;
}

static gpointer
wd_accept_thread(gpointer data)
{
    GSocketListener *listener = data;
    while (!g_atomic_int_get(&g_wd_quit)) {
        GError *err = NULL;
        GSocketConnection *conn =
            g_socket_listener_accept(listener, NULL, NULL, &err);
        g_clear_error(&err);
        if (!conn) {
            if (g_atomic_int_get(&g_wd_quit)) break;
            g_usleep(10000);
            continue;
        }
        GThread *t = g_thread_try_new("wd-conn", wd_connection_thread, conn,
                                      NULL);
        if (t)
            g_thread_unref(t);
        else
            g_object_unref(conn);
    }
    return NULL;
}

static void
wd_fail(wd_call *c, const char *code, const char *fmt, ...) G_GNUC_PRINTF(3, 4);

static void
wd_fail(wd_call *c, const char *code, const char *fmt, ...)
{
    if (c->error) return;
    va_list ap;
    va_start(ap, fmt);
    c->error = g_strdup(code);
    c->message = g_strdup_vprintf(fmt, ap);
    va_end(ap);
}

static void
wd_clear_error(wd_call *c)
{
    g_clear_pointer(&c->error, g_free);
    g_clear_pointer(&c->message, g_free);
    g_clear_pointer(&c->stacktrace, g_free);
}

static void
wd_set_value(wd_call *c, RjValue *v)
{
    rj_free(c->value);
    c->value = v;
}

static gboolean wd_failed(const wd_call *c) { return c->error != NULL; }

static gint64
wd_now_ms(void)
{
    return g_get_monotonic_time() / 1000;
}

static void
wd_source_free(wd_source *src)
{
    if (!src) return;
    g_free(src->id);
    g_free(src->pointer_type);
    if (src->pressed) g_ptr_array_free(src->pressed, TRUE);
    g_free(src);
}

static void
wd_window_free(wd_window *w)
{
    if (!w) return;
    g_free(w->handle);
    if (w->proc) ns_rproc_http_close(w->proc);
    if (w->history) g_ptr_array_free(w->history, TRUE);
    if (w->frame_path) g_ptr_array_free(w->frame_path, TRUE);
    g_free(w);
}

static void
wd_session_free(wd_session *s)
{
    if (!s) return;
    g_free(s->id);
    if (s->windows) g_ptr_array_free(s->windows, TRUE);
    g_free(s->load_strategy);
    g_free(s->prompt_behavior);
    rj_free(s->proxy);
    if (s->issued) g_hash_table_destroy(s->issued);
    if (s->sources) g_ptr_array_free(s->sources, TRUE);
    g_free(s);
}

static void
wd_history_push(wd_window *w, const char *url)
{
    if (!url || !*url) return;
    if (w->hist_index >= 0 && w->hist_index < (int)w->history->len &&
        strcmp(g_ptr_array_index(w->history, w->hist_index), url) == 0)
        return;
    while ((int)w->history->len > w->hist_index + 1)
        g_ptr_array_remove_index(w->history, w->history->len - 1);
    g_ptr_array_add(w->history, g_strdup(url));
    w->hist_index = (int)w->history->len - 1;
}

static ns_rproc_http *
wd_spawn_renderer(wd_session *s)
{
    ns_rproc_http *proc = ns_rproc_http_spawn_shm_ex(
        g_renderer_path, MAX(WD_MAX_WIDTH, s->vw), MAX(WD_MAX_HEIGHT, s->vh),
        s->private_mode ? 1 : 0);
    if (proc)
        ns_rproc_http_set_device_pixel_ratio(proc, 1.0);
    return proc;
}

static void
wd_window_new_document(wd_window *w)
{
    g_ptr_array_set_size(w->frame_path, 0);
    w->scroll_x = 0;
    w->scroll_y = 0;
    w->navigated = TRUE;
}

static int
wd_proc_open(wd_session *s, wd_window *w, const char *url, gboolean history,
             gboolean user_activated, ns_rproc_http_page *pg)
{
    memset(pg, 0, sizeof *pg);
    int rc = w->proc ? ns_rproc_http_open_ex(w->proc, url, s->vw, s->vh,
                                             s->settle_ms, history ? 1 : 0,
                                             user_activated ? 1 : 0, pg)
                     : -1;
    if (rc == 0) return 0;
    ns_rproc_http_page_clear(pg);
    if (w->proc) ns_rproc_http_close(w->proc);
    w->proc = wd_spawn_renderer(s);
    if (!w->proc) return -1;
    if (g_wd_log) g_printerr("[webdriver] restarted the renderer of %s\n", w->handle);
    return ns_rproc_http_open_ex(w->proc, url, s->vw, s->vh, s->settle_ms, 0,
                                 user_activated ? 1 : 0, pg);
}

static gboolean
wd_window_open(wd_session *s, wd_window *w, const char *url,
               gboolean history_load, gboolean user_activated, gboolean push)
{
    char *href = g_strdup(url);
    gboolean ok = TRUE;
    for (int hops = 0; href && hops < 8; hops++) {
        ns_rproc_http_page pg;
        if (wd_proc_open(s, w, href, history_load, user_activated, &pg) != 0)
            ok = FALSE;
        wd_window_new_document(w);
        if (push || hops > 0)
            wd_history_push(w, pg.url ? pg.url : href);
        if (g_wd_log)
            g_printerr("[webdriver] loaded %s\n", pg.url ? pg.url : href);
        g_free(href);
        href = pg.nav ? g_strdup(pg.nav) : NULL;
        ns_rproc_http_page_clear(&pg);
        history_load = FALSE;
        user_activated = FALSE;
    }
    g_free(href);
    return ok;
}

static void
wd_answer_capabilities(wd_window *w, const char *list)
{
    if (!list || !*list || !w->proc) return;
    char **entries = g_strsplit(list, ";", -1);
    for (int i = 0; entries[i]; i++) {
        char *entry = g_strstrip(entries[i]);
        char *space = strchr(entry, ' ');
        if (!space) continue;
        *space = '\0';
        const char *origin = g_strstrip(space + 1);
        ns_capability cap;
        gboolean allow = ns_capability_from_name(entry, &cap) &&
                         ns_capability_info_get(cap)->default_state == NS_CAP_ALLOW;
        ns_rproc_http_resolve_capability(w->proc, entry, origin, allow ? 1 : 0);
    }
    g_strfreev(entries);
}

static gboolean
wd_window_render_wheel(wd_session *s, wd_window *w,
                       const ns_rproc_http_wheel *wheel,
                       ns_rproc_http_frame *out)
{
    ns_rproc_http_frame fr;
    memset(&fr, 0, sizeof fr);
    if (out) memset(out, 0, sizeof *out);
    if (!w->proc ||
        ns_rproc_http_render_wheel(w->proc, s->vw, s->vh, w->scroll_x,
                                   w->scroll_y, 1.0, 0, wheel, &fr) != 0 ||
        !fr.ok) {
        ns_rproc_http_frame_clear(&fr);
        return FALSE;
    }
    if (fr.scroll_x >= 0) w->scroll_x = fr.scroll_x;
    if (fr.scroll_y >= 0) w->scroll_y = fr.scroll_y;
    w->page_w = fr.page_w;
    w->page_h = fr.page_h;
    wd_answer_capabilities(w, fr.capability);
    char *nav = fr.nav ? g_strdup(fr.nav) : NULL;
    if (out && !nav) {
        *out = fr;
        return FALSE;
    }
    ns_rproc_http_frame_clear(&fr);
    if (!nav) return FALSE;
    wd_window_open(s, w, nav, FALSE, FALSE, TRUE);
    g_free(nav);
    return TRUE;
}

static gboolean
wd_window_render(wd_session *s, wd_window *w, ns_rproc_http_frame *out)
{
    return wd_window_render_wheel(s, w, NULL, out);
}

static void
wd_window_tick(wd_session *s, wd_window *w)
{
    ns_rproc_http_tick t;
    memset(&t, 0, sizeof t);
    if (!w->proc || ns_rproc_http_tick_page(w->proc, &t) != 0 || !t.ok) {
        ns_rproc_http_tick_clear(&t);
        return;
    }
    wd_answer_capabilities(w, t.capability);
    char *nav = t.nav && *t.nav ? g_strdup(t.nav) : NULL;
    gboolean changed = t.changed || t.animating;
    ns_rproc_http_tick_clear(&t);
    if (nav) {
        wd_window_open(s, w, nav, FALSE, FALSE, TRUE);
        g_free(nav);
    } else if (changed) {
        wd_window_render(s, w, NULL);
    }
}

static void
wd_pump_step(wd_session *s)
{
    if (!s) return;
    gint64 now = g_get_monotonic_time();
    if (now < s->next_frame_us) return;
    s->next_frame_us = now + WD_FRAME_US;
    for (guint i = 0; i < s->windows->len; i++)
        wd_window_tick(s, g_ptr_array_index(s->windows, i));
}

static void
wd_pump(wd_session *s, int ms)
{
    gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
    do {
        while (g_main_context_iteration(NULL, FALSE))
            ;
        wd_pump_step(s);
        gint64 left = end - g_get_monotonic_time();
        if (left > 0) g_usleep((gulong)MIN(left, 4000));
    } while (g_get_monotonic_time() < end);
}

static void
wd_collect_issued(wd_session *s, const RjValue *v)
{
    if (!v) return;
    if (v->type == RJ_OBJ) {
        const char *e = rj_get_str(v, WD_ELEMENT);
        const char *sh = rj_get_str(v, WD_SHADOW);
        if (e) g_hash_table_add(s->issued, g_strdup(e));
        if (sh) g_hash_table_add(s->issued, g_strdup(sh));
    }
    if (v->type == RJ_OBJ || v->type == RJ_ARR)
        for (guint i = 0; i < v->items->len; i++)
            wd_collect_issued(s, g_ptr_array_index(v->items, i));
}

static RjValue *
wd_atom_raw(wd_window *w, RjValue *req, gboolean top, wd_call *c)
{
    RjValue *ctx = rj_new_arr();
    if (!top)
        for (guint i = 0; i < w->frame_path->len; i++)
            rj_arr_add(ctx, rj_new_str(g_ptr_array_index(w->frame_path, i)));
    rj_obj_set(req, "ctx", ctx);
    rj_obj_set(req, "handle", rj_new_str(w->handle));
    char *json = rj_to_string(req);
    rj_free(req);
    char *res = w->proc ? ns_rproc_http_webdriver(w->proc, json) : NULL;
    g_free(json);
    if (!res) {
        wd_fail(c, "unknown error",
                "The page did not answer (its renderer has no page or stopped)");
        return NULL;
    }
    if (g_str_has_prefix(res, "error: ")) {
        wd_fail(c, "javascript error", "%s", res + 7);
        free(res);
        return NULL;
    }
    RjValue *reply = rj_parse(res, strlen(res));
    free(res);
    if (!wd_is(reply, RJ_OBJ)) {
        rj_free(reply);
        wd_fail(c, "unknown error", "The page sent a reply that is not a JSON object");
        return NULL;
    }
    return reply;
}

static void
wd_atom_error(wd_session *s, const RjValue *reply, wd_call *c)
{
    const char *code = rj_get_str(reply, "error");
    const char *msg = rj_get_str(reply, "message");
    const char *unknown = rj_get_str(reply, "unknownRef");
    if (unknown && g_hash_table_contains(s->issued, unknown)) {
        if (g_strcmp0(code, "no such shadow root") == 0) {
            code = "detached shadow root";
            msg = "The shadow root belongs to a document that is gone";
        } else {
            code = "stale element reference";
            msg = "The element belongs to a document that is gone";
        }
    }
    wd_fail(c, code ? code : "unknown error", "%s", msg ? msg : "");
    const char *st = rj_get_str(reply, "stacktrace");
    if (st && !c->stacktrace) c->stacktrace = g_strdup(st);
}

static RjValue *
wd_atom(wd_session *s, wd_window *w, RjValue *req, gboolean top, wd_call *c)
{
    RjValue *reply = wd_atom_raw(w, req, top, c);
    if (!reply) return NULL;
    if (wd_has(reply, "error")) {
        wd_atom_error(s, reply, c);
        rj_free(reply);
        return NULL;
    }
    const RjValue *v = rj_get(reply, "value");
    RjValue *out = v ? rj_copy(v) : rj_new_null();
    rj_free(reply);
    wd_collect_issued(s, out);
    return out;
}

static RjValue *
wd_req(const char *cmd)
{
    RjValue *r = rj_new_obj();
    rj_obj_set(r, "cmd", rj_new_str(cmd));
    return r;
}

static RjValue *
wd_req_id(const char *cmd, const char *id)
{
    RjValue *r = wd_req(cmd);
    rj_obj_set(r, "id", rj_new_str(id));
    return r;
}

static RjValue *
wd_probe(wd_session *s, wd_window *w, RjValue *req, gboolean top)
{
    wd_call probe = { 0 };
    RjValue *v = wd_atom(s, w, req, top, &probe);
    wd_clear_error(&probe);
    return v;
}

static gboolean
wd_wait_for_load(wd_session *s, wd_window *w, gint64 start_ms, wd_call *c)
{
    if (g_strcmp0(s->load_strategy, "none") == 0) return TRUE;
    gboolean eager = g_strcmp0(s->load_strategy, "eager") == 0;
    for (;;) {
        RjValue *info = wd_probe(s, w, wd_req("info"), TRUE);
        const char *state = info ? rj_get_str(info, "readyState") : NULL;
        gboolean ready = !info || !state || strcmp(state, "complete") == 0 ||
                         (eager && strcmp(state, "interactive") == 0);
        rj_free(info);
        if (ready) return TRUE;
        if (wd_now_ms() - start_ms > s->t_page_load) {
            wd_fail(c, "timeout", "The page did not finish loading within %"
                    G_GINT64_FORMAT " ms", s->t_page_load);
            return FALSE;
        }
        wd_pump(s, 10);
    }
}

static void
wd_settle_after_input(wd_session *s, wd_window *w, char *href,
                      gint64 start_ms, wd_call *c)
{
    w->navigated = FALSE;
    if (href && *href)
        wd_window_open(s, w, href, FALSE, TRUE, TRUE);
    free(href);
    wd_window_render(s, w, NULL);
    if (w->navigated)
        wd_wait_for_load(s, w, start_ms, c);
}

static void
wd_key_lookup(gunichar ch, gboolean shift_down, wd_key *k)
{
    memset(k, 0, sizeof *k);
    k->code = "";
    if (ch == '\n' || ch == '\r') ch = 0xE006;
    else if (ch == '\t') ch = 0xE004;
    for (gsize i = 0; i < G_N_ELEMENTS(wd_special_keys); i++) {
        if (wd_special_keys[i].cp != ch) continue;
        g_strlcpy(k->key, wd_special_keys[i].key, sizeof k->key);
        k->code = wd_special_keys[i].code;
        k->keycode = wd_special_keys[i].keycode;
        k->printable = g_utf8_strlen(k->key, -1) == 1;
        if (strcmp(k->key, "Shift") == 0) k->modifier_bit = 1;
        else if (strcmp(k->key, "Control") == 0) k->modifier_bit = 2;
        else if (strcmp(k->key, "Alt") == 0) k->modifier_bit = 4;
        else if (strcmp(k->key, "Meta") == 0) k->modifier_bit = 8;
        k->modifier = k->modifier_bit != 0;
        return;
    }
    k->printable = TRUE;
    if (ch >= 'a' && ch <= 'z') {
        if (shift_down) ch = g_unichar_toupper(ch);
    } else if (ch < 0x80 && shift_down) {
        for (gsize i = 0; i < G_N_ELEMENTS(wd_punct_keys); i++)
            if (wd_punct_keys[i].plain == (char)ch) {
                ch = (gunichar)(unsigned char)wd_punct_keys[i].shifted;
                break;
            }
    }
    int n = g_unichar_to_utf8(ch, k->key);
    k->key[n] = '\0';
    if (ch >= 'a' && ch <= 'z') {
        k->keycode = 65 + (int)(ch - 'a');
    } else if (ch >= 'A' && ch <= 'Z') {
        k->keycode = 65 + (int)(ch - 'A');
        k->shifted = TRUE;
    } else if (ch == ' ') {
        k->code = "Space";
        k->keycode = 32;
    } else if (ch < 0x80) {
        for (gsize i = 0; i < G_N_ELEMENTS(wd_punct_keys); i++) {
            if (wd_punct_keys[i].plain == (char)ch ||
                wd_punct_keys[i].shifted == (char)ch) {
                k->code = wd_punct_keys[i].code;
                k->keycode = wd_punct_keys[i].keycode;
                k->shifted = wd_punct_keys[i].shifted == (char)ch;
                break;
            }
        }
    }
    if (k->keycode >= 65 && k->keycode <= 90) {
        static const char *const letters[] = {
            "KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH",
            "KeyI", "KeyJ", "KeyK", "KeyL", "KeyM", "KeyN", "KeyO", "KeyP",
            "KeyQ", "KeyR", "KeyS", "KeyT", "KeyU", "KeyV", "KeyW", "KeyX",
            "KeyY", "KeyZ",
        };
        k->code = letters[k->keycode - 65];
    }
}

static int
wd_session_mods(wd_session *s)
{
    int mods = 0;
    for (guint i = 0; i < s->sources->len; i++) {
        wd_source *src = g_ptr_array_index(s->sources, i);
        if (src->type != WD_SRC_KEY) continue;
        for (guint j = 0; j < src->pressed->len; j++) {
            const char *k = g_ptr_array_index(src->pressed, j);
            if (strcmp(k, "Shift") == 0) mods |= 1;
            else if (strcmp(k, "Control") == 0) mods |= 2;
            else if (strcmp(k, "Alt") == 0) mods |= 4;
            else if (strcmp(k, "Meta") == 0) mods |= 8;
        }
    }
    return mods;
}

static char *
wd_keep_nav(char *nav, char *more)
{
    if (nav && *nav) {
        free(more);
        return nav;
    }
    free(nav);
    return more;
}

static char *
wd_send_key_down(wd_window *w, const wd_key *k, int mods)
{
    if (!w->proc) return NULL;
    int prevented = 0;
    char *nav = ns_rproc_http_key_full(w->proc, 0, k->key, k->code, k->keycode,
                                       mods | NS_KEY_MOD_TEXT_SEPARATE,
                                       &prevented);
    if (k->printable && !(mods & (2 | 4 | 8))) {
        nav = wd_keep_nav(nav, ns_rproc_http_key_full(w->proc, 3, k->key, k->code,
                                                      k->keycode, mods, NULL));
        nav = wd_keep_nav(nav, ns_rproc_http_key_full(w->proc, 2, k->key, "", 0,
                                                      0, NULL));
    }
    return nav;
}

static char *
wd_send_key_up(wd_window *w, const wd_key *k, int mods)
{
    if (!w->proc) return NULL;
    return ns_rproc_http_key_full(w->proc, 1, k->key, k->code, k->keycode, mods,
                                  NULL);
}

static char *
wd_type_string(wd_window *w, const char *text)
{
    int mods = 0;
    GArray *held = g_array_new(FALSE, FALSE, sizeof(gunichar));
    char *nav = NULL;
    for (const char *p = text; *p && !(nav && *nav); p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        wd_key k;
        wd_key_lookup(ch, (mods & 1) != 0, &k);
        if (ch == 0xE000) {
            for (guint i = held->len; i > 0; i--) {
                wd_key mk;
                wd_key_lookup(g_array_index(held, gunichar, i - 1), FALSE, &mk);
                mods &= ~mk.modifier_bit;
                free(wd_send_key_up(w, &mk, mods));
            }
            g_array_set_size(held, 0);
            continue;
        }
        if (k.modifier) {
            guint at = held->len;
            for (guint i = 0; i < held->len; i++) {
                wd_key mk;
                wd_key_lookup(g_array_index(held, gunichar, i), FALSE, &mk);
                if (mk.modifier_bit == k.modifier_bit) at = i;
            }
            if (at < held->len) {
                mods &= ~k.modifier_bit;
                g_array_remove_index(held, at);
                free(wd_send_key_up(w, &k, mods));
            } else {
                mods |= k.modifier_bit;
                g_array_append_val(held, ch);
                nav = wd_keep_nav(nav, wd_send_key_down(w, &k, mods));
            }
            continue;
        }
        int m = mods | (k.shifted ? 1 : 0);
        nav = wd_keep_nav(nav, wd_send_key_down(w, &k, m));
        nav = wd_keep_nav(nav, wd_send_key_up(w, &k, m));
    }
    for (guint i = held->len; i > 0; i--) {
        wd_key mk;
        wd_key_lookup(g_array_index(held, gunichar, i - 1), FALSE, &mk);
        mods &= ~mk.modifier_bit;
        free(wd_send_key_up(w, &mk, mods));
    }
    g_array_free(held, TRUE);
    return nav;
}

static const RjValue *
wd_param(wd_call *c, const char *name)
{
    return rj_get(c->body, name);
}

static const char *
wd_param_string(wd_call *c, const char *name)
{
    const RjValue *v = wd_param(c, name);
    if (!wd_is(v, RJ_STR)) {
        wd_fail(c, "invalid argument", "'%s' must be a string", name);
        return NULL;
    }
    return v->str;
}

static gboolean
wd_is_int(const RjValue *v, double lo, double hi)
{
    return wd_is(v, RJ_NUM) && isfinite(v->num) && v->num == floor(v->num) &&
           v->num >= lo && v->num <= hi;
}

static double
wd_num(const RjValue *obj, const char *key, double def)
{
    const RjValue *v = rj_get(obj, key);
    return wd_is(v, RJ_NUM) ? v->num : def;
}

static wd_window *
wd_current(wd_session *s, wd_call *c)
{
    if (!s->current) {
        wd_fail(c, "no such window", "The current window is closed");
        return NULL;
    }
    return s->current;
}

static const char *
wd_platform_name(void)
{
#if defined(G_OS_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "mac";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#elif defined(__NetBSD__)
    return "netbsd";
#else
    return "unix";
#endif
}

static RjValue *
wd_timeouts_json(wd_session *s)
{
    RjValue *t = rj_new_obj();
    rj_obj_set(t, "script", s->t_script < 0 ? rj_new_null()
                                            : rj_new_num((double)s->t_script));
    rj_obj_set(t, "pageLoad", rj_new_num((double)s->t_page_load));
    rj_obj_set(t, "implicit", rj_new_num((double)s->t_implicit));
    return t;
}

static gboolean
wd_apply_timeouts(wd_session *s, const RjValue *t, wd_call *c)
{
    if (!wd_is(t, RJ_OBJ)) {
        wd_fail(c, "invalid argument", "timeouts must be an object");
        return FALSE;
    }
    gint64 script = s ? s->t_script : 0, page = s ? s->t_page_load : 0,
           implicit = s ? s->t_implicit : 0;
    const char *legacy = rj_get_str(t, "type");
    const RjValue *ms = rj_get(t, "ms");
    if (legacy && ms) {
        if (!wd_is_int(ms, 0, WD_MAX_SAFE_INT)) {
            wd_fail(c, "invalid argument", "ms must be a non-negative integer");
            return FALSE;
        }
        if (strcmp(legacy, "script") == 0) script = (gint64)ms->num;
        else if (strcmp(legacy, "implicit") == 0) implicit = (gint64)ms->num;
        else if (strcmp(legacy, "page load") == 0 ||
                 strcmp(legacy, "pageLoad") == 0) page = (gint64)ms->num;
        else {
            wd_fail(c, "invalid argument", "unknown timeout type %s", legacy);
            return FALSE;
        }
    } else {
        for (guint i = 0; i < t->keys->len; i++) {
            const char *k = g_ptr_array_index(t->keys, i);
            const RjValue *v = g_ptr_array_index(t->items, i);
            if (strcmp(k, "script") == 0) {
                if (wd_is(v, RJ_NULL)) { script = -1; continue; }
            } else if (strcmp(k, "pageLoad") != 0 && strcmp(k, "implicit") != 0) {
                continue;
            }
            if (!wd_is_int(v, 0, WD_MAX_SAFE_INT)) {
                wd_fail(c, "invalid argument",
                        "timeout '%s' must be a non-negative integer", k);
                return FALSE;
            }
            if (strcmp(k, "script") == 0) script = (gint64)v->num;
            else if (strcmp(k, "pageLoad") == 0) page = (gint64)v->num;
            else implicit = (gint64)v->num;
        }
    }
    if (s) {
        s->t_script = script;
        s->t_page_load = page;
        s->t_implicit = implicit;
    }
    return TRUE;
}

static RjValue *
wd_validate_caps(const RjValue *caps, wd_call *c)
{
    if (!wd_is(caps, RJ_OBJ)) {
        wd_fail(c, "invalid argument", "capabilities must be an object");
        return NULL;
    }
    RjValue *out = rj_new_obj();
    for (guint i = 0; i < caps->keys->len; i++) {
        const char *k = g_ptr_array_index(caps->keys, i);
        const RjValue *v = g_ptr_array_index(caps->items, i);
        if (wd_is(v, RJ_NULL)) continue;
        gboolean ok = TRUE;
        if (strcmp(k, "acceptInsecureCerts") == 0 ||
            strcmp(k, "strictFileInteractability") == 0 ||
            strcmp(k, "setWindowRect") == 0 || strcmp(k, "webSocketUrl") == 0) {
            ok = wd_is(v, RJ_BOOL);
        } else if (strcmp(k, "browserName") == 0 ||
                   strcmp(k, "browserVersion") == 0 ||
                   strcmp(k, "platformName") == 0 ||
                   strcmp(k, "userAgent") == 0) {
            ok = wd_is(v, RJ_STR);
        } else if (strcmp(k, "pageLoadStrategy") == 0) {
            ok = wd_is(v, RJ_STR) && (strcmp(v->str, "none") == 0 ||
                                      strcmp(v->str, "eager") == 0 ||
                                      strcmp(v->str, "normal") == 0);
        } else if (strcmp(k, "proxy") == 0) {
            const char *type = rj_get_str(v, "proxyType");
            ok = wd_is(v, RJ_OBJ) && type &&
                 (strcmp(type, "pac") == 0 || strcmp(type, "direct") == 0 ||
                  strcmp(type, "autodetect") == 0 ||
                  strcmp(type, "system") == 0 || strcmp(type, "manual") == 0);
        } else if (strcmp(k, "timeouts") == 0) {
            wd_call tc = { 0 };
            ok = wd_apply_timeouts(NULL, v, &tc);
            wd_clear_error(&tc);
        } else if (strcmp(k, "unhandledPromptBehavior") == 0) {
            ok = wd_is(v, RJ_OBJ);
            if (wd_is(v, RJ_STR))
                for (gsize j = 0; j < G_N_ELEMENTS(wd_prompt_behaviors); j++)
                    if (strcmp(v->str, wd_prompt_behaviors[j]) == 0) ok = TRUE;
        } else if (strcmp(k, "ns:options") == 0) {
            ok = wd_is(v, RJ_OBJ);
        } else if (!strchr(k, ':')) {
            wd_fail(c, "invalid argument", "unknown capability '%s'", k);
            rj_free(out);
            return NULL;
        }
        if (!ok) {
            wd_fail(c, "invalid argument", "capability '%s' has an invalid value", k);
            rj_free(out);
            return NULL;
        }
        rj_obj_set(out, k, rj_copy(v));
    }
    return out;
}

static gboolean
wd_caps_match(const RjValue *caps)
{
    const char *name = rj_get_str(caps, "browserName");
    if (name && g_ascii_strcasecmp(name, "nordstjernen") != 0) return FALSE;
    const char *ver = rj_get_str(caps, "browserVersion");
    if (ver && !g_str_has_prefix(NS_VERSION, ver)) return FALSE;
    const char *plat = rj_get_str(caps, "platformName");
    if (plat && g_ascii_strcasecmp(plat, wd_platform_name()) != 0 &&
        g_ascii_strcasecmp(plat, "any") != 0)
        return FALSE;
    if (rj_get_bool(caps, "acceptInsecureCerts", FALSE)) return FALSE;
    const RjValue *proxy = rj_get(caps, "proxy");
    const char *ptype = proxy ? rj_get_str(proxy, "proxyType") : NULL;
    if (ptype && (strcmp(ptype, "pac") == 0 || strcmp(ptype, "autodetect") == 0))
        return FALSE;
    return TRUE;
}

static char *
wd_proxy_url(const RjValue *proxy)
{
    const char *p = rj_get_str(proxy, "sslProxy");
    if (!p) p = rj_get_str(proxy, "httpProxy");
    if (!p || !*p) return NULL;
    return strstr(p, "://") ? g_strdup(p) : g_strdup_printf("http://%s", p);
}

static wd_window *
wd_window_new(wd_session *s)
{
    wd_window *w = g_new0(wd_window, 1);
    w->handle = g_uuid_string_random();
    w->proc = wd_spawn_renderer(s);
    w->history = g_ptr_array_new_with_free_func(g_free);
    w->hist_index = -1;
    w->frame_path = g_ptr_array_new_with_free_func(g_free);
    if (!w->proc) {
        wd_window_free(w);
        return NULL;
    }
    ns_rproc_http_page pg;
    if (wd_proc_open(s, w, "about:blank", FALSE, TRUE, &pg) != 0) {
        ns_rproc_http_page_clear(&pg);
        wd_window_free(w);
        return NULL;
    }
    ns_rproc_http_page_clear(&pg);
    wd_history_push(w, "about:blank");
    g_ptr_array_add(s->windows, w);
    return w;
}

static void
wd_cmd_new_session(wd_session *unused, wd_call *c)
{
    (void)unused;
    if (g_session) {
        wd_fail(c, "session not created",
                "Maximum number of active sessions (1) reached");
        return;
    }
    const RjValue *caps = wd_param(c, "capabilities");
    RjValue *empty = NULL;
    if (!caps) caps = empty = rj_new_obj();
    if (!wd_is(caps, RJ_OBJ)) {
        wd_fail(c, "invalid argument", "capabilities must be an object");
        return;
    }
    const RjValue *always_raw = rj_get(caps, "alwaysMatch");
    RjValue *always = always_raw ? wd_validate_caps(always_raw, c) : rj_new_obj();
    if (!always) {
        rj_free(empty);
        return;
    }
    const RjValue *first = rj_get(caps, "firstMatch");
    GPtrArray *merged = g_ptr_array_new_with_free_func((GDestroyNotify)rj_free);
    if (first && wd_arr_len(first) == 0) {
        wd_fail(c, "invalid argument", "firstMatch must be a non-empty array");
    } else if (!first) {
        g_ptr_array_add(merged, rj_copy(always));
    } else {
        for (guint i = 0; i < first->items->len && !wd_failed(c); i++) {
            RjValue *f = wd_validate_caps(g_ptr_array_index(first->items, i), c);
            if (!f) break;
            for (guint j = 0; j < f->keys->len && !wd_failed(c); j++) {
                const char *k = g_ptr_array_index(f->keys, j);
                if (wd_has(always, k))
                    wd_fail(c, "invalid argument",
                            "capability '%s' is in both alwaysMatch and firstMatch", k);
            }
            if (wd_failed(c)) {
                rj_free(f);
                break;
            }
            RjValue *m = rj_copy(always);
            for (guint j = 0; j < f->keys->len; j++)
                rj_obj_set(m, g_ptr_array_index(f->keys, j),
                           rj_copy(g_ptr_array_index(f->items, j)));
            rj_free(f);
            g_ptr_array_add(merged, m);
        }
    }
    rj_free(always);
    rj_free(empty);
    if (wd_failed(c)) {
        g_ptr_array_free(merged, TRUE);
        return;
    }
    RjValue *match = NULL;
    for (guint i = 0; i < merged->len && !match; i++)
        if (wd_caps_match(g_ptr_array_index(merged, i)))
            match = rj_copy(g_ptr_array_index(merged, i));
    g_ptr_array_free(merged, TRUE);
    if (!match) {
        wd_fail(c, "session not created",
                "No matching capabilities: this is browserName \"nordstjernen\" "
                "%s on %s", NS_VERSION, wd_platform_name());
        return;
    }

    wd_session *s = g_new0(wd_session, 1);
    s->id = g_uuid_string_random();
    s->windows = g_ptr_array_new_with_free_func((GDestroyNotify)wd_window_free);
    s->vw = g_opts.viewport_width;
    s->vh = g_opts.viewport_height;
    s->settle_ms = g_opts.settle_ms;
    s->t_script = 30000;
    s->t_page_load = 300000;
    s->t_implicit = 0;
    s->issued = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->sources = g_ptr_array_new_with_free_func((GDestroyNotify)wd_source_free);
    const char *strategy = rj_get_str(match, "pageLoadStrategy");
    s->load_strategy = g_strdup(strategy ? strategy : "normal");
    s->prompt_behavior = g_strdup("dismiss and notify");
    s->strict_file = rj_get_bool(match, "strictFileInteractability", FALSE);
    s->private_mode = rj_get_bool(rj_get(match, "ns:options"), "private", TRUE);
    const RjValue *v = rj_get(match, "timeouts");
    if (v) wd_apply_timeouts(s, v, c);
    v = rj_get(match, "proxy");
    if (v) {
        s->proxy = rj_copy(v);
        const char *ptype = rj_get_str(v, "proxyType");
        if (ptype && strcmp(ptype, "manual") == 0) {
            char *url = wd_proxy_url(v);
            if (url) {
                g_setenv("http_proxy", url, TRUE);
                g_setenv("https_proxy", url, TRUE);
            }
            g_free(url);
        } else if (ptype && strcmp(ptype, "direct") == 0) {
            g_unsetenv("http_proxy");
            g_unsetenv("https_proxy");
            g_unsetenv("HTTPS_PROXY");
            g_unsetenv("all_proxy");
            g_unsetenv("ALL_PROXY");
        }
    }

    s->current = wd_window_new(s);
    if (!s->current) {
        rj_free(match);
        wd_session_free(s);
        wd_fail(c, "session not created", "Could not start a renderer (%s)",
                g_renderer_path ? g_renderer_path : "not found");
        return;
    }
    g_session = s;

    RjValue *rc = rj_new_obj();
    rj_obj_set(rc, "browserName", rj_new_str("nordstjernen"));
    rj_obj_set(rc, "browserVersion", rj_new_str(NS_VERSION));
    rj_obj_set(rc, "platformName", rj_new_str(wd_platform_name()));
    rj_obj_set(rc, "acceptInsecureCerts", rj_new_bool(FALSE));
    rj_obj_set(rc, "pageLoadStrategy", rj_new_str(s->load_strategy));
    rj_obj_set(rc, "proxy", s->proxy ? rj_copy(s->proxy) : rj_new_obj());
    rj_obj_set(rc, "setWindowRect", rj_new_bool(TRUE));
    rj_obj_set(rc, "timeouts", wd_timeouts_json(s));
    rj_obj_set(rc, "strictFileInteractability", rj_new_bool(s->strict_file));
    rj_obj_set(rc, "unhandledPromptBehavior", rj_new_str(s->prompt_behavior));
    RjValue *opts = rj_new_obj();
    rj_obj_set(opts, "headless", rj_new_bool(TRUE));
    rj_obj_set(opts, "private", rj_new_bool(s->private_mode));
    rj_obj_set(rc, "ns:options", opts);
    rj_free(match);
    RjValue *out = rj_new_obj();
    rj_obj_set(out, "sessionId", rj_new_str(s->id));
    rj_obj_set(out, "capabilities", rc);
    wd_set_value(c, out);
    if (g_wd_log) g_printerr("[webdriver] session %s created\n", s->id);
}

static void
wd_end_session(void)
{
    wd_session *s = g_session;
    if (!s) return;
    g_session = NULL;
    if (g_wd_log) g_printerr("[webdriver] session %s ended\n", s->id);
    wd_session_free(s);
}

static void
wd_cmd_delete_session(wd_session *s, wd_call *c)
{
    (void)s;
    wd_end_session();
    wd_set_value(c, rj_new_null());
}

static void
wd_cmd_status(wd_session *s, wd_call *c)
{
    (void)s;
    RjValue *v = rj_new_obj();
    rj_obj_set(v, "ready", rj_new_bool(g_session == NULL));
    rj_obj_set(v, "message", rj_new_str(g_session ? "A session is already running"
                                                  : "Nordstjernen is ready"));
    RjValue *build = rj_new_obj();
    rj_obj_set(build, "version", rj_new_str(NS_VERSION));
    rj_obj_set(v, "build", build);
    RjValue *os = rj_new_obj();
    rj_obj_set(os, "name", rj_new_str(wd_platform_name()));
    rj_obj_set(v, "os", os);
    wd_set_value(c, v);
}

static void
wd_cmd_get_timeouts(wd_session *s, wd_call *c)
{
    wd_set_value(c, wd_timeouts_json(s));
}

static void
wd_cmd_set_timeouts(wd_session *s, wd_call *c)
{
    if (wd_apply_timeouts(s, c->body, c)) wd_set_value(c, rj_new_null());
}

static char *
wd_absolute_url(const char *url)
{
    if (!url || !*url) return NULL;
    if (g_str_has_prefix(url, "about:") || g_str_has_prefix(url, "data:") ||
        g_str_has_prefix(url, "javascript:"))
        return g_strdup(url);
    char *scheme = g_uri_parse_scheme(url);
    if (!scheme) return NULL;
    g_free(scheme);
    return g_strdup(url);
}

static void
wd_cmd_navigate(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    const char *url = w ? wd_param_string(c, "url") : NULL;
    if (!url) return;
    char *abs = wd_absolute_url(url);
    if (!abs) {
        wd_fail(c, "invalid argument", "'%s' is not an absolute URL", url);
        return;
    }
    gint64 start = wd_now_ms();
    if (g_str_has_prefix(abs, "javascript:")) {
        RjValue *req = wd_req("execute");
        rj_obj_set(req, "script", rj_new_str(abs + 11));
        rj_obj_set(req, "args", rj_new_arr());
        rj_free(wd_probe(s, w, req, TRUE));
        wd_settle_after_input(s, w, NULL, start, c);
    } else {
        wd_window_open(s, w, abs, FALSE, TRUE, TRUE);
        wd_window_render(s, w, NULL);
        wd_wait_for_load(s, w, start, c);
    }
    g_free(abs);
    if (!wd_failed(c)) wd_set_value(c, rj_new_null());
}

static void
wd_cmd_get_url(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    RjValue *info = wd_probe(s, w, wd_req("info"), TRUE);
    const char *url = info ? rj_get_str(info, "url") : NULL;
    wd_set_value(c, rj_new_str(url && *url ? url : "about:blank"));
    rj_free(info);
}

static void
wd_cmd_get_title(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    RjValue *info = wd_probe(s, w, wd_req("info"), TRUE);
    const char *t = info ? rj_get_str(info, "title") : NULL;
    wd_set_value(c, rj_new_str(t ? t : ""));
    rj_free(info);
}

static void
wd_traverse(wd_session *s, wd_call *c, int delta)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    gint64 start = wd_now_ms();
    RjValue *req = wd_req("execute");
    rj_obj_set(req, "script", rj_new_str(delta < 0
        ? "var n=window.navigation;if(n&&n.canGoBack){history.back();return true}return false"
        : "var n=window.navigation;if(n&&n.canGoForward){history.forward();return true}return false"));
    rj_obj_set(req, "args", rj_new_arr());
    RjValue *same_doc = wd_probe(s, w, req, TRUE);
    gboolean handled = wd_is(same_doc, RJ_BOOL) && same_doc->b;
    rj_free(same_doc);
    if (handled) {
        wd_pump(s, 20);
        wd_set_value(c, rj_new_null());
        return;
    }
    int target = w->hist_index + delta;
    if (target >= 0 && target < (int)w->history->len) {
        w->hist_index = target;
        char *url = g_strdup(g_ptr_array_index(w->history, target));
        wd_window_open(s, w, url, TRUE, TRUE, FALSE);
        g_free(url);
        wd_window_render(s, w, NULL);
        wd_wait_for_load(s, w, start, c);
    }
    if (!wd_failed(c)) wd_set_value(c, rj_new_null());
}

static void wd_cmd_back(wd_session *s, wd_call *c) { wd_traverse(s, c, -1); }
static void wd_cmd_forward(wd_session *s, wd_call *c) { wd_traverse(s, c, 1); }

static void
wd_cmd_refresh(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    gint64 start = wd_now_ms();
    RjValue *info = wd_probe(s, w, wd_req("info"), TRUE);
    const char *u = info ? rj_get_str(info, "url") : NULL;
    char *url = u && *u ? g_strdup(u) : NULL;
    rj_free(info);
    if (!url && w->hist_index >= 0)
        url = g_strdup(g_ptr_array_index(w->history, w->hist_index));
    if (url) {
        ns_rproc_http_page pg;
        memset(&pg, 0, sizeof pg);
        if (!w->proc || ns_rproc_http_reload(w->proc, url, s->vw, s->vh,
                                             s->settle_ms, &pg) != 0) {
            ns_rproc_http_page_clear(&pg);
            wd_window_open(s, w, url, FALSE, TRUE, FALSE);
        } else {
            char *nav = pg.nav ? g_strdup(pg.nav) : NULL;
            ns_rproc_http_page_clear(&pg);
            wd_window_new_document(w);
            if (nav) wd_window_open(s, w, nav, FALSE, FALSE, TRUE);
            g_free(nav);
        }
        wd_window_render(s, w, NULL);
        wd_wait_for_load(s, w, start, c);
    }
    g_free(url);
    if (!wd_failed(c)) wd_set_value(c, rj_new_null());
}

static RjValue *
wd_handles(wd_session *s)
{
    RjValue *a = rj_new_arr();
    for (guint i = 0; s && i < s->windows->len; i++)
        rj_arr_add(a, rj_new_str(((wd_window *)g_ptr_array_index(s->windows, i))->handle));
    return a;
}

static void
wd_cmd_get_window(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (w) wd_set_value(c, rj_new_str(w->handle));
}

static void
wd_cmd_get_handles(wd_session *s, wd_call *c)
{
    wd_set_value(c, wd_handles(s));
}

static void
wd_cmd_close_window(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    g_ptr_array_remove(s->windows, w);
    s->current = NULL;
    RjValue *handles = wd_handles(s);
    if (s->windows->len == 0) wd_end_session();
    wd_set_value(c, handles);
}

static void
wd_cmd_switch_window(wd_session *s, wd_call *c)
{
    const char *h = rj_get_str(c->body, "handle");
    if (!h) h = rj_get_str(c->body, "name");
    if (!h) {
        wd_fail(c, "invalid argument", "'handle' must be a string");
        return;
    }
    for (guint i = 0; i < s->windows->len; i++) {
        wd_window *w = g_ptr_array_index(s->windows, i);
        if (strcmp(w->handle, h) == 0) {
            s->current = w;
            g_ptr_array_set_size(w->frame_path, 0);
            wd_set_value(c, rj_new_null());
            return;
        }
    }
    wd_fail(c, "no such window", "No window with handle %s", h);
}

static void
wd_cmd_new_window(wd_session *s, wd_call *c)
{
    const RjValue *type = wd_param(c, "type");
    if (type && !wd_is(type, RJ_STR) && !wd_is(type, RJ_NULL)) {
        wd_fail(c, "invalid argument", "type must be a string");
        return;
    }
    wd_window *w = wd_window_new(s);
    if (!w) {
        wd_fail(c, "unknown error", "Could not open a new window");
        return;
    }
    RjValue *v = rj_new_obj();
    rj_obj_set(v, "handle", rj_new_str(w->handle));
    rj_obj_set(v, "type", rj_new_str("window"));
    wd_set_value(c, v);
}

static void
wd_cmd_switch_frame(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    const RjValue *id = wd_param(c, "id");
    if (!id) {
        wd_fail(c, "invalid argument", "'id' is required");
        return;
    }
    if (wd_is(id, RJ_NULL)) {
        g_ptr_array_set_size(w->frame_path, 0);
        wd_set_value(c, rj_new_null());
        return;
    }
    RjValue *req = wd_req("frame");
    if (wd_is_int(id, 0, 65535)) {
        rj_obj_set(req, "id", rj_new_num(id->num));
    } else if (wd_is(id, RJ_OBJ) && rj_get_str(id, WD_ELEMENT)) {
        rj_obj_set(req, "id", rj_copy(id));
    } else {
        rj_free(req);
        wd_fail(c, "invalid argument",
                "'id' must be null, an integer 0-65535 or an element reference");
        return;
    }
    RjValue *frame = wd_atom(s, w, req, FALSE, c);
    if (wd_is(frame, RJ_STR)) {
        g_ptr_array_add(w->frame_path, g_strdup(frame->str));
        wd_set_value(c, rj_new_null());
    }
    rj_free(frame);
}

static void
wd_cmd_parent_frame(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    if (w->frame_path->len > 0)
        g_ptr_array_remove_index(w->frame_path, w->frame_path->len - 1);
    wd_set_value(c, rj_new_null());
}

static RjValue *
wd_rect_json(wd_session *s)
{
    RjValue *r = rj_new_obj();
    rj_obj_set(r, "x", rj_new_num(s->win_x));
    rj_obj_set(r, "y", rj_new_num(s->win_y));
    rj_obj_set(r, "width", rj_new_num(s->vw));
    rj_obj_set(r, "height", rj_new_num(s->vh));
    return r;
}

static void
wd_resize(wd_session *s, int width, int height)
{
    s->vw = CLAMP(width, 1, MAX(WD_MAX_WIDTH, g_opts.viewport_width));
    s->vh = CLAMP(height, 1, MAX(WD_MAX_HEIGHT, g_opts.viewport_height));
    for (guint i = 0; i < s->windows->len; i++) {
        wd_window *w = g_ptr_array_index(s->windows, i);
        ns_rproc_http_page pg;
        memset(&pg, 0, sizeof pg);
        if (w->proc) ns_rproc_http_set_viewport(w->proc, s->vw, s->vh, &pg);
        ns_rproc_http_page_clear(&pg);
        if (!wd_window_render(s, w, NULL)) wd_window_render(s, w, NULL);
    }
}

static void
wd_cmd_get_rect(wd_session *s, wd_call *c)
{
    if (wd_current(s, c)) wd_set_value(c, wd_rect_json(s));
}

static void
wd_cmd_set_rect(wd_session *s, wd_call *c)
{
    if (!wd_current(s, c)) return;
    static const char *const keys[] = { "x", "y", "width", "height" };
    double vals[4] = { 0 };
    gboolean have[4] = { FALSE };
    for (int i = 0; i < 4; i++) {
        const RjValue *v = wd_param(c, keys[i]);
        if (!v || wd_is(v, RJ_NULL)) continue;
        if (!wd_is_int(v, i < 2 ? -2147483648.0 : 0, 2147483647.0)) {
            wd_fail(c, "invalid argument", "'%s' must be an integer", keys[i]);
            return;
        }
        vals[i] = v->num;
        have[i] = TRUE;
    }
    if (have[2] || have[3])
        wd_resize(s, have[2] ? (int)vals[2] : s->vw, have[3] ? (int)vals[3] : s->vh);
    if (have[0]) s->win_x = (int)vals[0];
    if (have[1]) s->win_y = (int)vals[1];
    wd_set_value(c, wd_rect_json(s));
}

static void
wd_cmd_maximize(wd_session *s, wd_call *c)
{
    if (!wd_current(s, c)) return;
    s->win_x = 0;
    s->win_y = 0;
    wd_resize(s, WD_SCREEN_W, WD_SCREEN_H);
    wd_set_value(c, wd_rect_json(s));
}

static void
wd_cmd_minimize(wd_session *s, wd_call *c)
{
    if (wd_current(s, c)) wd_set_value(c, wd_rect_json(s));
}

static gboolean
wd_valid_strategy(const char *using)
{
    return using && (strcmp(using, "css selector") == 0 ||
                     strcmp(using, "link text") == 0 ||
                     strcmp(using, "partial link text") == 0 ||
                     strcmp(using, "tag name") == 0 ||
                     strcmp(using, "xpath") == 0);
}

static void
wd_find(wd_session *s, wd_call *c, const char *from_key, const char *from_id,
        gboolean multiple)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    const char *using = wd_param_string(c, "using");
    if (!using) return;
    if (!wd_valid_strategy(using)) {
        wd_fail(c, "invalid argument", "Unknown location strategy '%s'", using);
        return;
    }
    const char *value = wd_param_string(c, "value");
    if (!value) return;
    gint64 deadline = wd_now_ms() + s->t_implicit;
    for (;;) {
        RjValue *req = wd_req("find");
        rj_obj_set(req, "using", rj_new_str(using));
        rj_obj_set(req, "value", rj_new_str(value));
        rj_obj_set(req, "multiple", rj_new_bool(multiple));
        if (from_key) {
            RjValue *from = rj_new_obj();
            rj_obj_set(from, from_key, rj_new_str(from_id));
            rj_obj_set(req, "from", from);
        }
        RjValue *found = wd_atom(s, w, req, FALSE, c);
        gboolean empty = multiple ? wd_arr_len(found) == 0
                                  : g_strcmp0(c->error, "no such element") == 0;
        if (!empty || wd_now_ms() >= deadline) {
            if (found) wd_set_value(c, found);
            return;
        }
        rj_free(found);
        wd_clear_error(c);
        wd_pump(s, 50);
        if (s != g_session || s->current != w) {
            wd_fail(c, "no such window", "The window closed while searching");
            return;
        }
    }
}

static void wd_cmd_find(wd_session *s, wd_call *c) { wd_find(s, c, NULL, NULL, FALSE); }
static void wd_cmd_find_all(wd_session *s, wd_call *c) { wd_find(s, c, NULL, NULL, TRUE); }
static void wd_cmd_find_from(wd_session *s, wd_call *c) { wd_find(s, c, WD_ELEMENT, c->seg[3], FALSE); }
static void wd_cmd_find_all_from(wd_session *s, wd_call *c) { wd_find(s, c, WD_ELEMENT, c->seg[3], TRUE); }
static void wd_cmd_find_from_shadow(wd_session *s, wd_call *c) { wd_find(s, c, WD_SHADOW, c->seg[3], FALSE); }
static void wd_cmd_find_all_from_shadow(wd_session *s, wd_call *c) { wd_find(s, c, WD_SHADOW, c->seg[3], TRUE); }

static void
wd_element_query(wd_session *s, wd_call *c, const char *cmd, const char *name)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    RjValue *req = wd_req_id(cmd, c->seg[3]);
    if (name) rj_obj_set(req, "name", rj_new_str(name));
    RjValue *v = wd_atom(s, w, req, FALSE, c);
    if (v) wd_set_value(c, v);
}

static void
wd_cmd_active(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    RjValue *v = wd_atom(s, w, wd_req("active"), FALSE, c);
    if (v) wd_set_value(c, v);
}

static void wd_cmd_shadow(wd_session *s, wd_call *c) { wd_element_query(s, c, "shadow", NULL); }
static void wd_cmd_selected(wd_session *s, wd_call *c) { wd_element_query(s, c, "selected", NULL); }
static void wd_cmd_attribute(wd_session *s, wd_call *c) { wd_element_query(s, c, "attribute", c->seg[5]); }
static void wd_cmd_property(wd_session *s, wd_call *c) { wd_element_query(s, c, "property", c->seg[5]); }
static void wd_cmd_css(wd_session *s, wd_call *c) { wd_element_query(s, c, "css", c->seg[5]); }
static void wd_cmd_text(wd_session *s, wd_call *c) { wd_element_query(s, c, "text", NULL); }
static void wd_cmd_tag(wd_session *s, wd_call *c) { wd_element_query(s, c, "name", NULL); }
static void wd_cmd_rect(wd_session *s, wd_call *c) { wd_element_query(s, c, "rect", NULL); }
static void wd_cmd_enabled(wd_session *s, wd_call *c) { wd_element_query(s, c, "enabled", NULL); }
static void wd_cmd_displayed(wd_session *s, wd_call *c) { wd_element_query(s, c, "displayed", NULL); }
static void wd_cmd_role(wd_session *s, wd_call *c) { wd_element_query(s, c, "role", NULL); }
static void wd_cmd_label(wd_session *s, wd_call *c) { wd_element_query(s, c, "label", NULL); }

static char *
wd_pointer_release(wd_session *s, wd_window *w, int px, int py, int button,
                   int clicks)
{
    if (!w->proc) return NULL;
    int changed = 0, newtab = 0;
    char *href = ns_rproc_http_release_button(w->proc, px, py, button, clicks,
                                              &changed, &newtab);
    if (href && *href && newtab) {
        wd_window *nw = wd_window_new(s);
        if (nw) wd_window_open(s, nw, href, FALSE, TRUE, TRUE);
        free(href);
        return NULL;
    }
    return href;
}

static char *
wd_mouse_click(wd_session *s, wd_window *w, int px, int py, int mods)
{
    if (!w->proc) return NULL;
    ns_rproc_http_hover_buttons(w->proc, px, py, mods, 0, NULL, NULL);
    char *nav = ns_rproc_http_press(w->proc, px, py, mods, 0, 1);
    return wd_keep_nav(nav, wd_pointer_release(s, w, px, py, 0, 1));
}

static void
wd_cmd_click(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    gint64 start = wd_now_ms();
    RjValue *pt = wd_atom(s, w, wd_req_id("clickPoint", c->seg[3]), FALSE, c);
    if (!pt) return;
    char *nav = NULL;
    if (!rj_get_bool(pt, "done", FALSE) && !wd_window_render(s, w, NULL))
        nav = wd_mouse_click(s, w, (int)wd_num(pt, "x", 0) + w->scroll_x,
                             (int)wd_num(pt, "y", 0) + w->scroll_y,
                             wd_session_mods(s));
    rj_free(pt);
    wd_settle_after_input(s, w, nav, start, c);
    if (!wd_failed(c)) wd_set_value(c, rj_new_null());
}

static void
wd_cmd_clear(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    gint64 start = wd_now_ms();
    RjValue *v = wd_atom(s, w, wd_req_id("clear", c->seg[3]), FALSE, c);
    if (!v) return;
    rj_free(v);
    wd_settle_after_input(s, w, NULL, start, c);
    if (!wd_failed(c)) wd_set_value(c, rj_new_null());
}

static void
wd_caret_to_end(wd_window *w)
{
    if (!w->proc) return;
    size_t caret = 0, anchor = 0;
    char *value = ns_rproc_http_focused_editable_value(w->proc, &caret, &anchor);
    if (value) {
        size_t len = strlen(value);
        ns_rproc_http_set_focused_editable_selection(w->proc, len, len);
    }
    free(value);
}

static void
wd_cmd_send_keys(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    const char *text = rj_get_str(c->body, "text");
    char *joined = NULL;
    if (!text) {
        const RjValue *arr = wd_param(c, "value");
        if (!wd_is(arr, RJ_ARR)) {
            wd_fail(c, "invalid argument", "'text' must be a string");
            return;
        }
        GString *buf = g_string_new(NULL);
        for (guint i = 0; i < arr->items->len; i++) {
            const RjValue *it = g_ptr_array_index(arr->items, i);
            if (wd_is(it, RJ_STR)) g_string_append(buf, it->str);
        }
        text = joined = g_string_free(buf, FALSE);
    }
    gint64 start = wd_now_ms();
    RjValue *req = wd_req_id("keysPrepare", c->seg[3]);
    rj_obj_set(req, "strictFile", rj_new_bool(s->strict_file));
    RjValue *prep = wd_atom(s, w, req, FALSE, c);
    if (prep) {
        gboolean is_file = rj_get_bool(prep, "file", FALSE);
        rj_free(prep);
        if (is_file) {
            wd_fail(c, "unsupported operation",
                    "File inputs cannot be given files over WebDriver yet");
        } else {
            wd_window_render(s, w, NULL);
            wd_caret_to_end(w);
            char *nav = wd_type_string(w, text);
            wd_settle_after_input(s, w, nav, start, c);
            if (!wd_failed(c)) wd_set_value(c, rj_new_null());
        }
    }
    g_free(joined);
}

static void
wd_cmd_source(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    RjValue *v = wd_atom(s, w, wd_req("source"), FALSE, c);
    if (v) wd_set_value(c, v);
}

static void
wd_execute(wd_session *s, wd_call *c, gboolean async)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    const char *script = wd_param_string(c, "script");
    if (!script) return;
    const RjValue *args = wd_param(c, "args");
    if (!wd_is(args, RJ_ARR)) {
        wd_fail(c, "invalid argument", "'args' must be an array");
        return;
    }
    RjValue *req = wd_req("execute");
    rj_obj_set(req, "script", rj_new_str(script));
    rj_obj_set(req, "args", rj_copy(args));
    rj_obj_set(req, "async", rj_new_bool(async));
    gint64 start = wd_now_ms();
    RjValue *reply = wd_atom_raw(w, req, FALSE, c);
    while (reply && rj_get_str(reply, "pending")) {
        char *token = g_strdup(rj_get_str(reply, "pending"));
        rj_free(reply);
        reply = NULL;
        if (s->t_script >= 0 && wd_now_ms() - start >= s->t_script) {
            RjValue *cancel = wd_req("cancel");
            rj_obj_set(cancel, "token", rj_new_str(token));
            rj_free(wd_atom_raw(w, cancel, TRUE, c));
            g_free(token);
            wd_clear_error(c);
            wd_fail(c, "script timeout",
                    "The script did not finish within %" G_GINT64_FORMAT " ms",
                    s->t_script);
            return;
        }
        wd_pump(s, 5);
        if (s != g_session || s->current != w) {
            g_free(token);
            wd_fail(c, "no such window", "The window closed while the script ran");
            return;
        }
        RjValue *poll = wd_req("poll");
        rj_obj_set(poll, "token", rj_new_str(token));
        g_free(token);
        reply = wd_atom_raw(w, poll, TRUE, c);
    }
    if (!reply) return;
    if (wd_has(reply, "error")) {
        wd_atom_error(s, reply, c);
    } else {
        const RjValue *v = rj_get(reply, "value");
        RjValue *out = v ? rj_copy(v) : rj_new_null();
        wd_collect_issued(s, out);
        wd_set_value(c, out);
    }
    rj_free(reply);
    w->navigated = FALSE;
    wd_window_render(s, w, NULL);
}

static void wd_cmd_execute_sync(wd_session *s, wd_call *c) { wd_execute(s, c, FALSE); }
static void wd_cmd_execute_async(wd_session *s, wd_call *c) { wd_execute(s, c, TRUE); }

static RjValue *
wd_cookie_list(wd_window *w)
{
    char *body = w->proc ? ns_rproc_http_cookies(w->proc) : NULL;
    RjValue *list = body ? rj_parse(body, strlen(body)) : NULL;
    free(body);
    if (!wd_is(list, RJ_ARR)) {
        rj_free(list);
        list = rj_new_arr();
    }
    return list;
}

static RjValue *
wd_cookie_json(const RjValue *ck)
{
    RjValue *o = rj_new_obj();
    const char *keys[] = { "name", "value", "path", "domain" };
    for (gsize i = 0; i < G_N_ELEMENTS(keys); i++) {
        const char *v = rj_get_str(ck, keys[i]);
        rj_obj_set(o, keys[i], rj_new_str(v ? v : ""));
    }
    rj_obj_set(o, "secure", rj_new_bool(rj_get_bool(ck, "secure", FALSE)));
    rj_obj_set(o, "httpOnly", rj_new_bool(rj_get_bool(ck, "httpOnly", FALSE)));
    double expires = 0;
    if (rj_get_num(ck, "expires", &expires) && expires > 0)
        rj_obj_set(o, "expiry", rj_new_num(floor(expires / 1000.0)));
    const char *ss = rj_get_str(ck, "sameSite");
    rj_obj_set(o, "sameSite", rj_new_str(ss && *ss ? ss : "Lax"));
    return o;
}

static void
wd_cmd_get_cookies(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    RjValue *list = wd_cookie_list(w);
    RjValue *out = rj_new_arr();
    for (guint i = 0; i < list->items->len; i++)
        rj_arr_add(out, wd_cookie_json(g_ptr_array_index(list->items, i)));
    rj_free(list);
    wd_set_value(c, out);
}

static void
wd_cmd_get_cookie(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    RjValue *list = wd_cookie_list(w);
    for (guint i = 0; i < list->items->len && !c->value; i++) {
        const RjValue *ck = g_ptr_array_index(list->items, i);
        if (g_strcmp0(rj_get_str(ck, "name"), c->seg[3]) == 0)
            wd_set_value(c, wd_cookie_json(ck));
    }
    rj_free(list);
    if (!c->value) wd_fail(c, "no such cookie", "No cookie named %s", c->seg[3]);
}

static gboolean
wd_cookie_text_ok(const char *s, gboolean name)
{
    for (const char *p = s; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == ';' || (name && *p == '='))
            return FALSE;
    return TRUE;
}

static gboolean
wd_domain_matches(const char *domain, const char *host)
{
    const char *d = domain[0] == '.' ? domain + 1 : domain;
    gsize dl = strlen(d), hl = strlen(host);
    return dl > 0 && (g_ascii_strcasecmp(host, d) == 0 ||
                      (hl > dl && host[hl - dl - 1] == '.' &&
                       g_ascii_strcasecmp(host + hl - dl, d) == 0));
}

static char *
wd_document_url(wd_session *s, wd_window *w)
{
    RjValue *info = wd_probe(s, w, wd_req("info"), TRUE);
    const char *u = info ? rj_get_str(info, "url") : NULL;
    char *url = u && *u ? g_strdup(u) : NULL;
    rj_free(info);
    return url;
}

static void
wd_cmd_add_cookie(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    const RjValue *ck = wd_param(c, "cookie");
    const char *name = ck ? rj_get_str(ck, "name") : NULL;
    const char *value = ck ? rj_get_str(ck, "value") : NULL;
    if (!wd_is(ck, RJ_OBJ) || !name || !value) {
        wd_fail(c, "invalid argument", "a cookie needs a string name and value");
        return;
    }
    static const struct { const char *key; RjType type; } fields[] = {
        { "path", RJ_STR }, { "domain", RJ_STR }, { "secure", RJ_BOOL },
        { "httpOnly", RJ_BOOL }, { "expiry", RJ_NUM }, { "sameSite", RJ_STR },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(fields); i++) {
        const RjValue *v = rj_get(ck, fields[i].key);
        if (v && !wd_is(v, RJ_NULL) && !wd_is(v, fields[i].type)) {
            wd_fail(c, "invalid argument", "cookie field '%s' has the wrong type",
                    fields[i].key);
            return;
        }
    }
    const RjValue *expiry = rj_get(ck, "expiry");
    const char *same_site = rj_get_str(ck, "sameSite");
    if ((wd_is(expiry, RJ_NUM) && !wd_is_int(expiry, 0, WD_MAX_SAFE_INT)) ||
        (same_site && strcmp(same_site, "Lax") != 0 &&
         strcmp(same_site, "Strict") != 0 && strcmp(same_site, "None") != 0)) {
        wd_fail(c, "invalid argument", "invalid cookie expiry or sameSite");
        return;
    }
    const char *path = rj_get_str(ck, "path");
    const char *domain = rj_get_str(ck, "domain");
    if (!*name || !wd_cookie_text_ok(name, TRUE) || !wd_cookie_text_ok(value, FALSE) ||
        (path && !wd_cookie_text_ok(path, FALSE)) ||
        (domain && !wd_cookie_text_ok(domain, FALSE))) {
        wd_fail(c, "unable to set cookie", "The cookie has characters a cookie cannot hold");
        return;
    }
    char *url = wd_document_url(s, w);
    char *host = NULL;
    if (url) {
        GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
        const char *scheme = uri ? g_uri_get_scheme(uri) : NULL;
        if (uri && scheme && (g_ascii_strcasecmp(scheme, "http") == 0 ||
                              g_ascii_strcasecmp(scheme, "https") == 0))
            host = g_strdup(g_uri_get_host(uri));
        if (uri) g_uri_unref(uri);
    }
    if (!host || (domain && *domain && !wd_domain_matches(domain, host))) {
        wd_fail(c, "invalid cookie domain",
                "The cookie domain does not match the current document");
        g_free(host);
        g_free(url);
        return;
    }
    GString *line = g_string_new(NULL);
    g_string_append_printf(line, "%s=%s; Path=%s", name, value,
                           path && path[0] == '/' ? path : "/");
    if (domain && *domain) g_string_append_printf(line, "; Domain=%s", domain);
    if (rj_get_bool(ck, "secure", FALSE)) g_string_append(line, "; Secure");
    if (rj_get_bool(ck, "httpOnly", FALSE)) g_string_append(line, "; HttpOnly");
    if (same_site) g_string_append_printf(line, "; SameSite=%s", same_site);
    if (wd_is(expiry, RJ_NUM)) {
        GDateTime *dt = g_date_time_new_from_unix_utc((gint64)expiry->num);
        char *when = dt ? g_date_time_format(dt, "%a, %d %b %Y %H:%M:%S GMT") : NULL;
        if (when) g_string_append_printf(line, "; Expires=%s", when);
        g_free(when);
        if (dt) g_date_time_unref(dt);
    }
    if (ns_rproc_http_cookie_set(w->proc, line->str))
        wd_set_value(c, rj_new_null());
    else
        wd_fail(c, "unable to set cookie", "The cookie %s was not stored", name);
    g_string_free(line, TRUE);
    g_free(host);
    g_free(url);
}

static void
wd_cmd_delete_cookie(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    const char *only = c->nseg > 3 ? c->seg[3] : NULL;
    RjValue *list = wd_cookie_list(w);
    for (guint i = 0; i < list->items->len; i++) {
        const RjValue *ck = g_ptr_array_index(list->items, i);
        const char *name = rj_get_str(ck, "name");
        if (!name || (only && strcmp(only, name) != 0)) continue;
        const char *path = rj_get_str(ck, "path");
        const char *domain = rj_get_str(ck, "domain");
        GString *line = g_string_new(NULL);
        g_string_append_printf(line, "%s=; Max-Age=0; Path=%s", name,
                               path && *path ? path : "/");
        if (!rj_get_bool(ck, "hostOnly", TRUE) && domain && *domain)
            g_string_append_printf(line, "; Domain=%s", domain);
        if (rj_get_bool(ck, "secure", FALSE)) g_string_append(line, "; Secure");
        if (rj_get_bool(ck, "httpOnly", FALSE)) g_string_append(line, "; HttpOnly");
        ns_rproc_http_cookie_set(w->proc, line->str);
        g_string_free(line, TRUE);
    }
    rj_free(list);
    wd_set_value(c, rj_new_null());
}

static wd_source *
wd_source_get(wd_session *s, const char *id, wd_source_type type,
              const char *pointer_type, wd_call *c)
{
    for (guint i = 0; i < s->sources->len; i++) {
        wd_source *src = g_ptr_array_index(s->sources, i);
        if (strcmp(src->id, id) != 0) continue;
        if (src->type != type ||
            (type == WD_SRC_POINTER && g_strcmp0(src->pointer_type, pointer_type) != 0)) {
            wd_fail(c, "invalid argument",
                    "input source %s was used with a different type", id);
            return NULL;
        }
        return src;
    }
    wd_source *src = g_new0(wd_source, 1);
    src->id = g_strdup(id);
    src->type = type;
    src->pointer_type = g_strdup(pointer_type);
    src->pressed = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(s->sources, src);
    return src;
}

static gboolean
wd_action_key(const char *value, gboolean shift_down, wd_key *k)
{
    if (!value || !*value) return FALSE;
    gunichar ch = g_utf8_get_char(value);
    const char *next = g_utf8_next_char(value);
    for (const char *p = next; *p; p = g_utf8_next_char(p)) {
        gunichar m = g_utf8_get_char(p);
        GUnicodeType t = g_unichar_type(m);
        if (t != G_UNICODE_NON_SPACING_MARK && t != G_UNICODE_SPACING_MARK &&
            t != G_UNICODE_ENCLOSING_MARK && m != 0x200D && m != 0xFE0F)
            return FALSE;
    }
    wd_key_lookup(ch, shift_down, k);
    if (*next && k->printable && !k->modifier && ch < 0xE000)
        g_strlcpy(k->key, value, sizeof k->key);
    return TRUE;
}

static gboolean
wd_validate_action(const char *src_type, const RjValue *a, wd_call *c)
{
    if (!wd_is(a, RJ_OBJ)) {
        wd_fail(c, "invalid argument", "an action must be an object");
        return FALSE;
    }
    const char *type = rj_get_str(a, "type");
    if (!type) {
        wd_fail(c, "invalid argument", "an action needs a type");
        return FALSE;
    }
    const RjValue *dur = rj_get(a, "duration");
    if (dur && !wd_is(dur, RJ_NULL) && !wd_is_int(dur, 0, WD_MAX_SAFE_INT)) {
        wd_fail(c, "invalid argument", "duration must be a non-negative integer");
        return FALSE;
    }
    if (strcmp(type, "pause") == 0) return TRUE;
    if (strcmp(src_type, "key") == 0) {
        wd_key k;
        if (strcmp(type, "keyDown") != 0 && strcmp(type, "keyUp") != 0) goto unknown;
        if (!wd_action_key(rj_get_str(a, "value"), FALSE, &k)) {
            wd_fail(c, "invalid argument", "a key action value must be a single key");
            return FALSE;
        }
        return TRUE;
    }
    if (strcmp(src_type, "pointer") == 0) {
        if (strcmp(type, "pointerDown") == 0 || strcmp(type, "pointerUp") == 0) {
            if (!wd_is_int(rj_get(a, "button"), 0, WD_MAX_SAFE_INT)) {
                wd_fail(c, "invalid argument", "button must be a non-negative integer");
                return FALSE;
            }
            return TRUE;
        }
        if (strcmp(type, "pointerCancel") == 0) return TRUE;
        if (strcmp(type, "pointerMove") != 0) goto unknown;
    } else if (strcmp(src_type, "wheel") == 0) {
        if (strcmp(type, "scroll") != 0) goto unknown;
        if (!wd_is_int(rj_get(a, "deltaX"), -WD_MAX_SAFE_INT, WD_MAX_SAFE_INT) ||
            !wd_is_int(rj_get(a, "deltaY"), -WD_MAX_SAFE_INT, WD_MAX_SAFE_INT)) {
            wd_fail(c, "invalid argument", "deltaX and deltaY must be integers");
            return FALSE;
        }
        if (!rj_get(a, "x") || !rj_get(a, "y")) {
            wd_fail(c, "invalid argument", "a scroll needs x and y");
            return FALSE;
        }
    } else {
        goto unknown;
    }
    static const char *const xy[] = { "x", "y" };
    for (int i = 0; i < 2; i++) {
        const RjValue *v = rj_get(a, xy[i]);
        if (v && !wd_is(v, RJ_NULL) && !(wd_is(v, RJ_NUM) && isfinite(v->num))) {
            wd_fail(c, "invalid argument", "%s must be a number", xy[i]);
            return FALSE;
        }
    }
    const RjValue *origin = rj_get(a, "origin");
    if (origin && !wd_is(origin, RJ_NULL)) {
        gboolean ok = wd_is(origin, RJ_STR)
            ? (strcmp(origin->str, "viewport") == 0 ||
               (strcmp(origin->str, "pointer") == 0 && strcmp(type, "scroll") != 0))
            : wd_is(origin, RJ_OBJ) && rj_get_str(origin, WD_ELEMENT);
        if (!ok) {
            wd_fail(c, "invalid argument", "origin must be viewport, pointer or an element");
            return FALSE;
        }
    }
    return TRUE;
unknown:
    wd_fail(c, "invalid argument", "unknown %s action type '%s'", src_type, type);
    return FALSE;
}

static gboolean
wd_action_point(wd_session *s, wd_window *w, wd_source *src, const RjValue *a,
                double *out_x, double *out_y, wd_call *c)
{
    double x = wd_num(a, "x", 0), y = wd_num(a, "y", 0);
    const RjValue *origin = rj_get(a, "origin");
    if (wd_is(origin, RJ_STR) && strcmp(origin->str, "pointer") == 0) {
        x += src ? src->x : 0;
        y += src ? src->y : 0;
    } else if (wd_is(origin, RJ_OBJ)) {
        RjValue *ctr = wd_atom(s, w, wd_req_id("center", rj_get_str(origin, WD_ELEMENT)),
                               FALSE, c);
        if (!ctr) return FALSE;
        x += wd_num(ctr, "x", 0);
        y += wd_num(ctr, "y", 0);
        rj_free(ctr);
    }
    if (x < 0 || y < 0 || x >= s->vw || y >= s->vh) {
        wd_fail(c, "move target out of bounds",
                "(%g, %g) is outside the %dx%d viewport", x, y, s->vw, s->vh);
        return FALSE;
    }
    *out_x = x;
    *out_y = y;
    return TRUE;
}

static void
wd_pointer_move_to(wd_session *s, wd_window *w, wd_source *src, double x,
                   double y)
{
    if (x != src->x || y != src->y) src->moved = TRUE;
    src->x = x;
    src->y = y;
    if (w->proc)
        ns_rproc_http_hover_buttons(w->proc, (int)x + w->scroll_x,
                                    (int)y + w->scroll_y, wd_session_mods(s),
                                    (int)src->buttons, NULL, NULL);
}

static void
wd_follow(wd_session *s, wd_window *w, char *nav, wd_call *c)
{
    if (nav && *nav) {
        wd_window_open(s, w, nav, FALSE, TRUE, TRUE);
        wd_wait_for_load(s, w, wd_now_ms(), c);
    }
    free(nav);
}

static gint64
wd_dispatch_key_action(wd_session *s, wd_window *w, wd_source *src,
                       const RjValue *a, wd_call *c)
{
    const char *type = rj_get_str(a, "type");
    wd_key k;
    wd_action_key(rj_get_str(a, "value"), (wd_session_mods(s) & 1) != 0, &k);
    guint at = src->pressed->len;
    for (guint i = 0; i < src->pressed->len; i++)
        if (strcmp(g_ptr_array_index(src->pressed, i), k.key) == 0) at = i;
    char *nav = NULL;
    if (strcmp(type, "keyDown") == 0) {
        if (at == src->pressed->len) g_ptr_array_add(src->pressed, g_strdup(k.key));
        nav = wd_send_key_down(w, &k, wd_session_mods(s));
    } else {
        for (guint i = 0; at == src->pressed->len && i < src->pressed->len; i++)
            if (g_ascii_strcasecmp(g_ptr_array_index(src->pressed, i), k.key) == 0)
                at = i;
        if (at == src->pressed->len) return 0;
        g_ptr_array_remove_index(src->pressed, at);
        nav = wd_send_key_up(w, &k, wd_session_mods(s));
    }
    wd_follow(s, w, nav, c);
    return 0;
}

static void
wd_dispatch_button_action(wd_session *s, wd_window *w, wd_source *src,
                          const RjValue *a, wd_call *c)
{
    const char *type = rj_get_str(a, "type");
    guint button = (guint)wd_num(a, "button", 0);
    guint bit = button < 31 ? 1u << button : 0;
    int px = (int)src->x + w->scroll_x, py = (int)src->y + w->scroll_y;
    int mods = wd_session_mods(s);
    char *nav = NULL;
    if (!w->proc) return;
    if (strcmp(type, "pointerDown") == 0) {
        if (src->buttons & bit) return;
        gint64 now = wd_now_ms();
        gboolean again = src->clicks >= 1 && now - src->last_click_ms <= 500 &&
                         fabs(src->x - src->click_x) <= 4 &&
                         fabs(src->y - src->click_y) <= 4;
        src->clicks = again ? MIN(src->clicks + 1, 3) : 1;
        src->last_click_ms = now;
        src->click_x = src->x;
        src->click_y = src->y;
        src->buttons |= bit;
        src->moved = FALSE;
        nav = ns_rproc_http_press(w->proc, px, py, mods, (int)button, src->clicks);
        if (button == 0 && src->clicks >= 2)
            free(ns_rproc_http_select(w->proc, src->clicks == 2 ? 5 : 6, px, py));
        if (button == 2)
            ns_rproc_http_contextmenu(w->proc, px, py, NULL, NULL);
    } else {
        if (!(src->buttons & bit)) return;
        src->buttons &= ~bit;
        nav = wd_pointer_release(s, w, px, py, (int)button,
                                 src->moved ? 1 : src->clicks);
        if (src->moved) src->clicks = 0;
    }
    wd_follow(s, w, nav, c);
}

static void
wd_dispatch_scroll_action(wd_session *s, wd_window *w, const RjValue *a,
                          wd_call *c)
{
    double tx, ty;
    if (!wd_action_point(s, w, NULL, a, &tx, &ty, c)) return;
    ns_rproc_http_wheel wheel;
    memset(&wheel, 0, sizeof wheel);
    wheel.x = wheel.ev_x = (int)tx + w->scroll_x;
    wheel.y = wheel.ev_y = (int)ty + w->scroll_y;
    wheel.dx = wheel.ev_dx = (int)wd_num(a, "deltaX", 0);
    wheel.dy = wheel.ev_dy = (int)wd_num(a, "deltaY", 0);
    wheel.ev = 1;
    wheel.ev_mods = wd_session_mods(s);
    if (wd_window_render_wheel(s, w, &wheel, NULL)) return;
    wd_pump(s, WD_SCROLL_SETTLE_MS);
    wd_window_tick(s, w);
}

static gint64
wd_dispatch_action(wd_session *s, wd_window *w, wd_source *src,
                   const RjValue *a, wd_call *c)
{
    const char *type = rj_get_str(a, "type");
    gint64 duration = (gint64)wd_num(a, "duration", 0);
    if (strcmp(type, "pause") == 0) return duration;
    if (strcmp(type, "keyDown") == 0 || strcmp(type, "keyUp") == 0)
        return wd_dispatch_key_action(s, w, src, a, c);
    if (strcmp(type, "pointerDown") == 0 || strcmp(type, "pointerUp") == 0) {
        wd_dispatch_button_action(s, w, src, a, c);
    } else if (strcmp(type, "pointerMove") == 0) {
        double tx, ty;
        if (!wd_action_point(s, w, src, a, &tx, &ty, c)) return 0;
        double sx = src->x, sy = src->y;
        gint64 t0 = wd_now_ms();
        for (gint64 el = 0; duration > 0 && el < duration; el = wd_now_ms() - t0) {
            double f = (double)el / (double)duration;
            wd_pointer_move_to(s, w, src, floor(sx + (tx - sx) * f),
                               floor(sy + (ty - sy) * f));
            wd_pump(s, 16);
        }
        wd_pointer_move_to(s, w, src, tx, ty);
    } else if (strcmp(type, "scroll") == 0) {
        wd_dispatch_scroll_action(s, w, a, c);
    }
    return duration;
}

static void
wd_cmd_perform_actions(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    const RjValue *actions = wd_param(c, "actions");
    if (!wd_is(actions, RJ_ARR)) {
        wd_fail(c, "invalid argument", "'actions' must be an array");
        return;
    }
    GArray *seqs = g_array_new(FALSE, FALSE, sizeof(wd_sequence));
    guint ticks = 0;
    for (guint i = 0; i < actions->items->len && !wd_failed(c); i++) {
        const RjValue *seq = g_ptr_array_index(actions->items, i);
        const char *type = rj_get_str(seq, "type");
        const char *id = rj_get_str(seq, "id");
        const RjValue *list = rj_get(seq, "actions");
        if (!type || !id || !wd_is(list, RJ_ARR)) {
            wd_fail(c, "invalid argument",
                    "an action sequence needs a type, an id and an actions array");
            break;
        }
        wd_source_type st;
        if (strcmp(type, "none") == 0) st = WD_SRC_NONE;
        else if (strcmp(type, "key") == 0) st = WD_SRC_KEY;
        else if (strcmp(type, "pointer") == 0) st = WD_SRC_POINTER;
        else if (strcmp(type, "wheel") == 0) st = WD_SRC_WHEEL;
        else {
            wd_fail(c, "invalid argument", "unknown input source type '%s'", type);
            break;
        }
        const char *ptype = NULL;
        if (st == WD_SRC_POINTER) {
            ptype = rj_get_str(rj_get(seq, "parameters"), "pointerType");
            if (!ptype) ptype = "mouse";
            if (strcmp(ptype, "mouse") != 0 && strcmp(ptype, "pen") != 0 &&
                strcmp(ptype, "touch") != 0) {
                wd_fail(c, "invalid argument", "unknown pointerType '%s'", ptype);
                break;
            }
        }
        for (guint j = 0; j < list->items->len; j++)
            if (!wd_validate_action(type, g_ptr_array_index(list->items, j), c)) break;
        if (wd_failed(c)) break;
        wd_source *src = wd_source_get(s, id, st, ptype, c);
        if (!src) break;
        wd_sequence sq = { src, list };
        g_array_append_val(seqs, sq);
        ticks = MAX(ticks, list->items->len);
    }
    for (guint t = 0; t < ticks && !wd_failed(c); t++) {
        gint64 tick_start = wd_now_ms(), tick_ms = 0;
        for (guint i = 0; i < seqs->len && !wd_failed(c); i++) {
            wd_sequence *sq = &g_array_index(seqs, wd_sequence, i);
            if (t >= sq->list->items->len || s->current != w) continue;
            gint64 d = wd_dispatch_action(s, w, sq->src,
                                          g_ptr_array_index(sq->list->items, t), c);
            tick_ms = MAX(tick_ms, d);
        }
        if (s->current != w) break;
        wd_window_render(s, w, NULL);
        gint64 left = tick_start + tick_ms - wd_now_ms();
        if (left > 0) wd_pump(s, (int)left);
    }
    g_array_free(seqs, TRUE);
    if (!wd_failed(c)) wd_set_value(c, rj_new_null());
}

static void
wd_cmd_release_actions(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    for (guint i = s->sources->len; i > 0; i--) {
        wd_source *src = g_ptr_array_index(s->sources, i - 1);
        while (src->type == WD_SRC_KEY && src->pressed->len > 0) {
            char *name = g_ptr_array_steal_index(src->pressed, src->pressed->len - 1);
            wd_key k;
            wd_action_key(name, FALSE, &k);
            free(wd_send_key_up(w, &k, wd_session_mods(s)));
            g_free(name);
        }
        for (int b = 0; src->type == WD_SRC_POINTER && b < 3; b++)
            if (src->buttons & (1u << b))
                free(wd_pointer_release(s, w, (int)src->x + w->scroll_x,
                                        (int)src->y + w->scroll_y, b, 1));
        src->buttons = 0;
    }
    g_ptr_array_set_size(s->sources, 0);
    wd_window_render(s, w, NULL);
    wd_set_value(c, rj_new_null());
}

static void
wd_cmd_alert(wd_session *s, wd_call *c)
{
    if (!wd_current(s, c)) return;
    wd_fail(c, "no such alert", "No user prompt is open");
}

static cairo_status_t
wd_byte_sink(void *closure, const unsigned char *data, unsigned int length)
{
    g_byte_array_append((GByteArray *)closure, data, length);
    return CAIRO_STATUS_SUCCESS;
}

static void
wd_cmd_screenshot(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    if (wd_window_render(s, w, NULL)) wd_window_render(s, w, NULL);
    ns_rproc_http_frame fr;
    wd_window_render(s, w, &fr);
    char *b64 = NULL;
    if (fr.ok && fr.pixels && fr.width > 0 && fr.height > 0) {
        cairo_surface_t *src = cairo_image_surface_create_for_data(
            (unsigned char *)fr.pixels, CAIRO_FORMAT_ARGB32, fr.width,
            fr.height, fr.stride);
        cairo_surface_t *dst =
            cairo_image_surface_create(CAIRO_FORMAT_RGB24, fr.width, fr.height);
        cairo_t *cr = cairo_create(dst);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        GByteArray *buf = g_byte_array_new();
        if (cairo_surface_write_to_png_stream(dst, wd_byte_sink, buf) ==
                CAIRO_STATUS_SUCCESS && buf->len)
            b64 = g_base64_encode(buf->data, buf->len);
        g_byte_array_free(buf, TRUE);
        cairo_surface_destroy(dst);
        cairo_surface_destroy(src);
    }
    ns_rproc_http_frame_clear(&fr);
    if (!b64) {
        wd_fail(c, "unable to capture screen", "The page could not be rendered");
        return;
    }
    wd_set_value(c, rj_new_str_take(b64));
}

static void
wd_cmd_element_screenshot(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    RjValue *box = wd_atom(s, w, wd_req_id("box", c->seg[3]), FALSE, c);
    if (!box) return;
    if (!wd_window_render(s, w, NULL)) wd_window_render(s, w, NULL);
    double x = wd_num(box, "x", 0) + w->scroll_x;
    double y = wd_num(box, "y", 0) + w->scroll_y;
    double width = ceil(wd_num(box, "width", 0)), height = ceil(wd_num(box, "height", 0));
    rj_free(box);
    size_t len = 0;
    unsigned char *png = width >= 1 && height >= 1 && w->proc
        ? ns_rproc_http_screenshot(w->proc, floor(x), floor(y), width, height,
                                   1.0, &len, NULL, NULL)
        : NULL;
    if (!png || !len) {
        free(png);
        wd_fail(c, "unable to capture screen", "The element has no area to capture");
        return;
    }
    wd_set_value(c, rj_new_str_take(g_base64_encode(png, len)));
    free(png);
}

static gboolean
wd_number_param(const RjValue *o, const char *key, double def, double min,
                double *out)
{
    const RjValue *v = o ? rj_get(o, key) : NULL;
    if (!v) {
        *out = def;
        return TRUE;
    }
    if (!wd_is(v, RJ_NUM) || !isfinite(v->num) || v->num < min) return FALSE;
    *out = v->num;
    return TRUE;
}

static char *
wd_page_ranges(const RjValue *ranges)
{
    GString *out = g_string_new(NULL);
    for (guint i = 0; ranges && i < ranges->items->len; i++) {
        const RjValue *r = g_ptr_array_index(ranges->items, i);
        if (out->len) g_string_append_c(out, ',');
        if (wd_is_int(r, 1, WD_MAX_SAFE_INT)) {
            g_string_append_printf(out, "%.0f", r->num);
        } else if (wd_is(r, RJ_STR) && *r->str &&
                   strspn(r->str, "0123456789- ") == strlen(r->str) &&
                   strchr(r->str, '-') == strrchr(r->str, '-')) {
            g_autofree char *trimmed = g_strstrip(g_strdup(r->str));
            g_string_append(out, trimmed);
        } else {
            g_string_free(out, TRUE);
            return NULL;
        }
    }
    return g_string_free(out, FALSE);
}

static void
wd_cmd_print(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    const RjValue *body = c->body;
    const char *orient = rj_get_str(body, "orientation");
    if (wd_has(body, "orientation") &&
        (!orient || (strcmp(orient, "portrait") != 0 && strcmp(orient, "landscape") != 0))) {
        wd_fail(c, "invalid argument", "orientation must be portrait or landscape");
        return;
    }
    double scale, pw, ph, mt, mb, ml, mr;
    const RjValue *page = rj_get(body, "page");
    const RjValue *margin = rj_get(body, "margin");
    const RjValue *ranges = rj_get(body, "pageRanges");
    if ((page && !wd_is(page, RJ_OBJ)) || (margin && !wd_is(margin, RJ_OBJ)) ||
        (ranges && !wd_is(ranges, RJ_ARR)) ||
        !wd_number_param(body, "scale", 1.0, 0.1, &scale) || scale > 2.0 ||
        !wd_number_param(page, "width", 21.59, 2.54 / 72.0, &pw) ||
        !wd_number_param(page, "height", 27.94, 2.54 / 72.0, &ph) ||
        !wd_number_param(margin, "top", 1.0, 0, &mt) ||
        !wd_number_param(margin, "bottom", 1.0, 0, &mb) ||
        !wd_number_param(margin, "left", 1.0, 0, &ml) ||
        !wd_number_param(margin, "right", 1.0, 0, &mr)) {
        wd_fail(c, "invalid argument", "invalid print parameters");
        return;
    }
    static const char *const bools[] = { "background", "shrinkToFit" };
    for (int i = 0; i < 2; i++) {
        const RjValue *v = rj_get(body, bools[i]);
        if (v && !wd_is(v, RJ_BOOL)) {
            wd_fail(c, "invalid argument", "%s must be a boolean", bools[i]);
            return;
        }
    }
    char *page_ranges = wd_page_ranges(ranges);
    if (!page_ranges) {
        wd_fail(c, "invalid argument", "invalid pageRanges");
        return;
    }
    double px_per_cm = 96.0 / 2.54;
    if ((pw - ml - mr) * px_per_cm < 1 || (ph - mt - mb) * px_per_cm < 1) {
        g_free(page_ranges);
        wd_fail(c, "unsupported operation", "The margins leave no room on the page");
        return;
    }
    RjValue *opts = rj_new_obj();
    rj_obj_set(opts, "width", rj_new_num(pw * px_per_cm));
    rj_obj_set(opts, "height", rj_new_num(ph * px_per_cm));
    rj_obj_set(opts, "margin_top", rj_new_num(mt * px_per_cm));
    rj_obj_set(opts, "margin_bottom", rj_new_num(mb * px_per_cm));
    rj_obj_set(opts, "margin_left", rj_new_num(ml * px_per_cm));
    rj_obj_set(opts, "margin_right", rj_new_num(mr * px_per_cm));
    rj_obj_set(opts, "scale", rj_new_num(scale));
    rj_obj_set(opts, "landscape", rj_new_num(orient && strcmp(orient, "landscape") == 0));
    rj_obj_set(opts, "ignore_css_page_size", rj_new_num(1));
    if (*page_ranges) rj_obj_set(opts, "page_ranges", rj_new_str(page_ranges));
    g_free(page_ranges);
    char *json = rj_to_string(opts);
    rj_free(opts);
    size_t len = 0;
    unsigned char *pdf = w->proc ? ns_rproc_http_pdf(w->proc, json, &len) : NULL;
    g_free(json);
    wd_window_render(s, w, NULL);
    if (!pdf || !len) {
        free(pdf);
        wd_fail(c, "unknown error", "The page could not be printed");
        return;
    }
    wd_set_value(c, rj_new_str_take(g_base64_encode(pdf, len)));
    free(pdf);
}

static const wd_route wd_routes[] = {
    { "POST",   "session", wd_cmd_new_session },
    { "GET",    "status", wd_cmd_status },
    { "DELETE", "session/*", wd_cmd_delete_session },
    { "GET",    "session/*/timeouts", wd_cmd_get_timeouts },
    { "POST",   "session/*/timeouts", wd_cmd_set_timeouts },
    { "POST",   "session/*/url", wd_cmd_navigate },
    { "GET",    "session/*/url", wd_cmd_get_url },
    { "POST",   "session/*/back", wd_cmd_back },
    { "POST",   "session/*/forward", wd_cmd_forward },
    { "POST",   "session/*/refresh", wd_cmd_refresh },
    { "GET",    "session/*/title", wd_cmd_get_title },
    { "GET",    "session/*/window", wd_cmd_get_window },
    { "DELETE", "session/*/window", wd_cmd_close_window },
    { "POST",   "session/*/window", wd_cmd_switch_window },
    { "GET",    "session/*/window/handles", wd_cmd_get_handles },
    { "POST",   "session/*/window/new", wd_cmd_new_window },
    { "POST",   "session/*/frame", wd_cmd_switch_frame },
    { "POST",   "session/*/frame/parent", wd_cmd_parent_frame },
    { "GET",    "session/*/window/rect", wd_cmd_get_rect },
    { "POST",   "session/*/window/rect", wd_cmd_set_rect },
    { "POST",   "session/*/window/maximize", wd_cmd_maximize },
    { "POST",   "session/*/window/minimize", wd_cmd_minimize },
    { "POST",   "session/*/window/fullscreen", wd_cmd_maximize },
    { "GET",    "session/*/element/active", wd_cmd_active },
    { "POST",   "session/*/element", wd_cmd_find },
    { "POST",   "session/*/elements", wd_cmd_find_all },
    { "POST",   "session/*/element/*/element", wd_cmd_find_from },
    { "POST",   "session/*/element/*/elements", wd_cmd_find_all_from },
    { "GET",    "session/*/element/*/shadow", wd_cmd_shadow },
    { "POST",   "session/*/shadow/*/element", wd_cmd_find_from_shadow },
    { "POST",   "session/*/shadow/*/elements", wd_cmd_find_all_from_shadow },
    { "GET",    "session/*/element/*/selected", wd_cmd_selected },
    { "GET",    "session/*/element/*/attribute/*", wd_cmd_attribute },
    { "GET",    "session/*/element/*/property/*", wd_cmd_property },
    { "GET",    "session/*/element/*/css/*", wd_cmd_css },
    { "GET",    "session/*/element/*/text", wd_cmd_text },
    { "GET",    "session/*/element/*/name", wd_cmd_tag },
    { "GET",    "session/*/element/*/rect", wd_cmd_rect },
    { "GET",    "session/*/element/*/enabled", wd_cmd_enabled },
    { "GET",    "session/*/element/*/displayed", wd_cmd_displayed },
    { "GET",    "session/*/element/*/computedrole", wd_cmd_role },
    { "GET",    "session/*/element/*/computedlabel", wd_cmd_label },
    { "POST",   "session/*/element/*/click", wd_cmd_click },
    { "POST",   "session/*/element/*/clear", wd_cmd_clear },
    { "POST",   "session/*/element/*/value", wd_cmd_send_keys },
    { "GET",    "session/*/element/*/screenshot", wd_cmd_element_screenshot },
    { "GET",    "session/*/source", wd_cmd_source },
    { "POST",   "session/*/execute/sync", wd_cmd_execute_sync },
    { "POST",   "session/*/execute/async", wd_cmd_execute_async },
    { "GET",    "session/*/cookie", wd_cmd_get_cookies },
    { "GET",    "session/*/cookie/*", wd_cmd_get_cookie },
    { "POST",   "session/*/cookie", wd_cmd_add_cookie },
    { "DELETE", "session/*/cookie/*", wd_cmd_delete_cookie },
    { "DELETE", "session/*/cookie", wd_cmd_delete_cookie },
    { "POST",   "session/*/actions", wd_cmd_perform_actions },
    { "DELETE", "session/*/actions", wd_cmd_release_actions },
    { "POST",   "session/*/alert/dismiss", wd_cmd_alert },
    { "POST",   "session/*/alert/accept", wd_cmd_alert },
    { "GET",    "session/*/alert/text", wd_cmd_alert },
    { "POST",   "session/*/alert/text", wd_cmd_alert },
    { "GET",    "session/*/screenshot", wd_cmd_screenshot },
    { "POST",   "session/*/print", wd_cmd_print },
};

static gboolean
wd_route_matches(const char *pattern, char **seg, int nseg)
{
    char **pat = g_strsplit(pattern, "/", -1);
    int n = (int)g_strv_length(pat);
    gboolean ok = n == nseg;
    for (int i = 0; ok && i < n; i++)
        if (strcmp(pat[i], "*") != 0 && strcmp(pat[i], seg[i]) != 0) ok = FALSE;
    g_strfreev(pat);
    return ok;
}

static int
wd_error_status(const char *code)
{
    static const struct { const char *code; int status; } map[] = {
        { "element click intercepted", 400 }, { "element not interactable", 400 },
        { "insecure certificate", 400 }, { "invalid argument", 400 },
        { "invalid cookie domain", 400 }, { "invalid element state", 400 },
        { "invalid selector", 400 }, { "invalid session id", 404 },
        { "javascript error", 500 }, { "move target out of bounds", 500 },
        { "no such alert", 404 }, { "no such cookie", 404 },
        { "no such element", 404 }, { "no such frame", 404 },
        { "no such window", 404 }, { "no such shadow root", 404 },
        { "script timeout", 500 }, { "session not created", 500 },
        { "stale element reference", 404 }, { "detached shadow root", 404 },
        { "timeout", 500 }, { "unable to set cookie", 500 },
        { "unable to capture screen", 500 }, { "unexpected alert open", 500 },
        { "unknown command", 404 }, { "unknown error", 500 },
        { "unknown method", 405 }, { "unsupported operation", 500 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
        if (strcmp(map[i].code, code) == 0) return map[i].status;
    return 500;
}

static char **
wd_split_path(const char *path, int *out_n)
{
    char **raw = g_strsplit(path, "/", -1);
    GPtrArray *segs = g_ptr_array_new();
    for (int i = 0; raw[i]; i++) {
        if (!*raw[i]) continue;
        char *seg = g_uri_unescape_string(raw[i], NULL);
        g_ptr_array_add(segs, seg ? seg : g_strdup(""));
    }
    g_strfreev(raw);
    if (segs->len >= 2 && g_strcmp0(g_ptr_array_index(segs, 0), "wd") == 0 &&
        g_strcmp0(g_ptr_array_index(segs, 1), "hub") == 0) {
        g_free(g_ptr_array_index(segs, 0));
        g_free(g_ptr_array_index(segs, 1));
        g_ptr_array_remove_range(segs, 0, 2);
    }
    *out_n = (int)segs->len;
    g_ptr_array_add(segs, NULL);
    return (char **)g_ptr_array_free(segs, FALSE);
}

static void
wd_handle(wd_request *r)
{
    wd_call c = { 0 };
    c.method = r->method;
    c.seg = wd_split_path(r->path, &c.nseg);
    if (strcmp(r->method, "POST") == 0) {
        c.body = r->body_len ? rj_parse(r->body, r->body_len) : rj_new_obj();
        if (!c.body)
            wd_fail(&c, "invalid argument", "The body is not valid JSON");
        else if (c.body->type != RJ_OBJ)
            wd_fail(&c, "invalid argument", "The body must be a JSON object");
    }
    if (g_wd_log)
        g_printerr("[webdriver] -> %s %s %.*s\n", r->method, r->path,
                   (int)MIN(r->body_len, 400), r->body ? r->body : "");
    const wd_route *route = NULL;
    gboolean path_known = FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(wd_routes) && !wd_failed(&c); i++) {
        if (!wd_route_matches(wd_routes[i].pattern, c.seg, c.nseg)) continue;
        path_known = TRUE;
        if (strcmp(wd_routes[i].method, r->method) == 0) {
            route = &wd_routes[i];
            break;
        }
    }
    if (!wd_failed(&c)) {
        if (!route)
            wd_fail(&c, path_known ? "unknown method" : "unknown command",
                    "%s %s is not a WebDriver command", r->method, r->path);
        else if (c.nseg >= 2 && strcmp(c.seg[0], "session") == 0 &&
                 (!g_session || strcmp(g_session->id, c.seg[1]) != 0))
            wd_fail(&c, "invalid session id", "No active session with id %s",
                    c.seg[1]);
        else
            route->fn(g_session, &c);
    }
    GString *out = g_string_new("{\"value\":");
    int status = 200;
    if (c.error) {
        status = wd_error_status(c.error);
        RjValue *e = rj_new_obj();
        rj_obj_set(e, "error", rj_new_str(c.error));
        rj_obj_set(e, "message", rj_new_str(c.message ? c.message : ""));
        rj_obj_set(e, "stacktrace", rj_new_str(c.stacktrace ? c.stacktrace : ""));
        rj_write(out, e);
        rj_free(e);
    } else {
        rj_write(out, c.value);
    }
    g_string_append_c(out, '}');
    if (g_wd_log)
        g_printerr("[webdriver] <- %d %.*s\n", status, (int)MIN(out->len, 400),
                   out->str);
    rj_free(c.body);
    rj_free(c.value);
    wd_clear_error(&c);
    g_strfreev(c.seg);
    wd_request_complete(r, status, g_string_free(out, FALSE));
}

#ifndef G_OS_WIN32
static gboolean
wd_on_signal(gpointer data)
{
    (void)data;
    g_atomic_int_set(&g_wd_quit, 1);
    return G_SOURCE_CONTINUE;
}
#endif

static GSocketListener *
wd_listen(int port, guint16 *out_port)
{
    GSocketListener *listener = g_socket_listener_new();
    GError *err = NULL;
    GInetAddress *lo4 = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *addr = g_inet_socket_address_new(lo4, (guint16)port);
    GSocketAddress *bound = NULL;
    gboolean ok = g_socket_listener_add_address(listener, addr,
                                                G_SOCKET_TYPE_STREAM,
                                                G_SOCKET_PROTOCOL_TCP, NULL,
                                                &bound, &err);
    g_object_unref(addr);
    g_object_unref(lo4);
    if (!ok) {
        g_printerr("nordstjernen: cannot listen for WebDriver on 127.0.0.1:%d: %s\n",
                   port, err ? err->message : "unknown error");
        g_clear_error(&err);
        g_object_unref(listener);
        return NULL;
    }
    *out_port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
    g_object_unref(bound);
    GInetAddress *lo6 = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV6);
    GSocketAddress *addr6 = g_inet_socket_address_new(lo6, *out_port);
    g_socket_listener_add_address(listener, addr6, G_SOCKET_TYPE_STREAM,
                                  G_SOCKET_PROTOCOL_TCP, NULL, NULL, NULL);
    g_object_unref(addr6);
    g_object_unref(lo6);
    return listener;
}

int
ns_webdriver_run(const ns_webdriver_opts *opts)
{
    g_opts = *opts;
    if (g_opts.viewport_width <= 0) g_opts.viewport_width = 1280;
    if (g_opts.viewport_height <= 0) g_opts.viewport_height = 800;
    g_opts.viewport_width = MIN(g_opts.viewport_width, NS_PROC_MAX_WIDTH);
    g_opts.viewport_height = MIN(g_opts.viewport_height, NS_PROC_MAX_HEIGHT);
    if (g_opts.settle_ms < 0) g_opts.settle_ms = 0;
    g_wd_log = opts->log;
    g_renderer_path = ns_proc_renderer_path();
    if (!g_renderer_path || !g_file_test(g_renderer_path, G_FILE_TEST_IS_EXECUTABLE)) {
        g_printerr("nordstjernen: the renderer %s is missing\n",
                   g_renderer_path ? g_renderer_path : "(unknown)");
        g_free(g_renderer_path);
        return 1;
    }
    guint16 port = 0;
    GSocketListener *listener = wd_listen(opts->port, &port);
    if (!listener) {
        g_free(g_renderer_path);
        return 1;
    }
    g_wd_queue = g_async_queue_new();
    g_atomic_int_set(&g_wd_quit, 0);
#ifndef G_OS_WIN32
    guint sig_int = g_unix_signal_add(SIGINT, wd_on_signal, NULL);
    guint sig_term = g_unix_signal_add(SIGTERM, wd_on_signal, NULL);
#endif
    g_print("Nordstjernen WebDriver %s listening on http://127.0.0.1:%u\n",
            NS_VERSION, port);
    fflush(stdout);
    GThread *acceptor = g_thread_new("wd-accept", wd_accept_thread, listener);
    while (!g_atomic_int_get(&g_wd_quit)) {
        wd_request *r = g_async_queue_timeout_pop(g_wd_queue, 4000);
        if (r) {
            wd_handle(r);
            continue;
        }
        while (g_main_context_iteration(NULL, FALSE))
            ;
        wd_pump_step(g_session);
    }
    wd_end_session();
    g_socket_listener_close(listener);
    for (wd_request *r; (r = g_async_queue_try_pop(g_wd_queue));)
        wd_request_complete(r, 503, wd_error_body("unknown error",
                                                  "The WebDriver server is stopping"));
#ifndef G_OS_WIN32
    g_source_remove(sig_int);
    g_source_remove(sig_term);
#endif
    g_thread_unref(acceptor);
    g_free(g_renderer_path);
    return 0;
}
