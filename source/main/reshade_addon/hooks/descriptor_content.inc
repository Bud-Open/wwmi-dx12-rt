// Included inside the descriptor authority. A slot and a frozen copy own one
// intrusive ID, not a resource or a second descriptor representation.
struct DescriptorRefHeader {
    mutable std::atomic<uint32_t> references{1};
    uint32_t cacheable{0};
};
struct DescriptorContent;
__declspec(noinline) void DeferDestroyDescriptor(const DescriptorContent*) noexcept;

__forceinline void RetainDescriptor(const DescriptorContent* content) noexcept {
    if(!content)return;
    reinterpret_cast<const DescriptorRefHeader*>(content)->references.fetch_add(1,std::memory_order_relaxed);
}
constexpr uint32_t kMaxBorrowRefCount=0x7FFFFFFFu;
constexpr uint32_t kMaxTryRetainRefCount=UINT32_MAX-1024u;

__forceinline bool TryRetainDescriptor(const DescriptorContent* content) noexcept {
    if(!content)return true;
    auto& refs=reinterpret_cast<const DescriptorRefHeader*>(content)->references;
    uint32_t r=refs.load(std::memory_order_relaxed);
    while(r!=0&&r<kMaxTryRetainRefCount){
        if(refs.compare_exchange_weak(r,r+1,std::memory_order_acquire,std::memory_order_relaxed))
            return true;
    }
    return false;
}
__forceinline bool TryRetainDescriptorN(const DescriptorContent* content,uint32_t n) noexcept {
    if(!content||!n)return true;
    auto& refs=reinterpret_cast<const DescriptorRefHeader*>(content)->references;
    uint32_t r=refs.load(std::memory_order_relaxed);
    while(r!=0&&n<=kMaxBorrowRefCount&&r<=kMaxBorrowRefCount-n){
        if(refs.compare_exchange_weak(r,r+n,std::memory_order_acquire,std::memory_order_relaxed))
            return true;
    }
    return false;
}
__forceinline void ReleaseDescriptor(const DescriptorContent* content) noexcept {
    if(!content)return;
    auto& refs=reinterpret_cast<const DescriptorRefHeader*>(content)->references;
    const uint32_t prev=refs.fetch_sub(1,std::memory_order_acq_rel);
    assert(prev>=1u);
    if(prev==1u){
        DeferDestroyDescriptor(content);
    }
}
__forceinline void ReleaseDescriptorN(const DescriptorContent* content,uint32_t n) noexcept {
    if(!content||!n)return;
    auto& refs=reinterpret_cast<const DescriptorRefHeader*>(content)->references;
    const uint32_t prev=refs.fetch_sub(n,std::memory_order_acq_rel);
    assert(prev>=n);
    if(prev==n){
        DeferDestroyDescriptor(content);
    }
}

class ContentId {
    std::atomic<const DescriptorContent*> p{nullptr};
public:
    ContentId()=default;
    static ContentId adopt(const DescriptorContent* value) noexcept {
        ContentId out;out.p.store(value,std::memory_order_relaxed);return out;
    }
    void init_adopted(const DescriptorContent* value) noexcept {
        p.store(value,std::memory_order_relaxed);
    }
    void store_release(const DescriptorContent* value) noexcept {
        p.store(value,std::memory_order_release);
    }
    void store_relaxed(const DescriptorContent* value) noexcept {
        p.store(value,std::memory_order_relaxed);
    }
    ContentId(const ContentId& other) noexcept {
        const auto* v=other.p.load(std::memory_order_relaxed);
        RetainDescriptor(v);
        p.store(v,std::memory_order_relaxed);
    }
    ContentId(ContentId&& other) noexcept {
        const auto* v=other.p.load(std::memory_order_relaxed);
        other.p.store(nullptr,std::memory_order_relaxed);
        p.store(v,std::memory_order_relaxed);
    }
    ContentId& operator=(const ContentId& other) noexcept {
        const auto* cur=p.load(std::memory_order_relaxed);
        const auto* nv=other.p.load(std::memory_order_relaxed);
        if(cur!=nv){RetainDescriptor(nv);p.store(nv,std::memory_order_release);ReleaseDescriptor(cur);}
        return *this;
    }
    ContentId& operator=(ContentId&& other) noexcept {
        if(this!=&other){
            const auto* cur=p.load(std::memory_order_relaxed);
            const auto* nv=other.p.load(std::memory_order_relaxed);
            other.p.store(nullptr,std::memory_order_relaxed);
            p.store(nv,std::memory_order_release);
            ReleaseDescriptor(cur);
        }
        return *this;
    }
    ~ContentId(){ReleaseDescriptor(p.load(std::memory_order_relaxed));}
    void swap(ContentId& other) noexcept {
        const auto* a=p.load(std::memory_order_relaxed);
        const auto* b=other.p.load(std::memory_order_relaxed);
        p.store(b,std::memory_order_release);
        other.p.store(a,std::memory_order_relaxed);
    }
    const DescriptorContent* load_raw() const noexcept {return p.load(std::memory_order_acquire);}
    const DescriptorContent* load_relaxed() const noexcept {return p.load(std::memory_order_relaxed);}
    explicit operator bool() const noexcept {return p.load(std::memory_order_relaxed)!=nullptr;}
    const DescriptorContent* operator->() const noexcept {return p.load(std::memory_order_relaxed);}
    bool operator==(const ContentId& other) const noexcept {
        return p.load(std::memory_order_relaxed)==other.p.load(std::memory_order_relaxed);
    }
};

