/* Nordstjernen — @counter-style registry and counter representations. */

#ifndef NS_CSS_COUNTER_STYLE_H
#define NS_CSS_COUNTER_STYLE_H

#include <glib.h>

G_BEGIN_DECLS

void ns_css_counter_style_register(const char *name, const char *body,
                                   gsize body_len);
char *ns_css_counter_style_text(const char *name, int value);
char *ns_css_counter_style_marker(const char *name, int value);

G_END_DECLS

#endif
