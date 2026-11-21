/* Nordstjernen — IndexedDB backend bridge.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_IDB_H
#define NS_IDB_H

#include <glib.h>

#include "ns_quickjs.h"

void ns_idb_install(JSContext *ctx, JSValueConst global);
gint64 ns_idb_origin_usage_bytes(JSContext *ctx);
gint64 ns_idb_origin_quota_bytes(void);

#endif
