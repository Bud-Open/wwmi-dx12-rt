#pragma once
#include "occlusion_inputs.hpp"
#include "occlusion_scene.hpp"

namespace hsin_rt {
struct OcclusionTablePage {
    ComPtr<ID3D12DescriptorHeap> heap;
    std::shared_ptr<const void> reservation; // real recording-page lease
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    unsigned available=0;
};
// When the actual graph cannot prove every eligible original MainRG/Miss
// record is compatible, the caller still needs real immutable prefixes.
struct OcclusionIdentifierUse {
    OcclusionBufferWrite writer;
    // Actual producer's no-rewrite lease, not just an AddRef on an upload.
    std::shared_ptr<const void> lease;
    ShaderId identifier{};
    bool covers(InputCollector::RayInputRange read)const{
        if(!lease||read.bytes<sizeof(identifier))return false;
        read.bytes=sizeof(identifier);
        return writer.covers(read);
    }
};
struct OcclusionProgramUse {
    Recording consumer;
    std::uint64_t state=0,root=0;
    OcclusionIdentifierUse raygen,miss0;
    bool matches(const RaygenIdentity &identities,Recording current,const RaySceneInput &ray,const OcclusionBindingRead &binding)const{
        if(current!=consumer||!occlusion_known_no_procedural(ray)||state!=ray.compute.pipeline||
           root!=ray.compute.root_signature||binding.recording!=current)return false;
        if(binding.closed_sbt){
            const auto proof=identities.closed_occlusion_records({state},root,binding.table_layout.generation);
            return proof==binding.closed_sbt&&ray.raygen.key&&binding.miss.key&&
                ray.raygen.bytes>=proof->raygen&&binding.miss.bytes>=proof->miss&&
                (!binding.miss_table.StrideInBytes||binding.miss_table.StrideInBytes>=proof->miss);
        }
        if(!raygen.covers(ray.raygen)||!miss0.covers(binding.miss))return false;
        const auto active=identities.select({state},std::as_bytes(std::span{raygen.identifier}));
        return active&&active->name==ray.program->name&&
            identities.matches_occlusion_miss({state},std::as_bytes(std::span{miss0.identifier}));
    }
};

class OcclusionPass {
    Recording recording_;
    D3D12_DISPATCH_RAYS_DESC dispatch_{};
    WWMI_ComputeRestoreInfo_v1 restore_{sizeof(restore_)};
    std::array<WWMI_ComputeRestoreRecord_v1,64> compute_{};
    std::vector<WWMI_GraphicsRestoreRecord_v1> graphics_;
    ComPtr<ID3D12GraphicsCommandList4> native_;
    std::shared_ptr<OcclusionScene> scene_;
    OcclusionTablePage page_;
    std::array<std::shared_ptr<const void>,2> shader_use_;
    std::array<ComPtr<ID3D12Resource>,2> original_program_tables_;
    ComPtr<ID3D12Resource> rays_,output_,output_counter_;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_{},uav_{};
public:
    static constexpr unsigned descriptors=64+16;
    bool can_issue(Recording current)const{return recording_==current&&native_&&scene_->can_record(current);}
    // Call synchronously in the original DispatchRays. The real 1.1+
    // non-volatile ranges protect the copied descriptor values. They make no
    // promise about the separate SBT resource bytes.
    static std::shared_ptr<OcclusionPass> prepare(ID3D12Device5 *device,api::command_list *list,Recording current,
        const RaySceneInput &ray,const D3D12_DISPATCH_RAYS_DESC &dispatch,const RaygenIdentity &identities,
        const OcclusionProgramUse &program,const OcclusionBindingRead &binding,
        std::shared_ptr<OcclusionScene> scene,OcclusionTablePage page){
        if(!device||!list||!scene||!scene->can_record(current)||
           binding.stage!=OcclusionBindingRead::ready||binding.recording!=current||
           !binding.table_layout.immutable_descriptors()||!program.matches(identities,current,ray,binding)||
           binding.miss_table.StartAddress!=dispatch.MissShaderTable.StartAddress||
           binding.miss_table.SizeInBytes!=dispatch.MissShaderTable.SizeInBytes||
           binding.miss_table.StrideInBytes!=dispatch.MissShaderTable.StrideInBytes||
           !dispatch.Width||!dispatch.Height||!dispatch.Depth||!page.reservation||!page.heap||
           page.available<descriptors||!page.cpu.ptr||!page.gpu.ptr)return {};
        const auto address=list->get_native();
        auto *base=reinterpret_cast<ID3D12GraphicsCommandList *>(address);
        const auto &compute=binding.compute_state;const auto &graphics=binding.graphics_state;
        const auto &rays=binding.rays;const auto &output=binding.output;
        if(!base||address!=graphics.native_list||address!=compute.native_list||
           compute.cbv_srv_uav_heap==reinterpret_cast<std::uint64_t>(page.heap.Get())||
           rays.elements<dispatch.Width||output.elements<dispatch.Width)return {};
        auto p=std::make_shared<OcclusionPass>();
        if(FAILED(base->QueryInterface(IID_PPV_ARGS(&p->native_))))return {};
        p->recording_=current;p->dispatch_=dispatch;p->scene_=std::move(scene);p->page_=std::move(page);
        if(binding.closed_sbt){
            // Both exact backing allocations were revalidated in the
            // same original callback. Keep the native ranges and do not
            // claim to freeze the game's future writes into those ranges.
            p->original_program_tables_[0]=reinterpret_cast<ID3D12Resource *>(ray.raygen.key.object);
            p->original_program_tables_[1]=reinterpret_cast<ID3D12Resource *>(binding.miss.key.object);
        }else p->shader_use_={program.raygen.lease,program.miss0.lease};
        p->restore_=compute;p->compute_=binding.compute_arguments;
        p->graphics_.assign(binding.graphics_tables.begin(),binding.graphics_tables.begin()+graphics.record_count);
        // The host's borrowed resource identities are only valid in this
        // original callback. Retain them before creating the private views.
        p->rays_=reinterpret_cast<ID3D12Resource *>(rays.resource);
        p->output_=reinterpret_cast<ID3D12Resource *>(output.resource);
        p->output_counter_=reinterpret_cast<ID3D12Resource *>(output.counter);
        const auto inc=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto dst=[&](unsigned i){return D3D12_CPU_DESCRIPTOR_HANDLE{p->page_.cpu.ptr+SIZE_T(i)*inc};};
        D3D12_SHADER_RESOURCE_VIEW_DESC source_srv{};
        source_srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;source_srv.Format=DXGI_FORMAT(rays.format);
        source_srv.Shader4ComponentMapping=rays.shader_mapping;
        source_srv.Buffer={rays.first_element,rays.elements,rays.stride,D3D12_BUFFER_SRV_FLAGS(rays.flags)};
        device->CreateShaderResourceView(p->rays_.Get(),&source_srv,dst(1));
        D3D12_UNORDERED_ACCESS_VIEW_DESC source_uav{};
        source_uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;source_uav.Format=DXGI_FORMAT(output.format);
        source_uav.Buffer={output.first_element,output.elements,output.stride,output.counter_offset_bytes,D3D12_BUFFER_UAV_FLAGS(output.flags)};
        device->CreateUnorderedAccessView(p->output_.Get(),p->output_counter_.Get(),&source_uav,dst(64+occlusion_output_slot.offset));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.RaytracingAccelerationStructure.Location=p->scene_->address();
        device->CreateShaderResourceView(nullptr,&srv,dst(0));
        // Fill the rest of these finite table ranges with typed nulls. The
        // verified active program has no reads of these physical offsets.
        srv={};srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements=1;srv.Buffer.StructureByteStride=32;
        for(unsigned i=2;i<64;++i)device->CreateShaderResourceView(nullptr,&srv,dst(i));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements=1;uav.Buffer.StructureByteStride=4;
        for(unsigned i=65;i<descriptors;++i)device->CreateUnorderedAccessView(nullptr,nullptr,&uav,dst(i));
        p->srv_=p->page_.gpu;p->uav_={p->page_.gpu.ptr+64ull*inc};
        return p;
    }
    // Caller installs the pass in the existing recording holds and commits
    // its already prepared author prefix BEFORE entering. No operation below
    // can fall back to the original after recording a partial private dispatch.
    // Return true only to cancel the one SDK original DispatchRays.
    bool issue(Recording current){
        if(!can_issue(current))return false;
        const auto native=std::move(native_);
        auto *list=native.Get();
        scene_->record(current,list);
        auto *samplers=reinterpret_cast<ID3D12DescriptorHeap *>(restore_.sampler_heap);
        ID3D12DescriptorHeap *private_pair[]{page_.heap.Get(),samplers};
        list->SetDescriptorHeaps(samplers?2:1,private_pair);
        list->SetComputeRootDescriptorTable(scene_srv_parameter,srv_);
        list->SetComputeRootDescriptorTable(scene_uav_parameter,uav_);
        list->DispatchRays(&dispatch_); // original state object, dimensions and all four SBT ranges
        ID3D12DescriptorHeap *original_pair[]{reinterpret_cast<ID3D12DescriptorHeap *>(restore_.cbv_srv_uav_heap),samplers};
        list->SetDescriptorHeaps(samplers?2:1,original_pair);
        for(const auto &r:graphics_)list->SetGraphicsRootDescriptorTable(r.parameter,{r.table});
        for(unsigned i=0;i<restore_.record_count;++i){
            const auto &r=compute_[i];
            if(r.kind==WWMI_COMPUTE_ROOT_TABLE)list->SetComputeRootDescriptorTable(r.parameter,{r.value});
        }
        return true;
    }
};
}
