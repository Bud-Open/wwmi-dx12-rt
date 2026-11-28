// ===========================================================================
//  tex_look.cpp -- see tex_look.h. A texture recognised by how it looks: five patches of its mip0
//  (centre, four corners inset 1/8, side 1/16) box-averaged to one density, a histogram shortlist, then
//  SSIM per patch against every reference (a mod target, or a proven lower mip of one). A match becomes a
//  look-alike link in tex_lod. References and one verdict per content hash persist in tex_look.bin under
//  the algorithm version: a hash is decoded once, again only when the references changed since.
// ===========================================================================
#define BCDEC_IMPLEMENTATION
#include "../../third_party/bcdec/bcdec.h"
#include "tex_look.h"
#include "tex_lod.h"
#include "tex_seed_dirs.hpp"
#include "tex_struct_solver.hpp"
#include "tex_struct_recipe_cache.hpp"
#include "tex_live_sig_retention.hpp"
#include "../../src/log.h"
#include "../../src/dxgi_bc.h"
#include "../config.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>
#include <share.h>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

extern bool g_tex_struct_fallback;
bool        TexHashHasResidentCarrier(uint32_t hash);

namespace {

constexpr uint32_t kAlgo = 1;           // what a signature and a verdict mean; another value discards the file
constexpr uint32_t kMagic = 0x4b4c5854;       // "TXLK"
constexpr uint32_t kMagicPlaces = 0x324c5854; // "TXL2": the same, then the places
constexpr uint32_t kMagicMesh = 0x334c5854;   // "TXL3": the same, then the mesh places
constexpr uint32_t kMixed = 0xffffffffu;      // a place that showed two targets
constexpr int      kPatches = 5;
constexpr uint32_t kRefSide = 512; // a patch holds at most kRefSide/16 cells a side
constexpr uint32_t kMaxCells = kRefSide / 16;
constexpr float    kHistMax = 0.08f;
constexpr float    kSsimMin = 0.80f;
constexpr float    kLiveStd = 4.0f;
constexpr int      kLiveMin = 3;
constexpr double   kC1 = (0.01 * 255) * (0.01 * 255);
constexpr double   kC2 = (0.03 * 255) * (0.03 * 255);

struct Codec {
    uint8_t id = 0;
    uint8_t nch = 0;
};
Codec codec_of(uint32_t fmt)
{
    if (fmt >= 70 && fmt <= 72) return { 1, 3 };
    if (fmt >= 76 && fmt <= 78) return { 3, 4 };
    if (fmt == 79 || fmt == 80) return { 4, 1 };
    if (fmt == 82 || fmt == 83) return { 5, 2 };
    if (fmt >= 97 && fmt <= 99) return { 7, 4 };
    return {};
}
bool pow2(uint32_t v)
{
    return v && !(v & (v - 1));
}
bool eligible(uint32_t w, uint32_t h, uint32_t fmt)
{
    return pow2(w) && pow2(h) && (std::min)(w, h) >= 128 && codec_of(fmt).id;
}
uint32_t cells_of(uint32_t w, uint32_t h)
{
    return ((std::min)(w, kRefSide) / 16) * ((std::min)(h, kRefSide) / 16);
}

struct Corner {
    uint32_t x = 0;
    uint32_t y = 0;
};
struct Geometry {
    uint32_t sw = 0; // patch side in mip0 texels
    uint32_t sh = 0;
    uint32_t fx = 0; // texels per cell
    uint32_t fy = 0;
    uint32_t pw = 0; // cells per patch side
    uint32_t ph = 0;
    Corner   at[kPatches];
};
Geometry geometry_of(uint32_t w, uint32_t h)
{
    Geometry g;
    g.sw = w / 16;
    g.sh = h / 16;
    g.fx = w / (std::min)(w, kRefSide);
    g.fy = h / (std::min)(h, kRefSide);
    g.pw = g.sw / g.fx;
    g.ph = g.sh / g.fy;
    const uint32_t iw = w / 8;
    const uint32_t ih = h / 8;
    g.at[0] = { (w - g.sw) / 2, (h - g.sh) / 2 };
    g.at[1] = { iw, ih };
    g.at[2] = { w - iw - g.sw, ih };
    g.at[3] = { iw, h - ih - g.sh };
    g.at[4] = { w - iw - g.sw, h - ih - g.sh };
    return g;
}

struct LookSig {
    uint8_t              codec = 0;
    uint8_t              nch = 0;
    uint32_t             w = 0; // the mip it was taken from
    uint32_t             h = 0;
    uint32_t             pw = 0;
    uint32_t             ph = 0;
    std::vector<uint8_t> px; // kPatches x ph x pw x nch
};

struct LookStats {
    float hist[4][32] = {};
    float ch_std[4] = {};
    float patch_std[kPatches][4] = {};
};

struct LookView {
    uint32_t           pw = 0;
    uint32_t           ph = 0;
    uint32_t           nch = 0;
    std::vector<float> v; // kPatches x ph x pw x nch
    LookStats          st;
};

struct Ref {
    LookSig   sig;
    LookStats st; // at the signature's own density
};

enum class Kind : uint8_t {
    NoMatch = 0,
    Match = 1,
    Ref = 2, // offered as a reference: a target, or a proven lower mip of one
};
struct Verdict {
    uint32_t gen = 0; // the reference generation it was decided against
    Kind     kind = Kind::NoMatch;
};

struct Score {
    float hist = 1.f;
    float ssim = -1.f; // below 0: not measured, or too flat to tell
    bool  match() const { return hist <= kHistMax && ssim >= kSsimMin; }
};

void decode_block(uint8_t codec, const uint8_t* blk, uint8_t rgba[64])
{
    memset(rgba, 0, 64);
    if (codec == 1)
        bcdec_bc1(blk, rgba, 16);
    else if (codec == 3)
        bcdec_bc3(blk, rgba, 16);
    else if (codec == 7)
        bcdec_bc7(blk, rgba, 16);
    else if (codec == 4) {
        uint8_t r[16];
        bcdec_bc4(blk, r, 4);
        for (int i = 0; i < 16; ++i) rgba[i * 4] = r[i];
    } else if (codec == 5) {
        uint8_t rg[32];
        bcdec_bc5(blk, rg, 8);
        for (int i = 0; i < 16; ++i) {
            rgba[i * 4] = rg[i * 2];
            rgba[i * 4 + 1] = rg[i * 2 + 1];
        }
    }
}

bool sig_of(const TexLookCut& cut, LookSig& s)
{
    if (!eligible(cut.w, cut.h, cut.fmt)) return false;
    const Codec    c = codec_of(cut.fmt);
    const Geometry g = geometry_of(cut.w, cut.h);
    const uint32_t bb = DxgiBcBlockBytes(cut.fmt);
    const uint32_t nbx = g.sw / 4;
    const uint32_t nby = g.sh / 4;
    if (cut.blocks.size() != (size_t)kPatches * nbx * nby * bb) return false;
    s.codec = c.id;
    s.nch = c.nch;
    s.w = cut.w;
    s.h = cut.h;
    s.pw = g.pw;
    s.ph = g.ph;
    const size_t cells = (size_t)g.pw * g.ph * c.nch;
    s.px.assign(kPatches * cells, 0);
    std::vector<uint32_t> sum(cells);
    const uint8_t*        blk = cut.blocks.data();
    const double          per = (double)g.fx * g.fy;
    for (int k = 0; k < kPatches; ++k) {
        std::fill(sum.begin(), sum.end(), 0u);
        for (uint32_t by = 0; by < nby; ++by)
            for (uint32_t bx = 0; bx < nbx; ++bx, blk += bb) {
                uint8_t t[64];
                decode_block(c.id, blk, t);
                for (uint32_t i = 0; i < 16; ++i) {
                    const uint32_t tx = bx * 4 + (i & 3);
                    const uint32_t ty = by * 4 + (i >> 2);
                    uint32_t*      cell = &sum[((size_t)(ty / g.fy) * g.pw + tx / g.fx) * c.nch];
                    for (uint32_t ch = 0; ch < c.nch; ++ch) cell[ch] += t[i * 4 + ch];
                }
            }
        uint8_t* out = &s.px[k * cells];
        for (size_t i = 0; i < cells; ++i) out[i] = (uint8_t)std::nearbyint(sum[i] / per);
    }
    return true;
}

void stats_of(LookView& o)
{
    const size_t cells = (size_t)o.pw * o.ph;
    const size_t all = kPatches * cells;
    for (uint32_t c = 0; c < o.nch; ++c) {
        double s = 0;
        double ss = 0;
        for (int k = 0; k < kPatches; ++k) {
            double ps = 0;
            double pss = 0;
            for (size_t i = 0; i < cells; ++i) {
                const float v = o.v[(k * cells + i) * o.nch + c];
                ++o.st.hist[c][(std::min)(31, (int)v >> 3)];
                ps += v;
                pss += (double)v * v;
            }
            const double pm = ps / cells;
            o.st.patch_std[k][c] = (float)std::sqrt((std::max)(0.0, pss / cells - pm * pm));
            s += ps;
            ss += pss;
        }
        const double m = s / all;
        o.st.ch_std[c] = (float)std::sqrt((std::max)(0.0, ss / all - m * m));
        for (float& b : o.st.hist[c]) b /= (float)all;
    }
}

// The signature at a coarser (or its own) cell density: each cell the mean of the cells it covers.
LookView view_of(const LookSig& s, uint32_t pw, uint32_t ph)
{
    LookView o;
    o.pw = pw;
    o.ph = ph;
    o.nch = s.nch;
    const uint32_t fx = s.pw / pw;
    const uint32_t fy = s.ph / ph;
    o.v.assign((size_t)kPatches * pw * ph * s.nch, 0.f);
    for (int k = 0; k < kPatches; ++k)
        for (uint32_t y = 0; y < s.ph; ++y)
            for (uint32_t x = 0; x < s.pw; ++x)
                for (uint32_t c = 0; c < s.nch; ++c)
                    o.v[(((size_t)k * ph + y / fy) * pw + x / fx) * s.nch + c] +=
                        s.px[(((size_t)k * s.ph + y) * s.pw + x) * s.nch + c];
    const float per = (float)(fx * fy);
    for (float& v : o.v) v /= per;
    stats_of(o);
    return o;
}

float hist_dist(const LookStats& a, const LookStats& b, uint32_t nch)
{
    double sum = 0;
    int    n = 0;
    for (uint32_t c = 0; c < nch; ++c) {
        if (a.ch_std[c] < 1.f && b.ch_std[c] < 1.f) continue;
        double in = 0;
        for (int i = 0; i < 32; ++i) in += (std::min)(a.hist[c][i], b.hist[c][i]);
        sum += in;
        ++n;
    }
    return n ? (float)(1.0 - sum / n) : 1.f;
}

double ssim_plane(const LookView& a, const LookView& b, int k, uint32_t c, uint32_t win)
{
    const size_t base = (size_t)k * a.ph * a.pw;
    const double n = (double)win * win;
    double       total = 0;
    uint32_t     windows = 0;
    for (uint32_t y0 = 0; y0 + win <= a.ph; ++y0)
        for (uint32_t x0 = 0; x0 + win <= a.pw; ++x0) {
            double sa = 0;
            double sb = 0;
            double saa = 0;
            double sbb = 0;
            double sab = 0;
            for (uint32_t y = y0; y < y0 + win; ++y)
                for (uint32_t x = x0; x < x0 + win; ++x) {
                    const size_t i = (base + (size_t)y * a.pw + x) * a.nch + c;
                    const double va = a.v[i];
                    const double vb = b.v[i];
                    sa += va;
                    sb += vb;
                    saa += va * va;
                    sbb += vb * vb;
                    sab += va * vb;
                }
            const double ma = sa / n;
            const double mb = sb / n;
            const double var_a = saa / n - ma * ma;
            const double var_b = sbb / n - mb * mb;
            const double cov = sab / n - ma * mb;
            total += ((2 * ma * mb + kC1) * (2 * cov + kC2)) / ((ma * ma + mb * mb + kC1) * (var_a + var_b + kC2));
            ++windows;
        }
    return windows ? total / windows : 0;
}

// Lowest SSIM over the (patch, channel) planes with structure on either side; below 0 if fewer than
// kLiveMin patches have any.
float ssim_min(const LookView& a, const LookView& b)
{
    const uint32_t win = (std::min)({ 7u, a.pw, a.ph });
    int            live = 0;
    double         lo = 2;
    for (int k = 0; k < kPatches; ++k) {
        bool any = false;
        for (uint32_t c = 0; c < a.nch; ++c) {
            if ((std::max)(a.st.patch_std[k][c], b.st.patch_std[k][c]) < kLiveStd) continue;
            lo = (std::min)(lo, ssim_plane(a, b, k, c, win));
            any = true;
        }
        live += any;
    }
    return live >= kLiveMin ? (float)lo : -1.f;
}

// `cv` is `cand` at its own density.
Score compare(const LookSig& cand, const LookView& cv, const Ref& r)
{
    Score s;
    if (cand.codec != r.sig.codec || (uint64_t)cand.w * r.sig.h != (uint64_t)cand.h * r.sig.w) return s;
    const uint32_t pw = (std::min)(cand.pw, r.sig.pw);
    const uint32_t ph = (std::min)(cand.ph, r.sig.ph);
    const bool     ref_native = pw == r.sig.pw && ph == r.sig.ph;
    LookView       down;
    if (pw != cand.pw || ph != cand.ph) down = view_of(cand, pw, ph);
    const LookView& a = down.v.empty() ? cv : down;
    if (ref_native) {
        s.hist = hist_dist(a.st, r.st, cand.nch);
        if (s.hist > kHistMax) return s;
    }
    const LookView b = view_of(r.sig, pw, ph);
    if (!ref_native) {
        s.hist = hist_dist(a.st, b.st, cand.nch);
        if (s.hist > kHistMax) return s;
    }
    s.ssim = ssim_min(a, b);
    return s;
}

int live_patches(const LookStats& st, uint32_t nch)
{
    int live = 0;
    for (int k = 0; k < kPatches; ++k)
        for (uint32_t c = 0; c < nch; ++c)
            if (st.patch_std[k][c] >= kLiveStd) {
                ++live;
                break;
            }
    return live;
}

template <typename T>
bool put(FILE* f, const T& v)
{
    return fwrite(&v, sizeof v, 1, f) == 1;
}
template <typename T>
bool get(FILE* f, T& v)
{
    return fread(&v, sizeof v, 1, f) == 1;
}

using PlaceMap = std::unordered_map<uint64_t, uint32_t>;

class LookIndex {
public:
    void Init(const std::wstring& cache_dir);
    bool Wants(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target);
    void OnCut(uint32_t hash, const TexLookCut& cut, bool is_target);
    void FlushIfQuiet(unsigned quiet_ms);
    void OnExit();
    void AtPlace(uint64_t place, uint32_t hash, uint32_t target);
    void AtMeshPlace(uint64_t place, uint32_t hash, uint32_t target);
    void AddMeshPlace(uint64_t place, uint32_t target);
    uint32_t MeshMask(uint64_t pass, unsigned regs) const;
    bool HasMeshPlaces() const { return has_mesh_.load(std::memory_order_relaxed); }
    bool HasRef(uint32_t target) const
    {
        if (!target) return false;
        std::shared_lock l(mtx_);
        return refs_.count(target) != 0;
    }
    bool HasAnySig(uint32_t key) const
    {
        if (!key) return false;
        std::shared_lock l(mtx_);
        return refs_.count(key) != 0 || live_sigs_.count(key) != 0;
    }
    bool WantsLiveSig(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt) const
    {
        if (!g_tex_struct_fallback || !hash || w < 256 || h < 256 || (std::max)(w, h) > (std::min)(w, h) * 2 ||
            !eligible(w, h, fmt))
            return false;
        std::shared_lock l(mtx_);
        return refs_.count(hash) == 0 && live_sigs_.count(hash) == 0;
    }
    void StoreLiveSig(uint32_t hash, const LookSig& sig)
    {
        if (!hash || sig.w < 256 || sig.h < 256 || (std::max)(sig.w, sig.h) > (std::min)(sig.w, sig.h) * 2 ||
            sig.px.empty())
            return;
        std::vector<uint32_t> candidates;
        {
            std::shared_lock l(mtx_);
            if (refs_.count(hash) || live_sigs_.count(hash)) return;
            candidates = wwmi_tex_live_sig::PeekEvictCandidates(live_order_, live_sigs_.size());
        }
        std::vector<uint8_t> resident_flags;
        if (!candidates.empty()) {
            resident_flags.resize(candidates.size(), 0u);
            for (size_t i = 0; i < candidates.size(); ++i)
                resident_flags[i] = TexHashHasResidentCarrier(candidates[i]) ? 1u : 0u;
        }
        std::unique_lock l(mtx_);
        if (refs_.count(hash) || live_sigs_.count(hash)) return;
        wwmi_struct_tex::PublishSeq::Guard pub(TexStructMetaSeq());
        if (live_sigs_.size() >= wwmi_tex_live_sig::kSoftMaxLiveSigs) {
            wwmi_tex_live_sig::ApplyResidentFirstEviction(live_sigs_, live_order_, candidates, resident_flags);
        }
        live_sigs_.emplace(hash, sig);
        live_order_.push_back(hash);
    }
    void CacheLiveCut(uint32_t hash, const TexLookCut& cut)
    {
        if (!g_tex_struct_fallback || !hash) return;
        LookSig sig;
        if (!sig_of(cut, sig)) return;
        StoreLiveSig(hash, sig);
    }
    bool CopyRefSig(uint32_t target, uint8_t* codec, uint8_t* nch, uint32_t* w, uint32_t* h,
        uint32_t* pw, uint32_t* ph, std::vector<uint8_t>* px) const
    {
        if (!target) return false;
        std::shared_lock l(mtx_);
        auto             it = refs_.find(target);
        if (it == refs_.end()) return false;
        const LookSig& s = it->second.sig;
        if (codec) *codec = s.codec;
        if (nch) *nch = s.nch;
        if (w) *w = s.w;
        if (h) *h = s.h;
        if (pw) *pw = s.pw;
        if (ph) *ph = s.ph;
        if (px) *px = s.px;
        return true;
    }
    bool CopyAnySig(uint32_t key, uint8_t* codec, uint8_t* nch, uint32_t* w, uint32_t* h,
        uint32_t* pw, uint32_t* ph, std::vector<uint8_t>* px) const
    {
        if (!key) return false;
        std::shared_lock l(mtx_);
        const LookSig*   sp = nullptr;
        auto             it = refs_.find(key);
        if (it != refs_.end()) {
            sp = &it->second.sig;
        } else {
            auto lit = live_sigs_.find(key);
            if (lit != live_sigs_.end()) sp = &lit->second;
        }
        if (!sp) return false;
        if (codec) *codec = sp->codec;
        if (nch) *nch = sp->nch;
        if (w) *w = sp->w;
        if (h) *h = sp->h;
        if (pw) *pw = sp->pw;
        if (ph) *ph = sp->ph;
        if (px) *px = sp->px;
        return true;
    }
    void CheckCutoutMask(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, const uint8_t* bc, size_t bc_bytes)
    {
        if (!g_tex_struct_fallback || !hash || !bc || w != h || w < 64 || !pow2(w)) return;
        const Codec c = codec_of(fmt);
        if (c.id != 1 && c.id != 3 && c.id != 7) return;
        const uint32_t bb = DxgiBcBlockBytes(fmt);
        const uint32_t nbx = w / 4;
        const uint32_t nby = h / 4;
        const size_t   row_bytes = static_cast<size_t>(nbx) * bb;
        if (!bb || bc_bytes < row_bytes * nby) return;
        {
            std::shared_lock l(mtx_);
            if (cutout_masks_.count(hash)) return;
        }

        // Stage 1: ultra-fast 4x4 block pre-filter (exits in ~1 us on non-binary textures).
        const uint8_t                    dark_max = (w <= 128) ? 48u : 15u;
        wwmi_struct_tex::CutoutMaskStats s1{};
        uint64_t                         bad1 = 0;
        bool                             pass1 = true;
        for (uint32_t gy = 0; gy < 4 && pass1; ++gy) {
            const uint32_t by = ((gy * 2 + 1) * nby) / 8;
            for (uint32_t gx = 0; gx < 4; ++gx) {
                const uint32_t bx = ((gx * 2 + 1) * nbx) / 8;
                const uint8_t* blk = bc + static_cast<size_t>(by) * row_bytes + static_cast<size_t>(bx) * bb;
                uint8_t        rgba[64];
                decode_block(c.id, blk, rgba);
                for (int i = 0; i < 16; ++i) {
                    const uint8_t r = rgba[i * 4];
                    const uint8_t g = rgba[i * 4 + 1];
                    const uint8_t b = rgba[i * 4 + 2];
                    const uint8_t a = rgba[i * 4 + 3];
                    wwmi_struct_tex::AccumulateCutoutTexel(s1, r, g, b, a);
                    const uint8_t mn = (std::min)({ r, g, b });
                    const uint8_t mx = (std::max)({ r, g, b });
                    if (a < 245 || static_cast<uint32_t>(mx - mn) > 8u || (mn < 240 && mx > dark_max)) ++bad1;
                }
                if (bad1 > 64) {
                    pass1 = false;
                    break;
                }
            }
        }

        bool                             is_cutout = false;
        wwmi_struct_tex::CutoutMaskStats s2{};
        if (pass1 && s1.white >= 64) {
            // Stage 2: uniform BC block scan (full coverage for <=256px, 64x64 strided grid =
            // 4,096 blocks = 65,536 texels for >=512px) so even small 0.2%-1% UV cutouts
            // (masks, sleeves, decals) are reliably captured at any streaming mip level.
            const uint32_t grid = (std::min)(64u, nbx);
            for (uint32_t gy = 0; gy < grid; ++gy) {
                const uint32_t by = (grid == nby) ? gy : (((gy * 2 + 1) * nby) / (grid * 2));
                for (uint32_t gx = 0; gx < grid; ++gx) {
                    const uint32_t bx = (grid == nbx) ? gx : (((gx * 2 + 1) * nbx) / (grid * 2));
                    const uint8_t* blk = bc + static_cast<size_t>(by) * row_bytes + static_cast<size_t>(bx) * bb;
                    uint8_t        rgba[64];
                    decode_block(c.id, blk, rgba);
                    for (int i = 0; i < 16; ++i)
                        wwmi_struct_tex::AccumulateCutoutTexel(s2, rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]);
                }
            }
            is_cutout = (w <= 128) ? wwmi_struct_tex::IsBinaryCutoutMaskLowMip(s2)
                                   : wwmi_struct_tex::IsBinaryCutoutMask(s2);
        }

