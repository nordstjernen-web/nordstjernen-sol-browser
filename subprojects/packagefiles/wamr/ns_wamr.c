/* Nordstjernen — narrow accessors into WAMR internals for the WebAssembly JS API. */

#include "ns_wamr.h"

#include "wasm_runtime_common.h"
#include "../interpreter/wasm_runtime.h"

static WASMTableInstance *
ns_wamr_find_export_table(wasm_module_inst_t inst, const char *export_name,
                          uint32 *out_table_idx)
{
    WASMModuleInstance *module_inst = (WASMModuleInstance *)inst;
    WASMModule *module;
    uint32 i;

    if (!module_inst || module_inst->module_type != Wasm_Module_Bytecode)
        return NULL;
    if (!export_name)
        return NULL;

    module = module_inst->module;
    for (i = 0; i < module->export_count; i++) {
        WASMExport *export = module->exports + i;
        if (export->kind == EXPORT_KIND_TABLE
            && !strcmp(export->name, export_name)) {
            if (out_table_idx)
                *out_table_idx = export->index;
            return wasm_get_table_inst(module_inst, export->index);
        }
    }
    return NULL;
}

bool
ns_wamr_table_size(wasm_module_inst_t inst, const char *export_name,
                   uint32_t *out_size, uint32_t *out_max)
{
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         NULL);
    if (!table)
        return false;
    if (out_size)
        *out_size = table->cur_size;
    if (out_max)
        *out_max = table->max_size;
    return true;
}

bool
ns_wamr_table_grow(wasm_module_inst_t inst, const char *export_name,
                   uint32_t delta, uint32_t *out_old_size)
{
    uint32 table_idx = 0;
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         &table_idx);
    if (!table)
        return false;
    if (out_old_size)
        *out_old_size = table->cur_size;
    return wasm_enlarge_table((WASMModuleInstance *)inst, table_idx, delta,
                              NULL_REF);
}

bool
ns_wamr_table_get_ref(wasm_module_inst_t inst, const char *export_name,
                      uint32_t idx, uint32_t *out_ref)
{
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         NULL);
    if (!table || idx >= table->cur_size)
        return false;
    *out_ref = (uint32)table->elems[idx];
    return true;
}

bool
ns_wamr_table_set_ref(wasm_module_inst_t inst, const char *export_name,
                      uint32_t idx, uint32_t ref)
{
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         NULL);
    if (!table || idx >= table->cur_size)
        return false;
    table->elems[idx] = ref;
    return true;
}

void
ns_wamr_externref_reclaim(wasm_module_inst_t inst)
{
    if (inst)
        wasm_externref_reclaim(inst);
}

extern bool ns_wamr_host_calls_ctors;

void
ns_wamr_set_host_calls_ctors(bool enabled)
{
    ns_wamr_host_calls_ctors = enabled;
}

void
wasm_trap_delete(void *trap)
{
    (void)trap;
}

#if WASM_ENABLE_SHARED_MEMORY != 0
#include "wasm_shared_memory.h"
#include "wasm_memory.h"
#include "ns_wamr_internal.h"

static os_thread_local_attribute WASMMemoryInstance *ns_wamr_adopted;
static korp_mutex ns_wamr_standalone_lock;
static bool ns_wamr_standalone_lock_ready;
static HashMap *ns_wamr_standalone;
static void (*ns_wamr_destroy_cb)(void *memory_data);

static uint32
ns_wamr_ptr_hash(const void *key)
{
    return (uint32)((uintptr_t)key >> 4);
}

static bool
ns_wamr_ptr_equal(void *a, void *b)
{
    return a == b;
}

static void
ns_wamr_standalone_init(void)
{
    if (ns_wamr_standalone_lock_ready)
        return;
    os_mutex_init(&ns_wamr_standalone_lock);
    ns_wamr_standalone = bh_hash_map_create(16, true, ns_wamr_ptr_hash,
                                            ns_wamr_ptr_equal, NULL, NULL);
    ns_wamr_standalone_lock_ready = true;
}

