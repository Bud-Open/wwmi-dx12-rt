#define WWMI_RT_DEFORMED_SOURCE_IMPLEMENTATION
#include "rt_shape_source.hpp"
#include "rt_native_shape_link.h"
#include "rt_shape_position_input.h"
#include "rt_mod_source.h"

#include "dx12_bridge.h"
#include "dx12_pipeline.h"
#include "dx12_wrap.h"
#include "port_api_internal.h"
#include "HackerDevice.h"

#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <utility>
#include <vector>

namespace wwmi::deform {
using Microsoft::WRL::ComPtr;

struct PublicationState {
    std::vector<WWMI_RtDeformedSource_v1> records;
    std::vector<ComPtr<IUnknown>> holds;
    std::shared_ptr<const void> rest_asset;
    // This proof identifies the issued recipe/actual controls; not a catalog stamp.
    uint64_t evaluated_recipe = 0, input_write = 0;
    Recording value_writer{};
    std::array<uint64_t, 4> author_input_creations{};
    std::array<uint64_t, 3> native_calls{}, stable_cb_snapshots{};
    WWMI_RtShapePositionRequest_v1 legacy_pose{};
    std::vector<WWMI_RtShapeComponent_v1> legacy_components;
    std::mutex mutex;
    bool live = true;
    ComPtr<ID3D12Fence> fence;
    uint64_t queue = 0, value = 0;
};

namespace {
thread_local PublicationState *current_source = nullptr;
std::atomic<uint64_t> serial{0};
uint64_t next_serial() noexcept { return serial.fetch_add(1, std::memory_order_relaxed) + 1; }

bool valid(Recording r) { return r.list && r.creation && r.recording; }
bool same(Recording a, Recording b) { return a.list == b.list && a.creation == b.creation && a.recording == b.recording; }
bool same(const WWMI_RtDeformedNativeWrite_v1 &a, const WWMI_RtDeformedNativeWrite_v1 &b) {
    return same(a.writer, b.writer) && a.resource == b.resource && a.creation == b.creation &&
        a.byte_offset == b.byte_offset && a.bytes == b.bytes && a.order == b.order && a.component == b.component;
}
bool same_owner(const Selection &a, const Selection &b) {
    return a.device == b.device && a.plan == b.plan && a.native_snapshot == b.native_snapshot &&
        a.primitive == b.primitive && a.primitive_creation == b.primitive_creation &&
        a.primitive_byte_origin == b.primitive_byte_origin && a.load_generation == b.load_generation &&
        a.mod_key == b.mod_key && a.mod_revision == b.mod_revision;
}
bool valid_owner(const Selection &o) {
    const auto &w = o.native_write;
    return o.device && o.plan && o.native_snapshot && o.primitive && o.primitive_creation &&
        o.load_generation && o.mod_key && o.mod_revision && !o.kind && !o.recipe_receipt &&
        !o.reserved && !w.reserved && valid(w.writer) && w.resource && w.creation && w.bytes &&
        w.byte_offset <= UINT64_MAX - w.bytes && w.order;
}
bool same_device(ID3D12DeviceChild *child, ID3D12Device *device) {
    if (!child || !device) return false;
    ComPtr<IUnknown> a, b;
    return SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&a))) &&
        SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
}
ID3D12Resource *resource(uint64_t value) { return reinterpret_cast<ID3D12Resource *>(value); }
ID3D12Device *device(uint64_t value) { return reinterpret_cast<ID3D12Device *>(value); }

bool buffer_range(ID3D12Resource *r, uint64_t offset, uint64_t bytes) {
    if (!r || !bytes) return false;
    const auto d = r->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && offset <= d.Width && bytes <= d.Width - offset;
}

WWMI_RtDeformedSource_v1 make_record(Selection o, uint32_t kind, uint64_t receipt) {
    o.kind = kind;
    o.recipe_receipt = receipt;
    WWMI_RtDeformedSource_v1 record{};
    record.struct_size = sizeof(record);
    record.abi_version = 1;
    record.selection = o;
    return record;
}

bool ready(const PublicationState &s, const WWMI_RtDeformedSource_v1 &v, Recording consumer) {
    if (!s.live) return false;
    if (v.selection.kind == WWMI_RT_DEFORMED_AUTHOR_REST) return true;
    if (!s.fence) return same(consumer, v.producer);
    const auto done = s.fence->GetCompletedValue(); // CPU observation; never a wait.
    return done != UINT64_MAX && done >= s.value;
}
}

