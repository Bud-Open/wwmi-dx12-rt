#pragma once
#include "hsin_pose.hpp"
#include "wwmi_compute_state.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <span>

namespace hsin_rt {
using Microsoft::WRL::ComPtr;
inline constexpr D3D12_RESOURCE_STATES position_read_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
using ComputeRunFn=decltype(&WWMI_ComputeState_Run_v1);
struct GeometryAssets {
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12Resource> positions,blends,indices,vectors,uv,color;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline,copy_pipeline;
    ComPtr<ID3D12RootSignature> copy_root,tangent_root;
    ComPtr<ID3D12PipelineState> tangent_pipeline;
    ComPtr<ID3D12RootSignature> shape_root,shape_tangent_root;
    ComPtr<ID3D12PipelineState> shape_pipeline,shape_tangent_pipeline;
    std::shared_ptr<const ModPack> pack;
    bool raster_only=false;
    static std::shared_ptr<GeometryAssets> create(ID3D12Device5 *unwrapped_device,
        std::shared_ptr<const ModPack> pack,std::span<const unsigned char> skin_dxil,std::span<const unsigned char> copy_dxil,std::span<const unsigned char> tangent_dxil,bool raster_only=false);
    void enable_shape_positions(std::span<const unsigned char> position,std::span<const unsigned char> tangent);
};
struct GeometryBatch {
    std::shared_ptr<GeometryAssets> assets;
    ComPtr<ID3D12Resource> palettes,positions,blas,scratch,tangents;
    std::vector<bool> tangent_ready;
    Recording gpu_recording;unsigned gpu_submissions=0;std::uint64_t gpu_queue=0,gpu_fence=0,gpu_value=0;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
    std::vector<float> scales;
    BlasPoseInput producer;
    std::vector<std::uint64_t> source_orders;
    std::vector<bool> native_extra_input;
    std::vector<std::uint32_t> native_delta_flags;
    std::vector<FinalPosition> final_positions;
    bool recording_started=false,recorded=false;
    unsigned single_component=UINT32_MAX; // previous-only output; UINT32_MAX keeps the existing full surface path
    static std::shared_ptr<GeometryBatch> prepare_streams(std::shared_ptr<GeometryAssets> assets,const Pose &pose,unsigned single_component=UINT32_MAX);
    static std::shared_ptr<GeometryBatch> prepare(std::shared_ptr<GeometryAssets> assets,
        const Pose &pose,const std::vector<D3D12_RAYTRACING_GEOMETRY_FLAGS> &native_flags);
    // Same evaluated triangle topology as the raster streams. An actual ray
    // consumer can request this before recording; raster-only batches do not
    // allocate acceleration structures or scratch.
    void prepare_acceleration(std::span<const D3D12_RAYTRACING_GEOMETRY_FLAGS> native_flags);
    // Put this batch in the recording's ownership list BEFORE calling record.
    // A recording and every submission retain it until reset/destruction and
    // the corresponding queue fences respectively. Failure is not GPU retirement.
    std::int32_t record(std::uint64_t api_list,ComputeRunFn run);
    // Fresh owned DIRECT list: REST or completed immutable author inputs.
    // A copy recorded in the later application list is not a completed input.
    bool record_owned(ID3D12GraphicsCommandList4 *);
    ComPtr<ID3D12Resource> inactive_triangle;
};
// The caller owns one exact immutable CPU previous pose/component. Only its
// read-only palette may be shared before the actual consumer queue is known.
std::shared_ptr<GeometryBatch> prepare_owned_previous_streams(std::shared_ptr<GeometryAssets> assets,
    const Pose &pose,unsigned component,ComPtr<ID3D12Resource> &palette);
bool completed_author_inputs_valid(const Pose &,unsigned component,ID3D12Device5 *);
}
#include "author_completed_input.hpp"
