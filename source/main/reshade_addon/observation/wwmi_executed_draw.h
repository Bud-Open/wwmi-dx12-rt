// SPDX-License-Identifier: GPL-3.0-only
// Original observation ABI. No upstream WWMI/3Dmigoto source is included here.
#pragma once
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
# define WWMI_EXEC_CALL __cdecl
# if defined(WWMI_EXECUTED_DRAW_BUILD)
#  define WWMI_EXEC_EXPORT __declspec(dllexport)
# else
#  define WWMI_EXEC_EXPORT
# endif
#else
# define WWMI_EXEC_CALL
# define WWMI_EXEC_EXPORT
#endif

#define WWMI_EXECUTED_DRAW_ABI_V1 1u
#define WWMI_EXEC_PHASE_BEFORE_NATIVE_CALL 1u
#define WWMI_EXEC_PHASE_AFTER_NATIVE_RETURN 2u
#define WWMI_EXEC_SOURCE_SECTION_DRAW_INDEXED 1u
#define WWMI_EXEC_CLOCK_QPC 1u
#define WWMI_EXEC_CLOCK_STEADY_NS 2u

#define WWMI_EXEC_KNOWN_REQUEST (UINT64_C(1) << 0)
#define WWMI_EXEC_KNOWN_ACTUAL_ARGUMENTS (UINT64_C(1) << 1)
#define WWMI_EXEC_KNOWN_ORIGINAL_ARGUMENTS (UINT64_C(1) << 2)
#define WWMI_EXEC_KNOWN_MOD_INDEX (UINT64_C(1) << 3)
#define WWMI_EXEC_KNOWN_COMPONENT_RULE (UINT64_C(1) << 4)
#define WWMI_EXEC_KNOWN_NATIVE_LIST (UINT64_C(1) << 5)
#define WWMI_EXEC_KNOWN_ORIGINAL_VS_HASH (UINT64_C(1) << 6)
#define WWMI_EXEC_KNOWN_RENDER_TARGET_CONTEXT (UINT64_C(1) << 7)
#define WWMI_EXEC_KNOWN_HOST_LIST_GENERATION_HINT (UINT64_C(1) << 8)

#define WWMI_EXEC_MISSING_FINAL_BINDINGS (UINT64_C(1) << 0)
#define WWMI_EXEC_MISSING_RESOURCE_GENERATIONS (UINT64_C(1) << 1)
#define WWMI_EXEC_MISSING_OBJECT_INSTANCE (UINT64_C(1) << 2)
#define WWMI_EXEC_MISSING_RENDER_EPOCH (UINT64_C(1) << 3)
#define WWMI_EXEC_MISSING_MATERIAL_INTENT (UINT64_C(1) << 4)
#define WWMI_EXEC_MISSING_INI_SOURCE_LOCATION (UINT64_C(1) << 5)
#define WWMI_EXEC_MISSING_MOD_GENERATION (UINT64_C(1) << 6)
#define WWMI_EXEC_MISSING_COMPLETE_SCOPE (UINT64_C(1) << 7)
#define WWMI_EXEC_MISSING_NATIVE_LIST_CREATION (UINT64_C(1) << 8)
#define WWMI_EXEC_MISSING_DEFORMATION_RECEIPT (UINT64_C(1) << 9)
#define WWMI_EXEC_MISSING_ALL_V1 UINT64_C(0x3ff)

#define WWMI_EXEC_VIEW_RAW_CAPTURED 1u
#define WWMI_EXEC_VIEW_RESOURCE_POINTER_KNOWN 2u
#define WWMI_EXEC_VIEW_STRIDE_FORMAT_KNOWN 4u
#define WWMI_EXEC_VIEW_ENTRY_GAME 1u
#define WWMI_EXEC_VIEW_SHIM_LAST_WRITE 2u
#define WWMI_EXEC_VIEW_RELOCATION_HINT 3u

#pragma pack(push, 8)
typedef struct WWMI_ExecutedDrawArguments_v1 {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    int32_t base_vertex;
    uint32_t first_instance;
    uint32_t reserved;
} WWMI_ExecutedDrawArguments_v1;