struct DescriptorCbv {HeapCbv value;uint64_t resource_generation=0;};
using DescriptorTyped=std::variant<RtPreviousView,RtAuxSource,DescriptorCbv>;
using NativeViewDesc=std::variant<std::monostate,D3D12_SHADER_RESOURCE_VIEW_DESC,
    D3D12_UNORDERED_ACCESS_VIEW_DESC,D3D12_CONSTANT_BUFFER_VIEW_DESC>;
struct DescriptorReplica;
struct DescriptorDefinition {
    uint64_t device=0,origin_cpu=0,resource_generation=0,counter_generation=0;
    uint32_t kind=WWMI_DESCRIPTOR_UNKNOWN;
    DXGI_FORMAT default_format=DXGI_FORMAT_UNKNOWN;
    bool default_texture_2d=false;
    ResInfo info{};
    ID3D12Resource* counter=nullptr;
    NativeViewDesc description;
    std::optional<DescriptorTyped> typed;
    // A raw stamp was copied from this actual CPU view. Keep the original
    // complete content separately from that effective replacement.
    ContentId original;
    std::shared_ptr<const DescriptorReplica> replica,original_replica;
    ID3D12Resource* replacement=nullptr;
    uint64_t replacement_cpu=0;
};
struct DescriptorContent : DescriptorRefHeader {
    const DescriptorDefinition definition;
    // A CPU replica is a lazily derived representation of this exact ID,
    // not a second slot or a replacement for its immutable definition.
    mutable std::once_flag material_once;
    mutable std::shared_ptr<const DescriptorReplica> material_replica;
    explicit DescriptorContent(DescriptorDefinition&& value)
        : DescriptorRefHeader{1,(!value.replacement&&!value.replica)?1u:0u},
          definition(std::move(value)) {}
};

// Per-CopyLane borrowed refcount credit bank.
// Invariant: global references(X) == live ContentId owners(X) + sum_lanes(credit(X)).
// Accessed exclusively by the owning thread while CopyLaneGuard holds copy_depth > 0
// with kCacheDrainBit == 0, and flushed under AcquireLaneCacheDrain on thread exit,
// PreviousReset(), and StopDescriptorPublisher().
struct LaneRefCache {
    static constexpr uint32_t kSets=512;
    static constexpr uint32_t kBorrow=32;
    static constexpr uint32_t kMaxCredit=96;
    static constexpr uint32_t kFlushChunk=64;
    struct Entry {
        const DescriptorContent* ptr=nullptr;
        uint32_t credit=0;
        uint8_t touched=0;
    };
    struct Set {
        Entry ways[2]{};
        uint8_t next_victim=0;
    };
    std::array<Set,kSets> sets{};
    uint32_t ops=0;
    uint32_t sweep_cursor=0;

    static __forceinline size_t set_idx(const DescriptorContent* p) noexcept {
        const uintptr_t v=reinterpret_cast<uintptr_t>(p);
        return size_t(((v>>6)^(v>>15)^(v>>24))&(kSets-1u));
    }

