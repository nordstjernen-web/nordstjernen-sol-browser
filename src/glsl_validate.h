/* Nordstjernen — WebGL shader validation ahead of the GPU driver.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_GLSL_VALIDATE_H
#define NS_GLSL_VALIDATE_H

#include <glib.h>

G_BEGIN_DECLS

/* The source with page functions that some GLSL compilers mistake for
 * built-ins renamed, or NULL when there are none; the caller frees it. */
char *ns_glsl_unshadow(const char *src, size_t len);

char *ns_glsl_validate(const char *src, size_t len, int webgl_version,
                       gboolean fragment);

G_END_DECLS

#endif
