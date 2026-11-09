/* QuickJS heap hardening: compressed pointer fields (cage offsets) and the fail-closed encoder. */

#ifndef QUICKJS_CPTR_H
#define QUICKJS_CPTR_H

static uintptr_t js_heap_cage_base;
static uintptr_t js_heap_cage_mask;

#if JS_CAGE_BOXING
#define JS_CPTR(T) uint32_t
static inline void *js_cptr_decode(uint32_t off)
{
    return off ? (void *)(js_heap_cage_base + off) : NULL;
}
static no_inline __attribute__((noreturn)) void js_cptr_encode_outside_cage(const void *ptr)
{
    fprintf(stderr, "quickjs heap cage: refusing to compress the pointer %p, which lies outside the cage\n", ptr);
    abort();
}
static inline uint32_t js_cptr_encode(const void *ptr)
{
    if (!ptr)
        return 0;
    if (unlikely(((uintptr_t)ptr & ~js_heap_cage_mask) != js_heap_cage_base))
        js_cptr_encode_outside_cage(ptr);
    return (uint32_t)((uintptr_t)ptr - js_heap_cage_base);
}
#define js_cload(T, field) ((T)js_cptr_decode(field))
#define js_cstore(field, ptr) ((field) = js_cptr_encode(ptr))
#else
#define JS_CPTR(T) T
#define js_cload(T, field) (field)
#define js_cstore(field, ptr) ((field) = (ptr))
#endif

#endif
