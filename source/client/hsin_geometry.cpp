#include "gpu_helpers.hpp"
#include "author_static_assets.hpp"
#include "author_stream_pool.hpp"
#include "author_owned_recording.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace hsin_rt {
namespace gpu {
void check(HRESULT result,const char *operation) {
    if(FAILED(result))throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(result));
}
ComPtr<ID3D12Resource> buffer(ID3D12Device5 *device,UINT64 bytes,D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES properties{};properties.Type=heap;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
    desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;
    desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,
        IID_PPV_ARGS(&resource)),"Hsin resource");return resource;
}
void upload(ID3D12Resource *resource,const void *data,SIZE_T bytes) {
    void *dest=nullptr;D3D12_RANGE reads{0,0};check(resource->Map(0,&reads,&dest),"Hsin upload Map");
    std::memcpy(dest,data,bytes);D3D12_RANGE writes{0,bytes};resource->Unmap(0,&writes);
}
ComPtr<ID3D12Resource> input_buffer(ID3D12Device5 *device,std::span<const unsigned char> bytes) {
    auto r=buffer(device,bytes.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    upload(r.Get(),bytes.data(),bytes.size());return r;
}
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS build_input(const GeometryBatch &batch) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
    input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;input.NumDescs=UINT(batch.geometries.size());input.pGeometryDescs=batch.geometries.data();
    // This is an owned BUILD, never an UPDATE using the game's source allocation.
    return input;
}
void uav_barrier(ID3D12GraphicsCommandList4 *list,ID3D12Resource *resource) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource=resource;list->ResourceBarrier(1,&barrier);
}
void pipeline(ID3D12Device5 *device,const D3D12_ROOT_PARAMETER *params,UINT count,
    std::span<const unsigned char> dxil,ComPtr<ID3D12RootSignature> &root,ComPtr<ID3D12PipelineState> &state) {
    D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=count;desc.pParameters=params;
    ComPtr<ID3DBlob> serialized,errors;
    check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"RT root serialize");
    check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)),"RT root create");
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.CS={dxil.data(),dxil.size()};
    check(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&state)),"RT compute PSO");
}
std::int32_t record_native(std::uint64_t native_list,void *context) {
    auto &b=*static_cast<GeometryBatch *>(context);
    auto *list=reinterpret_cast<ID3D12GraphicsCommandList4 *>(native_list);
    b.recording_started=true;
    if(!copy_completed_author_pairs(b,list)){
        D3D12_RESOURCE_BARRIER begin{};begin.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        begin.Transition={b.positions.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,position_read_state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        list->ResourceBarrier(1,&begin);
        if(b.tangents){auto barrier=begin;barrier.Transition.pResource=b.tangents.Get();list->ResourceBarrier(1,&barrier);}
    }
    if(b.blas){uav_barrier(list,b.blas.Get());uav_barrier(list,b.scratch.Get());}
    const auto &pack=*b.assets->pack;
    const bool raster=b.assets->raster_only;
    for(unsigned c=0;c<(raster?1u:pack.components.size());++c){const auto &component=pack.components[c];
        if(!raster&&(!component.index_count||(b.single_component!=UINT32_MAX&&c!=b.single_component)))continue;
        const auto first=raster?0:component.vertex_first,vertices=raster?pack.vertices:component.vertex_count;
        const auto &final=b.final_positions[c];
        if(final.mode==WWMI_RT_POSITION_FINAL&&final.completed_author){
            // This component's positions and vectors were copied together.
        }else if(final.mode==WWMI_RT_POSITION_FINAL){
            list->SetComputeRootSignature(b.assets->copy_root.Get());list->SetPipelineState(b.assets->copy_pipeline.Get());
            list->SetComputeRootShaderResourceView(0,final.address);list->SetComputeRootUnorderedAccessView(1,b.positions->GetGPUVirtualAddress());
            UINT constants[]{vertices,first-final.first_vertex,first};
            list->SetComputeRoot32BitConstants(2,3,constants,0);list->Dispatch((constants[0]+63)/64,1,1);
        }else if(final.mode==position_author_shape_delta_float6){
            list->SetComputeRootSignature(b.assets->shape_root.Get());list->SetPipelineState(b.assets->shape_pipeline.Get());
            list->SetComputeRootShaderResourceView(0,b.assets->positions->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1,b.assets->blends->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2,b.palettes->GetGPUVirtualAddress()+c*std::uint64_t(pack.canonical_bones)*sizeof(Matrix));
            // Align the raw root SRV without changing the snapshot's byte view.
            const auto address=final.address&~15ull;const auto bias=UINT(final.address-address);
            list->SetComputeRootShaderResourceView(3,address);
            list->SetComputeRootUnorderedAccessView(4,b.positions->GetGPUVirtualAddress());
            UINT constants[]{vertices,first,first,std::bit_cast<UINT>(b.scales[c]),final.first_vertex,bias};
            list->SetComputeRoot32BitConstants(5,6,constants,0);list->Dispatch((constants[0]+63)/64,1,1);
        }else{
            list->SetComputeRootSignature(b.assets->root.Get());list->SetPipelineState(b.assets->pipeline.Get());
            list->SetComputeRootShaderResourceView(0,b.assets->positions->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1,b.assets->blends->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2,b.palettes->GetGPUVirtualAddress()+c*std::uint64_t(pack.canonical_bones)*sizeof(Matrix));
            list->SetComputeRootUnorderedAccessView(3,b.positions->GetGPUVirtualAddress());
            UINT constants[]{vertices,first,first,std::bit_cast<UINT>(b.scales[c])};
            list->SetComputeRoot32BitConstants(4,4,constants,0);list->Dispatch((constants[0]+63)/64,1,1);
        }
        if(b.tangents&&final.mode==position_author_shape_delta_float6&&b.assets->shape_tangent_pipeline){
            list->SetComputeRootSignature(b.assets->shape_tangent_root.Get());list->SetPipelineState(b.assets->shape_tangent_pipeline.Get());
            list->SetComputeRootShaderResourceView(0,b.assets->vectors->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1,b.assets->blends->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2,b.palettes->GetGPUVirtualAddress()+c*std::uint64_t(pack.canonical_bones)*sizeof(Matrix));
            const auto address=final.address&~15ull;const auto bias=UINT(final.address-address);
            list->SetComputeRootShaderResourceView(3,address);list->SetComputeRootUnorderedAccessView(4,b.tangents->GetGPUVirtualAddress());
            UINT constants[]{vertices,first,first,std::bit_cast<UINT>(b.scales[c]),final.first_vertex,bias};
            list->SetComputeRoot32BitConstants(5,6,constants,0);list->Dispatch((constants[0]+63)/64,1,1);b.tangent_ready[c]=true;
        }else if(b.tangents&&final.mode!=WWMI_RT_POSITION_FINAL&&final.mode!=position_author_shape_delta_float6){
            list->SetComputeRootSignature(b.assets->tangent_root.Get());list->SetPipelineState(b.assets->tangent_pipeline.Get());
            list->SetComputeRootShaderResourceView(0,b.assets->vectors->GetGPUVirtualAddress());list->SetComputeRootShaderResourceView(1,b.assets->blends->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2,b.palettes->GetGPUVirtualAddress()+c*std::uint64_t(pack.canonical_bones)*sizeof(Matrix));
            list->SetComputeRootUnorderedAccessView(3,b.tangents->GetGPUVirtualAddress());
            UINT constants[]{vertices,first,first,std::bit_cast<UINT>(b.scales[c])};
            list->SetComputeRoot32BitConstants(4,4,constants,0);list->Dispatch((vertices+63)/64,1,1);b.tangent_ready[c]=true;
        }
    }
    uav_barrier(list,b.positions.Get());
    D3D12_RESOURCE_BARRIER read{};read.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    read.Transition={b.positions.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,position_read_state};
    list->ResourceBarrier(1,&read);
    if(b.tangents){uav_barrier(list,b.tangents.Get());auto barrier=read;barrier.Transition.pResource=b.tangents.Get();list->ResourceBarrier(1,&barrier);}
    if(!b.blas){b.recorded=true;return 0;}
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};desc.Inputs=build_input(b);
    desc.DestAccelerationStructureData=b.blas->GetGPUVirtualAddress();
    desc.ScratchAccelerationStructureData=b.scratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&desc,0,nullptr);uav_barrier(list,b.blas.Get());
    b.recorded=true;return 0;
}
}

