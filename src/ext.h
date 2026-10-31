/* Nordstjernen — WebExtensions loader and content-script host.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_EXT_H
#define NS_EXT_H

#include <glib.h>
#include "ns_quickjs.h"

G_BEGIN_DECLS

char    *ns_ext_content_scripts_for_url(JSContext *ctx, const char *url,
                                        gboolean at_start, JSValue *bridges);
gboolean ns_ext_should_block(const char *url, const char *initiator);
char    *ns_ext_default_root(void);

G_END_DECLS

#endif
