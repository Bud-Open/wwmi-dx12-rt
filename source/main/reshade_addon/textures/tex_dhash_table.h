#pragma once

#include "../migoto/port_api.h"
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

// Keep decodable single-slice 2D mips as uploads arrive so 3Dmigoto's
// FindOptimalMip can honor any dhash_mip_size and fall back across streamed mips.
// Match copies by mip size (streaming can move a mip to another subresource index).
class TexDHashTable {
public:
    struct UploadFootprint {
        uint32_t row_pitch = 0; // 0 falls back to the minimum 256-aligned pitch
        uint32_t footprint_height = 0;
        uint32_t src_left = 0;
        uint32_t src_top = 0;
        uint32_t src_front = 0;
        bool     has_src_box = false;
    };

    void SetEnabled(bool enabled);
    void FromUpload(ID3D12Resource* src, uint64_t offset, ID3D12Resource* dst, uint32_t sub,
        const UploadFootprint* fp = nullptr);
    void FromData(ID3D12Resource* dst, uint32_t sub, const uint8_t* data, size_t row_pitch);
    void Copy(uint64_t src, uint32_t src_sub, uint64_t dst, uint32_t dst_sub);
    void CopyAll(uint64_t src, uint64_t dst);
    void Unknown(uint64_t res, uint32_t sub);
    void Forget(uint64_t res);
    bool Lookup(ID3D12Resource* res, uint32_t mip, uint32_t view_fmt, uint64_t* out);
    bool State(ID3D12Resource* res, uint32_t view_fmt, PortApi::TextureDHashState* out);

private:
    struct Entry {
        uint32_t w = 0, h = 0;
        uint64_t linear = 0, srgb = 0;
        uint64_t gen = 0;
    };
    struct ResState {
        uint64_t           resource_gen = 0;
        std::vector<Entry> mips;
        bool               has_cached = false;
        bool               cached_srgb = false;
        uint64_t           cached_dhash = 0;
        uint64_t           cached_gen = 0;
    };

    void Store(uint64_t res, Entry entry, uint64_t enable_gen);
    void Drop(uint64_t res, uint32_t w, uint32_t h, uint64_t enable_gen);
    bool Enabled() const { return enabled_.load(std::memory_order_acquire); }

    std::atomic<bool> enabled_{true};
    std::atomic<uint64_t> enable_gen_{0};
    std::mutex mutex_;
    std::unordered_map<uint64_t, ResState> by_res_;
};

inline uint32_t DHashMipSide(uint64_t side, uint32_t mip)
{
    const uint64_t dim = mip < 64 ? side >> mip : 0;
    return dim ? static_cast<uint32_t>(dim) : 1u; // only used for D3D12 texture dimensions
}

extern bool g_tex_dhash_filters; // INI/overlay value; hooks use the table's published setting.
extern TexDHashTable g_tex_dhash;

bool TexDHashLookup(ID3D12Resource* res, unsigned mip, unsigned view_format, uint64_t* out);
bool TexDHashState(ID3D12Resource* res, unsigned view_format, PortApi::TextureDHashState* out);
