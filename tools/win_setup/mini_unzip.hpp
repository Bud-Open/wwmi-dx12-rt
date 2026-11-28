#pragma once
// Self-contained, zero-dependency RFC 1951 Inflate + strict ZIP Central Directory reader.
// Designed for WWMI-DX12-RT Setup: validates CRC-32, bounds, Zip-Slip, Windows reserved
// device names, ADS/colons, symlinks, duplicate/case-colliding paths, and size bombs.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace mini_unzip {

struct Limits {
    uint64_t max_zip_bytes = 64ull * 1024ull * 1024ull;
    uint64_t max_unpacked_bytes = 128ull * 1024ull * 1024ull;
    uint64_t max_single_file_bytes = 64ull * 1024ull * 1024ull;
    uint32_t max_entries = 1024u;
};

struct EntryStats {
    uint32_t file_count = 0;
    uint64_t unpacked_bytes = 0;
};

inline uint16_t ReadU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t ReadU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint32_t Crc32(const uint8_t* data, size_t len)
{
    static const std::array<uint32_t, 256> kTable = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int b = 0; b < 8; ++b)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = kTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

namespace detail {

struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    uint64_t bitbuf = 0;
    int bitcnt = 0;

    void Refill(int need)
    {
        while (bitcnt < need && pos < size) {
            bitbuf |= static_cast<uint64_t>(data[pos++]) << bitcnt;
            bitcnt += 8;
        }
    }

    bool ReadBits(int n, uint32_t& out)
    {
        if (n == 0) { out = 0; return true; }
        Refill(n);
        if (bitcnt < n) return false;
        out = static_cast<uint32_t>(bitbuf & ((1ull << n) - 1ull));
        bitbuf >>= n;
        bitcnt -= n;
        return true;
    }

    void AlignByte()
    {
        const int drop = bitcnt & 7;
        bitbuf >>= drop;
        bitcnt -= drop;
    }

    bool ReadAlignedBytes(uint8_t* dst, size_t n)
    {
        AlignByte();
        while (n > 0 && bitcnt >= 8) {
            *dst++ = static_cast<uint8_t>(bitbuf & 0xFFu);
            bitbuf >>= 8;
            bitcnt -= 8;
            --n;
        }
        if (pos + n > size) return false;
        if (n > 0) {
            std::memcpy(dst, data + pos, n);
            pos += n;
        }
        return true;
    }
};

struct HuffTree {
    static constexpr int kMaxBits = 15;
    static constexpr int kMaxSyms = 288;
    std::array<uint16_t, kMaxBits + 1> count{};
    std::array<uint16_t, kMaxSyms> symbol{};

    bool Build(const uint8_t* lengths, int num_syms)
    {
        if (num_syms <= 0 || num_syms > kMaxSyms) return false;
        count.fill(0);
        for (int i = 0; i < num_syms; ++i) {
            if (lengths[i] > kMaxBits) return false;
            count[lengths[i]]++;
        }
        if (count[0] == num_syms) return false; // empty tree

        // Verify Kraft inequality (not oversubscribed)
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        // RFC 1951 allows a single-symbol tree with 1-bit code (left == 1) for distances;
        // otherwise require complete code (left == 0) or single-code tree.
        int total_used = num_syms - count[0];
        if (left > 0 && !(total_used == 1 && count[1] == 1)) return false;

        std::array<uint16_t, kMaxBits + 1> offs{};
        uint16_t sum = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            offs[len] = sum;
            sum = static_cast<uint16_t>(sum + count[len]);
        }
        for (int sym = 0; sym < num_syms; ++sym) {
            if (lengths[sym] != 0)
                symbol[offs[lengths[sym]]++] = static_cast<uint16_t>(sym);
        }
        return true;
    }

    bool Decode(BitReader& br, uint16_t& out) const
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            uint32_t bit = 0;
            if (!br.ReadBits(1, bit)) return false;
            code |= static_cast<int>(bit);
            const int cnt = count[len];
            if (code - cnt < first) {
                out = symbol[index + (code - first)];
                return true;
            }
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        return false;
    }
};

