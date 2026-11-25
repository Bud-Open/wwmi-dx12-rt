// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "wwmi_binding_trace.h"
#include <d3d12.h>
#include <stdint.h>

// These forwarding helpers execute the original API exactly once, then publish
// a deep CPU POD. Before/after draw is the only exception to after-only timing.
// They are used only on _orig/native pointers, never on the ReShade wrapper.
namespace wwmi::binding_trace {
inline uint64_t pointer(const void *v) noexcept { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v)); }
inline WWMI_BindingInput_v1 input(uint32_t kind,uint32_t source,uint32_t site,const void *list,const void *pub=nullptr) noexcept {
    WWMI_BindingInput_v1 x{};x.struct_size=sizeof(x);x.abi_version=1;x.kind=kind;x.source=source;
    x.source_site=site;x.native_list=pointer(list);x.public_list=pointer(pub);return x;
}
inline void created(const void *list,const void *pub,uint32_t source,uint32_t initial=0) noexcept {
    auto x=input(WWMI_BIND_LIST_CREATE,source,0,list,pub);x.initial_state=initial;WWMI_BindingTrace_Submit_v1(&x);
}
inline void destroyed(const void *list,const void *pub,uint32_t source) noexcept {
    auto x=input(WWMI_BIND_LIST_DESTROY,source,0,list,pub);WWMI_BindingTrace_Submit_v1(&x);
}
inline void invalidated(const void *list,const void *pub,uint32_t source,uint32_t site,uint32_t reason) noexcept {
    auto x=input(WWMI_BIND_INVALIDATE,source,site,list,pub);x.reason=reason;WWMI_BindingTrace_Submit_v1(&x);
}
template<class T> inline HRESULT reset(T *list,uint32_t source,uint32_t site,ID3D12CommandAllocator *allocator,ID3D12PipelineState *pso) {
    const auto hr=list->Reset(allocator,pso);auto x=input(WWMI_BIND_LIST_RESET,source,site,list);x.hresult=hr;WWMI_BindingTrace_Submit_v1(&x);return hr;
}
template<class T> inline HRESULT close(T *list,uint32_t source,uint32_t site) {
    const auto hr=list->Close();auto x=input(WWMI_BIND_LIST_CLOSE,source,site,list);x.hresult=hr;WWMI_BindingTrace_Submit_v1(&x);return hr;
}
template<class T> inline void clear(T *list,uint32_t source,uint32_t site,ID3D12PipelineState *pso) {
    list->ClearState(pso);auto x=input(WWMI_BIND_CLEAR_STATE,source,site,list);WWMI_BindingTrace_Submit_v1(&x);
}
template<class T> inline void topology(T *list,uint32_t source,uint32_t site,D3D12_PRIMITIVE_TOPOLOGY value) {
    list->IASetPrimitiveTopology(value);auto x=input(WWMI_BIND_TOPOLOGY,source,site,list);x.topology=value;WWMI_BindingTrace_Submit_v1(&x);
}
template<class T> inline void vertices(T *list,uint32_t source,uint32_t site,UINT start,UINT count,const D3D12_VERTEX_BUFFER_VIEW *views) {
    list->IASetVertexBuffers(start,count,views);auto x=input(WWMI_BIND_VERTEX_BUFFERS,source,site,list);
    x.start_slot=start;x.count=count;x.views_null=views==nullptr;
    if(start<=32&&count<=32-start&&views) for(UINT i=0;i<count;++i) x.views[i]={views[i].BufferLocation,views[i].SizeInBytes,views[i].StrideInBytes};
    WWMI_BindingTrace_Submit_v1(&x);
}
template<class T> inline void indices(T *list,uint32_t source,uint32_t site,const D3D12_INDEX_BUFFER_VIEW *view) {
    list->IASetIndexBuffer(view);auto x=input(WWMI_BIND_INDEX_BUFFER,source,site,list);x.count=1;x.views_null=view==nullptr;
    if(view) x.views[0]={view->BufferLocation,view->SizeInBytes,static_cast<uint32_t>(view->Format)};
    WWMI_BindingTrace_Submit_v1(&x);
}
template<class T> inline void predicate(T *list,uint32_t source,uint32_t site,ID3D12Resource *buffer,UINT64 offset,D3D12_PREDICATION_OP operation) {
    list->SetPredication(buffer,offset,operation);auto x=input(WWMI_BIND_PREDICATION,source,site,list);
    x.predicate_resource=pointer(buffer);x.predicate_offset=offset;x.predicate_operation=static_cast<uint32_t>(operation);WWMI_BindingTrace_Submit_v1(&x);
}
template<class T> inline void draw_indexed(T *list,uint32_t source,uint32_t site,UINT count,UINT instances,UINT first,INT base,UINT first_instance) {
    auto x=input(WWMI_BIND_DRAW_BEFORE,source,site,list);x.draw={count,instances,first,base,first_instance,0};
    const auto before=WWMI_BindingTrace_Submit_v1(&x);
    list->DrawIndexedInstanced(count,instances,first,base,first_instance);
    if(before) { x.kind=WWMI_BIND_DRAW_AFTER;x.paired_before_sequence=before;WWMI_BindingTrace_Submit_v1(&x); }
}
template<class T> inline void bundle(T *list,uint32_t source,uint32_t site,ID3D12GraphicsCommandList *child) {
    list->ExecuteBundle(child);invalidated(list,nullptr,source,site,WWMI_BIND_REASON_BUNDLE);
}
template<class T> inline void indirect(T *list,uint32_t source,uint32_t site,ID3D12CommandSignature *sig,UINT maximum,ID3D12Resource *arguments,UINT64 offset,ID3D12Resource *counts,UINT64 count_offset) {
    list->ExecuteIndirect(sig,maximum,arguments,offset,counts,count_offset);invalidated(list,nullptr,source,site,WWMI_BIND_REASON_INDIRECT);
}
template<class T> inline void barriers(T *list,uint32_t source,uint32_t site,UINT count,const D3D12_RESOURCE_BARRIER *values) {
    list->ResourceBarrier(count,values);
    if(count&&!values) { invalidated(list,nullptr,source,site,WWMI_BIND_REASON_MALFORMED);return; }
    for(UINT i=0;i<count;++i) if(values[i].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
        invalidated(list,nullptr,source,site,WWMI_BIND_REASON_ALIASING);return;
    }
}
template<class T> inline void enhanced_barriers(T *list,const void *identity,uint32_t source,uint32_t site,UINT32 count,const D3D12_BARRIER_GROUP *groups) {
    list->Barrier(count,groups);
    if(count) invalidated(identity,nullptr,source,site,WWMI_BIND_REASON_ENHANCED_BARRIER);
}
template<class T> inline void strip_cut(T *list,const void *identity,uint32_t source,uint32_t site,D3D12_INDEX_BUFFER_STRIP_CUT_VALUE value) {
    list->IASetIndexBufferStripCutValue(value);invalidated(identity,nullptr,source,site,WWMI_BIND_REASON_STRIP_CUT);
}
} // namespace wwmi::binding_trace
