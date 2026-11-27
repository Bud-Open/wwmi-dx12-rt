#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WWMI_D3D12UploadFootprint_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t command_list;
    uint64_t src_resource;
    uint64_t dst_resource;
    uint32_t dst_subresource;
    uint32_t row_pitch;
    uint32_t footprint_width;
    uint32_t footprint_height;
    uint32_t footprint_depth;
    uint32_t src_left;
    uint32_t src_top;
    uint32_t src_front;
    uint32_t has_src_box;
    uint32_t reserved;
} WWMI_D3D12UploadFootprint_v1;

typedef uint32_t (__cdecl *WWMI_CurrentUploadFootprintFn_v1)(
    uint64_t command_list, uint64_t src_resource, uint64_t dst_resource,
    uint32_t dst_subresource, WWMI_D3D12UploadFootprint_v1 *out, uint32_t bytes);

#ifdef __cplusplus
}
#endif