inline bool InflateRaw(
    const uint8_t* src,
    size_t src_len,
    uint64_t expected_size,
    uint64_t max_size,
    std::vector<uint8_t>& out,
    std::string& err)
{
    if (expected_size > max_size) {
        err = "decompressed entry exceeds size limit";
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(std::min<uint64_t>(expected_size, 16ull * 1024ull * 1024ull)));

    static constexpr uint16_t kLenBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static constexpr uint8_t kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static constexpr uint16_t kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static constexpr uint8_t kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static constexpr uint8_t kOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    BitReader br{ src, src_len, 0, 0, 0 };
    uint32_t bfinal = 0;
    do {
        uint32_t btype = 0;
        if (!br.ReadBits(1, bfinal) || !br.ReadBits(2, btype)) {
            err = "truncated deflate block header";
            return false;
        }
        if (btype == 0) {
            uint8_t hdr[4]{};
            if (!br.ReadAlignedBytes(hdr, 4)) {
                err = "truncated stored deflate block";
                return false;
            }
            const uint16_t len = ReadU16(hdr);
            const uint16_t nlen = ReadU16(hdr + 2);
            if (static_cast<uint16_t>(~len) != nlen) {
                err = "corrupted stored deflate block length";
                return false;
            }
            if (out.size() + len > max_size) {
                err = "decompressed stream exceeds size limit";
                return false;
            }
            const size_t base = out.size();
            out.resize(base + len);
            if (len > 0 && !br.ReadAlignedBytes(out.data() + base, len)) {
                err = "truncated stored deflate payload";
                return false;
            }
        } else if (btype == 1 || btype == 2) {
            HuffTree lit_tree{};
            HuffTree dist_tree{};
            if (btype == 1) {
                std::array<uint8_t, 288> lit_lens{};
                for (int i = 0; i <= 143; ++i) lit_lens[i] = 8;
                for (int i = 144; i <= 255; ++i) lit_lens[i] = 9;
                for (int i = 256; i <= 279; ++i) lit_lens[i] = 7;
                for (int i = 280; i <= 287; ++i) lit_lens[i] = 8;
                std::array<uint8_t, 32> dist_lens{};
                dist_lens.fill(5);
                if (!lit_tree.Build(lit_lens.data(), 288) || !dist_tree.Build(dist_lens.data(), 32)) {
                    err = "failed building fixed Huffman tables";
                    return false;
                }
            } else {
                uint32_t hlit_r = 0, hdist_r = 0, hclen_r = 0;
                if (!br.ReadBits(5, hlit_r) || !br.ReadBits(5, hdist_r) || !br.ReadBits(4, hclen_r)) {
                    err = "truncated dynamic Huffman header";
                    return false;
                }
                const int hlit = static_cast<int>(hlit_r) + 257;
                const int hdist = static_cast<int>(hdist_r) + 1;
                const int hclen = static_cast<int>(hclen_r) + 4;
                if (hlit > 286 || hdist > 30) {
                    err = "invalid dynamic Huffman table counts";
                    return false;
                }
                std::array<uint8_t, 19> cl_lens{};
                for (int i = 0; i < hclen; ++i) {
                    uint32_t v = 0;
                    if (!br.ReadBits(3, v)) { err = "truncated code-length table"; return false; }
                    cl_lens[kOrder[i]] = static_cast<uint8_t>(v);
                }
                HuffTree cl_tree{};
                if (!cl_tree.Build(cl_lens.data(), 19)) {
                    err = "invalid code-length Huffman tree";
                    return false;
                }
                std::array<uint8_t, 286 + 30> lens{};
                const int total = hlit + hdist;
                int idx = 0;
                while (idx < total) {
                    uint16_t sym = 0;
                    if (!cl_tree.Decode(br, sym)) { err = "invalid code-length symbol"; return false; }
                    if (sym < 16) {
                        lens[idx++] = static_cast<uint8_t>(sym);
                    } else if (sym == 16) {
                        uint32_t rep = 0;
                        if (idx == 0 || !br.ReadBits(2, rep)) { err = "invalid RLE-16 code"; return false; }
                        rep += 3;
                        if (idx + static_cast<int>(rep) > total) { err = "RLE-16 overflows lengths"; return false; }
                        const uint8_t prev = lens[idx - 1];
                        for (uint32_t r = 0; r < rep; ++r) lens[idx++] = prev;
                    } else if (sym == 17) {
                        uint32_t rep = 0;
                        if (!br.ReadBits(3, rep)) { err = "invalid RLE-17 code"; return false; }
                        rep += 3;
                        if (idx + static_cast<int>(rep) > total) { err = "RLE-17 overflows lengths"; return false; }
                        for (uint32_t r = 0; r < rep; ++r) lens[idx++] = 0;
                    } else if (sym == 18) {
                        uint32_t rep = 0;
                        if (!br.ReadBits(7, rep)) { err = "invalid RLE-18 code"; return false; }
                        rep += 11;
                        if (idx + static_cast<int>(rep) > total) { err = "RLE-18 overflows lengths"; return false; }
                        for (uint32_t r = 0; r < rep; ++r) lens[idx++] = 0;
                    } else {
                        err = "unexpected code-length symbol";
                        return false;
                    }
                }
                if (lens[256] == 0) {
                    err = "dynamic Huffman table missing end-of-block symbol";
                    return false;
                }
                if (!lit_tree.Build(lens.data(), hlit)) {
                    err = "invalid literal/length Huffman tree";
                    return false;
                }
                // Distance tree may legally be all-zeros if block contains only literals.
                bool any_dist = false;
                for (int i = 0; i < hdist; ++i)
                    if (lens[hlit + i] != 0) { any_dist = true; break; }
                if (any_dist && !dist_tree.Build(lens.data() + hlit, hdist)) {
                    err = "invalid distance Huffman tree";
                    return false;
                }
            }

            for (;;) {
                uint16_t sym = 0;
                if (!lit_tree.Decode(br, sym)) {
                    err = "corrupted Huffman literal/length stream";
                    return false;
                }
                if (sym < 256) {
                    if (out.size() >= max_size) {
                        err = "decompressed stream exceeds size limit";
                        return false;
                    }
                    out.push_back(static_cast<uint8_t>(sym));
                } else if (sym == 256) {
                    break;
                } else if (sym <= 285) {
                    const int li = sym - 257;
                    uint32_t extra = 0;
                    if (!br.ReadBits(kLenExtra[li], extra)) {
                        err = "truncated length extra bits";
                        return false;
                    }
                    const size_t match_len = static_cast<size_t>(kLenBase[li]) + extra;
                    uint16_t dsym = 0;
                    if (!dist_tree.Decode(br, dsym) || dsym >= 30) {
                        err = "invalid distance code";
                        return false;
                    }
                    uint32_t dextra = 0;
                    if (!br.ReadBits(kDistExtra[dsym], dextra)) {
                        err = "truncated distance extra bits";
                        return false;
                    }
                    const size_t dist = static_cast<size_t>(kDistBase[dsym]) + dextra;
                    if (dist == 0 || dist > out.size()) {
                        err = "invalid back-reference distance";
                        return false;
                    }
                    if (out.size() + match_len > max_size) {
                        err = "decompressed match exceeds size limit";
                        return false;
                    }
                    const size_t dst_off = out.size();
                    out.resize(dst_off + match_len);
                    uint8_t* dst_p = out.data() + dst_off;
                    const uint8_t* src_p = dst_p - dist;
                    if (dist >= match_len) {
                        std::memcpy(dst_p, src_p, match_len);
                    } else {
                        for (size_t k = 0; k < match_len; ++k) dst_p[k] = src_p[k];
                    }
                } else {
                    err = "reserved literal/length symbol";
                    return false;
                }
            }
        } else {
            err = "reserved deflate block type 3";
            return false;
        }
    } while (!bfinal);

    if (out.size() != expected_size) {
        err = "decompressed size mismatch";
        return false;
    }
    return true;
}

