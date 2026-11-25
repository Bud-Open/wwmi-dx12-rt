#pragma once
#include "rt_author_draw.h"
#include "rt_shape_final_source.h"

#ifdef __cplusplus
extern "C" {
#endif
// Pre-VM, this only identifies the consumer and exact native write. A
// successful Begin does not promise a selected author source.
typedef struct WWMI_RtAuthorDeformedScope_v2 {
    WWMI_RtAuthorScope_v1 base;
    WWMI_RtDeformedSelection_v1 owner;
} WWMI_RtAuthorDeformedScope_v2;

typedef struct WWMI_RtAuthorDeformedFrameRequest_v2 {
    WWMI_RtAuthorFrameRequest_v1 base;
    WWMI_RtDeformedSelection_v1 selected;
} WWMI_RtAuthorDeformedFrameRequest_v2;

typedef uint32_t (*WWMI_RtPackBeginAuthorScopeFn_v2)(const WWMI_RtAuthorScopeRequest_v1 *,uint32_t,
    WWMI_RtAuthorDeformedScope_v2 *,uint32_t);
typedef uint32_t (*WWMI_RtPackAcquireAuthorFrameFn_v2)(const WWMI_RtAuthorDeformedFrameRequest_v2 *,uint32_t,
    WWMI_RtAuthorFrame_v1 *,uint32_t);
uint32_t WWMI_RtPackBeginAuthorScope_v2(const WWMI_RtAuthorScopeRequest_v1 *,uint32_t,
    WWMI_RtAuthorDeformedScope_v2 *,uint32_t);
uint32_t WWMI_RtPackAcquireAuthorFrame_v2(const WWMI_RtAuthorDeformedFrameRequest_v2 *,uint32_t,
    WWMI_RtAuthorFrame_v1 *,uint32_t);
#ifdef __cplusplus
}
#endif
