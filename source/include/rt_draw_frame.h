#pragma once
#include "rt_localvf_surface.h"
#ifdef __cplusplus
extern "C" {
#endif

enum WWMI_RtDrawFrameFlags_v1 { WWMI_RT_DRAW_MATERIAL=1, WWMI_RT_DRAW_PREVIOUS=2 };
// The exact current native palette already supplied to this actual section.
typedef struct WWMI_RtDrawSource_v1 {
    uint64_t list_creation,recording,blas_resource,blas_creation,blas_va,pose_order;
} WWMI_RtDrawSource_v1;
typedef struct WWMI_RtDrawFrameRequest_v1 {
    uint32_t struct_size,abi_version,flags,reserved;
    WWMI_RtPreviousSurfaceRequest_v1 inputs;
    WWMI_RtDrawSource_v1 source;
} WWMI_RtDrawFrameRequest_v1;
enum WWMI_RtDrawFramePhase_v1 { WWMI_RT_DRAW_CURRENT=3, WWMI_RT_DRAW_HISTORY=4 };
typedef struct WWMI_RtDrawFrame_v1 {
    uint32_t struct_size,abi_version,failed_phase,geometry_status;
    WWMI_RtLocalVfSurface_v1 current; // geometry-only unless MATERIAL was requested
    WWMI_RtPreviousSurface_v1 previous;
    uint64_t current_pose_order;
} WWMI_RtDrawFrame_v1;
typedef uint32_t (*WWMI_RtPackAcquireDrawFrameFn_v1)(const WWMI_RtDrawFrameRequest_v1 *,uint32_t,WWMI_RtDrawFrame_v1 *,uint32_t);
// One current native/batch proof, with optional previous determined from the
// exact native VS SRV. HISTORY + PREVIOUS_MISSING leaves current held, but
// authorizes no previous read. Only a renderer that independently proves and
// binds the native no-history program path may draw that current result.
uint32_t WWMI_RtPackAcquireDrawFrame_v1(const WWMI_RtDrawFrameRequest_v1 *,uint32_t,WWMI_RtDrawFrame_v1 *,uint32_t);

// Optional, from the original native VS bindings of this exact draw, before
// any INI override. Both root resources must be supplied. pass_generation is
// the observed OM bind generation (0 if none), not a previous-pose ID.
typedef struct WWMI_RtPreviousDrawScope_v1 {
    uint64_t view_resource,view_byte_origin,primitive_resource,primitive_byte_origin;
    uint32_t pass_generation;
    uint32_t view_words[20]; // b0[95..98].xyzw, b0[117].xyz, b0[160].x
    uint32_t primitive_words[13]; // b1[10..13].xyz, b1[26].w
} WWMI_RtPreviousDrawScope_v1;
typedef struct WWMI_RtDrawFrameRequest_v2 {
    WWMI_RtDrawFrameRequest_v1 base; // keeps its own v1 size/version
    WWMI_RtPreviousDrawScope_v1 previous_scope;
} WWMI_RtDrawFrameRequest_v2;
typedef uint32_t (*WWMI_RtPackAcquireDrawFrameFn_v2)(const WWMI_RtDrawFrameRequest_v2 *,uint32_t,WWMI_RtDrawFrame_v1 *,uint32_t);
uint32_t WWMI_RtPackAcquireDrawFrame_v2(const WWMI_RtDrawFrameRequest_v2 *,uint32_t,WWMI_RtDrawFrame_v1 *,uint32_t);

typedef struct WWMI_RtDrawBindingRequest_v1 {
    uint32_t struct_size,abi_version,descriptor_count,resource_count;
    uint64_t device,list,list_creation,recording,batch_token;
    uint32_t local_vf[6];
    // Caller owns these actual section override resources until this returns.
    const uint64_t *resources;
} WWMI_RtDrawBindingRequest_v1;
typedef struct WWMI_RtDrawBinding_v1 {
    uint32_t struct_size,abi_version,descriptor_increment,reserved;
    uint64_t heap,cpu_base,gpu_base,local_vf_va;
} WWMI_RtDrawBinding_v1;
typedef uint32_t (*WWMI_RtPackReserveDrawBindingFn_v1)(const WWMI_RtDrawBindingRequest_v1 *,uint32_t,WWMI_RtDrawBinding_v1 *,uint32_t);
// A disjoint descriptor range, and immutable constants already copied to an
// upload CB slot. Fill the range once, before recording the consuming draw.
// The exact list's existing submit/fence holds own both backing resources
// and the supplied Mod texture resources. Handles must not escape the list.
uint32_t WWMI_RtPackReserveDrawBinding_v1(const WWMI_RtDrawBindingRequest_v1 *,uint32_t,WWMI_RtDrawBinding_v1 *,uint32_t);

enum WWMI_RtNoHistoryCb_v2 {
    WWMI_RT_NH_VS_VIEW, WWMI_RT_NH_VS_PRIMITIVE, WWMI_RT_NH_PS_VIEW, WWMI_RT_NH_PS_PRIMITIVE,
    WWMI_RT_NH_CB_COUNT,
    WWMI_RT_NH_VIEW_BYTES=5952, WWMI_RT_NH_PRIMITIVE_BYTES=720
};
typedef struct WWMI_RtDrawBindingRequest_v2 {
    WWMI_RtDrawBindingRequest_v1 base; // keeps its own v1 size/version
    // Four fixed full native CB windows in the order above, in bytes.
    // NULL leaves a stage/root untouched; non-NULL is copied synchronously.
    const void *no_history_cb[WWMI_RT_NH_CB_COUNT];
} WWMI_RtDrawBindingRequest_v2;
typedef struct WWMI_RtDrawBinding_v2 {
    WWMI_RtDrawBinding_v1 base;
    // 256-byte aligned private CBV addresses; zero for untouched roots.
    // Same recording/replay/flight lifetime as base.local_vf_va.
    uint64_t no_history_va[WWMI_RT_NH_CB_COUNT];
} WWMI_RtDrawBinding_v2;
typedef uint32_t (*WWMI_RtPackReserveDrawBindingFn_v2)(const WWMI_RtDrawBindingRequest_v2 *,uint32_t,WWMI_RtDrawBinding_v2 *,uint32_t);
uint32_t WWMI_RtPackReserveDrawBinding_v2(const WWMI_RtDrawBindingRequest_v2 *,uint32_t,WWMI_RtDrawBinding_v2 *,uint32_t);

typedef void (*WWMI_RtPackNoteDrawResetFn_v1)(uint64_t native_list,int32_t hresult);
// Relay the paired host proxy's actual Reset return on the calling thread.
// The preceding ReShade notification alone cannot retire a recording.
void WWMI_RtPackNoteDrawReset_v1(uint64_t native_list,int32_t hresult);
#ifdef __cplusplus
}
#endif
