/* Nordstjernen — GTK thin client over the out-of-process renderer (rproc). */

#include "procview.h"
#include "i18n.h"
#include "pagelayers.h"

#include "libnordstjernen.h"
#include "proc_limits.h"
#include "../print.h"
#include "rproc_http.h"
#include "rproc_inproc.h"
#include "net.h"

#include <cairo.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <windows.h>
#endif

#define NS_PROC_CARET_BLINK_US (530 * 1000)
#define NS_PROC_FRAME_WAKE_MS  16
#define NS_PROC_AUDIO_POLL_MS  250
#define NS_PROC_ANIM_FRAME_US  (G_USEC_PER_SEC / 60)
#define NS_PROC_ANIM_FRAME_SLACK_US (NS_PROC_ANIM_FRAME_US / 4)
#define NS_PROC_RELOAD_RESTART_US (1000 * 1000)
#define NS_PV_WHEEL_STEP_PX    100.0
#define NS_PV_WHEEL_TAU_MS     45.0
#define NS_PV_FLING_TAU_MS     400.0
#define NS_PV_FLING_STOP_PX_S  20.0
#define NS_PV_HOVER_AFTER_SCROLL_MS 150
#define NS_PV_TILE_H           256
#define NS_PROC_HELPER_LINE_MAX 4096

#ifdef __APPLE__
#define NS_PROC_PRIMARY_MASK (GDK_CONTROL_MASK | GDK_META_MASK)
#else
#define NS_PROC_PRIMARY_MASK GDK_CONTROL_MASK
#endif

#ifndef G_OS_WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static int
pv_settle_ms(void)
{
    const char *e = g_getenv(NS_PROC_SETTLE_ENV);
    if (e && *e) {
        int v = atoi(e);
        if (v >= 0 && v <= 10000)
            return v;
    }
    return NS_PROC_SETTLE_MS;
}

typedef enum {
    REQ_LOAD, REQ_RENDER, REQ_LINK, REQ_CLICK, REQ_VIEWPORT, REQ_KEY,
    REQ_SELECT, REQ_HOVER, REQ_RELEASE, REQ_FIND, REQ_EXPORT, REQ_CONSOLE,
    REQ_EVAL, REQ_DUMP, REQ_DROPFILES, REQ_SCROLLBAR,
    REQ_WEBGL, REQ_CAMERA, REQ_COLOR_SCHEME, REQ_FAVICON,
    REQ_TICK, REQ_PRINT, REQ_AUDIO_EVENT, REQ_DOWNLOAD, REQ_POPUP, REQ_QUIT
} ReqType;
typedef enum { ACT_HOVER, ACT_NAVIGATE, ACT_CONTEXT } LinkAct;

enum {
    DEV_TAB_CONSOLE = 0,
    DEV_TAB_NETWORK,
    DEV_TAB_PERFORMANCE,
    DEV_TAB_LAYOUT,
    DEV_TAB_ELEMENTS
};

typedef struct {
    ReqType type;
    int     seq;
    char   *url;
    int     vw;
    int     vh;
    int     w, h, sx, sy;
    double  scale;
    double  dpr;
    double  raster;
    ns_rproc_http_wheel wheel;
    int     x, y;
    int     dx, dy;
    int     mods;
    int     button;
    int     buttons;
    int     clicks;
    int     kind;
    int     keycode;
    int     fallback_scroll;
    double  fallback_x;
    double  fallback_y;
    char   *key;
    char   *code;
    LinkAct action;
    char   *query;
    int     find_dir;
    int     find_from_y;
    int     find_case;
    char   *export_dest;
    char   *paths;
    int     dump_tab;
    gboolean inspect;
    gboolean history;
    gboolean user_activated;
    gboolean reload;
    gboolean caret_active;
    ns_rproc_http_tiles_req tiles;
    char   *have;
    char   *hold;
    NsProcViewTextFn remote_cb;
    gpointer remote_ud;
    long    download_id;
} Req;

typedef enum {
    RES_PAGE, RES_FRAME, RES_LINK, RES_CLICK, RES_VIEWPORT, RES_KEY,
    RES_SELECT, RES_COPY, RES_HOVER, RES_RELEASE, RES_FIND, RES_EXPORT,
    RES_CONSOLE, RES_EVAL, RES_DUMP, RES_FAVICON, RES_DROPFILES,
    RES_SCROLLBAR, RES_TICK, RES_PRINT, RES_DOWNLOAD, RES_POPUP
} ResType;

typedef struct {
    NsProcView      *view;
    ResType          type;
    int              seq;
    gboolean         ok;
    int              pw, ph;
    char            *title;
    char            *url;
    gboolean         url_pushed;
    char            *nav;
    int              security;
    char            *remote_ip;
    char            *webgl;
    char            *camera;
    char            *download;
    char            *config;
    char            *audio;
    char            *window_action;
    gboolean         popup;
    char            *clipboard;
    GdkTexture      *texture;
    double           texture_scale;
    NsLayerUpdate   *layers;
    char            *href;
    char            *cursor;
    LinkAct          action;
    int              kind;
    int              prevented;
    int              newtab;
    int              edit;
    int              fallback_scroll;
    double           fallback_x;
    double           fallback_y;
    gboolean         animating;
    int              wake_ms;
    gboolean         caret_blinking;
    gboolean         wheel_snapped;
    gboolean         wheel_answered;
    gboolean         wheel_prevented;
    gboolean         frame_unchanged;
    int              requested_scroll_y, requested_scroll_x;
    int              find_total, find_current, find_scroll_y;
    char            *media_url;
    int              media_is_video, media_stream;
    unsigned char   *favicon_data;
    int              favicon_w, favicon_h, favicon_stride;
    int              dump_tab;
    gboolean         inspect;
    GPtrArray       *print_pages;
    ns_print_setup   print_setup;
    double           print_scale;
    NsProcViewTextFn remote_cb;
    gpointer         remote_ud;
} Res;

struct NsProcView {
    char            *opener_token;
    char            *opener_origin;
    gboolean         popup_poll_pending;
    grefcount   rc;

    GtkWidget     *root;
    GtkWidget     *area;
    GtkWidget     *hscroll;
    GtkWidget     *vscroll;
    GtkIMContext  *im;
    GtkAdjustment *hadj;
    GtkAdjustment *vadj;
    gboolean    closed;

    GThread    *thread;
    GAsyncQueue *queue;
    ns_rproc_http *proc;
    GMutex      proc_lock;
    gint64      busy_since_us;
    ReqType     busy_type;
    char       *renderer_path;
    gboolean    private_mode;

    GSubprocess  *audio_proc;
    GOutputStream *audio_in;
    guint         audio_poll_id;

    NsProcNotify notify;
    gpointer     notify_ud;

    char       *current_url;
    char       *pending_url;
    char       *current_title;
    int         security;
    char       *remote_ip;
    int         page_w, page_h;
    int         scroll_x, scroll_y;
    gboolean    opened;

    GdkTexture      *frame;
    double           frame_scale;
    NsPageLayers    *layers;
    gboolean         tiles_mode;
    gboolean         tiles_fill;
    gboolean         wheel_remote;
    int              scroll_dir;
    gulong           surface_scale_handler;
    GdkSurface      *scale_surface;

    GdkPaintable    *favicon;

    gboolean    render_inflight;
    gboolean    render_pending;
    gboolean    tick_inflight;
    gboolean    tick_pending;
    int         render_restarts;

    gboolean    link_inflight;
    gboolean    link_pending;
    int         link_pending_x, link_pending_y;
    LinkAct     link_pending_action;

    gboolean    hover_inflight;
    gboolean    hover_pending;
    int         hover_pending_x, hover_pending_y;

    gboolean    has_selection;
    gboolean    multi_click;
    double      ctx_x, ctx_y;
    char       *ctx_link;
    GtkWidget  *ctx_popover;
    GSimpleActionGroup *ctx_actions;

    GtkWidget  *search_revealer;
    GtkWidget  *search_entry;
    GtkWidget  *search_label;
    int         find_seq;
    gboolean    find_case;

    GtkWidget  *overlay;
    GtkWidget  *perm_revealer;
    GtkWidget  *perm_label;
    ReqType     perm_kind;
    gboolean    perm_pending;
    char       *perm_origin;

    GtkWidget    *console_window;
    GtkWidget    *console_notebook;
    GtkWidget    *console_entry;
    GtkWidget    *console_view;
    GtkTextBuffer *console_buffer;
    GtkWidget    *net_view;
    GtkTextBuffer *net_buffer;
    GtkWidget    *perf_view;
    GtkTextBuffer *perf_buffer;
    GtkWidget    *layout_view;
    GtkTextBuffer *layout_buffer;
    GtkWidget    *elements_view;
    GtkTextBuffer *elements_buffer;
    GtkWidget    *inspect_entry;
    gboolean      console_open;
    guint         console_poll_id;

    GPtrArray  *history;
    int         hist_index;
    gboolean    pending_record;

    char       *deferred_url;
    gboolean    deferred_record;
    gboolean    deferred_history;
    gboolean    deferred_user_activated;

    int         js_redirects;

    double      scale;
    gboolean    loading;
    gboolean    busy_cursor;
    GdkCursor  *hourglass_cursor;

    int         load_seq, render_seq, link_seq, click_seq, viewport_seq;
    int         key_seq, select_seq, hover_seq;
    int         last_vp_w, last_vp_h;
    double      last_vp_dpr;
    double      wheel_left_x, wheel_left_y;
    gboolean    wheel_viewport;
    double      wheel_pend_x, wheel_pend_y;
    double      wev_dx, wev_dy;
    int         wev_x, wev_y, wev_mods;
    double      wev_hold_x, wev_hold_y;
    double      wev_sent_x, wev_sent_y;
    double      fling_vx, fling_vy;
    guint       wheel_tick_id;
    gint64      wheel_last_us;
    gboolean    adopting_scroll;
    guint       hover_after_scroll_id;
    double      drag_start_x, drag_start_y;
    double      pointer_x, pointer_y;
    int         pointer_mods;
    int         pointer_buttons;
    int         buttons_pressed;
    gboolean    drag_anchored;
    gboolean    sb_probe, sb_dragging, sb_have_last;
    double      sb_last_x, sb_last_y;

    guint       anim_tick_id;
    gint64      last_anim_frame_us;
    guint       anim_timeout_id;
    gint64      page_due_us;
    gint64      caret_due_us;
    gboolean    page_animating;
    int         page_wake_ms;
    gboolean    caret_blinking;
};

enum {
    NS_PV_ZOOM_MIN_PERMILLE = (int)(NS_PROC_ZOOM_MIN * 1000.0 + 0.5),
    NS_PV_ZOOM_MAX_PERMILLE = (int)(NS_PROC_ZOOM_MAX * 1000.0 + 0.5)
};

static NsProcView *pv_ref(NsProcView *v) { g_ref_count_inc(&v->rc); return v; }

static void
set_accessible_label(GtkWidget *w, const char *label)
{
    gtk_accessible_update_property(GTK_ACCESSIBLE(w),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static void
hourglass_bulbs_path(cairo_t *cr, double ox, double oy)
{
    cairo_move_to(cr, 14.6 + ox, 16.0 + oy);
    cairo_curve_to(cr, 12.0 + ox, 13.2 + oy, 9.9 + ox, 10.6 + oy, 10.1 + ox, 8.0 + oy);
    cairo_curve_to(cr, 10.0 + ox, 6.8 + oy, 10.8 + ox, 6.1 + oy, 12.2 + ox, 6.1 + oy);
    cairo_curve_to(cr, 14.2 + ox, 5.6 + oy, 17.8 + ox, 5.6 + oy, 19.8 + ox, 6.1 + oy);
    cairo_curve_to(cr, 21.2 + ox, 6.1 + oy, 22.0 + ox, 6.8 + oy, 21.9 + ox, 8.0 + oy);
    cairo_curve_to(cr, 22.1 + ox, 10.6 + oy, 20.0 + ox, 13.2 + oy, 17.4 + ox, 16.0 + oy);
    cairo_close_path(cr);
    cairo_move_to(cr, 14.6 + ox, 16.0 + oy);
    cairo_curve_to(cr, 12.0 + ox, 18.8 + oy, 9.9 + ox, 21.4 + oy, 10.1 + ox, 24.0 + oy);
    cairo_curve_to(cr, 10.0 + ox, 25.2 + oy, 10.8 + ox, 25.9 + oy, 12.2 + ox, 25.9 + oy);
    cairo_curve_to(cr, 14.2 + ox, 26.4 + oy, 17.8 + ox, 26.4 + oy, 19.8 + ox, 25.9 + oy);
    cairo_curve_to(cr, 21.2 + ox, 25.9 + oy, 22.0 + ox, 25.2 + oy, 21.9 + ox, 24.0 + oy);
    cairo_curve_to(cr, 22.1 + ox, 21.4 + oy, 20.0 + ox, 18.8 + oy, 17.4 + ox, 16.0 + oy);
    cairo_close_path(cr);
}

static void
hourglass_caps_path(cairo_t *cr, double ox, double oy)
{
    cairo_move_to(cr, 9.7 + ox, 6.2 + oy);
    cairo_line_to(cr, 22.3 + ox, 6.2 + oy);
    cairo_move_to(cr, 9.7 + ox, 25.8 + oy);
    cairo_line_to(cr, 22.3 + ox, 25.8 + oy);
}

static GdkTexture *
hourglass_texture(void)
{
    const int size = 32;
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.22);
    cairo_set_line_width(cr, 3.0);
    hourglass_caps_path(cr, 0.7, 0.9);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 2.6);
    hourglass_bulbs_path(cr, 0.7, 0.9);
    cairo_stroke(cr);

    cairo_pattern_t *glass = cairo_pattern_create_linear(16.0, 7.0, 16.0, 26.0);
    cairo_pattern_add_color_stop_rgba(glass, 0.0, 0.97, 1.0, 1.0, 0.55);
    cairo_pattern_add_color_stop_rgba(glass, 0.5, 0.62, 0.80, 0.95, 0.30);
    cairo_pattern_add_color_stop_rgba(glass, 1.0, 0.93, 0.98, 1.0, 0.52);
    cairo_set_source(cr, glass);
    hourglass_bulbs_path(cr, 0.0, 0.0);
    cairo_fill(cr);
    cairo_pattern_destroy(glass);

    cairo_pattern_t *sand = cairo_pattern_create_linear(16.0, 7.0, 16.0, 25.0);
    cairo_pattern_add_color_stop_rgba(sand, 0.0, 1.0, 0.85, 0.40, 0.98);
    cairo_pattern_add_color_stop_rgba(sand, 1.0, 0.90, 0.52, 0.14, 0.98);
    cairo_set_source(cr, sand);
    cairo_move_to(cr, 11.7, 8.0);
    cairo_curve_to(cr, 14.0, 7.6, 18.0, 7.6, 20.3, 8.0);
    cairo_curve_to(cr, 18.9, 10.9, 17.4, 13.3, 16.0, 15.3);
    cairo_curve_to(cr, 14.6, 13.3, 13.1, 10.9, 11.7, 8.0);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_move_to(cr, 11.7, 24.0);
    cairo_curve_to(cr, 13.5, 21.6, 14.8, 20.9, 16.0, 20.9);
    cairo_curve_to(cr, 17.2, 20.9, 18.5, 21.6, 20.3, 24.0);
    cairo_curve_to(cr, 17.6, 24.7, 14.4, 24.7, 11.7, 24.0);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.1);
    cairo_move_to(cr, 16.0, 15.3);
    cairo_line_to(cr, 16.0, 20.9);
    cairo_stroke(cr);
    cairo_pattern_destroy(sand);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.80);
    cairo_set_line_width(cr, 0.9);
    cairo_move_to(cr, 12.6, 8.2);
    cairo_curve_to(cr, 13.7, 10.4, 14.6, 12.2, 15.3, 13.8);
    cairo_stroke(cr);
    cairo_move_to(cr, 16.7, 18.4);
    cairo_curve_to(cr, 17.6, 20.2, 18.7, 22.2, 19.6, 24.0);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.10, 0.12, 0.14, 0.98);
    cairo_set_line_width(cr, 2.0);
    hourglass_caps_path(cr, 0.0, 0.0);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1.5);
    hourglass_bulbs_path(cr, 0.0, 0.0);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.74, 0.82, 0.90, 0.85);
    cairo_set_line_width(cr, 0.6);
    hourglass_bulbs_path(cr, 0.0, 0.0);
    cairo_stroke(cr);

    cairo_destroy(cr);
    cairo_surface_flush(surface);

    int stride = cairo_image_surface_get_stride(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    GBytes *bytes = g_bytes_new(data, (gsize)stride * (gsize)size);
    GdkTexture *texture =
        gdk_memory_texture_new(size, size, GDK_MEMORY_DEFAULT, bytes,
                               (gsize)stride);
    g_bytes_unref(bytes);
    cairo_surface_destroy(surface);
    return texture;
}

static GdkCursor *
busy_hourglass_cursor(NsProcView *v)
{
    if (v->hourglass_cursor)
        return v->hourglass_cursor;

    GdkTexture *texture = hourglass_texture();
    if (texture) {
        GdkCursor *fallback = gdk_cursor_new_from_name("wait", NULL);
        v->hourglass_cursor =
            gdk_cursor_new_from_texture(texture, 16, 16, fallback);
        if (fallback)
            g_object_unref(fallback);
        g_object_unref(texture);
    }
    if (!v->hourglass_cursor)
        v->hourglass_cursor = gdk_cursor_new_from_name("wait", NULL);
    return v->hourglass_cursor;
}

static void
pv_set_named_cursor(GtkWidget *w, const char *name)
{
    if (!name || !*name) {
        gtk_widget_set_cursor(w, NULL);
        return;
    }
    static const struct { const char *name; const char *fallback; } fb[] = {
        { "context-menu",  "default" },
        { "help",          "default" },
        { "progress",      "wait" },
        { "cell",          "crosshair" },
        { "vertical-text", "text" },
        { "alias",         "copy" },
        { "copy",          "default" },
        { "move",          "all-scroll" },
        { "no-drop",       "not-allowed" },
        { "not-allowed",   "default" },
        { "grab",          "all-scroll" },
        { "grabbing",      "all-scroll" },
        { "all-scroll",    "move" },
        { "col-resize",    "ew-resize" },
        { "row-resize",    "ns-resize" },
        { "ne-resize",     "nesw-resize" },
        { "sw-resize",     "nesw-resize" },
        { "nw-resize",     "nwse-resize" },
        { "se-resize",     "nwse-resize" },
        { "nesw-resize",   "crosshair" },
        { "nwse-resize",   "crosshair" },
        { "zoom-in",       "crosshair" },
        { "zoom-out",      "crosshair" },
    };
    static const char *const plain[] = {
        "default", "none", "pointer", "wait", "crosshair", "text",
        "n-resize", "e-resize", "s-resize", "w-resize", "ew-resize",
        "ns-resize",
    };
    const char *fallback = NULL;
    gboolean known = FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(fb); i++)
        if (strcmp(name, fb[i].name) == 0) {
            fallback = fb[i].fallback;
            known = TRUE;
            break;
        }
    for (gsize i = 0; i < G_N_ELEMENTS(plain) && !known; i++)
        known = strcmp(name, plain[i]) == 0;
    if (!known) {
        gtk_widget_set_cursor(w, NULL);
        return;
    }
    GdkCursor *fb_cur = fallback ? gdk_cursor_new_from_name(fallback, NULL) : NULL;
    GdkCursor *cur = gdk_cursor_new_from_name(name, fb_cur);
    gtk_widget_set_cursor(w, cur ? cur : fb_cur);
    if (cur)
        g_object_unref(cur);
    if (fb_cur)
        g_object_unref(fb_cur);
}

static void
set_busy_cursor(NsProcView *v)
{
    v->busy_cursor = TRUE;
    if (!v->area)
        return;

    GdkCursor *cursor = busy_hourglass_cursor(v);
    if (cursor)
        gtk_widget_set_cursor(v->area, cursor);
    else
        gtk_widget_set_cursor_from_name(v->area, "wait");
}

static void pv_audio_shutdown(NsProcView *v);

static void
pv_free(NsProcView *v)
{
    pv_audio_shutdown(v);
    if (v->queue) {
        Req *r;
        while ((r = g_async_queue_try_pop(v->queue))) {
            g_free(r->url);
            g_free(r->key);
            g_free(r->code);
            g_free(r->query);
            g_free(r->export_dest);
            g_free(r->paths);
            g_free(r->have);
            g_free(r->hold);
            g_free(r);
        }
        g_async_queue_unref(v->queue);
    }
    g_clear_object(&v->frame);
    ns_page_layers_free(v->layers);
    if (v->favicon)
        g_object_unref(v->favicon);
    v->favicon = NULL;
    if (v->ctx_popover)
        gtk_widget_unparent(v->ctx_popover);
    if (v->ctx_actions)
        g_object_unref(v->ctx_actions);
    g_free(v->ctx_link);
    if (v->history)
        g_ptr_array_unref(v->history);
    g_free(v->renderer_path);
    g_free(v->opener_token);
    g_free(v->opener_origin);
    g_free(v->current_url);
    g_free(v->pending_url);
    g_free(v->current_title);
    g_free(v->remote_ip);
    g_free(v->deferred_url);
    g_free(v->perm_origin);
    if (v->hourglass_cursor)
        g_object_unref(v->hourglass_cursor);
    g_mutex_clear(&v->proc_lock);
    g_free(v);
}

static void pv_unref(NsProcView *v) { if (g_ref_count_dec(&v->rc)) pv_free(v); }

static ns_rproc_http *
pv_swap_proc(NsProcView *v, ns_rproc_http *newp)
{
    if (newp)
        ns_rproc_http_set_opener(newp, v->opener_token, v->opener_origin);
    g_mutex_lock(&v->proc_lock);
    ns_rproc_http *old = v->proc;
    v->proc = newp;
    g_mutex_unlock(&v->proc_lock);
    return old;
}

static char *
ns_proc_sibling_exe_path(const char *base)
{
#ifdef G_OS_WIN32
    char *name = g_strconcat(base, ".exe", NULL);
#else
    char *name = g_strdup(base);
#endif
    const char *exe = ns_app_self_exe();
    if (!exe) return name;
    char *dir = g_path_get_dirname(exe);
    char *cand = g_build_filename(dir, name, NULL);
    if (!g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE)) {
        g_free(cand);
        cand = g_build_filename(dir, "..", name, NULL);
        if (!g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE))
            g_clear_pointer(&cand, g_free);
    }
    g_free(dir);
    if (!cand) return name;
    g_free(name);
    return cand;
}

