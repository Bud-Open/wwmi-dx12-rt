#pragma once

#include "hsin_pose.hpp"
#include "rt_shape_final_source.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

namespace hsin_rt {

class CompletedAuthorInput final {
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    using Fence = Microsoft::WRL::ComPtr<ID3D12Fence>;
    using Identity = Microsoft::WRL::ComPtr<IUnknown>;

    WWMI_RtDeformedSource_v1 source_;
    Resource primary_, vectors_;
    Fence fence_;
    Identity device_;
    std::uint64_t primary_address_;

    CompletedAuthorInput(const WWMI_RtDeformedSource_v1 &source, Resource primary,
        Resource vectors, Fence fence, Identity device, std::uint64_t address) noexcept
        : source_(source), primary_(std::move(primary)), vectors_(std::move(vectors)),
          fence_(std::move(fence)), device_(std::move(device)), primary_address_(address) {}

    CompletedAuthorInput(const CompletedAuthorInput &) = delete;
    CompletedAuthorInput &operator=(const CompletedAuthorInput &) = delete;

    static bool valid_recording(WWMI_RtDeformedRecording_v1 recording) noexcept {
        return recording.list && recording.creation && recording.recording;
    }

    static bool valid_selection(const WWMI_RtDeformedSelection_v1 &selection) noexcept {
        const auto &write = selection.native_write;
        return (selection.kind == WWMI_RT_DEFORMED_AUTHOR_FLOAT6 ||
                selection.kind == WWMI_RT_DEFORMED_FINAL_PAIR) &&
            selection.device && selection.plan && selection.native_snapshot &&
            selection.primitive && selection.primitive_creation && selection.load_generation &&
            selection.mod_key && selection.mod_revision && selection.recipe_receipt && !selection.reserved &&
            valid_recording(write.writer) && write.resource && write.creation && write.bytes &&
            write.byte_offset <= UINT64_MAX - write.bytes && write.order && !write.reserved;
    }

    static bool child_on_device(ID3D12DeviceChild *child, IUnknown *device) noexcept {
        Identity identity;
        return child && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&identity))) && identity.Get() == device;
    }

    static std::uint64_t view_address(const WWMI_RtDeformedView_v1 &view, ID3D12Resource *resource,
        IUnknown *device, unsigned vertex_count, unsigned stride, unsigned read_state) noexcept {
        constexpr unsigned read_mask = D3D12_RESOURCE_STATE_GENERIC_READ;
        if (!view.creation || view.stride != stride || view.bytes != std::uint64_t(stride) * vertex_count ||
            view.byte_offset % 4 || !(view.state & read_state) || (view.state & ~read_mask) ||
            !child_on_device(resource, device)) return 0;

        const auto desc = resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || view.byte_offset > desc.Width ||
            view.bytes > desc.Width - view.byte_offset ||
            (read_state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
             (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))) return 0;

        const auto base = resource->GetGPUVirtualAddress();
        if (!base || base > UINT64_MAX - view.byte_offset) return 0;
        const auto address = base + view.byte_offset;
        return address <= UINT64_MAX - view.bytes ? address : 0;
    }

    static bool disjoint_views(const WWMI_RtDeformedSource_v1 &source, ID3D12Resource *primary,
        ID3D12Resource *vectors) noexcept {
        Identity primary_identity, vectors_identity;
        if (FAILED(primary->QueryInterface(IID_PPV_ARGS(&primary_identity))) ||
            FAILED(vectors->QueryInterface(IID_PPV_ARGS(&vectors_identity)))) return false;
        if (primary_identity.Get() != vectors_identity.Get()) return true;

        const auto &p = source.primary;
        const auto &v = source.vectors;
        return p.creation == v.creation &&
            (p.byte_offset + p.bytes <= v.byte_offset || v.byte_offset + v.bytes <= p.byte_offset);
    }

    unsigned mode() const noexcept {
        return source_.selection.kind == WWMI_RT_DEFORMED_AUTHOR_FLOAT6
            ? unsigned(position_author_shape_delta_float6) : unsigned(WWMI_RT_POSITION_FINAL);
    }

