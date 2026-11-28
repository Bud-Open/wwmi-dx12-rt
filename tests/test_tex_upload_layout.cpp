// SPDX-License-Identifier: GPL-3.0-only
#include "tex_upload_layout.hpp"
#include "wwmi_upload_footprint_bridge.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr uint64_t kFloorPx = 256ull * 256ull;

// Castagnoli CRC-32C (matches source/client/crc32c.cpp and PortApi::Crc32c).
uint32_t Crc32c(uint32_t seed, const void* data, size_t n)
{
    constexpr uint32_t kPoly = 0x82f63b78u;
    uint32_t crc = ~seed;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ ((crc & 1u) ? kPoly : 0u);
    }
    return ~crc;
}

uint32_t TexTypelessBC(uint32_t f)
{
    if (f >= 70 && f <= 72) return 70;
    if (f >= 73 && f <= 75) return 73;
    if (f >= 76 && f <= 78) return 76;
    if (f >= 79 && f <= 81) return 79;
    if (f >= 82 && f <= 84) return 82;
    if (f >= 94 && f <= 96) return 94;
    if (f >= 97 && f <= 99) return 97;
    if (f == 87 || f == 91) return 90;
    if (f == 88 || f == 93) return 92;
    if (f == 28 || f == 29) return 27;
    return f;
}

uint32_t TexIdentityHash(uint32_t data_hash, uint32_t w, uint32_t h, uint32_t mips, uint32_t arr, uint32_t dxgi_fmt)
{
    const uint32_t desc[11] = { w, h, mips, arr, TexTypelessBC(dxgi_fmt), 1, 0, 0, 8, 0, 0 };
    return Crc32c(data_hash, desc, sizeof(desc));
}

std::vector<uint8_t> MakePattern(size_t bytes, uint32_t salt)
{
    std::vector<uint8_t> out(bytes);
    for (size_t i = 0; i < bytes; ++i)
        out[i] = static_cast<uint8_t>(((i * 131u) ^ (i >> 3) ^ salt) & 0xffu);
    return out;
}

WWMI_D3D12UploadFootprint_v1 MakeHostFp(
    uint64_t cl, uint64_t src, uint64_t dst, uint32_t sub,
    uint32_t row_pitch, uint32_t fp_w, uint32_t fp_h, uint32_t fp_d,
    bool has_box, uint32_t left = 0, uint32_t top = 0, uint32_t front = 0)
{
    WWMI_D3D12UploadFootprint_v1 fp{};
    fp.struct_size = sizeof(fp);
    fp.abi_version = 1u;
    fp.command_list = cl;
    fp.src_resource = src;
    fp.dst_resource = dst;
    fp.dst_subresource = sub;
    fp.row_pitch = row_pitch;
    fp.footprint_width = fp_w;
    fp.footprint_height = fp_h;
    fp.footprint_depth = fp_d;
    fp.src_left = left;
    fp.src_top = top;
    fp.src_front = front;
    fp.has_src_box = has_box ? 1u : 0u;
    fp.reserved = 0u;
    return fp;
}

uint64_t FootprintMinimalBytes(const wwmi_tex_upload::UploadMipLayout& L, const WWMI_D3D12UploadFootprint_v1& fp)
{
    const bool bc = L.bc_block_bytes != 0u;
    const uint64_t elem = bc ? uint64_t(L.bc_block_bytes) : uint64_t(L.pixel_bytes);
    const uint64_t fp_row_bytes = bc ? ((uint64_t(fp.footprint_width) + 3u) / 4u) * elem
                                     : uint64_t(fp.footprint_width) * elem;
    const uint64_t rows_per_slice = bc ? (uint64_t(fp.footprint_height) + 3u) / 4u : uint64_t(fp.footprint_height);
    const uint64_t total_rows = rows_per_slice * uint64_t(fp.footprint_depth);
    return (total_rows - 1u) * uint64_t(fp.row_pitch) + fp_row_bytes;
}

