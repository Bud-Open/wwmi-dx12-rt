#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>

namespace rt_slot_sequence {
using Tick = std::uint64_t;
constexpr Tick ready_ms=90000,request_ms=120000,stage_ms=15000,terminal_ms=2500;
constexpr unsigned hsin_slot=1,a_slot=2,both_slots=hsin_slot|a_slot,off_slot=3,off_bit=4;
constexpr int hsin_key=0x31,a_key=0x32,off_key=0x33; // main keyboard row, not numpad

inline Tick request_expiry(std::string_view request,std::string_view session) noexcept{
    constexpr std::string_view marker="\nsequence-v1\nexpires_unix_ms=";
    if(session.empty()||!request.starts_with(session))return 0;
    request.remove_prefix(session.size());
    if(!request.starts_with(marker))return 0;
    request.remove_prefix(marker.size());
    if(request.size()<2||request.back()!='\n')return 0;
    request.remove_suffix(1);
    Tick expiry=0;
    const auto end=request.data()+request.size();
    const auto value=std::from_chars(request.data(),end,expiry);
    return value.ec==std::errc{}&&value.ptr==end?expiry:0;
}
inline Tick request_remaining(Tick expires_utc,Tick now_utc) noexcept{
    if(expires_utc<=now_utc)return 0;
    const auto remaining=expires_utc-now_utc;
    // A 120 s request reserves its last 90 s for READY, not late admission.
    return remaining>=ready_ms&&remaining<=request_ms?remaining:0;
}

enum class Phase : unsigned {
    off,ready_release,ready,a1,switch_hsin,hsin,switch_a2,a2,complete,incomplete
};
inline const char *phase_name(Phase phase) noexcept{
    switch(phase){
    case Phase::ready_release:return "ready_release";
    case Phase::ready:return "ready";
    case Phase::a1:return "a1";
    case Phase::switch_hsin:return "switch_hsin";
    case Phase::hsin:return "hsin";
    case Phase::switch_a2:return "switch_a2";
    case Phase::a2:return "a2";
    case Phase::complete:return "complete";
    case Phase::incomplete:return "incomplete";
    default:return "off";
    }
}
enum class Kind {input,prompt,end};
struct Event {Kind kind;unsigned slot;const char *detail;bool early=false;};

// Owned by the existing game Present path. The input adapter only samples
// foreground key levels; this state machine detects edges and never acts on
// the game. Only begin_field can admit the existing diagnostic window.
struct Sequence {
    Phase phase=Phase::off;
    Tick ready_until=0,field_begin=0,stage_due=0,terminal_at=0;
    unsigned previous=0,early_starts=0;
    bool seen_keys=false,started=false;

    void enter_ready(Tick now,Tick request_left) noexcept{
        phase=Phase::ready_release;
        ready_until=now+(request_left<ready_ms?request_left:ready_ms);
    }
    bool waiting() const noexcept{return phase==Phase::ready_release||phase==Phase::ready;}
    bool running() const noexcept{return phase>=Phase::a1&&phase<=Phase::a2;}
    bool needs_input() const noexcept{return waiting()||running();}

