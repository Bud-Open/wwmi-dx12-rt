// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../../src/dxgi_bc.h"
#include "wwmi_upload_footprint.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace wwmi_tex_upload {

struct UploadMipLayout {
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t rows = 0;
    uint32_t tight_row = 0;
    uint32_t pitch = 0;
    uint32_t bc_block_bytes = 0; // 8 or 16 for BC1..BC7, 0 for uncompressed
    uint32_t pixel_bytes = 0;    // 1/2/4/8/16 for uncompressed, 0 for BC
    bool     floor = false;
};

struct DstCopyBox {
    uint32_t left = 0;
    uint32_t top = 0;
    uint32_t front = 0;
    uint32_t right = 0;
    uint32_t bottom = 0;
    uint32_t back = 0;
};

struct ResolvedUploadLayout {
    uint64_t base_offset = 0;   // source_offset + origin_offset
    uint64_t origin_offset = 0; // byte offset from placed footprint origin to src_box origin
    uint64_t row_pitch = 0;     // validated source row pitch (>= tight_row, element-aligned)
    uint64_t span_bytes = 0;    // (rows - 1) * row_pitch + tight_row (no trailing padding after last row)
    bool     from_host_footprint = false;
};

inline bool ComputeMipLayout(
    uint32_t tex_width,
    uint32_t tex_height,
    uint32_t mip_levels,
    uint32_t mip,
    uint32_t dxgi_fmt,
    uint64_t floor_px,
    UploadMipLayout& out) noexcept
{
    out = {};
    if (!tex_width || !tex_height || mip >= mip_levels) return false;
    const uint32_t bb = DxgiBcBlockBytes(dxgi_fmt);
    const uint32_t bpp = bb ? 0u : DxgiPlainBytesPerPixel(dxgi_fmt);
    if (!bb && !bpp) return false;

    const uint32_t w = (mip < 32u) ? (std::max)(1u, tex_width >> mip) : 1u;
    const uint32_t h = (mip < 32u) ? (std::max)(1u, tex_height >> mip) : 1u;
    const uint64_t rows64 = bb ? (uint64_t(h) + 3u) / 4u : uint64_t(h);
    const uint64_t tight64 = bb ? ((uint64_t(w) + 3u) / 4u) * uint64_t(bb) : uint64_t(w) * uint64_t(bpp);
    if (!rows64 || !tight64 || rows64 > UINT32_MAX || tight64 > UINT32_MAX - 255u) return false;

    out.w = w;
    out.h = h;
    out.rows = static_cast<uint32_t>(rows64);
    out.tight_row = static_cast<uint32_t>(tight64);
    out.pitch = static_cast<uint32_t>((tight64 + 255u) & ~255ull);
    out.bc_block_bytes = bb;
    out.pixel_bytes = bpp;
    out.floor = uint64_t(w) * uint64_t(h) >= floor_px;
    return true;
}

// Legacy CRC-32C destination coverage gate (kept independent of FullDHashMip):
// - Destination origin must be (0, 0, 0) with depth 1 (back == 1).
// - Uncompressed formats require exact mip extent (w, h).
// - BC formats accept either logical mip extent (w, h) or 4x4 block-rounded full coverage
//   (((w + 3) & ~3), ((h + 3) & ~3)) as emitted by D3D12 GetCopyableFootprints when pSrcBox is null
//   or block-aligned.
inline bool LegacyDstBoxCoversFullMip(
    const UploadMipLayout& L,
    const DstCopyBox* dst_box,
    uint32_t& copy_w,
    uint32_t& copy_h) noexcept
{
    copy_w = 0;
    copy_h = 0;
    if (!L.w || !L.h) return false;
    if (!dst_box) {
        copy_w = L.w;
        copy_h = L.h;
        return true;
    }
    if (dst_box->left != 0u || dst_box->top != 0u || dst_box->front != 0u || dst_box->back != 1u)
        return false;
    if (L.bc_block_bytes != 0u) {
        const uint64_t rw = (uint64_t(L.w) + 3u) & ~3ull;
        const uint64_t rh = (uint64_t(L.h) + 3u) & ~3ull;
        if ((uint64_t(dst_box->right) != L.w && uint64_t(dst_box->right) != rw) ||
            (uint64_t(dst_box->bottom) != L.h && uint64_t(dst_box->bottom) != rh))
            return false;
    } else {
        if (dst_box->right != L.w || dst_box->bottom != L.h)
            return false;
    }
    copy_w = dst_box->right;
    copy_h = dst_box->bottom;
    return true;
}

// Resolve effective row pitch for Upload/Readback arrival (0 falls back to L.pitch;
// non-zero values smaller than L.tight_row are rejected by returning 0).
inline size_t ResolveEffectiveRowPitch(size_t stored_row_pitch, const UploadMipLayout& L) noexcept
{
    if (!L.tight_row || !L.pitch || L.pitch < L.tight_row) return 0;
    const size_t pitch = stored_row_pitch ? stored_row_pitch : size_t(L.pitch);
    return pitch >= L.tight_row ? pitch : 0;
}

