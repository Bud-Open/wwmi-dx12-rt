// Use the actual cloned programs and input layout. A pass which obtains any
// other mesh input needs its own binding path, not another guess about slot 4.
#include "pso_world_base.inc"
#include "rt_native_no_pixel_proof.hpp"
#include <d3d12shader.h>
#include <wrl/client.h>
#include "hooks/dxil_psv.h"
#include <algorithm>
#include <span>
#include "rt_depth_route.h"
#include "rt_torso_ps.h"

static bool CopyHistoryPixel(const PsoClone& clone,uint64_t vs,uint64_t ps,RtHistoryPixelBlob& out)
{
    bool vertex=false;
    for(const auto& sub:clone.subs){
        if(sub.type==pipeline_subobject_type::vertex_shader||sub.type==pipeline_subobject_type::pixel_shader){
            if(!sub.data||sub.count!=1)return false;
            const auto& shader=*static_cast<const shader_desc*>(sub.data);
            if(!shader.code||!shader.code_size||shader.code_size>2u*1024u*1024u)return false;
            if(sub.type==pipeline_subobject_type::vertex_shader){
                if(vertex||HashShader(shader.code,shader.code_size)!=vs)return false;
                vertex=true;
            }else{
                if(out.bytes||HashShader(shader.code,shader.code_size)!=ps)return false;
                for(const auto& code:clone.code)
                    if(code->data()==shader.code&&code->size()==shader.code_size){out.bytes=code;break;}
                if(!out.bytes)return false;
            }
        }else if(sub.type==pipeline_subobject_type::render_target_formats && sub.data){
            if(sub.count>out.target_formats.size())return false;
            const auto* fm=static_cast<const format*>(sub.data);
            out.target_count=sub.count;
            for(unsigned i=0;i<sub.count;++i)out.target_formats[i]=uint32_t(fm[i]);
        }
    }
    return vertex&&bool(out.bytes);
}

bool RtCopyHistoryPixel(uint64_t pso,uint64_t layout,uint64_t vs,uint64_t ps,RtHistoryPixelBlob& out)
{
    std::lock_guard lock(g_pso_clone_mtx);
    const auto i=g_pso_clone.find(pso);
    if(i!=g_pso_clone.end())return i->second.layout.handle==layout&&CopyHistoryPixel(i->second,vs,ps,out);
    const auto proof=g_rt_effect_inputs.find(pso);
    return proof!=g_rt_effect_inputs.end()&&proof->second.layout.handle==layout&&CopyHistoryPixel(proof->second,vs,ps,out);
}

bool RtStrictPixelSrvRegs(const void*,size_t,DxilSrvRegs&);
bool RtReadProvenMaskedSibling(uint64_t pso,uint64_t layout,uint64_t vs,uint64_t ps,RtMaskedPixelReads& out){
    std::lock_guard lock(g_pso_clone_mtx);
    const auto i=g_pso_clone.find(pso);
    if(i==g_pso_clone.end()||i->second.layout.handle!=layout)return false;
    auto &proof=i->second.masked_sibling;
    if(!proof.checked){
        // Same native render-target domain as the two captured accepted
        // passes. The PSO, sample coverage and depth/stencil remain native.
        constexpr uint32_t formats[]{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R10G10B10A2_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R16G16B16A16_UNORM,
            DXGI_FORMAT_R16_FLOAT,DXGI_FORMAT_B8G8R8A8_UNORM};
        RtHistoryPixelBlob shader;
        DxilSrvRegs reads{};
        proof.valid=CopyHistoryPixel(i->second,vs,ps,shader)&&shader.target_count==std::size(formats)&&
            std::equal(std::begin(formats),std::end(formats),shader.target_formats.begin())&&
            RtStrictPixelSrvRegs(shader.bytes->data(),shader.bytes->size(),reads);
        if(proof.valid)proof.reads={reads.lo,reads.hi};
        proof.checked=true;
    }
    if(!proof.valid)return false;
    out=proof.reads;return true;
}

bool RtWriteHistoryCapture(const wchar_t* path,const void* bytes,uint32_t size)
{
    return std::strcmp(WriteSelectedShaderCapture(path,bytes,size),"written")==0;
}

