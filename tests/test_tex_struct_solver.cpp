#include "tex_struct_solver.hpp"

#include <cassert>
#include <string_view>

using namespace wwmi_struct_tex;
using Rgb = std::array<int, 3>;
using Rows = std::vector<std::vector<CandEval>>;

static PatchSig Signature(Rgb first, Rgb common, bool shifted = false)
{
    PatchSig sig;
    sig.valid = true;
    sig.nch = 4;
    sig.pw = sig.ph = 4;
    for (int k = 0; k < kPatches; ++k) {
        for (int i = 0; i < 16; ++i) {
            const int pattern = (i + (shifted ? 1 : 0)) % 2 ? 1 : -1;
            for (int c : k == 0 ? first : common)
                sig.px.push_back(static_cast<uint8_t>(c == 0 ? 0 : c + pattern * (k == 0 ? 4 : 55)));
            sig.px.push_back(255);
        }
    }
    for (double& sd : sig.rgb_std) sd = 20.0;
    return sig;
}

static TwinGroups Twins(const PatchSig& a, const PatchSig& b)
{
    return FindTwinGroups({ &a, &b }, [](size_t, size_t) { return true; });
}

static TwinGroups IdenticalTwins()
{
    const auto sig = Signature({ 40, 40, 40 }, { 155, 155, 155 });
    return Twins(sig, sig);
}

static CandEval Edge(int ri, double combined, double ssim = 0.04, double hist = 0.0, bool exact = true)
{
    return { ri, ssim, hist, combined, exact, true, false };
}

static VisualAssignment Solve(const Rows& rows, const std::vector<uint32_t>& dims,
    const TwinGroups& groups, std::initializer_list<std::pair<size_t, size_t>> simultaneous,
    const std::vector<PatchSig>& refs = {})
{
    return SolveVisualAssignment(rows, refs.empty() ? std::vector<PatchSig>(rows.size()) : refs,
        dims, groups, [&](size_t i, size_t j) {
            const auto pair = std::make_pair((std::min)(i, j), (std::max)(i, j));
            return std::find(simultaneous.begin(), simultaneous.end(), pair) != simultaneous.end();
        });
}

static void ReclaimTwin()
{
    // Exact initial scores: the original global match chose Form 1 and the auxiliary.
    const Rows rows{ { Edge(0, .297), Edge(1, .273) }, { Edge(1, .267), Edge(0, .257) },
        { Edge(0, .234), Edge(1, .220) } };
    assert((SolveMaxWeightAssignment(3, 2, { { { 0, .297 }, { 1, .273 } },
        { { 1, .267 }, { 0, .257 } }, { { 0, .234 }, { 1, .220 } } }) == std::vector{ 0, 1, -1 }));
    const auto out = Solve(rows, { 2048, 512, 2048 }, IdenticalTwins(), { { 0, 1 }, { 1, 2 } });
    assert((out.assigned == std::vector{ 0, -1, 1 }));
    assert(out.blocked[1][0] && out.blocked[1][1]);

    // Partial co-occurrence (e.g. after F10 reload in Form 2 while Form 1 only appeared on an
    // outline pass without the 512px auxiliary texture): unassigned 2048px row 2 must still
    // evict co-occurring 512px row 1 and reclaim twin 1.
    const auto out_partial = Solve(rows, { 2048, 512, 2048 }, IdenticalTwins(), { { 1, 2 } });
    assert((out_partial.assigned == std::vector{ 0, -1, 1 }));
    assert(out_partial.blocked[1][0] && out_partial.blocked[1][1]);

    // Challenger that cannot beat every co-occurring holder must NOT evict a weaker holder:
    // 4096px row 0 and 512px row 1 hold distinct twins (never co-occurring), while unassigned
    // 2048px row 2 co-occurs with both in separate draws. Row 2 loses to 4096px row 0, so it
    // cannot hold the twin group and must leave row 1's valid assignment intact.
    const auto out_no_wreck = Solve(rows, { 4096, 512, 2048 }, IdenticalTwins(), { { 0, 2 }, { 1, 2 } });
    assert((out_no_wreck.assigned == std::vector{ 0, 1, -1 }));
    assert(!out_no_wreck.blocked[1][0] && !out_no_wreck.blocked[1][1]);
}

