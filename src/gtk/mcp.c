/* Nordstjernen — stdio Model Context Protocol server that drives the GUI over its control channel. */

#include "mcp.h"

#include <gio/gio.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "remote_page.h"
#include "remotectl.h"
#include "rjson.h"
#include "version.h"

#define MCP_PROTOCOL_VERSION  "2025-06-18"
#define MCP_MAX_LINE          (64 * 1024 * 1024)
#define MCP_IO_TIMEOUT_S      150
#define MCP_LAUNCH_WAIT_MS    45000
#define MCP_JS_TIMEOUT_MS     30000
#define MCP_WAIT_MAX_MS       30000
#define MCP_SCROLL_TICK_PX    100

typedef struct {
    GString  *text;
    char     *image;
    gboolean  error;
} McpResult;

typedef void (*McpToolFn)(const RjValue *args, McpResult *res);

typedef struct {
    const char *name;
    const char *title;
    const char *description;
    const char *schema;
    McpToolFn   fn;
} McpTool;

static struct {
    char              *self_exe;
    char              *client_name;
    GSocketClient     *client;
    GSocketConnection *conn;
    GInputStream      *in;
    GOutputStream     *out;
    GByteArray        *buf;
    char              *token;
    long               next_id;
} mcp;

static void G_GNUC_PRINTF(2, 3)
res_add(McpResult *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_string_append_vprintf(r->text, fmt, ap);
    va_end(ap);
}

static void G_GNUC_PRINTF(2, 3)
res_fail(McpResult *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_string_append_vprintf(r->text, fmt, ap);
    va_end(ap);
    r->error = TRUE;
}

static void
ctl_drop(void)
{
    if (mcp.conn)
        g_io_stream_close(G_IO_STREAM(mcp.conn), NULL, NULL);
    g_clear_object(&mcp.conn);
    mcp.in = NULL;
    mcp.out = NULL;
    if (mcp.buf)
        g_byte_array_set_size(mcp.buf, 0);
    g_clear_pointer(&mcp.token, g_free);
}

static char *
ctl_read_line(GError **err)
{
    for (;;) {
        guint8 *nl = memchr(mcp.buf->data, '\n', mcp.buf->len);
        if (nl) {
            gsize len = (gsize)(nl - mcp.buf->data);
            char *line = g_strndup((const char *)mcp.buf->data, len);
            g_byte_array_remove_range(mcp.buf, 0, (guint)len + 1);
            return line;
        }
        if (mcp.buf->len > MCP_MAX_LINE) {
            g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                                "response too large");
            return NULL;
        }
        guint8 chunk[65536];
        gssize n = g_input_stream_read(mcp.in, chunk, sizeof chunk, NULL, err);
        if (n <= 0) {
            if (n == 0 && err && !*err)
                g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_CLOSED,
                                    "the browser closed the connection");
            return NULL;
        }
        g_byte_array_append(mcp.buf, chunk, (guint)n);
    }
}

static RjValue *
ctl_roundtrip(const char *cmd, int tab, const char *fields, char **error)
{
    GString *line = g_string_new("{\"token\":");
    rj_append_str(line, mcp.token);
    g_string_append_printf(line, ",\"id\":%ld,\"cmd\":", ++mcp.next_id);
    rj_append_str(line, cmd);
    if (tab > 0)
        g_string_append_printf(line, ",\"tab\":%d", tab);
    if (fields && *fields) {
        g_string_append_c(line, ',');
        g_string_append(line, fields);
    }
    g_string_append(line, "}\n");
    GError *err = NULL;
    gboolean ok = g_output_stream_write_all(mcp.out, line->str, line->len,
                                            NULL, NULL, &err);
    g_string_free(line, TRUE);
    char *reply = ok ? ctl_read_line(&err) : NULL;
    if (!reply) {
        *error = g_strdup(err ? err->message : "connection lost");
        g_clear_error(&err);
        ctl_drop();
        return NULL;
    }
    RjValue *resp = rj_parse(reply, strlen(reply));
    g_free(reply);
    if (!resp || resp->type != RJ_OBJ) {
        rj_free(resp);
        *error = g_strdup("malformed reply from the browser");
        ctl_drop();
        return NULL;
    }
    return resp;
}

