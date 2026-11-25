#pragma once
// Copied snapshots only. All offsets are byte offsets from WWMI_RtModCatalog_v1.
// A catalog/resource read never calls into the INI evaluator or returns engine pointers.
#include <stdint.h>
#if defined(_WIN32) && defined(WWMI_RT_SOURCE_IMPLEMENTATION)
#define WWMI_RT_EXPORT __declspec(dllexport)
#else
#define WWMI_RT_EXPORT
#endif
#ifdef __cplusplus
extern "C" {
#endif

enum WWMI_RtModStatus_v1 {
    WWMI_RT_MOD_OK=0, WWMI_RT_MOD_MORE=1, WWMI_RT_MOD_NOT_READY=2,
    WWMI_RT_MOD_STALE=3, WWMI_RT_MOD_BAD_INPUT=4, WWMI_RT_MOD_MISSING=5
};
enum WWMI_RtModFlags_v1 { WWMI_RT_MOD_ENABLED=1 };
enum WWMI_RtComponentFlags_v1 { WWMI_RT_COMPONENT_SELECTION_KNOWN=1 };
enum WWMI_RtResourceFlags_v1 { WWMI_RT_RESOURCE_INITIAL_BYTES=1 };

typedef struct WWMI_RtModCatalog_v1 {
    uint32_t struct_size,abi_version;
    uint64_t bytes,load_generation,revision;
    uint32_t mod_count,mods_offset;
} WWMI_RtModCatalog_v1;
typedef struct WWMI_RtMod_v1 {
    uint64_t key,revision;
    uint32_t flags,object_guid,mesh_vertex_count,shapekey_vertex_count;
    uint32_t component_count,components_offset,resource_count,resources_offset;
    uint32_t namespace_offset,namespace_bytes,directory_offset,directory_bytes; // UTF-16LE, including NUL
} WWMI_RtMod_v1;
typedef struct WWMI_RtModComponent_v1 {
    int32_t component;
    uint32_t flags,content_hash,native_index_first,native_index_count;
    uint32_t canonical_vg_offset,canonical_vg_count;
    uint32_t draw_count,draws_offset;
} WWMI_RtModComponent_v1;
typedef struct WWMI_RtModDraw_v1 {
    uint32_t index_count,first_index;
    int32_t base_vertex;
} WWMI_RtModDraw_v1;
typedef struct WWMI_RtModResource_v1 {
    uint32_t index,flags,stride,reserved;
    uint64_t bytes;
    uint32_t name_offset,name_bytes; // original Resource* name, UTF-16LE including NUL
} WWMI_RtModResource_v1;

typedef uint32_t (*WWMI_RtModReadCatalogFn_v1)(void *out,uint64_t capacity,uint64_t *required);
typedef uint32_t (*WWMI_RtModReadResourceFn_v1)(uint64_t load_generation,uint64_t mod_key,
    uint32_t resource_index,void *out,uint64_t capacity,uint64_t *required);
WWMI_RT_EXPORT uint32_t WWMI_RtModReadCatalog_v1(void *out,uint64_t capacity,uint64_t *required);
WWMI_RT_EXPORT uint32_t WWMI_RtModReadResource_v1(uint64_t load_generation,uint64_t mod_key,
    uint32_t resource_index,void *out,uint64_t capacity,uint64_t *required);

// Final geometry is an owned COM resource reference, never an engine container.
// Acquire adds one ID3D12Resource reference; the caller releases it after its
// recording/submission ownership retires. FINAL is fully deformed, object-local
// float3 positions; REST_VERIFIED is a target-draw fact that this Mod needs no
// extra displacement, not an inference from shapekey_vertex_count.
enum WWMI_RtPositionMode_v1 { WWMI_RT_POSITION_UNKNOWN=0, WWMI_RT_POSITION_REST_VERIFIED=1, WWMI_RT_POSITION_FINAL=2 };
typedef struct WWMI_RtModPosition_v1 {
    uint32_t struct_size,abi_version,mode,stride;
    uint64_t load_generation,mod_key,mod_revision;
    uint64_t resource,resource_creation,resource_offset,bytes;
    uint32_t first_vertex,vertex_count,resource_state,reserved;
} WWMI_RtModPosition_v1;
typedef uint32_t (*WWMI_RtModAcquirePositionFn_v1)(uint64_t list,uint64_t creation,uint64_t recording,
    uint64_t load_generation,uint64_t mod_key,uint64_t mod_revision,int32_t component,
    WWMI_RtModPosition_v1 *out,uint32_t out_size);
typedef void (*WWMI_RtModStampRecordingFn_v1)(uint64_t list,uint64_t creation,uint64_t recording);
WWMI_RT_EXPORT uint32_t WWMI_RtModAcquirePosition_v1(uint64_t list,uint64_t creation,uint64_t recording,
    uint64_t load_generation,uint64_t mod_key,uint64_t mod_revision,int32_t component,
    WWMI_RtModPosition_v1 *out,uint32_t out_size);
WWMI_RT_EXPORT void WWMI_RtModStampRecording_v1(uint64_t list,uint64_t creation,uint64_t recording);
// Main publishes only after the final position producer, in this recording,
// has made this resource readable as NON_PIXEL_SHADER_RESOURCE. The provider
// retains its own COM reference. Reset stamps discard older publications.
uint32_t WWMI_RtModPublishPosition_v1(uint64_t list,int32_t component,const WWMI_RtModPosition_v1 *value,uint32_t size);

// Native identity is reported even for disabled Mods/unknown selections.
// Drain on the engine-owning thread BEFORE the minimal [Present] evaluation.
typedef void (*WWMI_RtModMarkDetectedFn_v1)(uint32_t object_guid);
WWMI_RT_EXPORT void WWMI_RtModMarkDetected_v1(uint32_t object_guid);
void WWMI_RtModDrainDetection_v1(void);

// Main-only producer hooks. Publish on the engine's owning thread after geometry
// load and after the Present enable-state update. It reuses the parsed engine.
void WWMI_RtModPublish_v1(void);
// Wrap the *actual evaluated component section*, not its declared draw union.
// Begin returns zero when no published mod exists. Draw receives resolved auto
// arguments and signed base vertex. End(success=0) discards partial selections.
uint64_t WWMI_RtModSectionBegin_v1(int32_t mod_index,int32_t component);
void WWMI_RtModSectionDraw_v1(uint64_t token,uint32_t count,uint32_t first,int32_t base_vertex);
void WWMI_RtModSectionEnd_v1(uint64_t token,uint32_t success);

#ifdef __cplusplus
}
#endif
