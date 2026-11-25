#pragma once
#include <cstdint>
#include <limits>

// Preserve the game VB's resource-relative start while exposing the relocated tail.
// This computes only values: callers validate ownership/liveness before invoking it.
struct IaRelocView {
    uint64_t va = 0;
    uint32_t bytes = 0;
    uint64_t offset = 0;
};

inline bool MakeIaRelocView(uint64_t game_base, uint64_t game_width, uint64_t game_va,
    uint32_t game_bytes, uint64_t target_base, uint64_t target_width, IaRelocView& out)
{
    if (!game_base || !game_width || !target_base || !target_width || game_va < game_base ||
        game_base > (std::numeric_limits<uint64_t>::max)() - game_width ||
        target_base > (std::numeric_limits<uint64_t>::max)() - target_width) return false;
    const uint64_t off = game_va - game_base;
    if (off > game_width || game_bytes > game_width - off || off > target_width ||
        game_bytes > target_width - off || target_base > (std::numeric_limits<uint64_t>::max)() - off)
        return false;
    const uint64_t bytes = target_width - off;
    if (!bytes || bytes > (std::numeric_limits<uint32_t>::max)()) return false;
    out = { target_base + off, static_cast<uint32_t>(bytes), off };
    return true;
}