        bool inserted = false;
        {
            std::unique_lock l(mtx_);
            if (is_cutout) {
                wwmi_struct_tex::PublishSeq::Guard pub(TexStructMetaSeq());
                auto [it, ins] = cutout_masks_.emplace(hash, true);
                inserted = ins;
                if (!ins && !it->second) it->second = true;
                has_cutout_mask_.store(true, std::memory_order_release);
            } else if (cutout_masks_.size() < 16384) {
                inserted = cutout_masks_.emplace(hash, false).second;
            }
        }
        if (is_cutout) {
            if (inserted && s2.total > 0) {
                const double inv = 1.0 / static_cast<double>(s2.total);
                logger::probe("TexLook: cutout-mask detected hash=%08x %ux%u fmt=%u (white=%.3f black=%.3f mid=%.3f gray=%.3f opaque=%.3f)",
                    hash, w, h, fmt, s2.white * inv, s2.black * inv, s2.mid * inv, s2.gray * inv, s2.opaque * inv);
            }
        }
    }
    bool IsCutoutMask(uint32_t hash) const
    {
        if (!hash || !has_cutout_mask_.load(std::memory_order_acquire)) return false;
        std::shared_lock l(mtx_);
        auto             it = cutout_masks_.find(hash);
        return it != cutout_masks_.end() && it->second;
    }
    void MarkCutoutMask(uint32_t hash)
    {
        if (!hash) return;
        std::unique_lock l(mtx_);
        auto             it = cutout_masks_.find(hash);
        if (it != cutout_masks_.end() && it->second) return;
        wwmi_struct_tex::PublishSeq::Guard pub(TexStructMetaSeq());
        cutout_masks_[hash] = true;
        has_cutout_mask_.store(true, std::memory_order_release);
    }

private:
    struct LoadStats {
        size_t refs_taken = 0;
        size_t places_taken = 0;
        size_t mesh_places_taken = 0;
        bool   opened = false;
        bool   ok = false;
    };

