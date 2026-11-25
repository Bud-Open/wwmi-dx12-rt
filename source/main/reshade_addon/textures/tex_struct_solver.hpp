#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wwmi_struct_tex {

constexpr int kPatches = 5;

struct PatchSig {
    uint8_t              codec = 0;
    uint8_t              nch = 0;
    uint32_t             pw = 0;
    uint32_t             ph = 0;
    std::vector<uint8_t> px;
    double               rgb_mean[3]{};
    double               rgb_std[3]{};
    bool                 valid = false;

    bool is_flat() const
    {
        return (std::max)({ rgb_std[0], rgb_std[1], rgb_std[2] }) < 3.0;
    }
};

// Compare two 5-patch signatures -> (rgb_ssim, rgb_hist_sim), matching tools/migrate_hash_free.py.
inline std::pair<double, double> ComparePatchSigs(const PatchSig& a, const PatchSig& b)
{
    if (!a.valid || !b.valid || !a.pw || !a.ph || !b.pw || !b.ph) return { -1.0, 0.0 };
    const uint32_t pw = (std::min)(a.pw, b.pw);
    const uint32_t ph = (std::min)(a.ph, b.ph);
    const uint32_t nch = (std::min)(a.nch, b.nch);
    if (!pw || !ph || !nch || (a.pw % pw) || (a.ph % ph) || (b.pw % pw) || (b.ph % ph)) return { -1.0, 0.0 };

    auto downsample = [&](const PatchSig& s) -> std::vector<float> {
        const uint32_t fx = s.pw / pw;
        const uint32_t fy = s.ph / ph;
        std::vector<float> out(static_cast<size_t>(kPatches) * pw * ph * nch, 0.f);
        for (int k = 0; k < kPatches; ++k) {
            for (uint32_t y = 0; y < s.ph; ++y) {
                const uint32_t dy = y / fy;
                for (uint32_t x = 0; x < s.pw; ++x) {
                    const uint32_t dx = x / fx;
                    const size_t   si = ((static_cast<size_t>(k) * s.ph + y) * s.pw + x) * s.nch;
                    const size_t   di = ((static_cast<size_t>(k) * ph + dy) * pw + dx) * nch;
                    for (uint32_t c = 0; c < nch; ++c) out[di + c] += static_cast<float>(s.px[si + c]);
                }
            }
        }
        const float inv = 1.f / static_cast<float>(fx * fy);
        if (fx != 1 || fy != 1)
            for (float& v : out) v *= inv;
        return out;
    };

    const std::vector<float> va = downsample(a);
    const std::vector<float> vb = downsample(b);
    const size_t             n = static_cast<size_t>(kPatches) * pw * ph;
    const double             c1 = (0.01 * 255.0) * (0.01 * 255.0);
    const double             c2 = (0.03 * 255.0) * (0.03 * 255.0);

    double ssim_sum = 0.0;
    double hist_sum = 0.0;
    int    active_ch = 0;
    const uint32_t ch_limit = (std::min)(nch, 3u);
    for (uint32_t c = 0; c < ch_limit; ++c) {
        uint32_t ha[32]{};
        uint32_t hb[32]{};
        double   sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double x = va[i * nch + c];
            const double y = vb[i * nch + c];
            ++ha[(std::min)(31, static_cast<int>(x) >> 3)];
            ++hb[(std::min)(31, static_cast<int>(y) >> 3)];
            sa += x;
            sb += y;
            saa += x * x;
            sbb += y * y;
            sab += x * y;
        }
        const double ma = sa / static_cast<double>(n);
        const double mb = sb / static_cast<double>(n);
        const double vaa = (std::max)(0.0, saa / static_cast<double>(n) - ma * ma);
        const double vbb = (std::max)(0.0, sbb / static_cast<double>(n) - mb * mb);
        if (vaa < 1.0 && vbb < 1.0) continue;
        const double cov = sab / static_cast<double>(n) - ma * mb;
        const double ssim = ((2.0 * ma * mb + c1) * (2.0 * cov + c2)) /
            ((ma * ma + mb * mb + c1) * (vaa + vbb + c2));
        uint32_t inter = 0;
        for (int b_i = 0; b_i < 32; ++b_i) inter += (std::min)(ha[b_i], hb[b_i]);
        ssim_sum += ssim;
        hist_sum += static_cast<double>(inter) / static_cast<double>(n);
        ++active_ch;
    }
    if (!active_ch) return { -1.0, 0.0 };
    return { ssim_sum / active_ch, hist_sum / active_ch };
}