inline std::string ToLowerAscii(const std::string& s)
{
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

inline bool IsWindowsReservedName(const std::string& seg)
{
    const size_t dot = seg.find('.');
    const std::string stem = ToLowerAscii(dot == std::string::npos ? seg : seg.substr(0, dot));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return true;
    if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0)) {
        return stem[3] >= '1' && stem[3] <= '9';
    }
    return false;
}

inline bool ValidateAndStripPath(
    const std::string& raw,
    const std::string& required_root,
    bool is_dir,
    std::string& stripped_out,
    std::string& err)
{
    if (raw.empty()) {
        err = "empty archive entry path";
        return false;
    }
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            err = "forbidden character in archive path: " + raw;
            return false;
        }
    }
    if (raw.front() == '/') {
        err = "absolute path in archive: " + raw;
        return false;
    }
    std::string trimmed = raw;
    if (is_dir) {
        while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
    } else if (trimmed.back() == '/') {
        err = "file entry ends with slash: " + raw;
        return false;
    }

    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= trimmed.size()) {
        const size_t slash = trimmed.find('/', pos);
        const std::string seg = (slash == std::string::npos) ? trimmed.substr(pos) : trimmed.substr(pos, slash - pos);
        if (seg.empty() || seg == "." || seg == "..") {
            err = "unsafe path segment in archive: " + raw;
            return false;
        }
        if (seg.back() == ' ' || seg.back() == '.') {
            err = "trailing space or dot in archive path: " + raw;
            return false;
        }
        if (IsWindowsReservedName(seg)) {
            err = "Windows reserved device name in archive path: " + raw;
            return false;
        }
        parts.push_back(seg);
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }

    size_t start_idx = 0;
    if (!required_root.empty()) {
        if (parts.empty() || parts[0] != required_root) {
            err = "archive entry escapes required root prefix: " + raw;
            return false;
        }
        start_idx = 1;
        if (is_dir && parts.size() == 1) {
            stripped_out.clear();
            return true;
        }
        if (start_idx >= parts.size()) {
            err = "empty path after stripping root prefix: " + raw;
            return false;
        }
    }

    stripped_out.clear();
    for (size_t i = start_idx; i < parts.size(); ++i) {
        if (i > start_idx) stripped_out.push_back('/');
        stripped_out += parts[i];
    }
    return true;
}

} // namespace detail

