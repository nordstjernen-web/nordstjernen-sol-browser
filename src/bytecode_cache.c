/* Nordstjernen — per-process JavaScript bytecode cache.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "bytecode_cache.h"

#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#define NS_BYTECODE_CACHE_RAM_DIVISOR 64u
#define NS_BYTECODE_CACHE_MIN_BYTES   (32ull * 1024ull * 1024ull)
#define NS_BYTECODE_CACHE_MAX_BYTES   (512ull * 1024ull * 1024ull)

typedef struct ns_bytecode_cache_entry {
    guint8 *bytes;
    gsize   len;
    gint64  used_us;
} ns_bytecode_cache_entry;

static GHashTable *g_mem;
static GMutex      g_lock;
static guint64     g_mem_bytes;
static guint64     g_budget_bytes;

static guint64
physical_ram_bytes(void)
{
#if defined(G_OS_WIN32)
    MEMORYSTATUSEX m = { .dwLength = sizeof m };
    if (GlobalMemoryStatusEx(&m)) return (guint64)m.ullTotalPhys;
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof mem;
    if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) == 0) return mem;
#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0) return (guint64)pages * (guint64)psize;
#endif
    return 0;
}

static guint64
budget_for_ram(guint64 ram)
{
    guint64 budget = ram / NS_BYTECODE_CACHE_RAM_DIVISOR;
    return CLAMP(budget, NS_BYTECODE_CACHE_MIN_BYTES,
                 NS_BYTECODE_CACHE_MAX_BYTES);
}

static void
ns_bytecode_cache_entry_free(gpointer data)
{
    ns_bytecode_cache_entry *e = data;
    if (!e) return;
    g_free(e->bytes);
    g_free(e);
}

void
ns_bytecode_cache_init(void)
{
    guint64 budget = budget_for_ram(physical_ram_bytes());
    g_mutex_lock(&g_lock);
    if (!g_mem)
        g_mem = g_hash_table_new_full(g_str_hash, g_str_equal,
                                      g_free, ns_bytecode_cache_entry_free);
    g_budget_bytes = budget;
    g_mutex_unlock(&g_lock);
}

void
ns_bytecode_cache_shutdown(void)
{
    g_mutex_lock(&g_lock);
    if (g_mem) {
        g_hash_table_destroy(g_mem);
        g_mem = NULL;
    }
    g_mem_bytes = 0;
    g_mutex_unlock(&g_lock);
}

static void
hash_key(const char *key, gsize len, char out[65])
{
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(c, (const guchar *)(key ? key : ""), (gssize)len);
    g_strlcpy(out, g_checksum_get_string(c), 65);
    g_checksum_free(c);
}

static void
evict_until_fits(gsize want_bytes)
{
    if (!g_mem) return;
    while (g_mem_bytes + want_bytes > g_budget_bytes) {
        gint64   oldest_us = G_MAXINT64;
        gpointer oldest_key = NULL;
        ns_bytecode_cache_entry *oldest_entry = NULL;
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, g_mem);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            ns_bytecode_cache_entry *e = v;
            if (e->used_us < oldest_us) {
                oldest_us = e->used_us;
                oldest_key = k;
                oldest_entry = e;
            }
        }
        if (!oldest_entry) break;
        g_mem_bytes -= oldest_entry->len;
        g_hash_table_remove(g_mem, oldest_key);
    }
}

guint8 *
ns_bytecode_cache_get(const char *key, gsize key_len, gsize *out_len)
{
    if (!key || !key_len) return NULL;
    char digest[65];
    hash_key(key, key_len, digest);

    g_mutex_lock(&g_lock);
    ns_bytecode_cache_entry *e = g_mem ? g_hash_table_lookup(g_mem, digest)
                                       : NULL;
    guint8 *copy = NULL;
    if (e) {
        e->used_us = g_get_monotonic_time();
        copy = g_memdup2(e->bytes, e->len);
        if (out_len) *out_len = e->len;
    }
    g_mutex_unlock(&g_lock);
    return copy;
}

void
ns_bytecode_cache_put(const char *key, gsize key_len,
                      const guint8 *bc, gsize bc_len)
{
    if (!key || !key_len || !bc || !bc_len) return;
    char digest[65];
    hash_key(key, key_len, digest);

    g_mutex_lock(&g_lock);
    if (!g_mem || bc_len > g_budget_bytes / 2) {
        g_mutex_unlock(&g_lock);
        return;
    }
    ns_bytecode_cache_entry *existing = g_hash_table_lookup(g_mem, digest);
    if (existing) {
        existing->used_us = g_get_monotonic_time();
        g_mutex_unlock(&g_lock);
        return;
    }
    evict_until_fits(bc_len);
    ns_bytecode_cache_entry *ne = g_new0(ns_bytecode_cache_entry, 1);
    ne->bytes = g_memdup2(bc, bc_len);
    ne->len = bc_len;
    ne->used_us = g_get_monotonic_time();
    g_hash_table_insert(g_mem, g_strdup(digest), ne);
    g_mem_bytes += bc_len;
    g_mutex_unlock(&g_lock);
}
