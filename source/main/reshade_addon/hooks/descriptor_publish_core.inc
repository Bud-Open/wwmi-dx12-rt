// Main's authority for observed application CBV/SRV/UAV slots. Legacy
// diagnostics, material, previous, auxiliary and CBV all read these slots.
namespace {
#include "descriptor_content.inc"
struct PreviousResource {
    uint64_t device=0,generation=0,width=0,va=0,native_width=0;
    DXGI_FORMAT default_format=DXGI_FORMAT_UNKNOWN;
    bool shape_known=false,buffer=false,texture_candidate=false,default_texture_2d=false;
    uint16_t texture_height=0,texture_mips=0,texture_layers=0;
    bool texture_2d=false;
};
struct PreviousHandleHash {
    size_t operator()(uint64_t h) const noexcept {return (h>>6)^(h>>13)^(h>>21);}
};
struct alignas(64) PreviousResourceShard {
    std::mutex mutex;
    std::unordered_map<uint64_t,PreviousResource,PreviousHandleHash> resources;
    std::atomic<uint64_t> revision{0};
};
std::array<PreviousResourceShard,64> previous_resources;
std::atomic<uint64_t> previous_creation_serial{0};

struct PreviousPair { uint64_t device=0,epoch=0; };
struct PreviousHeapToken {
    uint64_t handle=0,cpu=0;
    uint32_t size=0;
    FlatHeapMap::Entry* entry=nullptr;
    bool live() const noexcept {
        return entry&&size&&entry->id.load(std::memory_order_acquire)==handle&&
            entry->sz.load(std::memory_order_relaxed)==size&&
            entry->lo.load(std::memory_order_relaxed)==cpu;
    }
    bool operator==(const PreviousHeapToken&) const=default;
};
constexpr uint32_t previous_page_slots=32;
// 3-mode user-space room mutex for PreviousPage:
// - Mode 0 (0x0000): unlocked
// - Mode 1 (0x4000): shared readers (std::shared_lock compatible)
// - Mode 2 (0x8000): concurrent disjoint D3D12 CopyDescriptors writers!
// - Mode 3 (0xC000): exclusive single-slot writer / heap drain (std::unique_lock compatible)
// Eliminates Win32 SRWLock ntdll thunks AND eliminates false serialization
// across 7 rendering threads suballocating adjacent descriptor ranges!
class FastPageMutex {
    static constexpr uint32_t kCountMask = 0x00003FFFu;
    static constexpr uint32_t kModeMask  = 0x0000C000u;
    static constexpr uint32_t kModeRead  = 0x00004000u;
    static constexpr uint32_t kModeCopy  = 0x00008000u;
    static constexpr uint32_t kModeExcl  = 0x0000C000u;
    static constexpr uint32_t kWaiterBit = 0x00010000u;
    std::atomic<uint32_t> state_{0};

    __declspec(noinline) void wait_until_unlocked_or_mode(uint32_t allow_mode) noexcept {
        for(unsigned spin=0;;){
            uint32_t s=state_.load(std::memory_order_relaxed);
            const uint32_t mode=s&kModeMask;
            if(mode==0){
                const uint32_t target=(allow_mode==kModeExcl)?(kModeExcl|1u):(allow_mode|1u);
                if(state_.compare_exchange_weak(s,(s&kWaiterBit)|target,
                    std::memory_order_acquire,std::memory_order_relaxed))return;
                continue;
            }
            if(allow_mode!=kModeExcl&&mode==allow_mode&&!(s&kWaiterBit)&&(s&kCountMask)<kCountMask){
                if(state_.compare_exchange_weak(s,s+1u,
                    std::memory_order_acquire,std::memory_order_relaxed))return;
                continue;
            }
            if(spin<32){++spin;_mm_pause();continue;}
            if(!(s&kWaiterBit)){
                if(!state_.compare_exchange_weak(s,s|kWaiterBit,
                    std::memory_order_relaxed,std::memory_order_relaxed))continue;
                s|=kWaiterBit;
            }
            state_.wait(s,std::memory_order_relaxed);
        }
    }
    __forceinline void leave_counted() noexcept {
        uint32_t s=state_.load(std::memory_order_relaxed);
        for(;;){
            const uint32_t cnt=s&kCountMask;
            const uint32_t next=(cnt>1u)?(s-1u):0u;
            if(state_.compare_exchange_weak(s,next,std::memory_order_release,std::memory_order_relaxed)){
                if(cnt==1u&&(s&kWaiterBit))state_.notify_all();
                return;
            }
        }
    }
public:
    __forceinline void lock() noexcept {
        uint32_t exp=0;
        if(state_.compare_exchange_strong(exp,kModeExcl|1u,std::memory_order_acquire,std::memory_order_relaxed))return;
        wait_until_unlocked_or_mode(kModeExcl);
    }
    __forceinline void unlock() noexcept {
        const uint32_t prev=state_.exchange(0u,std::memory_order_release);
        if(prev&kWaiterBit)state_.notify_all();
    }
    __forceinline void lock_shared() noexcept {
        uint32_t s=state_.load(std::memory_order_relaxed);
        if(s==0){
            if(state_.compare_exchange_strong(s,kModeRead|1u,std::memory_order_acquire,std::memory_order_relaxed))return;
        }else if((s&(kModeMask|kWaiterBit))==kModeRead&&(s&kCountMask)<kCountMask){
            if(state_.compare_exchange_strong(s,s+1u,std::memory_order_acquire,std::memory_order_relaxed))return;
        }
        wait_until_unlocked_or_mode(kModeRead);
    }
    __forceinline void unlock_shared() noexcept {leave_counted();}
    __forceinline void lock_copy_write() noexcept {
        uint32_t s=state_.load(std::memory_order_relaxed);
        if(s==0){
            if(state_.compare_exchange_strong(s,kModeCopy|1u,std::memory_order_acquire,std::memory_order_relaxed))return;
        }else if((s&(kModeMask|kWaiterBit))==kModeCopy&&(s&kCountMask)<kCountMask){
            if(state_.compare_exchange_strong(s,s+1u,std::memory_order_acquire,std::memory_order_relaxed))return;
        }
        wait_until_unlocked_or_mode(kModeCopy);
    }
    __forceinline void unlock_copy_write() noexcept {leave_counted();}
    __forceinline bool has_active_writer() const noexcept {
        const uint32_t m=state_.load(std::memory_order_acquire)&kModeMask;
        return m==kModeCopy||m==kModeExcl;
    }
};
struct alignas(64) PreviousPage {
    FastPageMutex mutex;
    // Writer-only page spinlock (NEVER touched by lock-free WithPublishedSlot
    // readers). Serializes all writers targeting the same 32-slot page with a
    // single LOCK BTS on acquire and a plain MOV 0 release store on unlock,
    // allowing slot loops inside the page to publish with zero LOCK instructions.
    // Global lock order: page.mutex (sorted by generation, page) -> vis_write_lock
    // (sorted by generation, page). Never hold vis_write_lock across allocations
    // or ContentId destruction/reclamation.
    std::atomic<uint32_t> vis_write_lock{0};
    std::atomic<uint64_t> seq{0}; // bumped BEFORE writer unlocks; seqlock checks !has_active_writer() + seq
    std::atomic<bool> may_stamp{false};

