// SPDX-License-Identifier: GPL-3.0-only
// Original portable subscription/snapshot code; contains no GPL host code.
#define WWMI_EXECUTED_DRAW_BUILD
#include "wwmi_executed_draw.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static_assert(std::is_standard_layout<WWMI_ExecutedDrawRecord_v1>::value, "C ABI requires standard layout");
static_assert(std::is_trivially_copyable<WWMI_ExecutedDrawRecord_v1>::value, "C ABI requires a POD snapshot");
static_assert(sizeof(WWMI_ExecutedDrawArguments_v1)==24, "Draw argument ABI changed");
static_assert(sizeof(WWMI_ExecutedDrawView_v1)==32, "View ABI changed");
static_assert(sizeof(WWMI_ExecutedDrawRecord_v1)==1160, "Record v1 ABI changed; introduce a new version");

namespace {
std::atomic<uint64_t> active_session{0};
std::atomic<uint64_t> sequence{0}, call_ids{0};
std::atomic<uint64_t> callback_failures{0}, suppressed{0}, observation_failures{0};
thread_local unsigned callback_depth=0;
struct State {
    std::mutex mutex;
    std::condition_variable drained;
    WWMI_ExecutedDraw_Callback_v1 callback=nullptr;
    void *user=nullptr;
    uint64_t next_session=0, registered_session=0;
    std::size_t in_flight=0;
    bool closing=false;
};
State &state() {
    // Process-lifetime ownership avoids a static destructor racing late draws.
    static State *const value=new State;
    return *value;
}
void callback_cpp(WWMI_ExecutedDraw_Callback_v1 callback,
    const WWMI_ExecutedDrawRecord_v1 *record, void *user) noexcept {
    try { callback(record,user); }
    catch (...) { callback_failures.fetch_add(1,std::memory_order_relaxed); }
}
void callback_isolated(WWMI_ExecutedDraw_Callback_v1 callback,
    const WWMI_ExecutedDrawRecord_v1 *record, void *user) noexcept {
#if defined(_MSC_VER)
    __try { callback_cpp(callback,record,user); }
    __except (EXCEPTION_EXECUTE_HANDLER) { callback_failures.fetch_add(1,std::memory_order_relaxed); }
#else
    callback_cpp(callback,record,user);
#endif
}
void metadata(WWMI_ExecutedDrawRecord_v1 &record, wwmi::executed_draw::Token token, uint32_t phase) {
    record.struct_size=sizeof(record);record.abi_version=WWMI_EXECUTED_DRAW_ABI_V1;
    record.phase=phase;record.source=WWMI_EXEC_SOURCE_SECTION_DRAW_INDEXED;
    record.sequence=sequence.fetch_add(1,std::memory_order_relaxed)+1;
    record.call_id=token.call_id;record.session_id=token.session;
    record.incomplete_fields|=WWMI_EXEC_MISSING_COMPLETE_SCOPE;
    record.callback_failures_total=callback_failures.load(std::memory_order_relaxed);
    record.suppressed_reentrant_total=suppressed.load(std::memory_order_relaxed);
    record.observation_failures_total=observation_failures.load(std::memory_order_relaxed);
#if defined(_WIN32)
    LARGE_INTEGER ticks{},frequency{};QueryPerformanceCounter(&ticks);QueryPerformanceFrequency(&frequency);
    record.thread_id=GetCurrentThreadId();record.timestamp_ticks=static_cast<uint64_t>(ticks.QuadPart);
    record.timestamp_frequency=static_cast<uint64_t>(frequency.QuadPart);record.clock_domain=WWMI_EXEC_CLOCK_QPC;
#else
    record.thread_id=static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    record.timestamp_ticks=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    record.timestamp_frequency=1000000000;record.clock_domain=WWMI_EXEC_CLOCK_STEADY_NS;
#endif
}
bool deliver(uint64_t session, const WWMI_ExecutedDrawRecord_v1 &record) {
    if (!session || active_session.load(std::memory_order_acquire)!=session) return false;
    if (callback_depth) { suppressed.fetch_add(1,std::memory_order_relaxed);return false; }
    State &s=state();WWMI_ExecutedDraw_Callback_v1 callback=nullptr;void *user=nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.closing || !s.callback || s.registered_session!=session) return false;
        callback=s.callback;user=s.user;++s.in_flight;
    }
    ++callback_depth;callback_isolated(callback,&record,user);--callback_depth;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        --s.in_flight;if (!s.in_flight) s.drained.notify_all();
    }
    return true;
}
} // namespace

