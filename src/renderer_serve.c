/* Nordstjernen — renderer request dispatch over the HTTP/JSON IPC protocol,
   shared by nordstjernen-renderer and the single-process in-process host. */

#define _GNU_SOURCE 1
#include "renderer_serve.h"
#include "download.h"
#include "libnordstjernen.h"
#include "renderer_tiles.h"
#include "layers.h"
#include "print.h"
#include "proc_limits.h"
#include "net.h"
#include "image.h"
#include "texture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_BFCACHE_MAX 4
#define NS_SCROLL_ACTIVE_US   150000
#define NS_SCROLL_TICK_GAP_US 250000

struct ns_renderer_session {
    int            ctrl_w;
    unsigned char *fb;
    int            max_w;
    int            max_h;
    int            shm_mode;
    ns_browser    *cur;
    ns_session_storage *session_storage;
    ns_browser    *bf[NS_BFCACHE_MAX];
    int            bf_n;
    int            tick_budget_ms;
    gint64         scroll_until_us;
    gint64         last_tick_us;
    gint64         tick_cost_us;
    int            tick_deferred;
    int            frame_valid;
    long           frame_sx;
    long           frame_sy;
    int            frame_w;
    int            frame_h;
    double         frame_scale;
    char          *post_url;
    char          *post_body;
    size_t         post_len;
    char          *post_ct;
    char          *page_nav;
    ns_tiles      *tiles;
};

static double
request_device_pixel_ratio(const char *body)
{
    double dpr = 0;
    json_get_double(body, "dpr", &dpr);
    return dpr > 0 && isfinite(dpr) ? CLAMP(dpr, 0.25, 40.0) : 0;
}

static void
session_apply_device_pixel_ratio(ns_renderer_session *s, const char *body)
{
    double dpr = request_device_pixel_ratio(body);
    if (dpr > 0 && ns_browser_set_device_pixel_ratio(s->cur, dpr) > 0)
        s->frame_valid = 0;
}

static void
session_bfcache_park_or_close(ns_renderer_session *s, ns_browser *b)
{
    if (!b)
        return;
    if (!ns_browser_bfcache_eligible(b)) {
        ns_browser_close(b);
        return;
    }
    ns_browser_bfcache_park(b);
    if (s->bf_n >= NS_BFCACHE_MAX) {
        ns_browser_close(s->bf[0]);
        for (int i = 1; i < s->bf_n; i++)
            s->bf[i - 1] = s->bf[i];
        s->bf_n--;
    }
    s->bf[s->bf_n++] = b;
}

static ns_browser *
session_bfcache_take(ns_renderer_session *s, const char *url)
{
    if (!url)
        return NULL;
    for (int i = s->bf_n - 1; i >= 0; i--) {
        char *u = ns_browser_url(s->bf[i]);
        int match = u && strcmp(u, url) == 0;
        free(u);
        if (!match)
            continue;
        ns_browser *b = s->bf[i];
        for (int j = i + 1; j < s->bf_n; j++)
            s->bf[j - 1] = s->bf[j];
        s->bf_n--;
        return b;
    }
    return NULL;
}

static void
session_bfcache_clear(ns_renderer_session *s)
{
    for (int i = 0; i < s->bf_n; i++)
        ns_browser_close(s->bf[i]);
    s->bf_n = 0;
}

static char *
session_take_config(void)
{
    g_autofree char *text = ns_config_take_pending_save();
    if (!text) return NULL;
    gchar *b64 = g_base64_encode((const guchar *)text, strlen(text));
    char *out = strdup(b64);
    g_free(b64);
    return out;
}

static void
session_stash_post(ns_renderer_session *s, const char *href)
{
    if (!s || !s->cur)
        return;
    free(s->page_nav);
    s->page_nav = (href && *href) ? strdup(href) : NULL;
    size_t len = 0;
    char *ct = NULL;
    char *pb = ns_browser_take_post(s->cur, &len, &ct);
    if (!pb)
        return;
    free(s->post_url);
    free(s->post_body);
    free(s->post_ct);
    s->post_url = (href && *href) ? strdup(href) : NULL;
    s->post_body = pb;
    s->post_len = len;
    s->post_ct = ct;
}

static void
session_clear_post(ns_renderer_session *s)
{
    if (!s)
        return;
    free(s->post_url);
    free(s->post_body);
    free(s->post_ct);
    s->post_url = NULL;
    s->post_body = NULL;
    s->post_ct = NULL;
    s->post_len = 0;
}

