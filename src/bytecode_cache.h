/* Nordstjernen — JavaScript bytecode cache, held in memory per process.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_BYTECODE_CACHE_H
#define NS_BYTECODE_CACHE_H

#include <glib.h>

G_BEGIN_DECLS

void          ns_bytecode_cache_init(void);
void          ns_bytecode_cache_shutdown(void);

GBytes *ns_bytecode_cache_get(const char *key, gsize key_len);
void    ns_bytecode_cache_put(const char *key, gsize key_len,
                              const guint8 *bc, gsize bc_len);

G_END_DECLS

#endif
