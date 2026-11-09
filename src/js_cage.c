/* Nordstjernen — the browser's view of the QuickJS heap cage: runtime creation and the shared cage heap. */

#include "js_cage.h"

static void
ns_js_cage_log_once(void)
{
    static gsize logged;
    if (!g_once_init_enter(&logged)) return;
    if (JS_CageSize())
        g_info("js heap cage: %zu MiB reserved at %p",
               JS_CageSize() >> 20, (void *)JS_CageBase());
    else
        g_info("js heap cage: not available in this build; QuickJS uses the process heap");
    g_once_init_leave(&logged, 1);
}

JSRuntime *
ns_js_runtime_new(void)
{
    JSRuntime *rt = JS_NewRuntime();
    ns_js_cage_log_once();
    if (!rt)
        g_warning("js heap cage: could not create a QuickJS runtime");
    return rt;
}

void
ns_js_runtime_free(JSRuntime *rt)
{
    if (rt) JS_FreeRuntime(rt);
}

gboolean
ns_js_cage_enabled(void)
{
    return JS_CageSize() != 0;
}

gboolean
ns_js_cage_contains(const void *ptr)
{
    return JS_CageContains(ptr);
}

uintptr_t
ns_js_cage_base(void)
{
    return JS_CageBase();
}

size_t
ns_js_cage_size(void)
{
    return JS_CageSize();
}

size_t
ns_js_cage_committed_bytes(void)
{
    return JS_CageCommittedBytes();
}

void *
ns_js_cage_shared_malloc(size_t size)
{
    return JS_CageMalloc(size);
}

void *
ns_js_cage_shared_realloc(void *ptr, size_t size)
{
    return JS_CageRealloc(ptr, size);
}

void
ns_js_cage_shared_free(void *ptr)
{
    JS_CageFree(ptr);
}

size_t
ns_js_cage_shared_usable_size(const void *ptr)
{
    return JS_CageUsableSize(ptr);
}
