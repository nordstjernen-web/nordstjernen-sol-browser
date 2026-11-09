/* Nordstjernen — reserved address range (the heap cage) that holds every QuickJS allocation. */

#include "js_cage.h"

#include <stdbool.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define NS_CAGE_SUPPORTED 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NS_CAGE_SUPPORTED 0
#endif
#endif
#ifndef NS_CAGE_SUPPORTED
#if UINTPTR_MAX > 0xffffffffu
#define NS_CAGE_SUPPORTED 1
#else
#define NS_CAGE_SUPPORTED 0
#endif
#endif

static GMutex g_shared_lock;
static GHashTable *g_shared_sizes;
static GHashTable *g_heaps;

static void *
ns_shared_fallback_malloc(size_t size)
{
    void *p = g_malloc(size ? size : 1);
    if (!g_shared_sizes) g_shared_sizes = g_hash_table_new(NULL, NULL);
    g_hash_table_insert(g_shared_sizes, p, GSIZE_TO_POINTER(size));
    return p;
}

static size_t
ns_shared_fallback_usable_size(const void *ptr)
{
    if (!g_shared_sizes) return 0;
    return GPOINTER_TO_SIZE(g_hash_table_lookup(g_shared_sizes, ptr));
}

static void
ns_shared_fallback_free(void *ptr)
{
    if (g_shared_sizes) g_hash_table_remove(g_shared_sizes, ptr);
    g_free(ptr);
}

static void *
ns_shared_fallback_realloc(void *ptr, size_t size)
{
    if (ptr && g_shared_sizes) g_hash_table_remove(g_shared_sizes, ptr);
    void *p = g_realloc(ptr, size ? size : 1);
    if (!g_shared_sizes) g_shared_sizes = g_hash_table_new(NULL, NULL);
    g_hash_table_insert(g_shared_sizes, p, GSIZE_TO_POINTER(size));
    return p;
}

#if NS_CAGE_SUPPORTED

#define NS_CAGE_CHUNK_SHIFT 20
#define NS_CAGE_CHUNK_SIZE ((size_t)1 << NS_CAGE_CHUNK_SHIFT)
#define NS_CAGE_SLAB_SHIFT 16
#define NS_CAGE_SLAB_SIZE ((size_t)1 << NS_CAGE_SLAB_SHIFT)
#define NS_CAGE_SLABS_PER_CHUNK (NS_CAGE_CHUNK_SIZE / NS_CAGE_SLAB_SIZE)
#define NS_CAGE_SMALL_MAX 32768u
#define NS_CAGE_MIN_BLOCK 16u
#define NS_CAGE_BITMAP_WORDS (NS_CAGE_SLAB_SIZE / NS_CAGE_MIN_BLOCK / 64)
#define NS_CAGE_GUARD_SIZE ((size_t)32 << 30)
#define NS_CAGE_MAX_SIZE ((size_t)1 << 36)
#define NS_CAGE_MIN_SIZE ((size_t)1 << 30)
#define NS_CAGE_COMMITTED_CACHE 32
#define NS_CAGE_ALL_SLABS_FREE ((uint16_t)0xffff)

enum {
    NS_SLAB_FREE,
    NS_SLAB_SMALL,
    NS_SLAB_RUN_HEAD,
    NS_SLAB_RUN_TAIL,
};

static const uint32_t ns_cage_class_sizes[] = {
    16, 32, 48, 64, 80, 96, 112, 128,
    160, 192, 224, 256,
    320, 384, 448, 512,
    640, 768, 896, 1024,
    1280, 1536, 1792, 2048,
    2560, 3072, 3584, 4096,
    5120, 6144, 7168, 8192,
    10240, 12288, 14336, 16384,
    20480, 24576, 28672, 32768,
};
#define NS_CAGE_CLASS_COUNT G_N_ELEMENTS(ns_cage_class_sizes)

typedef struct ns_cage_chunk ns_cage_chunk;
typedef struct ns_cage_heap ns_cage_heap;