    std::wstring path() const { return dir_.empty() ? std::wstring() : dir_ + L"\\tex_look.bin"; }
    std::wstring seed_state_path() const { return dir_.empty() ? std::wstring() : dir_ + L"\\tex_look_seeds.state"; }
    bool         weaker(uint32_t target, uint32_t w, uint32_t h) const;
    uint32_t     target_of(uint32_t hash, bool is_target) const;
    bool         offered(uint32_t hash) const;
    bool         keep_ref(uint32_t target, Ref&& r);
    void         offer(uint32_t source, uint32_t target, LookSig&& sig);
    void         match(uint32_t hash, const LookSig& sig);
    void         mark_dirty();
    bool         keep_place(PlaceMap& m, uint64_t place, uint32_t target, bool touch_dirty = true);
    void         at_place(PlaceMap& m, uint64_t place, uint32_t hash, uint32_t target);
    LoadStats    load(const std::wstring& file, bool seed);
    void         write();

    mutable std::shared_mutex                          mtx_;
    std::unordered_map<uint32_t, Ref>                  refs_;     // target -> its reference
    std::unordered_map<uint32_t, LookSig>              live_sigs_; // in-memory ring of recent live square BC textures (no disk persistence)
    std::deque<uint32_t>                               live_order_;
    std::unordered_map<uint32_t, bool>                 cutout_masks_; // content hash -> binary white-with-black-cutout mask
    std::atomic<bool>                                  has_cutout_mask_{ false };
    std::unordered_map<uint32_t, Verdict>              verdicts_; // content hash -> what its decode decided
    PlaceMap                                           places_;      // a pass's slot -> the target HD draws there, or kMixed
    PlaceMap                                           mesh_places_; // the same for a game character draw's slot
    std::atomic<bool>                                  has_mesh_{ false };
    std::atomic<uint32_t>                              mesh_gen_{ 1 }; // bumped by every change to mesh_places_
    std::unordered_set<uint64_t>                       placed_;   // (hash, target) a place linked this session
    uint32_t                                           gen_ = 1;  // bumped by every reference added or replaced
    bool                                               loaded_ = false;
    bool                                               had_main_cache_ = false;
    bool                                               dirty_ = false;
    uint64_t                                           dirty_at_ = 0;
    std::wstring                                       dir_;
    std::vector<std::pair<std::wstring, std::wstring>> imported_; // seed taken -> its DISABLED_ name, after the next write
    wwmi_seed::Ledger                                  seed_ledger_;
    bool                                               seed_ledger_dirty_ = false;
    std::atomic<uint32_t>                              decoded_{ 0 };
    std::atomic<uint32_t>                              skipped_{ 0 };
    std::atomic<uint32_t>                              matched_{ 0 };
    std::atomic<uint32_t>                              refs_new_{ 0 };
    std::atomic<uint32_t>                              places_new_{ 0 };
    std::atomic<uint32_t>                              by_place_{ 0 };
};

