#pragma once
#include "rt_native_dispatch_receipt.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {WWMI_RT_DISPATCH_UNPREDICATED=2};
enum WWMI_RtBufferKind_v3 {WWMI_RT_BUFFER_COPY=1,WWMI_RT_BUFFER_BLAS=2,WWMI_RT_BUFFER_WRITE=3,WWMI_RT_BUFFER_UNKNOWN=4};
enum WWMI_RtBufferPhase_v3 {WWMI_RT_BUFFER_BEFORE_NATIVE=1,WWMI_RT_BUFFER_NATIVE_RECORDED=2};
typedef struct WWMI_RtNativeBuffer_v3 {
    uint32_t size,version,kind,phase;
    uint64_t api_list,native_list,source,destination,source_offset,destination_offset,bytes,build_desc;
} WWMI_RtNativeBuffer_v3;
typedef void (*WWMI_RtBufferReceiptFn_v3)(const WWMI_RtNativeBuffer_v3 *,uint64_t context);
typedef struct WWMI_RtForwardCallbacks_v3 {
    WWMI_RtForwardCallbacks_v2 base;
    WWMI_RtBufferReceiptFn_v3 buffer_receipt;
} WWMI_RtForwardCallbacks_v3;
WWMI_RT_FORWARD_API int32_t WWMI_RtForward_Register_v3(const WWMI_RtForwardCallbacks_v3 *);
// Source is a current native resource from a real creation event, not a GPU
// address cast to COM. mode 1 is legacy read class; mode 2 is an enhanced lease.
// On return 2, reserved may identify this refusal. Zero is an older host
// without detail. It is never an authorization field; success keeps zero.
enum WWMI_RtNativeBufferLeaseReject_v1 {
    WWMI_RT_LEASE_TAG_MISSING=1, WWMI_RT_LEASE_TAG_EPOCH=2, WWMI_RT_LEASE_DOMAIN=3,
    WWMI_RT_LEASE_RECORDING_MISSING=4, WWMI_RT_LEASE_PREDICATION=5,
    WWMI_RT_LEASE_LOCAL_COVERAGE=6, WWMI_RT_LEASE_LOCAL_ACCESS=7,
    WWMI_RT_LEASE_LOCAL_SYNC=8, WWMI_RT_LEASE_LOCAL_MODE=9,
    WWMI_RT_LEASE_ENHANCED_UNPROVEN=10, WWMI_RT_LEASE_MODE_UNKNOWN=11
};
typedef struct WWMI_RtNativeBufferLease_v1 {
    uint32_t size,version,mode,reserved;
    uint64_t sync_after,access_after;
} WWMI_RtNativeBufferLease_v1;
// point: 1 actual compute SRV consumer, 2 actual BLAS input consumer.
WWMI_RT_FORWARD_API int32_t WWMI_RtNativeBufferReadLease_v1(uint64_t api_list,uint64_t native_resource,
    uint64_t offset,uint64_t bytes,uint32_t point,WWMI_RtNativeBufferLease_v1 *out);
#ifdef __cplusplus
}
inline const char *wwmi_rt_native_buffer_lease_rejection(uint32_t reason) noexcept {
    switch(reason){
    case WWMI_RT_LEASE_TAG_MISSING:return "resource_tag_missing";
    case WWMI_RT_LEASE_TAG_EPOCH:return "resource_epoch_invalidated";
    case WWMI_RT_LEASE_DOMAIN:return "resource_domain";
    case WWMI_RT_LEASE_RECORDING_MISSING:return "recording_missing";
    case WWMI_RT_LEASE_PREDICATION:return "predication_unproven";
    case WWMI_RT_LEASE_LOCAL_COVERAGE:return "local_coverage";
    case WWMI_RT_LEASE_LOCAL_ACCESS:return "local_read_access";
    case WWMI_RT_LEASE_LOCAL_SYNC:return "local_read_sync";
    case WWMI_RT_LEASE_LOCAL_MODE:return "local_mode_unknown";
    case WWMI_RT_LEASE_ENHANCED_UNPROVEN:return "enhanced_local_lease_missing";
    case WWMI_RT_LEASE_MODE_UNKNOWN:return "resource_mode_unknown";
    default:return "unspecified";
    }
}
static_assert(sizeof(WWMI_RtNativeBuffer_v3)==80,"native buffer receipt ABI");
static_assert(sizeof(WWMI_RtForwardCallbacks_v3)==64,"forward v3 ABI");
static_assert(sizeof(WWMI_RtNativeBufferLease_v1)==32,"native buffer lease ABI");
#endif
