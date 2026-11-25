#pragma once
// rt_pack.hpp also includes this exact header; use the same physical file so
// pragma-once covers both include chains, not the independent local copy.
#include "hsin_asset_layout.hpp"
#include "rt_mod_source.h"
#include "shapekey_position_mode.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
namespace hsin_rt {
class CompletedAuthorInput;
using Matrix=std::array<float,12>;
struct ResourceKey {
    std::uint64_t object=0,creation=0;
    bool operator==(const ResourceKey &) const=default;
    explicit operator bool() const {return object&&creation;}
};
struct Recording {std::uint64_t list=0,creation=0,generation=0;bool operator==(const Recording &) const=default;};
struct NativeShapeRead {
    ResourceKey resource;
    std::uint64_t view_offset=0,view_bytes=0,offset=0,bytes=0;
    unsigned first_vertex=0,vertex_count=0;
};
struct SkinSample {
    Recording recording;std::shared_ptr<const NativeProfile> profile;
    std::uint64_t order=0;unsigned component=0;
    ResourceKey output,content,palette_resource;
    std::uint64_t palette_base=0,palette_offset=0,palette_bytes=0;
    std::uint64_t output_base=0,output_offset=0,output_bytes=0,content_offset=0;
    std::uint32_t content_hash=0,native_delta_flags=0;
    bool content_confirmed=false,extra_position_input=false;
    float scale=1;std::vector<Matrix> palette;
    NativeShapeRead native_shape; // actual native skin8_extra t3; never an author provider by itself
};
struct GeometryInput {ResourceKey vertex;std::uint64_t offset=0,bytes=0;};
struct BlasPoseInput {Recording recording;std::vector<GeometryInput> geometries;};
struct FinalPosition {
    unsigned mode=WWMI_RT_POSITION_UNKNOWN;ResourceKey resource;
    std::uint64_t address=0,bytes=0;unsigned first_vertex=0,vertex_count=0;std::shared_ptr<void> hold;
    std::shared_ptr<const CompletedAuthorInput> completed_author;
};
struct Pose {
    Recording recording;std::shared_ptr<const ModPack> pack;ResourceKey content;
    std::vector<GeometryInput> source_vertices;std::uint64_t content_offset=0;
    std::vector<Matrix> rows;std::vector<float> scales;std::vector<std::uint64_t> source_orders;
    std::vector<bool> native_extra_input;std::vector<std::uint32_t> native_delta_flags;
    std::vector<FinalPosition> final_positions;
};
enum class PoseResult {complete,wrong_recording,missing_producer,wrong_content,short_palette,different_pose,
    missing_weighted_bone,invalid_matrix,mod_shapekey_missing,native_extra_unresolved,mod_extra_missing};
struct Assembly {PoseResult result;std::unique_ptr<Pose> pose;};
// The samples are exact native position-write receipts. The palette itself is
// copied into Pose, while these small proofs survive the producer-list reset.
struct ComponentAssembly {
    Assembly assembly{PoseResult::missing_producer,{}};std::vector<SkinSample> sources;unsigned missing_component=UINT32_MAX;
    // Optional window-only diagnostics, not proof that two samples are paired.
    unsigned local_same=UINT32_MAX,local_other=UINT32_MAX,other_recordings=UINT32_MAX;
};
class PoseInputs {
    Recording recording_{};std::vector<SkinSample> samples_;
    static bool overlap(std::uint64_t a,std::uint64_t n,std::uint64_t b,std::uint64_t m){return n&&m&&(a<=b?b-a<n:a-b<m);}
public:
    Recording recording()const noexcept{return recording_;}
    const std::vector<SkinSample> &samples()const noexcept{return samples_;}
    std::size_t mutable_sample_count()const noexcept{return samples_.size();}
    void reset(Recording recording){recording_=recording;samples_.clear();}
    void written(ResourceKey resource,std::uint64_t offset,std::uint64_t bytes){std::erase_if(samples_,[&](const auto &s){return s.output==resource&&overlap(s.output_offset,s.output_bytes,offset,bytes);});}
    void destroyed(ResourceKey resource){std::erase_if(samples_,[&](const auto &s){return s.output==resource||s.content==resource;});}
    bool add(SkinSample sample){
        if(sample.recording!=recording_)return false;written(sample.output,sample.output_offset,sample.output_bytes);
        if(!sample.profile||!sample.output||!sample.content||!sample.content_confirmed||sample.content_hash!=sample.profile->content_hash||
            sample.component>=sample.profile->components.size()||sample.output_bytes!=12ull*sample.profile->components[sample.component].count)return false;
        samples_.push_back(std::move(sample));return true;
    }
    void count_component_candidates(const std::shared_ptr<const NativeProfile> &profile,unsigned component,ResourceKey content,
        std::uint64_t content_offset,ResourceKey target,unsigned &same,unsigned &other) const {
        for(const auto &s:samples_)if(s.profile==profile&&s.component==component&&s.content==content&&s.content_offset==content_offset){
            auto &count=s.output==target?same:other;if(count<UINT32_MAX-1)++count;
        }
    }
    const SkinSample *draw_sample(std::uint64_t base,unsigned first,unsigned count) const {
        const SkinSample *found=nullptr;
        for(const auto &s:samples_){const auto &n=s.profile->components[s.component];
            if(n.index_first!=first||n.index_count!=count||s.output_offset<12ull*n.first)continue;
            const auto offset=s.output_offset-12ull*n.first;if(!s.output_base||s.output_base>UINT64_MAX-offset||s.output_base+offset!=base)continue;
            if(found&&(found->profile!=s.profile||found->output!=s.output||found->output_offset!=s.output_offset||found->content!=s.content||found->content_offset!=s.content_offset))return nullptr;found=&s;
        }
        return found;
    }
    // The ordinary dispatch path requires every provider in the same SRV.
    // An actual native BLAS snapshot may instead prove per-component rings in
    // one recording. Only the deferred-authorization cohort candidate may
    // supply exact retained provider snapshots with their own writer epochs.
    ComponentAssembly assemble_previous_component(std::shared_ptr<const ModPack> pack,ResourceKey output,
        std::uint64_t view_offset,unsigned component,ResourceKey content,std::uint64_t content_offset,
        const BlasPoseInput *native_link=nullptr,const std::vector<const SkinSample *> *retained_providers=nullptr) const {
        ComponentAssembly result;
        if(!pack||!pack->native||component>=pack->components.size()||!output||!content||
            pack->components.size()!=pack->native->components.size())return result;
        if(native_link&&(native_link->recording!=recording_||native_link->geometries.size()!=pack->components.size()||
            native_link->geometries[component].vertex!=output||native_link->geometries[component].offset!=view_offset||
            native_link->geometries[component].bytes!=pack->native->bytes()))return result;
        if(retained_providers&&(!native_link||retained_providers->size()!=pack->components.size()||(*retained_providers)[component]))return result;
        const auto &target=pack->components[component];
        if(!target.index_count||!target.vertex_count||target.vertex_first>pack->vertices||
            target.vertex_count>pack->vertices-target.vertex_first||!pack->canonical_bones||
            pack->shapekey_vertices||target.extra_position_input||!target.rest_skinning_known||target.bone_map.empty())return result;
        auto select=[&](unsigned index)->const SkinSample *{
            if(index>=pack->components.size())return nullptr;
            const auto &n=pack->components[index].native;const auto first=12ull*n.first;
            const auto base=native_link?native_link->geometries[index].offset:view_offset;
            const auto vertex=native_link?native_link->geometries[index].vertex:output;
            if(!vertex||(native_link&&native_link->geometries[index].bytes!=pack->native->bytes())||base>UINT64_MAX-first)return nullptr;
            const auto offset=base+first;
            const auto matches=[&](const SkinSample &s){return s.profile==pack->native&&s.component==index&&s.output==vertex&&
                s.output_offset==offset&&s.output_bytes==12ull*n.count&&s.content==content&&s.content_offset==content_offset;};
            if(const auto *old=retained_providers?(*retained_providers)[index]:nullptr)
                return old->recording!=recording_&&matches(*old)?old:nullptr;
            const SkinSample *found=nullptr;
            for(const auto &s:samples_){
                if(s.recording!=recording_||!matches(s))continue;
                if(found)return nullptr;found=&s;
            }
            return found;
        };
        const auto *selected=select(component);if(!selected){result.missing_component=component;return result;}
        if(!std::isfinite(selected->scale)){result.assembly.result=PoseResult::invalid_matrix;return result;}
        if((selected->extra_position_input||selected->native_delta_flags)&&!target.mod_author_shared){
            result.assembly.result=PoseResult::native_extra_unresolved;return result;
        }
        std::vector<std::pair<unsigned,const SkinSample *>> providers{{component,selected}};
        for(const auto &dep:target.dependencies){
            const auto *other=select(dep.provider);if(!other){result.missing_component=dep.provider;return result;}if(dep.anchors.empty())return result;
            for(const auto &a:dep.anchors){
                if(a[0]>=selected->palette.size()||a[1]>=other->palette.size()){
                    result.assembly.result=PoseResult::short_palette;return result;
                }
                if(selected->palette[a[0]]!=other->palette[a[1]]){
                    result.assembly.result=PoseResult::different_pose;return result;
                }
            }
            providers.push_back({dep.provider,other});
        }
        // This pose is recorded into the target's list. The native proofs
        // below preserve the actual, potentially distinct writer of each input.
        auto pose=std::make_unique<Pose>();pose->recording=recording_;pose->pack=pack;pose->content=content;pose->content_offset=content_offset;
        const auto count=pack->components.size();pose->source_vertices.resize(count);pose->source_orders.resize(count);
        pose->scales.resize(count);pose->native_extra_input.resize(count);pose->native_delta_flags.resize(count);pose->final_positions.resize(count);
        constexpr Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0};pose->rows.assign(count*pack->canonical_bones,identity);
        auto *rows=pose->rows.data()+component*pack->canonical_bones;std::vector<bool> present(pack->canonical_bones);
        for(const auto &[index,s]:providers){
            const auto source_first=12ull*pack->components[index].native.first;
            if(s->output_offset<source_first)return result;
            pose->source_vertices[index]={s->output,s->output_offset-source_first,pack->native->bytes()};pose->source_orders[index]=s->order;
            for(const auto &p:pack->components[index].bone_map){
                if(p.source>=s->palette.size()||p.canonical>=pack->canonical_bones){result.assembly.result=PoseResult::short_palette;return result;}
                for(float v:s->palette[p.source])if(!std::isfinite(v)){result.assembly.result=PoseResult::invalid_matrix;return result;}
                if(index!=component&&(present[p.canonical]||std::find(target.used.begin(),target.used.end(),p.canonical)==target.used.end()))continue;
                rows[p.canonical]=s->palette[p.source];present[p.canonical]=true;
            }
            SkinSample proof{};proof.recording=s->recording;proof.order=s->order;proof.component=index;proof.output=s->output;
            proof.output_offset=s->output_offset;proof.output_bytes=s->output_bytes;proof.content=s->content;proof.content_offset=s->content_offset;
            result.sources.push_back(std::move(proof));
        }
        for(unsigned bone:target.used)if(bone>=present.size()||!present[bone]){result.assembly.result=PoseResult::missing_weighted_bone;return result;}
        pose->scales[component]=selected->scale;pose->native_extra_input[component]=selected->extra_position_input;pose->native_delta_flags[component]=selected->native_delta_flags;
        result.assembly={PoseResult::complete,std::move(pose)};return result;
    }
    // Identity has no Mod enable/selection or deformation precondition.
    bool identify_native(Recording recording,const NativeProfile &profile,const std::vector<std::uint64_t> &bases,BlasPoseInput &input) const {
        if(recording!=recording_||bases.size()!=profile.components.size())return false;
        input={recording,{}};input.geometries.resize(bases.size());const SkinSample *content=nullptr;
        for(unsigned c=0;c<bases.size();++c){bool found=false;const auto component_offset=12ull*profile.components[c].first;
            for(const auto &s:samples_){if(s.profile.get()!=&profile||s.component!=c||!s.output_base||s.output_offset<component_offset)continue;
                const auto offset=s.output_offset-component_offset;if(s.output_base>UINT64_MAX-offset||s.output_base+offset!=bases[c])continue;
                const GeometryInput candidate{s.output,offset,profile.bytes()};
                if(found&&(input.geometries[c].vertex!=candidate.vertex||input.geometries[c].offset!=candidate.offset))return false;
                if(content&&(s.content!=content->content||s.content_offset!=content->content_offset))return false;
                input.geometries[c]=candidate;content=&s;found=true;
            }
            if(!found)return false;
        }
        return true;
    }
    Assembly assemble_native(Recording recording,const std::vector<std::uint64_t> &bases,std::shared_ptr<const ModPack> pack,std::vector<FinalPosition> final_positions={}) const {
        if(recording!=recording_)return {PoseResult::wrong_recording,{}};BlasPoseInput input;
        if(!pack||!identify_native(recording,*pack->native,bases,input))return {PoseResult::missing_producer,{}};
        return assemble(input,std::move(pack),std::move(final_positions));
    }
    // A caller may use this only for all the exact writers already paired
    // in one native BLAS snapshot. Disjoint WWMI merge windows can then be
    // concatenated without inventing cross-ring equality anchors.
    Assembly assemble_raster(const BlasPoseInput &input,std::shared_ptr<const ModPack> pack) const {
        if(!pack||!pack->native||input.recording!=recording_||input.geometries.empty()||
           input.geometries.size()!=pack->components.size())return {PoseResult::missing_producer,{}};
        auto out=std::make_unique<Pose>();out->pack=std::move(pack);out->recording=recording_;
        out->source_vertices=input.geometries;
        const auto count=out->pack->components.size(),rows=std::size_t(out->pack->canonical_bones);
        out->scales.resize(count);out->source_orders.resize(count);out->native_extra_input.resize(count);
        out->native_delta_flags.resize(count);out->final_positions.resize(count);out->rows.resize(rows);
        std::vector<bool> covered(rows);
        for(unsigned i=0;i<count;++i){
            const auto &layout=input.geometries[i];const auto &part=out->pack->components[i];
            if(!layout.vertex||layout.bytes!=out->pack->native->bytes()||
               layout.offset>UINT64_MAX-layout.bytes)return {PoseResult::missing_producer,{}};
            const auto offset=layout.offset+12ull*part.native.first;
            const auto sample=std::find_if(samples_.begin(),samples_.end(),[&](const auto &v){
                return v.profile==out->pack->native&&v.component==i&&v.recording==recording_&&
                    v.output==layout.vertex&&v.output_offset==offset&&v.output_bytes==12ull*part.native.count;
            });
            if(sample==samples_.end())return {PoseResult::missing_producer,{}};
            if(!i){out->content=sample->content;out->content_offset=sample->content_offset;}
            else if(sample->content!=out->content||sample->content_offset!=out->content_offset)
                return {PoseResult::wrong_content,{}};
            if(!std::isfinite(sample->scale))return {PoseResult::invalid_matrix,{}};
            if(i&&sample->scale!=out->scales[0])return {PoseResult::different_pose,{}};
            out->scales[i]=sample->scale;out->source_orders[i]=sample->order;
            out->native_extra_input[i]=sample->extra_position_input;out->native_delta_flags[i]=sample->native_delta_flags;
            for(const auto &pair:part.bone_map){
                if(pair.source>=sample->palette.size())return {PoseResult::short_palette,{}};
                if(pair.canonical>=rows||covered[pair.canonical])return {PoseResult::missing_weighted_bone,{}};
                for(float v:sample->palette[pair.source])if(!std::isfinite(v))return {PoseResult::invalid_matrix,{}};
                out->rows[pair.canonical]=sample->palette[pair.source];covered[pair.canonical]=true;
            }
        }
        if(!std::all_of(covered.begin(),covered.end(),[](bool v){return v;}))return {PoseResult::missing_weighted_bone,{}};
        return {PoseResult::complete,std::move(out)};
    }
    Assembly assemble(const BlasPoseInput &input,std::shared_ptr<const ModPack> pack,std::vector<FinalPosition> final_positions={}) const {
        if(input.recording!=recording_)return {PoseResult::wrong_recording,{}};
        if(!pack||input.geometries.size()!=pack->components.size())return {PoseResult::missing_producer,{}};
        const auto count=pack->components.size();if(final_positions.empty())final_positions.resize(count);
        if(final_positions.size()!=count)return {PoseResult::missing_producer,{}};std::vector<const SkinSample *> chosen(count);
        for(unsigned c=0;c<count;++c){const auto &g=input.geometries[c];const auto &component=pack->components[c];
            if(!g.vertex||g.bytes<pack->native->bytes()||g.offset>UINT64_MAX-pack->native->bytes())return {PoseResult::missing_producer,{}};
            const auto offset=g.offset+12ull*component.native.first;
            const auto it=std::find_if(samples_.rbegin(),samples_.rend(),[&](const auto &s){return s.profile==pack->native&&s.component==c&&s.output==g.vertex&&s.output_offset==offset;});
            if(it==samples_.rend())return {PoseResult::missing_producer,{}};chosen[c]=&*it;
            if(c&&(it->content!=chosen[0]->content||it->content_offset!=chosen[0]->content_offset))return {PoseResult::wrong_content,{}};
            if(!component.index_count)continue;
            const auto &final=final_positions[c];const bool resolved=final.mode==WWMI_RT_POSITION_FINAL;
            const bool shape=final.mode==position_author_shape_delta_float6;
            if(resolved){
                if(!final.resource||!final.hold||!final.address||component.vertex_first<final.first_vertex||component.vertex_first-final.first_vertex>final.vertex_count||
                    component.vertex_count>final.vertex_count-(component.vertex_first-final.first_vertex)||final.bytes!=12ull*final.vertex_count)return {PoseResult::missing_producer,{}};
            }else{
                if(pack->shapekey_vertices&&!shape)return {PoseResult::mod_shapekey_missing,{}};
                if(shape&&(!pack->shapekey_vertices||!component.mod_author_shared||!final.resource||!final.hold||!final.address||
                    component.vertex_first<final.first_vertex||component.vertex_first-final.first_vertex>final.vertex_count||
                    component.vertex_count>final.vertex_count-(component.vertex_first-final.first_vertex)||final.bytes!=24ull*final.vertex_count))
                    return {PoseResult::mod_shapekey_missing,{}};
                if(component.extra_position_input)return {PoseResult::mod_extra_missing,{}};
                // Explicit author-geometry policy: use the Mod's own authored
                // Position/Blend/BoneMap, shared by raster and RT. Native extra
                // facts are retained; this is NOT REST_VERIFIED or a zero-delta
                // assertion about the native topology/material.
                if((it->extra_position_input||it->native_delta_flags)&&final.mode!=WWMI_RT_POSITION_REST_VERIFIED&&!component.mod_author_shared)
                    return {PoseResult::native_extra_unresolved,{}};
                if(component.bone_map.empty()||!component.rest_skinning_known)return {PoseResult::missing_weighted_bone,{}};
            }
            if(!resolved&&!std::isfinite(it->scale))return {PoseResult::invalid_matrix,{}};
            // Only referenced rows are matrices; captured padding can be non-finite.
            if(!resolved)for(const auto &pair:component.bone_map){if(pair.source>=it->palette.size())return {PoseResult::short_palette,{}};
                for(float value:it->palette[pair.source])if(!std::isfinite(value))return {PoseResult::invalid_matrix,{}};
            }
        }
        // A final-position provider may still supply a native palette to another
        // component's pending merge; validate every referenced provider row.
        for(unsigned c=0;c<count;++c)if(pack->components[c].index_count&&final_positions[c].mode!=WWMI_RT_POSITION_FINAL)for(const auto &dep:pack->components[c].dependencies){
            for(const auto &pair:pack->components[dep.provider].bone_map){if(pair.source>=chosen[dep.provider]->palette.size())return {PoseResult::short_palette,{}};
                for(float value:chosen[dep.provider]->palette[pair.source])if(!std::isfinite(value))return {PoseResult::invalid_matrix,{}};
            }
            for(const auto &pair:dep.anchors)if(chosen[c]->palette[pair[0]]!=chosen[dep.provider]->palette[pair[1]])return {PoseResult::different_pose,{}};
        }
        auto result=std::make_unique<Pose>();result->recording=recording_;result->pack=std::move(pack);result->source_vertices=input.geometries;
        result->content=chosen[0]->content;result->content_offset=chosen[0]->content_offset;result->final_positions=std::move(final_positions);
        result->scales.resize(count);result->source_orders.resize(count);result->native_extra_input.resize(count);result->native_delta_flags.resize(count);
        constexpr Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0};result->rows.assign(count*result->pack->canonical_bones,identity);
        for(unsigned c=0;c<count;++c){result->source_orders[c]=chosen[c]->order;result->scales[c]=chosen[c]->scale;result->native_extra_input[c]=chosen[c]->extra_position_input;result->native_delta_flags[c]=chosen[c]->native_delta_flags;
            if(!result->pack->components[c].index_count||result->final_positions[c].mode==WWMI_RT_POSITION_FINAL)continue;
            auto *rows=result->rows.data()+c*result->pack->canonical_bones;std::vector<bool> present(result->pack->canonical_bones);
            auto import=[&](unsigned provider,bool only_missing){for(const auto &pair:result->pack->components[provider].bone_map){
                if(only_missing&&(present[pair.canonical]||std::find(result->pack->components[c].used.begin(),result->pack->components[c].used.end(),pair.canonical)==result->pack->components[c].used.end()))continue;
                rows[pair.canonical]=chosen[provider]->palette[pair.source];present[pair.canonical]=true;
            }};
            import(c,false);for(const auto &dep:result->pack->components[c].dependencies)import(dep.provider,true);
            for(unsigned bone:result->pack->components[c].used)if(!present[bone])return {PoseResult::missing_weighted_bone,{}};
        }
        return {PoseResult::complete,std::move(result)};
    }
};
}
