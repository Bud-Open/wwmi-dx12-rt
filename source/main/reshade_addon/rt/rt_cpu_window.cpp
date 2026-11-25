#include "rt_cpu_window.h"
#include "rt_cpu_profile.hpp"
#include "rt_pack_frame.h"

void RtCpuWindowBegin(std::uint64_t native_device){
    if(!native_device||native_device!=wwmi_rt_pack_frame_device.load(std::memory_order_acquire))return;
    LARGE_INTEGER q{};QueryPerformanceFrequency(&q);rt_cpu::observe(rt_cpu::now()+12ull*q.QuadPart);
}
void RtDescriptorCopyWaitReport();
void RtCpuWindowEnd(){rt_cpu::finish("main",true);RtDescriptorCopyWaitReport();}
extern "C" __declspec(dllexport) std::uint64_t WWMI_RtCpuWindowDeadline_v1(std::uint64_t native_device){
    if(!native_device||native_device!=wwmi_rt_pack_frame_device.load(std::memory_order_acquire))return 0;
    return rt_cpu::deadline.load(std::memory_order_acquire);
}
