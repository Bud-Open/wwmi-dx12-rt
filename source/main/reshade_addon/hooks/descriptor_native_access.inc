// Only our raw descriptor edits need exclusion. Game calls share admission,
// including a common source; the D3D12 caller remains responsible for legal
// app/app ranges. Stripes use actual CPU addresses, not optional shadow heaps.
namespace {
constexpr unsigned descriptor_page_shift=12;
constexpr unsigned descriptor_stripes=256;
constexpr uint32_t raw_descriptor_bit=uint32_t{1}<<31;

struct alignas(64) DescriptorStripe { std::atomic<uint32_t> state{0}; };
std::array<DescriptorStripe,descriptor_stripes> descriptor_stripes_state;
std::atomic<WWMI_DescriptorRawAfter_v1> descriptor_raw_after{nullptr};

// Per-thread private reader lanes for asymmetric fast admission and
// epoch-safe raw-heap / seqlock descriptor reads during OnNativeDescriptorCopy.
// Low 32 bits: copy_depth (protects TLS PreviousHeap* & seqlock DescriptorContent*).
// High 32 bits: raw_fast (fast-path exclusion against RawDescriptorAdmission).
constexpr unsigned copy_lane_count=128;
constexpr uint64_t kCopyDepthInc=1ull;
constexpr uint64_t kRawFastInc=1ull<<32;
constexpr uint64_t kCopyDepthMask=0xFFFFFFFFull;
constexpr uint64_t kRawFastMask=0xFFFFFFFF00000000ull;

struct alignas(64) CopyLane {
    std::atomic<uint64_t> state{0};
    std::atomic<bool> claimed{false};
};
std::array<CopyLane,copy_lane_count> copy_lanes;
std::atomic<uint32_t> raw_edits_pending{0};
std::atomic<uint32_t> copy_lane_waiters{0};
std::atomic<uint64_t> previous_retire_generation{1};

struct ThreadCopyLane {
    CopyLane* lane=nullptr;
    uint32_t copy_depth=0;
    uint32_t fast_raw_depth=0;
    ThreadCopyLane() noexcept {
        const uintptr_t seed=reinterpret_cast<uintptr_t>(this);
        const unsigned start=unsigned((seed>>6)^(seed>>13))&(copy_lane_count-1);
        for(unsigned i=0;i<copy_lane_count;++i){
            auto& cand=copy_lanes[(start+i)&(copy_lane_count-1)];
            bool expected=false;
            if(cand.claimed.compare_exchange_strong(expected,true,std::memory_order_acq_rel)){
                lane=&cand;
                break;
            }
        }
    }
    ~ThreadCopyLane() {
        if(lane)lane->claimed.store(false,std::memory_order_release);
    }
};
inline ThreadCopyLane& CurrentCopyLane() noexcept {
    static thread_local ThreadCopyLane tls;
    return tls;
}
void DrainCopyLanes(uint64_t mask) noexcept {
    copy_lane_waiters.fetch_add(1,std::memory_order_seq_cst);
    for(auto& lane:copy_lanes){
        auto v=lane.state.load(std::memory_order_seq_cst);
        while(v&mask){
            lane.state.wait(v,std::memory_order_acquire);
            v=lane.state.load(std::memory_order_seq_cst);
        }
    }
    copy_lane_waiters.fetch_sub(1,std::memory_order_release);
}
struct CopyLaneGuard {
    ThreadCopyLane* tls=nullptr;
    bool held_copy=false;
    bool held_fast_raw=false;
    bool outermost=false;
    uint64_t retire_gen=0;

