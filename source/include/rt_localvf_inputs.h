#pragma once
#include <mutex>
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <reshade.hpp>
#include <atomic>
#include <memory>
#include "rt_material_tables.h"
#include "rt_aux_descriptor.h"
#include "rt_draw_frame.h"
#include "rt_author_draw.h"
#include "rt_localvf_surface.h"
struct RtProgramPsoContract;
struct RtLocalProgram {
    uint64_t hash; int uv,tangent,aux,previous; bool auxiliary_requires_native_color; // aux = the shader's masked float4 stream
};
const RtLocalProgram* RtLocalProgramFor(uint64_t hash);
bool RtReadAuthorInstance(reshade::api::command_list*,int primitive_reg,WWMI_RtAuthorInstance_v1&);
struct RtHistoryNativeRoot {
    uint64_t va=0;
    uint8_t param=0xff;
};
struct RtHistoryStageSnapshot {
    std::array<unsigned char,WWMI_RT_NH_VIEW_BYTES> view;
    std::array<unsigned char,WWMI_RT_NH_PRIMITIVE_BYTES> primitive;
};
enum class RtHistoryFallback : uint8_t { none, unused_varying, native_gate };
struct RtWorldSurface {
    std::array<uint32_t,6> local{};
    uint64_t native_cb=0,prev_resource=0,prev_offset=0,prev_bytes=0;
    uint8_t local_param=0xff;
    struct StreamSlot { unsigned param=0xff,offset=0; };
    // Original VS SRV table locations (uv, tangent, aux, previous).
    std::array<StreamSlot,4> streams{};
    uint64_t native_layout=0,native_pso=0;
    bool reads_previous=false,aux_constant=false;
    RtHistoryFallback history_fallback=RtHistoryFallback::none;
    RtAuxSource aux_source{};
    unsigned previous_resolution=255;
    WWMI_RtPreviousDrawScope_v1 previous_scope{};
    // Keep the two actual CPU-readable root windows live until the paired
    // client has looked up their creation generations after the section VM.
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> previous_cb_holds;
    // Original native roots and immutable CPU bytes captured before the VM.
    // One stage for the single-RT PS; two for the GBuffer PS programs.
    uint64_t history_layout=0,history_pso=0;
    std::array<RtHistoryNativeRoot,WWMI_RT_NH_CB_COUNT> history_roots{};
    std::unique_ptr<const RtHistoryStageSnapshot[]> history_cbs;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> history_ps_holds;
    std::shared_ptr<const RtProgramPsoContract> contracts; // fixed at original native selection
};
struct RtHistoryInvalid {
    std::array<std::vector<unsigned char>,WWMI_RT_NH_CB_COUNT> cb;
    std::array<uint64_t,WWMI_RT_NH_CB_COUNT> va{};
};
bool RtReadWorldSurface(reshade::api::command_list*,const RtLocalProgram&,uint32_t guid,uint32_t hash,unsigned component,
                        RtWorldSurface&,bool capture_previous_scope=false,bool capture_history_invalid=false,
                        WWMI_RtAuthorInstance_v1* instance=nullptr);
// The original PSO/OM was proved by RtMaskedDepthMaterialPass. Check that
// the actual slot can be nulled without aliasing another stage or binding.
void RtAllowMaskedVelocityFallback(const RtLocalProgram&,RtWorldSurface&);
bool RtProveMaskedSiblingBindings(reshade::api::command_list*,const RtLocalProgram&,const RtWorldSurface&,
                                  const std::array<uint64_t,2>& ps_reads);
bool RtPrepareHistoryInvalid(reshade::api::command_list*,const RtLocalProgram&,const RtWorldSurface&,RtHistoryInvalid&);
// A section borrows the four CPU scratch descriptors until each part has
// copied them to its own recording page. No GPU page is shared by accident.
struct RtWorldMaterialInputs {
    struct Slot {ShadowSlotOvr entry;unsigned failure;};
    std::unique_lock<std::mutex> scratch;
    std::shared_ptr<const RtRecordedMaterialWindow> window;
    std::array<Slot,4> streams;
    unsigned count=0;
};
bool RtPrepareWorldMaterialInputs(reshade::api::command_list*,const RtLocalProgram&,const RtWorldSurface&,
    const WWMI_RtLocalVfSurface_v1&,const WWMI_RtPreviousSurface_v1&,bool no_history,RtWorldMaterialInputs&,RtMaterialTables&,
    bool shared_window=false);
bool RtPrepareWorldMaterialPart(reshade::api::command_list*,const RtWorldSurface&,const WWMI_RtLocalVfSurface_v1&,
    const RtWorldMaterialInputs&,const std::vector<ShadowSlotOvr>&,const std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>&,
    WWMI_RtPackReserveDrawBindingFn_v1,RtMaterialTables&,uint64_t&,WWMI_RtPackReserveDrawBindingFn_v2,RtHistoryInvalid*);
bool RtPrepareWorldMaterial(reshade::api::command_list*,const RtLocalProgram&,const RtWorldSurface&,
    const WWMI_RtLocalVfSurface_v1&,const WWMI_RtPreviousSurface_v1&,const std::vector<ShadowSlotOvr>&,
    const std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>&,WWMI_RtPackReserveDrawBindingFn_v1,RtMaterialTables&,uint64_t&,
    WWMI_RtPackReserveDrawBindingFn_v2,RtHistoryInvalid*);
