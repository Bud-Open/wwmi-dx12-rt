#include "../source/main/reshade_addon/textures/tex_struct_recipe_cache.hpp"
#include "../source/client/client_lock_stats.hpp"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
wwmi_struct_tex::PublishSeq g_test_meta_seq;
}
wwmi_struct_tex::PublishSeq& TexStructMetaSeq() noexcept
{
    return g_test_meta_seq;
}

using namespace wwmi_struct_tex;
using namespace hsin_rt;

static void test_publish_seq_in_flight_and_nested()
{
    PublishSeq seq;
    const uint64_t s0 = seq.snapshot();
    assert(PublishSeq::is_stable(s0));

    {
        PublishSeq::Guard g1(seq);
        const uint64_t s_mid1 = seq.snapshot();
        assert(!PublishSeq::is_stable(s_mid1));
        {
            PublishSeq::Guard g2(seq);
            const uint64_t s_mid2 = seq.snapshot();
            assert(!PublishSeq::is_stable(s_mid2));
            assert(s_mid2 != s_mid1);
        }
        assert(!PublishSeq::is_stable(seq.snapshot()));
    }
    const uint64_t s1 = seq.snapshot();
    assert(PublishSeq::is_stable(s1));
    assert(s1 == s0 + 2);

    // Concurrent stress: reader never sees stable s0 == s1 across a completed publication
    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> published_val{ 0 };
    std::thread writer([&] {
        for (uint64_t i = 1; i <= 2000; ++i) {
            PublishSeq::Guard g(seq);
            published_val.store(i, std::memory_order_release);
        }
        stop.store(true, std::memory_order_release);
    });
    while (!stop.load(std::memory_order_acquire)) {
        const uint64_t a = seq.snapshot();
        const uint64_t v1 = published_val.load(std::memory_order_acquire);
        const uint64_t v2 = published_val.load(std::memory_order_acquire);
        const uint64_t b = seq.snapshot();
        if (a == b && PublishSeq::is_stable(a)) {
            assert(v1 == v2);
        }
    }
    writer.join();
    assert(PublishSeq::is_stable(seq.snapshot()));
}

static void test_layout_ps_cache_epoch_and_rebuild()
{
    struct FakeRegBind {
        uint8_t  param = 0xFF;
        bool     is_table = false;
        uint32_t offset = 0;
    };
    ThreadLayoutPsCache<FakeRegBind> cache;
    FakeRegBind out[8]{};
    bool built = false;

    assert(!cache.find(0x1000, 1, built, out));

    // Cache negative (unbuilt) layout
    FakeRegBind empty[8]{};
    cache.put(0x1000, 1, false, empty);
    assert(cache.find(0x1000, 1, built, out));
    assert(!built);

    // Epoch bump invalidates negative entry
    assert(!cache.find(0x1000, 2, built, out));

    // Populate built layout
    FakeRegBind src[8]{};
    src[0] = { 3, true, 12 };
    src[4] = { 5, true, 99 };
    cache.put(0x1000, 2, true, src);
    assert(cache.find(0x1000, 2, built, out));
    assert(built);
    assert(out[0].param == 3 && out[0].offset == 12);
    assert(out[4].param == 5 && out[4].offset == 99);

    // Same-handle destroy + rebuild bumps epoch to 3
    assert(!cache.find(0x1000, 3, built, out));
    src[0] = { 7, true, 42 };
    cache.put(0x1000, 3, true, src);
    assert(cache.find(0x1000, 3, built, out));
    assert(built && out[0].param == 7 && out[0].offset == 42);
}

static RecipeKey make_sample_key()
{
    RecipeKey k{};
    k.load = 1;
    k.dm = 2;
    k.comp = 3;
    k.ps_hash = 0xabcdef1234567890ull;
    k.ps_regs_lo = 0x0full;
    k.layh = 0x5555ull;
    k.ps_decl_known = 1;
    k.lod_flags = 0x6; // resemblance=1, cache=1
    k.active_count = 2;
    k.slots[0] = RecipeSlotSnap{ 0, 2, 0, 0, 10, 99, 0x11223344u, 0x100000ull, 7 };
    k.slots[1] = RecipeSlotSnap{ 3, 2, 0, 0, 13, 77, 0x55667788u, 0x200000ull, 9 };
    return k;
}

