// This TU is part of the diagnostic host, never injected into a running game.
// Interpose only on the DLSS-G plugin gateway. Do not hook sl.interposer's DXGI
// exports or issue a new state query: that would consume the game's counter.
#include <dxgi1_2.h>
#include <atomic>
#include <mutex>
#include <cstring>
#include "upstream-v2.10.0/sl_dlss_g.h"
namespace fg_observe {
using Present = HRESULT(IDXGISwapChain*, UINT, UINT, bool&);
using Present1 = HRESULT(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*, bool&);
std::atomic<Present*> original_present{};
std::atomic<Present1*> original_present1{};
std::atomic<PFun_slDLSSGGetState*> original_get{};
std::atomic<PFun_slDLSSGSetOptions*> original_set{};
std::atomic<uint64_t> calls{}, errors{}, chain_changes{};
std::atomic<IDXGISwapChain*> chain{};
void count_present(IDXGISwapChain* sc,UINT flags,HRESULT hr) {
    if(flags & DXGI_PRESENT_TEST) return;
    if(chain.load(std::memory_order_relaxed)!=sc && chain.exchange(sc)!=sc) ++chain_changes;
    ++calls;
    if(FAILED(hr)) ++errors;
}
HRESULT on_present(IDXGISwapChain* sc,UINT sync,UINT flags,bool& skip) {
    auto hr=original_present.load()(sc,sync,flags,skip); count_present(sc,flags,hr); return hr;
}
HRESULT on_present1(IDXGISwapChain* sc,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* pp,bool& skip) {
    auto hr=original_present1.load()(sc,sync,flags,pp,skip); count_present(sc,flags,hr); return hr;
}
// Samples end on the existing game's GetState call. Report differences between
// successive successful query boundaries; skip first query and mixed intervals.
struct Window {
    bool valid=false;
    unsigned viewport=0,status=0;
    uint64_t start_ms=0, start_calls=0,start_changes=0,start_errors=0,displayed=0,queries=0;
};
Window window;
std::mutex window_lock;
sl::Result on_get(const sl::ViewportHandle& vp,sl::DLSSGState& state,const sl::DLSSGOptions* options) {
    const auto result=original_get.load()(vp,state,options);
    const auto now=GetTickCount64(), app=calls.load(), changes=chain_changes.load(), err=errors.load();
    const unsigned viewport=vp;
    std::lock_guard lock(window_lock);
    if(result!=sl::Result::eOk){window.valid=false;return result;}
    const unsigned status=unsigned(state.status);
    if(!window.valid || window.viewport!=viewport || window.status!=status ||
       window.start_changes!=changes || window.start_errors!=err) {
        reshade::log::message(reshade::log::level::info,
            "WWMI-FG boundary viewport=%u status=0x%x app_callbacks=%llu chain=%p",viewport,status,app,chain.load());
        window={true,viewport,status,now,app,changes,err,0,0};return result;
    }
    window.displayed+=state.numFramesActuallyPresented; ++window.queries;
    if(now-window.start_ms>=3000) {
        reshade::log::message(reshade::log::level::info,
            "WWMI-FG window viewport=%u status=0x%x elapsed_ms=%llu app_present_callbacks=%llu frames_presented=%llu existing_queries=%llu chain=%p",
            viewport,status,now-window.start_ms,app-window.start_calls,window.displayed,window.queries,chain.load());
        window={true,viewport,status,now,app,changes,err,0,0};
    }
    return result;
}
sl::Result on_set(const sl::ViewportHandle& vp,const sl::DLSSGOptions& opt) {
    const auto result=original_set.load()(vp,opt);
    // Games can repeat identical options every frame. Only changes matter.
    static unsigned last_v=~0u,last_m=~0u,last_n=~0u,last_f=~0u,last_rc=~0u;
    const unsigned v=vp,m=unsigned(opt.mode),n=opt.numFramesToGenerate,f=unsigned(opt.flags),rc=unsigned(result);
    std::lock_guard lock(window_lock);
    if(v!=last_v||m!=last_m||n!=last_n||f!=last_f||rc!=last_rc){
        reshade::log::message(reshade::log::level::info,"WWMI-FG options viewport=%u mode=%u requested_generated=%u flags=0x%x result=%u",v,m,n,f,rc);
        last_v=v;last_m=m;last_n=n;last_f=f;last_rc=rc;window.valid=false;
    }
    return result;
}
template<class T> void* wrap(void* p,std::atomic<T*>& slot,T* fn) {
    slot.store(reinterpret_cast<T*>(p));return reinterpret_cast<void*>(fn);
}
}
// Count below Streamline only if the exact proxy that received this Present is
// the native/base swap chain passed into the game's DLSS-G pre-present hook.
// A zero with no matching chain is not evidence of zero generated frames.
void wwmi_fg_dxgi_present(IDXGISwapChain* proxy,IDXGISwapChain* underlying,UINT flags,HRESULT hr) {
    using namespace fg_observe;
    if((flags&DXGI_PRESENT_TEST) || hr!=S_OK)return;
    IDXGISwapChain* sc=chain.load(std::memory_order_relaxed);
    if(sc!=proxy && sc!=underlying)return;
    const auto now=GetTickCount64();
    static std::mutex mutex; static uint64_t t0=0,a0=0,changes0=0,n=0;
    std::lock_guard lock(mutex);
    const auto ch=chain_changes.load(),a=calls.load();
    if(t0==0 || ch!=changes0){t0=now;a0=a;changes0=ch;n=0;return;}
    ++n;
    if(now-t0>=3000){
        reshade::log::message(reshade::log::level::info,
            "WWMI-FG DXGI elapsed_ms=%llu app_present_callbacks=%llu lower_present_ok=%llu chain=%p",
            now-t0,a-a0,n,sc);
        t0=now;a0=a;n=0;
    }
}
extern "C" __declspec(dllexport) void* slGetPluginFunction(const char* name) {
    static const auto real=reshade::hooks::call(&slGetPluginFunction);
    auto p=real(name);
    if(!p||!name)return p;
    using namespace fg_observe;
    if(!std::strcmp(name,"slDLSSGGetState"))return wrap(p,original_get,on_get);
    if(!std::strcmp(name,"slDLSSGSetOptions"))return wrap(p,original_set,on_set);
    if(!std::strcmp(name,"slHookPresent"))return wrap(p,original_present,on_present);
    if(!std::strcmp(name,"slHookPresent1"))return wrap(p,original_present1,on_present1);
    return p;
}
