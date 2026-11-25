#pragma once
#include "mods/layout_registry.h"
struct RtTableRange {
    unsigned param;
    reshade::api::descriptor_range desc;
};
inline std::mutex rt_material_layout_mutex;
inline std::unordered_map<uint64_t,std::vector<RtTableRange>> rt_material_layouts;
