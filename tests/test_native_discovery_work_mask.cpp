#include "../source/client/native_discovery.hpp"

#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

// Minimal deterministic CRC32C implementation for host-side unit tests.
extern "C" std::uint32_t crc32c_append(std::uint32_t crc, const unsigned char* data, std::size_t len)
{
    std::uint32_t c = ~crc;
    for (std::size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (int b = 0; b < 8; ++b)
            c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
    }
    return ~c;
}

using namespace hsin_rt;

static std::vector<NativeMeshIntent> filter_enabled_intents(const std::vector<NativeMeshIntent>& all)
{
    std::vector<NativeMeshIntent> out;
    for (const auto& i : all)
        if (i.enabled) out.push_back(i);
    return out;
}

static NativeProfile make_profile(unsigned guid, unsigned hash, unsigned verts)
{
    NativeProfile p{};
    p.object_guid = guid;
    p.content_hash = hash;
    p.vertices = verts;
    p.components.push_back(NativeComponent{ 0, 0, verts, 0, verts * 3u });
    return p;
}

static void test_settled_rest_proof_unpins_before_work_without_reprobe_loop()
{
    auto registry = std::make_shared<NativeProfileRegistry>();
    const std::vector<unsigned char> mod_bytes(120, 0x5Au);
    const std::uint32_t mod_hash = native_mesh_content_hash(mod_bytes);
    assert(registry->publish(make_profile(1001, mod_hash, 10)).inserted);

    NativeDiscovery disc(registry);
    NativeMeshIntent resolved_intent{};
    resolved_intent.object_guid = 1001;
    resolved_intent.content_hash = mod_hash;
    resolved_intent.enabled = true;
    resolved_intent.components.push_back(NativeIndexWindow{ 0, 0, 30 });
    disc.set_intents({ resolved_intent });
    assert(!disc.interested());
    assert(!disc.has_active_before_work());

    // Simulate a non-mod mesh whose (first, count) window accidentally matches the
    // known profile, triggering request_known_rest().
    NativeDiscovery::Writer fact{};
    fact.recording = Recording{ 0x100, 1, 1 };
    fact.rest = NativeRange{ ResourceKey{ 0x2000, 1 }, 0, 120, 0x80000 };
    fact.output = NativeRange{ ResourceKey{ 0x3000, 1 }, 0, 120, 0x90000 };
    fact.first = 0;
    fact.count = 10;

    const std::uint64_t cands[] = { 120 };
    // First call creates a new GPU rest_consumer request and activates BEFORE work.
    assert(disc.request_known_rest(fact, 0, cands));
    assert(disc.has_active_before_work());

    auto reqs = disc.requests(fact.recording, NativeDiscovery::ReadPoint::rest_consumer);
    assert(reqs.size() == 1);
    const std::uint64_t req_id = reqs[0]->id;

    // Duplicate call while in-flight deduplicates and returns false (must not re-arm).
    assert(!disc.request_known_rest(fact, 0, cands));

    // Forward, submit, signal, and complete with non-matching bytes (negative proof).
    disc.skin_forwarded(fact.recording, true);
    disc.submitted(fact.recording, 0xAAA, 1);
    disc.signalled(req_id, 0xAAA, 0xBBB, 1);
    assert(disc.has_active_before_work()); // still waiting GPU completion

    std::vector<unsigned char> non_mod_bytes(120, 0x33u);
    assert(disc.completed(req_id, 0xBBB, 1, non_mod_bytes));

    // publish() clears forwarded writers when !interested(), retains settled GPU rest proof.
    disc.publish();
    const auto bindings = disc.content_bindings();
    assert(bindings.empty()); // non-mod bytes didn't match any profile

    // Retained proof keeps pending_counts().bytes == 1 for dedup, yet BEFORE work is idle!
    assert(disc.pending_counts().bytes == 1);
    assert(disc.pending_counts().writers == 0);
    assert(!disc.has_active_before_work());

    // Next skin dispatch on the same non-mod rest buffer hits dedup -> returns false,
    // no duplicate GPU probe, and BEFORE work stays idle.
    NativeDiscovery::Writer next_fact = fact;
    next_fact.recording = Recording{ 0x100, 1, 2 };
    assert(!disc.request_known_rest(next_fact, 0, cands));
    assert(disc.requests(next_fact.recording, NativeDiscovery::ReadPoint::rest_consumer).empty());
    assert(!disc.has_active_before_work());

    // Overwriting the rest buffer invalidates the retained proof so a subsequent skin
    // dispatch CAN re-probe it and re-arm BEFORE work.
    assert(disc.written(fact.rest));
    assert(!disc.has_active_before_work());
    NativeDiscovery::Writer after_write = fact;
    after_write.recording = Recording{ 0x100, 1, 3 };
    assert(disc.request_known_rest(after_write, 0, cands));
    assert(disc.has_active_before_work());
}

static void test_disabled_intents_excluded_while_enabled_skip_only_kept()
{
    auto registry = std::make_shared<NativeProfileRegistry>();
    NativeDiscovery disc(registry);

    NativeMeshIntent disabled_regular{};
    disabled_regular.object_guid = 2001;
    disabled_regular.content_hash = 0x11111111u;
    disabled_regular.enabled = false;
    disabled_regular.skip_only = false;
    disabled_regular.components.push_back(NativeIndexWindow{ 0, 0, 30 });

    NativeMeshIntent disabled_skip{};
    disabled_skip.object_guid = 2002;
    disabled_skip.content_hash = 0x22222222u;
    disabled_skip.enabled = false;
    disabled_skip.skip_only = true;
    disabled_skip.components.push_back(NativeIndexWindow{ 0, 0, 30 });

    // Disabled intents alone must NOT keep discovery interested.
    const auto none_enabled = filter_enabled_intents({ disabled_regular, disabled_skip });
    assert(none_enabled.empty());
    disc.set_intents(none_enabled);
    assert(!disc.interested());
    assert(!disc.has_active_before_work());

    // Enabled skip-only intent MUST still be passed to discovery so partial profile
    // resolution continues working.
    NativeMeshIntent enabled_skip = disabled_skip;
    enabled_skip.enabled = true;
    const auto with_enabled_skip = filter_enabled_intents({ disabled_regular, enabled_skip });
    assert(with_enabled_skip.size() == 1);
    disc.set_intents(with_enabled_skip);
    assert(disc.interested());
}

int main()
{
    test_settled_rest_proof_unpins_before_work_without_reprobe_loop();
    test_disabled_intents_excluded_while_enabled_skip_only_kept();
    std::puts("OK: test_native_discovery_work_mask");
    return 0;
}
