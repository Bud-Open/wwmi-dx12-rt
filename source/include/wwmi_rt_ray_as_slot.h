// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct WWMI_ComputeStateInfo_v1;
struct WWMI_AsTableSlot_v1;

// Read one CPU-observed typed AS descriptor for the active RT state in the
// same DispatchRays callback. A Query_v1 rc=2 can supply the current epoch,
// root and pipeline fields, but does not grant restoration or replay rights.
// The original Query/Run/ReadAsTableSlot contracts remain unchanged.
#ifndef WWMI_RAY_AS_API
#define WWMI_RAY_AS_API
#endif
WWMI_RAY_AS_API int32_t WWMI_ComputeState_ReadRayAsSlot_v1(
    uint64_t api_list,const struct WWMI_ComputeStateInfo_v1 *observed,
    uint32_t parameter,uint32_t table_offset,struct WWMI_AsTableSlot_v1 *out);

#ifdef __cplusplus
}
#endif
