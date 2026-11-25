#pragma once
namespace hsin_rt {
// Internal pre-skin input, never a WWMI_RtPositionMode_v1 publication. FINAL and
// REST_VERIFIED retain their existing meanings. Only the shape consumer below
// constructs this mode after checking its actual native-pose/snapshot lease.
inline constexpr unsigned position_author_shape_delta_float6=0x100;
}
