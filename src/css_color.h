/* Nordstjernen — CSS Color 4/5 value parsing and serialization.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_CSS_COLOR_H
#define NS_CSS_COLOR_H

#include <glib.h>

gboolean ns_css_color_resolve(const char *s, const guint8 *current, guint8 rgba[4]);
char    *ns_css_color_specified(const char *s);
char    *ns_css_color_computed(const char *s, const guint8 *current);
gboolean ns_css_color_is_legacy_srgb(const char *s);
char    *ns_css_color_input_value(const char *s, gboolean alpha, gboolean display_p3);

#endif
