/* Nordstjernen — HTTP renderer IPC client (experiment; mirrors rproc.h). */

#ifndef NS_RPROC_HTTP_H
#define NS_RPROC_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ns_rproc_http ns_rproc_http;

typedef struct {
    int   ok;
    int   page_width;
    int   page_height;
    char *title;
    char *url;
    char *nav;
    int   security;
    char *remote_ip;
} ns_rproc_http_page;

typedef struct {
    int                  ok;
    int                  width;
    int                  height;
    int                  stride;
    int                  animating;
    int                  wake_ms;
    int                  caret_blinking;
    int                  wheel_snapped;
    int                  wheel_answered;
    int                  wheel_prevented;
    int                  page_w;
    int                  page_h;
    int                  scroll_y;
    int                  scroll_x;
    int                  unchanged;
    int                  render_rc;
    const unsigned char *pixels;
    char                *nav;
    char                *webgl;
    char                *camera;
    char                *download;
    char                *config;
    char                *audio;
    char                *window_action;
    int                  clipboard;
    char                *tiles;
} ns_rproc_http_frame;

typedef struct {
    int   ok;
    int   changed;
    int   animating;
    int   wake_ms;
    int   page_w;
    int   page_h;
    char *nav;
    char *webgl;
    char *camera;
    char *download;
    char *config;
    char *audio;
    char *window_action;
    char *title;
    char *url;
    int   url_pushed;
} ns_rproc_http_tick;

ns_rproc_http *ns_rproc_http_spawn(const char *renderer_path, int max_width,
                                   int max_height);
ns_rproc_http *ns_rproc_http_spawn_shm(const char *renderer_path,
                                       int max_width, int max_height);

ns_rproc_http *ns_rproc_http_spawn_shm_ex(const char *renderer_path,
                                          int max_width, int max_height,
                                          int private_mode);

typedef void *(*ns_rproc_inproc_attach_fn)(int ctrl_r, int ctrl_w,
                                           unsigned char *fb, int max_w,
                                           int max_h);
void ns_rproc_http_set_inproc(ns_rproc_inproc_attach_fn attach);
void ns_rproc_http_set_device_pixel_ratio(ns_rproc_http *r, double dpr);
int  ns_rproc_http_open(ns_rproc_http *r, const char *url, int viewport_width,
                        int viewport_height, int settle_ms,
                        ns_rproc_http_page *out);

int  ns_rproc_http_open_ex(ns_rproc_http *r, const char *url,
                           int viewport_width, int viewport_height,
                           int settle_ms, int history, int user_activated,
                           ns_rproc_http_page *out);
int  ns_rproc_http_reload(ns_rproc_http *r, const char *url,
                          int viewport_width, int viewport_height,
                          int settle_ms, ns_rproc_http_page *out);
int  ns_rproc_http_render(ns_rproc_http *r, int width, int height,
                          int scroll_x, int scroll_y, double scale,
                          int caret_active, ns_rproc_http_frame *out);

typedef struct ns_rproc_http_wheel {
    int x, y;
    int dx, dy;
    int viewport;
    int ev;
    int ev_x, ev_y;
    int ev_dx, ev_dy;
    int ev_mods;
} ns_rproc_http_wheel;

int  ns_rproc_http_render_wheel(ns_rproc_http *r, int width, int height,
                                int scroll_x, int scroll_y, double scale,
                                int caret_active,
                                const ns_rproc_http_wheel *wheel,
                                ns_rproc_http_frame *out);
typedef struct ns_rproc_http_tiles_req {
    int         tile_h;
    int         want_y0, want_y1;
    int         gen;
    int         vp_held;
    int         fill;
    const char *have;
    const char *hold;
} ns_rproc_http_tiles_req;

int  ns_rproc_http_render_tiles(ns_rproc_http *r, int width, int height,
                                int scroll_x, int scroll_y, double scale,
                                int caret_active,
                                const ns_rproc_http_wheel *wheel,
                                const ns_rproc_http_tiles_req *tiles,
                                ns_rproc_http_frame *out);
size_t ns_rproc_http_map_size(const ns_rproc_http *r);
int  ns_rproc_http_tick_page(ns_rproc_http *r, ns_rproc_http_tick *out);
void ns_rproc_http_tick_clear(ns_rproc_http_tick *out);
char *ns_rproc_http_link_at(ns_rproc_http *r, int x, int y);
char *ns_rproc_http_link_cursor_at(ns_rproc_http *r, int x, int y,
                                   char **out_cursor);
