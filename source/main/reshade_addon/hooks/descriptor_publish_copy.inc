// Uses the surrounding descriptor authority namespace.
enum class PageLockMode { SharedRead, DisjointCopyWrite, Exclusive };

template<PageLockMode Mode> class CopyPageSet {
    PreviousPage* first_=nullptr;
    uint32_t held_=0;
public:
    CopyPageSet()=default;
    CopyPageSet(const CopyPageSet&)=delete;
    void release() noexcept {
        while(held_){
            auto& m=first_[--held_].mutex;
            if constexpr(Mode==PageLockMode::SharedRead)m.unlock_shared();
            else if constexpr(Mode==PageLockMode::DisjointCopyWrite)m.unlock_copy_write();
            else m.unlock();
        }
        first_=nullptr;
    }
    ~CopyPageSet(){release();}
    void enter(PreviousHeap& h,uint32_t first,uint32_t count,CopyWaits* measured,CopyWaitPart part,bool timed=true) {
        auto* start=&h.page(first);
        const auto n=PreviousPageIndex(first+count-1)-PreviousPageIndex(first)+1;
        if(n==1&&held_==1&&first_==start)return;
        release();
        first_=start;
        for(;held_<n;++held_){
            auto& m=first_[held_].mutex;
            if constexpr(Mode==PageLockMode::DisjointCopyWrite){
                (void)measured;(void)part;(void)timed;
                m.lock_copy_write();
            }else if constexpr(Mode==PageLockMode::SharedRead){
                std::shared_lock<FastPageMutex> lock(m,std::defer_lock);
                if(timed)PreviousLock(lock,measured,part);else lock.lock();
                lock.release();
            }else{
                std::unique_lock<FastPageMutex> lock(m,std::defer_lock);
                if(timed)PreviousLock(lock,measured,part);else lock.lock();
                lock.release();
            }
        }
    }
    void end_span() noexcept {if(held_>1)release();}
    bool may_stamp() const noexcept requires (Mode==PageLockMode::SharedRead) {
        for(uint32_t i=0;i<held_;++i)if(first_[i].may_stamp.load(std::memory_order_relaxed))return true;
        return false;
    }
};

struct CopyTicket {
    explicit CopyTicket(const WWMI_DescriptorSpan_v1& input) : span(input) {}
    WWMI_DescriptorSpan_v1 span;
    PreviousHeap* destination=nullptr;
    PreviousHeap* source=nullptr;
    size_t begin=0;
    bool raw_destination=false,published=false,may_stamp=false,has_swap=false;
};
struct CopyHeapPin {
    uint64_t handle=0,host_creation=0;
    PreviousHeapPtr heap;
};
struct CopyScratch {
    bool busy=false;
    std::vector<CopyTicket> tickets;
    std::vector<CopyHeapPin> heaps;
    std::vector<ContentId> cells;
    uint32_t native_increment=0; // from the current callback's device only
    void clear() {cells.clear();tickets.clear();heaps.clear();native_increment=0;busy=false;}
};
struct CopyUse {
    CopyScratch& work;
    ~CopyUse(){work.clear();}
};
struct CopyIngress {
    DescriptorPages& pages;
    CopyScratch& work;
    device& api;
    uint64_t slots=0;
    bool complete=true;
    bool collect_pages=true;
};
struct FastHeapCache {
    struct Entry {
        uint64_t handle=0,host_gen=0,epoch=0,stamp=0;
        PreviousHeap* heap=nullptr;
    };
    uint64_t retire_gen=0;
    uint64_t stamp=1;
    std::array<Entry,512> entries{};
    void sync(uint64_t cur_retire_gen) noexcept {
        if(retire_gen!=cur_retire_gen){
            retire_gen=cur_retire_gen;
            ++stamp;
        }
    }
    PreviousHeap* find(uint64_t handle,uint64_t host_gen,uint64_t epoch) const noexcept {
        const auto& e=entries[PreviousHandleHash{}(handle)&(entries.size()-1)];
        return (e.stamp==stamp&&e.handle==handle&&e.host_gen==host_gen&&e.epoch==epoch)?e.heap:nullptr;
    }
    void put(uint64_t handle,uint64_t host_gen,uint64_t epoch,PreviousHeap* heap) noexcept {
        entries[PreviousHandleHash{}(handle)&(entries.size()-1)]={handle,host_gen,epoch,stamp,heap};
    }
};
inline FastHeapCache& ThreadFastHeapCache() noexcept {
    static thread_local FastHeapCache cache;
    return cache;
}