static gboolean
pid_alive(int pid)
{
    if (pid <= 0)
        return FALSE;
#ifdef G_OS_WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h)
        return FALSE;
    gboolean alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

static gboolean
ctl_stopped_by_user(void)
{
    char *path = ns_remote_control_file_path();
    char *contents = NULL;
    gsize len = 0;
    gboolean read = g_file_get_contents(path, &contents, &len, NULL);
    g_free(path);
    if (!read)
        return FALSE;
    RjValue *info = rj_parse(contents, len);
    g_free(contents);
    double pid = 0;
    gboolean stopped = rj_get_bool(info, "stopped", FALSE) &&
                       rj_get_num(info, "pid", &pid) && pid_alive((int)pid);
    rj_free(info);
    return stopped;
}

static gboolean
ctl_file_is_private_to_user(const char *path)
{
#ifdef G_OS_WIN32
    (void)path;
    return TRUE;
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == getuid() && (st.st_mode & 077) == 0;
#endif
}

static gboolean
ctl_try_file(void)
{
    char *path = ns_remote_control_file_path();
    char *contents = NULL;
    gsize len = 0;
    gboolean read = ctl_file_is_private_to_user(path) &&
                    g_file_get_contents(path, &contents, &len, NULL);
    g_free(path);
    if (!read)
        return FALSE;
    RjValue *info = rj_parse(contents, len);
    g_free(contents);
    double port = 0;
    const char *token = rj_get_str(info, "token");
    if (!info || !rj_get_num(info, "port", &port) || port <= 0 ||
        port > 65535 || !token) {
        rj_free(info);
        return FALSE;
    }
    GError *err = NULL;
    GSocketConnection *conn = g_socket_client_connect_to_host(
        mcp.client, "127.0.0.1", (guint16)port, NULL, &err);
    if (!conn) {
        g_clear_error(&err);
        rj_free(info);
        return FALSE;
    }
    mcp.conn = conn;
    mcp.in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    mcp.out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    mcp.token = g_strdup(token);
    rj_free(info);
    GString *fields = g_string_new("\"client\":");
    rj_append_str(fields, mcp.client_name ? mcp.client_name : "");
    char *error = NULL;
    RjValue *resp = ctl_roundtrip("hello", 0, fields->str, &error);
    g_string_free(fields, TRUE);
    gboolean ok = resp && rj_get_bool(resp, "ok", FALSE);
    rj_free(resp);
    g_free(error);
    if (!ok)
        ctl_drop();
    return ok;
}

#ifndef G_OS_WIN32
static void
detach_session(gpointer data)
{
    (void)data;
    setsid();
}
#endif

static gboolean
launch_gui(void)
{
    if (!mcp.self_exe)
        return FALSE;
#ifdef G_OS_WIN32
    wchar_t *app = g_utf8_to_utf16(mcp.self_exe, -1, NULL, NULL, NULL);
    char *cmd8 = g_strdup_printf("\"%s\" --remote-control", mcp.self_exe);
    wchar_t *cmd = g_utf8_to_utf16(cmd8, -1, NULL, NULL, NULL);
    g_free(cmd8);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP |
                  CREATE_UNICODE_ENVIRONMENT;
    BOOL ok = app && cmd &&
        CreateProcessW(app, cmd, NULL, NULL, FALSE,
                       flags | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi);
    if (!ok && app && cmd)
        ok = CreateProcessW(app, cmd, NULL, NULL, FALSE, flags, NULL, NULL,
                            &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    g_free(app);
    g_free(cmd);
    return ok;
#else
    GSubprocessLauncher *l = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_set_child_setup(l, detach_session, NULL, NULL);
    GSubprocess *p = g_subprocess_launcher_spawn(l, NULL, mcp.self_exe,
                                                 "--remote-control", NULL);
    g_object_unref(l);
    if (!p)
        return FALSE;
    g_object_unref(p);
    return TRUE;
#endif
}

static gboolean
ctl_ensure(char **error)
{
    if (mcp.conn)
        return TRUE;
    if (ctl_try_file())
        return TRUE;
    if (ctl_stopped_by_user()) {
        *error = g_strdup("remote control was stopped from the Nordstjernen "
                          "window. Ask the user to turn \"Allow remote "
                          "control\" back on in Settings, or to restart "
                          "Nordstjernen");
        return FALSE;
    }
    if (!launch_gui()) {
        *error = g_strdup("Nordstjernen is not running with remote control "
                          "enabled, and it could not be started");
        return FALSE;
    }
    for (int waited = 0; waited < MCP_LAUNCH_WAIT_MS; waited += 250) {
        g_usleep(250 * 1000);
        if (ctl_try_file())
            return TRUE;
    }
    *error = g_strdup("started Nordstjernen, but its remote-control channel "
                      "did not come up (is the Settings switch or "
                      "--remote-control being refused?)");
    return FALSE;
}

static RjValue *
ctl_call(const char *cmd, int tab, const char *fields, char **error)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        g_clear_pointer(error, g_free);
        if (!ctl_ensure(error))
            return NULL;
        RjValue *resp = ctl_roundtrip(cmd, tab, fields, error);
        if (!resp)
            continue;
        if (!rj_get_bool(resp, "ok", FALSE)) {
            const char *e = rj_get_str(resp, "error");
            *error = g_strdup(e ? e : "the browser refused the request");
            rj_free(resp);
            return NULL;
        }
        return resp;
    }
    return NULL;
}

static const RjValue *
resp_result(const RjValue *resp)
{
    return rj_get(resp, "result");
}

static int
arg_tab(const RjValue *args)
{
    double t = 0;
    return rj_get_num(args, "tab_id", &t) ? (int)t : 0;
}

static void
add_state(McpResult *r, const RjValue *state)
{
    const char *title = rj_get_str(state, "title");
    const char *url = rj_get_str(state, "url");
    double tab = 0;
    rj_get_num(state, "tab", &tab);
    res_add(r, "\nTab %d: %s%s%s", (int)tab, url ? url : "",
            title && *title ? " - " : "", title && *title ? title : "");
    if (rj_get_bool(state, "loading", FALSE))
        res_add(r, " (still loading)");
}

static char *
page_call(int tab, const char *call, char **error)
{
    GString *js = g_string_new("(");
    g_string_append(js, ns_remote_page_src);
    g_string_append(js, ").");
    g_string_append(js, call);
    GString *fields = g_string_new("\"js\":");
    rj_append_str(fields, js->str);
    g_string_free(js, TRUE);
    RjValue *resp = ctl_call("eval", tab, fields->str, error);
    g_string_free(fields, TRUE);
    if (!resp)
        return NULL;
    const char *text = rj_get_str(resp_result(resp), "text");
    char *out = NULL;
    if (!text)
        *error = g_strdup("the page returned nothing");
    else if (g_str_has_prefix(text, "error: "))
        *error = g_strdup(text + 7);
    else
        out = g_strdup(text);
    rj_free(resp);
    return out;
}

