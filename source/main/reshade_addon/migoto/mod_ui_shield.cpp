#include "mod_ui_shield.h"
#include "wwmi_mod_ui_shield.h"
#include "../../src/log.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

extern HMODULE g_wwmi_host_module;

namespace MigotoPort {
namespace {

constexpr uint32_t kMaxShieldRects = 128;
constexpr float    kMinQuadDim = 0.002f;
constexpr float    kFullScreenDim = 0.98f;
constexpr float    kContainEps = 1e-4f;

std::atomic<uint32_t> g_last_published_count{ 0 };

thread_local bool                         t_collecting = false;
thread_local uint8_t                      t_p87_written_mask = 0;
thread_local bool                         t_p87_dirty_since_draw = false;
thread_local std::vector<WWMI_ModUiRect_v1> t_rects;

struct HostShieldApi {
    WWMI_ModUiSetShieldRectsFn_v1 set_rects = nullptr;
    WWMI_ModUiSetInputBypassFn_v1 set_bypass = nullptr;
    bool                          resolved = false;
};

const HostShieldApi& GetHostApi() noexcept
{
    static HostShieldApi s_api;
    if (!s_api.resolved) {
        HMODULE host = g_wwmi_host_module ? g_wwmi_host_module : GetModuleHandleW(L"dxgi.dll");
        if (host) {
            s_api.set_rects = reinterpret_cast<WWMI_ModUiSetShieldRectsFn_v1>(
                GetProcAddress(host, "WWMI_ModUiSetShieldRects_v1"));
            s_api.set_bypass = reinterpret_cast<WWMI_ModUiSetInputBypassFn_v1>(
                GetProcAddress(host, "WWMI_ModUiSetInputBypass_v1"));
            s_api.resolved = true;
            if (s_api.set_rects && s_api.set_bypass) {
                logger::write("MOD-UI-SHIELD: attached to host input hooks");
            }
        }
    }
    return s_api;
}

bool ContainsRect(const WWMI_ModUiRect_v1& outer, const WWMI_ModUiRect_v1& inner) noexcept
{
    return outer.x0 <= inner.x0 + kContainEps &&
           outer.y0 <= inner.y0 + kContainEps &&
           outer.x1 >= inner.x1 - kContainEps &&
           outer.y1 >= inner.y1 - kContainEps;
}

} // namespace

void ModUiNoteCursorQuery() noexcept
{
}

void ModUiNoteIniParamWrite(int param_idx, unsigned comp_mask) noexcept
{
    if (!t_collecting || param_idx != 87 || (comp_mask & 0xFu) == 0) return;
    t_p87_written_mask |= static_cast<uint8_t>(comp_mask & 0xFu);
    t_p87_dirty_since_draw = true;
}

void ModUiFrameBegin(bool has_back_buffer) noexcept
{
    t_collecting = has_back_buffer;
    t_p87_written_mask = 0;
    t_p87_dirty_since_draw = false;
    t_rects.clear();
}

void ModUiNoteBackbufferQuad(float size_x, float size_y, float off_x, float off_y,
    float vp_x, float vp_y, float vp_w, float vp_h,
    float sc_x0, float sc_y0, float sc_x1, float sc_y1,
    uint32_t rt_w, uint32_t rt_h) noexcept
{
    if (!t_collecting) return;
    if ((t_p87_written_mask & 0xFu) != 0xFu || !t_p87_dirty_since_draw) return;
    t_p87_dirty_since_draw = false;

    if (!rt_w || !rt_h || !std::isfinite(vp_x) || !std::isfinite(vp_y) || !std::isfinite(vp_w) || !std::isfinite(vp_h) ||
        vp_w <= 0.0f || vp_h <= 0.0f)
        return;
    if (!std::isfinite(size_x) || !std::isfinite(size_y) || !std::isfinite(off_x) || !std::isfinite(off_y)) return;
    if (!std::isfinite(sc_x0) || !std::isfinite(sc_y0) || !std::isfinite(sc_x1) || !std::isfinite(sc_y1)) return;

    const float u0 = (std::min)(off_x, off_x + size_x);
    const float u1 = (std::max)(off_x, off_x + size_x);
    const float v0 = (std::min)(off_y, off_y + size_y);
    const float v1 = (std::max)(off_y, off_y + size_y);
    if ((u1 - u0) < kMinQuadDim || (v1 - v0) < kMinQuadDim) return;

    const float fw = static_cast<float>(rt_w);
    const float fh = static_cast<float>(rt_h);
    const float px0 = (std::max)({ vp_x + u0 * vp_w, vp_x, sc_x0, 0.0f });
    const float py0 = (std::max)({ vp_y + v0 * vp_h, vp_y, sc_y0, 0.0f });
    const float px1 = (std::min)({ vp_x + u1 * vp_w, vp_x + vp_w, sc_x1, fw });
    const float py1 = (std::min)({ vp_y + v1 * vp_h, vp_y + vp_h, sc_y1, fh });
    if (px1 <= px0 || py1 <= py0) return;

    WWMI_ModUiRect_v1 r;
    r.x0 = px0 / fw;
    r.y0 = py0 / fh;
    r.x1 = px1 / fw;
    r.y1 = py1 / fh;
    const float rw = r.x1 - r.x0;
    const float rh = r.y1 - r.y0;
    if (rw < kMinQuadDim || rh < kMinQuadDim) return;
    if (rw > kFullScreenDim && rh > kFullScreenDim) return;

    for (const auto& existing : t_rects) {
        if (ContainsRect(existing, r)) return;
    }
    t_rects.erase(
        std::remove_if(t_rects.begin(), t_rects.end(),
            [&](const WWMI_ModUiRect_v1& existing) { return ContainsRect(r, existing); }),
        t_rects.end());

    if (t_rects.size() < kMaxShieldRects) {
        t_rects.push_back(r);
    }
}

void ModUiFrameEnd(void* hwnd) noexcept
{
    if (!t_collecting) return;
    t_collecting = false;
    t_p87_written_mask = 0;
    t_p87_dirty_since_draw = false;

    const HostShieldApi& api = GetHostApi();
    if (!api.set_rects) return;

    const uint32_t count = static_cast<uint32_t>(t_rects.size());
    api.set_rects(hwnd, count ? t_rects.data() : nullptr, count);

    const uint32_t prev = g_last_published_count.exchange(count, std::memory_order_relaxed);
    if ((prev == 0) != (count == 0)) {
        if (count > 0) {
            logger::write("MOD-UI-SHIELD: active (%u rect(s), first=[%.3f,%.3f..%.3f,%.3f])",
                count, t_rects[0].x0, t_rects[0].y0, t_rects[0].x1, t_rects[0].y1);
        } else {
            logger::write("MOD-UI-SHIELD: cleared");
        }
    }
}

void ModUiClearShield() noexcept
{
    t_collecting = false;
    t_p87_written_mask = 0;
    t_p87_dirty_since_draw = false;
    t_rects.clear();
    const HostShieldApi& api = GetHostApi();
    if (api.set_rects) api.set_rects(nullptr, nullptr, 0);
    g_last_published_count.store(0, std::memory_order_relaxed);
}

ModUiInputBypassScope::ModUiInputBypassScope() noexcept
{
    const HostShieldApi& api = GetHostApi();
    if (api.set_bypass) api.set_bypass(1u);
}

ModUiInputBypassScope::~ModUiInputBypassScope() noexcept
{
    const HostShieldApi& api = GetHostApi();
    if (api.set_bypass) api.set_bypass(0u);
}

} // namespace MigotoPort
