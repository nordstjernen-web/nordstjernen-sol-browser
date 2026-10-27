/* Nordstjernen — opt-in loopback control channel that lets a local MCP client drive the GUI. */

#include "remotectl.h"

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <openssl/rand.h>
#include <string.h>

#include "config.h"
#include "procview.h"
#include "procwindow.h"
#include "rjson.h"
#include "rproc_http.h"
#include "version.h"

#define RC_MAX_CONNS        4
#define RC_MAX_LINE         (16 * 1024 * 1024)
#define RC_AUTH_TIMEOUT_S   10
#define RC_POLL_MS          50
#define RC_LOAD_TIMEOUT_MS  30000
#define RC_SETTLE_MS        2000
#define RC_SETTLE_MIN_MS    120
#define RC_EVAL_TIMEOUT_MS  30000

typedef struct RcConn RcConn;

struct RcConn {
    int                refs;
    GSocketConnection *conn;
    GInputStream      *in;
    GOutputStream     *out;
    GCancellable      *cancel;
    GByteArray        *buf;
    char               chunk[65536];
    gboolean           authed;
    gboolean           busy;
    gboolean           reading;
    gboolean           closed;
    gboolean           close_after_write;
    guint              auth_timer;
};

typedef enum {
    RC_WAIT_SETTLE,
    RC_WAIT_LOAD,
    RC_WAIT_RENDER
} RcWaitKind;

typedef struct {
    int          refs;
    RcConn      *conn;
    char        *id_json;
    RjValue     *req;
    int          tab;
    gboolean     done;
    guint        timer;
    guint        timeout;
    gint64       started_us;
    gint64       deadline_us;
    RcWaitKind   wait;
    gboolean     saw_loading;
} RcOp;

static struct {
    gboolean        inited;
    gboolean        requested;
    gboolean        config_enabled;
    gboolean        stopped;
    GSocketService *service;
    guint16         port;
    char            token[65];
    char           *file_path;
    GList          *conns;
    GFileMonitor   *config_monitor;
    char           *client_name;
} rc;

static void rc_conn_read_more(RcConn *c);
static void rc_conn_process(RcConn *c);
static void rc_update_indicator(void);
static void rc_start(void);
static void rc_close_all(gboolean user_stopped);

char *
ns_remote_control_file_path(void)
{
    return g_build_filename(g_get_user_state_dir(), NS_APP_DIR_NAME,
                            NS_REMOTE_CONTROL_FILE, NULL);
}

gboolean
ns_remote_control_running(void)
{
    return rc.service != NULL;
}

void
ns_remote_control_request(void)
{
    rc.requested = TRUE;
}

static gboolean
rc_token_equal(const char *a, const char *b)
{
    if (!a || !b)
        return FALSE;
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = la != lb;
    for (size_t i = 0; i < lb; i++)
        diff |= (unsigned char)(b[i] ^ (i < la ? a[i] : 0));
    return diff == 0;
}

static RcConn *
rc_conn_ref(RcConn *c)
{
    c->refs++;
    return c;
}

static void
rc_conn_unref(RcConn *c)
{
    if (--c->refs > 0)
        return;
    g_clear_object(&c->cancel);
    g_clear_object(&c->conn);
    g_byte_array_free(c->buf, TRUE);
    g_free(c);
}

static int
rc_authed_count(void)
{
    int n = 0;
    for (GList *l = rc.conns; l; l = l->next)
        if (((RcConn *)l->data)->authed)
            n++;
    return n;
}

static void
rc_conn_close(RcConn *c)
{
    if (c->closed)
        return;
    c->closed = TRUE;
    if (c->auth_timer) {
        g_source_remove(c->auth_timer);
        c->auth_timer = 0;
    }
    g_cancellable_cancel(c->cancel);
    g_io_stream_close(G_IO_STREAM(c->conn), NULL, NULL);
    rc.conns = g_list_remove(rc.conns, c);
    rc_update_indicator();
    rc_conn_unref(c);
}

typedef struct {
    RcConn *conn;
    GBytes *bytes;
} RcWrite;