static void test_recipe_cache_semantics_and_atomic_replay()
{
    PublishSeq state_seq;
    PublishSeq meta_seq;
    ThreadRecipeCache cache;

    const RecipeKey k = make_sample_key();
    const uint64_t s0 = state_seq.snapshot();

    // Non-register emission order (Pass 1 emits reg 3 first, Pass 2 emits reg 0 second)
    RecipeBind binds[2]{};
    binds[0] = RecipeBind{ 3, 2, 13, 77, L"mod\\tex_pass1.dds" };
    binds[1] = RecipeBind{ 0, 2, 10, 99, L"mod\\tex_pass2.dds" };

    // Storing while in-flight must be ignored
    {
        PublishSeq::Guard g(meta_seq);
        cache.store(k, s0, meta_seq.snapshot(), binds, 2);
        assert(cache.lookup(k, s0, meta_seq.snapshot()) == nullptr);
    }

    const uint64_t m1 = meta_seq.snapshot();
    cache.store(k, s0, m1, binds, 2);
    const RecipeEntry* e = cache.lookup(k, s0, m1);
    assert(e != nullptr);
    assert(e->bind_count == 2);
    assert(e->binds[0].reg == 3 && e->binds[0].fq == L"mod\\tex_pass1.dds");
    assert(e->binds[1].reg == 0 && e->binds[1].fq == L"mod\\tex_pass2.dds");

    // Invalidation checks: res_gen change, lod_flags change, pre_ovr change, seq bump
    RecipeKey k_diff_gen = k;
    k_diff_gen.slots[0].res_gen = 8;
    assert(cache.lookup(k_diff_gen, s0, m1) == nullptr);

    RecipeKey k_diff_lod = k;
    k_diff_lod.lod_flags ^= 1u;
    assert(cache.lookup(k_diff_lod, s0, m1) == nullptr);

    RecipeKey k_diff_ovr = k;
    k_diff_ovr.slots[1].pre_ovr = 1;
    assert(cache.lookup(k_diff_ovr, s0, m1) == nullptr);

    assert(cache.lookup(k, s0 + 1, m1) == nullptr);
    assert(cache.lookup(k, s0, m1 + 1) == nullptr);

    // Atomic replay helper: fail on 2nd FQ => zero partial appends + entry invalidated
    auto replay = [&](bool hybrid, bool fail_second, bool bump_mid_resolve,
                      std::vector<uint32_t>& out_offsets, std::vector<uint64_t>& out_owners) -> bool {
        const uint64_t ps = state_seq.snapshot();
        const uint64_t pm = meta_seq.snapshot();
        const RecipeEntry* hit = cache.lookup(k, ps, pm);
        if (!hit) return false;
        uint64_t resolved_res[8]{};
        bool ok = true;
        for (uint8_t i = 0; i < hit->bind_count; ++i) {
            if (fail_second && i == 1) { ok = false; break; }
            resolved_res[i] = 0x9000ull + i;
            if (bump_mid_resolve && i == 0) meta_seq.bump();
        }
        if (ok && state_seq.snapshot() == ps && meta_seq.snapshot() == pm) {
            for (uint8_t i = 0; i < hit->bind_count; ++i) {
                out_offsets.push_back(hit->binds[i].offset);
                if (hybrid && out_owners.size() + 1 == out_offsets.size())
                    out_owners.push_back(resolved_res[i]);
            }
            return true;
        }
        cache.invalidate(k);
        return false;
    };

    // Fail on 2nd FQ: no partial output, entry invalidated
    std::vector<uint32_t> offs;
    std::vector<uint64_t> owners;
    assert(!replay(true, true, false, offs, owners));
    assert(offs.empty() && owners.empty());
    assert(cache.lookup(k, s0, m1) == nullptr);

    // Re-store and test mid-resolve meta_seq bump: no partial output, entry invalidated
    cache.store(k, s0, m1, binds, 2);
    assert(!replay(true, false, true, offs, owners));
    assert(offs.empty() && owners.empty());

    // Re-store at current seq and verify successful hybrid & non-hybrid replay in emission order
    const uint64_t m2 = meta_seq.snapshot();
    cache.store(k, s0, m2, binds, 2);
    assert(replay(true, false, false, offs, owners));
    assert(offs.size() == 2 && offs[0] == 13 && offs[1] == 10);
    assert(owners.size() == 2 && owners[0] == 0x9000ull && owners[1] == 0x9001ull);

    // Non-hybrid replay does not touch owners
    std::vector<uint32_t> offs2;
    std::vector<uint64_t> owners2;
    assert(replay(false, false, false, offs2, owners2));
    assert(offs2.size() == 2 && owners2.empty());

    // Negative (0-bind) recipe caching
    RecipeKey k_empty = k;
    k_empty.comp = 99;
    cache.store(k_empty, s0, m2, nullptr, 0);
    const RecipeEntry* e0 = cache.lookup(k_empty, s0, m2);
    assert(e0 != nullptr && e0->bind_count == 0);
}

