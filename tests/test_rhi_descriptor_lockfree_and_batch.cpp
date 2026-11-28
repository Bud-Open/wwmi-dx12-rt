// Adversarial multi-threaded concurrency, lifetime, & ABI tests for RHIThread CPU reduction:
// 1. Overlapping same-slot concurrent writers (AcquireSlotWriteLock LOCK BTS) + readers +
//    ephemeral unpinned descriptor retirement under CopyLaneGuard grace period
// 2. Lane exhaustion (all lanes claimed by idle readers) + lane-less producer still draining
//    retired descriptors + wait-free TLS abandonment (kLaneAbandoned) & lane reuse
// 3. PreviousReset drain barrier with WithPublishedSlot direct readers
// 4. ThreadGpuAddrCache overlapping placed buffers (capped intervals + upper_bound equivalence)
// 5. Real WWMI_DescriptorPublisher v1/v2/v3 ABI validation, batch vs enumerate span order,
//    and exact single native continuation guarantee
// 6. Increment-shift handle decode equivalence (pow2 + non-pow2 + misaligned)
//
// Compile: g++ -std=c++20 -O2 -Wall -Wextra -pthread -Isource/include \
//          tests/test_rhi_descriptor_lockfree_and_batch.cpp -o test
#include "wwmi_descriptor_publish.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <thread>
#include <tuple>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
static inline void cpu_pause() noexcept { _mm_pause(); }
#else
static inline void cpu_pause() noexcept { std::this_thread::yield(); }
#endif

namespace {

constexpr unsigned kLaneCount = 32;
constexpr uint64_t kCopyDepthInc = 1ull;
constexpr uint64_t kRawFastInc = 1ull << 32;
constexpr uint64_t kCopyDepthMask = 0xFFFFFFFFull;
constexpr uint64_t kRawFastMask = 0x7FFFFFFF00000000ull;
constexpr uint64_t kCacheDrainBit = uint64_t{1} << 63;
constexpr uint64_t kSlotUpdatingBit = uint64_t{1} << 63;
constexpr uint8_t kLaneFree = 0;
constexpr uint8_t kLaneActive = 1;
constexpr uint8_t kLaneAbandoned = 2;
constexpr uint32_t kMaxBorrowRefCount = 0x7FFFFFFFu;
constexpr uint32_t kMaxTryRetainRefCount = UINT32_MAX - 1024u;

struct TestContent {
    mutable std::atomic<uint32_t> references{1};
    uint32_t cacheable{1};
    uint64_t tag{0};
    std::atomic<bool> destroyed{false};
};

struct SimReclaimEnv {
    std::mutex retired_mtx;
    std::vector<const TestContent*> retired;
    std::atomic<size_t> retired_count{0};
    std::atomic<uint64_t> deferred_destroys{0};
    std::atomic<uint64_t> destroyed_total{0};
    std::atomic<uint64_t> double_frees{0};
    std::atomic<uint64_t> uaf_reads{0};
    std::atomic<uint64_t> grace_epoch{1};

