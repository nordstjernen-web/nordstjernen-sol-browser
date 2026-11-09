/* QuickJS heap hardening: cage masking, external and code pointer tables, trusted context table. */

#ifndef QUICKJS_HARDENING_H
#define QUICKJS_HARDENING_H

static uintptr_t js_heap_cage_base;
static uintptr_t js_heap_cage_mask = ~(uintptr_t)0;
static bool js_heap_cage_active;

void JS_SetHeapCage(uintptr_t base, size_t size)
{
    js_heap_cage_base = base;
    js_heap_cage_mask = size - 1;
    js_heap_cage_active = true;
}

bool JS_CageContains(const void *ptr)
{
    return js_cage_contains(ptr);
}

uintptr_t JS_CageBase(void)
{
    return js_cage_base();
}

size_t JS_CageSize(void)
{
    return js_cage_size();
}

size_t JS_CageCommittedBytes(void)
{
    return js_cage_committed_bytes();
}

void *JS_CageMalloc(size_t size)
{
    return js_cage_shared_malloc(size);
}

void *JS_CageRealloc(void *ptr, size_t size)
{
    return js_cage_shared_realloc(ptr, size);
}

void JS_CageFree(void *ptr)
{
    js_cage_shared_free(ptr);
}

size_t JS_CageUsableSize(const void *ptr)
{
    return js_cage_shared_usable_size(ptr);
}

static inline void *js_caged_ptr(const void *ptr)
{
    return (void *)(js_heap_cage_base | ((uintptr_t)ptr & js_heap_cage_mask));
}

static inline bool js_in_heap_cage(const void *ptr)
{
    return ((uintptr_t)ptr & ~js_heap_cage_mask) == js_heap_cage_base;
}


#define js_abuf_data(abuf) ((uint8_t *)js_caged_ptr((abuf)->data))
#define js_abuf_owned_data(abuf) ((abuf)->data ? js_abuf_data(abuf) : NULL)
#define js_ta_ptr(p, member) \
    ((__typeof__((p)->u.array.u.member))js_caged_ptr((p)->u.array.u.member))

typedef void (*JSCodePtr)(void);

#define JS_CPT_SEG_SHIFT 12
#define JS_CPT_SEG_SIZE (1u << JS_CPT_SEG_SHIFT)
#define JS_CPT_SEGS 256u
#define JS_EPT_SEG_SHIFT 16
#define JS_EPT_SEG_SIZE (1u << JS_EPT_SEG_SHIFT)
#define JS_EPT_SEGS 256u

typedef struct JSExternalPointerEntry {
    void *ptr;
    uint32_t tag;
    uint32_t next_free;
} JSExternalPointerEntry;

typedef struct JSInternEntry {
    uintptr_t value;
    uint32_t kind;
    int32_t magic;
    uint16_t length;
    uint8_t brand;
    uint8_t brand_mode;
} JSInternEntry;

typedef struct JSInternTable {
    JSInternEntry *_Atomic segments[JS_CPT_SEGS];
    uint32_t count;
    uint32_t *hash;
    uint32_t hash_size;
} JSInternTable;

static _Atomic int js_ptr_tables_lock_word;
static JSInternTable js_code_table = { .count = 1 };
static JSInternTable js_func_list_table = { .count = 1 };
static JSExternalPointerEntry *_Atomic js_ept_segments[JS_EPT_SEGS];
static uint32_t js_ept_count = 1;
static uint32_t js_ept_free_head;

static void js_ptr_tables_lock(void)
{
    while (atomic_exchange(&js_ptr_tables_lock_word, 1))
        continue;
}

static void js_ptr_tables_unlock(void)
{
    atomic_store(&js_ptr_tables_lock_word, 0);
}

static inline const JSInternEntry *js_intern_entry(JSInternTable *t, uint32_t idx)
{
    idx &= JS_CPT_SEGS * JS_CPT_SEG_SIZE - 1;
    JSInternEntry *seg = atomic_load(&t->segments[idx >> JS_CPT_SEG_SHIFT]);
    return seg ? &seg[idx & (JS_CPT_SEG_SIZE - 1)] : NULL;
}

static inline uintptr_t js_intern_get(JSInternTable *t, uint32_t idx)
{
    const JSInternEntry *e = js_intern_entry(t, idx);
    return e ? e->value : 0;
}

static uint32_t js_intern_hash_slot(const JSInternEntry *k, uint32_t size)
{
    uint64_t h = (uint64_t)k->value ^ ((uint64_t)k->kind << 56) ^
                 ((uint64_t)(uint32_t)k->magic << 24) ^ ((uint64_t)k->length << 8) ^
                 ((uint64_t)k->brand << 48) ^ ((uint64_t)k->brand_mode << 44);
    h *= 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 32) & (size - 1);
}

static bool js_intern_same(const JSInternEntry *a, const JSInternEntry *b)
{
    return a->value == b->value && a->kind == b->kind && a->magic == b->magic &&
           a->length == b->length && a->brand == b->brand && a->brand_mode == b->brand_mode;
}

