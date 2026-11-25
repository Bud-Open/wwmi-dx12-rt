#pragma once
#include <cstdint>
inline bool SkCb0Range(uint64_t base, uint64_t width, uint64_t va, uint32_t size, uint64_t* off, unsigned* bytes)
{
    if (!base || !width || !size || size > 65536 || va < base) return false;
    const uint64_t len = (uint64_t(size) + 255u) & ~uint64_t(255u);
    const uint64_t pos = va - base;
    if ((pos & 3u) || pos > width || len > width - pos) return false;
    *off = pos; *bytes = static_cast<unsigned>(len); return true;
}