// Parses a ZIP buffer in memory, strictly validates all security/integrity invariants,
// and invokes `on_file(rel_path, bytes)` for every regular file entry.
// Returning false from `on_file` aborts extraction immediately.
inline bool ExtractZipBuffer(
    const uint8_t* zip_data,
    size_t zip_size,
    const std::string& required_root,
    const Limits& limits,
    const std::function<bool(const std::string& rel_path, const std::vector<uint8_t>& content, std::string& cb_err)>& on_file,
    EntryStats& stats_out,
    std::string& err)
{
    stats_out = {};
    if (!zip_data || zip_size < 22) {
        err = "archive is too small to be a valid ZIP";
        return false;
    }
    if (zip_size > limits.max_zip_bytes) {
        err = "archive exceeds maximum allowed size";
        return false;
    }

    // Locate EOCD scanning backwards from the end (max comment 65535 bytes)
    const size_t min_eocd = (zip_size > 65557) ? (zip_size - 65557) : 0;
    size_t eocd_off = std::string::npos;
    for (size_t i = zip_size - 22; ; --i) {
        if (ReadU32(zip_data + i) == 0x06054b50u) {
            const uint16_t cmt_len = ReadU16(zip_data + i + 20);
            if (i + 22 + cmt_len == zip_size) {
                eocd_off = i;
                break;
            }
        }
        if (i == min_eocd) break;
    }
    if (eocd_off == std::string::npos) {
        err = "ZIP end-of-central-directory record not found";
        return false;
    }

    const uint16_t disk_no = ReadU16(zip_data + eocd_off + 4);
    const uint16_t cd_disk = ReadU16(zip_data + eocd_off + 6);
    const uint16_t disk_entries = ReadU16(zip_data + eocd_off + 8);
    const uint16_t total_entries = ReadU16(zip_data + eocd_off + 10);
    const uint32_t cd_size = ReadU32(zip_data + eocd_off + 12);
    const uint32_t cd_off = ReadU32(zip_data + eocd_off + 16);

    if (disk_no != 0 || cd_disk != 0 || disk_entries != total_entries) {
        err = "multi-disk ZIP archives are not supported";
        return false;
    }
    if (total_entries == 0xFFFFu || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
        err = "ZIP64 archives are not supported";
        return false;
    }
    if (total_entries > limits.max_entries) {
        err = "archive entry count exceeds safety limit";
        return false;
    }
    if (static_cast<uint64_t>(cd_off) + static_cast<uint64_t>(cd_size) > eocd_off) {
        err = "ZIP central directory bounds out of range";
        return false;
    }

    std::unordered_set<std::string> seen_files_lower;
    size_t cur = cd_off;
    const size_t cd_end = static_cast<size_t>(cd_off) + static_cast<size_t>(cd_size);
    std::vector<uint8_t> unpacked;

    for (uint16_t idx = 0; idx < total_entries; ++idx) {
        if (cur + 46 > cd_end || ReadU32(zip_data + cur) != 0x02014b50u) {
            err = "corrupted ZIP central directory record";
            return false;
        }
        const uint16_t made_by = ReadU16(zip_data + cur + 4);
        const uint16_t flags = ReadU16(zip_data + cur + 8);
        const uint16_t method = ReadU16(zip_data + cur + 10);
        const uint32_t crc_expected = ReadU32(zip_data + cur + 16);
        const uint32_t comp_size = ReadU32(zip_data + cur + 20);
        const uint32_t uncomp_size = ReadU32(zip_data + cur + 24);
        const uint16_t name_len = ReadU16(zip_data + cur + 28);
        const uint16_t extra_len = ReadU16(zip_data + cur + 30);
        const uint16_t cmt_len = ReadU16(zip_data + cur + 32);
        const uint32_t ext_attr = ReadU32(zip_data + cur + 38);
        const uint32_t lfh_off = ReadU32(zip_data + cur + 42);

        const size_t rec_len = 46ull + name_len + extra_len + cmt_len;
        if (cur + rec_len > cd_end) {
            err = "truncated ZIP central directory metadata";
            return false;
        }
        if (flags & 0x0001u) {
            err = "encrypted ZIP entries are not supported";
            return false;
        }
        if (comp_size == 0xFFFFFFFFu || uncomp_size == 0xFFFFFFFFu || lfh_off == 0xFFFFFFFFu) {
            err = "ZIP64 entry is not supported";
            return false;
        }

        // Reject Unix symlinks / FIFOs / device nodes
        if ((made_by >> 8) == 3) {
            const uint32_t mode_type = (ext_attr >> 16) & 0xF000u;
            if (mode_type != 0 && mode_type != 0x8000u && mode_type != 0x4000u) {
                err = "symlink or special file rejected in ZIP";
                return false;
            }
        }

        const std::string raw_name(reinterpret_cast<const char*>(zip_data + cur + 46), name_len);
        cur += rec_len;

        const bool is_dir = !raw_name.empty() && raw_name.back() == '/';
        std::string rel_path;
        if (!detail::ValidateAndStripPath(raw_name, required_root, is_dir, rel_path, err))
            return false;

        if (is_dir) {
            if (uncomp_size != 0 || comp_size != 0) {
                err = "directory entry carries unexpected data: " + raw_name;
                return false;
            }
            continue;
        }

        const std::string low_rel = detail::ToLowerAscii(rel_path);
        if (!seen_files_lower.insert(low_rel).second) {
            err = "duplicate or case-colliding path in ZIP: " + rel_path;
            return false;
        }

        if (uncomp_size > limits.max_single_file_bytes) {
            err = "single file exceeds safety size limit: " + rel_path;
            return false;
        }
        if (stats_out.unpacked_bytes + uncomp_size > limits.max_unpacked_bytes) {
            err = "total unpacked archive size exceeds safety limit";
            return false;
        }

        if (static_cast<uint64_t>(lfh_off) + 30 > cd_off || ReadU32(zip_data + lfh_off) != 0x04034b50u) {
            err = "invalid ZIP local file header: " + rel_path;
            return false;
        }
        const uint16_t lf_method = ReadU16(zip_data + lfh_off + 8);
        const uint16_t lf_name_len = ReadU16(zip_data + lfh_off + 26);
        const uint16_t lf_extra_len = ReadU16(zip_data + lfh_off + 28);
        if (lf_method != method) {
            err = "ZIP local/central method mismatch: " + rel_path;
            return false;
        }
        const uint64_t data_off = static_cast<uint64_t>(lfh_off) + 30ull + lf_name_len + lf_extra_len;
        if (data_off + comp_size > cd_off) {
            err = "ZIP compressed payload bounds out of range: " + rel_path;
            return false;
        }

        const uint8_t* payload_ptr = zip_data + data_off;
        if (method == 0) {
            if (comp_size != uncomp_size) {
                err = "stored entry compressed/uncompressed size mismatch: " + rel_path;
                return false;
            }
            unpacked.assign(payload_ptr, payload_ptr + uncomp_size);
        } else if (method == 8) {
            std::string inf_err;
            if (!detail::InflateRaw(payload_ptr, comp_size, uncomp_size, limits.max_single_file_bytes, unpacked, inf_err)) {
                err = "inflate failed for " + rel_path + ": " + inf_err;
                return false;
            }
        } else {
            err = "unsupported ZIP compression method for: " + rel_path;
            return false;
        }

        if (Crc32(unpacked.data(), unpacked.size()) != crc_expected) {
            err = "CRC-32 mismatch for: " + rel_path;
            return false;
        }

        stats_out.file_count++;
        stats_out.unpacked_bytes += unpacked.size();

        std::string cb_err;
        if (!on_file(rel_path, unpacked, cb_err)) {
            err = cb_err.empty() ? ("callback rejected entry: " + rel_path) : cb_err;
            return false;
        }
    }

    if (cur != cd_end) {
        err = "trailing bytes in ZIP central directory";
        return false;
    }
    return true;
}

} // namespace mini_unzip