using namespace gpu;

std::shared_ptr<GeometryAssets> GeometryAssets::create(ID3D12Device5 *device,std::shared_ptr<const ModPack> pack,
    std::span<const unsigned char> dxil,std::span<const unsigned char> copy_dxil,std::span<const unsigned char> tangent_dxil,
    bool raster_only){
    if(!device||!pack||pack->positions.size()!=12ull*pack->vertices||pack->blends.size()!=32ull*pack->vertices||
       (raster_only?!pack->indices.empty():(pack->indices.empty()||pack->indices.size()%4))||dxil.empty()||copy_dxil.empty())throw std::runtime_error("RT Mod asset layout");
    for(std::size_t offset=0;offset<pack->indices.size();offset+=4){UINT index;std::memcpy(&index,pack->indices.data()+offset,4);if(index>=pack->vertices)throw std::runtime_error("Mod index outside owned vertex range");}
    const auto verify_bones=[&](unsigned first,unsigned count){
      for(unsigned v=first;v<first+count;++v)for(unsigned slot=0;slot<8;++slot){std::uint16_t bone;std::memcpy(&bone,pack->blends.data()+32ull*v+2*slot,2);if(bone>=pack->canonical_bones)throw std::runtime_error("Mod bone outside canonical palette");}
    };
    if(raster_only)verify_bones(0,pack->vertices);
    else for(const auto &component:pack->components)verify_bones(component.vertex_first,component.vertex_count);
    auto result=std::make_shared<GeometryAssets>();result->device=device;result->pack=std::move(pack);
    result->raster_only=raster_only;
    result->positions=input_buffer(device,result->pack->positions);result->blends=input_buffer(device,result->pack->blends);
    if(!raster_only)result->indices=input_buffer(device,result->pack->indices);
    D3D12_ROOT_PARAMETER params[5]{};
    for(UINT i=0;i<3;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[i].Descriptor.ShaderRegister=i;}
    params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[3].Descriptor.ShaderRegister=0;
    params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[4].Constants={0,0,4};
    gpu::pipeline(device,params,5,dxil,result->root,result->pipeline);
    if(!result->pack->uv.empty())result->uv=input_buffer(device,result->pack->uv);
    if(!result->pack->color.empty())result->color=input_buffer(device,result->pack->color);
    if(result->pack->surface_supported&&!tangent_dxil.empty()){
        result->vectors=input_buffer(device,result->pack->vectors);
        gpu::pipeline(device,params,5,tangent_dxil,result->tangent_root,result->tangent_pipeline);
    }
    D3D12_ROOT_PARAMETER copy_params[3]{};copy_params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    copy_params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;copy_params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;copy_params[2].Constants={0,0,3};
    gpu::pipeline(device,copy_params,3,copy_dxil,result->copy_root,result->copy_pipeline);return result;
}

