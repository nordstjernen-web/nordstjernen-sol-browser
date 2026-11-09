/* QuickJS heap cage: one reserved address range per process that holds every GC allocation. */

#include "quickjs-cage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cutils.h"
#include "quickjs-c-atomics.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define JS_CAGE_CHUNK_SHIFT 20
#define JS_CAGE_CHUNK_SIZE ((size_t)1 << JS_CAGE_CHUNK_SHIFT)
#define JS_CAGE_SLAB_SHIFT 16
#define JS_CAGE_SLAB_SIZE ((size_t)1 << JS_CAGE_SLAB_SHIFT)
#define JS_CAGE_SLABS_PER_CHUNK (JS_CAGE_CHUNK_SIZE / JS_CAGE_SLAB_SIZE)
#define JS_CAGE_SMALL_MAX 32768u
#define JS_CAGE_MIN_BLOCK 16u
#define JS_CAGE_BITMAP_WORDS (JS_CAGE_SLAB_SIZE / JS_CAGE_MIN_BLOCK / 64)
#define JS_CAGE_GUARD_SIZE ((size_t)32 << 30)
#define JS_CAGE_MAX_SIZE ((size_t)1 << 32)
#define JS_CAGE_MIN_SIZE ((size_t)1 << 30)
#define JS_CAGE_COMMITTED_CACHE 32
#define JS_CAGE_ALL_SLABS_FREE ((uint16_t)0xffff)

enum {
    JS_SLAB_FREE,
    JS_SLAB_SMALL,
    JS_SLAB_RUN_HEAD,
    JS_SLAB_RUN_TAIL,
};

