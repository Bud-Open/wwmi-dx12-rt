#pragma once
#include <cstdint>

// Read-only source provenance from the existing descriptor shadow. No COM or
// C++ aggregate crosses this boundary; the caller owns the output storage.
#pragma pack(push, 8)
struct WWMI_RtDescriptorSource_v1 {
    std::uint32_t struct_size, abi_version;
    std::uint64_t resource, source_view;
    std::uint32_t wrote_frame, wrote_source;
};
#pragma pack(pop)
static_assert(sizeof(WWMI_RtDescriptorSource_v1) == 32);
#ifdef _WIN32
#define WWMI_RT_SOURCE_CALL __cdecl
#else
#define WWMI_RT_SOURCE_CALL
#endif
using WWMI_RtReadDescriptorSourceFn_v1 = std::uint32_t (WWMI_RT_SOURCE_CALL *)(
    std::uint64_t native_device, std::uint64_t heap, std::uint32_t index,
    WWMI_RtDescriptorSource_v1 *output, std::uint32_t output_bytes) noexcept;
using WWMI_RtSetDescriptorObservationFn_v1 = void (WWMI_RT_SOURCE_CALL *)(std::uint32_t enabled) noexcept;

#pragma pack(push, 8)
struct WWMI_RtCachedVbHash_v1 {
    std::uint32_t struct_size, abi_version, hash, index_bytes;
    std::uint64_t vertex_base;
};
#pragma pack(pop)
static_assert(sizeof(WWMI_RtCachedVbHash_v1) == 24);
using WWMI_RtReadCachedVbHashFn_v1 = std::uint32_t (WWMI_RT_SOURCE_CALL *)(
    std::uint64_t native_device, std::uint64_t vertex_base,
    WWMI_RtCachedVbHash_v1 *output, std::uint32_t output_bytes) noexcept;
