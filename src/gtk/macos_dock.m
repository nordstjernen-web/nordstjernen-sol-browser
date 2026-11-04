/* Nordstjernen — set the macOS Dock icon when running unbundled.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */
#import <AppKit/AppKit.h>

#include <gtk/gtk.h>

#include "macos_dock.h"

#if !GTK_CHECK_VERSION(4, 22, 0)
#include "svg.h"
#endif

#define NS_DOCK_ICON_DIM 512

#if GTK_CHECK_VERSION(4, 22, 0)
static GBytes *
dock_icon_png(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return NULL;
    GtkIconPaintable *icon = gtk_icon_theme_lookup_icon(
        gtk_icon_theme_get_for_display(display), "nordstjernen", NULL,
        NS_DOCK_ICON_DIM, 1, GTK_TEXT_DIR_NONE, 0);
    if (!icon)
        return NULL;
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(GDK_PAINTABLE(icon), GDK_SNAPSHOT(snapshot),
                           NS_DOCK_ICON_DIM, NS_DOCK_ICON_DIM);
    g_object_unref(icon);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    if (!node)
        return NULL;
    GBytes *png = NULL;
    GskRenderer *renderer = gsk_cairo_renderer_new();
    if (gsk_renderer_realize(renderer, NULL, NULL)) {
        graphene_rect_t viewport =
            GRAPHENE_RECT_INIT(0, 0, NS_DOCK_ICON_DIM, NS_DOCK_ICON_DIM);
        GdkTexture *texture = gsk_renderer_render_texture(renderer, node,
                                                          &viewport);
        if (texture) {
            png = gdk_texture_save_to_png_bytes(texture);
            g_object_unref(texture);
        }
        gsk_renderer_unrealize(renderer);
    }
    g_object_unref(renderer);
    gsk_render_node_unref(node);
    return png;
}
#else
static cairo_status_t
dock_png_append(void *closure, const unsigned char *data, unsigned int length)
{
    g_byte_array_append(closure, data, length);
    return CAIRO_STATUS_SUCCESS;
}

static GBytes *
dock_icon_png(void)
{
    GBytes *svg = g_resources_lookup_data(
        "/org/nordstjernen/WebBrowser/icons/scalable/apps/nordstjernen.svg",
        G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    if (!svg)
        return NULL;
    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, NS_DOCK_ICON_DIM, NS_DOCK_ICON_DIM);
    GBytes *png = NULL;
    if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
        gsize len = 0;
        const guint8 *data = g_bytes_get_data(svg, &len);
        cairo_t *cr = cairo_create(surf);
        gboolean rendered = ns_svg_render_bytes(cr, data, len,
                                                NS_DOCK_ICON_DIM,
                                                NS_DOCK_ICON_DIM);
        cairo_destroy(cr);
        if (rendered) {
            cairo_surface_flush(surf);
            GByteArray *out = g_byte_array_new();
            if (cairo_surface_write_to_png_stream(surf, dock_png_append, out)
                == CAIRO_STATUS_SUCCESS)
                png = g_byte_array_free_to_bytes(out);
            else
                g_byte_array_unref(out);
        }
    }
    cairo_surface_destroy(surf);
    g_bytes_unref(svg);
    return png;
}
#endif

void
ns_macos_set_dock_icon(void)
{
    GBytes *png = dock_icon_png();
    if (!png)
        return;
    @autoreleasepool {
        gsize len = 0;
        const void *data = g_bytes_get_data(png, &len);
        NSImage *image = [[NSImage alloc]
            initWithData:[NSData dataWithBytes:data length:len]];
        if (image)
            [NSApplication sharedApplication].applicationIconImage = image;
    }
    g_bytes_unref(png);
}