    template<class Begin,class Emit>
    void present(Tick now,Tick capture_ms,bool foreground,unsigned keys,Begin begin_field,Emit emit,
                 bool request_current=true){
        if(!needs_input())return;
        if(waiting()&&!request_current){finish(false,"request_expired",now,emit);return;}
        if(waiting()&&now>=ready_until){finish(false,"ready_timeout",now,emit);return;}
        if(running()){
            const auto deadline=field_begin+capture_ms;
            if(now>=deadline&&!(now==deadline&&phase==Phase::a2&&now>=stage_due)){
                finish(false,"window_deadline",now,emit);return;
            }
        }
        if(!foreground){
            if(running())finish(false,"focus_lost",now,emit);
            else{phase=Phase::ready_release;seen_keys=false;}
            return;
        }
        const unsigned pressed=seen_keys?keys&~previous:0;
        previous=keys;seen_keys=true;
        if(pressed&off_bit){
            emit(Event{Kind::input,off_slot,"off_sequence"});
            inputs(pressed,"ambiguous",emit);
            if(waiting())phase=Phase::ready_release;
            else finish(false,"off_sequence_input",now,emit);
            return;
        }
        if(waiting()){
            if(!keys){phase=Phase::ready;return;}
            if(!pressed)return; // a key already held when READY appeared is not an edge
            if((keys&both_slots)==both_slots){
                phase=Phase::ready_release;
                inputs(pressed,"ambiguous",emit);
            }else if(phase==Phase::ready_release){
                inputs(pressed,"release_required",emit);
            }else if(pressed==hsin_slot){
                phase=Phase::ready_release;
                emit(Event{Kind::input,hsin_slot,"unexpected_ready"});
            }else if(!begin_field(now)){
                emit(Event{Kind::input,a_slot,"arm_unavailable"});
                finish(false,"arm_unavailable",now,emit);
            }else{
                started=true;field_begin=now;stage_due=now+stage_ms;phase=Phase::a1;
                emit(Event{Kind::input,a_slot,"start_a1"});
            }
            return;
        }
        if(phase==Phase::a1&&now>=stage_due){
            phase=Phase::switch_hsin;emit(Event{Kind::prompt,hsin_slot,nullptr});
        }else if(phase==Phase::hsin&&now>=stage_due){
            phase=Phase::switch_a2;emit(Event{Kind::prompt,a_slot,nullptr});
        }
        if(pressed&&(keys&both_slots)==both_slots){
            inputs(pressed,"ambiguous",emit);
            finish(false,"ambiguous_input",now,emit);return;
        }
        if(pressed){
            if((phase==Phase::a1||phase==Phase::switch_hsin)&&pressed==hsin_slot){
                const bool early=phase==Phase::a1;early_starts+=early;
                phase=Phase::hsin;stage_due=now+stage_ms;
                emit(Event{Kind::input,hsin_slot,"start_hsin",early});
            }else if((phase==Phase::hsin||phase==Phase::switch_a2)&&pressed==a_slot){
                const bool early=phase==Phase::hsin;early_starts+=early;
                phase=Phase::a2;stage_due=now+stage_ms;
                emit(Event{Kind::input,a_slot,"start_a2",early});
            }else if(pressed==((phase==Phase::hsin||phase==Phase::switch_a2)?hsin_slot:a_slot)){
                inputs(pressed,"repeat",emit);
            }else{
                inputs(pressed,"unexpected_slot",emit);
                finish(false,"unexpected_slot",now,emit);return;
            }
        }
        if(phase==Phase::a2&&now>=stage_due)finish(true,"third_segment_done",now,emit);
    }
    template<class Emit>
    void detach(Tick now,Emit emit){
        if(needs_input())finish(false,"device_detach",now,emit);
    }
    unsigned view(Tick now) const noexcept{
        const auto secs=[now](Tick end){return end>now?unsigned((end-now+999)/1000):0;};
        if(waiting())return pack(phase,secs(ready_until));
        if(running())return pack(phase,secs(stage_due));
        if((phase==Phase::complete||phase==Phase::incomplete)&&now<terminal_at+terminal_ms)
            return pack(phase,0);
        return 0;
    }
    static Phase view_phase(unsigned v) noexcept{return Phase(v&0xff);}
    static unsigned view_seconds(unsigned v) noexcept{return v>>8;}
private:
    static unsigned pack(Phase phase,unsigned seconds) noexcept{return (seconds<<8)|unsigned(phase);}
    template<class Emit>
    static void inputs(unsigned mask,const char *decision,Emit &emit){
        if(mask&a_slot)emit(Event{Kind::input,a_slot,decision});
        if(mask&hsin_slot)emit(Event{Kind::input,hsin_slot,decision});
    }
    template<class Emit>
    void finish(bool complete,const char *reason,Tick now,Emit &emit){
        phase=complete?Phase::complete:Phase::incomplete;
        terminal_at=now;
        emit(Event{Kind::end,0,reason});
    }
};
}

unsigned RtSlotSequenceOsdView() noexcept;
bool RtSlotSequencePresentDue() noexcept;
void RtSlotSequenceGamePresent();
