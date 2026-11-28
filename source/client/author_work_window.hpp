#pragma once

// Runtime::mutex owns this bounded receipt window. Its timers are anchored
// separately by the command recording and each native submission.
enum class GpuWorkKind : unsigned { prefix,section,native_direct,native_compute,count };
static constexpr const char *gpu_work_names[]{"owned_prefix","private_section","native_direct_dispatch","native_compute_dispatch"};
struct GpuWorkSample {
    ComPtr<ID3D12QueryHeap> query;
    ComPtr<ID3D12Resource> readback;
    GpuWorkKind kind{};
    Recording epoch{};
    NativeIdentityFence signal{};
    std::uint64_t id=0,call=0,qpc=0,at_ms=0,frame=0,native_list=0,frequency=0;
    std::uint64_t current=0,previous=0,work=0;
    unsigned parts=0,pre=0;
    unsigned current_coverage=UINT32_MAX,previous_coverage=UINT32_MAX,current_placement=0,previous_placement=0;
    bool ended=false,resolved=false,reported=false,producer_fence=false;
    const char *error=nullptr;
};
struct AuthorWorkBin {
    std::uint64_t first_frame=0,last_frame=0;
    std::uint64_t created_full=0,created_component=0,distinct_created=0,identity_excluded=0;
    std::uint64_t equivalent_pending=0,equivalent_unsubmitted=0,equivalent_inflight=0,storage_reuse=0,position_bytes=0,tangent_bytes=0;
    std::uint64_t page_breaks_desc=0,page_breaks_const=0,page_const_high_water=0;
    std::uint64_t local_acquire=0,cross_ready=0,uncommitted_reset=0;
    std::uint64_t prefix_submit_full=0,prefix_submit_component=0,prefix_vertices=0,command_retired=0;
    std::uint64_t sections=0,parts=0,indices=0,current_default=0,current_bootstrap=0,previous_default=0,previous_bootstrap=0;
    std::uint64_t current_unclassified=0,previous_unclassified=0,first_current=0,first_previous=0;
    std::uint64_t consumer_executes=0,consumer_sections=0,consumer_recorded_in_window=0,consumer_replays=0;
    unsigned asset_count[4]{};
    std::uint64_t asset_bytes[4]{};
    std::uint64_t owner_bytes[6]{};
    std::uint64_t inventory_at=0;
    unsigned uncommitted_live=0;
    bool inventory=false;
};
struct AuthorWorkWindow {
    static constexpr unsigned bin_ms=rt_phase_cpu::bin_ms,bin_count=rt_phase_cpu::bins,timing_stride_ms=4000,
        section_pass_retry_ms=250,identity_limit=4096;
    std::uint64_t start=0,admitted=0,end=0,start_qpc=0,qpc_frequency=0;
    std::array<AuthorWorkBin,bin_count> bins{};
    std::array<std::uint64_t,unsigned(GpuWorkKind::count)> next_sample{};
    std::array<unsigned,unsigned(GpuWorkKind::count)> omitted{};
    unsigned section_pass_unproven=0;
    std::vector<std::shared_ptr<GpuWorkSample>> samples;
    // Non-owning output sightings: neither diagnostics nor equivalence can
    // keep a retired output alive or grant cross-recording sharing.
    struct Generation {std::vector<std::weak_ptr<AuthorOutput>> outputs;bool created_here=false;};
    std::map<std::vector<std::uint64_t>,Generation> generations;
    unsigned generation_entries=0;
    bool summary=false,timing_summary=false;
    int last_inventory=-1;
    AuthorWorkBin *at(std::uint64_t t)noexcept{
        if(!end||t<start||t>=end)return nullptr;
        const auto slot=(t-start)/bin_ms;
        return slot<bins.size()?&bins[slot]:nullptr;
    }
    AuthorWorkBin *current()noexcept{return end&&!summary?at(GetTickCount64()):nullptr;}
};
