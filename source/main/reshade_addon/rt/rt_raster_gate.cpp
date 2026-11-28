#include "rt_raster_gate.h"
#include "rt_depth_route.h"
#include "rt_pack_frame.h"
#include "rt_stream_section.h"
#include "rt_localvf_inputs.h"
#include "rt_aux_proof_note.hpp"
#include "rt_first_masked_port.hpp"
#include "rt_program_runtime.h"
#include "rt_material_tables.h"
#include "rt_draw_frame.h"
#include "rt_hybrid_raster.hpp"
#include "rt_author_position_origin.hpp"
#include <cmath>
#include "rt_draw_identity_receipt.h"
#include "rt_torso_ps.h"
#include "rt_native_binding_restore.h"
#include "wwmi_compute_state.h"
#include "rt_history_roots.h"
#include "rt_material_layout.h"
#include "rt_author_rest.hpp"
#include "rt_author_section_frame.h"
#include "rt_author_work_receipt.hpp"
#include "rt_material_replica.hpp"
#include "rt_draw_window.h"
#include "rt_raster_effects.h"
#include "draw/draw_parts.h"
#include "rt_native_palette.h"
#include "mods/layout_registry.h"
#include "mods/ib_hash.h"
#include "mods/port_section.h"
#include "migoto/port_api.h"
namespace PortApi { void RunSiblingHashMarkers(unsigned ib_hash); }
#include <vector>
#include <utility>
#include "rt_geometry_plan.h"
#include "addon_common.h"
#include "draw/draw_ctx.h"
#include "draw/draw_inject.h"
#include "mods/claim_state.h"
#include "hooks/pipeline_hooks.h"
#include "hooks/descriptor_shadow.h"
#include "published_descriptor.h"
#include "observation/binding_client/wwmi_binding_client.hpp"
#include <map>
#include <optional>
#include <set>
#include <string>
#include <mutex>
#include <memory>
#include <array>
#include <tuple>
#include <algorithm>
#include <sstream>
#include <cstring>
#include "rt_author_choice_note.hpp"
#include "rt_first_masked_contract.hpp"

#include "rt_native_binding_restore.inc"
#include "rt_material_draw_note.inc"

uint32_t RtSetAuxNativeVertexEnd(uint32_t end) noexcept;

namespace {
struct RtAuxVertexEndGuard {
    uint32_t prev;
    explicit RtAuxVertexEndGuard(uint32_t end) noexcept : prev(RtSetAuxNativeVertexEnd(end)) {}
    ~RtAuxVertexEndGuard() noexcept { RtSetAuxNativeVertexEnd(prev); }
};
struct Ia {
    uint32_t known=0,nulls=0,defaults=0;bool ib_known=false,ib_null=false;
    uint64_t creation=0,recording=0,binding_sequence=0;
    D3D12_VERTEX_BUFFER_VIEW vb[8]{};D3D12_INDEX_BUFFER_VIEW ib{};
};
struct Reader {
    HMODULE module{};
    WWMI_RtPackIdentifyDrawFn_v2 identify{};
    WWMI_RtPackIdentifyDrawReceiptFn_v1 identify_receipt{};
    WWMI_RtPackIdentifyAuxSkipDrawFn_v1 identify_aux_skip{};
    WWMI_RtPackAcquireNativePaletteFn_v1 palette{};
    WWMI_RtPackAcquireDrawFrameFn_v1 frame{};
    WWMI_RtPackAcquireDrawFrameFn_v2 frame2{};
    WWMI_RtPackReserveDrawBindingFn_v1 reserve{};
    WWMI_RtPackReserveDrawBindingFn_v2 reserve2{};
    WWMI_RtPackNoteDrawResetFn_v1 reset{};
    WWMI_RtPackReadAuthorSnapshotFn_v1 author_snapshot{};
    WWMI_RtPackAcquireAuthorPaletteFn_v1 author_palette{};
    WWMI_RtPackBeginAuthorScopeFn_v2 author_scope{};
    WWMI_RtPackAcquireAuthorFrameFn_v2 author_frame{};
    WWMI_RtPackCommitAuthorDrawFn_v1 author_commit{};
    RtAuthorCommitNoteFn author_commit_note{};
    WWMI_RtPackObserveAuthorSectionFn_v1 author_work{};
    wwmi::hybrid::Probe hybrid_probe{};
    wwmi::hybrid::Acquire hybrid_acquire{};
    wwmi::hybrid::Commit hybrid_commit{};
    CaptureNativeBindingRestore capture_restore{};
    bool single_node=false;
    bool coherent() const {return author_snapshot&&author_palette&&author_scope&&author_frame&&author_commit;}
    decltype(&WWMI_BindingTrace_Unregister_v1) unreg{};
    ~Reader(){if(module)FreeLibrary(module);}
};
uint32_t RtCommitAuthor(const Reader& reader,const WWMI_RtAuthorCommit_v1& choice,RtAuthorChoiceSite site,
    unsigned reason,uint64_t program,unsigned surface=0,unsigned phase=0,unsigned geometry=0){
    if(reader.author_commit_note){
        const RtAuthorCommitNote note{choice,site,reason,surface,phase,geometry,program};
        return reader.author_commit_note(&note,sizeof(note));
    }
    return reader.author_commit(&choice,sizeof(choice));
}
std::atomic<std::shared_ptr<Reader>> paired;
std::mutex mutex;
std::map<uint64_t,Ia> views;
// The port's ordinary path remembers slots actually read on this command
// list. A private draw needs only their formats, not a borrowed resource.
using RtFormatOwner=std::tuple<unsigned,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,std::array<uint32_t,12>>;
struct RtSampledPsFormat {
    unsigned value=0;
    uint64_t seq=0,frame=0,vs=0,ps=0;
};
using RtPsCarrySources=std::array<RtSampledPsFormat,9>;
struct RtNativeFormatView {
    uint64_t resource=0,cpu_view=0;
    uint32_t view_generation=0,format=0,type=0,first_mip=0,mip_count=0,first_layer=0,layer_count=0;
    bool operator==(const RtNativeFormatView&) const=default;
};
struct RtNativePsFormat {
    RtNativeFormatView view;
    RtSampledPsFormat provenance;
    uint64_t observation=0;
};
struct RtFormatEntry {
    std::array<unsigned,16> formats{};
    std::unique_ptr<RtPsCarrySources> sources; // present only for an armed scope
    std::unique_ptr<std::array<RtNativePsFormat,16>> observed;
};
struct RtFormatList {
    uint64_t generation=0,observation=0;
    std::map<RtFormatOwner,RtFormatEntry> formats;
};
std::map<uint64_t,RtFormatList> format_lists;
// Existing host tracker supplies the actual D3D12 view extents and nulls.
// Do not replace short/zero-stride views with allocation-sized guesses.
void __cdecl event(const WWMI_BindingRecord_v1* e,void* user) {
    if(!e || e->input.kind==WWMI_BIND_DRAW_BEFORE || e->input.kind==WWMI_BIND_DRAW_AFTER)return;
    if(e->input.kind==WWMI_BIND_LIST_RESET && e->input.source==1)
        static_cast<Reader*>(user)->reset(e->input.native_list,e->input.hresult);
    std::lock_guard lk(mutex);
    if(e->input.kind==WWMI_BIND_LIST_DESTROY || e->recording_state!=1){views.erase(e->input.native_list);format_lists.erase(e->input.native_list);return;}
    if(e->input.kind==WWMI_BIND_LIST_CREATE || e->input.kind==WWMI_BIND_LIST_RESET || e->input.kind==WWMI_BIND_INVALIDATE)
        format_lists.erase(e->input.native_list);
    auto& s=views[e->input.native_list];
    if(e->input.kind==WWMI_BIND_LIST_RESET && e->input.hresult>=0)s.defaults=0xff;
    else if(e->input.kind==WWMI_BIND_LIST_CREATE && e->input.initial_state==WWMI_BIND_INITIAL_STATE_RECORDING)s.defaults=0xff;
    else if(e->input.kind==WWMI_BIND_INVALIDATE)s.defaults=0;
    s.defaults&=~e->vertex_known_mask;
    s.known=e->vertex_known_mask|s.defaults;s.nulls=e->vertex_null_mask|s.defaults;
    s.ib_known=e->index_known;s.ib_null=e->index_null;
    s.creation=e->creation_id;s.recording=e->recording_generation;s.binding_sequence=e->sequence;
    for(unsigned i=0;i<8;++i){auto &v=e->vertex_views[i];s.vb[i]={v.gpu_va,v.size_bytes,v.stride_or_format};}
    s.ib={e->index_view.gpu_va,e->index_view.size_bytes,static_cast<DXGI_FORMAT>(e->index_view.stride_or_format)};
}
// Programs with decoded current/previous palette semantics, independent of skin.
// A LocalVF/material pass with a different contract remains native here.
bool palette_vs(uint64_t vs) {
    return vs==0xdd68add8a369a3e9ULL || vs==0x4d60551e6a573cfcULL ||
           vs==0xe60bbf3b2d1e36d0ULL || vs==0x08dff51d98f468a5ULL;
}
}
bool RtRasterAttach(){
    if(paired.load())return true;
    auto r=std::make_shared<Reader>();
    if(!GetModuleHandleExW(0,L"zz_wwmi_rt_pack.addon64",&r->module))return false;
    r->identify=reinterpret_cast<WWMI_RtPackIdentifyDrawFn_v2>(GetProcAddress(r->module,"WWMI_RtPackIdentifyDraw_v2"));
    r->identify_receipt=reinterpret_cast<WWMI_RtPackIdentifyDrawReceiptFn_v1>(GetProcAddress(r->module,"WWMI_RtPackIdentifyDrawReceipt_v1"));
    r->identify_aux_skip=reinterpret_cast<WWMI_RtPackIdentifyAuxSkipDrawFn_v1>(GetProcAddress(r->module,"WWMI_RtPackIdentifyAuxSkipDraw_v1"));
    r->palette=reinterpret_cast<decltype(r->palette)>(GetProcAddress(r->module,"WWMI_RtPackAcquireNativePalette_v1"));
    r->frame=reinterpret_cast<decltype(r->frame)>(GetProcAddress(r->module,"WWMI_RtPackAcquireDrawFrame_v1"));
    r->frame2=reinterpret_cast<decltype(r->frame2)>(GetProcAddress(r->module,"WWMI_RtPackAcquireDrawFrame_v2"));
    r->reserve=reinterpret_cast<decltype(r->reserve)>(GetProcAddress(r->module,"WWMI_RtPackReserveDrawBinding_v1"));
    r->reserve2=reinterpret_cast<decltype(r->reserve2)>(GetProcAddress(r->module,"WWMI_RtPackReserveDrawBinding_v2"));
    r->reset=reinterpret_cast<decltype(r->reset)>(GetProcAddress(r->module,"WWMI_RtPackNoteDrawReset_v1"));
    r->author_snapshot=reinterpret_cast<decltype(r->author_snapshot)>(GetProcAddress(r->module,"WWMI_RtPackReadAuthorSnapshot_v1"));
    r->author_palette=reinterpret_cast<decltype(r->author_palette)>(GetProcAddress(r->module,"WWMI_RtPackAcquireAuthorPalette_v1"));
    r->author_scope=reinterpret_cast<decltype(r->author_scope)>(GetProcAddress(r->module,"WWMI_RtPackBeginAuthorScope_v2"));
    r->author_frame=reinterpret_cast<decltype(r->author_frame)>(GetProcAddress(r->module,"WWMI_RtPackAcquireAuthorFrame_v2"));
    r->author_commit=reinterpret_cast<decltype(r->author_commit)>(GetProcAddress(r->module,"WWMI_RtPackCommitAuthorDraw_v1"));
    r->author_commit_note=reinterpret_cast<RtAuthorCommitNoteFn>(GetProcAddress(r->module,"WWMI_RtPackCommitAuthorDrawNote_v1"));
    r->hybrid_probe=reinterpret_cast<decltype(r->hybrid_probe)>(GetProcAddress(r->module,"WWMI_RtPackProbeHybridRaster_v1"));
    r->hybrid_acquire=reinterpret_cast<decltype(r->hybrid_acquire)>(GetProcAddress(r->module,"WWMI_RtPackAcquireHybridRaster_v1"));
    r->hybrid_commit=reinterpret_cast<decltype(r->hybrid_commit)>(GetProcAddress(r->module,"WWMI_RtPackCommitHybridRaster_v1"));
    r->author_work=reinterpret_cast<decltype(r->author_work)>(GetProcAddress(r->module,"WWMI_RtPackObserveAuthorSection_v1"));
    r->single_node=g_d3d12_device&&g_d3d12_device->GetNodeCount()==1;
    extern HMODULE g_wwmi_host_module;
    auto host=g_wwmi_host_module?g_wwmi_host_module:GetModuleHandleW(L"dxgi.dll");
    r->capture_restore=reinterpret_cast<CaptureNativeBindingRestore>(GetProcAddress(host,"WWMI_ComputeState_CaptureRestore_v1"));
    auto reg=reinterpret_cast<decltype(&WWMI_BindingTrace_Register_v1)>(GetProcAddress(host,"WWMI_BindingTrace_Register_v1"));
    auto reg_ex=reinterpret_cast<decltype(&WWMI_BindingTrace_RegisterEx_v1)>(GetProcAddress(host,"WWMI_BindingTrace_RegisterEx_v1"));
    r->unreg=reinterpret_cast<decltype(r->unreg)>(GetProcAddress(host,"WWMI_BindingTrace_Unregister_v1"));
    if((!reg && !reg_ex) || !r->unreg || !r->identify || !r->palette || !r->frame || !r->reserve || !r->reset ||
       !r->capture_restore)return false;
    const int32_t reg_rc=reg_ex?reg_ex(event,r.get(),WWMI_BIND_REG_SKIP_DRAWS):reg(event,r.get());
    if(reg_rc!=0)return false;
    paired.store(std::move(r));return true;
}
extern "C" __declspec(dllexport) uint32_t WWMI_RtAuthorPlanConsumer_v1(uint64_t device){
    const auto r=paired.load();
    return r&&r->coherent()&&device&&device==wwmi_rt_pack_frame_device.load();
}
static void RtVisibleFieldStop();

