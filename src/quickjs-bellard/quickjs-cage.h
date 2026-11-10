/* QuickJS heap cage: one reserved address range per process that holds every GC allocation. */

#ifndef QUICKJS_CAGE_H
#define QUICKJS_CAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct JSCageHeap JSCageHeap;

extern uintptr_t js_cage_pointer_base;

bool        js_cage_init(void);
bool        js_cage_ready(void);
uintptr_t   js_cage_base(void);
size_t      js_cage_size(void);
bool        js_cage_contains(const void *ptr);
size_t      js_cage_committed_bytes(void);

JSCageHeap *js_cage_heap_new(void);
void        js_cage_heap_destroy(JSCageHeap *heap);
void       *js_cage_heap_alloc(JSCageHeap *heap, size_t size);
void       *js_cage_heap_realloc(JSCageHeap *heap, void *ptr, size_t size);
void        js_cage_free(void *ptr);
size_t      js_cage_usable_size(const void *ptr);

void       *js_cage_shared_malloc(size_t size);
void       *js_cage_shared_realloc(void *ptr, size_t size);
void        js_cage_shared_free(void *ptr);
size_t      js_cage_shared_usable_size(const void *ptr);

#endif
