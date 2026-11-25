// SPDX-License-Identifier: GPL-3.0-only
// Observer-only C ABI. All addresses are numeric observations, never CPU data.
#pragma once
#include <stdint.h>
#if defined(_WIN32)
# define WWMI_BIND_CALL __cdecl
# if defined(WWMI_BINDING_TRACE_BUILD)
#  define WWMI_BIND_EXPORT __declspec(dllexport)
# else
#  define WWMI_BIND_EXPORT
# endif
#else
# define WWMI_BIND_CALL
# define WWMI_BIND_EXPORT
#endif

#define WWMI_BIND_ABI_V1 1u
#define WWMI_BIND_SOURCE_WRAPPER 1u
#define WWMI_BIND_SOURCE_HOST_INTERNAL 2u
#define WWMI_BIND_SOURCE_CANDIDATE 3u
#define WWMI_BIND_LIST_CREATE 1u
#define WWMI_BIND_LIST_DESTROY 2u
#define WWMI_BIND_LIST_RESET 3u
#define WWMI_BIND_LIST_CLOSE 4u
#define WWMI_BIND_VERTEX_BUFFERS 5u
#define WWMI_BIND_INDEX_BUFFER 6u
#define WWMI_BIND_TOPOLOGY 7u
#define WWMI_BIND_CLEAR_STATE 8u
#define WWMI_BIND_PREDICATION 9u
#define WWMI_BIND_INVALIDATE 10u
#define WWMI_BIND_DRAW_BEFORE 11u
#define WWMI_BIND_DRAW_AFTER 12u
// List construction does not establish whether CreateCommandList1 was closed.
#define WWMI_BIND_INITIAL_STATE_UNKNOWN 0u
#define WWMI_BIND_INITIAL_STATE_RECORDING 1u
#define WWMI_BIND_INITIAL_STATE_CLOSED 2u
#define WWMI_BIND_REASON_BUNDLE 1u
#define WWMI_BIND_REASON_INDIRECT 2u
#define WWMI_BIND_REASON_ALIASING 3u
#define WWMI_BIND_REASON_ENHANCED_BARRIER 4u
#define WWMI_BIND_REASON_STRIP_CUT 5u
#define WWMI_BIND_REASON_EXTERNAL_NATIVE 6u
#define WWMI_BIND_REASON_MALFORMED 7u
#define WWMI_BIND_REASON_UNOBSERVED_LIFETIME 8u
#define WWMI_BIND_ISSUE_MISSING_CREATE (UINT64_C(1)<<0)
#define WWMI_BIND_ISSUE_UNKNOWN_RECORDING (UINT64_C(1)<<1)
#define WWMI_BIND_ISSUE_CLOSED_LIST (UINT64_C(1)<<2)
#define WWMI_BIND_ISSUE_INVALIDATED (UINT64_C(1)<<3)
#define WWMI_BIND_ISSUE_MALFORMED (UINT64_C(1)<<4)
#define WWMI_BIND_ISSUE_CALLBACK_FAILURE (UINT64_C(1)<<5)
#define WWMI_BIND_ISSUE_REENTRANT (UINT64_C(1)<<6)
#define WWMI_BIND_ISSUE_LATE_SUBSCRIPTION (UINT64_C(1)<<7)
// Always present: this observer is not a lifetime pin, layout/deformation proof,
// complete process hook, render-epoch proof, nor GPU-execution receipt.
#define WWMI_BIND_UNPROVEN_RESOURCES (UINT64_C(1)<<0)
#define WWMI_BIND_UNPROVEN_LAYOUT (UINT64_C(1)<<1)
#define WWMI_BIND_UNPROVEN_DEFORMATION (UINT64_C(1)<<2)
#define WWMI_BIND_UNPROVEN_EXTERNAL_PATHS (UINT64_C(1)<<3)
#define WWMI_BIND_UNPROVEN_RENDER_EPOCH (UINT64_C(1)<<4)
#define WWMI_BIND_UNPROVEN_GPU_EXECUTION (UINT64_C(1)<<5)
#define WWMI_BIND_UNPROVEN_ALL UINT64_C(0x3f)

