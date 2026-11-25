// SPDX-License-Identifier: GPL-3.0-only
#define WWMI_BINDING_TRACE_BUILD
#include "wwmi_binding_trace.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static_assert(sizeof(WWMI_BindingView_v1)==16,"binding view ABI");
static_assert(sizeof(WWMI_BindingDraw_v1)==24,"draw ABI");
static_assert(std::is_trivially_copyable<WWMI_BindingRecord_v1>::value,"POD ABI");
namespace {
std::atomic<uint64_t> active{0};
std::atomic<uint64_t> failures{0}, reentrant{0}, observation_failures{0};
thread_local unsigned callback_depth=0;
constexpr size_t kMaximumLists=65536, kMaximumPending=65536;
struct List {
    uint64_t creation=0,generation=0,sequence=0,revision=0,issues=0;
    uint32_t recording=0,known=0,nulls=0,index_known=0,index_null=0;
    uint32_t topology_known=0,topology=0,predicate_known=0,predicate_op=0;
    uint64_t predicate=0,predicate_offset=0;
    WWMI_BindingView_v1 vertices[32]{},indices{};
};
struct Pending { uint64_t native=0,session=0,call=0,creation=0,generation=0;uint32_t source=0; };
struct State {
    std::mutex mutex;
    std::condition_variable drained;
    WWMI_BindingTrace_Callback_v1 callback=nullptr;
    void *user=nullptr;
    bool closing=false;
    size_t in_flight=0;
    uint64_t next_session=0,session=0,sequence=0,creation=0;
    uint64_t closed_pending=0,closed_lists=0;
    std::unordered_map<uint64_t,List> lists;
    std::unordered_map<uint64_t,Pending> pending;
};
State &state() { static State *const s=new State;return *s; }
void unknown(List &v) {
    v.known=v.nulls=v.index_known=v.index_null=v.topology_known=v.topology=0;
    v.predicate_known=v.predicate_op=0;v.predicate=v.predicate_offset=0;
    for(auto &x:v.vertices) x={};
    v.indices={};
}
void invalidate(List &v,uint64_t issue) { v.issues|=issue;unknown(v);++v.revision; }
void callback_cpp(WWMI_BindingTrace_Callback_v1 callback,const WWMI_BindingRecord_v1 *r,void *u) noexcept {
    try { callback(r,u); } catch(...) { failures.fetch_add(1,std::memory_order_relaxed); }
}
void callback_isolated(WWMI_BindingTrace_Callback_v1 callback,const WWMI_BindingRecord_v1 *r,void *u) noexcept {
#if defined(_MSC_VER)
    __try { callback_cpp(callback,r,u); }
    __except(EXCEPTION_EXECUTE_HANDLER) { failures.fetch_add(1,std::memory_order_relaxed); }
#else
    callback_cpp(callback,r,u);
#endif
}
void clock_metadata(WWMI_BindingRecord_v1 &r) {
#if defined(_WIN32)
    LARGE_INTEGER tick{},freq{};QueryPerformanceCounter(&tick);QueryPerformanceFrequency(&freq);
    r.qpc=static_cast<uint64_t>(tick.QuadPart);r.qpc_frequency=static_cast<uint64_t>(freq.QuadPart);
    r.clock_domain=1;r.thread_id=GetCurrentThreadId();
#else
    r.qpc=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
    r.qpc_frequency=1000000000;r.clock_domain=2;
    r.thread_id=static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}
bool valid(const WWMI_BindingInput_v1 &x) {
    if(x.struct_size!=sizeof(x)||x.abi_version!=1||!x.native_list||x.reserved2||
       x.source<1||x.source>3||x.kind<1||x.kind>12) return false;
    if(x.views_null>1||x.initial_state>2) return false;
    if(x.kind==WWMI_BIND_VERTEX_BUFFERS&&(x.start_slot>32||x.count>32-x.start_slot)) return false;
    if(x.kind==WWMI_BIND_INDEX_BUFFER&&x.count!=1) return false;
    if(x.kind==WWMI_BIND_DRAW_BEFORE&&(x.paired_before_sequence||x.draw.reserved)) return false;
    if(x.kind==WWMI_BIND_DRAW_AFTER&&!x.paired_before_sequence) return false;
    if(x.kind==WWMI_BIND_INVALIDATE&&(x.reason<1||x.reason>8)) return false;
    return true;
}
void snapshot(const List &l,WWMI_BindingRecord_v1 &r) {
    r.creation_id=l.creation;r.recording_generation=l.generation;r.list_sequence=l.sequence;
    r.binding_revision=l.revision;r.capture_issues=l.issues;r.recording_state=l.recording;
    if(!l.recording) r.capture_issues|=WWMI_BIND_ISSUE_UNKNOWN_RECORDING;
    if(l.recording==2) r.capture_issues|=WWMI_BIND_ISSUE_CLOSED_LIST;
    r.vertex_known_mask=l.known;r.vertex_null_mask=l.nulls;
    r.index_known=l.index_known;r.index_null=l.index_null;
    r.index_view=l.indices;r.topology_known=l.topology_known;r.topology=l.topology;
    r.predicate_known=l.predicate_known;r.predicate_resource=l.predicate;
    r.predicate_offset=l.predicate_offset;r.predicate_operation=l.predicate_op;
    for(unsigned i=0;i<32;++i) r.vertex_views[i]=l.vertices[i];
}
}
extern "C" int32_t WWMI_BIND_CALL WWMI_BindingTrace_Register_v1(WWMI_BindingTrace_Callback_v1 callback,void *user) {
    if(!callback) return -1;
    if(callback_depth) return -3;
    try {
        State &s=state();std::lock_guard<std::mutex> lock(s.mutex);
        if(s.closing) return -2;
        if(s.callback) return s.callback==callback&&s.user==user?0:-2;
        if(s.next_session==std::numeric_limits<uint64_t>::max()) return -2;
        s.lists.clear();s.pending.clear();s.sequence=0;s.creation=0;s.closed_pending=s.closed_lists=0;
        failures=0;reentrant=0;observation_failures=0;
        s.callback=callback;s.user=user;s.session=++s.next_session;
        active.store(s.session,std::memory_order_release);return 0;
    } catch(...) { return -2; }
}
extern "C" int32_t WWMI_BIND_CALL WWMI_BindingTrace_Unregister_v1(WWMI_BindingTrace_Callback_v1 callback,void *user) {
    if(!callback) return -1;
    if(callback_depth) return -3;
    try {
        State &s=state();std::unique_lock<std::mutex> lock(s.mutex);
        if(s.closing) return -2;
        if(s.callback!=callback||s.user!=user) return -4;
        s.closing=true;active.store(0,std::memory_order_release);
        s.drained.wait(lock,[&]{return !s.in_flight;});
        s.closed_pending=s.pending.size();s.closed_lists=s.lists.size();
        s.callback=nullptr;s.user=nullptr;s.session=0;s.lists.clear();s.pending.clear();s.closing=false;
        return 0;
    } catch(...) { return -2; }
}
extern "C" uint64_t WWMI_BIND_CALL WWMI_BindingTrace_Submit_v1(const WWMI_BindingInput_v1 *input) {
    const auto session=active.load(std::memory_order_acquire);if(!session) return 0;
    if(callback_depth) { reentrant.fetch_add(1,std::memory_order_relaxed);return 0; }
    if(!input) { observation_failures.fetch_add(1,std::memory_order_relaxed);return 0; }
    try {
        const WWMI_BindingInput_v1 x=*input;
        if(!valid(x)) { observation_failures.fetch_add(1,std::memory_order_relaxed);return 0; }
        State &s=state();WWMI_BindingRecord_v1 r{};WWMI_BindingTrace_Callback_v1 callback;void *user;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if(s.closing||!s.callback||s.session!=session) return 0;
            if(s.sequence==std::numeric_limits<uint64_t>::max()||s.creation==std::numeric_limits<uint64_t>::max()) {
                observation_failures.fetch_add(1,std::memory_order_relaxed);return 0;
            }
            auto it=s.lists.find(x.native_list);
            if(it==s.lists.end()) {
                if(s.lists.size()>=kMaximumLists) { observation_failures.fetch_add(1,std::memory_order_relaxed);return 0; }
                List l{};
                if(x.kind!=WWMI_BIND_LIST_CREATE) l.issues=WWMI_BIND_ISSUE_MISSING_CREATE|WWMI_BIND_ISSUE_LATE_SUBSCRIPTION;
                it=s.lists.emplace(x.native_list,l).first;
            }
            List &l=it->second;
            if(x.kind==WWMI_BIND_LIST_CREATE) {
                const bool duplicate=l.creation!=0;
                l={};l.creation=++s.creation;l.recording=x.initial_state;
                if(duplicate) l.issues|=WWMI_BIND_ISSUE_MALFORMED;
            }
            if(l.sequence==std::numeric_limits<uint64_t>::max()||l.revision==std::numeric_limits<uint64_t>::max()) {
                observation_failures.fetch_add(1,std::memory_order_relaxed);return 0;
            }
            r.struct_size=sizeof(r);r.abi_version=1;r.session=session;r.sequence=++s.sequence;r.input=x;
            r.unproven_fields=WWMI_BIND_UNPROVEN_ALL;++l.sequence;
            switch(x.kind) {
            case WWMI_BIND_LIST_RESET:
                if(x.hresult>=0) {
                    if(l.generation==std::numeric_limits<uint64_t>::max()) invalidate(l,WWMI_BIND_ISSUE_MALFORMED);
                    else { ++l.generation;++l.revision;l.recording=1;unknown(l); }
                } break;
            case WWMI_BIND_LIST_CLOSE:
                if(x.hresult>=0) l.recording=2;else l.issues|=WWMI_BIND_ISSUE_INVALIDATED;
                break;
            case WWMI_BIND_VERTEX_BUFFERS:
                for(uint32_t i=0;i<x.count;++i) {
                    const uint32_t slot=x.start_slot+i,bit=UINT32_C(1)<<slot;
                    l.known|=bit;
                    if(x.views_null) { l.nulls|=bit;l.vertices[slot]={}; }
                    else { l.nulls&=~bit;l.vertices[slot]=x.views[i]; }
                }
                if(x.count) ++l.revision;
                break;
            case WWMI_BIND_INDEX_BUFFER:
                l.index_known=1;l.index_null=x.views_null;l.indices=x.views_null?WWMI_BindingView_v1{}:x.views[0];
                ++l.revision;break;
            case WWMI_BIND_TOPOLOGY:l.topology_known=1;l.topology=x.topology;++l.revision;break;
            case WWMI_BIND_CLEAR_STATE:
                unknown(l);l.known=l.nulls=UINT32_MAX;l.index_known=l.index_null=1;l.topology_known=1;++l.revision;break;
            case WWMI_BIND_PREDICATION:
                l.predicate_known=1;l.predicate=x.predicate_resource;l.predicate_offset=x.predicate_offset;
                l.predicate_op=x.predicate_operation;++l.revision;break;
            case WWMI_BIND_INVALIDATE:invalidate(l,WWMI_BIND_ISSUE_INVALIDATED);break;
            case WWMI_BIND_DRAW_BEFORE:
                if(s.pending.size()>=kMaximumPending) l.issues|=WWMI_BIND_ISSUE_MALFORMED;
                else s.pending.emplace(r.sequence,Pending{x.native_list,x.executed_session,x.executed_call,l.creation,l.generation,x.source});
                break;
            case WWMI_BIND_DRAW_AFTER: {
                const auto p=s.pending.find(x.paired_before_sequence);
                if(p==s.pending.end()||p->second.native!=x.native_list||p->second.source!=x.source||
                   p->second.session!=x.executed_session||p->second.call!=x.executed_call||
                   p->second.creation!=l.creation||p->second.generation!=l.generation)
                    l.issues|=WWMI_BIND_ISSUE_MALFORMED;
                else s.pending.erase(p);
                break;
            }
            default:break;
            }
            snapshot(l,r);
            r.callback_failures_total=failures.load(std::memory_order_relaxed);
            r.suppressed_reentrant_total=reentrant.load(std::memory_order_relaxed);
            r.observation_failures_total=observation_failures.load(std::memory_order_relaxed);
            if(r.callback_failures_total) r.capture_issues|=WWMI_BIND_ISSUE_CALLBACK_FAILURE;
            if(r.suppressed_reentrant_total) r.capture_issues|=WWMI_BIND_ISSUE_REENTRANT;
            if(r.observation_failures_total) r.capture_issues|=WWMI_BIND_ISSUE_MALFORMED;
            if(x.kind==WWMI_BIND_LIST_DESTROY) {
                s.lists.erase(it);
                for(auto p=s.pending.begin();p!=s.pending.end();) {
                    if(p->second.native==x.native_list) p=s.pending.erase(p);else ++p;
                }
            }
            clock_metadata(r);callback=s.callback;user=s.user;++s.in_flight;
        }
        ++callback_depth;callback_isolated(callback,&r,user);--callback_depth;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            --s.in_flight;if(!s.in_flight) s.drained.notify_all();
        }
        return r.sequence;
    } catch(...) { observation_failures.fetch_add(1,std::memory_order_relaxed);return 0; }
}
extern "C" int32_t WWMI_BIND_CALL WWMI_BindingTrace_GetStatus_v1(WWMI_BindingStatus_v1 *out) {
    if(!out||out->struct_size!=sizeof(*out)||out->abi_version!=1) return -1;
    try {
        State &s=state();std::lock_guard<std::mutex> lock(s.mutex);
        WWMI_BindingStatus_v1 r{};r.struct_size=sizeof(r);r.abi_version=1;
        r.last_session=s.next_session;r.last_sequence=s.sequence;
        r.callback_failures_total=failures.load(std::memory_order_relaxed);
        r.suppressed_reentrant_total=reentrant.load(std::memory_order_relaxed);
        r.observation_failures_total=observation_failures.load(std::memory_order_relaxed);
        r.active=s.callback!=nullptr;r.draining=s.closing;
        r.pending_draws=r.active?s.pending.size():s.closed_pending;
        r.tracked_lists=r.active?s.lists.size():s.closed_lists;*out=r;return 0;
    } catch(...) { return -2; }
}