char *
ns_proc_renderer_path(void)
{
    const char *env = g_getenv(NS_PROC_RENDERER_ENV);
    if (env && *env)
        return g_strdup(env);
    return ns_proc_sibling_exe_path(NS_PROC_RENDERER_NAME);
}

static void
ns_proc_audio_apply_proxy_env(GSubprocessLauncher *launcher)
{
    const char *override = ns_net_proxy_override();
    if (override && *override) {
        g_subprocess_launcher_setenv(launcher, "all_proxy", override, TRUE);
        g_subprocess_launcher_setenv(launcher, "http_proxy", override, TRUE);
        g_subprocess_launcher_setenv(launcher, "https_proxy", override, TRUE);
    } else {
        const char *http_proxy = ns_net_http_proxy();
        const char *https_proxy = ns_net_https_proxy();
        if (http_proxy && *http_proxy)
            g_subprocess_launcher_setenv(launcher, "http_proxy", http_proxy, TRUE);
        if (https_proxy && *https_proxy)
            g_subprocess_launcher_setenv(launcher, "https_proxy", https_proxy, TRUE);
    }
    const char *no_proxy = ns_net_no_proxy();
    if (no_proxy && *no_proxy)
        g_subprocess_launcher_setenv(launcher, "no_proxy", no_proxy, TRUE);
    const char *ca = ns_net_ca_bundle_path();
    if (ca && *ca)
        g_subprocess_launcher_setenv(launcher, "CURL_CA_BUNDLE", ca, TRUE);
}

static void request_tick(NsProcView *v);

static gboolean
pv_audio_feedback_for_page(const char *line)
{
    static const char *const kinds[] = {
        "meta ", "pos ", "playing ", "ended ", "error ",
    };
    for (guint i = 0; i < G_N_ELEMENTS(kinds); i++)
        if (g_str_has_prefix(line, kinds[i])) return TRUE;
    return FALSE;
}

static void
pv_audio_feedback_line(GObject *src, GAsyncResult *res, gpointer user_data)
{
    NsProcView *v = user_data;
    GDataInputStream *in = G_DATA_INPUT_STREAM(src);
    char *line = g_data_input_stream_read_line_finish(in, res, NULL, NULL);
    if (!line) {
        g_object_unref(in);
        if (v) pv_unref(v);
        return;
    }
    if (g_str_has_prefix(line, "error ") || g_getenv("NS_DBG_AUDIO"))
        g_printerr("[audio-helper] %s\n", line);
    if (v && !v->closed && pv_audio_feedback_for_page(line)) {
        Req *req = g_new0(Req, 1);
        req->type = REQ_AUDIO_EVENT;
        req->query = line;
        line = NULL;
        request_tick(v);
        g_async_queue_push_front(v->queue, req);
    }
    g_free(line);
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL,
                                        pv_audio_feedback_line, v);
}

static char *
pv_audio_url_path(const char *url)
{
    char *path = g_filename_from_uri(url, NULL, NULL);
    if (path) return path;
    const char *p = url + strlen("file://");
    if (g_str_has_prefix(p, "localhost")) p += strlen("localhost");
    return g_strdup(p);
}

static gboolean
pv_stream_url_allowed(const char *url, const char *subdir)
{
    if (!g_str_has_prefix(url, "file://")) return FALSE;
    char *path = pv_audio_url_path(url);
    char *canon = g_canonicalize_filename(path, NULL);
    char *streams = g_build_filename(g_get_user_cache_dir(), "nordstjernen",
                                     subdir, "", NULL);
    g_strdelimit(canon, "\\", '/');
    g_strdelimit(streams, "\\", '/');
    gboolean inside = g_str_has_prefix(canon, streams);
    g_free(streams);
    g_free(canon);
    g_free(path);
    return inside;
}

static gboolean
pv_audio_url_allowed(NsProcView *v, const char *url)
{
    if (!g_str_has_prefix(url, "file://")) return FALSE;
    if (v->current_url && g_str_has_prefix(v->current_url, "file:"))
        return TRUE;
    return pv_stream_url_allowed(url, "msaudio");
}

static gboolean
pv_helper_is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static const char *
pv_helper_open_url(const char *cmd)
{
    const char *s = cmd;
    while (pv_helper_is_blank(*s)) s++;
    const char *op = s;
    while (*s && !pv_helper_is_blank(*s)) s++;
    gsize op_len = (gsize)(s - op);
    if (!(op_len == 4 && strncmp(op, "open", 4) == 0) &&
        !(op_len == 6 && strncmp(op, "reload", 6) == 0))
        return NULL;
    while (pv_helper_is_blank(*s)) s++;
    while (*s && !pv_helper_is_blank(*s)) s++;
    if (*s) s++;
    while (*s == ' ') s++;
    return s;
}

static gboolean
pv_helper_line_ok(const char *cmd)
{
    return strlen(cmd) < NS_PROC_HELPER_LINE_MAX - 1 && !strpbrk(cmd, "\r\n");
}

static gboolean
pv_audio_command_allowed(NsProcView *v, const char *cmd)
{
    if (!pv_helper_line_ok(cmd)) return FALSE;
    const char *url = pv_helper_open_url(cmd);
    return !url || pv_audio_url_allowed(v, url);
}

static gboolean
pv_audio_poll(gpointer data)
{
    NsProcView *v = data;
    if (!v->audio_in) {
        v->audio_poll_id = 0;
        return G_SOURCE_REMOVE;
    }
    g_output_stream_write_all(v->audio_in, "poll\n", 5, NULL, NULL, NULL);
    g_output_stream_flush(v->audio_in, NULL, NULL);
    return G_SOURCE_CONTINUE;
}

static void
pv_audio_pump(NsProcView *v, const char *commands)
{
    if (!commands || !*commands) return;
    if (!v->audio_proc) {
        char *path = ns_proc_sibling_exe_path("nordstjernen-audio");
        GError *err = NULL;
        GSubprocessLauncher *launcher = g_subprocess_launcher_new(
            G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_SILENCE);
        ns_proc_audio_apply_proxy_env(launcher);
        v->audio_proc = g_subprocess_launcher_spawn(launcher, &err, path, NULL);
        g_object_unref(launcher);
        if (!v->audio_proc) {
            g_printerr("nordstjernen: audio helper %s failed to start: %s\n",
                       path, err ? err->message : "unknown error");
            g_free(path);
            g_clear_error(&err);
            return;
        }
        if (g_getenv("NS_DBG_AUDIO"))
            g_printerr("[audio-pump] spawn %s -> ok\n", path);
        g_free(path);
        v->audio_in = g_subprocess_get_stdin_pipe(v->audio_proc);
        GInputStream *feedback = g_subprocess_get_stdout_pipe(v->audio_proc);
        if (feedback)
            g_data_input_stream_read_line_async(
                g_data_input_stream_new(feedback), G_PRIORITY_DEFAULT, NULL,
                pv_audio_feedback_line, pv_ref(v));
        if (!v->audio_poll_id)
            v->audio_poll_id = g_timeout_add(NS_PROC_AUDIO_POLL_MS,
                                             pv_audio_poll, v);
    }
    if (!v->audio_in) return;

    char **lines = g_strsplit(commands, "\x1f", -1);
    for (int i = 0; lines[i]; i++) {
        if (!*lines[i]) continue;
        if (!pv_audio_command_allowed(v, lines[i])) {
            if (g_getenv("NS_DBG_AUDIO"))
                g_printerr("[audio-pump] refused: %s\n", lines[i]);
            continue;
        }
        char *line = g_strconcat(lines[i], "\n", NULL);
        if (g_getenv("NS_DBG_AUDIO"))
            g_printerr("[audio-pump] cmd: %s", line);
        g_output_stream_write_all(v->audio_in, line, strlen(line),
                                  NULL, NULL, NULL);
        g_free(line);
    }
    g_output_stream_flush(v->audio_in, NULL, NULL);
    g_strfreev(lines);
}

static void
pv_audio_shutdown(NsProcView *v)
{
    if (v->audio_poll_id) {
        g_source_remove(v->audio_poll_id);
        v->audio_poll_id = 0;
    }
    if (!v->audio_proc) return;
    if (v->audio_in) {
        g_output_stream_write_all(v->audio_in, "quit\n", 5, NULL, NULL, NULL);
        g_output_stream_flush(v->audio_in, NULL, NULL);
    }
    g_subprocess_force_exit(v->audio_proc);
    g_clear_object(&v->audio_proc);
    v->audio_in = NULL;
}

static void request_render(NsProcView *v);
static gboolean maybe_update_viewport(NsProcView *v);
static void request_hover(NsProcView *v, int x, int y);
static void print_run(NsProcView *v, GPtrArray *pages,
                      const ns_print_setup *setup, double raster_scale);

static void
pv_append_proc_threads(GString *out, int pid)
{
#ifdef __linux__
    char *taskdir = g_strdup_printf("/proc/%d/task", pid);
    GDir *dir = g_dir_open(taskdir, 0, NULL);
    if (dir) {
        long hz = sysconf(_SC_CLK_TCK);
        if (hz <= 0) hz = 100;
        const char *tid;
        while ((tid = g_dir_read_name(dir))) {
            char *stat_path = g_strdup_printf("%s/%s/stat", taskdir, tid);
            char *stat = NULL;
            if (g_file_get_contents(stat_path, &stat, NULL, NULL)) {
                char *close = strrchr(stat, ')');
                char *open = strchr(stat, '(');
                if (open && close && close > open) {
                    char name[32] = {0};
                    g_strlcpy(name, open + 1,
                              MIN((gsize)(close - open), sizeof name));
                    char state = 0;
                    unsigned long utime = 0, stime = 0;
                    if (sscanf(close + 1,
                               " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                               "%lu %lu", &state, &utime, &stime) >= 1)
                        g_string_append_printf(
                            out, "    tid %-7s %-16s %c  cpu %.2fs\n",
                            tid, name, state,
                            (double)(utime + stime) / (double)hz);
                }
                g_free(stat);
            }
            g_free(stat_path);
        }
        g_dir_close(dir);
    }
    g_free(taskdir);
#else
    (void)out; (void)pid;
#endif
}

static void
pv_append_media_process_stats(NsProcView *v, GString *out)
{
    int pid = ns_proc_view_audio_pid(v);
    if (pid <= 0) return;
    char state[32] = "";
    long rss = -1;
    ns_rproc_http_proc_info(pid, state, sizeof state, &rss);
    g_string_append_printf(out, "\n\n== Media helper processes ==\n"
                           "audio helper (nordstjernen-audio)  pid %d  %s  "
                           "rss %.1f MB\n", pid, state,
                           rss >= 0 ? rss / 1024.0 : 0.0);
    pv_append_proc_threads(out, pid);
}

static GdkTexture *
stage_fill(NsProcView *v, const unsigned char *px, int w, int h, int stride)
{
    (void)v;
    size_t row = (size_t)w * 4u;
    if (w <= 0 || h <= 0 || (size_t)stride < row)
        return NULL;
    GBytes *bytes;
    if ((size_t)stride == row) {
        bytes = g_bytes_new(px, row * (size_t)h);
    } else {
        unsigned char *dst = g_malloc(row * (size_t)h);
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y * row, px + (size_t)y * stride, row);
        bytes = g_bytes_new_take(dst, row * (size_t)h);
    }
    GdkTexture *tex = gdk_memory_texture_new(w, h, GDK_MEMORY_DEFAULT, bytes,
                                             row);
    g_bytes_unref(bytes);
    return tex;
}

static void
post_emit(NsProcView *v, NsProcEvent evt, const char *text)
{
    if (v->notify)
        v->notify(v, evt, text, v->notify_ud);
}

static void
clear_busy_cursor(NsProcView *v)
{
    if (!v->busy_cursor)
        return;
    v->busy_cursor = FALSE;
    if (v->area)
        gtk_widget_set_cursor_from_name(v->area, NULL);
}

static void
finish_loading(NsProcView *v)
{
    if (v->loading) {
        v->loading = FALSE;
        post_emit(v, NS_PROC_EVT_LOADING, "0");
    }
}

void
ns_proc_view_stop(NsProcView *v)
{
    if (!v)
        return;
    v->render_seq++;
    v->render_inflight = FALSE;
    v->render_pending = FALSE;
    finish_loading(v);
    clear_busy_cursor(v);
}

static gboolean on_result(gpointer data);

static void
post(Res *res)
{
    g_idle_add(on_result, res);
}

static void
req_free(Req *req)
{
    g_free(req->url);
    g_free(req->key);
    g_free(req->code);
    g_free(req->query);
    g_free(req->export_dest);
    g_free(req->paths);
    g_free(req->have);
    g_free(req->hold);
    g_free(req);
}

static gboolean
pv_load_superseded(NsProcView *v, const Req *req)
{
    return req->seq != g_atomic_int_get(&v->load_seq);
}

static void
pv_note_busy(NsProcView *v, const Req *req)
{
    g_mutex_lock(&v->proc_lock);
    v->busy_since_us = req ? g_get_monotonic_time() : 0;
    v->busy_type = req ? req->type : REQ_QUIT;
    g_mutex_unlock(&v->proc_lock);
}

static int
pv_open(NsProcView *v, const Req *req, int settle, ns_rproc_http_page *pg)
{
    if (!v->proc)
        return -1;
    if (req->reload)
        return ns_rproc_http_reload(v->proc, req->url, req->vw, req->vh,
                                    settle, pg);
    return ns_rproc_http_open_ex(v->proc, req->url, req->vw, req->vh, settle,
                                 req->history, req->user_activated, pg);
}

static gpointer
worker_main(gpointer data)
{
    NsProcView *v = data;
    for (;;) {
        Req *req = g_async_queue_pop(v->queue);
        if (req->type == REQ_QUIT) {
            req_free(req);
            break;
        }
        if (req->type == REQ_LOAD && pv_load_superseded(v, req)) {
            req_free(req);
            continue;
        }
        pv_note_busy(v, req);
        if (!v->proc && !v->closed) {
            pv_swap_proc(v, ns_rproc_http_spawn_shm_ex(v->renderer_path,
                                     NS_PROC_MAX_WIDTH, NS_PROC_MAX_HEIGHT,
                                     v->private_mode));
            if (!v->proc)
                g_printerr("nordstjernen: could not start renderer %s\n",
                           v->renderer_path);
        }
        if (v->proc && req->dpr > 0)
            ns_rproc_http_set_device_pixel_ratio(v->proc, req->dpr);

        if (req->type == REQ_LOAD) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_PAGE;
            res->seq = req->seq;
            ns_rproc_http_page pg;
            int settle = pv_settle_ms();
            int rc = pv_open(v, req, settle, &pg);
            if (rc != 0 && v->proc && !v->closed) {
                ns_rproc_http_close(pv_swap_proc(v, NULL));
                if (!pv_load_superseded(v, req)) {
                    pv_swap_proc(v, ns_rproc_http_spawn_shm_ex(
                                        v->renderer_path, NS_PROC_MAX_WIDTH,
                                        NS_PROC_MAX_HEIGHT, v->private_mode));
                    if (v->proc)
                        ns_rproc_http_set_device_pixel_ratio(v->proc,
                                                             req->dpr);
                    rc = pv_open(v, req, settle, &pg);
                }
            }
            if (rc == 0 && pg.ok) {
                res->ok = TRUE;
                res->pw = pg.page_width;
                res->ph = pg.page_height;
                res->title = g_strdup(pg.title ? pg.title : "");
                res->url = g_strdup(pg.url ? pg.url : req->url);
                res->nav = pg.nav ? g_strdup(pg.nav) : NULL;
                res->security = pg.security;
                res->remote_ip = pg.remote_ip ? g_strdup(pg.remote_ip) : NULL;
            }
            if (rc == 0)
                ns_rproc_http_page_clear(&pg);
            post(res);
        } else if (req->type == REQ_RENDER) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_FRAME;
            res->seq = req->seq;
            ns_rproc_http_frame fr;
            req->tiles.have = req->have;
            req->tiles.hold = req->hold;
            gboolean rendered = v->proc &&
                (req->tiles.tile_h > 0
                     ? ns_rproc_http_render_tiles(v->proc, req->w, req->h,
                                                  req->sx, req->sy,
                                                  req->scale,
                                                  req->caret_active,
                                                  &req->wheel, &req->tiles,
                                                  &fr)
                     : ns_rproc_http_render_wheel(v->proc, req->w, req->h,
                                                  req->sx, req->sy,
                                                  req->scale,
                                                  req->caret_active,
                                                  &req->wheel, &fr)) == 0 &&
                fr.ok;
            if (rendered) {
                res->ok = TRUE;
                res->animating = fr.animating ? TRUE : FALSE;
                res->wake_ms = fr.wake_ms;
                res->caret_blinking = fr.caret_blinking ? TRUE : FALSE;
                res->wheel_snapped = fr.wheel_snapped ? TRUE : FALSE;
                res->wheel_answered = fr.wheel_answered ? TRUE : FALSE;
                res->wheel_prevented = fr.wheel_prevented ? TRUE : FALSE;
                res->pw = fr.page_w;
                res->ph = fr.page_h;
                res->requested_scroll_y = fr.scroll_y;
                res->requested_scroll_x = fr.scroll_x;
                res->frame_unchanged = fr.unchanged ? TRUE : FALSE;
                if (fr.tiles) {
                    res->layers = ns_layer_update_parse(
                        fr.tiles, fr.pixels, ns_rproc_http_map_size(v->proc));
                    free(fr.tiles);
                    res->frame_unchanged = res->layers == NULL;
                } else if (!fr.unchanged) {
                    res->texture = stage_fill(v, fr.pixels, fr.width,
                                              fr.height, fr.stride);
                    res->texture_scale = req->raster > 0 ? req->raster : 1.0;
                }
                if (fr.nav) {
                    res->nav = g_strdup(fr.nav);
                    free(fr.nav);
                }
                if (fr.webgl) {
                    res->webgl = g_strdup(fr.webgl);
                    free(fr.webgl);
                }
                if (fr.camera) {
                    res->camera = g_strdup(fr.camera);
                    free(fr.camera);
                }
                if (fr.download) {
                    res->download = g_strdup(fr.download);
                    free(fr.download);
                }
                if (fr.config) {
                    res->config = g_strdup(fr.config);
                    free(fr.config);
                }
                if (fr.audio) {
                    res->audio = g_strdup(fr.audio);
                    free(fr.audio);
                }
                if (fr.window_action) {
                    res->window_action = g_strdup(fr.window_action);
                    free(fr.window_action);
                }
                res->popup = fr.popup ? TRUE : FALSE;
                if (fr.clipboard && v->proc)
                    res->clipboard = ns_rproc_http_clipboard(v->proc);
            } else if (v->proc) {
                ns_rproc_http_close(pv_swap_proc(v, NULL));
            }
            post(res);
        } else if (req->type == REQ_TICK) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_TICK;
            res->seq = req->seq;
            ns_rproc_http_tick tick;
            if (v->proc && ns_rproc_http_tick_page(v->proc, &tick) == 0) {
                res->ok = tick.ok ? TRUE : FALSE;
                res->kind = tick.changed;
                res->animating = tick.animating ? TRUE : FALSE;
                res->wake_ms = tick.wake_ms;
                res->pw = tick.page_w;
                res->ph = tick.page_h;
                res->nav = tick.nav ? g_strdup(tick.nav) : NULL;
                res->webgl = tick.webgl ? g_strdup(tick.webgl) : NULL;
                res->camera = tick.camera ? g_strdup(tick.camera) : NULL;
                res->download = tick.download ? g_strdup(tick.download) : NULL;
                res->config = tick.config ? g_strdup(tick.config) : NULL;
                res->audio = tick.audio ? g_strdup(tick.audio) : NULL;
                res->window_action = tick.window_action
                    ? g_strdup(tick.window_action) : NULL;
                res->title = tick.title ? g_strdup(tick.title) : NULL;
                res->url = tick.url ? g_strdup(tick.url) : NULL;
                res->url_pushed = tick.url_pushed ? TRUE : FALSE;
                res->popup = tick.popup ? TRUE : FALSE;
                ns_rproc_http_tick_clear(&tick);
            }
            post(res);
        } else if (req->type == REQ_LINK) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_LINK;
            res->seq = req->seq;
            res->action = req->action;
            if (v->proc && req->action == ACT_HOVER)
                res->href = ns_rproc_http_link_cursor_at(v->proc, req->x,
                                                         req->y, &res->cursor);
            else if (v->proc && req->action == ACT_CONTEXT) {
                int prevented = 0;
                ns_rproc_http_contextmenu(v->proc, req->x, req->y, &prevented,
                                          &res->edit);
                res->prevented = prevented;
                if (!prevented)
                    res->href = ns_rproc_http_link_at(v->proc, req->x, req->y);
            }
            else if (v->proc)
                res->href = ns_rproc_http_link_at(v->proc, req->x, req->y);
            post(res);
        } else if (req->type == REQ_CLICK) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_CLICK;
            res->seq = req->seq;
            res->href = v->proc
                ? ns_rproc_http_press(v->proc, req->x, req->y, req->mods,
                                      req->button, req->clicks)
                : NULL;
            post(res);
        } else if (req->type == REQ_VIEWPORT) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_VIEWPORT;
            res->seq = req->seq;
            ns_rproc_http_page pg;
            if (v->proc &&
                ns_rproc_http_set_viewport(v->proc, req->vw, req->vh, &pg) == 0) {
                res->ok = pg.ok;
                res->pw = pg.page_width;
                res->ph = pg.page_height;
                ns_rproc_http_page_clear(&pg);
            }
            post(res);
        } else if (req->type == REQ_KEY) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_KEY;
            res->seq = req->seq;
            res->kind = req->kind;
            res->fallback_scroll = req->fallback_scroll;
            res->fallback_x = req->fallback_x;
            res->fallback_y = req->fallback_y;
            res->href = v->proc
                ? ns_rproc_http_key_full(v->proc, req->kind, req->key,
                               req->code, req->keycode, req->mods,
                               &res->prevented)
                : NULL;
            post(res);
        } else if (req->type == REQ_SELECT) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = (req->kind == 4 || req->kind == 7) ? RES_COPY
                                                           : RES_SELECT;
            res->seq = req->seq;
            res->kind = req->kind;
            res->href = v->proc
                ? ns_rproc_http_select(v->proc, req->kind, req->x, req->y)
                : NULL;
            post(res);
        } else if (req->type == REQ_HOVER) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_HOVER;
            res->seq = req->seq;
            if (v->proc) {
                int rc = ns_rproc_http_hover_buttons(v->proc, req->x, req->y,
                                                     req->mods, req->buttons,
                                                     &res->href, &res->cursor);
                res->ok = rc == 1;
                res->animating = rc == 2;
            }
            post(res);
        } else if (req->type == REQ_SCROLLBAR) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_SCROLLBAR;
            res->seq = req->seq;
            res->kind = req->kind;
            res->ok = v->proc
                ? ns_rproc_http_scrollbar(v->proc, req->kind, req->x, req->y)
                : 0;
            post(res);
        } else if (req->type == REQ_DROPFILES) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_DROPFILES;
            res->seq = req->seq;
            if (v->proc && req->paths && *req->paths) {
                char **list = g_strsplit(req->paths, "\n", -1);
                guint count = list ? g_strv_length(list) : 0;
                if (count > 0)
                    res->ok = ns_rproc_http_drop_files(
                        v->proc, req->x, req->y,
                        (const char *const *)list, (int)count) == 1;
                g_strfreev(list);
            }
            post(res);
        } else if (req->type == REQ_RELEASE) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_RELEASE;
            res->seq = req->seq;
            res->href = v->proc
                ? ns_rproc_http_release_button(v->proc, req->x, req->y,
                                               req->button, req->clicks,
                                               &res->ok, &res->newtab)
                : NULL;
            if (v->proc && req->button == 0 && (!res->href || !*res->href))
                res->media_url = ns_rproc_http_media_at(v->proc, req->x, req->y,
                                                   &res->media_is_video,
                                                   &res->media_stream);
            post(res);
        } else if (req->type == REQ_FIND) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_FIND;
            res->seq = req->seq;
            if (v->proc)
                ns_rproc_http_find(v->proc, req->query, req->find_case,
                              req->find_dir, req->find_from_y,
                              &res->find_total, &res->find_current,
                              &res->find_scroll_y);
            post(res);
        } else if (req->type == REQ_EXPORT) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_EXPORT;
            res->seq = req->seq;
            gboolean ok = FALSE;
            if (v->proc && req->url && req->export_dest &&
                ns_rproc_http_export(v->proc, req->url) == 0) {
                GFile *src = g_file_new_for_path(req->url);
                GFile *dst = g_file_new_for_path(req->export_dest);
                if (g_file_query_file_type(src,
                                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                           NULL) == G_FILE_TYPE_REGULAR)
                    ok = g_file_copy(src, dst,
                                     G_FILE_COPY_OVERWRITE |
                                         G_FILE_COPY_NOFOLLOW_SYMLINKS,
                                     NULL, NULL, NULL, NULL);
                g_object_unref(src);
                g_object_unref(dst);
            }
            if (req->url)
                g_unlink(req->url);
            res->ok = ok;
            res->url = g_strdup(req->export_dest ? req->export_dest : "");
            post(res);
        } else if (req->type == REQ_CONSOLE) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_CONSOLE;
            res->seq = req->seq;
            res->remote_cb = req->remote_cb;
            res->remote_ud = req->remote_ud;
            res->href = v->proc ? ns_rproc_http_console_poll(v->proc) : NULL;
            post(res);
        } else if (req->type == REQ_EVAL) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_EVAL;
            res->seq = req->seq;
            res->dump_tab = req->dump_tab;
            res->inspect = req->inspect;
            res->remote_cb = req->remote_cb;
            res->remote_ud = req->remote_ud;
            res->href = v->proc ? ns_rproc_http_eval(v->proc, req->query) : NULL;
            post(res);
        } else if (req->type == REQ_POPUP) {
            if (req->kind == 0) {
                Res *res = g_new0(Res, 1);
                res->view = pv_ref(v);
                res->type = RES_POPUP;
                res->seq = req->seq;
                res->href = v->proc ? ns_rproc_http_popup_poll(v->proc) : NULL;
                post(res);
            } else if (v->proc) {
                ns_rproc_http_popup_deliver(v->proc, req->query);
            }
        } else if (req->type == REQ_DOWNLOAD) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_DOWNLOAD;
            res->seq = req->seq;
            res->remote_cb = req->remote_cb;
            res->remote_ud = req->remote_ud;
            if (v->proc && req->kind == 0) {
                long id = ns_rproc_http_download_start(v->proc, req->query);
                char idbuf[32];
                snprintf(idbuf, sizeof idbuf, "%ld", id);
                if (id > 0) res->href = strdup(idbuf);
            } else if (v->proc && req->kind == 1) {
                res->href = ns_rproc_http_download_poll(v->proc,
                                                        req->download_id);
            } else if (v->proc) {
                ns_rproc_http_download_cancel(v->proc, req->download_id);
            }
            post(res);
        } else if (req->type == REQ_DUMP) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_DUMP;
            res->seq = req->seq;
            res->dump_tab = req->dump_tab;
            res->href = v->proc ? ns_rproc_http_dump(v->proc, req->query) : NULL;
            post(res);
        } else if (req->type == REQ_WEBGL) {
            if (v->proc)
                ns_rproc_http_resolve_webgl(v->proc, req->url, req->mods);
        } else if (req->type == REQ_PRINT) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_PRINT;
            res->seq = req->seq;
            if (v->proc)
                res->print_pages = ns_rproc_http_print(v->proc,
                                                       &res->print_setup,
                                                       &res->print_scale);
            res->ok = res->print_pages != NULL;
            post(res);
        } else if (req->type == REQ_CAMERA) {
            if (v->proc)
                ns_rproc_http_resolve_camera(v->proc, req->url, req->mods);
        } else if (req->type == REQ_COLOR_SCHEME) {
            if (v->proc)
                ns_rproc_http_set_color_scheme(v->proc, req->mods);
        } else if (req->type == REQ_AUDIO_EVENT) {
            if (v->proc)
                ns_rproc_http_audio_event(v->proc, req->query);
        } else if (req->type == REQ_FAVICON) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_FAVICON;
            res->seq = req->seq;
            if (v->proc)
                res->favicon_data = ns_rproc_http_favicon(
                    v->proc, &res->favicon_w, &res->favicon_h,
                    &res->favicon_stride);
            post(res);
        }
        pv_note_busy(v, NULL);
        req_free(req);
    }
    if (v->proc)
        ns_rproc_http_close(pv_swap_proc(v, NULL));
    pv_unref(v);
    return NULL;
}

