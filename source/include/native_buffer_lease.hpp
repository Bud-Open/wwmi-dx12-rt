#pragma once
#include "rt_native_buffer_receipt.h"
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>
// A lease belongs to this recording, not the last CPU callback on another
// list. The object tag only remembers whether legacy-only is still possible.
namespace wwmi::native_buffer {
inline constexpr GUID mode_guid{0x46a22e77,0x7f4a,0x42c6,{0x81,0x65,0xbc,0x30,0x53,0xf4,0x29,0x81}};
struct Mode {uint32_t mode=0,reserved=0;uint64_t epoch=0,bytes=0;};
struct Range {uint64_t resource=0,offset=0,bytes=0,sync=0,access=0;uint32_t mode=0;};
struct RecordingLease {std::vector<Range> ranges;bool unpredicated=true;};
inline std::atomic<uint64_t> alias_epoch{1};
inline std::mutex mutex;
inline std::unordered_map<uint64_t,RecordingLease> recordings;
inline bool overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m){return n&&m&&a<=UINT64_MAX-n&&b<=UINT64_MAX-m&&a<b+m&&b<a+n;}
inline void mode(ID3D12Resource *resource,uint32_t value){
    if(!resource)return;const auto desc=resource->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)return;
    const Mode tag{value,0,alias_epoch.load(std::memory_order_acquire),desc.Width};resource->SetPrivateData(mode_guid,sizeof(tag),&tag);
}
inline void created(ID3D12Resource *resource,bool enhanced){mode(resource,enhanced?2u:1u);}
inline void reset(uint64_t list){std::lock_guard<std::mutex> lock(mutex);recordings[list]={};}
inline void destroy(uint64_t list){std::lock_guard<std::mutex> lock(mutex);recordings.erase(list);}
inline void predicate(uint64_t list,bool disabled){std::lock_guard<std::mutex> lock(mutex);recordings[list].unpredicated=disabled;}
inline bool unpredicated(uint64_t list){std::lock_guard<std::mutex> lock(mutex);const auto it=recordings.find(list);return it!=recordings.end()&&it->second.unpredicated;}
inline void unknown(){alias_epoch.fetch_add(1,std::memory_order_acq_rel);std::lock_guard<std::mutex> lock(mutex);for(auto &[_,r]:recordings)r.ranges.clear();}
inline void opaque(uint64_t list){unknown();predicate(list,false);}
inline void erase(uint64_t list,ID3D12Resource *resource,uint64_t offset=0,uint64_t bytes=UINT64_MAX){
    std::lock_guard<std::mutex> lock(mutex);auto it=recordings.find(list);if(it==recordings.end())return;
    auto &v=it->second.ranges;v.erase(std::remove_if(v.begin(),v.end(),[&](const Range &r){return r.resource==uint64_t(resource)&&overlap(r.offset,r.bytes,offset,bytes);}),v.end());
}
inline void legacy(uint64_t list,UINT count,const D3D12_RESOURCE_BARRIER *barriers){
    for(UINT i=0;i<count;++i){const auto &b=barriers[i];
        if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){unknown();mode(b.Aliasing.pResourceBefore,0);mode(b.Aliasing.pResourceAfter,0);continue;}
        if(b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION||!b.Transition.pResource)continue;
        auto *resource=b.Transition.pResource;const auto desc=resource->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)continue;
        erase(list,resource);
        if(b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_NONE){mode(resource,0);continue;}
        // A complete local transition can establish a local legacy read
        // class. It must not globally erase enhanced/split observations.
        Mode tag{};UINT n=sizeof(tag);if(FAILED(resource->GetPrivateData(mode_guid,&n,&tag))||n!=sizeof(tag)||tag.epoch!=alias_epoch.load(std::memory_order_acquire))mode(resource,0);
        const auto readable=(b.Transition.StateAfter&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)!=0;
        std::lock_guard<std::mutex> lock(mutex);recordings[list].ranges.push_back({uint64_t(resource),0,desc.Width,0,0,readable?1u:0u});
    }
}
inline void enhanced(uint64_t list,UINT count,const D3D12_BARRIER_GROUP *groups){
    for(UINT k=0;k<count;++k){const auto &group=groups[k];
        if(group.Type==D3D12_BARRIER_TYPE_GLOBAL){unknown();continue;}
        if(group.Type!=D3D12_BARRIER_TYPE_BUFFER)continue;
        for(UINT i=0;i<group.NumBarriers;++i){const auto &b=group.pBufferBarriers[i];if(!b.pResource)continue;
            mode(b.pResource,2);const auto desc=b.pResource->GetDesc();
            const auto bytes=b.Size==UINT64_MAX?(b.Offset<=desc.Width?desc.Width-b.Offset:0):b.Size;
            erase(list,b.pResource,b.Offset,bytes);
            const bool split=b.SyncBefore==D3D12_BARRIER_SYNC_SPLIT||b.SyncAfter==D3D12_BARRIER_SYNC_SPLIT;
            if(split)mode(b.pResource,0);
            if(b.Offset>desc.Width||bytes>desc.Width-b.Offset||!bytes)continue;
            std::lock_guard<std::mutex> lock(mutex);recordings[list].ranges.push_back({uint64_t(b.pResource),b.Offset,bytes,uint64_t(b.SyncAfter),uint64_t(b.AccessAfter),split?0u:2u});
        }
    }
}
inline bool compute_sync(uint64_t sync){return (sync&(D3D12_BARRIER_SYNC_ALL|D3D12_BARRIER_SYNC_ALL_SHADING|D3D12_BARRIER_SYNC_NON_PIXEL_SHADING|D3D12_BARRIER_SYNC_COMPUTE_SHADING))!=0;}
inline int32_t read(uint64_t list,ID3D12Resource *resource,uint64_t offset,uint64_t bytes,uint32_t point,WWMI_RtNativeBufferLease_v1 *out){
    if(!list||!resource||!bytes||!out||out->size!=sizeof(*out)||(point!=1&&point!=2))return 3;
    *out={sizeof(*out),1,0,0,0,0};Mode tag{};UINT n=sizeof(tag);
    const auto reject=[&](uint32_t reason){out->reserved=reason;return int32_t(2);};
    if(FAILED(resource->GetPrivateData(mode_guid,&n,&tag))||n!=sizeof(tag))return reject(WWMI_RT_LEASE_TAG_MISSING);
    if(tag.epoch!=alias_epoch.load(std::memory_order_acquire))return reject(WWMI_RT_LEASE_TAG_EPOCH);
    if(offset>tag.bytes||bytes>tag.bytes-offset)return reject(WWMI_RT_LEASE_DOMAIN);
    std::lock_guard<std::mutex> lock(mutex);const auto found=recordings.find(list);if(found==recordings.end())return reject(WWMI_RT_LEASE_RECORDING_MISSING);
    if(!found->second.unpredicated)return reject(WWMI_RT_LEASE_PREDICATION);
    bool touched=false;
    for(auto it=found->second.ranges.rbegin();it!=found->second.ranges.rend();++it){const auto &r=*it;if(r.resource!=uint64_t(resource)||!overlap(r.offset,r.bytes,offset,bytes))continue;
        touched=true;if(offset<r.offset||offset-r.offset>r.bytes||bytes>r.bytes-(offset-r.offset))return reject(WWMI_RT_LEASE_LOCAL_COVERAGE);
        if(r.mode==1){out->mode=1;return 0;}
        if(r.mode==2&&(r.access&D3D12_BARRIER_ACCESS_SHADER_RESOURCE)&&r.sync!=D3D12_BARRIER_SYNC_NONE&&(point==2||compute_sync(r.sync))){*out={sizeof(*out),1,2,0,r.sync,r.access};return 0;}
        if(r.mode==2){
            if(!(r.access&D3D12_BARRIER_ACCESS_SHADER_RESOURCE))return reject(WWMI_RT_LEASE_LOCAL_ACCESS);
            return reject(WWMI_RT_LEASE_LOCAL_SYNC);
        }
        return reject(WWMI_RT_LEASE_LOCAL_MODE);
    }
    if(!touched&&tag.mode==1){out->mode=1;return 0;}
    return reject(tag.mode==2?WWMI_RT_LEASE_ENHANCED_UNPROVEN:WWMI_RT_LEASE_MODE_UNKNOWN);
}
}
