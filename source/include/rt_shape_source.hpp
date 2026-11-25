#pragma once
#include "rt_shape_final_source.h"
#include <d3d12.h>
#include <cstddef>
#include <memory>
#include <utility>

struct WWMI_RtShapePositionRequest_v1;
struct WWMI_RtShapeComponent_v1;
struct WWMI_RtNativeShapeLink_v1;

namespace wwmi::deform {
using Recording = WWMI_RtDeformedRecording_v1;
using Selection = WWMI_RtDeformedSelection_v1;
using View = WWMI_RtDeformedView_v1;
struct PublicationState;

template<class T> struct Slice {
    const T *values = nullptr;
    size_t count = 0;
    const T *begin() const noexcept { return values; }
    const T *end() const noexcept { return count ? values + count : values; }
    const T *data() const noexcept { return values; }
    size_t size() const noexcept { return count; }
    bool empty() const noexcept { return count == 0; }
    const T &front() const noexcept { return *values; }
    const T &operator[](size_t i) const noexcept { return values[i]; }
};

// The actual draw/scene owner owns these publications and the command list.
// Keep a publication in the existing recording/flight holds before issuing
// work; discard an unsuccessful private prefix, never execute its partial work.
class Publication {
    std::shared_ptr<PublicationState> data_;
    friend class ScopedSourceRead;
public:
    Publication() = default;
    explicit Publication(std::shared_ptr<PublicationState> data) : data_(std::move(data)) {}
    explicit operator bool() const noexcept { return bool(data_); }
    const Selection *selection(uint32_t component) const noexcept;

    // Only after the one-shot producer's real Execute and successful Signal.
    // Reset/unload invalidates fresh acquisition, not existing GPU flight holds.
    bool submitted_once(Recording, ID3D12CommandQueue *, ID3D12Fence *, uint64_t value) const;
    void invalidate() const;
};

class ScopedSourceRead {
    std::shared_ptr<PublicationState> hold_;
    PublicationState *outer_;
public:
    explicit ScopedSourceRead(const Publication &) noexcept;
    ~ScopedSourceRead();
    ScopedSourceRead(const ScopedSourceRead &) = delete;
    ScopedSourceRead &operator=(const ScopedSourceRead &) = delete;
};

struct RestSelection {
    uint64_t completed_vm_recipe; // actual evaluated author recipe, not geometry_revision
    uint64_t immutable_position_asset; // actual independently owned author input generation
    std::shared_ptr<const void> position_asset; // owner of that exact immutable input
};
Publication publish_author_rest(Selection owner, RestSelection);

struct FinalWrite {
    Recording producer;
    uint64_t deformation_stamp, write_order; // real per-output write receipt
    uint32_t first_vertex, vertex_count;
    View view;
};
struct FinalPair {
    ID3D12GraphicsCommandList *one_shot;
    FinalWrite position, vectors;
};
// Call at the real final producer, after both matched writes and transitions.
// Both views must be immutable for their leases; producer recording is owned
// and once-only. Neither a mutable external buffer nor a pointer-only receipt
// can be authorized by this entry point.
Publication publish_final_pair(Selection owner, const FinalPair &);

// Exact approved Core descriptors from stable asset/writer leases. The asset
// owner validates ID and control-table domains once against the loaded files;
// do not sample arbitrary live descriptors or trust the section name alone.
struct ShapeTypedInput {
    ID3D12Resource *resource;
    uint64_t creation, first_element;
    uint32_t elements;
    DXGI_FORMAT format;
    D3D12_RESOURCE_STATES state;
};
struct ShapeFrozenCb {
    uint64_t native_call, snapshot; // actual stable snapshot receipt for this read
    uint32_t words[264];
};
struct ShapeBatch {
    uint32_t checksum, original_offset, author_offset, author_entries;
    uint32_t loader_groups_y; // actual evaluated custom dispatch argument
    ShapeFrozenCb loader_cb;
};
struct ShapeValuesWrite {
    Recording producer;
    uint64_t order;
    ID3D12Fence *fence; // actual successful Signal after producer Execute, if not this prefix
    uint64_t fence_value;
};
struct ShapeRecipe {
    uint64_t evaluated_recipe; // actual VM closure
    ShapeValuesWrite values_write;
    // Exact core CustomShader section keys from the parsed recipe. These
    // select kernels, not the instance, source writer or selected Mod.
    const wchar_t *overrider, *loader, *multiplier;
    uint32_t author_vertices, multiplier_groups_y;
    Slice<ShapeBatch> batches; // actual Core table order (not command order)
    ShapeFrozenCb multiplier_cb;
    ShapeTypedInput offsets, vertex_ids, vertex_deltas, values;
    // values must be from the completed matching write or written previously
    // in this exact prefix and then kept immutable through prefix completion.
};
struct ShapeJob {
    ID3D12Device *device;
    ID3D12GraphicsCommandList *one_shot; // actual raw DIRECT prefix, outside any pass
    Recording producer;                // actual private list address and generations
    Slice<Selection> owners; // only the selected components using this float6
    const WWMI_RtShapePositionRequest_v1 *legacy_pose; // optional whole pose for the unchanged v1 adapter
    Slice<WWMI_RtShapeComponent_v1> components; // actual native input rows; full v1 pose if legacy_pose is set
    Slice<WWMI_RtNativeShapeLink_v1> links; // one exact t3 read per owner
    const ShapeRecipe *recipe;
};

// Replays only already evaluated Shape compute, using the loaded VM core
// shaders and immutable selected inputs. Does not re-run Mod/FrameStep/other
// sections or touch original u0/u1. On failure, discard this one-shot list.
Publication record_author_shape(const ShapeJob &);

// IssueDispatch's selected-scope descriptor path. Only this one private list
// can use it; normal IssueDispatch never resolves bindings through this API.
struct RootSlot { unsigned param; char kind; unsigned reg, space, type, dimension; };
bool shape_dispatch_active(ID3D12GraphicsCommandList *) noexcept;
bool bind_shape_dispatch(ID3D12GraphicsCommandList *, ID3D12RootSignature *, ID3D12PipelineState *,
                         Slice<RootSlot>, unsigned x, unsigned y, unsigned z);
void shape_dispatch_recorded(ID3D12GraphicsCommandList *) noexcept;
}
