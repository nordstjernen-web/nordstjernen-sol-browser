/* Nordstjernen — process-global JS class-ID allocation.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_JS_CLASSID_H
#define NS_JS_CLASSID_H

#include "ns_quickjs.h"

JSClassID ns_new_class_id(JSClassID *pclass_id);
void ns_js_pin_realm(JSContext *ctx);

#endif