// Uncapped, lock-free aggregate counters for pre-scope silent identity misses
// and post-scope draw outcomes. Zero hot-path locks; emits 1 summary line per
// active mod on detach and at most once every 15 seconds when counts advance.
struct RtGateModCounters {
    std::atomic<uint64_t> pre_shape{0}, pre_views{0}, pre_ia{0}, id_fail{0};
    std::atomic<uint64_t> id_reason[12]{};
    std::atomic<uint64_t> id_comp[8]{};
    std::atomic<uint64_t> early[4]{};   // 5:admit, 6:depth/rtv, 7:restore, 8:palette
    std::atomic<uint64_t> refuse[5]{};  // 5:masked, 10:disabled, 11:world, 12:inputs, 14/other:vm
    std::atomic<uint64_t> ok_priv{0}, ok_hyb{0}, ok_hid{0}, native_pass{0}, plan_viol{0};
    uint64_t snapshot_total() const noexcept {
        return pre_shape.load(std::memory_order_relaxed)+pre_views.load(std::memory_order_relaxed)+
            pre_ia.load(std::memory_order_relaxed)+id_fail.load(std::memory_order_relaxed)+
            ok_priv.load(std::memory_order_relaxed)+ok_hyb.load(std::memory_order_relaxed)+
            ok_hid.load(std::memory_order_relaxed)+native_pass.load(std::memory_order_relaxed)+
            plan_viol.load(std::memory_order_relaxed);
    }
};
constexpr int kRtGateModCap=64;
static RtGateModCounters g_rt_gate_counts[kRtGateModCap];
static std::atomic<uint64_t> g_rt_gate_last_emit_ms{0};
static std::atomic<uint64_t> g_rt_gate_last_total{0};
static std::atomic<bool> g_rt_reloading{false};
static std::atomic<uint32_t> g_rt_active_draws{0};
void RtBeginCatalogReload() noexcept {
    g_rt_reloading.store(true,std::memory_order_seq_cst);
    g_merged_loaded=false;
    while(g_rt_active_draws.load(std::memory_order_acquire)!=0)YieldProcessor();
}
void RtEndCatalogReload() noexcept {
    g_rt_reloading.store(false,std::memory_order_release);
}
void RtResetGateCounts() noexcept {
    for(auto& c:g_rt_gate_counts){
        c.pre_shape.store(0,std::memory_order_relaxed);
        c.pre_views.store(0,std::memory_order_relaxed);
        c.pre_ia.store(0,std::memory_order_relaxed);
        c.id_fail.store(0,std::memory_order_relaxed);
        for(auto& v:c.id_reason)v.store(0,std::memory_order_relaxed);
        for(auto& v:c.id_comp)v.store(0,std::memory_order_relaxed);
        for(auto& v:c.early)v.store(0,std::memory_order_relaxed);
        for(auto& v:c.refuse)v.store(0,std::memory_order_relaxed);
        c.ok_priv.store(0,std::memory_order_relaxed);
        c.ok_hyb.store(0,std::memory_order_relaxed);
        c.ok_hid.store(0,std::memory_order_relaxed);
        c.native_pass.store(0,std::memory_order_relaxed);
        c.plan_viol.store(0,std::memory_order_relaxed);
    }
    g_rt_gate_last_total.store(0,std::memory_order_relaxed);
}
static RtGateModCounters* RtGateCountFor(int mod) noexcept {
    return (mod>=0&&mod<kRtGateModCap)?&g_rt_gate_counts[mod]:nullptr;
}
static void RtGateEmitSummary(bool force) noexcept {
    const uint64_t now=GetTickCount64();
    if(!force){
        const uint64_t last=g_rt_gate_last_emit_ms.load(std::memory_order_relaxed);
        if(now-last<15000)return;
    }
    uint64_t total=0;
    for(const auto& c:g_rt_gate_counts)total+=c.snapshot_total();
    if(!total)return;
    if(!force){
        const uint64_t last=g_rt_gate_last_emit_ms.load(std::memory_order_relaxed);
        if(now-last<15000||total==g_rt_gate_last_total.load(std::memory_order_relaxed))return;
        uint64_t expected=last;
        if(!g_rt_gate_last_emit_ms.compare_exchange_strong(expected,now,std::memory_order_relaxed))return;
    }else{
        g_rt_gate_last_emit_ms.store(now,std::memory_order_relaxed);
    }
    g_rt_gate_last_total.store(total,std::memory_order_relaxed);
    auto L=[](const std::atomic<uint64_t>& v){return (unsigned long long)v.load(std::memory_order_relaxed);};
    for(int m=0;m<kRtGateModCap;++m){
        const auto& c=g_rt_gate_counts[m];
        if(!c.snapshot_total())continue;
        const uint32_t guid=(m<int(g_geomods.size()))?g_geomods[m].mod.object_guid:0u;
        logger::probe("RT-GATE-SUMMARY phase=%s mod=%d guid=%u pre=%llu/%llu/%llu id_fail=%llu "
            "id_reasons=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu "
            "id_comps=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu "
            "early5678=%llu,%llu,%llu,%llu refuse=%llu,%llu,%llu,%llu,%llu "
            "ok_priv=%llu ok_hyb=%llu ok_hid=%llu native=%llu plan_viol=%llu",
            force?"detach":"periodic",m+1,guid,L(c.pre_shape),L(c.pre_views),L(c.pre_ia),L(c.id_fail),
            L(c.id_reason[0]),L(c.id_reason[1]),L(c.id_reason[2]),L(c.id_reason[3]),
            L(c.id_reason[4]),L(c.id_reason[5]),L(c.id_reason[6]),L(c.id_reason[7]),
            L(c.id_reason[8]),L(c.id_reason[9]),L(c.id_reason[10]),L(c.id_reason[11]),
            L(c.id_comp[0]),L(c.id_comp[1]),L(c.id_comp[2]),L(c.id_comp[3]),
            L(c.id_comp[4]),L(c.id_comp[5]),L(c.id_comp[6]),L(c.id_comp[7]),
            L(c.early[0]),L(c.early[1]),L(c.early[2]),L(c.early[3]),
            L(c.refuse[0]),L(c.refuse[1]),L(c.refuse[2]),L(c.refuse[3]),L(c.refuse[4]),
            L(c.ok_priv),L(c.ok_hyb),L(c.ok_hid),L(c.native_pass),L(c.plan_viol));
    }
}