static void
push_req(NsProcView *v, Req *req)
{
    g_async_queue_push(v->queue, req);
}

static void
request_favicon(NsProcView *v)
{
    Req *req = g_new0(Req, 1);
    req->type = REQ_FAVICON;
    req->seq = v->load_seq;
    push_req(v, req);
}

static int
viewport_w(NsProcView *v)
{
    int w = v->area ? gtk_widget_get_width(v->area) : 0;
    return w > 0 ? w : 1;
}

static int
viewport_h(NsProcView *v)
{
    int h = v->area ? gtk_widget_get_height(v->area) : 0;
    return h > 0 ? h : 1;
}

static double
cur_scale(NsProcView *v)
{
    return v->scale > 0.0 ? v->scale : 1.0;
}

static double
device_scale(NsProcView *v)
{
    GtkNative *native = v->area ? gtk_widget_get_native(v->area) : NULL;
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    double s = surface ? gdk_surface_get_scale(surface) : 0.0;
    if (!(s > 0.0) && v->area)
        s = gtk_widget_get_scale_factor(v->area);
    return s >= 1.0 && s <= 8.0 ? s : 1.0;
}

static double
raster_scale(NsProcView *v)
{
    double s = device_scale(v);
    double fit_w = (double)NS_PROC_MAX_WIDTH / viewport_w(v);
    double fit_h = (double)NS_PROC_MAX_HEIGHT / viewport_h(v);
    if (fit_w < s) s = fit_w;
    if (fit_h < s) s = fit_h;
    return s > 0.0 ? s : 1.0;
}

static double
page_dpr(NsProcView *v)
{
    return cur_scale(v) * device_scale(v);
}

static int
css_viewport_w(NsProcView *v)
{
    int w = (int)lround(viewport_w(v) / cur_scale(v));
    return w > 0 ? w : 1;
}

static int
css_viewport_h(NsProcView *v)
{
    int h = (int)lround(viewport_h(v) / cur_scale(v));
    return h > 0 ? h : 1;
}

static void
configure_adjustments(NsProcView *v)
{
    double s = cur_scale(v);
    double cw = viewport_w(v) / s;
    double ch = viewport_h(v) / s;
    gboolean can_scroll_x = v->page_w > cw + 0.5;
    gboolean can_scroll_y = v->page_h > ch + 0.5;
    if (!can_scroll_x) v->scroll_x = 0;
    if (!can_scroll_y) v->scroll_y = 0;
    if (v->hscroll) gtk_widget_set_visible(v->hscroll, can_scroll_x);
    if (v->vscroll) gtk_widget_set_visible(v->vscroll, can_scroll_y);
    double upper_w = v->page_w > cw ? v->page_w : cw;
    double upper_h = v->page_h > ch ? v->page_h : ch;
    gtk_adjustment_configure(v->hadj, v->scroll_x, 0, upper_w, 60, cw, cw);
    gtk_adjustment_configure(v->vadj, v->scroll_y, 0, upper_h, 60, ch, ch);
    v->scroll_x = (int)gtk_adjustment_get_value(v->hadj);
    v->scroll_y = (int)gtk_adjustment_get_value(v->vadj);
}

static void
on_adj_changed(GtkAdjustment *adj, gpointer data)
{
    (void)adj;
    NsProcView *v = data;
    if (v->closed)
        return;
    v->scroll_x = (int)gtk_adjustment_get_value(v->hadj);
    v->scroll_y = (int)gtk_adjustment_get_value(v->vadj);
    if (v->tiles_mode)
        gtk_widget_queue_draw(v->area);
    if (v->opened && !v->adopting_scroll)
        request_render(v);
}

static void start_render(NsProcView *v);

static gboolean
pv_tiles_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = g_getenv("NS_TILES");
        enabled = !(e && strcmp(e, "0") == 0);
    }
    return enabled;
}

static void
pv_tiles_range(NsProcView *v, double *y0, double *y1)
{
    double vh = viewport_h(v) / cur_scale(v);
    double ahead = vh * 1.5, behind = vh * 0.5;
    *y0 = MAX(v->scroll_y - (v->scroll_dir < 0 ? ahead : behind), 0);
    *y1 = v->scroll_y + vh + (v->scroll_dir < 0 ? behind : ahead);
}

static void
pv_tiles_request(NsProcView *v, Req *req)
{
    if (!pv_tiles_enabled())
        return;
    double y0, y1;
    pv_tiles_range(v, &y0, &y1);
    req->tiles.tile_h = NS_PV_TILE_H;
    req->tiles.want_y0 = (int)floor(y0);
    req->tiles.want_y1 = (int)ceil(y1);
    req->tiles.gen = ns_page_layers_gen(v->layers);
    req->tiles.vp_held = ns_page_layers_vp_held(v->layers);
    req->tiles.fill = v->tiles_fill;
    v->tiles_fill = FALSE;
    req->have = ns_page_layers_have(v->layers, y0, y1, FALSE);
    req->hold = ns_page_layers_have(v->layers, y0, y1, TRUE);
}

static gboolean
pv_adjustments_stale(NsProcView *v)
{
    double s = cur_scale(v);
    double cw = viewport_w(v) / s, ch = viewport_h(v) / s;
    return fabs(gtk_adjustment_get_upper(v->hadj) - MAX(v->page_w, cw)) > 0.5 ||
           fabs(gtk_adjustment_get_upper(v->vadj) - MAX(v->page_h, ch)) > 0.5;
}

static void
pv_tiles_apply(NsProcView *v, Res *res)
{
    gboolean changed = ns_page_layers_apply(v->layers, res->layers);
    if (!ns_page_layers_active(v->layers))
        return;
    if (!v->tiles_mode) {
        v->tiles_mode = TRUE;
        g_clear_object(&v->frame);
    }
    v->render_restarts = 0;
    clear_busy_cursor(v);
    double vh = viewport_h(v) / cur_scale(v);
    ns_page_layers_evict(v->layers, v->scroll_y - vh * 1.5,
                         v->scroll_y + vh * 2.5);
    if (res->ph > 0) {
        v->page_h = res->ph;
        if (res->pw > 0) v->page_w = res->pw;
    }
    if (pv_adjustments_stale(v)) {
        v->adopting_scroll = TRUE;
        configure_adjustments(v);
        v->adopting_scroll = FALSE;
    }
    double y0, y1;
    pv_tiles_range(v, &y0, &y1);
    if (changed) {
        gtk_widget_queue_draw(v->area);
        if (ns_page_layers_missing(v->layers, y0, y1, v->scroll_y,
                                   v->scroll_y + vh, v->page_h) &&
            !v->render_pending) {
            v->render_pending = TRUE;
            v->tiles_fill = TRUE;
        }
    }
}

static void
pv_tiles_leave(NsProcView *v)
{
    if (!v->tiles_mode)
        return;
    v->tiles_mode = FALSE;
    ns_page_layers_reset(v->layers);
}

static void
request_tick(NsProcView *v)
{
    if (!v->opened) return;
    if (v->tick_inflight) {
        v->tick_pending = TRUE;
        return;
    }
    v->tick_inflight = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_TICK;
    req->seq = v->load_seq;
    g_async_queue_push_front(v->queue, req);
}

static void
request_render(NsProcView *v)
{
    if (!v->opened)
        return;
    v->tiles_fill = FALSE;
    if (v->render_inflight) {
        v->render_pending = TRUE;
        return;
    }
    start_render(v);
}

static void
start_render(NsProcView *v)
{
    if (!v->opened)
        return;
    v->render_inflight = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_RENDER;
    req->seq = ++v->render_seq;
    req->raster = raster_scale(v);
    req->w = (int)ceil(viewport_w(v) * req->raster);
    req->h = (int)ceil(viewport_h(v) * req->raster);
    req->sx = v->scroll_x;
    req->sy = v->scroll_y;
    req->scale = cur_scale(v) * req->raster;
    req->dpr = page_dpr(v);
    req->caret_active = gtk_widget_has_focus(v->area);
    pv_tiles_request(v, req);
    req->wheel.dx = (int)v->wheel_pend_x;
    req->wheel.dy = (int)v->wheel_pend_y;
    if (req->wheel.dx || req->wheel.dy) {
        double s = cur_scale(v);
        v->wheel_pend_x -= req->wheel.dx;
        v->wheel_pend_y -= req->wheel.dy;
        req->wheel.x = v->scroll_x + (int)(v->pointer_x / s);
        req->wheel.y = v->scroll_y + (int)(v->pointer_y / s);
        req->wheel.viewport = v->wheel_viewport;
    }
    if (v->wev_dx != 0 || v->wev_dy != 0) {
        req->wheel.ev = 1;
        req->wheel.ev_x = v->wev_x;
        req->wheel.ev_y = v->wev_y;
        req->wheel.ev_dx = (int)lround(v->wev_dx);
        req->wheel.ev_dy = (int)lround(v->wev_dy);
        req->wheel.ev_mods = v->wev_mods;
        v->wev_dx = v->wev_dy = 0;
        v->wev_sent_x += v->wev_hold_x;
        v->wev_sent_y += v->wev_hold_y;
        v->wev_hold_x = v->wev_hold_y = 0;
    }
    push_req(v, req);
}

static gboolean
hover_after_scroll(gpointer data)
{
    NsProcView *v = data;
    v->hover_after_scroll_id = 0;
    if (v->closed || !v->opened)
        return G_SOURCE_REMOVE;
    if (gtk_widget_contains(v->area, v->pointer_x, v->pointer_y)) {
        double s = cur_scale(v);
        request_hover(v, v->scroll_x + (int)(v->pointer_x / s),
                      v->scroll_y + (int)(v->pointer_y / s));
    }
    return G_SOURCE_REMOVE;
}

static void
pv_route_wheel(NsProcView *v, double dx, double dy)
{
    double s = cur_scale(v);
    v->wheel_remote = !v->wheel_viewport &&
        ns_page_layers_scroller_at(v->layers, v->pointer_x / s,
                                   v->pointer_y / s, v->scroll_x,
                                   v->scroll_y, fabs(dy) >= fabs(dx));
}

static int
pv_take_whole(double *pend)
{
    double r = round(*pend);
    int whole = fabs(*pend - r) < 1e-6 ? (int)r : (int)*pend;
    *pend -= whole;
    return whole;
}

static void
pv_scroll_local(NsProcView *v)
{
    int dx = pv_take_whole(&v->wheel_pend_x);
    int dy = pv_take_whole(&v->wheel_pend_y);
    if (!dx && !dy)
        return;
    if (dy)
        v->scroll_dir = dy > 0 ? 1 : -1;
    int want_x = v->scroll_x + dx, want_y = v->scroll_y + dy;
    gtk_adjustment_set_value(v->hadj, want_x);
    gtk_adjustment_set_value(v->vadj, want_y);
    if (v->scroll_x != want_x)
        v->wheel_left_x = v->fling_vx = v->wheel_pend_x = 0;
    if (v->scroll_y != want_y)
        v->wheel_left_y = v->fling_vy = v->wheel_pend_y = 0;
}

static void
queue_wheel_scroll(NsProcView *v, double dx, double dy)
{
    if (!v->opened)
        return;
    if (v->hover_after_scroll_id)
        g_source_remove(v->hover_after_scroll_id);
    else
        pv_route_wheel(v, dx, dy);
    v->hover_after_scroll_id =
        g_timeout_add(NS_PV_HOVER_AFTER_SCROLL_MS, hover_after_scroll, v);
    v->wheel_pend_x += dx;
    v->wheel_pend_y += dy;
    if (v->tiles_mode && !v->wheel_remote)
        pv_scroll_local(v);
    else if (fabs(v->wheel_pend_x) >= 1.0 || fabs(v->wheel_pend_y) >= 1.0)
        request_render(v);
}

static gboolean
wheel_animation_idle(const NsProcView *v)
{
    return v->wheel_left_x == 0 && v->wheel_left_y == 0 &&
           v->fling_vx == 0 && v->fling_vy == 0;
}

static gboolean
pv_wheel_scrolling(const NsProcView *v)
{
    return v->hover_after_scroll_id != 0 || !wheel_animation_idle(v) ||
           v->wheel_pend_x != 0 || v->wheel_pend_y != 0;
}

static gboolean
pv_queue_wheel_event(NsProcView *v, double dx, double dy, GdkModifierType mods)
{
    double s = cur_scale(v);
    v->wev_dx += dx;
    v->wev_dy += dy;
    v->wev_x = v->scroll_x + (int)(v->pointer_x / s);
    v->wev_y = v->scroll_y + (int)(v->pointer_y / s);
    v->wev_mods = ((mods & GDK_SHIFT_MASK) ? 1 : 0) |
                  ((mods & GDK_ALT_MASK)   ? 4 : 0) |
                  ((mods & GDK_META_MASK)  ? 8 : 0);
    gboolean held = v->wev_hold_x != 0 || v->wev_hold_y != 0 ||
        ns_page_layers_wheel_listener_at(v->layers, v->pointer_x / s,
                                         v->pointer_y / s, v->scroll_x,
                                         v->scroll_y);
    if (held) {
        v->wev_hold_x += dx;
        v->wev_hold_y += dy;
    }
    if (!v->opened)
        return held;
    if (v->render_inflight)
        v->render_pending = TRUE;
    else
        start_render(v);
    return held;
}

static void arm_wheel_animation(NsProcView *v);

static void
pv_wheel_answered(NsProcView *v, gboolean prevented)
{
    if (!prevented && (v->wev_sent_x != 0 || v->wev_sent_y != 0)) {
        v->wheel_left_x += v->wev_sent_x;
        v->wheel_left_y += v->wev_sent_y;
        arm_wheel_animation(v);
    }
    v->wev_sent_x = v->wev_sent_y = 0;
}

static gboolean
wheel_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)widget;
    NsProcView *v = data;
    if (v->closed || !v->opened || wheel_animation_idle(v)) {
        v->wheel_tick_id = 0;
        v->wheel_last_us = 0;
        return G_SOURCE_REMOVE;
    }
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt_ms = v->wheel_last_us > 0
        ? (double)(now - v->wheel_last_us) / 1000.0 : 1000.0 / 60.0;
    dt_ms = CLAMP(dt_ms, 1.0, 100.0);
    v->wheel_last_us = now;

    double share = 1.0 - exp(-dt_ms / NS_PV_WHEEL_TAU_MS);
    double mx = v->wheel_left_x * share;
    double my = v->wheel_left_y * share;
    if (fabs(v->wheel_left_x - mx) < 0.5) mx = v->wheel_left_x;
    if (fabs(v->wheel_left_y - my) < 0.5) my = v->wheel_left_y;
    v->wheel_left_x -= mx;
    v->wheel_left_y -= my;

    mx += v->fling_vx * dt_ms / 1000.0;
    my += v->fling_vy * dt_ms / 1000.0;
    double decay = exp(-dt_ms / NS_PV_FLING_TAU_MS);
    v->fling_vx *= decay;
    v->fling_vy *= decay;
    if (hypot(v->fling_vx, v->fling_vy) < NS_PV_FLING_STOP_PX_S)
        v->fling_vx = v->fling_vy = 0;

    queue_wheel_scroll(v, mx, my);
    return G_SOURCE_CONTINUE;
}

static void
arm_wheel_animation(NsProcView *v)
{
    if (v->wheel_tick_id || !v->area || wheel_animation_idle(v))
        return;
    v->wheel_last_us = 0;
    v->wheel_tick_id = gtk_widget_add_tick_callback(v->area, wheel_tick, v,
                                                    NULL);
}

static void
stop_wheel_animation(NsProcView *v)
{
    v->wheel_left_x = v->wheel_left_y = 0;
    v->wheel_pend_x = v->wheel_pend_y = 0;
    v->fling_vx = v->fling_vy = 0;
}

static double
scroll_target_x(const NsProcView *v)
{
    return v->scroll_x + v->wheel_pend_x + v->wheel_left_x;
}

static double
scroll_target_y(const NsProcView *v)
{
    return v->scroll_y + v->wheel_pend_y + v->wheel_left_y;
}

static void
scroll_view_to(NsProcView *v, double x, double y)
{
    double max_x = gtk_adjustment_get_upper(v->hadj) -
                   gtk_adjustment_get_page_size(v->hadj);
    double max_y = gtk_adjustment_get_upper(v->vadj) -
                   gtk_adjustment_get_page_size(v->vadj);
    x = CLAMP(x, 0, MAX(max_x, 0));
    y = CLAMP(y, 0, MAX(max_y, 0));
    v->fling_vx = v->fling_vy = 0;
    v->wheel_viewport = TRUE;
    v->wheel_left_x += x - scroll_target_x(v);
    v->wheel_left_y += y - scroll_target_y(v);
    arm_wheel_animation(v);
}

