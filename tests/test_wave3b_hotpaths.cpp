#include "../source/client/runtime_lease.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct FakeRuntime {
    static constexpr std::uint64_t kAliveMagic = 0x57574d4933424f4bULL;
    static constexpr std::uint64_t kDeadMagic  = 0xdeadbeefdeadbeefULL;

    std::atomic<std::uint64_t> magic{kAliveMagic};
    std::uint64_t native_id = 0;

    explicit FakeRuntime(std::uint64_t id) : native_id(id) {}
    ~FakeRuntime() { magic.store(kDeadMagic, std::memory_order_release); }
};

void test_runtime_lease_basic_and_reentrant() {
    hsin_rt::PrimaryRuntimeTable<FakeRuntime, void *, std::uint64_t> table;
    void *api1 = reinterpret_cast<void *>(0x1001);
    void *api2 = reinterpret_cast<void *>(0x2002);
    auto r1 = std::make_unique<FakeRuntime>(0xAA01);

    assert(!table.has_primary());
    assert(!table.try_acquire_api(api1));
    assert(!table.try_acquire_native(0xAA01));

    assert(table.publish_primary_locked(api1, r1->native_id, r1.get()));
    assert(table.has_primary());

    {
        auto l1 = table.try_acquire_api(api1);
        assert(l1 && l1.get() == r1.get());
        // Wrong keys miss cleanly
        assert(!table.try_acquire_api(api2));
        assert(!table.try_acquire_native(0xBB02));

        // Re-entrant nested lease on the same thread + move construction
        auto l2 = table.try_acquire_native(0xAA01);
        assert(l2 && l2->native_id == 0xAA01);
        auto l3 = std::move(l2);
        assert(!l2 && l3 && l3.get() == r1.get());
    }

    // Mismatched unpublish does nothing
    FakeRuntime other(0x9999);
    assert(!table.begin_unpublish_locked(&other));
    assert(table.has_primary());

    assert(table.begin_unpublish_locked(r1.get()));
    assert(!table.has_primary());
    assert(!table.try_acquire_api(api1));
    table.finish_drain_unlocked();

    // Can re-publish after drain finishes
    assert(table.publish_primary_locked(api1, r1->native_id, r1.get()));
    assert(table.try_acquire_api(api1).get() == r1.get());
    assert(table.begin_unpublish_locked());
    table.finish_drain_unlocked();
}

void test_runtime_lease_drain_waits_for_active_reader() {
    hsin_rt::PrimaryRuntimeTable<FakeRuntime, void *, std::uint64_t> table;
    void *api1 = reinterpret_cast<void *>(0x3003);
    auto r1 = std::make_unique<FakeRuntime>(0xCC03);
    assert(table.publish_primary_locked(api1, r1->native_id, r1.get()));

    std::atomic<bool> reader_holding{false};
    std::atomic<bool> release_reader{false};
    std::atomic<bool> drain_done{false};

    std::thread reader([&] {
        auto lease = table.try_acquire_api(api1);
        assert(lease && lease->magic.load(std::memory_order_acquire) == FakeRuntime::kAliveMagic);
        reader_holding.store(true, std::memory_order_release);
        while (!release_reader.load(std::memory_order_acquire))
            std::this_thread::yield();
        assert(lease->magic.load(std::memory_order_acquire) == FakeRuntime::kAliveMagic);
    });

    while (!reader_holding.load(std::memory_order_acquire))
        std::this_thread::yield();

    std::thread destroyer([&] {
        assert(table.begin_unpublish_locked(r1.get()));
        table.finish_drain_unlocked();
        drain_done.store(true, std::memory_order_release);
        r1.reset();
    });

    // Give destroyer time to enter drain loop; it MUST NOT complete while reader holds lease.
    for (int i = 0; i < 200; ++i) {
        std::this_thread::yield();
        assert(!drain_done.load(std::memory_order_acquire));
    }

    release_reader.store(true, std::memory_order_release);
    reader.join();
    destroyer.join();
    assert(drain_done.load(std::memory_order_acquire));
}