char *ns_rproc_http_clipboard(ns_rproc_http *r);
char *ns_rproc_http_click(ns_rproc_http *r, int x, int y, int mods);
char *ns_rproc_http_press(ns_rproc_http *r, int x, int y, int mods,
                          int button, int clicks);
char *ns_rproc_http_select(ns_rproc_http *r, int kind, int x, int y);
char *ns_rproc_http_key(ns_rproc_http *r, int kind, const char *key,
                        const char *code, int keycode, int mods);
char *ns_rproc_http_key_full(ns_rproc_http *r, int kind, const char *key,
                             const char *code, int keycode, int mods,
                             int *out_prevented);
int   ns_rproc_http_hover_full(ns_rproc_http *r, int x, int y,
                               char **out_href, char **out_cursor);
int   ns_rproc_http_hover_buttons(ns_rproc_http *r, int x, int y, int mods,
                                  int buttons, char **out_href,
                                  char **out_cursor);
int   ns_rproc_http_scroll(ns_rproc_http *r, int x, int y, int dx, int dy);
int   ns_rproc_http_scrollbar(ns_rproc_http *r, int kind, int x, int y);
int   ns_rproc_http_drop_files(ns_rproc_http *r, int x, int y,
                               const char *const *paths, int n_paths);
char *ns_rproc_http_release_full(ns_rproc_http *r, int *out_changed);
char *ns_rproc_http_release_at(ns_rproc_http *r, int x, int y,
                               int *out_changed);
char *ns_rproc_http_release_button(ns_rproc_http *r, int x, int y,
                                   int button, int clicks, int *out_changed,
                                   int *out_newtab);
int   ns_rproc_http_focused_editable(ns_rproc_http *r);
char *ns_rproc_http_focused_editable_value(ns_rproc_http *r,
                                           size_t *out_caret,
                                           size_t *out_anchor);
int   ns_rproc_http_set_focused_editable_selection(ns_rproc_http *r,
                                                   size_t caret,
                                                   size_t anchor);
long  ns_rproc_http_download_start(ns_rproc_http *r, const char *url);
char *ns_rproc_http_download_poll(ns_rproc_http *r, long id);
int   ns_rproc_http_download_cancel(ns_rproc_http *r, long id);
int   ns_rproc_http_find(ns_rproc_http *r, const char *query,
                         int case_sensitive, int direction, int from_y,
                         int *out_total, int *out_current, int *out_scroll_y);
int   ns_rproc_http_set_viewport(ns_rproc_http *r, int width, int height,
                                 ns_rproc_http_page *out);
int   ns_rproc_http_resolve_camera(ns_rproc_http *r, const char *origin,
                                   int allow);
int   ns_rproc_http_resolve_webgl(ns_rproc_http *r, const char *origin,
                                  int allow);
int   ns_rproc_http_set_color_scheme(ns_rproc_http *r, int dark);
int   ns_rproc_http_audio_event(ns_rproc_http *r, const char *line);
char *ns_rproc_http_eval(ns_rproc_http *r, const char *src);
char *ns_rproc_http_dump(ns_rproc_http *r, const char *kind);
char *ns_rproc_http_console_poll(ns_rproc_http *r);
char *ns_rproc_http_media_at(ns_rproc_http *r, int x, int y, int *out_is_video,
                             int *out_stream);
void  ns_rproc_http_contextmenu(ns_rproc_http *r, int x, int y,
                                int *out_prevented, int *out_edit);
int   ns_rproc_http_export(ns_rproc_http *r, const char *path);

unsigned char *ns_rproc_http_favicon(ns_rproc_http *r, int *out_w, int *out_h,
                                     int *out_stride);

void ns_rproc_http_page_clear(ns_rproc_http_page *out);
void ns_rproc_http_close(ns_rproc_http *r);

void ns_rproc_http_interrupt(ns_rproc_http *r);

int  ns_rproc_http_pid(ns_rproc_http *r);
void ns_rproc_http_terminate(ns_rproc_http *r);

int  ns_rproc_self_pid(void);

int  ns_rproc_http_proc_info(int pid, char *state, int state_sz, long *rss_kb);
double ns_rproc_http_proc_cpu(int pid);
int  ns_rproc_http_proc_threads(int pid);

#ifdef __cplusplus
}
#endif

#endif
