#pragma once

#include <array>
#include <optional>

enum class RtAuxInput : uint8_t { none, unobserved, direct, constant, match, missing, ineligible };
struct RtAuxProofNote {
    RtAuxInput input=RtAuxInput::none;
    uint32_t guid=0, hash=0;
    RtAuxSource source{};
    uint64_t epoch=0, proof=0;
    int aux_register=-1;
    uint32_t mask=0,native_base=0;
    uint64_t heap=0;
    uint32_t slot=0;
    const char* origin="register-not-table";
};

// Attach to the actual existing LocalVF read without changing its result,
// exported signature, or RtWorldSurface layout.
inline thread_local RtAuxProofNote* rt_aux_note=nullptr;
struct RtAuxNoteScope {
    RtAuxProofNote* const old=rt_aux_note;
    explicit RtAuxNoteScope(RtAuxProofNote& value){rt_aux_note=&value;}
    ~RtAuxNoteScope(){rt_aux_note=old;}
};
struct RtAuxDrawReceipt {
    uint64_t vs, ps, pso, recording, ticket, frame;
    uint32_t component, mrt, first, count;
    int32_t base;
    const char* route;
    const char* reason;
    int32_t mod=-1;
    uint64_t plan=0;
    bool section_violation=false, caller_cancelled=false, draw_violation=false;
    uint32_t omitted_sections=0;
};
void RtFinishAuxProofNote(const RtAuxProofNote&,const RtAuxDrawReceipt&);

// A native draw may match more than one authored section. Its cancellation
// is only known after all sections, so keep their own sources and decisions
// separately until the real aggregate outcome exists.
struct RtAuxSectionNotes {
    struct Entry {
        RtAuxProofNote note;
        uint64_t ps,plan;
        int32_t mod;
        const char* route;
        const char* reason;
        bool violation;
    };
    std::array<std::optional<Entry>,8> entries{};
    uint32_t size=0,omitted=0;
    void remember(const RtAuxProofNote& note,uint64_t ps,uint64_t plan,int32_t mod,
                  const char* route,const char* reason,bool violation){
        if(note.input==RtAuxInput::none)return;
        if(size==entries.size()){++omitted;return;}
        entries[size++].emplace(Entry{note,ps,plan,mod,route,reason,violation});
    }
    void finish(RtAuxDrawReceipt draw,bool caller_cancelled,bool draw_violation)const{
        draw.caller_cancelled=caller_cancelled;
        draw.draw_violation=draw_violation;
        draw.omitted_sections=omitted;
        for(uint32_t i=0;i<size;++i){
            const auto& e=*entries[i];
            draw.ps=e.ps;draw.plan=e.plan;draw.mod=e.mod;draw.route=e.route;
            draw.reason=e.reason;draw.section_violation=e.violation;
            RtFinishAuxProofNote(e.note,draw);
        }
    }
};
