#pragma once
#include "rt_geometry_plan.h"
#ifdef __cplusplus
extern "C" {
#endif
enum WWMI_RtLocalVfFlags_v1 {WWMI_RT_SURFACE_NEED_COLOR=1};
enum WWMI_RtLocalVfStatus_v1 {
    WWMI_RT_SURFACE_OK=0,WWMI_RT_SURFACE_NOT_READY=1,WWMI_RT_SURFACE_UV_MISSING=2,
    WWMI_RT_SURFACE_TANGENTS_MISSING=3,WWMI_RT_SURFACE_COLOR_MISSING=4,
    WWMI_RT_SURFACE_PREVIOUS_MISSING=5,WWMI_RT_SURFACE_BAD_INPUT=6,WWMI_RT_SURFACE_INTERNAL_ERROR=7
};
typedef struct WWMI_RtLocalVfView_v1 {
    uint64_t resource,gpu_va,byte_offset,bytes;
    uint32_t format,element_stride,element_count,vertex_stride,resource_state,reserved;
} WWMI_RtLocalVfView_v1;
typedef struct WWMI_RtLocalVfRequest_v1 {
    WWMI_RtPlannedGeometryRequest_v1 planned;
    uint64_t current_frame; // actual main frame, NOT the saved plan's frame
    uint32_t flags,reserved;
} WWMI_RtLocalVfRequest_v1;
typedef struct WWMI_RtLocalVfSurface_v1 {
    uint32_t struct_size,abi_version,vertex_count,uv_pairs;
    WWMI_RtGraphicsGeometry_v1 geometry;
    WWMI_RtLocalVfView_v1 uv,tangents,color;
} WWMI_RtLocalVfSurface_v1;
// The ACTUAL previous-position VS buffer SRV, supplied by main after resolving
// that shader's descriptor. Byte window, not a slot number or guessed VA.
typedef struct WWMI_RtNativePositionSrv_v1 {
    uint64_t resource,byte_offset,bytes;
    uint32_t format,structure_stride; // typed R32_FLOAT, structure_stride=0
} WWMI_RtNativePositionSrv_v1;
typedef struct WWMI_RtPreviousSurfaceRequest_v1 {
    WWMI_RtLocalVfRequest_v1 current;
    WWMI_RtNativePositionSrv_v1 native_previous;
} WWMI_RtPreviousSurfaceRequest_v1;
typedef struct WWMI_RtPreviousSurface_v1 {
    uint32_t struct_size,abi_version,component,reserved;
    WWMI_RtLocalVfView_v1 positions; // R32_FLOAT, 3 elements/Mod vertex
    uint64_t frame_id,pose_order,native_resource,native_creation,native_offset;
    uint64_t producer_queue,producer_fence,producer_value,observed_completion;
    uint64_t consumer_list,consumer_creation,consumer_recording;
} WWMI_RtPreviousSurface_v1;
typedef uint32_t (*WWMI_RtPackAcquireLocalVfSurfaceFn_v1)(const WWMI_RtLocalVfRequest_v1 *,uint32_t,WWMI_RtLocalVfSurface_v1 *,uint32_t);
typedef uint32_t (*WWMI_RtPackAcquirePreviousSurfaceFn_v1)(const WWMI_RtPreviousSurfaceRequest_v1 *,uint32_t,WWMI_RtPreviousSurface_v1 *,uint32_t);
// Success has committed all returned resources to the actual graphics recording
// and real-submit/fence holds. Borrowed handles; no Release or cross-list cache.
// UV: R16G16_FLOAT, uv_pairs elements/vertex (Hsin has four). Tangent basis: two R32G32B32A32_FLOAT
// elements/vertex (T.xyz, then N.xyz + handedness). Color: R8G8B8A8_UNORM.
uint32_t WWMI_RtPackAcquireLocalVfSurface_v1(const WWMI_RtLocalVfRequest_v1 *,uint32_t,WWMI_RtLocalVfSurface_v1 *,uint32_t);
// Returns only a unique, submitted/completed previous-frame Mod position output
// linked by this draw's real native previous SRV. Never aliases current by guess.
uint32_t WWMI_RtPackAcquirePreviousSurface_v1(const WWMI_RtPreviousSurfaceRequest_v1 *,uint32_t,WWMI_RtPreviousSurface_v1 *,uint32_t);
// Main provider: same flat planned catalog, resource.reserved carries its parsed
// DXGI format. Existing catalog exports and issued-draw semantics stay unchanged.
WWMI_RT_EXPORT uint32_t WWMI_RtModReadSurfaceCatalog_v1(void *,uint64_t,uint64_t *);
#ifdef __cplusplus
}
#endif