static int
clamp(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static int
css_viewport_extent(long css_px, int max_device_px)
{
    long max_css_px = (long)(max_device_px / NS_PROC_ZOOM_MIN);
    return (int)CLAMP(css_px, 1, max_css_px);
}

static void
reply_str(int fd, const char *key, const char *val)
{
    char *e = json_escape(val ? val : "");
    char *json = NULL;
    int n = asprintf(&json, "{\"%s\":\"%s\"}", key, e ? e : "");
    if (n > 0)
        http_write_response(fd, 200, "application/json", NULL, json,
                            (size_t)n);
    free(json);
    free(e);
}

static void
reply_str2(int fd, const char *key1, const char *val1,
           const char *key2, const char *val2)
{
    char *escaped1 = json_escape(val1 ? val1 : "");
    char *escaped2 = json_escape(val2 ? val2 : "");
    char *json = NULL;
    int n = asprintf(&json, "{\"%s\":\"%s\",\"%s\":\"%s\"}",
                     key1, escaped1 ? escaped1 : "",
                     key2, escaped2 ? escaped2 : "");
    if (n > 0)
        http_write_response(fd, 200, "application/json", NULL, json,
                            (size_t)n);
    free(json);
    free(escaped1);
    free(escaped2);
}

static void
reply_href_changed(int fd, const char *href, int changed, int newtab)
{
    char *e = json_escape(href ? href : "");
    char *json = NULL;
    int n = asprintf(&json,
                     "{\"href\":\"%s\",\"changed\":%d,\"newtab\":%d}",
                     e ? e : "", changed ? 1 : 0, newtab ? 1 : 0);
    if (n > 0)
        http_write_response(fd, 200, "application/json", NULL, json,
                            (size_t)n);
    free(json);
    free(e);
}

static unsigned char *
favicon_bgra(ns_browser *b, int *out_w, int *out_h)
{
    char *url = ns_browser_favicon_url(b);
    if (!url || !*url) {
        free(url);
        return NULL;
    }
    ns_response *resp = ns_net_fetch_blocking(url, NULL, NULL);
    free(url);
    if (!resp || resp->status >= 400 || !resp->body || resp->body->len == 0) {
        if (resp)
            ns_response_free(resp);
        return NULL;
    }
    int w = 0, h = 0;
    ns_texture *tex =
        ns_image_decode_bytes(resp->body->data, resp->body->len, &w, &h);
    ns_response_free(resp);
    if (!tex)
        return NULL;
    if (w < 1 || h < 1 || w > 512 || h > 512) {
        ns_texture_unref(tex);
        return NULL;
    }
    gsize stride = (gsize)w * 4u;
    unsigned char *px = malloc(stride * (gsize)h);
    if (!px) {
        ns_texture_unref(tex);
        return NULL;
    }
    ns_texture_download(tex, px, stride);
    ns_texture_unref(tex);
    *out_w = w;
    *out_h = h;
    return px;
}

ns_renderer_session *
ns_renderer_session_new(int ctrl_w, unsigned char *fb, int max_w, int max_h,
                        int shm_mode)
{
    if (!fb || max_w <= 0 || max_h <= 0)
        return NULL;
    ns_renderer_session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->ctrl_w = ctrl_w;
    s->fb = fb;
    s->max_w = max_w;
    s->max_h = max_h;
    s->shm_mode = shm_mode;
    s->tick_budget_ms = 16;
    s->frame_scale = 1.0;
    s->tiles = ns_tiles_new();
    return s;
}

int
ns_renderer_session_busy(const ns_renderer_session *s)
{
    if (!s)
        return 0;
    if (ns_browser_busy(s->cur))
        return 1;
    for (int i = 0; i < s->bf_n; i++)
        if (ns_browser_busy(s->bf[i]))
            return 1;
    return 0;
}

GPtrArray *
ns_renderer_session_print(ns_renderer_session *s, ns_print_setup *out_setup)
{
    if (!s || !s->cur || !out_setup)
        return NULL;
    s->frame_valid = 0;
    return ns_browser_print_pages(s->cur, out_setup);
}

void
ns_renderer_session_free(ns_renderer_session *s)
{
    if (!s)
        return;
    session_clear_post(s);
    session_bfcache_clear(s);
    if (s->cur)
        ns_browser_close(s->cur);
    ns_tiles_free(s->tiles);
    ns_session_storage_free(s->session_storage);
    free(s);
}

static int
serve_append_hdr(char *buf, int pos, size_t cap, const char *name,
                 const char *value, int maxval)
{
    if (!value || !*value || pos < 0 || (size_t)pos >= cap) return pos;
    int need = snprintf(NULL, 0, "%s: %.*s\r\n", name, maxval, value);
    if (need <= 0 || (size_t)pos + (size_t)need >= cap) return pos;
    return pos + snprintf(buf + pos, cap - (size_t)pos, "%s: %.*s\r\n",
                          name, maxval, value);
}

static gboolean
session_tick_deferred(const struct ns_renderer_session *s, gint64 now)
{
    if (now >= s->scroll_until_us) return FALSE;
    gint64 gap = s->tick_cost_us * 4;
    if (gap < NS_SCROLL_TICK_GAP_US) gap = NS_SCROLL_TICK_GAP_US;
    return now - s->last_tick_us < gap;
}

static int
session_tick(struct ns_renderer_session *s)
{
    gint64 now = g_get_monotonic_time();
    if (session_tick_deferred(s, now)) {
        s->tick_deferred = 1;
        return 0;
    }
    int changed = ns_browser_tick(s->cur, s->tick_budget_ms);
    s->last_tick_us = g_get_monotonic_time();
    s->tick_cost_us = s->last_tick_us - now;
    s->tick_deferred = 0;
    return changed;
}

static gboolean
session_animating(const struct ns_renderer_session *s)
{
    return ns_browser_animating(s->cur) || s->tick_deferred;
}

static int
session_wake_ms(const struct ns_renderer_session *s)
{
    if (s->tick_deferred) return 0;
    int wake = ns_browser_wake_ms(s->cur);
    return wake > 0 ? wake : 0;
}

static void
session_note_scroll(struct ns_renderer_session *s, long sx, long sy,
                    gboolean wheel)
{
    if (wheel || (s->frame_valid && (sx != s->frame_sx || sy != s->frame_sy)))
        s->scroll_until_us = g_get_monotonic_time() + NS_SCROLL_ACTIVE_US;
}

typedef struct render_view {
    long   sx, sy;
    int    vw, vh;
    double scale;
    int    caret;
    int    ticked;
    int    caret_changed;
    int    requested_x, requested_y;
    int    page_w, page_h;
    int    wheel_snapped;
    int    wheel_answered;
    int    wheel_prevented;
    int    unchanged;
    int    render_rc;
} render_view;

static int
render_max_scroll(int page_extent, int view_px, double scale)
{
    int max = page_extent - (int)ceil((double)view_px / scale);
    return max > 0 ? max : 0;
}

static void
render_apply_pending_scroll(ns_renderer_session *s, render_view *rv)
{
    ns_browser_take_pending_scroll(s->cur, &rv->requested_x,
                                   &rv->requested_y);
    ns_browser_page_size(s->cur, &rv->page_w, &rv->page_h);
    if (rv->requested_y >= 0) {
        rv->requested_y = MIN(rv->requested_y,
                              render_max_scroll(rv->page_h, rv->vh,
                                                rv->scale));
        rv->sy = rv->requested_y;
    }
    if (rv->requested_x >= 0) {
        rv->requested_x = MIN(rv->requested_x,
                              render_max_scroll(rv->page_w, rv->vw,
                                                rv->scale));
        rv->sx = rv->requested_x;
    }
}

static void
render_apply_wheel(ns_renderer_session *s, const char *body, render_view *rv,
                   long wheel_dx, long wheel_dy)
{
    if (!wheel_dx && !wheel_dy) return;
    long wheel_x = 0, wheel_y = 0, wheel_viewport = 0;
    json_get_long(body, "wheel_x", &wheel_x);
    json_get_long(body, "wheel_y", &wheel_y);
    json_get_long(body, "wheel_viewport", &wheel_viewport);
    if (!wheel_viewport &&
        ns_browser_scroll_at_full(s->cur, (int)wheel_x, (int)wheel_y,
                                  (int)wheel_dx, (int)wheel_dy,
                                  &rv->wheel_snapped)) {
        s->frame_valid = 0;
        return;
    }
    int max_x = render_max_scroll(rv->page_w, rv->vw, rv->scale);
    int max_y = render_max_scroll(rv->page_h, rv->vh, rv->scale);
    long nx = clamp((int)(rv->sx + wheel_dx), 0, max_x);
    long ny = clamp((int)(rv->sy + wheel_dy), 0, max_y);
    if (nx != rv->sx) rv->requested_x = (int)(rv->sx = nx);
    if (ny != rv->sy) rv->requested_y = (int)(rv->sy = ny);
}

static void
render_dispatch_wheel(ns_renderer_session *s, const char *body,
                      render_view *rv)
{
    long ev = 0, x = 0, y = 0, dx = 0, dy = 0, mods = 0;
    if (json_get_long(body, "wev", &ev) != 0 || !ev) return;
    json_get_long(body, "wev_x", &x);
    json_get_long(body, "wev_y", &y);
    json_get_long(body, "wev_dx", &dx);
    json_get_long(body, "wev_dy", &dy);
    json_get_long(body, "wev_mods", &mods);
    int changed = 0;
    rv->wheel_answered = 1;
    rv->wheel_prevented = ns_browser_wheel_event(s->cur, (int)x, (int)y,
                                                 (int)dx, (int)dy, (int)mods,
                                                 &changed);
    if (changed) s->frame_valid = 0;
}

static void
render_apply_snap(ns_renderer_session *s, render_view *rv, int wheeled)
{
    int snap_x = (int)rv->sx, snap_y = (int)rv->sy;
    if (!ns_browser_snap_document(s->cur, (double)rv->vw / rv->scale,
                                  (double)rv->vh / rv->scale,
                                  (int)s->frame_sx, (int)s->frame_sy,
                                  &snap_x, &snap_y))
        return;
    if (snap_x != (int)rv->sx) rv->requested_x = (int)(rv->sx = snap_x);
    if (snap_y != (int)rv->sy) rv->requested_y = (int)(rv->sy = snap_y);
    if (wheeled) rv->wheel_snapped = 1;
}

static void
session_note_frame(ns_renderer_session *s, const render_view *rv)
{
    s->frame_valid = 1;
    s->frame_sx = rv->sx;
    s->frame_sy = rv->sy;
    s->frame_w = rv->vw;
    s->frame_h = rv->vh;
    s->frame_scale = rv->scale;
}

static void
render_frame(ns_renderer_session *s, render_view *rv)
{
    rv->unchanged = s->frame_valid && rv->ticked == 0 && !rv->caret_changed &&
                    rv->sx == s->frame_sx && rv->sy == s->frame_sy &&
                    rv->vw == s->frame_w && rv->vh == s->frame_h &&
                    rv->scale == s->frame_scale;
    if (rv->unchanged) return;
    int stride = rv->vw * 4;
    rv->render_rc = ns_browser_render_argb32(s->cur, (int)rv->sx,
                                             (int)rv->sy, rv->vw, rv->vh,
                                             rv->scale, s->fb, stride);
    if (rv->render_rc == 0) {
        session_note_frame(s, rv);
    } else {
        memset(s->fb, 0xff, (size_t)stride * (size_t)rv->vh);
        s->frame_valid = 0;
    }
}

static char *
render_take_line(ns_browser *b, char *(*take)(ns_browser *), int keep_lf)
{
    char *v = take(b);
    for (char *p = v; p && *p; p++) {
        if (*p == '\r') *p = ' ';
        else if (*p == '\n') *p = keep_lf ? '\x1f' : ' ';
    }
    return v;
}

static void
render_reply(ns_renderer_session *s, int ctrl_w, const render_view *rv,
             const GString *tiles)
{
    char *nav = render_take_line(s->cur, ns_browser_take_pending_nav, 0);
    if (nav)
        session_stash_post(s, nav);
    char *webgl = render_take_line(s->cur, ns_browser_take_pending_webgl, 0);
    char *camera = render_take_line(s->cur, ns_browser_take_pending_camera, 0);
    char *download = render_take_line(s->cur, ns_browser_take_pending_download,
                                      0);
    char *config = session_take_config();
    char *audio = render_take_line(s->cur, ns_browser_take_pending_audio, 1);
    char *window_action =
        render_take_line(s->cur, ns_browser_take_pending_window_action, 0);
    int animating = session_animating(s) ? 1 : 0;
    if (ns_browser_caret_blinking(s->cur)) animating |= 2;
    if (rv->wheel_snapped) animating |= 4;
    if (rv->wheel_answered) animating |= 8;
    if (rv->wheel_prevented) animating |= 16;
    int clipboard_pending = ns_browser_has_pending_clipboard(s->cur) ? 1 : 0;
    char hdrs[32768];
    int hn = snprintf(hdrs, sizeof hdrs,
             "X-W: %d\r\nX-H: %d\r\nX-Stride: %d\r\nX-Anim: %d\r\n"
             "X-PageW: %d\r\nX-PageH: %d\r\nX-ScrollY: %d\r\n"
             "X-ScrollX: %d\r\nX-Render-RC: %d\r\nX-Clipboard: %d\r\n"
             "X-Wake: %d\r\n%s%s",
             rv->vw, rv->vh, rv->vw * 4, animating,
             rv->page_w, rv->page_h, rv->requested_y, rv->requested_x,
             rv->render_rc, clipboard_pending,
             (animating & 1) ? session_wake_ms(s) : 0,
             rv->unchanged ? "X-Unchanged: 1\r\n" : "",
             tiles ? "X-Tiles: 1\r\n" : "");
    if (ns_browser_popup_pending() && hn > 0 && (size_t)hn < sizeof hdrs)
        hn += snprintf(hdrs + hn, sizeof hdrs - (size_t)hn, "X-Popup: 1\r\n");
    hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-Nav", nav, 2000);
    hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-WebGL", webgl, 2000);
    hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-Camera", camera, 2000);
    hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-Download", download,
                          3000);
    hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-Config", config, 12000);
    hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-Audio", audio, 16000);
    hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-Window-Action",
                          window_action, 31);
    free(nav);
    free(webgl);
    free(camera);
    free(download);
    free(config);
    free(audio);
    free(window_action);
    if (tiles)
        http_write_response(ctrl_w, 200, "text/plain", hdrs, tiles->str,
                            tiles->len);
    else if (s->shm_mode || rv->unchanged)
        http_write_response(ctrl_w, 200, "application/octet-stream",
                            hdrs, NULL, 0);
    else
        http_write_response(ctrl_w, 200, "application/octet-stream",
                            hdrs, s->fb,
                            (size_t)rv->vw * 4u * (size_t)rv->vh);
}

