// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "producer_trace.h"
#include <stdint.h>

namespace wwmi::semantic {
constexpr uint32_t kMaxElements=32;
constexpr uint32_t kMaxSubobjects=64;
constexpr uint32_t kUnknownClassification=UINT32_MAX;
enum Site : uint32_t {
    PublicInit=3001, PublicDestroy=3002,
    ShimGraphicsCreate=3010, ShimComputeCreate=3011,
    BlendVariantCreate=3020, LayoutVariantCreate=3021, RfxVariantCreate=3022,
    VariantDestroy=3023, ShimDescriptorTable=3030,
    ShimGraphicsPso=3040, ShimGraphicsRoot=3041,
    ShimComputePso=3042, ShimComputeRoot=3043,
    SectionBlendPso=3050, SectionLayoutPso=3051, SectionRfxPso=3052,
    SectionRestorePso=3053
};
enum Declaration : uint32_t { NativeDxgi=1, ReshadeDeclaration=2, LayoutAbsent=3 };
enum StateOperation : uint32_t { SetPso=1, SetGraphicsRoot=2, SetComputeRoot=3 };

// These are CPU API declarations, not live vertex data or shader fetch results.
struct Element {
    const char* semantic=nullptr;
    uint32_t semantic_index=0, format=0, slot=0, offset=0, stride=0;
    uint32_t classification=kUnknownClassification, instance_step_rate=0;
    bool recognized_format=false;
};
struct Pipeline {
    uint64_t handle=0, root_signature=0, shader_hash=0;
    uint32_t site=0, provenance=0, stage=0, declaration=LayoutAbsent;
    uint32_t declared_count=0, available_count=0, flags=0;
    const Element* elements=nullptr;
};

// A deliberately limited catalog of common typed DXGI/ReShade IA formats.
// A false result means "this observer has not decoded the enum", not invalid GPU use.
bool RecognizedVertexFormat(uint32_t value) noexcept;
void PipelineCreated(const Pipeline&) noexcept;
void PipelineDestroyed(uint64_t pipeline,uint32_t site,uint32_t provenance) noexcept;
void DescriptorBound(uint64_t list,uint64_t root,uint32_t stage,uint32_t type,
                     uint32_t parameter,uint32_t space,uint32_t reg,uint32_t ring_slot,
                     uint64_t cpu,uint64_t gpu,uint32_t site=ShimDescriptorTable) noexcept;
void NativeState(uint64_t list,uint64_t handle,uint32_t operation,uint32_t stage,uint32_t site) noexcept;

template<class T> inline uint64_t Number(T* p) noexcept {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
}
template<class List,class Pso> inline void SetPipelineState(List* list,Pso* pso,uint32_t site) {
    list->SetPipelineState(pso);
    NativeState(Number(list),Number(pso),SetPso,0,site);
}
template<class List,class Root> inline void SetGraphicsRootSignature(List* list,Root* root,uint32_t site) {
    list->SetGraphicsRootSignature(root);
    NativeState(Number(list),Number(root),SetGraphicsRoot,0,site);
}
template<class List,class Root> inline void SetComputeRootSignature(List* list,Root* root,uint32_t site) {
    list->SetComputeRootSignature(root);
    NativeState(Number(list),Number(root),SetComputeRoot,'c',site);
}
} // namespace wwmi::semantic
