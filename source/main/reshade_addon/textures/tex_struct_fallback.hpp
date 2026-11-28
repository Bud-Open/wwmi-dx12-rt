#pragma once
// ===========================================================================
//  tex_struct_fallback.hpp -- Layer 3 zero-touch runtime auto-adaptation
//  engine for classic hash-based WWMI mods.
//
//  Combines two complementary, 100% generic mechanisms (ZERO per-character,
//  per-GUID, per-name, or per-hash hardcoding; ZERO edits to user mod files):
//
//    1) Fast Path (Frame 1, 0ms): Unambiguous 1-to-1 (Component, PS Slot,
//       DXGI_FORMAT) structural fallback for exclusive single-component slots.
//    2) Background Zero-Touch Visual Auto-Adapter (Off-Render Thread + Disk
//       Persistence): Whenever a classic hash mod draws a component slot whose
//       live texture has a canonical TexLook seed reference (`lod = TexLodTargetOf`),
//       a low-priority worker streams only the 5 TexLook patches (~1.3 MB per 8K
//       DDS via `TexLookComputeDdsSig`), computes 5-patch RGB SSIM + 32-bin
//       histogram similarity against canonical HD seed references, runs exact
//       1-to-1 maximum-weight bipartite assignment with twin-variant consistency
//       checks (plus BC3->BC7 author re-save tolerance), publishes resolved
//       bindings transparently on the very next frame, and persists them to
//       `<cache_dir>\mod_tex_adapt.cache` for instant frame-1 hits across
//       restarts and F10 reloads.
// ===========================================================================
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tex_struct_solver.hpp"
#include "tex_struct_recipe_cache.hpp"

extern bool                  g_tex_struct_fallback;
extern std::atomic<uint64_t> g_layout_regmap_epoch;
extern std::atomic<uint64_t> g_struct_fb_cache_hits;
extern std::atomic<uint64_t> g_struct_fb_cache_misses;
bool ReadPublishedDescriptorAndFormat(const HeapKey& key, ResInfo& out, DXGI_FORMAT& out_fmt, uint64_t* out_res_gen = nullptr) noexcept;

// Implemented in source/main/tex_look.cpp (no ABI change to tex_look.h).
bool TexLookHasRef(uint32_t target);
bool TexLookHasAnySig(uint32_t key);
bool TexLookCopyRefSig(uint32_t target, uint8_t* codec, uint8_t* nch, uint32_t* w, uint32_t* h,
    uint32_t* pw, uint32_t* ph, std::vector<uint8_t>* px);
bool TexLookCopyAnySig(uint32_t key, uint8_t* codec, uint8_t* nch, uint32_t* w, uint32_t* h,
    uint32_t* pw, uint32_t* ph, std::vector<uint8_t>* px);
bool TexLookComputeDdsSig(const wchar_t* dds_path, uint32_t* out_w, uint32_t* out_h, uint32_t* out_fmt,
    uint8_t* out_codec, uint8_t* out_nch, uint32_t* out_pw, uint32_t* out_ph, std::vector<uint8_t>* out_px,
    double out_rgb_mean[3], double out_rgb_std[3]);
bool TexLookIsCutoutMask(uint32_t hash);
void TexLookMarkCutoutMask(uint32_t hash);
bool TexLookIsDdsPureWhite(const wchar_t* dds_path);
uint32_t TexVisualParentOf(uint32_t hash);
void TexNoteVisualMipParent(uint32_t child, uint32_t parent);
std::vector<std::pair<uint32_t, uint32_t>> TexCopyVisualMipParentsFor(const std::unordered_set<uint32_t>& parents);

namespace wwmi_struct_tex {

struct ExtraOverrideDds {
    std::wstring          fq;
    std::wstring          name;
    std::wstring          filename;
    uint32_t              dxgi_fmt = 0;
    uint32_t              width = 0;
    uint32_t              height = 0;
    uint64_t              file_size = 0;
    std::vector<uint32_t> override_hashes;
};

struct ModTexCandidate {
    std::wstring     fq;
    std::wstring     name;
    std::wstring     filename;
    std::vector<int> comps;
    bool             exclusive = false;
    uint32_t         dxgi_fmt = 0;
    uint32_t         width = 0;
    uint32_t         height = 0;
    uint32_t         file_hash = 0;
    uint64_t         file_size = 0;
};

struct ModStructIndex {
    uint64_t                                                   fingerprint = 0;
    uint32_t                                                   object_guid = 0;
    std::vector<ModTexCandidate>                               all_dds;            // all valid square >=256 DDS for visual adapter
    std::vector<ExtraOverrideDds>                              extra_override_dds; // [TextureOverride]-backed DDS in mod dir (incl. sibling .ini)
    std::unordered_map<uint64_t, std::vector<size_t>>          excl_by_comp_fmt;   // (comp, dxgi_fmt) -> indices in all_dds
    std::unordered_map<int, std::vector<size_t>>               by_comp;            // comp -> indices in all_dds (exclusive + shared)
};

struct FallbackResolvedSlot {
    unsigned                               reg = 0;
    uint8_t                                param = 0xFF;
    uint32_t                               offset = 0;
    uint32_t                               vfmt = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE            srv{ 0 };
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
};

struct VisualResolvedEntry {
    std::wstring fq;
    std::wstring name;
    float        score = 0.f;
    std::string  tier;
    uint32_t     source_lod = 0;
};

inline bool ReadDdsHeaderMeta(const std::wstring& path, uint32_t& width, uint32_t& height, uint32_t& dxgi_fmt, uint64_t* out_size = nullptr)
{
    width = height = dxgi_fmt = 0;
    if (out_size) *out_size = 0;
    if (path.empty()) return false;
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return false;
    if (out_size) {
        if (_fseeki64(f, 0, SEEK_END) == 0) {
            const __int64 sz = _ftelli64(f);
            if (sz > 0) *out_size = static_cast<uint64_t>(sz);
            _fseeki64(f, 0, SEEK_SET);
        }
    }
    uint8_t      buf[148]{};
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < 128 || std::memcmp(buf, "DDS ", 4) != 0) return false;

    auto u32 = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        std::memcpy(&v, buf + off, 4);
        return v;
    };
    height = u32(12);
    width = u32(16);
    const uint32_t pf_flags = u32(80);
    const uint32_t fourcc = u32(84);
    const uint32_t rgb_bits = u32(88);
    const uint32_t rmask = u32(92);
    const uint32_t gmask = u32(96);
    const uint32_t bmask = u32(100);
    const uint32_t amask = u32(104);

    if ((pf_flags & 0x4u) != 0) {
        if (std::memcmp(&fourcc, "DX10", 4) == 0 && n >= 148) {
            dxgi_fmt = u32(128);
        } else if (std::memcmp(&fourcc, "DXT1", 4) == 0) {
            dxgi_fmt = 71;
        } else if (std::memcmp(&fourcc, "DXT3", 4) == 0) {
            dxgi_fmt = 74;
        } else if (std::memcmp(&fourcc, "DXT5", 4) == 0) {
            dxgi_fmt = 77;
        } else if (std::memcmp(&fourcc, "ATI1", 4) == 0 || std::memcmp(&fourcc, "BC4U", 4) == 0) {
            dxgi_fmt = 80;
        } else if (std::memcmp(&fourcc, "ATI2", 4) == 0 || std::memcmp(&fourcc, "BC5U", 4) == 0) {
            dxgi_fmt = 83;
        }
    } else if (rgb_bits == 32) {
        if (rmask == 0x00ff0000u && gmask == 0x0000ff00u && bmask == 0x000000ffu)
            dxgi_fmt = amask ? 87u : 88u;
        else if (rmask == 0x000000ffu && gmask == 0x0000ff00u && bmask == 0x00ff0000u)
            dxgi_fmt = 28u;
    } else if (rgb_bits == 8) {
        dxgi_fmt = 61u;
    }
    return width > 0 && height > 0 && dxgi_fmt != 0;
}

inline bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix)
{
    const size_t sl = wcslen(suffix);
    return s.size() >= sl && _wcsicmp(s.c_str() + (s.size() - sl), suffix) == 0;
}

inline bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix)
{
    const size_t pl = wcslen(prefix);
    return s.size() >= pl && _wcsnicmp(s.c_str(), prefix, pl) == 0;
}

inline std::wstring LowerStr(std::wstring s)
{
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

inline std::wstring NormalizePathLower(std::wstring p)
{
    for (wchar_t& c : p) {
        if (c == L'/') c = L'\\';
        c = static_cast<wchar_t>(towlower(c));
    }
    return p;
}

inline std::wstring NormalizeDirPrefix(std::wstring p)
{
    p = NormalizePathLower(std::move(p));
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    if (!p.empty()) p.push_back(L'\\');
    return p;
}

inline bool IsBcRgbaFormat(uint32_t fmt) noexcept
{
    return fmt == 71 || fmt == 72 || fmt == 77 || fmt == 78 || fmt == 98 || fmt == 99;
}

inline bool IsNeutralizerDdsFormat(uint32_t fmt) noexcept
{
    return IsBcRgbaFormat(fmt) || fmt == 28 || fmt == 29 || fmt == 87 || fmt == 91;
}

inline std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline uint64_t FnvMixStr(uint64_t h, const std::wstring& s)
{
    for (wchar_t c : s) {
        h ^= static_cast<uint64_t>(towlower(c));
        h *= 0x100000001b3ull;
    }
    return h;
}

inline uint64_t FnvMixU64(uint64_t h, uint64_t v)
{
    h ^= v;
    h *= 0x100000001b3ull;
    return h;
}

inline std::vector<int> ParseFilenameComponents(const std::wstring& filename)
{
    std::vector<int>   comps;
    const size_t       slash = filename.find_last_of(L"\\/");
    const std::wstring low = LowerStr((slash == std::wstring::npos) ? filename : filename.substr(slash + 1));

    size_t pos = low.find(L"components-");
    if (pos != std::wstring::npos) {
        pos += 11;
        while (pos < low.size()) {
            if (!iswdigit(low[pos])) break;
            int val = 0;
            while (pos < low.size() && iswdigit(low[pos])) {
                val = val * 10 + (low[pos] - L'0');
                ++pos;
            }
            if (std::find(comps.begin(), comps.end(), val) == comps.end()) comps.push_back(val);
            if (pos < low.size() && (low[pos] == L'-' || low[pos] == L'_')) {
                ++pos;
                continue;
            }
            break;
        }
        return comps;
    }

    pos = low.find(L"component");
    if (pos != std::wstring::npos) {
        pos += 9;
        if (pos < low.size() && (low[pos] == L'-' || low[pos] == L'_')) ++pos;
        if (pos < low.size() && iswdigit(low[pos])) {
            int val = 0;
            while (pos < low.size() && iswdigit(low[pos])) {
                val = val * 10 + (low[pos] - L'0');
                ++pos;
            }
            comps.push_back(val);
        }
        return comps;
    }

    // Generic shorthand `comp<N>` (e.g. `comp3_a1b2c3d4.dds`, `comp-3-5_...`)
    size_t scan = 0;
    while ((pos = low.find(L"comp", scan)) != std::wstring::npos) {
        scan = pos + 4;
        if (pos > 0 && iswalnum(low[pos - 1])) continue;
        size_t p = pos + 4;
        if (p < low.size() && (low[p] == L'-' || low[p] == L'_')) ++p;
        if (p >= low.size() || !iswdigit(low[p])) continue;
        while (p < low.size() && iswdigit(low[p])) {
            int val = 0;
            while (p < low.size() && iswdigit(low[p])) {
                val = val * 10 + (low[p] - L'0');
                ++p;
            }
            if (std::find(comps.begin(), comps.end(), val) == comps.end()) comps.push_back(val);
            if (p + 1 < low.size() && (low[p] == L'-' || low[p] == L'_') && iswdigit(low[p + 1])) {
                // Only continue chaining short component indices (<100), not an 8-hex texture hash suffix
                size_t q = p + 1;
                while (q < low.size() && iswxdigit(low[q])) ++q;
                if (q - (p + 1) >= 8) break;
                ++p;
                continue;
            }
            break;
        }
        if (!comps.empty()) break;
    }
    return comps;
}

inline uint32_t ParseFilenameHash(const std::wstring& filename)
{
    const size_t       slash = filename.find_last_of(L"\\/");
    const std::wstring base = (slash == std::wstring::npos) ? filename : filename.substr(slash + 1);
    for (size_t i = 0; i + 10 <= base.size(); ++i) {
        if ((base[i] == L't' || base[i] == L'T') && base[i + 1] == L'=') {
            if (i > 0 && iswalnum(base[i - 1])) continue;
            bool hex_ok = true;
            for (size_t k = 0; k < 8; ++k) {
                if (!iswxdigit(base[i + 2 + k])) {
                    hex_ok = false;
                    break;
                }
            }
            if (hex_ok && (i + 10 == base.size() || !iswxdigit(base[i + 10]))) {
                wchar_t tmp[9]{};
                std::memcpy(tmp, base.c_str() + i + 2, 8 * sizeof(wchar_t));
                return static_cast<uint32_t>(wcstoul(tmp, nullptr, 16));
            }
        }
    }
    // Generic fallback: trailing `<sep><8hex>.dds` (e.g. `comp3_a1b2c3d4.dds`)
    if (base.size() >= 12 && EndsWithNoCase(base, L".dds")) {
        const size_t start = base.size() - 12;
        if (start == 0 || !iswalnum(base[start - 1])) {
            bool hex_ok = true;
            bool has_digit = false;
            for (size_t k = 0; k < 8; ++k) {
                const wchar_t c = base[start + k];
                if (!iswxdigit(c)) {
                    hex_ok = false;
                    break;
                }
                if (iswdigit(c)) has_digit = true;
            }
            if (hex_ok && has_digit) {
                wchar_t tmp[9]{};
                std::memcpy(tmp, base.c_str() + start, 8 * sizeof(wchar_t));
                return static_cast<uint32_t>(wcstoul(tmp, nullptr, 16));
            }
        }
    }
    return 0;
}

inline uint64_t CompFmtKey(int comp, uint32_t fmt)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(comp)) << 32) | fmt;
}

