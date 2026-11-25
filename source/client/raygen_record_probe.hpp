#pragma once
#include "occlusion_inputs.hpp"
#include "gpu_helpers.hpp"
#include "rt_native_root_flags.hpp"

namespace hsin_rt {
// Only the two descriptor-table sets are changed. In particular TABLES
// restore remains sufficient when an earlier ExecuteIndirect invalidated
// knowledge of non-table root parameters.
struct RaygenProbeBindings {
    WWMI_ComputeRestoreInfo_v1 compute{sizeof(compute)};
    WWMI_GraphicsRestoreInfo_v1 graphics{sizeof(graphics)};
    std::array<WWMI_ComputeRestoreRecord_v1,64> compute_args{};
    std::array<WWMI_GraphicsRestoreRecord_v1,64> graphics_args{};
    std::uint64_t original_as_table=0;

    int read(std::uint64_t list,std::uint64_t native,std::uint64_t root,
        CaptureOcclusionRestoreFn read_compute,CaptureOcclusionGraphicsFn read_graphics){
        if(const auto status=read_graphics(list,&graphics,graphics_args.data(),unsigned(graphics_args.size())))
            return 10+status;
        if(graphics.version!=1||graphics.native_list!=native||!graphics.outside_render_pass||
           (graphics.level!=WWMI_GRAPHICS_RESTORE_EMPTY&&graphics.level!=WWMI_GRAPHICS_RESTORE_TABLES)||
           (graphics.level==WWMI_GRAPHICS_RESTORE_EMPTY)!=(graphics.root_signature==0)||
           graphics.record_count>graphics_args.size())return 10;
        std::uint32_t words[64];
        if(const auto status=read_compute(list,&compute,compute_args.data(),unsigned(compute_args.size()),words,64))
            return 20+status;
        if(compute.version!=1||compute.native_list!=native||compute.root_signature!=root||
           (compute.level!=WWMI_COMPUTE_RESTORE_TABLES&&compute.level!=WWMI_COMPUTE_RESTORE_ROOT_ARGUMENTS)||
           compute.record_count>compute_args.size()||!compute.cbv_srv_uav_heap||
           compute.cbv_srv_uav_heap!=graphics.cbv_srv_uav_heap||compute.sampler_heap!=graphics.sampler_heap)return 20;
        for(unsigned i=0;i<compute.record_count;++i)
            if(compute_args[i].kind==WWMI_COMPUTE_ROOT_TABLE&&compute_args[i].parameter==scene_srv_parameter)
                original_as_table=compute_args[i].value;
        return 0;
    }
};

// A table shape is not the native root flags. SM6.6 directly indexed heaps
// cannot be switched underneath a signature without a new root binding.
inline int raygen_probe_root_flags(std::uint64_t root){
    using namespace wwmi::rt_native_root_flags;
    Flags_v1 flags{};
    UINT bytes=sizeof(flags);
    auto *signature=reinterpret_cast<ID3D12RootSignature *>(root);
    if(!signature||FAILED(signature->GetPrivateData(private_data_guid,&bytes,&flags))||
       bytes!=sizeof(flags)||flags.size!=sizeof(flags)||flags.version!=version_v1||flags.reserved)return 1;
    return flags.flags&direct_heap_indexing?2:0;
}

struct RaygenProbeAs {
    unsigned offset=0;
    int status=0;
    WWMI_AsTableSlot_v1 slot{sizeof(slot)};
    ResourceKey allocation;
    // A candidate offset is not a game-resource lease. The probe never
    // dereferences an AS allocation that the actual shader may not use.
    std::optional<RayBuildUse> build;
};

struct RaygenRecordProbe {
    static constexpr std::uint32_t marker=0x52474944u;
    struct Result {ShaderId id;std::uint32_t marker;};
    static_assert(sizeof(Result)==36);
    Recording recording;
    WWMI_ComputeStateInfo_v1 state{};
    OcclusionTableLayout layout;
    D3D12_DISPATCH_RAYS_DESC dispatch{};
    InputCollector::RayInputRange view;
    RaygenIdentity::RecordCatalog catalog;
    std::vector<RaygenProbeAs> slots;
    NativeIdentityFence completion;
    unsigned pre=0,post=0;
    ComPtr<ID3D12StateObject> original_state;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12DescriptorHeap>,2> original_heaps;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> private_heap;
    ComPtr<ID3D12Resource> source,output,readback;
    D3D12_GPU_DESCRIPTOR_HANDLE srv{},uav{};

