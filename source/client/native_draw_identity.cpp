#include "native_draw_identity.hpp"
#include <algorithm>
namespace hsin_rt {
namespace {
bool overlaps(std::uint64_t a,std::uint64_t an,std::uint64_t b,std::uint64_t bn){return an&&bn&&(a<=b?b-a<an:a-b<bn);}
bool same_identity(const NativeComponentIdentity &a,const NativeComponentIdentity &b){
    return a.sample.profile==b.sample.profile&&a.sample.component==b.sample.component&&a.sample.recording==b.sample.recording&&a.sample.order==b.sample.order&&
        a.sample.content==b.sample.content&&a.sample.content_offset==b.sample.content_offset&&a.sample.output==b.sample.output&&a.sample.output_offset==b.sample.output_offset&&
        a.indices==b.indices&&a.index_offset==b.index_offset&&a.index_count==b.index_count&&a.index_stride==b.index_stride;
}
}
void NativeDrawIdentityCache::compact(){
    if(std::erase_if(entries_,[](const auto &e){return !e->valid;}))++validity_revision_;
}
void NativeDrawIdentityCache::record(std::shared_ptr<const NativeBlasIdentity> identity){
    record_with_state(std::move(identity),0,{});
}
void NativeDrawIdentityCache::record_with_state(std::shared_ptr<const NativeBlasIdentity> identity,unsigned submissions,NativeIdentityFence producer){
    if(!identity||!identity->allocation||!identity->native_blas||identity->components.empty()||!identity->recording.list||!identity->recording.creation||!identity->recording.generation)return;
    if(submissions>1)return;
    if(submissions==1&&(!producer.queue||!producer.fence||!producer.value))return;
    for(const auto &c:identity->components){const auto &s=c.sample;
        if(!s.profile||s.component>=s.profile->components.size()||s.recording!=identity->recording||!s.content_confirmed||s.content_hash!=s.profile->content_hash||
            !s.content||!s.output||!c.indices||(c.index_stride!=2&&c.index_stride!=4)||c.vertices.vertex!=s.output||c.vertices.bytes!=s.profile->bytes()||
            !s.output_base||s.output_base>UINT64_MAX-c.vertices.offset||c.vertex_address!=s.output_base+c.vertices.offset)return;
        const auto &n=s.profile->components[s.component];
        if(c.vertices.offset>UINT64_MAX-12ull*n.first||s.output_offset!=c.vertices.offset+12ull*n.first||s.output_bytes!=12ull*n.count||c.index_count!=n.index_count)return;
    }
    overwritten_blas(identity->allocation,identity->native_blas);
    auto entry=std::make_shared<Entry>();entry->identity=std::move(identity);
    entry->submissions=submissions;if(submissions==1)entry->producer=producer;
    entry->components_live.assign(entry->identity->components.size(),true);
    entry->snapshot=++next_snapshot_;entries_.push_back(std::move(entry));
}
void NativeDrawIdentityCache::reset(Recording recording){
    for(auto &e:entries_)if(e->identity->recording==recording&&!e->submissions)e->valid=false;compact();
}
void NativeDrawIdentityCache::overwritten_blas(ResourceKey allocation,std::uint64_t address){
    for(auto &e:entries_)if(e->identity->native_blas==address&&(!allocation||e->identity->allocation==allocation))e->valid=false;compact();
}
void NativeDrawIdentityCache::written(ResourceKey resource,std::uint64_t offset,std::uint64_t bytes){
    if(!resource||!bytes)return;
    bool changed=false;
    for(auto &e:entries_){if(e->identity->allocation==resource){e->valid=false;continue;}
        for(unsigned i=0;i<e->identity->components.size();++i){if(!e->components_live[i])continue;const auto &c=e->identity->components[i];if((c.sample.output==resource&&overlaps(offset,bytes,c.sample.output_offset,c.sample.output_bytes))||
            (c.sample.content==resource&&overlaps(offset,bytes,c.sample.content_offset,c.sample.profile->bytes()))||
            (c.indices==resource&&overlaps(offset,bytes,c.index_offset,std::uint64_t(c.index_stride)*c.index_count))){e->components_live[i]=false;changed=true;}}
        if(std::none_of(e->components_live.begin(),e->components_live.end(),[](bool live){return live;}))e->valid=false;
    }
    compact();
    if(changed)++validity_revision_;
}
void NativeDrawIdentityCache::destroyed(ResourceKey resource){
    bool changed=false;
    for(auto &e:entries_){if(e->identity->allocation==resource){e->valid=false;continue;}
        for(unsigned i=0;i<e->identity->components.size();++i){if(!e->components_live[i])continue;const auto &c=e->identity->components[i];if(c.sample.output==resource||c.sample.content==resource||c.indices==resource){e->components_live[i]=false;changed=true;}}
        if(std::none_of(e->components_live.begin(),e->components_live.end(),[](bool live){return live;}))e->valid=false;
    }
    compact();
    if(changed)++validity_revision_;
}
NativeDrawIdentityCache::Frozen NativeDrawIdentityCache::freeze(Recording recording) const {
    Frozen frozen;for(const auto &e:entries_)if(e->valid&&e->identity->recording==recording)frozen.entries.push_back(e);return frozen;
}
void NativeDrawIdentityCache::submitted(const Frozen &frozen,NativeIdentityFence producer){
    if(!producer.queue||!producer.fence||!producer.value)return;
    for(const auto &e:frozen.entries){if(!e->valid)continue;
        if(e->submissions++){e->valid=false;continue;} // replay cannot reuse old CPU input proof
        e->producer=producer;
    }
    compact();
}
NativeIdentityFence NativeDrawIdentityCache::producer_of(const Frozen &frozen,const std::shared_ptr<const NativeBlasIdentity> &id) const {
    for(const auto &e:frozen.entries)if(e->identity==id)return e->valid&&e->submissions==1?e->producer:NativeIdentityFence{};
    return {};
}
void NativeDrawIdentityCache::completion(std::uint64_t fence,std::uint64_t value){
    if(!fence)return;
    if(value==UINT64_MAX){completed_.erase(fence);for(auto &e:entries_)if(e->producer.fence==fence)e->valid=false;compact();return;}
    auto &known=completed_[fence];known=std::max(known,value);
}
bool NativeDrawIdentityCache::live(const std::shared_ptr<const NativeBlasIdentity> &identity,unsigned component) const {
    return std::any_of(entries_.begin(),entries_.end(),[&](const auto &entry){return entry->valid&&entry->identity==identity&&component<entry->components_live.size()&&entry->components_live[component];});
}
bool NativeDrawIdentityCache::live_state(const std::shared_ptr<const NativeBlasIdentity> &identity,unsigned &submissions_out,NativeIdentityFence &producer_out) const {
    for(const auto &entry:entries_){
        if(entry->valid&&entry->identity==identity){
            submissions_out=entry->submissions;
            producer_out=entry->producer;
            return true;
        }
    }
    return false;
}
std::optional<NativeDrawIdentityCache::Match> NativeDrawIdentityCache::resolve(const DrawIdentityQuery &q,LookupReceipt *receipt) const {
    if(!q.recording.list||!q.recording.creation||!q.recording.generation||!q.vertices||!q.indices||!q.index_count||q.index_bytes<2ull*q.index_count)return {};
    std::optional<Match> found;
    for(const auto &e:entries_){if(!e->valid)continue;unsigned ordering=0;std::uint64_t completed=0;
        if(e->identity->recording==q.recording)ordering=WWMI_RT_PACK_SAME_RECORDING;
        else if(e->submissions==1&&e->producer.fence){const auto point=completed_.find(e->producer.fence);
            if(point!=completed_.end()){
                completed=point->second;
                if(completed>=e->producer.value)ordering=WWMI_RT_PACK_PRODUCER_FENCE_COMPLETED;
            }
        }
        if(!ordering&&!receipt)continue;
        for(unsigned i=0;i<e->identity->components.size();++i){const auto &c=e->identity->components[i];if(!e->components_live[i])continue;
            if(c.vertices.vertex!=q.vertices||c.indices!=q.indices||c.vertex_address!=q.vertex_address||c.index_address!=q.index_address||c.index_count!=q.index_count||q.index_bytes!=std::uint64_t(c.index_stride)*c.index_count||q.vertex_bytes<c.vertices.bytes)continue;
            Match candidate{e->identity,i,ordering,e->producer,completed,e->submissions,e->snapshot};
            if(receipt&&receipt->exact_matches++==0)receipt->first_exact=candidate;
            if(!ordering)continue;
            if(found&&!same_identity(found->identity->components[found->component],c)){
                if(receipt)receipt->ambiguous=true;
                return {};
            }
            if(!found||ordering==WWMI_RT_PACK_SAME_RECORDING)found=std::move(candidate);
        }
    }
    return found;
}
std::optional<NativeDrawIdentityCache::Match> NativeDrawIdentityCache::resolve_snapshot(const DrawIdentityQuery &q,LookupReceipt &receipt) const {
    // Reuse the exact subview and current generation matcher. Count even the
    // unsubmitted foreign candidates: any competing exact owner is ambiguous.
    resolve(q,&receipt);
    if(receipt.ambiguous||receipt.exact_matches!=1||!receipt.first_exact)return {};
    const auto &m=*receipt.first_exact;
    // Author snapshots consume owned CPU bone palettes, not native GPU buffers.
    // Permit both same-recording and parallel cross-recording unsubmitted
    // producers (verified active/un-reset by read_author_snapshot), as well as
    // single-submitted producers.
    if(m.submissions==0)return m;
    if(m.submissions==1&&m.producer.queue&&m.producer.fence&&m.producer.value)return m;
    return {};
}
bool NativeDrawIdentityCache::might_resolve(const DrawIdentityQuery &q)const{
    for(const auto &e:entries_){if(!e->valid)continue;
        for(unsigned i=0;i<e->identity->components.size();++i){const auto &c=e->identity->components[i];
            if(e->components_live[i]&&c.vertex_address==q.vertex_address&&c.index_address==q.index_address&&
                c.index_count==q.index_count&&q.index_bytes==std::uint64_t(c.index_stride)*c.index_count&&q.vertex_bytes>=c.vertices.bytes)return true;
        }
    }
    return false;
}
}
