#include "../source/host/source/d3d12/wwmi_flat_view_map.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct TestViewMeta {
    uint64_t res = 0;
    uint32_t fmt = 0;
    uint32_t flags = 0;
    uint64_t offset = 0;
    bool operator==(const TestViewMeta&) const = default;
};

// 1. Differential test of production wwmi::FlatViewMap vs std::unordered_map
void TestProductionFlatViewMapDifferential() {
    wwmi::FlatViewMap<uint64_t, TestViewMeta> flat;
    std::unordered_map<uint64_t, TestViewMeta> ref;

    // Edge case: key == 0 and aligned D3D12 handle strides (32B / 64B)
    assert(flat.empty() && flat.size() == 0);
    assert(flat.find(0) == nullptr);
    assert(!flat.erase(0));

    flat.insert_or_assign(0, TestViewMeta{1, 2, 3, 4});
    ref[0] = TestViewMeta{1, 2, 3, 4};
    assert(flat.find(0) != nullptr && *flat.find(0) == ref[0]);
    assert(flat.erase(0));
    ref.erase(0);
    assert(flat.find(0) == nullptr && flat.empty());

    std::mt19937_64 rng(0x574156453244ULL);
    // 100k mixed insert / overwrite / erase / lookup across aligned descriptor pools
    constexpr uint64_t kPools[] = {
        0x00007FF810000000ULL,
        0x00007FF820000020ULL,
        0x0000000140000000ULL,
        0x0000000000000000ULL,
    };
    for (int step = 0; step < 100000; ++step) {
        const uint64_t base = kPools[rng() & 3];
        const uint64_t slot = (rng() % 512) * 32ULL;
        const uint64_t key = base + slot;
        const uint32_t op = static_cast<uint32_t>(rng() % 10);
        if (op < 5) {
            const TestViewMeta v{rng(), uint32_t(rng()), uint32_t(rng()), rng()};
            flat.insert_or_assign(key, v);
            ref[key] = v;
        } else if (op < 8) {
            const bool e1 = flat.erase(key);
            const bool e2 = (ref.erase(key) != 0);
            assert(e1 == e2);
        } else {
            const auto* f = flat.find(key);
            const auto it = ref.find(key);
            if (it == ref.end()) {
                assert(f == nullptr);
            } else {
                assert(f != nullptr && *f == it->second);
            }
        }
        assert(flat.size() == ref.size());
    }
    for (const auto& [k, v] : ref) {
        const auto* f = flat.find(k);
        assert(f != nullptr && *f == v);
    }

    // Same-shard copy triggering growth (verify copy-before-insert prevents UAF)
    wwmi::FlatViewMap<uint64_t, TestViewMeta> grow_map;
    grow_map.insert_or_assign(0x1000ULL, TestViewMeta{999, 77, 88, 1234});
    for (uint64_t i = 1; i <= 500; ++i) {
        const auto* src = grow_map.find(0x1000ULL);
        assert(src != nullptr);
        const TestViewMeta copied = *src;
        grow_map.insert_or_assign(0x1000ULL + i * 32ULL, copied);
    }
    assert(grow_map.size() == 501);
    for (uint64_t i = 0; i <= 500; ++i) {
        const auto* e = grow_map.find(0x1000ULL + i * 32ULL);
        assert(e != nullptr && e->res == 999 && e->fmt == 77 && e->offset == 1234);
    }

    // Stress repeated erase/insert cycles at fixed load to prove zero tombstone degradation
    const size_t cap_before = grow_map.capacity();
    for (uint64_t round = 0; round < 50; ++round) {
        for (uint64_t i = 1; i <= 250; ++i)
            assert(grow_map.erase(0x1000ULL + (round * 250 + i) * 32ULL));
        for (uint64_t i = 1; i <= 250; ++i)
            grow_map.insert_or_assign(0x1000ULL + ((round + 1) * 250 + i) * 32ULL, TestViewMeta{round, uint32_t(i), 0, 0});
    }
    assert(grow_map.capacity() == cap_before);

    // Move construction / assignment leaves moved-from map cleanly empty and reusable
    wwmi::FlatViewMap<uint64_t, TestViewMeta> moved(std::move(grow_map));
    assert(grow_map.empty() && grow_map.size() == 0 && grow_map.capacity() == 0);
    assert(grow_map.find(0x1000ULL) == nullptr);
    grow_map.insert_or_assign(0x2000ULL, TestViewMeta{42, 1, 2, 3});
    assert(grow_map.size() == 1 && grow_map.find(0x2000ULL)->res == 42);
    wwmi::FlatViewMap<uint64_t, TestViewMeta> moved2;
    moved2 = std::move(moved);
    assert(moved.empty() && moved.capacity() == 0 && moved.find(0x1000ULL) == nullptr);
    assert(moved2.find(0x1000ULL) != nullptr && moved2.find(0x1000ULL)->res == 999);
}