static char *
js_str(const char *s)
{
    GString *g = g_string_new(NULL);
    rj_append_str(g, s);
    return g_string_free(g, FALSE);
}

static gboolean
resolve_ref(int tab, const char *ref, RjValue **out_point, McpResult *r)
{
    char *q = js_str(ref);
    char *call = g_strdup_printf("point(%s)", q);
    g_free(q);
    char *error = NULL;
    char *text = page_call(tab, call, &error);
    g_free(call);
    if (!text) {
        res_fail(r, "%s", error ? error : "could not locate the element");
        g_free(error);
        return FALSE;
    }
    *out_point = rj_parse(text, strlen(text));
    g_free(text);
    if (!*out_point) {
        res_fail(r, "could not locate the element");
        return FALSE;
    }
    return TRUE;
}

typedef struct {
    double   x, y;
    gboolean page;
    double   vx, vy;
} Target;

static gboolean
resolve_target(const RjValue *args, int tab, Target *t, McpResult *r,
               gboolean required)
{
    const char *ref = rj_get_str(args, "ref");
    if (ref && *ref) {
        RjValue *pt = NULL;
        if (!resolve_ref(tab, ref, &pt, r))
            return FALSE;
        rj_get_num(pt, "px", &t->x);
        rj_get_num(pt, "py", &t->y);
        rj_get_num(pt, "x", &t->vx);
        rj_get_num(pt, "y", &t->vy);
        t->page = TRUE;
        const char *desc = rj_get_str(pt, "desc");
        res_add(r, "Target: %s", desc ? desc : ref);
        const char *cover = rj_get_str(pt, "covered_by");
        if (cover)
            res_add(r, "\nWarning: the element's centre is covered by %s",
                    cover);
        rj_free(pt);
        return TRUE;
    }
    const RjValue *coord = rj_get(args, "coordinate");
    if (coord && coord->type == RJ_ARR && coord->items->len == 2) {
        const RjValue *cx = g_ptr_array_index(coord->items, 0);
        const RjValue *cy = g_ptr_array_index(coord->items, 1);
        if (cx->type == RJ_NUM && cy->type == RJ_NUM) {
            t->x = t->vx = cx->num;
            t->y = t->vy = cy->num;
            t->page = FALSE;
            res_add(r, "Target: (%d, %d)", (int)t->x, (int)t->y);
            return TRUE;
        }
    }
    if (rj_get_num(args, "x", &t->x) && rj_get_num(args, "y", &t->y)) {
        t->vx = t->x;
        t->vy = t->y;
        t->page = FALSE;
        res_add(r, "Target: (%d, %d)", (int)t->x, (int)t->y);
        return TRUE;
    }
    if (required)
        res_fail(r, "give either ref (from read_page/find) or x and y "
                    "(viewport pixels, as in the screenshot)");
    return FALSE;
}

static int
parse_mods(const char *s)
{
    int mods = 0;
    if (!s)
        return 0;
    char *lower = g_ascii_strdown(s, -1);
    if (strstr(lower, "shift")) mods |= 1;
    if (strstr(lower, "ctrl") || strstr(lower, "control")) mods |= 2;
    if (strstr(lower, "alt")) mods |= 4;
    if (strstr(lower, "meta") || strstr(lower, "cmd")) mods |= 8;
    g_free(lower);
    return mods;
}

static void
do_click(const RjValue *args, McpResult *r, int count)
{
    int tab = arg_tab(args);
    Target t = { 0 };
    if (!resolve_target(args, tab, &t, r, TRUE))
        return;
    char *fields = g_strdup_printf("\"x\":%d,\"y\":%d,\"page\":%s,"
                                   "\"count\":%d,\"mods\":%d",
                                   (int)t.x, (int)t.y,
                                   t.page ? "true" : "false", count,
                                   parse_mods(rj_get_str(args, "modifiers")));
    char *error = NULL;
    RjValue *resp = ctl_call("click", tab, fields, &error);
    g_free(fields);
    if (!resp) {
        res_fail(r, "\n%s", error);
        g_free(error);
        return;
    }
    res_add(r, "\n%s.", count == 2 ? "Double-clicked" : "Clicked");
    add_state(r, resp_result(resp));
    rj_free(resp);
}

static void
tool_click(const RjValue *args, McpResult *r)
{
    do_click(args, r, 1);
}

static void
tool_double_click(const RjValue *args, McpResult *r)
{
    do_click(args, r, 2);
}

static void
tool_hover(const RjValue *args, McpResult *r)
{
    int tab = arg_tab(args);
    Target t = { 0 };
    if (!resolve_target(args, tab, &t, r, TRUE))
        return;
    char *fields = g_strdup_printf("\"x\":%d,\"y\":%d,\"page\":%s",
                                   (int)t.x, (int)t.y,
                                   t.page ? "true" : "false");
    char *error = NULL;
    RjValue *resp = ctl_call("hover", tab, fields, &error);
    g_free(fields);
    if (!resp) {
        res_fail(r, "\n%s", error);
        g_free(error);
        return;
    }
    res_add(r, "\nHovering.");
    add_state(r, resp_result(resp));
    rj_free(resp);
}

