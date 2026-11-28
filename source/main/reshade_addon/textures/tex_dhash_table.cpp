// CPU texture fingerprints for native [Filter] matching.
#include "tex_dhash_table.h"
#include "tex_dhash.h"
#include "../instrumentation/profiler.h"
#include "../migoto/port_api.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <smmintrin.h>

bool g_tex_dhash_filters = true;
TexDHashTable g_tex_dhash;

namespace {

bool Wanted(const D3D12_RESOURCE_DESC& d, uint32_t sub)
{
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
        d.SampleDesc.Count == 1 && sub < d.MipLevels && texdhash::Decodes(uint32_t(d.Format));
}

struct DHashMipLayout {
    uint64_t tight_row = 0;
    uint32_t rows = 0;
    uint64_t pitch = 0;

    DHashMipLayout(uint32_t w, uint32_t h, uint32_t fmt)
    {
        if (texdhash::RowLayout(w, h, fmt, tight_row, rows)) {
            // RowLayout starts from 32-bit dimensions; rounding up cannot overflow uint64_t.
            constexpr uint64_t alignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
            pitch = (tight_row + alignment - 1) & ~(alignment - 1);
        }
    }

    bool Fits(uint64_t row_pitch, uint64_t available) const
    {
        // Equivalent to (rows - 1) * row_pitch + tight_row <= available, without overflow.
        return rows > 0 && tight_row > 0 && row_pitch >= tight_row && available >= tight_row &&
            uint64_t(rows - 1) <= (available - tight_row) / row_pitch;
    }
};

bool HashMip(const texdhash::Mip& m, uint64_t& linear, uint64_t& srgb)
{
    if (!texdhash::Hash(m, false, &linear)) return false;
    srgb = linear;
    return !texdhash::HasSrgbView(m.fmt) || texdhash::Hash(m, true, &srgb);
}

#if defined(__clang__)
__attribute__((target("sse4.1")))
#endif
void CopyUploadRow(uint8_t* dst, const uint8_t* src, size_t n)
{
    // UPLOAD memory is write-combined. Stream the aligned part and leave no
    // vector loads crossing the row end (the last row need not be padded).
    for (; n && (reinterpret_cast<uintptr_t>(src) & 15); --n) *dst++ = *src++;
    for (; n >= 16; n -= 16, src += 16, dst += 16) {
        const __m128i v = _mm_stream_load_si128(const_cast<__m128i*>(reinterpret_cast<const __m128i*>(src)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), v);
    }
    std::memcpy(dst, src, n);
}

bool HashUpload(ID3D12Resource* src, uint64_t offset, const DHashMipLayout& l,
    const TexDHashTable::UploadFootprint* fp, texdhash::Mip mip, uint64_t& linear, uint64_t& srgb)
{
    if (!src) return false;
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS flags{};
    if (FAILED(src->GetHeapProperties(&hp, &flags)) || hp.Type != D3D12_HEAP_TYPE_UPLOAD) return false;
    const uint64_t pitch = (fp && fp->row_pitch) ? uint64_t(fp->row_pitch) : l.pitch;
    if (!pitch || pitch < l.tight_row) return false;
    uint64_t origin_off = 0;
    if (fp && fp->has_src_box && (fp->src_left || fp->src_top || fp->src_front)) {
        if (!texdhash::UploadRegionOffset(mip.fmt, fp->footprint_height, fp->src_left, fp->src_top,
                fp->src_front, pitch, l.tight_row, origin_off))
            return false;
    }
    const auto desc = src->GetDesc();
    const uint64_t limit = (std::min)(desc.Width, uint64_t((std::numeric_limits<size_t>::max)()));
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || offset > limit || origin_off > limit - offset)
        return false;
    const uint64_t base_off = offset + origin_off;
    if (!l.Fits(pitch, limit - base_off))
        return false;

    PROF(PROF_CB_DHash);
    std::vector<uint32_t> rows;
    texdhash::RowsRead(mip.w, mip.h, mip.fmt, [&](uint32_t r) { rows.push_back(r); });
    if (rows.empty() || rows.back() >= l.rows) return false;
    const size_t tight = static_cast<size_t>(l.tight_row);
    std::vector<uint8_t> bytes(rows.size() * tight);
    const D3D12_RANGE read_range{
        static_cast<size_t>(base_off + rows.front() * pitch),
        static_cast<size_t>(base_off + rows.back() * pitch + tight)
    };
    void* mapped = nullptr;
    if (FAILED(src->Map(0, &read_range, &mapped))) return false;
    if (mapped) {
        const auto* base = static_cast<const uint8_t*>(mapped) + base_off;
        for (size_t i = 0; i < rows.size(); ++i)
            CopyUploadRow(bytes.data() + i * tight, base + rows[i] * pitch, tight);
    }
    const D3D12_RANGE no_writes{};
    src->Unmap(0, &no_writes);
    if (!mapped) return false;