template<class Assets> static auto static_buffers(Assets &assets){
    // WRL overloads operator& for out parameters. These are addresses of
    // the ComPtr objects themselves, with Assets' const qualification.
    return std::array{std::addressof(assets.positions),std::addressof(assets.blends),std::addressof(assets.indices),
        std::addressof(assets.vectors),std::addressof(assets.uv),std::addressof(assets.color)};
}
std::shared_ptr<GeometryAssets> make_default_author_assets(const GeometryAssets &bootstrap){
    // The short-lived bootstrap is also the transfer source. Reuse its exact
    // data and pipeline objects; do not upload a second CPU copy.
    auto target=std::make_shared<GeometryAssets>(bootstrap);
    for(auto *slot:static_buffers(*target))if(*slot)
        *slot=buffer(target->device.Get(),(*slot)->GetDesc().Width,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    return target;
}
void record_author_asset_upload(ID3D12GraphicsCommandList4 *list,const GeometryAssets &target,const GeometryAssets &bootstrap){
    const auto sources=static_buffers(bootstrap),destinations=static_buffers(target);
    std::array<D3D12_RESOURCE_BARRIER,6> barriers{};
    UINT count=0;
    for(std::size_t i=0;i<sources.size();++i)if(auto *src=sources[i]->Get()){
        auto *dst=destinations[i]->Get();
        list->CopyBufferRegion(dst,0,src,0,src->GetDesc().Width);
        auto &barrier=barriers[count++];barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={dst,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_GENERIC_READ};
    }
    list->ResourceBarrier(count,barriers.data());
}

void GeometryAssets::enable_shape_positions(std::span<const unsigned char> dxil,std::span<const unsigned char> tangent_dxil){
    if(!shape_pipeline&&dxil.empty())throw std::runtime_error("post-multiplier ShapePositions.cso missing");
    D3D12_ROOT_PARAMETER params[6]{};
    for(UINT i=0;i<4;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[i].Descriptor.ShaderRegister=i;}
    params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[4].Descriptor.ShaderRegister=0;
    params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[5].Constants={0,0,6};
    if(!shape_pipeline)gpu::pipeline(device.Get(),params,6,dxil,shape_root,shape_pipeline);
    if(vectors&&!shape_tangent_pipeline&&!tangent_dxil.empty())
        gpu::pipeline(device.Get(),params,6,tangent_dxil,shape_tangent_root,shape_tangent_pipeline);
}

std::shared_ptr<AuthorStreamPool::Storage> AuthorStreamPool::acquire(ID3D12Device5 *device,
    std::uint64_t positions,std::uint64_t tangents,bool &reused){
    std::shared_ptr<Storage> best;
    for(const auto &candidate:storage){
        if(candidate.use_count()!=1||bool(candidate->tangent_capacity)!=bool(tangents)||
           candidate->position_capacity<positions||candidate->tangent_capacity<tangents)continue;
        if(!best||candidate->position_capacity<best->position_capacity)best=candidate;
    }
    reused=bool(best);
    if(best)return best;
    auto allocation=std::make_shared<Storage>();
    allocation->positions=buffer(device,positions,D3D12_HEAP_TYPE_DEFAULT,position_read_state,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    allocation->position_capacity=positions;
    if(tangents){
        allocation->tangents=buffer(device,tangents,D3D12_HEAP_TYPE_DEFAULT,position_read_state,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        allocation->tangent_capacity=tangents;
    }
    storage.push_back(allocation);
    return allocation;
}
void AuthorStreamPool::release_idle()noexcept{
    std::erase_if(storage,[](const auto &entry){return entry.use_count()==1;});
}
static std::shared_ptr<GeometryBatch> prepare_streams_impl(std::shared_ptr<GeometryAssets> assets,const Pose &pose,
    unsigned single_component,ID3D12Resource *owned_palette,AuthorStreamPool *pool=nullptr,bool *reused=nullptr,
    std::uint64_t generation=0,bool default_static=false) {
    if(!assets||assets->pack!=pose.pack)throw std::runtime_error("RT Mod assets/pose mismatch");
    if(single_component!=UINT32_MAX&&single_component>=pose.pack->components.size())throw std::runtime_error("RT previous component out of range");
    const auto positions=std::uint64_t(assets->pack->vertices)*12;
    const auto tangents=assets->vectors&&single_component==UINT32_MAX?std::uint64_t(assets->pack->vertices)*32:0;
    auto *device=assets->device.Get();
    std::shared_ptr<GeometryBatch> b;
    if(pool){
        struct Lease {std::shared_ptr<AuthorStreamPool::Storage> storage;GeometryBatch batch;};
        auto lease=std::make_shared<Lease>();
        lease->storage=pool->acquire(device,positions,tangents,*reused);
        lease->batch.positions=lease->storage->positions;lease->batch.tangents=lease->storage->tangents;
        b=std::shared_ptr<GeometryBatch>(lease,&lease->batch);
        lease->storage->resident=b;
        lease->storage->generation=generation;lease->storage->default_static=default_static;
        lease->storage->coverage=single_component;
    }else b=std::make_shared<GeometryBatch>();
    b->assets=std::move(assets);
    b->single_component=single_component;
    b->producer={pose.recording,pose.source_vertices};b->source_orders=pose.source_orders;b->scales=pose.scales;b->native_extra_input=pose.native_extra_input;b->native_delta_flags=pose.native_delta_flags;b->final_positions=pose.final_positions;
    if(owned_palette)b->palettes=owned_palette;
    else{
        b->palettes=buffer(device,std::max<std::size_t>(sizeof(Matrix),pose.rows.size()*sizeof(Matrix)),
            D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        if(!pose.rows.empty())upload(b->palettes.Get(),pose.rows.data(),pose.rows.size()*sizeof(Matrix));
    }
    // Generation-owned output: a retained graphics recording must not observe
    // a later pose overwriting the same allocation on an unknown future queue.
    b->tangent_ready.assign(pose.pack->components.size(),false);
    if(!pool){
        if(tangents)b->tangents=buffer(device,tangents,D3D12_HEAP_TYPE_DEFAULT,position_read_state,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        b->positions=buffer(device,positions,D3D12_HEAP_TYPE_DEFAULT,position_read_state,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    // Each new batch owns its BLAS and scratch. Completion of generation is
    // not retirement of TLAS/ray consumers which still reference an old BLAS.
    return b;
}
std::shared_ptr<GeometryBatch> prepare_pooled_author_streams(std::shared_ptr<GeometryAssets> assets,const Pose &pose,
    unsigned component,ComPtr<ID3D12Resource> *palette,AuthorStreamPool &pool,bool &reused,
    std::uint64_t generation,bool default_static){
    auto b=prepare_streams_impl(std::move(assets),pose,component,palette?palette->Get():nullptr,&pool,&reused,generation,default_static);
    if(palette&&!*palette)*palette=b->palettes;
    return b;
}
std::shared_ptr<GeometryBatch> GeometryBatch::prepare_streams(std::shared_ptr<GeometryAssets> assets,const Pose &pose,unsigned single_component) {
    return prepare_streams_impl(std::move(assets),pose,single_component,nullptr);
}
std::shared_ptr<GeometryBatch> prepare_owned_previous_streams(std::shared_ptr<GeometryAssets> assets,const Pose &pose,
    unsigned component,ComPtr<ID3D12Resource> &palette) {
    if(component==UINT32_MAX)throw std::runtime_error("RT owned previous requires one component");
    auto b=prepare_streams_impl(std::move(assets),pose,component,palette.Get());
    if(!palette)palette=b->palettes;
    return b;
}
std::shared_ptr<GeometryBatch> GeometryBatch::prepare(std::shared_ptr<GeometryAssets> assets,
    const Pose &pose,const std::vector<D3D12_RAYTRACING_GEOMETRY_FLAGS> &flags){
    auto b=prepare_streams(std::move(assets),pose);b->prepare_acceleration(flags);return b;
}
void GeometryBatch::prepare_acceleration(std::span<const D3D12_RAYTRACING_GEOMETRY_FLAGS> flags){
    if(!assets||assets->raster_only||!positions||recording_started||blas||single_component!=UINT32_MAX||flags.size()!=assets->pack->components.size())
        throw std::runtime_error("RT Mod geometry flags or stream batch mismatch");
    auto *device=assets->device.Get();
    geometries.resize(flags.size());
    for(unsigned c=0;c<flags.size();++c) {
        const auto &component=assets->pack->components[c];auto &g=geometries[c];g={};g.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;g.Flags=flags[c];
        g.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
        if(!component.index_count){
            if(!component.selection_planned||!component.draws.empty())throw std::runtime_error("RT empty geometry lacks an evaluated selection");
            // Retain GeometryIndex and native material contribution order. DXR
            // treats a triangle whose vertex X is NaN as inactive, not a hit.
            if(!inactive_triangle){
                constexpr std::array<std::uint32_t,9> vertices{0x7fc00000u,0,0,0x7fc00000u,0,0,0x7fc00000u,0,0};
                inactive_triangle=buffer(device,sizeof(vertices),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
                upload(inactive_triangle.Get(),vertices.data(),sizeof(vertices));
            }
            g.Triangles.VertexBuffer={inactive_triangle->GetGPUVirtualAddress(),12};g.Triangles.VertexCount=3;
            g.Triangles.IndexFormat=DXGI_FORMAT_UNKNOWN;continue;
        }
        g.Triangles.VertexBuffer={positions->GetGPUVirtualAddress(),12};g.Triangles.VertexCount=assets->pack->vertices;
        g.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
        g.Triangles.IndexBuffer=assets->indices->GetGPUVirtualAddress()+4ull*component.index_first;
        g.Triangles.IndexCount=component.index_count;
    }
    auto input=build_input(*this);D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
    if(!info.ResultDataMaxSizeInBytes || !info.ScratchDataSizeInBytes)throw std::runtime_error("RT Mod BLAS prebuild sizes");
    blas=buffer(device,info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    scratch=buffer(device,info.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
}
std::int32_t GeometryBatch::record(std::uint64_t list,ComputeRunFn run) {
    if(!list || !run || recording_started)return 3;
    return run(list,record_native,this);
}
bool GeometryBatch::record_owned(ID3D12GraphicsCommandList4 *list) {
    if(!list||list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT||recording_started||
       !completed_author_batch_valid(*this))return false;
    return record_native(reinterpret_cast<std::uint64_t>(list),this)==0&&recorded;
}
}
