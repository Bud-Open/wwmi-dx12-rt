// In the captured LocalVF shaders the auxiliary attribute is manually fetched
// through a VS t-register. It need not appear in the input assembler at all.
#include "rt_aux_proof_note.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <string_view>
#include <tuple>

namespace {
using AuxProfile = std::pair<uint32_t,uint32_t>;
auto RtAuxProofKey(const AuxProfile& profile,const RtAuxSource& src){
    const auto& view=src.view;
    return std::make_tuple(profile,src.resource_generation,view.resource,view.first,view.count,
        view.format,view.mapping,view.stride,view.flags);
}
using AuxKey=decltype(RtAuxProofKey(AuxProfile{},RtAuxSource{}));

enum class AuxNoteKind { learned, evicted, unobserved, miss, resolved, matched_fallback, evicted_miss };
struct AuxAudit {
    struct Limit {unsigned rows=0;bool saturated=false;};
    Limit limit[7]{};
    std::map<AuxKey,std::pair<uint64_t,uint64_t>> retired; // Only eviction receipts we actually emitted.
    using Key=std::tuple<AuxKey,int32_t,unsigned,uint64_t,uint64_t,unsigned,unsigned,unsigned,int32_t>;
    using Result=std::tuple<RtAuxInput,uint64_t,std::string_view,std::string_view,bool,bool,bool,bool>;
    std::map<Key,Result> outcomes[5]; // Cold observations cannot suppress a proved-key fallback.
};
const char* RtAuxKind(AuxNoteKind kind){
    constexpr const char* names[]{"proof-learned","proof-evicted","unobserved-outcome","unproved-outcome","resolved-outcome","matched-fallback","evicted-miss"};
    return names[unsigned(kind)];
}
struct AuxAuditPermit {
    AuxNoteKind kind;
    const char* capped=nullptr;
    unsigned cap=0;
    uint64_t evicted_proof=0;
    bool emit=false;
};
AuxAuditPermit RtReserveAuxAudit(const AuxKey& key,AuxNoteKind kind,uint64_t epoch,uint64_t proof,
                                const RtAuxProofNote* note=nullptr,const RtAuxDrawReceipt* draw=nullptr){
    constexpr unsigned max_profiles=16, max_rows=64;
    static std::mutex mutex;
    static std::map<AuxProfile,AuxAudit> profiles;
    static bool profile_limit=false;
    const auto& profile=std::get<0>(key);
    uint64_t evicted_proof=0;
    {
        std::lock_guard lock(mutex);
        auto it=profiles.find(profile);
        if(it==profiles.end()&&profiles.size()<max_profiles)it=profiles.try_emplace(profile).first;
        if(it==profiles.end()){
            if(profile_limit)return {kind};
            profile_limit=true;return {kind,"profiles",max_profiles};
        }else{
            auto& audit=it->second;
            if(kind==AuxNoteKind::miss&&note&&note->input==RtAuxInput::missing){
                const auto lost=audit.retired.find(key);
                if(lost!=audit.retired.end()&&epoch>=lost->second.first){
                    kind=AuxNoteKind::evicted_miss;evicted_proof=lost->second.second;
                }
            }
            auto& limit=audit.limit[unsigned(kind)];
            if(limit.saturated)return {kind};
            if(draw&&note){
                const AuxAudit::Key draw_key{key,draw->mod,draw->component,draw->vs,draw->ps,draw->mrt,draw->first,draw->count,draw->base};
                const AuxAudit::Result result{note->input,proof,draw->route,draw->reason,draw->section_violation,
                    draw->caller_cancelled,draw->draw_violation,draw->omitted_sections!=0};
                const auto group_index=unsigned(kind)-unsigned(AuxNoteKind::unobserved);
                auto& group=audit.outcomes[group_index];
                auto prev=group.find(draw_key);
                if(prev!=group.end()&&prev->second==result)return {kind};
                if(limit.rows<max_rows){
                    // A→B→A outcomes must record both transitions, even when
                    // the stable precise key and final fallback reason repeat.
                    for(unsigned i=0;i<5;++i)if(i!=group_index)audit.outcomes[i].erase(draw_key);
                    group.insert_or_assign(draw_key,result);
                }
            }
            if(limit.rows==max_rows){limit.saturated=true;return {kind,RtAuxKind(kind),max_rows};}
            ++limit.rows;
            if(kind==AuxNoteKind::evicted)audit.retired.insert_or_assign(key,std::pair{epoch,proof});
        }
    }
    return {kind,nullptr,0,evicted_proof,true};
}
void RtEmitAuxAudit(const AuxKey& key,AuxAuditPermit permit,uint64_t epoch,uint64_t proof,
                    const RtAuxProofNote* note=nullptr,const RtAuxDrawReceipt* draw=nullptr,
                    const RtAuxSource* source=nullptr){
    const auto& [profile,generation,resource,first,count,format,mapping,stride,flags]=key;
    const auto [guid,hash]=profile;
    if(permit.capped){
        logger::probe("RT-WORLD-AUX-PROOF limit=%s profile=%08x/%08x cap=%u later-events-omitted",permit.capped,guid,hash,permit.cap);
        return;
    }
    if(!permit.emit)return;
    const auto kind=permit.kind;
    constexpr const char* inputs[]{"not-reached","unobserved","direct-color","constant-native","color-match","no-color-proof","ineligible-view"};
    logger::probe("RT-WORLD-AUX-PROOF event=%s profile=%08x/%08x epoch=%llu proof=%llu evicted_proof=%llu resource=%llx generation=%llu "
                  "view=%llu+%u format=%u mapping=%x stride=%u flags=%x source=%s heap=%llx slot=%u heap_gen=%llu write=%llu "
                  "input=%s t=%d mask=%x native_base=%u route=%s reason=%s frame=%llu rec=%llu ticket=%llu mod=%d plan=%llu comp=%u vs=%llx ps=%llx pso=%llx mrt=%u draw=%u@%u base=%d "
                  "section_violation=%u caller_cancelled=%u draw_violation=%u omitted_sections=%u",
        RtAuxKind(kind),guid,hash,(unsigned long long)epoch,(unsigned long long)proof,(unsigned long long)permit.evicted_proof,(unsigned long long)resource,
        (unsigned long long)generation,(unsigned long long)first,count,unsigned(format),mapping,stride,flags,
        note?note->origin:source?"published-live":"proof-cache",(unsigned long long)(note?note->heap:0),note?note->slot:0,
        (unsigned long long)(source?source->heap_generation:0),(unsigned long long)(source?source->write_generation:0),
        note?inputs[unsigned(note->input)]:"direct-color",note?note->aux_register:-1,note?note->mask:0,note?note->native_base:0,
        draw?draw->route:"proof-cache",draw?draw->reason:(kind==AuxNoteKind::learned?"direct-color":"lru-capacity"),
        (unsigned long long)(draw?draw->frame:0),(unsigned long long)(draw?draw->recording:0),(unsigned long long)(draw?draw->ticket:0),
        draw?draw->mod:-1,(unsigned long long)(draw?draw->plan:0),draw?draw->component:0,
        (unsigned long long)(draw?draw->vs:0),(unsigned long long)(draw?draw->ps:0),
        (unsigned long long)(draw?draw->pso:0),draw?draw->mrt:0,draw?draw->count:0,draw?draw->first:0,draw?draw->base:0,
        unsigned(draw&&draw->section_violation),unsigned(draw&&draw->caller_cancelled),unsigned(draw&&draw->draw_violation),
        draw?draw->omitted_sections:0);
}
}

