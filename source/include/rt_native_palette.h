#pragma once
#include "rt_pack_identity_v2.h"
#ifdef __cplusplus
extern "C" {
#endif
enum WWMI_RtNativePaletteStatus_v1 {
    WWMI_RT_PALETTE_OK=0,WWMI_RT_PALETTE_NATIVE_UNPROVEN=1,
    WWMI_RT_PALETTE_NOT_READY=2,WWMI_RT_PALETTE_COMPONENT_MISMATCH=3,
    WWMI_RT_PALETTE_BAD_INPUT=4,WWMI_RT_PALETTE_INTERNAL_ERROR=5
};
enum WWMI_RtNativePaletteInputs_v1 {WWMI_RT_PALETTE_CURRENT_POSE=1};
enum WWMI_RtNativePaletteSource_v1 {WWMI_RT_PALETTE_NATIVE_UPLOAD=1,WWMI_RT_PALETTE_OWNED_UPLOAD=2};
// Only the current pose which produced this exact proved native draw.
// No previous-frame/primitive input is provided or implied. Main must retain
// its independent primitive and instance-association readiness gates.
typedef struct WWMI_RtNativePalette_v1 {
    uint32_t struct_size,abi_version,source_kind,resource_state;
    uint64_t resource,resource_creation,gpu_va,resource_offset,bytes,cbv_bytes,resource_bytes;
    uint32_t matrix_count,matrix_stride,float4_count,provided_inputs;
    uint64_t consumer_list,consumer_list_creation,consumer_recording;
    // Geometry/pose provenance, not a unique engine primitive/instance key.
    uint64_t native_blas_resource,native_blas_creation,native_blas_gpu_va,pose_order;
    // Source provenance only: do not dereference these source handles. The
    // source can be gone/reused when OWNED_UPLOAD supplies the saved snapshot.
    uint64_t source_resource,source_creation,source_offset,source_bytes;
    WWMI_RtPackDrawIdentity_v2 native_identity;
} WWMI_RtNativePalette_v1;
typedef uint32_t (*WWMI_RtPackAcquireNativePaletteFn_v1)(const WWMI_RtPackDrawQuery_v2 *query,uint32_t query_size,
    uint32_t expected_component,WWMI_RtNativePalette_v1 *out,uint32_t out_size);
// query uses ORIGINAL native IA and resolved draw arguments, before overrides.
// Success commits the palette to this graphics recording's existing holds and
// actual-submit fences before return. Borrowed COM handles: do not Release or
// cache for another recording. bytes is the exact logical palette length;
// cbv_bytes includes allocation padding, never extra matrices. No GPU command
// or native root/VS binding is emitted by this function.
uint32_t WWMI_RtPackAcquireNativePalette_v1(const WWMI_RtPackDrawQuery_v2 *query,uint32_t query_size,
    uint32_t expected_component,WWMI_RtNativePalette_v1 *out,uint32_t out_size);
#ifdef __cplusplus
}
#endif