static void
tool_type(const RjValue *args, McpResult *r)
{
    int tab = arg_tab(args);
    const char *text = rj_get_str(args, "text");
    if (!text) {
        res_fail(r, "text is required");
        return;
    }
    const char *ref = rj_get_str(args, "ref");
    char *error = NULL;
    if (ref && *ref) {
        Target t = { 0 };
        if (!resolve_target(args, tab, &t, r, TRUE))
            return;
        char *fields = g_strdup_printf("\"x\":%d,\"y\":%d,\"page\":true,"
                                       "\"count\":1", (int)t.x, (int)t.y);
        RjValue *resp = ctl_call("click", tab, fields, &error);
        g_free(fields);
        if (!resp) {
            res_fail(r, "\n%s", error);
            g_free(error);
            return;
        }
        rj_free(resp);
        res_add(r, "\n");
    }
    GString *fields = g_string_new("\"text\":");
    rj_append_str(fields, text);
    RjValue *resp = ctl_call("text", tab, fields->str, &error);
    g_string_free(fields, TRUE);
    if (!resp) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    res_add(r, "Typed %ld characters.", (long)g_utf8_strlen(text, -1));
    add_state(r, resp_result(resp));
    rj_free(resp);
}

static void
tool_key(const RjValue *args, McpResult *r)
{
    int tab = arg_tab(args);
    const char *keys = rj_get_str(args, "keys");
    if (!keys)
        keys = rj_get_str(args, "text");
    if (!keys || !*keys) {
        res_fail(r, "keys is required, e.g. \"Enter\", \"ctrl+a\" or "
                    "\"Tab Tab Enter\"");
        return;
    }
    double repeat = 1;
    rj_get_num(args, "repeat", &repeat);
    int n = CLAMP((int)repeat, 1, 100);
    GString *seq = g_string_new(NULL);
    for (int i = 0; i < n; i++) {
        if (i)
            g_string_append_c(seq, ' ');
        g_string_append(seq, keys);
    }
    GString *fields = g_string_new("\"keys\":");
    rj_append_str(fields, seq->str);
    g_string_free(seq, TRUE);
    char *error = NULL;
    RjValue *resp = ctl_call("key", tab, fields->str, &error);
    g_string_free(fields, TRUE);
    if (!resp) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    res_add(r, "Pressed %s%s.", keys, n > 1 ? " repeatedly" : "");
    add_state(r, resp_result(resp));
    rj_free(resp);
}

static void
tool_scroll(const RjValue *args, McpResult *r)
{
    int tab = arg_tab(args);
    const char *dir = rj_get_str(args, "direction");
    Target t = { 0 };
    gboolean have = resolve_target(args, tab, &t, r, FALSE);
    if (r->error)
        return;
    if (!dir || !*dir) {
        if (have && rj_get_str(args, "ref")) {
            res_add(r, "\nScrolled the element into view.");
            return;
        }
        res_fail(r, "direction is required (up, down, left or right)");
        return;
    }
    char *error = NULL;
    if (!have) {
        RjValue *st = ctl_call("state", tab, NULL, &error);
        if (!st) {
            res_fail(r, "%s", error);
            g_free(error);
            return;
        }
        const RjValue *vp = rj_get(resp_result(st), "viewport");
        double w = 800, h = 600;
        rj_get_num(vp, "width", &w);
        rj_get_num(vp, "height", &h);
        t.vx = w / 2;
        t.vy = h / 2;
        rj_free(st);
    }
    double amount = 3;
    rj_get_num(args, "amount", &amount);
    double px = CLAMP(amount, 0.1, 100) * MCP_SCROLL_TICK_PX;
    double dx = 0, dy = 0;
    if (!g_ascii_strcasecmp(dir, "down")) dy = px;
    else if (!g_ascii_strcasecmp(dir, "up")) dy = -px;
    else if (!g_ascii_strcasecmp(dir, "right")) dx = px;
    else if (!g_ascii_strcasecmp(dir, "left")) dx = -px;
    else {
        res_fail(r, "direction must be up, down, left or right");
        return;
    }
    char *fields = g_strdup_printf("\"x\":%d,\"y\":%d,\"dx\":%d,\"dy\":%d",
                                   (int)t.vx, (int)t.vy, (int)dx, (int)dy);
    RjValue *resp = ctl_call("scroll", tab, fields, &error);
    g_free(fields);
    if (!resp) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    const RjValue *vp = rj_get(resp_result(resp), "viewport");
    double sx = 0, sy = 0;
    rj_get_num(vp, "scroll_x", &sx);
    rj_get_num(vp, "scroll_y", &sy);
    res_add(r, "%sScrolled %s by %d px; scroll position is now %d,%d.",
            have ? "\n" : "", dir, (int)px, (int)sx, (int)sy);
    rj_free(resp);
}

static void
tool_form_input(const RjValue *args, McpResult *r)
{
    int tab = arg_tab(args);
    const char *ref = rj_get_str(args, "ref");
    const RjValue *value = rj_get(args, "value");
    if (!ref || !value) {
        res_fail(r, "ref and value are required");
        return;
    }
    GString *call = g_string_new("setValue(");
    rj_append_str(call, ref);
    g_string_append_c(call, ',');
    rj_write(call, value);
    g_string_append_c(call, ')');
    char *error = NULL;
    char *text = page_call(tab, call->str, &error);
    g_string_free(call, TRUE);
    if (!text) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    res_add(r, "%s", text);
    g_free(text);
}

static void
tool_read_page(const RjValue *args, McpResult *r)
{
    int tab = arg_tab(args);
    const char *filter = rj_get_str(args, "filter");
    double max_chars = 40000;
    rj_get_num(args, "max_chars", &max_chars);
    char *f = js_str(filter && *filter ? filter : "all");
    char *call = g_strdup_printf("readPage(%s,%d)", f, (int)max_chars);
    g_free(f);
    char *error = NULL;
    char *text = page_call(tab, call, &error);
    g_free(call);
    if (!text) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    res_add(r, "%s", text);
    g_free(text);
}

