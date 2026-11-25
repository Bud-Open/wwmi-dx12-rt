#pragma once
#include "rt_pack_identity.h"
#ifdef __cplusplus
extern "C" {
#endif
// Actual IA views and resolved native DrawIndexed arguments. No guessed queue.
typedef struct WWMI_RtPackDrawQuery_v2 {
    uint32_t struct_size,abi_version,index_format,vertex_stride;
    uint32_t index_count,first_index;
    int32_t base_vertex;
    uint32_t reserved;
    uint64_t device,list;
    uint64_t vertex_buffer_gpu_va,vertex_buffer_bytes;
    uint64_t index_buffer_gpu_va,index_buffer_bytes;
} WWMI_RtPackDrawQuery_v2;
enum WWMI_RtPackOrdering_v2 {WWMI_RT_PACK_SAME_RECORDING=1,WWMI_RT_PACK_PRODUCER_FENCE_COMPLETED=2};
typedef struct WWMI_RtPackDrawIdentity_v2 {
    uint32_t struct_size,abi_version,ordering,reserved;
    WWMI_RtPackDrawIdentity_v1 native;
    uint64_t index_resource,index_creation,index_offset;
    uint64_t producer_queue,producer_fence,producer_fence_value,observed_completion;
} WWMI_RtPackDrawIdentity_v2;
typedef uint32_t (*WWMI_RtPackIdentifyDrawFn_v2)(const WWMI_RtPackDrawQuery_v2 *query,uint32_t query_size,
    WWMI_RtPackDrawIdentity_v2 *out,uint32_t out_size,void *palette,uint32_t palette_capacity);
uint32_t WWMI_RtPackIdentifyDraw_v2(const WWMI_RtPackDrawQuery_v2 *query,uint32_t query_size,
    WWMI_RtPackDrawIdentity_v2 *out,uint32_t out_size,void *palette,uint32_t palette_capacity);
// Identifies a SkinCache-skinned draw for an auxiliary skip-only mesh that has no
// RT BLAS profile: returns 2 when rest content hash is proved (*out_content_hash),
// 1 when output matches a SkinCache write and a rest-position probe is pending,
// or 0 when VB0 is not a tracked SkinCache output (caller may fall back to IB hash).
typedef uint32_t (*WWMI_RtPackIdentifyAuxSkipDrawFn_v1)(const WWMI_RtPackDrawQuery_v2 *query,uint32_t query_size,
    uint32_t *out_content_hash);
uint32_t WWMI_RtPackIdentifyAuxSkipDraw_v1(const WWMI_RtPackDrawQuery_v2 *query,uint32_t query_size,
    uint32_t *out_content_hash);
#ifdef __cplusplus
}
#endif
