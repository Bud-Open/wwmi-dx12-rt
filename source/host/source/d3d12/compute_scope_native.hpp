#pragma once
#include "compute_scope.hpp"
#include <d3d12.h>
namespace hsin_rt {
// Caller supplies the unwrapped list. No descriptor heap or graphics root is changed.
struct NativeComputeSink {
    ID3D12GraphicsCommandList4 *list;
    void signature(uint64_t v){list->SetComputeRootSignature(reinterpret_cast<ID3D12RootSignature*>(v));}
    void pipeline(PipelineKind k,uint64_t v){if(k==PipelineKind::ray_tracing)list->SetPipelineState1(reinterpret_cast<ID3D12StateObject*>(v));else list->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(v));}
    void constants(uint32_t index,uint32_t first,uint32_t count,const uint32_t *v){list->SetComputeRoot32BitConstants(index,count,v,first);}
    void binding(RootKind k,uint32_t i,uint64_t v){switch(k){
        case RootKind::table:list->SetComputeRootDescriptorTable(i,{v});break;
        case RootKind::cbv:list->SetComputeRootConstantBufferView(i,v);break;
        case RootKind::srv:list->SetComputeRootShaderResourceView(i,v);break;
        case RootKind::uav:list->SetComputeRootUnorderedAccessView(i,v);break;
        default:break;
    }}
};
}
