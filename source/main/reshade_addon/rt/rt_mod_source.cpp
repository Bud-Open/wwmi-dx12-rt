#define WWMI_RT_SOURCE_IMPLEMENTATION
#include "rt_mod_source.h"
#include "rt_section_identity.h"
#include "rt_geometry_plan.h"
#include "rt_localvf_surface.h"
#include "rt_author_rest.hpp"
#include "rt_author_position_origin.hpp"
#include "mods/claim_state.h"
#include "migoto/port_api.h"
#include "src/log.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <limits>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <vector>
#include <wrl/client.h>

extern std::atomic<bool> g_rt_user_disabled;

namespace {
struct AuthorRestAsset {
    uint64_t generation=0;
    RtAuthorPositionBinding binding;
    Microsoft::WRL::ComPtr<ID3D12Resource> binding_owner;
    std::vector<unsigned char> bytes;
};
struct ResourceCopy {
    std::wstring name;uint32_t stride=0,format=0;
    std::vector<unsigned char> bytes;
    std::shared_ptr<const AuthorRestAsset> position;
    const auto &data()const{return position?position->bytes:bytes;}
};
struct ComponentCopy {int32_t component=-1;uint32_t hash=0,first=0,count=0,vg_offset=0,vg_count=0;bool known=false;std::vector<WWMI_RtModDraw_v1> draws;
    bool plan_known=false,plan_seen=false;uint32_t plan_flags=0;uint64_t plan_frame=0,plan_token=0;std::vector<WWMI_RtModDraw_v1> plan_draws;
};
struct ModCopy {
    uint64_t key=0,revision=1,geometry_revision=1;uint32_t flags=0,guid=0,vertices=0,shape_vertices=0;
    std::wstring ns,dir;std::vector<ResourceCopy> resources;std::vector<ComponentCopy> components;
};
struct Pending {uint64_t generation,key;int32_t component;std::vector<WWMI_RtModDraw_v1> draws;
    uint64_t author_plan=0;WWMI_RtModGeometrySection_v1 complete_plan{};
    std::vector<WWMI_RtModDraw_v1> complete_geometry,complete_draws;
    uint64_t completed_recipe=0;};
std::mutex source_mutex;
uint64_t generation=0,revision=0,geometry_revision=0,next_token=0;
uint64_t next_position_asset=0;
std::vector<ModCopy> mods;
std::map<uint64_t,Pending> pending;
struct PendingPlan {uint64_t generation,key,section_token,frame;int32_t component;uint32_t flags;bool valid=true,author=false;
    std::vector<WWMI_RtModDraw_v1> draws,issued;};
std::map<uint64_t,PendingPlan> plans;
std::vector<unsigned char> catalog,geometry_catalog,surface_catalog;
bool dirty=true;
std::set<uint32_t> detected;
struct PositionCopy {WWMI_RtModPosition_v1 value;Microsoft::WRL::ComPtr<ID3D12Resource> hold;};
struct RecordingCopy {uint64_t creation=0,recording=0;std::map<std::pair<uint64_t,int32_t>,PositionCopy> positions,geometry_positions;};
std::map<uint64_t,RecordingCopy> recordings;

bool same(const std::vector<WWMI_RtModDraw_v1> &a,const std::vector<WWMI_RtModDraw_v1> &b){
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const auto &x,const auto &y){
        return x.index_count==y.index_count&&x.first_index==y.first_index&&x.base_vertex==y.base_vertex;});
}
struct Snapshot {
    std::vector<unsigned char> bytes;
    uint32_t append(const void *p,size_t n,size_t alignment=4){
        const size_t offset=(bytes.size()+alignment-1)&~(alignment-1);
        if(offset>UINT32_MAX || n>UINT32_MAX-offset)throw std::bad_alloc();
        bytes.resize(offset+n);if(n)std::memcpy(bytes.data()+offset,p,n);return uint32_t(offset);
    }
    template<class T> uint32_t reserve(size_t n){std::vector<T> value(n);return append(value.data(),value.size()*sizeof(T),alignof(T));}
    template<class T> void put(uint32_t offset,const T &v){std::memcpy(bytes.data()+offset,&v,sizeof(v));}
    uint32_t string(const std::wstring &value){return append(value.c_str(),(value.size()+1)*sizeof(wchar_t),2);}
};
uint32_t planned_geometry_flags(uint32_t flags){
    return WWMI_RT_COMPONENT_SELECTION_KNOWN|WWMI_RT_GEOMETRY_SELECTION_PLANNED|
        ((flags&WWMI_RT_PLAN_MOD_AUTHOR_SHARED)?WWMI_RT_GEOMETRY_MOD_AUTHOR_SHARED:0)|
        ((flags&WWMI_RT_PLAN_EXTRA_POSITION_INPUT)?WWMI_RT_GEOMETRY_EXTRA_POSITION_INPUT:0);
}
uint32_t geometry_flags(const ComponentCopy &c){
    if(!c.plan_known)return !c.plan_seen&&c.known?WWMI_RT_COMPONENT_SELECTION_KNOWN:0;
    return planned_geometry_flags(c.plan_flags);
}
std::vector<unsigned char> serialize(bool use_plans,bool surface=false){
    Snapshot out;out.reserve<WWMI_RtModCatalog_v1>(1);const uint32_t mo=out.reserve<WWMI_RtMod_v1>(mods.size());
    for(size_t i=0;i<mods.size();++i){const auto &m=mods[i];WWMI_RtMod_v1 d{};
        d.key=m.key;d.revision=use_plans?m.geometry_revision:m.revision;d.flags=m.flags;d.object_guid=m.guid;d.mesh_vertex_count=m.vertices;d.shapekey_vertex_count=m.shape_vertices;
        d.namespace_offset=out.string(m.ns);d.namespace_bytes=uint32_t((m.ns.size()+1)*2);
        d.directory_offset=out.string(m.dir);d.directory_bytes=uint32_t((m.dir.size()+1)*2);
        d.component_count=uint32_t(m.components.size());d.components_offset=out.reserve<WWMI_RtModComponent_v1>(m.components.size());
        for(size_t j=0;j<m.components.size();++j){const auto &c=m.components[j];WWMI_RtModComponent_v1 x{};
            x.component=c.component;x.flags=use_plans?geometry_flags(c):(c.known?WWMI_RT_COMPONENT_SELECTION_KNOWN:0);
            x.content_hash=c.hash;x.native_index_first=c.first;x.native_index_count=c.count;x.canonical_vg_offset=c.vg_offset;x.canonical_vg_count=c.vg_count;
            const std::vector<WWMI_RtModDraw_v1> empty;
            const auto &draws=use_plans?(c.plan_known?c.plan_draws:(c.plan_seen?empty:c.draws)):c.draws;
            x.draw_count=uint32_t(draws.size());x.draws_offset=out.append(draws.data(),draws.size()*sizeof(WWMI_RtModDraw_v1));
            out.put(d.components_offset+uint32_t(j*sizeof(x)),x);
        }
        d.resource_count=uint32_t(m.resources.size());d.resources_offset=out.reserve<WWMI_RtModResource_v1>(m.resources.size());
        for(size_t j=0;j<m.resources.size();++j){const auto &r=m.resources[j];WWMI_RtModResource_v1 x{};
            x.index=uint32_t(j);x.reserved=surface?r.format:0;x.flags=r.data().empty()?0:WWMI_RT_RESOURCE_INITIAL_BYTES;x.stride=r.stride;x.bytes=r.data().size();
            x.name_offset=out.string(r.name);x.name_bytes=uint32_t((r.name.size()+1)*2);out.put(d.resources_offset+uint32_t(j*sizeof(x)),x);
        }
        out.put(mo+uint32_t(i*sizeof(d)),d);
    }
    WWMI_RtModCatalog_v1 h{sizeof(h),1,out.bytes.size(),generation,use_plans?geometry_revision:revision,uint32_t(mods.size()),mo};out.put(0,h);return std::move(out.bytes);
}
void rebuild(){if(dirty){catalog=serialize(false);geometry_catalog=serialize(true);surface_catalog=serialize(true,true);dirty=false;}}
void clear_plan(ComponentCopy &c,bool reset_mode=false){if(reset_mode)c.plan_seen=false;c.plan_known=false;c.plan_flags=0;c.plan_frame=0;c.plan_token=0;c.plan_draws.clear();}
bool relevant(const std::wstring &name){
    std::wstring n=name;for(auto &c:n)c=wchar_t(towlower(c));
    return n==L"resourcevectorbuffer"||n==L"resourcetexcoordbuffer"||n==L"resourcecolorbuffer"||n==L"resourcepositionbuffer"||n==L"resourceblendbuffer"||n==L"resourceindexbuffer"||n.rfind(L"resourcedrawbonemapcomponent",0)==0;
}
}