const Selection *Publication::selection(uint32_t component) const noexcept {
    if (!data_) return nullptr;
    for (const auto &v : data_->records)
        if (v.selection.native_write.component == component) return &v.selection;
    return nullptr;
}
bool Publication::submitted_once(Recording r, ID3D12CommandQueue *q, ID3D12Fence *f, uint64_t value) const {
    if (!data_ || !q || !f || !value || value == UINT64_MAX || data_->records.empty()) return false;
    const auto &entry = data_->records.front();
    if (!valid(entry.producer) || !same(r, entry.producer) ||
        !same_device(q, device(entry.selection.device)) || !same_device(f, device(entry.selection.device))) return false;
    std::lock_guard lock(data_->mutex);
    if (!data_->live) return false;
    if (data_->value) { data_->live = false; return false; }
    data_->queue = reinterpret_cast<uint64_t>(q);
    data_->fence = f;
    data_->value = value;
    return true;
}
void Publication::invalidate() const {
    if (!data_) return;
    std::lock_guard lock(data_->mutex);
    data_->live = false;
}
ScopedSourceRead::ScopedSourceRead(const Publication &p) noexcept : hold_(p.data_), outer_(current_source) { current_source = hold_.get(); }
ScopedSourceRead::~ScopedSourceRead() { current_source = outer_; }

Publication publish_author_rest(Selection owner, RestSelection rest) {
    if (!valid_owner(owner) || !rest.completed_vm_recipe || !rest.immutable_position_asset || !rest.position_asset) return {};
    auto data = std::make_shared<PublicationState>();
    data->evaluated_recipe = rest.completed_vm_recipe;
    data->input_write = rest.immutable_position_asset;
    data->rest_asset = std::move(rest.position_asset);
    data->records.push_back(make_record(owner, WWMI_RT_DEFORMED_AUTHOR_REST, next_serial()));
    return Publication(std::move(data));
}

Publication publish_final_pair(Selection owner, const FinalPair &p) {
    const auto &x = p.position; const auto &t = p.vectors;
    if (!valid_owner(owner) || !valid(x.producer) || !same(x.producer, t.producer) ||
        !p.one_shot || x.producer.list != reinterpret_cast<uint64_t>(p.one_shot) ||
        p.one_shot->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !same_device(p.one_shot, device(owner.device)) ||
        !x.deformation_stamp || x.deformation_stamp != t.deformation_stamp || !x.write_order || !t.write_order ||
        x.first_vertex != t.first_vertex || !x.vertex_count || x.vertex_count != t.vertex_count ||
        x.first_vertex > UINT32_MAX - x.vertex_count) return {};
    const auto view_ok = [&](const View &v, unsigned stride) {
        constexpr unsigned reads = D3D12_RESOURCE_STATE_COPY_SOURCE;
        return v.resource && v.creation && v.stride == stride && v.bytes == uint64_t(stride) * x.vertex_count &&
            v.byte_offset % 4 == 0 && (v.state & reads) == reads && (v.state & ~unsigned(D3D12_RESOURCE_STATE_GENERIC_READ)) == 0 &&
            same_device(resource(v.resource), device(owner.device)) && buffer_range(resource(v.resource), v.byte_offset, v.bytes);
    };
    if (!view_ok(x.view, 12) || !view_ok(t.view, 32)) return {};
    if (x.view.resource == t.view.resource && (x.view.creation != t.view.creation ||
        (x.view.byte_offset < t.view.byte_offset + t.view.bytes && t.view.byte_offset < x.view.byte_offset + x.view.bytes))) return {};
    auto data = std::make_shared<PublicationState>();
    data->evaluated_recipe = x.deformation_stamp;
    auto v = make_record(owner, WWMI_RT_DEFORMED_FINAL_PAIR, next_serial());
    v.first_vertex = x.first_vertex; v.vertex_count = x.vertex_count;
    v.producer = x.producer; v.producer_write_order = (std::max)(x.write_order, t.write_order);
    v.primary = x.view; v.vectors = t.view;
    data->holds.emplace_back(resource(x.view.resource));
    data->holds.emplace_back(resource(t.view.resource));
    data->records.push_back(v);
    return Publication(std::move(data));
}

