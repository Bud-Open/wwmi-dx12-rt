#pragma once
#include "hsin_geometry.hpp"
#include "author_completed_input.hpp"
#include <limits>

namespace hsin_rt {

inline bool author_component_used(const ModPack &pack,unsigned c,unsigned component) noexcept {
    return pack.components[c].index_count && (component==UINT32_MAX || component==c);
}

// An existing REST pose takes no new source path. A borrowed FinalPosition
// must carry the separately acquired, completed immutable author publication.
bool completed_author_inputs_valid(const Pose &pose,unsigned component,ID3D12Device5 *device) {
    if(!pose.pack || (component!=UINT32_MAX && component>=pose.pack->components.size()))return false;
    const auto &pack=*pose.pack;
    if(pose.final_positions.size()!=pack.components.size())return false;
    for(unsigned c=0;c<pack.components.size();++c){
        if(!author_component_used(pack,c,component))continue;
        const auto &position=pose.final_positions[c];
        const bool shape=position.mode==position_author_shape_delta_float6;
        if(!shape && position.mode!=WWMI_RT_POSITION_FINAL)continue;
        const auto &lease=position.completed_author;
        const auto &part=pack.components[c];
        if(!lease || !lease->matches(position) || !lease->on_device(device) ||
           !lease->covers(part.vertex_first,part.vertex_count))return false;
        const auto &s=lease->source();
        if(s.selection.load_generation!=pack.load_generation || s.selection.mod_key!=pack.key ||
           s.selection.mod_revision!=pack.revision || s.selection.native_write.component!=c)return false;
        // The existing raw Shape kernels use 32-bit byte offsets.
        if(shape && position.bytes>UINT32_MAX-(position.address&15ull))return false;
    }
    return true;
}

inline bool completed_author_batch_valid(const GeometryBatch &batch) {
    if(!batch.assets || !batch.assets->device || !batch.assets->pack || !batch.positions)return false;
    const auto &pack=*batch.assets->pack;
    if(batch.final_positions.size()!=pack.components.size() ||
       batch.tangent_ready.size()!=pack.components.size())return false;
    if(batch.single_component!=UINT32_MAX && batch.single_component>=pack.components.size())return false;
    for(unsigned c=0;c<pack.components.size();++c){
        if(!author_component_used(pack,c,batch.single_component))continue;
        const auto &p=batch.final_positions[c];
        const bool shape=p.mode==position_author_shape_delta_float6;
        if(!shape && p.mode!=WWMI_RT_POSITION_FINAL)continue;
        const auto &lease=p.completed_author;
        const auto &part=pack.components[c];
        if(!lease || !lease->matches(p) || !lease->on_device(batch.assets->device.Get()) ||
           !lease->covers(part.vertex_first,part.vertex_count))return false;
        const auto &s=lease->source();
        if(s.selection.load_generation!=pack.load_generation || s.selection.mod_key!=pack.key ||
           s.selection.mod_revision!=pack.revision || s.selection.native_write.component!=c)return false;
        if(shape && (!batch.assets->shape_root || !batch.assets->shape_pipeline ||
           p.bytes>UINT32_MAX-(p.address&15ull) ||
           (batch.tangents && (!batch.assets->shape_tangent_root || !batch.assets->shape_tangent_pipeline))))return false;
    }
    return true;
}

// Called before the ordinary UAV-producing portion of record_native.
// Only destination resources change state. Published sources remain read-only.
inline bool copy_completed_author_pairs(GeometryBatch &batch,ID3D12GraphicsCommandList4 *list) {
    const auto &pack=*batch.assets->pack;
    bool pairs=false;
    for(unsigned c=0;c<pack.components.size();++c)
        if(author_component_used(pack,c,batch.single_component) &&
           batch.final_positions[c].mode==WWMI_RT_POSITION_FINAL && batch.final_positions[c].completed_author)
            pairs=true;
    if(!pairs)return false;
    const auto transition=[&](ID3D12Resource *resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
        list->ResourceBarrier(1,&b);
    };
    transition(batch.positions.Get(),position_read_state,D3D12_RESOURCE_STATE_COPY_DEST);
    if(batch.tangents)transition(batch.tangents.Get(),position_read_state,D3D12_RESOURCE_STATE_COPY_DEST);
    for(unsigned c=0;c<pack.components.size();++c){
        if(!author_component_used(pack,c,batch.single_component))continue;
        const auto &position=batch.final_positions[c];
        if(position.mode!=WWMI_RT_POSITION_FINAL || !position.completed_author)continue;
        const auto &lease=*position.completed_author;
        const auto &s=lease.source();const auto &part=pack.components[c];
        const auto relative=std::uint64_t(part.vertex_first)-s.first_vertex;
        list->CopyBufferRegion(batch.positions.Get(),12ull*part.vertex_first,
            lease.primary(),s.primary.byte_offset+12ull*relative,12ull*part.vertex_count);
        if(batch.tangents){
            list->CopyBufferRegion(batch.tangents.Get(),32ull*part.vertex_first,
                lease.vectors(),s.vectors.byte_offset+32ull*relative,32ull*part.vertex_count);
            batch.tangent_ready[c]=true;
        }
    }
    transition(batch.positions.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if(batch.tangents)transition(batch.tangents.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

} // namespace hsin_rt
