// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// Native CBV/SRV/UAV writes only. CPU handles in this packet are untagged.
// A physical address without an application heap is not a tracked app slot.
// All pointers and the native continuation are borrowed synchronously.
#ifdef __cplusplus
extern "C" {
#endif
typedef struct WWMI_DescriptorRange_v1 {
    uint64_t heap, heap_creation, cpu;
    uint32_t slot, slots, heap_index, increment;
} WWMI_DescriptorRange_v1;
typedef struct WWMI_DescriptorSpan_v1 {
    WWMI_DescriptorRange_v1 destination, source;
    uint32_t count, reserved;
} WWMI_DescriptorSpan_v1;

enum {
    WWMI_DESCRIPTOR_CONTENT_VERSION = 2,
    WWMI_DESCRIPTOR_BATCH_VERSION = 3,
    WWMI_DESCRIPTOR_UNKNOWN = 0,
    WWMI_DESCRIPTOR_SRV = 1,
    WWMI_DESCRIPTOR_UAV = 2,
    WWMI_DESCRIPTOR_CBV = 3,
    WWMI_DESCRIPTOR_RELEASE_CPU_VIEW = 4
};
typedef struct WWMI_DescriptorWrite_v1 {
    WWMI_DescriptorRange_v1 destination;
    // In v2 registration, the kind-specific cbv_va word carries the actual
    // counter-resource handle for UAV. A v1 observer never reads it for UAV.
    uint64_t resource, description, cbv_va;
    uint32_t kind, cbv_bytes;
} WWMI_DescriptorWrite_v1;

// Invoke at most once; repeated calls return the already captured HRESULT.
// Host forwards itself if the observer never calls it. Observer must not
// publish a failed native write or keep either pointer past this callback.
typedef int32_t (*WWMI_DescriptorNative_v1)(void *native_context);
typedef void (*WWMI_DescriptorVisitSpan_v1)(void *visit_context,const WWMI_DescriptorSpan_v1 *span);
typedef void (*WWMI_DescriptorSpans_v1)(void *spans_context,WWMI_DescriptorVisitSpan_v1 visit,void *visit_context);
// The span enumerator is repeatable so a failed freeze can still invalidate
// exactly the destinations of a native copy; it allocates no receipt array.
typedef void (*WWMI_DescriptorCopy_v1)(uint64_t api_device, WWMI_DescriptorSpans_v1 spans,void *spans_context,
    WWMI_DescriptorNative_v1 native, void *native_context);
typedef void (*WWMI_DescriptorWriteFn_v1)(uint64_t api_device, const WWMI_DescriptorWrite_v1 *write,
    WWMI_DescriptorNative_v1 native, void *native_context);
typedef void (*WWMI_DescriptorCopyBatch_v1)(uint64_t api_device, const WWMI_DescriptorSpan_v1 *spans,
    uint32_t span_count, uint32_t total_slots, WWMI_DescriptorNative_v1 native, void *native_context);
typedef struct WWMI_DescriptorPublisher_v1 {
    uint32_t size, version;
    uint64_t owner;
    WWMI_DescriptorCopy_v1 copy;
    WWMI_DescriptorWriteFn_v1 write;
} WWMI_DescriptorPublisher_v1;
typedef struct WWMI_DescriptorPublisher_v3 {
    WWMI_DescriptorPublisher_v1 base;
    WWMI_DescriptorCopyBatch_v1 copy_batch;
} WWMI_DescriptorPublisher_v3;
// Register with callbacks != nullptr. Unregister by owner outside loader lock
// with callbacks == nullptr; the call drains any entered native continuations.
// Main's own native stamps are outside the device proxy. Report their exact
// live destination here to invalidate the host's sparse AS/structured value.
typedef void (*WWMI_DescriptorRawAfter_v1)(uint64_t api_device,uint64_t heap,uint64_t creation,
    uint32_t heap_index,uint32_t slot);
typedef int32_t (*WWMI_DescriptorPublish_ControlFn_v1)(uint64_t owner, const WWMI_DescriptorPublisher_v1 *callbacks,
    WWMI_DescriptorRawAfter_v1 *raw_after);
#ifdef __cplusplus
}
static_assert(sizeof(WWMI_DescriptorRange_v1)==40 && sizeof(WWMI_DescriptorSpan_v1)==88);
static_assert(sizeof(WWMI_DescriptorWrite_v1)==72 && sizeof(WWMI_DescriptorPublisher_v1)==32);
static_assert(sizeof(WWMI_DescriptorPublisher_v3)==40);
#endif