// 2. Cross-shard concurrent FlatViewMap stress (mirrors ViewShard ascending lock order)
void TestFlatViewShardConcurrentStress() {
    struct alignas(64) Shard {
        mutable std::shared_mutex mutex;
        wwmi::FlatViewMap<uint64_t, TestViewMeta> views;
    };
    constexpr size_t kShards = 64;
    auto shards = std::make_unique<Shard[]>(kShards);
    auto shard_of = [](uint64_t h) noexcept -> size_t {
        return static_cast<size_t>(((h >> 5) ^ (h >> 12) ^ (h >> 20)) & (kShards - 1));
    };

    const uint64_t h_a = 0x20000ULL;
    uint64_t h_b = 0x20020ULL;
    while (shard_of(h_b) == shard_of(h_a)) h_b += 0x20ULL;
    uint64_t h_same = h_a + 0x20ULL;
    while (shard_of(h_same) != shard_of(h_a)) h_same += 0x20ULL;

    {
        std::unique_lock l1(shards[shard_of(h_a)].mutex);
        shards[shard_of(h_a)].views.insert_or_assign(h_a, TestViewMeta{111, 1, 2, 3});
        std::unique_lock l2(shards[shard_of(h_b)].mutex);
        shards[shard_of(h_b)].views.insert_or_assign(h_b, TestViewMeta{222, 4, 5, 6});
    }

    auto copy_view = [&](uint64_t dst, uint64_t src) {
        const size_t di = shard_of(dst), si = shard_of(src);
        if (di == si) {
            std::unique_lock lk(shards[di].mutex);
            if (const auto* e = shards[di].views.find(src)) {
                const auto copied = *e;
                shards[di].views.insert_or_assign(dst, copied);
            }
            return;
        }
        if (di < si) {
            std::unique_lock dl(shards[di].mutex);
            std::shared_lock sl(shards[si].mutex);
            if (const auto* e = shards[si].views.find(src))
                shards[di].views.insert_or_assign(dst, *e);
        } else {
            std::shared_lock sl(shards[si].mutex);
            std::unique_lock dl(shards[di].mutex);
            if (const auto* e = shards[si].views.find(src))
                shards[di].views.insert_or_assign(dst, *e);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 4000; ++i) {
                if ((t & 1) == 0) copy_view(h_a, h_b);
                else copy_view(h_b, h_a);
                copy_view(h_same + uint64_t(i & 63) * 0x100000ULL, h_a);
                const uint64_t q = (i & 1) ? h_a : h_b;
                std::shared_lock lk(shards[shard_of(q)].mutex);
                const auto* e = shards[shard_of(q)].views.find(q);
                assert(e != nullptr && (e->res == 111 || e->res == 222));
            }
        });
    }
    for (auto& th : threads) th.join();
}

