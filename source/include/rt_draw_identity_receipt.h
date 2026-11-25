#pragma once
#include "rt_pack_identity_v2.h"
#ifdef __cplusplus
extern "C" {
#endif

enum WWMI_RtDrawIdentityReason_v1 {
    WWMI_RT_IDENTIFY_OK, WWMI_RT_IDENTIFY_QUERY, WWMI_RT_IDENTIFY_DEVICE,
    WWMI_RT_IDENTIFY_INACTIVE, WWMI_RT_IDENTIFY_RECORDING, WWMI_RT_IDENTIFY_RAW_SUBVIEW,
    WWMI_RT_IDENTIFY_VERTEX_RANGE, WWMI_RT_IDENTIFY_INDEX_RANGE, WWMI_RT_IDENTIFY_RESOURCE_KEYS,
    WWMI_RT_IDENTIFY_ORDER_PENDING, WWMI_RT_IDENTIFY_AMBIGUOUS, WWMI_RT_IDENTIFY_SOURCE_EXPIRED
};
typedef struct WWMI_RtDrawIdentityReceipt_v1 {
    uint32_t struct_size,abi_version,reason,exact_matches;
    uint32_t submissions,object_guid,content_hash,component;
    uint64_t consumer_creation,consumer_recording;
    uint64_t vertex_resource,vertex_creation,index_resource,index_creation;
    uint64_t content_resource,content_creation,content_offset;
    uint64_t blas_resource,blas_creation,blas_va,pose_order;
    uint64_t writer_list,writer_creation,writer_recording;
    uint64_t producer_queue,producer_fence,producer_value,observed_completion;
    // Only on a raw subview miss: exact tracked write to the native profile
    // candidate's vertex byte window, not an identified draw or BLAS proof.
    uint64_t candidate_write_offset,candidate_write_bytes,candidate_write_order;
    uint64_t candidate_writer_list,candidate_writer_creation,candidate_writer_recording;
    uint32_t candidate_writer_submissions,reserved;
} WWMI_RtDrawIdentityReceipt_v1;
// Same result and render authorization as IdentifyDraw_v2. A uniquely
// matched pending writer is useful evidence, not a successful identify.
typedef uint32_t (*WWMI_RtPackIdentifyDrawReceiptFn_v1)(const WWMI_RtPackDrawQuery_v2 *,uint32_t,
    WWMI_RtPackDrawIdentity_v2 *,uint32_t,WWMI_RtDrawIdentityReceipt_v1 *,uint32_t);
uint32_t WWMI_RtPackIdentifyDrawReceipt_v1(const WWMI_RtPackDrawQuery_v2 *,uint32_t,
    WWMI_RtPackDrawIdentity_v2 *,uint32_t,WWMI_RtDrawIdentityReceipt_v1 *,uint32_t);
#ifdef __cplusplus
}
#endif