void LookIndex::mark_dirty()
{ // caller holds mtx_
    dirty_ = true;
    dirty_at_ = GetTickCount64();
}

bool LookIndex::keep_place(PlaceMap& m, uint64_t place, uint32_t target, bool touch_dirty)
{ // caller holds mtx_
    auto it = m.find(place);
    if (it == m.end()) {
        m.emplace(place, target);
    } else {
        if (it->second == target || it->second == kMixed) return false;
        it->second = kMixed;
    }
    if (&m == &mesh_places_) mesh_gen_.fetch_add(1, std::memory_order_release);
    if (touch_dirty) mark_dirty();
    return true;
}

// `target`: what `hash` is to the mods (a target, or exactly one), 0 for a texture they do not know. A known one
// records its place; an unknown one at a place recorded with one target becomes that target's look-alike.
void LookIndex::at_place(PlaceMap& m, uint64_t place, uint32_t hash, uint32_t target)
{
    if (!g_texlod_cache || !hash) return;
    if (target) {
        {
            std::shared_lock l(mtx_);
            auto             it = m.find(place);
            if (it != m.end() && (it->second == target || it->second == kMixed)) return;
        }
        std::unique_lock l(mtx_);
        if (keep_place(m, place, target)) places_new_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (TexLodDegenerate(hash)) return;
    uint32_t to = 0;
    {
        std::shared_lock l(mtx_);
        auto             it = m.find(place);
        if (it == m.end() || it->second == kMixed) return;
        to = it->second;
        if (placed_.count(((uint64_t)hash << 32) | to)) return;
    }
    {
        std::unique_lock l(mtx_);
        if (!placed_.insert(((uint64_t)hash << 32) | to).second) return;
    }
    if (!TexLodLearnPlace(hash, to)) return;
    by_place_.fetch_add(1, std::memory_order_relaxed);
    logger::info("TexLook: %08x is drawn where HD draws %08x -- its look-alike by place", hash, to);
}

void LookIndex::AtPlace(uint64_t place, uint32_t hash, uint32_t target)
{
    at_place(places_, place, hash, target);
}

void LookIndex::AtMeshPlace(uint64_t place, uint32_t hash, uint32_t target)
{
    at_place(mesh_places_, place, hash, target);
}

void LookIndex::AddMeshPlace(uint64_t place, uint32_t target)
{
    std::unique_lock l(mtx_);
    if (keep_place(mesh_places_, place, target)) places_new_.fetch_add(1, std::memory_order_relaxed);
    has_mesh_.store(true, std::memory_order_relaxed);
}

// Remembered per thread and pass until mesh_places_ changes: a character draw asks every frame.
uint32_t LookIndex::MeshMask(uint64_t pass, unsigned regs) const
{
    struct Memo {
        uint32_t mask = 0;
        uint32_t gen = 0;
    };
    thread_local std::unordered_map<uint64_t, Memo> t_memo;
    const uint32_t                                  gen = mesh_gen_.load(std::memory_order_acquire);
    auto                                            it = t_memo.find(pass);
    if (it != t_memo.end() && it->second.gen == gen) return it->second.mask;
    uint32_t mask = 0;
    {
        std::shared_lock l(mtx_);
        for (unsigned r = 0; r < regs && r < 32; ++r) {
            auto p = mesh_places_.find(TexLookSlotOf(pass, r));
            if (p != mesh_places_.end() && p->second != kMixed) mask |= 1u << r;
        }
    }
    t_memo[pass] = Memo{ mask, gen };
    return mask;
}

bool LookIndex::weaker(uint32_t target, uint32_t w, uint32_t h) const
{ // caller holds mtx_
    auto it = refs_.find(target);
    return it == refs_.end() || it->second.sig.pw * it->second.sig.ph < cells_of(w, h);
}

uint32_t LookIndex::target_of(uint32_t hash, bool is_target) const
{ // caller holds mtx_
    return is_target || refs_.count(hash) ? hash : TexLodKnown(hash);
}

bool LookIndex::offered(uint32_t hash) const
{ // caller holds mtx_
    auto it = verdicts_.find(hash);
    return it != verdicts_.end() && it->second.kind == Kind::Ref;
}

bool LookIndex::Wants(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target)
{
    if (!g_texlod_cache || !hash || !eligible(w, h, fmt)) return false;
    bool want = false;
    if (!TexLodDegenerate(hash)) {
        std::shared_lock l(mtx_);
        const uint32_t   high = target_of(hash, is_target);
        if (high)
            want = !offered(hash) && weaker(high, w, h);
        else {
            auto v = verdicts_.find(hash);
            want = v == verdicts_.end() || v->second.kind == Kind::Ref ||
                (v->second.kind == Kind::Match ? !TexLodLinked(hash) : v->second.gen != gen_);
        }
    }
    if (!want) skipped_.fetch_add(1, std::memory_order_relaxed);
    return want;
}

bool LookIndex::keep_ref(uint32_t target, Ref&& r)
{ // caller holds mtx_
    auto it = refs_.find(target);
    if (it != refs_.end() && it->second.sig.pw * it->second.sig.ph >= r.sig.pw * r.sig.ph) return false;
    wwmi_struct_tex::PublishSeq::Guard pub(TexStructMetaSeq());
    refs_[target] = std::move(r);
    ++gen_;
    mark_dirty();
    return true;
}

void LookIndex::offer(uint32_t source, uint32_t target, LookSig&& sig)
{
    Ref r;
    r.sig = std::move(sig);
    r.st = view_of(r.sig, r.sig.pw, r.sig.ph).st;
    const bool     flat = live_patches(r.st, r.sig.nch) < kLiveMin;
    const uint32_t w = r.sig.w;
    const uint32_t h = r.sig.h;
    bool           kept = false;
    {
        std::unique_lock l(mtx_);
        verdicts_[source] = Verdict{ gen_, Kind::Ref };
        mark_dirty();
        if (!flat) kept = keep_ref(target, std::move(r));
    }
    if (kept) refs_new_.fetch_add(1, std::memory_order_relaxed);
    if (g_debug && flat)
        logger::trace("TexLook: %08x %ux%u too flat to be the reference of %08x", source, w, h, target);
    else if (g_debug && kept)
        logger::trace("TexLook: %08x %ux%u is now the reference of %08x%s", source, w, h, target,
            source == target ? " (itself)" : ", a proven lower mip of it");
}

void LookIndex::match(uint32_t hash, const LookSig& sig)
{
    const LookView                         cv = view_of(sig, sig.pw, sig.ph);
    std::vector<std::pair<uint32_t, Score>> hits;
    uint32_t                               gen = 0;
    uint32_t                               closest_t = 0;
    Score                                  closest;
    {
        std::shared_lock l(mtx_);
        gen = gen_;
        for (const auto& kv : refs_) {
            if (kv.first == hash) continue;
            const Score s = compare(sig, cv, kv.second);
            if (s.match())
                hits.emplace_back(kv.first, s);
            else if (s.hist < closest.hist || (s.hist == closest.hist && s.ssim > closest.ssim)) {
                closest = s;
                closest_t = kv.first;
            }
        }
    }
    {
        std::unique_lock l(mtx_);
        verdicts_[hash] = Verdict{ gen, hits.empty() ? Kind::NoMatch : Kind::Match };
        mark_dirty();
    }
    for (const auto& h : hits) {
        if (TexLodLearnVisual(hash, h.first, h.second.ssim) && g_debug)
            logger::trace("TexLook: %08x %ux%u looks like %08x (hist %.3f, ssim-min %.3f)", hash, sig.w, sig.h, h.first,
                h.second.hist, h.second.ssim);
    }
    if (hits.size() > 1)
        logger::info("TexLook: %08x %ux%u looks like %zu targets, e.g. %08x (ssim-min %.3f) and %08x (%.3f)", hash, sig.w,
            sig.h, hits.size(), hits[0].first, hits[0].second.ssim, hits[1].first, hits[1].second.ssim);
    if (!hits.empty())
        matched_.fetch_add(1, std::memory_order_relaxed);
    else if (closest_t && DiagOnce("TEXLOOK-NOMATCH", (int)hash))
        logger::trace("TexLook: NO-MATCH %08x %ux%u | closest %08x hist %.3f ssim-min %.3f (need hist <= %.2f, ssim >= %.2f)",
            hash, sig.w, sig.h, closest_t, closest.hist, closest.ssim, kHistMax, kSsimMin);
}

void LookIndex::OnCut(uint32_t hash, const TexLookCut& cut, bool is_target)
{
    LookSig sig;
    if (!g_texlod_cache || !sig_of(cut, sig)) return;
    if (g_tex_struct_fallback) StoreLiveSig(hash, sig);
    decoded_.fetch_add(1, std::memory_order_relaxed);
    uint32_t high = 0;
    {
        std::shared_lock l(mtx_);
        high = target_of(hash, is_target);
    }
    if (high)
        offer(hash, high, std::move(sig));
    else
        match(hash, sig);
}

// The references (all a seed gives) and, from our own file, the verdicts. Caller holds mtx_.
LookIndex::LoadStats LookIndex::load(const std::wstring& file, bool seed)
{
    LoadStats stats;
    FILE* f = _wfsopen(file.c_str(), L"rb", _SH_DENYNO);
    if (!f) return stats;
    stats.opened = true;
    uint32_t magic = 0;
    uint32_t algo = 0;
    uint32_t gen = 0;
    uint32_t nrefs = 0;
    uint32_t nverdicts = 0;
    if (!get(f, magic) || !get(f, algo) || !get(f, gen) || !get(f, nrefs) || !get(f, nverdicts) ||
        (magic != kMagic && magic != kMagicPlaces && magic != kMagicMesh) || algo != kAlgo) {
        fclose(f);
        logger::write("TexLook: %S is not algorithm v%u (it says v%u) -- %s", file.c_str(), kAlgo, algo,
            seed ? "seed not taken" : "discarded, every texture is looked at again");
        return stats;
    }
    bool ok = true;
    for (uint32_t i = 0; ok && i < nrefs; ++i) {
        uint32_t target = 0;
        Ref      r;
        ok = get(f, target) && get(f, r.sig.codec) && get(f, r.sig.nch) && get(f, r.sig.w) && get(f, r.sig.h) &&
            get(f, r.sig.pw) && get(f, r.sig.ph);
        ok = ok && r.sig.nch && r.sig.nch <= 4 && r.sig.pw && r.sig.pw <= kMaxCells && r.sig.ph && r.sig.ph <= kMaxCells;
        if (!ok) break;
        r.sig.px.resize((size_t)kPatches * r.sig.pw * r.sig.ph * r.sig.nch);
        ok = fread(r.sig.px.data(), 1, r.sig.px.size(), f) == r.sig.px.size();
        if (!ok) break;
        r.st = view_of(r.sig, r.sig.pw, r.sig.ph).st;
        if (seed)
            stats.refs_taken += keep_ref(target, std::move(r)) ? 1 : 0;
        else {
            wwmi_struct_tex::PublishSeq::Guard pub(TexStructMetaSeq());
            refs_[target] = std::move(r);
            ++stats.refs_taken;
        }
    }
    for (uint32_t i = 0; ok && i < nverdicts; ++i) {
        uint32_t hash = 0;
        Verdict  v;
        uint8_t  kind = 0;
        ok = get(f, hash) && get(f, v.gen) && get(f, kind) && kind <= (uint8_t)Kind::Ref;
        v.kind = (Kind)kind;
        if (ok && !seed) verdicts_[hash] = v;
    }
    uint32_t nplaces = 0;
    if (ok && magic != kMagic) ok = get(f, nplaces);
    for (uint32_t i = 0; ok && i < nplaces; ++i) {
        uint64_t place = 0;
        uint32_t target = 0;
        ok = get(f, place) && get(f, target);
        if (ok && target) stats.places_taken += keep_place(places_, place, target, seed) ? 1 : 0;
    }
    uint32_t nmesh = 0;
    if (ok && magic == kMagicMesh) ok = get(f, nmesh);
    for (uint32_t i = 0; ok && i < nmesh; ++i) {
        uint64_t place = 0;
        uint32_t target = 0;
        ok = get(f, place) && get(f, target);
        if (ok && target) stats.mesh_places_taken += keep_place(mesh_places_, place, target, seed) ? 1 : 0;
    }
    if (!mesh_places_.empty()) has_mesh_.store(true, std::memory_order_relaxed);
    fclose(f);
    if (!seed) gen_ = (std::max)(gen_, gen);
    stats.ok = ok;
    if (!ok) logger::write("TexLook: %S ends early -- what came before is kept", file.c_str());
    return stats;
}

void LookIndex::write()
{ // caller holds mtx_
    const std::wstring p = path();
    if (p.empty()) return;
    const std::wstring tmp = p + L".tmp";
    FILE*              f = _wfsopen(tmp.c_str(), L"wb", _SH_DENYNO);
    if (!f) {
        logger::write("TexLook: cannot write %S", tmp.c_str());
        return;
    }
    bool ok = put(f, kMagicMesh) && put(f, kAlgo) && put(f, gen_) && put(f, (uint32_t)refs_.size()) &&
        put(f, (uint32_t)verdicts_.size());
    for (const auto& kv : refs_) {
        const LookSig& s = kv.second.sig;
        ok = ok && put(f, kv.first) && put(f, s.codec) && put(f, s.nch) && put(f, s.w) && put(f, s.h) && put(f, s.pw) &&
            put(f, s.ph) && fwrite(s.px.data(), 1, s.px.size(), f) == s.px.size();
    }
    for (const auto& kv : verdicts_) ok = ok && put(f, kv.first) && put(f, kv.second.gen) && put(f, (uint8_t)kv.second.kind);
    ok = ok && put(f, (uint32_t)places_.size());
    for (const auto& kv : places_) ok = ok && put(f, kv.first) && put(f, kv.second);
    ok = ok && put(f, (uint32_t)mesh_places_.size());
    for (const auto& kv : mesh_places_) ok = ok && put(f, kv.first) && put(f, kv.second);
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        logger::write("TexLook: cannot replace %S (err %lu)", p.c_str(), GetLastError());
        return;
    }
    dirty_ = false;
    logger::trace("TexLook: saved %zu reference(s), %zu verdict(s), %zu place(s), %zu mesh place(s); this session %u "
                  "decoded, %u skipped by their hash, %u matched, %u new reference(s), %u new place(s), %u look-alike(s) by "
                  "place",
        refs_.size(), verdicts_.size(), places_.size(), mesh_places_.size(), decoded_.load(), skipped_.load(),
        matched_.load(), refs_new_.load(), places_new_.load(), by_place_.load());
    for (const auto& seed : imported_) {
        if (MoveFileW(seed.first.c_str(), seed.second.c_str()))
            logger::write("TexLook: seed %S is in tex_look.bin now -- renamed %S", seed.first.c_str(), seed.second.c_str());
        else
            logger::write("TexLook: seed %S is in tex_look.bin now, but cannot be renamed (err %lu) -- it is taken again "
                          "next start",
                seed.first.c_str(), GetLastError());
    }
    imported_.clear();
    if (seed_ledger_dirty_) {
        if (wwmi_seed::SaveLedger(seed_state_path(), seed_ledger_))
            seed_ledger_dirty_ = false;
        else
            logger::write("TexLook: cannot save modular seed ledger %S", seed_state_path().c_str());
    }
}