static void
rc_write_done(GObject *src, GAsyncResult *res, gpointer data)
{
    RcWrite *w = data;
    RcConn *c = w->conn;
    GError *err = NULL;
    gboolean ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(src), res,
                                                   NULL, &err);
    g_clear_error(&err);
    if (!ok || c->close_after_write)
        rc_conn_close(c);
    else if (!c->closed) {
        c->busy = FALSE;
        rc_conn_process(c);
    }
    g_bytes_unref(w->bytes);
    rc_conn_unref(c);
    g_free(w);
}

static void
rc_send(RcConn *c, GString *line)
{
    g_string_append_c(line, '\n');
    gsize len = line->len;
    char *data = g_string_free(line, FALSE);
    if (c->closed) {
        g_free(data);
        return;
    }
    RcWrite *w = g_new0(RcWrite, 1);
    w->conn = rc_conn_ref(c);
    w->bytes = g_bytes_new_take(data, len);
    g_output_stream_write_all_async(c->out, g_bytes_get_data(w->bytes, NULL),
                                    len, G_PRIORITY_DEFAULT, c->cancel,
                                    rc_write_done, w);
}

static void
rc_reply_ok(RcConn *c, const char *id_json, const char *result_json)
{
    GString *s = g_string_new("{\"id\":");
    g_string_append(s, id_json ? id_json : "null");
    g_string_append(s, ",\"ok\":true,\"result\":");
    g_string_append(s, result_json && *result_json ? result_json : "{}");
    g_string_append_c(s, '}');
    rc_send(c, s);
}

static void
rc_reply_err(RcConn *c, const char *id_json, const char *message)
{
    GString *s = g_string_new("{\"id\":");
    g_string_append(s, id_json ? id_json : "null");
    g_string_append(s, ",\"ok\":false,\"error\":");
    rj_append_str(s, message);
    g_string_append_c(s, '}');
    rc_send(c, s);
}

static RcOp *
rc_op_ref(RcOp *op)
{
    op->refs++;
    return op;
}

static void
rc_op_unref(RcOp *op)
{
    if (--op->refs > 0)
        return;
    rj_free(op->req);
    g_free(op->id_json);
    rc_conn_unref(op->conn);
    g_free(op);
}

static void
rc_op_finish(RcOp *op)
{
    op->done = TRUE;
    if (op->timer) {
        g_source_remove(op->timer);
        op->timer = 0;
    }
    if (op->timeout) {
        g_source_remove(op->timeout);
        op->timeout = 0;
    }
}

static void
rc_op_ok(RcOp *op, const char *result_json)
{
    if (op->done)
        return;
    rc_op_finish(op);
    rc_reply_ok(op->conn, op->id_json, result_json);
    rc_op_unref(op);
}

static void
rc_op_err(RcOp *op, const char *message)
{
    if (op->done)
        return;
    rc_op_finish(op);
    rc_reply_err(op->conn, op->id_json, message);
    rc_op_unref(op);
}

static void
rc_append_view_state(GString *s, NsProcView *v)
{
    const char *url = ns_proc_view_display_url(v);
    const char *title = ns_proc_view_title(v);
    int w = 0, h = 0, sx = 0, sy = 0;
    double scale = 1.0;
    ns_proc_view_remote_geometry(v, &w, &h, &sx, &sy, &scale);
    g_string_append(s, "\"url\":");
    rj_append_str(s, url ? url : "");
    g_string_append(s, ",\"title\":");
    rj_append_str(s, title ? title : "");
    g_string_append_printf(s, ",\"loading\":%s,\"viewport\":{\"width\":%d,"
                           "\"height\":%d,\"scroll_x\":%d,\"scroll_y\":%d},"
                           "\"zoom\":%d",
                           ns_proc_view_is_loading(v) ? "true" : "false",
                           w, h, sx, sy, (int)(scale * 100 + 0.5));
}

static void
rc_op_reply_state(RcOp *op)
{
    NsProcView *v = ns_procapp_tab_view(op->tab);
    if (!v) {
        rc_op_err(op, "the tab was closed");
        return;
    }
    GString *s = g_string_new("{");
    g_string_append_printf(s, "\"tab\":%d,", ns_procapp_tab_id(v));
    rc_append_view_state(s, v);
    g_string_append_c(s, '}');
    rc_op_ok(op, s->str);
    g_string_free(s, TRUE);
}

static void rc_op_screenshot_now(RcOp *op);

