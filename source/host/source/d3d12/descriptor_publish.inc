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
std::mutex lifecycle;
std::condition_variable idle;
bool draining=false;
thread_local unsigned callback_depth=0;
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
        static thread_local const auto slot=[] {
            const uint32_t tid=GetCurrentThreadId();return (tid^(tid>>6)^(tid>>12))&63u;
        }();
        auto& candidate=lanes[slot];
        candidate.count.fetch_add(1,std::memory_order_seq_cst);
        // Order the lane publication, gate recheck, close and lane drain
        // together. A late entrant must see the changed gate or be drained.
        if(epoch.load(std::memory_order_seq_cst)!=open){leave(candidate);return;}
        lane=&candidate;++callback_depth;
    }
    ~Entered(){if(lane){--callback_depth;leave(*lane);}}
};
}
bool enabled() noexcept {return (epoch.load(std::memory_order_acquire)&1)!=0;}
void observe_copy(uint64_t device,WWMI_DescriptorSpans_v1 spans,void* source,WWMI_DescriptorNative_v1 native,void* context) noexcept {
    const Entered entered;
    if(entered.lane)try{sink.copy(device,spans,source,native,context);}catch(...){}
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
        if(next->size!=sizeof(*next)||(next->version!=1&&next->version!=WWMI_DESCRIPTOR_CONTENT_VERSION)||
            next->owner!=owner||!next->copy||!next->write||!raw)return -1;
        if(enabled())return -2;
        sink=*next;
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
    draining=false;
    return 0;
}
#endif