void LookIndex::Init(const std::wstring& cache_dir)
{
    std::unique_lock l(mtx_);
    if (!cache_dir.empty()) dir_ = cache_dir;
    if (dir_.empty()) return;
    if (!loaded_) {
        loaded_ = true;
        const LoadStats st = load(path(), false);
        had_main_cache_ = st.opened && st.ok && (!refs_.empty() || !places_.empty() || !mesh_places_.empty());
        seed_ledger_ = wwmi_seed::LoadLedger(seed_state_path());
        logger::write("TexLook: %zu reference(s), %zu verdict(s), %zu place(s), %zu mesh place(s) from %S (algorithm v%u)",
            st.refs_taken, verdicts_.size(), places_.size(), mesh_places_.size(), path().c_str(), kAlgo);
    }

    // 1) Non-destructive modular seed directories (overlay in lexicographical order without renaming sources).
    const auto seed_dirs = wwmi_seed::CollectSeedDirs(dir_, g_data_dir);
    const auto mod_seeds = wwmi_seed::EnumerateModularSeeds(seed_dirs, L".texlook");
    size_t     mod_applied = 0;
    size_t     mod_skipped = 0;
    for (const auto& sf : mod_seeds) {
        auto it = seed_ledger_.find(sf.norm_key);
        if (had_main_cache_ && it != seed_ledger_.end() && it->second == sf.stamp) {
            ++mod_skipped;
            logger::info("TexLook: modular seed %S unchanged (size=%llu) -- skipped",
                sf.path.c_str(), static_cast<unsigned long long>(sf.size));
            continue;
        }
        const LoadStats st = load(sf.path, true);
        if (!st.opened) continue;
        ++mod_applied;
        const size_t total_delta = st.refs_taken + st.places_taken + st.mesh_places_taken;
        if (total_delta > 0) mark_dirty();
        if (st.ok) {
            seed_ledger_[sf.norm_key] = sf.stamp;
            seed_ledger_dirty_ = true;
        }
        logger::write("TexLook: modular seed %S -- +%zu ref(s), +%zu place(s), +%zu mesh place(s) (size=%llu%s)",
            sf.path.c_str(), st.refs_taken, st.places_taken, st.mesh_places_taken,
            static_cast<unsigned long long>(sf.size), st.ok ? "" : ", truncated");
    }
    if (!seed_dirs.empty()) {
        logger::write("TexLook: modular seeds -- %zu dir(s), %zu applied, %zu unchanged skipped (live: %zu ref(s), %zu place(s), %zu mesh place(s))",
            seed_dirs.size(), mod_applied, mod_skipped, refs_.size(), places_.size(), mesh_places_.size());
    }

    // 2) Legacy cache-root drop-in seeds (*.texlook directly in cache_dir, renamed DISABLED_* after write).
    WIN32_FIND_DATAW sf;
    HANDLE           hs = FindFirstFileW((dir_ + L"\\*.texlook").c_str(), &sf);
    if (hs != INVALID_HANDLE_VALUE) {
        do {
            if (sf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const size_t n = wcslen(sf.cFileName);
            if (n < 8 || _wcsicmp(sf.cFileName + n - 8, L".texlook") != 0 || _wcsnicmp(sf.cFileName, L"DISABLED_", 9) == 0)
                continue;
            const std::wstring from = dir_ + L"\\" + sf.cFileName;
            if (std::any_of(imported_.begin(), imported_.end(), [&](const auto& s) { return s.first == from; })) continue;
            const LoadStats st = load(from, true);
            SYSTEMTIME      tm;
            GetLocalTime(&tm);
            wchar_t when[32];
            swprintf_s(when, L"%04u%02u%02u-%02u%02u%02u", tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond);
            imported_.emplace_back(from, dir_ + L"\\DISABLED_" + when + L"_" + sf.cFileName);
            mark_dirty();
            logger::write("TexLook: seed %S -- +%zu ref(s), +%zu place(s), +%zu mesh place(s)",
                from.c_str(), st.refs_taken, st.places_taken, st.mesh_places_taken);
        } while (FindNextFileW(hs, &sf));
        FindClose(hs);
    }

    if (seed_ledger_dirty_ && !dirty_) {
        if (wwmi_seed::SaveLedger(seed_state_path(), seed_ledger_)) seed_ledger_dirty_ = false;
    }
}

void LookIndex::FlushIfQuiet(unsigned quiet_ms)
{
    std::unique_lock l(mtx_);
    if (dirty_ && GetTickCount64() - dirty_at_ >= quiet_ms) write();
}

void LookIndex::OnExit()
{
    std::unique_lock l(mtx_);
    if (dirty_) write();
    logger::write("TexLook: exit -- %zu reference(s), %zu verdict(s), %zu place(s), %zu mesh place(s); this session %u "
                  "decoded, %u skipped by their hash, %u matched, %u new reference(s), %u new place(s), %u look-alike(s) by "
                  "place",
        refs_.size(), verdicts_.size(), places_.size(), mesh_places_.size(), decoded_.load(), skipped_.load(),
        matched_.load(), refs_new_.load(), places_new_.load(), by_place_.load());
}

LookIndex g_look;

} // namespace

