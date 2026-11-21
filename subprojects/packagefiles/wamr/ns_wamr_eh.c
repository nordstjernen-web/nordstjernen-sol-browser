/* Nordstjernen — WAMR accessors for wasm exception tags and in-flight exceptions. */

#include "ns_wamr_eh.h"

#include "wasm_runtime_common.h"
#include "../interpreter/wasm_runtime.h"

static WASMModule *
ns_wamr_eh_module(wasm_module_t module)
{
    WASMModule *m = (WASMModule *)module;
    return m && m->module_type == Wasm_Module_Bytecode ? m : NULL;
}

static WASMModuleInstance *
ns_wamr_eh_instance(wasm_module_inst_t inst)
{
    WASMModuleInstance *m = (WASMModuleInstance *)inst;
    return m && m->module_type == Wasm_Module_Bytecode ? m : NULL;
}

static WASMFuncType *
ns_wamr_eh_tag_type(WASMModule *m, uint32 tag_index)
{
    if (!m || tag_index >= m->import_tag_count + m->tag_count)
        return NULL;
    if (tag_index < m->import_tag_count)
        return m->import_tags[tag_index].u.tag.tag_type;
    return m->tags[tag_index - m->import_tag_count]->tag_type;
}

uint32_t
ns_wamr_tag_count(wasm_module_t module)
{
    WASMModule *m = ns_wamr_eh_module(module);
    return m ? m->import_tag_count + m->tag_count : 0;
}

uint32_t
ns_wamr_import_tag_count(wasm_module_t module)
{
    WASMModule *m = ns_wamr_eh_module(module);
    return m ? m->import_tag_count : 0;
}

bool
ns_wamr_import_tag_name(wasm_module_t module, uint32_t tag_index,
                        const char **out_module, const char **out_name)
{
    WASMModule *m = ns_wamr_eh_module(module);
    if (!m || tag_index >= m->import_tag_count)
        return false;
    *out_module = m->import_tags[tag_index].u.tag.module_name;
    *out_name = m->import_tags[tag_index].u.tag.field_name;
    return true;
}

uint32_t
ns_wamr_tag_param_count(wasm_module_t module, uint32_t tag_index)
{
    WASMFuncType *type =
        ns_wamr_eh_tag_type(ns_wamr_eh_module(module), tag_index);
    return type ? type->param_count : 0;
}

wasm_valkind_t
ns_wamr_tag_param_kind(wasm_module_t module, uint32_t tag_index,
                       uint32_t param)
{
    WASMFuncType *type =
        ns_wamr_eh_tag_type(ns_wamr_eh_module(module), tag_index);
    if (!type || param >= type->param_count)
        return WASM_I32;
    switch (type->types[param]) {
        case VALUE_TYPE_I64:
            return WASM_I64;
        case VALUE_TYPE_F32:
            return WASM_F32;
        case VALUE_TYPE_F64:
            return WASM_F64;
        case VALUE_TYPE_V128:
            return WASM_V128;
        case VALUE_TYPE_FUNCREF:
            return WASM_FUNCREF;
        case VALUE_TYPE_EXTERNREF:
            return WASM_EXTERNREF;
        default:
            return WASM_I32;
    }
}

bool
ns_wamr_export_tag_index(wasm_module_t module, int32_t export_index,
                         uint32_t *out_tag_index)
{
    WASMModule *m = ns_wamr_eh_module(module);
    if (!m || export_index < 0 || (uint32)export_index >= m->export_count
        || m->exports[export_index].kind != EXPORT_KIND_TAG)
        return false;
    *out_tag_index = m->exports[export_index].index;
    return true;
}

void *
ns_wamr_tag_identity(wasm_module_inst_t inst, uint32_t tag_index)
{
    WASMModuleInstance *m = ns_wamr_eh_instance(inst);
    if (!m || tag_index >= m->e->tag_count)
        return NULL;
    return wasm_exn_tag_identity(m, tag_index);
}

bool
ns_wamr_tag_link_import(wasm_module_inst_t inst, uint32_t tag_index,
                        void *identity)
{
    WASMModuleInstance *m = ns_wamr_eh_instance(inst);
    if (!m || tag_index >= m->module->import_tag_count
        || !m->e->import_tag_ptrs)
        return false;
    m->e->import_tag_ptrs[tag_index] = identity;
    return true;
}

static bool
ns_wamr_exn_marked(WASMModuleInstance *m)
{
    const char *exception = wasm_get_exception(m);
    return m->e->exn_tag && exception
           && !strcmp(exception, "Exception: " WASM_EXN_UNCAUGHT);
}

bool
ns_wamr_exn_pending(wasm_module_inst_t inst, void **out_tag,
                    const uint32_t **out_cells, uint32_t *out_cell_num)
{
    WASMModuleInstance *m = ns_wamr_eh_instance(inst);
    if (!m || !ns_wamr_exn_marked(m))
        return false;
    *out_tag = m->e->exn_tag;
    *out_cells = m->e->exn_cells;
    *out_cell_num = m->e->exn_cell_num;
    return true;
}

void
ns_wamr_exn_clear(wasm_module_inst_t inst)
{
    WASMModuleInstance *m = ns_wamr_eh_instance(inst);
    if (!m)
        return;
    if (ns_wamr_exn_marked(m))
        wasm_set_exception(m, NULL);
    m->e->exn_tag = NULL;
    m->e->exn_cell_num = 0;
}

bool
ns_wamr_exn_raise(wasm_module_inst_t inst, void *tag, const uint32_t *cells,
                  uint32_t cell_num)
{
    WASMModuleInstance *m = ns_wamr_eh_instance(inst);
    if (!m || !tag || cell_num > WASM_EXN_MAX_CELLS)
        return false;
    if (cell_num)
        memcpy(m->e->exn_cells, cells, sizeof(uint32) * cell_num);
    m->e->exn_tag = tag;
    m->e->exn_cell_num = cell_num;
    wasm_set_exception(m, WASM_EXN_UNCAUGHT);
    return true;
}
