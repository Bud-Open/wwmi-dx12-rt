#pragma once
#include "rt_scene_consumer.hpp"
#include "wwmi_compute_occlusion_input.h"

namespace hsin_rt {
using CaptureOcclusionRestoreFn=decltype(&WWMI_ComputeState_CaptureRestore_v1);
using CaptureOcclusionGraphicsFn=decltype(&WWMI_GraphicsState_CaptureTables_v1);
using ReadOcclusionStructuredFn=decltype(&WWMI_ComputeState_ReadStructuredTableSlot_v1);

struct OcclusionBindingRead {
    enum Stage : unsigned { layout, graphics, compute, rays_view, output_view, miss_record, ready };
    Stage stage=layout;
    int status=0;
    Recording recording;
    OcclusionTableLayout table_layout;
    WWMI_GraphicsRestoreInfo_v1 graphics_state{sizeof(WWMI_GraphicsRestoreInfo_v1)};
    std::array<WWMI_GraphicsRestoreRecord_v1,64> graphics_tables{};
    WWMI_ComputeRestoreInfo_v1 compute_state{sizeof(WWMI_ComputeRestoreInfo_v1)};
    std::array<WWMI_ComputeRestoreRecord_v1,64> compute_arguments{};
    WWMI_StructuredTableSlot_v1 rays{sizeof(WWMI_StructuredTableSlot_v1)};
    WWMI_StructuredTableSlot_v1 output{sizeof(WWMI_StructuredTableSlot_v1)};
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE miss_table{};
    InputCollector::RayInputRange miss;
    std::optional<RaygenIdentity::OcclusionRecordBytes> closed_sbt;
};

// Read only inside the same original DispatchRays callback and before any
// raw mutation. These exact bound values are not a submission-time lease.
inline OcclusionBindingRead read_occlusion_bindings(RaySceneConsumer &consumer,InputCollector &inputs,
    api::command_list *list,Recording epoch,const RaySceneInput &ray,const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE &miss,
    CaptureOcclusionGraphicsFn graphics,CaptureOcclusionRestoreFn compute,ReadOcclusionStructuredFn view){
    OcclusionBindingRead out;out.recording=epoch;
    if(!list||!epoch.list||!graphics||!compute||!view||!is_occlusion(ray))return out;
    const auto layout=consumer.occlusion_layout(ray.compute.root_signature);
    if(!layout)return out;
    out.table_layout=*layout;
    const auto api_list=reinterpret_cast<std::uint64_t>(list),native=list->get_native();
    auto &gs=out.graphics_state;auto &cs=out.compute_state;
    out.stage=OcclusionBindingRead::graphics;
    out.status=graphics(api_list,&gs,out.graphics_tables.data(),unsigned(out.graphics_tables.size()));
    if(out.status||gs.version!=1||!gs.outside_render_pass||gs.native_list!=native||
       (gs.level!=WWMI_GRAPHICS_RESTORE_EMPTY&&gs.level!=WWMI_GRAPHICS_RESTORE_TABLES)||
       gs.record_count>out.graphics_tables.size())return out;
    out.stage=OcclusionBindingRead::compute;
    std::uint32_t words[64];
    out.status=compute(api_list,&cs,out.compute_arguments.data(),unsigned(out.compute_arguments.size()),words,64);
    if(out.status||cs.version!=1||cs.native_list!=native||cs.root_signature!=ray.compute.root_signature||
       (cs.level!=WWMI_COMPUTE_RESTORE_TABLES&&cs.level!=WWMI_COMPUTE_RESTORE_ROOT_ARGUMENTS)||
       cs.record_count>out.compute_arguments.size()||!cs.cbv_srv_uav_heap||
       cs.cbv_srv_uav_heap!=gs.cbv_srv_uav_heap||cs.sampler_heap!=gs.sampler_heap)return out;
    std::uint64_t p4=0,p6=0;
    for(unsigned i=0;i<cs.record_count;++i){
        const auto &a=out.compute_arguments[i];
        if(a.kind!=WWMI_COMPUTE_ROOT_TABLE)continue;
        if(a.parameter==scene_srv_parameter)p4=a.value;
        if(a.parameter==scene_uav_parameter)p6=a.value;
    }
    if(p4!=ray.as.table||!p6)return out;
    out.stage=OcclusionBindingRead::rays_view;
    out.status=view(api_list,&ray.compute,scene_srv_parameter,1,&out.rays);
    if(out.status||out.rays.version!=1||out.rays.parameter!=scene_srv_parameter||out.rays.table_offset!=1||
       out.rays.kind!=WWMI_STRUCTURED_SLOT_SRV||out.rays.table!=p4||out.rays.stride!=32||
       out.rays.heap_generation!=ray.as.heap_generation||out.rays.heap_slot!=ray.as.heap_slot+1||
       !out.rays.resource||!out.rays.resource_creation)return out;
    out.stage=OcclusionBindingRead::output_view;
    out.status=view(api_list,&ray.compute,scene_uav_parameter,occlusion_output_slot.offset,&out.output);
    if(out.status||out.output.version!=1||out.output.parameter!=scene_uav_parameter||
       out.output.table_offset!=occlusion_output_slot.offset||
       out.output.kind!=WWMI_STRUCTURED_SLOT_UAV||out.output.table!=p6||out.output.stride!=occlusion_output_slot.stride||
       out.output.heap_generation!=ray.as.heap_generation||!out.output.resource||!out.output.resource_creation||
       (out.output.counter&&!out.output.counter_creation))return out;
    // MissShaderIndex is zero. A complete compatible native graph can
    // preserve that original record in place; otherwise require its actual
    // UPLOAD identifier as before. Neither read snapshots GPU contents.
    out.stage=OcclusionBindingRead::miss_record;
    out.miss_table=miss;
    std::array<std::byte,D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES> head;
    if(!miss.StartAddress||miss.StartAddress%D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT||
       miss.SizeInBytes<head.size()||miss.StrideInBytes>D3D12_RAYTRACING_MAX_SHADER_RECORD_STRIDE||
       miss.StrideInBytes%D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT||
       (miss.StrideInBytes&&(miss.StrideInBytes<head.size()||miss.SizeInBytes<miss.StrideInBytes)))return out;
    if(const auto closed=consumer.raygens.closed_occlusion_records({ray.compute.pipeline},ray.compute.root_signature,
        out.table_layout.generation);
        closed&&ray.raygen.key&&ray.raygen.bytes>=closed->raygen&&miss.SizeInBytes>=closed->miss&&
        (!miss.StrideInBytes||miss.StrideInBytes>=closed->miss)){
        out.miss=inputs.ray_input_range(miss.StartAddress,miss.SizeInBytes);
        if(!out.miss.key)return out;
        out.closed_sbt=closed;
        out.stage=OcclusionBindingRead::ready;
        return out;
    }
    out.miss=inputs.ray_input_range(miss.StartAddress,miss.SizeInBytes,head);
    if(!out.miss.upload_read||!consumer.raygens.matches_occlusion_miss({ray.compute.pipeline},head))return out;
    out.stage=OcclusionBindingRead::ready;
    return out;
}
}
