// Included inside the descriptor authority. A slot and a frozen copy own one
// intrusive ID, not a resource or a second descriptor representation.
constexpr uint32_t kPinnedRefBit = 0x80000000u;
constexpr uint32_t kMaxPinnedContents = 131072u;
constexpr uint32_t kPinCopyHitThreshold = 4u;

struct DescriptorRefHeader {
    mutable std::atomic<uint32_t> references{1};
    mutable std::atomic<uint32_t> copy_hits{0};
};
struct DescriptorContent;
__declspec(noinline) void DeferDestroyDescriptor(const DescriptorContent*) noexcept;
__declspec(noinline) void TryPinHotDescriptor(const DescriptorContent*, uint32_t observed_refs) noexcept;

__forceinline void RetainDescriptor(const DescriptorContent* content) noexcept {
    if(!content)return;
    const auto* hdr=reinterpret_cast<const DescriptorRefHeader*>(content);
    const uint32_t r=hdr->references.load(std::memory_order_relaxed);
    if(r&kPinnedRefBit)return;
    const uint32_t prev=hdr->references.fetch_add(1,std::memory_order_relaxed);
    const uint32_t hits=hdr->copy_hits.load(std::memory_order_relaxed)+1u;
    hdr->copy_hits.store(hits,std::memory_order_relaxed);
    if(hits>=kPinCopyHitThreshold)TryPinHotDescriptor(content,prev+1u);
}
__forceinline bool TryRetainDescriptor(const DescriptorContent* content) noexcept {
    if(!content)return true;
    const auto* hdr=reinterpret_cast<const DescriptorRefHeader*>(content);
    auto& refs=hdr->references;
    uint32_t r=refs.load(std::memory_order_relaxed);
    if(r&kPinnedRefBit)return true;
    while(r!=0){
        if(r&kPinnedRefBit)return true;
        if(refs.compare_exchange_weak(r,r+1,std::memory_order_acquire,std::memory_order_relaxed)){
            const uint32_t hits=hdr->copy_hits.load(std::memory_order_relaxed)+1u;
            hdr->copy_hits.store(hits,std::memory_order_relaxed);
            if(hits>=kPinCopyHitThreshold)TryPinHotDescriptor(content,r+1);
            return true;
        }
    }
    return false;
}
__forceinline void ReleaseDescriptor(const DescriptorContent* content) noexcept {
    if(!content)return;
    auto& refs=reinterpret_cast<const DescriptorRefHeader*>(content)->references;
    const uint32_t r=refs.load(std::memory_order_relaxed);
    if(r&kPinnedRefBit)return;
    if(refs.fetch_sub(1,std::memory_order_acq_rel)==1){
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
    explicit DescriptorContent(DescriptorDefinition&& value):definition(std::move(value)){}
};
// Reuse storage in blocks, but only after the last slot, freeze or reader
// lease has gone AND any concurrent seqlock readers have drained.
std::pmr::synchronized_pool_resource descriptor_content_pool;
std::mutex retired_content_mutex;
std::vector<const DescriptorContent*> retired_contents;
std::atomic<size_t> retired_content_count{0};

std::mutex pinned_content_mutex;
std::vector<const DescriptorContent*> pinned_contents;
std::atomic<uint32_t> pinned_content_count{0};

void DestroyDescriptorBatch(std::vector<const DescriptorContent*>& batch) noexcept {
    for(const auto* content:batch){
        content->~DescriptorContent();
        descriptor_content_pool.deallocate(const_cast<DescriptorContent*>(content),
            sizeof(DescriptorContent),alignof(DescriptorContent));
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
__declspec(noinline) void TryPinHotDescriptor(const DescriptorContent* content,uint32_t) noexcept {
    if(!content)return;
    // Only pin plain game views that own zero COM replicas/replacements.
    const auto& d=content->definition;
    if(d.replacement||d.replica||d.original_replica||d.original)return;
    if(pinned_content_count.load(std::memory_order_relaxed)>=kMaxPinnedContents)return;
    try{
        std::lock_guard lock(pinned_content_mutex);
        if(pinned_contents.size()>=kMaxPinnedContents)return;
        uint32_t r=content->references.load(std::memory_order_relaxed);
        while(r!=0&&!(r&kPinnedRefBit)){
            // Set kPinnedRefBit and reset low count to 1 owned by pinned_contents.
            if(content->references.compare_exchange_weak(r,kPinnedRefBit|1u,
                std::memory_order_acq_rel,std::memory_order_relaxed)){
                pinned_contents.push_back(content);
                pinned_content_count.store(uint32_t(pinned_contents.size()),std::memory_order_relaxed);
                return;
            }
        }
    }catch(...){}
}
// Called ONLY inside PreviousReset() after all PreviousHeaps are retired & drained
// and before previous_epoch re-opens (no live heap slots hold uncounted refs).
// Note: raw_cpu_views might still hold ContentIds across PreviousReset? Wait!
// Check if raw_cpu_views can hold a pinned ContentId across PreviousReset!
// YES! raw_cpu_views is NOT cleared in PreviousReset! If raw_cpu_views held a
// ContentId to a pinned object and we destroyed it in PreviousReset, ~RawCpuView
// would touch freed memory! So ONLY unpin & destroy pinned_contents at DLL unload
// (max 65,536 * ~280B = 18 MB ceiling, 0 bytes of VRAM)!
struct PinnedContentsShutdownGuard {
    ~PinnedContentsShutdownGuard() {
        std::vector<const DescriptorContent*> batch;
        {
            std::lock_guard lock(pinned_content_mutex);
            batch.swap(pinned_contents);
            pinned_content_count.store(0,std::memory_order_relaxed);
        }
        // At static teardown inside the addon TU, destroy remaining pinned structs
        // before descriptor_content_pool is destroyed (reverse declaration order).
        // Clear kPinnedRefBit so any nested ~ContentId doesn't double-retire.
        DestroyDescriptorBatch(batch);
    }
} pinned_contents_shutdown_guard;

void TryReclaimRetiredContents() noexcept {
    if(retired_content_count.load(std::memory_order_relaxed)<512)return;
    std::vector<const DescriptorContent*> batch;
    {
        std::unique_lock lock(retired_content_mutex,std::try_to_lock);
        if(!lock.owns_lock()||retired_contents.size()<512)return;
        batch.swap(retired_contents);
        retired_content_count.store(0,std::memory_order_relaxed);
    }
    DrainCopyLanes(kCopyDepthMask);
    DestroyDescriptorBatch(batch);
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
        DrainCopyLanes(kCopyDepthMask);
        DestroyDescriptorBatch(batch);
    }
}

ContentId NewDescriptor(DescriptorDefinition definition) {
    auto* storage=descriptor_content_pool.allocate(sizeof(DescriptorContent),alignof(DescriptorContent));
    auto* obj=::new(storage) DescriptorContent(std::move(definition));
    assert(static_cast<const void*>(static_cast<const DescriptorRefHeader*>(obj))==static_cast<const void*>(obj));
    return ContentId::adopt(obj);
}

struct DescriptorSlot {
    ContentId content;
    // Zero is uncovered; nonzero with no content is a real unknown write.
    // Two low bits preserve the Create/Copy diagnostic without a projection.
    uint64_t write=0;
    uint32_t frame=0,miss=0;
    uint64_t version() const noexcept {return write>>2;}
};