// Run production decision helpers end-to-end:
// LegacyDstBoxCoversFullMip -> ResolveUploadLayout -> CPU extraction + simulated
// CopyBufferRegion([base_off, base_off + span)) + ResolveEffectiveRowPitch arrival parity.
void VerifyProductionUploadPipeline(
    const std::vector<uint8_t>& upload_buf,
    uint64_t source_offset,
    const wwmi_tex_upload::UploadMipLayout& L,
    const wwmi_tex_upload::DstCopyBox* dst_box,
    const WWMI_D3D12UploadFootprint_v1* host_fp,
    uint32_t mip_levels,
    uint32_t sub,
    uint32_t dxgi_fmt,
    const std::vector<uint8_t>& expected_tight)
{
    uint32_t copy_w = 0, copy_h = 0;
    assert(wwmi_tex_upload::LegacyDstBoxCoversFullMip(L, dst_box, copy_w, copy_h));

    wwmi_tex_upload::ResolvedUploadLayout resolved{};
    assert(wwmi_tex_upload::ResolveUploadLayout(
        L, source_offset, upload_buf.size(), host_fp, resolved, copy_w, copy_h));
    assert(resolved.base_offset + resolved.span_bytes <= upload_buf.size());

    // 1. CPU map path (mip0, sub>0, and D16 compare_from).
    const size_t cpu_pitch = wwmi_tex_upload::ResolveEffectiveRowPitch(static_cast<size_t>(resolved.row_pitch), L);
    assert(cpu_pitch == resolved.row_pitch);
    std::vector<uint8_t> cpu_tight;
    assert(wwmi_tex_upload::ExtractTightRows(
        upload_buf.data() + resolved.base_offset,
        upload_buf.size() - static_cast<size_t>(resolved.base_offset),
        cpu_pitch, L, cpu_tight));
    assert(cpu_tight == expected_tight);
    const uint32_t cpu_crc = Crc32c(0, cpu_tight.data(), cpu_tight.size());

    // 2. Simulated GPU readback: CopyBufferRegion of exact [base_off, base_off + span).
    std::vector<uint8_t> readback(static_cast<size_t>(resolved.span_bytes));
    std::memcpy(readback.data(), upload_buf.data() + resolved.base_offset, readback.size());

    // 3. TexReadback::Arrived (stored p.rowPitch = resolved.row_pitch) -> ResolveEffectiveRowPitch.
    const size_t gpu_pitch = wwmi_tex_upload::ResolveEffectiveRowPitch(static_cast<size_t>(resolved.row_pitch), L);
    assert(gpu_pitch == resolved.row_pitch);
    std::vector<uint8_t> gpu_tight;
    assert(wwmi_tex_upload::ExtractTightRows(readback.data(), readback.size(), gpu_pitch, L, gpu_tight));
    assert(gpu_tight == expected_tight);
    const uint32_t gpu_crc = Crc32c(0, gpu_tight.data(), gpu_tight.size());
    assert(gpu_crc == cpu_crc);

    // 4. Identity hash parity (mip0 uses mip_levels, sub>0 uses mip_levels - sub).
    const uint32_t eff_mips = sub ? (mip_levels - sub) : mip_levels;
    const uint32_t expected_data_crc = Crc32c(0, expected_tight.data(), expected_tight.size());
    assert(TexIdentityHash(cpu_crc, L.w, L.h, eff_mips, 1u, dxgi_fmt) ==
           TexIdentityHash(expected_data_crc, L.w, L.h, eff_mips, 1u, dxgi_fmt));
    assert(TexIdentityHash(gpu_crc, L.w, L.h, eff_mips, 1u, dxgi_fmt) ==
           TexIdentityHash(expected_data_crc, L.w, L.h, eff_mips, 1u, dxgi_fmt));
}

