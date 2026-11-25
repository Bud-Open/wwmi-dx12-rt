// SPDX-License-Identifier: GPL-3.0-only
#define WWMI_RT_FORWARD_API __declspec(dllexport)
#include "wwmi_rt_dispatch_scope.hpp"
#include "dll_log.hpp"
#include "native_buffer_lease.hpp"
#include <atomic>
#include <mutex>
#include <condition_variable>
namespace wwmi::rt_forward {
static WWMI_RtForwardCallbacks_v2 callbacks{};
static WWMI_RtBufferReceiptFn_v3 buffer_receipt=nullptr;
static std::atomic<uint64_t> dispatch_serial{0};
static std::atomic<bool> dispatch_enabled{false};
static std::atomic<uint64_t> epoch{0};
static std::atomic<unsigned> entered{0};
static std::atomic<bool> error_reported{false};
static std::mutex lifecycle;
static std::condition_variable idle;
static bool draining=false;
static thread_local bool inside=false;
bool enabled() noexcept{return (epoch.load(std::memory_order_acquire)&1)!=0;}
static void leave() noexcept {
    if(entered.fetch_sub(1,std::memory_order_acq_rel)==1&&!enabled()){
        std::lock_guard<std::mutex> lock(lifecycle);idle.notify_all();
    }
}
static bool enter() noexcept {
    if(inside)return false;
    const auto observed=epoch.load(std::memory_order_acquire);if(!(observed&1))return false;
    entered.fetch_add(1,std::memory_order_acq_rel);
    if(epoch.load(std::memory_order_acquire)!=observed){leave();return false;}
    inside=true;return true;
}
static void end() noexcept {inside=false;leave();}
struct Call {
    bool active=enter();
    ~Call(){if(active)end();}
};
static void failure() noexcept {
    if(!error_reported.exchange(true))reshade::log::message(reshade::log::level::error,"WWMI RT forwarding callback failed; original native arguments retained.");
}
void blas_after(reshade::api::command_list *api,ID3D12GraphicsCommandList *native,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc) noexcept {
    Call call;if(!call.active||!callbacks.blas_after)return;
    try{callbacks.blas_after((uint64_t)api,(uint64_t)native,(uint64_t)desc,callbacks.context);}catch(...){failure();}
}
uint64_t tlas_input(reshade::api::command_list *api,ID3D12GraphicsCommandList *native,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc) noexcept {
    Call call;if(!call.active||!callbacks.tlas_input)return 0;
    try{return callbacks.tlas_input((uint64_t)api,(uint64_t)native,(uint64_t)desc,callbacks.context);}catch(...){failure();return 0;}
}
uint64_t hit_table(reshade::api::command_list *api,ID3D12GraphicsCommandList *native,const D3D12_DISPATCH_RAYS_DESC *desc) noexcept {
    Call call;if(!call.active||!callbacks.hit_table)return 0;
    try{return callbacks.hit_table((uint64_t)api,(uint64_t)native,(uint64_t)desc,callbacks.context);}catch(...){failure();return 0;}
}
void queue_after(reshade::api::command_queue *api,ID3D12CommandQueue *native,reshade::api::command_list *list) noexcept {
    Call call;if(!call.active||!callbacks.queue_after)return;
    try{callbacks.queue_after((uint64_t)api,(uint64_t)native,(uint64_t)list,callbacks.context);}catch(...){failure();}
}
void buffer(reshade::api::command_list *api,ID3D12GraphicsCommandList *native,uint32_t kind,uint32_t phase,
    ID3D12Resource *source,ID3D12Resource *destination,uint64_t source_offset,uint64_t destination_offset,uint64_t bytes,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc) noexcept {
    Call call;if(!call.active||!buffer_receipt)return;
    if(kind==WWMI_RT_BUFFER_COPY&&!wwmi::native_buffer::unpredicated(uint64_t(api))){if(phase==WWMI_RT_BUFFER_NATIVE_RECORDED)return;kind=WWMI_RT_BUFFER_WRITE;}
    const WWMI_RtNativeBuffer_v3 packet{sizeof(packet),3,kind,phase,uint64_t(api),uint64_t(native),uint64_t(source),uint64_t(destination),source_offset,destination_offset,bytes,uint64_t(desc)};
    try{buffer_receipt(&packet,callbacks.context);}catch(...){failure();}
}
DispatchScope::DispatchScope(reshade::api::command_list *api,ID3D12GraphicsCommandList *native,uint32_t x,uint32_t y,uint32_t z) noexcept {
    if(!dispatch_enabled.load(std::memory_order_acquire))return;
    active=enter();if(!active)return;
    if(!callbacks.dispatch_receipt){end();active=false;return;}
    packet.struct_size=sizeof(packet);packet.abi_version=2;packet.call_id=dispatch_serial.fetch_add(1,std::memory_order_relaxed)+1;
    packet.api_list=reinterpret_cast<uint64_t>(api);packet.native_list=reinterpret_cast<uint64_t>(native);
    packet.group_x=x;packet.group_y=y;packet.group_z=z;
}
DispatchScope::~DispatchScope(){if(active)end();}
void DispatchScope::before(bool ready,uint64_t pipeline,uint64_t layout) noexcept {
    if(!active)return;packet.phase=WWMI_RT_DISPATCH_BEFORE_NATIVE;
    packet.flags=(ready?WWMI_RT_DISPATCH_STATE_KNOWN:0)|(wwmi::native_buffer::unpredicated(packet.api_list)?WWMI_RT_DISPATCH_UNPREDICATED:0);packet.pipeline=pipeline;packet.layout=layout;
    try{callbacks.dispatch_receipt(&packet,callbacks.context);}catch(...){failure();}
}
void DispatchScope::after() noexcept {
    if(!active||finished)return;finished=true;packet.phase=WWMI_RT_DISPATCH_NATIVE_RECORDED;
    try{callbacks.dispatch_receipt(&packet,callbacks.context);}catch(...){failure();}
}
void DispatchScope::cancelled(bool ready,uint64_t pipeline,uint64_t layout) noexcept {
    if(!active||finished)return;finished=true;packet.phase=WWMI_RT_DISPATCH_CANCELLED;
    packet.flags=(ready?WWMI_RT_DISPATCH_STATE_KNOWN:0)|(wwmi::native_buffer::unpredicated(packet.api_list)?WWMI_RT_DISPATCH_UNPREDICATED:0);packet.pipeline=pipeline;packet.layout=layout;
    try{callbacks.dispatch_receipt(&packet,callbacks.context);}catch(...){failure();}
}

}
static int32_t register_v3(const WWMI_RtForwardCallbacks_v2 *c,WWMI_RtBufferReceiptFn_v3 buffer) {
    using namespace wwmi::rt_forward;
    if(inside)return -3;
    if(!c||c->size!=sizeof(*c)||c->version!=2||!c->context)return -1;
    std::lock_guard<std::mutex> lock(lifecycle);
    if(draining)return -2;
    if(enabled())return callbacks.context==c->context&&callbacks.blas_after==c->blas_after&&callbacks.tlas_input==c->tlas_input&&callbacks.hit_table==c->hit_table&&callbacks.queue_after==c->queue_after&&callbacks.dispatch_receipt==c->dispatch_receipt&&buffer_receipt==buffer?0:-2;
    callbacks=*c;buffer_receipt=buffer;dispatch_enabled.store(c->dispatch_receipt!=nullptr,std::memory_order_release);error_reported.store(false);epoch.fetch_add(1,std::memory_order_release);return 0;
}
extern "C" int32_t WWMI_RtForward_Register_v3(const WWMI_RtForwardCallbacks_v3 *c) {
    if(!c)return -1;return register_v3(&c->base,c->buffer_receipt);
}
extern "C" int32_t WWMI_RtForward_Register_v2(const WWMI_RtForwardCallbacks_v2 *c) {return register_v3(c,nullptr);}
extern "C" int32_t WWMI_RtNativeBufferReadLease_v1(uint64_t list,uint64_t resource,uint64_t offset,uint64_t bytes,uint32_t point,WWMI_RtNativeBufferLease_v1 *out){
    return wwmi::native_buffer::read(list,reinterpret_cast<ID3D12Resource *>(resource),offset,bytes,point,out);
}
extern "C" int32_t WWMI_RtForward_Register_v1(const WWMI_RtForwardCallbacks_v1 *c) {
    if(wwmi::rt_forward::inside)return -3;
    if(!c||c->size!=sizeof(*c)||c->version!=1||!c->context)return -1;
    const WWMI_RtForwardCallbacks_v2 v2{sizeof(v2),2,c->context,c->blas_after,c->tlas_input,c->hit_table,c->queue_after,nullptr};
    return WWMI_RtForward_Register_v2(&v2);
}
extern "C" int32_t WWMI_RtForward_Unregister_v1(uint64_t context) {
    using namespace wwmi::rt_forward;
    if(inside)return -3;
    std::unique_lock<std::mutex> lock(lifecycle);
    if(draining){if(callbacks.context!=context)return -2;idle.wait(lock,[]{return !draining;});return 0;}
    if(!enabled())return callbacks.context==0||callbacks.context==context?0:-2;
    if(callbacks.context!=context)return -2;
    draining=true;dispatch_enabled.store(false,std::memory_order_release);epoch.fetch_add(1,std::memory_order_acq_rel);
    idle.wait(lock,[]{return entered.load(std::memory_order_acquire)==0;});
    callbacks={};buffer_receipt=nullptr;draining=false;idle.notify_all();return 0;
}