    mip.row = [&](uint32_t r) -> const uint8_t* {
        auto it = std::lower_bound(rows.begin(), rows.end(), r);
        return it != rows.end() && *it == r ? bytes.data() + (it - rows.begin()) * tight : nullptr;
    };
    return HashMip(mip, linear, srgb);
}

} // namespace

void TexDHashTable::SetEnabled(bool enabled)
{
    std::lock_guard lock(mutex_);
    enable_gen_.fetch_add(1, std::memory_order_relaxed);
    enabled_.store(enabled, std::memory_order_release);
    if (!enabled) by_res_.clear();
}

void TexDHashTable::Store(uint64_t res, Entry entry, uint64_t enable_gen)
{
    std::lock_guard lock(mutex_);
    if (!Enabled() || enable_gen_.load(std::memory_order_relaxed) != enable_gen) return;
    ResState& st = by_res_[res];
    entry.gen = ++st.resource_gen;
    for (Entry& e : st.mips) {
        if (e.w == entry.w && e.h == entry.h) {
            e = entry;
            return;
        }
    }
    st.mips.push_back(entry);
}

void TexDHashTable::Drop(uint64_t res, uint32_t w, uint32_t h, uint64_t enable_gen)
{
    std::lock_guard lock(mutex_);
    if (enable_gen_.load(std::memory_order_relaxed) != enable_gen) return;
    auto it = by_res_.find(res);
    if (it == by_res_.end()) return;
    auto& mips = it->second.mips;
    const size_t before = mips.size();
    mips.erase(std::remove_if(mips.begin(), mips.end(), [&](const Entry& e) { return e.w == w && e.h == h; }), mips.end());
    if (mips.empty()) {
        by_res_.erase(it);
    } else if (mips.size() != before) {
        ++it->second.resource_gen;
    }
}

void TexDHashTable::FromUpload(ID3D12Resource* src, uint64_t offset, ID3D12Resource* dst, uint32_t sub,
    const UploadFootprint* fp)
{
    if (!dst) return;
    const uint64_t res = reinterpret_cast<uint64_t>(dst);
    const uint64_t gen = enable_gen_.load(std::memory_order_acquire);
    if (!Enabled() || !PortApi::TextureDHashesWanted()) return;
    const auto d = dst->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sub >= d.MipLevels) return;
    Entry e{DHashMipSide(d.Width, sub), DHashMipSide(d.Height, sub)};
    texdhash::Mip m{e.w, e.h, uint32_t(d.Format), {}};
    if (Wanted(d, sub) && HashUpload(src, offset, DHashMipLayout(m.w, m.h, m.fmt), fp, m, e.linear, e.srgb))
        Store(res, e, gen);
    else
        Drop(res, e.w, e.h, gen);
}

void TexDHashTable::FromData(ID3D12Resource* dst, uint32_t sub, const uint8_t* data, size_t row_pitch)
{
    if (!dst) return;
    const uint64_t res = reinterpret_cast<uint64_t>(dst);
    const uint64_t gen = enable_gen_.load(std::memory_order_acquire);
    if (!Enabled() || !PortApi::TextureDHashesWanted()) return;
    const auto d = dst->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sub >= d.MipLevels) return;
    Entry e{DHashMipSide(d.Width, sub), DHashMipSide(d.Height, sub)};
    const DHashMipLayout l(e.w, e.h, uint32_t(d.Format));
    const texdhash::Mip m{e.w, e.h, uint32_t(d.Format), [&](uint32_t r) -> const uint8_t* {
        return r < l.rows ? data + size_t(r) * row_pitch : nullptr;
    }};
    if (data && Wanted(d, sub) && l.Fits(row_pitch, (std::numeric_limits<size_t>::max)()) && HashMip(m, e.linear, e.srgb))
        Store(res, e, gen);
    else
        Drop(res, e.w, e.h, gen);
}