static uint32_t parsed_format(const std::wstring &fq){
    wchar_t value[128]{};if(!PortApi::GetSectionValue(fq.c_str(),L"format",value,128))return 0;
    std::wstring f;for(wchar_t c:std::wstring(value))if(!iswspace(c))f+=wchar_t(towlower(c));
    if(f.rfind(L"dxgi_format_",0)==0)f.erase(0,12);
    if(f==L"r8g8b8a8_snorm")return DXGI_FORMAT_R8G8B8A8_SNORM;
    if(f==L"r16g16_float")return DXGI_FORMAT_R16G16_FLOAT;
    if(f==L"r8g8b8a8_unorm")return DXGI_FORMAT_R8G8B8A8_UNORM;return 0;
}

void WWMI_RtModPublish_v1(){
    // This function is called under the main's normal geometry/engine ownership.
    // Reader exports below touch only these immutable CPU copies.
    try {
        PortApi::EngineScope engine;
        const uint64_t live_generation=PortApi::LoadGeneration();
        std::lock_guard lock(source_mutex);
        if(!g_geom_loaded){if(!mods.empty()||generation!=live_generation){mods.clear();pending.clear();plans.clear();for(auto &[_,cl]:recordings){cl.positions.clear();cl.geometry_positions.clear();}generation=live_generation;++revision;++geometry_revision;dirty=true;}return;}
        if(generation!=live_generation||mods.size()!=g_geomods.size()){
            mods.clear();pending.clear();plans.clear();for(auto &[_,cl]:recordings){cl.positions.clear();cl.geometry_positions.clear();}generation=live_generation;
            for(size_t i=0;i<g_geomods.size();++i){const auto &gm=g_geomods[i];ModCopy m;
                m.key=i+1;m.ns=gm.wcl_ns;m.dir=gm.mod.dir;m.guid=gm.mod.object_guid;m.vertices=gm.mod.mesh_vertex_count;
                m.shape_vertices=uint32_t(PortApi::GetVarNs(m.ns.c_str(),L"shapekey_vertex_count",0.f));
                m.flags=((!g_rt_user_disabled.load(std::memory_order_acquire)&&ModGeomEnabled(int(i)))?WWMI_RT_MOD_ENABLED:0) |
                    (gm.aux_skip_only?WWMI_RT_MOD_SKIP_ONLY:0);
                for(const auto &c:gm.mod.comps)m.components.push_back({c.index,c.ib_hash,c.match_first_index,c.match_index_count,c.vg_offset,c.vg_count,false,{}});
                for(const auto &r:gm.mod.resources){if(!relevant(r.name))continue;ResourceCopy copy;copy.name=r.name;copy.stride=r.stride;copy.format=parsed_format(r.fq);
                    PortApi::BufferResourceInfo bi{};
                    if(PortApi::GetBufferResource(r.fq.c_str(),&bi)&&bi.byte_width){
                        copy.stride=bi.stride;
                        if(_wcsicmp(r.name.c_str(),L"ResourcePositionBuffer")==0&&bi.dx12&&bi.gpu_va&&bi.stride==12&&
                           m.vertices&&bi.byte_width==12ull*m.vertices&&
                           wwmi::author_position_origin::still_initial(bi.dx12,bi.byte_width)){
                            auto asset=std::make_shared<AuthorRestAsset>();
                            asset->bytes.resize(bi.byte_width);
                            if(wwmi::author_position_origin::read_initial(bi.dx12,asset->bytes.data(),bi.byte_width)){
                                asset->generation=++next_position_asset;
                                asset->binding={reinterpret_cast<uint64_t>(bi.dx12),bi.gpu_va,bi.byte_width,bi.stride};
                                asset->binding_owner=bi.dx12;
                                copy.position=std::move(asset);
                            }
                        }
                        if(!copy.position){
                            copy.bytes.resize(bi.byte_width);
                            if(PortApi::BufferInitialData(r.fq.c_str(),copy.bytes.data(),copy.bytes.size())!=copy.bytes.size()){
                                // Legacy catalog data only. Reopening a file does not
                                // establish what initialized the selected live Position.
                                if(r.filename.empty())copy.bytes.clear();
                                else {std::ifstream file(std::filesystem::path(r.filename),std::ios::binary|std::ios::ate);
                                    if(!file||file.tellg()!=std::streamoff(copy.bytes.size()))copy.bytes.clear();
                                    else {file.seekg(0);if(!file.read(reinterpret_cast<char *>(copy.bytes.data()),copy.bytes.size()))copy.bytes.clear();}
                                }
                            }
                        }
                    }
                    m.resources.push_back(std::move(copy));
                }
                mods.push_back(std::move(m));
            }
            ++revision;++geometry_revision;dirty=true;
            logger::probe("WWMI-LOAD phase=catalog-published generation=%llu rows=%zu",
                (unsigned long long)generation,mods.size());
        }else for(size_t i=0;i<mods.size();++i){auto &m=mods[i];const uint32_t flags=((!g_rt_user_disabled.load(std::memory_order_acquire)&&ModGeomEnabled(int(i)))?WWMI_RT_MOD_ENABLED:0) |
            (g_geomods[i].aux_skip_only?WWMI_RT_MOD_SKIP_ONLY:0);
            if(flags==m.flags)continue;m.flags=flags;for(auto &c:m.components){c.known=false;c.draws.clear();clear_plan(c,true);}std::erase_if(plans,[&](const auto &p){return p.second.key==m.key;});++m.revision;++revision;++m.geometry_revision;++geometry_revision;dirty=true;
        }
    }catch(...){std::lock_guard lock(source_mutex);mods.clear();pending.clear();plans.clear();recordings.clear();++revision;++geometry_revision;dirty=true;}
}

