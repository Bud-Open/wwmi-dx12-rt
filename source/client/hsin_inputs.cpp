#include "hsin_inputs.hpp"
#include "wc_copy_seh.hpp"
#include "rt_skin_known_shaders.hpp"
#include "rt_upload_identity.hpp"
#include <cstdio>
#include <cstring>
#include <iterator>
#include "rt_cpu_profile.hpp"
#include "rt_descriptor_content.h"
extern "C" std::uint32_t crc32c_append(std::uint32_t,const unsigned char *,std::size_t);
namespace hsin_rt {
namespace identity=wwmi::observe::upload_identity;
static std::uint64_t id(api::command_list *c){return reinterpret_cast<std::uint64_t>(c);}
static unsigned native_index_stride(DXGI_FORMAT fmt)noexcept{
    return fmt==DXGI_FORMAT_R32_UINT?4u:(fmt==DXGI_FORMAT_R16_UINT?2u:0u);
}
static const D3D12_RAYTRACING_GEOMETRY_DESC *blas_geom_desc(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc,unsigned i)noexcept{
    if(i>=desc.Inputs.NumDescs)return nullptr;
    if(desc.Inputs.DescsLayout==D3D12_ELEMENTS_LAYOUT_ARRAY)return desc.Inputs.pGeometryDescs?&desc.Inputs.pGeometryDescs[i]:nullptr;
    if(desc.Inputs.DescsLayout==D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS)return desc.Inputs.ppGeometryDescs?desc.Inputs.ppGeometryDescs[i]:nullptr;
    return nullptr;
}
// Generic DXBC PSV0 classifier for UE4 SkinCacheUpdateBatchCS permutations
// (covers 4-weight + MorphTarget / 16-bit bone index / future patch builds
// without relying solely on hardcoded bytecode blobs).
static skin::Kind classify_skin_shader(const void *data,std::size_t size){
    if(const auto exact=skin::classify(data,size);exact!=skin::Kind::none)return exact;
    if(!data||size<64)return skin::Kind::none;
    const auto *bytes=static_cast<const unsigned char *>(data);
    if(std::memcmp(bytes,"DXBC",4)!=0)return skin::Kind::none;
    auto rd32=[&](std::size_t off)->std::uint32_t{
        std::uint32_t v=0;std::memcpy(&v,bytes+off,4);return v;
    };
    const std::uint32_t chunk_count=rd32(28);
    if(!chunk_count||chunk_count>64||32ull+4ull*chunk_count>size)return skin::Kind::none;
    for(std::uint32_t ci=0;ci<chunk_count;++ci){
        const std::uint32_t off=rd32(32+4*ci);
        if(off>size||size-off<8)continue;
        if(std::memcmp(bytes+off,"PSV0",4)!=0)continue;
        const std::uint32_t chunk_sz=rd32(off+4);
        if(chunk_sz<32||chunk_sz>size-(off+8))return skin::Kind::none;
        const std::size_t base=off+8;
        const std::uint32_t info_sz=rd32(base);
        if(info_sz<24||4ull+info_sz+8>chunk_sz)return skin::Kind::none;
        const std::size_t pos=4ull+info_sz;
        const std::uint32_t res_count=rd32(base+pos);
        if(!res_count||res_count>256)return skin::Kind::none;
        const std::uint32_t res_stride=rd32(base+pos+4);
        if(res_stride<16||res_stride>256)return skin::Kind::none;
        const std::uint64_t res_bytes=std::uint64_t(res_count)*res_stride;
        if(pos+8>chunk_sz||res_bytes>chunk_sz-(pos+8))return skin::Kind::none;
        std::uint32_t cbv=0,srv=0,uav=0,other=0;
        for(std::uint32_t ri=0;ri<res_count;++ri){
            const std::size_t rpos=base+pos+8+std::size_t(ri)*res_stride;
            const std::uint32_t rt=rd32(rpos),sp=rd32(rpos+4),lo=rd32(rpos+8),hi=rd32(rpos+12);
            if(sp!=0||hi<lo||hi>=32){++other;continue;}
            const std::uint32_t span=hi-lo+1;
            const std::uint32_t mask=(span>=32?0xffffffffu:((1u<<span)-1u))<<lo;
            if(rt==2)cbv|=mask;
            else if(rt>=3&&rt<=5)srv|=mask;
            else if(rt>=6&&rt<=8)uav|=mask;
            else ++other;
        }
        const std::size_t pos2=pos+8+std::size_t(res_bytes);
        if(pos2+4>chunk_sz)return skin::Kind::none;
        const std::uint32_t st_sz=rd32(base+pos2);
        if(st_sz>chunk_sz-(pos2+4))return skin::Kind::none;
        const char *st=reinterpret_cast<const char *>(bytes+base+pos2+4);
        bool has_entry=false;
        for(std::size_t p=0;p<st_sz;){
            std::size_t e=p;
            while(e<st_sz&&st[e]!='\0')++e;
            if(e-p==22&&std::memcmp(st+p,"SkinCacheUpdateBatchCS",22)==0){has_entry=true;break;}
            p=e+1;
        }
        if(!has_entry)return skin::Kind::none;
        skin::Kind result=skin::Kind::none;
        if(cbv==0x1u&&uav==0x3u&&other==0){
            if(srv==0x3fu)result=skin::Kind::skin8;
            else if(srv==0x7fu)result=skin::Kind::skin8_extra;
        }
        static std::mutex log_mtx;
        static std::vector<std::uint64_t> logged_sigs;
        const std::uint64_t sig=(std::uint64_t(size)<<32)^(std::uint64_t(cbv)<<24)^(std::uint64_t(srv)<<8)^uav^(std::uint64_t(other)<<48);
        bool log_now=false;
        {
            std::lock_guard lk(log_mtx);
            if(logged_sigs.size()<64&&std::find(logged_sigs.begin(),logged_sigs.end(),sig)==logged_sigs.end()){
                logged_sigs.push_back(sig);log_now=true;
            }
        }
        if(log_now){
            char line[256];
            std::snprintf(line,sizeof(line),"RT-SKIN-CLASSIFY %s kind=%u size=%llu cbv=0x%x srv=0x%x uav=0x%x other=%u",
                result!=skin::Kind::none?"psv0_match":"unknown_psv0",unsigned(result),(unsigned long long)size,cbv,srv,uav,other);
            reshade::log::message(reshade::log::level::info,line);
        }
        return result;
    }
    return skin::Kind::none;
}
// Zero-allocation extraction of valid triangle skin-layout geometries from a BLAS build.
// Skips inactive/null DXR slots (IndexCount==0 / VertexCount==0 / VB==0 used by UE4 to
// preserve SBT HitGroupIndex when bVisibleInRayTracing=false) and non-conforming aux slots.
struct ValidBlasGeoms {
    std::array<unsigned,64> idx{};
    unsigned count=0;
    unsigned idx_stride=0;
    unsigned max_n=0;
    bool all_valid=true;
    unsigned first_bad=UINT32_MAX;
};
static bool collect_valid_blas_geoms(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc,ValidBlasGeoms &out)noexcept{
    out={};
    if(desc.Inputs.Type!=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL||!desc.Inputs.NumDescs||desc.Inputs.NumDescs>out.idx.size())return false;
    for(unsigned i=0;i<desc.Inputs.NumDescs;++i){
        const auto *vp=blas_geom_desc(desc,i);
        if(!vp){out.all_valid=false;if(out.first_bad==UINT32_MAX)out.first_bad=i;continue;}
        const auto &v=*vp;
        const unsigned s=native_index_stride(v.Triangles.IndexFormat);
        const bool ok=(v.Type==D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES&&v.Triangles.VertexCount&&v.Triangles.VertexCount<=UINT32_MAX/12&&
            v.Triangles.VertexBuffer.StartAddress&&v.Triangles.VertexBuffer.StrideInBytes==12&&
            v.Triangles.VertexFormat==DXGI_FORMAT_R32G32B32_FLOAT&&s!=0&&!v.Triangles.Transform3x4&&
            v.Triangles.IndexCount&&v.Triangles.IndexBuffer&&(!out.idx_stride||s==out.idx_stride));
        if(!ok){out.all_valid=false;if(out.first_bad==UINT32_MAX)out.first_bad=i;continue;}
        if(!out.idx_stride)out.idx_stride=s;
        out.idx[out.count++]=i;
        if(v.Triangles.VertexCount>out.max_n)out.max_n=v.Triangles.VertexCount;
    }
    return out.count!=0;
}
// The exact structural rejection already used by select. Matching this layout
// is only permission to look for the existing content/pose/resource proof.
static bool native_layout_candidate(const NativeProfile &profile,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc,
    std::vector<unsigned> *mapping=nullptr,std::uint64_t *ib_base_out=nullptr,std::size_t *matched_out=nullptr,bool *all_valid_out=nullptr){
    ValidBlasGeoms valid;
    if(profile.components.empty()||!collect_valid_blas_geoms(desc,valid))return false;
    if(valid.max_n!=profile.vertices)return false;
    std::vector<unsigned> sub_map;
    const bool ok=native_geometry_window_map_subset(
        profile.components.size(),
        [&](auto c){const auto &p=profile.components[c];return NativeIndexWindow{p.component,p.index_first,p.index_count};},
        valid.count,
        [&](auto vi){const auto &v=blas_geom_desc(desc,valid.idx[unsigned(vi)])->Triangles;return std::pair{v.IndexBuffer,v.IndexCount};},
        mapping?&sub_map:nullptr,valid.idx_stride,ib_base_out,matched_out);
    if(!ok)return false;
    if(mapping){
        mapping->assign(sub_map.size(),UINT32_MAX);
        for(std::size_t c=0;c<sub_map.size();++c)
            (*mapping)[c]=sub_map[c]==UINT32_MAX?UINT32_MAX:valid.idx[sub_map[c]];
    }
    if(all_valid_out)*all_valid_out=valid.all_valid;
    return true;
}

bool InputCollector::may_select_blas(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc)const noexcept{
    try{
        const auto native_profiles=registry_->snapshot();
        for(const auto &profile:native_profiles->profiles)if(native_layout_candidate(*profile,desc))return true;
        const auto interests=std::atomic_load(&discovery_interests_);
        if(native_receipts_&&desc.Inputs.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL&&desc.Inputs.NumDescs){
            ValidBlasGeoms valid;
            const bool layout=collect_valid_blas_geoms(desc,valid);
            if(layout){
                for(const auto &i:*interests)if(NativeProfileRegistry::resolve(native_profiles,i).match==NativeProfileRegistry::Match::missing&&
                    native_geometry_window_map_subset(i.components.size(),[&](auto c){return i.components[c];},valid.count,
                        [&](auto vi){const auto &g=blas_geom_desc(desc,valid.idx[unsigned(vi)])->Triangles;return std::pair{g.IndexBuffer,g.IndexCount};},nullptr,valid.idx_stride))return true;
            }
            for(const auto &i:*interests){
                if(NativeProfileRegistry::resolve(native_profiles,i).match!=NativeProfileRegistry::Match::missing)continue;
                unsigned shared_big=0,first_ic=0;
                for(unsigned g=0;g<desc.Inputs.NumDescs;++g){
                    const auto *vp=blas_geom_desc(desc,g);
                    if(!vp||vp->Type!=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES||vp->Triangles.IndexCount<5000)continue;
                    for(const auto &ic:i.components)if(ic.count==vp->Triangles.IndexCount){++shared_big;if(!first_ic)first_ic=ic.count;break;}
                }
                if(shared_big){
                    static std::mutex near_miss_mtx;
                    static std::vector<std::uint64_t> near_miss_seen;
                    const auto *g0=blas_geom_desc(desc,0);
                    const auto *gb=valid.first_bad!=UINT32_MAX?blas_geom_desc(desc,valid.first_bad):nullptr;
                    const auto vfmt=g0?unsigned(g0->Triangles.VertexFormat):0u;
                    const auto ifmt=g0?unsigned(g0->Triangles.IndexFormat):0u;
                    const auto vstr=g0?unsigned(g0->Triangles.VertexBuffer.StrideInBytes):0u;
                    const auto n0=g0?g0->Triangles.VertexCount:0u;
                    const std::uint64_t sig=(std::uint64_t(i.object_guid)<<32)^(std::uint64_t(desc.Inputs.NumDescs)<<20)^(std::uint64_t(valid.count)<<12)^(std::uint64_t(vfmt)<<6)^ifmt^std::uint64_t(first_ic);
                    bool log_now=false;
                    {
                        std::lock_guard lk(near_miss_mtx);
                        if(near_miss_seen.size()<64&&std::find(near_miss_seen.begin(),near_miss_seen.end(),sig)==near_miss_seen.end()){
                            near_miss_seen.push_back(sig);log_now=true;
                        }
                    }
                    if(log_now){
                        char line[512];
                        std::snprintf(line,sizeof(line),"RT-NATIVE-DISCOVERY blas_near_miss guid=%08x intent_comps=%llu blas_descs=%u valid_descs=%u descs_layout=%u layout_ok=%u shared_big=%u sample_ic=%u g0_vtx=%u g0_vfmt=%u g0_vstr=%u g0_ifmt=%u bad_g=%d bad_t=%u bad_vtx=%u bad_vfmt=%u bad_vstr=%u bad_ic=%u bad_ifmt=%u bad_xf=%u",
                            i.object_guid,(unsigned long long)i.components.size(),desc.Inputs.NumDescs,valid.count,unsigned(desc.Inputs.DescsLayout),unsigned(layout),shared_big,first_ic,n0,vfmt,vstr,ifmt,
                            valid.first_bad==UINT32_MAX?-1:int(valid.first_bad),gb?unsigned(gb->Type):0u,gb?gb->Triangles.VertexCount:0u,gb?unsigned(gb->Triangles.VertexFormat):0u,
                            gb?unsigned(gb->Triangles.VertexBuffer.StrideInBytes):0u,gb?gb->Triangles.IndexCount:0u,gb?unsigned(gb->Triangles.IndexFormat):0u,unsigned(gb&&gb->Triangles.Transform3x4!=0));
                        reshade::log::message(reshade::log::level::info,line);
                    }
                }
            }
        }
    }catch(...){}
    return false;
}
static std::uint64_t gpu_va_range_end(std::uint64_t base,std::uint64_t bytes)noexcept{
    return bytes>UINT64_MAX-base?UINT64_MAX:base+bytes;
}
std::vector<InputCollector::VaEntry>::iterator InputCollector::location_of(const Resource &r){
    return std::lower_bound(locations_.begin(),locations_.end(),&r,[](const VaEntry &a,const Resource *b){
        return a.resource->base<b->base||(a.resource->base==b->base&&a.resource->key.object<b->key.object);
    });
}
void InputCollector::refresh_location_ends(std::vector<VaEntry>::iterator it){
    auto end=it==locations_.begin()?0:std::prev(it)->prefix_end;
    for(;it!=locations_.end();++it){
        end=(std::max)(end,gpu_va_range_end(it->resource->base,it->resource->bytes));
        if(it->prefix_end==end)break; // the remainder already has this prefix
        it->prefix_end=end;
    }
}
void InputCollector::add_location(const Resource &r){
    if(r.base)refresh_location_ends(locations_.insert(location_of(r),{&r,0}));
}
void InputCollector::erase_location(const Resource &r){
    if(r.base)refresh_location_ends(locations_.erase(location_of(r)));
}
void InputCollector::init_resource(api::resource r,const api::resource_desc &desc,std::uint64_t creation,std::uint64_t gpu_address){
    if(desc.type!=api::resource_type::buffer)return;
    std::lock_guard lock(mutex_);
    auto old=resources_.find(r.handle);
    if(gpu_address&&(old==resources_.end()||!old->second.base)&&locations_.size()==locations_.capacity())
        locations_.reserve((std::max)(std::size_t{64},2*locations_.size()));
    if(erase_position_stamps_if_locked([&](const auto &s){return s.resource.object==r.handle;}))history_changed();
    const Resource value{{r.handle,creation?creation:++serial_},desc.buffer.size,gpu_address,desc.heap};
    if(old!=resources_.end()){
        destroyed(old->second.key);erase_location(old->second);
        old->second=value;add_location(old->second);
    }else add_location(resources_.emplace(r.handle,value).first->second);
}
void InputCollector::destroyed(ResourceKey key){
    shape_evidence_.destroyed(key);
    if(native_receipts_)discovery_.destroyed(key);
    bool changed=erase_position_stamps_if_locked([&](const auto &s){return s.resource==key;});
    if(identity_cache_)identity_cache_->destroyed(key);
    changed|=std::erase_if(content_,[&](const Content &c){return c.resource==key;})!=0;
    std::erase_if(proved_mesh_ibs_,[&](const ProvedMeshIb &ib){
        if(ib.allocation==key||ib.content==key)return true;
        return std::any_of(ib.components.begin(),ib.components.end(),[&](const ProvedComponentIb &c){return c.indices==key;});
    });
    std::erase_if(aux_skin_writers_,[&](const AuxSkinWriter &w){return w.output==key||w.rest==key;});
    std::erase_if(aux_probe_targets_,[&](const AuxRestProbeTarget &t){return t.rest==key;});
    std::erase_if(aux_rest_hashes_,[&](const AuxRestHash &h){return h.rest==key;});
    changed|=invalidate_pose_owners([&](PoseInputs &poses){poses.destroyed(key);});
    if(changed)history_changed();
    refresh_native_copy_mask_locked();
}
ResourceKey InputCollector::destroy_resource(api::resource r,std::uint64_t creation){
    std::lock_guard lock(mutex_);auto p=resources_.find(r.handle);
    const ResourceKey key=creation?ResourceKey{r.handle,creation}:p==resources_.end()?ResourceKey{}:p->second.key;
    if(!key)return {};destroyed(key);
    // A delayed old notification must never erase a newer same-address entry.
    if(p!=resources_.end()&&p->second.key==key){erase_location(p->second);resources_.erase(p);}
    return key;
}
ResourceKey InputCollector::key_of(api::resource r){
    std::lock_guard lock(mutex_);const auto p=resources_.find(r.handle);return p==resources_.end()?ResourceKey{}:p->second.key;
}
Recording InputCollector::init_list_deferred_stamp(api::command_list *c){
    std::lock_guard lock(mutex_);auto &list=lists_[id(c)];
    if(shape_evidence_enabled_)shape_evidence_.reset(list.epoch);
    bool copy_state_changed=false;
    if(native_receipts_)copy_state_changed|=discovery_.reset(list.epoch);
    if(identity_cache_)identity_cache_->reset(list.epoch);
    if(!aux_skin_writers_.empty())
        copy_state_changed|=std::erase_if(aux_skin_writers_,[&](const AuxSkinWriter &w){return w.recording==list.epoch&&w.submissions==0;})!=0;
    if(list.has_skin_sample||list.skin_submit_started||list.pose_owner!=no_pose_owner)retain_submitted_poses(list);
    list.epoch={id(c),++serial_,1};list.poses.reset(list.epoch);
    if(copy_state_changed)refresh_native_copy_mask_locked();
    return list.epoch;
}
void InputCollector::init_list(api::command_list *c){
    const auto epoch=init_list_deferred_stamp(c);
    if(stamp_&&epoch.list)stamp_(epoch.list,epoch.creation,epoch.generation);
}
void InputCollector::retain_submitted_poses(List &list){
    if(list.has_skin_sample)history_changed();
    if(list.skin_submit_started){
        // A native Reset can follow Execute but race the post callback.
        // Store the bytes, not authorization: no stamp with zero actual
        // post-Executes is readable by the new author consumer.
        std::shared_ptr<const PoseInputs> snapshot;
        for(auto &p:position_stamps_)if(p.producer==list.epoch&&p.submissions<2){
            if(!snapshot)snapshot=std::make_shared<const PoseInputs>(std::move(list.poses));
            p.submitted_poses=snapshot;
        }
    }
    detach_pose_owner(list);
    list.has_skin_sample=false;list.skin_submit_started=false;
}
void InputCollector::detach_pose_owner(List &list)noexcept{
    if(list.pose_owner==no_pose_owner)return;
    auto *moved=pose_owners_.back();
    pose_owners_[list.pose_owner]=moved;
    moved->pose_owner=list.pose_owner;
    pose_owners_.pop_back();
    list.pose_owner=no_pose_owner;
}
Recording InputCollector::reset_list_deferred_stamp(api::command_list *c){
    std::lock_guard lock(mutex_);auto p=lists_.find(id(c));if(p==lists_.end())return {};
    auto &list=p->second;
    const auto old_epoch=list.epoch;
    if(shape_evidence_enabled_)shape_evidence_.reset(old_epoch);
    bool copy_state_changed=false;
    if(native_receipts_)copy_state_changed|=discovery_.reset(old_epoch);
    if(!aux_skin_writers_.empty())
        copy_state_changed|=std::erase_if(aux_skin_writers_,[&](const AuxSkinWriter &w){return w.recording==old_epoch&&w.submissions==0;})!=0;
    if(!content_.empty()){
        if(std::erase_if(content_,[&](const Content &v){return v.producer==old_epoch&&!v.submitted;})){
            history_changed();
            copy_state_changed=true;
        }
    }
    if(identity_cache_)identity_cache_->reset(old_epoch);
    if(list.has_skin_sample||list.skin_submit_started||list.pose_owner!=no_pose_owner)retain_submitted_poses(list);
    ++list.epoch.generation;
    list.poses.reset(list.epoch);
    if(!bindings_.lists.empty())bindings_.reset(id(c));
    if(copy_state_changed)refresh_native_copy_mask_locked();
    return list.epoch;
    // This is a client observed-reset epoch. The actual native scope rejects a
    // failed Reset, so this counter alone never authorizes GPU command recording.
}
void InputCollector::reset_list(api::command_list *c){
    const auto epoch=reset_list_deferred_stamp(c);
    if(stamp_&&epoch.list)stamp_(epoch.list,epoch.creation,epoch.generation);
}
void InputCollector::destroy_list_deferred_stamp(api::command_list *c){
    (void)reset_list_deferred_stamp(c);
    std::lock_guard lock(mutex_);
    lists_.erase(id(c));
    if(!bindings_.lists.empty())bindings_.reset(id(c));
}
void InputCollector::destroy_list(api::command_list *c){
    destroy_list_deferred_stamp(c);
    if(stamp_)stamp_(id(c),0,0);
}
void InputCollector::init_layout(api::pipeline_layout layout,unsigned count,const api::pipeline_layout_param *params){
    skin::Ports ports;
    for(unsigned i=0;i<count;++i){const auto &p=params[i];
        auto range=[&](const auto &r){if(r.dx_register_space||r.dx_register_index||r.binding)return;
            if(r.type==api::descriptor_type::shader_resource_view&&r.count>=6)ports.srv=i;
            if(r.type==api::descriptor_type::unordered_access_view&&r.count>=2)ports.uav=i;
            if(r.type==api::descriptor_type::constant_buffer&&r.count==1)ports.cb=i;};
        if(p.type==api::pipeline_layout_param_type::push_descriptors)range(p.push_descriptors);
        else if(p.type==api::pipeline_layout_param_type::descriptor_table||p.type==api::pipeline_layout_param_type::push_descriptors_with_ranges)
            for(unsigned j=0;j<p.descriptor_table.count;++j)range(p.descriptor_table.ranges[j]);
        else if(p.type==api::pipeline_layout_param_type::descriptor_table_with_flags||p.type==api::pipeline_layout_param_type::push_descriptors_with_ranges_and_flags)
            for(unsigned j=0;j<p.descriptor_table_with_flags.count;++j)range(p.descriptor_table_with_flags.ranges[j]);
    }
    std::lock_guard lock(mutex_);bindings_.layouts[layout.handle]=ports;
    if(shape_evidence_enabled_){std::vector<ShapeEvidenceRange> ranges;
        for(unsigned i=0;i<count;++i){const auto &p=params[i];const auto add=[&](const auto &v,bool root){
            root|=(p.type==api::pipeline_layout_param_type::push_descriptors_with_ranges||p.type==api::pipeline_layout_param_type::push_descriptors_with_ranges_and_flags)&&v.type!=api::descriptor_type::sampler;
            ranges.push_back({i,unsigned(v.type),v.binding,v.dx_register_index,v.dx_register_space,v.count,unsigned(v.visibility),unsigned(p.type),root});auto &out=ranges.back();out.array_size=v.array_size;
            if constexpr(requires{v.flags;}){out.flags=unsigned(v.flags);out.flags_known=true;}};
        if(p.type==api::pipeline_layout_param_type::push_descriptors)add(p.push_descriptors,true);
        else if(p.type==api::pipeline_layout_param_type::descriptor_table||p.type==api::pipeline_layout_param_type::push_descriptors_with_ranges)
            for(unsigned j=0;j<p.descriptor_table.count;++j)add(p.descriptor_table.ranges[j],false);
        else if(p.type==api::pipeline_layout_param_type::descriptor_table_with_flags||p.type==api::pipeline_layout_param_type::push_descriptors_with_ranges_and_flags)
            for(unsigned j=0;j<p.descriptor_table_with_flags.count;++j)add(p.descriptor_table_with_flags.ranges[j],false);
        else ranges.push_back({i,UINT32_MAX,0,0,0,0,0,unsigned(p.type),true});
        }evidence_layouts_[layout.handle]=shape_evidence_.layout(layout.handle,count,std::move(ranges));
    }
}
void InputCollector::destroy_layout(api::pipeline_layout layout){
    std::lock_guard lock(mutex_);evidence_layouts_.erase(layout.handle);bindings_.layouts.erase(layout.handle);
    std::erase_if(bindings_.lists,[&](const auto &v){return v.second.layout==layout.handle;});
}
skin::Kind InputCollector::classify_compute_shader(const void *code,std::size_t bytes){
    return classify_skin_shader(code,bytes);
}
void InputCollector::init_pipeline_classified(api::pipeline p,api::pipeline_layout layout,skin::Kind kind,bool compute,const void *capture_code,std::size_t capture_bytes){
    std::lock_guard lock(mutex_);
    if(!evidence_shaders_.empty())evidence_shaders_.erase(p.handle);
    if(!shaders_.empty())shaders_.erase(p.handle);
    if(kind!=skin::Kind::none)shaders_[p.handle]={layout.handle,++serial_,kind};
    if(shape_evidence_enabled_&&compute){const auto l=evidence_layouts_.find(layout.handle);
        evidence_shaders_[p.handle]=shape_evidence_.shader(p.handle,shape_evidence_.next_creation(),l==evidence_layouts_.end()?nullptr:l->second,capture_code,capture_bytes);
    }
}
void InputCollector::init_pipeline(api::pipeline p,api::pipeline_layout layout,unsigned count,const api::pipeline_subobject *subs){
    skin::Kind kind=skin::Kind::none;const void *capture_code=nullptr;std::size_t capture_bytes=0;bool compute=false;
    for(unsigned i=0;i<count;++i)if(subs[i].type==api::pipeline_subobject_type::compute_shader&&subs[i].count){
        const auto &shader=*static_cast<const api::shader_desc *>(subs[i].data);kind=classify_skin_shader(shader.code,shader.code_size);compute=true;capture_code=shader.code;capture_bytes=shader.code_size;break;}
    init_pipeline_classified(p,layout,kind,compute,capture_code,capture_bytes);
}
void InputCollector::destroy_pipeline(api::pipeline p){
    std::lock_guard lock(mutex_);evidence_shaders_.erase(p.handle);shaders_.erase(p.handle);
    std::erase_if(bindings_.lists,[&](const auto &v){return v.second.pipeline==p.handle;});
}
bool InputCollector::tracks_pipeline_bind(api::command_list *c,api::pipeline p){
    std::lock_guard lock(mutex_);
    // A new known shader starts root evidence; an unrelated shader can
    // still erase evidence from a prior skin bind on this list.
    return shaders_.contains(p.handle)||bindings_.lists.contains(id(c));
}
bool InputCollector::tracks_table_bind(api::command_list *c,api::pipeline_layout layout,unsigned first,unsigned count){
    std::lock_guard lock(mutex_);
    const auto b=bindings_.lists.find(id(c));
    if(b==bindings_.lists.end()||b->second.layout!=layout.handle)return false;
    const auto includes=[&](int p){return p>=0&&unsigned(p)>=first&&unsigned(p)-first<count;};
    return includes(b->second.ports.srv)||includes(b->second.ports.uav);
}
bool InputCollector::tracks_cb_bind(api::command_list *c,api::pipeline_layout layout,unsigned param){
    std::lock_guard lock(mutex_);
    const auto b=bindings_.lists.find(id(c));
    return b!=bindings_.lists.end()&&b->second.layout==layout.handle&&
        b->second.ports.cb>=0&&unsigned(b->second.ports.cb)==param;
}
void InputCollector::bind_pipeline(api::command_list *c,api::pipeline_stage stages,api::pipeline p){
    if(!(static_cast<unsigned>(stages)&static_cast<unsigned>(api::pipeline_stage::compute_shader|api::pipeline_stage::ray_tracing_shader)))return;
    std::lock_guard lock(mutex_);auto s=shaders_.find(p.handle);
    if(s==shaders_.end()){bindings_.reset(id(c));return;}
    bindings_.bind(id(c),p.handle,s->second.generation,s->second.layout,s->second.kind);
}
void InputCollector::bind_tables(api::command_list *c,api::shader_stage stages,api::pipeline_layout layout,unsigned first,unsigned count,const api::descriptor_table *tables){
    if(!(static_cast<unsigned>(stages)&static_cast<unsigned>(api::shader_stage::all_compute)))return;
    std::lock_guard lock(mutex_);for(unsigned i=0;i<count;++i)bindings_.table(id(c),layout.handle,first+i,tables[i].handle);
}
void InputCollector::push_descriptors(api::command_list *c,api::shader_stage stages,api::pipeline_layout layout,unsigned param,const api::descriptor_table_update &u){
    if(!(static_cast<unsigned>(stages)&static_cast<unsigned>(api::shader_stage::all_compute))||u.type!=api::descriptor_type::constant_buffer||u.binding||!u.count)return;
    const auto &range=*static_cast<const api::buffer_range *>(u.descriptors);
    std::lock_guard lock(mutex_);bindings_.cb(id(c),layout.handle,param,range.buffer.handle,range.offset,range.size,range.buffer.handle!=0);
}
bool InputCollector::read(const Resource &r,std::uint64_t offset,std::uint64_t bytes,void *dest){
    if(!current(r)||r.heap!=api::memory_heap::upload||!identity::fits(offset,bytes,r.bytes))return false;
    auto *native=reinterpret_cast<ID3D12Resource *>(r.key.object);void *base=nullptr;
    D3D12_RANGE range{SIZE_T(offset),SIZE_T(offset+bytes)},none{0,0};
    if(FAILED(native->Map(0,&range,&base)))return false;
    const bool ok=base&&wwmi::safe_copy_from_wc(dest,static_cast<const unsigned char *>(base)+offset,static_cast<std::size_t>(bytes));
    native->Unmap(0,&none);return ok;
}
void InputCollector::written(ResourceKey r,std::uint64_t offset,std::uint64_t bytes){
    bool copy_state_changed=false;
    if(native_receipts_)copy_state_changed|=discovery_.written({r,offset,bytes});
    if(identity_cache_)identity_cache_->written(r,offset,bytes);
    bool changed=erase_position_stamps_if_locked([&](const auto &s){return s.resource==r&&identity::overlaps(s.offset,s.bytes,offset,bytes);});
    if(!content_.empty()){
        if(std::erase_if(content_,[&](const Content &c){return c.resource==r&&identity::overlaps(c.offset,c.profile->bytes(),offset,bytes);})){
            changed=true;copy_state_changed=true;
        }
    }
    if(!proved_mesh_ibs_.empty())std::erase_if(proved_mesh_ibs_,[&](const ProvedMeshIb &ib){
        if(ib.content==r&&identity::overlaps(ib.content_offset,ib.profile->bytes(),offset,bytes))return true;
        return std::any_of(ib.components.begin(),ib.components.end(),[&](const ProvedComponentIb &c){
            return c.indices==r&&identity::overlaps(c.index_offset,std::uint64_t(c.index_stride)*c.index_count,offset,bytes);
        });
    });
    if(!aux_skin_writers_.empty())
        copy_state_changed|=std::erase_if(aux_skin_writers_,[&](const AuxSkinWriter &w){
            return (w.output==r&&identity::overlaps(w.output_offset,w.output_bytes,offset,bytes))||
                   (w.rest==r&&identity::overlaps(w.rest_offset,w.rest_bytes,offset,bytes));
        })!=0;
    if(!aux_probe_targets_.empty())std::erase_if(aux_probe_targets_,[&](const AuxRestProbeTarget &t){
        return t.rest==r&&identity::overlaps(t.rest_offset,t.rest_bytes,offset,bytes);
    });
    if(!aux_rest_hashes_.empty())std::erase_if(aux_rest_hashes_,[&](const AuxRestHash &h){
        return h.rest==r&&identity::overlaps(h.rest_offset,h.rest_bytes,offset,bytes);
    });
    changed|=invalidate_pose_owners([&](PoseInputs &poses){poses.written(r,offset,bytes);});
    if(changed)history_changed();
    if(copy_state_changed)refresh_native_copy_mask_locked();
}
void InputCollector::copy(api::command_list *c,api::resource source,std::uint64_t from,api::resource dest,std::uint64_t to,std::uint64_t bytes){
    const auto native_profiles=registry_->snapshot();
    std::lock_guard lock(mutex_);auto d=resources_.find(dest.handle);if(d==resources_.end())return;
    written(d->second.key,to,bytes);
    auto s=resources_.find(source.handle);auto list=lists_.find(id(c));
    if(s==resources_.end()||list==lists_.end()||!current(s->second)||!current(d->second)||!identity::fits(to,bytes,d->second.bytes))return;
    for(const auto &profile:native_profiles->profiles){const auto n=profile->bytes();
        if(bytes!=n&&bytes!=((n+15)&~15ull)&&bytes!=((n+255)&~255ull))continue;
        std::vector<unsigned char> values(static_cast<size_t>(n));if(!read(s->second,from,n,values.data()))continue;
        const std::uint32_t hash_desc[]{std::uint32_t(n),0,9,0,0,0};
        const auto hash=crc32c_append(crc32c_append(0,values.data(),values.size()),reinterpret_cast<const unsigned char *>(hash_desc),sizeof(hash_desc));
        if(hash==profile->content_hash){
            arm_native_copy_mask(kCopyBeforeActive|kCopyRecActive);
            content_.push_back({d->second.key,to,list->second.epoch,false,profile});
        }
    }
    refresh_native_copy_mask_locked();
}
Recording InputCollector::recording(api::command_list *c){
    std::lock_guard lock(mutex_);auto list=lists_.find(id(c));return list==lists_.end()?Recording{}:list->second.epoch;
}
Recording InputCollector::submission_recording(api::command_list *c){
    std::lock_guard lock(mutex_);auto list=lists_.find(id(c));if(list==lists_.end())return {};
    if(list->second.has_skin_sample)list->second.skin_submit_started=true;
    return list->second.epoch;
}
bool InputCollector::recording_has_skin_samples(Recording epoch){
    if(!epoch.list||!epoch.creation||!epoch.generation)return false;
    std::lock_guard lock(mutex_);
    if(const auto list=lists_.find(epoch.list);list!=lists_.end()&&list->second.epoch==epoch&&list->second.has_skin_sample)return true;
    return std::any_of(position_stamps_.begin(),position_stamps_.end(),[&](const PositionStamp &p){return p.producer==epoch&&p.submissions==0;});
}
void InputCollector::record_skin_fence(Recording epoch,NativeIdentityFence signal){
    if(!epoch.list||!epoch.creation||!epoch.generation||!signal.queue||!signal.fence||!signal.value)return;
    std::lock_guard lock(mutex_);
    for(auto &f:submitted_skin_fences_){
        if(f.recording==epoch){f.invalidated=true;return;} // replay invalidates skin fence
    }
    submitted_skin_fences_.push_back({epoch,signal,false});
    while(submitted_skin_fences_.size()>128)submitted_skin_fences_.pop_front();
}
bool InputCollector::lookup_skin_fence_locked(Recording epoch,NativeIdentityFence &out)const{
    for(auto it=submitted_skin_fences_.rbegin();it!=submitted_skin_fences_.rend();++it){
        if(it->recording==epoch){
            if(it->invalidated||!it->signal.queue||!it->signal.fence||!it->signal.value)return false;
            out=it->signal;return true;
        }
    }
    return false;
}
const PoseInputs *InputCollector::poses_for_submitted_stamp_locked(const PositionStamp &p)const{
    if(p.submitted_poses)return p.submitted_poses.get();
    const auto it=lists_.find(p.producer.list);
    if(it!=lists_.end()&&it->second.epoch==p.producer)return &it->second.poses;
    return nullptr;
}
bool InputCollector::position_stamp_matches_locked(const SkinSample &s,unsigned expected_submissions)const{
    const auto *p=find_position_stamp_locked(s.output,s.output_offset,s.output_bytes);
    return p&&p->order==s.order&&p->producer==s.recording&&p->submissions==expected_submissions;
}
void InputCollector::remember_proved_ib_locked(const NativeBlasIdentity &id_val){
    if(id_val.components.empty())return;
    const auto &first_s=id_val.components.front().sample;
    if(!first_s.profile||!first_s.content||!id_val.allocation||!id_val.native_blas)return;
    ProvedMeshIb entry;
    entry.profile=first_s.profile;entry.content=first_s.content;entry.content_offset=first_s.content_offset;
    entry.allocation=id_val.allocation;entry.native_blas=id_val.native_blas;entry.geometries=id_val.geometries;
    entry.components.reserve(id_val.components.size());
    for(const auto &c:id_val.components)
        entry.components.push_back({c.indices,c.index_address,c.index_offset,c.index_count,c.index_stride});
    for(auto &existing:proved_mesh_ibs_){
        if(existing.profile==entry.profile&&existing.content==entry.content&&existing.content_offset==entry.content_offset){
            existing=std::move(entry);return;
        }
    }
    if(proved_mesh_ibs_.size()>=64)proved_mesh_ibs_.erase(proved_mesh_ibs_.begin());
    proved_mesh_ibs_.push_back(std::move(entry));
}
void InputCollector::submitted(Recording recording,std::uint64_t native_queue){
    std::lock_guard lock(mutex_);
    if(shape_evidence_enabled_)shape_evidence_.submitted(recording,native_queue);
    if(native_receipts_)discovery_.submitted(recording,native_queue,++order_);
    // Count only actual native post-Execute boundaries, never a CPU frame tag.
    // Replay cannot turn an old captured palette into a newly observed pose.
    bool changed=false;
    for(auto &p:position_stamps_)if(p.producer==recording&&p.submissions<2){
        changed=true;
        if(++p.submissions==1)p.native_queue=native_queue;
        else {
            p.native_queue=0;p.submitted_poses.reset();
            for(auto &f:submitted_skin_fences_)if(f.recording==recording)f.invalidated=true;
        }
    }
    if(has_skip_intents_&&!aux_skin_writers_.empty()){
        std::vector<AuxSkinWriter> newly_submitted;
        std::erase_if(aux_skin_writers_,[&](AuxSkinWriter &w){
            if(w.recording!=recording)return false;
            if(!w.forwarded)return true;
            if(++w.submissions>1)return true;
            newly_submitted.push_back(w);
            return false;
        });
        for(const auto &nw:newly_submitted){
            std::erase_if(aux_skin_writers_,[&](const AuxSkinWriter &old){
                return old.submissions==1&&old.order<nw.order&&old.output==nw.output&&
                       identity::overlaps(old.output_offset,old.output_bytes,nw.output_offset,nw.output_bytes);
            });
        }
    }
    std::erase_if(content_,[&](Content &proof){
        if(proof.producer!=recording)return false;
        changed=true;
        if(native_receipts_&&!proof.forwarded)return true;
        if(!proof.submitted){proof.submitted=true;return false;}
        // A replay can read changed upload memory. The old CPU byte proof is no longer sufficient.
        invalidate_pose_owners([&](PoseInputs &poses){poses.destroyed(proof.resource);});
        return true;
    });
    if(changed)history_changed();
    refresh_native_copy_mask_locked();
}
#include "native_shape_evidence_input.inc"
bool InputCollector::view(std::uint64_t table,unsigned binding,View &out){
    if(!table||!source_)return false;api::descriptor_heap heap{};std::uint32_t index=0;
    device_->get_descriptor_heap_offset(api::descriptor_table{table},binding,0,&heap,&index);
    if(!heap.handle)return false;
    WWMI_RtDescriptorBuffer_v2 copied{};
    const auto status=source_(device_->get_native(),heap.handle,index,&copied.base,sizeof(copied));
    if(copied.base.abi_version==2){
        if(!status||copied.base.struct_size!=sizeof(copied)||!copied.base.resource)return false;
        auto entry=resources_.find(copied.base.resource);if(entry==resources_.end()||!current(entry->second))return false;
        api::resource_view_desc desc{static_cast<api::format>(copied.format),copied.first,copied.count};
        if(!copied.format){
            desc.buffer.structured.count=copied.count;desc.buffer.structured.stride=copied.stride;
            if(copied.stride&&copied.first>UINT64_MAX/copied.stride)return false;
            if(copied.stride)desc.buffer.offset*=copied.stride;
        }
        out={entry->second,desc};return true;
    }
    // Existing main providers still expose only the v1 physical source.
    WWMI_RtDescriptorSource_v1 source{};
    if(!source_(device_->get_native(),heap.handle,index,&source,sizeof(source))||source.struct_size!=sizeof(source)||source.abi_version!=1)return false;
    auto resource=device_->get_resource_from_view(api::resource_view{source.source_view});
    if(resource.handle!=source.resource)return false;
    auto entry=resources_.find(resource.handle);if(entry==resources_.end()||!current(entry->second))return false;
    out={entry->second,device_->get_resource_view_desc(api::resource_view{source.source_view})};return true;
}
bool InputCollector::has_skin_dispatch(api::command_list *c){
    std::lock_guard lock(mutex_);const auto b=bindings_.lists.find(id(c));
    if(b==bindings_.lists.end()||!lists_.contains(id(c)))return false;
    const auto s=shaders_.find(b->second.pipeline);
    return s!=shaders_.end()&&s->second.generation==b->second.generation;
}
InputCollector::DispatchWrite InputCollector::dispatch(api::command_list *c,bool authorize){
    rt_cpu::Scope measured(rt_cpu::skin_dispatch);
    std::lock_guard lock(mutex_);auto bind=bindings_.lists.find(id(c));auto list=lists_.find(id(c));
    if(bind==bindings_.lists.end()||list==lists_.end())return {};const auto &b=bind->second;
    auto shader=shaders_.find(b.pipeline);if(shader==shaders_.end()||shader->second.generation!=b.generation)return {};
    const bool diagnostic_target=native_receipts_&&discovery_.interested();
    if(diagnostic_target)discovery_.note_skin();
    const auto reject_fact=[&](NativeDiscovery::Failure failure){if(diagnostic_target)discovery_.note_failure(failure);return DispatchWrite{};};
    auto cb=resources_.find(b.cb_resource);const unsigned cb_bytes=skin::cb_bytes(b.kind);
    std::uint32_t words[8]{};
    if(!b.cb_known||cb==resources_.end()||!skin::cb_range(cb->second.bytes,b.cb_offset,b.cb_size,cb_bytes)||!read(cb->second,b.cb_offset,cb_bytes,words))return reject_fact(NativeDiscovery::Failure::skin_cb);
    View output{},rest{},bones{};
    if(!view(b.uav_table,1,output)||output.desc.format!=api::format::r32_float)return reject_fact(NativeDiscovery::Failure::skin_output);
    const auto span=skin::position_span_for(b.kind,output.desc.buffer.offset,output.desc.buffer.size,output.resource.bytes,words);
    if(!span.known)return reject_fact(NativeDiscovery::Failure::skin_output);
    // Invalidate previous producer before any later input/proof rejection.
    measured.add(pose_owners_.size()); // mutable pose recordings visited
    written(output.resource.key,span.offset,span.bytes);
    if(!authorize)return reject_fact(NativeDiscovery::Failure::skin_unproven);
    const bool extra=b.kind==skin::Kind::skin8_extra;
    if(!view(b.srv_table,extra?6:5,rest)||rest.desc.format!=api::format::r32_float||rest.desc.buffer.offset>rest.resource.bytes/4)return reject_fact(NativeDiscovery::Failure::skin_rest);
    if(rest.desc.buffer.size>rest.resource.bytes/4-rest.desc.buffer.offset)return reject_fact(NativeDiscovery::Failure::skin_rest);
    const auto offset=rest.desc.buffer.offset*4;
    const unsigned first=words[extra?2:1],count=words[extra?4:3];
    NativeDiscovery::Writer fact;
    fact.recording=list->second.epoch;fact.output={output.resource.key,span.offset,span.bytes,output.resource.base};
    fact.rest={rest.resource.key,offset,4ull*rest.desc.buffer.size,rest.resource.base};fact.first=first;fact.count=count;fact.extra=extra;
    std::uint64_t discovery_writer_id=0;
    if(native_receipts_&&discovery_.interested()){
        arm_native_copy_mask(kCopyBeforeActive);
        discovery_writer_id=discovery_.skin(fact);
    }
    if(has_skip_intents_&&fact.rest.bytes>=12){
        std::erase_if(aux_skin_writers_,[&](const AuxSkinWriter &w){
            return w.recording==list->second.epoch&&w.submissions==0&&
                   w.output==output.resource.key&&identity::overlaps(w.output_offset,w.output_bytes,span.offset,span.bytes);
        });
        if(aux_skin_writers_.size()>=512)aux_skin_writers_.erase(aux_skin_writers_.begin());
        const std::uint64_t aux_order=++order_;
        aux_skin_writers_.push_back({output.resource.key,output.resource.base,span.offset,span.bytes,
            rest.resource.key,offset,fact.rest.bytes,first,count,list->second.epoch,aux_order,false,0});
        if(native_receipts_){
            auto pt=std::find_if(aux_probe_targets_.begin(),aux_probe_targets_.end(),[&](const AuxRestProbeTarget &t){
                return t.rest==rest.resource.key&&t.rest_offset==offset;
            });
            if(pt!=aux_probe_targets_.end()){
                const std::uint64_t end_bytes=12ull*(std::uint64_t(first)+count);
                if(end_bytes>pt->max_end_bytes&&end_bytes<=fact.rest.bytes)pt->max_end_bytes=end_bytes;
                for(const auto &w:aux_skin_writers_){
                    if(w.rest==rest.resource.key&&w.rest_offset==offset){
                        const std::uint64_t we=12ull*(std::uint64_t(w.first)+w.count);
                        if(we>pt->max_end_bytes&&we<=fact.rest.bytes)pt->max_end_bytes=we;
                    }
                }
                constexpr std::size_t kMaxTriedSizes=8;
                auto add_cand=[&](std::vector<std::uint64_t> &cands,std::uint64_t sz){
                    if(sz<12||sz>fact.rest.bytes||sz>NativeDiscovery::max_probe_bytes||(sz%4)!=0)return;
                    if(pt->tried_bytes.size()+cands.size()>=kMaxTriedSizes)return;
                    if(std::find(pt->tried_bytes.begin(),pt->tried_bytes.end(),sz)!=pt->tried_bytes.end())return;
                    if(std::find(cands.begin(),cands.end(),sz)!=cands.end())return;
                    const bool hashed=std::any_of(aux_rest_hashes_.begin(),aux_rest_hashes_.end(),[&](const AuxRestHash &h){
                        return h.rest==rest.resource.key&&h.rest_offset==offset&&h.rest_bytes==sz;
                    });
                    if(!hashed)cands.push_back(sz);
                };
                std::vector<std::uint64_t> cands;
                add_cand(cands,pt->max_end_bytes);
                add_cand(cands,end_bytes);
                add_cand(cands,fact.rest.bytes);
                if(!cands.empty()){
                    if(discovery_.request_known_rest(fact,discovery_writer_id,cands))
                        arm_native_copy_mask(kCopyBeforeActive);
                    for(const auto sz:cands)pt->tried_bytes.push_back(sz);
                }
            }
        }
    }
    unsigned component=UINT32_MAX;
    auto proof=content_.end();
    for(auto it=content_.begin();it!=content_.end();++it)if(it->submitted&&it->resource==rest.resource.key&&it->offset==offset&&rest.desc.buffer.size>=it->profile->bytes()/4)
        for(unsigned i=0;i<it->profile->components.size();++i)if(first==it->profile->components[i].first&&count==it->profile->components[i].count){
            if(proof!=content_.end()&&proof->profile!=it->profile)return {};
            proof=it;component=i;
        }
    if(proof==content_.end()){
        if(native_receipts_){
            const auto state=registry_->snapshot();
            std::vector<std::uint64_t> candidate_bytes;
            for(const auto &profile:state->profiles)if(rest.desc.buffer.size>=profile->bytes()/4){
                bool matches_window=false;
                for(const auto &comp:profile->components)if(first==comp.first&&count==comp.count){matches_window=true;break;}
                if(matches_window&&std::find(candidate_bytes.begin(),candidate_bytes.end(),profile->bytes())==candidate_bytes.end())
                    candidate_bytes.push_back(profile->bytes());
            }
            if(!candidate_bytes.empty()){
                if(discovery_.request_known_rest(fact,discovery_writer_id,candidate_bytes))
                    arm_native_copy_mask(kCopyBeforeActive);
            }
        }
        return {};
    }
    if(component==UINT32_MAX||!view(b.srv_table,0,bones)||bones.desc.format!=api::format::r32g32b32a32_float)return {};
    const auto palette=skin::palette_span(bones.desc.buffer.offset,bones.desc.buffer.size,bones.resource.bytes);
    if(!palette.known)return {};
    SkinSample sample{};sample.recording=list->second.epoch;sample.order=++order_;sample.component=component;
    sample.output=output.resource.key;sample.output_base=output.resource.base;sample.output_offset=span.offset;sample.output_bytes=span.bytes;
    sample.content=rest.resource.key;sample.content_offset=offset;sample.content_hash=proof->profile->content_hash;sample.content_confirmed=true;sample.profile=proof->profile;
    sample.extra_position_input=extra;sample.native_delta_flags=words[0]&3u;std::memcpy(&sample.scale,&words[extra?7:6],4);
    if(extra){
        // Exact known kernel: t3 reads six floats at 6*(cb0.word1+tid).
        // t5 is the SNORM tangent basis, NOT the morph stream. This is a
        // bounded view/creation receipt only; no GPU readback or guessed writer.
        View morph{};
        if(view(b.srv_table,3,morph)&&morph.desc.type==api::resource_view_type::buffer&&
            morph.desc.format==api::format::r32_float&&morph.desc.buffer.offset<=morph.resource.bytes/4&&
            morph.desc.buffer.size<=morph.resource.bytes/4-morph.desc.buffer.offset&&
            words[1]<=UINT32_MAX/6&&count<=UINT32_MAX/6-words[1]){
            const auto first=6ull*words[1],length=6ull*count;
            if(first<=morph.desc.buffer.size&&length<=morph.desc.buffer.size-first)
                sample.native_shape={morph.resource.key,4*morph.desc.buffer.offset,4*morph.desc.buffer.size,
                    4*(morph.desc.buffer.offset+first),4*length,words[1],count};
        }
    }
    sample.palette_resource=bones.resource.key;sample.palette_base=bones.resource.base;sample.palette_offset=palette.offset;sample.palette_bytes=palette.bytes;
    sample.palette.resize(palette.bytes/sizeof(Matrix));
    if(!read(bones.resource,palette.offset,palette.bytes,sample.palette.data()))return {};
    if(shape_evidence_enabled_&&std::any_of(packs_.begin(),packs_.end(),[&](const auto &p){return p->native==sample.profile&&p->shapekey_vertices;}))
        shape_evidence_.mark_target(sample.recording,sample.profile->object_guid,sample.profile->content_hash,sample.component,sample.native_shape);
    const PositionStamp stamp{sample.output,sample.output_offset,sample.output_bytes,sample.order,sample.recording};
    auto &owner=list->second;
    if(owner.pose_owner==no_pose_owner&&pose_owners_.size()==pose_owners_.capacity())
        pose_owners_.reserve((std::max)(std::size_t{4},2*pose_owners_.size()));
    if(!owner.poses.add(std::move(sample)))return {};
    if(owner.pose_owner==no_pose_owner){owner.pose_owner=pose_owners_.size();pose_owners_.push_back(&owner);}
    owner.has_skin_sample=true;
    add_position_stamp_locked(stamp);
    return {stamp.resource,component,stamp.order};
}
bool InputCollector::borrow_palette(const SkinSample &sample,Recording consumer,BorrowedPalette &out){
    std::lock_guard lock(mutex_);out={};
    if(sample.recording!=consumer||!sample.palette_resource||sample.palette.empty()||sample.palette.size()>512||
        sample.palette_bytes!=sample.palette.size()*sizeof(Matrix)||sample.palette_offset%256)return false;
    const auto entry=resources_.find(sample.palette_resource.object);
    if(entry==resources_.end()||!current(entry->second)||entry->second.key!=sample.palette_resource||entry->second.heap!=api::memory_heap::upload||entry->second.base!=sample.palette_base)return false;
    const auto &r=entry->second;const auto rounded=(sample.palette_bytes+255)&~255ull;
    if(!r.base||!identity::fits(sample.palette_offset,rounded,r.bytes)||r.base>UINT64_MAX-sample.palette_offset||(r.base+sample.palette_offset)%256)return false;
    auto *resource=reinterpret_cast<ID3D12Resource *>(r.key.object);D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
    const auto desc=resource->GetDesc();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||desc.Width!=r.bytes||resource->GetGPUVirtualAddress()!=r.base||
        FAILED(resource->GetHeapProperties(&heap,&flags))||heap.Type!=D3D12_HEAP_TYPE_UPLOAD)return false;
    // D3D12 UPLOAD resources remain GENERIC_READ. Check only this already
    // identified window, not arbitrary uploads. A live allocation alone does
    // not prove that its CPU-written ring window still contains this pose.
    std::array<Matrix,512> current;
    if(!read(r,sample.palette_offset,sample.palette_bytes,current.data())||std::memcmp(current.data(),sample.palette.data(),size_t(sample.palette_bytes)))return false;
    out.resource=resource;out.key=r.key;out.offset=sample.palette_offset;out.bytes=sample.palette_bytes;out.cbv_bytes=rounded;out.resource_bytes=r.bytes;
    return true;
}
bool InputCollector::position_live(const SkinSample &s){
    std::lock_guard lock(mutex_);const auto r=resources_.find(s.output.object),content=resources_.find(s.content.object);
    if(r==resources_.end()||r->second.key!=s.output||!current(r->second)||content==resources_.end()||content->second.key!=s.content||!current(content->second))return false;
    const auto *p=find_position_stamp_locked(s.output,s.output_offset,s.output_bytes);
    return p&&p->order==s.order;
}
InputCollector::PositionReceipt InputCollector::position_receipt(ResourceKey key,std::uint64_t offset,std::uint64_t bytes){
    std::lock_guard lock(mutex_);const auto r=resources_.find(key.object);
    if(r==resources_.end()||r->second.key!=key||!current(r->second))return {};
    const auto *p=find_position_stamp_locked(key,offset,bytes);
    return !p?PositionReceipt{}:PositionReceipt{p->producer,p->order,p->submissions,p->native_queue};
}
unsigned InputCollector::position_submission_count(const SkinSample &s){
    const auto p=position_receipt(s.output,s.output_offset,s.output_bytes);
    return p.order==s.order&&p.producer==s.recording?p.submissions:UINT32_MAX;
}
const char *InputCollector::native_position_srv_error(std::uint64_t resource,std::uint64_t offset,std::uint64_t bytes,unsigned format,unsigned stride,ResourceKey &out){
    std::lock_guard lock(mutex_);
    if(format!=DXGI_FORMAT_R32_FLOAT || stride)return "view_type";
    if(!bytes || offset%4 || bytes%4)return "view_alignment_or_empty";
    const auto r=resources_.find(resource);
    if(r==resources_.end())return "resource_not_tracked";
    if(!current(r->second))return "resource_no_longer_current";
    if(!identity::fits(offset,bytes,r->second.bytes))return "view_outside_resource";
    out=r->second.key;return nullptr;
}
bool InputCollector::previous_cb_keys(std::uint64_t view,std::uint64_t view_origin,std::uint64_t primitive,std::uint64_t primitive_origin,
    ResourceKey &view_key,ResourceKey &primitive_key){
    std::lock_guard lock(mutex_);
    const auto key=[&](std::uint64_t handle,std::uint64_t offset,std::uint64_t extent)->ResourceKey{
        const auto r=resources_.find(handle);
        if(r==resources_.end()||r->second.heap!=api::memory_heap::upload||!current(r->second)||
            !r->second.base||offset>UINT64_MAX-r->second.base||(r->second.base+offset)%256||!identity::fits(offset,extent,r->second.bytes))return {};
        return r->second.key;
    };
    view_key=key(view,view_origin,16*160+4);primitive_key=key(primitive,primitive_origin,16*27);
    return bool(view_key)&&bool(primitive_key);
}
ResourceKey InputCollector::primitive_key(std::uint64_t primitive,std::uint64_t origin){
    std::lock_guard lock(mutex_);
    const auto r=resources_.find(primitive);
    if(r==resources_.end()||r->second.heap!=api::memory_heap::upload||!current(r->second)||
       !r->second.base||origin>UINT64_MAX-r->second.base||(r->second.base+origin)%256||
       !identity::fits(origin,720,r->second.bytes)||!unique_current(r->second,r->second.base+origin,720))return {};
    return r->second.key;
}
Assembly InputCollector::assemble_snapshot(const NativeBlasIdentity &source,std::shared_ptr<const ModPack> pack){
    rt_cpu::Scope measured(rt_cpu::pose_assemble);
    std::lock_guard lock(mutex_);if(!pack||source.components.size()!=pack->components.size())return {PoseResult::missing_producer,{}};
    PoseInputs values;values.reset(source.recording);BlasPoseInput input{source.recording,{}};
    for(const auto &c:source.components){if(!values.add(c.sample))return {PoseResult::missing_producer,{}};input.geometries.push_back(c.vertices);}
    // No guessed current-graphics FINAL or previous-frame input. The author
    // path uses the exact native snapshot's canonical bone merge and weights.
    return values.assemble(input,std::move(pack));
}
ComponentAssembly InputCollector::assemble_previous_component(api::command_list *c,std::shared_ptr<const ModPack> pack,ResourceKey output,
    std::uint64_t view_offset,unsigned component,ResourceKey content,std::uint64_t content_offset,bool diagnostic,
    const BlasPoseInput *candidate_layout,const std::vector<const SkinSample *> *retained_providers){
    rt_cpu::Scope measured(rt_cpu::pose_assemble);
    std::lock_guard lock(mutex_);const auto list=lists_.find(id(c));
    if(!pack||list==lists_.end()||!live_key(output)||!live_key(content))return {};
    const auto proof=std::find_if(content_.begin(),content_.end(),[&](const Content &v){
        return v.submitted&&v.resource==content&&v.offset==content_offset&&v.profile==pack->native;
    });
    if(proof==content_.end())return {};
    auto result=list->second.poses.assemble_previous_component(pack,output,view_offset,component,content,content_offset,candidate_layout,retained_providers);
    if(diagnostic&&result.missing_component!=UINT32_MAX){
        result.local_same=result.local_other=result.other_recordings=0;
        list->second.poses.count_component_candidates(pack->native,result.missing_component,content,content_offset,output,result.local_same,result.local_other);
        for(const auto *other:pose_owners_)if(other!=&list->second){
            unsigned same=0,split=0;other->poses.count_component_candidates(pack->native,result.missing_component,content,content_offset,output,same,split);
            result.other_recordings+=same+split;
        }
    }
    return result;
}
ComponentAssembly InputCollector::assemble_submitted_previous(std::shared_ptr<const ModPack> pack,const SkinSample &current,
    ResourceKey output,std::uint64_t view_offset,PositionReceipt observed){
    rt_cpu::Scope measured(rt_cpu::pose_assemble);
    std::lock_guard lock(mutex_);
    if(!author_history_||!pack||!pack->native||current.profile!=pack->native||current.component>=pack->native->components.size()||
       !observed.order||observed.order==current.order||observed.submissions!=1||!observed.native_queue||!live_key(output)||!live_key(current.content))return {};
    const auto &part=pack->native->components[current.component];
    const auto first=12ull*part.first,bytes=12ull*part.count;
    if(view_offset>UINT64_MAX-first)return {};
    const auto *stamp=find_position_stamp_locked(output,view_offset+first,bytes);
    if(!stamp||stamp->order!=observed.order||stamp->producer!=observed.producer||
       stamp->submissions!=1||stamp->native_queue!=observed.native_queue)return {};
    const PoseInputs *values=stamp->submitted_poses.get();
    if(!values){
        const auto list=lists_.find(stamp->producer.list);
        if(list==lists_.end()||list->second.epoch!=stamp->producer)return {};
        values=&list->second.poses;
    }
    // No BLAS, nearest-frame, or cross-ring guess. The established same-SRV
    // assembly checks content, each component's exact range and BoneMap.
    return values->assemble_previous_component(std::move(pack),output,view_offset,current.component,current.content,current.content_offset);
}
InputCollector::PreviousBlasProof InputCollector::assemble_previous_blas(api::command_list *c,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc,std::shared_ptr<const ModPack> pack,ResourceKey output,
    std::uint64_t view_offset,unsigned component,ResourceKey content,std::uint64_t content_offset){
    rt_cpu::Scope measured(rt_cpu::pose_assemble);std::lock_guard lock(mutex_);PreviousBlasProof out;
    const auto list=lists_.find(id(c));
    std::vector<unsigned> mapping;
    if(!pack||!pack->native||component>=pack->components.size()||list==lists_.end()||!native_layout_candidate(*pack->native,desc,&mapping))return out;
    if(mapping[component]==UINT32_MAX)return out;
    const auto *target_gd=blas_geom_desc(desc,mapping[component]);
    if(!target_gd)return out;
    const auto &target_geometry=target_gd->Triangles;
    const auto *target_resource=locate(target_geometry.VertexBuffer.StartAddress,pack->native->bytes());
    if(!target_resource||target_resource->key!=output||target_resource->base>UINT64_MAX-view_offset||
        target_resource->base+view_offset!=target_geometry.VertexBuffer.StartAddress)return out;
    out.relevant=true;out.recording=list->second.epoch;out.stage="target_not_recorded_here";
    const auto &target_native=pack->components[component].native;
    if(view_offset>UINT64_MAX-12ull*target_native.first)return out;
    const auto expected_offset=view_offset+12ull*target_native.first;
    const auto *stamp=find_position_stamp_locked(output,expected_offset,12ull*target_native.count);
    if(stamp){out.target_order=stamp->order;out.target_recording=stamp->producer;}
    const auto *target=list->second.poses.draw_sample(target_geometry.VertexBuffer.StartAddress,target_native.index_first,target_native.index_count);
    if(!target||target->profile!=pack->native||target->recording!=out.recording||target->output!=output||
        target->output_offset!=expected_offset||target->content!=content||target->content_offset!=content_offset||
        !stamp||stamp->order!=target->order||stamp->producer!=target->recording)return out;
    if(!locate(desc.DestAccelerationStructureData,1)){out.stage="blas_allocation_unresolved";return out;}
    const unsigned target_idx_stride=native_index_stride(target_geometry.IndexFormat);
    if(!target_idx_stride){out.stage="target_indices_unresolved";return out;}
    const auto *target_indices=locate(target_geometry.IndexBuffer,std::uint64_t(target_idx_stride)*target_geometry.IndexCount);
    if(!target_indices){out.stage="target_indices_unresolved";return out;}
    const ResourceKey index_key=target_indices->key;
    BlasPoseInput link{out.recording,{}};link.geometries.resize(pack->components.size());
    link.geometries[component]={output,view_offset,pack->native->bytes()};
    for(const auto &dep:pack->components[component].dependencies){
        if(dep.provider>=pack->components.size()||mapping[dep.provider]==UINT32_MAX){out.stage="provider_invalid";return out;}
        const auto *dep_gd=blas_geom_desc(desc,mapping[dep.provider]);
        if(!dep_gd){out.stage="provider_invalid";return out;}
        const auto &n=pack->components[dep.provider].native;const auto &g=dep_gd->Triangles;
        const unsigned dep_idx_stride=native_index_stride(g.IndexFormat);
        const auto *indices=dep_idx_stride?locate(g.IndexBuffer,std::uint64_t(dep_idx_stride)*g.IndexCount):nullptr;const auto *vertices=locate(g.VertexBuffer.StartAddress,pack->native->bytes());
        const auto *sample=list->second.poses.draw_sample(g.VertexBuffer.StartAddress,n.index_first,n.index_count);
        if(!sample){out.stage="provider_not_recorded_here";return out;}
        if(!indices||indices->key!=index_key||!vertices||sample->output!=vertices->key||sample->profile!=pack->native||
            sample->recording!=out.recording||sample->content!=content||sample->content_offset!=content_offset||
            g.VertexBuffer.StartAddress<vertices->base){out.stage="provider_proof_mismatch";return out;}
        out.provider_order=sample->order;out.provider_output=sample->output;
        link.geometries[dep.provider]={sample->output,g.VertexBuffer.StartAddress-vertices->base,pack->native->bytes()};
    }
    out.stage="linked";
    out.candidate=list->second.poses.assemble_previous_component(pack,output,view_offset,component,content,content_offset,&link);
    if(out.candidate.assembly.result!=PoseResult::complete)out.stage="linked_pose_rejected";
    return out;
}
bool InputCollector::previous_sources_live_impl(const std::vector<SkinSample> &sources,std::shared_ptr<const ModPack> pack,bool require_submitted,
    Recording historical_writer,std::uint64_t native_queue){
    std::lock_guard lock(mutex_);if(!pack||sources.empty())return false;
    const auto &first=sources.front();
    if(!live_key(first.content))return false;
    const auto proof=std::find_if(content_.begin(),content_.end(),[&](const Content &v){
        return v.submitted&&v.resource==first.content&&v.offset==first.content_offset&&v.profile==pack->native;
    });
    if(proof==content_.end())return false;
    bool has_historical=false;
    for(const auto &s:sources){
        const bool historical=historical_writer.list&&s.component!=first.component&&s.recording==historical_writer;
        if((!historical&&s.recording!=first.recording)||s.content!=first.content||s.content_offset!=first.content_offset||!live_key(s.output))return false;
        const auto *stamp=find_position_stamp_locked(s.output,s.output_offset,s.output_bytes);
        if(!stamp||stamp->order!=s.order||stamp->producer!=s.recording||
           ((require_submitted||historical)?stamp->submissions!=1:stamp->submissions>1)||
           (native_queue&&stamp->native_queue!=native_queue))return false;
        has_historical|=historical;
    }
    return !historical_writer.list||has_historical;
}
ResourceKey InputCollector::resource_at(std::uint64_t address){
    std::lock_guard lock(mutex_);const auto *r=locate(address,1);return r?r->key:ResourceKey{};
}
InputCollector::RayInputRange InputCollector::ray_input_range(std::uint64_t address,std::uint64_t bytes,std::span<std::byte> prefix){
    if(!address||!bytes||prefix.size()>bytes)return {};
    std::lock_guard lock(mutex_);
    const auto *r=locate(address,bytes);
    if(!r)return {};
    RayInputRange out{r->key,address-r->base,bytes,r->heap,false};
    if(!prefix.empty())out.upload_read=read(*r,out.offset,prefix.size(),prefix.data());
    return out;
}
const InputCollector::Resource *InputCollector::locate(std::uint64_t address,std::uint64_t bytes){
    rt_cpu::Scope indexed(rt_cpu::locate_index);
    const auto first=std::lower_bound(locations_.begin(),locations_.end(),gpu_va_range_end(address,bytes),
        [](const VaEntry &e,std::uint64_t end){return e.prefix_end<end;});
    const Resource *found=nullptr;
    for(auto it=first;it!=locations_.end()&&it->resource->base<=address;++it){
        indexed.add();
        const auto &r=*it->resource;
        if(identity::fits(address-r.base,bytes,r.bytes)&&current(r)){
            if(found)return nullptr;
            found=&r;
        }
    }
    // The inbox may have published a newer generation or a covering alias
    // after RuntimeCall's drain. This remains the final native liveness gate.
    return found&&unique_current(*found,address,bytes)?found:nullptr;
}
bool InputCollector::identify_draw(api::command_list *c,std::uint64_t base,unsigned first,unsigned count,WWMI_RtPackDrawIdentity_v1 &out,void *palette,unsigned capacity){
    std::lock_guard lock(mutex_);auto list=lists_.find(id(c));if(list==lists_.end())return false;
    const auto *sample=list->second.poses.draw_sample(base,first,count);if(!sample)return false;
    const auto output=resources_.find(sample->output.object),content=resources_.find(sample->content.object);
    if(output==resources_.end()||output->second.key!=sample->output||!current(output->second)||content==resources_.end()||content->second.key!=sample->content||!current(content->second))return false;
    const auto &s=*sample;const auto &n=s.profile->components[s.component];out={};out.struct_size=sizeof(out);out.abi_version=1;
    out.object_guid=s.profile->object_guid;out.component=n.component;out.content_hash=s.content_hash;out.native_vertex_first=n.first;out.native_vertex_count=n.count;
    out.list=s.recording.list;out.list_creation=s.recording.creation;out.recording=s.recording.generation;
    out.content_resource=s.content.object;out.content_creation=s.content.creation;out.content_offset=s.content_offset;
    out.output_resource=s.output.object;out.output_creation=s.output.creation;out.position_base=base;out.output_offset=s.output_offset;out.output_bytes=s.output_bytes;
    out.scale=s.scale;out.native_extra_input=s.extra_position_input;out.native_delta_flags=s.native_delta_flags;out.palette_bytes=unsigned(s.palette.size()*sizeof(Matrix));
    if(palette&&capacity>=out.palette_bytes){std::memcpy(palette,s.palette.data(),out.palette_bytes);out.palette_copied_bytes=out.palette_bytes;}return true;
}
bool InputCollector::try_synthesize_from_proved_ib_locked(const WWMI_RtPackDrawQuery_v2 &q,const Resource &vertices,
    const Resource &indices,unsigned idx_stride,unsigned &diag_reason,unsigned &diag_found,unsigned &diag_total,std::uint32_t &diag_guid){
    diag_reason=0;diag_found=0;diag_total=0;diag_guid=0;
    if(!identity_cache_||proved_mesh_ibs_.empty())return false;
    const std::uint64_t idx_va=q.index_buffer_gpu_va+std::uint64_t(idx_stride)*q.first_index;
    const ProvedMeshIb *matched_ib=nullptr;unsigned matched_comp=UINT32_MAX;
    for(const auto &ib:proved_mesh_ibs_){
        if(!ib.profile||q.vertex_buffer_bytes<ib.profile->bytes()||!live_key(ib.allocation)||!live_key(ib.content))continue;
        for(unsigned ci=0;ci<ib.components.size();++ci){
            const auto &c=ib.components[ci];
            if(c.indices!=indices.key||c.index_address!=idx_va||c.index_count!=q.index_count||c.index_stride!=idx_stride)continue;
            if(matched_ib){diag_reason=1;diag_guid=ib.profile->object_guid;return false;} // ambiguous IB match
            matched_ib=&ib;matched_comp=ci;
        }
    }
    if(!matched_ib){diag_reason=2;return false;}
    const auto &profile=matched_ib->profile;
    diag_guid=profile->object_guid;diag_total=unsigned(profile->components.size());
    for(const auto &c:matched_ib->components)if(!live_key(c.indices)){diag_reason=3;return false;}
    const auto &target_n=profile->components[matched_comp];
    if(q.vertex_buffer_gpu_va<vertices.base)return false;
    const std::uint64_t target_view_off=q.vertex_buffer_gpu_va-vertices.base;
    if(target_view_off>UINT64_MAX-12ull*target_n.first)return false;
    const std::uint64_t target_out_off=target_view_off+12ull*target_n.first;

    struct SynthCandidate {
        std::shared_ptr<NativeBlasIdentity> identity;
        unsigned submissions=0;
        NativeIdentityFence producer{};
    };
    std::optional<SynthCandidate> winner;
    bool ambiguous=false;
    std::vector<Recording> visited;
    auto try_poses=[&](const PoseInputs &poses,unsigned expected_subs,NativeIdentityFence producer){
        const Recording rec=poses.recording();
        if(!rec.list||!rec.creation||!rec.generation)return;
        if(std::find_if(visited.begin(),visited.end(),[&](const Recording &v){return v==rec;})!=visited.end())return;
        visited.push_back(rec);
        // 1. Require exact match for the drawn component's live VB0 GPU VA & resource key.
        const SkinSample *anchor=nullptr;
        for(const auto &s:poses.samples()){
            if(s.profile!=profile||s.component!=matched_comp||s.content!=matched_ib->content||s.content_offset!=matched_ib->content_offset)continue;
            if(s.output!=vertices.key||s.output_offset!=target_out_off||s.output_bytes!=12ull*target_n.count)continue;
            if(!s.output_base||s.output_base>UINT64_MAX-target_view_off||s.output_base+target_view_off!=q.vertex_buffer_gpu_va)continue;
            if(!position_stamp_matches_locked(s,expected_subs))continue;
            if(anchor){ambiguous=true;return;}
            anchor=&s;
        }
        if(!anchor)return;
        // 2. Gather live CRC32C-proved SkinSamples for ALL M components in this same recording.
        const std::size_t comp_cnt=profile->components.size();
        std::vector<const SkinSample *> chosen(comp_cnt,nullptr);
        std::vector<std::uint64_t> bases(comp_cnt,0);
        chosen[matched_comp]=anchor;bases[matched_comp]=q.vertex_buffer_gpu_va;
        unsigned found_cnt=1;
        for(unsigned i=0;i<comp_cnt;++i){
            if(i==matched_comp)continue;
            const auto &n=profile->components[i];
            const SkinSample *best=nullptr;unsigned matches_i=0;std::uint64_t best_dist=UINT64_MAX;
            for(const auto &s:poses.samples()){
                if(s.profile!=profile||s.component!=i||s.content!=matched_ib->content||s.content_offset!=matched_ib->content_offset)continue;
                if(!s.output_base||s.output_offset<12ull*n.first||!live_key(s.output))continue;
                const auto off=s.output_offset-12ull*n.first;
                if(s.output_base>UINT64_MAX-off)continue;
                const auto va=s.output_base+off;
                const auto *vr=locate(va,profile->bytes());
                if(!vr||vr->key!=s.output||!position_stamp_matches_locked(s,expected_subs))continue;
                ++matches_i;
                const auto dist=s.order>anchor->order?s.order-anchor->order:anchor->order-s.order;
                if(!best||va==q.vertex_buffer_gpu_va||(bases[i]!=q.vertex_buffer_gpu_va&&dist<best_dist)){
                    best=&s;bases[i]=va;best_dist=dist;
                }
            }
            if(!best||(matches_i>1&&bases[i]!=q.vertex_buffer_gpu_va)){continue;}
            chosen[i]=best;++found_cnt;
        }
        diag_found=(std::max)(diag_found,found_cnt);
        if(found_cnt!=comp_cnt){diag_reason=4;return;}
        auto val=std::make_shared<NativeBlasIdentity>();
        val->recording=rec;val->allocation=matched_ib->allocation;val->native_blas=matched_ib->native_blas;
        val->geometries=matched_ib->geometries;val->components.reserve(comp_cnt);
        for(unsigned i=0;i<comp_cnt;++i){
            const auto &s=*chosen[i];const auto &n=profile->components[i];
            const auto &cib=matched_ib->components[i];
            const GeometryInput gi{s.output,s.output_offset-12ull*n.first,profile->bytes()};
            val->components.push_back({s,gi,cib.indices,bases[i],cib.index_address,cib.index_offset,cib.index_count,cib.index_stride});
        }
        if(winner){ambiguous=true;return;}
        winner=SynthCandidate{std::move(val),expected_subs,producer};
    };

    // Search open recordings first (submissions == 0).
    for(const auto *owner:pose_owners_){
        if(owner->has_skin_sample)try_poses(owner->poses,0,{});
        if(ambiguous){diag_reason=5;return false;}
    }
    // Search already-submitted-once recordings with a real GPU queue fence (submissions == 1).
    for(auto it=position_stamps_.rbegin();it!=position_stamps_.rend();++it){
        const auto &p=*it;
        if(p.submissions!=1||p.resource!=vertices.key||p.offset!=target_out_off||p.bytes!=12ull*target_n.count)continue;
        NativeIdentityFence fence{};
        if(!lookup_skin_fence_locked(p.producer,fence)){diag_reason=6;continue;}
        const auto *poses=poses_for_submitted_stamp_locked(p);
        if(!poses){diag_reason=7;continue;}
        try_poses(*poses,1,fence);
        if(ambiguous){diag_reason=5;return false;}
    }
    if(!winner){if(!diag_reason)diag_reason=8;return false;}
    identity_cache_->record_with_state(std::move(winner->identity),winner->submissions,winner->producer);
    return true;
}
bool InputCollector::identify_draw_v2(const WWMI_RtPackDrawQuery_v2 &q,WWMI_RtPackDrawIdentity_v2 &out,void *palette,unsigned capacity,
                                     NativeDrawIdentityCache::Match *match_out,WWMI_RtDrawIdentityReceipt_v1 *receipt,bool author_snapshot){
    const auto native_profiles=registry_->snapshot();
    auto fail=[&](unsigned why){if(receipt)receipt->reason=why;return false;};
    std::lock_guard lock(mutex_);auto list=lists_.find(q.list);
    if(!identity_cache_||list==lists_.end())return fail(WWMI_RT_IDENTIFY_RECORDING);
    if(receipt){receipt->consumer_creation=list->second.epoch.creation;receipt->consumer_recording=list->second.epoch.generation;}
    const unsigned idx_stride=native_index_stride(static_cast<DXGI_FORMAT>(q.index_format));
    if(q.device!=device_->get_native()||
        !idx_stride||q.vertex_stride!=12||q.base_vertex||!q.index_count||!q.vertex_buffer_bytes||
        !identity::fits(std::uint64_t(idx_stride)*q.first_index,std::uint64_t(idx_stride)*q.index_count,q.index_buffer_bytes)||q.index_buffer_gpu_va>UINT64_MAX-std::uint64_t(idx_stride)*q.first_index)
        return fail(WWMI_RT_IDENTIFY_QUERY);
    // A raw address is used only to reject draws that cannot resolve to any
    // already proved native BLAS component. All positive paths still locate
    // unique live allocations and run resolve's generation/order/IB checks.
    DrawIdentityQuery query{list->second.epoch,{},{},q.vertex_buffer_gpu_va,q.vertex_buffer_bytes,
        q.index_buffer_gpu_va+std::uint64_t(idx_stride)*q.first_index,std::uint64_t(idx_stride)*q.index_count,q.index_count};
    bool raw=identity_cache_->might_resolve(query);
    if(!raw&&!receipt&&!author_snapshot)return false;
    const auto *vertices=locate(q.vertex_buffer_gpu_va,q.vertex_buffer_bytes),*indices=locate(q.index_buffer_gpu_va,q.index_buffer_bytes);
    if(receipt){
        if(vertices){receipt->vertex_resource=vertices->key.object;receipt->vertex_creation=vertices->key.creation;}
        if(indices){receipt->index_resource=indices->key.object;receipt->index_creation=indices->key.creation;}
    }
    unsigned synth_reason=0,synth_found=0,synth_total=0;std::uint32_t synth_guid=0;
    if(!raw&&author_snapshot&&vertices&&indices&&!proved_mesh_ibs_.empty()){
        if(try_synthesize_from_proved_ib_locked(q,*vertices,*indices,idx_stride,synth_reason,synth_found,synth_total,synth_guid))
            raw=identity_cache_->might_resolve(query);
    }
    if(!raw){
        // Observe the existing exact write stamp when a known native profile
        // supplies this candidate byte window. Never treat it as draw identity.
        const PositionStamp *candidate=nullptr;
        std::uint32_t cand_guid=synth_guid;
        unsigned cand_comp=UINT32_MAX;
        bool ambiguous=false;
        if(vertices)for(const auto& profile:native_profiles->profiles)if(profile->bytes()<=q.vertex_buffer_bytes)
            for(const auto& component:profile->components)if(component.index_first==q.first_index&&component.index_count==q.index_count){
                const auto offset=q.vertex_buffer_gpu_va-vertices->base+12ull*component.first;
                const auto bytes=12ull*component.count;
                if(const auto *p=find_position_stamp_locked(vertices->key,offset,bytes)){
                    if(candidate&&candidate!=p)ambiguous=true;
                    else{candidate=p;cand_guid=profile->object_guid;cand_comp=component.component;}
                }
            }
        if(candidate&&!ambiguous){
            if(receipt){
                receipt->candidate_write_offset=candidate->offset;receipt->candidate_write_bytes=candidate->bytes;
                receipt->candidate_write_order=candidate->order;
                receipt->candidate_writer_list=candidate->producer.list;receipt->candidate_writer_creation=candidate->producer.creation;
                receipt->candidate_writer_recording=candidate->producer.generation;receipt->candidate_writer_submissions=candidate->submissions;
            }
            if(author_snapshot&&cand_guid&&synth_diag_logged_.size()<32&&
               synth_diag_logged_.emplace(cand_guid,(synth_reason<<8)|candidate->submissions).second){
                NativeIdentityFence dummy{};
                const bool have_fence=candidate->submissions==1&&lookup_skin_fence_locked(candidate->producer,dummy);
                char msg[256];
                std::snprintf(msg,sizeof(msg),"RT-ID-SYNTH-DIAG guid=%u comp=%u synth_reason=%u found=%u/%u cand_subs=%u have_fence=%u proved_ibs=%zu",
                    cand_guid,cand_comp,synth_reason,synth_found,synth_total,candidate->submissions,unsigned(have_fence),proved_mesh_ibs_.size());
                reshade::log::message(reshade::log::level::info,msg);
            }
        }
        return fail(WWMI_RT_IDENTIFY_RAW_SUBVIEW);
    }
    if(!vertices)return fail(WWMI_RT_IDENTIFY_VERTEX_RANGE);
    if(!indices)return fail(WWMI_RT_IDENTIFY_INDEX_RANGE);
    query.vertices=vertices->key;query.indices=indices->key;
    NativeDrawIdentityCache::LookupReceipt lookup;
    const auto match=author_snapshot?identity_cache_->resolve_snapshot(query,lookup):identity_cache_->resolve(query,receipt?&lookup:nullptr);
    if(receipt)receipt->exact_matches=lookup.exact_matches;
    auto live_source=[&](const NativeDrawIdentityCache::Match &m){
        const auto &s=m.identity->components[m.component].sample;
        const auto allocation=resources_.find(m.identity->allocation.object),content=resources_.find(s.content.object);
        return allocation!=resources_.end()&&allocation->second.key==m.identity->allocation&&current(allocation->second)&&
               content!=resources_.end()&&content->second.key==s.content&&current(content->second);
    };
    auto describe=[&](const NativeDrawIdentityCache::Match &m){
        if(!receipt)return;
        const auto &s=m.identity->components[m.component].sample;
        receipt->object_guid=s.profile->object_guid;receipt->content_hash=s.content_hash;receipt->component=s.component;
        receipt->content_resource=s.content.object;receipt->content_creation=s.content.creation;receipt->content_offset=s.content_offset;
        receipt->blas_resource=m.identity->allocation.object;receipt->blas_creation=m.identity->allocation.creation;
        receipt->blas_va=m.identity->native_blas;receipt->pose_order=s.order;
        receipt->writer_list=s.recording.list;receipt->writer_creation=s.recording.creation;receipt->writer_recording=s.recording.generation;
        receipt->submissions=m.submissions;receipt->producer_queue=m.producer.queue;receipt->producer_fence=m.producer.fence;
        receipt->producer_value=m.producer.value;receipt->observed_completion=m.observed_completion;
    };
    if(!match){
        if(!receipt)return false;
        if(lookup.ambiguous||lookup.exact_matches>1)return fail(WWMI_RT_IDENTIFY_AMBIGUOUS);
        if(!lookup.first_exact)return fail(WWMI_RT_IDENTIFY_RESOURCE_KEYS);
        if(!live_source(*lookup.first_exact))return fail(WWMI_RT_IDENTIFY_SOURCE_EXPIRED);
        describe(*lookup.first_exact);
        return fail(WWMI_RT_IDENTIFY_ORDER_PENDING);
    }
    if(!live_source(*match))return fail(WWMI_RT_IDENTIFY_SOURCE_EXPIRED);
    if(author_snapshot){
        const auto &s=match->identity->components[match->component].sample;
        const auto *write=find_position_stamp_locked(s.output,s.output_offset,s.output_bytes);
        if(!write||write->order!=s.order||write->producer!=s.recording||write->submissions!=match->submissions)
            return fail(WWMI_RT_IDENTIFY_SOURCE_EXPIRED);
    }
    if(match_out)*match_out=*match;
    const auto &c=match->identity->components[match->component];const auto &s=c.sample;const auto &n=s.profile->components[s.component];
    describe(*match);if(receipt)receipt->reason=match->ordering?WWMI_RT_IDENTIFY_OK:WWMI_RT_IDENTIFY_ORDER_PENDING;
    out={};out.struct_size=sizeof(out);out.abi_version=2;out.ordering=author_snapshot?0:match->ordering;auto &v=out.native;v.struct_size=sizeof(v);v.abi_version=1;
    v.object_guid=s.profile->object_guid;v.component=n.component;v.content_hash=s.content_hash;v.native_vertex_first=n.first;v.native_vertex_count=n.count;
    v.list=s.recording.list;v.list_creation=s.recording.creation;v.recording=s.recording.generation;
    v.content_resource=s.content.object;v.content_creation=s.content.creation;v.content_offset=s.content_offset;
    v.output_resource=s.output.object;v.output_creation=s.output.creation;v.position_base=c.vertex_address;v.output_offset=s.output_offset;v.output_bytes=s.output_bytes;
    v.scale=s.scale;v.native_extra_input=s.extra_position_input;v.native_delta_flags=s.native_delta_flags;v.palette_bytes=unsigned(s.palette.size()*sizeof(Matrix));
    if(palette&&capacity>=v.palette_bytes){std::memcpy(palette,s.palette.data(),v.palette_bytes);v.palette_copied_bytes=v.palette_bytes;}
    out.index_resource=c.indices.object;out.index_creation=c.indices.creation;out.index_offset=c.index_offset;
    out.producer_queue=match->producer.queue;out.producer_fence=match->producer.fence;out.producer_fence_value=match->producer.value;out.observed_completion=match->observed_completion;return true;
}
Assembly InputCollector::select(api::command_list *c,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc,std::shared_ptr<const NativeProfile> *identified,std::shared_ptr<const NativeBlasIdentity> *snapshot,SelectedIdentityState *snapshot_state){
    const auto native_profiles=registry_->snapshot();
    rt_cpu::Scope measured(rt_cpu::native_select);
    std::lock_guard lock(mutex_);auto list=lists_.find(id(c));if(list==lists_.end())return {PoseResult::wrong_recording,{}};
    if(identified)identified->reset();if(snapshot)snapshot->reset();
    if(snapshot_state)*snapshot_state={};
    ValidBlasGeoms valid;
    if(!collect_valid_blas_geoms(desc,valid))return {PoseResult::missing_producer,{}};
    const unsigned idx_stride=valid.idx_stride;
    for(const auto &profile:native_profiles->profiles){
        std::vector<unsigned> mapping;std::uint64_t ib_base=0;std::size_t matched_count=0;bool all_valid=false;
        if(!native_layout_candidate(*profile,desc,&mapping,&ib_base,&matched_count,&all_valid))continue;
        std::uint64_t shared_vb_va=0;bool shared_vb_consistent=true;
        const std::size_t comp_cnt=profile->components.size();
        std::vector<std::uint64_t> mapped_bases(comp_cnt,0);
        std::vector<std::uint64_t> index_vas(comp_cnt,0);
        bool ib_ok=true;
        for(unsigned i=0;i<comp_cnt;++i){
            const auto &n=profile->components[i];
            if(mapping[i]!=UINT32_MAX){
                const auto &g=blas_geom_desc(desc,mapping[i])->Triangles;
                mapped_bases[i]=g.VertexBuffer.StartAddress;
                index_vas[i]=g.IndexBuffer;
                if(!shared_vb_va)shared_vb_va=mapped_bases[i];
                else if(shared_vb_va!=mapped_bases[i])shared_vb_consistent=false;
            }else{
                const auto off=std::uint64_t(idx_stride)*n.index_first;
                if(ib_base>UINT64_MAX-off){ib_ok=false;break;}
                index_vas[i]=ib_base+off;
            }
        }
        if(!ib_ok)continue;
        bool all_ibs_live=true;
        for(unsigned i=0;i<comp_cnt;++i){
            if(!locate(index_vas[i],std::uint64_t(idx_stride)*profile->components[i].index_count)){all_ibs_live=false;break;}
        }
        if(!all_ibs_live)continue;
        const bool is_exact=(all_valid&&matched_count==comp_cnt&&desc.Inputs.NumDescs==comp_cnt&&desc.Inputs.DescsLayout==D3D12_ELEMENTS_LAYOUT_ARRAY);

        // Resolve all M components (including BLAS-omitted components such as
        // translucent/non-RT sections AND multi-ring FGPUSkinCache allocations)
        // from a single consistent live skin-writer recording.
        auto resolve_in_poses=[&](const PoseInputs &poses,unsigned expected_subs,
            std::vector<std::uint64_t> &bases_out,std::vector<const SkinSample *> &samples_out,BlasPoseInput &native_out)->bool {
            bases_out.assign(comp_cnt,0);
            samples_out.assign(comp_cnt,nullptr);
            native_out={poses.recording(),{}};
            native_out.geometries.resize(comp_cnt);
            ResourceKey proved_content{};std::uint64_t proved_content_off=0;
            std::uint64_t min_order=UINT64_MAX,max_order=0;
            // 1. Prove every BLAS-mapped component against its exact per-geometry VertexBuffer VA.
            for(unsigned i=0;i<comp_cnt;++i){
                if(mapping[i]==UINT32_MAX)continue;
                const auto &n=profile->components[i];
                const auto vb_va=mapped_bases[i];
                const auto *vr=locate(vb_va,profile->bytes());
                if(!vr)return false;
                const SkinSample *found=nullptr;
                for(const auto &s:poses.samples()){
                    if(s.profile!=profile||s.component!=i||!s.output_base||s.output_offset<12ull*n.first)continue;
                    const auto off=s.output_offset-12ull*n.first;
                    if(s.output_base>UINT64_MAX-off||s.output_base+off!=vb_va)continue;
                    if(s.output!=vr->key||!live_key(s.output)||!live_key(s.content))continue;
                    if(!position_stamp_matches_locked(s,expected_subs))continue;
                    if(found&&(found->output!=s.output||found->output_offset!=s.output_offset||found->content!=s.content||found->content_offset!=s.content_offset))return false;
                    found=&s;
                }
                if(!found)return false;
                if(!proved_content){proved_content=found->content;proved_content_off=found->content_offset;}
                else if(found->content!=proved_content||found->content_offset!=proved_content_off)return false;
                bases_out[i]=vb_va;
                samples_out[i]=found;
                native_out.geometries[i]={found->output,found->output_offset-12ull*n.first,profile->bytes()};
                min_order=(std::min)(min_order,found->order);
                max_order=(std::max)(max_order,found->order);
            }
            if(!proved_content)return false;
            // 2. Resolve every BLAS-omitted component directly from this same recording's
            //    CRC32C-proved SkinSamples matching (profile, component, content, content_offset),
            //    without requiring it to share a single FGPUSkinCache ring buffer with BLAS components.
            for(unsigned u=0;u<comp_cnt;++u){
                if(mapping[u]!=UINT32_MAX)continue;
                const auto &n=profile->components[u];
                const SkinSample *best=nullptr;std::uint64_t best_va=0;unsigned match_cnt=0;std::uint64_t best_dist=UINT64_MAX;
                for(const auto &s:poses.samples()){
                    if(s.profile!=profile||s.component!=u||s.content!=proved_content||s.content_offset!=proved_content_off)continue;
                    if(!s.output_base||s.output_offset<12ull*n.first||!live_key(s.output))continue;
                    const auto off=s.output_offset-12ull*n.first;
                    if(s.output_base>UINT64_MAX-off)continue;
                    const auto va=s.output_base+off;
                    const auto *vr=locate(va,profile->bytes());
                    if(!vr||vr->key!=s.output||!position_stamp_matches_locked(s,expected_subs))continue;
                    ++match_cnt;
                    const std::uint64_t dist=s.order<min_order?(min_order-s.order):(s.order>max_order?(s.order-max_order):0ull);
                    const bool prefer_shared=shared_vb_consistent&&shared_vb_va&&va==shared_vb_va;
                    const bool cur_shared=shared_vb_consistent&&shared_vb_va&&best_va==shared_vb_va;
                    if(!best||(prefer_shared&&!cur_shared)||(!cur_shared&&dist<best_dist)){
                        best=&s;best_va=va;best_dist=dist;
                    }
                }
                if(!best)return false;
                if(match_cnt>1&&!(shared_vb_consistent&&shared_vb_va&&best_va==shared_vb_va)&&best_dist>comp_cnt*2)return false;
                bases_out[u]=best_va;
                samples_out[u]=best;
                native_out.geometries[u]={best->output,best->output_offset-12ull*n.first,profile->bytes()};
            }
            return true;
        };

        std::vector<std::uint64_t> bases;
        std::vector<const SkinSample *> samples;
        BlasPoseInput native;
        bool resolved=false;
        bool resolved_same_cl=false;
        unsigned resolved_subs=0;
        NativeIdentityFence resolved_producer{};

        // Search order: (1) this BLAS command list, (2) other open parallel skin CLs,
        // (3) already-submitted-once skin CLs with a real GPU queue fence.
        if(resolve_in_poses(list->second.poses,0,bases,samples,native)){
            resolved=true;resolved_same_cl=true;resolved_subs=0;
        }else{
            for(const auto *owner:pose_owners_){
                if(owner==&list->second||!owner->has_skin_sample)continue;
                if(resolve_in_poses(owner->poses,0,bases,samples,native)){
                    resolved=true;resolved_same_cl=false;resolved_subs=0;break;
                }
            }
            if(!resolved){
                std::vector<Recording> tried;
                for(auto it=position_stamps_.rbegin();it!=position_stamps_.rend();++it){
                    const auto &p=*it;
                    if(p.submissions!=1)continue;
                    if(std::find_if(tried.begin(),tried.end(),[&](const Recording &v){return v==p.producer;})!=tried.end())continue;
                    tried.push_back(p.producer);
                    NativeIdentityFence fence{};
                    if(!lookup_skin_fence_locked(p.producer,fence))continue;
                    const auto *poses=poses_for_submitted_stamp_locked(p);
                    if(!poses)continue;
                    if(resolve_in_poses(*poses,1,bases,samples,native)){
                        resolved=true;resolved_same_cl=false;resolved_subs=1;resolved_producer=fence;break;
                    }
                }
            }
        }
        if(!resolved)continue;
        if(identified)*identified=profile;
        if(snapshot){
            const auto *allocation=locate(desc.DestAccelerationStructureData,1);
            if(allocation){
                auto value=std::make_shared<NativeBlasIdentity>();
                value->recording=native.recording;value->allocation=allocation->key;value->native_blas=desc.DestAccelerationStructureData;
                value->geometries.reserve(desc.Inputs.NumDescs);
                for(unsigned gi=0;gi<desc.Inputs.NumDescs;++gi)if(const auto *gp=blas_geom_desc(desc,gi))value->geometries.push_back(*gp);
                bool complete=true;
                for(unsigned i=0;i<comp_cnt;++i){
                    const auto &n=profile->components[i];
                    const auto *sample=samples[i];
                    const auto *indices=locate(index_vas[i],std::uint64_t(idx_stride)*n.index_count);
                    const auto *vertices=locate(bases[i],profile->bytes());
                    if(!sample||sample->profile!=profile||!indices||!vertices||vertices->key!=native.geometries[i].vertex){complete=false;break;}
                    value->components.push_back({*sample,native.geometries[i],indices->key,bases[i],index_vas[i],index_vas[i]-indices->base,n.index_count,idx_stride});
                }
                if(complete){
                    remember_proved_ib_locked(*value);
                    *snapshot=std::move(value);
                    if(snapshot_state)*snapshot_state={resolved_subs,resolved_producer};
                }
            }
        }
        if(!is_exact||!resolved_same_cl)return {PoseResult::missing_producer,{}};
        const auto pack=std::find_if(packs_.begin(),packs_.end(),[&](const auto &p){return p->native==profile;});
        if(pack==packs_.end())return {PoseResult::missing_producer,{}};
        std::vector<FinalPosition> final_positions((*pack)->components.size());
        if(position_)for(unsigned i=0;i<final_positions.size();++i){WWMI_RtModPosition_v1 source{};const auto &epoch=list->second.epoch;
            const auto status=position_(epoch.list,epoch.creation,epoch.generation,(*pack)->load_generation,(*pack)->key,(*pack)->revision,int32_t((*pack)->components[i].native.component),&source,sizeof(source));
            if(status!=WWMI_RT_MOD_OK)continue;
            // A successful FINAL acquire transfers one resource reference even
            // when a later descriptor check rejects the input.
            std::shared_ptr<void> hold;
            if(source.mode==WWMI_RT_POSITION_FINAL&&source.resource)hold=std::shared_ptr<void>(reinterpret_cast<ID3D12Resource *>(source.resource),[](void *p){static_cast<ID3D12Resource *>(p)->Release();});
            if(source.struct_size!=sizeof(source)||source.abi_version!=1||source.load_generation!=(*pack)->load_generation||source.mod_key!=(*pack)->key||source.mod_revision!=(*pack)->revision)continue;
            auto &out=final_positions[i];
            if(source.mode==WWMI_RT_POSITION_REST_VERIFIED&&!(*pack)->shapekey_vertices){out.mode=source.mode;continue;}
            if(source.mode!=WWMI_RT_POSITION_FINAL||!hold||!source.resource_creation||source.stride!=12||source.bytes!=12ull*source.vertex_count||
                !(source.resource_state&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))continue;
            auto *resource=static_cast<ID3D12Resource *>(hold.get());const auto rd=resource->GetDesc();ComPtr<ID3D12Device> source_device;
            if(rd.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||source.resource_offset>rd.Width||source.bytes>rd.Width-source.resource_offset||
                FAILED(resource->GetDevice(IID_PPV_ARGS(&source_device)))||source_device.Get()!=reinterpret_cast<ID3D12Device *>(device_->get_native()))continue;
            const auto address=resource->GetGPUVirtualAddress();if(!address||address>UINT64_MAX-source.resource_offset)continue;
            out.mode=source.mode;out.resource={source.resource,source.resource_creation};out.address=address+source.resource_offset;out.bytes=source.bytes;
            out.first_vertex=source.first_vertex;out.vertex_count=source.vertex_count;out.hold=std::move(hold);
        }
        rt_cpu::Scope pose_measured(rt_cpu::pose_assemble);return list->second.poses.assemble(native,*pack,std::move(final_positions));
    }
    return {PoseResult::missing_producer,{}};
}
bool InputCollector::discovery_interested()const noexcept{
    if(!native_receipts_)return false;const auto state=registry_->snapshot();const auto interests=std::atomic_load(&discovery_interests_);
    return std::any_of(interests->begin(),interests->end(),[&](const auto &i){return NativeProfileRegistry::resolve(state,i).match==NativeProfileRegistry::Match::missing;});
}
void InputCollector::native_copy(api::command_list *c,api::resource source,std::uint64_t from,api::resource dest,std::uint64_t to,std::uint64_t bytes,bool recorded){
    std::lock_guard lock(mutex_);const auto d=resources_.find(dest.handle),s=resources_.find(source.handle);const auto list=lists_.find(id(c));
    if(d==resources_.end()||list==lists_.end())return;
    const NativeRange range{d->second.key,to,bytes,d->second.base};
    if(recorded){
        discovery_.copy_forwarded(list->second.epoch,range);
        for(auto &p:content_)if(p.resource==range.resource&&p.offset==to&&p.producer==list->second.epoch)p.forwarded=true;
        refresh_native_copy_mask_locked();
        return;
    }
    // The SDK pre-event already invalidated static consumer state. Capture
    // destination content here only after all cancellation callbacks returned.
    discovery_.written(range);
    if(s!=resources_.end()&&current(d->second)&&identity::fits(to,bytes,d->second.bytes)&&discovery_.needs_copy_bytes(range)&&bytes<=NativeDiscovery::max_probe_bytes){
        std::vector<unsigned char> data(static_cast<std::size_t>(bytes));
        if(read(s->second,from,bytes,data.data())){
            arm_native_copy_mask(kCopyBeforeActive|kCopyRecActive);
            discovery_.copy(range,list->second.epoch,data);
        }
    }
    refresh_native_copy_mask_locked();
}
void InputCollector::native_written(api::resource r,std::uint64_t offset,std::uint64_t bytes){std::lock_guard lock(mutex_);const auto it=resources_.find(r.handle);if(it!=resources_.end())written(it->second.key,offset,bytes);}
void InputCollector::native_unknown(){std::lock_guard lock(mutex_);discovery_.unknown();for(const auto &[_,r]:resources_)written(r.key,0,r.bytes);refresh_native_copy_mask_locked();}
void InputCollector::native_skin_forwarded(api::command_list *c,bool forwarded){
    std::lock_guard lock(mutex_);const auto list=lists_.find(id(c));
    if(list==lists_.end())return;
    discovery_.skin_forwarded(list->second.epoch,forwarded);
    if(has_skip_intents_){
        for(auto it=aux_skin_writers_.rbegin();it!=aux_skin_writers_.rend();++it){
            if(it->recording==list->second.epoch&&!it->forwarded){
                if(forwarded)it->forwarded=true;
                else aux_skin_writers_.erase(std::next(it).base());
                break;
            }
        }
    }
    refresh_native_copy_mask_locked();
}
void InputCollector::native_blas(api::command_list *c,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc,bool recorded){
    std::lock_guard lock(mutex_);const auto list=lists_.find(id(c));if(list==lists_.end())return;
    if(recorded){discovery_.build_forwarded(list->second.epoch,desc.DestAccelerationStructureData);return;}
    if(!discovery_.interested()||desc.Inputs.Type!=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL||!desc.Inputs.NumDescs)return;
    discovery_.note_blas();
    ValidBlasGeoms valid;
    if(!collect_valid_blas_geoms(desc,valid)){discovery_.note_failure(NativeDiscovery::Failure::geometry);return;}
    const unsigned idx_stride=valid.idx_stride,max_n=valid.max_n;
    std::vector<NativeDiscovery::Geometry> geometries;geometries.reserve(valid.count);
    for(unsigned vi=0;vi<valid.count;++vi){const auto &g=blas_geom_desc(desc,valid.idx[vi])->Triangles;
        const auto *vb=locate(g.VertexBuffer.StartAddress,12ull*max_n),*ib=locate(g.IndexBuffer,std::uint64_t(idx_stride)*g.IndexCount);if(!vb||!ib){discovery_.note_failure(NativeDiscovery::Failure::blas_resource);return;}
        NativeDiscovery::Geometry dg{};
        dg.vertices={vb->key,g.VertexBuffer.StartAddress-vb->base,12ull*max_n,vb->base};
        dg.indices={ib->key,g.IndexBuffer-ib->base,std::uint64_t(idx_stride)*g.IndexCount,ib->base};
        dg.vertex_address=g.VertexBuffer.StartAddress;dg.index_address=g.IndexBuffer;dg.ib_resource_bytes=ib->bytes;
        dg.vertices_count=g.VertexCount;dg.index_count=g.IndexCount;dg.index_stride=idx_stride;dg.layout_confirmed=true;
        geometries.push_back(dg);
    }
    arm_native_copy_mask(kCopyBeforeActive);
    discovery_.build(list->second.epoch,desc.DestAccelerationStructureData,std::move(geometries));
    refresh_native_copy_mask_locked();
}
std::vector<std::shared_ptr<NativeDiscovery::Bytes>> InputCollector::native_requests(api::command_list *c,NativeDiscovery::ReadPoint point){std::lock_guard lock(mutex_);const auto list=lists_.find(id(c));return list==lists_.end()?std::vector<std::shared_ptr<NativeDiscovery::Bytes>>{}:discovery_.requests(list->second.epoch,point);}
bool InputCollector::native_request_input(const NativeDiscovery::Bytes &p,std::uint64_t &address,bool &cpu){
    std::lock_guard lock(mutex_);const auto it=resources_.find(p.range.resource.object);if(it==resources_.end())return false;const auto &r=it->second;
    if(!p.valid||r.key!=p.range.resource||!current(r)||!identity::fits(p.range.offset,p.range.bytes,r.bytes)||!r.base||r.base>UINT64_MAX-p.range.offset||!unique_current(r,r.base+p.range.offset,p.range.bytes))return false;
    address=r.base+p.range.offset;cpu=r.heap==api::memory_heap::upload;return true;
}
bool InputCollector::native_request_cpu(const NativeDiscovery::Bytes &p){
    std::lock_guard lock(mutex_);const auto it=resources_.find(p.range.resource.object);if(it==resources_.end()||it->second.key!=p.range.resource||!p.valid)return false;
    std::vector<unsigned char> data(static_cast<std::size_t>(p.range.bytes));
    if(!read(it->second,p.range.offset,p.range.bytes,data.data()))return false;
    const std::uint32_t aux_hash=(has_skip_intents_&&data.size()>=12)?native_mesh_content_hash(data):0u;
    if(!discovery_.provide_cpu(p.id,std::move(data)))return false;
    if(aux_hash)record_aux_rest_hash_locked(p.range,aux_hash);
    return true;
}
void InputCollector::native_request_cancel(std::uint64_t id){
    std::lock_guard lock(mutex_);
    if(has_skip_intents_){
        if(const auto rng=discovery_.range_of(id)){
            for(auto &t:aux_probe_targets_){
                if(t.rest==rng->resource&&t.rest_offset==rng->offset&&t.cancel_retries<4){
                    if(std::erase(t.tried_bytes,rng->bytes)>0)++t.cancel_retries;
                    break;
                }
            }
        }
    }
    discovery_.cancel(id);
    refresh_native_copy_mask_locked();
}
void InputCollector::native_submission_started(Recording rec){std::lock_guard lock(mutex_);discovery_.submission_started(rec);refresh_native_copy_mask_locked();}
void InputCollector::native_signalled(std::uint64_t id,std::uint64_t queue,std::uint64_t fence,std::uint64_t value){std::lock_guard lock(mutex_);discovery_.signalled(id,queue,fence,value);}
bool InputCollector::native_completed(std::uint64_t id,std::uint64_t fence,std::uint64_t value,std::vector<unsigned char> data){
    std::lock_guard lock(mutex_);const auto key=discovery_.resource_of(id);
    if(!key||!live_key(key))return false;
    const auto rng=has_skip_intents_?discovery_.range_of(id):std::optional<NativeRange>{};
    const std::uint32_t aux_hash=(rng&&data.size()>=12)?native_mesh_content_hash(data):0u;
    if(!discovery_.completed(id,fence,value,std::move(data)))return false;
    if(rng&&aux_hash)record_aux_rest_hash_locked(*rng,aux_hash);
    refresh_native_copy_mask_locked();
    return true;
}
void InputCollector::record_aux_rest_hash_locked(const NativeRange &range,std::uint32_t content_hash){
    if(!content_hash)return;
    const bool known=std::any_of(aux_rest_hashes_.begin(),aux_rest_hashes_.end(),[&](const AuxRestHash &h){
        return h.rest==range.resource&&h.rest_offset==range.offset&&h.rest_bytes==range.bytes&&h.content_hash==content_hash;
    });
    if(!known){
        if(aux_rest_hashes_.size()>=256)aux_rest_hashes_.erase(aux_rest_hashes_.begin());
        aux_rest_hashes_.push_back({range.resource,range.offset,range.bytes,content_hash});
    }
    const bool matches_skip=std::any_of(intents_.begin(),intents_.end(),[&](const NativeMeshIntent &i){
        return i.enabled&&i.skip_only&&i.content_hash==content_hash;
    });
    if(matches_skip){
        std::erase_if(aux_probe_targets_,[&](const AuxRestProbeTarget &t){
            return t.rest==range.resource&&t.rest_offset==range.offset;
        });
    }else{
        std::erase_if(aux_probe_targets_,[&](const AuxRestProbeTarget &t){
            return t.rest==range.resource&&t.rest_offset==range.offset&&t.tried_bytes.size()>=8;
        });
    }
}
std::uint32_t InputCollector::identify_aux_skip_draw(const WWMI_RtPackDrawQuery_v2 &q,std::uint32_t &out_content_hash){
    out_content_hash=0;
    std::lock_guard lock(mutex_);
    if(!has_skip_intents_||!q.vertex_buffer_gpu_va||!q.vertex_buffer_bytes||q.base_vertex!=0||q.vertex_stride!=12)return 0;
    const auto draw_list=lists_.find(q.list);
    if(draw_list==lists_.end())return 0;
    const Resource *vb=locate(q.vertex_buffer_gpu_va,q.vertex_buffer_bytes);
    if(!vb)return 0;
    const AuxSkinWriter *same_rec_match=nullptr;
    const AuxSkinWriter *submitted_match=nullptr;
    for(const auto &w:aux_skin_writers_){
        if(!w.forwarded||w.output!=vb->key||!w.count)continue;
        if(w.output_base>UINT64_MAX-w.output_offset)continue;
        const std::uint64_t w_va=w.output_base+w.output_offset;
        if(w_va<q.vertex_buffer_gpu_va||w.output_bytes>q.vertex_buffer_bytes)continue;
        const std::uint64_t delta=w_va-q.vertex_buffer_gpu_va;
        if(delta>q.vertex_buffer_bytes-w.output_bytes)continue;
        const bool aligned=(delta==12ull*w.first)||(delta==0&&q.vertex_buffer_bytes==w.output_bytes);
        if(!aligned)continue;
        if(w.recording==draw_list->second.epoch){
            if(!same_rec_match||w.order>same_rec_match->order)same_rec_match=&w;
        }else if(w.submissions==1){
            if(!submitted_match||w.order>submitted_match->order)submitted_match=&w;
        }
    }
    const AuxSkinWriter *matched=same_rec_match?same_rec_match:submitted_match;
    if(!matched)return 0;
    std::size_t hash_count_for_rest=0;
    for(const auto &h:aux_rest_hashes_){
        if(h.rest!=matched->rest||h.rest_offset!=matched->rest_offset)continue;
        ++hash_count_for_rest;
        const bool want=std::any_of(intents_.begin(),intents_.end(),[&](const NativeMeshIntent &i){
            if(!i.skip_only||!i.enabled||i.content_hash!=h.content_hash)return false;
            return std::any_of(i.components.begin(),i.components.end(),[&](const NativeIndexWindow &c){
                return c.first==q.first_index&&c.count==q.index_count;
            });
        });
        if(want){out_content_hash=h.content_hash;return 2;}
    }
    if(hash_count_for_rest>=8)return 1;
    std::uint64_t cur_max_end=12ull*(std::uint64_t(matched->first)+matched->count);
    for(const auto &w:aux_skin_writers_){
        if(!w.forwarded||w.rest!=matched->rest||w.rest_offset!=matched->rest_offset)continue;
        if(w.recording!=draw_list->second.epoch&&w.submissions!=1)continue;
        const std::uint64_t we=12ull*(std::uint64_t(w.first)+w.count);
        if(we>cur_max_end&&we<=matched->rest_bytes)cur_max_end=we;
    }
    auto has_hashed_size=[&](std::uint64_t sz){
        return std::any_of(aux_rest_hashes_.begin(),aux_rest_hashes_.end(),[&](const AuxRestHash &h){
            return h.rest==matched->rest&&h.rest_offset==matched->rest_offset&&h.rest_bytes==sz;
        });
    };
    const bool need_probe=(cur_max_end>=12&&!has_hashed_size(cur_max_end))||(matched->rest_bytes>=12&&!has_hashed_size(matched->rest_bytes));
    if(need_probe){
        auto pt=std::find_if(aux_probe_targets_.begin(),aux_probe_targets_.end(),[&](const AuxRestProbeTarget &t){
            return t.rest==matched->rest&&t.rest_offset==matched->rest_offset;
        });
        if(pt!=aux_probe_targets_.end()){
            if(cur_max_end>pt->max_end_bytes)pt->max_end_bytes=cur_max_end;
        }else{
            if(aux_probe_targets_.size()>=64)aux_probe_targets_.erase(aux_probe_targets_.begin());
            aux_probe_targets_.push_back({matched->rest,matched->rest_offset,matched->rest_bytes,cur_max_end,{}});
        }
    }
    return 1;
}
void InputCollector::publish_discovery(){
    std::lock_guard lock(mutex_);discovery_.publish();
    for(auto &[bytes,profile]:discovery_.content_bindings())if(live_key(bytes->range.resource)&&std::none_of(content_.begin(),content_.end(),[&](const auto &p){return p.resource==bytes->range.resource&&p.offset==bytes->range.offset&&p.profile==profile;}))
        content_.push_back({bytes->range.resource,bytes->range.offset,bytes->recording,true,profile,true});
    refresh_discovery_interest_locked();
    refresh_native_copy_mask_locked();
}

}