static void
tool_find(const RjValue *args, McpResult *r)
{
    int tab = arg_tab(args);
    const char *query = rj_get_str(args, "query");
    if (!query || !*query) {
        res_fail(r, "query is required");
        return;
    }
    double limit = 20;
    rj_get_num(args, "limit", &limit);
    char *q = js_str(query);
    char *call = g_strdup_printf("find(%s,%d)", q, (int)limit);
    g_free(q);
    char *error = NULL;
    char *text = page_call(tab, call, &error);
    g_free(call);
    if (!text) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    res_add(r, "%s", text);
    g_free(text);
}

static void
tool_get_page_text(const RjValue *args, McpResult *r)
{
    int tab = arg_tab(args);
    double max_chars = 100000;
    rj_get_num(args, "max_chars", &max_chars);
    char *call = g_strdup_printf("pageText(%d)", (int)max_chars);
    char *error = NULL;
    char *text = page_call(tab, call, &error);
    g_free(call);
    if (!text) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    res_add(r, "%s", text);
    g_free(text);
}

static void
tool_javascript(const RjValue *args, McpResult *r)
{
    int tab = arg_tab(args);
    const char *code = rj_get_str(args, "code");
    if (!code)
        code = rj_get_str(args, "text");
    if (!code || !*code) {
        res_fail(r, "code is required");
        return;
    }
    char *q = js_str(code);
    char *call = g_strdup_printf("run(%s)", q);
    g_free(q);
    char *error = NULL;
    char *text = page_call(tab, call, &error);
    g_free(call);
    gint64 deadline = g_get_monotonic_time() + MCP_JS_TIMEOUT_MS * 1000;
    while (text) {
        RjValue *v = rj_parse(text, strlen(text));
        g_free(text);
        text = NULL;
        double pending = 0;
        if (v && rj_get_num(v, "pending", &pending)) {
            rj_free(v);
            if (g_get_monotonic_time() > deadline) {
                res_fail(r, "the promise did not settle within %d s",
                         MCP_JS_TIMEOUT_MS / 1000);
                return;
            }
            g_usleep(50 * 1000);
            char *poll = g_strdup_printf("poll(%d)", (int)pending);
            text = page_call(tab, poll, &error);
            g_free(poll);
            continue;
        }
        const char *err = rj_get_str(v, "error");
        const char *val = rj_get_str(v, "value");
        if (err)
            res_fail(r, "%s", err);
        else
            res_add(r, "%s", val ? val : "undefined");
        rj_free(v);
        return;
    }
    res_fail(r, "%s", error ? error : "evaluation failed");
    g_free(error);
}

static void
tool_screenshot(const RjValue *args, McpResult *r)
{
    int tab = arg_tab(args);
    char *error = NULL;
    RjValue *resp = ctl_call("screenshot", tab, NULL, &error);
    if (!resp) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    const RjValue *res = resp_result(resp);
    const char *png = rj_get_str(res, "png");
    double w = 0, h = 0;
    rj_get_num(res, "width", &w);
    rj_get_num(res, "height", &h);
    if (!png) {
        res_fail(r, "no image");
        rj_free(resp);
        return;
    }
    r->image = g_strdup(png);
    res_add(r, "Screenshot %dx%d. Its pixels are CSS viewport pixels: pass "
               "them as x/y to click, hover and scroll.", (int)w, (int)h);
    add_state(r, res);
    rj_free(resp);
}

static void
tool_navigate(const RjValue *args, McpResult *r)
{
    int tab = arg_tab(args);
    const char *url = rj_get_str(args, "url");
    if (!url || !*url) {
        res_fail(r, "url is required (or \"back\" / \"forward\")");
        return;
    }
    GString *fields = g_string_new("\"url\":");
    rj_append_str(fields, url);
    char *error = NULL;
    RjValue *resp = ctl_call("navigate", tab, fields->str, &error);
    g_string_free(fields, TRUE);
    if (!resp) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    res_add(r, "Navigated.");
    add_state(r, resp_result(resp));
    rj_free(resp);
}

static void
tool_reload(const RjValue *args, McpResult *r)
{
    char *error = NULL;
    RjValue *resp = ctl_call("navigate", arg_tab(args), "\"url\":\"reload\"",
                             &error);
    if (!resp) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    res_add(r, "Reloaded.");
    add_state(r, resp_result(resp));
    rj_free(resp);
}

static void
tool_tabs_list(const RjValue *args, McpResult *r)
{
    (void)args;
    char *error = NULL;
    RjValue *resp = ctl_call("tabs", 0, NULL, &error);
    if (!resp) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    const RjValue *tabs = rj_get(resp_result(resp), "tabs");
    guint n = tabs && tabs->type == RJ_ARR ? tabs->items->len : 0;
    res_add(r, "%u open tab%s:", n, n == 1 ? "" : "s");
    for (guint i = 0; i < n; i++) {
        const RjValue *t = g_ptr_array_index(tabs->items, i);
        double id = 0, win = 0;
        rj_get_num(t, "tab", &id);
        rj_get_num(t, "window", &win);
        const char *title = rj_get_str(t, "title");
        const char *url = rj_get_str(t, "url");
        res_add(r, "\n- tab_id %d (window %d)%s%s%s: %s%s%s", (int)id,
                (int)win, rj_get_bool(t, "active", FALSE) ? " [active]" : "",
                rj_get_bool(t, "private", FALSE) ? " [private]" : "",
                rj_get_bool(t, "loading", FALSE) ? " [loading]" : "",
                url ? url : "", title && *title ? " - " : "",
                title ? title : "");
    }
    rj_free(resp);
}

