#pragma once
#include "hsin_inputs.hpp"
#include "rt_shape_position_input.h"

namespace hsin_rt {
// Client-internal C++ input. Do not exchange this type across DLLs. A producer
// adapter must COPY its values/COM references into it on the client side.
//
// This is an immutable, post-author-multiplier snapshot, not the mutable game's
// u0 and not the integer loader output. The producer must establish the actual
// association to native_identity; assigning the latest draw is not sufficient.
struct PostMultiplierShapeSnapshot {
    std::shared_ptr<const NativeBlasIdentity> native_identity;
    std::shared_ptr<const ModPack> pack; // exact loaded/selected pack revision
    ComPtr<ID3D12Resource> resource;
    ResourceKey key;                    // this owned snapshot's creation identity
    Recording producer;
    std::uint64_t byte_offset=0,bytes=0; // view, in BYTES; bytes == vertex_count*24
    unsigned first_vertex=0,vertex_count=0; // AUTHOR vertex domain, not native
    D3D12_RESOURCE_STATES read_state=D3D12_RESOURCE_STATE_COMMON;
    // Absent: only this exact actual recording can consume earlier commands.
    // Present: query this real retained fence; do not provide a planned Signal.
    ComPtr<ID3D12Fence> completion;
    std::uint64_t completion_value=0;
};

enum class ShapePositionStatus {
    recorded, missing_snapshot, missing_native_identity, stale_native_pose,
    wrong_pack_revision, author_policy_required, extra_mod_position_missing,
    bad_snapshot_resource, short_author_vertex_domain, producer_not_ready,
    wrong_consumer_recording, pose_rejected, record_failed
};
struct ShapePositionResult {
    ShapePositionStatus status=ShapePositionStatus::missing_snapshot;
    PoseResult pose_status=PoseResult::missing_producer;
    std::shared_ptr<GeometryBatch> batch;
};

// Copies a real optional provider result into client-owned values/references.
// No producer or synthetic successful response is supplied by this candidate.
std::shared_ptr<const PostMultiplierShapeSnapshot> acquire_post_multiplier_shape(
    WWMI_RtModAcquirePostMultiplierShapeFn_v1,ID3D12Device5 *,Recording consumer,
    const NativeDrawIdentityCache::Match &,std::shared_ptr<const ModPack>);

// Caller holds the existing device Runtime mutex. The two vectors must be the
// Runtime's actual records[epoch.list] and generated_records[epoch.list], not
// temporary vectors: pre/post submit and queue fences already own their lease.
// Records real private positions and dynamic tangents at the eligible producer.
// Do not build a BLAS until there is a proven ray consumer.
ShapePositionResult record_shape_positions(InputCollector &,NativeDrawIdentityCache &,
    api::command_list *actual_consumer,const NativeDrawIdentityCache::Match &,
    std::shared_ptr<GeometryAssets>,std::shared_ptr<const PostMultiplierShapeSnapshot>,
    std::span<const unsigned char> shape_dxil,std::span<const unsigned char> shape_tangent_dxil,ComputeRunFn,
    std::vector<std::shared_ptr<void>> &recording_holds,
    std::vector<std::shared_ptr<GeometryBatch>> &generated);

// The usual Runtime::gpu_ready(batch, current_recording) remains necessary.
// In particular a shared native pose alone does not authorize old shape data.
bool shape_batch_matches(const GeometryBatch &,const PostMultiplierShapeSnapshot &);
bool shape_snapshot_ready(const PostMultiplierShapeSnapshot &,Recording,ID3D12Device5 *);
}