static gboolean
rc_op_poll(gpointer data)
{
    RcOp *op = data;
    NsProcView *v = ns_procapp_tab_view(op->tab);
    if (!v) {
        op->timer = 0;
        rc_op_err(op, "the tab was closed");
        return G_SOURCE_REMOVE;
    }
    gint64 now = g_get_monotonic_time();
    gboolean loading = ns_proc_view_is_loading(v);
    gboolean rendering = ns_proc_view_remote_rendering(v);
    if (loading && !op->saw_loading) {
        op->saw_loading = TRUE;
        gint64 load_deadline = now + (gint64)RC_LOAD_TIMEOUT_MS * 1000;
        if (op->deadline_us < load_deadline)
            op->deadline_us = load_deadline;
    }
    gboolean min_elapsed =
        now - op->started_us >= (gint64)RC_SETTLE_MIN_MS * 1000;
    gboolean expired = now >= op->deadline_us;
    gboolean settled = !loading && !rendering && min_elapsed;
    if (!settled && !expired)
        return G_SOURCE_CONTINUE;
    op->timer = 0;
    if (op->wait == RC_WAIT_RENDER)
        rc_op_screenshot_now(op);
    else
        rc_op_reply_state(op);
    return G_SOURCE_REMOVE;
}

static void
rc_op_start_wait(RcOp *op, RcWaitKind kind, int max_ms)
{
    op->wait = kind;
    op->started_us = g_get_monotonic_time();
    op->deadline_us = op->started_us + (gint64)max_ms * 1000;
    op->timer = g_timeout_add(RC_POLL_MS, rc_op_poll, op);
}

static void
rc_barrier_done(const char *text, gpointer data)
{
    (void)text;
    RcOp *op = data;
    if (!op->done)
        rc_op_start_wait(op, op->wait, op->wait == RC_WAIT_LOAD
                                           ? RC_LOAD_TIMEOUT_MS
                                           : RC_SETTLE_MS);
    rc_op_unref(op);
}

static void
rc_op_settle(RcOp *op, NsProcView *v, RcWaitKind kind)
{
    op->wait = kind;
    ns_proc_view_remote_eval(v, "0", rc_barrier_done, rc_op_ref(op));
}

static gboolean
rc_op_timeout(gpointer data)
{
    RcOp *op = data;
    op->timeout = 0;
    rc_op_err(op, "timed out waiting for the page");
    return G_SOURCE_REMOVE;
}

static void
rc_eval_done(const char *text, gpointer data)
{
    RcOp *op = data;
    if (!text) {
        rc_op_err(op, "the page did not answer (it may be loading or busy)");
    } else {
        GString *s = g_string_new("{\"text\":");
        rj_append_str(s, text);
        g_string_append_c(s, '}');
        rc_op_ok(op, s->str);
        g_string_free(s, TRUE);
    }
    rc_op_unref(op);
}

static void
rc_op_screenshot_now(RcOp *op)
{
    NsProcView *v = ns_procapp_tab_view(op->tab);
    if (!v) {
        rc_op_err(op, "the tab was closed");
        return;
    }
    int w = 0, h = 0;
    GBytes *png = ns_proc_view_remote_screenshot(v, &w, &h);
    if (!png) {
        rc_op_err(op, "could not capture the tab (is the window minimised?)");
        return;
    }
    gsize len = 0;
    const guchar *data = g_bytes_get_data(png, &len);
    char *b64 = g_base64_encode(data, len);
    g_bytes_unref(png);
    GString *s = g_string_sized_new(strlen(b64) + 256);
    g_string_append_printf(s, "{\"tab\":%d,\"width\":%d,\"height\":%d,"
                           "\"png\":\"%s\",", ns_procapp_tab_id(v), w, h, b64);
    rc_append_view_state(s, v);
    g_string_append_c(s, '}');
    g_free(b64);
    rc_op_ok(op, s->str);
    g_string_free(s, TRUE);
}

