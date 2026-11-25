// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "producer_trace.h"
#include <exception>
#include <string>
#include <limits>

namespace wwmi::producer {
// TLS contains scoped *hints*, not a game instance or current-pose receipt.
struct Context { Token token{}; WWMI_ProducerInput_v1 input{}; };
inline thread_local Context current_context{};
inline Token CurrentParent() noexcept {
    return current_context.token.session==SnapshotSession()?current_context.token:Token{};
}
inline WWMI_ProducerInput_v1 Input(uint32_t kind,uint32_t site,uint32_t provenance=WWMI_PRODUCER_PROVENANCE_CANDIDATE_HINT) noexcept {
    WWMI_ProducerInput_v1 r{};r.struct_size=sizeof(r);r.abi_version=1;r.kind=kind;r.source_site=site;r.provenance=provenance;
    r.mod_index=r.owner_mod_index=-1;return r;
}
inline WWMI_ProducerInput_v1 ContextualInput(uint32_t kind,uint32_t site,uint32_t provenance=WWMI_PRODUCER_PROVENANCE_CANDIDATE_HINT) noexcept {
    auto r=Input(kind,site,provenance);
    if(CurrentParent().session) {
        const auto& c=current_context.input;
        constexpr uint64_t hints=WWMI_PRODUCER_KNOWN_MOD_HINT|WWMI_PRODUCER_KNOWN_INSTANCE_HINT|
            WWMI_PRODUCER_KNOWN_FRAME_HINT|WWMI_PRODUCER_KNOWN_NAMESPACE_HASH|WWMI_PRODUCER_KNOWN_PASS;
        r.known_fields=c.known_fields&hints;r.frame_hint=c.frame_hint;r.instance_hint=c.instance_hint;
        r.namespace_hash=c.namespace_hash;r.namespace_units=c.namespace_units;
        r.mod_index=c.mod_index;r.owner_mod_index=c.owner_mod_index;r.pass_index=c.pass_index;r.pass_count=c.pass_count;
    }
    return r;
}
inline uint64_t NamespaceHash(const std::wstring& text) noexcept {
    uint64_t h=UINT64_C(14695981039346656037);
    for(wchar_t c:text) {
        // Production Windows wchar_t is one UTF-16 unit. Portable fixtures use
        // the same supplied 16-bit values; supplementary Linux codepoints are
        // not silently interpreted as a different namespace representation.
        const uint32_t v=static_cast<uint32_t>(c);
        if(v>0xffffu)return 0;
        h^=v&255u;h*=UINT64_C(1099511628211);h^=(v>>8)&255u;h*=UINT64_C(1099511628211);
    }
    return h;
}
inline void SetHints(WWMI_ProducerInput_v1& r,uint64_t list,uint64_t frame,int mod,
                     uint64_t instance,const std::wstring& ns) noexcept {
    r.native_command_list=list;r.frame_hint=frame;r.mod_index=mod;r.instance_hint=instance;
    r.known_fields|=WWMI_PRODUCER_KNOWN_LIST|WWMI_PRODUCER_KNOWN_FRAME_HINT|
        WWMI_PRODUCER_KNOWN_MOD_HINT|WWMI_PRODUCER_KNOWN_INSTANCE_HINT;
    if(ns.size()<=UINT32_MAX) {
        r.namespace_hash=NamespaceHash(ns);r.namespace_units=static_cast<uint32_t>(ns.size());
        if(r.namespace_hash)r.known_fields|=WWMI_PRODUCER_KNOWN_NAMESPACE_HASH;
        else r.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
    } else r.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
}
inline WWMI_ProducerInput_v1 HintInput(uint32_t kind,uint32_t site,uint64_t list,
    uint64_t frame,int mod,uint64_t instance,const std::wstring& ns) noexcept {
    // No namespace hashing or shared-state work when unsubscribed.
    if(!SnapshotSession())return {};
    auto r=Input(kind,site);SetHints(r,list,frame,mod,instance,ns);return r;
}
inline void SetResourceHint(WWMI_ProducerInput_v1& r,uint64_t resource,uint64_t va,
                            uint64_t bytes) noexcept {
    r.resource=resource;r.resource_va=va;r.resource_bytes=bytes;
    if(resource)r.known_fields|=WWMI_PRODUCER_KNOWN_RESOURCE;
    if(resource&&va&&bytes&&bytes<=UINT64_MAX-va)
        r.known_fields|=WWMI_PRODUCER_KNOWN_RESOURCE_RANGE;
    else r.flags|=WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
}
inline void OutputHint(uint32_t site,uint64_t resource,uint64_t va,uint64_t bytes,
                       uint32_t reason) noexcept {
    if(!SnapshotSession())return;
    auto r=ContextualInput(WWMI_PRODUCER_OUTPUT_HINT,site);
    if(CurrentParent().session) {
        r.native_command_list=current_context.input.native_command_list;
        r.known_fields|=current_context.input.known_fields&WWMI_PRODUCER_KNOWN_LIST;
    }
    SetResourceHint(r,resource,va,bytes);r.slot=0;r.reason=reason;
    r.known_fields|=WWMI_PRODUCER_KNOWN_SLOT;Event(r,CurrentParent());
}
inline void RelocationHint(uint32_t site,uint64_t list,uint64_t frame,int mod,
    uint64_t instance,const std::wstring& ns,uint64_t resource,uint64_t va,
    uint64_t view_bytes,uint32_t stride,uint32_t format,uint32_t slot,
    uint64_t prior_resource,uint64_t prior_va) noexcept {
    if(!SnapshotSession())return;
    auto r=HintInput(WWMI_PRODUCER_RELOCATION,site,list,frame,mod,instance,ns);
    SetResourceHint(r,resource,va,view_bytes);r.stride=stride;r.format=format;r.slot=slot;
    r.known_fields|=WWMI_PRODUCER_KNOWN_SLOT;
    r.aux_resource=prior_resource;r.aux_va=prior_va;
    if(prior_resource)r.known_fields|=WWMI_PRODUCER_KNOWN_AUX_RESOURCE;
    // No previous-view size is available here. Never advertise an AUX_RANGE.
    r.reason=slot==UINT32_MAX?2:1;Event(r,CurrentParent());
}
class Scope {
    Context previous_{};
    Token token_{};
    int uncaught_=0;
public:
    WWMI_ProducerInput_v1 data{};
    explicit Scope(const WWMI_ProducerInput_v1& input) noexcept : data(input) {
        if(!SnapshotSession())return;
        previous_=current_context;uncaught_=std::uncaught_exceptions();
        token_=Begin(data,CurrentParent());
        if(token_.session)current_context={token_,data};
    }
    ~Scope() noexcept {
        if(!token_.session)return;
        if(std::uncaught_exceptions()>uncaught_)data.flags|=WWMI_PRODUCER_FLAG_EXCEPTION_UNWIND;
        End(token_,data);current_context=previous_;
    }
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
    Token token() const noexcept { return token_; }
    void UpdateHints() noexcept { if(token_.session)current_context={token_,data}; }
};
template<class NativeList>
inline void NativeDispatch(NativeList* list,uint32_t x,uint32_t y,uint32_t z,uint32_t site,
                           uint64_t pipeline=0,uint64_t root=0) {
    if(!SnapshotSession()) { list->Dispatch(x,y,z);return; }
    auto input=ContextualInput(WWMI_PRODUCER_NATIVE_DISPATCH,site,WWMI_PRODUCER_PROVENANCE_NATIVE_CALL);
    input.native_command_list=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(list));
    input.dispatch_x=x;input.dispatch_y=y;input.dispatch_z=z;
    input.known_fields|=WWMI_PRODUCER_KNOWN_LIST|WWMI_PRODUCER_KNOWN_DISPATCH;
    input.pipeline=pipeline;input.root_signature=root;
    if(pipeline)input.known_fields|=WWMI_PRODUCER_KNOWN_PIPELINE;
    if(root)input.known_fields|=WWMI_PRODUCER_KNOWN_ROOT_SIGNATURE;
    Scope call(input);list->Dispatch(x,y,z);
}
}
