#pragma once
#include <cstdint>
#include <reshade.hpp>

// One narrowly typed input: the actual copied R32_FLOAT buffer SRV. All units
// are bytes. resource is a borrowed native resource identity, not a new COM
// reference: it is checked against observed resource/heap lifetimes at lookup.
// Use it immediately for the existing previous-pose query; do not dereference
// it or chase an original CPU descriptor. The real game draw owns its lifetime.
struct RtPreviousView {
    std::uint64_t resource=0,byte_offset=0,bytes=0;
    // Cache-local observation generations, not the client's resource counter.
    std::uint64_t resource_generation=0,heap_generation=0,write_generation=0;
};
// Missing coverage is not proof of overwrite. source_same_copy means the
// original source still passed destination copy-seq, resource creation epoch,
// width/range and destination write-generation checks. invalidated never falls back.
enum class RtPreviousReadResult : std::uint8_t {
    copied, source_same_copy, uncovered, source_unavailable, invalidated,
    not_paired, heap_unavailable
};
RtPreviousReadResult RtReadPreviousView(reshade::api::device* device,std::uint64_t heap,
    std::uint32_t index,RtPreviousView& out) noexcept;
// Compatibility entry: true only for copied or source_same_copy. Only the paired
// native D3D12 device and a live shader-visible destination can succeed.
// CPU source-slot reuse does not invalidate an independently copied destination.
// No D3D command, resource mutation, descriptor creation or fallback is performed.
bool RtReadCopiedPreviousView(reshade::api::device* device,std::uint64_t heap,
    std::uint32_t index,RtPreviousView& out) noexcept;
