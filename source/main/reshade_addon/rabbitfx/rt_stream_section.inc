// Each stage is reported once per real component, not once per frame.
static void RtWorldNote(unsigned stage,uint64_t vs,int component,unsigned status){
    if(component<0 || component>=32 || stage>=10)return;
    // Follow one actually identified torso pass across neighboring frames,
    // rather than inferring a time series from the once-per-stage summary.
    if(component==3 && vs==0xaad17e90aee71ee7ULL && stage>=3 && stage<=6){
        static std::atomic<uint64_t> start{0},last{0};
        static std::atomic<unsigned> lines{0};
        const uint64_t frame=uint64_t(g_frame_id)+1;
        if(stage==6){uint64_t empty=0;start.compare_exchange_strong(empty,frame);}
        const auto begin=start.load();
        if(begin && frame>=begin && frame-begin<24 && lines.load()<48){
            const uint64_t key=(frame<<32)|(uint64_t(stage)<<28)|(status&0x0fffffffu);
            if(last.exchange(key)!=key && lines.fetch_add(1)<48){
                char line[144];std::snprintf(line,sizeof(line),"RT-WORLD-TORSO frame=%llu stage=%u status=%u",(unsigned long long)frame,stage,status);
                reshade::log::message(reshade::log::level::info,line);
            }
        }
    }
    static std::atomic<uint32_t> seen[10];const uint32_t bit=1u<<component;
    if(!(seen[stage].fetch_or(bit,std::memory_order_relaxed)&bit)){
        char line[144];
        std::snprintf(line,sizeof(line),"RT-WORLD stage=%u component=%d vs=%016llx status=%u",stage,component,(unsigned long long)vs,status);
        reshade::log::message(reshade::log::level::info,line);
    }
}
bool RtVisibleMaterialWindow();
struct RtAuthorSelection {
    bool observed=false; unsigned sampled=0,bound=0,carried=0; int formats[9]{};
};
struct RtHybridAsset;
#include "rt_hybrid_rfx_material.hpp"
#include "rt_material_draw_note.hpp"
bool ReadPublishedDescriptor(const HeapKey&, ResInfo&) noexcept;
DXGI_FORMAT PublishedViewFormat(const HeapKey&) noexcept;
void TexQuerySubDiag(uint64_t res, uint32_t* sub1_id, uint32_t* sub2_id, uint32_t* via_mask);
#include "tex_struct_fallback.hpp"
struct StreamDraw {
    unsigned count,first;int base;unsigned flags;
    std::vector<ShadowSlotOvr> ps;
    std::vector<uint64_t> ps_owners;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> source_views;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> textures;
    RtAuthorSelection selection{};size_t trace_part=SIZE_MAX;
    RtAuthorPositionBinding author_position;
    wwmi::hybrid::Part hybrid;
    RtRfxDrawFacts rfx;
    unsigned issued_uv_pairs=0;
};
struct StreamContext {
    const WWMI_RtPackDrawQuery_v2 &query; Reader &reader;int mod;const WWMI_RtPackDrawIdentity_v2 &identity;
    uint64_t plan=0,draw_frame=0; bool replaced=false,depth_only=false,history_invalid_draw=false;RootCbvBind primitive{};WWMI_RtNativePalette_v1 palette{};
    const RtLocalProgram* program=nullptr;RtWorldSurface world{};
    const RtWorldSurface* original_world=nullptr;
    const RtWorldSurface& surface() const {return original_world?*original_world:world;}
    std::vector<StreamDraw> requested;
    RtClothSection* cloth=nullptr;
    unsigned recorded_subdraws=0, empty_selections=0,author_t0_draws=0;
    struct FormatQuery {unsigned format=0,slot_gen=0;};
    FormatQuery formats[16]{};
    uint64_t native_list=0;RtFormatOwner format_owner{};unsigned carried_mask=0;
    WWMI_RtAuthorSnapshot_v1 author{};
    WWMI_RtAuthorScope_v1 author_scope{};
    WWMI_RtDeformedSelection_v1 deformed_owner{};
    RtObservedPsFormats native_formats{};
    bool format_observed=false;
    bool hybrid=false,hybrid_failed=false,first_masked=false;
    RtRfxTextureSources hybrid_rfx;
    RtRfxDrawFacts rfx_latch;
    RtRfxDrawFacts rejected_rfx;
    const char* rejection=nullptr;
    WWMI_RtAuthorInstance_v1 hybrid_instance{};
    std::shared_ptr<const RtHybridAsset> hybrid_asset;
    bool author_decided=false,author_legacy=false,author_native_refused=false;
    bool stamps_cached=false;
    uint64_t cached_stamps_pso=0;
    uint32_t cached_srv_gen0=0;
    uint32_t cached_ps_gen[MigotoPort::kMaxSRV]{};
    SectionPsStamps cached_stamps{};
    bool struct_fallback_cached=false;
    uint64_t cached_struct_pso=0;
    std::vector<wwmi_struct_tex::FallbackResolvedSlot> cached_struct_slots;
};
static thread_local StreamContext *active_stream=nullptr;
bool RtHasPrivateSection(){return active_stream!=nullptr;}
bool RtHasHybridSection(){return active_stream&&active_stream->hybrid;}
void RtRememberHybridRfxTextures(RtRfxTextureSources sources,unsigned named,unsigned resolved){
    active_stream->hybrid_rfx=std::move(sources);
    active_stream->rfx_latch.called=1;
    active_stream->rfx_latch.requested=uint8_t(named);
    active_stream->rfx_latch.resolved=uint8_t(resolved);
}
bool RtHasPrivateDepthSection(){return active_stream && active_stream->depth_only;}
void RtRememberPrivatePsFormat(unsigned slot,unsigned format){
    if(!active_stream || !format || slot>=16)return;
    active_stream->formats[slot]={format,MigotoPort::State().srv_gen[MigotoPort::StageIndex('p')][slot]};
}
extern "C" unsigned WWMI_RtScriptPsFormat(unsigned slot){
    if(!active_stream || slot>=16)return 0;
    const auto& value=active_stream->formats[slot];
    // An explicit script rebind or unbind takes precedence for later queries.
    return value.format && value.slot_gen==MigotoPort::State().srv_gen[MigotoPort::StageIndex('p')][slot]?value.format:0;
}
void RtCarryPrivatePsFormats(uint64_t recording,unsigned reads,const unsigned* current,unsigned count){
    if(!active_stream || !recording)return;
    auto& ctx=*active_stream;
    if(ctx.format_observed){
        if(ctx.cloth)ctx.cloth->format_generation=recording;
        for(unsigned r=0;r<(std::min)(count,16u);++r){
            if(reads&(1u<<r)){
                if(ctx.native_formats.current[r])RtRememberPrivatePsFormat(r,ctx.native_formats.current[r]);
            }else if(const auto &source=ctx.native_formats.carry[r];source.value){
                RtRememberPrivatePsFormat(r,source.value);ctx.carried_mask|=1u<<r;
                if(ctx.cloth&&r<ctx.cloth->carry_sources.size())ctx.cloth->carry_sources[r]=source;
            }
        }
        return;
    }
    std::lock_guard lock(mutex);
    if(views.find(ctx.native_list)==views.end())return;
    auto& list=format_lists[ctx.native_list];
    if(list.generation!=recording){list.formats.clear();list.generation=recording;}
    auto& last=list.formats[ctx.format_owner];
    if(ctx.cloth){
        ctx.cloth->format_generation=recording;
        if(!last.sources)last.sources=std::make_unique<RtPsCarrySources>();
    }
    for(unsigned r=0;r<(std::min)(count,16u);++r){
        const auto bit=1u<<r;
        if(reads&bit){
            last.formats[r]=current[r];
            if(last.sources && r<last.sources->size()){
                auto& source=(*last.sources)[r];
                if(ctx.cloth && current[r])
                    source={current[r],ctx.cloth->trace_seq,ctx.cloth->trace_frame,ctx.cloth->trace_vs,ctx.cloth->trace_ps};
                else source={};
            }
            continue;
        }
        // A physical descriptor in an unread register can belong to another
        // pass (observed t1 switched between BC7 formats). The port's logical
        // register retains this section's last actual read in this recording.
        if(last.formats[r]){
            RtRememberPrivatePsFormat(r,last.formats[r]);ctx.carried_mask|=bit;
            if(ctx.cloth && r<last.sources->size()){
                auto source=(*last.sources)[r];source.value=last.formats[r];
                ctx.cloth->carry_sources[r]=source;
            }
        }
    }
}
namespace PortApi {
bool SectionHasPsFilterRules(const wchar_t* fq_section);
bool SectionTestsPsFilter(const wchar_t* fq_section, float filter_index);
bool ResolvePsFilterBridge(const wchar_t* fq_section,
    unsigned ps_declared_mask, bool ps_declared_known, unsigned rtv_count,
    const unsigned live_vfmt[8], const unsigned live_w[8], const unsigned live_h[8],
    uint64_t* out_dx11_so_hash, float* out_filter_index, bool* out_slots_ready);
}
static void RtBridgePsFilterForActiveSection(){
    if(!active_stream)return;
    const auto &ctx=*active_stream;
    if(ctx.mod<0||ctx.mod>=int(g_geomods.size()))return;
    const int comp=int(ctx.identity.native.component);
    const WwmiMod::Component* wc=WmodComp(ctx.mod,comp);
    if(!wc||wc->tail.empty())return;
    const unsigned load=PortApi::LoadGeneration();

    // Fast cached section FQ name & presence of any `if ps == <filter_index>` rules.
    struct SecInfo { std::wstring fq; bool has_rules=false; };
    std::wstring fq_section;
    {
        static std::mutex s_sec_mtx;
        static unsigned s_sec_gen=~0u;
        static std::map<std::pair<int,int>,SecInfo> s_sec_cache;
        std::lock_guard lk(s_sec_mtx);
        if(s_sec_gen!=load){s_sec_cache.clear();s_sec_gen=load;}
        const auto key=std::make_pair(ctx.mod,comp);
        auto it=s_sec_cache.find(key);
        if(it==s_sec_cache.end()){
            SecInfo si{};
            wchar_t buf[512]{};
            const auto &ns=g_geomods[ctx.mod].wcl_ns;
            if(!ns.empty()&&PortApi::ResolveSection(ns.c_str(),nullptr,wc->tail.c_str(),buf,std::size(buf))&&buf[0]){
                si.fq=buf;
                si.has_rules=PortApi::SectionHasPsFilterRules(si.fq.c_str());
            }
            it=s_sec_cache.emplace(key,std::move(si)).first;
        }
        if(!it->second.has_rules)return;
        fq_section=it->second.fq;
    }

    // Preserve an existing filter_index only if this section actually tests it
    // (avoid blocking on unrelated global framework regex tags such as RabbitFX).
    const float cur_fi=PortApi::BoundShaderFilter('p',nullptr,nullptr,nullptr);
    if(cur_fi!=0.0f&&PortApi::SectionTestsPsFilter(fq_section.c_str(),cur_fi))return;

    auto* list=reinterpret_cast<reshade::api::command_list*>(ctx.query.list);
    if(!list)return;
    const auto &gs=CmdGfx(list);
    const auto rt=CmdRt(list);
    uint64_t ps_hash=0;
    list->get_private_data(s_iid_ps_hash,&ps_hash);
    if(!gs.gfx_pso&&!ps_hash)return;

    struct Decision { uint64_t dx11_so=0; float filter_index=0.f; };
    const auto dkey=std::make_tuple(ctx.mod,comp,gs.gfx_pso,ps_hash,rt.rtv_count);
    static std::mutex s_dec_mtx;
    static unsigned s_dec_gen=~0u;
    static std::map<std::tuple<int,int,uint64_t,uint64_t,uint32_t>,Decision> s_dec_cache;
    {
        std::lock_guard lk(s_dec_mtx);
        if(s_dec_gen!=load){s_dec_cache.clear();s_dec_gen=load;}
        if(auto it=s_dec_cache.find(dkey);it!=s_dec_cache.end()){
            if(it->second.dx11_so){
                PortApi::RegisterShaderHash(it->second.dx11_so);
                PortApi::SetBoundShader('p',it->second.dx11_so);
            }
            return;
        }
    }

    const DxilSrvRegs ps_regs=PipelineSrvRegs(gs.gfx_pso,'p');
    const bool ps_known=ps_regs.parsed&&!ps_regs.unbounded;
    const unsigned ps_decl=ps_regs.lo;
    unsigned live_vfmt[8]{}, live_w[8]{}, live_h[8]{};
    const uint64_t layh=CmdGfxLayout(list);
    RegBind t_ps[8]{};
    bool have_layout=false;
    {
        std::lock_guard l(g_layout_regmap_mtx);
        auto it=g_layout_regmap.find(layh);
        if(it!=g_layout_regmap.end()&&it->second.built){
            std::memcpy(t_ps,it->second.t_ps,sizeof(t_ps));
            have_layout=true;
        }
    }
    if(have_layout){
        auto* dev=list->get_device();
        for(unsigned r=0;r<8;++r){
            if(ps_known&&!ps_regs.reads(r))continue;
            const RegBind &b=t_ps[r];
            if(b.param==0xFF||b.param>=64||!b.is_table)continue;
            const TableStart ts=TableStartOf(dev,gs.root[b.param].table);
            if(!ts)continue;
            const HeapKey key{ts.heap,ts.base+b.offset};
            ResInfo ri{};
            if(!ReadPublishedDescriptor(key,ri)||!ri.res)continue;
            live_vfmt[r]=unsigned(PublishedViewFormat(key));
            TexInfo ti{};
            TexDimsOf(reinterpret_cast<uint64_t>(ri.res),&ti);
            live_w[r]=ti.width;
            live_h[r]=ti.height;
        }
    }

    uint64_t dx11_so=0;
    float filter_index=0.f;
    bool slots_ready=false;
    const bool matched=PortApi::ResolvePsFilterBridge(fq_section.c_str(),
        ps_decl,ps_known,rt.rtv_count,live_vfmt,live_w,live_h,&dx11_so,&filter_index,&slots_ready);
    if(matched&&dx11_so){
        {
            std::lock_guard lk(s_dec_mtx);
            if(s_dec_gen!=load){s_dec_cache.clear();s_dec_gen=load;}
            if(s_dec_cache.emplace(dkey,Decision{dx11_so,filter_index}).second){
                logger::probe("WWMI-PS-FILTER-BRIDGE gen=%u mod=%d guid=%u comp=%d dx12_ps=%016llx rtv=%u ps_decl=%x -> filter_index=%g dx11_so=%016llx",
                    load,ctx.mod+1,g_geomods[ctx.mod].mod.object_guid,comp,
                    (unsigned long long)ps_hash,rt.rtv_count,ps_decl,filter_index,(unsigned long long)dx11_so);
            }
        }
        PortApi::RegisterShaderHash(dx11_so);
        PortApi::SetBoundShader('p',dx11_so);
    }else if(slots_ready){
        std::lock_guard lk(s_dec_mtx);
        if(s_dec_gen!=load){s_dec_cache.clear();s_dec_gen=load;}
        s_dec_cache.emplace(dkey,Decision{0,0.f});
    }
}
void RtBeginPrivateSection(uint64_t section,uint64_t frame){
    RtBridgePsFilterForActiveSection();
    if(active_stream && section){
        active_stream->draw_frame=frame; // one-based frame of this actual draw, independent of the saved plan
        if(active_stream->hybrid)return; // No RT or shared author plan is published by this lane.
        active_stream->plan=active_stream->author_scope.plan?
            WWMI_RtModBeginAuthorSection_v1(section,frame,WWMI_RT_PLAN_MOD_AUTHOR_SHARED):
            WWMI_RtModPlanBegin_v1(section,frame,WWMI_RT_PLAN_MOD_AUTHOR_SHARED);
    }
}
void RtDiscardPrivatePlan(){
    if(active_stream && active_stream->plan)WWMI_RtModPlanEnd_v1(std::exchange(active_stream->plan,uint64_t{}),0);
}
#include "rt_hybrid_main.inc"
static const WwmiMod::Resource* RtCachedPositionResource(int mod){
    static std::mutex cache_mtx;
    static unsigned last_load=0;
    static std::vector<const WwmiMod::Resource*> cache;
    const auto load=PortApi::LoadGeneration();
    std::lock_guard lk(cache_mtx);
    if(load!=last_load){cache.clear();last_load=load;}
    if(mod<0)return nullptr;
    const auto idx=size_t(mod);
    if(idx<cache.size()&&cache[idx])return cache[idx];
    const auto* res=WwmiMod::Find(g_geomods[mod].mod,L"ResourcePositionBuffer");
    if(idx>=cache.size())cache.resize(idx+1,nullptr);
    cache[idx]=res;
    return res;
}
static void RtAuditClassicTexSlots(reshade::api::command_list* list, int dm, int comp, uint32_t rtv,
    const std::vector<ShadowSlotOvr>& overrides){
    if(!list||dm<0||dm>=int(g_geomods.size())||comp<0||rtv<2)return;
    static std::atomic<unsigned> s_gen{0};
    static std::atomic<unsigned> s_emitted{0};
    const unsigned load=PortApi::LoadGeneration();
    if(s_gen.load(std::memory_order_relaxed)==load && s_emitted.load(std::memory_order_relaxed)>=256)return;
    const auto& gs=CmdGfx(list);
    const DxilSrvRegs ps_regs=PipelineSrvRegs(gs.gfx_pso,'p');
    const uint64_t layh=CmdGfxLayout(list);
    RegBind t_ps[8]{};
    {
        std::lock_guard l(g_layout_regmap_mtx);
        auto it=g_layout_regmap.find(layh);
        if(it==g_layout_regmap.end()||!it->second.built)return;
        std::memcpy(t_ps,it->second.t_ps,sizeof(t_ps));
    }
    static std::mutex s_mtx;
    static std::set<std::tuple<unsigned,int,int,unsigned,uint32_t,bool>> s_seen;
    auto* dev=list->get_device();
    const uint32_t guid=g_geomods[dm].mod.object_guid;
    for(unsigned r=0;r<8;++r){
        if(ps_regs.parsed&&!ps_regs.unbounded&&!ps_regs.reads(r))continue;
        const RegBind& b=t_ps[r];
        if(b.param==0xFF||b.param>=64||!b.is_table)continue;
        const TableStart ts=TableStartOf(dev,gs.root[b.param].table);
        if(!ts)continue;
        const HeapKey key{ts.heap,ts.base+b.offset};
        ResInfo ri{};
        if(!ReadPublishedDescriptor(key,ri)||!ri.res)continue;
        const uint64_t res_h=reinterpret_cast<uint64_t>(ri.res);
        const uint32_t hash=TexHashOf(res_h);
        const uint32_t rule=TexRuleHashFor(hash);
        const uint32_t lod=TexLodTargetOf(hash);
        const bool ovr=std::any_of(overrides.begin(),overrides.end(),[&](const ShadowSlotOvr& o){
            return o.param==b.param&&o.offset==b.offset;
        });
        const bool matched=(rule!=0||ovr||ri.swapped!=nullptr);
        {
            std::lock_guard lk(s_mtx);
            if(s_gen.load(std::memory_order_relaxed)!=load){
                s_seen.clear();
                s_emitted.store(0,std::memory_order_relaxed);
                s_gen.store(load,std::memory_order_relaxed);
            }
            if(s_emitted.load(std::memory_order_relaxed)>=256)return;
            if(!s_seen.emplace(load,dm,comp,r,hash,matched).second)continue;
            s_emitted.fetch_add(1,std::memory_order_relaxed);
        }
        TexInfo ti{};
        TexDimsOf(res_h,&ti);
        const uint32_t vfmt=uint32_t(PublishedViewFormat(key));
        uint32_t sub1=0,sub2=0,via=0;
        TexQuerySubDiag(res_h,&sub1,&sub2,&via);
        logger::probe("TEX-UHD-AUDIT gen=%u mod=%d guid=%u comp=%d rtv=%u t%u res=%ux%u m%u fmt=%u vfmt=%u hash=%08x rule=%08x lod=%08x sub1=%08x sub2=%08x via=%u swapped=%d ovr=%d",
            load,dm+1,guid,comp,rtv,r,ti.width,ti.height,ti.mips,ti.format,vfmt,hash,rule,lod,sub1,sub2,via,
            int(ri.swapped!=nullptr),int(ovr));
    }
}
bool RtQueuePrivateSubdraw(SectionDraw& sd,unsigned count,unsigned first,int32_t base){
    if(!active_stream)return false;
    auto &ctx=*active_stream;
    if((!ctx.plan&&!ctx.hybrid)||!count)return true;
    // Compare actual port VM binding, not an assumption based on ShapeKey count.
    const auto *pos=RtCachedPositionResource(ctx.mod);
    PortApi::BufferResourceInfo original{};void* bound=nullptr; unsigned long long va=0;unsigned size=0,stride=0;
    const bool author_position=pos && PortApi::GetBufferResource(pos->fq.c_str(),&original) &&
        PortApi::GetShimVertexBuffer(0,&bound,&va,&size,&stride,nullptr) &&
        bound==original.dx12 && va==original.gpu_va && stride==12 && size>=original.byte_width;
    const unsigned flags=author_position?0u:unsigned(WWMI_RT_PLAN_EXTRA_POSITION_INPUT);
    if(!ctx.hybrid)WWMI_RtModPlanDraw_v1(ctx.plan,count,first,base,flags);
    StreamDraw part{count,first,base,flags};
    part.rfx=ctx.rfx_latch;
    if(!ctx.hybrid)part.rfx.step=RtRfxStep::old_private;
    else if(!ctx.program)part.rfx.step=RtRfxStep::no_color;
    if(author_position)
        part.author_position={reinterpret_cast<uint64_t>(original.dx12),original.gpu_va,original.byte_width,original.stride};
    if(ctx.hybrid&&!RtFreezeHybridPart(ctx,sd,part)){
        if(!ctx.rejection){ctx.rejection="freeze-geometry";ctx.rejected_rfx=part.rfx;}
        ctx.hybrid_failed=true;return true;
    }
    if(ctx.program){
        // Capture the VM's material state at each draw, before the skin's own
        // Cleanup/RestorePixelShaderResources runs at the end of the section.
        const bool material_rt = sd.fr.da.draw_rt.rtv_count >= 2;
        if(ctx.hybrid&&!RtFreezeRabbitFxSlots(sd.fr.da.cmd_list,sd.dm,sd.wc?sd.wc->index:0,
                sd.rfx_tex,ctx.hybrid_rfx,material_rt,part.ps,part.textures,part.ps_owners,part.rfx)){
            if(!(sd.fr.da.draw_rt.rtv_count<4&&part.rfx.step==RtRfxStep::not_native_2d)){
                if(!ctx.rejection){ctx.rejection="freeze-rfx";ctx.rejected_rfx=part.rfx;}
                ctx.hybrid_failed=true;return true;
            }
        }
        // Private/Hybrid draws never run StampClaimedTextures on the game's
        // live descriptor heap; hash-matched `this = ResourceTextureN` swaps
        // must therefore be collected into the private recorded table, yielding
        // to RabbitFX SetTextures on any slot claimed by both while explicit
        // `ps-tN =` remains last with highest precedence.
        const auto& ps=MigotoPort::State().srv[MigotoPort::StageIndex('p')];
        const auto& ps_gen=MigotoPort::State().srv_gen[MigotoPort::StageIndex('p')];
        const uint64_t cur_pso=CmdGfx(sd.fr.da.cmd_list).gfx_pso;
        if(!ctx.stamps_cached||ctx.cached_stamps_pso!=cur_pso||ctx.cached_srv_gen0!=sd.hk.srv_gen0||
           std::memcmp(ps_gen,ctx.cached_ps_gen,sizeof(ctx.cached_ps_gen))!=0){
            static const std::vector<uint32_t> kIncludeThisSwaps;
            ctx.cached_stamps=PlanSectionPsSlots(sd.fr.da.cmd_list,sd.hk.srv_gen0,kIncludeThisSwaps,sd.dm,sd.wc?sd.wc->index:0);
            ctx.cached_stamps_pso=cur_pso;
            ctx.cached_srv_gen0=sd.hk.srv_gen0;
            std::memcpy(ctx.cached_ps_gen,ps_gen,sizeof(ctx.cached_ps_gen));
            ctx.stamps_cached=true;
        }
        const auto& stamps=ctx.cached_stamps;
        for(auto& t:stamps.slots){
            const bool from_this=t.reg<sd.hk.this_gen.size()&&sd.hk.this_gen[t.reg]&&ps_gen[t.reg]==sd.hk.this_gen[t.reg];
            if(from_this&&std::any_of(part.ps.begin(),part.ps.end(),[&](const auto& o){
                    return o.param==t.param&&o.offset==t.offset;
                }))continue;
            part.ps.push_back({t.param,t.offset,t.src});
            if(ctx.hybrid&&part.ps_owners.size()+1==part.ps.size())
                part.ps_owners.push_back(reinterpret_cast<uint64_t>(t.res));
            // A DX12 resource reference alone does not own WrapView's CPU
            // staging slot. The VM may release/rebind it before FinishSection.
            if(ps[t.reg])part.source_views.emplace_back(ps[t.reg]);
            if(t.res)part.textures.emplace_back(t.res);
        }
        if(ctx.hybrid&&part.rfx.step==RtRfxStep::no_call&&sd.fr.da.draw_rt.rtv_count>=2&&sd.wc&&
           !g_geomods[sd.dm].mod.queries_ps_slots)
            RtAuditClassicTexSlots(sd.fr.da.cmd_list,sd.dm,sd.wc->index,sd.fr.da.draw_rt.rtv_count,part.ps);
        if(sd.fr.da.draw_rt.rtv_count>=2&&sd.wc&&!g_geomods[sd.dm].mod.queries_ps_slots&&g_tex_struct_fallback){
            if(!ctx.struct_fallback_cached||ctx.cached_struct_pso!=cur_pso){
                ctx.cached_struct_slots.clear();
                wwmi_struct_tex::g_struct_fallback.Apply(sd.fr.da.cmd_list,sd.dm,sd.wc->index,
                    sd.fr.da.draw_rt.rtv_count,ctx.program?ctx.program->hash:0,ctx.hybrid,part.ps,part.textures,part.ps_owners,
                    &ctx.cached_struct_slots);
                ctx.cached_struct_pso=cur_pso;
                ctx.struct_fallback_cached=true;
            }else{
                for(const auto& fb:ctx.cached_struct_slots){
                    if(std::any_of(part.ps.begin(),part.ps.end(),[&](const auto& o){
                            return o.param==fb.param&&o.offset==fb.offset;
                        }))continue;
                    part.ps.push_back({fb.param,fb.offset,fb.srv});
                    if(ctx.hybrid&&part.ps_owners.size()+1==part.ps.size())
                        part.ps_owners.push_back(reinterpret_cast<uint64_t>(fb.res.Get()));
                    if(fb.res)part.textures.emplace_back(fb.res);
                }
            }
        }
        if(ctx.cloth&&ctx.cloth->material_detail){
            part.trace_part=ctx.cloth->parts.size();
            auto& c=ctx.cloth->parts.emplace_back();
            c.requested_first=first;c.requested_count=count;c.requested_flags=flags;c.carried=ctx.carried_mask;
            c.carry_sources=ctx.cloth->carry_sources;c.format_generation=ctx.cloth->format_generation;
            for(const auto& t:stamps.slots)if(t.reg<9){
                auto& tx=c.tex[t.reg];tx.resource=reinterpret_cast<uint64_t>(t.res);tx.descriptor=t.src.ptr;tx.param=t.param;tx.offset=t.offset;
            }
            const auto& ns=g_geomods[sd.dm].wcl_ns;
            for(unsigned r=0;r<9;++r){
                wchar_t name[]=L"slot_original_format_t0";name[sizeof(name)/sizeof(wchar_t)-2]=wchar_t(L'0'+r);
                c.formats[r]=int(PortApi::GetVarNs(ns.c_str(),name,-1.f));
            }
            c.mod_enabled=PortApi::GetVarNs(ns.c_str(),L"mod_enabled",-1.f);
            c.instance_valid=PortApi::GetVarNs(ns.c_str(),L"instance_valid",-1.f);
            c.object_detected=PortApi::GetVarNs(ns.c_str(),L"object_detected",-1.f);
            RtClothTextureNames(c,sd.dm);
        }
        if(RtVisibleMaterialWindow() && sd.wc && sd.wc->index==3 && CmdRt(sd.fr.da.cmd_list).rtv_count==7 &&
           (ctx.program->hash==0x23830857e27f2dcdULL || ctx.program->hash==0xaad17e90aee71ee7ULL)){
            auto& m=part.selection;m.observed=true;
            m.carried=ctx.carried_mask;
            m.sampled=unsigned(PipelinePsReadMask(CmdGfx(sd.fr.da.cmd_list).gfx_pso))&0x1ff;
            for(auto& t:stamps.slots)if(t.reg<9 && t.res && t.src.ptr)m.bound|=1u<<t.reg;
            const auto& ns=g_geomods[sd.dm].wcl_ns;
            for(unsigned r=0;r<9;++r){
                wchar_t name[]=L"slot_original_format_t0";name[sizeof(name)/sizeof(wchar_t)-2]=wchar_t(L'0'+r);
                m.formats[r]=int(PortApi::GetVarNs(ns.c_str(),name,-1.f));
            }
        }
    }
    ctx.requested.push_back(std::move(part));
    return true;
}
bool RtFinishPrivateSection(uint64_t section,bool complete,InjectFrame& fr){
    if(!active_stream)return false;
    auto &ctx=*active_stream;
    if(ctx.hybrid)return RtFinishHybridSection(ctx,complete,fr);
    const auto &world=ctx.surface();
    WWMI_RtAuthorCommit_v1 commit{ctx.query.device,ctx.query.list,ctx.author.source.list_creation,ctx.author.source.recording,
        ctx.author_scope.plan,0,0,ctx.author.draw_ticket,WWMI_RT_AUTHOR_TENTATIVE,0,ctx.identity.native.component,0};
    struct Failure {RtAuthorFinishReason why=RtAuthorFinishReason::none;unsigned surface=0,phase=0,geometry=0;} failure;
    const auto reject=[&](RtAuthorFinishReason why,unsigned surface=0,unsigned phase=0,unsigned geometry=0){
        failure={why,surface,phase,geometry};return false;
    };
    struct ChooseNative {
        StreamContext& ctx;WWMI_RtAuthorCommit_v1& commit;const Failure& failure;
        ~ChooseNative(){
            if(!commit.plan||commit.disposition==WWMI_RT_AUTHOR_PRIVATE)return;
            commit.disposition=WWMI_RT_AUTHOR_NATIVE;
            ctx.author_decided=true;
            const bool retained=RtCommitAuthor(ctx.reader,commit,RtAuthorChoiceSite::finish,unsigned(failure.why),
                ctx.program?ctx.program->hash:0,failure.surface,failure.phase,failure.geometry)!=0;
            ctx.author_native_refused=!retained;
            if(ctx.cloth){
                if(retained)ctx.cloth->author_disposition=WWMI_RT_AUTHOR_NATIVE;
                else ctx.cloth->native_choice_rejected=1;
            }
        }
    } choose_native{ctx,commit,failure};
    if(!ctx.plan){if(ctx.cloth)ctx.cloth->reason=13;return reject(RtAuthorFinishReason::plan_missing);}
    const auto completed_recipe=std::exchange(ctx.plan,uint64_t{});
    const auto ended=WWMI_RtModPlanEnd_v1(completed_recipe,complete);
    if(!complete){if(ctx.cloth)ctx.cloth->reason=14;return reject(RtPortWorkRefused(fr.da.d3d12_cl)?
        RtAuthorFinishReason::port_work_refused:RtAuthorFinishReason::vm_incomplete);}
    if(ended!=WWMI_RT_MOD_OK){if(ctx.cloth){ctx.cloth->reason=15;ctx.cloth->status=ended;}return reject(RtAuthorFinishReason::plan_end,ended);}
    if(ctx.author_scope.disposition==WWMI_RT_AUTHOR_NATIVE){
        // The whole current scope was rejected before any pass was changed.
        // Keep the completed real VM recipe for the next native pose, without
        // allocating a prefix, CB page, or descriptor mirror for this one.
        ctx.author_decided=true;commit.plan=0;
        if(ctx.cloth)ctx.cloth->reason=27;
        return false;
    }
    WWMI_RtModGeometrySection_v1 selection{};
    const auto selection_status=ctx.author_scope.plan?WWMI_RtModReadAuthorSection_v1(section,ctx.draw_frame,&selection,sizeof(selection),0,0,nullptr):
        WWMI_RtModReadGeometrySection_v1(section,0,&selection,sizeof(selection));
    if(selection_status!=WWMI_RT_MOD_OK){if(ctx.cloth){ctx.cloth->reason=15;ctx.cloth->status=unsigned(selection_status);}return reject(RtAuthorFinishReason::read_selection,selection_status);}
    bool author=ctx.author_scope.plan&&std::none_of(ctx.requested.begin(),ctx.requested.end(),[](const auto& p){return p.flags&WWMI_RT_PLAN_EXTRA_POSITION_INPUT;});
    if(ctx.first_masked&&(!author||ctx.requested.empty()))return reject(RtAuthorFinishReason::no_rest_recipe);
    if(!author){
        if(ctx.author_scope.disposition==WWMI_RT_AUTHOR_PRIVATE){if(ctx.cloth)ctx.cloth->reason=26;return false;}
        if(commit.plan){
            commit.disposition=WWMI_RT_AUTHOR_NATIVE;
            if(!RtCommitAuthor(ctx.reader,commit,RtAuthorChoiceSite::finish,unsigned(RtAuthorFinishReason::not_owned_rest),
                ctx.program?ctx.program->hash:0)){if(ctx.cloth)ctx.cloth->reason=26;return false;}
            commit.disposition=WWMI_RT_AUTHOR_TENTATIVE;
        }
        ctx.author_legacy=ctx.author_scope.plan!=0;
        commit.plan=0; // External FINAL and the old raster contract remain independent.
    }
    wwmi::deform::Publication publication;
    if(author&&!ctx.requested.empty()&&ctx.deformed_owner.plan){
        const auto &source=ctx.requested.front().author_position;
        if(source.resource&&std::all_of(ctx.requested.begin(),ctx.requested.end(),[&](const auto &part){
            return part.author_position==source;
        }))publication=RtPublishSelectedAuthorRest(section,completed_recipe,ctx.deformed_owner,source);
    }
    const auto *selected_source=author?publication.selection(selection.component):nullptr;
    if(ctx.first_masked&&!RtFirstMaskedRest(ctx.deformed_owner,selected_source))
        return reject(RtAuthorFinishReason::no_rest_source);
    std::optional<RtHistoryInvalid> no_history; // applicable only to a single complete subdraw
    if(selected_source&&ctx.program){
        // These prerequisites are already final for this actual original
        // draw. Do not record author work that cannot reach a material draw.
        const auto *heaps=RtOriginalDrawHeaps(fr.da.d3d12_cl);
        if(!heaps||!heaps->resources){
            if(ctx.cloth){ctx.cloth->reason=18;ctx.cloth->status=6;}
            if(!ctx.depth_only)RtWorldNote(5,ctx.program->hash,selection.component,6);
            return reject(RtAuthorFinishReason::missing_heaps);
        }
        if(world.reads_previous&&!ctx.author_scope.previous.order){
            // V2 previous_for requires the actual pre-VM read receipt.
            // A later stamp cannot supply it. Use only a program's existing
            // proven invalid-history path, before preparing a GPU prefix.
            if(!ctx.depth_only&&ctx.requested.size()==1&&ctx.reader.reserve2){
                no_history.emplace();
                if(!RtPrepareHistoryInvalid(fr.da.cmd_list,*ctx.program,world,*no_history))no_history.reset();
            }
            if(!no_history){
                if(ctx.cloth){ctx.cloth->reason=17;ctx.cloth->status=WWMI_RT_SURFACE_PREVIOUS_MISSING;}
                if(!ctx.depth_only)RtWorldNote(4,ctx.program->hash,selection.component,WWMI_RT_SURFACE_PREVIOUS_MISSING);
                return reject(RtAuthorFinishReason::missing_previous,WWMI_RT_SURFACE_PREVIOUS_MISSING,WWMI_RT_DRAW_HISTORY);
            }
        }
    }
    auto draw_packet=[&](const StreamDraw& part){
        WWMI_RtDrawFrameRequest_v1 packet{};packet.struct_size=sizeof(packet);packet.abi_version=1;
        const auto &palette=ctx.palette;
        packet.source={palette.consumer_list_creation,palette.consumer_recording,palette.native_blas_resource,
                       palette.native_blas_creation,palette.native_blas_gpu_va,palette.pose_order};
        auto &req=packet.inputs.current;auto &request=req.planned;auto &draw=request.draw;
        draw.struct_size=sizeof(draw);draw.abi_version=1;draw.native_draw=ctx.query;draw.section_token=section;
        draw.load_generation=selection.load_generation;draw.mod_key=selection.mod_key;draw.mod_revision=selection.mod_revision;
        draw.component=selection.component;draw.mod_index_count=part.count;draw.mod_first_index=part.first;draw.mod_base_vertex=part.base;
        request.frame_id=selection.plan_frame;request.path_flags=WWMI_RT_PLAN_MOD_AUTHOR_SHARED|part.flags;
        req.current_frame=ctx.draw_frame;
        if(ctx.program){
            packet.flags=WWMI_RT_DRAW_MATERIAL;
            req.flags=ctx.program->aux>=0 && !world.aux_constant?unsigned(WWMI_RT_SURFACE_NEED_COLOR):0u;
            if(world.reads_previous){
                packet.flags|=WWMI_RT_DRAW_PREVIOUS;
                packet.inputs.native_previous={world.prev_resource,world.prev_offset,world.prev_bytes,DXGI_FORMAT_R32_FLOAT,0};
            }
        }
        return packet;
    };
    std::optional<unsigned> first_unowned_status;
    auto acquire=[&](WWMI_RtDrawFrameRequest_v1& packet,unsigned ordinal,WWMI_RtDrawFrame_v1& acquired,
                     const WWMI_RtAuthorSectionParts_v3 *parts=nullptr){
        RtPreviousClothTraceScope trace(packet.flags&WWMI_RT_DRAW_PREVIOUS?ctx.cloth:nullptr,packet.flags&WWMI_RT_DRAW_PREVIOUS);
        if(author){
            WWMI_RtAuthorDeformedFrameRequest_v2 request{{{packet,world.previous_scope},ctx.author_scope.plan,ctx.author.draw_ticket,
                ordinal,unsigned(ctx.requested.size()),ctx.author_scope.previous},{}};
            request.base.draw.base.source=ctx.author.source;
            if(selected_source)request.selected=*selected_source;
            if(no_history)request.base.draw.base.flags&=~unsigned(WWMI_RT_DRAW_PREVIOUS);
            const wwmi::deform::ScopedSourceRead source_scope(publication);
            WWMI_RtAuthorFrame_v1 result{};
            unsigned status;
            if(parts){
                const WWMI_RtAuthorSectionFrameRequest_v3 together{request,*parts};
                status=ctx.reader.author_frame(&together.first,sizeof(together),&result,sizeof(result));
            }else status=ctx.reader.author_frame(&request,sizeof(request),&result,sizeof(result));
            if(ctx.first_masked&&(result.plan!=commit.plan||
               (result.disposition!=WWMI_RT_AUTHOR_TENTATIVE&&result.disposition!=WWMI_RT_AUTHOR_PRIVATE))){
                // Refuse before clearing plan or using the independent
                // legacy frame APIs. Keep the real acquire receipt intact.
                acquired=result.frame;first_unowned_status=status;
                return status==WWMI_RT_SURFACE_OK?unsigned(WWMI_RT_SURFACE_NOT_READY):status;
            }
            if(result.disposition!=WWMI_RT_AUTHOR_LEGACY||ctx.author_scope.disposition==WWMI_RT_AUTHOR_PRIVATE){
                if(result.plan&&result.plan!=commit.plan)return unsigned(WWMI_RT_SURFACE_NOT_READY);
                commit.current_batch=result.frame.current.geometry.batch_token;commit.previous_batch=result.previous_batch;
                if(ctx.cloth){ctx.cloth->author_plan=result.plan;ctx.cloth->author_disposition=result.disposition;}
                acquired=result.frame;return status;
            }
            no_history.reset(); // do not carry an author bypass into the old GPU-dependent entry
            author=false; // Explicitly external FINAL; the old entry owns its actual GPU proof.
            ctx.author_legacy=true;
            commit.plan=0;
        }
        if(ctx.reader.frame2&&(packet.flags&WWMI_RT_DRAW_PREVIOUS)){
            const WWMI_RtDrawFrameRequest_v2 scoped{packet,world.previous_scope};
            return ctx.reader.frame2(&scoped,sizeof(scoped),&acquired,sizeof(acquired));
        }
        return ctx.reader.frame(&packet,sizeof(packet),&acquired,sizeof(acquired));
    };
    if(ctx.requested.empty()){
        if(author){
            auto packet=draw_packet(StreamDraw{});WWMI_RtDrawFrame_v1 acquired{};
            const auto status=acquire(packet,0,acquired);
            if(author){
                if(status!=WWMI_RT_SURFACE_OK||!commit.plan){if(ctx.cloth)ctx.cloth->reason=19;return false;}
                commit.empty_selection=1;commit.disposition=WWMI_RT_AUTHOR_PRIVATE;
                if(!RtCommitAuthor(ctx.reader,commit,RtAuthorChoiceSite::private_commit,0,ctx.program?ctx.program->hash:0)){
                    commit.disposition=WWMI_RT_AUTHOR_TENTATIVE;if(ctx.cloth)ctx.cloth->reason=26;return reject(RtAuthorFinishReason::private_rejected);
                }
                if(ctx.cloth)ctx.cloth->author_disposition=WWMI_RT_AUTHOR_PRIVATE;
            }
        }
        ++ctx.empty_selections;ctx.replaced=true;return true;
    }
    struct Material {RtMaterialTables tables;uint64_t local_cb=0;};
    std::vector<Material> material;
    if(ctx.program)material.reserve(ctx.requested.size());
    std::vector<WWMI_RtGraphicsGeometry_v1> views;views.reserve(ctx.requested.size());
    WWMI_RtDrawFrame_v1 shared_frame{};
    std::array<WWMI_RtModDraw_v1,4> local_recipe;
    std::array<WWMI_RtAuthorIndexView_v3,4> local_indices;
    std::vector<WWMI_RtAuthorIndexView_v3> longer_indices;
    auto *indices=local_indices.data();
    unsigned shared_status=WWMI_RT_SURFACE_NOT_READY;
    const bool try_shared=author&&ctx.requested.size()>1;
    if(try_shared){
        auto *recipe=local_recipe.data();
        std::vector<WWMI_RtModDraw_v1> longer_recipe;
        if(ctx.requested.size()>local_recipe.size()){
            longer_recipe.resize(ctx.requested.size());recipe=longer_recipe.data();
            longer_indices.resize(ctx.requested.size());indices=longer_indices.data();
        }
        for(size_t i=0;i<ctx.requested.size();++i){
            const auto &p=ctx.requested[i];recipe[i]={p.count,p.first,p.base};
        }
        auto first=draw_packet(ctx.requested.front());
        const WWMI_RtAuthorSectionParts_v3 parts{recipe,indices};
        shared_status=acquire(first,0,shared_frame,&parts);
    }
    const bool shared_author=try_shared&&author;
    std::optional<RtWorldMaterialInputs> shared_material;
    uint64_t previous_order=0,previous_resource=0;
    for(unsigned part_index=0;part_index<ctx.requested.size();++part_index){
        auto& part=ctx.requested[part_index];auto packet=draw_packet(part);
        WWMI_RtDrawFrame_v1 acquired{};
        unsigned status;
        if(try_shared&&(author||!part_index)){
            status=shared_status;acquired=shared_frame;
            if(author&&status==WWMI_RT_SURFACE_OK){
                auto &g=acquired.current.geometry;const auto &index=indices[part_index];
                g.index_gpu_va=index.gpu_va;g.index_offset=index.offset;g.index_bytes=index.bytes;g.index_count=index.count;
            }
        }else status=acquire(packet,part_index,acquired);
        const auto &surface=acquired.current;const auto &v=surface.geometry;const auto &history=acquired.previous;
        part.issued_uv_pairs=surface.uv_pairs;
        bool history_continuation=false;
        if(!first_unowned_status&&status==WWMI_RT_SURFACE_PREVIOUS_MISSING &&
            acquired.failed_phase==WWMI_RT_DRAW_HISTORY && acquired.geometry_status==WWMI_RT_GEOMETRY_OK &&
            acquired.struct_size==sizeof(acquired) && acquired.abi_version==1 &&
            surface.struct_size==sizeof(surface) && surface.abi_version==1 &&
            v.struct_size==sizeof(v) && v.abi_version==1 && v.batch_token && acquired.current_pose_order==packet.source.pose_order &&
            v.consumer_list==reinterpret_cast<uint64_t>(fr.da.cmd_list) &&
            v.consumer_list_creation==packet.source.list_creation && v.consumer_recording==packet.source.recording &&
            v.index_count==part.count && packet.flags==(WWMI_RT_DRAW_MATERIAL|WWMI_RT_DRAW_PREVIOUS) &&
            !ctx.depth_only && ctx.program && ctx.requested.size()==1 && ctx.reader.reserve2){
            no_history.emplace();
            if(!RtPrepareHistoryInvalid(fr.da.cmd_list,*ctx.program,world,*no_history))no_history.reset();
            history_continuation=bool(no_history);
        }
        if(status!=WWMI_RT_SURFACE_OK&&!history_continuation){
            const bool previous=acquired.failed_phase==WWMI_RT_DRAW_HISTORY;
            if(previous && ctx.program && !ctx.depth_only && selection.component==3 && ctx.program->hash==0xaad17e90aee71ee7ULL){
                static std::atomic<unsigned> seen{};
                const unsigned reason=world.prev_resource?4u:2u;
                if(!(seen.fetch_or(reason)&reason)){
                    char line[184];std::snprintf(line,sizeof(line),"RT-WORLD-PRIOR frame=%llu binding=%u called=%u status=%u native_view_result=%u",
                        (unsigned long long)ctx.draw_frame,unsigned(reason==4),unsigned(reason==4),status,world.previous_resolution);
                    reshade::log::message(reshade::log::level::info,line);
                }
            }
            if(ctx.cloth){ctx.cloth->reason=ctx.program?(previous?17u:16u):19u;ctx.cloth->status=ctx.program?status:acquired.geometry_status;}
            if(ctx.program&&!ctx.depth_only)RtWorldNote(previous?4:3,ctx.program->hash,selection.component,status);
            return reject(first_unowned_status?RtAuthorFinishReason::frame_owner:RtAuthorFinishReason::frame_result,
                first_unowned_status.value_or(status),acquired.failed_phase,acquired.geometry_status);
        }
        const bool other_current=!views.empty()&&views.front().batch_token!=v.batch_token;
        if(other_current||(!views.empty()&&(previous_order!=history.pose_order||previous_resource!=history.positions.resource))){
            // No partly prepared section may mix poses after another draw has
            // advanced either source while we were preparing its subdraws.
            const auto mismatch=other_current?WWMI_RT_SURFACE_NOT_READY:WWMI_RT_SURFACE_PREVIOUS_MISSING;
            if(ctx.cloth){ctx.cloth->reason=other_current?16u:17u;ctx.cloth->status=mismatch;}
            if(ctx.program&&!ctx.depth_only)RtWorldNote(other_current?3:4,ctx.program->hash,selection.component,mismatch);
            return reject(RtAuthorFinishReason::incoherent_parts,mismatch,other_current?WWMI_RT_DRAW_CURRENT:WWMI_RT_DRAW_HISTORY);
        }
        previous_order=history.pose_order;previous_resource=history.positions.resource;
        if(ctx.program){
            material.emplace_back();auto& m=material.back();
            auto *invalid=no_history?&*no_history:nullptr;
            bool prepared;
            if(shared_author){
                prepared=true;
                if(!shared_material){
                    shared_material.emplace();
                    prepared=RtPrepareWorldMaterialInputs(fr.da.cmd_list,*ctx.program,world,surface,history,invalid!=nullptr,
                        *shared_material,m.tables,true);
                }
                if(prepared)prepared=RtPrepareWorldMaterialPart(fr.da.cmd_list,world,surface,*shared_material,part.ps,
                    part.textures,ctx.reader.reserve,m.tables,m.local_cb,ctx.reader.reserve2,invalid);
            }else prepared=RtPrepareWorldMaterial(fr.da.cmd_list,*ctx.program,world,surface,history,part.ps,part.textures,
                ctx.reader.reserve,m.tables,m.local_cb,ctx.reader.reserve2,invalid);
            if(!prepared){
                const unsigned why=m.tables.failure;
                const unsigned code=why!=8 ? why : (8u | ((m.tables.missing_param+1)<<8) | ((m.tables.missing_offset+1)<<16));
                if(ctx.cloth){ctx.cloth->reason=18;ctx.cloth->status=code;}
                if(!ctx.depth_only)RtWorldNote(5,ctx.program->hash,selection.component,code);return reject(RtAuthorFinishReason::material_tables,code);
            }
        }
        if(v.position_bytes>UINT32_MAX || v.index_bytes>UINT32_MAX){if(ctx.cloth)ctx.cloth->reason=20;return reject(RtAuthorFinishReason::invalid_range);}
        views.push_back(v);
    }
    shared_material.reset(); // every part now owns its immutable table range
    if(author&&!commit.plan)return false;
    // Acquire all actual parts first; do not draw half a skin over the native one.
    RestoreGameState(fr);
    auto cl=fr.da.d3d12_cl;
    // Depth retains the exact native PSO (including depth/stencil/raster state),
    // not a material/decensor variant. OM targets, viewport and scissor are unchanged.
    const auto& graphics=CmdGfx(fr.da.cmd_list);
    if(ctx.first_masked){
        const auto actual=CmdRt(fr.da.cmd_list);
        const auto& original=fr.da.draw_rt;
        if(!actual.valid||actual.pass_gen!=original.pass_gen||actual.has_dsv!=original.has_dsv||
           actual.dsv!=original.dsv||actual.rtv_count!=original.rtv_count||
           !std::equal(std::begin(original.rtv),std::end(original.rtv),std::begin(actual.rtv))||
           graphics.gfx_pso!=world.native_pso||graphics.gfx_restore()!=world.native_pso||
           CmdGfxLayout(fr.da.cmd_list)!=world.native_layout)return reject(RtAuthorFinishReason::lost_ps_o_m);
    }
    if(author){
        commit.disposition=WWMI_RT_AUTHOR_PRIVATE;
        if(!RtCommitAuthor(ctx.reader,commit,RtAuthorChoiceSite::private_commit,unsigned(ctx.first_masked),ctx.program?ctx.program->hash:0)){
            commit.disposition=WWMI_RT_AUTHOR_TENTATIVE;if(ctx.cloth)ctx.cloth->reason=26;return reject(RtAuthorFinishReason::private_rejected);
        }
        if(ctx.cloth)ctx.cloth->author_disposition=WWMI_RT_AUTHOR_PRIVATE;
    }
    const auto work_deadline=author&&ctx.reader.author_work&&RtVisibleMaterialWindow()?
        WWMI_RtDrawWindowDeadline_v1(ctx.query.device,nullptr):0;
    uint64_t timer=0,issued_indices=0;
    if(work_deadline)
        timer=ctx.reader.author_work(&commit,sizeof(commit),work_deadline,ctx.draw_frame,
            WWMI_RT_SECTION_WORK_BEFORE,0,0,0);
    cl->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(ctx.depth_only?graphics.gfx_pso:graphics.gfx_restore()));
    struct RestoreMaterialSection {
        ID3D12GraphicsCommandList* list;
        const RtWorldSurface& world;
        const RfxBindState* original=nullptr;
        ~RestoreMaterialSection(){
            if(!original)return;
            list->SetGraphicsRootConstantBufferView(world.local_param,world.native_cb);
            RfxRestore(list,*original);
        }
    } restore_material{cl,world};
    for(size_t i=0;i<views.size();++i){
        const auto& v=views[i];
        D3D12_VERTEX_BUFFER_VIEW vb{v.position_gpu_va,(UINT)v.position_bytes,v.position_stride};
        D3D12_INDEX_BUFFER_VIEW ib{v.index_gpu_va,(UINT)v.index_bytes,(DXGI_FORMAT)v.index_format};
        if(!i||!shared_author)cl->IASetVertexBuffers(0,1,&vb);
        cl->IASetIndexBuffer(&ib);
        if(ctx.program){
            restore_material.original=&material[i].tables.original;
            // The native binding scope already suppresses redundant heap
            // sets. Parts on a new page still bind it and their own tables.
            RtBindMaterialTables(cl,material[i].tables);
            cl->SetGraphicsRootConstantBufferView(world.local_param,material[i].local_cb);
            if(no_history)for(unsigned j=0;j<WWMI_RT_NH_CB_COUNT;++j)
                if(no_history->va[j])cl->SetGraphicsRootConstantBufferView(world.history_roots[j].param,no_history->va[j]);
        }
        cl->DrawIndexedInstanced(v.index_count,1,v.first_index,v.base_vertex,0);
        if(v.index_count)RtMaterialDrawNote(fr.da.cmd_list,ctx.mod,selection.component,fr.da.draw_rt.rtv_count,
            ctx.depth_only?graphics.gfx_pso:graphics.gfx_restore(),RtNoteRoute::old_private,
            "submitted",v.first_index,v.index_count,v.base_vertex,&ctx.requested[i].rfx,ctx.requested[i].ps,
            ctx.program?&material[i].tables:nullptr,ctx.requested[i].issued_uv_pairs);
        if(work_deadline)issued_indices+=v.index_count;
        if(no_history){
            for(unsigned j=0;j<WWMI_RT_NH_CB_COUNT;++j)if(no_history->va[j])
                cl->SetGraphicsRootConstantBufferView(world.history_roots[j].param,world.history_roots[j].va);
            if(v.index_count){
                ctx.history_invalid_draw=true;
                RtWorldNote(9,ctx.program->hash,selection.component,WWMI_RT_SURFACE_PREVIOUS_MISSING);
            }
        }
        if(v.index_count)++ctx.recorded_subdraws;
        if(ctx.cloth&&ctx.requested[i].trace_part<ctx.cloth->parts.size()){
            auto& part=ctx.cloth->parts[ctx.requested[i].trace_part];
            part.emitted_first=v.first_index;part.emitted_count=v.index_count;part.emitted_base=v.base_vertex;
            part.draw_pso=ctx.depth_only?graphics.gfx_pso:graphics.gfx_restore();
            const auto regs=PipelineSrvRegs(part.draw_pso,'p');part.ps_declared=regs.lo;part.ps_known=regs.parsed&&!regs.unbounded;part.ps_unbounded=regs.unbounded;
            if(ctx.program)for(auto& t:part.tex)if(t.descriptor){
                for(const auto& [root,table]:material[i].tables.tables)if(root==t.param){t.bound_table=table.ptr;break;}
                // The mirror validates each override's destination, then copies
                // in order. VS overrides cannot replace a different PS source.
                const auto& ps=ctx.requested[i].ps;
                const auto last=std::find_if(ps.rbegin(),ps.rend(),[&](const auto& b){return b.param==t.param&&b.offset==t.offset;});
                if(t.bound_table&&last!=ps.rend())t.recorded_source=last->src.ptr;
            }
        }
        const auto& chosen=ctx.requested[i].selection;
        if(chosen.observed && RtVisibleMaterialWindow()){
            if(v.index_count && (chosen.bound&1u))++ctx.author_t0_draws;
            char line[320];std::snprintf(line,sizeof(line),"RT-AUTHOR vs=%llx pso=%llx ps_reads=%x bound_author_ps=%x carried_fmt=%x vm_formats=%d,%d,%d,%d,%d,%d,%d,%d,%d indices=%u",
                (unsigned long long)ctx.program->hash,(unsigned long long)graphics.gfx_pso,chosen.sampled,chosen.bound,chosen.carried,
                chosen.formats[0],chosen.formats[1],chosen.formats[2],chosen.formats[3],chosen.formats[4],chosen.formats[5],
                chosen.formats[6],chosen.formats[7],chosen.formats[8],v.index_count);
            static std::mutex audit_mu;static std::set<std::string> seen;
            std::lock_guard lock(audit_mu);if(seen.size()<12 && seen.insert(line).second)
                reshade::log::message(reshade::log::level::info,line);
        }
        if(ctx.program){
            if(!ctx.depth_only)RtWorldNote(6,ctx.program->hash,selection.component,v.index_count);
        }
        const auto &part=ctx.requested[i];WWMI_RtModSectionDraw_v1(section,part.count,part.first,part.base);
    }
    if(work_deadline)ctx.reader.author_work(&commit,sizeof(commit),work_deadline,ctx.draw_frame,WWMI_RT_SECTION_WORK_AFTER,
        timer,ctx.recorded_subdraws,issued_indices);
    ctx.replaced=true;return true;
}

