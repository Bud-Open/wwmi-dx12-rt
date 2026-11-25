#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace hsin_rt {
struct NativeComponent {unsigned component=0,first=0,count=0,index_first=0,index_count=0;};
struct NativeProfile {
    unsigned object_guid=0,content_hash=0,vertices=0;
    bool partial=false;
    std::vector<NativeComponent> components;
    std::uint64_t bytes() const {return 12ull*vertices;}
};
struct BonePair {unsigned source=0,canonical=0;};
struct Dependency {unsigned provider=0;std::vector<std::array<unsigned,2>> anchors;};
struct PackDraw {unsigned source_first=0,count=0;std::int32_t source_base=0;unsigned packed_first=0;};
struct PackComponent {
    NativeComponent native;
    bool rest_skinning_known=true,selection_planned=false,mod_author_shared=false,extra_position_input=false;
    unsigned vertex_first=0,vertex_count=0,index_first=0,index_count=0;
    std::vector<BonePair> bone_map;
    std::vector<unsigned> used;
    std::vector<Dependency> dependencies;
    std::vector<PackDraw> draws;
};
struct ModPack {
    std::uint64_t load_generation=0,key=0,revision=0;
    std::wstring name;
    std::shared_ptr<const NativeProfile> native;
    unsigned vertices=0,canonical_bones=0,shapekey_vertices=0;
    bool rest_skinning_known=true;
    std::vector<PackComponent> components;
    // Copied from resolved Mod resources; blends are normalized to U16/UNORM16.
    std::vector<unsigned char> positions,blends,indices,vectors,uv,color;
    unsigned uv_pairs=0;bool surface_supported=false;
};
}
