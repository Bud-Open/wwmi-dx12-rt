// Classify each physical offset from its actual range and visibility. An
// unreadable PS cannot make a VS-only table unknown (and vice versa).
#include "rt_material_layout.h"
bool RtKnownNoPixelShader(uint64_t pipeline);
DxilSrvRegs RtPipelineUavRegs(uint64_t pipeline,bool pixel);
struct RtReadDecision {
    bool known=false,read=true; unsigned reg=~0u,space=~0u,type=~0u,visibility=0,vs_kind=0,ps_kind=0;
};
static RtReadDecision RtDecideOffset(const std::vector<RtTableRange>& ranges,unsigned param,unsigned offset,
                                     const DxilSrvRegs& vs,const DxilSrvRegs& ps,const DxilSrvRegs& vu,const DxilSrvRegs& pu,bool no_ps){
    using namespace reshade::api;
    RtReadDecision decision{};
    for(const auto& row:ranges){
        const auto& d=row.desc;
        if(row.param!=param||offset<d.binding||offset-d.binding>=d.count)continue;
        // Aliasing ranges are not proof of a single binding.
        if(decision.type!=~0u){decision.known=false;decision.read=true;return decision;}
        decision.type=(unsigned)d.type;decision.visibility=(unsigned)d.visibility;decision.space=d.dx_register_space;
        if(d.dx_register_index>UINT32_MAX-(offset-d.binding))continue;
        decision.reg=d.dx_register_index+(offset-d.binding);
        bool v=(uint32_t(d.visibility)&uint32_t(shader_stage::vertex))!=0;
        bool p=!no_ps && (uint32_t(d.visibility)&uint32_t(shader_stage::pixel))!=0;
        if(!v&&!p){decision.known=true;decision.read=false;continue;}
        const bool uav=d.type==descriptor_type::unordered_access_view||d.type==descriptor_type::buffer_unordered_access_view;
        const bool srv=d.type==descriptor_type::shader_resource_view||d.type==descriptor_type::buffer_shader_resource_view;
        const auto& vregs=uav?vu:vs;const auto& pregs=uav?pu:ps;
        if((!srv&&!uav)||decision.space||decision.reg>=128||
           (v&&(!vregs.parsed||vregs.unbounded))||(p&&(!pregs.parsed||pregs.unbounded)))continue;
        decision.vs_kind=v?vregs.kind[decision.reg]:0;decision.ps_kind=p?pregs.kind[decision.reg]:0;
        decision.known=true;decision.read=(v&&vregs.has(decision.reg))||(p&&pregs.has(decision.reg));
    }
    return decision;
}
static const ReadSlots& RtMaterialReadSlots(reshade::api::command_list* list,const Param0Window& w){
    struct Memo {uint64_t pso=0,layout=0;uint32_t frame=~0u;ReadSlots slots{};};
    static thread_local Memo cache[4];static thread_local unsigned cursor;
    const auto pso=CmdGfx(list).gfx_pso,layout=CmdGfxLayout(list);
    for(auto& m:cache)if(m.pso==pso&&m.layout==layout&&m.frame==g_frame_id)return m.slots;
    auto& m=cache[cursor++%4];m.pso=pso;m.layout=layout;m.frame=g_frame_id;m.slots={};
    if(!RtOnlyVertexPixelStages(pso))return m.slots;
    const auto vs=PipelineSrvRegs(pso,'v'),ps=PipelineSrvRegs(pso,'p');const auto vu=RtPipelineUavRegs(pso,false),pu=RtPipelineUavRegs(pso,true);const bool no_ps=!ps.parsed&&RtKnownNoPixelShader(pso);
    std::lock_guard lock(rt_material_layout_mutex);auto rows=rt_material_layouts.find(layout);
    if(rows==rt_material_layouts.end())return m.slots;
    for(const auto& t:w.ltabs){
        if(t.param>=64||!t.count||t.count>kTexScanMax)continue;
        // Preserve unknown offsets and kinds; clear only a positively unused slot.
        m.slots.want[t.param].set();
        for(unsigned j=0;j<t.count;++j){
            auto d=RtDecideOffset(rows->second,t.param,j,vs,ps,vu,pu,no_ps);
            if(d.known&&!d.read)m.slots.want[t.param].reset(j);
        }
        m.slots.known[t.param]=true;
    }
    return m.slots;
}
// This is the first missing physical slot across all eight A13 sections. Keep
// its provenance at the actual failing draw rather than guessing it is a PS t10.
static void RtNoteMaterialSource(reshade::api::command_list* list,uint64_t heap,unsigned base,
                                 unsigned param,unsigned off,bool have,const ResInfo& ri){
    if(!list || param!=2)return;
    static std::atomic<uint32_t> lines{0};
    if(lines.load(std::memory_order_relaxed)>=6)return;
    const auto pso=CmdGfx(list).gfx_pso,layout=CmdGfxLayout(list);
    const auto vs=PipelineSrvRegs(pso,'v'),ps=PipelineSrvRegs(pso,'p');const auto vu=RtPipelineUavRegs(pso,false),pu=RtPipelineUavRegs(pso,true);const bool no_ps=RtKnownNoPixelShader(pso);
    RtReadDecision d{};{
        std::lock_guard lock(rt_material_layout_mutex);auto it=rt_material_layouts.find(layout);
        if(it!=rt_material_layouts.end())d=RtDecideOffset(it->second,param,off,vs,ps,vu,pu,no_ps);
    }
    const auto descriptor=ReadPublishedDescriptorDiagnostic({heap,base+off});
    const auto at=descriptor.write;
    const auto src=descriptor.origin;
    const auto live=descriptor.origin_live;
    static std::atomic<uint64_t> last{};
    uint64_t key=(pso>>3)^(uint64_t(d.type)<<41)^(uint64_t(d.known)<<39)^(uint64_t(have)<<38)^(uint64_t(live)<<37)^(uint64_t(at!=0)<<36);
    if(last.exchange(key)==key || lines.fetch_add(1)>=6)return;
    char msg[512];std::snprintf(msg,sizeof(msg),"RT-MATERIAL-SOURCE frame=%u pso=%llx p=%u off=%u type=%u r=%u space=%u vis=%x classified=%u used=%u vk=%u pk=%u vsParsed=%u psParsed=%u psAbsent=%u heap=%llx base=%u origin=%llx write_generation=%llu origin_live=%u sourceMatch=%u matchedView=%llx",
        (unsigned)g_frame_id+1,(unsigned long long)pso,param,off,d.type,d.reg,d.space,d.visibility,d.known,d.read,d.vs_kind,d.ps_kind,vs.parsed,ps.parsed,no_ps,
        (unsigned long long)heap,base,(unsigned long long)src,(unsigned long long)at,live,have,(unsigned long long)ri.view_handle);
    reshade::log::message(reshade::log::level::info,msg);
}