bool RtPixelIgnoresPrevious(const RtHistoryPixelBlob& shader)
{
    // Input ISG1 elements are 32 bytes. Byte 24 is the declared mask,
    // byte 25 the input's actually used mask. The native C7 one-target
    // program declares VELOCITY_PREV_POS with a zero Used mask; the two
    // real GBuffer PS containers declare and use all four lanes.
    if(!shader.bytes)return false;
    const auto& b=*shader.bytes;
    const auto read=[](const uint8_t* p){uint32_t n;std::memcpy(&n,p,4);return n;};
    if(b.size()<32||std::memcmp(b.data(),"DXBC",4)||read(b.data()+24)!=b.size())return false;
    const auto chunks=read(b.data()+28);
    if(chunks>(b.size()-32)/4)return false;
    bool signature=false;
    for(uint32_t i=0;i<chunks;++i){
        const auto off=read(b.data()+32+4ull*i);
        if(off>b.size()||b.size()-off<8)return false;
        if(std::memcmp(b.data()+off,"ISG1",4))continue;
        if(signature)return false;
        signature=true;
        const auto bytes=read(b.data()+off+4);
        if(bytes<8||bytes>b.size()-off-8)return false;
        const auto* part=b.data()+off+8;
        const auto count=read(part),first=read(part+4);
        if(first<8||first>bytes||count>(bytes-first)/32)return false;
        for(uint32_t j=0;j<count;++j){
            const auto* element=part+first+32ull*j;
            const auto name=read(element+4);
            if(name>=bytes||!std::memchr(part+name,0,bytes-name))return false;
            if(!_stricmp(reinterpret_cast<const char*>(part+name),"VELOCITY_PREV_POS")&&element[25])return false;
        }
    }
    return signature;
}

bool RtKnownUnusedPreviousVarying(uint64_t pso,uint64_t layout,uint64_t vs,uint64_t ps,uint32_t targets)
{
    std::lock_guard lock(g_pso_clone_mtx);
    const auto i=g_pso_clone.find(pso);
    if(i==g_pso_clone.end()||i->second.layout.handle!=layout)return false;
    auto& proof=i->second.private_previous;
    if(proof.vs==vs&&proof.ps==ps)return proof.targets==targets&&proof.unused;
    RtHistoryPixelBlob shader;
    if(!CopyHistoryPixel(i->second,vs,ps,shader))return false;
    proof={vs,ps,shader.target_count,RtPixelIgnoresPrevious(shader)};
    return proof.targets==targets&&proof.unused;
}

bool RtPositionOnlyPass(uint64_t pipeline)
{
    std::lock_guard lock(g_pso_clone_mtx);
    const auto found=g_pso_clone.find(pipeline);
    if(found==g_pso_clone.end())return false;
    auto& cached=found->second.private_position_only;
    if(cached>=0)return cached!=0;
    const bool accepted=[&]() {
    bool vs_ok=false,has_position=false;
    for(auto &sub:found->second.subs){
        if(!sub.count || !sub.data)continue;
        if(sub.type==pipeline_subobject_type::input_layout){
            const auto *el=static_cast<const input_element*>(sub.data);
            for(unsigned i=0;i<sub.count;++i){
                if(el[i].semantic && !_stricmp(el[i].semantic,"ATTRIBUTE") && !el[i].semantic_index){
                    if(el[i].buffer_binding || el[i].offset || el[i].instance_step_rate ||
                       el[i].format!=reshade::api::format::r32g32b32_float)return false;
                    has_position=true;
                }
            }
        }else if(sub.type==pipeline_subobject_type::vertex_shader || sub.type==pipeline_subobject_type::pixel_shader){
            if(sub.count!=1)return false;
            auto &shader=*static_cast<const shader_desc*>(sub.data);if(!shader.code_size)continue;
            DxilShape shape;if(!DxilShaderShape(shader.code,shader.code_size,shape) ||
                shape.typed_srvs || shape.other_resources || shape.samplers)return false;
            // Direct heap indexing would bypass the PSV resource list.
            const auto* bytes=static_cast<const unsigned char*>(shader.code);
            uint32_t parts=0;memcpy(&parts,bytes+28,4);
            for(unsigned i=0;i<parts;++i){uint32_t off=0;memcpy(&off,bytes+32+4*i,4);
                if(off>shader.code_size || shader.code_size-off<8)return false;
                if(!memcmp(bytes+off,"SFI0",4)){uint64_t bits=0;if(shader.code_size-off<16)return false;
                    memcpy(&bits,bytes+off+8,8);if(bits&(D3D_SHADER_REQUIRES_RESOURCE_DESCRIPTOR_HEAP_INDEXING|D3D_SHADER_REQUIRES_SAMPLER_DESCRIPTOR_HEAP_INDEXING))return false;}}
            if(sub.type==pipeline_subobject_type::vertex_shader){
                if(shape.inputs.size()!=1 || _stricmp(shape.inputs[0].c_str(),"ATTRIBUTE0") ||
                   !std::any_of(shape.outputs.begin(),shape.outputs.end(),[](auto&s){return !_stricmp(s.c_str(),"SV_Position0");}))return false;
                vs_ok=true;
            }
        }else if(sub.type==pipeline_subobject_type::geometry_shader || sub.type==pipeline_subobject_type::hull_shader ||
                 sub.type==pipeline_subobject_type::domain_shader){
            if(static_cast<const shader_desc*>(sub.data)->code_size)return false;
        }
    }
    return vs_ok && has_position;
    }();cached=int(accepted);return accepted;
}

