/* Nordstjernen — WebAssembly JS API implemented over the vendored WAMR interpreter.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_WASM_H
#define NS_WASM_H

#include "ns_quickjs.h"

void ns_wasm_install(JSContext *ctx, JSValueConst global);
bool ns_wasm_compile_allowed(JSContext *ctx);
JSValue ns_wasm_wire_handle(JSContext *ctx, JSValueConst v, const char **kind);
JSValue ns_wasm_wire_restore(JSContext *ctx, const char *kind,
                             JSValueConst sab);

#endif
