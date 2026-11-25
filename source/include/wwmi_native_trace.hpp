// SPDX-License-Identifier: GPL-3.0-only
// Observer-only bridge. No function in this interface records GPU work.
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
# define WWMI_NATIVE_CALL __cdecl
# if defined(WWMI_NATIVE_TRACE_BUILD)
#  define WWMI_NATIVE_EXPORT __declspec(dllexport)
# else
#  define WWMI_NATIVE_EXPORT
# endif
#else
# define WWMI_NATIVE_CALL
# define WWMI_NATIVE_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (WWMI_NATIVE_CALL *WWMI_NativeTrace_Callback_v1)(
    const char *utf8_record, size_t byte_length, void *user);

// 0: success; -1: invalid callback; -2: busy/draining;
// -3: called recursively from a callback; -4: subscriber identity mismatch.
// Register is idempotent for the same callback/user pair. One subscriber only.
// Unregister must not run under the loader lock. On success all callbacks from
// that registration are drained. Never unload subscriber code before it returns.
WWMI_NATIVE_EXPORT int32_t WWMI_NATIVE_CALL WWMI_NativeTrace_Register_v1(
    WWMI_NativeTrace_Callback_v1 callback, void *user);
WWMI_NATIVE_EXPORT int32_t WWMI_NATIVE_CALL WWMI_NativeTrace_Unregister_v1(
    WWMI_NativeTrace_Callback_v1 callback, void *user);

#ifdef __cplusplus
}

#include <d3d12.h>

namespace wwmi::native_trace {

struct CallToken {
    uint64_t call_id = 0;
    uint64_t session = 0;
};

bool enabled() noexcept;

// Descriptor payloads are synchronously serialized before this returns. GPU VAs
// are emitted as addresses only: no GPU memory is mapped/read by the bridge.
CallToken build_as_before(const void *command_list, const void *native_command_list,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc,
    UINT postbuild_count,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild) noexcept;

CallToken dispatch_rays_before(const void *command_list, const void *native_command_list,
    const D3D12_DISPATCH_RAYS_DESC *desc) noexcept;

// path = "wrapper_input" or "native_forward". The distinction matters because
// another ReShade addon can request pipeline reconstruction before forwarding.
CallToken state_object_before(const void *device, const void *native_device,
    const char *api, const char *path, const D3D12_STATE_OBJECT_DESC *desc,
    const void *existing_collection = nullptr) noexcept;

CallToken execute_before(const void *queue, const void *native_queue, UINT count,
    ID3D12CommandList *const *caller_lists,
    ID3D12CommandList *const *native_lists) noexcept;

CallToken queue_sync_before(const char *kind, const void *queue,
    const void *native_queue, const void *fence, uint64_t value) noexcept;

// "after" means the CPU API returned. It never means fence/GPU/present complete.
// outcome = "returned", "suppressed_by_existing_addon", or another explicit
// forwarding outcome. The bridge itself never suppresses a call.
void call_after(CallToken token, const char *kind, const void *object,
    bool has_hresult = false, HRESULT hr = S_OK, const void *result = nullptr,
    const char *outcome = "returned") noexcept;

void command_list_created(const void *command_list,
    ID3D12GraphicsCommandList *native_command_list) noexcept;
void command_list_destroyed(const void *command_list,
    const void *native_command_list) noexcept;
void command_list_reset_after(const void *command_list,
    const void *native_command_list, const void *allocator,
    const void *initial_pipeline, HRESULT hr) noexcept;
void command_list_close_after(const void *command_list,
    const void *native_command_list, HRESULT hr) noexcept;

void pipeline_state_bound(const void *command_list,
    const void *native_command_list, const void *state_object) noexcept;

// Basic resource identity/VA only. Lifecycle coverage is the existing ReShade
// invoke_init_resource_event path, not every allocation/interface in the process.
uint64_t resource_created(const void *device, ID3D12Resource *resource,
    uint64_t reshade_initial_usage = 0) noexcept;
void resource_destroyed(const void *device, const void *resource,
    uint64_t creation_id = 0) noexcept;

CallToken copy_as_before(const void *command_list, const void *native_command_list,
    D3D12_GPU_VIRTUAL_ADDRESS dest, D3D12_GPU_VIRTUAL_ADDRESS source,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) noexcept;
CallToken postbuild_before(const void *command_list, const void *native_command_list,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc,
    UINT count, const D3D12_GPU_VIRTUAL_ADDRESS *source_addresses) noexcept;

} // namespace wwmi::native_trace
#endif