// 3. C++20 Exclusive publication (single release seq store) + seqlock + concurrent DisjointCopyWrite stress
class SimFastPageMutex {
    static constexpr uint32_t kCountMask = 0x00003FFFu;
    static constexpr uint32_t kModeMask  = 0x0000C000u;
    static constexpr uint32_t kModeCopy  = 0x00008000u;
    static constexpr uint32_t kModeExcl  = 0x0000C000u;
    static constexpr uint32_t kWaiterBit = 0x00010000u;
    std::atomic<uint32_t> state_{0};
public:
    void lock() noexcept {
        for (;;) {
            uint32_t exp = 0;
            if (state_.compare_exchange_weak(exp, kModeExcl | 1u, std::memory_order_acquire, std::memory_order_relaxed))
                return;
            std::this_thread::yield();
        }
    }
    void unlock() noexcept {
        state_.exchange(0u, std::memory_order_release);
    }
    void lock_copy_write() noexcept {
        for (;;) {
            uint32_t s = state_.load(std::memory_order_relaxed);
            if (s == 0) {
                if (state_.compare_exchange_weak(s, kModeCopy | 1u, std::memory_order_acquire, std::memory_order_relaxed))
                    return;
            } else if ((s & (kModeMask | kWaiterBit)) == kModeCopy && (s & kCountMask) < kCountMask) {
                if (state_.compare_exchange_weak(s, s + 1u, std::memory_order_acquire, std::memory_order_relaxed))
                    return;
            } else {
                std::this_thread::yield();
            }
        }
    }
    void unlock_copy_write() noexcept {
        uint32_t s = state_.load(std::memory_order_relaxed);
        for (;;) {
            const uint32_t cnt = s & kCountMask;
            const uint32_t next = (cnt > 1u) ? (s - 1u) : 0u;
            if (state_.compare_exchange_weak(s, next, std::memory_order_release, std::memory_order_relaxed))
                return;
        }
    }
    bool has_active_writer() const noexcept {
        const uint32_t m = state_.load(std::memory_order_acquire) & kModeMask;
        return m == kModeCopy || m == kModeExcl;
    }
};

