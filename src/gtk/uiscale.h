/* Nordstjernen — UI scale for displays GTK does not scale itself (fractional scales on Windows).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NORDSTJERNEN_GTK_UISCALE_H
#define NORDSTJERNEN_GTK_UISCALE_H

#include <glib.h>

double ns_ui_scale(void);
int    ns_ui_px(int px);
char  *ns_ui_scale_css(const char *css);

#endif