    __declspec(noinline) bool try_retain_miss(const DescriptorContent* ptr,Set& set) noexcept {
        if(!reinterpret_cast<const DescriptorRefHeader*>(ptr)->cacheable)
            return TryRetainDescriptor(ptr);
        if(!TryRetainDescriptorN(ptr,kBorrow))
            return TryRetainDescriptor(ptr); // overflow edge fallback
        constexpr uint32_t spare=kBorrow-1u;
        if(set.ways[0].credit==0){
            set.ways[0]={ptr,spare,1};
            set.next_victim=1;
            return true;
        }
        if(set.ways[1].credit==0){
            set.ways[1]={ptr,spare,1};
            set.next_victim=0;
            return true;
        }
        // Prefer evicting a cold 1-credit release straggler over a warm borrowed entry.
        unsigned v=set.next_victim&1u;
        if(set.ways[0].credit<=1u&&set.ways[1].credit>1u)v=0;
        else if(set.ways[1].credit<=1u&&set.ways[0].credit>1u)v=1;
        const auto* old_ptr=set.ways[v].ptr;
        const uint32_t old_cred=set.ways[v].credit;
        set.ways[v]={ptr,spare,1};
        set.next_victim=uint8_t(v^1u);
        ReleaseDescriptorN(old_ptr,old_cred);
        return true;
    }

    __forceinline bool try_retain(const DescriptorContent* ptr) noexcept {
        if(!ptr)return true;
        auto& set=sets[set_idx(ptr)];
        if(set.ways[0].ptr==ptr&&set.ways[0].credit>0){
            const uint32_t c=--set.ways[0].credit;
            set.ways[0].touched=1;
            set.next_victim=1;
            if(c==0)set.ways[0].ptr=nullptr;
            return true;
        }
        if(set.ways[1].ptr==ptr&&set.ways[1].credit>0){
            const uint32_t c=--set.ways[1].credit;
            set.ways[1].touched=1;
            set.next_victim=0;
            if(c==0)set.ways[1].ptr=nullptr;
            return true;
        }
        return try_retain_miss(ptr,set);
    }

    __declspec(noinline) static void flush_way_chunk(Entry& e) noexcept {
        e.credit-=kFlushChunk;
        ReleaseDescriptorN(e.ptr,kFlushChunk);
    }

    // No-write-allocate on miss: displaced GPU ring descriptors are typically
    // 2-3 frames old and cold; inserting them into an occupied set evicts warm
    // source borrow credits and causes cascading refcount atomics on freeze.
    __declspec(noinline) void release_miss(const DescriptorContent* ptr,Set& set) noexcept {
        if(!reinterpret_cast<const DescriptorRefHeader*>(ptr)->cacheable){
            ReleaseDescriptor(ptr);
            return;
        }
        if(set.ways[0].credit==0){
            set.ways[0]={ptr,1u,1};
            set.next_victim=1;
            return;
        }
        if(set.ways[1].credit==0){
            set.ways[1]={ptr,1u,1};
            set.next_victim=0;
            return;
        }
        ReleaseDescriptor(ptr);
    }

    __forceinline void release(const DescriptorContent* ptr) noexcept {
        if(!ptr)return;
        auto& set=sets[set_idx(ptr)];
        if(set.ways[0].ptr==ptr){
            const uint32_t c=++set.ways[0].credit;
            set.ways[0].touched=1;
            set.next_victim=1;
            if(c>=kMaxCredit)flush_way_chunk(set.ways[0]);
            return;
        }
        if(set.ways[1].ptr==ptr){
            const uint32_t c=++set.ways[1].credit;
            set.ways[1].touched=1;
            set.next_victim=0;
            if(c>=kMaxCredit)flush_way_chunk(set.ways[1]);
            return;
        }
        release_miss(ptr,set);
    }

    __forceinline void maybe_sweep() noexcept {
        if((++ops&4095u)!=0u)return;
        sweep_cold();
    }

    __declspec(noinline) void sweep_cold() noexcept {
        for(uint32_t k=0;k<16u;++k){
            auto& set=sets[sweep_cursor&(kSets-1u)];
            ++sweep_cursor;
            for(auto& e:set.ways){
                if(e.credit==0)continue;
                if(e.touched){e.touched=0;continue;}
                const auto* p=e.ptr;
                const uint32_t c=e.credit;
                e={};
                ReleaseDescriptorN(p,c);
            }
        }
    }

    void flush_all() noexcept {
        for(auto& set:sets){
            for(auto& e:set.ways){
                if(e.ptr&&e.credit){
                    const auto* p=e.ptr;
                    const uint32_t c=e.credit;
                    e={};
                    ReleaseDescriptorN(p,c);
                }else{
                    e={};
                }
            }
            set.next_victim=0;
        }
    }
};

