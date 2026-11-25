#pragma once
#include <string_view>

namespace rt_scene {
// The request identifies this capture session, not a draw, instance or BLAS.
inline bool request_matches(std::string_view request, std::string_view session) noexcept {
    if (!request.empty() && request.back() == '\n') request.remove_suffix(1);
    if (!request.empty() && request.back() == '\r') request.remove_suffix(1);
    return !session.empty() && request == session;
}
}