static void
tool_tabs_create(const RjValue *args, McpResult *r)
{
    const char *url = rj_get_str(args, "url");
    GString *fields = g_string_new("\"url\":");
    rj_append_str(fields, url && *url ? url : "about:blank");
    char *error = NULL;
    RjValue *resp = ctl_call("tab_new", 0, fields->str, &error);
    g_string_free(fields, TRUE);
    if (!resp) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    res_add(r, "Opened a new tab.");
    add_state(r, resp_result(resp));
    rj_free(resp);
}

static void
tool_tabs_close(const RjValue *args, McpResult *r)
{
    char *error = NULL;
    RjValue *resp = ctl_call("tab_close", arg_tab(args), NULL, &error);
    if (!resp) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    res_add(r, "Closed the tab.");
    rj_free(resp);
}

static void
tool_tab_select(const RjValue *args, McpResult *r)
{
    if (arg_tab(args) <= 0) {
        res_fail(r, "tab_id is required (see tabs_list)");
        return;
    }
    char *error = NULL;
    RjValue *resp = ctl_call("tab_select", arg_tab(args), NULL, &error);
    if (!resp) {
        res_fail(r, "%s", error);
        g_free(error);
        return;
    }
    res_add(r, "Selected.");
    add_state(r, resp_result(resp));
    rj_free(resp);
}

static void
tool_wait(const RjValue *args, McpResult *r)
{
    double ms = 1000;
    if (!rj_get_num(args, "ms", &ms)) {
        double s = 0;
        if (rj_get_num(args, "seconds", &s))
            ms = s * 1000;
    }
    int wait = CLAMP((int)ms, 0, MCP_WAIT_MAX_MS);
    g_usleep((gulong)wait * 1000);
    res_add(r, "Waited %d ms.", wait);
}

#define TAB_PROP "\"tab_id\":{\"type\":\"integer\",\"description\":\"Tab to act on (from tabs_list). Defaults to the active tab.\"}"
#define REF_PROP "\"ref\":{\"type\":\"string\",\"description\":\"Element reference such as ref_12 from read_page or find.\"}"
#define XY_PROPS "\"x\":{\"type\":\"number\",\"description\":\"Viewport x in screenshot pixels.\"},\"y\":{\"type\":\"number\",\"description\":\"Viewport y in screenshot pixels.\"},\"coordinate\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":2,\"maxItems\":2,\"description\":\"[x, y] viewport pixels, alternative to x and y.\"}"