namespace {
bool typed_input(const ShapeTypedInput &in, ID3D12Device *d, DXGI_FORMAT format,
                 unsigned element, uint64_t needed, bool uav = false) {
    if (in.format != format || !in.creation || !in.elements || needed > in.elements ||
        in.first_element > UINT64_MAX / element || !same_device(in.resource, d)) return false;
    const auto flags = in.resource->GetDesc().Flags;
    if (uav ? (!(flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
               (in.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS && in.state != D3D12_RESOURCE_STATE_COMMON)) :
              ((flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
               (in.state != D3D12_RESOURCE_STATE_COMMON &&
                (!(in.state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) || (in.state & ~D3D12_RESOURCE_STATE_GENERIC_READ))))) return false;
    return buffer_range(in.resource, element * in.first_element, uint64_t(element) * in.elements);
}

bool same_view(const WWMI_RtNativeShapeView_v1 &a, const WWMI_RtNativeShapeView_v1 &b) {
    return a.resource && a.creation && a.bytes && a.resource == b.resource && a.creation == b.creation &&
        a.byte_offset == b.byte_offset && a.bytes == b.bytes && a.format == b.format;
}
bool freeze_matches(const ShapeFrozenCb &copy, const WWMI_RtNativeShapeWrite_v1 &w, Recording recording) {
    constexpr auto proof = WWMI_RT_SHAPE_EVIDENCE_NATIVE_CALL_RETURNED | WWMI_RT_SHAPE_EVIDENCE_CB_CPU_OBSERVED;
    return copy.snapshot && copy.native_call && copy.native_call == w.call_id && w.order && w.cb0.resource &&
        w.cb0.creation && w.cb0.bytes == sizeof(copy.words) && (w.evidence & proof) == proof &&
        same(recording, {w.list, w.list_creation, w.recording}) &&
        std::memcmp(copy.words, w.cb_words, sizeof(copy.words)) == 0;
}

bool exact_native(const ShapeJob &j) {
    if (!j.recipe || !j.owners.data() || j.owners.empty() || !j.links.data() ||
        j.links.size() != j.owners.size() || !j.components.data() || j.components.empty()) return false;
    const auto *n = j.legacy_pose;
    const auto &r = *j.recipe;
    if (n && (n->struct_size != sizeof(*n) || n->abi_version != 1 || n->reserved ||
        n->component_count != j.components.size() || j.components.size() != j.owners.size() ||
        !n->device || !same_device(j.one_shot, device(n->device)) || !n->native_blas_resource ||
        !n->native_blas_creation || !n->native_blas_address ||
        !valid({n->native_list, n->native_list_creation, n->native_recording}))) return false;
    for (size_t i = 0; i < j.owners.size(); ++i) {
        const auto &owner = j.owners[i];
        if (!valid_owner(owner) || !same_owner(owner, j.owners.front()) ||
            (n && (owner.load_generation != n->load_generation || owner.mod_key != n->mod_key || owner.mod_revision != n->mod_revision)) ||
            std::any_of(j.owners.begin(), j.owners.begin() + i, [&](const auto &other) {
                return other.native_write.component == owner.native_write.component;
            })) return false;
        const auto &w = owner.native_write;
        const auto c = std::find_if(j.components.begin(), j.components.end(), [&](const auto &c) { return c.component == w.component; });
        if (c == j.components.end() || c->reserved || c->reserved2 ||
            c->output_resource != w.resource || c->output_creation != w.creation ||
            c->output_offset != w.byte_offset || c->output_bytes != w.bytes || c->skin_order != w.order) return false;
        const auto &link = j.links[i];
        const auto group_ok = [](const WWMI_RtNativeShapeWrite_v1 &write, unsigned evaluated_y, unsigned count) {
            return write.group_x == 1 && write.group_z == 1 && write.group_y &&
                evaluated_y == (std::max)(write.group_y, count / 32u + 1u);
        };
        constexpr auto read_proof = WWMI_RT_SHAPE_EVIDENCE_DECLARED_UAV_CONTAINS_READ | WWMI_RT_SHAPE_EVIDENCE_LOCAL_COMMAND_ORDER;
        if (link.struct_size != sizeof(link) || link.abi_version != 1 || link.status != WWMI_RT_SHAPE_LINK_LOCAL_RECORDED ||
            link.loader_count != r.batches.size() || link.skin_order != w.order ||
            (link.multiplier.evidence & read_proof) != read_proof ||
            !same(w.writer, {link.read_list, link.read_list_creation, link.read_recording}) ||
            !c->native_shape_resource || !c->native_shape_creation || !c->native_shape_bytes ||
            link.native_t3.resource != c->native_shape_resource || link.native_t3.creation != c->native_shape_creation ||
            link.native_t3.byte_offset != c->native_shape_view_offset || link.native_t3.bytes != c->native_shape_view_bytes ||
            link.read_byte_offset != c->native_shape_offset || link.read_bytes != c->native_shape_bytes ||
            link.multiplier.u0.resource != link.native_t3.resource || link.multiplier.u0.creation != link.native_t3.creation ||
            link.read_byte_offset < link.multiplier.u0.byte_offset ||
            link.read_byte_offset - link.multiplier.u0.byte_offset > link.multiplier.u0.bytes ||
            link.read_bytes > link.multiplier.u0.bytes - (link.read_byte_offset - link.multiplier.u0.byte_offset) ||
            !freeze_matches(r.multiplier_cb, link.multiplier, w.writer) ||
            !group_ok(link.multiplier, r.multiplier_groups_y, r.author_vertices) ||
            link.multiplier.order >= link.read_order) return false;
        uint64_t order = 0;
        unsigned seen = 0;
        for (size_t b = 0; b < r.batches.size(); ++b) {
            const auto &loader = link.loaders[b];
            const auto row = std::find_if(r.batches.begin(), r.batches.end(), [&](const auto &candidate) {
                return candidate.loader_cb.native_call == loader.call_id;
            });
            if (row == r.batches.end()) return false;
            const unsigned bit = 1u << (row - r.batches.begin());
            if ((seen & bit) || !freeze_matches(row->loader_cb, loader, w.writer) || !same_view(loader.u0, link.multiplier.u0) ||
                !same_view(loader.u1, link.multiplier.u1) || !group_ok(loader, row->loader_groups_y, row->author_entries) ||
                loader.order <= order || loader.order >= link.multiplier.order) return false;
            const auto *words = row->loader_cb.words;
            if (uint32_t(words[0] + words[1] + words[2] + words[3]) != row->checksum || words[261] != row->original_offset) return false;
            seen |= bit;
            order = loader.order;
        }
    }
    return true;
}

ComPtr<ID3D12Resource> make_resource(ID3D12Device *d, const D3D12_RESOURCE_DESC &desc, D3D12_HEAP_TYPE type,
                                    D3D12_RESOURCE_STATES state) {
    ComPtr<ID3D12Resource> value;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type; heap.CreationNodeMask = 1; heap.VisibleNodeMask = 1;
    if (FAILED(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&value)))) return {};
    return value;
}
ComPtr<ID3D12Resource> make_buffer(ID3D12Device *d, uint64_t bytes, D3D12_HEAP_TYPE type,
                                  D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
    return make_resource(d, desc, type, state);
}
ComPtr<ID3D12Resource> upload(ID3D12Device *d, unsigned allocation, const void *data, unsigned bytes) {
    auto value = make_buffer(d, allocation, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr; D3D12_RANGE none{0, 0};
    if (!value || FAILED(value->Map(0, &none, &mapped))) return {};
    std::memcpy(mapped, data, bytes);
    if (bytes < allocation) std::memset(static_cast<unsigned char *>(mapped) + bytes, 0, allocation - bytes);
    D3D12_RANGE written{0, allocation};
    value->Unmap(0, &written);
    return value;
}
void transition(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    l->ResourceBarrier(1, &b);
}
void uav_barrier(ID3D12GraphicsCommandList *l, std::initializer_list<ID3D12Resource *> resources) {
    D3D12_RESOURCE_BARRIER b[3]{}; unsigned n = 0;
    for (auto *r : resources) { b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b[n++].UAV.pResource = r; }
    l->ResourceBarrier(n, b);
}

enum class Pass { control, load, multiply };
struct ShapeWork {
    const ShapeJob &job;
    PublicationState &output;
    ComPtr<ID3D12DescriptorHeap> visible, cpu;
    ComPtr<ID3D12Resource> u0, u1, control, params;
    D3D12_GPU_VIRTUAL_ADDRESS cb_base = 0, cb_bound = 0;
    static constexpr unsigned cb_stride = 1280; // 1056-byte CB, rounded to 256
    uint32_t u0_elements = 0, u1_elements = 0, step = 0, next_slot = 2;
    uint64_t u0_creation = 0, last_write = 0;
    Pass pass = Pass::control;
    unsigned expected_y = 0;
    bool bound = false, issued = false;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(ID3D12DescriptorHeap *h, unsigned index) const {
        auto v = h->GetCPUDescriptorHandleForHeapStart();
        v.ptr += SIZE_T(job.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) * index;
        return v;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(unsigned index) const {
        auto v = visible->GetGPUDescriptorHandleForHeapStart();
        v.ptr += UINT64(job.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) * index;
        return v;
    }
    void make_uav(ID3D12Resource *r, DXGI_FORMAT format, uint64_t first, uint32_t count, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = format; v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; v.Buffer.FirstElement = first; v.Buffer.NumElements = count;
        job.device->CreateUnorderedAccessView(r, nullptr, &v, h);
    }
    void make_srv(const ShapeTypedInput &r, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = r.format; v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Buffer.FirstElement = r.first_element; v.Buffer.NumElements = r.elements;
        job.device->CreateShaderResourceView(r.resource, &v, h);
    }
    bool init(uint32_t padded, uint32_t entries) {
        constexpr auto flag = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        u0_elements = 6u * padded;
        u1_elements = (std::max)(padded, entries);
        const auto storage = [&](uint64_t bytes) {
            return make_buffer(job.device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, flag);
        };
        u0 = storage(4ull * u0_elements); u1 = storage(4ull * u1_elements); control = storage(66 * 16);
        if (!u0 || !u1 || !control) return false;
        u0_creation = next_serial(); // This distinct, real successful allocation is never reused for another write.
        D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(job.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&visible)))) return false;
        desc.NumDescriptors = 2; desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(job.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&cpu)))) return false;

        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D; texture.Width = 2; texture.Height = 1;
        texture.DepthOrArraySize = 1; texture.MipLevels = 1; texture.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        texture.SampleDesc.Count = 1; texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        params = make_resource(job.device, texture, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!params) return false;
        std::array<float, 8> values{};
        for (size_t i = 0; i < job.recipe->batches.size(); ++i) {
            const auto &b = job.recipe->batches[i];
            values[4*i] = float(b.checksum); values[4*i+1] = float(b.original_offset); values[4*i+2] = float(b.author_offset);
        }
        // Distinct CB windows for every actual batch, followed by the two
        // IniParams texels in one private immutable upload for the entire prefix.
        constexpr unsigned pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        constexpr unsigned align = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        constexpr auto aligned = [](unsigned size) { return (size + align - 1) & ~(align - 1); };
        std::array<unsigned char, aligned(3 * cb_stride) + pitch> bytes{};
        const unsigned n = unsigned(job.recipe->batches.size());
        for (unsigned i = 0; i < n; ++i) {
            const auto &cb = job.recipe->batches[i].loader_cb;
            std::memcpy(bytes.data() + i * cb_stride, cb.words, sizeof(cb.words));
        }
        const auto &last = job.recipe->multiplier_cb;
        std::memcpy(bytes.data() + n * cb_stride, last.words, sizeof(last.words));
        const unsigned params_offset = aligned((n + 1) * cb_stride);
        std::memcpy(bytes.data() + params_offset, values.data(), sizeof(values));
        auto staging = upload(job.device, params_offset + pitch, bytes.data(), params_offset + pitch);
        if (!staging || !(cb_base = staging->GetGPUVirtualAddress())) return false;
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = staging.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = params_offset;
        src.PlacedFootprint.Footprint = {texture.Format, 2, 1, 1, pitch};
        dst.pResource = params.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        auto *list = job.one_shot;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(list, params.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        output.holds.emplace_back(staging.Get());
        for (IUnknown *p : std::array<IUnknown *, 6>{u0.Get(), u1.Get(), control.Get(), params.Get(), visible.Get(), cpu.Get()})
            output.holds.emplace_back(p);

        ID3D12DescriptorHeap *heaps[]{visible.Get()};
        list->SetDescriptorHeaps(1, heaps);
        const UINT zero[4]{};
        for (unsigned slot = 0; slot < 2; ++slot) {
            auto *r = slot ? u1.Get() : u0.Get();
            const auto format = slot ? DXGI_FORMAT_R32_SINT : DXGI_FORMAT_R32_UINT;
            const auto n = slot ? u1_elements : u0_elements;
            make_uav(r, format, 0, n, cpu_slot(cpu.Get(), slot));
            make_uav(r, format, 0, n, cpu_slot(visible.Get(), slot));
            list->ClearUnorderedAccessViewUint(gpu_slot(slot), cpu_slot(cpu.Get(), slot), r, zero, 0, nullptr);
        }
        uav_barrier(list, {u0.Get(), u1.Get()});
        return true;
    }
    void set_cb(unsigned i) { cb_bound = cb_base + i * cb_stride; }
    bool bind(ID3D12RootSignature *root, ID3D12PipelineState *pso, Slice<RootSlot> slots, unsigned x, unsigned y, unsigned z) {
        if (bound || issued || !root || !pso || x != 1 || z != 1 || y != expected_y || next_slot + slots.size() > 32) return false;
        const auto expected = [&](const RootSlot &s) -> unsigned {
            if (s.space) return 0;
            if (s.kind == 'b' && (!cb_bound || s.type != D3D_SIT_CBUFFER)) return 0;
            if (s.kind == 'u' && (s.type != D3D_SIT_UAV_RWTYPED || s.dimension != D3D_SRV_DIMENSION_BUFFER)) return 0;
            if (s.kind == 't' && (s.type != D3D_SIT_TEXTURE ||
                s.dimension != (s.reg == 120 ? D3D_SRV_DIMENSION_TEXTURE1D : D3D_SRV_DIMENSION_BUFFER))) return 0;
            if (pass == Pass::control) {
                if (s.kind == 'b' && s.reg == 0) return 1;
                if (s.kind == 't' && s.reg == 33) return 2;
                if (s.kind == 't' && s.reg == 120) return 4;
                if (s.kind == 'u' && s.reg == 5) return 8;
                if (s.kind == 'u' && s.reg == 6) return 16;
            } else if (pass == Pass::load) {
                if (s.kind == 't' && s.reg == 0) return 1;
                if (s.kind == 't' && s.reg == 1) return 2;
                if (s.kind == 'u' && s.reg == 0) return 4;
                if (s.kind == 'u' && s.reg == 1) return 8;
                if (s.kind == 'u' && s.reg == 6) return 16;
                if (s.kind == 't' && s.reg == 120) return 32; // may survive reflection
            } else {
                if (s.kind == 'b' && s.reg == 0) return 1;
                if (s.kind == 'u' && s.reg == 0) return 2;
                if (s.kind == 'u' && s.reg == 1) return 4;
                if (s.kind == 't' && s.reg == 120) return 8; // may survive reflection
            }
            return 0;
        };
        unsigned seen = 0, params_seen = 0;
        for (const auto &s : slots) {
            const auto bit = expected(s);
            if (!bit || (seen & bit) || s.param >= slots.size() || (params_seen & (1u << s.param))) return false;
            seen |= bit; params_seen |= 1u << s.param;
        }
        const unsigned required = pass == Pass::multiply ? 7u : 31u;
        if ((seen & required) != required) return false;
        auto *list = job.one_shot;
        list->SetComputeRootSignature(root);
        list->SetPipelineState(pso);
        const auto &r = *job.recipe;
        for (const auto &s : slots) {
            const auto index = next_slot++;
            const auto dst = cpu_slot(visible.Get(), index);
            if (s.kind == 'b') {
                D3D12_CONSTANT_BUFFER_VIEW_DESC desc{cb_bound, cb_stride};
                job.device->CreateConstantBufferView(&desc, dst);
            } else if (s.kind == 't' && s.reg == 120) {
                D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; desc.Texture1D.MipLevels = 1;
                job.device->CreateShaderResourceView(params.Get(), &desc, dst);
            } else if (s.kind == 't') {
                make_srv(s.reg == 33 ? r.offsets : s.reg == 0 ? r.vertex_ids : r.vertex_deltas, dst);
            } else if (s.reg == 0) make_uav(u0.Get(), DXGI_FORMAT_R32_UINT, 0, u0_elements, dst);
            else if (s.reg == 1) make_uav(u1.Get(), DXGI_FORMAT_R32_SINT, 0, u1_elements, dst);
            else if (s.reg == 5) make_uav(r.values.resource, r.values.format, r.values.first_element, r.values.elements, dst);
            else make_uav(control.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 0, 66, dst);
            list->SetComputeRootDescriptorTable(s.param, gpu_slot(index));
        }
        output.holds.emplace_back(root);
        output.holds.emplace_back(pso);
        bound = true;
        return true;
    }
};

