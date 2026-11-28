#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

namespace hsin_rt {
struct GeometryBatch;
struct GeometryAssets;
struct Pose;
// Device-local; Runtime serializes acquire and release. A GeometryBatch aliases its
// lease, so history, replayable recording, and every flight keep storage busy.
struct AuthorStreamPool {
    struct Storage {
        Microsoft::WRL::ComPtr<ID3D12Resource> positions,tangents;
        std::uint64_t position_capacity=0,tangent_capacity=0;
        std::weak_ptr<GeometryBatch> resident;
        std::uint64_t generation=0;
        unsigned coverage=UINT32_MAX;
        bool default_static=false;
    };
    struct PaletteStorage {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        void *mapped=nullptr;
        std::uint64_t capacity=0;
        ~PaletteStorage(){if(resource&&mapped){D3D12_RANGE none{0,0};resource->Unmap(0,&none);}}
    };
    std::vector<std::shared_ptr<Storage>> storage;
    std::vector<std::shared_ptr<PaletteStorage>> palettes;
    std::shared_ptr<Storage> acquire(ID3D12Device5 *,std::uint64_t positions,std::uint64_t tangents,bool &reused);
    std::shared_ptr<PaletteStorage> acquire_palette(ID3D12Device5 *,std::uint64_t bytes);
    bool empty()const noexcept{return storage.empty()&&palettes.empty();}
    void release_idle()noexcept;
};
std::shared_ptr<GeometryBatch> prepare_pooled_author_streams(std::shared_ptr<GeometryAssets>,const Pose &,
    unsigned component,Microsoft::WRL::ComPtr<ID3D12Resource> *palette,AuthorStreamPool &,bool &reused,
    std::uint64_t generation,bool default_static,bool want_tangents=true);
}
