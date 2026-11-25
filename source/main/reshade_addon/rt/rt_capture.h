#pragma once
#include <cstdint>
#include <atomic>
#include "rt_section_input.hpp"
// Enabled only while the paired diagnostic observer needs CPU descriptor origins.
extern std::atomic<bool> g_rt_descriptor_observe;
// Read-only, bounded diagnostics. No D3D command, resource mutation or RT adaptation.
uint32_t RtCaptureBegin(const wchar_t *mod_dir, int component, uint32_t count, uint32_t first, int32_t base);
void RtCaptureSection(const rt_section::Input &in, uint32_t seq, bool full_shadow);
void RtCaptureFailed() noexcept;
void RtCaptureFocus(bool active) noexcept;
void RtCaptureCommit() noexcept;
struct RtTargetFocusScope {
    explicit RtTargetFocusScope(bool active) noexcept : active_(active) { if (active_) RtCaptureFocus(true); }
    ~RtTargetFocusScope() { if (active_) RtCaptureFocus(false); }
    bool active_;
};
extern "C" __declspec(dllexport) uint32_t __cdecl WWMI_RtTargetFocus_v1();
extern "C" __declspec(dllexport) uint32_t __cdecl WWMI_RtTargetState_v1();
