#pragma once
#include "hsin_material.hpp"
#include "wwmi_rt_hit_group_graph.hpp"
#include "native_raygen_domain.hpp"
#include <array>
#include <cwchar>
#include <optional>
#include <unordered_map>

namespace hsin_rt {
// Payload-level research proofs require exact bytecode, independently of
// live export/AS reflection. Release builds do not seed captured programs.
enum class RayPayload { lumen_minimal, default_payload, material_closest_hit };
inline const char *ray_payload_label(RayPayload payload){
    switch(payload){
    case RayPayload::lumen_minimal:return "lumen_minimal";
    case RayPayload::default_payload:return "default";
    case RayPayload::material_closest_hit:return "material_closest_hit";
    }
    return "unknown";
}
struct RaygenName {
    const wchar_t *alias;
    const char *entry;
    uint32_t as_register;
    RayPayload payload;
    unsigned ray_contribution=0;
};
inline constexpr RaygenName known_raygens[]{
    {L"RayGen_19c05a81b832ce34","LumenReflectionHardwareRayTracingRGS",14,RayPayload::lumen_minimal},
    {L"RayGen_6e0e9af8ba6c971b","LumenReflectionHardwareRayTracingRGS",18,RayPayload::lumen_minimal},
    {L"RayGen_b015ed8b6274ac05","LumenReflectionHardwareRayTracingRGS",18,RayPayload::lumen_minimal},
    {L"RayGen_f1c3b297855d3694","LumenReflectionHardwareRayTracingRGS",14,RayPayload::lumen_minimal},
    {L"RayGen_86aa662fab993e04","OcclusionMainRG",0,RayPayload::default_payload},
    {L"RayGen_7cd593dfd13f1bd1","IntersectionMainRG",0,RayPayload::default_payload},
    {L"RayGen_175b6a45890be187","OcclusionRGS",4,RayPayload::material_closest_hit,1},
    {L"RayGen_7034c5cdd6ae514f","OcclusionRGS",4,RayPayload::material_closest_hit,1},
    {L"RayGen_b3b7c1d108b3e776","OcclusionRGS",4,RayPayload::material_closest_hit,1},
    {L"RayGen_fb9ca5a115cc7dc6","OcclusionRGS",4,RayPayload::material_closest_hit,1},
    {L"RayGen_68a11c5ea87278dd","OcclusionRGS",4,RayPayload::material_closest_hit,1},
    {L"RayGen_eb8d764f0917ea59","OcclusionRGS",4,RayPayload::material_closest_hit,1},
    {L"RayGen_8c622780c1eadf7b","OcclusionRGS",4,RayPayload::material_closest_hit,1},
    {L"RayGen_24f6c4c1cd211904","OcclusionRGS",4,RayPayload::material_closest_hit,1},
    {L"RayGen_8c7774f2f8b39ab3","OcclusionRGS",4,RayPayload::material_closest_hit,1},
    {L"RayGen_91987cd6ca87cf6b","OcclusionRGS",4,RayPayload::material_closest_hit,1}
};
inline constexpr wchar_t occlusion_miss_alias[]=L"Miss_f744b0e05ee97e37";

class RaygenIdentity {
    using Code=std::vector<unsigned char>;
    struct Entry {
        std::shared_ptr<const Code> code;
        std::optional<ShaderId> identifier;
        bool ambiguous=false;
    };
    using Programs=std::array<Entry,std::size(known_raygens)>;
    struct Pipeline {Programs programs;unsigned hit_group_bits=0;};
    std::unordered_map<std::uint64_t,Pipeline> pipelines_;
    struct Native {
        std::shared_ptr<const NativeRaygenDomain> domain;
        unsigned hit_group_bits=0;
        bool has_live_raygen=false;
        unsigned fixed_offset=UINT32_MAX;
        std::optional<size_t> single_pin;
    };
    std::unordered_map<std::uint64_t,Native> native_;
    unsigned native_live_=0;
    std::array<std::shared_ptr<const Code>,std::size(known_raygens)> pins_;
    std::unordered_map<std::uint64_t,Entry> occlusion_misses_;
    std::shared_ptr<const Code> occlusion_miss_pin_;
    bool has_pins_=false;
    static std::optional<unsigned> as_slot(const NativeRaygenExport &e){
        if(!e.pin)return e.reflected_as;
        const auto exact=known_raygens[*e.pin].as_register;
        return !e.reflected_as||*e.reflected_as==exact?std::optional<unsigned>{exact}:std::nullopt;
    }
    static void add(Entry &entry,const std::shared_ptr<const Code> &code){
        if(!code||entry.ambiguous)return;
        if(!entry.code)entry.code=code;
        else if(entry.code!=code&&*entry.code!=*code){entry.code.reset();entry.ambiguous=true;}
    }
    static unsigned native_hit_group_bits(ID3D12StateObject *state){
        using namespace wwmi::rt_hit_group_graph;
        Graph_v1 graph{};
        UINT bytes=sizeof(graph);
        if(FAILED(state->GetPrivateData(private_data_guid,&bytes,&graph))||bytes!=sizeof(graph)||graph.size!=sizeof(graph)||
           graph.version!=version_v1||((graph.local_bits|graph.reachable_bits)&~known_bits))return 0;
        return graph.reachable_bits;
    }
public:
    void pin(unsigned i,Code bytes){
        pins_[i]=std::make_shared<const Code>(std::move(bytes));
        has_pins_=true;
    }
    void pin_occlusion_miss(Code bytes){
        occlusion_miss_pin_=std::make_shared<const Code>(std::move(bytes));
        has_pins_=true;
    }
    bool has_pins()const noexcept{return has_pins_;}
    bool has_pipeline(std::uint64_t pipeline)const{
        if(const auto it=native_.find(pipeline);it!=native_.end())return it->second.has_live_raygen;
        return pipelines_.contains(pipeline);
    }
    bool has_programs()const{return native_live_||!pipelines_.empty();}
    // Called from the existing init_pipeline under the device Runtime lock.
    // Exact RDAT/code is a collection fact, even if a collection cannot yet
    // return an identifier. Identifiers belong only to this state object.
    void init(api::pipeline state,unsigned count,const api::pipeline_subobject *subobjects){
        Programs programs{};
        Entry miss{};
        bool imports_library=false;
        for(unsigned i=0;i<count;++i){
            const auto &s=subobjects[i];
            if(s.type!=api::pipeline_subobject_type::libraries)continue;
            imports_library|=s.count!=0;
            const auto *parents=static_cast<const api::pipeline *>(s.data);
            for(unsigned j=0;j<s.count;++j){
                if(const auto parent=pipelines_.find(parents[j].handle);parent!=pipelines_.end()){
                    for(size_t k=0;k<programs.size();++k){
                        const auto &source=parent->second.programs[k];
                        if(source.ambiguous){programs[k].ambiguous=true;programs[k].code.reset();}
                        else add(programs[k],source.code);
                    }
                }
                if(const auto parent=occlusion_misses_.find(parents[j].handle);parent!=occlusion_misses_.end()){
                    if(parent->second.ambiguous){miss.ambiguous=true;miss.code.reset();}
                    else add(miss,parent->second.code);
                }
            }
        }
        // The saved native graph's target raygen collections each have one
        // RDAT raygen. Don't correlate multiple variants in one unproven node.
        const api::shader_desc *local=nullptr;
        bool multiple=false;
        for(unsigned i=0;i<count;++i){
            const auto &s=subobjects[i];
            if(s.type==api::pipeline_subobject_type::miss_shader&&occlusion_miss_pin_){
                const auto *shaders=static_cast<const api::shader_desc *>(s.data);
                for(unsigned j=0;j<s.count;++j){
                    const auto &m=shaders[j];
                    if(!m.entry_point||std::strcmp(m.entry_point,"DefaultPayloadMS"))continue;
                    if(m.code&&occlusion_miss_pin_->size()==m.code_size&&
                       !std::memcmp(occlusion_miss_pin_->data(),m.code,m.code_size))add(miss,occlusion_miss_pin_);
                    else{miss.ambiguous=true;miss.code.reset();}
                }
            }
            if(s.type!=api::pipeline_subobject_type::raygen_shader)continue;
            if(s.count==1&&!local)local=static_cast<const api::shader_desc *>(s.data);
            else if(s.count)multiple=true;
        }
        if(imports_library||multiple||!local||!local->entry_point||!local->code||!local->code_size)local=nullptr;
        destroy(state);
        if(!state.handle)return;
        bool retained=false;
        for(size_t k=0;k<programs.size();++k){
            auto &p=programs[k];
            const auto &name=known_raygens[k];
            const auto &pin=pins_[k];
            const bool candidate=local&&pin&&pin->size()==local->code_size&&
                !std::strcmp(name.entry,local->entry_point)&&!std::memcmp(pin->data(),local->code,local->code_size);
            if(candidate)add(p,pin);
            retained|=bool(p.code)||p.ambiguous;
        }
        const bool miss_retained=bool(miss.code)||miss.ambiguous;
        if(!retained&&!miss_retained)return;
        auto *native=reinterpret_cast<ID3D12StateObject *>(state.handle);
        ComPtr<ID3D12StateObjectProperties> properties;
        if(SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&properties)))){
            for(size_t k=0;k<programs.size();++k){
                auto &p=programs[k];
                if(!p.code||p.ambiguous)continue;
                if(const auto *identifier=properties->GetShaderIdentifier(known_raygens[k].alias)){
                    p.identifier.emplace();
                    std::memcpy(p.identifier->data(),identifier,sizeof(ShaderId));
                }
            }
            if(miss.code&&!miss.ambiguous)if(const auto *identifier=properties->GetShaderIdentifier(occlusion_miss_alias)){
                miss.identifier.emplace();
                std::memcpy(miss.identifier->data(),identifier,sizeof(ShaderId));
            }
        }
        // Group completeness is supplied by this actual successful native
        // state. A child collection's program bytes may be inherited, its
        // group completeness cannot substitute for the final state graph.
        if(retained)pipelines_.emplace(state.handle,Pipeline{std::move(programs),native_hit_group_bits(native)});
        if(miss_retained)occlusion_misses_.emplace(state.handle,std::move(miss));
    }
    struct NativeInitSnapshot {
        std::vector<std::pair<std::uint64_t,std::shared_ptr<const NativeRaygenDomain>>> parents;
        std::vector<std::pair<std::uint64_t,std::uint64_t>> root_gens;
        std::vector<std::pair<std::uint64_t,unsigned>> local_bytes;
        std::array<std::shared_ptr<const Code>,std::size(known_raygens)> pins{};
        std::shared_ptr<const Code> occlusion_miss_pin;
        bool has_pins=false;
    };
    struct PreparedNative {
        std::uint64_t state=0;
        std::shared_ptr<const NativeRaygenDomain> domain;
        unsigned hit_group_bits=0;
        bool has_live_raygen=false;
        unsigned fixed_offset=UINT32_MAX;
        std::optional<size_t> single_pin;
    };
    template<class RootGeneration,class LocalArgumentBytes>
    NativeInitSnapshot snapshot_native(const WWMI_RtNativeStateObject_v1 &view,
                                       RootGeneration root_generation,
                                       LocalArgumentBytes local_argument_bytes)const{
        NativeInitSnapshot snap;
        if(view.add_to_base){
            if(const auto found=native_.find(view.add_to_base);found!=native_.end())
                snap.parents.emplace_back(view.add_to_base,found->second.domain);
        }
        if(const auto *raw_desc=reinterpret_cast<const D3D12_STATE_OBJECT_DESC *>(view.desc);
           raw_desc&&raw_desc->pSubobjects){
            const auto &desc=*raw_desc;
            for(unsigned i=0;i<desc.NumSubobjects;++i){
                const auto &s=desc.pSubobjects[i];
                if(!s.pDesc)continue;
                switch(s.Type){
                case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
                    const auto &p=*static_cast<const D3D12_EXISTING_COLLECTION_DESC *>(s.pDesc);
                    const auto parent=reinterpret_cast<std::uint64_t>(p.pExistingCollection);
                    if(const auto found=native_.find(parent);found!=native_.end())
                        snap.parents.emplace_back(parent,found->second.domain);
                    break;
                }
                case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE: {
                    const auto &p=*static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE *>(s.pDesc);
                    if(p.pGlobalRootSignature){
                        const auto root=reinterpret_cast<std::uint64_t>(p.pGlobalRootSignature);
                        if(const auto gen=root_generation(root))
                            snap.root_gens.emplace_back(root,gen);
                    }
                    break;
                }
                case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
                    const auto &p=*static_cast<const D3D12_LOCAL_ROOT_SIGNATURE *>(s.pDesc);
                    if(p.pLocalRootSignature){
                        const auto root=reinterpret_cast<std::uint64_t>(p.pLocalRootSignature);
                        if(const auto bytes=local_argument_bytes(root))
                            snap.local_bytes.emplace_back(root,*bytes);
                    }
                    break;
                }
                default:break;
                }
            }
        }
        if(has_pins_){
            snap.has_pins=true;
            snap.pins=pins_;
            snap.occlusion_miss_pin=occlusion_miss_pin_;
        }
        return snap;
    }
    static std::optional<PreparedNative> build_native_unlocked(
        const WWMI_RtNativeStateObject_v1 &view,unsigned count,
        const api::pipeline_subobject *subobjects,const NativeInitSnapshot &snap){
        auto domain=native_raygen_domain(view,count,subobjects,[&](uint64_t parent)->const NativeRaygenDomain *{
            for(const auto &[id,ptr]:snap.parents)if(id==parent)return ptr.get();
            return nullptr;
        },[&](const api::shader_desc &shader,const wchar_t *alias)->std::optional<size_t>{
            if(!snap.has_pins)return {};
            for(size_t k=0;k<snap.pins.size();++k){
                const auto &p=snap.pins[k];const auto &name=known_raygens[k];
                if(p&&p->size()==shader.code_size&&!std::wcscmp(alias,name.alias)&&
                   !std::strcmp(shader.entry_point,name.entry)&&!std::memcmp(p->data(),shader.code,shader.code_size))
                    return k;
            }
            return {};
        },[&](const api::shader_desc &shader,const wchar_t *alias){
            return snap.occlusion_miss_pin&&snap.occlusion_miss_pin->size()==shader.code_size&&
                !std::wcscmp(alias,occlusion_miss_alias)&&!std::strcmp(shader.entry_point,"DefaultPayloadMS")&&
                !std::memcmp(snap.occlusion_miss_pin->data(),shader.code,shader.code_size);
        },[&](uint64_t root)->uint64_t{
            for(const auto &[id,gen]:snap.root_gens)if(id==root)return gen;
            return 0;
        },[&](uint64_t root)->std::optional<unsigned>{
            for(const auto &[id,bytes]:snap.local_bytes)if(id==root)return bytes;
            return {};
        });
        if(!domain)return {};
        auto *state=reinterpret_cast<ID3D12StateObject *>(view.state);
        ComPtr<ID3D12StateObjectProperties> properties;
        if(domain->executable)state->QueryInterface(IID_PPV_ARGS(&properties));
        bool has_live_raygen=false, complete_ids=true, same_program=true, same_slot=true;
        std::optional<unsigned> single_slot;
        std::optional<size_t> first_pin;
        if(properties)for(auto &e:domain->exports)if(e.raygen||e.miss){
            if(const auto *id=properties->GetShaderIdentifier(e.name.c_str())){
                e.identifier.emplace();std::memcpy(e.identifier->data(),id,e.identifier->size());
                has_live_raygen|=e.raygen;
            }
            if(!e.raygen)continue;
            const auto slot=as_slot(e);
            complete_ids&=bool(e.identifier)&&bool(slot);
            if(slot){
                if(single_slot) same_slot&=*single_slot==*slot;
                else single_slot=slot;
            }
            if(e.pin){
                if(first_pin) same_program&=*first_pin==*e.pin;
                else first_pin=e.pin;
            }else same_program=false;
        }
        const auto offset=complete_ids&&same_slot&&single_slot?*single_slot:UINT32_MAX;
        PreparedNative out;
        out.state=view.state;
        out.domain=std::make_shared<const NativeRaygenDomain>(std::move(*domain));
        out.hit_group_bits=native_hit_group_bits(state);
        out.has_live_raygen=has_live_raygen;
        out.fixed_offset=offset;
        out.single_pin=offset!=UINT32_MAX&&same_program?first_pin:std::nullopt;
        return out;
    }
    void commit_native(PreparedNative &&built){
        if(const auto it=native_.find(built.state);it!=native_.end()){
            if(it->second.has_live_raygen)--native_live_;
            native_.erase(it);
        }
        const bool live=built.has_live_raygen;
        const auto [_,added]=native_.emplace(built.state,Native{
            std::move(built.domain),built.hit_group_bits,live,built.fixed_offset,built.single_pin});
        if(added&&live)++native_live_;
    }
    template<class RootGeneration,class LocalArgumentBytes>
    void init_native(const WWMI_RtNativeStateObject_v1 &view,unsigned count,
                     const api::pipeline_subobject *subobjects,RootGeneration root_generation,
                     LocalArgumentBytes local_argument_bytes){
        const auto snap=snapshot_native(view,root_generation,local_argument_bytes);
        if(auto built=build_native_unlocked(view,count,subobjects,snap))
            commit_native(std::move(*built));
    }

    void destroy(api::pipeline state){
        pipelines_.erase(state.handle);occlusion_misses_.erase(state.handle);
        if(const auto it=native_.find(state.handle);it!=native_.end()){
            if(it->second.has_live_raygen)--native_live_;
            native_.erase(it);
        }
    }
    bool matches_occlusion_miss(api::pipeline state,std::span<const std::byte> record)const{
        const auto found=occlusion_misses_.find(state.handle);
        return found!=occlusion_misses_.end()&&found->second.code&&found->second.identifier&&
            record.size()>=sizeof(ShaderId)&&!std::memcmp(record.data(),found->second.identifier->data(),sizeof(ShaderId));
    }
    struct Active {
        const RaygenName *name;std::shared_ptr<const Code> code;unsigned hit_group_bits=0;
        bool known_no_procedural()const{
            using namespace wwmi::rt_hit_group_graph;
            return (hit_group_bits&complete)&&!(hit_group_bits&procedural);
        }
    };
    struct AsDomain {
        unsigned offset;
        // Populated only when every export means exactly the same pinned
        // program. Equivalent AS reads do not select a payload or hit ABI.
        std::optional<Active> same_program;
    };
    struct RecordCandidate {
        std::wstring alias;
        std::string entry;
        ShaderId id{};
        std::optional<unsigned> as_offset;
        bool pinned=false;
    };
    struct RecordCatalog {
        const char *closure="no_native_domain";
        unsigned declared=0;
        std::vector<RecordCandidate> candidates;
    };
    const char *fixed_as_status(api::pipeline state,uint64_t root,uint64_t generation)const{
        const auto it=native_.find(state.handle);
        if(it==native_.end())return "no_native_domain";
        const auto &native=it->second;
        const auto &domain=*native.domain;
        if(!domain.executable)return "not_executable";
        if(domain.conflicting_roots)return "root_conflict";
        if(domain.root!=root)return "root_mismatch";
        if(!generation||domain.root_generation!=generation)return "root_generation";
        if(native.fixed_offset!=UINT32_MAX)return "closed_as_slot";
        bool raygen=false,unresolved=false,unpinned=false;
        for(const auto &e:domain.exports)if(e.raygen){
            raygen=true;unresolved|=!e.identifier;unpinned|=!as_slot(e);
        }
        return !raygen?"no_raygen":unresolved?"identifier_unresolved":
            unpinned?"unproven_as_slot":"mixed_as_slots";
    }
    // Own the final identifier map at the actual native use. The later
    // readback is not looked up through a potentially recycled state handle.
    // Slots are positive per-entry facts; an unproved export adds no slot.
    RecordCatalog record_catalog(api::pipeline state,uint64_t root,uint64_t generation)const{
        RecordCatalog out;
        out.closure=fixed_as_status(state,root,generation);
        if(const auto it=native_.find(state.handle);it!=native_.end()){
            const auto &native=it->second;
            const auto &domain=*native.domain;
            const bool same_root=domain.executable&&!domain.conflicting_roots&&domain.root==root&&
                generation&&domain.root_generation==generation;
            for(const auto &e:domain.exports)if(e.raygen){
                ++out.declared;
                if(!e.identifier)continue;
                RecordCandidate candidate{e.name,e.entry,{},same_root?as_slot(e):std::nullopt,bool(e.pin)};
                std::memcpy(candidate.id.data(),e.identifier->data(),sizeof(candidate.id));
                out.candidates.push_back(std::move(candidate));
            }
            return out;
        }
        // An exact existing SDK pin may still label a selected ID; it
        // cannot supply a native-root/AS proof for a missing native graph.
        if(const auto it=pipelines_.find(state.handle);it!=pipelines_.end())
            for(size_t i=0;i<it->second.programs.size();++i){
                const auto &p=it->second.programs[i];
                if(p.code&&p.identifier&&!p.ambiguous)
                    out.candidates.push_back({known_raygens[i].alias,known_raygens[i].entry,*p.identifier,{},true});
            }
        return out;
    }
    std::optional<AsDomain> fixed_as(api::pipeline state,uint64_t root,uint64_t root_generation)const{
        const auto it=native_.find(state.handle);
        if(it==native_.end()||!root_generation)return {};
        const auto &native=it->second;
        const auto &domain=*native.domain;
        if(!domain.executable||domain.conflicting_roots||domain.root!=root||domain.root_generation!=root_generation)return {};
        if(native.fixed_offset==UINT32_MAX)return {};
        AsDomain as{native.fixed_offset,{}};
        if(native.single_pin){
            const auto i=*native.single_pin;
            as.same_program=Active{&known_raygens[i],pins_[i],native.hit_group_bits};
        }
        return as;
    }
    std::optional<unsigned> reflected_record_as(api::pipeline state,uint64_t root,uint64_t generation,
                                               std::span<const std::byte> record)const{
        const auto it=native_.find(state.handle);
        if(it==native_.end()||!generation||record.size()<sizeof(ShaderId))return {};
        const auto &domain=*it->second.domain;
        if(!domain.executable||domain.conflicting_roots||domain.root!=root||domain.root_generation!=generation)return {};
        const NativeRaygenExport *match=nullptr;
        for(const auto &e:domain.exports)if(e.raygen&&e.identifier&&
           !std::memcmp(record.data(),e.identifier->data(),e.identifier->size())){
            if(match)return {};
            match=&e;
        }
        return match?as_slot(*match):std::nullopt;
    }
    struct OcclusionRecordBytes {
        unsigned raygen,miss;
        bool operator==(const OcclusionRecordBytes &)const=default;
    };
    // Only the captured MainRG domain can reuse its original tables
    // without the caller proving particular identifier bytes. This is a
    // set of compatible possibilities, not a report that Miss executed.
    std::optional<OcclusionRecordBytes> closed_occlusion_records(api::pipeline state,uint64_t root,uint64_t generation)const{
        const auto fixed=fixed_as(state,root,generation);
        if(!fixed||!fixed->same_program||!fixed->same_program->known_no_procedural()||
           std::strcmp(fixed->same_program->name->entry,"OcclusionMainRG"))return {};
        const auto &domain=*native_.find(state.handle)->second.domain;
        if(domain.default_local_association)return {};
        const NativeRaygenExport *rg=nullptr,*ms=nullptr;
        for(const auto &e:domain.exports){
            if(e.raygen){if(rg||!e.identifier||!e.local_argument_bytes)return {};rg=&e;}
            if(e.miss){if(ms||!e.identifier||!e.default_miss||!e.local_argument_bytes)return {};ms=&e;}
        }
        if(!rg||!ms)return {};
        return OcclusionRecordBytes{D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES+*rg->local_argument_bytes,
            D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES+*ms->local_argument_bytes};
    }
    // `record` is the actual RayGenerationShaderRecord, read from its exact
    // live bound view or a proven source copy. This does not look up by VA.
    std::optional<Active> select(api::pipeline state,std::span<const std::byte> record)const{
        if(record.size()<sizeof(ShaderId))return {};
        if(const auto native=native_.find(state.handle);native!=native_.end()){
            std::optional<size_t> match;
            for(const auto &e:native->second.domain->exports)if(e.raygen&&e.identifier&&
                !std::memcmp(record.data(),e.identifier->data(),e.identifier->size())){
                if(!e.pin||(match&&*match!=*e.pin))return {};
                match=e.pin;
            }
            if(!match)return {};
            return Active{&known_raygens[*match],pins_[*match],native->second.hit_group_bits};
        }
        const auto it=pipelines_.find(state.handle);
        if(it==pipelines_.end())return {};
        std::optional<Active> match;
        for(size_t k=0;k<it->second.programs.size();++k){
            const auto &p=it->second.programs[k];
            if(!p.code||!p.identifier||std::memcmp(record.data(),p.identifier->data(),sizeof(ShaderId)))continue;
            if(match)return {};
            match=Active{&known_raygens[k],p.code,it->second.hit_group_bits};
        }
        return match;
    }
};
}