typedef struct ns_cage_slab {
    ns_cage_chunk *chunk;
    struct ns_cage_slab *prev_partial;
    struct ns_cage_slab *next_partial;
    uint8_t  kind;
    uint8_t  class_idx;
    uint8_t  index;
    uint16_t run_slabs;
    uint16_t n_blocks;
    uint16_t used;
    uint16_t hint;
    uint64_t bits[NS_CAGE_BITMAP_WORDS];
} ns_cage_slab;

struct ns_cage_chunk {
    ns_cage_heap *heap;
    ns_cage_chunk *prev;
    ns_cage_chunk *next;
    uint32_t index;
    uint32_t huge_chunks;
    uint16_t free_slabs;
    ns_cage_slab slabs[NS_CAGE_SLABS_PER_CHUNK];
};

struct ns_cage_heap {
    ns_cage_slab *partial[NS_CAGE_CLASS_COUNT];
    ns_cage_chunk *chunks;
};

static struct {
    uintptr_t base;
    size_t size;
    uint32_t n_chunks;
    uint32_t committed_free;
    uint32_t scan_hint;
    size_t committed_bytes;
    GMutex lock;
    ns_cage_chunk **meta;
    uint64_t *free_bits;
    uint64_t *committed_bits;
    bool ready;
} g_cage;

static inline void *ns_cage_chunk_addr(uint32_t index)
{
    return (void *)(g_cage.base + ((uintptr_t)index << NS_CAGE_CHUNK_SHIFT));
}

static inline uint32_t ns_cage_chunk_index(const void *ptr)
{
    return (uint32_t)(((uintptr_t)ptr - g_cage.base) >> NS_CAGE_CHUNK_SHIFT);
}

static inline bool ns_bit_test(const uint64_t *bits, uint32_t i)
{
    return (bits[i >> 6] >> (i & 63)) & 1;
}

static inline void ns_bit_set(uint64_t *bits, uint32_t i)
{
    bits[i >> 6] |= (uint64_t)1 << (i & 63);
}

static inline void ns_bit_clear(uint64_t *bits, uint32_t i)
{
    bits[i >> 6] &= ~((uint64_t)1 << (i & 63));
}

#ifdef G_OS_WIN32

static bool
ns_cage_os_reserve(size_t size, size_t guard)
{
    size_t span = size + 2 * guard;
    for (int attempt = 0; attempt < 8; attempt++) {
        char *probe = VirtualAlloc(NULL, span + size, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) return false;
        uintptr_t aligned = ((uintptr_t)probe + guard + size - 1) & ~(size - 1);
        VirtualFree(probe, 0, MEM_RELEASE);
        void *got = VirtualAlloc((void *)(aligned - guard), span, MEM_RESERVE,
                                 PAGE_NOACCESS);
        if (got) {
            g_cage.base = aligned;
            g_cage.size = size;
            return true;
        }
    }
    return false;
}

