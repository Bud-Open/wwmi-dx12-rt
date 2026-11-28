// World LocalVF programs captured from actual RT-enabled character draws.
// This table identifies the program's data contract, not a skin/controller.
#include "addon_common.h"
#include "rt_localvf_inputs.h"
#include "rt_program_runtime.h"
#include "rt_previous_descriptor.h"
#include "mods/layout_registry.h"
#include "hooks/cmd_state.h"
#include "hooks/descriptor_shadow.h"
#include "rt_history_roots.h"
#include "rt_material_layout.h"
#include "rt_torso_ps.h"
#include "wc_copy_seh.hpp"
#include <cstring>
#include <new>
#include <unordered_map>
#include <tuple>

bool RtNativeIaHasColor(uint64_t);
namespace {
using List=reshade::api::command_list;
constexpr RtLocalProgram programs[]={
    {0x1d81f22188e43d16ULL,0,1,-1,2,false}, // draw 41
    {0xaad17e90aee71ee7ULL,0,1,-1,2,false}, // draw 75
    {0x6f87d6fe6100e275ULL,0,1,-1,2,false}, // draw 104
    {0xdf33ba06aa41929aULL,0,1,-1,2,false}, // draw 269
    {0xc5ca17d16251294aULL,0,1,2,3,false}, // draw 309
    {0x9731862f398ac9aULL,0,1,2,3,false}, // draw 344
    {0xdab05449f3159b7dULL,0,1,-1,2,false}, // draw 384
    {0xafbc4278dae25417ULL,0,1,2,3,false}, // draw 461
    {0x1b4ed7b6e4b7455dULL,-1,0,1,2,true}, // draw 496
    {0xdaed5c8b5efa34baULL,0,1,-1,2,false}, // draw 527
    {0x2a4e495a3dde516ULL,0,1,-1,2,false}, // draw 563
    {0x56463b2df44dbcefULL,0,1,2,3,false}, // draw 678
    {0xa74d4b12cc0fb2e8ULL,0,1,2,3,true}, // draw 740
    {0x44fc785c22b368a2ULL,0,1,2,3,true}, // draw 773
    {0x23830857e27f2dcdULL,0,1,2,3,true}, // draw 806

};
struct NoHistoryProgram {uint64_t vs,ps;unsigned rtv_count;int ps_primitive;};
// Actual native DXIL pairs. The 7-target PS paths write native invalid
// velocity (zero Target4) when both stage-local gates are closed. The 1-target
// PS does not read previous at all. These are program contracts, not pack IDs.
constexpr NoHistoryProgram no_history_programs[]={
    {0xdab05449f3159b7dULL,0x7b4b04ad91d93db1ULL,7,2}, // PS b1 is ToonShadowLightData
    {0x56463b2df44dbcefULL,0x43418a165a198e87ULL,7,1},
    {0x6f87d6fe6100e275ULL,0xba076214c240bc26ULL,1,-1},

};
const NoHistoryProgram* no_history_program(List* list,uint64_t vs,const RtProgramPsoContract* contract,NoHistoryProgram& derived){
    const auto& g=CmdGfx(list);const auto& rt=CmdRt(list);
    if(!g.gfx_pso||g.gfx_restore()!=g.gfx_pso||!rt.valid||!rt.has_dsv||!rt.dsv)return nullptr;
    uint64_t ps=0;list->get_private_data(s_iid_ps_hash,&ps);
    if(contract){
        if(contract->native_pso!=g.gfx_pso||contract->native_layout!=CmdGfxLayout(list)||
           !contract->capabilities.current_previous||!contract->capabilities.no_history||
           contract->vertex->native_identity!=vs||contract->pixel->native_identity!=ps||
           contract->target_count!=rt.rtv_count)return nullptr;
        derived={vs,ps,rt.rtv_count,contract->capabilities.pixel_primitive};
        return &derived;
    }
    // A VS can have several actual PS/RT contracts. A different earlier row
    // must not hide a later exact match; the native PSO remains unchanged.
    static const auto legacy=[](){
        std::map<std::tuple<uint64_t,uint64_t,unsigned>,const NoHistoryProgram*> index;
        for(const auto& p:no_history_programs)index.emplace(std::tuple{p.vs,p.ps,p.rtv_count},&p);
        return index;
    }();
    const auto i=legacy.find(std::tuple{vs,ps,rt.rtv_count});
    return i==legacy.end()?nullptr:i->second;
}
bool check_upload_window(const RootCbvBind& binding,uint32_t bytes){
    if(!binding.res || !binding.va || !bytes || bytes>binding.size)return false;
    const auto base=binding.res->GetGPUVirtualAddress();
    const auto desc=binding.res->GetDesc();
    if(!base||binding.va<base||binding.va-base>desc.Width||bytes>desc.Width-(binding.va-base))return false;
    D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
    return SUCCEEDED(binding.res->GetHeapProperties(&heap,&flags))&&heap.Type==D3D12_HEAP_TYPE_UPLOAD;
}
template<class Read> bool read_upload(const RootCbvBind& binding,uint32_t begin,uint32_t end,Read copy,uint32_t full_bytes=0,bool expand_range=true){
    if(!binding.res || !binding.va || begin>end || end>binding.size)return false;
    const auto base=binding.res->GetGPUVirtualAddress();
    const auto desc=binding.res->GetDesc();
    if(!base||binding.va<base||binding.va-base>desc.Width||end>desc.Width-(binding.va-base))return false;
    D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
    if(FAILED(binding.res->GetHeapProperties(&heap,&flags))||heap.Type!=D3D12_HEAP_TYPE_UPLOAD)return false;
    const auto origin=binding.va-base;
    // An unsupported full window does not invalidate the established gate and
    // scope read. Map the larger range only if this source actually has it and
    // the caller intends to read it immediately.
    const bool full=full_bytes && full_bytes>=end && full_bytes<=binding.size && full_bytes<=desc.Width-origin;
    const bool map_full=full && expand_range;
    D3D12_RANGE read{SIZE_T(origin+(map_full?0:begin)),SIZE_T(origin+(map_full?full_bytes:end))},written{};
    void* mapped=nullptr;if(FAILED(binding.res->Map(0,&read,&mapped)))return false;
    const bool ok=mapped&&copy(static_cast<const unsigned char*>(mapped)+origin,origin,full);
    binding.res->Unmap(0,&written);return ok;
}
bool upload_bytes(const RootCbvBind& binding,uint32_t offset,void* dst,uint32_t bytes){
    return read_upload(binding,offset,offset+bytes,[&](const auto* mapped,uint64_t,bool){
        return wwmi::safe_copy_from_wc(dst,mapped+offset,bytes);
    });
}
bool copy_author_instance(const RootCbvBind& cb,const unsigned char* p,uint64_t origin,WWMI_RtAuthorInstance_v1& out){
    static_assert(sizeof(out.primitive_words)==WWMI_RT_NH_PRIMITIVE_BYTES);
    WWMI_RtAuthorInstance_v1 staged{};
    if(!wwmi::safe_copy_from_wc(staged.primitive_words,p,sizeof(staged.primitive_words)))return false;
    // The actual VS uses b1[0..3].xyz for current position. b1[10..13]
    // belongs only to the previous-position transform, not instance scope.
    const auto* cached=reinterpret_cast<const unsigned char*>(staged.primitive_words);
    for(unsigned i=0;i<4;++i)std::memcpy(staged.local_to_world+3*i,cached+16*i,12);
    staged.resource=reinterpret_cast<uint64_t>(cb.res);staged.byte_origin=origin;
    out=staged;
    return true;
}
bool reg_of(List* list,bool srv,int reg,RegBind& out){
    if(reg<0||unsigned(reg)>=RegisterFileWidth(srv?TargetClass::ShaderResource:TargetClass::ConstantBuffer))return false;
    std::lock_guard lk(g_layout_regmap_mtx);
    auto i=g_layout_regmap.find(CmdGfxLayout(list));if(i==g_layout_regmap.end())return false;
    out=srv?i->second.t_vs[reg]:i->second.b_vs[reg];return out.param<64;
}
bool vertex_only_previous(uint64_t layout,int reg,const RtWorldSurface::StreamSlot& binding){
    if(reg<0||binding.param>=64)return false;
    std::lock_guard lock(rt_material_layout_mutex);
    auto i=rt_material_layouts.find(layout);if(i==rt_material_layouts.end())return false;
    bool found=false;
    for(const auto& range:i->second){
        const auto& r=range.desc;
        if(range.param!=binding.param||binding.offset<r.binding||binding.offset-r.binding>=r.count)continue;
        using namespace reshade::api;
        if(r.type!=descriptor_type::shader_resource_view||r.visibility!=shader_stage::vertex||r.dx_register_space||
           uint64_t(r.dx_register_index)+binding.offset-r.binding!=unsigned(reg))return false;
        found=true;
    }
    return found;
}
bool history_varying_unused(List* list,uint64_t vs,uint64_t layout,uint64_t pso){
    // The actual previous-buffer loads in these two VS binaries reach only
    // VELOCITY_PREV_POS. A paired PS that consumes it still needs exact
    // history; seven targets alone do not prove an invalid-velocity branch.
    if(vs!=0xaad17e90aee71ee7ULL&&vs!=0x23830857e27f2dcdULL)return false;
    const auto& rt=CmdRt(list);
    if(!rt.valid||!rt.has_dsv||!rt.dsv)return false;
    uint64_t ps=0;list->get_private_data(s_iid_ps_hash,&ps);
    const bool single=vs==0xaad17e90aee71ee7ULL&&ps==0x94b809e92a2ad4a1ULL&&rt.rtv_count==1;
    const bool gbuffer=rt.rtv_count==7&&((vs==0xaad17e90aee71ee7ULL&&ps==0xd296022e79aa9bcfULL)||
        (vs==0x23830857e27f2dcdULL&&ps==0x2e5b6fc9d5451064ULL));
    if(!single&&!gbuffer)return false;
    return RtKnownUnusedPreviousVarying(pso,layout,vs,ps,rt.rtv_count);
}
}
const RtLocalProgram* RtLocalProgramFor(uint64_t vs){
    static const auto legacy=[](){
        std::unordered_map<uint64_t,const RtLocalProgram*> index;
        for(const auto& p:programs)index.emplace(p.hash,&p);
        return index;
    }();
    const auto i=legacy.find(vs);return i==legacy.end()?nullptr:i->second;
}
bool RtReadAuthorInstance(List* list,int primitive_reg,WWMI_RtAuthorInstance_v1& out){
    RegBind binding{};
    if(!reg_of(list,false,primitive_reg,binding)||binding.is_table)return false;
    const auto cb=CmdCbv(list).binds[binding.param];
    if(cb.size<WWMI_RT_NH_PRIMITIVE_BYTES)return false;
    return read_upload(cb,0,WWMI_RT_NH_PRIMITIVE_BYTES,[&](const auto* p,uint64_t origin,bool){
        return copy_author_instance(cb,p,origin,out);
    });
}
static thread_local uint32_t t_rt_aux_native_vertex_end=0;
uint32_t RtSetAuxNativeVertexEnd(uint32_t end) noexcept {
    const uint32_t prev=t_rt_aux_native_vertex_end;
    t_rt_aux_native_vertex_end=end;
    return prev;
}
#include "rt_aux_color.inc"
bool RtReadWorldSurface(List* list,const RtLocalProgram& prog,uint32_t guid,uint32_t hash,unsigned component,RtWorldSurface& out,
                        bool capture_previous_scope,bool capture_history_invalid,WWMI_RtAuthorInstance_v1* instance){
    RegBind view,primitive,local;
    const auto layout=CmdGfxLayout(list);
    const bool can_capture_fallback=capture_previous_scope&&capture_history_invalid;
    const auto* contract=out.contracts.get();
    if(contract&&(!contract->bindings.valid||!contract->capabilities.current_previous||
       contract->native_pso!=CmdGfx(list).gfx_pso||contract->native_layout!=layout||
       contract->vertex->native_identity!=prog.hash))return false;
    const wwmi::program::Layout legacy_layout;
    const auto& fields=contract?contract->vertex->layout:legacy_layout;
    NoHistoryProgram derived{};
    auto* no_history=can_capture_fallback?no_history_program(list,prog.hash,contract,derived):nullptr;
    RtHistoryRoots history;
    bool material_layout=true;
    if(contract){
        const auto& plan=contract->bindings;
        view=plan.cb[0];primitive=plan.cb[1];local=plan.cb[2];out.streams=plan.streams;history=plan.history;
    }else{
        std::lock_guard lock(g_layout_regmap_mtx);
        auto i=g_layout_regmap.find(layout);if(i==g_layout_regmap.end())return false;
        view=i->second.b_vs[0];primitive=i->second.b_vs[1];local=i->second.b_vs[2];
        const int streams[]{prog.uv,prog.tangent,prog.aux,prog.previous};
        for(unsigned n=0;n<std::size(streams);++n){
            const int reg=streams[n];if(reg<0)continue;
            if(unsigned(reg)>=RegisterFileWidth(TargetClass::ShaderResource)){material_layout=false;continue;}
            const auto &b=i->second.t_vs[reg];
            if(b.param>=64||!b.is_table){material_layout=false;continue;}
            for(unsigned k=0;k<n;++k)if(out.streams[k].param==b.param&&out.streams[k].offset==b.offset)material_layout=false;
            out.streams[n]={b.param,b.offset};
        }
        if(no_history){
            auto h=rt_history_roots.find(layout);if(h!=rt_history_roots.end())history=h->second;
        }
    }
    if(view.param>=64||primitive.param>=64||local.param>=64||
       view.is_table||primitive.is_table||local.is_table)return false;
    const auto& graphics=CmdGfx(list);
    const bool unconsumed_candidate=prog.hash==0xaad17e90aee71ee7ULL||prog.hash==0x23830857e27f2dcdULL;
    const bool fallback_slot_proven=can_capture_fallback&&(no_history||unconsumed_candidate)&&material_layout&&
        graphics.gfx_pso&&graphics.gfx_restore()==graphics.gfx_pso&&
        (contract?contract->bindings.previous_vertex_only:vertex_only_previous(layout,prog.previous,out.streams[3]))&&
        (contract?contract->only_vertex_pixel:RtOnlyVertexPixelStages(graphics.gfx_pso));
    if(!fallback_slot_proven)no_history=nullptr;
    auto& cb=CmdCbv(list);
    const auto view_bind=cb.binds[view.param];const auto primitive_bind=cb.binds[primitive.param];
    if(contract&&(view_bind.size<fields.view_read_end||primitive_bind.size<fields.primitive_read_end))return false;
    const bool has_ps=no_history&&no_history->ps_primitive>=0;
    const uint8_t ps_primitive=has_ps?history.ps[no_history->ps_primitive]:0xff;
    const bool snapshot_vs=no_history && (history.vs_only&(1ull<<view.param)) && (history.vs_only&(1ull<<primitive.param)) &&
        (!has_ps||(history.ps[0]<64&&ps_primitive<64));
    uint32_t flag=0,view_velocity=0;auto& scope=out.previous_scope;
    const bool has_previous=prog.previous>=0;
    const bool unconditional_previous=has_previous&&fields.view_gate_offset==UINT32_MAX;
    const uint32_t view_gate_offset=unconditional_previous?2560u:fields.view_gate_offset;
    // Validate native gates and scope fields with minimal WC upload reads.
    // Full 13.3 KB history-invalid CB bytes are deferred lazily to
    // RtPrepareHistoryInvalid (only when previous_missing actually fires).
    bool view_read=false;
    bool vs_history_ready=false;
    WWMI_RtAuthorInstance_v1 staged_instance{};
    bool have_staged_instance=false;
    decltype(out.previous_scope) staged_scope{};
    if(!read_upload(primitive_bind,instance?0u:capture_previous_scope?16u*10:fields.primitive_gate_offset,fields.primitive_gate_offset+4,[&](const auto* p,uint64_t origin,bool primitive_full) -> bool {
           const unsigned char* prim_src=p;
           if(instance&&primitive_full){
               if(!copy_author_instance(primitive_bind,p,origin,staged_instance))return false;
               have_staged_instance=true;
               prim_src=reinterpret_cast<const unsigned char*>(staged_instance.primitive_words);
           }
           if(prim_src!=p && fields.primitive_gate_offset+4<=WWMI_RT_NH_PRIMITIVE_BYTES)
               memcpy(&flag,prim_src+fields.primitive_gate_offset,4);
           else if(!wwmi::safe_copy_from_wc(&flag,p+fields.primitive_gate_offset,4))
               return false;
           if(capture_previous_scope){
               staged_scope.primitive_resource=reinterpret_cast<uint64_t>(primitive_bind.res);
               staged_scope.primitive_byte_origin=origin;
               // 16*(10..13)+12 = 220 <= WWMI_RT_NH_PRIMITIVE_BYTES (720)
               if(prim_src!=p){
                   for(unsigned i=0;i<4;++i)memcpy(staged_scope.primitive_words+3*i,prim_src+16*(10+i),12);
               }else{
                   for(unsigned i=0;i<4;++i)
                       if(!wwmi::safe_copy_from_wc(staged_scope.primitive_words+3*i,p+16*(10+i),12))return false;
               }
               staged_scope.primitive_words[12]=flag;
           }
           if(!has_previous){view_read=true;return true;}
           view_read=read_upload(view_bind,capture_previous_scope?16*95:view_gate_offset,view_gate_offset+4,[&](const auto* v,uint64_t view_origin,bool view_full) -> bool {
               if(!wwmi::safe_copy_from_wc(&view_velocity,v+view_gate_offset,4))return false;
               if(capture_previous_scope){
                   staged_scope.view_resource=reinterpret_cast<uint64_t>(view_bind.res);
                   staged_scope.view_byte_origin=view_origin;
                   if(!wwmi::safe_copy_from_wc(staged_scope.view_words,v+16*95,64)||
                      !wwmi::safe_copy_from_wc(staged_scope.view_words+16,v+16*117,12))return false;
                   staged_scope.view_words[19]=view_velocity;
               }
               if(unconditional_previous||!primitive_full||!view_full||!((flag&fields.primitive_gate_mask)|view_velocity))return true;
               vs_history_ready=true;
               return true;
           },snapshot_vs?WWMI_RT_NH_VIEW_BYTES:0,false);
           return view_read;
       },(instance||snapshot_vs)?WWMI_RT_NH_PRIMITIVE_BYTES:0,instance!=nullptr)||!view_read||
       !upload_bytes(cb.binds[local.param],0,out.local.data(),24))return false;
    if(have_staged_instance&&instance)*instance=staged_instance;
    if(capture_previous_scope)scope=staged_scope;
    out.native_cb=cb.binds[local.param].va;out.local_param=local.param;
    out.native_layout=layout;out.native_pso=CmdGfx(list).gfx_pso;
    out.reads_previous=has_previous&&(unconditional_previous||((flag&fields.primitive_gate_mask)|view_velocity)!=0);
    const bool aux_ok=(prog.aux<0||RtResolveAuxColor(list,prog,guid,hash,component,out))&&material_layout;
    if(!out.reads_previous)return aux_ok;
    if(capture_previous_scope){
        scope.pass_generation=CmdRt(list).pass_gen;
        out.previous_cb_holds[0]=view_bind.res;out.previous_cb_holds[1]=primitive_bind.res;
    }
    if(vs_history_ready){
        out.previous_cb_holds[0]=view_bind.res;out.previous_cb_holds[1]=primitive_bind.res;
        out.history_roots[WWMI_RT_NH_VS_VIEW]={view_bind.va,view.param};
        out.history_roots[WWMI_RT_NH_VS_PRIMITIVE]={primitive_bind.va,primitive.param};
        bool history_ok=true;
        if(has_ps){
            const auto ps_v=cb.binds[history.ps[0]];
            const auto ps_p=cb.binds[ps_primitive];
            const bool ps_v_ok=(ps_v.res==view_bind.res && ps_v.va==view_bind.va && ps_v.size>=WWMI_RT_NH_VIEW_BYTES) ||
                               check_upload_window(ps_v,WWMI_RT_NH_VIEW_BYTES);
            const bool ps_p_ok=(ps_p.res==primitive_bind.res && ps_p.va==primitive_bind.va && ps_p.size>=WWMI_RT_NH_PRIMITIVE_BYTES) ||
                               check_upload_window(ps_p,WWMI_RT_NH_PRIMITIVE_BYTES);
            if(ps_v_ok && ps_p_ok){
                out.history_roots[WWMI_RT_NH_PS_VIEW]={ps_v.va,history.ps[0]};
                out.history_roots[WWMI_RT_NH_PS_PRIMITIVE]={ps_p.va,ps_primitive};
                out.history_ps_holds[0]=ps_v.res;
                out.history_ps_holds[1]=ps_p.res;
            }else{
                history_ok=false;
                out.history_roots={};
                out.history_ps_holds={};
            }
        }
        if(history_ok){
            out.history_layout=layout;out.history_pso=CmdGfx(list).gfx_pso;
            out.history_fallback=RtHistoryFallback::native_gate;
        }
    }
    if(fallback_slot_proven&&unconsumed_candidate&&history_varying_unused(list,prog.hash,layout,out.native_pso))
        out.history_fallback=RtHistoryFallback::unused_varying;
    const auto &previous=out.streams[3];if(previous.param>=64)return aux_ok;
    auto* dev=list->get_device();
    auto start=TableStartOf(dev,CmdGfx(list).root[previous.param].table);
    if(!start || previous.offset>UINT32_MAX-start.base)return aux_ok;
    RtPreviousView copy{};
    const auto resolution=RtReadPreviousView(dev,start.heap,start.base+previous.offset,copy);
    out.previous_resolution=unsigned(resolution);
    if(resolution==RtPreviousReadResult::copied || resolution==RtPreviousReadResult::source_same_copy){
        out.prev_resource=copy.resource;out.prev_offset=copy.byte_offset;out.prev_bytes=copy.bytes;
    }
    return aux_ok;
}
void RtAllowMaskedVelocityFallback(const RtLocalProgram& program,RtWorldSurface& world){
    // In these admitted color-masked pairs the previous data only reaches
    // masked Target4, after discard. Leave real history preferred; only
    // the existing no-history continuation may bind a typed null here.
    if(world.reads_previous&&vertex_only_previous(world.native_layout,program.previous,world.streams[3]))
        world.history_fallback=RtHistoryFallback::unused_varying;
}
bool RtProveMaskedSiblingBindings(List* list,const RtLocalProgram& program,const RtWorldSurface& world,
                                  const std::array<uint64_t,2>& ps_reads){
    using namespace reshade::api;
    if(const auto* contract=world.contracts.get()){
        const auto& g=CmdGfx(list);
        if(!contract->bindings.masked_no_alias||contract->native_pso!=g.gfx_pso||
           contract->native_layout!=CmdGfxLayout(list)||contract->pixel->pixel_srv_reads!=ps_reads)return false;
        for(const auto param:contract->bindings.pixel_srv_parameters)if(!g.root[param].table.handle)return false;
        return true;
    }
    // No PS constant or texture can be aliased by the LocalVF roots/streams
    // that the private section changes. Other PS roots remain native.
    {
        std::lock_guard lock(g_layout_regmap_mtx);
        const auto roots=rt_history_roots.find(world.native_layout);
        if(world.local_param>=64||roots==rt_history_roots.end()||
           !(roots->second.vs_only&(1ull<<world.local_param)))return false;
    }
    std::lock_guard lock(rt_material_layout_mutex);
    const auto layout=rt_material_layouts.find(world.native_layout);
    if(layout==rt_material_layouts.end())return false;
    const auto &rows=layout->second;
    const int streams[]{program.uv,program.tangent,program.aux,program.previous};
    for(unsigned n=0;n<std::size(streams);++n)if(streams[n]>=0){
        const auto &s=world.streams[n];
        bool found=false;
        for(const auto &row:rows){
            const auto &r=row.desc;
            if(row.param!=s.param||s.offset<r.binding||s.offset-r.binding>=r.count)continue;
            if(found||r.type!=descriptor_type::shader_resource_view||r.visibility!=shader_stage::vertex||r.dx_register_space||
               uint64_t(r.dx_register_index)+s.offset-r.binding!=unsigned(streams[n]))return false;
            found=true;
        }
        if(!found)return false;
    }
    const auto &bound=CmdGfx(list).root;
    for(unsigned reg=0;reg<128;++reg)if(ps_reads[reg/64]&(1ull<<(reg%64))){
        bool found=false;
        for(const auto &row:rows){
            const auto &r=row.desc;
            if(!(unsigned(r.visibility)&unsigned(shader_stage::pixel))||r.dx_register_space||
               (r.type!=descriptor_type::shader_resource_view&&r.type!=descriptor_type::buffer_shader_resource_view)||
               reg<r.dx_register_index||reg-r.dx_register_index>=r.count)continue;
            if(found||row.param>=64||!bound[row.param].table.handle)return false;
            found=true;
        }
        if(!found)return false;
    }
    return true;
}
bool RtPrepareHistoryInvalid(List* list,const RtLocalProgram&,const RtWorldSurface& world,RtHistoryInvalid& out){
    const auto &g=CmdGfx(list);
    if(!world.reads_previous||!world.native_pso||g.gfx_pso!=world.native_pso||g.gfx_restore()!=g.gfx_pso||
       world.native_layout!=CmdGfxLayout(list))return false;
    if(world.history_fallback==RtHistoryFallback::unused_varying)return true;
    if(world.history_fallback!=RtHistoryFallback::native_gate)return false;
    if(world.history_layout!=world.native_layout||world.history_pso!=world.native_pso)return false;
    const auto* contract=world.contracts.get();
    const wwmi::program::Layout legacy_layout;
    // Patch copies of the original bytes, never the native upload.
    if(world.history_cbs){
        for(unsigned i=0;i<WWMI_RT_NH_CB_COUNT;++i){
            if(world.history_roots[i].param==0xff)continue; // the single-RT PS is untouched
            const auto& stage=world.history_cbs[i<WWMI_RT_NH_PS_VIEW?0:1];
            const bool primitive=(i==WWMI_RT_NH_VS_PRIMITIVE||i==WWMI_RT_NH_PS_PRIMITIVE);
            const auto* src=primitive?stage.primitive.data():stage.view.data();
            const auto length=primitive?stage.primitive.size():stage.view.size();
            const auto& fields=contract?(i<WWMI_RT_NH_PS_VIEW?contract->vertex->layout:contract->pixel->layout):legacy_layout;
            const unsigned offset=primitive?fields.primitive_gate_offset:fields.view_gate_offset;
            if(offset+sizeof(uint32_t)>length)return false;
            uint32_t word=0;
            if(primitive){memcpy(&word,src+offset,sizeof(word));word&=~fields.primitive_gate_mask;}
            out.cb[i].assign(src,src+length);
            memcpy(out.cb[i].data()+offset,&word,sizeof(word));
        }
        return true;
    }
    // Lazy capture path: read the held native upload CBs on demand only when
    // previous_missing actually fires, deduplicating identical VS/PS ranges.
    std::vector<unsigned char> vs_view_raw,vs_prim_raw;
    ID3D12Resource* vs_view_res=nullptr; uint64_t vs_view_va=0;
    ID3D12Resource* vs_prim_res=nullptr; uint64_t vs_prim_va=0;
    for(unsigned i=0;i<WWMI_RT_NH_CB_COUNT;++i){
        if(world.history_roots[i].param==0xff)continue;
        const bool primitive=(i==WWMI_RT_NH_VS_PRIMITIVE||i==WWMI_RT_NH_PS_PRIMITIVE);
        const uint32_t length=primitive?WWMI_RT_NH_PRIMITIVE_BYTES:WWMI_RT_NH_VIEW_BYTES;
        const auto& fields=contract?(i<WWMI_RT_NH_PS_VIEW?contract->vertex->layout:contract->pixel->layout):legacy_layout;
        const unsigned offset=primitive?fields.primitive_gate_offset:fields.view_gate_offset;
        if(offset+sizeof(uint32_t)>length)return false;
        ID3D12Resource* res=nullptr;
        switch(i){
            case WWMI_RT_NH_VS_VIEW:      res=world.previous_cb_holds[0].Get(); break;
            case WWMI_RT_NH_VS_PRIMITIVE: res=world.previous_cb_holds[1].Get(); break;
            case WWMI_RT_NH_PS_VIEW:      res=world.history_ps_holds[0].Get(); break;
            case WWMI_RT_NH_PS_PRIMITIVE: res=world.history_ps_holds[1].Get(); break;
        }
        const uint64_t va=world.history_roots[i].va;
        if(!res||!va)return false;
        if(i==WWMI_RT_NH_PS_VIEW&&res==vs_view_res&&va==vs_view_va&&vs_view_raw.size()==length){
            out.cb[i]=vs_view_raw;
        }else if(i==WWMI_RT_NH_PS_PRIMITIVE&&res==vs_prim_res&&va==vs_prim_va&&vs_prim_raw.size()==length){
            out.cb[i]=vs_prim_raw;
        }else{
            out.cb[i].resize(length);
            RootCbvBind bind{};bind.res=res;bind.va=va;bind.size=length;
            if(!upload_bytes(bind,0,out.cb[i].data(),length)){out.cb[i].clear();return false;}
            if(i==WWMI_RT_NH_VS_VIEW&&world.history_roots[WWMI_RT_NH_PS_VIEW].param!=0xff&&
               world.history_ps_holds[0].Get()==res&&world.history_roots[WWMI_RT_NH_PS_VIEW].va==va){
                vs_view_raw=out.cb[i];vs_view_res=res;vs_view_va=va;
            }else if(i==WWMI_RT_NH_VS_PRIMITIVE&&world.history_roots[WWMI_RT_NH_PS_PRIMITIVE].param!=0xff&&
                     world.history_ps_holds[1].Get()==res&&world.history_roots[WWMI_RT_NH_PS_PRIMITIVE].va==va){
                vs_prim_raw=out.cb[i];vs_prim_res=res;vs_prim_va=va;
            }
        }
        uint32_t word=0;
        if(primitive){memcpy(&word,out.cb[i].data()+offset,sizeof(word));word&=~fields.primitive_gate_mask;}
        memcpy(out.cb[i].data()+offset,&word,sizeof(word));
    }
    return true;
}
#include "rt_material_draw_note.hpp"
static bool RtPushVsOverride(const RtWorldSurface::StreamSlot& rb,D3D12_CPU_DESCRIPTOR_HANDLE src,std::vector<ShadowSlotOvr>& out){
    if(rb.param>=64||!src.ptr)return false;
    // CopyDescriptors applies the vector in order: explicit PS wins over
    // RFX. Check the exact view of the final write, not its resource.
    for(auto old=out.rbegin();old!=out.rend();++old)
        if(old->param==rb.param && old->offset==rb.offset)return old->src.ptr==src.ptr;
    out.push_back({static_cast<uint8_t>(rb.param),rb.offset,src});return true;
}

