#pragma once
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
namespace hsin_rt {
// Only native buffer lifetime notifications cross this inbox. No GPU command,
// other ReShade callback or engine operation is deferred.
struct BufferEvent {
    bool created=false;api::resource resource{};std::uint64_t creation=0;
    std::uint64_t gpu_address=0;
    api::resource_desc desc{};ComPtr<ID3D12Resource> keep;
};
class BufferInbox {
    struct PendingCreation {std::uint64_t creation=0,base=0,bytes=0;};
    std::mutex mutex_;
    std::uint64_t next_=0;
    std::unordered_map<std::uint64_t,std::uint64_t> live_;
    std::unordered_map<std::uint64_t,unsigned> pending_;
    // Only not-yet-applied live creates, not the device-wide resource ledger.
    std::unordered_map<std::uint64_t,PendingCreation> pending_creations_;
    std::deque<BufferEvent> events_;
    std::atomic<bool> queued_{false};
public:
    void created(api::resource resource,const api::resource_desc &desc){
        BufferEvent e;e.created=true;e.resource=resource;e.desc=desc;
        // The creation callback's object is alive. Keep just this notification
        // alive until its metadata is consumed; release outside Runtime's lock.
        e.keep=reinterpret_cast<ID3D12Resource *>(resource.handle);
        // Native read on the live creation callback, before taking the inbox
        // lock. The eventual ledger application only reuses this value.
        e.gpu_address=e.keep->GetGPUVirtualAddress();
        std::lock_guard lock(mutex_);
        queued_.store(true,std::memory_order_release);
        e.creation=++next_;live_[resource.handle]=e.creation;
        pending_creations_[resource.handle]={e.creation,e.gpu_address,e.desc.buffer.size};
        ++pending_[resource.handle];events_.push_back(std::move(e));
    }
    void destroyed(api::resource resource){
        std::lock_guard lock(mutex_);auto p=live_.find(resource.handle);if(p==live_.end())return;
        queued_.store(true,std::memory_order_release);
        BufferEvent e;e.resource=resource;e.creation=p->second;live_.erase(p);++pending_[resource.handle];events_.push_back(std::move(e));
        pending_creations_.erase(resource.handle);
        // Never AddRef, query or otherwise dereference a dying native object.
    }
    bool current(ResourceKey key){
        std::lock_guard lock(mutex_);auto p=live_.find(key.object);
        return p!=live_.end()&&p->second==key.creation&&!pending_.contains(key.object);
    }
    ComPtr<ID3D12Resource> acquire_current(ResourceKey key){
        std::lock_guard lock(mutex_);const auto it=live_.find(key.object);
        if(it==live_.end()||it->second!=key.creation||pending_.contains(key.object))return {};
        // Called only inside the real native consumer, while its source is
        // live. No GPU VA is converted to an object; destroy is serialized.
        return ComPtr<ID3D12Resource>(reinterpret_cast<ID3D12Resource *>(key.object));
    }
    bool unique_current(ResourceKey key,std::uint64_t address,std::uint64_t bytes){
        std::lock_guard lock(mutex_);auto p=live_.find(key.object);
        if(p==live_.end()||p->second!=key.creation||pending_.contains(key.object))return false;
        // Match locate's complete-query containment, using subtraction to
        // avoid VA-end overflow. Unrelated and merely partial ranges do not
        // introduce a second possible allocation for this query.
        for(const auto &[_,r]:pending_creations_)
            if(r.base&&address>=r.base&&address-r.base<=r.bytes&&bytes<=r.bytes-(address-r.base))return false;
        return true;
    }
    // This hint answers only whether an inbox drain can do work. The same
    // lock serializes set/clear; an event arriving later still sets it and
    // remains queued. current/unique_current continue to test pending state.
    bool queued()const noexcept{return queued_.load(std::memory_order_acquire);}
    bool take(BufferEvent &e){
        std::lock_guard lock(mutex_);
        const bool have=!events_.empty();
        if(have){e=std::move(events_.front());events_.pop_front();}
        if(events_.empty())queued_.store(false,std::memory_order_release);
        return have;
    }
    void applied(const BufferEvent &e){
        std::lock_guard lock(mutex_);
        if(e.created){auto p=pending_creations_.find(e.resource.handle);if(p!=pending_creations_.end()&&p->second.creation==e.creation)pending_creations_.erase(p);}
        auto p=pending_.find(e.resource.handle);if(p!=pending_.end()&&!--p->second)pending_.erase(p);
    }
};
class RuntimeMutex {
    std::mutex mutex_;
    std::atomic<DWORD> owner_{0};
public:
    void lock(){mutex_.lock();owner_.store(GetCurrentThreadId(),std::memory_order_release);}
    void unlock(){owner_.store(0,std::memory_order_release);mutex_.unlock();}
    DWORD owner()const{return owner_.load(std::memory_order_acquire);}
};
// Bounded ReShade log boundaries, armed only by an actually recorded private
// batch. Separate buckets prevent resource chatter hiding submit/Signal data.
struct FlowTrace {
    std::atomic<bool> armed{false};
    std::atomic<unsigned> sequence{0};
    std::array<std::atomic<unsigned>,16> counts{};
    unsigned bucket(const char *name)const{
        if(std::strstr(name,"first_private"))return 15;
        if(std::strstr(name,"instances")||std::strstr(name,"tlas"))return 0;
        if(std::strstr(name,"submit_before")||!std::strcmp(name,"execute"))return 1;
        if(std::strstr(name,"submit_after")||!std::strcmp(name,"forward_queue"))return 2;
        if(std::strstr(name,"signal"))return 3;
        if(std::strstr(name,"completed")||std::strstr(name,"completion"))return 4;
        if(std::strstr(name,"resource"))return 5;
        if(std::strstr(name,"reset")||std::strstr(name,"destroy_list"))return 6;
        if(std::strstr(name,"blas"))return 7;
        if(std::strstr(name,"hit")||std::strstr(name,"sbt"))return 8;
        if(std::strstr(name,"Surface")||std::strstr(name,"Geometry"))return 9;
        if(std::strstr(name,"Palette"))return 10;
        if(std::strstr(name,"Identify"))return 11;
        if(!std::strcmp(name,"present"))return 12;
        if(!std::strcmp(name,"dispatch"))return 13;
        return 14;
    }
    void emit(const char *name,const char *phase,std::uint64_t a=0,std::uint64_t b=0,std::uint64_t c=0){
        if(!armed.load(std::memory_order_acquire))return;
        auto &count=counts[bucket(name)];if(count.load(std::memory_order_relaxed)>=24||count.fetch_add(1,std::memory_order_relaxed)>=24)return;
        char line[384];std::snprintf(line,sizeof(line),"RT-FLOW seq=%u tid=%lu step=%s phase=%s a=0x%llx b=0x%llx c=0x%llx",sequence.fetch_add(1)+1,static_cast<unsigned long>(GetCurrentThreadId()),name,phase,static_cast<unsigned long long>(a),static_cast<unsigned long long>(b),static_cast<unsigned long long>(c));
        reshade::log::message(reshade::log::level::info,line);
    }
};
}