static gboolean
rc_parse_mod(const char *word, GdkModifierType *mods)
{
    if (!g_ascii_strcasecmp(word, "ctrl") || !g_ascii_strcasecmp(word, "control"))
        *mods |= GDK_CONTROL_MASK;
    else if (!g_ascii_strcasecmp(word, "shift"))
        *mods |= GDK_SHIFT_MASK;
    else if (!g_ascii_strcasecmp(word, "alt") || !g_ascii_strcasecmp(word, "option"))
        *mods |= GDK_ALT_MASK;
    else if (!g_ascii_strcasecmp(word, "meta") || !g_ascii_strcasecmp(word, "cmd") ||
             !g_ascii_strcasecmp(word, "command") || !g_ascii_strcasecmp(word, "super") ||
             !g_ascii_strcasecmp(word, "win"))
        *mods |= GDK_META_MASK;
    else
        return FALSE;
    return TRUE;
}

static guint
rc_keyval(const char *name)
{
    static const struct { const char *alias; guint keyval; } aliases[] = {
        { "enter", GDK_KEY_Return }, { "return", GDK_KEY_Return },
        { "esc", GDK_KEY_Escape }, { "escape", GDK_KEY_Escape },
        { "tab", GDK_KEY_Tab }, { "space", GDK_KEY_space },
        { "backspace", GDK_KEY_BackSpace }, { "delete", GDK_KEY_Delete },
        { "del", GDK_KEY_Delete }, { "insert", GDK_KEY_Insert },
        { "up", GDK_KEY_Up }, { "arrowup", GDK_KEY_Up },
        { "down", GDK_KEY_Down }, { "arrowdown", GDK_KEY_Down },
        { "left", GDK_KEY_Left }, { "arrowleft", GDK_KEY_Left },
        { "right", GDK_KEY_Right }, { "arrowright", GDK_KEY_Right },
        { "home", GDK_KEY_Home }, { "end", GDK_KEY_End },
        { "pageup", GDK_KEY_Page_Up }, { "page_up", GDK_KEY_Page_Up },
        { "pagedown", GDK_KEY_Page_Down }, { "page_down", GDK_KEY_Page_Down },
        { "plus", GDK_KEY_plus }, { "minus", GDK_KEY_minus },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(aliases); i++)
        if (!g_ascii_strcasecmp(name, aliases[i].alias))
            return aliases[i].keyval;
    if (g_utf8_strlen(name, -1) == 1)
        return gdk_unicode_to_keyval(g_utf8_get_char(name));
    if ((name[0] == 'F' || name[0] == 'f') && g_ascii_isdigit(name[1])) {
        char *upper = g_ascii_strup(name, -1);
        guint kv = gdk_keyval_from_name(upper);
        g_free(upper);
        return kv;
    }
    return gdk_keyval_from_name(name);
}

static gboolean
rc_send_combo(NsProcView *v, char *combo, char **error)
{
    const char *key = combo;
    char *mods_part = NULL;
    char *last_plus = strrchr(combo, '+');
    if (last_plus && last_plus != combo) {
        if (last_plus[1] == '\0') {
            *last_plus = '\0';
            char *prev = strrchr(combo, '+');
            if (prev)
                *prev = '\0';
            key = "+";
        } else {
            *last_plus = '\0';
            key = last_plus + 1;
        }
        mods_part = combo;
    }
    GdkModifierType mods = 0;
    if (mods_part) {
        char **words = g_strsplit(mods_part, "+", -1);
        for (int i = 0; words[i]; i++) {
            if (*words[i] && !rc_parse_mod(words[i], &mods)) {
                *error = g_strdup_printf("unknown modifier '%s'", words[i]);
                g_strfreev(words);
                return FALSE;
            }
        }
        g_strfreev(words);
    }
    guint kv = rc_keyval(key);
    if (kv == 0 || kv == GDK_KEY_VoidSymbol) {
        *error = g_strdup_printf("unknown key '%s'", key);
        return FALSE;
    }
    if (mods & GDK_SHIFT_MASK)
        kv = gdk_keyval_to_upper(kv);
    ns_proc_view_remote_key(v, kv, mods);
    return TRUE;
}

static gboolean
rc_send_keys(NsProcView *v, const char *spec, char **error)
{
    char **combos = g_strsplit_set(spec, " ", -1);
    gboolean ok = TRUE;
    for (int i = 0; combos[i] && ok; i++)
        if (*combos[i])
            ok = rc_send_combo(v, combos[i], error);
    g_strfreev(combos);
    return ok;
}

