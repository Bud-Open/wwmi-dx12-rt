#include "published_descriptor.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include "rt_capture.h"
#include "rt_capture_gate.hpp"
#include "rt_descriptor_source.h"
#include "../addon_state.h"
#include "../mods/ib_hash.h"
#include "../hooks/resource_tracker.h"
#include "../rabbitfx/pso_blend.h"
#include "../config.h"
#include "../../src/log.h"
std::atomic<bool> g_rt_descriptor_observe{false};
namespace {
std::mutex g_gate_mutex;
rt_capture::Gate g_gate;
// bit 0: target claim, bit 1: selected VS written, bit 2: target overflow,
// bit 3: capture dir/VS unavailable. Upper bits: unique section count.
std::atomic<uint32_t> g_state{0};
thread_local bool g_target_focus = false;
void note(uint32_t bits) noexcept { g_state.fetch_or(bits, std::memory_order_release); }
}
uint32_t RtCaptureBegin(const wchar_t *mod_dir, int component, uint32_t count, uint32_t first, int32_t base)
{
    if (!mod_dir || !*mod_dir) return rt_capture::kNoReceipt;
    std::lock_guard lock(g_gate_mutex);
    const uint32_t seq = g_gate.accept(mod_dir, {component, count, first, base});
    if (g_gate.overflow) note(4);
    if (seq != rt_capture::kNoReceipt) g_state.fetch_add(1u << 16, std::memory_order_release);
    return seq;
}
void RtCaptureFailed() noexcept { note(8); }
void RtCaptureFocus(bool active) noexcept { g_target_focus = active; }
void RtCaptureCommit() noexcept { note(1); }
extern "C" __declspec(dllexport) uint32_t __cdecl WWMI_RtTargetFocus_v1() { return g_target_focus ? 1u : 0u; }

// This is an observation of a copied descriptor's origin, not a rendering path.
// RT guard and addon-disabled remain enabled. The separate observation lease
// permits CPU shadow updates only. Never create entries in this getter.
extern "C" __declspec(dllexport) void __cdecl WWMI_RtSetDescriptorObservation_v1(std::uint32_t enabled) noexcept
{
    g_rt_descriptor_observe.store(enabled != 0, std::memory_order_release);
}
extern "C" __declspec(dllexport) std::uint32_t __cdecl WWMI_RtReadDescriptorSource_v1(
    std::uint64_t native_device, std::uint64_t heap, std::uint32_t index,
    WWMI_RtDescriptorSource_v1 *output, std::uint32_t output_bytes) noexcept
{
    if (!output) return 0;
    const bool copied_buffer=output_bytes==sizeof(WWMI_RtDescriptorBuffer_v2);
    if(!copied_buffer&&output_bytes!=sizeof(*output))return 0;
    auto* direct=copied_buffer?reinterpret_cast<WWMI_RtDescriptorBuffer_v2*>(output):nullptr;
    if(direct)*direct={};
    *output={output_bytes,copied_buffer?2u:1u,0,0,0,0};
    if (!native_device || native_device != reinterpret_cast<std::uint64_t>(g_d3d12_device) ||
        !heap || (g_addon_disabled.load(std::memory_order_acquire) &&
                  !g_rt_descriptor_observe.load(std::memory_order_acquire))) return 0;
    return (direct?ReadPublishedBuffer({heap,index},*direct):ReadPublishedLegacyBuffer({heap,index},*output))?1u:0u;
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl WWMI_RtReadCachedVbHash_v1(
    std::uint64_t native_device, std::uint64_t vertex_base,
    WWMI_RtCachedVbHash_v1 *output, std::uint32_t output_bytes) noexcept
{
    if (!output || output_bytes != sizeof(*output)) return 0;
    *output = { sizeof(*output), 1, 0, 0, vertex_base };
    if (!native_device || native_device != reinterpret_cast<std::uint64_t>(g_d3d12_device) ||
        !vertex_base || !g_rt_descriptor_observe.load(std::memory_order_acquire)) return 0;
    return IbHash::ReadCompleted(vertex_base, output->hash, output->index_bytes) ? 1u : 0u;
}

void RtCaptureSection(const rt_section::Input &in, uint32_t seq, bool full_shadow)
{
    if (seq == rt_capture::kNoReceipt) return;
    rt_section::Format(in, seq, [](const char *line) { logger::probe("%s", line); });
    logger::probe("RTINPUT seq=%u target_source=mod_section_folder full_shadow=%u instance_identity=unknown binding_final=unknown", seq, (unsigned)full_shadow);
    wchar_t dir[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"WWMI_RT_CAPTURE_DIR", dir, (DWORD)std::size(dir));
    if (!n || n >= std::size(dir)) {
        note(8); logger::probe("RTINPUT seq=%u shader_role=selected shader_status=missing_capture_directory", seq); return;
    }
    const auto result = PsoCloneCaptureVs(in.pso, in.original_pso, dir, seq);
    char filename[128]{};
    if (result.written) _snprintf_s(filename, sizeof(filename), _TRUNCATE, "wwmi-selected-%02u-%016llx.vs.cso", seq, (unsigned long long)result.hash);
    else strcpy_s(filename, "missing");
    logger::probe("RTINPUT seq=%u shader_role=selected shader_pso=%llx source_pso=%llx proof=%s vs_hash=%016llx bytes=%u shader_status=%s shader_file=%s",
        seq, (unsigned long long)result.pso, (unsigned long long)result.source_pso, result.proof, (unsigned long long)result.hash, result.bytes, result.status, filename);
    if (result.written) note(2); else note(8);
    // No fallback is promoted to selected; absent clone remains explicit.
}
extern "C" __declspec(dllexport) uint32_t __cdecl WWMI_RtTargetState_v1()
{
    const uint32_t state = g_state.load(std::memory_order_acquire);
    return state | (g_rt_blocked.load(std::memory_order_acquire) ? 16u : 0u);
}