void TexDHashTable::Copy(uint64_t src, uint32_t src_sub, uint64_t dst, uint32_t dst_sub)
{
    if (!dst) return;
    const uint64_t gen = enable_gen_.load(std::memory_order_acquire);
    if (!Enabled()) return;
    if (!src) {
        Unknown(dst, dst_sub);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (!by_res_.count(src) && !by_res_.count(dst)) return;
    }
    const auto sd = reinterpret_cast<ID3D12Resource*>(src)->GetDesc();
    const auto dd = reinterpret_cast<ID3D12Resource*>(dst)->GetDesc();
    if (dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dst_sub >= dd.MipLevels) return;
    const uint32_t w = DHashMipSide(dd.Width, dst_sub);
    const uint32_t h = DHashMipSide(dd.Height, dst_sub);
    if (Wanted(dd, dst_sub) && sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && src_sub < sd.MipLevels &&
        DHashMipSide(sd.Width, src_sub) == w && DHashMipSide(sd.Height, src_sub) == h) {
        Entry entry{w, h};
        bool found = false;
        {
            std::lock_guard lock(mutex_);
            auto it = by_res_.find(src);
            if (it != by_res_.end()) {
                for (const Entry& e : it->second.mips) {
                    if (e.w != w || e.h != h) continue;
                    entry = e;
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            Store(dst, entry, gen);
            return;
        }
    }
    Drop(dst, w, h, gen);
}

void TexDHashTable::CopyAll(uint64_t src, uint64_t dst)
{
    if (!src || !dst || src == dst) return;
    std::lock_guard lock(mutex_);
    auto it = by_res_.find(src);
    if (!Enabled() || it == by_res_.end()) {
        by_res_.erase(dst);
        return;
    }
    const std::vector<Entry> copied = it->second.mips;
    ResState& dst_st = by_res_[dst];
    const uint64_t next_gen = dst_st.resource_gen + 1;
    dst_st.resource_gen = next_gen;
    dst_st.mips = copied;
    for (Entry& e : dst_st.mips) e.gen = next_gen;
}

void TexDHashTable::Unknown(uint64_t res, uint32_t sub)
{
    if (!res) return;
    const uint64_t gen = enable_gen_.load(std::memory_order_acquire);
    if (!Enabled()) return;
    {
        std::lock_guard lock(mutex_);
        if (!by_res_.count(res)) return;
    }
    const auto d = reinterpret_cast<ID3D12Resource*>(res)->GetDesc();
    if (sub < d.MipLevels) Drop(res, DHashMipSide(d.Width, sub), DHashMipSide(d.Height, sub), gen);
}

void TexDHashTable::Forget(uint64_t res)
{
    std::lock_guard lock(mutex_);
    by_res_.erase(res);
}

bool TexDHashTable::Lookup(ID3D12Resource* res, uint32_t mip, uint32_t view_fmt, uint64_t* out)
{
    if (!Enabled() || !res) return false;
    const auto d = res->GetDesc();
    if (mip >= d.MipLevels) return false;
    const uint32_t w = DHashMipSide(d.Width, mip);
    const uint32_t h = DHashMipSide(d.Height, mip);
    const bool srgb = texdhash::IsSrgb(view_fmt);
    std::lock_guard lock(mutex_);
    if (!Enabled()) return false;
    auto it = by_res_.find(reinterpret_cast<uint64_t>(res));
    if (it == by_res_.end()) return false;
    for (const Entry& e : it->second.mips) {
        if (e.w == w && e.h == h) {
            const uint64_t hash = srgb ? e.srgb : e.linear;
            if (out) *out = hash;
            it->second.has_cached = true;
            it->second.cached_srgb = srgb;
            it->second.cached_dhash = hash;
            it->second.cached_gen = it->second.resource_gen;
            return true;
        }
    }
    return false;
}

bool TexDHashTable::State(ID3D12Resource* res, uint32_t view_fmt, PortApi::TextureDHashState* out)
{
    if (!out) return false;
    *out = PortApi::TextureDHashState{};
    if (!Enabled() || !res) return false;
    const auto d = res->GetDesc();
    const bool srgb = texdhash::IsSrgb(view_fmt);
    std::lock_guard lock(mutex_);
    if (!Enabled()) return false;
    auto it = by_res_.find(reinterpret_cast<uint64_t>(res));
    if (it == by_res_.end()) return false;
    const ResState& st = it->second;
    out->resource_generation = st.resource_gen;
    for (uint32_t m = 0; m < d.MipLevels && m < 32; ++m) {
        const uint32_t w = DHashMipSide(d.Width, m);
        const uint32_t h = DHashMipSide(d.Height, m);
        for (const Entry& e : st.mips) {
            if (e.w == w && e.h == h) {
                out->mip_generations[m] = e.gen;
                break;
            }
        }
    }
    if (st.has_cached && st.cached_srgb == srgb) {
        out->has_cached = true;
        out->cached_dhash = st.cached_dhash;
        out->cached_generation = st.cached_gen;
    }
    return true;
}

bool TexDHashLookup(ID3D12Resource* res, unsigned mip, unsigned view_format, uint64_t* out)
{
    return g_tex_dhash.Lookup(res, mip, view_format, out);
}

bool TexDHashState(ID3D12Resource* res, unsigned view_format, PortApi::TextureDHashState* out)
{
    return g_tex_dhash.State(res, view_format, out);
}