#include "dxil_cb.h"
int RtPrimitiveRegister(uint64_t pso)
{
    std::lock_guard lock(g_pso_clone_mtx);
    auto it=g_pso_clone.find(pso);if(it==g_pso_clone.end())return -1;
    auto& reg=it->second.private_primitive_register;
    if(reg!=-2)return reg;
    reg=-1;
    for(auto& s:it->second.subs){
        if(s.type!=pipeline_subobject_type::vertex_shader || !s.data || s.count!=1)continue;
        const auto& vs=*static_cast<const shader_desc*>(s.data);
        std::vector<DxilCbuf> cbs;
        if(!DxilCbuffers(vs.code,vs.code_size,cbs))return reg;
        for(auto& cb:cbs){
            if(cb.space || cb.count!=1 || cb.bytes!=720 ||
               (cb.name!="Primitive" && cb.type!="hostlayout.Primitive" && cb.type!="Primitive"))continue;
            if(reg!=-1){reg=-1;return reg;}
            reg=int(cb.reg);
        }
    }
    return reg;
}

bool RtNativeIaHasColor(uint64_t pipeline){
    std::lock_guard lk(g_pso_clone_mtx);auto i=g_pso_clone.find(pipeline);if(i==g_pso_clone.end())return false;
    auto& value=i->second.private_ia_color;if(value>=0)return value;value=0;
    for(auto& sub:i->second.subs)if(sub.type==pipeline_subobject_type::input_layout && sub.data){
        auto* el=static_cast<const input_element*>(sub.data);for(unsigned n=0;n<sub.count;n++)
            if(el[n].semantic && !_stricmp(el[n].semantic,"ATTRIBUTE") && el[n].semantic_index==3 &&
               el[n].buffer_binding==2 && !el[n].offset && !el[n].instance_step_rate &&
               el[n].format==reshade::api::format::b8g8r8a8_unorm){value=1;return true;}
    }return false;
}
static RtRasterEffects NativePsoEffects(std::span<const pipeline_subobject> subobjects,bool native_no_pixel=false)
{
    RtRasterEffects out;
    out.basis=RtColorBasis::InvalidState;
    const blend_desc *blend=nullptr;
    const depth_stencil_desc *depth=nullptr;
    RtRasterShaderFacts pixel;
    bool pixel_present=false, shader_unknown=false, shader_uav=false;
    bool vs=false, has_targets=false, stream=false, alpha_to_coverage_dynamic=false, color_dynamic=false;
    bool depth_dynamic=false, stencil_dynamic=false;
    bool has_sample_mask=false,sample_mask_restricts=false;
    uint8_t target_mask=0;
    for (const auto &s:subobjects) {
        using T=pipeline_subobject_type;
        switch(s.type) {
        case T::blend_state:
            if (blend || s.count!=1 || !s.data) return out;
            blend=static_cast<const blend_desc *>(s.data);
            break;
        case T::depth_stencil_state:
            if (depth || s.count!=1 || !s.data) return out;
            depth=static_cast<const depth_stencil_desc *>(s.data);
            break;
        case T::stream_output_state: stream=true; break;
        case T::sample_mask:
            if(has_sample_mask||s.count!=1||!s.data)return out;
            has_sample_mask=true;
            sample_mask_restricts=*static_cast<const uint32_t *>(s.data)!=UINT32_MAX;break;
        case T::render_target_formats: {
            if (has_targets || s.count>8 || (s.count&&!s.data)) return out;
            has_targets=true;target_mask=0;
            const auto *fm=static_cast<const format *>(s.data);
            for(unsigned n=0;n<s.count;++n)if(fm[n]!=format::unknown)target_mask|=uint8_t(1u<<n);
            break;
        }
        case T::dynamic_pipeline_states: {
            if(s.count&&!s.data)return out;
            const auto *p=static_cast<const dynamic_state *>(s.data);
            for (unsigned i=0;i<s.count;++i) {
                using D=dynamic_state;
                switch(p[i]) {
                case D::render_target_write_mask: case D::blend_enable: case D::logic_op_enable:
                case D::source_color_blend_factor: case D::dest_color_blend_factor:
                case D::source_alpha_blend_factor: case D::dest_alpha_blend_factor:
                    color_dynamic=true;break;
                case D::depth_enable: case D::depth_write_mask: depth_dynamic=true;break;
                case D::stencil_enable:
                case D::front_stencil_write_mask: case D::back_stencil_write_mask:
                case D::front_stencil_fail_op: case D::front_stencil_depth_fail_op: case D::front_stencil_pass_op:
                case D::back_stencil_fail_op: case D::back_stencil_depth_fail_op: case D::back_stencil_pass_op:
                    stencil_dynamic=true;break;
                case D::alpha_to_coverage_enable: alpha_to_coverage_dynamic=true;break;
                default:break;
                }
            }
            break;
        }
        case T::vertex_shader: case T::hull_shader: case T::domain_shader: case T::geometry_shader:
        case T::amplification_shader: case T::mesh_shader: case T::pixel_shader: {
            if (!s.count) break; // valid empty optional shader stage
            if (s.count!=1 || !s.data) return out;
            const auto &shader=*static_cast<const shader_desc *>(s.data);
            if (!shader.code&&!shader.code_size) break;
            if (!shader.code || !shader.code_size) return out;
            if (s.type==T::vertex_shader) {if(vs)return out;vs=true;}
            const bool is_pixel=s.type==T::pixel_shader;
            if (is_pixel && pixel_present) return out;
            RtRasterShaderFacts facts;
            RtReadRasterShaderFacts(shader.code,shader.code_size,is_pixel,facts);
            if (is_pixel){pixel_present=true;pixel=facts;}
            shader_uav|=facts.uav==RtEffect::Possible;
            shader_unknown|=facts.uav==RtEffect::Unknown;
            break;
        }
        default: break;
        }
    }
    if (!vs) return out;
    // The SDK stream-output payload contains only RasterizedStream. A
    // present state is a possible write, never proof of no declarations.
    out.stream_output=stream?RtEffect::Possible:RtEffect::Absent;
    out.uav=shader_uav?RtEffect::Possible:(shader_unknown?RtEffect::Unknown:RtEffect::Absent);
    out.pixel_depth=pixel.outputs_known&&pixel.depth;
    out.pixel_coverage=pixel.outputs_known&&pixel.coverage;
    out.pixel_stencil_ref=pixel.outputs_known&&pixel.stencil_ref;
    // Even a pixel shader with no SV_Coverage may discard. Metadata cannot
    // exclude it. Query/predication state is not provided by a PSO.
    if(pixel_present||sample_mask_restricts||(blend&&blend->alpha_to_coverage_enable))out.coverage=RtEffect::Possible;
    else if(native_no_pixel&&blend&&has_sample_mask&&!alpha_to_coverage_dynamic)out.coverage=RtEffect::Absent;
    if (depth) {
        if(!depth_dynamic) {
            out.depth_test=depth->depth_enable?RtEffect::Possible:RtEffect::Absent;
            out.depth_write=depth->depth_enable&&depth->depth_write_mask?RtEffect::Possible:RtEffect::Absent;
        }
        if(!stencil_dynamic) {
            const auto writes=[](uint8_t mask,stencil_op fail,stencil_op zfail,stencil_op pass){
                return mask&&(fail!=stencil_op::keep||zfail!=stencil_op::keep||pass!=stencil_op::keep);
            };
            out.stencil_test=depth->stencil_enable?RtEffect::Possible:RtEffect::Absent;
            out.stencil_write=depth->stencil_enable&&
                (writes(depth->front_stencil_write_mask,depth->front_stencil_fail_op,depth->front_stencil_depth_fail_op,depth->front_stencil_pass_op)||
                 writes(depth->back_stencil_write_mask,depth->back_stencil_fail_op,depth->back_stencil_depth_fail_op,depth->back_stencil_pass_op))?
                RtEffect::Possible:RtEffect::Absent;
        }
    }
    if (!pixel_present) {
        if(native_no_pixel){out.color=RtColorKnowledge::KnownZero;out.basis=RtColorBasis::NoPixel;}
        return out;
    }
    if (!pixel.outputs_known) {out.basis=RtColorBasis::PixelSignature;return out;}
    out.declared_slots=pixel.color_slots;
    out.standard_pixel_shape=pixel.standard_shape;
    if (!blend) {out.basis=RtColorBasis::BlendUnreported;return out;}
    if (!has_targets || color_dynamic) return out;
    const auto source1=[](blend_factor f){
        return f==blend_factor::source1_color||f==blend_factor::one_minus_source1_color||
            f==blend_factor::source1_alpha||f==blend_factor::one_minus_source1_alpha;
    };
    uint8_t shader_writes=0;
    for (unsigned n=0;n<8;++n) {
        if(blend->blend_enable[n]&&(source1(blend->source_color_blend_factor[n])||
           source1(blend->dest_color_blend_factor[n])||source1(blend->source_alpha_blend_factor[n])||
           source1(blend->dest_alpha_blend_factor[n])))return out;
        if(blend->logic_op_enable[n]&&blend->logic_op[n]==logic_op::noop)continue;
        const auto channels=blend->render_target_write_mask[n]&0x0f;
        if(channels)out.write_slots|=uint8_t(1u<<n);
        if(channels&pixel.color_channels[n])shader_writes|=uint8_t(1u<<n);
    }
    out.basis=RtColorBasis::Reflected;
    out.color_mask=shader_writes&target_mask;
    out.color=out.color_mask?RtColorKnowledge::KnownMask:RtColorKnowledge::KnownZero;
    return out;
}

