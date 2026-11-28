#include "tex_live_sig_retention.hpp"

#include <cassert>
#include <cstdio>
#include <unordered_set>

using namespace wwmi_tex_live_sig;

static void TestRefCounts()
{
    ResidentHashRefCounts rc;
    assert(!rc.IsResident(0));
    assert(!rc.IsResident(0x66f878c4u));

    // Two resources carrying the same hash: destroying one keeps the hash resident.
    rc.OnSet(0, 0x66f878c4u);
    rc.OnSet(0, 0x66f878c4u);
    assert(rc.Count(0x66f878c4u) == 2u);
    assert(rc.IsResident(0x66f878c4u));

    rc.OnErase(0x66f878c4u);
    assert(rc.Count(0x66f878c4u) == 1u);
    assert(rc.IsResident(0x66f878c4u));

    // Overwrite and restore semantics (e.g. inherited belief -> verified hash -> restored belief).
    rc.OnSet(0x66f878c4u, 0xe898b084u);
    assert(!rc.IsResident(0x66f878c4u));
    assert(rc.IsResident(0xe898b084u));

    rc.OnSet(0xe898b084u, 0x66f878c4u);
    assert(rc.IsResident(0x66f878c4u));
    assert(!rc.IsResident(0xe898b084u));

    rc.OnErase(0x66f878c4u);
    assert(!rc.IsResident(0x66f878c4u));
    assert(rc.UniqueHashes() == 0u);
}

static void TestResidentSurvivesDeadChurn()
{
    ResidentHashRefCounts          rc;
    std::unordered_map<uint32_t, int> sigs;
    std::deque<uint32_t>           order;

    auto insert_sig = [&](uint32_t h, size_t soft = 32, size_t hard = 64, size_t batch = 8) {
        if (sigs.count(h)) return;
        const auto cands = PeekEvictCandidates(order, sigs.size(), soft, batch);
        std::vector<uint8_t> flags(cands.size(), 0u);
        for (size_t i = 0; i < cands.size(); ++i) flags[i] = rc.IsResident(cands[i]) ? 1u : 0u;
        if (sigs.size() >= soft) ApplyResidentFirstEviction(sigs, order, cands, flags, soft, hard);
        sigs.emplace(h, 1);
        order.push_back(h);
    };

    // Boot-resident texture (like 66f878c4) stays alive throughout 500 dead uploads.
    constexpr uint32_t kBootResident = 0x66f878c4u;
    rc.OnSet(0, kBootResident);
    insert_sig(kBootResident);

    for (uint32_t i = 1; i <= 500; ++i) {
        rc.OnSet(0, i);
        insert_sig(i);
        // Transient world texture destroyed shortly after upload.
        if (i > 4) rc.OnErase(i - 4);
    }

    assert(sigs.count(kBootResident) == 1u);
    assert(sigs.size() <= 32u);
    assert(order.size() == sigs.size());

    // Destroy boot-resident carrier: subsequent dead churn can now evict it cleanly.
    rc.OnErase(kBootResident);
    for (uint32_t i = 501; i <= 600; ++i) {
        rc.OnSet(0, i);
        insert_sig(i);
        rc.OnErase(i);
    }
    assert(sigs.count(kBootResident) == 0u);
    assert(order.size() == sigs.size());
}

static void TestHardCapEnforcedWithAllResident()
{
    ResidentHashRefCounts             rc;
    std::unordered_map<uint32_t, int> sigs;
    std::deque<uint32_t>              order;

    constexpr size_t kSoft = 16;
    constexpr size_t kHard = 32;
    constexpr size_t kBatch = 8;

    for (uint32_t i = 1; i <= 100; ++i) {
        rc.OnSet(0, i);
        const auto cands = PeekEvictCandidates(order, sigs.size(), kSoft, kBatch);
        std::vector<uint8_t> flags(cands.size(), 0u);
        for (size_t k = 0; k < cands.size(); ++k) flags[k] = rc.IsResident(cands[k]) ? 1u : 0u;
        if (sigs.size() >= kSoft) ApplyResidentFirstEviction(sigs, order, cands, flags, kSoft, kHard);
        sigs.emplace(i, 1);
        order.push_back(i);
        assert(sigs.size() <= kHard);
        assert(order.size() == sigs.size());
    }

    // Verify zero duplicates in order deque.
    std::unordered_set<uint32_t> uniq(order.begin(), order.end());
    assert(uniq.size() == order.size());
    assert(uniq.size() == sigs.size());
}

int main()
{
    TestRefCounts();
    TestResidentSurvivesDeadChurn();
    TestHardCapEnforcedWithAllResident();
    std::puts("test_tex_live_sig_retention: PASS");
    return 0;
}
