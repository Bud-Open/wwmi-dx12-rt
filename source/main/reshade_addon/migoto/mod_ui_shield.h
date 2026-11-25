#pragma once
#include <cstdint>

namespace MigotoPort {

// Called when 3Dmigoto evaluates cursor operands (cursor_x/y, cursor_window_x/y, cursor_screen_x/y).
void ModUiNoteCursorQuery() noexcept;

// Called when 3Dmigoto writes an IniParam component (e.g. x87/y87/z87/w87).
void ModUiNoteIniParamWrite(int param_idx, unsigned comp_mask) noexcept;

// Bracket a [Present] pre+post frame that has a live backbuffer so we collect all 2D UI quads
// rendered during this frame and publish them atomically to the host input hook.
void ModUiFrameBegin(bool has_back_buffer) noexcept;
void ModUiNoteBackbufferQuad(float size_x, float size_y, float off_x, float off_y,
    float vp_x, float vp_y, float vp_w, float vp_h,
    float sc_x0, float sc_y0, float sc_x1, float sc_y1,
    uint32_t rt_w, uint32_t rt_h) noexcept;
void ModUiFrameEnd(void* hwnd) noexcept;

// Clear all published shield rects (swapchain destroy / addon unload).
void ModUiClearShield() noexcept;

// RAII thread-local bypass so 3Dmigoto's own VKInputButton polling still sees real mouse buttons.
struct ModUiInputBypassScope {
    ModUiInputBypassScope() noexcept;
    ~ModUiInputBypassScope() noexcept;
    ModUiInputBypassScope(const ModUiInputBypassScope&) = delete;
    ModUiInputBypassScope& operator=(const ModUiInputBypassScope&) = delete;
};

} // namespace MigotoPort
