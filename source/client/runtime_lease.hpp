#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace hsin_rt {

// Lock-free single-device reader lease with cache-line isolated per-thread
// reference counters and quiescent-state drain on unpublish/device destruction.
//
// Invariant:
// - Publishing a primary runtime requires that no previous primary is live and
//   no drain is in progress (`pending_drains_ == 0`). Keys (`primary_api_`,
//   `primary_native_`) are written before `primary_raw_.store(ptr, release)`.
// - Readers increment `slots_[s].depth` with `seq_cst` BEFORE loading
//   `primary_raw_` with `acquire`.
// - Unpublishing stores `nullptr` into `primary_raw_` with `seq_cst` (under
//   the caller's devices mutex), increments `pending_drains_`, releases the
//   caller's mutex, and waits until every `slots_[i].depth == 0`.
// - By total order of `seq_cst`, any reader that observes a non-null `ptr`
//   has already incremented `depth` before the drain scans it, so neither the
//   runtime nor `primary_api_`/`primary_native_` can be torn down or replaced
//   until that reader's Lease destructs.
template<class T, class ApiKey, class NativeKey>
class PrimaryRuntimeTable {
public:
    static constexpr std::size_t kSlotCount = 64;

private:
    struct alignas(64) Slot {
        std::atomic<std::uint32_t> depth{0};
    };

    std::array<Slot, kSlotCount> slots_{};
    std::atomic<T *> primary_raw_{nullptr};
    std::atomic<ApiKey> primary_api_{};
    std::atomic<NativeKey> primary_native_{};
    std::atomic<std::uint32_t> pending_drains_{0};

    static std::size_t thread_slot() noexcept {
        // Trivial thread_local with zero destructor: safe across DLL unload
        // and supports arbitrary thread counts (hash collisions merely share
        // an atomic counter cleanly).
        static thread_local std::size_t cached = []() noexcept -> std::size_t {
            const std::size_t h = std::hash<std::thread::id>{}(std::this_thread::get_id());
            return ((h * 0x9e3779b97f4a7c15ULL) >> 58) & (kSlotCount - 1);
        }();
        return cached;
    }

public:
    class Lease {
        T *raw_ = nullptr;
        std::atomic<std::uint32_t> *depth_ = nullptr;
        std::shared_ptr<T> fallback_;

    public:
        Lease() noexcept = default;
        Lease(T *raw, std::atomic<std::uint32_t> *depth) noexcept
            : raw_(raw), depth_(depth) {}
        explicit Lease(std::shared_ptr<T> fallback) noexcept
            : raw_(fallback.get()), fallback_(std::move(fallback)) {}

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        Lease(Lease &&other) noexcept
            : raw_(other.raw_), depth_(other.depth_), fallback_(std::move(other.fallback_)) {
            other.raw_ = nullptr;
            other.depth_ = nullptr;
        }
        Lease &operator=(Lease &&other) noexcept {
            if (this != &other) {
                reset();
                raw_ = other.raw_;
                depth_ = other.depth_;
                fallback_ = std::move(other.fallback_);
                other.raw_ = nullptr;
                other.depth_ = nullptr;
            }
            return *this;
        }
        ~Lease() noexcept { reset(); }

        void reset() noexcept {
            if (depth_) {
                depth_->fetch_sub(1, std::memory_order_release);
                depth_ = nullptr;
            }
            fallback_.reset();
            raw_ = nullptr;
        }

        T *get() const noexcept { return raw_; }
        T *operator->() const noexcept { return raw_; }
        T &operator*() const noexcept { return *raw_; }
        explicit operator bool() const noexcept { return raw_ != nullptr; }
    };

    // Fast-path lookup by ApiKey. Returns an engaged Lease on hit, or an empty
    // Lease if no primary is active or the key does not match (caller should
    // then fall back to the mutex-protected device map).
    Lease try_acquire_api(ApiKey key) noexcept {
        if (primary_raw_.load(std::memory_order_relaxed) == nullptr)
            return {};
        auto &counter = slots_[thread_slot()].depth;
        counter.fetch_add(1, std::memory_order_seq_cst);
        T *raw = primary_raw_.load(std::memory_order_seq_cst);
        if (raw && primary_api_.load(std::memory_order_relaxed) == key)
            return Lease(raw, &counter);
        counter.fetch_sub(1, std::memory_order_release);
        return {};
    }

    // Fast-path lookup by NativeKey.
    Lease try_acquire_native(NativeKey key) noexcept {
        if (primary_raw_.load(std::memory_order_relaxed) == nullptr)
            return {};
        auto &counter = slots_[thread_slot()].depth;
        counter.fetch_add(1, std::memory_order_seq_cst);
        T *raw = primary_raw_.load(std::memory_order_seq_cst);
        if (raw && primary_native_.load(std::memory_order_relaxed) == key)
            return Lease(raw, &counter);
        counter.fetch_sub(1, std::memory_order_release);
        return {};
    }

    // Called under external exclusive devices mutex when devices.size() == 1.
    // Skips publication if a prior drain has not yet finished.
    bool publish_primary_locked(ApiKey api_key, NativeKey native_key, T *raw) noexcept {
        if (!raw)
            return false;
        if (primary_raw_.load(std::memory_order_relaxed) != nullptr)
            return false;
        if (pending_drains_.load(std::memory_order_acquire) != 0)
            return false;
        primary_api_.store(api_key, std::memory_order_relaxed);
        primary_native_.store(native_key, std::memory_order_relaxed);
        primary_raw_.store(raw, std::memory_order_release);
        return true;
    }

    // Called under external exclusive devices mutex before releasing the mutex.
    // Returns true if a primary was unpublished and the caller MUST invoke
    // `finish_drain_unlocked()` AFTER releasing the external mutex.
    bool begin_unpublish_locked(T *only_if_matches = nullptr) noexcept {
        T *cur = primary_raw_.load(std::memory_order_relaxed);
        if (!cur || (only_if_matches && cur != only_if_matches))
            return false;
        pending_drains_.fetch_add(1, std::memory_order_acq_rel);
        primary_raw_.store(nullptr, std::memory_order_seq_cst);
        return true;
    }

    // Called AFTER releasing the external devices mutex when
    // `begin_unpublish_locked()` returned true.
    void finish_drain_unlocked() noexcept {
        for (auto &slot : slots_) {
            unsigned spins = 0;
            while (slot.depth.load(std::memory_order_seq_cst) != 0) {
                if (++spins < 64) {
                    // Brief yield-free spin for nanoseconds-long fast leases.
                } else {
                    std::this_thread::yield();
                }
            }
        }
        pending_drains_.fetch_sub(1, std::memory_order_release);
    }

    bool has_primary() const noexcept {
        return primary_raw_.load(std::memory_order_acquire) != nullptr;
    }
};

} // namespace hsin_rt