static void
rc_tabs_json(GString *s)
{
    GArray *tabs = ns_procapp_tabs();
    g_string_append(s, "{\"tabs\":[");
    for (guint i = 0; i < tabs->len; i++) {
        NsProcTab *t = &g_array_index(tabs, NsProcTab, i);
        if (i)
            g_string_append_c(s, ',');
        g_string_append_printf(s, "{\"tab\":%d,\"window\":%d,\"active\":%s,"
                               "\"private\":%s,", t->id, t->window,
                               t->active ? "true" : "false",
                               ns_proc_view_is_private(t->view) ? "true" : "false");
        rc_append_view_state(s, t->view);
        g_string_append_c(s, '}');
    }
    g_string_append(s, "]}");
    g_array_free(tabs, TRUE);
}

static void
rc_set_client(const char *name)
{
    g_free(rc.client_name);
    rc.client_name = name && *name ? g_strndup(name, 64) : NULL;
    rc_update_indicator();
}

static void
rc_dispatch(RcOp *op)
{
    const RjValue *req = op->req;
    const char *cmd = rj_get_str(req, "cmd");
    double num = 0;
    op->tab = rj_get_num(req, "tab", &num) ? (int)num : 0;

    if (!g_strcmp0(cmd, "hello")) {
        rc_set_client(rj_get_str(req, "client"));
        GString *s = g_string_new(NULL);
        g_string_append_printf(s, "{\"version\":\"%s\",\"pid\":%d}",
                               NS_VERSION, (int)ns_rproc_self_pid());
        rc_op_ok(op, s->str);
        g_string_free(s, TRUE);
        return;
    }
    if (!g_strcmp0(cmd, "tabs")) {
        GString *s = g_string_new(NULL);
        rc_tabs_json(s);
        rc_op_ok(op, s->str);
        g_string_free(s, TRUE);
        return;
    }
    if (!g_strcmp0(cmd, "tab_new")) {
        const char *url = rj_get_str(req, "url");
        int id = ns_procapp_tab_new(url && *url ? url : "about:blank");
        if (id <= 0) {
            rc_op_err(op, "could not open a tab");
            return;
        }
        op->tab = id;
        NsProcView *v = ns_procapp_tab_view(id);
        if (!v) {
            rc_op_err(op, "could not open a tab");
            return;
        }
        rc_op_start_wait(op, RC_WAIT_LOAD, RC_LOAD_TIMEOUT_MS);
        return;
    }

    NsProcView *v = ns_procapp_tab_view(op->tab);
    if (!v) {
        rc_op_err(op, op->tab > 0 ? "no tab with that id" : "no open tab");
        return;
    }
    op->tab = ns_procapp_tab_id(v);

    if (!g_strcmp0(cmd, "tab_close")) {
        if (!ns_procapp_tab_close(op->tab))
            rc_op_err(op, "cannot close the last tab");
        else
            rc_op_ok(op, "{}");
        return;
    }
    if (!g_strcmp0(cmd, "tab_select")) {
        ns_procapp_tab_select(op->tab, TRUE);
        rc_op_reply_state(op);
        return;
    }
    if (!g_strcmp0(cmd, "state")) {
        rc_op_reply_state(op);
        return;
    }
    if (!g_strcmp0(cmd, "eval")) {
        const char *js = rj_get_str(req, "js");
        if (!js) {
            rc_op_err(op, "missing js");
            return;
        }
        op->timeout = g_timeout_add(RC_EVAL_TIMEOUT_MS, rc_op_timeout, op);
        ns_proc_view_remote_eval(v, js, rc_eval_done, rc_op_ref(op));
        return;
    }
    if (!g_strcmp0(cmd, "console")) {
        op->timeout = g_timeout_add(RC_EVAL_TIMEOUT_MS, rc_op_timeout, op);
        ns_proc_view_remote_console(v, rc_eval_done, rc_op_ref(op));
        return;
    }
    if (!g_strcmp0(cmd, "wait_load")) {
        double ms = RC_LOAD_TIMEOUT_MS;
        rj_get_num(req, "ms", &ms);
        rc_op_start_wait(op, RC_WAIT_LOAD, CLAMP((int)ms, 0, 120000));
        return;
    }

    ns_procapp_tab_select(op->tab, FALSE);

    if (!g_strcmp0(cmd, "navigate")) {
        const char *url = rj_get_str(req, "url");
        if (!url || !*url) {
            rc_op_err(op, "missing url");
            return;
        }
        if (!g_ascii_strcasecmp(url, "back")) {
            if (!ns_proc_view_can_back(v)) {
                rc_op_err(op, "no earlier page in this tab's history");
                return;
            }
            ns_proc_view_back(v);
        } else if (!g_ascii_strcasecmp(url, "forward")) {
            if (!ns_proc_view_can_forward(v)) {
                rc_op_err(op, "no later page in this tab's history");
                return;
            }
            ns_proc_view_forward(v);
        } else if (!g_ascii_strcasecmp(url, "reload")) {
            ns_proc_view_reload(v);
        } else {
            ns_procapp_tab_load(op->tab, url);
        }
        rc_op_start_wait(op, RC_WAIT_LOAD, RC_LOAD_TIMEOUT_MS);
        return;
    }
    if (!g_strcmp0(cmd, "screenshot")) {
        ns_proc_view_remote_refresh(v);
        rc_op_start_wait(op, RC_WAIT_RENDER, 1500);
        return;
    }

    double x = 0, y = 0;
    gboolean have_xy = rj_get_num(req, "x", &x) && rj_get_num(req, "y", &y);
    gboolean page = rj_get_bool(req, "page", FALSE);
    gboolean ok = TRUE;
    if (!g_strcmp0(cmd, "click")) {
        double count = 1, mods = 0;
        rj_get_num(req, "count", &count);
        rj_get_num(req, "mods", &mods);
        ok = have_xy && ns_proc_view_remote_click(v, x, y, page, (int)count,
                                                  (int)mods);
    } else if (!g_strcmp0(cmd, "hover")) {
        ok = have_xy && ns_proc_view_remote_hover(v, x, y, page);
    } else if (!g_strcmp0(cmd, "scroll")) {
        double dx = 0, dy = 0;
        rj_get_num(req, "dx", &dx);
        rj_get_num(req, "dy", &dy);
        ok = have_xy && ns_proc_view_remote_scroll(v, x, y, dx, dy);
    } else if (!g_strcmp0(cmd, "text")) {
        const char *text = rj_get_str(req, "text");
        ok = text && ns_proc_view_remote_text(v, text);
    } else if (!g_strcmp0(cmd, "key")) {
        const char *keys = rj_get_str(req, "keys");
        char *error = NULL;
        if (!keys || !*keys) {
            rc_op_err(op, "missing keys");
            return;
        }
        if (!rc_send_keys(v, keys, &error)) {
            rc_op_err(op, error ? error : "bad key");
            g_free(error);
            return;
        }
    } else {
        char *msg = g_strdup_printf("unknown command '%s'", cmd ? cmd : "");
        rc_op_err(op, msg);
        g_free(msg);
        return;
    }
    if (!ok) {
        rc_op_err(op, "the page is not ready for input yet (or arguments are "
                      "missing)");
        return;
    }
    rc_op_settle(op, v, RC_WAIT_SETTLE);
}