thread_local ShapeWork *active_work = nullptr;
struct WorkScope {
    ShapeWork *previous;
    ID3D11DeviceChild *previous_shader;
    explicit WorkScope(ShapeWork &work) : previous(active_work), previous_shader(MigotoPort::State().shader[5]) {
        MigotoPort::Dx12Bridge bridge{};
        bridge.device = work.job.device;
        bridge.cl = work.job.one_shot;
        bridge.own_list = true;
        MigotoPort::BridgePush(bridge);
        active_work = &work;
    }
    ~WorkScope() {
        active_work = previous;
        MigotoPort::State().shader[5] = previous_shader;
        MigotoPort::BridgePop();
    }
};

ComPtr<ID3D11ComputeShader> vm_shader(const wchar_t *key, Pass pass, size_t batches) {
    if (!key) return {};
    const wchar_t *expected = pass == Pass::load ? L"customshader\\wwmiv1\\shapekeyloader" :
        pass == Pass::multiply ? L"customshader\\wwmiv1\\shapekeymultiplier" :
        batches == 1 ? L"customshader\\wwmiv1\\shapekeyoverrider" : L"customshader\\wwmiv1\\shapekeybatchoverrider";
    if (std::wcscmp(key, expected) != 0) return {};
    const auto i = customShaders.find(key); // actual parser-normalized namespaced key.
    if (i == customShaders.end() || !i->second.cs_override || i->second.max_executions_per_frame) return {};
    auto &s = i->second;
    if (!s.substantiated) {
        auto *device = PortApi::g_dev.GetPassThroughOrigDevice1();
        if (!device || !s.cs_bytecode) return {};
        s.substantiate(device);
    }
    if (!s.cs || !MigotoPort::AsShaderCode(s.cs)) return {};
    return ComPtr<ID3D11ComputeShader>(s.cs);
}
}

