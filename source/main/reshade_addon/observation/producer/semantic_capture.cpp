// SPDX-License-Identifier: GPL-3.0-only
#include "semantic_capture.hpp"
#include "producer_hooks.hpp"
#include <algorithm>

namespace wwmi::semantic {
bool RecognizedVertexFormat(uint32_t v) noexcept {
    switch(v) {
    case 2: case 3: case 4: case 6: case 7: case 8:
    case 10: case 11: case 12: case 13: case 14:
    case 16: case 17: case 18: case 24: case 25: case 26:
    case 28: case 29: case 30: case 31: case 32:
    case 34: case 35: case 36: case 37: case 38:
    case 41: case 42: case 43: case 49: case 50: case 51: case 52:
    case 54: case 56: case 57: case 58: case 59:
    case 61: case 62: case 63: case 64: return true;
    default:return false;
    }
}
static void Name(WWMI_ProducerInput_v1& r,const char* s) noexcept {
    if(!s) { r.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;return; }
    for(uint32_t n=0;n<sizeof(r.semantic_name)-1;++n) {
        if(s[n]=='\0') {
            r.semantic_bytes=n;
            if(n==0)r.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
            return;
        }
        r.semantic_name[n]=s[n];r.semantic_bytes=n+1;
    }
    // At most 64 CPU bytes are examined; no unbounded strlen or UTF-8 repair.
    if(s[sizeof(r.semantic_name)-1]!='\0')
        r.flags|=WWMI_PRODUCER_FLAG_TRUNCATED|WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
}
void PipelineCreated(const Pipeline& p) noexcept {
    if(!producer::SnapshotSession())return;
    auto create=producer::ContextualInput(WWMI_PRODUCER_PSO_CREATE,p.site,p.provenance);
    create.pipeline=p.handle;create.root_signature=p.root_signature;
    create.known_fields|=WWMI_PRODUCER_KNOWN_PIPELINE;
    create.stage=p.stage;create.reason=p.declaration;create.flags=p.flags;
    if(p.root_signature)create.known_fields|=WWMI_PRODUCER_KNOWN_ROOT_SIGNATURE;
    else create.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
    create.element_count=p.declared_count;
    if(p.shader_hash) { create.shader_hash=p.shader_hash;create.known_fields|=WWMI_PRODUCER_KNOWN_SHADER_HASH; }
    if(!p.handle || p.declaration==LayoutAbsent)create.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
    uint32_t captured=(std::min)(p.declared_count,(std::min)(p.available_count,kMaxElements));
    if(captured&&!p.elements) { captured=0;create.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE; }
    if(captured<p.declared_count)create.flags|=WWMI_PRODUCER_FLAG_TRUNCATED|WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
    create.emitted_count=captured;
    WWMI_ProducerInput_v1 elements[kMaxElements]{};
    for(uint32_t i=0;i<captured;++i) {
        auto& r=elements[i];r=create;r.kind=WWMI_PRODUCER_INPUT_ELEMENT;
        r.known_fields|=WWMI_PRODUCER_KNOWN_INPUT_ELEMENT|WWMI_PRODUCER_KNOWN_SLOT;
        r.element_index=i;const auto& e=p.elements[i];
        r.slot=e.slot;r.format=e.format;r.element_offset=e.offset;r.stride=e.stride;
        r.semantic_index=e.semantic_index;r.input_classification=e.classification;
        r.instance_step_rate=e.instance_step_rate;Name(r,e.semantic);
        if(!e.recognized_format || e.classification>1 || e.offset==UINT32_MAX)
            r.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
        create.flags|=r.flags;
    }
    const auto parent=producer::CurrentParent();
    producer::Event(create,parent);
    for(uint32_t i=0;i<captured;++i) {
        elements[i].flags|=create.flags;producer::Event(elements[i],parent);
    }
}
void PipelineDestroyed(uint64_t p,uint32_t site,uint32_t provenance) noexcept {
    if(!producer::SnapshotSession())return;
    auto r=producer::ContextualInput(WWMI_PRODUCER_PSO_DESTROY,site,provenance);
    r.pipeline=p;r.known_fields|=WWMI_PRODUCER_KNOWN_PIPELINE;
    // This callback/API retirement is not last GPU use or a lifetime lease.
    if(!p || provenance==WWMI_PRODUCER_PROVENANCE_CANDIDATE_HINT)r.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
    producer::Event(r,producer::CurrentParent());
}
void DescriptorBound(uint64_t list,uint64_t root,uint32_t stage,uint32_t type,
                     uint32_t parameter,uint32_t space,uint32_t reg,uint32_t slot,
                     uint64_t cpu,uint64_t gpu,uint32_t site) noexcept {
    if(!producer::SnapshotSession())return;
    auto r=producer::ContextualInput(WWMI_PRODUCER_DESCRIPTOR_BIND,site,WWMI_PRODUCER_PROVENANCE_NATIVE_CALL);
    r.native_command_list=list;r.root_signature=root;r.stage=stage;r.descriptor_type=type;
    r.root_parameter=parameter;r.register_space=space;r.shader_register=reg;
    r.slot=slot;r.descriptor_cpu=cpu;r.descriptor_gpu=gpu;r.descriptor_offset=0;
    r.known_fields|=WWMI_PRODUCER_KNOWN_LIST|
        WWMI_PRODUCER_KNOWN_REGISTER_MAPPING|WWMI_PRODUCER_KNOWN_SLOT|WWMI_PRODUCER_KNOWN_DESCRIPTOR_HANDLE;
    if(root)r.known_fields|=WWMI_PRODUCER_KNOWN_ROOT_SIGNATURE;
    const bool valid_stage=stage=='v'||stage=='h'||stage=='d'||stage=='g'||stage=='p'||stage=='c';
    const bool valid_type=type=='b'||type=='t'||type=='u'||type=='s';
    if(!list||!root||!valid_stage||!valid_type)r.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
    producer::Event(r,producer::CurrentParent());
}
void NativeState(uint64_t list,uint64_t handle,uint32_t operation,uint32_t stage,uint32_t site) noexcept {
    if(!producer::SnapshotSession())return;
    auto r=producer::ContextualInput(WWMI_PRODUCER_NATIVE_GRAPHICS_STATE,site,WWMI_PRODUCER_PROVENANCE_NATIVE_CALL);
    r.native_command_list=list;r.reason=operation;r.stage=stage;r.known_fields|=WWMI_PRODUCER_KNOWN_LIST;
    if(operation==SetPso) { r.pipeline=handle;r.known_fields|=WWMI_PRODUCER_KNOWN_PIPELINE; }
    else if(operation==SetGraphicsRoot||operation==SetComputeRoot) {
        r.root_signature=handle;r.known_fields|=WWMI_PRODUCER_KNOWN_ROOT_SIGNATURE;
    } else r.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
    producer::Event(r,producer::CurrentParent());
}
} // namespace wwmi::semantic
