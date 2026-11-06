/* Nordstjernen — SVG documents fetched for external <use> references.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_SVG_EXTERNAL_H
#define NS_SVG_EXTERNAL_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

void           ns_svg_external_note_base(const ns_node *doc,
                                         const char *base_url,
                                         const char *document_url);
const ns_node *ns_svg_external_target(const ns_node *referrer,
                                      const char *href);
guint          ns_svg_external_pending(void);
guint64        ns_svg_external_generation(void);

G_END_DECLS

#endif
