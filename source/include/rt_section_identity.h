#pragma once
#include "rt_mod_source.h"
#ifdef __cplusplus
extern "C" {
#endif
// Main-only narrow lookup of the current actual section token. It copies no
// catalog or resources and does not evaluate an INI or change the selection.
typedef struct WWMI_RtModSectionIdentity_v1 {
    uint32_t struct_size,abi_version;
    uint64_t load_generation,mod_key,mod_revision;
    int32_t component;
    uint32_t selection_known;
} WWMI_RtModSectionIdentity_v1;
typedef uint32_t (*WWMI_RtModReadSectionIdentityFn_v1)(uint64_t token,WWMI_RtModSectionIdentity_v1 *out,uint32_t out_size);
WWMI_RT_EXPORT uint32_t WWMI_RtModReadSectionIdentity_v1(uint64_t token,WWMI_RtModSectionIdentity_v1 *out,uint32_t out_size);
#ifdef __cplusplus
}
#endif
