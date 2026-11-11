/* Nordstjernen — CSS transitions and @keyframes animation engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */
#ifndef NS_ANIM_H
#define NS_ANIM_H
#include <glib.h>
#include "css.h"
#include "dom.h"
G_BEGIN_DECLS
typedef struct ns_anim ns_anim;
ns_anim *ns_anim_new(void);
void     ns_anim_free(ns_anim *a);
void     ns_anim_load_from_stylesheet(ns_anim *a, const ns_css_stylesheet *sh);
void     ns_anim_observe_all(ns_anim *a, GHashTable *styles, gint64 now_us);
void     ns_anim_apply(ns_anim *a, GHashTable *styles);
gboolean ns_anim_tick(ns_anim *a, gint64 now_us);
gboolean ns_anim_has_active(const ns_anim *a);
gboolean ns_anim_needs_layout(const ns_anim *a);
gboolean ns_anim_paint_only(const ns_anim *a);
typedef void (*ns_anim_event_cb)(const ns_node *node, const char *type,
                                 const char *name, double elapsed_ms,
                                 gpointer user);
void     ns_anim_drain_events(ns_anim *a, ns_anim_event_cb cb, gpointer user);
gboolean                 ns_anim_get_opacity   (ns_anim *a,
                                                const ns_node *dom,
                                                double *out_opacity);
const ns_css_transform  *ns_anim_get_transform (ns_anim *a,
                                                const ns_node *dom);
gboolean                 ns_anim_get_color     (ns_anim *a,
                                                const ns_node *dom,
                                                ns_css_anim_target which,
                                                guint8 out_rgba[4]);
typedef struct ns_anim_info {
    const ns_node *node;
    int            prop;
    int            run;
    const char    *name;
    const char    *fill;
    const char    *direction;
    char           easing[96];
    double         current_ms;
    double         duration_ms;
    double         delay_ms;
    double         iterations;
    gboolean       active;
    gboolean       paused;
    gboolean       pending;
    gboolean       finished;
    guint          generation;
} ns_anim_info;
typedef void (*ns_anim_visit_cb)(const ns_anim_info *info, gpointer user);
void     ns_anim_visit(ns_anim *a, const ns_node *node, ns_anim_visit_cb cb,
                       gpointer user);
gboolean ns_anim_info_for(ns_anim *a, const ns_node *node, int prop,
                          ns_anim_info *out);
typedef void (*ns_anim_keyframe_cb)(double offset, const char *easing,
                                    const GArray *decls, gpointer user);
void     ns_anim_keyframes_visit(ns_anim *a, const ns_node *node, int prop,
                                 ns_anim_keyframe_cb cb, gpointer user);
gboolean ns_anim_seek(ns_anim *a, const ns_node *node, int prop, double ms);
const ns_css_value *ns_anim_base_value(ns_anim *a, const ns_node *node, int prop);
gboolean ns_anim_control(ns_anim *a, const ns_node *node, int prop,
                         const char *op);
typedef struct ns_anim_script_timing {
    double      duration_ms;
    double      delay_ms;
    double      iterations;
    const char *direction;
    const char *fill;
    const char *easing;
    const char *composite;
} ns_anim_script_timing;
gboolean ns_anim_script_start(ns_anim *a, const ns_node *node,
                              const char *const *stop_css, const double *stop_pct,
                              int n_stops, const ns_anim_script_timing *t,
                              int *out_prop, guint *out_generation);
void     ns_anim_prune(ns_anim *a, GHashTable *live);
gboolean ns_anim_is_scroll_source(const ns_anim *a, const ns_node *node);
gboolean ns_anim_scroll_needs_sync(const ns_anim *a, guint epoch);
gboolean ns_anim_scroll_fresh(const ns_anim *a);
gboolean ns_anim_scroll_uses_root(const ns_anim *a);
gboolean ns_anim_scroll_has_sources(const ns_anim *a);
void     ns_anim_scroll_reset(ns_anim *a, guint epoch);
void     ns_anim_note_scroll(ns_anim *a, const ns_node *node, double x,
                             double y, double max_x, double max_y);
gboolean ns_anim_scroll_resample(ns_anim *a, gboolean *out_layout);
typedef gboolean (*ns_anim_scroll_lookup_cb)(const ns_node *node, double pos[2],
                                             double max[2], gpointer user);
void     ns_anim_set_scroll_lookup(ns_anim *a, ns_anim_scroll_lookup_cb cb,
                                   gpointer user);
typedef void (*ns_anim_size_each_fn)(const ns_node *node, double width,
                                    double height, gpointer data);
typedef void (*ns_anim_size_visit_cb)(gpointer user, ns_anim_size_each_fn each,
                                      gpointer data);
void     ns_anim_set_size_visitor(ns_anim *a, ns_anim_size_visit_cb cb,
                                  gpointer user);
gboolean ns_anim_resolve_pending_sizes(ns_anim *a);
void     ns_anim_rebase(ns_anim *a, gint64 base_us);
G_END_DECLS
#endif
