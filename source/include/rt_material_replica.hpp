#pragma once
#include <array>
#include <memory>
#include <d3d12.h>
#include <wrl/client.h>

struct HeapKey;
struct ResInfo;
struct RtMaterialReplica {
    std::shared_ptr<const void> owner;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> resources;
    bool initialized=false,stamped=false;
};
bool AcquirePublishedMaterialReplica(ID3D12Device*,const HeapKey&,ResInfo&,RtMaterialReplica&) noexcept;
// Actual unwrapped material copies, independent of the host's D1 ledger.
void RtCountRecordedMaterial(unsigned created,unsigned reused,unsigned stamped,unsigned copied,unsigned missed)noexcept;