#pragma pack(push,8)
typedef struct WWMI_BindingView_v1 {
    uint64_t gpu_va;
    uint32_t size_bytes;
    uint32_t stride_or_format;
} WWMI_BindingView_v1;
typedef struct WWMI_BindingDraw_v1 {
    uint32_t index_count, instance_count, first_index;
    int32_t base_vertex;
    uint32_t first_instance, reserved;
} WWMI_BindingDraw_v1;
typedef struct WWMI_BindingInput_v1 {
    uint32_t struct_size, abi_version, kind, source;
    uint64_t native_list, public_list;
    uint64_t executed_session, executed_call;
    uint64_t paired_before_sequence;
    uint32_t start_slot, count, views_null, topology;
    int32_t hresult;
    uint32_t reason, initial_state, source_site;
    uint64_t predicate_resource, predicate_offset;
    uint32_t predicate_operation, reserved2;
    WWMI_BindingDraw_v1 draw;
    WWMI_BindingView_v1 views[32];
} WWMI_BindingInput_v1;
typedef struct WWMI_BindingRecord_v1 {
    uint32_t struct_size, abi_version;
    uint64_t session, sequence, list_sequence;
    uint64_t qpc, qpc_frequency, thread_id;
    uint32_t clock_domain, reserved;
    uint64_t creation_id, recording_generation, binding_revision;
    uint64_t capture_issues, unproven_fields;
    uint64_t callback_failures_total, suppressed_reentrant_total, observation_failures_total;
    uint32_t recording_state, vertex_known_mask, vertex_null_mask;
    uint32_t index_known, index_null, topology_known, topology;
    uint32_t predicate_known, predicate_operation, reserved2;
    uint64_t predicate_resource, predicate_offset;
    WWMI_BindingView_v1 vertex_views[32];
    WWMI_BindingView_v1 index_view;
    WWMI_BindingInput_v1 input;
} WWMI_BindingRecord_v1;
typedef struct WWMI_BindingStatus_v1 {
    uint32_t struct_size, abi_version;
    uint64_t last_session, last_sequence;
    uint64_t callback_failures_total, suppressed_reentrant_total, observation_failures_total;
    uint64_t pending_draws, tracked_lists;
    uint32_t active, draining;
} WWMI_BindingStatus_v1;
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif
typedef void (WWMI_BIND_CALL *WWMI_BindingTrace_Callback_v1)(const WWMI_BindingRecord_v1*,void*);
typedef uint64_t (WWMI_BIND_CALL *WWMI_BindingTrace_SubmitFn_v1)(const WWMI_BindingInput_v1*);
// Single subscriber. 0 success, -1 invalid, -2 busy, -3 recursive, -4 mismatch.
// Unregister drains callbacks; never invoke from DllMain or unload first.
WWMI_BIND_EXPORT int32_t WWMI_BIND_CALL WWMI_BindingTrace_Register_v1(WWMI_BindingTrace_Callback_v1,void*);
WWMI_BIND_EXPORT int32_t WWMI_BIND_CALL WWMI_BindingTrace_Unregister_v1(WWMI_BindingTrace_Callback_v1,void*);
// Returns this event's sequence, or zero when inactive/rejected. A draw-after
// input must carry the corresponding nonzero draw-before return value. All
// input CPU arrays have already been copied into this POD. No native calls here.
WWMI_BIND_EXPORT uint64_t WWMI_BIND_CALL WWMI_BindingTrace_Submit_v1(const WWMI_BindingInput_v1*);
// Read after Unregister has drained to capture terminal failures/pending draws.
// Caller sets struct_size and abi_version=1; no native/GPU/Streamline calls.
WWMI_BIND_EXPORT int32_t WWMI_BIND_CALL WWMI_BindingTrace_GetStatus_v1(WWMI_BindingStatus_v1*);
#ifdef __cplusplus
}
#endif