void TestCanonicalUploads()
{
    struct Case {
        uint32_t w, h, mips, sub, fmt;
    };
    const Case cases[] = {
        { 512, 512, 10, 0, 28 },   // RGBA8 mip0
        { 512, 512, 10, 1, 28 },   // RGBA8 sub=1 (256x256)
        { 1024, 1024, 11, 0, 98 }, // BC7 mip0
        { 1024, 1024, 11, 1, 98 }, // BC7 sub=1 (512x512)
        { 1024, 1024, 11, 2, 77 }, // BC3 sub=2 (256x256)
        { 512, 512, 10, 0, 71 },   // BC1 mip0
        { 256, 256, 9, 0, 10 },    // R16G16B16A16_FLOAT (legacy-only uncompressed format)
    };
    for (const auto& c : cases) {
        wwmi_tex_upload::UploadMipLayout L{};
        assert(wwmi_tex_upload::ComputeMipLayout(c.w, c.h, c.mips, c.sub, c.fmt, kFloorPx, L));
        assert(L.floor);
        assert(wwmi_tex_upload::ResolveEffectiveRowPitch(0, L) == L.pitch);
        assert(wwmi_tex_upload::ResolveEffectiveRowPitch(L.tight_row - 1u, L) == 0);

        const std::vector<uint8_t> tight = MakePattern(size_t(L.tight_row) * L.rows, c.fmt * 17u + c.sub);
        const uint64_t source_offset = 512u;
        const uint64_t exact_span = uint64_t(L.rows - 1u) * L.pitch + L.tight_row;
        std::vector<uint8_t> upload(static_cast<size_t>(source_offset + exact_span), 0xA5);
        for (uint32_t r = 0; r < L.rows; ++r)
            std::memcpy(upload.data() + source_offset + size_t(r) * L.pitch,
                        tight.data() + size_t(r) * L.tight_row, L.tight_row);

        const wwmi_tex_upload::DstCopyBox exact_dst{ 0, 0, 0, L.w, L.h, 1 };
        // Absent host metadata (inferred fallback).
        VerifyProductionUploadPipeline(upload, source_offset, L, &exact_dst, nullptr, c.mips, c.sub, c.fmt, tight);
        VerifyProductionUploadPipeline(upload, source_offset, L, nullptr, nullptr, c.mips, c.sub, c.fmt, tight);

        // Present canonical host metadata: must match absent fallback byte-for-byte.
        const auto fp = MakeHostFp(0x100, 0x200, 0x300, c.sub, L.pitch, L.w, L.h, 1u, false);
        VerifyProductionUploadPipeline(upload, source_offset, L, &exact_dst, &fp, c.mips, c.sub, c.fmt, tight);

        // Buffer 1 byte shorter than span must fail both inferred and host paths.
        wwmi_tex_upload::ResolvedUploadLayout bad{};
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, source_offset, source_offset + exact_span - 1u, nullptr, bad));
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, source_offset, source_offset + exact_span - 1u, &fp, bad));
    }
}

void TestPaddedRowPitch()
{
    // 1. 1024x1024 RGBA8 with padded RowPitch = 4352 vs canonical 4096 (mip0).
    {
        wwmi_tex_upload::UploadMipLayout L{};
        assert(wwmi_tex_upload::ComputeMipLayout(1024, 1024, 11, 0, 28, kFloorPx, L));
        assert(L.tight_row == 4096 && L.pitch == 4096 && L.rows == 1024);

        constexpr uint32_t padded_pitch = 4352;
        constexpr uint64_t src_off = 1024;
        const std::vector<uint8_t> tight = MakePattern(size_t(L.tight_row) * L.rows, 0x1234u);
        const uint64_t span = uint64_t(L.rows - 1u) * padded_pitch + L.tight_row;
        std::vector<uint8_t> upload(static_cast<size_t>(src_off + span), 0xCC);
        for (uint32_t r = 0; r < L.rows; ++r)
            std::memcpy(upload.data() + src_off + size_t(r) * padded_pitch,
                        tight.data() + size_t(r) * L.tight_row, L.tight_row);

        const wwmi_tex_upload::DstCopyBox dst_box{ 0, 0, 0, 1024, 1024, 1 };
        const auto fp = MakeHostFp(1, 2, 3, 0, padded_pitch, 1024, 1024, 1, false);
        VerifyProductionUploadPipeline(upload, src_off, L, &dst_box, &fp, 11, 0, 28, tight);

        // Confirm naive canonical pitch hashing would have produced the WRONG CRC.
        std::vector<uint8_t> wrong_tight;
        assert(wwmi_tex_upload::ExtractTightRows(upload.data() + src_off, upload.size() - src_off, L.pitch, L, wrong_tight));
        assert(Crc32c(0, wrong_tight.data(), wrong_tight.size()) != Crc32c(0, tight.data(), tight.size()));
    }

    // 2. BC7 1024x1024 sub=1 (512x512 -> tight_row = 2048, 128 block rows) with padded RowPitch = 2560.
    {
        wwmi_tex_upload::UploadMipLayout L{};
        assert(wwmi_tex_upload::ComputeMipLayout(1024, 1024, 11, 1, 98, kFloorPx, L));
        assert(L.w == 512 && L.h == 512 && L.tight_row == 2048 && L.rows == 128);

        constexpr uint32_t padded_pitch = 2560;
        constexpr uint64_t src_off = 512;
        const std::vector<uint8_t> tight = MakePattern(size_t(L.tight_row) * L.rows, 0x5678u);
        const uint64_t span = uint64_t(L.rows - 1u) * padded_pitch + L.tight_row;
        std::vector<uint8_t> upload(static_cast<size_t>(src_off + span), 0xEE);
        for (uint32_t r = 0; r < L.rows; ++r)
            std::memcpy(upload.data() + src_off + size_t(r) * padded_pitch,
                        tight.data() + size_t(r) * L.tight_row, L.tight_row);

        const wwmi_tex_upload::DstCopyBox dst_box{ 0, 0, 0, 512, 512, 1 };
        const auto fp = MakeHostFp(1, 2, 3, 1, padded_pitch, 512, 512, 1, false);
        VerifyProductionUploadPipeline(upload, src_off, L, &dst_box, &fp, 11, 1, 98, tight);
    }
}

