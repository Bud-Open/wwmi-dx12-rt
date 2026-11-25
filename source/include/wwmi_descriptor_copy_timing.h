// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// Optional host export; included after the existing descriptor publisher ABI.
// Both operands are borrowed from the same synchronous copy callback.
// The HRESULT and once semantics are those of the supplied native thunk.
typedef struct WWMI_CopyOrigQpc_v1 {
    uint64_t begin, end; // Windows QPC ticks; 0/0 means no interval was obtained
} WWMI_CopyOrigQpc_v1;
typedef int32_t (*WWMI_DescriptorCopyInvokeTimedFn_v1)(
    WWMI_DescriptorNative_v1 native, void *native_context, WWMI_CopyOrigQpc_v1 *orig);
