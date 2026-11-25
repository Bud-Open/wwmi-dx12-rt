#include "hsin_inputs.hpp"
#include "rt_graphics_geometry.h"
#include "rt_section_identity.h"
#include "rt_geometry_plan.h"
#include "rt_native_palette.h"
#include "rt_draw_frame.h"
#include "rt_author_draw.h"
#include "rt_author_choice_note.hpp"
#include "rt_hybrid_assets.hpp"
#include "rt_author_deformed.h"
#include "rt_author_section_frame.h"
#include "hsin_patches.hpp"
#include "hsin_material.hpp"
#include "occlusion_tables.hpp"
#include "occlusion_instance_probe.hpp"
#include "raygen_record_probe.hpp"
#include "shapekey_position_input.hpp"
#include "gpu_helpers.hpp"
#include "author_static_assets.hpp"
#include "author_stream_pool.hpp"
#include "rt_author_work_receipt.hpp"
#include "rt_pack.hpp"
#include "native_buffer_probe.hpp"
#include "native_profile_archive.hpp"
#include "rt_pack_identity.h"
#include "runtime_boundaries.hpp"
#include "rt_cpu_profile.hpp"
#include "rt_cpu_window.h"
#include "rt_draw_window.h"
#include "native_work_interest.hpp"
#include "material_chs_intake.hpp"
#include "rt_pack_frame.h"
#include <set>
#include "rt_native_dispatch_receipt.h"
#include <filesystem>
#include <fstream>
#include <deque>
#include <map>
#include <condition_variable>
#include <bit>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <utility>

using namespace hsin_rt;
namespace {
HMODULE self_module=nullptr;
HMODULE unfinished_gpu_module=nullptr;
wwmi::rt_material_chs::Writer material_chs_writer;
// GetModuleHandleW is only a lookup: the provider may be unmapped before our
// AddonUninit. Own an ordinary loader reference, never a permanent PIN.
struct ProviderModule {
    HMODULE module=nullptr;
    ~ProviderModule(){if(module)FreeLibrary(module);}
};
std::shared_ptr<ProviderModule> provider_module;
using Holds=std::vector<std::shared_ptr<void>>;
using ComputeQueryFn=decltype(&WWMI_ComputeState_Query_v1);
using EvidenceRootArgumentFn=decltype(&WWMI_ComputeState_ReadRootArgument_v1);
EvidenceRootArgumentFn evidence_root_argument=nullptr;
using ComputeEnableFn=decltype(&WWMI_ComputeState_Enable_v1);
using ForwardRegisterFn=decltype(&WWMI_RtForward_Register_v1);
using ForwardRegisterV2Fn=decltype(&WWMI_RtForward_Register_v2);
using ForwardRegisterV3Fn=decltype(&WWMI_RtForward_Register_v3);
ForwardRegisterV3Fn register_forward_v3=nullptr;
NativeBufferLeaseFn native_buffer_lease=nullptr;
bool native_discovery_available=false;
using ForwardUnregisterFn=decltype(&WWMI_RtForward_Unregister_v1);
using AuthorConsumerFn=std::uint32_t (*)(std::uint64_t);
ComputeRunFn compute_run=nullptr;ComputeQueryFn compute_query=nullptr;
ReadAsSlotFn read_as_slot=nullptr;
WWMI_RtCurrentNativeStateObjectFn_v1 read_native_state_object=nullptr;
ReadRayAsSlotFn read_ray_as_slot=nullptr;
CaptureOcclusionRestoreFn capture_occlusion_compute=nullptr;
CaptureOcclusionGraphicsFn capture_occlusion_graphics=nullptr;
ReadOcclusionStructuredFn read_occlusion_structured=nullptr;
bool ray_input_window(){
    return read_as_slot&&rt_cpu::phase.load(std::memory_order_acquire)==1&&
        rt_cpu::now()<rt_cpu::deadline.load(std::memory_order_acquire);
}
// Scheduling hint only. Unrelated v2 dispatches do not enter Runtime or
// read descriptor/compute state during this one producer probe.
std::atomic<std::uint64_t> instance_dispatch_watch{};
ComputeEnableFn compute_enable=nullptr;ForwardRegisterFn register_forward=nullptr;ForwardUnregisterFn unregister_forward=nullptr;
ForwardRegisterV2Fn register_forward_v2=nullptr;
AuthorConsumerFn author_consumer=nullptr;
WWMI_RtSetDescriptorObservationFn_v1 descriptor_lease=nullptr;WWMI_RtReadDescriptorSourceFn_v1 descriptor_source=nullptr;
std::mutex devices_mutex;
std::condition_variable devices_idle;
bool accepting_devices=false;
std::set<api::device *> initializing_devices;
WWMI_RtModReadGeometrySectionFn_v1 mod_geometry_section=nullptr;
decltype(&WWMI_RtModReadAuthorSection_v2) mod_author_section=nullptr;
decltype(&WWMI_RtModReadAuthorRevision_v1) mod_author_revision=nullptr;
WWMI_RtModAcquireDeformedSourceFn_v1 mod_acquire_deformed=nullptr;
WWMI_RtModAcquirePostMultiplierShapeFn_v1 shape_position=nullptr;
WWMI_RtModReadSectionIdentityFn_v1 mod_section_identity=nullptr;
WWMI_RtCpuWindowDeadlineFn_v1 cpu_window=nullptr;
WWMI_RtDrawWindowDeadlineFn_v1 draw_window=nullptr;
std::atomic<std::uint64_t> timed_device{0},timed_until{0},timed_close_lists{0};
std::array<std::atomic<std::uint64_t>,2> native_timing_due{};
std::uint64_t timed_list_bit(std::uint64_t list)noexcept{return 1ull<<((list>>6)&63);}
WWMI_RtModReadCatalogFn_v1 mod_catalog=nullptr;WWMI_RtModReadResourceFn_v1 mod_resource=nullptr;
WWMI_RtModAcquirePositionFn_v1 mod_position=nullptr;WWMI_RtModStampRecordingFn_v1 mod_stamp=nullptr;
WWMI_RtModMarkDetectedFn_v1 mod_detected=nullptr;WWMI_RtSetPackFrameDeviceFn_v1 set_frame_device=nullptr;
// Optional diagnostic join key from the main plugin. It is nonzero only in
// the synchronous previous-surface call of an armed, identified cloth draw.
using ClothPreviousTraceSeqFn=uint64_t (*)();
ClothPreviousTraceSeqFn cloth_previous_trace_seq=nullptr;
std::filesystem::path data_directory(){wchar_t module[32768]{};if(!GetModuleFileNameW(self_module,module,32768))throw std::runtime_error("RT module path");return std::filesystem::path(module).parent_path()/L"rt-pack-data";}

std::filesystem::path native_profile_cache_path(){
    wchar_t value[32768]{};const auto n=GetEnvironmentVariableW(L"WWMI_RT_NATIVE_PROFILE_CACHE",value,DWORD(std::size(value)));
    if(n&&n<std::size(value))return std::filesystem::path(value);
    wchar_t mod_buf[32768]{};
    if(!GetModuleFileNameW(self_module,mod_buf,DWORD(std::size(mod_buf))))return std::filesystem::path{};
    const std::filesystem::path addon_dir=std::filesystem::path(mod_buf).parent_path();
    const std::filesystem::path base_root=(_wcsicmp(addon_dir.filename().c_str(),L"reshade-addons")==0&&addon_dir.has_parent_path())?addon_dir.parent_path():addon_dir;
    std::error_code ec;
    const std::filesystem::path rt_dir=base_root/L"runtime";
    std::filesystem::create_directories(rt_dir,ec);
    return rt_dir/L"native-profiles-v2.txt";
}

std::shared_ptr<NativeProfileRegistry> load_native_registry(){
    // Compatibility for user-supplied V1 profiles, not a release input.
    const auto path=data_directory()/L"native-profiles.txt";
    std::error_code ec;
    if(std::filesystem::is_regular_file(path,ec)){
        try{return std::make_shared<NativeProfileRegistry>(read_native_profiles(path));}
        catch(const std::exception &e){reshade::log::message(reshade::log::level::warning,e.what());}
    }
    return std::make_shared<NativeProfileRegistry>();
}

std::vector<unsigned char> read_asset(const wchar_t *name){
    const auto path=data_directory()/name;
    std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("RT asset missing: "+path.string());
    const auto size=f.tellg();if(size<=0)throw std::runtime_error("RT empty asset");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));f.seekg(0);
    if(!f.read(reinterpret_cast<char *>(bytes.data()),size))throw std::runtime_error("RT asset read");return bytes;
}
struct TlasWrite {std::uint64_t address=0;ResourceKey allocation;std::shared_ptr<InstanceBatch> instance;};
struct SceneOwner {TlasWrite write;std::uint64_t queue=0;};
struct RetiringSceneOwner {SceneOwner owner;NativeIdentityFence overwritten;};
struct AuthorCommands;
struct GpuWorkSample;
struct DrawBindingRecording;
struct AuthorPreviousWriter {Recording recording;NativeIdentityFence signal;};
struct Submission {Recording epoch;Holds holds;NativeDrawIdentityCache::Frozen identities;std::vector<std::shared_ptr<GeometryBatch>> generated;
    std::vector<TlasWrite> tlas_writes;std::vector<GeometryBatch *> author_generated;std::shared_ptr<AuthorPreviousWriter> author_previous_writer;
    std::shared_ptr<OcclusionInstanceProbe> instance_probe;
    std::shared_ptr<RaygenRecordProbe> raygen_probe;
    std::vector<std::shared_ptr<NativeBufferProbe>> native_probes;
    std::weak_ptr<DrawBindingRecording> draw;
    bool has_skin_samples=false;};
struct Flight {std::uint64_t value;Holds holds;std::weak_ptr<DrawBindingRecording> draw;};
struct Queue {
    ComPtr<ID3D12CommandQueue> native;
    ComPtr<ID3D12Fence> fence,author_ready;
    std::uint64_t value=0,completed=0,author_value=0,author_completed=0;
    std::uint64_t timestamp_frequency=0;
    bool timestamp_frequency_checked=false;
    std::deque<Flight> flights;
    AuthorCommands *producer_head=nullptr,*producer_tail=nullptr;
};
struct NativePaletteBatch {
    ComPtr<ID3D12Resource> resource;
    std::uint64_t creation=0,offset=0,bytes=0,cbv_bytes=0,resource_bytes=0;
    unsigned kind=0;
};
struct NativePaletteCache {
    std::shared_ptr<const NativeBlasIdentity> identity;
    unsigned component=0;
    std::weak_ptr<NativePaletteBatch> native;
    std::shared_ptr<NativePaletteBatch> owned;
};
// A page is append-only for one recording. The pool can reclaim it only
// after both that recording and all its actual flights have dropped it.
struct DrawBindingPage {
    static constexpr unsigned constants_slots=128, constant_stride=256;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> constants;
    unsigned capacity=0,increment=0,descriptors_used=0,constants_used=0;
    unsigned char *mapped=nullptr;
    bool fits(unsigned n,unsigned slots) const {return n<=capacity-descriptors_used&&slots<=constants_slots-constants_used;}
};
struct PreviousDrawScope {
    ResourceKey view,primitive;
    WWMI_RtPreviousDrawScope_v1 native{};
    bool same_view_primitive(const PreviousDrawScope &other)const{
        // The enclosing recording supplies actual draw order. OMSet can
        // rebind the same attachments; its counter is not a pose boundary.
        return view==other.view&&primitive==other.primitive&&native.view_byte_origin==other.native.view_byte_origin&&
            native.primitive_byte_origin==other.native.primitive_byte_origin&&
            !std::memcmp(native.view_words,other.native.view_words,19*sizeof(std::uint32_t))&&
            !std::memcmp(native.primitive_words,other.native.primitive_words,12*sizeof(std::uint32_t));
    }
};
struct PreviousReadUse {
    ResourceKey resource;std::uint64_t offset=0,bytes=0;
    InputCollector::PositionReceipt write;
    bool operator==(const PreviousReadUse &other)const{
        return resource==other.resource&&offset==other.offset&&bytes==other.bytes&&write.order==other.write.order&&
            write.producer==other.write.producer;
    }
    bool matches(const SkinSample &proof)const{
        return resource==proof.output&&offset==proof.output_offset&&bytes==proof.output_bytes&&
            write.order&&write.order==proof.order&&write.producer==proof.recording;
    }
};
struct PreviousProviderRead {
    std::weak_ptr<const NativeBlasIdentity> owner;std::weak_ptr<const ModPack> pack;
    PreviousDrawScope scope;unsigned component=0;
    PreviousReadUse use;bool conflict=false;
};
struct PendingPreviousProvider {
    struct Choice {unsigned component;PreviousReadUse use;};
    std::weak_ptr<const NativeBlasIdentity> owner;std::weak_ptr<const ModPack> pack;
    PreviousDrawScope scope;std::vector<Choice> expected;
    std::uint64_t failed_seq=0,target_order=0;
};
struct AuthorCommands {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList4> list;
    // Intrusive ready order avoids allocating an owner after native Execute.
    // author_commands owns these nodes while they are queued.
    std::shared_ptr<GeometryBatch> producing;
    std::shared_ptr<GpuWorkSample> timer;
    std::uint64_t ready_value=0;
    AuthorCommands *next=nullptr;
};
struct AuthorStaticAssets {
    std::shared_ptr<GeometryAssets> bootstrap,geometry;
    std::shared_ptr<AuthorCommands> upload;
    ComPtr<ID3D12Fence> ready;
    ComPtr<ID3D12CommandQueue> producer;
    NativeIdentityFence recovery;
    bool signalled=false;
};
// One immutable output for an exact CPU pose and full or component coverage.
// Production readiness never implies retirement of a consumer recording.
struct AuthorOutput {
    std::shared_ptr<const Pose> pose;
    std::shared_ptr<GeometryBatch> batch;
    std::vector<std::pair<std::uint64_t,Recording>> other_writers;
    NativeIdentityFence production;
    bool submitted=false;
};
struct AuthorOutputs {
    explicit AuthorOutputs(size_t n):parts(n){}
    std::shared_ptr<const Pose> pose;
    std::shared_ptr<AuthorOutput> full;
    std::vector<std::shared_ptr<AuthorOutput>> parts;
    std::shared_ptr<AuthorOutput>& slot(unsigned component){return component==UINT32_MAX?full:parts[component];}
};
struct AuthorPrefix {
    std::shared_ptr<const NativeBlasIdentity> identity;
    std::shared_ptr<AuthorOutput> output;
    std::weak_ptr<AuthorOutputs> history;
    std::shared_ptr<AuthorCommands> commands;
    std::shared_ptr<AuthorStaticAssets> static_assets;
    bool committed=false,executed=false;
};
struct AuthorComponentSource {
    // A source proved by this actual complete recipe, not the latest section
    // associated with the Mod revision. Empty is a separate, explicit proof.
    std::uint64_t section=0;
    WWMI_RtDeformedSelection_v1 selection{};
    bool empty=false;
};
enum class AuthorFreezeCause : unsigned {
    no_recipe_revision, catalog_pack_absent, pack_not_author, no_surface, component_set,
    native_cohort, pose_assembly, rest_candidate, external_final
};
struct AuthorPlan {
    std::uint64_t id=0,snapshot=0,load=0,mod=0;
    ResourceKey primitive;
    WWMI_RtAuthorInstance_v1 scope{};
    std::shared_ptr<const NativeBlasIdentity> identity;
    std::shared_ptr<const ModPack> pack;
    std::shared_ptr<const Pose> pose;
    std::vector<AuthorComponentSource> sources;
    bool requires_source=false;
    bool choice_checked=false;
    bool completed_source_closed=false;
    bool refused_native=false;
    unsigned disposition=WWMI_RT_AUTHOR_TENTATIVE;
    AuthorFreezeCause freeze_cause=AuthorFreezeCause::no_recipe_revision;
    unsigned initial_disposition=WWMI_RT_AUTHOR_TENTATIVE;
    uint64_t freeze_revision=0,freeze_ticket=0,freeze_recording=0;
    unsigned freeze_component=0;
};
struct AuthorSectionLease {
    std::shared_ptr<GeometryBatch> current,previous;
    std::shared_ptr<const NativeBlasIdentity> previous_cohort;
};
// The actual completed section's acquisitions. Cache retirement or another
// recording must not re-select the current or previous batch at commit.
struct AuthorPreparedDraw {
    std::uint64_t plan=0,section=0,current=0,previous=0;
    unsigned component=UINT32_MAX,count=0,issued=0;
    AuthorSectionLease lease;
    bool complete()const{return plan&&section&&issued==(count?count:1u);}
};
struct AuthorCapture {
    WWMI_RtPackDrawQuery_v2 query{};
    WWMI_RtAuthorSnapshot_v1 snapshot{};
    NativeDrawIdentityCache::Match match;
    std::shared_ptr<AuthorPlan> plan; // keeps the currently evaluated plan through the VM
    std::optional<WWMI_RtDeformedSelection_v1> source; // verified selection, or the exact owner of an empty recipe
    AuthorPreparedDraw prepared;
    struct RasterPrevious {
        ResourceKey resource;std::uint64_t offset=0,bytes=0,view_offset=0;
        InputCollector::PositionReceipt receipt{};
    } raster_previous{};
};
struct HybridAsset {
    wwmi::hybrid::AssetProof proof;
    std::shared_ptr<const ModPack> pack;
    ComPtr<ID3D12Resource> index;
};
struct HybridPose {
    std::shared_ptr<HybridAsset> asset;
    std::shared_ptr<const NativeBlasIdentity> identity;
    ResourceKey primitive;
    std::uint64_t origin;
    std::shared_ptr<const Pose> pose;
    std::shared_ptr<AuthorOutputs> outputs;
    std::vector<wwmi::hybrid::Result> adopted;
    NativeIdentityFence linked_history; // actual native submit, not a Mod draw or a frame number
};
struct HybridPending {
    std::uint64_t token=0,ticket=0;
    unsigned component=0;
    wwmi::hybrid::Result mode=wwmi::hybrid::unavailable;
    std::shared_ptr<const Pose> pose;
    std::shared_ptr<GeometryBatch> current,previous;
};
struct DrawBindingRecording {
    enum Reset { active, waiting, failed, succeeded };
    Recording epoch;
    std::uint64_t native_list=0;
    std::atomic<Reset> reset{active};
    std::vector<std::shared_ptr<DrawBindingPage>> pages;
    std::vector<ComPtr<ID3D12Resource>> textures;
    std::vector<PreviousProviderRead> previous_reads;
    PendingPreviousProvider pending_previous;
    std::vector<AuthorPrefix> author_prefixes;
    HybridPending hybrid_pending;
    std::vector<GeometryBatch *> author_pending_batches;
    std::vector<ID3D12CommandList *> author_pending_lists;
    std::vector<GeometryBatch *> author_batches; // the same recording's holds own these batches
    std::vector<std::shared_ptr<GpuWorkSample>> timers;
    std::uint64_t committed_sections=0,observed_sections=0;
    bool consumer_submitted=false;
    AuthorCapture author_capture;
};
// The proxy notifies before Reset and reports its HRESULT synchronously on
// this thread. The post callback only publishes the result, without taking
// Runtime locks while the provider's detach may be draining callbacks.
thread_local std::weak_ptr<DrawBindingRecording> draw_reset_receipt;
struct SurfaceHistory {
    std::shared_ptr<const NativeBlasIdentity> identity;std::shared_ptr<GeometryBatch> batch;
    std::array<std::uint64_t,2> frames{};
    std::shared_ptr<const ModPack> pack;
    std::shared_ptr<const Pose> owned_pose;
    NativeIdentityFence native_signal;
    std::shared_ptr<AuthorOutputs> author_outputs;
    std::uint64_t completed_plan=0; // zero denotes native/REST history
};
struct PreviousInterest {
    std::shared_ptr<const ModPack> pack;ResourceKey output,content;
    std::uint64_t view_offset=0,view_bytes=0,content_offset=0;unsigned component=0;
    bool scoped_demand=false,author_demand=false,legacy_demand=false;
    // Bounded per-observed-SRV diagnostic; emitted only on an armed failure.
    const char *last_stage="not_started";PoseResult last_pose=PoseResult::missing_producer;
    unsigned missing_component=UINT32_MAX,last_write_component=UINT32_MAX;
    std::uint64_t last_write_order=0;Recording last_recording{};
    unsigned local_same=UINT32_MAX,local_other=UINT32_MAX,other_recordings=UINT32_MAX;
    const char *link_stage="not_observed";std::uint64_t link_order=0,provider_order=0;
    Recording link_recording{},link_build_recording{};ResourceKey provider_output{};
    std::uint64_t failed_seq=0,failed_order=0;Recording failed_recording{};
};
enum class PreviousRoute {dispatch,linked_blas,alternate_write,cohort_candidate,submitted_ring};
struct PreviousHistoricalWriter {
    Recording recording;
    NativeIdentityFence signal;
};
struct PreviousPrepared {
    PreviousInterest source;std::vector<SkinSample> proofs;std::shared_ptr<GeometryBatch> batch;PreviousRoute route;
    std::weak_ptr<const NativeBlasIdentity> cohort;
    PreviousHistoricalWriter historical;
    // The author derives its output on the actual graphics consumer. This
    // receipt belongs to the native SkinCache writer, not that GPU batch.
    std::shared_ptr<const Pose> owned_pose;
    Recording native_writer;
    std::shared_ptr<AuthorPreviousWriter> native_receipt;
    std::shared_ptr<AuthorOutputs> author_outputs;
    std::uint64_t native_submit_queue=0; // submitted_ring only: actual post-Execute, no GPU input borrowed
    // Written once from owned_pose; each independently produced stream holds
    // a reference. This is not an output, writer receipt or queue assumption.
    ComPtr<ID3D12Resource> author_palette;
};
enum class CohortBlock { none, no_exact_candidate, batch_unready, native_unready, draw_scope_missing, other_current_identity,
    provider_unseen_at_draw, provider_other_scope, provider_other_write, provider_conflict };
constexpr const char *cohort_block_names[]{"none","no_exact_candidate","batch_unready","native_unready","draw_scope_missing","other_current_identity",
    "provider_unseen_at_draw","provider_other_scope","provider_other_write","provider_conflict"};
