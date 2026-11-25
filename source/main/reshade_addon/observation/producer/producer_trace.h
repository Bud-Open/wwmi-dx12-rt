// SPDX-License-Identifier: GPL-3.0-only
// Passive candidate metadata. Pointers and GPU VAs are numbers, never leases.
#pragma once
#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
# define WWMI_PRODUCER_CALL __cdecl
# if defined(WWMI_PRODUCER_BUILD)
#  define WWMI_PRODUCER_EXPORT __declspec(dllexport)
# else
#  define WWMI_PRODUCER_EXPORT
# endif
#else
# define WWMI_PRODUCER_CALL
# define WWMI_PRODUCER_EXPORT
#endif

#define WWMI_PRODUCER_ABI_V1 1u
#define WWMI_PRODUCER_PHASE_EVENT 0u
#define WWMI_PRODUCER_PHASE_BEGIN 1u
#define WWMI_PRODUCER_PHASE_RETURN 2u

#define WWMI_PRODUCER_DRAIN 1u
#define WWMI_PRODUCER_VM_PASS 2u
#define WWMI_PRODUCER_VM_DISPATCH_SUMMARY 3u
#define WWMI_PRODUCER_NATIVE_DISPATCH 4u
#define WWMI_PRODUCER_OUTPUT_HINT 5u
#define WWMI_PRODUCER_RELOCATION 6u
#define WWMI_PRODUCER_PSO_CREATE 7u
#define WWMI_PRODUCER_PSO_DESTROY 8u
#define WWMI_PRODUCER_INPUT_ELEMENT 9u
#define WWMI_PRODUCER_DESCRIPTOR_BIND 10u
#define WWMI_PRODUCER_NATIVE_GRAPHICS_STATE 11u
#define WWMI_PRODUCER_RESOURCE_BIND_HINT 12u
#define WWMI_PRODUCER_INVALIDATION 13u

// Provenance describes the source of metadata, not the identity of a model.
#define WWMI_PRODUCER_PROVENANCE_CANDIDATE_HINT 1u
#define WWMI_PRODUCER_PROVENANCE_NATIVE_CALL 2u
#define WWMI_PRODUCER_PROVENANCE_PUBLIC_CALLBACK 3u
#define WWMI_PRODUCER_PROVENANCE_NATIVE_CREATE_INPUT 4u

#define WWMI_PRODUCER_KNOWN_LIST (UINT64_C(1)<<0)
#define WWMI_PRODUCER_KNOWN_MOD_HINT (UINT64_C(1)<<1)
#define WWMI_PRODUCER_KNOWN_INSTANCE_HINT (UINT64_C(1)<<2)
#define WWMI_PRODUCER_KNOWN_FRAME_HINT (UINT64_C(1)<<3)
#define WWMI_PRODUCER_KNOWN_NAMESPACE_HASH (UINT64_C(1)<<4)
#define WWMI_PRODUCER_KNOWN_PASS (UINT64_C(1)<<5)
#define WWMI_PRODUCER_KNOWN_PIPELINE (UINT64_C(1)<<6)
#define WWMI_PRODUCER_KNOWN_ROOT_SIGNATURE (UINT64_C(1)<<7)
#define WWMI_PRODUCER_KNOWN_SHADER_HASH (UINT64_C(1)<<8)
#define WWMI_PRODUCER_KNOWN_RESOURCE (UINT64_C(1)<<9)
#define WWMI_PRODUCER_KNOWN_RESOURCE_RANGE (UINT64_C(1)<<10)
#define WWMI_PRODUCER_KNOWN_AUX_RESOURCE (UINT64_C(1)<<11)
#define WWMI_PRODUCER_KNOWN_AUX_RANGE (UINT64_C(1)<<12)
#define WWMI_PRODUCER_KNOWN_DISPATCH (UINT64_C(1)<<13)
#define WWMI_PRODUCER_KNOWN_SLOT (UINT64_C(1)<<14)
#define WWMI_PRODUCER_KNOWN_REGISTER_MAPPING (UINT64_C(1)<<15)
#define WWMI_PRODUCER_KNOWN_DESCRIPTOR_HANDLE (UINT64_C(1)<<16)
#define WWMI_PRODUCER_KNOWN_INPUT_ELEMENT (UINT64_C(1)<<17)
#define WWMI_PRODUCER_KNOWN_EXECUTED_TOKEN (UINT64_C(1)<<18)
#define WWMI_PRODUCER_KNOWN_ALL UINT64_C(0x7ffff)

