#pragma once
// Include after the existing author frame ABI. This larger input is
// negotiated by size on the same v2 export; older v1/v2 callers are unchanged.
struct WWMI_RtAuthorIndexView_v3 {
    uint64_t gpu_va,offset,bytes;
    uint32_t count;
};
struct WWMI_RtAuthorSectionParts_v3 {
    // Both arrays have first.base.subdraw_count elements. Borrowed only
    // during this call. Each exact issued ordinal has its own index range.
    const WWMI_RtModDraw_v1 *draws;
    WWMI_RtAuthorIndexView_v3 *indices;
};
struct WWMI_RtAuthorSectionFrameRequest_v3 {
    WWMI_RtAuthorDeformedFrameRequest_v2 first;
    WWMI_RtAuthorSectionParts_v3 parts;
};
