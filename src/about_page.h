/* Nordstjernen: shared builders of the built-in about: pages.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */
#ifndef NS_ABOUT_PAGE_H
#define NS_ABOUT_PAGE_H

#include <glib.h>

G_BEGIN_DECLS

#define NS_TR(text)    "\x01" text "\x02"
#define NS_TR_JS(text) "\x03" text "\x04"

char *ns_about_read_first(const char *const *rel_paths, gsize *out_len);
char *ns_about_logo_markup(void);
char *ns_about_substitute(const char *template_text,
                          const char *placeholder, const char *value);
char *ns_about_localize(const char *template_text);
char *ns_about_fill(const char *template_text, const char *const *keys,
                    char **values, gsize n);
char *ns_about_format(const char *template_text, const char *value);
char *ns_about_start_html(gboolean private_window);

G_END_DECLS

#endif