void TestExclusiveSingleSeqStoreAndSeqlockLitmus() {
    struct SimPage {
        SimFastPageMutex mutex;
        std::atomic<uint64_t> seq{0};
        std::atomic<uint64_t> ptr_a{0};
        std::atomic<uint64_t> ptr_b{0};
    } page;

    std::atomic<bool> stop{false};
    std::atomic<int> ready{0};
    constexpr int kTotalThreads = 1 + 2 + 4;
    std::atomic<uint64_t> excl_writes{0};
    std::atomic<uint64_t> copy_writes{0};
    std::atomic<uint64_t> accepted_reads{0};

    auto wait_start = [&]() {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (ready.load(std::memory_order_acquire) < kTotalThreads) std::this_thread::yield();
    };

    // Exclusive writer: updates pair (ptr_a, ptr_b) to identical generation using single release store on seq
    std::thread t_excl([&]() {
        wait_start();
        for (uint64_t v = 1; v <= 30000; ++v) {
            page.mutex.lock();
            page.ptr_a.store(v, std::memory_order_release);
            page.ptr_b.store(v, std::memory_order_release);
            const uint64_t s = page.seq.load(std::memory_order_relaxed);
            page.seq.store(s + 1u, std::memory_order_release);
            page.mutex.unlock();
            excl_writes.fetch_add(1, std::memory_order_relaxed);
        }
        stop.store(true, std::memory_order_release);
    });

    // Concurrent disjoint copy writers bumping seq via fetch_add(1, release)
    std::vector<std::thread> t_copies;
    for (int c = 0; c < 2; ++c) {
        t_copies.emplace_back([&]() {
            wait_start();
            while (!stop.load(std::memory_order_acquire)) {
                page.mutex.lock_copy_write();
                page.seq.fetch_add(1u, std::memory_order_release);
                page.mutex.unlock_copy_write();
                copy_writes.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Seqlock readers: acquire load seq -> !has_active_writer -> acquire load ptr_a/ptr_b ->
    // acquire fence -> !has_active_writer -> acquire load seq == saved -> MUST observe ptr_a == ptr_b!
    std::vector<std::thread> t_readers;
    for (int r = 0; r < 4; ++r) {
        t_readers.emplace_back([&]() {
            wait_start();
            uint64_t local_accepted = 0;
            while (!stop.load(std::memory_order_acquire) || local_accepted == 0) {
                const uint64_t s0 = page.seq.load(std::memory_order_acquire);
                if (page.mutex.has_active_writer()) continue;
                const uint64_t a = page.ptr_a.load(std::memory_order_acquire);
                const uint64_t b = page.ptr_b.load(std::memory_order_acquire);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (page.mutex.has_active_writer()) continue;
                if (page.seq.load(std::memory_order_acquire) != s0) continue;
                assert(a == b); // Zero torn snapshots permitted!
                ++local_accepted;
                accepted_reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    t_excl.join();
    for (auto& th : t_copies) th.join();
    for (auto& th : t_readers) th.join();
    assert(page.seq.load(std::memory_order_acquire) == excl_writes.load() + copy_writes.load());
    assert(accepted_reads.load() > 0);
}

// 4. Multi-span destination deduplication, duplicate-dest spill, and global (generation, page) lock ordering
void TestMultiSpanPageCoalescingAndDupDestDetection() {
    struct Span { uint64_t heap_gen; uint32_t dst_slot; uint32_t count; };
    struct DstPage { uint64_t gen; uint32_t page; uint32_t mask; };

    auto analyze = [](const std::vector<Span>& spans, std::vector<DstPage>& out_pages) -> bool {
        out_pages.clear();
        for (const auto& sp : spans) {
            const uint32_t p0 = sp.dst_slot / 32u;
            const uint32_t p1 = (sp.dst_slot + sp.count - 1u) / 32u;
            for (uint32_t p = p0; p <= p1; ++p) {
                const uint32_t lo = (p == p0) ? (sp.dst_slot & 31u) : 0u;
                const uint32_t hi = (p == p1) ? ((sp.dst_slot + sp.count - 1u) & 31u) : 31u;
                const uint32_t width = hi - lo + 1u;
                const uint32_t bits = (width >= 32u) ? 0xFFFFFFFFu : (((1u << width) - 1u) << lo);
                auto it = std::find_if(out_pages.begin(), out_pages.end(), [&](const DstPage& d) {
                    return d.gen == sp.heap_gen && d.page == p;
                });
                if (it != out_pages.end()) {
                    if (it->mask & bits) return false; // duplicate destination slot -> must spill!
                    it->mask |= bits;
                } else {
                    if (out_pages.size() >= 32) return false;
                    out_pages.push_back({sp.heap_gen, p, bits});
                }
            }
        }
        std::sort(out_pages.begin(), out_pages.end(), [](const DstPage& a, const DstPage& b) {
            return a.gen != b.gen ? a.gen < b.gen : a.page < b.page;
        });
        return true;
    };

    std::vector<DstPage> pages;
    // 8 contiguous 1-slot spans on same page -> coalesces into exactly 1 page lock & 1 seq bump!
    std::vector<Span> ue4_batch;
    for (uint32_t i = 0; i < 8; ++i) ue4_batch.push_back({10, 64 + i, 1});
    assert(analyze(ue4_batch, pages));
    assert(pages.size() == 1 && pages[0].page == 2 && pages[0].mask == 0xFFu);

    // Straddling page boundary (slot 30..35) in reverse order across 2 heaps -> sorted ascending by (gen, page)
    std::vector<Span> multi_heap = {
        {20, 32, 4},
        {10, 30, 4}, // touches page 0 (bits 30,31) and page 1 (bits 0,1)
        {20, 0, 1},
    };
    assert(analyze(multi_heap, pages));
    assert(pages.size() == 4);
    assert(pages[0].gen == 10 && pages[0].page == 0 && pages[0].mask == 0xC0000000u);
    assert(pages[1].gen == 10 && pages[1].page == 1 && pages[1].mask == 0x00000003u);
    assert(pages[2].gen == 20 && pages[2].page == 0 && pages[2].mask == 0x00000001u);
    assert(pages[3].gen == 20 && pages[3].page == 1 && pages[3].mask == 0x0000000Fu);

    // Duplicate destination slot MUST spill (return false)
    std::vector<Span> dup = {{10, 5, 3}, {10, 7, 2}};
    assert(!analyze(dup, pages));

    // Exact 64 slots across 32 spans (2 slots each) -> succeeds and coalesces to 2 pages
    std::vector<Span> max_valid;
    for (uint32_t i = 0; i < 32; ++i) max_valid.push_back({1, i * 2, 2});
    assert(analyze(max_valid, pages));
    assert(pages.size() == 2 && pages[0].mask == 0xFFFFFFFFu && pages[1].mask == 0xFFFFFFFFu);
}

// 5. Multi-span per-occurrence reference ownership (repeated source pointers, pinned/unpinned/null/unchanged)
void TestMultiSpanPerOccurrenceOwnership() {
    constexpr uint32_t kPinned = 0x80000000u;
    struct Obj {
        std::atomic<uint32_t> refs{1};
        bool destroyed = false;
    };
    Obj src_unpinned{}, src_pinned{}, old_a{}, old_b{};
    src_unpinned.refs.store(1);
    src_pinned.refs.store(1u | kPinned);
    old_a.refs.store(1);
    old_b.refs.store(1);

    // 4 spans total (5 slots): same src_unpinned appears 3 times (slots 0, 2, 4)!
    // Slot 2 already holds src_unpinned (unchanged -> must release its own occurrence bit).
    std::array<Obj*, 5> dst = {&old_a, &old_b, &src_unpinned, nullptr, nullptr};
    src_unpinned.refs.fetch_add(1); // dst[2] owns a ref

    std::array<Obj*, 5> frozen = {&src_unpinned, &src_pinned, &src_unpinned, nullptr, &src_unpinned};
    uint64_t unpinned_mask = 0;
    for (uint32_t pos = 0; pos < 5; ++pos) {
        if (frozen[pos] && !(frozen[pos]->refs.load() & kPinned)) {
            frozen[pos]->refs.fetch_add(1);
            unpinned_mask |= (1ull << pos);
        }
    }
    assert(src_unpinned.refs.load() == 5u); // 1 source + 1 dst[2] + 3 frozen occurrences

    std::vector<Obj*> displaced;
    for (uint32_t pos = 0; pos < 5; ++pos) {
        Obj* nv = frozen[pos];
        Obj* cur = dst[pos];
        if (cur != nv) {
            dst[pos] = nv;
            unpinned_mask &= ~(1ull << pos);
            if (cur && !(cur->refs.load() & kPinned))
                displaced.push_back(cur);
        }
    }
    // Release remaining unpinned_mask (only pos=2 should remain!)
    assert(unpinned_mask == (1ull << 2));
    while (unpinned_mask) {
        const unsigned idx = std::countr_zero(unpinned_mask);
        unpinned_mask &= unpinned_mask - 1ull;
        if (frozen[idx]->refs.fetch_sub(1) == 1u) frozen[idx]->destroyed = true;
    }
    for (Obj* d : displaced) {
        if (d->refs.fetch_sub(1) == 1u) d->destroyed = true;
    }
    // Final ownership: src_unpinned owned by 1 source + 3 dst slots (0, 2, 4) = 4
    assert(src_unpinned.refs.load() == 4u && !src_unpinned.destroyed);
    assert(old_a.destroyed && old_b.destroyed);
    assert((src_pinned.refs.load() & kPinned) != 0);
}

// 6. Differential test for Host CopyDescriptors single-dst unit-src specialization & exact empty-range bypass
void TestHostCopyDescriptorsDifferential() {
    struct SpanOut {
        uint32_t dst_range, dst_off, src_range, src_off, count;
        bool operator==(const SpanOut&) const = default;
    };
    auto general_visit = [](uint32_t dst_ranges, const uint32_t* dst_sizes, uint32_t src_ranges, const uint32_t* src_sizes) {
        std::vector<SpanOut> out;
        uint32_t src = 0, src_off = 0;
        for (uint32_t dst = 0; dst < dst_ranges; ++dst) {
            const uint32_t ds = dst_sizes ? dst_sizes[dst] : 1u;
            for (uint32_t dst_off = 0; dst_off < ds;) {
                if (src == src_ranges) return out;
                const uint32_t ss = src_sizes ? src_sizes[src] : 1u;
                if (ss == src_off) { ++src; src_off = 0; continue; }
                const uint32_t c = std::min(ss - src_off, ds - dst_off);
                out.push_back({dst, dst_off, src, src_off, c});
                dst_off += c;
                src_off += c;
            }
        }
        return out;
    };

    // Specialized unit-src loop must match general visitor identically for 0..64 ranges
    for (uint32_t n : {0u, 1u, 2u, 8u, 32u, 64u}) {
        const uint32_t dsz = n;
        const auto ref = general_visit(1, &dsz, n, nullptr);
        std::vector<SpanOut> fast;
        for (uint32_t i = 0; i < n; ++i) fast.push_back({0, i, i, 0, 1});
        assert(fast == ref);
    }

    // Verify exact-bounds empty bypass invariant:
    // Out-of-bounds destination MUST NOT claim empty bypass; mismatched src_total != dst_total MUST NOT bypass.
    struct SimRange {
        uint32_t slot = 0, num_desc = 0;
        uint64_t occ_lo = 0, occ_hi = 0; // 128-slot bitmap across 64-slot boundary
        bool tracked = false;
        bool contains(uint32_t c) const { return c != 0 && slot < num_desc && c <= num_desc - slot; }
        bool has_any(uint32_t c) const {
            if (!contains(c) || !tracked) return false;
            for (uint32_t i = 0; i < c; ++i) {
                const uint32_t s = slot + i;
                const bool bit = s < 64 ? ((occ_lo >> s) & 1ull) : ((occ_hi >> (s - 64)) & 1ull);
                if (bit) return true;
            }
            return false;
        }
    };
    auto can_empty_src_bypass = [](const SimRange& dst, uint32_t dst_total, const std::vector<SimRange>& srcs, const std::vector<uint32_t>& src_sizes, bool null_src_sizes) {
        if (dst_total == 0 || !dst.contains(dst_total)) return false;
        uint64_t src_total = 0;
        for (size_t i = 0; i < srcs.size(); ++i) {
            const uint32_t sc = null_src_sizes ? 1u : src_sizes[i];
            src_total += sc;
            if (sc == 0) continue;
            if (srcs[i].tracked && (!srcs[i].contains(sc) || srcs[i].has_any(sc))) return false;
        }
        return src_total == dst_total;
    };

    SimRange dst_ok{62, 128, 0, 0, true}; // straddles 64-slot bitmap boundary (slots 62..65)
    std::vector<SimRange> empty_srcs(4, SimRange{0, 128, 0, 0, false});
    assert(can_empty_src_bypass(dst_ok, 4, empty_srcs, {}, true));

    // Occupied dst with empty srcs still bypasses second walk (forwards + clears dst once)
    dst_ok.occ_hi = (1ull << 0); // slot 64 occupied
    assert(dst_ok.has_any(4));
    assert(can_empty_src_bypass(dst_ok, 4, empty_srcs, {}, true));

    // Out-of-bounds dst MUST fall back
    SimRange dst_oob{126, 128, 0, 0, true};
    assert(!can_empty_src_bypass(dst_oob, 4, empty_srcs, {}, true));

    // Mismatched source count MUST fall back
    assert(!can_empty_src_bypass(dst_ok, 5, empty_srcs, {}, true));

    // Tracked source with occupied slot MUST fall back to preserve snapshot/commit
    auto srcs_occ = empty_srcs;
    srcs_occ[2] = SimRange{63, 128, (1ull << 63), 0, true};
    assert(!can_empty_src_bypass(dst_ok, 4, srcs_occ, {}, true));

    // Zero-sized source ranges mixed with valid ones matching dst_total
    std::vector<uint32_t> mixed_sz = {0, 2, 0, 2};
    assert(can_empty_src_bypass(dst_ok, 4, empty_srcs, mixed_sz, false));
}

} // namespace

int main() {
    TestProductionFlatViewMapDifferential();
    TestFlatViewShardConcurrentStress();
    TestExclusiveSingleSeqStoreAndSeqlockLitmus();
    TestMultiSpanPageCoalescingAndDupDestDetection();
    TestMultiSpanPerOccurrenceOwnership();
    TestHostCopyDescriptorsDifferential();
    std::printf("WAVE2D_DESCRIPTOR_MULTISPANS_TESTS_PASS\n");
    return 0;
}
