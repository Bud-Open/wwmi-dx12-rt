#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Optional main -> client source contract. The provider reads previously
// committed CPU metadata only; this call must not run the INI VM or GPU work.
// Missing association / incomplete author multiplier / mutable u0 => NOT_READY.
typedef struct WWMI_RtShapeComponent_v1 {
    uint32_t component,native_first_vertex,native_vertex_count,reserved;
    uint64_t skin_order;
    uint64_t output_resource,output_creation,output_offset,output_bytes;
    uint64_t content_resource,content_creation,content_offset;
    uint64_t palette_resource,palette_creation,palette_offset,palette_bytes;
    uint64_t index_resource,index_creation,index_offset;
    uint32_t index_count,reserved2;
    // Actual known skin8_extra t3 view consumed by this SkinSample. Zero means
    // not observed; it is not permission to select an arbitrary ShapeKey u0.
    // This records a READ, not the input's contents or last writer generation.
    uint64_t native_shape_resource,native_shape_creation;
    uint64_t native_shape_view_offset,native_shape_view_bytes;
    uint64_t native_shape_offset,native_shape_bytes;
    uint32_t native_shape_first_vertex,native_shape_vertex_count;
} WWMI_RtShapeComponent_v1;
typedef struct WWMI_RtShapePositionRequest_v1 {
    uint32_t struct_size,abi_version,object_guid,content_hash;
    uint64_t device,consumer_list,consumer_creation,consumer_recording;
    uint64_t load_generation,mod_key,mod_revision;
    uint64_t native_blas_resource,native_blas_creation,native_blas_address;
    uint64_t native_list,native_list_creation,native_recording;
    uint32_t component_count,reserved;
} WWMI_RtShapePositionRequest_v1;
enum WWMI_RtShapeLayout_v1 { WWMI_RT_SHAPE_AUTHOR_POSITION_NORMAL_FLOAT6=1 };
typedef struct WWMI_RtPostMultiplierShape_v1 {
    uint32_t struct_size,abi_version,first_vertex,vertex_count;
    uint64_t resource,resource_creation,byte_offset,bytes;
    uint32_t resource_state,layout;
    uint64_t producer_list,producer_creation,producer_recording;
    uint64_t completion_fence,completion_value;
} WWMI_RtPostMultiplierShape_v1;
// Components are copied for this call, not engine containers. All offsets and
// lengths are BYTES; vertex indices address AUTHOR topology. Output is float6
// per author vertex after the actual author loader/multiplier has completed:
// [position_delta.xyz, normal_delta.xyz], in author/object-local coordinates.
// `layout` must be WWMI_RT_SHAPE_AUTHOR_POSITION_NORMAL_FLOAT6; a same-stride
// stream with different semantics is unsupported. Its normal half is added to
// author N and used to re-orthogonalize author T BEFORE the shared skinning.
// It is neither loader integer data nor fully-deformed FINAL positions.
//
// Success (WWMI_RT_MOD_OK): add one resource and optional fence COM reference.
// The returned allocation remains IMMUTABLE for the lifetime of those leases.
// Its creation token must be genuine; reusing a mutable resource with a new
// invented token does not satisfy this contract. State includes NON_PIXEL_SRV.
// With no completed fence, only producer == consumer actual recording is valid.
// Provider lookup must prove the exact requested pose/pack; echoing this request
// or selecting by native address, CS hash, size or most recent draw is invalid.
// Failure transfers no references. A reset/unload/selection change invalidates
// the provider's matching records; absent real inputs must remain NOT_READY.
typedef uint32_t (*WWMI_RtModAcquirePostMultiplierShapeFn_v1)(
    const WWMI_RtShapePositionRequest_v1 *,const WWMI_RtShapeComponent_v1 *,
    uint32_t component_count,WWMI_RtPostMultiplierShape_v1 *,uint32_t out_size);
uint32_t WWMI_RtModAcquirePostMultiplierShape_v1(
    const WWMI_RtShapePositionRequest_v1 *,const WWMI_RtShapeComponent_v1 *,
    uint32_t component_count,WWMI_RtPostMultiplierShape_v1 *,uint32_t out_size);
#ifdef __cplusplus
}
#endif
