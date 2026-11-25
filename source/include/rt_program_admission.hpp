#pragma once
// Ordinary admission diagnostics only. No shader, clone, or render ownership.
#include <array>
#include <cstdint>
#include <memory>

namespace reshade::api { struct pipeline_subobject; }
struct RtProgramPsoContract;

enum RtProgramSdkBit : uint16_t {
    rt_sdk_vs=1, rt_sdk_ps=2, rt_sdk_other_stage=4, rt_sdk_targets=8,
    rt_sdk_blend=16, rt_sdk_depth=32, rt_sdk_samples=64, rt_sdk_sample_mask=128,
    rt_sdk_stream=256, rt_sdk_dynamic=512, rt_sdk_layout=1024, rt_sdk_graphics_other=2048
};
enum class RtProgramBuildPhase : uint8_t { not_run, sdk_metadata, vertex, pixel, pair, bindings, ready };
enum class RtProgramBuildIssue : uint8_t {
    none, missing_layout, invalid_description, shader_description, other_stage,
    missing_stage, targets_unreported, malformed_state, root_map, cb_register,
    cb_slot, cb_alias, stream_register, stream_slot, stream_alias
};
struct RtProgramBuildMark {
    uint16_t sdk_presence=0;
    RtProgramBuildPhase phase{};
    RtProgramBuildIssue issue{};
    // For a root failure: CB role 0..2 or stream role 0..3, and the native register.
    uint8_t role=0xff, reg=0xff;
};
enum class RtProgramStore : uint8_t { not_seen, unpaired, sdk_blend_unreported, sdk_blend_unusable, effects_only, clone, retired };
enum class RtProgramLookup : uint8_t { no_clone, no_contract, vs_not_resolved, vs_mismatch, unavailable, usable };
struct RtProgramLookupStamp {
    // The observed SDK generation and the legacy lookup's actual candidate
    // are different facts when the address is reused without a destroy event.
    uint64_t creation=0;
    uint64_t lookup_creation=0;
    RtProgramLookup status{};
    bool returned=false;
};
struct RtProgramIntakeDetail {
    uint64_t creation=0, layout=0, vs=0, ps=0;
    RtProgramStore store{};
    RtProgramBuildMark build{};
    std::array<char,96> reason{};
};

uint64_t RtNextProgramCreation();
uint16_t RtProgramSdkPresence(uint64_t layout,uint32_t count,const reshade::api::pipeline_subobject*);
std::shared_ptr<const RtProgramPsoContract> RtBuildObservedProgramPsoContract(uint64_t pso,uint64_t layout,
    uint32_t count,const reshade::api::pipeline_subobject*,RtProgramBuildMark&);
void RtNoteUnpairedProgramPso(uint64_t pso,uint64_t layout,uint32_t count,const reshade::api::pipeline_subobject*);
std::shared_ptr<const RtProgramPsoContract> RtProgramContractForPsoObserved(uint64_t pso,uint64_t vs,RtProgramLookupStamp&);
RtProgramIntakeDetail RtReadProgramIntakeDetail(uint64_t pso,RtProgramLookupStamp);
