#include "addon_common.h"
#include "config.h"
#include "rt_program_runtime.h"
#include "rt_program_admission.hpp"
#include "rt_material_layout.h"
#include "rabbitfx/rfx_dxil.h"
#include "hooks/pipeline_hooks.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>

// The existing RabbitFX DXC connection is reused read-only. No invocation of
// a compiler, game process, filesystem cache or shader patcher occurs on draw.
bool RfxDxilDisassemble(const void*,size_t,std::string&,std::string&);
namespace {
wwmi::program::ContentCache content;
std::atomic<uint64_t> creation{0};
bool disassemble(const void* code,size_t size,std::string& text,std::string& why){
    if(!RfxDxilInit((g_cso_dir+L"\\dxc").c_str(),why))return false;
    return RfxDxilDisassemble(code,size,text,why);
}
void RtLogProgramReject(const char* stage,uint64_t vs_hash,uint64_t ps_hash,uint32_t targets,const std::string& reason){
    const int sk=stage[0]=='v'?0:(stage[0]=='p'&&stage[1]=='i'?1:2);
    // rtv=0 depth/shadow PSOs are expected to skip the color pair contract; don't crowd out real rtv>=1 rejections.
    if(sk==2&&targets==0)return;
    static std::mutex s_mtx;
    static std::set<std::tuple<uint64_t,uint64_t,uint32_t>> s_seen[3];
    std::lock_guard lk(s_mtx);
    auto& seen=s_seen[sk];
    if(seen.size()>=64||!seen.emplace(vs_hash,ps_hash,targets).second)return;
    logger::probe("RT-PROG-REJECT stage=%s vs=%016llx ps=%016llx rtv=%u reason=\"%s\"",
        stage,(unsigned long long)vs_hash,(unsigned long long)ps_hash,unsigned(targets),reason.c_str());
}
bool bindings(RtProgramPsoContract& out,RtProgramBuildMark& mark){
    const auto refuse=[&](RtProgramBuildIssue issue,unsigned role=0xff,unsigned reg=0xff){
        mark.issue=issue;mark.role=uint8_t(role);mark.reg=uint8_t(reg);return false;
    };
    const auto& l=out.vertex->layout;
    const int streams[]{out.local.uv,out.local.tangent,out.local.aux,out.local.previous};
    auto& plan=out.bindings;
    {
    std::lock_guard lock(g_layout_regmap_mtx);
    const auto map=g_layout_regmap.find(out.native_layout);
    if(map==g_layout_regmap.end())return refuse(RtProgramBuildIssue::root_map);
    const uint32_t registers[]{l.view_reg,l.primitive_reg,l.local_reg};
    for(unsigned n=0;n<3;++n){
        if(registers[n]>=RegisterFileWidth(TargetClass::ConstantBuffer))return refuse(RtProgramBuildIssue::cb_register,n,registers[n]);
        plan.cb[n]=map->second.b_vs[registers[n]];
        if(plan.cb[n].param>=64||plan.cb[n].is_table)return refuse(RtProgramBuildIssue::cb_slot,n,registers[n]);
        for(unsigned k=0;k<n;++k)if(plan.cb[n].param==plan.cb[k].param)return refuse(RtProgramBuildIssue::cb_alias,n,registers[n]);
    }
    for(unsigned n=0;n<4;++n){
        if(streams[n]<0)continue;
        if(unsigned(streams[n])>=RegisterFileWidth(TargetClass::ShaderResource))return refuse(RtProgramBuildIssue::stream_register,n,streams[n]);
        const auto b=map->second.t_vs[streams[n]];
        if(b.param>=64||!b.is_table)return refuse(RtProgramBuildIssue::stream_slot,n,streams[n]);
        for(unsigned k=0;k<n;++k)if(plan.streams[k].param==b.param&&plan.streams[k].offset==b.offset)return refuse(RtProgramBuildIssue::stream_alias,n,streams[n]);
        plan.streams[n]={b.param,b.offset};
    }
    const auto history=rt_history_roots.find(out.native_layout);
    if(history!=rt_history_roots.end())plan.history=history->second;
    if(out.capabilities.no_history&&out.capabilities.pixel_primitive>=0&&
       (unsigned(out.capabilities.pixel_primitive)>=std::size(plan.history.ps)||
        plan.history.ps[0]>=64||plan.history.ps[out.capabilities.pixel_primitive]>=64))
        out.capabilities.no_history=false; // normal current/previous is independent
    }
    {
        std::lock_guard lock(rt_material_layout_mutex);
        const auto found=rt_material_layouts.find(out.native_layout);
        if(found!=rt_material_layouts.end()){
            using namespace reshade::api;
            bool stream_slots=true;
            for(unsigned n=0;n<4;++n)if(streams[n]>=0){
                const auto& b=plan.streams[n];bool present=false,valid=true;
                for(const auto& range:found->second){
                    const auto& r=range.desc;
                    if(range.param!=b.param||b.offset<r.binding||b.offset-r.binding>=r.count)continue;
                    valid&=!present&&r.type==descriptor_type::shader_resource_view&&r.visibility==shader_stage::vertex&&
                        !r.dx_register_space&&uint64_t(r.dx_register_index)+b.offset-r.binding==unsigned(streams[n]);
                    present=true;
                }
                stream_slots&=present&&valid;
                if(n==3)plan.previous_vertex_only=present&&valid;
            }
            bool pixel_slots=true;
            for(unsigned reg=0;reg<128;++reg)if(out.pixel->pixel_srv_reads[reg/64]&(1ull<<(reg%64))){
                unsigned matches=0,param=UINT32_MAX;
                for(const auto& range:found->second){
                    const auto& r=range.desc;
                    if(!(unsigned(r.visibility)&unsigned(shader_stage::pixel))||r.dx_register_space||
                       (r.type!=descriptor_type::shader_resource_view&&r.type!=descriptor_type::buffer_shader_resource_view)||
                       reg<r.dx_register_index||reg-r.dx_register_index>=r.count)continue;
                    ++matches;param=range.param;
                }
                if(matches!=1||param>=64){pixel_slots=false;break;}
                if(std::find(plan.pixel_srv_parameters.begin(),plan.pixel_srv_parameters.end(),param)==plan.pixel_srv_parameters.end())
                    plan.pixel_srv_parameters.push_back(uint8_t(param));
            }
            plan.masked_no_alias=stream_slots&&pixel_slots&&(plan.history.vs_only&(1ull<<plan.cb[2].param));
        }
    }
    plan.valid=true;return true;
}
}
wwmi::program::CacheStats RtProgramContentCacheStats(){return content.stats();}
uint64_t RtNextProgramCreation(){return ++creation;}
uint16_t RtProgramSdkPresence(uint64_t layout,uint32_t count,const reshade::api::pipeline_subobject* subs){
    using T=reshade::api::pipeline_subobject_type;
    uint16_t bits=layout?rt_sdk_layout:0;
    for(uint32_t n=0;subs&&n<count;++n)switch(subs[n].type){
        case T::vertex_shader:bits|=rt_sdk_vs;break;
        case T::pixel_shader:bits|=rt_sdk_ps;break;
        case T::geometry_shader:case T::hull_shader:case T::domain_shader:case T::mesh_shader:
        case T::amplification_shader:bits|=rt_sdk_other_stage|rt_sdk_graphics_other;break;
        case T::compute_shader:bits|=rt_sdk_other_stage;break;
        case T::input_layout:case T::rasterizer_state:case T::primitive_topology:
        case T::depth_stencil_format:case T::viewport_count:bits|=rt_sdk_graphics_other;break;
        case T::render_target_formats:bits|=rt_sdk_targets;break;
        case T::blend_state:bits|=rt_sdk_blend;break;
        case T::depth_stencil_state:bits|=rt_sdk_depth;break;
        case T::sample_count:bits|=rt_sdk_samples;break;
        case T::sample_mask:bits|=rt_sdk_sample_mask;break;
        case T::stream_output_state:bits|=rt_sdk_stream;break;
        case T::dynamic_pipeline_states:bits|=rt_sdk_dynamic;break;
        default:break;
    }
    return bits;
}
std::shared_ptr<const RtProgramPsoContract> RtBuildObservedProgramPsoContract(uint64_t pso,uint64_t layout,uint32_t count,
    const reshade::api::pipeline_subobject* subs,RtProgramBuildMark& mark){
    using namespace reshade::api;using T=pipeline_subobject_type;
    auto result=std::make_shared<RtProgramPsoContract>();auto& out=*result;
    out.creation=RtNextProgramCreation();out.native_pso=pso;out.native_layout=layout;
    mark={RtProgramSdkPresence(layout,count,subs),RtProgramBuildPhase::sdk_metadata};
    const auto refuse=[&](RtProgramBuildIssue issue){mark.issue=issue;return result;};
    if(!pso||!layout||!count||!subs)return refuse(!layout?RtProgramBuildIssue::missing_layout:RtProgramBuildIssue::invalid_description);
    bool targets=false,blend=false,depth=false,samples=false,mask=false,unknown_stage=false,stream=false,dynamic=false;
    const shader_desc* native_vertex=nullptr;const shader_desc* native_pixel=nullptr;
    for(uint32_t n=0;n<count;++n){
        const auto& s=subs[n];
        if(s.type==T::vertex_shader||s.type==T::pixel_shader){
            if(s.count!=1||!s.data)return refuse(RtProgramBuildIssue::shader_description);
            const auto& d=*static_cast<const shader_desc*>(s.data);
            if(!d.code||!d.code_size)return refuse(RtProgramBuildIssue::shader_description);
            auto& slot=s.type==T::vertex_shader?native_vertex:native_pixel;if(slot)return refuse(RtProgramBuildIssue::shader_description);slot=&d;
        }else if(s.type==T::geometry_shader||s.type==T::hull_shader||s.type==T::domain_shader||s.type==T::mesh_shader||
                 s.type==T::amplification_shader||s.type==T::compute_shader){
            if(s.count&&!s.data)return refuse(RtProgramBuildIssue::shader_description);
            const auto* d=static_cast<const shader_desc*>(s.data);
            for(uint32_t j=0;j<s.count;++j)unknown_stage|=d[j].code_size!=0;
        }else if(s.type==T::render_target_formats){
            if(targets||s.count>8||(s.count&&!s.data))return refuse(RtProgramBuildIssue::malformed_state);targets=true;out.target_count=s.count;
            const auto* f=static_cast<const format*>(s.data);for(uint32_t j=0;j<s.count;++j)out.target_formats[j]=uint32_t(f[j]);
        }else if(s.type==T::sample_count){
            if(samples||s.count!=1||!s.data)return refuse(RtProgramBuildIssue::malformed_state);samples=true;out.sample_count=*static_cast<const uint32_t*>(s.data);
        }else if(s.type==T::sample_mask){if(mask||s.count!=1||!s.data)return refuse(RtProgramBuildIssue::malformed_state);mask=true;
        }else if(s.type==T::blend_state){if(blend||s.count!=1||!s.data)return refuse(RtProgramBuildIssue::malformed_state);blend=true;
        }else if(s.type==T::depth_stencil_state){if(depth||s.count!=1||!s.data)return refuse(RtProgramBuildIssue::malformed_state);depth=true;
        }else if(s.type==T::stream_output_state){stream|=s.count!=0;
        }else if(s.type==T::dynamic_pipeline_states){dynamic|=s.count!=0;}
    }
    out.only_vertex_pixel=native_vertex&&native_pixel&&!unknown_stage;
    // The full immutable native clone still owns every actual state value.
    // This readiness bit never substitutes for the existing PSO/OM effects
    // guard. Layout, RT/DS, sample, depth/stencil, blend and dynamic states
    // remain attached to this PSO creation, not to a shader-only tuple cache.
    out.fixed_native_state=targets&&blend&&depth&&samples&&mask&&!stream&&!dynamic;
    if(!out.only_vertex_pixel||!targets)return refuse(!native_vertex||!native_pixel?RtProgramBuildIssue::missing_stage:
        unknown_stage?RtProgramBuildIssue::other_stage:RtProgramBuildIssue::targets_unreported);
    mark.phase=RtProgramBuildPhase::vertex;
    out.vertex=content.resolve(native_vertex->code,native_vertex->code_size,HashShader(native_vertex->code,native_vertex->code_size),
        wwmi::program::Stage::vertex,disassemble);
    if(out.vertex->recognition!=wwmi::program::Recognition::executable_model){
        out.capabilities.reason=out.vertex->reason;
        if(out.vertex->reason.compare(0,3,"VS ")==0)
            RtLogProgramReject("vertex",out.vertex->native_identity,0,out.target_count,out.vertex->reason);
        return result;
    }
    // An unrelated native VS never causes its material PS to be disassembled.
    out.pixel=content.resolve(native_pixel->code,native_pixel->code_size,HashShader(native_pixel->code,native_pixel->code_size),
        wwmi::program::Stage::pixel,disassemble);
    out.capabilities=wwmi::program::pair(*out.vertex,*out.pixel,out.target_count);
    mark.phase=out.pixel->recognition==wwmi::program::Recognition::native_pixel_interface?
        RtProgramBuildPhase::pair:RtProgramBuildPhase::pixel;
    if(!out.capabilities.current_previous){
        const bool ps_fail=out.pixel->recognition!=wwmi::program::Recognition::native_pixel_interface;
        RtLogProgramReject(ps_fail?"pixel":"pair",out.vertex->native_identity,out.pixel->native_identity,out.target_count,
            ps_fail?out.pixel->reason:out.capabilities.reason);
        return result;
    }
    const auto& v=*out.vertex;
    out.local={v.native_identity,v.uv,v.tangent,v.auxiliary,v.previous,v.auxiliary_requires_native_color};
    mark.phase=RtProgramBuildPhase::bindings;
    if(!bindings(out,mark)){out.capabilities.current_previous=false;out.capabilities.no_history=false;out.capabilities.reason="actual native root binding plan is unsupported";return result;}
    constexpr uint32_t formats[]{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R16G16B16A16_UNORM,
        DXGI_FORMAT_R16_FLOAT,DXGI_FORMAT_B8G8R8A8_UNORM};
    const bool gbuffer_masked_domain=out.target_count==std::size(formats)&&
        std::equal(std::begin(formats),std::end(formats),out.target_formats.begin());
    const bool single_rt_masked_domain=out.target_count==1&&out.target_formats[0]!=DXGI_FORMAT_UNKNOWN;
    out.masked_material_domain=(gbuffer_masked_domain||single_rt_masked_domain)&&out.pixel->all_resource_reads_modelled;
    mark.phase=RtProgramBuildPhase::ready;
    return result;
}
std::shared_ptr<const RtProgramPsoContract> RtBuildProgramPsoContract(uint64_t pso,uint64_t layout,uint32_t count,
    const reshade::api::pipeline_subobject* subs){
    RtProgramBuildMark ignored;
    return RtBuildObservedProgramPsoContract(pso,layout,count,subs,ignored);
}