static void LightmapResolution()
{
    const Rows rows{ { Edge(0, .151) }, { Edge(1, .281) } };
    const auto twins = IdenticalTwins();
    assert((Solve(rows, { 2048, 1024 }, twins, { { 0, 1 } }).assigned == std::vector{ 0, -1 }));
    assert((Solve(rows, { 2047, 1024 }, twins, { { 0, 1 } }).assigned == std::vector{ -1, 1 }));
    assert((Solve(rows, { 2048, 0 }, twins, { { 0, 1 } }).assigned == std::vector{ -1, 1 }));
    assert((Solve(rows, { UINT32_MAX, 1024 }, twins, { { 0, 1 } }).assigned == std::vector{ 0, -1 }));
    assert(PreferTwinOwner(Edge(0, .2, .12), 0, Edge(1, .2, .05), 0));
}

static void DirectSsim()
{
    const Rows rows{ { Edge(0, .580, .74) }, { Edge(1, .530, .75) } };
    const auto out = Solve(rows, { 2048, 512 }, IdenticalTwins(), { { 0, 1 } });
    assert((out.assigned == std::vector{ -1, 1 }));
    assert(out.blocked[0][0] && out.blocked[0][1]);
}

static void DifferingPatch()
{
    const Rgb dark{ 28, 25, 23 }, bright{ 49, 54, 63 }, common{ 155, 145, 135 };
    const auto a = Signature(dark, common), b = Signature(bright, common);
    const auto twins = Twins(a, b);
    assert(twins.cluster[0] == twins.cluster[1]);
    assert(twins.active_patches[twins.cluster[0]] == 1);

    // Shift the pixels and change the four uninformative patches. Only patch 0
    // supplies affinity; unadjusted scores favor the wrong assignment.
    const std::vector refs{ Signature(dark, { 100, 130, 185 }, true),
        Signature(bright, { 130, 185, 100 }, true) };
    const Rows rows{ { Edge(1, .273), Edge(0, .250) }, { Edge(0, .234), Edge(1, .220) } };
    const auto out = Solve(rows, { 2048, 2048 }, twins, {}, refs);
    assert((out.assigned == std::vector{ 0, 1 }));
    const auto& chosen = rows[1][1];
    assert(std::string_view(VisualTier(chosen, 2, chosen.combined - rows[1][0].combined,
        true, chosen.combined, true)) == "medium_twin");

    const auto same = DescribePatches(refs[0]);
    assert(std::abs(DifferingPatchBonus(same, *twins.colors[0], 1) - .08) < 1e-9);
    const auto blank = Signature({ 0, 0, 0 }, common);
    assert(DifferingPatchBonus(DescribePatches(blank), *twins.colors[0], 1) == 0.0);
    const auto blanks = Twins(blank, a);
    assert(blanks.active_patches[blanks.cluster[0]] == 0);

    const Rows direct{ { Edge(1, .273, .75), Edge(0, .250) } };
    assert(Solve(direct, { 2048 }, twins, {}, { refs[0] }).assigned[0] == 1);
    const auto unrelated = Signature({ 160, 40, 230 }, { 160, 0, 65 }, true);
    const auto groups = FindTwinGroups({ &a, &b, &unrelated }, [](size_t, size_t) { return true; });
    const Rows unrelated_first{ { Edge(2, .258), Edge(0, .250), Edge(1, .242) } };
    assert(Solve(unrelated_first, { 2048 }, groups, {}, { refs[0] }).assigned[0] == 2);
}

static void NonTwinAndReuse()
{
    const auto a = Signature({ 28, 25, 23 }, { 155, 145, 135 });
    const auto b = Signature({ 160, 40, 230 }, { 160, 0, 65 }, true);
    auto flat = a;
    for (double& sd : flat.rgb_std) sd = 0.0;
    const auto groups = FindTwinGroups({ &a, &b, &a, &a, &flat }, [](size_t i, size_t j) {
        // Candidate 2 is non-exclusive; candidate 3 belongs to another format.
        return i != 2 && j != 2 && (i == 3) == (j == 3);
    });
    assert((groups.cluster == std::vector<size_t>{ 0, 1, 2, 3, 4 }));
    const Rows mips{ { Edge(0, .59) }, { Edge(0, .26) }, { Edge(1, .34), Edge(0, .31) } };
    assert((Solve(mips, { 2048, 256, 1024 }, groups, { { 0, 2 }, { 1, 2 } }).assigned ==
        std::vector{ 0, 0, 1 }));

    // This row had no Phase 1 assignment (and no pruning); Phase 2 must check
    // the holder of the sibling even though its preferred candidate is reusable.
    const Rows reuse{ { Edge(0, .51) }, { Edge(1, .50) }, { Edge(1, .20), Edge(0, .19) },
        { Edge(1, .18) } };
    const auto out = Solve(reuse, { 2048, 2048, 512, 256 }, IdenticalTwins(), { { 0, 2 } });
    assert((out.assigned == std::vector{ 0, 1, -1, 1 }));
    assert(!out.blocked[2][0] && !out.blocked[2][1]);

    assert(std::string_view(VisualTier(Edge(0, .50), 2, .11, false, .11, true)) == "high_visual");
    assert(std::string_view(VisualTier(Edge(0, .151), 1, .01, false, .01, true)) == "high_unique");
    assert(std::string_view(VisualTier(Edge(0, .29, .1), 2, .04, false, .04, true)) == "medium_visual");
    const auto cross = Edge(0, .23, .0, .36, false);
    assert(std::string_view(VisualTier(cross, 0, .07, false, .07, true)) == "medium_cross_bc");
    assert(std::string_view(VisualTier(cross, 0, .23, false, .23, false)) == "medium_cross_bc_unique");
}

