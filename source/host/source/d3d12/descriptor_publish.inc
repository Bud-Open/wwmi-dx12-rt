// Included once by the owning D3D12 device TU. No hot-path lifecycle mutex.
#if RESHADE_ADDON >= 2
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
namespace wwmi::descriptor_publish {
namespace {
struct alignas(64) Lane { std::atomic<unsigned> count{0}; };
std::array<Lane,64> lanes;
std::atomic<uint64_t> epoch{0};
WWMI_DescriptorPublisher_v1 sink{};
WWMI_DescriptorCopyBatch_v1 sink_batch=nullptr;
std::mutex lifecycle;
std::condition_variable idle;
bool draining=false;
thread_local unsigned callback_depth=0;
thread_local Lane* tls_lane=nullptr;
bool empty() noexcept {
    for(const auto& lane:lanes)if(lane.count.load(std::memory_order_seq_cst))return false;
    return true;
}
void leave(Lane& lane) noexcept {
    if(lane.count.fetch_sub(1,std::memory_order_seq_cst)==1&&!(epoch.load(std::memory_order_seq_cst)&1)){
        const std::lock_guard lock(lifecycle);
        idle.notify_all();
    }
}
struct Entered {
    Lane* lane=nullptr;
    Entered() noexcept {
        const auto open=epoch.load(std::memory_order_seq_cst);
        if(!(open&1))return;
        Lane* candidate=tls_lane;
        if(!candidate){
            const uint32_t tid=GetCurrentThreadId();
            candidate=&lanes[(tid^(tid>>6)^(tid>>12))&63u];
            tls_lane=candidate;
        }
        candidate->count.fetch_add(1,std::memory_order_seq_cst);
        // Order the lane publication, gate recheck, close and lane drain
        // together. A late entrant must see the changed gate or be drained.
        if(epoch.load(std::memory_order_seq_cst)!=open){leave(*candidate);return;}
        lane=candidate;++callback_depth;
    }
    ~Entered(){if(lane){--callback_depth;leave(*lane);}}
};
}
bool enabled() noexcept {return (epoch.load(std::memory_order_acquire)&1)!=0;}
void observe_copy(uint64_t device,WWMI_DescriptorSpans_v1 spans,void* source,WWMI_DescriptorNative_v1 native,void* context) noexcept {
    const Entered entered;
    if(entered.lane)try{sink.copy(device,spans,source,native,context);}catch(...){}
}
void observe_copy_batch(uint64_t device,const WWMI_DescriptorSpan_v1* spans,uint32_t span_count,uint32_t total_slots,WWMI_DescriptorNative_v1 native,void* context) noexcept {
    const Entered entered;
    if(!entered.lane)return;
    if(sink_batch){
        try{sink_batch(device,spans,span_count,total_slots,native,context);}catch(...){}
        return;
    }
    struct BatchSlice { const WWMI_DescriptorSpan_v1* items; uint32_t count; };
    BatchSlice slice{spans,span_count};
    auto enumerate=[](void* src,WWMI_DescriptorVisitSpan_v1 visit,void* user){
        const auto& s=*static_cast<const BatchSlice*>(src);
        for(uint32_t i=0;i<s.count;++i)visit(user,&s.items[i]);
    };
    try{sink.copy(device,enumerate,&slice,native,context);}catch(...){}
}
void observe_write(uint64_t device,const WWMI_DescriptorWrite_v1& packet,WWMI_DescriptorNative_v1 native,void* context) noexcept {
    const Entered entered;
    if(entered.lane)try{sink.write(device,&packet,native,context);}catch(...){}
}
}
extern "C" int32_t WWMI_DescriptorCopyInvokeTimed_v1(
    WWMI_DescriptorNative_v1 native,void* context,WWMI_CopyOrigQpc_v1* orig) {
    if(!orig)return native(context);
    *orig={};
    auto& request=*static_cast<wwmi::descriptor_publish::CopyTimingContext*>(context);
    struct Restore {
        wwmi::descriptor_publish::CopyTimingContext& request;
        WWMI_CopyOrigQpc_v1* prior;
        ~Restore(){request.orig=prior;}
    } restore{request,request.orig};
    request.orig=orig;
    return native(context);
}
extern "C" int32_t WWMI_DescriptorPublish_Control_v1(uint64_t owner,const WWMI_DescriptorPublisher_v1* next,
    WWMI_DescriptorRawAfter_v1* raw) {
    using namespace wwmi::descriptor_publish;
    if(!owner||callback_depth)return -1;
    std::unique_lock lock(lifecycle);
    if(draining)return -2;
    if(next){
        const bool is_v3=(next->version==WWMI_DESCRIPTOR_BATCH_VERSION);
        const bool valid_v12=(next->version==1||next->version==WWMI_DESCRIPTOR_CONTENT_VERSION)&&next->size==sizeof(*next);
        const bool valid_v3=is_v3&&next->size==sizeof(WWMI_DescriptorPublisher_v3);
        if((!valid_v12&&!valid_v3)||next->owner!=owner||!next->copy||!next->write||!raw)return -1;
        WWMI_DescriptorCopyBatch_v1 batch_fn=nullptr;
        if(valid_v3){
            batch_fn=reinterpret_cast<const WWMI_DescriptorPublisher_v3*>(next)->copy_batch;
            if(!batch_fn)return -1;
        }
        if(enabled())return -2;
        sink=*next;
        sink_batch=batch_fn;
        *raw=raw_after;
        epoch.fetch_add(1,std::memory_order_release);
        return 0;
    }
    if(!enabled())return 0;
    if(sink.owner!=owner)return -2;
    draining=true;
    epoch.fetch_add(1,std::memory_order_seq_cst);
    idle.wait(lock,empty);
    sink={};
    sink_batch=nullptr;
    draining=false;
    return 0;
}
#endif