void TestBlockRoundedBcAndDstBoxGate()
{
    // 1028x1028 BC7 mip 2 -> logical 257x257, D3D12 GetCopyableFootprints emits 260x260!
    wwmi_tex_upload::UploadMipLayout L{};
    assert(wwmi_tex_upload::ComputeMipLayout(1028, 1028, 11, 2, 98, kFloorPx, L));
    assert(L.w == 257 && L.h == 257 && L.rows == 65 && L.tight_row == 65u * 16u && L.pitch == 1280 && L.floor);

    const std::vector<uint8_t> tight = MakePattern(size_t(L.tight_row) * L.rows, 0x257u);
    const uint64_t src_off = 512u;

    // Case 1: pSrcBox == nullptr -> host sets dst_box = {0,0,0,260,260,1}, footprint = 260x260.
    {
        const auto fp = MakeHostFp(1, 2, 3, 2, 1536, 260, 260, 1, false);
        const uint64_t fp_bytes = FootprintMinimalBytes(L, fp);
        std::vector<uint8_t> upload(static_cast<size_t>(src_off + fp_bytes), 0x77);
        for (uint32_t r = 0; r < L.rows; ++r)
            std::memcpy(upload.data() + src_off + size_t(r) * fp.row_pitch,
                        tight.data() + size_t(r) * L.tight_row, L.tight_row);
        const wwmi_tex_upload::DstCopyBox dst_rounded{ 0, 0, 0, 260, 260, 1 };
        VerifyProductionUploadPipeline(upload, src_off, L, &dst_rounded, &fp, 11, 2, 98, tight);
    }

    // Case 2: block-aligned pSrcBox = {4, 8, 0, 264, 268, 1} (actual copy extent 260x260) inside 512x512 atlas!
    {
        const auto fp = MakeHostFp(1, 2, 3, 2, 2304, 512, 512, 1, true, 4, 8, 0);
        const uint64_t fp_bytes = FootprintMinimalBytes(L, fp);
        std::vector<uint8_t> upload(static_cast<size_t>(src_off + fp_bytes), 0x44);
        const uint64_t origin_off = uint64_t(8 / 4) * fp.row_pitch + uint64_t(4 / 4) * 16u;
        for (uint32_t r = 0; r < L.rows; ++r)
            std::memcpy(upload.data() + src_off + origin_off + size_t(r) * fp.row_pitch,
                        tight.data() + size_t(r) * L.tight_row, L.tight_row);
        const wwmi_tex_upload::DstCopyBox dst_rounded{ 0, 0, 0, 260, 260, 1 };
        VerifyProductionUploadPipeline(upload, src_off, L, &dst_rounded, &fp, 11, 2, 98, tight);
    }

    // Case 3: dst_box gate rejections (non-zero origin, wrong depth, partial atlas, wrong rounded extent for RGBA8).
    {
        uint32_t cw = 0, ch = 0;
        const wwmi_tex_upload::DstCopyBox partial{ 0, 0, 0, 256, 257, 1 };
        const wwmi_tex_upload::DstCopyBox bad_origin{ 4, 0, 0, 260, 260, 1 };
        const wwmi_tex_upload::DstCopyBox bad_depth{ 0, 0, 0, 257, 257, 2 };
        const wwmi_tex_upload::DstCopyBox bad_front{ 0, 0, 1, 257, 257, 2 };
        const wwmi_tex_upload::DstCopyBox oversize{ 0, 0, 0, 264, 260, 1 };
        assert(!wwmi_tex_upload::LegacyDstBoxCoversFullMip(L, &partial, cw, ch));
        assert(!wwmi_tex_upload::LegacyDstBoxCoversFullMip(L, &bad_origin, cw, ch));
        assert(!wwmi_tex_upload::LegacyDstBoxCoversFullMip(L, &bad_depth, cw, ch));
        assert(!wwmi_tex_upload::LegacyDstBoxCoversFullMip(L, &bad_front, cw, ch));
        assert(!wwmi_tex_upload::LegacyDstBoxCoversFullMip(L, &oversize, cw, ch));

        // Uncompressed 257x257 MUST NOT accept 260x260.
        wwmi_tex_upload::UploadMipLayout rgba{};
        assert(wwmi_tex_upload::ComputeMipLayout(257, 257, 1, 0, 28, kFloorPx, rgba));
        const wwmi_tex_upload::DstCopyBox rounded{ 0, 0, 0, 260, 260, 1 };
        const wwmi_tex_upload::DstCopyBox exact{ 0, 0, 0, 257, 257, 1 };
        assert(!wwmi_tex_upload::LegacyDstBoxCoversFullMip(rgba, &rounded, cw, ch));
        assert(wwmi_tex_upload::LegacyDstBoxCoversFullMip(rgba, &exact, cw, ch) && cw == 257 && ch == 257);
    }
}