static bool
ns_cage_os_commit(void *ptr, size_t len)
{
    return VirtualAlloc(ptr, len, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static void
ns_cage_os_decommit(void *ptr, size_t len)
{
    VirtualFree(ptr, len, MEM_DECOMMIT);
}

#else

static bool
ns_cage_os_reserve(size_t size, size_t guard)
{
    size_t span = size + 2 * guard;
    size_t total = span + size;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    char *probe = mmap(NULL, total, PROT_NONE, flags, -1, 0);
    if (probe == MAP_FAILED) return false;
    uintptr_t aligned = ((uintptr_t)probe + guard + size - 1) & ~(size - 1);
    char *start = (char *)(aligned - guard);
    char *end = start + span;
    if (start > probe) munmap(probe, (size_t)(start - probe));
    if (end < probe + total) munmap(end, (size_t)(probe + total - end));
    g_cage.base = aligned;
    g_cage.size = size;
    return true;
}

static bool
ns_cage_os_commit(void *ptr, size_t len)
{
    return mprotect(ptr, len, PROT_READ | PROT_WRITE) == 0;
}

static void
ns_cage_os_decommit(void *ptr, size_t len)
{
    (void)madvise(ptr, len, MADV_DONTNEED);
    (void)mprotect(ptr, len, PROT_NONE);
}

#endif

static gboolean
ns_cage_disabled_by_env(void)
{
    const char *v = g_getenv("NS_JS_HEAP_CAGE");
    return v && (strcmp(v, "0") == 0 || g_ascii_strcasecmp(v, "off") == 0);
}

static gpointer
ns_cage_init_once(gpointer unused)
{
    (void)unused;
#if !NS_CAGE_SUPPORTED
    return NULL;
#else
    if (ns_cage_disabled_by_env()) return NULL;
    size_t size = NS_CAGE_MAX_SIZE;
    bool reserved = false;
    while (size >= NS_CAGE_MIN_SIZE) {
        size_t guard = MIN(NS_CAGE_GUARD_SIZE, size);
        if (ns_cage_os_reserve(size, guard)) {
            reserved = true;
            break;
        }
        size >>= 1;
    }
    if (!reserved) {
        g_warning("js heap cage: could not reserve address space; "
                  "QuickJS runtimes fall back to the process heap");
        return NULL;
    }
    g_cage.n_chunks = (uint32_t)(g_cage.size >> NS_CAGE_CHUNK_SHIFT);
    g_cage.meta = g_malloc0(sizeof(ns_cage_chunk *) * g_cage.n_chunks);
    size_t words = (g_cage.n_chunks + 63) / 64;
    g_cage.free_bits = g_malloc(sizeof(uint64_t) * words);
    memset(g_cage.free_bits, 0xff, sizeof(uint64_t) * words);
    g_cage.committed_bits = g_malloc0(sizeof(uint64_t) * words);
    g_mutex_init(&g_cage.lock);
    g_cage.ready = true;
    JS_SetHeapCage(g_cage.base, g_cage.size);
    g_info("js heap cage: %zu MiB reserved at %p",
           g_cage.size >> 20, (void *)g_cage.base);
    return NULL;
#endif
}

static bool
ns_cage_ready(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, ns_cage_init_once, NULL);
    return g_cage.ready;
}

static bool
ns_cage_find_free_run(uint32_t n, uint32_t *out_index)
{
    uint32_t run = 0;
    uint32_t start = 0;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t from = pass == 0 ? g_cage.scan_hint : 0;
        uint32_t to = pass == 0 ? g_cage.n_chunks : g_cage.scan_hint;
        run = 0;
        for (uint32_t i = from; i < to; i++) {
            if ((i & 63) == 0 && g_cage.free_bits[i >> 6] == 0 && run == 0) {
                i += 63;
                continue;
            }
            if (ns_bit_test(g_cage.free_bits, i)) {
                if (run == 0) start = i;
                if (++run == n) {
                    *out_index = start;
                    return true;
                }
            } else {
                run = 0;
            }
        }
    }
    return false;
}

static ns_cage_chunk *
ns_cage_acquire_chunks(ns_cage_heap *heap, uint32_t n)
{
    uint32_t index;
    g_mutex_lock(&g_cage.lock);
    if (!ns_cage_find_free_run(n, &index)) {
        g_mutex_unlock(&g_cage.lock);
        return NULL;
    }
    uint32_t fresh = 0;
    for (uint32_t i = index; i < index + n; i++) {
        if (ns_bit_test(g_cage.committed_bits, i)) {
            ns_bit_clear(g_cage.committed_bits, i);
            g_cage.committed_free--;
        } else {
            fresh++;
        }
    }
    if (fresh) {
        if (!ns_cage_os_commit(ns_cage_chunk_addr(index), (size_t)n << NS_CAGE_CHUNK_SHIFT)) {
            ns_cage_os_decommit(ns_cage_chunk_addr(index), (size_t)n << NS_CAGE_CHUNK_SHIFT);
            g_cage.committed_bytes -= (size_t)(n - fresh) << NS_CAGE_CHUNK_SHIFT;
            g_mutex_unlock(&g_cage.lock);
            return NULL;
        }
        g_cage.committed_bytes += (size_t)fresh << NS_CAGE_CHUNK_SHIFT;
    }
    for (uint32_t i = index; i < index + n; i++) ns_bit_clear(g_cage.free_bits, i);
    g_cage.scan_hint = index + n;
    ns_cage_chunk *chunk = g_malloc0(sizeof(*chunk));
    chunk->heap = heap;
    chunk->index = index;
    chunk->huge_chunks = n > 1 ? n : 0;
    chunk->free_slabs = n == 1 ? NS_CAGE_ALL_SLABS_FREE : 0;
    for (uint32_t s = 0; s < NS_CAGE_SLABS_PER_CHUNK; s++) {
        chunk->slabs[s].chunk = chunk;
        chunk->slabs[s].index = (uint8_t)s;
    }
    g_atomic_pointer_set(&g_cage.meta[index], chunk);
    g_mutex_unlock(&g_cage.lock);
    chunk->next = heap->chunks;
    if (heap->chunks) heap->chunks->prev = chunk;
    heap->chunks = chunk;
    return chunk;
}

