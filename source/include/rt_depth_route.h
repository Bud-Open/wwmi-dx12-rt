#pragma once
#include <cstdint>
// Live immutable native PSO: a real VS, no PS/other shader stage, depth write on.
// Caller still proves actual OM binding, mesh inputs, instance and private batch.
bool RtOpaqueDepthPass(std::uint64_t pipeline);
// Independent of color paths 6/7. Existing path 4 remains rejected zero-RTV.
inline constexpr unsigned kRtDepthNativePath=8;
inline constexpr unsigned kRtDepthPrivatePath=9;
