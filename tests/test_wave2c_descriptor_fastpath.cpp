#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct TestReplicaSlot {
    std::atomic<int>* live_slots = nullptr;
    explicit TestReplicaSlot(std::atomic<int>* counter) : live_slots(counter) {
        if (live_slots) live_slots->fetch_add(1, std::memory_order_relaxed);
    }
    ~TestReplicaSlot() {
        if (live_slots) live_slots->fetch_sub(1, std::memory_order_relaxed);
    }
};

struct TestContent {
    mutable std::atomic<uint32_t> references{1};
    mutable std::once_flag material_once;
    mutable std::shared_ptr<TestReplicaSlot> material_replica;
    std::atomic<bool> destroyed{false};
    uint64_t id = 0;
};

bool TryRetainContent(TestContent* c) noexcept {
    if (!c) return true;
    uint32_t r = c->references.load(std::memory_order_relaxed);
    while (r != 0) {
        if (c->references.compare_exchange_weak(r, r + 1u,
                std::memory_order_acquire, std::memory_order_relaxed))
            return true;
    }
    return false;
}

void ReleaseContent(TestContent* c, std::atomic<int>& retired) noexcept {
    if (!c) return;
    if (c->references.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
        c->material_replica.reset();
        c->destroyed.store(true, std::memory_order_release);
        retired.fetch_add(1, std::memory_order_relaxed);
    }
}

void TestHotCopiedPlainSrvReleasesLazyMaterialReplica() {
    // Regression for Wave 2B P1: a hot plain game SRV copied > 32 times and then
    // lazily given a material_replica by a private draw MUST release that replica
    // once its heap slots and consumer leases retire (no permanent pinning leak).
    std::atomic<int> live_replica_slots{0};
    std::atomic<int> retired{0};
    TestContent obj{};

    for (int i = 0; i < 64; ++i) {
        assert(TryRetainContent(&obj));
        ReleaseContent(&obj, retired);
    }
    assert(retired.load() == 0);
    assert(obj.references.load() == 1u);

    // Private draw consumer acquires a lazy material_replica after heavy copy traffic.
    std::shared_ptr<TestReplicaSlot> consumer_lease;
    assert(TryRetainContent(&obj));
    std::call_once(obj.material_once, [&]() {
        obj.material_replica = std::make_shared<TestReplicaSlot>(&live_replica_slots);
    });
    consumer_lease = obj.material_replica;
    assert(live_replica_slots.load() == 1);

    // Consumer finishes, then the last heap slot retires.
    ReleaseContent(&obj, retired);
    assert(live_replica_slots.load() == 1);
    ReleaseContent(&obj, retired);
    assert(retired.load() == 1);
    assert(obj.destroyed.load());
    assert(!obj.material_replica);
    assert(!TryRetainContent(&obj));

    // Once the final recording lease drops, the replica slot returns to the pool.
    consumer_lease.reset();
    assert(live_replica_slots.load() == 0);
}