bool shape_dispatch_active(ID3D12GraphicsCommandList *list) noexcept { return active_work && active_work->job.one_shot == list; }
bool bind_shape_dispatch(ID3D12GraphicsCommandList *list, ID3D12RootSignature *root, ID3D12PipelineState *pso,
                         Slice<RootSlot> slots, unsigned x, unsigned y, unsigned z) {
    return shape_dispatch_active(list) && active_work->bind(root, pso, slots, x, y, z);
}
void shape_dispatch_recorded(ID3D12GraphicsCommandList *list) noexcept {
    if (!shape_dispatch_active(list) || !active_work->bound || active_work->issued) return;
    active_work->issued = true;
    ++active_work->step;
    active_work->last_write = next_serial();
}

Publication record_author_shape(const ShapeJob &j) {
    try {
        if (active_work || !j.device || !j.one_shot || j.one_shot->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            !valid(j.producer) || j.producer.list != reinterpret_cast<uint64_t>(j.one_shot) || !same_device(j.one_shot, j.device) ||
            !j.recipe || !j.owners.data() || j.owners.empty()) return {};
        const auto &r = *j.recipe;
        if (!r.evaluated_recipe || !valid(r.values_write.producer) || !r.values_write.order ||
            !r.author_vertices || !r.multiplier_groups_y ||
            r.multiplier_groups_y > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
            (!r.batches.data() || (r.batches.size() != 1 && r.batches.size() != 2))) return {};
        const bool values_local = same(r.values_write.producer, j.producer);
        if (values_local) {
            if (r.values_write.fence || r.values_write.fence_value ||
                r.values.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return {};
        } else {
            if (!r.values_write.fence || !r.values_write.fence_value || !same_device(r.values_write.fence, j.device)) return {};
            const auto done = r.values_write.fence->GetCompletedValue();
            if (done == UINT64_MAX || done < r.values_write.fence_value) return {};
        }
        const unsigned padded = 32u * r.multiplier_groups_y;
        if (padded < r.author_vertices) return {};
        if (r.batches.size() == 1 && (r.batches[0].author_offset || r.batches[0].original_offset)) return {};
        uint32_t entry_end = 0;
        for (const auto &b : r.batches) {
            // IniParams is float4; the existing Core uses numeric float casts
            // for these three fields. Reject a value the real shader can't read back.
            if (!b.checksum || b.checksum > 16777216u || b.original_offset > 16777216u ||
                b.author_offset > 16777216u || !b.author_entries ||
                b.author_entries > UINT32_MAX - b.author_offset || !b.loader_groups_y ||
                b.loader_groups_y > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
                uint64_t(32) * b.loader_groups_y < b.author_entries) return {};
            entry_end = (std::max)(entry_end, b.author_offset + b.author_entries);
        }
        if (r.batches.size() == 2) {
            const auto &a = r.batches[0]; const auto &b = r.batches[1];
            if (a.checksum == b.checksum || a.loader_cb.native_call == b.loader_cb.native_call ||
                (a.author_offset < b.author_offset + b.author_entries && b.author_offset < a.author_offset + a.author_entries)) return {};
        }
        if (!typed_input(r.offsets, j.device, DXGI_FORMAT_R32G32B32A32_UINT, 16, 32 * r.batches.size()) ||
            !typed_input(r.vertex_ids, j.device, DXGI_FORMAT_R32_UINT, 4, entry_end) ||
            !typed_input(r.vertex_deltas, j.device, DXGI_FORMAT_R16_FLOAT, 2, 6ull * entry_end) ||
            !typed_input(r.values, j.device, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, 32 * r.batches.size(), true) ||
            !exact_native(j)) return {};
        for (const auto &o : j.owners) if (o.device != reinterpret_cast<uint64_t>(j.device)) return {};
        PortApi::EngineScope engine;
        if (PortApi::LoadGeneration() != j.owners.front().load_generation) return {};
        const auto control = vm_shader(r.overrider, Pass::control, r.batches.size());
        const auto load = vm_shader(r.loader, Pass::load, r.batches.size());
        const auto multiply = vm_shader(r.multiplier, Pass::multiply, r.batches.size());
        if (!control || !load || !multiply || control.Get() == load.Get() ||
            load.Get() == multiply.Get() || control.Get() == multiply.Get()) return {};

        auto output = std::make_shared<PublicationState>();
        output->evaluated_recipe = r.evaluated_recipe; output->input_write = r.values_write.order;
        output->value_writer = r.values_write.producer;
        output->author_input_creations = {r.offsets.creation, r.vertex_ids.creation, r.vertex_deltas.creation, r.values.creation};
        for (size_t i = 0; i < r.batches.size(); ++i) {
            output->native_calls[i] = r.batches[i].loader_cb.native_call;
            output->stable_cb_snapshots[i] = r.batches[i].loader_cb.snapshot;
        }
        output->native_calls[2] = r.multiplier_cb.native_call;
        output->stable_cb_snapshots[2] = r.multiplier_cb.snapshot;
        if (j.legacy_pose) {
            output->legacy_pose = *j.legacy_pose;
            output->legacy_components.assign(j.components.begin(), j.components.end());
        }
        for (auto *p : {r.offsets.resource, r.vertex_ids.resource, r.vertex_deltas.resource, r.values.resource})
            output->holds.emplace_back(p);
        ShapeWork work{j, *output};
        WorkScope scope(work);
        if (!work.init(padded, entry_end)) return {};
        if (values_local) uav_barrier(j.one_shot, {r.values.resource});
        const auto issue = [&](Pass pass, ID3D11ComputeShader *shader, unsigned y) {
            work.pass = pass; work.expected_y = y; work.bound = work.issued = false;
            MigotoPort::State().shader[5] = shader;
            return MigotoPort::IssueDispatch(1, y, 1) && work.issued;
        };
        for (size_t i = 0; i < r.batches.size(); ++i) {
            const auto call = j.links.front().loaders[i].call_id;
            const auto *b = std::find_if(r.batches.begin(), r.batches.end(), [&](const auto &r) { return r.loader_cb.native_call == call; });
            work.set_cb(unsigned(b - r.batches.begin()));
            if (!issue(Pass::control, control.Get(), 1)) return {};
            uav_barrier(j.one_shot, {work.control.Get()});
            if (!issue(Pass::load, load.Get(), b->loader_groups_y)) return {};
            uav_barrier(j.one_shot, {work.u0.Get(), work.u1.Get(), work.control.Get()});
        }
        work.set_cb(unsigned(r.batches.size()));
        if (!issue(Pass::multiply, multiply.Get(), r.multiplier_groups_y) ||
            work.step != 2 * r.batches.size() + 1 || !work.last_write) return {};
        transition(j.one_shot, work.u0.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto receipt = next_serial();
        for (const auto &owner : j.owners) {
            auto v = make_record(owner, WWMI_RT_DEFORMED_AUTHOR_FLOAT6, receipt);
            v.vertex_count = r.author_vertices; v.producer = j.producer; v.producer_write_order = work.last_write;
            v.primary = {reinterpret_cast<uint64_t>(work.u0.Get()), work.u0_creation, 0, 24ull * r.author_vertices, 24,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            output->records.push_back(v);
        }
        return Publication(std::move(output));
    } catch (...) {
        return {}; // The caller drops this private recording; partial work is not submitted.
    }
}
} // namespace wwmi::deform

extern "C" uint32_t WWMI_RtModAcquireDeformedSource_v1(const WWMI_RtDeformedSourceRequest_v1 *q, uint32_t size,
    WWMI_RtDeformedSource_v1 *out, uint32_t out_size) {
    using namespace wwmi::deform;
    if (!out || out_size != sizeof(*out)) return WWMI_RT_MOD_BAD_INPUT;
    *out = {};
    if (!q || size != sizeof(*q) || q->struct_size != sizeof(*q) || q->abi_version != 1 || !valid(q->consumer) ||
        q->selection.reserved || q->selection.native_write.reserved) return WWMI_RT_MOD_BAD_INPUT;
    auto *s = current_source;
    if (!s || !q->selection.kind || !q->selection.recipe_receipt) return WWMI_RT_MOD_NOT_READY;
    std::lock_guard lock(s->mutex);
    for (const auto &v : s->records) {
        const auto &a = q->selection; const auto &b = v.selection;
        if (!same_owner(a,b) || !same(a.native_write,b.native_write) || a.kind != b.kind || a.recipe_receipt != b.recipe_receipt) continue;
        if (!ready(*s, v, q->consumer)) return WWMI_RT_MOD_NOT_READY;
        *out = v;
        if (out->primary.resource) resource(out->primary.resource)->AddRef();
        if (out->vectors.resource) resource(out->vectors.resource)->AddRef();
        if (s->fence) {
            s->fence->AddRef();
            out->completion_fence = reinterpret_cast<uint64_t>(s->fence.Get());
            out->completion_queue = s->queue; out->completion_value = s->value;
        }
        return WWMI_RT_MOD_OK;
    }
    return WWMI_RT_MOD_STALE;
}

extern "C" uint32_t WWMI_RtModAcquirePostMultiplierShape_v1(
    const WWMI_RtShapePositionRequest_v1 *q, const WWMI_RtShapeComponent_v1 *components,
    uint32_t count, WWMI_RtPostMultiplierShape_v1 *out, uint32_t size) {
    using namespace wwmi::deform;
    if (!out || size != sizeof(*out)) return WWMI_RT_MOD_BAD_INPUT;
    *out = {};
    if (!q || q->struct_size != sizeof(*q) || q->abi_version != 1 || q->reserved || !components ||
        !count || q->component_count != count || !valid({q->consumer_list,q->consumer_creation,q->consumer_recording})) return WWMI_RT_MOD_BAD_INPUT;
    auto *s = current_source;
    if (!s || s->records.empty()) return WWMI_RT_MOD_NOT_READY;
    std::lock_guard lock(s->mutex);
    const auto &v = s->records.front();
    if (v.selection.kind != WWMI_RT_DEFORMED_AUTHOR_FLOAT6 || !ready(*s, v, {q->consumer_list,q->consumer_creation,q->consumer_recording})) return WWMI_RT_MOD_NOT_READY;
    if (!s->legacy_pose.struct_size) return WWMI_RT_MOD_NOT_READY;
    auto exact = s->legacy_pose;
    exact.consumer_list = q->consumer_list; exact.consumer_creation = q->consumer_creation; exact.consumer_recording = q->consumer_recording;
    if (std::memcmp(&exact, q, sizeof(exact)) || s->legacy_components.size() != count ||
        std::memcmp(s->legacy_components.data(), components, size_t(count) * sizeof(*components))) return WWMI_RT_MOD_STALE;
    resource(v.primary.resource)->AddRef();
    *out = {sizeof(*out), 1, v.first_vertex, v.vertex_count, v.primary.resource, v.primary.creation, v.primary.byte_offset, v.primary.bytes,
            v.primary.state, WWMI_RT_SHAPE_AUTHOR_POSITION_NORMAL_FLOAT6, v.producer.list, v.producer.creation, v.producer.recording, 0, 0};
    if (s->fence) {
        s->fence->AddRef();
        out->completion_fence = reinterpret_cast<uint64_t>(s->fence.Get()); out->completion_value = s->value;
    }
    return WWMI_RT_MOD_OK;
}