void test_runtime_lease_concurrent_lifecycle_stress() {
    hsin_rt::PrimaryRuntimeTable<FakeRuntime, void *, std::uint64_t> table;
    std::shared_mutex devices_mutex;
    std::unordered_map<void *, std::shared_ptr<FakeRuntime>> devices;

    auto lookup_api = [&](void *api) {
        if (auto fast = table.try_acquire_api(api))
            return fast;
        std::shared_lock lock(devices_mutex);
        auto it = devices.find(api);
        return hsin_rt::PrimaryRuntimeTable<FakeRuntime, void *, std::uint64_t>::Lease(
            it == devices.end() ? nullptr : it->second);
    };
    auto lookup_native = [&](std::uint64_t native) {
        if (auto fast = table.try_acquire_native(native))
            return fast;
        std::shared_lock lock(devices_mutex);
        for (const auto &[_, r] : devices)
            if (r->native_id == native)
                return hsin_rt::PrimaryRuntimeTable<FakeRuntime, void *, std::uint64_t>::Lease(r);
        return hsin_rt::PrimaryRuntimeTable<FakeRuntime, void *, std::uint64_t>::Lease{};
    };

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> hits{0};
    void *api1 = reinterpret_cast<void *>(0x4004);
    void *api2 = reinterpret_cast<void *>(0x4008);

    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&, t] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto lease = (t & 1) ? lookup_api(api1) : lookup_native(0xDD01);
                if (lease) {
                    assert(lease->magic.load(std::memory_order_acquire) == FakeRuntime::kAliveMagic);
                    hits.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (int iter = 0; iter < 150; ++iter) {
        auto r1 = std::make_shared<FakeRuntime>(0xDD01);
        {
            std::unique_lock lock(devices_mutex);
            devices.emplace(api1, r1);
            assert(table.publish_primary_locked(api1, r1->native_id, r1.get()));
        }
        std::this_thread::yield();

        // Every 10th iteration, exercise 1 -> 2 multi-device unpublish fallback
        if ((iter % 10) == 0) {
            auto r2 = std::make_shared<FakeRuntime>(0xDD02);
            bool drain = false;
            {
                std::unique_lock lock(devices_mutex);
                devices.emplace(api2, r2);
                drain = table.begin_unpublish_locked();
            }
            if (drain) table.finish_drain_unlocked();
            assert(!table.has_primary());
            {
                std::unique_lock lock(devices_mutex);
                devices.erase(api2);
            }
        }

        std::shared_ptr<FakeRuntime> dying;
        bool drain = false;
        {
            std::unique_lock lock(devices_mutex);
            auto it = devices.find(api1);
            dying = std::move(it->second);
            devices.erase(it);
            drain = table.begin_unpublish_locked(dying.get());
        }
        if (drain) table.finish_drain_unlocked();
        // Safe to poison/destroy now: zero fast leases remain, slow leases hold shared_ptr
        if (dying.use_count() == 1)
            dying->magic.store(FakeRuntime::kDeadMagic, std::memory_order_release);
        dying.reset();
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto &th : readers) th.join();
    assert(hits.load() > 0);
}

// Mirrors host device_impl's flat atomic + overflow descriptor heap table.
struct FlatHeapTableModel {
    static constexpr std::size_t kFast = 64;
    std::unique_ptr<std::atomic<void *>[]> fast;
    std::vector<void *> overflow;
    std::atomic<std::size_t> count{0};
    mutable std::shared_mutex mtx;

    FlatHeapTableModel() : fast(std::make_unique<std::atomic<void *>[]>(kFast)) {
        for (std::size_t i = 0; i < kFast; ++i)
            fast[i].store(nullptr, std::memory_order_relaxed);
    }

    std::size_t reg(void *p) {
        std::unique_lock lock(mtx);
        const std::size_t idx = count.load(std::memory_order_relaxed);
        if (idx < kFast)
            fast[idx].store(p, std::memory_order_release);
        else
            overflow.push_back(p);
        count.store(idx + 1, std::memory_order_release);
        return idx;
    }

    void unreg(std::size_t idx, void *expected) {
        std::unique_lock lock(mtx);
        assert(at_locked(idx) == expected);
        if (idx < kFast)
            fast[idx].store(nullptr, std::memory_order_release);
        else
            overflow[idx - kFast] = nullptr;
    }

