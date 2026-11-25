#pragma once
#include <guiddef.h>
#include <stdint.h>

namespace wwmi::rt_native_no_pixel {
// A live ID3D12PipelineState private datum, not a replay/replace permission.
// Set only after successful native Graphics/stream creation, where the
// complete stream was consumed, has a VS and has no PS (not unreported).
inline constexpr GUID key{0x27e690c5,0x6390,0x4472,{0xa5,0xc8,0x39,0x5b,0xee,0x6e,0x2d,0xd4}};
inline constexpr uint32_t version_v1=1;
}