static void
ns_cage_release_chunks(ns_cage_chunk *chunk)
{
    ns_cage_heap *heap = chunk->heap;
    if (chunk->prev) chunk->prev->next = chunk->next;
    else heap->chunks = chunk->next;
    if (chunk->next) chunk->next->prev = chunk->prev;
    uint32_t n = chunk->huge_chunks ? chunk->huge_chunks : 1;
    uint32_t index = chunk->index;
    g_mutex_lock(&g_cage.lock);
    g_atomic_pointer_set(&g_cage.meta[index], NULL);
    if (n == 1 && g_cage.committed_free < NS_CAGE_COMMITTED_CACHE) {
        ns_bit_set(g_cage.committed_bits, index);
        g_cage.committed_free++;
    } else {
        ns_cage_os_decommit(ns_cage_chunk_addr(index), (size_t)n << NS_CAGE_CHUNK_SHIFT);
        g_cage.committed_bytes -= (size_t)n << NS_CAGE_CHUNK_SHIFT;
    }
    for (uint32_t i = index; i < index + n; i++) ns_bit_set(g_cage.free_bits, i);
    if (index < g_cage.scan_hint) g_cage.scan_hint = index;
    g_mutex_unlock(&g_cage.lock);
    g_free(chunk);
}

static inline uint32_t
ns_cage_class_index(size_t size)
{
    if (size <= 128) return size <= 16 ? 0 : (uint32_t)((size + 15) / 16 - 1);
    uint32_t k = 63 - (uint32_t)__builtin_clzll((unsigned long long)(size - 1));
    return 8 + 4 * (k - 7) + (uint32_t)((size - ((size_t)1 << k) - 1) >> (k - 2));
}

static inline void *
ns_cage_slab_addr(const ns_cage_slab *slab)
{
    return (char *)ns_cage_chunk_addr(slab->chunk->index) +
           ((size_t)slab->index << NS_CAGE_SLAB_SHIFT);
}

static void
ns_cage_partial_link(ns_cage_heap *heap, ns_cage_slab *slab)
{
    slab->prev_partial = NULL;
    slab->next_partial = heap->partial[slab->class_idx];
    if (slab->next_partial) slab->next_partial->prev_partial = slab;
    heap->partial[slab->class_idx] = slab;
}

static void
ns_cage_partial_unlink(ns_cage_heap *heap, ns_cage_slab *slab)
{
    if (slab->prev_partial) slab->prev_partial->next_partial = slab->next_partial;
    else heap->partial[slab->class_idx] = slab->next_partial;
    if (slab->next_partial) slab->next_partial->prev_partial = slab->prev_partial;
    slab->prev_partial = slab->next_partial = NULL;
}

static int
ns_cage_find_slab_run(uint16_t free_slabs, uint32_t k)
{
    uint16_t mask = (uint16_t)((1u << k) - 1);
    for (uint32_t s = 0; s + k <= NS_CAGE_SLABS_PER_CHUNK; s++) {
        if (((free_slabs >> s) & mask) == mask) return (int)s;
    }
    return -1;
}

