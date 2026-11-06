/* Nordstjernen — Cairo paint API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_PAINT_H
#define NS_PAINT_H

#include <cairo.h>
#include <glib.h>
#include "ns_pango.h"

#include "js.h"
#include "layout.h"

G_BEGIN_DECLS

struct ns_selection;
struct ns_anim;
typedef struct ns_paint_stats {
    guint boxes_seen;
    guint hidden;
    guint skipped_top;
    guint culled_bounds;
    guint offscreen;
    guint grouped;
    guint overflow_clips;
    guint sorted_parents;
    guint sorted_children;
    guint blocks;
    guint inlines;
    guint images;
    guint videos;
    guint canvases;
} ns_paint_stats;

void ns_paint(cairo_t *cr, const ns_box *root, const char *highlight_query);
void ns_paint_blur_argb(guchar *data, int stride, int w, int h, double sigma);
void ns_paint_with_selection(cairo_t *cr, const ns_box *root,
                             const char *highlight_query,
                             const struct ns_selection *sel);
enum {
    NS_PAINT_VP_FIXED = 1,
    NS_PAINT_VP_STICKY = 2,
};

typedef struct ns_paint_vp_capture {
    const ns_box *box;
    int kind;
    cairo_matrix_t rel;
} ns_paint_vp_capture;

typedef struct ns_paint_layer_plan {
    gboolean dynamic;
    GHashTable *kinds;
    GArray *vp;
} ns_paint_layer_plan;

void ns_paint_layer_plan_init(ns_paint_layer_plan *plan);
void ns_paint_layer_plan_clear(ns_paint_layer_plan *plan);
void ns_paint_plan_layers(cairo_t *cr, const ns_box *root,
                          ns_paint_layer_plan *plan);
typedef cairo_t *(*ns_paint_upper_fn)(int index, gpointer data);

gboolean ns_paint_doc_layers(cairo_t *cr, ns_paint_upper_fn upper,
                             gpointer upper_data, const ns_box *root,
                             const char *highlight_query,
                             const struct ns_selection *sel,
                             const ns_paint_layer_plan *plan);
void ns_paint_vp_layer(cairo_t *cr, const ns_box *root,
                       const ns_paint_vp_capture *layer, double vp_x,
                       double vp_y,
                       const char *highlight_query,
                       const struct ns_selection *sel);
gboolean ns_paint_canvas_color(const ns_box *root, double rgba_out[4]);
void ns_paint_set_js(ns_js *js);
void ns_paint_set_anim(struct ns_anim *anim);
void ns_paint_set_caret_visible(gboolean visible);
void ns_paint_set_video_layer(guint gen, double tile_y);

void ns_paint_3d_invalidate(void);
gboolean ns_paint_3d_registered(const ns_box *b);
gboolean ns_paint_box_paints_children(const ns_box *b);
gboolean ns_paint_box_clip_hides(const ns_box *b);
gboolean ns_paint_box_establishes_3d(const ns_box *b);
const ns_box *ns_paint_3d_pick(const ns_box *root3d, double x, double y);

void ns_paint_set_search(gboolean case_sensitive, const ns_box *active);

gboolean ns_paint_inline_range_extents(const ns_box *b, gsize start, gsize len,
                                       const ns_inline_attr *element,
                                       double *out_x, double *out_y,
                                       double *out_w, double *out_h);
gboolean ns_paint_inline_range_rects(const ns_box *b, gsize start, gsize len,
                                     const ns_inline_attr *element,
                                     GArray *out);
gboolean ns_paint_inline_toggle_rect(const ns_box *b, const ns_inline_attr *r,
                                     double *out_x, double *out_y,
                                     double *out_w, double *out_h);
gboolean ns_paint_inline_empty_rect(const ns_box *b, const ns_inline_attr *r,
                                    double *out_x, double *out_y,
                                    double *out_h);
gboolean ns_paint_inline_xy_to_byte(const ns_box *b,
                                    double rel_x, double rel_y,
                                    gsize *out_byte);
gboolean ns_paint_inline_xy_to_hit_byte(const ns_box *b,
                                        double rel_x, double rel_y,
                                        gsize *out_byte);
gboolean ns_paint_inline_moved_extents(const ns_box *b, double *x0, double *y0,
                                       double *x1, double *y1);
gboolean ns_paint_inline_on_shifted_text(const ns_box *b,
                                         double rel_x, double rel_y);
gboolean ns_paint_inline_word_range(const ns_box *b, gsize byte,
                                    gsize *out_start, gsize *out_end);
double ns_paint_inline_y_offset_for_layout(const ns_box *b,
                                           NsPangoLayout *layout);

NsPangoLayout *ns_paint_build_inline_layout(cairo_t *cr, const ns_box *b);
void ns_paint_sync_inline_atomic_offsets(ns_box *root);

void ns_paint_register_font_oracle(void);

void ns_paint_apply_inline_font(NsPangoLayout *layout, const ns_style *style);
void ns_paint_apply_text_align(NsPangoLayout *layout, const ns_style *s);
NsPangoAttribute *ns_paint_inline_font_attr(const ns_inline_attr *r);
gboolean ns_paint_inline_attr_nowrap(const ns_inline_attr *r);
int ns_paint_pango_font_size(double size_px);

NsPangoContext *ns_paint_text_context(void);

void ns_paint_apply_i18n(NsPangoLayout *layout, NsPangoAttrList *attrs,
                         const ns_box *box);
void ns_paint_apply_font_features(NsPangoAttrList *attrs, const ns_style *style,
                                  guint start, guint end);
NsPangoAttribute *ns_paint_font_features_attr_from_values(int kerning,
                                                        const char *ligatures,
                                                        const char *settings);
NsPangoAttribute *ns_paint_font_variations_attr_from_values(const char *settings);

NsPangoWrapMode ns_paint_wrap_mode_for(const ns_style *style);
void ns_paint_i18n_memo_begin(void);
void ns_paint_i18n_memo_end(void);
gboolean ns_paint_text_ellipsizes(const ns_style *style);
void ns_paint_layout_set_inline_text(NsPangoLayout *layout, const char *text);

double ns_paint_css_line_height_px(const ns_style *style);
gboolean ns_paint_css_line_height_is_normal(const ns_style *style);
double ns_paint_normal_line_height_px(const ns_style *style);
gboolean ns_paint_style_font_metrics(const ns_style *style,
                                     ns_css_font_metrics *out);
#define NS_CSS_LINE_HEIGHT_KEY "ns-css-line-height"
typedef struct ns_css_line_height {
    double   px;
    gboolean normal;
} ns_css_line_height;
void ns_paint_apply_css_line_spacing(NsPangoLayout *layout,
                                     const ns_style *style);
void ns_paint_start_align_overflow(NsPangoLayout *layout);

gboolean ns_paint_li_is_inside(const ns_style *li_style);
void     ns_paint_list_ordinals_begin(void);
void     ns_paint_list_ordinals_end(void);
gboolean ns_paint_li_marker_text(const ns_node *li, const ns_style *li_style,
                                 char *out, gsize out_sz);

G_END_DECLS

#endif