void RtObserveNativeNoPixelEffects(uint64_t pipeline,uint32_t count,const pipeline_subobject *subobjects)
{
    if(!pipeline||!count||!subobjects)return;
    uint32_t proof=0;UINT bytes=sizeof(proof);
    {
        // init_pipeline owns the live object for this synchronous read.
        // Do not treat SDK state objects as ID3D12PipelineState or retain it.
        Microsoft::WRL::ComPtr<ID3D12PipelineState> native;
        auto *object=reinterpret_cast<IUnknown *>(pipeline);
        if(FAILED(object->QueryInterface(IID_PPV_ARGS(&native)))||
           FAILED(native->GetPrivateData(wwmi::rt_native_no_pixel::key,&bytes,&proof))||
           bytes!=sizeof(proof)||proof!=wwmi::rt_native_no_pixel::version_v1)return;
    }
    // The proved absence of PS is enough to know color is zero. Other
    // effects still use whichever independent PSO/OM metadata was observed.
    const auto effects=NativePsoEffects(std::span(subobjects,count),true);
    if(effects.basis!=RtColorBasis::NoPixel)return;
    std::lock_guard lk(g_pso_clone_mtx);
    g_rt_no_pixel_effects.insert_or_assign(pipeline,effects);
}

RtRasterEffects RtReadRasterEffects(uint64_t pipeline,uint8_t attached,bool om_known,bool dsv)
{
    std::lock_guard lk(g_pso_clone_mtx);
    const auto i=g_pso_clone.find(pipeline);
    PsoClone *proof=nullptr;
    if(i!=g_pso_clone.end())proof=&i->second;
    else if(const auto raw=g_rt_effect_inputs.find(pipeline);raw!=g_rt_effect_inputs.end())proof=&raw->second;
    RtRasterEffects out;
    if(proof){
        auto &clone=*proof;
        if(!clone.private_effects_ready){
            clone.private_effects=NativePsoEffects(clone.subs);
            clone.private_effects_ready=true;
        }
        out=clone.private_effects;
    }else{
        const auto no_pixel=g_rt_no_pixel_effects.find(pipeline);
        if(no_pixel==g_rt_no_pixel_effects.end())return out;
        out=no_pixel->second;
    }
    if(!om_known) {
        if(out.color==RtColorKnowledge::KnownMask){
            out.color=RtColorKnowledge::Unknown;out.basis=RtColorBasis::UnknownOm;out.color_mask=0;
        }
        out.depth_test=out.depth_write=out.stencil_test=out.stencil_write=RtEffect::Unknown;
        return out;
    }
    out.attached_slots=attached;
    if(out.color==RtColorKnowledge::KnownMask&&!(out.color_mask&=attached))out.color=RtColorKnowledge::KnownZero;
    if(!dsv)out.depth_test=out.depth_write=out.stencil_test=out.stencil_write=RtEffect::Absent;
    return out;
}


