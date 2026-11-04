/* Nordstjernen — GTK tabbed browser window over the out-of-process renderer.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NORDSTJERNEN_GTK_PROCWINDOW_H
#define NORDSTJERNEN_GTK_PROCWINDOW_H

#include <gtk/gtk.h>

#include "procview.h"

G_BEGIN_DECLS

int ns_procapp_run(const char *startup_url, gboolean explicit_url,
                   const char *instance_id, gboolean recover,
                   gboolean private_mode);

void ns_procapp_set_window_size(int width, int height);

typedef struct {
    int         id;
    int         window;
    gboolean    active;
    NsProcView *view;
} NsProcTab;

GArray     *ns_procapp_tabs(void);
NsProcView *ns_procapp_tab_view(int id);
int         ns_procapp_tab_id(NsProcView *view);
int         ns_procapp_tab_new(const char *url);
gboolean    ns_procapp_tab_close(int id);
gboolean    ns_procapp_tab_select(int id, gboolean present);
gboolean    ns_procapp_tab_load(int id, const char *url);
void        ns_procapp_set_remote_status(const char *controller);

G_END_DECLS

#endif
