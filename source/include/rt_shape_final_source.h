#pragma once
#include <stdint.h>

#if defined(_WIN32) && defined(WWMI_RT_DEFORMED_SOURCE_IMPLEMENTATION)
#define WWMI_RT_DEFORMED_API __declspec(dllexport)
#else
#define WWMI_RT_DEFORMED_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum WWMI_RtDeformedKind_v1 {
    WWMI_RT_DEFORMED_UNKNOWN = 0,
    WWMI_RT_DEFORMED_AUTHOR_REST = 1,
    WWMI_RT_DEFORMED_AUTHOR_FLOAT6 = 2,
    WWMI_RT_DEFORMED_FINAL_PAIR = 3
};

typedef struct WWMI_RtDeformedRecording_v1 {
    uint64_t list, creation, recording;
} WWMI_RtDeformedRecording_v1;

// The exact native position write named by this component's SkinSample, or
// by the real original previous SRV. These are historical IDs, not COM leases.
typedef struct WWMI_RtDeformedNativeWrite_v1 {
    WWMI_RtDeformedRecording_v1 writer;
    uint64_t resource, creation, byte_offset, bytes, order;
    uint32_t component, reserved;
} WWMI_RtDeformedNativeWrite_v1;

// Selection comes from the already proved AuthorPlan, not from the number of
// ShapeKeys, vb0 equality, a resource pointer, or a map's only remaining entry.
// recipe_receipt names the actual evaluated inputs including control writes;
// it is not the catalog geometry revision or a guess from the INI namespace.
typedef struct WWMI_RtDeformedSelection_v1 {
    uint64_t device, plan, native_snapshot;
    uint64_t primitive, primitive_creation, primitive_byte_origin;
    uint64_t load_generation, mod_key, mod_revision, recipe_receipt;
    WWMI_RtDeformedNativeWrite_v1 native_write;
    uint32_t kind, reserved;
} WWMI_RtDeformedSelection_v1;

typedef struct WWMI_RtDeformedSourceRequest_v1 {
    uint32_t struct_size, abi_version;
    WWMI_RtDeformedSelection_v1 selection;
    WWMI_RtDeformedRecording_v1 consumer;
} WWMI_RtDeformedSourceRequest_v1;

typedef struct WWMI_RtDeformedView_v1 {
    uint64_t resource, creation, byte_offset, bytes;
    uint32_t stride, state;
} WWMI_RtDeformedView_v1;

typedef struct WWMI_RtDeformedSource_v1 {
    uint32_t struct_size, abi_version, first_vertex, vertex_count;
    WWMI_RtDeformedSelection_v1 selection;
    // A producer recording and its last write are independent of native_write.
    WWMI_RtDeformedRecording_v1 producer;
    uint64_t producer_write_order;
    uint64_t completion_queue, completion_fence, completion_value;
    // AUTHOR_FLOAT6: primary is [position delta.xyz, normal delta.xyz], stride
    // 24, still pre-skin, vectors absent. FINAL_PAIR: primary is fully deformed
    // object-local float3 XYZ (stride 12), vectors are float4 T followed by
    // float4 N (stride 32, both w preserved). Same author vertex window.
    // AUTHOR_REST is a positive immutable author-input decision, no views.
    WWMI_RtDeformedView_v1 primary, vectors;
} WWMI_RtDeformedSource_v1;

// Existing WWMI_RtModStatus_v1 values: 0 OK, 2 NOT_READY, 3 STALE,
// 4 BAD_INPUT. The caller supplies the expected selection from the actual
// scope, not a fallback preference. No successful UNKNOWN; any mismatch or
// incomplete FINAL remains unready, never a successful REST. Failure yields
// no leases. On success each nonzero resource field independently transfers
// one ID3D12Resource reference (even if the two fields alias); a nonzero fence
// transfers one ID3D12Fence reference. No queue reference is transferred.
// Without actual completion the source can only be read after its write in
// the same real producer recording. No mutation, GPU recording, or wait.
// For previous, retain the original source (or the batch derived from it)
// with its leases. Resolve the real original SRV/stamp in the consumer and
// match its native_write exactly, plus existing proved instance/recipe
// compatibility and producer completion. Never retry this query as if a
// previous pose belonged to the current plan or had today's control values.
typedef uint32_t (*WWMI_RtModAcquireDeformedSourceFn_v1)(
    const WWMI_RtDeformedSourceRequest_v1 *, uint32_t request_size,
    WWMI_RtDeformedSource_v1 *, uint32_t out_size);
WWMI_RT_DEFORMED_API uint32_t WWMI_RtModAcquireDeformedSource_v1(
    const WWMI_RtDeformedSourceRequest_v1 *, uint32_t request_size,
    WWMI_RtDeformedSource_v1 *, uint32_t out_size);

#ifdef __cplusplus
}
static_assert(sizeof(WWMI_RtDeformedRecording_v1) == 24);
static_assert(sizeof(WWMI_RtDeformedNativeWrite_v1) == 72);
static_assert(sizeof(WWMI_RtDeformedSelection_v1) == 160);
static_assert(sizeof(WWMI_RtDeformedSourceRequest_v1) == 192);
static_assert(sizeof(WWMI_RtDeformedView_v1) == 40);
static_assert(sizeof(WWMI_RtDeformedSource_v1) == 312);
#endif
