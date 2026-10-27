/* Nordstjernen — GTK view backed by the out-of-process renderer (thin client).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NORDSTJERNEN_GTK_PROCVIEW_H
#define NORDSTJERNEN_GTK_PROCVIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct NsProcView NsProcView;

const char *ns_app_self_exe(void);

void ns_popover_menu_fit(GtkWidget *popover);

typedef enum {
    NS_PROC_EVT_TITLE,
    NS_PROC_EVT_URL,
    NS_PROC_EVT_STATUS,
    NS_PROC_EVT_NEWTAB,
    NS_PROC_EVT_HISTORY,
    NS_PROC_EVT_LOADING,
    NS_PROC_EVT_DOWNLOAD,
    NS_PROC_EVT_FAVICON,
    NS_PROC_EVT_WEBGL,
    NS_PROC_EVT_FULLSCREEN,
    NS_PROC_EVT_ZOOM,
    NS_PROC_EVT_CONFIG
} NsProcEvent;

typedef void (*NsProcNotify)(NsProcView *view, NsProcEvent evt,
                             const char *text, gpointer user_data);

char *ns_proc_renderer_path(void);

NsProcView *ns_proc_view_new(void);
GtkWidget  *ns_proc_view_widget(NsProcView *view);
void        ns_proc_view_set_notify(NsProcView *view, NsProcNotify cb,
                                    gpointer user_data);

void        ns_proc_view_set_private(NsProcView *view, gboolean private_mode);
gboolean    ns_proc_view_is_private(NsProcView *view);

void ns_proc_view_load(NsProcView *view, const char *url);
void ns_proc_view_back(NsProcView *view);
void ns_proc_view_forward(NsProcView *view);
void ns_proc_view_reload(NsProcView *view);
void ns_proc_view_stop(NsProcView *view);
void ns_proc_view_toggle_console(NsProcView *view);
void ns_proc_view_exit_fullscreen(NsProcView *view);

gboolean    ns_proc_view_can_back(NsProcView *view);
gboolean    ns_proc_view_can_forward(NsProcView *view);
const char *ns_proc_view_url(NsProcView *view);
const char *ns_proc_view_display_url(NsProcView *view);
const char *ns_proc_view_title(NsProcView *view);
gboolean    ns_proc_view_is_loading(NsProcView *view);

int         ns_proc_view_security(NsProcView *view);
const char *ns_proc_view_remote_ip(NsProcView *view);

GdkPaintable *ns_proc_view_favicon(NsProcView *view);

int         ns_proc_view_renderer_pid(NsProcView *view);
int         ns_proc_view_audio_pid(NsProcView *view);
void        ns_proc_view_end_task(NsProcView *view);

void   ns_proc_view_print(NsProcView *view);
void   ns_proc_view_save_pdf(NsProcView *view);
void   ns_proc_view_save_image(NsProcView *view);
void        ns_proc_view_stop_audio(NsProcView *view);

void   ns_proc_view_zoom_in(NsProcView *view);
void   ns_proc_view_zoom_out(NsProcView *view);
void   ns_proc_view_zoom_reset(NsProcView *view);
int    ns_proc_view_zoom_percent(NsProcView *view);
void   ns_proc_view_focus(NsProcView *view);
void   ns_proc_view_find_open(NsProcView *view);
gboolean ns_proc_view_find_close(NsProcView *view);
void   ns_proc_view_set_color_scheme(NsProcView *view, gboolean dark);

typedef void (*NsProcViewTextFn)(const char *text, gpointer user_data);

void     ns_proc_view_remote_eval(NsProcView *view, const char *js,
                                  NsProcViewTextFn cb, gpointer user_data);
NsProcView *ns_proc_view_ref(NsProcView *view);
void     ns_proc_view_unref(NsProcView *view);
gboolean ns_proc_view_is_closed(NsProcView *view);
void     ns_proc_view_download_start(NsProcView *view, const char *url,
                                     NsProcViewTextFn cb, gpointer user_data);
void     ns_proc_view_download_poll(NsProcView *view, long id,
                                    NsProcViewTextFn cb, gpointer user_data);
void     ns_proc_view_download_cancel(NsProcView *view, long id);
gboolean ns_proc_view_remote_click(NsProcView *view, double x, double y,
                                   gboolean page_coords, int count, int mods);
gboolean ns_proc_view_remote_hover(NsProcView *view, double x, double y,
                                   gboolean page_coords);
gboolean ns_proc_view_remote_key(NsProcView *view, guint keyval,
                                 GdkModifierType state);
gboolean ns_proc_view_remote_text(NsProcView *view, const char *text);
gboolean ns_proc_view_remote_scroll(NsProcView *view, double x, double y,
                                    double dx, double dy);
gboolean ns_proc_view_remote_rendering(NsProcView *view);
void     ns_proc_view_remote_refresh(NsProcView *view);
void     ns_proc_view_remote_geometry(NsProcView *view, int *width,
                                      int *height, int *scroll_x,
                                      int *scroll_y, double *scale);
GBytes  *ns_proc_view_remote_screenshot(NsProcView *view, int *out_w,
                                        int *out_h);

G_END_DECLS

#endif
