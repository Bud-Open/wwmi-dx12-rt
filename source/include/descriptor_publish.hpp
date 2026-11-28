// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#if RESHADE_ADDON >= 2
#include "wwmi_descriptor_publish.h"
#include "wwmi_descriptor_copy_timing.h"
#include "wwmi_as_slots.hpp"
#include <type_traits>

namespace wwmi::descriptor_publish {
bool enabled() noexcept;
void observe_copy(uint64_t device,WWMI_DescriptorSpans_v1,void*,WWMI_DescriptorNative_v1,void*) noexcept;
void observe_copy_batch(uint64_t device,const WWMI_DescriptorSpan_v1* spans,uint32_t span_count,uint32_t total_slots,WWMI_DescriptorNative_v1,void*) noexcept;
void observe_write(uint64_t device,const WWMI_DescriptorWrite_v1&,WWMI_DescriptorNative_v1,void*) noexcept;
void raw_after(uint64_t device,uint64_t heap,uint64_t creation,uint32_t index,uint32_t slot);

inline WWMI_DescriptorRange_v1 range(rt_as::Range r,D3D12_CPU_DESCRIPTOR_HANDLE cpu) noexcept {
    if(!r.contains(1))return {0,0,cpu.ptr,0,0};
    // Registration 0 is the private transient shader-visible heap, not an
    // app descriptor heap or a CPU-only SDK origin.
    if(r.heap->index<2)return {};
    return {reinterpret_cast<uint64_t>(r.heap->native),r.heap->generation,cpu.ptr,r.slot,r.heap->desc.NumDescriptors,
        static_cast<uint32_t>(r.heap->index),r.heap->increment};
}

// Private GPU tables are not main's publication targets. Their real
// physical handle still participates in native-vs-raw access ordering.
inline WWMI_DescriptorRange_v1 copy_range(rt_as::Range r,D3D12_CPU_DESCRIPTOR_HANDLE cpu) noexcept {
    auto out=range(r,cpu);
    if(!out.cpu)out.cpu=cpu.ptr;
    if(!out.increment&&r.heap)out.increment=r.heap->increment;
    return out;
}

template<class Forward> struct Once {
    Forward &forward;
    bool called=false;
    HRESULT result=E_FAIL;
    static int32_t invoke(void* context) {
        auto& self=*static_cast<Once*>(context);
        if(!self.called){self.called=true;self.result=self.forward();}
        return self.result;
    }
};

// Copy and write keep their existing C callback signatures. Only a copy's
// borrowed context can carry a request to time the actual underlying call.
struct CopyTimingContext {
    WWMI_CopyOrigQpc_v1 *orig=nullptr;
};
template<class Forward> struct CopyOnce : CopyTimingContext {
    Forward &forward;
    bool called=false;
    HRESULT result=E_FAIL;
    explicit CopyOnce(Forward &call) noexcept :forward(call) {}
    static int32_t invoke(void* context) {
        auto& self=*static_cast<CopyOnce*>(static_cast<CopyTimingContext*>(context));
        if(!self.called){self.called=true;self.result=self.forward(self.orig);}
        return self.result;
    }
};

template<class Call>
void copy_orig(WWMI_CopyOrigQpc_v1* orig,Call&& call) {
    if(!orig){call();return;}
    LARGE_INTEGER begin{},end{};
    const bool began=QueryPerformanceCounter(&begin)!=0;
    call();
    if(QueryPerformanceCounter(&end)&&began&&begin.QuadPart>0&&end.QuadPart>=begin.QuadPart)
        *orig={uint64_t(begin.QuadPart),uint64_t(end.QuadPart)};
}

template<class Enumerate,class Forward>
void copy(reshade::api::device* api,Enumerate&& spans,Forward&& forward) {
    CopyOnce<std::remove_reference_t<Forward>> call{forward};
    auto* context=static_cast<CopyTimingContext*>(&call);
    if(enabled())observe_copy(reinterpret_cast<uint64_t>(api),
        +[](void* context,WWMI_DescriptorVisitSpan_v1 visit,void* user){
            (*static_cast<std::remove_reference_t<Enumerate>*>(context))(visit,user);
        },&spans,decltype(call)::invoke,context);
    if(!call.called)decltype(call)::invoke(context);
}

template<class Forward>
void copy_batch(reshade::api::device* api,const WWMI_DescriptorSpan_v1* spans,uint32_t span_count,uint32_t total_slots,Forward&& forward) {
    CopyOnce<std::remove_reference_t<Forward>> call{forward};
    auto* context=static_cast<CopyTimingContext*>(&call);
    if(enabled())observe_copy_batch(reinterpret_cast<uint64_t>(api),spans,span_count,total_slots,decltype(call)::invoke,context);
    if(!call.called)decltype(call)::invoke(context);
}

template<class Forward>
HRESULT write(reshade::api::device* api,const WWMI_DescriptorWrite_v1& packet,Forward&& forward) {
    Once<std::remove_reference_t<Forward>> call{forward};
    if(packet.destination.cpu && enabled())
        observe_write(reinterpret_cast<uint64_t>(api),packet,decltype(call)::invoke,&call);
    if(!call.called)decltype(call)::invoke(&call);
    return call.result;
}
// The SDK pre-events are outside this scope. The existing sparse host
// projection belongs to the actual native write, before main's admission
// releases. Metadata failure must not replace the original HRESULT.
template<class Forward,class Record>
HRESULT write_slot(reshade::api::device* api,WWMI_DescriptorWrite_v1 packet,rt_as::Range dst,
    Forward&& forward,Record&& record) {
    if(!packet.destination.cpu&&dst.contains(1)){
        packet.destination=copy_range(dst,{dst.heap->cpu_start+SIZE_T(dst.slot)*dst.heap->increment});
        packet.kind=WWMI_DESCRIPTOR_UNKNOWN; // physical-only, not a CPU view origin
    }
    return write(api,packet,[&]() -> HRESULT {
        const HRESULT hr=forward();
        if(SUCCEEDED(hr))try{record();}catch(...){rt_as::clear(dst);}
        return hr;
    });
}

}
#endif
