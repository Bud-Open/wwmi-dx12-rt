#pragma once

// Optional companion of the existing commit ABI. Its base and state
// transitions are unchanged; older Main/client builds still pair via v1.
enum class RtAuthorChoiceSite : uint32_t { unspecified, early_native, section_refusal, finish, private_commit };
enum class RtAuthorFinishReason : uint32_t {
    none, plan_missing, vm_incomplete, plan_end, read_selection, no_rest_recipe, no_rest_source,
    missing_heaps, missing_previous, frame_result, incoherent_parts, material_tables,
    invalid_range, private_rejected, lost_ps_o_m, not_owned_rest, port_work_refused, frame_owner
};

struct RtAuthorCommitNote {
    WWMI_RtAuthorCommit_v1 choice;
    RtAuthorChoiceSite site;
    uint32_t reason, surface, phase, geometry;
    uint64_t program;
};
using RtAuthorCommitNoteFn=uint32_t (*)(const RtAuthorCommitNote*,uint32_t);
