// SPDX-License-Identifier: GPL-3.0-only
#define WWMI_BINDING_CLIENT_BUILD
#include "wwmi_binding_client.hpp"
#include <condition_variable>
#include <mutex>
#if defined(_WIN32)
# include <Windows.h>
#endif

namespace {
struct State {
    std::mutex mutex;
    std::condition_variable drained;
    WWMI_BindingTrace_SubmitFn_v1 endpoint = nullptr;
    uint64_t active = 0;
    bool stopping = false;
    WWMI_BindingClientStatus_v1 status{sizeof(WWMI_BindingClientStatus_v1),1,0,0,0,0,0,0,0,0,0};
};
// Survives static destruction order; no resources/threads/native objects owned.
State &state() { static State *s = new State; return *s; }
thread_local unsigned submit_depth = 0;
}

namespace wwmi::binding_client {
int InitializeResolved(WWMI_BindingTrace_SubmitFn_v1 endpoint, uint64_t host) noexcept {
    try {
        auto &s = state(); std::lock_guard<std::mutex> lock(s.mutex);
        if (s.stopping || s.active) return -2;
        if (s.endpoint && (s.endpoint != endpoint || s.status.host_module != host)) return -2;
        s.status.initialization_attempted = 1;
        s.status.host_module = host;
        s.endpoint = host ? endpoint : nullptr;
        s.status.endpoint_available = s.endpoint ? 1u : 0u;
        ++s.status.endpoint_generation;
        return s.endpoint ? 0 : -1;
    } catch (...) { return -2; }
}

int Initialize(void *host) noexcept {
#if defined(_WIN32)
    WWMI_BindingTrace_SubmitFn_v1 fn = nullptr;
    if (host) fn = reinterpret_cast<WWMI_BindingTrace_SubmitFn_v1>(
        GetProcAddress(static_cast<HMODULE>(host), "WWMI_BindingTrace_Submit_v1"));
    return InitializeResolved(fn, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(host)));
#else
    (void)host;
    return InitializeResolved(nullptr, 0);
#endif
}

int Shutdown() noexcept {
    if (submit_depth) return -3;
    try {
        auto &s = state(); std::unique_lock<std::mutex> lock(s.mutex);
        if (s.stopping) return -2;
        s.stopping = true; s.endpoint = nullptr; s.status.endpoint_available = 0;
        s.drained.wait(lock, [&s] { return s.active == 0; });
        s.status.host_module = 0; s.status.initialization_attempted = 0;
        s.stopping = false;
        return 0;
    } catch (...) { return -2; }
}

uint64_t Submit(const WWMI_BindingInput_v1 &input) noexcept {
    State *p = nullptr;
    WWMI_BindingTrace_SubmitFn_v1 fn = nullptr;
    try {
        p = &state(); std::lock_guard<std::mutex> lock(p->mutex);
        ++p->status.submit_attempts;
        if (submit_depth) { ++p->status.recursive_submissions; return 0; }
        fn = p->endpoint;
        if (!fn || p->stopping) { ++p->status.missing_endpoint_attempts; return 0; }
        ++p->active;
    } catch (...) { return 0; }
    ++submit_depth;
    uint64_t sequence = 0;
    bool threw = false;
    try { sequence = fn(&input); } catch (...) { threw = true; }
    --submit_depth;
    try {
        std::lock_guard<std::mutex> lock(p->mutex);
        if (threw) ++p->status.callback_exceptions;
        if (!sequence) ++p->status.zero_sequence_returns;
        --p->active;
        if (!p->active) p->drained.notify_all();
    } catch (...) { /* Native forwarding must not throw for observer failure. */ }
    return sequence;
}
} // namespace wwmi::binding_client

extern "C" WWMI_CLIENT_EXPORT int32_t WWMI_BIND_CALL WWMI_BindingClient_GetStatus_v1(
    WWMI_BindingClientStatus_v1 *output, uint32_t bytes) noexcept {
    if (!output || bytes != sizeof(*output)) return -1;
    try {
        auto &s = state(); std::lock_guard<std::mutex> lock(s.mutex);
        *output = s.status;
        return 0;
    } catch (...) { return -2; }
}
