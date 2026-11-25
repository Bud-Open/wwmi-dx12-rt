#pragma once
#include <guiddef.h>
#include <stdint.h>

namespace wwmi::rt_native_root_flags {
// Publish only on the actual native root after its successful creation
// from the exact observed serialized signature, before the init event.
inline constexpr GUID private_data_guid{0x7746d79b,0x548a,0x40d3,{0xa6,0x45,0x88,0xd8,0x9b,0x2b,0x5d,0xec}};
inline constexpr uint32_t version_v1=1;
inline constexpr uint32_t direct_heap_indexing=0x400u|0x800u;
struct Flags_v1 {
    uint32_t size,version,flags,reserved;
};
static_assert(sizeof(Flags_v1)==16);
}