static void
on_device_scale_changed(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object;
    (void)pspec;
    NsProcView *v = data;
    if (v->closed || !v->opened)
        return;
    if (!maybe_update_viewport(v))
        request_render(v);
}

static void
on_area_realize(GtkWidget *area, gpointer data)
{
    NsProcView *v = data;
    GtkNative *native = gtk_widget_get_native(area);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    if (!surface || surface == v->scale_surface)
        return;
    if (v->scale_surface && v->surface_scale_handler)
        g_signal_handler_disconnect(v->scale_surface, v->surface_scale_handler);
    v->scale_surface = surface;
    v->surface_scale_handler =
        g_signal_connect(surface, "notify::scale",
                         G_CALLBACK(on_device_scale_changed), v);
}

static void
on_area_unrealize(GtkWidget *area, gpointer data)
{
    (void)area;
    NsProcView *v = data;
    if (v->scale_surface && v->surface_scale_handler)
        g_signal_handler_disconnect(v->scale_surface, v->surface_scale_handler);
    v->scale_surface = NULL;
    v->surface_scale_handler = 0;
}

static void
on_area_focus_notify(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object;
    (void)pspec;
    NsProcView *v = data;
    v->last_anim_frame_us = 0;
    request_render(v);
}

static gboolean
anim_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)widget;
    (void)clock;
    NsProcView *v = data;
    if (v->closed || !v->opened) {
        v->anim_tick_id = 0;
        v->last_anim_frame_us = 0;
        return G_SOURCE_REMOVE;
    }
    gint64 now = clock ? gdk_frame_clock_get_frame_time(clock) : 0;
    if (now <= 0) now = g_get_monotonic_time();
    if (ns_page_layers_advance_videos(v->layers, g_get_monotonic_time()))
        gtk_widget_queue_draw(v->area);
    gint64 interval = v->page_animating
                          ? NS_PROC_ANIM_FRAME_US - NS_PROC_ANIM_FRAME_SLACK_US
                          : NS_PROC_CARET_BLINK_US;
    if (v->last_anim_frame_us > 0 && now - v->last_anim_frame_us < interval)
        return G_SOURCE_CONTINUE;
    v->last_anim_frame_us = now;
    if (v->page_animating && !pv_wheel_scrolling(v))
        request_tick(v);
    else
        request_render(v);
    return G_SOURCE_CONTINUE;
}

static void
arm_anim(NsProcView *v)
{
    if (v->anim_tick_id || v->closed || !v->area)
        return;
    v->last_anim_frame_us = 0;
    v->anim_tick_id = gtk_widget_add_tick_callback(v->area, anim_tick, v, NULL);
}

static void
cancel_anim_timeout(NsProcView *v)
{
    if (v->anim_timeout_id)
        g_source_remove(v->anim_timeout_id);
    v->anim_timeout_id = 0;
}

static void
disarm_anim(NsProcView *v)
{
    if (v->anim_tick_id && v->area)
        gtk_widget_remove_tick_callback(v->area, v->anim_tick_id);
    v->anim_tick_id = 0;
    v->last_anim_frame_us = 0;
    cancel_anim_timeout(v);
}

static void schedule_anim(NsProcView *v);

static gboolean
anim_timeout(gpointer data)
{
    NsProcView *v = data;
    v->anim_timeout_id = 0;
    if (v->closed || !v->opened || !v->area ||
        !gtk_widget_get_mapped(v->area))
        return G_SOURCE_REMOVE;
    gint64 now = g_get_monotonic_time();
    gboolean page_due = v->page_animating && now >= v->page_due_us;
    gboolean caret_due = v->caret_blinking && now >= v->caret_due_us;
    if (page_due && !pv_wheel_scrolling(v))
        request_tick(v);
    if (caret_due)
        request_render(v);
    if (!page_due && !caret_due)
        schedule_anim(v);
    return G_SOURCE_REMOVE;
}

static void
schedule_anim(NsProcView *v)
{
    if (v->closed || !v->area)
        return;
    if (v->page_animating && v->page_wake_ms <= NS_PROC_FRAME_WAKE_MS) {
        cancel_anim_timeout(v);
        arm_anim(v);
        return;
    }
    disarm_anim(v);
    if (!v->page_animating && !v->caret_blinking)
        return;
    gint64 due = G_MAXINT64;
    if (v->page_animating && v->page_due_us < due)
        due = v->page_due_us;
    if (v->caret_blinking && v->caret_due_us < due)
        due = v->caret_due_us;
    gint64 delay_ms = (due - g_get_monotonic_time() + 999) / 1000;
    v->anim_timeout_id = g_timeout_add((guint)CLAMP(delay_ms, 1, G_MAXINT),
                                       anim_timeout, v);
}

static void
note_anim(NsProcView *v, gboolean page_animating, int wake_ms)
{
    v->page_animating = page_animating;
    v->page_wake_ms = wake_ms;
    v->page_due_us = g_get_monotonic_time() + (gint64)wake_ms * 1000;
    schedule_anim(v);
}

static void
on_area_map(GtkWidget *area, gpointer data)
{
    (void)area;
    schedule_anim(data);
}

static void start_link(NsProcView *v, int x, int y, LinkAct action);
static void show_context_menu(NsProcView *v, const char *href, int edit);
static void build_search_bar(NsProcView *v);
static void console_append(NsProcView *v, const char *text);
static void console_set_open(NsProcView *v, gboolean open);

static void
request_link(NsProcView *v, int x, int y, LinkAct action)
{
    if (!v->opened)
        return;
    if (v->link_inflight) {
        if (action != ACT_HOVER || v->link_pending_action == ACT_HOVER) {
            v->link_pending_x = x;
            v->link_pending_y = y;
            v->link_pending_action = action;
        }
        v->link_pending = TRUE;
        return;
    }
    start_link(v, x, y, action);
}

static void
start_link(NsProcView *v, int x, int y, LinkAct action)
{
    if (!v->opened)
        return;
    v->link_inflight = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_LINK;
    req->seq = ++v->link_seq;
    req->x = x;
    req->y = y;
    req->action = action;
    push_req(v, req);
}

static void start_hover(NsProcView *v, int x, int y);

static void
request_hover(NsProcView *v, int x, int y)
{
    if (!v->opened)
        return;
    if (v->hover_inflight) {
        v->hover_pending_x = x;
        v->hover_pending_y = y;
        v->hover_pending = TRUE;
        return;
    }
    start_hover(v, x, y);
}

static void
start_hover(NsProcView *v, int x, int y)
{
    if (!v->opened)
        return;
    v->hover_inflight = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_HOVER;
    req->seq = ++v->hover_seq;
    req->x = x;
    req->y = y;
    req->mods = v->pointer_mods;
    req->buttons = v->pointer_buttons;
    push_req(v, req);
}

typedef struct {
    guint       keyval;
    const char *key;
    const char *code;
    int         keycode;
} PvKeyName;

static const PvKeyName pv_key_names[] = {
    { GDK_KEY_Up,           "ArrowUp",     "ArrowUp",        38 },
    { GDK_KEY_Down,         "ArrowDown",   "ArrowDown",      40 },
    { GDK_KEY_Left,         "ArrowLeft",   "ArrowLeft",      37 },
    { GDK_KEY_Right,        "ArrowRight",  "ArrowRight",     39 },
    { GDK_KEY_Return,       "Enter",       "Enter",          13 },
    { GDK_KEY_KP_Enter,     "Enter",       "NumpadEnter",    13 },
    { GDK_KEY_Escape,       "Escape",      "Escape",         27 },
    { GDK_KEY_BackSpace,    "Backspace",   "Backspace",       8 },
    { GDK_KEY_Tab,          "Tab",         "Tab",             9 },
    { GDK_KEY_ISO_Left_Tab, "Tab",         "Tab",             9 },
    { GDK_KEY_Delete,       "Delete",      "Delete",         46 },
    { GDK_KEY_Insert,       "Insert",      "Insert",         45 },
    { GDK_KEY_Home,         "Home",        "Home",           36 },
    { GDK_KEY_End,          "End",         "End",            35 },
    { GDK_KEY_Page_Up,      "PageUp",      "PageUp",         33 },
    { GDK_KEY_Page_Down,    "PageDown",    "PageDown",       34 },
    { GDK_KEY_space,        NULL,          "Space",          32 },
    { GDK_KEY_Shift_L,      "Shift",       "ShiftLeft",      16 },
    { GDK_KEY_Shift_R,      "Shift",       "ShiftRight",     16 },
    { GDK_KEY_Control_L,    "Control",     "ControlLeft",    17 },
    { GDK_KEY_Control_R,    "Control",     "ControlRight",   17 },
    { GDK_KEY_Alt_L,        "Alt",         "AltLeft",        18 },
    { GDK_KEY_Alt_R,        "Alt",         "AltRight",       18 },
    { GDK_KEY_ISO_Level3_Shift, "AltGraph", "AltRight",      225 },
    { GDK_KEY_Super_L,      "Meta",        "MetaLeft",       91 },
    { GDK_KEY_Super_R,      "Meta",        "MetaRight",      92 },
    { GDK_KEY_Meta_L,       "Meta",        "MetaLeft",       91 },
    { GDK_KEY_Meta_R,       "Meta",        "MetaRight",      92 },
    { GDK_KEY_Menu,         "ContextMenu", "ContextMenu",    93 },
    { GDK_KEY_Caps_Lock,    "CapsLock",    "CapsLock",       20 },
    { GDK_KEY_Num_Lock,     "NumLock",     "NumLock",       144 },
    { GDK_KEY_Scroll_Lock,  "ScrollLock",  "ScrollLock",    145 },
    { GDK_KEY_Pause,        "Pause",       "Pause",          19 },
    { GDK_KEY_Print,        "PrintScreen", "PrintScreen",    44 },
    { GDK_KEY_KP_Add,       NULL,          "NumpadAdd",     107 },
    { GDK_KEY_KP_Subtract,  NULL,          "NumpadSubtract", 109 },
    { GDK_KEY_KP_Multiply,  NULL,          "NumpadMultiply", 106 },
    { GDK_KEY_KP_Divide,    NULL,          "NumpadDivide",  111 },
    { GDK_KEY_KP_Decimal,   NULL,          "NumpadDecimal", 110 },
    { GDK_KEY_KP_Separator, NULL,          "NumpadDecimal", 110 },
    { GDK_KEY_KP_Insert,    "Insert",      "Numpad0",        45 },
    { GDK_KEY_KP_End,       "End",         "Numpad1",        35 },
    { GDK_KEY_KP_Down,      "ArrowDown",   "Numpad2",        40 },
    { GDK_KEY_KP_Page_Down, "PageDown",    "Numpad3",        34 },
    { GDK_KEY_KP_Left,      "ArrowLeft",   "Numpad4",        37 },
    { GDK_KEY_KP_Begin,     "Clear",       "Numpad5",        12 },
    { GDK_KEY_KP_Right,     "ArrowRight",  "Numpad6",        39 },
    { GDK_KEY_KP_Home,      "Home",        "Numpad7",        36 },
    { GDK_KEY_KP_Up,        "ArrowUp",     "Numpad8",        38 },
    { GDK_KEY_KP_Page_Up,   "PageUp",      "Numpad9",        33 },
    { GDK_KEY_KP_Delete,    "Delete",      "NumpadDecimal",  46 },
    { GDK_KEY_minus,        NULL,          "Minus",         189 },
    { GDK_KEY_underscore,   NULL,          "Minus",         189 },
    { GDK_KEY_equal,        NULL,          "Equal",         187 },
    { GDK_KEY_plus,         NULL,          "Equal",         187 },
    { GDK_KEY_bracketleft,  NULL,          "BracketLeft",   219 },
    { GDK_KEY_braceleft,    NULL,          "BracketLeft",   219 },
    { GDK_KEY_bracketright, NULL,          "BracketRight",  221 },
    { GDK_KEY_braceright,   NULL,          "BracketRight",  221 },
    { GDK_KEY_backslash,    NULL,          "Backslash",     220 },
    { GDK_KEY_bar,          NULL,          "Backslash",     220 },
    { GDK_KEY_semicolon,    NULL,          "Semicolon",     186 },
    { GDK_KEY_colon,        NULL,          "Semicolon",     186 },
    { GDK_KEY_apostrophe,   NULL,          "Quote",         222 },
    { GDK_KEY_quotedbl,     NULL,          "Quote",         222 },
    { GDK_KEY_grave,        NULL,          "Backquote",     192 },
    { GDK_KEY_asciitilde,   NULL,          "Backquote",     192 },
    { GDK_KEY_comma,        NULL,          "Comma",         188 },
    { GDK_KEY_less,         NULL,          "Comma",         188 },
    { GDK_KEY_period,       NULL,          "Period",        190 },
    { GDK_KEY_greater,      NULL,          "Period",        190 },
    { GDK_KEY_slash,        NULL,          "Slash",         191 },
    { GDK_KEY_question,     NULL,          "Slash",         191 },
    { GDK_KEY_parenright,   NULL,          "Digit0",         48 },
    { GDK_KEY_exclam,       NULL,          "Digit1",         49 },
    { GDK_KEY_at,           NULL,          "Digit2",         50 },
    { GDK_KEY_numbersign,   NULL,          "Digit3",         51 },
    { GDK_KEY_dollar,       NULL,          "Digit4",         52 },
    { GDK_KEY_percent,      NULL,          "Digit5",         53 },
    { GDK_KEY_asciicircum,  NULL,          "Digit6",         54 },
    { GDK_KEY_ampersand,    NULL,          "Digit7",         55 },
    { GDK_KEY_asterisk,     NULL,          "Digit8",         56 },
    { GDK_KEY_parenleft,    NULL,          "Digit9",         57 },
};

static const PvKeyName *
pv_key_name(guint keyval)
{
    for (gsize i = 0; i < G_N_ELEMENTS(pv_key_names); i++)
        if (pv_key_names[i].keyval == keyval)
            return &pv_key_names[i];
    return NULL;
}

static gboolean
pv_keyval_is_dead(guint keyval)
{
    return (keyval >= GDK_KEY_dead_grave && keyval <= GDK_KEY_dead_currency) ||
           (keyval >= GDK_KEY_dead_a &&
            keyval <= GDK_KEY_dead_longsolidusoverlay);
}

static const char *
keyval_js_key(guint keyval, char *buf, size_t bufsz)
{
    gunichar uc = gdk_keyval_to_unicode(keyval);
    if (uc >= 0x20 && uc != 0x7f) {
        int len = g_unichar_to_utf8(uc, buf);
        if (len >= (int)bufsz) len = (int)bufsz - 1;
        buf[len] = '\0';
        return buf;
    }
    const PvKeyName *name = pv_key_name(keyval);
    if (name && name->key)
        return name->key;
    if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F24) {
        g_snprintf(buf, bufsz, "F%u", keyval - GDK_KEY_F1 + 1);
        return buf;
    }
    if (pv_keyval_is_dead(keyval))
        return "Dead";
    const char *n = gdk_keyval_name(keyval);
    return n ? n : "";
}

static const char *
keyval_js_code(guint keyval, char *buf, size_t bufsz)
{
    if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_z) {
        g_snprintf(buf, bufsz, "Key%c", 'A' + (int)(keyval - GDK_KEY_a));
        return buf;
    }
    if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z) {
        g_snprintf(buf, bufsz, "Key%c", 'A' + (int)(keyval - GDK_KEY_A));
        return buf;
    }
    if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) {
        g_snprintf(buf, bufsz, "Digit%c", '0' + (int)(keyval - GDK_KEY_0));
        return buf;
    }
    if (keyval >= GDK_KEY_KP_0 && keyval <= GDK_KEY_KP_9) {
        g_snprintf(buf, bufsz, "Numpad%c", '0' + (int)(keyval - GDK_KEY_KP_0));
        return buf;
    }
    if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F24) {
        g_snprintf(buf, bufsz, "F%u", keyval - GDK_KEY_F1 + 1);
        return buf;
    }
    const PvKeyName *name = pv_key_name(keyval);
    return name ? name->code : "";
}

static int
keyval_js_keycode(guint keyval)
{
    if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_z)
        return 65 + (int)(keyval - GDK_KEY_a);
    if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z)
        return 65 + (int)(keyval - GDK_KEY_A);
    if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9)
        return 48 + (int)(keyval - GDK_KEY_0);
    if (keyval >= GDK_KEY_KP_0 && keyval <= GDK_KEY_KP_9)
        return 96 + (int)(keyval - GDK_KEY_KP_0);
    if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F24)
        return 112 + (int)(keyval - GDK_KEY_F1);
    const PvKeyName *name = pv_key_name(keyval);
    return name ? name->keycode : 0;
}

static void
start_key_full(NsProcView *v, int kind, guint keyval, GdkModifierType state,
               int fallback_scroll, double fallback_x, double fallback_y)
{
    if (!v->opened)
        return;
    char keybuf[8] = {0}, codebuf[16] = {0};
    Req *req = g_new0(Req, 1);
    req->type = REQ_KEY;
    req->seq = kind == 0 ? ++v->key_seq : v->key_seq;
    req->kind = kind;
    req->keycode = keyval_js_keycode(keyval);
    req->fallback_scroll = fallback_scroll;
    req->fallback_x = fallback_x;
    req->fallback_y = fallback_y;
    req->mods = ((state & GDK_SHIFT_MASK)   ? 1 : 0) |
                ((state & GDK_CONTROL_MASK) ? 2 : 0) |
                ((state & GDK_ALT_MASK)     ? 4 : 0) |
                ((state & GDK_META_MASK)    ? 8 : 0) |
                (kind == 0 ? NS_KEY_MOD_TEXT_SEPARATE : 0);
    req->key = g_strdup(keyval_js_key(keyval, keybuf, sizeof keybuf));
    req->code = g_strdup(keyval_js_code(keyval, codebuf, sizeof codebuf));
    push_req(v, req);
}

static void
start_key(NsProcView *v, int kind, guint keyval, GdkModifierType state)
{
    start_key_full(v, kind, keyval, state, 0, 0, 0);
}

static void
start_key_text(NsProcView *v, int kind, const char *text)
{
    if (!v->opened || !text || !*text)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_KEY;
    req->seq = kind == 2 ? v->key_seq : ++v->key_seq;
    req->kind = kind;
    req->keycode = 0;
    req->mods = 0;
    req->key = g_strdup(text);
    req->code = g_strdup("");
    push_req(v, req);
}

static void
on_im_commit(GtkIMContext *im, const char *text, gpointer data)
{
    (void)im;
    NsProcView *v = data;
    if (!v || !v->opened || !text || !*text) return;
    start_key_text(v, 2, text);
}

static void
start_release(NsProcView *v, int x, int y, int button, int clicks)
{
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_RELEASE;
    req->x = x;
    req->y = y;
    req->button = button;
    req->clicks = clicks;
    push_req(v, req);
}

static void
start_select(NsProcView *v, int kind, int x, int y)
{
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_SELECT;
    req->seq = ++v->select_seq;
    req->kind = kind;
    req->x = x;
    req->y = y;
    push_req(v, req);
}

static void
start_click(NsProcView *v, int x, int y, int mods, int button, int clicks)
{
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_CLICK;
    req->seq = ++v->click_seq;
    req->x = x;
    req->y = y;
    req->mods = mods;
    req->button = button;
    req->clicks = clicks;
    push_req(v, req);
}

static void
start_dropfiles(NsProcView *v, int x, int y, char *paths)
{
    if (!v->opened) {
        g_free(paths);
        return;
    }
    Req *req = g_new0(Req, 1);
    req->type = REQ_DROPFILES;
    req->x = x;
    req->y = y;
    req->paths = paths;
    push_req(v, req);
}

static void
start_viewport(NsProcView *v, int width, int height)
{
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_VIEWPORT;
    req->seq = ++v->viewport_seq;
    req->vw = width;
    req->vh = height;
    req->dpr = page_dpr(v);
    push_req(v, req);
}

static gboolean
maybe_update_viewport(NsProcView *v)
{
    if (!v->opened)
        return FALSE;
    if (viewport_w(v) <= 1 || viewport_h(v) <= 1)
        return FALSE;
    int w = css_viewport_w(v);
    int h = css_viewport_h(v);
    double dpr = page_dpr(v);
    if (w == v->last_vp_w && h == v->last_vp_h && dpr == v->last_vp_dpr)
        return FALSE;
    gtk_widget_queue_draw(v->area);
    v->last_vp_w = w;
    v->last_vp_h = h;
    v->last_vp_dpr = dpr;
    start_viewport(v, w, h);
    return TRUE;
}

static void
push_history(NsProcView *v, const char *url)
{
    if (!url || !*url)
        return;
    if (v->hist_index >= 0 &&
        g_strcmp0(g_ptr_array_index(v->history, v->hist_index), url) == 0)
        return;
    while ((int)v->history->len > v->hist_index + 1)
        g_ptr_array_remove_index(v->history, v->history->len - 1);
    g_ptr_array_add(v->history, g_strdup(url));
    v->hist_index = (int)v->history->len - 1;
    post_emit(v, NS_PROC_EVT_HISTORY, NULL);
}

static gboolean
pv_same_document_url(const char *from, const char *to)
{
    if (!from || !to) return FALSE;
    gsize from_len = strcspn(from, "#"), to_len = strcspn(to, "#");
    if (from_len == to_len && strncmp(from, to, from_len) == 0) return TRUE;
    return ns_url_same_origin(from, to);
}

static void
pv_follow_same_document_state(NsProcView *v, const char *url, gboolean pushed,
                              const char *title)
{
    if (url && *url && g_strcmp0(url, v->current_url) != 0 &&
        pv_same_document_url(v->current_url, url)) {
        if (pushed || v->hist_index < 0) {
            push_history(v, url);
        } else {
            g_free(g_ptr_array_index(v->history, v->hist_index));
            g_ptr_array_index(v->history, v->hist_index) = g_strdup(url);
        }
        g_free(v->current_url);
        v->current_url = g_strdup(url);
        post_emit(v, NS_PROC_EVT_URL, v->current_url);
    }
    if (title && g_strcmp0(title, v->current_title) != 0) {
        g_free(v->current_title);
        v->current_title = g_strdup(title);
        post_emit(v, NS_PROC_EVT_TITLE, v->current_title);
    }
}

static void pv_perm_resolve(NsProcView *v, gboolean allow);

static gboolean
req_outlived_by_load(const Req *req)
{
    switch (req->type) {
    case REQ_LOAD: case REQ_RENDER: case REQ_TICK: case REQ_LINK:
    case REQ_CLICK: case REQ_VIEWPORT: case REQ_KEY: case REQ_HOVER:
    case REQ_RELEASE: case REQ_FIND: case REQ_FAVICON:
        return TRUE;
    default:
        return FALSE;
    }
}

