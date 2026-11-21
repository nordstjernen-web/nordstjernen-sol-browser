/* Nordstjernen — hooks the patched WAMR runtime calls back into ns_wamr.c. */

#ifndef NS_WAMR_INTERNAL_H
#define NS_WAMR_INTERNAL_H

#include <stdbool.h>

struct WASMMemoryInstance;

struct WASMMemoryInstance *ns_wamr_take_adopted_memory(void);
bool ns_wamr_shared_memory_destroy_standalone(struct WASMMemoryInstance *memory);

#endif