static void
rc_handle_line(RcConn *c, const char *line, gsize len)
{
    c->busy = TRUE;
    RjValue *req = rj_parse(line, len);
    char *id_json = NULL;
    if (req) {
        const RjValue *id = rj_get(req, "id");
        GString *s = g_string_new(NULL);
        rj_write(s, id);
        id_json = g_string_free(s, FALSE);
    }
    if (!req || req->type != RJ_OBJ) {
        rc_reply_err(c, id_json, "malformed request");
        rj_free(req);
        g_free(id_json);
        return;
    }
    if (!rc.service || !rc.token[0] ||
        !rc_token_equal(rj_get_str(req, "token"), rc.token)) {
        c->close_after_write = TRUE;
        rc_reply_err(c, id_json, "bad token");
        rj_free(req);
        g_free(id_json);
        return;
    }
    if (!c->authed) {
        c->authed = TRUE;
        if (c->auth_timer) {
            g_source_remove(c->auth_timer);
            c->auth_timer = 0;
        }
        rc_update_indicator();
    }
    RcOp *op = g_new0(RcOp, 1);
    op->refs = 1;
    op->conn = rc_conn_ref(c);
    op->id_json = id_json;
    op->req = req;
    rc_dispatch(op);
}

static void
rc_conn_process(RcConn *c)
{
    while (!c->closed && !c->busy) {
        guint8 *nl = memchr(c->buf->data, '\n', c->buf->len);
        if (!nl)
            break;
        gsize len = (gsize)(nl - c->buf->data);
        char *line = g_strndup((const char *)c->buf->data, len);
        g_byte_array_remove_range(c->buf, 0, (guint)len + 1);
        gsize l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == ' '))
            line[--l] = '\0';
        if (l > 0)
            rc_handle_line(c, line, l);
        g_free(line);
    }
    if (!c->closed && !c->busy)
        rc_conn_read_more(c);
}

