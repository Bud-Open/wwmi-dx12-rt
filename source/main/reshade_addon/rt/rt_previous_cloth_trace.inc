// A synchronous join key for the armed previous-surface window. No drawing,
// resource selection or GPU state is changed. The sequence is zero outside
// the actual previous-surface call, including outside the armed window.
static thread_local uint64_t rt_previous_cloth_sequence=0;
static thread_local uint64_t rt_previous_draw_sequence=0;
extern "C" __declspec(dllexport) uint64_t WWMI_RtClothPreviousTraceSeq_v1(){
    return rt_previous_cloth_sequence;
}
#include "rt_draw_window.h"
extern "C" __declspec(dllexport) uint64_t WWMI_RtDrawWindowDeadline_v1(uint64_t device,uint64_t *sequence){
    if(sequence)*sequence=0;
    if(!device||device!=wwmi_rt_pack_frame_device.load(std::memory_order_acquire))return 0;
    const auto phase=field_state.load(std::memory_order_acquire);
    if(!phase)return 0;
    if(sequence)*sequence=rt_previous_draw_sequence;
    return phase==1?field_begin+kRtDrawWindowMs:field_end;
}
struct RtPreviousClothTraceScope {
    uint64_t saved_draw,saved_cloth;
    RtPreviousClothTraceScope(const RtClothSection* section,bool cloth)
        :saved_draw(rt_previous_draw_sequence),saved_cloth(rt_previous_cloth_sequence){
        rt_previous_draw_sequence=section?section->trace_seq:0;
        rt_previous_cloth_sequence=cloth?rt_previous_draw_sequence:0;
    }
    ~RtPreviousClothTraceScope(){rt_previous_draw_sequence=saved_draw;rt_previous_cloth_sequence=saved_cloth;}
};