static ns_cage_slab *
ns_cage_take_slabs(ns_cage_heap *heap, uint32_t k)
{
    for (ns_cage_chunk *c = heap->chunks; c; c = c->next) {
        if (!c->free_slabs) continue;
        int s = ns_cage_find_slab_run(c->free_slabs, k);
        if (s < 0) continue;
        c->free_slabs &= (uint16_t)~(((1u << k) - 1) << s);
        return &c->slabs[s];
    }
    ns_cage_chunk *c = ns_cage_acquire_chunks(heap, 1);
    if (!c) return NULL;
    c->free_slabs &= (uint16_t)~((1u << k) - 1);
    return &c->slabs[0];
}

static void
ns_cage_give_back_slabs(ns_cage_slab *head, uint32_t k)
{
    ns_cage_chunk *c = head->chunk;
    for (uint32_t i = 0; i < k; i++) {
        c->slabs[head->index + i].kind = NS_SLAB_FREE;
    }
    c->free_slabs |= (uint16_t)(((1u << k) - 1) << head->index);
    if (c->free_slabs == NS_CAGE_ALL_SLABS_FREE) ns_cage_release_chunks(c);
}

static ns_cage_slab *
ns_cage_new_small_slab(ns_cage_heap *heap, uint32_t class_idx)
{
    ns_cage_slab *slab = ns_cage_take_slabs(heap, 1);
    if (!slab) return NULL;
    uint32_t size = ns_cage_class_sizes[class_idx];
    uint32_t n_blocks = (uint32_t)(NS_CAGE_SLAB_SIZE / size);
    slab->kind = NS_SLAB_SMALL;
    slab->class_idx = (uint8_t)class_idx;
    slab->run_slabs = 1;
    slab->n_blocks = (uint16_t)n_blocks;
    slab->used = 0;
    slab->hint = 0;
    memset(slab->bits, 0xff, sizeof(slab->bits));
    memset(slab->bits, 0, (n_blocks / 64) * sizeof(uint64_t));
    if (n_blocks % 64) slab->bits[n_blocks / 64] &= ~(((uint64_t)1 << (n_blocks % 64)) - 1);
    ns_cage_partial_link(heap, slab);
    return slab;
}

static void *
ns_cage_small_alloc(ns_cage_heap *heap, size_t size)
{
    uint32_t class_idx = ns_cage_class_index(size);
    ns_cage_slab *slab = heap->partial[class_idx];
    if (!slab) {
        slab = ns_cage_new_small_slab(heap, class_idx);
        if (!slab) return NULL;
    }
    uint32_t w = slab->hint;
    while (slab->bits[w] == ~(uint64_t)0) w++;
    uint32_t bit = (uint32_t)__builtin_ctzll(~slab->bits[w]);
    slab->bits[w] |= (uint64_t)1 << bit;
    slab->hint = (uint16_t)w;
    if (++slab->used == slab->n_blocks) ns_cage_partial_unlink(heap, slab);
    uint32_t block = w * 64 + bit;
    return (char *)ns_cage_slab_addr(slab) + (size_t)block * ns_cage_class_sizes[class_idx];
}

static void *
ns_cage_run_alloc(ns_cage_heap *heap, size_t size)
{
    uint32_t k = (uint32_t)((size + NS_CAGE_SLAB_SIZE - 1) >> NS_CAGE_SLAB_SHIFT);
    ns_cage_slab *head = ns_cage_take_slabs(heap, k);
    if (!head) return NULL;
    ns_cage_chunk *c = head->chunk;
    head->kind = NS_SLAB_RUN_HEAD;
    head->run_slabs = (uint16_t)k;
    for (uint32_t i = 1; i < k; i++) {
        c->slabs[head->index + i].kind = NS_SLAB_RUN_TAIL;
        c->slabs[head->index + i].run_slabs = (uint16_t)i;
    }
    return ns_cage_slab_addr(head);
}

static void *
ns_cage_huge_alloc(ns_cage_heap *heap, size_t size)
{
    uint32_t n = (uint32_t)((size + NS_CAGE_CHUNK_SIZE - 1) >> NS_CAGE_CHUNK_SHIFT);
    ns_cage_chunk *c = ns_cage_acquire_chunks(heap, n);
    if (!c) return NULL;
    return ns_cage_chunk_addr(c->index);
}

