/* Nordstjernen — layout tree API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_LAYOUT_H
#define NS_LAYOUT_H

#include <glib.h>

#include "css.h"
#include "dom.h"

G_BEGIN_DECLS

typedef enum ns_box_kind {
    NS_BOX_BLOCK,
    NS_BOX_INLINE,
    NS_BOX_TEXT,
    NS_BOX_IMAGE,
    NS_BOX_TABLE,
    NS_BOX_TABLE_CAPTION,
    NS_BOX_TABLE_ROW,
    NS_BOX_TABLE_CELL,
    NS_BOX_VIDEO,
    NS_BOX_MATH,
    NS_BOX_SVG,
} ns_box_kind;

const char *ns_box_kind_name(ns_box_kind k);

typedef struct ns_edges {
    double top, right, bottom, left;
} ns_edges;

typedef struct ns_link_range {
    gsize start;
    gsize len;
    char *href;
    char *target;
    const ns_node *dom;
} ns_link_range;

typedef enum ns_inline_attr_kind {
    NS_INLINE_BOLD,
    NS_INLINE_ITALIC,
    NS_INLINE_MONOSPACE,
    NS_INLINE_UNDERLINE,
    NS_INLINE_OVERLINE,
    NS_INLINE_STRIKETHROUGH,
    NS_INLINE_INPUT_FIELD,
    NS_INLINE_INPUT_FIELD_FOCUSED,
    NS_INLINE_BUTTON,
    NS_INLINE_CHECKBOX,
    NS_INLINE_CHECKBOX_CHECKED,
    NS_INLINE_RADIO,
    NS_INLINE_RADIO_CHECKED,
    NS_INLINE_PROGRESS,
    NS_INLINE_METER,
    NS_INLINE_RANGE,
    NS_INLINE_FONT_SIZE,
    NS_INLINE_FONT_WEIGHT,
    NS_INLINE_FONT_STRETCH,
    NS_INLINE_FONT_FEATURES,
    NS_INLINE_FONT_VARIATIONS,
    NS_INLINE_COLOR,
    NS_INLINE_FONT_FAMILY,
    NS_INLINE_BG_COLOR,
    NS_INLINE_RISE,
    NS_INLINE_SMALL_CAPS,
    NS_INLINE_CARET,
    NS_INLINE_SELECTION,
    NS_INLINE_ELEMENT,
    NS_INLINE_SPACER,
    NS_INLINE_SPELLCHECK,
    NS_INLINE_EMPTY_ELEMENT,
    NS_INLINE_GENERATED,
    NS_INLINE_LETTER_SPACING,
} ns_inline_attr_kind;

typedef struct ns_inline_attr {
    ns_inline_attr_kind kind;
    gsize start;
    gsize len;
    double font_size_px;
    double letter_spacing_px;
    double rel_dx, rel_dy;
    const ns_node *rel_layer;
    int font_weight;
    int font_stretch;
    int font_kerning;
    const char *font_ligatures;
    const char *font_features;
    const char *font_variations;
    double box_w, box_h;
    double rise_px;
    gboolean native_chrome;
    guint8 r, g, b, a;
    const char *family;
    const ns_node *dom;
    const ns_style *style;
    const char *bg_image_src;
    void *bg_image;
} ns_inline_attr;

typedef struct ns_inline_atomic {
    gsize byte_off;
    struct ns_box *box;
    double owner_offset_x;
    double owner_offset_y;
    double rise_px;
} ns_inline_atomic;

typedef struct ns_box_media {
    char  *image_src;
    void  *image;
    char  *bg_image_src;
    void  *bg_image;
    char  *marker_image_src;
    void  *marker_image;
    char  *border_image_src;
    void  *border_image;
    char  *mask_image_src;
    void  *mask_image;
    GPtrArray *bg_layer_srcs;
    GPtrArray *bg_layer_images;
    char  *video_src;
    char  *video_poster;
    char  *video_audio_src;
    void  *video;
    gboolean declared_image_size;
    gboolean placeholder_image_size;
    gboolean intrinsic_ratio_only;
    double   image_density;
    GPtrArray *svg_foreign;
    double    *svg_foreign_xform;
} ns_box_media;

typedef struct ns_grid_track_edges {
    double start, end;
} ns_grid_track_edges;

typedef struct ns_box {
    ns_box_kind kind;
    const ns_node  *dom;
    const ns_style *style;
    ns_style       *owned_style;

    double x, y;
    double rel_dx, rel_dy;

    double content_width, content_height;
    double first_baseline;
    double last_baseline;
    double definite_height;
    double definite_height_before_flex;
    double flex_pass_x, flex_pass_y;
    double last_layout_width;
    gboolean definite_height_read;
    double measured_content_height;
    double cb_height_override;
    gboolean block_size_indefinite;
    double cb_width_override;
    gboolean cb_width_set;
    double flex_main_size;
    gboolean has_flex_main;
    gboolean is_rendered_legend;
    gboolean inline_split_tail;
    gboolean phantom_line;
    double margin_top_through;
    double paint_top, paint_bottom;
    GPtrArray *negative_layers;
    gboolean negative_lifted;
    guint8 layer_flags;
    gboolean pseudo_after;
    const ns_node *pseudo_host;
    ns_edges margin, padding, border;

    double scroll_x, scroll_y;
    double scroll_max_x, scroll_max_y;
    gboolean scrolls;

    char *text;

    const ns_style *inline_layout_cache_style;
    double inline_layout_cache_width;
    double inline_layout_cache_height;
    gboolean inline_layout_cache_valid;
    int vertical_wm;
    int text_orient;
    const ns_style *inline_natural_cache_style;
    double inline_natural_cache_width;
    gboolean inline_natural_cache_valid;
    const ns_style *inline_min_cache_style;
    double inline_min_cache_width;
    gboolean inline_min_cache_valid;

    void *paint_layout;

    GArray *links;
    GArray *attrs;
    GArray *inline_atomics;
    GArray *atomic_line_heights;
    GArray *atomic_line_offsets;
    GArray *table_col_hints;
    GArray *grid_col_tracks;
    GArray *grid_row_tracks;
    int grid_explicit_cols;
    int grid_explicit_rows;

    ns_box_media *media;
    GHashTable   *svg_styles;

    int colspan;
    int rowspan;
    int columns;

    guint order_epoch;
    guint order_rank;
    guint order_end;
    const ns_style *order_abs_style;
    gboolean order_abs_style_set;
    gboolean order_abs;

    struct ns_box *parent;
    struct ns_box *first_child;
    struct ns_box *last_child;
    struct ns_box *next_sibling;
} ns_box;

struct _PangoAttrList;
struct _PangoLayout;
void ns_inline_apply_atomic_shapes(struct _PangoAttrList *list, const ns_box *box);
void ns_inline_apply_spacing(struct _PangoAttrList *list, const ns_style *style,
                             const char *text);
gboolean ns_inline_toggle_geometry(const ns_inline_attr *r, double *side,
                                   double margins[4]);
gboolean ns_inline_range_geometry(const ns_inline_attr *r, const ns_box *box,
                                  double *w, double *h, double margins[4]);
void ns_inline_layout_set_attrs(struct _PangoLayout *layout,
                                struct _PangoAttrList *list, const ns_box *box);
void ns_inline_offset_at(const ns_box *box, gsize byte, double *dx, double *dy);
gboolean ns_inline_shift_bounds(const ns_box *box, double *x0, double *y0,
                                double *x1, double *y1);
gboolean ns_inline_unshift_point(const ns_box *box, struct _PangoLayout *layout,
                                 double *x, double *y);
char *ns_layout_select_srcset(const char *srcset, const char *sizes);
double ns_text_indent_px(const ns_style *s, double basis);
double ns_inline_text_indent_px(const ns_box *run, const ns_style *s,
                                double basis);
double ns_control_dim_px(const ns_css_value *v, double font_size, double basis);
double ns_inline_attr_control_width(const ns_inline_attr *r, const ns_box *box);
double ns_control_css_extra_w(const ns_node *dom, const ns_style *s);
double ns_control_css_extra_h(const ns_node *dom, const ns_style *s);

void ns_box_free(ns_box *box);

double ns_box_max_bottom(const ns_box *root, double seed);

gboolean ns_box_fieldset_legend_gap(const ns_box *fieldset, double *border_inset,
                                    double *gap_x0, double *gap_x1,
                                    double *gap_y0, double *gap_y1);

void ns_box_table_caption_extents(const ns_box *table, double *above,
                                  double *below);

void ns_paint_drop_box_cache(ns_box *box);

struct ns_image_cache;
ns_box *ns_layout_build(const ns_node *doc, GHashTable *styles,
                        double viewport_width,
                        const ns_node *focused_input,
                        gsize focused_caret_byte,
                        gsize focused_sel_anchor_byte,
                        struct ns_image_cache *image_cache,
                        const char *base_url);

gboolean ns_layout_frame_viewport(const ns_node *frame, double *w, double *h);

void ns_layout_set_open_select(const ns_node *select);
void ns_layout_set_datalist_open(gboolean open);
char *ns_vertical_stack_text(const char *text);

void ns_layout_collect_images(const ns_box *root, GPtrArray *out_boxes);
void ns_layout_collect_videos(const ns_box *root, GPtrArray *out_boxes);

const char *ns_box_hit_link(const ns_box *root, double x, double y);
const ns_link_range *ns_box_hit_link_range(const ns_box *root, double x, double y);

const ns_box *ns_box_hit_test(const ns_box *root, double x, double y);
const ns_box *ns_box_hit_test_local(const ns_box *root, double x, double y,
                                    double *local_x, double *local_y);
const ns_node *ns_box_image_map_area(const ns_box *b, double local_x,
                                     double local_y);
void ns_box_set_hit_viewport(double scroll_x, double scroll_y);
void ns_box_hit_viewport(double *scroll_x, double *scroll_y);
void ns_box_note_scroll(void);
guint ns_box_scroll_epoch(void);
GHashTable *ns_box_scroll_save(const ns_box *root);
void ns_box_scroll_restore(ns_box *root, GHashTable *map);
gboolean ns_box_is_fixed(const ns_box *b);
void ns_box_sticky_offset_in(const ns_box *b, double sp_x0, double sp_y0,
                             double sp_x1, double sp_y1,
                             double *out_dx, double *out_dy);
void ns_box_sticky_offset(const ns_box *b, double vp_x0, double vp_y0,
                          double vp_x1, double vp_y1,
                          double *out_dx, double *out_dy);
void ns_box_hit_offset(const ns_box *b, double *dx, double *dy);
gboolean ns_box_is_stacking_context(const ns_box *b);
gboolean ns_style_is_stacking_context(const ns_style *s);
gboolean ns_box_is_upper_layer(const ns_box *b);
gboolean ns_box_is_flex_or_grid_item(const ns_box *b);
int ns_box_layer_z(const ns_box *b);
int ns_layout_tree_order_cmp(const ns_box *a, const ns_node *a_dom,
                             const ns_box *b, const ns_node *b_dom);
int ns_box_stack_key(const ns_box *b);

typedef struct ns_sticky_y {
    gboolean has_top, has_bottom;
    double top_start, top_cap;
    double bottom_start, bottom_cap;
} ns_sticky_y;

gboolean ns_box_in_scroller(const ns_box *b);
gboolean ns_box_subtree_extent_y(const ns_box *b, double *top,
                                 double *bottom);
gboolean ns_box_sticky_y_model(const ns_box *b, double viewport_h,
                               ns_sticky_y *out);

ns_box *ns_box_hit_scrollable(ns_box *root, double x, double y);

void ns_box_scroll_snap(ns_box *scroller);
void ns_box_scroll_snap_from(ns_box *scroller, double prev_x, double prev_y);

gboolean ns_box_scroll_snap_viewport(ns_box *root, const ns_style *s,
                                     double viewport_w, double viewport_h,
                                     double max_x, double max_y,
                                     double prev_x, double prev_y,
                                     double *x, double *y);
ns_box *ns_box_hit_scrollbar(ns_box *root, double x, double y,
                             double *lx, double *ly);

gboolean ns_box_clips_out_point(const ns_box *b, double x, double y);

const ns_node *ns_box_hit_form_dom(const ns_box *root, double x, double y);

const ns_node *ns_box_hit_inline_dom(const ns_box *root, double x, double y);

const ns_node *ns_box_hit_node(const ns_box *root, double x, double y);

gboolean ns_box_inline_rect_for_dom(const ns_box *root, const ns_node *target,
                                    double *x, double *y,
                                    double *w, double *h);
GArray *ns_box_inline_rects_for_dom(const ns_box *root, const ns_node *target);

const ns_box *ns_layout_svg_foreign_root(const ns_box *svg_box,
                                         const ns_node *fo);
gboolean ns_layout_svg_foreign_xform(const ns_box *svg_box, const ns_node *fo,
                                     double xform[6]);

char *ns_img_chosen_url(const ns_node *n);
double ns_img_chosen_density(const ns_node *n);

guint ns_box_count_matches(const ns_box *root, const char *needle,
                           gboolean case_sensitive);

const ns_box *ns_box_first_match_below(const ns_box *root,
                                       const char *needle,
                                       double y_threshold,
                                       gboolean case_sensitive);

const ns_box *ns_box_first_match_above(const ns_box *root,
                                       const char *needle,
                                       double y_threshold,
                                       gboolean case_sensitive);

guint ns_box_match_ordinal(const ns_box *root,
                           const char *needle,
                           const ns_box *target,
                           gboolean case_sensitive);

char *ns_layout_grid_resolved_tracks(const ns_box *box, gboolean columns);
char *ns_layout_pseudo_content_text(const ns_css_value *content,
                                   const ns_node *host);

G_END_DECLS

#endif
