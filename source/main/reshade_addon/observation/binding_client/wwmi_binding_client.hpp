// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Native forwarding helpers used at the candidate's actual D3D12 call sites.
// They never suppress/repeat native calls and never dereference GPU addresses.
#include "../binding_bridge/wwmi_binding_trace.h"
#include <cstdint>

#if defined(_WIN32) && defined(WWMI_BINDING_CLIENT_BUILD)
# define WWMI_CLIENT_EXPORT __declspec(dllexport)
#else
# define WWMI_CLIENT_EXPORT
#endif

#pragma pack(push,8)
struct WWMI_BindingClientStatus_v1 {
    uint32_t struct_size, abi_version, initialization_attempted, endpoint_available;
    uint64_t host_module, endpoint_generation, submit_attempts, missing_endpoint_attempts;
    uint64_t zero_sequence_returns, callback_exceptions, recursive_submissions;
};
#pragma pack(pop)
extern "C" WWMI_CLIENT_EXPORT int32_t WWMI_BIND_CALL WWMI_BindingClient_GetStatus_v1(
    WWMI_BindingClientStatus_v1 *status, uint32_t status_size) noexcept;

namespace wwmi::binding_client {
// Called from AddonInit/AddonUninit, never DllMain. The provided host owns this
// addon, so it must remain mapped through Shutdown and all forwarded calls.
int Initialize(void *host_module) noexcept;
int Shutdown() noexcept;
// The same resolution boundary used by Initialize; exposed for CPU contracts.
// A numeric function pointer is NOT evidence that the game host was executed.
int InitializeResolved(WWMI_BindingTrace_SubmitFn_v1, uint64_t host_identity) noexcept;
uint64_t Submit(const WWMI_BindingInput_v1 &) noexcept;

inline WWMI_BindingInput_v1 Input(const void *list, uint32_t kind, uint32_t site) noexcept {
    WWMI_BindingInput_v1 e{};
    e.struct_size = sizeof(e); e.abi_version = WWMI_BIND_ABI_V1;
    e.kind = kind; e.source = WWMI_BIND_SOURCE_CANDIDATE;
    e.native_list = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(list));
    e.source_site = site;
    return e;
}

template<class Native, class View>
inline void IASetVertexBuffers(Native *list, unsigned start, unsigned count,
                               const View *views, uint32_t site) {
    list->IASetVertexBuffers(start, count, views);
    auto e = Input(list, WWMI_BIND_VERTEX_BUFFERS, site);
    e.start_slot = start; e.count = count; e.views_null = views ? 0u : 1u;
    // Count/range violations are reported without reading beyond the ABI array.
    // The bridge rejects malformed payloads; the original native call is intact.
    if (views && start <= 32 && count <= 32 - start) for (unsigned i = 0; i < count; ++i)
        e.views[i] = {views[i].BufferLocation, views[i].SizeInBytes, views[i].StrideInBytes};
    (void)Submit(e);
}

template<class Native, class View>
inline void IASetIndexBuffer(Native *list, const View *view, uint32_t site) {
    list->IASetIndexBuffer(view);
    auto e = Input(list, WWMI_BIND_INDEX_BUFFER, site);
    e.count = 1; e.views_null = view ? 0u : 1u;
    if (view) e.views[0] = {view->BufferLocation, view->SizeInBytes, static_cast<uint32_t>(view->Format)};
    (void)Submit(e);
}

template<class Native, class Topology>
inline void IASetPrimitiveTopology(Native *list, Topology topology, uint32_t site) {
    list->IASetPrimitiveTopology(topology);
    auto e = Input(list, WWMI_BIND_TOPOLOGY, site);
    e.topology = static_cast<uint32_t>(topology);
    (void)Submit(e);
}

// Actual successful CreateCommandList produces a recording list. Failed calls
// must not invent an object identity. No COM method is called by this notifier.
template<class Native>
inline void ListCreatedAfter(Native *list, int32_t hresult, uint32_t site) noexcept {
    if (hresult < 0 || !list) return;
    auto e = Input(list, WWMI_BIND_LIST_CREATE, site);
    e.hresult = hresult; e.initial_state = WWMI_BIND_INITIAL_STATE_RECORDING;
    (void)Submit(e);
}

template<class Native, class Allocator, class Pipeline>
inline auto ResetCommandList(Native *list, Allocator *allocator, Pipeline *pipeline, uint32_t site)
    -> decltype(list->Reset(allocator, pipeline)) {
    const auto result = list->Reset(allocator, pipeline);
    auto e = Input(list, WWMI_BIND_LIST_RESET, site); e.hresult = static_cast<int32_t>(result);
    (void)Submit(e);
    return result;
}

template<class Native>
inline auto CloseCommandList(Native *list, uint32_t site) -> decltype(list->Close()) {
    const auto result = list->Close();
    auto e = Input(list, WWMI_BIND_LIST_CLOSE, site); e.hresult = static_cast<int32_t>(result);
    (void)Submit(e);
    return result;
}

// Immediately before an EXISTING owner replacement releases its pointer.
// The ABI's LIST_DESTROY retires the observed owner identity, not the last COM
// reference or submitted GPU work. No Release call is added or reordered.
// Do not use this notifier from static destructors / DllMain.
template<class Native>
inline void OwnerRetiringBeforeReplacement(Native *list, uint32_t site) noexcept {
    if (!list) return;
    auto e = Input(list, WWMI_BIND_LIST_DESTROY, site);
    e.reason = WWMI_BIND_REASON_UNOBSERVED_LIFETIME;
    (void)Submit(e);
}

template<class Native>
inline void DrawIndexedInstanced(Native *list, unsigned count, unsigned instances,
    unsigned first, int base, unsigned first_instance, uint32_t site,
    uint64_t executed_session = 0, uint64_t executed_call = 0) {
    auto e = Input(list, WWMI_BIND_DRAW_BEFORE, site);
    e.executed_session = executed_session; e.executed_call = executed_call;
    e.draw = {count, instances, first, base, first_instance, 0};
    const uint64_t before = Submit(e);
    list->DrawIndexedInstanced(count, instances, first, base, first_instance);
    // Even a missing subscriber must not change the forwarded call. There is
    // no pair to invent after a rejected/unobserved before event.
    if (before) {
        e.kind = WWMI_BIND_DRAW_AFTER; e.paired_before_sequence = before;
        (void)Submit(e);
    }
}
} // namespace wwmi::binding_client