static int
session_render_tiles(ns_renderer_session *s, int ctrl_w, const char *body,
                     render_view *rv)
{
    ns_tiles_view view = { rv->sx, rv->sy, rv->vw, rv->vh, rv->scale,
                           rv->page_h };
    gboolean invalid = !s->frame_valid || rv->ticked || rv->caret_changed;
    GString *desc = g_string_sized_new(4096);
    int rc = ns_tiles_render(s->tiles, s->cur, body, &view, invalid, s->fb,
                             (size_t)s->max_w * (size_t)s->max_h * 4u, desc);
    if (rc == 0) {
        session_note_frame(s, rv);
        render_reply(s, ctrl_w, rv, desc);
    }
    g_string_free(desc, TRUE);
    return rc;
}

static void
render_view_parse(ns_renderer_session *s, const char *body, render_view *rv)
{
    long w = 0, h = 0, caret = 0;
    memset(rv, 0, sizeof *rv);
    rv->scale = 1.0;
    rv->requested_x = rv->requested_y = -1;
    json_get_long(body, "width", &w);
    json_get_long(body, "height", &h);
    json_get_long(body, "scroll_x", &rv->sx);
    json_get_long(body, "scroll_y", &rv->sy);
    json_get_long(body, "caret", &caret);
    json_get_double(body, "scale", &rv->scale);
    if (!(rv->scale > 0)) rv->scale = 1.0;
    rv->caret = caret != 0;
    rv->vw = clamp((int)w, 1, s->max_w);
    rv->vh = clamp((int)h, 1, s->max_h);
}