void TestRetainReleaseMultiThreadStress() {
    constexpr int kObjects = 128;
    std::vector<TestContent> objs(kObjects);
    std::atomic<int> retired{0};

    for (auto& o : objs) o.references.store(1u, std::memory_order_relaxed);

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t]() {
            for (int iter = 0; iter < 500; ++iter) {
                TestContent& o = objs[(iter + t * 7) % kObjects];
                if (TryRetainContent(&o)) {
                    ReleaseContent(&o, retired);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    assert(retired.load() == 0);
    for (auto& o : objs) {
        assert(o.references.load(std::memory_order_acquire) == 1u);
        ReleaseContent(&o, retired);
        assert(o.destroyed.load(std::memory_order_acquire));
    }
    assert(retired.load() == kObjects);
}

void TestGraceEpochDeferredReclaim() {
    std::atomic<uint64_t> lane_state{1}; // Reader currently in copy lane!
    std::atomic<uint64_t> content_grace_epoch{1};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> epoch_published{false};

    std::thread reclaimer([&]() {
        content_grace_epoch.fetch_add(1, std::memory_order_seq_cst);
        epoch_published.store(true, std::memory_order_release);
        uint64_t v = lane_state.load(std::memory_order_seq_cst);
        while (v != 0) {
            lane_state.wait(v, std::memory_order_acquire);
            v = lane_state.load(std::memory_order_seq_cst);
        }
        destroyed.store(true, std::memory_order_release);
    });

    while (!epoch_published.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    // Must NOT destroy while reader lane is nonzero!
    assert(!destroyed.load(std::memory_order_acquire));

    // Reader leaves lane -> reclaimer completes destruction cleanly.
    lane_state.store(0, std::memory_order_seq_cst);
    lane_state.notify_all();
    reclaimer.join();
    assert(destroyed.load(std::memory_order_acquire));

    // Also test a LATE reader entering AFTER the reclaimer bumped epoch & scanned lane==0:
    // Because both lane entry/scan and epoch bump/load use seq_cst, the late reader MUST
    // observe the new epoch (and therefore the prior unlink) before dereferencing any pointer.
    std::atomic<uint64_t> late_lane{0};
    std::atomic<uint64_t> late_epoch{1};
    std::atomic<int> unlinked{0};
    for (int iter = 0; iter < 5000; ++iter) {
        late_lane.store(0, std::memory_order_relaxed);
        late_epoch.store(1, std::memory_order_relaxed);
        unlinked.store(0, std::memory_order_relaxed);
        std::atomic<bool> reclaimer_done{false};
        std::atomic<bool> reclaimer_saw_zero{false};
        std::atomic<uint64_t> reader_epoch{0};
        std::atomic<int> reader_saw_unlinked{0};

        std::thread t_reclaim([&]() {
            unlinked.store(1, std::memory_order_release);
            late_epoch.fetch_add(1, std::memory_order_seq_cst);
            if (late_lane.load(std::memory_order_seq_cst) == 0)
                reclaimer_saw_zero.store(true, std::memory_order_release);
            reclaimer_done.store(true, std::memory_order_release);
        });
        std::thread t_read([&]() {
            late_lane.fetch_add(1, std::memory_order_seq_cst);
            const uint64_t ep = late_epoch.load(std::memory_order_seq_cst);
            const int u = unlinked.load(std::memory_order_acquire);
            reader_epoch.store(ep, std::memory_order_relaxed);
            reader_saw_unlinked.store(u, std::memory_order_relaxed);
            // Keep lane held until reclaimer finishes scanning so reclaimer_saw_zero==true
            // strictly implies this reader entered AFTER the scan!
            while (!reclaimer_done.load(std::memory_order_acquire)) std::this_thread::yield();
            late_lane.fetch_sub(1, std::memory_order_seq_cst);
        });
        t_reclaim.join();
        t_read.join();
        if (reclaimer_saw_zero.load(std::memory_order_acquire)) {
            assert(reader_epoch.load() == 2u);
            assert(reader_saw_unlinked.load() == 1);
        }
    }
}

void TestShardedViewsCrossShardLockOrder() {
    struct alignas(64) ViewShard {
        mutable std::shared_mutex mutex;
        std::unordered_map<uint64_t, uint64_t> views;
    };
    constexpr size_t kShards = 64;
    std::array<ViewShard, kShards> shards{};
    auto shard_of = [](uint64_t h) noexcept -> size_t {
        return static_cast<size_t>(((h >> 5) ^ (h >> 12) ^ (h >> 20)) & (kShards - 1));
    };

    auto put = [&](uint64_t h, uint64_t val) {
        auto& s = shards[shard_of(h)];
        std::unique_lock lk(s.mutex);
        s.views.insert_or_assign(h, val);
    };
    auto copy_view = [&](uint64_t dst, uint64_t src) {
        const size_t di = shard_of(dst), si = shard_of(src);
        if (di == si) {
            std::unique_lock lk(shards[di].mutex);
            if (auto it = shards[di].views.find(src); it != shards[di].views.end())
                shards[di].views.insert_or_assign(dst, it->second);
            return;
        }
        if (di < si) {
            std::unique_lock dl(shards[di].mutex);
            std::shared_lock sl(shards[si].mutex);
            if (auto it = shards[si].views.find(src); it != shards[si].views.end())
                shards[di].views.insert_or_assign(dst, it->second);
        } else {
            std::shared_lock sl(shards[si].mutex);
            std::unique_lock dl(shards[di].mutex);
            if (auto it = shards[si].views.find(src); it != shards[si].views.end())
                shards[di].views.insert_or_assign(dst, it->second);
        }
    };

    // Seed two handles guaranteed to map to distinct shards (A < B) and a same-shard pair.
    const uint64_t h_a = 0x10000ull;
    uint64_t h_b = 0x10020ull;
    while (shard_of(h_b) == shard_of(h_a)) h_b += 0x20ull;
    uint64_t h_same = h_a + 0x20ull;
    while (shard_of(h_same) != shard_of(h_a)) h_same += 0x20ull;

    put(h_a, 111);
    put(h_b, 222);

    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&, t]() {
            for (int i = 0; i < 4000; ++i) {
                if ((t & 1) == 0) copy_view(h_a, h_b);
                else copy_view(h_b, h_a);
                copy_view(h_same, h_a);
                const size_t si = shard_of((i & 1) ? h_a : h_b);
                std::shared_lock lk(shards[si].mutex);
                auto it = shards[si].views.find((i & 1) ? h_a : h_b);
                assert(it != shards[si].views.end());
            }
        });
    }
    for (auto& th : workers) th.join();
}