uint64_t WWMI_RtModSectionBegin_v1(int32_t mod_index,int32_t component){
    std::lock_guard lock(source_mutex);if(mod_index<0||size_t(mod_index)>=mods.size())return 0;
    const auto &m=mods[mod_index];if(!(m.flags&WWMI_RT_MOD_ENABLED)||std::none_of(m.components.begin(),m.components.end(),[&](const auto &c){return c.component==component;}))return 0;
    const auto token=++next_token;pending.emplace(token,Pending{generation,m.key,component,{}});return token;
}
void WWMI_RtModSectionDraw_v1(uint64_t token,uint32_t count,uint32_t first,int32_t base){
    if(!token||!count)return;std::lock_guard lock(source_mutex);auto p=pending.find(token);if(p==pending.end())return;
    // Repeated material/pass draws are one geometric range, not duplicate triangles.
    const WWMI_RtModDraw_v1 value{count,first,base};auto &draws=p->second.draws;
    if(std::none_of(draws.begin(),draws.end(),[&](const auto &v){return v.index_count==count&&v.first_index==first&&v.base_vertex==base;}))draws.push_back(value);
}
void WWMI_RtModSectionEnd_v1(uint64_t token,uint32_t success){
    if(!token)return;std::lock_guard lock(source_mutex);auto p=pending.find(token);if(p==pending.end())return;
    auto value=std::move(p->second);pending.erase(p);if(!success||value.generation!=generation||!value.key||value.key>mods.size())return;
    auto &m=mods[size_t(value.key-1)];auto c=std::find_if(m.components.begin(),m.components.end(),[&](const auto &v){return v.component==value.component;});
    if(c==m.components.end()||!(m.flags&WWMI_RT_MOD_ENABLED))return;
    if(!c->known||!same(c->draws,value.draws)){c->known=true;c->draws=std::move(value.draws);++m.revision;++revision;
        if(!c->plan_known&&!c->plan_seen){++m.geometry_revision;++geometry_revision;}dirty=true;}
}
extern "C" WWMI_RT_EXPORT uint32_t WWMI_RtModReadCatalog_v1(void *out,uint64_t capacity,uint64_t *required){
    if(!required)return WWMI_RT_MOD_BAD_INPUT;try{std::lock_guard lock(source_mutex);rebuild();*required=catalog.size();
        if(!out||capacity<catalog.size())return WWMI_RT_MOD_MORE;std::memcpy(out,catalog.data(),catalog.size());return WWMI_RT_MOD_OK;
    }catch(...){*required=0;return WWMI_RT_MOD_NOT_READY;}
}
extern "C" WWMI_RT_EXPORT uint32_t WWMI_RtModReadResource_v1(uint64_t gen,uint64_t key,uint32_t index,void *out,uint64_t capacity,uint64_t *required){
    if(!required)return WWMI_RT_MOD_BAD_INPUT;std::lock_guard lock(source_mutex);*required=0;
    if(gen!=generation)return WWMI_RT_MOD_STALE;if(!key||key>mods.size()||index>=mods[size_t(key-1)].resources.size())return WWMI_RT_MOD_BAD_INPUT;
    const auto &bytes=mods[size_t(key-1)].resources[index].data();*required=bytes.size();if(bytes.empty())return WWMI_RT_MOD_MISSING;
    if(!out||capacity<bytes.size())return WWMI_RT_MOD_MORE;std::memcpy(out,bytes.data(),bytes.size());return WWMI_RT_MOD_OK;
}

