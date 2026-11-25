#pragma once
#include <atomic>
#include <memory>
#include "rabbitfx/rabbitfx_bind.h"
#include "rt_draw_frame.h"
#include <wrl/client.h>
// Prepare every subdraw before recording the first one, so one native section
// is either replaced in full or remains native. The original RFX path is unchanged.
struct RtMaterialTables {
    unsigned failure=0,missing_param=~0u,missing_offset=~0u;
    RfxBindState original;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    std::vector<std::pair<unsigned,D3D12_GPU_DESCRIPTOR_HANDLE>> tables;
};
bool RtPrepareMaterialTables(reshade::api::command_list*,const std::vector<ShadowSlotOvr>&,RtMaterialTables&);
struct RtRecordedMaterial {
    WWMI_RtPackReserveDrawBindingFn_v1 reserve{};
    WWMI_RtPackReserveDrawBindingFn_v2 reserve2{};
    WWMI_RtDrawBindingRequest_v2 request{};
    WWMI_RtDrawBinding_v2 backing{};
};
bool RtPrepareRecordedMaterialTables(reshade::api::command_list*,const std::vector<ShadowSlotOvr>&,
                                     RtRecordedMaterial&,RtMaterialTables&);
// A synchronous section-local view of the original layout, table handles
// and heap pair. Each part still reserves and populates its own table slice.
struct RtRecordedMaterialWindow;
std::shared_ptr<const RtRecordedMaterialWindow> RtReadRecordedMaterialWindow(reshade::api::command_list*);
bool RtPrepareRecordedMaterialTablesInWindow(reshade::api::command_list*,const RtRecordedMaterialWindow&,
    const std::vector<ShadowSlotOvr>&,RtRecordedMaterial&,RtMaterialTables&);
void RtBindMaterialTables(ID3D12GraphicsCommandList*,const RtMaterialTables&);

bool RtOnlyVertexPixelStages(uint64_t pipeline);
