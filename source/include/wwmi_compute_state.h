// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#ifndef WWMI_COMPUTE_API
#define WWMI_COMPUTE_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
// All list arguments are live reshade::api::command_list pointers, NOT COM pointers.
// The caller owns list and callback lifetime. Calls occur on its recording thread.
typedef struct WWMI_ComputeStateInfo_v1 {
    uint32_t size, version;
    uint64_t root_signature, pipeline;
    uint32_t ready, pipeline_kind; // kind: 0 ordinary PSO, 1 RT StateObject
    uint64_t epoch;
} WWMI_ComputeStateInfo_v1;
typedef int32_t (*WWMI_ComputeWork_v1)(uint64_t native_list, void *context);
// Actual CPU-recorded value of one root argument. A zero value can be a
// known setter result. UNKNOWN means there is no valid observed value.
enum WWMI_ComputeRootKind_v1 {
    WWMI_COMPUTE_ROOT_UNKNOWN=0,WWMI_COMPUTE_ROOT_TABLE=1,WWMI_COMPUTE_ROOT_CBV=2,
    WWMI_COMPUTE_ROOT_SRV=3,WWMI_COMPUTE_ROOT_UAV=4,WWMI_COMPUTE_ROOT_CONSTANTS=5
};
typedef struct WWMI_ComputeRootArgument_v1 {
    uint32_t size,version,parameter,kind;
    uint64_t epoch,root_signature,pipeline,address;
    uint32_t pipeline_kind,reserved;
} WWMI_ComputeRootArgument_v1;
// Single RT-module owner. Default disabled; repeated same-state calls are idempotent.
WWMI_COMPUTE_API uint64_t WWMI_ComputeState_Enable_v1(uint32_t enabled);
WWMI_COMPUTE_API int32_t WWMI_ComputeState_Query_v1(uint64_t list, WWMI_ComputeStateInfo_v1 *info);
// Call synchronously inside the real consumer callback, using Query_v1's
// result from that list and callback. The observation epoch is not a list
// recording ID. Do not cache the query result across callbacks or resets.
// 0 known kind/value (including address 0), 1 inactive, 2 unobserved or
// query mismatch, 3 bad argument, 4 nested. For TABLE, address is the actual
// GPU table handle; for CONSTANTS it stays 0 and no constant words are read out.
// This is observed binding state. A raw native mutation can bypass observation.
// Does not identify a resource, read a table slot, or call any D3D method.
WWMI_COMPUTE_API int32_t WWMI_ComputeState_ReadRootArgument_v1(
    uint64_t list,const WWMI_ComputeStateInfo_v1 *expected,uint32_t parameter,WWMI_ComputeRootArgument_v1 *out);
typedef struct WWMI_AsTableSlot_v1 {
    uint32_t size,version,parameter,table_offset;
    uint64_t table,location,heap_generation,write;
    // Immutable identity of the typed write that originated the copied value.
    uint64_t source_heap_generation,source_write;
    uint32_t heap_slot,source_slot;
} WWMI_AsTableSlot_v1;
// Same callback and Query contract as ReadRootArgument. table_offset is a
// physical descriptor offset in the caller's proven layout, not a register.
// 0 known typed AS (including Location 0); 2 also covers an unknown slot or
// heap binding. This is a CPU recording-time fact, not a GPU consumption
// receipt or a TLAS identity. The list and relevant heap writes must go through
// observed paths: raw native setters/writes through get_native bypass them.
// Restoring just graphics state after a raw heap switch does not restore these
// compute bindings. Query only after the actual compute bindings are known.
WWMI_COMPUTE_API int32_t WWMI_ComputeState_ReadAsTableSlot_v1(
    uint64_t list,const WWMI_ComputeStateInfo_v1 *expected,uint32_t parameter,uint32_t table_offset,WWMI_AsTableSlot_v1 *out);

enum WWMI_ComputeRestoreLevel_v1 {
    WWMI_COMPUTE_RESTORE_UNKNOWN = 0,
    WWMI_COMPUTE_RESTORE_EMPTY = 1,
    WWMI_COMPUTE_RESTORE_TABLES = 2,
    WWMI_COMPUTE_RESTORE_ROOT_ARGUMENTS = 3
};
typedef struct WWMI_ComputeRestoreInfo_v1 {
    uint32_t size, version, level, record_count;
    uint32_t word_count, reserved;
    uint64_t native_list, cbv_srv_uav_heap, sampler_heap, root_signature;
} WWMI_ComputeRestoreInfo_v1;
typedef struct WWMI_ComputeRestoreRecord_v1 {
    uint32_t kind, parameter, first_word, word_count;
    uint64_t value;
} WWMI_ComputeRestoreRecord_v1;
// Take before the first raw state mutation in the same live recording callback.
// No PSO or prior Query required. Native pointers are borrowed; each zero heap
// is a known absence. EMPTY is a known null compute root, not an unknown root.
// TABLES is usable only when the caller leaves the root and non-table arguments
// unchanged. ROOT_ARGUMENTS includes every originally written argument.
// kind uses WWMI_ComputeRootKind_v1. For CONSTANTS, value indexes words[] and
// first_word/word_count name the actual destination span. For other kinds,
// value is the native binding and the span fields are zero.
// Set out->size. 64 records and 64 words cover the native root-cost limit.
// Returns 0 known, 1 inactive, 2 unknown, 3 bad argument, 4 nested Run,
// 5 insufficient capacity. Nonzero results carry no consumable snapshot.
WWMI_COMPUTE_API int32_t WWMI_ComputeState_CaptureRestore_v1(
    uint64_t api_list, WWMI_ComputeRestoreInfo_v1 *out,
    WWMI_ComputeRestoreRecord_v1 *records, uint32_t record_capacity,
    uint32_t *words, uint32_t word_capacity);

// Work is synchronous and must not change descriptor heaps or graphics state.
// Returns 0 success, 1 inactive, 2 unobserved, 3 bad argument, 4 nested, 5 work failed.
// Work is not called for statuses 1..4. Its compute state is restored even for 5.
WWMI_COMPUTE_API int32_t WWMI_ComputeState_Run_v1(uint64_t list, WWMI_ComputeWork_v1 work, void *context);
#ifdef __cplusplus
}
static_assert(sizeof(WWMI_ComputeStateInfo_v1) == 40, "C ABI size");
static_assert(sizeof(WWMI_ComputeRootArgument_v1) == 56, "C ABI size");
static_assert(sizeof(WWMI_AsTableSlot_v1) == 72, "C ABI size");
static_assert(sizeof(WWMI_ComputeRestoreInfo_v1) == 56, "C ABI size");
static_assert(sizeof(WWMI_ComputeRestoreRecord_v1) == 24, "C ABI size");
#endif

#include "wwmi_compute_occlusion_input.h"