static void test_observed_owner_and_client_lock_tracker()
{
    // Pack/unpack edge cases
    assert(ObservedOwnerCodec::pack(0, ClientLockOp::present, 1234) == 0);
    assert(ObservedOwnerCodec::pack(1, ClientLockOp::none, 1234) == 0);

    const uint64_t big_rel = (1ull << 49) + 0x1234567ull;
    const uint64_t packed = ObservedOwnerCodec::pack(255, ClientLockOp::native_buffer_copy_before, big_rel);
    assert(packed != 0);
    assert(ObservedOwnerCodec::unpack_window(packed) == 255);
    assert(ObservedOwnerCodec::unpack_op(packed) == ClientLockOp::native_buffer_copy_before);
    assert(ObservedOwnerCodec::unpack_rel(packed) == big_rel);

    // Name classification
    assert(client_lock_op_from_name("native_buffer_copy_before") == ClientLockOp::native_buffer_copy_before);
    assert(client_lock_op_from_name("native_buffer_copy_rec") == ClientLockOp::native_buffer_copy_rec);
    assert(client_lock_op_from_name("native_buffer_write") == ClientLockOp::native_buffer_write);
    assert(client_lock_op_from_name("native_buffer_unknown") == ClientLockOp::native_buffer_unknown);
    assert(client_lock_op_from_name("WWMI_RtPackAcquireHybridRaster_v1") == ClientLockOp::api_hybrid_acquire);
    assert(client_lock_op_from_name("forward_blas") == ClientLockOp::forward_blas);
    assert(client_lock_op_from_name("present") == ClientLockOp::present);
    assert(client_lock_op_from_name("unknown_xyz") == ClientLockOp::other);

    // Fake-clock tracker test (freq = 10 MHz => 1 ms = 10,000 ticks, 10 us = 100 ticks, 250 us = 2,500 ticks)
    ClientLockTracker tracker;
    assert(tracker.active_window(500) == 0);
    assert(!tracker.claim_emit_once());

    constexpr uint64_t kFreq = 10'000'000ull;
    tracker.reset_for_test(1, 1000, 1000 + 120'000'000ull, kFreq);
    assert(tracker.active_window(2000) == 1);
    assert(tracker.active_window(1000 + 120'000'000ull) == 0);

    // Record slow wait blamed on `present`
    const uint64_t owner_present = ObservedOwnerCodec::pack(1, ClientLockOp::present, 5000);
    tracker.record_lock_sample(ClientLockOp::api_hybrid_acquire, 1, 85'000, 1'200, owner_present, 90'000);
    // Record wait with stale window_id=2 => owner attribution ignored, wait still counted
    const uint64_t stale_owner = ObservedOwnerCodec::pack(2, ClientLockOp::forward_queue, 1000);
    tracker.record_lock_sample(ClientLockOp::execute, 1, 30'000, 500, stale_owner, 40'000);
    // Fill >8 slow waits to verify top-8 retention
    for (uint64_t i = 1; i <= 10; ++i) {
        tracker.record_lock_sample(ClientLockOp::native_skin_receipt, 1, 3'000 + i * 1'000, 200, owner_present, 100'000 + i * 1'000);
    }

    tracker.record_devices_lookup(42);
    tracker.record_devices_lookup(105);
    tracker.note_copy_bypass(false);
    tracker.note_copy_bypass(true);
    tracker.note_copy_bypass(true);
    tracker.note_empty_tables_bypass();
    tracker.note_sdk_dispatch_bypass();

    assert(!tracker.is_emitted());
    assert(tracker.claim_emit_once());
    assert(tracker.is_emitted());
    assert(!tracker.claim_emit_once());

    const auto snap = tracker.snapshot();
    assert(snap.devs_calls == 2 && snap.devs_ticks == 147 && snap.devs_max_ticks == 105);
    assert(snap.copy_bypass_before == 1 && snap.copy_bypass_rec == 2);
    assert(snap.empty_tables_bypass == 1 && snap.sdk_dispatch_bypass == 1);
    assert(snap.slow_waits.size() == ClientLockTracker::kMaxSlowWaits);
    assert(snap.slow_waits[0].waiter == ClientLockOp::api_hybrid_acquire);
    assert(snap.slow_waits[0].obs_owner == ClientLockOp::present);
    assert(snap.slow_waits[0].wait_ticks == 85'000);
    assert(snap.slow_waits[0].owner_held_ticks == 85'000);
    assert(snap.slow_waits[1].waiter == ClientLockOp::execute);
    assert(snap.slow_waits[1].obs_owner == ClientLockOp::none); // stale window ignored!

    bool saw_present_blame = false;
    for (const auto& op : snap.ops) {
        if (op.op == ClientLockOp::present) {
            saw_present_blame = true;
            assert(op.blamed_count == 11);
            assert(op.blamed_wait_ticks > 85'000);
        }
    }
    assert(saw_present_blame);
}

static void test_shared_mutex_cv_any_lifecycle()
{
    std::shared_mutex mtx;
    std::condition_variable_any cv;
    std::unordered_map<uint64_t, int> devices;
    int initializing = 1;
    std::atomic<bool> stop_readers{ false };
    std::atomic<uint64_t> reads{ 0 };

    devices[1] = 42;
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop_readers.load(std::memory_order_acquire)) {
                std::shared_lock lk(mtx);
                auto it = devices.find(1);
                if (it != devices.end()) assert(it->second == 42);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread finisher([&] {
        std::this_thread::yield();
        {
            std::unique_lock lk(mtx);
            devices[2] = 99;
            initializing = 0;
        }
        cv.notify_all();
    });

    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return initializing == 0; });
        assert(devices.size() == 2);
        devices.clear();
    }

    stop_readers.store(true, std::memory_order_release);
    finisher.join();
    for (auto& th : readers) th.join();
    assert(reads.load() > 0);
}

static void test_concurrent_atomic_lod_toggle_invalidates_old_recipe()
{
    std::atomic<bool> alias_off{ false };
    std::atomic<bool> resemblance{ true };
    std::atomic<bool> cache_on{ true };
    PublishSeq state_seq;
    PublishSeq meta_seq;

    auto read_flags = [&]() noexcept -> uint8_t {
        return (alias_off.load(std::memory_order_acquire) ? 1u : 0u) |
               (resemblance.load(std::memory_order_acquire) ? 2u : 0u) |
               (cache_on.load(std::memory_order_acquire) ? 4u : 0u);
    };

    ThreadRecipeCache cache;
    RecipeKey base = make_sample_key();
    base.lod_flags = read_flags();
    RecipeBind bind{ 0, 2, 10, 99, L"ResourceTex0" };
    cache.store(base, state_seq.snapshot(), meta_seq.snapshot(), &bind, 1);
    assert(cache.lookup(base, state_seq.snapshot(), meta_seq.snapshot()) != nullptr);

    // Concurrent UI thread toggles atomic LOD switches under PublishSeq::Guard
    // while recording threads continuously read flags & query/store recipes.
    std::atomic<bool> stop{ false };
    std::thread ui_thread([&] {
        for (int i = 0; i < 400; ++i) {
            PublishSeq::Guard g(meta_seq);
            alias_off.store((i & 1) != 0, std::memory_order_release);
            resemblance.store((i & 2) == 0, std::memory_order_release);
            cache_on.store((i & 4) == 0, std::memory_order_release);
        }
        {
            PublishSeq::Guard g(meta_seq);
            alias_off.store(true, std::memory_order_release);
            resemblance.store(false, std::memory_order_release);
            cache_on.store(true, std::memory_order_release);
        }
        stop.store(true, std::memory_order_release);
    });

    while (!stop.load(std::memory_order_acquire)) {
        const uint64_t pre_s = state_seq.snapshot();
        const uint64_t pre_m = meta_seq.snapshot();
        RecipeKey k = base;
        k.lod_flags = read_flags();
        if (const auto* hit = cache.lookup(k, pre_s, pre_m)) {
            assert(hit->key.lod_flags == k.lod_flags);
            continue;
        }
        const uint8_t post_flags = read_flags();
        if (post_flags == k.lod_flags && PublishSeq::is_stable(pre_s) && PublishSeq::is_stable(pre_m) &&
            state_seq.snapshot() == pre_s && meta_seq.snapshot() == pre_m) {
            cache.store(k, pre_s, pre_m, &bind, 1);
        }
    }
    ui_thread.join();

    // After final toggle (alias_off=1, resemblance=0, cache_on=1 -> flags=5),
    // old recipe (flags=6) MUST NOT hit.
    RecipeKey old_k = base;
    old_k.lod_flags = 6;
    assert(cache.lookup(old_k, state_seq.snapshot(), meta_seq.snapshot()) == nullptr);
}

int main()
{
    test_publish_seq_in_flight_and_nested();
    test_layout_ps_cache_epoch_and_rebuild();
    test_recipe_cache_semantics_and_atomic_replay();
    test_concurrent_atomic_lod_toggle_invalidates_old_recipe();
    test_observed_owner_and_client_lock_tracker();
    test_shared_mutex_cv_any_lifecycle();
    std::puts("OK: test_wave3a_recipe_and_client_lock");
    return 0;
}
