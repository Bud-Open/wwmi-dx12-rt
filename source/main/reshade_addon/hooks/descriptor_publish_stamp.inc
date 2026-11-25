namespace {
struct DescriptorReplicaPool {
    std::mutex mutex;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> heaps;
    std::vector<uint32_t> free;
    uint32_t next=0,increment=0;
};
struct DescriptorReplica {
    std::shared_ptr<DescriptorReplicaPool> pool;
    Microsoft::WRL::ComPtr<ID3D12Resource> owned_replacement;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    uint32_t index=0;
    ~DescriptorReplica(){
        if(cpu.ptr)try{std::lock_guard lock(pool->mutex);pool->free.push_back(index);}catch(...){}
    }
};
std::shared_ptr<DescriptorReplica> AllocateDescriptorReplica(ID3D12Device* device,ID3D12Resource* replacement=nullptr) {
    static std::mutex device_mutex;
    static std::weak_ptr<DescriptorReplicaPool> last;
    std::shared_ptr<DescriptorReplicaPool> pool;
    {
        std::lock_guard lock(device_mutex);
        pool=last.lock();
        if(!pool||pool->device.Get()!=device){
            pool=std::make_shared<DescriptorReplicaPool>();pool->device=device;
            pool->increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            last=pool;
        }
    }
    constexpr uint32_t block=4096;
    auto replica=std::make_shared<DescriptorReplica>();replica->pool=pool;replica->owned_replacement=replacement;
    {
        std::lock_guard lock(pool->mutex);
        if(!pool->free.empty()){replica->index=pool->free.back();pool->free.pop_back();}
        else{
            if(pool->next==pool->heaps.size()*block){
                Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
                const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,block,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
                if(FAILED(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&heap))))return {};
                pool->heaps.push_back(std::move(heap));
            }
            replica->index=pool->next++;
        }
        replica->cpu=pool->heaps[replica->index/block]->GetCPUDescriptorHandleForHeapStart();
        replica->cpu.ptr+=SIZE_T(replica->index%block)*pool->increment;
    }
    return replica;
}
std::shared_ptr<const DescriptorReplica> SaveDescriptorReplica(D3D12_CPU_DESCRIPTOR_HANDLE source,ID3D12Resource* replacement=nullptr) {
    auto replica=AllocateDescriptorReplica(g_d3d12_device,replacement);
    if(replica)g_d3d12_device->CopyDescriptorsSimple(1,replica->cpu,source,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return replica;
}

bool MaterializeDefinition(ID3D12Device* device,const DescriptorDefinition& definition,D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    if(definition.device!=reinterpret_cast<uint64_t>(device))return false;
    if(definition.replica){
        device->CopyDescriptorsSimple(1,destination,definition.replica->cpu,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }
    if(!DefinitionLive(definition.device,definition))return false;
    switch(definition.kind){
    case WWMI_DESCRIPTOR_SRV:{
        const auto* view=std::get_if<D3D12_SHADER_RESOURCE_VIEW_DESC>(&definition.description);
        // The host tracks an AS by the allocation behind its VA. This add-on
        // does not infer that allocation's lifetime from a resource-less SRV.
        if(view&&view->ViewDimension==D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)return false;
        device->CreateShaderResourceView(definition.info.res,view,destination);return true;
    }
    case WWMI_DESCRIPTOR_UAV:
        device->CreateUnorderedAccessView(definition.info.res,definition.counter,
            std::get_if<D3D12_UNORDERED_ACCESS_VIEW_DESC>(&definition.description),destination);return true;
    case WWMI_DESCRIPTOR_CBV:{
        const auto* view=std::get_if<D3D12_CONSTANT_BUFFER_VIEW_DESC>(&definition.description);
        if(view&&view->BufferLocation&&!definition.info.res)return false;
        device->CreateConstantBufferView(view,destination);return true;
    }
    default:return false;
    }
}
D3D12_CPU_DESCRIPTOR_HANDLE HeapCpu(const PreviousHeap& h,uint32_t slot) {
    const auto inc=g_d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return {SIZE_T(h.flat.cpu+uint64_t(slot)*inc)};
}
PreviousHeapPtr RawDestination(HeapKey key,PreviousPair& pair,bool create=false) {
    if(!key.heap||!g_d3d12_device||!PreviousDevice(g_reshade_device,pair,false))return {};
    auto* entry=create?EnterPreviousHeapIdentity(key.heap):g_heap_to_res.entry_of(key.heap);
    const auto t=PreviousToken(key.heap,entry);
    return key.offset<t.size?PreviousGetHeap(t,pair,0,create):nullptr;
}
void PublishRawEdit(const RawDescriptorAdmission& admission,PreviousHeap& h,uint32_t slot,ContentId content) noexcept {
    const auto gen=h.host_generation.load(std::memory_order_acquire);
    try{
        admission.after(reinterpret_cast<uint64_t>(g_reshade_device),h.flat.handle,gen,
            h.host_index.load(std::memory_order_relaxed),slot);
    }catch(...){}
    PublishContent(h,slot,h.write_version(),std::move(content),3);
    try{UavObserve::g_probe.invalidate({h.flat.handle,slot});}catch(...){}
}
void RawCopyTo(const RawDescriptorAdmission& admission,PreviousHeap& h,uint32_t slot,
    D3D12_CPU_DESCRIPTOR_HANDLE source,ContentId content) {
    g_d3d12_device->CopyDescriptorsSimple(1,HeapCpu(h,slot),source,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    PublishRawEdit(admission,h,slot,std::move(content));
}
ContentId DefineTextureStamp(PreviousPair pair,const PreviousHeap& h,uint32_t slot,
    D3D12_CPU_DESCRIPTOR_HANDLE source,ID3D12Resource* replacement) {
    DescriptorDefinition stamp{};
    stamp.device=pair.device;stamp.replacement=replacement;stamp.replacement_cpu=source.ptr;
    stamp.replica=SaveDescriptorReplica(source,replacement);
    if(!stamp.replica)return {};
    if(replacement){
        const auto& old=h.slots[slot];
        if(old.content&&old.content->definition.replacement){
            stamp.original=old.content->definition.original;
            stamp.original_replica=old.content->definition.original_replica;
        }else{
            stamp.original=old.content;
            // A covered but opaque CPU slot can preserve its exact physical
            // bytes. A shader-visible or uncovered slot is not a copy source.
            if(!old.content&&old.version()&&!h.flat.entry->vis.load(std::memory_order_relaxed))
                stamp.original_replica=SaveDescriptorReplica(HeapCpu(h,slot));
        }
    }
    return NewDescriptor(std::move(stamp));
}

template<class F> void ForEachPublishedStamp(F&& visit) {
    PreviousPair pair;if(!PreviousDevice(g_reshade_device,pair,false))return;
    std::vector<PreviousHeapPtr> heaps;
    for(auto& shard:previous_heaps){
        std::shared_lock lock(shard.mutex);
        for(const auto& entry:shard.heaps)heaps.push_back(entry.second);
    }
    for(const auto& h:heaps){
        const auto visible=h->flat.entry->vis.load(std::memory_order_relaxed)!=0;
        for(uint32_t first=0;first<h->flat.size;){
            const auto end=PreviousPageEnd(first,h->flat.size);
            std::array<std::pair<uint32_t,ID3D12Resource*>,previous_page_slots> found;
            unsigned count=0;
            auto& page=h->page(first);
            {
                std::shared_lock lock(page.mutex);
                if(!PreviousCurrent(pair,*h))break;
                if(page.may_stamp)for(uint32_t slot=first;slot<end;++slot)
                    if(const auto* d=PublishedDefinition(*h,slot);d&&d->replacement)found[count++]={slot,d->replacement};
            }
            for(unsigned i=0;i<count;++i)visit(h->flat.handle,visible,found[i].first,found[i].second);
            first=end;
        }
    }
}
}
bool StampTextureDescriptor(ID3D12Device* dev,uint64_t heap,uint32_t slot,ID3D12Resource* original,
    D3D12_CPU_DESCRIPTOR_HANDLE source,ID3D12Resource* replacement) {
    if(!dev||dev!=g_d3d12_device||!source.ptr)return false;
    try{
        PreviousPair pair;
        const auto h=RawDestination({heap,slot},pair,!original);
        if(!h)return false;
        RawDescriptorAdmission admission(HeapCpu(*h,slot).ptr,source.ptr);
        if(!admission)return false;
        std::unique_lock lock(h->page(slot).mutex);
        if(!PreviousCurrent(pair,*h))return false;
        if(original){
            ResInfo ri{};
            if(!ProjectDescriptorInfo(pair,h->slots[slot],ri)||ri.res!=original||ri.is_buffer)return false;
        }
        auto stamp=DefineTextureStamp(pair,*h,slot,source,replacement);
        if(!stamp)return false;
        const auto replica=stamp->definition.replica->cpu;
        RawCopyTo(admission,*h,slot,replica,std::move(stamp));
        return true;
    }catch(...){return false;}
}

static bool UnstampSlotImpl(uint64_t heap,uint32_t slot,bool log,ID3D12Resource* expected) {
    try{
        PreviousPair pair;
        const auto h=RawDestination({heap,slot},pair);
        if(!h)return false;
        const auto cpu=HeapCpu(*h,slot);
        RawDescriptorAdmission admission(cpu.ptr,cpu.ptr);
        if(!admission)return false;
        std::unique_lock lock(h->page(slot).mutex);
        if(!PreviousCurrent(pair,*h))return false;
        const auto* current=PublishedDefinition(*h,slot);
        if(!current||!current->replacement||(expected&&current->replacement!=expected))return false;
        const auto original=current->original;
        const auto* definition=original?&original->definition:nullptr;
        if(definition&&MaterializeDefinition(g_d3d12_device,*definition,cpu))
            PublishRawEdit(admission,*h,slot,original);
        else if(current->original_replica&&(!definition||DefinitionLive(pair.device,*definition))){
            const auto replica=current->original_replica->cpu;
            RawCopyTo(admission,*h,slot,replica,original);
        }else{
            if(log)logger::write("UNSTAMP heap=%llx slot %u: no live original descriptor; left unchanged",
                (unsigned long long)heap,slot);
            return false;
        }
        if(log)logger::write("UNSTAMP res=%p slot %u: restored the game's descriptor",
            definition?(void*)definition->info.res:nullptr,slot);
        return true;
    }catch(...){return false;}
}
bool UnstampSlot(uint64_t heap,uint32_t slot,bool log) {
    return UnstampSlotImpl(heap,slot,log,nullptr);
}
