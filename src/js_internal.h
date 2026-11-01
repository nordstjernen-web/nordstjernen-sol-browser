/* Nordstjernen — internal JS engine declarations shared between
 * js.c and js_canvas.c. Not a public API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_JS_INTERNAL_H
#define NS_JS_INTERNAL_H

#include <glib.h>
#include <cairo.h>
#include "ns_pango.h"
#include "ns_quickjs.h"

#include "js.h"
#include "dom.h"
#include "image.h"
#include "layout.h"

typedef struct ns_worker_host ns_worker_host;

typedef struct ns_canvas_state {
    int w, h;
    cairo_surface_t *surf;
    cairo_t         *cr;
    double fill_r, fill_g, fill_b, fill_a;
    double stroke_r, stroke_g, stroke_b, stroke_a;
    double line_width;
    char  *font;
    cairo_pattern_t *fill_pattern;
    cairo_pattern_t *stroke_pattern;
    double shadow_r, shadow_g, shadow_b, shadow_a;
    double shadow_blur, shadow_ox, shadow_oy;
    gboolean origin_clean;
    JSValue ctx2d;
    JSContext *jsctx;
    JSRuntime *rt;
    ns_node *owned_node;
    int context_kind;
} ns_canvas_state;

typedef struct ns_path2d {
    cairo_surface_t *rs;
    cairo_t         *cr;
} ns_path2d;

typedef struct ns_image_bitmap {
    cairo_surface_t *surf;
    int w, h;
    gboolean origin_clean;
} ns_image_bitmap;

typedef struct ns_perf_observer {
    JSValue   cb;
    JSValue   wrapper;
    gconstpointer realm;
    gboolean  disconnected;
    gboolean  pinned;
    GPtrArray *entry_types;
    GPtrArray *records;
} ns_perf_observer;

struct ns_js {
    JSRuntime    *rt;
    JSContext    *ctx;
    JSContext    *main_realm_ctx;
    JSContext    *module_ctx;
    GPtrArray    *frame_ctxs;
    GHashTable   *frame_contexts;
    GHashTable   *frame_csps;
    GHashTable   *frame_posts;
    GHashTable   *frame_windows;
    GArray       *font_ready_resolvers;
    gboolean      fonts_loadingdone_armed;
    gboolean      fonts_idle_watched;
    ns_js_log_cb  log_cb;
    gpointer      log_user_data;
    ns_js_mutated_cb mut_cb;
    gpointer      mut_user_data;
    ns_js_navigate_cb nav_cb;
    gpointer      nav_user_data;
    ns_js_download_cb download_cb;
    gpointer      download_user_data;
    ns_js_audio_cb audio_cb;
    gpointer      audio_user_data;
    GPtrArray    *webaudio_rt;
    guint         webaudio_source;
    ns_js_media_seek_cb media_seek_cb;
    gpointer      media_seek_user_data;
    ns_js_media_play_cb media_play_cb;
    gpointer      media_play_user_data;
    ns_js_media_muted_cb media_muted_cb;
    gpointer      media_muted_user_data;
    ns_js_media_frame_cb media_frame_cb;
    gpointer      media_frame_user_data;
    ns_js_mse_cb  mse_cb;
    gpointer      mse_user_data;
    ns_js_mse_buffered_cb mse_buffered_cb;
    gpointer      mse_buffered_user_data;
    ns_js_mse_remove_cb mse_remove_cb;
    gpointer      mse_remove_user_data;
    ns_js_mse_bytes_cb mse_bytes_cb;
    gpointer      mse_bytes_user_data;
    ns_js_media_volume_cb media_volume_cb;
    gpointer      media_volume_user_data;
    guint         next_audio_token;
    GHashTable   *audio_elements;
    GPtrArray    *media_helper_fetches;
    ns_js_scroll_to_cb scroll_to_cb;
    gpointer      scroll_to_user_data;
    ns_js_fragment_nav_cb fragment_nav_cb;
    gpointer      fragment_nav_user_data;
    ns_js_form_submit_cb form_submit_cb;
    gpointer      form_submit_user_data;
    ns_js_soft_nav_cb soft_nav_cb;
    gpointer      soft_nav_user_data;
    ns_js_repaint_cb repaint_cb;
    gpointer      repaint_user_data;
    ns_js_layout_flush_cb layout_flush_cb;
    ns_js_layout_flush_cb style_flush_cb;
    gpointer      layout_flush_user_data;
    ns_js_viewport_scroll_cb viewport_scroll_cb;
    gpointer      viewport_scroll_user_data;
    gboolean    (*load_delay_cb)(gpointer user_data);
    gpointer      load_delay_user_data;
    gboolean      in_layout_flush;
    gboolean      cssom_commit_pending;
    guint64       task_epoch;
    ns_js_clipboard_write_cb clipboard_write_cb;
    ns_js_selection_cmd_cb selection_cmd_cb;
    gpointer      selection_cmd_user_data;
    gpointer      clipboard_write_user_data;
    ns_js_window_action_cb window_action_cb;
    gpointer      window_action_user_data;
    const ns_node *pending_fullscreen_event_target;
    JSValue       pending_fullscreen_resolve;
    JSValue       history_state;
    int           history_length;
    GPtrArray    *history_entries;
    int           history_pos;
    guint64       nav_key_seq;
    JSValue       navigation;
    char         *current_url;
    char         *document_origin;
    ns_node       *current_doc;
    GHashTable    *frame_urls;
    GHashTable    *frame_referrers;
    gpointer       realm_scope_base;
    char         **top_url_slot;
    ns_node       *current_script;
    char         *early_inject_src;
    gboolean      mutated;
    guint64       mutation_serial;
    GHashTable   *timers;
    GMainContext *main_context;
    GPtrArray    *workers;
    GHashTable   *shared_workers;
    GHashTable   *popups;
    char         *opener_token;
    char         *opener_origin;
    ns_worker_host *worker_host;
    int           next_timer_id;
    int           timer_nesting_level;
    int           n_immediate_timers;
    gboolean      running_due_timers;
    GArray       *raf_pending;
    int           next_raf_id;
    gint64        raf_last_us;
    ns_node      *raf_frame_ctx;
    JSValue       pristine_promise;
    JSValue       fonts_set;
    GHashTable   *style_table;
    const struct ns_box *layout_root;
    GHashTable   *box_lookup_cache;
    const void   *box_lookup_cache_root;
    const void   *box_lookup_pending_root;
    int           box_lookup_pending_count;
    const ns_node *focused_node;
    const ns_node *change_pending;
    char          *change_baseline;
    const ns_node *focused_doc;
    gpointer       focus_guard;
    gpointer       parser_hold;
    gboolean      pointer_input;
    gboolean      autofocus_processed;
    const ns_node *focus_nav_start;
    const ns_node *active_modal;
    GPtrArray     *close_watchers;
    GPtrArray     *modal_dialogs;
    ns_node       *dialog_pointerdown;
    GPtrArray     *popover_auto;
    GPtrArray     *popover_hint;
    GHashTable    *popover_info;
    ns_node       *popover_hint_parent;
    ns_node       *popover_pointerdown;
    gboolean       popover_showing;
    int            popover_hiding_count;
    GArray        *attr_element_refs;
    const ns_node *pointer_lock_element;
    const ns_node *pointer_capture_element;
    int            mouse_buttons;
    double         last_mouse_x[2];
    double         last_mouse_y[2];
    gboolean       has_last_mouse[2];
    gint64         user_activation_us;
    gboolean       user_ever_activated;
    GHashTable   *canvas_states;
    ns_image_cache *image_cache;
    struct ns_anim *anim;
    GHashTable   *js_image_loads;
    GHashTable   *orphan_nodes;
    GPtrArray    *listeners;
    GHashTable   *listener_index;
    GHashTable   *pinned_wrappers_set;
    GHashTable   *attr_wrappers;
    GHashTable   *attribute_maps;
    GPtrArray    *pending_fetches;
    GHashTable   *fetch_states_by_id;
    guint         next_fetch_id;
    GPtrArray    *pending_xhrs;
    GPtrArray    *link_fetches;
    GHashTable   *link_results;
    GPtrArray    *pending_ws;
    GPtrArray    *pending_aborts;
    GPtrArray    *filereader_idles;
    GHashTable   *local_storage;
    GHashTable   *session_storage;
    GPtrArray    *frame_storages;
    char         *local_storage_origin;
    char         *local_storage_path;
    gboolean      local_storage_dirty;
    guint         local_storage_flush_source;
    gboolean      local_storage_disabled;
    char         *cookie_value;
    guint64       cookie_version;
    GHashTable   *session_storage_buckets;
    GHashTable   *cookie_buckets;
    char         *partition_key;
    guint64       opaque_counter;
    char         *referrer;
    int           ready_state;
    GHashTable   *doc_ready_states;
    GHashTable   *initial_blank_realms;
    GHashTable   *window_forwards;
    GHashTable   *window_outwards;
    GQueue       *message_tasks;
    GHashTable   *realm_cloners;
    GHashTable   *realm_origins;
    JSValue       navigator_brand;
    guint         message_task_source;
    guint         lifecycle_source;
    GArray       *lifecycle_tasks;
    ns_node      *lifecycle_doc;
    char         *lifecycle_origin;
    int           lifecycle_phase;
    gint64        lifecycle_start_us;
    gint64        eval_deadline_us;
    gint64        js_monitor_deadline_us;
    gboolean      halted;
    gboolean      in_pump;
    gboolean      in_scroll_dispatch;
    GPtrArray    *pending_scrollend;
    gboolean      pending_scrollend_doc;
    int           eval_depth;
    GString      *document_write_buffer;
    ns_node      *document_write_script;
    gboolean      document_write_parser_open;
    const ns_node *document_write_body_doc;
    guint         document_write_body_count;
    GPtrArray    *deferred_script_roots;
    GPtrArray    *async_script_roots;
    guint         async_script_source;
    GPtrArray    *pending_iframe_loads;
    GPtrArray    *deferred_iframe_loads;
    GHashTable   *pending_rejections;
    GHashTable   *reported_rejections;
    GHashTable   *iframe_globals;
    int           iframe_load_depth;
    int           frame_parser_depth;
    GArray       *pending_storage_events;
    gboolean      storage_events_draining;
    gint64        last_orphan_sweep_us;
    int           dispatch_depth;
    int           listener_snapshots;
    guint         listener_tombstones;
    int           callback_depth;
    int           synthetic_click_depth;
    GPtrArray    *mutation_observers;
    gboolean      mutation_drain_scheduled;
    GPtrArray    *intersection_observers;
    GPtrArray    *media_query_lists;
    GPtrArray    *resize_observers;
    guint         observer_tick_source;
    gboolean      observer_ticking;
    guint         raf_tick_source;
    gboolean      raf_host_driven;
    JSValue       iframe_doc;
    int           iframe_doc_set;
    ns_csp *csp;
    GPtrArray    *csp_headers;
    char         *selection_text;
    gboolean      selection_has_range;
    double        selection_x, selection_y, selection_w, selection_h;
    int           module_load_count;
    gsize         module_load_bytes;
    gboolean      module_load_capped;
    GPtrArray    *import_map;
    gint64        time_origin_us;
    double        time_origin_real_ms;
    ns_js_navigation_timing navigation_timing;
    GPtrArray    *perf_entries;
    GPtrArray    *perf_observers;
    gboolean      perf_drain_scheduled;
    GPtrArray    *node_iters;
    GHashTable   *console_counts;
    GHashTable   *console_timers;
    GHashTable   *blob_urls;
    GHashTable   *ce_registry;
    const ns_node *ce_main_doc;
    GHashTable   *ce_pending;
    GHashTable   *platform_globals;
    GHashTable   *ce_under_construction;
    ns_node      *ce_upgrading;
    void         *ce_upgrading_wrapper;
    int           ce_in_attr_callback;
    int           ce_defer_upgrades;
    int           throw_on_dynamic_markup;
    int           ignore_destructive_writes;
    int           in_error_report;
    JSValue       nodelist_decorator;
    int           nodelist_decorator_set;
    JSValue       live_html_proto;
    JSValue       live_node_proto;
    JSValue       live_radionode_proto;
    int           live_protos_set;
    JSValue       computed_style_proxy;
    int           computed_style_proxy_set;
    JSValue       url_helper;
    int           url_helper_set;
    JSValue       search_params_helper;
    int           search_params_helper_set;
    JSValue       form_data_helper;
    int           form_data_helper_set;
    JSValue       body_consumer_helper;
    int           body_consumer_helper_set;
    GHashTable   *realm_helpers;
    JSAtom        atom_capture;
    JSAtom        atom_once;
    JSAtom        atom_signal;
    JSAtom        atom_passive;
    JSAtom        atom_aborted;
    JSAtom        atom_immediate_stopped;
    JSAtom        atom_propagation_stopped;
    int           listener_atoms_set;
    guint64       dom_gen;
    JSValue       proto_node;
    JSValue       proto_element;
    JSValue       proto_htmlelement;
    JSValue       proto_htmlunknownelement;
    JSValue       proto_svgelement;
    JSValue       proto_svgaelement;
    JSValue       proto_mathmlelement;
    JSValue       proto_chardata;
    JSValue       proto_text;
    JSValue       proto_comment;
    JSValue       proto_cdata;
    JSValue       proto_pi;
    JSValue       proto_doctype;
    JSValue       proto_docfrag;
    JSValue       proto_document;
    GHashTable   *per_tag_protos;
    int           dom_protos_set;
    struct {
        const void *root;
        char        kind;
        char       *key;
        guint64     gen;
        JSValue     value;
        int         set;
    } qcache[16];
    int           qcache_next;
    GPtrArray    *current_dispatch_path;
    gboolean      current_dispatch_window;
    gboolean      in_hashchange;
};

static inline ns_js *
js_from_ctx(JSContext *ctx)
{
    return ctx ? (ns_js *)JS_GetContextOpaque(ctx) : NULL;
}

typedef void (*ns_ctx_drawfn)(cairo_t *cr, void *ud);

typedef struct ns_draw_rect_ud {
    double x, y, w, h, lw;
    JSContext *ctx;
    JSValueConst this_val;
    ns_canvas_state *st;
} ns_draw_rect_ud;

typedef struct ns_draw_path_ud {
    JSContext *ctx;
    JSValueConst this_val;
    ns_canvas_state *st;
    double lw;
    cairo_path_t *snapshot;
    cairo_fill_rule_t fill_rule;
} ns_draw_path_ud;

double ns_arg_d(JSContext *ctx, JSValueConst v);
void ns_bind_fn(JSContext *ctx, JSValueConst obj, const char *name, JSCFunction *fn, int argc);
const ns_box *ns_box_find_by_dom(const ns_box *root, const ns_node *target);
uint32_t ns_js_array_length(JSContext *ctx, JSValueConst arr);
gboolean ns_webaudio_render_offline(JSContext *ctx, JSValueConst destination,
                                    uint32_t frames, double rate, float *out);
void     ns_webaudio_rt_add(ns_js *js, JSContext *ctx, JSValueConst obj,
                            double rate, gboolean running);
void     ns_webaudio_rt_set_state(ns_js *js, JSValueConst obj, const char *state);
gboolean ns_webaudio_rt_busy(ns_js *js);
gboolean ns_webaudio_rt_pump(ns_js *js);
double ns_webaudio_schedule_floor(ns_js *js, JSContext *ctx,
                                  JSValueConst audio_ctx);
void     ns_webaudio_rt_free(ns_js *js);
void ns_js_promise_reject(JSContext *ctx, JSValue resolvers[2], const char *message);
JSValue ns_make_element(JSContext *ctx, const ns_node *cnode);
JSValue ns_js_realm_event(JSContext *ctx, JSValueConst target, const char *type);
const ns_node *ns_unwrap_element(JSValueConst val);

void
ns_path2d_finalizer(JSRuntime *rt, JSValue val);
void
ns_image_bitmap_finalizer(JSRuntime *rt, JSValue val);
void
ns_canvas_state_free(gpointer data);
JSValue
ns_image_bitmap_close(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv);
JSValue
ns_image_bitmap_make(JSContext *ctx, cairo_surface_t *surf, int w, int h,
                     gboolean origin_clean);
cairo_surface_t *
ns_image_bitmap_from_imagedata(JSContext *ctx, JSValueConst src,
                               int *out_w, int *out_h);
cairo_surface_t *
ns_image_bitmap_crop(cairo_surface_t *src, int sw, int sh,
                     int sx, int sy, int rw, int rh);
gboolean
ns_image_bitmap_is(JSValueConst v);
JSValue
ns_image_bitmap_clone(JSContext *ctx, JSValueConst v);
JSValue
ns_canvas_clone_object(JSContext *ctx, JSValueConst v);
JSValue
ns_window_create_image_bitmap(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
JSValue
ns_offscreen_transferToImageBitmap(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv);
JSValue
ns_offscreen_getContext(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv);
JSValue
ns_dommatrix_make(JSContext *ctx, double a, double b, double c, double d,
                  double e, double f);
JSValue
ns_canvas_throw_dom(JSContext *ctx, const char *name, const char *msg);
int
ns_canvas_dim_from_attr(const ns_node *el, const char *name, int defv);
gboolean
ns_canvas_parse_color(const char *s, double *r, double *g, double *b, double *a);
gboolean ns_canvas_color_is_literal(const char *s);
ns_canvas_state *
ns_canvas_state_for(ns_js *js, const ns_node *el);
ns_canvas_state *
ns_ctx_state(JSContext *ctx, JSValueConst this_val);
cairo_pattern_t *
ns_ctx_build_pattern(JSContext *ctx, JSValueConst obj, gboolean *origin_clean);
gboolean
ns_js_resource_origin_clean(ns_js *js, JSContext *ctx, const char *url,
                            const char *cors_allow_origin);
double
ns_ctx_global_alpha(JSContext *ctx, JSValueConst this_val);
cairo_operator_t
ns_ctx_parse_composite(const char *s);
void
ns_ctx_apply_composite(JSContext *ctx, JSValueConst this_val, cairo_t *cr);
gboolean
ns_ctx_image_smoothing(JSContext *ctx, JSValueConst this_val);
void
ns_ctx_sync_styles(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st);
gboolean
ns_ctx_has_shadow(const ns_canvas_state *st);
void
ns_box_blur_argb(uint8_t *data, int w, int h, int stride, int radius);
void
ns_ctx_with_shadow(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st,
                   ns_ctx_drawfn draw, void *ud);
void
ns_ctx_set_fill_source(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st);
void
ns_ctx_set_stroke_source(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st);
void
ns_draw_fillrect(cairo_t *cr, void *vud);
void
ns_draw_strokerect(cairo_t *cr, void *vud);
JSValue
ns_ctx_fillRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_strokeRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_clearRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_beginPath(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_closePath(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_moveTo(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_lineTo(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_arc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
gboolean
ns_value_is_path2d(JSValueConst v);
void
ns_replay_path2d(cairo_t *target, JSValueConst path_v);
cairo_fill_rule_t
ns_parse_fill_rule(const char *s);
cairo_path_t *
ns_ctx_prepare_path_and_rule(JSContext *ctx, cairo_t *cr,
                             int argc, JSValueConst *argv);
void
ns_ctx_restore_path(cairo_t *cr, cairo_path_t *saved);
void
ns_draw_fillpath(cairo_t *cr, void *vud);
void
ns_draw_strokepath(cairo_t *cr, void *vud);
JSValue
ns_ctx_fill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_stroke(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_save(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_restore(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_translate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_scale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_rotate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
NsPangoFontDescription *
ns_canvas_font_desc(const char *css_font);
gboolean
ns_ctx_direction_is_rtl(JSContext *ctx, JSValueConst this_val);
void
ns_ctx_paint_text(JSContext *ctx, JSValueConst this_val,
                  ns_canvas_state *st, const char *text,
                  double x, double y, double max_width,
                  gboolean stroke);
JSValue
ns_ctx_fillText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_measureText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_quadraticCurveTo(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv);
JSValue
ns_ctx_bezierCurveTo(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);
JSValue
ns_ctx_arcTo(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv);
JSValue
ns_ctx_ellipse(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv);
JSValue
ns_ctx_clip(JSContext *ctx, JSValueConst this_val,
            int argc, JSValueConst *argv);
gboolean
ns_matrix_from_obj(JSContext *ctx, JSValueConst v, cairo_matrix_t *m);
JSValue
ns_ctx_setTransform(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_ctx_transform(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_ctx_resetTransform(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv);
JSValue
ns_ctx_setLineDash(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv);
JSValue
ns_ctx_getLineDash(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv);
JSValue
ns_ctx_gradient_addColorStop(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);
cairo_surface_t *
ns_ctx_drawimage_source(JSContext *ctx, JSValueConst src, int *out_w, int *out_h,
                        gboolean *origin_clean);
JSValue
ns_ctx_drawImage(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_ctx_createPattern(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);
JSValue
ns_ctx_createLinearGradient(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv);
JSValue
ns_ctx_createRadialGradient(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv);
JSValue
ns_ctx_createConicGradient(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);
JSValue
ns_ctx_createImageData(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv);
JSValue
ns_ctx_getImageData(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_ctx_putImageData(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_ctx_strokeText(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv);
void
ns_round_rect_subpath(cairo_t *cr, double x, double y, double w, double h,
                      double rtl, double rtr, double rbr, double rbl);
gboolean
ns_extract_radii(JSContext *ctx, JSValueConst v,
                 double *rtl, double *rtr, double *rbr, double *rbl);
JSValue
ns_ctx_roundRect(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_ctx_reset(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv);
JSValue
ns_ctx_getTransform(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_ctx_isPointInPath(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);
JSValue
ns_ctx_isPointInStroke(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv);
JSValue
ns_path2d_get_cr(JSContext *ctx, JSValueConst this_val, cairo_t **out);
JSValue
ns_path2d_moveTo(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_path2d_lineTo(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_path2d_closePath(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_path2d_bezierCurveTo(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv);
JSValue
ns_path2d_quadraticCurveTo(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);
JSValue
ns_path2d_arc(JSContext *ctx, JSValueConst this_val,
              int argc, JSValueConst *argv);
JSValue
ns_path2d_arcTo(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv);
JSValue
ns_path2d_ellipse(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv);
JSValue
ns_path2d_rect(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv);
JSValue
ns_path2d_roundRect(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_path2d_addPath(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv);
const char *
ns_svg_skip_ws(const char *p);
gboolean
ns_svg_read_number(const char **pp, double *out);
void
ns_path2d_arc_svg(cairo_t *cr, double x1, double y1,
                  double rx, double ry, double phi_deg,
                  gboolean large_arc, gboolean sweep,
                  double x2, double y2);
void
ns_path2d_parse_svg(cairo_t *cr, const char *d);
JSValue
ns_path2d_ctor(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv);
JSValue
ns_ctx_get_attrs(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_ctx_is_context_lost(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv);
JSValue
ns_ctx_draw_focus_if_needed(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv);
JSValue
ns_element_getContext(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv);
cairo_status_t
ns_canvas_png_write(void *closure, const unsigned char *data, unsigned int length);
JSValue
ns_offscreen_convertToBlob(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);

void ns_canvas_register_image_bitmap_class(JSRuntime *rt);
void ns_canvas_register_path2d_class(JSRuntime *rt);
void ns_image_bitmap_define_members(JSContext *ctx, JSValueConst global);

enum {
    NS_HK_CTX2D,
    NS_HK_OFFSCREEN_CTX2D,
    NS_HK_GRADIENT,
    NS_HK_PATTERN,
    NS_HK_IMAGEDATA,
    NS_HK_TEXTMETRICS,
    NS_HK_OFFSCREEN,
    NS_HK_ACTIVEINFO,
    NS_HK_PRECISION,
};
void ns_canvas_register_classes(JSRuntime *rt);
gpointer ns_hidden_ptr(JSValueConst v);
void ns_hidden_set_ptr(JSValueConst v, gpointer ptr);
JSValue ns_hidden_new(JSContext *realm, int kind, JSValueConst proto);
gboolean ns_hidden_is(JSValueConst v, int kind);
JSValue ns_hget(JSContext *ctx, JSValueConst obj, const char *name);
void ns_hset(JSContext *ctx, JSValueConst obj, const char *name, JSValue val);
gboolean ns_ctx2d_is(JSValueConst v);
JSContext *ns_canvas_realm(JSContext *ctx, const ns_node *el);
JSContext *ns_ctx_realm(JSContext *ctx, JSValueConst this_val);
JSContext *ns_js_realm_for_node(ns_js *js, const ns_node *node);
char *ns_js_computed_text(JSContext *ctx, const ns_node *node, const char *name);
JSValue ns_api_proto(JSContext *realm, const char *iface);
JSValue ns_api_proto_of_ctor(JSContext *ctx, JSValueConst new_target,
                             const char *iface);
JSValue ns_api_throw_new_required(JSContext *ctx, const char *iface);
JSValue ns_api_interface(JSContext *ctx, JSValueConst global, const char *name,
                         JSValue ctor, const char *parent);
char *ns_canvas_color_string(const char *css);
char *ns_canvas_font_string(const char *css);
void ns_ctx2d_init_state(JSContext *ctx, JSValueConst obj);
JSValue ns_ctx2d_new(JSContext *ctx, const ns_node *el, JSValueConst canvas_obj,
                     gboolean offscreen, JSValue attrs);
JSValue ns_gradient_new(JSContext *ctx, JSContext *realm, const char *type);
JSValue ns_pattern_new(JSContext *ctx, JSContext *realm, JSValueConst source,
                       const char *repetition);
JSValue ns_textmetrics_new(JSContext *ctx, JSContext *realm, const double v[10]);
JSValue ns_imagedata_wrap(JSContext *ctx, JSContext *realm, JSValueConst proto,
                          int w, int h, JSValue data, const char *color_space);
JSValue ns_imagedata_new(JSContext *ctx, JSContext *realm, int w, int h,
                         const uint8_t *rgba);
JSValue ns_imagedata_construct(JSContext *ctx, JSValueConst new_target, int argc,
                               JSValueConst *argv);
const ns_node *ns_offscreen_node(JSValueConst obj);
void ns_offscreen_sync_size(JSContext *ctx, JSValueConst obj);
JSValue ns_offscreen_construct(JSContext *ctx, JSValueConst new_target, int argc,
                               JSValueConst *argv);
void ns_canvas_install(JSContext *ctx, JSValueConst global, gboolean window);

void ns_bind_fn_if_not_callable(JSContext *ctx, JSValueConst obj, const char *name,
                                JSCFunction *fn, int argc);
JSValue ns_own_data_props_toJSON(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv);
gboolean ns_js_get_bool_prop(JSContext *ctx, JSValueConst obj, const char *key,
                             gboolean *was_set);

void ns_perf_observer_release(JSContext *ctx, ns_perf_observer *o);
double ns_perf_now_ms(const ns_js *js);
typedef struct ns_realm_origin {
    gint64 origin_us;
    double origin_real_ms;
} ns_realm_origin;
gint64 ns_js_time_origin_us(const ns_js *js, gconstpointer realm);
double ns_js_time_origin_real_ms(const ns_js *js, gconstpointer realm);
double ns_perf_realm_now_ms(JSContext *ctx);
void   ns_js_start_frame_clock(ns_js *js, gconstpointer frame);
void   ns_js_adopt_frame_clock(ns_js *js, gconstpointer frame, JSContext *ctx);
void   ns_js_clear_frame_clocks(ns_js *js, gboolean destroy);
double ns_perf_relative_ms(gint64 now_us, gint64 origin_us);
typedef struct ns_perf_resource_info {
    gconstpointer timeline;
    const char   *document_url;
    gboolean      render_blocking;
    gboolean      cors_mode;
    gboolean      preload;
    const char   *next_hop_protocol;
    const char   *timing_allow_origin;
    long          status;
    gint64        body_size;
} ns_perf_resource_info;
struct ns_response;
void ns_perf_add_resource_timed(ns_js *js, const ns_perf_resource_info *info,
                                const char *url, const char *initiator,
                                gint64 start_us, gint64 end_us,
                                const struct ns_response *resp);
gboolean ns_perf_has_resource(ns_js *js, gconstpointer timeline,
                              const char *url, const char *initiator);
void ns_perf_move_timeline(ns_js *js, gconstpointer from, gconstpointer to);
JSValue ns_perf_new_performance_object(JSContext *ctx);
void    ns_perf_set_performance_objects(JSContext *ctx, JSValueConst perf,
                                        JSValue timing, JSValue navigation,
                                        JSValue event_counts);
JSValue ns_window_performance_time_origin_get(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv);
JSValue ns_window_performance_object_get(JSContext *ctx,
                                         JSValueConst this_val, int argc,
                                         JSValueConst *argv, int magic);
void ns_perf_entry_free(gpointer p);
JSValue ns_perf_supported_entry_types(JSContext *ctx);
void ns_perf_install_entry_list(JSContext *ctx, JSValueConst global);
JSValue ns_perf_observer_ctor(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
JSValue ns_perf_observer_observe(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv);
JSValue ns_perf_observer_disconnect(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv);
JSValue ns_perf_observer_takeRecords(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);
JSValue ns_window_performance_now(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv);
JSValue ns_window_performance_mark(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv);
JSValue ns_window_performance_measure(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv);
JSValue ns_window_performance_clearMarks(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);
JSValue ns_window_performance_clearMeasures(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);
JSValue ns_window_performance_clearResourceTimings(JSContext *ctx,
                                                   JSValueConst this_val,
                                                   int argc, JSValueConst *argv);
JSValue ns_window_performance_getEntries(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);
JSValue ns_window_performance_getEntriesByName(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv);
JSValue ns_window_performance_getEntriesByType(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv);
JSValue ns_window_performance_memory_get(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);

#endif
