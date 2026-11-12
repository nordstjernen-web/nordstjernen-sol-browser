/* Nordstjernen — UI scale for displays GTK does not scale itself.
 * GTK on Windows scales widgets by whole factors only: at 125%, 150% or 175%
 * it scales text (gtk-xft-dpi) and leaves every pixel length at 100%.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "uiscale.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#define NS_UI_SCALE_MAX 4.0

static double g_ui_scale;

static double
detect_ui_scale(void)
{
    const char *env = g_getenv("NS_UI_SCALE");
    if (env && *env) {
        double forced = g_ascii_strtod(env, NULL);
        if (forced >= 1.0 && forced <= NS_UI_SCALE_MAX)
            return forced;
    }
#ifdef G_OS_WIN32
    GtkSettings *settings = gtk_settings_get_default();
    int xft_dpi = 0;
    if (settings)
        g_object_get(settings, "gtk-xft-dpi", &xft_dpi, NULL);
    double scale = xft_dpi / 1024.0 / 96.0;
    if (scale > 1.01 && scale <= NS_UI_SCALE_MAX)
        return scale;
#endif
    return 1.0;
}

double
ns_ui_scale(void)
{
    if (g_ui_scale <= 0.0)
        g_ui_scale = detect_ui_scale();
    return g_ui_scale;
}

int
ns_ui_px(int px)
{
    if (px >= -1 && px <= 1)
        return px;
    return (int)(px * ns_ui_scale() + (px < 0 ? -0.5 : 0.5));
}

char *
ns_ui_scale_css(const char *css)
{
    if (ns_ui_scale() == 1.0)
        return g_strdup(css);
    GString *out = g_string_sized_new(strlen(css) + 64);
    const char *p = css;
    while (*p) {
        gboolean boundary = p == css ||
            !(g_ascii_isalnum(p[-1]) || p[-1] == '.' || p[-1] == '#' ||
              p[-1] == '-' || p[-1] == '_');
        if (boundary && g_ascii_isdigit(*p)) {
            const char *end = p;
            while (g_ascii_isdigit(*end))
                end++;
            if (end[0] == 'p' && end[1] == 'x' && !g_ascii_isalnum(end[2])) {
                g_string_append_printf(out, "%dpx", ns_ui_px(atoi(p)));
                p = end + 2;
                continue;
            }
            g_string_append_len(out, p, end - p);
            p = end;
            continue;
        }
        g_string_append_c(out, *p++);
    }
    return g_string_free(out, FALSE);
}