static void
drop_stale_requests(NsProcView *v)
{
    GQueue keep = G_QUEUE_INIT;
    g_async_queue_lock(v->queue);
    Req *req;
    while ((req = g_async_queue_try_pop_unlocked(v->queue))) {
        if (req_outlived_by_load(req))
            req_free(req);
        else
            g_queue_push_tail(&keep, req);
    }
    while ((req = g_queue_pop_head(&keep)))
        g_async_queue_push_unlocked(v->queue, req);
    g_async_queue_unlock(v->queue);
}

static void
do_load_full(NsProcView *v, const char *url, gboolean record,
             gboolean history, gboolean user_activated, gboolean reload)
{
    if (!url || !*url)
        return;
    pv_perm_resolve(v, FALSE);
    pv_audio_shutdown(v);
    drop_stale_requests(v);
    v->pending_record = record;
    int seq = g_atomic_int_add(&v->load_seq, 1) + 1;
    ++v->render_seq;
    ++v->link_seq;
    ++v->click_seq;
    ++v->viewport_seq;
    ++v->key_seq;
    ++v->select_seq;
    ++v->hover_seq;
    v->render_pending = FALSE;
    v->render_inflight = FALSE;
    v->tick_pending = FALSE;
    v->tick_inflight = FALSE;
    v->link_inflight = FALSE;
    v->link_pending = FALSE;
    v->link_pending_action = ACT_HOVER;
    v->hover_inflight = FALSE;
    v->hover_pending = FALSE;
    v->has_selection = FALSE;
    ++v->find_seq;
    if (v->search_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(v->search_revealer), FALSE);
    if (v->search_label)
        gtk_label_set_text(GTK_LABEL(v->search_label), "");
    v->opened = FALSE;
    v->page_animating = FALSE;
    v->caret_blinking = FALSE;
    stop_wheel_animation(v);
    disarm_anim(v);
    g_clear_object(&v->frame);
    ns_page_layers_reset(v->layers);
    v->tiles_mode = FALSE;
    gtk_widget_queue_draw(v->area);
    if (!v->loading) {
        v->loading = TRUE;
        post_emit(v, NS_PROC_EVT_LOADING, "1");
    }
    set_busy_cursor(v);
    g_free(v->pending_url);
    v->pending_url = g_strdup(url);
    if (user_activated && g_strcmp0(url, v->current_url) != 0)
        post_emit(v, NS_PROC_EVT_URL, url);
    post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Loading…"));

    int vw = gtk_widget_get_width(v->area);
    int vh = gtk_widget_get_height(v->area);
    if (vw <= 1 || vh <= 1) {
        g_free(v->deferred_url);
        v->deferred_url = g_strdup(url);
        v->deferred_record = record;
        v->deferred_history = history;
        v->deferred_user_activated = user_activated;
        return;
    }
    v->last_vp_w = css_viewport_w(v);
    v->last_vp_h = css_viewport_h(v);
    v->last_vp_dpr = page_dpr(v);

    Req *req = g_new0(Req, 1);
    req->type = REQ_LOAD;
    req->seq = seq;
    req->url = g_strdup(url);
    req->vw = v->last_vp_w;
    req->vh = v->last_vp_h;
    req->dpr = v->last_vp_dpr;
    req->history = history;
    req->user_activated = user_activated;
    req->reload = reload;
    push_req(v, req);
}

static void
do_load(NsProcView *v, const char *url, gboolean record, gboolean history,
        gboolean user_activated)
{
    do_load_full(v, url, record, history, user_activated, FALSE);
}

void
ns_proc_view_load(NsProcView *v, const char *url)
{
    v->render_restarts = 0;
    do_load(v, url, TRUE, FALSE, TRUE);
}

gboolean ns_proc_view_can_back(NsProcView *v) { return v->hist_index > 0; }

gboolean
ns_proc_view_can_forward(NsProcView *v)
{
    return v->hist_index >= 0 && v->hist_index < (int)v->history->len - 1;
}

void
ns_proc_view_back(NsProcView *v)
{
    if (!ns_proc_view_can_back(v))
        return;
    v->hist_index--;
    v->render_restarts = 0;
    post_emit(v, NS_PROC_EVT_HISTORY, NULL);
    do_load(v, g_ptr_array_index(v->history, v->hist_index), FALSE, TRUE, TRUE);
}

void
ns_proc_view_forward(NsProcView *v)
{
    if (!ns_proc_view_can_forward(v))
        return;
    v->hist_index++;
    v->render_restarts = 0;
    post_emit(v, NS_PROC_EVT_HISTORY, NULL);
    do_load(v, g_ptr_array_index(v->history, v->hist_index), FALSE, TRUE, TRUE);
}

static void
restart_wedged_renderer(NsProcView *v)
{
    g_mutex_lock(&v->proc_lock);
    if (v->proc && v->busy_since_us > 0 && v->busy_type != REQ_PRINT &&
        v->busy_type != REQ_EXPORT &&
        g_get_monotonic_time() - v->busy_since_us >=
            NS_PROC_RELOAD_RESTART_US) {
        ns_rproc_http_interrupt(v->proc);
        ns_rproc_http_terminate(v->proc);
    }
    g_mutex_unlock(&v->proc_lock);
}

void
ns_proc_view_reload(NsProcView *v)
{
    v->render_restarts = 0;
    char *url = NULL;
    if (v->pending_url)
        url = g_strdup(v->pending_url);
    else if (v->hist_index >= 0 && v->hist_index < (int)v->history->len)
        url = g_strdup(g_ptr_array_index(v->history, v->hist_index));
    else if (v->current_url)
        url = g_strdup(v->current_url);
    if (!url)
        return;
    gboolean record = v->pending_url != NULL && v->pending_record;
    do_load_full(v, url, record, FALSE, TRUE, TRUE);
    g_free(url);
    restart_wedged_renderer(v);
}

void
ns_proc_view_toggle_console(NsProcView *v)
{
    if (v->opened)
        console_set_open(v, !v->console_open);
}

void
ns_proc_view_exit_fullscreen(NsProcView *v)
{
    if (!v || v->closed || !v->opened) return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_EVAL;
    req->query = g_strdup("document.exitFullscreen()");
    push_req(v, req);
}

const char *ns_proc_view_url(NsProcView *v) { return v->current_url; }

const char *
ns_proc_view_display_url(NsProcView *v)
{
    return v->loading && v->pending_url ? v->pending_url : v->current_url;
}
const char *ns_proc_view_title(NsProcView *v) { return v->current_title; }
int ns_proc_view_security(NsProcView *v) { return v ? v->security : 0; }
const char *ns_proc_view_remote_ip(NsProcView *v) { return v ? v->remote_ip : NULL; }
gboolean ns_proc_view_is_loading(NsProcView *v) { return v->loading; }
GdkPaintable *ns_proc_view_favicon(NsProcView *v) { return v ? v->favicon : NULL; }

int
ns_proc_view_renderer_pid(NsProcView *v)
{
    if (!v) return -1;
    g_mutex_lock(&v->proc_lock);
    int pid = v->proc ? ns_rproc_http_pid(v->proc) : -1;
    g_mutex_unlock(&v->proc_lock);
    return pid;
}

static int
pv_subprocess_pid(GSubprocess *proc)
{
    if (!proc) return -1;
    const char *id = g_subprocess_get_identifier(proc);
    return id ? atoi(id) : -1;
}

int
ns_proc_view_audio_pid(NsProcView *v)
{
    return v ? pv_subprocess_pid(v->audio_proc) : -1;
}

void
ns_proc_view_end_task(NsProcView *v)
{
    if (!v) return;
    g_mutex_lock(&v->proc_lock);
    if (v->proc) {
        ns_rproc_http_interrupt(v->proc);
        ns_rproc_http_terminate(v->proc);
    }
    g_mutex_unlock(&v->proc_lock);
}

void
ns_proc_view_stop_audio(NsProcView *v)
{
    if (v) pv_audio_shutdown(v);
}

void ns_proc_view_focus(NsProcView *v)
{
    if (v->area)
        gtk_widget_grab_focus(v->area);
}

static void
set_zoom(NsProcView *v, double scale)
{
    int permille = (int)(scale * 1000.0 + 0.5);
    if (permille < NS_PV_ZOOM_MIN_PERMILLE)
        permille = NS_PV_ZOOM_MIN_PERMILLE;
    if (permille > NS_PV_ZOOM_MAX_PERMILLE)
        permille = NS_PV_ZOOM_MAX_PERMILLE;
    double clamped = permille / 1000.0;
    if (clamped == cur_scale(v))
        return;
    v->scale = clamped;
    char percent[16];
    g_snprintf(percent, sizeof percent, "%d", permille / 10);
    post_emit(v, NS_PROC_EVT_ZOOM, percent);
    if (v->opened && !maybe_update_viewport(v)) {
        configure_adjustments(v);
        request_render(v);
    }
}

static const int k_zoom_ladder_percent[] = {
    25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300,
    400, 500
};

static void
zoom_step(NsProcView *v, int direction)
{
    int now = (int)(cur_scale(v) * 100.0 + 0.5);
    gsize n = G_N_ELEMENTS(k_zoom_ladder_percent);
    if (direction > 0) {
        for (gsize i = 0; i < n; i++)
            if (k_zoom_ladder_percent[i] > now) {
                set_zoom(v, k_zoom_ladder_percent[i] / 100.0);
                return;
            }
        set_zoom(v, k_zoom_ladder_percent[n - 1] / 100.0);
        return;
    }
    for (gsize i = n; i-- > 0; )
        if (k_zoom_ladder_percent[i] < now) {
            set_zoom(v, k_zoom_ladder_percent[i] / 100.0);
            return;
        }
    set_zoom(v, k_zoom_ladder_percent[0] / 100.0);
}

void ns_proc_view_zoom_in(NsProcView *v)  { zoom_step(v, 1); }
void ns_proc_view_zoom_out(NsProcView *v) { zoom_step(v, -1); }
void ns_proc_view_zoom_reset(NsProcView *v) { set_zoom(v, 1.0); }

int
ns_proc_view_zoom_percent(NsProcView *v)
{
    return v ? (int)(cur_scale(v) * 100.0 + 0.5) : 100;
}

static void
pv_perm_resolve(NsProcView *v, gboolean allow)
{
    if (!v->perm_pending)
        return;
    v->perm_pending = FALSE;
    ReqType kind = v->perm_kind;
    char *origin = v->perm_origin;
    v->perm_origin = NULL;
    if (v->perm_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(v->perm_revealer), FALSE);
    if (!v->closed) {
        Req *req = g_new0(Req, 1);
        req->type = kind;
        req->url = g_strdup(origin);
        req->mods = allow ? 1 : 0;
        push_req(v, req);
        if (kind == REQ_WEBGL && allow && v->current_url)
            do_load(v, v->current_url, FALSE, FALSE, TRUE);
    }
    g_free(origin);
}

static void
on_perm_allow(GtkButton *btn, gpointer data)
{
    (void)btn;
    pv_perm_resolve(data, TRUE);
}

static void
on_perm_deny(GtkButton *btn, gpointer data)
{
    (void)btn;
    pv_perm_resolve(data, FALSE);
}

static void
pv_perm_bar_show(NsProcView *v, ReqType kind, const char *origin)
{
    if (v->perm_pending && v->perm_kind == kind &&
        g_strcmp0(v->perm_origin, origin) == 0)
        return;
    if (v->perm_pending)
        pv_perm_resolve(v, FALSE);
    v->perm_pending = TRUE;
    v->perm_kind = kind;
    v->perm_origin = g_strdup(origin);
    const char *what =
        kind == REQ_WEBGL
            ? ns_i18n("This site wants to use WebGL (3D graphics)")
            : ns_i18n("This site wants to use your camera and microphone");
    char *text = g_strdup_printf("%s — %s", what, origin);
    gtk_label_set_text(GTK_LABEL(v->perm_label), text);
    g_free(text);
    gtk_revealer_set_reveal_child(GTK_REVEALER(v->perm_revealer), TRUE);
}

static void
pv_apply_window_action(NsProcView *v, const char *action)
{
    if (!v || !action) return;
    if (strcmp(action, "fullscreen-enter") == 0 ||
        strcmp(action, "fullscreen-exit") == 0)
        post_emit(v, NS_PROC_EVT_FULLSCREEN, action);
}

static void
pv_popup_poll(NsProcView *v)
{
    if (!v || v->closed || v->popup_poll_pending) return;
    v->popup_poll_pending = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_POPUP;
    req->kind = 0;
    push_req(v, req);
}

void
ns_proc_view_popup_deliver(NsProcView *v, const char *json)
{
    if (!v || v->closed || !json) return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_POPUP;
    req->kind = 1;
    req->query = g_strdup(json);
    push_req(v, req);
    request_render(v);
}

void
ns_proc_view_set_opener(NsProcView *v, const char *token, const char *origin)
{
    if (!v) return;
    g_free(v->opener_token);
    g_free(v->opener_origin);
    v->opener_token = token && *token ? g_strdup(token) : NULL;
    v->opener_origin = origin && *origin ? g_strdup(origin) : NULL;
    g_mutex_lock(&v->proc_lock);
    if (v->proc)
        ns_rproc_http_set_opener(v->proc, v->opener_token, v->opener_origin);
    g_mutex_unlock(&v->proc_lock);
}

static gboolean
on_result(gpointer data)
{
    Res *res = data;
    NsProcView *v = res->view;

    if (res->type == RES_EVAL && res->remote_cb) {
        res->remote_cb(v->closed ? NULL : res->href, res->remote_ud);
        if (!v->closed)
            request_render(v);
        goto done;
    }
    if (res->type == RES_POPUP) {
        v->popup_poll_pending = FALSE;
        if (!v->closed && res->href && *res->href && strcmp(res->href, "[]") != 0)
            post_emit(v, NS_PROC_EVT_POPUP, res->href);
        goto done;
    }
    if (res->type == RES_DOWNLOAD) {
        if (res->remote_cb)
            res->remote_cb(v->closed ? NULL : res->href, res->remote_ud);
        goto done;
    }
    if (res->type == RES_CONSOLE && res->remote_cb) {
        if (res->href && *res->href)
            console_append(v, res->href);
        res->remote_cb(v->closed ? NULL : res->href ? res->href : "",
                       res->remote_ud);
        goto done;
    }

    if (v->closed)
        goto done;

    if (res->type == RES_PAGE) {
        if (res->seq != v->load_seq)
            goto done;
        if (!res->ok) {
            finish_loading(v);
            clear_busy_cursor(v);
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Failed to load page"));
            goto done;
        }
        if (res->nav && *res->nav) {
            if (v->js_redirects < NS_PROC_MAX_JS_REDIRECTS) {
                v->js_redirects++;
                do_load(v, res->nav, v->pending_record, FALSE, FALSE);
                goto done;
            }
            post_emit(v, NS_PROC_EVT_STATUS,
                      ns_i18n("Stopped after too many redirects"));
        }
        v->js_redirects = 0;
        g_free(v->current_url);
        v->current_url = g_strdup(res->url);
        g_clear_pointer(&v->pending_url, g_free);
        g_free(v->current_title);
        v->current_title = g_strdup(res->title);
        v->security = res->security;
        g_free(v->remote_ip);
        v->remote_ip = res->remote_ip ? g_strdup(res->remote_ip) : NULL;
        v->page_w = res->pw;
        v->page_h = res->ph;
        v->scroll_x = 0;
        v->scroll_y = 0;
        v->opened = TRUE;
        configure_adjustments(v);
        if (v->pending_record)
            push_history(v, v->current_url);
        post_emit(v, NS_PROC_EVT_URL, v->current_url);
        post_emit(v, NS_PROC_EVT_TITLE, v->current_title);
        post_emit(v, NS_PROC_EVT_STATUS, "");
        finish_loading(v);
        request_render(v);
        request_favicon(v);
    } else if (res->type == RES_TICK) {
        gboolean current = res->seq == v->load_seq;
        gboolean navigated = FALSE;
        if (current && res->ok && res->nav && *res->nav &&
            v->js_redirects < NS_PROC_MAX_JS_REDIRECTS) {
            v->js_redirects++;
            do_load(v, res->nav, FALSE, FALSE, FALSE);
            navigated = TRUE;
        }
        if (current && res->ok && res->webgl && *res->webgl)
            post_emit(v, NS_PROC_EVT_WEBGL, "1");
        if (current && res->ok && res->camera && *res->camera)
            pv_perm_bar_show(v, REQ_CAMERA, res->camera);
        if (current && res->ok && res->download && *res->download)
            post_emit(v, NS_PROC_EVT_DOWNLOAD, res->download);
        if (res->ok && res->config && *res->config)
            post_emit(v, NS_PROC_EVT_CONFIG, res->config);
        if (current && res->ok && res->audio && *res->audio)
            pv_audio_pump(v, res->audio);
        if (current && res->ok && res->window_action && *res->window_action)
            pv_apply_window_action(v, res->window_action);
        if (res->ok && res->popup)
            pv_popup_poll(v);
        if (current && res->ok && !navigated)
            pv_follow_same_document_state(v, res->url, res->url_pushed,
                                          res->title);
        if (current && res->ok) {
            note_anim(v, res->animating, res->wake_ms);
            if (res->ph > 0 && res->ph != v->page_h) {
                v->page_h = res->ph;
                if (res->pw > 0) v->page_w = res->pw;
                configure_adjustments(v);
            }
            if (res->kind)
                request_render(v);
        }
        v->tick_inflight = FALSE;
        if (!navigated && v->tick_pending && v->opened) {
            v->tick_pending = FALSE;
            request_tick(v);
        }
    } else if (res->type == RES_FRAME) {
        gboolean current = res->seq == v->render_seq;
        if (res->ok && res->wheel_snapped)
            stop_wheel_animation(v);
        if (res->ok && res->wheel_answered)
            pv_wheel_answered(v, res->wheel_prevented);
        if (current && res->ok) {
            v->caret_blinking = res->caret_blinking;
            v->caret_due_us = g_get_monotonic_time() + NS_PROC_CARET_BLINK_US;
            note_anim(v, res->animating, res->wake_ms);
            if (res->ph > 0 && res->ph != v->page_h) {
                v->page_h = res->ph;
                if (res->pw > 0) v->page_w = res->pw;
                gtk_widget_queue_draw(v->area);
            }
            if (res->requested_scroll_y >= 0 || res->requested_scroll_x >= 0) {
                v->adopting_scroll = TRUE;
                configure_adjustments(v);
                if (res->requested_scroll_y >= 0)
                    gtk_adjustment_set_value(v->vadj,
                                             res->requested_scroll_y);
                if (res->requested_scroll_x >= 0)
                    gtk_adjustment_set_value(v->hadj,
                                             res->requested_scroll_x);
                v->adopting_scroll = FALSE;
                v->scroll_x = (int)gtk_adjustment_get_value(v->hadj);
                v->scroll_y = (int)gtk_adjustment_get_value(v->vadj);
            }
        }
        if (current && res->ok && res->layers) {
            pv_tiles_apply(v, res);
        } else if (current && res->ok && res->texture) {
            pv_tiles_leave(v);
            g_clear_object(&v->frame);
            v->frame = g_steal_pointer(&res->texture);
            v->frame_scale = res->texture_scale;
            v->render_restarts = 0;
            gtk_widget_queue_draw(v->area);
            clear_busy_cursor(v);
        } else if (current && res->ok && res->frame_unchanged) {
            v->render_restarts = 0;
        }
        if (current && res->ok && res->nav && *res->nav &&
            v->js_redirects < NS_PROC_MAX_JS_REDIRECTS) {
            v->js_redirects++;
            do_load(v, res->nav, FALSE, FALSE, FALSE);
        }
        if (res->ok && res->webgl && *res->webgl)
            post_emit(v, NS_PROC_EVT_WEBGL, "1");
        if (res->ok && res->camera && *res->camera)
            pv_perm_bar_show(v, REQ_CAMERA, res->camera);
        if (res->ok && res->download && *res->download)
            post_emit(v, NS_PROC_EVT_DOWNLOAD, res->download);
        if (res->ok && res->config && *res->config)
            post_emit(v, NS_PROC_EVT_CONFIG, res->config);
        if (res->ok && res->audio && *res->audio)
            pv_audio_pump(v, res->audio);
        if (res->ok && res->window_action && *res->window_action)
            pv_apply_window_action(v, res->window_action);
        if (res->ok && res->popup)
            pv_popup_poll(v);
        if (res->ok && res->clipboard && v->area) {
            gdk_clipboard_set_text(gtk_widget_get_clipboard(v->area),
                                   res->clipboard);
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Copied to clipboard"));
        }
        v->render_inflight = FALSE;
        if (v->render_pending) {
            v->render_pending = FALSE;
            start_render(v);
        } else if (current && !res->ok && v->current_url) {
            if (v->render_restarts < NS_PROC_MAX_RESTARTS) {
                v->render_restarts++;
                post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Renderer restarted"));
                do_load(v, v->current_url, FALSE, FALSE, FALSE);
            } else {
                post_emit(v, NS_PROC_EVT_STATUS,
                          ns_i18n("This tab's renderer keeps failing — "
                                  "reload to retry"));
                finish_loading(v);
                clear_busy_cursor(v);
            }
        }
    } else if (res->type == RES_VIEWPORT) {
        if (res->seq != v->viewport_seq)
            goto done;
        if (res->ok) {
            v->page_w = res->pw;
            v->page_h = res->ph;
            configure_adjustments(v);
            request_render(v);
        }
    } else if (res->type == RES_SELECT) {
        if (res->seq == v->select_seq)
            request_render(v);
    } else if (res->type == RES_COPY) {
        if (res->href && *res->href && v->area) {
            gdk_clipboard_set_text(gtk_widget_get_clipboard(v->area),
                                   res->href);
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Copied selection"));
        }
        if (res->kind == 7)
            request_render(v);
    } else if (res->type == RES_KEY) {
        if (res->seq != v->key_seq)
            goto done;
        if (res->kind == 0 && res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            ns_proc_view_load(v, res->href);
        } else {
            if (res->fallback_scroll && !res->prevented)
                scroll_view_to(v, res->fallback_x, res->fallback_y);
            request_render(v);
        }
    } else if (res->type == RES_CLICK) {
        if (res->seq != v->click_seq)
            goto done;
        if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            ns_proc_view_load(v, res->href);
        } else {
            request_render(v);
        }
    } else if (res->type == RES_LINK) {
        if (res->seq != v->link_seq)
            goto done;
        v->link_inflight = FALSE;
        gboolean navigated = FALSE;
        GtkWidget *area = v->area;
        if (res->action == ACT_CONTEXT) {
            if (res->prevented || res->edit)
                request_render(v);
            if (!res->prevented)
                show_context_menu(v, res->href, res->edit);
            if (v->link_pending) {
                v->link_pending = FALSE;
                LinkAct a = v->link_pending_action;
                v->link_pending_action = ACT_HOVER;
                start_link(v, v->link_pending_x, v->link_pending_y, a);
            }
            goto done;
        }
        if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            if (!v->busy_cursor)
                pv_set_named_cursor(area, res->cursor ? res->cursor : "pointer");
            if (res->action == ACT_NAVIGATE) {
                navigated = TRUE;
                ns_proc_view_load(v, res->href);
            }
        } else {
            post_emit(v, NS_PROC_EVT_STATUS, "");
            if (!v->busy_cursor)
                pv_set_named_cursor(area, res->cursor);
        }
        if (!navigated && v->link_pending) {
            v->link_pending = FALSE;
            LinkAct a = v->link_pending_action;
            v->link_pending_action = ACT_HOVER;
            start_link(v, v->link_pending_x, v->link_pending_y, a);
        }
    } else if (res->type == RES_HOVER) {
        if (res->seq != v->hover_seq)
            goto done;
        v->hover_inflight = FALSE;
        if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            if (!v->busy_cursor)
                pv_set_named_cursor(v->area, res->cursor ? res->cursor : "pointer");
        } else {
            post_emit(v, NS_PROC_EVT_STATUS, "");
            if (!v->busy_cursor)
                pv_set_named_cursor(v->area, res->cursor);
        }
        if (res->ok)
            request_render(v);
        else if (res->animating && !v->page_animating)
            request_tick(v);
        if (v->hover_pending) {
            v->hover_pending = FALSE;
            start_hover(v, v->hover_pending_x, v->hover_pending_y);
        }
    } else if (res->type == RES_DROPFILES) {
        if (res->ok)
            request_render(v);
    } else if (res->type == RES_SCROLLBAR) {
        if (res->kind == 0) {
            gboolean still = v->sb_probe;
            v->sb_probe = FALSE;
            if (res->ok) {
                request_render(v);
                if (still) {
                    v->sb_dragging = TRUE;
                    if (v->sb_have_last) {
                        double s = cur_scale(v);
                        Req *req = g_new0(Req, 1);
                        req->type = REQ_SCROLLBAR;
                        req->kind = 1;
                        req->x = v->scroll_x + (int)(v->sb_last_x / s);
                        req->y = v->scroll_y + (int)(v->sb_last_y / s);
                        push_req(v, req);
                    }
                }
            } else if (still && v->sb_have_last) {
                double s = cur_scale(v);
                if (!v->multi_click) {
                    start_select(v, 0,
                                 v->scroll_x + (int)(v->drag_start_x / s),
                                 v->scroll_y + (int)(v->drag_start_y / s));
                    v->drag_anchored = TRUE;
                }
                start_select(v, 1, v->scroll_x + (int)(v->sb_last_x / s),
                             v->scroll_y + (int)(v->sb_last_y / s));
                v->has_selection = TRUE;
            }
        } else if (res->kind == 1) {
            if (res->ok)
                request_render(v);
        }
    } else if (res->type == RES_RELEASE) {
        if (res->href && *res->href && res->newtab) {
            post_emit(v, NS_PROC_EVT_NEWTAB, res->href);
            if (res->ok)
                request_render(v);
        } else if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            ns_proc_view_load(v, res->href);
        } else if (res->ok) {
            request_render(v);
        }
    } else if (res->type == RES_FIND) {
        if (res->seq != v->find_seq)
            goto done;
        if (v->search_label) {
            const char *q = v->search_entry
                ? gtk_editable_get_text(GTK_EDITABLE(v->search_entry)) : NULL;
            if (res->find_total > 0) {
                char buf[64];
                g_snprintf(buf, sizeof buf, "%d/%d", res->find_current,
                           res->find_total);
                gtk_label_set_text(GTK_LABEL(v->search_label), buf);
            } else {
                gtk_label_set_text(GTK_LABEL(v->search_label),
                                   (q && *q) ? ns_i18n("No results") : "");
            }
        }
        if (res->find_total > 0) {
            double target = res->find_scroll_y > 40 ? res->find_scroll_y - 40
                                                    : 0;
            gtk_adjustment_set_value(v->vadj, target);
        }
        request_render(v);
    } else if (res->type == RES_EXPORT) {
        if (res->ok && res->url)
            post_emit(v, NS_PROC_EVT_STATUS, res->url);
        else
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Could not save page"));
    } else if (res->type == RES_PRINT) {
        if (res->print_pages && res->print_pages->len > 0) {
            print_run(v, res->print_pages, &res->print_setup,
                      res->print_scale);
            res->print_pages = NULL;
        } else {
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Nothing to print"));
        }
        request_render(v);
    } else if (res->type == RES_CONSOLE) {
        if (res->href && *res->href)
            console_append(v, res->href);
    } else if (res->type == RES_EVAL) {
        if (res->inspect) {
            if (v->elements_buffer)
                gtk_text_buffer_set_text(
                    v->elements_buffer,
                    (res->href && *res->href) ? res->href
                                              : ns_i18n("No matching element"),
                    -1);
        } else if (res->href && *res->href) {
            console_append(v, res->href);
            console_append(v, "\n");
        } else {
            console_append(v, "undefined\n");
        }
        request_render(v);
    } else if (res->type == RES_DUMP) {
        GtkTextBuffer *buf = NULL;
        switch (res->dump_tab) {
        case DEV_TAB_NETWORK:     buf = v->net_buffer;      break;
        case DEV_TAB_PERFORMANCE: buf = v->perf_buffer;     break;
        case DEV_TAB_LAYOUT:      buf = v->layout_buffer;   break;
        case DEV_TAB_ELEMENTS:    buf = v->elements_buffer; break;
        default: break;
        }
        if (buf == v->perf_buffer) {
            GString *text = g_string_new(
                (res->href && *res->href) ? res->href : ns_i18n("(empty)"));
            pv_append_media_process_stats(v, text);
            gtk_text_buffer_set_text(buf, text->str, -1);
            g_string_free(text, TRUE);
        } else if (buf) {
            gtk_text_buffer_set_text(
                buf, (res->href && *res->href) ? res->href : ns_i18n("(empty)"),
                -1);
        }
    } else if (res->type == RES_FAVICON) {
        if (res->seq != v->load_seq)
            goto done;
        g_clear_object(&v->favicon);
        if (res->favicon_data && res->favicon_w > 0 && res->favicon_h > 0) {
            gsize len = (gsize)res->favicon_stride * (gsize)res->favicon_h;
            GBytes *bytes = g_bytes_new(res->favicon_data, len);
            GdkTexture *tex = gdk_memory_texture_new(
                res->favicon_w, res->favicon_h,
                GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, res->favicon_stride);
            g_bytes_unref(bytes);
            v->favicon = GDK_PAINTABLE(tex);
        }
        post_emit(v, NS_PROC_EVT_FAVICON, NULL);
    }

