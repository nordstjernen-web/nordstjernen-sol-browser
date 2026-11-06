/* Nordstjernen — paginates a laid-out page onto sheets of paper.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_PRINT_H
#define NS_PRINT_H

#include <cairo.h>
#include <glib.h>

#include "css.h"
#include "layout.h"
#include "print_setup.h"

G_BEGIN_DECLS

struct ns_browser;
struct ns_renderer_session;
struct ns_rproc_http;

typedef struct ns_print_link { double x, y, w, h; char *uri; } ns_print_link;
typedef struct ns_print_heading { int level; double y; char *text; } ns_print_heading;

void ns_print_setup_apply_page_rule(ns_print_setup *setup,
                                    const ns_css_page_rule *rule);

GArray *ns_print_page_offsets(const ns_box *root, double page_content_height);

double ns_print_page_bottom(const GArray *offsets, guint i,
                            double page_content_height);

void ns_print_draw_page(cairo_t *cr, const ns_box *root,
                        const ns_print_setup *setup, double scale,
                        double page_top, double page_bottom);

void ns_print_draw_page_scaled(cairo_t *cr, const ns_box *root,
                               const ns_print_setup *setup, double unit,
                               double content_scale, double page_top,
                               double page_bottom);

void ns_print_draw_margin_text(cairo_t *cr, const ns_print_setup *setup,
                               double unit, const char *text, int slot,
                               gboolean footer);

GArray *ns_print_links(const ns_box *root);
void ns_print_links_free(GArray *links);
GArray *ns_print_headings(const ns_box *root);
void ns_print_headings_free(GArray *headings);

GPtrArray *ns_browser_print_pages(struct ns_browser *browser,
                                  ns_print_setup *out_setup);

GPtrArray *ns_renderer_session_print(struct ns_renderer_session *s,
                                     ns_print_setup *out_setup);


G_END_DECLS

#endif