static const McpTool tools[] = {
    { "tabs_list", "List tabs",
      "List the open browser tabs with their tab_id, URL, title and which one is active.",
      "{\"type\":\"object\",\"properties\":{}}", tool_tabs_list },
    { "tabs_create", "Open tab",
      "Open a new foreground tab, optionally at a URL, and wait for it to load.",
      "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Address or search text. Defaults to about:blank.\"}}}",
      tool_tabs_create },
    { "tabs_close", "Close tab", "Close a tab (the active one by default).",
      "{\"type\":\"object\",\"properties\":{" TAB_PROP "}}", tool_tabs_close },
    { "tab_select", "Select tab", "Bring a tab to the front of its window.",
      "{\"type\":\"object\",\"properties\":{" TAB_PROP "},\"required\":[\"tab_id\"]}",
      tool_tab_select },
    { "navigate", "Navigate",
      "Load a URL in a tab and wait for the page to finish loading. Pass \"back\" or \"forward\" to move through the tab's history. Text that is not a URL is searched with the default search engine.",
      "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL, search text, \\\"back\\\" or \\\"forward\\\".\"}," TAB_PROP "},\"required\":[\"url\"]}",
      tool_navigate },
    { "reload", "Reload", "Reload the page in a tab and wait for it to load.",
      "{\"type\":\"object\",\"properties\":{" TAB_PROP "}}", tool_reload },
    { "read_page", "Read page",
      "Return an accessibility-tree-like outline of the page: roles, accessible names, states and viewport bounding boxes (CSS pixels), with stable refs such as ref_12 for interactive elements that the other tools accept. filter=\"interactive\" lists only links, buttons, fields and other controls.",
      "{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\",\"enum\":[\"all\",\"interactive\"],\"description\":\"all (default) or interactive.\"},\"max_chars\":{\"type\":\"integer\",\"description\":\"Truncate the outline after this many characters (default 40000).\"}," TAB_PROP "}}",
      tool_read_page },
    { "find", "Find elements",
      "Find elements by CSS selector or by visible text, accessible name, placeholder or value. Returns matching elements with refs.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"CSS selector or text to look for (case-insensitive).\"},\"limit\":{\"type\":\"integer\",\"description\":\"Maximum matches (default 20).\"}," TAB_PROP "},\"required\":[\"query\"]}",
      tool_find },
    { "click", "Click",
      "Left-click an element (by ref, scrolled into view first) or a viewport point (x, y in screenshot pixels) with real mouse input, then wait for any navigation to settle.",
      "{\"type\":\"object\",\"properties\":{" REF_PROP "," XY_PROPS ",\"modifiers\":{\"type\":\"string\",\"description\":\"Optional held keys, e.g. \\\"shift\\\" or \\\"ctrl+shift\\\".\"}," TAB_PROP "}}",
      tool_click },
    { "double_click", "Double-click", "Double-click an element (by ref) or a viewport point.",
      "{\"type\":\"object\",\"properties\":{" REF_PROP "," XY_PROPS "," TAB_PROP "}}",
      tool_double_click },
    { "hover", "Hover", "Move the mouse pointer over an element (by ref) or a viewport point.",
      "{\"type\":\"object\",\"properties\":{" REF_PROP "," XY_PROPS "," TAB_PROP "}}",
      tool_hover },
    { "type", "Type text",
      "Type text into the focused element, or click the element given by ref first. Newlines press Enter.",
      "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}," REF_PROP "," TAB_PROP "},\"required\":[\"text\"]}",
      tool_type },
    { "key", "Press keys",
      "Press keys or shortcuts, space separated, e.g. \"Enter\", \"Tab\", \"ctrl+a\", \"shift+Tab\", \"ArrowDown ArrowDown Enter\", \"Escape\", \"PageDown\", \"F5\".",
      "{\"type\":\"object\",\"properties\":{\"keys\":{\"type\":\"string\"},\"repeat\":{\"type\":\"integer\",\"description\":\"Repeat the whole sequence this many times (default 1).\"}," TAB_PROP "},\"required\":[\"keys\"]}",
      tool_key },
    { "scroll", "Scroll",
      "Scroll with the mouse wheel at an element (by ref), a viewport point, or the centre of the viewport. With only a ref and no direction, scrolls that element into view.",
      "{\"type\":\"object\",\"properties\":{\"direction\":{\"type\":\"string\",\"enum\":[\"up\",\"down\",\"left\",\"right\"]},\"amount\":{\"type\":\"number\",\"description\":\"Wheel ticks of 100 px (default 3).\"}," REF_PROP "," XY_PROPS "," TAB_PROP "}}",
      tool_scroll },
    { "form_input", "Set form value",
      "Set the value of an input, textarea, select (by option value or label), checkbox or radio (true/false), or contenteditable element, firing input and change events.",
      "{\"type\":\"object\",\"properties\":{" REF_PROP ",\"value\":{\"description\":\"New value: string, number, boolean, or an array of option values for a multiple select.\"}," TAB_PROP "},\"required\":[\"ref\",\"value\"]}",
      tool_form_input },
    { "screenshot", "Screenshot",
      "Capture the visible viewport of a tab as a PNG. Image pixels are CSS viewport pixels, the same coordinates click, hover and scroll take.",
      "{\"type\":\"object\",\"properties\":{" TAB_PROP "}}", tool_screenshot },
    { "javascript", "Run JavaScript",
      "Evaluate JavaScript in the page's global scope and return the value of the last expression (objects as JSON). A returned Promise is awaited.",
      "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"}," TAB_PROP "},\"required\":[\"code\"]}",
      tool_javascript },
    { "get_page_text", "Get page text",
      "Return the page's readable text (title, URL and body text) without markup.",
      "{\"type\":\"object\",\"properties\":{\"max_chars\":{\"type\":\"integer\",\"description\":\"Truncate after this many characters (default 100000).\"}," TAB_PROP "}}",
      tool_get_page_text },
    { "wait", "Wait", "Wait for a number of milliseconds (at most 30000), e.g. for a page to update.",
      "{\"type\":\"object\",\"properties\":{\"ms\":{\"type\":\"integer\",\"description\":\"Milliseconds to wait (default 1000).\"}}}",
      tool_wait },
};

static void
send_line(GString *s)
{
    g_string_append_c(s, '\n');
    fwrite(s->str, 1, s->len, stdout);
    fflush(stdout);
    g_string_free(s, TRUE);
}

static void
send_result(const RjValue *id, const char *result_json)
{
    GString *s = g_string_new("{\"jsonrpc\":\"2.0\",\"id\":");
    rj_write(s, id);
    g_string_append(s, ",\"result\":");
    g_string_append(s, result_json);
    g_string_append_c(s, '}');
    send_line(s);
}

static void
send_error(const RjValue *id, int code, const char *message)
{
    GString *s = g_string_new("{\"jsonrpc\":\"2.0\",\"id\":");
    rj_write(s, id);
    g_string_append_printf(s, ",\"error\":{\"code\":%d,\"message\":", code);
    rj_append_str(s, message);
    g_string_append(s, "}}");
    send_line(s);
}

static char *
pretty_client_name(const RjValue *info)
{
    const char *title = rj_get_str(info, "title");
    if (title && *title)
        return g_strdup(title);
    const char *name = rj_get_str(info, "name");
    if (!name || !*name)
        return NULL;
    GString *s = g_string_new(NULL);
    gboolean start = TRUE;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c == '-' || c == '_') {
            g_string_append_c(s, ' ');
            start = TRUE;
            continue;
        }
        g_string_append_c(s, start ? g_ascii_toupper(c) : c);
        start = FALSE;
    }
    return g_string_free(s, FALSE);
}