void RtFinishAuxProofNote(const RtAuxProofNote& note,const RtAuxDrawReceipt& draw){
    if(note.input==RtAuxInput::none)return;
    const bool resolved=note.input==RtAuxInput::match||note.input==RtAuxInput::constant||
        (note.input==RtAuxInput::direct&&note.proof!=0);
    const std::string_view route=draw.route;
    const auto kind=note.proof&&(route=="section-native"||draw.section_violation)?AuxNoteKind::matched_fallback:
        resolved?AuxNoteKind::resolved:note.source.view.resource?AuxNoteKind::miss:AuxNoteKind::unobserved;
    const auto key=RtAuxProofKey({note.guid,note.hash},note.source);
    RtEmitAuxAudit(key,RtReserveAuxAudit(key,kind,note.epoch,note.proof,&note,&draw),note.epoch,note.proof,&note,&draw,&note.source);
}

namespace {
bool RtAuxColorProof(const AuxProfile& profile,const RtAuxSource& src,bool direct,RtAuxProofNote& note){
    if(src.view.format!=DXGI_FORMAT_R8G8B8A8_UNORM||src.view.stride||src.view.flags){
        note.input=RtAuxInput::ineligible;return false;
    }
    note.input=RtAuxInput::missing;
    // The caller's original published slot already checked current resource
    // lifetime. Direct COLOR proves this complete view, not the latest view
    // seen for a component. Other render passes may bind a different window
    // without revoking this fact. Retain only bounded non-owning evidence.
    struct Stamp {uint64_t recent,learned;};
    static std::mutex mutex;
    static std::map<AuxKey,Stamp> proofs;
    static uint64_t next=0;
    constexpr size_t max_proofs=128;
    const auto key=RtAuxProofKey(profile,src);
    std::optional<std::pair<AuxKey,uint64_t>> evicted;
    AuxAuditPermit eviction_permit{AuxNoteKind::evicted};
    {
        std::lock_guard lock(mutex);
        note.epoch=next;
        if(auto found=proofs.find(key);found!=proofs.end()){
            found->second.recent=++next;
            note.epoch=next;note.proof=found->second.learned;return true;
        }
        if(!direct)return false;
        if(proofs.size()==max_proofs){
            const auto oldest=std::min_element(proofs.begin(),proofs.end(),[](const auto& a,const auto& b){return a.second.recent<b.second.recent;});
            evicted.emplace(oldest->first,oldest->second.learned);
            // Publish the bounded retirement receipt before another reader
            // can observe the miss. File logging remains outside both locks.
            eviction_permit=RtReserveAuxAudit(evicted->first,AuxNoteKind::evicted,next+1,evicted->second);
            proofs.erase(oldest);
        }
        ++next;
        proofs.emplace(key,Stamp{next,next});
        note.epoch=note.proof=next;
    }
    if(evicted)RtEmitAuxAudit(evicted->first,eviction_permit,note.epoch,evicted->second);
    RtEmitAuxAudit(key,RtReserveAuxAudit(key,AuxNoteKind::learned,note.epoch,note.proof),note.epoch,note.proof,nullptr,nullptr,&src);
    return true;
}
bool RtResolveAuxColor(List* list,const RtLocalProgram& p,uint32_t guid,uint32_t hash,unsigned,RtWorldSurface& world){
    RegBind b{};RtAuxSource src{};
    RtAuxProofNote local{};
    auto& note=rt_aux_note?*rt_aux_note:local;
    note={RtAuxInput::unobserved,guid,hash,{},0,0,p.aux,world.local[0],world.local[3]};
    if(reg_of(list,true,p.aux,b)&&b.is_table){
        note.origin="table-not-resolved";
        auto* dev=list->get_device();auto start=TableStartOf(dev,CmdGfx(list).root[b.param].table);
        if(start && b.offset<=UINT32_MAX-start.base){
            note.heap=start.heap;note.slot=start.base+b.offset;
            const bool found=RtReadCopiedAuxView(dev,start.heap,note.slot,src);
            note.origin=found?"published-live":"unpublished-or-not-live";
        }
    }
    note.source=src;
    if(!src.view.resource||!src.resource_generation||!src.view.count){
        // The direct COLOR-varying programs already have an established author
        // binding; the newly observed source is not a prerequisite for them.
        return !p.auxiliary_requires_native_color;
    }
    if(p.auxiliary_requires_native_color && world.local[0]==0){
        world.aux_constant=true;world.aux_source=src;
        note.input=RtAuxInput::constant;return true;
    }
    const AuxProfile key{guid,hash};
    if(!p.auxiliary_requires_native_color){
        if(RtAuxColorProof(key,src,true,note))world.aux_source=src;
        note.input=RtAuxInput::direct;return true;
    }
    const bool known=RtAuxColorProof(key,src,false,note);
    if(known){
        world.aux_source=src;
        note.input=RtAuxInput::match;
        return true;
    }
    // Single-component or split-mesh character models (e.g. ult/form meshes with no eyes/accessories)
    // may never execute a direct-color VS on the same (guid, hash), leaving proofs[key] empty forever
    // and forcing every color-reading pass to fall back to native (vanilla outfit bleeding through).
    // Once RtIdentifyDraw has proven the native mesh's vertex range, accept and learn a live published
    // RGBA8 non-structured buffer view covering [0, native_vertex_end) when LocalVF color indexing is on.
    if(t_rt_aux_native_vertex_end>0 &&
       world.local[0]!=0 && world.local[3]==0 &&
       src.view.format==DXGI_FORMAT_R8G8B8A8_UNORM &&
       src.view.stride==0 && src.view.flags==0 &&
       src.view.count>=t_rt_aux_native_vertex_end &&
       src.view.count<=262144u &&
       RtAuxColorProof(key,src,true,note)){
        world.aux_source=src;
        note.input=RtAuxInput::match;
        return true;
    }
    return false;
}
}
