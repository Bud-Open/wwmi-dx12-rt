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
    struct Set {
        Entry ways[2]{};
        uint8_t next_victim=0;
    };
    uint64_t retire_gen=0;
    uint64_t stamp=1;
    std::array<Set,256> sets{};
    void sync(uint64_t cur_retire_gen) noexcept {
        if(retire_gen!=cur_retire_gen){
            retire_gen=cur_retire_gen;
            ++stamp;
        }
    }
    PreviousHeap* find(uint64_t handle,uint64_t host_gen,uint64_t epoch) const noexcept {
        const auto& s=sets[PreviousHandleHash{}(handle)&(sets.size()-1)];
        for(const auto& e:s.ways)
            if(e.stamp==stamp&&e.handle==handle&&e.host_gen==host_gen&&e.epoch==epoch)return e.heap;
        return nullptr;
    }
    PreviousHeap* find_any_gen(uint64_t handle,uint64_t epoch) const noexcept {
        const auto& s=sets[PreviousHandleHash{}(handle)&(sets.size()-1)];
        for(const auto& e:s.ways)
            if(e.stamp==stamp&&e.handle==handle&&e.epoch==epoch)return e.heap;
        return nullptr;
    }
    void put(uint64_t handle,uint64_t host_gen,uint64_t epoch,PreviousHeap* heap) noexcept {
        auto& s=sets[PreviousHandleHash{}(handle)&(sets.size()-1)];
        for(unsigned w=0;w<2;++w){
            if(s.ways[w].stamp==stamp&&s.ways[w].handle==handle){
                s.ways[w]={handle,host_gen,epoch,stamp,heap};
                s.next_victim=uint8_t(w^1u);
                return;
            }
        }
        for(unsigned w=0;w<2;++w){
            if(s.ways[w].stamp!=stamp){
                s.ways[w]={handle,host_gen,epoch,stamp,heap};
                s.next_victim=uint8_t(w^1u);
                return;
            }
        }
        const unsigned victim=s.next_victim&1u;
        s.ways[victim]={handle,host_gen,epoch,stamp,heap};
        s.next_victim=uint8_t(victim^1u);
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
PreviousHeap* PinCpuSourceHeap(CopyScratch& w,HeapKey key,FlatHeapMap::Entry* entry,
    PreviousPair pair,CopyWaits* measured,const CopyLaneGuard& lane,FastHeapCache* fast_cache) {
    if(!entry)return nullptr;
    if(fast_cache){
        if(auto* cached=fast_cache->find_any_gen(key.heap,pair.epoch)){
            if(!cached->retired.load(std::memory_order_acquire)&&cached->flat.entry==entry&&cached->flat.live())
                return cached;
        }
    }
    const auto existing=std::find_if(w.heaps.begin(),w.heaps.end(),[&](const auto& pin){return pin.handle==key.heap&&pin.heap;});
    if(existing!=w.heaps.end())return existing->heap.get();
    auto h=PreviousGetHeap(PreviousToken(key.heap,entry),pair,0,false,0,measured);
    if(!h)return nullptr;
    const auto host=h->host_generation.load(std::memory_order_acquire);
    if(fast_cache&&previous_retire_generation.load(std::memory_order_acquire)==lane.retire_gen)
        fast_cache->put(key.heap,host,pair.epoch,h.get());
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
            if(measured)measured->add(copy_raw_src,span.count);
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
                const uint32_t cpu_inc=w.native_increment?w.native_increment:CurrentDescriptorIncrement();
                HeapKey key{};
                if(auto* entry=g_heap_to_res.find_cpu_entry(sr.cpu,cpu_inc,&key)){
                    t.span.source.slot=key.offset;
                    t.source=PinCpuSourceHeap(w,key,entry,pair,measured,lane,fast_cache);
                }
            }
        }
    }
}
bool TryFreezeSpanLockFree(PreviousHeap& s,PreviousPair pair,uint32_t first,uint32_t count,
    ContentId* dst_cells,bool want_stamp,bool& out_may_stamp,CopyWaits* measured=nullptr) noexcept {
    if(s.is_vis||!PreviousCurrent(pair,s))return false;
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
    uint32_t frozen_slots=0;
    for(uint32_t i=0;i<count;++i){
        const auto* raw=s.slots[first+i].content.load_raw();
        if(raw){
            if(!TryRetainDescriptor(raw)){
                for(uint32_t j=0;j<retained;++j)dst_cells[j]={};
                return false;
            }
            ++frozen_slots;
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
    if(measured&&frozen_slots)measured->add(copy_frozen_unpinned,frozen_slots);
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
                t.destination!=nullptr,t.may_stamp,measured))
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
        const uint64_t new_w=((version<<2)|2u)&~kSlotUpdatingBit;
        for(uint32_t p=p0;p<=p1;++p)h.pages[p].lock_vis_write();
        for(uint32_t i=0;i<count;++i){
            target[i].write.store(new_w|kSlotUpdatingBit,std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            if(target[i].content.load_relaxed()!=frozen[i].load_relaxed()){
                target[i].content.swap(frozen[i]);
            }
            target[i].frame.store(g_frame_id,std::memory_order_relaxed);
            target[i].miss.store(0,std::memory_order_relaxed);
            target[i].write.store(new_w,std::memory_order_release);
        }
        if(!h.is_vis){
            for(uint32_t p=p0;p<=p1;++p){
                const uint64_t s=h.pages[p].seq.load(std::memory_order_relaxed);
                h.pages[p].seq.store(s+1u,std::memory_order_release);
            }
        }
        for(uint32_t p=p1+1u;p>p0;--p)h.pages[p-1u].unlock_vis_write();
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
__forceinline bool TryRetainCached(const DescriptorContent* ptr,LaneRefCache* cache) noexcept {
    return cache?cache->try_retain(ptr):TryRetainDescriptor(ptr);
}
__forceinline void ReleaseDescriptorCached(const DescriptorContent* ptr,LaneRefCache* cache) noexcept {
    if(!ptr)return;
    if(cache)cache->release(ptr);else ReleaseDescriptor(ptr);
}
__forceinline void ReleaseUnpinnedMask(const DescriptorContent* const* ptrs,uint64_t mask,LaneRefCache* cache=nullptr) noexcept {
    while(mask){
        const unsigned idx=std::countr_zero(mask);
        mask&=mask-1ull;
        ReleaseDescriptorCached(ptrs[idx],cache);
    }
}
bool TryFreezeSpanRawLockFree(PreviousHeap& s,PreviousPair pair,uint32_t first,uint32_t count,
    const DescriptorContent** dst_ptrs,uint64_t& out_unpinned_mask,CopyWaits* measured=nullptr,
    LaneRefCache* ref_cache=nullptr) noexcept {
    out_unpinned_mask=0;
    if(s.is_vis||!PreviousCurrent(pair,s))return false;
    const uint32_t p0=PreviousPageIndex(first);
    const uint32_t p1=PreviousPageIndex(first+count-1);
    if(p1-p0>=4)return false;
    uint64_t seqs[4];
    for(uint32_t p=p0;p<=p1;++p){
        seqs[p-p0]=s.pages[p].seq.load(std::memory_order_acquire);
        if(s.pages[p].mutex.has_active_writer())return false;
        if(s.pages[p].may_stamp.load(std::memory_order_relaxed))return false;
    }
    uint64_t retained_mask=0;
    uint32_t frozen_slots=0;
    for(uint32_t i=0;i<count;++i){
        const auto* raw=s.slots[first+i].content.load_raw();
        dst_ptrs[i]=raw;
        if(raw){
            if(!TryRetainCached(raw,ref_cache)){
                ReleaseUnpinnedMask(dst_ptrs,retained_mask,ref_cache);
                return false;
            }
            retained_mask|=(1ull<<i);
            ++frozen_slots;
        }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    for(uint32_t p=p0;p<=p1;++p){
        if(s.pages[p].mutex.has_active_writer()||
           s.pages[p].seq.load(std::memory_order_acquire)!=seqs[p-p0]||
           s.pages[p].may_stamp.load(std::memory_order_relaxed)){
            ReleaseUnpinnedMask(dst_ptrs,retained_mask,ref_cache);
            return false;
        }
    }
    if(!PreviousCurrent(pair,s)){
        ReleaseUnpinnedMask(dst_ptrs,retained_mask,ref_cache);
        return false;
    }
    if(measured&&frozen_slots)measured->add(copy_frozen_unpinned,frozen_slots);
    out_unpinned_mask=retained_mask;
    return true;
}
constexpr uint32_t kFastCopyMaxSpans=32;
constexpr uint32_t kFastCopyMaxSlots=64;
constexpr uint32_t kFastCopyMaxPages=32;
enum FastCopySpillReason : uint8_t {
    fast_spill_none=0,
    fast_spill_spans=1,
    fast_spill_slots=2,
    fast_spill_raw=3,
    fast_spill_miss=4,
    fast_spill_freeze=5,
    fast_spill_dup_dst=6,
    fast_spill_pages=7
};
inline void RecordFastCopySpill(FastCopySpillReason reason) noexcept {
    switch(reason){
    case fast_spill_spans:  g_copy_spill_spans.fetch_add(1,std::memory_order_relaxed);break;
    case fast_spill_slots:  g_copy_spill_slots.fetch_add(1,std::memory_order_relaxed);break;
    case fast_spill_raw:    g_copy_spill_raw.fetch_add(1,std::memory_order_relaxed);break;
    case fast_spill_miss:   g_copy_spill_miss.fetch_add(1,std::memory_order_relaxed);break;
    case fast_spill_freeze: g_copy_spill_freeze.fetch_add(1,std::memory_order_relaxed);break;
    case fast_spill_dup_dst:g_copy_spill_dup_dst.fetch_add(1,std::memory_order_relaxed);break;
    case fast_spill_pages:  g_copy_spill_pages.fetch_add(1,std::memory_order_relaxed);break;
    default:break;
    }
}
bool TryFreezeMultiSpanRawLockFree(const WWMI_DescriptorSpan_v1* items,PreviousHeap* const* src_heaps,
    const uint32_t* span_offsets,uint32_t span_count,PreviousPair pair,bool single_src_heap,
    const DescriptorContent** dst_ptrs,uint64_t& out_unpinned_mask,CopyWaits* measured=nullptr,
    LaneRefCache* ref_cache=nullptr) noexcept {
    out_unpinned_mask=0;
    if(single_src_heap){
        PreviousHeap* const h=src_heaps[0];
        if(!h||h->is_vis||!PreviousCurrent(pair,*h))return false;
        const uint32_t heap_size=h->flat.size;
        uint32_t uniq_pages[kFastCopyMaxPages];
        uint64_t saved_seqs[kFastCopyMaxPages];
        uint32_t uniq_page_count=0;
        uint32_t last_page=UINT32_MAX;
        for(uint32_t s=0;s<span_count;++s){
            const uint32_t first=items[s].source.slot;
            const uint32_t count=items[s].count;
            if(!count||first>=heap_size||count>heap_size-first)return false;
            const uint32_t p0=PreviousPageIndex(first);
            const uint32_t p1=(count==1u)?p0:PreviousPageIndex(first+count-1u);
            if(p0==last_page&&p1==last_page)continue;
            for(uint32_t p=p0;p<=p1;++p){
                if(p==last_page)continue;
                last_page=p;
                if(uniq_page_count==1u&&uniq_pages[0]==p)continue;
                bool page_seen=false;
                for(uint32_t k=0;k<uniq_page_count;++k){
                    if(uniq_pages[k]==p){page_seen=true;break;}
                }
                if(!page_seen){
                    if(uniq_page_count>=kFastCopyMaxPages)return false;
                    uniq_pages[uniq_page_count++]=p;
                }
            }
        }
        const PreviousPage* const pages=h->pages.get();
        if(uniq_page_count==1){
            const auto& pg=pages[uniq_pages[0]];
            saved_seqs[0]=pg.seq.load(std::memory_order_acquire);
            if(pg.mutex.has_active_writer()||pg.may_stamp.load(std::memory_order_relaxed))return false;
        }else{
            for(uint32_t k=0;k<uniq_page_count;++k){
                const auto& pg=pages[uniq_pages[k]];
                saved_seqs[k]=pg.seq.load(std::memory_order_acquire);
                if(pg.mutex.has_active_writer()||pg.may_stamp.load(std::memory_order_relaxed))return false;
            }
        }
        const DescriptorSlot* const h_slots=h->slots.get();
        uint64_t retained_mask=0;
        uint32_t frozen_slots=0;
        for(uint32_t s=0;s<span_count;++s){
            const uint32_t first=items[s].source.slot;
            const uint32_t count=items[s].count;
            const uint32_t base_pos=span_offsets[s];
            for(uint32_t i=0;i<count;++i){
                const uint32_t pos=base_pos+i;
                const auto* raw=h_slots[first+i].content.load_raw();
                dst_ptrs[pos]=raw;
                if(raw){
                    if(!TryRetainCached(raw,ref_cache)){
                        ReleaseUnpinnedMask(dst_ptrs,retained_mask,ref_cache);
                        return false;
                    }
                    retained_mask|=(1ull<<pos);
                    ++frozen_slots;
                }
            }
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if(uniq_page_count==1){
            const auto& pg=pages[uniq_pages[0]];
            if(pg.mutex.has_active_writer()||
               pg.seq.load(std::memory_order_acquire)!=saved_seqs[0]||
               pg.may_stamp.load(std::memory_order_relaxed)){
                ReleaseUnpinnedMask(dst_ptrs,retained_mask,ref_cache);
                return false;
            }
        }else{
            for(uint32_t k=0;k<uniq_page_count;++k){
                const auto& pg=pages[uniq_pages[k]];
                if(pg.mutex.has_active_writer()||
                   pg.seq.load(std::memory_order_acquire)!=saved_seqs[k]||
                   pg.may_stamp.load(std::memory_order_relaxed)){
                    ReleaseUnpinnedMask(dst_ptrs,retained_mask,ref_cache);
                    return false;
                }
            }
        }
        if(!PreviousCurrent(pair,*h)){
            ReleaseUnpinnedMask(dst_ptrs,retained_mask,ref_cache);
            return false;
        }
        if(measured&&frozen_slots)measured->add(copy_frozen_unpinned,frozen_slots);
        out_unpinned_mask=retained_mask;
        return true;
    }
    PreviousHeap* uniq_heaps[kFastCopyMaxSpans];
    uint32_t uniq_heap_count=0;
    struct SrcPage { PreviousHeap* heap; uint32_t page; uint64_t saved_seq; };
    SrcPage uniq_pages[kFastCopyMaxPages];
    uint32_t uniq_page_count=0;
    for(uint32_t s=0;s<span_count;++s){
        PreviousHeap* h=src_heaps[s];
        const uint32_t first=items[s].source.slot;
        const uint32_t count=items[s].count;
        if(!h||h->is_vis||!count||first>=h->flat.size||count>h->flat.size-first)return false;
        bool heap_seen=false;
        for(uint32_t k=0;k<uniq_heap_count;++k){
            if(uniq_heaps[k]==h){heap_seen=true;break;}
        }
        if(!heap_seen)uniq_heaps[uniq_heap_count++]=h;
        const uint32_t p0=PreviousPageIndex(first);
        const uint32_t p1=PreviousPageIndex(first+count-1u);
        for(uint32_t p=p0;p<=p1;++p){
            bool page_seen=false;
            for(uint32_t k=0;k<uniq_page_count;++k){
                if(uniq_pages[k].heap==h&&uniq_pages[k].page==p){page_seen=true;break;}
            }
            if(!page_seen){
                if(uniq_page_count>=kFastCopyMaxPages)return false;
                uniq_pages[uniq_page_count++]={h,p,0};
            }
        }
    }
    for(uint32_t k=0;k<uniq_heap_count;++k){
        if(!PreviousCurrent(pair,*uniq_heaps[k]))return false;
    }
    for(uint32_t k=0;k<uniq_page_count;++k){
        const auto& pg=uniq_pages[k].heap->pages[uniq_pages[k].page];
        uniq_pages[k].saved_seq=pg.seq.load(std::memory_order_acquire);
        if(pg.mutex.has_active_writer())return false;
        if(pg.may_stamp.load(std::memory_order_relaxed))return false;
    }
    uint64_t retained_mask=0;
    uint32_t frozen_slots=0;
    for(uint32_t s=0;s<span_count;++s){
        const PreviousHeap* h=src_heaps[s];
        const uint32_t first=items[s].source.slot;
        const uint32_t count=items[s].count;
        const uint32_t base_pos=span_offsets[s];
        for(uint32_t i=0;i<count;++i){
            const uint32_t pos=base_pos+i;
            const auto* raw=h->slots[first+i].content.load_raw();
            dst_ptrs[pos]=raw;
            if(raw){
                if(!TryRetainCached(raw,ref_cache)){
                    ReleaseUnpinnedMask(dst_ptrs,retained_mask,ref_cache);
                    return false;
                }
                retained_mask|=(1ull<<pos);
                ++frozen_slots;
            }
        }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    for(uint32_t k=0;k<uniq_page_count;++k){
        const auto& pg=uniq_pages[k].heap->pages[uniq_pages[k].page];
        if(pg.mutex.has_active_writer()||
           pg.seq.load(std::memory_order_acquire)!=uniq_pages[k].saved_seq||
           pg.may_stamp.load(std::memory_order_relaxed)){
            ReleaseUnpinnedMask(dst_ptrs,retained_mask,ref_cache);
            return false;
        }
    }
    for(uint32_t k=0;k<uniq_heap_count;++k){
        if(!PreviousCurrent(pair,*uniq_heaps[k])){
            ReleaseUnpinnedMask(dst_ptrs,retained_mask,ref_cache);
            return false;
        }
    }
    if(measured&&frozen_slots)measured->add(copy_frozen_unpinned,frozen_slots);
    out_unpinned_mask=retained_mask;
    return true;
}
struct FastSpanCollector {
    WWMI_DescriptorSpan_v1 items[kFastCopyMaxSpans];
    uint32_t spans=0;
    uint32_t slots=0;
    bool spilled=false;
    FastCopySpillReason spill_reason=fast_spill_none;
};

template<class Forward>
bool TryRunFastDescriptorCopy(const WWMI_DescriptorSpan_v1* items,uint32_t span_count,
    PreviousPair pair,FastHeapCache* fast_cache,LaneRefCache* ref_cache,
    CopyWaits* sampled,CopyWaits& breakdown,Forward&& forward,
    FastCopySpillReason& out_spill_reason) noexcept {
    if(span_count==1){
        const auto& sp=items[0];
        const uint32_t count=sp.count;
        const uint64_t t_resolve=sampled?rt_cpu::now():0;
        PreviousHeap* dst=fast_cache->find(sp.destination.heap,sp.destination.heap_creation,pair.epoch);
        if(dst&&(dst->retired.load(std::memory_order_acquire)||!dst->flat.live()||!CopyRangeMatchesHeap(dst,sp.destination)))
            dst=nullptr;
        PreviousHeap* src=nullptr;
        if(dst){
            if(sp.source.heap==sp.destination.heap&&sp.source.heap_creation==sp.destination.heap_creation)
                src=CopyRangeMatchesHeap(dst,sp.source)?dst:nullptr;
            else{
                src=fast_cache->find(sp.source.heap,sp.source.heap_creation,pair.epoch);
                if(src&&(src->retired.load(std::memory_order_acquire)||!src->flat.live()||!CopyRangeMatchesHeap(src,sp.source)))
                    src=nullptr;
            }
        }
        if(dst&&src){
            if(sampled)breakdown.elapsed(copy_phase_resolve,t_resolve);
            const uint64_t t_freeze=sampled?rt_cpu::now():0;
            const DescriptorContent* frozen_ptrs[kFastCopyMaxSlots];
            uint64_t unpinned_mask=0;
            if(TryFreezeSpanRawLockFree(*src,pair,sp.source.slot,count,frozen_ptrs,unpinned_mask,sampled,ref_cache)){
                if(sampled)breakdown.elapsed(copy_phase_freeze,t_freeze);
                if(FAILED(forward())){
                    if(unpinned_mask)ReleaseUnpinnedMask(frozen_ptrs,unpinned_mask,ref_cache);
                    return true;
                }
                const uint64_t t_commit=sampled?rt_cpu::now():0;
                const uint32_t dst_first=sp.destination.slot;
                const DescriptorContent* displaced[kFastCopyMaxSlots];
                uint32_t disp_count=0;
                bool published=false;
                if(dst->is_vis){
                    const uint32_t p0=PreviousPageIndex(dst_first);
                    const uint32_t p1=(count==1u)?p0:PreviousPageIndex(dst_first+count-1u);
                    for(uint32_t p=p0;p<=p1;++p)dst->pages[p].lock_vis_write();
                    if(PreviousCurrent(pair,*dst)){
                        const uint64_t version=dst->reserve_write_versions(1);
                        auto* target=dst->slots.get()+dst_first;
                        for(uint32_t i=0;i<count;++i){
                            bool changed=false;
                            const DescriptorContent* cur=nullptr;
                            WriteSlotUnderPageLock(target[i],frozen_ptrs[i],version,2,changed,cur);
                            if(changed){
                                unpinned_mask&=~(1ull<<i);
                                if(cur)displaced[disp_count++]=cur;
                            }
                        }
                        published=true;
                    }
                    for(uint32_t p=p1+1u;p>p0;--p)dst->pages[p-1u].unlock_vis_write();
                }else{
                    const uint32_t p0=PreviousPageIndex(dst_first);
                    const uint32_t p1=(count==1u)?p0:PreviousPageIndex(dst_first+count-1u);
                    const uint64_t version=dst->reserve_write_versions(1);
                    for(uint32_t p=p0;p<=p1;++p)dst->pages[p].mutex.lock_copy_write();
                    for(uint32_t p=p0;p<=p1;++p)dst->pages[p].lock_vis_write();
                    if(PreviousCurrent(pair,*dst)){
                        auto* target=dst->slots.get()+dst_first;
                        for(uint32_t i=0;i<count;++i){
                            bool changed=false;
                            const DescriptorContent* cur=nullptr;
                            WriteSlotUnderPageLock(target[i],frozen_ptrs[i],version,2,changed,cur);
                            if(changed){
                                unpinned_mask&=~(1ull<<i);
                                if(cur)displaced[disp_count++]=cur;
                            }
                        }
                        for(uint32_t p=p0;p<=p1;++p){
                            const uint64_t s=dst->pages[p].seq.load(std::memory_order_relaxed);
                            dst->pages[p].seq.store(s+1u,std::memory_order_release);
                        }
                        published=true;
                    }
                    for(uint32_t p=p1+1u;p>p0;--p)dst->pages[p-1u].unlock_vis_write();
                    for(uint32_t p=p1+1u;p>p0;--p)dst->pages[p-1u].mutex.unlock_copy_write();
                }
                if(unpinned_mask)ReleaseUnpinnedMask(frozen_ptrs,unpinned_mask,ref_cache);
                for(uint32_t k=0;k<disp_count;++k)ReleaseDescriptorCached(displaced[k],ref_cache);
                if(ref_cache)ref_cache->maybe_sweep();
                if(published&&UavObserve::g_probe.enabled.load(std::memory_order_relaxed)){
                    try{
                        for(uint32_t i=0;i<count;++i)
                            UavObserve::g_probe.copy({sp.destination.heap,dst_first+i},
                                {sp.source.heap,sp.source.slot+i},
                                sp.source.cpu+uint64_t(i)*sp.destination.increment,true);
                    }catch(...){}
                }
                if(sampled){
                    breakdown.elapsed(copy_phase_commit,t_commit);
                    g_copy_fast_hits.fetch_add(1,std::memory_order_relaxed);
                }
                return true;
            }
            out_spill_reason=fast_spill_freeze;
        }else{
            out_spill_reason=fast_spill_miss;
        }
        return false;
    }

    // Multi-span fast path (2..32 spans, total slots <= 64): coalesced
    // heap resolution, coalesced source seqlock validation, and either
    // lock-free shader-visible publication or coalesced non-vis page locks.
    const uint64_t t_resolve=sampled?rt_cpu::now():0;
    PreviousHeap* src_heaps[kFastCopyMaxSpans];
    uint32_t span_offsets[kFastCopyMaxSpans];
    uint32_t span_dst_heap_idx[kFastCopyMaxSpans];
    struct FastDstHeap {
        PreviousHeap* heap;
        uint32_t span_count;
        bool live;
        uint64_t next_version;
    };
    struct FastDstPage {
        PreviousHeap* heap;
        uint64_t generation;
        uint32_t page;
        uint32_t touched_mask;
        uint32_t heap_slot_idx;
    };
    FastDstHeap dst_uniq_heaps[kFastCopyMaxSpans];
    uint32_t dst_heap_count=0;
    FastDstPage dst_pages[kFastCopyMaxPages];
    uint32_t dst_page_count=0;
    bool all_dst_vis=true;

    auto add_dst_pages_for=[&](uint32_t idx) noexcept -> bool {
        const auto& sp2=items[idx];
        const uint32_t d_idx2=span_dst_heap_idx[idx];
        PreviousHeap* dh2=dst_uniq_heaps[d_idx2].heap;
        const uint32_t df=sp2.destination.slot;
        const uint32_t p0=PreviousPageIndex(df);
        const uint32_t p1=PreviousPageIndex(df+sp2.count-1u);
        for(uint32_t p=p0;p<=p1;++p){
            const uint32_t lo=(p==p0)?(df&(previous_page_slots-1u)):0u;
            const uint32_t hi=(p==p1)?((df+sp2.count-1u)&(previous_page_slots-1u)):(previous_page_slots-1u);
            const uint32_t width=hi-lo+1u;
            const uint32_t bits=(width>=32u)?0xFFFFFFFFu:(((1u<<width)-1u)<<lo);
            uint32_t found=dst_page_count;
            for(uint32_t k=0;k<dst_page_count;++k){
                if(dst_pages[k].heap==dh2&&dst_pages[k].page==p){found=k;break;}
            }
            if(found<dst_page_count){
                if(dst_pages[found].touched_mask&bits){
                    out_spill_reason=fast_spill_dup_dst;
                    return false;
                }
                dst_pages[found].touched_mask|=bits;
            }else{
                if(dst_page_count>=kFastCopyMaxPages){
                    out_spill_reason=fast_spill_pages;
                    return false;
                }
                dst_pages[dst_page_count++]={dh2,dh2->generation,p,bits,d_idx2};
            }
        }
        return true;
    };

    uint64_t last_dst_handle=0,last_dst_gen=0;
    PreviousHeap* last_dst_heap=nullptr;
    uint32_t last_dst_idx=0;
    uint64_t last_src_handle=0,last_src_gen=0;
    PreviousHeap* last_src_heap=nullptr;
    PreviousHeap* first_src_heap=nullptr;
    bool single_src_heap=true;

    bool resolve_ok=true;
    uint32_t cur_offset=0;
    for(uint32_t s=0;s<span_count;++s){
        const auto& sp=items[s];
        const auto& dr=sp.destination;
        const auto& sr=sp.source;
        span_offsets[s]=cur_offset;
        cur_offset+=sp.count;

        PreviousHeap* dh=nullptr;
        uint32_t d_idx=0;
        if(last_dst_heap&&dr.heap==last_dst_handle&&dr.heap_creation==last_dst_gen){
            if(CopyRangeMatchesHeap(last_dst_heap,dr)){
                dh=last_dst_heap;
                d_idx=last_dst_idx;
            }
        }else{
            dh=fast_cache->find(dr.heap,dr.heap_creation,pair.epoch);
            if(dh&&(dh->retired.load(std::memory_order_acquire)||!dh->flat.live()||!CopyRangeMatchesHeap(dh,dr)))
                dh=nullptr;
            if(dh){
                d_idx=dst_heap_count;
                for(uint32_t k=0;k<dst_heap_count;++k){
                    if(dst_uniq_heaps[k].heap==dh){d_idx=k;break;}
                }
                if(d_idx==dst_heap_count)
                    dst_uniq_heaps[dst_heap_count++]={dh,0,false,0};
                last_dst_handle=dr.heap;last_dst_gen=dr.heap_creation;
                last_dst_heap=dh;last_dst_idx=d_idx;
            }
        }
        if(!dh){out_spill_reason=fast_spill_miss;resolve_ok=false;break;}
        span_dst_heap_idx[s]=d_idx;
        ++dst_uniq_heaps[d_idx].span_count;

        if(!dh->is_vis){
            if(all_dst_vis){
                all_dst_vis=false;
                for(uint32_t prev=0;prev<s;++prev){
                    if(!add_dst_pages_for(prev)){resolve_ok=false;break;}
                }
            }
            if(resolve_ok&&!add_dst_pages_for(s))resolve_ok=false;
        }else if(!all_dst_vis){
            if(!add_dst_pages_for(s))resolve_ok=false;
        }
        if(!resolve_ok)break;

        PreviousHeap* sh=nullptr;
        if(last_src_heap&&sr.heap==last_src_handle&&sr.heap_creation==last_src_gen){
            if(CopyRangeMatchesHeap(last_src_heap,sr))sh=last_src_heap;
        }else if(sr.heap==dr.heap&&sr.heap_creation==dr.heap_creation){
            if(CopyRangeMatchesHeap(dh,sr)){
                sh=dh;
                last_src_handle=sr.heap;last_src_gen=sr.heap_creation;last_src_heap=sh;
            }
        }else{
            sh=fast_cache->find(sr.heap,sr.heap_creation,pair.epoch);
            if(sh&&(sh->retired.load(std::memory_order_acquire)||!sh->flat.live()||!CopyRangeMatchesHeap(sh,sr)))
                sh=nullptr;
            if(sh){
                last_src_handle=sr.heap;last_src_gen=sr.heap_creation;last_src_heap=sh;
            }
        }
        if(!sh){out_spill_reason=fast_spill_miss;resolve_ok=false;break;}
        src_heaps[s]=sh;
        if(!first_src_heap)first_src_heap=sh;
        else if(sh!=first_src_heap)single_src_heap=false;
    }
    if(!resolve_ok)return false;

    if(sampled)breakdown.elapsed(copy_phase_resolve,t_resolve);
    const uint64_t t_freeze=sampled?rt_cpu::now():0;
    const DescriptorContent* frozen_ptrs[kFastCopyMaxSlots];
    uint64_t unpinned_mask=0;
    if(!TryFreezeMultiSpanRawLockFree(items,src_heaps,span_offsets,
        span_count,pair,single_src_heap,frozen_ptrs,unpinned_mask,sampled,ref_cache)){
        out_spill_reason=fast_spill_freeze;
        return false;
    }
    if(sampled)breakdown.elapsed(copy_phase_freeze,t_freeze);
    if(FAILED(forward())){
        if(unpinned_mask)ReleaseUnpinnedMask(frozen_ptrs,unpinned_mask,ref_cache);
        return true;
    }
    const uint64_t t_commit=sampled?rt_cpu::now():0;
    if(all_dst_vis&&dst_heap_count==1){
        PreviousHeap* const dh=dst_uniq_heaps[0].heap;
        // Collect unique destination pages (total slots <= kFastCopyMaxSlots == 64,
        // so at most kFastCopyMaxSlots unique pages can ever be touched).
        uint32_t vis_pages[kFastCopyMaxSlots];
        uint32_t vis_page_count=0;
        uint32_t last_p=UINT32_MAX;
        for(uint32_t s=0;s<span_count;++s){
            const uint32_t df=items[s].destination.slot;
            const uint32_t cnt=items[s].count;
            const uint32_t p0=PreviousPageIndex(df);
            const uint32_t p1=(cnt==1u)?p0:PreviousPageIndex(df+cnt-1u);
            if(p0==last_p&&p1==last_p)continue;
            for(uint32_t p=p0;p<=p1;++p){
                if(p==last_p)continue;
                last_p=p;
                if(vis_page_count==1u&&vis_pages[0]==p)continue;
                bool seen=false;
                for(uint32_t k=0;k<vis_page_count;++k){
                    if(vis_pages[k]==p){seen=true;break;}
                }
                if(!seen)vis_pages[vis_page_count++]=p;
            }
        }
        PreviousPage* const dh_pages=dh->pages.get();
        if(vis_page_count==1u){
            dh_pages[vis_pages[0]].lock_vis_write();
        }else if(vis_page_count==2u){
            if(vis_pages[0]>vis_pages[1])std::swap(vis_pages[0],vis_pages[1]);
            dh_pages[vis_pages[0]].lock_vis_write();
            dh_pages[vis_pages[1]].lock_vis_write();
        }else{
            for(uint32_t i=1;i<vis_page_count;++i){
                const uint32_t key=vis_pages[i];
                uint32_t j=i;
                while(j>0&&vis_pages[j-1u]>key){vis_pages[j]=vis_pages[j-1u];--j;}
                vis_pages[j]=key;
            }
            for(uint32_t k=0;k<vis_page_count;++k)dh_pages[vis_pages[k]].lock_vis_write();
        }
        const DescriptorContent* displaced[kFastCopyMaxSlots];
        uint32_t disp_count=0;
        bool published=false;
        if(PreviousCurrent(pair,*dh)){
            uint64_t next_version=dh->reserve_write_versions(span_count);
            DescriptorSlot* const dh_slots=dh->slots.get();
            for(uint32_t s=0;s<span_count;++s){
                const uint64_t span_version=next_version++;
                const auto& sp=items[s];
                auto* target=dh_slots+sp.destination.slot;
                const uint32_t base_pos=span_offsets[s];
                for(uint32_t i=0;i<sp.count;++i){
                    const uint32_t pos=base_pos+i;
                    bool changed=false;
                    const DescriptorContent* cur=nullptr;
                    WriteSlotUnderPageLock(target[i],frozen_ptrs[pos],span_version,2,changed,cur);
                    if(changed){
                        unpinned_mask&=~(1ull<<pos);
                        if(cur)displaced[disp_count++]=cur;
                    }
                }
            }
            published=true;
        }
        if(vis_page_count==1u){
            dh_pages[vis_pages[0]].unlock_vis_write();
        }else if(vis_page_count==2u){
            dh_pages[vis_pages[1]].unlock_vis_write();
            dh_pages[vis_pages[0]].unlock_vis_write();
        }else{
            for(uint32_t k=vis_page_count;k>0;--k)dh_pages[vis_pages[k-1u]].unlock_vis_write();
        }
        if(unpinned_mask)ReleaseUnpinnedMask(frozen_ptrs,unpinned_mask,ref_cache);
        for(uint32_t k=0;k<disp_count;++k)ReleaseDescriptorCached(displaced[k],ref_cache);
        if(ref_cache)ref_cache->maybe_sweep();
        if(published&&UavObserve::g_probe.enabled.load(std::memory_order_relaxed)){
            try{
                for(uint32_t s=0;s<span_count;++s){
                    const auto& sp=items[s];
                    for(uint32_t i=0;i<sp.count;++i)
                        UavObserve::g_probe.copy({sp.destination.heap,sp.destination.slot+i},
                            {sp.source.heap,sp.source.slot+i},
                            sp.source.cpu+uint64_t(i)*sp.destination.increment,true);
                }
            }catch(...){}
        }
        if(sampled){
            breakdown.elapsed(copy_phase_commit,t_commit);
            g_copy_fast_multi_hits.fetch_add(1,std::memory_order_relaxed);
            g_copy_fast_multi_spans.fetch_add(span_count,std::memory_order_relaxed);
            g_copy_fast_multi_dst_pages.fetch_add(vis_page_count,std::memory_order_relaxed);
        }
        return true;
    }
    if(all_dst_vis){
        struct VisPageRef { PreviousHeap* heap; uint64_t generation; uint32_t page; };
        VisPageRef vis_pages[kFastCopyMaxSlots];
        uint32_t vis_page_count=0;
        for(uint32_t s=0;s<span_count;++s){
            PreviousHeap* const dh=dst_uniq_heaps[span_dst_heap_idx[s]].heap;
            const uint32_t df=items[s].destination.slot;
            const uint32_t cnt=items[s].count;
            const uint32_t p0=PreviousPageIndex(df);
            const uint32_t p1=(cnt==1u)?p0:PreviousPageIndex(df+cnt-1u);
            for(uint32_t p=p0;p<=p1;++p){
                bool seen=false;
                for(uint32_t k=0;k<vis_page_count;++k){
                    if(vis_pages[k].heap==dh&&vis_pages[k].page==p){seen=true;break;}
                }
                if(!seen)vis_pages[vis_page_count++]={dh,dh->generation,p};
            }
        }
        for(uint32_t i=1;i<vis_page_count;++i){
            const VisPageRef key=vis_pages[i];
            uint32_t j=i;
            while(j>0&&(vis_pages[j-1u].generation>key.generation||
                (vis_pages[j-1u].generation==key.generation&&vis_pages[j-1u].page>key.page))){
                vis_pages[j]=vis_pages[j-1u];
                --j;
            }
            vis_pages[j]=key;
        }
        for(uint32_t k=0;k<vis_page_count;++k)
            vis_pages[k].heap->pages[vis_pages[k].page].lock_vis_write();
        for(uint32_t k=0;k<dst_heap_count;++k){
            auto& dh=dst_uniq_heaps[k];
            if(PreviousCurrent(pair,*dh.heap)){
                dh.live=true;
                dh.next_version=dh.heap->reserve_write_versions(dh.span_count);
            }
        }
        const DescriptorContent* displaced[kFastCopyMaxSlots];
        uint32_t disp_count=0;
        bool any_published=false;
        for(uint32_t s=0;s<span_count;++s){
            auto& dh=dst_uniq_heaps[span_dst_heap_idx[s]];
            if(!dh.live)continue;
            any_published=true;
            const uint64_t span_version=dh.next_version++;
            const auto& sp=items[s];
            auto* target=dh.heap->slots.get()+sp.destination.slot;
            const uint32_t base_pos=span_offsets[s];
            for(uint32_t i=0;i<sp.count;++i){
                const uint32_t pos=base_pos+i;
                bool changed=false;
                const DescriptorContent* cur=nullptr;
                WriteSlotUnderPageLock(target[i],frozen_ptrs[pos],span_version,2,changed,cur);
                if(changed){
                    unpinned_mask&=~(1ull<<pos);
                    if(cur)displaced[disp_count++]=cur;
                }
            }
        }
        for(uint32_t k=vis_page_count;k>0;--k)
            vis_pages[k-1u].heap->pages[vis_pages[k-1u].page].unlock_vis_write();
        if(unpinned_mask)ReleaseUnpinnedMask(frozen_ptrs,unpinned_mask,ref_cache);
        for(uint32_t k=0;k<disp_count;++k)ReleaseDescriptorCached(displaced[k],ref_cache);
        if(ref_cache)ref_cache->maybe_sweep();
        if(any_published&&UavObserve::g_probe.enabled.load(std::memory_order_relaxed)){
            try{
                for(uint32_t s=0;s<span_count;++s){
                    if(!dst_uniq_heaps[span_dst_heap_idx[s]].live)continue;
                    const auto& sp=items[s];
                    for(uint32_t i=0;i<sp.count;++i)
                        UavObserve::g_probe.copy({sp.destination.heap,sp.destination.slot+i},
                            {sp.source.heap,sp.source.slot+i},
                            sp.source.cpu+uint64_t(i)*sp.destination.increment,true);
                }
            }catch(...){}
        }
        if(sampled){
            breakdown.elapsed(copy_phase_commit,t_commit);
            g_copy_fast_multi_hits.fetch_add(1,std::memory_order_relaxed);
            g_copy_fast_multi_spans.fetch_add(span_count,std::memory_order_relaxed);
            g_copy_fast_multi_dst_pages.fetch_add(vis_page_count,std::memory_order_relaxed);
        }
        return true;
    }
    if(dst_page_count==1&&dst_heap_count==1){
        PreviousHeap* const dh=dst_uniq_heaps[0].heap;
        PreviousPage& pg=dh->pages[dst_pages[0].page];
        pg.mutex.lock_copy_write();
        pg.lock_vis_write();
        const DescriptorContent* displaced[kFastCopyMaxSlots];
        uint32_t disp_count=0;
        bool published=false;
        if(PreviousCurrent(pair,*dh)){
            uint64_t next_version=dh->reserve_write_versions(span_count);
            DescriptorSlot* const dh_slots=dh->slots.get();
            for(uint32_t s=0;s<span_count;++s){
                const uint64_t span_version=next_version++;
                const auto& sp=items[s];
                auto* target=dh_slots+sp.destination.slot;
                const uint32_t base_pos=span_offsets[s];
                for(uint32_t i=0;i<sp.count;++i){
                    const uint32_t pos=base_pos+i;
                    bool changed=false;
                    const DescriptorContent* cur=nullptr;
                    WriteSlotUnderPageLock(target[i],frozen_ptrs[pos],span_version,2,changed,cur);
                    if(changed){
                        unpinned_mask&=~(1ull<<pos);
                        if(cur)displaced[disp_count++]=cur;
                    }
                }
            }
            if(!dh->is_vis){
                const uint64_t sq=pg.seq.load(std::memory_order_relaxed);
                pg.seq.store(sq+1u,std::memory_order_release);
            }
            published=true;
        }
        pg.unlock_vis_write();
        pg.mutex.unlock_copy_write();
        if(unpinned_mask)ReleaseUnpinnedMask(frozen_ptrs,unpinned_mask,ref_cache);
        for(uint32_t k=0;k<disp_count;++k)ReleaseDescriptorCached(displaced[k],ref_cache);
        if(ref_cache)ref_cache->maybe_sweep();
        if(published&&UavObserve::g_probe.enabled.load(std::memory_order_relaxed)){
            try{
                for(uint32_t s=0;s<span_count;++s){
                    const auto& sp=items[s];
                    for(uint32_t i=0;i<sp.count;++i)
                        UavObserve::g_probe.copy({sp.destination.heap,sp.destination.slot+i},
                            {sp.source.heap,sp.source.slot+i},
                            sp.source.cpu+uint64_t(i)*sp.destination.increment,true);
                }
            }catch(...){}
        }
        if(sampled){
            breakdown.elapsed(copy_phase_commit,t_commit);
            g_copy_fast_multi_hits.fetch_add(1,std::memory_order_relaxed);
            g_copy_fast_multi_spans.fetch_add(span_count,std::memory_order_relaxed);
            g_copy_fast_multi_dst_pages.fetch_add(1,std::memory_order_relaxed);
        }
        return true;
    }
    // Sort unique destination pages in global (heap generation, page index)
    // order before acquiring locks to prevent AB-BA deadlocks across threads.
    for(uint32_t i=1;i<dst_page_count;++i){
        const FastDstPage key=dst_pages[i];
        uint32_t j=i;
        while(j>0&&(dst_pages[j-1u].generation>key.generation||
            (dst_pages[j-1u].generation==key.generation&&dst_pages[j-1u].page>key.page))){
            dst_pages[j]=dst_pages[j-1u];
            --j;
        }
        dst_pages[j]=key;
    }
    for(uint32_t k=0;k<dst_page_count;++k)
        dst_pages[k].heap->pages[dst_pages[k].page].mutex.lock_copy_write();
    for(uint32_t k=0;k<dst_page_count;++k)
        dst_pages[k].heap->pages[dst_pages[k].page].lock_vis_write();
    for(uint32_t k=0;k<dst_heap_count;++k){
        auto& dh=dst_uniq_heaps[k];
        if(PreviousCurrent(pair,*dh.heap)){
            dh.live=true;
            dh.next_version=dh.heap->reserve_write_versions(dh.span_count);
        }
    }
    const DescriptorContent* displaced[kFastCopyMaxSlots];
    uint32_t disp_count=0;
    bool any_published=false;
    for(uint32_t s=0;s<span_count;++s){
        auto& dh=dst_uniq_heaps[span_dst_heap_idx[s]];
        if(!dh.live)continue;
        any_published=true;
        const uint64_t span_version=dh.next_version++;
        const auto& sp=items[s];
        auto* target=dh.heap->slots.get()+sp.destination.slot;
        const uint32_t base_pos=span_offsets[s];
        for(uint32_t i=0;i<sp.count;++i){
            const uint32_t pos=base_pos+i;
            bool changed=false;
            const DescriptorContent* cur=nullptr;
            WriteSlotUnderPageLock(target[i],frozen_ptrs[pos],span_version,2,changed,cur);
            if(changed){
                unpinned_mask&=~(1ull<<pos);
                if(cur)displaced[disp_count++]=cur;
            }
        }
    }
    for(uint32_t k=0;k<dst_page_count;++k){
        if(dst_uniq_heaps[dst_pages[k].heap_slot_idx].live&&!dst_pages[k].heap->is_vis){
            auto& pg=dst_pages[k].heap->pages[dst_pages[k].page];
            const uint64_t sq=pg.seq.load(std::memory_order_relaxed);
            pg.seq.store(sq+1u,std::memory_order_release);
        }
    }
    for(uint32_t k=dst_page_count;k>0;--k)
        dst_pages[k-1u].heap->pages[dst_pages[k-1u].page].unlock_vis_write();
    for(uint32_t k=dst_page_count;k>0;--k)
        dst_pages[k-1u].heap->pages[dst_pages[k-1u].page].mutex.unlock_copy_write();
    if(unpinned_mask)ReleaseUnpinnedMask(frozen_ptrs,unpinned_mask,ref_cache);
    for(uint32_t k=0;k<disp_count;++k)ReleaseDescriptorCached(displaced[k],ref_cache);
    if(ref_cache)ref_cache->maybe_sweep();
    if(any_published&&UavObserve::g_probe.enabled.load(std::memory_order_relaxed)){
        try{
            for(uint32_t s=0;s<span_count;++s){
                if(!dst_uniq_heaps[span_dst_heap_idx[s]].live)continue;
                const auto& sp=items[s];
                for(uint32_t i=0;i<sp.count;++i)
                    UavObserve::g_probe.copy({sp.destination.heap,sp.destination.slot+i},
                        {sp.source.heap,sp.source.slot+i},
                        sp.source.cpu+uint64_t(i)*sp.destination.increment,true);
            }
        }catch(...){}
    }
    if(sampled){
        breakdown.elapsed(copy_phase_commit,t_commit);
        g_copy_fast_multi_hits.fetch_add(1,std::memory_order_relaxed);
        g_copy_fast_multi_spans.fetch_add(span_count,std::memory_order_relaxed);
        g_copy_fast_multi_dst_pages.fetch_add(dst_page_count,std::memory_order_relaxed);
    }
    return true;
}

void OnNativeDescriptorCopyBatch(uint64_t api,const WWMI_DescriptorSpan_v1* items,
    uint32_t span_count,uint32_t total_slots,WWMI_DescriptorNative_v1 native,void* call) noexcept {
    auto* dev=reinterpret_cast<device*>(api);
    if(!dev||dev!=g_reshade_device||!g_is_dx12)return;
    rt_cpu::Scope cpu(rt_cpu::desc_copy);
    rt_cpu::Scope measured(rt_cpu::previous_copy);
    rt_phase_cpu::Scope phase_copy(rt_phase_cpu::descriptor_copy,measured.start);
    CopyWaits breakdown(measured);
    auto* sampled=measured.kind>=0?&breakdown:nullptr;
    struct ReclaimOnOuterExit {
        ThreadCopyLane& tls;
        bool outermost;
        ReclaimOnOuterExit() noexcept : tls(CurrentCopyLane()), outermost(++tls.callback_depth==1u) {}
        ~ReclaimOnOuterExit() noexcept {
            --tls.callback_depth;
            if(outermost&&tls.copy_depth==0u){
                FlushAbandonedLaneRefCaches();
                TryReclaimRetiredContents();
                DrainDeferredRetiredHeaps();
            }
        }
    } reclaim_on_exit;
    CopyLaneGuard lane_guard;
    FastHeapCache* fast_cache=nullptr;
    LaneRefCache* ref_cache=nullptr;
    if(lane_guard.has_copy_depth()){
        fast_cache=&ThreadFastHeapCache();
        fast_cache->sync(lane_guard.retire_gen);
        if(lane_guard.has_ref_cache())ref_cache=EnsureLaneRefCache(lane_guard.lane_ptr());
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
    if(!span_count||!items){forward();return;}
    const bool allow_fast_copy=lane_guard.has_copy_depth()&&lane_guard.has_fast_raw()&&
        fast_cache!=nullptr&&!UavObserve::g_probe.enabled.load(std::memory_order_relaxed);
    FastCopySpillReason spill_reason=fast_spill_none;
    if(allow_fast_copy){
        PreviousPair pair;
        if(!PreviousDevice(dev,pair,false)){forward();return;}
        bool eligible=(span_count<=kFastCopyMaxSpans&&total_slots<=kFastCopyMaxSlots);
        if(!eligible){
            spill_reason=(span_count>kFastCopyMaxSpans)?fast_spill_spans:fast_spill_slots;
        }else{
            uint32_t checked_slots=0;
            for(uint32_t s=0;s<span_count;++s){
                const auto& sp=items[s];
                const auto& d=sp.destination;
                const auto& src=sp.source;
                const bool valid_reg=sp.count>=1u&&
                    d.heap&&d.heap_creation&&d.increment&&d.slot<d.slots&&sp.count<=d.slots-d.slot&&
                    src.heap&&src.heap_creation&&src.increment&&src.slot<src.slots&&sp.count<=src.slots-src.slot;
                if(!valid_reg){eligible=false;spill_reason=fast_spill_raw;break;}
                if(sp.count>kFastCopyMaxSlots-checked_slots){eligible=false;spill_reason=fast_spill_slots;break;}
                checked_slots+=sp.count;
            }
            if(eligible&&checked_slots>0){
                measured.add(checked_slots);
                phase_copy.add(checked_slots);
                if(TryRunFastDescriptorCopy(items,span_count,pair,fast_cache,ref_cache,
                    sampled,breakdown,forward,spill_reason))
                    return;
            }
        }
        // Generic fallback reusing the already-validated slice without re-enumeration.
        AppDescriptorAdmission admission;
        static thread_local CopyScratch reuse;
        std::optional<CopyScratch> nested;
        CopyScratch* w_ptr=reuse.busy?&nested.emplace():&reuse;
        w_ptr->busy=true;
        CopyUse clear_guard{*w_ptr};
        CopyIngress ingress{admission.pages,*w_ptr,*dev,0,true,false};
        for(uint32_t s=0;s<span_count;++s)DescribeCopySpan(&ingress,&items[s]);
        if(!eligible){
            measured.add(ingress.slots);
            phase_copy.add(ingress.slots);
        }
        if(sampled){
            g_copy_generic_hits.fetch_add(1,std::memory_order_relaxed);
            RecordFastCopySpill(spill_reason);
        }
        if(ingress.complete)try{
            const uint64_t t_resolve=sampled?rt_cpu::now():0;
            ResolveCopyTickets(*w_ptr,pair,sampled,lane_guard,fast_cache);
            if(sampled)breakdown.elapsed(copy_phase_resolve,t_resolve);
            const uint64_t t_freeze=sampled?rt_cpu::now():0;
            FreezeCopySources(*w_ptr,pair,sampled,lane_guard);
            if(sampled)breakdown.elapsed(copy_phase_freeze,t_freeze);
        }catch(...){ingress.complete=false;}
        if(FAILED(forward()))return;
        if(ingress.complete)try{
            const uint64_t t_commit=sampled?rt_cpu::now():0;
            CommitCopyTickets(*w_ptr,pair,sampled);
            if(sampled)breakdown.elapsed(copy_phase_commit,t_commit);
            return;
        }catch(...){}
        InvalidateCopy invalidate{pair,sampled,w_ptr->native_increment};
        for(uint32_t s=0;s<span_count;++s)try{InvalidateCopiedSpan(&invalidate,&items[s]);}catch(...){}
        return;
    }
    AppDescriptorAdmission admission;
    static thread_local CopyScratch reuse;
    std::optional<CopyScratch> nested;
    CopyScratch* w_ptr=reuse.busy?&nested.emplace():&reuse;
    w_ptr->busy=true;
    CopyUse clear_guard{*w_ptr};
    CopyIngress ingress{admission.pages,*w_ptr,*dev,0,true,!lane_guard.has_fast_raw()};
    for(uint32_t s=0;s<span_count;++s)DescribeCopySpan(&ingress,&items[s]);
    if(!lane_guard.has_fast_raw()){
        uint64_t blocked=0;
        admission.enter([&]() noexcept {
            if(sampled&&!blocked)blocked=rt_cpu::now();
        });
        if(blocked)breakdown.elapsed(copy_raw_admission,blocked);
    }
    PreviousPair pair;
    if(!PreviousDevice(dev,pair,false)){forward();return;}
    measured.add(ingress.slots);
    phase_copy.add(ingress.slots);
    if(sampled)g_copy_generic_hits.fetch_add(1,std::memory_order_relaxed);
    if(ingress.complete)try{
        const uint64_t t_resolve=sampled?rt_cpu::now():0;
        ResolveCopyTickets(*w_ptr,pair,sampled,lane_guard,fast_cache);
        if(sampled)breakdown.elapsed(copy_phase_resolve,t_resolve);
        const uint64_t t_freeze=sampled?rt_cpu::now():0;
        FreezeCopySources(*w_ptr,pair,sampled,lane_guard);
        if(sampled)breakdown.elapsed(copy_phase_freeze,t_freeze);
    }catch(...){ingress.complete=false;}
    if(FAILED(forward()))return;
    if(ingress.complete)try{
        const uint64_t t_commit=sampled?rt_cpu::now():0;
        CommitCopyTickets(*w_ptr,pair,sampled);
        if(sampled)breakdown.elapsed(copy_phase_commit,t_commit);
        return;
    }catch(...){}
    InvalidateCopy invalidate{pair,sampled,w_ptr->native_increment};
    for(uint32_t s=0;s<span_count;++s)try{InvalidateCopiedSpan(&invalidate,&items[s]);}catch(...){}
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
        ThreadCopyLane& tls;
        bool outermost;
        ReclaimOnOuterExit() noexcept : tls(CurrentCopyLane()), outermost(++tls.callback_depth==1u) {}
        ~ReclaimOnOuterExit() noexcept {
            --tls.callback_depth;
            if(outermost&&tls.copy_depth==0u){
                FlushAbandonedLaneRefCaches();
                TryReclaimRetiredContents();
                DrainDeferredRetiredHeaps();
            }
        }
    } reclaim_on_exit;
    CopyLaneGuard lane_guard;
    FastHeapCache* fast_cache=nullptr;
    LaneRefCache* ref_cache=nullptr;
    if(lane_guard.has_copy_depth()){
        fast_cache=&ThreadFastHeapCache();
        fast_cache->sync(lane_guard.retire_gen);
        if(lane_guard.has_ref_cache())ref_cache=EnsureLaneRefCache(lane_guard.lane_ptr());
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
    AppDescriptorAdmission admission;
    static thread_local CopyScratch reuse;
    std::optional<CopyScratch> nested;
    CopyScratch* w_ptr=nullptr;
    std::optional<CopyUse> clear_guard;
    auto ensure_scratch=[&]() noexcept -> CopyScratch& {
        if(!w_ptr){
            w_ptr=reuse.busy?&nested.emplace():&reuse;
            w_ptr->busy=true;
            clear_guard.emplace(*w_ptr);
        }
        return *w_ptr;
    };
    const bool allow_fast_copy=lane_guard.has_copy_depth()&&lane_guard.has_fast_raw()&&
        fast_cache!=nullptr&&!UavObserve::g_probe.enabled.load(std::memory_order_relaxed);
    if(allow_fast_copy){
        PreviousPair pair;
        if(!PreviousDevice(dev,pair,false)){forward();return;}
        FastSpanCollector collector;
        struct LazyIngress {
            DescriptorPages& pages;
            device& api;
            decltype(ensure_scratch)& get_scratch;
            std::optional<CopyIngress> built;
            CopyIngress& get() noexcept {
                if(!built)built.emplace(CopyIngress{pages,get_scratch(),api,0,true,false});
                return *built;
            }
        } lazy_ingress{admission.pages,*dev,ensure_scratch,{}};
        auto visit_lazy=[](void* context,const WWMI_DescriptorSpan_v1* span) noexcept {
            auto* ctx=static_cast<std::pair<FastSpanCollector*,LazyIngress*>*>(context);
            auto& self=*ctx->first;
            if(!self.spilled){
                const auto& d=span->destination;
                const auto& s=span->source;
                const bool valid_reg=span->count>=1u&&
                    d.heap&&d.heap_creation&&d.increment&&d.slot<d.slots&&span->count<=d.slots-d.slot&&
                    s.heap&&s.heap_creation&&s.increment&&s.slot<s.slots&&span->count<=s.slots-s.slot;
                if(valid_reg&&self.spans<kFastCopyMaxSpans&&span->count<=kFastCopyMaxSlots-self.slots){
                    self.items[self.spans++]=*span;
                    self.slots+=span->count;
                    return;
                }
                if(!valid_reg)self.spill_reason=fast_spill_raw;
                else if(self.spans>=kFastCopyMaxSpans)self.spill_reason=fast_spill_spans;
                else self.spill_reason=fast_spill_slots;
                self.spilled=true;
                auto& ing=ctx->second->get();
                for(uint32_t i=0;i<self.spans;++i)DescribeCopySpan(&ing,&self.items[i]);
            }
            DescribeCopySpan(&ctx->second->get(),span);
        };
        std::pair<FastSpanCollector*,LazyIngress*> ctx{&collector,&lazy_ingress};
        enumerate(spans,visit_lazy,&ctx);
        if(!collector.spilled){
            if(collector.spans==0){forward();return;}
            measured.add(collector.slots);
            phase_copy.add(collector.slots);
            if(TryRunFastDescriptorCopy(collector.items,collector.spans,pair,fast_cache,ref_cache,
                sampled,breakdown,forward,collector.spill_reason))
                return;
            auto& ing=lazy_ingress.get();
            for(uint32_t s=0;s<collector.spans;++s)DescribeCopySpan(&ing,&collector.items[s]);
        }
        auto& gen=*lazy_ingress.built;
        auto& w=gen.work;
        if(!collector.spilled){
            // measured/phase_copy slots already added above
        }else{
            measured.add(gen.slots);
            phase_copy.add(gen.slots);
        }
        if(sampled){
            g_copy_generic_hits.fetch_add(1,std::memory_order_relaxed);
            RecordFastCopySpill(collector.spill_reason);
        }
        if(gen.complete)try{
            const uint64_t t_resolve=sampled?rt_cpu::now():0;
            ResolveCopyTickets(w,pair,sampled,lane_guard,fast_cache);
            if(sampled)breakdown.elapsed(copy_phase_resolve,t_resolve);
            const uint64_t t_freeze=sampled?rt_cpu::now():0;
            FreezeCopySources(w,pair,sampled,lane_guard);
            if(sampled)breakdown.elapsed(copy_phase_freeze,t_freeze);
        }catch(...){gen.complete=false;}
        if(FAILED(forward()))return;
        if(gen.complete)try{
            const uint64_t t_commit=sampled?rt_cpu::now():0;
            CommitCopyTickets(w,pair,sampled);
            if(sampled)breakdown.elapsed(copy_phase_commit,t_commit);
            return;
        }catch(...){}
        InvalidateCopy invalidate{pair,sampled,w.native_increment};
        try{enumerate(spans,InvalidateCopiedSpan,&invalidate);}catch(...){}
        return;
    }
    auto& w=ensure_scratch();
    CopyIngress ingress{admission.pages,w,*dev,0,true,!lane_guard.has_fast_raw()};
    enumerate(spans,DescribeCopySpan,&ingress);
    if(!lane_guard.has_fast_raw()){
        uint64_t blocked=0;
        admission.enter([&]() noexcept {
            if(sampled&&!blocked)blocked=rt_cpu::now();
        });
        if(blocked)breakdown.elapsed(copy_raw_admission,blocked);
    }
    PreviousPair pair;
    if(!PreviousDevice(dev,pair,false)){forward();return;} // reset does not waive CPU admission
    measured.add(ingress.slots);
    phase_copy.add(ingress.slots);
    if(sampled)g_copy_generic_hits.fetch_add(1,std::memory_order_relaxed);
    if(ingress.complete)try{
        const uint64_t t_resolve=sampled?rt_cpu::now():0;
        ResolveCopyTickets(w,pair,sampled,lane_guard,fast_cache);
        if(sampled)breakdown.elapsed(copy_phase_resolve,t_resolve);
        const uint64_t t_freeze=sampled?rt_cpu::now():0;
        FreezeCopySources(w,pair,sampled,lane_guard);
        if(sampled)breakdown.elapsed(copy_phase_freeze,t_freeze);
    }catch(...){ingress.complete=false;}
    if(FAILED(forward()))return;
    if(ingress.complete)try{
        const uint64_t t_commit=sampled?rt_cpu::now():0;
        CommitCopyTickets(w,pair,sampled);
        if(sampled)breakdown.elapsed(copy_phase_commit,t_commit);
        return;
    }catch(...){}
    InvalidateCopy invalidate{pair,sampled,w.native_increment};
    try{enumerate(spans,InvalidateCopiedSpan,&invalidate);}catch(...){}
}
