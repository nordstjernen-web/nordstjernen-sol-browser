/* Nordstjernen — W3C WebDriver server that drives sandboxed renderers over the engine protocol. */

#ifndef NORDSTJERNEN_GTK_WEBDRIVER_H
#define NORDSTJERNEN_GTK_WEBDRIVER_H

#include <glib.h>

G_BEGIN_DECLS

#define NS_WEBDRIVER_DEFAULT_PORT 4444

typedef struct ns_webdriver_opts {
    int      port;
    int      viewport_width;
    int      viewport_height;
    int      settle_ms;
    gboolean log;
} ns_webdriver_opts;

int ns_webdriver_run(const ns_webdriver_opts *opts);

G_END_DECLS

#endif
