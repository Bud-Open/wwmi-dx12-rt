#include "rt_published_texture.hpp"

namespace {
ContentId PublishedContentLease(const HeapKey& key,PreviousPair& pair,ResInfo* info=nullptr,
    uint64_t* write=nullptr,uint32_t* frame=nullptr,uint32_t* kind=nullptr) {
    if(!PreviousDevice(g_reshade_device,pair,false))return {};
    const auto h=ReadHeap(key.heap,pair);
    if(!h||key.offset>=h->flat.size)return {};
    std::shared_lock lock(h->page(key.offset).mutex);
    if(!PreviousCurrent(pair,*h))return {};
    const auto& s=h->slots[key.offset];
    if(info)ProjectDescriptorInfo(pair,s,*info);
    if(write)*write=s.version();
    if(frame)*frame=s.frame;
    if(kind)*kind=uint32_t(s.write&3);
    return s.content;
}
bool LivePhysicalOrigin(PreviousPair pair,const ContentId& content) {
    if(!content)return false;
    const auto cpu=content->definition.origin_cpu;
    if(!cpu)return false;
    RawCpuView raw;
    if(raw_cpu_views.find(cpu,raw))return raw.device==pair.device&&raw.content==content;
    HeapKey origin{};
    if(!TrackedSlotOfCpu(cpu,&origin))return false;
    const auto h=ReadHeap(origin.heap,pair);
    if(!h||origin.offset>=h->flat.size)return false;
    std::shared_lock lock(h->page(origin.offset).mutex);
    return PreviousCurrent(pair,*h)&&h->slots[origin.offset].content==content;
}
}
bool ReadPublishedDescriptor(const FlatHeapMap::Entry* entry,uint32_t index,ResInfo& out) noexcept {
    out={};try{
        PreviousPair pair;
        if(!entry||!PreviousDevice(g_reshade_device,pair,false))return false;
        const auto heap=entry->id.load(std::memory_order_acquire);
        if(heap<=FlatHeapMap::kDead)return false;
        const auto h=PreviousGetHeap(PreviousToken(heap,const_cast<FlatHeapMap::Entry*>(entry)),pair);
        if(!h||index>=h->flat.size)return false;
        std::shared_lock lock(h->page(index).mutex);
        return PreviousCurrent(pair,*h)&&ProjectDescriptorInfo(pair,h->slots[index],out);
    }catch(...){return false;}
}
bool ReadPublishedDescriptor(const HeapKey& key,ResInfo& out) noexcept {
    return ReadPublishedDescriptor(g_heap_to_res.entry_of(key.heap),key.offset,out);
}
bool ReadPublishedDescriptorAndFormat(const HeapKey& key,ResInfo& out,DXGI_FORMAT& out_fmt,uint64_t* out_res_gen=nullptr) noexcept {
    out={};
    out_fmt=DXGI_FORMAT_UNKNOWN;
    if(out_res_gen)*out_res_gen=0;
    try{
        PreviousPair pair;
        if(!PreviousDevice(g_reshade_device,pair,false))return false;
        const auto h=ReadHeap(key.heap,pair);
        if(!h||key.offset>=h->flat.size)return false;
        std::shared_lock lock(h->page(key.offset).mutex);
        if(!PreviousCurrent(pair,*h))return false;
        const auto& s=h->slots[key.offset];
        if(!ProjectDescriptorInfo(pair,s,out))return false;
        const auto* d=OriginalDefinition(s.content->definition);
        if(d){
            if(out_res_gen)*out_res_gen=d->resource_generation;
            if(const auto* v=std::get_if<D3D12_SHADER_RESOURCE_VIEW_DESC>(&d->description))
                out_fmt=v->Format;
            else if(const auto* v=std::get_if<D3D12_UNORDERED_ACCESS_VIEW_DESC>(&d->description))
                out_fmt=v->Format;
            else
                out_fmt=d->default_format;
        }
        return true;
    }catch(...){return false;}
}