namespace PortApi {void PushRtVirtualConstantBuffer(char,unsigned,ID3D12Resource*,unsigned long long,unsigned,unsigned);}
static bool PrepareWorldSkinInputs(StreamContext& ctx,reshade::api::command_list* list,int component){
    const int reg=RtPrimitiveRegister(CmdGfx(list).gfx_pso);
    RootCbvBind primitive{};
    if(reg>=0 && unsigned(reg)<RegisterFileWidth(TargetClass::ConstantBuffer)){
        std::lock_guard lock(g_layout_regmap_mtx);
        auto i=g_layout_regmap.find(CmdGfxLayout(list));
        if(i!=g_layout_regmap.end()){
            auto b=i->second.b_vs[reg];
            if(!b.is_table && b.param<64)primitive=CmdCbv(list).binds[b.param];
        }
    }
    if(!primitive.res || !primitive.va || primitive.size<720 || !ctx.reader.palette){if(ctx.cloth){ctx.cloth->reason=12;ctx.cloth->status=1;}return false;}
    WWMI_RtNativePalette_v1 palette{};
    const auto status=ctx.author.token?
        ctx.reader.author_palette(&ctx.query,sizeof(ctx.query),ctx.author.draw_ticket,component,&palette,sizeof(palette)):
        ctx.reader.palette(&ctx.query,sizeof(ctx.query),component,&palette,sizeof(palette));
    if(status!=WWMI_RT_PALETTE_OK || !palette.resource || !palette.gpu_va || !palette.bytes || palette.bytes>UINT32_MAX){
        if(ctx.cloth){ctx.cloth->reason=12;ctx.cloth->status=status==WWMI_RT_PALETTE_OK?2u:(0x100u|unsigned(status));}return false;
    }
    const auto &before=ctx.identity,&after=palette.native_identity;
    const auto &a=before.native,&b=after.native;
    if(std::tie(a.object_guid,a.content_hash,a.component,a.list,a.list_creation,a.recording,a.content_resource,a.content_creation,
                a.content_offset,a.output_resource,a.output_creation,a.position_base,a.output_offset,a.output_bytes,
                before.index_resource,before.index_creation,before.index_offset)!=
       std::tie(b.object_guid,b.content_hash,b.component,b.list,b.list_creation,b.recording,b.content_resource,b.content_creation,
                b.content_offset,b.output_resource,b.output_creation,b.position_base,b.output_offset,b.output_bytes,
                after.index_resource,after.index_creation,after.index_offset)){
        if(ctx.cloth){ctx.cloth->reason=12;ctx.cloth->status=0x100u|unsigned(WWMI_RT_PALETTE_NATIVE_UNPROVEN);}return false;
    }
    ctx.primitive=primitive;ctx.palette=palette;return true;
}
void RtPublishWorldSkeletonInputs(){
    const auto &primitive=active_stream->primitive;const auto &palette=active_stream->palette;
    PortApi::PushRtVirtualConstantBuffer('v',2,primitive.res,primitive.va,720,1024);
    PortApi::PushGameConstantBuffer('v',3,nullptr,0,0); // previous pose not acquired
    PortApi::PushRtVirtualConstantBuffer('v',4,reinterpret_cast<ID3D12Resource*>(palette.resource),palette.gpu_va,(unsigned)palette.bytes,16384);
}
