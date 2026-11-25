#pragma once
#include <cstdint>

struct HeapKey;
enum class RtPublishedTexture : uint8_t {
    missing, stale, wrong_view, non_texture_2d, unknown, ready
};
struct RtPublishedTextureShape {
    unsigned format=0,width=0,height=0,mips=0,arrays=0;
};

// Original SRV at a physical published slot, with the immutable description
// captured for that same native resource generation. Never queries the GPU
// or dereferences the native resource here.
RtPublishedTexture ReadPublishedTextureShape(const HeapKey&,RtPublishedTextureShape&) noexcept;