static void *
ns_cage_heap_alloc(ns_cage_heap *heap, size_t size)
{
    if (size <= NS_CAGE_SMALL_MAX) return ns_cage_small_alloc(heap, size ? size : 1);
    if (size <= NS_CAGE_CHUNK_SIZE) return ns_cage_run_alloc(heap, size);
    return ns_cage_huge_alloc(heap, size);
}

static ns_cage_chunk *
ns_cage_chunk_of(const void *ptr)
{
    if (!g_cage.ready) return NULL;
    uintptr_t p = (uintptr_t)ptr;
    if (p < g_cage.base || p - g_cage.base >= g_cage.size) return NULL;
    return g_atomic_pointer_get(&g_cage.meta[ns_cage_chunk_index(ptr)]);
}

static ns_cage_slab *
ns_cage_slab_of(ns_cage_chunk *chunk, const void *ptr)
{
    uint32_t s = (uint32_t)(((uintptr_t)ptr & (NS_CAGE_CHUNK_SIZE - 1)) >> NS_CAGE_SLAB_SHIFT);
    return &chunk->slabs[s];
}

static G_NORETURN void
ns_cage_corrupt(const char *what, const void *ptr)
{
    g_error("js heap cage: %s at %p", what, ptr);
}

static size_t
ns_cage_usable_size(const void *ptr)
{
    if (!ptr) return 0;
    ns_cage_chunk *chunk = ns_cage_chunk_of(ptr);
    if (!chunk) ns_cage_corrupt("pointer outside the cage", ptr);
    if (chunk->huge_chunks) {
        if (ptr != ns_cage_chunk_addr(chunk->index))
            ns_cage_corrupt("interior pointer into a huge block", ptr);
        return (size_t)chunk->huge_chunks << NS_CAGE_CHUNK_SHIFT;
    }
    ns_cage_slab *slab = ns_cage_slab_of(chunk, ptr);
    switch (slab->kind) {
    case NS_SLAB_SMALL:
        return ns_cage_class_sizes[slab->class_idx];
    case NS_SLAB_RUN_HEAD:
        if (ptr != ns_cage_slab_addr(slab))
            ns_cage_corrupt("interior pointer into a run", ptr);
        return (size_t)slab->run_slabs << NS_CAGE_SLAB_SHIFT;
    default:
        ns_cage_corrupt("pointer into unallocated memory", ptr);
    }
}

static void
ns_cage_heap_free(void *ptr)
{
    if (!ptr) return;
    ns_cage_chunk *chunk = ns_cage_chunk_of(ptr);
    if (!chunk) ns_cage_corrupt("free of a pointer outside the cage", ptr);
    if (chunk->huge_chunks) {
        if (ptr != ns_cage_chunk_addr(chunk->index))
            ns_cage_corrupt("free of an interior huge pointer", ptr);
        ns_cage_release_chunks(chunk);
        return;
    }
    ns_cage_heap *heap = chunk->heap;
    ns_cage_slab *slab = ns_cage_slab_of(chunk, ptr);
    if (slab->kind == NS_SLAB_RUN_HEAD) {
        if (ptr != ns_cage_slab_addr(slab))
            ns_cage_corrupt("free of an interior run pointer", ptr);
        ns_cage_give_back_slabs(slab, slab->run_slabs);
        return;
    }
    if (slab->kind != NS_SLAB_SMALL) ns_cage_corrupt("free of unallocated memory", ptr);
    uint32_t size = ns_cage_class_sizes[slab->class_idx];
    uint32_t off = (uint32_t)((uintptr_t)ptr & (NS_CAGE_SLAB_SIZE - 1));
    if (off % size) ns_cage_corrupt("misaligned free", ptr);
    uint32_t block = off / size;
    if (!ns_bit_test(slab->bits, block)) ns_cage_corrupt("double free", ptr);
    ns_bit_clear(slab->bits, block);
    if ((block >> 6) < slab->hint) slab->hint = (uint16_t)(block >> 6);
    if (slab->used == slab->n_blocks) ns_cage_partial_link(heap, slab);
    if (--slab->used == 0) {
        ns_cage_partial_unlink(heap, slab);
        ns_cage_give_back_slabs(slab, 1);
    }
}

