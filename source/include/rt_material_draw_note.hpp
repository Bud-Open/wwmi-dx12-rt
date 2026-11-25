#pragma once

#include <array>
#include <cstdint>
#include <span>
#include "rt_published_texture.hpp"

namespace reshade::api { struct command_list; }
struct ShadowSlotOvr;
struct RtMaterialTables;

enum class RtRfxStep : uint8_t {
    no_call, pending, no_maps, disabled, no_color, low_mrt, ps_reads,
    window, missing_native, unknown_description, known_empty, plan_ready, offset, owner, no_device, old_private,
    stale_native, wrong_native_view, not_native_2d
};

struct RtRfxDrawFacts {
    RtRfxStep step=RtRfxStep::no_call;
    uint8_t called=0, requested=0, resolved=0, reads=0, bound_count=0, missing_slot=0xff;
    uint8_t native_valid=0,native_present=0,native_mapped=0;
    unsigned material_failure=0,material_param=~0u,material_offset=~0u;
    bool vs_alias_conflict=false;
    struct Native { unsigned format=0,width=0,height=0,mips=0,classification=0; };
    std::array<Native,3> native{};
    std::array<uint8_t,4> fresh{},masks{}; // D/N/L/M; desired by classifier, accepted by plan
    std::array<uint8_t,16> bound_role{}; // each actually queued RFX override
    struct Destination {uint8_t param=0xff;unsigned offset=0;};
    std::array<Destination,4> ps_locations{};
    bool locations_known=false;
    struct Final {
        uint64_t resource=0,view=0,root_table=0;
        unsigned param=~0u,offset=~0u;
        char role='?';
    };
    std::array<Final,4> final{};
    bool copied=false;
    std::array<RtPublishedTexture,3> native_sources{};
};

// Only while building this ready subdraw's recorded table, on this thread.
// The owners are parallel to PS writes, not to additional VS-only writes.
struct RtMaterialCopyNote {
    RtRfxDrawFacts* facts;
    std::span<const uint64_t> owners;
    struct PrivateView {uint64_t view=0,resource=0;};
    std::array<PrivateView,4> vs_inputs{};
};
inline thread_local RtMaterialCopyNote* rt_material_copy_note=nullptr;
struct RtMaterialCopyScope {
    RtMaterialCopyNote note;
    RtMaterialCopyNote* previous=rt_material_copy_note;
    RtMaterialCopyScope(RtRfxDrawFacts& facts,std::span<const uint64_t> owners)
        :note{&facts,owners}{
        rt_material_copy_note=facts.requested&&facts.called&&!(facts.requested&~facts.resolved)?&note:nullptr;
    }
    ~RtMaterialCopyScope(){rt_material_copy_note=previous;}
};
inline void RtNoteLocalVfView(unsigned stream,uint64_t view,uint64_t resource){
    if(rt_material_copy_note)rt_material_copy_note->vs_inputs[stream]={view,resource};
}
inline char RtRfxRole(uint8_t role){
    return role==1?'D':role==2?'N':role==4?'L':role==8?'M':'R';
}

enum class RtNoteRoute : uint8_t {
    hybrid, old_private, old_palette,
    refused_hybrid, refused_old, native_pass_through, plan_violation, hidden
};

// Emits only a bounded ordinary metadata row, not a capture. “recorded”
// callers must invoke this after the actual graphics draw, never after prepare.
void RtMaterialDrawNote(reshade::api::command_list*, int mod, unsigned component, unsigned rtv,
    uint64_t effective_pso, RtNoteRoute, const char* reason, unsigned first, unsigned count, int base,
    const RtRfxDrawFacts* = nullptr, std::span<const ShadowSlotOvr> = {}, const RtMaterialTables* = nullptr, unsigned uv_pairs = 0);
void RtMaterialDrawFlush();