inline bool IsPow2(uint32_t v) noexcept
{
    return v != 0 && (v & (v - 1)) == 0;
}

inline bool IsMaterialTexDims(uint32_t w, uint32_t h) noexcept
{
    if (w < 256 || h < 256) return false;
    if (w == h) return true; // square textures (including 6144x6144 mod atlases)
    // Non-square: require power-of-two axes with exact 2:1 / 1:2 aspect (e.g. 4096x2048, 8192x4096),
    // cleanly excluding viewport render targets like 1920x1080 / 2560x1440 / 3840x2160.
    return IsPow2(w) && IsPow2(h) && (w == h * 2 || h == w * 2);
}

// Same-color-space cross-BC author re-save compatibility (e.g. native BC1_UNORM/BC3_UNORM
// Material ID or lightmap re-exported by a mod author as BC7_UNORM, or BC1/BC3_SRGB -> BC7_SRGB).
// Never crosses Linear <-> sRGB or single/two-channel formats (BC4/BC5/R8).
inline bool CrossBcResaveCompatible(uint32_t live_vfmt, uint32_t mod_fmt) noexcept
{
    if (live_vfmt == mod_fmt) return false;
    const auto is_linear_bc_rgba = [](uint32_t f) { return f == 71 || f == 77 || f == 98; };
    const auto is_srgb_bc_rgba = [](uint32_t f) { return f == 72 || f == 78 || f == 99; };
    if (is_linear_bc_rgba(live_vfmt) && is_linear_bc_rgba(mod_fmt)) return true;
    if (is_srgb_bc_rgba(live_vfmt) && is_srgb_bc_rgba(mod_fmt)) return true;
    return false;
}

class StructFallbackEngine {
public:
    ~StructFallbackEngine()
    {
        Stop();
    }

    void Stop()
    {
        {
            std::lock_guard l(worker_mtx_);
            stop_.store(true, std::memory_order_release);
        }
        worker_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        FlushDiskCache();
    }

