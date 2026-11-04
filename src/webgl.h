/* Nordstjernen — minimalist WebGL: canvas.getContext mapped onto GLES.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_WEBGL_H
#define NS_WEBGL_H

#include <glib.h>
#include <cairo.h>
#include "ns_quickjs.h"

#include "dom.h"

G_BEGIN_DECLS

typedef struct ns_js ns_js;

JSValue ns_webgl_get_context(JSContext *ctx, ns_js *js, JSValueConst canvas_obj,
                             const ns_node *canvas, int version,
                             JSValueConst attrs);

void ns_webgl_install(JSContext *ctx, JSValueConst global);

cairo_surface_t *ns_webgl_canvas_surface(const ns_node *canvas);

char *ns_webgl_take_pending_origin(void);
void  ns_webgl_set_decision(const char *origin, int allow);
void  ns_webgl_set_state(const char *origin, int state);
void  ns_webgl_clear_decisions(void);

cairo_surface_t *ns_js_drawimage_source_surface(JSContext *ctx,
                                                JSValueConst src,
                                                int *out_w, int *out_h,
                                                gboolean *threw);

G_END_DECLS

#endif
