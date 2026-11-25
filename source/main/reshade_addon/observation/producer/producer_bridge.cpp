// SPDX-License-Identifier: GPL-3.0-only
#define WWMI_PRODUCER_BUILD
#include "producer_trace.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

static_assert(std::is_trivially_copyable<WWMI_ProducerRecord_v1>::value,"POD wire ABI");
namespace {
std::atomic<uint64_t> active{0}, failures{0}, recursive{0}, rejected{0};
thread_local unsigned callback_depth=0;
constexpr size_t kMaximumPending=65536;
struct Pending {
    WWMI_ProducerInput_v1 input{};
    uint64_t thread=0, parent_session=0, parent_call=0;
};
struct State {
    std::mutex mutex;
    std::condition_variable drained;
    WWMI_Producer_Callback_v1 callback=nullptr;
    void* user=nullptr;
    bool closing=false;
    size_t in_flight=0;
    uint64_t next_session=0, session=0, sequence=0, next_call=0, closed_pending=0;
    std::unordered_map<uint64_t,Pending> pending;
};
State& state() { static State* const value=new State;return *value; }
bool paired(uint32_t kind) { return kind==1||kind==2||kind==4; }
bool valid(const WWMI_ProducerInput_v1& x) {
    if(x.struct_size!=sizeof(x)||x.abi_version!=1||x.kind<1||x.kind>13||
       x.provenance<1||x.provenance>4||(x.flags&~WWMI_PRODUCER_FLAG_ALL)||
       (x.known_fields&~WWMI_PRODUCER_KNOWN_ALL)||x.semantic_bytes>63) return false;
    for(uint32_t i=0;i<x.semantic_bytes;++i) if(!x.semantic_name[i]) return false;
    for(uint32_t i=x.semantic_bytes;i<64;++i) if(x.semantic_name[i]) return false;
    auto range=[](uint64_t va,uint64_t offset,uint64_t bytes) {
        return offset<=UINT64_MAX-va&&bytes<=UINT64_MAX-va-offset;
    };
    if((x.known_fields&WWMI_PRODUCER_KNOWN_RESOURCE_RANGE)&&
       (!(x.known_fields&WWMI_PRODUCER_KNOWN_RESOURCE)||!x.resource||!x.resource_va||!x.resource_bytes||
        !range(x.resource_va,x.resource_offset,x.resource_bytes))) return false;
    if((x.known_fields&WWMI_PRODUCER_KNOWN_AUX_RANGE)&&
       (!(x.known_fields&WWMI_PRODUCER_KNOWN_AUX_RESOURCE)||!x.aux_resource||!x.aux_va||!x.aux_bytes||
        !range(x.aux_va,x.aux_offset,x.aux_bytes))) return false;
    if(x.kind==WWMI_PRODUCER_NATIVE_DISPATCH &&
       (x.provenance!=WWMI_PRODUCER_PROVENANCE_NATIVE_CALL||
        (x.known_fields&(WWMI_PRODUCER_KNOWN_LIST|WWMI_PRODUCER_KNOWN_DISPATCH))!=
        (WWMI_PRODUCER_KNOWN_LIST|WWMI_PRODUCER_KNOWN_DISPATCH))) return false;
    if(x.kind==WWMI_PRODUCER_VM_DISPATCH_SUMMARY&&x.provenance!=WWMI_PRODUCER_PROVENANCE_CANDIDATE_HINT) return false;
    return true;
}
void clock_metadata(WWMI_ProducerRecord_v1& r) {
#if defined(_WIN32)
    LARGE_INTEGER tick{},frequency{};QueryPerformanceCounter(&tick);QueryPerformanceFrequency(&frequency);
    r.timestamp_ticks=static_cast<uint64_t>(tick.QuadPart);
    r.timestamp_frequency=static_cast<uint64_t>(frequency.QuadPart);r.clock_domain=1;r.thread_id=GetCurrentThreadId();
#else
    r.timestamp_ticks=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    r.timestamp_frequency=1000000000;r.clock_domain=2;
    r.thread_id=static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}
void invoke_cpp(WWMI_Producer_Callback_v1 callback,const WWMI_ProducerRecord_v1* r,void* user) noexcept {
    try { callback(r,user); } catch(...) { failures.fetch_add(1,std::memory_order_relaxed); }
}
void invoke(WWMI_Producer_Callback_v1 callback,const WWMI_ProducerRecord_v1* r,void* user) noexcept {
#if defined(_MSC_VER)
    __try { invoke_cpp(callback,r,user); }
    __except(EXCEPTION_EXECUTE_HANDLER) { failures.fetch_add(1,std::memory_order_relaxed); }
#else
    invoke_cpp(callback,r,user);
#endif
}
bool identity_matches(const WWMI_ProducerInput_v1& a,const WWMI_ProducerInput_v1& b) {
    if(a.kind!=b.kind||a.provenance!=b.provenance||a.source_site!=b.source_site||
       a.native_command_list!=b.native_command_list) return false;
    if(a.kind==WWMI_PRODUCER_NATIVE_DISPATCH && (a.dispatch_x!=b.dispatch_x||a.dispatch_y!=b.dispatch_y||
       a.dispatch_z!=b.dispatch_z||a.pipeline!=b.pipeline||a.root_signature!=b.root_signature)) return false;
    return true;
}
uint64_t emit(uint32_t phase,const WWMI_ProducerInput_v1& x,wwmi::producer::Token token,
              wwmi::producer::Token parent,wwmi::producer::Token* result) noexcept {
    const auto session=active.load(std::memory_order_acquire);if(!session) return 0;
    if(callback_depth) { recursive.fetch_add(1,std::memory_order_relaxed);return 0; }
    if(phase==WWMI_PRODUCER_PHASE_RETURN&&(!token.session||!token.call||token.session!=session)) return 0;
    try {
        if(!valid(x)||(phase==0?paired(x.kind):!paired(x.kind))) {
            rejected.fetch_add(1,std::memory_order_relaxed);return 0;
        }
        State& s=state();WWMI_ProducerRecord_v1 r{};WWMI_Producer_Callback_v1 callback;void* user;
        clock_metadata(r);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if(s.closing||!s.callback||s.session!=session) return 0;
            if(s.sequence==UINT64_MAX||s.next_call==UINT64_MAX) { ++rejected;return 0; }
            if(phase==WWMI_PRODUCER_PHASE_RETURN) {
                auto it=s.pending.find(token.call);
                if(it==s.pending.end()||it->second.thread!=r.thread_id||!identity_matches(it->second.input,x)) { ++rejected;return 0; }
                for(const auto& item:s.pending) if(item.second.parent_call==token.call) { ++rejected;return 0; }
                parent={it->second.parent_session,it->second.parent_call};
                s.pending.erase(it);
            } else {
                if(bool(parent.session)!=bool(parent.call)||(parent.session&&parent.session!=session)) { ++rejected;return 0; }
                if(parent.call) {
                    const auto it=s.pending.find(parent.call);
                    if(it==s.pending.end()||it->second.thread!=r.thread_id) { ++rejected;return 0; }
                }
                if(phase==WWMI_PRODUCER_PHASE_BEGIN) {
                    if(s.pending.size()>=kMaximumPending) { ++rejected;return 0; }
                    token={session,++s.next_call};
                    s.pending.emplace(token.call,Pending{x,r.thread_id,parent.session,parent.call});
                }
            }
            r.struct_size=sizeof(r);r.abi_version=1;r.phase=phase;r.session_id=session;r.sequence=++s.sequence;
            r.call_id=phase?token.call:0;r.parent_session=parent.session;r.parent_call=parent.call;
            r.unproven_fields=WWMI_PRODUCER_UNPROVEN_ALL;
            r.callback_failures_total=failures.load(std::memory_order_relaxed);
            r.suppressed_reentrant_total=recursive.load(std::memory_order_relaxed);
            r.observation_failures_total=rejected.load(std::memory_order_relaxed);r.input=x;
            callback=s.callback;user=s.user;++s.in_flight;
        }
        ++callback_depth;invoke(callback,&r,user);--callback_depth;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            --s.in_flight;if(!s.in_flight)s.drained.notify_all();
        }
        if(result&&phase==WWMI_PRODUCER_PHASE_BEGIN)*result=token;
        return r.sequence;
    } catch(...) { rejected.fetch_add(1,std::memory_order_relaxed);return 0; }
}
}
extern "C" int32_t WWMI_PRODUCER_CALL WWMI_ProducerTrace_Register_v1(WWMI_Producer_Callback_v1 callback,void* user) {
    if(!callback)return -1;
    if(callback_depth)return -3;
    try {
        auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);
        if(s.closing)return -2;
        if(s.callback)return s.callback==callback&&s.user==user?0:-2;
        if(s.next_session==UINT64_MAX)return -2;
        s.pending.clear();s.sequence=s.next_call=s.closed_pending=0;
        failures=0;recursive=0;rejected=0;s.callback=callback;s.user=user;s.session=++s.next_session;
        active.store(s.session,std::memory_order_release);return 0;
    } catch(...) { return -2; }
}
extern "C" int32_t WWMI_PRODUCER_CALL WWMI_ProducerTrace_Unregister_v1(WWMI_Producer_Callback_v1 callback,void* user) {
    if(!callback)return -1;
    if(callback_depth)return -3;
    try {
        auto& s=state();std::unique_lock<std::mutex> lock(s.mutex);
        if(s.closing)return -2;
        if(s.callback!=callback||s.user!=user)return -4;
        s.closing=true;active.store(0,std::memory_order_release);
        s.drained.wait(lock,[&]{return s.in_flight==0;});
        s.closed_pending=s.pending.size();s.pending.clear();s.callback=nullptr;s.user=nullptr;s.session=0;s.closing=false;
        return 0;
    } catch(...) { return -2; }
}
extern "C" int32_t WWMI_PRODUCER_CALL WWMI_ProducerTrace_GetStatus_v1(WWMI_ProducerStatus_v1* output) {
    if(!output||output->struct_size!=sizeof(*output)||output->abi_version!=1)return -1;
    try {
        auto& s=state();std::lock_guard<std::mutex> lock(s.mutex);WWMI_ProducerStatus_v1 r{};
        r.struct_size=sizeof(r);r.abi_version=1;r.last_session=s.next_session;r.last_sequence=s.sequence;
        r.callback_failures_total=failures.load(std::memory_order_relaxed);
        r.suppressed_reentrant_total=recursive.load(std::memory_order_relaxed);
        r.observation_failures_total=rejected.load(std::memory_order_relaxed);
        r.pending_calls=s.callback?s.pending.size():s.closed_pending;r.active=s.callback&&!s.closing;r.draining=s.closing;
        *output=r;return 0;
    } catch(...) { return -2; }
}
namespace wwmi::producer {
uint64_t SnapshotSession() noexcept { return active.load(std::memory_order_acquire); }
Token Begin(const WWMI_ProducerInput_v1& input,Token parent) noexcept { Token r{};emit(1,input,{},parent,&r);return r; }
void End(Token token,const WWMI_ProducerInput_v1& input) noexcept { if(token.session&&token.call)emit(2,input,token,{},nullptr); }
uint64_t Event(const WWMI_ProducerInput_v1& input,Token parent) noexcept { return emit(0,input,{},parent,nullptr); }
void ObservationFailed() noexcept { if(active.load(std::memory_order_acquire))rejected.fetch_add(1,std::memory_order_relaxed); }
}
