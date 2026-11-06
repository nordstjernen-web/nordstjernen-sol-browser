/* Nordstjernen — @font-face web font loader.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_FONT_H
#define NS_FONT_H

#include <glib.h>

G_BEGIN_DECLS

typedef void (*ns_font_loaded_cb)(const char *family, gpointer user_data);

void     ns_font_init(void);
void     ns_font_shutdown(void);

gboolean ns_font_available(void);


gboolean ns_font_family_loaded(const char *family);
char    *ns_font_metric_alias(const char *family);
gboolean ns_font_family_is_emoji(const char *family);
char    *ns_font_family_for_text(const char *family);
guint    ns_font_generation(void);

typedef enum ns_font_slant {
    NS_FONT_SLANT_AUTO,
    NS_FONT_SLANT_ROMAN,
    NS_FONT_SLANT_ITALIC,
    NS_FONT_SLANT_OBLIQUE,
} ns_font_slant;

typedef struct ns_font_descriptors {
    int           weight;
    ns_font_slant slant;
} ns_font_descriptors;

void     ns_font_request(const char *family, const char *src_url,
                         const char *base_url, const char *document_url,
                         ns_font_descriptors descriptors);

gboolean ns_font_install_bytes(const char *family, const guint8 *data,
                               gsize len, ns_font_descriptors descriptors);

guint    ns_font_pending_count(void);

typedef void (*ns_font_idle_cb)(gpointer user_data);
void     ns_font_add_idle_cb(ns_font_idle_cb cb, gpointer user_data);
void     ns_font_remove_idle_cb(ns_font_idle_cb cb, gpointer user_data);

G_END_DECLS

#endif