struct CohortCheck {CohortBlock block;const SkinSample *provider=nullptr;};
constexpr const char *previous_result_names[]{"selection","component","source_layout","source_resource","vertex_origin","view_extent","native_written","native_submission","private_incomplete","batch_provenance","ambiguous","ok","previous_srv_unproved","private_history_missing","ok_label_gap","previous_binding_missing"};
// Actual reasons returned by InputCollector::native_position_srv_error.
constexpr const char *previous_srv_rejects[]{"view_type","view_alignment_or_empty","resource_not_tracked","resource_no_longer_current","view_outside_resource"};
struct PreviousRejectCount {
    const char *reason=nullptr,*srv_reject=nullptr;
    std::uint64_t count=0,armed=0,after_window=0;
    std::uint64_t first_frame=0,last_frame=0;
};
#include "author_work_window.hpp"
struct Runtime {
    // Declared first, destroyed last: InputCollector's destructor still stamps
    // its recordings in main after ~Runtime has released the frame/Reader.
    std::shared_ptr<ProviderModule> provider;
    api::device *api_device;
    const std::uint64_t native_device_id;
    ComPtr<ID3D12Device5> native;
    BufferInbox buffer_events;
    FlowTrace trace;
    std::shared_ptr<NativeProfileRegistry> native_profiles;
    NativeProfileArchive native_archive;
    PackSource source;PackCatalog catalog;
    NativeDrawIdentityCache identities;
    InputCollector inputs;
    NativeBlasInterest blas_interest;
    MaterialIdentity materials;
    RaySceneConsumer ray_scene;
    wwmi::rt_material_chs::Intake material_chs;
    std::uint64_t occlusion_binding_reported=0;
    OcclusionProbeWindow instance_probe_watch;
    OcclusionProbeCopy instance_probe_copy;
    OcclusionProbeDispatch instance_dispatch_pending,instance_dispatch_recorded;
    Recording instance_dispatch_hint;
    std::uint64_t instance_probe_source_address=0;
    std::shared_ptr<OcclusionProbePrograms> instance_probe_program;
    std::shared_ptr<OcclusionInstanceProbe> instance_probe;
    unsigned instance_probe_reasons=0;
    std::shared_ptr<RaygenRecordProbe> raygen_probe;
    std::atomic<bool> raygen_probe_taken=false;
    unsigned raygen_probe_reasons=0;
    std::vector<unsigned char> native_probe_code;
    std::shared_ptr<NativeBufferProbeProgram> native_probe_program;
    std::unordered_map<std::uint64_t,std::vector<std::shared_ptr<NativeBufferProbe>>> native_probe_records;
    std::vector<std::shared_ptr<NativeBufferProbe>> native_probe_pending;
    bool native_probe_error_reported=false,discovery_enabled=false;
    enum class ProbeReject:unsigned {input,hold,logical_lease,cpu_read,prepare,run,exception,replay,signal,invalidated,readback,completion,count};
    static constexpr const char *probe_reject_name(ProbeReject value){
        constexpr const char *names[]{"input_resource","source_hold","logical_read_lease","cpu_read","prepare","compute_run","exception","replay","signal","invalidated","readback","completion_rejected"};return names[unsigned(value)];
    }
    struct ProbeRejectRow {
        std::uint64_t count=0,reported=0,last_log=0,id=0,write_version=0,source_bytes=0;
        NativeRange range;Recording recording;NativeDiscovery::ReadPoint point=NativeDiscovery::ReadPoint::upload_copy;
        const char *stage="none";std::int32_t status=0;std::uint32_t flags=0,mode=0;
        std::uint64_t sync=0,access=0;std::uint32_t lease_reason=0;
    };
    std::array<ProbeRejectRow,unsigned(ProbeReject::count)> native_probe_rejects{};
    std::uint64_t native_probe_requested=0,native_probe_cpu=0,native_probe_recorded=0,native_probe_submitted=0,native_probe_completed=0,native_probe_rejected=0;
    std::uint64_t native_skin_state_unknown=0,native_skin_unpredicated_missing=0;
    std::uint64_t native_diag_due=0,native_diag_last_activity=UINT64_MAX,native_diag_last_missing=UINT64_MAX;
    std::size_t native_diag_profile_cursor=0;
    const char *native_diag_last_stage=nullptr;
    std::unordered_map<const ModPack *,std::shared_ptr<GeometryAssets>> geometry;
    std::unordered_map<const ModPack *,std::shared_ptr<AuthorStaticAssets>> author_static_assets;
    std::vector<std::shared_ptr<AuthorStaticAssets>> uploading_author_assets;
    std::vector<unsigned char> skin_dxil,copy_dxil,tangent_dxil,shape_dxil,shape_tangent_dxil;
    std::set<std::pair<unsigned,PoseResult>> missing_inputs;
    std::uint64_t frame_device=0;
    std::shared_ptr<PatchAssets> patches;
    RuntimeMutex mutex;
    std::unordered_map<std::uint64_t,std::shared_ptr<Target>> targets;
    struct Tlas {ResourceKey allocation;std::shared_ptr<InstanceBatch> instance;};
    std::unordered_map<std::uint64_t,Tlas> tlas;
    std::unordered_map<std::uint64_t,std::vector<TlasWrite>> tlas_records;
    // One actual scene owner per native AS/actual queue. Cross-queue overwrite
    // order is not guessed; another queue's owner stays until its own overwrite.
    std::map<std::pair<std::uint64_t,std::uint64_t>,SceneOwner> scene_owners;
    std::vector<RetiringSceneOwner> retiring_scene_owners;
    std::unordered_map<std::uint64_t,Holds> records;
    std::unordered_map<std::uint64_t,std::weak_ptr<DrawBindingRecording>> binding_recordings;
    std::vector<std::pair<api::command_list *,std::weak_ptr<DrawBindingRecording>>> pending_draw_resets;
    std::vector<std::shared_ptr<DrawBindingPage>> binding_pages;
    std::unordered_map<std::uint64_t,std::vector<std::shared_ptr<GeometryBatch>>> generated_records;
    std::vector<SurfaceHistory> surface_history;
    std::vector<std::shared_ptr<AuthorPlan>> author_plans;
    std::map<std::pair<std::uint64_t,std::uint64_t>,unsigned> author_primitive_refs;
    std::vector<std::weak_ptr<AuthorPlan>> history_held_plans;
    std::vector<std::weak_ptr<const NativeBlasIdentity>> history_cohorts;
    std::vector<std::shared_ptr<AuthorCommands>> author_commands;
    AuthorStreamPool author_stream_pool;
    bool author_pool_release_due=false;
    AuthorWorkWindow author_work;
    bool history_dirty=false,scene_retirement_due=false;
    std::uint64_t seen_input_history=0,seen_identity_history=0;
    std::uint64_t previous_palette_prepared=0,previous_palette_reused=0;
    bool previous_palette_reported=false;
    std::uint64_t next_author_plan=0,next_author_draw=0,next_author_output=0;
    bool author_path=false;
    // Only SRVs actually observed at a real draw become preparation interests.
    std::vector<PreviousInterest> previous_interests;
    std::vector<PreviousPrepared> previous_prepared;
    std::unordered_map<std::uint64_t,std::weak_ptr<AuthorPreviousWriter>> pending_previous_writers;
    std::array<std::uint64_t,4> previous_history_drops{}; // selection/reset/native writes/resource destruction
    // One slot per actual component/reason, not per frame or AS. An early
    // failure cannot consume the first receipt in the existing armed window.
    using PreviousRejectRows=std::array<PreviousRejectCount,std::size(previous_result_names)+std::size(previous_srv_rejects)>;
    std::map<std::pair<unsigned,unsigned>,PreviousRejectRows> previous_reject_counts;
    std::map<std::pair<std::uint64_t,std::uint64_t>,std::deque<Submission>> submissions;
    std::unordered_map<std::uint64_t,Queue> queues;
    std::map<std::pair<std::uint64_t,unsigned>,NativePaletteCache> native_palettes;
    std::uint64_t palette_creation=0;
    std::uint64_t geometry_count=0,tlas_count=0,sbt_count=0,errors=0;
    NativeIdentityFence first_private_submission;
    bool first_private_completed=false;
    bool active=true;
    explicit Runtime(api::device *d,std::shared_ptr<ProviderModule> owner):provider(std::move(owner)),api_device(d),native_device_id(d->get_native()),native_profiles(load_native_registry()),native_archive(native_profile_cache_path()),source(native_profiles,mod_catalog,mod_resource),
        inputs(native_profiles,d,descriptor_source,mod_position,mod_stamp,&identities,[this](ResourceKey key){return buffer_events.current(key);},
            [this](ResourceKey key,std::uint64_t address,std::uint64_t bytes){return buffer_events.unique_current(key,address,bytes);}){
        gpu::check(reinterpret_cast<ID3D12Device *>(d->get_native())->QueryInterface(IID_PPV_ARGS(&native)),"RT native device5");
        {wchar_t path[32768];const auto length=GetEnvironmentVariableW(L"WWMI_RT_SHAPE_EVIDENCE_DIR",path,DWORD(std::size(path)));
            if(length&&length<std::size(path)){
                static std::atomic<std::uint64_t> evidence_instance{0};
                inputs.configure_shape_evidence(std::filesystem::path(path)/(L"device-"+std::to_wstring(native_device_id)+L"-instance-"+std::to_wstring(++evidence_instance)));
                reshade::log::message(reshade::log::level::info,"RT passive native Shape evidence enabled: creation cache; 60s window waits for ShapeKey consumer; no role/write proof");
            }
        }
        if(const auto issue=native_archive.import(native_profiles);!issue.empty())reshade::log::message(reshade::log::level::warning,issue.c_str());
        if(native_discovery_available)try{
            native_probe_code=read_asset(L"NativeDiscoveryCopy.cso");inputs.enable_native_discovery();discovery_enabled=true;
        }catch(const std::exception &error){
            reshade::log::message(reshade::log::level::warning,(std::string("RT automatic native discovery unavailable: ")+error.what()).c_str());
        }
        native_diag_profile_cursor=native_profiles->snapshot()->profiles.size();
        {char line[512];const char *reason=discovery_enabled?"enabled":!register_forward_v3?"host_v3_missing":!native_buffer_lease?"read_lease_missing":!native_discovery_available?"copy_asset_unavailable":"copy_asset_read_failed";
            std::snprintf(line,sizeof(line),"RT-NATIVE-DISCOVERY capability device=%llx enabled=%u reason=%s host_v3=%u read_lease=%u copy_loaded=%u native_profiles=%llu legacy_static_path=%u",
                (unsigned long long)native_device_id,unsigned(discovery_enabled),reason,unsigned(register_forward_v3!=nullptr),unsigned(native_buffer_lease!=nullptr),unsigned(!native_probe_code.empty()),(unsigned long long)native_diag_profile_cursor,unsigned(!discovery_enabled));
            reshade::log::message(reshade::log::level::info,line);}
        skin_dxil=read_asset(L"SkinPositions.cso");copy_dxil=read_asset(L"CopyFinalPositions.cso");tangent_dxil=read_asset(L"SkinTangents.cso");
        // This client leaves native TLAS and hit tables in place; neither patch pipeline is consumed.
        const auto device=d->get_native();if(!set_frame_device||set_frame_device(device,1)!=1)throw std::runtime_error("RT main rejected pack-frame device lease");frame_device=device;
        author_path=author_consumer&&mod_author_section&&mod_author_revision&&native->GetNodeCount()==1&&author_consumer(device)==1;
        if(author_path)inputs.enable_author_history();
        // Native state/record observations do not require distributed game bytecode.
        // A reflected AS slot is not a payload/SBT proof.
        ray_scene.enabled=author_path&&read_as_slot&&read_native_state_object;
        reshade::log::message(reshade::log::level::info,"RT pack CPU scope v1: native-scene target gates; graphics notifications and unrelated compute/BLAS skip Runtime");
    }
    ~Runtime(){release_frame();}
    void release_frame(){const auto device=std::exchange(frame_device,0);if(device&&set_frame_device)set_frame_device(device,0);}
    void refresh(){auto next=source.refresh(catalog);if(next.load_generation==catalog.load_generation&&next.revision==catalog.revision&&next.native_revision==catalog.native_revision)return;
        // A load-generation change ends the old pack lifetime. Selection
        // revisions and character switches must not discard reusable capacity.
        if(next.load_generation!=catalog.load_generation&&!author_stream_pool.storage.empty())
            author_pool_release_due=true;
        inputs.set_catalog(next);
        auto retained=[&](const std::shared_ptr<const ModPack> &pack){return std::find(next.packs.begin(),next.packs.end(),pack)!=next.packs.end();};
        const auto before=targets.size();std::erase_if(targets,[&](const auto &v){return !retained(v.second->geometry->assets->pack);});
        if(targets.size()!=before)tlas.clear(); // actual scene_owners are independent
        std::erase_if(geometry,[&](const auto &v){return !retained(v.second->pack);});
        std::erase_if(author_static_assets,[&](const auto &v){
            const auto &assets=v.second->bootstrap?v.second->bootstrap:v.second->geometry;
            return !retained(assets->pack)&&!std::any_of(hybrid_assets.begin(),hybrid_assets.end(),[&](const auto &h){
                return h.second->pack==assets->pack&&h.second->pack->load_generation==next.load_generation;
            });
        });
        for(const auto &entry:next.status)reshade::log::message(reshade::log::level::info,("RT Mod "+std::to_string(entry.key)+": "+entry.reason).c_str());
        catalog=std::move(next);missing_inputs.clear();history_dirty=true;
    }
    // Metadata only, under the same Runtime lock as the probe. Failure lines
    // are emitted at Present, not from each rejected native dispatch.
    void note_native_probe_reject(ProbeReject reason,const NativeDiscovery::Bytes &evidence,const char *stage,std::int32_t status=0,const NativeBufferProbe *probe=nullptr,const WWMI_RtNativeBufferLease_v1 *failed_lease=nullptr){
        ++native_probe_rejected;auto &row=native_probe_rejects[unsigned(reason)];++row.count;
        row.id=evidence.id;row.write_version=evidence.write_version;row.range=evidence.range;row.recording=evidence.recording;row.point=evidence.point;row.stage=stage;row.status=status;
        row.source_bytes=probe?probe->diagnostic_source_bytes:0;row.flags=probe?probe->diagnostic_source_flags:0;
        const auto *lease=probe?&probe->lease:failed_lease;row.mode=lease?lease->mode:0;row.sync=lease?lease->sync_after:0;row.access=lease?lease->access_after:0;row.lease_reason=lease?lease->reserved:0;
    }
    void report_native_discovery(bool force=false){
        if(!discovery_enabled)return;const auto now=GetTickCount64();if(!force&&now<native_diag_due)return;native_diag_due=now+5000;
        const auto report=inputs.native_diagnostics();const auto state=native_profiles->snapshot();
        const bool published=state->profiles.size()>native_diag_profile_cursor;
        for(;native_diag_profile_cursor<state->profiles.size();++native_diag_profile_cursor){const auto &profile=state->profiles[native_diag_profile_cursor];char line[384];
            std::snprintf(line,sizeof(line),"RT-NATIVE-DISCOVERY published device=%llx object=%08x hash=%08x N=%u components=%llu native_revision=%llu",
                (unsigned long long)native_device_id,profile->object_guid,profile->content_hash,profile->vertices,(unsigned long long)profile->components.size(),(unsigned long long)state->revision);
            reshade::log::message(reshade::log::level::info,line);
        }
        const bool changed_rejections=std::any_of(native_probe_rejects.begin(),native_probe_rejects.end(),[](const auto &row){return row.count!=row.reported;});
        if(!report.missing_intents&&!published&&!changed_rejections&&(native_diag_last_missing==UINT64_MAX||!native_diag_last_missing))return;
        std::uint64_t not_submitted=0,no_signal=0,fence_wait=0,oldest_ms=0;
        for(const auto &probe:native_probe_pending)if(!probe->delivered&&probe->recorded){
            if(!probe->submissions)++not_submitted;
            else if(!probe->completion.value)++no_signal;
            else{const auto q=queues.find(probe->completion.queue);if(q==queues.end()||reinterpret_cast<std::uint64_t>(q->second.fence.Get())!=probe->completion.fence||q->second.completed<probe->completion.value)++fence_wait;}
            if(probe->diagnostic_recorded_ms&&now>=probe->diagnostic_recorded_ms)oldest_ms=(std::max)(oldest_ms,now-probe->diagnostic_recorded_ms);
        }
        const auto &total=report.total;const char *stage="await_content";
        if(!report.missing_intents)stage="resolved";
        else if(!total.builds)stage="await_target_build";
        else if(report.unforwarded_builds)stage="await_native_build_receipt";
        else if(not_submitted||report.unsubmitted_builds)stage="await_native_submit";
        else if(no_signal)stage="await_signal";
        else if(fence_wait)stage="await_fence";
        else if(native_probe_rejected&&!native_probe_recorded&&!native_probe_cpu)stage="probe_not_issued";
        else if(total.last_resolution_failure!=NativeDiscovery::Failure::none)stage=NativeDiscovery::failure_name(total.last_resolution_failure);
        std::uint64_t first_failures=0;for(unsigned i=1;i<unsigned(NativeDiscovery::Failure::count);++i)if(total.rejected[i])first_failures|=1ull<<i;
        const auto activity=total.blas_seen+total.builds+total.submitted_builds+total.published+native_probe_requested+native_probe_completed+native_probe_rejected+first_failures+std::uint64_t(total.skin_seen!=0)+std::uint64_t(total.skin_facts!=0)+std::uint64_t(total.copy_facts!=0)+std::uint64_t(native_skin_state_unknown!=0)+std::uint64_t(native_skin_unpredicated_missing!=0);
        char line[3072];
        const bool summary_changed=force||published||native_diag_last_missing!=report.missing_intents||native_diag_last_stage!=stage||native_diag_last_activity!=activity||fence_wait||not_submitted||no_signal;
        if(summary_changed){
        // Callback sightings do not prove a missing target matched: the existing
        // static-profile prefilter can also admit them. Raw skin churn alone
        // does not retrigger the bounded summary.
        native_diag_last_missing=report.missing_intents;native_diag_last_stage=stage;native_diag_last_activity=activity;
        char rejects[896]{};std::size_t used=0;
        for(unsigned i=1;i<unsigned(NativeDiscovery::Failure::count);++i)if(total.rejected[i]&&used+48<sizeof(rejects)){
            const auto n=std::snprintf(rejects+used,sizeof(rejects)-used,"%s%s:%llu",used?",":"",NativeDiscovery::failure_name(NativeDiscovery::Failure(i)),(unsigned long long)total.rejected[i]);if(n>0)used+=(std::min)(std::size_t(n),sizeof(rejects)-used-1);
        }
        char wm_diag[128]{};
        if(total.rejected[unsigned(NativeDiscovery::Failure::writer_missing)]&&total.last_wm_n){
            std::snprintf(wm_diag,sizeof(wm_diag)," wm=%08x/c%u/v%u/%u/bw%u/rw%u",
                total.last_wm_guid,total.last_wm_comp,total.last_wm_index,total.last_wm_n,total.last_wm_writers,total.last_wm_res_writers);
        }
        std::snprintf(line,sizeof(line),"RT-NATIVE-DISCOVERY summary device=%llx stage=%s missing_intents=%llu enabled_missing=%llu pending_builds=%llu unsubmitted_builds=%llu unforwarded_builds=%llu pending_bytes=%llu pending_writers=%llu rest_candidates=%llu skin_seen=%llu skin_facts=%llu skin_state_unknown=%llu skin_unpredicated_missing=%llu copy_facts=%llu blas_seen=%llu builds=%llu submitted_builds=%llu retired_unproved=%llu reset_unsubmitted=%llu rest_requests=%llu ib_requests=%llu probe_requested=%llu cpu_proofs=%llu gpu_recorded=%llu gpu_submitted=%llu gpu_completed=%llu probe_rejected=%llu not_submitted=%llu no_signal=%llu fence_wait=%llu oldest_probe_ms=%llu published=%llu catalog_native_revision=%llu registry_revision=%llu reject_attempts=%s%s",
            (unsigned long long)native_device_id,stage,(unsigned long long)report.missing_intents,(unsigned long long)report.enabled_missing_intents,
            (unsigned long long)report.pending.builds,(unsigned long long)report.unsubmitted_builds,(unsigned long long)report.unforwarded_builds,(unsigned long long)report.pending.bytes,(unsigned long long)report.pending.writers,(unsigned long long)report.pending.rest_candidates,
            (unsigned long long)total.skin_seen,(unsigned long long)total.skin_facts,(unsigned long long)native_skin_state_unknown,(unsigned long long)native_skin_unpredicated_missing,(unsigned long long)total.copy_facts,(unsigned long long)total.blas_seen,(unsigned long long)total.builds,(unsigned long long)total.submitted_builds,(unsigned long long)total.retired_unproved,(unsigned long long)total.reset_builds,(unsigned long long)total.rest_requests,(unsigned long long)total.index_requests,
            (unsigned long long)native_probe_requested,(unsigned long long)native_probe_cpu,(unsigned long long)native_probe_recorded,(unsigned long long)native_probe_submitted,(unsigned long long)native_probe_completed,(unsigned long long)native_probe_rejected,
            (unsigned long long)not_submitted,(unsigned long long)no_signal,(unsigned long long)fence_wait,(unsigned long long)oldest_ms,(unsigned long long)total.published,(unsigned long long)catalog.native_revision,(unsigned long long)state->revision,used?rejects:"none",wm_diag);
        reshade::log::message(reshade::log::level::info,line);
        } // Due rejection rows are independent of summary activity.
        for(unsigned i=0;i<native_probe_rejects.size();++i){auto &row=native_probe_rejects[i];if(row.count==row.reported||(!force&&row.last_log&&now-row.last_log<30000))continue;
            const auto point=row.point==NativeDiscovery::ReadPoint::rest_consumer?"rest":row.point==NativeDiscovery::ReadPoint::index_consumer?"ib":"copy";
            std::snprintf(line,sizeof(line),"RT-NATIVE-DISCOVERY probe_reject device=%llx reason=%s stage=%s status=%d count=%llu id=%llu resource=%llx/%llu base=%llx logical_offset=%llu logical_bytes=%llu recording=%llx/%llu/%llu point=%s write_version=%llu source_bytes=%llu source_flags=%x lease_mode=%u sync=%llx access=%llx lease_reason=%s(%u)",
                (unsigned long long)native_device_id,probe_reject_name(ProbeReject(i)),row.stage,int(row.status),(unsigned long long)row.count,(unsigned long long)row.id,
                (unsigned long long)row.range.resource.object,(unsigned long long)row.range.resource.creation,(unsigned long long)row.range.base,(unsigned long long)row.range.offset,(unsigned long long)row.range.bytes,
                (unsigned long long)row.recording.list,(unsigned long long)row.recording.creation,(unsigned long long)row.recording.generation,point,(unsigned long long)row.write_version,(unsigned long long)row.source_bytes,row.flags,row.mode,(unsigned long long)row.sync,(unsigned long long)row.access,wwmi_rt_native_buffer_lease_rejection(row.lease_reason),row.lease_reason);
            reshade::log::message(reshade::log::level::warning,line);row.reported=row.count;row.last_log=now;
        }
    }
    void issue_native_requests(api::command_list *list,NativeDiscovery::ReadPoint point){
        for(auto &evidence:inputs.native_requests(list,point)){
            ++native_probe_requested;std::uint64_t address=0;bool cpu=false;
            if(!inputs.native_request_input(*evidence,address,cpu)){note_native_probe_reject(ProbeReject::input,*evidence,"input_resource");inputs.native_request_cancel(evidence->id);continue;}
            auto source_hold=buffer_events.acquire_current(evidence->range.resource);
            if(!source_hold){note_native_probe_reject(ProbeReject::hold,*evidence,"source_hold");inputs.native_request_cancel(evidence->id);continue;}
            // Same actual consumer and same lease call as 02; only retain its
            // status for the bounded failure summary.
            WWMI_RtNativeBufferLease_v1 lease{sizeof(lease)};
            const auto lease_status=native_buffer_lease?native_buffer_lease(reinterpret_cast<std::uint64_t>(list),reinterpret_cast<std::uint64_t>(source_hold.Get()),evidence->range.offset,evidence->range.bytes,
                point==NativeDiscovery::ReadPoint::rest_consumer?1u:2u,&lease):-1;
            if(!native_buffer_lease||lease_status){
                note_native_probe_reject(ProbeReject::logical_lease,*evidence,"logical_read_lease",lease_status,nullptr,&lease);inputs.native_request_cancel(evidence->id);continue;
            }
            if(cpu){if(!inputs.native_request_cpu(*evidence)){note_native_probe_reject(ProbeReject::cpu_read,*evidence,"cpu_upload_read");inputs.native_request_cancel(evidence->id);}else ++native_probe_cpu;continue;}
            try{
                if(!native_probe_program)native_probe_program=std::make_shared<NativeBufferProbeProgram>(native.Get(),native_probe_code);
                auto probe=std::make_shared<NativeBufferProbe>();probe->evidence=evidence;probe->program=native_probe_program;
                if(!probe->prepare(native.Get(),reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native()),std::move(source_hold),address,native_buffer_lease)){
                    note_native_probe_reject(ProbeReject::prepare,*evidence,probe->prepare_stage,probe->prepare_status,probe.get());inputs.native_request_cancel(evidence->id);continue;
                }
                const auto key=reinterpret_cast<std::uint64_t>(list);
                records[key].push_back(probe);native_probe_records[key].push_back(probe);native_probe_pending.push_back(probe);
                const auto result=compute_run(key,NativeBufferProbe::work,probe.get());
                if(result||!probe->recorded){note_native_probe_reject(ProbeReject::run,*evidence,"compute_run",result,probe.get());inputs.native_request_cancel(evidence->id);probe->delivered=true;}
                else{++native_probe_recorded;probe->diagnostic_recorded_ms=GetTickCount64();}
            }catch(const std::exception &error){
                note_native_probe_reject(ProbeReject::exception,*evidence,"gpu_prepare_or_record_exception");inputs.native_request_cancel(evidence->id);
                if(!native_probe_error_reported){native_probe_error_reported=true;reshade::log::message(reshade::log::level::warning,(std::string("RT native discovery probe: ")+error.what()).c_str());}
            }
        }
    }
    void settle_native_probes(){
        std::erase_if(native_probe_pending,[&](const auto &probe){
            if(probe->delivered)return true;
            if(!probe->recorded)return false;
            if(probe->submissions>1){note_native_probe_reject(ProbeReject::replay,*probe->evidence,"submission_replay",int(probe->submissions),probe.get());inputs.native_request_cancel(probe->evidence->id);probe->delivered=true;return true;}
            if(probe->submissions!=1||!probe->completion.value)return false;
            const auto q=queues.find(probe->completion.queue);
            if(q==queues.end()||reinterpret_cast<std::uint64_t>(q->second.fence.Get())!=probe->completion.fence)return false;
            const auto done=q->second.completed;if(done!=UINT64_MAX&&done<probe->completion.value)return false;
            if(done==UINT64_MAX||!probe->evidence->valid||probe->submissions!=1){note_native_probe_reject(ProbeReject::invalidated,*probe->evidence,done==UINT64_MAX?"device_removed":"content_version_invalid",0,probe.get());inputs.native_request_cancel(probe->evidence->id);}
            else{std::vector<unsigned char> bytes;
                if(!probe->read(bytes)){note_native_probe_reject(ProbeReject::readback,*probe->evidence,"readback_map_or_data",0,probe.get());inputs.native_request_cancel(probe->evidence->id);}
                else if(!inputs.native_completed(probe->evidence->id,probe->completion.fence,done,std::move(bytes))){note_native_probe_reject(ProbeReject::completion,*probe->evidence,"completed_proof_rejected",0,probe.get());inputs.native_request_cancel(probe->evidence->id);}
                else ++native_probe_completed;
            }
            probe->delivered=true;return true;
        });
    }
    void collect(){
        rt_cpu::Scope measured(rt_cpu::collect);
        bool retired=false;
        for(auto &[queue,q]:queues){
            retired|=collect_author_producers(q);
            if(!q.fence)continue;
            const auto fence=reinterpret_cast<std::uint64_t>(q.fence.Get());
            if(q.completed<q.value){
                trace.emit("completion","get_begin",queue,fence,q.value);
                rt_cpu::Scope poll(rt_cpu::fence_poll);const auto done=q.fence->GetCompletedValue();poll.end();
                trace.emit("completion","get_end",queue,fence,done);
                if(done!=q.completed){
                    q.completed=done;identities.completion(fence,done);
                    if(!retiring_scene_owners.empty())scene_retirement_due=true;
                }
            }
            // A newly registered owner can already be covered by cached
            // completion. Retirement is not conditional on a fresh poll.
            if(!first_private_completed&&first_private_submission.queue==queue&&first_private_submission.fence==fence&&
               q.completed!=UINT64_MAX&&q.completed>=first_private_submission.value){
                first_private_completed=true;trace.emit("first_private_done","completed",queue,fence,q.completed);
            }
            while(q.completed!=UINT64_MAX&&!q.flights.empty()&&q.flights.front().value<=q.completed){q.flights.pop_front();retired=true;}
        }
        settle_native_probes();
        inputs.publish_discovery();
        settle_occlusion_instance_probe();
        settle_raygen_probe();
        collect_author_uploads();
        if(scene_retirement_due){
            scene_retirement_due=false;
            retired|=std::erase_if(retiring_scene_owners,[&](const auto &old){const auto q=queues.find(old.overwritten.queue);return q!=queues.end()&&reinterpret_cast<std::uint64_t>(q->second.fence.Get())==old.overwritten.fence&&q->second.completed!=UINT64_MAX&&q->second.completed>=old.overwritten.value;})!=0;
        }
        if(retired)history_owners_changed();
        const auto input=inputs.history_revision(),identity=identities.validity_revision();
        if(history_dirty||seen_input_history!=input||seen_identity_history!=identity){
            history_dirty=false;seen_input_history=input;seen_identity_history=identity;
            sweep_history();retired=true;
        }
        if(retired&&author_pool_release_due)release_unloaded_author_storage();
    }
    void release_unloaded_author_storage()noexcept{
        author_stream_pool.release_idle();
        // If the load left no pack, free the remaining backings as their
        // real owners retire. With a live pack, late retirements can be reused.
        author_pool_release_due=catalog.packs.empty()&&!author_stream_pool.storage.empty();
    }
    void history_owners_changed()noexcept{
        if(history_dirty)return;
        history_dirty=std::any_of(history_held_plans.begin(),history_held_plans.end(),[](const auto &p){return p.use_count()==1;})||
            std::any_of(history_cohorts.begin(),history_cohorts.end(),[](const auto &cohort){return cohort.expired();});
    }
    void sweep_history(){
        rt_cpu::Scope history_measured(rt_cpu::history);
        history_measured.add(native_palettes.size()+author_plans.size()+surface_history.size()+previous_interests.size()+previous_prepared.size());
        history_held_plans.clear();history_cohorts.clear();
        std::erase_if(native_palettes,[&](const auto &entry){return !identities.live(entry.second.identity,entry.second.component);});
        std::erase_if(author_plans,[&](const auto &p){
            if(inputs.resource_current(p->primitive)){
                const auto &parts=p->identity->components;
                if(std::any_of(parts.begin(),parts.end(),[&](const auto &c){return identities.live(p->identity,c.sample.component);}))return false;
                // The old BLAS may be gone while its actual position writer
                // is still the input to a later previous-position SRV.
                if(p->requires_source&&p->pose&&
                   std::any_of(parts.begin(),parts.end(),[&](const auto &c){
                       const auto i=c.sample.component;
                       const bool completed=i<p->pose->final_positions.size()&&p->pose->final_positions[i].completed_author;
                       const bool adopted=i<p->sources.size()&&p->sources[i].section&&!p->sources[i].empty&&
                           std::find(catalog.packs.begin(),catalog.packs.end(),p->pack)!=catalog.packs.end();
                       return (completed||adopted)&&inputs.position_live(c.sample);
                   }))return false;
            }
            if(p.use_count()==1){
                const auto primitive=author_primitive_refs.find({p->primitive.object,p->primitive.creation});
                if(--primitive->second==0)author_primitive_refs.erase(primitive);
                return true;
            }
            history_held_plans.push_back(p);return false;
        });
        std::erase_if(surface_history,[&](const auto &h){
            if(h.pack&&std::find(catalog.packs.begin(),catalog.packs.end(),h.pack)==catalog.packs.end()){++previous_history_drops[0];return true;}
            if(!h.native_signal.fence&&inputs.recording(reinterpret_cast<api::command_list *>(h.identity->recording.list))!=h.identity->recording&&
               (!h.batch||!h.batch->gpu_submissions)){++previous_history_drops[1];return true;}
            const bool gone=std::none_of(h.identity->components.begin(),h.identity->components.end(),[&](const auto &c){return inputs.position_live(c.sample);});
            if(gone)++previous_history_drops[2];return gone;
        });
        std::erase_if(previous_interests,[&](const auto &i){
            return std::find(catalog.packs.begin(),catalog.packs.end(),i.pack)==catalog.packs.end()||
                !inputs.resource_current(i.output)||!inputs.resource_current(i.content);
        });
        std::erase_if(previous_prepared,[&](const auto &p){
            const bool gone=std::find(catalog.packs.begin(),catalog.packs.end(),p.source.pack)==catalog.packs.end()||
                (p.route==PreviousRoute::cohort_candidate&&p.cohort.expired())||
                (!p.native_submit_queue&&(!p.native_receipt||!p.native_receipt->signal.fence)&&(!p.batch||!p.batch->gpu_submissions)&&
                 inputs.recording(reinterpret_cast<api::command_list *>(p.native_writer.list))!=p.native_writer)||
                !previous_inputs_live(p.proofs,p.source.pack,p.native_writer,p.route,p.historical,false);
            if(!gone&&p.route==PreviousRoute::cohort_candidate)history_cohorts.push_back(p.cohort);
            return gone;
        });
    }
    void stop(){
        if(author_work.end)rt_phase_cpu::close(true);
        active=false;release_frame();
        report_previous_palette(true);
        for(const auto &[key,rows]:previous_reject_counts)for(const auto &row:rows)if(row.count){
            char line[320];std::snprintf(line,sizeof(line),"RT-PREV-REJECT-SUMMARY object=%u component=%u result=%s srv_reject=%s count=%llu unarmed=%llu armed_window=%llu after_window=%llu first_request_frame=%llu last_request_frame=%llu",
                key.first,key.second,row.reason,row.srv_reject?row.srv_reject:"none",(unsigned long long)row.count,
                (unsigned long long)(row.count-row.armed-row.after_window),(unsigned long long)row.armed,(unsigned long long)row.after_window,
                (unsigned long long)row.first_frame,(unsigned long long)row.last_frame);
            reshade::log::message(reshade::log::level::info,line);
        }
        for(auto &[_,q]:queues)if(!q.flights.empty()&&(!q.fence||q.fence->GetCompletedValue()<q.value)){
            if(!q.fence){if(FAILED(native->GetDeviceRemovedReason()))continue;throw std::runtime_error("RT own GPU submission lacks a retirement fence");}
            HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("RT shutdown fence event");
            const auto hr=q.fence->SetEventOnCompletion(q.value,event);
            const DWORD wait=SUCCEEDED(hr)?WaitForSingleObject(event,5000):WAIT_FAILED;CloseHandle(event);
            if(wait!=WAIT_OBJECT_0&&SUCCEEDED(native->GetDeviceRemovedReason()))throw std::runtime_error("RT own GPU submissions have not retired");
        }
        if(SUCCEEDED(native->GetDeviceRemovedReason())){
            collect();
            for(auto &[_,q]:queues){collect_author_producers(q);if(q.producer_head)throw std::runtime_error("RT author producer commands have not retired");}
            collect_author_uploads();
            if(!uploading_author_assets.empty())throw std::runtime_error("RT author static upload has not retired");
            for(const auto &[_,pending]:submissions)
                if(std::any_of(pending.begin(),pending.end(),[](const auto &s){return !s.author_generated.empty()||bool(s.instance_probe)||bool(s.raygen_probe)||!s.native_probes.empty();}))
                    throw std::runtime_error("RT owned GPU work lacks its paired native submit receipt");
        }
        if(instance_probe_watch.resource){
            auto hint=instance_dispatch_hint.list;
            instance_dispatch_watch.compare_exchange_strong(hint,0,std::memory_order_relaxed);
            if(instance_probe&&!instance_probe->reported)
                occlusion_probe_status(4096,"runtime_stopped_before_sample_delivery",&instance_probe->build);
        }
        if(raygen_probe){
            const auto q=queues.find(raygen_probe->completion.queue);
            if(q!=queues.end()&&q->second.fence)q->second.completed=q->second.fence->GetCompletedValue();
            settle_raygen_probe();
            if(raygen_probe)settle_raygen_probe("runtime_stopped_before_readback");
        }
        report_author_work(true);report_native_discovery(true);
        native_probe_pending.clear();native_probe_records.clear();native_probe_program.reset();
        queues.clear();submissions.clear();records.clear();binding_recordings.clear();pending_draw_resets.clear();binding_pages.clear();generated_records.clear();surface_history.clear();previous_interests.clear();previous_prepared.clear();pending_previous_writers.clear();native_palettes.clear();tlas.clear();tlas_records.clear();scene_owners.clear();retiring_scene_owners.clear();targets.clear();
        author_plans.clear();author_primitive_refs.clear();history_held_plans.clear();history_cohorts.clear();
        author_static_assets.clear();uploading_author_assets.clear();author_commands.clear();author_stream_pool.release_idle();
        reshade::log::message(reshade::log::level::info,("RT pack stopped: geometry="+std::to_string(geometry_count)+" tlas="+std::to_string(tlas_count)+" sbt="+std::to_string(sbt_count)+" errors="+std::to_string(errors)).c_str());
    }
    bool writer_signal_known(NativeIdentityFence signal,bool require_completed)const{
        if(!signal.queue||!signal.fence||!signal.value)return false;
        const auto q=queues.find(signal.queue);
        return q!=queues.end()&&reinterpret_cast<std::uint64_t>(q->second.fence.Get())==signal.fence&&signal.value<=q->second.value&&
            (!require_completed||(q->second.completed!=UINT64_MAX&&q->second.completed>=signal.value));
    }
    bool completed(const GeometryBatch &b) const {
        if(b.gpu_submissions!=1||!b.gpu_queue||!b.gpu_fence||!b.gpu_value)return false;
        const auto q=queues.find(b.gpu_queue);return q!=queues.end()&&reinterpret_cast<std::uint64_t>(q->second.fence.Get())==b.gpu_fence&&q->second.completed!=UINT64_MAX&&q->second.completed>=b.gpu_value;
    }
    bool previous_inputs_live(const std::vector<SkinSample> &proofs,const std::shared_ptr<const ModPack> &pack,Recording private_writer,
        PreviousRoute route,const PreviousHistoricalWriter &historical,bool require_submitted){
        if(!historical.recording.list)return inputs.previous_sources_live(proofs,pack,require_submitted);
        return route==PreviousRoute::cohort_candidate&&!proofs.empty()&&proofs.front().recording==private_writer&&historical.recording!=private_writer&&
            writer_signal_known(historical.signal,require_submitted)&&inputs.previous_cohort_sources_live(proofs,pack,require_submitted,historical.recording);
    }
    bool previous_author_inputs(const PreviousPrepared &p){
        if(!p.owned_pose||p.proofs.empty()||p.native_writer!=p.proofs.front().recording)return false;
        if(p.route==PreviousRoute::submitted_ring)return inputs.previous_sources_on_queue(p.proofs,p.source.pack,p.native_submit_queue);
        if(!p.native_receipt||
           p.native_receipt->recording!=p.native_writer||!writer_signal_known(p.native_receipt->signal,false))return false;
        if(!p.historical.recording.list)return inputs.previous_sources_live(p.proofs,p.source.pack,true);
        return p.route==PreviousRoute::cohort_candidate&&p.historical.recording!=p.native_writer&&
            writer_signal_known(p.historical.signal,false)&&
            inputs.previous_cohort_sources_live(p.proofs,p.source.pack,true,p.historical.recording);
    }
    bool gpu_ready(const GeometryBatch &b,Recording consumer) const {return b.recorded&&((!b.gpu_submissions&&b.gpu_recording==consumer)||completed(b));}
    void remember_previous(const std::shared_ptr<const ModPack> &pack,ResourceKey output,std::uint64_t offset,std::uint64_t bytes,
        unsigned component,ResourceKey content,std::uint64_t content_offset,bool scoped,bool author){
        const auto same=[&](const PreviousInterest &i){return i.pack==pack&&i.output==output&&i.view_offset==offset&&i.view_bytes==bytes&&
            i.component==component&&i.content==content&&i.content_offset==content_offset;};
        const auto found=std::find_if(previous_interests.begin(),previous_interests.end(),same);
        if(found==previous_interests.end())previous_interests.push_back({pack,output,content,offset,bytes,content_offset,component,scoped,author,!author});
        else {found->scoped_demand|=scoped;found->author_demand|=author;found->legacy_demand|=!author;}
    }
    const char *record_previous(const PreviousInterest &i,ComponentAssembly &candidate,Recording epoch,PreviousRoute route,
        const std::shared_ptr<const NativeBlasIdentity> &cohort={},PreviousHistoricalWriter historical={},bool cpu_only=false){
        const auto stage=[&](const char *plain,const char *linked){return route==PreviousRoute::linked_blas?linked:plain;};
        if(!previous_inputs_live(candidate.sources,i.pack,epoch,route,historical,false))return stage("source_not_live","linked_source_not_live");
        const auto same=[&](const PreviousPrepared &p){
            if(p.source.pack!=i.pack||p.source.output!=i.output||p.source.view_offset!=i.view_offset||
                p.source.view_bytes!=i.view_bytes||p.source.component!=i.component||p.source.content!=i.content||
                p.source.content_offset!=i.content_offset||p.proofs.size()!=candidate.sources.size())return false;
            if(p.route==PreviousRoute::cohort_candidate&&(route!=p.route||p.cohort.lock()!=cohort))return false;
            return std::equal(p.proofs.begin(),p.proofs.end(),candidate.sources.begin(),[](const SkinSample &a,const SkinSample &b){
                return a.component==b.component&&a.output==b.output&&a.output_offset==b.output_offset&&a.output_bytes==b.output_bytes&&
                    a.content==b.content&&a.content_offset==b.content_offset&&a.order==b.order&&a.recording==b.recording;
            });
        };
        auto saved=std::find_if(previous_prepared.begin(),previous_prepared.end(),same);
        const bool own=author_path&&i.author_demand&&owned_author_pack(i.pack)&&candidate.sources.front().recording==epoch;
        if(cpu_only&&!own)return stage("author_unavailable","linked_author_unavailable");
        const bool legacy=!cpu_only&&i.legacy_demand;
        if(!own&&!legacy)return stage("no_author_or_legacy_route","linked_no_author_or_legacy_route");
        if(saved!=previous_prepared.end()&&(!own||saved->owned_pose)&&(!legacy||saved->batch))
            return stage("already_prepared","linked_already_prepared");
        if(own){
            if(saved==previous_prepared.end()){
                previous_prepared.push_back({i,std::move(candidate.sources),{},route,cohort,historical,{},epoch,{},{}});
                if(route==PreviousRoute::cohort_candidate)history_dirty=true;
                saved=std::prev(previous_prepared.end());
            }
            if(!saved->owned_pose){
                auto &pending=pending_previous_writers[epoch.list];
                auto receipt=pending.lock();
                if(!receipt||receipt->recording!=epoch){
                    receipt=std::make_shared<AuthorPreviousWriter>();receipt->recording=epoch;pending=receipt;
                }
                saved->native_receipt=std::move(receipt);
                saved->owned_pose=std::move(candidate.assembly.pose);
            }
            if(!legacy)return stage("author_cpu_saved","linked_author_cpu_saved");
        }
        WWMI_ComputeStateInfo_v1 state{sizeof(state)};
        if(!compute_query||compute_query(epoch.list,&state)!=0||(route==PreviousRoute::dispatch&&state.pipeline_kind!=0))
            return stage("compute_state","linked_compute_state");
        auto assets=geometry.find(i.pack.get());
        if(const auto prepared=author_static_assets.find(i.pack.get());assets==geometry.end()&&prepared!=author_static_assets.end()){
            // The legacy writer-list route used to see the same assets as an
            // owned prefix. Do not make it depend on a future graphics upload.
            rt_cpu::Scope measured(rt_cpu::assets);
            auto legacy_assets=prepared->second->bootstrap;
            if(!legacy_assets)legacy_assets=GeometryAssets::create(native.Get(),i.pack,skin_dxil,copy_dxil,tangent_dxil);
            assets=geometry.emplace(i.pack.get(),std::move(legacy_assets)).first;
        }
        if(assets==geometry.end()||!assets->second||assets->second->pack!=i.pack)return stage("assets","linked_assets");
        rt_cpu::Scope allocation_measured(rt_cpu::batch_create);
        auto batch=GeometryBatch::prepare_streams(assets->second,own?*saved->owned_pose:*candidate.assembly.pose,i.component);allocation_measured.end();
        batch->gpu_recording=epoch;records[epoch.list].push_back(batch);generated_records[epoch.list].push_back(batch);
        rt_cpu::Scope recording_measured(rt_cpu::skin_record);const auto status=batch->record(epoch.list,compute_run);recording_measured.end();
        if(status!=0||!batch->recorded)return stage("record_failed","linked_record_failed");
        if(saved!=previous_prepared.end())saved->batch=std::move(batch);
        else{
            previous_prepared.push_back({i,std::move(candidate.sources),std::move(batch),route,cohort,historical,{},epoch,{},{}});
            if(route==PreviousRoute::cohort_candidate)history_dirty=true;
        }
        return stage("recorded","linked_recorded");
    }
    void prepare_previous(api::command_list *c,InputCollector::DispatchWrite write){
        if(!write.resource||previous_interests.empty())return;
        const auto epoch=inputs.recording(c);if(!epoch.list||!epoch.creation||!epoch.generation)return;
        for(auto &i:previous_interests){
            if(i.output!=write.resource||i.component>=i.pack->components.size())continue;
            const auto &target=i.pack->components[i.component];
            // An independent author component can keep its exact CPU write
            // even when this writer never produces a complete native BLAS.
            // The old GPU route still waits for its real BLAS/alternate point.
            const bool independent=target.dependencies.empty();
            if(independent&&!(author_path&&i.author_demand&&owned_author_pack(i.pack)))continue;
            if(write.component!=i.component&&std::none_of(target.dependencies.begin(),target.dependencies.end(),
                [&](const auto &d){return d.provider==write.component;}))continue;
            i.last_write_component=write.component;i.last_write_order=write.order;i.last_recording=epoch;
            auto candidate=inputs.assemble_previous_component(c,i.pack,i.output,i.view_offset,i.component,i.content,i.content_offset,
                rt_cpu::phase.load(std::memory_order_relaxed)==1);
            i.last_pose=candidate.assembly.result;i.missing_component=candidate.missing_component;
            i.local_same=candidate.local_same;i.local_other=candidate.local_other;i.other_recordings=candidate.other_recordings;
            if(candidate.assembly.result!=PoseResult::complete||!candidate.assembly.pose){i.last_stage="pose_unavailable";continue;}
            if(independent&&(candidate.sources.empty()||candidate.sources.front().order!=write.order))continue;
            // This callback is entered immediately before the game's real skin
            // Dispatch. Restore the captured compute PSO/root state before that
            // dispatch; never inject compute in the later graphics draw.
            i.last_stage=record_previous(i,candidate,epoch,PreviousRoute::dispatch,{},{},independent);
        }
    }
    void prepare_previous_blas(api::command_list *c,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc,bool full_surface){
        // Called by the existing native BLAS-after callback. The original BLAS
        // has already been recorded; this command point is outside render pass.
        // Dependent parts may use only a real BLAS link or the exact same-SRV
        // dispatch proof. A different ring in this recording is not that link.
        const auto epoch=inputs.recording(c);
        for(auto &i:previous_interests){
            if(i.component>=i.pack->components.size()||i.pack->components[i.component].dependencies.empty())continue;
            auto proof=inputs.assemble_previous_blas(c,desc,i.pack,i.output,i.view_offset,i.component,i.content,i.content_offset);
            if(!proof.relevant)continue;
            i.link_stage=proof.stage;i.link_order=proof.target_order;i.link_recording=proof.target_recording;i.link_build_recording=proof.recording;
            i.provider_order=proof.provider_order;i.provider_output=proof.provider_output;
            if(proof.candidate.assembly.result==PoseResult::complete&&proof.candidate.assembly.pose){
                if(full_surface)i.link_stage="linked_full_surface_path";
                else i.link_stage=record_previous(i,proof.candidate,epoch,PreviousRoute::linked_blas);
            }
            if(i.failed_seq&&i.failed_order==i.link_order&&i.failed_recording==i.link_recording){
                char line[460];std::snprintf(line,sizeof(line),"RT-PREV-LINK-AFTER seq=%llu target_order=%llu record=%llx/%llu/%llu build_record=%llx/%llu/%llu stage=%s provider_order=%llu provider=%llx/%llu",
                    (unsigned long long)i.failed_seq,(unsigned long long)i.link_order,(unsigned long long)i.link_recording.list,
                    (unsigned long long)i.link_recording.creation,(unsigned long long)i.link_recording.generation,
                    (unsigned long long)i.link_build_recording.list,(unsigned long long)i.link_build_recording.creation,(unsigned long long)i.link_build_recording.generation,i.link_stage,
                    (unsigned long long)i.provider_order,(unsigned long long)i.provider_output.object,(unsigned long long)i.provider_output.creation);
                reshade::log::message(reshade::log::level::info,line);i.failed_seq=0;
            }
        }
    }
    bool select_previous_candidate_layout(const PreviousInterest &target,Recording epoch,BlasPoseInput &layout){
        const auto &pack=target.pack;const auto span=pack->native->bytes();
        if(!target.scoped_demand||target.view_bytes<span)return false;
        layout={epoch,{}};layout.geometries.resize(pack->components.size());
        layout.geometries[target.component]={target.output,target.view_offset,span};
        for(const auto &dependency:pack->components[target.component].dependencies){
            const auto &part=pack->components[dependency.provider].native;
            GeometryInput found;
            for(const auto &provider:previous_interests){
                if(!provider.scoped_demand||provider.pack!=pack||provider.component!=dependency.provider||
                    provider.content!=target.content||provider.content_offset!=target.content_offset||provider.view_bytes<span)continue;
                const auto stamp=inputs.position_receipt(provider.output,provider.view_offset+12ull*part.first,12ull*part.count);
                if(!stamp.order||stamp.producer!=epoch||stamp.submissions)continue;
                if(found.vertex&&(found.vertex!=provider.output||found.offset!=provider.view_offset))return false;
                found={provider.output,provider.view_offset,span};
            }
            if(!found.vertex)return false;
            layout.geometries[dependency.provider]=found;
        }
        return true;
    }
    std::vector<const SurfaceHistory *> retained_previous_providers(const PreviousInterest &target,const NativeBlasIdentity &current){
        const auto &pack=target.pack;const auto span=pack->native->bytes();
        const auto &dependencies=pack->components[target.component].dependencies;
        std::vector<const SurfaceHistory *> selected;
        if(!target.scoped_demand||target.view_bytes<span||dependencies.empty())return selected;
        for(const auto &h:surface_history){
            if(h.completed_plan)continue; // Only used to reconstruct native/REST palettes.
            const auto &old=*h.identity;
            const bool bare_author=!h.pack&&target.author_demand&&owned_author_pack(pack);
            if((h.pack!=pack&&!bare_author)||old.recording==current.recording||old.components.size()!=current.components.size()||
                !writer_signal_known(h.native_signal,false))continue;
            if(!bare_author&&!h.owned_pose&&(!h.batch||h.batch->source_orders.size()!=old.components.size()||
                h.batch->producer.recording!=old.recording||h.batch->gpu_recording!=old.recording||h.batch->gpu_submissions!=1))continue;
            const auto matches=std::all_of(dependencies.begin(),dependencies.end(),[&](const auto &d){
                const auto &c=old.components[d.provider],&now=current.components[d.provider];const auto &s=c.sample;
                if(s.recording!=old.recording||s.component!=d.provider||s.profile!=pack->native||s.palette.empty()||
                    s.content!=target.content||s.content_offset!=target.content_offset||c.indices!=now.indices||
                    c.index_offset!=now.index_offset||c.index_count!=now.index_count||c.vertices.vertex!=s.output||c.vertices.bytes!=span)return false;
                const auto &n=pack->components[d.provider].native;
                if(s.output_offset!=c.vertices.offset+12ull*n.first||s.output_bytes!=12ull*n.count||inputs.position_submission_count(s)!=1)return false;
                return std::any_of(previous_interests.begin(),previous_interests.end(),[&](const auto &i){return
                    i.scoped_demand&&i.pack==pack&&i.component==d.provider&&i.content==s.content&&i.content_offset==s.content_offset&&
                    i.output==s.output&&i.view_offset==c.vertices.offset&&i.view_bytes>=span;});
            });
            if(!matches)continue;
            // Keep each exact live group once, never select the nearest one
            // or form a cross product of the providers' individual rings.
            const auto duplicate=std::find_if(selected.begin(),selected.end(),[&](const auto *other){
                return old.recording==other->identity->recording&&std::all_of(dependencies.begin(),dependencies.end(),[&](const auto &d){
                    const auto &a=old.components[d.provider].sample,&b=other->identity->components[d.provider].sample;
                    return a.output==b.output&&a.output_offset==b.output_offset&&a.output_bytes==b.output_bytes&&a.order==b.order;
                });
            });
            if(duplicate==selected.end())selected.push_back(&h);
            else if(!(*duplicate)->pack&&h.pack)*duplicate=&h; // same native evidence, preserve an eligible legacy route
        }
        return selected;
    }
    void prepare_alternate_previous(api::command_list *c,const std::shared_ptr<const NativeBlasIdentity> &owner,const Pose &current){
        // The native BLAS is already recorded on this list. A complete CPU
        // pose describes which write current covers even before we own a GPU
        // batch. Only prepare a different exact write in that same epoch.
        const auto &identity=*owner;
        const auto &pack=current.pack;
        if(identity.recording!=inputs.recording(c)||identity.recording!=current.recording)return;
        for(const auto &i:previous_interests){
            if(i.pack!=pack||i.component>=identity.components.size())continue;
            const auto &part=pack->components[i.component];
            if(current.final_positions[i.component].mode==WWMI_RT_POSITION_FINAL)continue;
            const auto &s=identity.components[i.component].sample;
            if(s.profile!=pack->native||s.component!=i.component||s.content!=i.content||s.content_offset!=i.content_offset||
                current.source_orders[i.component]!=s.order)continue;
            const auto offset=i.view_offset+12ull*part.native.first,bytes=12ull*part.native.count;
            const auto latest=inputs.position_receipt(i.output,offset,bytes);
            if(!latest.order||latest.producer!=identity.recording||latest.submissions)continue;
            if(s.output==i.output&&s.output_offset==offset&&s.output_bytes==bytes&&s.order==latest.order&&s.recording==latest.producer)continue;
            BlasPoseInput layout;const bool dependent=!part.dependencies.empty();
            if(dependent){
                // A real dispatch or BLAS link takes priority over any
                // cross-ring candidate for this exact target write.
                const auto covered=[&](const PreviousPrepared &p){
                    return p.route!=PreviousRoute::cohort_candidate&&p.source.pack==pack&&p.source.component==i.component&&
                        p.source.output==i.output&&p.source.view_offset==i.view_offset&&p.source.view_bytes==i.view_bytes&&
                        (!i.author_demand||p.owned_pose)&&(!i.legacy_demand||p.batch)&&
                        !p.proofs.empty()&&p.proofs.front().order==latest.order&&p.proofs.front().recording==latest.producer&&
                        inputs.previous_sources_live(p.proofs,pack,false);
                };
                if(std::any_of(previous_prepared.begin(),previous_prepared.end(),covered))continue;
            }
            const auto prepare=[&](const BlasPoseInput *windows,const std::vector<const SkinSample *> *retained=nullptr,PreviousHistoricalWriter historical={},bool cpu_only=false){
                auto candidate=inputs.assemble_previous_component(c,pack,i.output,i.view_offset,i.component,i.content,i.content_offset,false,windows,retained);
                if(candidate.assembly.result!=PoseResult::complete||!candidate.assembly.pose||candidate.sources.size()!=1+part.dependencies.size())return;
                const auto &proof=candidate.sources.front();
                if(proof.output!=i.output||proof.output_offset!=offset||proof.output_bytes!=bytes||proof.order!=latest.order||proof.recording!=latest.producer)return;
                record_previous(i,candidate,latest.producer,dependent?PreviousRoute::cohort_candidate:PreviousRoute::alternate_write,dependent?owner:nullptr,historical,cpu_only);
            };
            if(!dependent){prepare(nullptr);continue;}
            if(select_previous_candidate_layout(i,latest.producer,layout))prepare(&layout);
            for(const auto *h:retained_previous_providers(i,identity)){
                const auto span=pack->native->bytes();layout={latest.producer,{}};layout.geometries.resize(pack->components.size());
                layout.geometries[i.component]={i.output,i.view_offset,span};
                std::vector<const SkinSample *> retained(pack->components.size());
                for(const auto &d:part.dependencies){const auto &old=h->identity->components[d.provider];
                    layout.geometries[d.provider]=old.vertices;retained[d.provider]=&old.sample;}
                // Bare native evidence is only for the owned-author entry.
                // The actual provider read is still required at consumption.
                prepare(&layout,&retained,{h->identity->recording,h->native_signal},!h->pack);
            }
        }
    }
    std::vector<std::shared_ptr<Target>> current_targets(api::command_list *c){std::vector<std::shared_ptr<Target>> out;const auto epoch=inputs.recording(c);for(const auto &[_,t]:targets)if(gpu_ready(*t->geometry,epoch))out.push_back(t);return out;}
    bool ensure_geometry(api::command_list *c,const NativeDrawIdentityCache::Match &match,const std::shared_ptr<const ModPack> &pack){
        const auto source=match.identity;const auto epoch=inputs.recording(c);auto old=targets.find(source->native_blas);
        auto previous=old==targets.end()?std::shared_ptr<Target>{}:old->second;
        if(source->geometries.size()!=pack->components.size())return false;
        for(unsigned i=0;i<source->components.size();++i)if(!identities.live(source,i)||!inputs.position_live(source->components[i].sample))return false;
        if(previous&&previous->native_identity==source&&previous->geometry->assets->pack==pack&&gpu_ready(*previous->geometry,epoch))return true;
        for(const auto &h:surface_history)if(h.identity==source&&h.pack==pack&&h.batch&&gpu_ready(*h.batch,epoch)){
            auto target=std::make_shared<Target>();target->native_blas=source->native_blas;target->native_resource=source->allocation;target->native_identity=source;target->native_geometry=source->geometries;target->geometry=h.batch;
            targets[target->native_blas]=std::move(target);return true;
        }
        auto pose=inputs.assemble_snapshot(*source,pack);if(pose.result!=PoseResult::complete)return false;
        auto &assets=geometry[pack.get()];if(!assets){rt_cpu::Scope measured(rt_cpu::assets);assets=GeometryAssets::create(native.Get(),pack,skin_dxil,copy_dxil,tangent_dxil);}
        rt_cpu::Scope allocation_measured(rt_cpu::batch_create);auto batch=GeometryBatch::prepare_streams(assets,*pose.pose);allocation_measured.end();
        batch->gpu_recording=epoch;records[epoch.list].push_back(batch);generated_records[epoch.list].push_back(batch);
        rt_cpu::Scope recording_measured(rt_cpu::skin_record);const auto recording_status=batch->record(epoch.list,compute_run);recording_measured.end();
        if(recording_status!=0||!batch->recorded)return false;
        auto target=std::make_shared<Target>();target->native_blas=source->native_blas;target->native_resource=source->allocation;target->native_identity=source;target->native_geometry=source->geometries;target->geometry=batch;
        targets[target->native_blas]=target;
        if(auto *h=history_for(source,pack))h->batch=batch;
        else surface_history.push_back({source,batch,{},pack,{},match.producer,{}});
        ++geometry_count;
        trace.armed.store(true,std::memory_order_release);trace.emit("surface_recorded","private",epoch.list,source->native_blas,batch->positions->GetGPUVirtualAddress());return true;
    }
    bool shape_at_native_producer(api::command_list *c,const std::shared_ptr<const NativeBlasIdentity> &identity){
        if(!shape_position||!identity||identity->components.empty())return false;
        const auto epoch=inputs.recording(c);
        if(epoch!=identity->recording||!inputs.resource_current(identity->allocation))return false;
        const auto &profile=identity->components.front().sample.profile;
        const auto found=std::find_if(catalog.packs.begin(),catalog.packs.end(),[&](const auto &p){return p->native==profile&&p->shapekey_vertices;});
        if(found==catalog.packs.end())return false;
        const auto pack=*found;
        NativeDrawIdentityCache::Match match{};match.identity=identity;
        auto shape=acquire_post_multiplier_shape(shape_position,native.Get(),epoch,match,pack);
        if(!shape||!shape_snapshot_ready(*shape,epoch,native.Get()))return false;
        auto &assets=geometry[pack.get()];if(!assets)assets=GeometryAssets::create(native.Get(),pack,skin_dxil,copy_dxil,tangent_dxil);
        if(shape_dxil.empty())shape_dxil=read_asset(L"ShapePositions.cso");
        if(assets->vectors&&shape_tangent_dxil.empty())shape_tangent_dxil=read_asset(L"ShapeTangents.cso");
        auto prepared=record_shape_positions(inputs,identities,c,match,assets,std::move(shape),shape_dxil,shape_tangent_dxil,
            compute_run,records[epoch.list],generated_records[epoch.list]);
        if(prepared.status!=ShapePositionStatus::recorded)return false;
        auto target=std::make_shared<Target>();target->native_blas=identity->native_blas;target->native_resource=identity->allocation;
        target->native_identity=identity;target->native_geometry=identity->geometries;target->geometry=std::move(prepared.batch);
        surface_history.push_back({identity,target->geometry,{},pack,{},{},{}});
        targets[target->native_blas]=std::move(target);++geometry_count;
        return true;
    }
    void blas(api::command_list *c,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc){
        // The game is overwriting this destination, including when it is no longer the identified game object.
        targets.erase(desc.DestAccelerationStructureData);
        blas_interest.overwritten(desc.DestAccelerationStructureData);
        // An actual native build supersedes every old identity at this AS
        // destination. Resolving its allocation is unnecessary for invalidation;
        // positive identities still require select's exact unique live resource.
        identities.overwritten_blas({},desc.DestAccelerationStructureData);
        if(!inputs.may_select_blas(desc))return;
        std::shared_ptr<const NativeProfile> identified;std::shared_ptr<const NativeBlasIdentity> native_identity;
        InputCollector::SelectedIdentityState id_state{};
        auto pose=inputs.select(c,desc,&identified,&native_identity,&id_state);
        if(native_identity){identities.record_with_state(native_identity,id_state.submissions,id_state.producer);blas_interest.proved(desc.DestAccelerationStructureData,native_identity->allocation);}
        if(identified&&mod_detected)mod_detected(identified->object_guid);
        const bool complete_native_surface=pose.result==PoseResult::complete&&bool(native_identity);
        // A proved native CPU pose can predate the Mod's first evaluated
        // recipe. Keep that original linked writer, not a guessed Mod pose.
        // Actual submit_after gives it a receipt, independently of a GPU batch.
        if(author_path&&native_identity)
            surface_history.push_back({native_identity,{},{},{},{},id_state.submissions==1?id_state.producer:NativeIdentityFence{},{}});
        // Optional, externally written author deltas must be ready at this
        // real producer. Never pull them into an owned prefix before their writer.
        if(pose.result==PoseResult::mod_shapekey_missing&&shape_at_native_producer(c,native_identity))return;
        if(!previous_interests.empty()&&(!complete_native_surface||rt_cpu::phase.load(std::memory_order_relaxed)==1))
            prepare_previous_blas(c,desc,complete_native_surface);
        if(pose.result!=PoseResult::complete){
            if(identified&&missing_inputs.emplace(identified->object_guid,pose.result).second){
                if(pose.result==PoseResult::mod_shapekey_missing)reshade::log::message(reshade::log::level::info,"RT native fallback: this Mod requires its own final ShapeKey positions");
                else if(pose.result==PoseResult::mod_extra_missing)reshade::log::message(reshade::log::level::info,"RT native fallback: the evaluated Mod draw has extra position inputs and requires its real final output");
                else if(pose.result==PoseResult::native_extra_unresolved)reshade::log::message(reshade::log::level::info,"RT native fallback: native extra/delta input is present; target Mod draw has not supplied final positions or verified absence of displacement");
            }return;
        }
        const auto allocation=native_identity?native_identity->allocation:inputs.resource_at(desc.DestAccelerationStructureData);
        if(!allocation||(native_identity&&!inputs.resource_current(allocation)))return;
        if(author_path&&native_identity&&owned_author_pack(pose.pose->pack)&&
           std::none_of(pose.pose->final_positions.begin(),pose.pose->final_positions.end(),[](const auto &p){return p.mode==WWMI_RT_POSITION_FINAL;})){
            auto pack=pose.pose->pack;
            auto &h=author_history_for(native_identity,pack,{});
            if(!h.owned_pose)h.owned_pose=std::move(pose.pose);
            if(!previous_interests.empty())prepare_alternate_previous(c,native_identity,*h.owned_pose);
            return;
        }
        auto &assets=geometry[pose.pose->pack.get()];if(!assets){rt_cpu::Scope measured(rt_cpu::assets);assets=GeometryAssets::create(native.Get(),pose.pose->pack,skin_dxil,copy_dxil,tangent_dxil);}
        rt_cpu::Scope allocation_measured(rt_cpu::batch_create);auto batch=GeometryBatch::prepare_streams(assets,*pose.pose);allocation_measured.end();
        batch->gpu_recording=inputs.recording(c);generated_records[batch->gpu_recording.list].push_back(batch);
        records[reinterpret_cast<std::uint64_t>(c)].push_back(batch);
        rt_cpu::Scope recording_measured(rt_cpu::skin_record);const auto recording_status=batch->record(reinterpret_cast<std::uint64_t>(c),compute_run);recording_measured.end();
        if(recording_status!=0||!batch->recorded)return;
        auto target=std::make_shared<Target>();target->native_blas=desc.DestAccelerationStructureData;target->native_resource=allocation;target->native_identity=native_identity;target->geometry=std::move(batch);
        if(native_identity){
            surface_history.push_back({native_identity,target->geometry,{},pose.pose->pack,{},{},{}});
            if(!previous_interests.empty())prepare_alternate_previous(c,native_identity,*pose.pose);
        }
        target->native_geometry.assign(desc.Inputs.pGeometryDescs,desc.Inputs.pGeometryDescs+desc.Inputs.NumDescs);targets[target->native_blas]=std::move(target);++geometry_count;
        trace.armed.store(true,std::memory_order_release);trace.emit("surface_recorded","private",reinterpret_cast<std::uint64_t>(c),desc.DestAccelerationStructureData,targets[desc.DestAccelerationStructureData]->geometry->positions->GetGPUVirtualAddress());
        if(geometry_count==1)reshade::log::message(reshade::log::level::info,"RT pack: first identified Mod surface recorded (native RT scene)");
    }
    std::uint64_t instances(api::command_list *c,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &desc){
        const auto native_allocation=inputs.resource_at(desc.DestAccelerationStructureData);
        auto &writes=tlas_records[reinterpret_cast<std::uint64_t>(c)];std::size_t write_index=SIZE_MAX;
        const bool owned=tlas.contains(desc.DestAccelerationStructureData)||std::any_of(scene_owners.begin(),scene_owners.end(),[&](const auto &entry){return entry.second.write.address==desc.DestAccelerationStructureData;})||std::any_of(writes.begin(),writes.end(),[&](const auto &w){return w.address==desc.DestAccelerationStructureData&&bool(w.instance);});
        if(owned&&native_allocation){write_index=writes.size();writes.push_back({desc.DestAccelerationStructureData,native_allocation,{}});}
        const auto existing=tlas.find(desc.DestAccelerationStructureData);
        if(existing!=tlas.end())tlas.erase(existing);
        if(targets.empty()||desc.Inputs.DescsLayout!=D3D12_ELEMENTS_LAYOUT_ARRAY||!desc.Inputs.NumDescs)return 0;
        auto allocation=inputs.resource_at(desc.DestAccelerationStructureData);if(!allocation)return 0;
        collect();auto available=current_targets(c);if(available.empty())return 0;
        trace.emit("tlas_prepare","begin",reinterpret_cast<std::uint64_t>(c),desc.Inputs.InstanceDescs,desc.Inputs.NumDescs);
        auto job=InstanceBatch::prepare(patches,desc.Inputs.InstanceDescs,desc.Inputs.NumDescs,std::move(available));
        trace.emit("tlas_prepare","end",reinterpret_cast<std::uint64_t>(c),job->address(),job->targets.size());
        records[reinterpret_cast<std::uint64_t>(c)].push_back(job);
        trace.emit("tlas_record","begin",reinterpret_cast<std::uint64_t>(c),c->get_native(),job->address());
        const auto record_status=job->record(reinterpret_cast<std::uint64_t>(c),compute_run);trace.emit("tlas_record","end",reinterpret_cast<std::uint64_t>(c),record_status,job->recorded);
        if(record_status!=0||!job->recorded)return 0;
        tlas[desc.DestAccelerationStructureData]={allocation,job};
        if(write_index==SIZE_MAX)writes.push_back({desc.DestAccelerationStructureData,allocation,job});else writes[write_index].instance=job;
        ++tlas_count;return job->address();
    }
    std::uint64_t hit_table(api::command_list *c,const D3D12_DISPATCH_RAYS_DESC &desc){
        // The forwarder calls here for the actual ray consumer, even when its
        // SBT/pipeline is unsupported. Hold known TLAS owners before ANY return.
        // InstanceBatch already owns its exact old Targets and GeometryBatches.
        auto &ray_holds=records[reinterpret_cast<std::uint64_t>(c)];
        auto retain=[&](const auto &entries){for(const auto &[_,entry]:entries)if(entry.instance&&std::none_of(ray_holds.begin(),ray_holds.end(),[&](const auto &hold){return hold.get()==entry.instance.get();}))ray_holds.push_back(entry.instance);};
        retain(tlas);
        auto retain_instance=[&](const std::shared_ptr<InstanceBatch> &owner){if(owner&&std::none_of(ray_holds.begin(),ray_holds.end(),[&](const auto &hold){return hold.get()==owner.get();}))ray_holds.push_back(owner);};
        for(const auto &[_,owner]:scene_owners)retain_instance(owner.write.instance);
        for(const auto &owner:retiring_scene_owners)retain_instance(owner.owner.write.instance);
        if(targets.empty()||tlas.empty()||desc.HitGroupTable.StrideInBytes!=128||!desc.HitGroupTable.SizeInBytes)return 0;
        WWMI_ComputeStateInfo_v1 state{};state.size=sizeof(state);
        if(compute_query(reinterpret_cast<std::uint64_t>(c),&state)!=0||state.pipeline_kind!=1)return 0;
        ShaderId id{};if(!materials.identifier(state.pipeline,id))return 0;
        collect();auto available=current_targets(c);if(available.empty())return 0;
        auto job=SbtBatch::prepare(patches,desc.HitGroupTable.StartAddress,desc.HitGroupTable.SizeInBytes,id,std::move(available));
        records[reinterpret_cast<std::uint64_t>(c)].push_back(job);
        if(job->record(reinterpret_cast<std::uint64_t>(c),compute_run)!=0||!job->recorded)return 0;
        ++sbt_count;return job->copy->GetGPUVirtualAddress();
    }
    bool native_draw_candidate(const WWMI_RtPackDrawQuery_v2 &q)const{
        const auto idx_stride=q.index_format==DXGI_FORMAT_R32_UINT?4ull:(q.index_format==DXGI_FORMAT_R16_UINT?2ull:0ull);
        if(!idx_stride||q.index_buffer_gpu_va>UINT64_MAX-idx_stride*q.first_index)return false;
        // Completion can make an existing proof usable, but cannot manufacture
        // a new native component at another VB/IB view. Only a negative result
        // skips collect; all possible matches retain the original observation.
        return identities.might_resolve({{},{},{},q.vertex_buffer_gpu_va,q.vertex_buffer_bytes,
            q.index_buffer_gpu_va+idx_stride*q.first_index,idx_stride*q.index_count,q.index_count});
    }
