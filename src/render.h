/* Nordstjernen — shared style/layout pipeline used by GUI and headless.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_RENDER_H
#define NS_RENDER_H

#include <glib.h>

#include "anim.h"
#include "css.h"
#include "dom.h"
#include "js.h"
#include "layout.h"

G_BEGIN_DECLS

struct ns_image_cache;

typedef struct ns_render_ctx {
    ns_node                        *doc;
    const ns_css_stylesheet *const *sheets;
    const ns_node *const           *sheet_docs;
    guint                           n_sheets;
    double                          viewport_width;
    double                          viewport_height;
    double                          zoom;
    struct ns_image_cache          *images;
    const char                     *base_url;
    const char                     *document_url;
    ns_anim                        *anim;
    ns_js                          *js;
    const ns_node                  *focused_input;
    const ns_node                  *hover_node;
    gsize                           caret_byte;
    gsize                           sel_anchor_byte;
    char     *(*resolve_url)(const char *href, gpointer ud);
    gboolean  (*font_allowed)(const char *abs_url, gpointer ud);
    gpointer                        cb_ud;
} ns_render_ctx;

typedef struct ns_render_profile {
    gint64 css1_us;
    gint64 style1_us;
    gint64 layout1_us;
    gint64 container_us;
    gint64 css2_us;
    gint64 style2_us;
    gint64 layout2_us;
    guint  containers;
    guint  container_passes;
    gboolean container_pass;
} ns_render_profile;

gboolean ns_render_page_uses_hover(void);
gboolean ns_render_page_uses_active(void);

const ns_css_page_rule *ns_render_page_rule(void);

GHashTable *ns_render_relayout(const ns_render_ctx *c, ns_box **out_layout);
GHashTable *ns_render_restyle(const ns_render_ctx *c);
GHashTable *ns_render_relayout_profile(const ns_render_ctx *c,
                                       ns_box **out_layout,
                                       ns_render_profile *profile);

G_END_DECLS

#endif
