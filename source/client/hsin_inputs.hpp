#pragma once
#include "hsin_geometry.hpp"
#include "rt_pack.hpp"
#include "native_discovery.hpp"
#include "rt_pack_identity.h"
#include "native_draw_identity.hpp"
#include "rt_draw_identity_receipt.h"
#include <reshade.hpp>
#include "native_shape_evidence.hpp"
#include "rt_native_dispatch_receipt.h"
#include "rt_skin_input_state.hpp"
#include "rt_descriptor_source.h"
#include <mutex>
#include <unordered_map>
#include <functional>
#include <vector>
#include <deque>
#include <set>
#include <atomic>

namespace hsin_rt {
namespace api=reshade::api;
namespace skin=wwmi::observe::skin_input;
// The eventual add-on callbacks forward to this one device-local collector.
// No disk writer, sampling window, hash lookup that queues GPU work, or Mod draw.
class InputCollector {
    struct Resource {ResourceKey key;std::uint64_t bytes=0,base=0;api::memory_heap heap;};
    struct Shader {std::uint64_t layout=0,generation=0;skin::Kind kind=skin::Kind::none;};
    static constexpr std::size_t no_pose_owner=SIZE_MAX;
    struct List {
        Recording epoch;PoseInputs poses;
        bool has_skin_sample=false,skin_submit_started=false;
        std::size_t pose_owner=no_pose_owner;
    };
    struct Content {ResourceKey resource;std::uint64_t offset=0;Recording producer;bool submitted=false;std::shared_ptr<const NativeProfile> profile;bool forwarded=false;};
    api::device *device_;
    WWMI_RtReadDescriptorSourceFn_v1 source_;
    WWMI_RtModAcquirePositionFn_v1 position_;
    WWMI_RtModStampRecordingFn_v1 stamp_;
    NativeDrawIdentityCache *identity_cache_=nullptr;
    std::function<bool(ResourceKey)> resource_live_;
    std::function<bool(ResourceKey,std::uint64_t,std::uint64_t)> resource_unique_;
    // Shared snapshots are immutable. The negative prefilter keeps its own
    // snapshot reference without entering the Runtime/collector lock.
    std::shared_ptr<NativeProfileRegistry> registry_;
    std::vector<NativeMeshIntent> intents_;
    NativeDiscovery discovery_;
    bool native_receipts_=false;
    std::shared_ptr<const std::vector<NativeMeshIntent>> discovery_interests_=std::make_shared<const std::vector<NativeMeshIntent>>();
    std::vector<std::shared_ptr<const ModPack>> packs_;
    std::mutex mutex_;
    NativeShapeEvidence shape_evidence_;
    bool shape_evidence_enabled_=false;
    std::unordered_map<std::uint64_t,std::shared_ptr<ShapeEvidenceLayout>> evidence_layouts_;
    std::unordered_map<std::uint64_t,std::shared_ptr<ShapeEvidenceShader>> evidence_shaders_;

