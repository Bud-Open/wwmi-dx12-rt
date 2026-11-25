#include "rt_pack.hpp"
#include <dxgiformat.h>
#include "rt_geometry_plan.h"
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <limits>
namespace hsin_rt {
namespace {
struct Blob {
    std::span<const unsigned char> bytes;
    template<class T> std::vector<T> array(std::uint64_t offset,std::uint64_t count) const {
        if(offset>bytes.size()||count>(bytes.size()-offset)/sizeof(T))throw std::runtime_error("RT catalog array outside snapshot");
        std::vector<T> out(static_cast<size_t>(count));if(count)std::memcpy(out.data(),bytes.data()+offset,out.size()*sizeof(T));return out;
    }
    std::wstring string(std::uint32_t offset,std::uint32_t size) const {
        if(!size||size%2)throw std::runtime_error("RT catalog UTF-16 string");
        const auto value=array<std::uint16_t>(offset,size/2);if(value.back())throw std::runtime_error("RT catalog unterminated string");
        return std::wstring(value.begin(),value.end()-1);
    }
};
std::wstring lower(std::wstring value){for(auto &c:value)c=wchar_t(towlower(c));return value;}
NativeMeshIntent read_intent(const Blob &blob,const WWMI_RtModCatalog_v1 &h,const WWMI_RtMod_v1 &mod){
    NativeMeshIntent out;out.load_generation=h.load_generation;out.mod_key=mod.key;out.mod_revision=mod.revision;
    out.object_guid=mod.object_guid;out.enabled=(mod.flags&WWMI_RT_MOD_ENABLED)!=0;
    if(!out.object_guid||!mod.component_count||mod.component_count>4096)throw std::runtime_error("parsed Mod native intent bounds");
    const auto components=blob.array<WWMI_RtModComponent_v1>(mod.components_offset,mod.component_count);std::set<unsigned> ids;
    for(const auto &c:components){
        if(c.component<0||!ids.insert(unsigned(c.component)).second||!c.content_hash||
           (out.content_hash&&out.content_hash!=c.content_hash)||!c.native_index_count||c.native_index_count%3||
           c.native_index_first>UINT32_MAX-c.native_index_count)throw std::runtime_error("parsed Mod native intent identity invalid");
        out.content_hash=c.content_hash;out.components.push_back({unsigned(c.component),c.native_index_first,c.native_index_count});
    }
    std::sort(out.components.begin(),out.components.end(),[](const auto &a,const auto &b){return a.component<b.component;});
    return out;
}
unsigned word(std::istream &in){std::string s;if(!(in>>s))throw std::runtime_error("RT native profile truncated");size_t end=0;const auto n=std::stoull(s,&end,0);if(end!=s.size()||n>UINT32_MAX)throw std::runtime_error("RT native profile integer");return unsigned(n);}
template<class T> T scalar(const unsigned char *p){T v;std::memcpy(&v,p,sizeof(v));return v;}
std::vector<unsigned char> read_resource(WWMI_RtModReadResourceFn_v1 fn,const WWMI_RtModCatalog_v1 &h,
    const WWMI_RtMod_v1 &mod,const WWMI_RtModResource_v1 &res){
    if(!res.bytes||!(res.flags&WWMI_RT_RESOURCE_INITIAL_BYTES))throw std::runtime_error("engine resource has no copied source bytes");
    std::vector<unsigned char> value(static_cast<size_t>(res.bytes));std::uint64_t required=0;
    const auto status=fn(h.load_generation,mod.key,res.index,value.data(),value.size(),&required);
    if(status==WWMI_RT_MOD_STALE)throw std::runtime_error("RT Mod load generation changed during snapshot");
    if(status!=WWMI_RT_MOD_OK||required!=res.bytes)throw std::runtime_error("engine resource byte copy unavailable");return value;
}
std::shared_ptr<ModPack> build_pack(const Blob &blob,const WWMI_RtModCatalog_v1 &h,const WWMI_RtMod_v1 &mod,
    std::shared_ptr<const NativeProfile> native,WWMI_RtModReadResourceFn_v1 resource){
    auto pack=std::make_shared<ModPack>();pack->load_generation=h.load_generation;pack->key=mod.key;pack->revision=mod.revision;
    pack->name=blob.string(mod.namespace_offset,mod.namespace_bytes);pack->native=std::move(native);pack->vertices=mod.mesh_vertex_count;pack->shapekey_vertices=mod.shapekey_vertex_count;
    if(!pack->vertices||pack->vertices>UINT32_MAX/32)throw std::runtime_error("Mod vertex count outside supported buffer range");
    const auto components=blob.array<WWMI_RtModComponent_v1>(mod.components_offset,mod.component_count);
    if(components.size()!=pack->native->components.size())throw std::runtime_error("native component set differs from Mod component set");
    const auto resources=blob.array<WWMI_RtModResource_v1>(mod.resources_offset,mod.resource_count);
    auto find=[&](const std::wstring &name){const auto want=lower(name);return std::find_if(resources.begin(),resources.end(),[&](const auto &v){return lower(blob.string(v.name_offset,v.name_bytes))==want;});};
    auto get=[&](const std::wstring &name){const auto r=find(name);if(r==resources.end())throw std::runtime_error("required parsed Mod resource missing");return std::pair{*r,read_resource(resource,h,mod,*r)};};
    auto [position_info,positions]=get(L"ResourcePositionBuffer");
    auto [blend_info,source_blends]=get(L"ResourceBlendBuffer");
    auto [index_info,source_indices]=get(L"ResourceIndexBuffer");
    if(position_info.stride!=12||positions.size()!=12ull*pack->vertices||source_indices.empty()||source_indices.size()%4)throw std::runtime_error("Mod Position/Index source layout unsupported");
    pack->positions=std::move(positions);
    if(blend_info.stride==32&&source_blends.size()==32ull*pack->vertices)pack->blends=std::move(source_blends);
    else if(blend_info.stride==16&&source_blends.size()==16ull*pack->vertices){
        pack->blends.resize(32ull*pack->vertices);
        for(unsigned v=0;v<pack->vertices;++v)for(unsigned slot=0;slot<8;++slot){
            const std::uint16_t bone=source_blends[16ull*v+slot],weight=std::uint16_t(source_blends[16ull*v+8+slot])*257u;
            std::memcpy(pack->blends.data()+32ull*v+2*slot,&bone,2);std::memcpy(pack->blends.data()+32ull*v+16+2*slot,&weight,2);
        }
    }else if(blend_info.stride==8&&source_blends.size()==8ull*pack->vertices){
        pack->blends.assign(32ull*pack->vertices,0);
        for(unsigned v=0;v<pack->vertices;++v)for(unsigned slot=0;slot<4;++slot){
            const std::uint16_t bone=source_blends[8ull*v+slot],weight=std::uint16_t(source_blends[8ull*v+4+slot])*257u;
            std::memcpy(pack->blends.data()+32ull*v+2*slot,&bone,2);std::memcpy(pack->blends.data()+32ull*v+16+2*slot,&weight,2);
        }
    }else throw std::runtime_error("Mod Blend source requires 8 x U16/UNORM16, 8 x U8/UNORM8, or 4 x U8/UNORM8");
    // Require the parsed formats, not directory names or byte counts alone.
    const auto vector=find(L"ResourceVectorBuffer"),uv=find(L"ResourceTexCoordBuffer"),color=find(L"ResourceColorBuffer");
    if(vector!=resources.end()&&vector->reserved==DXGI_FORMAT_R8G8B8A8_SNORM&&vector->bytes==8ull*pack->vertices)pack->vectors=read_resource(resource,h,mod,*vector);
    if(uv!=resources.end()&&uv->reserved==DXGI_FORMAT_R16G16_FLOAT&&uv->bytes%(4ull*pack->vertices)==0&&uv->bytes/(4ull*pack->vertices)>0&&uv->bytes/(4ull*pack->vertices)<=8){
        pack->uv=read_resource(resource,h,mod,*uv);pack->uv_pairs=unsigned(uv->bytes/(4ull*pack->vertices));
    }
    pack->surface_supported=!pack->vectors.empty()&&!pack->uv.empty();
    if(color!=resources.end()&&color->reserved==DXGI_FORMAT_R8G8B8A8_UNORM&&color->bytes==4ull*pack->vertices)pack->color=read_resource(resource,h,mod,*color);
    std::vector<unsigned> vertex_owner(pack->vertices,UINT32_MAX);
    for(size_t c=0;c<pack->native->components.size();++c){const auto &n=pack->native->components[c];
        const auto source=std::find_if(components.begin(),components.end(),[&](const auto &v){return v.component>=0&&unsigned(v.component)==n.component;});
        if(source==components.end()||source->content_hash!=pack->native->content_hash||source->native_index_first!=n.index_first||source->native_index_count!=n.index_count)throw std::runtime_error("parsed Mod native identity does not match game-object profile");
        if(!(source->flags&WWMI_RT_COMPONENT_SELECTION_KNOWN))throw std::runtime_error("waiting for actual evaluated component draw selection");
        const auto draws=blob.array<WWMI_RtModDraw_v1>(source->draws_offset,source->draw_count);
        
        PackComponent out;out.native=n;
        out.selection_planned=(source->flags&WWMI_RT_GEOMETRY_SELECTION_PLANNED)!=0;
        out.mod_author_shared=(source->flags&WWMI_RT_GEOMETRY_MOD_AUTHOR_SHARED)!=0;
        out.extra_position_input=(source->flags&WWMI_RT_GEOMETRY_EXTRA_POSITION_INPUT)!=0;out.index_first=unsigned(pack->indices.size()/4);unsigned lo=UINT32_MAX,hi=0;
        for(const auto &draw:draws){
            if(!draw.index_count||draw.index_count%3||draw.first_index>source_indices.size()/4||draw.index_count>source_indices.size()/4-draw.first_index)throw std::runtime_error("actual Mod draw outside parsed Index resource");
            out.draws.push_back({draw.first_index,draw.index_count,draw.base_vertex,unsigned(pack->indices.size()/4)});
            for(unsigned k=0;k<draw.index_count;++k){const auto src=scalar<std::uint32_t>(source_indices.data()+4ull*(draw.first_index+k));const std::int64_t index=std::int64_t(src)+draw.base_vertex;
                if(index<0||std::uint64_t(index)>=pack->vertices)throw std::runtime_error("actual Mod draw index outside Position resource");
                const unsigned value=unsigned(index);lo=std::min(lo,value);hi=std::max(hi,value);const auto offset=pack->indices.size();pack->indices.resize(offset+4);std::memcpy(pack->indices.data()+offset,&value,4);
            }
        }
        out.index_count=unsigned(pack->indices.size()/4)-out.index_first;out.vertex_first=draws.empty()?0:lo;out.vertex_count=draws.empty()?0:hi-lo+1;
        if(out.vertex_count>65535u*64u)throw std::runtime_error("Mod component exceeds one skin dispatch range");
        std::set<unsigned> used;
        // The compute kernel writes the contiguous component interval. Do not
        // race two component poses on overlapping output vertices.
        for(unsigned v=out.vertex_first;v<out.vertex_first+out.vertex_count;++v){if(vertex_owner[v]!=UINT32_MAX&&vertex_owner[v]!=c)throw std::runtime_error("Mod components share a skinned output interval");vertex_owner[v]=unsigned(c);
            for(unsigned slot=0;slot<8;++slot){const auto bone=scalar<std::uint16_t>(pack->blends.data()+32ull*v+2*slot);const auto weight=scalar<std::uint16_t>(pack->blends.data()+32ull*v+16+2*slot);
                pack->canonical_bones=std::max(pack->canonical_bones,unsigned(bone)+1);if(weight)used.insert(bone);
            }
        }
        out.used.assign(used.begin(),used.end());
        const auto map=find(L"ResourceDrawBoneMapComponent"+std::to_wstring(n.component));
        std::vector<unsigned char> map_bytes;
        if(map==resources.end()){
            // Standard WWMI merge windows are already parsed by WwmiMod::Mod.
            // Prefer an explicit canonical BoneMap when the Mod provides one.
            if(!source->canonical_vg_count){out.rest_skinning_known=false;pack->rest_skinning_known=false;pack->components.push_back(std::move(out));continue;}
            if(source->canonical_vg_count>512||source->canonical_vg_offset>65536-source->canonical_vg_count)throw std::runtime_error("parsed WWMI canonical merge window out of range");
            map_bytes.resize(8ull*source->canonical_vg_count);
            for(unsigned k=0;k<source->canonical_vg_count;++k){const unsigned canonical=source->canonical_vg_offset+k;std::memcpy(map_bytes.data()+8ull*k,&k,4);std::memcpy(map_bytes.data()+8ull*k+4,&canonical,4);}
        }else map_bytes=read_resource(resource,h,mod,*map);
        if(map_bytes.empty()||map_bytes.size()%8)throw std::runtime_error("Mod canonical BoneMap byte layout");
        std::set<unsigned> destinations,sources;
        for(size_t k=0;k<map_bytes.size();k+=8){BonePair pair{scalar<unsigned>(map_bytes.data()+k),scalar<unsigned>(map_bytes.data()+k+4)};
            if(pair.source>=512||pair.canonical>=65536||!destinations.insert(pair.canonical).second||!sources.insert(pair.source).second)throw std::runtime_error("Mod canonical BoneMap is ambiguous or out of range");
            out.bone_map.push_back(pair);pack->canonical_bones=std::max(pack->canonical_bones,pair.canonical+1);
        }
        pack->components.push_back(std::move(out));
    }
    for(size_t c=0;c<pack->components.size();++c){auto &target=pack->components[c];if(!target.index_count||target.bone_map.empty())continue;std::set<unsigned> providers;
        for(unsigned bone:target.used){if(std::any_of(target.bone_map.begin(),target.bone_map.end(),[&](const auto &p){return p.canonical==bone;}))continue;unsigned provider=UINT32_MAX;
            for(size_t p=0;p<pack->components.size();++p)if(p!=c&&std::any_of(pack->components[p].bone_map.begin(),pack->components[p].bone_map.end(),[&](const auto &v){return v.canonical==bone;})){
                if(provider!=UINT32_MAX){target.rest_skinning_known=false;break;}provider=unsigned(p);
            }
            if(provider==UINT32_MAX||!target.rest_skinning_known){target.rest_skinning_known=false;break;}providers.insert(provider);
        }
        for(unsigned p:providers){Dependency dep;dep.provider=p;for(const auto &a:target.bone_map)for(const auto &b:pack->components[p].bone_map)if(a.canonical==b.canonical)dep.anchors.push_back({a.source,b.source});
            if(dep.anchors.empty()){target.rest_skinning_known=false;break;}target.dependencies.push_back(std::move(dep));
        }
        if(!target.rest_skinning_known){target.dependencies.clear();pack->rest_skinning_known=false;}
    }
    if(pack->indices.empty())pack->indices.resize(4);
    return pack;
}
}
std::vector<std::shared_ptr<const NativeProfile>> read_native_profiles(const std::filesystem::path &path){
    std::ifstream input(path);std::string token;if(!(input>>token)||token!="WWMI_RT_NATIVE_V1")throw std::runtime_error("RT native profile file missing/version mismatch");
    std::vector<std::shared_ptr<const NativeProfile>> result;
    while(input>>token){if(token!="object")throw std::runtime_error("RT native profile object expected");auto p=std::make_shared<NativeProfile>();p->object_guid=word(input);p->content_hash=word(input);p->vertices=word(input);const auto count=word(input);
        if(!p->object_guid||!p->content_hash||!p->vertices||p->vertices>UINT32_MAX/12||!count||count>4096)throw std::runtime_error("RT native profile bounds");unsigned next=0;std::set<unsigned> ids;
        for(unsigned c=0;c<count;++c){if(!(input>>token)||token!="component")throw std::runtime_error("RT native profile component expected");NativeComponent n{word(input),word(input),word(input),word(input),word(input)};
            if(!ids.insert(n.component).second||n.first!=next||!n.count||n.first>p->vertices||n.count>p->vertices-n.first||!n.index_count||n.index_count%3||n.index_first>UINT32_MAX-n.index_count)throw std::runtime_error("RT native component windows invalid");next+=n.count;p->components.push_back(n);
        }
        if(next!=p->vertices||!(input>>token)||token!="end")throw std::runtime_error("RT native profile incomplete");
        if(std::any_of(result.begin(),result.end(),[&](const auto &v){return v->object_guid==p->object_guid||v->content_hash==p->content_hash;}))throw std::runtime_error("RT native profile identity duplicate");result.push_back(std::move(p));
    }
    return result;
}
PackCatalog PackSource::refresh(const PackCatalog &old) const {
    if(!catalog_||!resource_)throw std::runtime_error("RT main Mod snapshot ABI missing");std::uint64_t required=0;auto status=catalog_(nullptr,0,&required);
    if(status!=WWMI_RT_MOD_MORE||required<sizeof(WWMI_RtModCatalog_v1)||required>UINT32_MAX)throw std::runtime_error("RT main Mod catalog unavailable");
    std::vector<unsigned char> bytes(static_cast<size_t>(required));status=catalog_(bytes.data(),bytes.size(),&required);
    if(status==WWMI_RT_MOD_MORE)return old;
    if(status!=WWMI_RT_MOD_OK||required!=bytes.size())throw std::runtime_error("RT main Mod catalog read failed");
    Blob blob{bytes};const auto h=blob.array<WWMI_RtModCatalog_v1>(0,1).front();
    if(h.struct_size!=sizeof(h)||h.abi_version!=1||h.bytes!=bytes.size())throw std::runtime_error("RT main Mod catalog ABI mismatch");
    const auto native_state=registry_->snapshot();
    if(h.load_generation==old.load_generation&&h.revision==old.revision&&native_state->revision==old.native_revision)return old;
    PackCatalog next;next.load_generation=h.load_generation;next.revision=h.revision;next.native_revision=native_state->revision;
    const auto mods=blob.array<WWMI_RtMod_v1>(h.mods_offset,h.mod_count);std::set<unsigned> ambiguous;
    for(const auto &m:mods)if(m.flags&WWMI_RT_MOD_ENABLED)if(std::count_if(mods.begin(),mods.end(),[&](const auto &v){return (v.flags&WWMI_RT_MOD_ENABLED)&&v.object_guid==m.object_guid;})>1)ambiguous.insert(m.object_guid);
    for(const auto &mod:mods){const auto name=blob.string(mod.namespace_offset,mod.namespace_bytes);std::string reason;
        try{next.intents.push_back(read_intent(blob,h,mod));}catch(const std::exception &e){reason=e.what();}
        if(!(mod.flags&WWMI_RT_MOD_ENABLED))reason="Mod geometry disabled";
        else if(ambiguous.contains(mod.object_guid))reason="native fallback: multiple enabled Mods claim this game object";
        else if(reason.empty()){const auto native=NativeProfileRegistry::resolve(native_state,next.intents.back());
            if(native.match==NativeProfileRegistry::Match::ambiguous)reason="native fallback: game-object native mesh signature ambiguous";
            else if(!native.profile)reason=mod.shapekey_vertex_count?"native fallback: Mod needs final ShapeKey positions and game-object native vertex profile":"native fallback: game-object native vertex profile unavailable";
            else{const auto retained=std::find_if(old.packs.begin(),old.packs.end(),[&](const auto &p){return p->load_generation==h.load_generation&&p->key==mod.key&&p->revision==mod.revision&&p->native==native.profile;});
                if(retained!=old.packs.end())next.packs.push_back(*retained);
                else try{next.packs.push_back(build_pack(blob,h,mod,native.profile,resource_));}catch(const std::exception &e){reason=e.what();}
                if(reason.empty())reason=mod.shapekey_vertex_count?"loaded: final Mod ShapeKey position input required":(!next.packs.back()->rest_skinning_known?"loaded: final positions required for unresolved canonical bone mapping":"loaded: engine resources and evaluated draw selection");
            }
        }
        next.status.push_back({mod.key,name,std::move(reason)});
    }
    return next;
}
}
