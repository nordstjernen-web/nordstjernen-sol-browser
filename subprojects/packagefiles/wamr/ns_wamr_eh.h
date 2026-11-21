/* Nordstjernen — WAMR accessors for wasm exception tags and in-flight exceptions. */

#ifndef NS_WAMR_EH_H
#define NS_WAMR_EH_H

#include <stdbool.h>
#include <stdint.h>

#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_WAMR_EXN_MAX_CELLS 64

uint32_t ns_wamr_tag_count(wasm_module_t module);
uint32_t ns_wamr_import_tag_count(wasm_module_t module);
bool ns_wamr_import_tag_name(wasm_module_t module, uint32_t tag_index,
                             const char **out_module, const char **out_name);
uint32_t ns_wamr_tag_param_count(wasm_module_t module, uint32_t tag_index);
wasm_valkind_t ns_wamr_tag_param_kind(wasm_module_t module, uint32_t tag_index,
                                      uint32_t param);
bool ns_wamr_export_tag_index(wasm_module_t module, int32_t export_index,
                              uint32_t *out_tag_index);

void *ns_wamr_tag_identity(wasm_module_inst_t inst, uint32_t tag_index);
bool ns_wamr_tag_link_import(wasm_module_inst_t inst, uint32_t tag_index,
                             void *identity);

bool ns_wamr_exn_pending(wasm_module_inst_t inst, void **out_tag,
                         const uint32_t **out_cells, uint32_t *out_cell_num);
void ns_wamr_exn_clear(wasm_module_inst_t inst);
bool ns_wamr_exn_raise(wasm_module_inst_t inst, void *tag,
                       const uint32_t *cells, uint32_t cell_num);

#ifdef __cplusplus
}
#endif

#endif
