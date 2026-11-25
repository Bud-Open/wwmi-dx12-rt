#pragma once

struct ID3D12DescriptorHeap;
struct ID3D12GraphicsCommandList;
struct ID3D12RootSignature;

// Internal to the paired draw callback. These are borrowed native objects,
// never handles to a previous recording or an inferred "last used" heap.
struct RtNativeDrawHeaps {
    ID3D12DescriptorHeap *resources, *samplers;
};

bool RtHasNativeBindingScope(ID3D12GraphicsCommandList*) noexcept;
const RtNativeDrawHeaps* RtOriginalDrawHeaps(ID3D12GraphicsCommandList*) noexcept;
bool RtSetDrawHeaps(ID3D12GraphicsCommandList*,ID3D12DescriptorHeap*,ID3D12DescriptorHeap*) noexcept;
// Used only after a material mirror was prepared from the exact native heaps.
void RtBindPreparedDrawHeaps(ID3D12GraphicsCommandList*,ID3D12DescriptorHeap*,ID3D12DescriptorHeap*) noexcept;
// Returns true if this is a paired scope. Only a scope that was allowed to
// change its heaps can have any restoration to record.
bool RtRestoreDrawHeaps(ID3D12GraphicsCommandList*) noexcept;
bool RtBeforePortComputeRoots(ID3D12GraphicsCommandList*,ID3D12RootSignature*) noexcept;
void RtRefusePortWork(ID3D12GraphicsCommandList*) noexcept;
bool RtPortWorkRefused(ID3D12GraphicsCommandList*) noexcept;
