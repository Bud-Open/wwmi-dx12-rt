#include "hsin_patches.hpp"
#include "gpu_helpers.hpp"
#include <stdexcept>
namespace hsin_rt {
namespace {
void transition(ID3D12GraphicsCommandList4 *list,ID3D12Resource *r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
}
std::int32_t instance_work(std::uint64_t cl,void *context){
    auto &b=*static_cast<InstanceBatch *>(context);auto *list=reinterpret_cast<ID3D12GraphicsCommandList4 *>(cl);
    b.recording_started=true;list->SetComputeRootSignature(b.assets->instances_root.Get());list->SetPipelineState(b.assets->instances_pso.Get());
    std::uint64_t source=b.source;
    for(std::size_t i=0;i<b.targets.size();++i){
        auto *out=b.copies[i%2].Get();const auto &target=*b.targets[i];
        if(i>=2)transition(list,out,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto dest=target.geometry->blas->GetGPUVirtualAddress();
        UINT constants[]{b.count,UINT(target.native_blas),UINT(target.native_blas>>32),UINT(dest),UINT(dest>>32)};
        list->SetComputeRootShaderResourceView(0,source);list->SetComputeRootUnorderedAccessView(1,out->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(2,5,constants,0);list->Dispatch((b.count+63)/64,1,1);
        gpu::uav_barrier(list,out);transition(list,out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        source=out->GetGPUVirtualAddress();
    }
    b.recorded=true;return 0;
}
std::int32_t sbt_work(std::uint64_t cl,void *context){
    auto &b=*static_cast<SbtBatch *>(context);auto *list=reinterpret_cast<ID3D12GraphicsCommandList4 *>(cl);
    b.recording_started=true;list->SetComputeRootSignature(b.assets->sbt_root.Get());list->SetPipelineState(b.assets->sbt_pso.Get());
    list->SetComputeRootShaderResourceView(0,b.source);list->SetComputeRootShaderResourceView(1,b.rules->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2,b.copy->GetGPUVirtualAddress());
    UINT constants[12]{b.records,b.rule_count,0,0};std::copy(b.shader.begin(),b.shader.end(),constants+4);
    list->SetComputeRoot32BitConstants(3,12,constants,0);list->Dispatch((b.records+63)/64,1,1);
    gpu::uav_barrier(list,b.copy.Get());transition(list,b.copy.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    b.recorded=true;return 0;
}
}
std::shared_ptr<PatchAssets> PatchAssets::create(ID3D12Device5 *device,std::span<const unsigned char> instances,std::span<const unsigned char> sbt){
    if(!device||instances.empty()||sbt.empty())throw std::runtime_error("RT patch programs missing");
    auto a=std::make_shared<PatchAssets>();a->device=device;
    D3D12_ROOT_PARAMETER p[4]{};
    p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[0].Descriptor.ShaderRegister=0;
    p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[1].Descriptor.ShaderRegister=0;
    p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[2].Constants={0,0,5};
    gpu::pipeline(device,p,3,instances,a->instances_root,a->instances_pso);
    p[1]={};p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[1].Descriptor.ShaderRegister=1;
    p[2]={};p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[2].Descriptor.ShaderRegister=0;
    p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[3].Constants={0,0,12};
    gpu::pipeline(device,p,4,sbt,a->sbt_root,a->sbt_pso);return a;
}
std::shared_ptr<InstanceBatch> InstanceBatch::prepare(std::shared_ptr<PatchAssets> assets,std::uint64_t source,UINT count,std::vector<std::shared_ptr<Target>> targets){
    if(!source||!count||count>65535u*64u||targets.empty())throw std::runtime_error("RT instance input outside supported dispatch range");
    auto b=std::make_shared<InstanceBatch>();b->assets=std::move(assets);b->source=source;b->count=count;b->targets=std::move(targets);
    const unsigned copies=b->targets.size()>1?2:1;
    for(unsigned i=0;i<copies;++i)b->copies[i]=gpu::buffer(b->assets->device.Get(),64ull*count,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    return b;
}
std::uint64_t InstanceBatch::address() const {return copies[(targets.size()-1)%2]->GetGPUVirtualAddress();}
std::int32_t InstanceBatch::record(std::uint64_t list,ComputeRunFn run){if(!run||!list||recording_started)return 3;return run(list,instance_work,this);}
std::shared_ptr<SbtBatch> SbtBatch::prepare(std::shared_ptr<PatchAssets> assets,std::uint64_t source,std::uint64_t bytes,const ShaderId &shader,std::vector<std::shared_ptr<Target>> targets){
    if(!source||!bytes||bytes%128||bytes/128>65535u*64u||targets.empty())throw std::runtime_error("RT hit table outside supported 128-byte record range");
    auto b=std::make_shared<SbtBatch>();b->assets=std::move(assets);b->source=source;b->records=UINT(bytes/128);b->shader=shader;b->targets=std::move(targets);
    std::vector<SbtRule> rules;
    for(const auto &t:b->targets)for(unsigned c=0;c<t->native_geometry.size();++c){
        const auto &g=t->native_geometry[c].Triangles;
        rules.push_back({g.IndexBuffer,g.VertexBuffer.StartAddress,0,4|(12<<8),t->geometry->assets->indices->GetGPUVirtualAddress(),t->geometry->positions->GetGPUVirtualAddress(),(t->geometry->assets->pack->components[c].index_count?4*t->geometry->assets->pack->components[c].index_first:0),4|(12<<8)});
    }
    b->rule_count=UINT(rules.size());b->rules=gpu::buffer(b->assets->device.Get(),rules.size()*sizeof(SbtRule),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    gpu::upload(b->rules.Get(),rules.data(),rules.size()*sizeof(SbtRule));
    b->copy=gpu::buffer(b->assets->device.Get(),bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);return b;
}
std::int32_t SbtBatch::record(std::uint64_t list,ComputeRunFn run){if(!run||!list||recording_started)return 3;return run(list,sbt_work,this);}
}