static void js_intern_rehash(JSInternTable *t, uint32_t new_size)
{
    uint32_t *table = calloc(new_size, sizeof(*table));
    if (!table)
        abort();
    for (uint32_t idx = 1; idx < t->count; idx++) {
        const JSInternEntry *e = js_intern_entry(t, idx);
        uint32_t slot = js_intern_hash_slot(e, new_size);
        while (table[slot])
            slot = (slot + 1) & (new_size - 1);
        table[slot] = idx;
    }
    free(t->hash);
    t->hash = table;
    t->hash_size = new_size;
}

static uint32_t js_intern_add(JSInternTable *t, const JSInternEntry *key)
{
    if (!key->value)
        return 0;
    js_ptr_tables_lock();
    if (t->count * 2 > t->hash_size)
        js_intern_rehash(t, t->hash_size ? t->hash_size * 2 : 4096);
    uint32_t slot = js_intern_hash_slot(key, t->hash_size);
    for (;;) {
        uint32_t idx = t->hash[slot];
        if (!idx)
            break;
        const JSInternEntry *e = js_intern_entry(t, idx);
        if (js_intern_same(e, key)) {
            js_ptr_tables_unlock();
            return idx;
        }
        slot = (slot + 1) & (t->hash_size - 1);
    }
    uint32_t idx = t->count;
    if (idx >= JS_CPT_SEGS * JS_CPT_SEG_SIZE)
        abort();
    uint32_t seg = idx >> JS_CPT_SEG_SHIFT;
    JSInternEntry *entries = atomic_load(&t->segments[seg]);
    if (!entries) {
        entries = calloc(JS_CPT_SEG_SIZE, sizeof(*entries));
        if (!entries)
            abort();
        atomic_store(&t->segments[seg], entries);
    }
    entries[idx & (JS_CPT_SEG_SIZE - 1)] = *key;
    t->count++;
    t->hash[slot] = idx;
    js_ptr_tables_unlock();
    return idx;
}

enum {
    JS_CPT_KIND_NONE,
    JS_CPT_KIND_JOB,
    JS_CPT_KIND_CFUNC_DATA,
    JS_CPT_KIND_CCLOSURE,
    JS_CPT_KIND_CCLOSURE_FINALIZER,
    JS_CPT_KIND_MODULE_INIT,
    JS_CPT_KIND_ABUF_REALLOC,
    JS_CPT_KIND_CFUNC,
};

#define JS_CPT_KIND_CFUNC_PROTO(cproto) (JS_CPT_KIND_CFUNC + (uint32_t)(cproto))

static no_inline __attribute__((noreturn)) void js_cpt_kind_mismatch(uint32_t idx, uint32_t kind)
{
    fprintf(stderr, "quickjs heap cage: code pointer %u is not registered for call kind %u\n", idx, kind);
    abort();
}

static inline JSCodePtr js_cpt_get(uint32_t idx)
{
    return (JSCodePtr)js_intern_get(&js_code_table, idx);
}

static inline const JSInternEntry *js_cpt_entry(uint32_t idx, uint32_t kind)
{
    const JSInternEntry *e = js_intern_entry(&js_code_table, idx);
    if (!e || !e->value)
        return NULL;
    if (unlikely(e->kind != kind))
        js_cpt_kind_mismatch(idx, kind);
    return e;
}

static inline const JSInternEntry *js_cpt_call_entry(uint32_t idx, uint32_t kind)
{
    const JSInternEntry *e = js_cpt_entry(idx, kind);
    if (unlikely(!e))
        js_cpt_kind_mismatch(idx, kind);
    return e;
}

static inline JSCodePtr js_cpt_fn(uint32_t idx, uint32_t kind)
{
    const JSInternEntry *e = js_cpt_entry(idx, kind);
    return e ? (JSCodePtr)e->value : NULL;
}

static uint32_t js_cpt_intern_entry(const JSInternEntry *key)
{
    return js_intern_add(&js_code_table, key);
}

static uint32_t js_cpt_intern(JSCodePtr fn, uint32_t kind)
{
    JSInternEntry key = { .value = (uintptr_t)fn, .kind = kind };
    return js_cpt_intern_entry(&key);
}

static inline const JSCFunctionListEntry *js_flt_get(uint32_t idx)
{
    return (const JSCFunctionListEntry *)js_intern_get(&js_func_list_table, idx);
}

static uint32_t js_flt_intern(const JSCFunctionListEntry *e)
{
    JSInternEntry key = { .value = (uintptr_t)e, .kind = JS_CPT_KIND_NONE };
    return js_intern_add(&js_func_list_table, &key);
}

static inline JSExternalPointerEntry *js_ept_entry(uint32_t idx)
{
    idx &= JS_EPT_SEGS * JS_EPT_SEG_SIZE - 1;
    JSExternalPointerEntry *seg = atomic_load(&js_ept_segments[idx >> JS_EPT_SEG_SHIFT]);
    return seg ? &seg[idx & (JS_EPT_SEG_SIZE - 1)] : NULL;
}

