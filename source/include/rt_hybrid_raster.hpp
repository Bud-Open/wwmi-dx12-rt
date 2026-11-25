#pragma once
#include <cstdint>
#include "rt_draw_frame.h"
#include "rt_author_draw.h"

// Synchronous main/client contract. Byte pointers are never retained.
namespace wwmi::hybrid {
enum AssetStream : unsigned { positions, blends, indices, vectors, uv, color, shape_ids,
                             full_vg, forward, reverse, stream_count };
struct Bytes { const void* data=nullptr; std::uint64_t size=0; unsigned stride=0; };
struct BoneWindow { unsigned component=0, first=0, count=0; };
struct Asset {
    std::uint64_t load=0, mod=0, version=0;
    unsigned vertices=0, shapes=0;
    const BoneWindow* bones=nullptr;
    unsigned bone_windows=0;
    Bytes streams[stream_count]{};
};
enum Blend : unsigned { raw, paired_remap };
struct Part {
    unsigned count=0,first=0; int base=0;
    Blend blend=raw;
    unsigned remap_row=0, remap_count=0;
};
enum Result : unsigned { unavailable, exact, base_skin, previous_missing, empty_selection };
struct HistoryRead { std::uint64_t resource=0, offset=0, bytes=0; };
struct Request {
    WWMI_RtPackDrawQuery_v2 native{};
    std::uint64_t ticket=0;
    WWMI_RtAuthorInstance_v1 instance{};
    const Asset* asset=nullptr;
    const Part* parts=nullptr;
    unsigned count=0;
    bool allow_base=false, need_material=false, need_color=false, need_previous=false;
};
struct Frame {
    WWMI_RtLocalVfSurface_v1 current{};
    WWMI_RtPreviousSurface_v1 previous{};
    std::uint64_t transaction=0;
    Result mode=unavailable;
};
using Probe=bool(__cdecl*)(const WWMI_RtPackDrawQuery_v2*, std::uint64_t, std::uint64_t, std::uint64_t, const HistoryRead*);
using Acquire=Result(__cdecl*)(const Request*, Frame*);
using Commit=bool(__cdecl*)(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t);
}