    std::uint64_t serial_=0,order_=0;
    skin::State bindings_;
    std::unordered_map<std::uint64_t,Resource> resources_;
    // One entry per applied nonzero VA. Map nodes remain stable until erase.
    // Sort by (base, object); the prefix maximum makes older covering
    // allocations visible even when the nearest base only partly overlaps.
    struct VaEntry {const Resource *resource;std::uint64_t prefix_end;};
    std::vector<VaEntry> locations_;
    std::vector<VaEntry>::iterator location_of(const Resource &);
    void refresh_location_ends(std::vector<VaEntry>::iterator);
    void add_location(const Resource &);
    void erase_location(const Resource &);
    const Resource *locate(std::uint64_t address,std::uint64_t bytes);
    std::unordered_map<std::uint64_t,Shader> shaders_;
    std::unordered_map<std::uint64_t,List> lists_;
    // List nodes are stable until erase. Only these recordings can have a
    // sample to invalidate; submitted snapshots remain owned by their stamps.
    std::vector<List *> pose_owners_;
    std::atomic<std::uint64_t> history_revision_{0};
    void history_changed()noexcept{history_revision_.fetch_add(1,std::memory_order_relaxed);}
    void detach_pose_owner(List &)noexcept;
    template<class Invalidate> bool invalidate_pose_owners(Invalidate invalidate){
        bool changed=false;
        for(std::size_t i=0;i<pose_owners_.size();){
            auto &list=*pose_owners_[i];
            const auto before=list.poses.mutable_sample_count();
            invalidate(list.poses);
            const auto count=list.poses.mutable_sample_count();
            changed|=count!=before;
            if(count)++i;else detach_pose_owner(list);
        }
        return changed;
    }
    std::vector<Content> content_;
    struct PositionStamp {
        ResourceKey resource;std::uint64_t offset,bytes,order;Recording producer;unsigned submissions=0;
        std::uint64_t native_queue=0; // actual native post-Execute, never an observed completion
        std::shared_ptr<const PoseInputs> submitted_poses;
    };
    std::vector<PositionStamp> position_stamps_;
    struct SubmittedSkinFence {Recording recording;NativeIdentityFence signal;bool invalidated=false;};
    std::deque<SubmittedSkinFence> submitted_skin_fences_;
    struct ProvedComponentIb {
        ResourceKey indices;
        std::uint64_t index_address=0,index_offset=0;
        unsigned index_count=0,index_stride=4;
    };
    struct ProvedMeshIb {
        std::shared_ptr<const NativeProfile> profile;
        ResourceKey content;
        std::uint64_t content_offset=0;
        ResourceKey allocation;
        std::uint64_t native_blas=0;
        std::vector<ProvedComponentIb> components;
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
    };
    std::vector<ProvedMeshIb> proved_mesh_ibs_;
    std::set<std::pair<std::uint32_t,unsigned>> synth_diag_logged_;
    bool author_history_=false;
    struct View {Resource resource;api::resource_view_desc desc;};
    bool view(std::uint64_t table,unsigned binding,View &out);
    bool read(const Resource &,std::uint64_t offset,std::uint64_t bytes,void *dest);
    void written(ResourceKey resource,std::uint64_t offset,std::uint64_t bytes);
    bool current(const Resource &r)const{return !resource_live_||resource_live_(r.key);}
    bool unique_current(const Resource &r,std::uint64_t address,std::uint64_t bytes)const{
        return resource_unique_?resource_unique_(r.key,address,bytes):current(r);
    }
    bool live_key(ResourceKey key)const{auto r=resources_.find(key.object);return r!=resources_.end()&&r->second.key==key&&current(r->second);}
    bool lookup_skin_fence_locked(Recording,NativeIdentityFence &)const;
    const PoseInputs *poses_for_submitted_stamp_locked(const PositionStamp &)const;
    bool position_stamp_matches_locked(const SkinSample &,unsigned expected_submissions)const;
    void remember_proved_ib_locked(const NativeBlasIdentity &);
    bool try_synthesize_from_proved_ib_locked(const WWMI_RtPackDrawQuery_v2 &,const Resource &vertices,const Resource &indices,
        unsigned idx_stride,unsigned &diag_reason,unsigned &diag_found,unsigned &diag_total,std::uint32_t &diag_guid);
    bool previous_sources_live_impl(const std::vector<SkinSample> &,std::shared_ptr<const ModPack>,bool require_submitted,Recording historical_writer,
        std::uint64_t native_queue=0);
    void retain_submitted_poses(List &);
    void destroyed(ResourceKey);
public:
    struct EvidenceRoot {unsigned status=2,kind=0;std::uint64_t address=0,epoch=0;};
    using EvidenceRootReader=std::function<EvidenceRoot(unsigned)>;
    void configure_shape_evidence(const std::filesystem::path &path){std::lock_guard lock(mutex_);shape_evidence_.configure(path);shape_evidence_enabled_=shape_evidence_.enabled();}
    bool shape_evidence_enabled()const noexcept{return shape_evidence_enabled_;}
    void shape_evidence_dispatch(api::command_list *,const WWMI_RtNativeDispatch_v2 &,const EvidenceRootReader &);
    NativeShapeEvidence::Batch take_shape_evidence(){std::lock_guard lock(mutex_);return shape_evidence_.take(GetTickCount64());}
    InputCollector(api::device *device,WWMI_RtReadDescriptorSourceFn_v1 source,
        std::vector<std::shared_ptr<const NativeProfile>> profiles,WWMI_RtModAcquirePositionFn_v1 position,WWMI_RtModStampRecordingFn_v1 stamp,NativeDrawIdentityCache *identity_cache=nullptr,std::function<bool(ResourceKey)> resource_live={},std::function<bool(ResourceKey,std::uint64_t,std::uint64_t)> resource_unique={})
        :InputCollector(std::make_shared<NativeProfileRegistry>(profiles),device,source,position,stamp,identity_cache,std::move(resource_live),std::move(resource_unique)){}
    InputCollector(std::shared_ptr<NativeProfileRegistry> registry,api::device *device,WWMI_RtReadDescriptorSourceFn_v1 source,
        WWMI_RtModAcquirePositionFn_v1 position,WWMI_RtModStampRecordingFn_v1 stamp,NativeDrawIdentityCache *identity_cache=nullptr,std::function<bool(ResourceKey)> resource_live={},std::function<bool(ResourceKey,std::uint64_t,std::uint64_t)> resource_unique={})
        :device_(device),source_(source),position_(position),stamp_(stamp),identity_cache_(identity_cache),resource_live_(std::move(resource_live)),resource_unique_(std::move(resource_unique)),registry_(std::move(registry)),discovery_(registry_){}
    ~InputCollector(){if(stamp_)for(const auto &[_,list]:lists_)stamp_(list.epoch.list,0,0);}
    void enable_native_discovery(){native_receipts_=true;} // before Runtime publication; legacy constructors retain their contract
    bool discovery_interested()const noexcept;
    void native_copy(api::command_list *,api::resource,std::uint64_t,api::resource,std::uint64_t,std::uint64_t,bool recorded);
    void native_written(api::resource,std::uint64_t,std::uint64_t);
    void native_unknown();
    void native_skin_forwarded(api::command_list *,bool);
    void native_blas(api::command_list *,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &,bool recorded);
    std::vector<std::shared_ptr<NativeDiscovery::Bytes>> native_requests(api::command_list *,NativeDiscovery::ReadPoint);
    bool native_request_input(const NativeDiscovery::Bytes &,std::uint64_t &address,bool &cpu);
    bool native_request_cpu(const NativeDiscovery::Bytes &);
    void native_request_cancel(std::uint64_t);
    void native_submission_started(Recording);
    void native_signalled(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t);
    bool native_completed(std::uint64_t,std::uint64_t,std::uint64_t,std::vector<unsigned char>);
    void publish_discovery();
    NativeDiscovery::Diagnostics native_diagnostics(){std::lock_guard lock(mutex_);return discovery_.diagnostics();}
    void enable_author_history(){author_history_=true;} // before this device's Runtime is published
    std::uint64_t history_revision()const noexcept{return history_revision_.load(std::memory_order_relaxed);}
    void set_packs(const std::vector<std::shared_ptr<const ModPack>> &packs){std::lock_guard lock(mutex_);packs_=packs;}
    void set_catalog(const PackCatalog &catalog){std::lock_guard lock(mutex_);packs_=catalog.packs;intents_=catalog.intents;discovery_.set_intents(intents_);std::atomic_store(&discovery_interests_,std::make_shared<const std::vector<NativeMeshIntent>>(intents_));}
    // Ledger application consumes the live creation callback's VA snapshot;
    // it never queries the native object while holding collector/Runtime locks.
    void init_resource(api::resource,const api::resource_desc &,std::uint64_t creation,std::uint64_t gpu_address);
    ResourceKey destroy_resource(api::resource,std::uint64_t creation=0);
    ResourceKey key_of(api::resource); // metadata only, including a pending old generation
    void init_list(api::command_list *);
    void reset_list(api::command_list *);
    void destroy_list(api::command_list *);
    void init_layout(api::pipeline_layout,unsigned,const api::pipeline_layout_param *);
    void destroy_layout(api::pipeline_layout);
    void init_pipeline(api::pipeline,api::pipeline_layout,unsigned,const api::pipeline_subobject *);
    void destroy_pipeline(api::pipeline);
    // Negative-only preflight. Positive callbacks still drain resets and
    // recheck the binding under the ordinary Runtime/collector locks.
    bool tracks_pipeline_bind(api::command_list *,api::pipeline);
    bool tracks_table_bind(api::command_list *,api::pipeline_layout,unsigned first,unsigned count);
    bool tracks_cb_bind(api::command_list *,api::pipeline_layout,unsigned param);
    void bind_pipeline(api::command_list *,api::pipeline_stage,api::pipeline);
    void bind_tables(api::command_list *,api::shader_stage,api::pipeline_layout,unsigned,unsigned,const api::descriptor_table *);
    void push_descriptors(api::command_list *,api::shader_stage,api::pipeline_layout,unsigned,const api::descriptor_table_update &);
    void copy(api::command_list *,api::resource,std::uint64_t,api::resource,std::uint64_t,std::uint64_t);
    Recording recording(api::command_list *);
    Recording submission_recording(api::command_list *); // existing pre-Execute event
    bool recording_has_skin_samples(Recording);
    void record_skin_fence(Recording,NativeIdentityFence);
    void submitted(Recording,std::uint64_t native_queue); // exact pre token after actual native Execute
    bool has_skin_dispatch(api::command_list *); // metadata only; never grants input/pose identity
    bool may_select_blas(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &)const noexcept;
    struct DispatchWrite {ResourceKey resource;unsigned component=UINT32_MAX;std::uint64_t order=0;};
    DispatchWrite dispatch(api::command_list *,bool authorize=true); // recognized native output, or empty
    ResourceKey resource_at(std::uint64_t address); // backing allocation only, not character identity
    struct RayInputRange {
        ResourceKey key;
        std::uint64_t offset=0,bytes=0;
        api::memory_heap heap=api::memory_heap::unknown;
        bool upload_read=false;
    };
    // An exact range from a real DXR callback. Only the supplied prefix is
    // copied, and only from a current, unique CPU UPLOAD allocation.
    RayInputRange ray_input_range(std::uint64_t address,std::uint64_t bytes,std::span<std::byte> prefix={});
    bool resource_current(ResourceKey key){std::lock_guard lock(mutex_);return live_key(key);}
    bool identify_draw(api::command_list *,std::uint64_t vertex_base,unsigned first,unsigned count,WWMI_RtPackDrawIdentity_v1 &,void *palette,unsigned capacity);
    bool identify_draw_v2(const WWMI_RtPackDrawQuery_v2 &,WWMI_RtPackDrawIdentity_v2 &,void *palette,unsigned capacity,
                          NativeDrawIdentityCache::Match *match_out=nullptr,WWMI_RtDrawIdentityReceipt_v1 *receipt=nullptr,
                          bool author_snapshot=false);
    struct BorrowedPalette {ComPtr<ID3D12Resource> resource;ResourceKey key;std::uint64_t offset=0,bytes=0,cbv_bytes=0,resource_bytes=0;};
    // A native upload's existing GPU read lease can cover this consumer only
    // within the same actual recording. Other recordings use an owned snapshot.
    bool borrow_palette(const SkinSample &,Recording consumer,BorrowedPalette &);
    bool position_live(const SkinSample &);
    struct PositionReceipt {Recording producer;std::uint64_t order=0;unsigned submissions=0;std::uint64_t native_queue=0;};
    PositionReceipt position_receipt(ResourceKey,std::uint64_t offset,std::uint64_t bytes);
    unsigned position_submission_count(const SkinSample &); // exact native write; UINT32_MAX = no receipt
    const char *native_position_srv_error(std::uint64_t resource,std::uint64_t offset,std::uint64_t bytes,unsigned format,unsigned stride,ResourceKey &);
    bool previous_cb_keys(std::uint64_t view,std::uint64_t view_origin,std::uint64_t primitive,std::uint64_t primitive_origin,
        ResourceKey &view_key,ResourceKey &primitive_key);
    ResourceKey primitive_key(std::uint64_t primitive,std::uint64_t origin);
    Assembly assemble_snapshot(const NativeBlasIdentity &,std::shared_ptr<const ModPack>);
    // An explicit candidate layout selects only recorded windows. It does not
    // prove an actual BLAS link or authorize any previous-position consumer.
    ComponentAssembly assemble_previous_component(api::command_list *,std::shared_ptr<const ModPack>,ResourceKey output,
        std::uint64_t view_offset,unsigned component,ResourceKey content,std::uint64_t content_offset,bool diagnostic=false,
        const BlasPoseInput *candidate_layout=nullptr,const std::vector<const SkinSample *> *retained_providers=nullptr);
    // The actual previous SRV picked this source. Only exact same-ring
    // samples from its single native writer are assembled in this route.
    ComponentAssembly assemble_submitted_previous(std::shared_ptr<const ModPack>,const SkinSample &current,ResourceKey output,
        std::uint64_t view_offset,PositionReceipt);
    struct PreviousBlasProof {
        // Only a match to this exact target SRV is relevant. The descriptor is
        // already recorded by the native BLAS call before this probe runs.
        const char *stage="unrelated";bool relevant=false;
        Recording recording{},target_recording{};std::uint64_t target_order=0,provider_order=0;
        ResourceKey provider_output{};ComponentAssembly candidate;
    };
    PreviousBlasProof assemble_previous_blas(api::command_list *,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &,
        std::shared_ptr<const ModPack>,ResourceKey output,std::uint64_t view_offset,unsigned component,ResourceKey content,std::uint64_t content_offset);
    bool previous_sources_live(const std::vector<SkinSample> &sources,std::shared_ptr<const ModPack> pack,bool require_submitted){
        return previous_sources_live_impl(sources,pack,require_submitted,{});
    }
    bool previous_sources_on_queue(const std::vector<SkinSample> &sources,std::shared_ptr<const ModPack> pack,std::uint64_t native_queue){
        return native_queue&&previous_sources_live_impl(sources,pack,true,{},native_queue);
    }
    // Only an authorization-gated cohort may call this with a verified real
    // historical fence. An older source must have executed exactly once even
    // while the new private recording is still open.
    bool previous_cohort_sources_live(const std::vector<SkinSample> &sources,std::shared_ptr<const ModPack> pack,
        bool require_submitted,Recording historical_writer){
        return historical_writer.list&&previous_sources_live_impl(sources,pack,require_submitted,historical_writer);
    }
    struct SelectedIdentityState {unsigned submissions=0;NativeIdentityFence producer{};};
    Assembly select(api::command_list *,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &,std::shared_ptr<const NativeProfile> *identified=nullptr,std::shared_ptr<const NativeBlasIdentity> *snapshot=nullptr,SelectedIdentityState *snapshot_state=nullptr);
};
}