static uint32_t js_ept_alloc(void *ptr, uint32_t tag)
{
    js_ptr_tables_lock();
    uint32_t idx = js_ept_free_head;
    JSExternalPointerEntry *e;
    if (idx) {
        e = js_ept_entry(idx);
        js_ept_free_head = e->next_free;
    } else {
        idx = js_ept_count;
        if (idx >= JS_EPT_SEGS * JS_EPT_SEG_SIZE) {
            js_ptr_tables_unlock();
            return 0;
        }
        uint32_t seg = idx >> JS_EPT_SEG_SHIFT;
        JSExternalPointerEntry *entries = atomic_load(&js_ept_segments[seg]);
        if (!entries) {
            entries = calloc(JS_EPT_SEG_SIZE, sizeof(*entries));
            if (!entries) {
                js_ptr_tables_unlock();
                return 0;
            }
            atomic_store(&js_ept_segments[seg], entries);
        }
        js_ept_count++;
        e = &entries[idx & (JS_EPT_SEG_SIZE - 1)];
    }
    e->ptr = ptr;
    e->tag = tag;
    e->next_free = 0;
    js_ptr_tables_unlock();
    return idx;
}

static void js_ept_release(uint32_t idx)
{
    JSExternalPointerEntry *e = js_ept_entry(idx);
    if (!e)
        return;
    js_ptr_tables_lock();
    if (e->tag) {
        e->ptr = NULL;
        e->tag = 0;
        e->next_free = js_ept_free_head;
        js_ept_free_head = idx;
    }
    js_ptr_tables_unlock();
}

static void *js_ept_get(const void *slot, uint32_t tag)
{
    uint32_t idx = (uint32_t)(uintptr_t)slot;
    if (!idx)
        return NULL;
    JSExternalPointerEntry *e = js_ept_entry(idx);
    if (!e || e->tag != tag)
        return NULL;
    return e->ptr;
}

#define js_abuf_realloc_func(abuf) \
    ((JSReallocArrayBufferDataFunc *)js_cpt_fn((abuf)->realloc_func, JS_CPT_KIND_ABUF_REALLOC))

#define JS_EPT_TAG_ABUF_OPAQUE 0xfffffffeu
#define JS_EPT_TAG_CCLOSURE_OPAQUE 0xfffffffdu
#define JS_EPT_TAG_INTL_OPAQUE 0xfffffffcu

static inline bool js_class_is_intl(JSClassID class_id)
{
    return class_id >= JS_CLASS_INTL_LOCALE && class_id <= JS_CLASS_INTL_SEGMENT_ITERATOR;
}

static uint32_t js_ept_store(void *ptr, uint32_t tag)
{
    return ptr ? js_ept_alloc(ptr, tag) : 0;
}

static void *js_ept_load(uint32_t idx, uint32_t tag)
{
    return js_ept_get((const void *)(uintptr_t)idx, tag);
}

static void js_ept_clear(uint32_t *slot)
{
    if (*slot) {
        js_ept_release(*slot);
        *slot = 0;
    }
}

#define js_abuf_opaque(abuf) js_ept_load((abuf)->opaque, JS_EPT_TAG_ABUF_OPAQUE)

#define JS_CTX_TABLE_SIZE 65536u

static JSContext *_Atomic js_ctx_table[JS_CTX_TABLE_SIZE];
static uint32_t js_ctx_table_hint = 1;

static uint32_t js_ctx_table_add(JSContext *ctx)
{
    js_ptr_tables_lock();
    for (uint32_t n = 0; n < JS_CTX_TABLE_SIZE; n++) {
        uint32_t idx = (js_ctx_table_hint + n) & (JS_CTX_TABLE_SIZE - 1);
        if (idx && !atomic_load(&js_ctx_table[idx])) {
            atomic_store(&js_ctx_table[idx], ctx);
            js_ctx_table_hint = idx + 1;
            js_ptr_tables_unlock();
            return idx;
        }
    }
    js_ptr_tables_unlock();
    return 0;
}

static void js_ctx_table_remove(uint32_t idx)
{
    if (idx)
        atomic_store(&js_ctx_table[idx & (JS_CTX_TABLE_SIZE - 1)], NULL);
}

static no_inline __attribute__((noreturn)) void js_ctx_foreign(uint32_t idx)
{
    fprintf(stderr, "quickjs heap cage: context %u belongs to another runtime\n", idx);
    abort();
}

static inline JSContext *js_ctx_get(JSRuntime *rt, uint32_t idx)
{
    JSContext *ctx = atomic_load(&js_ctx_table[idx & (JS_CTX_TABLE_SIZE - 1)]);
    if (ctx && unlikely(ctx->rt != rt))
        js_ctx_foreign(idx);
    return ctx;
}

static inline uint32_t js_ctx_ref(JSContext *ctx)
{
    return ctx ? ctx->table_index : 0;
}

#endif