bool RtPrepareWorldMaterialInputs(List* list,const RtLocalProgram& p,const RtWorldSurface& world,
    const WWMI_RtLocalVfSurface_v1& surface,const WWMI_RtPreviousSurface_v1& previous,bool no_history,
    RtWorldMaterialInputs& inputs,RtMaterialTables& target,bool shared_window){
    if(!world.native_layout||world.native_layout!=CmdGfxLayout(list)){target.failure=6;return false;}
    // Only CPU scratch descriptors here. Each one is copied synchronously to
    // a disjoint range owned by the actual graphics recording and its flights.
    static std::mutex mx;
    static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cpu;
    static ID3D12Device *cpu_device=nullptr;
    static D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    static UINT cpu_inc=0;
    inputs.scratch=std::unique_lock(mx);
    if(!cpu||cpu_device!=g_d3d12_device){D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        if(FAILED(g_d3d12_device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(cpu.ReleaseAndGetAddressOf())))){target.failure=9;return false;}
        cpu_device=g_d3d12_device;
        cpu_start=cpu->GetCPUDescriptorHandleForHeapStart();
        cpu_inc=g_d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);}
    auto handle=cpu_start;
    const auto inc=cpu_inc;
    const auto stream=[&](unsigned slot,D3D12_CPU_DESCRIPTOR_HANDLE src){
        const auto &b=world.streams[slot];
        if(b.param>=64||!src.ptr)return false;
        inputs.streams[inputs.count++]={{static_cast<uint8_t>(b.param),b.offset,src},slot+1};
        return true;
    };
    auto add=[&](unsigned slot,int reg,const WWMI_RtLocalVfView_v1& v,DXGI_FORMAT fmt,bool required=true,uint32_t mapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING){
        if(reg<0)return true;
        if(required && (!v.resource || v.format!=unsigned(fmt) || !v.element_stride || !v.element_count || v.byte_offset%v.element_stride))return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=fmt;d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping=mapping;
        d.Buffer.FirstElement=required?v.byte_offset/v.element_stride:0;d.Buffer.NumElements=required?v.element_count:1;
        // A real null typed descriptor is valid when the shader does not read
        // the buffer, or a proven program/PSO cannot observe the previous result.
        g_d3d12_device->CreateShaderResourceView(required?reinterpret_cast<ID3D12Resource*>(v.resource):nullptr,&d,handle);
        RtNoteLocalVfView(slot,handle.ptr,required?v.resource:0);
        auto current=handle;handle.ptr+=inc;return stream(slot,current);
    };
    if(!add(0,p.uv,surface.uv,DXGI_FORMAT_R16G16_FLOAT)){target.failure=1;return false;}
    if(!add(1,p.tangent,surface.tangents,DXGI_FORMAT_R32G32B32A32_FLOAT)){target.failure=2;return false;}
    if(world.aux_constant){
        const auto& v=world.aux_source.view;
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=(DXGI_FORMAT)v.format;d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping=v.mapping;d.Buffer.FirstElement=v.first;d.Buffer.NumElements=v.count;
        d.Buffer.StructureByteStride=v.stride;d.Buffer.Flags=(D3D12_BUFFER_SRV_FLAGS)v.flags;
        g_d3d12_device->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(v.resource),&d,handle);
        RtNoteLocalVfView(2,handle.ptr,v.resource);
        auto src=handle;handle.ptr+=inc;
        if(!stream(2,src)){target.failure=3;return false;}
    }else if(!add(2,p.aux,surface.color,DXGI_FORMAT_R8G8B8A8_UNORM,true,
                 world.aux_source.view.resource?world.aux_source.view.mapping:D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)){target.failure=3;return false;}
    if(!add(3,p.previous,previous.positions,DXGI_FORMAT_R32_FLOAT,world.reads_previous&&!no_history)){target.failure=4;return false;}
    if(shared_window){
        inputs.window=RtReadRecordedMaterialWindow(list);
        if(!inputs.window){target.failure=6;return false;}
    }
    return true;
}
bool RtPrepareWorldMaterialPart(List* list,const RtWorldSurface& world,const WWMI_RtLocalVfSurface_v1& surface,
    const RtWorldMaterialInputs& inputs,const std::vector<ShadowSlotOvr>& ps,
    const std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& textures,WWMI_RtPackReserveDrawBindingFn_v1 reserve,
    RtMaterialTables& target,uint64_t& cb,WWMI_RtPackReserveDrawBindingFn_v2 reserve2,RtHistoryInvalid* no_history){
    std::vector<ShadowSlotOvr> all;
    all.reserve(ps.size()+inputs.count);
    all.assign(ps.begin(),ps.end());
    for(unsigned i=0;i<inputs.count;++i){
        const auto &entry=inputs.streams[i];const auto &src=entry.entry;
        if(!RtPushVsOverride({src.param,src.offset},src.src,all)){
            target.failure=entry.failure;
            if(rt_material_copy_note){
                rt_material_copy_note->facts->vs_alias_conflict=true;
                target.missing_param=src.param;target.missing_offset=src.offset;
            }
            return false;
        }
    }
    uint64_t resources[128];
    if(textures.size()>std::size(resources)){target.failure=10;return false;}
    for(size_t i=0;i<textures.size();++i)resources[i]=reinterpret_cast<uint64_t>(textures[i].Get());
    const auto &g=surface.geometry;
    RtRecordedMaterial recorded{};recorded.reserve=reserve;auto &r=recorded.request.base;
    if(no_history){
        recorded.reserve2=reserve2;
        for(unsigned i=0;i<WWMI_RT_NH_CB_COUNT;++i)
            if(!no_history->cb[i].empty())recorded.request.no_history_cb[i]=no_history->cb[i].data();
    }
    r.struct_size=sizeof(r);r.abi_version=1;r.resource_count=unsigned(textures.size());r.resources=resources;
    r.device=list->get_device()->get_native();r.list=g.consumer_list;r.list_creation=g.consumer_list_creation;
    r.recording=g.consumer_recording;r.batch_token=g.batch_token;
    memcpy(r.local_vf,world.local.data(),sizeof(r.local_vf));
    // All per-vertex streams use the packed Mod's absolute vertex indices.
    r.local_vf[0]=world.aux_constant?0u:UINT32_MAX;r.local_vf[1]=surface.uv_pairs;r.local_vf[3]=0;
    if(inputs.window){
        if(!RtPrepareRecordedMaterialTablesInWindow(list,*inputs.window,all,recorded,target))return false;
    }else if(!RtPrepareRecordedMaterialTables(list,all,recorded,target))return false;
    cb=recorded.backing.base.local_vf_va;
    if(no_history)for(unsigned i=0;i<WWMI_RT_NH_CB_COUNT;++i)no_history->va[i]=recorded.backing.no_history_va[i];
    return true;
}
bool RtPrepareWorldMaterial(List* list,const RtLocalProgram& p,const RtWorldSurface& world,
    const WWMI_RtLocalVfSurface_v1& surface,const WWMI_RtPreviousSurface_v1& previous,
    const std::vector<ShadowSlotOvr>& ps,const std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& textures,
    WWMI_RtPackReserveDrawBindingFn_v1 reserve,RtMaterialTables& target,uint64_t& cb,
    WWMI_RtPackReserveDrawBindingFn_v2 reserve2,RtHistoryInvalid* no_history){
    RtWorldMaterialInputs inputs;
    return RtPrepareWorldMaterialInputs(list,p,world,surface,previous,no_history!=nullptr,inputs,target)&&
        RtPrepareWorldMaterialPart(list,world,surface,inputs,ps,textures,reserve,target,cb,reserve2,no_history);
}