#include "author_draw.inc"
#include "rt_hybrid_client.inc"
#include "author_work.inc"
#include "author_occlusion.inc"
#include "occlusion_instance_input.inc"
#include "raygen_record_probe.inc"
    std::uint32_t acquire_native_palette(const WWMI_RtPackDrawQuery_v2 &query,unsigned expected_component,WWMI_RtNativePalette_v1 &out,std::uint64_t snapshot=0){
        NativeDrawIdentityCache::Match match;WWMI_RtPackDrawIdentity_v2 proof{};
        if(snapshot){
            const auto *captured=captured_author(query,snapshot);
            if(!captured)return WWMI_RT_PALETTE_NATIVE_UNPROVEN;
            match=captured->match;proof=captured->snapshot.binding;
        }else{
            if(!native_draw_candidate(query))return WWMI_RT_PALETTE_NATIVE_UNPROVEN;
            collect();
            if(!inputs.identify_draw_v2(query,proof,nullptr,0,&match))return WWMI_RT_PALETTE_NATIVE_UNPROVEN;
        }
        if(proof.native.component!=expected_component)return WWMI_RT_PALETTE_COMPONENT_MISMATCH;
        const auto &sample=match.identity->components[match.component].sample;
        if(sample.palette.empty()||sample.palette.size()>512||sample.palette_bytes!=sample.palette.size()*sizeof(Matrix)||proof.native.palette_bytes!=sample.palette_bytes)
            return WWMI_RT_PALETTE_NOT_READY;
        const auto consumer=inputs.recording(reinterpret_cast<api::command_list *>(query.list));
        if(!consumer.list||!consumer.creation||!consumer.generation)return WWMI_RT_PALETTE_NATIVE_UNPROVEN;
        auto &cache=native_palettes[{reinterpret_cast<std::uint64_t>(match.identity.get()),match.component}];
        cache.identity=match.identity;cache.component=match.component;
        std::shared_ptr<NativePaletteBatch> palette;InputCollector::BorrowedPalette borrowed;
        if(!snapshot&&match.ordering==WWMI_RT_PACK_SAME_RECORDING&&inputs.borrow_palette(sample,consumer,borrowed)){
            palette=cache.native.lock();
            if(!palette||palette->resource.Get()!=borrowed.resource.Get()||palette->creation!=borrowed.key.creation||palette->offset!=borrowed.offset){
                auto batch=std::make_shared<NativePaletteBatch>();batch->resource=std::move(borrowed.resource);batch->creation=borrowed.key.creation;
                batch->offset=borrowed.offset;batch->bytes=borrowed.bytes;batch->cbv_bytes=borrowed.cbv_bytes;batch->resource_bytes=borrowed.resource_bytes;batch->kind=WWMI_RT_PALETTE_NATIVE_UPLOAD;
                palette=std::move(batch);cache.native=palette;
            }
        }else{
            if(!cache.owned){
                auto batch=std::make_shared<NativePaletteBatch>();batch->bytes=sample.palette_bytes;batch->cbv_bytes=(batch->bytes+255)&~255ull;batch->resource_bytes=batch->cbv_bytes;
                batch->resource=gpu::buffer(native.Get(),batch->resource_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
                std::vector<unsigned char> bytes(size_t(batch->resource_bytes),0);std::memcpy(bytes.data(),sample.palette.data(),size_t(batch->bytes));gpu::upload(batch->resource.Get(),bytes.data(),bytes.size());
                batch->creation=++palette_creation;batch->kind=WWMI_RT_PALETTE_OWNED_UPLOAD;cache.owned=std::move(batch);
            }
            palette=cache.owned;
        }
        if(!palette||!palette->resource||!palette->resource->GetGPUVirtualAddress())return WWMI_RT_PALETTE_NOT_READY;
        // Commit ownership before the caller can publish a virtual CB or emit
        // its consuming commands. Existing actual-submit/fence paths retire it.
        auto &holds=records[consumer.list];
        if(std::none_of(holds.begin(),holds.end(),[&](const auto &hold){return hold.get()==palette.get();}))holds.push_back(palette);
        out={};out.struct_size=sizeof(out);out.abi_version=1;out.source_kind=palette->kind;out.resource_state=D3D12_RESOURCE_STATE_GENERIC_READ;
        out.resource=reinterpret_cast<std::uint64_t>(palette->resource.Get());out.resource_creation=palette->creation;out.resource_offset=palette->offset;
        out.gpu_va=palette->resource->GetGPUVirtualAddress()+palette->offset;out.bytes=palette->bytes;out.cbv_bytes=palette->cbv_bytes;out.resource_bytes=palette->resource_bytes;
        out.matrix_count=unsigned(sample.palette.size());out.matrix_stride=sizeof(Matrix);out.float4_count=out.matrix_count*3;out.provided_inputs=WWMI_RT_PALETTE_CURRENT_POSE;
        out.consumer_list=consumer.list;out.consumer_list_creation=consumer.creation;out.consumer_recording=consumer.generation;
        out.native_blas_resource=match.identity->allocation.object;out.native_blas_creation=match.identity->allocation.creation;out.native_blas_gpu_va=match.identity->native_blas;out.pose_order=sample.order;
        out.source_resource=sample.palette_resource.object;out.source_creation=sample.palette_resource.creation;out.source_offset=sample.palette_offset;out.source_bytes=sample.palette_bytes;out.native_identity=proof;
        return WWMI_RT_PALETTE_OK;
    }
    static std::uint32_t selected_index_view(const GeometryBatch &batch,unsigned component,const WWMI_RtModDraw_v1 &request,
        WWMI_RtAuthorIndexView_v3 &out){
        const auto &pack=*batch.assets->pack;
        const auto &draws=pack.components[component].draws;
        const auto draw=std::find_if(draws.begin(),draws.end(),[&](const auto &d){
            return d.source_first==request.first_index&&d.count==request.index_count&&d.source_base==request.base_vertex;
        });
        if(draw==draws.end())return WWMI_RT_GEOMETRY_DRAW_NOT_SELECTED;
        const auto offset=4ull*draw->packed_first,bytes=4ull*draw->count;
        if(!bytes||bytes>UINT32_MAX||offset>pack.indices.size()||bytes>pack.indices.size()-offset)
            return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
        out={batch.assets->indices->GetGPUVirtualAddress()+offset,offset,bytes,draw->count};
        return WWMI_RT_GEOMETRY_OK;
    }
    std::uint32_t acquire_graphics_geometry(const WWMI_RtGraphicsGeometryRequest_v1 &request,WWMI_RtGraphicsGeometry_v1 &out,std::uint64_t plan_frame=0,std::uint32_t path_flags=0,std::shared_ptr<GeometryBatch> *selected=nullptr,NativeDrawIdentityCache::Match *selected_native=nullptr,const WWMI_RtDrawSource_v1 *expected=nullptr,
        const WWMI_RtAuthorFrameRequest_v1 *author=nullptr,WWMI_RtAuthorFrame_v1 *author_out=nullptr,
        const WWMI_RtDeformedSelection_v1 *deformed=nullptr,const WWMI_RtModDraw_v1 *section_draws=nullptr){
        // Compare only this live section against the frozen plan. A revision
        // advanced by another Primitive is not a mismatch of this recipe.
        AuthorPlan *plan=author?find_author_plan(author->plan):nullptr;
        struct CloseCompletedSourceOnLegacy {
            AuthorPlan *plan;
            WWMI_RtAuthorFrame_v1 *out;
            ~CloseCompletedSourceOnLegacy(){
                if(plan&&out&&out->disposition==WWMI_RT_AUTHOR_LEGACY)plan->completed_source_closed=true;
            }
        } close_completed_source{plan,author_out};
        if(author&&(!plan||plan->requires_source!=bool(deformed)))return WWMI_RT_GEOMETRY_NATIVE_UNPROVEN;
        WWMI_RtModSectionIdentity_v1 section{};
        if(plan_frame){
            WWMI_RtModGeometrySection_v1 planned{};
            WWMI_RtModDraw_v1 subdraw{};
            WWMI_RtModAuthorRecipe_v1 expected_recipe{};
            std::array<WWMI_RtModDraw_v1,4> local_recipe;
            std::vector<WWMI_RtModDraw_v1> longer_recipe;
            if(plan&&plan->pack&&request.component<plan->pack->components.size()){
                const auto &part=plan->pack->components[request.component];
                auto *draws=local_recipe.data();
                if(part.draws.size()>local_recipe.size()){longer_recipe.resize(part.draws.size());draws=longer_recipe.data();}
                for(size_t i=0;i<part.draws.size();++i){const auto &d=part.draws[i];draws[i]={d.count,d.source_first,d.source_base};}
                expected_recipe={draws,uint32_t(part.draws.size()),WWMI_RT_COMPONENT_SELECTION_KNOWN|WWMI_RT_GEOMETRY_SELECTION_PLANNED|WWMI_RT_GEOMETRY_MOD_AUTHOR_SHARED};
            }
            const auto read_status=author?
                (mod_author_section?mod_author_section(request.section_token,plan_frame,&planned,sizeof(planned),author->subdraw_count,author->subdraw_index,
                    &subdraw,expected_recipe.draws?&expected_recipe:nullptr):WWMI_RT_MOD_NOT_READY):
                (mod_geometry_section?mod_geometry_section(request.section_token,plan_frame,&planned,sizeof(planned)):WWMI_RT_MOD_NOT_READY);
            if(read_status!=WWMI_RT_MOD_OK||
                planned.struct_size!=sizeof(planned)||planned.abi_version!=1||planned.plan_frame!=plan_frame||
                !(planned.selection_flags&WWMI_RT_GEOMETRY_SELECTION_PLANNED))
                return read_status==WWMI_RT_MOD_STALE&&expected_recipe.draws?WWMI_RT_GEOMETRY_STALE_SELECTION:WWMI_RT_GEOMETRY_BATCH_NOT_READY;
            if(author&&(subdraw.index_count!=request.mod_index_count||subdraw.first_index!=request.mod_first_index||subdraw.base_vertex!=request.mod_base_vertex))
                return WWMI_RT_GEOMETRY_DRAW_NOT_SELECTED;
            if(section_draws){
                const auto same_draw=[](const auto &a,const auto &b){
                    return a.index_count==b.index_count&&a.first_index==b.first_index&&a.base_vertex==b.base_vertex;
                };
                if(!same_draw(subdraw,section_draws[0]))return WWMI_RT_GEOMETRY_DRAW_NOT_SELECTED;
                // The producer retains actual issued order, including repeated
                // equal index ranges. Verify every ordinal against the same
                // complete recipe before acquiring the shared pose and cohort.
                for(unsigned i=1;i<author->subdraw_count;++i){
                    WWMI_RtModGeometrySection_v1 same{};
                    if(mod_author_section(request.section_token,plan_frame,&same,sizeof(same),author->subdraw_count,i,&subdraw,
                        expected_recipe.draws?&expected_recipe:nullptr)!=WWMI_RT_MOD_OK||
                        same.struct_size!=planned.struct_size||same.abi_version!=planned.abi_version||same.plan_frame!=planned.plan_frame||
                        same.selection_flags!=planned.selection_flags||same.load_generation!=planned.load_generation||
                        same.mod_key!=planned.mod_key||same.mod_revision!=planned.mod_revision||same.component!=planned.component||
                        !same_draw(subdraw,section_draws[i]))return WWMI_RT_GEOMETRY_STALE_SELECTION;
                }
            }
            section.struct_size=sizeof(section);section.abi_version=1;section.load_generation=planned.load_generation;
            section.mod_key=planned.mod_key;section.mod_revision=planned.mod_revision;section.component=planned.component;section.selection_known=1;
        }else if(!mod_section_identity||mod_section_identity(request.section_token,&section,sizeof(section))!=WWMI_RT_MOD_OK||!section.selection_known)
            return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
        if(section.struct_size!=sizeof(section)||section.abi_version!=1||section.load_generation!=request.load_generation||section.mod_key!=request.mod_key||
            section.mod_revision!=request.mod_revision||section.component<0||unsigned(section.component)!=request.component)return WWMI_RT_GEOMETRY_STALE_SELECTION;
        NativeDrawIdentityCache::Match native_match;WWMI_RtPackDrawIdentity_v2 native_identity{};
        WWMI_RtAuthorSnapshot_v1 snapshot{};
        AuthorCapture *captured=nullptr;
        if(author){
            captured=captured_author(request.native_draw,author->draw_ticket);
            if(!captured)return WWMI_RT_GEOMETRY_NATIVE_UNPROVEN;
            snapshot=captured->snapshot;native_match=captured->match;
            native_identity=snapshot.binding;
        }else{
            if(!native_draw_candidate(request.native_draw))return WWMI_RT_GEOMETRY_NATIVE_UNPROVEN;
            collect();
            if(!inputs.identify_draw_v2(request.native_draw,native_identity,nullptr,0,&native_match))return WWMI_RT_GEOMETRY_NATIVE_UNPROVEN;
        }
        if(native_identity.native.component!=request.component)return WWMI_RT_GEOMETRY_NATIVE_UNPROVEN;
        auto *list=reinterpret_cast<api::command_list *>(request.native_draw.list);
        const auto consumer=inputs.recording(list);
        if(!consumer.list||!consumer.creation||!consumer.generation)return WWMI_RT_GEOMETRY_NATIVE_UNPROVEN;
        if(expected){
            const auto &id=*native_match.identity;
            const auto &sample=id.components[native_match.component].sample;
            if(consumer.creation!=expected->list_creation||consumer.generation!=expected->recording||
               id.allocation.object!=expected->blas_resource||id.allocation.creation!=expected->blas_creation||
               id.native_blas!=expected->blas_va||sample.order!=expected->pose_order)return WWMI_RT_GEOMETRY_NATIVE_UNPROVEN;
            if(auto bound=draw_recording(consumer.list)){
                const auto reset=bound->reset.load(std::memory_order_acquire);
                if(reset==DrawBindingRecording::waiting||reset==DrawBindingRecording::succeeded)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
            }
        }
        if(author){
            if(plan->snapshot!=snapshot.token||plan->identity!=native_match.identity||plan->load!=request.load_generation||
               plan->mod!=request.mod_key||!inputs.resource_current(plan->primitive)||!hold_draw_recording(consumer,list))
                return WWMI_RT_GEOMETRY_NATIVE_UNPROVEN;
            author_out->plan=plan->id;author_out->disposition=plan->disposition;
            if(plan->disposition==WWMI_RT_AUTHOR_NATIVE)return WWMI_RT_GEOMETRY_STALE_SELECTION;
        }
        auto find_current=[&](){return std::find_if(catalog.packs.begin(),catalog.packs.end(),[&](const auto &p){return p->key==request.mod_key&&p->load_generation==request.load_generation;});};
        std::shared_ptr<const ModPack> pack;
        if(plan&&plan->pack)pack=plan->pack;
        else{
            auto current=find_current();
            if(current==catalog.packs.end()||(*current)->revision!=request.mod_revision){refresh();current=find_current();}
            if(current==catalog.packs.end()||(*current)->revision!=request.mod_revision)return WWMI_RT_GEOMETRY_STALE_SELECTION;
            pack=*current;
        }
        std::shared_ptr<GeometryBatch> batch;
        if(plan){
            const bool completed=deformed&&(deformed->kind==WWMI_RT_DEFORMED_AUTHOR_FLOAT6||deformed->kind==WWMI_RT_DEFORMED_FINAL_PAIR);
            if(completed&&plan->completed_source_closed)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
            const bool frozen_completed=plan->pose&&
                std::any_of(plan->pose->final_positions.begin(),plan->pose->final_positions.end(),[](const auto &p){return bool(p.completed_author);});
            const bool completed_empty=deformed&&!author->subdraw_count&&plan->disposition==WWMI_RT_AUTHOR_PRIVATE&&frozen_completed;
            if(frozen_completed&&!completed&&!completed_empty)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
            if((plan->disposition==WWMI_RT_AUTHOR_LEGACY||(plan->disposition==WWMI_RT_AUTHOR_TENTATIVE&&(pack->shapekey_vertices||
               std::any_of(pack->components.begin(),pack->components.end(),[](const auto &p){return p.extra_position_input;}))))&&!completed){
                author_out->disposition=WWMI_RT_AUTHOR_LEGACY;return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
            }
            if(!((completed||completed_empty)?planned_shared_author_pack(pack):owned_author_pack(pack))||!(path_flags&WWMI_RT_PLAN_MOD_AUTHOR_SHARED)||(path_flags&WWMI_RT_PLAN_EXTRA_POSITION_INPUT)||
               request.component>=pack->components.size()||native_match.identity->components.size()!=pack->components.size())
                return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
            std::shared_ptr<const Pose> completed_pose;
            const auto &part=pack->components[request.component];
            if((author->subdraw_count==0)!=part.draws.empty())return WWMI_RT_GEOMETRY_DRAW_NOT_SELECTED;
            if(deformed){
                if(!plan->pack)return WWMI_RT_GEOMETRY_NATIVE_UNPROVEN;
                if(!author->subdraw_count){
                    // This complete, selected empty component consumes no
                    // vertex source and does not imply AUTHOR_REST. Still,
                    // prove pose assembly before freezing PRIVATE so an empty
                    // hide component cannot strand sibling components in
                    // plan-violation (disappearing body parts).
                    if(deformed->kind||deformed->recipe_receipt)return WWMI_RT_GEOMETRY_STALE_SELECTION;
                    if(!frozen_completed){
                        auto &h=author_history_for(native_match.identity,pack,native_match.producer,0);
                        if(!h.owned_pose&&plan->pose)h.owned_pose=plan->pose;
                        if(!h.owned_pose){
                            auto pose=inputs.assemble_snapshot(*native_match.identity,pack);
                            if(pose.result!=PoseResult::complete||!pose.pose)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
                            h.owned_pose=std::move(pose.pose);
                        }
                        plan->pack=pack;plan->pose=h.owned_pose;
                    }
                    captured->source=deformed_owner(*plan,native_match.component);
                    if(selected_native)*selected_native=native_match;
                    return WWMI_RT_GEOMETRY_OK;
                }
                if(completed){
                    completed_pose=acquire_completed_author_pose(*plan,native_match,consumer,*deformed);
                    if(!completed_pose)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
                }else if(!acquire_author_rest(*plan,native_match,consumer,*deformed))return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
                captured->source=*deformed;
            }
            auto &h=author_history_for(native_match.identity,pack,native_match.producer,completed_pose?plan->id:0);
            if(completed_pose){
                // acquire_completed_author_pose verifies the exact selection
                // against an already frozen scope. A different scope owns a
                // separate history; the native/REST entry is untouched.
                if(h.owned_pose&&h.owned_pose!=completed_pose)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
                if(!h.owned_pose)h.owned_pose=std::move(completed_pose);
                if(plan->disposition==WWMI_RT_AUTHOR_LEGACY)plan->disposition=WWMI_RT_AUTHOR_TENTATIVE;
                author_out->disposition=plan->disposition;
            }
            if(!h.owned_pose&&plan->pose)h.owned_pose=plan->pose;
            if(!h.owned_pose){
                auto pose=inputs.assemble_snapshot(*native_match.identity,pack);
                if(pose.result!=PoseResult::complete||!pose.pose)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
                h.owned_pose=std::move(pose.pose);
            }
            plan->pack=pack;plan->pose=h.owned_pose;
            if(!author->subdraw_count){if(selected_native)*selected_native=native_match;return WWMI_RT_GEOMETRY_OK;}
            batch=prepare_author_batch(h,consumer,list);
        }else{
        if(plan_frame){
            if(native_match.component>=pack->components.size())return WWMI_RT_GEOMETRY_NATIVE_UNPROVEN;
            const auto &part=pack->components[native_match.component];
            if(part.mod_author_shared!=bool(path_flags&WWMI_RT_PLAN_MOD_AUTHOR_SHARED))return WWMI_RT_GEOMETRY_STALE_SELECTION;
            if(std::none_of(part.draws.begin(),part.draws.end(),[&](const auto &d){return d.source_first==request.mod_first_index&&d.count==request.mod_index_count&&d.source_base==request.mod_base_vertex;}))return WWMI_RT_GEOMETRY_DRAW_NOT_SELECTED;
            if(path_flags&WWMI_RT_PLAN_EXTRA_POSITION_INPUT){
                const auto existing=targets.find(native_match.identity->native_blas);
                if(existing==targets.end()||existing->second->native_identity!=native_match.identity||existing->second->geometry->assets->pack!=pack||
                    native_match.component>=existing->second->geometry->final_positions.size()||existing->second->geometry->final_positions[native_match.component].mode!=WWMI_RT_POSITION_FINAL)
                    return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
            }
            if(!ensure_geometry(list,native_match,pack))return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
        }
        const auto target=targets.find(native_match.identity->native_blas);
        if(target==targets.end()||target->second->native_resource!=native_match.identity->allocation||target->second->native_identity!=native_match.identity)
            return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
        batch=target->second->geometry;
        }
        if(!batch||(!plan&&!gpu_ready(*batch,consumer))||batch->assets->pack!=pack||batch->producer.recording!=native_match.identity->recording||
            batch->producer.geometries.size()!=native_match.identity->components.size()||batch->source_orders.size()!=native_match.identity->components.size())
            return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
        for(unsigned i=0;i<batch->producer.geometries.size();++i){const auto &source=native_match.identity->components[i];const auto &produced=batch->producer.geometries[i];
            if(produced.vertex!=source.vertices.vertex||produced.offset!=source.vertices.offset||produced.bytes!=source.vertices.bytes||batch->source_orders[i]!=source.sample.order)
                return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
        }
        const unsigned component=native_match.component;
        if(component>=batch->assets->pack->components.size()||batch->assets->pack->native!=native_match.identity->components[component].sample.profile)
            return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
        const auto &part=batch->assets->pack->components[component];
        // The old entry point never opts its caller into the new author policy.
        if(!plan_frame&&part.selection_planned)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
        if(plan_frame){
            if(part.mod_author_shared!=bool(path_flags&WWMI_RT_PLAN_MOD_AUTHOR_SHARED))return WWMI_RT_GEOMETRY_STALE_SELECTION;
            if((path_flags&WWMI_RT_PLAN_EXTRA_POSITION_INPUT)&&(component>=batch->final_positions.size()||batch->final_positions[component].mode!=WWMI_RT_POSITION_FINAL))
                return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
        }
        WWMI_RtAuthorIndexView_v3 index{};
        const auto index_status=selected_index_view(*batch,component,{request.mod_index_count,request.mod_first_index,request.mod_base_vertex},index);
        if(index_status!=WWMI_RT_GEOMETRY_OK)return index_status;
        const auto position_bytes=12ull*batch->assets->pack->vertices;
        if(!position_bytes||position_bytes>UINT32_MAX)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
        // This is the lease commit, before main can issue a consuming command.
        // The existing Reset/pre-submit/native-submit/fence path owns release.
        auto &holds=records[consumer.list];
        if(expected&&!draw_recording(consumer.list)){
            auto binding=std::make_shared<DrawBindingRecording>();binding->epoch=consumer;binding->native_list=list->get_native();
            holds.push_back(binding);binding_recordings[consumer.list]=binding;
        }
        if(std::none_of(holds.begin(),holds.end(),[&](const auto &hold){return hold.get()==batch.get();}))holds.push_back(batch);
        out={};out.struct_size=sizeof(out);out.abi_version=1;
        out.load_generation=request.load_generation;out.mod_key=request.mod_key;out.mod_revision=plan?pack->revision:request.mod_revision;out.batch_token=reinterpret_cast<std::uint64_t>(batch.get());
        out.consumer_list=consumer.list;out.consumer_list_creation=consumer.creation;out.consumer_recording=consumer.generation;
        out.native_blas_resource=native_match.identity->allocation.object;out.native_blas_creation=native_match.identity->allocation.creation;out.native_blas_gpu_va=native_match.identity->native_blas;
        out.position_resource=reinterpret_cast<std::uint64_t>(batch->positions.Get());out.position_gpu_va=batch->positions->GetGPUVirtualAddress();out.position_offset=0;out.position_bytes=position_bytes;
        out.position_stride=12;out.position_format=DXGI_FORMAT_R32G32B32_FLOAT;out.position_state=position_read_state;
        out.index_resource=reinterpret_cast<std::uint64_t>(batch->assets->indices.Get());out.index_gpu_va=index.gpu_va;out.index_offset=index.offset;out.index_bytes=index.bytes;
        out.index_format=DXGI_FORMAT_R32_UINT;out.index_count=index.count;out.first_index=0;out.base_vertex=0;out.native_identity=native_identity;
        if(selected)*selected=batch;if(selected_native)*selected_native=native_match;
        return WWMI_RT_GEOMETRY_OK;
    }
    static WWMI_RtLocalVfView_v1 surface_view(ID3D12Resource *resource,std::uint64_t bytes,unsigned format,unsigned element_stride,unsigned vertex_stride,unsigned state){
        WWMI_RtLocalVfView_v1 v{};if(!resource||!bytes)return v;
        v.resource=reinterpret_cast<std::uint64_t>(resource);v.gpu_va=resource->GetGPUVirtualAddress();v.bytes=bytes;v.format=format;v.element_stride=element_stride;
        v.element_count=unsigned(bytes/element_stride);v.vertex_stride=vertex_stride;v.resource_state=state;return v;
    }
    std::uint32_t acquire_surface(const WWMI_RtLocalVfRequest_v1 &request,WWMI_RtLocalVfSurface_v1 &out,std::shared_ptr<GeometryBatch> *selected=nullptr,NativeDrawIdentityCache::Match *selected_native=nullptr,const WWMI_RtDrawSource_v1 *expected=nullptr,std::uint32_t *geometry_status=nullptr,
        const WWMI_RtAuthorFrameRequest_v1 *author=nullptr,WWMI_RtAuthorFrame_v1 *author_out=nullptr,
        const WWMI_RtDeformedSelection_v1 *deformed=nullptr,const WWMI_RtModDraw_v1 *section_draws=nullptr){
        std::shared_ptr<GeometryBatch> batch;NativeDrawIdentityCache::Match source;const auto &p=request.planned;
        const auto status=acquire_graphics_geometry(p.draw,out.geometry,p.frame_id,p.path_flags,&batch,&source,expected,author,author_out,deformed,section_draws);
        if(geometry_status)*geometry_status=status;
        if(status!=WWMI_RT_GEOMETRY_OK)return WWMI_RT_SURFACE_NOT_READY;
        const auto &assets=*batch->assets;const auto &pack=*assets.pack;
        if(!assets.uv||!pack.uv_pairs)return WWMI_RT_SURFACE_UV_MISSING;
        if(!batch->tangents||source.component>=batch->tangent_ready.size()||!batch->tangent_ready[source.component])return WWMI_RT_SURFACE_TANGENTS_MISSING;
        if((request.flags&WWMI_RT_SURFACE_NEED_COLOR)&&!assets.color)return WWMI_RT_SURFACE_COLOR_MISSING;
        out.struct_size=sizeof(out);out.abi_version=1;out.vertex_count=pack.vertices;out.uv_pairs=pack.uv_pairs;
        out.uv=surface_view(assets.uv.Get(),pack.uv.size(),DXGI_FORMAT_R16G16_FLOAT,4,4*pack.uv_pairs,D3D12_RESOURCE_STATE_GENERIC_READ);
        out.tangents=surface_view(batch->tangents.Get(),32ull*pack.vertices,DXGI_FORMAT_R32G32B32A32_FLOAT,16,32,position_read_state);
        out.color=surface_view(assets.color.Get(),pack.color.size(),DXGI_FORMAT_R8G8B8A8_UNORM,4,4,D3D12_RESOURCE_STATE_GENERIC_READ);
        const auto &completed=batch->final_positions[source.component].completed_author;
        if(auto *h=history_for(source.identity,batch->assets->pack,completed?completed->source().selection.plan:0)){
            if(request.current_frame>h->frames[0]){h->frames[1]=h->frames[0];h->frames[0]=request.current_frame;}
            else if(request.current_frame!=h->frames[0]&&request.current_frame>h->frames[1])h->frames[1]=request.current_frame;
        }
        if(selected)*selected=batch;if(selected_native)*selected_native=source;return WWMI_RT_SURFACE_OK;
    }
    std::uint32_t acquire_previous(const WWMI_RtPreviousSurfaceRequest_v1 &request,WWMI_RtPreviousSurface_v1 &out){
        WWMI_RtLocalVfSurface_v1 current{};std::shared_ptr<GeometryBatch> batch;NativeDrawIdentityCache::Match source;
        const auto status=acquire_surface(request.current,current,&batch,&source);if(status!=WWMI_RT_SURFACE_OK)return status;
        return previous_for(request,current,batch,source,out);
    }
    bool record_previous_reject(unsigned object,unsigned component,unsigned result,const char *srv_reject,std::uint64_t frame,bool &armed_window,
        std::uint64_t &sequence){
        auto slot=std::size_t(result);
        if(srv_reject)for(unsigned i=0;i<std::size(previous_srv_rejects);++i)if(!std::strcmp(srv_reject,previous_srv_rejects[i])){
            slot=std::size(previous_result_names)+i;break;
        }
        auto &row=previous_reject_counts[{object,component}][slot];
        const bool first=row.count++==0;row.last_frame=frame;
        if(first){row.reason=previous_result_names[result];row.srv_reject=srv_reject;row.first_frame=frame;}
        bool started;
        if(draw_window){
            const auto deadline=draw_window(native_device_id,&sequence);
            started=deadline!=0;
            // A draw admitted before the deadline can still be returning
            // from its VM while Present drains the same sequence.
            armed_window=sequence||(started&&GetTickCount64()<deadline);
        }else{
            const auto phase=rt_cpu::phase.load(std::memory_order_acquire);
            started=phase!=0;
            armed_window=phase==1&&rt_cpu::now()<rt_cpu::deadline.load(std::memory_order_acquire);
        }
        if(armed_window)return row.armed++==0;
        if(started)++row.after_window;
        return first;
    }
    static void note_previous_provider(DrawBindingRecording &record,const PreviousDrawScope &scope,
        const std::shared_ptr<const NativeBlasIdentity> &owner,const std::shared_ptr<const ModPack> &pack,unsigned component,PreviousReadUse use){
        const auto found=std::find_if(record.previous_reads.begin(),record.previous_reads.end(),[&](const auto &r){
            return r.owner.lock()==owner&&r.pack.lock()==pack&&r.component==component&&r.scope.same_view_primitive(scope);
        });
        if(found==record.previous_reads.end())record.previous_reads.push_back({owner,pack,scope,component,use});
        else if(!(found->use==use))found->conflict=true;
        else found->use.write.submissions=use.write.submissions;
        auto &pending=record.pending_previous;
        if(pending.failed_seq&&pending.owner.lock()==owner&&pending.pack.lock()==pack&&pending.scope.same_view_primitive(scope)&&
            std::any_of(pending.expected.begin(),pending.expected.end(),[&](const auto &e){return e.component==component&&e.use==use;})){
            // Follow the already-reported failure once, in its existing arm.
            // This is a later native selection, not a replaced draw receipt.
            char line[270];std::snprintf(line,sizeof(line),"RT-PREV-LINK-AFTER seq=%llu stage=cohort_provider_after_reject target_order=%llu provider_component=%u provider_order=%llu",
                (unsigned long long)pending.failed_seq,(unsigned long long)pending.target_order,component,(unsigned long long)use.write.order);
            reshade::log::message(reshade::log::level::info,line);
            pending={};
        }
    }
    static CohortCheck previous_cohort_read(const PreviousPrepared &p,const DrawBindingRecording *record,const PreviousDrawScope &scope,
        const std::shared_ptr<const NativeBlasIdentity> &owner,const std::shared_ptr<const ModPack> &pack){
        if(!record)return {CohortBlock::draw_scope_missing};
        if(p.cohort.lock()!=owner)return {CohortBlock::other_current_identity};
        for(const auto &proof:p.proofs){
            if(proof.component==p.source.component)continue;
            bool accepted=false,other_scope=false;
            for(const auto &r:record->previous_reads){
                if(r.owner.lock()!=owner||r.pack.lock()!=pack||r.component!=proof.component)continue;
                if(!r.scope.same_view_primitive(scope)){other_scope|=!r.conflict&&r.use.matches(proof);continue;}
                if(r.conflict)return {CohortBlock::provider_conflict,&proof};
                if(!r.use.matches(proof))return {CohortBlock::provider_other_write,&proof};
                accepted=true;break;
            }
            if(!accepted)return {other_scope?CohortBlock::provider_other_scope:CohortBlock::provider_unseen_at_draw,&proof};
        }
        return {CohortBlock::none};
    }
    std::uint32_t previous_for(const WWMI_RtPreviousSurfaceRequest_v1 &request,const WWMI_RtLocalVfSurface_v1 &current,
        const std::shared_ptr<GeometryBatch> &batch,const NativeDrawIdentityCache::Match &source,WWMI_RtPreviousSurface_v1 &out,bool draw_frame=false,
        const WWMI_RtPreviousDrawScope_v1 *native_scope=nullptr,std::uint64_t *author_previous=nullptr,
        const WWMI_RtAuthorPreviousUse_v1 *author_use=nullptr,AuthorSectionLease *lease=nullptr,
        const AuthorPlan *current_scope=nullptr){
        const auto &v=request.native_previous;ResourceKey native_view;
        const auto &now=source.identity->components[source.component];
        const auto current_frame=request.current.current_frame;
        const auto &native_component=now.sample.profile->components[now.sample.component];
        const auto first=12ull*native_component.first,count=12ull*native_component.count;
        const auto &pack=batch->assets->pack;const auto &part=pack->components[source.component];
        PreviousDrawScope scope;
        std::shared_ptr<DrawBindingRecording> scoped_record;
        bool provider=false;
        if(native_scope&&native_scope->view_resource&&native_scope->primitive_resource&&
            ((native_scope->primitive_words[12]&1u)|native_scope->view_words[19])){
            // The author entry already noted the pre-VM read at BeginScope.
            provider=!author_use&&std::any_of(pack->components.begin(),pack->components.end(),[&](const auto &p){
                return std::any_of(p.dependencies.begin(),p.dependencies.end(),[&](const auto &d){return d.provider==source.component;});
            });
            if((provider||!part.dependencies.empty())&&inputs.previous_cb_keys(native_scope->view_resource,native_scope->view_byte_origin,
                native_scope->primitive_resource,native_scope->primitive_byte_origin,scope.view,scope.primitive)){
                const auto &g=current.geometry;scoped_record=draw_recording(g.consumer_list);
                if(scoped_record&&scoped_record->epoch!=Recording{g.consumer_list,g.consumer_list_creation,g.consumer_recording})scoped_record.reset();
                scope.native=*native_scope;
            }
        }
        const auto observe=[&](PreviousReadUse use={}){
            if(provider&&scoped_record)note_previous_provider(*scoped_record,scope,source.identity,pack,source.component,use);
        };
        InputCollector::PositionReceipt latest;
        const char *ring_stage=nullptr;
        auto ring_pose=PoseResult::missing_producer;
        unsigned ring_missing=UINT32_MAX;
        const auto exact_prepared=[&](const PreviousPrepared &p)->const SkinSample*{
            const auto &i=p.source;
            if(!latest.order||i.pack!=pack||i.output!=native_view||i.view_offset!=v.byte_offset||i.view_bytes!=v.bytes||
                i.component!=source.component||i.content!=now.sample.content||i.content_offset!=now.sample.content_offset)return nullptr;
            const auto actual=std::find_if(p.proofs.begin(),p.proofs.end(),[&](const auto &proof){
                return proof.component==i.component&&proof.output==native_view&&proof.output_offset==v.byte_offset+first&&
                    proof.output_bytes==count&&proof.order==latest.order&&proof.recording==latest.producer;
            });
            return actual==p.proofs.end()?nullptr:&*actual;
        };
        enum Reject {selection,component,source_layout,source_resource,vertex_origin,view_extent,native_written,native_submission,private_incomplete,batch_provenance,ambiguous,reason_count};
        constexpr unsigned binding_missing=reason_count+4;
        const auto &names=previous_result_names;
        std::array<unsigned,reason_count> rejected{};int furthest=-1;
        struct Debug {size_t index=SIZE_MAX;unsigned reason=0,score=0;};std::array<Debug,2> debug{};
        auto remember=[&](const SurfaceHistory &h,unsigned reason){
            if(source.component>=h.identity->components.size())return;
            const auto &s=h.identity->components[source.component].sample;
            unsigned score=(s.output==native_view)?4:0;if(latest.order&&s.order==latest.order&&s.recording==latest.producer)score+=8;
            const auto index=static_cast<size_t>(&h-surface_history.data());
            if(debug[0].index==SIZE_MAX||score>debug[0].score){debug[1]=debug[0];debug[0]={index,reason,score};}
            else if(debug[1].index==SIZE_MAX||score>debug[1].score)debug[1]={index,reason,score};
        };
        auto report=[&](unsigned result,const char *srv_reject=nullptr){
            const auto &s=now.sample;
            bool armed_window=false;
            std::uint64_t sequence=0;
            if(draw_frame&&!record_previous_reject(s.profile->object_guid,s.component,result,srv_reject,current_frame,armed_window,sequence))return;
            // In the combined draw entry the cloth sequence joins the first
            // receipt to main's observation; it does not bypass aggregation.
            const auto cloth_seq=cloth_previous_trace_seq?cloth_previous_trace_seq():0;
            if(!draw_frame&&!cloth_seq)return;
            const unsigned event=cloth_seq?0u:1u;
            char tag[80];
            if(cloth_seq)std::snprintf(tag,sizeof(tag),"RT-PREV-CLOTH seq=%llu",(unsigned long long)cloth_seq);
            else std::snprintf(tag,sizeof(tag),"RT-PREV-REJECT armed_window=%u",unsigned(armed_window));
            char line[3072];
            std::snprintf(line,sizeof(line),"%s event=%u frame=%llu component=%u result=%s mod=%llu rev=%llu history=%llu srv=%llx/%llu off=%llu bytes=%llu fmt=%u stride=%u native_window=%llu/%llu latest_order=%llu latest_record=%llx/%llu/%llu latest_submit=%u current_as=%llx/%llu@%llx current_out=%llx/%llu+%llu/%llu order=%llu drops=%llu,%llu,%llu,%llu reject=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                tag,event,(unsigned long long)current_frame,source.component,names[result],
                (unsigned long long)batch->assets->pack->key,(unsigned long long)batch->assets->pack->revision,(unsigned long long)surface_history.size(),
                (unsigned long long)v.resource,(unsigned long long)native_view.creation,(unsigned long long)v.byte_offset,(unsigned long long)v.bytes,v.format,v.structure_stride,
                (unsigned long long)first,(unsigned long long)count,(unsigned long long)latest.order,(unsigned long long)latest.producer.list,
                (unsigned long long)latest.producer.creation,(unsigned long long)latest.producer.generation,latest.submissions,
                (unsigned long long)source.identity->allocation.object,(unsigned long long)source.identity->allocation.creation,(unsigned long long)source.identity->native_blas,
                (unsigned long long)s.output.object,(unsigned long long)s.output.creation,(unsigned long long)s.output_offset,(unsigned long long)s.output_bytes,(unsigned long long)s.order,
                (unsigned long long)previous_history_drops[0],(unsigned long long)previous_history_drops[1],(unsigned long long)previous_history_drops[2],(unsigned long long)previous_history_drops[3],
                rejected[0],rejected[1],rejected[2],rejected[3],rejected[4],rejected[5],rejected[6],rejected[7],rejected[8],rejected[9],rejected[10]);
            if(srv_reject){
                const auto used=std::strlen(line);
                std::snprintf(line+used,sizeof(line)-used," srv_reject=%s",srv_reject);
            }
            if(sequence){
                const auto &g=current.geometry;
                const auto used=std::strlen(line);
                std::snprintf(line+used,sizeof(line)-used," draw_seq=%llu consumer=%llx/%llu/%llu",
                    (unsigned long long)sequence,(unsigned long long)g.consumer_list,(unsigned long long)g.consumer_list_creation,
                    (unsigned long long)g.consumer_recording);
            }
            if(ring_stage){
                const auto used=std::strlen(line);
                std::snprintf(line+used,sizeof(line)-used," submitted_ring=%s pose=%u missing_component=%u",ring_stage,unsigned(ring_pose),ring_missing);
            }
            if(draw_frame&&!part.dependencies.empty()&&latest.order){
                unsigned exact=0,historical=0,blocks=0;PendingPreviousProvider late;
                const bool track_late=cloth_seq&&armed_window&&scoped_record&&!scoped_record->pending_previous.failed_seq;
                for(const auto &p:previous_prepared){
                    if(p.route!=PreviousRoute::cohort_candidate||!exact_prepared(p))continue;
                    ++exact;historical+=bool(p.historical.recording.list);
                    const bool owned=author_previous&&previous_author_inputs(p);
                    const auto check=!owned&&(!p.batch||!completed(*p.batch))?
                        CohortCheck{author_previous&&p.owned_pose?CohortBlock::native_unready:CohortBlock::batch_unready}:
                        !owned&&!previous_inputs_live(p.proofs,pack,p.native_writer,p.route,p.historical,true)?CohortCheck{CohortBlock::native_unready}:
                        previous_cohort_read(p,scoped_record.get(),scope,source.identity,pack);
                    blocks|=1u<<unsigned(check.block);
                    if(track_late&&check.block==CohortBlock::provider_unseen_at_draw){
                        const auto &s=*check.provider;PreviousReadUse wanted{s.output,s.output_offset,s.output_bytes,{s.recording,s.order,1}};
                        if(std::none_of(late.expected.begin(),late.expected.end(),[&](const auto &e){return e.component==s.component&&e.use==wanted;}))
                            late.expected.push_back({s.component,wanted});
                    }
                }
                if(!exact)blocks=1u<<unsigned(CohortBlock::no_exact_candidate);
                const auto used=std::strlen(line);
                std::snprintf(line+used,sizeof(line)-used," cohort_exact=%u cohort_historical=%u cohort_block=",exact,historical);
                bool separator=false;
                for(unsigned i=0;i<std::size(cohort_block_names);++i)if(blocks&(1u<<i)){
                    const auto end=std::strlen(line);
                    std::snprintf(line+end,sizeof(line)-end,"%s%s",separator?",":"",cohort_block_names[i]);separator=true;
                }
                if(!late.expected.empty()){
                    late.owner=source.identity;late.pack=pack;late.scope=scope;late.failed_seq=cloth_seq;late.target_order=latest.order;
                    scoped_record->pending_previous=std::move(late);
                }
            }
            reshade::log::message(reshade::log::level::info,line);
            if(cloth_seq&&native_view){
                const auto interest=std::find_if(previous_interests.begin(),previous_interests.end(),[&](const auto &i){return
                    i.pack==batch->assets->pack&&i.output==native_view&&i.view_offset==v.byte_offset&&i.view_bytes==v.bytes&&
                    i.component==source.component&&i.content==s.content&&i.content_offset==s.content_offset;});
                if(interest!=previous_interests.end()){
                    unsigned candidates=0,live=0,submitted=0,complete=0,exact=0,exact_live=0,exact_submitted=0,exact_complete=0;
                    for(const auto &p:previous_prepared){const auto &i=p.source;
                        if(i.pack!=interest->pack||i.output!=interest->output||i.view_offset!=interest->view_offset||i.view_bytes!=interest->view_bytes||
                            i.component!=interest->component||i.content!=interest->content||i.content_offset!=interest->content_offset)continue;
                        if(p.route==PreviousRoute::cohort_candidate&&previous_cohort_read(p,scoped_record.get(),scope,source.identity,pack).block!=CohortBlock::none)continue;
                        const bool is_live=previous_inputs_live(p.proofs,i.pack,p.native_writer,p.route,p.historical,false),
                            is_submitted=previous_inputs_live(p.proofs,i.pack,p.native_writer,p.route,p.historical,true),is_complete=p.batch&&completed(*p.batch);
                        ++candidates;live+=is_live;submitted+=is_submitted;complete+=is_complete;
                        const auto actual=std::find_if(p.proofs.begin(),p.proofs.end(),[&](const auto &proof){return latest.order&&
                            proof.component==i.component&&proof.output==native_view&&proof.output_offset==v.byte_offset+first&&
                            proof.output_bytes==count&&proof.order==latest.order&&proof.recording==latest.producer;});
                        if(actual!=p.proofs.end()){++exact;exact_live+=is_live;exact_submitted+=is_submitted;exact_complete+=is_complete;}
                    }
                    char prep[480];std::snprintf(prep,sizeof(prep),"RT-PREV-PREP seq=%llu stage=%s pose=%u missing_component=%u write_component=%u write_order=%llu write_record=%llx/%llu/%llu candidates=%u live=%u submitted=%u completed=%u",
                        (unsigned long long)cloth_seq,interest->last_stage,unsigned(interest->last_pose),interest->missing_component,interest->last_write_component,
                        (unsigned long long)interest->last_write_order,(unsigned long long)interest->last_recording.list,
                        (unsigned long long)interest->last_recording.creation,(unsigned long long)interest->last_recording.generation,candidates,live,submitted,complete);
                    reshade::log::message(reshade::log::level::info,prep);
                    const bool link_current=latest.order&&interest->link_order==latest.order&&interest->link_recording==latest.producer;
                    const bool sample_current=latest.order&&interest->last_write_order==latest.order&&interest->last_recording==latest.producer;
                    char link[640];std::snprintf(link,sizeof(link),"RT-PREV-LINK seq=%llu target_order=%llu record=%llx/%llu/%llu stage=%s build_record=%llx/%llu/%llu provider_order=%llu provider=%llx/%llu local_same=%u local_other=%u other_recordings=%u exact_prepared=%u exact_live=%u exact_submitted=%u exact_completed=%u",
                        (unsigned long long)cloth_seq,(unsigned long long)latest.order,(unsigned long long)latest.producer.list,
                        (unsigned long long)latest.producer.creation,(unsigned long long)latest.producer.generation,
                        link_current?interest->link_stage:"not_observed_for_write",
                        (unsigned long long)(link_current?interest->link_build_recording.list:0),
                        (unsigned long long)(link_current?interest->link_build_recording.creation:0),
                        (unsigned long long)(link_current?interest->link_build_recording.generation:0),
                        (unsigned long long)(link_current?interest->provider_order:0),
                        (unsigned long long)(link_current?interest->provider_output.object:0),
                        (unsigned long long)(link_current?interest->provider_output.creation:0),
                        sample_current?interest->local_same:UINT32_MAX,sample_current?interest->local_other:UINT32_MAX,
                        sample_current?interest->other_recordings:UINT32_MAX,exact,exact_live,exact_submitted,exact_complete);
                    reshade::log::message(reshade::log::level::info,link);
                    if(latest.order){interest->failed_seq=cloth_seq;interest->failed_order=latest.order;interest->failed_recording=latest.producer;}
                }
            }
            for(const auto &d:debug){if(d.index==SIZE_MAX)continue;const auto &h=surface_history[d.index];if(!h.batch)continue;
                const auto &c=h.identity->components[source.component];const auto &old=c.sample;const auto &b=*h.batch;
                const auto q=queues.find(b.gpu_queue);const auto done=q==queues.end()?0:q->second.completed;
                const unsigned live=cloth_seq?unsigned(inputs.position_live(old)):2u;
                std::snprintf(line,sizeof(line),"%s candidate frame=%llu component=%u reason=%s seen=%llu,%llu label_minus_one=%u historical_as=%llx/%llu@%llx origin=%llu full_span=%llu out=%llx/%llu+%llu/%llu order=%llu write_record=%llx/%llu/%llu live=%u native_submit=%u rest=%llx/%llu+%llu ib=%llx/%llu+%llu/%u batch_record=%llx/%llu/%llu submit=%u queue=%llx fence=%llx value=%llu done=%llu",
                    tag,(unsigned long long)current_frame,source.component,names[d.reason],(unsigned long long)h.frames[0],(unsigned long long)h.frames[1],
                    unsigned(current_frame&&(h.frames[0]==current_frame-1||h.frames[1]==current_frame-1)),
                    (unsigned long long)h.identity->allocation.object,(unsigned long long)h.identity->allocation.creation,(unsigned long long)h.identity->native_blas,
                    (unsigned long long)c.vertices.offset,(unsigned long long)c.vertices.bytes,(unsigned long long)old.output.object,(unsigned long long)old.output.creation,
                    (unsigned long long)old.output_offset,(unsigned long long)old.output_bytes,(unsigned long long)old.order,
                    (unsigned long long)old.recording.list,(unsigned long long)old.recording.creation,(unsigned long long)old.recording.generation,live,inputs.position_submission_count(old),
                    (unsigned long long)old.content.object,(unsigned long long)old.content.creation,(unsigned long long)old.content_offset,
                    (unsigned long long)c.indices.object,(unsigned long long)c.indices.creation,(unsigned long long)c.index_offset,c.index_count,
                    (unsigned long long)b.gpu_recording.list,(unsigned long long)b.gpu_recording.creation,(unsigned long long)b.gpu_recording.generation,b.gpu_submissions,
                    (unsigned long long)b.gpu_queue,(unsigned long long)b.gpu_fence,(unsigned long long)b.gpu_value,(unsigned long long)done);
                reshade::log::message(reshade::log::level::info,line);
            }
        };
        if(draw_frame&&!v.resource){observe();report(binding_missing);return WWMI_RT_SURFACE_PREVIOUS_MISSING;}
        if(const char *why=inputs.native_position_srv_error(v.resource,v.byte_offset,v.bytes,v.format,v.structure_stride,native_view)){
            observe();report(reason_count+1,why);return WWMI_RT_SURFACE_PREVIOUS_MISSING;
        }
        if(v.byte_offset>UINT64_MAX-first||first>v.bytes||count>v.bytes-first){observe();++rejected[view_extent];report(view_extent);return WWMI_RT_SURFACE_PREVIOUS_MISSING;}
        // A real, valid SRV establishes demand even before we have seen its
        // first write. Only the producer/consumer proofs can establish readiness.
        if(!author_previous&&!pack->shapekey_vertices&&part.index_count&&part.vertex_count&&part.rest_skinning_known&&!part.extra_position_input&&
            !part.bone_map.empty()&&batch->final_positions[source.component].mode!=WWMI_RT_POSITION_FINAL)
            remember_previous(pack,native_view,v.byte_offset,v.bytes,source.component,now.sample.content,now.sample.content_offset,
                bool(scoped_record),false);
        // The real VS reads prev[3*SV_VertexID + xyz]. Resolve THAT input's
        // unique current native write receipt, not a globally numbered frame.
        // An intervening present/pass need not draw or update this character.
        latest=inputs.position_receipt(native_view,v.byte_offset+first,count);
        // The original graphics SRV was resolved before the VM. A new write
        // between that draw observation and acquisition is not its previous pose.
        if(author_use&&(native_view.creation!=author_use->resource_creation||!author_use->order||
           latest.order!=author_use->order||latest.producer!=Recording{author_use->writer_list,
               author_use->writer_creation,author_use->writer_recording})){
            report(native_written);return WWMI_RT_SURFACE_PREVIOUS_MISSING;
        }
        // Native read-use is real even when this provider's private history
        // is unavailable. An unresolved or differing later use conflicts.
        observe({native_view,v.byte_offset+first,count,latest});
        SurfaceHistory *found=nullptr;bool source_candidate=false;
        std::shared_ptr<const Pose> found_pose;
        const auto same_instance=[&](const AuthorPlan &p){
            return current_scope&&p.requires_source&&p.load==current_scope->load&&p.mod==current_scope->mod&&
                p.primitive==current_scope->primitive&&p.scope.byte_origin==current_scope->scope.byte_origin;
        };
        // Ordinary REST v2 already had native/REST and submitted-ring
        // recovery. Narrow it only for a completed decision on this source.
        const bool require_adopted=current_scope&&current_scope->requires_source&&
            (bool(batch->final_positions[source.component].completed_author)||
             (latest.order&&std::any_of(author_plans.begin(),author_plans.end(),[&](const auto &p){
                 const auto i=source.component;
                 if(!same_instance(*p)||!p->pose||p->identity==source.identity||
                     i>=p->pose->final_positions.size()||!p->pose->final_positions[i].completed_author||
                     i>=p->identity->components.size())return false;
                 const auto &old=p->identity->components[i].sample;
                 return old.component==i&&old.recording==latest.producer&&old.order==latest.order&&
                     old.output==native_view&&old.output_offset==v.byte_offset+first&&old.output_bytes==count;
             })));
        const auto adopted=[&](const SurfaceHistory &h) {
            if(!h.owned_pose||source.component>=h.owned_pose->final_positions.size())return false;
            const auto &old_input=h.owned_pose->final_positions[source.component].completed_author;
            return std::any_of(author_plans.begin(),author_plans.end(),[&](const auto &p){
                if(!same_instance(*p)||p->identity!=h.identity||p->pack!=h.pack||p->pose!=h.owned_pose||
                    source.component>=p->sources.size())return false;
                const auto &old=p->sources[source.component];
                if(!old.section||old.empty||!old.selection.recipe_receipt)return false;
                return old_input?h.completed_plan==p->id&&
                    !std::memcmp(&old.selection,&old_input->source().selection,sizeof(old.selection)):
                    !h.completed_plan&&old.selection.kind==WWMI_RT_DEFORMED_AUTHOR_REST;
            });
        };
        for(auto &h:surface_history){
            if((require_adopted&&!h.pack)||(!require_adopted&&h.completed_plan))continue;
            auto reject=[&](unsigned reason){++rejected[reason];furthest=std::max(furthest,int(reason));remember(h,reason);};
            const bool native_only=author_previous&&!h.pack&&owned_author_pack(pack);
            if(!h.pack&&!native_only)continue; // The old entry points don't see bare sources.
            if(!native_only&&h.pack!=pack){reject(selection);continue;}
            if(source.component>=h.identity->components.size()){reject(component);continue;}
            const auto &old=h.identity->components[source.component];const auto &s=old.sample;
            if(s.output==native_view)source_candidate=true;
            if(s.profile!=now.sample.profile||s.component!=now.sample.component||s.content!=now.sample.content||s.content_offset!=now.sample.content_offset||
                old.indices!=now.indices||old.index_offset!=now.index_offset||old.index_count!=now.index_count){reject(source_layout);continue;}
            if(s.output!=native_view){reject(source_resource);continue;}
            if(old.vertices.offset!=v.byte_offset||s.output_offset!=v.byte_offset+first||s.output_bytes!=count){reject(vertex_origin);continue;}
            if(!latest.order||s.order!=latest.order||s.recording!=latest.producer||!inputs.position_live(s)){reject(native_written);continue;}
            if(latest.submissions!=1){reject(native_submission);continue;}
            if(author_previous&&h.identity==source.identity){reject(native_written);continue;}
            std::shared_ptr<const Pose> owned;
            if(author_previous&&writer_signal_known(h.native_signal,false)){
                owned=h.owned_pose;
                if(native_only){
                    const auto *cached=history_for(h.identity,pack);
                    if(cached&&cached->owned_pose&&cached->native_signal==h.native_signal)continue; // same derivation is considered below
                    // This draw's actual previous SRV uniquely identifies the
                    // original linked pose. Skin that precise CPU snapshot
                    // using the current immutable author topology; no old Mod
                    // draw, private batch or GPU resource is presumed here.
                    auto exact=inputs.assemble_snapshot(*h.identity,pack);
                    if(exact.result==PoseResult::complete)owned=std::move(exact.pose);
                }
            }
            // The previous SRV proves the writer, not the source its author
            // adopted. Verify that scope's committed source, whatever its kind.
            if(require_adopted&&(!owned||!adopted(h))){reject(private_incomplete);continue;}
            if(!owned&&(!h.batch||!completed(*h.batch))){reject(private_incomplete);continue;}
            const auto &writer=owned?owned->recording:h.batch->producer.recording;
            const auto &windows=owned?owned->source_vertices:h.batch->producer.geometries;
            const auto &orders=owned?owned->source_orders:h.batch->source_orders;
            bool provenance=writer==h.identity->recording&&windows.size()==h.identity->components.size()&&orders.size()==h.identity->components.size();
            if(provenance)for(unsigned i=0;i<orders.size();++i){const auto &p=windows[i],&native=h.identity->components[i].vertices;
                if(orders[i]!=h.identity->components[i].sample.order||p.vertex!=native.vertex||p.offset!=native.offset||p.bytes!=native.bytes){provenance=false;break;}}
            if(!provenance){reject(batch_provenance);continue;}
            // The actual previous SRV names this native source pose. It is not
            // inferred from AS equality, nearest frame, current or latest draw.
            // Two different complete bone-provider generations stay ambiguous.
            if(found&&(found->identity->recording!=h.identity->recording||
                (found_pose?found_pose->source_orders:found->batch->source_orders)!=orders||
                (require_adopted&&found_pose!=owned))){
                reject(ambiguous);report(ambiguous);return WWMI_RT_SURFACE_PREVIOUS_MISSING;
            }
            remember(h,reason_count);if(!found){found=&h;found_pose=std::move(owned);}
        }
        if(!found){
            // Unselected native/REST caches cannot resolve a missing
            // source-aware adoption, even when this draw itself uses REST.
            if(require_adopted){report(furthest<0?reason_count+2:unsigned(furthest));return WWMI_RT_SURFACE_PREVIOUS_MISSING;}
            PreviousPrepared *prepared=nullptr;const SkinSample *target=nullptr;
            PreviousPrepared *cohort=nullptr;const SkinSample *cohort_target=nullptr;bool cohort_conflict=false;
            for(auto &p:previous_prepared){
                if(p.route==PreviousRoute::cohort_candidate&&(prepared||!scoped_record))continue;
                const auto *actual=exact_prepared(p);
                if(!actual)continue;
                const bool owned=author_previous&&previous_author_inputs(p);
                if(!owned&&(!p.batch||!completed(*p.batch)||!previous_inputs_live(p.proofs,pack,p.native_writer,p.route,p.historical,true)))continue;
                if(p.route==PreviousRoute::cohort_candidate){
                    if(previous_cohort_read(p,scoped_record.get(),scope,source.identity,pack).block==CohortBlock::none){
                        if(cohort&&cohort!=&p)cohort_conflict=true;
                        else{cohort=&p;cohort_target=actual;}
                    }
                    continue;
                }
                if(prepared&&prepared!=&p){report(ambiguous);return WWMI_RT_SURFACE_PREVIOUS_MISSING;}
                prepared=&p;target=actual;
            }
            if(!prepared){
                if(cohort_conflict){report(ambiguous);return WWMI_RT_SURFACE_PREVIOUS_MISSING;}
                prepared=cohort;target=cohort_target;
            }
            if(!prepared&&author_previous&&author_use&&latest.submissions==1&&latest.native_queue&&owned_author_pack(pack)){
                // Native order/creation was frozen by the original SRV before
                // the VM. This route can recover it even if demand arrived
                // after the first write and its list has since been reset.
                auto recovered=inputs.assemble_submitted_previous(pack,now.sample,native_view,v.byte_offset,latest);
                ring_pose=recovered.assembly.result;ring_missing=recovered.missing_component;
                ring_stage="no_complete_cpu_pose";
                if(ring_pose==PoseResult::complete&&recovered.assembly.pose&&!recovered.sources.empty()){
                    const auto &s=recovered.sources.front();
                    ring_stage="source_no_longer_exact";
                    if(s.order==latest.order&&s.recording==latest.producer&&s.output==native_view&&
                       s.output_offset==v.byte_offset+first&&s.output_bytes==count&&
                       inputs.previous_sources_on_queue(recovered.sources,pack,latest.native_queue)){
                        PreviousInterest observed{pack,native_view,now.sample.content,v.byte_offset,v.bytes,now.sample.content_offset,
                            source.component,false,true,false};
                        previous_prepared.push_back({std::move(observed),std::move(recovered.sources),{},PreviousRoute::submitted_ring,{},{},
                            std::shared_ptr<const Pose>(std::move(recovered.assembly.pose)),latest.producer,{},{},latest.native_queue});
                        prepared=&previous_prepared.back();target=&prepared->proofs.front();
                        ring_stage="cpu_pose_ready";
                    }
                }
            }
            if(prepared){
                const auto &g=current.geometry;
                auto previous=prepared->batch;
                if(author_previous&&(!previous||!completed(*previous)||!previous_inputs_live(prepared->proofs,pack,prepared->native_writer,prepared->route,prepared->historical,true))){
                    previous=prepare_author_streams(prepared->owned_pose,{g.consumer_list,g.consumer_list_creation,g.consumer_recording},
                        reinterpret_cast<api::command_list *>(g.consumer_list),source.component,prepared->author_outputs,{},&prepared->author_palette,prepared->proofs);
                }
                if(!previous){report(private_incomplete);return WWMI_RT_SURFACE_PREVIOUS_MISSING;}
                const auto &b=*previous;auto &holds=records[g.consumer_list];
                if(std::none_of(holds.begin(),holds.end(),[&](const auto &h){return h.get()==previous.get();}))holds.push_back(previous);
                out.struct_size=sizeof(out);out.abi_version=1;out.component=request.current.planned.draw.component;
                out.positions=surface_view(b.positions.Get(),12ull*b.assets->pack->vertices,DXGI_FORMAT_R32_FLOAT,4,12,position_read_state);
                // No frame tag is invented: this was observed as a producer,
                // not as an earlier graphics acquisition.
                out.frame_id=0;out.pose_order=target->order;out.native_resource=target->output.object;out.native_creation=target->output.creation;out.native_offset=v.byte_offset;
                out.producer_queue=b.gpu_queue;out.producer_fence=b.gpu_fence;out.producer_value=b.gpu_value;
                if(const auto q=queues.find(b.gpu_queue);q!=queues.end())out.observed_completion=q->second.completed;
                out.consumer_list=current.geometry.consumer_list;out.consumer_creation=current.geometry.consumer_list_creation;out.consumer_recording=current.geometry.consumer_recording;
                if(author_previous)*author_previous=reinterpret_cast<std::uint64_t>(previous.get());
                if(lease){lease->previous=previous;lease->previous_cohort=prepared->cohort.lock();}
                if(cloth_previous_trace_seq){const auto seq=cloth_previous_trace_seq();if(seq){char line[440];
                    const auto provider=std::find_if(prepared->proofs.begin(),prepared->proofs.end(),[&](const auto &p){return p.component!=source.component;});
                    std::snprintf(line,sizeof(line),"RT-PREV-CLOTH seq=%llu result=prepared_ok component=%u order=%llu native=%llx/%llu producer=%llx/%llu/%llu fence=%llx value=%llu done=%llu route=%s provider_order=%llu provider=%llx/%llu",
                        (unsigned long long)seq,source.component,(unsigned long long)target->order,(unsigned long long)target->output.object,(unsigned long long)target->output.creation,
                        (unsigned long long)target->recording.list,(unsigned long long)target->recording.creation,(unsigned long long)target->recording.generation,
                        (unsigned long long)b.gpu_fence,(unsigned long long)b.gpu_value,(unsigned long long)out.observed_completion,
                        prepared->route==PreviousRoute::linked_blas?"blas":prepared->route==PreviousRoute::dispatch?"dispatch":
                        prepared->route==PreviousRoute::cohort_candidate?(prepared->historical.recording.list?"cohort_history":"cohort_read"):
                        prepared->route==PreviousRoute::submitted_ring?"submitted_ring":"alternate_write",
                        (unsigned long long)(provider==prepared->proofs.end()?0:provider->order),
                        (unsigned long long)(provider==prepared->proofs.end()?0:provider->output.object),
                        (unsigned long long)(provider==prepared->proofs.end()?0:provider->output.creation));
                    reshade::log::message(reshade::log::level::info,line);}}
                return WWMI_RT_SURFACE_OK;
            }
            const unsigned reason=!latest.order?unsigned(native_written):latest.submissions!=1?unsigned(native_submission):
                !source_candidate||furthest<0?unsigned(reason_count+2):unsigned(furthest);
            report(reason);return WWMI_RT_SURFACE_PREVIOUS_MISSING;
        }
        if(found_pose&&!found->pack){
            // Push may move the history vector. Copy the native owner and
            // receipt before appending a separate, pack-specific derivation.
            const auto identity=found->identity;
            auto &derived=author_history_for(identity,pack,found->native_signal);
            if(!derived.owned_pose)derived.owned_pose=found_pose;
            found=&derived;
        }
        // A legacy batch for this native write does not prove the source
        // adopted by the old scope. Resolve the previous window from its Pose.
        auto previous=require_adopted?std::shared_ptr<GeometryBatch>{}:found->batch;
        const auto &g=current.geometry;
        if(found_pose&&(!previous||!completed(*previous)))
            previous=prepare_author_batch(*found,{g.consumer_list,g.consumer_list_creation,g.consumer_recording},
                reinterpret_cast<api::command_list *>(g.consumer_list),source.component);
        if(!previous){report(private_incomplete);return WWMI_RT_SURFACE_PREVIOUS_MISSING;}
        const auto &old=found->identity->components[source.component];const auto &b=*previous;
        auto &holds=records[g.consumer_list];if(std::none_of(holds.begin(),holds.end(),[&](const auto &hold){return hold.get()==previous.get();}))holds.push_back(previous);
        out.struct_size=sizeof(out);out.abi_version=1;out.component=request.current.planned.draw.component;
        out.positions=surface_view(b.positions.Get(),12ull*b.assets->pack->vertices,DXGI_FORMAT_R32_FLOAT,4,12,position_read_state);
        // Tags describe observed surface acquisition, not game-pose chronology.
        // Do not invent a current_frame-1 tag for a source never observed there.
        for(const auto observed:found->frames)if(observed<current_frame)out.frame_id=std::max(out.frame_id,observed);
        out.pose_order=old.sample.order;out.native_resource=old.sample.output.object;out.native_creation=old.sample.output.creation;out.native_offset=old.vertices.offset;
        out.producer_queue=b.gpu_queue;out.producer_fence=b.gpu_fence;out.producer_value=b.gpu_value;
        if(const auto q=queues.find(b.gpu_queue);q!=queues.end())out.observed_completion=q->second.completed;
        out.consumer_list=current.geometry.consumer_list;out.consumer_creation=current.geometry.consumer_list_creation;out.consumer_recording=current.geometry.consumer_recording;
        if(author_previous)*author_previous=reinterpret_cast<std::uint64_t>(previous.get());
        if(lease){lease->previous=previous;lease->previous_cohort=found->identity;}
        return WWMI_RT_SURFACE_OK;
    }
    std::uint32_t acquire_draw_frame(const WWMI_RtDrawFrameRequest_v1 &request,WWMI_RtDrawFrame_v1 &out,const WWMI_RtPreviousDrawScope_v1 *scope=nullptr,
        const WWMI_RtAuthorFrameRequest_v1 *author=nullptr,WWMI_RtAuthorFrame_v1 *author_out=nullptr,
        const WWMI_RtDeformedSelection_v1 *deformed=nullptr,const WWMI_RtAuthorSectionParts_v3 *section=nullptr,
        AuthorSectionLease *lease=nullptr){
        std::shared_ptr<GeometryBatch> batch;NativeDrawIdentityCache::Match source;
        const auto &current=request.inputs.current;
        out.struct_size=sizeof(out);out.abi_version=1;out.failed_phase=WWMI_RT_DRAW_CURRENT;
        if((request.flags&WWMI_RT_DRAW_MATERIAL)&&!(author&&!author->subdraw_count)){
            const auto status=acquire_surface(current,out.current,&batch,&source,&request.source,&out.geometry_status,
                author,author_out,deformed,section?section->draws:nullptr);
            if(status!=WWMI_RT_SURFACE_OK)return status;
        }else{
            const auto &p=current.planned;
            out.geometry_status=acquire_graphics_geometry(p.draw,out.current.geometry,p.frame_id,p.path_flags,&batch,&source,&request.source,
                author,author_out,deformed,section?section->draws:nullptr);
            if(out.geometry_status!=WWMI_RT_GEOMETRY_OK)return WWMI_RT_SURFACE_NOT_READY;
        }
        if(section){
            const auto &g=out.current.geometry;
            section->indices[0]={g.index_gpu_va,g.index_offset,g.index_bytes,g.index_count};
            for(unsigned i=1;i<author->subdraw_count;++i){
                out.geometry_status=selected_index_view(*batch,source.component,section->draws[i],section->indices[i]);
                if(out.geometry_status!=WWMI_RT_GEOMETRY_OK)return WWMI_RT_SURFACE_NOT_READY;
            }
            lease->current=batch;
        }
        out.current_pose_order=source.identity->components[source.component].sample.order;
        if((request.flags&WWMI_RT_DRAW_PREVIOUS)&&!(author&&!author->subdraw_count)){
            out.failed_phase=WWMI_RT_DRAW_HISTORY;
            const auto status=previous_for(request.inputs,out.current,batch,source,out.previous,true,scope,
                author_out?&author_out->previous_batch:nullptr,author?&author->previous:nullptr,lease,
                author&&deformed?find_author_plan(author->plan):nullptr);
            if(status!=WWMI_RT_SURFACE_OK)return status;
        }
        out.failed_phase=0;return WWMI_RT_SURFACE_OK;
    }
    // Raster and the exact ray recording share append-only descriptor pages.
    // The caller's DrawBindingRecording and native flights own them.
    std::shared_ptr<DrawBindingPage> take_binding_page(DrawBindingRecording &binding,unsigned n,unsigned slots){
        auto page=binding.pages.empty()?std::shared_ptr<DrawBindingPage>{}:binding.pages.back();
        if(!page||!page->fits(n,slots)){
            // A recording keeps its pages even after its first successful
            // submit so later replay still sees immutable descriptor/CB data.
            page.reset();
            for(const auto &available:binding_pages)if(available.use_count()==1&&available->capacity>=n){page=available;break;}
            if(!page){
                page=std::make_shared<DrawBindingPage>();page->capacity=(std::max)(4096u,n);
                D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,page->capacity,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
                gpu::check(native->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&page->heap)),"RT draw binding heap");
                page->increment=native->GetDescriptorHandleIncrementSize(desc.Type);
                page->constants=gpu::buffer(native.Get(),DrawBindingPage::constant_stride*DrawBindingPage::constants_slots,
                    D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
                D3D12_RANGE no_read{0,0};void *mapped=nullptr;
                gpu::check(page->constants->Map(0,&no_read,&mapped),"RT draw binding constants map");page->mapped=static_cast<unsigned char *>(mapped);
                binding_pages.push_back(page);
            }
            page->descriptors_used=page->constants_used=0;
            binding.pages.push_back(page);
        }
        return page;
    }
    std::uint32_t reserve_draw_binding(const WWMI_RtDrawBindingRequest_v1 &request,WWMI_RtDrawBinding_v1 &out,
        const void *const *no_history_cb,std::uint64_t *no_history_va){
        const auto epoch=inputs.recording(reinterpret_cast<api::command_list *>(request.list));
        const auto list_holds=records.find(request.list);
        if(epoch!=Recording{request.list,request.list_creation,request.recording}||list_holds==records.end()||
           std::none_of(list_holds->second.begin(),list_holds->second.end(),[&](const auto &held){
               return reinterpret_cast<std::uint64_t>(held.get())==request.batch_token;}))return WWMI_RT_SURFACE_NOT_READY;
        auto binding=draw_recording(epoch.list);
        if(!binding||binding->epoch!=epoch)return WWMI_RT_SURFACE_NOT_READY;
        const auto reset=binding->reset.load(std::memory_order_acquire);
        if(reset==DrawBindingRecording::waiting||reset==DrawBindingRecording::succeeded)return WWMI_RT_SURFACE_NOT_READY;
        const auto n=request.descriptor_count;
        constexpr unsigned cb_bytes[]={WWMI_RT_NH_VIEW_BYTES,WWMI_RT_NH_PRIMITIVE_BYTES,
                                       WWMI_RT_NH_VIEW_BYTES,WWMI_RT_NH_PRIMITIVE_BYTES};
        constexpr unsigned stride=DrawBindingPage::constant_stride;
        const auto slots_for=[](unsigned bytes){return (bytes+stride-1)/stride;};
        unsigned slots=1;
        if(no_history_cb)for(unsigned i=0;i<WWMI_RT_NH_CB_COUNT;++i)
            if(no_history_cb[i])slots+=slots_for(cb_bytes[i]);
        auto page=take_binding_page(*binding,n,slots);
        for(unsigned i=0;i<request.resource_count;++i){
            auto *texture=reinterpret_cast<ID3D12Resource *>(request.resources[i]);
            if(texture&&std::none_of(binding->textures.begin(),binding->textures.end(),[&](const auto &r){return r.Get()==texture;}))binding->textures.emplace_back(texture);
        }
        const auto offset=std::uint64_t(page->descriptors_used)*page->increment;
        const auto cb_offset=page->constants_used*stride;
        page->constants_used+=slots;
        page->descriptors_used+=n;
        std::memcpy(page->mapped+cb_offset,request.local_vf,sizeof(request.local_vf));
        const auto cb_base=page->constants->GetGPUVirtualAddress();
        if(no_history_cb){
            auto next_cb=cb_offset+stride;
            for(unsigned i=0;i<WWMI_RT_NH_CB_COUNT;++i)if(no_history_cb[i]){
                std::memcpy(page->mapped+next_cb,no_history_cb[i],cb_bytes[i]);
                no_history_va[i]=cb_base+next_cb;
                next_cb+=slots_for(cb_bytes[i])*stride;
            }
        }
        out={};out.struct_size=sizeof(out);out.abi_version=1;out.descriptor_increment=page->increment;
        out.heap=reinterpret_cast<std::uint64_t>(page->heap.Get());
        out.cpu_base=page->heap->GetCPUDescriptorHandleForHeapStart().ptr+offset;
        out.gpu_base=page->heap->GetGPUDescriptorHandleForHeapStart().ptr+offset;
        out.local_vf_va=cb_base+cb_offset;
        return WWMI_RT_SURFACE_OK;
    }
    std::shared_ptr<DrawBindingRecording> draw_recording(std::uint64_t list) const{
        auto p=binding_recordings.find(list);return p==binding_recordings.end()?nullptr:p->second.lock();
    }
    void forget_recording(api::command_list *c){
        const auto list=reinterpret_cast<std::uint64_t>(c);
        if(auto *bin=author_work.current())if(const auto draw=draw_recording(list))
            for(const auto &prefix:draw->author_prefixes)bin->uncommitted_reset+=!prefix.committed;
        const bool released=records.erase(list)!=0;
        binding_recordings.erase(list);generated_records.erase(list);tlas_records.erase(list);
        native_probe_records.erase(list);
        std::erase_if(native_probe_pending,[&](const auto &p){return p->evidence->recording.list==list&&!p->submissions;});
        const bool writer=pending_previous_writers.erase(list)!=0;
        if(writer)history_dirty=true;
        if(released){
            history_owners_changed();
            if(author_pool_release_due&&catalog.packs.empty())release_unloaded_author_storage();
        }
    }
    void reset_recording(api::command_list *c){
        auto binding=draw_recording(reinterpret_cast<std::uint64_t>(c));
        if(!binding){inputs.reset_list(c);forget_recording(c);return;}
        if(binding->reset.exchange(DrawBindingRecording::waiting,std::memory_order_relaxed)!=DrawBindingRecording::waiting)
            pending_draw_resets.emplace_back(c,binding);
        draw_reset_receipt=binding;
    }
    void drain_draw_resets(){
        std::erase_if(pending_draw_resets,[&](const auto &p){
            auto binding=p.second.lock();if(!binding)return true;
            const auto result=binding->reset.load(std::memory_order_acquire);
            if(result==DrawBindingRecording::waiting)return false;
            if(result==DrawBindingRecording::succeeded){
                inputs.reset_list(p.first);
                binding.reset(); // let forget_recording see the final recording owner leave
                forget_recording(p.first);
            }
            return true;
        });
    }
    void submit_before(api::command_queue *q,api::command_list *c){
        rt_cpu::Scope measured(rt_cpu::submit_pre);
        const auto list=reinterpret_cast<std::uint64_t>(c);
        const auto epoch=inputs.submission_recording(c);
        // Keep the exact pre/native-post epoch pair, including content upload
        // receipts and replay invalidation. Do not manufacture three empty map
        // entries for every unrelated recording merely to copy empty vectors.
        inputs.native_submission_started(epoch);
        Submission snapshot;snapshot.epoch=epoch;snapshot.identities=identities.freeze(epoch);
        snapshot.has_skin_samples=inputs.recording_has_skin_samples(epoch);
        if(const auto p=native_probe_records.find(list);p!=native_probe_records.end())for(const auto &probe:p->second)if(probe->recorded&&probe->evidence->recording==epoch)snapshot.native_probes.push_back(probe);
        if(instance_probe&&instance_probe->build.recording==epoch){
            if(instance_probe->pre<2)++instance_probe->pre;
            snapshot.instance_probe=instance_probe;
        }
        if(raygen_probe&&raygen_probe->recording==epoch){
            if(raygen_probe->pre<2)++raygen_probe->pre;
            snapshot.raygen_probe=raygen_probe;
        }
        if(const auto p=pending_previous_writers.find(list);p!=pending_previous_writers.end()){
            auto receipt=p->second.lock();
            if(receipt&&receipt->recording==epoch)snapshot.author_previous_writer=std::move(receipt);
        }
        if(auto p=records.find(list);p!=records.end())snapshot.holds=p->second;
        if(auto p=generated_records.find(list);p!=generated_records.end())snapshot.generated=p->second;
        if(auto p=tlas_records.find(list);p!=tlas_records.end())snapshot.tlas_writes=p->second;
        const auto binding=draw_recording(list);
        if(binding&&binding->epoch==epoch){
            snapshot.draw=binding;
            for(const auto &timer:binding->timers)if(timer->pre<2)++timer->pre;
            // Own allocator/list before Execute. If producer-ready Signal
            // fails, the ordinary consumer Flight still retires these holds.
            for(const auto &prefix:binding->author_prefixes)
                if(prefix.committed&&!prefix.executed&&prefix.commands)
                    snapshot.holds.push_back(prefix.commands);
        }
        auto &pending=submissions[{reinterpret_cast<std::uint64_t>(q),list}];
        pending.push_back(std::move(snapshot));
        if(binding&&binding->epoch==epoch){
            auto *consumer_queue=reinterpret_cast<ID3D12CommandQueue *>(q->get_native());
            // The bootstrap is immediately readable on every queue. Static
            // DEFAULT is exposed only after actual upload completion.
            for(const auto &prefix:binding->author_prefixes)
                if(prefix.committed)start_author_upload(prefix.static_assets,consumer_queue);
            if(!binding->author_pending_lists.empty()){
                pending.back().author_generated=std::move(binding->author_pending_batches);
                execute_author_outputs(*binding,consumer_queue);
            }
        }
        trace.emit("submit_before","frozen",reinterpret_cast<std::uint64_t>(q),q->get_native(),list);
    }
    void submit_after(std::uint64_t api_queue,ID3D12CommandQueue *native_queue,api::command_list *c){
        rt_cpu::Scope measured(rt_cpu::submit_post);
        const auto key=std::make_pair(api_queue,reinterpret_cast<std::uint64_t>(c));auto pending=submissions.find(key);
        if(pending==submissions.end()||pending->second.empty())throw std::runtime_error("RT native submit lacks paired pre-submit snapshot");
        auto &frozen=pending->second.front();
        Queue *flight_queue=nullptr;
        if(!frozen.holds.empty()||!frozen.native_probes.empty()||!frozen.identities.entries.empty()||!frozen.tlas_writes.empty()||frozen.author_previous_writer||frozen.has_skin_samples){
            flight_queue=&queues[reinterpret_cast<std::uint64_t>(native_queue)];
            flight_queue->flights.emplace_back();
            flight_queue->flights.back().value=++flight_queue->value;
            flight_queue->flights.back().holds=std::move(frozen.holds);
            flight_queue->flights.back().draw=frozen.draw;
        }
        auto snapshot=std::move(frozen);pending->second.pop_front();if(pending->second.empty())submissions.erase(pending);
        trace.emit("submit_after","native_execute_returned",api_queue,reinterpret_cast<std::uint64_t>(native_queue),snapshot.epoch.list);
        const bool ray_build_armed=ray_scene.observed_build()&&ray_input_window();
        const bool ray_build_submit=ray_build_armed||ray_scene.awaiting_submit(snapshot.epoch);
        if(ray_build_submit)ray_scene.submitted(snapshot.epoch,reinterpret_cast<std::uint64_t>(native_queue),ray_build_armed);
        inputs.submitted(snapshot.epoch,reinterpret_cast<std::uint64_t>(native_queue));if(!flight_queue){
            if(ray_build_submit)ray_scene.report_submitted(snapshot.epoch);
            return;
        }
        if(const auto draw=snapshot.draw.lock();draw&&draw->committed_sections){
            if(auto *bin=author_work.current()){
                ++bin->consumer_executes;bin->consumer_sections+=draw->committed_sections;
                bin->consumer_recorded_in_window+=draw->observed_sections;bin->consumer_replays+=draw->consumer_submitted;
            }
            draw->consumer_submitted=true;
        }
        auto &q=*flight_queue;if(!q.fence){q.native=native_queue;
            trace.emit("submit_after_fence_create","begin_original_device",reinterpret_cast<std::uint64_t>(native.Get()),reinterpret_cast<std::uint64_t>(native_queue));
            gpu::check(native->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&q.fence)),"RT retirement fence");
            trace.emit("submit_after_fence_create","end_original_device",reinterpret_cast<std::uint64_t>(q.fence.Get()));}
        // Flight already holds the actual submitted resources, even on failure.
        trace.emit("signal","begin_original_queue",reinterpret_cast<std::uint64_t>(q.native.Get()),reinterpret_cast<std::uint64_t>(q.fence.Get()),q.value);
        rt_cpu::Scope signal_measured(rt_cpu::signal);const auto signal_status=q.native->Signal(q.fence.Get(),q.value);signal_measured.end();trace.emit("signal","end_original_queue",reinterpret_cast<std::uint64_t>(q.native.Get()),std::uint64_t(std::uint32_t(signal_status)),q.value);
        const NativeIdentityFence signal{reinterpret_cast<std::uint64_t>(native_queue),reinterpret_cast<std::uint64_t>(q.fence.Get()),q.value};
        if(ray_build_submit){
            if(SUCCEEDED(signal_status))ray_scene.signalled(snapshot.epoch,signal,ray_build_armed);
            ray_scene.report_submitted(snapshot.epoch);
        }
        for(auto &probe:snapshot.native_probes){
            ++probe->submissions;++native_probe_submitted;
            if(SUCCEEDED(signal_status)&&probe->submissions==1){probe->completion=signal;inputs.native_signalled(probe->evidence->id,signal.queue,signal.fence,signal.value);}
            else{note_native_probe_reject(FAILED(signal_status)?ProbeReject::signal:ProbeReject::replay,*probe->evidence,FAILED(signal_status)?"queue_signal_failed":"submission_replay",std::int32_t(signal_status),probe.get());inputs.native_request_cancel(probe->evidence->id);}
        }
        if(FAILED(signal_status)&&!snapshot.native_probes.empty())report_native_discovery(true);
        if(snapshot.instance_probe&&FAILED(signal_status))
            occlusion_probe_status(8192,"probe_signal_failed",&snapshot.instance_probe->build,signal_status);
        if(snapshot.raygen_probe&&FAILED(signal_status))
            settle_raygen_probe("signal_failed",signal_status);
        if(const auto draw=snapshot.draw.lock())for(const auto &timer:draw->timers)
            if(timer->pre==1&&!timer->signal.value&&!timer->reported){
                if(SUCCEEDED(signal_status))set_work_signal(*timer,q,signal,false);
                else timer->error="consumer_signal_failed";
            }
        gpu::check(signal_status,"RT queue completion signal");
        if(snapshot.has_skin_samples)inputs.record_skin_fence(snapshot.epoch,signal);
        for(auto &upload:uploading_author_assets)
            if(!upload->signalled&&!upload->recovery.value&&upload->producer.Get()==native_queue)
                upload->recovery=signal;
        if(snapshot.instance_probe&&snapshot.instance_probe->post<2){
            if(++snapshot.instance_probe->post==1)snapshot.instance_probe->completion=signal;
        }
        if(snapshot.raygen_probe&&snapshot.raygen_probe->post<2){
            if(++snapshot.raygen_probe->post==1)snapshot.raygen_probe->completion=signal;
        }
        if(snapshot.author_previous_writer){
            const auto receipt=pending_previous_writers.find(snapshot.epoch.list);
            if(receipt!=pending_previous_writers.end()&&receipt->second.lock()==snapshot.author_previous_writer)
                pending_previous_writers.erase(receipt);
            snapshot.author_previous_writer->signal=signal;
        }
        if(!first_private_submission.value&&(!snapshot.generated.empty()||!snapshot.author_generated.empty())){first_private_submission=signal;trace.emit("first_private_submit","signal_succeeded",signal.queue,signal.fence,signal.value);}
        const auto stamp=[&](GeometryBatch &batch){++batch.gpu_submissions;batch.gpu_queue=signal.queue;batch.gpu_fence=signal.fence;batch.gpu_value=signal.value;};
        for(auto &batch:snapshot.generated)stamp(*batch);
        // Prefix generation happens once. Replaying the consumer continues
        // to read the same immutable output and retains its own flight.
        for(auto *batch:snapshot.author_generated)stamp(*batch);
        for(const auto &write:snapshot.tlas_writes){
            const auto key=std::make_pair(write.address,reinterpret_cast<std::uint64_t>(native_queue));const auto old=scene_owners.find(key);
            if(old!=scene_owners.end()){
                // Native Execute + successful Signal on this ACTUAL queue is
                // an ordered overwrite. Producer completion alone was not one.
                retiring_scene_owners.push_back({old->second,{key.second,reinterpret_cast<std::uint64_t>(q.fence.Get()),q.value}});scene_owners.erase(old);
                scene_retirement_due=true;
            }
            if(write.instance)scene_owners.emplace(key,SceneOwner{write,key.second});
        }
        identities.submitted(snapshot.identities,signal);
        if(!snapshot.identities.entries.empty())for(auto &h:surface_history)if(h.identity->recording==snapshot.epoch)
            h.native_signal=identities.producer_of(snapshot.identities,h.identity);
        if(!snapshot.identities.entries.empty()){snapshot.identities.entries.clear();history_owners_changed();}
        collect();
    }
    void destroyed(api::resource r,std::uint64_t creation){
        const auto key=inputs.destroy_resource(r,creation);if(!key)return;
        blas_interest.destroyed(key);
        // A historical AS allocation is not the previous-position input. The
        // batch owns its finished vertices/palettes; the exact still-live native
        // output window, write stamp and actual previous SRV prove consumption.
        // Destroying the old AS cannot destroy that independent owned snapshot.
        if(std::erase_if(surface_history,[&](const auto &h){const bool gone=std::any_of(h.identity->components.begin(),h.identity->components.end(),[&](const auto &c){return c.sample.output==key||c.sample.content==key;});if(gone)++previous_history_drops[3];return gone;}))history_dirty=true;
        if(author_primitive_refs.contains({key.object,key.creation}))history_dirty=true;
        std::erase_if(previous_interests,[&](const auto &i){return i.output==key||i.content==key;});
        std::erase_if(previous_prepared,[&](const auto &p){return p.source.output==key||p.source.content==key||std::any_of(p.proofs.begin(),p.proofs.end(),[&](const auto &s){return s.output==key||s.content==key;});});
        if(std::erase_if(targets,[&](const auto &x){return x.second->native_resource==key||std::any_of(x.second->geometry->producer.geometries.begin(),x.second->geometry->producer.geometries.end(),[&](const auto &g){return g.vertex==key;});}))history_owners_changed();
        std::erase_if(tlas,[&](const auto &x){return x.second.allocation==key;});
        std::erase_if(scene_owners,[&](const auto &x){return x.second.write.allocation==key;});
        std::erase_if(retiring_scene_owners,[&](const auto &x){return x.owner.write.allocation==key;});
    }
    void drain_buffers(std::vector<ComPtr<ID3D12Resource>> &release_outside_lock){
        if(!buffer_events.queued())return;
        rt_cpu::Scope measured(rt_cpu::inbox);
        BufferEvent event;
        while(buffer_events.take(event)){
            measured.add();
            trace.emit("resource_apply","begin",event.resource.handle,event.creation,event.created);
            if(event.created){const auto old=inputs.key_of(event.resource);if(old&&old.creation!=event.creation)destroyed(event.resource,old.creation);inputs.init_resource(event.resource,event.desc,event.creation,event.gpu_address);}
            else destroyed(event.resource,event.creation);
            buffer_events.applied(event);if(event.keep)release_outside_lock.push_back(std::move(event.keep));
            trace.emit("resource_apply","end",event.resource.handle,event.creation,event.created);
        }
    }
};
std::unordered_map<api::device *,std::shared_ptr<Runtime>> devices;
std::vector<std::shared_ptr<Runtime>> unfinished; // only an actual failed GPU retirement, never a successful stop
std::shared_ptr<Runtime> runtime(api::device *d){std::lock_guard lock(devices_mutex);auto p=devices.find(d);return p==devices.end()?nullptr:p->second;}
std::shared_ptr<Runtime> runtime_native(std::uint64_t device){
    std::lock_guard lock(devices_mutex);
    for(const auto &[_,r]:devices)if(r->native_device_id==device)return r;
    return {};
}
static rt_cpu::Kind cpu_callback_kind(const char *name){
    if(std::strstr(name,"PreviousSurface"))return rt_cpu::api_previous;
    if(std::strstr(name,"NativePalette")||std::strstr(name,"AuthorPalette"))return rt_cpu::api_palette;
    if(std::strstr(name,"IdentifyDraw")||std::strstr(name,"ReadAuthorSnapshot"))return rt_cpu::api_identify;
    if(std::strstr(name,"Surface")||std::strstr(name,"Geometry")||std::strstr(name,"AcquireDrawFrame")||std::strstr(name,"AcquireAuthorFrame"))return rt_cpu::api_surface;
    if(!std::strcmp(name,"forward_blas"))return rt_cpu::blas_callback;
    if(!std::strcmp(name,"forward_queue"))return rt_cpu::queue_callback;
    if(!std::strcmp(name,"execute"))return rt_cpu::execute_callback;
    return rt_cpu::other_callback;
}
struct RuntimeCall {
    Runtime &runtime;const char *name;
    rt_cpu::Scope api_measured,body_measured;
    // Declaration order also unlocks before COM release on construction failure.
    std::vector<ComPtr<ID3D12Resource>> release_outside_lock;
    std::unique_lock<RuntimeMutex> lock;
    RuntimeCall(Runtime &r,const char *n):runtime(r),name(n),lock(r.mutex,std::defer_lock){
        if(cpu_window)rt_cpu::observe(cpu_window(r.native_device_id));
        api_measured.begin(rt_cpu::phase.load(std::memory_order_relaxed)==1?cpu_callback_kind(name):rt_cpu::other_callback);
        rt_cpu::Scope wait_measured(rt_cpu::runtime_wait);
        r.trace.emit(name,"runtime_lock_wait",reinterpret_cast<std::uint64_t>(&r),r.mutex.owner());
        lock.lock();wait_measured.end();body_measured.begin(rt_cpu::runtime_body);r.trace.emit(name,"runtime_lock_acquired",reinterpret_cast<std::uint64_t>(&r),r.mutex.owner());r.drain_buffers(release_outside_lock);r.drain_draw_resets();
    }
    ~RuntimeCall(){
        runtime.drain_buffers(release_outside_lock);runtime.trace.emit(name,"runtime_call_end",reinterpret_cast<std::uint64_t>(&runtime));
        lock.unlock();release_outside_lock.clear();body_measured.end();api_measured.end();
    }
};
template<class Fn> void with_runtime(Runtime &r,const char *name,Fn &&fn,bool lifetime=false){{RuntimeCall call(r,name);if(r.active||lifetime)try{fn(r);}catch(const std::exception &e){if(++r.errors==1)reshade::log::message(reshade::log::level::error,e.what());r.active=false;r.release_frame();}}rt_cpu::finish("client");}
template<class Fn> void with(api::device *d,const char *name,Fn &&fn,bool lifetime=false){auto r=runtime(d);if(r)with_runtime(*r,name,std::forward<Fn>(fn),lifetime);}
struct DeviceInitializer {
    api::device *device=nullptr;
    std::shared_ptr<ProviderModule> provider;
    explicit DeviceInitializer(api::device *d){
        std::lock_guard lock(devices_mutex);
        if(!accepting_devices||devices.contains(d)||initializing_devices.contains(d))return;
        provider=provider_module;if(!provider)return;
        initializing_devices.insert(d);device=d;
    }
    ~DeviceInitializer(){if(device){std::lock_guard lock(devices_mutex);initializing_devices.erase(device);devices_idle.notify_all();}}
};
void init_device(api::device *d){
    if(d->get_api()!=api::device_api::d3d12)return;
    DeviceInitializer lease(d);if(!lease.provider)return;
    try{
        auto r=std::make_shared<Runtime>(d,lease.provider);
        {std::lock_guard lock(devices_mutex);if(accepting_devices){devices.emplace(d,r);return;}}
        // Uninit closed admission while this constructor was outside the lock.
        // Undo the actual frame lease/Reader before acknowledging its drain.
        RuntimeCall call(*r,__func__);r->stop();
    }catch(const std::exception &e){reshade::log::message(reshade::log::level::error,e.what());}
}
void destroy_device(api::device *d){
    auto r=runtime(d);if(!r)return;{std::lock_guard lock(devices_mutex);devices.erase(d);}
    {
        RuntimeCall call(*r,__func__);
        try{r->stop();}catch(const std::exception &e){
            reshade::log::message(reshade::log::level::error,e.what());
            std::lock_guard global(devices_mutex);
            if(!unfinished_gpu_module)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(self_module),&unfinished_gpu_module);
            unfinished.push_back(r);
        }
    }
    if(r->author_work.start)rt_phase_cpu::report("client",r->author_work.start,rt_phase_cpu::runtime_wait,rt_phase_cpu::descriptor_copy,true);
}
void present(api::command_queue *q,api::swapchain *,const api::rect *,const api::rect *,unsigned,const api::rect *){
    auto owner=runtime(q->get_device());if(!owner)return;NativeProfileRegistry::SnapshotRef published;NativeShapeEvidence::Batch shape_evidence;
    with_runtime(*owner,__func__,[&](Runtime &r){
        r.observe_author_work(draw_window?draw_window(r.native_device_id,nullptr):0);
        r.collect();r.refresh();published=r.native_profiles->snapshot();
        r.report_author_work(false);r.report_previous_palette();r.report_native_discovery();
        if(r.inputs.shape_evidence_enabled())shape_evidence=r.inputs.take_shape_evidence();
    });
    // No engine, Runtime or InputCollector lock is held during persistent IO.
    if(const auto issue=NativeShapeEvidence::write(std::move(shape_evidence));!issue.empty()){
        static std::atomic<bool> warned{false};if(!warned.exchange(true))reshade::log::message(reshade::log::level::warning,issue.c_str());
    }
    if(published)if(const auto issue=owner->native_archive.append(published);!issue.empty())reshade::log::message(reshade::log::level::warning,issue.c_str());
}