// Exact maximum-weight 1-to-1 bipartite matching (branch-and-bound over small component graphs).
inline std::vector<int> SolveMaxWeightAssignment(
    size_t n_left, size_t n_right, const std::vector<std::vector<std::pair<int, double>>>& adj_sorted)
{
    std::vector<int> best_assign(n_left, -1);
    if (!n_left || !n_right) return best_assign;

    std::vector<double> suffix_ub(n_left + 1, 0.0);
    for (size_t i = n_left; i-- > 0;) {
        const double top = adj_sorted[i].empty() ? 0.0 : adj_sorted[i][0].second;
        suffix_ub[i] = suffix_ub[i + 1] + (top > 0.0 ? top : 0.0);
    }

    double           best_score = -1.0;
    std::vector<int> cur(n_left, -1);
    std::vector<uint8_t> used(n_right, 0);

    auto dfs = [&](auto&& self, size_t i, double score) -> void {
        if (score + suffix_ub[i] <= best_score + 1e-12) return;
        if (i == n_left) {
            if (score > best_score) {
                best_score = score;
                best_assign = cur;
            }
            return;
        }
        const size_t lim = (std::min)(adj_sorted[i].size(), size_t(10));
        for (size_t k = 0; k < lim; ++k) {
            const int    j = adj_sorted[i][k].first;
            const double w = adj_sorted[i][k].second;
            if (j < 0 || static_cast<size_t>(j) >= n_right || used[j] || w <= 0.0) continue;
            used[j] = 1;
            cur[i] = j;
            self(self, i + 1, score + w);
            cur[i] = -1;
            used[j] = 0;
        }
        self(self, i + 1, score);
    };
    dfs(dfs, 0, 0.0);
    return best_assign;
}

struct CandEval {
    int    ri = -1;
    double ssim = -1.0;
    double hist = 0.0;
    double combined = 0.0;
    bool   exact_fmt = false;
    bool   exclusive = false;
    bool   is_flat = false;
};

struct PatchColor {
    std::array<double, 3> mean{};
    std::array<std::array<double, 32>, 3> hist{};
};

using PatchColors = std::array<PatchColor, kPatches>;

inline PatchColors DescribePatches(const PatchSig& sig)
{
    PatchColors colors{};
    if (!sig.valid || sig.nch < 3 || !sig.pw || !sig.ph) return colors;

    const size_t n = static_cast<size_t>(sig.pw) * sig.ph;
    if (sig.px.size() < colors.size() * n * sig.nch) return colors;
    for (size_t k = 0; k < colors.size(); ++k) {
        auto& p = colors[k];
        for (size_t i = 0; i < n; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                const auto v = sig.px[(k * n + i) * sig.nch + c];
                p.mean[c] += v;
                ++p.hist[c][v >> 3];
            }
        }
        for (size_t c = 0; c < 3; ++c) {
            p.mean[c] /= n;
            for (double& count : p.hist[c]) count /= n;
        }
    }
    return colors;
}

inline double MeanDistance(const PatchColor& a, const PatchColor& b)
{
    return (std::abs(a.mean[0] - b.mean[0]) + std::abs(a.mean[1] - b.mean[1]) +
               std::abs(a.mean[2] - b.mean[2])) / 3.0;
}

inline double HistogramIntersection(const PatchColor& a, const PatchColor& b)
{
    double inter = 0.0;
    for (size_t c = 0; c < 3; ++c)
        for (size_t i = 0; i < 32; ++i)
            inter += (std::min)(a.hist[c][i], b.hist[c][i]);
    return inter / 3.0;
}

inline bool BlankPatch(const PatchColor& p)
{
    return p.mean[0] + p.mean[1] + p.mean[2] == 0.0;
}

inline bool IsTwinSimilarity(std::pair<double, double> score)
{
    return score.first >= 0.85 || score.second >= 0.90;
}

struct TwinGroups {
    std::vector<size_t> cluster;
    std::vector<unsigned> active_patches;
    std::vector<std::optional<PatchColors>> colors; // populated only for twin members
};

