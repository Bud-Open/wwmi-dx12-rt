#pragma once
#include "mods/layout_registry.h"

// Only direct, single-stage root CBVs. Kept alongside the native register
// projection under g_layout_regmap_mtx without changing LayoutRegMap.
struct RtHistoryRoots {
    uint64_t vs_only=0;
    uint8_t ps[3]={0xff,0xff,0xff};
};
inline std::unordered_map<uint64_t,RtHistoryRoots> rt_history_roots;
