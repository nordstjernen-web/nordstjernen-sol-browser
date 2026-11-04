/* Nordstjernen: paint a page as scroll-layer tiles and viewport layers. */

#ifndef NS_LAYERS_H
#define NS_LAYERS_H

#include <glib.h>

#include "libnordstjernen.h"
#include "paint.h"
#include "texture.h"
#include "video.h"

G_BEGIN_DECLS

#define NS_VP_LAYER_PAD 24

typedef struct ns_vp_layer_info {
    int kind;
    double top, bottom;
    double x_offset;
    ns_sticky_y sticky;
} ns_vp_layer_info;

void ns_browser_note_viewport(ns_browser *browser, int scroll_x, int scroll_y,
                              int height, double scale);
int ns_browser_layers_prepare(ns_browser *browser, int scroll_x, int scroll_y,
                              int width, int height, double scale,
                              ns_paint_layer_plan *plan);
int ns_browser_render_doc_tile(ns_browser *browser,
                               const ns_paint_layer_plan *plan, int scroll_x,
                               int tile_y, int width, int height,
                               double scale, unsigned char *const *bufs,
                               int stride, gboolean *upper_used);
int ns_browser_vp_layer_info(ns_browser *browser,
                             const ns_paint_layer_plan *plan, int index,
                             ns_vp_layer_info *out);
int ns_browser_render_vp_layer(ns_browser *browser,
                               const ns_paint_layer_plan *plan, int index,
                               int scroll_x, int scroll_y, int origin_y,
                               int width, int height, double scale,
                               unsigned char *out, int stride);
gboolean ns_browser_canvas_color(ns_browser *browser, double rgba_out[4]);

typedef struct ns_video_layer {
    guint              id;
    double             x, y, w, h;
    int                slot;
    ns_video_frame_ref frames[NS_VIDEO_LAYER_FRAMES];
    guint              n_frames;
    guint             *sent_serial;
} ns_video_layer;

void ns_browser_set_video_layer_gen(ns_browser *browser, guint gen);
gboolean ns_browser_video_layer_frames_pending(ns_browser *browser);
void ns_browser_foreach_video_layer(ns_browser *browser,
                                    void (*fn)(const ns_video_layer *layer,
                                               gpointer data),
                                    gpointer data);
void ns_browser_scroller_rects(ns_browser *browser, GString *out,
                               int max_rects);

G_END_DECLS

#endif
