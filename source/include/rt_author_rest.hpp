#pragma once
#include "rt_author_deformed.h"
#include "rt_shape_source.hpp"

// Actual resolved author Position binding taken at the VM's draw, not from
// its final state after resource restoration.
struct RtAuthorPositionBinding {
    uint64_t resource=0,va=0,bytes=0;
    unsigned stride=0;
    bool operator==(const RtAuthorPositionBinding &)const=default;
};
wwmi::deform::Publication RtPublishSelectedAuthorRest(uint64_t section,uint64_t completed_recipe,
    const WWMI_RtDeformedSelection_v1 &,const RtAuthorPositionBinding &);
