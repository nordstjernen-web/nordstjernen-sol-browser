/* Nordstjernen — CSS engine API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_CSS_H
#define NS_CSS_H

#include <glib.h>

#include "css_prop_syntax.h"
#include "dom.h"
#include "font.h"
#include "mat4.h"

G_BEGIN_DECLS

typedef enum ns_css_prop {
    NS_CSS_DISPLAY,
    NS_CSS_COLOR,
    NS_CSS_BACKGROUND_COLOR,
    NS_CSS_FONT_SIZE,
    NS_CSS_FONT_WEIGHT,
    NS_CSS_FONT_STYLE,
    NS_CSS_FONT_STRETCH,
    NS_CSS_FONT_KERNING,
    NS_CSS_FONT_VARIANT_LIGATURES,
    NS_CSS_FONT_FEATURE_SETTINGS,
    NS_CSS_FONT_VARIATION_SETTINGS,
    NS_CSS_FONT_FAMILY,
    NS_CSS_TEXT_ALIGN,
    NS_CSS_MARGIN_TOP,
    NS_CSS_MARGIN_RIGHT,
    NS_CSS_MARGIN_BOTTOM,
    NS_CSS_MARGIN_LEFT,
    NS_CSS_PADDING_TOP,
    NS_CSS_PADDING_RIGHT,
    NS_CSS_PADDING_BOTTOM,
    NS_CSS_PADDING_LEFT,
    NS_CSS_BORDER_TOP_WIDTH,
    NS_CSS_BORDER_RIGHT_WIDTH,
    NS_CSS_BORDER_BOTTOM_WIDTH,
    NS_CSS_BORDER_LEFT_WIDTH,
    NS_CSS_BORDER_TOP_COLOR,
    NS_CSS_BORDER_RIGHT_COLOR,
    NS_CSS_BORDER_BOTTOM_COLOR,
    NS_CSS_BORDER_LEFT_COLOR,
    NS_CSS_BORDER_TOP_STYLE,
    NS_CSS_BORDER_RIGHT_STYLE,
    NS_CSS_BORDER_BOTTOM_STYLE,
    NS_CSS_BORDER_LEFT_STYLE,
    NS_CSS_WIDTH,
    NS_CSS_HEIGHT,
    NS_CSS_MAX_WIDTH,
    NS_CSS_MAX_HEIGHT,
    NS_CSS_MIN_WIDTH,
    NS_CSS_MIN_HEIGHT,
    NS_CSS_LINE_HEIGHT,
    NS_CSS_TEXT_DECORATION,
    NS_CSS_POSITION,
    NS_CSS_TOP,
    NS_CSS_RIGHT,
    NS_CSS_BOTTOM,
    NS_CSS_LEFT,
    NS_CSS_Z_INDEX,
    NS_CSS_OPACITY,
    NS_CSS_CURSOR,
    NS_CSS_POINTER_EVENTS,
    NS_CSS_LETTER_SPACING,
    NS_CSS_WORD_SPACING,
    NS_CSS_WHITE_SPACE,
    NS_CSS_BOX_SIZING,
    NS_CSS_TEXT_INDENT,
    NS_CSS_TEXT_TRANSFORM,
    NS_CSS_LIST_STYLE_TYPE,
    NS_CSS_VERTICAL_ALIGN,
    NS_CSS_VISIBILITY,
    NS_CSS_OVERFLOW,
    NS_CSS_FONT_VARIANT,
    NS_CSS_BORDER_RADIUS,
    NS_CSS_BORDER_TOP_LEFT_RADIUS,
    NS_CSS_BORDER_TOP_RIGHT_RADIUS,
    NS_CSS_BORDER_BOTTOM_RIGHT_RADIUS,
    NS_CSS_BORDER_BOTTOM_LEFT_RADIUS,
    NS_CSS_FLEX_DIRECTION,
    NS_CSS_FLEX_WRAP,
    NS_CSS_JUSTIFY_CONTENT,
    NS_CSS_ALIGN_ITEMS,
    NS_CSS_ALIGN_SELF,
    NS_CSS_GAP,
    NS_CSS_ROW_GAP,
    NS_CSS_COLUMN_GAP,
    NS_CSS_FLEX_GROW,
    NS_CSS_FLEX_SHRINK,
    NS_CSS_FLEX_BASIS,
    NS_CSS_ORDER,
    NS_CSS_FLOAT,
    NS_CSS_CLEAR,
    NS_CSS_BOX_SHADOW,
    NS_CSS_OUTLINE_WIDTH,
    NS_CSS_OUTLINE_STYLE,
    NS_CSS_OUTLINE_COLOR,
    NS_CSS_OUTLINE_OFFSET,
    NS_CSS_BACKGROUND_IMAGE,
    NS_CSS_BACKGROUND_REPEAT,
    NS_CSS_BACKGROUND_POSITION_X,
    NS_CSS_BACKGROUND_POSITION_Y,
    NS_CSS_BACKGROUND_SIZE,
    NS_CSS_BACKGROUND_CLIP,
    NS_CSS_BACKGROUND_ORIGIN,
    NS_CSS_BACKGROUND_ATTACHMENT,
    NS_CSS_SCROLLBAR_WIDTH,
    NS_CSS_SCROLLBAR_COLOR,
    NS_CSS_IMAGE_RENDERING,
    NS_CSS_CONTENT,
    NS_CSS_GRID_TEMPLATE_COLUMNS,
    NS_CSS_GRID_TEMPLATE_ROWS,
    NS_CSS_GRID_TEMPLATE_AREAS,
    NS_CSS_GRID_COLUMN,
    NS_CSS_GRID_ROW,
    NS_CSS_GRID_COLUMN_START,
    NS_CSS_GRID_COLUMN_END,
    NS_CSS_GRID_ROW_START,
    NS_CSS_GRID_ROW_END,
    NS_CSS_GRID_AREA,
    NS_CSS_GRID_AUTO_ROWS,
    NS_CSS_GRID_AUTO_COLUMNS,
    NS_CSS_GRID_AUTO_FLOW,
    NS_CSS_TRANSFORM,
    NS_CSS_TRANSFORM_ORIGIN,
    NS_CSS_TRANSITION,
    NS_CSS_ANIMATION,
    NS_CSS_ASPECT_RATIO,
    NS_CSS_TEXT_SHADOW,
    NS_CSS_OVERFLOW_WRAP,
    NS_CSS_WORD_BREAK,
    NS_CSS_HYPHENS,
    NS_CSS_TEXT_OVERFLOW,
    NS_CSS_TEXT_DECORATION_COLOR,
    NS_CSS_TEXT_DECORATION_STYLE,
    NS_CSS_LIST_STYLE_POSITION,
    NS_CSS_LIST_STYLE_IMAGE,
    NS_CSS_USER_SELECT,
    NS_CSS_QUOTES,
    NS_CSS_COLUMN_COUNT,
    NS_CSS_COLUMN_WIDTH,
    NS_CSS_COLUMN_RULE_WIDTH,
    NS_CSS_COLUMN_RULE_STYLE,
    NS_CSS_COLUMN_RULE_COLOR,
    NS_CSS_FILTER,
    NS_CSS_CLIP_PATH,
    NS_CSS_MIX_BLEND_MODE,
    NS_CSS_ACCENT_COLOR,
    NS_CSS_COUNTER_RESET,
    NS_CSS_COUNTER_INCREMENT,
    NS_CSS_LINE_CLAMP,
    NS_CSS_OBJECT_FIT,
    NS_CSS_OBJECT_POSITION_X,
    NS_CSS_OBJECT_POSITION_Y,
    NS_CSS_MASK_IMAGE,
    NS_CSS_OVERFLOW_X,
    NS_CSS_OVERFLOW_Y,
    NS_CSS_APPEARANCE,
    NS_CSS_TABLE_LAYOUT,
    NS_CSS_CAPTION_SIDE,
    NS_CSS_BORDER_COLLAPSE,
    NS_CSS_BORDER_SPACING,
    NS_CSS_CONTAINER_TYPE,
    NS_CSS_CONTAINER_NAME,
    NS_CSS_CARET_COLOR,
    NS_CSS_TAB_SIZE,
    NS_CSS_JUSTIFY_ITEMS,
    NS_CSS_JUSTIFY_SELF,
    NS_CSS_ALIGN_CONTENT,
    NS_CSS_DIRECTION,
    NS_CSS_UNICODE_BIDI,
    NS_CSS_TRANSLATE,
    NS_CSS_ROTATE,
    NS_CSS_SCALE,
    NS_CSS_PERSPECTIVE,
    NS_CSS_PERSPECTIVE_ORIGIN,
    NS_CSS_TRANSFORM_BOX,
    NS_CSS_TRANSFORM_STYLE,
    NS_CSS_BACKFACE_VISIBILITY,
    NS_CSS_ANIMATION_PLAY_STATE,
    NS_CSS_CLIP,
    NS_CSS_CONTENT_VISIBILITY,
    NS_CSS_WRITING_MODE,
    NS_CSS_TEXT_ORIENTATION,
    NS_CSS_TRANSITION_DELAY,
    NS_CSS_TRANSITION_DURATION,
    NS_CSS_ANIMATION_DELAY,
    NS_CSS_ANIMATION_DURATION,
    NS_CSS_ORPHANS,
    NS_CSS_WIDOWS,
    NS_CSS_MAX_LINES,
    NS_CSS_HYPHENATE_LIMIT_LINES,
    NS_CSS_COLUMN_SPAN,
    NS_CSS_BREAK_BEFORE,
    NS_CSS_BREAK_AFTER,
    NS_CSS_BREAK_INSIDE,
    NS_CSS_SCROLL_SNAP_TYPE,
    NS_CSS_SCROLL_SNAP_ALIGN,
    NS_CSS_SCROLL_SNAP_STOP,
    NS_CSS_SCROLL_PADDING_TOP,
    NS_CSS_SCROLL_PADDING_RIGHT,
    NS_CSS_SCROLL_PADDING_BOTTOM,
    NS_CSS_SCROLL_PADDING_LEFT,
    NS_CSS_SCROLL_MARGIN_TOP,
    NS_CSS_SCROLL_MARGIN_RIGHT,
    NS_CSS_SCROLL_MARGIN_BOTTOM,
    NS_CSS_SCROLL_MARGIN_LEFT,
    NS_CSS_BORDER_IMAGE_SOURCE,
    NS_CSS_BORDER_IMAGE_SLICE,
    NS_CSS_BORDER_IMAGE_WIDTH,
    NS_CSS_BORDER_IMAGE_OUTSET,
    NS_CSS_BORDER_IMAGE_REPEAT,
    NS_CSS_FILL,
    NS_CSS_FILL_OPACITY,
    NS_CSS_FILL_RULE,
    NS_CSS_STROKE,
    NS_CSS_STROKE_WIDTH,
    NS_CSS_STROKE_OPACITY,
    NS_CSS_STROKE_LINECAP,
    NS_CSS_STROKE_LINEJOIN,
    NS_CSS_STROKE_MITERLIMIT,
    NS_CSS_STROKE_DASHARRAY,
    NS_CSS_STROKE_DASHOFFSET,
    NS_CSS_STOP_COLOR,
    NS_CSS_STOP_OPACITY,
    NS_CSS_CLIP_RULE,
    NS_CSS_TEXT_ANCHOR,
    NS_CSS_DOMINANT_BASELINE,
    NS_CSS_PAINT_ORDER,
    NS_CSS_VECTOR_EFFECT,
    NS_CSS_SHAPE_RENDERING,
    NS_CSS_SVG_X,
    NS_CSS_SVG_Y,
    NS_CSS_CX,
    NS_CSS_CY,
    NS_CSS_R,
    NS_CSS_RX,
    NS_CSS_RY,
    NS_CSS_ANIMATION_NAME,
    NS_CSS_ANIMATION_TIMING_FUNCTION,
    NS_CSS_ANIMATION_ITERATION_COUNT,
    NS_CSS_ANIMATION_DIRECTION,
    NS_CSS_ANIMATION_FILL_MODE,
    NS_CSS_TRANSITION_PROPERTY,
    NS_CSS_TRANSITION_TIMING_FUNCTION,
    NS_CSS_TRANSITION_BEHAVIOR,
    NS_CSS_ANIMATION_TIMELINE,
    NS_CSS_ANIMATION_RANGE_START,
    NS_CSS_ANIMATION_RANGE_END,
    NS_CSS_ANIMATION_COMPOSITION,
    NS_CSS_COUNTER_SET,
    NS_CSS_OVERFLOW_CLIP_MARGIN,
    NS_CSS_WEBKIT_BOX_ORIENT,
    NS_CSS_MASK_CLIP,
    NS_CSS_MASK_COMPOSITE,
    NS_CSS_MASK_SIZE,
    NS_CSS_MASK_POSITION_X,
    NS_CSS_MASK_POSITION_Y,
    NS_CSS_MASK_REPEAT,
    NS_CSS_ISOLATION,
    NS_CSS_WILL_CHANGE,
    NS_CSS_CONTAIN,
    NS_CSS_BACKDROP_FILTER,
    NS_CSS_SCROLL_TIMELINE_NAME,
    NS_CSS_SCROLL_TIMELINE_AXIS,
    NS_CSS_VIEW_TIMELINE_NAME,
    NS_CSS_TIMELINE_SCOPE,
    NS_CSS_ANCHOR_NAME,
    NS_CSS_ANCHOR_SCOPE,
    NS_CSS_POSITION_ANCHOR,
    NS_CSS_POSITION_AREA,
    NS_CSS_POSITION_TRY_FALLBACKS,
    NS_CSS_POSITION_TRY_ORDER,
    NS_CSS_POSITION_VISIBILITY,
    NS_CSS_SHAPE_OUTSIDE,
    NS_CSS_SHAPE_MARGIN,
    NS_CSS_SHAPE_IMAGE_THRESHOLD,
    NS_CSS_FONT_SIZE_ADJUST,
    NS_CSS_TEXT_JUSTIFY,
    NS_CSS_TEXT_ALIGN_LAST,
    NS_CSS_CONTAIN_INTRINSIC_WIDTH,
    NS_CSS_CONTAIN_INTRINSIC_HEIGHT,
    NS_CSS_MASK_BORDER_SOURCE,
    NS_CSS_MASK_BORDER_SLICE,
    NS_CSS_MASK_BORDER_WIDTH,
    NS_CSS_MASK_BORDER_OUTSET,
    NS_CSS_MASK_BORDER_REPEAT,
    NS_CSS_MASK_BORDER_MODE,
    NS_CSS_WEBKIT_TEXT_FILL_COLOR,
    NS_CSS_TEXT_UNDERLINE_OFFSET,
    NS_CSS_TEXT_DECORATION_THICKNESS,
    NS_CSS_TEXT_UNDERLINE_POSITION,
    NS_CSS_TEXT_DECORATION_SKIP_INK,
    NS_CSS_TEXT_DECORATION_SKIP_SPACES,
    NS_CSS_TEXT_EMPHASIS_STYLE,
    NS_CSS_TEXT_EMPHASIS_COLOR,
    NS_CSS_TEXT_EMPHASIS_POSITION,
    NS_CSS_SCROLL_BEHAVIOR,
    NS_CSS_TOUCH_ACTION,
    NS_CSS_COLOR_SCHEME,
    NS_CSS_OVERSCROLL_BEHAVIOR_X,
    NS_CSS_OVERSCROLL_BEHAVIOR_Y,
    NS_CSS_FONT_VARIANT_NUMERIC,
    NS_CSS_FONT_VARIANT_EAST_ASIAN,
    NS_CSS_FONT_VARIANT_ALTERNATES,
    NS_CSS_FONT_VARIANT_POSITION,
    NS_CSS_FONT_VARIANT_EMOJI,
    NS_CSS_FONT_SYNTHESIS_WEIGHT,
    NS_CSS_FONT_SYNTHESIS_STYLE,
    NS_CSS_FONT_SYNTHESIS_SMALL_CAPS,
    NS_CSS_FONT_SYNTHESIS_POSITION,
    NS_CSS_FONT_OPTICAL_SIZING,
    NS_CSS_FONT_PALETTE,
    NS_CSS_FONT_LANGUAGE_OVERRIDE,
    NS_CSS_TEXT_RENDERING,
    NS_CSS_TEXT_SIZE_ADJUST,
    NS_CSS_ZOOM,
    NS_CSS_RESIZE,
    NS_CSS_EMPTY_CELLS,
    NS_CSS_LINE_BREAK,
    NS_CSS_HYPHENATE_CHARACTER,
    NS_CSS_HYPHENATE_LIMIT_CHARS,
    NS_CSS_TEXT_COMBINE_UPRIGHT,
    NS_CSS_RUBY_POSITION,
    NS_CSS_RUBY_ALIGN,
    NS_CSS_WHITE_SPACE_COLLAPSE,
    NS_CSS_TEXT_WRAP_MODE,
    NS_CSS_TEXT_WRAP_STYLE,
    NS_CSS_PRINT_COLOR_ADJUST,
    NS_CSS_FORCED_COLOR_ADJUST,
    NS_CSS_IMAGE_ORIENTATION,
    NS_CSS_SCROLLBAR_GUTTER,
    NS_CSS_OVERFLOW_ANCHOR,
    NS_CSS_BACKGROUND_BLEND_MODE,
    NS_CSS_MASK_MODE,
    NS_CSS_MASK_ORIGIN,
    NS_CSS_MASK_TYPE,
    NS_CSS_BOX_DECORATION_BREAK,
    NS_CSS_COLOR_INTERPOLATION,
    NS_CSS_COLOR_INTERPOLATION_FILTERS,
    NS_CSS_COLOR_RENDERING,
    NS_CSS_FLOOD_COLOR,
    NS_CSS_FLOOD_OPACITY,
    NS_CSS_LIGHTING_COLOR,
    NS_CSS_MARKER_START,
    NS_CSS_MARKER_MID,
    NS_CSS_MARKER_END,
    NS_CSS_BASELINE_SHIFT,
    NS_CSS_ALIGNMENT_BASELINE,
    NS_CSS_BASELINE_SOURCE,
    NS_CSS_INITIAL_LETTER,
    NS_CSS_MARGIN_TRIM,
    NS_CSS_FIELD_SIZING,
    NS_CSS_INTERPOLATE_SIZE,
    NS_CSS_VIEW_TRANSITION_NAME,
    NS_CSS_VIEW_TRANSITION_CLASS,
    NS_CSS_WEBKIT_FONT_SMOOTHING,
    NS_CSS_WEBKIT_TAP_HIGHLIGHT_COLOR,
    NS_CSS_WEBKIT_TEXT_STROKE_WIDTH,
    NS_CSS_WEBKIT_TEXT_STROKE_COLOR,
    NS_CSS_OFFSET_PATH,
    NS_CSS_OFFSET_DISTANCE,
    NS_CSS_OFFSET_ROTATE,
    NS_CSS_OFFSET_ANCHOR,
    NS_CSS_OFFSET_POSITION,
    NS_CSS_PROP_COUNT,
} ns_css_prop;

int         ns_css_prop_id(const char *name);
const char *ns_css_prop_name(int prop);
gboolean    ns_css_prop_inherits(int prop);
gboolean    ns_css_prop_initial_currentcolor(int prop);

#define NS_CSS_KEYWORD_SHORTHAND_PARTS_MAX 8
int         ns_css_keyword_shorthand_parts(const char *name, int *props);
char       *ns_css_keyword_shorthand_compose(const char *name, char *const *values);
gboolean    ns_css_declaration_valid(int prop, const char *text);
gboolean    ns_css_named_property_supported(const char *name);
gboolean    ns_css_named_declaration_valid(const char *name, const char *text);

typedef enum ns_css_value_kind {
    NS_CSS_V_KEYWORD,
    NS_CSS_V_LENGTH,
    NS_CSS_V_SIZE,
    NS_CSS_V_COLOR,
    NS_CSS_V_CALC,
    NS_CSS_V_SHADOW,
    NS_CSS_V_GRADIENT,
    NS_CSS_V_TRACKS,
    NS_CSS_V_URL,
    NS_CSS_V_TRANSFORM,
    NS_CSS_V_AREAS,
    NS_CSS_V_ANIM,
    NS_CSS_V_RECT,
} ns_css_value_kind;

typedef enum ns_css_timing_kind {
    NS_CSS_TIMING_LINEAR,
    NS_CSS_TIMING_EASE,
    NS_CSS_TIMING_EASE_IN,
    NS_CSS_TIMING_EASE_OUT,
    NS_CSS_TIMING_EASE_IN_OUT,
    NS_CSS_TIMING_STEPS,
    NS_CSS_TIMING_CUBIC,
} ns_css_timing_kind;

typedef enum ns_css_step_pos {
    NS_CSS_STEP_JUMP_END,
    NS_CSS_STEP_JUMP_START,
    NS_CSS_STEP_JUMP_NONE,
    NS_CSS_STEP_JUMP_BOTH,
} ns_css_step_pos;

typedef struct ns_css_timing {
    ns_css_timing_kind kind;
    int                steps;
    ns_css_step_pos    step_pos;
    gboolean           jump_keyword;
    double             cb[4];
} ns_css_timing;

typedef enum ns_css_anim_target {
    NS_CSS_ANIM_TARGET_NONE,
    NS_CSS_ANIM_TARGET_ALL,
    NS_CSS_ANIM_TARGET_OPACITY,
    NS_CSS_ANIM_TARGET_TRANSFORM,
    NS_CSS_ANIM_TARGET_COLOR,
    NS_CSS_ANIM_TARGET_BG_COLOR,
    NS_CSS_ANIM_TARGET_OTHER,
} ns_css_anim_target;

typedef enum ns_css_anim_direction {
    NS_CSS_ANIM_DIR_NORMAL,
    NS_CSS_ANIM_DIR_REVERSE,
    NS_CSS_ANIM_DIR_ALTERNATE,
    NS_CSS_ANIM_DIR_ALTERNATE_REVERSE,
} ns_css_anim_direction;

typedef enum ns_css_anim_fill {
    NS_CSS_ANIM_FILL_NONE,
    NS_CSS_ANIM_FILL_FORWARDS,
    NS_CSS_ANIM_FILL_BACKWARDS,
    NS_CSS_ANIM_FILL_BOTH,
} ns_css_anim_fill;

typedef enum ns_css_timeline_kind {
    NS_CSS_TIMELINE_AUTO,
    NS_CSS_TIMELINE_NONE,
    NS_CSS_TIMELINE_NAMED,
    NS_CSS_TIMELINE_SCROLL,
    NS_CSS_TIMELINE_VIEW,
} ns_css_timeline_kind;

typedef enum ns_css_scroller {
    NS_CSS_SCROLLER_NEAREST,
    NS_CSS_SCROLLER_ROOT,
    NS_CSS_SCROLLER_SELF,
} ns_css_scroller;

typedef enum ns_css_scroll_axis {
    NS_CSS_SCROLL_AXIS_BLOCK,
    NS_CSS_SCROLL_AXIS_INLINE,
    NS_CSS_SCROLL_AXIS_X,
    NS_CSS_SCROLL_AXIS_Y,
} ns_css_scroll_axis;

typedef struct ns_css_timeline {
    ns_css_timeline_kind kind;
    ns_css_scroller      scroller;
    ns_css_scroll_axis   axis;
} ns_css_timeline;

typedef struct ns_css_anim_entry {
    ns_css_anim_target target;
    char         *name;
    double        duration_ms;
    double        delay_ms;
    ns_css_timing timing;
    double        iterations;
    ns_css_anim_direction direction;
    ns_css_anim_fill      fill;
    gboolean      paused;
    gboolean      duration_auto;
    gboolean      allow_discrete;
    ns_css_timeline timeline;
} ns_css_anim_entry;

#define NS_CSS_ANIM_ENTRIES_MAX 8

typedef struct ns_css_anim_list {
    int n;
    ns_css_anim_entry entries[NS_CSS_ANIM_ENTRIES_MAX];
} ns_css_anim_list;

struct ns_style;
gboolean ns_css_style_may_animate(const struct ns_style *s);
void  ns_css_anim_effective(const struct ns_style *s, gboolean is_animation,
                            ns_css_anim_list *out);
void  ns_css_anim_list_clear(ns_css_anim_list *list);
void  ns_css_anim_lists(const struct ns_style *s, gboolean is_animation,
                        ns_css_anim_list *out, gboolean *out_mismatch);
char *ns_css_anim_shorthand_serialize(const ns_css_anim_list *list,
                                      gboolean is_animation);
char *ns_css_animation_shorthand_canonical(const char *text, gboolean is_animation);
char *ns_css_ident_serialize(const char *name);
gboolean ns_css_timeline_parse(const char *item, ns_css_timeline *out);
gboolean ns_css_scroll_axis_parse(const char *item, ns_css_scroll_axis *out);
int   ns_css_timeline_name_index(const struct ns_style *s, ns_css_prop prop,
                                 const char *name);
char *ns_css_timeline_name_at(const struct ns_style *s, int index);
ns_css_scroll_axis ns_css_timeline_axis_at(const struct ns_style *s, int index);
char *ns_css_scroll_timeline_compose(const char *names, const char *axes);
char *ns_css_list_style_serialize(const char *type, const char *position,
                                  const char *image);
char *ns_css_grid_shorthand_compose(char *const values[6], gboolean full);
char *ns_css_grid_placement_compose(char *const values[4], gboolean area);
char *ns_css_animation_range_serialize(const char *start_list, const char *end_list);
char *ns_css_timing_serialize(const ns_css_timing *t);
const char *ns_css_initial_value_text(const char *name);
gboolean ns_css_timing_parse(const char *text, ns_css_timing *out);

typedef enum ns_css_transform_op_kind {
    NS_CSS_TFN_TRANSLATE,
    NS_CSS_TFN_ROTATE,
    NS_CSS_TFN_SCALE,
    NS_CSS_TFN_SKEW,
    NS_CSS_TFN_MATRIX,
    NS_CSS_TFN_MATRIX3D,
    NS_CSS_TFN_ROTATE3D,
    NS_CSS_TFN_PERSPECTIVE,
} ns_css_transform_op_kind;

typedef struct ns_css_transform_op {
    ns_css_transform_op_kind kind;
    double a, b, c, d, e, f;
    double m3d[16];
    gboolean a_is_percent, b_is_percent;
    double a_pct, b_pct;
    double em[3], rem[3];
} ns_css_transform_op;

#define NS_CSS_TRANSFORM_OPS_MAX 8

typedef struct ns_css_transform {
    int n_ops;
    ns_css_transform_op ops[NS_CSS_TRANSFORM_OPS_MAX];
} ns_css_transform;

gboolean ns_css_transform_is_3d(const ns_css_transform *tf);

typedef enum ns_css_track_kind {
    NS_CSS_TRACK_PX,
    NS_CSS_TRACK_PERCENT,
    NS_CSS_TRACK_FR,
    NS_CSS_TRACK_AUTO,
    NS_CSS_TRACK_MIN_CONTENT,
    NS_CSS_TRACK_MAX_CONTENT,
} ns_css_track_kind;

#define NS_CSS_TRACKS_MAX 24

typedef struct ns_css_track {
    ns_css_track_kind kind;
    double v;
    double em, rem, pct;
    ns_css_track_kind min_kind;
    double min_v;
    double min_em, min_rem, min_pct;
    gboolean has_min;
    gboolean fit_content;
} ns_css_track;

typedef enum ns_css_auto_repeat {
    NS_CSS_AUTO_REPEAT_NONE,
    NS_CSS_AUTO_REPEAT_FIT,
    NS_CSS_AUTO_REPEAT_FILL,
} ns_css_auto_repeat;

#define NS_CSS_LINE_NAMES_MAX 32

typedef struct ns_css_line_name {
    const char *name;
    int  line;
} ns_css_line_name;

typedef struct ns_css_tracks {
    int n;
    ns_css_track tracks[NS_CSS_TRACKS_MAX];
    ns_css_auto_repeat auto_repeat;
    int auto_repeat_start;
    int auto_repeat_count;
    int auto_repeat_names_start;
    int auto_repeat_names_end;
    gboolean subgrid;
    int n_line_names;
    ns_css_line_name line_names[NS_CSS_LINE_NAMES_MAX];
} ns_css_tracks;

typedef struct ns_css_area_rect {
    char *name;
    int r0, r1;
    int c0, c1;
} ns_css_area_rect;

#define NS_CSS_AREAS_MAX 32

typedef struct ns_css_areas {
    int n_rows;
    int n_cols;
    int n_rects;
    ns_css_area_rect rects[NS_CSS_AREAS_MAX];
} ns_css_areas;

typedef struct ns_css_shadow {
    double x, y, blur, spread;
    double em[4], rem[4];
    guint8 r, g, b, a;
    gboolean inset;
    gboolean currentcolor;
} ns_css_shadow;

#define NS_CSS_SHADOWS_MAX 8

typedef struct ns_css_shadow_list {
    int n;
    gboolean is_text;
    ns_css_shadow s[NS_CSS_SHADOWS_MAX];
} ns_css_shadow_list;

#define NS_CSS_GRADIENT_STOPS_MAX 32
#define NS_CSS_GRADIENT_INTERP_MAX 32

typedef struct ns_css_gradient_stop {
    guint8 r, g, b, a;
    double pos;
    double pos_px;
    gboolean has_pos;
    gboolean pos_is_angle;
    gboolean is_hint;
    gboolean pair_with_prev;
} ns_css_gradient_stop;

typedef enum ns_css_gradient_size {
    NS_CSS_GRADIENT_FARTHEST_CORNER,
    NS_CSS_GRADIENT_CLOSEST_SIDE,
    NS_CSS_GRADIENT_FARTHEST_SIDE,
    NS_CSS_GRADIENT_CLOSEST_CORNER,
    NS_CSS_GRADIENT_EXPLICIT_SIZE,
} ns_css_gradient_size;

enum {
    NS_CSS_GRADIENT_TO_TOP    = 1,
    NS_CSS_GRADIENT_TO_BOTTOM = 2,
    NS_CSS_GRADIENT_TO_LEFT   = 4,
    NS_CSS_GRADIENT_TO_RIGHT  = 8,
};

typedef struct ns_css_gradient {
    double angle_deg;
    int to_side;
    gboolean has_angle;
    int n_stops;
    gboolean radial;
    gboolean conic;
    gboolean repeating;
    gboolean circle;
    ns_css_gradient_size size;
    double size_x, size_y;
    double size_x_pct, size_y_pct;
    double from_deg;
    gboolean has_from;
    double center_x, center_y;
    double center_x_px, center_y_px;
    gboolean has_center;
    char interp[NS_CSS_GRADIENT_INTERP_MAX];
    ns_css_gradient_stop stops[NS_CSS_GRADIENT_STOPS_MAX];
} ns_css_gradient;

typedef enum ns_css_unit {
    NS_CSS_UNIT_PX,
    NS_CSS_UNIT_EM,
    NS_CSS_UNIT_REM,
    NS_CSS_UNIT_PERCENT,
    NS_CSS_UNIT_NUMBER,
    NS_CSS_UNIT_VW,
    NS_CSS_UNIT_VH,
    NS_CSS_UNIT_VMIN,
    NS_CSS_UNIT_VMAX,
    NS_CSS_UNIT_CQW,
    NS_CSS_UNIT_CQH,
    NS_CSS_UNIT_CQMIN,
    NS_CSS_UNIT_CQMAX,
    NS_CSS_UNIT_EX,
    NS_CSS_UNIT_CH,
    NS_CSS_UNIT_CAP,
    NS_CSS_UNIT_IC,
    NS_CSS_UNIT_LH,
    NS_CSS_UNIT_RLH,
    NS_CSS_UNIT_REX,
    NS_CSS_UNIT_RCH,
    NS_CSS_UNIT_RCAP,
    NS_CSS_UNIT_RIC,
} ns_css_unit;

void     ns_css_set_viewport(double vw_px, double vh_px);
void     ns_css_set_frame_viewport_cb(
             void (*cb)(const ns_node *frame, double *w, double *h));
double   ns_css_viewport_w(void);
double   ns_css_viewport_h(void);
double   ns_css_container_w(void);
double   ns_css_container_h(void);

typedef enum ns_css_color_scheme {
    NS_CSS_COLOR_SCHEME_LIGHT,
    NS_CSS_COLOR_SCHEME_DARK,
} ns_css_color_scheme;

typedef enum ns_css_reduced_motion {
    NS_CSS_REDUCED_MOTION_NO_PREFERENCE,
    NS_CSS_REDUCED_MOTION_REDUCE,
} ns_css_reduced_motion;

ns_css_reduced_motion ns_css_get_reduced_motion(void);
ns_css_color_scheme ns_css_get_color_scheme(void);
void ns_css_set_reduced_motion(ns_css_reduced_motion motion);
void ns_css_set_color_scheme(ns_css_color_scheme scheme);

char    *ns_css_media_list_serialize(const char *query);
void     ns_css_media_viewport_push(double w, double h);
void     ns_css_media_viewport_pop(void);
double   ns_css_media_viewport_current_w(void);
double   ns_css_media_viewport_current_h(void);
void     ns_css_set_device_size(double w, double h);
void     ns_css_set_device_pixel_ratio(double dppx);
double   ns_css_device_pixel_ratio(void);
void     ns_css_set_print_media(gboolean printing);

typedef struct ns_css_value {
    ns_css_value_kind kind;
    int ref;
    union {
        char *keyword;
        struct { double v; ns_css_unit unit; } length;
        struct { double w, h; ns_css_unit w_unit, h_unit; gboolean w_auto, h_auto; } size;
        struct { guint8 r, g, b, a; } color;
        struct {
            double pct; double px; double em; double rem; double lh;
            double vw, vh, vmin, vmax;
            double parsed_vw, parsed_vh;
            guint8 fn;
            guint8 n_args;
            guint8 arg_none;
            struct { double px, pct, em, rem; } args[4];
        } calc;
        ns_css_shadow_list shadow;
        ns_css_gradient  gradient;
        ns_css_tracks    tracks;
        char            *url;
        ns_css_transform transform;
        ns_css_areas     areas;
        ns_css_anim_list anim;
        struct { double v[4]; ns_css_unit unit[4]; gboolean is_auto[4]; } rect;
    } u;
    char *image_set_text;
    char *specified;
    struct ns_css_value *next_layer;
} ns_css_value;

int                 ns_css_font_stretch_rank(const ns_css_value *v);
const ns_css_value *ns_css_value_layer(const ns_css_value *head, int index);
int                 ns_css_value_layer_count(const ns_css_value *head);

double   ns_css_length_or(const ns_css_value *v, double fallback);
gboolean ns_css_calc_is_math_fn(const ns_css_value *v);
double   ns_css_calc_math_fn_px(const ns_css_value *v, double basis);
gboolean ns_css_keyword_is(const ns_css_value *v, const char *kw);
gboolean ns_css_filter_list_valid(const char *s);
char    *ns_css_font_family_for_pango(const char *css_family);
char    *ns_css_font_family_first_for_pango(const char *css_family);
char    *ns_css_font_family_first_name(const char *css_family);
void     ns_css_set_font_available_cb(gboolean (*cb)(const char *family));
void     ns_css_set_font_alias_cb(char *(*cb)(const char *family));
void     ns_css_set_font_family_name_cb(char *(*cb)(const char *family));
void     ns_css_set_font_generation_cb(guint64 (*cb)(void));
int      ns_css_font_weight_number(const ns_css_value *v, int fallback);

typedef struct ns_css_font_metrics {
    double ex_px;
    double ch_px;
    double cap_px;
    double ic_px;
    double line_px;
    double ascent_px;
    double descent_px;
} ns_css_font_metrics;

void ns_css_set_font_metrics_cb(
    void (*cb)(const char *family, double size_px, int weight,
               gboolean italic, ns_css_font_metrics *out));

typedef enum ns_css_attr_op {
    NS_CSS_ATTR_PRESENT,
    NS_CSS_ATTR_EQ,
    NS_CSS_ATTR_PREFIX,
    NS_CSS_ATTR_SUFFIX,
    NS_CSS_ATTR_SUBSTR,
    NS_CSS_ATTR_WORD,
    NS_CSS_ATTR_HYPHEN,
} ns_css_attr_op;

typedef struct ns_css_attr_pred {
    char *name;
    ns_css_attr_op op;
    char *value;
    gboolean case_insensitive;
    gboolean case_sensitive;
    gboolean html_ci;
    guint64 name_bit;
} ns_css_attr_pred;

typedef enum ns_css_pseudo {
    NS_CSS_PC_FIRST_CHILD,
    NS_CSS_PC_LAST_CHILD,
    NS_CSS_PC_ONLY_CHILD,
    NS_CSS_PC_ONLY_OF_TYPE,
    NS_CSS_PC_FIRST_OF_TYPE,
    NS_CSS_PC_LAST_OF_TYPE,
    NS_CSS_PC_EMPTY,
    NS_CSS_PC_ROOT,
    NS_CSS_PC_CHECKED,
    NS_CSS_PC_DISABLED,
    NS_CSS_PC_ENABLED,
    NS_CSS_PC_REQUIRED,
    NS_CSS_PC_OPTIONAL,
    NS_CSS_PC_VALID,
    NS_CSS_PC_INVALID,
    NS_CSS_PC_IN_RANGE,
    NS_CSS_PC_OUT_OF_RANGE,
    NS_CSS_PC_DEFAULT,
    NS_CSS_PC_INDETERMINATE,
    NS_CSS_PC_NTH_CHILD,
    NS_CSS_PC_NTH_LAST_CHILD,
    NS_CSS_PC_NTH_OF_TYPE,
    NS_CSS_PC_NTH_LAST_OF_TYPE,
    NS_CSS_PC_LINK,
    NS_CSS_PC_VISITED,
    NS_CSS_PC_ANY_LINK,
    NS_CSS_PC_HOVER,
    NS_CSS_PC_ACTIVE,
    NS_CSS_PC_FOCUS,
    NS_CSS_PC_FOCUS_VISIBLE,
    NS_CSS_PC_FOCUS_WITHIN,
    NS_CSS_PC_TARGET,
    NS_CSS_PC_TARGET_WITHIN,
    NS_CSS_PC_DEFINED,
    NS_CSS_PC_SCOPE,
    NS_CSS_PC_PLACEHOLDER_SHOWN,
    NS_CSS_PC_READ_ONLY,
    NS_CSS_PC_READ_WRITE,
    NS_CSS_PC_BLANK,
    NS_CSS_PC_LANG,
    NS_CSS_PC_DIR,
    NS_CSS_PC_OPEN,
    NS_CSS_PC_POPOVER_OPEN,
    NS_CSS_PC_MODAL,
    NS_CSS_PC_FULLSCREEN,
    NS_CSS_PC_HEADING,
    NS_CSS_PC_USER_VALID,
    NS_CSS_PC_USER_INVALID,
    NS_CSS_PC_AUTOFILL,
    NS_CSS_PC_PLAYING,
    NS_CSS_PC_PAUSED,
    NS_CSS_PC_MUTED,
    NS_CSS_PC_SEEKING,
    NS_CSS_PC_BUFFERING,
    NS_CSS_PC_STALLED,
} ns_css_pseudo;

typedef struct ns_css_pseudo_pred {
    ns_css_pseudo kind;
    int a, b;
    char *arg;
    GPtrArray *of_group;
} ns_css_pseudo_pred;

typedef struct ns_css_simple {
    char *type;
    char *type_cased;
    char *id;
    GPtrArray *classes;
    GArray    *class_lens;
    GArray    *attrs;
    GArray    *pseudos;
    GPtrArray *matches_any;
    GPtrArray *matches_none;
    GPtrArray *has_groups;
    gboolean   never_match;
    gboolean   known_unmatched;
    gboolean   ns_none;
} ns_css_simple;

typedef enum ns_css_comb {
    NS_CSS_COMB_NONE,
    NS_CSS_COMB_DESCENDANT,
    NS_CSS_COMB_CHILD,
    NS_CSS_COMB_ADJACENT,
    NS_CSS_COMB_SIBLING,
} ns_css_comb;

typedef enum ns_css_pseudo_element {
    NS_CSS_PE_NONE,
    NS_CSS_PE_BEFORE,
    NS_CSS_PE_AFTER,
    NS_CSS_PE_FIRST_LETTER,
    NS_CSS_PE_FIRST_LINE,
    NS_CSS_PE_SELECTION,
    NS_CSS_PE_MARKER,
    NS_CSS_PE_BACKDROP,
    NS_CSS_PE_PLACEHOLDER,
    NS_CSS_PE_FILE_SELECTOR_BUTTON,
} ns_css_pseudo_element;

typedef struct ns_css_selector {
    GPtrArray *compounds;
    GArray    *combinators;

    ns_css_pseudo_element pseudo_element;

    int spec_a, spec_b, spec_c;

    guint32 ancestor_hashes[4];
    guint   n_ancestor_hashes;
    guint   n_ancestor_attr_hashes;
} ns_css_selector;

GPtrArray *ns_css_parse_selector_list(const char *text);
GPtrArray *ns_css_parse_selector_list_checked(const char *text,
                                              gboolean *out_valid);

const ns_node *ns_css_set_match_scope(const ns_node *scope);
void ns_css_selector_batch_begin(void);
void ns_css_selector_batch_end(void);

gboolean   ns_css_selector_matches(const ns_css_selector *sel, const ns_node *el);
const char *ns_css_node_dir(const ns_node *el);

gboolean   ns_css_media_query_matches(const char *query);

double     ns_css_sizes_resolve(const char *sizes);

void       ns_css_register_defined_element(const char *tag);
void       ns_css_clear_defined_elements(void);

char *ns_inline_style_get(const char *style_text, const char *prop_name);
char *ns_inline_style_set(const char *style_text, const char *prop_name, const char *value);
char *ns_inline_style_serialize(const char *style_text);
const GPtrArray *ns_inline_style_names(const char *style_text);
gboolean ns_inline_value_strip_important(char *value);

typedef struct ns_css_decl {
    ns_css_prop prop;
    ns_css_value *value;
    gboolean important;
} ns_css_decl;

typedef struct ns_css_pending_decl {
    char     *pname;
    char     *raw_vtext;
    gboolean  important;
    int       decl_index;
    int       decl_rank;
} ns_css_pending_decl;

typedef struct ns_css_container_query ns_css_container_query;

typedef struct ns_css_rule {
    GPtrArray  *selectors;
    GArray     *decls;
    GHashTable *vars;
    GHashTable *var_important;
    GArray     *pending;
    char       *lazy_decls;
    char       *layer_name;
    char       *container_condition;
    ns_css_container_query *container_query;
    GPtrArray  *scopes;
    int         source_order;
    guint       pe_mask;
} ns_css_rule;

typedef struct ns_css_import {
    char *url;
    char *layer_name;
    char *media;
} ns_css_import;

typedef struct ns_css_font_face {
    char *family;
    char *src_url;
    char *unicode_range;
    ns_font_descriptors descriptors;
} ns_css_font_face;

typedef struct ns_css_property_rule {
    char *name;
    char *initial_value;
    char *syntax_text;
    ns_css_syntax_def *syntax;
    gboolean inherits;
    gboolean has_initial;
} ns_css_property_rule;

typedef enum ns_css_register_status {
    NS_CSS_REGISTER_OK,
    NS_CSS_REGISTER_BAD_NAME,
    NS_CSS_REGISTER_BAD_SYNTAX,
    NS_CSS_REGISTER_BAD_INITIAL,
    NS_CSS_REGISTER_EXISTS,
} ns_css_register_status;

ns_css_register_status ns_css_register_property(const char *name,
                                                const char *syntax_text,
                                                gboolean inherits,
                                                const char *initial_value,
                                                gboolean has_initial);
void ns_css_clear_registered_properties(void);

typedef struct ns_css_keyframe_stop {
    double pct;
    char *raw_props;
} ns_css_keyframe_stop;

typedef struct ns_css_keyframes {
    char *name;
    int n_stops;
    ns_css_keyframe_stop *stops;
} ns_css_keyframes;

struct ns_css_rule_index;

typedef struct ns_css_page_rule {
    double   width, height;
    gboolean has_size;
    double   margin[4];
    gboolean has_margin[4];
} ns_css_page_rule;

typedef struct ns_css_stylesheet {
    GPtrArray *rules;
    GArray    *imports;
    GPtrArray *layer_names;
    GHashTable *layers;
    GArray    *font_faces;
    GArray    *keyframes;
    GArray    *property_rules;
    ns_css_page_rule *page_rule;
    gboolean   has_container_rules;
    gboolean   has_container_units;
    gboolean   has_hover_rules;
    gboolean   has_active_rules;
    gboolean   lazy_decls;
    gboolean   cached;
    guint      pseudo_mask;
    guint64    serial;
    char      *resolved_base;
    struct ns_css_rule_index *index;
} ns_css_stylesheet;

gboolean ns_css_stylesheet_has_container_rules(const ns_css_stylesheet *sh);
gboolean ns_css_stylesheet_has_container_units(const ns_css_stylesheet *sh);
gboolean ns_css_stylesheet_has_hover_rules(const ns_css_stylesheet *sh);
gboolean ns_css_text_has_container_units(const char *text, gssize len);
gboolean ns_css_stylesheet_has_active_rules(const ns_css_stylesheet *sh);

typedef void (*ns_css_match_fn)(const ns_css_rule *rule, guint rule_index,
                                const ns_css_selector *sel, gpointer data);
void ns_css_stylesheet_each_match(const ns_css_stylesheet *sheet,
                                  const ns_node *el, ns_css_match_fn fn,
                                  gpointer data);
GHashTable *ns_css_layer_ranks_new(const ns_css_stylesheet *const *sheets,
                                   gsize n);
int         ns_css_layer_rank(GHashTable *ranks, const char *layer_name);

#define NS_CSS_IMPORT_MAX_DEPTH 8

ns_css_stylesheet *ns_css_stylesheet_parse(const char *text, gssize len);
ns_css_stylesheet *ns_css_stylesheet_parse_author(const char *text, gssize len);
const ns_css_stylesheet *ns_css_ua_stylesheet(void);
ns_css_stylesheet *ns_css_stylesheet_parse_detached(const char *text, gssize len);
gboolean           ns_css_supports_declaration(const char *property,
                                               const char *value);
gboolean           ns_css_supports_condition(const char *condition,
                                             gboolean allow_bare_declaration);
ns_css_stylesheet *ns_css_stylesheet_from_style_element_cached(ns_node *style);
char              *ns_css_style_element_text(ns_node *style);
char              *ns_css_shadow_adopted_css(ns_node *root);
guint              ns_css_stylesheet_cache_generation(void);
typedef void (*ns_css_text_builder)(GString *out, gpointer data);
ns_css_stylesheet *ns_css_merged_styles_cached_digest(guint64 digest, gsize len,
                                                      const char *base_url,
                                                      ns_css_text_builder build,
                                                      gpointer data);
ns_css_stylesheet *ns_css_stylesheet_parse_url_cached(const char *url,
                                                      const char *css,
                                                      gssize len);
ns_css_stylesheet *ns_css_stylesheet_parse_import_cached(const char *url,
                                                         const char *layer_name,
                                                         GBytes *bytes);
void               ns_css_style_element_cache_begin(void);
void               ns_css_stylesheet_cache_drop(void);
void               ns_css_style_element_cache_end(void);
void               ns_css_relayout_enter(void);
void               ns_css_relayout_leave(void);
void               ns_css_stylesheet_resolve_urls(ns_css_stylesheet *s,
                                                  const char *base_url);
void               ns_css_stylesheet_free(ns_css_stylesheet *s);
void               ns_css_stylesheet_force_layer(ns_css_stylesheet *s,
                                                 const char *layer_name);

typedef enum ns_display_box {
    NS_DISPLAY_BOX_NORMAL,
    NS_DISPLAY_BOX_NONE,
    NS_DISPLAY_BOX_CONTENTS,
} ns_display_box;

typedef enum ns_display_outer {
    NS_DISPLAY_OUTER_INLINE,
    NS_DISPLAY_OUTER_BLOCK,
    NS_DISPLAY_OUTER_RUN_IN,
} ns_display_outer;

typedef enum ns_display_inner {
    NS_DISPLAY_INNER_FLOW,
    NS_DISPLAY_INNER_FLOW_ROOT,
    NS_DISPLAY_INNER_TABLE,
    NS_DISPLAY_INNER_FLEX,
    NS_DISPLAY_INNER_GRID,
    NS_DISPLAY_INNER_RUBY,
} ns_display_inner;

typedef enum ns_display_internal {
    NS_DISPLAY_INTERNAL_NONE,
    NS_DISPLAY_INTERNAL_TABLE_ROW_GROUP,
    NS_DISPLAY_INTERNAL_TABLE_HEADER_GROUP,
    NS_DISPLAY_INTERNAL_TABLE_FOOTER_GROUP,
    NS_DISPLAY_INTERNAL_TABLE_ROW,
    NS_DISPLAY_INTERNAL_TABLE_CELL,
    NS_DISPLAY_INTERNAL_TABLE_COLUMN_GROUP,
    NS_DISPLAY_INTERNAL_TABLE_COLUMN,
    NS_DISPLAY_INTERNAL_TABLE_CAPTION,
    NS_DISPLAY_INTERNAL_RUBY_BASE,
    NS_DISPLAY_INTERNAL_RUBY_TEXT,
} ns_display_internal;

typedef struct ns_display {
    guint8 box;
    guint8 outer;
    guint8 inner;
    guint8 internal;
    guint8 list_item;
} ns_display;

typedef struct ns_style {
    ns_css_value *values[NS_CSS_PROP_COUNT];
    ns_display display;
    guint8 specified_inline;
    struct ns_style *before;
    struct ns_style *after;
    struct ns_style *first_letter;
    struct ns_style *first_line;
    struct ns_style *placeholder;
    struct ns_style *selection;
    struct ns_style *marker;
    struct ns_style *backdrop;
    struct ns_style *file_selector_button;
    struct ns_style *hidden_before;
    struct ns_style *hidden_after;
    guint64 share_id;
    int   ref;
    guint32 currentcolor_bits;
    guint32 inherited_bits;
    guint8 animation_hint;
    guint8 explicit_inherit;
    guint8 env_dependent;
    struct ns_var_map *vars;
} ns_style;

gboolean ns_style_prop_from_currentcolor(const ns_style *s, int prop);
gboolean ns_style_prop_inherited(const ns_style *s, int prop);
void     ns_css_value_absolutize(ns_css_value *v, int prop, const ns_style *st);
const ns_css_value *ns_style_text_fill_color(const ns_style *s);

void   ns_style_free(ns_style *s);
ns_style *ns_style_clone(const ns_style *src);
const char *ns_css_position_try_rule(const char *name);
double ns_css_dimension_px(const ns_css_value *v, double font_size,
                           double basis);
const ns_style *ns_css_style_before_change(const void *node);

ns_display ns_css_display_of(const ns_style *s);
ns_display ns_css_display_from_keyword(const char *canonical);
ns_display ns_css_display_blockified(ns_display d);
char      *ns_css_display_serialize(ns_display d);

typedef enum ns_border_image_tile {
    NS_BORDER_IMAGE_STRETCH,
    NS_BORDER_IMAGE_REPEAT,
    NS_BORDER_IMAGE_ROUND,
    NS_BORDER_IMAGE_SPACE,
} ns_border_image_tile;

typedef struct ns_border_image {
    double               slice[4];
    gboolean             slice_percent[4];
    gboolean             fill;
    double               width[4];
    ns_css_unit          width_unit[4];
    gboolean             width_auto[4];
    double               outset[4];
    ns_css_unit          outset_unit[4];
    ns_border_image_tile tile[2];
} ns_border_image;

const ns_css_value *ns_css_border_image_source(const ns_style *s);
void ns_css_border_image_params(const ns_style *s, ns_border_image *out);

static inline gboolean
ns_display_is_none(ns_display d)
{
    return d.box == NS_DISPLAY_BOX_NONE;
}

static inline gboolean
ns_display_is_contents(ns_display d)
{
    return d.box == NS_DISPLAY_BOX_CONTENTS;
}

static inline gboolean
ns_display_is_internal(ns_display d)
{
    return d.box == NS_DISPLAY_BOX_NORMAL &&
           d.internal != NS_DISPLAY_INTERNAL_NONE;
}

static inline gboolean
ns_display_is_table_internal(ns_display d)
{
    return ns_display_is_internal(d) &&
           d.internal <= NS_DISPLAY_INTERNAL_TABLE_CAPTION;
}

static inline gboolean
ns_display_is(ns_display d, ns_display_internal kind)
{
    return ns_display_is_internal(d) && d.internal == kind;
}

static inline gboolean
ns_display_is_block_level(ns_display d)
{
    return d.box == NS_DISPLAY_BOX_NORMAL &&
           d.outer == NS_DISPLAY_OUTER_BLOCK &&
           d.inner != NS_DISPLAY_INNER_RUBY &&
           (d.internal == NS_DISPLAY_INTERNAL_NONE ||
            d.internal == NS_DISPLAY_INTERNAL_TABLE_CAPTION);
}

static inline gboolean
ns_display_is_atomic_inline(ns_display d)
{
    return d.box == NS_DISPLAY_BOX_NORMAL &&
           d.internal == NS_DISPLAY_INTERNAL_NONE &&
           d.outer == NS_DISPLAY_OUTER_INLINE &&
           d.inner != NS_DISPLAY_INNER_FLOW &&
           d.inner != NS_DISPLAY_INNER_RUBY;
}

static inline gboolean
ns_display_generates_own_box(ns_display d)
{
    if (d.box == NS_DISPLAY_BOX_NONE) return FALSE;
    if (d.box == NS_DISPLAY_BOX_CONTENTS) return TRUE;
    return ns_display_is_block_level(d) || ns_display_is_atomic_inline(d);
}

static inline gboolean
ns_display_inner_is(ns_display d, ns_display_inner inner)
{
    return d.box == NS_DISPLAY_BOX_NORMAL &&
           d.internal == NS_DISPLAY_INTERNAL_NONE && d.inner == inner;
}

static inline gboolean
ns_display_is_flex_container(ns_display d)
{
    return ns_display_inner_is(d, NS_DISPLAY_INNER_FLEX);
}

static inline gboolean
ns_display_is_grid_container(ns_display d)
{
    return ns_display_inner_is(d, NS_DISPLAY_INNER_GRID);
}

static inline gboolean
ns_display_is_table_wrapper(ns_display d)
{
    return ns_display_inner_is(d, NS_DISPLAY_INNER_TABLE);
}

int ns_css_writing_mode(const ns_style *s);
int ns_css_text_orientation(const ns_style *s);

const char *ns_var_map_lookup(const struct ns_var_map *m, const char *name);
GPtrArray  *ns_var_map_names(const struct ns_var_map *m);
char *ns_css_resolve_style_vars(const char *text, const ns_style *style);

void ns_css_style_effective_transform(const ns_style *st,
                                      const ns_css_transform *transform_override,
                                      ns_css_transform *out);
void ns_css_transform_to_mat4(const ns_css_transform *tf,
                              double bw, double bh, ns_mat4 *out);

ns_css_keyframes *ns_css_keyframes_resolve(const ns_css_keyframes *kf,
                                           const struct ns_var_map *vars);
void ns_css_keyframes_resolved_free(ns_css_keyframes *kf);

void ns_css_append_unescaped(GString *out, const char **pp);
gboolean ns_css_number_value(const char *text, double *out);

GHashTable *ns_css_compute(ns_node                 *doc,
                           const ns_css_stylesheet *const *author_sheets,
                           const ns_node *const    *sheet_docs,
                           gsize                     n_sheets);
void ns_css_selector_cache_begin(void);
void ns_css_selector_cache_end(void);

void ns_css_mark_restyle_dirty(ns_node *parent);
void ns_css_mark_childlist_dirty(ns_node *parent, ns_node *added);
void ns_css_mark_childlist_replaced(ns_node *parent, ns_node *added,
                                    ns_node *const *removed, guint n_removed);
void ns_css_mark_childlist_removed(ns_node *parent, ns_node *const *removed,
                                  guint n_removed, ns_node *next_sibling,
                                  ns_node *previous_sibling);
void ns_css_mark_attr_dirty(ns_node *target, const char *name,
                            const char *old_value);
gboolean ns_css_attr_may_affect_style(const ns_node *target, const char *name);
void ns_css_set_render_zoom(double zoom);
void ns_css_style_scale_font_size(ns_style *s, double factor);

void ns_css_set_container_map(GHashTable *map);
void ns_css_set_container_dims(double inline_px, double block_px);
void ns_css_container_features_begin(void);
gboolean ns_css_container_features_used(void);
GHashTable *ns_css_container_map_new(void);
gboolean ns_css_container_maps_equal(GHashTable *a, GHashTable *b);
void ns_css_container_map_add(GHashTable *map, const void *node,
                              const char *type_kw, const char *name_kw,
                              double w, double h, gboolean vertical);

void ns_css_set_target_fragment(const char *fragment);

const ns_node *ns_css_set_focus_node(const ns_node *node);
void ns_css_set_focus_visible_node(const ns_node *node);
const ns_node *ns_css_set_hover_node(const ns_node *node);
const ns_node *ns_css_set_active_node(const ns_node *node);
const ns_node *ns_css_set_fullscreen_node(const ns_node *node);
void ns_css_forget_node(const ns_node *node);

void ns_css_mark_visited(const char *abs_url);
void ns_css_set_doc_base(const char *base_url);
int           ns_css_font_face_weight(const char *val);
ns_font_slant ns_css_font_face_slant(const char *val);
char         *ns_css_font_face_src_url(const char *src);
void ns_css_set_doc_language(const char *lang);

const char *ns_style_keyword(const ns_style *s, ns_css_prop p);
const char *ns_style_overflow_keyword(const ns_style *s, ns_css_prop axis);
const char *ns_css_alignment_base(const char *keyword);
char *ns_css_font_family_canonical(const char *text);
char *ns_css_font_shorthand_canonical(const char *text);
char *ns_css_image_value_canonical(const char *text);
char *ns_css_background_position_join(const char *xs, const char *ys);
char *ns_css_background_shorthand_serialize(const char *image, const char *position,
                                            const char *size, const char *repeat,
                                            const char *attachment,
                                            const char *origin, const char *clip,
                                            const char *color);
char *ns_css_content_canonical(const char *text);
ns_css_value *ns_css_value_interpolate(const ns_css_value *a, const ns_css_value *b, double t);
ns_css_value *ns_css_value_interpolate_prop(int prop, const ns_css_value *a,
                                            const ns_css_value *b, double t);
ns_css_value *ns_css_value_composite(int prop, const ns_css_value *underlying,
                                     const ns_css_value *v, gboolean accumulate);
void          ns_css_value_clamp_animated(int prop, ns_css_value *v);
gboolean ns_css_value_equal(const ns_css_value *a, const ns_css_value *b);
ns_css_value *ns_css_value_dup(const ns_css_value *v);
void ns_css_value_free(ns_css_value *v);
GArray *ns_css_parse_declarations(const char *text);
void ns_css_declarations_free(GArray *decls);
void ns_css_incremental_exclude(const void *node, gboolean exclude);
char *ns_css_unicode_range_canonical(const char *text);
char *ns_css_container_condition_canonical(const char *cond);
char *ns_css_container_name_canonical(const char *text);
char *ns_css_container_shorthand_canonical(const char *text);
double ns_css_gradient_angle(const ns_css_gradient *gr, double w, double h);
void ns_css_gradient_radii(const ns_css_gradient *gr, double w, double h,
                           double cx, double cy, double *rx, double *ry);

int ns_css_used_column_count(const ns_style *s, double avail_w,
                             double *out_gap);

char *ns_css_value_serialize(const ns_css_value *v);
char *ns_css_value_serialize_specified(const ns_css_value *v);
char *ns_css_individual_transform_serialize(const ns_css_value *v, int prop);
char *ns_css_math_canonical(const char *value);
char *ns_css_font_size_adjust_computed(const ns_style *s);
double ns_css_contain_intrinsic_px(const ns_style *s, gboolean height);
char *ns_css_contain_intrinsic_computed(const ns_style *s, int prop);
char *ns_css_shape_computed_serialize(const ns_style *s, const ns_style *root, int prop);
char *ns_css_tracks_computed_serialize(const ns_style *s, const ns_style *root,
                                       int prop);
char *ns_css_transform_canonical(const char *value);
char *ns_css_display_canonical(const char *value);
char *ns_css_specified_canonical(const char *prop, const char *value);
char *ns_css_time_specified(const char *value);
char *ns_css_time_computed(const char *value);

gboolean ns_css_parse_color(const char *s, guint8 *r, guint8 *g, guint8 *b,
                            guint8 *a);
gboolean ns_css_named_color(const char *name, guint8 *r, guint8 *g, guint8 *b);

G_END_DECLS

#endif