    // All fallible work is before recording. The incoming SBT allocation is
    // already live at this original call and is retained in the recording.
    bool prepare(ID3D12Device5 *device,const RaygenProbeBindings &binding,std::span<const unsigned char> code){
        source=reinterpret_cast<ID3D12Resource *>(view.key.object);
        const auto d=source->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||
           view.offset>d.Width||view.bytes>d.Width-view.offset||
           view.bytes<sizeof(ShaderId)||(view.offset&15)||
           dispatch.RayGenerationShaderRecord.StartAddress<view.offset||
           source->GetGPUVirtualAddress()!=dispatch.RayGenerationShaderRecord.StartAddress-view.offset)return false;
        root=reinterpret_cast<ID3D12RootSignature *>(state.root_signature);
        original_state=reinterpret_cast<ID3D12StateObject *>(state.pipeline);
        original_heaps[0]=reinterpret_cast<ID3D12DescriptorHeap *>(binding.compute.cbv_srv_uav_heap);
        original_heaps[1]=reinterpret_cast<ID3D12DescriptorHeap *>(binding.compute.sampler_heap);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature=root.Get();pso.CS={code.data(),code.size()};
        gpu::check(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline)),"RT identifier probe PSO");
        output=gpu::buffer(device,sizeof(Result),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        readback=gpu::buffer(device,sizeof(Result),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        // Dispatch and CopyBufferRegion are both subject to the game's
        // unchanged predicate. Establish the private buffer's original
        // marker rather than depending on implicit zero initialization.
        Result initial{};
        if(!read_result(initial)||initial.marker==marker)return false;
        constexpr unsigned srvs=64,uavs=16;
        const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,srvs+uavs,
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        gpu::check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&private_heap)),"RT identifier probe heap");
        const auto inc=device->GetDescriptorHandleIncrementSize(heap.Type);
        const auto cpu=private_heap->GetCPUDescriptorHandleForHeapStart();
        const auto at=[&](unsigned i){return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr+SIZE_T(i)*inc};};
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format=DXGI_FORMAT_R32_TYPELESS;s.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Buffer={view.offset/4,unsigned(sizeof(ShaderId)/4),0,D3D12_BUFFER_SRV_FLAG_RAW};
        device->CreateShaderResourceView(source.Get(),&s,at(0));
        s.Buffer.FirstElement=0;
        for(unsigned i=1;i<srvs;++i)device->CreateShaderResourceView(nullptr,&s,at(i));
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format=DXGI_FORMAT_R32_TYPELESS;u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer={0,unsigned(sizeof(Result)/4),0,0,D3D12_BUFFER_UAV_FLAG_RAW};
        device->CreateUnorderedAccessView(output.Get(),nullptr,&u,at(srvs));
        for(unsigned i=1;i<uavs;++i)device->CreateUnorderedAccessView(nullptr,nullptr,&u,at(srvs+i));
        srv=private_heap->GetGPUDescriptorHandleForHeapStart();
        uav={srv.ptr+std::uint64_t(srvs)*inc};
        return true;
    }
    static void transition(ID3D12GraphicsCommandList4 *list,ID3D12Resource *res,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
        D3D12_RESOURCE_BARRIER b{};
        b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={res,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};
        list->ResourceBarrier(1,&b);
    }
    void issue(ID3D12GraphicsCommandList4 *list,const RaygenProbeBindings &binding)const{
        const auto count=original_heaps[1]?2u:1u;
        ID3D12DescriptorHeap *pair[]{private_heap.Get(),original_heaps[1].Get()};
        list->SetDescriptorHeaps(count,pair);
        list->SetPipelineState(pipeline.Get());
        list->SetComputeRootDescriptorTable(scene_srv_parameter,srv);
        list->SetComputeRootDescriptorTable(scene_uav_parameter,uav);
        // DXR guarantees NON_PIXEL_SHADER_RESOURCE for the actual SBT at
        // this call. No game resource is transitioned or written.
        transition(list,output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->Dispatch(1,1,1);
        transition(list,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(),0,output.Get(),0,sizeof(Result));
        transition(list,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        pair[0]=original_heaps[0].Get();
        list->SetPipelineState1(original_state.Get());
        list->SetDescriptorHeaps(count,pair);
        for(unsigned i=0;i<binding.graphics.record_count;++i){
            const auto &r=binding.graphics_args[i];
            list->SetGraphicsRootDescriptorTable(r.parameter,{r.table});
        }
        for(unsigned i=0;i<binding.compute.record_count;++i){
            const auto &r=binding.compute_args[i];
            if(r.kind==WWMI_COMPUTE_ROOT_TABLE)list->SetComputeRootDescriptorTable(r.parameter,{r.value});
        }
    }
    bool read_result(Result &out)const{
        D3D12_RANGE range{0,sizeof(Result)};void *mapped=nullptr;
        if(FAILED(readback->Map(0,&range,&mapped)))return false;
        if(mapped)std::memcpy(&out,mapped,sizeof(out));
        const D3D12_RANGE no_write{0,0};readback->Unmap(0,&no_write);
        return mapped!=nullptr;
    }
};
}
