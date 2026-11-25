// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d12.h>
#include <reshade_api_device.hpp>
namespace wwmi::rt_forward {
bool enabled() noexcept;
void blas_after(reshade::api::command_list *,ID3D12GraphicsCommandList *,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *) noexcept;
uint64_t tlas_input(reshade::api::command_list *,ID3D12GraphicsCommandList *,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *) noexcept;
uint64_t hit_table(reshade::api::command_list *,ID3D12GraphicsCommandList *,const D3D12_DISPATCH_RAYS_DESC *) noexcept;
void queue_after(reshade::api::command_queue *,ID3D12CommandQueue *,reshade::api::command_list *) noexcept;
}
