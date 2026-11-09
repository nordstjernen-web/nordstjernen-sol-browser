/* Nordstjernen — reserved address range (the heap cage) that holds every QuickJS allocation. */

#ifndef NS_JS_CAGE_H
#define NS_JS_CAGE_H

#include <glib.h>
#include <stdint.h>
#include "ns_quickjs.h"

G_BEGIN_DECLS

JSRuntime *ns_js_runtime_new(void);
void       ns_js_runtime_free(JSRuntime *rt);

gboolean   ns_js_cage_enabled(void);
gboolean   ns_js_cage_contains(const void *ptr);
uintptr_t  ns_js_cage_base(void);
size_t     ns_js_cage_size(void);
size_t     ns_js_cage_committed_bytes(void);

void      *ns_js_cage_shared_malloc(size_t size);
void      *ns_js_cage_shared_realloc(void *ptr, size_t size);
void       ns_js_cage_shared_free(void *ptr);
size_t     ns_js_cage_shared_usable_size(const void *ptr);

G_END_DECLS

#endif
