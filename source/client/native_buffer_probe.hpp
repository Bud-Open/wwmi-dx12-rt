#pragma once
#include "native_discovery.hpp"
#include "gpu_helpers.hpp"
#include "rt_native_buffer_receipt.h"
namespace hsin_rt {
using NativeBufferLeaseFn=decltype(&WWMI_RtNativeBufferReadLease_v1);
// Root descriptors do not change either descriptor heap. Only the owned
// destination changes legacy state; source VA is never a COM pointer.
struct NativeBufferProbeProgram {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    NativeBufferProbeProgram(ID3D12Device5 *device,std::span<const unsigned char> code){
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[0].Descriptor={0,0};
        p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[1].Descriptor={0,0};
        p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[2].Constants={0,0,2};
        gpu::pipeline(device,p,3,code,root,pipeline);
    }
};
struct NativeBufferProbe {
    std::shared_ptr<NativeDiscovery::Bytes> evidence;
    std::shared_ptr<NativeBufferProbeProgram> program;
    ComPtr<ID3D12Resource> source,output,readback;
    ComPtr<ID3D12GraphicsCommandList7> enhanced;
    WWMI_RtNativeBufferLease_v1 lease{sizeof(lease)};
    std::uint64_t source_address=0,aligned_offset=0,aligned_bytes=0,slice_offset=0;
    NativeIdentityFence completion;
    unsigned submissions=0;
    bool recorded=false,delivered=false;
    const char *prepare_stage="input";std::int32_t prepare_status=0;
    std::uint64_t diagnostic_source_bytes=0,diagnostic_recorded_ms=0;
    std::uint32_t diagnostic_source_flags=0;
    bool prepare(ID3D12Device5 *device,ID3D12GraphicsCommandList *list,ComPtr<ID3D12Resource> hold,
        std::uint64_t address,NativeBufferLeaseFn read_lease){
        const unsigned align=(evidence&&evidence->point==NativeDiscovery::ReadPoint::index_consumer)?2u:4u;
        source=std::move(hold);if(!source||!read_lease||!evidence||!evidence->valid||!evidence->range.bytes||evidence->range.bytes>NativeDiscovery::max_probe_bytes||evidence->range.bytes%align||evidence->range.offset%align)return false;
        prepare_stage="source_desc";const auto &r=evidence->range;const auto d=source->GetDesc();
        diagnostic_source_bytes=d.Width;diagnostic_source_flags=std::uint32_t(d.Flags);
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||r.offset>d.Width||r.bytes>d.Width-r.offset||source->GetGPUVirtualAddress()>UINT64_MAX-r.offset||source->GetGPUVirtualAddress()+r.offset!=address)return false;
        aligned_offset=r.offset&~3ull;
        slice_offset=r.offset-aligned_offset;
        const auto aligned_end=(r.offset+r.bytes+3ull)&~3ull;
        aligned_bytes=aligned_end-aligned_offset;
        if(!aligned_bytes||aligned_bytes>NativeDiscovery::max_probe_bytes)return false;
        const auto point=evidence->point==NativeDiscovery::ReadPoint::rest_consumer?1u:2u;
        // D3D12 buffer barriers are whole-resource operations. A logical
        // subrange lease cannot authorize restoring the rest of this buffer.
        prepare_stage="whole_buffer_lease";prepare_status=read_lease(evidence->recording.list,std::uint64_t(source.Get()),0,d.Width,point,&lease);
        if(prepare_status||lease.version!=1||(lease.mode!=1&&lease.mode!=2))return false;
        prepare_stage="enhanced_interface";
        if(lease.mode==2){prepare_status=list->QueryInterface(IID_PPV_ARGS(&enhanced));if(FAILED(prepare_status))return false;}
        output=gpu::buffer(device,aligned_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        readback=gpu::buffer(device,aligned_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        const auto aligned_va=source->GetGPUVirtualAddress()+aligned_offset;
        source_address=aligned_va-(aligned_offset&15ull);prepare_stage="prepared";return true;
    }
    void bridge(bool before)const{
        if(lease.mode!=2)return;
        D3D12_BUFFER_BARRIER b{};
        b.SyncBefore=before?D3D12_BARRIER_SYNC(lease.sync_after):D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        b.SyncAfter=before?D3D12_BARRIER_SYNC_COMPUTE_SHADING:D3D12_BARRIER_SYNC(lease.sync_after);
        b.AccessBefore=before?D3D12_BARRIER_ACCESS(lease.access_after):D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        b.AccessAfter=before?D3D12_BARRIER_ACCESS_SHADER_RESOURCE:D3D12_BARRIER_ACCESS(lease.access_after);
        b.pResource=source.Get();b.Offset=0;b.Size=UINT64_MAX;
        D3D12_BARRIER_GROUP group{};group.Type=D3D12_BARRIER_TYPE_BUFFER;group.NumBarriers=1;group.pBufferBarriers=&b;enhanced->Barrier(1,&group);
    }
    static int32_t work(std::uint64_t native,void *context){
        auto &p=*static_cast<NativeBufferProbe *>(context);auto *list=reinterpret_cast<ID3D12GraphicsCommandList4 *>(native);
        p.bridge(true);list->SetComputeRootSignature(p.program->root.Get());list->SetPipelineState(p.program->pipeline.Get());
        list->SetComputeRootShaderResourceView(0,p.source_address);list->SetComputeRootUnorderedAccessView(1,p.output->GetGPUVirtualAddress());
        const UINT dwords=UINT(p.aligned_bytes/4);
        const UINT constants[]{dwords,UINT(p.aligned_offset&15ull)};
        list->SetComputeRoot32BitConstants(2,2,constants,0);
        const auto groups=UINT((dwords+63)/64);
        list->Dispatch((std::min)(groups,65535u),(groups+65534u)/65535u,1);
        p.bridge(false);
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={p.output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&b);list->CopyBufferRegion(p.readback.Get(),0,p.output.Get(),0,p.aligned_bytes);
        p.recorded=true;return 0;
    }
    bool read(std::vector<unsigned char> &out)const{
        out.resize(std::size_t(evidence->range.bytes));const D3D12_RANGE range{std::size_t(slice_offset),std::size_t(slice_offset)+out.size()},no_write{0,0};void *data=nullptr;
        if(FAILED(readback->Map(0,&range,&data)))return false;
        if(data)std::memcpy(out.data(),static_cast<const unsigned char *>(data)+slice_offset,out.size());readback->Unmap(0,&no_write);return data!=nullptr;
    }
};
}