uint32_t RawCopyIncrement(const WWMI_DescriptorSpan_v1& span,uint32_t native_increment) noexcept {
    if(span.destination.increment)return span.destination.increment;
    if(span.source.increment)return span.source.increment;
    return native_increment;
}
void DescribeCopySpan(void* context,const WWMI_DescriptorSpan_v1* span) noexcept {
    auto& ingress=*static_cast<CopyIngress*>(context);
    const auto& d=span->destination;
    const auto& s=span->source;
    if(!d.increment&&!s.increment&&span->count>1&&(d.cpu||s.cpu)){
        auto& inc=ingress.work.native_increment;
        if(!inc)if(auto* dev=reinterpret_cast<ID3D12Device*>(ingress.api.get_native()))
            inc=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if(ingress.collect_pages){
            ingress.pages.add(d.cpu,span->count,inc);
            ingress.pages.add(s.cpu,span->count,inc);
        }
    }else if(ingress.collect_pages)AddAppDescriptorSpan(&ingress.pages,span);
    // Native/raw admission above never depends on shadow allocation.
    ingress.slots+=span->count;
    const bool raw=!d.heap&&d.cpu&&!d.increment; // never the private GPU table
    if(!ingress.complete||!span->count||(!raw&&(!d.heap||d.slot>=d.slots||span->count>d.slots-d.slot)))return;
    try{ingress.work.tickets.emplace_back(*span);}catch(...){ingress.complete=false;}
}
inline bool CopyRangeMatchesHeap(const PreviousHeap* h,const WWMI_DescriptorRange_v1& range) noexcept {
    return h&&h->flat.size==range.slots&&h->host_index.load(std::memory_order_relaxed)==range.heap_index&&
        range.cpu>=h->flat.cpu&&range.cpu-h->flat.cpu==uint64_t(range.slot)*range.increment;
}
PreviousHeap* PinCopyHeap(CopyScratch& w,const WWMI_DescriptorRange_v1& range,
    PreviousPair pair,bool create,CopyWaits* measured,const CopyLaneGuard& lane,FastHeapCache* fast_cache) {
    if(!range.heap||!range.heap_creation||!range.increment||range.slot>=range.slots)return nullptr;
    if(fast_cache){
        if(auto* cached=fast_cache->find(range.heap,range.heap_creation,pair.epoch)){
            if(!cached->retired.load(std::memory_order_acquire)&&cached->flat.live())
                return CopyRangeMatchesHeap(cached,range)?cached:nullptr;
        }
    }
    auto pin=std::find_if(w.heaps.begin(),w.heaps.end(),[&](const auto& item){
        return item.handle==range.heap&&item.host_creation==range.heap_creation;
    });
    if(pin==w.heaps.end()){
        auto h=PreviousGetRange(range,pair,create,measured);
        if(h&&fast_cache&&previous_retire_generation.load(std::memory_order_acquire)==lane.retire_gen)
            fast_cache->put(range.heap,range.heap_creation,pair.epoch,h.get());
        w.heaps.push_back({range.heap,range.heap_creation,std::move(h)});pin=std::prev(w.heaps.end());
    }
    auto* h=pin->heap.get();
    return CopyRangeMatchesHeap(h,range)?h:nullptr;
}
PreviousHeap* PinCpuSourceHeap(CopyScratch& w,HeapKey key,PreviousPair pair,CopyWaits* measured) {
    const auto existing=std::find_if(w.heaps.begin(),w.heaps.end(),[&](const auto& pin){return pin.handle==key.heap&&pin.heap;});
    if(existing!=w.heaps.end())return existing->heap.get();
    auto h=PreviousGetHeap(PreviousToken(key.heap,g_heap_to_res.entry_of(key.heap)),pair,0,false,0,measured);
    const auto host=h?h->host_generation.load(std::memory_order_acquire):0;
    w.heaps.push_back({key.heap,host,std::move(h)});return w.heaps.back().heap.get();
}
void ResolveCopyTickets(CopyScratch& w,PreviousPair pair,CopyWaits* measured,const CopyLaneGuard& lane,FastHeapCache* fast_cache) {
    size_t cells=0;
    uint64_t last_dst_handle=0,last_dst_gen=0;
    PreviousHeap* last_dst_heap=nullptr;
    for(auto& t:w.tickets){
        const auto& span=t.span;
        const auto& dr=span.destination;
        if(last_dst_heap&&dr.heap==last_dst_handle&&dr.heap_creation==last_dst_gen&&
           dr.increment&&dr.slot<dr.slots&&CopyRangeMatchesHeap(last_dst_heap,dr)){
            t.destination=last_dst_heap;
        }else{
            t.destination=PinCopyHeap(w,dr,pair,true,measured,lane,fast_cache);
            if(t.destination){last_dst_handle=dr.heap;last_dst_gen=dr.heap_creation;last_dst_heap=t.destination;}
        }
        t.raw_destination=!dr.heap&&dr.cpu&&!dr.increment;
        if(!t.destination&&!t.raw_destination)continue;
        t.begin=cells;
        cells+=span.count;
    }
    w.cells.resize(cells);
    uint64_t last_src_handle=last_dst_handle,last_src_gen=last_dst_gen;
    PreviousHeap* last_src_heap=last_dst_heap;
    for(auto& t:w.tickets){
        if(!t.destination&&!t.raw_destination)continue;
        const auto& span=t.span;
        const auto& sr=span.source;
        if(last_src_heap&&sr.heap==last_src_handle&&sr.heap_creation==last_src_gen&&
           sr.increment&&sr.slot<sr.slots&&CopyRangeMatchesHeap(last_src_heap,sr)){
            t.source=last_src_heap;
        }else{
            t.source=PinCopyHeap(w,sr,pair,false,measured,lane,fast_cache);
            if(t.source){last_src_handle=sr.heap;last_src_gen=sr.heap_creation;last_src_heap=t.source;}
        }
        if(!sr.heap&&sr.cpu&&!sr.increment){
            const auto increment=span.count>1?RawCopyIncrement(span,w.native_increment):0;
            bool allocator_view=false;
            for(uint32_t i=0;i<span.count;++i){
                RawCpuView raw;
                if(raw_cpu_views.find(sr.cpu+uint64_t(i)*increment,raw)&&raw.device==pair.device){
                    if(t.destination&&raw.content&&raw.content->definition.replacement)t.may_stamp=true;
                    allocator_view=true;w.cells[t.begin+i]=std::move(raw.content);
                }
            }
            if(!allocator_view){
                HeapKey key{};
                if(TrackedSlotOfCpu(sr.cpu,&key)){
                    t.span.source.slot=key.offset;
                    t.source=PinCpuSourceHeap(w,key,pair,measured);
                }
            }
        }
    }
}
bool TryFreezeSpanLockFree(PreviousHeap& s,PreviousPair pair,uint32_t first,uint32_t count,
    ContentId* dst_cells,bool want_stamp,bool& out_may_stamp) noexcept {
    if(!PreviousCurrent(pair,s))return false;
    const uint32_t p0=PreviousPageIndex(first);
    const uint32_t p1=PreviousPageIndex(first+count-1);
    if(p1-p0>=4)return false;
    uint64_t seqs[4];
    bool stamp=false;
    for(uint32_t p=p0;p<=p1;++p){
        seqs[p-p0]=s.pages[p].seq.load(std::memory_order_acquire);
        if(s.pages[p].mutex.has_active_writer())return false;
        if(want_stamp&&s.pages[p].may_stamp.load(std::memory_order_relaxed))stamp=true;
    }
    uint32_t retained=0;
    for(uint32_t i=0;i<count;++i){
        const auto* raw=s.slots[first+i].content.load_raw();
        if(raw){
            if(!TryRetainDescriptor(raw)){
                for(uint32_t j=0;j<retained;++j)dst_cells[j]={};
                return false;
            }
            dst_cells[i].init_adopted(raw);
        }
        ++retained;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    for(uint32_t p=p0;p<=p1;++p){
        if(s.pages[p].mutex.has_active_writer()||
           s.pages[p].seq.load(std::memory_order_acquire)!=seqs[p-p0]){
            for(uint32_t j=0;j<retained;++j)dst_cells[j]={};
            return false;
        }
    }
    if(!PreviousCurrent(pair,s)){
        for(uint32_t j=0;j<retained;++j)dst_cells[j]={};
        return false;
    }
    if(stamp)out_may_stamp=true;
    return true;
}
void FreezeCopySources(CopyScratch& w,PreviousPair pair,CopyWaits* measured,const CopyLaneGuard& lane) {
    CopyPageSet<PageLockMode::SharedRead> lock;
    const bool allow_seqlock=lane.has_copy_depth();
    for(auto& t:w.tickets){
        const auto first=t.span.source.slot,count=t.span.count;
        auto* s=t.source;
        if(!t.destination&&!t.raw_destination)continue;
        if(s&&first<s->flat.size&&count<=s->flat.size-first){
            if(allow_seqlock&&TryFreezeSpanLockFree(*s,pair,first,count,w.cells.data()+t.begin,
                t.destination!=nullptr,t.may_stamp))
                continue;
            lock.enter(*s,first,count,measured,copy_source);
            if(PreviousCurrent(pair,*s)){
                if(t.destination&&lock.may_stamp())t.may_stamp=true;
                for(uint32_t i=0;i<count;++i)w.cells[t.begin+i]=s->slots[first+i].content;
            }
            lock.end_span();
        }
    }
}
void ForgetCopySpan(PreviousHeap& h,const WWMI_DescriptorSpan_v1& span) noexcept {
    const auto version=h.write_version();
    for(uint32_t i=0;i<span.count;++i)PublishContent(h,span.destination.slot+i,version,{},2);
}
void PublishCopySpan(CopyScratch& w,CopyTicket& t,PreviousHeap& h,uint64_t version) noexcept {
    const uint32_t first=t.span.destination.slot;
    const uint32_t count=t.span.count;
    const uint32_t p0=PreviousPageIndex(first);
    const uint32_t p1=PreviousPageIndex(first+count-1);
    if(!t.may_stamp){
        auto* target=h.slots.get()+first;
        auto* frozen=w.cells.data()+t.begin;
        for(uint32_t i=0;i<count;++i){
            if(target[i].content.load_relaxed()!=frozen[i].load_relaxed()){
                target[i].content.swap(frozen[i]);
            }
            StampDescriptorWrite(target[i],version,2);
        }
        for(uint32_t p=p0;p<=p1;++p){
            h.pages[p].seq.fetch_add(1,std::memory_order_release);
        }
        return;
    }
    for(uint32_t i=0;i<count;++i){
        auto& content=w.cells[t.begin+i];
        if(content&&content->definition.replacement){
            t.has_swap=true; // keep only a stamp for post-lock texture accounting
            PublishContent(h,first+i,version,content,2);
        }else PublishContent(h,first+i,version,std::move(content),2);
    }
}
void CommitCopyTickets(CopyScratch& w,PreviousPair pair,CopyWaits* measured) {
    auto next=w.tickets.begin();
    const auto end=w.tickets.end();
    while(next!=end){
        if(!next->destination){++next;continue;}
        auto& d=*next->destination;
        auto stop=next+1;
        uint64_t count=1;
        for(;stop!=end;++stop){
            const auto* heap=stop->destination;
            if(heap&&heap!=&d)break;
            if(heap)++count;
        }
        auto version=d.reserve_write_versions(count);
        CopyPageSet<PageLockMode::DisjointCopyWrite> lock;
        for(;next!=stop;++next){
            auto& t=*next;
            if(!t.destination)continue;
            const auto span_version=version++;
            lock.enter(d,t.span.destination.slot,t.span.count,measured,copy_destination);
            if(PreviousCurrent(pair,d)){
                PublishCopySpan(w,t,d,span_version);
                t.published=true;
            }
            lock.end_span();
        }
    }
    for(const auto& t:w.tickets)if(t.raw_destination&&previous_epoch.load(std::memory_order_acquire)==pair.epoch){
        const auto increment=t.span.count>1?RawCopyIncrement(t.span,w.native_increment):0;
        for(uint32_t i=0;i<t.span.count;++i)
            raw_cpu_views.with(t.span.destination.cpu+uint64_t(i)*increment,[&](RawCpuView& raw){
                if(raw.device==pair.device)raw.content=w.cells[t.begin+i];
            });
    }
    const bool probe=UavObserve::g_probe.enabled.load();
    if(!g_addon_disabled||probe)try{
        for(const auto& t:w.tickets)if(t.published&&(probe||t.has_swap)){
            const auto& d=t.span.destination;
            const auto& s=t.span.source;
            for(uint32_t i=0;i<t.span.count;++i){
                if(probe)UavObserve::g_probe.copy({d.heap,d.slot+i},{s.heap,s.slot+i},
                    s.cpu+uint64_t(i)*d.increment,s.heap!=0);
                const auto& cell=w.cells[t.begin+i];
                if(t.has_swap&&cell&&cell->definition.replacement&&!g_addon_disabled)
                    ModLoader::TexTouchRes(cell->definition.replacement);
            }
        }
    }catch(...){}
}
struct InvalidateCopy { PreviousPair pair; CopyWaits* measured; uint32_t native_increment; };
void InvalidateCopiedSpan(void* context,const WWMI_DescriptorSpan_v1* span) {
    const auto& c=*static_cast<const InvalidateCopy*>(context);
    if(!span->destination.heap&&span->destination.cpu&&!span->destination.increment&&
        previous_epoch.load(std::memory_order_acquire)==c.pair.epoch){
        const auto increment=span->count>1?RawCopyIncrement(*span,c.native_increment):0;
        for(uint32_t i=0;i<span->count;++i)
            raw_cpu_views.with(span->destination.cpu+uint64_t(i)*increment,[&](RawCpuView& raw){
                if(raw.device==c.pair.device)raw.content={};
            });
        return;
    }
    auto h=PreviousGetRange(span->destination,c.pair,false,c.measured);
    if(!h||!span->count||span->count>h->flat.size-span->destination.slot)return;
    CopyPageSet<PageLockMode::Exclusive> lock;
    lock.enter(*h,span->destination.slot,span->count,nullptr,copy_destination,false);
    if(PreviousCurrent(c.pair,*h))ForgetCopySpan(*h,*span);
}
void OnNativeDescriptorCopy(uint64_t api,WWMI_DescriptorSpans_v1 enumerate,void* spans,
    WWMI_DescriptorNative_v1 native,void* call) noexcept {
    auto* dev=reinterpret_cast<device*>(api);
    if(!dev||dev!=g_reshade_device||!g_is_dx12)return; // other devices never receive our raw edits
    rt_cpu::Scope cpu(rt_cpu::desc_copy);
    rt_cpu::Scope measured(rt_cpu::previous_copy);
    rt_phase_cpu::Scope phase_copy(rt_phase_cpu::descriptor_copy,measured.start);
    CopyWaits breakdown(measured);
    auto* sampled=measured.kind>=0?&breakdown:nullptr;
    struct ReclaimOnOuterExit {
        bool outermost=false;
        ~ReclaimOnOuterExit(){if(outermost)TryReclaimRetiredContents();}
    } reclaim_on_exit;
    CopyLaneGuard lane_guard;
    reclaim_on_exit.outermost=lane_guard.is_outermost();
    FastHeapCache* fast_cache=nullptr;
    if(lane_guard.has_copy_depth()){
        fast_cache=&ThreadFastHeapCache();
        fast_cache->sync(lane_guard.retire_gen);
    }
    AppDescriptorAdmission admission;
    static thread_local CopyScratch reuse;
    std::optional<CopyScratch> nested;
    auto& w=reuse.busy?nested.emplace():reuse;
    w.busy=true;const CopyUse clear{w};
    CopyIngress ingress{admission.pages,w,*dev,0,true,!lane_guard.has_fast_raw()};
    enumerate(spans,DescribeCopySpan,&ingress);
    if(!lane_guard.has_fast_raw()){
        uint64_t blocked=0;
        admission.enter([&]() noexcept {
            if(sampled&&!blocked)blocked=rt_cpu::now();
        });
        if(blocked)breakdown.elapsed(copy_raw_admission,blocked);
    }
    const auto forward=[&] {
        auto phase_native=rt_phase_cpu::begin(rt_phase_cpu::native_copy);
        const auto start=phase_native.start?phase_native.start:sampled?rt_cpu::now():0;
        const auto hr=native(call);
        const auto stop=sampled||phase_native.start?rt_cpu::now():0;
        if(sampled)breakdown.elapsed(copy_native,start,stop);
        rt_phase_cpu::end(phase_native,stop);
        return hr;
    };
    PreviousPair pair;
    if(!PreviousDevice(dev,pair,false)){forward();return;} // reset does not waive CPU admission
    measured.add(ingress.slots);
    phase_copy.add(ingress.slots);
    if(ingress.complete)try{
        ResolveCopyTickets(w,pair,sampled,lane_guard,fast_cache);
        FreezeCopySources(w,pair,sampled,lane_guard);
    }catch(...){ingress.complete=false;}
    if(FAILED(forward()))return;
    if(ingress.complete)try{
        CommitCopyTickets(w,pair,sampled);
        return;
    }catch(...){}
    InvalidateCopy invalidate{pair,sampled,w.native_increment};
    try{enumerate(spans,InvalidateCopiedSpan,&invalidate);}catch(...){}
}
