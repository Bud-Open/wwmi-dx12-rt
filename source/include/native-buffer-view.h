#pragma once
#include <stdint.h>
// Valid only inside this host's actual CreateShaderResourceView notification.
struct WWMI_NativeBufferView_v1 {
    uint32_t size,version;
    uint64_t resource,first;
    uint32_t count,stride,format,mapping,flags,reserved;
};
static_assert(sizeof(WWMI_NativeBufferView_v1)==48);
using WWMI_CurrentBufferViewFn_v1=uint32_t (__cdecl *)(uint64_t,uint64_t,WWMI_NativeBufferView_v1*,uint32_t);