static void
rc_read_done(GObject *src, GAsyncResult *res, gpointer data)
{
    RcConn *c = data;
    GError *err = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &err);
    c->reading = FALSE;
    g_clear_error(&err);
    if (c->closed) {
        rc_conn_unref(c);
        return;
    }
    if (n <= 0 || c->buf->len + (gsize)n > RC_MAX_LINE) {
        rc_conn_close(c);
        rc_conn_unref(c);
        return;
    }
    g_byte_array_append(c->buf, (const guint8 *)c->chunk, (guint)n);
    rc_conn_process(c);
    rc_conn_unref(c);
}

static void
rc_conn_read_more(RcConn *c)
{
    if (c->reading || c->closed)
        return;
    c->reading = TRUE;
    g_input_stream_read_async(c->in, c->chunk, sizeof c->chunk,
                              G_PRIORITY_DEFAULT, c->cancel, rc_read_done,
                              rc_conn_ref(c));
}

static gboolean
rc_auth_expired(gpointer data)
{
    RcConn *c = data;
    c->auth_timer = 0;
    if (!c->authed)
        rc_conn_close(c);
    return G_SOURCE_REMOVE;
}

static gboolean
rc_on_incoming(GSocketService *service, GSocketConnection *connection,
               GObject *source, gpointer data)
{
    (void)service; (void)source; (void)data;
    if (g_list_length(rc.conns) >= RC_MAX_CONNS) {
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        return TRUE;
    }
    RcConn *c = g_new0(RcConn, 1);
    c->refs = 1;
    c->conn = g_object_ref(connection);
    c->in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    c->out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    c->cancel = g_cancellable_new();
    c->buf = g_byte_array_new();
    c->auth_timer = g_timeout_add_seconds(RC_AUTH_TIMEOUT_S, rc_auth_expired, c);
    rc.conns = g_list_prepend(rc.conns, c);
    rc_conn_read_more(c);
    return TRUE;
}

static gboolean
rc_make_token(void)
{
    unsigned char raw[32];
    if (RAND_bytes(raw, sizeof raw) != 1)
        return FALSE;
    for (gsize i = 0; i < sizeof raw; i++)
        g_snprintf(rc.token + i * 2, 3, "%02x", raw[i]);
    return TRUE;
}

