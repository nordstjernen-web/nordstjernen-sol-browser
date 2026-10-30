/* Nordstjernen — the shell's icons through the in-engine SVG renderer, built only for GTK before 4.22.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "icons.h"
#include "dom.h"
#include "html.h"
#include "svg.h"

#define NS_ICON_RESOURCE_DIR \
    "/org/nordstjernen/WebBrowser/icons/scalable/apps/"
#define NS_ICON_DEFAULT_PX 16

#define NS_TYPE_ICON (ns_icon_get_type())
G_DECLARE_FINAL_TYPE(NsIcon, ns_icon, NS, ICON, GObject)

struct _NsIcon {
    GObject        parent_instance;
    ns_node       *doc;
    const ns_node *root;
    int            width;
    int            height;
    gboolean       symbolic;
};

static void ns_icon_paintable_init(GdkPaintableInterface *iface);
static void ns_icon_symbolic_paintable_init(GtkSymbolicPaintableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(NsIcon, ns_icon, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GDK_TYPE_PAINTABLE,
                                              ns_icon_paintable_init)
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_SYMBOLIC_PAINTABLE,
                                              ns_icon_symbolic_paintable_init))

static GHashTable *g_icons;

static void
ns_icon_draw(NsIcon *self, GtkSnapshot *snapshot, double width, double height,
             const GdkRGBA *color)
{
    if (width <= 0 || height <= 0)
        return;
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, (float)width,
                                                (float)height);
    cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &bounds);
    if (color) {
        cairo_push_group(cr);
        ns_svg_render_node(cr, self->root, width, height, NULL, NULL);
        cairo_pattern_t *shape = cairo_pop_group(cr);
        cairo_set_source_rgba(cr, color->red, color->green, color->blue,
                              color->alpha);
        cairo_mask(cr, shape);
        cairo_pattern_destroy(shape);
    } else {
        ns_svg_render_node(cr, self->root, width, height, NULL, NULL);
    }
    cairo_destroy(cr);
}

static void
ns_icon_snapshot(GdkPaintable *paintable, GdkSnapshot *snapshot,
                 double width, double height)
{
    ns_icon_draw(NS_ICON(paintable), GTK_SNAPSHOT(snapshot), width, height,
                 NULL);
}

static void
ns_icon_snapshot_symbolic(GtkSymbolicPaintable *paintable,
                          GdkSnapshot *snapshot, double width, double height,
                          const GdkRGBA *colors, gsize n_colors)
{
    NsIcon *self = NS_ICON(paintable);
    const GdkRGBA *foreground = self->symbolic && n_colors > 0 ? &colors[0]
                                                               : NULL;
    ns_icon_draw(self, GTK_SNAPSHOT(snapshot), width, height, foreground);
}

static GdkPaintableFlags
ns_icon_get_flags(GdkPaintable *paintable)
{
    (void)paintable;
    return GDK_PAINTABLE_STATIC_SIZE | GDK_PAINTABLE_STATIC_CONTENTS;
}

static int
ns_icon_get_intrinsic_width(GdkPaintable *paintable)
{
    return NS_ICON(paintable)->width;
}

static int
ns_icon_get_intrinsic_height(GdkPaintable *paintable)
{
    return NS_ICON(paintable)->height;
}

static void
ns_icon_paintable_init(GdkPaintableInterface *iface)
{
    iface->snapshot = ns_icon_snapshot;
    iface->get_flags = ns_icon_get_flags;
    iface->get_intrinsic_width = ns_icon_get_intrinsic_width;
    iface->get_intrinsic_height = ns_icon_get_intrinsic_height;
}

static void
ns_icon_symbolic_paintable_init(GtkSymbolicPaintableInterface *iface)
{
    iface->snapshot_symbolic = ns_icon_snapshot_symbolic;
}

static void
ns_icon_finalize(GObject *object)
{
    NsIcon *self = NS_ICON(object);
    if (self->doc)
        ns_node_free(self->doc);
    G_OBJECT_CLASS(ns_icon_parent_class)->finalize(object);
}

static void
ns_icon_class_init(NsIconClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ns_icon_finalize;
}

static void
ns_icon_init(NsIcon *self)
{
    self->width = NS_ICON_DEFAULT_PX;
    self->height = NS_ICON_DEFAULT_PX;
}

static int
intrinsic_px(gboolean has, double value)
{
    return has && value >= 1 && value <= 1024 ? (int)value
                                               : NS_ICON_DEFAULT_PX;
}

static NsIcon *
ns_icon_new_from_resource(const char *name)
{
    char *path = g_strconcat(NS_ICON_RESOURCE_DIR, name, ".svg", NULL);
    GBytes *bytes = g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE,
                                            NULL);
    g_free(path);
    if (!bytes)
        return NULL;
    gsize len = 0;
    const char *data = g_bytes_get_data(bytes, &len);
    ns_node *doc = ns_html_parse(data, (gssize)len);
    g_bytes_unref(bytes);
    const ns_node *root = doc ? ns_svg_document_root(doc) : NULL;
    if (!root) {
        if (doc)
            ns_node_free(doc);
        return NULL;
    }
    NsIcon *icon = g_object_new(NS_TYPE_ICON, NULL);
    icon->doc = doc;
    icon->root = root;
    icon->symbolic = g_str_has_suffix(name, "-symbolic");
    ns_svg_size size;
    ns_svg_intrinsic_size(root, &size);
    icon->width = intrinsic_px(size.has_width, size.width);
    icon->height = intrinsic_px(size.has_height, size.height);
    return icon;
}

GdkPaintable *
ns_icon_lookup(const char *name)
{
    if (!name || !*name)
        return NULL;
    if (!g_icons)
        g_icons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        g_object_unref);
    NsIcon *icon = g_hash_table_lookup(g_icons, name);
    if (!icon) {
        icon = ns_icon_new_from_resource(name);
        if (!icon)
            return NULL;
        g_hash_table_insert(g_icons, g_strdup(name), icon);
    }
    return GDK_PAINTABLE(icon);
}

static GtkWidget *
centered_image(GdkPaintable *paintable)
{
    GtkWidget *image = gtk_image_new_from_paintable(paintable);
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    return image;
}

GtkWidget *
ns_icon_image_new(const char *name)
{
    GdkPaintable *paintable = ns_icon_lookup(name);
    return paintable ? centered_image(paintable)
                     : gtk_image_new_from_icon_name(name);
}

void
ns_icon_image_set(GtkImage *image, const char *name)
{
    GdkPaintable *paintable = ns_icon_lookup(name);
    if (paintable)
        gtk_image_set_from_paintable(image, paintable);
    else
        gtk_image_set_from_icon_name(image, name);
}

GtkWidget *
ns_icon_button_new(const char *name)
{
    GtkWidget *button = gtk_button_new();
    ns_icon_button_set(GTK_BUTTON(button), name);
    return button;
}

void
ns_icon_button_set(GtkButton *button, const char *name)
{
    GdkPaintable *paintable = ns_icon_lookup(name);
    if (!paintable) {
        gtk_button_set_icon_name(button, name);
        return;
    }
    GtkWidget *child = gtk_button_get_child(button);
    if (GTK_IS_IMAGE(child))
        gtk_image_set_from_paintable(GTK_IMAGE(child), paintable);
    else
        gtk_button_set_child(button, centered_image(paintable));
    gtk_widget_add_css_class(GTK_WIDGET(button), "image-button");
}

void
ns_icon_menu_button_set(GtkMenuButton *button, const char *name)
{
    GdkPaintable *paintable = ns_icon_lookup(name);
    if (paintable)
        gtk_menu_button_set_child(button, centered_image(paintable));
    else
        gtk_menu_button_set_icon_name(button, name);
}

void
ns_icon_entry_set(GtkEntry *entry, GtkEntryIconPosition pos, const char *name)
{
    GdkPaintable *paintable = ns_icon_lookup(name);
    if (paintable)
        gtk_entry_set_icon_from_paintable(entry, pos, paintable);
    else
        gtk_entry_set_icon_from_icon_name(entry, pos, name);
}

static gboolean
pixbuf_loads_svg(void)
{
    gboolean found = FALSE;
    GSList *formats = gdk_pixbuf_get_formats();
    for (GSList *l = formats; l && !found; l = l->next) {
        char *format = gdk_pixbuf_format_get_name(l->data);
        found = g_strcmp0(format, "svg") == 0;
        g_free(format);
    }
    g_slist_free(formats);
    return found;
}

void
ns_icon_install_window_icon(const char *name)
{
    if (pixbuf_loads_svg())
        gtk_window_set_default_icon_name(name);
}
