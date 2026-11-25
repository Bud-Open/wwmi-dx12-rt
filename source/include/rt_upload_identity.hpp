#pragma once
#include <cstdint>

namespace wwmi::observe::upload_identity {
inline bool fits(std::uint64_t offset,std::uint64_t size,std::uint64_t width) {
    return offset<=width && size<=width-offset;
}
inline bool overlaps(std::uint64_t a,std::uint64_t an,std::uint64_t b,std::uint64_t bn) {
    if(!an || !bn)return false;
    return a<=b ? b-a<an : a-b<bn;
}
}
