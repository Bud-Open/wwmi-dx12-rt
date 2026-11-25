// Report only execution decided by the existing draw caller. The startup
// burst must never spend the budget for ready material.
#include "rt_material_draw_note.hpp"
#include "rt_program_admission.hpp"
#include "rt_raster_effects.h"
#include <atomic>
#include <cstdio>
#include <optional>

namespace {
struct RtPassAdmissionNote {
    RtProgramLookupStamp intake;
    const char* first=nullptr;
    const char* world="not-run";
    bool position_evaluated=false;
    std::optional<RtRasterEffects> effects;
    uint64_t pso=0,effective_pso=0,active_layout=0,frame=0,recording=0,ticket=0;
    bool om_known=false,dsv_attached=false;
};
using MaterialNoteKey=std::tuple<uint64_t,uint64_t,unsigned,unsigned,bool,RtNoteRoute,const char*,
    unsigned,unsigned,int,uint64_t,uint64_t,const char*>;
enum MaterialNoteKind : size_t {
    refusal,ready_refusal,passthrough,pending_color,other_color,ready_low,ready_high,depth
};
constexpr std::array<size_t,8> kMaterialNoteLimits{24,24,24,48,24,48,48,8};
constexpr std::array<const char*,8> kMaterialNoteKinds{
    "refusal","ready-refusal","passthrough-or-hidden","pending-color","other-color",
    "ready-low-mrt","ready-gbuffer-sized","depth-recorded"};
constexpr unsigned kMaterialChangesPerKey=2,kMaterialChangesPerBank=24;
struct MaterialWinners {
    std::array<char,5> tags{'?','?','?','?',0};
    std::array<unsigned,4> slots{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    std::array<uint64_t,4> resources{};
    bool operator==(const MaterialWinners&)const=default;
};
struct MaterialSeen {MaterialWinners last;unsigned changes=0;bool capped=false;};
struct MaterialNoteQuota {
    std::map<MaterialNoteKey,MaterialSeen> seen;
    uint64_t dropped=0;
    unsigned changes=0;
};
struct MaterialNotes {
    std::mutex lock;
    unsigned generation=0;
    std::map<int,std::array<MaterialNoteQuota,kMaterialNoteLimits.size()>> mods;
    std::atomic<uint64_t> change_epoch{};
};
MaterialNotes& RtMaterialNotes(){static auto* notes=new MaterialNotes;return *notes;}
const char* RtMaterialRouteName(RtNoteRoute route){
    switch(route){
    case RtNoteRoute::hybrid:return "hybrid-recorded";
    case RtNoteRoute::old_private:return "old-private-recorded";
    case RtNoteRoute::old_palette:return "old-palette-recorded";
    case RtNoteRoute::refused_hybrid:return "hybrid-refused";
    case RtNoteRoute::refused_old:return "old-refused";
    case RtNoteRoute::native_pass_through:return "native-pass-through";
    case RtNoteRoute::plan_violation:return "plan-violation";
    case RtNoteRoute::hidden:return "intentionally-hidden";
    }
    return "unknown";
}
const char* RtMaterialRfxName(RtRfxStep state){
    switch(state){
    case RtRfxStep::no_call:return "no-settextures";
    case RtRfxStep::pending:return "textures-unresolved";
    case RtRfxStep::no_maps:return "no-maps";
    case RtRfxStep::disabled:return "disabled";
    case RtRfxStep::no_color:return "not-a-material-draw";
    case RtRfxStep::low_mrt:return "low-mrt";
    case RtRfxStep::ps_reads:return "ps-read-mask";
    case RtRfxStep::window:return "no-table-window";
    case RtRfxStep::missing_native:return "native-input-missing";
    case RtRfxStep::unknown_description:return "native-description-unknown";
    case RtRfxStep::stale_native:return "native-source-retired";
    case RtRfxStep::wrong_native_view:return "native-source-not-srv";
    case RtRfxStep::not_native_2d:return "native-source-not-texture2d";
    case RtRfxStep::known_empty:return "classification-known-empty";
    case RtRfxStep::no_device:return "binding-device-missing";
    case RtRfxStep::plan_ready:return "plan-ready";
    case RtRfxStep::offset:return "target-outside-window";
    case RtRfxStep::owner:return "descriptor-owner-missing";
    case RtRfxStep::old_private:return "old-private-no-rfx-plan";
    }
    return "unknown";
}
void RtMaterialFlushLocked(MaterialNotes& notes){
    for(const auto& [mod,buckets]:notes.mods)
        for(size_t i=0;i<buckets.size();++i)
            if(buckets[i].dropped)
                logger::probe("WWMI-DRAW event=limit-end generation=%u mod=%d category=%s dropped=%llu",
                    notes.generation,mod+1,kMaterialNoteKinds[i],(unsigned long long)buckets[i].dropped);
    notes.mods.clear();
    notes.change_epoch.fetch_add(1,std::memory_order_release);
}
enum class MaterialReceipt {drop,first,change};
MaterialReceipt RtTakeMaterialNote(unsigned load,int mod,const MaterialNoteKey& key,MaterialNoteKind kind,const MaterialWinners& bindings){
    const bool track=kind==ready_low||kind==ready_high;
    // Most repetitions never take a global lock. Cache exact keys, not an
    // identity hash that could accidentally merge different Mods or passes.
    struct Recent {
        unsigned load=0;int mod=-1;MaterialNoteKey key{};MaterialNoteKind kind{};
        MaterialWinners bindings{};uint64_t epoch=0;bool capped=false;
    };
    thread_local std::array<Recent,64> recent{};
    const auto index=(std::get<0>(key)+std::get<1>(key)*7u+std::get<2>(key)*13u+unsigned(mod)*23u+
        unsigned(kind)*5u+std::get<7>(key)*11u+(std::get<10>(key)>>4)+std::get<11>(key)*17u)%recent.size();
    auto& slot=recent[index];
    auto& notes=RtMaterialNotes();
    if(slot.load==load&&slot.mod==mod&&slot.key==key&&slot.kind==kind&&
       (!track||slot.capped||(slot.bindings==bindings&&slot.epoch==notes.change_epoch.load(std::memory_order_acquire))))
        return MaterialReceipt::drop;
    std::lock_guard lock(notes.lock);
    if(load<notes.generation)return MaterialReceipt::drop;
    if(load!=notes.generation){RtMaterialFlushLocked(notes);notes.generation=load;}
    auto& quota=notes.mods[mod][kind];
    slot={load,mod,key,kind,bindings,notes.change_epoch.load(std::memory_order_relaxed),false};
    const auto limit=kMaterialNoteLimits[kind];
    if(auto found=quota.seen.find(key);found!=quota.seen.end()){
        auto& old=found->second;
        if(old.capped){slot.capped=true;return MaterialReceipt::drop;}
        if(!track||old.last==bindings)return MaterialReceipt::drop;
        if(old.changes<kMaterialChangesPerKey&&quota.changes<kMaterialChangesPerBank){
            old.last=bindings;
            slot.epoch=notes.change_epoch.fetch_add(1,std::memory_order_release)+1;
            ++old.changes;++quota.changes;return MaterialReceipt::change;
        }
        old.capped=slot.capped=true;
    }else if(quota.seen.size()<limit){
        quota.seen.emplace(key,MaterialSeen{bindings});
        return MaterialReceipt::first;
    }else slot.capped=true;
    const uint64_t dropped=++quota.dropped;
    if((dropped&(dropped-1))==0)
        logger::probe("WWMI-DRAW event=limit-count generation=%u mod=%d category=%s limit=%zu change_limit=%u dropped=%llu",
            load,mod+1,kMaterialNoteKinds[kind],limit,track?kMaterialChangesPerBank:0,(unsigned long long)dropped);
    return MaterialReceipt::drop;
}

struct MaterialReadPlan {
    uint64_t declared=0;
    bool known=false;
    unsigned unread=0;
    std::array<RegBind,4> destinations;
};

MaterialReadPlan RtMaterialReadPlan(reshade::api::command_list* list,uint64_t pso,bool use_tables){
    const auto layout=use_tables?CmdGfxLayout(list):0;
    const auto recording=CmdGen(list);
    // Reuse known immutable shader/root metadata within the same recording.
    // PSO pointers are only this cache's input, not an evidence key.
    struct Memo {
        const void* list=nullptr;
        uint64_t recording=0,pso=0,layout=0;
        MaterialReadPlan plan{};
    };
    thread_local std::array<Memo,16> memo{};
    auto& slot=memo[((pso>>4)^layout)%memo.size()];
    if(recording&&slot.list==list&&slot.recording==recording&&slot.pso==pso&&slot.layout==layout)
        return slot.plan;
    MaterialReadPlan plan{};
    const auto declared=PipelineSrvRegs(pso,'p');
    plan.declared=declared.lo;plan.known=declared.parsed&&!declared.unbounded;
    if(plan.known)for(unsigned reg=0;reg<4;++reg)if(!declared.reads(reg))plan.unread|=1u<<reg;
    bool valid_layout=false;
    if(layout){
        std::lock_guard l(g_layout_regmap_mtx);
        const auto it=g_layout_regmap.find(layout);
        valid_layout=it!=g_layout_regmap.end()&&it->second.built;
        if(valid_layout)for(unsigned reg=0;reg<4;++reg)plan.destinations[reg]=it->second.t_ps[reg];
    }
    if(recording&&plan.known&&(!use_tables||valid_layout))slot={list,recording,pso,layout,plan};
    return plan;
}

MaterialWinners RtMaterialWinners(const MaterialReadPlan& plan,const RtRfxDrawFacts* rfx,
    std::span<const ShadowSlotOvr> overrides,const RtMaterialTables* tables){
    MaterialWinners result;
    if(!tables)return result;
    uint64_t applied=0;
    for(const auto& [root,handle]:tables->tables)if(root<64&&handle.ptr)applied|=1ull<<root;
    for(unsigned reg=0;reg<4;++reg){
        if(plan.unread&(1u<<reg)){result.tags[reg]='-';continue;}
        if(rfx&&rfx->copied){
            const auto& final=rfx->final[reg];
            if(final.view&&final.root_table&&final.param<64){
                result.tags[reg]=final.role;
                result.slots[reg]=(final.param<<24)|final.offset;
                result.resources[reg]=final.resource;
            }
            continue;
        }
        const auto& dest=plan.destinations[reg];
        if(dest.param>=64||!dest.is_table||!(applied&(1ull<<dest.param)))continue;
        result.tags[reg]='G';
        result.slots[reg]=(unsigned(dest.param)<<24)|dest.offset;
        for(size_t n=overrides.size();n>0;--n){
            const auto& s=overrides[n-1];
            if(s.param!=dest.param||s.offset!=dest.offset)continue;
            if(!rfx||n-1>=rfx->bound_count)result.tags[reg]='E';
            else{
                const auto role=rfx->bound_role[n-1];
                result.tags[reg]=RtRfxRole(role);
            }
            break;
        }
    }
    return result;
}
}
void RtMaterialDrawFlush(){
    auto& notes=RtMaterialNotes();
    std::lock_guard lock(notes.lock);
    RtMaterialFlushLocked(notes);
}
static void RtFormatPassAdmission(char* buffer,size_t size,const RtPassAdmissionNote& note){
    const auto intake=RtReadProgramIntakeDetail(note.pso,note.intake);
    const char* const stores[]{"not-seen","unpaired","sdk-blend-unreported","sdk-blend-unusable","effects-only","clone","retired"};
    const char* const lookups[]{"no-clone","no-contract","vs-not-resolved","vs-mismatch","capability-unavailable","usable"};
    const char* const phases[]{"not-run","sdk-metadata","vertex","pixel","pair","bindings","ready"};
    const char* const issues[]{"none","sdk-layout-unreported","invalid-description","shader-description","other-stage",
        "missing-stage","sdk-targets-unreported","malformed-state","root-map","cb-register","cb-slot","cb-alias",
        "stream-register","stream-slot","stream-alias"};
    const auto effects=note.effects.value_or(RtRasterEffects{});
    using U=unsigned long long;
    std::snprintf(buffer,size,
        " admit_first=%s admit_world=%s admit_position_only=%s admit_pso_gen=%llu admit_store=%s"
        " admit_lookup=%s admit_lookup_gen=%llu admit_lookup_return=%u"
        " admit_build=%s admit_issue=%s admit_reason=\"%s\" admit_sdk=%x admit_role=%u admit_reg=%u"
        " admit_sdk_layout=%llx admit_built_vs=%016llx admit_built_ps=%016llx"
        " admit_effective_pso=%llx admit_active_layout=%llx admit_frame=%llu admit_recording=%llu admit_ticket=%llu"
        " admit_om=%u:%u admit_effects=%s admit_color=%u:%u:%u:%x admit_slots=%x:%x:%x"
        " admit_ds_uav_so=%u:%u:%u:%u admit_pixel_outputs=%x",
        note.first?note.first:"unattributed",note.world,note.position_evaluated?"failed":"not-run",
        U(note.intake.creation),stores[unsigned(intake.store)],lookups[unsigned(note.intake.status)],
        U(note.intake.lookup_creation),unsigned(note.intake.returned),
        phases[unsigned(intake.build.phase)],issues[unsigned(intake.build.issue)],
        intake.reason[0]?intake.reason.data():"-",unsigned(intake.build.sdk_presence),unsigned(intake.build.role),unsigned(intake.build.reg),
        U(intake.layout),U(intake.vs),U(intake.ps),U(note.effective_pso),U(note.active_layout),U(note.frame),U(note.recording),U(note.ticket),
        unsigned(note.om_known),unsigned(note.dsv_attached),note.effects?"observed":"not-run",
        unsigned(effects.color),unsigned(effects.basis),unsigned(effects.standard_pixel_shape),unsigned(effects.color_mask),
        unsigned(effects.declared_slots),unsigned(effects.attached_slots),unsigned(effects.write_slots),unsigned(effects.depth_write),
        unsigned(effects.stencil_write),unsigned(effects.uav),unsigned(effects.stream_output),
        unsigned(effects.pixel_depth)|(unsigned(effects.pixel_stencil_ref)<<1)|(unsigned(effects.pixel_coverage)<<2));
}
static void RtMaterialDrawNoteImpl(reshade::api::command_list* list,int mod,unsigned component,unsigned rtv,
    uint64_t effective_pso,RtNoteRoute route,const char* reason,unsigned first,unsigned count,int base,
    const RtRfxDrawFacts* rfx,std::span<const ShadowSlotOvr> overrides,const RtMaterialTables* tables,unsigned uv_pairs,
    const RtPassAdmissionNote* admission)
{
    if(mod<0||mod>=int(g_geomods.size())||!list)return;
    const auto load=PortApi::LoadGeneration();
    const bool recorded=route==RtNoteRoute::hybrid||route==RtNoteRoute::old_private||route==RtNoteRoute::old_palette;
    const bool named=rfx&&rfx->called&&rfx->requested;
    const bool waiting=named&&(rfx->requested&~rfx->resolved);
    const bool ready=named&&!waiting;
    const bool is_refusal=route==RtNoteRoute::refused_hybrid||route==RtNoteRoute::refused_old||route==RtNoteRoute::plan_violation;
    const auto category=!recorded?(is_refusal?(ready?ready_refusal:refusal):passthrough):
        !rtv?depth:waiting?pending_color:!ready?other_color:rtv>=4?ready_high:ready_low;
    const bool track=category==ready_high||category==ready_low;
    uint64_t vs=0,ps=0,issued_ps=0,unused_vs=0,unused_gs=0;
    list->get_private_data(s_iid_vs_hash,&vs);
    list->get_private_data(s_iid_ps_hash,&ps);
    const bool dsv=CmdRt(list).has_dsv;
    // Keep unchanged keys for all pre-existing non-admission outcomes.
    const MaterialNoteKey key{vs,ps,component,rtv,dsv,route,reason,first,count,base,
        admission?admission->pso:0,admission?admission->intake.creation:0,admission?admission->first:nullptr};
    MaterialReadPlan plan{};
    MaterialWinners bindings{};
    const bool use_layout=tables&&!(rfx&&rfx->copied);
    if(track){
        plan=RtMaterialReadPlan(list,effective_pso,use_layout);
        bindings=RtMaterialWinners(plan,rfx,overrides,tables);
    }
    const auto receipt=RtTakeMaterialNote(load,mod,key,category,bindings);
    if(receipt==MaterialReceipt::drop)return;
    if(!track){
        plan=RtMaterialReadPlan(list,effective_pso,use_layout);
        bindings=RtMaterialWinners(plan,rfx,overrides,tables);
    }
    const uint64_t issued_pso=recorded&&route!=RtNoteRoute::old_palette?effective_pso:0;
    if(issued_pso)PsoBoundHashes(issued_pso,&unused_vs,&issued_ps,&unused_gs);
    const auto& slots=bindings.slots;
    const auto none=RtRfxDrawFacts{};
    const auto& facts=rfx?*rfx:none;
    const auto& a=facts.native;
    std::array<RtRfxDrawFacts::Final,4> final{};
    unsigned adopted[4]{};
    if(recorded&&facts.copied)for(unsigned r=0;r<4;++r){
        if(bindings.tags[r]=='-'||bindings.tags[r]=='?')continue;
        final[r]=facts.final[r];
        const auto tag=bindings.tags[r];
        const unsigned role=tag=='D'?0:tag=='N'?1:tag=='L'?2:tag=='M'?3:4;
        if(role<4)adopted[role]|=1u<<r;
    }
    using U=unsigned long long;
    char admission_suffix[1024];
    if(admission)RtFormatPassAdmission(admission_suffix,sizeof(admission_suffix),*admission);
    logger::probe("WWMI-DRAW event=%s generation=%u mod=%d guid=%u comp=%u rtv=%u dsv=%d "
        "vs=%016llx native_ps=%016llx native_pso=%llx issued_pso=%llx issued_ps=%016llx ps_declared=%llx ps_known=%d "
        "first=%u count=%u base=%d uv_pairs=%u table_bound=%d winners_t0_t3=%s slots=%08x,%08x,%08x,%08x "
        "reason=%s rfx=%s material_phase=%s transition=%d call=%u named=%x loaded=%x reads=%x "
        "fresh_masks=%x,%x,%x,%x planned_masks=%x,%x,%x,%x adopted_masks=%x,%x,%x,%x "
        "missing=%u native_mapped=%x native_present=%x native_valid=%x native_source_states=%u,%u,%u "
        "material_failure=%u material_param=%u material_offset=%u copied=%d "
        "native_fmt_dims_mips_class=%u:%ux%u:%u:%u,%u:%ux%u:%u:%u,%u:%ux%u:%u:%u "
        "final_res_view_root_off=%llx:%llx:%llx:%u,%llx:%llx:%llx:%u,%llx:%llx:%llx:%u,%llx:%llx:%llx:%u%s",
        RtMaterialRouteName(route),load,mod+1,g_geomods[mod].mod.object_guid,component,rtv,int(dsv),
        (unsigned long long)vs,(unsigned long long)ps,(unsigned long long)CmdGfx(list).gfx_pso,
        (unsigned long long)issued_pso,(unsigned long long)issued_ps,
        (unsigned long long)plan.declared,int(plan.known),
        first,count,base,uv_pairs,int(tables!=nullptr),bindings.tags.data(),slots[0],slots[1],slots[2],slots[3],
        reason,rfx?RtMaterialRfxName(facts.step):"not-inspected",
        ready?"ready":waiting?"pending":"other",int(receipt==MaterialReceipt::change),
        unsigned(facts.called),unsigned(facts.requested),unsigned(facts.resolved),
        unsigned(facts.reads),unsigned(facts.fresh[0]),unsigned(facts.fresh[1]),unsigned(facts.fresh[2]),unsigned(facts.fresh[3]),
        unsigned(facts.masks[0]),unsigned(facts.masks[1]),unsigned(facts.masks[2]),unsigned(facts.masks[3]),
        adopted[0],adopted[1],adopted[2],adopted[3],
        unsigned(facts.missing_slot),unsigned(facts.native_mapped),unsigned(facts.native_present),unsigned(facts.native_valid),
        unsigned(facts.native_sources[0]),unsigned(facts.native_sources[1]),unsigned(facts.native_sources[2]),
        facts.material_failure,facts.material_param,facts.material_offset,int(recorded&&facts.copied),
        a[0].format,a[0].width,a[0].height,a[0].mips,a[0].classification,
        a[1].format,a[1].width,a[1].height,a[1].mips,a[1].classification,
        a[2].format,a[2].width,a[2].height,a[2].mips,a[2].classification,
        U(final[0].resource),U(final[0].view),U(final[0].root_table),final[0].offset,
        U(final[1].resource),U(final[1].view),U(final[1].root_table),final[1].offset,
        U(final[2].resource),U(final[2].view),U(final[2].root_table),final[2].offset,
        U(final[3].resource),U(final[3].view),U(final[3].root_table),final[3].offset,admission?admission_suffix:"");
}
void RtMaterialDrawNote(reshade::api::command_list* list,int mod,unsigned component,unsigned rtv,
    uint64_t effective_pso,RtNoteRoute route,const char* reason,unsigned first,unsigned count,int base,
    const RtRfxDrawFacts* rfx,std::span<const ShadowSlotOvr> overrides,const RtMaterialTables* tables,unsigned uv_pairs){
    RtMaterialDrawNoteImpl(list,mod,component,rtv,effective_pso,route,reason,first,count,base,rfx,overrides,tables,uv_pairs,nullptr);
}
static void RtMaterialDrawAdmissionNote(reshade::api::command_list* list,int mod,unsigned component,unsigned rtv,
    uint64_t effective_pso,RtNoteRoute route,const char* reason,unsigned first,unsigned count,int base,
    const RtPassAdmissionNote& admission){
    RtMaterialDrawNoteImpl(list,mod,component,rtv,effective_pso,route,reason,first,count,base,nullptr,{},nullptr,0,&admission);
}