void TexLookInit(const std::wstring& cache_dir)
{
    g_look.Init(cache_dir);
}
void TexLookFlushIfQuiet(unsigned quiet_ms)
{
    g_look.FlushIfQuiet(quiet_ms);
}
void TexLookOnExit()
{
    g_look.OnExit();
}
bool TexLookWants(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target)
{
    return g_look.Wants(hash, w, h, fmt, is_target);
}
void TexLookOnCut(uint32_t hash, const TexLookCut& cut, bool is_target)
{
    g_look.OnCut(hash, cut, is_target);
}
uint64_t TexLookPlaceOf(const wchar_t* ns, int comp, uint64_t ps)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const wchar_t* c = ns; c && *c; ++c) h = (h ^ (uint64_t)towlower(*c)) * 0x100000001b3ull;
    h = (h ^ (uint64_t)(uint32_t)comp) * 0x100000001b3ull;
    return (h ^ ps) * 0x100000001b3ull;
}
void TexLookAtPlace(uint64_t pass, uint32_t slot, uint32_t hash, uint32_t target)
{
    g_look.AtPlace(TexLookSlotOf(pass, slot), hash, target);
}
uint64_t TexLookMeshPassOf(uint32_t ib_hash, uint32_t first_index, uint32_t index_count, uint64_t ps)
{
    uint64_t h = 0x84222325cbf29ce4ull;
    for (const uint32_t v : { ib_hash, first_index, index_count }) h = (h ^ v) * 0x100000001b3ull;
    return (h ^ ps) * 0x100000001b3ull;
}
uint64_t TexLookSlotOf(uint64_t pass, uint32_t slot)
{
    return (pass ^ slot) * 0x100000001b3ull;
}
bool TexLookHasMeshPlaces()
{
    return g_look.HasMeshPlaces();
}
uint32_t TexLookMeshMask(uint64_t pass, unsigned regs)
{
    return g_look.MeshMask(pass, regs);
}
void TexLookAtMeshPlace(uint64_t place, uint32_t hash, uint32_t target)
{
    g_look.AtMeshPlace(place, hash, target);
}
void TexLookAddMeshPlace(uint64_t place, uint32_t target)
{
    g_look.AddMeshPlace(place, target);
}

