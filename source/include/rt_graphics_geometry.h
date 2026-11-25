#pragma once
#include "rt_pack_identity_v2.h"
#ifdef __cplusplus
extern "C" {
#endif
// A request for one actual Mod section draw, tied to its original native draw.
typedef struct WWMI_RtGraphicsGeometryRequest_v1 {
    uint32_t struct_size,abi_version;
    WWMI_RtPackDrawQuery_v2 native_draw;
    uint64_t section_token,load_generation,mod_key,mod_revision;
    uint32_t component,mod_index_count,mod_first_index;
    int32_t mod_base_vertex;
} WWMI_RtGraphicsGeometryRequest_v1;
enum WWMI_RtGraphicsGeometryStatus_v1 {
    WWMI_RT_GEOMETRY_OK=0,WWMI_RT_GEOMETRY_NATIVE_UNPROVEN=1,
    WWMI_RT_GEOMETRY_STALE_SELECTION=2,WWMI_RT_GEOMETRY_BATCH_NOT_READY=3,
    WWMI_RT_GEOMETRY_DRAW_NOT_SELECTED=4,WWMI_RT_GEOMETRY_BAD_INPUT=5,
    WWMI_RT_GEOMETRY_INTERNAL_ERROR=6
};
typedef struct WWMI_RtGraphicsGeometry_v1 {
    uint32_t struct_size,abi_version;
    uint64_t load_generation,mod_key,mod_revision,batch_token;
    uint64_t consumer_list,consumer_list_creation,consumer_recording;
    uint64_t native_blas_resource,native_blas_creation,native_blas_gpu_va;
    uint64_t position_resource,position_gpu_va,position_offset,position_bytes;
    uint32_t position_stride,position_format,position_state,reserved;
    uint64_t index_resource,index_gpu_va,index_offset,index_bytes;
    uint32_t index_format,index_count,first_index;
    int32_t base_vertex;
    WWMI_RtPackDrawIdentity_v2 native_identity;
} WWMI_RtGraphicsGeometry_v1;
typedef uint32_t (*WWMI_RtPackAcquireGraphicsGeometryFn_v1)(const WWMI_RtGraphicsGeometryRequest_v1 *request,uint32_t request_size,
    WWMI_RtGraphicsGeometry_v1 *out,uint32_t out_size);
// Success has already retained the exact batch in this graphics recording's
// existing pre/post-submit/fence holds. Main may now bind the returned views and
// issue this one draw. Resource handles are borrowed under that recording lease;
// do not Release or cache them for another recording. No GPU work is initiated.
// The owned IB subview uses returned first_index=0/base_vertex=0, not INI first.
uint32_t WWMI_RtPackAcquireGraphicsGeometry_v1(const WWMI_RtGraphicsGeometryRequest_v1 *request,uint32_t request_size,
    WWMI_RtGraphicsGeometry_v1 *out,uint32_t out_size);
#ifdef __cplusplus
}
#endif
