#pragma once
#include "rt_scene_consumer.hpp"
#include "gpu_helpers.hpp"
#include <array>
#include <cstring>

namespace hsin_rt {
// One exact observed Build input, armed only after the actual MainRG p4
// joined it. Waiting for its next genuine Build does not authorize the row.
struct OcclusionProbeWindow {
    ResourceKey resource;
    std::uint64_t offset=0,bytes=0;
    bool matches(const InputCollector::RayInputRange &view)const{
        return resource&&view.key==resource&&view.offset==offset&&view.bytes==bytes;
    }
};
struct OcclusionProbeTarget {
    ResourceKey native_blas,primitive;
    Recording native_pose;
    std::uint64_t address=0,plan=0,revision=0,primitive_offset=0;
};
struct OcclusionProbeTargets {
    static constexpr unsigned capacity=4;
    std::array<OcclusionProbeTarget,capacity> entries{};
    unsigned count=0;
};
struct OcclusionProbeCopy {
    Recording recording;
    ResourceKey source;
    std::uint64_t source_offset=0,destination_offset=0,bytes=0;
};
struct OcclusionProbeDispatch {
    Recording recording;
    std::uint64_t call=0,pipeline=0,layout=0,uav_address=0;
    unsigned parameter=UINT32_MAX,x=0,y=0,z=0;
};
struct OcclusionProbePrograms {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> zero;
    OcclusionProbePrograms(ID3D12Device5 *device,std::span<const unsigned char> dxil){
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants={0,0,16};
        gpu::pipeline(device,params,3,dxil,root,pso);
        constexpr std::uint32_t header[4]{};
        zero=gpu::buffer(device,sizeof(header),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        gpu::upload(zero.Get(),header,sizeof(header));
    }
};
struct OcclusionInstanceProbe {
    static constexpr unsigned capacity=8;
    static constexpr std::uint64_t max_source_bytes=1024*1024;
    struct Row {std::uint32_t index,padding[3],words[16];};
    struct Result {std::uint32_t total,covered,order_low,order_high;Row rows[capacity];};
    static_assert(sizeof(Row)==80&&sizeof(Result)==16+80*capacity);
    RayBuildUse build;
    OcclusionProbeTargets targets;
    OcclusionProbeCopy copy_request;
    OcclusionProbeDispatch native_dispatch;
    WWMI_ComputeStateInfo_v1 state_at_build{};
    std::shared_ptr<OcclusionProbePrograms> program;
    ComPtr<ID3D12Resource> source,output,readback;
    Recording use;
    NativeIdentityFence completion;
    unsigned pre=0,post=0;
    bool recorded=false,reported=false;

    static void transition(ID3D12GraphicsCommandList4 *list,ID3D12Resource *resource,
        D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
        D3D12_RESOURCE_BARRIER b{};
        b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};
        list->ResourceBarrier(1,&b);
    }
    static std::int32_t work(std::uint64_t native_list,void *context){
        auto *list=reinterpret_cast<ID3D12GraphicsCommandList4 *>(native_list);
        auto &p=*static_cast<OcclusionInstanceProbe *>(context);
        // Game resources and descriptor heaps are never changed. Return the
        // private UAV to its starting state so legal list replay is safe.
        list->CopyBufferRegion(p.output.Get(),0,p.program->zero.Get(),0,16);
        transition(list,p.output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(p.program->root.Get());
        list->SetPipelineState(p.program->pso.Get());
        list->SetComputeRootShaderResourceView(0,p.build.instances_va);
        list->SetComputeRootUnorderedAccessView(1,p.output->GetGPUVirtualAddress());
        UINT args[16]{p.build.count,p.targets.count,capacity,UINT(p.build.order),UINT(p.build.order>>32)};
        for(unsigned i=0;i<p.targets.count;++i){
            const auto address=p.targets.entries[i].address;
            args[8+2*i]=UINT(address);args[9+2*i]=UINT(address>>32);
        }
        list->SetComputeRoot32BitConstants(2,16,args,0);
        list->Dispatch((p.build.count+63)/64,1,1);
        transition(list,p.output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(p.readback.Get(),0,p.output.Get(),0,sizeof(Result));
        transition(list,p.output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        p.recorded=true;
        return 0;
    }
    bool read_completed(Result &out)const{
        // Called under the Runtime lock after its actual fence proves
        // completion; pre excludes a racing second native Execute.
        D3D12_RANGE range{0,sizeof(Result)};void *mapped=nullptr;
        if(FAILED(readback->Map(0,&range,&mapped)))return false;
        std::memcpy(&out,mapped,sizeof(out));
        const D3D12_RANGE no_write{0,0};readback->Unmap(0,&no_write);
        return true;
    }
};
}