static gboolean
rc_write_json(const char *json)
{
    if (!rc.file_path)
        rc.file_path = ns_remote_control_file_path();
    char *dir = g_path_get_dirname(rc.file_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GError *err = NULL;
    gboolean ok = g_file_set_contents_full(rc.file_path, json, -1,
                                           G_FILE_SET_CONTENTS_CONSISTENT,
                                           0600, &err);
    if (!ok) {
        g_warning("remote control: cannot write %s: %s", rc.file_path,
                  err ? err->message : "?");
        g_clear_error(&err);
    }
    return ok;
}

static gboolean
rc_write_file(void)
{
    char *json = g_strdup_printf("{\"port\":%u,\"token\":\"%s\",\"pid\":%d,"
                                 "\"version\":\"%s\"}\n", (unsigned)rc.port,
                                 rc.token, (int)ns_rproc_self_pid(),
                                 NS_VERSION);
    gboolean ok = rc_write_json(json);
    g_free(json);
    return ok;
}

static void
rc_write_stopped(void)
{
    char *json = g_strdup_printf("{\"stopped\":true,\"pid\":%d}\n",
                                 (int)ns_rproc_self_pid());
    rc_write_json(json);
    g_free(json);
}

static void
rc_remove_file(void)
{
    char *contents = NULL;
    gsize len = 0;
    if (!rc.file_path ||
        !g_file_get_contents(rc.file_path, &contents, &len, NULL))
        return;
    RjValue *info = rj_parse(contents, len);
    double pid = 0;
    if (rj_get_num(info, "pid", &pid) && (int)pid == ns_rproc_self_pid())
        g_unlink(rc.file_path);
    rj_free(info);
    g_free(contents);
}

static void
rc_start(void)
{
    if (rc.service)
        return;
    if (!rc_make_token()) {
        g_warning("remote control: no random source for the token");
        return;
    }
    GInetAddress *lo = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *addr = g_inet_socket_address_new(lo, 0);
    g_object_unref(lo);
    GSocketService *service = g_socket_service_new();
    GSocketAddress *effective = NULL;
    GError *err = NULL;
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service), addr,
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_TCP, NULL,
                                       &effective, &err)) {
        g_warning("remote control: cannot listen on 127.0.0.1: %s",
                  err ? err->message : "?");
        g_clear_error(&err);
        g_object_unref(addr);
        g_object_unref(service);
        return;
    }
    g_object_unref(addr);
    rc.port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
    g_object_unref(effective);
    g_signal_connect(service, "incoming", G_CALLBACK(rc_on_incoming), NULL);
    rc.service = service;
    if (!rc.file_path)
        rc.file_path = ns_remote_control_file_path();
    if (!rc_write_file()) {
        g_socket_service_stop(service);
        g_socket_listener_close(G_SOCKET_LISTENER(service));
        g_clear_object(&rc.service);
        return;
    }
    g_socket_service_start(service);
    rc_update_indicator();
}

static void
rc_close_all(gboolean user_stopped)
{
    while (rc.conns)
        rc_conn_close(rc.conns->data);
    if (rc.service) {
        g_socket_service_stop(rc.service);
        g_socket_listener_close(G_SOCKET_LISTENER(rc.service));
        g_clear_object(&rc.service);
    }
    if (user_stopped)
        rc_write_stopped();
    else
        rc_remove_file();
    memset(rc.token, 0, sizeof rc.token);
    rc_update_indicator();
}

static void
rc_update_indicator(void)
{
    if (rc_authed_count() > 0)
        ns_procapp_set_remote_status(rc.client_name ? rc.client_name : "");
    else
        ns_procapp_set_remote_status(NULL);
}

static gboolean
rc_config_enabled(void)
{
    ns_config_lock();
    ns_config_reload();
    const ns_config *cfg = ns_config_get();
    gboolean on = cfg && cfg->remote_control;
    ns_config_unlock();
    return on;
}

static void
rc_on_config_changed(GFileMonitor *mon, GFile *file, GFile *other,
                     GFileMonitorEvent event, gpointer data)
{
    (void)mon; (void)file; (void)other; (void)data;
    if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        event != G_FILE_MONITOR_EVENT_CREATED &&
        event != G_FILE_MONITOR_EVENT_DELETED)
        return;
    gboolean on = rc_config_enabled();
    if (on == rc.config_enabled)
        return;
    rc.config_enabled = on;
    if (on) {
        rc.stopped = FALSE;
        rc_start();
    } else {
        rc_close_all(FALSE);
    }
}

void
ns_remote_control_init(void)
{
    if (rc.inited)
        return;
    rc.inited = TRUE;
    rc.config_enabled = rc_config_enabled();
    char *cfg_path = g_build_filename(g_get_user_config_dir(), NS_APP_DIR_NAME,
                                      "nordstjernen.conf", NULL);
    GFile *f = g_file_new_for_path(cfg_path);
    g_free(cfg_path);
    rc.config_monitor = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
    g_object_unref(f);
    if (rc.config_monitor)
        g_signal_connect(rc.config_monitor, "changed",
                         G_CALLBACK(rc_on_config_changed), NULL);
    if (rc.requested || rc.config_enabled)
        rc_start();
}

void
ns_remote_control_stop(void)
{
    rc.stopped = TRUE;
    rc_close_all(TRUE);
}

void
ns_remote_control_shutdown(void)
{
    rc_close_all(FALSE);
    g_clear_object(&rc.config_monitor);
    g_clear_pointer(&rc.file_path, g_free);
    g_clear_pointer(&rc.client_name, g_free);
}