void TestSrcBoxOriginAndValidation()
{
    // 1. Valid RGBA8 non-zero src_box (left=13, top=7, front=1) inside a 2-slice staging footprint.
    {
        wwmi_tex_upload::UploadMipLayout L{};
        assert(wwmi_tex_upload::ComputeMipLayout(256, 256, 9, 0, 28, kFloorPx, L));
        constexpr uint32_t fp_w = 320, fp_h = 270, fp_d = 2, pitch = 1536;
        constexpr uint32_t left = 13, top = 7, front = 1;
        constexpr uint64_t src_off = 512;
        const auto fp = MakeHostFp(10, 20, 30, 0, pitch, fp_w, fp_h, fp_d, true, left, top, front);
        const uint64_t fp_bytes = FootprintMinimalBytes(L, fp);
        const std::vector<uint8_t> tight = MakePattern(size_t(L.tight_row) * L.rows, 0x99u);
        const uint64_t origin_off = (uint64_t(front) * fp_h + top) * pitch + uint64_t(left) * 4u;
        // Exact minimal footprint size (no trailing padding after final row of slice 1).
        std::vector<uint8_t> upload(static_cast<size_t>(src_off + fp_bytes), 0x33);
        for (uint32_t r = 0; r < L.rows; ++r)
            std::memcpy(upload.data() + src_off + origin_off + size_t(r) * pitch,
                        tight.data() + size_t(r) * L.tight_row, L.tight_row);

        const wwmi_tex_upload::DstCopyBox dst_box{ 0, 0, 0, 256, 256, 1 };
        VerifyProductionUploadPipeline(upload, src_off, L, &dst_box, &fp, 9, 0, 28, tight);

        // Buffer 1 byte shorter than declared footprint MUST fail even though selected box rows fit!
        wwmi_tex_upload::ResolvedUploadLayout bad{};
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, src_off, src_off + fp_bytes - 1u, &fp, bad, 256, 256));
    }

    // 2. Valid BC3 block-aligned non-zero src_box (left=16, top=12, front=2) for sub=1 (256x256).
    {
        wwmi_tex_upload::UploadMipLayout L{};
        assert(wwmi_tex_upload::ComputeMipLayout(512, 512, 10, 1, 77, kFloorPx, L));
        constexpr uint32_t fp_w = 320, fp_h = 280, fp_d = 3, pitch = 1536;
        constexpr uint32_t left = 16, top = 12, front = 2;
        constexpr uint64_t src_off = 1024;
        const auto fp = MakeHostFp(10, 20, 30, 1, pitch, fp_w, fp_h, fp_d, true, left, top, front);
        const uint64_t fp_bytes = FootprintMinimalBytes(L, fp);
        const std::vector<uint8_t> tight = MakePattern(size_t(L.tight_row) * L.rows, 0xABu);
        const uint64_t fp_blocks_y = (fp_h + 3u) / 4u;
        const uint64_t origin_off = (uint64_t(front) * fp_blocks_y + top / 4u) * pitch + (left / 4u) * 16u;
        std::vector<uint8_t> upload(static_cast<size_t>(src_off + fp_bytes), 0x55);
        for (uint32_t r = 0; r < L.rows; ++r)
            std::memcpy(upload.data() + src_off + origin_off + size_t(r) * pitch,
                        tight.data() + size_t(r) * L.tight_row, L.tight_row);

        const wwmi_tex_upload::DstCopyBox dst_box{ 0, 0, 0, 256, 256, 1 };
        VerifyProductionUploadPipeline(upload, src_off, L, &dst_box, &fp, 10, 1, 77, tight);
    }

    // 3. BC 4x4 block edge rules for non-multiple-of-4 mip (257x257 BC7 -> 65x65 blocks, tight_row = 1040).
    {
        wwmi_tex_upload::UploadMipLayout L{};
        assert(wwmi_tex_upload::ComputeMipLayout(257, 257, 1, 0, 98, kFloorPx, L));
        assert(L.rows == 65 && L.tight_row == 65u * 16u && L.pitch == 1280);
        wwmi_tex_upload::ResolvedUploadLayout res{};

        // Valid: 4-rounded footprint (260x260), origin (0,0) with logical copy extent (257x257).
        const auto edge_rounded = MakeHostFp(1, 2, 3, 0, 1280, 260, 260, 1, true, 0, 0, 0);
        assert(wwmi_tex_upload::ResolveUploadLayout(L, 0, 1024 * 1024, &edge_rounded, res, 257, 257));
        assert(res.origin_offset == 0 && res.row_pitch == 1280);

        // Valid: exact footprint (257x257), origin (0,0).
        const auto edge_exact = MakeHostFp(1, 2, 3, 0, 1280, 257, 257, 1, true, 0, 0, 0);
        assert(wwmi_tex_upload::ResolveUploadLayout(L, 0, 1024 * 1024, &edge_exact, res, 257, 257));

        // Valid: non-zero aligned origin terminating at final block edge of 264x264 footprint.
        const auto edge_shifted = MakeHostFp(1, 2, 3, 0, 1280, 264, 264, 1, true, 4, 4, 0);
        assert(wwmi_tex_upload::ResolveUploadLayout(L, 0, 1024 * 1024, &edge_shifted, res, 257, 257));
        assert(res.origin_offset == 1280 + 16);

        // Invalid: interior unrounded 257x257 box inside 512x512 footprint (violates BC edge rule).
        const auto interior_bad = MakeHostFp(1, 2, 3, 0, 2048, 512, 512, 1, true, 0, 0, 0);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, 1024 * 1024, &interior_bad, res, 257, 257));

        // Valid: interior 4-aligned 260x260 box inside 512x512 footprint IS legal!
        assert(wwmi_tex_upload::ResolveUploadLayout(L, 0, 1024 * 1024, &interior_bad, res, 260, 260));

        // Invalid: src_left=4 + 257 = 261 > 260.
        const auto oob_edge = MakeHostFp(1, 2, 3, 0, 1280, 260, 260, 1, true, 4, 0, 0);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, 1024 * 1024, &oob_edge, res, 257, 257));
    }

    // 4. Invalid host metadata MUST be rejected cleanly (never fall back to L.pitch).
    {
        wwmi_tex_upload::UploadMipLayout L{};
        assert(wwmi_tex_upload::ComputeMipLayout(256, 256, 9, 0, 98, kFloorPx, L));
        wwmi_tex_upload::ResolvedUploadLayout res{};
        const uint64_t big_buf = 16ull * 1024 * 1024;

        // Unaligned BC src_left / src_top
        auto fp = MakeHostFp(1, 2, 3, 0, 2048, 512, 512, 1, true, 2, 0, 0);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));
        fp = MakeHostFp(1, 2, 3, 0, 2048, 512, 512, 1, true, 0, 3, 0);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));

        // Out-of-bounds src_box origin / extent / depth
        fp = MakeHostFp(1, 2, 3, 0, 2048, 256, 256, 1, true, 4, 0, 0);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));
        fp = MakeHostFp(1, 2, 3, 0, 2048, 256, 256, 1, true, 0, 4, 0);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));
        fp = MakeHostFp(1, 2, 3, 0, 2048, 256, 256, 1, true, 0, 0, 1);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));

        // Zero, undersized, or non-element-aligned row_pitch (even though inferred L.pitch would fit big_buf!)
        fp = MakeHostFp(1, 2, 3, 0, 0, 256, 256, 1, false);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));
        fp = MakeHostFp(1, 2, 3, 0, L.tight_row - 16u, 256, 256, 1, false);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));
        fp = MakeHostFp(1, 2, 3, 0, L.tight_row + 8u, 256, 256, 1, false); // not 16B BC7 block aligned
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));
        // row_pitch covers L.tight_row (1024) but NOT full footprint_width (512 -> 2048 bytes)
        fp = MakeHostFp(1, 2, 3, 0, 1024, 512, 512, 1, true, 0, 0, 0);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));

        // Non-texel-aligned pitch on RGBA8 (1025 not divisible by 4)
        wwmi_tex_upload::UploadMipLayout rgba{};
        assert(wwmi_tex_upload::ComputeMipLayout(256, 256, 1, 0, 28, kFloorPx, rgba));
        fp = MakeHostFp(1, 2, 3, 0, 1025, 256, 256, 1, false);
        assert(!wwmi_tex_upload::ResolveUploadLayout(rgba, 0, big_buf, &fp, res));

        // Bad ABI header (struct_size / abi_version)
        fp = MakeHostFp(1, 2, 3, 0, 1024, 256, 256, 1, false);
        fp.struct_size -= 4;
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));
        fp = MakeHostFp(1, 2, 3, 0, 1024, 256, 256, 1, false);
        fp.abi_version = 2;
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));

        // Non-zero src_left when has_src_box == 0, or footprint dimension mismatch when !has_src_box
        fp = MakeHostFp(1, 2, 3, 0, 1024, 256, 256, 1, false, 4, 0, 0);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));
        fp = MakeHostFp(1, 2, 3, 0, 2048, 512, 256, 1, false);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, big_buf, &fp, res));

        // Arithmetic overflow checks (source_offset near UINT64_MAX and huge footprint/src_front)
        fp = MakeHostFp(1, 2, 3, 0, 2048, 512, 512, 1, true, 16, 16, 0);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, UINT64_MAX - 16u, UINT64_MAX, &fp, res));
        fp = MakeHostFp(1, 2, 3, 0, 0x80000000u, 512, 0x40000000u, 0x40000000u, true, 0, 0, 0x3fffffffu);
        assert(!wwmi_tex_upload::ResolveUploadLayout(L, 0, UINT64_MAX, &fp, res));
    }
}

