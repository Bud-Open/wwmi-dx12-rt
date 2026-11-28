#pragma once
// ===========================================================================
//  client_lock_stats.hpp -- Sampled-window RT-CLIENT-LOCK attribution and
//  single-64-bit coherent observed-owner tracking for zz_wwmi_rt_pack.
//
//  Platform-independent C++20 header directly #include-able by both
//  production client code (`runtime_boundaries.hpp`, `hsin_runtime.cpp`)
//  and Linux host unit tests.
// ===========================================================================
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace hsin_rt {

enum class ClientLockOp : std::uint8_t {
    none = 0,
    present,
    init_device,
    destroy_device,
    init_list,
    reset_list,
    destroy_list,
    close_list,
    init_layout,
    destroy_layout,
    init_pipeline,
    destroy_pipeline,
    bind_pipeline,
    bind_tables,
    push_descriptors,
    copy_buffer,
    dispatch,
    execute,
    note_ray_consumer,
    forward_blas,
    forward_instances,
    observe_native_tlas,
    observe_native_raygen,
    forward_hit,
    forward_queue,
    passive_shape_dispatch,
    native_skin_receipt,
    native_dispatch_probe,
    native_dispatch_timer,
    native_blas_receipt,
    native_buffer_copy_before,
    native_buffer_copy_rec,
    native_buffer_write,
    native_buffer_unknown,
    api_identify,
    api_aux_skip,
    api_geometry,
    api_palette,
    api_hybrid_probe,
    api_hybrid_acquire,
    api_hybrid_commit,
    api_surface,
    api_previous,
    api_draw_frame,
    api_author_snapshot,
    api_author_palette,
    api_author_frame,
    api_author_scope,
    api_author_commit,
    api_observe_section,
    api_reserve_binding,
    other,
    count
};

static_assert(static_cast<std::size_t>(ClientLockOp::count) <= 63, "ClientLockOp must fit in 6 bits");

inline constexpr const char* client_lock_op_name(ClientLockOp op) noexcept
{
    constexpr const char* names[]{
        "none",
        "present",
        "init_device",
        "destroy_device",
        "init_list",
        "reset_list",
        "destroy_list",
        "close_list",
        "init_layout",
        "destroy_layout",
        "init_pipeline",
        "destroy_pipeline",
        "bind_pipeline",
        "bind_tables",
        "push_descriptors",
        "copy_buffer",
        "dispatch",
        "execute",
        "note_ray_consumer",
        "forward_blas",
        "forward_instances",
        "observe_native_tlas",
        "observe_native_raygen",
        "forward_hit",
        "forward_queue",
        "passive_shape_dispatch",
        "native_skin_receipt",
        "native_dispatch_probe",
        "native_dispatch_timer",
        "native_blas_receipt",
        "native_buffer_copy_before",
        "native_buffer_copy_rec",
        "native_buffer_write",
        "native_buffer_unknown",
        "api_identify",
        "api_aux_skip",
        "api_geometry",
        "api_palette",
        "api_hybrid_probe",
        "api_hybrid_acquire",
        "api_hybrid_commit",
        "api_surface",
        "api_previous",
        "api_draw_frame",
        "api_author_snapshot",
        "api_author_palette",
        "api_author_frame",
        "api_author_scope",
        "api_author_commit",
        "api_observe_section",
        "api_reserve_binding",
        "other"
    };
    const auto idx = static_cast<std::size_t>(op);
    return idx < std::size(names) ? names[idx] : "other";
}

inline ClientLockOp client_lock_op_from_name(const char* name) noexcept
{
    if (!name || !*name) return ClientLockOp::other;
    if (!std::strcmp(name, "present")) return ClientLockOp::present;
    if (!std::strcmp(name, "init_device")) return ClientLockOp::init_device;
    if (!std::strcmp(name, "destroy_device")) return ClientLockOp::destroy_device;
    if (!std::strcmp(name, "init_list")) return ClientLockOp::init_list;
    if (!std::strcmp(name, "reset_list")) return ClientLockOp::reset_list;
    if (!std::strcmp(name, "destroy_list")) return ClientLockOp::destroy_list;
    if (!std::strcmp(name, "close_list")) return ClientLockOp::close_list;
    if (!std::strcmp(name, "init_layout")) return ClientLockOp::init_layout;
    if (!std::strcmp(name, "destroy_layout")) return ClientLockOp::destroy_layout;
    if (!std::strcmp(name, "init_pipeline")) return ClientLockOp::init_pipeline;
    if (!std::strcmp(name, "destroy_pipeline")) return ClientLockOp::destroy_pipeline;
    if (!std::strcmp(name, "bind_pipeline")) return ClientLockOp::bind_pipeline;
    if (!std::strcmp(name, "bind_tables")) return ClientLockOp::bind_tables;
    if (!std::strcmp(name, "push_descriptors")) return ClientLockOp::push_descriptors;
    if (!std::strcmp(name, "copy_buffer")) return ClientLockOp::copy_buffer;
    if (!std::strcmp(name, "dispatch")) return ClientLockOp::dispatch;
    if (!std::strcmp(name, "execute")) return ClientLockOp::execute;
    if (!std::strcmp(name, "note_ray_consumer")) return ClientLockOp::note_ray_consumer;
    if (!std::strcmp(name, "forward_blas")) return ClientLockOp::forward_blas;
    if (!std::strcmp(name, "forward_instances")) return ClientLockOp::forward_instances;
    if (!std::strcmp(name, "observe_native_tlas")) return ClientLockOp::observe_native_tlas;
    if (!std::strcmp(name, "observe_native_raygen")) return ClientLockOp::observe_native_raygen;
    if (!std::strcmp(name, "forward_hit")) return ClientLockOp::forward_hit;
    if (!std::strcmp(name, "forward_queue")) return ClientLockOp::forward_queue;
    if (!std::strcmp(name, "passive_shape_dispatch")) return ClientLockOp::passive_shape_dispatch;
    if (!std::strcmp(name, "native_skin_receipt")) return ClientLockOp::native_skin_receipt;
    if (!std::strcmp(name, "native_dispatch_receipt")) return ClientLockOp::native_dispatch_probe;
    if (!std::strcmp(name, "native_dispatch_timer")) return ClientLockOp::native_dispatch_timer;
    if (!std::strcmp(name, "native_blas_receipt")) return ClientLockOp::native_blas_receipt;
    if (!std::strcmp(name, "native_buffer_copy_before")) return ClientLockOp::native_buffer_copy_before;
    if (!std::strcmp(name, "native_buffer_copy_rec")) return ClientLockOp::native_buffer_copy_rec;
    if (!std::strcmp(name, "native_buffer_write")) return ClientLockOp::native_buffer_write;
    if (!std::strcmp(name, "native_buffer_unknown")) return ClientLockOp::native_buffer_unknown;
    if (std::strstr(name, "IdentifyAuxSkipDraw")) return ClientLockOp::api_aux_skip;
    if (std::strstr(name, "IdentifyDraw")) return ClientLockOp::api_identify;
    if (std::strstr(name, "GraphicsGeometry") || std::strstr(name, "PlannedGeometry")) return ClientLockOp::api_geometry;
    if (std::strstr(name, "NativePalette")) return ClientLockOp::api_palette;
    if (std::strstr(name, "ProbeHybridRaster")) return ClientLockOp::api_hybrid_probe;
    if (std::strstr(name, "AcquireHybridRaster")) return ClientLockOp::api_hybrid_acquire;
    if (std::strstr(name, "CommitHybridRaster")) return ClientLockOp::api_hybrid_commit;
    if (std::strstr(name, "PreviousSurface")) return ClientLockOp::api_previous;
    if (std::strstr(name, "LocalVfSurface")) return ClientLockOp::api_surface;
    if (std::strstr(name, "AcquireDrawFrame")) return ClientLockOp::api_draw_frame;
    if (std::strstr(name, "ReadAuthorSnapshot")) return ClientLockOp::api_author_snapshot;
    if (std::strstr(name, "AuthorPalette")) return ClientLockOp::api_author_palette;
    if (std::strstr(name, "AcquireAuthorFrame")) return ClientLockOp::api_author_frame;
    if (std::strstr(name, "BeginAuthorScope")) return ClientLockOp::api_author_scope;
    if (std::strstr(name, "CommitAuthorDraw")) return ClientLockOp::api_author_commit;
    if (std::strstr(name, "ObserveAuthorSection")) return ClientLockOp::api_observe_section;
    if (std::strstr(name, "ReserveDrawBinding")) return ClientLockOp::api_reserve_binding;
    return ClientLockOp::other;
}

// Single-64-bit coherent observed owner pack:
// - bits [0..5]   (6 bits):  ClientLockOp (1..63; 0 = none/unlocked)
// - bits [6..13]  (8 bits):  window_id    (1..255; 0 = inactive)
// - bits [14..63] (50 bits): relative QPC ticks since window_start (wraps >3.5y @ 10MHz)
struct ObservedOwnerCodec {
    static constexpr std::uint64_t kOpMask = 0x3Full;
    static constexpr std::uint64_t kWinMask = 0xFFull;
    static constexpr unsigned      kWinShift = 6;
    static constexpr unsigned      kRelShift = 14;
    static constexpr std::uint64_t kRelMask = (1ull << 50) - 1ull;

    static constexpr std::uint64_t pack(std::uint8_t window_id, ClientLockOp op, std::uint64_t rel_ticks) noexcept
    {
        if (!window_id || op == ClientLockOp::none) return 0;
        return ((rel_ticks & kRelMask) << kRelShift) |
               ((static_cast<std::uint64_t>(window_id) & kWinMask) << kWinShift) |
               (static_cast<std::uint64_t>(op) & kOpMask);
    }

    static constexpr ClientLockOp unpack_op(std::uint64_t packed) noexcept
    {
        const auto raw = static_cast<std::uint8_t>(packed & kOpMask);
        return raw < static_cast<std::uint8_t>(ClientLockOp::count) ? static_cast<ClientLockOp>(raw) : ClientLockOp::other;
    }

    static constexpr std::uint8_t unpack_window(std::uint64_t packed) noexcept
    {
        return static_cast<std::uint8_t>((packed >> kWinShift) & kWinMask);
    }

    static constexpr std::uint64_t unpack_rel(std::uint64_t packed) noexcept
    {
        return (packed >> kRelShift) & kRelMask;
    }
};

struct SlowWaitSample {
    ClientLockOp  waiter = ClientLockOp::none;
    ClientLockOp  obs_owner = ClientLockOp::none;
    std::uint64_t wait_ticks = 0;
    std::uint64_t owner_held_ticks = 0;
};

class ClientLockTracker {
public:
    static constexpr std::size_t kOpCount = static_cast<std::size_t>(ClientLockOp::count);
    static constexpr std::size_t kMaxSlowWaits = 8;

    struct OpRow {
        std::atomic<std::uint64_t> calls{ 0 };
        std::atomic<std::uint64_t> wait_ticks{ 0 };
        std::atomic<std::uint64_t> hold_ticks{ 0 };
        std::atomic<std::uint64_t> max_wait_ticks{ 0 };
        std::atomic<std::uint64_t> max_hold_ticks{ 0 };
        std::atomic<std::uint64_t> blamed_wait_ticks{ 0 };
        std::atomic<std::uint64_t> blamed_count{ 0 };
    };

    struct OpSnapshot {
        ClientLockOp  op = ClientLockOp::none;
        std::uint64_t calls = 0;
        std::uint64_t wait_ticks = 0;
        std::uint64_t hold_ticks = 0;
        std::uint64_t max_wait_ticks = 0;
        std::uint64_t max_hold_ticks = 0;
        std::uint64_t blamed_wait_ticks = 0;
        std::uint64_t blamed_count = 0;
    };

    struct SummarySnapshot {
        std::uint64_t               frequency = 0;
        std::uint64_t               devs_calls = 0;
        std::uint64_t               devs_ticks = 0;
        std::uint64_t               devs_max_ticks = 0;
        std::uint64_t               copy_bypass_before = 0;
        std::uint64_t               copy_bypass_rec = 0;
        std::uint64_t               empty_tables_bypass = 0;
        std::uint64_t               sdk_dispatch_bypass = 0;
        std::vector<OpSnapshot>     ops;
        std::vector<SlowWaitSample> slow_waits;
    };

private:
    std::atomic<std::uint8_t>  window_id_{ 0 }; // 0 = unarmed
    std::atomic<bool>          emitted_{ false };
    std::atomic<std::uint64_t> start_ticks_{ 0 };
    std::atomic<std::uint64_t> deadline_ticks_{ 0 };
    std::atomic<std::uint64_t> frequency_{ 0 };
    std::atomic<std::uint64_t> blame_threshold_ticks_{ 0 }; // ~10 us
    std::atomic<std::uint64_t> slow_threshold_ticks_{ 0 };  // ~250 us

    std::array<OpRow, kOpCount> ops_{};
    std::atomic<std::uint64_t>  devs_calls_{ 0 };
    std::atomic<std::uint64_t>  devs_ticks_{ 0 };
    std::atomic<std::uint64_t>  devs_max_ticks_{ 0 };
    std::atomic<std::uint64_t>  copy_bypass_before_{ 0 };
    std::atomic<std::uint64_t>  copy_bypass_rec_{ 0 };
    std::atomic<std::uint64_t>  empty_tables_bypass_{ 0 };
    std::atomic<std::uint64_t>  sdk_dispatch_bypass_{ 0 };

    mutable std::mutex                             slow_mtx_;
    std::array<SlowWaitSample, kMaxSlowWaits>      slow_waits_{};
    std::size_t                                    slow_count_{ 0 };

    static void atomic_max(std::atomic<std::uint64_t>& dst, std::uint64_t v) noexcept
    {
        auto cur = dst.load(std::memory_order_relaxed);
        while (cur < v && !dst.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }

public:
    void arm_once(std::uint64_t start_ticks, std::uint64_t deadline_ticks, std::uint64_t freq) noexcept
    {
        if (!freq || !deadline_ticks) return;
        if (window_id_.load(std::memory_order_acquire) != 0) return;
        std::uint64_t zero = 0;
        if (frequency_.compare_exchange_strong(zero, freq, std::memory_order_acq_rel)) {
            start_ticks_.store(start_ticks, std::memory_order_relaxed);
            deadline_ticks_.store(deadline_ticks, std::memory_order_relaxed);
            blame_threshold_ticks_.store((std::max<std::uint64_t>)(1, freq / 100000), std::memory_order_relaxed);
            slow_threshold_ticks_.store((std::max<std::uint64_t>)(1, freq / 4000), std::memory_order_relaxed);
            window_id_.store(1, std::memory_order_release);
        }
    }

    void reset_for_test(std::uint8_t next_window_id, std::uint64_t start_ticks, std::uint64_t deadline_ticks, std::uint64_t freq) noexcept
    {
        window_id_.store(0, std::memory_order_release);
        emitted_.store(false, std::memory_order_relaxed);
        for (auto& r : ops_) {
            r.calls.store(0, std::memory_order_relaxed);
            r.wait_ticks.store(0, std::memory_order_relaxed);
            r.hold_ticks.store(0, std::memory_order_relaxed);
            r.max_wait_ticks.store(0, std::memory_order_relaxed);
            r.max_hold_ticks.store(0, std::memory_order_relaxed);
            r.blamed_wait_ticks.store(0, std::memory_order_relaxed);
            r.blamed_count.store(0, std::memory_order_relaxed);
        }
        devs_calls_.store(0, std::memory_order_relaxed);
        devs_ticks_.store(0, std::memory_order_relaxed);
        devs_max_ticks_.store(0, std::memory_order_relaxed);
        copy_bypass_before_.store(0, std::memory_order_relaxed);
        copy_bypass_rec_.store(0, std::memory_order_relaxed);
        empty_tables_bypass_.store(0, std::memory_order_relaxed);
        sdk_dispatch_bypass_.store(0, std::memory_order_relaxed);
        {
            std::lock_guard lk(slow_mtx_);
            slow_waits_ = {};
            slow_count_ = 0;
        }
        start_ticks_.store(start_ticks, std::memory_order_relaxed);
        deadline_ticks_.store(deadline_ticks, std::memory_order_relaxed);
        frequency_.store(freq, std::memory_order_relaxed);
        blame_threshold_ticks_.store(freq ? (std::max<std::uint64_t>)(1, freq / 100000) : std::uint64_t{ 1 }, std::memory_order_relaxed);
        slow_threshold_ticks_.store(freq ? (std::max<std::uint64_t>)(1, freq / 4000) : std::uint64_t{ 1 }, std::memory_order_relaxed);
        window_id_.store(next_window_id, std::memory_order_release);
    }

    std::uint8_t active_window(std::uint64_t now_ticks) const noexcept
    {
        const auto wid = window_id_.load(std::memory_order_acquire);
        if (!wid) return 0;
        const auto dl = deadline_ticks_.load(std::memory_order_relaxed);
        return (dl && now_ticks < dl) ? wid : 0;
    }

    std::uint8_t window_id() const noexcept
    {
        return window_id_.load(std::memory_order_acquire);
    }

    std::uint64_t relative_ticks(std::uint64_t now_ticks) const noexcept
    {
        const auto st = start_ticks_.load(std::memory_order_relaxed);
        return now_ticks > st ? (now_ticks - st) : 0;
    }

    std::uint64_t clamp_to_deadline(std::uint64_t ticks) const noexcept
    {
        const auto dl = deadline_ticks_.load(std::memory_order_relaxed);
        return (dl && ticks > dl) ? dl : ticks;
    }

    void record_lock_sample(ClientLockOp op, std::uint8_t sampled_wid, std::uint64_t wait_ticks, std::uint64_t hold_ticks,
        std::uint64_t obs_owner_packed, std::uint64_t acq_rel_ticks) noexcept
    {
        if (!sampled_wid) return;
        const auto idx = static_cast<std::size_t>(op);
        if (idx >= kOpCount) return;
        auto& row = ops_[idx];
        row.calls.fetch_add(1, std::memory_order_relaxed);
        row.wait_ticks.fetch_add(wait_ticks, std::memory_order_relaxed);
        row.hold_ticks.fetch_add(hold_ticks, std::memory_order_relaxed);
        atomic_max(row.max_wait_ticks, wait_ticks);
        atomic_max(row.max_hold_ticks, hold_ticks);

        ClientLockOp  owner_op = ClientLockOp::none;
        std::uint64_t owner_held = 0;
        if (obs_owner_packed != 0 && ObservedOwnerCodec::unpack_window(obs_owner_packed) == sampled_wid) {
            owner_op = ObservedOwnerCodec::unpack_op(obs_owner_packed);
            const auto owner_rel = ObservedOwnerCodec::unpack_rel(obs_owner_packed);
            owner_held = acq_rel_ticks > owner_rel ? (acq_rel_ticks - owner_rel) : 0;
        }

        const auto blame_th = blame_threshold_ticks_.load(std::memory_order_relaxed);
        if (owner_op != ClientLockOp::none && wait_ticks >= blame_th) {
            auto& orow = ops_[static_cast<std::size_t>(owner_op)];
            orow.blamed_wait_ticks.fetch_add(wait_ticks, std::memory_order_relaxed);
            orow.blamed_count.fetch_add(1, std::memory_order_relaxed);
        }

        const auto slow_th = slow_threshold_ticks_.load(std::memory_order_relaxed);
        if (wait_ticks >= slow_th) {
            std::lock_guard lk(slow_mtx_);
            const SlowWaitSample cand{ op, owner_op, wait_ticks, owner_held };
            if (slow_count_ < kMaxSlowWaits) {
                slow_waits_[slow_count_++] = cand;
            } else {
                std::size_t min_i = 0;
                for (std::size_t i = 1; i < kMaxSlowWaits; ++i)
                    if (slow_waits_[i].wait_ticks < slow_waits_[min_i].wait_ticks) min_i = i;
                if (wait_ticks > slow_waits_[min_i].wait_ticks) slow_waits_[min_i] = cand;
            }
        }
    }

    void record_devices_lookup(std::uint64_t ticks) noexcept
    {
        if (!window_id_.load(std::memory_order_relaxed)) return;
        devs_calls_.fetch_add(1, std::memory_order_relaxed);
        devs_ticks_.fetch_add(ticks, std::memory_order_relaxed);
        atomic_max(devs_max_ticks_, ticks);
    }

    void note_copy_bypass(bool recorded) noexcept
    {
        if (!window_id_.load(std::memory_order_relaxed)) return;
        if (recorded) copy_bypass_rec_.fetch_add(1, std::memory_order_relaxed);
        else copy_bypass_before_.fetch_add(1, std::memory_order_relaxed);
    }

    void note_empty_tables_bypass() noexcept
    {
        if (!window_id_.load(std::memory_order_relaxed)) return;
        empty_tables_bypass_.fetch_add(1, std::memory_order_relaxed);
    }

    void note_sdk_dispatch_bypass() noexcept
    {
        if (!window_id_.load(std::memory_order_relaxed)) return;
        sdk_dispatch_bypass_.fetch_add(1, std::memory_order_relaxed);
    }

    bool is_emitted() const noexcept
    {
        return emitted_.load(std::memory_order_relaxed);
    }

    bool claim_emit_once() noexcept
    {
        if (emitted_.load(std::memory_order_relaxed)) return false;
        if (!window_id_.load(std::memory_order_acquire)) return false;
        return !emitted_.exchange(true, std::memory_order_acq_rel);
    }

    SummarySnapshot snapshot() const
    {
        SummarySnapshot out{};
        out.frequency = frequency_.load(std::memory_order_acquire);
        out.devs_calls = devs_calls_.load(std::memory_order_relaxed);
        out.devs_ticks = devs_ticks_.load(std::memory_order_relaxed);
        out.devs_max_ticks = devs_max_ticks_.load(std::memory_order_relaxed);
        out.copy_bypass_before = copy_bypass_before_.load(std::memory_order_relaxed);
        out.copy_bypass_rec = copy_bypass_rec_.load(std::memory_order_relaxed);
        out.empty_tables_bypass = empty_tables_bypass_.load(std::memory_order_relaxed);
        out.sdk_dispatch_bypass = sdk_dispatch_bypass_.load(std::memory_order_relaxed);
        for (std::size_t i = 1; i < kOpCount; ++i) {
            const auto& r = ops_[i];
            const auto  calls = r.calls.load(std::memory_order_relaxed);
            const auto  blamed_n = r.blamed_count.load(std::memory_order_relaxed);
            if (!calls && !blamed_n) continue;
            out.ops.push_back(OpSnapshot{
                static_cast<ClientLockOp>(i),
                calls,
                r.wait_ticks.load(std::memory_order_relaxed),
                r.hold_ticks.load(std::memory_order_relaxed),
                r.max_wait_ticks.load(std::memory_order_relaxed),
                r.max_hold_ticks.load(std::memory_order_relaxed),
                r.blamed_wait_ticks.load(std::memory_order_relaxed),
                blamed_n
            });
        }
        std::sort(out.ops.begin(), out.ops.end(), [](const OpSnapshot& a, const OpSnapshot& b) {
            return (a.wait_ticks + a.hold_ticks) > (b.wait_ticks + b.hold_ticks);
        });
        {
            std::lock_guard lk(slow_mtx_);
            out.slow_waits.assign(slow_waits_.begin(), slow_waits_.begin() + slow_count_);
        }
        std::sort(out.slow_waits.begin(), out.slow_waits.end(), [](const SlowWaitSample& a, const SlowWaitSample& b) {
            return a.wait_ticks > b.wait_ticks;
        });
        return out;
    }
};

} // namespace hsin_rt