done:
    g_clear_object(&res->texture);
    ns_layer_update_free(res->layers);
    g_free(res->title);
    g_free(res->url);
    g_free(res->nav);
    g_free(res->remote_ip);
    g_free(res->webgl);
    g_free(res->camera);
    g_free(res->download);
    g_free(res->config);
    g_free(res->audio);
    free(res->clipboard);
    g_free(res->window_action);
    free(res->href);
    free(res->cursor);
    free(res->media_url);
    free(res->favicon_data);
    pv_unref(res->view);
    g_free(res);
    return G_SOURCE_REMOVE;
}

G_DECLARE_FINAL_TYPE(NsProcViewArea, ns_proc_view_area, NS, PROC_VIEW_AREA,
                     GtkDrawingArea)

struct _NsProcViewArea {
    GtkDrawingArea parent_instance;
    NsProcView    *view;
};

G_DEFINE_TYPE(NsProcViewArea, ns_proc_view_area, GTK_TYPE_DRAWING_AREA)

static gboolean
pv_frame_covers(NsProcView *v, double fs, const graphene_rect_t *area)
{
    return v->frame &&
        gdk_texture_get_width(v->frame) / fs >= area->size.width - 0.5 &&
        gdk_texture_get_height(v->frame) / fs >= area->size.height - 0.5;
}

static void
ns_proc_view_area_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    NsProcView *v = NS_PROC_VIEW_AREA(widget)->view;
    if (!v) {
        GTK_WIDGET_CLASS(ns_proc_view_area_parent_class)->snapshot(widget, snapshot);
        return;
    }
    if (v->tiles_mode) {
        ns_page_layers_snapshot(v->layers, snapshot,
                                gtk_widget_get_width(widget),
                                gtk_widget_get_height(widget),
                                raster_scale(v), v->scroll_x, v->scroll_y);
        return;
    }
    double fs = v->frame_scale > 0 ? v->frame_scale : 1.0;
    graphene_rect_t area = GRAPHENE_RECT_INIT(
        0, 0, gtk_widget_get_width(widget), gtk_widget_get_height(widget));
    if (!pv_frame_covers(v, fs, &area)) {
        GdkRGBA white = { 1.0f, 1.0f, 1.0f, 1.0f };
        gtk_snapshot_append_color(snapshot, &white, &area);
    }
    if (!v->frame)
        return;
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(
        0, 0, gdk_texture_get_width(v->frame) / fs,
        gdk_texture_get_height(v->frame) / fs);
    gtk_snapshot_push_clip(snapshot, &area);
    gtk_snapshot_append_texture(snapshot, v->frame, &bounds);
    gtk_snapshot_pop(snapshot);
}

static void
ns_proc_view_area_class_init(NsProcViewAreaClass *klass)
{
    GTK_WIDGET_CLASS(klass)->snapshot = ns_proc_view_area_snapshot;
}

static void
ns_proc_view_area_init(NsProcViewArea *self)
{
    self->view = NULL;
}

static void
on_resize(GtkDrawingArea *area, int width, int height, gpointer data)
{
    (void)area;
    NsProcView *v = data;
    if (v->deferred_url && width > 1 && height > 1) {
        char *u = v->deferred_url;
        gboolean rec = v->deferred_record;
        gboolean hist = v->deferred_history;
        gboolean activated = v->deferred_user_activated;
        v->deferred_url = NULL;
        do_load(v, u, rec, hist, activated);
        g_free(u);
        return;
    }
    if (v->opened) {
        if (maybe_update_viewport(v))
            return;
        configure_adjustments(v);
        request_render(v);
    }
}

static gboolean
on_scroll(GtkEventControllerScroll *ctrl, double dx, double dy, gpointer data)
{
    NsProcView *v = data;
    if (!v->opened)
        return FALSE;
    GdkModifierType mods =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(ctrl));
    GdkEvent *ev =
        gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(ctrl));
    if (ev)
        mods |= gdk_event_get_modifier_state(ev);
    if (mods & GDK_CONTROL_MASK) {
        double delta = dy != 0.0 ? dy : dx;
        if (delta < 0)
            ns_proc_view_zoom_in(v);
        else if (delta > 0)
            ns_proc_view_zoom_out(v);
        return TRUE;
    }
    double s = cur_scale(v);
    v->fling_vx = v->fling_vy = 0;
    v->wheel_viewport = FALSE;
    gboolean surface =
        gtk_event_controller_scroll_get_unit(ctrl) == GDK_SCROLL_UNIT_SURFACE;
    double step = surface ? 1.0 : NS_PV_WHEEL_STEP_PX;
    if (pv_queue_wheel_event(v, dx * step / s, dy * step / s, mods))
        return TRUE;
    if (surface) {
        queue_wheel_scroll(v, dx / s, dy / s);
        return TRUE;
    }
    v->wheel_left_x += dx * NS_PV_WHEEL_STEP_PX / s;
    v->wheel_left_y += dy * NS_PV_WHEEL_STEP_PX / s;
    arm_wheel_animation(v);
    return TRUE;
}

static void
on_scroll_decelerate(GtkEventControllerScroll *ctrl, double vel_x,
                     double vel_y, gpointer data)
{
    NsProcView *v = data;
    if (!v->opened ||
        (gtk_event_controller_get_current_event_state(
             GTK_EVENT_CONTROLLER(ctrl)) & GDK_CONTROL_MASK))
        return;
    double s = cur_scale(v);
    v->fling_vx = vel_x / s;
    v->fling_vy = vel_y / s;
    v->wheel_viewport = FALSE;
    arm_wheel_animation(v);
}

typedef struct {
    GPtrArray      *pages;
    ns_print_setup  setup;
    double          raster_scale;
    char           *title;
} PrintJob;

static void
print_job_free(gpointer data)
{
    PrintJob *job = data;
    for (guint i = 0; i < job->pages->len; i++)
        cairo_surface_destroy(g_ptr_array_index(job->pages, i));
    g_ptr_array_free(job->pages, TRUE);
    g_free(job->title);
    g_free(job);
}

static void
on_print_begin(GtkPrintOperation *op, GtkPrintContext *ctx, gpointer data)
{
    (void)ctx;
    PrintJob *job = data;
    gtk_print_operation_set_n_pages(op, (int)job->pages->len);
}

static void
on_print_draw(GtkPrintOperation *op, GtkPrintContext *ctx, int page_nr,
              gpointer data)
{
    (void)op;
    PrintJob *job = data;
    if (page_nr < 0 || (guint)page_nr >= job->pages->len)
        return;
    cairo_t *cr = gtk_print_context_get_cairo_context(ctx);
    double surface_w = gtk_print_context_get_width(ctx);
    double scale = job->setup.width > 0 ? surface_w / job->setup.width : 1.0;
    scale /= job->raster_scale > 0 ? job->raster_scale : 1.0;
    cairo_save(cr);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, g_ptr_array_index(job->pages, page_nr), 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void
print_run(NsProcView *v, GPtrArray *pages, const ns_print_setup *setup,
          double raster_scale)
{
    PrintJob *job = g_new0(PrintJob, 1);
    job->pages = pages;
    job->setup = *setup;
    job->raster_scale = raster_scale > 0 ? raster_scale : 1.0;
    job->title = g_strdup((v->current_title && *v->current_title)
                          ? v->current_title : "page");

    GtkPageSetup *page_setup = gtk_page_setup_new();
    GtkPaperSize *paper = gtk_paper_size_new_custom(
        "northstar", "Northstar", setup->width * 72.0 / 96.0,
        setup->height * 72.0 / 96.0, GTK_UNIT_POINTS);
    gtk_page_setup_set_paper_size(page_setup, paper);
    gtk_page_setup_set_orientation(page_setup,
        setup->width > setup->height ? GTK_PAGE_ORIENTATION_LANDSCAPE
                                     : GTK_PAGE_ORIENTATION_PORTRAIT);
    gtk_paper_size_free(paper);

    GtkPrintOperation *op = gtk_print_operation_new();
    gtk_print_operation_set_default_page_setup(op, page_setup);
    gtk_print_operation_set_use_full_page(op, TRUE);
    gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
    gtk_print_operation_set_job_name(op, job->title);
    gtk_print_operation_set_embed_page_setup(op, TRUE);
    g_signal_connect(op, "begin-print", G_CALLBACK(on_print_begin), job);
    g_signal_connect(op, "draw-page", G_CALLBACK(on_print_draw), job);
    g_object_set_data_full(G_OBJECT(op), "ns-print-job", job, print_job_free);

    GtkRoot *root = v->area ? gtk_widget_get_root(v->area) : NULL;
    GError *err = NULL;
    gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                            GTK_WINDOW(root), &err);
    if (err) {
        post_emit(v, NS_PROC_EVT_STATUS, err->message);
        g_error_free(err);
    }
    g_object_unref(page_setup);
    g_object_unref(op);
}

typedef struct { NsProcView *view; gboolean pdf; } ExportCtx;

static void
on_save_dialog_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    ExportCtx *c = ud;
    NsProcView *v = c->view;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);
    if (file) {
        char *dest = g_file_get_path(file);
        if (dest && v->opened) {
            static int export_counter = 0;
            char *base = g_strdup_printf(
                "nordstjernen-export-%" G_GINT64_FORMAT "-%d.%s",
                g_get_monotonic_time(), ++export_counter,
                c->pdf ? "pdf" : "png");
            Req *req = g_new0(Req, 1);
            req->type = REQ_EXPORT;
            req->url = g_build_filename(g_get_user_runtime_dir(), base, NULL);
            req->export_dest = g_strdup(dest);
            push_req(v, req);
            g_free(base);
        }
        g_free(dest);
        g_object_unref(file);
    }
    g_clear_error(&err);
    pv_unref(v);
    g_free(c);
}

static void
view_save(NsProcView *v, gboolean pdf)
{
    if (!v->opened || !v->area)
        return;
    GtkRoot *root = gtk_widget_get_root(v->area);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog,
        pdf ? ns_i18n("Save page as PDF")
                                      : ns_i18n("Save page as PNG"));
    const char *t = (v->current_title && *v->current_title)
        ? v->current_title : "page";
    char *name = g_strdup_printf("%s.%s", t, pdf ? "pdf" : "png");
    g_strdelimit(name, "/", '_');
    gtk_file_dialog_set_initial_name(dialog, name);
    ExportCtx *c = g_new0(ExportCtx, 1);
    c->view = pv_ref(v);
    c->pdf = pdf;
    gtk_file_dialog_save(dialog, GTK_WINDOW(root), NULL,
                         on_save_dialog_done, c);
    g_object_unref(dialog);
    g_free(name);
}

typedef enum {
    EDIT_NONE, EDIT_CUT, EDIT_COPY, EDIT_PASTE, EDIT_SELECT_ALL
} EditOp;

static void
on_paste_text_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    NsProcView *v = data;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (text && *text && v->opened)
        start_key_text(v, 4, text);
    g_free(text);
    pv_unref(v);
}

static void
paste_clipboard(NsProcView *v)
{
    if (!v->opened || !v->area)
        return;
    gdk_clipboard_read_text_async(gtk_widget_get_clipboard(v->area), NULL,
                                  on_paste_text_ready, pv_ref(v));
}

static EditOp
edit_op_for_key(guint keyval, GdkModifierType state)
{
    GdkModifierType mods = state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK |
                                    GDK_ALT_MASK | GDK_META_MASK);
    if (keyval == GDK_KEY_Insert || keyval == GDK_KEY_KP_Insert) {
        if (mods == GDK_SHIFT_MASK)   return EDIT_PASTE;
        if (mods == GDK_CONTROL_MASK) return EDIT_COPY;
        return EDIT_NONE;
    }
    if (!(mods & NS_PROC_PRIMARY_MASK) ||
        (mods & ~(NS_PROC_PRIMARY_MASK | GDK_SHIFT_MASK)))
        return EDIT_NONE;
    switch (gdk_keyval_to_lower(keyval)) {
    case GDK_KEY_x: return EDIT_CUT;
    case GDK_KEY_c: return EDIT_COPY;
    case GDK_KEY_v: return EDIT_PASTE;
    case GDK_KEY_a: return EDIT_SELECT_ALL;
    default:        return EDIT_NONE;
    }
}

static void
run_edit_op(NsProcView *v, EditOp op)
{
    switch (op) {
    case EDIT_CUT:        start_select(v, 7, 0, 0); break;
    case EDIT_COPY:       start_select(v, 4, 0, 0); break;
    case EDIT_PASTE:      paste_clipboard(v); break;
    case EDIT_SELECT_ALL: start_select(v, 3, 0, 0); break;
    case EDIT_NONE:       break;
    }
}

static void
ctx_set_clipboard(NsProcView *v, const char *text, const char *status)
{
    if (!text || !*text || !v->area)
        return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(v->area), text);
    post_emit(v, NS_PROC_EVT_STATUS, status);
}

static void
on_ctx_back(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; ns_proc_view_back(ud); }

static void
on_ctx_forward(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; ns_proc_view_forward(ud); }

static void
on_ctx_reload(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; ns_proc_view_reload(ud); }

static void
on_ctx_copy_url(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    ctx_set_clipboard(v, v->current_url, ns_i18n("Copied page address"));
}

static void
on_ctx_open_newtab(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    if (v->ctx_link && *v->ctx_link)
        post_emit(v, NS_PROC_EVT_NEWTAB, v->ctx_link);
}

static void
on_ctx_open_link(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    if (v->ctx_link && *v->ctx_link)
        ns_proc_view_load(v, v->ctx_link);
}

static void
on_ctx_copy_link(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    ctx_set_clipboard(v, v->ctx_link, ns_i18n("Copied link address"));
}

static void
on_ctx_cut(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; run_edit_op(ud, EDIT_CUT); }

static void
on_ctx_copy_sel(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; run_edit_op(ud, EDIT_COPY); }

static void
on_ctx_paste(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; run_edit_op(ud, EDIT_PASTE); }

static void
on_ctx_select_all(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    v->has_selection = TRUE;
    start_select(v, 3, 0, 0);
}

void
ns_proc_view_print(NsProcView *view)
{
    if (!view || !view->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_PRINT;
    push_req(view, req);
}

void
ns_proc_view_save_pdf(NsProcView *view)
{ if (view) view_save(view, TRUE); }

void
ns_proc_view_save_image(NsProcView *view)
{ if (view) view_save(view, FALSE); }

static void
on_ctx_save_pdf(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; view_save(ud, TRUE); }

static void
on_ctx_save_png(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; view_save(ud, FALSE); }

static void
ctx_action_enable(NsProcView *v, const char *name, gboolean on)
{
    GAction *act = g_action_map_lookup_action(G_ACTION_MAP(v->ctx_actions),
                                              name);
    if (act)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(act), on);
}

static void
ctx_install_actions(NsProcView *v)
{
    static const GActionEntry entries[] = {
        { "back",        on_ctx_back,        NULL, NULL, NULL, {0} },
        { "forward",     on_ctx_forward,     NULL, NULL, NULL, {0} },
        { "reload",      on_ctx_reload,      NULL, NULL, NULL, {0} },
        { "copy-url",    on_ctx_copy_url,    NULL, NULL, NULL, {0} },
        { "open-link",   on_ctx_open_link,   NULL, NULL, NULL, {0} },
        { "open-newtab", on_ctx_open_newtab, NULL, NULL, NULL, {0} },
        { "copy-link",   on_ctx_copy_link,   NULL, NULL, NULL, {0} },
        { "cut",         on_ctx_cut,         NULL, NULL, NULL, {0} },
        { "copy-sel",    on_ctx_copy_sel,    NULL, NULL, NULL, {0} },
        { "paste",       on_ctx_paste,       NULL, NULL, NULL, {0} },
        { "select-all",  on_ctx_select_all,  NULL, NULL, NULL, {0} },
        { "save-pdf",    on_ctx_save_pdf,    NULL, NULL, NULL, {0} },
        { "save-png",    on_ctx_save_png,    NULL, NULL, NULL, {0} },
    };
    v->ctx_actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(v->ctx_actions), entries,
                                    G_N_ELEMENTS(entries), v);
    gtk_widget_insert_action_group(v->area, "ctx",
                                   G_ACTION_GROUP(v->ctx_actions));
}

