#include "shapekey_position_input.hpp"
#include <limits>

namespace hsin_rt {
namespace {
struct ShapePositionLease {PostMultiplierShapeSnapshot snapshot;};
bool valid(Recording r){return r.list&&r.creation&&r.generation;}
bool same_device(ID3D12DeviceChild *object,ID3D12Device5 *device){
    if(!object||!device)return false;
    ComPtr<IUnknown> owner,expected;
    return SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&owner)))&&
        SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&expected)))&&owner.Get()==expected.Get();
}
bool ready(const PostMultiplierShapeSnapshot &snapshot,Recording consumer,ID3D12Device5 *device){
    if(!valid(snapshot.producer))return false;
    if(snapshot.completion){
        if(!snapshot.completion_value||!same_device(snapshot.completion.Get(),device))return false;
        const auto done=snapshot.completion->GetCompletedValue();
        return done!=UINT64_MAX&&done>=snapshot.completion_value;
    }
    return !snapshot.completion_value&&snapshot.producer==consumer;
}
}

std::shared_ptr<const PostMultiplierShapeSnapshot> acquire_post_multiplier_shape(
    WWMI_RtModAcquirePostMultiplierShapeFn_v1 acquire,ID3D12Device5 *device,Recording consumer,
    const NativeDrawIdentityCache::Match &match,std::shared_ptr<const ModPack> pack){
    if(!acquire||!device||!pack||!pack->native||!match.identity||!valid(consumer)||
        match.identity->components.size()!=pack->components.size()||pack->components.size()>4096)return {};
    const auto &native=*match.identity;
    WWMI_RtShapePositionRequest_v1 request{};request.struct_size=sizeof(request);request.abi_version=1;
    request.object_guid=pack->native->object_guid;request.content_hash=pack->native->content_hash;
    request.device=reinterpret_cast<std::uint64_t>(device);request.consumer_list=consumer.list;
    request.consumer_creation=consumer.creation;request.consumer_recording=consumer.generation;
    request.load_generation=pack->load_generation;request.mod_key=pack->key;request.mod_revision=pack->revision;
    request.native_blas_resource=native.allocation.object;request.native_blas_creation=native.allocation.creation;
    request.native_blas_address=native.native_blas;request.native_list=native.recording.list;
    request.native_list_creation=native.recording.creation;request.native_recording=native.recording.generation;
    std::vector<WWMI_RtShapeComponent_v1> components;
    for(unsigned c=0;c<native.components.size();++c){
        const auto &n=native.components[c];const auto &s=n.sample;
        if(s.profile!=pack->native||s.component>=s.profile->components.size())return {};
        const auto &p=s.profile->components[s.component];WWMI_RtShapeComponent_v1 row{};
        row.component=p.component;row.native_first_vertex=p.first;row.native_vertex_count=p.count;row.skin_order=s.order;
        row.output_resource=s.output.object;row.output_creation=s.output.creation;row.output_offset=s.output_offset;row.output_bytes=s.output_bytes;
        row.content_resource=s.content.object;row.content_creation=s.content.creation;row.content_offset=s.content_offset;
        row.palette_resource=s.palette_resource.object;row.palette_creation=s.palette_resource.creation;
        row.palette_offset=s.palette_offset;row.palette_bytes=s.palette_bytes;
        row.index_resource=n.indices.object;row.index_creation=n.indices.creation;row.index_offset=n.index_offset;row.index_count=n.index_count;
        row.native_shape_resource=s.native_shape.resource.object;row.native_shape_creation=s.native_shape.resource.creation;
        row.native_shape_view_offset=s.native_shape.view_offset;row.native_shape_view_bytes=s.native_shape.view_bytes;
        row.native_shape_offset=s.native_shape.offset;row.native_shape_bytes=s.native_shape.bytes;
        row.native_shape_first_vertex=s.native_shape.first_vertex;row.native_shape_vertex_count=s.native_shape.vertex_count;
        components.push_back(row);
    }
    request.component_count=unsigned(components.size());WWMI_RtPostMultiplierShape_v1 out{};
    if(acquire(&request,components.data(),request.component_count,&out,sizeof(out))!=WWMI_RT_MOD_OK)return {};
    auto result=std::make_shared<PostMultiplierShapeSnapshot>();
    // Adopt every transferred reference before validating response fields, so
    // a rejected well-formed COM result does not leak a provider-owned lease.
    result->resource.Attach(reinterpret_cast<ID3D12Resource *>(out.resource));
    result->completion.Attach(reinterpret_cast<ID3D12Fence *>(out.completion_fence));
    if(out.struct_size!=sizeof(out)||out.abi_version!=1||out.layout!=WWMI_RT_SHAPE_AUTHOR_POSITION_NORMAL_FLOAT6)return {};
    result->native_identity=match.identity;result->pack=std::move(pack);
    result->key={out.resource,out.resource_creation};result->byte_offset=out.byte_offset;result->bytes=out.bytes;
    result->first_vertex=out.first_vertex;result->vertex_count=out.vertex_count;
    result->read_state=D3D12_RESOURCE_STATES(out.resource_state);
    result->producer={out.producer_list,out.producer_creation,out.producer_recording};result->completion_value=out.completion_value;
    return result;
}

