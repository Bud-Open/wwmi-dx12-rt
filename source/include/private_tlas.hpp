#pragma once
#include "rt_scene_consumer.hpp"
#include "gpu_helpers.hpp"
#include <algorithm>

namespace hsin_rt {
struct AuthorSceneSlot {
    unsigned instance_index;
    D3D12_RAYTRACING_INSTANCE_DESC original;
    std::shared_ptr<const NativeBlasIdentity> source;
    std::shared_ptr<GeometryBatch> geometry;
};

// An immutable private scene input for one already identified native TLAS.
// Callers supply bytes from the *actual* InstanceDescs view of this native
// build, with resource lifetime/creation, producer and submit proven. They
// select the exact instance index by the actual 64-byte instance and shared
// author plan, not by searching for a matching AS address. The original
// command and resource are never modified.
class PrivateTlas {
    ComPtr<ID3D12Resource> instances_,scratch_,tlas_;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_{};
    std::vector<std::shared_ptr<GeometryBatch>> geometry_;
    Recording recording_{};
    bool recorded_=false;
public:
    static std::shared_ptr<PrivateTlas> prepare(ID3D12Device5 *device,Recording consumer,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &original,
        std::span<const D3D12_RAYTRACING_INSTANCE_DESC> exact_instances,
        std::span<const AuthorSceneSlot> selected) {
        static_assert(sizeof(D3D12_RAYTRACING_INSTANCE_DESC)==64);
        const auto &inputs=original.Inputs;
        if(!device||!consumer.list||inputs.Type!=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL||
           inputs.DescsLayout!=D3D12_ELEMENTS_LAYOUT_ARRAY||!inputs.InstanceDescs||
           !inputs.NumDescs||inputs.NumDescs!=exact_instances.size()||selected.empty())return {};
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> copy(exact_instances.begin(),exact_instances.end());
        auto scene=std::make_shared<PrivateTlas>();
        scene->recording_=consumer;
        for(const auto &slot:selected){
            if(!slot.source||!slot.geometry||!slot.geometry->assets||!slot.geometry->assets->pack||!slot.geometry->blas||
               slot.instance_index>=copy.size()||std::memcmp(&copy[slot.instance_index],&slot.original,sizeof(slot.original))||
               !slot.original.AccelerationStructure||slot.original.AccelerationStructure!=slot.source->native_blas)return {};
            const auto &batch=*slot.geometry;
            const auto &native=*slot.source;
            if(batch.producer.recording!=native.recording||batch.producer.geometries.size()!=native.components.size()||
               batch.source_orders.size()!=native.components.size()||batch.geometries.size()!=native.components.size())return {};
            for(size_t i=0;i<native.components.size();++i){
                const auto &a=batch.producer.geometries[i];const auto &n=native.components[i];
                if(n.sample.profile!=batch.assets->pack->native||n.sample.component!=i||batch.source_orders[i]!=n.sample.order||
                   a.vertex!=n.vertices.vertex||a.offset!=n.vertices.offset||a.bytes!=n.vertices.bytes)return {};
            }
            auto &value=copy[slot.instance_index];
            value.AccelerationStructure=batch.blas->GetGPUVirtualAddress();
            if(!value.AccelerationStructure||value.AccelerationStructure==slot.original.AccelerationStructure)return {};
            scene->geometry_.push_back(slot.geometry);
        }
        // Every original transform, mask, contribution and instance flag
        // stays identical, including other scene objects. This is a full
        // private Build, never UPDATE on an allocation we don't own.
        scene->build_.Inputs=inputs;
        scene->build_.Inputs.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            unsigned(inputs.Flags)&~unsigned(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE));
        scene->instances_=gpu::buffer(device,std::uint64_t(copy.size())*64,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        gpu::upload(scene->instances_.Get(),copy.data(),copy.size()*64);
        scene->build_.Inputs.InstanceDescs=scene->instances_->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&scene->build_.Inputs,&info);
        if(!info.ResultDataMaxSizeInBytes||!info.ScratchDataSizeInBytes)return {};
        scene->scratch_=gpu::buffer(device,info.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        scene->tlas_=gpu::buffer(device,info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        scene->build_.ScratchAccelerationStructureData=scene->scratch_->GetGPUVirtualAddress();
        scene->build_.DestAccelerationStructureData=scene->tlas_->GetGPUVirtualAddress();
        return scene;
    }
    std::uint64_t address()const{return build_.DestAccelerationStructureData;}
    // Invoke only at the actual legal consumer, once the complete table/SBT
    // pair is prepared. Caller holds this object before recording and in
    // every actual submission; all private BLAS inputs must be ordered.
    bool record(Recording consumer,ID3D12GraphicsCommandList4 *list){
        if(!list||recorded_||consumer!=recording_)return false;
        recorded_=true;
        list->BuildRaytracingAccelerationStructure(&build_,0,nullptr);
        gpu::uav_barrier(list,tlas_.Get());
        return true;
    }
};
}
