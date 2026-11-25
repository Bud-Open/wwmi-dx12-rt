#pragma once
#include "private_tlas.hpp"
#include "rt_author_draw.h"
#include <optional>

namespace hsin_rt {
// A real writer is independent of the recording that uses InstanceDescs.
// For an upload the producer must actually seal those bytes; for a command
// it must prove this is the completed content of that exact GPU write.
struct OcclusionBufferWrite {
    enum Kind { unknown, sealed_upload, command_snapshot } kind=unknown;
    ResourceKey resource;
    Recording recording;
    std::uint64_t byte_origin=0,bytes=0,order=0;
    NativeIdentityFence submit;
    unsigned submissions=0;
    bool operator==(const OcclusionBufferWrite &)const=default;
    bool covers(const InputCollector::RayInputRange &read)const{
        if(!resource||resource!=read.key||!order||read.offset<byte_origin||!read.bytes)return false;
        const auto within=read.offset-byte_origin;
        if(within>bytes||read.bytes>bytes-within)return false;
        if(kind==sealed_upload)return !recording.list&&!submissions&&!submit.queue&&!submit.fence&&!submit.value&&
            read.heap==api::memory_heap::upload;
        return kind==command_snapshot&&recording.list&&recording.creation&&recording.generation&&submissions==1&&
            submit.queue&&submit.fence&&submit.value;
    }
};
using OcclusionInstanceWrite=OcclusionBufferWrite;
// Keep immutable bytes from the original Build, not the newest bytes at its
// address. The interval is exactly the full ARRAY Build view (64 * count).
struct OcclusionNativeScene {
    RayBuildUse build;
    OcclusionInstanceWrite writer;
    std::span<const D3D12_RAYTRACING_INSTANCE_DESC> instances;
};
// From the real instance producer's association with a captured raster
// Primitive. Matching BLAS addresses or transforms cannot create this link.
struct OcclusionPrimitiveSource {
    ResourceKey resource;
    std::uint64_t byte_origin=0;
    // The immutable original Primitive window from the shared source. This
    // is not a transform comparison against an arbitrary upload allocation.
    std::span<const std::uint32_t> words;
};
struct OcclusionInstanceLink {
    ResourceKey scene;
    Recording build_recording;
    std::shared_ptr<const NativeBlasIdentity> native_blas;
    std::uint64_t build_order=0;
    OcclusionInstanceWrite writer;
    unsigned index=UINT32_MAX;
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    OcclusionPrimitiveSource primitive;
};
inline bool occlusion_build_matches(const RayBuildUse &actual,const RayBuildUse &sealed){
    return actual.order&&actual.order==sealed.order&&actual.recording==sealed.recording&&
        actual.destination&&actual.destination==sealed.destination&&actual.address==sealed.address&&
        actual.instances_va==sealed.instances_va&&actual.count==sealed.count&&actual.flags==sealed.flags&&
        actual.instances.key==sealed.instances.key&&actual.instances.offset==sealed.instances.offset&&
        actual.instances.bytes==sealed.instances.bytes&&actual.instances.heap==sealed.instances.heap;
}
inline bool occlusion_instance_matches(const OcclusionNativeScene &scene,const OcclusionInstanceLink &link){
    static_assert(sizeof(D3D12_RAYTRACING_INSTANCE_DESC)==64);
    const auto &build=scene.build;
    return build.instances_va&&build.destination==link.scene&&build.recording.list&&
        build.recording==link.build_recording&&build.order&&build.order==link.build_order&&
        scene.writer==link.writer&&scene.writer.covers(build.instances)&&
        build.count==scene.instances.size()&&build.instances.bytes==scene.instances.size_bytes()&&link.index<scene.instances.size()&&
        link.native_blas&&link.primitive.resource&&
        std::memcmp(&scene.instances[link.index],&link.instance,sizeof(link.instance))==0;
}

// The native producer can run before the author draw. It supplies the source
// identity, not a Mod plan id. Join at the actual consumer, then use that
// scope's frozen recipe, not the latest revision from a different Primitive.
template<class Plan> bool occlusion_primitive_matches(const Plan &plan,const OcclusionInstanceLink &link){
    return plan.identity&&plan.identity==link.native_blas&&plan.primitive==link.primitive.resource&&
        plan.scope.byte_origin==link.primitive.byte_origin&&
        link.primitive.words.size()==std::size(plan.scope.primitive_words)&&
        std::memcmp(plan.scope.primitive_words,link.primitive.words.data(),sizeof(plan.scope.primitive_words))==0;
}
template<class Plan> bool occlusion_author_matches(const Plan &plan,const OcclusionInstanceLink &link){
    return occlusion_primitive_matches(plan,link)&&plan.disposition==WWMI_RT_AUTHOR_PRIVATE&&
        plan.pack&&plan.pose&&plan.pose->pack==plan.pack&&plan.pose->recording==plan.identity->recording&&
        plan.identity->native_blas==link.instance.AccelerationStructure&&
        plan.pack->load_generation==plan.load&&plan.pack->key==plan.mod&&plan.pack->revision;
}

// Builds one new AS from the already selected current author positions/IB.
// Never redo skin/tangent dispatches or attach a different BLAS to the raster
// batch; that batch can still be read by a retained graphics recording.
class OcclusionScene {
    std::shared_ptr<const NativeBlasIdentity> native_;
    std::shared_ptr<GeometryBatch> surface_,acceleration_;
    std::shared_ptr<PrivateTlas> tlas_;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_{};
    Recording recording_;
    bool recorded_=false;
public:
    // Call only with the actual active scene allocation and an ordered,
    // immutable snapshot of its Build/InstanceDescs. The author surface is
    // held by this job; its owned prefix or completed submission must be
    // committed before any ray or AS work uses it.
    static std::shared_ptr<OcclusionScene> prepare(ID3D12Device5 *device,Recording consumer,const RaySceneInput &ray,
        ResourceKey active_scene,const OcclusionNativeScene &native_scene,const OcclusionInstanceLink &link,
        const std::shared_ptr<const NativeBlasIdentity> &identity,const Pose &pose,std::shared_ptr<GeometryBatch> author_surface){
        // The selected Primitive authorizes the row. The geometry inputs
        // themselves are just the immutable source identity and the pose/recipe.
        if(!device||!consumer.list||!is_occlusion(ray)||active_scene!=native_scene.build.destination||
            ray.as.location!=native_scene.build.address||!occlusion_instance_matches(native_scene,link)||
            !identity||identity!=link.native_blas||!pose.pack||pose.recording!=identity->recording||
            !author_surface||!author_surface->recorded||
            author_surface->single_component!=UINT32_MAX||!author_surface->assets||author_surface->assets->pack!=pose.pack||
            author_surface->producer.recording!=pose.recording||author_surface->source_orders!=pose.source_orders||
            !author_surface->positions)return {};
        const auto &source=*identity;
        const auto n=source.components.size();
        if(!n||n!=pose.pack->components.size()||n!=source.geometries.size()||
            n!=author_surface->producer.geometries.size()||n!=author_surface->source_orders.size())return {};
        std::vector<D3D12_RAYTRACING_GEOMETRY_FLAGS> flags;
        flags.reserve(n);
        for(size_t i=0;i<n;++i){
            const auto &g=source.geometries[i];
            const auto &sample=source.components[i].sample;
            const auto &b=author_surface->producer.geometries[i];const auto &s=source.components[i].vertices;
            if(g.Type!=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES||g.Triangles.Transform3x4||
               sample.order!=author_surface->source_orders[i]||sample.component!=i||sample.profile!=pose.pack->native||
               b.vertex!=s.vertex||b.offset!=s.offset||b.bytes!=s.bytes)return {};
            flags.push_back(g.Flags);
        }
        auto job=std::make_shared<OcclusionScene>();
        job->recording_=consumer;job->native_=identity;job->surface_=std::move(author_surface);
        auto a=std::make_shared<GeometryBatch>();
        a->assets=job->surface_->assets;a->positions=job->surface_->positions;
        a->producer=job->surface_->producer;a->source_orders=job->surface_->source_orders;
        a->prepare_acceleration(flags);
        job->blas_.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        job->blas_.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        job->blas_.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
        job->blas_.Inputs.NumDescs=UINT(a->geometries.size());
        job->blas_.Inputs.pGeometryDescs=a->geometries.data();
        job->blas_.DestAccelerationStructureData=a->blas->GetGPUVirtualAddress();
        job->blas_.ScratchAccelerationStructureData=a->scratch->GetGPUVirtualAddress();
        const AuthorSceneSlot selected{link.index,link.instance,job->native_,a};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC original{};
        original.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        original.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        original.Inputs.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(native_scene.build.flags);
        original.Inputs.NumDescs=native_scene.build.count;
        original.Inputs.InstanceDescs=native_scene.build.instances_va;
        job->tlas_=PrivateTlas::prepare(device,consumer,original,native_scene.instances,{&selected,1});
        if(!job->tlas_)return {};
        job->acceleration_=std::move(a);
        return job;
    }
    std::uint64_t address()const{return tlas_->address();}
    const std::shared_ptr<GeometryBatch> &surface()const{return surface_;}
    bool can_record(Recording current)const{return !recorded_&&recording_==current;}
    // Caller has a valid original DispatchRays scope outside RenderPass, has
    // committed the exact position prefix, and holds this job for replays and
    // every actual submission. Neither build changes graphics/compute roots.
    void record(Recording current,ID3D12GraphicsCommandList4 *list){
        recorded_=true;
        list->BuildRaytracingAccelerationStructure(&blas_,0,nullptr);
        gpu::uav_barrier(list,acceleration_->blas.Get());
        tlas_->record(current,list);
    }
};
}
