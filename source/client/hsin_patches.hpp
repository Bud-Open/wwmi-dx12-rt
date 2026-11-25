#pragma once
#include "hsin_geometry.hpp"
#include "sbt_patch.hpp"
namespace hsin_rt {
struct NativeBlasIdentity;
struct Target {
    std::uint64_t native_blas=0;
    ResourceKey native_resource;
    std::shared_ptr<const NativeBlasIdentity> native_identity;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> native_geometry;
    std::shared_ptr<GeometryBatch> geometry;
};
struct PatchAssets {
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12RootSignature> instances_root,sbt_root;
    ComPtr<ID3D12PipelineState> instances_pso,sbt_pso;
    static std::shared_ptr<PatchAssets> create(ID3D12Device5 *,std::span<const unsigned char> instances,std::span<const unsigned char> sbt);
};
struct InstanceBatch {
    std::shared_ptr<PatchAssets> assets;
    std::vector<std::shared_ptr<Target>> targets;
    std::array<ComPtr<ID3D12Resource>,2> copies;
    std::uint64_t source=0;
    UINT count=0;
    bool recording_started=false,recorded=false;
    static std::shared_ptr<InstanceBatch> prepare(std::shared_ptr<PatchAssets>,std::uint64_t,UINT,std::vector<std::shared_ptr<Target>>);
    std::uint64_t address() const;
    std::int32_t record(std::uint64_t,ComputeRunFn);
};
struct SbtBatch {
    std::shared_ptr<PatchAssets> assets;
    std::vector<std::shared_ptr<Target>> targets;
    ComPtr<ID3D12Resource> rules,copy;
    ShaderId shader{};
    std::uint64_t source=0;
    UINT records=0,rule_count=0;
    bool recording_started=false,recorded=false;
    static std::shared_ptr<SbtBatch> prepare(std::shared_ptr<PatchAssets>,std::uint64_t,std::uint64_t,const ShaderId &,std::vector<std::shared_ptr<Target>>);
    std::int32_t record(std::uint64_t,ComputeRunFn);
};
}
