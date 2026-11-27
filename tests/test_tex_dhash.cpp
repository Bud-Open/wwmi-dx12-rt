#include "tex_dhash.h"

// The addon gets these symbols from tex_look.cpp. For the host test link the
// same upstream decoder without the Windows-only texture matcher.
#define BCDEC_IMPLEMENTATION
#include "../../third_party/bcdec/bcdec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string_view>
#include <vector>

namespace {

using Pixel = std::array<uint8_t, 4>;

template <typename F>
uint64_t Rgba(uint32_t w, uint32_t h, bool srgb, F pixel)
{
    std::vector<Pixel> image;
    image.reserve(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            image.push_back(pixel(x, y));
    const texdhash::Mip mip{w, h, 28, [&](uint32_t y) { return image[size_t(y) * w].data(); }};
    uint64_t out = 0;
    assert(texdhash::Hash(mip, srgb, &out));
    return out;
}

void Expect(uint64_t actual, uint64_t expected)
{
    if (actual != expected)
        std::fprintf(stderr, "got %016llx, expected %016llx\n",
            static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
    assert(actual == expected);
}

void Gradient()
{
    Expect(Rgba(256, 256, false, [](uint32_t x, uint32_t) {
        return Pixel{static_cast<uint8_t>(255 - x), 0, 0, 255};
    }), 0xffffffffffffffffULL);

    // At 18x8 the shader's 9x8 grid lands halfway between each pair of
    // texels in a row, exactly on each row. Prescribe distinct comparison
    // masks to catch a transposed grid or reversed bit/word ordering.
    constexpr std::array<uint8_t, 8> masks{0xa5, 0x24, 0x81, 0x7e, 0x90, 0x33, 0x51, 0xff};
    Expect(Rgba(18, 8, false, [&](uint32_t x, uint32_t y) {
        int v = 128;
        for (uint32_t i = 0; i < x / 2; ++i) v += ((masks[y] >> i) & 1) ? -10 : 10;
        return Pixel{uint8_t(v), uint8_t(v), uint8_t(v), 255};
    }), 0xff5133907e8124a5ULL);
}

void Resolution()
{
    const auto image = [](uint32_t side) {
        return Rgba(side, side, false, [side](uint32_t x, uint32_t y) {
            const double u = (x + 0.5) / side, v = (y + 0.5) / side;
            const auto c = uint8_t(std::lround(128 + 100 * std::cos(6.283185307179586 * (u + 0.41 * v))));
            return Pixel{c, c, c, 255};
        });
    };
    const uint64_t h256 = image(256), h512 = image(512), h1024 = image(1024);
    assert(h256 != 0 && h256 != UINT64_MAX);
    assert(std::popcount(h256 ^ h512) <= 2);
    assert(std::popcount(h512 ^ h1024) <= 2);
    assert(std::popcount(h256 ^ h1024) <= 2);
}

constexpr Pixel SolidPixel{128, 64, 255, 32};

void Solid()
{
    // Half-up quantization to {8,4,16,2}, then high RG and low BA.
    Expect(Rgba(5, 13, false, [](uint32_t, uint32_t) { return SolidPixel; }), 0x00ff000fffff0003ULL);
    Expect(Rgba(1, 1, false, [](uint32_t, uint32_t) { return Pixel{}; }), 0);
}

void Srgb()
{
    for (uint32_t fmt : {29, 72, 75, 78, 91, 93, 99}) {
        assert(texdhash::IsSrgb(fmt));
        assert(texdhash::HasSrgbView(fmt));
    }
    for (uint32_t fmt : {27, 28, 70, 71, 74, 77, 87, 88, 90, 92, 97, 98}) {
        assert(!texdhash::IsSrgb(fmt));
        assert(texdhash::HasSrgbView(fmt));
    }
    for (uint32_t fmt : {48, 61, 65, 80, 83, 95}) {
        assert(!texdhash::IsSrgb(fmt));
        assert(!texdhash::HasSrgbView(fmt));
    }
    // Linear: three mid-tone channels outweigh one full-strength channel.
    // In an sRGB view they linearize before filtering and the order reverses.
    const auto pattern = [](uint32_t x, uint32_t) {
        return ((x / 2) & 1) ? Pixel{255, 0, 0, 255} : Pixel{100, 100, 100, 255};
    };
    Expect(Rgba(18, 8, false, pattern), 0x5555555555555555ULL);
    Expect(Rgba(18, 8, true, pattern), 0xaaaaaaaaaaaaaaaaULL);
    Expect(Rgba(5, 13, true, [](uint32_t, uint32_t) { return SolidPixel; }), 0x00070001ffff0003ULL);
}

// BC1: RGB565 red to black; the row's indices select 255, 170, 85, 0.
constexpr uint8_t Bc1[]{0x00, 0xf8, 0, 0, 0x78, 0x78, 0x78, 0x78};
// BC7 mode 6: RGBA endpoints (0,0,0,0) and (255,255,255,255) with
// shared endpoint p-bits 0 and 1. Indices 0,15,15,0 in all four rows.
constexpr uint8_t Bc7[]{
    0x40, 0xc0, 0x1f, 0xf0, 0x07, 0xfc, 0x01, 0x7f, 0xf1, 0x0f, 0xf0, 0x0f, 0xf0, 0x0f, 0xf0, 0x0f
};

void Blocks()
{
    const auto hash = [](uint32_t fmt, const uint8_t* block) {
        uint64_t out = 0;
        assert(texdhash::Hash({4, 4, fmt, [block](uint32_t row) {
            return row == 0 ? block : nullptr;
        }}, false, &out));
        return out;
    };
    Expect(hash(71, Bc1), 0xffffffffffffffffULL);
    // Clamp/lerp across [0,255,255,0]: only comparisons 5,6,7 decrease.
    Expect(hash(98, Bc7), 0xe0e0e0e0e0e0e0e0ULL);
}

void Rows()
{
    constexpr std::array<std::array<uint32_t, 2>, 7> sizes{{
        {1, 1}, {4, 4}, {8, 8}, {13, 19}, {256, 256}, {257, 263}, {1024, 1024}
    }};
    for (const auto& [w, h] : sizes) {
        for (const uint32_t fmt : {28u, 98u}) {
            std::set<uint32_t> planned, fetched;
            uint64_t tight = 0;
            uint32_t rows = 0;
            assert(texdhash::RowLayout(w, h, fmt, tight, rows));
            texdhash::RowsRead(w, h, fmt, [&](uint32_t r) {
                assert(r < rows);
                assert(planned.empty() || r > *planned.rbegin());
                planned.insert(r);
            });
            assert(!planned.empty() && planned.size() <= 18);
            if (h >= 256) assert(planned.size() < rows);

            // A solid image forces the centre fallback as well as the grid.
            // BC7 mode 6 with every bit after the mode field zero is transparent black.
            std::vector<uint8_t> data(static_cast<size_t>(tight), 0);
            if (fmt == 98)
                for (size_t i = 0; i < data.size(); i += 16) data[i] = 0x40;
            const texdhash::Mip mip{w, h, fmt, [&](uint32_t r) -> const uint8_t* {
                fetched.insert(r);
                return planned.count(r) ? data.data() : nullptr;
            }};
            uint64_t out = UINT64_MAX;
            assert(texdhash::Hash(mip, false, &out));
            Expect(out, 0);
            assert(fetched == planned);
        }
    }
}

void Layout()
{
    const auto check = [](std::initializer_list<uint32_t> formats, uint64_t bytes, uint32_t rows) {
        for (const auto fmt : formats) {
            assert(texdhash::Decodes(fmt));
            uint64_t tight = 0;
            uint32_t count = 0;
            assert(texdhash::RowLayout(13, 19, fmt, tight, count));
            assert(tight == bytes && count == rows);
        }
    };
    check({70, 71, 72, 79, 80}, 4 * 8, 5);
    check({73, 74, 75, 76, 77, 78, 82, 83, 94, 95, 97, 98, 99}, 4 * 16, 5);
    check({27, 28, 29, 87, 88, 90, 91, 92, 93}, 13 * 4, 19);
    check({48, 49}, 13 * 2, 19);
    check({60, 61, 65}, 13, 19);
    assert(!texdhash::Decodes(0));
    assert(texdhash::IsBlockCompressed(71));
    assert(texdhash::IsBlockCompressed(98));
    assert(!texdhash::IsBlockCompressed(28));
    assert(!texdhash::IsBlockCompressed(0));
}

void UploadFootprint()
{
    uint64_t off = 0;
    // Padded BC7 upload footprint (2048B stride for a 1024B tight row) with non-zero 4-aligned source box.
    assert(texdhash::UploadRegionOffset(98, 256, 16, 8, 0, 2048, 1024, off));
    assert(off == 2 * 2048 + 4 * 16);
    // Slice offset for src_front > 0.
    assert(texdhash::UploadRegionOffset(98, 16, 0, 4, 2, 512, 64, off));
    assert(off == (1 + 2 * 4) * 512);
    // Reject non-block-aligned BC source box origin, tight_row exceeding remaining row_pitch, and unknown format.
    assert(!texdhash::UploadRegionOffset(98, 256, 2, 0, 0, 2048, 1024, off));
    assert(!texdhash::UploadRegionOffset(98, 256, 0, 3, 0, 2048, 1024, off));
    assert(!texdhash::UploadRegionOffset(98, 256, 400, 0, 0, 2048, 1024, off));
    assert(!texdhash::UploadRegionOffset(0, 16, 0, 0, 0, 256, 64, off));

    // Hashing a 16x16 RGBA8 mip placed inside a padded 512B-stride buffer at src_box=(4, 2) must match tight hashing.
    constexpr uint32_t w = 16, h = 16, pitch = 512, src_x = 4, src_y = 2;
    std::vector<uint8_t> tight(size_t(w) * h * 4);
    std::vector<uint8_t> staged(size_t(h + src_y + 2) * pitch, 0xCC);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t px[4] = {uint8_t(x * 15), uint8_t(y * 15), uint8_t((x + y) * 7), 255};
            std::memcpy(&tight[(size_t(y) * w + x) * 4], px, 4);
            std::memcpy(&staged[size_t(y + src_y) * pitch + size_t(x + src_x) * 4], px, 4);
        }
    }
    uint64_t tight_row = 0;
    uint32_t rows = 0;
    assert(texdhash::RowLayout(w, h, 28, tight_row, rows));
    assert(texdhash::UploadRegionOffset(28, h + src_y, src_x, src_y, 0, pitch, tight_row, off));
    uint64_t h_tight = 0, h_staged = 0;
    assert(texdhash::Hash({w, h, 28, [&](uint32_t r) { return tight.data() + size_t(r) * tight_row; }}, false, &h_tight));
    assert(texdhash::Hash({w, h, 28, [&](uint32_t r) { return staged.data() + off + size_t(r) * pitch; }}, false, &h_staged));
    Expect(h_staged, h_tight);
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string_view test = argv[1];
    if (test == "gradient") Gradient();
    else if (test == "resolution") Resolution();
    else if (test == "solid") Solid();
    else if (test == "srgb") Srgb();
    else if (test == "blocks") Blocks();
    else if (test == "rows") Rows();
    else if (test == "layout") Layout();
    else if (test == "upload_footprint") UploadFootprint();
    else return 2;
}
