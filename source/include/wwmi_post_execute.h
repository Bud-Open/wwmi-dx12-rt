#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (__cdecl *WWMI_PostExecuteFn_v1)(
    uint64_t api_queue,
    uint64_t native_queue,
    const uint64_t *api_lists,
    uint32_t list_count,
    uint64_t context);

typedef int32_t (__cdecl *WWMI_RegisterPostExecuteFn_v1)(
    WWMI_PostExecuteFn_v1 callback,
    uint64_t context);

#ifdef __cplusplus
}
#endif
