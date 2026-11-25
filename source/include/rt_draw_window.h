#pragma once
#include <cstdint>

// Read the existing armed draw window. Time is GetTickCount64 milliseconds,
// not the CPU profiler's QPC deadline. A nonzero sequence is the audited
// graphics call currently invoking the combined previous-surface entry.
using WWMI_RtDrawWindowDeadlineFn_v1=std::uint64_t (*)(std::uint64_t native_device,std::uint64_t *sequence);
extern "C" std::uint64_t WWMI_RtDrawWindowDeadline_v1(std::uint64_t native_device,std::uint64_t *sequence);