static int
session_render(ns_renderer_session *s, int ctrl_w, const char *body)
{
    render_view rv;
    render_view_parse(s, body, &rv);
    session_apply_device_pixel_ratio(s, body);
    if (!s->cur) {
        http_write_response(ctrl_w, 200, "application/octet-stream",
                            "X-W: 0\r\nX-H: 0\r\nX-Stride: 0\r\n"
                            "X-Anim: 0\r\n", NULL, 0);
        return 0;
    }
    long wheel_dx = 0, wheel_dy = 0;
    json_get_long(body, "wheel_dx", &wheel_dx);
    json_get_long(body, "wheel_dy", &wheel_dy);
    session_note_scroll(s, rv.sx, rv.sy, wheel_dx || wheel_dy);
    long fill = 0;
    json_get_long(body, "fill", &fill);
    rv.ticked = s->frame_valid && !fill ? session_tick(s) : 0;
    render_apply_pending_scroll(s, &rv);
    render_dispatch_wheel(s, body, &rv);
    if (rv.wheel_prevented)
        wheel_dx = wheel_dy = 0;
    render_apply_wheel(s, body, &rv, wheel_dx, wheel_dy);
    render_apply_snap(s, &rv, wheel_dx || wheel_dy);
    rv.caret_changed = ns_browser_set_caret_blink_active(s->cur, rv.caret);
    if (s->shm_mode && ns_tiles_requested(body) &&
        session_render_tiles(s, ctrl_w, body, &rv) == 0)
        return 0;
    render_frame(s, &rv);
    render_reply(s, ctrl_w, &rv, NULL);
    return 0;
}

