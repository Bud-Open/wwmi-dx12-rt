#pragma once
#include "rt_native_state_object.h"
#include <d3d12.h>
#include <d3d12shader.h>
#include <reshade.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace hsin_rt {
// An export set describes a successful native object, not the subset of
// shader bytes recognized by the client. Native inputs and SDK shader
// descriptors are borrowed; the entry name and binding facts are owned.
struct NativeRaygenExport {
    std::wstring name;
    bool raygen=false;
    std::optional<size_t> pin;
    std::optional<std::array<unsigned char,D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES>> identifier;
    bool miss=false,default_miss=false,local_associated=false;
    std::optional<unsigned> local_argument_bytes{0};
    std::string entry;
    std::optional<unsigned> reflected_as;
};
struct NativeRaygenDomain {
    uint64_t root=0,root_generation=0;
    bool conflicting_roots=false, executable=false,default_local_association=false;
    std::vector<NativeRaygenExport> exports;
    void require_root(uint64_t value,uint64_t generation){
        if(value&&root&&(root!=value||root_generation!=generation))conflicting_roots=true;
        else if(value){root=value;root_generation=generation;}
    }
    const NativeRaygenExport *find(const wchar_t *name)const{
        if(!name||!*name)return nullptr;
        const auto found=std::find_if(exports.begin(),exports.end(),[&](const auto &e){return e.name==name;});
        return found==exports.end()?nullptr:&*found;
    }
    bool add(NativeRaygenExport e){
        if(e.name.empty()||find(e.name.c_str()))return false;
        // A parent's executable identifiers never identify the new object.
        e.identifier.reset();exports.push_back(std::move(e));return true;
    }
};

// DXC's RDAT ResourceInfo/FunctionInfo. This is only a binding fact for a
// named executable entry, not a TraceRay contribution or payload proof.
inline uint32_t native_ray_u32(const uint8_t *p){uint32_t n;std::memcpy(&n,p,4);return n;}
struct NativeRayLibrary {
    using Bytes=std::span<const uint8_t>;
    Bytes strings,indices,resources,functions;
    static Bytes row(Bytes table,size_t stride_min,uint32_t index){
        if(table.size()<8)return {};
        const auto count=native_ray_u32(table.data()),stride=native_ray_u32(table.data()+4);
        if(stride<stride_min||count>(table.size()-8)/stride||index>=count)return {};
        return table.subspan(8+size_t(index)*stride,stride);
    }
    bool name(uint32_t start,const char *entry)const{
        const auto size=std::strlen(entry);
        return start<strings.size()&&size<strings.size()-start&&
            !std::memcmp(strings.data()+start,entry,size)&&strings[start+size]==0;
    }
    std::optional<unsigned> as_for(const char *entry)const{
        if(!entry||!*entry||functions.size()<8)return {};
        const auto count=native_ray_u32(functions.data()),stride=native_ray_u32(functions.data()+4);
        if(stride<44||count>(functions.size()-8)/stride)return {};
        Bytes function;
        for(uint32_t i=0;i<count;++i){
            const auto r=row(functions,44,i);
            if(native_ray_u32(r.data()+16)!=7||
               !(name(native_ray_u32(r.data()),entry)||name(native_ray_u32(r.data()+4),entry)))continue;
            if(!function.empty())return {};
            function=r;
        }
        // No unlinked function calls or ResourceDescriptorHeap indexing.
        if(function.empty()||native_ray_u32(function.data()+12)!=UINT32_MAX||
           (native_ray_u32(function.data()+28)&0x02000000u))return {};
        const auto first=native_ray_u32(function.data()+8);
        const size_t words=indices.size()/4;
        if(first>=words)return {};
        const auto *refs=indices.data()+size_t(first)*4;
        const auto n=native_ray_u32(refs);
        if(n>words-first-1)return {};
        std::optional<unsigned> binding;
        for(uint32_t i=0;i<n;++i){
            const auto r=row(resources,32,native_ray_u32(refs+4+size_t(i)*4));
            if(r.empty())return {};
            if(native_ray_u32(r.data()+4)!=16)continue; // RTAccelerationStructure
            const auto reg=native_ray_u32(r.data()+16);
            if(binding||native_ray_u32(r.data())!=0||native_ray_u32(r.data()+12)!=1||
               reg!=native_ray_u32(r.data()+20)||reg>=64)return {};
            binding=reg;
        }
        return binding;
    }
};

