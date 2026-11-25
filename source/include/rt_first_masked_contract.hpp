#pragma once

// BeginScopeV2 already validated the exact draw ticket/query and native
// Primitive generation on the client. Positive corroboration is required
// here only for the new first-pass attempt, not existing routes.
static bool RtFirstMaskedOwner(bool began,const WWMI_RtAuthorDeformedScope_v2& scope,
    const WWMI_RtAuthorSnapshot_v1& a,const WWMI_RtAuthorInstance_v1& i,
    uint64_t device,uint64_t load,uint64_t mod,unsigned component){
    if(!began||!a.token||!a.draw_ticket||!a.source.list_creation||!a.source.recording||
       !scope.base.plan||scope.base.disposition!=WWMI_RT_AUTHOR_TENTATIVE)return false;
    const auto& p=scope.owner;
    const auto& w=p.native_write;
    const auto& n=a.binding.native;
    return p.device==device&&p.plan==scope.base.plan&&p.native_snapshot==a.token&&
        i.resource&&p.primitive==i.resource&&p.primitive_creation&&p.primitive_byte_origin==i.byte_origin&&
        p.load_generation==load&&p.mod_key==mod&&p.mod_revision&&
        p.kind==WWMI_RT_DEFORMED_UNKNOWN&&!p.recipe_receipt&&!p.reserved&&!w.reserved&&
        w.component==component&&n.component==component&&w.order&&w.order==a.source.pose_order&&
        w.writer.list==n.list&&w.writer.creation==n.list_creation&&w.writer.recording==n.recording&&
        w.resource==n.output_resource&&w.creation==n.output_creation&&w.byte_offset==n.output_offset&&w.bytes==n.output_bytes;
}

static bool RtFirstMaskedRest(const WWMI_RtDeformedSelection_v1& owner,const WWMI_RtDeformedSelection_v1* selected){
    if(!selected||selected->kind!=WWMI_RT_DEFORMED_AUTHOR_REST||!selected->recipe_receipt)return false;
    auto expected=owner;
    expected.kind=selected->kind;
    expected.recipe_receipt=selected->recipe_receipt;
    return std::memcmp(&expected,selected,sizeof(expected))==0;
}