bool RtOnlyVertexPixelStages(uint64_t pipeline){
    std::lock_guard lock(g_pso_clone_mtx);auto i=g_pso_clone.find(pipeline);
    if(i==g_pso_clone.end())return false;
    for(auto &sub:i->second.subs)
        if((sub.type==pipeline_subobject_type::geometry_shader || sub.type==pipeline_subobject_type::hull_shader ||
            sub.type==pipeline_subobject_type::domain_shader || sub.type==pipeline_subobject_type::amplification_shader ||
            sub.type==pipeline_subobject_type::mesh_shader) && sub.data && sub.count){
            const auto *d=static_cast<const shader_desc*>(sub.data);
            for(unsigned j=0;j<sub.count;++j)if(d[j].code_size)return false;
        }
    return true;
}

// A recorded, fully cloned graphics PSO with no PS has no pixel-stage reads.
// Missing reflection for an actual pixel shader is not the same as no PS.
bool RtKnownNoPixelShader(uint64_t pipeline){
    std::lock_guard lock(g_pso_clone_mtx);auto i=g_pso_clone.find(pipeline);
    if(i==g_pso_clone.end() || i->second.subs.empty())return false;
    for(auto& sub:i->second.subs)if(sub.type==pipeline_subobject_type::pixel_shader){
        if(!sub.data && sub.count)return false;
        auto* shaders=static_cast<const shader_desc*>(sub.data);
        for(unsigned j=0;j<sub.count;++j)if(shaders[j].code_size)return false;
    }
    return true;
}

