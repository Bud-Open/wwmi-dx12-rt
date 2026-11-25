#pragma once
#include <stdint.h>
#include "wwmi_rt_forward.h"
#ifdef __cplusplus
extern "C" {
#endif
enum WWMI_RtDispatchPhase_v2 {
    WWMI_RT_DISPATCH_BEFORE_NATIVE=1,
    WWMI_RT_DISPATCH_NATIVE_RECORDED=2,
    WWMI_RT_DISPATCH_CANCELLED=3
};
enum WWMI_RtDispatchFlags_v2 { WWMI_RT_DISPATCH_STATE_KNOWN=1 };
typedef struct WWMI_RtNativeDispatch_v2 {
    uint32_t struct_size,abi_version,phase,flags;
    uint64_t call_id,api_list,native_list,pipeline,layout;
    uint32_t group_x,group_y,group_z,reserved;
} WWMI_RtNativeDispatch_v2;
// BEFORE is after all add-on cancellation callbacks, immediately before the
// unchanged original native Dispatch. RECORDED is after that call returns.
// CANCELLED has no corresponding native call. These are command-recording
// receipts, not GPU execution/completion or proof of an author's dispatch.
typedef void (*WWMI_RtDispatchReceiptFn_v2)(const WWMI_RtNativeDispatch_v2 *,uint64_t context);
typedef struct WWMI_RtForwardCallbacks_v2 {
    uint32_t size,version;
    uint64_t context;
    WWMI_RtBlasAfter_v1 blas_after;
    WWMI_RtTlasInput_v1 tlas_input;
    WWMI_RtHitTable_v1 hit_table;
    WWMI_RtQueueAfter_v1 queue_after;
    WWMI_RtDispatchReceiptFn_v2 dispatch_receipt;
} WWMI_RtForwardCallbacks_v2;
// Uses the existing v1 owner, admission/drain and Unregister_v1 lifecycle.
// v1 registration remains supported unchanged; its dispatch callback is null.
WWMI_RT_FORWARD_API int32_t WWMI_RtForward_Register_v2(const WWMI_RtForwardCallbacks_v2 *);
#ifdef __cplusplus
}
static_assert(sizeof(WWMI_RtNativeDispatch_v2)==72,"native dispatch receipt ABI");
static_assert(sizeof(WWMI_RtForwardCallbacks_v2)==56,"forward v2 ABI");
#endif