RtPublishedTexture ReadPublishedTextureShape(const HeapKey& key,RtPublishedTextureShape& out) noexcept {
    out={};
    try{
        PreviousPair pair;
        const auto content=PublishedContentLease(key,pair);
        if(!content)return RtPublishedTexture::missing;
        const auto* source=OriginalDefinition(content->definition);
        if(!source||!source->info.res)return RtPublishedTexture::missing;
        if(source->kind!=WWMI_DESCRIPTOR_SRV)return RtPublishedTexture::wrong_view;
        // Look up the same live identity as ProjectDescriptorInfo without a
        // second lookup in the global texture-replacement cache.
        const auto resource=PreviousFindResource(reinterpret_cast<uint64_t>(source->info.res));
        if(!source->resource_generation||source->device!=pair.device||
           resource.device!=pair.device||resource.generation!=source->resource_generation)
            return RtPublishedTexture::stale;
        if(!resource.shape_known)return RtPublishedTexture::unknown;
        if(!resource.texture_2d)return RtPublishedTexture::non_texture_2d;
        if(!resource.native_width||resource.native_width>UINT32_MAX||!resource.texture_height||
           resource.default_format==DXGI_FORMAT_UNKNOWN)return RtPublishedTexture::unknown;
        out={unsigned(resource.default_format),unsigned(resource.native_width),resource.texture_height,
             resource.texture_mips,resource.texture_layers};
        return RtPublishedTexture::ready;
    }catch(...){return RtPublishedTexture::missing;}
}
void NotePublishedDescriptorMiss(const HeapKey& key,uint32_t frame) noexcept {
    try{
        PreviousPair pair;if(!PreviousDevice(g_reshade_device,pair,false))return;
        const auto h=ReadHeap(key.heap,pair);
        if(!h||key.offset>=h->flat.size)return;
        std::unique_lock lock(h->page(key.offset).mutex);
        if(PreviousCurrent(pair,*h))h->slots[key.offset].miss=frame;
    }catch(...){}
}
bool ReadPublishedBuffer(const HeapKey& key,WWMI_RtDescriptorBuffer_v2& out) noexcept {
    try{
        PreviousPair pair;
        const auto content=PublishedContentLease(key,pair,nullptr,nullptr,&out.base.wrote_frame,&out.base.wrote_source);
        if(!content)return false;
        const auto& d=content->definition;
        if(!d.info.is_buffer||!d.info.res||!DefinitionLive(pair.device,d))return false;
        const auto assign=[&](const auto& view){
            out.first=view.Buffer.FirstElement;out.count=view.Buffer.NumElements;out.format=uint32_t(view.Format);
            out.stride=view.Buffer.StructureByteStride;out.flags=uint32_t(view.Buffer.Flags);
            out.base.resource=reinterpret_cast<uint64_t>(d.info.res);
            return true;
        };
        if(const auto* srv=std::get_if<D3D12_SHADER_RESOURCE_VIEW_DESC>(&d.description);
            srv&&srv->ViewDimension==D3D12_SRV_DIMENSION_BUFFER)return assign(*srv);
        if(const auto* uav=std::get_if<D3D12_UNORDERED_ACCESS_VIEW_DESC>(&d.description);
            uav&&uav->ViewDimension==D3D12_UAV_DIMENSION_BUFFER)return assign(*uav);
        return false;
    }catch(...){return false;}
}
bool ReadPublishedLegacyBuffer(const HeapKey& key,WWMI_RtDescriptorSource_v1& out) noexcept {
    try{
        PreviousPair pair;
        const auto content=PublishedContentLease(key,pair,nullptr,nullptr,&out.wrote_frame,&out.wrote_source);
        if(!content)return false;
        const auto& d=content->definition;
        if(!d.info.is_buffer||!d.info.res||!DefinitionLive(pair.device,d)||!LivePhysicalOrigin(pair,content))return false;
        out.resource=reinterpret_cast<uint64_t>(d.info.res);
        out.source_view=d.origin_cpu;return true;
    }catch(...){return false;}
}
static DXGI_FORMAT PublishedTextureFormat(const HeapKey& key,bool unread_format_only) noexcept {
    try{
        PreviousPair pair;
        const auto content=PublishedContentLease(key,pair);
        if(!content)return DXGI_FORMAT_UNKNOWN;
        const auto* d=OriginalDefinition(content->definition);
        if(!d||!d->info.res||
            (unread_format_only?(content->definition.replacement||d->info.is_buffer):!DefinitionLive(pair.device,*d)))
            return DXGI_FORMAT_UNKNOWN;
        if(const auto* v=std::get_if<D3D12_SHADER_RESOURCE_VIEW_DESC>(&d->description))
            return !unread_format_only||v->ViewDimension==D3D12_SRV_DIMENSION_TEXTURE2D||
                v->ViewDimension==D3D12_SRV_DIMENSION_TEXTURE2DARRAY?v->Format:DXGI_FORMAT_UNKNOWN;
        if(const auto* v=std::get_if<D3D12_UNORDERED_ACCESS_VIEW_DESC>(&d->description))
            return !unread_format_only||v->ViewDimension==D3D12_UAV_DIMENSION_TEXTURE2D||
                v->ViewDimension==D3D12_UAV_DIMENSION_TEXTURE2DARRAY?v->Format:DXGI_FORMAT_UNKNOWN;
        return !unread_format_only||d->default_texture_2d?d->default_format:DXGI_FORMAT_UNKNOWN;
    }catch(...){return DXGI_FORMAT_UNKNOWN;}
}
DXGI_FORMAT PublishedViewFormat(const HeapKey& key) noexcept {return PublishedTextureFormat(key,false);}
DXGI_FORMAT PublishedUnreadTextureFormat(const HeapKey& key) noexcept {return PublishedTextureFormat(key,true);}
PublishedDescriptorDiagnostic ReadPublishedDescriptorDiagnostic(const HeapKey& key) noexcept {
    PublishedDescriptorDiagnostic out{};
    try{
        PreviousPair pair;
        const auto content=PublishedContentLease(key,pair,nullptr,&out.write);
        if(content){
            out.origin=content->definition.origin_cpu;
            out.origin_live=LivePhysicalOrigin(pair,content);
        }
    }catch(...){}
    return out;
}
bool MaterializePublishedDescriptor(ID3D12Device* device,const HeapKey& key,D3D12_CPU_DESCRIPTOR_HANDLE dst,ResInfo& info) noexcept {
    info={};try{
        PreviousPair pair;
        const auto content=PublishedContentLease(key,pair,&info);
        return content&&dst.ptr&&MaterializeDefinition(device,content->definition,dst);
    }catch(...){return false;}
}
bool AcquirePublishedMaterialReplica(ID3D12Device* device,const HeapKey& key,ResInfo& info,RtMaterialReplica& out) noexcept {
    info={};out={};
    try{
        PreviousPair pair;
        const auto content=PublishedContentLease(key,pair);
        if(!content||!device||pair.device!=reinterpret_cast<uint64_t>(device))return false;
        const auto& definition=content->definition;
        if(!DefinitionLive(pair.device,definition))return false;
        // This is an actually read binding in the game's recording, not
        // speculative retention of a descriptor ledger. The native caller
        // still owns these valid inputs at this draw; the private recording
        // retains the same resources (including a stamp's effective resource).
        RtMaterialReplica result{};
        result.resources[0]=definition.replacement?definition.replacement:definition.info.res;
        result.resources[1]=definition.counter;
        if(!DefinitionLive(pair.device,definition))return false;
        auto replica=definition.replica;
        result.stamped=bool(replica);
        if(!replica){
            std::call_once(content->material_once,[&] {
                auto candidate=AllocateDescriptorReplica(device);
                if(!candidate)throw std::bad_alloc(); // a future valid consumer may retry
                if(MaterializeDefinition(device,definition,candidate->cpu)){
                    content->material_replica=std::move(candidate);
                    result.initialized=true;
                }
            });
            replica=content->material_replica;
        }
        if(!replica)return false;
        info=definition.info;
        result.cpu=replica->cpu;
        result.owner=std::move(replica);
        out=std::move(result);
        return true;
    }catch(...){return false;}
}