bool shape_batch_matches(const GeometryBatch &batch,const PostMultiplierShapeSnapshot &snapshot){
    const auto &source=snapshot.native_identity;
    if(!source||batch.assets->pack!=snapshot.pack||batch.producer.recording!=source->recording||
        batch.producer.geometries.size()!=source->components.size()||batch.source_orders.size()!=source->components.size()||
        batch.final_positions.size()!=source->components.size())return false;
    for(unsigned c=0;c<source->components.size();++c){
        const auto &native=source->components[c];const auto &vertex=batch.producer.geometries[c];
        if(vertex.vertex!=native.vertices.vertex||vertex.offset!=native.vertices.offset||vertex.bytes!=native.vertices.bytes||
            batch.source_orders[c]!=native.sample.order)return false;
        if(!snapshot.pack->components[c].index_count)continue;
        const auto &input=batch.final_positions[c];
        if(input.mode!=position_author_shape_delta_float6||!input.hold||input.resource!=snapshot.key||
            input.first_vertex!=snapshot.first_vertex||input.vertex_count!=snapshot.vertex_count||input.bytes!=snapshot.bytes)return false;
        const auto &retained=static_cast<const ShapePositionLease *>(input.hold.get())->snapshot;
        if(retained.native_identity!=source||retained.byte_offset!=snapshot.byte_offset||retained.key!=snapshot.key)return false;
    }
    return true;
}

bool shape_snapshot_ready(const PostMultiplierShapeSnapshot &snapshot,Recording consumer,ID3D12Device5 *device){
    if(!snapshot.resource||!snapshot.key||snapshot.key.object!=reinterpret_cast<std::uint64_t>(snapshot.resource.Get())||
        !snapshot.pack||!snapshot.vertex_count||!same_device(snapshot.resource.Get(),device)||
        snapshot.first_vertex>snapshot.pack->vertices||snapshot.vertex_count>snapshot.pack->vertices-snapshot.first_vertex||
        snapshot.bytes!=24ull*snapshot.vertex_count||snapshot.byte_offset%4||
        (snapshot.read_state&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)==0||
        (snapshot.read_state&~D3D12_RESOURCE_STATE_GENERIC_READ)!=0)return false;
    const auto desc=snapshot.resource->GetDesc();const auto base=snapshot.resource->GetGPUVirtualAddress();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||!base||snapshot.byte_offset>desc.Width||
        snapshot.bytes>desc.Width-snapshot.byte_offset||base>UINT64_MAX-snapshot.byte_offset||
        snapshot.bytes>UINT32_MAX-(snapshot.byte_offset&15ull))return false;
    return ready(snapshot,consumer,device);
}

