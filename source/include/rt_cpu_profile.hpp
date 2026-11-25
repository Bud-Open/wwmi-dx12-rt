#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <windows.h>
#include <reshade.hpp>
#include "rt_cpu_phase.hpp"

// CPU-only, one bounded window. Adapted from the existing ProfScope's QPC,
// per-thread slots and nested self-time accounting; does not enable GPU timers.
namespace rt_cpu {
enum Kind {
    desc_update,desc_copy,previous_update,previous_copy,previous_wait,previous_r32,previous_aux,previous_read,
    runtime_wait,runtime_body,inbox,collect,fence_poll,history,locate,locate_hit,native_select,skin_dispatch,
    pose_assemble,assets,batch_create,skin_record,submit_pre,submit_post,signal,resource_life,present_wait,engine_tick,present_submit,
    api_identify,api_palette,api_surface,api_previous,blas_callback,queue_callback,execute_callback,other_callback,
    locate_index,author_default_publish,author_default_submit,author_bootstrap_submit,author_local_acquire,author_cross_acquire,count
};
inline constexpr const char *names[]{
    "main.desc_update","main.desc_copy","main.previous_update","main.previous_copy","main.previous_mutex_wait","main.previous_capture_views","main.previous_apply_views","main.previous_read",
    "client.runtime_mutex_wait","client.runtime_body","client.buffer_inbox","client.collect","client.fence_poll","client.history","client.locate_scan","client.locate_cache_hit","client.native_select","client.skin_dispatch",
    "client.pose_assemble","client.assets_create","client.batch_create","client.skin_record","client.submit_pre","client.submit_post","client.queue_signal","main.resource_life","main.present_wait","main.engine_tick","main.present_submit",
    "client.api_identify","client.api_palette","client.api_surface","client.api_previous","client.forward_blas","client.forward_queue","client.execute","client.other_callback",
    "client.locate_index","client.author_default_publish","client.author_default_submitted_prefix","client.author_bootstrap_submitted_prefix",
    "client.author_local_prefix_acquire","client.author_cross_recording_ready_acquire"
};
struct Row {std::atomic<std::uint64_t> calls{},ticks{},self{},maximum{},work{};};
struct Lane {std::atomic<DWORD> tid{};std::atomic<unsigned> active{},current{};std::atomic<std::uint64_t> entered{};std::array<Row,count> rows;};
inline std::array<Lane,64> lanes;
inline std::atomic<std::uint64_t> deadline{};
inline std::atomic<unsigned> phase{}; // idle, measuring, closed, logged
inline std::atomic<unsigned> overflow{};
inline std::atomic<std::uint64_t> frequency{};
inline std::uint64_t now(){LARGE_INTEGER q{};QueryPerformanceCounter(&q);return q.QuadPart;}
inline void observe(std::uint64_t end){
    if(!end||deadline.load(std::memory_order_relaxed))return;
    LARGE_INTEGER q{};QueryPerformanceFrequency(&q);frequency.store(q.QuadPart,std::memory_order_relaxed);
    std::uint64_t empty=0;
    if(deadline.compare_exchange_strong(empty,end,std::memory_order_release)){
        rt_phase_cpu::arm(end,q.QuadPart);
        phase.store(1,std::memory_order_release);
    }
}
inline int lane(){
    static thread_local int slot=-2;
    if(slot!=-2)return slot;
    const DWORD tid=GetCurrentThreadId();
    for(unsigned i=0;i<lanes.size();++i){DWORD empty=0;
        if(lanes[i].tid.load(std::memory_order_relaxed)==tid||lanes[i].tid.compare_exchange_strong(empty,tid,std::memory_order_relaxed))return slot=int(i);
    }
    overflow.fetch_add(1,std::memory_order_relaxed);return slot=-1;
}
struct Scope;
inline thread_local Scope *current=nullptr;
inline rt_phase_cpu::Kind phase_kind(Kind kind)noexcept{
    switch(kind){
    case runtime_wait:return rt_phase_cpu::runtime_wait;
    case runtime_body:return rt_phase_cpu::runtime_body;
    case history:return rt_phase_cpu::history;
    case inbox:return rt_phase_cpu::inbox;
    case fence_poll:return rt_phase_cpu::fence_poll;
    case skin_dispatch:return rt_phase_cpu::skin_dispatch;
    case locate_index:return rt_phase_cpu::locate_index;
    case batch_create:return rt_phase_cpu::batch_create;
    case skin_record:return rt_phase_cpu::skin_record;
    default:return rt_phase_cpu::count;
    }
}
struct Scope {
    int kind=-1,slot=-1;std::uint64_t start=0,child=0,work=0;Scope *previous=nullptr;
    rt_phase_cpu::Ticket segment;
    Scope()=default;
    explicit Scope(Kind k){begin(k);}
    void begin(Kind k){
        const auto segmented=phase_kind(k);
        if(phase.load(std::memory_order_relaxed)!=1){segment=rt_phase_cpu::begin(segmented);return;}
        slot=lane();if(slot<0){segment=rt_phase_cpu::begin(segmented);return;}
        auto &l=lanes[slot];l.active.fetch_add(1,std::memory_order_acquire);
        if(phase.load(std::memory_order_acquire)!=1){
            l.active.fetch_sub(1,std::memory_order_release);segment=rt_phase_cpu::begin(segmented);return;
        }
        const auto t=now(),end=deadline.load(std::memory_order_acquire);
        segment=rt_phase_cpu::begin(segmented,t);
        if(t>=end){phase.store(2,std::memory_order_release);l.active.fetch_sub(1,std::memory_order_release);return;}
        kind=k;start=t;previous=current;current=this;
        l.current.store(k,std::memory_order_relaxed);l.entered.store(t,std::memory_order_relaxed);
    }
    Scope(const Scope &)=delete;
    void add(std::uint64_t n=1){work+=n;}
    void end(){
        if(kind<0&&!segment.start)return;
        const auto observed=now();
        if(kind<0){rt_phase_cpu::end(segment,observed,work);return;}
        const auto stop=(std::min)(observed,deadline.load(std::memory_order_acquire));
        const auto elapsed=stop>start?stop-start:0;
        auto &l=lanes[slot];auto &r=l.rows[kind];
        r.calls.fetch_add(1,std::memory_order_relaxed);r.ticks.fetch_add(elapsed,std::memory_order_relaxed);r.self.fetch_add(elapsed>child?elapsed-child:0,std::memory_order_relaxed);r.work.fetch_add(work,std::memory_order_relaxed);
        auto maximum=r.maximum.load(std::memory_order_relaxed);while(maximum<elapsed&&!r.maximum.compare_exchange_weak(maximum,elapsed,std::memory_order_relaxed)){}
        if(current==this){current=previous;if(previous)previous->child+=elapsed;}
        l.current.store(previous?unsigned(previous->kind):unsigned(count),std::memory_order_relaxed);l.entered.store(previous?previous->start:0,std::memory_order_relaxed);l.active.fetch_sub(1,std::memory_order_release);kind=-1;
        rt_phase_cpu::end(segment,observed,work);
    }
    ~Scope(){end();}
};
inline void hit(Kind kind,std::uint64_t work=1)noexcept{
    if(phase.load(std::memory_order_relaxed)!=1)return;
    const int s=lane();if(s<0)return;
    auto &l=lanes[s];
    l.active.fetch_add(1,std::memory_order_acquire);
    if(phase.load(std::memory_order_acquire)==1&&now()<deadline.load(std::memory_order_acquire)){
        l.rows[kind].calls.fetch_add(1,std::memory_order_relaxed);
        l.rows[kind].work.fetch_add(work,std::memory_order_relaxed);
    }
    l.active.fetch_sub(1,std::memory_order_release);
}
inline void finish(const char *owner,bool force=false){
    auto p=phase.load(std::memory_order_acquire);if(!p||p==3)return;
    if(p==1){if(!force&&now()<deadline.load(std::memory_order_acquire))return;phase.store(2,std::memory_order_release);}
    unsigned expected=2;if(!phase.compare_exchange_strong(expected,3,std::memory_order_acq_rel))return;
    const double ms=1000.0/double(frequency.load());constexpr unsigned budget=192;unsigned pending=0,rows=0;
    char msg[400];
    std::snprintf(msg,sizeof(msg),"RT-CPU begin owner=%s deadline_qpc=%llu frequency=%llu clock=wall_qpc gpu_queries=0",owner,(unsigned long long)deadline.load(),(unsigned long long)frequency.load());
    reshade::log::message(reshade::log::level::info,msg);
    struct Snapshot {DWORD tid;unsigned kind,pending;std::uint64_t n,ticks,self,maximum,work;};
    std::vector<Snapshot> values;
    struct Total {std::uint64_t n=0,ticks=0,self=0,maximum=0,work=0;};
    std::array<Total,count> totals{};
    for(const auto &l:lanes){const auto tid=l.tid.load(std::memory_order_acquire);if(!tid)continue;
        const auto active=l.active.load(std::memory_order_acquire);
        if(active){++pending;std::snprintf(msg,sizeof(msg),"RT-CPU pending owner=%s tid=%lu scopes=%u current_kind=%u entered_qpc=%llu",owner,(unsigned long)tid,active,l.current.load(),(unsigned long long)l.entered.load());reshade::log::message(reshade::log::level::info,msg);}
        for(unsigned i=0;i<count;++i){
            const auto &r=l.rows[i];const auto n=r.calls.load(std::memory_order_relaxed);if(!n)continue;
            const auto &v=values.emplace_back(Snapshot{tid,i,unsigned(active!=0),n,r.ticks.load(),r.self.load(),r.maximum.load(),r.work.load()});
            auto &t=totals[i];t.n+=v.n;t.ticks+=v.ticks;t.self+=v.self;t.maximum=(std::max)(t.maximum,v.maximum);t.work+=v.work;
        }
    }
    // Totals cover the sampled lanes before the per-thread row budget.
    // Outstanding or overflow lanes are visible on every total.
    const auto overflowed=overflow.load(std::memory_order_relaxed);
    for(unsigned i=0;i<count;++i){
        const auto &t=totals[i];if(!t.n&&i<author_default_publish)continue;
        std::snprintf(msg,sizeof(msg),"RT-CPU total owner=%s kind=%s calls=%llu wall_ms=%.3f self_ms=%.3f max_ms=%.3f work=%llu pending_threads=%u overflow_threads=%u",
            owner,names[i],(unsigned long long)t.n,t.ticks*ms,t.self*ms,t.maximum*ms,(unsigned long long)t.work,pending,overflowed);
        reshade::log::message(reshade::log::level::info,msg);
    }
    std::sort(values.begin(),values.end(),[](const auto &a,const auto &b){return a.self>b.self;});
    for(const auto &v:values){if(rows==budget)break;++rows;
        std::snprintf(msg,sizeof(msg),"RT-CPU row owner=%s tid=%lu kind=%s calls=%llu wall_ms=%.3f self_ms=%.3f max_ms=%.3f work=%llu pending_thread=%u",owner,(unsigned long)v.tid,names[v.kind],(unsigned long long)v.n,v.ticks*ms,v.self*ms,v.maximum*ms,(unsigned long long)v.work,v.pending);
        reshade::log::message(reshade::log::level::info,msg);
    }
    std::snprintf(msg,sizeof(msg),"RT-CPU end owner=%s rows=%u pending_threads=%u overflow_threads=%u row_budget_exhausted=%u",owner,rows,pending,overflowed,unsigned(values.size()>rows));
    reshade::log::message(reshade::log::level::info,msg);
}
}