template <class SameExclusiveFormat>
TwinGroups FindTwinGroups(const std::vector<const PatchSig*>& sigs, SameExclusiveFormat same_format)
{
    TwinGroups groups;
    groups.cluster.resize(sigs.size());
    groups.active_patches.resize(sigs.size());
    groups.colors.resize(sigs.size());

    auto root = [&](size_t i) {
        while (groups.cluster[i] != i) i = groups.cluster[i];
        return i;
    };
    for (size_t i = 0; i < sigs.size(); ++i) {
        groups.cluster[i] = i;
        if (!sigs[i] || !sigs[i]->valid || sigs[i]->is_flat()) continue;
        for (size_t j = 0; j < i; ++j) {
            if (!same_format(i, j) || !sigs[j] || !sigs[j]->valid || sigs[j]->is_flat()) continue;
            if (!IsTwinSimilarity(ComparePatchSigs(*sigs[i], *sigs[j]))) continue;

            if (!groups.colors[i]) groups.colors[i] = DescribePatches(*sigs[i]);
            if (!groups.colors[j]) groups.colors[j] = DescribePatches(*sigs[j]);
            const size_t ri = root(i), rj = root(j);
            groups.cluster[rj] = ri;
            unsigned& active = groups.active_patches[ri];
            active |= groups.active_patches[rj];
            for (int k = 0; k < kPatches; ++k) {
                const auto& a = (*groups.colors[i])[k];
                const auto& b = (*groups.colors[j])[k];
                if (!BlankPatch(a) && !BlankPatch(b) &&
                    (MeanDistance(a, b) >= 6.0 || HistogramIntersection(a, b) <= 0.80))
                    active |= 1u << k;
            }
        }
    }
    for (size_t i = 0; i < sigs.size(); ++i) groups.cluster[i] = root(i);
    return groups;
}

inline double DifferingPatchBonus(const PatchColors& obs, const PatchColors& cand, unsigned active)
{
    double sum = 0.0;
    unsigned count = 0;
    for (int k = 0; k < kPatches; ++k) {
        if (!(active & (1u << k)) || BlankPatch(obs[k]) || BlankPatch(cand[k])) continue;
        const auto& a = obs[k];
        const auto& b = cand[k];
        const double dg = a.mean[1] - b.mean[1];
        const double chroma = (std::abs(a.mean[0] - b.mean[0] - dg) +
                                  std::abs(a.mean[2] - b.mean[2] - dg)) / 2.0;
        sum += 0.45 / (1.0 + MeanDistance(a, b) / 24.0) +
            0.35 / (1.0 + chroma / 16.0) + 0.20 * HistogramIntersection(a, b);
        ++count;
    }
    return count ? 0.08 * sum / count : 0.0;
}

inline constexpr double kDirectSsim = 0.75;

inline double TwinRankWeight(const CandEval& ev, const std::vector<CandEval>& row,
    const TwinGroups& twins, const PatchSig& ref, std::optional<PatchColors>& ref_colors)
{
    const size_t group = twins.cluster[ev.ri];
    const unsigned active = twins.active_patches[group];
    if (!active) return ev.combined;

    // Use color only to separate weak twins, and don't leapfrog better unrelated matches.
    double limit = 0.08;
    for (const auto& other : row) {
        if (twins.cluster[other.ri] == group) {
            if (other.ssim >= kDirectSsim) return ev.combined;
        } else if (other.combined >= ev.combined) {
            limit = (std::min)(limit, (other.combined - ev.combined) * 0.5);
        }
    }
    if (limit == 0.0) return ev.combined;
    if (!ref_colors) ref_colors = DescribePatches(ref);
    return ev.combined + (std::min)(limit, DifferingPatchBonus(*ref_colors, *twins.colors[ev.ri], active));
}

inline bool PreferTwinOwner(const CandEval& a, uint32_t dim_a, const CandEval& b, uint32_t dim_b)
{
    if ((a.ssim >= kDirectSsim) != (b.ssim >= kDirectSsim)) return a.ssim >= kDirectSsim;
    if (dim_a && dim_b) {
        if (dim_a / 2 >= dim_b) return true;
        if (dim_b / 2 >= dim_a) return false;
    }
    return a.combined > b.combined + 1e-6 ||
        (std::abs(a.combined - b.combined) <= 1e-6 && a.ssim >= b.ssim);
}

struct VisualAssignment {
    std::vector<int> assigned;
    std::vector<std::vector<uint8_t>> blocked;
};

