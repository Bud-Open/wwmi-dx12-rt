#pragma once
#include <cstdint>
// Query from a draw callback while the game-bound ReShade api::pipeline.handle
// is live. No borrowed bytes, shader dumping, GPU commands, or PSO creation.
struct WWMI_RtIaElement_v1 {
    char semantic[64];
    std::uint32_t semantic_index, format, slot, offset, slot_class, step;
};
enum : std::uint32_t {
    WWMI_GFX_HAS_VS=1, WWMI_GFX_HAS_IA=2, WWMI_GFX_VS_COPIED=4,
    // ReShade's D3D12 -> SDK conversion retains the step but drops native
    // InputSlotClass. A zero step does not disambiguate the two native classes.
    WWMI_GFX_ZERO_STEP_CLASS_UNKNOWN=0xffffffffU
};
struct WWMI_RtGraphicsPipeline_v1 {
    std::uint32_t size, version, flags, vs_bytes;
    std::uint64_t vs_hash;
    std::uint32_t ia_count, reserved;
    WWMI_RtIaElement_v1 ia[32];
};
static_assert(sizeof(WWMI_RtIaElement_v1)==88 && sizeof(WWMI_RtGraphicsPipeline_v1)==2848);
using WWMI_RtReadGraphicsPipelineFn_v1=std::uint32_t (__cdecl *)(
    std::uint64_t pipeline, WWMI_RtGraphicsPipeline_v1 *info, std::uint32_t info_bytes,
    void *vs_out, std::uint32_t vs_capacity) noexcept;
