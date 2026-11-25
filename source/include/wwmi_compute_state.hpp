// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "compute_scope.hpp"
#include <d3d12.h>
namespace wwmi::compute_state {
struct RestoreProof {
    uint64_t heap_tags[2]={0,0};
    bool heaps_known=false, root_known=false, tables_known=false, non_tables_known=false;
};
struct GraphicsTables {
    enum class Pass : uint8_t { unknown, outside, open, suspending };
    uint64_t root=0, written=0;
    uint64_t bases[64]; // read only under the corresponding written bit
    bool root_known=false, tables_known=false;
    Pass pass=Pass::unknown;

    void clear_tables(bool known) noexcept {written=0;tables_known=known;}
    void forget_root() noexcept {root=0;root_known=false;clear_tables(false);}
    void reset() noexcept {root=0;root_known=true;clear_tables(true);pass=Pass::outside;}
};
struct __declspec(uuid("3d9da6c4-914b-42c1-b0ef-2759546492cd")) State {
    hsin_rt::ComputeScope snapshot;
    // With ADDON >= 2, these are actual registered heap creation generations.
    uint64_t epoch=0, heaps[2]={0,0};
    bool inside=false, recording_known=false;
    RestoreProof restore;
    GraphicsTables graphics;
};
uint64_t current_epoch() noexcept;
template<class F> void observe(State &s, F apply) noexcept {
    const uint64_t epoch=current_epoch();
    if(!(epoch&1)||s.inside)return;
    try {
        if(s.epoch!=epoch){
            s.snapshot.reset();s.heaps[0]=s.heaps[1]=0;s.epoch=epoch;s.recording_known=false;
            s.restore={};
            s.graphics.forget_root();s.graphics.pass=GraphicsTables::Pass::unknown;
        }
        apply(s.snapshot);
    } catch(...) {
        s.snapshot.invalidate();s.recording_known=false;s.restore={};
        s.graphics.clear_tables(false);
    }
}
inline void reset(State &s,uint64_t pso) noexcept {
    observe(s,[&](auto &v){
        v.reset(pso);s.heaps[0]=s.heaps[1]=0;s.recording_known=true;
        s.restore={{0,0},true,true,true,true};
        s.graphics.reset();
    });
}
inline void invalidate_legacy(State &s,hsin_rt::ComputeScope &v) noexcept {
    v.invalidate();v.pipeline(hsin_rt::PipelineKind::ordinary,0);s.recording_known=false;
}
inline void unobserved(State &s,bool pass_unknown=true) noexcept {
    observe(s,[&](auto &v){
        invalidate_legacy(s,v);s.restore={};
        s.graphics.forget_root();
        if(pass_unknown)s.graphics.pass=GraphicsTables::Pass::unknown;
    });
}
inline void indirect(State &s) noexcept {
    observe(s,[&](auto &v){
        // Native indirect signatures can change root descriptors/constants,
        // but do not set descriptor heaps, the root signature, or tables.
        invalidate_legacy(s,v);
        s.restore.non_tables_known=false;
    });
}
inline void signature(State &s,uint64_t root) noexcept {
    observe(s,[&](auto &v){
        const bool new_arguments=root==0||(s.restore.root_known&&v.root_value()!=root);
        v.signature(root);
        if(new_arguments)s.restore.tables_known=s.restore.non_tables_known=true;
        s.restore.root_known=true;
    });
}
inline void binding(State &s,hsin_rt::RootKind kind,uint32_t parameter,uint64_t value) noexcept {
    observe(s,[&](auto &v){
        if(!v.binding(kind,parameter,value)){
            if(kind==hsin_rt::RootKind::table)s.restore.tables_known=false;
            else s.restore.non_tables_known=false;
        }
    });
}
inline void constants(State &s,uint32_t parameter,uint32_t first,uint32_t count,const uint32_t *words) noexcept {
    observe(s,[&](auto &v){
        if(!v.constants(parameter,first,count,words))s.restore.non_tables_known=false;
    });
}
inline void graphics_signature(State &s,uint64_t root) noexcept {
    observe(s,[&](auto &){
        auto &g=s.graphics;
        if(root==0||(g.root_known&&g.root!=root))g.clear_tables(true);
        g.root=root;g.root_known=true;
    });
}
inline void graphics_table(State &s,uint32_t parameter,uint64_t base) noexcept {
    observe(s,[&](auto &){
        auto &g=s.graphics;
        if(!g.root_known||parameter>=64){g.clear_tables(false);return;}
        if(!g.tables_known)return;
        g.bases[parameter]=base;
        g.written|=uint64_t(1)<<parameter;
    });
}
inline void begin_render_pass(State &s,D3D12_RENDER_PASS_FLAGS flags) noexcept {
    observe(s,[&](auto &){
        s.graphics.pass=(flags&D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS)?
            GraphicsTables::Pass::suspending:GraphicsTables::Pass::open;
    });
}
inline void end_render_pass(State &s) noexcept {
    observe(s,[&](auto &){
        // A suspended pass or an End without an observed Begin does not
        // prove the command list is in a scope where a heap switch is legal.
        s.graphics.pass=s.graphics.pass==GraphicsTables::Pass::open?
            GraphicsTables::Pass::outside:GraphicsTables::Pass::unknown;
    });
}
template<class Read> void heaps(State &s, Read read) noexcept {
    observe(s,[&](auto &v){
        uint64_t next[2]={0,0},tags[2]={0,0};
        const bool known=read(next,tags);
        const bool changed=next[0]!=s.heaps[0]||next[1]!=s.heaps[1];
        if(changed)v.clear_tables();
        if(!known||(!s.restore.heaps_known&&s.graphics.written))
            s.graphics.clear_tables(false);
        else if(s.restore.heaps_known&&changed)
            s.graphics.clear_tables(true);
        if(!known||!s.restore.heaps_known)s.restore.tables_known=false;
        else if(changed)s.restore.tables_known=true;
        s.restore.heaps_known=known;
        s.restore.heap_tags[0]=tags[0];s.restore.heap_tags[1]=tags[1];
        s.heaps[0]=next[0];s.heaps[1]=next[1];
    });
}
inline void forget_heaps(State &s) noexcept {
    observe(s,[&](auto &v){
        // The SDK raw call can be redundant (for example bind with count=0).
        // Without the heap-pair proof, dropping cached tables is not proof
        // that the native list has no active tables.
        v.invalidate();
        v.clear_tables();s.heaps[0]=s.heaps[1]=0;
        s.restore.heap_tags[0]=s.restore.heap_tags[1]=0;
        s.restore.heaps_known=s.restore.tables_known=false;
        s.graphics.clear_tables(false);
    });
}
inline void forget_arguments(State &s) noexcept {
    observe(s,[&](auto &v){
        // The mipmap loop records real constants/tables outside this snapshot.
        v.invalidate();
        v.clear_tables();
        s.restore.tables_known=s.restore.non_tables_known=false;
    });
}
}