std::mutex lane_ref_cache_alloc_mutex;
std::array<std::unique_ptr<LaneRefCache>,copy_lane_count> lane_ref_cache_storage;
LaneRefCache* EnsureLaneRefCache(CopyLane* lane) noexcept {
    if(!lane)return nullptr;
    if(auto* c=lane->ref_cache.load(std::memory_order_acquire))return c;
    const size_t idx=size_t(lane-copy_lanes.data());
    if(idx>=copy_lane_count)return nullptr;
    try{
        std::lock_guard lock(lane_ref_cache_alloc_mutex);
        if(auto* c=lane->ref_cache.load(std::memory_order_relaxed))return c;
        lane_ref_cache_storage[idx]=std::make_unique<LaneRefCache>();
        auto* ptr=lane_ref_cache_storage[idx].get();
        lane->ref_cache.store(ptr,std::memory_order_release);
        return ptr;
    }catch(...){return nullptr;}
}
void FlushLaneRefCache(LaneRefCache* cache) noexcept {
    if(cache)cache->flush_all();
}
// Reuse storage in blocks, but only after the last slot, freeze or reader
// lease has gone AND any concurrent seqlock readers have drained.
std::pmr::synchronized_pool_resource descriptor_content_pool;
std::mutex retired_content_mutex;
std::vector<const DescriptorContent*> retired_contents;
std::atomic<size_t> retired_content_count{0};
std::atomic<uint64_t> reclaimed_content_batches{0};
std::atomic<uint64_t> reclaimed_content_ticks{0};

void DestroyDescriptorBatch(std::vector<const DescriptorContent*>& batch) noexcept {
    for(const auto* content:batch){
        content->~DescriptorContent();
        descriptor_content_pool.deallocate(const_cast<DescriptorContent*>(content),
            sizeof(DescriptorContent),alignof(DescriptorContent));
    }
}
void ReclaimDetachedBatchWithGrace(std::vector<const DescriptorContent*>& batch) noexcept {
    if(batch.empty())return;
    const bool timed=rt_cpu::phase.load(std::memory_order_relaxed)==2;
    const uint64_t t0=timed?rt_cpu::now():0;
    // Publish a dedicated content grace epoch (separate from previous_retire_generation
    // so heap TLS caches stay warm) before draining copy lanes.
    content_grace_epoch.fetch_add(1,std::memory_order_seq_cst);
    DrainCopyLanes(kCopyDepthMask);
    DestroyDescriptorBatch(batch);
    if(timed){
        const uint64_t t1=rt_cpu::now();
        reclaimed_content_batches.fetch_add(1,std::memory_order_relaxed);
        if(t1>t0)reclaimed_content_ticks.fetch_add(t1-t0,std::memory_order_relaxed);
    }
}
__declspec(noinline) void DeferDestroyDescriptor(const DescriptorContent* content) noexcept {
    try{
        std::lock_guard lock(retired_content_mutex);
        retired_contents.push_back(content);
        retired_content_count.store(retired_contents.size(),std::memory_order_relaxed);
    }catch(...){
        // Never use-after-free under OOM during retirement.
    }
}

void TryReclaimRetiredContents() noexcept {
    if(retired_content_count.load(std::memory_order_relaxed)<512u)return;
    std::vector<const DescriptorContent*> batch;
    {
        std::unique_lock lock(retired_content_mutex,std::try_to_lock);
        if(!lock.owns_lock()||retired_contents.size()<512u)return;
        batch.swap(retired_contents);
        retired_content_count.store(0,std::memory_order_relaxed);
    }
    ReclaimDetachedBatchWithGrace(batch);
}
void ReclaimRetiredContentsBlocking() noexcept {
    for(;;){
        std::vector<const DescriptorContent*> batch;
        {
            std::lock_guard lock(retired_content_mutex);
            batch.swap(retired_contents);
            retired_content_count.store(0,std::memory_order_relaxed);
        }
        if(batch.empty())return;
        ReclaimDetachedBatchWithGrace(batch);
    }
}

ContentId NewDescriptor(DescriptorDefinition definition) {
    auto* storage=descriptor_content_pool.allocate(sizeof(DescriptorContent),alignof(DescriptorContent));
    auto* obj=::new(storage) DescriptorContent(std::move(definition));
    assert(static_cast<const void*>(static_cast<const DescriptorRefHeader*>(obj))==static_cast<const void*>(obj));
    return ContentId::adopt(obj);
}

constexpr uint64_t kSlotUpdatingBit=uint64_t{1}<<63;

struct DescriptorSlot {
    ContentId content;
    // Zero is uncovered; nonzero with no content is a real unknown write.
    // Two low bits preserve the Create/Copy diagnostic without a projection.
    // Bit 63 (kSlotUpdatingBit) brackets lock-free slot updates for readers.
    std::atomic<uint64_t> write{0};
    std::atomic<uint32_t> frame{0};
    std::atomic<uint32_t> miss{0};
    uint64_t version() const noexcept {return (write.load(std::memory_order_relaxed)&~kSlotUpdatingBit)>>2;}
};