    void Apply(reshade::api::command_list* list, int dm, int comp, uint32_t rtv, uint64_t ps_hash, bool hybrid,
        std::vector<ShadowSlotOvr>& out_ps, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& out_textures,
        std::vector<uint64_t>& out_owners, std::vector<FallbackResolvedSlot>* out_cache)
    {
        if (!g_tex_struct_fallback || !list || dm < 0 || dm >= int(g_geomods.size()) || comp < 0 || rtv < 2) return;
        if (g_geomods[dm].mod.queries_ps_slots) return;

        const unsigned    load = PortApi::LoadGeneration();
        const auto&       gs = CmdGfx(list);
        const DxilSrvRegs ps_regs = PipelineSrvRegs(gs.gfx_pso, 'p');
        const uint64_t    layh = CmdGfxLayout(list);
        RegBind           t_ps[8]{};
        {
            static thread_local ThreadLayoutPsCache<RegBind> t_layout_cache;
            const uint64_t cur_epoch = g_layout_regmap_epoch.load(std::memory_order_acquire);
            bool           built = false;
            if (!t_layout_cache.find(layh, cur_epoch, built, t_ps)) {
                uint64_t locked_epoch = 0;
                {
                    std::lock_guard l(g_layout_regmap_mtx);
                    locked_epoch = g_layout_regmap_epoch.load(std::memory_order_relaxed);
                    auto it = g_layout_regmap.find(layh);
                    if (it != g_layout_regmap.end() && it->second.built) {
                        built = true;
                        std::memcpy(t_ps, it->second.t_ps, sizeof(t_ps));
                    }
                }
                t_layout_cache.put(layh, locked_epoch, built, t_ps);
            }
            if (!built) return;
        }

        const uint64_t pre_state = state_seq_.snapshot();
        const uint64_t pre_meta = TexStructMetaSeq().snapshot();
        const bool     ps_decl_known = ps_regs.parsed && !ps_regs.unbounded;

        auto read_lod_flags = []() noexcept -> uint8_t {
            return (g_texlod_alias_off.load(std::memory_order_acquire) ? 1u : 0u) |
                   (g_texlod_resemblance.load(std::memory_order_acquire) ? 2u : 0u) |
                   (g_texlod_cache.load(std::memory_order_acquire) ? 4u : 0u);
        };

        RecipeKey rkey{};
        rkey.load = load;
        rkey.dm = dm;
        rkey.comp = comp;
        rkey.ps_hash = ps_hash;
        rkey.ps_regs_lo = ps_regs.lo;
        rkey.layh = layh;
        rkey.ps_decl_known = ps_decl_known ? 1u : 0u;
        rkey.lod_flags = read_lod_flags();

        auto* dev = list->get_device();
        for (unsigned r = 0; r < 8; ++r) {
            if (ps_decl_known && !ps_regs.reads(r)) continue;
            const RegBind& b = t_ps[r];
            if (b.param == 0xFF || b.param >= 64 || !b.is_table) continue;
            const TableStart ts = TableStartOf(dev, gs.root[b.param].table);
            if (!ts) continue;
            const HeapKey key{ ts.heap, ts.base + b.offset };
            ResInfo       ri{};
            DXGI_FORMAT   dfmt = DXGI_FORMAT_UNKNOWN;
            uint64_t      res_gen = 0;
            if (!ReadPublishedDescriptorAndFormat(key, ri, dfmt, &res_gen) || !ri.res) continue;

            const uint64_t res_h = reinterpret_cast<uint64_t>(ri.res);
            const uint32_t hash = TexHashOf(res_h);
            const bool     pre_ovr = ri.swapped != nullptr ||
                std::any_of(out_ps.begin(), out_ps.end(), [&](const ShadowSlotOvr& o) {
                    return o.param == b.param && o.offset == b.offset;
                });

            RecipeSlotSnap& sn = rkey.slots[rkey.active_count++];
            sn.reg = static_cast<uint8_t>(r);
            sn.param = b.param;
            sn.pre_ovr = pre_ovr ? 1u : 0u;
            sn.pad = 0u;
            sn.offset = b.offset;
            sn.vfmt = static_cast<uint32_t>(dfmt);
            sn.hash = hash;
            sn.res_h = res_h;
            sn.res_gen = res_gen;
        }
        if (!rkey.active_count) return;

        static thread_local ThreadRecipeCache t_recipe_cache;
        if (const RecipeEntry* hit = t_recipe_cache.lookup(rkey, pre_state, pre_meta)) {
            RtRfxTextureSource live_srcs[8]{};
            bool               all_ok = true;
            const wchar_t*     wcl_ns = g_geomods[dm].wcl_ns.c_str();
            for (uint8_t i = 0; i < hit->bind_count; ++i) {
                live_srcs[i] = RtResolveRfxTexture(hit->binds[i].fq, wcl_ns);
                if (!live_srcs[i].srv.ptr || !live_srcs[i].resource) {
                    all_ok = false;
                    break;
                }
            }
            if (all_ok && state_seq_.snapshot() == pre_state && TexStructMetaSeq().snapshot() == pre_meta) {
                for (uint8_t i = 0; i < hit->bind_count; ++i) {
                    const auto& b = hit->binds[i];
                    const auto& src = live_srcs[i];
                    out_ps.push_back(ShadowSlotOvr{ b.param, b.offset, src.srv });
                    if (hybrid && out_owners.size() + 1 == out_ps.size())
                        out_owners.push_back(reinterpret_cast<uint64_t>(src.resource.Get()));
                    out_textures.emplace_back(src.resource);
                    if (out_cache) {
                        FallbackResolvedSlot cached{};
                        cached.reg = b.reg;
                        cached.param = b.param;
                        cached.offset = b.offset;
                        cached.vfmt = b.vfmt;
                        cached.srv = src.srv;
                        cached.res = src.resource;
                        out_cache->push_back(std::move(cached));
                    }
                }
                g_struct_fb_cache_hits.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            t_recipe_cache.invalidate(rkey);
        }
        g_struct_fb_cache_misses.fetch_add(1, std::memory_order_relaxed);

        struct DrawSlot {
            unsigned reg = 0;
            uint8_t  param = 0xFF;
            uint32_t offset = 0;
            uint32_t vfmt = 0;
            uint32_t w = 0;
            uint32_t h = 0;
            uint32_t hash = 0;
            uint32_t canon_hash = 0;
            uint32_t lod = 0;
            uint32_t vis_key = 0;
            bool     vis_is_live = false;
            bool     is_cutout = false;
            bool     resolved = false;
        };
        DrawSlot slots[8]{};
        unsigned nslots = 0;
        uint32_t exact_rules_local[8]{};
        unsigned nexact_local = 0;

        for (uint8_t si = 0; si < rkey.active_count; ++si) {
            const RecipeSlotSnap& sn = rkey.slots[si];
            const uint32_t        hash = sn.hash;
            const uint32_t        rule = TexRuleHashFor(hash);
            if (rule) {
                if (nexact_local < 8) exact_rules_local[nexact_local++] = rule;
                NoteExactHitRule(load, dm, comp, rule);
                continue;
            }
            if (sn.pre_ovr) continue;
            TexInfo ti{};
            if (!TexDimsOf(sn.res_h, &ti)) continue;

            const uint32_t vfmt = sn.vfmt;
            const uint32_t mip_parent = hash ? TexVisualParentOf(hash) : 0;
            const uint32_t canon_hash = mip_parent ? mip_parent : hash;
            const bool     is_cutout = (hash && TexLookIsCutoutMask(hash)) ||
                (mip_parent && TexLookIsCutoutMask(mip_parent));
            if (!vfmt || (!IsMaterialTexDims(ti.width, ti.height) && !is_cutout)) continue;

            uint32_t lod = hash ? TexLodTargetOf(hash) : 0;
            if (!lod && mip_parent) lod = TexLodTargetOf(mip_parent);
            if (canon_hash && !lod && !is_cutout) {
                if (NoteAndCheckSharedEngineTex(load, dm, comp, canon_hash)) continue;
            }

            uint32_t vis_key = 0;
            bool     vis_is_live = false;
            if (!is_cutout) {
                if (lod && TexLookHasRef(lod)) {
                    vis_key = lod;
                } else if (canon_hash && TexLookHasAnySig(canon_hash)) {
                    vis_key = canon_hash;
                    vis_is_live = true;
                } else if (hash && TexLookHasAnySig(hash)) {
                    vis_key = hash;
                    vis_is_live = true;
                }
            }

            slots[nslots++] = DrawSlot{ sn.reg, sn.param, sn.offset, vfmt, ti.width, ti.height, hash, canon_hash, lod, vis_key,
                vis_is_live, is_cutout, false };
        }
        if (!nslots) {
            const uint8_t post_lod_flags = read_lod_flags();
            if (post_lod_flags == rkey.lod_flags &&
                PublishSeq::is_stable(pre_state) && PublishSeq::is_stable(pre_meta) &&
                state_seq_.snapshot() == pre_state && TexStructMetaSeq().snapshot() == pre_meta) {
                t_recipe_cache.store(rkey, pre_state, pre_meta, nullptr, 0);
            }
            return;
        }

        std::shared_ptr<const ModStructIndex> idx = GetOrBuildIndex(load, dm);
        if (!idx || (idx->all_dds.empty() && idx->extra_override_dds.empty())) {
            const uint8_t post_lod_flags = read_lod_flags();
            if (post_lod_flags == rkey.lod_flags &&
                PublishSeq::is_stable(pre_state) && PublishSeq::is_stable(pre_meta) &&
                state_seq_.snapshot() == pre_state && TexStructMetaSeq().snapshot() == pre_meta) {
                t_recipe_cache.store(rkey, pre_state, pre_meta, nullptr, 0);
            }
            return;
        }

        const uint32_t guid = idx->object_guid;
        const uint64_t slot_shader_key = ComputeSlotShaderKey(ps_hash, ps_regs.lo, ps_decl_known);

        std::vector<std::wstring> used_fqs;
        used_fqs.reserve(nslots);
        auto fq_already_used = [&](const std::wstring& fq) -> bool {
            for (const auto& u : used_fqs)
                if (_wcsicmp(u.c_str(), fq.c_str()) == 0) return true;
            return false;
        };

        RecipeBind emitted_binds[8]{};
        uint8_t    emitted_count = 0;
        bool       any_bind_failed = false;

        auto bind_resource = [&](const DrawSlot& ds, const std::wstring& fq, const std::wstring& res_name,
                                 const char* action) -> bool {
            const RtRfxTextureSource src = RtResolveRfxTexture(fq, g_geomods[dm].wcl_ns.c_str());
            if (!src.srv.ptr || !src.resource) {
                any_bind_failed = true;
                return false; // still deferred-loading; next frame retries
            }
            out_ps.push_back(ShadowSlotOvr{ ds.param, ds.offset, src.srv });
            if (hybrid && out_owners.size() + 1 == out_ps.size())
                out_owners.push_back(reinterpret_cast<uint64_t>(src.resource.Get()));
            out_textures.emplace_back(src.resource);
            if (out_cache) {
                FallbackResolvedSlot cached{};
                cached.reg = ds.reg;
                cached.param = ds.param;
                cached.offset = ds.offset;
                cached.vfmt = ds.vfmt;
                cached.srv = src.srv;
                cached.res = src.resource;
                out_cache->push_back(std::move(cached));
            }
            if (emitted_count < 8) {
                emitted_binds[emitted_count++] = RecipeBind{ static_cast<uint8_t>(ds.reg), ds.param, ds.offset, ds.vfmt, fq };
            }
            used_fqs.push_back(fq);
            LogDecisionOnce(load, dm, comp, ds.reg, ds.vfmt, action, 1, 1, res_name,
                guid, ds.w, ds.h, ds.hash, ds.lod);
            return true;
        };

        // Record co-occurring visual keys within the same draw call so Pass 1 distinguishes
        // simultaneous distinct slots (must be 1-to-1 exclusive) from time-separated streaming
        // mips or alternate character forms (which may share or split twin variants cleanly).
        bool need_enqueue = false;
        bool added_cooccur = false;
        uint32_t draw_vis_keys[8]{};
        unsigned n_vis_keys = 0;
        for (unsigned i = 0; i < nslots; ++i) {
            if (slots[i].is_cutout || !slots[i].vis_key) continue;
            const uint32_t ck = slots[i].lod ? slots[i].lod : (slots[i].canon_hash ? slots[i].canon_hash : slots[i].vis_key);
            if (ck) draw_vis_keys[n_vis_keys++] = ck;
        }
        if (n_vis_keys >= 2) {
            added_cooccur = RecordCooccurringKeys(load, dm, comp, draw_vis_keys, n_vis_keys);
        }

        auto yield_cutout_to_unique_excl = [&](const DrawSlot& ds) -> bool {
            unsigned vfmt_slots = 0;
            for (unsigned j = 0; j < nslots; ++j)
                if (slots[j].vfmt == ds.vfmt) ++vfmt_slots;
            if (vfmt_slots != 1) return false;
            auto it = idx->excl_by_comp_fmt.find(CompFmtKey(comp, ds.vfmt));
            if (it == idx->excl_by_comp_fmt.end() || it->second.empty()) return false;
            const ModTexCandidate* unique_cand = nullptr;
            unsigned               live_cands = 0;
            for (size_t ci : it->second) {
                const ModTexCandidate& cand = idx->all_dds[ci];
                if (cand.file_hash) {
                    bool hit_exact = false;
                    for (unsigned k = 0; k < nexact_local; ++k)
                        if (exact_rules_local[k] == cand.file_hash) hit_exact = true;
                    if (hit_exact || IsExactHitRule(load, dm, cand.file_hash)) continue;
                }
                unique_cand = &cand;
                ++live_cands;
            }
            if (live_cands != 1 || !unique_cand || fq_already_used(unique_cand->fq)) return false;
            return !IsFqOwnedByOtherHash(load, idx->fingerprint, comp, unique_cand->fq, ds.hash, ds.canon_hash);
        };

        // Pass 1: Zero-touch visual & cached bindings (canonical TexLook seed, live 5-patch, or persisted hash).
        bool                      any_unresolved_vis = false;
        bool                      any_same_draw_dup = false;
        bool                      any_same_draw_same_vfmt = false;
        uint32_t                  resolved_non_cutout_vfmts[8]{};
        unsigned                  n_resolved_vfmts = 0;
        std::vector<std::wstring> resolved_fqs;
        resolved_fqs.reserve(nslots);
        for (unsigned i = 0; i < nslots; ++i) {
            DrawSlot&      ds = slots[i];
            const uint32_t primary_key = ds.lod ? ds.lod : (ds.canon_hash ? ds.canon_hash : ds.vis_key);
            if (!primary_key) continue;
            VisualResolvedEntry vr;
            uint32_t            matched_key = 0;
            if (LookupVisualResolved(load, idx->fingerprint, dm, comp, primary_key, &vr)) {
                matched_key = primary_key;
            } else if (ds.vis_key && ds.vis_key != primary_key &&
                LookupVisualResolved(load, idx->fingerprint, dm, comp, ds.vis_key, &vr)) {
                matched_key = ds.vis_key;
            }
            if (matched_key) {
                const bool is_cutout_bind = ds.is_cutout || (vr.tier == "cutout_neutralizer") ||
                    (ds.hash && TexLookIsCutoutMask(ds.hash)) || !IsComponentDds(*idx, vr.fq);
                if (is_cutout_bind && yield_cutout_to_unique_excl(ds)) {
                    EraseVisualResolved(load, idx->fingerprint, comp, matched_key);
                    if (!IsMaterialTexDims(ds.w, ds.h)) ds.resolved = true;
                    continue;
                }
            }
            bool obs_changed = false;
            if (ds.vis_key && (!matched_key || matched_key == ds.vis_key)) {
                const uint32_t dim = (std::max)(ds.w, ds.h);
                obs_changed = RecordObservedLod(load, dm, comp, ds.vis_key, ds.vfmt, dim);
            }
            if (matched_key) {
                if (ds.hash && ds.canon_hash && ds.hash != ds.canon_hash)
                    NoteMipAliasForDiskCache(ds.hash, ds.canon_hash);
                // Track every resolved FQ (even while deferred DDS upload is still in flight) so
                // same-draw duplicates are never missed on the draw that first records co-occurrence.
                for (const auto& rfq : resolved_fqs) {
                    if (_wcsicmp(rfq.c_str(), vr.fq.c_str()) == 0) {
                        any_same_draw_dup = true;
                        break;
                    }
                }
                resolved_fqs.push_back(vr.fq);
                const bool is_cutout_bind = ds.is_cutout || (vr.tier == "cutout_neutralizer") ||
                    (ds.hash && TexLookIsCutoutMask(ds.hash)) || !IsComponentDds(*idx, vr.fq);
                if (is_cutout_bind) {
                    if (ds.hash) TexLookMarkCutoutMask(ds.hash);
                    if (ds.canon_hash && ds.canon_hash != ds.hash) TexLookMarkCutoutMask(ds.canon_hash);
                }
                if (!is_cutout_bind && ds.vfmt) {
                    for (unsigned vi = 0; vi < n_resolved_vfmts; ++vi) {
                        if (resolved_non_cutout_vfmts[vi] == ds.vfmt) {
                            any_same_draw_same_vfmt = true;
                            break;
                        }
                    }
                    if (n_resolved_vfmts < 8) resolved_non_cutout_vfmts[n_resolved_vfmts++] = ds.vfmt;
                }
                const char* act = is_cutout_bind
                    ? "bind_cutout_clear"
                    : (ds.vis_key ? (ds.vis_is_live ? "bind_visual_live" : "bind_visual_auto") : "bind_visual_cached");
                bind_resource(ds, vr.fq, vr.name, act);
                // Keep cutout neutralizers strictly in the content-hash cache (never in generic proven-slot
                // replay, so Pass 1.5 cannot whiten a different non-cutout texture on the same slot).
                if (slot_shader_key && !is_cutout_bind)
                    NoteProvenSlot(load, idx->fingerprint, dm, comp, slot_shader_key, ds.reg, ds.vfmt, matched_key, vr);
                // Re-enqueue a cached hit only if a higher-res mip arrived during live solving (not F10/restart cache reload).
                if (obs_changed && vr.tier != "cached") need_enqueue = true;
                // Mark resolved even while deferred DDS upload is in flight so structural fallback won't collide.
                ds.resolved = true;
                continue;
            }
            if (ds.vis_key) {
                any_unresolved_vis = true;
                if (obs_changed) need_enqueue = true;
            }
        }
        if (added_cooccur && (any_unresolved_vis || any_same_draw_dup || any_same_draw_same_vfmt)) need_enqueue = true;
        if (need_enqueue) EnqueueCompAdapt(load, dm, comp, idx);

        // Pass 1b: Binary cutout-mask neutralization (universal: detects white-with-black-cutout
        // visibility/alpha masks at upload time — including 64x64/128x128 resident mip tails —
        // and neutralizes them with the mod's own verified pure-white [TextureOverride] DDS
        // when its HD hash shifted on DX12/UHD).
        bool any_pending_neut = false;
        for (unsigned i = 0; i < nslots; ++i) {
            DrawSlot& ds = slots[i];
            if (ds.resolved || !ds.hash || ds.w < 64 || ds.h < 64 || !IsBcRgbaFormat(ds.vfmt)) continue;
            if (!ds.is_cutout && !TexLookIsCutoutMask(ds.hash)) continue;
            // If this is the sole slot of its format and the component owns a unique unclaimed
            // exclusive DDS of that exact format (e.g. a grayscale/binary eye/eyelash mask),
            // yield to Pass 2 `bind_unique` instead of blanking it with the pure-white neutralizer.
            if (yield_cutout_to_unique_excl(ds)) {
                if (!IsMaterialTexDims(ds.w, ds.h)) ds.resolved = true;
                continue;
            }
            // Never let Pass 1.5 or Pass 2 bind a diffuse/normal texture onto a binary cutout mask.
            ds.resolved = true;
            if (idx->extra_override_dds.empty()) continue;

            NeutralizerState ns;
            bool             need_probe = false;
            bool             all_exact = false;
            {
                std::lock_guard l(mtx_);
                ResetIfNeededLocked(load);
                auto nit = neutralizers_.find(dm);
                if (nit == neutralizers_.end() || !nit->second.probed) {
                    need_probe = true;
                } else {
                    ns = nit->second;
                    if (ns.found && !ns.override_hashes.empty()) {
                        auto eit = exact_hits_.find(dm);
                        if (eit != exact_hits_.end()) {
                            all_exact = true;
                            for (uint32_t h : ns.override_hashes) {
                                if (!eit->second.count(h)) {
                                    all_exact = false;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            if (need_probe) {
                any_pending_neut = true;
                EnqueueNeutralizerProbe(load, dm, idx);
                continue;
            }
            if (!ns.found || all_exact || ns.fq.empty()) continue;

            const VisualResolvedEntry vr{ ns.fq, ns.name, 1.f, "cutout_neutralizer" };
            if (bind_resource(ds, vr.fq, vr.name, "bind_cutout_clear")) {
                const uint32_t rec_hash = ds.canon_hash ? ds.canon_hash : ds.hash;
                if (ds.hash) TexLookMarkCutoutMask(ds.hash);
                if (rec_hash && rec_hash != ds.hash) {
                    TexLookMarkCutoutMask(rec_hash);
                    NoteMipAliasForDiskCache(ds.hash, rec_hash);
                }
                RecordCutoutNeutralized(load, idx->fingerprint, dm, comp, rec_hash, guid, vr);
            }
        }

        // Pass 1.5: Proven (Shader+PSDecl, Comp, Slot, Format) anti-flicker memory (persisted across F10 & restarts)
        // when a VRAM-reused streaming mip or form texture temporarily lacks a hash or live 5-patch signature.
        if (slot_shader_key) {
            for (unsigned i = 0; i < nslots; ++i) {
                DrawSlot& ds = slots[i];
                if (ds.resolved) continue;
                VisualResolvedEntry vr;
                if (LookupProvenSlot(load, idx->fingerprint, dm, comp, slot_shader_key, ds.reg, ds.vfmt, used_fqs, &vr)) {
                    bind_resource(ds, vr.fq, vr.name, "bind_proven_slot");
                    ds.resolved = true;
                }
            }
        }

        // Pass 2: Unambiguous 1-to-1 structural fallback for remaining slots without a visual binding.
        auto remaining_slot_count = [&](uint32_t fmt) -> unsigned {
            unsigned c = 0;
            for (unsigned i = 0; i < nslots; ++i)
                if (!slots[i].resolved && slots[i].vfmt == fmt) ++c;
            return c;
        };

        for (unsigned i = 0; i < nslots; ++i) {
            const DrawSlot& ds = slots[i];
            if (ds.resolved) continue;
            const unsigned sc = remaining_slot_count(ds.vfmt);
            if (sc != 1) {
                LogDecisionOnce(load, dm, comp, ds.reg, ds.vfmt, "skip_multi_slot", sc, 0, L"",
                    guid, ds.w, ds.h, ds.hash, ds.lod);
                continue;
            }

            auto it = idx->excl_by_comp_fmt.find(CompFmtKey(comp, ds.vfmt));
            if (it == idx->excl_by_comp_fmt.end() || it->second.empty()) continue;

            const ModTexCandidate* unique_cand = nullptr;
            unsigned               live_cands = 0;
            for (size_t ci : it->second) {
                const ModTexCandidate& cand = idx->all_dds[ci];
                if (cand.file_hash) {
                    bool hit_exact = false;
                    for (unsigned k = 0; k < nexact_local; ++k)
                        if (exact_rules_local[k] == cand.file_hash) hit_exact = true;
                    if (hit_exact || IsExactHitRule(load, dm, cand.file_hash)) continue;
                }
                unique_cand = &cand;
                ++live_cands;
            }

            if (live_cands != 1 || !unique_cand) {
                if (live_cands > 1)
                    LogDecisionOnce(load, dm, comp, ds.reg, ds.vfmt, "skip_multi_cand", sc, live_cands, L"",
                        guid, ds.w, ds.h, ds.hash, ds.lod);
                continue;
            }
            if (fq_already_used(unique_cand->fq)) continue;

            bind_resource(ds, unique_cand->fq, unique_cand->name, "bind_unique");
        }

        const uint8_t post_lod_flags = read_lod_flags();
        const bool    vis_ready = !any_unresolved_vis || (!need_enqueue && IsCompEvidenceSettled(load, dm, comp));
        if (!need_enqueue && vis_ready && !any_pending_neut && !any_bind_failed &&
            post_lod_flags == rkey.lod_flags &&
            PublishSeq::is_stable(pre_state) && PublishSeq::is_stable(pre_meta) &&
            state_seq_.snapshot() == pre_state && TexStructMetaSeq().snapshot() == pre_meta) {
            t_recipe_cache.store(rkey, pre_state, pre_meta, emitted_binds, emitted_count);
        }
    }

private:
    using ObsLod = ObsLodRecord;
    struct AdaptJob {
        unsigned                              load = 0;
        int                                   dm = 0;
        int                                   comp = 0;
        std::shared_ptr<const ModStructIndex> idx;
    };
    struct DiskKey {
        uint64_t fp = 0;
        int      comp = 0;
        uint32_t lod = 0;
        bool     operator==(const DiskKey& o) const { return fp == o.fp && comp == o.comp && lod == o.lod; }
    };
    struct DiskKeyHash {
        size_t operator()(const DiskKey& k) const noexcept
        {
            uint64_t h = k.fp ^ (static_cast<uint64_t>(static_cast<uint32_t>(k.comp)) << 32) ^ k.lod;
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdull;
            h ^= h >> 33;
            return static_cast<size_t>(h);
        }
    };

    struct ProvenSlotKey {
        int      dm = 0;
        int      comp = 0;
        uint64_t ps_hash = 0;
        unsigned reg = 0;
        uint32_t vfmt = 0;
        bool     operator==(const ProvenSlotKey& o) const
        {
            return dm == o.dm && comp == o.comp && ps_hash == o.ps_hash && reg == o.reg && vfmt == o.vfmt;
        }
    };
    struct ProvenSlotKeyHash {
        size_t operator()(const ProvenSlotKey& k) const noexcept
        {
            uint64_t h = k.ps_hash ^ (static_cast<uint64_t>(static_cast<uint32_t>(k.dm)) << 48) ^
                (static_cast<uint64_t>(static_cast<uint32_t>(k.comp)) << 32) ^
                (static_cast<uint64_t>(k.reg) << 16) ^ k.vfmt;
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdull;
            h ^= h >> 33;
            return static_cast<size_t>(h);
        }
    };

    struct DiskSlotKey {
        uint64_t fp = 0;
        int      comp = 0;
        uint64_t ps_hash = 0;
        unsigned reg = 0;
        uint32_t vfmt = 0;
        bool     operator==(const DiskSlotKey& o) const
        {
            return fp == o.fp && comp == o.comp && ps_hash == o.ps_hash && reg == o.reg && vfmt == o.vfmt;
        }
    };
    struct DiskSlotKeyHash {
        size_t operator()(const DiskSlotKey& k) const noexcept
        {
            uint64_t h = k.fp ^ k.ps_hash ^ (static_cast<uint64_t>(static_cast<uint32_t>(k.comp)) << 32) ^
                (static_cast<uint64_t>(k.reg) << 16) ^ k.vfmt;
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdull;
            h ^= h >> 33;
            return static_cast<size_t>(h);
        }
    };

    struct StrongFqOwner {
        int      comp = -1;
        uint32_t key = 0;
        double   ssim = 0.0;
    };

    struct NeutralizerState {
        bool                  probed = false;
        bool                  found = false;
        std::wstring          fq;
        std::wstring          name;
        std::vector<uint32_t> override_hashes;
    };

    struct NeutralizerJob {
        unsigned                              load = 0;
        int                                   dm = 0;
        std::shared_ptr<const ModStructIndex> idx;
    };

    std::mutex                                               mtx_;
    PublishSeq                                               state_seq_;
    unsigned                                                 gen_ = 0;
    std::unordered_map<int, std::shared_ptr<ModStructIndex>> indices_;
    std::unordered_map<int, NeutralizerState>                neutralizers_;
    std::unordered_map<int, std::unordered_set<uint32_t>>    exact_hits_;
    std::unordered_map<uint64_t, uint64_t>                   small_hash_comps_;
    std::unordered_map<uint64_t, uint64_t>                   comp_evidence_seq_;
    std::unordered_map<uint64_t, uint64_t>                   comp_evaluated_seq_;
    std::unordered_map<int, std::unordered_set<uint32_t>>    shared_engine_hashes_; // dm -> hashes seen on >=3 comps
    std::unordered_map<uint64_t, std::vector<ObsLod>>        observed_lods_; // (dm, comp) -> observed (vis_key, vfmt)
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> cooccur_pairs_; // (dm, comp) -> unordered pairs of same-draw vis_keys
    std::unordered_map<ProvenSlotKey, VisualResolvedEntry, ProvenSlotKeyHash> proven_slots_; // anti-flicker proven (shader_key,comp,slot,vfmt) memory
    std::unordered_map<int, std::unordered_map<std::wstring, StrongFqOwner>> strong_fq_owners_; // dm -> lower(fq) -> high-SSIM owner
    std::unordered_map<DiskKey, VisualResolvedEntry, DiskKeyHash> visual_resolved_;
    struct DiskSlotValue {
        std::string fq;
        uint32_t    source_lod = 0;
    };

    std::unordered_map<DiskKey, std::string, DiskKeyHash>    disk_cache_; // persisted across runs: key -> fq (utf8)
    std::unordered_map<DiskSlotKey, DiskSlotValue, DiskSlotKeyHash> disk_slot_cache_; // persisted unambiguous (fp,comp,shader_key,reg,vfmt) -> (fq, source_lod)
    std::unordered_set<DiskSlotKey, DiskSlotKeyHash>         ambiguous_disk_slots_; // slots observed with >1 distinct fq
    std::unordered_set<uint64_t>                             persisted_mip_aliases_; // (child<<32)|parent persisted in disk cache
    bool                                                     disk_loaded_ = false;
    bool                                                     disk_dirty_ = false;
    std::set<std::tuple<unsigned, int, int, unsigned, uint32_t, std::string>> logged_;

    // Background worker state
    std::mutex                                  worker_mtx_;
    std::condition_variable                     worker_cv_;
    std::deque<AdaptJob>                        queue_;
    std::unordered_set<uint64_t>                queued_keys_; // (dm, comp) currently queued
    std::deque<NeutralizerJob>                  neut_queue_;
    std::unordered_set<int>                     neut_queued_dms_;
    unsigned                                    queued_gen_ = 0;
    bool                                        flush_requested_ = false;
    std::atomic<bool>                           stop_{ false };
    std::thread                                 worker_;
    std::unordered_map<std::wstring, PatchSig>  dds_sig_cache_;   // worker-thread only!
    std::unordered_map<std::wstring, bool>      dds_white_cache_; // worker-thread only!

    static uint64_t DmCompKey(int dm, int comp)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(dm)) << 32) | static_cast<uint32_t>(comp);
    }

    static std::wstring DiskCachePath()
    {
        if (g_data_dir.empty()) return {};
        return g_data_dir + L"\\cache\\mod_tex_adapt.cache";
    }

    void NoteMipAliasForDiskCache(uint32_t child, uint32_t parent)
    {
        if (!child || !parent || child == parent) return;
        const uint64_t key = (static_cast<uint64_t>(child) << 32) | parent;
        bool           need_flush = false;
        {
            std::lock_guard l(mtx_);
            if (persisted_mip_aliases_.insert(key).second) {
                disk_dirty_ = true;
                need_flush = true;
            }
        }
        if (need_flush) RequestDiskFlush();
    }

    void EnsureDiskCacheLoadedLocked()
    {
        if (disk_loaded_) return;
        PublishSeq::Guard pub(state_seq_);
        disk_loaded_ = true;
        const std::wstring p = DiskCachePath();
        if (p.empty()) return;
        FILE* f = _wfsopen(p.c_str(), L"r", _SH_DENYNO);
        if (!f) return;
        char        buf[4096];
        std::string line;
        size_t      loaded = 0;
        size_t      loaded_slots = 0;
        size_t      loaded_mips = 0;
        bool        header_checked = false;
        bool        version_ok = false;
        while (fgets(buf, sizeof(buf), f)) {
            line += buf;
            if (!line.empty() && line.back() != '\n' && !feof(f)) continue;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!header_checked) {
                header_checked = true;
                version_ok = (line.find("(v6)") != std::string::npos);
                if (!version_ok) {
                    logger::probe("TEX-AUTO-ADAPT discarding legacy mod_tex_adapt.cache (upgrading to v6)");
                    disk_dirty_ = true;
                    break;
                }
            }
            if (line.empty() || line[0] == ';') {
                line.clear();
                continue;
            }
            if (line.size() > 2 && line[0] == 'M' && line[1] == '\t') {
                // Format: M\t<child_hex>\t<parent_hex>
                size_t t1 = 1;
                size_t t2 = line.find('\t', t1 + 1);
                if (t2 != std::string::npos) {
                    const uint32_t child = static_cast<uint32_t>(strtoul(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 16));
                    const uint32_t parent = static_cast<uint32_t>(strtoul(line.substr(t2 + 1).c_str(), nullptr, 16));
                    if (child && parent && child != parent) {
                        TexNoteVisualMipParent(child, parent);
                        persisted_mip_aliases_.insert((static_cast<uint64_t>(child) << 32) | parent);
                        ++loaded_mips;
                    }
                }
                line.clear();
                continue;
            }
            if (line.size() > 2 && line[0] == 'S' && line[1] == '\t') {
                // Format: S\t<fp_hex>\t<comp>\t<ps_hash_hex>\t<reg>\t<vfmt>\t<fq_utf8>
                size_t t1 = 1;
                size_t t2 = line.find('\t', t1 + 1);
                size_t t3 = (t2 == std::string::npos) ? std::string::npos : line.find('\t', t2 + 1);
                size_t t4 = (t3 == std::string::npos) ? std::string::npos : line.find('\t', t3 + 1);
                size_t t5 = (t4 == std::string::npos) ? std::string::npos : line.find('\t', t4 + 1);
                size_t t6 = (t5 == std::string::npos) ? std::string::npos : line.find('\t', t5 + 1);
                if (t2 != std::string::npos && t3 != std::string::npos && t4 != std::string::npos &&
                    t5 != std::string::npos && t6 != std::string::npos) {
                    const uint64_t fp = strtoull(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 16);
                    const int      comp = atoi(line.substr(t2 + 1, t3 - t2 - 1).c_str());
                    const uint64_t ps = strtoull(line.substr(t3 + 1, t4 - t3 - 1).c_str(), nullptr, 16);
                    const unsigned reg = static_cast<unsigned>(strtoul(line.substr(t4 + 1, t5 - t4 - 1).c_str(), nullptr, 10));
                    const uint32_t vfmt = static_cast<uint32_t>(strtoul(line.substr(t5 + 1, t6 - t5 - 1).c_str(), nullptr, 10));
                    const size_t   t7 = line.find('\t', t6 + 1);
                    const std::string fq = (t7 == std::string::npos) ? line.substr(t6 + 1) : line.substr(t6 + 1, t7 - t6 - 1);
                    const uint32_t src_lod = (t7 == std::string::npos)
                        ? 0u
                        : static_cast<uint32_t>(strtoul(line.substr(t7 + 1).c_str(), nullptr, 16));
                    if (fp && ps && vfmt && !fq.empty()) {
                        disk_slot_cache_[DiskSlotKey{ fp, comp, ps, reg, vfmt }] = DiskSlotValue{ fq, src_lod };
                        ++loaded_slots;
                    }
                }
                line.clear();
                continue;
            }
            // Format: <fp_hex>\t<comp>\t<lod_hex>\t<fq_utf8>
            size_t t1 = line.find('\t');
            size_t t2 = (t1 == std::string::npos) ? std::string::npos : line.find('\t', t1 + 1);
            size_t t3 = (t2 == std::string::npos) ? std::string::npos : line.find('\t', t2 + 1);
            if (t1 != std::string::npos && t2 != std::string::npos && t3 != std::string::npos) {
                const uint64_t fp = strtoull(line.substr(0, t1).c_str(), nullptr, 16);
                const int      comp = atoi(line.substr(t1 + 1, t2 - t1 - 1).c_str());
                const uint32_t lod = static_cast<uint32_t>(strtoul(line.substr(t2 + 1, t3 - t2 - 1).c_str(), nullptr, 16));
                const std::string fq = line.substr(t3 + 1);
                if (fp && lod && !fq.empty()) {
                    disk_cache_[DiskKey{ fp, comp, lod }] = fq;
                    ++loaded;
                }
            }
            line.clear();
        }
        fclose(f);
        if (loaded || loaded_slots || loaded_mips)
            logger::probe("TEX-AUTO-ADAPT loaded %zu visual + %zu proven-slot + %zu mip-alias persisted binding(s) from mod_tex_adapt.cache",
                loaded, loaded_slots, loaded_mips);
    }

    void FlushDiskCache()
    {
        std::vector<std::tuple<uint64_t, int, uint32_t, std::string>> rows;
        std::vector<std::tuple<uint64_t, int, uint64_t, unsigned, uint32_t, std::string, uint32_t>> slot_rows;
        std::unordered_set<uint32_t> parent_hashes;
        std::vector<std::pair<uint32_t, uint32_t>> mip_rows;
        {
            std::lock_guard l(mtx_);
            if (!disk_dirty_) return;
            rows.reserve(disk_cache_.size());
            for (const auto& kv : disk_cache_) {
                rows.emplace_back(kv.first.fp, kv.first.comp, kv.first.lod, kv.second);
                parent_hashes.insert(kv.first.lod);
            }
            slot_rows.reserve(disk_slot_cache_.size());
            for (const auto& kv : disk_slot_cache_)
                slot_rows.emplace_back(kv.first.fp, kv.first.comp, kv.first.ps_hash, kv.first.reg, kv.first.vfmt,
                    kv.second.fq, kv.second.source_lod);
            for (uint64_t packed : persisted_mip_aliases_) {
                const uint32_t child = static_cast<uint32_t>(packed >> 32);
                const uint32_t parent = static_cast<uint32_t>(packed & 0xffffffffu);
                if (child && parent && parent_hashes.count(parent))
                    mip_rows.emplace_back(child, parent);
            }
            disk_dirty_ = false;
        }
        const auto live_mips = TexCopyVisualMipParentsFor(parent_hashes);
        if (!live_mips.empty()) {
            mip_rows.insert(mip_rows.end(), live_mips.begin(), live_mips.end());
            std::lock_guard l(mtx_);
            for (const auto& mp : live_mips)
                persisted_mip_aliases_.insert((static_cast<uint64_t>(mp.first) << 32) | mp.second);
        }
        std::sort(mip_rows.begin(), mip_rows.end());
        mip_rows.erase(std::unique(mip_rows.begin(), mip_rows.end()), mip_rows.end());
        const std::wstring p = DiskCachePath();
        if (p.empty()) return;
        const std::wstring tmp = p + L".tmp";
        FILE*              f = _wfsopen(tmp.c_str(), L"w", _SH_DENYNO);
        if (!f) return;
        fputs("; wwmi-dx12-rt Layer 3 auto-adapted visual texture bindings (v6)\n", f);
        std::sort(rows.begin(), rows.end());
        std::sort(slot_rows.begin(), slot_rows.end());
        bool ok = true;
        for (const auto& r : rows) {
            if (fprintf(f, "%016llx\t%d\t%08x\t%s\n",
                    static_cast<unsigned long long>(std::get<0>(r)),
                    std::get<1>(r),
                    std::get<2>(r),
                    std::get<3>(r).c_str()) < 0) {
                ok = false;
                break;
            }
        }
        if (ok) {
            for (const auto& s : slot_rows) {
                const uint32_t src_lod = std::get<6>(s);
                const int      rc = src_lod
                    ? fprintf(f, "S\t%016llx\t%d\t%016llx\t%u\t%u\t%s\t%08x\n",
                          static_cast<unsigned long long>(std::get<0>(s)),
                          std::get<1>(s),
                          static_cast<unsigned long long>(std::get<2>(s)),
                          std::get<3>(s),
                          std::get<4>(s),
                          std::get<5>(s).c_str(),
                          src_lod)
                    : fprintf(f, "S\t%016llx\t%d\t%016llx\t%u\t%u\t%s\n",
                          static_cast<unsigned long long>(std::get<0>(s)),
                          std::get<1>(s),
                          static_cast<unsigned long long>(std::get<2>(s)),
                          std::get<3>(s),
                          std::get<4>(s),
                          std::get<5>(s).c_str());
                if (rc < 0) {
                    ok = false;
                    break;
                }
            }
        }
        if (ok) {
            for (const auto& m : mip_rows) {
                if (fprintf(f, "M\t%08x\t%08x\n", m.first, m.second) < 0) {
                    ok = false;
                    break;
                }
            }
        }
        ok = (fclose(f) == 0) && ok;
        if (ok)
            MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
        else
            DeleteFileW(tmp.c_str());
    }

    void ResetIfNeededLocked(unsigned load)
    {
        EnsureDiskCacheLoadedLocked();
        if (gen_ == load) return;
        PublishSeq::Guard pub(state_seq_);
        gen_ = load;
        indices_.clear();
        neutralizers_.clear();
        exact_hits_.clear();
        small_hash_comps_.clear();
        comp_evidence_seq_.clear();
        comp_evaluated_seq_.clear();
        shared_engine_hashes_.clear();
        observed_lods_.clear();
        cooccur_pairs_.clear();
        proven_slots_.clear();
        strong_fq_owners_.clear();
        visual_resolved_.clear();
        logged_.clear();
    }

    void NoteExactHitRule(unsigned load, int dm, int comp, uint32_t rule)
    {
        if (!rule) return;
        std::shared_ptr<const ModStructIndex> re_idx;
        std::vector<int>                      requeue_comps;
        {
            std::lock_guard l(mtx_);
            ResetIfNeededLocked(load);
            if (exact_hits_[dm].insert(rule).second) {
                auto iit = indices_.find(dm);
                if (iit != indices_.end()) re_idx = iit->second;
                for (const auto& kv : observed_lods_) {
                    if (static_cast<int>(kv.first >> 32) == dm && !kv.second.empty()) {
                        ++comp_evidence_seq_[kv.first];
                        if (re_idx) requeue_comps.push_back(static_cast<int>(kv.first & 0xffffffffu));
                    }
                }
                state_seq_.bump();
                (void)comp;
            }
        }
        if (re_idx) {
            for (int c : requeue_comps) EnqueueCompAdapt(load, dm, c, re_idx);
        }
    }

    bool IsExactHitRule(unsigned load, int dm, uint32_t rule)
    {
        if (!rule) return false;
        std::lock_guard l(mtx_);
        if (gen_ != load) return false;
        auto it = exact_hits_.find(dm);
        return it != exact_hits_.end() && it->second.count(rule) != 0;
    }

    bool NoteAndCheckSharedEngineTex(unsigned load, int dm, int comp, uint32_t hash)
    {
        if (comp < 0 || comp >= 64) return false;
        std::vector<int>                      affected_comps;
        std::shared_ptr<const ModStructIndex> re_idx;
        bool                                  shared_now = false;
        {
            std::lock_guard l(mtx_);
            ResetIfNeededLocked(load);
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(dm)) << 32) | hash;
            uint64_t&      mask = small_hash_comps_[key];
            mask |= (1ull << comp);
            unsigned bits = 0;
            for (uint64_t v = mask; v; v &= (v - 1)) ++bits;
            shared_now = (bits >= 3);
            if (shared_now && shared_engine_hashes_[dm].insert(hash).second) {
                PublishSeq::Guard pub(state_seq_);
                // Newly identified cross-component engine/FX texture (seen on >=3 components with no seed ref):
                // purge any premature entries written while only 1-2 components had been observed.
                auto iit = indices_.find(dm);
                const uint64_t fp = (iit != indices_.end()) ? iit->second->fingerprint : 0ull;
                if (iit != indices_.end()) re_idx = iit->second;
                for (int c = 0; c < 64; ++c) {
                    if (!(mask & (1ull << c))) continue;
                    auto oit = observed_lods_.find(DmCompKey(dm, c));
                    if (oit != observed_lods_.end()) {
                        auto& v = oit->second;
                        const size_t before = v.size();
                        v.erase(std::remove_if(v.begin(), v.end(), [&](const ObsLod& e) { return e.lod == hash; }), v.end());
                        if (v.size() != before) affected_comps.push_back(c);
                    }
                    if (fp) {
                        const DiskKey dk{ fp, c, hash };
                        visual_resolved_.erase(dk);
                        if (disk_cache_.erase(dk)) disk_dirty_ = true;
                    }
                }
            }
        }
        if (re_idx) {
            for (int c : affected_comps) EnqueueCompAdapt(load, dm, c, re_idx);
        }
        return shared_now;
    }

    static bool IsComponentDds(const ModStructIndex& idx, const std::wstring& fq)
    {
        for (const auto& c : idx.all_dds)
            if (_wcsicmp(c.fq.c_str(), fq.c_str()) == 0) return true;
        return false;
    }

    static bool FindResourceInIndex(const ModStructIndex& idx, const std::wstring& fq,
        std::wstring* out_fq, std::wstring* out_name)
    {
        for (const auto& c : idx.all_dds) {
            if (_wcsicmp(c.fq.c_str(), fq.c_str()) == 0) {
                if (out_fq) *out_fq = c.fq;
                if (out_name) *out_name = c.name;
                return true;
            }
        }
        for (const auto& e : idx.extra_override_dds) {
            if (_wcsicmp(e.fq.c_str(), fq.c_str()) == 0) {
                if (out_fq) *out_fq = e.fq;
                if (out_name) *out_name = e.name;
                return true;
            }
        }
        return false;
    }

    bool IsFqOwnedByOtherHash(unsigned load, uint64_t fp, int comp, const std::wstring& fq,
        uint32_t self_hash, uint32_t self_canon)
    {
        if (!fp || fq.empty()) return false;
        const std::string fq_u8 = WideToUtf8(fq);
        std::lock_guard   l(mtx_);
        ResetIfNeededLocked(load);
        for (const auto& kv : visual_resolved_) {
            if (kv.first.fp != fp || kv.first.comp != comp) continue;
            if (kv.first.lod == self_hash || kv.first.lod == self_canon) continue;
            if (_wcsicmp(kv.second.fq.c_str(), fq.c_str()) == 0) return true;
        }
        for (const auto& kv : disk_cache_) {
            if (kv.first.fp != fp || kv.first.comp != comp) continue;
            if (kv.first.lod == self_hash || kv.first.lod == self_canon) continue;
            if (_stricmp(kv.second.c_str(), fq_u8.c_str()) == 0) return true;
        }
        return false;
    }

    void EraseVisualResolved(unsigned load, uint64_t fp, int comp, uint32_t lod)
    {
        if (!fp || !lod) return;
        bool need_flush = false;
        {
            std::lock_guard l(mtx_);
            ResetIfNeededLocked(load);
            const DiskKey dk{ fp, comp, lod };
            const bool    erased_mem = (visual_resolved_.erase(dk) > 0);
            const bool    erased_disk = (disk_cache_.erase(dk) > 0);
            if (erased_disk) {
                disk_dirty_ = true;
                need_flush = true;
            }
            if (erased_mem || erased_disk) state_seq_.bump();
        }
        if (need_flush) RequestDiskFlush();
    }

    bool LookupVisualResolved(unsigned load, uint64_t fp, int dm, int comp, uint32_t lod, VisualResolvedEntry* out)
    {
        std::lock_guard l(mtx_);
        ResetIfNeededLocked(load);
        const DiskKey dk{ fp, comp, lod };
        auto          it = visual_resolved_.find(dk);
        if (it != visual_resolved_.end()) {
            *out = it->second;
            return true;
        }
        // Check persisted cross-session disk cache
        auto dit = disk_cache_.find(dk);
        if (dit != disk_cache_.end()) {
            const std::wstring fq = Utf8ToWide(dit->second);
            // Verify this fq still exists in the live mod index (component DDS or sibling [TextureOverride] DDS)
            auto iit = indices_.find(dm);
            std::wstring matched_fq, matched_name;
            if (iit != indices_.end() && FindResourceInIndex(*iit->second, fq, &matched_fq, &matched_name)) {
                VisualResolvedEntry e{ matched_fq, matched_name, 1.f, "cached" };
                visual_resolved_[dk] = e;
                state_seq_.bump();
                *out = e;
                return true;
            }
        }
        return false;
    }

    void RecordCutoutNeutralized(unsigned load, uint64_t fp, int dm, int comp, uint32_t hash, uint32_t guid,
        const VisualResolvedEntry& vr)
    {
        if (!fp || !hash || vr.fq.empty()) return;
        bool need_flush = false;
        bool changed = false;
        {
            std::lock_guard l(mtx_);
            ResetIfNeededLocked(load);
            const DiskKey dk{ fp, comp, hash };
            auto          prev = visual_resolved_.find(dk);
            changed = (prev == visual_resolved_.end() || _wcsicmp(prev->second.fq.c_str(), vr.fq.c_str()) != 0);
            const bool tier_changed = (prev == visual_resolved_.end() || prev->second.tier != vr.tier);
            visual_resolved_[dk] = vr;
            const std::string fq_u8 = WideToUtf8(vr.fq);
            auto              dp = disk_cache_.find(dk);
            if (dp == disk_cache_.end() || _stricmp(dp->second.c_str(), fq_u8.c_str()) != 0) {
                disk_cache_[dk] = fq_u8;
                disk_dirty_ = true;
                need_flush = true;
            }
            if (changed || tier_changed || need_flush) state_seq_.bump();
        }
        if (changed) {
            logger::probe("TEX-AUTO-ADAPT gen=%u mod=%d guid=%u comp=%d lod=%08x tier=%s score=1.000 -> %ls",
                load, dm + 1, guid, comp, hash, vr.tier.c_str(), vr.name.c_str());
        }
        if (need_flush) RequestDiskFlush();
    }

    static uint64_t PairKey(uint32_t a, uint32_t b)
    {
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | b;
    }

    bool RecordCooccurringKeys(unsigned load, int dm, int comp, const uint32_t* keys, unsigned count)
    {
        bool added = false;
        std::lock_guard l(mtx_);
        ResetIfNeededLocked(load);
        auto& set = cooccur_pairs_[DmCompKey(dm, comp)];
        for (unsigned i = 0; i < count; ++i) {
            for (unsigned j = i + 1; j < count; ++j) {
                if (keys[i] == keys[j]) continue;
                if (set.insert(PairKey(keys[i], keys[j])).second) added = true;
            }
        }
        if (added) {
            ++comp_evidence_seq_[DmCompKey(dm, comp)];
            state_seq_.bump();
        }
        return added;
    }

    void RequestDiskFlush()
    {
        std::lock_guard wl(worker_mtx_);
        if (stop_) return;
        flush_requested_ = true;
        if (!worker_.joinable()) {
            worker_ = std::thread(&StructFallbackEngine::WorkerLoop, this);
        }
        worker_cv_.notify_one();
    }

    void NoteProvenSlot(unsigned load, uint64_t fp, int dm, int comp, uint64_t ps_hash, unsigned reg, uint32_t vfmt,
        uint32_t source_lod, const VisualResolvedEntry& vr)
    {
        if (!ps_hash || vr.fq.empty()) return;
        const ProvenSlotKey pk{ dm, comp, ps_hash, reg, vfmt };
        bool                need_flush = false;
        {
            std::lock_guard l(mtx_);
            ResetIfNeededLocked(load);
            // Guard against a concurrent worker revocation between LookupVisualResolved and NoteProvenSlot.
            if (fp && source_lod) {
                auto vit = visual_resolved_.find(DiskKey{ fp, comp, source_lod });
                if (vit == visual_resolved_.end() || _wcsicmp(vit->second.fq.c_str(), vr.fq.c_str()) != 0)
                    return;
            }
            bool mutated = false;
            auto it = proven_slots_.find(pk);
            if (it == proven_slots_.end() || _wcsicmp(it->second.fq.c_str(), vr.fq.c_str()) != 0 ||
                (source_lod && (it->second.source_lod == 0 || (it->second.source_lod != source_lod && vr.tier == "high_visual")))) {
                VisualResolvedEntry tagged = vr;
                tagged.source_lod = source_lod;
                proven_slots_[pk] = std::move(tagged);
                mutated = true;
            }
            if (fp) {
                const DiskSlotKey dsk{ fp, comp, ps_hash, reg, vfmt };
                if (!ambiguous_disk_slots_.count(dsk)) {
                    const std::string fq_u8 = WideToUtf8(vr.fq);
                    auto              dit = disk_slot_cache_.find(dsk);
                    if (dit == disk_slot_cache_.end()) {
                        disk_slot_cache_[dsk] = DiskSlotValue{ fq_u8, source_lod };
                        disk_dirty_ = true;
                        need_flush = true;
                        mutated = true;
                    } else if (_stricmp(dit->second.fq.c_str(), fq_u8.c_str()) != 0) {
                        // Multiple distinct textures observed on the same (fp, comp, ps, reg, vfmt)
                        // (e.g. dual-form toggle); never persist an ambiguous slot binding.
                        disk_slot_cache_.erase(dit);
                        ambiguous_disk_slots_.insert(dsk);
                        disk_dirty_ = true;
                        need_flush = true;
                        mutated = true;
                    } else if (source_lod && (dit->second.source_lod == 0 || (dit->second.source_lod != source_lod && vr.tier == "high_visual"))) {
                        dit->second.source_lod = source_lod;
                        disk_dirty_ = true;
                        need_flush = true;
                        mutated = true;
                    }
                }
            }
            if (mutated) state_seq_.bump();
        }
        if (need_flush) RequestDiskFlush();
    }

    bool LookupProvenSlot(unsigned load, uint64_t fp, int dm, int comp, uint64_t ps_hash, unsigned reg, uint32_t vfmt,
        const std::vector<std::wstring>& used_fqs, VisualResolvedEntry* out)
    {
        if (!ps_hash) return false;
        const ProvenSlotKey pk{ dm, comp, ps_hash, reg, vfmt };
        std::lock_guard     l(mtx_);
        ResetIfNeededLocked(load);
        auto it = proven_slots_.find(pk);
        if (it != proven_slots_.end() && !it->second.fq.empty()) {
            for (const auto& u : used_fqs)
                if (_wcsicmp(u.c_str(), it->second.fq.c_str()) == 0) return false;
            *out = it->second;
            return true;
        }
        if (fp) {
            const DiskSlotKey dsk{ fp, comp, ps_hash, reg, vfmt };
            if (!ambiguous_disk_slots_.count(dsk)) {
                auto dit = disk_slot_cache_.find(dsk);
                if (dit != disk_slot_cache_.end()) {
                    const std::wstring fq = Utf8ToWide(dit->second.fq);
                    const uint32_t     src_lod = dit->second.source_lod;
                    auto               iit = indices_.find(dm);
                    if (iit != indices_.end()) {
                        for (const auto& c : iit->second->all_dds) {
                            if (_wcsicmp(c.fq.c_str(), fq.c_str()) == 0) {
                                for (const auto& u : used_fqs)
                                    if (_wcsicmp(u.c_str(), c.fq.c_str()) == 0) return false;
                                VisualResolvedEntry e{ c.fq, c.name, 1.f, "cached_slot", src_lod };
                                proven_slots_[pk] = e;
                                state_seq_.bump();
                                *out = e;
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    bool IsCompEvidenceSettled(unsigned load, int dm, int comp)
    {
        std::lock_guard l(mtx_);
        ResetIfNeededLocked(load);
        const uint64_t qk = DmCompKey(dm, comp);
        const auto     eit = comp_evidence_seq_.find(qk);
        if (eit == comp_evidence_seq_.end() || eit->second == 0) return false;
        const auto vit = comp_evaluated_seq_.find(qk);
        return vit != comp_evaluated_seq_.end() && vit->second == eit->second;
    }

    bool RecordObservedLod(unsigned load, int dm, int comp, uint32_t lod, uint32_t vfmt, uint32_t dim)
    {
        std::lock_guard l(mtx_);
        ResetIfNeededLocked(load);
        const uint64_t qk = DmCompKey(dm, comp);
        auto&          vec = observed_lods_[qk];
        if (!UpsertObservedLod(vec, lod, vfmt, dim)) return false;
        ++comp_evidence_seq_[qk];
        state_seq_.bump();
        return true;
    }

    void EnqueueCompAdapt(unsigned load, int dm, int comp, const std::shared_ptr<const ModStructIndex>& idx)
    {
        std::lock_guard wl(worker_mtx_);
        if (stop_) return;
        if (queued_gen_ != load) {
            queue_.clear();
            queued_keys_.clear();
            neut_queue_.clear();
            neut_queued_dms_.clear();
            queued_gen_ = load;
        }
        const uint64_t qk = DmCompKey(dm, comp);
        if (!queued_keys_.insert(qk).second) return;
        queue_.push_back(AdaptJob{ load, dm, comp, idx });
        if (!worker_.joinable()) {
            worker_ = std::thread(&StructFallbackEngine::WorkerLoop, this);
        }
        worker_cv_.notify_one();
    }

    void EnqueueNeutralizerProbe(unsigned load, int dm, const std::shared_ptr<const ModStructIndex>& idx)
    {
        std::lock_guard wl(worker_mtx_);
        if (stop_) return;
        if (queued_gen_ != load) {
            queue_.clear();
            queued_keys_.clear();
            neut_queue_.clear();
            neut_queued_dms_.clear();
            queued_gen_ = load;
        }
        if (!neut_queued_dms_.insert(dm).second) return;
        neut_queue_.push_back(NeutralizerJob{ load, dm, idx });
        if (!worker_.joinable()) {
            worker_ = std::thread(&StructFallbackEngine::WorkerLoop, this);
        }
        worker_cv_.notify_one();
    }

    std::shared_ptr<const ModStructIndex> GetOrBuildIndex(unsigned load, int dm)
    {
        {
            std::lock_guard l(mtx_);
            ResetIfNeededLocked(load);
            auto it = indices_.find(dm);
            if (it != indices_.end()) return it->second;
        }

        auto                built = std::make_shared<ModStructIndex>();
        const WwmiMod::Mod& mod = g_geomods[dm].mod;
        built->object_guid = mod.object_guid;
        uint64_t fp = 0xcbf29ce484222325ull;
        fp = FnvMixStr(fp, mod.dir);
        fp = FnvMixU64(fp, static_cast<uint64_t>(mod.object_guid));

        struct DdsHeaderCacheEntry {
            uint32_t w = 0;
            uint32_t h = 0;
            uint32_t fmt = 0;
            uint64_t sz = 0;
        };
        std::unordered_set<std::wstring>                          seen_files;
        std::unordered_map<std::wstring, DdsHeaderCacheEntry>     seen_meta;
        std::unordered_set<std::wstring>                          seen_fp_fqs;
        std::vector<std::pair<std::wstring, uint64_t>>            fp_items;

        for (const WwmiMod::Resource& r : mod.resources) {
            if (r.filename.empty() || r.fq.empty()) continue;
            if (!EndsWithNoCase(r.filename, L".dds")) continue;
            if (!StartsWithNoCase(r.name, L"ResourceTexture")) continue;

            const std::vector<int> comps = ParseFilenameComponents(r.filename);
            if (comps.empty()) continue;

            const std::wstring fkey = NormalizePathLower(r.filename);
            if (!seen_files.insert(fkey).second) continue;

            uint32_t w = 0, h = 0, fmt = 0;
            uint64_t sz = 0;
            if (!ReadDdsHeaderMeta(r.filename, w, h, fmt, &sz)) continue;
            seen_meta.emplace(fkey, DdsHeaderCacheEntry{ w, h, fmt, sz });
            if (!IsMaterialTexDims(w, h)) continue;

            ModTexCandidate cand;
            cand.fq = r.fq;
            cand.name = r.name;
            cand.filename = r.filename;
            cand.comps = comps;
            cand.exclusive = (comps.size() == 1);
            cand.dxgi_fmt = fmt;
            cand.width = w;
            cand.height = h;
            cand.file_hash = ParseFilenameHash(r.filename);
            cand.file_size = sz;

            const size_t ci = built->all_dds.size();
            built->all_dds.push_back(std::move(cand));
            const ModTexCandidate& stored = built->all_dds[ci];
            for (int c : stored.comps) built->by_comp[c].push_back(ci);
            if (stored.exclusive) built->excl_by_comp_fmt[CompFmtKey(stored.comps[0], stored.dxgi_fmt)].push_back(ci);
            const std::wstring lfq = LowerStr(stored.fq);
            if (seen_fp_fqs.insert(lfq).second) fp_items.emplace_back(lfq, sz);
        }

        // Also index any [TextureOverride]-backed DDS resources living inside this mod's folder
        // (including sibling .ini files or non-component-named utility/mask-neutralizer DDS files).
        const std::wstring dir_prefix = NormalizeDirPrefix(mod.dir);
        if (!dir_prefix.empty()) {
            struct DirResCtx {
                std::wstring                                  dir_prefix;
                std::vector<std::pair<std::wstring, std::wstring>> items;
            } dctx{ dir_prefix, {} };
            PortApi::ForEachCustomResource(
                [](const wchar_t* fq_name, const wchar_t* filename, int, bool, ID3D12Resource*, void* user) {
                    if (!fq_name || !*fq_name || !filename || !*filename) return;
                    auto* c = static_cast<DirResCtx*>(user);
                    const std::wstring fn(filename);
                    if (!EndsWithNoCase(fn, L".dds")) return;
                    const std::wstring norm = NormalizePathLower(fn);
                    if (norm.size() <= c->dir_prefix.size() ||
                        norm.compare(0, c->dir_prefix.size(), c->dir_prefix) != 0)
                        return;
                    c->items.emplace_back(fq_name, fn);
                },
                &dctx);

            std::unordered_set<std::wstring> seen_extra_fqs;
            for (const auto& [fq, fn] : dctx.items) {
                const std::wstring lfq = LowerStr(fq);
                if (!seen_extra_fqs.insert(lfq).second) continue;
                std::vector<uint32_t> ovr_hashes = PortApi::TextureOverrideHashesOfResource(fq.c_str());
                if (ovr_hashes.empty()) continue;

                uint32_t w = 0, h = 0, fmt = 0;
                uint64_t sz = 0;
                const std::wstring nfn = NormalizePathLower(fn);
                auto               mit = seen_meta.find(nfn);
                if (mit != seen_meta.end()) {
                    w = mit->second.w;
                    h = mit->second.h;
                    fmt = mit->second.fmt;
                    sz = mit->second.sz;
                } else {
                    if (!ReadDdsHeaderMeta(fn, w, h, fmt, &sz)) continue;
                    seen_meta.emplace(nfn, DdsHeaderCacheEntry{ w, h, fmt, sz });
                }
                if (!IsMaterialTexDims(w, h) || !IsNeutralizerDdsFormat(fmt)) continue;

                const size_t       slash = fq.find_last_of(L"\\/");
                const std::wstring name = (slash == std::wstring::npos) ? fq : fq.substr(slash + 1);
                built->extra_override_dds.push_back(ExtraOverrideDds{ fq, name, fn, fmt, w, h, sz, std::move(ovr_hashes) });
                if (seen_fp_fqs.insert(lfq).second) fp_items.emplace_back(lfq, sz);
            }
        }

        std::sort(fp_items.begin(), fp_items.end());
        for (const auto& kv : fp_items) {
            fp = FnvMixStr(fp, kv.first);
            fp = FnvMixU64(fp, kv.second);
        }
        built->fingerprint = fp ? fp : 1ull;

        std::lock_guard l(mtx_);
        ResetIfNeededLocked(load);
        auto [it, inserted] = indices_.emplace(dm, built);
        if (inserted) {
            state_seq_.bump();
            if (!built->all_dds.empty() || !built->extra_override_dds.empty()) {
                logger::probe("TEX-STRUCT-INDEX gen=%u mod=%d guid=%u dds=%zu extra_dds=%zu excl_buckets=%zu fp=%016llx",
                    load, dm + 1, mod.object_guid, built->all_dds.size(), built->extra_override_dds.size(),
                    built->excl_by_comp_fmt.size(), static_cast<unsigned long long>(built->fingerprint));
            }
        }
        return it->second;
    }

    const PatchSig& GetOrDecodeDdsSig(const std::wstring& filename)
    {
        const std::wstring key = LowerStr(filename);
        auto               it = dds_sig_cache_.find(key);
        if (it != dds_sig_cache_.end()) return it->second;
        PatchSig sig;
        uint32_t w = 0, h = 0, fmt = 0;
        if (TexLookComputeDdsSig(filename.c_str(), &w, &h, &fmt,
                &sig.codec, &sig.nch, &sig.pw, &sig.ph, &sig.px, sig.rgb_mean, sig.rgb_std)) {
            sig.valid = true;
        }
        return dds_sig_cache_.emplace(key, std::move(sig)).first->second;
    }

    bool GetOrCheckDdsPureWhite(const std::wstring& filename)
    {
        const std::wstring key = LowerStr(filename);
        auto               it = dds_white_cache_.find(key);
        if (it != dds_white_cache_.end()) return it->second;
        const bool ok = TexLookIsDdsPureWhite(filename.c_str());
        dds_white_cache_.emplace(key, ok);
        return ok;
    }

    void RunNeutralizerProbe(const NeutralizerJob& job)
    {
        if (!job.idx) return;
        const ExtraOverrideDds* best = nullptr;
        for (const auto& cand : job.idx->extra_override_dds) {
            if (stop_.load(std::memory_order_acquire)) return;
            if (!GetOrCheckDdsPureWhite(cand.filename)) continue;
            if (!best) {
                best = &cand;
                continue;
            }
            const uint64_t area = static_cast<uint64_t>(cand.width) * cand.height;
            const uint64_t best_area = static_cast<uint64_t>(best->width) * best->height;
            if (area != best_area) {
                if (area > best_area) best = &cand;
            } else if (cand.override_hashes.size() != best->override_hashes.size()) {
                if (cand.override_hashes.size() > best->override_hashes.size()) best = &cand;
            } else if (_wcsicmp(cand.fq.c_str(), best->fq.c_str()) < 0) {
                best = &cand;
            }
        }
        NeutralizerState st{};
        st.probed = true;
        if (best) {
            st.found = true;
            st.fq = best->fq;
            st.name = best->name;
            st.override_hashes = best->override_hashes;
        }
        {
            std::lock_guard l(mtx_);
            if (gen_ != job.load) return;
            PublishSeq::Guard pub(state_seq_);
            neutralizers_[job.dm] = st;
        }
        if (st.found) {
            logger::probe("TEX-AUTO-ADAPT gen=%u mod=%d guid=%u cutout_neutralizer=%ls hashes=%zu",
                job.load, job.dm + 1, job.idx->object_guid, st.name.c_str(), st.override_hashes.size());
        }
    }

    void WorkerLoop()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            AdaptJob       job;
            bool           has_job = false;
            NeutralizerJob njob;
            bool           has_njob = false;
            {
                std::unique_lock ul(worker_mtx_);
                worker_cv_.wait(ul, [&]() { return stop_ || !neut_queue_.empty() || !queue_.empty() || flush_requested_; });
                if (stop_) break;
                if (!neut_queue_.empty()) {
                    njob = std::move(neut_queue_.front());
                    neut_queue_.pop_front();
                    if (queued_gen_ == njob.load) neut_queued_dms_.erase(njob.dm);
                    has_njob = true;
                } else if (!queue_.empty()) {
                    job = std::move(queue_.front());
                    queue_.pop_front();
                    if (queued_gen_ == job.load) queued_keys_.erase(DmCompKey(job.dm, job.comp));
                    has_job = true;
                }
                flush_requested_ = false;
            }
            if (has_njob) RunNeutralizerProbe(njob);
            if (has_job) RunCompVisualAdapt(job);
            FlushDiskCache();
        }
    }

    void RunCompVisualAdapt(const AdaptJob& job)
    {
        if (!job.idx) return;
        std::vector<ObsLod>                             obs;
        std::unordered_set<uint32_t>                    exact_anchors;
        std::unordered_set<uint64_t>                    cooccur;
        std::unordered_set<uint32_t>                    shared_hashes;
        std::unordered_map<std::wstring, StrongFqOwner> strong_owners;
        std::unordered_map<uint32_t, std::wstring>      persisted_fqs;
        const uint64_t                                  qk = DmCompKey(job.dm, job.comp);
        uint64_t                                        snap_ev_seq = 0;
        {
            std::lock_guard l(mtx_);
            if (gen_ != job.load) return;
            auto oit = observed_lods_.find(qk);
            if (oit == observed_lods_.end() || oit->second.empty()) return;
            if (auto ev_it = comp_evidence_seq_.find(qk); ev_it != comp_evidence_seq_.end())
                snap_ev_seq = ev_it->second;
            obs = oit->second;
            auto eit = exact_hits_.find(job.dm);
            if (eit != exact_hits_.end()) exact_anchors = eit->second;
            auto cit_co = cooccur_pairs_.find(DmCompKey(job.dm, job.comp));
            if (cit_co != cooccur_pairs_.end()) cooccur = cit_co->second;
            auto sit = shared_engine_hashes_.find(job.dm);
            if (sit != shared_engine_hashes_.end()) shared_hashes = sit->second;
            auto owit = strong_fq_owners_.find(job.dm);
            if (owit != strong_fq_owners_.end()) strong_owners = owit->second;
            for (const auto& e : obs) {
                auto dit = disk_cache_.find(DiskKey{ job.idx->fingerprint, job.comp, e.lod });
                if (dit != disk_cache_.end()) persisted_fqs.emplace(e.lod, Utf8ToWide(dit->second));
                const uint32_t parent = TexVisualParentOf(e.lod);
                if (parent && parent != e.lod) {
                    const DiskKey pdk{ job.idx->fingerprint, job.comp, parent };
                    auto          pvit = visual_resolved_.find(pdk);
                    if (pvit != visual_resolved_.end()) {
                        persisted_fqs.emplace(parent, pvit->second.fq);
                    } else {
                        auto pdit = disk_cache_.find(pdk);
                        if (pdit != disk_cache_.end()) persisted_fqs.emplace(parent, Utf8ToWide(pdit->second));
                    }
                }
            }
        }
        std::vector<uint8_t> obs_missing_sig;
        auto mark_evaluated = [&]() {
            if (!snap_ev_seq) return;
            std::lock_guard l(mtx_);
            if (gen_ != job.load) return;
            bool mutated = false;
            if (!obs_missing_sig.empty()) {
                auto oit = observed_lods_.find(qk);
                if (oit != observed_lods_.end()) {
                    const auto cur_ev = comp_evidence_seq_.find(qk);
                    const bool unchanged = (cur_ev != comp_evidence_seq_.end() && cur_ev->second == snap_ev_seq);
                    mutated |= ApplyWorkerSigAvailability(oit->second, obs, obs_missing_sig, unchanged);
                }
            }
            auto& ev = comp_evaluated_seq_[qk];
            if (ev < snap_ev_seq) {
                ev = snap_ev_seq;
                mutated = true;
            }
            if (mutated) state_seq_.bump();
        };
        if (!shared_hashes.empty()) {
            obs.erase(std::remove_if(obs.begin(), obs.end(),
                          [&](const ObsLod& e) { return shared_hashes.count(e.lod) != 0; }),
                obs.end());
            if (obs.empty()) { mark_evaluated(); return; }
        }
        // Collapse streaming sub-mip observations whose proven mip0 parent is already present
        // in obs or already resolved, so a lower mip never competes against its own parent
        // and scrambles twin assignments.
        {
            std::unordered_set<uint32_t> obs_keys;
            obs_keys.reserve(obs.size());
            for (const auto& e : obs) obs_keys.insert(e.lod);
            obs.erase(std::remove_if(obs.begin(), obs.end(), [&](const ObsLod& e) {
                const uint32_t parent = TexVisualParentOf(e.lod);
                if (!parent || parent == e.lod) return false;
                return obs_keys.count(parent) != 0 || persisted_fqs.count(parent) != 0;
            }),
                obs.end());
            if (obs.empty()) { mark_evaluated(); return; }
        }

        auto cit = job.idx->by_comp.find(job.comp);
        if (cit == job.idx->by_comp.end() || cit->second.empty()) { mark_evaluated(); return; }

        // Build component candidate pool (excluding invariant exact-hit anchors)
        std::vector<const ModTexCandidate*> pool;
        pool.reserve(cit->second.size());
        for (size_t ci : cit->second) {
            const ModTexCandidate& c = job.idx->all_dds[ci];
            if (c.file_hash && exact_anchors.count(c.file_hash)) continue;
            pool.push_back(&c);
        }
        if (pool.empty()) { mark_evaluated(); return; }

        // Decode 5-patch signatures (cached per file; ~1.3 MB seek-read per 8K DDS)
        std::vector<const PatchSig*> pool_sigs(pool.size(), nullptr);
        for (size_t ri = 0; ri < pool.size(); ++ri) {
            if (stop_.load(std::memory_order_acquire)) return;
            pool_sigs[ri] = &GetOrDecodeDdsSig(pool[ri]->filename);
        }

        // Copy canonical HD seed references OR live-texture 5-patch signatures for each observed key
        std::vector<PatchSig> obs_refs(obs.size());
        std::vector<uint32_t> eff_dims(obs.size());
        obs_missing_sig.assign(obs.size(), 0u);
        for (size_t gi = 0; gi < obs.size(); ++gi) {
            uint32_t rw = 0, rh = 0;
            if (TexLookCopyAnySig(obs[gi].lod, &obs_refs[gi].codec, &obs_refs[gi].nch,
                    &rw, &rh, &obs_refs[gi].pw, &obs_refs[gi].ph, &obs_refs[gi].px)) {
                const size_t   n = static_cast<size_t>(kPatches) * obs_refs[gi].pw * obs_refs[gi].ph;
                const uint32_t nch = obs_refs[gi].nch;
                if (n > 0 && nch > 0 && obs_refs[gi].px.size() >= n * nch) {
                    const uint32_t ch_lim = (std::min)(nch, 3u);
                    double         s[3]{}, ss[3]{};
                    for (size_t i = 0; i < n; ++i) {
                        for (uint32_t c = 0; c < ch_lim; ++c) {
                            const double v = static_cast<double>(obs_refs[gi].px[i * nch + c]);
                            s[c] += v;
                            ss[c] += v * v;
                        }
                    }
                    for (uint32_t c = 0; c < ch_lim; ++c) {
                        const double m = s[c] / static_cast<double>(n);
                        const double var = (std::max)(0.0, ss[c] / static_cast<double>(n) - m * m);
                        obs_refs[gi].rgb_mean[c] = m;
                        obs_refs[gi].rgb_std[c] = std::sqrt(var);
                    }
                    obs_refs[gi].valid = !obs_refs[gi].is_flat();
                }
            } else {
                obs_missing_sig[gi] = 1u;
            }
            eff_dims[gi] = (std::max)(obs[gi].max_dim, (std::max)(rw, rh));
        }

        std::vector<std::vector<CandEval>> evals(obs.size());

        for (size_t gi = 0; gi < obs.size(); ++gi) {
            if (!obs_refs[gi].valid) continue;
            const uint32_t vfmt = obs[gi].vfmt;

            std::vector<size_t> compat;
            bool                has_excl_same = false;
            bool                has_excl_nonflat = false;
            for (size_t ri = 0; ri < pool.size(); ++ri) {
                if (pool[ri]->dxgi_fmt == vfmt) {
                    compat.push_back(ri);
                    if (pool[ri]->exclusive) {
                        has_excl_same = true;
                        if (pool_sigs[ri] && pool_sigs[ri]->valid && !pool_sigs[ri]->is_flat())
                            has_excl_nonflat = true;
                    }
                }
            }
            // Universal same-color-space cross-BC author re-save compatibility
            // (e.g. native BC1_UNORM/BC3_UNORM ID/mask/lightmap re-exported by a mod author as BC7_UNORM,
            //  or BC1/BC3_UNORM_SRGB re-exported as BC7_UNORM_SRGB).
            if (!has_excl_same) {
                for (size_t ri = 0; ri < pool.size(); ++ri) {
                    if (pool[ri]->exclusive && CrossBcResaveCompatible(vfmt, pool[ri]->dxgi_fmt) &&
                        pool_sigs[ri] && pool_sigs[ri]->valid && !pool_sigs[ri]->is_flat()) {
                        compat.push_back(ri);
                        has_excl_nonflat = true;
                    }
                }
            }

            for (size_t ri : compat) {
                const ModTexCandidate* c = pool[ri];
                const PatchSig*        csig = pool_sigs[ri];
                if (!csig || !csig->valid) continue;

                const bool flat = csig->is_flat();
                const bool exact_fmt = (c->dxgi_fmt == vfmt);
                double     ssim = -1.0;
                double     hist = 0.0;
                double     comb = 0.0;
                if (!flat) {
                    auto sc = ComparePatchSigs(*csig, obs_refs[gi]);
                    ssim = sc.first;
                    hist = sc.second;
                    comb = 0.60 * (std::max)(0.0, ssim) + 0.40 * hist;
                    if (exact_fmt) comb += 0.05;
                    if (c->exclusive) comb += 0.03;
                }

                // Exclude non-exclusive candidates already strongly claimed (SSIM >= 0.55) by another component
                // for a different game texture hash unless this observation itself is a direct high-SSIM match
                // (prevents e.g. 6-component hair DDS from hijacking torso while allowing streaming mip upgrades
                // of a shared multi-component texture to bind cleanly regardless of component worker ordering).
                if (!c->exclusive) {
                    auto oit = strong_owners.find(LowerStr(c->fq));
                    if (oit != strong_owners.end() &&
                        ShouldExcludeCrossCompStrongOwner(c->exclusive, flat, ssim,
                            oit->second.comp, oit->second.key, job.comp, obs[gi].lod))
                        continue;
                }

                // Order-independent exclusive-first gating: when this component already has non-flat exclusive
                // DDS candidates for this format, multi-component shared files (whether 2-comp or >=3-comp)
                // may compete only if they show strong visual similarity (SSIM >= 0.45 && combined >= 0.50).
                // This prevents low-score shared detail/FX files from stealing a completely re-UV'd exclusive
                // outfit diffuse/lightmap slot.
                if (has_excl_nonflat && !c->exclusive) {
                    if (flat || ssim < 0.45 || comb < 0.50) continue;
                }

                evals[gi].push_back(CandEval{ static_cast<int>(ri), ssim, hist, comb, exact_fmt, c->exclusive, flat });
            }
            std::sort(evals[gi].begin(), evals[gi].end(),
                [](const CandEval& a, const CandEval& b) { return a.combined > b.combined; });
        }

        const auto twins = FindTwinGroups(pool_sigs, [&](size_t ri, size_t rj) {
            return pool[ri]->exclusive && pool[rj]->exclusive && pool[ri]->dxgi_fmt == pool[rj]->dxgi_fmt;
        });
        auto cooccurs = [&](size_t gi, size_t gj) {
            return cooccur.count(PairKey(obs[gi].lod, obs[gj].lod)) != 0;
        };
        const auto match = SolveVisualAssignment(evals, obs_refs, eff_dims, twins, cooccurs);
        const auto& assigned = match.assigned;

        // Evaluate confidence + twin-variant / direct-SSIM owner reconciliation and publish resolved entries
        std::vector<std::tuple<uint32_t, VisualResolvedEntry, double, double, bool>> resolved_batch;
        std::vector<std::pair<uint32_t, const std::wstring*>> revoked_bindings;
        for (size_t gi = 0; gi < obs.size(); ++gi) {
            const int chosen_ri = assigned[gi];

            // An occupied candidate or twin cannot be a rival in the same draw.
            auto taken_by_cooccur = [&](int ri) -> bool {
                for (size_t gj = 0; gj < obs.size(); ++gj) {
                    if (gj == gi || assigned[gj] < 0 || twins.cluster[assigned[gj]] != twins.cluster[ri]) continue;
                    if (cooccurs(gi, gj)) return true;
                }
                return false;
            };

            const CandEval*              chosen = nullptr;
            unsigned                     excl_same_all = 0;
            std::vector<const CandEval*> others;
            for (const auto& ev : evals[gi]) {
                if (ev.exclusive && ev.exact_fmt) ++excl_same_all;
                if (ev.ri == chosen_ri) {
                    chosen = &ev;
                } else {
                    const bool claimed_direct = (ev.ssim < kDirectSsim) &&
                        IsClaimedByOtherDirectSsim(gi, ev.ri, evals, assigned);
                    if (!ev.is_flat && (chosen_ri >= 0 || twins.colors[ev.ri] || claimed_direct)) {
                        if (match.blocked[gi][ev.ri] || taken_by_cooccur(ev.ri) || claimed_direct) {
                            if (twins.colors[ev.ri] || claimed_direct)
                                revoked_bindings.emplace_back(obs[gi].lod, &pool[ev.ri]->fq);
                        } else if (chosen_ri >= 0) {
                            others.push_back(&ev);
                        }
                    }
                }
            }
            if (!chosen) continue;

            const double margin = chosen->combined - (others.empty() ? 0.0 : others[0]->combined);
            bool         twin_pair = false;
            double       non_twin_margin = margin;
            if (margin < 0.05 && !others.empty() && pool_sigs[chosen_ri]->valid) {
                double max_non_twin = 0.0;
                bool   saw_twin = false;
                for (const CandEval* oe : others) {
                    if (twins.cluster[chosen_ri] == twins.cluster[oe->ri] ||
                        IsTwinSimilarity(ComparePatchSigs(*pool_sigs[chosen_ri], *pool_sigs[oe->ri]))) {
                        saw_twin = true;
                        continue;
                    }
                    if (oe->combined > max_non_twin) max_non_twin = oe->combined;
                }
                if (saw_twin) {
                    twin_pair = true;
                    non_twin_margin = chosen->combined - max_non_twin;
                }
            }

            const char* tier = VisualTier(*chosen, excl_same_all, margin, twin_pair, non_twin_margin, !others.empty());
            if (!tier) {
                logger::probe("TEX-AUTO-ADAPT gen=%u mod=%d comp=%d lod=%08x vfmt=%u action=skip_ambiguous top=%ls score=%.3f margin=%.3f",
                    job.load, job.dm + 1, job.comp, obs[gi].lod, obs[gi].vfmt,
                    pool[chosen_ri]->name.c_str(), chosen->combined, margin);
                continue;
            }

            const ModTexCandidate* win = pool[chosen_ri];
            // Preserve a previously persisted weak-twin binding across F10 / single-form reloads
            // when the other form has not been observed yet (unless the old twin was claimed by
            // another row in this observation set or overridden by a direct high-SSIM match).
            if (chosen->ssim < kDirectSsim) {
                auto pit = persisted_fqs.find(obs[gi].lod);
                if (pit != persisted_fqs.end() && _wcsicmp(pit->second.c_str(), win->fq.c_str()) != 0) {
                    int old_ri = -1;
                    for (size_t ri = 0; ri < pool.size(); ++ri) {
                        if (_wcsicmp(pool[ri]->fq.c_str(), pit->second.c_str()) == 0) {
                            old_ri = static_cast<int>(ri);
                            break;
                        }
                    }
                    if (old_ri >= 0 && !match.blocked[gi][old_ri] &&
                        twins.cluster[old_ri] == twins.cluster[chosen_ri]) {
                        bool claimed_by_other = false;
                        for (size_t gj = 0; gj < obs.size(); ++gj) {
                            if (gj != gi && assigned[gj] == old_ri) {
                                claimed_by_other = true;
                                break;
                            }
                        }
                        if (!claimed_by_other) win = pool[old_ri];
                    }
                }
            }
            resolved_batch.emplace_back(
                obs[gi].lod,
                VisualResolvedEntry{ win->fq, win->name, static_cast<float>(chosen->combined), tier },
                chosen->ssim,
                chosen->hist,
                win->exclusive);
        }

        if (resolved_batch.empty() && revoked_bindings.empty()) { mark_evaluated(); return; }
        std::vector<int> requeue_others;
        {
            std::lock_guard l(mtx_);
            if (gen_ != job.load) return;
            const auto shared = shared_engine_hashes_.find(job.dm);
            bool       mutated = false;

            // A weak binding published before a larger mip, co-occurrence, or direct-SSIM owner
            // appeared must not survive via the visual, disk, or anti-flicker proven-slot cache.
            for (const auto& [lod, fq] : revoked_bindings) {
                const DiskKey dk{ job.idx->fingerprint, job.comp, lod };
                const auto prev = visual_resolved_.find(dk);
                const auto disk = disk_cache_.find(dk);
                if (prev == visual_resolved_.end() && disk == disk_cache_.end()) continue;
                const bool was_visual = prev != visual_resolved_.end() && _wcsicmp(prev->second.fq.c_str(), fq->c_str()) == 0;
                const std::string fq_u8 = WideToUtf8(*fq);
                const bool was_cached = disk != disk_cache_.end() && _stricmp(disk->second.c_str(), fq_u8.c_str()) == 0;
                if (!(was_visual || was_cached)) continue;
                mutated = true;
                if (was_visual) visual_resolved_.erase(prev);
                if (was_cached) {
                    disk_cache_.erase(disk);
                    disk_dirty_ = true;
                }
                std::erase_if(proven_slots_, [&](const auto& kv) {
                    return kv.first.dm == job.dm && kv.first.comp == job.comp &&
                        _wcsicmp(kv.second.fq.c_str(), fq->c_str()) == 0 &&
                        ShouldRevokeSlotForSource(kv.second.source_lod, lod);
                });
                if (std::erase_if(disk_slot_cache_, [&](const auto& kv) {
                        return kv.first.fp == dk.fp && kv.first.comp == job.comp &&
                            _stricmp(kv.second.fq.c_str(), fq_u8.c_str()) == 0 &&
                            ShouldRevokeSlotForSource(kv.second.source_lod, lod);
                    }))
                    disk_dirty_ = true;
                logger::probe("TEX-AUTO-ADAPT gen=%u mod=%d guid=%u comp=%d lod=%08x action=revoke_displaced -> %s",
                    job.load, job.dm + 1, job.idx->object_guid, job.comp, lod, fq_u8.c_str());
            }
            auto& owner_map = strong_fq_owners_[job.dm];
            bool  new_strong_shared = false;
            for (const auto& item : resolved_batch) {
                const uint32_t             lod = std::get<0>(item);
                if (shared != shared_engine_hashes_.end() && shared->second.count(lod)) continue;
                const VisualResolvedEntry& ent = std::get<1>(item);
                const double               ssim = std::get<2>(item);
                const bool                 is_excl = std::get<4>(item);
                const DiskKey              dk{ job.idx->fingerprint, job.comp, lod };
                auto                       prev = visual_resolved_.find(dk);
                const bool                 changed = (prev == visual_resolved_.end() || prev->second.fq != ent.fq);
                const bool                 tier_changed = (prev == visual_resolved_.end() || prev->second.tier != ent.tier);
                if (changed || tier_changed) mutated = true;
                visual_resolved_[dk] = ent;
                const std::string          fq_u8 = WideToUtf8(ent.fq);
                auto                       dp = disk_cache_.find(dk);
                if (dp == disk_cache_.end() || dp->second != fq_u8) {
                    disk_cache_[dk] = fq_u8;
                    disk_dirty_ = true;
                    mutated = true;
                }
                if (!is_excl && ssim >= 0.55) {
                    const std::wstring lfq = LowerStr(ent.fq);
                    auto               oit = owner_map.find(lfq);
                    if (oit == owner_map.end() || ssim > oit->second.ssim + 0.02) {
                        owner_map[lfq] = StrongFqOwner{ job.comp, lod, ssim };
                        new_strong_shared = true;
                        mutated = true;
                    }
                }
                if (changed) {
                    logger::probe("TEX-AUTO-ADAPT gen=%u mod=%d guid=%u comp=%d lod=%08x tier=%s score=%.3f ssim=%.3f hist=%.2f -> %ls",
                        job.load, job.dm + 1, job.idx->object_guid, job.comp, lod,
                        ent.tier.c_str(), ent.score, ssim, std::get<3>(item), ent.name.c_str());
                }
            }
            if (!obs_missing_sig.empty()) {
                auto oit = observed_lods_.find(qk);
                if (oit != observed_lods_.end()) {
                    const auto cur_ev = comp_evidence_seq_.find(qk);
                    const bool unchanged = (cur_ev != comp_evidence_seq_.end() && cur_ev->second == snap_ev_seq);
                    mutated |= ApplyWorkerSigAvailability(oit->second, obs, obs_missing_sig, unchanged);
                }
            }
            if (snap_ev_seq) {
                auto& ev = comp_evaluated_seq_[qk];
                if (ev < snap_ev_seq) {
                    ev = snap_ev_seq;
                    mutated = true;
                }
            }
            if (new_strong_shared) {
                for (const auto& kv : observed_lods_) {
                    const int dm_k = static_cast<int>(kv.first >> 32);
                    const int comp_k = static_cast<int>(kv.first & 0xffffffffu);
                    if (dm_k == job.dm && comp_k != job.comp && !kv.second.empty()) {
                        ++comp_evidence_seq_[kv.first];
                        requeue_others.push_back(comp_k);
                    }
                }
            }
            if (mutated) state_seq_.bump();
        }
        for (int other_c : requeue_others) EnqueueCompAdapt(job.load, job.dm, other_c, job.idx);
    }

    void LogDecisionOnce(unsigned load, int dm, int comp, unsigned reg, uint32_t vfmt, const char* action,
        unsigned slot_cnt, unsigned cand_cnt, const std::wstring& res_name,
        uint32_t guid = 0, uint32_t w = 0, uint32_t h = 0, uint32_t hash = 0, uint32_t lod = 0)
    {
        std::lock_guard l(mtx_);
        ResetIfNeededLocked(load);
        if (logged_.size() >= 384) return;
        if (!logged_.emplace(load, dm, comp, reg, vfmt, action).second) return;
        if (res_name.empty()) {
            logger::probe("TEX-STRUCT-FALLBACK gen=%u mod=%d comp=%d t%u vfmt=%u res=%ux%u hash=%08x lod=%08x action=%s slots=%u cands=%u",
                load, dm + 1, comp, reg, vfmt, w, h, hash, lod, action, slot_cnt, cand_cnt);
        } else {
            logger::probe("TEX-STRUCT-FALLBACK gen=%u mod=%d guid=%u comp=%d t%u vfmt=%u res=%ux%u hash=%08x lod=%08x action=%s -> %ls",
                load, dm + 1, guid, comp, reg, vfmt, w, h, hash, lod, action, res_name.c_str());
        }
    }
};

inline StructFallbackEngine g_struct_fallback;

} // namespace wwmi_struct_tex