static void TransitiveCluster()
{
    auto a = Signature({ 40, 40, 40 }, { 155, 155, 155 }), b = a, c = a;
    for (int k = 0; k < kPatches; ++k) {
        for (int i = 0; i < 16; ++i) {
            for (int ch = 0; ch < 3; ++ch) {
                const size_t p = (k * 16 + i) * 4 + ch;
                a.px[p] = i % 2 ? 112 : 32;
                b.px[p] = i == 0 ? 80 : a.px[p];
                c.px[p] = i == 1 || i == 3 ? 80 : (i % 2 ? 32 : 112);
            }
        }
    }
    assert(IsTwinSimilarity(ComparePatchSigs(a, b)));
    assert(IsTwinSimilarity(ComparePatchSigs(b, c)));
    assert(!IsTwinSimilarity(ComparePatchSigs(a, c)));
    const auto groups = FindTwinGroups({ &a, &b, &c }, [](size_t, size_t) { return true; });
    assert(groups.cluster[0] == groups.cluster[1] && groups.cluster[0] == groups.cluster[2]);

    const Rows rows{ { Edge(0, .4) }, { Edge(2, .31), Edge(1, .30) } };
    const auto out = Solve(rows, { 2048, 512 }, groups, { { 0, 1 } }, { a, c });
    assert((out.assigned == std::vector{ 0, -1 }));
    assert(out.blocked[1][0] && out.blocked[1][1] && out.blocked[1][2]);
}

static void CutoutMaskClassifier()
{
    auto make_stats = [](uint64_t white, uint64_t black, uint64_t mid, uint64_t colored, uint64_t trans) {
        CutoutMaskStats st{};
        for (uint64_t i = 0; i < white; ++i) AccumulateCutoutTexel(st, 255, 255, 255, 255);
        for (uint64_t i = 0; i < black; ++i) AccumulateCutoutTexel(st, 0, 0, 0, 255);
        for (uint64_t i = 0; i < mid; ++i) AccumulateCutoutTexel(st, 128, 128, 128, 255);
        for (uint64_t i = 0; i < colored; ++i) AccumulateCutoutTexel(st, 240, 100, 60, 255);
        for (uint64_t i = 0; i < trans; ++i) AccumulateCutoutTexel(st, 255, 255, 255, 128);
        return st;
    };

    // Binary white-with-black-cutout masks (e.g. 0.5% and 2.2% black cutouts, sharp edges)
    const auto mask_small = make_stats(9930, 50, 20, 0, 0);
    assert(IsBinaryCutoutMask(mask_small));
    assert(!IsPureWhiteNeutralizer(mask_small));

    const auto mask_med = make_stats(9740, 220, 40, 0, 0);
    assert(IsBinaryCutoutMask(mask_med));
    assert(!IsPureWhiteNeutralizer(mask_med));

    // Pure white neutralizer (100% opaque white)
    const auto pure_white = make_stats(10000, 0, 0, 0, 0);
    assert(!IsBinaryCutoutMask(pure_white));
    assert(IsPureWhiteNeutralizer(pure_white));

    // Near-white FX texture with 0.8% mid-gray pixels -> must reject both
    const auto fx_near_white = make_stats(9920, 0, 80, 0, 0);
    assert(!IsBinaryCutoutMask(fx_near_white));
    assert(!IsPureWhiteNeutralizer(fx_near_white));

    // Smooth grayscale gradient / diffuse / translucent UI / all-black -> reject both
    assert(!IsBinaryCutoutMask(make_stats(6000, 2000, 2000, 0, 0)));
    assert(!IsBinaryCutoutMask(make_stats(9000, 500, 0, 500, 0)));
    assert(!IsBinaryCutoutMask(make_stats(9000, 500, 0, 0, 500)));
    assert(!IsBinaryCutoutMask(make_stats(0, 10000, 0, 0, 0)));
    assert(!IsBinaryCutoutMask(CutoutMaskStats{}));
    assert(!IsPureWhiteNeutralizer(CutoutMaskStats{}));

    // Low-mip (64x64 / 128x128) resident streaming-tail cutout mask cases
    assert(IsBinaryCutoutMaskLowMip(make_stats(4046, 20, 30, 0, 0)));
    CutoutMaskStats srgb_downsampled{};
    for (int i = 0; i < 4046; ++i) AccumulateCutoutTexel(srgb_downsampled, 255, 255, 255, 255);
    for (int i = 0; i < 20; ++i) AccumulateCutoutTexel(srgb_downsampled, 32, 32, 32, 255); // sRGB linear-mip bleed
    for (int i = 0; i < 30; ++i) AccumulateCutoutTexel(srgb_downsampled, 140, 140, 140, 255);
    assert(!IsBinaryCutoutMask(srgb_downsampled));
    assert(IsBinaryCutoutMaskLowMip(srgb_downsampled));
    // Pure white / mid-gray FX / smooth gradient / colorful textures must still reject low-mip classifier
    assert(!IsBinaryCutoutMaskLowMip(pure_white));
    assert(!IsBinaryCutoutMaskLowMip(fx_near_white));
    assert(!IsBinaryCutoutMaskLowMip(make_stats(6000, 2000, 2000, 0, 0)));
    assert(!IsBinaryCutoutMaskLowMip(make_stats(9000, 500, 0, 500, 0)));
    assert(!IsBinaryCutoutMaskLowMip(CutoutMaskStats{}));
}

