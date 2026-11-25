#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct WWMI_RtPackDrawQuery_v2 WWMI_RtPackDrawQuery_v2;
#ifdef __cplusplus
extern "C" {
#endif
enum WWMI_RtNativeShapeLinkStatus_v1 {
    WWMI_RT_SHAPE_LINK_LOCAL_RECORDED=0,
    WWMI_RT_SHAPE_LINK_NOT_READY=1,
    WWMI_RT_SHAPE_LINK_READ_UNOBSERVED=2,
    WWMI_RT_SHAPE_LINK_WRITER_UNOBSERVED=3,
    WWMI_RT_SHAPE_LINK_VIEW_MISSING=4,
    WWMI_RT_SHAPE_LINK_CB_UNOBSERVED=5,
    WWMI_RT_SHAPE_LINK_BATCH_INCOMPLETE=6,
    WWMI_RT_SHAPE_LINK_INTERVENING_WRITE=7,
    WWMI_RT_SHAPE_LINK_CROSS_RECORDING=8,
    WWMI_RT_SHAPE_LINK_VIEW_PARTIAL=9,
    WWMI_RT_SHAPE_LINK_AMBIGUOUS=10,
    WWMI_RT_SHAPE_LINK_RECORDING_EXPIRED=11,
    WWMI_RT_SHAPE_LINK_BAD_INPUT=12,
    WWMI_RT_SHAPE_LINK_NOT_NATIVE_RECORDED=13
};
enum WWMI_RtNativeShapeEvidence_v1 {
    WWMI_RT_SHAPE_EVIDENCE_NATIVE_CALL_RETURNED=1,
    WWMI_RT_SHAPE_EVIDENCE_CB_CPU_OBSERVED=2,
    WWMI_RT_SHAPE_EVIDENCE_DECLARED_UAV_CONTAINS_READ=4,
    WWMI_RT_SHAPE_EVIDENCE_LOCAL_COMMAND_ORDER=8
};
#pragma pack(push,8)
typedef struct WWMI_RtNativeShapeView_v1 {
    uint64_t resource,creation,byte_offset,bytes;
    uint32_t format,reserved;
} WWMI_RtNativeShapeView_v1;
typedef struct WWMI_RtNativeShapeWrite_v1 {
    uint64_t list,list_creation,recording,order,call_id,cs_hash;
    uint32_t group_x,group_y,group_z,evidence;
    WWMI_RtNativeShapeView_v1 cb0,u0,u1;
    // Recording-thread CPU observation of the actual bound 66 uint4 window.
    // This is NOT an immutable GPU CB snapshot or GPU contents/readback proof.
    uint32_t cb_words[264];
} WWMI_RtNativeShapeWrite_v1;
typedef struct WWMI_RtNativeShapeLink_v1 {
    uint32_t struct_size,abi_version,status,loader_count;
    uint64_t read_list,read_list_creation,read_recording,read_order,skin_order;
    WWMI_RtNativeShapeView_v1 native_t3;
    uint64_t read_byte_offset,read_bytes;
    WWMI_RtNativeShapeWrite_v1 loaders[2],multiplier;
} WWMI_RtNativeShapeLink_v1;
#pragma pack(pop)
// Read-only causal metadata for the exact v2-proven native component/pose.
// No references/resources are transferred. Resource values are historical IDs,
// NEVER borrowed COM pointers. LOCAL_RECORDED means this exact native binding
// chain was recorded locally, not that author geometry was produced, all UAV
// elements were written, or cross-queue/GPU completion has been established.
// In particular this is NOT WWMI_RtModAcquirePostMultiplierShape_v1 and cannot
// be converted into that provider's OK result without actual author execution
// and an immutable output lease. Missing/expired input stays not ready.
#ifndef WWMI_RT_NATIVE_SHAPE_LINK_API
#define WWMI_RT_NATIVE_SHAPE_LINK_API
#endif
WWMI_RT_NATIVE_SHAPE_LINK_API uint32_t WWMI_RtPackReadNativeShapeLink_v1(const WWMI_RtPackDrawQuery_v2 *,uint32_t query_size,
    uint32_t component,WWMI_RtNativeShapeLink_v1 *,uint32_t out_size);
#ifdef __cplusplus
}
static_assert(sizeof(WWMI_RtNativeShapeView_v1)==40,"native shape view ABI");
static_assert(sizeof(WWMI_RtNativeShapeWrite_v1)==1240,"native shape write ABI");
static_assert(sizeof(WWMI_RtNativeShapeLink_v1)==3832,"native shape link ABI");
#endif