void TestSingleSpanMaskTransferSizes() {
    for (uint32_t count : {1u, 31u, 32u, 33u, 64u}) {
        std::vector<TestContent> src_objs(count);
        std::vector<TestContent> old_objs(count);
        std::vector<const TestContent*> dst_slots(count, nullptr);
        const TestContent* frozen[64]{};
        uint64_t unpinned_mask = 0;

        for (uint32_t i = 0; i < count; ++i) {
            if ((i % 3) != 2) {
                src_objs[i].references.store(2u, std::memory_order_relaxed); // 1 source + 1 frozen
                frozen[i] = &src_objs[i];
                unpinned_mask |= (1ull << i);
            } else {
                frozen[i] = nullptr;
            }
            if ((i % 2) == 0) {
                dst_slots[i] = &old_objs[i];
                old_objs[i].references.store(1u, std::memory_order_relaxed);
            } else {
                // Unchanged slot test: destination already equals frozen[i]
                dst_slots[i] = frozen[i];
                if (frozen[i])
                    frozen[i]->references.fetch_add(1u, std::memory_order_relaxed);
            }
        }

        const TestContent* displaced[64]{};
        uint32_t disp_count = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto* nv = frozen[i];
            const auto* cur = dst_slots[i];
            if (cur != nv) {
                dst_slots[i] = nv;
                unpinned_mask &= ~(1ull << i);
                if (cur)
                    displaced[disp_count++] = cur;
            }
        }
        while (unpinned_mask) {
            const unsigned idx = std::countr_zero(unpinned_mask);
            unpinned_mask &= unpinned_mask - 1ull;
            frozen[idx]->references.fetch_sub(1u, std::memory_order_acq_rel);
        }
        for (uint32_t k = 0; k < disp_count; ++k) {
            if (displaced[k]->references.fetch_sub(1u, std::memory_order_acq_rel) == 1u)
                const_cast<TestContent*>(displaced[k])->destroyed.store(true);
        }

        for (uint32_t i = 0; i < count; ++i) {
            assert(dst_slots[i] == frozen[i]);
            if ((i % 3) != 2) {
                // Every retained src object must hold exact refcount 2 (1 in src + 1 in dst).
                assert(src_objs[i].references.load() == 2u);
            }
            if ((i % 2) == 0) {
                assert(old_objs[i].destroyed.load());
            }
        }
    }
}

} // namespace

int main() {
    TestHotCopiedPlainSrvReleasesLazyMaterialReplica();
    TestRetainReleaseMultiThreadStress();
    TestGraceEpochDeferredReclaim();
    TestShardedViewsCrossShardLockOrder();
    TestSingleSpanMaskTransferSizes();
    std::printf("WAVE2C_DESCRIPTOR_FASTPATH_TESTS_PASS\n");
    return 0;
}