static void VisualMipParentEquivalence()
{
    VisualMipParentTable t;
    assert(t.Resolve(0xacba6531u) == 0);
    // Unique sub-mip -> parent (1024px sub1 -> 2048px mip0)
    assert(t.Note(0xacba6531u, 0x32be8842u));
    assert(t.Resolve(0xacba6531u) == 0x32be8842u);
    // Multi-level consistent chain (512px -> 1024px -> 2048px, then direct 512px -> 2048px)
    assert(t.Note(0xe24c1905u, 0xacba6531u));
    assert(t.Resolve(0xe24c1905u) == 0x32be8842u);
    assert(t.Note(0xe24c1905u, 0x32be8842u));
    assert(t.Resolve(0xe24c1905u) == 0x32be8842u);
    // Cycle guard: reversing an existing chain must be ignored
    assert(!t.Note(0x32be8842u, 0xacba6531u));
    assert(t.Resolve(0x32be8842u) == 0);
    assert(t.Resolve(0xacba6531u) == 0x32be8842u);
    // Shared/placeholder sub-mip seen under two distinct parents -> permanently ambiguous
    assert(t.Note(0x86e6d9e1u, 0x9394fba6u));
    assert(t.Resolve(0x86e6d9e1u) == 0x9394fba6u);
    assert(t.Note(0x86e6d9e1u, 0x925af8a7u));
    assert(t.Resolve(0x86e6d9e1u) == 0);
    assert(!t.Note(0x86e6d9e1u, 0x9394fba6u));
    assert(t.Resolve(0x86e6d9e1u) == 0);
    // CollectForParents exports only unambiguous children of persisted parents
    const auto exported = t.CollectForParents([](uint32_t p) { return p == 0x32be8842u; });
    assert(exported.size() == 2);
}

static void SlotShaderKeySeparation()
{
    constexpr uint64_t kSharedVs = 0xe996af1f8b5c2488ull;
    const uint64_t k_decl1 = ComputeSlotShaderKey(kSharedVs, 0x1ull, true);
    const uint64_t k_decl3 = ComputeSlotShaderKey(kSharedVs, 0x3ull, true);
    const uint64_t k_unknown = ComputeSlotShaderKey(kSharedVs, 0x3ull, false);
    assert(k_decl1 != 0 && k_decl3 != 0);
    assert(k_decl1 != k_decl3);
    assert(k_decl1 != kSharedVs && k_decl3 != kSharedVs);
    assert(k_unknown == kSharedVs);
    assert(ComputeSlotShaderKey(0, 0x3ull, true) == 0);
}

int main(int argc, char* argv[])
{
    assert(argc == 2);
    const std::string_view scenario = argv[1];
    if (scenario == "reclaim") ReclaimTwin();
    else if (scenario == "resolution") LightmapResolution();
    else if (scenario == "direct") DirectSsim();
    else if (scenario == "patch") DifferingPatch();
    else if (scenario == "reuse") NonTwinAndReuse();
    else if (scenario == "cluster") TransitiveCluster();
    else if (scenario == "cutout") CutoutMaskClassifier();
    else if (scenario == "slotkey") SlotShaderKeySeparation();
    else if (scenario == "mipparent") VisualMipParentEquivalence();
    else return 1;
}