TexLookCut TexLookCutOf(const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, uint32_t fmt)
{
    TexLookCut cut;
    if (!bc || !eligible(w, h, fmt)) return cut;
    const uint32_t bb = DxgiBcBlockBytes(fmt);
    const size_t   row = (size_t)(w / 4) * bb;
    if (bc_size < row * (h / 4)) return cut;
    const Geometry g = geometry_of(w, h);
    const size_t   run = (size_t)(g.sw / 4) * bb;
    cut.blocks.resize(kPatches * (size_t)(g.sh / 4) * run);
    uint8_t* d = cut.blocks.data();
    for (const Corner& p : g.at)
        for (uint32_t r = 0; r < g.sh / 4; ++r, d += run) memcpy(d, bc + (size_t)(p.y / 4 + r) * row + (size_t)(p.x / 4) * bb, run);
    cut.fmt = fmt;
    cut.w = w;
    cut.h = h;
    return cut;
}

bool TexLookHasRef(uint32_t target)
{
    return g_look.HasRef(target);
}

bool TexLookHasAnySig(uint32_t key)
{
    return g_look.HasAnySig(key);
}

bool TexLookWantsLiveSig(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt)
{
    return g_look.WantsLiveSig(hash, w, h, fmt);
}

void TexLookCacheLiveCut(uint32_t hash, const TexLookCut& cut)
{
    g_look.CacheLiveCut(hash, cut);
}

bool TexLookCopyRefSig(uint32_t target, uint8_t* codec, uint8_t* nch, uint32_t* w, uint32_t* h,
    uint32_t* pw, uint32_t* ph, std::vector<uint8_t>* px)
{
    return g_look.CopyRefSig(target, codec, nch, w, h, pw, ph, px);
}

bool TexLookCopyAnySig(uint32_t key, uint8_t* codec, uint8_t* nch, uint32_t* w, uint32_t* h,
    uint32_t* pw, uint32_t* ph, std::vector<uint8_t>* px)
{
    return g_look.CopyAnySig(key, codec, nch, w, h, pw, ph, px);
}

// Stream only the 5 TexLook patches directly from a DDS file on disk (e.g., ~1.3 MB out of a 67 MB 8K DDS)
// and decode them with bcdec into a 5-patch LookSig + RGB channel statistics. Safe to call off the render thread.
bool TexLookComputeDdsSig(const wchar_t* dds_path, uint32_t* out_w, uint32_t* out_h, uint32_t* out_fmt,
    uint8_t* out_codec, uint8_t* out_nch, uint32_t* out_pw, uint32_t* out_ph, std::vector<uint8_t>* out_px,
    double out_rgb_mean[3], double out_rgb_std[3])
{
    if (!dds_path || !dds_path[0]) return false;
    FILE* f = _wfsopen(dds_path, L"rb", _SH_DENYNO);
    if (!f) return false;
    uint8_t      hdr[148]{};
    const size_t n = fread(hdr, 1, sizeof(hdr), f);
    if (n < 128 || std::memcmp(hdr, "DDS ", 4) != 0) {
        fclose(f);
        return false;
    }
    auto u32 = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        std::memcpy(&v, hdr + off, 4);
        return v;
    };
    const uint32_t h = u32(12);
    const uint32_t w = u32(16);
    const uint32_t pf_flags = u32(80);
    const uint32_t fourcc = u32(84);
    uint32_t       fmt = 0;
    uint64_t       data_off = 128;
    if ((pf_flags & 0x4u) != 0) {
        if (std::memcmp(&fourcc, "DX10", 4) == 0 && n >= 148) {
            fmt = u32(128);
            data_off = 148;
        } else if (std::memcmp(&fourcc, "DXT1", 4) == 0) {
            fmt = 71;
        } else if (std::memcmp(&fourcc, "DXT5", 4) == 0) {
            fmt = 77;
        } else if (std::memcmp(&fourcc, "ATI1", 4) == 0 || std::memcmp(&fourcc, "BC4U", 4) == 0) {
            fmt = 80;
        } else if (std::memcmp(&fourcc, "ATI2", 4) == 0 || std::memcmp(&fourcc, "BC5U", 4) == 0) {
            fmt = 83;
        }
    }
    if (!eligible(w, h, fmt)) {
        fclose(f);
        return false;
    }
    const Codec    c = codec_of(fmt);
    const Geometry g = geometry_of(w, h);
    const uint32_t bb = DxgiBcBlockBytes(fmt);
    const uint64_t row_bytes = static_cast<uint64_t>(w / 4) * bb;
    const uint32_t nbx = g.sw / 4;
    const uint32_t nby = g.sh / 4;
    const size_t   run_bytes = static_cast<size_t>(nbx) * bb;

    std::vector<uint8_t> row_buf(run_bytes);
    const size_t         cells = static_cast<size_t>(g.pw) * g.ph * c.nch;
    std::vector<uint8_t> px(kPatches * cells, 0);
    std::vector<uint32_t> sum(cells);
    const double         per = static_cast<double>(g.fx) * g.fy;
    double               g_sum[3] = { 0.0, 0.0, 0.0 };
    double               g_ss[3] = { 0.0, 0.0, 0.0 };
    double               g_n = 0.0;

    for (int k = 0; k < kPatches; ++k) {
        std::fill(sum.begin(), sum.end(), 0u);
        const Corner& p = g.at[k];
        for (uint32_t by = 0; by < nby; ++by) {
            const uint64_t file_off = data_off + static_cast<uint64_t>(p.y / 4 + by) * row_bytes +
                static_cast<uint64_t>(p.x / 4) * bb;
            if (_fseeki64(f, static_cast<__int64>(file_off), SEEK_SET) != 0 ||
                fread(row_buf.data(), 1, run_bytes, f) != run_bytes) {
                fclose(f);
                return false;
            }
            const uint8_t* blk = row_buf.data();
            for (uint32_t bx = 0; bx < nbx; ++bx, blk += bb) {
                uint8_t t[64];
                decode_block(c.id, blk, t);
                for (uint32_t i = 0; i < 16; ++i) {
                    const uint32_t tx = bx * 4 + (i & 3);
                    const uint32_t ty = by * 4 + (i >> 2);
                    uint32_t*      cell = &sum[(static_cast<size_t>(ty / g.fy) * g.pw + tx / g.fx) * c.nch];
                    for (uint32_t ch = 0; ch < c.nch; ++ch) cell[ch] += t[i * 4 + ch];
                    for (uint32_t ch = 0; ch < 3 && ch < c.nch; ++ch) {
                        const double v = static_cast<double>(t[i * 4 + ch]);
                        g_sum[ch] += v;
                        g_ss[ch] += v * v;
                    }
                    g_n += 1.0;
                }
            }
        }
        uint8_t* out = &px[k * cells];
        for (size_t i = 0; i < cells; ++i) out[i] = static_cast<uint8_t>(std::nearbyint(sum[i] / per));
    }
    fclose(f);

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_fmt) *out_fmt = fmt;
    if (out_codec) *out_codec = c.id;
    if (out_nch) *out_nch = c.nch;
    if (out_pw) *out_pw = g.pw;
    if (out_ph) *out_ph = g.ph;
    if (out_px) *out_px = std::move(px);
    for (int ch = 0; ch < 3; ++ch) {
        const double m = g_n > 0.0 ? g_sum[ch] / g_n : 0.0;
        const double var = g_n > 0.0 ? g_ss[ch] / g_n - m * m : 0.0;
        if (out_rgb_mean) out_rgb_mean[ch] = m;
        if (out_rgb_std) out_rgb_std[ch] = std::sqrt(var > 0.0 ? var : 0.0);
    }
    return true;
}