// Same DXBC/RDAT boundary as the host's hit-group proof. We can reason
// about the public explicit export list only when no embedded subobject
// can add an unknown local/global root or association.
inline bool native_no_embedded_ray_state(const D3D12_SHADER_BYTECODE &library,NativeRayLibrary *facts=nullptr){
    const auto *blob=static_cast<const uint8_t *>(library.pShaderBytecode);
    const size_t bytes=library.BytecodeLength;
    const auto u32=native_ray_u32;
    if(!blob||bytes<32||std::memcmp(blob,"DXBC",4)||u32(blob+20)!=1||u32(blob+24)!=bytes)return false;
    const auto walk=[&](const uint8_t *p,size_t n,size_t base,uint32_t count,auto part){
        if(base>n||count>(n-base)/4)return false;
        size_t end=base+size_t(count)*4;
        for(uint32_t i=0;i<count;++i){
            const size_t off=u32(p+base+size_t(i)*4);
            if((off&3)||off<end||off>n||n-off<8)return false;
            const size_t len=u32(p+off+4);
            if(len>n-off-8)return false;
            if(!part(u32(p+off),p+off+8,len))return false;
            end=off+8+len;
        }
        return true;
    };
    const uint8_t *rdat=nullptr;size_t size=0;
    if(!walk(blob,bytes,32,u32(blob+28),[&](uint32_t t,const uint8_t *p,size_t n){
        if(t==0x54414452u){if(rdat)return false;rdat=p;size=n;}return true;
    })||!rdat||size<8||u32(rdat)!=0x10)return false;
    uint32_t seen=0;
    return walk(rdat,size,8,u32(rdat+4),[&](uint32_t t,const uint8_t *p,size_t n){
        if(!t||t>6||(n&3)||(seen&(1u<<t)))return false;
        seen|=1u<<t;
        if(facts)switch(t){
        case 1:facts->strings={p,n};break;
        case 2:facts->indices={p,n};break;
        case 3:facts->resources={p,n};break;
        case 4:facts->functions={p,n};break;
        }
        return t!=6||(n==8&&!u32(p)&&!(u32(p+4)&3));
    });
}

inline bool native_ray_export_name(const wchar_t *source,const char *reflected){
    if(!source||!reflected||!*source)return false;
    while(*source&&*reflected&&*source==static_cast<unsigned char>(*reflected)){++source;++reflected;}
    return !*source&&!*reflected;
}

