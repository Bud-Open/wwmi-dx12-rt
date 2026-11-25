#pragma once
#include "raygen_identity.hpp"
#include "wwmi_rt_ray_as_slot.h"
#include <cstdio>

namespace hsin_rt {
using ReadAsSlotFn=decltype(&WWMI_ComputeState_ReadAsTableSlot_v1);
using ReadComputeStateFn=decltype(&WWMI_ComputeState_Query_v1);
using ReadRayAsSlotFn=decltype(&WWMI_ComputeState_ReadRayAsSlot_v1);

inline constexpr unsigned scene_srv_parameter=4,scene_sampler_parameter=5,scene_uav_parameter=6;
struct RayOutputSlot { unsigned offset,stride; };
inline constexpr RayOutputSlot occlusion_output_slot{0,4},intersection_output_slot{1,20};

struct RaySceneInput {
    enum Stage : unsigned { state, raygen_state, raygen_range, raygen_upload, raygen_identifier, root_layout, as_slot, typed_null, bound, stages };
    Stage stage=state;
    enum class RaygenProof { none, record_bytes, closed_program, equivalent_as_slot };
    RaygenProof raygen_proof=RaygenProof::none;
    unsigned as_offset=UINT32_MAX;
    int status=0;
    WWMI_ComputeStateInfo_v1 compute{};
    std::optional<RaygenIdentity::Active> program;
    InputCollector::RayInputRange raygen,hit;
    WWMI_AsTableSlot_v1 as{};
};
// These pins use the same AS and Rays inputs, but their output register
// and payload stores differ. This is not an Intersection replay capability.
inline const RayOutputSlot *ray_output_slot(const RaySceneInput &ray){
    if(ray.stage!=RaySceneInput::bound||!ray.program||ray.as.parameter!=scene_srv_parameter||
       ray.as.table_offset!=0||!ray.as.location||ray.program->name->payload!=RayPayload::default_payload)return nullptr;
    if(!std::strcmp(ray.program->name->entry,"OcclusionMainRG"))return &occlusion_output_slot;
    if(!std::strcmp(ray.program->name->entry,"IntersectionMainRG"))return &intersection_output_slot;
    return nullptr;
}
inline bool is_occlusion(const RaySceneInput &ray){
    return ray_output_slot(ray)==&occlusion_output_slot;
}
inline bool occlusion_known_no_procedural(const RaySceneInput &ray){
    return is_occlusion(ray)&&ray.program->known_no_procedural();
}

// A Build seen at the native TOP_LEVEL/ARRAY forwarder. This says which
// bytes the Build asks to read, not who last wrote them or which row is ours.
struct RayBuildUse {
    Recording recording;
    ResourceKey destination;
    std::uint64_t address=0,instances_va=0,order=0;
    InputCollector::RayInputRange instances;
    unsigned count=0,flags=0,submissions=0;
    NativeIdentityFence submit{};
};

struct OcclusionTableLayout {
    bool has_outputs=false;
    // UINT32_MAX denotes legacy (v1.0) layout without range flags.
    unsigned p4_flags=UINT32_MAX,p6_flags=UINT32_MAX;
    uint64_t generation=0;
    bool has_probe_uav=false;
    bool immutable_descriptors()const{
        constexpr auto volatile_bit=unsigned(api::descriptor_range_flags::descriptors_volatile);
        return has_outputs&&p4_flags!=UINT32_MAX&&p6_flags!=UINT32_MAX&&
            !((p4_flags|p6_flags)&volatile_bit);
    }
    bool probe_data_supported()const{
        using F=api::descriptor_range_flags;
        const auto static_data=[](unsigned flags){
            return flags!=UINT32_MAX&&(flags&unsigned(F::data_static));
        };
        // 1.0 is volatile. 1.1 DATA_STATIC would promise that the game's
        // SBT contents are already fixed during CPU command recording.
        return has_probe_uav&&!static_data(p4_flags)&&!static_data(p6_flags);
    }
};
class RaySceneConsumer {
    // The established p4 range suffices for all pins. MainRG's private
    // continuation additionally needs the actual p5 sampler and p6 UAV layout.
    std::unordered_map<std::uint64_t,OcclusionTableLayout> table_layouts_;
    // Two actual local layouts used by the pinned 0x33 MainRG family.
    // Values are argument bytes following the shader identifier, not a
    // promise about record bytes or an association inferred from a name.
    std::unordered_map<std::uint64_t,unsigned> local_argument_bytes_;
    uint64_t next_layout_=0;
    // A single original-world arm. At most one row for each actual program
    // and terminal stage. Missing bindings are not displaced by successes.
    std::array<unsigned,std::size(known_raygens)+1> reported_{};
    struct BuildCandidate : RayBuildUse { bool report_submit=false; };
    // Only nearby native-forward TOP_LEVEL metadata, never the instance
    // array. Native queue-after separately confirms actual Execute.
    std::array<BuildCandidate,16> builds_{};
    unsigned next_build_=0;
    std::uint64_t next_build_order_=0;
    // A report already delivered its Build candidate. Keep exactly that
    // candidate until its own native post-Execute, even after the arm ends.
    std::unordered_multimap<std::uint64_t,BuildCandidate *> awaiting_submit_;
    template<class F> void submission_candidates(Recording epoch,bool armed,F update){
        if(armed){
            for(auto &b:builds_)if(b.recording.list&&b.recording==epoch)update(b);
            return;
        }
        const auto pending=awaiting_submit_.equal_range(epoch.list);
        for(auto i=pending.first;i!=pending.second;++i)if(i->second->recording==epoch)update(*i->second);
    }
    BuildCandidate *find_build(Recording epoch,ResourceKey allocation,std::uint64_t address,unsigned &matches){
        BuildCandidate *matched=nullptr;
        matches=0;
        if(allocation)for(unsigned n=0;n<(std::min)(next_build_,unsigned(builds_.size()));++n){
            auto &candidate=builds_[(next_build_-1-n)%builds_.size()];
            if(candidate.address!=address||candidate.destination!=allocation)continue;
            ++matches;
            if(candidate.recording==epoch)return &candidate;
            if(matches==1)matched=&candidate;
        }
        return matches==1?matched:nullptr;
    }
    static bool table_range(const api::pipeline_layout_param &p,api::descriptor_type type,unsigned count){
        const api::descriptor_range *r=nullptr;
        if(p.type==api::pipeline_layout_param_type::descriptor_table&&p.descriptor_table.count==1)
            r=p.descriptor_table.ranges;
        else if(p.type==api::pipeline_layout_param_type::descriptor_table_with_flags&&p.descriptor_table_with_flags.count==1)
            r=p.descriptor_table_with_flags.ranges;
        return r&&r->type==type&&r->dx_register_space==1&&r->dx_register_index==0&&r->binding==0&&
            r->count==count&&r->visibility==api::shader_stage::all;
    }
    static unsigned table_flags(const api::pipeline_layout_param &p){
        return p.type==api::pipeline_layout_param_type::descriptor_table_with_flags?
            unsigned(p.descriptor_table_with_flags.ranges[0].flags):UINT32_MAX;
    }
    static std::optional<unsigned> mainrg_local_arguments(unsigned count,const api::pipeline_layout_param *p){
        using T=api::pipeline_layout_param_type;
        if(!count)return 0;
        if(count!=3||!p||p[2].type!=T::push_constants)return {};
        for(unsigned i=0;i<2;++i){
            const api::descriptor_range *r=nullptr;
            if(p[i].type==T::push_descriptors)r=&p[i].push_descriptors;
            else if(p[i].type==T::push_descriptors_with_ranges_and_flags&&
                    p[i].descriptor_table_with_flags.count==1)r=p[i].descriptor_table_with_flags.ranges;
            if(!r||r->type!=api::descriptor_type::buffer_shader_resource_view||r->binding||
               r->dx_register_index!=i||r->dx_register_space!=2||r->count!=1||
               r->visibility!=api::shader_stage::all)return {};
        }
        const auto &c=p[2].push_constants;
        if(c.binding||c.dx_register_index||c.dx_register_space!=2||c.count!=4||
           c.visibility!=api::shader_stage::all)return {};
        return 2*sizeof(D3D12_GPU_VIRTUAL_ADDRESS)+4*sizeof(UINT);
    }
public:
    RaygenIdentity raygens;
    bool enabled=false;
    void init_pipeline(uint64_t device,api::pipeline state,unsigned count,const api::pipeline_subobject *subobjects,
                       WWMI_RtCurrentNativeStateObjectFn_v1 read_native){
        raygens.init(state,count,subobjects);
        if(!read_native)return;
        WWMI_RtNativeStateObject_v1 view{};
        if(read_native(state.handle,&view,sizeof(view))||view.state!=state.handle||view.device!=device)return;
        raygens.init_native(view,count,subobjects,[&](uint64_t root){
            const auto found=table_layouts_.find(root);
            return found==table_layouts_.end()?uint64_t{}:found->second.generation;
        },[&](uint64_t root)->std::optional<unsigned>{
            const auto found=local_argument_bytes_.find(root);
            if(found==local_argument_bytes_.end())return {};
            return found->second;
        });
    }
    void layout(api::pipeline_layout key,unsigned count,const api::pipeline_layout_param *params){
        table_layouts_.erase(key.handle);
        local_argument_bytes_.erase(key.handle);
        if(!enabled||!key.handle)return;
        if(const auto local=mainrg_local_arguments(count,params))local_argument_bytes_.emplace(key.handle,*local);
        if(count<=scene_srv_parameter||!params||
           !table_range(params[scene_srv_parameter],api::descriptor_type::shader_resource_view,64))return;
        const bool probe_uav=count>scene_uav_parameter&&
            table_range(params[scene_uav_parameter],api::descriptor_type::unordered_access_view,16);
        const bool outputs=probe_uav&&table_range(params[scene_sampler_parameter],api::descriptor_type::sampler,16);
        // p4-only layouts still provide the independent AS/Build input read.
        table_layouts_.emplace(key.handle,OcclusionTableLayout{outputs,table_flags(params[scene_srv_parameter]),
            probe_uav?table_flags(params[scene_uav_parameter]):UINT32_MAX,++next_layout_,probe_uav});
    }
    void destroy_layout(api::pipeline_layout key){table_layouts_.erase(key.handle);local_argument_bytes_.erase(key.handle);}
    std::optional<OcclusionTableLayout> occlusion_layout(std::uint64_t key)const{
        const auto found=table_layouts_.find(key);
        if(found==table_layouts_.end()||!found->second.has_outputs)return {};
        return found->second;
    }
    std::optional<OcclusionTableLayout> raygen_probe_layout(std::uint64_t key)const{
        const auto found=table_layouts_.find(key);
        return found==table_layouts_.end()?std::nullopt:std::optional<OcclusionTableLayout>{found->second};
    }
    std::optional<RayBuildUse> build_candidate(InputCollector &inputs,api::command_list *list,std::uint64_t destination,
                         std::uint64_t instances,unsigned count,unsigned flags){
        if(!destination||!instances||!count)return {};
        const auto epoch=inputs.recording(list);
        if(!epoch.list||!epoch.creation||!epoch.generation)return {};
        const auto allocation=inputs.resource_at(destination);
        const auto view=inputs.ray_input_range(instances,64ull*count);
        const auto order=++next_build_order_;
        for(size_t i=0;i<builds_.size();++i){
            auto &slot=builds_[next_build_++%builds_.size()];
            if(slot.report_submit)continue;
            slot={{epoch,allocation,destination,instances,order,view,count,flags}};
            return static_cast<const RayBuildUse &>(slot);
        }
        return {};
    }
    bool observed_build()const{return next_build_!=0;}
    bool awaiting_submit(Recording epoch)const{
        const auto pending=awaiting_submit_.equal_range(epoch.list);
        for(auto i=pending.first;i!=pending.second;++i)if(i->second->recording==epoch)return true;
        return false;
    }
    void submitted(Recording epoch,std::uint64_t queue,bool armed){
        submission_candidates(epoch,armed,[&](BuildCandidate &b){
            if(b.submissions<2)++b.submissions;
            b.submit={queue,0,0};
        });
    }
    void signalled(Recording epoch,NativeIdentityFence signal,bool armed){
        submission_candidates(epoch,armed,[&](BuildCandidate &b){
            if(b.submissions&&b.submit.queue==signal.queue)b.submit=signal;
        });
    }
    void report_submitted(Recording epoch){
        const auto pending=awaiting_submit_.equal_range(epoch.list);
        for(auto i=pending.first;i!=pending.second;){
            auto &b=*i->second;
            if(b.recording!=epoch||!b.submissions){++i;continue;}
            b.report_submit=false;
            i=awaiting_submit_.erase(i);
            char msg[320];
            std::snprintf(msg,sizeof(msg),"RT-RAY-INPUT-SUBMIT record=%llx/%llu/%llu as=%llx alloc=%llx/%llu native_submits=%u queue=%llx fence=%llx/%llu",
                (unsigned long long)epoch.list,(unsigned long long)epoch.creation,(unsigned long long)epoch.generation,
                (unsigned long long)b.address,(unsigned long long)b.destination.object,(unsigned long long)b.destination.creation,
                b.submissions,(unsigned long long)b.submit.queue,(unsigned long long)b.submit.fence,(unsigned long long)b.submit.value);
            reshade::log::message(reshade::log::level::info,msg);
        }
    }
    std::optional<RaySceneInput> read(InputCollector &inputs,api::command_list *list,
        const D3D12_GPU_VIRTUAL_ADDRESS_RANGE &raygen,const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE &hit,
        ReadComputeStateFn query,ReadAsSlotFn read_as,ReadRayAsSlotFn read_ray_as=nullptr){
        if(!enabled||!raygens.has_programs()||!query||!read_as)return {};
        RaySceneInput result;
        const auto c=reinterpret_cast<std::uint64_t>(list);
        result.compute.size=sizeof(result.compute);
        result.status=query(c,&result.compute);
        const bool table_only=result.status==2&&read_ray_as&&result.compute.pipeline_kind==1&&
            result.compute.pipeline&&result.compute.root_signature;
        if(result.status&&!table_only)return result;
        if(result.compute.pipeline_kind!=1||!raygens.has_pipeline(result.compute.pipeline)){
            if(!table_only)return {};
            result.stage=RaySceneInput::raygen_state;
            return result;
        }
        result.status=0; // From here report the actual read/identification stage.
        result.stage=RaySceneInput::raygen_range;
        std::array<std::byte,D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES> head;
        if(!raygen.StartAddress||raygen.StartAddress%D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT||
           raygen.SizeInBytes<head.size()||raygen.SizeInBytes>D3D12_RAYTRACING_MAX_SHADER_RECORD_STRIDE||
           raygen.SizeInBytes%D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT)return result;
        result.raygen=inputs.ray_input_range(raygen.StartAddress,raygen.SizeInBytes,head);
        if(!result.raygen.key)return result;
        result.stage=RaySceneInput::raygen_upload;
        const auto layout=table_layouts_.find(result.compute.root_signature);
        if(result.raygen.upload_read){
            result.stage=RaySceneInput::raygen_identifier;
            result.program=raygens.select({result.compute.pipeline},head);
            if(result.program)result.as_offset=result.program->name->as_register;
            else{
                if(layout==table_layouts_.end())return result;
                const auto slot=raygens.reflected_record_as({result.compute.pipeline},layout->first,layout->second.generation,head);
                if(!slot)return result;
                result.as_offset=*slot;
            }
            result.raygen_proof=RaySceneInput::RaygenProof::record_bytes;
        }else{
            if(result.raygen.heap!=api::memory_heap::default_||layout==table_layouts_.end())return result;
            const auto fixed=raygens.fixed_as({result.compute.pipeline},layout->first,layout->second.generation);
            if(!fixed)return result;
            result.as_offset=fixed->offset;
            result.program=fixed->same_program;
            result.raygen_proof=result.program?RaySceneInput::RaygenProof::closed_program:RaySceneInput::RaygenProof::equivalent_as_slot;
        }
        result.stage=RaySceneInput::root_layout;
        if(layout==table_layouts_.end())return result;
        // The exact program identifies the physical slot. Never enumerate
        // p1 or p4, and do not use the GPU table handle as the TLAS VA.
        result.stage=RaySceneInput::as_slot;
        result.as.size=sizeof(result.as);
        result.status=(table_only?read_ray_as:read_as)(c,&result.compute,scene_srv_parameter,result.as_offset,&result.as);
        if(result.status)return result;
        result.stage=result.as.location?RaySceneInput::bound:RaySceneInput::typed_null;
        if(result.as.location&&hit.StartAddress&&hit.SizeInBytes)
            result.hit=inputs.ray_input_range(hit.StartAddress,hit.SizeInBytes);
        return result;
    }
    // Matches the active typed AS, not a previous frame's VA. The consumer
    // still needs a separate sealed source for these exact requested bytes.
    std::optional<RayBuildUse> build_use(InputCollector &inputs,Recording epoch,const RaySceneInput &ray){
        if(ray.stage!=RaySceneInput::bound)return {};
        unsigned matches=0;
        const auto *build=find_build(epoch,inputs.resource_at(ray.as.location),ray.as.location,matches);
        if(!build||!build->instances.key||!inputs.resource_current(build->instances.key))return {};
        return static_cast<const RayBuildUse &>(*build);
    }
    void report(InputCollector &inputs,api::command_list *list,const RaySceneInput &r,
        const D3D12_GPU_VIRTUAL_ADDRESS_RANGE &raygen,const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE &hit,bool chs_program=false){
        static constexpr const char *stage[]{"wcs_unavailable","raygen_state_unproven","raygen_range","raygen_upload","raygen_identifier",
            "root_layout","as_slot","typed_null","bound"};
        const auto i=r.program?size_t(r.program->name-known_raygens):std::size(known_raygens);
        if(r.stage!=RaySceneInput::bound&&(reported_[i]&(1u<<r.stage)))return;
        const auto epoch=inputs.recording(list);
        BuildCandidate build{};
        BuildCandidate *matched=nullptr;
        ResourceKey allocation{};
        unsigned matching=0;
        if(r.stage==RaySceneInput::bound){
            allocation=inputs.resource_at(r.as.location);
            // Match metadata only, not a submitted version or GPU read. If
            // multiple recordings wrote this allocation, do not pick another
            // one by CPU observation time over this actual consumer's list.
            matched=find_build(epoch,allocation,r.as.location,matching);
            if(matched)build=*matched;
        }
        const bool instances_current=build.instances.key&&inputs.resource_current(build.instances.key);
        // Early shader success without the Build cannot suppress the first
        // match, nor can a metadata-only match suppress the first live view.
        const unsigned bit=1u<<(r.stage+(r.stage==RaySceneInput::bound?
            (!matched?0u:!instances_current?1u:2u):0u));
        if(reported_[i]&bit)return;
        reported_[i]|=bit;
        if(matched&&!matched->submissions&&!matched->report_submit){
            matched->report_submit=true;
            awaiting_submit_.emplace(matched->recording.list,matched);
        }
        // Known shader/layout mapping, not a descriptor or a later GPU read.
        const auto *out=occlusion_layout(r.compute.root_signature)?ray_output_slot(r):nullptr;
        char msg[1430];
        static constexpr const char *proof[]{"none","record_bytes","closed_program","equivalent_as_slot"};
        std::snprintf(msg,sizeof(msg),"RT-RAY-INPUT stage=%s rc=%d legacy_ready=%u record=%llx/%llu/%llu state=%llx root=%llx rg=%llx/%llu rg_alloc=%llx/%llu+%llu rg_heap=%u program=%ls payload=%s p4_offset=%u output_map=%u/%u/%u as=%llx as_alloc=%llx/%llu heap_write=%llu/%llu source_write=%llu/%llu hit=%llx/%llu/%llu hit_alloc=%llx/%llu+%llu hit_heap=%u build_candidate=%llx/%llu/%llu build_order=%llu build_matches=%u build_flags=%u build_submits=%u build_queue=%llx build_fence=%llx/%llu instances=%llx count=%u instance_alloc=%llx/%llu+%llu instance_heap=%u instance_live=%u raygen_proof=%s material_chs_program=%u",
            stage[r.stage],r.status,r.compute.ready,(unsigned long long)epoch.list,(unsigned long long)epoch.creation,(unsigned long long)epoch.generation,
            (unsigned long long)r.compute.pipeline,(unsigned long long)r.compute.root_signature,(unsigned long long)raygen.StartAddress,
            (unsigned long long)raygen.SizeInBytes,(unsigned long long)r.raygen.key.object,(unsigned long long)r.raygen.key.creation,
            (unsigned long long)r.raygen.offset,unsigned(r.raygen.heap),r.program?r.program->name->alias:L"unknown",
            r.program?ray_payload_label(r.program->name->payload):"unknown",
            r.as_offset,
            out?scene_uav_parameter:UINT32_MAX,out?out->offset:UINT32_MAX,out?out->stride:0,
            (unsigned long long)r.as.location,(unsigned long long)allocation.object,(unsigned long long)allocation.creation,
            (unsigned long long)r.as.heap_generation,(unsigned long long)r.as.write,
            (unsigned long long)r.as.source_heap_generation,(unsigned long long)r.as.source_write,
            (unsigned long long)hit.StartAddress,(unsigned long long)hit.SizeInBytes,(unsigned long long)hit.StrideInBytes,
            (unsigned long long)r.hit.key.object,(unsigned long long)r.hit.key.creation,(unsigned long long)r.hit.offset,unsigned(r.hit.heap),
            (unsigned long long)build.recording.list,(unsigned long long)build.recording.creation,(unsigned long long)build.recording.generation,
            (unsigned long long)build.order,matching,build.flags,build.submissions,(unsigned long long)build.submit.queue,
            (unsigned long long)build.submit.fence,(unsigned long long)build.submit.value,
            (unsigned long long)build.instances_va,build.count,(unsigned long long)build.instances.key.object,
            (unsigned long long)build.instances.key.creation,(unsigned long long)build.instances.offset,unsigned(build.instances.heap),unsigned(instances_current),
            proof[unsigned(r.raygen_proof)],unsigned(chs_program));
        reshade::log::message(reshade::log::level::info,msg);
    }
};

// All pinned programs use GeometryMultiplier=3, MissIndex=0. The selected
// instance and geometry still come from the actual TLAS. OcclusionRGS uses
// RayContribution=1; it is not the OcclusionMainRG hit group ABI.
inline std::optional<std::uint64_t> ray_scene_hit_record(const RaySceneInput &scene,
    const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE &hit,const D3D12_RAYTRACING_INSTANCE_DESC &instance,unsigned geometry){
    if(scene.stage!=RaySceneInput::bound||!scene.program||!hit.StartAddress||
       hit.StartAddress%D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT||!hit.StrideInBytes||
       hit.StrideInBytes>D3D12_RAYTRACING_MAX_SHADER_RECORD_STRIDE||
       hit.StrideInBytes%D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT||hit.SizeInBytes%hit.StrideInBytes)return {};
    const auto record=instance.InstanceContributionToHitGroupIndex+3ull*geometry+scene.program->name->ray_contribution;
    return record<hit.SizeInBytes/hit.StrideInBytes?std::optional<std::uint64_t>(record):std::nullopt;
}
}
