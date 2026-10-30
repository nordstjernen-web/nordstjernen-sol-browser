/* Northstar — in-engine SVG rendering API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_SVG_H
#define NS_SVG_H

#include <cairo.h>
#include <glib.h>

#include "dom.h"
#include "texture.h"

G_BEGIN_DECLS

struct ns_style;

typedef struct ns_svg_size {
    double   width;
    double   height;
    double   ratio;
    gboolean has_width;
    gboolean has_height;
    gboolean has_ratio;
} ns_svg_size;

typedef struct ns_svg_geometry {
    gboolean       found;
    gboolean       rendered;
    gboolean       has_box;
    double         x, y, width, height;
    cairo_matrix_t to_root;
    cairo_matrix_t to_viewport;
} ns_svg_geometry;

gboolean ns_svg_node_is_root(const ns_node *n);

const ns_node *ns_svg_document_root(const ns_node *doc);

void ns_svg_intrinsic_size(const ns_node *svg, ns_svg_size *out);

typedef void (*ns_svg_foreign_painter)(cairo_t *cr, const ns_node *fo,
                                       double width, double height,
                                       void *user_data);
void ns_svg_set_foreign_painter(ns_svg_foreign_painter painter,
                                void *user_data);

void ns_svg_render_node(cairo_t          *cr,
                        const ns_node    *svg,
                        double            width,
                        double            height,
                        GHashTable       *styles,
                        const struct ns_style *inherited);

gboolean ns_svg_node_geometry(const ns_node *svg, const ns_node *target,
                              double width, double height,
                              GHashTable *styles,
                              const struct ns_style *inherited,
                              ns_svg_geometry *out);

ns_texture *ns_svg_decode_bytes(const guchar *data, gsize len,
                                int *out_w, int *out_h);

typedef struct ns_svg_doc ns_svg_doc;

ns_svg_doc *ns_svg_doc_parse(const guchar *data, gsize len);
gboolean    ns_svg_doc_render(cairo_t *cr, ns_svg_doc *doc,
                              double width, double height);
void        ns_svg_doc_free(ns_svg_doc *doc);

gboolean ns_svg_render_bytes(cairo_t *cr, const guchar *data, gsize len,
                             double width, double height);

gboolean ns_svg_bytes_look_like_svg(const guchar *data, gsize len);

G_END_DECLS

#endif
