/* Nordstjernen — PDF documents rendered to an inline HTML page.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_PDF_H
#define NS_PDF_H

#include <glib.h>

G_BEGIN_DECLS

char *ns_pdf_document_html(const guint8 *data, gsize len, const char *url);
const char *ns_pdf_document_csp(void);

G_END_DECLS

#endif