bool RtShaderUavRegs(const void*,size_t,DxilSrvRegs&);
DxilSrvRegs RtPipelineUavRegs(uint64_t pipeline,bool pixel){
    std::lock_guard lock(g_pso_clone_mtx);auto i=g_pso_clone.find(pipeline);
    if(i==g_pso_clone.end())return {};
    const auto type=pixel?pipeline_subobject_type::pixel_shader:pipeline_subobject_type::vertex_shader;
    DxilSrvRegs out{};
    for(auto& sub:i->second.subs)if(sub.type==type){
        if(sub.count!=1 || !sub.data)return {};
        const auto& shader=*static_cast<const shader_desc*>(sub.data);
        RtShaderUavRegs(shader.code,shader.code_size,out);return out;
    }
    return out;
}

// Do not infer a depth pass from VS hash or absence of COLOR varyings. This is
// checked against the same fully captured native PSO that the draw will use.
bool RtOpaqueDepthPass(uint64_t pipeline){
    std::lock_guard lock(g_pso_clone_mtx);
    const auto i=g_pso_clone.find(pipeline);if(i==g_pso_clone.end())return false;
    bool vertex=false,depth=false;
    for(const auto& sub:i->second.subs){
        const auto type=sub.type;
        if(type==pipeline_subobject_type::vertex_shader){
            if(vertex||sub.count!=1||!sub.data)return false;
            const auto& shader=*static_cast<const shader_desc*>(sub.data);
            if(!shader.code||!shader.code_size)return false;vertex=true;
        }else if(type==pipeline_subobject_type::pixel_shader||type==pipeline_subobject_type::hull_shader||
                 type==pipeline_subobject_type::domain_shader||type==pipeline_subobject_type::geometry_shader||
                 type==pipeline_subobject_type::amplification_shader||type==pipeline_subobject_type::mesh_shader||
                 type==pipeline_subobject_type::compute_shader){
            if(sub.count&&!sub.data)return false;
            const auto* shaders=static_cast<const shader_desc*>(sub.data);
            for(unsigned n=0;n<sub.count;++n)if(shaders[n].code_size)return false;
        }else if(type==pipeline_subobject_type::depth_stencil_state){
            if(depth||sub.count!=1||!sub.data)return false;
            const auto& state=*static_cast<const depth_stencil_desc*>(sub.data);
            if(!state.depth_enable||!state.depth_write_mask)return false;depth=true;
        }else if(type==pipeline_subobject_type::stream_output_state){
            if(!sub.count)continue;
            if(sub.count!=1||!sub.data||static_cast<const stream_output_desc*>(sub.data)->rasterized_stream!=0)return false;
        }
    }
    return vertex&&depth;
}