public:
    static std::shared_ptr<const CompletedAuthorInput> acquire(WWMI_RtModAcquireDeformedSourceFn_v1 acquire_source,
        ID3D12Device5 *device, const WWMI_RtDeformedSelection_v1 &expected, Recording consumer) {
        const WWMI_RtDeformedRecording_v1 epoch{consumer.list, consumer.creation, consumer.generation};
        if (!acquire_source || !device || !valid_recording(epoch) || !valid_selection(expected)) return {};

        const WWMI_RtDeformedSourceRequest_v1 request{sizeof(request), 1, expected, epoch};
        WWMI_RtDeformedSource_v1 source{};
        if (acquire_source(&request, sizeof(request), &source, sizeof(source)) != WWMI_RT_MOD_OK) return {};

        Resource primary, vectors;
        Fence fence;
        primary.Attach(reinterpret_cast<ID3D12Resource *>(source.primary.resource));
        vectors.Attach(reinterpret_cast<ID3D12Resource *>(source.vectors.resource));
        fence.Attach(reinterpret_cast<ID3D12Fence *>(source.completion_fence));

        if (source.struct_size != sizeof(source) || source.abi_version != 1 ||
            std::memcmp(&source.selection, &expected, sizeof(expected)) || !valid_recording(source.producer) ||
            !source.producer_write_order || !source.completion_queue || !fence || !source.completion_value ||
            source.completion_value == UINT64_MAX || !source.vertex_count ||
            source.first_vertex > UINT32_MAX - source.vertex_count) return {};

        Identity device_identity;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device_identity))) || !device_identity ||
            !child_on_device(fence.Get(), device_identity.Get())) return {};

        // The queue is a historical identifier. The ABI transfers the fence lease.
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < source.completion_value) return {};

        const bool shape = expected.kind == WWMI_RT_DEFORMED_AUTHOR_FLOAT6;
        const auto address = view_address(source.primary, primary.Get(), device_identity.Get(), source.vertex_count,
            shape ? 24u : 12u, shape ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (!address) return {};

        if (shape) {
            const WWMI_RtDeformedView_v1 empty{};
            if (std::memcmp(&source.vectors, &empty, sizeof(empty))) return {};
        } else {
            if (!view_address(source.vectors, vectors.Get(), device_identity.Get(), source.vertex_count,
                    32u, D3D12_RESOURCE_STATE_COPY_SOURCE) ||
                !disjoint_views(source, primary.Get(), vectors.Get())) return {};
        }

        // The trusted publisher must preserve the completed bytes and read state
        // for the lease's lifetime. AddRef preserves the COM objects' lifetime.
        return std::shared_ptr<const CompletedAuthorInput>(new CompletedAuthorInput(source,
            std::move(primary), std::move(vectors), std::move(fence), std::move(device_identity), address));
    }

    const WWMI_RtDeformedSource_v1 &source() const noexcept { return source_; }
    ID3D12Resource *primary() const noexcept { return primary_.Get(); }
    ID3D12Resource *vectors() const noexcept { return vectors_.Get(); }

    bool on_device(ID3D12Device5 *device) const noexcept {
        if (!device) return false;
        Identity identity;
        return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&identity))) && identity.Get() == device_.Get();
    }

    bool covers(unsigned first, unsigned count) const noexcept {
        return count && first >= source_.first_vertex && first - source_.first_vertex <= source_.vertex_count &&
            count <= source_.vertex_count - (first - source_.first_vertex);
    }

    static bool bind(const std::shared_ptr<const CompletedAuthorInput> &input, FinalPosition &position) noexcept {
        if (!input) return false;
        const auto &source = input->source_;
        position = {input->mode(), {source.primary.resource, source.primary.creation}, input->primary_address_,
            source.primary.bytes, source.first_vertex, source.vertex_count,
            std::shared_ptr<void>(input, input->primary_.Get()), input};
        return true;
    }

    bool matches(const FinalPosition &position) const noexcept {
        return position.completed_author.get() == this && position.hold.get() == primary_.Get() &&
            !position.hold.owner_before(position.completed_author) && !position.completed_author.owner_before(position.hold) &&
            position.mode == mode() && position.resource == ResourceKey{source_.primary.resource, source_.primary.creation} &&
            position.address == primary_address_ && position.bytes == source_.primary.bytes &&
            position.first_vertex == source_.first_vertex && position.vertex_count == source_.vertex_count;
    }
};

} // namespace hsin_rt