// Validate and resolve the D3D12 upload buffer layout for a full-mip copy.
// - When host_fp is nullptr (host metadata absent/unavailable), falls back to D3D12's
//   minimal 256-aligned pitch (L.pitch) and zero source-box origin.
// - When host_fp is non-null (host metadata present), strictly validates ABI header,
//   element-aligned row_pitch, full declared placed footprint bounds in src_buffer_bytes,
//   and (if has_src_box) source-box origin & actual copy extent (copy_w, copy_h) with
//   BC 4x4 block coordinates and edge rules. NEVER falls back on invalid metadata.
// - Always checks overflow-safe base_offset = source_offset + origin_offset and
//   span = (rows - 1) * row_pitch + tight_row against src_buffer_bytes (does NOT require
//   row-pitch padding after the final row).
inline bool ResolveUploadLayout(
    const UploadMipLayout& L,
    uint64_t source_offset,
    uint64_t src_buffer_bytes,
    const WWMI_D3D12UploadFootprint_v1* host_fp,
    ResolvedUploadLayout& out,
    uint32_t copy_w = 0,
    uint32_t copy_h = 0,
    uint64_t max_map_limit = (std::numeric_limits<size_t>::max)()) noexcept
{
    out = {};
    if (!L.w || !L.h || !L.rows || !L.tight_row || !L.pitch || L.pitch < L.tight_row) return false;
    const bool bc = L.bc_block_bytes != 0u;
    if (!bc && !L.pixel_bytes) return false;
    if (bc && L.pixel_bytes != 0u) return false;

    const uint64_t elem_bytes = bc ? uint64_t(L.bc_block_bytes) : uint64_t(L.pixel_bytes);
    const uint64_t expected_rows = bc ? (uint64_t(L.h) + 3u) / 4u : uint64_t(L.h);
    const uint64_t expected_tight = bc ? ((uint64_t(L.w) + 3u) / 4u) * elem_bytes
                                       : uint64_t(L.w) * elem_bytes;
    if (expected_rows != L.rows || expected_tight != L.tight_row) return false;

    const uint64_t rw = bc ? ((uint64_t(L.w) + 3u) & ~3ull) : uint64_t(L.w);
    const uint64_t rh = bc ? ((uint64_t(L.h) + 3u) & ~3ull) : uint64_t(L.h);
    const bool     has_explicit_copy_extent = (copy_w != 0u || copy_h != 0u);
    const uint64_t eff_copy_w = has_explicit_copy_extent ? uint64_t(copy_w) : uint64_t(L.w);
    const uint64_t eff_copy_h = has_explicit_copy_extent ? uint64_t(copy_h) : uint64_t(L.h);
    if ((eff_copy_w != L.w && eff_copy_w != rw) || (eff_copy_h != L.h && eff_copy_h != rh))
        return false;

    const uint64_t limit = (std::min)(src_buffer_bytes, max_map_limit);
    uint64_t       pitch = L.pitch;
    uint64_t       origin_off = 0;

    if (host_fp) {
        if (host_fp->struct_size != sizeof(WWMI_D3D12UploadFootprint_v1) || host_fp->abi_version != 1u)
            return false;
        if (host_fp->row_pitch == 0u || uint64_t(host_fp->row_pitch) < uint64_t(L.tight_row))
            return false;
        if (host_fp->footprint_width == 0u || host_fp->footprint_height == 0u || host_fp->footprint_depth == 0u)
            return false;

        pitch = uint64_t(host_fp->row_pitch);
        if ((pitch % elem_bytes) != 0u)
            return false;

        const uint64_t fp_blocks_x = bc ? (uint64_t(host_fp->footprint_width) + 3u) / 4u : 0u;
        const uint64_t fp_blocks_y = bc ? (uint64_t(host_fp->footprint_height) + 3u) / 4u : 0u;
        const uint64_t fp_row_bytes = bc ? fp_blocks_x * elem_bytes
                                         : uint64_t(host_fp->footprint_width) * elem_bytes;
        const uint64_t fp_rows_per_slice = bc ? fp_blocks_y : uint64_t(host_fp->footprint_height);
        if (!fp_row_bytes || fp_row_bytes > pitch || !fp_rows_per_slice)
            return false;

        // Validate the entire declared placed footprint fits inside [source_offset, limit]
        // (no trailing row-pitch padding required after the last row of the last slice).
        if (uint64_t(host_fp->footprint_depth) > UINT64_MAX / fp_rows_per_slice)
            return false;
        const uint64_t fp_total_rows = fp_rows_per_slice * uint64_t(host_fp->footprint_depth);
        if (!fp_total_rows || (fp_total_rows - 1u) > (UINT64_MAX - fp_row_bytes) / pitch)
            return false;
        const uint64_t fp_span = (fp_total_rows - 1u) * pitch + fp_row_bytes;
        if (source_offset > limit || fp_span > limit - source_offset)
            return false;

        if (!host_fp->has_src_box) {
            if (host_fp->src_left != 0u || host_fp->src_top != 0u || host_fp->src_front != 0u)
                return false;
            if (host_fp->footprint_depth != 1u)
                return false;
            if (has_explicit_copy_extent) {
                if (uint64_t(host_fp->footprint_width) != eff_copy_w ||
                    uint64_t(host_fp->footprint_height) != eff_copy_h)
                    return false;
            } else {
                if ((uint64_t(host_fp->footprint_width) != L.w && uint64_t(host_fp->footprint_width) != rw) ||
                    (uint64_t(host_fp->footprint_height) != L.h && uint64_t(host_fp->footprint_height) != rh))
                    return false;
            }
            origin_off = 0;
        } else {
            if (host_fp->src_front >= host_fp->footprint_depth)
                return false;
            if (host_fp->src_left > host_fp->footprint_width ||
                eff_copy_w > uint64_t(host_fp->footprint_width) - uint64_t(host_fp->src_left))
                return false;
            if (host_fp->src_top > host_fp->footprint_height ||
                eff_copy_h > uint64_t(host_fp->footprint_height) - uint64_t(host_fp->src_top))
                return false;

            uint64_t col_bytes = 0;
            uint64_t row_off = 0;
            if (bc) {
                if ((host_fp->src_left & 3u) != 0u || (host_fp->src_top & 3u) != 0u)
                    return false;
                const uint64_t origin_bx = uint64_t(host_fp->src_left) / 4u;
                const uint64_t origin_by = uint64_t(host_fp->src_top) / 4u;
                const uint64_t mip_bx = (uint64_t(L.w) + 3u) / 4u;
                const uint64_t mip_by = uint64_t(L.rows);
                if (origin_bx > fp_blocks_x || mip_bx > fp_blocks_x - origin_bx)
                    return false;
                if (origin_by > fp_blocks_y || mip_by > fp_blocks_y - origin_by)
                    return false;
                // D3D12 BC edge rule based on actual copied box extent (eff_copy_w, eff_copy_h):
                // - Block-aligned extents (including 4-rounded rw/rh) are valid anywhere in the footprint.
                // - Non-multiple-of-4 extents are valid only when terminating on the footprint's final block edge.
                if ((eff_copy_w & 3u) != 0u && (origin_bx + mip_bx != fp_blocks_x))
                    return false;
                if ((eff_copy_h & 3u) != 0u && (origin_by + mip_by != fp_blocks_y))
                    return false;
                col_bytes = origin_bx * elem_bytes;
                row_off = origin_by;
            } else {
                col_bytes = uint64_t(host_fp->src_left) * elem_bytes;
                row_off = uint64_t(host_fp->src_top);
            }

            if (col_bytes > pitch || uint64_t(L.tight_row) > pitch - col_bytes)
                return false;

            uint64_t total_rows = row_off;
            if (host_fp->src_front != 0u) {
                if (uint64_t(host_fp->src_front) > (UINT64_MAX - row_off) / fp_rows_per_slice)
                    return false;
                total_rows += uint64_t(host_fp->src_front) * fp_rows_per_slice;
            }
            if (total_rows > (UINT64_MAX - col_bytes) / pitch)
                return false;
            origin_off = total_rows * pitch + col_bytes;
        }
        out.from_host_footprint = true;
    }

    if (source_offset > limit || origin_off > limit - source_offset)
        return false;
    const uint64_t base_off = source_offset + origin_off;
    if (uint64_t(L.rows - 1u) > (UINT64_MAX - uint64_t(L.tight_row)) / pitch)
        return false;
    const uint64_t span = uint64_t(L.rows - 1u) * pitch + uint64_t(L.tight_row);
    if (!span || span > limit - base_off)
        return false;

    out.base_offset = base_off;
    out.origin_offset = origin_off;
    out.row_pitch = pitch;
    out.span_bytes = span;
    return true;
}

inline bool ExtractTightRows(
    const uint8_t* src,
    size_t src_bytes,
    size_t row_pitch,
    const UploadMipLayout& L,
    std::vector<uint8_t>& out)
{
    if (!src || !L.rows || !L.tight_row || row_pitch < L.tight_row) return false;
    if (uint64_t(L.rows - 1u) > (UINT64_MAX - uint64_t(L.tight_row)) / uint64_t(row_pitch)) return false;
    const uint64_t needed = uint64_t(L.rows - 1u) * uint64_t(row_pitch) + uint64_t(L.tight_row);
    if (needed > src_bytes) return false;
    const uint64_t total_tight = uint64_t(L.tight_row) * uint64_t(L.rows);
    if (total_tight > (std::numeric_limits<size_t>::max)()) return false;
    out.resize(static_cast<size_t>(total_tight));
    for (uint32_t r = 0; r < L.rows; ++r) {
        std::memcpy(out.data() + size_t(r) * L.tight_row, src + size_t(r) * row_pitch, L.tight_row);
    }
    return true;
}

} // namespace wwmi_tex_upload