void
ns_wamr_set_shared_memory_destroy_callback(void (*cb)(void *memory_data))
{
    ns_wamr_standalone_init();
    ns_wamr_destroy_cb = cb;
}

wasm_memory_inst_t
ns_wamr_shared_memory_new(uint32_t init_pages, uint32_t max_pages)
{
    WASMMemoryInstance *memory;
    uint64 data_size = 0;

    if (max_pages < init_pages || max_pages > DEFAULT_MAX_PAGES)
        return NULL;
    ns_wamr_standalone_init();
    if (!(memory = wasm_runtime_malloc(sizeof(WASMMemoryInstance))))
        return NULL;
    memset(memory, 0, sizeof(WASMMemoryInstance));
    if (wasm_allocate_linear_memory(&memory->memory_data, true, false,
                                    DEFAULT_NUM_BYTES_PER_PAGE, init_pages,
                                    max_pages, &data_size)
        != BHT_OK) {
        wasm_runtime_free(memory);
        return NULL;
    }
    memory->module_type = Wasm_Module_Bytecode;
    memory->num_bytes_per_page = DEFAULT_NUM_BYTES_PER_PAGE;
    memory->cur_page_count = init_pages;
    memory->max_page_count = max_pages;
    memory->memory_data_size = data_size;
    memory->memory_data_end = memory->memory_data + data_size;
    memory->heap_data = memory->memory_data;
    memory->heap_data_end = memory->memory_data;
    wasm_runtime_set_mem_bound_check_bytes(memory, data_size);
    memory->is_shared_memory = 1;
    memory->ref_count = 1;
    os_mutex_lock(&ns_wamr_standalone_lock);
    bh_hash_map_insert(ns_wamr_standalone, memory, memory);
    os_mutex_unlock(&ns_wamr_standalone_lock);
    return (wasm_memory_inst_t)memory;
}

bool
ns_wamr_shared_memory_destroy_standalone(WASMMemoryInstance *memory)
{
    void *value = NULL;
    bool found;

    if (!ns_wamr_standalone_lock_ready)
        return false;
    os_mutex_lock(&ns_wamr_standalone_lock);
    found = bh_hash_map_remove(ns_wamr_standalone, memory, NULL, &value);
    os_mutex_unlock(&ns_wamr_standalone_lock);
    if (!found)
        return false;
    if (ns_wamr_destroy_cb)
        ns_wamr_destroy_cb(memory->memory_data);
    if (memory->memory_data)
        wasm_deallocate_linear_memory(memory);
    wasm_runtime_free(memory);
    return true;
}

void
ns_wamr_shared_memory_dup(wasm_memory_inst_t memory)
{
    shared_memory_inc_reference((WASMMemoryInstance *)memory);
}

void
ns_wamr_shared_memory_release(wasm_memory_inst_t memory)
{
    WASMMemoryInstance *m = (WASMMemoryInstance *)memory;
    if (shared_memory_dec_reference(m) == 0)
        ns_wamr_shared_memory_destroy_standalone(m);
}

bool
ns_wamr_memory_is_shared(wasm_memory_inst_t memory)
{
    return memory && shared_memory_is_shared((WASMMemoryInstance *)memory);
}

uint32_t
ns_wamr_memory_max_pages(wasm_memory_inst_t memory)
{
    return memory ? ((WASMMemoryInstance *)memory)->max_page_count : 0;
}

void
ns_wamr_set_adopted_memory(wasm_memory_inst_t memory)
{
    ns_wamr_adopted = (WASMMemoryInstance *)memory;
}

WASMMemoryInstance *
ns_wamr_take_adopted_memory(void)
{
    WASMMemoryInstance *memory = ns_wamr_adopted;
    ns_wamr_adopted = NULL;
    return memory;
}
#endif
