#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>
#include <unordered_map>

namespace wwmi::observe::skin_input {
enum class Kind : std::uint32_t { none, skin4, skin8, skin8_extra };
inline std::uint32_t cb_bytes(Kind k) { return k==Kind::none?0:k==Kind::skin8_extra?32:28; }
struct Ports { int srv=-1, uav=-1, cb=-1; };
struct Binding {
    std::uint64_t pipeline=0, generation=0, layout=0;
    Kind kind=Kind::none;
    Ports ports;
    std::uint64_t srv_table=0, uav_table=0, cb_resource=0, cb_offset=0, cb_size=0;
    bool cb_known=false;
};
// Caller holds the observer input mutex. Every PSO bind starts fresh root evidence.
struct State {
    std::unordered_map<std::uint64_t,Ports> layouts;
    std::unordered_map<std::uint64_t,Binding> lists;
    void bind(std::uint64_t list, std::uint64_t pipeline, std::uint64_t generation,
              std::uint64_t layout, Kind kind) {
        lists.erase(list);
        if(kind==Kind::none) return;
        Binding b; b.pipeline=pipeline;b.generation=generation;b.layout=layout;b.kind=kind;
        if(auto p=layouts.find(layout);p!=layouts.end()) b.ports=p->second;
        lists.emplace(list,b);
    }
    void table(std::uint64_t list,std::uint64_t layout,std::uint32_t param,std::uint64_t value) {
        auto p=lists.find(list);if(p==lists.end() || p->second.layout!=layout)return;
        if(p->second.ports.srv==static_cast<int>(param))p->second.srv_table=value;
        if(p->second.ports.uav==static_cast<int>(param))p->second.uav_table=value;
    }
    void cb(std::uint64_t list,std::uint64_t layout,std::uint32_t param,
            std::uint64_t resource,std::uint64_t offset,std::uint64_t size,bool known) {
        auto p=lists.find(list);if(p==lists.end() || p->second.layout!=layout ||
            p->second.ports.cb!=static_cast<int>(param))return;
        auto &b=p->second;b.cb_resource=resource;b.cb_offset=offset;b.cb_size=size;b.cb_known=known && resource;
    }
    void reset(std::uint64_t list) {lists.erase(list);}
};
inline bool cb_range(std::uint64_t width,std::uint64_t offset,std::uint64_t view_size,std::uint32_t bytes) {
    return bytes && offset<=width && bytes<=width-offset && bytes<=view_size;
}
struct Span { bool known=false;std::uint64_t offset=0,bytes=0; };
// Bone t0 is R32G32B32A32_FLOAT. The known framework has at most 512 x 3 rows.
inline Span palette_span(std::uint64_t first_element,std::uint64_t elements,
                         std::uint64_t resource_bytes) {
    if (!elements || elements>1536 || elements%3 || first_element>resource_bytes/16 ||
        elements>resource_bytes/16-first_element) return {};
    return {true,16*first_element,16*elements};
}
inline constexpr std::size_t kPaletteChunkBytes=4096;
// Typed R32_FLOAT view offsets/counts are elements, not bytes. The shader uses
// i32 indices; do not present a wrapped shader write as a linear 64-bit range.
inline Span position_span(std::uint64_t first_element,std::uint64_t elements,
    std::uint64_t resource_bytes,std::uint32_t out_vertex,std::uint32_t vertices) {
    const auto max=std::numeric_limits<std::uint32_t>::max();
    if(out_vertex>max/3 || vertices>max/3-out_vertex)return {};
    const std::uint64_t first=3ull*out_vertex,count=3ull*vertices;
    if(first>elements || count>elements-first || first_element>resource_bytes/4)return {};
    const auto remaining=resource_bytes/4-first_element;
    if(first>remaining || count>remaining-first)return {};
    return {true,4*(first_element+first),4*count};
}
inline Span position_span_for(Kind kind,std::uint64_t first_element,std::uint64_t elements,
    std::uint64_t resource_bytes,const std::uint32_t *words) {
    if(kind==Kind::none)return {};
    // skin8_extra uses an extra input index before the output index. Confirmed
    // by the decoded kernel's cbuffer loads and u1 bufferStore coordinate.
    const unsigned output_word=kind==Kind::skin8_extra?3u:2u;
    return position_span(first_element,elements,resource_bytes,words[output_word],words[output_word+1]);
}
}
