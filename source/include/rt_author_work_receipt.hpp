#pragma once
#include <cstdint>
struct WWMI_RtAuthorCommit_v1;

// Optional same-recording section observation, not permission to draw.
// BEFORE is immediately before the real part loop; AFTER reports what it
// actually issued. The receiver proves timestamp eligibility independently;
// a zero timer still permits the AFTER work count.
enum { WWMI_RT_SECTION_WORK_BEFORE=1, WWMI_RT_SECTION_WORK_AFTER=2 };
using WWMI_RtPackObserveAuthorSectionFn_v1=std::uint64_t (*)(const WWMI_RtAuthorCommit_v1 *,
    std::uint32_t,std::uint64_t deadline_ms,std::uint64_t frame,std::uint32_t phase,
    std::uint64_t timer,std::uint32_t parts,std::uint64_t indices);
