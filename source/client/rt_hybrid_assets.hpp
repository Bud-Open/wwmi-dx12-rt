#pragma once
#include <algorithm>
#include <cstring>
#include <span>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "rt_hybrid_raster.hpp"

namespace wwmi::hybrid {
template<class T> T read(std::span<const unsigned char> bytes, std::size_t i) {
    T out{};
    std::memcpy(&out,bytes.data()+i*sizeof(T),sizeof(T));
    return out;
}
inline std::span<const unsigned char> view(Bytes b) {
    return {static_cast<const unsigned char*>(b.data),static_cast<std::size_t>(b.size)};
}

// The kernels own one whole, non-overlapping author domain. The *meaning*
// of a subdraw is nevertheless proved only for its real signed IB window.
struct AssetProof {
    unsigned vertices=0,bones=0,rows=0;
    bool shape_oob=false;
    std::vector<unsigned char> normalized,source_indices;
    std::vector<std::uint16_t> full,original,forwards,reverses;
    std::vector<bool> shaped;
    struct Key {
        unsigned count=0,first=0;
        int base=0;
        Blend blend=raw;
        unsigned remap_row=0,remap_count=0;
        bool operator==(const Key&)const=default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k)const noexcept{
            std::uint64_t h=std::uint64_t(k.count)*0x9e3779b97f4a7c15ull;
            h^=std::uint64_t(k.first)+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);
            h^=std::uint64_t(std::uint32_t(k.base))+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);
            h^=(std::uint64_t(k.blend)<<32|std::uint64_t(k.remap_row)<<16|std::uint64_t(k.remap_count))+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);
            return static_cast<std::size_t>(h);
        }
    };
    struct Window {bool valid=false,shape=false,remap_mismatch=false; unsigned max_bone=0;};
    std::unordered_map<Key,Window,KeyHash> windows;
    bool load(const Asset& a){
        const auto as=std::span(a.streams);
        const bool shape_stream_ok=a.shapes
            ?(as[shape_ids].stride==4&&as[shape_ids].size==4ull*a.shapes)
            :(as[shape_ids].size==0);
        if(!a.vertices||a.vertices>65535u*64u||!a.bones||!a.bone_windows||
           as[positions].size!=12ull*a.vertices||as[positions].stride!=12||
           as[indices].stride!=4||!as[indices].size||as[indices].size%4||
           !shape_stream_ok||
           (as[blends].stride!=8&&as[blends].stride!=16&&as[blends].stride!=32)||
           as[blends].size!=std::uint64_t(as[blends].stride)*a.vertices)return false;
        for(const auto &s:as)if(s.size&&!s.data)return false;
        vertices=a.vertices;
        shape_oob=false;
        shaped.assign(vertices,false);
        for(unsigned i=0;i<a.shapes;++i){
            const auto v=read<std::uint32_t>(view(as[shape_ids]),i);
            if(v>=vertices){shape_oob=true;continue;}
            shaped[v]=true;
        }
        source_indices.assign(view(as[indices]).begin(),view(as[indices]).end());
        std::uint64_t max_bone=0;
        for(unsigned i=0;i<a.bone_windows;++i){
            const auto &b=a.bones[i];
            if(!b.count||b.count>512||b.first>65536-b.count)return false;
            max_bone=(std::max)(max_bone,std::uint64_t(b.first)+b.count);
        }
        bones=unsigned(max_bone);
        const unsigned influences=as[blends].stride==8?4u:8u;
        const bool remap=as[full_vg].size||as[forward].size||as[reverse].size;
        if(remap){
            if((as[blends].stride!=8&&as[blends].stride!=16)||
               (as[full_vg].size!=2ull*influences*vertices&&as[full_vg].size!=16ull*vertices)||
               !as[forward].size||as[forward].size%1024||as[forward].size!=as[reverse].size)return false;
            full.assign(8ull*vertices,0);
            if(as[full_vg].size==16ull*vertices){
                std::memcpy(full.data(),as[full_vg].data,as[full_vg].size);
            }else{
                const auto *src=static_cast<const unsigned char*>(as[full_vg].data);
                for(unsigned v=0;v<vertices;++v)
                    std::memcpy(full.data()+8ull*v,src+2ull*influences*v,2ull*influences);
            }
            forwards.resize(as[forward].size/2);reverses.resize(forwards.size());rows=unsigned(forwards.size()/512);
            std::memcpy(forwards.data(),as[forward].data,as[forward].size);
            std::memcpy(reverses.data(),as[reverse].data,as[reverse].size);
        }
        normalized.resize(32ull*vertices);
        original.resize(8ull*vertices);
        const auto raw=view(as[blends]);
        for(unsigned v=0;v<vertices;++v)for(unsigned s=0;s<8;++s){
            const auto slot=8ull*v+s;
            std::uint16_t native_bone=0,weight=0;
            if(s<influences){
                if(as[blends].stride==8){
                    native_bone=raw[8ull*v+s];
                    weight=std::uint16_t(raw[8ull*v+4+s]*257u);
                }else if(as[blends].stride==16){
                    native_bone=raw[16ull*v+s];
                    weight=std::uint16_t(raw[16ull*v+8+s]*257u);
                }else{
                    native_bone=read<std::uint16_t>(raw,16ull*v+s);
                    weight=read<std::uint16_t>(raw,16ull*v+8+s);
                }
            }
            const auto canonical=remap?full[slot]:native_bone;
            original[slot]=native_bone;
            // Existing kernels read rows even for zero weight. Any unloaded
            // influence is safe to read but that vertex cannot be selected.
            const std::uint16_t safe=weight&&canonical<bones?canonical:0;
            std::memcpy(normalized.data()+32ull*v+2*s,&safe,2);
            std::memcpy(normalized.data()+32ull*v+16+2*s,&weight,2);
        }
        return true;
    }
    Window prove(Part p){
        const Key key{p.count,p.first,p.base,p.blend,p.remap_row,p.remap_count};
        if(const auto it=windows.find(key);it!=windows.end())return it->second;
        Window w{true,shape_oob,false,0};
        const auto n=source_indices.size()/4;
        if(!p.count||p.count%3||p.first>n||p.count>n-p.first||
           (p.blend!=raw&&p.blend!=paired_remap)||
           (p.blend==paired_remap&&(p.remap_row>=rows||!p.remap_count||p.remap_count>256)))w.valid=false;
        const auto row=512ull*p.remap_row;
        for(unsigned i=0;w.valid&&i<p.count;++i){
            const auto id=std::int64_t(read<std::uint32_t>(source_indices,std::size_t(p.first)+i))+p.base;
            if(id<0||id>=vertices){w.valid=false;break;}
            w.shape|=shaped[id];
            for(unsigned s=0;s<8;++s){
                if(!read<std::uint16_t>(normalized,16ull*id+8+s))continue;
                const auto slot=8ull*id+s;
                const unsigned vg=full.empty()?original[slot]:full[slot];
                if(vg>=bones){w.valid=false;break;}
                if(p.blend==raw){
                    if(vg!=original[slot])w.remap_mismatch=true;
                }else if(p.blend==paired_remap){
                    if(vg>=512){
                        w.remap_mismatch=true;
                    }else{
                        const auto compact=reverses[row+vg];
                        if(compact>=p.remap_count||forwards[row+compact]!=vg)w.remap_mismatch=true;
                    }
                }else {w.valid=false;break;}
                w.max_bone=(std::max)(w.max_bone,vg);
            }
        }
        return windows.emplace(key,w).first->second;
    }
};
}
