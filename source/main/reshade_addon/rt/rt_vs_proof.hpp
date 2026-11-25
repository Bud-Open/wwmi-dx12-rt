#pragma once
#include <cstdint>
namespace rt_vs_proof {
inline bool exact_variant(uint64_t selected,uint64_t original,uint64_t variant,uint64_t source) noexcept {
    return selected && original && selected==variant && original==source;
}
inline bool direct(uint64_t selected,uint64_t original) noexcept {return selected && selected==original;}
}