extern "C" WWMI_RT_EXPORT void WWMI_RtModStampRecording_v1(uint64_t list,uint64_t creation,uint64_t recording){
    std::lock_guard lock(source_mutex);
    if(!creation||!recording){recordings.erase(list);return;}
    auto it=recordings.find(list);
    if(it!=recordings.end()){
        it->second.creation=creation;
        it->second.recording=recording;
        if(!it->second.positions.empty())it->second.positions.clear();
        if(!it->second.geometry_positions.empty())it->second.geometry_positions.clear();
    }else{
        recordings.emplace(list,RecordingCopy{creation,recording,{}});
    }
}
static uint32_t publish_position(uint64_t list,int32_t component,const WWMI_RtModPosition_v1 *v,uint32_t size,bool geometry){
    if(!v||size!=sizeof(*v)||v->struct_size!=sizeof(*v)||v->abi_version!=1||
        (v->mode!=WWMI_RT_POSITION_FINAL&&v->mode!=WWMI_RT_POSITION_REST_VERIFIED))return WWMI_RT_MOD_BAD_INPUT;
    std::lock_guard lock(source_mutex);auto cl=recordings.find(list);if(cl==recordings.end())return WWMI_RT_MOD_NOT_READY;
    if(v->load_generation!=generation||!v->mod_key||v->mod_key>mods.size()||(v->mod_revision&&(geometry?mods[size_t(v->mod_key-1)].geometry_revision:mods[size_t(v->mod_key-1)].revision)!=v->mod_revision))return WWMI_RT_MOD_STALE;
    const auto &mod=mods[size_t(v->mod_key-1)];if(std::none_of(mod.components.begin(),mod.components.end(),[&](const auto &c){return c.component==component;}))return WWMI_RT_MOD_BAD_INPUT;
    PositionCopy copy;copy.value=*v;
    if(v->mode==WWMI_RT_POSITION_FINAL){
        if(!v->resource||!v->resource_creation||v->stride!=12||!v->vertex_count||v->bytes!=12ull*v->vertex_count||
            v->first_vertex>mod.vertices||v->vertex_count>mod.vertices-v->first_vertex||
            !(v->resource_state&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))return WWMI_RT_MOD_BAD_INPUT;
        auto *resource=reinterpret_cast<ID3D12Resource *>(v->resource);const auto desc=resource->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||v->resource_offset>desc.Width||v->bytes>desc.Width-v->resource_offset)return WWMI_RT_MOD_BAD_INPUT;
        copy.hold=resource;
    }else if(mod.shape_vertices)return WWMI_RT_MOD_BAD_INPUT;
    (geometry?cl->second.geometry_positions:cl->second.positions)[{v->mod_key,component}]=std::move(copy);return WWMI_RT_MOD_OK;
}
static uint32_t acquire_position(uint64_t list,uint64_t creation,uint64_t recording,
    uint64_t gen,uint64_t key,uint64_t rev,int32_t component,WWMI_RtModPosition_v1 *out,uint32_t size,bool geometry){
    if(!out||size!=sizeof(*out))return WWMI_RT_MOD_BAD_INPUT;*out={};std::lock_guard lock(source_mutex);
    if(gen!=generation)return WWMI_RT_MOD_STALE;auto cl=recordings.find(list);if(cl==recordings.end()||cl->second.creation!=creation||cl->second.recording!=recording)return WWMI_RT_MOD_NOT_READY;
    if(geometry&&(!key||key>mods.size()||mods[size_t(key-1)].geometry_revision!=rev))return WWMI_RT_MOD_STALE;
    const auto &positions=geometry?cl->second.geometry_positions:cl->second.positions;
    const auto found=positions.find({key,component});const PositionCopy *position=found==positions.end()?nullptr:&found->second;
    // Existing callers keep their original publication route until they opt
    // this component into the new planning domain. Do not reinterpret a plan
    // revision as an issued-draw revision merely because integers coincide.
    if(geometry&&!position&&mods[size_t(key-1)].geometry_revision==mods[size_t(key-1)].revision){
        const auto &mod=mods[size_t(key-1)];const auto part=std::find_if(mod.components.begin(),mod.components.end(),[&](const auto &c){return c.component==component;});
        if(part!=mod.components.end()&&!part->plan_seen){const auto legacy=cl->second.positions.find({key,component});if(legacy!=cl->second.positions.end())position=&legacy->second;}
    }
    if(!position)return WWMI_RT_MOD_MISSING;
    if(position->value.mod_revision&&position->value.mod_revision!=rev)return WWMI_RT_MOD_STALE;*out=position->value;out->mod_revision=rev;
    if(position->hold)position->hold->AddRef();return WWMI_RT_MOD_OK;
}

