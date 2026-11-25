// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#ifndef WWMI_COMPUTE_API
#define WWMI_COMPUTE_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

struct WWMI_ComputeStateInfo_v1;

enum WWMI_GraphicsRestoreLevel_v1 {
    WWMI_GRAPHICS_RESTORE_UNKNOWN = 0,
    WWMI_GRAPHICS_RESTORE_EMPTY = 1,
    WWMI_GRAPHICS_RESTORE_TABLES = 2
};
typedef struct WWMI_GraphicsRestoreInfo_v1 {
    uint32_t size, version, level, record_count;
    uint32_t outside_render_pass, reserved;
    uint64_t native_list, cbv_srv_uav_heap, sampler_heap, root_signature;
} WWMI_GraphicsRestoreInfo_v1;
typedef struct WWMI_GraphicsRestoreRecord_v1 {
    uint32_t parameter, reserved;
    uint64_t table;
} WWMI_GraphicsRestoreRecord_v1;
// Same recording-thread and pre-mutation scope as CaptureRestore. No PSO or
// Query required. EMPTY means a known null graphics root; TABLES may contain
// zero records. Success requires a known heap pair and an observed scope
// outside a render pass. Compare the borrowed native list and both heaps
// with the same callback's compute snapshot. Replay after restoring those
// heaps, without changing the graphics root or non-table arguments.
// Raw native mutations must be absent or fully restored before capture.
// Set out->size; 64 records suffice. Statuses as in CaptureRestore.
WWMI_COMPUTE_API int32_t WWMI_GraphicsState_CaptureTables_v1(
    uint64_t api_list, WWMI_GraphicsRestoreInfo_v1 *out,
    WWMI_GraphicsRestoreRecord_v1 *records, uint32_t record_capacity);

enum WWMI_StructuredSlotKind_v1 {
    WWMI_STRUCTURED_SLOT_UNKNOWN = 0,
    WWMI_STRUCTURED_SLOT_SRV = 1,
    WWMI_STRUCTURED_SLOT_UAV = 2
};
typedef struct WWMI_StructuredTableSlot_v1 {
    uint32_t size, version, parameter, table_offset;
    uint32_t kind, heap_slot, source_slot, reserved;
    uint64_t table, heap_generation, write, source_heap_generation, source_write;
    uint64_t resource, resource_creation, counter, counter_creation;
    uint64_t first_element, counter_offset_bytes;
    uint32_t elements, stride, format, shader_mapping, flags, reserved2;
} WWMI_StructuredTableSlot_v1;
// Same callback, Query and physical-offset contract as ReadAsTableSlot.
// Success gives the original native BUFFER description of an observed
// stride-32 structured SRV or stride-4 structured UAV at the bound slot.
// Unused fields are zero. Resource/counter are borrowed, not retained.
// Use in the original call's live resource scope; the consumer owns any
// references needed after recording. No game CPU descriptor is returned.
// This is a CPU snapshot, not a receipt for a later GPU read: the consumer
// must account for any actual descriptor rewrites before that read.
WWMI_COMPUTE_API int32_t WWMI_ComputeState_ReadStructuredTableSlot_v1(
    uint64_t list, const struct WWMI_ComputeStateInfo_v1 *expected, uint32_t parameter,
    uint32_t table_offset, WWMI_StructuredTableSlot_v1 *out);

#ifdef __cplusplus
}
static_assert(sizeof(WWMI_GraphicsRestoreInfo_v1) == 56, "C ABI size");
static_assert(sizeof(WWMI_GraphicsRestoreRecord_v1) == 16, "C ABI size");
static_assert(sizeof(WWMI_StructuredTableSlot_v1) == 144, "C ABI size");
#endif