static void *
ns_cage_heap_realloc(ns_cage_heap *heap, void *ptr, size_t size)
{
    if (!ptr) return ns_cage_heap_alloc(heap, size);
    if (size == 0) {
        ns_cage_heap_free(ptr);
        return NULL;
    }
    size_t old = ns_cage_usable_size(ptr);
    if (size <= old) {
        if (old <= NS_CAGE_SMALL_MAX) {
            if (ns_cage_class_index(size) == ns_cage_class_index(old)) return ptr;
        } else if (old <= NS_CAGE_CHUNK_SIZE) {
            ns_cage_chunk *chunk = ns_cage_chunk_of(ptr);
            ns_cage_slab *head = ns_cage_slab_of(chunk, ptr);
            uint32_t k = (uint32_t)((size + NS_CAGE_SLAB_SIZE - 1) >> NS_CAGE_SLAB_SHIFT);
            if (size > NS_CAGE_SMALL_MAX && k < head->run_slabs) {
                ns_cage_slab *tail = &chunk->slabs[head->index + k];
                uint32_t drop = head->run_slabs - k;
                head->run_slabs = (uint16_t)k;
                chunk->free_slabs |= (uint16_t)(((1u << drop) - 1) << tail->index);
                for (uint32_t i = 0; i < drop; i++) chunk->slabs[tail->index + i].kind = NS_SLAB_FREE;
                return ptr;
            }
            if (size > NS_CAGE_SMALL_MAX) return ptr;
        } else {
            if (size > NS_CAGE_CHUNK_SIZE) return ptr;
        }
    }
    void *fresh = ns_cage_heap_alloc(heap, size);
    if (!fresh) return NULL;
    memcpy(fresh, ptr, MIN(old, size));
    ns_cage_heap_free(ptr);
    return fresh;
}

static void *
ns_cage_mf_calloc(void *opaque, size_t count, size_t size)
{
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) return NULL;
    void *p = ns_cage_heap_alloc(opaque, total);
    if (p) memset(p, 0, total);
    return p;
}

static void *
ns_cage_mf_malloc(void *opaque, size_t size)
{
    return ns_cage_heap_alloc(opaque, size);
}

static void
ns_cage_mf_free(void *opaque, void *ptr)
{
    (void)opaque;
    ns_cage_heap_free(ptr);
}

static void *
ns_cage_mf_realloc(void *opaque, void *ptr, size_t size)
{
    return ns_cage_heap_realloc(opaque, ptr, size);
}

static size_t
ns_cage_mf_usable_size(const void *ptr)
{
    return ns_cage_usable_size(ptr);
}

static const JSMallocFunctions ns_cage_malloc_functions = {
    ns_cage_mf_calloc,
    ns_cage_mf_malloc,
    ns_cage_mf_free,
    ns_cage_mf_realloc,
    ns_cage_mf_usable_size,
};

static void
ns_cage_heap_destroy(ns_cage_heap *heap)
{
    while (heap->chunks) ns_cage_release_chunks(heap->chunks);
    g_free(heap);
}

JSRuntime *
ns_js_runtime_new(void)
{
    if (!ns_cage_ready()) return JS_NewRuntime();
    ns_cage_heap *heap = g_malloc0(sizeof(*heap));
    JSRuntime *rt = JS_NewRuntime2(&ns_cage_malloc_functions, heap);
    if (!rt) {
        ns_cage_heap_destroy(heap);
        return NULL;
    }
    g_mutex_lock(&g_shared_lock);
    if (!g_heaps) g_heaps = g_hash_table_new(NULL, NULL);
    g_hash_table_insert(g_heaps, rt, heap);
    g_mutex_unlock(&g_shared_lock);
    return rt;
}

void
ns_js_runtime_free(JSRuntime *rt)
{
    if (!rt) return;
    g_mutex_lock(&g_shared_lock);
    ns_cage_heap *heap = g_heaps ? g_hash_table_lookup(g_heaps, rt) : NULL;
    if (heap) g_hash_table_remove(g_heaps, rt);
    g_mutex_unlock(&g_shared_lock);
    JS_FreeRuntime(rt);
    if (heap) ns_cage_heap_destroy(heap);
}

