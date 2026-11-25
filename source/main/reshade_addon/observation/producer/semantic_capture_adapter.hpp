// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d12.h>
#if __has_include("../../sdk/include/reshade_api.hpp")
#include "../../sdk/include/reshade_api.hpp"
#else
// The isolated CPU SDK test compiles this header from the repository's stage5/
// directory and supplies the same pinned SDK as an external system include.
#include <reshade_api.hpp>
#endif
#include "semantic_capture.hpp"
#include "producer_hooks.hpp"
#include <algorithm>

namespace wwmi::semantic {
inline void NativeGraphicsCreated(ID3D12PipelineState* pso,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) noexcept {
    if(!producer::SnapshotSession())return;
    Element elements[kMaxElements]{};
    const uint32_t count=desc.InputLayout.NumElements;
    const uint32_t n=desc.InputLayout.pInputElementDescs?(std::min)(count,kMaxElements):0;
    for(uint32_t i=0;i<n;++i) {
        const auto& e=desc.InputLayout.pInputElementDescs[i];auto& out=elements[i];
        out.semantic=e.SemanticName;out.semantic_index=e.SemanticIndex;out.format=static_cast<uint32_t>(e.Format);
        out.slot=e.InputSlot;out.offset=e.AlignedByteOffset;out.classification=static_cast<uint32_t>(e.InputSlotClass);
        out.instance_step_rate=e.InstanceDataStepRate;out.recognized_format=RecognizedVertexFormat(out.format);
    }
    Pipeline p{};p.handle=Number(pso);p.root_signature=Number(desc.pRootSignature);
    p.site=ShimGraphicsCreate;p.provenance=WWMI_PRODUCER_PROVENANCE_NATIVE_CREATE_INPUT;
    p.stage='v';p.declaration=NativeDxgi;p.declared_count=count;p.available_count=n;p.elements=elements;
    PipelineCreated(p);
}
inline void NativeComputeCreated(ID3D12PipelineState* pso,const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc) noexcept {
    Pipeline p{};p.handle=Number(pso);p.root_signature=Number(desc.pRootSignature);
    p.site=ShimComputeCreate;p.provenance=WWMI_PRODUCER_PROVENANCE_NATIVE_CREATE_INPUT;
    p.stage='c';p.declaration=NativeDxgi;PipelineCreated(p);
}
inline void ReshadeCreated(reshade::api::pipeline pso,reshade::api::pipeline_layout layout,
                           uint32_t count,const reshade::api::pipeline_subobject* objects,
                           uint32_t site,uint32_t provenance,uint64_t shader_hash=0,uint32_t hash_stage=0) noexcept {
    if(!producer::SnapshotSession())return;
    Pipeline p{};p.handle=pso.handle;p.root_signature=layout.handle;
    p.site=site;p.provenance=provenance;p.shader_hash=shader_hash;p.stage=hash_stage;
    if(provenance==WWMI_PRODUCER_PROVENANCE_CANDIDATE_HINT)p.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
    if(count>kMaxSubobjects)p.flags|=WWMI_PRODUCER_FLAG_TRUNCATED|WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
    if(count&&!objects)p.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
    Element elements[kMaxElements]{};bool found=false,compute=false;
    if(objects)for(uint32_t i=0;i<(std::min)(count,kMaxSubobjects);++i) {
        const auto& o=objects[i];
        if(o.type==reshade::api::pipeline_subobject_type::compute_shader)compute=true;
        if(o.type!=reshade::api::pipeline_subobject_type::input_layout)continue;
        if(found) { p.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;continue; }
        found=true;p.declaration=ReshadeDeclaration;p.declared_count=o.count;
        const auto* input=static_cast<const reshade::api::input_element*>(o.data);
        p.available_count=input?(std::min)(o.count,kMaxElements):0;
        for(uint32_t j=0;j<p.available_count;++j) {
            const auto& e=input[j];auto& out=elements[j];
            out.semantic=e.semantic;out.semantic_index=e.semantic_index;out.format=static_cast<uint32_t>(e.format);
            out.slot=e.buffer_binding;out.offset=e.offset;out.stride=e.stride;out.instance_step_rate=e.instance_step_rate;
            out.recognized_format=RecognizedVertexFormat(out.format);
            // ReShade's step-rate abstraction does not preserve every native
            // classification (notably per-instance with step rate zero).
            out.classification=kUnknownClassification;
        }
    }
    if(!found&&compute&&objects&&count<=kMaxSubobjects) {
        p.stage='c';p.declaration=ReshadeDeclaration; // compute has no IA layout
    }
    p.elements=elements;PipelineCreated(p);
}
inline bool CreateReshadeVariant(reshade::api::device* device,reshade::api::pipeline_layout layout,
                                 uint32_t count,const reshade::api::pipeline_subobject* objects,
                                 reshade::api::pipeline* out,uint32_t site) {
    const bool ok=device->create_pipeline(layout,count,objects,out);
    if(ok&&out)ReshadeCreated(*out,layout,count,objects,site,WWMI_PRODUCER_PROVENANCE_CANDIDATE_HINT);
    return ok;
}
inline void DestroyReshadeVariant(reshade::api::device* device,reshade::api::pipeline pso) {
    device->destroy_pipeline(pso);
    PipelineDestroyed(pso.handle,VariantDestroy,WWMI_PRODUCER_PROVENANCE_CANDIDATE_HINT);
}
} // namespace wwmi::semantic
