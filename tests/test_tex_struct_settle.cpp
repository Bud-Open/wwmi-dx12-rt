#include "tex_struct_recipe_cache.hpp"

#include <cassert>
#include <cstdio>

using namespace wwmi_struct_tex;

int main()
{
    std::vector<ObsLodRecord> live;

    // 1. Initial observation records new evidence.
    assert(UpsertObservedLod(live, 0x66f878c4u, 99u, 512u));
    assert(live.size() == 1u && !live[0].missing_sig);

    // Same key and size while not missing_sig does not requeue (stays settled for Wave 3C cache).
    assert(!UpsertObservedLod(live, 0x66f878c4u, 99u, 512u));

    // 2. Worker runs after eviction: TexLookCopyAnySig returned false -> marks missing_sig=true.
    const std::vector<ObsLodRecord> worker_snap = live;
    const std::vector<uint8_t>      missing_flags{ 1u };
    assert(ApplyWorkerSigAvailability(live, worker_snap, missing_flags, true));
    assert(live[0].missing_sig);

    // 3. Signature returns -> recipe misses -> Apply calls RecordObservedLod with same key & size:
    //    must become new evidence ONCE so the worker requeues!
    assert(UpsertObservedLod(live, 0x66f878c4u, 99u, 512u));
    assert(!live[0].missing_sig);
    // Subsequent draw without another missing-sig event stays settled.
    assert(!UpsertObservedLod(live, 0x66f878c4u, 99u, 512u));

    // 4. Worker copies signature (missing_sig=0) even if row turns out ambiguous or flat:
    //    missing_sig remains false -> row stays settled!
    const std::vector<uint8_t> present_flags{ 0u };
    assert(!ApplyWorkerSigAvailability(live, worker_snap, present_flags, true));
    assert(!live[0].missing_sig);
    assert(!UpsertObservedLod(live, 0x66f878c4u, 99u, 512u));

    // 5. If newer evidence arrived concurrently while worker ran (evidence_unchanged=false),
    //    stale missing_sig must NOT overwrite the fresh observation.
    assert(UpsertObservedLod(live, 0x66f878c4u, 99u, 1024u));
    assert(!ApplyWorkerSigAvailability(live, worker_snap, missing_flags, false));
    assert(!live[0].missing_sig);

    std::puts("test_tex_struct_settle: PASS");
    return 0;
}