    CopyLaneGuard() noexcept {
        auto& t=CurrentCopyLane();
        if(!t.lane)return;
        tls=&t;
        if(t.copy_depth==0){
            outermost=true;
            t.lane->state.fetch_add(kCopyDepthInc|kRawFastInc,std::memory_order_seq_cst);
            t.copy_depth=1;
            held_copy=true;
            if(raw_edits_pending.load(std::memory_order_seq_cst)==0){
                t.fast_raw_depth=1;
                held_fast_raw=true;
            }else{
                t.lane->state.fetch_sub(kRawFastInc,std::memory_order_seq_cst);
                if(copy_lane_waiters.load(std::memory_order_seq_cst)!=0)
                    t.lane->state.notify_all();
            }
        }else{
            ++t.copy_depth;
            held_copy=true;
            if(t.fast_raw_depth>0){
                ++t.fast_raw_depth;
                held_fast_raw=true;
            }
        }
        retire_gen=previous_retire_generation.load(std::memory_order_acquire);
    }
    CopyLaneGuard(const CopyLaneGuard&)=delete;
    bool has_copy_depth() const noexcept {return held_copy;}
    bool has_fast_raw() const noexcept {return held_fast_raw;}
    bool is_outermost() const noexcept {return outermost;}
    ~CopyLaneGuard() {
        if(!held_copy)return;
        uint64_t sub=0;
        if(held_fast_raw&&--tls->fast_raw_depth==0)sub|=kRawFastInc;
        if(--tls->copy_depth==0)sub|=kCopyDepthInc;
        if(sub){
            tls->lane->state.fetch_sub(sub,std::memory_order_seq_cst);
            if(copy_lane_waiters.load(std::memory_order_seq_cst)!=0)
                tls->lane->state.notify_all();
        }
    }
};

unsigned DescriptorStripeOf(uint64_t cpu) noexcept {
    return unsigned(cpu>>descriptor_page_shift)&(descriptor_stripes-1);
}
struct DescriptorPages {
    std::array<uint64_t,descriptor_stripes/64> bits{};
    void mark(unsigned s) noexcept {bits[s/64]|=uint64_t{1}<<(s%64);}
    void add(uint64_t cpu,uint32_t count,uint32_t inc) noexcept {
        if(!cpu||!count)return;
        const uint64_t delta=uint64_t(count-1)*inc;
        if((count>1&&!inc)||delta>UINT64_MAX-cpu){
            bits.fill(UINT64_MAX);return;
        }
        const auto first=cpu>>descriptor_page_shift,last=(cpu+delta)>>descriptor_page_shift;
        if(last-first+1>=descriptor_stripes){bits.fill(UINT64_MAX);return;}
        for(auto page=first;page<=last;++page)mark(unsigned(page)&(descriptor_stripes-1));
    }
    template<class F> unsigned each(F&& f) const noexcept {
        for(unsigned i=0;i<bits.size();++i)
            for(auto word=bits[i];word;word&=word-1){
                const unsigned s=i*64+std::countr_zero(word);
                if(!f(s))return s;
            }
        return descriptor_stripes;
    }
};
void WaitForDescriptorRaw(unsigned stripe) noexcept {
    auto& state=descriptor_stripes_state[stripe].state;
    for(auto v=state.load(std::memory_order_acquire);v&raw_descriptor_bit;v=state.load(std::memory_order_acquire))
        state.wait(v,std::memory_order_acquire);
}

struct AppDescriptorAdmission {
    DescriptorPages pages,held;
    AppDescriptorAdmission()=default;
    AppDescriptorAdmission(const AppDescriptorAdmission&)=delete;
    template<class OnBlocked>
    void enter(OnBlocked&& on_blocked) noexcept {
        for(;;){
            const auto blocked=pages.each([&](unsigned i){
                auto& state=descriptor_stripes_state[i].state;
                auto v=state.load(std::memory_order_relaxed);
                while(!(v&raw_descriptor_bit))
                    if(state.compare_exchange_weak(v,v+1,std::memory_order_acquire,std::memory_order_relaxed)){
                        held.mark(i);return true;
                    }
                return false;
            });
            if(blocked==descriptor_stripes)return;
            // A raw edit may need both its source and destination. Do not
            // wait on that edit while retaining any partial app admissions.
            on_blocked();release();WaitForDescriptorRaw(blocked);
        }
    }
    void enter() noexcept {enter([]() noexcept {});}
    void release() noexcept {
        held.each([](unsigned i){
            auto& state=descriptor_stripes_state[i].state;
            if(state.fetch_sub(1,std::memory_order_release)==(raw_descriptor_bit|1))state.notify_all();
            return true;
        });
        held={};
    }
    ~AppDescriptorAdmission(){release();}
};
void AddAppDescriptorSpan(void* context,const WWMI_DescriptorSpan_v1* span) noexcept {
    auto& pages=*static_cast<DescriptorPages*>(context);
    const auto inc=span->destination.increment?span->destination.increment:span->source.increment;
    pages.add(span->destination.cpu,span->count,inc);
    pages.add(span->source.cpu,span->count,inc);
}

struct RawDescriptorAdmission {
    unsigned first=descriptor_stripes,second=descriptor_stripes;
    WWMI_DescriptorRawAfter_v1 after=nullptr;
    bool raw_pending=false;
    RawDescriptorAdmission(uint64_t destination,uint64_t source) noexcept {
        if(!destination||!source||!descriptor_raw_after.load(std::memory_order_seq_cst))return;
        raw_edits_pending.fetch_add(1,std::memory_order_seq_cst);
        raw_pending=true;
        const auto a=DescriptorStripeOf(destination),b=DescriptorStripeOf(source);
        first=(std::min)(a,b);second=a==b?descriptor_stripes:(std::max)(a,b);
        for(;;){
            if(!claim(first)){WaitForDescriptorRaw(first);continue;}
            if(second==descriptor_stripes||claim(second))break;
            unclaim(first);WaitForDescriptorRaw(second);
        }
        // Mark both stripes before waiting for any app calls already in
        // flight. New app calls on these stripes cannot starve this edit.
        drain(first);if(second!=descriptor_stripes)drain(second);
        // Also drain any fast-lane app copies that entered before raw_edits_pending rose.
        DrainCopyLanes(kRawFastMask);
        // Paired with Stop's sequentially consistent close/drain. Save the
        // host callback so an already admitted edit can finish its AS erase.
        after=descriptor_raw_after.load(std::memory_order_seq_cst);
    }
    RawDescriptorAdmission(const RawDescriptorAdmission&)=delete;
    explicit operator bool() const noexcept {return after!=nullptr;}
    ~RawDescriptorAdmission(){
        if(second!=descriptor_stripes)unclaim(second);
        if(first!=descriptor_stripes)unclaim(first);
        if(raw_pending)raw_edits_pending.fetch_sub(1,std::memory_order_release);
    }
private:
    static bool claim(unsigned i) noexcept {
        return !(descriptor_stripes_state[i].state.fetch_or(raw_descriptor_bit,std::memory_order_seq_cst)&raw_descriptor_bit);
    }
    static void unclaim(unsigned i) noexcept {
        auto& state=descriptor_stripes_state[i].state;
        state.fetch_and(~raw_descriptor_bit,std::memory_order_release);
        state.notify_all();
    }
    static void drain(unsigned i) noexcept {
        auto& state=descriptor_stripes_state[i].state;
        for(auto v=state.load(std::memory_order_acquire);v!=raw_descriptor_bit;v=state.load(std::memory_order_acquire))
            state.wait(v,std::memory_order_acquire);
    }
};
void CloseRawDescriptorAdmission() noexcept {
    descriptor_raw_after.store(nullptr,std::memory_order_seq_cst);
    for(unsigned i=0;i<descriptor_stripes;++i)
        if(descriptor_stripes_state[i].state.load(std::memory_order_seq_cst)&raw_descriptor_bit)
            WaitForDescriptorRaw(i);
}
}