int
ns_renderer_session_handle(ns_renderer_session *s, const http_head *head,
                           const char *body)
{
    int ctrl_w = s->ctrl_w;

    if (strcmp(head->path, "/quit") == 0) {
        http_write_response(ctrl_w, 200, "text/plain", NULL, NULL, 0);
        return 1;
    }

    if (strcmp(head->path, "/webgl") == 0) {
        char *origin = json_get_str(body, "origin");
        long allow = 0;
        json_get_long(body, "allow", &allow);
        if (s->cur && origin)
            ns_browser_resolve_webgl(s->cur, origin, (int)allow);
        s->frame_valid = 0;
        http_write_response(ctrl_w, 200, "text/plain", NULL, NULL, 0);
        free(origin);
        return 0;
    }

    if (strcmp(head->path, "/colorscheme") == 0) {
        long dark = 0;
        json_get_long(body, "dark", &dark);
        ns_browser_set_color_scheme((int)dark);
        s->frame_valid = 0;
        http_write_response(ctrl_w, 200, "text/plain", NULL, NULL, 0);
        return 0;
    }

    if (strcmp(head->path, "/camera") == 0) {
        char *origin = json_get_str(body, "origin");
        long allow = 0;
        json_get_long(body, "allow", &allow);
        if (s->cur && origin)
            ns_browser_resolve_camera(s->cur, origin, (int)allow);
        s->frame_valid = 0;
        http_write_response(ctrl_w, 200, "text/plain", NULL, NULL, 0);
        free(origin);
        return 0;
    }

    if (strcmp(head->path, "/open") == 0) {
        ns_config_reload();
        char *url = json_get_str(body, "url");
        long w = 0, h = 0, settle = 0, history = 0, user_activated = 0;
        long reload = 0;
        json_get_long(body, "width", &w);
        json_get_long(body, "height", &h);
        json_get_long(body, "settle_ms", &settle);
        json_get_long(body, "history", &history);
        json_get_long(body, "user_activated", &user_activated);
        json_get_long(body, "reload", &reload);
        long popups = 0;
        json_get_long(body, "popups", &popups);
        ns_browser_set_popups_supported((int)popups);
        char *opener_token = json_get_str(body, "opener_token");
        char *opener_origin = json_get_str(body, "opener_origin");
        ns_browser_set_opener(opener_token, opener_origin);
        free(opener_token);
        free(opener_origin);
        int vw = css_viewport_extent(w, s->max_w);
        int vh = css_viewport_extent(h, s->max_h);
        double dpr = request_device_pixel_ratio(body);
        if (dpr > 0)
            ns_browser_set_device_pixel_ratio(NULL, dpr);
        ns_browser *restored = (history && !reload && url)
            ? session_bfcache_take(s, url) : NULL;
        char *referrer = (!history && !reload && s->cur)
            ? ns_browser_url(s->cur) : NULL;
        if (s->cur && reload)
            ns_browser_close(s->cur);
        else if (s->cur)
            session_bfcache_park_or_close(s, s->cur);
        s->cur = NULL;
        gboolean page_initiated = !user_activated ||
            (s->page_nav && url && strcmp(s->page_nav, url) == 0);
        g_clear_pointer(&s->page_nav, free);
        if (referrer && !restored && url && ns_url_same_origin(referrer, url))
            ns_browser_set_next_referrer(referrer);
        else if (referrer && !restored && url && page_initiated)
            ns_browser_set_next_initiator(referrer);
        ns_browser_set_next_user_activated(user_activated != 0);
        ns_browser_set_next_reload(reload != 0 && url && *url);
        if (!s->session_storage) s->session_storage = ns_session_storage_new();
        ns_browser_set_next_session_storage(s->session_storage);
        g_free(referrer);
        s->frame_valid = 0;
        ns_net_log_clear();
        if (restored) {
            ns_browser_bfcache_restore(restored, vw, (double)vh);
            if (dpr > 0)
                ns_browser_set_device_pixel_ratio(restored, dpr);
            s->cur = restored;
        } else if (url && s->post_body && s->post_url &&
            strcmp(url, s->post_url) == 0)
            s->cur = ns_browser_open_post_viewport(url, vw, vh, (int)settle,
                                                   s->post_body, s->post_len,
                                                   s->post_ct);
        else
            s->cur = url ? ns_browser_open_viewport(url, vw, vh, (int)settle)
                         : NULL;
        session_clear_post(s);
        int pw = 0, ph = 0, ok = s->cur != NULL;
        int security = 0;
        const char *ip = NULL;
        char *title = NULL, *final_url = NULL, *nav = NULL;
        char *title_escaped = NULL, *url_escaped = NULL, *nav_escaped = NULL;
        char *ip_escaped = NULL;
        if (s->cur) {
            ns_browser_page_size(s->cur, &pw, &ph);
            title = ns_browser_title(s->cur);
            final_url = ns_browser_url(s->cur);
            nav = ns_browser_take_pending_nav(s->cur);
            if (nav) session_stash_post(s, nav);
            security = ns_browser_security(s->cur, &ip);
        }
        title_escaped = json_escape(title ? title : "");
        url_escaped = json_escape(final_url ? final_url : (url ? url : ""));
        nav_escaped = json_escape(nav ? nav : "");
        ip_escaped = json_escape(ip ? ip : "");
        char *json = NULL;
        int n = asprintf(&json,
                         "{\"ok\":%d,\"page_width\":%d,\"page_height\":%d,"
                         "\"title\":\"%s\",\"url\":\"%s\",\"nav\":\"%s\","
                         "\"security\":%d,\"ip\":\"%s\"}",
                         ok, pw, ph,
                         title_escaped ? title_escaped : "",
                         url_escaped ? url_escaped : "",
                         nav_escaped ? nav_escaped : "",
                         security, ip_escaped ? ip_escaped : "");
        if (n >= 0)
            http_write_response(ctrl_w, 200, "application/json", NULL, json,
                                (size_t)n);
        free(json);
        free(title_escaped);
        free(url_escaped);
        free(nav_escaped);
        free(ip_escaped);
        free(title);
        free(final_url);
        free(nav);
        free(url);
        return 0;
    }

    if (strcmp(head->path, "/tick") == 0) {
        if (!s->cur) {
            const char *json = "{\"ok\":0}";
            http_write_response(ctrl_w, 200, "application/json", NULL, json,
                                strlen(json));
            return 0;
        }
        int changed = session_tick(s);
        if (changed) s->frame_valid = 0;
        else if (ns_browser_video_layer_frames_pending(s->cur)) changed = 1;
        char *nav = ns_browser_take_pending_nav(s->cur);
        if (nav) {
            for (char *p = nav; *p; p++)
                if (*p == '\r' || *p == '\n') *p = ' ';
            session_stash_post(s, nav);
        }
        char *webgl = ns_browser_take_pending_webgl(s->cur);
        char *camera = ns_browser_take_pending_camera(s->cur);
        char *download = ns_browser_take_pending_download(s->cur);
        char *config = session_take_config();
        char *audio = ns_browser_take_pending_audio(s->cur);
        char *window_action = ns_browser_take_pending_window_action(s->cur);
        if (audio)
            for (char *p = audio; *p; p++) {
                if (*p == '\r') *p = ' ';
                else if (*p == '\n') *p = '\x1f';
            }
        char *nav_e = json_escape(nav ? nav : "");
        char *webgl_e = json_escape(webgl ? webgl : "");
        char *camera_e = json_escape(camera ? camera : "");
        char *download_e = json_escape(download ? download : "");
        char *config_e = json_escape(config ? config : "");
        char *audio_e = json_escape(audio ? audio : "");
        char *window_action_e = json_escape(window_action ? window_action : "");
        char *title = ns_browser_title(s->cur);
        char *page_url = ns_browser_url(s->cur);
        char *title_e = json_escape(title ? title : "");
        char *url_e = json_escape(page_url ? page_url : "");
        int url_pushed = ns_browser_take_soft_nav_pushed(s->cur);
        int page_w = 0, page_h = 0;
        ns_browser_page_size(s->cur, &page_w, &page_h);
        char *json = NULL;
        int n = asprintf(&json,
            "{\"ok\":1,\"changed\":%d,\"animating\":%d,\"wake_ms\":%d,"
            "\"page_width\":%d,\"page_height\":%d,\"nav\":\"%s\","
            "\"webgl\":\"%s\",\"camera\":\"%s\","
            "\"download\":\"%s\",\"config\":\"%s\",\"audio\":\"%s\","
            "\"window_action\":\"%s\",\"title\":\"%s\",\"url\":\"%s\","
            "\"url_pushed\":%d,\"popup\":%d}",
            changed != 0, session_animating(s) ? 1 : 0, session_wake_ms(s),
            page_w, page_h, nav_e ? nav_e : "", webgl_e ? webgl_e : "",
            camera_e ? camera_e : "", download_e ? download_e : "",
            config_e ? config_e : "",
            audio_e ? audio_e : "", window_action_e ? window_action_e : "",
            title_e ? title_e : "", url_e ? url_e : "", url_pushed,
            ns_browser_popup_pending());
        if (n >= 0)
            http_write_response(ctrl_w, 200, "application/json", NULL, json,
                                (size_t)n);
        free(json);
        free(nav_e);
        free(webgl_e);
        free(camera_e);
        free(download_e);
        free(config_e);
        free(audio_e);
        free(window_action_e);
        free(title_e);
        free(url_e);
        free(title);
        free(page_url);
        free(nav);
        free(webgl);
        free(camera);
        free(download);
        free(config);
        free(audio);
        free(window_action);
        return 0;
    }

    if (strcmp(head->path, "/render") == 0)
        return session_render(s, ctrl_w, body);

    if (strcmp(head->path, "/favicon") == 0) {
        int fw = 0, fh = 0;
        unsigned char *px = s->cur ? favicon_bgra(s->cur, &fw, &fh) : NULL;
        if (px) {
            char hdrs[96];
            int hn = snprintf(hdrs, sizeof hdrs,
                              "X-W: %d\r\nX-H: %d\r\nX-Stride: %d\r\n",
                              fw, fh, fw * 4);
            http_write_response(ctrl_w, 200, "application/octet-stream",
                                hn > 0 ? hdrs : NULL, px,
                                (size_t)fw * (size_t)fh * 4u);
            free(px);
        } else {
            http_write_response(ctrl_w, 200, "application/octet-stream",
                                "X-W: 0\r\nX-H: 0\r\nX-Stride: 0\r\n", NULL, 0);
        }
        return 0;
    }

    if (strcmp(head->path, "/link") == 0 ||
        strcmp(head->path, "/click") == 0 ||
        strcmp(head->path, "/select") == 0) {
        long x = 0, y = 0, mods = 0, kind = 0, button = 0, clicks = 1;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        json_get_long(body, "mods", &mods);
        json_get_long(body, "kind", &kind);
        json_get_long(body, "button", &button);
        json_get_long(body, "clicks", &clicks);
        char *href = NULL;
        if (s->cur && strcmp(head->path, "/link") == 0) {
            href = ns_browser_link_at(s->cur, (int)x, (int)y);
            char *cursor = ns_browser_cursor_at(s->cur, (int)x, (int)y);
            reply_str2(ctrl_w, "href", href, "cursor", cursor);
            free(cursor);
            free(href);
            return 0;
        }
        s->frame_valid = 0;
        if (s->cur && strcmp(head->path, "/click") == 0) {
            href = ns_browser_press_full(s->cur, (int)x, (int)y, (int)mods,
                                         (int)button, (int)clicks);
            session_stash_post(s, href);
        } else if (s->cur)
            href = ns_browser_select(s->cur, (int)kind, (int)x, (int)y);
        reply_str(ctrl_w, "href", href);
        free(href);
        return 0;
    }

    if (strcmp(head->path, "/key") == 0) {
        long kind = 0, keycode = 0, mods = 0;
        json_get_long(body, "kind", &kind);
        json_get_long(body, "keycode", &keycode);
        json_get_long(body, "mods", &mods);
        char *key = json_get_str(body, "key");
        char *code = json_get_str(body, "code");
        s->frame_valid = 0;
        int prevented = 0;
        char *href = s->cur ? ns_browser_key_full(s->cur, (int)kind,
                                                  key ? key : "",
                                                  code ? code : "",
                                                  (int)keycode, (int)mods,
                                                  &prevented)
                            : NULL;
        session_stash_post(s, href);
        char *e = json_escape(href ? href : "");
        char *json = NULL;
        int n = asprintf(&json, "{\"href\":\"%s\",\"prevented\":%d}",
                         e ? e : "", prevented ? 1 : 0);
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL, json,
                                (size_t)n);
        free(json);
        free(e);
        free(href);
        free(key);
        free(code);
        return 0;
    }

    if (strcmp(head->path, "/clipboard") == 0) {
        char *text = s->cur ? ns_browser_take_pending_clipboard(s->cur) : NULL;
        http_write_response(ctrl_w, 200, "text/plain; charset=utf-8", NULL,
                            text ? text : "", text ? strlen(text) : 0);
        free(text);
        return 0;
    }

    if (strcmp(head->path, "/release") == 0) {
        s->frame_valid = 0;
        int changed = 0, newtab = 0;
        long x = 0, y = 0, button = 0, clicks = 1;
        gboolean at = json_get_long(body, "x", &x) == 0 &&
                      json_get_long(body, "y", &y) == 0;
        json_get_long(body, "button", &button);
        json_get_long(body, "clicks", &clicks);
        char *href = !s->cur ? NULL
                   : at ? ns_browser_release_full(s->cur, (int)x, (int)y,
                                                  (int)button, (int)clicks,
                                                  &changed, &newtab)
                        : ns_browser_release_click(s->cur, &changed);
        if (!newtab)
            session_stash_post(s, href);
        reply_href_changed(ctrl_w, href, changed > 0, newtab);
        free(href);
        return 0;
    }

    if (strcmp(head->path, "/dropfiles") == 0) {
        long x = 0, y = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        char *paths = json_get_str(body, "paths");
        int changed = 0;
        if (s->cur && paths && *paths) {
            char **list = g_strsplit(paths, "\n", -1);
            guint count = list ? g_strv_length(list) : 0;
            if (count > 0)
                changed = ns_browser_drop_files(s->cur, (int)x, (int)y,
                                                (const char *const *)list,
                                                (int)count);
            g_strfreev(list);
        }
        if (changed > 0) s->frame_valid = 0;
        char *json = NULL;
        int n = asprintf(&json, "{\"changed\":%d}", changed > 0 ? 1 : 0);
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL,
                                json, (size_t)n);
        free(json);
        free(paths);
        return 0;
    }

    if (strcmp(head->path, "/hover") == 0) {
        long x = 0, y = 0, mods = 0, buttons = -1;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        json_get_long(body, "mods", &mods);
        json_get_long(body, "buttons", &buttons);
        int changed = s->cur ? ns_browser_hover_full(s->cur, (int)x, (int)y,
                                                     (int)mods, (int)buttons)
                             : 0;
        if (changed > 0)
            s->frame_valid = 0;
        char *href = s->cur ? ns_browser_link_under(s->cur, (int)x, (int)y)
                            : NULL;
        char *cursor = s->cur ? ns_browser_cursor_at(s->cur, (int)x, (int)y)
                              : NULL;
        char *href_escaped = json_escape(href ? href : "");
        char *cursor_escaped = json_escape(cursor ? cursor : "");
        char *json = NULL;
        int n = asprintf(&json,
                         "{\"changed\":%d,\"animating\":%d,"
                         "\"href\":\"%s\",\"cursor\":\"%s\"}",
                         changed > 0 ? 1 : 0,
                         s->cur && session_animating(s) ? 1 : 0,
                         href_escaped ? href_escaped : "",
                         cursor_escaped ? cursor_escaped : "");
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL,
                                json, (size_t)n);
        free(json);
        free(href_escaped);
        free(cursor_escaped);
        free(href);
        free(cursor);
        return 0;
    }

    if (strcmp(head->path, "/scroll") == 0) {
        long x = 0, y = 0, dx = 0, dy = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        json_get_long(body, "dx", &dx);
        json_get_long(body, "dy", &dy);
        int consumed = s->cur ? ns_browser_scroll_at(s->cur, (int)x, (int)y,
                                                      (int)dx, (int)dy) : 0;
        if (consumed)
            s->frame_valid = 0;
        char json[32];
        int n = snprintf(json, sizeof json, "{\"consumed\":%d}", consumed ? 1 : 0);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        return 0;
    }

    if (strcmp(head->path, "/scrollbar-press") == 0 ||
        strcmp(head->path, "/scrollbar-drag") == 0) {
        long x = 0, y = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        int hit = 0;
        if (s->cur) {
            hit = strcmp(head->path, "/scrollbar-press") == 0
                ? ns_browser_scrollbar_press(s->cur, (int)x, (int)y)
                : ns_browser_scrollbar_drag(s->cur, (int)x, (int)y);
        }
        if (hit)
            s->frame_valid = 0;
        char json[32];
        int n = snprintf(json, sizeof json, "{\"hit\":%d}", hit ? 1 : 0);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        return 0;
    }

    if (strcmp(head->path, "/scrollbar-release") == 0) {
        if (s->cur)
            ns_browser_scrollbar_release(s->cur);
        http_write_response(ctrl_w, 200, "application/json", NULL,
                            "{\"ok\":1}", 8);
        return 0;
    }

    if (strcmp(head->path, "/focused-editable") == 0) {
        char json[32];
        int active = s->cur ? ns_browser_focused_editable(s->cur) : 0;
        int n = snprintf(json, sizeof json, "{\"active\":%d}", active);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        return 0;
    }

    if (strcmp(head->path, "/focused-editable-state") == 0) {
        size_t caret = 0, anchor = 0;
        char *value = s->cur ? ns_browser_focused_editable_value(s->cur,
                                                                 &caret,
                                                                 &anchor)
                             : NULL;
        char *e = json_escape(value ? value : "");
        char *json = NULL;
        int n = asprintf(&json,
                         "{\"active\":%d,\"caret\":%zu,\"anchor\":%zu,"
                         "\"value\":\"%s\"}",
                         value ? 1 : 0, caret, anchor, e ? e : "");
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL,
                                json, (size_t)n);
        free(json);
        free(e);
        free(value);
        return 0;
    }

    if (strcmp(head->path, "/focused-editable-selection") == 0) {
        long caret = 0, anchor = 0;
        json_get_long(body, "caret", &caret);
        json_get_long(body, "anchor", &anchor);
        int ok = s->cur ? ns_browser_set_focused_editable_selection(
                              s->cur,
                              caret > 0 ? (size_t)caret : 0,
                              anchor > 0 ? (size_t)anchor : 0)
                        : 0;
        if (ok)
            s->frame_valid = 0;
        char json[16];
        int n = snprintf(json, sizeof json, "{\"ok\":%d}", ok ? 1 : 0);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        return 0;
    }

    if (strcmp(head->path, "/find") == 0) {
        long cs = 0, dir = 0, from_y = 0;
        json_get_long(body, "case_sensitive", &cs);
        json_get_long(body, "direction", &dir);
        json_get_long(body, "from_y", &from_y);
        char *q = json_get_str(body, "query");
        int total = 0, current = 0, scroll_y = 0;
        s->frame_valid = 0;
        if (s->cur)
            ns_browser_find(s->cur, q ? q : "", (int)cs, (int)dir,
                            (int)from_y, &total, &current, &scroll_y);
        char json[96];
        int n = snprintf(json, sizeof json,
                         "{\"total\":%d,\"current\":%d,\"scroll_y\":%d}",
                         total, current, scroll_y);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        free(q);
        return 0;
    }

    if (strcmp(head->path, "/download-start") == 0) {
        char *url = json_get_str(body, "url");
        long id = url && *url ? ns_download_start(url, url) : 0;
        char json[48];
        int n = snprintf(json, sizeof json, "{\"id\":%ld}", id);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        free(url);
        return 0;
    }

    if (strcmp(head->path, "/popup-poll") == 0) {
        char *json = ns_browser_popup_take();
        const char *out = json ? json : "[]";
        http_write_response(ctrl_w, 200, "application/json", NULL, out,
                            strlen(out));
        free(json);
        return 0;
    }

    if (strcmp(head->path, "/popup-deliver") == 0) {
        if (s->cur && body) {
            ns_browser_popup_deliver(s->cur, body);
            s->frame_valid = 0;
        }
        const char *json = "{\"ok\":1}";
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            strlen(json));
        return 0;
    }

    if (strcmp(head->path, "/download-poll") == 0) {
        long id = 0;
        json_get_long(body, "id", &id);
        ns_download_status st;
        gboolean known = ns_download_poll(id, &st);
        char *path_e = json_escape(known && st.path ? st.path : "");
        char *final_e = json_escape(known && st.final_url ? st.final_url : "");
        char *type_e = json_escape(known && st.content_type ? st.content_type
                                                            : "");
        char *error_e = json_escape(known ? st.error : "unknown download");
        char *json = NULL;
        int n = asprintf(&json,
                         "{\"known\":%d,\"state\":%d,\"bytes\":%llu,"
                         "\"total\":%llu,\"path\":\"%s\",\"final_url\":\"%s\","
                         "\"content_type\":\"%s\",\"error\":\"%s\"}",
                         known ? 1 : 0, known ? st.state : NS_DOWNLOAD_FAILED,
                         (unsigned long long)st.bytes,
                         (unsigned long long)st.total,
                         path_e ? path_e : "", final_e ? final_e : "",
                         type_e ? type_e : "", error_e ? error_e : "");
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL, json,
                                (size_t)n);
        free(json);
        free(path_e);
        free(final_e);
        free(type_e);
        free(error_e);
        if (known) ns_download_status_clear(&st);
        return 0;
    }

    if (strcmp(head->path, "/download-cancel") == 0) {
        long id = 0;
        json_get_long(body, "id", &id);
        ns_download_cancel(id);
        http_write_response(ctrl_w, 200, "application/json", NULL,
                            "{\"ok\":1}", 8);
        return 0;
    }

    if (strcmp(head->path, "/viewport") == 0) {
        long w = 0, h = 0;
        json_get_long(body, "width", &w);
        json_get_long(body, "height", &h);
        int vw = css_viewport_extent(w, s->max_w);
        int vh = css_viewport_extent(h, s->max_h);
        int pw = 0, ph = 0, ok = 0;
        s->frame_valid = 0;
        session_apply_device_pixel_ratio(s, body);
        if (s->cur && ns_browser_set_viewport(s->cur, vw, vh) == 0) {
            ns_browser_window_action_applied(s->cur);
            ns_browser_page_size(s->cur, &pw, &ph);
            ok = 1;
        }
        char json[96];
        int n = snprintf(json, sizeof json,
                         "{\"ok\":%d,\"page_width\":%d,\"page_height\":%d}",
                         ok, pw, ph);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        return 0;
    }

    if (strcmp(head->path, "/eval") == 0) {
        char *src = json_get_str(body, "src");
        s->frame_valid = 0;
        char *res = s->cur ? ns_browser_eval(s->cur, src ? src : "") : NULL;
        reply_str(ctrl_w, "text", res);
        free(res);
        free(src);
        return 0;
    }

    if (strcmp(head->path, "/dump") == 0) {
        char *kind = json_get_str(body, "kind");
        char *res = NULL;
        if (s->cur && kind) {
            if (strcmp(kind, "dom") == 0)
                res = ns_browser_dump_dom(s->cur);
            else if (strcmp(kind, "layout") == 0)
                res = ns_browser_dump_layout(s->cur);
            else if (strcmp(kind, "text") == 0)
                res = ns_browser_render_text(s->cur);
            else if (strcmp(kind, "links") == 0)
                res = ns_browser_links(s->cur);
            else if (strcmp(kind, "performance") == 0)
                res = ns_browser_dump_performance(s->cur);
        }
        if (!res && kind && strcmp(kind, "network") == 0)
            res = ns_net_log_dump();
        reply_str(ctrl_w, "text", res);
        free(res);
        free(kind);
        return 0;
    }

    if (strcmp(head->path, "/console") == 0) {
        char *log = s->cur ? ns_browser_console_drain(s->cur) : NULL;
        reply_str(ctrl_w, "text", log);
        free(log);
        return 0;
    }

    if (strcmp(head->path, "/audio-event") == 0) {
        char *line = json_get_str(body, "line");
        if (s->cur && line) ns_browser_audio_feedback(s->cur, line);
        s->frame_valid = 0;
        http_write_response(ctrl_w, 200, "text/plain", NULL, NULL, 0);
        free(line);
        return 0;
    }

    if (strcmp(head->path, "/media") == 0) {
        long x = 0, y = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        int is_video = 0, stream = 0;
        char *url = s->cur ? ns_browser_media_at(s->cur, (int)x, (int)y,
                                                 &is_video, &stream)
                           : NULL;
        char *e = json_escape(url ? url : "");
        char *json = NULL;
        int n = asprintf(&json,
                         "{\"url\":\"%s\",\"is_video\":%d,\"stream\":%d}",
                         e ? e : "", is_video, stream);
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL,
                                json, (size_t)n);
        free(json);
        free(e);
        free(url);
        return 0;
    }

    if (strcmp(head->path, "/contextmenu") == 0) {
        long x = 0, y = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        int edit = 0;
        int prevented = s->cur
            ? ns_browser_contextmenu_full(s->cur, (int)x, (int)y, &edit) : 0;
        if (edit) s->frame_valid = 0;
        char *json = NULL;
        int n = asprintf(&json, "{\"prevented\":%d,\"edit\":%d}", prevented,
                         edit);
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL,
                                json, (size_t)n);
        free(json);
        return 0;
    }

    if (strcmp(head->path, "/export") == 0) {
        char *path = json_get_str(body, "path");
        s->frame_valid = 0;
        int rc = (s->cur && path) ? ns_browser_render_image(s->cur, path)
                                  : -1;
        char json[24];
        int n = snprintf(json, sizeof json, "{\"ok\":%d}", rc);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        free(path);
        return 0;
    }

    if (strcmp(head->path, "/print") == 0) {
        char *prefix = json_get_str(body, "prefix");
        double scale = 0;
        json_get_double(body, "scale", &scale);
        if (!(scale > 0.1) || scale > 8)
            scale = 2.0;
        ns_print_setup setup;
        ns_print_setup_default(&setup);
        s->frame_valid = 0;
        GPtrArray *pages = prefix ? ns_renderer_session_print(s, &setup) : NULL;
        guint written = 0;
        if (pages) {
            int w = (int)ceil(setup.width * scale);
            int h = (int)ceil(setup.height * scale);
            for (guint i = 0; i < pages->len; i++) {
                cairo_surface_t *sheet = g_ptr_array_index(pages, i);
                if (written == i && w > 0 && h > 0) {
                    cairo_surface_t *img = cairo_image_surface_create(
                        CAIRO_FORMAT_RGB24, w, h);
                    cairo_t *cr = cairo_create(img);
                    cairo_set_source_rgb(cr, 1, 1, 1);
                    cairo_paint(cr);
                    cairo_scale(cr, scale, scale);
                    cairo_set_source_surface(cr, sheet, 0, 0);
                    cairo_paint(cr);
                    cairo_destroy(cr);
                    char *path = NULL;
                    if (asprintf(&path, "%s-%u.png", prefix, i) >= 0 && path) {
                        if (cairo_surface_write_to_png(img, path) ==
                            CAIRO_STATUS_SUCCESS)
                            written = i + 1;
                        free(path);
                    }
                    cairo_surface_destroy(img);
                }
                cairo_surface_destroy(sheet);
            }
            g_ptr_array_free(pages, TRUE);
        }
        char json[256];
        int jn = snprintf(json, sizeof json,
                          "{\"pages\":%u,\"scale\":%.6f,\"width\":%.6f,"
                          "\"height\":%.6f,\"mt\":%.6f,\"mr\":%.6f,"
                          "\"mb\":%.6f,\"ml\":%.6f}",
                          written, scale, setup.width, setup.height,
                          setup.margin_top, setup.margin_right,
                          setup.margin_bottom, setup.margin_left);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)jn);
        free(prefix);
        return 0;
    }

    http_write_response(ctrl_w, 404, "text/plain", NULL, NULL, 0);
    return 0;
}