uint32_t WWMI_RtModPublishPosition_v1(uint64_t list,int32_t component,const WWMI_RtModPosition_v1 *value,uint32_t size){return publish_position(list,component,value,size,false);}
extern "C" WWMI_RT_EXPORT uint32_t WWMI_RtModAcquirePosition_v1(uint64_t list,uint64_t creation,uint64_t recording,
    uint64_t gen,uint64_t key,uint64_t revision,int32_t component,WWMI_RtModPosition_v1 *out,uint32_t size){return acquire_position(list,creation,recording,gen,key,revision,component,out,size,false);}
uint32_t WWMI_RtModPublishGeometryPosition_v1(uint64_t list,int32_t component,const WWMI_RtModPosition_v1 *value,uint32_t size){return publish_position(list,component,value,size,true);}
extern "C" WWMI_RT_EXPORT uint32_t WWMI_RtModAcquireGeometryPosition_v1(uint64_t list,uint64_t creation,uint64_t recording,
    uint64_t gen,uint64_t key,uint64_t revision,int32_t component,WWMI_RtModPosition_v1 *out,uint32_t size){return acquire_position(list,creation,recording,gen,key,revision,component,out,size,true);}

extern "C" WWMI_RT_EXPORT void WWMI_RtModMarkDetected_v1(uint32_t guid){std::lock_guard lock(source_mutex);detected.insert(guid);}
void WWMI_RtModDrainDetection_v1(){
    std::set<uint32_t> found;std::vector<std::wstring> namespaces;
    {std::lock_guard lock(source_mutex);found.swap(detected);for(const auto &m:mods)if(found.count(m.guid))namespaces.push_back(m.ns);}
    PortApi::EngineScope engine;for(const auto &ns:namespaces)PortApi::SetVarNs(ns.c_str(),L"object_detected",1.f);
}

