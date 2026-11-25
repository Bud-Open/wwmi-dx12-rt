#pragma once
#include <cstdint>
#include <atomic>
#include "rt_mod_source.h"
// A paired RT client may request pack registration and evaluation on this
// device, independently of the global no-RT-rendering guard.
extern std::atomic<std::uint64_t> wwmi_rt_pack_frame_device;
using WWMI_RtSetPackFrameDeviceFn_v1 = std::uint32_t (__cdecl *)(std::uint64_t device, std::uint32_t enable) noexcept;