typedef struct WWMI_ExecutedDrawView_v1 {
    uint64_t resource_pointer; // Numeric observation only. Never dereference it.
    uint64_t gpu_va;           // Numeric GPU address only.
    uint32_t size_bytes;
    uint32_t stride_or_dxgi_format;
    uint32_t known;
    uint32_t provenance;       // A last-write hint is NOT a final binding proof.
} WWMI_ExecutedDrawView_v1;

typedef struct WWMI_ExecutedDrawRecord_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t phase;
    uint32_t source;
    uint64_t sequence;
    uint64_t call_id;
    uint64_t session_id;
    uint64_t thread_id;
    uint64_t timestamp_ticks;
    uint64_t timestamp_frequency;
    uint32_t clock_domain;
    uint32_t reserved0;
    uint64_t known_fields;
    uint64_t incomplete_fields;
    uint64_t callback_failures_total;
    uint64_t suppressed_reentrant_total;
    uint64_t observation_failures_total;
    uint64_t native_command_list;
    uint64_t public_command_list;
    uint64_t host_list_generation_hint;
    uint64_t native_list_creation_id; // Unknown in the current integration.
    uint64_t render_epoch;            // Never filled from Present/frame counter.
    uint64_t object_instance_id;
    uint64_t mod_generation;
    uint64_t material_intent_receipt;
    uint64_t deformation_receipt;
    uint64_t original_vs_hash;
    uint64_t original_depth_view;
    uint32_t mod_index;
    uint32_t component_index;
    uint32_t configured_rule_ib_hash; // Section rule, NOT a newly measured hash.
    uint32_t configured_rule_first_index;
    uint32_t configured_rule_index_count;
    uint32_t host_pass_generation_hint;
    uint32_t original_render_target_count;
    uint32_t original_has_depth;
    uint32_t full_shadow_path;
    uint32_t requested_index_count;   // Preserves 0xffffffff drawindexed=auto.
    uint32_t requested_first_index;
    uint32_t requested_base_vertex_bits; // Original uint32 b2; no signed guess.
    WWMI_ExecutedDrawArguments_v1 original_arguments;
    WWMI_ExecutedDrawArguments_v1 actual_arguments;
    WWMI_ExecutedDrawView_v1 actual_vb[8]; // Unknown, zero, in current patch.
    WWMI_ExecutedDrawView_v1 actual_ib;
    WWMI_ExecutedDrawView_v1 entry_game_vb[8];
    WWMI_ExecutedDrawView_v1 entry_game_ib;
    WWMI_ExecutedDrawView_v1 shim_last_write_vb[8];
    WWMI_ExecutedDrawView_v1 relocation_slot6_hint;
} WWMI_ExecutedDrawRecord_v1;
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif
typedef void (WWMI_EXEC_CALL *WWMI_ExecutedDraw_Callback_v1)(
    const WWMI_ExecutedDrawRecord_v1 *record, void *user);

// 0 success, -1 invalid callback, -2 busy/draining/internal failure,
// -3 called from inside callback, -4 subscriber mismatch. Single subscriber.
// Unregister success drains callbacks. Do not call under DllMain/loader lock.
WWMI_EXEC_EXPORT int32_t WWMI_EXEC_CALL WWMI_ExecutedDraw_Register_v1(
    WWMI_ExecutedDraw_Callback_v1 callback, void *user);
WWMI_EXEC_EXPORT int32_t WWMI_EXEC_CALL WWMI_ExecutedDraw_Unregister_v1(
    WWMI_ExecutedDraw_Callback_v1 callback, void *user);
#ifdef __cplusplus
}

namespace wwmi::executed_draw {
struct Token { uint64_t session = 0, call_id = 0; };
// Pure atomic fast path when no subscriber; no state allocation/metadata work.
uint64_t SnapshotSession() noexcept;
Token Before(uint64_t session, const WWMI_ExecutedDrawRecord_v1 &payload) noexcept;
// After carries only a correlated API-return marker, never a completion claim.
void After(Token token) noexcept;
void ObservationFailed() noexcept;
}
#endif
