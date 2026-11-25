#pragma once
#include "rt_mod_source.h"
#include "rt_graphics_geometry.h"
#ifdef __cplusplus
extern "C" {
#endif
// Only an actual main path which shares these private vertices between raster
// and RT may set MOD_AUTHOR_SHARED. It is not a displacement-zero assertion.
enum WWMI_RtModPlanFlags_v1 {
    WWMI_RT_PLAN_MOD_AUTHOR_SHARED=1,
    WWMI_RT_PLAN_EXTRA_POSITION_INPUT=2
};
// Additional flags only in ReadGeometryCatalog, never in the old draw catalog.
enum WWMI_RtGeometryComponentFlags_v1 {
    WWMI_RT_GEOMETRY_SELECTION_PLANNED=0x10000,
    WWMI_RT_GEOMETRY_MOD_AUTHOR_SHARED=0x20000,
    WWMI_RT_GEOMETRY_EXTRA_POSITION_INPUT=0x40000
};
typedef struct WWMI_RtModGeometrySection_v1 {
    uint32_t struct_size,abi_version;
    uint64_t load_generation,mod_key,mod_revision,plan_frame;
    int32_t component;
    uint32_t selection_flags;
} WWMI_RtModGeometrySection_v1;
// Wrap an existing live SectionBegin token. Record resolved, conditionally
// requested VM draws even if raster must keep this frame's original draw.
uint64_t WWMI_RtModPlanBegin_v1(uint64_t section_token,uint64_t frame_id,uint32_t flags);
uint32_t WWMI_RtModPlanDraw_v1(uint64_t plan_token,uint32_t count,uint32_t first,int32_t base_vertex,uint32_t position_flags);
// Call only after the ENTIRE section succeeds, and before old SectionEnd.
// complete=0 discards the partial plan. No draws + complete=1 means hidden.
uint32_t WWMI_RtModPlanEnd_v1(uint64_t plan_token,uint32_t complete);
// Same existing flat catalog/resource layout, with complete plans preferred.
// Its revision is a geometry-selection revision: issuing a planned draw does
// not invalidate geometry merely by changing the separate issued-draw history.
// It neither changes nor reports the old issued-draw catalog as planned.
WWMI_RT_EXPORT uint32_t WWMI_RtModReadGeometryCatalog_v1(void *out,uint64_t capacity,uint64_t *required);
typedef uint32_t (*WWMI_RtModReadGeometrySectionFn_v1)(uint64_t section_token,uint64_t frame_id,WWMI_RtModGeometrySection_v1 *out,uint32_t out_size);
// Read only the last COMPLETE plan. frame_id=0 selects that plan; a nonzero
// frame must match it exactly. An in-progress plan is never exposed.
WWMI_RT_EXPORT uint32_t WWMI_RtModReadGeometrySection_v1(uint64_t section_token,uint64_t frame_id,WWMI_RtModGeometrySection_v1 *out,uint32_t out_size);
// The new selection revision is separate from issued-draw history. Publish
// FINAL or the existing REST_VERIFIED fact in this geometry revision domain;
// the modes, recording stamps and COM ownership have their original meanings.
uint32_t WWMI_RtModPublishGeometryPosition_v1(uint64_t list,int32_t component,const WWMI_RtModPosition_v1 *value,uint32_t size);
WWMI_RT_EXPORT uint32_t WWMI_RtModAcquireGeometryPosition_v1(uint64_t list,uint64_t creation,uint64_t recording,
    uint64_t load_generation,uint64_t mod_key,uint64_t mod_revision,int32_t component,WWMI_RtModPosition_v1 *out,uint32_t out_size);
// Preserve the existing graphics ABI and its old entry point. This additional
// entry accepts an explicit completed plan for the new policy. The requested
// frame_id is that committed plan's frame, not necessarily this draw's frame.
typedef struct WWMI_RtPlannedGeometryRequest_v1 {
    WWMI_RtGraphicsGeometryRequest_v1 draw;
    uint64_t frame_id;
    // Current draw path's explicit policy, not inferred from shape-key count.
    // MOD_AUTHOR_SHARED must match the completed plan. EXTRA_POSITION_INPUT
    // requires this batch to have consumed a real FINAL source for this part.
    uint32_t path_flags,reserved;
} WWMI_RtPlannedGeometryRequest_v1;
typedef uint32_t (*WWMI_RtPackAcquirePlannedGeometryFn_v1)(const WWMI_RtPlannedGeometryRequest_v1 *request,uint32_t request_size,WWMI_RtGraphicsGeometry_v1 *out,uint32_t out_size);
uint32_t WWMI_RtPackAcquirePlannedGeometry_v1(const WWMI_RtPlannedGeometryRequest_v1 *request,uint32_t request_size,WWMI_RtGraphicsGeometry_v1 *out,uint32_t out_size);
#ifdef __cplusplus
}
#endif