// Always retained by the bridge. No event is a runtime replacement credential.
#define WWMI_PRODUCER_UNPROVEN_ALL UINT64_C(0x1ff)
// Bits: final deformation, live resource/content generation, game object,
// render epoch, material, complete native paths, GPU completion, temporal pose,
// actual shader fetch semantics. Even POSITION is only a declaration.
#define WWMI_PRODUCER_FLAG_TRUNCATED 1u
#define WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE 2u
#define WWMI_PRODUCER_FLAG_EXCEPTION_UNWIND 4u
#define WWMI_PRODUCER_FLAG_ALL 7u

#pragma pack(push,8)
typedef struct WWMI_ProducerInput_v1 {
    uint32_t struct_size, abi_version;
    uint32_t kind, provenance;
    uint32_t source_site, flags;
    uint64_t known_fields;
    uint64_t native_command_list;
    uint64_t frame_hint, instance_hint, namespace_hash;
    int32_t mod_index, owner_mod_index;
    uint32_t pass_index, pass_count;
    uint64_t pipeline, root_signature, shader_hash;
    uint64_t resource, resource_va, resource_bytes, resource_offset;
    uint64_t aux_resource, aux_va, aux_bytes, aux_offset;
    uint64_t descriptor_cpu, descriptor_gpu;
    uint64_t executed_session, executed_call;
    uint32_t dispatch_x, dispatch_y, dispatch_z, emitted_count;
    uint32_t slot, stage, descriptor_type, root_parameter;
    uint32_t register_space, shader_register, descriptor_offset, format;
    uint32_t stride, element_offset, semantic_index, input_classification;
    uint32_t instance_step_rate, element_count, namespace_units, reason;
    uint32_t element_index, semantic_bytes;
    char semantic_name[64]; // bounded UTF-8/ASCII declaration; truncation is explicit
} WWMI_ProducerInput_v1;

typedef struct WWMI_ProducerRecord_v1 {
    uint32_t struct_size, abi_version;
    uint32_t phase, clock_domain;
    uint64_t session_id, sequence, call_id;
    uint64_t parent_session, parent_call;
    uint64_t thread_id, timestamp_ticks, timestamp_frequency;
    uint64_t unproven_fields;
    uint64_t callback_failures_total, suppressed_reentrant_total, observation_failures_total;
    WWMI_ProducerInput_v1 input;
} WWMI_ProducerRecord_v1;

typedef struct WWMI_ProducerStatus_v1 {
    uint32_t struct_size, abi_version;
    uint64_t last_session, last_sequence;
    uint64_t callback_failures_total, suppressed_reentrant_total, observation_failures_total;
    uint64_t pending_calls;
    uint32_t active, draining;
} WWMI_ProducerStatus_v1;
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif
typedef void (WWMI_PRODUCER_CALL *WWMI_Producer_Callback_v1)(const WWMI_ProducerRecord_v1*,void*);
WWMI_PRODUCER_EXPORT int32_t WWMI_PRODUCER_CALL WWMI_ProducerTrace_Register_v1(WWMI_Producer_Callback_v1,void*);
WWMI_PRODUCER_EXPORT int32_t WWMI_PRODUCER_CALL WWMI_ProducerTrace_Unregister_v1(WWMI_Producer_Callback_v1,void*);
WWMI_PRODUCER_EXPORT int32_t WWMI_PRODUCER_CALL WWMI_ProducerTrace_GetStatus_v1(WWMI_ProducerStatus_v1*);
#ifdef __cplusplus
}
namespace wwmi::producer {
struct Token { uint64_t session=0, call=0; };
uint64_t SnapshotSession() noexcept;
Token Begin(const WWMI_ProducerInput_v1&,Token parent={}) noexcept;
void End(Token,const WWMI_ProducerInput_v1&) noexcept;
uint64_t Event(const WWMI_ProducerInput_v1&,Token parent={}) noexcept;
void ObservationFailed() noexcept;
}
static_assert(sizeof(((WWMI_ProducerInput_v1*)0)->semantic_name)==64,"bounded semantic name");
#endif
