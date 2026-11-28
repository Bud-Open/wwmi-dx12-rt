#pragma once
#include <cstdint>

namespace wwmi_tex_readback {

struct DrainDecision {
    bool drop = false;
    bool ready = false;
    bool release_unsignalled = false;
    bool release_once_retry = false;
};

inline bool FenceValueIsRemoved(uint64_t completed_value) noexcept
{
    return completed_value == UINT64_MAX;
}

// Pure decision policy shared by TexReadback::Drain() and unit tests:
// - Never maps a readback buffer without GPU fence completion proof.
// - Treats confirmed device removal (or UINT64_MAX from GetCompletedValue) as terminal drop, never completion.
// - Never recycles an executed/submitted readback buffer if CreateFence failed; keep it pending so
//   fence creation can be retried on the recorded queue while GPU copy may still be in flight.
// - Releases one-shot `tried_` suppression only on confirmed pre-submit abandonment (or failed Map).
inline DrainDecision EvaluateDrainEntry(
    bool has_fence_value,
    bool fence_completed,
    bool has_queue_fence,
    bool has_exec_ms,
    bool submitted,
    bool list_gen_changed,
    bool once,
    bool dst_dead,
    bool device_removed = false) noexcept
{
    (void)has_queue_fence;
    DrainDecision d{};
    if (device_removed) {
        d.drop = true;
        d.release_unsignalled = !has_fence_value;
        return d;
    }
    if (has_fence_value) {
        if (fence_completed) d.ready = true;
        return d;
    }
    const bool abandoned = !has_exec_ms && !submitted && list_gen_changed;
    if (abandoned) {
        d.drop = true;
        d.release_unsignalled = true;
        d.release_once_retry = once && !dst_dead;
        return d;
    }
    return d;
}

inline bool ShouldSignalConfirmed(bool has_fence_value, bool submitted) noexcept
{
    return !has_fence_value && submitted;
}

inline bool ShouldSignalPreExecuteReset(
    bool post_exec_active, bool has_fence_value, bool same_thread, bool resets_advanced) noexcept
{
    return !post_exec_active && !has_fence_value && same_thread && resets_advanced;
}

inline bool ShouldSignalLateFallback(
    bool has_fence_value, bool submitted, uint64_t exec_ms, uint64_t now_ms,
    bool post_exec_active = false, uint64_t fallback_ms = 1000) noexcept
{
    return !post_exec_active && !has_fence_value && !submitted && exec_ms != 0 && now_ms >= exec_ms &&
           (now_ms - exec_ms) >= fallback_ms;
}

} // namespace wwmi_tex_readback
