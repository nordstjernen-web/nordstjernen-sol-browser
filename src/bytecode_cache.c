/* Nordstjernen — per-process JavaScript bytecode cache.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "bytecode_cache.h"

#define NS_BYTECODE_CACHE_MEM_CAP_BYTES   (64u * 1024u * 1024u)
#define NS_BYTECODE_CACHE_VALUE_CAP_BYTES (20u * 1024u * 1024u)

typedef struct ns_bytecode_cache_entry {
    guint8 *bytes;
    gsize   len;
    gint64  used_us;
} ns_bytecode_cache_entry;

static GHashTable *g_mem;
static GMutex      g_lock;
static guint64     g_mem_bytes;

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
    g_mutex_lock(&g_lock);
    if (!g_mem)
        g_mem = g_hash_table_new_full(g_str_hash, g_str_equal,
                                      g_free, ns_bytecode_cache_entry_free);
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
    while (g_mem_bytes + want_bytes > NS_BYTECODE_CACHE_MEM_CAP_BYTES) {
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
    if (bc_len > NS_BYTECODE_CACHE_VALUE_CAP_BYTES) return;
    char digest[65];
    hash_key(key, key_len, digest);

    g_mutex_lock(&g_lock);
    if (!g_mem) { g_mutex_unlock(&g_lock); return; }
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
