namespace {
ContentId CaptureNativeView(device* dev,const WWMI_DescriptorWrite_v1& write,PreviousPair pair) {
    DescriptorDefinition result{};
    result.device=pair.device;result.origin_cpu=write.destination.cpu;result.kind=write.kind;
    const auto res=write.resource;
    if(write.kind==WWMI_DESCRIPTOR_CBV){
        if(write.description)result.description=*reinterpret_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(write.description);
        else if(write.cbv_va||write.cbv_bytes)
            result.description=D3D12_CONSTANT_BUFFER_VIEW_DESC{write.cbv_va,write.cbv_bytes};
        if(res&&write.cbv_va&&write.cbv_bytes){
            auto* buffer=reinterpret_cast<ID3D12Resource*>(res);
            const auto life=PreviousDescribeResource(dev,pair.device,res);
            if(life.va&&write.cbv_va>=life.va){
                const auto offset=write.cbv_va-life.va;
                if(offset<=life.native_width&&write.cbv_bytes<=life.native_width-offset){
                    result.resource_generation=life.generation;result.info.res=buffer;
                    if(life.generation)result.typed=DescriptorCbv{{buffer,write.cbv_va,write.cbv_bytes},life.generation};
                }
            }
        }
        return NewDescriptor(std::move(result));
    }
    if(write.kind==WWMI_DESCRIPTOR_SRV){
        if(write.description)result.description=*reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(write.description);
    }else if(write.kind==WWMI_DESCRIPTOR_UAV){
        if(write.description)result.description=*reinterpret_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(write.description);
        if(write.cbv_va){ // v2 publisher: the kind-specific word is the actual UAV counter resource
            result.counter=reinterpret_cast<ID3D12Resource*>(write.cbv_va);
            result.counter_generation=PreviousDescribeResource(dev,pair.device,write.cbv_va).generation;
        }
    }else return {};
    if(!res)return NewDescriptor(std::move(result)); // null view; keep its exact descriptor
    auto* native_res=reinterpret_cast<ID3D12Resource*>(res);
    const auto r=PreviousDescribeResource(dev,pair.device,res);
    if(!r.generation)return {}; // lifetime changed during capture
    result.resource_generation=r.generation;
    const auto cpu=result.origin_cpu;
    if(r.buffer){
        result.info={(uint32_t)r.width,native_res,r.va,false,0,cpu,true};
        result.info.buffer_width=r.width;
        if(const auto* srv=std::get_if<D3D12_SHADER_RESOURCE_VIEW_DESC>(&result.description);
            srv&&srv->ViewDimension==D3D12_SRV_DIMENSION_BUFFER&&r.generation){
            const auto& b=srv->Buffer;
            if(srv->Format==DXGI_FORMAT_R32_FLOAT&&!b.StructureByteStride&&!b.Flags&&b.NumElements&&
                b.FirstElement<=r.width/4&&b.NumElements<=r.width/4-b.FirstElement)
                result.typed=RtPreviousView{res,b.FirstElement*4,uint64_t(b.NumElements)*4,r.generation};
            else{
                RtAuxSource a{};
                a.view={sizeof(a.view),1,res,b.FirstElement,b.NumElements,b.StructureByteStride,
                    (uint32_t)srv->Format,srv->Shader4ComponentMapping,(uint32_t)b.Flags,0};
                a.resource_generation=r.generation;
                if(AuxiliaryFits(pair.device,a,r))result.typed=a;
            }
        }
    }else{
        if(!write.description){
            result.default_format=r.default_format;
            result.default_texture_2d=r.default_texture_2d;
        }
        result.info={0,native_res,0,r.texture_candidate,0,cpu};
    }
    return NewDescriptor(std::move(result));
}

void NotePublishedView(const WWMI_DescriptorWrite_v1& w) {
    if(!UavObserve::g_probe.enabled.load())return;
    const auto& r=w.destination;
    if(w.kind==WWMI_DESCRIPTOR_UAV&&w.description){
        const auto& desc=*reinterpret_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(w.description);
        if(desc.ViewDimension==D3D12_UAV_DIMENSION_BUFFER){
            UavObserve::g_probe.view(r.cpu,w.resource,desc.Buffer.FirstElement,desc.Buffer.NumElements,(uint32_t)desc.Format,true);
            if(r.heap)UavObserve::g_probe.direct({r.heap,r.slot},r.cpu,true);
            return;
        }
    }
    UavObserve::g_probe.view(r.cpu,0,0,0,0,false);
    if(r.heap)UavObserve::g_probe.invalidate({r.heap,r.slot});
}
void OnNativeDescriptorWrite(uint64_t api,const WWMI_DescriptorWrite_v1* packet,
    WWMI_DescriptorNative_v1 native,void* call) noexcept {
    auto* dev=reinterpret_cast<device*>(api);
    if(!dev||dev!=g_reshade_device||!g_is_dx12||!packet||!packet->destination.cpu)return;
    const auto& r=packet->destination;
    // Declared before admission so deferred heap retirement drains only after
    // stripe admission is released (never inverting admission -> copy lanes).
    struct DeferredHeapDrainGuard {
        ~DeferredHeapDrainGuard() noexcept { DrainDeferredRetiredHeaps(); }
    } deferred_drain;

    // Fast path for top-level registered CBV/SRV/UAV writes: use CopyLaneGuard
    // (asymmetric fast-raw admission) + ThreadFastHeapCache to avoid striped CAS
    // admission and PreviousGetRange weak_ptr/shared_ptr atomic refcounting.
    // CopyLaneGuard is strictly destroyed before GlobalTexSwap (RawDescriptorAdmission)
    // and before any fallback to AppDescriptorAdmission.
    const bool eligible_fast=r.heap&&r.heap_creation&&r.increment&&r.slot<r.slots&&
        packet->kind!=WWMI_DESCRIPTOR_RELEASE_CPU_VIEW&&
        packet->kind!=WWMI_DESCRIPTOR_UNKNOWN&&
        CurrentCopyLane().copy_depth==0;
    if(eligible_fast){
        bool handled_fast=false;
        bool fast_ok=false;
        uint64_t fast_native_dev=0;
        ResInfo written{};
        {
            PreviousPair pair;
            if(PreviousDevice(dev,pair,false)){
                CopyLaneGuard lane_guard;
                if(lane_guard.has_copy_depth()&&lane_guard.has_fast_raw()){
                    auto& fast_cache=ThreadFastHeapCache();
                    fast_cache.sync(lane_guard.retire_gen);
                    PreviousHeap* cached=fast_cache.find(r.heap,r.heap_creation,pair.epoch);
                    if(cached&&!cached->retired.load(std::memory_order_acquire)&&
                       cached->flat.live()&&CopyRangeMatchesHeap(cached,r)){
                        handled_fast=true;
                        fast_native_dev=pair.device;
                        rt_cpu::Scope cpu(rt_cpu::desc_update);
                        rt_cpu::Scope measured(rt_cpu::previous_update);
                        measured.add();
                        if(measured.kind>=0)g_write_fast_hits.fetch_add(1,std::memory_order_relaxed);
                        if(SUCCEEDED(native(call))){
                            fast_ok=true;
                            try{
                                ContentId content;
                                {
                                    rt_cpu::Scope capture(rt_cpu::previous_r32);
                                    content=CaptureNativeView(dev,*packet,pair);
                                    if(content)written=content->definition.info;
                                }
                                std::unique_lock lock(cached->page(r.slot).mutex,std::defer_lock);
                                PreviousLock(lock);
                                if(PreviousCurrent(pair,*cached)){
                                    rt_cpu::Scope apply(rt_cpu::previous_aux);
                                    PublishContentExclusive(*cached,r.slot,cached->write_version(),std::move(content),3);
                                }
                            }catch(...){
                                std::unique_lock lock(cached->page(r.slot).mutex);
                                if(PreviousCurrent(pair,*cached))PublishUnknownSlotExclusive(*cached,r.slot,3);
                                fast_ok=false;
                            }
                        }
                    }
                }
            }
        }
        if(handled_fast){
            if(!fast_ok)return;
            try{NotePublishedView(*packet);}catch(...){}
            try{
                if(!g_addon_disabled&&packet->kind==WWMI_DESCRIPTOR_SRV&&packet->resource){
                    if(written.tex_cand&&
                        !GlobalTexSwap(reinterpret_cast<ID3D12Device*>(fast_native_dev),r.heap,r.slot,written.res)&&
                        g_texnoswap_arm.load(std::memory_order_relaxed)>0)
                        TexNoSwapNote(r.heap,r.slot,written.res,"write");
                }
            }catch(...){}
            return;
        }
    }

    AppDescriptorAdmission admission;
    admission.pages.add(r.cpu,1,0);
    admission.enter();
    if(packet->kind==WWMI_DESCRIPTOR_RELEASE_CPU_VIEW){
        try{raw_cpu_views.erase(r.cpu);}catch(...){}
        native(call);return;
    }
    if(!r.heap&&packet->kind==WWMI_DESCRIPTOR_UNKNOWN){
        // A physical-only registered private table has an increment but
        // never an allocator CPU view. For an unregistered address, forget
        // any CPU alias overwritten by this real write.
        if(SUCCEEDED(native(call))&&!r.increment)try{
            raw_cpu_views.erase(r.cpu);
        }catch(...){}
        return;
    }
    PreviousPair pair;
    if(!PreviousDevice(dev,pair,false)){native(call);return;}
    rt_cpu::Scope cpu(rt_cpu::desc_update);
    rt_cpu::Scope measured(rt_cpu::previous_update);
    measured.add();
    if(measured.kind>=0)g_write_generic_hits.fetch_add(1,std::memory_order_relaxed);
    const uint64_t retire_gen_before=previous_retire_generation.load(std::memory_order_acquire);
    PreviousHeapPtr heap;
    try{heap=PreviousGetRange(r,pair,true);}catch(...){}
    if(heap&&r.heap&&r.heap_creation&&CurrentCopyLane().copy_depth==0&&
       previous_retire_generation.load(std::memory_order_acquire)==retire_gen_before){
        auto& fast_cache=ThreadFastHeapCache();
        fast_cache.sync(retire_gen_before);
        fast_cache.put(r.heap,r.heap_creation,pair.epoch,heap.get());
    }
    if(FAILED(native(call)))return;
    if(r.heap&&!heap)return; // failed allocation on a never-observed heap
    ResInfo written{};
    try{
        ContentId content;
        {
            rt_cpu::Scope capture(rt_cpu::previous_r32);
            content=CaptureNativeView(dev,*packet,pair);
            if(content)written=content->definition.info;
        }
        if(!r.heap)raw_cpu_views.put(r.cpu,{std::move(content),pair.device});
        else{
            std::unique_lock lock(heap->page(r.slot).mutex,std::defer_lock);PreviousLock(lock);
            if(!PreviousCurrent(pair,*heap))return;
            rt_cpu::Scope apply(rt_cpu::previous_aux);
            PublishContentExclusive(*heap,r.slot,heap->write_version(),std::move(content),3);
        }
    }catch(...){
        if(!r.heap)raw_cpu_views.erase(r.cpu);
        else if(heap){
            std::unique_lock lock(heap->page(r.slot).mutex);
            if(PreviousCurrent(pair,*heap))PublishUnknownSlotExclusive(*heap,r.slot,3);
        }
        return;
    }

    try{NotePublishedView(*packet);}catch(...){}
    admission.release(); // GlobalTexSwap is a new raw write to this same slot.
    try{
        if(r.heap&&!g_addon_disabled&&packet->kind==WWMI_DESCRIPTOR_SRV&&packet->resource){
            if(written.tex_cand&&
                !GlobalTexSwap(reinterpret_cast<ID3D12Device*>(pair.device),r.heap,r.slot,written.res)&&
                g_texnoswap_arm.load(std::memory_order_relaxed)>0)
                TexNoSwapNote(r.heap,r.slot,written.res,"write");
        }
    }catch(...){} // the native write and its origin are already published
}

WWMI_DescriptorPublish_ControlFn_v1 descriptor_control=nullptr;
int descriptor_owner;
}
bool InstallDescriptorPublisher(HMODULE host) {
    const auto control=host?reinterpret_cast<WWMI_DescriptorPublish_ControlFn_v1>(
        GetProcAddress(host,"WWMI_DescriptorPublish_Control_v1")):nullptr;
    if(!control)return false;
    const auto owner=reinterpret_cast<uint64_t>(&descriptor_owner);
    const WWMI_DescriptorPublisher_v1 sink{sizeof(sink),WWMI_DESCRIPTOR_CONTENT_VERSION,owner,OnNativeDescriptorCopy,OnNativeDescriptorWrite};
    WWMI_DescriptorRawAfter_v1 after=nullptr;
    if(control(owner,&sink,&after)!=0)return false;
    descriptor_raw_after.store(after,std::memory_order_release);
    descriptor_control=control;
    g_heap_to_res.retire_hook.store(PreviousHeapRetired,std::memory_order_release);
    return true;
}
void StopDescriptorPublisher() {
    if(descriptor_control){
        CloseRawDescriptorAdmission(); // host still admits app calls until existing raw edits finish
        descriptor_control(reinterpret_cast<uint64_t>(&descriptor_owner),nullptr,nullptr);
        descriptor_control=nullptr;
    }
    ReclaimRetiredContentsBlocking();
}