static void
handle_initialize(const RjValue *id, const RjValue *params)
{
    g_free(mcp.client_name);
    mcp.client_name = pretty_client_name(rj_get(params, "clientInfo"));
    const char *asked = rj_get_str(params, "protocolVersion");
    static const char *const known[] = {
        "2025-06-18", "2025-03-26", "2024-11-05",
    };
    const char *version = MCP_PROTOCOL_VERSION;
    for (gsize i = 0; asked && i < G_N_ELEMENTS(known); i++)
        if (!strcmp(asked, known[i]))
            version = known[i];
    GString *s = g_string_new("{\"protocolVersion\":");
    rj_append_str(s, version);
    g_string_append(s, ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
                       "\"serverInfo\":{\"name\":\"nordstjernen\",\"title\":"
                       "\"Nordstjernen Web Navigator\",\"version\":");
    rj_append_str(s, NS_VERSION);
    g_string_append(s, "},\"instructions\":");
    rj_append_str(s,
        "These tools drive the Nordstjernen browser window on the user's "
        "computer. Start with tabs_list or navigate, then read_page or find "
        "to get element refs (ref_N) and act on them with click, type, "
        "form_input, hover or scroll. Use screenshot to see the page; its "
        "pixel coordinates are what x/y arguments expect. Refs go stale "
        "after navigation: read the page again.");
    g_string_append_c(s, '}');
    send_result(id, s->str);
    g_string_free(s, TRUE);
}

static void
handle_tools_list(const RjValue *id)
{
    GString *s = g_string_new("{\"tools\":[");
    for (gsize i = 0; i < G_N_ELEMENTS(tools); i++) {
        if (i)
            g_string_append_c(s, ',');
        g_string_append(s, "{\"name\":");
        rj_append_str(s, tools[i].name);
        g_string_append(s, ",\"title\":");
        rj_append_str(s, tools[i].title);
        g_string_append(s, ",\"description\":");
        rj_append_str(s, tools[i].description);
        g_string_append(s, ",\"inputSchema\":");
        g_string_append(s, tools[i].schema);
        g_string_append_c(s, '}');
    }
    g_string_append(s, "]}");
    send_result(id, s->str);
    g_string_free(s, TRUE);
}

static void
handle_tools_call(const RjValue *id, const RjValue *params)
{
    const char *name = rj_get_str(params, "name");
    const RjValue *args = rj_get(params, "arguments");
    const McpTool *tool = NULL;
    for (gsize i = 0; name && i < G_N_ELEMENTS(tools); i++)
        if (!strcmp(tools[i].name, name))
            tool = &tools[i];
    if (!tool) {
        send_error(id, -32602, "unknown tool");
        return;
    }
    McpResult res = { g_string_new(NULL), NULL, FALSE };
    tool->fn(args && args->type == RJ_OBJ ? args : NULL, &res);
    GString *s = g_string_new("{\"content\":[");
    if (res.image) {
        g_string_append(s, "{\"type\":\"image\",\"mimeType\":\"image/png\","
                           "\"data\":");
        rj_append_str(s, res.image);
        g_string_append(s, "},");
    }
    g_string_append(s, "{\"type\":\"text\",\"text\":");
    rj_append_str(s, res.text->len ? res.text->str : "Done.");
    g_string_append_printf(s, "}],\"isError\":%s}",
                           res.error ? "true" : "false");
    send_result(id, s->str);
    g_string_free(s, TRUE);
    g_string_free(res.text, TRUE);
    g_free(res.image);
}

static void
handle_message(const char *line, gsize len)
{
    RjValue *msg = rj_parse(line, len);
    if (!msg || msg->type != RJ_OBJ) {
        RjValue null_id = { .type = RJ_NULL };
        send_error(&null_id, -32700, "parse error");
        rj_free(msg);
        return;
    }
    const RjValue *id = rj_get(msg, "id");
    const char *method = rj_get_str(msg, "method");
    const RjValue *params = rj_get(msg, "params");
    if (!method || !id) {
        rj_free(msg);
        return;
    }
    if (!strcmp(method, "initialize"))
        handle_initialize(id, params);
    else if (!strcmp(method, "ping"))
        send_result(id, "{}");
    else if (!strcmp(method, "tools/list"))
        handle_tools_list(id);
    else if (!strcmp(method, "tools/call"))
        handle_tools_call(id, params);
    else if (!strcmp(method, "resources/list"))
        send_result(id, "{\"resources\":[]}");
    else if (!strcmp(method, "prompts/list"))
        send_result(id, "{\"prompts\":[]}");
    else
        send_error(id, -32601, "method not found");
    rj_free(msg);
}

static GLogWriterOutput
log_to_stderr(GLogLevelFlags level, const GLogField *fields, gsize n_fields,
              gpointer data)
{
    (void)data;
    if (level & (G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO))
        return G_LOG_WRITER_HANDLED;
    char *text = g_log_writer_format_fields(level, fields, n_fields, FALSE);
    fprintf(stderr, "%s\n", text);
    g_free(text);
    return G_LOG_WRITER_HANDLED;
}

int
ns_mcp_run(const char *self_exe)
{
#ifdef G_OS_WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    g_log_set_writer_func(log_to_stderr, NULL, NULL);
    mcp.self_exe = g_strdup(self_exe);
    mcp.client = g_socket_client_new();
    g_socket_client_set_timeout(mcp.client, MCP_IO_TIMEOUT_S);
    mcp.buf = g_byte_array_new();
    GString *line = g_string_new(NULL);
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (c != '\n') {
            if (line->len < MCP_MAX_LINE)
                g_string_append_c(line, (char)c);
            continue;
        }
        while (line->len && (line->str[line->len - 1] == '\r' ||
                             line->str[line->len - 1] == ' '))
            g_string_truncate(line, line->len - 1);
        if (line->len)
            handle_message(line->str, line->len);
        g_string_truncate(line, 0);
    }
    g_string_free(line, TRUE);
    ctl_drop();
    g_clear_object(&mcp.client);
    g_byte_array_free(mcp.buf, TRUE);
    g_free(mcp.self_exe);
    g_free(mcp.client_name);
    return 0;
}
