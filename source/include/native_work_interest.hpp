#pragma once
#include "hsin_pose.hpp" // Use the same candidate-local ResourceKey and pose layout as Runtime.
#include <mutex>
#include <unordered_map>

namespace hsin_rt {
// An admission index for CPU work, NOT object identity. Exact content, current
// resources, pose, native IB subview and actual submission proof stay in the
// existing collector/cache. A VA in this index grants none of those proofs.
class NativeBlasInterest {
    struct Entry {ResourceKey allocation;unsigned pending=0;};
    std::mutex mutex_;
    std::unordered_map<std::uint64_t,Entry> destinations_;
    bool enter(std::uint64_t address,bool structural_candidate){
        std::lock_guard lock(mutex_);
        auto p=destinations_.find(address);
        if(p==destinations_.end()){
            if(!structural_candidate)return false;
            p=destinations_.try_emplace(address).first;
        }
        // Reserve BEFORE waiting for Runtime. Otherwise a later noncandidate
        // overwrite could be skipped while the first candidate is being proved.
        ++p->second.pending;return true;
    }
    void leave(std::uint64_t address){
        std::lock_guard lock(mutex_);auto p=destinations_.find(address);
        if(p==destinations_.end())return;
        if(p->second.pending)--p->second.pending;
        if(!p->second.pending&&!p->second.allocation)destinations_.erase(p);
    }
public:
    class Call {
        NativeBlasInterest &index_;std::uint64_t address_;bool admitted_;
    public:
        Call(NativeBlasInterest &index,std::uint64_t address,bool candidate)
            :index_(index),address_(address),admitted_(index.enter(address,candidate)){}
        Call(const Call &)=delete;
        ~Call(){if(admitted_)index_.leave(address_);}
        explicit operator bool()const{return admitted_;}
    };
    // Runtime owns the following operations. No index lock is held when a
    // caller takes Runtime's lock, calls a provider or releases COM objects.
    void overwritten(std::uint64_t address){
        std::lock_guard lock(mutex_);auto p=destinations_.find(address);
        if(p!=destinations_.end())p->second.allocation={};
    }
    void proved(std::uint64_t address,ResourceKey allocation){
        std::lock_guard lock(mutex_);auto p=destinations_.find(address);
        if(p!=destinations_.end())p->second.allocation=allocation;
    }
    void destroyed(ResourceKey allocation){
        std::lock_guard lock(mutex_);
        for(auto p=destinations_.begin();p!=destinations_.end();){
            if(p->second.allocation==allocation)p->second.allocation={};
            if(!p->second.allocation&&!p->second.pending)p=destinations_.erase(p);else ++p;
        }
    }
};
}