    __forceinline void lock_vis_write() noexcept {
#if defined(_MSC_VER) && defined(_M_X64)
        static_assert(sizeof(std::atomic<uint32_t>)==sizeof(long)&&alignof(std::atomic<uint32_t>)==alignof(long));
        if(!_interlockedbittestandset(reinterpret_cast<volatile long*>(&vis_write_lock),0))return;
#else
        if(vis_write_lock.exchange(1u,std::memory_order_acquire)==0u)return;
#endif
        lock_vis_write_slow();
    }
    __declspec(noinline) void lock_vis_write_slow() noexcept {
        for(unsigned spin=0;;++spin){
            if(vis_write_lock.load(std::memory_order_relaxed)==0u){
#if defined(_MSC_VER) && defined(_M_X64)
                if(!_interlockedbittestandset(reinterpret_cast<volatile long*>(&vis_write_lock),0))return;
#else
                if(vis_write_lock.exchange(1u,std::memory_order_acquire)==0u)return;
#endif
            }
            if(spin<64)_mm_pause();else std::this_thread::yield();
        }
    }
    __forceinline void unlock_vis_write() noexcept {
        vis_write_lock.store(0u,std::memory_order_release);
    }
};
struct VisWriteGuard {
    PreviousPage& page;
    explicit VisWriteGuard(PreviousPage& p) noexcept : page(p) {page.lock_vis_write();}
    ~VisWriteGuard() noexcept {page.unlock_vis_write();}
    VisWriteGuard(const VisWriteGuard&)=delete;
};
uint32_t PreviousPageIndex(uint32_t slot) noexcept {return slot/previous_page_slots;}
uint32_t PreviousPageEnd(uint32_t slot,uint32_t end) noexcept {
    return (std::min)(end,(PreviousPageIndex(slot)+1)*previous_page_slots);
}
struct PreviousHeap {
    const PreviousHeapToken flat;
    const uint64_t epoch,generation;
    const bool is_vis;
    std::atomic<uint64_t> host_generation;
    std::atomic<uint32_t> host_index;
    std::atomic<bool> retired{false};
    // One compact authoritative slot. The older FlatHeapMap is used only
    // for native heap identity, physical range and the existing death watch.
    const std::unique_ptr<DescriptorSlot[]> slots;
    const std::unique_ptr<PreviousPage[]> pages;
    std::atomic<uint64_t> next_write{0};
    PreviousHeap(PreviousHeapToken token,PreviousPair pair,uint64_t host_gen,uint32_t index)
        :flat(token),epoch(pair.epoch),generation(previous_creation_serial.fetch_add(1,std::memory_order_relaxed)+1),
         is_vis(token.entry&&token.entry->vis.load(std::memory_order_relaxed)),
         host_generation(host_gen),host_index(index),
         slots(new DescriptorSlot[token.size]),
         pages(new PreviousPage[PreviousPageIndex(token.size-1)+1]) {}
    PreviousPage& page(uint32_t slot) const noexcept {return pages[PreviousPageIndex(slot)];}
    // A version identifies one original span; it does not order writes to
    // different slots across threads. Batch reservations per-thread to avoid
    // global atomic contention on the single GPU heap while preserving strict
    // same-thread monotonic completion order (including reentrant callbacks).
    uint64_t reserve_write_versions(uint64_t count) noexcept {
        if(count<=64){
            struct VersionCache { uint64_t heap_gen=0,next=0,remaining=0; };
            static thread_local std::array<VersionCache,8> caches{};
            auto& c=caches[generation&7];
            if(c.heap_gen!=generation||c.remaining<count){
                constexpr uint64_t kBatch=256;
                const uint64_t take=count>kBatch?count:kBatch;
                c.heap_gen=generation;
                c.next=next_write.fetch_add(take,std::memory_order_relaxed)+1;
                c.remaining=take;
            }
            const uint64_t base=c.next;
            c.next+=count;
            c.remaining-=count;
            return base;
        }
        return next_write.fetch_add(count,std::memory_order_relaxed)+1;
    }
    uint64_t write_version() noexcept {return reserve_write_versions(1);}
};
using PreviousHeapPtr=std::shared_ptr<PreviousHeap>;
struct alignas(64) PreviousHeapShard {
    std::shared_mutex mutex;
    std::unordered_map<uint64_t,PreviousHeapPtr> heaps;
    std::atomic<uint64_t> revision{1};
};
std::array<PreviousHeapShard,64> previous_heaps;
PreviousHeapShard& PreviousDirectory(uint64_t h) {
    return previous_heaps[PreviousHandleHash{}(h)&(previous_heaps.size()-1)];
}
std::mutex deferred_retired_heaps_mutex;
std::vector<PreviousHeapPtr> deferred_retired_heaps;
std::atomic<uint32_t> deferred_retired_heap_count{0};
void DrainPreviousHeap(const PreviousHeap& h);
uint32_t CurrentDescriptorIncrement() noexcept {
    static std::atomic<ID3D12Device*> s_dev{nullptr};
    static std::atomic<uint32_t> s_inc{0};
    ID3D12Device* dev=g_d3d12_device;
    if(!dev)return 0;
    if(s_dev.load(std::memory_order_acquire)==dev){
        const uint32_t inc=s_inc.load(std::memory_order_relaxed);
        if(inc)return inc;
    }
    const uint32_t inc=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if(inc){
        s_inc.store(inc,std::memory_order_relaxed);
        s_dev.store(dev,std::memory_order_release);
    }
    return inc;
}

std::mutex previous_reset;
std::atomic<uint64_t> previous_epoch{1};
uint64_t previous_epoch_serial=1; // under previous_reset

enum CopyWaitPart {
    copy_directory,copy_source,copy_destination,copy_raw_admission,copy_native,
    copy_phase_resolve,copy_phase_freeze,copy_phase_commit,copy_dir_neg_hit,
    copy_frozen_unpinned,copy_raw_src,copy_wait_count
};
struct CopyWaitRow {std::atomic<uint64_t> calls{},ticks{};};
struct alignas(64) CopyWaitLane {std::array<CopyWaitRow,copy_wait_count> rows;};
std::array<CopyWaitLane,std::tuple_size_v<decltype(rt_cpu::lanes)>> descriptor_copy_waits;
std::atomic<uint64_t> g_copy_fast_hits{0};
std::atomic<uint64_t> g_copy_fast_multi_hits{0};
std::atomic<uint64_t> g_copy_fast_multi_spans{0};
std::atomic<uint64_t> g_copy_fast_multi_dst_pages{0};
std::atomic<uint64_t> g_copy_generic_hits{0};
std::atomic<uint64_t> g_copy_spill_spans{0};
std::atomic<uint64_t> g_copy_spill_slots{0};
std::atomic<uint64_t> g_copy_spill_raw{0};
std::atomic<uint64_t> g_copy_spill_miss{0};
std::atomic<uint64_t> g_copy_spill_freeze{0};
std::atomic<uint64_t> g_copy_spill_dup_dst{0};
std::atomic<uint64_t> g_copy_spill_pages{0};
std::atomic<uint64_t> g_write_fast_hits{0};
std::atomic<uint64_t> g_write_generic_hits{0};
struct CopyWaits {
    struct Total {uint64_t calls,ticks;};
    rt_cpu::Scope& scope;
    std::array<Total,copy_wait_count> totals;
    explicit CopyWaits(rt_cpu::Scope& s) : scope(s) {if(s.kind>=0)totals={};}
    void add(CopyWaitPart part,uint64_t ticks) noexcept {auto& x=totals[part];++x.calls;x.ticks+=ticks;}
    void elapsed(CopyWaitPart part,uint64_t start,uint64_t observed=0) noexcept {
        const auto stop=(std::min)(observed?observed:rt_cpu::now(),rt_cpu::deadline.load(std::memory_order_acquire));
        add(part,stop>start?stop-start:0);
    }
    ~CopyWaits() {
        if(scope.kind<0)return;
        auto& out=descriptor_copy_waits[scope.slot].rows;
        for(unsigned i=0;i<copy_wait_count;++i)if(totals[i].calls){
            out[i].calls.fetch_add(totals[i].calls,std::memory_order_relaxed);
            out[i].ticks.fetch_add(totals[i].ticks,std::memory_order_relaxed);
        }
    }
};
template<class Lock> void PreviousLock(Lock& lock,CopyWaits* measured=nullptr,CopyWaitPart part=copy_directory) {
    rt_cpu::Scope wait(rt_cpu::previous_wait);
    if(!measured||wait.kind<0||wait.previous!=&measured->scope){lock.lock();return;}
    const auto before=measured->scope.child;
    lock.lock();wait.end(); // attribute the same old QPC duration, no second pair
    measured->add(part,measured->scope.child-before);
}
bool PreviousDevice(device* dev,PreviousPair& out,bool require_rt) noexcept {
    if(!dev||dev!=g_reshade_device||!g_is_dx12)return false;
    const auto native=dev->get_native();
    if(!native||(require_rt&&wwmi_rt_pack_frame_device.load(std::memory_order_acquire)!=native))return false;
    const auto epoch=previous_epoch.load(std::memory_order_acquire);
    if(!epoch)return false;
    out={native,epoch};return true;
}
bool PreviousCurrent(PreviousPair pair,const PreviousHeap& heap) noexcept {
    return pair.epoch&&heap.epoch==pair.epoch&&previous_epoch.load(std::memory_order_acquire)==pair.epoch&&
        !heap.retired.load(std::memory_order_acquire)&&heap.flat.live();
}
PreviousHeapToken PreviousToken(uint64_t heap,FlatHeapMap::Entry* entry) {
    if(!entry||entry->id.load(std::memory_order_acquire)!=heap)return {};
    return {heap,entry->lo.load(std::memory_order_relaxed),entry->sz.load(std::memory_order_relaxed),entry};
}
// Reuse the established directory and death callback, without allocating a
// parallel array of legacy slots or projection marks.
FlatHeapMap::Entry* EnterPreviousHeapIdentity(uint64_t heap) {
    auto& book=g_heap_to_res;
    if(auto* e=book.entry_of(heap))return e;
    FlatHeapMap::Entry* added=nullptr;
    {
        std::lock_guard life(book.add_mtx);
        if(auto* e=book.entry_of(heap))return e;
        book.bury_due(GetTickCount64());
        auto* native=reinterpret_cast<ID3D12DescriptorHeap*>(heap);
        const auto desc=native->GetDesc();
        if(desc.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)return nullptr;
        if(!desc.NumDescriptors||desc.NumDescriptors>2000000u){
            book.rejected.fetch_add(1,std::memory_order_relaxed);return nullptr;
        }
        added=book.scan([](auto& entry){return entry.id.load(std::memory_order_relaxed)==0;});
        if(!added){
            auto* last=&book.head;
            while(auto* next=last->next.load(std::memory_order_relaxed))last=next;
            auto* block=new FlatHeapMap::Block;
            added=&block->e[0];last->next.store(block,std::memory_order_release);
        }
        added->sz.store(desc.NumDescriptors,std::memory_order_relaxed);
        added->lo.store(native->GetCPUDescriptorHandleForHeapStart().ptr,std::memory_order_relaxed);
        added->vis.store((desc.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)!=0,std::memory_order_relaxed);
        added->arr.store(nullptr,std::memory_order_relaxed);
        added->marks.store(nullptr,std::memory_order_relaxed);
        book.published.fetch_add(1,std::memory_order_acq_rel);
        added->id.store(heap,std::memory_order_release);
        book.published.fetch_add(1,std::memory_order_acq_rel);
    }
    book.watch(heap);
    return added;
}
PreviousHeapPtr PreviousGetHeap(PreviousHeapToken token,PreviousPair pair,uint64_t host_gen=0,bool create=false,uint32_t host_index=0,
    CopyWaits* measured=nullptr) {
    if(!token.live()||previous_epoch.load(std::memory_order_acquire)!=pair.epoch)return {};
    auto same_heap=[&](const PreviousHeapPtr& h){
        return h&&h->flat==token&&h->epoch==pair.epoch&&
            !h->retired.load(std::memory_order_acquire);
    };
    auto matches=[&](const PreviousHeapPtr& h){
        return same_heap(h)&&(!host_gen||h->host_generation.load(std::memory_order_acquire)==host_gen);
    };
    struct Memo { uint64_t handle=0; std::weak_ptr<PreviousHeap> heap; };
    struct NegMemo { uint64_t handle=0,host_gen=0,epoch=0,shard_rev=0; };
    static thread_local std::array<Memo,512> recent{};
    static thread_local std::array<NegMemo,256> neg_recent{};
    const size_t h_idx=PreviousHandleHash{}(token.handle);
    auto& memo=recent[h_idx&(recent.size()-1)];
    if(memo.handle==token.handle){
        auto h=memo.heap.lock();if(matches(h))return h;
    }
    auto& dir=PreviousDirectory(token.handle);
    auto& neg=neg_recent[h_idx&(neg_recent.size()-1)];
    if(!create){
        const uint64_t rev=dir.revision.load(std::memory_order_acquire);
        if(neg.handle==token.handle&&neg.host_gen==host_gen&&neg.epoch==pair.epoch&&neg.shard_rev==rev){
            if(measured)measured->add(copy_dir_neg_hit,0);
            return {};
        }
    }
    auto existing=[&]() -> PreviousHeapPtr {
        const auto it=dir.heaps.find(token.handle);
        return it!=dir.heaps.end()&&matches(it->second)?it->second:nullptr;
    };
    bool attach_native_identity=false;
    uint64_t neg_rev=0;
    {
        std::shared_lock lock(dir.mutex,std::defer_lock);PreviousLock(lock,measured);
        if(auto h=existing()){memo={token.handle,h};return h;}
        if(host_gen&&!create){
            const auto i=dir.heaps.find(token.handle);
            attach_native_identity=i!=dir.heaps.end()&&same_heap(i->second)&&
                !i->second->host_generation.load(std::memory_order_relaxed);
        }
        if(!create&&!attach_native_identity)neg_rev=dir.revision.load(std::memory_order_relaxed);
    }
    if(!create&&!attach_native_identity){
        neg={token.handle,host_gen,pair.epoch,neg_rev};
        return {};
    }
    PreviousHeapPtr replaced;
    PreviousHeapPtr result;
    {
        std::unique_lock lock(dir.mutex,std::defer_lock);PreviousLock(lock,measured);
        if(!token.live()||previous_epoch.load(std::memory_order_acquire)!=pair.epoch)return {};
        if(auto h=existing()){memo={token.handle,h};return h;}
        if(const auto it=dir.heaps.find(token.handle);it!=dir.heaps.end()&&same_heap(it->second)&&
            !it->second->host_generation.load(std::memory_order_relaxed)){
            const auto& h=it->second;h->host_index.store(host_index,std::memory_order_relaxed);
            h->host_generation.store(host_gen,std::memory_order_release);
            dir.revision.fetch_add(1,std::memory_order_release);
            memo={token.handle,h};return h;
        }
        if(!create)return {};
        result=std::make_shared<PreviousHeap>(token,pair,host_gen,host_index);
        auto [it,added]=dir.heaps.try_emplace(token.handle,result);
        if(!added){
            replaced=std::move(it->second);
            replaced->retired.store(true,std::memory_order_release);
            it->second=result;
        }
        dir.revision.fetch_add(1,std::memory_order_release);
        memo={token.handle,result};
    }
    if(replaced){
        previous_retire_generation.fetch_add(1,std::memory_order_seq_cst);
        std::lock_guard dl(deferred_retired_heaps_mutex);
        deferred_retired_heaps.push_back(std::move(replaced));
        deferred_retired_heap_count.fetch_add(1,std::memory_order_release);
    }
    return result;
}
PreviousHeapPtr PreviousGetRange(const WWMI_DescriptorRange_v1& r,PreviousPair pair,bool create,CopyWaits* measured=nullptr) {
    if(!r.heap||!r.heap_creation||!r.slots||r.slot>=r.slots)return {};
    auto* entry=create?EnterPreviousHeapIdentity(r.heap):g_heap_to_res.entry_of(r.heap);
    const auto t=PreviousToken(r.heap,entry);
    return t.size==r.slots&&r.increment&&r.cpu==t.cpu+uint64_t(r.slot)*r.increment?
        PreviousGetHeap(t,pair,r.heap_creation,create,r.heap_index,measured):nullptr;
}
void DrainPreviousHeap(const PreviousHeap& h) {
    // The epoch/retired bit has closed admission to data. Any span already
    // past PreviousCurrent keeps its page set until its whole span finishes.
    for(uint32_t i=0,n=PreviousPageIndex(h.flat.size-1)+1;i<n;++i){
        std::unique_lock drained(h.pages[i].mutex);
        VisWriteGuard vis_drained(h.pages[i]);
    }
}
void DrainDeferredRetiredHeaps() noexcept {
    if(deferred_retired_heap_count.load(std::memory_order_acquire)==0)return;
    if(CurrentCopyLane().copy_depth!=0)return;
    std::vector<PreviousHeapPtr> deferred;
    {
        std::lock_guard dl(deferred_retired_heaps_mutex);
        if(deferred_retired_heaps.empty())return;
        deferred.swap(deferred_retired_heaps);
        deferred_retired_heap_count.store(0,std::memory_order_release);
    }
    DrainCopyLanes(kCopyDepthMask);
    for(auto& old:deferred)DrainPreviousHeap(*old);
}
void PreviousHeapRetired(uint64_t heap) {
    PreviousHeapPtr h;
    {
        auto& dir=PreviousDirectory(heap);
        std::unique_lock lock(dir.mutex);
        const auto it=dir.heaps.find(heap);
        if(it!=dir.heaps.end()&&!it->second->flat.live()){
            it->second->retired.store(true,std::memory_order_release);
            h=std::move(it->second);dir.heaps.erase(it);
            dir.revision.fetch_add(1,std::memory_order_release);
        }
    }
    if(CurrentCopyLane().copy_depth!=0){
        if(h){
            previous_retire_generation.fetch_add(1,std::memory_order_seq_cst);
            // Wait for any already-admitted page-locked raw edit (Stamp/Unstamp)
            // before returning to the native heap death watch, while deferring
            // CopyLane drain of shadow storage until our own copy lane exits.
            DrainPreviousHeap(*h);
            std::lock_guard dl(deferred_retired_heaps_mutex);
            deferred_retired_heaps.push_back(std::move(h));
            deferred_retired_heap_count.fetch_add(1,std::memory_order_release);
        }
        return;
    }
    std::vector<PreviousHeapPtr> deferred;
    if(deferred_retired_heap_count.load(std::memory_order_acquire)!=0){
        std::lock_guard dl(deferred_retired_heaps_mutex);
        deferred.swap(deferred_retired_heaps);
        deferred_retired_heap_count.store(0,std::memory_order_release);
    }
    if(h||!deferred.empty()){
        previous_retire_generation.fetch_add(1,std::memory_order_seq_cst);
        DrainCopyLanes(kCopyDepthMask);
        if(h)DrainPreviousHeap(*h);
        for(auto& old:deferred)DrainPreviousHeap(*old);
    }
}
void PreviousReset() {
    std::lock_guard reset(previous_reset);
    previous_epoch.store(0,std::memory_order_release);
    std::vector<PreviousHeapPtr> retired;
    for(auto& dir:previous_heaps){
        // All writers from the old epoch have either committed or will now
        // reject. Remove from directory and mark retired before draining lanes.
        std::unique_lock directory(dir.mutex);
        for(auto& [key,heap]:dir.heaps){
            heap->retired.store(true,std::memory_order_release);
            retired.push_back(std::move(heap));
        }
        dir.heaps.clear();
        dir.revision.fetch_add(1,std::memory_order_release);
    }
    {
        std::lock_guard dl(deferred_retired_heaps_mutex);
        for(auto& heap:deferred_retired_heaps)retired.push_back(std::move(heap));
        deferred_retired_heaps.clear();
        deferred_retired_heap_count.store(0,std::memory_order_release);
    }
    previous_retire_generation.fetch_add(1,std::memory_order_seq_cst);
    DrainCopyLanes(kCopyDepthMask);
    for(auto& heap:retired)DrainPreviousHeap(*heap);
    retired.clear();
    FlushAllLaneRefCaches();
    ReclaimRetiredContentsBlocking();
    previous_epoch.store(++previous_epoch_serial,std::memory_order_release);
}

PreviousResource PreviousFindResource(uint64_t resource) {
    const auto hash=PreviousHandleHash{}(resource);
    auto& shard=previous_resources[hash&(previous_resources.size()-1)];
    struct Memo {uint64_t resource=0,revision=0;PreviousResource value;};
    struct MemoSet {Memo ways[2]{};uint8_t next_victim=0;};
    static thread_local std::array<MemoSet,512> recent{};
    auto& set=recent[hash&(recent.size()-1)];
    const auto revision=shard.revision.load(std::memory_order_acquire);
    if(set.ways[0].resource==resource&&set.ways[0].revision==revision){
        set.next_victim=1;return set.ways[0].value;
    }
    if(set.ways[1].resource==resource&&set.ways[1].revision==revision){
        set.next_victim=0;return set.ways[1].value;
    }
    std::unique_lock lock(shard.mutex,std::defer_lock);PreviousLock(lock);
    const auto i=shard.resources.find(resource);
    const uint64_t locked_rev=shard.revision.load(std::memory_order_relaxed);
    const PreviousResource val=i==shard.resources.end()?PreviousResource{}:i->second;
    unsigned w=set.next_victim&1u;
    if(set.ways[0].resource==resource)w=0;
    else if(set.ways[1].resource==resource)w=1;
    else if(set.ways[0].resource==0)w=0;
    else if(set.ways[1].resource==0)w=1;
    set.ways[w]={resource,locked_rev,val};
    set.next_victim=uint8_t(w^1u);
    return val;
}
void PreviousForgetResource(uint64_t res) {
    rt_cpu::Scope measured(rt_cpu::resource_life);
    if(!res)return;
    auto& shard=previous_resources[PreviousHandleHash{}(res)&(previous_resources.size()-1)];
    std::lock_guard lock(shard.mutex);
    if(shard.resources.erase(res))shard.revision.fetch_add(1,std::memory_order_release);
}
void PreviousRegisterResource(device* dev,uint64_t res,uint64_t width) {
    rt_cpu::Scope measured(rt_cpu::resource_life);
    if(!dev||!res)return;
    const auto native=dev->get_native();if(!native)return;
    auto& shard=previous_resources[PreviousHandleHash{}(res)&(previous_resources.size()-1)];
    std::unique_lock lock(shard.mutex,std::defer_lock);PreviousLock(lock);
    shard.resources.insert_or_assign(res,PreviousResource{
        native,previous_creation_serial.fetch_add(1,std::memory_order_relaxed)+1,width});
    shard.revision.fetch_add(1,std::memory_order_release);
}
PreviousResource PreviousDescribeResource(device* dev,uint64_t native,uint64_t res) {
    rt_cpu::Scope measured(rt_cpu::resource_life);
    const auto observed=PreviousFindResource(res);
    if(observed.device==native&&observed.shape_known)return observed;
    // This path has a native view packet's live resource. Capture immutable
    // shape once on first use; read/Copy consumers never query a dying object.
    auto* resource=reinterpret_cast<ID3D12Resource*>(res);
    const auto api_desc=dev->get_resource_desc({res});
    const auto native_desc=resource->GetDesc();
    PreviousResource value{};
    value.device=native;value.shape_known=true;
    value.buffer=api_desc.type==resource_type::buffer;
    value.native_width=native_desc.Width;
    if(value.buffer)value.width=api_desc.buffer.size;
    if(native_desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER){
        value.va=resource->GetGPUVirtualAddress();
    }
    if(!value.buffer){
        value.default_format=native_desc.Format;
        value.default_texture_2d=native_desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&native_desc.SampleDesc.Count==1;
        value.texture_2d=native_desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        if(value.texture_2d&&native_desc.Height<=UINT16_MAX){
            value.texture_height=uint16_t(native_desc.Height);
            value.texture_mips=native_desc.MipLevels?native_desc.MipLevels:1;
            value.texture_layers=native_desc.DepthOrArraySize?native_desc.DepthOrArraySize:1;
        }
        value.texture_candidate=api_desc.type==resource_type::texture_2d&&
            (uint32_t)(api_desc.usage&(resource_usage::render_target|resource_usage::depth_stencil|resource_usage::unordered_access))==0&&
            uint64_t(api_desc.texture.width)*api_desc.texture.height>=kTexHashFloorPx;
    }
    measured.add(); // native shape queries, not resource registrations
    auto& shard=previous_resources[PreviousHandleHash{}(res)&(previous_resources.size()-1)];
    std::unique_lock lock(shard.mutex,std::defer_lock);PreviousLock(lock);
    const auto old=shard.resources.find(res);
    if(observed.generation&&(old==shard.resources.end()||old->second.generation!=observed.generation))return {};
    if(old!=shard.resources.end()&&old->second.device==native){
        if(old->second.shape_known)return old->second; // another Create captured this generation
        value.generation=old->second.generation;
    }else value.generation=previous_creation_serial.fetch_add(1,std::memory_order_relaxed)+1;
    shard.resources.insert_or_assign(res,value);
    shard.revision.fetch_add(1,std::memory_order_release);
    return value;
}
bool PreviousViewLive(uint64_t native,const RtPreviousView& v) {
    const auto r=PreviousFindResource(v.resource);
    return r.device==native&&r.generation==v.resource_generation&&
        v.byte_offset<=r.width&&v.bytes&&v.bytes<=r.width-v.byte_offset;
}
uint32_t AuxiliaryElementBytes(uint32_t format) {
    switch(static_cast<DXGI_FORMAT>(format)){
    case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:return 8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:return 4;
    default:return 0;
    }
}
bool AuxiliaryFits(uint64_t native,const RtAuxSource& v,const PreviousResource& r) {
    const auto element=AuxiliaryElementBytes(v.view.format);
    return element&&v.view.count&&!v.view.stride&&!v.view.flags&&
        r.device==native&&r.generation==v.resource_generation&&
        v.view.first<=r.width/element&&v.view.count<=r.width/element-v.view.first;
}
bool AuxiliaryViewLive(uint64_t native,const RtAuxSource& v) {
    return v.view.resource&&AuxiliaryFits(native,v,PreviousFindResource(v.view.resource));
}

struct RawCpuView {ContentId content;uint64_t device=0;};
ShardedMap<uint64_t,RawCpuView,PreviousHandleHash> raw_cpu_views;

const DescriptorDefinition* PublishedDefinition(const PreviousHeap& h,uint32_t slot) noexcept {
    const auto& content=h.slots[slot].content;
    return content?&content->definition:nullptr;
}
const DescriptorTyped* PublishedSlotType(const PreviousHeap& h,uint32_t slot) noexcept {
    const auto* d=PublishedDefinition(h,slot);
    return d&&d->typed?&*d->typed:nullptr;
}
bool DefinitionLive(uint64_t native,const DescriptorDefinition& definition) {
    if(definition.device!=native)return false;
    const auto live=[&](ID3D12Resource* resource,uint64_t generation){
        if(!resource)return true;
        const auto r=PreviousFindResource(reinterpret_cast<uint64_t>(resource));
        return generation&&r.device==native&&r.generation==generation;
    };
    return live(definition.info.res,definition.resource_generation)&&live(definition.counter,definition.counter_generation);
}
// Writer-side slot publisher: caller MUST hold target's page vis_write_lock so
// no concurrent writer touches this 32-slot page. Zero LOCK instructions on x64:
// Boehm C++20 seqlock write protocol (updating bit -> release fence -> data ->
// release store of unique monotonic new_w) pairs with WithPublishedSlot's
// acquire loads + acquire fence. Never allocates or destroys ContentId.
__forceinline void WriteSlotUnderPageLock(DescriptorSlot& target,const DescriptorContent* nv,
    uint64_t version,uint8_t kind,bool& changed_out,const DescriptorContent*& cur_out) noexcept {
    const uint64_t new_w=((version<<2)|kind)&~kSlotUpdatingBit;
    target.write.store(new_w|kSlotUpdatingBit,std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const auto* cur=target.content.load_relaxed();
    if(cur!=nv){
        target.content.store_relaxed(nv);
        changed_out=true;
        cur_out=cur;
    }else{
        changed_out=false;
        cur_out=nullptr;
    }
    target.frame.store(g_frame_id,std::memory_order_relaxed);
    target.miss.store(0,std::memory_order_relaxed);
    target.write.store(new_w,std::memory_order_release);
}
void PublishContent(PreviousHeap& h,uint32_t slot,uint64_t version,ContentId content,uint8_t kind) noexcept {
    auto& page=h.page(slot);
    auto& target=h.slots[slot];
    const uint64_t new_w=((version<<2)|kind)&~kSlotUpdatingBit;
    {
        VisWriteGuard vis_lock(page);
        if(content&&content->definition.replacement)page.may_stamp.store(true,std::memory_order_release);
        target.write.store(new_w|kSlotUpdatingBit,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        target.content.swap(content);
        target.frame.store(g_frame_id,std::memory_order_relaxed);
        target.miss.store(0,std::memory_order_relaxed);
        target.write.store(new_w,std::memory_order_release);
        const uint64_t s=page.seq.load(std::memory_order_relaxed);
        page.seq.store(s+1u,std::memory_order_release);
    }
    // ~ContentId() releases displaced content strictly AFTER vis_lock is released.
}
// Exclusive-lock-only publisher: caller holds std::unique_lock<FastPageMutex>
// (kModeExcl). VisWriteGuard still serializes against any lock-free vis copy
// writer touching the same page, in global page.mutex -> vis_write_lock order.
void PublishContentExclusive(PreviousHeap& h,uint32_t slot,uint64_t version,ContentId content,uint8_t kind) noexcept {
    PublishContent(h,slot,version,std::move(content),kind);
}
// Per-page writer-only serialized publisher for shader-visible heaps
// (is_vis == true): never touches page.mutex so lock-free WithPublishedSlot
// readers never contend with writers.
void PublishContentVisLockFree(PreviousHeap& h,uint32_t slot,uint64_t version,ContentId content,uint8_t kind) noexcept {
    auto& page=h.page(slot);
    auto& target=h.slots[slot];
    const uint64_t new_w=((version<<2)|kind)&~kSlotUpdatingBit;
    {
        VisWriteGuard vis_lock(page);
        if(content&&content->definition.replacement)page.may_stamp.store(true,std::memory_order_release);
        target.write.store(new_w|kSlotUpdatingBit,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        target.content.swap(content);
        target.frame.store(g_frame_id,std::memory_order_relaxed);
        target.miss.store(0,std::memory_order_relaxed);
        target.write.store(new_w,std::memory_order_release);
    }
    // ~ContentId() releases displaced content strictly AFTER vis_lock is released.
}
void PublishUnknownSlotExclusive(PreviousHeap& h,uint32_t slot,uint8_t kind) noexcept {
    PublishContentExclusive(h,slot,h.write_version(),{},kind);
}

struct SlotSnapshot {
    ContentId content;
    uint64_t write=0;
    uint32_t frame=0,miss=0;
    uint64_t version() const noexcept {return write>>2;}
};
template<class F>
bool WithPublishedSlot(const PreviousHeap& h,PreviousPair pair,uint32_t slot,bool timed_wait,F&& fn) {
    if(h.is_vis){
        CopyLaneGuard guard;
        if(guard.has_copy_depth()){
            const auto& s=h.slots[slot];
            for(unsigned spin=0;;++spin){
                if(!PreviousCurrent(pair,h))return false;
                const uint64_t w1=s.write.load(std::memory_order_acquire);
                if(w1&kSlotUpdatingBit){
                    if(spin<64)_mm_pause();else std::this_thread::yield();
                    continue;
                }
                const auto* raw=s.content.load_relaxed();
                const uint32_t f=s.frame.load(std::memory_order_relaxed);
                const uint32_t m=s.miss.load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                const uint64_t w2=s.write.load(std::memory_order_relaxed);
                if(w1!=w2){
                    if(spin<64)_mm_pause();else std::this_thread::yield();
                    continue;
                }
                if(raw&&!TryRetainDescriptor(raw))continue;
                SlotSnapshot snap{ContentId::adopt(raw),w1,f,m};
                if(!PreviousCurrent(pair,h))return false;
                // Keep CopyLaneGuard active across validation and projection so
                // PreviousReset() cannot finish draining before this query ends.
                const bool ok=fn(snap);
                return ok&&PreviousCurrent(pair,h);
            }
        }
        // Extreme fallback when all copy lanes are occupied: temporarily
        // drain fast-lane writers so shared_lock sees a consistent slot.
        raw_edits_pending.fetch_add(1,std::memory_order_seq_cst);
        DrainCopyLanes(kRawFastMask);
        struct ReleaseRaw { ~ReleaseRaw() noexcept { raw_edits_pending.fetch_sub(1,std::memory_order_release); } } rel_raw;
        std::shared_lock lock(h.page(slot).mutex,std::defer_lock);
        if(timed_wait)PreviousLock(lock);else lock.lock();
        if(!PreviousCurrent(pair,h))return false;
        const auto& s=h.slots[slot];
        SlotSnapshot snap{s.content,s.write.load(std::memory_order_relaxed)&~kSlotUpdatingBit,
            s.frame.load(std::memory_order_relaxed),s.miss.load(std::memory_order_relaxed)};
        const bool ok=fn(snap);
        return ok&&PreviousCurrent(pair,h);
    }
    std::shared_lock lock(h.page(slot).mutex,std::defer_lock);
    if(timed_wait)PreviousLock(lock);else lock.lock();
    if(!PreviousCurrent(pair,h))return false;
    const auto& s=h.slots[slot];
    SlotSnapshot snap{s.content,s.write.load(std::memory_order_relaxed)&~kSlotUpdatingBit,
        s.frame.load(std::memory_order_relaxed),s.miss.load(std::memory_order_relaxed)};
    const bool ok=fn(snap);
    return ok&&PreviousCurrent(pair,h);
}
bool ReadPublishedSlot(const PreviousHeap& h,PreviousPair pair,uint32_t slot,SlotSnapshot& out,bool timed_wait=false) noexcept {
    const bool ok=WithPublishedSlot(h,pair,slot,timed_wait,[&](SlotSnapshot& snap) noexcept {
        out=std::move(snap);
        return true;
    });
    if(!ok)out={};
    return ok;
}

PreviousHeapPtr ReadHeap(uint64_t heap,PreviousPair pair) {
    return PreviousGetHeap(PreviousToken(heap,g_heap_to_res.entry_of(heap)),pair);
}
const DescriptorDefinition* OriginalDefinition(const DescriptorDefinition& content) noexcept {
    if(!content.replacement_cpu)return &content;
    return content.original?&content.original->definition:nullptr;
}
bool ProjectDescriptorInfo(PreviousPair pair,const ContentId& content,uint64_t write,uint32_t frame,uint32_t miss,ResInfo& out) {
    if(!content)return false;
    const auto& effective=content->definition;
    const auto* original=OriginalDefinition(effective);
    if(!original||original->kind==WWMI_DESCRIPTOR_CBV||!original->info.res||!DefinitionLive(pair.device,*original))return false;
    out=original->info;out.swapped=effective.replacement;out.wrote_frame=frame;
    out.wrote_src=uint8_t(write&3u);out.miss_frame=miss;return true;
}
bool ProjectDescriptorInfo(PreviousPair pair,const DescriptorSlot& slot,ResInfo& out) {
    return ProjectDescriptorInfo(pair,slot.content,
        slot.write.load(std::memory_order_relaxed)&~kSlotUpdatingBit,
        slot.frame.load(std::memory_order_relaxed),
        slot.miss.load(std::memory_order_relaxed),out);
}
bool ProjectDescriptorInfo(PreviousPair pair,const SlotSnapshot& snap,ResInfo& out) {
    return ProjectDescriptorInfo(pair,snap.content,snap.write,snap.frame,snap.miss,out);
}

#include "descriptor_publish_copy.inc"

}

std::atomic<uint64_t> g_struct_fb_cache_hits{0};
std::atomic<uint64_t> g_struct_fb_cache_misses{0};

void RtDescriptorCopyWaitReport() {
    if(rt_cpu::phase.load(std::memory_order_acquire)!=3)return;
    static std::atomic_flag reported{};
    if(reported.test_and_set(std::memory_order_relaxed))return;
    const auto deadline=rt_cpu::deadline.load(std::memory_order_acquire);
    const auto frequency=rt_cpu::frequency.load(std::memory_order_relaxed);
    unsigned rows=0;
    uint64_t total_frozen_unpinned=0,total_raw_src_calls=0,total_raw_src_slots=0;
    for(unsigned i=0;i<descriptor_copy_waits.size();++i){
        const auto& lane=rt_cpu::lanes[i];
        const auto tid=lane.tid.load(std::memory_order_acquire);
        if(!tid)continue;
        const auto pending=lane.active.load(std::memory_order_acquire);
        std::array<CopyWaits::Total,copy_wait_count> v{};
        for(unsigned j=0;j<copy_wait_count;++j){
            const auto& r=descriptor_copy_waits[i].rows[j];
            v[j]={r.calls.load(std::memory_order_relaxed),r.ticks.load(std::memory_order_relaxed)};
        }
        if(!pending&&std::none_of(v.begin(),v.end(),[](const auto& t){return t.calls!=0;}))continue;
        ++rows;
        total_frozen_unpinned+=v[copy_frozen_unpinned].ticks;
        total_raw_src_calls+=v[copy_raw_src].calls;
        total_raw_src_slots+=v[copy_raw_src].ticks;
        char msg[704];
        std::snprintf(msg,sizeof(msg),
            "RT-DESC-COPY-WAIT row tid=%lu directory=%llu/%llu source_page=%llu/%llu destination_page=%llu/%llu "
            "raw_admission=%llu/%llu native_continuation=%llu/%llu resolve=%llu/%llu freeze=%llu/%llu "
            "commit=%llu/%llu dir_neg_hit=%llu frequency=%llu deadline_qpc=%llu pending_thread=%u",
            tid,v[0].calls,v[0].ticks,v[1].calls,v[1].ticks,v[2].calls,v[2].ticks,v[3].calls,v[3].ticks,
            v[4].calls,v[4].ticks,v[5].calls,v[5].ticks,v[6].calls,v[6].ticks,v[7].calls,v[7].ticks,
            v[8].calls,frequency,deadline,unsigned(pending!=0));
        reshade::log::message(reshade::log::level::info,msg);
    }
    char msg[704];
    std::snprintf(msg,sizeof(msg),
        "RT-DESC-COPY-WAIT end rows=%u frozen_unpinned=%llu "
        "raw_src=%llu/%llu copy_fast=%llu copy_fast_multi=%llu multi_spans=%llu multi_dst_pages=%llu "
        "copy_generic=%llu spill=spans:%llu/slots:%llu/raw:%llu/miss:%llu/freeze:%llu/dup:%llu/pages:%llu "
        "write_fast=%llu write_generic=%llu struct_fb=%llu/%llu reclaim=%llu/%llu frequency=%llu deadline_qpc=%llu overflow_threads=%u",
        rows,(unsigned long long)total_frozen_unpinned,
        (unsigned long long)total_raw_src_calls,(unsigned long long)total_raw_src_slots,
        (unsigned long long)g_copy_fast_hits.load(std::memory_order_relaxed),
        (unsigned long long)g_copy_fast_multi_hits.load(std::memory_order_relaxed),
        (unsigned long long)g_copy_fast_multi_spans.load(std::memory_order_relaxed),
        (unsigned long long)g_copy_fast_multi_dst_pages.load(std::memory_order_relaxed),
        (unsigned long long)g_copy_generic_hits.load(std::memory_order_relaxed),
        (unsigned long long)g_copy_spill_spans.load(std::memory_order_relaxed),
        (unsigned long long)g_copy_spill_slots.load(std::memory_order_relaxed),
        (unsigned long long)g_copy_spill_raw.load(std::memory_order_relaxed),
        (unsigned long long)g_copy_spill_miss.load(std::memory_order_relaxed),
        (unsigned long long)g_copy_spill_freeze.load(std::memory_order_relaxed),
        (unsigned long long)g_copy_spill_dup_dst.load(std::memory_order_relaxed),
        (unsigned long long)g_copy_spill_pages.load(std::memory_order_relaxed),
        (unsigned long long)g_write_fast_hits.load(std::memory_order_relaxed),
        (unsigned long long)g_write_generic_hits.load(std::memory_order_relaxed),
        (unsigned long long)g_struct_fb_cache_hits.load(std::memory_order_relaxed),
        (unsigned long long)g_struct_fb_cache_misses.load(std::memory_order_relaxed),
        (unsigned long long)reclaimed_content_batches.load(std::memory_order_relaxed),
        (unsigned long long)reclaimed_content_ticks.load(std::memory_order_relaxed),
        frequency,deadline,rt_cpu::overflow.load(std::memory_order_relaxed));
    reshade::log::message(reshade::log::level::info,msg);
}

RtPreviousReadResult RtReadPreviousView(device* dev,uint64_t heap,uint32_t index,RtPreviousView& out) noexcept {
    rt_cpu::Scope measured(rt_cpu::previous_read);
    out={};try{
        PreviousPair pair;if(!heap||!PreviousDevice(dev,pair,true))return RtPreviousReadResult::not_paired;
        auto* entry=g_heap_to_res.entry_of(heap);
        if(!entry||!entry->vis.load(std::memory_order_relaxed))return RtPreviousReadResult::heap_unavailable;
        const auto h=PreviousGetHeap(PreviousToken(heap,entry),pair);
        if(!h||index>=h->flat.size)return RtPreviousReadResult::heap_unavailable;
        RtPreviousReadResult res=RtPreviousReadResult::heap_unavailable;
        const bool ok=WithPublishedSlot(*h,pair,index,true,[&](const SlotSnapshot& snap){
            if(!snap.version()){res=RtPreviousReadResult::uncovered;return true;}
            const auto* d=snap.content?&snap.content->definition:nullptr;
            const auto* v=(d&&d->typed)?std::get_if<RtPreviousView>(&*d->typed):nullptr;
            if(!v||!PreviousViewLive(pair.device,*v)){res=RtPreviousReadResult::invalidated;return true;}
            out=*v;out.heap_generation=h->generation;out.write_generation=snap.version();
            res=RtPreviousReadResult::copied;return true;
        });
        if(!ok){out={};return RtPreviousReadResult::heap_unavailable;}
        return res;
    }catch(...){out={};return RtPreviousReadResult::uncovered;}
}
bool RtReadCopiedPreviousView(device* dev,uint64_t heap,uint32_t index,RtPreviousView& out) noexcept {
    const auto result=RtReadPreviousView(dev,heap,index,out);
    return result==RtPreviousReadResult::copied||result==RtPreviousReadResult::source_same_copy;
}

bool ReadPublishedHeapCbv(const HeapKey& key,HeapCbv& out) noexcept {
    out={};try{
        PreviousPair pair;if(!PreviousDevice(g_reshade_device,pair,false))return false;
        const auto h=PreviousGetHeap(PreviousToken(key.heap,g_heap_to_res.entry_of(key.heap)),pair);
        if(!h||key.offset>=h->flat.size)return false;
        const bool ok=WithPublishedSlot(*h,pair,key.offset,true,[&](const SlotSnapshot& snap){
            const auto* d=snap.content?&snap.content->definition:nullptr;
            const auto* cb=(d&&d->typed)?std::get_if<DescriptorCbv>(&*d->typed):nullptr;
            if(!cb)return false;
            const auto life=PreviousFindResource(reinterpret_cast<uint64_t>(cb->value.res));
            if(life.device!=pair.device||life.generation!=cb->resource_generation)return false;
            out=cb->value;return true;
        });
        if(!ok)out={};
        return ok;
    }catch(...){out={};return false;}
}
bool RtReadCopiedAuxView(device* dev,uint64_t heap,uint32_t index,RtAuxSource& out) noexcept {
    out={};try{
        PreviousPair pair;if(!heap||!PreviousDevice(dev,pair,true))return false;
        auto* entry=g_heap_to_res.entry_of(heap);
        if(!entry||!entry->vis.load(std::memory_order_relaxed))return false;
        const auto h=PreviousGetHeap(PreviousToken(heap,entry),pair);
        if(!h||index>=h->flat.size)return false;
        const bool ok=WithPublishedSlot(*h,pair,index,true,[&](const SlotSnapshot& snap){
            const auto* d=snap.content?&snap.content->definition:nullptr;
            const auto* v=(d&&d->typed)?std::get_if<RtAuxSource>(&*d->typed):nullptr;
            if(!v||!AuxiliaryViewLive(pair.device,*v))return false;
            out=*v;out.heap_generation=h->generation;out.write_generation=snap.version();return true;
        });
        if(!ok)out={};
        return ok;
    }catch(...){out={};return false;}
}
