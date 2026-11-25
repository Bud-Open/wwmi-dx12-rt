#pragma once
#include "hsin_asset_layout.hpp"
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace hsin_rt {
struct NativeIndexWindow {
    unsigned component=0,first=0,count=0;
    bool operator==(const NativeIndexWindow &) const = default;
};
// Imported native identity, independent of author assets and profile presence.
struct NativeMeshIntent {
    std::uint64_t load_generation=0,mod_key=0,mod_revision=0;
    unsigned object_guid=0,content_hash=0;
    bool enabled=false;
    std::vector<NativeIndexWindow> components;
    bool matches(const NativeProfile &p) const {
        if(object_guid!=p.object_guid||content_hash!=p.content_hash||components.size()!=p.components.size())return false;
        for(const auto &n:p.components)
            if(std::none_of(components.begin(),components.end(),[&](const auto &c){return c.component==n.component&&c.first==n.index_first&&c.count==n.index_count;}))return false;
        return true;
    }
};
// This registry admits the existing float3/stride12, U32, no-transform layout.
// Every variable NativeProfile field is part of its canonical signature.
class NativeProfileRegistry {
public:
    struct Snapshot {std::uint64_t revision=0;std::vector<std::shared_ptr<const NativeProfile>> profiles;};
    using SnapshotRef=std::shared_ptr<const Snapshot>;
    enum class Match {missing,unique,ambiguous};
    struct Resolution {Match match=Match::missing;std::shared_ptr<const NativeProfile> profile;};
    struct Publication {std::shared_ptr<const NativeProfile> profile;std::uint64_t revision=0;bool inserted=false;};
private:
    mutable std::mutex mutex_;
    SnapshotRef snapshot_=std::make_shared<const Snapshot>();
    static bool same(const NativeProfile &a,const NativeProfile &b);
    static void validate(const NativeProfile &p);
public:
    NativeProfileRegistry()=default;
    explicit NativeProfileRegistry(const std::vector<std::shared_ptr<const NativeProfile>> &seeds) {
        // Trusted immutable V1 seeds retain their existing pointer identity,
        // including legacy callers that share that vector between collectors.
        auto state=std::make_shared<Snapshot>();
        for(const auto &p:seeds){
            if(!p)throw std::runtime_error("RT native registry null seed");validate(*p);
            if(std::none_of(state->profiles.begin(),state->profiles.end(),[&](const auto &q){return same(*p,*q);})){
                state->profiles.push_back(p);++state->revision;
            }
        }
        snapshot_=std::move(state);
    }
    SnapshotRef snapshot() const noexcept {return std::atomic_load_explicit(&snapshot_,std::memory_order_acquire);}
    Publication publish(const NativeProfile &value);
    static Resolution resolve(const SnapshotRef &state,const NativeMeshIntent &intent) {
        Resolution result;
        for(const auto &p:state->profiles)if(intent.matches(*p)){
            if(result.profile)return {Match::ambiguous,{}};
            result={Match::unique,p};
        }
        return result;
    }
};
inline bool NativeProfileRegistry::same(const NativeProfile &a,const NativeProfile &b) {
    if(a.object_guid!=b.object_guid||a.content_hash!=b.content_hash||a.vertices!=b.vertices||a.components.size()!=b.components.size())return false;
    for(std::size_t i=0;i<a.components.size();++i){const auto &x=a.components[i],&y=b.components[i];
        if(x.component!=y.component||x.first!=y.first||x.count!=y.count||x.index_first!=y.index_first||x.index_count!=y.index_count)return false;
    }
    return true;
}
inline void NativeProfileRegistry::validate(const NativeProfile &p) {
    if(!p.object_guid||!p.content_hash||!p.vertices||p.vertices>UINT32_MAX/12||p.components.empty()||p.components.size()>4096)
        throw std::runtime_error("RT native registry profile bounds");
    unsigned next=0;
    for(std::size_t i=0;i<p.components.size();++i){const auto &n=p.components[i];
        if(n.first!=next||!n.count||n.first>p.vertices||n.count>p.vertices-n.first||!n.index_count||n.index_count%3||n.index_first>UINT32_MAX-n.index_count)
            throw std::runtime_error("RT native registry component windows invalid");
        for(std::size_t j=0;j<i;++j)if(p.components[j].component==n.component)
            throw std::runtime_error("RT native registry component identity duplicate");
        next+=n.count;
    }
    if(next!=p.vertices)throw std::runtime_error("RT native registry profile incomplete");
}
inline NativeProfileRegistry::Publication NativeProfileRegistry::publish(const NativeProfile &value) {
    // Never retain a caller's potentially mutable shared object.
    NativeProfile copy=value;
    std::sort(copy.components.begin(),copy.components.end(),[](const auto &a,const auto &b){return a.first<b.first;});
    validate(copy);std::lock_guard lock(mutex_);const auto old=snapshot();
    for(const auto &p:old->profiles)if(same(*p,copy))return {p,old->revision,false};
    if(old->revision==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("RT native registry revision exhausted");
    auto next=std::make_shared<Snapshot>(*old);++next->revision;
    auto canonical=std::make_shared<const NativeProfile>(std::move(copy));next->profiles.push_back(canonical);
    std::atomic_store_explicit(&snapshot_,SnapshotRef(next),std::memory_order_release);
    return {std::move(canonical),next->revision,true};
}
}