// Address reuse replaces the owning clone and therefore this creation stamp.
// Draw callers retain the immutable snapshot, not a pointer into the map.
static std::shared_ptr<const RtProgramPsoContract> FindProgramContract(uint64_t pso,uint64_t actual_vs,
    RtProgramLookupStamp* stamp)
{
    std::lock_guard lock(g_pso_clone_mtx);
    const auto i=g_pso_clone.find(pso);
    const auto* clone=i==g_pso_clone.end()?nullptr:&i->second;
    const auto* stored=clone?&clone->private_contract:nullptr;
    const auto* contract=stored?stored->get():nullptr;
    const bool identity=contract&&contract->vertex&&contract->vertex->native_identity==actual_vs;
    if(stamp){
        stamp->creation=0;
        if(!clone||clone->private_intake_superseded){
            const auto note=g_rt_unpublished_programs.find(pso);
            if(note!=g_rt_unpublished_programs.end())stamp->creation=note->second.creation;
        }else if(contract)stamp->creation=contract->creation;
        // The render lookup deliberately still follows its existing clone
        // and VS check. Report what it actually returned or refused.
        stamp->lookup_creation=contract?contract->creation:0;
        stamp->status=!clone?RtProgramLookup::no_clone:!contract?RtProgramLookup::no_contract:
            !contract->vertex?RtProgramLookup::vs_not_resolved:!identity?RtProgramLookup::vs_mismatch:
            contract->capabilities.current_previous?RtProgramLookup::usable:RtProgramLookup::unavailable;
        stamp->returned=identity;
    }
    return identity?*stored:std::shared_ptr<const RtProgramPsoContract>{};
}
std::shared_ptr<const RtProgramPsoContract> RtProgramContractForPso(uint64_t pso,uint64_t actual_vs){
    return FindProgramContract(pso,actual_vs,nullptr);
}
std::shared_ptr<const RtProgramPsoContract> RtProgramContractForPsoObserved(uint64_t pso,uint64_t actual_vs,RtProgramLookupStamp& stamp){
    return FindProgramContract(pso,actual_vs,&stamp);
}
RtProgramIntakeDetail RtReadProgramIntakeDetail(uint64_t pso,RtProgramLookupStamp stamp){
    if(!stamp.creation)return {};
    std::lock_guard lock(g_pso_clone_mtx);
    if(const auto i=g_pso_clone.find(pso);i!=g_pso_clone.end()&&!i->second.private_intake_superseded&&
        i->second.private_contract&&i->second.private_contract->creation==stamp.creation)
        return PsoProgramIntakeDetail(i->second,RtProgramStore::clone);
    if(const auto i=g_rt_unpublished_programs.find(pso);i!=g_rt_unpublished_programs.end()&&i->second.creation==stamp.creation)
        return i->second;
    RtProgramIntakeDetail detail{};
    detail.creation=stamp.creation;detail.store=RtProgramStore::retired;
    return detail;
}
