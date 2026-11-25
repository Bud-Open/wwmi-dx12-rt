#include "rt_cpu_window.h"
#include "rt_cpu_phase.hpp"
#include "rt_slot_sequence.hpp"
// Reuse the observer's session-scoped arm file. Begin at Present even if no
// character draw matches, so an absent world consumer is distinguishable.
#include <algorithm>
#include <string>
#include "rt_scene_arm.hpp"
void DrawRtSlotSequenceOsd(reshade::api::effect_runtime*);
namespace {
struct RtPassKey {
    uint64_t vs,pso;unsigned rts,depth,path;
    bool operator<(const RtPassKey& o)const{return std::tie(vs,pso,rts,depth,path)<std::tie(o.vs,o.pso,o.rts,o.depth,o.path);}
};
struct RtPassCount { unsigned candidates=0,subdraws=0,empty=0,author_t0=0; };
std::mutex field_mx;std::map<RtPassKey,RtPassCount> field_rows;
std::atomic<unsigned> field_state{}; // waiting, observing, draining, finished
ULONGLONG field_begin=0; // immutable once phase 1 is published
constexpr ULONGLONG kRtDrawWindowMs=rt_phase_cpu::bin_ms*rt_phase_cpu::bins;
struct RtMaterialMirrorBin { std::atomic<uint64_t> created{},reused{},stamped{},copied{},missed{}; };
constexpr unsigned kRtMaterialBinMs=rt_phase_cpu::bin_ms;
std::array<RtMaterialMirrorBin,kRtDrawWindowMs/kRtMaterialBinMs> rt_material_mirror{};
std::array<unsigned,rt_phase_cpu::bins> rt_phase_presents{};
std::atomic<unsigned> rt_material_mirror_writers{};
bool RtDrawWindowOpen(){
    return field_state.load(std::memory_order_acquire)==1&&GetTickCount64()-field_begin<kRtDrawWindowMs;
}
struct RtFieldDraw {
    bool active;RtPassKey key{};unsigned subdraws=0,empty=0,author_t0=0;
    explicit RtFieldDraw(bool candidate):active(candidate&&RtDrawWindowOpen()){}
    ~RtFieldDraw(){
        if(!active)return;
        std::lock_guard l(field_mx);if(field_state.load()!=1)return;
        auto& row=field_rows[key];++row.candidates;row.subdraws+=subdraws;row.empty+=empty;row.author_t0+=author_t0;
    }
};
#include "rt_cloth_observer.inc"
ULONGLONG field_end=0,field_next_write=0,field_last_present=0,field_max_present_gap=0;
constexpr ULONGLONG kRtCpuWindowMs=1000*rt_phase_cpu::detail_seconds,kRtDrawWriteMs=1000;
bool field_cpu_open=false,field_detached=false;
unsigned cloth_written=0,cloth_identified=0,cloth_candidates=0,cloth_native=0,cloth_private=0;
unsigned field_presents=0,cloth_native_choice_rejected=0,cloth_native_after_private=0;
std::mutex field_write_mx; // only Present/Detach write; draw rows never take this lock

void RtReportPhaseCpu(ULONGLONG now,bool detach)noexcept{
    rt_phase_cpu::close(detach);
    rt_phase_cpu::report("main",field_begin,rt_phase_cpu::descriptor_copy,rt_phase_cpu::count,
        detach||now>=field_end+2000,rt_phase_presents.data());
}
void RtWriteDrawWindow(ULONGLONG now,bool detach=false){
    std::lock_guard writer(field_write_mx);
    if(field_state.load(std::memory_order_acquire)==3){RtReportPhaseCpu(now,detach);return;}
    decltype(cloth_rows) rows;
    decltype(field_rows) passes;
    bool stop_cpu=false,write=false,finished=false,early=false;
    unsigned written=0,identified=0,candidates=0,native=0,private_draw=0,dropped=0,abandoned=0,inflight=0,presents=0,rejected_choice=0,after_private=0;
    ULONGLONG covered=0,end=0,last_present=0,max_present_gap=0;
    {
        std::lock_guard lock(field_mx);
        auto phase=field_state.load(std::memory_order_relaxed);
        if(phase==0||phase==3)return;
        now=(std::max)(now,field_begin);
        if(phase==1&&!detach&&now-field_begin<kRtDrawWindowMs){
            ++field_presents;
            ++rt_phase_presents[(now-field_begin)/rt_phase_cpu::bin_ms];
            field_max_present_gap=(std::max)(field_max_present_gap,now-field_last_present);
            field_last_present=now;
        }
        if(field_cpu_open&&(detach||now-field_begin>=kRtCpuWindowMs)){field_cpu_open=false;stop_cpu=true;}
        if(phase==1&&(detach||now-field_begin>=kRtDrawWindowMs)){
            field_end=(std::min)(now,field_begin+kRtDrawWindowMs);
            field_detached=detach&&field_end<field_begin+kRtDrawWindowMs;
            rt_phase_cpu::close(detach);
            field_state.store(2);phase=2;
        }
        finished=phase==2&&(!cloth_inflight||detach);
        write=finished||now>=field_next_write;
        if(write){
            rows.swap(cloth_rows);field_next_write=now+kRtDrawWriteMs;
            for(const auto& row:rows){
                ++cloth_written;cloth_identified+=row.identified!=0;cloth_candidates+=row.identified==0;
                cloth_private+=row.did!=0;cloth_native+=row.did==0;
                if(row.identified&&!row.did){
                    cloth_native_choice_rejected+=row.native_choice_rejected||std::any_of(row.sections.begin(),row.sections.end(),[](const auto &s){return s.native_choice_rejected!=0;});
                    cloth_native_after_private+=row.author_disposition==WWMI_RT_AUTHOR_PRIVATE||
                        std::any_of(row.sections.begin(),row.sections.end(),[](const auto &s){return s.author_disposition==WWMI_RT_AUTHOR_PRIVATE;});
                }
            }
            written=cloth_written;identified=cloth_identified;candidates=cloth_candidates;
            native=cloth_native;private_draw=cloth_private;dropped=cloth_dropped;abandoned=cloth_abandoned;inflight=cloth_inflight;
            end=field_end?field_end:now;covered=end-field_begin;
            presents=field_presents;last_present=field_last_present;max_present_gap=field_max_present_gap;
            rejected_choice=cloth_native_choice_rejected;after_private=cloth_native_after_private;
        }
        if(finished){early=field_detached;passes.swap(field_rows);field_state.store(3);}
    }
    if(write){
        std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.seq<b.seq;});
        RtClothLog(rows);
        char line[640];
        std::snprintf(line,sizeof(line),"RT-CLOTH %s tick_ms=%llu covered_ms=%llu requested_ms=%llu written=%u identified=%u candidate_unidentified=%u native_retained=%u private_or_hidden=%u dropped_backlog=%u excluded_other_identity=%u %s=%u reason=%s observed_present_callbacks=%u last_present_tick_ms=%llu max_callback_gap_ms=%llu native_choice_rejected=%u native_with_observed_private=%u",
            finished?"end":"checkpoint",(unsigned long long)end,(unsigned long long)covered,(unsigned long long)kRtDrawWindowMs,
            written,identified,candidates,native,private_draw,dropped,abandoned,finished?"lost_inflight":"pending",inflight,
            finished?(early?"device_detach":inflight?"detach_during_drain":"deadline"):"window_open",
            presents,(unsigned long long)last_present,(unsigned long long)max_present_gap,rejected_choice,after_private);
        reshade::log::message(reshade::log::level::info,line);
    }
    if(stop_cpu)RtCpuWindowEnd();
    if(!finished)return;
    RtReportPhaseCpu(now,detach);
    const auto mirror_pending=rt_material_mirror_writers.load(std::memory_order_acquire);
    for(unsigned i=0;i<rt_material_mirror.size();++i){
        const auto& b=rt_material_mirror[i];
        const auto created=b.created.load(),reused=b.reused.load(),stamped=b.stamped.load(),copied=b.copied.load(),missed=b.missed.load();
        if(!(created||reused||stamped||copied||missed))continue;
        char msg[336];std::snprintf(msg,sizeof(msg),
            "RT-MATERIAL-MIRROR bin=%u begin_ms=%llu ordinary_created=%llu ordinary_reused=%llu stamped=%llu table_copies=%llu unresolved=%llu pending_writers=%u",
            i,(unsigned long long)(field_begin+i*kRtMaterialBinMs),(unsigned long long)created,(unsigned long long)reused,
            (unsigned long long)stamped,(unsigned long long)copied,(unsigned long long)missed,mirror_pending);
        reshade::log::message(reshade::log::level::info,msg);
    }
    unsigned total=0;for(auto& [k,row]:passes){
        total+=row.candidates;char msg[224];std::snprintf(msg,sizeof(msg),"RT-VISIBLE pass vs=%llx pso=%llx rts=%u depth=%u path=%u n=%u recorded_subdraws=%u empty_selections=%u sampled_author_t0=%u",
           (unsigned long long)k.vs,(unsigned long long)k.pso,k.rts,k.depth,k.path,row.candidates,row.subdraws,row.empty,row.author_t0);
        reshade::log::message(reshade::log::level::info,msg);
    }
    char msg[144];std::snprintf(msg,sizeof(msg),"RT-VISIBLE end: elapsed_ms=%llu torso_candidates=%u",(unsigned long long)covered,total);
    reshade::log::message(reshade::log::level::info,msg);
}
}
void RtCountRecordedMaterial(unsigned created,unsigned reused,unsigned stamped,unsigned copied,unsigned missed)noexcept{
    if(field_state.load(std::memory_order_acquire)!=1)return;
    rt_material_mirror_writers.fetch_add(1,std::memory_order_acquire);
    const auto bin=(GetTickCount64()-field_begin)/kRtMaterialBinMs;
    if(field_state.load(std::memory_order_acquire)==1&&bin<rt_material_mirror.size()){
        auto& b=rt_material_mirror[bin];
        b.created.fetch_add(created,std::memory_order_relaxed);b.reused.fetch_add(reused,std::memory_order_relaxed);
        b.stamped.fetch_add(stamped,std::memory_order_relaxed);b.copied.fetch_add(copied,std::memory_order_relaxed);
        b.missed.fetch_add(missed,std::memory_order_relaxed);
    }
    rt_material_mirror_writers.fetch_sub(1,std::memory_order_release);
}
namespace {
bool RtStartDrawWindow(ULONGLONG begin){
    {
        std::lock_guard lock(field_mx);
        const auto device=wwmi_rt_pack_frame_device.load(std::memory_order_acquire);
        if(field_state.load()!=0||!device)return false;
        field_begin=begin;field_last_present=begin;field_presents=1;field_next_write=begin+kRtDrawWriteMs;
        rt_phase_presents[0]=1;
        RtCpuWindowBegin(device);
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
        rt_phase_cpu::arm(WWMI_RtCpuWindowDeadline_v1(device),frequency.QuadPart);
        field_cpu_open=true;field_state.store(1);
    }
    char line[256];
    std::snprintf(line,sizeof(line),"RT-VISIBLE start: draw_ms=%llu cpu_ms=%llu begin_tick_ms=%llu identified=hsin_all candidate_hints=2,3,4,7 material_detail=3,7",
        (unsigned long long)kRtDrawWindowMs,(unsigned long long)kRtCpuWindowMs,(unsigned long long)begin);
    reshade::log::message(reshade::log::level::info,line);
    return true;
}

enum class SlotMode {unclaimed,live,spent};
std::atomic<SlotMode> field_slot_mode{SlotMode::unclaimed};
std::atomic<unsigned> field_slot_view{};
std::atomic<ULONGLONG> field_slot_probe_after{};
std::mutex field_slot_mx; // serialize the Present-owned sequence with device detach
rt_slot_sequence::Sequence field_slots;
std::array<char,256> field_slot_session{};
unsigned field_slot_frame=0;
rt_slot_sequence::Tick field_slot_expiry_utc=0;

void RtSlotLog(ULONGLONG now,const char *event,const char *detail){
    const auto offset=field_slots.started?static_cast<long long>(now-field_slots.field_begin):-1ll;
    char line[640];
    std::snprintf(line,sizeof(line),
        "RT-SLOT-SEQUENCE event=%s session=%s tick_ms=%llu frame=%u field_begin_ms=%llu offset_ms=%lld phase=%s%s",
        event,field_slot_session.data(),(unsigned long long)now,field_slot_frame,
        (unsigned long long)field_slots.field_begin,offset,rt_slot_sequence::phase_name(field_slots.phase),detail);
    reshade::log::message(reshade::log::level::info,line);
}
auto RtSlotEvents(ULONGLONG now){
    return [now](rt_slot_sequence::Event event){
        using K=rt_slot_sequence::Kind;
        char detail[144];
        switch(event.kind){
        case K::input:
            std::snprintf(detail,sizeof(detail)," slot=%u decision=%s early=%u",event.slot,event.detail,unsigned(event.early));
            RtSlotLog(now,"input",detail);break;
        case K::prompt:
            std::snprintf(detail,sizeof(detail)," slot=%u",event.slot);
            RtSlotLog(now,"prompt",detail);break;
        case K::end:
            std::snprintf(detail,sizeof(detail)," status=%s reason=%s early_starts=%u",
                field_slots.phase==rt_slot_sequence::Phase::complete?"complete":"incomplete",event.detail,field_slots.early_starts);
            RtSlotLog(now,"end",detail);break;
        }
    };
}
void RtPublishSlotOsd(unsigned view){
    const auto previous=field_slot_view.exchange(view,std::memory_order_release);
    if(view&&!previous)reshade::register_overlay("OSD",DrawRtSlotSequenceOsd);
    else if(!view&&previous)reshade::unregister_overlay("OSD",DrawRtSlotSequenceOsd);
}
rt_slot_sequence::Tick RtUnixMs(){
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value{};
    value.LowPart=ft.dwLowDateTime;value.HighPart=ft.dwHighDateTime;
    return value.QuadPart/10000-11644473600000ULL; // FILETIME epoch to Unix ms
}
void RtReadySlotSequence(std::string_view session,rt_slot_sequence::Tick expiry_utc){
    std::lock_guard lock(field_slot_mx);
    if(field_slot_mode.load()!=SlotMode::unclaimed||field_state.load()!=0||
       !wwmi_rt_pack_frame_device.load(std::memory_order_acquire))return;
    const auto now=GetTickCount64();
    const auto remaining=rt_slot_sequence::request_remaining(expiry_utc,RtUnixMs());
    if(!remaining)return;
    std::copy(session.begin(),session.end(),field_slot_session.begin());
    field_slot_frame=g_frame_id;
    field_slot_expiry_utc=expiry_utc;
    field_slots.enter_ready(now,remaining);
    char detail[176];
    std::snprintf(detail,sizeof(detail)," ready_deadline_ms=%llu draw_ms=%llu stage_ms=%llu request_expires_unix_ms=%llu",
        (unsigned long long)field_slots.ready_until,(unsigned long long)kRtDrawWindowMs,
        (unsigned long long)rt_slot_sequence::stage_ms,(unsigned long long)expiry_utc);
    RtSlotLog(now,"ready",detail);
    RtPublishSlotOsd(field_slots.view(now));
    field_slot_mode.store(SlotMode::live,std::memory_order_release);
}
ULONGLONG RtPollSlotSequence(){
    std::lock_guard lock(field_slot_mx);
    if(field_slot_mode.load()!=SlotMode::live)return GetTickCount64();
    field_slot_frame=g_frame_id;
    const bool watch=field_slots.needs_input();
    bool foreground=false;unsigned keys=0;
    if(watch){
        DWORD pid=0;
        if(HWND fg=GetForegroundWindow())GetWindowThreadProcessId(fg,&pid);
        foreground=pid==GetCurrentProcessId();
        if(foreground){
            if(GetAsyncKeyState(rt_slot_sequence::a_key)&0x8000)keys|=rt_slot_sequence::a_slot;
            if(GetAsyncKeyState(rt_slot_sequence::hsin_key)&0x8000)keys|=rt_slot_sequence::hsin_slot;
            if(GetAsyncKeyState(rt_slot_sequence::off_key)&0x8000)keys|=rt_slot_sequence::off_bit;
        }
    }
    const auto now=GetTickCount64();
    if(watch)
        field_slots.present(now,kRtDrawWindowMs,foreground,keys,RtStartDrawWindow,RtSlotEvents(now),
            !field_slots.waiting()||RtUnixMs()<field_slot_expiry_utc);
    const auto view=field_slots.view(now);
    RtPublishSlotOsd(view);
    if(!view)field_slot_mode.store(SlotMode::spent,std::memory_order_release);
    return now;
}
ULONGLONG RtStopSlotSequence(){
    std::lock_guard lock(field_slot_mx);
    const auto now=GetTickCount64();
    if(field_slot_mode.load()!=SlotMode::live)return now;
    field_slots.detach(now,RtSlotEvents(now));
    RtPublishSlotOsd(0);
    field_slot_mode.store(SlotMode::spent,std::memory_order_release);
    return now;
}
template<std::size_t N>
std::string_view RtReadArmRequest(const std::wstring &path,char (&content)[N]){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return {};
    DWORD bytes=0;
    const bool read=ReadFile(f,content,N,&bytes,nullptr)!=FALSE;
    CloseHandle(f);
    return read?std::string_view(content,bytes):std::string_view{};
}
const std::array<std::wstring,2> &RtArmRequests(){
    static const auto paths=[](){
        std::array<std::wstring,2> requests;
        wchar_t path[1024]{};const auto n=GetEnvironmentVariableW(L"WWMI_RT_CAPTURE_DIR",path,1024);
        if(n&&n<1024){
            requests[0]=std::wstring(path)+L"\\wwmi-rt-scene-arm.request";
            requests[1]=std::wstring(path)+L"\\wwmi-rt-slot-sequence.request";
        }
        return requests;
    }();
    return paths;
}
std::string_view RtArmSession(char (&token)[256]){
    const auto n=GetEnvironmentVariableA("WWMI_RT_CAPTURE_SESSION",token,sizeof(token));
    return n&&n<sizeof(token)?std::string_view(token,n):std::string_view{};
}
}
unsigned RtSlotSequenceOsdView() noexcept{return field_slot_view.load(std::memory_order_acquire);}
static void RtVisibleFieldStop(){
    RtWriteDrawWindow(RtStopSlotSequence(),true);
}
bool RtVisibleMaterialWindow(){return RtDrawWindowOpen();}
bool RtSlotSequencePresentDue() noexcept{
    const auto mode=field_slot_mode.load(std::memory_order_acquire);
    const auto phase=field_state.load(std::memory_order_acquire);
    if(mode==SlotMode::unclaimed)
        return phase==0&&GetTickCount64()>=field_slot_probe_after.load(std::memory_order_relaxed);
    return mode==SlotMode::live||phase==1||phase==2||(phase==3&&rt_phase_cpu::waiting_report());
}
void RtSlotSequenceGamePresent(){
    const auto phase=field_state.load(std::memory_order_relaxed);
    const auto mode=field_slot_mode.load(std::memory_order_acquire);
    if(mode==SlotMode::unclaimed){
        const auto now=GetTickCount64();
        if(phase!=0||now<field_slot_probe_after.load(std::memory_order_relaxed))return;
        field_slot_probe_after.store(now+250,std::memory_order_relaxed);
        char data[320]{},token[256]{};
        const auto session=RtArmSession(token);
        if(session.empty())return;
        const auto &request=RtArmRequests()[1];
        if(request.empty())return;
        const auto opt_in=RtReadArmRequest(request,data);
        if(opt_in.empty())return;
        if(const auto expires=rt_slot_sequence::request_expiry(opt_in,session))RtReadySlotSequence(session,expires);
        return; // readiness cannot accept an input on the same Present
    }
    const auto now=mode==SlotMode::live?RtPollSlotSequence():GetTickCount64();
    if(phase==3){
        if(rt_phase_cpu::waiting_report())RtWriteDrawWindow(now);
        return;
    }
    // A first-slot edge already counts the arming Present; don't count it twice.
    if(phase!=0)RtWriteDrawWindow(now);
}
void RtVisibleFieldPresent(){
    // Legacy token-only requests still run on the original engine-step
    // cadence (including FG FrameEnd). An adopted sequence owns its window.
    if(field_slot_mode.load(std::memory_order_acquire)!=SlotMode::unclaimed)return;
    static ULONGLONG check=0;
    const auto phase=field_state.load(std::memory_order_relaxed);
    if(phase==3){
        if(rt_phase_cpu::waiting_report())RtWriteDrawWindow(GetTickCount64());
        return;
    }
    const auto now=GetTickCount64();
    if(phase!=0){RtWriteDrawWindow(now);return;}
    if(now<check)return;check=now+250;
    char legacy[256]{},token[256]{};
    const auto session=RtArmSession(token);
    if(session.empty())return;
    const auto &requests=RtArmRequests();
    if(requests[0].empty())return;
    const auto direct=RtReadArmRequest(requests[0],legacy);
    if(direct.empty())return;
    if(!rt_scene::request_matches(direct,session))return;
    char slot[320]{};
    const auto expires=rt_slot_sequence::request_expiry(RtReadArmRequest(requests[1],slot),session);
    if(expires&&rt_slot_sequence::request_remaining(expires,RtUnixMs()))return;
    RtStartDrawWindow(GetTickCount64());
}