void init_resource(api::device *d,const api::resource_desc &desc,const api::subresource_data *,api::resource_usage,api::resource resource){
    if(desc.type!=api::resource_type::buffer)return;auto r=runtime(d);if(!r)return;
    r->buffer_events.created(resource,desc);r->trace.emit(__func__,"queued",resource.handle);
}
void destroy_resource(api::device *d,api::resource resource){
    auto r=runtime(d);if(!r)return;r->buffer_events.destroyed(resource);r->trace.emit(__func__,"queued",resource.handle);
}
void init_list(api::command_list *c){with(c->get_device(),__func__,[&](Runtime &r){r.inputs.init_list(c);});}
void reset_list(api::command_list *c){with(c->get_device(),__func__,[&](Runtime &r){r.reset_recording(c);},true);}
void destroy_list(api::command_list *c){with(c->get_device(),__func__,[&](Runtime &r){
    r.inputs.destroy_list(c);r.forget_recording(c);std::erase_if(r.pending_draw_resets,[&](const auto &p){return p.first==c;});
},true);}
void close_list(api::command_list *c){
    if(!(timed_close_lists.load(std::memory_order_acquire)&timed_list_bit(reinterpret_cast<std::uint64_t>(c))))return;
    if(auto r=runtime(c->get_device())){
        std::lock_guard lock(r->mutex);r->close_work_timers(c);
    }
}
void init_layout(api::device *d,unsigned count,const api::pipeline_layout_param *p,api::pipeline_layout l){with(d,__func__,[&](Runtime &r){
    r.material_chs.layout(material_chs_writer,l,count,p);
    r.inputs.init_layout(l,count,p);r.ray_scene.layout(l,count,p);
});}
void destroy_layout(api::device *d,api::pipeline_layout l){with(d,__func__,[&](Runtime &r){
    r.inputs.destroy_layout(l);r.ray_scene.destroy_layout(l);
    r.material_chs.destroy_layout(l);
});}
void init_pipeline(api::device *d,api::pipeline_layout l,unsigned n,const api::pipeline_subobject *s,api::pipeline p){with(d,__func__,[&](Runtime &r){
    r.material_chs.pipeline(material_chs_writer,r.native_device_id,p,l,n,s);
    r.inputs.init_pipeline(p,l,n,s);r.materials.init(p,n,s);
    if(r.ray_scene.enabled)r.ray_scene.init_pipeline(r.native_device_id,p,n,s,read_native_state_object);
});}
void destroy_pipeline(api::device *d,api::pipeline p){with(d,__func__,[&](Runtime &r){
    r.inputs.destroy_pipeline(p);r.materials.destroy(p);r.ray_scene.raygens.destroy(p);
    r.material_chs.destroy_pipeline(p);
});}
void bind_pipeline(api::command_list *c,api::pipeline_stage s,api::pipeline p){
    if(!(static_cast<unsigned>(s)&static_cast<unsigned>(api::pipeline_stage::compute_shader|api::pipeline_stage::ray_tracing_shader)))return;
    auto r=runtime(c->get_device());if(!r||!r->inputs.tracks_pipeline_bind(c,p))return;
    with_runtime(*r,__func__,[&](Runtime &v){v.inputs.bind_pipeline(c,s,p);});
}
void bind_tables(api::command_list *c,api::shader_stage s,api::pipeline_layout l,unsigned first,unsigned n,const api::descriptor_table *t){
    if(!(static_cast<unsigned>(s)&static_cast<unsigned>(api::shader_stage::all_compute)))return;
    auto r=runtime(c->get_device());if(!r||!r->inputs.tracks_table_bind(c,l,first,n))return;
    with_runtime(*r,__func__,[&](Runtime &v){v.inputs.bind_tables(c,s,l,first,n,t);});
}
void push_descriptors(api::command_list *c,api::shader_stage s,api::pipeline_layout l,unsigned p,const api::descriptor_table_update &u){
    if(!(static_cast<unsigned>(s)&static_cast<unsigned>(api::shader_stage::all_compute))||u.type!=api::descriptor_type::constant_buffer||u.binding||!u.count)return;
    auto r=runtime(c->get_device());if(!r||!r->inputs.tracks_cb_bind(c,l,p))return;
    with_runtime(*r,__func__,[&](Runtime &v){v.inputs.push_descriptors(c,s,l,p,u);});
}
bool copy_buffer(api::command_list *c,api::resource a,std::uint64_t ao,api::resource b,std::uint64_t bo,std::uint64_t n){
    with(c->get_device(),__func__,[&](Runtime &r){
        r.inputs.copy(c,a,ao,b,bo,n);r.note_occlusion_copy(c,a,ao,b,bo,n);
    });return false;
}
bool copy_resource(api::command_list *c,api::resource a,api::resource b){const auto desc=c->get_device()->get_resource_desc(b);if(desc.type==api::resource_type::buffer)copy_buffer(c,a,0,b,0,desc.buffer.size);return false;}
bool dispatch(api::command_list *c,unsigned,unsigned,unsigned){
    auto r=runtime(c->get_device());if(!r||!r->inputs.has_skin_dispatch(c))return false;
    if(!r->discovery_enabled)with_runtime(*r,__func__,[&](Runtime &v){const auto write=v.inputs.dispatch(c);if(write.resource)v.prepare_previous(c,write);});
    return false; // enabled v3 BEFORE owns the post-cancellation observation
}
void report_occlusion_bindings(Runtime &r,api::command_list *list,const RaySceneInput &ray,
    const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE &miss,unsigned width){
    if(!is_occlusion(ray)||!capture_occlusion_compute||!capture_occlusion_graphics||!read_occlusion_structured)return;
    const unsigned graph=ray.program->hit_group_bits;
    const auto graph_shift=8*graph; // two proofs may have different actual state objects
    if(r.occlusion_binding_reported&(1ull<<(graph_shift+OcclusionBindingRead::ready)))return;
    const auto epoch=r.inputs.recording(list);
    const auto seen=read_occlusion_bindings(r.ray_scene,r.inputs,list,epoch,ray,miss,capture_occlusion_graphics,
        capture_occlusion_compute,read_occlusion_structured);
    const auto bit=1ull<<(graph_shift+seen.stage);
    if(r.occlusion_binding_reported&bit)return;
    r.occlusion_binding_reported|=bit;
    static constexpr const char *stages[]{"layout","graphics","compute","rays","output","miss","ready"};
    char msg[1140];
    const auto &rays=seen.rays;const auto &output=seen.output;
    std::snprintf(msg,sizeof(msg),"RT-RAY-OCCLUSION-BINDINGS stage=%s rc=%d record=%llx/%llu/%llu state=%llx root=%llx hit_group_bits=%x known_no_procedural=%u width=%u p4_flags=%x p6_flags=%x gfx_level=%u gfx_tables=%u compute_records=%u rays=%llx/%llu first=%llu elements=%u write=%llu/%llu output=%llx/%llu first=%llu elements=%u write=%llu/%llu counter=%llx/%llu miss=%llx/%llu+%llu miss_heap=%u miss_upload=%u sbt=%s record_min=%u/%u",
        stages[seen.stage],seen.status,(unsigned long long)epoch.list,(unsigned long long)epoch.creation,(unsigned long long)epoch.generation,
        (unsigned long long)ray.compute.pipeline,(unsigned long long)ray.compute.root_signature,graph,unsigned(occlusion_known_no_procedural(ray)),width,
        seen.table_layout.p4_flags,seen.table_layout.p6_flags,seen.graphics_state.level,seen.graphics_state.record_count,
        seen.compute_state.record_count,(unsigned long long)rays.resource,(unsigned long long)rays.resource_creation,
        (unsigned long long)rays.first_element,rays.elements,(unsigned long long)rays.heap_generation,(unsigned long long)rays.write,
        (unsigned long long)output.resource,(unsigned long long)output.resource_creation,(unsigned long long)output.first_element,
        output.elements,(unsigned long long)output.heap_generation,(unsigned long long)output.write,
        (unsigned long long)output.counter,(unsigned long long)output.counter_creation,
        (unsigned long long)seen.miss.key.object,(unsigned long long)seen.miss.key.creation,
        (unsigned long long)seen.miss.offset,unsigned(seen.miss.heap),unsigned(seen.miss.upload_read),
        seen.closed_sbt?"original_closed_graph":seen.stage==OcclusionBindingRead::ready?"upload_prefix":"unavailable",
        seen.closed_sbt?seen.closed_sbt->raygen:0,seen.closed_sbt?seen.closed_sbt->miss:0);
    reshade::log::message(reshade::log::level::info,msg);
}
bool note_ray_consumer(api::command_list *c,api::resource rg,std::uint64_t rg_at,std::uint64_t rg_bytes,
    api::resource ms,std::uint64_t ms_at,std::uint64_t ms_bytes,std::uint64_t ms_stride,
    api::resource hg,std::uint64_t hg_at,std::uint64_t hg_bytes,std::uint64_t hg_stride,
    api::resource,std::uint64_t,std::uint64_t,std::uint64_t,unsigned width,unsigned,unsigned){
    if(!ray_input_window()||rg.handle||hg.handle)return false;
    with(c->get_device(),__func__,[&](Runtime &r){
        const D3D12_GPU_VIRTUAL_ADDRESS_RANGE raygen{rg_at,rg_bytes};
        const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE hit{hg_at,hg_bytes,hg_stride};
        if(auto seen=r.ray_scene.read(r.inputs,c,raygen,hit,compute_query,read_as_slot,read_ray_as_slot)){
            r.ray_scene.report(r.inputs,c,*seen,raygen,hit,r.material_chs.has_hit_program({seen->compute.pipeline}));
            const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE miss{
                ms.handle?0:ms_at,ms_bytes,ms_stride};
            if(seen->compute.ready)report_occlusion_bindings(r,c,*seen,miss,width);
            r.note_occlusion_instance_input(c,*seen);
        }
    });
    return false; // actual original DispatchRays stays intact
}
void execute(api::command_queue *q,api::command_list *c){with(c->get_device(),__func__,[&](Runtime &r){r.submit_before(q,c);},true);}
void forward_blas(std::uint64_t list,std::uint64_t,std::uint64_t desc,std::uint64_t){
    auto *c=reinterpret_cast<api::command_list *>(list);auto r=runtime(c->get_device());if(!r)return;
    const auto &build=*reinterpret_cast<const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *>(desc);
    NativeBlasInterest::Call interest(r->blas_interest,build.DestAccelerationStructureData,r->inputs.may_select_blas(build));
    if(!interest)return;
    with_runtime(*r,__func__,[&](Runtime &v){v.blas(c,build);});
}
std::uint64_t forward_instances(std::uint64_t list,std::uint64_t,std::uint64_t desc,std::uint64_t){std::uint64_t value=0;auto *c=reinterpret_cast<api::command_list *>(list);with(c->get_device(),__func__,[&](Runtime &r){value=r.instances(c,*reinterpret_cast<const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *>(desc));});return value;}
std::uint64_t observe_native_tlas(std::uint64_t list,std::uint64_t native_list,std::uint64_t desc,std::uint64_t){
    if(!ray_input_window())return 0;
    auto *c=reinterpret_cast<api::command_list *>(list);
    if(c->get_native()!=native_list)return 0;
    const auto &build=*reinterpret_cast<const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *>(desc);
    if(build.Inputs.Type!=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL||
       build.Inputs.DescsLayout!=D3D12_ELEMENTS_LAYOUT_ARRAY)return 0;
    // This forwarder runs after any SDK cancellation and immediately before
    // native Build. It never patches InstanceDescs or substitutes a TLAS.
    with(c->get_device(),__func__,[&](Runtime &r){
        if(r.ray_scene.enabled&&r.ray_scene.raygens.has_programs())
            if(const auto observed=r.ray_scene.build_candidate(r.inputs,c,build.DestAccelerationStructureData,
                build.Inputs.InstanceDescs,build.Inputs.NumDescs,unsigned(build.Inputs.Flags)))
                r.record_occlusion_instance_probe(c,*observed);
    });
    return 0;
}
void native_dispatch_receipt(const WWMI_RtNativeDispatch_v2 *event,std::uint64_t){
    if(!event||event->struct_size!=sizeof(*event)||event->abi_version!=2||!event->api_list||!event->native_list)return;
    const bool probe=event->api_list==instance_dispatch_watch.load(std::memory_order_relaxed)&&ray_input_window();
    struct Timing {std::uint64_t list=0,call=0,id=0;};
    static thread_local Timing timing;
    const bool finish=event->phase==WWMI_RT_DISPATCH_NATIVE_RECORDED&&
        timing.list==event->api_list&&timing.call==event->call_id&&timing.id;
    auto kind=GpuWorkKind::count;
    if(event->phase==WWMI_RT_DISPATCH_BEFORE_NATIVE&&(event->flags&WWMI_RT_DISPATCH_STATE_KNOWN)){
        if(const auto until=timed_until.load(std::memory_order_acquire)){
            const auto now=GetTickCount64();
            // Scan only a short slice of each 2s bin, even if a queue
            // class never issues an eligible native Dispatch.
            if(now<until&&(until-now)%AuthorWorkWindow::bin_ms>AuthorWorkWindow::bin_ms-250&&
                (now>=native_timing_due[0].load(std::memory_order_relaxed)||
                now>=native_timing_due[1].load(std::memory_order_relaxed))){
                const auto type=reinterpret_cast<ID3D12GraphicsCommandList *>(event->native_list)->GetType();
                if(type==D3D12_COMMAND_LIST_TYPE_DIRECT&&now>=native_timing_due[0].load(std::memory_order_relaxed))
                    kind=GpuWorkKind::native_direct;
                if(type==D3D12_COMMAND_LIST_TYPE_COMPUTE&&now>=native_timing_due[1].load(std::memory_order_relaxed))
                    kind=GpuWorkKind::native_compute;
            }
        }
    }
    auto *consumer=reinterpret_cast<api::command_list *>(event->api_list);
    if(consumer->get_native()!=event->native_list)return;
    auto native_owner=runtime(consumer->get_device());
    if(auto owner=native_owner;owner&&owner->inputs.shape_evidence_enabled())
        with_runtime(*owner,"passive_shape_dispatch",[&](Runtime &r){
            WWMI_ComputeStateInfo_v1 state{};state.size=sizeof(state);unsigned status=2;
            if(event->phase==WWMI_RT_DISPATCH_BEFORE_NATIVE&&(event->flags&WWMI_RT_DISPATCH_STATE_KNOWN)&&compute_query&&evidence_root_argument){
                status=compute_query(event->api_list,&state);
                if(!status&&(state.pipeline_kind||state.pipeline!=event->pipeline||state.root_signature!=event->layout))status=2;
            }
            r.inputs.shape_evidence_dispatch(consumer,*event,[&](unsigned parameter){
                InputCollector::EvidenceRoot result;result.status=status;
                if(!status){WWMI_ComputeRootArgument_v1 argument{};argument.size=sizeof(argument);
                    result.status=evidence_root_argument(event->api_list,&state,parameter,&argument);
                    result.kind=argument.kind;result.address=argument.address;result.epoch=argument.epoch;
                }return result;
            });
        });
    if(auto owner=native_owner;owner&&owner->discovery_enabled&&owner->inputs.has_skin_dispatch(consumer))
        with_runtime(*owner,"native_skin_receipt",[&](Runtime &r){
            if(event->phase==WWMI_RT_DISPATCH_BEFORE_NATIVE){
                r.native_skin_state_unknown+=!(event->flags&WWMI_RT_DISPATCH_STATE_KNOWN);
                r.native_skin_unpredicated_missing+=!(event->flags&WWMI_RT_DISPATCH_UNPREDICATED);
                const bool authorized=(event->flags&(WWMI_RT_DISPATCH_STATE_KNOWN|WWMI_RT_DISPATCH_UNPREDICATED))==(WWMI_RT_DISPATCH_STATE_KNOWN|WWMI_RT_DISPATCH_UNPREDICATED);
                const auto write=r.inputs.dispatch(consumer,authorized);
                if(authorized)r.issue_native_requests(consumer,NativeDiscovery::ReadPoint::rest_consumer);
                if(write.resource)r.prepare_previous(consumer,write);
            }else r.inputs.native_skin_forwarded(consumer,event->phase==WWMI_RT_DISPATCH_NATIVE_RECORDED);
        });
    if(!probe&&!finish&&kind==GpuWorkKind::count)return;
    auto *c=reinterpret_cast<api::command_list *>(event->api_list);
    if(c->get_native()!=event->native_list)return;
    if(probe)with(c->get_device(),__func__,[&](Runtime &r){r.note_occlusion_native_dispatch(c,*event);});
    if((finish||kind!=GpuWorkKind::count)&&c->get_device()->get_native()==timed_device.load(std::memory_order_relaxed))
        if(auto r=runtime(c->get_device())){
            std::lock_guard lock(r->mutex);
            if(finish){r->end_native_timer(*event,timing.id);timing={};}
            else if(r->active){
                if(const auto id=r->start_native_timer(c,*event,kind))timing={event->api_list,event->call_id,id};
            }
        }
}
void native_buffer_receipt(const WWMI_RtNativeBuffer_v3 *event,std::uint64_t){
    if(!event||event->size!=sizeof(*event)||event->version!=3||!event->api_list||!event->native_list)return;
    auto *list=reinterpret_cast<api::command_list *>(event->api_list);if(list->get_native()!=event->native_list)return;
    auto owner=runtime(list->get_device());if(!owner||!owner->discovery_enabled)return;
    if(event->kind==WWMI_RT_BUFFER_BLAS){
        if(!event->build_desc||!owner->inputs.discovery_interested())return;
        const auto &desc=*reinterpret_cast<const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *>(event->build_desc);
        if(!owner->inputs.may_select_blas(desc))return;
        with_runtime(*owner,"native_blas_receipt",[&](Runtime &r){
            r.inputs.native_blas(list,desc,event->phase==WWMI_RT_BUFFER_NATIVE_RECORDED);
            if(event->phase==WWMI_RT_BUFFER_BEFORE_NATIVE)r.issue_native_requests(list,NativeDiscovery::ReadPoint::index_consumer);
        });return;
    }
    with_runtime(*owner,"native_buffer_receipt",[&](Runtime &r){
        if(event->kind==WWMI_RT_BUFFER_COPY)r.inputs.native_copy(list,api::resource{event->source},event->source_offset,api::resource{event->destination},event->destination_offset,event->bytes,event->phase==WWMI_RT_BUFFER_NATIVE_RECORDED);
        else if(event->kind==WWMI_RT_BUFFER_WRITE)r.inputs.native_written(api::resource{event->destination},event->destination_offset,event->bytes);
        else if(event->kind==WWMI_RT_BUFFER_UNKNOWN)r.inputs.native_unknown();
    });
}
std::uint64_t observe_native_raygen(std::uint64_t list,std::uint64_t native_list,std::uint64_t desc,std::uint64_t){
    if(!ray_input_window())return 0;
    auto *c=reinterpret_cast<api::command_list *>(list);
    if(!c||!desc||c->get_native()!=native_list)return 0;
    const auto r=runtime(c->get_device());
    if(!r||r->raygen_probe_taken.load(std::memory_order_relaxed))return 0;
    with_runtime(*r,__func__,[&](Runtime &v){
        v.record_raygen_probe(c,native_list,*reinterpret_cast<const D3D12_DISPATCH_RAYS_DESC *>(desc));
    });
    return 0; // native call and all original SBT fields are unmodified
}
std::uint64_t forward_hit(std::uint64_t list,std::uint64_t,std::uint64_t desc,std::uint64_t){std::uint64_t value=0;auto *c=reinterpret_cast<api::command_list *>(list);with(c->get_device(),__func__,[&](Runtime &r){value=r.hit_table(c,*reinterpret_cast<const D3D12_DISPATCH_RAYS_DESC *>(desc));});return value;}
void forward_queue(std::uint64_t queue,std::uint64_t native,std::uint64_t list,std::uint64_t){auto *c=reinterpret_cast<api::command_list *>(list);with(c->get_device(),__func__,[&](Runtime &r){r.submit_after(queue,reinterpret_cast<ID3D12CommandQueue *>(native),c);},true);}
#define EVENTS(X) \
 X(present,present) X(init_device,init_device) X(destroy_device,destroy_device) X(init_resource,init_resource) X(destroy_resource,destroy_resource) \
 X(init_command_list,init_list) X(close_command_list,close_list) X(reset_command_list,reset_list) X(destroy_command_list,destroy_list) \
 X(init_pipeline_layout,init_layout) X(destroy_pipeline_layout,destroy_layout) X(init_pipeline,init_pipeline) X(destroy_pipeline,destroy_pipeline) \
 X(bind_pipeline,bind_pipeline) X(bind_descriptor_tables,bind_tables) X(push_descriptors,push_descriptors) \
 X(copy_buffer_region,copy_buffer) X(copy_resource,copy_resource) X(dispatch,dispatch) X(execute_command_list,execute) \
 X(dispatch_rays,note_ray_consumer)
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackIdentifyDraw_v1(std::uint64_t device,std::uint64_t list,std::uint64_t base,
    std::uint32_t first,std::uint32_t count,WWMI_RtPackDrawIdentity_v1 *out,std::uint32_t out_size,void *palette,std::uint32_t capacity){
    if(!out||out_size!=sizeof(*out)||!list)return 0;*out={};
    std::shared_ptr<Runtime> found;{std::lock_guard lock(devices_mutex);for(const auto &[d,r]:devices)if(d->get_native()==device){found=r;break;}}
    if(!found)return 0;RuntimeCall call(*found,__func__);if(!found->active||!found->frame_device)return 0;
    if(!found->inputs.identify_draw(reinterpret_cast<api::command_list *>(list),base,first,count,*out,palette,capacity))return 0;
    if(mod_detected)mod_detected(out->object_guid);return 1;
}
static std::uint32_t identify_draw(const WWMI_RtPackDrawQuery_v2 *query,std::uint32_t query_size,
    WWMI_RtPackDrawIdentity_v2 *out,std::uint32_t out_size,void *palette,std::uint32_t capacity,
    WWMI_RtDrawIdentityReceipt_v1 *receipt,const char *operation){
    if(receipt)receipt->reason=WWMI_RT_IDENTIFY_QUERY;
    if(!query||query_size!=sizeof(*query)||query->struct_size!=sizeof(*query)||query->abi_version!=2||!out||out_size!=sizeof(*out))return 0;*out={};
    std::shared_ptr<Runtime> found;{std::lock_guard lock(devices_mutex);for(const auto &[d,r]:devices)if(d->get_native()==query->device){found=r;break;}}
    if(!found){if(receipt)receipt->reason=WWMI_RT_IDENTIFY_DEVICE;return 0;}
    RuntimeCall call(*found,operation);
    if(!found->active||!found->frame_device){if(receipt)receipt->reason=WWMI_RT_IDENTIFY_INACTIVE;return 0;}
    const bool raw=found->native_draw_candidate(*query);
    if(!raw&&!receipt)return 0;
    if(raw)found->collect(); // actual producer-fence completion, not a remembered queue hint
    if(!found->inputs.identify_draw_v2(*query,*out,palette,capacity,nullptr,receipt))return 0;
    if(mod_detected)mod_detected(out->native.object_guid);return 1;
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackIdentifyDraw_v2(const WWMI_RtPackDrawQuery_v2 *query,std::uint32_t query_size,
    WWMI_RtPackDrawIdentity_v2 *out,std::uint32_t out_size,void *palette,std::uint32_t capacity){
    return identify_draw(query,query_size,out,out_size,palette,capacity,nullptr,__func__);
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackIdentifyDrawReceipt_v1(const WWMI_RtPackDrawQuery_v2 *query,std::uint32_t query_size,
    WWMI_RtPackDrawIdentity_v2 *out,std::uint32_t out_size,WWMI_RtDrawIdentityReceipt_v1 *receipt,std::uint32_t receipt_size){
    if(!receipt||receipt_size!=sizeof(*receipt))return 0;
    *receipt={};receipt->struct_size=sizeof(*receipt);receipt->abi_version=1;
    return identify_draw(query,query_size,out,out_size,nullptr,0,receipt,__func__);
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackAcquireGraphicsGeometry_v1(const WWMI_RtGraphicsGeometryRequest_v1 *request,std::uint32_t request_size,
    WWMI_RtGraphicsGeometry_v1 *out,std::uint32_t out_size){
    if(!out||out_size!=sizeof(*out))return WWMI_RT_GEOMETRY_BAD_INPUT;*out={};
    if(!request||request_size!=sizeof(*request)||request->struct_size!=sizeof(*request)||request->abi_version!=1||
        request->native_draw.struct_size!=sizeof(request->native_draw)||request->native_draw.abi_version!=2||
        !request->section_token||!request->load_generation||!request->mod_key||!request->mod_revision||!request->mod_index_count)return WWMI_RT_GEOMETRY_BAD_INPUT;
    std::shared_ptr<Runtime> found;{std::lock_guard lock(devices_mutex);for(const auto &[d,r]:devices)if(d->get_native()==request->native_draw.device){found=r;break;}}
    if(!found)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
    RuntimeCall call(*found,__func__);if(!found->active||!found->frame_device)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
    try{return found->acquire_graphics_geometry(*request,*out);}catch(const std::exception &e){
        if(++found->errors==1)reshade::log::message(reshade::log::level::error,e.what());found->active=false;found->release_frame();return WWMI_RT_GEOMETRY_INTERNAL_ERROR;
    }
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackAcquirePlannedGeometry_v1(const WWMI_RtPlannedGeometryRequest_v1 *request,std::uint32_t request_size,
    WWMI_RtGraphicsGeometry_v1 *out,std::uint32_t out_size){
    if(!out||out_size!=sizeof(*out))return WWMI_RT_GEOMETRY_BAD_INPUT;*out={};
    if(!request||request_size!=sizeof(*request)||!request->frame_id||request->reserved||
        (request->path_flags&~std::uint32_t(WWMI_RT_PLAN_MOD_AUTHOR_SHARED|WWMI_RT_PLAN_EXTRA_POSITION_INPUT)))return WWMI_RT_GEOMETRY_BAD_INPUT;const auto &draw=request->draw;
    if(draw.struct_size!=sizeof(draw)||draw.abi_version!=1||draw.native_draw.struct_size!=sizeof(draw.native_draw)||draw.native_draw.abi_version!=2||
        !draw.section_token||!draw.load_generation||!draw.mod_key||!draw.mod_revision||!draw.mod_index_count)return WWMI_RT_GEOMETRY_BAD_INPUT;
    std::shared_ptr<Runtime> found;{std::lock_guard lock(devices_mutex);for(const auto &[d,r]:devices)if(d->get_native()==draw.native_draw.device){found=r;break;}}
    if(!found)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
    RuntimeCall call(*found,__func__);if(!found->active||!found->frame_device)return WWMI_RT_GEOMETRY_BATCH_NOT_READY;
    try{return found->acquire_graphics_geometry(draw,*out,request->frame_id,request->path_flags);}catch(const std::exception &e){
        if(++found->errors==1)reshade::log::message(reshade::log::level::error,e.what());found->active=false;found->release_frame();return WWMI_RT_GEOMETRY_INTERNAL_ERROR;
    }
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackAcquireNativePalette_v1(const WWMI_RtPackDrawQuery_v2 *query,std::uint32_t query_size,
    std::uint32_t expected_component,WWMI_RtNativePalette_v1 *out,std::uint32_t out_size){
    if(!out||out_size!=sizeof(*out))return WWMI_RT_PALETTE_BAD_INPUT;*out={};
    if(!query||query_size!=sizeof(*query)||query->struct_size!=sizeof(*query)||query->abi_version!=2||!query->device||!query->list)return WWMI_RT_PALETTE_BAD_INPUT;
    std::shared_ptr<Runtime> found;{std::lock_guard lock(devices_mutex);for(const auto &[d,r]:devices)if(d->get_native()==query->device){found=r;break;}}
    if(!found)return WWMI_RT_PALETTE_NOT_READY;
    RuntimeCall call(*found,__func__);if(!found->active||!found->frame_device)return WWMI_RT_PALETTE_NOT_READY;
    try{return found->acquire_native_palette(*query,expected_component,*out);}catch(const std::exception &e){
        if(++found->errors==1)reshade::log::message(reshade::log::level::error,e.what());found->active=false;found->release_frame();return WWMI_RT_PALETTE_INTERNAL_ERROR;
    }
}
static bool valid_surface_request(const WWMI_RtLocalVfRequest_v1 &r,bool empty_author_selection=false){
    const auto &p=r.planned;const auto &d=p.draw;return r.current_frame&&r.current_frame>=p.frame_id&&!(r.flags&~std::uint32_t(WWMI_RT_SURFACE_NEED_COLOR))&&!r.reserved&&p.frame_id&&!p.reserved&&
        !(p.path_flags&~std::uint32_t(WWMI_RT_PLAN_MOD_AUTHOR_SHARED|WWMI_RT_PLAN_EXTRA_POSITION_INPUT))&&d.struct_size==sizeof(d)&&d.abi_version==1&&d.section_token&&d.load_generation&&d.mod_key&&d.mod_revision&&(d.mod_index_count||empty_author_selection)&&
        d.native_draw.struct_size==sizeof(d.native_draw)&&d.native_draw.abi_version==2;
}
static std::shared_ptr<Runtime> hybrid_runtime(std::uint64_t device){
    std::lock_guard lock(devices_mutex);
    for(const auto &[_,r]:devices)if(r->native_device_id==device)return r;
    return {};
}
extern "C" __declspec(dllexport) bool WWMI_RtPackProbeHybridRaster_v1(const WWMI_RtPackDrawQuery_v2 *q,std::uint64_t ticket,std::uint64_t load,std::uint64_t mod,const wwmi::hybrid::HistoryRead *previous){
    if(!q)return false;auto r=hybrid_runtime(q->device);if(!r)return false;
    RuntimeCall call(*r,__func__);
    if(!r->active||r->frame_device!=q->device||!r->hybrid_probe(*q,ticket,load,mod))return false;
    if(previous)r->hybrid_previous(*q,ticket,*previous);
    return true;
}
extern "C" __declspec(dllexport) wwmi::hybrid::Result WWMI_RtPackAcquireHybridRaster_v1(const wwmi::hybrid::Request *q,wwmi::hybrid::Frame *f){
    using namespace wwmi::hybrid;
    if(!f)return unavailable;*f={};if(!q)return unavailable;
    auto r=hybrid_runtime(q->native.device);if(!r)return unavailable;
    RuntimeCall call(*r,__func__);if(!r->active||r->frame_device!=q->native.device)return unavailable;
    try{return r->acquire_hybrid(*q,*f);}catch(const std::exception &){return unavailable;}
}
extern "C" __declspec(dllexport) bool WWMI_RtPackCommitHybridRaster_v1(std::uint64_t device,std::uint64_t list,std::uint64_t ticket,std::uint64_t transaction){
    auto r=hybrid_runtime(device);if(!r)return false;
    RuntimeCall call(*r,__func__);if(!r->active||r->frame_device!=device)return false;
    try{return r->commit_hybrid(list,ticket,transaction);}catch(const std::exception &){return false;}
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackAcquireLocalVfSurface_v1(const WWMI_RtLocalVfRequest_v1 *request,std::uint32_t request_size,WWMI_RtLocalVfSurface_v1 *out,std::uint32_t out_size){
    if(!out||out_size!=sizeof(*out))return WWMI_RT_SURFACE_BAD_INPUT;*out={};if(!request||request_size!=sizeof(*request)||!valid_surface_request(*request))return WWMI_RT_SURFACE_BAD_INPUT;
    std::shared_ptr<Runtime> found;{std::lock_guard lock(devices_mutex);for(const auto &[d,r]:devices)if(d->get_native()==request->planned.draw.native_draw.device){found=r;break;}}
    if(!found)return WWMI_RT_SURFACE_NOT_READY;RuntimeCall call(*found,__func__);if(!found->active||!found->frame_device)return WWMI_RT_SURFACE_NOT_READY;
    try{return found->acquire_surface(*request,*out);}catch(const std::exception &e){if(++found->errors==1)reshade::log::message(reshade::log::level::error,e.what());found->active=false;found->release_frame();return WWMI_RT_SURFACE_INTERNAL_ERROR;}
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackAcquirePreviousSurface_v1(const WWMI_RtPreviousSurfaceRequest_v1 *request,std::uint32_t request_size,WWMI_RtPreviousSurface_v1 *out,std::uint32_t out_size){
    if(!out||out_size!=sizeof(*out))return WWMI_RT_SURFACE_BAD_INPUT;*out={};if(!request||request_size!=sizeof(*request)||!valid_surface_request(request->current))return WWMI_RT_SURFACE_BAD_INPUT;
    std::shared_ptr<Runtime> found;{std::lock_guard lock(devices_mutex);for(const auto &[d,r]:devices)if(d->get_native()==request->current.planned.draw.native_draw.device){found=r;break;}}
    if(!found)return WWMI_RT_SURFACE_NOT_READY;RuntimeCall call(*found,__func__);if(!found->active||!found->frame_device)return WWMI_RT_SURFACE_NOT_READY;
    try{return found->acquire_previous(*request,*out);}catch(const std::exception &e){if(++found->errors==1)reshade::log::message(reshade::log::level::error,e.what());found->active=false;found->release_frame();return WWMI_RT_SURFACE_INTERNAL_ERROR;}
}
static std::uint32_t acquire_draw_frame(const WWMI_RtDrawFrameRequest_v1 *request,std::uint32_t request_size,WWMI_RtDrawFrame_v1 *out,std::uint32_t out_size,
    const WWMI_RtPreviousDrawScope_v1 *scope,const char *operation){
    if(!out||out_size!=sizeof(*out))return WWMI_RT_SURFACE_BAD_INPUT;*out={};out->failed_phase=WWMI_RT_DRAW_CURRENT;
    if(!request||request_size!=sizeof(*request)||request->struct_size!=sizeof(*request)||request->abi_version!=1||request->reserved||
       (request->flags&~std::uint32_t(WWMI_RT_DRAW_MATERIAL|WWMI_RT_DRAW_PREVIOUS))||
       ((request->flags&WWMI_RT_DRAW_PREVIOUS)&&!(request->flags&WWMI_RT_DRAW_MATERIAL))||
       !valid_surface_request(request->inputs.current)||!request->source.list_creation||!request->source.recording||
       !request->source.blas_resource||!request->source.blas_creation||!request->source.blas_va||!request->source.pose_order)
        return WWMI_RT_SURFACE_BAD_INPUT;
    const auto device=request->inputs.current.planned.draw.native_draw.device;
    std::shared_ptr<Runtime> found;{std::lock_guard lock(devices_mutex);for(const auto &[_,r]:devices)if(r->native_device_id==device){found=r;break;}}
    if(!found)return WWMI_RT_SURFACE_NOT_READY;RuntimeCall call(*found,operation);if(!found->active||found->frame_device!=device)return WWMI_RT_SURFACE_NOT_READY;
    try{return found->acquire_draw_frame(*request,*out,scope);}catch(const std::exception &e){if(++found->errors==1)reshade::log::message(reshade::log::level::error,e.what());found->active=false;found->release_frame();return WWMI_RT_SURFACE_INTERNAL_ERROR;}
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackAcquireDrawFrame_v1(const WWMI_RtDrawFrameRequest_v1 *request,std::uint32_t request_size,WWMI_RtDrawFrame_v1 *out,std::uint32_t out_size){
    return acquire_draw_frame(request,request_size,out,out_size,nullptr,__func__);
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackAcquireDrawFrame_v2(const WWMI_RtDrawFrameRequest_v2 *request,std::uint32_t request_size,WWMI_RtDrawFrame_v1 *out,std::uint32_t out_size){
    if(!request||request_size!=sizeof(*request))return acquire_draw_frame(nullptr,0,out,out_size,nullptr,__func__);
    return acquire_draw_frame(&request->base,sizeof(request->base),out,out_size,&request->previous_scope,__func__);
}
static bool valid_author_query(const WWMI_RtPackDrawQuery_v2 *q,std::uint32_t size){
    return q&&size==sizeof(*q)&&q->struct_size==sizeof(*q)&&q->abi_version==2&&q->device&&q->list;
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackReadAuthorSnapshot_v1(const WWMI_RtPackDrawQuery_v2 *query,std::uint32_t query_size,
    WWMI_RtAuthorSnapshot_v1 *out,std::uint32_t out_size,WWMI_RtDrawIdentityReceipt_v1 *receipt,std::uint32_t receipt_size){
    if(!out||out_size!=sizeof(*out)||(receipt&&receipt_size!=sizeof(*receipt)))return 0;
    *out={};if(receipt){*receipt={};receipt->struct_size=sizeof(*receipt);receipt->abi_version=1;receipt->reason=WWMI_RT_IDENTIFY_QUERY;}
    if(!valid_author_query(query,query_size))return 0;
    auto r=runtime_native(query->device);if(!r){if(receipt)receipt->reason=WWMI_RT_IDENTIFY_DEVICE;return 0;}
    RuntimeCall call(*r,__func__);
    if(!r->active||!r->frame_device||!r->author_path){if(receipt)receipt->reason=WWMI_RT_IDENTIFY_INACTIVE;return 0;}
    try{return r->capture_author_draw(*query,*out,receipt);}catch(const std::exception &){return 0;}
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackAcquireAuthorPalette_v1(const WWMI_RtPackDrawQuery_v2 *query,std::uint32_t query_size,
    std::uint64_t snapshot,std::uint32_t component,WWMI_RtNativePalette_v1 *out,std::uint32_t out_size){
    if(!out||out_size!=sizeof(*out))return WWMI_RT_PALETTE_BAD_INPUT;*out={};
    if(!valid_author_query(query,query_size)||!snapshot)return WWMI_RT_PALETTE_BAD_INPUT;
    auto r=runtime_native(query->device);if(!r)return WWMI_RT_PALETTE_NOT_READY;
    RuntimeCall call(*r,__func__);if(!r->active||!r->frame_device||!r->author_path)return WWMI_RT_PALETTE_NOT_READY;
    try{return r->acquire_native_palette(*query,component,*out,snapshot);}catch(const std::exception &){return WWMI_RT_PALETTE_INTERNAL_ERROR;}
}
static std::uint32_t acquire_author_frame(const WWMI_RtAuthorFrameRequest_v1 *request,std::uint32_t request_size,
    WWMI_RtAuthorFrame_v1 *out,std::uint32_t out_size,const WWMI_RtDeformedSelection_v1 *deformed,const char *operation,
    const WWMI_RtAuthorSectionParts_v3 *section=nullptr){
    if(!out||out_size!=sizeof(*out))return WWMI_RT_SURFACE_BAD_INPUT;*out={};
    if(!request||request_size!=sizeof(*request)||!request->plan)return WWMI_RT_SURFACE_BAD_INPUT;
    if(section&&(!deformed||!section->draws||!section->indices||request->subdraw_index||request->subdraw_count<2))
        return WWMI_RT_SURFACE_BAD_INPUT;
    const auto &v=request->draw.base;const auto &p=v.inputs.current.planned;
    if(v.struct_size!=sizeof(v)||v.abi_version!=1||v.reserved||
       (v.flags&~std::uint32_t(WWMI_RT_DRAW_MATERIAL|WWMI_RT_DRAW_PREVIOUS))||
       ((v.flags&WWMI_RT_DRAW_PREVIOUS)&&!(v.flags&WWMI_RT_DRAW_MATERIAL))||
       !valid_surface_request(v.inputs.current,!request->subdraw_count)||
       p.path_flags!=WWMI_RT_PLAN_MOD_AUTHOR_SHARED||
       (!request->subdraw_count&&(request->subdraw_index||p.draw.mod_index_count)))return WWMI_RT_SURFACE_BAD_INPUT;
    auto r=runtime_native(p.draw.native_draw.device);if(!r)return WWMI_RT_SURFACE_NOT_READY;
    RuntimeCall call(*r,operation);if(!r->active||!r->frame_device||!r->author_path)return WWMI_RT_SURFACE_NOT_READY;
    try{
        AuthorSectionLease lease;
        const auto status=r->acquire_draw_frame(v,out->frame,&request->draw.previous_scope,request,out,deformed,
            section,section?&lease:nullptr);
        if(deformed)r->seal_author_acquisition(*request,*out,status,section?request->subdraw_count:1,std::move(lease));
        return status;
    }
    catch(const std::exception &){return WWMI_RT_SURFACE_INTERNAL_ERROR;}
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackAcquireAuthorFrame_v1(const WWMI_RtAuthorFrameRequest_v1 *request,std::uint32_t request_size,
    WWMI_RtAuthorFrame_v1 *out,std::uint32_t out_size){
    return acquire_author_frame(request,request_size,out,out_size,nullptr,__func__);
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackAcquireAuthorFrame_v2(const WWMI_RtAuthorDeformedFrameRequest_v2 *request,std::uint32_t request_size,
    WWMI_RtAuthorFrame_v1 *out,std::uint32_t out_size){
    if(request&&request_size==sizeof(WWMI_RtAuthorSectionFrameRequest_v3)){
        const auto &section=*reinterpret_cast<const WWMI_RtAuthorSectionFrameRequest_v3 *>(request);
        return acquire_author_frame(&section.first.base,sizeof(section.first.base),out,out_size,&section.first.selected,__func__,&section.parts);
    }
    if(!request||request_size!=sizeof(*request))return acquire_author_frame(nullptr,0,out,out_size,nullptr,__func__);
    return acquire_author_frame(&request->base,sizeof(request->base),out,out_size,&request->selected,__func__);
}
static std::uint32_t begin_author_scope(const WWMI_RtAuthorScopeRequest_v1 *request,std::uint32_t request_size,
    WWMI_RtAuthorScope_v1 *out,std::uint32_t out_size,WWMI_RtDeformedSelection_v1 *source,const char *operation){
    if(!out||out_size!=sizeof(*out))return 0;*out={};
    if(!request||request_size!=sizeof(*request)||!request->snapshot||!request->instance.resource||
       !request->mod_key||!request->load_generation||!valid_author_query(&request->native_draw,sizeof(request->native_draw)))return 0;
    auto r=runtime_native(request->native_draw.device);if(!r)return 0;
    RuntimeCall call(*r,operation);if(!r->active||!r->frame_device||!r->author_path||(source&&!mod_acquire_deformed))return 0;
    try{return r->begin_author_scope(*request,*out,source);}catch(const std::exception &){return 0;}
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackBeginAuthorScope_v1(const WWMI_RtAuthorScopeRequest_v1 *request,std::uint32_t request_size,
    WWMI_RtAuthorScope_v1 *out,std::uint32_t out_size){
    return begin_author_scope(request,request_size,out,out_size,nullptr,__func__);
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackBeginAuthorScope_v2(const WWMI_RtAuthorScopeRequest_v1 *request,std::uint32_t request_size,
    WWMI_RtAuthorDeformedScope_v2 *out,std::uint32_t out_size){
    if(!out||out_size!=sizeof(*out))return 0;*out={};
    return begin_author_scope(request,request_size,&out->base,sizeof(out->base),&out->owner,__func__);
}
static std::uint32_t commit_author_draw(const WWMI_RtAuthorCommit_v1 *request,std::uint32_t request_size,
    const RtAuthorCommitNote* note){
    if(!request||request_size!=sizeof(*request)||!request->device||!request->list||!request->creation||!request->recording||!request->plan||
       request->empty_selection>1||request->reserved)return 0;
    auto r=runtime_native(request->device);if(!r)return 0;
    RuntimeCall call(*r,"WWMI_RtPackCommitAuthorDraw_v1");if(!r->active||!r->frame_device||!r->author_path)return 0;
    try{return r->commit_author(*request,note);}catch(const std::exception &){return 0;}
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackCommitAuthorDraw_v1(const WWMI_RtAuthorCommit_v1 *request,std::uint32_t request_size){
    return commit_author_draw(request,request_size,nullptr);
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackCommitAuthorDrawNote_v1(const RtAuthorCommitNote *note,std::uint32_t size){
    if(!note||size!=sizeof(*note))return 0;
    return commit_author_draw(&note->choice,sizeof(note->choice),note);
}
extern "C" __declspec(dllexport) std::uint64_t WWMI_RtPackObserveAuthorSection_v1(const WWMI_RtAuthorCommit_v1 *request,
    std::uint32_t size,std::uint64_t deadline,std::uint64_t frame,std::uint32_t phase,std::uint64_t timer,
    std::uint32_t parts,std::uint64_t indices){
    if(!request||size!=sizeof(*request)||!request->device||!request->list||!deadline||!frame)return 0;
    try{
        const auto r=runtime_native(request->device);if(!r)return 0;
        std::lock_guard lock(r->mutex);
        if(!r->active||!r->frame_device||!r->author_path)return 0;
        return r->observe_section_work(*request,deadline,frame,phase,timer,parts,indices);
    }catch(const std::exception &){return 0;}
}
static std::uint32_t reserve_draw_binding(const WWMI_RtDrawBindingRequest_v1 *request,std::uint32_t request_size,WWMI_RtDrawBinding_v1 *out,std::uint32_t out_size,
    const void *const *no_history_cb,std::uint64_t *no_history_va,const char *operation){
    if(!out||out_size!=sizeof(*out))return WWMI_RT_SURFACE_BAD_INPUT;*out={};
    if(!request||request_size!=sizeof(*request)||request->struct_size!=sizeof(*request)||request->abi_version!=1||
       !request->device||!request->list||!request->list_creation||!request->recording||!request->batch_token||
       !request->descriptor_count||request->descriptor_count>64u*1024u||
       request->resource_count>128u+2u*request->descriptor_count||
       (request->resource_count&&!request->resources))return WWMI_RT_SURFACE_BAD_INPUT;
    std::shared_ptr<Runtime> found;{std::lock_guard lock(devices_mutex);for(const auto &[_,r]:devices)if(r->native_device_id==request->device){found=r;break;}}
    if(!found)return WWMI_RT_SURFACE_NOT_READY;RuntimeCall call(*found,operation);if(!found->active||found->frame_device!=request->device)return WWMI_RT_SURFACE_NOT_READY;
    try{return found->reserve_draw_binding(*request,*out,no_history_cb,no_history_va);}catch(const std::exception &){return WWMI_RT_SURFACE_INTERNAL_ERROR;}
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackReserveDrawBinding_v1(const WWMI_RtDrawBindingRequest_v1 *request,std::uint32_t request_size,WWMI_RtDrawBinding_v1 *out,std::uint32_t out_size){
    return reserve_draw_binding(request,request_size,out,out_size,nullptr,nullptr,__func__);
}
extern "C" __declspec(dllexport) std::uint32_t WWMI_RtPackReserveDrawBinding_v2(const WWMI_RtDrawBindingRequest_v2 *request,std::uint32_t request_size,WWMI_RtDrawBinding_v2 *out,std::uint32_t out_size){
    if(!out||out_size!=sizeof(*out))return WWMI_RT_SURFACE_BAD_INPUT;*out={};
    if(!request||request_size!=sizeof(*request))return WWMI_RT_SURFACE_BAD_INPUT;
    return reserve_draw_binding(&request->base,sizeof(request->base),&out->base,sizeof(out->base),
                                request->no_history_cb,out->no_history_va,__func__);
}
extern "C" __declspec(dllexport) void WWMI_RtPackNoteDrawReset_v1(std::uint64_t native_list,std::int32_t hresult){
    auto binding=draw_reset_receipt.lock();if(!binding||binding->native_list!=native_list)return;
    draw_reset_receipt.reset();
    binding->reset.store(hresult>=0?DrawBindingRecording::succeeded:DrawBindingRecording::failed,std::memory_order_release);
}
extern "C" __declspec(dllexport) const char *NAME="WWMI data-driven RT Mod consumer";
extern "C" __declspec(dllexport) const char *DESCRIPTION="Consumes parsed-engine Mod packs, recording-bound pose/final positions, AS and hit tables.";
static void clear_provider_exports(){
    cpu_window=nullptr;draw_window=nullptr;descriptor_lease=nullptr;descriptor_source=nullptr;mod_geometry_section=nullptr;mod_section_identity=nullptr;
    mod_catalog=nullptr;mod_resource=nullptr;mod_position=nullptr;mod_stamp=nullptr;mod_detected=nullptr;set_frame_device=nullptr;
    cloth_previous_trace_seq=nullptr;
    author_consumer=nullptr;native_discovery_available=false;
    mod_author_section=nullptr;mod_author_revision=nullptr;shape_position=nullptr;mod_acquire_deformed=nullptr;
}
extern "C" __declspec(dllexport) bool AddonInit(HMODULE module,HMODULE reshade_module){
    self_module=module;auto host=reshade_module?reshade_module:GetModuleHandleW(L"dxgi.dll");
    auto provider=std::make_shared<ProviderModule>();
    // This acquires the reference atomically with lookup, before resolving or
    // invoking any provider export. Every failure below releases this owner.
    if(!GetModuleHandleExW(0,L"wwmi_dx12.addon64",&provider->module))return false;
    auto main=provider->module;
    author_consumer=reinterpret_cast<AuthorConsumerFn>(GetProcAddress(main,"WWMI_RtAuthorPlanConsumer_v1"));
    cloth_previous_trace_seq=reinterpret_cast<ClothPreviousTraceSeqFn>(GetProcAddress(main,"WWMI_RtClothPreviousTraceSeq_v1"));
    compute_run=reinterpret_cast<ComputeRunFn>(GetProcAddress(host,"WWMI_ComputeState_Run_v1"));compute_query=reinterpret_cast<ComputeQueryFn>(GetProcAddress(host,"WWMI_ComputeState_Query_v1"));
    evidence_root_argument=reinterpret_cast<EvidenceRootArgumentFn>(GetProcAddress(host,"WWMI_ComputeState_ReadRootArgument_v1"));
    read_as_slot=reinterpret_cast<ReadAsSlotFn>(GetProcAddress(host,"WWMI_ComputeState_ReadAsTableSlot_v1"));
    read_native_state_object=reinterpret_cast<WWMI_RtCurrentNativeStateObjectFn_v1>(GetProcAddress(host,"WWMI_RtCurrentNativeStateObject_v1"));
    read_ray_as_slot=reinterpret_cast<ReadRayAsSlotFn>(GetProcAddress(host,"WWMI_ComputeState_ReadRayAsSlot_v1"));
    capture_occlusion_compute=reinterpret_cast<CaptureOcclusionRestoreFn>(GetProcAddress(host,"WWMI_ComputeState_CaptureRestore_v1"));
    capture_occlusion_graphics=reinterpret_cast<CaptureOcclusionGraphicsFn>(GetProcAddress(host,"WWMI_GraphicsState_CaptureTables_v1"));
    read_occlusion_structured=reinterpret_cast<ReadOcclusionStructuredFn>(GetProcAddress(host,"WWMI_ComputeState_ReadStructuredTableSlot_v1"));
    compute_enable=reinterpret_cast<ComputeEnableFn>(GetProcAddress(host,"WWMI_ComputeState_Enable_v1"));register_forward=reinterpret_cast<ForwardRegisterFn>(GetProcAddress(host,"WWMI_RtForward_Register_v1"));unregister_forward=reinterpret_cast<ForwardUnregisterFn>(GetProcAddress(host,"WWMI_RtForward_Unregister_v1"));
    register_forward_v2=reinterpret_cast<ForwardRegisterV2Fn>(GetProcAddress(host,"WWMI_RtForward_Register_v2"));
    register_forward_v3=reinterpret_cast<ForwardRegisterV3Fn>(GetProcAddress(host,"WWMI_RtForward_Register_v3"));
    native_buffer_lease=reinterpret_cast<NativeBufferLeaseFn>(GetProcAddress(host,"WWMI_RtNativeBufferReadLease_v1"));
    descriptor_lease=reinterpret_cast<WWMI_RtSetDescriptorObservationFn_v1>(GetProcAddress(main,"WWMI_RtSetDescriptorObservation_v1"));descriptor_source=reinterpret_cast<WWMI_RtReadDescriptorSourceFn_v1>(GetProcAddress(main,"WWMI_RtReadDescriptorSource_v1"));
    mod_catalog=reinterpret_cast<WWMI_RtModReadCatalogFn_v1>(GetProcAddress(main,"WWMI_RtModReadCatalog_v1"));mod_resource=reinterpret_cast<WWMI_RtModReadResourceFn_v1>(GetProcAddress(main,"WWMI_RtModReadResource_v1"));
    mod_position=reinterpret_cast<WWMI_RtModAcquirePositionFn_v1>(GetProcAddress(main,"WWMI_RtModAcquireGeometryPosition_v1"));mod_stamp=reinterpret_cast<WWMI_RtModStampRecordingFn_v1>(GetProcAddress(main,"WWMI_RtModStampRecording_v1"));
    mod_detected=reinterpret_cast<WWMI_RtModMarkDetectedFn_v1>(GetProcAddress(main,"WWMI_RtModMarkDetected_v1"));set_frame_device=reinterpret_cast<WWMI_RtSetPackFrameDeviceFn_v1>(GetProcAddress(main,"WWMI_RtSetPackFrameDevice_v1"));
    // The planned catalog keeps the old wire format/resources, not a second Mod system.
    mod_catalog=reinterpret_cast<WWMI_RtModReadCatalogFn_v1>(GetProcAddress(main,"WWMI_RtModReadSurfaceCatalog_v1"));
    mod_geometry_section=reinterpret_cast<WWMI_RtModReadGeometrySectionFn_v1>(GetProcAddress(main,"WWMI_RtModReadGeometrySection_v1"));
    mod_author_section=reinterpret_cast<decltype(mod_author_section)>(GetProcAddress(main,"WWMI_RtModReadAuthorSection_v2"));
    mod_author_revision=reinterpret_cast<decltype(mod_author_revision)>(GetProcAddress(main,"WWMI_RtModReadAuthorRevision_v1"));
    mod_acquire_deformed=reinterpret_cast<decltype(mod_acquire_deformed)>(GetProcAddress(main,"WWMI_RtModAcquireDeformedSource_v1"));
    shape_position=reinterpret_cast<WWMI_RtModAcquirePostMultiplierShapeFn_v1>(GetProcAddress(main,"WWMI_RtModAcquirePostMultiplierShape_v1"));
    cpu_window=reinterpret_cast<WWMI_RtCpuWindowDeadlineFn_v1>(GetProcAddress(main,"WWMI_RtCpuWindowDeadline_v1"));
    draw_window=reinterpret_cast<WWMI_RtDrawWindowDeadlineFn_v1>(GetProcAddress(main,"WWMI_RtDrawWindowDeadline_v1"));
    mod_section_identity=reinterpret_cast<WWMI_RtModReadSectionIdentityFn_v1>(GetProcAddress(main,"WWMI_RtModReadSectionIdentity_v1"));
    if(!mod_catalog||!mod_resource||!mod_position||!mod_stamp||!mod_detected||!set_frame_device||!mod_section_identity||!mod_geometry_section){clear_provider_exports();return false;}
    if(!compute_run||!compute_query||!compute_enable||!register_forward||!unregister_forward||!descriptor_lease||!descriptor_source){clear_provider_exports();return false;}
    if(!reshade::register_addon(module)){clear_provider_exports();return false;}
    const bool can_probe_raygen=read_ray_as_slot&&read_native_state_object&&capture_occlusion_compute&&capture_occlusion_graphics;
    WWMI_RtForwardCallbacks_v1 callbacks{sizeof(callbacks),1,reinterpret_cast<std::uint64_t>(&devices),forward_blas,
        read_as_slot?observe_native_tlas:nullptr,can_probe_raygen?observe_native_raygen:nullptr,forward_queue};
    WWMI_RtForwardCallbacks_v2 with_dispatch{sizeof(with_dispatch),2,callbacks.context,callbacks.blas_after,
        callbacks.tlas_input,callbacks.hit_table,callbacks.queue_after,native_dispatch_receipt};
    std::error_code asset_error;
    native_discovery_available=register_forward_v3&&native_buffer_lease&&std::filesystem::is_regular_file(data_directory()/L"NativeDiscoveryCopy.cso",asset_error);
    const WWMI_RtForwardCallbacks_v3 with_buffers{with_dispatch,native_buffer_receipt};
    const auto registered=native_discovery_available?register_forward_v3(&with_buffers):
        register_forward_v2&&(draw_window||(read_as_slot&&capture_occlusion_compute))?
            register_forward_v2(&with_dispatch):register_forward(&callbacks);
    if(registered!=0){reshade::unregister_addon(module);clear_provider_exports();return false;}
    {std::lock_guard lock(devices_mutex);provider_module=std::move(provider);accepting_devices=true;}
    descriptor_lease(1);compute_enable(1);
    material_chs_writer.start();
#define REGISTER(e,f) reshade::register_event<reshade::addon_event::e>(f);
    EVENTS(REGISTER)
#undef REGISTER
    return true;
}
extern "C" __declspec(dllexport) void AddonUninit(HMODULE module,HMODULE){
    {std::lock_guard lock(devices_mutex);if(!accepting_devices)return;accepting_devices=false;}
    // The host unregister drains entered forward calls before returning.
    unregister_forward(reinterpret_cast<std::uint64_t>(&devices));compute_enable(0);
    instance_dispatch_watch.store(0,std::memory_order_relaxed);
    timed_until.store(0,std::memory_order_release);timed_close_lists.store(0,std::memory_order_release);
#define UNREGISTER(e,f) reshade::unregister_event<reshade::addon_event::e>(f);
    EVENTS(UNREGISTER)
#undef UNREGISTER
    material_chs_writer.stop();
    {std::unique_lock lock(devices_mutex);devices_idle.wait(lock,[]{return initializing_devices.empty();});}
    std::vector<api::device *> remaining;{std::lock_guard lock(devices_mutex);for(auto &[d,_]:devices)remaining.push_back(d);}
    for(auto *d:remaining)destroy_device(d);
    // stop()/~Runtime first releases main's frame Reader, then InputCollector
    // performs its final provider stamps. Only now drop the observation lease.
    descriptor_lease(0);
    reshade::unregister_addon(module);
    std::shared_ptr<ProviderModule> provider;
    {std::lock_guard lock(devices_mutex);provider=std::move(provider_module);}
    // Entered device/export calls and a genuine unfinished-GPU Runtime retain
    // their own owner until their final provider-using destructor completes.
    provider.reset();
}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
