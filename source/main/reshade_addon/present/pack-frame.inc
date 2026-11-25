#include "rt_raster_gate.h"
#include "rt_slot_sequence.hpp"
#include "rt_hybrid_rfx_material.hpp"
#include "../mods/skeleton.h"
#include "../../src/mod_loader.h"
static bool GameKeyDown(int vk);
void RtBeginCatalogReload() noexcept;
void RtEndCatalogReload() noexcept;
void RtResetGateCounts() noexcept;
std::atomic<uint64_t> wwmi_rt_pack_frame_device{0};
extern "C" __declspec(dllexport) std::uint32_t __cdecl WWMI_RtSetPackFrameDevice_v1(
    std::uint64_t native_device, std::uint32_t enable) noexcept
{
    if(!native_device) return 0;
    if(!enable) {
        if(!wwmi_rt_pack_frame_device.compare_exchange_strong(native_device,0)) return 0;
        RtRasterDetach();return 1;
    }
    if(native_device!=reinterpret_cast<uint64_t>(g_d3d12_device)) return 0;
    uint64_t empty=0;
    if(!wwmi_rt_pack_frame_device.compare_exchange_strong(empty,native_device))return empty==native_device;
    if(RtRasterAttach())return 1;
    wwmi_rt_pack_frame_device.store(0);return 0;
}
static bool RtPackFrameActive() {
    return g_rt_blocked && !g_faulted && g_d3d12_device &&
        wwmi_rt_pack_frame_device.load()==reinterpret_cast<uint64_t>(g_d3d12_device);
}
bool WwmiRtPackActive() noexcept {
    return RtPackFrameActive();
}
static bool RtPackDirectSwapchain(command_queue* queue,swapchain* sc) {
    if(!RtPackFrameActive() || !sc || sc->get_device()!=g_reshade_device ||
        queue->get_device()!=g_reshade_device) return false;
    auto* q=reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    return q->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT;
}
static void RtWaitQueueIdle(ID3D12CommandQueue* q) noexcept {
    if(!q || !g_d3d12_device) return;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if(FAILED(g_d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) return;
    if(FAILED(q->Signal(fence.Get(),1))) return;
    if(fence->GetCompletedValue()<1) {
        if(HANDLE ev=CreateEventW(nullptr,FALSE,FALSE,nullptr)) {
            if(SUCCEEDED(fence->SetEventOnCompletion(1,ev)))
                WaitForSingleObject(ev,2000);
            CloseHandle(ev);
        }
    }
}
static void RtFlushAndClearModCatalog(command_queue* queue) {
    auto* q12=reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    RtWaitQueueIdle(q12);
    auto* exec_q=reinterpret_cast<ID3D12CommandQueue*>(const_cast<void*>(g_claim_exec_queue.load(std::memory_order_relaxed)));
    if(exec_q && exec_q!=q12) RtWaitQueueIdle(exec_q);
    ModLoader::Reload();
    ModLoader::Tick();
    g_geomods.clear();
    g_ns_to_mod.clear();
    g_merged_comps.clear();
    g_first_to_comp.clear();
    g_first_to_shared.clear();
    g_ibhash_slot.clear();
    g_mod_index_end.clear();
    g_geom_loaded=false;
    g_texovr_ready=false;
    RtResetGateCounts();
}
namespace PortApi {
bool RtPackConfigReady();
void RequestReload();
bool AliasEmptyBufferResource(const wchar_t* dst_fq, const wchar_t* src_fq);
}
extern bool WwmiIsHybridBaseSkinMode() noexcept;
static bool RtPackBaseSkinMode() {
    return WwmiIsHybridBaseSkinMode();
}
static void RtAliasBaseSkinPositionTargets(unsigned gen) {
    if(!RtPackBaseSkinMode()) return;
    static unsigned s_aliased_gen=~0u;
    if(s_aliased_gen==gen || g_geomods.empty()) return;
    s_aliased_gen=gen;

    auto trim=[](std::wstring s) {
        while(!s.empty() && (s.front()==L' ' || s.front()==L'\t')) s.erase(s.begin());
        while(!s.empty() && (s.back()==L' ' || s.back()==L'\t' || s.back()==L'\r' || s.back()==L'\n')) s.pop_back();
        return s;
    };
    auto lower=[](std::wstring s) {
        for(wchar_t& ch : s) ch=(wchar_t)towlower(ch);
        return s;
    };

    for(size_t mi=0; mi<g_geomods.size(); ++mi) {
        const GeoMod& gm=g_geomods[mi];
        if(gm.aux_skip_only) continue;
        const WwmiMod::Resource* pos=WwmiMod::Find(gm.mod,L"ResourcePositionBuffer");
        if(!pos || pos->fq.empty()) continue;

        struct ScanCtx {
            std::set<std::wstring> bypass_names;
            std::set<std::wstring> vb0_names;
        } scan{};
        scan.bypass_names.insert(L"resourcebypassvb0");

        if(!gm.wcl_ns.empty()) {
            PortApi::ForEachSectionInNs(gm.wcl_ns.c_str(), [](const wchar_t* fq_sec, void* u) {
                PortApi::ForEachSectionKey(fq_sec, [](const wchar_t* key, const wchar_t* val, void* u2) {
                    if(!key || !val) return;
                    auto* sc=static_cast<ScanCtx*>(u2);
                    std::wstring k(key), v(val);
                    while(!k.empty() && (k.front()==L' ' || k.front()==L'\t')) k.erase(k.begin());
                    while(!k.empty() && (k.back()==L' ' || k.back()==L'\t')) k.pop_back();
                    while(!v.empty() && (v.front()==L' ' || v.front()==L'\t')) v.erase(v.begin());
                    while(!v.empty() && (v.back()==L' ' || v.back()==L'\t' || v.back()==L'\r' || v.back()==L'\n')) v.pop_back();
                    for(wchar_t& ch : k) ch=(wchar_t)towlower(ch);
                    std::wstring lv=v;
                    for(wchar_t& ch : lv) ch=(wchar_t)towlower(ch);
                    if(lv==L"vb0" || lv==L"ref vb0") {
                        if(k.rfind(L"resource",0)==0) sc->bypass_names.insert(k);
                        return;
                    }
                    if(k==L"vb0") {
                        std::wstring target=lv;
                        if(target.rfind(L"ref ",0)==0) {
                            target=target.substr(4);
                            while(!target.empty() && (target.front()==L' ' || target.front()==L'\t')) target.erase(target.begin());
                        }
                        if(target.rfind(L"resource",0)==0) sc->vb0_names.insert(target);
                    }
                }, u);
            }, &scan);
        }
        scan.vb0_names.insert(L"resourceshapekeyedposition");

        for(const std::wstring& target_lower : scan.vb0_names) {
            if(scan.bypass_names.count(target_lower)) continue;
            const WwmiMod::Resource* dst=nullptr;
            for(const WwmiMod::Resource& r : gm.mod.resources) {
                if(lower(trim(r.name))==target_lower) { dst=&r; break; }
            }
            if(!dst || !dst->filename.empty() || dst->fq.empty() || _wcsicmp(dst->fq.c_str(),pos->fq.c_str())==0)
                continue;
            if(PortApi::AliasEmptyBufferResource(dst->fq.c_str(),pos->fq.c_str())) {
                logger::probe("WWMI-LOAD phase=base-skin-pos-alias generation=%u mod=%zu guid=%u dst='%ls' -> src='%ls'",
                    gen,mi+1,gm.mod.object_guid,dst->name.c_str(),pos->name.c_str());
            }
        }
    }
}
static void RtPackWorldFrame(command_queue* queue,swapchain* sc,bool at_present) {
    std::lock_guard frame_lock(EngineFrameMutex());
    if(!RtPackDirectSwapchain(queue,sc))return;
    static bool s_f10=false;
    const bool f10=GameKeyDown(VK_F10);
    const bool f10_edge=f10 && !s_f10;
    s_f10=f10;
    const bool ui_reload=g_reload_requested.exchange(false);
    struct RtReloadGuard {
        bool active=false;
        void enter() noexcept { if(!active){ RtBeginCatalogReload(); active=true; } }
        ~RtReloadGuard() { if(active) RtEndCatalogReload(); }
    } reload_guard;
    if(f10_edge || ui_reload) {
        logger::probe("WWMI-LOAD phase=hot-reload-begin trigger=%s frame=%u existing_mods=%zu",
            f10_edge?"VK_F10":"overlay",g_frame_id,g_geomods.size());
        reload_guard.enter();
        RtFlushAndClearModCatalog(queue);
        PortApi::RequestReload();
    }
    RtVisibleFieldPresent();
    PortProbeTick(queue,sc);
    WWMI_RtModDrainDetection_v1();
    EngineFrameTick(queue,sc,at_present);
    if(PortApi::RtPackConfigReady()) {
        static unsigned prepared_generation=~0u;
        const unsigned gen=PortApi::LoadGeneration();
        if(prepared_generation!=gen) {
            if(!g_geomods.empty()) {
                reload_guard.enter();
                RtFlushAndClearModCatalog(queue);
            }
            logger::probe("WWMI-LOAD phase=config-ready generation=%u geometry_cached=%u existing_mods=%zu",
                gen,unsigned(g_geom_loaded),g_geomods.size());
            if(g_geomods.empty()) g_geom_loaded=false;
            prepared_generation=gen;
        }
        ModTexturesTick();
        RtPublishPendingHybridNamedTextures();
        LoadGeometryMods();
        RtAliasBaseSkinPositionTargets(gen);
        WWMI_RtModPublish_v1();
    }
    DrainTexReadback();
    IbHash::DrainQueue();
    ++g_frame_id;
}
