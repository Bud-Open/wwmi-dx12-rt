#pragma once
#include "hsin_geometry.hpp"
namespace hsin_rt::gpu {
void check(HRESULT,const char *);
ComPtr<ID3D12Resource> buffer(ID3D12Device5 *,UINT64,D3D12_HEAP_TYPE,D3D12_RESOURCE_STATES,D3D12_RESOURCE_FLAGS=D3D12_RESOURCE_FLAG_NONE);
void upload(ID3D12Resource *,const void *,SIZE_T);
void uav_barrier(ID3D12GraphicsCommandList4 *,ID3D12Resource *);
void pipeline(ID3D12Device5 *,const D3D12_ROOT_PARAMETER *,UINT,std::span<const unsigned char>,ComPtr<ID3D12RootSignature> &,ComPtr<ID3D12PipelineState> &);
}
