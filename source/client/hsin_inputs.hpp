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
#include <array>
#include <mutex>
#include <span>
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
    bool discovery_interested_cached_=true;
    static constexpr std::uint8_t kCopyBeforeActive=1u<<0;
    static constexpr std::uint8_t kCopyRecActive=1u<<1;
    std::atomic<std::uint8_t> native_copy_mask_{kCopyBeforeActive|kCopyRecActive};
    void arm_native_copy_mask(std::uint8_t bits)noexcept{
        native_copy_mask_.fetch_or(bits,std::memory_order_seq_cst);
    }
    void refresh_discovery_interest_locked()noexcept{
        discovery_interested_cached_=native_receipts_&&discovery_.interested();
    }
    void refresh_native_copy_mask_locked()noexcept{
        if(!native_receipts_){native_copy_mask_.store(0,std::memory_order_release);return;}
        std::uint8_t mask=0;
        if(discovery_interested_cached_||discovery_.has_active_before_work())mask|=kCopyBeforeActive;
        if(discovery_.has_unforwarded_upload_copy()||std::any_of(content_.begin(),content_.end(),[](const Content &c){return !c.forwarded;}))mask|=kCopyRecActive;
        native_copy_mask_.store(mask,std::memory_order_release);
    }
    struct DiscoveryInterestsSnapshot {
        std::uint64_t generation=0;
        std::vector<NativeMeshIntent> intents;
    };
    std::shared_ptr<const DiscoveryInterestsSnapshot> discovery_interests_=std::make_shared<const DiscoveryInterestsSnapshot>();
    std::uint64_t discovery_interests_serial_=0; // protected by mutex_
    const std::uint64_t collector_id_=next_collector_id();
    static std::uint64_t next_collector_id()noexcept{
        static std::atomic<std::uint64_t> seq{1};
        return seq.fetch_add(1,std::memory_order_relaxed);
    }
    std::vector<std::shared_ptr<const ModPack>> packs_;
    std::mutex mutex_;
    NativeShapeEvidence shape_evidence_;
    bool shape_evidence_enabled_=false;
    std::unordered_map<std::uint64_t,std::shared_ptr<ShapeEvidenceLayout>> evidence_layouts_;
    std::unordered_map<std::uint64_t,std::shared_ptr<ShapeEvidenceShader>> evidence_shaders_;

    std::uint64_t serial_=0,order_=0;
    skin::State bindings_;
    std::atomic<std::size_t> active_binding_lists_{0};
    void sync_active_binding_lists_locked() noexcept {
        active_binding_lists_.store(bindings_.lists.size(),std::memory_order_release);
    }
    static constexpr std::size_t kFastShaderCap=16;
    std::atomic<std::uint64_t> shaders_seq_{0};
    std::atomic<std::uint32_t> fast_shader_count_{0};
    std::array<std::atomic<std::uint64_t>,kFastShaderCap> fast_shader_handles_{};
    void sync_fast_shaders_locked() noexcept {
        const std::uint64_t s=shaders_seq_.load(std::memory_order_relaxed);
        shaders_seq_.store(s+1u,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        if(shaders_.size()<=kFastShaderCap){
            std::uint32_t n=0;
            for(const auto &[handle,_]:shaders_)fast_shader_handles_[n++].store(handle,std::memory_order_relaxed);
            fast_shader_count_.store(n,std::memory_order_relaxed);
        }else{
            fast_shader_count_.store(UINT32_MAX,std::memory_order_relaxed);
        }
        shaders_seq_.store(s+2u,std::memory_order_release);
    }
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
    struct PositionStampKey {
        ResourceKey resource;
        std::uint64_t offset=0,bytes=0;
        bool operator==(const PositionStampKey &)const=default;
    };
    struct PositionStampKeyHash {
        std::size_t operator()(const PositionStampKey &k)const noexcept{
            std::uint64_t h=k.resource.object*0x9e3779b97f4a7c15ull;
            h^=k.resource.creation+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);
            h^=k.offset+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);
            h^=k.bytes+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);
            return static_cast<std::size_t>(h);
        }
    };
    std::vector<PositionStamp> position_stamps_;
    std::unordered_map<PositionStampKey,std::size_t,PositionStampKeyHash> position_stamp_index_;
    void rebuild_position_stamp_index_locked(){
        position_stamp_index_.clear();
        position_stamp_index_.reserve(position_stamps_.size());
        for(std::size_t i=0;i<position_stamps_.size();++i){
            const auto &s=position_stamps_[i];
            position_stamp_index_.emplace(PositionStampKey{s.resource,s.offset,s.bytes},i);
        }
    }
    void add_position_stamp_locked(const PositionStamp &stamp){
        const std::size_t idx=position_stamps_.size();
        position_stamps_.push_back(stamp);
        position_stamp_index_.insert_or_assign(PositionStampKey{stamp.resource,stamp.offset,stamp.bytes},idx);
    }
    template<class Pred> bool erase_position_stamps_if_locked(Pred &&pred){
        const bool changed=std::erase_if(position_stamps_,std::forward<Pred>(pred))!=0;
        if(changed)rebuild_position_stamp_index_locked();
        return changed;
    }
    const PositionStamp *find_position_stamp_locked(ResourceKey key,std::uint64_t offset,std::uint64_t bytes)const noexcept{
        const auto it=position_stamp_index_.find(PositionStampKey{key,offset,bytes});
        return it==position_stamp_index_.end()?nullptr:&position_stamps_[it->second];
    }
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
    struct AuxSkinWriter {
        ResourceKey output;std::uint64_t output_base=0,output_offset=0,output_bytes=0;
        ResourceKey rest;std::uint64_t rest_offset=0,rest_bytes=0;
        unsigned first=0,count=0;Recording recording{};
        std::uint64_t order=0;bool forwarded=false;unsigned submissions=0;
    };
    struct AuxRestProbeTarget {
        ResourceKey rest;std::uint64_t rest_offset=0,rest_bytes=0,max_end_bytes=0;
        std::vector<std::uint64_t> tried_bytes;
        unsigned cancel_retries=0;
    };
    struct AuxRestHash {
        ResourceKey rest;std::uint64_t rest_offset=0,rest_bytes=0;std::uint32_t content_hash=0;
    };
    bool has_skip_intents_=false;
    std::vector<AuxSkinWriter> aux_skin_writers_;
    std::vector<AuxRestProbeTarget> aux_probe_targets_;
    std::vector<AuxRestHash> aux_rest_hashes_;
    void record_aux_rest_hash_locked(const NativeRange &range,std::uint32_t content_hash);
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
    void enable_native_discovery(){native_receipts_=true;native_copy_mask_.store(kCopyBeforeActive|kCopyRecActive,std::memory_order_release);} // before Runtime publication; legacy constructors retain their contract
    bool discovery_interested()const noexcept;
    bool may_need_native_copy(bool recorded)const noexcept{
        const std::uint8_t bit=recorded?kCopyRecActive:kCopyBeforeActive;
        return (native_copy_mask_.load(std::memory_order_acquire)&bit)!=0;
    }
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
    void set_catalog(const PackCatalog &catalog){
        std::lock_guard lock(mutex_);packs_=catalog.packs;intents_=catalog.intents;
        std::vector<NativeMeshIntent> enabled_intents;
        enabled_intents.reserve(intents_.size());
        for(const auto &i:intents_)if(i.enabled)enabled_intents.push_back(i);
        has_skip_intents_=std::any_of(enabled_intents.begin(),enabled_intents.end(),[](const NativeMeshIntent &i){return i.skip_only;});
        if(!has_skip_intents_){aux_skin_writers_.clear();aux_probe_targets_.clear();aux_rest_hashes_.clear();}
        arm_native_copy_mask(kCopyBeforeActive|kCopyRecActive);
        discovery_.set_intents(enabled_intents);
        auto next_interests=std::make_shared<DiscoveryInterestsSnapshot>();
        next_interests->generation=++discovery_interests_serial_;
        next_interests->intents=std::move(enabled_intents);
        std::atomic_store(&discovery_interests_,std::shared_ptr<const DiscoveryInterestsSnapshot>(std::move(next_interests)));
        refresh_discovery_interest_locked();
        refresh_native_copy_mask_locked();
    }
    // Ledger application consumes the live creation callback's VA snapshot;
    // it never queries the native object while holding collector/Runtime locks.
    void init_resource(api::resource,const api::resource_desc &,std::uint64_t creation,std::uint64_t gpu_address);
    ResourceKey destroy_resource(api::resource,std::uint64_t creation=0);
    ResourceKey key_of(api::resource); // metadata only, including a pending old generation
    void init_list(api::command_list *);
    Recording init_list_deferred_stamp(api::command_list *);
    void reset_list(api::command_list *);
    Recording reset_list_deferred_stamp(api::command_list *);
    void destroy_list(api::command_list *);
    void destroy_list_deferred_stamp(api::command_list *);
    void init_layout(api::pipeline_layout,unsigned,const api::pipeline_layout_param *);
    void destroy_layout(api::pipeline_layout);
    static skin::Kind classify_compute_shader(const void *code,std::size_t bytes);
    void init_pipeline(api::pipeline,api::pipeline_layout,unsigned,const api::pipeline_subobject *);
    void init_pipeline_classified(api::pipeline,api::pipeline_layout,skin::Kind,bool compute,const void *capture_code,std::size_t capture_bytes);
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
    std::uint32_t identify_aux_skip_draw(const WWMI_RtPackDrawQuery_v2 &,std::uint32_t &out_content_hash);
    struct BorrowedPalette {ComPtr<ID3D12Resource> resource;ResourceKey key;std::uint64_t offset=0,bytes=0,cbv_bytes=0,resource_bytes=0;};
    // A native upload's existing GPU read lease can cover this consumer only
    // within the same actual recording. Other recordings use an owned snapshot.
    bool borrow_palette(const SkinSample &,Recording consumer,BorrowedPalette &);
    bool position_live(const SkinSample &);
    struct PositionReceipt {Recording producer;std::uint64_t order=0;unsigned submissions=0;std::uint64_t native_queue=0;};
    PositionReceipt position_receipt(ResourceKey,std::uint64_t offset,std::uint64_t bytes);
    unsigned position_submission_count(const SkinSample &); // exact native write; UINT32_MAX = no receipt
    struct HybridPoseCheck {
        ResourceKey primitive{};
        bool linked_known=false;
        const NativeBlasIdentity *identity=nullptr;
        bool alive=false;
    };
    struct HybridPreflight {
        Recording consumer{};
        ResourceKey primitive{};
    };
    // Early preflight under 1 mutex_ lock (recording + primitive_key).
    HybridPreflight query_hybrid_preflight(api::command_list *list,std::uint64_t primitive_res,std::uint64_t primitive_origin);
    // Post-asset/proof liveness & pose-pruning batch under 1 mutex_ lock.
    bool query_hybrid_liveness(std::span<HybridPoseCheck> pose_checks,const NativeBlasIdentity &current_id,
        unsigned expected_submissions,bool check_current);
    // Batches position_receipt + position_live checks under a single mutex_
    // acquisition for hybrid_previous_pose().
    PositionReceipt query_hybrid_previous(
        ResourceKey read_resource,std::uint64_t read_offset,std::uint64_t read_bytes,
        std::span<const SkinSample *const> candidates,
        std::span<std::uint8_t> live_out);
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
