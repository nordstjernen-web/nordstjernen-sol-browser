/* Nordstjernen: the page as cached tile and viewport-layer textures. */

#ifndef NS_PAGELAYERS_H
#define NS_PAGELAYERS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct NsPageLayers NsPageLayers;
typedef struct NsLayerUpdate NsLayerUpdate;

NsLayerUpdate *ns_layer_update_parse(const char *desc,
                                     const unsigned char *map,
                                     size_t map_size);
void ns_layer_update_free(NsLayerUpdate *u);

NsPageLayers *ns_page_layers_new(void);
void ns_page_layers_free(NsPageLayers *pl);
void ns_page_layers_reset(NsPageLayers *pl);
gboolean ns_page_layers_apply(NsPageLayers *pl, NsLayerUpdate *u);
gboolean ns_page_layers_active(const NsPageLayers *pl);
gboolean ns_page_layers_advance_videos(NsPageLayers *pl, gint64 now_us);
int ns_page_layers_gen(const NsPageLayers *pl);
int ns_page_layers_vp_held(const NsPageLayers *pl);
char *ns_page_layers_video_held(const NsPageLayers *pl);
char *ns_page_layers_have(const NsPageLayers *pl, double y0, double y1,
                          gboolean any_gen);
gboolean ns_page_layers_missing(const NsPageLayers *pl, double y0, double y1,
                                double view_y0, double view_y1,
                                double page_h);
void ns_page_layers_evict(NsPageLayers *pl, double y0, double y1);
gboolean ns_page_layers_scroller_at(const NsPageLayers *pl, double x,
                                    double y, double scroll_x,
                                    double scroll_y, gboolean vertical);
gboolean ns_page_layers_wheel_listener_at(const NsPageLayers *pl, double x,
                                          double y, double scroll_x,
                                          double scroll_y);

void ns_page_layers_snapshot(const NsPageLayers *pl, GtkSnapshot *snapshot,
                             double width, double height, double raster,
                             double scroll_x, double scroll_y);

G_END_DECLS

#endif
