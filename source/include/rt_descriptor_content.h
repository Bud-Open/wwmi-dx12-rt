#pragma once
#include "rt_descriptor_source.h"

// Negotiated via the existing export's output_bytes. A v1 provider rejects
// this larger size without writing; a v2 provider writes abi_version=2 even
// on a miss. v2 describes the copied buffer itself, not a physical origin.
struct WWMI_RtDescriptorBuffer_v2 {
    WWMI_RtDescriptorSource_v1 base;
    std::uint64_t first;
    std::uint32_t count,format,stride,flags;
};
static_assert(sizeof(WWMI_RtDescriptorBuffer_v2)==56);
