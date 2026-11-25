#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Native pipeline capability at the original draw, not a claim that any
// sample passed coverage/depth, and not permission to discard the draw.
enum class RtColorKnowledge : uint8_t { Unknown, KnownMask, KnownZero };
enum class RtColorBasis : uint8_t {
    NoClone, UnknownOm, InvalidState, PixelSignature, NoPixel, Reflected, BlendUnreported
};
enum class RtEffect : uint8_t { Unknown, Absent, Possible };

struct RtRasterShaderFacts {
    bool outputs_known = false, standard_shape = false;
    uint8_t color_slots = 0;
    std::array<uint8_t,8> color_channels{};
    bool depth = false, coverage = false, stencil_ref = false;
    RtEffect uav = RtEffect::Unknown;
};
void RtReadRasterShaderFacts(const void *code, size_t size, bool pixel, RtRasterShaderFacts &out);

struct RtRasterEffects {
    RtColorKnowledge color = RtColorKnowledge::Unknown;
    RtColorBasis basis = RtColorBasis::NoClone;
    uint8_t color_mask = 0, declared_slots = 0, write_slots = 0, attached_slots = 0;
    bool standard_pixel_shape = false;
    RtEffect depth_test{}, depth_write{}, stencil_test{}, stencil_write{};
    RtEffect coverage{}, uav{}, stream_output{}, queries{}, predication{};
    bool pixel_depth = false, pixel_coverage = false, pixel_stencil_ref = false;
};
RtRasterEffects RtReadRasterEffects(uint64_t pipeline, uint8_t attached, bool om_known, bool dsv);

enum class RtDrawExecution : uint8_t {
    Unrelated, NativeSelected, PrivateRecorded, HiddenSelected, PlanViolation
};
struct RtDrawOutcome {
    RtDrawExecution execution = RtDrawExecution::Unrelated;
    // A different section can fail after a replacement already recorded.
    // Never replay the original on top of a recorded/explicitly hidden draw.
    bool plan_violation = false;
    bool cancels_native() const {
        return execution == RtDrawExecution::PrivateRecorded ||
               execution == RtDrawExecution::HiddenSelected;
    }
};
