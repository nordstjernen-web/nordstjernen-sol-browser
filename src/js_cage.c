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

typedef union {
    gint   refs;
    guint8 pad[16];
} ns_sab_header;

typedef struct {
    void (*dup)(void *opaque);
    void (*release)(void *opaque);
    void *opaque;
} ns_sab_foreign;

static GMutex      g_sab_foreign_lock;
static GHashTable *g_sab_foreign;

static gboolean
ns_sab_foreign_lookup(void *ptr, ns_sab_foreign *out)
{
    g_mutex_lock(&g_sab_foreign_lock);
    ns_sab_foreign *f = g_sab_foreign ? g_hash_table_lookup(g_sab_foreign, ptr)
                                      : NULL;
    if (f) *out = *f;
    g_mutex_unlock(&g_sab_foreign_lock);
    return f != NULL;
}

void
ns_js_sab_register_foreign(void *data, void (*dup)(void *opaque),
                           void (*release)(void *opaque), void *opaque)
{
    ns_sab_foreign *f = g_new(ns_sab_foreign, 1);
    f->dup = dup;
    f->release = release;
    f->opaque = opaque;
    g_mutex_lock(&g_sab_foreign_lock);
    if (!g_sab_foreign)
        g_sab_foreign = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, g_free);
    g_hash_table_replace(g_sab_foreign, data, f);
    g_mutex_unlock(&g_sab_foreign_lock);
}

void
ns_js_sab_unregister_foreign(void *data)
{
    g_mutex_lock(&g_sab_foreign_lock);
    if (g_sab_foreign) g_hash_table_remove(g_sab_foreign, data);
    g_mutex_unlock(&g_sab_foreign_lock);
}

static void *
ns_sab_alloc(void *opaque, size_t size)
{
    (void)opaque;
    if (size > G_MAXSIZE - sizeof(ns_sab_header)) return NULL;
    ns_sab_header *block = JS_CageMalloc(size + sizeof(ns_sab_header));
    if (!block) return NULL;
    g_atomic_int_set(&block->refs, 1);
    return block + 1;
}

void
ns_js_sab_dup(void *ptr)
{
    if (!ptr) return;
    ns_sab_foreign f;
    if (ns_sab_foreign_lookup(ptr, &f)) {
        f.dup(f.opaque);
        return;
    }
    g_atomic_int_inc(&((ns_sab_header *)ptr - 1)->refs);
}

void
ns_js_sab_free(void *ptr)
{
    if (!ptr) return;
    ns_sab_foreign f;
    if (ns_sab_foreign_lookup(ptr, &f)) {
        f.release(f.opaque);
        return;
    }
    ns_sab_header *block = (ns_sab_header *)ptr - 1;
    if (g_atomic_int_dec_and_test(&block->refs))
        JS_CageFree(block);
}

static void
ns_sab_dup_cb(void *opaque, void *ptr)
{
    (void)opaque;
    ns_js_sab_dup(ptr);
}

static void
ns_sab_free_cb(void *opaque, void *ptr)
{
    (void)opaque;
    ns_js_sab_free(ptr);
}

JSRuntime *
ns_js_runtime_new(void)
{
    JSRuntime *rt = JS_NewRuntime();
    ns_js_cage_log_once();
    if (!rt) {
        g_warning("js heap cage: could not create a QuickJS runtime");
        return NULL;
    }
    static const JSSharedArrayBufferFunctions sab_funcs = {
        .sab_alloc = ns_sab_alloc,
        .sab_free = ns_sab_free_cb,
        .sab_dup = ns_sab_dup_cb,
        .sab_opaque = NULL,
    };
    JS_SetSharedArrayBufferFunctions(rt, &sab_funcs);
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
