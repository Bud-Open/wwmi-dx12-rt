#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// A native producer proof, independent of Mod enable/selection. The caller
// copies the optional palette into its own float buffer (3 float4 per bone).
typedef struct WWMI_RtPackDrawIdentity_v1 {
    uint32_t struct_size,abi_version,object_guid,component,content_hash;
    uint32_t native_vertex_first,native_vertex_count,palette_bytes;
    uint64_t list,list_creation,recording;
    uint64_t content_resource,content_creation,content_offset;
    uint64_t output_resource,output_creation,position_base,output_offset,output_bytes;
    float scale;
    uint32_t native_extra_input,native_delta_flags,palette_copied_bytes;
} WWMI_RtPackDrawIdentity_v1;
typedef uint32_t (*WWMI_RtPackIdentifyDrawFn_v1)(uint64_t device,uint64_t list,uint64_t native_vertex_base,
    uint32_t native_first_index,uint32_t native_index_count,WWMI_RtPackDrawIdentity_v1 *out,uint32_t out_size,
    void *palette,uint32_t palette_capacity);
// Returns 1 only for one exact live resource-creation / recording producer
// joined to this native vertex base and this object's original IB range.
// Zero never authorizes skipping a native draw. With insufficient palette
// capacity the identity remains valid, but palette_copied_bytes is zero.
uint32_t WWMI_RtPackIdentifyDraw_v1(uint64_t device,uint64_t list,uint64_t native_vertex_base,
    uint32_t native_first_index,uint32_t native_index_count,WWMI_RtPackDrawIdentity_v1 *out,uint32_t out_size,
    void *palette,uint32_t palette_capacity);
#ifdef __cplusplus
}
#endif