// Evals are in descending raw combined-score order. Affinity affects assignment only.
template <class Cooccurs>
VisualAssignment SolveVisualAssignment(const std::vector<std::vector<CandEval>>& evals,
    const std::vector<PatchSig>& refs, const std::vector<uint32_t>& dims, const TwinGroups& twins,
    Cooccurs cooccurs)
{
    const size_t nr = twins.cluster.size(), ng = evals.size();
    VisualAssignment result{ {}, std::vector(ng, std::vector<uint8_t>(nr)) };
    auto& assigned = result.assigned;
    std::vector<std::vector<std::pair<int, double>>> adj(ng);
    for (size_t gi = 0; gi < ng; ++gi) {
        std::optional<PatchColors> ref_colors;
        for (const auto& ev : evals[gi])
            if (!ev.is_flat && ev.combined > 0.0)
                adj[gi].emplace_back(ev.ri, TwinRankWeight(ev, evals[gi], twins, refs[gi], ref_colors));
        if (ref_colors)
            std::stable_sort(adj[gi].begin(), adj[gi].end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
    }

    auto eval_of = [&](size_t gi, int ri) -> const CandEval& {
        const auto& row = evals[gi];
        return *std::find_if(row.begin(), row.end(), [&](const CandEval& ev) { return ev.ri == ri; });
    };
    auto prune_conflicts = [&]() {
        bool pruned = false;
        for (size_t gi = 0; gi < ng; ++gi) {
            const int ri = assigned[gi];
            if (ri < 0) continue;
            const size_t group = twins.cluster[ri];
            for (size_t gj = gi + 1; gj < ng; ++gj) {
                const int rj = assigned[gj];
                if (rj < 0 || ri == rj || twins.cluster[rj] != group || !cooccurs(gi, gj)) continue;
                const size_t loser = PreferTwinOwner(eval_of(gi, ri), dims[gi], eval_of(gj, rj), dims[gj]) ? gj : gi;
                for (size_t r = 0; r < nr; ++r)
                    if (twins.cluster[r] == group) result.blocked[loser][r] = 1;
                std::erase_if(adj[loser], [&](const auto& edge) { return result.blocked[loser][edge.first]; });
                assigned[loser] = -1;
                pruned = true;
                if (loser == gi) break;
            }
        }
        // An unassigned higher-priority row (e.g. a 2048px main diffuse) that co-occurs with
        // an assigned lower-priority row (e.g. a 512px auxiliary texture) holding its twin group
        // must evict the weaker holder so the next pass can reclaim the twin for the true owner,
        // provided the challenger outranks every co-occurring holder in that twin group.
        for (size_t gi = 0; gi < ng; ++gi) {
            if (assigned[gi] >= 0) continue;
            for (const auto& edge : adj[gi]) {
                const int    ri = edge.first;
                if (result.blocked[gi][ri]) continue;
                const size_t group = twins.cluster[ri];
                const auto&  ev_i = eval_of(gi, ri);
                bool         has_cooccur_holder = false;
                bool         beats_all = true;
                for (size_t gj = 0; gj < ng; ++gj) {
                    const int rj = assigned[gj];
                    if (rj < 0 || twins.cluster[rj] != group || !cooccurs(gi, gj)) continue;
                    has_cooccur_holder = true;
                    if (!PreferTwinOwner(ev_i, dims[gi], eval_of(gj, rj), dims[gj])) {
                        beats_all = false;
                        break;
                    }
                }
                if (!has_cooccur_holder || !beats_all) continue;
                for (size_t gj = 0; gj < ng; ++gj) {
                    const int rj = assigned[gj];
                    if (rj < 0 || twins.cluster[rj] != group || !cooccurs(gi, gj)) continue;
                    for (size_t r = 0; r < nr; ++r)
                        if (twins.cluster[r] == group) result.blocked[gj][r] = 1;
                    std::erase_if(adj[gj], [&](const auto& e) { return result.blocked[gj][e.first]; });
                    assigned[gj] = -1;
                    pruned = true;
                }
                break;
            }
        }
        return pruned;
    };

    // Reclaim freed twins before considering reuse. Even on the last pass, drop conflicts.
    for (size_t pass = 0; pass < ng * 2; ++pass) {
        assigned = SolveMaxWeightAssignment(ng, nr, adj);
        if (!prune_conflicts()) break;
    }

    // Singleton groups check same-candidate reuse; twin groups check all variants together.
    std::vector<std::vector<size_t>> holders(nr);
    for (size_t gi = 0; gi < ng; ++gi)
        if (assigned[gi] >= 0) holders[twins.cluster[assigned[gi]]].push_back(gi);
    for (size_t gi = 0; gi < ng; ++gi) {
        if (assigned[gi] >= 0) continue;
        for (const auto& edge : adj[gi]) {
            const int ri = edge.first;
            if (result.blocked[gi][ri]) continue;
            auto& occupied = holders[twins.cluster[ri]];
            if (std::any_of(occupied.begin(), occupied.end(), [&](size_t gj) { return cooccurs(gi, gj); }))
                continue;
            assigned[gi] = ri;
            occupied.push_back(gi);
            break;
        }
    }
    prune_conflicts();
    return result;
}

inline const char* VisualTier(const CandEval& chosen, unsigned excl_same_all, double margin,
    bool twin_pair, double non_twin_margin, bool has_rivals)
{
    if (chosen.ssim >= kDirectSsim) return "high_visual";
    if (excl_same_all == 1 && chosen.exclusive && chosen.exact_fmt && chosen.combined >= 0.15) return "high_unique";
    if (chosen.combined >= 0.45 && margin >= 0.08) return "high_visual";
    if (twin_pair && chosen.combined >= 0.12 && non_twin_margin >= 0.04) return "medium_twin";
    if (chosen.combined >= 0.28 &&
        (chosen.ssim >= 0.08 || chosen.hist >= 0.55 ||
            (chosen.exclusive && chosen.exact_fmt && chosen.hist >= 0.40 && margin >= 0.08)) &&
        (margin >= 0.03 || (!chosen.exact_fmt && chosen.hist >= 0.25 && margin >= 0.05)))
        return "medium_visual";
    if (!chosen.exact_fmt && chosen.exclusive &&
        ((!has_rivals && chosen.hist >= 0.20) ||
            (chosen.combined >= 0.22 && chosen.hist >= 0.35 && margin >= 0.06)))
        return has_rivals ? "medium_cross_bc" : "medium_cross_bc_unique";
    return nullptr;
}

struct CutoutMaskStats {
    uint64_t total = 0;
    uint64_t opaque = 0;
    uint64_t gray = 0;
    uint64_t white = 0;
    uint64_t black = 0;
    uint64_t dark = 0;
    uint64_t mid = 0;
};

inline void AccumulateCutoutTexel(CutoutMaskStats& st, uint8_t r, uint8_t g, uint8_t b, uint8_t a) noexcept
{
    ++st.total;
    if (a >= 245) ++st.opaque;
    const uint8_t mn = (std::min)({ r, g, b });
    const uint8_t mx = (std::max)({ r, g, b });
    if (static_cast<uint32_t>(mx - mn) <= 8u) ++st.gray;
    if (mx <= 48) ++st.dark;
    if (mn >= 240)
        ++st.white;
    else if (mx <= 15)
        ++st.black;
    else
        ++st.mid;
}

// Binary white-with-black-cutout visibility/alpha-mask detector (e.g. outfit/form cutout masks).
// Requires opaque grayscale pixels, majority white (>=50%), genuine black cutouts (>=0.1%),
// and sharp binary transitions (<=6% mid-gray anti-aliased edge pixels).
inline bool IsBinaryCutoutMask(const CutoutMaskStats& st) noexcept
{
    if (st.total == 0) return false;
    const double inv = 1.0 / static_cast<double>(st.total);
    const double opaque_frac = static_cast<double>(st.opaque) * inv;
    const double gray_frac = static_cast<double>(st.gray) * inv;
    const double white_frac = static_cast<double>(st.white) * inv;
    const double black_frac = static_cast<double>(st.black) * inv;
    const double mid_frac = static_cast<double>(st.mid) * inv;
    return opaque_frac >= 0.98 && gray_frac >= 0.98 &&
        white_frac >= 0.50 && black_frac >= 0.001 && mid_frac <= 0.06;
}

// Low-mip (64x64 / 128x128) resident streaming-tail cutout mask detector.
// Accounts for linear-space mip downsampling into sRGB (where 1-2% white bleed raises dark
// cutout interior texels into 16..48 and widens anti-aliased edge bands proportionally).
inline bool IsBinaryCutoutMaskLowMip(const CutoutMaskStats& st) noexcept
{
    if (st.total == 0) return false;
    const double inv = 1.0 / static_cast<double>(st.total);
    const double opaque_frac = static_cast<double>(st.opaque) * inv;
    const double gray_frac = static_cast<double>(st.gray) * inv;
    const double white_frac = static_cast<double>(st.white) * inv;
    const double black_frac = static_cast<double>(st.black) * inv;
    const double dark_frac = static_cast<double>(st.dark) * inv;
    const double mid_frac = static_cast<double>(st.mid) * inv;
    return opaque_frac >= 0.98 && gray_frac >= 0.98 &&
        white_frac >= 0.50 && mid_frac <= 0.12 &&
        (black_frac >= 0.001 || (white_frac >= 0.80 && dark_frac >= 0.001));
}

// Exact mip-chain equivalence table mapping streaming sub-mip hashes (e.g. 1024px sub1)
// to their higher-resolution mip0 parent hash. Any sub-mip hash observed under >=2
// unrelated root parents (e.g. blank/shared placeholder mips) is permanently marked
// ambiguous and never aliased.
class VisualMipParentTable {
public:
    static constexpr uint32_t kAmbiguous = 0xFFFFFFFFu;
    static constexpr size_t   kMaxEntries = 32768;

    uint32_t Resolve(uint32_t hash) const noexcept
    {
        if (!hash || hash == kAmbiguous || map_.empty()) return 0;
        uint32_t cur = hash;
        for (int hop = 0; hop < 4; ++hop) {
            auto it = map_.find(cur);
            if (it == map_.end()) break;
            const uint32_t next = it->second;
            if (!next || next == kAmbiguous || next == cur || next == hash)
                return (cur != hash && next != kAmbiguous) ? cur : 0;
            cur = next;
        }
        return cur != hash ? cur : 0;
    }

    bool Note(uint32_t child, uint32_t parent)
    {
        if (!child || !parent || child == parent || child == kAmbiguous || parent == kAmbiguous) return false;
        const uint32_t p_root = RootOrSelf(parent);
        if (p_root == child) return false;
        auto it = map_.find(child);
        if (it == map_.end()) {
            if (map_.size() >= kMaxEntries) return false;
            map_.emplace(child, parent);
            return true;
        }
        if (it->second == parent || it->second == kAmbiguous) return false;
        const uint32_t e_root = RootOrSelf(it->second);
        if (e_root == p_root) {
            if (it->second != p_root) {
                it->second = p_root;
                return true;
            }
            return false;
        }
        it->second = kAmbiguous;
        return true;
    }

    template <class ParentPred>
    std::vector<std::pair<uint32_t, uint32_t>> CollectForParents(ParentPred keep_parent) const
    {
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (const auto& kv : map_) {
            if (kv.second == kAmbiguous) continue;
            const uint32_t root = Resolve(kv.first);
            if (root && keep_parent(root)) out.emplace_back(kv.first, root);
        }
        return out;
    }

private:
    uint32_t RootOrSelf(uint32_t h) const noexcept
    {
        const uint32_t r = Resolve(h);
        return r ? r : h;
    }

    std::unordered_map<uint32_t, uint32_t> map_;
};

// Pure opaque white neutralizer DDS detector (e.g. mod-supplied white mask clear texture).
inline bool IsPureWhiteNeutralizer(const CutoutMaskStats& st) noexcept
{
    if (st.total == 0) return false;
    const double inv = 1.0 / static_cast<double>(st.total);
    const double opaque_frac = static_cast<double>(st.opaque) * inv;
    const double gray_frac = static_cast<double>(st.gray) * inv;
    const double white_frac = static_cast<double>(st.white) * inv;
    const double mid_frac = static_cast<double>(st.mid) * inv;
    return opaque_frac >= 0.998 && gray_frac >= 0.998 &&
        white_frac >= 0.998 && st.black == 0 && mid_frac <= 0.002;
}

// Mix the pixel shader's declared SRV register mask into the program hash so passes sharing the
// same VS hash but using different PS SRV layouts (e.g. decl=0x1 t0=diffuse vs decl=0x3 t0=mask,
// t1=diffuse) never collide in proven-slot or disk-slot anti-flicker caches.
inline uint64_t ComputeSlotShaderKey(uint64_t prog_hash, uint64_t ps_decl_mask, bool ps_decl_known) noexcept
{
    if (!prog_hash) return 0;
    if (!ps_decl_known) return prog_hash;
    uint64_t h = prog_hash ^ 0x9e3779b97f4a7c15ull;
    h ^= ps_decl_mask;
    h *= 0x100000001b3ull;
    return h ? h : 1ull;
}

} // namespace wwmi_struct_tex