gboolean
ns_js_cage_enabled(void)
{
    return ns_cage_ready();
}

gboolean
ns_js_cage_contains(const void *ptr)
{
    if (!g_cage.ready) return FALSE;
    uintptr_t p = (uintptr_t)ptr;
    return p >= g_cage.base && p - g_cage.base < g_cage.size;
}

uintptr_t
ns_js_cage_base(void)
{
    return g_cage.base;
}

size_t
ns_js_cage_size(void)
{
    return g_cage.size;
}

size_t
ns_js_cage_committed_bytes(void)
{
    g_mutex_lock(&g_cage.lock);
    size_t bytes = g_cage.committed_bytes;
    g_mutex_unlock(&g_cage.lock);
    return bytes;
}

static ns_cage_heap *g_shared_heap;

void *
ns_js_cage_shared_malloc(size_t size)
{
    g_mutex_lock(&g_shared_lock);
    void *p;
    if (ns_cage_ready()) {
        if (!g_shared_heap) g_shared_heap = g_malloc0(sizeof(*g_shared_heap));
        p = ns_cage_heap_alloc(g_shared_heap, size);
    } else {
        p = ns_shared_fallback_malloc(size);
    }
    g_mutex_unlock(&g_shared_lock);
    return p;
}

void *
ns_js_cage_shared_realloc(void *ptr, size_t size)
{
    if (!ptr) return ns_js_cage_shared_malloc(size);
    g_mutex_lock(&g_shared_lock);
    void *p = ns_cage_chunk_of(ptr) ? ns_cage_heap_realloc(g_shared_heap, ptr, size)
                                    : ns_shared_fallback_realloc(ptr, size);
    g_mutex_unlock(&g_shared_lock);
    return p;
}

void
ns_js_cage_shared_free(void *ptr)
{
    if (!ptr) return;
    g_mutex_lock(&g_shared_lock);
    if (ns_cage_chunk_of(ptr)) ns_cage_heap_free(ptr);
    else ns_shared_fallback_free(ptr);
    g_mutex_unlock(&g_shared_lock);
}

size_t
ns_js_cage_shared_usable_size(const void *ptr)
{
    if (!ptr) return 0;
    g_mutex_lock(&g_shared_lock);
    size_t n = ns_cage_chunk_of(ptr) ? ns_cage_usable_size(ptr)
                                     : ns_shared_fallback_usable_size(ptr);
    g_mutex_unlock(&g_shared_lock);
    return n;
}

#else

JSRuntime *
ns_js_runtime_new(void)
{
    return JS_NewRuntime();
}

void
ns_js_runtime_free(JSRuntime *rt)
{
    if (rt) JS_FreeRuntime(rt);
}

gboolean
ns_js_cage_enabled(void)
{
    return FALSE;
}

gboolean
ns_js_cage_contains(const void *ptr)
{
    (void)ptr;
    return FALSE;
}

uintptr_t
ns_js_cage_base(void)
{
    return 0;
}

size_t
ns_js_cage_size(void)
{
    return 0;
}

size_t
ns_js_cage_committed_bytes(void)
{
    return 0;
}

void *
ns_js_cage_shared_malloc(size_t size)
{
    g_mutex_lock(&g_shared_lock);
    void *p = ns_shared_fallback_malloc(size);
    g_mutex_unlock(&g_shared_lock);
    return p;
}

void *
ns_js_cage_shared_realloc(void *ptr, size_t size)
{
    g_mutex_lock(&g_shared_lock);
    void *p = ns_shared_fallback_realloc(ptr, size);
    g_mutex_unlock(&g_shared_lock);
    return p;
}

void
ns_js_cage_shared_free(void *ptr)
{
    if (!ptr) return;
    g_mutex_lock(&g_shared_lock);
    ns_shared_fallback_free(ptr);
    g_mutex_unlock(&g_shared_lock);
}

size_t
ns_js_cage_shared_usable_size(const void *ptr)
{
    if (!ptr) return 0;
    g_mutex_lock(&g_shared_lock);
    size_t n = ns_shared_fallback_usable_size(ptr);
    g_mutex_unlock(&g_shared_lock);
    return n;
}

#endif
