// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "rt_native_dispatch_receipt.h"
#include "wwmi_rt_forward.hpp"

namespace wwmi::rt_forward {
struct DispatchScope {
    bool active=false,finished=false;
    WWMI_RtNativeDispatch_v2 packet{};
    DispatchScope(reshade::api::command_list *,ID3D12GraphicsCommandList *,uint32_t,uint32_t,uint32_t) noexcept;
    ~DispatchScope();
    explicit operator bool() const noexcept { return active; }
    void before(bool ready,uint64_t pipeline,uint64_t layout) noexcept;
    void after() noexcept;
    void cancelled(bool ready,uint64_t pipeline,uint64_t layout) noexcept;
    DispatchScope(const DispatchScope &)=delete;
    DispatchScope &operator=(const DispatchScope &)=delete;
};
}