void TexLookCheckCutoutMask(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, const uint8_t* bc, size_t bc_bytes)
{
    g_look.CheckCutoutMask(hash, w, h, fmt, bc, bc_bytes);
}

bool TexLookIsCutoutMask(uint32_t hash)
{
    return g_look.IsCutoutMask(hash);
}

void TexLookMarkCutoutMask(uint32_t hash)
{
    g_look.MarkCutoutMask(hash);
}

// Fast strided-grid check (<=16 KB I/O even on an 8K DDS) verifying whether a DDS texture is
// 100% pure opaque white suitable for neutralizing binary cutout masks. Off-render-thread safe.
bool TexLookIsDdsPureWhite(const wchar_t* dds_path)
{
    if (!dds_path || !dds_path[0]) return false;
    FILE* f = _wfsopen(dds_path, L"rb", _SH_DENYNO);
    if (!f) return false;
    uint8_t      hdr[148]{};
    const size_t n = fread(hdr, 1, sizeof(hdr), f);
    if (n < 128 || std::memcmp(hdr, "DDS ", 4) != 0) {
        fclose(f);
        return false;
    }
    auto u32 = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        std::memcpy(&v, hdr + off, 4);
        return v;
    };
    const uint32_t h = u32(12);
    const uint32_t w = u32(16);
    const uint32_t pf_flags = u32(80);
    const uint32_t fourcc = u32(84);
    const uint32_t rgb_bits = u32(88);
    const uint32_t rmask = u32(92);
    const uint32_t gmask = u32(96);
    const uint32_t bmask = u32(100);
    const uint32_t amask = u32(104);
    uint32_t       fmt = 0;
    uint64_t       data_off = 128;
    if ((pf_flags & 0x4u) != 0) {
        if (std::memcmp(&fourcc, "DX10", 4) == 0 && n >= 148) {
            fmt = u32(128);
            data_off = 148;
        } else if (std::memcmp(&fourcc, "DXT1", 4) == 0) {
            fmt = 71;
        } else if (std::memcmp(&fourcc, "DXT5", 4) == 0) {
            fmt = 77;
        }
    } else if (rgb_bits == 32) {
        if (rmask == 0x00ff0000u && gmask == 0x0000ff00u && bmask == 0x000000ffu)
            fmt = amask ? 87u : 88u;
        else if (rmask == 0x000000ffu && gmask == 0x0000ff00u && bmask == 0x00ff0000u)
            fmt = 28u;
    }
    if (w < 256 || h < 256) {
        fclose(f);
        return false;
    }

    wwmi_struct_tex::CutoutMaskStats st{};
    const Codec                      c = codec_of(fmt);
    if (c.id == 1 || c.id == 3 || c.id == 7) {
        const uint32_t bb = DxgiBcBlockBytes(fmt);
        const uint32_t nbx = w / 4;
        const uint32_t nby = h / 4;
        if (!bb || !nbx || !nby) {
            fclose(f);
            return false;
        }
        const uint64_t     row_bytes = static_cast<uint64_t>(nbx) * bb;
        constexpr uint32_t kGrid = 32;
        uint8_t            blk[16]{};
        for (uint32_t gy = 0; gy < kGrid; ++gy) {
            const uint32_t by = ((gy * 2 + 1) * nby) / (kGrid * 2);
            for (uint32_t gx = 0; gx < kGrid; ++gx) {
                const uint32_t bx = ((gx * 2 + 1) * nbx) / (kGrid * 2);
                const uint64_t off = data_off + static_cast<uint64_t>(by) * row_bytes + static_cast<uint64_t>(bx) * bb;
                if (_fseeki64(f, static_cast<__int64>(off), SEEK_SET) != 0 || fread(blk, 1, bb, f) != bb) {
                    fclose(f);
                    return false;
                }
                uint8_t rgba[64];
                decode_block(c.id, blk, rgba);
                for (int i = 0; i < 16; ++i)
                    wwmi_struct_tex::AccumulateCutoutTexel(st, rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]);
                if (st.black > 0 || (st.total - st.white) > 40) {
                    fclose(f);
                    return false;
                }
            }
        }
        fclose(f);
        return wwmi_struct_tex::IsPureWhiteNeutralizer(st);
    }

    if (fmt == 28 || fmt == 29 || fmt == 87 || fmt == 91) {
        const bool         bgra = (fmt == 87 || fmt == 91);
        const uint64_t     row_bytes = static_cast<uint64_t>(w) * 4;
        constexpr uint32_t kGrid = 32;
        uint8_t            px[4]{};
        for (uint32_t gy = 0; gy < kGrid; ++gy) {
            const uint32_t py = ((gy * 2 + 1) * h) / (kGrid * 2);
            for (uint32_t gx = 0; gx < kGrid; ++gx) {
                const uint32_t px_x = ((gx * 2 + 1) * w) / (kGrid * 2);
                const uint64_t off = data_off + static_cast<uint64_t>(py) * row_bytes + static_cast<uint64_t>(px_x) * 4;
                if (_fseeki64(f, static_cast<__int64>(off), SEEK_SET) != 0 || fread(px, 1, 4, f) != 4) {
                    fclose(f);
                    return false;
                }
                const uint8_t r = bgra ? px[2] : px[0];
                const uint8_t g = px[1];
                const uint8_t b = bgra ? px[0] : px[2];
                const uint8_t a = px[3];
                wwmi_struct_tex::AccumulateCutoutTexel(st, r, g, b, a);
                if (st.black > 0 || (st.total - st.white) > 4) {
                    fclose(f);
                    return false;
                }
            }
        }
        fclose(f);
        return wwmi_struct_tex::IsPureWhiteNeutralizer(st);
    }

    fclose(f);
    return false;
}