void RtRasterDetach(){
    auto r=paired.exchange(nullptr);
    if(r){
        r->unreg(event,r.get());{std::lock_guard lk(mutex);views.clear();format_lists.clear();}
        RtVisibleFieldStop();
        RtGateEmitSummary(true);
        RtMaterialDrawFlush();
    }
}
#include "rt_visible_window.inc"
#include "rt_previous_cloth_trace.inc"
#include "rt_native_ps_formats.inc"
#include "rt_stream_section.inc"
bool RtDrawComponent(InjectFrame&,int,const WwmiMod::Component*,RtDrawExecution&);
enum class MaskedMaterial {none,captured,sibling};
static MaskedMaterial RtMaskedDepthMaterialPass(uint64_t vs,const CmdRT& rt,const CmdState& gfx,
    const RtRasterEffects& e,uint64_t layout,RtMaskedPixelReads& reads,const char*& first){
    const auto contract=RtProgramContractForPso(gfx.gfx_pso,vs);
    const char* automatic_refusal=!contract?"masked-contract":!contract->capabilities.current_previous?"masked-capability":
        !contract->bindings.valid?"masked-root-plan":contract->native_layout!=layout?"masked-layout":
        !contract->masked_material_domain?"masked-target-domain":!contract->only_vertex_pixel?"masked-stages":nullptr;
    const bool automatic=!automatic_refusal;
    const auto refuse=[&](const char* why){first=why;return MaskedMaterial::none;};
    const auto expected_ps=vs==0xdaed5c8b5efa34baULL?0x798b8d626000aa93ULL:
                           vs==0xafbc4278dae25417ULL?0xfe69868a66f5a7c2ULL:0ULL;
    if(!automatic&&!expected_ps)return refuse(automatic_refusal);
    if(!gfx.gfx_pso)return refuse("masked-no-native-pso");
    if(gfx.gfx_restore()!=gfx.gfx_pso)return refuse("masked-effective-pso");
    if(!rt.valid)return refuse("masked-om-unobserved");
    if(!rt.has_dsv||!rt.dsv)return refuse("masked-dsv");
    const bool is_gbuffer_7=(rt.rtv_count==7);
    const bool is_depth_prepass_1=(rt.rtv_count==1&&automatic);
    if(!is_gbuffer_7&&!is_depth_prepass_1)return refuse("masked-rtv-count");
    const uint8_t expected_slots=is_gbuffer_7?0x7fu:0x01u;
    if(e.color!=RtColorKnowledge::KnownZero)return refuse("masked-color");
    if(e.basis!=RtColorBasis::Reflected)return refuse("masked-color-basis");
    if(!e.standard_pixel_shape)return refuse("masked-pixel-shape");
    if(e.declared_slots!=expected_slots)return refuse("masked-declared-slots");
    if(e.attached_slots!=expected_slots)return refuse("masked-attached-slots");
    if(e.write_slots&e.attached_slots)return refuse("masked-write-slots");
    if(e.depth_write!=RtEffect::Possible)return refuse("masked-depth-write");
    if(is_gbuffer_7&&e.stencil_write!=RtEffect::Possible)return refuse("masked-stencil-write");
    if(is_depth_prepass_1&&e.stencil_write!=RtEffect::Absent&&e.stencil_write!=RtEffect::Possible)
        return refuse("masked-stencil-write");
    if(e.pixel_depth||e.pixel_stencil_ref||e.pixel_coverage)return refuse("masked-pixel-outputs");
    if(e.uav!=RtEffect::Absent)return refuse("masked-uav");
    if(e.stream_output!=RtEffect::Absent)return refuse("masked-stream-output");
    uint64_t bound_vs=0,ps=0,gs=0;
    PsoBoundHashes(gfx.gfx_pso,&bound_vs,&ps,&gs);
    if(bound_vs!=vs||gs)return refuse("masked-bound-stages");
    if(automatic){
        if(contract->pixel->native_identity!=ps)return refuse("masked-ps-identity");
        if(contract->target_count!=rt.rtv_count)return refuse("masked-contract-target-count");
        reads=contract->pixel->pixel_srv_reads;
        // The same strict root/PS read no-alias guard below checks this real
        // native material. No-history is separately required by its own path.
        return MaskedMaterial::sibling;
    }
    if(ps==expected_ps)
        return RtOnlyVertexPixelStages(gfx.gfx_pso)?MaskedMaterial::captured:refuse("masked-legacy-stages");
    const bool sibling=(vs==0xdaed5c8b5efa34baULL&&ps==0x8be56796e7031cdfULL)||
        (vs==0xafbc4278dae25417ULL&&ps==0x63e973159c634169ULL);
    if(!sibling)return refuse("masked-legacy-identity");
    if(!RtOnlyVertexPixelStages(gfx.gfx_pso))return refuse("masked-legacy-stages");
    return RtReadProvenMaskedSibling(gfx.gfx_pso,layout,vs,ps,reads)?
        MaskedMaterial::sibling:refuse("masked-legacy-proof");
}
static bool RtPreviousVaryingUnobservable(const RtProgramPsoContract* contract,const RtLocalProgram* program,
    const CmdRT& rt,const CmdState& gfx,const RtRasterEffects& effects,uint64_t layout,uint64_t ps){
    // An open VS gate is not a demand for history when its only consumer
    // would be a varying the original PS never loads. This is not a
    // zero-velocity fallback: the proven pass has no velocity output.
    return contract&&program==&contract->local&&contract->vertex&&contract->pixel&&ps&&
        contract->native_pso==gfx.gfx_pso&&gfx.gfx_pso==gfx.gfx_restore()&&contract->native_layout==layout&&
        contract->pixel->native_identity==ps&&contract->only_vertex_pixel&&
        contract->capabilities.current_previous&&contract->capabilities.pixel_unused_previous&&
        contract->vertex->previous_only_velocity&&contract->bindings.valid&&contract->bindings.previous_vertex_only&&
        rt.valid&&(rt.rtv_count==1||rt.rtv_count==2)&&rt.rtv[0]&&(rt.rtv_count==1||rt.rtv[1])&&contract->target_count==rt.rtv_count&&
        effects.stream_output==RtEffect::Absent&&effects.uav==RtEffect::Absent;
}
static bool RtEvalAuxSkipForHash(reshade::api::command_list* list,uint64_t fc_key,int head_hint,uint32_t ib_hash,
    uint32_t first,uint32_t count,uint32_t instances,uint32_t first_instance,uint64_t gfx_pso,const char* src_tag){
    if(!ib_hash)return false;
    bool any_skip=false;
    PortApi::EngineScope engine;
    uint16_t pushed_ps=0;
    if(list){
        const uint64_t layh=CmdGfxLayout(list);
        RegBind t_ps[kSectionTexRegs]{};
        bool has_layout=false;
        {
            std::lock_guard l(g_layout_regmap_mtx);
            auto it=g_layout_regmap.find(layh);
            if(it!=g_layout_regmap.end()&&it->second.built){
                memcpy(t_ps,it->second.t_ps,sizeof t_ps);
                has_layout=true;
            }
        }
        if(has_layout){
            reshade::api::device* dev=list->get_device();
            const auto& root=CmdGfx(list).root;
            for(unsigned r=0;r<kSectionTexRegs;++r){
                const RegBind& b=t_ps[r];
                if(b.param==0xFF||b.param>=64||!b.is_table)continue;
                const TableStart ts=TableStartOf(dev,root[b.param].table);
                if(!ts)continue;
                const HeapKey key{ts.heap,ts.base+b.offset};
                ResInfo ri{};
                if(!ReadPublishedDescriptor(key,ri)||!ri.res)continue;
                const uint32_t resident=TexHashOf(reinterpret_cast<uint64_t>(ri.res));
                const uint32_t hash=(!resident||PortApi::TextureHashHasSections(resident))?resident:TexLodTargetOf(resident);
                const unsigned fmt=unsigned(PublishedViewFormat(key));
                PortApi::PushGameTexture('p',r,ri.res,hash?hash:resident,fmt);
                pushed_ps|=uint16_t(1u<<r);
            }
        }
    }
    struct UnpushPsGuard {
        uint16_t mask;
        ~UnpushPsGuard(){
            for(unsigned r=0;r<kSectionTexRegs;++r)
                if(mask&(1u<<r))PortApi::UnpushGameTexture('p',r);
        }
    } unpush_ps{pushed_ps};
    auto eval_slot=[&](int idx){
        const auto &mc=g_merged_comps[idx];
        if(mc.mod_id<0||mc.mod_id>=int(g_geomods.size())||mc.vanilla||mc.hide)return;
        const auto &gm=g_geomods[mc.mod_id];
        if(!gm.aux_skip_only||!gm.geom_enabled)return;
        const auto *wc=WmodComp(mc.mod_id,mc.comp_idx);
        if(!wc||wc->ib_hash!=ib_hash)return;
        const std::wstring fq=PortComponentSection(mc.mod_id,wc->index,wc->tail,gm.wcl_ns);
        if(fq.empty())return;
        PortApi::DrawInfo di{};
        di.index_count=count;
        di.instance_count=instances;
        di.first_index=first;
        di.first_instance=first_instance;
        di.indexed=true;
        di.pso=gfx_pso;
        PortApi::RunSectionThenPost(fq.c_str(),&di);
        if(di.skip){
            any_skip=true;
            if(auto *mc_cnt=RtGateCountFor(mc.mod_id))
                mc_cnt->ok_hid.fetch_add(1,std::memory_order_relaxed);
            static std::atomic<uint32_t> s_aux_log{0};
            if(s_aux_log.fetch_add(1,std::memory_order_relaxed)<16)
                logger::probe("RT-AUX-SKIP src=%s mod=%d ns='%ls' tail='%ls' hash=%08x first=%u count=%u",
                    src_tag,mc.mod_id,gm.wcl_ns.c_str(),wc->tail.c_str(),ib_hash,first,count);
        }else if(auto *mc_cnt=RtGateCountFor(mc.mod_id)){
            mc_cnt->native_pass.fetch_add(1,std::memory_order_relaxed);
        }
    };
    if(const auto sh=g_first_to_shared.find(fc_key);sh!=g_first_to_shared.end()){
        for(const int cand:sh->second)eval_slot(cand);
    }else{
        eval_slot(head_hint);
    }
    return any_skip;
}
static RtDrawOutcome RtSelectGpuSkinDraw(reshade::api::command_list *list,uint32_t count,uint32_t instances,uint32_t first,int32_t base,uint32_t first_instance){
    extern std::atomic<bool> g_rt_user_disabled;
    if(!g_rt_blocked||g_faulted||g_rt_user_disabled.load(std::memory_order_acquire)||!g_is_dx12||!g_merged_loaded||!g_port_draw_list||!g_wcl_draw||!list||
       !count||!g_d3d12_device||g_rt_reloading.load(std::memory_order_acquire)||
       wwmi_rt_pack_frame_device.load()!=reinterpret_cast<uint64_t>(g_d3d12_device))return {};
    struct RtActiveDrawGuard {
        RtActiveDrawGuard() noexcept {g_rt_active_draws.fetch_add(1,std::memory_order_acq_rel);}
        ~RtActiveDrawGuard() noexcept {g_rt_active_draws.fetch_sub(1,std::memory_order_release);}
    } active_draw_guard;
    if(g_rt_reloading.load(std::memory_order_acquire)||!g_merged_loaded)return {};
    const bool shape_ok=instances==1&&!first_instance&&!base;
    if(!shape_ok&&field_state.load(std::memory_order_relaxed)!=1)return {};
    const uint64_t fc_key=FirstCountKey(first,count);
    auto slot=g_first_to_comp.find(fc_key);if(slot==g_first_to_comp.end())return {};
    rt_phase_cpu::Scope gate_cpu(rt_phase_cpu::rt_gate_select);
    const bool gate_active=gate_cpu.ticket.start!=0;
    rt_phase_cpu::GateActiveGuard gate_flag(gate_active);
    PortApi::NextDrawCall();
    const auto component_hint=g_merged_comps[slot->second].comp_idx;
    const int mod_hint=g_merged_comps[slot->second].mod_id;
    auto* hint_counts=RtGateCountFor(mod_hint);
    RtGateEmitSummary(false);
    RtClothDraw cloth(component_hint);
    cloth.capture(list,first,count,instances,base,first_instance);
    if(!shape_ok){if(hint_counts)hint_counts->pre_shape.fetch_add(1,std::memory_order_relaxed);cloth.fail(25);return {};}
    RtFieldDraw field(component_hint>=0);
    if(field.active){list->get_private_data(s_iid_vs_hash,&field.key.vs);const auto rt=CmdRt(list);field.key.pso=CmdGfx(list).gfx_pso;field.key.rts=rt.rtv_count;field.key.depth=rt.dsv!=0;}

    auto r=paired.load();if(!r){cloth.fail(1);return {};}
    auto *cl=reinterpret_cast<ID3D12GraphicsCommandList*>(list->get_native());Ia ia;
    {std::lock_guard lk(mutex);auto i=views.find(reinterpret_cast<uint64_t>(cl));if(i==views.end()){if(hint_counts)hint_counts->pre_views.fetch_add(1,std::memory_order_relaxed);cloth.fail(2);return {};}ia=i->second;}
    cloth.capture_ia(ia);
    const uint64_t ib_stride=ia.ib.Format==DXGI_FORMAT_R32_UINT?4ull:(ia.ib.Format==DXGI_FORMAT_R16_UINT?2ull:0ull);
    if(!(ia.known&1)||(ia.nulls&1)||!ia.ib_known||ia.ib_null||!ib_stride||
       (uint64_t(first)+count)*ib_stride>ia.ib.SizeInBytes){if(hint_counts)hint_counts->pre_ia.fetch_add(1,std::memory_order_relaxed);cloth.fail(3);return {};}
    WWMI_RtPackDrawQuery_v2 q{};
    q.struct_size=sizeof(q);q.abi_version=2;q.index_format=ia.ib.Format;q.vertex_stride=ia.vb[0].StrideInBytes;
    q.index_count=count;q.first_index=first;q.base_vertex=base;
    q.device=reinterpret_cast<uint64_t>(g_d3d12_device);q.list=reinterpret_cast<uint64_t>(list);
    q.vertex_buffer_gpu_va=ia.vb[0].BufferLocation;q.vertex_buffer_bytes=ia.vb[0].SizeInBytes;
    q.index_buffer_gpu_va=ia.ib.BufferLocation;q.index_buffer_bytes=ia.ib.SizeInBytes;
    WWMI_RtPackDrawIdentity_v2 id{};
    WWMI_RtAuthorSnapshot_v1 author{};
    uint64_t vs=0;list->get_private_data(s_iid_vs_hash,&vs);const CmdRT rt=CmdRt(list);
    auto& g=CmdGfx(list);
    const bool gpu_palette=palette_vs(vs);
    const bool author_consumer=r->coherent()&&!gpu_palette&&r->single_node&&cl->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT;
    field.key.path=1; // first/count candidate only, identity not proved
    WWMI_RtDrawIdentityReceipt_v1 local_receipt{};local_receipt.struct_size=sizeof(local_receipt);local_receipt.abi_version=1;
    auto *receipt=cloth.row?&cloth.row->receipt:&local_receipt;
    const auto identified=[&]{
        rt_phase_cpu::ConditionalScope id_cpu(gate_active,rt_phase_cpu::gate_identify);
        return author_consumer?r->author_snapshot(&q,sizeof(q),&author,sizeof(author),receipt,sizeof(*receipt)):
            (receipt&&r->identify_receipt)?r->identify_receipt(&q,sizeof(q),&id,sizeof(id),receipt,sizeof(*receipt)):
            r->identify(&q,sizeof(q),&id,sizeof(id),nullptr,0);
    }();
    if(!identified){
        bool has_aux_skip=false;
        auto check_aux=[&](int idx){
            const auto &mc=g_merged_comps[idx];
            if(mc.mod_id<0||mc.mod_id>=int(g_geomods.size())||mc.vanilla||mc.hide)return;
            const auto &gm=g_geomods[mc.mod_id];
            if(gm.aux_skip_only&&gm.geom_enabled)has_aux_skip=true;
        };
        if(const auto sh=g_first_to_shared.find(fc_key);sh!=g_first_to_shared.end()){
            for(const int cand:sh->second)check_aux(cand);
        }else{
            check_aux(slot->second);
        }
        if(has_aux_skip){
            uint32_t aux_hash=0;
            const uint32_t aux_state=r->identify_aux_skip?r->identify_aux_skip(&q,sizeof(q),&aux_hash):0u;
            if(aux_state==2u&&aux_hash){
                const bool skipped=RtEvalAuxSkipForHash(list,fc_key,slot->second,aux_hash,first,count,instances,first_instance,g.gfx_pso,"skincache");
                return RtDrawOutcome{skipped?RtDrawExecution::HiddenSelected:RtDrawExecution::NativeSelected,false};
            }
            if(aux_state==0u&&g.ib.res&&g.vb[0].res&&g.vb[0].stride){
                const uint64_t ib_base=g.ib.res->GetGPUVirtualAddress();
                const uint64_t vb_base=g.vb[0].res->GetGPUVirtualAddress();
                if(g.ib.va>=ib_base&&g.vb[0].va>=vb_base&&(g.ib.va-ib_base)<=UINT32_MAX&&(g.vb[0].va-vb_base)<=UINT32_MAX){
                    const uint32_t idxb=g.ib.index_bytes?g.ib.index_bytes:uint32_t(ib_stride);
                    const uint32_t full_ib_bytes=idxb?uint32_t((uint64_t(ia.ib.SizeInBytes)/idxb)*idxb):0u;
                    const uint32_t ib_hash=full_ib_bytes?IbHash::Lookup(g.ib.res,uint32_t(g.ib.va-ib_base),full_ib_bytes,idxb,
                        g.vb[0].res,uint32_t(g.vb[0].va-vb_base),g.vb[0].stride,false):0u;
                    if(ib_hash){
                        const bool skipped=RtEvalAuxSkipForHash(list,fc_key,slot->second,ib_hash,first,count,instances,first_instance,g.gfx_pso,"ibhash");
                        if(skipped)return RtDrawOutcome{RtDrawExecution::HiddenSelected,false};
                    }
                }
            }
        }
        if(hint_counts){
            hint_counts->id_fail.fetch_add(1,std::memory_order_relaxed);
            const unsigned rsn=receipt->reason<12?receipt->reason:11u;
            hint_counts->id_reason[rsn].fetch_add(1,std::memory_order_relaxed);
            if(component_hint>=0&&component_hint<8)hint_counts->id_comp[component_hint].fetch_add(1,std::memory_order_relaxed);
        }
        cloth.capture_ps(list); // after the actual identity/fence decision, still before any INI work
        cloth.fail(4);RtWorldNote(0,0,g_merged_comps[slot->second].comp_idx,0);return {};
    }
    if(author.token)id=author.binding;
    const auto& target=id.native;
    const bool identified_component=target.object_guid && target.content_hash && target.component<=INT_MAX;
    if(identified_component){
        if(!cloth.row){cloth.start(target.component);cloth.capture(list,first,count,instances,base,first_instance);cloth.capture_ia(ia);}
        if(cloth.row){cloth.row->identified=1;cloth.row->object=target.object_guid;cloth.row->hash=target.content_hash;
            cloth.row->component=target.component;cloth.row->ordering=id.ordering;cloth.row->author_snapshot=author.token;}
        cloth.capture_ps(list);
    }
    else cloth.abandon();
    if(!identified_component)field.active=false;
    field.key.path=2; // identified object and component; pass not admitted yet
    const auto section_matches=[&](int idx){
        const auto &mc=g_merged_comps[idx];
        if(mc.mod_id<0||mc.mod_id>=int(g_geomods.size())||mc.vanilla||mc.hide||mc.comp_idx!=int(target.component))return false;
        const auto* wc=WmodComp(mc.mod_id,mc.comp_idx);
        return wc&&g_geomods[mc.mod_id].mod.object_guid==target.object_guid&&wc->ib_hash==target.content_hash;
    };
    // Multiple distinct meshes (e.g. overworld vs resonator-menu models with different IB hashes)
    // can declare the exact same (first_index, index_count) window. g_first_to_comp retains only
    // the last loaded slot and head.run_order only links peers sharing the same IB hash, so resolve
    // through g_first_to_shared against the verified (object_guid, content_hash, component) identity.
    int head_slot=slot->second;
    if(!section_matches(head_slot)){
        if(const auto sh=g_first_to_shared.find(fc_key);sh!=g_first_to_shared.end()){
            for(const int cand:sh->second){
                if(section_matches(cand)){head_slot=cand;break;}
            }
        }
    }
    auto& head=g_merged_comps[head_slot];
    if(section_matches(head_slot)&&g_geomods[head.mod_id].aux_skip_only){
        const bool skipped=RtEvalAuxSkipForHash(list,fc_key,head_slot,target.content_hash,first,count,instances,first_instance,g.gfx_pso,"blas");
        field.key.path=skipped?7:6;
        return RtDrawOutcome{skipped?RtDrawExecution::HiddenSelected:RtDrawExecution::NativeSelected,false};
    }
    const auto for_each_head_slot=[&](auto&& fn){
        if(head.run_order.empty())fn(head_slot);
        else for(auto i:head.run_order)fn(i);
    };
    // This is a native depth-writing draw, not an unclassified color/velocity
    // pass. Missing OM or PSO evidence remains native through the old gate.
    const bool depth_only=!rt.rtv_count&&rt.valid&&rt.has_dsv&&rt.dsv&&RtOpaqueDepthPass(g.gfx_pso);
    RtProgramLookupStamp admission_stamp;
    const auto contract=RtProgramContractForPsoObserved(g.gfx_pso,vs,admission_stamp);
    const auto* program=contract&&contract->capabilities.current_previous?&contract->local:RtLocalProgramFor(vs);
    const char* admission_first=program?nullptr:"local-program";
    const char* admission_world="not-run";
    std::optional<RtRasterEffects> admission_effects;
    bool masked_material=false, unused_previous=false;
    auto masked=MaskedMaterial::none;
    RtMaskedPixelReads sibling_reads{};
    if(program || cloth.row){
        uint8_t attached=0;
        for(unsigned i=0;i<(std::min)(rt.rtv_count,8u);++i)if(rt.rtv[i])attached|=uint8_t(1u<<i);
        const auto effects=RtReadRasterEffects(g.gfx_pso,attached,rt.valid,rt.has_dsv&&rt.dsv);
        if(cloth.row){cloth.row->effects=effects;cloth.row->effects_observed=true;cloth.row->effective_pso=g.gfx_restore();}
        if(program&&!depth_only&&(effects.color!=RtColorKnowledge::KnownMask||!effects.standard_pixel_shape)){
            // These two PS programs still determine depth/stencil coverage by
            // sampling and discarding. Keep the material path and exact PSO;
            // this is not the no-PS depth path or permission to omit a draw.
            admission_effects=effects;
            if(author.token)masked=RtMaskedDepthMaterialPass(vs,rt,g,effects,CmdGfxLayout(list),sibling_reads,admission_first);
            else admission_first="masked-no-author-snapshot";
            masked_material=masked!=MaskedMaterial::none;
            if(!masked_material)program=nullptr;
        }else if(!program)admission_effects=effects; // already evaluated for the cloth receipt
        if(program&&contract&&(rt.rtv_count==1||rt.rtv_count==2)){
            uint64_t ps=0;list->get_private_data(s_iid_ps_hash,&ps);
            unused_previous=RtPreviousVaryingUnobservable(contract.get(),program,rt,g,effects,CmdGfxLayout(list),ps);
        }
    }
    const bool position_evaluated=!gpu_palette&&!program;
    bool stream=!gpu_palette && (program || RtPositionOnlyPass(g.gfx_pso));
    if(cloth.row){cloth.row->stream=stream;cloth.row->depth_only=depth_only;cloth.row->palette=gpu_palette;}
    WWMI_RtAuthorInstance_v1 instance{};
    RtWorldSurface original_world;
    RtAuxProofNote original_aux;
    RtAuxSectionNotes aux_sections;
    const uint64_t aux_native_pso=g.gfx_pso;
    uint64_t original_aux_ps=0;
    const char* original_aux_refusal=nullptr;
    const auto finish_aux=[&](const RtDrawOutcome& outcome){
        if(!aux_sections.size)return;
        aux_sections.finish({vs,0,aux_native_pso,CmdGen(list),author.draw_ticket,g_frame_id,
            target.component,rt.rtv_count,first,count,base,nullptr,nullptr},
            outcome.cancels_native(),outcome.plan_violation);
    };
    if(contract&&program==&contract->local)original_world.contracts=contract;
    bool world_observed=false,world_valid=false;
    RtFormatOwner owner{target.component,target.content_creation,target.output_creation,target.position_base,0,0,0,std::array<uint32_t,12>{}};
    RtObservedPsFormats native_formats{};
    bool format_observed=false;
    if(author.token){
        rt_phase_cpu::ConditionalScope world_cpu(gate_active,rt_phase_cpu::gate_world);
        // Obtain the original shader and Primitive scope before the VM and
        // before any geometry, history or material readiness decision.
        if(program){
            world_observed=true;
            {
                RtAuxNoteScope note(original_aux);
                RtAuxVertexEndGuard aux_end(target.native_vertex_first+target.native_vertex_count);
                world_valid=RtReadWorldSurface(list,*program,target.object_guid,target.content_hash,target.component,original_world,true,r->reserve2!=nullptr,&instance);
            }
            admission_world=world_valid?"ready":"failed";
            if(original_aux.input!=RtAuxInput::none)list->get_private_data(s_iid_ps_hash,&original_aux_ps);
            if(!world_valid)original_aux_refusal="world-read-refused";
            if(world_valid&&(unused_previous||program->previous<0))original_world.reads_previous=false;
            if(world_valid&&masked==MaskedMaterial::captured&&r->reserve2)RtAllowMaskedVelocityFallback(*program,original_world);
            if(masked==MaskedMaterial::sibling){
                // Preserve the exact native history/binding checks. Only an
                // independently proved VS/PS/RT row may own native-gate no-history.
                if(!world_valid||!RtProveMaskedSiblingBindings(list,*program,original_world,sibling_reads)){
                    admission_first=world_valid?"masked-sibling-bindings":"masked-world-read";
                    if(!original_aux_refusal)original_aux_refusal="masked-sibling-bindings";
                    world_valid=false;stream=false;program=nullptr;
                    if(cloth.row)cloth.row->stream=false;
                }
            }
        }
        if(!instance.resource)RtReadAuthorInstance(list,RtPrimitiveRegister(g.gfx_pso),instance);
    }
    // Do not disturb an existing palette pass or an ambiguous Mod stack.
    int hybrid_mod=-1, matched=0;
    const hybrid::HistoryRead previous{original_world.prev_resource,original_world.prev_offset,original_world.prev_bytes};
    if(stream&&author.token&&instance.resource&&r->hybrid_probe&&r->hybrid_acquire&&r->hybrid_commit){
        rt_phase_cpu::ConditionalScope probe_cpu(gate_active,rt_phase_cpu::gate_identify);
        const auto candidate=[&](int idx){
            if(!section_matches(idx))return;
            const auto mod=g_merged_comps[idx].mod_id;const auto &gm=g_geomods[mod];
            if(!gm.geom_enabled)return;
            ++matched;
            const bool force_hybrid=!gm.has_merged_skeleton||gm.any_remapped||
                (PortApi::GetVarNs(gm.wcl_ns.c_str(),L"shapekey_vertex_count",0.f)>0.f);
            const auto load_gen=PortApi::LoadGeneration();
            const auto mod_key=std::uint64_t(mod)+1;
            // For shapekey==0 merged-skeleton non-remapped mods, let Frame 1
            // publish its evaluated author selection first; from Frame 2 onward
            // (author revision != 0), hybrid_probe keeps private-RT-capable mods
            // (rest_skinning_known == true) on Private RT while routing mods
            // with disjoint VG windows or build_pack rejection to Hybrid Raster.
            if((force_hybrid||WWMI_RtModReadAuthorRevision_v1(load_gen,mod_key)!=0)&&
               r->hybrid_probe(&q,author.draw_ticket,load_gen,mod_key,world_valid&&original_world.reads_previous?&previous:nullptr))
                hybrid_mod=mod;
        };
        for_each_head_slot(candidate);
    }
    if(matched!=1)hybrid_mod=-1;
    struct BegunScope {WWMI_RtAuthorDeformedScope_v2 value{};bool opened=false;uint64_t load=0;};
    const auto begin_scope=[&](int mod){
        BegunScope scope;
        if(author.token&&instance.resource){
            WWMI_RtAuthorScopeRequest_v1 request{q,instance,author.token,PortApi::LoadGeneration(),uint64_t(mod)+1,author.draw_ticket};
            if(original_world.reads_previous){
                request.previous_scope=original_world.previous_scope;
                request.previous_resource=original_world.prev_resource;
                request.previous_offset=original_world.prev_offset;
                request.previous_bytes=original_world.prev_bytes;
            }
            scope.load=request.load_generation;
            scope.opened=r->author_scope(&request,sizeof(request),&scope.value,sizeof(scope.value))!=0;
        }
        return scope;
    };
    int format_mod=-1;
    BegunScope format_scope;
    const auto observe_formats=[&](int idx){
        if(format_mod>=0||!section_matches(idx))return;
        const auto mod=g_merged_comps[idx].mod_id;
        if(!g_geomods[mod].mod.queries_ps_slots)return;
        if(mod==hybrid_mod)return;
        format_mod=mod;format_scope=begin_scope(mod);
        if(!format_scope.value.base.plan)return;
        auto& words=std::get<7>(owner);std::copy_n(instance.local_to_world,words.size(),words.begin());
        std::get<4>(owner)=format_scope.value.base.plan;std::get<5>(owner)=instance.resource;std::get<6>(owner)=instance.byte_origin;
        native_formats=RtObserveAuthorPsFormats(list,owner,vs,cloth.row?&*cloth.row:nullptr);
        format_observed=true;
    };
    if(author.token&&instance.resource){
        for_each_head_slot(observe_formats);
    }
    const auto scope_for=[&](int mod){return mod==hybrid_mod?BegunScope{}:(mod==format_mod?format_scope:begin_scope(mod));};
    const auto retain_native=[&](WWMI_RtAuthorScope_v1 scope,RtAuthorChoiceSite site,unsigned why){
        if(!scope.plan)return true;
        WWMI_RtAuthorCommit_v1 decision{q.device,q.list,author.source.list_creation,author.source.recording,scope.plan,0,0,author.draw_ticket,
            WWMI_RT_AUTHOR_NATIVE,0,target.component,0};
        return RtCommitAuthor(*r,decision,site,why,vs)!=0;
    };
    // In the private-position path, the legacy VM restore touches slots 0..4.
    // Relocation is disabled there and the private draw changes just VB0 and IB.
    const unsigned restore_mask=stream ? 0x1fu : 0xffu;
    const auto early_native=[&](unsigned why){
        RtPassAdmissionNote admission{};
        if(why==5)admission={admission_stamp,admission_first,admission_world,position_evaluated,admission_effects,
            aux_native_pso,g.gfx_restore(),CmdGfxLayout(list),g_frame_id,CmdGen(list),author.draw_ticket,
            rt.valid,bool(rt.has_dsv&&rt.dsv)};
        bool coherent=true;
        bool related=false;
        const auto reason=why==5?"pass-not-admitted":why==6?"no-color-or-proved-depth":
            why==7?"missing-restore-state":"palette-input-unavailable";
        const auto keep=[&](int idx){
            if(!section_matches(idx))return;
            related=true;
            const auto mod=g_merged_comps[idx].mod_id;
            const auto scope=scope_for(mod).value.base;
            if(cloth.row&&scope.disposition==WWMI_RT_AUTHOR_PRIVATE)cloth.row->author_disposition=scope.disposition;
            const bool retained=retain_native(scope,RtAuthorChoiceSite::early_native,why);
            coherent=retained&&coherent;
            if(auto* mc_cnt=RtGateCountFor(mod)){
                if(why>=5&&why<=8)mc_cnt->early[why-5].fetch_add(1,std::memory_order_relaxed);
                if(!retained)mc_cnt->plan_viol.fetch_add(1,std::memory_order_relaxed);
                else mc_cnt->native_pass.fetch_add(1,std::memory_order_relaxed);
            }
            aux_sections.remember(original_aux,original_aux_ps,scope.plan,mod,
                retained?"section-native":"plan-violation",original_aux_refusal?original_aux_refusal:reason,!retained);
        };
        for_each_head_slot(keep);
        cloth.fail(why);
        const auto result=!coherent?RtDrawExecution::PlanViolation:
            (related?RtDrawExecution::NativeSelected:RtDrawExecution::Unrelated);
        const auto note=[&](int idx){
            if(!section_matches(idx))return;
            const auto route=coherent?RtNoteRoute::native_pass_through:RtNoteRoute::plan_violation;
            if(why==5)RtMaterialDrawAdmissionNote(list,g_merged_comps[idx].mod_id,target.component,rt.rtv_count,g.gfx_pso,
                route,reason,first,count,base,admission);
            else RtMaterialDrawNote(list,g_merged_comps[idx].mod_id,target.component,rt.rtv_count,g.gfx_pso,
                route,reason,first,count,base);
        };
        for_each_head_slot(note);
        const RtDrawOutcome outcome{result,!coherent};
        finish_aux(outcome);
        if(cloth.row){cloth.row->native_choice_rejected=!coherent;cloth.row->execution=result;}
        return outcome;
    };
    if(!stream&&!gpu_palette){field.key.path=3;return early_native(5);}
    if(!rt.rtv_count&&!(stream&&(depth_only||(masked_material&&hybrid_mod>=0)))){field.key.path=4;return early_native(6);}
    if(depth_only)field.key.path=kRtDepthNativePath;
    if((ia.known&restore_mask)!=restore_mask){if(!depth_only)field.key.path=5;return early_native(7);}
    BoneRoots bones{list};
    if(gpu_palette){
        uint32_t b4size=0,b3size=0;
        if(!PipelineVsCbufferBytes(g.gfx_pso,4,b4size)||b4size!=kBonesCbufferBytes||
           !PipelineVsCbufferBytes(g.gfx_pso,3,b3size)||b3size!=kBonesCbufferBytes){
            auto result=early_native(8);cloth.fail(8,1);return result;
        }
        bones.Get();
        if(!bones.b4cb.res||!bones.b3cb.res||!bones.b4cb.va||!bones.b3cb.va||
           bones.b4cb.size<b4size||bones.b3cb.size<b3size){
            auto result=early_native(8);cloth.fail(8,2);return result;
        }
    }
    NativeBindingRestore native_scope(r->capture_restore,list,cl,g.gfx_restore()!=0);
    bool related=false,recorded=false,hidden=false,violation=false;
    auto section=[&](int idx){
        if(!section_matches(idx))return;
        related=true;
        auto& mc=g_merged_comps[idx];
        auto& gm=g_geomods[mc.mod_id];auto* wc=WmodComp(mc.mod_id,mc.comp_idx);
        auto* cloth_section=cloth.section(mc.mod_id);RtClothSectionVariables(cloth_section,mc.mod_id,false);
        const auto bound_scope=scope_for(mc.mod_id);
        const auto &scope=bound_scope.value.base;
        RtAuxProofNote section_aux=original_aux;
        uint64_t section_aux_ps=original_aux_ps;
        const char* aux_refusal=original_aux_refusal;
        const char* aux_issued=nullptr;
        bool aux_violation=false;
        const auto remember_aux=[&]{
            const auto route=aux_issued?aux_issued:aux_violation?"plan-violation":"section-native";
            const auto reason=aux_issued?(aux_violation&&aux_refusal?aux_refusal:
                aux_issued==std::string_view("hidden")?"vm-selected-none":"section-issued"):
                aux_refusal?aux_refusal:"section-not-issued";
            aux_sections.remember(section_aux,section_aux_ps,scope.plan,mc.mod_id,route,reason,aux_violation);
        };
        struct AuxOnExit {const decltype(remember_aux)& finish;~AuxOnExit(){finish();}} aux_on_exit{remember_aux};
        if(cloth_section){cloth_section->author_plan=scope.plan;cloth_section->author_disposition=scope.disposition;}
        const auto refuse=[&](unsigned why){
            const bool retained=retain_native(scope,RtAuthorChoiceSite::section_refusal,why);
            violation|=!retained;
            aux_violation|=!retained;
            if(auto* mc_cnt=RtGateCountFor(mc.mod_id)){
                const unsigned ri=why==5?0u:why==10?1u:why==11?2u:why==12?3u:4u;
                mc_cnt->refuse[ri].fetch_add(1,std::memory_order_relaxed);
                if(!retained)mc_cnt->plan_viol.fetch_add(1,std::memory_order_relaxed);
                else mc_cnt->native_pass.fetch_add(1,std::memory_order_relaxed);
            }
            const auto reason=why==5?"masked-pass-refused":why==10?"mod-disabled":why==11?"scope-or-world-not-ready":
                why==12?"world-inputs-refused":"old-private-refused";
            RtMaterialDrawNote(list,mc.mod_id,target.component,rt.rtv_count,g.gfx_pso,
                !retained?RtNoteRoute::plan_violation:mc.mod_id==hybrid_mod?RtNoteRoute::refused_hybrid:RtNoteRoute::refused_old,
                reason,first,count,base);
            if(!aux_refusal)aux_refusal=reason;
            if(cloth_section){
                cloth_section->reason=why;cloth_section->native_choice_rejected=!retained;
                cloth_section->author_disposition=retained&&scope.plan?WWMI_RT_AUTHOR_NATIVE:scope.disposition;
            }
        };
        const bool hybrid=stream&&mc.mod_id==hybrid_mod;
        // The sibling flag reaches here only after the strict native
        // PSO/effects guard, and world_valid entails its actual PS/root proof.
        const bool first_masked=!hybrid&&stream&&masked==MaskedMaterial::sibling&&world_valid&&
            bound_scope.load==PortApi::LoadGeneration()&&RtFirstMaskedOwner(bound_scope.opened,
                bound_scope.value,author,instance,q.device,bound_scope.load,uint64_t(mc.mod_id)+1,target.component);
        if(masked_material&&!hybrid&&scope.disposition!=WWMI_RT_AUTHOR_PRIVATE&&!first_masked){refuse(5);return;}
        const bool first_frame_draw = (gm.last_char_frame != g_frame_id);
        gm.last_char_frame=g_frame_id;
        if(first_frame_draw && wc && wc->ib_hash) PortApi::RunSiblingHashMarkers(wc->ib_hash);
        if(!gm.geom_enabled){refuse(10);return;}
        if(program&&!depth_only)RtWorldNote(8,vs,mc.comp_idx,0); // Known material draw reached the live skin path.
        DrawArgs da{list,cl,count,instances,first,base,first_instance,vs,rt};DrawClaim dc{};
        dc.mc=&mc;dc.mc_cb13=gpu_palette;dc.cb13_root=bones.b4;dc.cb14_root=bones.b3;
        dc.port_cb4_res=bones.b4cb.res;dc.cb13_va=dc.port_cb4_va=bones.b4cb.va;
        dc.port_cb3_res=bones.b3cb.res;dc.cb14_va=dc.port_cb3_va=bones.b3cb.va;
        StreamContext ctx{q,*r,mc.mod_id,id};ctx.cloth=cloth_section;
        ctx.hybrid=hybrid;ctx.first_masked=first_masked;ctx.hybrid_instance=instance;
        ctx.native_list=reinterpret_cast<uint64_t>(cl);
        ctx.format_owner=owner;ctx.author=author;ctx.author_scope=scope;ctx.deformed_owner=bound_scope.value.owner;
        if(hybrid&&instance.resource&&gm.mod.queries_ps_slots){
            auto &format_owner=ctx.format_owner;
            std::get<4>(format_owner)=std::uint64_t(mc.mod_id)+1;
            std::get<5>(format_owner)=instance.resource;std::get<6>(format_owner)=instance.byte_origin;
            auto &words=std::get<7>(format_owner);std::copy_n(instance.local_to_world,words.size(),words.begin());
            ctx.native_formats=RtObserveAuthorPsFormats(list,format_owner,vs,cloth.row?&*cloth.row:nullptr);
            ctx.format_observed=true;
        }else if(mc.mod_id==format_mod){
            ctx.native_formats=native_formats;ctx.format_observed=format_observed;
        }else if(scope.plan&&gm.mod.queries_ps_slots){
            auto& format_owner=ctx.format_owner;
            std::get<4>(format_owner)=scope.plan;std::get<5>(format_owner)=instance.resource;std::get<6>(format_owner)=instance.byte_origin;
            auto& words=std::get<7>(format_owner);std::copy_n(instance.local_to_world,words.size(),words.begin());
            ctx.native_formats=RtObserveAuthorPsFormats(list,format_owner,vs,cloth.row?&*cloth.row:nullptr);
            ctx.format_observed=true;
        }
        ctx.program=program;ctx.depth_only=depth_only;
        bool have_world=true;
        if(program){
            if(world_observed){ctx.original_world=&original_world;have_world=world_valid;}
            else{
                // Legacy identify reaches the same selected PSO generation
                // without an author snapshot. Carry its immutable contract,
                // while an old static selection remains contractless.
                ctx.world.contracts=original_world.contracts;
                {
                    rt_phase_cpu::ConditionalScope world_cpu(gate_active,rt_phase_cpu::gate_world);
                    RtAuxNoteScope note(section_aux);
                    RtAuxVertexEndGuard aux_end(target.native_vertex_first+target.native_vertex_count);
                    have_world=RtReadWorldSurface(list,*program,target.object_guid,target.content_hash,mc.comp_idx,ctx.world,r->frame2!=nullptr,r->reserve2!=nullptr);
                }
                if(section_aux.input!=RtAuxInput::none)list->get_private_data(s_iid_ps_hash,&section_aux_ps);
                if(!have_world)aux_refusal="world-read-refused";
                if(have_world&&(unused_previous||program->previous<0))ctx.world.reads_previous=false;
            }
        }
        if(!have_world){
            refuse(11);
            if(!depth_only){RtWorldNote(1,vs,mc.comp_idx,0);}return;
        }
        if(stream&&ctx.author.token&&!(hybrid?instance.resource!=0:scope.plan!=0)){refuse(11);return;}
        if(cloth_section){const auto &world=ctx.surface();cloth_section->reads_previous=world.reads_previous;cloth_section->previous_bound=world.prev_resource!=0;cloth_section->previous_resolution=world.previous_resolution;}
        {
            rt_phase_cpu::ConditionalScope world_cpu(gate_active,rt_phase_cpu::gate_world);
            if(stream && !PrepareWorldSkinInputs(ctx,list,mc.comp_idx)){refuse(12);if(program&&!depth_only){RtWorldNote(2,vs,mc.comp_idx,0);}return;}
        }
        InjectFrame fr{da,dc};fr.save_ib=ia.ib;fr.save_ib_set=true;
        for(unsigned i=0;i<8;++i){fr.save_vb[i]=ia.vb[i];fr.save_vb_set[i]=(ia.known&(~ia.nulls)&(1u<<i))!=0;}
        struct Restore {InjectFrame& f;const Ia& original;ID3D12GraphicsCommandList* cl;
            ~Restore(){RestoreGameState(f);
                for(unsigned slot=0;slot<8;){
                    if(!(original.known&(1u<<slot))){++slot;continue;}
                    const unsigned start=slot;
                    while(slot<8 && (original.known&(1u<<slot)))++slot;
                    wwmi::binding_client::IASetVertexBuffers(cl,start,slot-start,&original.vb[start],5);
                }
                wwmi::binding_client::IASetIndexBuffer(cl,&original.ib,8);}} restore{fr,ia,cl};
        struct Scope {StreamContext *old;~Scope(){active_stream=old;}} stream_scope{active_stream};
        active_stream=stream?&ctx:nullptr;
        native_scope.refused=false;
        RtDrawExecution legacy;
        const bool result=[&]{
            struct Trial {NativeBindingRestore& b;~Trial(){b.masked_trial=false;b.hybrid_section=false;}} trial{native_scope};
            native_scope.masked_trial=first_masked;
            native_scope.hybrid_section=hybrid;
            return RtDrawComponent(fr,mc.mod_id,wc,legacy);
        }();
        if(hybrid&&!ctx.replaced){
            if(!aux_refusal)aux_refusal=ctx.rejection?ctx.rejection:"vm-not-dispatched";
            RtMaterialDrawNote(list,mc.mod_id,target.component,rt.rtv_count,g.gfx_pso,
                RtNoteRoute::refused_hybrid,ctx.rejection?ctx.rejection:"vm-not-dispatched",first,count,base,
                ctx.rejection?&ctx.rejected_rfx:&ctx.rfx_latch);
        }
        if(!stream&&legacy==RtDrawExecution::PrivateRecorded)
            RtMaterialDrawNote(list,mc.mod_id,target.component,rt.rtv_count,g.gfx_pso,
                RtNoteRoute::old_palette,"section-issued",first,count,base);
        if((stream&&ctx.replaced&&ctx.empty_selections)||(!stream&&legacy==RtDrawExecution::HiddenSelected))
            RtMaterialDrawNote(list,mc.mod_id,target.component,rt.rtv_count,g.gfx_pso,
                RtNoteRoute::hidden,"vm-selected-none",first,count,base);
        if(stream&&scope.plan&&!ctx.replaced&&!ctx.author_decided&&!ctx.author_legacy)
            refuse(cloth_section&&cloth_section->reason?cloth_section->reason:14);
        violation|=ctx.author_native_refused;
        aux_violation|=ctx.author_native_refused;
        if(ctx.author_native_refused&&!aux_refusal)aux_refusal="author-native-refused";
        if(stream?ctx.replaced&&ctx.recorded_subdraws!=0:legacy==RtDrawExecution::PrivateRecorded)
            aux_issued=hybrid?"hybrid-recorded":"private-recorded";
        else if(stream?ctx.replaced&&ctx.empty_selections!=0:legacy==RtDrawExecution::HiddenSelected)
            aux_issued="hidden";
        RtClothSectionVariables(cloth_section,mc.mod_id,true);
        if(cloth_section){
            cloth_section->draw_component_result=result;cloth_section->replaced=stream?ctx.replaced:result;
            cloth_section->requested=unsigned(ctx.requested.size());cloth_section->recorded=ctx.recorded_subdraws;cloth_section->empty=ctx.empty_selections;
            if(!stream)cloth_section->reason=23;
            else if(ctx.replaced)cloth_section->reason=ctx.empty_selections?21:(ctx.history_invalid_draw?24:22);
            else if(!cloth_section->reason)cloth_section->reason=14;
        }
        if(field.active && stream){field.subdraws+=ctx.recorded_subdraws;field.empty+=ctx.empty_selections;field.author_t0+=ctx.author_t0_draws;}
        if(program&&!depth_only){if(!ctx.replaced)RtWorldNote(7,vs,mc.comp_idx,result?1:0);}
        if(auto* mc_cnt=RtGateCountFor(mc.mod_id)){
            const bool is_rec=stream?(ctx.replaced&&ctx.recorded_subdraws!=0):(legacy==RtDrawExecution::PrivateRecorded);
            const bool is_hid=stream?(ctx.replaced&&ctx.empty_selections!=0):(legacy==RtDrawExecution::HiddenSelected);
            if(is_rec)(hybrid?mc_cnt->ok_hyb:mc_cnt->ok_priv).fetch_add(1,std::memory_order_relaxed);
            else if(is_hid)mc_cnt->ok_hid.fetch_add(1,std::memory_order_relaxed);
            else if(ctx.author_native_refused)mc_cnt->plan_viol.fetch_add(1,std::memory_order_relaxed);
            else mc_cnt->native_pass.fetch_add(1,std::memory_order_relaxed);
        }
        if(stream){
            recorded|=ctx.replaced&&ctx.recorded_subdraws!=0;
            hidden|=ctx.replaced&&ctx.empty_selections!=0;
        }else{
            recorded|=legacy==RtDrawExecution::PrivateRecorded;
            hidden|=legacy==RtDrawExecution::HiddenSelected;
        }
    };
    for_each_head_slot(section);
    const RtDrawOutcome outcome{recorded?RtDrawExecution::PrivateRecorded:hidden?RtDrawExecution::HiddenSelected:
        violation?RtDrawExecution::PlanViolation:related?RtDrawExecution::NativeSelected:RtDrawExecution::Unrelated,violation};
    const bool did=outcome.cancels_native();
    finish_aux(outcome);
    if(!did&&related){
        extern void RecordNativeReadAsBound(reshade::api::command_list*);
        RecordNativeReadAsBound(list);
        const auto note=[&](int idx){if(section_matches(idx))
            RtMaterialDrawNote(list,g_merged_comps[idx].mod_id,target.component,rt.rtv_count,g.gfx_pso,
                RtNoteRoute::native_pass_through,"caller-resumes-native",first,count,base);
        };
        for_each_head_slot(note);
    }
    if(cloth.row){
        cloth.row->did=did;cloth.row->execution=outcome.execution;cloth.row->native_choice_rejected=violation;
        if(!cloth.row->sections.empty())cloth.row->reason=0;
    }
    field.key.path=depth_only?(did?kRtDepthPrivatePath:kRtDepthNativePath):(did?7:6);return outcome;
}

bool RtTryGpuSkinDraw(reshade::api::command_list *list,uint32_t count,uint32_t instances,uint32_t first,int32_t base,uint32_t first_instance){
    return RtSelectGpuSkinDraw(list,count,instances,first,base,first_instance).cancels_native();
}

void RtStopStructTexFallbackWorker(){
    wwmi_struct_tex::g_struct_fallback.Stop();
}