ShapePositionResult record_shape_positions(InputCollector &inputs,NativeDrawIdentityCache &identities,
    api::command_list *consumer,const NativeDrawIdentityCache::Match &match,
    std::shared_ptr<GeometryAssets> assets,std::shared_ptr<const PostMultiplierShapeSnapshot> snapshot,
    std::span<const unsigned char> shape_dxil,std::span<const unsigned char> shape_tangent_dxil,ComputeRunFn run,
    std::vector<std::shared_ptr<void>> &holds,std::vector<std::shared_ptr<GeometryBatch>> &generated){
    ShapePositionResult result;
    auto reject=[&](ShapePositionStatus status){result.status=status;return result;};
    if(!snapshot)return reject(ShapePositionStatus::missing_snapshot);
    if(!match.identity||!snapshot->native_identity||match.identity!=snapshot->native_identity)
        return reject(ShapePositionStatus::missing_native_identity);
    if(!assets||!snapshot->pack||assets->pack!=snapshot->pack||!assets->pack->shapekey_vertices)
        return reject(ShapePositionStatus::wrong_pack_revision);
    const auto &pack=*assets->pack;const auto &source=*match.identity;
    if(!pack.native||source.components.size()!=pack.components.size()||source.geometries.size()!=pack.components.size()||
        match.component>=source.components.size())return reject(ShapePositionStatus::missing_native_identity);
    const auto epoch=consumer?inputs.recording(consumer):Recording{};
    if(!valid(epoch)||epoch.list!=reinterpret_cast<std::uint64_t>(consumer)||!run)
        return reject(ShapePositionStatus::wrong_consumer_recording);
    if(epoch!=source.recording&&!match.ordering)return reject(ShapePositionStatus::missing_native_identity);
    for(unsigned c=0;c<source.components.size();++c){
        if(source.components[c].sample.profile!=pack.native||!identities.live(match.identity,c)||
            !inputs.position_live(source.components[c].sample))return reject(ShapePositionStatus::stale_native_pose);
        const auto &component=pack.components[c];if(!component.index_count)continue;
        if(!component.selection_planned||!component.mod_author_shared)return reject(ShapePositionStatus::author_policy_required);
        if(component.extra_position_input)return reject(ShapePositionStatus::extra_mod_position_missing);
        if(component.vertex_first<snapshot->first_vertex||component.vertex_first-snapshot->first_vertex>snapshot->vertex_count||
            component.vertex_count>snapshot->vertex_count-(component.vertex_first-snapshot->first_vertex))
            return reject(ShapePositionStatus::short_author_vertex_domain);
    }
    // Only a strongly held, immutable view of the author's float6 domain. We
    // never look up a source by VA or read an unowned native object here.
    if(!snapshot->resource||!snapshot->key||snapshot->key.object!=reinterpret_cast<std::uint64_t>(snapshot->resource.Get())||
        !same_device(snapshot->resource.Get(),assets->device.Get())||!snapshot->vertex_count||
        snapshot->first_vertex>pack.vertices||snapshot->vertex_count>pack.vertices-snapshot->first_vertex||
        snapshot->bytes!=24ull*snapshot->vertex_count||snapshot->byte_offset%4||
        (snapshot->read_state&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)==0||
        (snapshot->read_state&~D3D12_RESOURCE_STATE_GENERIC_READ)!=0)
        return reject(ShapePositionStatus::bad_snapshot_resource);
    const auto desc=snapshot->resource->GetDesc();const auto base=snapshot->resource->GetGPUVirtualAddress();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||!base||snapshot->byte_offset>desc.Width||
        snapshot->bytes>desc.Width-snapshot->byte_offset||base>UINT64_MAX-snapshot->byte_offset||
        snapshot->bytes>UINT32_MAX-(snapshot->byte_offset&15ull))
        return reject(ShapePositionStatus::bad_snapshot_resource);
    if(!ready(*snapshot,epoch,assets->device.Get()))return reject(ShapePositionStatus::producer_not_ready);
    // Own a frozen copy of every COM/value lease. Releasing or resetting the
    // caller's input object after this call cannot retire the batch's source.
    auto lease=std::make_shared<ShapePositionLease>();lease->snapshot=*snapshot;

    PoseInputs values;values.reset(source.recording);BlasPoseInput input{source.recording,{}};
    std::vector<FinalPosition> positions(pack.components.size());
    for(unsigned c=0;c<source.components.size();++c){
        const auto &native=source.components[c];
        if(!values.add(native.sample))return reject(ShapePositionStatus::stale_native_pose);
        input.geometries.push_back(native.vertices);
        if(!pack.components[c].index_count)continue;
        auto &out=positions[c];out.mode=position_author_shape_delta_float6;out.resource=snapshot->key;
        out.address=base+snapshot->byte_offset;out.bytes=snapshot->bytes;
        out.first_vertex=snapshot->first_vertex;out.vertex_count=snapshot->vertex_count;out.hold=lease;
    }
    auto pose=values.assemble(input,assets->pack,std::move(positions));result.pose_status=pose.result;
    if(pose.result!=PoseResult::complete)return reject(ShapePositionStatus::pose_rejected);
    assets->enable_shape_positions(shape_dxil,shape_tangent_dxil);
    auto batch=GeometryBatch::prepare_streams(std::move(assets),*pose.pose);
    batch->gpu_recording=epoch;
    // Generation and recording must still be the actual consumer's. No global
    // fence waits, guessed queues or unsubmitted/previous-pose fallbacks.
    if(inputs.recording(consumer)!=epoch)return reject(ShapePositionStatus::wrong_consumer_recording);
    holds.push_back(batch);generated.push_back(batch);result.batch=batch;
    if(batch->record(epoch.list,run)!=0||!batch->recorded)return reject(ShapePositionStatus::record_failed);
    result.status=ShapePositionStatus::recorded;return result;
}
}