extern "C" int32_t WWMI_EXEC_CALL WWMI_ExecutedDraw_Register_v1(WWMI_ExecutedDraw_Callback_v1 callback, void *user) {
    if (!callback) return -1;
    if (callback_depth) return -3;
    try {
        State &s=state();std::lock_guard<std::mutex> lock(s.mutex);
        if (s.closing) return -2;
        if (s.callback) return s.callback==callback && s.user==user ? 0 : -2;
        if (s.next_session==std::numeric_limits<uint64_t>::max()) return -2;
        s.callback=callback;s.user=user;s.registered_session=++s.next_session;
        active_session.store(s.registered_session,std::memory_order_release);
        return 0;
    } catch (...) { return -2; }
}
extern "C" int32_t WWMI_EXEC_CALL WWMI_ExecutedDraw_Unregister_v1(WWMI_ExecutedDraw_Callback_v1 callback, void *user) {
    if (!callback) return -1;
    if (callback_depth) return -3;
    try {
        State &s=state();std::unique_lock<std::mutex> lock(s.mutex);
        if (s.closing) return -2;
        if (s.callback!=callback || s.user!=user) return -4;
        s.closing=true;active_session.store(0,std::memory_order_release);
        s.drained.wait(lock,[&s]{return s.in_flight==0;});
        s.callback=nullptr;s.user=nullptr;s.registered_session=0;s.closing=false;
        return 0;
    } catch (...) { return -2; }
}
namespace wwmi::executed_draw {
uint64_t SnapshotSession() noexcept { return active_session.load(std::memory_order_acquire); }
void ObservationFailed() noexcept { observation_failures.fetch_add(1,std::memory_order_relaxed); }
Token Before(uint64_t session, const WWMI_ExecutedDrawRecord_v1 &payload) noexcept {
    if (!session || SnapshotSession()!=session) return {};
    if (callback_depth) { suppressed.fetch_add(1,std::memory_order_relaxed);return {}; }
    try {
        if (payload.struct_size!=sizeof(payload) || payload.abi_version!=WWMI_EXECUTED_DRAW_ABI_V1) {
            observation_failures.fetch_add(1,std::memory_order_relaxed);return {};
        }
        const Token token{session,call_ids.fetch_add(1,std::memory_order_relaxed)+1};
        WWMI_ExecutedDrawRecord_v1 record=payload; // Borrowed CPU stack input, deep POD copy.
        metadata(record,token,WWMI_EXEC_PHASE_BEFORE_NATIVE_CALL);
        if (!deliver(session,record)) return {};
        return token;
    } catch (...) { observation_failures.fetch_add(1,std::memory_order_relaxed);return {}; }
}
void After(Token token) noexcept {
    if (!token.session || SnapshotSession()!=token.session) return;
    if (callback_depth) { suppressed.fetch_add(1,std::memory_order_relaxed);return; }
    try {
        WWMI_ExecutedDrawRecord_v1 record{};
        record.incomplete_fields=WWMI_EXEC_MISSING_ALL_V1;
        metadata(record,token,WWMI_EXEC_PHASE_AFTER_NATIVE_RETURN);
        deliver(token.session,record);
    } catch (...) { observation_failures.fetch_add(1,std::memory_order_relaxed); }
}
} // namespace wwmi::executed_draw
