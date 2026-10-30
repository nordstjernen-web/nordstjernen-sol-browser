/* Nordstjernen — the shell's own icons, drawn by icons.c where GTK predates 4.22.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NORDSTJERNEN_GTK_ICONS_H
#define NORDSTJERNEN_GTK_ICONS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#if GTK_CHECK_VERSION(4, 22, 0)

static inline GtkWidget *
ns_icon_image_new(const char *name)
{
    return gtk_image_new_from_icon_name(name);
}

static inline void
ns_icon_image_set(GtkImage *image, const char *name)
{
    gtk_image_set_from_icon_name(image, name);
}

static inline GtkWidget *
ns_icon_button_new(const char *name)
{
    return gtk_button_new_from_icon_name(name);
}

static inline void
ns_icon_button_set(GtkButton *button, const char *name)
{
    gtk_button_set_icon_name(button, name);
}

static inline void
ns_icon_menu_button_set(GtkMenuButton *button, const char *name)
{
    gtk_menu_button_set_icon_name(button, name);
}

static inline void
ns_icon_entry_set(GtkEntry *entry, GtkEntryIconPosition pos, const char *name)
{
    gtk_entry_set_icon_from_icon_name(entry, pos, name);
}

static inline void
ns_icon_install_window_icon(const char *name)
{
    gtk_window_set_default_icon_name(name);
}

#else

GdkPaintable *ns_icon_lookup(const char *name);

GtkWidget *ns_icon_image_new(const char *name);

void ns_icon_image_set(GtkImage *image, const char *name);

GtkWidget *ns_icon_button_new(const char *name);

void ns_icon_button_set(GtkButton *button, const char *name);

void ns_icon_menu_button_set(GtkMenuButton *button, const char *name);

void ns_icon_entry_set(GtkEntry *entry, GtkEntryIconPosition pos,
                       const char *name);

void ns_icon_install_window_icon(const char *name);

#endif

G_END_DECLS

#endif