    void *at(std::size_t idx) const noexcept {
        if (idx < kFast)
            return fast[idx].load(std::memory_order_acquire);
        std::shared_lock lock(mtx);
        const std::size_t oi = idx - kFast;
        return oi < overflow.size() ? overflow[oi] : nullptr;
    }

    void *at_locked(std::size_t idx) const noexcept {
        if (idx < kFast)
            return fast[idx].load(std::memory_order_acquire);
        const std::size_t oi = idx - kFast;
        return oi < overflow.size() ? overflow[oi] : nullptr;
    }
};

void test_flat_descriptor_heap_table() {
    FlatHeapTableModel tbl;
    std::vector<int> dummies(120);
    for (std::size_t i = 0; i < dummies.size(); ++i) {
        dummies[i] = static_cast<int>(i + 1);
        const std::size_t idx = tbl.reg(&dummies[i]);
        assert(idx == i);
        assert(tbl.at(idx) == &dummies[i]);
    }
    // Unregister both fast and overflow slots; indices must never be recycled
    tbl.unreg(5, &dummies[5]);
    tbl.unreg(90, &dummies[90]);
    assert(tbl.at(5) == nullptr);
    assert(tbl.at(90) == nullptr);
    assert(tbl.at(6) == &dummies[6]);
    assert(tbl.at(91) == &dummies[91]);
    int extra = 999;
    assert(tbl.reg(&extra) == 120);
    assert(tbl.at(120) == &extra);
    assert(tbl.at(5) == nullptr);
    assert(tbl.at(9999) == nullptr);
}

// Verify 1-heap single-page vs multi-page dedup matches generic multi-heap dedup.
void test_single_heap_page_dedup_equivalence() {
    constexpr std::uint32_t kPageSlots = 32;
    constexpr std::uint32_t kMaxPages = 32;
    struct Span { std::uint32_t first, count; };

    auto collect_single = [](const std::vector<Span> &spans) {
        std::vector<std::uint32_t> pages;
        for (const auto &sp : spans) {
            const std::uint32_t p0 = sp.first / kPageSlots;
            const std::uint32_t p1 = (sp.first + sp.count - 1u) / kPageSlots;
            for (std::uint32_t p = p0; p <= p1; ++p) {
                if (pages.size() == 1 && pages[0] == p) continue;
                bool seen = false;
                for (auto q : pages) if (q == p) { seen = true; break; }
                if (!seen) {
                    assert(pages.size() < kMaxPages);
                    pages.push_back(p);
                }
            }
        }
        return pages;
    };

    auto collect_generic = [](const std::vector<Span> &spans) {
        std::vector<std::uint32_t> pages;
        for (const auto &sp : spans) {
            const std::uint32_t p0 = sp.first / kPageSlots;
            const std::uint32_t p1 = (sp.first + sp.count - 1u) / kPageSlots;
            for (std::uint32_t p = p0; p <= p1; ++p) {
                bool seen = false;
                for (auto q : pages) if (q == p) { seen = true; break; }
                if (!seen) pages.push_back(p);
            }
        }
        return pages;
    };

    // Case 1: all spans inside page 0
    std::vector<Span> s1{{0, 4}, {8, 2}, {30, 2}};
    assert(collect_single(s1) == collect_generic(s1));
    assert(collect_single(s1).size() == 1);

    // Case 2: straddling page boundaries & repeated pages
    std::vector<Span> s2{{30, 5}, {0, 3}, {63, 4}, {32, 2}, {95, 2}};
    assert(collect_single(s2) == collect_generic(s2));
    assert(collect_single(s2).size() == 4);
}

} // namespace

int main() {
    test_runtime_lease_basic_and_reentrant();
    test_runtime_lease_drain_waits_for_active_reader();
    test_runtime_lease_concurrent_lifecycle_stress();
    test_flat_descriptor_heap_table();
    test_single_heap_page_dedup_equivalence();
    std::puts("OK: test_wave3b_hotpaths");
    return 0;
}
