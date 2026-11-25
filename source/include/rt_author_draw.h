#pragma once
#include "rt_draw_frame.h"
#include "rt_draw_identity_receipt.h"
#include "rt_native_palette.h"
#include "rt_geometry_plan.h"

#ifdef __cplusplus
extern "C" {
#endif
// Immutable native CPU source. The token names a full BLAS/pose cohort on
// this device, not a frame label, GPU wait, or native GPU readiness.
typedef struct WWMI_RtAuthorSnapshot_v1 {
    uint32_t struct_size,abi_version;
    uint64_t token,draw_ticket; // per real graphics call in this recording
    WWMI_RtPackDrawIdentity_v2 binding; // ordering is deliberately zero
    WWMI_RtDrawSource_v1 source;
} WWMI_RtAuthorSnapshot_v1;
// Original VS Primitive CB window before the VM. Preserve the actual bytes
// too: one upload allocation and origin can serve new data later, and
// LocalToWorld alone does not distinguish instances.
typedef struct WWMI_RtAuthorInstance_v1 {
    uint64_t resource,byte_origin;
    uint32_t local_to_world[12];
    uint32_t primitive_words[180];
} WWMI_RtAuthorInstance_v1;
typedef struct WWMI_RtAuthorScopeRequest_v1 {
    WWMI_RtPackDrawQuery_v2 native_draw;
    WWMI_RtAuthorInstance_v1 instance;
    uint64_t snapshot,load_generation,mod_key,draw_ticket;
    // The actual pre-VM previous binding. An empty view is not a previous
    // pose. Scope words are taken from the original VS roots, not a later VM binding.
    WWMI_RtPreviousDrawScope_v1 previous_scope;
    uint64_t previous_resource,previous_offset,previous_bytes;
} WWMI_RtAuthorScopeRequest_v1;
typedef struct WWMI_RtAuthorPreviousUse_v1 {
    uint64_t resource_creation,order,writer_list,writer_creation,writer_recording;
} WWMI_RtAuthorPreviousUse_v1;
typedef struct WWMI_RtAuthorScope_v1 {
    uint64_t plan;
    uint32_t disposition,reserved;
    WWMI_RtAuthorPreviousUse_v1 previous;
} WWMI_RtAuthorScope_v1;
typedef struct WWMI_RtAuthorFrameRequest_v1 {
    WWMI_RtDrawFrameRequest_v2 draw;
    uint64_t plan,draw_ticket;
    uint32_t subdraw_index,subdraw_count;
    WWMI_RtAuthorPreviousUse_v1 previous;
} WWMI_RtAuthorFrameRequest_v1;
enum WWMI_RtAuthorDisposition_v1 {
    WWMI_RT_AUTHOR_TENTATIVE=0, WWMI_RT_AUTHOR_NATIVE=1, WWMI_RT_AUTHOR_PRIVATE=2,
    WWMI_RT_AUTHOR_LEGACY=3 // the pack requires an external FINAL dependency
};
typedef struct WWMI_RtAuthorFrame_v1 {
    WWMI_RtDrawFrame_v1 frame;
    uint64_t plan,previous_batch;
    uint32_t disposition,reserved;
} WWMI_RtAuthorFrame_v1;
typedef struct WWMI_RtAuthorCommit_v1 {
    uint64_t device,list,creation,recording,plan,current_batch,previous_batch,draw_ticket;
    // PRIVATE requires a prepared current batch, except a verified empty
    // choice. NATIVE never invalidates a committed private plan.
    uint32_t disposition,empty_selection,component,reserved;
} WWMI_RtAuthorCommit_v1;
typedef uint32_t (*WWMI_RtPackReadAuthorSnapshotFn_v1)(const WWMI_RtPackDrawQuery_v2 *,uint32_t,
    WWMI_RtAuthorSnapshot_v1 *,uint32_t,WWMI_RtDrawIdentityReceipt_v1 *,uint32_t);
typedef uint32_t (*WWMI_RtPackAcquireAuthorPaletteFn_v1)(const WWMI_RtPackDrawQuery_v2 *,uint32_t,
    uint64_t,uint32_t,WWMI_RtNativePalette_v1 *,uint32_t);
typedef uint32_t (*WWMI_RtPackBeginAuthorScopeFn_v1)(const WWMI_RtAuthorScopeRequest_v1 *,uint32_t,
    WWMI_RtAuthorScope_v1 *,uint32_t);
typedef uint32_t (*WWMI_RtPackAcquireAuthorFrameFn_v1)(const WWMI_RtAuthorFrameRequest_v1 *,uint32_t,
    WWMI_RtAuthorFrame_v1 *,uint32_t);
typedef uint32_t (*WWMI_RtPackCommitAuthorDrawFn_v1)(const WWMI_RtAuthorCommit_v1 *,uint32_t);
uint32_t WWMI_RtPackReadAuthorSnapshot_v1(const WWMI_RtPackDrawQuery_v2 *,uint32_t,
    WWMI_RtAuthorSnapshot_v1 *,uint32_t,WWMI_RtDrawIdentityReceipt_v1 *,uint32_t);
uint32_t WWMI_RtPackAcquireAuthorPalette_v1(const WWMI_RtPackDrawQuery_v2 *,uint32_t,
    uint64_t,uint32_t,WWMI_RtNativePalette_v1 *,uint32_t);
uint32_t WWMI_RtPackBeginAuthorScope_v1(const WWMI_RtAuthorScopeRequest_v1 *,uint32_t,WWMI_RtAuthorScope_v1 *,uint32_t);
uint32_t WWMI_RtPackAcquireAuthorFrame_v1(const WWMI_RtAuthorFrameRequest_v1 *,uint32_t,
    WWMI_RtAuthorFrame_v1 *,uint32_t);
uint32_t WWMI_RtPackCommitAuthorDraw_v1(const WWMI_RtAuthorCommit_v1 *,uint32_t);
// The exact VM section that ended successfully, rather than whatever last
// plan currently occupies the same Mod component in the catalog.
// A nonnull subdraw additionally requires the complete ordered request, even
// when the same index range is deliberately drawn more than once.
// A nonnull expected recipe validates the whole ordered unique geometry of
// this section against the consumer's immutable pack, including empty.
// Its input memory is borrowed only for the duration of the call.
typedef struct WWMI_RtModAuthorRecipe_v1 {
    const WWMI_RtModDraw_v1 *draws;
    uint32_t draw_count,selection_flags;
} WWMI_RtModAuthorRecipe_v1;
uint64_t WWMI_RtModBeginAuthorSection_v1(uint64_t token,uint64_t frame,uint32_t flags);
uint32_t WWMI_RtModReadAuthorSection_v1(uint64_t token,uint64_t frame,WWMI_RtModGeometrySection_v1 *,uint32_t,
    uint32_t subdraw_count,uint32_t subdraw_index,WWMI_RtModDraw_v1 *subdraw);
uint32_t WWMI_RtModReadAuthorSection_v2(uint64_t token,uint64_t frame,WWMI_RtModGeometrySection_v1 *,uint32_t,
    uint32_t subdraw_count,uint32_t subdraw_index,WWMI_RtModDraw_v1 *subdraw,const WWMI_RtModAuthorRecipe_v1 *expected);
// Fast publication of the whole evaluated geometry recipe, not a re-evaluation
// of the INI and not a full resource/catalog copy. Zero while still learning.
uint64_t WWMI_RtModReadAuthorRevision_v1(uint64_t load_generation,uint64_t mod_key);
#ifdef __cplusplus
}
#endif
