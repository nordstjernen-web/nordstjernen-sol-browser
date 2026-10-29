/* Nordstjernen — public C API for embedding the browser engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef LIBNORDSTJERNEN_H
#define LIBNORDSTJERNEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ns_browser ns_browser;

int ns_browser_init(void);

ns_browser *ns_browser_open(const char *url, int viewport_width, int settle_ms);
ns_browser *ns_browser_open_viewport(const char *url, int viewport_width,
                                     double viewport_height, int settle_ms);

ns_browser *ns_browser_open_post(const char *url, int viewport_width,
                                 int settle_ms, const void *body,
                                 size_t body_len, const char *content_type);
ns_browser *ns_browser_open_post_viewport(const char *url, int viewport_width,
                                          double viewport_height,
                                          int settle_ms, const void *body,
                                          size_t body_len,
                                          const char *content_type);

char *ns_browser_take_post(ns_browser *browser, size_t *out_len,
                           char **out_content_type);

char *ns_browser_render_text(ns_browser *browser);

char *ns_browser_dump_dom(ns_browser *browser);
char *ns_browser_dump_layout(ns_browser *browser);
char *ns_browser_dump_performance(ns_browser *browser);

int ns_browser_render_image(ns_browser *browser, const char *path);

int ns_browser_page_size(ns_browser *browser, int *out_width, int *out_height);

int ns_browser_snap_document(ns_browser *browser, double viewport_w,
                             double viewport_h, int prev_x, int prev_y,
                             int *scroll_x, int *scroll_y);

int ns_browser_set_viewport_width(ns_browser *browser, int css_width);
int ns_browser_set_viewport(ns_browser *browser, int css_width,
                            double css_height);

int ns_browser_set_device_pixel_ratio(ns_browser *browser, double dppx);
void ns_browser_window_action_applied(ns_browser *browser);

int ns_browser_render_rgba(ns_browser *browser, int scroll_x, int scroll_y,
                           int width, int height, double scale,
                           unsigned char *out, int stride);

int ns_browser_render_argb32(ns_browser *browser, int scroll_x, int scroll_y,
                             int width, int height, double scale,
                             unsigned char *out, int stride);

char *ns_browser_link_at(ns_browser *browser, int x, int y);
char *ns_browser_link_under(ns_browser *browser, int x, int y);

int ns_browser_release(ns_browser *browser);
char *ns_browser_release_click(ns_browser *browser, int *out_changed);
int ns_browser_wheel_event(ns_browser *browser, int x, int y, int dx, int dy,
                           int mods, int *out_changed);
char *ns_browser_release_click_at(ns_browser *browser, int x, int y,
                                  int *out_changed);

char *ns_browser_cursor_at(ns_browser *browser, int x, int y);

char *ns_browser_click(ns_browser *browser, int x, int y, int mods);
char *ns_browser_press(ns_browser *browser, int x, int y, int mods);
char *ns_browser_press_full(ns_browser *browser, int x, int y, int mods,
                            int button, int clicks);
char *ns_browser_release_full(ns_browser *browser, int x, int y, int button,
                              int clicks, int *out_changed, int *out_newtab);

#define NS_KEY_MOD_TEXT_SEPARATE 16

char *ns_browser_key(ns_browser *browser, int kind, const char *key,
                     const char *code, int keycode, int mods);
char *ns_browser_key_full(ns_browser *browser, int kind, const char *key,
                          const char *code, int keycode, int mods,
                          int *out_prevented);
int   ns_browser_focused_editable(ns_browser *browser);
char *ns_browser_focused_editable_value(ns_browser *browser,
                                        size_t *out_caret,
                                        size_t *out_anchor);
int   ns_browser_set_focused_editable_selection(ns_browser *browser,
                                                size_t caret,
                                                size_t anchor);

int ns_browser_hover(ns_browser *browser, int x, int y);
int ns_browser_hover_full(ns_browser *browser, int x, int y, int mods,
                          int buttons);

int ns_browser_scroll_at(ns_browser *browser, int x, int y, int dx, int dy);
int ns_browser_scroll_at_full(ns_browser *browser, int x, int y, int dx,
                              int dy, int *out_snapped);

int  ns_browser_scrollbar_press(ns_browser *browser, int x, int y);
int  ns_browser_scrollbar_drag(ns_browser *browser, int x, int y);
void ns_browser_scrollbar_release(ns_browser *browser);

int ns_browser_drop_files(ns_browser *browser, int x, int y,
                          const char *const *paths, int n_paths);

char *ns_browser_take_pending_audio(ns_browser *browser);

char *ns_browser_console_drain(ns_browser *browser);

char *ns_browser_eval(ns_browser *browser, const char *src);

char *ns_browser_media_at(ns_browser *browser, int x, int y, int *out_is_video,
                          int *out_stream);

void ns_browser_audio_feedback(ns_browser *browser, const char *line);

int ns_browser_contextmenu(ns_browser *browser, int x, int y);

enum {
    NS_BROWSER_EDIT_FIELD     = 1,
    NS_BROWSER_EDIT_WRITABLE  = 2,
    NS_BROWSER_EDIT_SELECTION = 4,
};
int ns_browser_contextmenu_full(ns_browser *browser, int x, int y,
                                int *out_edit);

int ns_browser_find(ns_browser *browser, const char *query, int case_sensitive,
                    int direction, int from_y, int *out_total,
                    int *out_current, int *out_y);

char *ns_browser_select(ns_browser *browser, int kind, int x, int y);

int ns_browser_tick(ns_browser *browser, int budget_ms);

int ns_browser_animating(ns_browser *browser);
int ns_browser_wake_ms(ns_browser *browser);
int ns_browser_set_caret_blink_active(ns_browser *browser, int active);
int ns_browser_caret_blinking(ns_browser *browser);

char *ns_browser_title(ns_browser *browser);

char *ns_browser_url(ns_browser *browser);

void ns_browser_set_next_referrer(const char *url);
void ns_browser_set_next_initiator(const char *url);
void ns_browser_set_next_user_activated(int user_activated);
void ns_browser_set_next_reload(int reload);

typedef struct ns_session_storage ns_session_storage;
ns_session_storage *ns_session_storage_new(void);
void ns_session_storage_free(ns_session_storage *storage);
void ns_browser_set_next_session_storage(ns_session_storage *storage);

void ns_browser_set_color_scheme(int dark);
void ns_browser_set_reduced_motion(int reduce);

int ns_browser_security(ns_browser *browser, const char **out_ip);

char *ns_browser_take_pending_nav(ns_browser *browser);
int ns_browser_take_soft_nav_pushed(ns_browser *browser);
int ns_browser_take_pending_scroll_y(ns_browser *browser, int *out_scroll_y);
int ns_browser_take_pending_scroll(ns_browser *browser, int *out_scroll_x,
                                   int *out_scroll_y);

char *ns_browser_take_pending_webgl(ns_browser *browser);
void  ns_browser_resolve_webgl(ns_browser *browser, const char *origin,
                               int allow);
char *ns_browser_take_pending_camera(ns_browser *browser);
void  ns_browser_resolve_camera(ns_browser *browser, const char *origin,
                                int allow);

int   ns_browser_has_pending_clipboard(ns_browser *browser);
char *ns_browser_take_pending_clipboard(ns_browser *browser);
char *ns_browser_take_pending_download(ns_browser *browser);
char *ns_browser_take_pending_window_action(ns_browser *browser);

char *ns_browser_links(ns_browser *browser);

char *ns_browser_favicon_url(ns_browser *browser);

int  ns_browser_bfcache_eligible(ns_browser *browser);
void ns_browser_bfcache_park(ns_browser *browser);
void ns_browser_bfcache_restore(ns_browser *browser, int viewport_width,
                                double viewport_height);

void ns_browser_close(ns_browser *browser);

int ns_browser_busy(const ns_browser *browser);

void ns_history_record(const char *url, const char *title);

void ns_browser_shutdown(void);

void ns_browser_sandbox(const char *self_exe);

#ifdef __cplusplus
}
#endif

#endif