void TestHostFootprintBridge()
{
    WWMI_D3D12UploadFootprint_v1 out{};
    out.row_pitch = 0xdeadbeefu;

    // 1. Missing active notice -> returns 0 and leaves out untouched.
    assert(wwmi::upload_footprint::query(0x11, 0x22, 0x33, 0, &out, sizeof(out)) == 0);
    assert(out.row_pitch == 0xdeadbeefu);

    const auto outer = MakeHostFp(0x11, 0x22, 0x33, 2, 4352, 1024, 1024, 1, true, 4, 8, 0);
    const auto inner = MakeHostFp(0xAA, 0xBB, 0xCC, 1, 2560, 512, 512, 2, true, 16, 12, 1);

    {
        const wwmi::upload_footprint::Notice outer_guard(&outer);

        // 2. Exact tuple match succeeds and copies every field.
        WWMI_D3D12UploadFootprint_v1 got{};
        assert(wwmi::upload_footprint::query(0x11, 0x22, 0x33, 2, &got, sizeof(got)) == 1);
        assert(std::memcmp(&got, &outer, sizeof(got)) == 0);

        // 3. Mismatch on each of the 4 tuple fields returns 0 and preserves sentinel.
        WWMI_D3D12UploadFootprint_v1 sentinel{};
        sentinel.row_pitch = 0x12345678u;
        assert(wwmi::upload_footprint::query(0x12, 0x22, 0x33, 2, &sentinel, sizeof(sentinel)) == 0);
        assert(wwmi::upload_footprint::query(0x11, 0x23, 0x33, 2, &sentinel, sizeof(sentinel)) == 0);
        assert(wwmi::upload_footprint::query(0x11, 0x22, 0x34, 2, &sentinel, sizeof(sentinel)) == 0);
        assert(wwmi::upload_footprint::query(0x11, 0x22, 0x33, 3, &sentinel, sizeof(sentinel)) == 0);
        assert(sentinel.row_pitch == 0x12345678u);

        // 4. Bad output pointer or size returns 0 and preserves sentinel.
        assert(wwmi::upload_footprint::query(0x11, 0x22, 0x33, 2, nullptr, sizeof(sentinel)) == 0);
        assert(wwmi::upload_footprint::query(0x11, 0x22, 0x33, 2, &sentinel, 0) == 0);
        assert(wwmi::upload_footprint::query(0x11, 0x22, 0x33, 2, &sentinel, sizeof(sentinel) - 1) == 0);
        assert(wwmi::upload_footprint::query(0x11, 0x22, 0x33, 2, &sentinel, sizeof(sentinel) + 1) == 0);
        assert(sentinel.row_pitch == 0x12345678u);

        // 5. Nested Notice overrides and restores cleanly.
        {
            const wwmi::upload_footprint::Notice inner_guard(&inner);
            assert(wwmi::upload_footprint::query(0x11, 0x22, 0x33, 2, &got, sizeof(got)) == 0);
            assert(wwmi::upload_footprint::query(0xAA, 0xBB, 0xCC, 1, &got, sizeof(got)) == 1);
            assert(std::memcmp(&got, &inner, sizeof(got)) == 0);

            // End-to-end: bridge query -> LegacyDstBoxCoversFullMip -> ResolveUploadLayout -> CPU & GPU parity.
            wwmi_tex_upload::UploadMipLayout L{};
            assert(wwmi_tex_upload::ComputeMipLayout(512, 512, 10, 1, 98, kFloorPx, L)); // 256x256 BC7
            const std::vector<uint8_t> tight = MakePattern(size_t(L.tight_row) * L.rows, 0x777u);
            const uint64_t fp_bytes = FootprintMinimalBytes(L, inner);
            const uint64_t fp_blocks_y = (inner.footprint_height + 3u) / 4u;
            const uint64_t origin_off =
                (uint64_t(inner.src_front) * fp_blocks_y + inner.src_top / 4u) * inner.row_pitch +
                (inner.src_left / 4u) * 16u;
            const uint64_t src_off = 512u;
            std::vector<uint8_t> upload(static_cast<size_t>(src_off + fp_bytes), 0x99);
            for (uint32_t r = 0; r < L.rows; ++r)
                std::memcpy(upload.data() + src_off + origin_off + size_t(r) * inner.row_pitch,
                            tight.data() + size_t(r) * L.tight_row, L.tight_row);
            const wwmi_tex_upload::DstCopyBox dst_box{ 0, 0, 0, 256, 256, 1 };
            VerifyProductionUploadPipeline(upload, src_off, L, &dst_box, &got, 10, 1, 98, tight);
        }

        // Restored back to outer notice.
        assert(wwmi::upload_footprint::query(0x11, 0x22, 0x33, 2, &got, sizeof(got)) == 1);
        assert(std::memcmp(&got, &outer, sizeof(got)) == 0);
    }

    // 6. Notice destroyed -> missing again.
    assert(wwmi::upload_footprint::query(0x11, 0x22, 0x33, 2, &out, sizeof(out)) == 0);
}

} // namespace

int main()
{
    TestCanonicalUploads();
    TestPaddedRowPitch();
    TestBlockRoundedBcAndDstBoxGate();
    TestSrcBoxOriginAndValidation();
    TestHostFootprintBridge();
    std::puts("OK: test_tex_upload_layout");
    return 0;
}