static gboolean
popover_present_idle(gpointer data)
{
    GtkWidget *pop = data;
    if (gtk_widget_get_mapped(pop))
        gtk_popover_present(GTK_POPOVER(pop));
    g_object_unref(pop);
    return G_SOURCE_REMOVE;
}

static void
on_popover_menu_mapped(GtkWidget *pop, gpointer user_data)
{
    (void)user_data;
    g_idle_add(popover_present_idle, g_object_ref(pop));
}

void
ns_popover_menu_fit(GtkWidget *popover)
{
    if (!GTK_IS_POPOVER(popover)) return;
    g_signal_connect(popover, "map", G_CALLBACK(on_popover_menu_mapped), NULL);
}

static GMenu *
ctx_field_menu(NsProcView *v, int edit)
{
    gboolean selected = (edit & NS_BROWSER_EDIT_SELECTION) != 0;
    gboolean writable = (edit & NS_BROWSER_EDIT_WRITABLE) != 0;
    ctx_action_enable(v, "cut", writable && selected);
    ctx_action_enable(v, "copy-sel", selected);
    ctx_action_enable(v, "paste", writable);

    GMenu *menu = g_menu_new();
    GMenu *clip = g_menu_new();
    g_menu_append(clip, ns_i18n("Cut"), "ctx.cut");
    g_menu_append(clip, ns_i18n("Copy"), "ctx.copy-sel");
    g_menu_append(clip, ns_i18n("Paste"), "ctx.paste");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(clip));
    g_object_unref(clip);
    GMenu *all = g_menu_new();
    g_menu_append(all, ns_i18n("Select All"), "ctx.select-all");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(all));
    g_object_unref(all);
    return menu;
}

static GMenu *
ctx_page_menu(NsProcView *v)
{
    ctx_action_enable(v, "back", ns_proc_view_can_back(v));
    ctx_action_enable(v, "forward", ns_proc_view_can_forward(v));
    ctx_action_enable(v, "open-link", v->ctx_link != NULL);
    ctx_action_enable(v, "open-newtab", v->ctx_link != NULL);
    ctx_action_enable(v, "copy-link", v->ctx_link != NULL);
    ctx_action_enable(v, "copy-sel", v->has_selection);

    GMenu *menu = g_menu_new();
    if (v->ctx_link) {
        GMenu *s = g_menu_new();
        g_menu_append(s, ns_i18n("Open Link"), "ctx.open-link");
        if (!ns_rproc_single_process_enabled())
            g_menu_append(s, ns_i18n("Open Link in New Tab"), "ctx.open-newtab");
        g_menu_append(s, ns_i18n("Copy Link Address"), "ctx.copy-link");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(s));
        g_object_unref(s);
    }
    if (v->has_selection) {
        GMenu *s = g_menu_new();
        g_menu_append(s, ns_i18n("Copy"), "ctx.copy-sel");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(s));
        g_object_unref(s);
    }
    GMenu *nav = g_menu_new();
    g_menu_append(nav, ns_i18n("Back"), "ctx.back");
    g_menu_append(nav, ns_i18n("Forward"), "ctx.forward");
    g_menu_append(nav, ns_i18n("Reload"), "ctx.reload");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(nav));
    g_object_unref(nav);
    GMenu *page = g_menu_new();
    g_menu_append(page, ns_i18n("Select All"), "ctx.select-all");
    g_menu_append(page, ns_i18n("Copy Page Address"), "ctx.copy-url");
    g_menu_append(page, ns_i18n("Save Page as PDF…"), "ctx.save-pdf");
    g_menu_append(page, ns_i18n("Save Page as Image…"), "ctx.save-png");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(page));
    g_object_unref(page);
    return menu;
}

static void
show_context_menu(NsProcView *v, const char *href, int edit)
{
    g_free(v->ctx_link);
    v->ctx_link = (href && *href) ? g_strdup(href) : NULL;

    GMenu *menu = (edit & NS_BROWSER_EDIT_FIELD) ? ctx_field_menu(v, edit)
                                                 : ctx_page_menu(v);
    if (v->ctx_popover)
        gtk_widget_unparent(v->ctx_popover);
    v->ctx_popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_widget_set_parent(v->ctx_popover, v->area);
    ns_popover_menu_fit(v->ctx_popover);
    gtk_popover_set_has_arrow(GTK_POPOVER(v->ctx_popover), FALSE);
    gtk_popover_set_pointing_to(GTK_POPOVER(v->ctx_popover),
        &(GdkRectangle){ (int)v->ctx_x, (int)v->ctx_y, 1, 1 });
    gtk_popover_popup(GTK_POPOVER(v->ctx_popover));
}

static int
pv_dom_mods(GdkModifierType state)
{
    return ((state & GDK_SHIFT_MASK)   ? 1 : 0) |
           ((state & GDK_CONTROL_MASK) ? 2 : 0) |
           ((state & GDK_ALT_MASK)     ? 4 : 0) |
           ((state & GDK_META_MASK)    ? 8 : 0);
}

static int
pv_dom_buttons(GdkModifierType state)
{
    return ((state & GDK_BUTTON1_MASK) ? 1 : 0) |
           ((state & GDK_BUTTON3_MASK) ? 2 : 0) |
           ((state & GDK_BUTTON2_MASK) ? 4 : 0);
}

static int
pv_gesture_dom_button(GtkGestureClick *gesture)
{
    switch (gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(gesture))) {
    case GDK_BUTTON_MIDDLE:    return 1;
    case GDK_BUTTON_SECONDARY: return 2;
    default:                   return 0;
    }
}

static int
pv_dom_button_bit(int button)
{
    return button == 1 ? 4 : button == 2 ? 2 : 1;
}

static void
pv_press_button(NsProcView *v, GtkGestureClick *gesture, int n_press,
                int px, int py)
{
    GdkModifierType state = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(gesture));
    int button = pv_gesture_dom_button(gesture);
    v->buttons_pressed |= pv_dom_button_bit(button);
    start_click(v, px, py, pv_dom_mods(state), button, n_press);
}

static void
pv_release_button(NsProcView *v, int button, double x, double y, int clicks)
{
    int bit = pv_dom_button_bit(button);
    if (!(v->buttons_pressed & bit))
        return;
    v->buttons_pressed &= ~bit;
    if (!v->opened)
        return;
    double s = cur_scale(v);
    start_release(v, v->scroll_x + (int)(x / s), v->scroll_y + (int)(y / s),
                  button, clicks);
}

static void
on_button_cancel(GtkGesture *gesture, GdkEventSequence *sequence,
                 gpointer data)
{
    (void)sequence;
    NsProcView *v = data;
    pv_release_button(v, pv_gesture_dom_button(GTK_GESTURE_CLICK(gesture)),
                      v->pointer_x, v->pointer_y, 1);
}

static void
on_secondary_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                     gpointer data)
{
    NsProcView *v = data;
    if (!v->opened)
        return;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    v->ctx_x = x;
    v->ctx_y = y;
    double s = cur_scale(v);
    int px = v->scroll_x + (int)(x / s);
    int py = v->scroll_y + (int)(y / s);
    pv_press_button(v, gesture, n_press, px, py);
    request_link(v, px, py, ACT_CONTEXT);
}

static void
request_find(NsProcView *v, int direction)
{
    if (!v->opened || !v->search_entry)
        return;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->search_entry));
    Req *req = g_new0(Req, 1);
    req->type = REQ_FIND;
    req->seq = ++v->find_seq;
    req->query = g_strdup(q ? q : "");
    req->find_dir = direction;
    req->find_from_y = v->scroll_y;
    req->find_case = v->find_case;
    push_req(v, req);
}

static void
search_open(NsProcView *v)
{
    if (!v->search_revealer)
        return;
    gtk_revealer_set_reveal_child(GTK_REVEALER(v->search_revealer), TRUE);
    gtk_widget_grab_focus(v->search_entry);
    gtk_editable_select_region(GTK_EDITABLE(v->search_entry), 0, -1);
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->search_entry));
    if (q && *q)
        request_find(v, 0);
}

static void
search_close(NsProcView *v)
{
    if (!v->search_revealer)
        return;
    gtk_revealer_set_reveal_child(GTK_REVEALER(v->search_revealer), FALSE);
    gtk_label_set_text(GTK_LABEL(v->search_label), "");
    Req *req = g_new0(Req, 1);
    req->type = REQ_FIND;
    req->seq = ++v->find_seq;
    req->query = g_strdup("");
    req->find_from_y = v->scroll_y;
    push_req(v, req);
    gtk_widget_grab_focus(v->area);
}

void
ns_proc_view_find_open(NsProcView *v)
{
    search_open(v);
}

void
ns_proc_view_set_color_scheme(NsProcView *v, gboolean dark)
{
    if (!v) return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_COLOR_SCHEME;
    req->mods = dark ? 1 : 0;
    push_req(v, req);
}

static void
on_search_changed(GtkSearchEntry *e, gpointer data)
{ (void)e; request_find(data, 0); }

static void
on_search_next(GtkWidget *w, gpointer data)
{ (void)w; request_find(data, 1); }

static void
on_search_prev(GtkWidget *w, gpointer data)
{ (void)w; request_find(data, 2); }

gboolean
ns_proc_view_find_close(NsProcView *view)
{
    if (!view || !view->search_revealer ||
        !gtk_revealer_get_reveal_child(GTK_REVEALER(view->search_revealer)))
        return FALSE;
    search_close(view);
    return TRUE;
}

static void
on_search_stop(GtkSearchEntry *e, gpointer data)
{ (void)e; search_close(data); }

static void
on_search_close_clicked(GtkButton *b, gpointer data)
{ (void)b; search_close(data); }

static void
on_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
           gpointer data)
{
    NsProcView *v = data;
    if (!v->opened)
        return;
    GdkModifierType mods =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(gesture));
    gtk_widget_grab_focus(v->area);
    double s = cur_scale(v);
    int px = v->scroll_x + (int)(x / s);
    int py = v->scroll_y + (int)(y / s);
    if (pv_gesture_dom_button(gesture) != 0) {
        pv_press_button(v, gesture, n_press, px, py);
        return;
    }
    if (!(mods & GDK_SHIFT_MASK))
        v->has_selection = FALSE;
    pv_press_button(v, gesture, n_press, px, py);
    v->multi_click = n_press >= 2;
    if (mods & GDK_SHIFT_MASK) {
        start_select(v, 1, px, py);
        v->has_selection = TRUE;
    } else if (n_press == 2) {
        start_select(v, 5, px, py);
        v->has_selection = TRUE;
    } else if (n_press >= 3) {
        start_select(v, 6, px, py);
        v->has_selection = TRUE;
    }
}

static void
on_released(GtkGestureClick *gesture, int n_press, double x, double y,
            gpointer data)
{
    pv_release_button(data, pv_gesture_dom_button(gesture), x, y, n_press);
}

static void
on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer data)
{
    NsProcView *v = data;
    GdkModifierType state = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(ctrl));
    int mods = pv_dom_mods(state), buttons = pv_dom_buttons(state);
    gboolean state_changed = mods != v->pointer_mods ||
                             buttons != v->pointer_buttons;
    v->pointer_mods = mods;
    v->pointer_buttons = buttons;
    if (!state_changed &&
        fabs(x - v->pointer_x) < 0.01 && fabs(y - v->pointer_y) < 0.01)
        return;
    v->pointer_x = x;
    v->pointer_y = y;
    if (v->opened && !v->hover_after_scroll_id) {
        double s = cur_scale(v);
        int px = v->scroll_x + (int)(x / s);
        int py = v->scroll_y + (int)(y / s);
        request_hover(v, px, py);
    }
}

static void
push_scrollbar(NsProcView *v, int kind, int px, int py)
{
    Req *req = g_new0(Req, 1);
    req->type = REQ_SCROLLBAR;
    req->kind = kind;
    req->x = px;
    req->y = py;
    push_req(v, req);
}

static void
on_drag_begin(GtkGestureDrag *g, double sx, double sy, gpointer data)
{
    (void)g;
    NsProcView *v = data;
    v->drag_start_x = sx;
    v->drag_start_y = sy;
    v->drag_anchored = FALSE;
    v->sb_dragging = FALSE;
    v->sb_have_last = FALSE;
    v->sb_probe = FALSE;
    if (v->opened) {
        double s = cur_scale(v);
        v->sb_probe = TRUE;
        push_scrollbar(v, 0, v->scroll_x + (int)(sx / s),
                       v->scroll_y + (int)(sy / s));
    }
}

static void
on_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    (void)g;
    NsProcView *v = data;
    if (!v->opened)
        return;
    double s = cur_scale(v);
    double wx = v->drag_start_x + ox;
    double wy = v->drag_start_y + oy;
    if (v->sb_dragging) {
        push_scrollbar(v, 1, v->scroll_x + (int)(wx / s),
                       v->scroll_y + (int)(wy / s));
        return;
    }
    if (v->sb_probe) {
        v->sb_last_x = wx;
        v->sb_last_y = wy;
        v->sb_have_last = TRUE;
        return;
    }
    if (v->multi_click) {
        start_select(v, 1, v->scroll_x + (int)(wx / s),
                     v->scroll_y + (int)(wy / s));
        return;
    }
    if (!v->drag_anchored) {
        start_select(v, 0, v->scroll_x + (int)(v->drag_start_x / s),
                     v->scroll_y + (int)(v->drag_start_y / s));
        v->drag_anchored = TRUE;
    }
    start_select(v, 1, v->scroll_x + (int)(wx / s),
                 v->scroll_y + (int)(wy / s));
    v->has_selection = TRUE;
}

static void
on_drag_end(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    (void)g;
    (void)ox;
    (void)oy;
    NsProcView *v = data;
    if (v->sb_dragging || v->sb_probe) {
        push_scrollbar(v, 2, 0, 0);
        v->sb_dragging = FALSE;
        v->sb_probe = FALSE;
        v->sb_have_last = FALSE;
    }
}

static gboolean
on_key(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
       GdkModifierType state, gpointer data)
{
    (void)ctrl;
    (void)keycode;
    NsProcView *v = data;
    if (!v->opened)
        return FALSE;
    if (keyval == GDK_KEY_F12) {
        console_set_open(v, !v->console_open);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) &&
        (keyval == GDK_KEY_j || keyval == GDK_KEY_J)) {
        console_set_open(v, !v->console_open);
        return TRUE;
    }
    EditOp edit = edit_op_for_key(keyval, state);
    if (edit != EDIT_NONE) {
        start_key(v, 0, keyval, state);
        run_edit_op(v, edit);
        return TRUE;
    }
    gunichar uc = gdk_keyval_to_unicode(keyval);
    if (uc && uc != ' ' && !g_unichar_iscntrl(uc) &&
        !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_META_MASK))) {
        start_key(v, 0, keyval, state);
        start_key(v, 3, keyval, state);
        return FALSE;
    }
    if (state & GDK_CONTROL_MASK) {
        start_key(v, 0, keyval, state);
        switch (keyval) {
        case GDK_KEY_plus:
        case GDK_KEY_equal:
        case GDK_KEY_KP_Add:      ns_proc_view_zoom_in(v); return TRUE;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract: ns_proc_view_zoom_out(v); return TRUE;
        case GDK_KEY_0:
        case GDK_KEY_KP_0:        ns_proc_view_zoom_reset(v); return TRUE;
        case GDK_KEY_f:
        case GDK_KEY_F:           search_open(v); return TRUE;
        case GDK_KEY_g:
        case GDK_KEY_G:
            request_find(v, (state & GDK_SHIFT_MASK) ? 2 : 1);
            return TRUE;
        case GDK_KEY_p:
        case GDK_KEY_P:           view_save(v, TRUE); return TRUE;
        default: return FALSE;
        }
    }
    if ((state & GDK_ALT_MASK) && !(state & GDK_CONTROL_MASK) &&
        !(state & GDK_META_MASK)) {
        gunichar a = gdk_keyval_to_unicode(keyval);
        if (a && !g_unichar_iscntrl(a)) {
            start_key(v, 0, keyval, state);
            return TRUE;
        }
    }
    double line = 60.0;
    double page = viewport_h(v) / cur_scale(v) - line;
    if (page < line) page = line;
    double vy = scroll_target_y(v);
    double vx = scroll_target_x(v);
    double tx = vx, ty = vy;
    switch (keyval) {
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: start_key(v, 0, keyval, state); return TRUE;
    case GDK_KEY_Down:       ty = vy + line; break;
    case GDK_KEY_Up:         ty = vy - line; break;
    case GDK_KEY_Right:      tx = vx + line; break;
    case GDK_KEY_Left:       tx = vx - line; break;
    case GDK_KEY_Page_Down:
    case GDK_KEY_space:      ty = vy + page; break;
    case GDK_KEY_Page_Up:    ty = vy - page; break;
    case GDK_KEY_Home:       ty = 0; break;
    case GDK_KEY_End:        ty = gtk_adjustment_get_upper(v->vadj); break;
    default:                 start_key(v, 0, keyval, state); return FALSE;
    }
    start_key_full(v, 0, keyval, state, 1, tx, ty);
    if (keyval == GDK_KEY_space) {
        start_key(v, 3, keyval, state);
        return FALSE;
    }
    return TRUE;
}

static void
on_key_released(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                GdkModifierType state, gpointer data)
{
    (void)ctrl;
    (void)keycode;
    NsProcView *v = data;
    if (v->opened)
        start_key(v, 1, keyval, state);
}

static void
on_area_destroy(GtkWidget *widget, gpointer data)
{
    NsProcView *v = data;
    NS_PROC_VIEW_AREA(widget)->view = NULL;
    v->closed = TRUE;
    g_clear_object(&v->im);
    disarm_anim(v);
    if (v->hover_after_scroll_id) {
        g_source_remove(v->hover_after_scroll_id);
        v->hover_after_scroll_id = 0;
    }
    if (v->console_poll_id) {
        g_source_remove(v->console_poll_id);
        v->console_poll_id = 0;
    }
    if (v->console_window) {
        GtkWidget *win = v->console_window;
        v->console_window = NULL;
        v->console_notebook = NULL;
        v->console_entry = NULL;
        v->console_view = NULL;
        v->console_buffer = NULL;
        v->net_view = NULL;
        v->net_buffer = NULL;
        v->perf_view = NULL;
        v->perf_buffer = NULL;
        v->layout_view = NULL;
        v->layout_buffer = NULL;
        v->elements_view = NULL;
        v->elements_buffer = NULL;
        v->inspect_entry = NULL;
        gtk_window_destroy(GTK_WINDOW(win));
    }
    if (v->ctx_popover) {
        gtk_widget_unparent(v->ctx_popover);
        v->ctx_popover = NULL;
    }
    v->area = NULL;
    v->perm_revealer = NULL;
    v->perm_label = NULL;
    Req *req = g_new0(Req, 1);
    req->type = REQ_QUIT;
    push_req(v, req);
    g_mutex_lock(&v->proc_lock);
    if (v->proc)
        ns_rproc_http_interrupt(v->proc);
    g_mutex_unlock(&v->proc_lock);
    if (v->thread) {
        g_thread_join(v->thread);
        v->thread = NULL;
    }
    pv_unref(v);
}

static void
console_append(NsProcView *v, const char *text)
{
    if (!v->console_buffer || !text || !*text)
        return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(v->console_buffer, &end);
    gtk_text_buffer_insert(v->console_buffer, &end, text, -1);
    if (v->console_view) {
        gtk_text_buffer_get_end_iter(v->console_buffer, &end);
        GtkTextMark *m = gtk_text_buffer_create_mark(v->console_buffer, NULL,
                                                     &end, FALSE);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(v->console_view), m);
        gtk_text_buffer_delete_mark(v->console_buffer, m);
    }
}

static gboolean
console_poll_cb(gpointer data)
{
    NsProcView *v = data;
    if (!v->console_open || !v->opened)
        return G_SOURCE_CONTINUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_CONSOLE;
    push_req(v, req);
    return G_SOURCE_CONTINUE;
}

static void
on_console_eval(GtkEntry *entry, gpointer data)
{
    NsProcView *v = data;
    const char *src = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!src || !*src || !v->opened)
        return;
    char *echo = g_strdup_printf("> %s\n", src);
    console_append(v, echo);
    g_free(echo);
    Req *req = g_new0(Req, 1);
    req->type = REQ_EVAL;
    req->query = g_strdup(src);
    push_req(v, req);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

static void build_console_window(NsProcView *v);

static void
console_set_open(NsProcView *v, gboolean open)
{
    if (open && !v->console_window)
        build_console_window(v);
    if (!v->console_window)
        return;
    v->console_open = open;
    if (open) {
        GtkRoot *root = gtk_widget_get_root(v->area);
        if (GTK_IS_WINDOW(root))
            gtk_window_set_transient_for(GTK_WINDOW(v->console_window),
                                         GTK_WINDOW(root));
        gtk_window_present(GTK_WINDOW(v->console_window));
        if (!v->console_poll_id)
            v->console_poll_id = g_timeout_add(NS_PROC_CONSOLE_POLL_MS, console_poll_cb, v);
        gtk_widget_grab_focus(v->console_entry);
    } else {
        gtk_widget_set_visible(v->console_window, FALSE);
        if (v->console_poll_id) {
            g_source_remove(v->console_poll_id);
            v->console_poll_id = 0;
        }
        if (v->area)
            gtk_widget_grab_focus(v->area);
    }
}

static void
console_request_dump(NsProcView *v, int tab)
{
    const char *kind = NULL;
    switch (tab) {
    case DEV_TAB_NETWORK:     kind = "network";     break;
    case DEV_TAB_PERFORMANCE: kind = "performance"; break;
    case DEV_TAB_LAYOUT:      kind = "layout";      break;
    case DEV_TAB_ELEMENTS:    kind = "dom";         break;
    default: return;
    }
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_DUMP;
    req->dump_tab = tab;
    req->query = g_strdup(kind);
    push_req(v, req);
}

static int
console_current_tab(NsProcView *v)
{
    if (!v->console_notebook)
        return DEV_TAB_CONSOLE;
    return gtk_notebook_get_current_page(GTK_NOTEBOOK(v->console_notebook));
}

