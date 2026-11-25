#pragma once
#include "rt_program_contracts.hpp"
#include "rt_localvf_inputs.h"
#include "mods/layout_registry.h"
#include "rt_history_roots.h"
#include <reshade.hpp>

// Owned by an actual immutable PsoClone creation, not by a recycled COM address
// or a synthetic old shader hash. A recorded section keeps this snapshot alive.
struct RtProgramPsoContract {
    uint64_t creation=0, native_pso=0, native_layout=0;
    std::shared_ptr<const wwmi::program::Facts> vertex,pixel;
    wwmi::program::Pair capabilities;
    RtLocalProgram local{};
    struct BindingPlan {
        bool valid=false;
        std::array<RegBind,3> cb;
        std::array<RtWorldSurface::StreamSlot,4> streams;
        RtHistoryRoots history;
        bool previous_vertex_only=false,masked_no_alias=false;
        std::vector<uint8_t> pixel_srv_parameters;
    } bindings;
    uint32_t target_count=0,sample_count=0;
    std::array<uint32_t,8> target_formats{};
    bool only_vertex_pixel=false, fixed_native_state=false, masked_material_domain=false;
};
std::shared_ptr<const RtProgramPsoContract> RtBuildProgramPsoContract(uint64_t native_pso,uint64_t layout,
    uint32_t count,const reshade::api::pipeline_subobject*);
std::shared_ptr<const RtProgramPsoContract> RtProgramContractForPso(uint64_t native_pso,uint64_t actual_vs);
wwmi::program::CacheStats RtProgramContentCacheStats();