extern "C" WWMI_RT_EXPORT uint32_t WWMI_RtModReadSectionIdentity_v1(uint64_t token,WWMI_RtModSectionIdentity_v1 *out,uint32_t size){
    if(!out||size!=sizeof(*out))return WWMI_RT_MOD_BAD_INPUT;*out={};if(!token)return WWMI_RT_MOD_NOT_READY;
    std::lock_guard lock(source_mutex);const auto p=pending.find(token);if(p==pending.end())return WWMI_RT_MOD_NOT_READY;
    if(p->second.generation!=generation||!p->second.key||p->second.key>mods.size())return WWMI_RT_MOD_STALE;
    const auto &m=mods[size_t(p->second.key-1)];if(!(m.flags&WWMI_RT_MOD_ENABLED))return WWMI_RT_MOD_NOT_READY;
    const auto component=std::find_if(m.components.begin(),m.components.end(),[&](const auto &c){return c.component==p->second.component;});
    if(component==m.components.end())return WWMI_RT_MOD_STALE;
    out->struct_size=sizeof(*out);out->abi_version=1;out->load_generation=generation;out->mod_key=m.key;out->mod_revision=m.revision;
    out->component=component->component;out->selection_known=component->known?1:0;return WWMI_RT_MOD_OK;
}

static uint64_t begin_plan(uint64_t section_token,uint64_t frame,uint32_t flags,bool author){
    if(!section_token||!frame||(flags&~uint32_t(WWMI_RT_PLAN_MOD_AUTHOR_SHARED|WWMI_RT_PLAN_EXTRA_POSITION_INPUT)))return 0;
    std::lock_guard lock(source_mutex);const auto section=pending.find(section_token);if(section==pending.end())return 0;const auto &s=section->second;
    if(author){section->second.complete_plan={};section->second.complete_geometry.clear();section->second.complete_draws.clear();section->second.completed_recipe=0;}
    if(s.generation!=generation||!s.key||s.key>mods.size())return 0;auto &m=mods[size_t(s.key-1)];
    if(!(m.flags&WWMI_RT_MOD_ENABLED))return 0;
    auto c=std::find_if(m.components.begin(),m.components.end(),[&](const auto &v){return v.component==s.component;});
    if(c==m.components.end()||(!author&&frame<c->plan_frame))return 0;
    if(!c->plan_seen){c->plan_seen=true;++m.geometry_revision;++geometry_revision;dirty=true;}
    auto &open=author?section->second.author_plan:c->plan_token;
    if(open)plans.erase(open);
    const auto token=++next_token;plans.emplace(token,PendingPlan{generation,m.key,section_token,frame,s.component,flags,true,author,{},{}});
    open=token;return token;
}
uint64_t WWMI_RtModPlanBegin_v1(uint64_t section_token,uint64_t frame,uint32_t flags){return begin_plan(section_token,frame,flags,false);}
extern "C" uint64_t WWMI_RtModBeginAuthorSection_v1(uint64_t section_token,uint64_t frame,uint32_t flags){return begin_plan(section_token,frame,flags,true);}
uint32_t WWMI_RtModPlanDraw_v1(uint64_t token,uint32_t count,uint32_t first,int32_t base,uint32_t flags){
    std::lock_guard lock(source_mutex);auto p=plans.find(token);if(p==plans.end())return WWMI_RT_MOD_STALE;
    if((flags&~uint32_t(WWMI_RT_PLAN_EXTRA_POSITION_INPUT))||first>UINT32_MAX-count){p->second.valid=false;return WWMI_RT_MOD_BAD_INPUT;}
    if(!count)return WWMI_RT_MOD_OK;p->second.flags|=flags;const WWMI_RtModDraw_v1 draw{count,first,base};
    auto &draws=p->second.draws;if(std::none_of(draws.begin(),draws.end(),[&](const auto &v){return v.index_count==count&&v.first_index==first&&v.base_vertex==base;}))draws.push_back(draw);
    if(p->second.author)p->second.issued.push_back(draw);
    return WWMI_RT_MOD_OK;
}
uint32_t WWMI_RtModPlanEnd_v1(uint64_t token,uint32_t complete){
    std::lock_guard lock(source_mutex);auto p=plans.find(token);if(p==plans.end())return WWMI_RT_MOD_STALE;
    auto plan=std::move(p->second);plans.erase(p);
    if(plan.generation!=generation||!plan.key||plan.key>mods.size())return WWMI_RT_MOD_STALE;
    auto &m=mods[size_t(plan.key-1)];auto c=std::find_if(m.components.begin(),m.components.end(),[&](const auto &v){return v.component==plan.component;});
    if(c==m.components.end())return WWMI_RT_MOD_STALE;
    const auto section=pending.find(plan.section_token);
    if(plan.author&&section==pending.end())return WWMI_RT_MOD_STALE;
    auto &open=plan.author?section->second.author_plan:c->plan_token;
    if(open!=token)return WWMI_RT_MOD_STALE;open=0;
    if(!complete||!plan.valid||section==pending.end()||section->second.generation!=generation||!(m.flags&WWMI_RT_MOD_ENABLED)){
        if(!plan.author&&c->plan_known){clear_plan(*c);++m.geometry_revision;++geometry_revision;dirty=true;}
        return !plan.valid?WWMI_RT_MOD_BAD_INPUT:(!complete?WWMI_RT_MOD_OK:WWMI_RT_MOD_STALE);
    }
    const bool changed=!c->plan_known||c->plan_flags!=plan.flags||!same(c->plan_draws,plan.draws);
    // Frame ordering governs only the shared catalog seed. An older genuine
    // VM invocation still owns its local receipt; a later Primitive cannot
    // invalidate its actual draw recipe.
    if(!plan.author||plan.frame>=c->plan_frame){
        if(changed){
            c->plan_flags=plan.flags;
            if(plan.author)c->plan_draws=plan.draws;else c->plan_draws=std::move(plan.draws);
            ++m.geometry_revision;++geometry_revision;dirty=true;
        }
        c->plan_known=true;c->plan_frame=plan.frame;
    }
    if(plan.author){
        section->second.complete_plan={sizeof(WWMI_RtModGeometrySection_v1),1,generation,m.key,m.geometry_revision,plan.frame,c->component,planned_geometry_flags(plan.flags)};
        section->second.complete_geometry=std::move(plan.draws);
        section->second.complete_draws=std::move(plan.issued);
        section->second.completed_recipe=token;
    }
    return WWMI_RT_MOD_OK;
}
wwmi::deform::Publication RtPublishSelectedAuthorRest(uint64_t section,uint64_t completed_recipe,
    const WWMI_RtDeformedSelection_v1 &owner,const RtAuthorPositionBinding &binding){
    std::shared_ptr<const AuthorRestAsset> asset;
    {
        std::lock_guard lock(source_mutex);
        const auto it=pending.find(section);
        if(it==pending.end()||!completed_recipe||it->second.completed_recipe!=completed_recipe)return {};
        const auto &selected=it->second.complete_plan;
        if(selected.load_generation!=generation||selected.load_generation!=owner.load_generation||
           !selected.mod_key||selected.mod_key>mods.size()||selected.mod_key!=owner.mod_key||
           !owner.mod_revision||selected.component<0||
           uint32_t(selected.component)!=owner.native_write.component||it->second.complete_draws.empty()||
           !(selected.selection_flags&WWMI_RT_GEOMETRY_MOD_AUTHOR_SHARED)||
           (selected.selection_flags&WWMI_RT_GEOMETRY_EXTRA_POSITION_INPUT))return {};
        const auto &mod=mods[size_t(selected.mod_key-1)];
        if(!(mod.flags&WWMI_RT_MOD_ENABLED)||mod.shape_vertices)return {};
        for(const auto &r:mod.resources)if(r.position){
            if(asset||r.position->binding!=binding)return {};
            asset=r.position;
        }
    }
    // The same EngineScope spans VM evaluation, this closed recipe, and
    // its synchronous client acquire. All actual port writes revoke the
    // selected object's initial fact, even if IA resource/VA/stride did not
    // change (including a write in the post section after the subdraw).
    if(!asset||!wwmi::author_position_origin::still_initial(asset->binding_owner.Get(),UINT(asset->bytes.size())))return {};
    const auto id=asset->generation;
    return wwmi::deform::publish_author_rest(owner,{completed_recipe,id,std::move(asset)});
}
extern "C" WWMI_RT_EXPORT uint32_t WWMI_RtModReadAuthorSection_v2(uint64_t token,uint64_t frame,WWMI_RtModGeometrySection_v1 *out,uint32_t size,
    uint32_t subdraw_count,uint32_t subdraw_index,WWMI_RtModDraw_v1 *subdraw,const WWMI_RtModAuthorRecipe_v1 *expected){
    if(!out||size!=sizeof(*out))return WWMI_RT_MOD_BAD_INPUT;*out={};
    std::lock_guard lock(source_mutex);const auto it=pending.find(token);
    if(it==pending.end()||!it->second.complete_plan.struct_size)return WWMI_RT_MOD_NOT_READY;
    const auto &plan=it->second.complete_plan;
    if(plan.load_generation!=generation||!plan.mod_key||plan.mod_key>mods.size()||
       (frame&&plan.plan_frame!=frame))return WWMI_RT_MOD_STALE;
    const auto &mod=mods[size_t(plan.mod_key-1)];
    if(!(mod.flags&WWMI_RT_MOD_ENABLED))return WWMI_RT_MOD_STALE;
    if(expected){
        const auto &geometry=it->second.complete_geometry;
        if(expected->selection_flags!=plan.selection_flags||geometry.size()!=expected->draw_count||
           (expected->draw_count&&(!expected->draws||!std::equal(geometry.begin(),geometry.end(),expected->draws,
               [](const auto &a,const auto &b){return a.index_count==b.index_count&&a.first_index==b.first_index&&a.base_vertex==b.base_vertex;}))))
            return WWMI_RT_MOD_STALE;
    }
    if(subdraw){
        *subdraw={};const auto &draws=it->second.complete_draws;
        if(draws.size()!=subdraw_count||(subdraw_count?subdraw_index>=subdraw_count:subdraw_index!=0))return WWMI_RT_MOD_STALE;
        if(subdraw_count)*subdraw=draws[subdraw_index];
    }
    *out=plan;return WWMI_RT_MOD_OK;
}
extern "C" WWMI_RT_EXPORT uint32_t WWMI_RtModReadAuthorSection_v1(uint64_t token,uint64_t frame,WWMI_RtModGeometrySection_v1 *out,uint32_t size,
    uint32_t subdraw_count,uint32_t subdraw_index,WWMI_RtModDraw_v1 *subdraw){
    return WWMI_RtModReadAuthorSection_v2(token,frame,out,size,subdraw_count,subdraw_index,subdraw,nullptr);
}
extern "C" WWMI_RT_EXPORT uint64_t WWMI_RtModReadAuthorRevision_v1(uint64_t gen,uint64_t key){
    std::lock_guard lock(source_mutex);
    if(gen!=generation||!key||key>mods.size())return 0;
    const auto &mod=mods[size_t(key-1)];
    if(!(mod.flags&WWMI_RT_MOD_ENABLED)||mod.components.empty()||
       std::any_of(mod.components.begin(),mod.components.end(),[](const auto &c){return !c.plan_known;}))return 0;
    return mod.geometry_revision;
}
extern "C" WWMI_RT_EXPORT uint32_t WWMI_RtModReadGeometryCatalog_v1(void *out,uint64_t capacity,uint64_t *required){
    if(!required)return WWMI_RT_MOD_BAD_INPUT;try{std::lock_guard lock(source_mutex);rebuild();*required=geometry_catalog.size();
        if(!out||capacity<geometry_catalog.size())return WWMI_RT_MOD_MORE;std::memcpy(out,geometry_catalog.data(),geometry_catalog.size());return WWMI_RT_MOD_OK;
    }catch(...){*required=0;return WWMI_RT_MOD_NOT_READY;}
}
extern "C" WWMI_RT_EXPORT uint32_t WWMI_RtModReadGeometrySection_v1(uint64_t token,uint64_t frame,WWMI_RtModGeometrySection_v1 *out,uint32_t size){
    if(!out||size!=sizeof(*out))return WWMI_RT_MOD_BAD_INPUT;*out={};
    std::lock_guard lock(source_mutex);const auto p=pending.find(token);if(p==pending.end())return WWMI_RT_MOD_NOT_READY;
    const auto &section=p->second;if(section.generation!=generation||!section.key||section.key>mods.size())return WWMI_RT_MOD_STALE;
    const auto &m=mods[size_t(section.key-1)];if(!(m.flags&WWMI_RT_MOD_ENABLED))return WWMI_RT_MOD_NOT_READY;
    const auto c=std::find_if(m.components.begin(),m.components.end(),[&](const auto &v){return v.component==section.component;});
    if(c==m.components.end()||!c->plan_known||(frame&&c->plan_frame!=frame))return WWMI_RT_MOD_NOT_READY;
    out->struct_size=sizeof(*out);out->abi_version=1;out->load_generation=generation;out->mod_key=m.key;out->mod_revision=m.geometry_revision;
    out->plan_frame=c->plan_frame;out->component=c->component;out->selection_flags=geometry_flags(*c);return WWMI_RT_MOD_OK;
}

extern "C" WWMI_RT_EXPORT uint32_t WWMI_RtModReadSurfaceCatalog_v1(void *out,uint64_t capacity,uint64_t *required){
    if(!required)return WWMI_RT_MOD_BAD_INPUT;try{std::lock_guard lock(source_mutex);rebuild();*required=surface_catalog.size();
        if(!out||capacity<surface_catalog.size())return WWMI_RT_MOD_MORE;std::memcpy(out,surface_catalog.data(),surface_catalog.size());return WWMI_RT_MOD_OK;
    }catch(...){*required=0;return WWMI_RT_MOD_NOT_READY;}
}