static void
on_console_clear(GtkButton *b, gpointer data)
{
    (void)b;
    NsProcView *v = data;
    GtkTextBuffer *buf = NULL;
    switch (console_current_tab(v)) {
    case DEV_TAB_CONSOLE:     buf = v->console_buffer;  break;
    case DEV_TAB_NETWORK:     buf = v->net_buffer;      break;
    case DEV_TAB_PERFORMANCE: buf = v->perf_buffer;     break;
    case DEV_TAB_LAYOUT:      buf = v->layout_buffer;   break;
    case DEV_TAB_ELEMENTS:    buf = v->elements_buffer; break;
    default: break;
    }
    if (buf)
        gtk_text_buffer_set_text(buf, "", 0);
}

static void
on_console_refresh(GtkButton *b, gpointer data)
{
    (void)b;
    NsProcView *v = data;
    console_request_dump(v, console_current_tab(v));
}

static void
on_console_notebook_switch(GtkNotebook *nb, GtkWidget *page, guint num,
                           gpointer data)
{
    (void)nb;
    (void)page;
    NsProcView *v = data;
    console_request_dump(v, (int)num);
}

static void
on_inspect_activate(GtkEntry *entry, gpointer data)
{
    NsProcView *v = data;
    const char *sel = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!sel || !*sel || !v->opened)
        return;
    char *esc = g_strescape(sel, NULL);
    char *src = g_strdup_printf(
        "(function(){try{var e=document.querySelector(\"%s\");"
        "if(!e)return \"\";var s=e.outerHTML;"
        "return s.length>20000?s.slice(0,20000)+\"\\n\\u2026(truncated)\":s;}"
        "catch(err){return \"Error: \"+err;}})()",
        esc);
    g_free(esc);
    Req *req = g_new0(Req, 1);
    req->type = REQ_EVAL;
    req->inspect = TRUE;
    req->dump_tab = DEV_TAB_ELEMENTS;
    req->query = src;
    push_req(v, req);
}

static gboolean
on_console_close_request(GtkWindow *win, gpointer data)
{
    (void)win;
    console_set_open(data, FALSE);
    return TRUE;
}

static GtkWidget *
console_make_view(GtkWidget **out_view, GtkTextBuffer **out_buffer,
                  gboolean wrap)
{
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),
                                wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    *out_view = view;
    *out_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    return scroll;
}

static void
console_add_tab(NsProcView *v, GtkWidget *child, const char *title)
{
    gtk_notebook_append_page(GTK_NOTEBOOK(v->console_notebook), child,
                             gtk_label_new(ns_i18n(title)));
}

static void
build_console_window(NsProcView *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(header, "toolbar");
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, ns_i18n("Refresh"));
    set_accessible_label(refresh, ns_i18n("Refresh"));
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_console_refresh), v);
    GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_set_tooltip_text(clear, ns_i18n("Clear"));
    set_accessible_label(clear, ns_i18n("Clear"));
    g_signal_connect(clear, "clicked", G_CALLBACK(on_console_clear), v);
    gtk_box_append(GTK_BOX(header), spacer);
    gtk_box_append(GTK_BOX(header), refresh);
    gtk_box_append(GTK_BOX(header), clear);

    v->console_notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(v->console_notebook, TRUE);

    GtkWidget *console_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *console_scroll =
        console_make_view(&v->console_view, &v->console_buffer, TRUE);
    v->console_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(v->console_entry),
                                   ns_i18n("Evaluate JavaScript and press Enter"));
    g_signal_connect(v->console_entry, "activate",
                     G_CALLBACK(on_console_eval), v);
    gtk_box_append(GTK_BOX(console_page), console_scroll);
    gtk_box_append(GTK_BOX(console_page), v->console_entry);
    console_add_tab(v, console_page, "Console");

    console_add_tab(v, console_make_view(&v->net_view, &v->net_buffer, FALSE),
                    "Network");
    console_add_tab(v, console_make_view(&v->perf_view, &v->perf_buffer, FALSE),
                    "Performance");
    console_add_tab(v,
                    console_make_view(&v->layout_view, &v->layout_buffer, FALSE),
                    "Layout");

    GtkWidget *elements_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *elements_scroll =
        console_make_view(&v->elements_view, &v->elements_buffer, FALSE);
    v->inspect_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(v->inspect_entry),
        ns_i18n("Inspect: CSS selector, then Enter"));
    g_signal_connect(v->inspect_entry, "activate",
                     G_CALLBACK(on_inspect_activate), v);
    gtk_box_append(GTK_BOX(elements_page), elements_scroll);
    gtk_box_append(GTK_BOX(elements_page), v->inspect_entry);
    console_add_tab(v, elements_page, "Elements");

    g_signal_connect(v->console_notebook, "switch-page",
                     G_CALLBACK(on_console_notebook_switch), v);

    gtk_box_append(GTK_BOX(box), header);
    gtk_box_append(GTK_BOX(box), v->console_notebook);

    v->console_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(v->console_window),
                         ns_i18n("Developer Tools"));
    gtk_window_set_default_size(GTK_WINDOW(v->console_window), 720, 420);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(v->console_window), TRUE);
    gtk_window_set_child(GTK_WINDOW(v->console_window), box);
    g_signal_connect(v->console_window, "close-request",
                     G_CALLBACK(on_console_close_request), v);
}

static void
build_search_bar(NsProcView *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(box, "toolbar");

    v->search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(v->search_entry, 220, -1);
    set_accessible_label(v->search_entry, ns_i18n("Find in page"));
    g_signal_connect(v->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), v);
    g_signal_connect(v->search_entry, "activate",
                     G_CALLBACK(on_search_next), v);
    g_signal_connect(v->search_entry, "next-match",
                     G_CALLBACK(on_search_next), v);
    g_signal_connect(v->search_entry, "previous-match",
                     G_CALLBACK(on_search_prev), v);
    g_signal_connect(v->search_entry, "stop-search",
                     G_CALLBACK(on_search_stop), v);

    v->search_label = gtk_label_new("");
    gtk_widget_set_size_request(v->search_label, 56, -1);

    GtkWidget *prev = gtk_button_new_from_icon_name("go-up-symbolic");
    GtkWidget *next = gtk_button_new_from_icon_name("go-down-symbolic");
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(prev, ns_i18n("Previous match (Shift+Enter)"));
    gtk_widget_set_tooltip_text(next, ns_i18n("Next match (Enter)"));
    gtk_widget_set_tooltip_text(close, ns_i18n("Close (Esc)"));
    set_accessible_label(prev, ns_i18n("Previous match"));
    set_accessible_label(next, ns_i18n("Next match"));
    set_accessible_label(close, ns_i18n("Close search"));
    g_signal_connect(prev, "clicked", G_CALLBACK(on_search_prev), v);
    g_signal_connect(next, "clicked", G_CALLBACK(on_search_next), v);
    g_signal_connect(close, "clicked", G_CALLBACK(on_search_close_clicked), v);

    gtk_box_append(GTK_BOX(box), v->search_entry);
    gtk_box_append(GTK_BOX(box), v->search_label);
    gtk_box_append(GTK_BOX(box), prev);
    gtk_box_append(GTK_BOX(box), next);
    gtk_box_append(GTK_BOX(box), close);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_widget_add_css_class(frame, "ns-findbar");
    gtk_frame_set_child(GTK_FRAME(frame), box);

    v->search_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(v->search_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_child(GTK_REVEALER(v->search_revealer), frame);
    gtk_widget_set_halign(v->search_revealer, GTK_ALIGN_END);
    gtk_widget_set_valign(v->search_revealer, GTK_ALIGN_START);
    gtk_widget_set_margin_top(v->search_revealer, 8);
    gtk_widget_set_margin_end(v->search_revealer, 20);
    gtk_overlay_add_overlay(GTK_OVERLAY(v->overlay), v->search_revealer);
}

static void
build_perm_bar(NsProcView *v)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);

    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-question-symbolic");

    v->perm_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(v->perm_label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(v->perm_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(v->perm_label, TRUE);

    GtkWidget *allow = gtk_button_new_with_label(ns_i18n("Allow"));
    gtk_widget_add_css_class(allow, "suggested-action");
    gtk_widget_set_tooltip_text(allow, ns_i18n("Allow and trust this site"));
    GtkWidget *deny = gtk_button_new_with_label(ns_i18n("Not now"));
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    set_accessible_label(close, ns_i18n("Dismiss"));
    g_signal_connect(allow, "clicked", G_CALLBACK(on_perm_allow), v);
    g_signal_connect(deny, "clicked", G_CALLBACK(on_perm_deny), v);
    g_signal_connect(close, "clicked", G_CALLBACK(on_perm_deny), v);

    gtk_box_append(GTK_BOX(bar), icon);
    gtk_box_append(GTK_BOX(bar), v->perm_label);
    gtk_box_append(GTK_BOX(bar), allow);
    gtk_box_append(GTK_BOX(bar), deny);
    gtk_box_append(GTK_BOX(bar), close);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), bar);

    v->perm_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(v->perm_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_child(GTK_REVEALER(v->perm_revealer), frame);
}

static gboolean
on_file_drop(GtkDropTarget *target, const GValue *value, double x, double y,
             gpointer data)
{
    (void)target;
    NsProcView *v = data;
    if (!v->opened || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;
    GdkFileList *fl = g_value_get_boxed(value);
    if (!fl)
        return FALSE;
    GSList *files = gdk_file_list_get_files(fl);
    GString *paths = g_string_new(NULL);
    for (GSList *l = files; l; l = l->next) {
        char *p = g_file_get_path(G_FILE(l->data));
        if (!p)
            continue;
        if (paths->len)
            g_string_append_c(paths, '\n');
        g_string_append(paths, p);
        g_free(p);
    }
    g_slist_free(files);
    if (paths->len == 0) {
        g_string_free(paths, TRUE);
        return FALSE;
    }
    double s = cur_scale(v);
    int px = v->scroll_x + (int)(x / s);
    int py = v->scroll_y + (int)(y / s);
    start_dropfiles(v, px, py, g_string_free(paths, FALSE));
    return TRUE;
}

NsProcView *
ns_proc_view_new(void)
{
    NsProcView *v = g_new0(NsProcView, 1);
    g_ref_count_init(&v->rc);
    g_mutex_init(&v->proc_lock);
    v->renderer_path = ns_proc_renderer_path();
    v->queue = g_async_queue_new();
    v->history = g_ptr_array_new_with_free_func(g_free);
    v->hist_index = -1;
    v->link_pending_action = ACT_HOVER;
    v->pending_record = TRUE;
    v->scale = 1.0;

    v->hadj = gtk_adjustment_new(0, 0, 1, 60, 60, 1);
    v->vadj = gtk_adjustment_new(0, 0, 1, 60, 60, 1);
    g_signal_connect(v->hadj, "value-changed", G_CALLBACK(on_adj_changed), v);
    g_signal_connect(v->vadj, "value-changed", G_CALLBACK(on_adj_changed), v);

    v->layers = ns_page_layers_new();
    v->area = g_object_new(ns_proc_view_area_get_type(), NULL);
    NS_PROC_VIEW_AREA(v->area)->view = v;
    gtk_widget_set_hexpand(v->area, TRUE);
    gtk_widget_set_vexpand(v->area, TRUE);
    gtk_widget_set_focusable(v->area, TRUE);
    g_signal_connect(v->area, "resize", G_CALLBACK(on_resize), v);
    g_signal_connect(v->area, "notify::has-focus",
                     G_CALLBACK(on_area_focus_notify), v);
    g_signal_connect(v->area, "notify::scale-factor",
                     G_CALLBACK(on_device_scale_changed), v);
    g_signal_connect(v->area, "realize", G_CALLBACK(on_area_realize), v);
    g_signal_connect(v->area, "map", G_CALLBACK(on_area_map), v);
    g_signal_connect(v->area, "unrealize", G_CALLBACK(on_area_unrealize), v);

    GtkWidget *grid = gtk_grid_new();
    v->vscroll =
        gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, v->vadj);
    v->hscroll =
        gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, v->hadj);
    gtk_grid_attach(GTK_GRID(grid), v->area, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v->vscroll, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v->hscroll, 0, 1, 1, 1);
    gtk_widget_set_visible(v->vscroll, FALSE);
    gtk_widget_set_visible(v->hscroll, FALSE);

    v->overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(v->overlay), grid);
    gtk_widget_set_vexpand(v->overlay, TRUE);
    build_perm_bar(v);
    v->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(v->root), v->perm_revealer);
    gtk_box_append(GTK_BOX(v->root), v->overlay);
    gtk_widget_set_vexpand(v->root, TRUE);
    build_search_bar(v);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), v);
    g_signal_connect(click, "released", G_CALLBACK(on_released), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), v);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), v);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(drag));

    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST,
                                              GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_file_drop), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(drop));

    GtkGesture *middle = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), GDK_BUTTON_MIDDLE);
    g_signal_connect(middle, "pressed", G_CALLBACK(on_pressed), v);
    g_signal_connect(middle, "released", G_CALLBACK(on_released), v);
    g_signal_connect(middle, "cancel", G_CALLBACK(on_button_cancel), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(middle));

    ctx_install_actions(v);
    GtkGesture *secondary = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(secondary, "pressed", G_CALLBACK(on_secondary_pressed), v);
    g_signal_connect(secondary, "released", G_CALLBACK(on_released), v);
    g_signal_connect(secondary, "cancel", G_CALLBACK(on_button_cancel), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(secondary));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), v);
    gtk_widget_add_controller(v->area, motion);

    GtkEventController *scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES |
        GTK_EVENT_CONTROLLER_SCROLL_KINETIC);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), v);
    g_signal_connect(scroll, "decelerate", G_CALLBACK(on_scroll_decelerate), v);
    gtk_widget_add_controller(v->area, scroll);

    GtkEventController *key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), v);
    g_signal_connect(key, "key-released", G_CALLBACK(on_key_released), v);
    gtk_widget_add_controller(v->area, key);
    GtkEventController *text_input = gtk_event_controller_key_new();
    v->im = gtk_im_multicontext_new();
    gtk_im_context_set_client_widget(v->im, v->area);
    g_signal_connect(v->im, "commit", G_CALLBACK(on_im_commit), v);
    gtk_event_controller_key_set_im_context(
        GTK_EVENT_CONTROLLER_KEY(text_input), v->im);
    gtk_widget_add_controller(v->area, text_input);

    g_signal_connect(v->area, "destroy", G_CALLBACK(on_area_destroy), v);

    v->thread = g_thread_new("ns-proc-view", worker_main, pv_ref(v));
    return v;
}

GtkWidget *ns_proc_view_widget(NsProcView *v) { return v->root; }

void
ns_proc_view_set_notify(NsProcView *v, NsProcNotify cb, gpointer ud)
{
    v->notify = cb;
    v->notify_ud = ud;
}

void
ns_proc_view_set_private(NsProcView *v, gboolean private_mode)
{
    if (v)
        v->private_mode = private_mode;
}

gboolean
ns_proc_view_is_private(NsProcView *v)
{
    return v && v->private_mode;
}

void
ns_proc_view_remote_eval(NsProcView *v, const char *js, NsProcViewTextFn cb,
                         gpointer user_data)
{
    if (!v || v->closed) {
        cb(NULL, user_data);
        return;
    }
    Req *req = g_new0(Req, 1);
    req->type = REQ_EVAL;
    req->query = g_strdup(js ? js : "");
    req->remote_cb = cb;
    req->remote_ud = user_data;
    push_req(v, req);
}

NsProcView *
ns_proc_view_ref(NsProcView *v)
{
    return v ? pv_ref(v) : NULL;
}

void
ns_proc_view_unref(NsProcView *v)
{
    if (v) pv_unref(v);
}

gboolean
ns_proc_view_is_closed(NsProcView *v)
{
    return !v || v->closed;
}

static void
push_download_req(NsProcView *v, int kind, const char *url, long id,
                  NsProcViewTextFn cb, gpointer user_data)
{
    if (!v || v->closed) {
        if (cb) cb(NULL, user_data);
        return;
    }
    Req *req = g_new0(Req, 1);
    req->type = REQ_DOWNLOAD;
    req->kind = kind;
    req->query = g_strdup(url ? url : "");
    req->download_id = id;
    req->remote_cb = cb;
    req->remote_ud = user_data;
    push_req(v, req);
}

void
ns_proc_view_download_start(NsProcView *v, const char *url,
                            NsProcViewTextFn cb, gpointer user_data)
{
    push_download_req(v, 0, url, 0, cb, user_data);
}

void
ns_proc_view_download_poll(NsProcView *v, long id, NsProcViewTextFn cb,
                           gpointer user_data)
{
    push_download_req(v, 1, NULL, id, cb, user_data);
}

void
ns_proc_view_download_cancel(NsProcView *v, long id)
{
    push_download_req(v, 2, NULL, id, NULL, NULL);
}

void
ns_proc_view_remote_console(NsProcView *v, NsProcViewTextFn cb,
                            gpointer user_data)
{
    if (!v || v->closed) {
        cb(NULL, user_data);
        return;
    }
    Req *req = g_new0(Req, 1);
    req->type = REQ_CONSOLE;
    req->remote_cb = cb;
    req->remote_ud = user_data;
    push_req(v, req);
}

static void
remote_to_page(NsProcView *v, double x, double y, gboolean page_coords,
               int *px, int *py)
{
    *px = page_coords ? (int)x : v->scroll_x + (int)x;
    *py = page_coords ? (int)y : v->scroll_y + (int)y;
}

gboolean
ns_proc_view_remote_click(NsProcView *v, double x, double y,
                          gboolean page_coords, int count, int mods)
{
    if (!v || v->closed || !v->opened)
        return FALSE;
    int px, py;
    remote_to_page(v, x, y, page_coords, &px, &py);
    double s = cur_scale(v);
    v->pointer_x = (px - v->scroll_x) * s;
    v->pointer_y = (py - v->scroll_y) * s;
    gtk_widget_grab_focus(v->area);
    if (count < 1)
        count = 1;
    for (int n = 1; n <= count && n <= 3; n++) {
        if (!(mods & 1))
            v->has_selection = FALSE;
        start_click(v, px, py, mods, 0, n);
        v->multi_click = n >= 2;
        if (n == 2 || n == 3) {
            start_select(v, n == 2 ? 5 : 6, px, py);
            v->has_selection = TRUE;
        }
        start_release(v, px, py, 0, n);
    }
    return TRUE;
}

gboolean
ns_proc_view_remote_hover(NsProcView *v, double x, double y,
                          gboolean page_coords)
{
    if (!v || v->closed || !v->opened)
        return FALSE;
    int px, py;
    remote_to_page(v, x, y, page_coords, &px, &py);
    double s = cur_scale(v);
    v->pointer_x = (px - v->scroll_x) * s;
    v->pointer_y = (py - v->scroll_y) * s;
    request_hover(v, px, py);
    return TRUE;
}

gboolean
ns_proc_view_remote_key(NsProcView *v, guint keyval, GdkModifierType state)
{
    if (!v || v->closed || !v->opened)
        return FALSE;
    gtk_widget_grab_focus(v->area);
    gboolean handled = on_key(NULL, keyval, 0, state, v);
    gunichar uc = gdk_keyval_to_unicode(keyval);
    if (!handled && uc >= 0x20 && uc != 0x7f &&
        !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_META_MASK))) {
        char buf[8];
        buf[g_unichar_to_utf8(uc, buf)] = '\0';
        start_key_text(v, 2, buf);
    }
    on_key_released(NULL, keyval, 0, state, v);
    return TRUE;
}

gboolean
ns_proc_view_remote_text(NsProcView *v, const char *text)
{
    if (!v || v->closed || !v->opened || !text)
        return FALSE;
    gtk_widget_grab_focus(v->area);
    char **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (i > 0)
            ns_proc_view_remote_key(v, GDK_KEY_Return, 0);
        char *line = lines[i];
        gsize len = strlen(line);
        if (len > 0 && line[len - 1] == '\r')
            line[len - 1] = '\0';
        if (*line)
            start_key_text(v, 2, line);
    }
    g_strfreev(lines);
    return TRUE;
}

gboolean
ns_proc_view_remote_scroll(NsProcView *v, double x, double y, double dx,
                           double dy)
{
    if (!v || v->closed || !v->opened)
        return FALSE;
    double s = cur_scale(v);
    v->pointer_x = x * s;
    v->pointer_y = y * s;
    v->fling_vx = v->fling_vy = 0;
    v->wheel_viewport = FALSE;
    if (!pv_queue_wheel_event(v, dx, dy, 0))
        queue_wheel_scroll(v, dx, dy);
    return TRUE;
}

gboolean
ns_proc_view_remote_rendering(NsProcView *v)
{
    return v && (v->render_inflight || v->render_pending ||
                 v->wheel_tick_id != 0);
}

void
ns_proc_view_remote_refresh(NsProcView *v)
{
    if (v && !v->closed && v->opened)
        request_render(v);
}

void
ns_proc_view_remote_geometry(NsProcView *v, int *width, int *height,
                             int *scroll_x, int *scroll_y, double *scale)
{
    double s = cur_scale(v);
    *width = (int)(viewport_w(v) / s + 0.5);
    *height = (int)(viewport_h(v) / s + 0.5);
    *scroll_x = v->scroll_x;
    *scroll_y = v->scroll_y;
    *scale = s;
}

GBytes *
ns_proc_view_remote_screenshot(NsProcView *v, int *out_w, int *out_h)
{
    if (!v || v->closed || !v->area || !gtk_widget_get_mapped(v->area))
        return NULL;
    int w = gtk_widget_get_width(v->area);
    int h = gtk_widget_get_height(v->area);
    GtkNative *native = gtk_widget_get_native(v->area);
    GskRenderer *renderer = native ? gtk_native_get_renderer(native) : NULL;
    if (w <= 1 || h <= 1 || !renderer)
        return NULL;
    double s = cur_scale(v);
    int cw = MAX(1, (int)(w / s + 0.5));
    int ch = MAX(1, (int)(h / s + 0.5));
    GdkPaintable *paintable = gtk_widget_paintable_new(v->area);
    GtkSnapshot *snap = gtk_snapshot_new();
    gtk_snapshot_scale(snap, (float)cw / (float)w, (float)ch / (float)h);
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snap), w, h);
    GskRenderNode *node = gtk_snapshot_free_to_node(snap);
    g_object_unref(paintable);
    if (!node)
        return NULL;
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, (float)cw, (float)ch);
    GdkTexture *tex = gsk_renderer_render_texture(renderer, node, &bounds);
    gsk_render_node_unref(node);
    if (!tex)
        return NULL;
    GBytes *png = gdk_texture_save_to_png_bytes(tex);
    *out_w = gdk_texture_get_width(tex);
    *out_h = gdk_texture_get_height(tex);
    g_object_unref(tex);
    return png;
}