static const uint32_t js_cage_class_sizes[] = {
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
#define JS_CAGE_CLASS_COUNT countof(js_cage_class_sizes)

typedef struct JSCageChunk JSCageChunk;

typedef struct JSCageSlab {
    JSCageChunk *chunk;
    struct JSCageSlab *prev_partial;
    struct JSCageSlab *next_partial;
    uint8_t  kind;
    uint8_t  class_idx;
    uint8_t  index;
    uint16_t run_slabs;
    uint16_t n_blocks;
    uint16_t used;
    uint16_t hint;
    uint64_t bits[JS_CAGE_BITMAP_WORDS];
} JSCageSlab;

struct JSCageChunk {
    JSCageHeap *heap;
    JSCageChunk *prev;
    JSCageChunk *next;
    uint32_t index;
    uint32_t huge_chunks;
    uint16_t free_slabs;
    JSCageSlab slabs[JS_CAGE_SLABS_PER_CHUNK];
};

struct JSCageHeap {
    JSCageSlab *partial[JS_CAGE_CLASS_COUNT];
    JSCageChunk *chunks;
};

static struct {
    uintptr_t base;
    size_t size;
    uint32_t n_chunks;
    uint32_t committed_free;
    uint32_t scan_hint;
    size_t committed_bytes;
    js_mutex_t lock;
    js_mutex_t shared_lock;
    JSCageHeap *shared_heap;
    JSCageChunk *_Atomic *meta;
    uint64_t *free_bits;
    uint64_t *committed_bits;
    bool ready;
} g_cage;

uintptr_t js_cage_pointer_base;

static inline void *js_cage_chunk_addr(uint32_t index)
{
    return (void *)(g_cage.base + ((uintptr_t)index << JS_CAGE_CHUNK_SHIFT));
}

static inline uint32_t js_cage_chunk_index(const void *ptr)
{
    return (uint32_t)(((uintptr_t)ptr - g_cage.base) >> JS_CAGE_CHUNK_SHIFT);
}

static inline bool js_bit_test(const uint64_t *bits, uint32_t i)
{
    return (bits[i >> 6] >> (i & 63)) & 1;
}

static inline void js_bit_set(uint64_t *bits, uint32_t i)
{
    bits[i >> 6] |= (uint64_t)1 << (i & 63);
}

static inline void js_bit_clear(uint64_t *bits, uint32_t i)
{
    bits[i >> 6] &= ~((uint64_t)1 << (i & 63));
}

#if defined(_WIN32)

static bool js_cage_os_reserve(size_t size, size_t guard)
{
    size_t span = size + 2 * guard;
    for (int attempt = 0; attempt < 8; attempt++) {
        char *probe = VirtualAlloc(NULL, span + size, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe)
            return false;
        uintptr_t aligned = ((uintptr_t)probe + guard + size - 1) & ~(size - 1);
        VirtualFree(probe, 0, MEM_RELEASE);
        void *got = VirtualAlloc((void *)(aligned - guard), span, MEM_RESERVE, PAGE_NOACCESS);
        if (got) {
            g_cage.base = aligned;
            g_cage.size = size;
            return true;
        }
    }
    return false;
}

static bool js_cage_os_commit(void *ptr, size_t len)
{
    return VirtualAlloc(ptr, len, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static void js_cage_os_decommit(void *ptr, size_t len)
{
    VirtualFree(ptr, len, MEM_DECOMMIT);
}

#else

static bool js_cage_os_reserve(size_t size, size_t guard)
{
    size_t span = size + 2 * guard;
    size_t total = span + size;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    char *probe = mmap(NULL, total, PROT_NONE, flags, -1, 0);
    if (probe == MAP_FAILED)
        return false;
    uintptr_t aligned = ((uintptr_t)probe + guard + size - 1) & ~(size - 1);
    char *start = (char *)(aligned - guard);
    char *end = start + span;
    if (start > probe)
        munmap(probe, (size_t)(start - probe));
    if (end < probe + total)
        munmap(end, (size_t)(probe + total - end));
    g_cage.base = aligned;
    g_cage.size = size;
    return true;
}

static bool js_cage_os_commit(void *ptr, size_t len)
{
    return mprotect(ptr, len, PROT_READ | PROT_WRITE) == 0;
}

static void js_cage_os_decommit(void *ptr, size_t len)
{
    (void)madvise(ptr, len, MADV_DONTNEED);
    (void)mprotect(ptr, len, PROT_NONE);
}

#endif

static void js_cage_init_once(void)
{
    size_t size = JS_CAGE_MAX_SIZE;
    bool reserved = false;
    while (size >= JS_CAGE_MIN_SIZE) {
        size_t guard = size < JS_CAGE_GUARD_SIZE ? size : JS_CAGE_GUARD_SIZE;
        if (js_cage_os_reserve(size, guard)) {
            reserved = true;
            break;
        }
        size >>= 1;
    }
    if (!reserved) {
        fprintf(stderr, "quickjs: could not reserve the heap cage address range\n");
        return;
    }
    g_cage.n_chunks = (uint32_t)(g_cage.size >> JS_CAGE_CHUNK_SHIFT);
    g_cage.meta = calloc(g_cage.n_chunks, sizeof(*g_cage.meta));
    size_t words = (g_cage.n_chunks + 63) / 64;
    g_cage.free_bits = malloc(sizeof(uint64_t) * words);
    g_cage.committed_bits = calloc(words, sizeof(uint64_t));
    if (!g_cage.meta || !g_cage.free_bits || !g_cage.committed_bits) {
        fprintf(stderr, "quickjs: could not allocate the heap cage tables\n");
        return;
    }
    memset(g_cage.free_bits, 0xff, sizeof(uint64_t) * words);
    js_bit_clear(g_cage.free_bits, 0);
    g_cage.scan_hint = 1;
    js_mutex_init(&g_cage.lock);
    js_mutex_init(&g_cage.shared_lock);
    js_cage_pointer_base = g_cage.base;
    g_cage.ready = true;
}

bool js_cage_init(void)
{
    static js_once_t once = JS_ONCE_INIT;
    js_once(&once, js_cage_init_once);
    return g_cage.ready;
}

bool js_cage_ready(void)
{
    return g_cage.ready;
}

uintptr_t js_cage_base(void)
{
    return g_cage.base;
}

size_t js_cage_size(void)
{
    return g_cage.size;
}

bool js_cage_contains(const void *ptr)
{
    if (!g_cage.ready)
        return false;
    uintptr_t p = (uintptr_t)ptr;
    return p >= g_cage.base && p - g_cage.base < g_cage.size;
}

size_t js_cage_committed_bytes(void)
{
    if (!g_cage.ready)
        return 0;
    js_mutex_lock(&g_cage.lock);
    size_t bytes = g_cage.committed_bytes;
    js_mutex_unlock(&g_cage.lock);
    return bytes;
}

static bool js_cage_find_free_run(uint32_t n, uint32_t *out_index)
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
            if (js_bit_test(g_cage.free_bits, i)) {
                if (run == 0)
                    start = i;
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

static JSCageChunk *js_cage_acquire_chunks(JSCageHeap *heap, uint32_t n)
{
    uint32_t index;
    js_mutex_lock(&g_cage.lock);
    if (!js_cage_find_free_run(n, &index)) {
        js_mutex_unlock(&g_cage.lock);
        return NULL;
    }
    uint32_t fresh = 0;
    for (uint32_t i = index; i < index + n; i++) {
        if (js_bit_test(g_cage.committed_bits, i)) {
            js_bit_clear(g_cage.committed_bits, i);
            g_cage.committed_free--;
        } else {
            fresh++;
        }
    }
    if (fresh) {
        if (!js_cage_os_commit(js_cage_chunk_addr(index), (size_t)n << JS_CAGE_CHUNK_SHIFT)) {
            js_cage_os_decommit(js_cage_chunk_addr(index), (size_t)n << JS_CAGE_CHUNK_SHIFT);
            g_cage.committed_bytes -= (size_t)(n - fresh) << JS_CAGE_CHUNK_SHIFT;
            js_mutex_unlock(&g_cage.lock);
            return NULL;
        }
        g_cage.committed_bytes += (size_t)fresh << JS_CAGE_CHUNK_SHIFT;
    }
    for (uint32_t i = index; i < index + n; i++)
        js_bit_clear(g_cage.free_bits, i);
    g_cage.scan_hint = index + n;
    JSCageChunk *chunk = calloc(1, sizeof(*chunk));
    if (!chunk) {
        for (uint32_t i = index; i < index + n; i++)
            js_bit_set(g_cage.free_bits, i);
        js_mutex_unlock(&g_cage.lock);
        return NULL;
    }
    chunk->heap = heap;
    chunk->index = index;
    chunk->huge_chunks = n > 1 ? n : 0;
    chunk->free_slabs = n == 1 ? JS_CAGE_ALL_SLABS_FREE : 0;
    for (uint32_t s = 0; s < JS_CAGE_SLABS_PER_CHUNK; s++) {
        chunk->slabs[s].chunk = chunk;
        chunk->slabs[s].index = (uint8_t)s;
    }
    atomic_store(&g_cage.meta[index], chunk);
    js_mutex_unlock(&g_cage.lock);
    chunk->next = heap->chunks;
    if (heap->chunks)
        heap->chunks->prev = chunk;
    heap->chunks = chunk;
    return chunk;
}

static void js_cage_release_chunks(JSCageChunk *chunk)
{
    JSCageHeap *heap = chunk->heap;
    if (chunk->prev)
        chunk->prev->next = chunk->next;
    else
        heap->chunks = chunk->next;
    if (chunk->next)
        chunk->next->prev = chunk->prev;
    uint32_t n = chunk->huge_chunks ? chunk->huge_chunks : 1;
    uint32_t index = chunk->index;
    js_mutex_lock(&g_cage.lock);
    atomic_store(&g_cage.meta[index], (JSCageChunk *)NULL);
    if (n == 1 && g_cage.committed_free < JS_CAGE_COMMITTED_CACHE) {
        js_bit_set(g_cage.committed_bits, index);
        g_cage.committed_free++;
    } else {
        js_cage_os_decommit(js_cage_chunk_addr(index), (size_t)n << JS_CAGE_CHUNK_SHIFT);
        g_cage.committed_bytes -= (size_t)n << JS_CAGE_CHUNK_SHIFT;
    }
    for (uint32_t i = index; i < index + n; i++)
        js_bit_set(g_cage.free_bits, i);
    if (index < g_cage.scan_hint)
        g_cage.scan_hint = index;
    js_mutex_unlock(&g_cage.lock);
    free(chunk);
}

static inline uint32_t js_cage_class_index(size_t size)
{
    if (size <= 128)
        return size <= 16 ? 0 : (uint32_t)((size + 15) / 16 - 1);
    uint32_t k = 63 - clz64((uint64_t)(size - 1));
    return 8 + 4 * (k - 7) + (uint32_t)((size - ((size_t)1 << k) - 1) >> (k - 2));
}

static inline void *js_cage_slab_addr(const JSCageSlab *slab)
{
    return (char *)js_cage_chunk_addr(slab->chunk->index) +
           ((size_t)slab->index << JS_CAGE_SLAB_SHIFT);
}

static void js_cage_partial_link(JSCageHeap *heap, JSCageSlab *slab)
{
    slab->prev_partial = NULL;
    slab->next_partial = heap->partial[slab->class_idx];
    if (slab->next_partial)
        slab->next_partial->prev_partial = slab;
    heap->partial[slab->class_idx] = slab;
}

static void js_cage_partial_unlink(JSCageHeap *heap, JSCageSlab *slab)
{
    if (slab->prev_partial)
        slab->prev_partial->next_partial = slab->next_partial;
    else
        heap->partial[slab->class_idx] = slab->next_partial;
    if (slab->next_partial)
        slab->next_partial->prev_partial = slab->prev_partial;
    slab->prev_partial = slab->next_partial = NULL;
}

static int js_cage_find_slab_run(uint16_t free_slabs, uint32_t k)
{
    uint16_t mask = (uint16_t)((1u << k) - 1);
    for (uint32_t s = 0; s + k <= JS_CAGE_SLABS_PER_CHUNK; s++) {
        if (((free_slabs >> s) & mask) == mask)
            return (int)s;
    }
    return -1;
}

static JSCageSlab *js_cage_take_slabs(JSCageHeap *heap, uint32_t k)
{
    for (JSCageChunk *c = heap->chunks; c; c = c->next) {
        if (!c->free_slabs)
            continue;
        int s = js_cage_find_slab_run(c->free_slabs, k);
        if (s < 0)
            continue;
        c->free_slabs &= (uint16_t)~(((1u << k) - 1) << s);
        return &c->slabs[s];
    }
    JSCageChunk *c = js_cage_acquire_chunks(heap, 1);
    if (!c)
        return NULL;
    c->free_slabs &= (uint16_t)~((1u << k) - 1);
    return &c->slabs[0];
}

static void js_cage_give_back_slabs(JSCageSlab *head, uint32_t k)
{
    JSCageChunk *c = head->chunk;
    for (uint32_t i = 0; i < k; i++)
        c->slabs[head->index + i].kind = JS_SLAB_FREE;
    c->free_slabs |= (uint16_t)(((1u << k) - 1) << head->index);
    if (c->free_slabs == JS_CAGE_ALL_SLABS_FREE)
        js_cage_release_chunks(c);
}

static JSCageSlab *js_cage_new_small_slab(JSCageHeap *heap, uint32_t class_idx)
{
    JSCageSlab *slab = js_cage_take_slabs(heap, 1);
    if (!slab)
        return NULL;
    uint32_t size = js_cage_class_sizes[class_idx];
    uint32_t n_blocks = (uint32_t)(JS_CAGE_SLAB_SIZE / size);
    slab->kind = JS_SLAB_SMALL;
    slab->class_idx = (uint8_t)class_idx;
    slab->run_slabs = 1;
    slab->n_blocks = (uint16_t)n_blocks;
    slab->used = 0;
    slab->hint = 0;
    memset(slab->bits, 0xff, sizeof(slab->bits));
    memset(slab->bits, 0, (n_blocks / 64) * sizeof(uint64_t));
    if (n_blocks % 64)
        slab->bits[n_blocks / 64] &= ~(((uint64_t)1 << (n_blocks % 64)) - 1);
    js_cage_partial_link(heap, slab);
    return slab;
}

static void *js_cage_small_alloc(JSCageHeap *heap, size_t size)
{
    uint32_t class_idx = js_cage_class_index(size);
    JSCageSlab *slab = heap->partial[class_idx];
    if (!slab) {
        slab = js_cage_new_small_slab(heap, class_idx);
        if (!slab)
            return NULL;
    }
    uint32_t w = slab->hint;
    while (slab->bits[w] == ~(uint64_t)0)
        w++;
    uint32_t bit = ctz64(~slab->bits[w]);
    slab->bits[w] |= (uint64_t)1 << bit;
    slab->hint = (uint16_t)w;
    if (++slab->used == slab->n_blocks)
        js_cage_partial_unlink(heap, slab);
    uint32_t block = w * 64 + bit;
    return (char *)js_cage_slab_addr(slab) + (size_t)block * js_cage_class_sizes[class_idx];
}

static void *js_cage_run_alloc(JSCageHeap *heap, size_t size)
{
    uint32_t k = (uint32_t)((size + JS_CAGE_SLAB_SIZE - 1) >> JS_CAGE_SLAB_SHIFT);
    JSCageSlab *head = js_cage_take_slabs(heap, k);
    if (!head)
        return NULL;
    JSCageChunk *c = head->chunk;
    head->kind = JS_SLAB_RUN_HEAD;
    head->run_slabs = (uint16_t)k;
    for (uint32_t i = 1; i < k; i++) {
        c->slabs[head->index + i].kind = JS_SLAB_RUN_TAIL;
        c->slabs[head->index + i].run_slabs = (uint16_t)i;
    }
    return js_cage_slab_addr(head);
}

static void *js_cage_huge_alloc(JSCageHeap *heap, size_t size)
{
    uint32_t n = (uint32_t)((size + JS_CAGE_CHUNK_SIZE - 1) >> JS_CAGE_CHUNK_SHIFT);
    JSCageChunk *c = js_cage_acquire_chunks(heap, n);
    if (!c)
        return NULL;
    return js_cage_chunk_addr(c->index);
}

void *js_cage_heap_alloc(JSCageHeap *heap, size_t size)
{
    if (size <= JS_CAGE_SMALL_MAX)
        return js_cage_small_alloc(heap, size ? size : 1);
    if (size <= JS_CAGE_CHUNK_SIZE)
        return js_cage_run_alloc(heap, size);
    return js_cage_huge_alloc(heap, size);
}

static JSCageChunk *js_cage_chunk_of(const void *ptr)
{
    if (!g_cage.ready)
        return NULL;
    uintptr_t p = (uintptr_t)ptr;
    if (p < g_cage.base || p - g_cage.base >= g_cage.size)
        return NULL;
    return atomic_load(&g_cage.meta[js_cage_chunk_index(ptr)]);
}

static JSCageSlab *js_cage_slab_of(JSCageChunk *chunk, const void *ptr)
{
    uint32_t s = (uint32_t)(((uintptr_t)ptr & (JS_CAGE_CHUNK_SIZE - 1)) >> JS_CAGE_SLAB_SHIFT);
    return &chunk->slabs[s];
}

static no_inline __attribute__((noreturn)) void js_cage_corrupt(const char *what, const void *ptr)
{
    fprintf(stderr, "quickjs heap cage: %s at %p\n", what, ptr);
    abort();
}

size_t js_cage_usable_size(const void *ptr)
{
    if (!ptr)
        return 0;
    JSCageChunk *chunk = js_cage_chunk_of(ptr);
    if (!chunk)
        js_cage_corrupt("pointer outside the cage", ptr);
    if (chunk->huge_chunks) {
        if (ptr != js_cage_chunk_addr(chunk->index))
            js_cage_corrupt("interior pointer into a huge block", ptr);
        return (size_t)chunk->huge_chunks << JS_CAGE_CHUNK_SHIFT;
    }
    JSCageSlab *slab = js_cage_slab_of(chunk, ptr);
    switch (slab->kind) {
    case JS_SLAB_SMALL:
        return js_cage_class_sizes[slab->class_idx];
    case JS_SLAB_RUN_HEAD:
        if (ptr != js_cage_slab_addr(slab))
            js_cage_corrupt("interior pointer into a run", ptr);
        return (size_t)slab->run_slabs << JS_CAGE_SLAB_SHIFT;
    default:
        js_cage_corrupt("pointer into unallocated memory", ptr);
    }
}

void js_cage_free(void *ptr)
{
    if (!ptr)
        return;
    JSCageChunk *chunk = js_cage_chunk_of(ptr);
    if (!chunk)
        js_cage_corrupt("free of a pointer outside the cage", ptr);
    if (chunk->huge_chunks) {
        if (ptr != js_cage_chunk_addr(chunk->index))
            js_cage_corrupt("free of an interior huge pointer", ptr);
        js_cage_release_chunks(chunk);
        return;
    }
    JSCageHeap *heap = chunk->heap;
    JSCageSlab *slab = js_cage_slab_of(chunk, ptr);
    if (slab->kind == JS_SLAB_RUN_HEAD) {
        if (ptr != js_cage_slab_addr(slab))
            js_cage_corrupt("free of an interior run pointer", ptr);
        js_cage_give_back_slabs(slab, slab->run_slabs);
        return;
    }
    if (slab->kind != JS_SLAB_SMALL)
        js_cage_corrupt("free of unallocated memory", ptr);
    uint32_t size = js_cage_class_sizes[slab->class_idx];
    uint32_t off = (uint32_t)((uintptr_t)ptr & (JS_CAGE_SLAB_SIZE - 1));
    if (off % size)
        js_cage_corrupt("misaligned free", ptr);
    uint32_t block = off / size;
    if (!js_bit_test(slab->bits, block))
        js_cage_corrupt("double free", ptr);
    js_bit_clear(slab->bits, block);
    if ((block >> 6) < slab->hint)
        slab->hint = (uint16_t)(block >> 6);
    if (slab->used == slab->n_blocks)
        js_cage_partial_link(heap, slab);
    if (--slab->used == 0) {
        js_cage_partial_unlink(heap, slab);
        js_cage_give_back_slabs(slab, 1);
    }
}

void *js_cage_heap_realloc(JSCageHeap *heap, void *ptr, size_t size)
{
    if (!ptr)
        return js_cage_heap_alloc(heap, size);
    if (size == 0) {
        js_cage_free(ptr);
        return NULL;
    }
    size_t old = js_cage_usable_size(ptr);
    if (size <= old) {
        if (old <= JS_CAGE_SMALL_MAX) {
            if (js_cage_class_index(size) == js_cage_class_index(old))
                return ptr;
        } else if (old <= JS_CAGE_CHUNK_SIZE) {
            JSCageChunk *chunk = js_cage_chunk_of(ptr);
            JSCageSlab *head = js_cage_slab_of(chunk, ptr);
            uint32_t k = (uint32_t)((size + JS_CAGE_SLAB_SIZE - 1) >> JS_CAGE_SLAB_SHIFT);
            if (size > JS_CAGE_SMALL_MAX && k < head->run_slabs) {
                JSCageSlab *tail = &chunk->slabs[head->index + k];
                uint32_t drop = head->run_slabs - k;
                head->run_slabs = (uint16_t)k;
                chunk->free_slabs |= (uint16_t)(((1u << drop) - 1) << tail->index);
                for (uint32_t i = 0; i < drop; i++)
                    chunk->slabs[tail->index + i].kind = JS_SLAB_FREE;
                return ptr;
            }
            if (size > JS_CAGE_SMALL_MAX)
                return ptr;
        } else {
            if (size > JS_CAGE_CHUNK_SIZE)
                return ptr;
        }
    }
    void *fresh = js_cage_heap_alloc(heap, size);
    if (!fresh)
        return NULL;
    memcpy(fresh, ptr, old < size ? old : size);
    js_cage_free(ptr);
    return fresh;
}

JSCageHeap *js_cage_heap_new(void)
{
    if (!js_cage_init())
        return NULL;
    return calloc(1, sizeof(JSCageHeap));
}

void js_cage_heap_destroy(JSCageHeap *heap)
{
    if (!heap)
        return;
    while (heap->chunks)
        js_cage_release_chunks(heap->chunks);
    free(heap);
}

static void *js_cage_fallback_malloc(size_t size)
{
    size_t *raw = malloc(sizeof(size_t) + (size ? size : 1));
    if (!raw)
        return NULL;
    raw[0] = size;
    return raw + 1;
}

void *js_cage_shared_malloc(size_t size)
{
    if (!js_cage_init())
        return js_cage_fallback_malloc(size);
    js_mutex_lock(&g_cage.shared_lock);
    if (!g_cage.shared_heap)
        g_cage.shared_heap = calloc(1, sizeof(JSCageHeap));
    void *p = g_cage.shared_heap ? js_cage_heap_alloc(g_cage.shared_heap, size) : NULL;
    js_mutex_unlock(&g_cage.shared_lock);
    return p;
}

void *js_cage_shared_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return js_cage_shared_malloc(size);
    if (!js_cage_contains(ptr)) {
        size_t *raw = realloc((size_t *)ptr - 1, sizeof(size_t) + (size ? size : 1));
        if (!raw)
            return NULL;
        raw[0] = size;
        return raw + 1;
    }
    js_mutex_lock(&g_cage.shared_lock);
    void *p = js_cage_heap_realloc(g_cage.shared_heap, ptr, size);
    js_mutex_unlock(&g_cage.shared_lock);
    return p;
}

void js_cage_shared_free(void *ptr)
{
    if (!ptr)
        return;
    if (!js_cage_contains(ptr)) {
        free((size_t *)ptr - 1);
        return;
    }
    js_mutex_lock(&g_cage.shared_lock);
    js_cage_free(ptr);
    js_mutex_unlock(&g_cage.shared_lock);
}

size_t js_cage_shared_usable_size(const void *ptr)
{
    if (!ptr)
        return 0;
    if (!js_cage_contains(ptr))
        return ((const size_t *)ptr)[-1];
    js_mutex_lock(&g_cage.shared_lock);
    size_t n = js_cage_usable_size(ptr);
    js_mutex_unlock(&g_cage.shared_lock);
    return n;
}
