#pragma once
#include <stdint.h>
#include <reshade.hpp>
bool RtRasterAttach();
void RtRasterDetach();
bool RtTryGpuSkinDraw(reshade::api::command_list*,uint32_t,uint32_t,uint32_t,int32_t,uint32_t);

void RtVisibleFieldPresent();