    void defer_destroy(const TestContent* c) noexcept {
        std::lock_guard lk(retired_mtx);
        retired.push_back(c);
        retired_count.store(retired.size(), std::memory_order_relaxed);
        deferred_destroys.fetch_add(1, std::memory_order_relaxed);
    }
};

static inline bool TryRetain(const TestContent* c, SimReclaimEnv* env = nullptr) noexcept {
    if (!c) return true;
    if (c->destroyed.load(std::memory_order_acquire)) {
        if (env) env->uaf_reads.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uint32_t r = c->references.load(std::memory_order_relaxed);
    while (r != 0 && r < kMaxTryRetainRefCount) {
        if (c->references.compare_exchange_weak(r, r + 1, std::memory_order_acquire, std::memory_order_relaxed))
            return true;
    }
    return false;
}

static inline bool TryRetainN(const TestContent* c, uint32_t n) noexcept {
    if (!c || !n) return true;
    uint32_t r = c->references.load(std::memory_order_relaxed);
    while (r != 0 && n <= kMaxBorrowRefCount && r <= kMaxBorrowRefCount - n) {
        if (c->references.compare_exchange_weak(r, r + n, std::memory_order_acquire, std::memory_order_relaxed))
            return true;
    }
    return false;
}

static inline void ReleaseN(const TestContent* c, uint32_t n, SimReclaimEnv& env) noexcept {
    if (!c || !n) return;
    const uint32_t prev = c->references.fetch_sub(n, std::memory_order_acq_rel);
    if (prev < n) {
        env.double_frees.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (prev == n) env.defer_destroy(c);
}

struct SimLaneRefCache {
    static constexpr uint32_t kSets = 64;
    static constexpr uint32_t kBorrow = 32;
    static constexpr uint32_t kMaxCredit = 96;
    static constexpr uint32_t kFlushChunk = 64;
    struct Entry {
        const TestContent* ptr = nullptr;
        uint32_t credit = 0;
        uint8_t touched = 0;
    };
    struct Set {
        Entry ways[2]{};
        uint8_t next_victim = 0;
    };
    std::array<Set, kSets> sets{};
    uint32_t ops = 0;
    uint32_t sweep_cursor = 0;

    static size_t set_idx(const TestContent* p) noexcept {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return size_t(((v >> 5) ^ (v >> 11) ^ (v >> 17)) & (kSets - 1u));
    }

    bool try_retain(const TestContent* ptr, SimReclaimEnv& env, uint64_t* misses = nullptr) noexcept {
        if (!ptr) return true;
        auto& set = sets[set_idx(ptr)];
        for (unsigned w = 0; w < 2; ++w) {
            auto& e = set.ways[w];
            if (e.ptr == ptr && e.credit > 0) {
                --e.credit;
                e.touched = 1;
                set.next_victim = uint8_t(w ^ 1u);
                if (e.credit == 0) e.ptr = nullptr;
                return true;
            }
        }
        if (misses) ++(*misses);
        if (!ptr->cacheable) return TryRetain(ptr, &env);
        if (!TryRetainN(ptr, kBorrow)) return TryRetain(ptr, &env);
        constexpr uint32_t spare = kBorrow - 1u;
        for (unsigned w = 0; w < 2; ++w) {
            if (set.ways[w].credit == 0) {
                set.ways[w] = {ptr, spare, 1};
                set.next_victim = uint8_t(w ^ 1u);
                return true;
            }
        }
        unsigned v = set.next_victim & 1u;
        if (set.ways[0].credit <= 1u && set.ways[1].credit > 1u) v = 0;
        else if (set.ways[1].credit <= 1u && set.ways[0].credit > 1u) v = 1;
        const auto* old_ptr = set.ways[v].ptr;
        const uint32_t old_cred = set.ways[v].credit;
        set.ways[v] = {ptr, spare, 1};
        set.next_victim = uint8_t(v ^ 1u);
        ReleaseN(old_ptr, old_cred, env);
        return true;
    }

    void release(const TestContent* ptr, SimReclaimEnv& env) noexcept {
        if (!ptr) return;
        auto& set = sets[set_idx(ptr)];
        for (unsigned w = 0; w < 2; ++w) {
            auto& e = set.ways[w];
            if (e.ptr == ptr) {
                ++e.credit;
                e.touched = 1;
                set.next_victim = uint8_t(w ^ 1u);
                if (e.credit >= kMaxCredit) {
                    e.credit -= kFlushChunk;
                    ReleaseN(ptr, kFlushChunk, env);
                }
                return;
            }
        }
        if (!ptr->cacheable) {
            ReleaseN(ptr, 1, env);
            return;
        }
        for (unsigned w = 0; w < 2; ++w) {
            if (set.ways[w].credit == 0) {
                set.ways[w] = {ptr, 1u, 1};
                set.next_victim = uint8_t(w ^ 1u);
                return;
            }
        }
        // No-write-allocate: do NOT evict warm borrowed ways for a cold 1-credit release.
        ReleaseN(ptr, 1, env);
    }

    void maybe_sweep(SimReclaimEnv& env) noexcept {
        if ((++ops & 255u) != 0u) return;
        for (uint32_t k = 0; k < 8u; ++k) {
            auto& set = sets[sweep_cursor & (kSets - 1u)];
            ++sweep_cursor;
            for (auto& e : set.ways) {
                if (e.credit == 0) continue;
                if (e.touched) { e.touched = 0; continue; }
                const auto* p = e.ptr;
                const uint32_t c = e.credit;
                e = {};
                ReleaseN(p, c, env);
            }
        }
    }

    void flush_all(SimReclaimEnv& env) noexcept {
        for (auto& set : sets) {
            for (auto& e : set.ways) {
                if (e.ptr && e.credit) {
                    const auto* p = e.ptr;
                    const uint32_t c = e.credit;
                    e = {};
                    ReleaseN(p, c, env);
                } else {
                    e = {};
                }
            }
            set.next_victim = 0;
        }
    }
};

struct alignas(64) SimCopyLane {
    std::atomic<uint64_t> state{0};
    std::atomic<uint8_t> owner_state{kLaneFree};
    std::atomic<SimLaneRefCache*> ref_cache{nullptr};
};

struct SimLaneEnv {
    std::array<SimCopyLane, kLaneCount> lanes{};
    std::array<std::unique_ptr<SimLaneRefCache>, kLaneCount> storage{};
    std::mutex alloc_mtx;
    std::atomic<uint32_t> active_lanes{0};
    std::atomic<uint32_t> abandoned_lanes{0};
    std::atomic<uint32_t> raw_pending{0};
    std::atomic<uint32_t> waiters{0};
    SimReclaimEnv reclaim;

    SimLaneRefCache* ensure_cache(SimCopyLane* lane) {
        if (!lane) return nullptr;
        if (auto* c = lane->ref_cache.load(std::memory_order_acquire)) return c;
        const size_t idx = size_t(lane - lanes.data());
        std::lock_guard lk(alloc_mtx);
        if (auto* c = lane->ref_cache.load(std::memory_order_relaxed)) return c;
        storage[idx] = std::make_unique<SimLaneRefCache>();
        auto* ptr = storage[idx].get();
        lane->ref_cache.store(ptr, std::memory_order_release);
        return ptr;
    }

    void acquire_cache_drain(SimCopyLane& lane) noexcept {
        waiters.fetch_add(1, std::memory_order_seq_cst);
        uint64_t s = lane.state.load(std::memory_order_relaxed);
        for (;;) {
            if (s & kCacheDrainBit) {
                lane.state.wait(s, std::memory_order_acquire);
                s = lane.state.load(std::memory_order_relaxed);
                continue;
            }
            if (lane.state.compare_exchange_weak(s, s | kCacheDrainBit,
                std::memory_order_seq_cst, std::memory_order_relaxed))
                break;
        }
        auto v = lane.state.load(std::memory_order_seq_cst);
        while (v & kCopyDepthMask) {
            lane.state.wait(v, std::memory_order_acquire);
            v = lane.state.load(std::memory_order_seq_cst);
        }
    }

    void release_cache_drain(SimCopyLane& lane) noexcept {
        lane.state.fetch_and(~kCacheDrainBit, std::memory_order_release);
        lane.state.notify_all();
        waiters.fetch_sub(1, std::memory_order_release);
    }

    SimCopyLane* try_claim_lane(unsigned start) noexcept {
        if (active_lanes.load(std::memory_order_acquire) >= kLaneCount &&
            abandoned_lanes.load(std::memory_order_acquire) == 0)
            return nullptr;
        for (unsigned i = 0; i < kLaneCount; ++i) {
            auto& cand = lanes[(start + i) & (kLaneCount - 1u)];
            uint8_t exp = kLaneFree;
            if (cand.owner_state.compare_exchange_strong(exp, kLaneActive, std::memory_order_acq_rel)) {
                active_lanes.fetch_add(1, std::memory_order_release);
                return &cand;
            }
        }
        if (abandoned_lanes.load(std::memory_order_acquire) == 0) return nullptr;
        for (unsigned i = 0; i < kLaneCount; ++i) {
            auto& cand = lanes[(start + i) & (kLaneCount - 1u)];
            uint8_t exp = kLaneAbandoned;
            if (cand.owner_state.compare_exchange_strong(exp, kLaneActive, std::memory_order_acq_rel)) {
                abandoned_lanes.fetch_sub(1, std::memory_order_release);
                active_lanes.fetch_add(1, std::memory_order_release);
                acquire_cache_drain(cand);
                if (auto* c = cand.ref_cache.load(std::memory_order_acquire))
                    c->flush_all(reclaim);
                release_cache_drain(cand);
                return &cand;
            }
        }
        return nullptr;
    }

    // Matches wait-free ~ThreadCopyLane() (zero locks / zero waits!)
    void abandon_lane_waitfree(SimCopyLane* lane) noexcept {
        if (!lane) return;
        if (lane->ref_cache.load(std::memory_order_acquire) != nullptr) {
            abandoned_lanes.fetch_add(1, std::memory_order_release);
            lane->owner_state.store(kLaneAbandoned, std::memory_order_release);
        } else {
            lane->owner_state.store(kLaneFree, std::memory_order_release);
        }
        active_lanes.fetch_sub(1, std::memory_order_release);
    }

    void flush_abandoned_caches() noexcept {
        if (abandoned_lanes.load(std::memory_order_acquire) == 0) return;
        for (auto& lane : lanes) {
            if (lane.owner_state.load(std::memory_order_acquire) != kLaneAbandoned) continue;
            uint8_t exp = kLaneAbandoned;
            if (!lane.owner_state.compare_exchange_strong(exp, kLaneActive, std::memory_order_acq_rel)) continue;
            abandoned_lanes.fetch_sub(1, std::memory_order_release);
            acquire_cache_drain(lane);
            if (auto* c = lane.ref_cache.load(std::memory_order_acquire))
                c->flush_all(reclaim);
            release_cache_drain(lane);
            lane.owner_state.store(kLaneFree, std::memory_order_release);
        }
    }

    void flush_all_caches() noexcept {
        flush_abandoned_caches();
        for (auto& lane : lanes) {
            if (!lane.ref_cache.load(std::memory_order_acquire)) continue;
            acquire_cache_drain(lane);
            if (auto* c = lane.ref_cache.load(std::memory_order_acquire))
                c->flush_all(reclaim);
            release_cache_drain(lane);
        }
    }

    void drain_lanes(uint64_t mask) noexcept {
        waiters.fetch_add(1, std::memory_order_seq_cst);
        for (auto& lane : lanes) {
            auto v = lane.state.load(std::memory_order_seq_cst);
            while (v & mask) {
                lane.state.wait(v, std::memory_order_acquire);
                v = lane.state.load(std::memory_order_seq_cst);
            }
        }
        waiters.fetch_sub(1, std::memory_order_release);
    }

    void try_reclaim_retired(size_t threshold = 64) noexcept {
        if (reclaim.retired_count.load(std::memory_order_relaxed) < threshold) return;
        std::vector<const TestContent*> batch;
        {
            std::unique_lock lk(reclaim.retired_mtx, std::try_to_lock);
            if (!lk.owns_lock() || reclaim.retired.size() < threshold) return;
            batch.swap(reclaim.retired);
            reclaim.retired_count.store(0, std::memory_order_relaxed);
        }
        reclaim.grace_epoch.fetch_add(1, std::memory_order_seq_cst);
        drain_lanes(kCopyDepthMask);
        for (const auto* c : batch) {
            const_cast<TestContent*>(c)->destroyed.store(true, std::memory_order_release);
            reclaim.destroyed_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void reclaim_blocking() noexcept {
        for (;;) {
            std::vector<const TestContent*> batch;
            {
                std::lock_guard lk(reclaim.retired_mtx);
                batch.swap(reclaim.retired);
                reclaim.retired_count.store(0, std::memory_order_relaxed);
            }
            if (batch.empty()) return;
            reclaim.grace_epoch.fetch_add(1, std::memory_order_seq_cst);
            drain_lanes(kCopyDepthMask);
            for (const auto* c : batch) {
                const_cast<TestContent*>(c)->destroyed.store(true, std::memory_order_release);
                reclaim.destroyed_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
};

struct SimVisSlot {
    std::atomic<const TestContent*> content{nullptr};
    std::atomic<uint64_t> write{0};
    std::atomic<uint32_t> frame{0};
    std::atomic<uint32_t> miss{0};
};

struct SimVisPage {
    std::atomic<uint32_t> vis_write_lock{0};
    void lock_vis_write() noexcept {
        if (vis_write_lock.exchange(1u, std::memory_order_acquire) == 0u) return;
        for (unsigned spin = 0;; ++spin) {
            if (vis_write_lock.load(std::memory_order_relaxed) == 0u &&
                vis_write_lock.exchange(1u, std::memory_order_acquire) == 0u)
                return;
            if (spin < 64) cpu_pause(); else std::this_thread::yield();
        }
    }
    void unlock_vis_write() noexcept {
        vis_write_lock.store(0u, std::memory_order_release);
    }
};

// Zero-LOCK Boehm C++20 seqlock slot write while holding page vis_write_lock.
static inline bool WriteSlotUnderVisPageLock(SimVisSlot& slot, const TestContent* nv,
    uint64_t new_w, uint32_t enc_frame, const TestContent*& cur_out) noexcept {
    slot.write.store(new_w | kSlotUpdatingBit, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const TestContent* cur = slot.content.load(std::memory_order_relaxed);
    bool changed = false;
    if (cur != nv) {
        slot.content.store(nv, std::memory_order_relaxed);
        changed = true;
        cur_out = cur;
    } else {
        cur_out = nullptr;
    }
    slot.frame.store(enc_frame, std::memory_order_relaxed);
    slot.miss.store(0, std::memory_order_relaxed);
    slot.write.store(new_w, std::memory_order_release);
    return changed;
}

// ---------------------------------------------------------------------------
// Test 1: Adversarial overlapping SAME-PAGE/SAME-SLOT writers (per-page
//         vis_write_lock + zero-LOCK Boehm seqlock writes) + ephemeral unpinned
//         objects + concurrent lock-free readers + grace-period reclamation
// ---------------------------------------------------------------------------
static void test_adversarial_same_slot_two_writers_and_readers() {
    SimLaneEnv env;
    constexpr size_t kPages = 2;
    constexpr size_t kSlotsPerPage = 4;
    constexpr size_t kHotSlots = kPages * kSlotsPerPage; // 6 writers hammer the SAME 2 pages / 8 slots!
    std::array<SimVisPage, kPages> pages{};
    std::array<SimVisSlot, kHotSlots> slots{};
    std::atomic<uint64_t> next_ver{1};
    std::atomic<uint64_t> next_tag{1};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> read_ok{0};
    std::atomic<uint64_t> retain_rejected_zero{0};
    std::atomic<uint64_t> torn_reads{0};

    // Keep backing storage alive only after destroyed==true check so we can assert
    // exact refcount/destroyed state on every single ephemeral object created!
    std::mutex all_objs_mtx;
    std::vector<std::unique_ptr<TestContent>> all_objs;

    constexpr size_t kWriters = 6;
    constexpr int kItersPerWriter = 12000;
    std::vector<std::thread> writers;
    for (size_t w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w] {
            SimCopyLane* lane = env.try_claim_lane(unsigned(w));
            assert(lane != nullptr);
            std::vector<std::unique_ptr<TestContent>> local_objs;
            local_objs.reserve(kItersPerWriter / 4 + 16);
            std::mt19937_64 rng(0xBEEF0000ull + w);

            // Also keep a tiny rotating set of 8 shared-ish objects per writer to hit LaneRefCache
            std::vector<TestContent*> rotating;
            for (int r = 0; r < 8; ++r) {
                auto obj = std::make_unique<TestContent>();
                obj->cacheable = (r == 0) ? 0u : 1u;
                obj->tag = next_tag.fetch_add(1, std::memory_order_relaxed);
                rotating.push_back(obj.get());
                local_objs.push_back(std::move(obj));
            }

            for (int iter = 0; iter < kItersPerWriter; ++iter) {
                const uint64_t prev = lane->state.fetch_add(kCopyDepthInc | kRawFastInc, std::memory_order_seq_cst);
                (void)env.reclaim.grace_epoch.load(std::memory_order_seq_cst);
                const bool use_cache = (prev & kCacheDrainBit) == 0;
                SimLaneRefCache* rc = use_cache ? env.ensure_cache(lane) : nullptr;
                const bool fast_raw = env.raw_pending.load(std::memory_order_seq_cst) == 0;
                if (!fast_raw) {
                    const uint64_t p2 = lane->state.fetch_sub(kRawFastInc, std::memory_order_seq_cst);
                    if ((p2 & kCacheDrainBit) || env.waiters.load(std::memory_order_seq_cst) != 0)
                        lane->state.notify_all();
                }

                // Every 4th iteration allocate a fresh UNPINNED ephemeral object (refs == 1,
                // transferred directly into slot -> hits refs==0 on displacement while readers race!).
                const TestContent* nv = nullptr;
                bool ephemeral = ((iter & 3) == 0);
                if (ephemeral) {
                    auto obj = std::make_unique<TestContent>();
                    obj->cacheable = ((iter & 7) == 0) ? 0u : 1u;
                    obj->tag = next_tag.fetch_add(1, std::memory_order_relaxed);
                    nv = obj.get(); // owns 1 ref directly
                    local_objs.push_back(std::move(obj));
                } else {
                    nv = rotating[rng() % rotating.size()];
                    const bool ok = rc ? rc->try_retain(nv, env.reclaim) : TryRetain(nv, &env.reclaim);
                    assert(ok);
                }

                const uint64_t ver = next_ver.fetch_add(1, std::memory_order_relaxed);
                const uint32_t enc_frame = uint32_t((nv->tag & 0xFFFFu) ^ ((ver & 0xFFFFu) << 16));
                const uint64_t new_w = ((ver << 2) | 2u) & ~kSlotUpdatingBit;

                // Multi-page (1 or 2 pages in sorted order) + multi-slot batch write under per-page vis_write_lock
                const size_t s0 = rng() % kHotSlots;
                const size_t s1 = ((iter & 3) == 0) ? ((s0 + kSlotsPerPage) % kHotSlots) : s0;
                const size_t p0 = s0 / kSlotsPerPage;
                const size_t p1 = s1 / kSlotsPerPage;
                const size_t lo_p = (std::min)(p0, p1);
                const size_t hi_p = (std::max)(p0, p1);
                pages[lo_p].lock_vis_write();
                if (hi_p != lo_p) pages[hi_p].lock_vis_write();
                const TestContent* cur0 = nullptr;
                const bool changed0 = WriteSlotUnderVisPageLock(slots[s0], nv, new_w, enc_frame, cur0);
                if (hi_p != lo_p) pages[hi_p].unlock_vis_write();
                pages[lo_p].unlock_vis_write();

                if (changed0) {
                    if (cur0) { if (rc) rc->release(cur0, env.reclaim); else ReleaseN(cur0, 1, env.reclaim); }
                } else {
                    if (rc) rc->release(nv, env.reclaim); else ReleaseN(nv, 1, env.reclaim);
                }
                if (rc) rc->maybe_sweep(env.reclaim);

                const uint64_t sub = kCopyDepthInc | (fast_raw ? kRawFastInc : 0ull);
                const uint64_t p_exit = lane->state.fetch_sub(sub, std::memory_order_seq_cst);
                if ((p_exit & kCacheDrainBit) || env.waiters.load(std::memory_order_seq_cst) != 0)
                    lane->state.notify_all();

                if ((iter & 127) == 0) env.try_reclaim_retired(64);
            }

            // Release rotating owner refs so rotating objects also retire cleanly!
            for (TestContent* p : rotating) ReleaseN(p, 1, env.reclaim);
            // Wait-free TLS exit (zero locks!)
            env.abandon_lane_waitfree(lane);

            std::lock_guard lk(all_objs_mtx);
            for (auto& o : local_objs) all_objs.push_back(std::move(o));
        });
    }

    // Readers racing against overlapping writers + ephemeral retirement
    std::vector<std::thread> readers;
    for (size_t r = 0; r < 4; ++r) {
        readers.emplace_back([&, r] {
            SimCopyLane* lane = env.try_claim_lane(unsigned(kWriters + r));
            assert(lane != nullptr);
            std::mt19937_64 rng(0xCAFE0000ull + r);
            while (!stop.load(std::memory_order_relaxed)) {
                (void)lane->state.fetch_add(kCopyDepthInc, std::memory_order_seq_cst);
                (void)env.reclaim.grace_epoch.load(std::memory_order_seq_cst);
                const auto& s = slots[rng() % kHotSlots];
                for (unsigned spin = 0; spin < 256; ++spin) {
                    const uint64_t w1 = s.write.load(std::memory_order_acquire);
                    if (w1 & kSlotUpdatingBit) { cpu_pause(); continue; }
                    const TestContent* raw = s.content.load(std::memory_order_relaxed);
                    const uint32_t f = s.frame.load(std::memory_order_relaxed);
                    const uint32_t m = s.miss.load(std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_acquire);
                    const uint64_t w2 = s.write.load(std::memory_order_relaxed);
                    if (w1 != w2) continue;
                    if (raw && !TryRetain(raw, &env.reclaim)) {
                        retain_rejected_zero.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    if (w1 != 0) {
                        const uint64_t ver = w1 >> 2;
                        const uint32_t expect_f = uint32_t((raw->tag & 0xFFFFu) ^ ((ver & 0xFFFFu) << 16));
                        if (!raw || raw->destroyed.load(std::memory_order_acquire) || f != expect_f || m != 0) {
                            torn_reads.fetch_add(1, std::memory_order_relaxed);
                        }
                        read_ok.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (raw) ReleaseN(raw, 1, env.reclaim);
                    break;
                }
                const uint64_t p_exit = lane->state.fetch_sub(kCopyDepthInc, std::memory_order_seq_cst);
                if ((p_exit & kCacheDrainBit) || env.waiters.load(std::memory_order_seq_cst) != 0)
                    lane->state.notify_all();
            }
            env.abandon_lane_waitfree(lane);
        });
    }

    std::thread drainer([&] {
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            env.raw_pending.fetch_add(1, std::memory_order_seq_cst);
            env.drain_lanes(kRawFastMask);
            env.raw_pending.fetch_sub(1, std::memory_order_release);
            env.flush_all_caches();
        }
    });

    for (auto& t : writers) t.join();
    drainer.join();
    stop.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();

    // Clear remaining slot references, flush abandoned caches, and reclaim all retired objects!
    for (size_t i = 0; i < kHotSlots; ++i) {
        auto& p = pages[i / kSlotsPerPage];
        p.lock_vis_write();
        const TestContent* old = nullptr;
        (void)WriteSlotUnderVisPageLock(slots[i], nullptr, 0, 0, old);
        p.unlock_vis_write();
        if (old) ReleaseN(old, 1, env.reclaim);
    }
    env.flush_all_caches();
    env.reclaim_blocking();

    assert(torn_reads.load() == 0);
    assert(env.reclaim.double_frees.load() == 0);
    assert(env.reclaim.uaf_reads.load() == 0);
    assert(read_ok.load() > 5000);
    assert(env.reclaim.destroyed_total.load() == all_objs.size());
    for (size_t i = 0; i < all_objs.size(); ++i) {
        assert(all_objs[i]->references.load(std::memory_order_acquire) == 0u);
        assert(all_objs[i]->destroyed.load(std::memory_order_acquire));
    }
    std::printf("PASS: test_adversarial_same_slot_two_writers_and_readers (objs=%zu, reads=%llu, zero_retries=%llu, torn=0, leaks=0, uaf=0)\n",
        all_objs.size(), (unsigned long long)read_ok.load(), (unsigned long long)retain_rejected_zero.load());
}

// ---------------------------------------------------------------------------
// Test 2: Lane exhaustion (all 32 lanes held by idle readers) + lane-less
//         producer still drains retirement queue + wait-free abandonment & reuse
// ---------------------------------------------------------------------------
static void test_lane_exhaustion_reclamation_and_abandoned_tls_exit() {
    SimLaneEnv env;
    // Claim ALL kLaneCount lanes as resident idle readers (with credits in lane 0).
    std::vector<SimCopyLane*> claimed(kLaneCount, nullptr);
    for (unsigned i = 0; i < kLaneCount; ++i) {
        claimed[i] = env.try_claim_lane(i);
        assert(claimed[i] != nullptr);
    }
    assert(env.active_lanes.load() == kLaneCount);

    // Park a credit in lane 0 so lane 0 becomes kLaneAbandoned on wait-free exit.
    auto parked = std::make_unique<TestContent>();
    parked->cacheable = 1;
    auto* lane0_cache = env.ensure_cache(claimed[0]);
    assert(lane0_cache->try_retain(parked.get(), env.reclaim));
    ReleaseN(parked.get(), 1, env.reclaim); // release extra retained copy; cache still holds kBorrow-1 credits!
    ReleaseN(parked.get(), 1, env.reclaim); // release creator ref -> ONLY lane0_cache keeps parked alive!
    assert(parked->references.load() == SimLaneRefCache::kBorrow - 1u);

    // 33rd thread (lane-less copy worker) fails try_claim_lane, yet tracks outermost
    // callback_depth independently of lane ownership and MUST drain retired objects!
    assert(env.try_claim_lane(0) == nullptr);
    std::vector<std::unique_ptr<TestContent>> produced;
    uint32_t callback_depth = 0;
    for (int i = 0; i < 200; ++i) {
        const bool outermost = (++callback_depth == 1u);
        auto obj = std::make_unique<TestContent>();
        ReleaseN(obj.get(), 1, env.reclaim); // hits 0 -> retired queue
        produced.push_back(std::move(obj));
        --callback_depth;
        if (outermost) {
            env.flush_abandoned_caches();
            env.try_reclaim_retired(64);
        }
    }
    assert(env.reclaim.destroyed_total.load() >= 128u);

    // Now lane 0's thread exits wait-free WITHOUT locking retired_mtx!
    {
        // Lock retired_mtx on the current thread to PROVE abandon_lane_waitfree NEVER touches it!
        std::lock_guard hold_retired_mtx(env.reclaim.retired_mtx);
        env.abandon_lane_waitfree(claimed[0]);
        assert(env.abandoned_lanes.load() == 1u);
        assert(claimed[0]->owner_state.load() == kLaneAbandoned);
    }

    // Now the formerly lane-less 33rd worker retries try_claim_lane -> MUST reclaim lane 0,
    // flush parked's credits (dropping parked->references to 0), and return lane 0!
    SimCopyLane* reclaimed = env.try_claim_lane(0);
    assert(reclaimed == claimed[0]);
    assert(env.abandoned_lanes.load() == 0u);
    assert(parked->references.load() == 0u);
    env.reclaim_blocking();
    assert(parked->destroyed.load());

    // Cleanup all lanes
    env.abandon_lane_waitfree(reclaimed);
    for (unsigned i = 1; i < kLaneCount; ++i) env.abandon_lane_waitfree(claimed[i]);
    env.flush_all_caches();
    env.reclaim_blocking();
    assert(env.active_lanes.load() == 0u);
    assert(env.abandoned_lanes.load() == 0u);
    std::printf("PASS: test_lane_exhaustion_reclamation_and_abandoned_tls_exit\n");
}

// ---------------------------------------------------------------------------
// Test 3: Reset drain barrier with WithPublishedSlot direct readers
// ---------------------------------------------------------------------------
static void test_reset_barrier_with_published_slot() {
    SimLaneEnv env;
    std::atomic<uint64_t> epoch{1};
    std::atomic<bool> heap_retired{false};
    std::atomic<bool> reader_entered_projection{false};
    std::atomic<bool> allow_projection_finish{false};
    std::atomic<bool> reset_finished{false};
    std::atomic<bool> reader_accepted_stale{false};

    auto current = [&](uint64_t pair_epoch) noexcept {
        return epoch.load(std::memory_order_acquire) == pair_epoch &&
               !heap_retired.load(std::memory_order_acquire);
    };

    std::thread reader([&] {
        SimCopyLane* lane = env.try_claim_lane(0);
        assert(lane != nullptr);
        const uint64_t my_epoch = 1;
        // Enter WithPublishedSlot: holds CopyLaneGuard across projection AND trailing recheck!
        (void)lane->state.fetch_add(kCopyDepthInc, std::memory_order_seq_cst);
        assert(current(my_epoch));
        reader_entered_projection.store(true, std::memory_order_release);
        while (!allow_projection_finish.load(std::memory_order_acquire)) cpu_pause();
        // Reset MUST NOT have finished draining while reader still holds CopyLaneGuard!
        assert(!reset_finished.load(std::memory_order_acquire));
        // Trailing PreviousCurrent recheck MUST reject old-epoch data!
        if (current(my_epoch)) reader_accepted_stale.store(true, std::memory_order_release);
        const uint64_t p_exit = lane->state.fetch_sub(kCopyDepthInc, std::memory_order_seq_cst);
        if ((p_exit & kCacheDrainBit) || env.waiters.load(std::memory_order_seq_cst) != 0)
            lane->state.notify_all();
        env.abandon_lane_waitfree(lane);
    });

    while (!reader_entered_projection.load(std::memory_order_acquire)) cpu_pause();

    std::thread resetter([&] {
        epoch.store(0, std::memory_order_release);
        heap_retired.store(true, std::memory_order_release);
        env.drain_lanes(kCopyDepthMask);
        epoch.store(2, std::memory_order_release);
        reset_finished.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // Resetter MUST still be blocked in drain_lanes until reader finishes projection!
    assert(!reset_finished.load(std::memory_order_acquire));
    allow_projection_finish.store(true, std::memory_order_release);
    reader.join();
    resetter.join();
    assert(reset_finished.load(std::memory_order_acquire));
    assert(!reader_accepted_stale.load(std::memory_order_acquire));
    std::printf("PASS: test_reset_barrier_with_published_slot\n");
}

// ---------------------------------------------------------------------------
// Test 4: ThreadGpuAddrCache overlapping placed buffers & map equivalence
// ---------------------------------------------------------------------------
struct SimGpuEntry {
    uint64_t start = 0;
    uint64_t width = 0;
    uintptr_t res = 0;
    uint64_t gen = 0;
    bool is_as = false;
};

struct SimThreadGpuAddrCache {
    static constexpr size_t kPageSets = 128;
    static constexpr size_t kMruSize = 4;
    struct PageSet {
        SimGpuEntry ways[2]{};
        uint8_t next_victim = 0;
    };
    const void* dev = nullptr;
    std::array<SimGpuEntry, kMruSize> mru{};
    std::array<PageSet, kPageSets> sets{};

    static size_t page_set_index(uint64_t addr) noexcept {
        const uint64_t p = addr >> 16;
        return static_cast<size_t>((p ^ (p >> 7) ^ (p >> 14)) & (kPageSets - 1));
    }
    void reset_for_device(const void* d) noexcept {
        dev = d;
        for (auto& e : mru) e.gen = 0;
        for (auto& s : sets) { s.ways[0].gen = 0; s.ways[1].gen = 0; s.next_victim = 0; }
    }
    void insert_page(uint64_t addr, const SimGpuEntry& e, uint64_t cur_gen) noexcept {
        auto& set = sets[page_set_index(addr)];
        if (set.ways[0].gen == cur_gen && set.ways[0].start == e.start) { set.ways[0] = e; set.next_victim = 1; }
        else if (set.ways[1].gen == cur_gen && set.ways[1].start == e.start) { set.ways[1] = e; set.next_victim = 0; }
        else if (set.ways[0].gen != cur_gen) { set.ways[0] = e; set.next_victim = 1; }
        else if (set.ways[1].gen != cur_gen) { set.ways[1] = e; set.next_victim = 0; }
        else { const uint8_t v = set.next_victim & 1u; set.ways[v] = e; set.next_victim = v ^ 1u; }
    }
    void insert(uint64_t addr, uint64_t start, uint64_t width, uintptr_t res, bool is_as, uint64_t cur_gen) noexcept {
        const SimGpuEntry e{start, width, res, cur_gen, is_as};
        insert_page(addr, e, cur_gen);
        size_t pos = kMruSize - 1;
        for (size_t i = 0; i < kMruSize; ++i) {
            if ((mru[i].gen == cur_gen && mru[i].start == start) || mru[i].gen != cur_gen) { pos = i; break; }
        }
        for (size_t i = pos; i > 0; --i) mru[i] = mru[i - 1];
        mru[0] = e;
    }
    bool lookup(const void* cur_dev, uint64_t cur_gen, uint64_t addr, uintptr_t& out_res, uint64_t& out_off, bool& out_as) noexcept {
        if (dev != cur_dev) return false;
        auto& set = sets[page_set_index(addr)];
        for (unsigned w = 0; w < 2; ++w) {
            const auto& e = set.ways[w];
            if (e.gen == cur_gen && e.width != 0 && addr >= e.start && (addr - e.start) < e.width) {
                set.next_victim = uint8_t(w ^ 1u);
                out_res = e.res; out_off = addr - e.start; out_as = e.is_as;
                return true;
            }
        }
        for (size_t i = 0; i < kMruSize; ++i) {
            const auto& e = mru[i];
            if (e.gen == cur_gen && e.width != 0 && addr >= e.start && (addr - e.start) < e.width) {
                const SimGpuEntry hit = e;
                if (i > 0) { for (size_t j = i; j > 0; --j) mru[j] = mru[j - 1]; mru[0] = hit; }
                insert_page(addr, hit, cur_gen);
                out_res = hit.res; out_off = addr - hit.start; out_as = hit.is_as;
                return true;
            }
        }
        return false;
    }
};

struct SimGpuAddrDevice {
    std::map<uint64_t, std::tuple<uint64_t, uintptr_t, bool>> map;
    uint64_t gen = 1;
    SimThreadGpuAddrCache cache;

    void register_buf(uint64_t start, uint64_t width, uintptr_t res, bool is_as) {
        const auto [it, inserted] = map.insert_or_assign(start, std::make_tuple(width, res, is_as));
        bool overlaps = !inserted;
        if (!overlaps && it != map.begin()) {
            const auto prev = std::prev(it);
            if (std::get<0>(prev->second) > start - prev->first) overlaps = true;
        }
        if (!overlaps) {
            const auto next = std::next(it);
            if (next != map.end() && width > next->first - start) overlaps = true;
        }
        if (overlaps) ++gen;
    }

    void unregister_buf(uint64_t start, uintptr_t res) {
        if (const auto it = map.find(start); it != map.end() && std::get<1>(it->second) == res) {
            map.erase(it);
            ++gen;
        }
    }

    bool resolve_ref(uint64_t addr, uintptr_t& out_res, uint64_t& out_off, bool& out_as) const {
        const auto next_it = map.upper_bound(addr);
        if (map.empty() || next_it == map.begin()) return false;
        const auto it = std::prev(next_it);
        const uint64_t off = addr - it->first;
        if (off >= std::get<0>(it->second)) return false;
        out_off = off;
        out_res = std::get<1>(it->second);
        out_as = std::get<2>(it->second);
        return true;
    }

    bool resolve_cached(uint64_t addr, uintptr_t& out_res, uint64_t& out_off, bool& out_as) {
        if (cache.lookup(this, gen, addr, out_res, out_off, out_as)) return true;
        if (cache.dev != this) cache.reset_for_device(this);
        const auto next_it = map.upper_bound(addr);
        if (map.empty() || next_it == map.begin()) return false;
        const auto it = std::prev(next_it);
        const uint64_t start = it->first;
        const uint64_t width = std::get<0>(it->second);
        const uint64_t off = addr - start;
        if (off >= width) return false;
        const uintptr_t res = std::get<1>(it->second);
        const bool is_as = std::get<2>(it->second);
        uint64_t cached_width = width;
        if (next_it != map.end()) {
            const uint64_t gap = next_it->first - start;
            if (gap < cached_width) cached_width = gap;
        }
        if (cached_width != 0) cache.insert(addr, start, cached_width, res, is_as, gen);
        out_off = off; out_res = res; out_as = is_as;
        return true;
    }
};

static void test_gpu_addr_cache_overlapping_placed_buffers() {
    // 1. Exact Codex adversarial sequence: A=[0x10000,0x810000), B=[0x800000,0x830000),
    //    both already registered; query 0x10000 -> 0x820000 -> 0x800000 MUST return B!
    SimGpuAddrDevice dev;
    dev.register_buf(0x10000ull, 0x800000ull, 0x1111, false); // A
    dev.register_buf(0x800000ull, 0x30000ull, 0x2222, true);  // B
    uintptr_t r = 0; uint64_t off = 0; bool is_as = false;
    assert(dev.resolve_cached(0x10000ull, r, off, is_as) && r == 0x1111 && off == 0 && !is_as);
    assert(dev.resolve_cached(0x820000ull, r, off, is_as) && r == 0x2222 && off == 0x20000 && is_as);
    assert(dev.resolve_cached(0x800000ull, r, off, is_as) && r == 0x2222 && off == 0 && is_as);

    // 2. Intermediate placed buffer C=[0x20000,0x30000), then insert/erase D=[0x400000,0x420000)
    dev.register_buf(0x20000ull, 0x10000ull, 0x3333, false);
    assert(dev.resolve_cached(0x10000ull, r, off, is_as) && r == 0x1111);
    assert(!dev.resolve_cached(0x50000ull, r, off, is_as)); // C blocks A in upper_bound!
    dev.register_buf(0x400000ull, 0x20000ull, 0x4444, true);
    assert(dev.resolve_cached(0x400000ull, r, off, is_as) && r == 0x4444 && is_as);
    dev.unregister_buf(0x20000ull, 0x3333); // erasing C exposes A up to D=0x400000
    assert(dev.resolve_cached(0x50000ull, r, off, is_as) && r == 0x1111 && off == 0x40000);
    assert(dev.resolve_cached(0x400000ull, r, off, is_as) && r == 0x4444 && off == 0 && is_as);

    // 3. Randomized fuzzing of overlapping placed buffers vs std::map::upper_bound (5,000 ops)
    SimGpuAddrDevice fuzz;
    std::mt19937_64 rng(0xD3D121234ull);
    std::vector<std::pair<uint64_t, uintptr_t>> live;
    uintptr_t next_res = 0x10000;
    for (int step = 0; step < 5000; ++step) {
        const uint64_t op = rng() % 10;
        if (op < 2 || live.empty()) {
            const uint64_t start = 0x10000ull + (rng() % 64) * 0x10000ull;
            const uint64_t width = (1 + (rng() % 32)) * 0x10000ull;
            const uintptr_t res = next_res++;
            fuzz.register_buf(start, width, res, (rng() & 1) != 0);
            live.push_back({start, res});
        } else if (op == 2 && !live.empty()) {
            const size_t idx = rng() % live.size();
            fuzz.unregister_buf(live[idx].first, live[idx].second);
            live.erase(live.begin() + idx);
        } else {
            const uint64_t addr = 0x10000ull + (rng() % 96) * 0x10000ull + (rng() % 0x10000ull);
            uintptr_t r1 = 0, r2 = 0; uint64_t o1 = 0, o2 = 0; bool a1 = false, a2 = false;
            const bool ok1 = fuzz.resolve_ref(addr, r1, o1, a1);
            const bool ok2 = fuzz.resolve_cached(addr, r2, o2, a2);
            assert(ok1 == ok2);
            if (ok1) assert(r1 == r2 && o1 == o2 && a1 == a2);
        }
    }

    // 4. 200 distinct 64KB buffers hit 100% + 4MB MRU + cross-device isolation
    SimThreadGpuAddrCache cache;
    const int dev_a = 1, dev_b = 2;
    cache.reset_for_device(&dev_a);
    for (uint64_t i = 0; i < 200; ++i) {
        const uint64_t base = 0x100000000ull + i * 0x10000ull;
        cache.insert(base + 0x120, base, 0x10000ull, uintptr_t(0xA000 + i), (i & 1) != 0, 1);
    }
    size_t hits = 0;
    for (uint64_t i = 0; i < 200; ++i) {
        const uint64_t base = 0x100000000ull + i * 0x10000ull;
        if (cache.lookup(&dev_a, 1, base + 0x400, r, off, is_as)) {
            assert(r == uintptr_t(0xA000 + i) && off == 0x400 && is_as == ((i & 1) != 0));
            ++hits;
        }
    }
    assert(hits == 200);
    const uint64_t big_base = 0x200000000ull;
    cache.insert(big_base + 0x100, big_base, 4ull * 1024 * 1024, 0xBEEF, false, 1);
    assert(cache.lookup(&dev_a, 1, big_base + 0x250000ull, r, off, is_as) && r == 0xBEEF && off == 0x250000ull);
    assert(!cache.lookup(&dev_a, 2, big_base + 0x100, r, off, is_as));
    assert(!cache.lookup(&dev_b, 1, big_base + 0x100, r, off, is_as));

    std::printf("PASS: test_gpu_addr_cache_overlapping_placed_buffers (5000 fuzz ops + 200/200 page hits)\n");
}

// ---------------------------------------------------------------------------
// Test 5: Real WWMI_DescriptorPublisher v1/v2/v3 ABI dispatch & single-call guarantee
// ---------------------------------------------------------------------------
struct SimHostPublisher {
    WWMI_DescriptorPublisher_v1 sink{};
    WWMI_DescriptorCopyBatch_v1 sink_batch = nullptr;
    bool active = false;

    static void dummy_raw_after(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t) {}

    int32_t control(uint64_t owner, const WWMI_DescriptorPublisher_v1* next, WWMI_DescriptorRawAfter_v1* raw) {
        if (next) {
            const bool is_v3 = (next->version == WWMI_DESCRIPTOR_BATCH_VERSION);
            const bool valid_v12 = (next->version == 1 || next->version == WWMI_DESCRIPTOR_CONTENT_VERSION) &&
                next->size == sizeof(*next);
            const bool valid_v3 = is_v3 && next->size == sizeof(WWMI_DescriptorPublisher_v3);
            if ((!valid_v12 && !valid_v3) || next->owner != owner || !next->copy || !next->write || !raw) return -1;
            WWMI_DescriptorCopyBatch_v1 batch_fn = nullptr;
            if (valid_v3) {
                batch_fn = reinterpret_cast<const WWMI_DescriptorPublisher_v3*>(next)->copy_batch;
                if (!batch_fn) return -1;
            }
            if (active) return -2;
            sink = *next;
            sink_batch = batch_fn;
            *raw = dummy_raw_after;
            active = true;
            return 0;
        }
        if (!active || sink.owner != owner) return -1;
        sink = {};
        sink_batch = nullptr;
        active = false;
        return 0;
    }

    template<class Forward>
    void copy_batch(uint64_t dev, const WWMI_DescriptorSpan_v1* spans, uint32_t count, uint32_t total_slots, Forward&& forward) {
        struct Once {
            Forward& fn;
            bool called = false;
            int32_t result = 0;
            static int32_t invoke(void* ctx) {
                auto& s = *static_cast<Once*>(ctx);
                if (!s.called) { s.called = true; s.result = s.fn(); }
                return s.result;
            }
        } once{forward};
        if (active) {
            if (sink_batch) {
                sink_batch(dev, spans, count, total_slots, Once::invoke, &once);
            } else {
                struct Slice { const WWMI_DescriptorSpan_v1* items; uint32_t n; } sl{spans, count};
                auto enumerate = [](void* src, WWMI_DescriptorVisitSpan_v1 visit, void* user) {
                    const auto& s = *static_cast<const Slice*>(src);
                    for (uint32_t i = 0; i < s.n; ++i) visit(user, &s.items[i]);
                };
                sink.copy(dev, enumerate, &sl, Once::invoke, &once);
            }
        }
        if (!once.called) Once::invoke(&once);
    }
};

static std::vector<WWMI_DescriptorSpan_v1> g_recorded_spans;
static bool g_skip_native_in_cb = false;
static bool g_call_native_twice_in_cb = false;

static void cb_copy_v2(uint64_t, WWMI_DescriptorSpans_v1 enumerate, void* ctx, WWMI_DescriptorNative_v1 native, void* nctx) {
    enumerate(ctx, [](void*, const WWMI_DescriptorSpan_v1* sp) { g_recorded_spans.push_back(*sp); }, nullptr);
    if (!g_skip_native_in_cb) {
        native(nctx);
        if (g_call_native_twice_in_cb) native(nctx);
    }
}
static void cb_copy_v3_batch(uint64_t, const WWMI_DescriptorSpan_v1* spans, uint32_t count, uint32_t, WWMI_DescriptorNative_v1 native, void* nctx) {
    for (uint32_t i = 0; i < count; ++i) g_recorded_spans.push_back(spans[i]);
    if (!g_skip_native_in_cb) {
        native(nctx);
        if (g_call_native_twice_in_cb) native(nctx);
    }
}
static void cb_write_dummy(uint64_t, const WWMI_DescriptorWrite_v1*, WWMI_DescriptorNative_v1 native, void* nctx) { native(nctx); }

static void test_v1_v2_v3_descriptor_publisher_callback_dispatch() {
    SimHostPublisher host;
    WWMI_DescriptorRawAfter_v1 raw = nullptr;
    constexpr uint64_t kOwner = 0xABCD1234ull;

    // Invalid v3 (null copy_batch or wrong size) MUST fail
    WWMI_DescriptorPublisher_v3 bad_v3{{sizeof(bad_v3), WWMI_DESCRIPTOR_BATCH_VERSION, kOwner, cb_copy_v2, cb_write_dummy}, nullptr};
    assert(host.control(kOwner, &bad_v3.base, &raw) == -1);
    bad_v3.copy_batch = cb_copy_v3_batch;
    bad_v3.base.size = sizeof(WWMI_DescriptorPublisher_v1);
    assert(host.control(kOwner, &bad_v3.base, &raw) == -1);

    // Prepare 4 ordered cross-heap spans
    WWMI_DescriptorSpan_v1 input[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        input[i].destination = {0x100 + i, 1, 0x1000 + i * 32, i * 2, 64, 2, 32};
        input[i].source = {0x200 + (i & 1), 2, 0x2000 + i * 32, i, 64, 3, 32};
        input[i].count = i + 1;
    }

    // 1. Register v3, verify span order & exact single native continuation (normal, skip, double-call)
    WWMI_DescriptorPublisher_v3 good_v3{{sizeof(good_v3), WWMI_DESCRIPTOR_BATCH_VERSION, kOwner, cb_copy_v2, cb_write_dummy}, cb_copy_v3_batch};
    assert(host.control(kOwner, &good_v3.base, &raw) == 0 && raw != nullptr);
    g_recorded_spans.clear(); g_skip_native_in_cb = false; g_call_native_twice_in_cb = true;
    int native_calls = 0;
    host.copy_batch(1, input, 4, 10, [&] { ++native_calls; return 0; });
    assert(native_calls == 1);
    const auto v3_spans = g_recorded_spans;
    assert(v3_spans.size() == 4);

    // Observer skips calling native -> host STILL invokes native exactly once!
    g_skip_native_in_cb = true; g_call_native_twice_in_cb = false;
    native_calls = 0;
    host.copy_batch(1, input, 4, 10, [&] { ++native_calls; return 0; });
    assert(native_calls == 1);
    assert(host.control(kOwner, nullptr, nullptr) == 0);

    // 2. Register v2 fallback, verify identical span order & exact single native continuation
    WWMI_DescriptorPublisher_v1 good_v2{sizeof(good_v2), WWMI_DESCRIPTOR_CONTENT_VERSION, kOwner, cb_copy_v2, cb_write_dummy};
    assert(host.control(kOwner, &good_v2, &raw) == 0);
    g_recorded_spans.clear(); g_skip_native_in_cb = false; g_call_native_twice_in_cb = true;
    native_calls = 0;
    host.copy_batch(1, input, 4, 10, [&] { ++native_calls; return 0; });
    assert(native_calls == 1);
    assert(g_recorded_spans.size() == v3_spans.size());
    for (size_t i = 0; i < v3_spans.size(); ++i)
        assert(std::memcmp(&g_recorded_spans[i], &v3_spans[i], sizeof(WWMI_DescriptorSpan_v1)) == 0);
    assert(host.control(kOwner, nullptr, nullptr) == 0);

    // 3. Disabled publisher still calls native exactly once
    native_calls = 0;
    host.copy_batch(1, input, 4, 10, [&] { ++native_calls; return 0; });
    assert(native_calls == 1);

    std::printf("PASS: test_v1_v2_v3_descriptor_publisher_callback_dispatch\n");
}

// ---------------------------------------------------------------------------
// Test 6: Increment-shift handle decode equivalence (pow2 + non-pow2 + edge cases)
// ---------------------------------------------------------------------------
static uint8_t compute_increment_shift(uint32_t inc) noexcept {
    if (inc == 0 || (inc & (inc - 1u)) != 0) return 0xFFu;
    uint8_t shift = 0;
    while ((inc >> shift) > 1u) ++shift;
    return shift;
}

static bool legacy_fast_slot(uint64_t bytes, uint32_t inc, uint64_t& out) noexcept {
    if (inc == 0) return false;
    if ((inc & (inc - 1u)) == 0) {
        if ((bytes & (inc - 1u)) != 0) return false;
        out = bytes >> std::countr_zero(inc);
        return true;
    }
    if ((bytes % inc) != 0) return false;
    out = bytes / inc;
    return true;
}

static bool new_shift_slot(uint64_t bytes, uint32_t inc, uint8_t shift, uint64_t& out) noexcept {
    if (shift != 0xFFu) {
        if ((bytes & (uint64_t(inc) - 1u)) != 0) return false;
        out = bytes >> shift;
        return true;
    }
    if (inc == 0 || (bytes % inc) != 0) return false;
    out = bytes / inc;
    return true;
}

static void test_increment_shift_decode_equivalence() {
    const uint32_t increments[] = {0, 1, 8, 16, 24, 32, 48, 64, 96, 128, 256};
    for (uint32_t inc : increments) {
        const uint8_t shift = compute_increment_shift(inc);
        for (uint64_t bytes = 0; bytes < 4096; bytes += 4) {
            uint64_t a = 999, b = 999;
            const bool ok_a = legacy_fast_slot(bytes, inc, a);
            const bool ok_b = new_shift_slot(bytes, inc, shift, b);
            assert(ok_a == ok_b);
            if (ok_a) assert(a == b);
        }
    }
    std::printf("PASS: test_increment_shift_decode_equivalence\n");
}

// ---------------------------------------------------------------------------
// Test 7: No-write-allocate LaneRefCache resists cold displaced ring churn
// ---------------------------------------------------------------------------
static void test_lane_ref_cache_no_write_allocate_under_cold_churn() {
    SimReclaimEnv env;
    SimLaneRefCache cache;

    // Deterministically populate a target 2-way set with 2 hot source descriptors
    // and collect 64 cold descriptors that map to that EXACT SAME set!
    // (Old write-allocate release() would evict both hot ways immediately.)
    std::vector<std::unique_ptr<TestContent>> pool;
    std::array<std::vector<TestContent*>, SimLaneRefCache::kSets> by_set;
    size_t target_set = SimLaneRefCache::kSets;
    while (target_set == SimLaneRefCache::kSets) {
        auto o = std::make_unique<TestContent>();
        o->cacheable = 1;
        const size_t idx = SimLaneRefCache::set_idx(o.get());
        by_set[idx].push_back(o.get());
        if (by_set[idx].size() >= 66u) target_set = idx;
        pool.push_back(std::move(o));
    }
    TestContent* hot[2] = { by_set[target_set][0], by_set[target_set][1] };
    std::vector<TestContent*> same_set_cold(by_set[target_set].begin() + 2, by_set[target_set].begin() + 66);
    assert(SimLaneRefCache::set_idx(hot[0]) == target_set);
    assert(SimLaneRefCache::set_idx(hot[1]) == target_set);
    for (TestContent* c : same_set_cold) assert(SimLaneRefCache::set_idx(c) == target_set);

    // Warm BOTH ways of target_set with hot[0] and hot[1]
    uint64_t warm_misses = 0;
    for (TestContent* h : hot) {
        assert(cache.try_retain(h, env, &warm_misses));
        ReleaseN(h, 1, env);
    }
    assert(warm_misses == 2);
    assert(cache.sets[target_set].ways[0].ptr != nullptr && cache.sets[target_set].ways[1].ptr != nullptr);

    // Hammer the EXACT SAME occupied set with 64 colliding cold 1-credit releases!
    for (TestContent* c : same_set_cold) {
        cache.release(c, env);
    }

    // With no-write-allocate, BOTH hot entries MUST still be resident in target_set (0 misses)!
    uint64_t post_churn_misses = 0;
    for (int rep = 0; rep < 16; ++rep) {
        for (TestContent* h : hot) {
            assert(cache.try_retain(h, env, &post_churn_misses));
            ReleaseN(h, 1, env);
        }
    }
    assert(post_churn_misses == 0);

    // Clean shutdown: flush cache and release all remaining creator refs -> every refcount returns to 0
    cache.flush_all(env);
    for (auto& o : pool) {
        const TestContent* p = o.get();
        const bool was_cold = std::find(same_set_cold.begin(), same_set_cold.end(), p) != same_set_cold.end();
        if (!was_cold) ReleaseN(p, 1, env);
    }
    assert(env.double_frees.load() == 0);
    for (auto& o : pool) assert(o->references.load() == 0u);
    std::printf("PASS: test_lane_ref_cache_no_write_allocate_under_cold_churn (target_set=%zu, colliding_cold=64, post_churn_misses=0)\n", target_set);
}

// ---------------------------------------------------------------------------
// Test 8: Shader seqlock fast-path concurrency (InputCollector::tracks_pipeline_bind)
// ---------------------------------------------------------------------------
static void test_shader_seqlock_concurrent_mutations() {
    constexpr size_t kCap = 16;
    std::mutex mtx;
    std::vector<uint64_t> shaders;
    std::atomic<uint64_t> seq{0};
    std::atomic<uint32_t> fast_count{0};
    std::array<std::atomic<uint64_t>, kCap> fast_handles{};
    std::atomic<bool> stop{false};

    auto sync_locked = [&]() {
        const uint64_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        if (shaders.size() <= kCap) {
            for (size_t i = 0; i < shaders.size(); ++i)
                fast_handles[i].store(shaders[i], std::memory_order_relaxed);
            fast_count.store(uint32_t(shaders.size()), std::memory_order_relaxed);
        } else {
            fast_count.store(UINT32_MAX, std::memory_order_relaxed);
        }
        seq.store(s + 2u, std::memory_order_release);
    };

    auto query_fast = [&](uint64_t handle, uint64_t& seen_seq_out, bool& took_fallback_out) -> bool {
        took_fallback_out = false;
        const uint64_t s1 = seq.load(std::memory_order_acquire);
        seen_seq_out = s1;
        if ((s1 & 1u) == 0u) {
            const uint32_t n = fast_count.load(std::memory_order_relaxed);
            if (n <= kCap) {
                bool found = false;
                for (uint32_t i = 0; i < n; ++i) {
                    if (fast_handles[i].load(std::memory_order_relaxed) == handle) { found = true; break; }
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                if (seq.load(std::memory_order_relaxed) == s1) return found;
            }
        }
        took_fallback_out = true;
        std::lock_guard lk(mtx);
        return std::find(shaders.begin(), shaders.end(), handle) != shaders.end();
    };

    // Always keep 0x7777 in shaders; never insert 0x9999; churn others including >kCap overflow!
    {
        std::lock_guard lk(mtx);
        shaders.push_back(0x7777ull);
        sync_locked();
    }

    constexpr uint32_t kReaders = 4;
    constexpr uint64_t kMinSeqChanges = 100;
    constexpr int kMinIters = 4000;
    constexpr int kMaxIters = 200000;
    std::atomic<uint32_t> readers_started{0};
    std::atomic<bool> writer_started{false};
    std::atomic<uint64_t> distinct_seq_observed{0};
    std::atomic<uint64_t> fallback_hits{0};
    std::vector<std::thread> readers;
    for (uint32_t r = 0; r < kReaders; ++r) {
        readers.emplace_back([&] {
            uint64_t prev_seq = 0;
            bool fb = false;
            assert(query_fast(0x7777ull, prev_seq, fb) == true);
            assert(query_fast(0x9999ull, prev_seq, fb) == false);
            readers_started.fetch_add(1, std::memory_order_release);
            while (!writer_started.load(std::memory_order_acquire)) cpu_pause();
            while (!stop.load(std::memory_order_acquire)) {
                uint64_t s_a = 0, s_b = 0;
                bool fb_a = false, fb_b = false;
                assert(query_fast(0x7777ull, s_a, fb_a) == true);
                assert(query_fast(0x9999ull, s_b, fb_b) == false);
                if (s_a != prev_seq) {
                    prev_seq = s_a;
                    distinct_seq_observed.fetch_add(1, std::memory_order_relaxed);
                }
                if (s_b != prev_seq) {
                    prev_seq = s_b;
                    distinct_seq_observed.fetch_add(1, std::memory_order_relaxed);
                }
                if (fb_a || fb_b) {
                    fallback_hits.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    while (readers_started.load(std::memory_order_acquire) < kReaders) cpu_pause();
    writer_started.store(true, std::memory_order_release);

    // Bounded mutation loop: runs at least kMinIters and up to kMaxIters until
    // readers observe >= kMinSeqChanges live publication transitions + fallback hits.
    for (int iter = 0; iter < kMaxIters; ++iter) {
        if (iter >= kMinIters &&
            distinct_seq_observed.load(std::memory_order_relaxed) >= kMinSeqChanges &&
            fallback_hits.load(std::memory_order_relaxed) > 0) {
            break;
        }
        {
            std::lock_guard lk(mtx);
            shaders.clear();
            shaders.push_back(0x7777ull);
            const size_t extra = size_t(iter % 20); // crosses kCap=16 overflow boundary repeatedly
            for (size_t k = 0; k < extra; ++k) shaders.push_back(0x1000ull + k);
            sync_locked();
        }
        if ((iter & 63) == 0) std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();
    assert(distinct_seq_observed.load() >= kMinSeqChanges);
    assert(fallback_hits.load() > 0);
    std::printf("PASS: test_shader_seqlock_concurrent_mutations (seq_transitions=%llu, fallback_hits=%llu)\n",
        (unsigned long long)distinct_seq_observed.load(), (unsigned long long)fallback_hits.load());
}

} // namespace

int main() {
    test_adversarial_same_slot_two_writers_and_readers();
    test_lane_exhaustion_reclamation_and_abandoned_tls_exit();
    test_reset_barrier_with_published_slot();
    test_gpu_addr_cache_overlapping_placed_buffers();
    test_v1_v2_v3_descriptor_publisher_callback_dispatch();
    test_increment_shift_decode_equivalence();
    test_lane_ref_cache_no_write_allocate_under_cold_churn();
    test_shader_seqlock_concurrent_mutations();
    std::printf("OK: test_rhi_descriptor_lockfree_and_batch\n");
    return 0;
}
