#pragma once
#include <reshade.hpp>
#include "native-buffer-view.h"
struct RtAuxSource {
    WWMI_NativeBufferView_v1 view{};
    uint64_t resource_generation=0,heap_generation=0,write_generation=0;
};
// The destination slot's observed value at this draw, not the current CPU source.
bool RtReadCopiedAuxView(reshade::api::device*,uint64_t heap,uint32_t index,RtAuxSource&) noexcept;