// "Unknown" is the whole result on unsupported export semantics, not
// an empty raygen list. The captured MainRG/Intersection/Reflection and
// scene-RGS library inputs use explicit names, with whole collection edges.
// The actual successful AddTo base is handled independently of imports.
template<class FindParent,class FindPin,class FindMiss,class RootGeneration,class LocalArgumentBytes>
std::optional<NativeRaygenDomain> native_raygen_domain(const WWMI_RtNativeStateObject_v1 &call,
    unsigned count,const reshade::api::pipeline_subobject *subobjects,FindParent find_parent,
    FindPin find_pin,FindMiss find_miss,RootGeneration root_generation,LocalArgumentBytes local_argument_bytes)
{
    if(call.size!=sizeof(call)||call.version!=1||!call.state||!call.device||!call.desc||(count&&!subobjects))return {};
    const auto &desc=*reinterpret_cast<const D3D12_STATE_OBJECT_DESC *>(call.desc);
    if((desc.Type!=D3D12_STATE_OBJECT_TYPE_COLLECTION&&desc.Type!=D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)||
       (desc.NumSubobjects&&!desc.pSubobjects))return {};
    NativeRaygenDomain out;
    std::vector<const D3D12_STATE_SUBOBJECT *> local_roots;
    out.executable=desc.Type==D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    const auto inherit=[&](uint64_t parent,const D3D12_EXPORT_DESC *names,unsigned n){
        const auto *source=find_parent(parent);
        if(!source||(n&&!names))return false;
        out.require_root(source->root,source->root_generation);
        out.conflicting_roots|=source->conflicting_roots;
        out.default_local_association|=source->default_local_association;
        if(!n){
            for(const auto &e:source->exports)if(!out.add(e))return false;
        }else for(unsigned i=0;i<n;++i){
            const auto &e=names[i];
            if(e.Flags!=D3D12_EXPORT_FLAG_NONE||!e.Name)return false;
            const auto *original=source->find(e.ExportToRename?e.ExportToRename:e.Name);
            if(!original)return false;
            auto selected=*original;selected.name=e.Name;
            if(!out.add(std::move(selected)))return false;
        }
        return true;
    };
    if(call.add_to_base&&!inherit(call.add_to_base,nullptr,0))return {};
    const auto shader_kind=[](reshade::api::pipeline_subobject_type t){
        using T=reshade::api::pipeline_subobject_type;
        return t==T::raygen_shader||t==T::miss_shader||t==T::any_hit_shader||
            t==T::closest_hit_shader||t==T::intersection_shader||t==T::callable_shader;
    };
    for(unsigned i=0;i<desc.NumSubobjects;++i){
        const auto &s=desc.pSubobjects[i];
        if(!s.pDesc)return {};
        switch(s.Type){
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
            const auto &library=*static_cast<const D3D12_DXIL_LIBRARY_DESC *>(s.pDesc);
            NativeRayLibrary facts;
            if(!library.NumExports||!library.pExports||!native_no_embedded_ray_state(library.DXILLibrary,&facts))return {};
            for(unsigned j=0;j<library.NumExports;++j){
                const auto &e=library.pExports[j];
                if(e.Flags!=D3D12_EXPORT_FLAG_NONE||!e.Name)return {};
                const auto *name=e.ExportToRename?e.ExportToRename:e.Name;
                const reshade::api::shader_desc *shader=nullptr;
                bool raygen=false,miss=false;
                for(unsigned k=0;k<count;++k){
                    const auto &reflected=subobjects[k];
                    if(!shader_kind(reflected.type)||!reflected.data)continue;
                    const auto *programs=static_cast<const reshade::api::shader_desc *>(reflected.data);
                    for(unsigned l=0;l<reflected.count;++l){
                        const auto &program=programs[l];
                        if(program.code!=library.DXILLibrary.pShaderBytecode||program.code_size!=library.DXILLibrary.BytecodeLength||
                           !native_ray_export_name(name,program.entry_point))continue;
                        if(shader)return {};
                        shader=&program;
                        raygen=reflected.type==reshade::api::pipeline_subobject_type::raygen_shader;
                        miss=reflected.type==reshade::api::pipeline_subobject_type::miss_shader;
                    }
                }
                if(!shader)return {};
                NativeRaygenExport exported{e.Name,raygen,raygen?find_pin(*shader,e.Name):std::nullopt,{},
                    miss,miss&&find_miss(*shader,e.Name)};
                if(raygen){
                    exported.entry=shader->entry_point;
                    exported.reflected_as=facts.as_for(shader->entry_point);
                }
                if(!out.add(std::move(exported)))return {};
            }
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
            const auto &p=*static_cast<const D3D12_EXISTING_COLLECTION_DESC *>(s.pDesc);
            if(!inherit(reinterpret_cast<uint64_t>(p.pExistingCollection),p.pExports,p.NumExports))return {};
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE: {
            const auto &p=*static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE *>(s.pDesc);
            if(!p.pGlobalRootSignature)return {};
            const auto root=reinterpret_cast<uint64_t>(p.pGlobalRootSignature);
            out.require_root(root,root_generation(root));break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
            const auto &p=*static_cast<const D3D12_HIT_GROUP_DESC *>(s.pDesc);
            if(!p.HitGroupExport||!out.add({p.HitGroupExport,false,{},{}}))return {};
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
            local_roots.push_back(&s);
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
        case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
        case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:
            break;
        default:
            return {}; // named embedded association, newer/unmodeled object or import
        }
    }
    for(unsigned i=0;i<desc.NumSubobjects;++i){
        const auto &s=desc.pSubobjects[i];
        if(s.Type!=D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION)continue;
        const auto &a=*static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(s.pDesc);
        if(!a.pSubobjectToAssociate||(a.NumExports&&!a.pExports))return {};
        const D3D12_STATE_SUBOBJECT *target=nullptr;
        for(unsigned j=0;j<desc.NumSubobjects;++j)if(a.pSubobjectToAssociate==&desc.pSubobjects[j])target=&desc.pSubobjects[j];
        if(!target)return {};
        switch(target->Type){
        case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
        case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:break;
        default:return {};
        }
        for(unsigned j=0;j<a.NumExports;++j)if(!out.find(a.pExports[j]))return {};
        if(target->Type!=D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE)continue;
        if(!a.NumExports){out.default_local_association=true;continue;}
        std::erase(local_roots,target);
        const auto &root=*static_cast<const D3D12_LOCAL_ROOT_SIGNATURE *>(target->pDesc);
        const auto bytes=local_argument_bytes(reinterpret_cast<uint64_t>(root.pLocalRootSignature));
        for(auto &e:out.exports)
            for(unsigned j=0;j<a.NumExports;++j)if(e.name==a.pExports[j]){
                e.local_argument_bytes=e.local_associated?std::nullopt:bytes;
                e.local_associated=true;
            }
    }
    // Implicit/default associations are unnecessary for the AS register
    // fact, but cannot authorize preservation of a specific SBT ABI.
    out.default_local_association|=!local_roots.empty();
    return out;
}
}
