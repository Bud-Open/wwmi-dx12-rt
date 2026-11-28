#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <windows.h>
#include <reshade.hpp>

// Small totals for the existing draw window. Main and client each own their
// module's rows; both use the QPC origin of the same existing 12s arm.
namespace rt_phase_cpu {
constexpr unsigned bin_ms=2000,bins=30,detail_seconds=12;
enum Kind : unsigned {
    runtime_wait,runtime_body,history,inbox,fence_poll,skin_dispatch,locate_index,batch_create,skin_record,
    descriptor_copy,native_copy,material,hybrid_finish,private_finish,rt_gate_select,
    gate_identify,gate_world,gate_setup,gate_vm,gate_fallback,count
};
inline constexpr const char *labels[]{
    "runtime_wait","runtime_body","history","inbox","fence_poll","skin_dispatch","locate_index","batch_create","skin_record",
    "descriptor_copy","native_copy","recorded_material","hybrid_finish","private_finish","rt_gate_select",
    "gate_identify","gate_world","gate_setup","gate_vm","gate_fallback"
};
struct Row {std::atomic<std::uint64_t> ticks{},calls{},work{};};
inline std::array<std::array<Row,count>,bins> totals;
inline std::atomic<std::uint64_t> admission{},cutoff{};
inline std::atomic<unsigned> pending{};
inline std::atomic<bool> published{};
inline std::uint64_t origin=0,frequency=0,admitted=0;
inline std::uint64_t now()noexcept{LARGE_INTEGER q{};QueryPerformanceCounter(&q);return q.QuadPart;}
inline std::uint64_t stride()noexcept{return frequency*(bin_ms/1000);}
// The caller is the one successful arm for this module.
inline void arm(std::uint64_t detail_deadline,std::uint64_t hz)noexcept{
    if(!hz||detail_deadline<=detail_seconds*hz)return;
    frequency=hz;origin=detail_deadline-detail_seconds*hz;admitted=now();
    admission.store(origin,std::memory_order_release);
}
struct Ticket {std::uint64_t start=0;Kind kind=count;};
inline Ticket begin(Kind kind,std::uint64_t t=0)noexcept{
    if(kind==count)return {};
    const auto start=admission.load(std::memory_order_acquire);
    if(!start)return {};
    const auto observed_start=(std::max)(start,admitted);
    const auto stop=start+bins*stride();
    if(t&&(t<observed_start||t>=stop))return {};
    pending.fetch_add(1,std::memory_order_seq_cst);
    if(!t)t=now();
    if(t<observed_start||t>=stop||!admission.load(std::memory_order_seq_cst)){
        pending.fetch_sub(1,std::memory_order_release);return {};
    }
    return {t,kind};
}
inline void end(Ticket &ticket,std::uint64_t t=0,std::uint64_t work=0)noexcept{
    if(!ticket.start)return;
    if(!t)t=now();
    if(!published.load(std::memory_order_acquire)){
        const auto width=stride(),closed=cutoff.load(std::memory_order_acquire);
        const auto stop=(std::min)(t,closed?closed:origin+bins*width);
        auto at=ticket.start;
        if(at<=stop&&at<(closed?closed:origin+bins*width)){
            const auto first=(at-origin)/width;
            auto &row=totals[first][ticket.kind];
            row.calls.fetch_add(1,std::memory_order_relaxed);
            if(work)row.work.fetch_add(work,std::memory_order_relaxed);
            // A scope crossing a boundary contributes its actual overlap on
            // either side. Calls and work units belong to its start bin.
            while(at<stop){
                const auto bin=(at-origin)/width;
                const auto limit=(std::min)(stop,origin+(bin+1)*width);
                totals[bin][ticket.kind].ticks.fetch_add(limit-at,std::memory_order_relaxed);
                at=limit;
            }
        }
    }
    ticket={};pending.fetch_sub(1,std::memory_order_release);
}
struct Scope {
    Ticket ticket;
    std::uint64_t work=0;
    explicit Scope(Kind kind,std::uint64_t start=0)noexcept:ticket(begin(kind,start)){}
    Scope(const Scope &)=delete;
    void add(std::uint64_t n)noexcept{work+=n;}
    ~Scope(){end(ticket,0,work);}
};
struct ConditionalScope {
    Ticket ticket;
    std::uint64_t work=0;
    ConditionalScope(bool active,Kind kind,std::uint64_t start=0)noexcept:ticket(active?begin(kind,start):Ticket{}){}
    ConditionalScope(const ConditionalScope &)=delete;
    void add(std::uint64_t n)noexcept{work+=n;}
    void finish()noexcept{end(ticket,0,work);}
    ~ConditionalScope(){end(ticket,0,work);}
};
inline thread_local bool gate_timing_active=false;
inline bool gate_active()noexcept{return gate_timing_active;}
struct GateActiveGuard {
    bool prev;
    explicit GateActiveGuard(bool active)noexcept:prev(gate_timing_active){gate_timing_active=active;}
    GateActiveGuard(const GateActiveGuard &)=delete;
    ~GateActiveGuard(){gate_timing_active=prev;}
};
// Normal close is fixed at the end of the existing draw window, not the
// next Present. Early stop uses the actual QPC boundary.
inline void close(bool stopping)noexcept{
    const auto start=admission.load(std::memory_order_acquire);
    if(!start)return;
    const auto natural=start+bins*stride(),t=now();
    if(!stopping&&t<natural)return;
    cutoff.store((std::min)(t,natural),std::memory_order_release);
    admission.store(0,std::memory_order_seq_cst);
}
inline bool waiting_report()noexcept{
    return !published.load(std::memory_order_relaxed)&&
        (cutoff.load(std::memory_order_acquire)||admission.load(std::memory_order_acquire));
}
inline void report(const char *owner,std::uint64_t start_ms,Kind first,Kind last,bool force,const unsigned *presents=nullptr)noexcept{
    const auto stop=cutoff.load(std::memory_order_acquire);
    if(!stop||published.load(std::memory_order_relaxed))return;
    const auto active=pending.load(std::memory_order_seq_cst);
    if(active&&!force)return;
    if(published.exchange(true,std::memory_order_acq_rel))return;
    using U=unsigned long long;
    const auto observed_start=(std::max)(origin,admitted);
    const auto width=stride();
    const auto window_ticks=stop>origin?stop-origin:0;
    const auto covered_ticks=stop>observed_start?stop-observed_start:0;
    const auto window_ms=window_ticks*1000/frequency;
    const auto covered_ms=covered_ticks*1000/frequency;
    const auto first_covered_bin=covered_ticks?int((observed_start-origin)/width):-1;
    const double ms=1000.0/double(frequency);
    char line[2048];
    std::snprintf(line,sizeof(line),
        "RT-CPU-PHASE begin owner=%s start_ms=%llu window_ms=%llu covered_ms=%llu first_covered_bin=%d origin_qpc=%llu admitted_qpc=%llu frequency=%llu bin_ms=%u pending_scopes=%u timing=wall_qpc_inclusive",
        owner,U(start_ms),U(window_ms),U(covered_ms),first_covered_bin,U(origin),U(admitted),U(frequency),bin_ms,active);
    reshade::log::message(reshade::log::level::info,line);
    for(unsigned i=0;i<bins&&std::uint64_t(i)*width<window_ticks;++i){
        const auto offset=std::uint64_t(i)*bin_ms;
        const auto bin_begin=origin+std::uint64_t(i)*width;
        const auto a=(std::max)(bin_begin,observed_start),b=(std::min)(bin_begin+width,stop);
        const auto seen=b>a?b-a:0;
        auto used=std::snprintf(line,sizeof(line),
            "RT-CPU-PHASE bin owner=%s bin=%u begin_ms=%llu end_ms=%llu covered_ms=%llu coverage=%s",
            owner,i,U(start_ms+offset),U(start_ms+(std::min)(window_ms,offset+bin_ms)),
            U(seen*1000/frequency),seen==width?"full":seen?"partial":"unobserved");
        if(presents)used+=std::snprintf(line+used,sizeof(line)-used," presents=%u",presents[i]);
        for(unsigned k=first;k<last;++k){
            const auto &r=totals[i][k];const auto label=labels[k];
            used+=std::snprintf(line+used,sizeof(line)-used," %s_ms=%.3f %s_calls=%llu %s_work=%llu",
                label,double(r.ticks.load(std::memory_order_relaxed))*ms,label,U(r.calls.load(std::memory_order_relaxed)),label,U(r.work.load(std::memory_order_relaxed)));
        }
        reshade::log::message(reshade::log::level::info,line);
    }
}
}
