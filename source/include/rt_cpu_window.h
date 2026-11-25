#pragma once
#include <cstdint>

// Optional paired-provider input. QPC ticks, zero means not armed/not paired.
// No C++ storage, resources or callbacks cross this ABI. The existing provider
// module lease covers every use of this function by the client.
using WWMI_RtCpuWindowDeadlineFn_v1=std::uint64_t (*)(std::uint64_t native_device);
extern "C" std::uint64_t WWMI_RtCpuWindowDeadline_v1(std::uint64_t native_device);

// Main only: call at the EXISTING RT-VISIBLE 12-second window boundaries.
// End logs a bounded thread/category aggregate, never waits for callbacks.
void RtCpuWindowBegin(std::uint64_t native_device);
void RtCpuWindowEnd();
