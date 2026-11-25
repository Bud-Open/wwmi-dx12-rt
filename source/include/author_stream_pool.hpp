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
    std::vector<std::shared_ptr<Storage>> storage;
    std::shared_ptr<Storage> acquire(ID3D12Device5 *,std::uint64_t positions,std::uint64_t tangents,bool &reused);
    void release_idle()noexcept;
};
std::shared_ptr<GeometryBatch> prepare_pooled_author_streams(std::shared_ptr<GeometryAssets>,const Pose &,
    unsigned component,Microsoft::WRL::ComPtr<ID3D12Resource> *palette,AuthorStreamPool &,bool &reused,
    std::uint64_t generation,bool default_static);
}
