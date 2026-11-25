// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#ifndef WWMI_RT_FORWARD_API
#define WWMI_RT_FORWARD_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
// Pointers are in-process, borrowed for this synchronous callback only.
// api_* are actual ReShade interface pointers, not proxy COM addresses.
// Zero replacement address means preserve the original native call.
typedef void (*WWMI_RtBlasAfter_v1)(uint64_t api_list,uint64_t native_list,uint64_t native_build_desc,uint64_t context);
typedef uint64_t (*WWMI_RtTlasInput_v1)(uint64_t api_list,uint64_t native_list,uint64_t native_build_desc,uint64_t context);
typedef uint64_t (*WWMI_RtHitTable_v1)(uint64_t api_list,uint64_t native_list,uint64_t native_rays_desc,uint64_t context);
typedef void (*WWMI_RtQueueAfter_v1)(uint64_t api_queue,uint64_t native_queue,uint64_t api_list,uint64_t context);
typedef struct WWMI_RtForwardCallbacks_v1 {
    uint32_t size,version;
    uint64_t context;
    WWMI_RtBlasAfter_v1 blas_after;
    WWMI_RtTlasInput_v1 tlas_input;
    WWMI_RtHitTable_v1 hit_table;
    WWMI_RtQueueAfter_v1 queue_after;
} WWMI_RtForwardCallbacks_v1;
// 0 success, -1 invalid ABI, -2 another owner, -3 recursive lifecycle call.
// Unregister must run outside the loader lock before unloading the client.
// Unregister waits for entered CPU callbacks; the RT owner separately retires GPU resources.
WWMI_RT_FORWARD_API int32_t WWMI_RtForward_Register_v1(const WWMI_RtForwardCallbacks_v1 *callbacks);
WWMI_RT_FORWARD_API int32_t WWMI_RtForward_Unregister_v1(uint64_t context);
#ifdef __cplusplus
}
static_assert(sizeof(WWMI_RtForwardCallbacks_v1)==48,"RT forwarding C ABI");
#endif
