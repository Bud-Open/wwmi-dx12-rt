#include "observation/binding_client/wwmi_binding_client.hpp"
// The addon's shared include block. Every hook module compiled into this addon sees the world through
// addon_common.h, so an extracted file is a move rather than a re-derivation of which header declares
// what. It is .cpp-only: it ends with `using namespace reshade::api`, which no header may impose.
#include "addon_common.h"
#include "descriptor_publish_main.hpp"
#include "instrumentation/uav_observe.h"
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include "migoto/dump_preview.h"
#include "mods/claim_state.h"
#include "mods/skeleton.h"
#include "hooks/pipeline_hooks.h"
#include "hooks/cmd_bind_hooks.h"
#include "hooks/resource_tracker.h"
#include "dispatch/dispatch_parts.h"
#include "present/present_parts.h"
#include "instrumentation/device_watch.h" // DredWatchdogStop
#include "rabbitfx/pso_blend.h"           // RfxPatchWorkerStop
#include "textures/texture_module.h"      // g_tex_lod_worker
#include "textures/tex_dhash_table.h"
#include "textures/tex_crawl.h"
#include "textures/tex_defer.h"
#include "textures/tex_gc.h"
#include "textures/tex_replace.h"
#include "migoto/dx12_wrap.h" // BackBufferForget
#include "migoto/mod_ui_shield.h"
#include "migoto/port_toggles.h" // g_buffers_in_vram
#include "instrumentation/removal_pin.h"
#include "instrumentation/hot_path.h"
#include "draw/draw_hook.h"
#include "instrumentation/draw_dump_publish.h" // X27ResolveVsCbs -- the game's bone CBs per game draw (X23)

bool g_is_dx12 = false;
bool g_tex_struct_fallback = true;
HMODULE g_wwmi_host_module = nullptr;
std::atomic<bool> g_rt_user_disabled{ false };
static std::atomic<int> g_hybrid_skin_mode{ -1 }; // -1 = uninitialized, 0 = exact, 1 = base

bool WwmiIsHybridBaseSkinMode() noexcept
{
    const int cached = g_hybrid_skin_mode.load(std::memory_order_acquire);
    if (cached >= 0) return cached != 0;
    char arg[32]{};
    const DWORD n = GetEnvironmentVariableA("WWMI_HYBRID_RASTER_MODE", arg, (DWORD)std::size(arg));
    if (n > 0 && n < std::size(arg))
        return _stricmp(arg, "base") == 0 || std::strcmp(arg, "1") == 0;
    return true;
}

ID3D12InfoQueue* g_infoq = nullptr;

static ID3D12InfoQueue1* s_infoq1 = nullptr;
static DWORD             s_infoq1_cookie = 0;

static void __stdcall OnDebugLayerMessage(
    D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id, LPCSTR, void*)
{
    if (sev > D3D12_MESSAGE_SEVERITY_ERROR) return;
    static std::mutex                        s_mtx;
    static std::unordered_map<int, unsigned> s_traced;
    unsigned                                 k = 0;
    {
        std::lock_guard<std::mutex> l(s_mtx);
        unsigned&                   seen = s_traced[(int)id];
        if (seen >= 3) return;
        k = ++seen;
    }
    char stack[3072];
    CrashHandler::FormatCallers(stack, sizeof(stack), 1);
    const wchar_t* ctx = CrashHandler::Context();
    logger::write("DBGCALLER id=%d #%u thread %lu%s%ls: %s", (int)id, k, GetCurrentThreadId(),
        ctx ? " in section " : "", ctx ? ctx : L"", stack);
}

extern HINSTANCE migoto_handle;
void SetExternalUserConfigDirectory(const wchar_t *directory);

// Mods directory, set in ActivateD3D12.
std::wstring g_wwmi_dir;

static void on_destroy_swapchain(swapchain*, bool resize)
{
    if (resize) {
        MigotoPort::BackBufferForget();
        return;
    }
    MigotoPort::ModUiClearShield();
    MigotoPort::BackBufferForget();
    ::logger::write("Exit: swapchain destroyed -- stopping the watchdogs and saving the persistent settings");
    FrameEndForgetSwapchain();
    StallWatchdogStop();
    DredWatchdogStop();
    PortApi::SaveSettingsOnExit();
}
extern void RtStopStructTexFallbackWorker();

static void on_destroy_device(device*)
{
    StallWatchdogStop();
    DredWatchdogStop();
    RfxPatchWorkerStop(); // it creates PSOs on this device
    g_heap_to_res.unwatch_all();
    RtStopStructTexFallbackWorker();
    g_tex_lod_worker.Stop();
    TexCrawlFlushIfQuiet(0);
    TexLodOnExit();
    TexLookOnExit();
    g_tex_defer.Stop();
    g_tex_gc.Stop();
    g_tex_replace.Stop();
    PortApi::SaveSettingsOnExit();
}

static device* DeviceOf(device* dev)
{
    return dev;
}
static device* DeviceOf(device_object* obj)
{
    return obj->get_device();
}

template <auto Fn>
struct OwnDevice;
template <typename R, typename First, typename... A, R (*Fn)(First, A...)>
struct OwnDevice<Fn> {
    static R call(First first, A... a)
    {
        if (DeviceOf(first) != g_reshade_device) return R();
        return Fn(first, a...);
    }
};
#define OWN_DEVICE(hook) (&OwnDevice<hook>::call)

static void ActivateD3D12();

static void on_init_device(device* dev)
{
    ::logger::write("Init: on_init_device entered (api=%d)", (int)dev->get_api());
    if (dev->get_api() != device_api::d3d12) {
        ::logger::write("Init: not a D3D12 device -- its events are ignored");
        return;
    }
    if (g_reshade_device) {
        ::logger::write("Init: a further D3D12 device -- its events are ignored, the addon stays on the first");
        return;
    }
    {
        g_is_dx12 = true;
        g_reshade_device = dev;
        ActivateD3D12();
        ::logger::write("Init: d3d12 device; calling ModLoader::Init");
        ModLoader::Init(dev);
        ::logger::write("Init: ModLoader::Init returned");

        auto* d3d12_dev = reinterpret_cast<ID3D12Device*>(dev->get_native());
        g_d3d12_device = d3d12_dev;
        if (g_dx12_debug_layer && d3d12_dev && !g_infoq) {
            if (SUCCEEDED(d3d12_dev->QueryInterface(IID_PPV_ARGS(&g_infoq))) && g_infoq) {
                g_infoq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
                g_infoq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
                D3D12_MESSAGE_SEVERITY  keep[] = { D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR };
                D3D12_INFO_QUEUE_FILTER flt{};
                flt.AllowList.NumSeverities = _countof(keep);
                flt.AllowList.pSeverityList = keep;
                g_infoq->PushStorageFilter(&flt);
                ::logger::write("DX12 DEBUG LAYER: InfoQueue attached -- validation errors (not warnings) will be "
                                "logged as DBGLAYER");
                if (SUCCEEDED(g_infoq->QueryInterface(IID_PPV_ARGS(&s_infoq1))) && s_infoq1 &&
                    SUCCEEDED(s_infoq1->RegisterMessageCallback(
                        OnDebugLayerMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, s_infoq1, &s_infoq1_cookie)))
                    ::logger::write("DX12 DEBUG LAYER: the call stack of each error id is logged as DBGCALLER, "
                                    "first 3 per id");
                else
                    ::logger::write("DX12 DEBUG LAYER: no message callback on this layer -- errors carry no call "
                                    "stack");
            } else {
                ::logger::write("DX12 DEBUG LAYER: no ID3D12InfoQueue on this device -- the layer is on but "
                                "nothing can be read back");
            }
        }
        ::logger::write("Init: on_init_device complete");
    }
}

extern "C" __declspec(dllexport) const char* NAME = "wwmi-dx12";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Wuthering Waves Mod Importer (DX12)";

// The patched shaders of a build from before cache\shaders, moved there; how many.
static unsigned MoveShaderCache(const std::wstring& from, const std::wstring& to)
{
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW          fd;
    const HANDLE              h = FindFirstFileW((from + L"\\*.rfx*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.emplace_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    unsigned moved = 0;
    for (const std::wstring& n : names)
        moved += MoveFileExW((from + L"\\" + n).c_str(), (to + L"\\" + n).c_str(), 0) != 0;
    return moved;
}

static bool on_draw_indexed_guarded(command_list* cmd_list, uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
    WWMI_GUARD_BEGIN("on_draw_indexed")
    return on_draw_indexed_body(cmd_list, index_count, instance_count, first_index, vertex_offset,
        first_instance);
    WWMI_GUARD_END(false) // false = let the game draw its own geometry for this call
}

// X23: the J1 game-draw table (morph_dump.h GameDrawRecord). Outside the SEH guard on purpose:
// this layer holds a struct and takes a lock, which the guard function may not (C2712). Entry state is read
// BEFORE the body -- the pass as the GAME set it, untouched by anything the port binds -- and the outcome
// AFTER, from the body's return value and the t_x23_* flags. While J1 is not armed this costs one relaxed
// load and a branch per draw.
// X25: the mesh buffers CLAIMED draws bound, remembered so a draw of the same buffers through
// an index range the ini does not declare can be listed too (comp = -1, same_mesh). Only armed J1 reaches
// this. A handful of entries: one character = one vb0 and one IB.
namespace {
struct X25Mesh {
    uint64_t vb0;
    uint64_t ib;
    int      mod;
};
std::mutex g_x25_mtx;
X25Mesh    g_x25[16];
int        g_x25_n = 0;
}
static bool X23Begin(command_list* cmd_list, uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, MorphDump::GameDrawRecord* r)
{
    auto       hit = g_first_to_comp.find(FirstCountKey(first_index, index_count));
    const bool is_comp = !(hit == g_first_to_comp.end() || hit->second < 0 || hit->second >= (int)g_merged_comps.size());
    int        mod_id = -1;
    {
        const auto& gs = CmdGfx(cmd_list);
        r->pso = gs.gfx_pso;
        if (gs.vp_count) {
            r->vp_w = gs.vp[0].Width;
            r->vp_h = gs.vp[0].Height;
        }
        r->stencil_ref = gs.stencil_ref;
        r->stencil_ref_set = gs.stencil_ref_set ? 1 : 0;
        r->vb0_va = gs.vb[0].va;
        r->vb6_va = gs.vb[6].va;
        r->vb6_stride = gs.vb[6].stride;
        r->ib_va = gs.ib.va;
        const CmdRT& rt = CmdRt(cmd_list);
        if (rt.valid) {
            r->rtv_count = rt.rtv_count;
            r->has_dsv = rt.has_dsv ? 1 : 0;
            r->dsv = rt.dsv;
            r->pass_gen = rt.pass_gen;
        }
    }
    if (is_comp) {
        const MergedComp& mc = g_merged_comps[hit->second];
        mod_id = mc.mod_id;
        r->comp = mc.comp_idx;
        if (r->vb0_va || r->ib_va) {
            std::lock_guard<std::mutex> l(g_x25_mtx);
            bool                        known = false;
            for (int i = 0; i < g_x25_n; ++i)
                if (g_x25[i].mod == mod_id && g_x25[i].vb0 == r->vb0_va && g_x25[i].ib == r->ib_va) {
                    known = true;
                    break;
                }
            if (!known && g_x25_n < 16) g_x25[g_x25_n++] = X25Mesh{ r->vb0_va, r->ib_va, mod_id };
        }
    } else {
        // Not a component. The same character's mesh through another index range?
        if (!r->vb0_va && !r->ib_va) return false;
        uint8_t same = 0;
        {
            std::lock_guard<std::mutex> l(g_x25_mtx);
            for (int i = 0; i < g_x25_n; ++i) {
                uint8_t s = 0;
                if (r->vb0_va && g_x25[i].vb0 == r->vb0_va) s |= 1;
                if (r->ib_va && g_x25[i].ib == r->ib_va) s |= 2;
                if (s) {
                    same = s;
                    mod_id = g_x25[i].mod;
                    break;
                }
            }
        }
        if (!same) return false;
        r->same_mesh = same;
        r->comp = -1;
    }
    r->frame = g_frame_id;
    r->mod_id = mod_id;
    r->list = (uint64_t)cmd_list->get_native();
    r->thread = (uint32_t)GetCurrentThreadId();
    r->index_count = index_count;
    r->first_index = first_index;
    r->instance_count = instance_count;
    cmd_list->get_private_data(s_iid_vs_hash, &r->vs);
    cmd_list->get_private_data(s_iid_ps_hash, &r->ps);
    if (mod_id >= 0 && mod_id < (int)g_geomods.size()) r->ns = g_geomods[mod_id].wcl_ns.c_str();
    X27Cb cbs[8];
    X27ResolveVsCbs(cmd_list, cbs);
    r->cb3_va = cbs[3].va;
    r->cb4_va = cbs[4].va;
    t_x23_claimed = t_x23_shadow = t_x23_cast = t_x23_hidden = 0;
    t_x23_injected = 0;
    t_x23_merges = 0;
    return true;
}

static bool on_draw_indexed(command_list* cmd_list, uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
    CmdNoteDraw(cmd_list);
#ifdef WWMI_DEBUG
    if (g_present_log_left.load(std::memory_order_relaxed) > 0) {
        g_present_log_draws.fetch_add(1, std::memory_order_relaxed);
        g_frame_end_draws.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    if (!MorphDump::Armed() || !g_merged_loaded || g_addon_disabled)
        return on_draw_indexed_guarded(cmd_list, index_count, instance_count, first_index, vertex_offset, first_instance);
    MorphDump::GameDrawRecord r{};
    const bool                row = X23Begin(cmd_list, index_count, instance_count, first_index, &r);
    const bool                ret = on_draw_indexed_guarded(cmd_list, index_count, instance_count, first_index, vertex_offset, first_instance);
    if (row) {
        r.claimed = t_x23_claimed;
        r.shadow = t_x23_shadow;
        r.cast = t_x23_cast;
        r.hidden = t_x23_hidden;
        r.injected = t_x23_injected;
        r.ret = ret ? 1 : 0;
        r.merges = t_x23_merges;
        MorphDump::RecordGameDraw(r);
    }
    return ret;
}

static void on_present(command_queue* queue, swapchain* sc, const rect*, const rect*, uint32_t, const rect*)
{
    WWMI_GUARD_BEGIN("on_present")
    on_present_body(queue, sc);
    WWMI_GUARD_END_VOID()
}

static int g_dred_armed = 0;

void DrainDebugLayer(const char* when)
{
    if (!g_infoq) return;
    const UINT64 n = g_infoq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(g_infoq->GetMessage(i, nullptr, &len)) || !len) continue;
        std::vector<char> buf(len);
        D3D12_MESSAGE*    m = (D3D12_MESSAGE*)buf.data();
        if (FAILED(g_infoq->GetMessage(i, m, &len))) continue;
        if (m->Severity > D3D12_MESSAGE_SEVERITY_ERROR) continue;
        static std::unordered_map<int, uint64_t> s_seen;
        const uint64_t                           k = ++s_seen[(int)m->ID];
        const bool                               full = k <= 3 || strcmp(when, "frame") != 0;
        bool                                     decade = false;
        for (uint64_t p = 10; p <= k; p *= 10)
            if (k == p) {
                decade = true;
                break;
            }
        if (full)
            logger::write("DBGLAYER [%s] sev=%d id=%d #%llu: %.*s", when, (int)m->Severity, (int)m->ID,
                (unsigned long long)k, (int)m->DescriptionByteLength, m->pDescription);
        else if (decade)
            logger::write("DBGLAYER id=%d seen %llu times (only the first 3 are printed)", (int)m->ID,
                (unsigned long long)k);
    }
    if (n) g_infoq->ClearStoredMessages();
}

static std::wstring          s_win64;
static std::filesystem::path s_ini_base_dir;
static std::wstring          s_resolved_runtime_dir;
static std::filesystem::path s_mounted_runtime_dir;

namespace {

std::wstring WwmiTrimWide(std::wstring s)
{
    auto is_trim = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'"'; };
    while (!s.empty() && is_trim(s.front())) s.erase(s.begin());
    while (!s.empty() && is_trim(s.back())) s.pop_back();
    return s;
}

std::filesystem::path WwmiModuleFilePath(HMODULE mod)
{
    wchar_t buf[32768]{};
    const DWORD n = GetModuleFileNameW(mod, buf, (DWORD)std::size(buf));
    if (!n || n >= std::size(buf)) return {};
    return std::filesystem::path(std::wstring(buf, n));
}

std::wstring WwmiResolveConfigPath(const std::wstring& raw_val, const std::filesystem::path& base_dir)
{
    std::wstring s = WwmiTrimWide(raw_val);
    if (s.empty()) return {};
    for (wchar_t& c : s)
        if (c == L'/') c = L'\\';
    std::wstring out;
    if (PathIsRelativeW(s.c_str()) && !base_dir.empty()) {
        const std::filesystem::path joined = base_dir / s;
        wchar_t full_buf[32768]{};
        const DWORD fn = GetFullPathNameW(joined.c_str(), (DWORD)std::size(full_buf), full_buf, nullptr);
        out = (fn > 0 && fn < std::size(full_buf)) ? std::wstring(full_buf, fn) : joined.lexically_normal().wstring();
    } else {
        out = s;
    }
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

std::filesystem::path WwmiResolveIniFile(
    const std::filesystem::path& addon_dir, const std::filesystem::path& base_root, const std::filesystem::path& exe_dir)
{
    std::error_code ec;
    wchar_t env_ini[32768]{};
    const DWORD env_n = GetEnvironmentVariableW(L"WWMI_DX12_INI", env_ini, (DWORD)std::size(env_ini));
    if (env_n > 0 && env_n < std::size(env_ini)) {
        const std::filesystem::path p(WwmiTrimWide(std::wstring(env_ini, env_n)));
        if (!p.empty() && std::filesystem::is_regular_file(p, ec)) return p;
    }
    for (const std::filesystem::path& cand : {
             base_root / L"wwmi_dx12.ini",
             addon_dir / L"wwmi_dx12.ini",
             exe_dir / L"wwmi_dx12.ini",
         }) {
        if (!cand.empty() && std::filesystem::is_regular_file(cand, ec)) return cand;
    }
    return (!base_root.empty() ? base_root : exe_dir) / L"wwmi_dx12.ini";
}

bool WwmiIsAllowedTargetExe(const std::filesystem::path& exe_path, const std::wstring& ini_path)
{
    const std::wstring exe_name = exe_path.filename().wstring();
    if (exe_name.empty()) return false;

    std::wstring spec;
    wchar_t env_buf[1024]{};
    const DWORD env_n = GetEnvironmentVariableW(L"WWMI_DX12_TARGET_EXE", env_buf, (DWORD)std::size(env_buf));
    if (env_n > 0 && env_n < std::size(env_buf)) {
        spec = WwmiTrimWide(std::wstring(env_buf, env_n));
    } else if (!ini_path.empty()) {
        wchar_t ini_buf[1024]{};
        GetPrivateProfileStringW(L"wwmi-dx12", L"target_exe", L"", ini_buf, (DWORD)std::size(ini_buf), ini_path.c_str());
        spec = WwmiTrimWide(ini_buf);
    }
    if (spec.empty()) spec = L"Client-Win64-Shipping.exe";
    if (spec == L"*") return true;

    size_t pos = 0;
    while (pos <= spec.size()) {
        const size_t sep = spec.find(L';', pos);
        const std::wstring tok = WwmiTrimWide(sep == std::wstring::npos ? spec.substr(pos) : spec.substr(pos, sep - pos));
        if (!tok.empty() && _wcsicmp(exe_name.c_str(), tok.c_str()) == 0) return true;
        if (sep == std::wstring::npos) break;
        pos = sep + 1;
    }
    return false;
}

void WwmiRotateSessionLogs(const std::filesystem::path& logs_dir, int max_logs)
{
    if (max_logs <= 0) return;
    struct LogEntry {
        FILETIME     ft{};
        std::wstring path;
    };
    std::vector<LogEntry> entries;
    WIN32_FIND_DATAW      fd{};
    const HANDLE          h = FindFirstFileW((logs_dir / L"wwmi_dx12_candidate-*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        entries.push_back(LogEntry{ fd.ftLastWriteTime, (logs_dir / fd.cFileName).wstring() });
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    const size_t keep = static_cast<size_t>((std::max)(0, max_logs - 1));
    if (entries.size() <= keep) return;
    std::sort(entries.begin(), entries.end(), [](const LogEntry& a, const LogEntry& b) {
        return CompareFileTime(&a.ft, &b.ft) > 0;
    });
    for (size_t i = keep; i < entries.size(); ++i) {
        DeleteFileW(entries[i].path.c_str());
    }
}

void WwmiBootstrapStandaloneConfig(const std::filesystem::path& base_root, const std::filesystem::path& ini_file)
{
    const std::filesystem::path ini_dir = ini_file.parent_path();
    std::error_code             ec;

    // 1. Resolve persistent runtime root (env > ini > legacy capture subdir > <base_root>\runtime)
    wchar_t env_rt[32768]{};
    const DWORD env_rt_n = GetEnvironmentVariableW(L"WWMI_DX12_RUNTIME_DIR", env_rt, (DWORD)std::size(env_rt));
    wchar_t env_cap[32768]{};
    const DWORD env_cap_n = GetEnvironmentVariableW(L"WWMI_RT_CAPTURE_DIR", env_cap, (DWORD)std::size(env_cap));
    const bool  has_explicit_capture = (env_cap_n > 0 && env_cap_n < std::size(env_cap));

    if (env_rt_n > 0 && env_rt_n < std::size(env_rt)) {
        s_resolved_runtime_dir = WwmiTrimWide(std::wstring(env_rt, env_rt_n));
    } else {
        wchar_t ini_rt[4096]{};
        GetPrivateProfileStringW(L"wwmi-dx12", L"runtime_dir", L"", ini_rt, (DWORD)std::size(ini_rt), ini_file.c_str());
        const std::wstring resolved_ini_rt = WwmiResolveConfigPath(ini_rt, ini_dir);
        if (!resolved_ini_rt.empty()) {
            s_resolved_runtime_dir = resolved_ini_rt;
            SetEnvironmentVariableW(L"WWMI_DX12_RUNTIME_DIR", s_resolved_runtime_dir.c_str());
        } else if (has_explicit_capture) {
            s_resolved_runtime_dir = std::wstring(env_cap, env_cap_n) + L"\\wwmi-runtime";
        } else {
            s_resolved_runtime_dir = (base_root / L"runtime").wstring();
            SetEnvironmentVariableW(L"WWMI_DX12_RUNTIME_DIR", s_resolved_runtime_dir.c_str());
        }
    }
    if (!s_resolved_runtime_dir.empty()) {
        std::filesystem::create_directories(s_resolved_runtime_dir, ec);
    }

    // 2. Session log & crash report output directory + automatic rotation in standalone mode
    if (!has_explicit_capture && !s_resolved_runtime_dir.empty()) {
        const std::filesystem::path logs_dir = std::filesystem::path(s_resolved_runtime_dir) / L"logs";
        std::filesystem::create_directories(logs_dir, ec);
        const int max_logs = (int)GetPrivateProfileIntW(L"wwmi-dx12", L"max_logs", 10, ini_file.c_str());
        WwmiRotateSessionLogs(logs_dir, max_logs);
        SetEnvironmentVariableW(L"WWMI_RT_CAPTURE_DIR", logs_dir.c_str());
    }

    // 3. Hybrid raster skin mode (env > ini[skin_mode] > default 'base')
    char env_skin[32]{};
    const DWORD env_skin_n = GetEnvironmentVariableA("WWMI_HYBRID_RASTER_MODE", env_skin, (DWORD)std::size(env_skin));
    bool is_base_skin = true;
    if (env_skin_n > 0 && env_skin_n < std::size(env_skin)) {
        is_base_skin = (_stricmp(env_skin, "base") == 0 || std::strcmp(env_skin, "1") == 0);
    } else {
        wchar_t ini_skin[64]{};
        GetPrivateProfileStringW(L"wwmi-dx12", L"skin_mode", L"base", ini_skin, (DWORD)std::size(ini_skin), ini_file.c_str());
        const std::wstring s = WwmiTrimWide(ini_skin);
        if (_wcsicmp(s.c_str(), L"exact") == 0 || s == L"0" || _wcsicmp(s.c_str(), L"off") == 0 ||
            _wcsicmp(s.c_str(), L"false") == 0) {
            is_base_skin = false;
        }
        SetEnvironmentVariableA("WWMI_HYBRID_RASTER_MODE", is_base_skin ? "base" : "exact");
    }
    g_hybrid_skin_mode.store(is_base_skin ? 1 : 0, std::memory_order_release);

    // 4. Native V2 profile cache path (env > ini[native_profile_cache] > <runtime_dir>\native-profiles-v2.txt)
    if (!GetEnvironmentVariableW(L"WWMI_RT_NATIVE_PROFILE_CACHE", nullptr, 0) && !s_resolved_runtime_dir.empty()) {
        wchar_t ini_np[4096]{};
        GetPrivateProfileStringW(L"wwmi-dx12", L"native_profile_cache", L"", ini_np, (DWORD)std::size(ini_np), ini_file.c_str());
        std::wstring np_path = WwmiResolveConfigPath(ini_np, ini_dir);
        if (np_path.empty()) np_path = (std::filesystem::path(s_resolved_runtime_dir) / L"native-profiles-v2.txt").wstring();
        std::filesystem::create_directories(std::filesystem::path(np_path).parent_path(), ec);
        SetEnvironmentVariableW(L"WWMI_RT_NATIVE_PROFILE_CACHE", np_path.c_str());
    }

    // 5. Modular seed overlay directories (env > ini[seed_dirs] > <base_root>\seed;<runtime_dir>\seed)
    if (!GetEnvironmentVariableW(L"WWMI_DX12_SEED_DIRS", nullptr, 0) && !s_resolved_runtime_dir.empty()) {
        wchar_t ini_seeds[8192]{};
        GetPrivateProfileStringW(L"wwmi-dx12", L"seed_dirs", L"", ini_seeds, (DWORD)std::size(ini_seeds), ini_file.c_str());
        const std::wstring raw_seeds = WwmiTrimWide(ini_seeds);
        std::wstring       resolved_seeds;
        if (!raw_seeds.empty()) {
            size_t pos = 0;
            while (pos <= raw_seeds.size()) {
                const size_t sep = raw_seeds.find(L';', pos);
                const std::wstring part = WwmiResolveConfigPath(
                    sep == std::wstring::npos ? raw_seeds.substr(pos) : raw_seeds.substr(pos, sep - pos), ini_dir);
                if (!part.empty()) {
                    if (!resolved_seeds.empty()) resolved_seeds += L';';
                    resolved_seeds += part;
                }
                if (sep == std::wstring::npos) break;
                pos = sep + 1;
            }
        } else {
            const std::filesystem::path user_seed = std::filesystem::path(s_resolved_runtime_dir) / L"seed";
            std::filesystem::create_directories(user_seed, ec);
            const std::filesystem::path pkg_seed = base_root / L"seed";
            if (std::filesystem::is_directory(pkg_seed, ec))
                resolved_seeds = pkg_seed.wstring() + L";" + user_seed.wstring();
            else
                resolved_seeds = user_seed.wstring();
        }
        if (!resolved_seeds.empty()) SetEnvironmentVariableW(L"WWMI_DX12_SEED_DIRS", resolved_seeds.c_str());
    }
}

} // namespace

static void ActivateD3D12()
{
    static std::atomic<bool> s_done{ false };
    if (s_done.exchange(true)) return;
    HotPath::Init();
    CrashHandler::InitReporting();                // report file + module range; needed by the SEH guards
    if (g_crash_handler) CrashHandler::Install(); // the last-chance filter is the optional part
    std::wstring wwmi_dir_str;
    {
        wchar_t raw[4096]{};
        GetPrivateProfileStringW(L"wwmi-dx12", L"wwmi_dir", L"", raw, (DWORD)std::size(raw), g_addon_ini_path.c_str());
        wwmi_dir_str = WwmiResolveConfigPath(raw, !s_ini_base_dir.empty() ? s_ini_base_dir : std::filesystem::path(s_win64));
    }
    if (!wwmi_dir_str.empty()) {
        const auto dumps_root = s_mounted_runtime_dir.empty()
            ? std::filesystem::path(wwmi_dir_str) : s_mounted_runtime_dir;
        MigotoPort::SetDumpDir((dumps_root / L"DX12_Dumps").c_str());
        wchar_t persistent_runtime[32768]{};
        const DWORD persistent_runtime_n = GetEnvironmentVariableW(L"WWMI_DX12_RUNTIME_DIR", persistent_runtime, (DWORD)std::size(persistent_runtime));
        if (persistent_runtime_n && persistent_runtime_n < std::size(persistent_runtime))
            g_data_dir = persistent_runtime;
        else if (!s_resolved_runtime_dir.empty())
            g_data_dir = s_resolved_runtime_dir;
        else {
            wchar_t rt_output[32768]{};
            const DWORD rt_output_n = GetEnvironmentVariableW(L"WWMI_RT_CAPTURE_DIR", rt_output, (DWORD)std::size(rt_output));
            if (rt_output_n && rt_output_n < std::size(rt_output))
                g_data_dir = std::wstring(rt_output, rt_output_n) + L"\\wwmi-runtime";
            else
                g_data_dir = wwmi_dir_str + L"\\DX12_Runtime";
        }
        const std::wstring cache_dir = g_data_dir + L"\\cache";
        g_shader_cache_dir = cache_dir + L"\\shaders";
        std::error_code ec;
        std::filesystem::create_directories(g_shader_cache_dir, ec);
        if (const unsigned moved = MoveShaderCache(cache_dir, g_shader_cache_dir))
            ::logger::write("Config: %u cached shader patch(es) moved into %S", moved, g_shader_cache_dir.c_str());
        // Preserve cache evidence across sessions.
        if (GetPrivateProfileIntW(L"wwmi-dx12", L"clear_cache", 0, g_addon_ini_path.c_str()) != 0)
            ::logger::write("Config: clear_cache ignored in bounded candidate; cache evidence is retained");
        g_wwmi_dir = wwmi_dir_str;
        ModLoader::Reload(); // deferred -- executes on first Tick() after device init
        TexLodInit(cache_dir);
        TexLookInit(cache_dir);
        ::logger::write("Config: ini=%S wwmi_dir=%S runtime_dir=%S skin_mode=%s",
            g_addon_ini_path.c_str(), wwmi_dir_str.c_str(), g_data_dir.c_str(),
            WwmiIsHybridBaseSkinMode() ? "base" : "exact");
    } else {
        ::logger::write("Config: wwmi_dx12.ini not found or wwmi_dir not set (looked at %S)", g_addon_ini_path.c_str());
    }

    reshade::register_event<reshade::addon_event::destroy_device>(OWN_DEVICE(HP(destroy_device)));
    reshade::register_event<reshade::addon_event::destroy_swapchain>(OWN_DEVICE(HP(destroy_swapchain)));
    reshade::register_event<reshade::addon_event::create_resource>(OWN_DEVICE(HP(create_resource))); // ALLOW_UNORDERED_ACCESS on a shapekey buffer at creation
    reshade::register_event<reshade::addon_event::init_resource>(OWN_DEVICE(HP(init_resource)));
    reshade::register_event<reshade::addon_event::update_texture_region>(OWN_DEVICE(HP(update_texture_region)));
    reshade::register_event<reshade::addon_event::copy_buffer_to_texture>(OWN_DEVICE(HP(copy_buffer_to_texture)));
    reshade::register_event<reshade::addon_event::copy_texture_region>(OWN_DEVICE(HP(copy_texture_region)));
    reshade::register_event<reshade::addon_event::copy_resource>(OWN_DEVICE(HP(copy_resource)));
    reshade::register_event<reshade::addon_event::copy_buffer_region>(OWN_DEVICE(HP(copy_buffer_region)));
    reshade::register_event<reshade::addon_event::barrier>(OWN_DEVICE(HP(barrier)));
    MigotoPort::SetResourceStateQuery(ResourceStateOf);
    {
        const auto cso = WwmiReadFile((g_cso_dir + L"\\DumpPreview.cso").c_str()); // the capture's .jpg beside each .dds
        MigotoPort::SetDumpPreviewShader(cso.data(), cso.size());
    }
    /* Same contract as the state query above: the addon owns g_buffer_reloc, the port leaf asks. */
    MigotoPort::g_query_reloc = [](unsigned long long res, unsigned long long* va,
                                    unsigned long long* bytes) -> bool {
        RelocBuf rb{};
        if (!g_buffer_reloc.Resolve(reinterpret_cast<ID3D12Resource*>(res), &rb)) return false;
        if (va) *va = rb.va;
        if (bytes) *bytes = rb.bytes;
        return true;
    };
    reshade::register_event<reshade::addon_event::execute_command_list>(OWN_DEVICE(HP(execute_command_list))); // submission order vs record order

    reshade::register_event<reshade::addon_event::destroy_resource>(OWN_DEVICE(HP(destroy_resource)));
    reshade::register_event<reshade::addon_event::init_resource_view>(OWN_DEVICE(HP(init_resource_view)));
    reshade::register_event<reshade::addon_event::create_pipeline>(OWN_DEVICE(HP(create_pipeline))); // shader replacement (DXIL swap by hash)
    reshade::register_event<reshade::addon_event::init_pipeline>(OWN_DEVICE(HP(init_pipeline)));
    reshade::register_event<reshade::addon_event::init_pipeline_layout>(OWN_DEVICE(HP(init_pipeline_layout)));
    reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(OWN_DEVICE(HP(destroy_pipeline_layout)));
    reshade::register_event<reshade::addon_event::destroy_pipeline>(OWN_DEVICE(HP(destroy_pipeline)));
    reshade::register_event<reshade::addon_event::bind_pipeline>(OWN_DEVICE(HP(bind_pipeline)));
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(OWN_DEVICE(HP(bind_descriptor_tables)));
    reshade::register_event<reshade::addon_event::reset_command_list>(OWN_DEVICE(HP(reset_command_list))); // the record follows D3D12: nothing survives a Reset
    reshade::register_event<reshade::addon_event::close_command_list>(OWN_DEVICE(HP(close_command_list)));
    reshade::register_event<reshade::addon_event::build_acceleration_structure>(OWN_DEVICE(HP(build_acceleration_structure))); // RT ACTIVE + the game's RT work in the pin's tallies
    reshade::register_event<reshade::addon_event::dispatch_rays>(OWN_DEVICE(HP(dispatch_rays)));
    reshade::register_event<reshade::addon_event::push_descriptors>(OWN_DEVICE(HP(push_descriptors)));
    reshade::register_event<reshade::addon_event::bind_vertex_buffers>(OWN_DEVICE(HP(bind_vertex_buffers)));
    reshade::register_event<reshade::addon_event::bind_index_buffer>(OWN_DEVICE(HP(bind_index_buffer)));
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OWN_DEVICE(HP(bind_render_targets)));
    // The game's non-PSO state, so IssueDraw can restore what it overwrites (their CommandList.cpp:2385).
    reshade::register_event<reshade::addon_event::bind_viewports>(OWN_DEVICE(HP(bind_viewports)));
    reshade::register_event<reshade::addon_event::bind_scissor_rects>(OWN_DEVICE(HP(bind_scissor_rects)));
    reshade::register_event<reshade::addon_event::bind_pipeline_states>(OWN_DEVICE(HP(bind_pipeline_states)));
    reshade::register_event<reshade::addon_event::draw_indexed>(OWN_DEVICE(HP(draw_indexed)));
    reshade::register_event<reshade::addon_event::dispatch>(OWN_DEVICE(HP(dispatch)));
    reshade::register_event<reshade::addon_event::present>(OWN_DEVICE(HP(present)));

    ProfBindLibSteps(); // the lib's timed steps -> their rows, before the overlay can enable the profiler
    DockOverlayPanelOnce();
    reshade::register_event<reshade::addon_event::reshade_open_overlay>(OnOverlayOpen);
    reshade::register_event<reshade::addon_event::reshade_overlay>(DrawOverlayPanel);
    ::logger::write("Init: D3D12 device -- events + overlay registered");
}

static constexpr uint32_t kOldestReShadeApi = 18;

static bool RegisterWithReShade(HMODULE addon_module)
{
    if (reshade::register_addon(addon_module)) {
        g_reshade_api = RESHADE_API_VERSION;
        return true;
    }
    const HMODULE rs = reshade::internal::get_reshade_module_handle();
    const auto    reg = rs ? reinterpret_cast<bool (*)(void*, uint32_t)>(GetProcAddress(rs, "ReShadeRegisterAddon")) : nullptr;
    const auto    imgui = rs ? reinterpret_cast<const imgui_function_table* (*)(uint32_t)>(GetProcAddress(rs, "ReShadeGetImGuiFunctionTable")) : nullptr;
    if (!reg || !imgui) return false;
    for (uint32_t v = RESHADE_API_VERSION - 1; v >= kOldestReShadeApi; --v) {
        if (!reg(addon_module, v)) continue;
        g_reshade_api = v;
        imgui_function_table_instance() = imgui(IMGUI_VERSION_NUM);
        return imgui_function_table_instance() != nullptr;
    }
    return false;
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE wwmi_host_module)
{
    g_wwmi_host_module = wwmi_host_module;
    const std::filesystem::path addon_file = WwmiModuleFilePath(addon_module);
    const std::filesystem::path addon_dir = addon_file.parent_path();
    using MountedBasePath = const wchar_t *(WINAPI *)();
    const auto get_mounted_base = wwmi_host_module ? reinterpret_cast<MountedBasePath>(
        GetProcAddress(wwmi_host_module, "WWMIGetMountedBasePath")) : nullptr;
    const wchar_t *mounted_base = get_mounted_base ? get_mounted_base() : nullptr;
    const std::filesystem::path base_root = mounted_base ? std::filesystem::path(mounted_base) :
        (_wcsicmp(addon_dir.filename().c_str(), L"reshade-addons") == 0 && addon_dir.has_parent_path())
            ? addon_dir.parent_path()
            : addon_dir;
    const std::filesystem::path exe_file = WwmiModuleFilePath(nullptr);
    const std::filesystem::path exe_dir = exe_file.parent_path();
    const std::filesystem::path resolved_ini = WwmiResolveIniFile(addon_dir, base_root, exe_dir);
    std::wstring                ini_path = resolved_ini.wstring();

    // Target process whitelist guard: ignore helper executables or launchers before touching ReShade/hooks/files.
    if (!WwmiIsAllowedTargetExe(exe_file, ini_path)) {
        return false;
    }

    if (!RegisterWithReShade(addon_module)) return false;
    if (!InstallDescriptorPublisher(wwmi_host_module)) {
        reshade::log::message(reshade::log::level::error,"WWMI descriptor publisher requires the paired native host.");
        reshade::unregister_addon(addon_module);
        return false;
    }
    const int wwmi_binding_status = wwmi::binding_client::Initialize(wwmi_host_module);
    migoto_handle = addon_module;

    std::wstring win64 = exe_dir.wstring();
    if (!win64.empty() && win64.back() != L'\\' && win64.back() != L'/') win64 += L'\\';
    s_ini_base_dir = resolved_ini.parent_path();

    // Bootstrap standalone INI/default paths into process-local environment when launched directly without wrapper scripts.
    WwmiBootstrapStandaloneConfig(base_root, resolved_ini);
    s_mounted_runtime_dir = mounted_base ? base_root / L"runtime" : std::filesystem::path{};
    if (mounted_base) {
        std::error_code ec;
        std::filesystem::create_directories(s_mounted_runtime_dir, ec);
        if (ec) ::logger::write("Cannot create external settings directory: %S", s_mounted_runtime_dir.c_str());
    }
    SetExternalUserConfigDirectory(mounted_base ? s_mounted_runtime_dir.c_str() : nullptr);

    // Our shaders and dxc: wwmi_dx12\ next to the addon, else under base_root, else next to the game exe, else addon_dir.
    g_cso_dir = addon_dir.wstring();
    for (const std::filesystem::path& d : {
             addon_dir / L"wwmi_dx12",
             base_root / L"wwmi_dx12",
             exe_dir / L"wwmi_dx12",
         }) {
        std::error_code ec;
        if (!d.empty() && std::filesystem::is_directory(d, ec)) {
            g_cso_dir = d.wstring();
            break;
        }
    }
    UavObserve::g_probe.configure(GetPrivateProfileIntW(L"wwmi-dx12", L"uav_observe", 0, ini_path.c_str()) != 0);
    g_heap_to_res.probe_retire_hook.store(+[](uint64_t h) { UavObserve::g_probe.heap_destroy(h); }, std::memory_order_release);
    // The candidate writes exclusively to this launch's results directory; game logs are untouched.
    wchar_t rt_results[32768]{};
    const DWORD rt_results_n = GetEnvironmentVariableW(L"WWMI_RT_CAPTURE_DIR", rt_results, (DWORD)std::size(rt_results));
    if (rt_results_n && rt_results_n < std::size(rt_results)) {
        // A short-lived probe (or a same-process addon reload) must not
        // reserve the only CREATE_NEW pathname before the real game device.
        LARGE_INTEGER instance{};
        QueryPerformanceCounter(&instance);
        const auto path = std::wstring(rt_results) + L"\\wwmi_dx12_candidate-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(instance.QuadPart) + L".log";
        ::logger::init(path.c_str());
    }
    char rt_session[128]{};
    const DWORD rt_session_n = GetEnvironmentVariableA("WWMI_RT_CAPTURE_SESSION", rt_session, (DWORD)std::size(rt_session));
    ::logger::probe("RTCAPTURE addon_loaded=1 pid=%lu candidate_session=%s", GetCurrentProcessId(),
        rt_session_n && rt_session_n < std::size(rt_session) ? rt_session : "missing");
    ::logger::set_build_id(WWMI_VERSION " " WWMI_BUILD_SHA); // emitted the moment the level is raised above 0
    ::logger::write("Research binding observer: supplied host endpoint %s (status=%d); no GPU/RT compatibility inferred",
        wwmi_binding_status == 0 ? "available" : "unavailable", wwmi_binding_status);

    if (GetPrivateProfileIntW(L"wwmi-dx12", L"dred", 0, ini_path.c_str()) != 0) {
        ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
        HRESULT                                  hr = D3D12GetDebugInterface(IID_PPV_ARGS(&dred));
        if (SUCCEEDED(hr) && dred) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            {
                ID3D12DeviceRemovedExtendedDataSettings1* s1 = nullptr;
                if (SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&s1))) && s1) {
                    s1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                    s1->Release();
                }
            }
            dred->Release();
            g_dred_armed = 1;
        } else {
            g_dred_armed = -(int)(hr & 0xffff);
        }
    }

    {
        const bool debug = GetPrivateProfileIntW(L"wwmi-dx12", L"debug", 0, ini_path.c_str()) != 0;
        const int  lvl = GetPrivateProfileIntW(L"wwmi-dx12", L"log_level", g_log_verbosity, ini_path.c_str());
        SetDiagnostics(debug, (lvl < 1) ? 1 : (lvl > 2) ? 2
                                                        : lvl);
    }

    // Now that the level is live, say what the pre-log arming actually did.
    if (g_dred_armed > 0)
        ::logger::write("DRED: breadcrumbs + page-fault tracking FORCED ON");
    else if (g_dred_armed == 0)
        ::logger::write("DRED: off (dred=1 in the ini turns it on)");
    else
        ::logger::write("DRED: unavailable (hr=....%04x) -- device likely already created; a removal will "
                        "report only a reason code",
            (unsigned)(-g_dred_armed));
    if (g_reshade_api != RESHADE_API_VERSION)
        ::logger::write("ReShade: add-on API %u accepted, not %u -- a ReShade older than 6.8.0, which is not supported",
            g_reshade_api, (unsigned)RESHADE_API_VERSION);

    PortApi::SetTextureDHash(&TexDHashLookup, &TexDHashState);
    g_dx12_debug_layer = GetPrivateProfileIntW(L"wwmi-dx12", L"dx12_debug_layer", 0, ini_path.c_str()) != 0;
    g_addon_ini_path = ini_path;
    g_decensor = GetPrivateProfileIntW(L"wwmi-dx12", L"decensor", 0, ini_path.c_str()) != 0;
    g_hide_uid = GetPrivateProfileIntW(L"wwmi-dx12", L"hide_uid", 0, ini_path.c_str()) != 0;
#ifdef WWMI_DEBUG
    g_hide_outline = GetPrivateProfileIntW(L"wwmi-dx12", L"hide_outline", 0, ini_path.c_str()) != 0;
#endif
    g_tex_deferred = GetPrivateProfileIntW(L"wwmi-dx12", L"tex_background", 1, ini_path.c_str()) != 0;
    g_texlod_resemblance = GetPrivateProfileIntW(L"wwmi-dx12", L"lod_resemblance", 1, ini_path.c_str()) != 0;
    g_texlod_cache = GetPrivateProfileIntW(L"wwmi-dx12", L"lod_cache", 1, ini_path.c_str()) != 0;
    MigotoPort::g_buffers_in_vram = GetPrivateProfileIntW(L"wwmi-dx12", L"buffers_in_vram", 1, ini_path.c_str()) != 0;
    const int budget_step = (int)GetPrivateProfileIntW(L"wwmi-dx12", L"tex_mod_budget", 1, ini_path.c_str());
    g_tex_mod_budget = budget_step >= 0 && budget_step < kTexBudgetSteps ? budget_step : 1;
    g_tex_distance = GetPrivateProfileIntW(L"wwmi-dx12", L"tex_distance", 0, ini_path.c_str()) != 0;
    g_tex_dhash_filters = GetPrivateProfileIntW(L"wwmi-dx12", L"texture_filters", 1, ini_path.c_str()) != 0;
    g_tex_dhash.SetEnabled(g_tex_dhash_filters);
    g_tex_struct_fallback = GetPrivateProfileIntW(L"wwmi-dx12", L"tex_struct_fallback", 1, ini_path.c_str()) != 0;
    {
        char sf_env[16]{};
        if (GetEnvironmentVariableA("WWMI_DX12_STRUCT_FALLBACK", sf_env, (DWORD)std::size(sf_env)) > 0)
            g_tex_struct_fallback = (sf_env[0] != '0');
    }
#ifdef WWMI_DEBUG
    g_tex_watch_ms = GetPrivateProfileIntW(L"wwmi-dx12", L"tex_watch_ms", 2000, ini_path.c_str());
    g_tex_vram_cap_mb = GetPrivateProfileIntW(L"wwmi-dx12", L"tex_vram_cap_mb", 0, ini_path.c_str());
#endif
    ::logger::write("Config: decensor=%d hide_uid=%d lod_resemblance=%d lod_cache=%d buffers_in_vram=%d tex_mod_budget=%s tex_distance=%d tex_struct_fallback=%d texture_filters=%d",
        (int)g_decensor, (int)g_hide_uid, (int)g_texlod_resemblance, (int)g_texlod_cache, (int)MigotoPort::g_buffers_in_vram,
        TexBudgetName(g_tex_mod_budget), (int)g_tex_distance, (int)g_tex_struct_fallback, (int)g_tex_dhash_filters);
    g_tex_defer.Start();
    if (g_dx12_debug_layer) {
        ID3D12Debug*  dbg = nullptr;
        const HRESULT hrd = D3D12GetDebugInterface(IID_PPV_ARGS(&dbg));
        if (SUCCEEDED(hrd) && dbg) {
            dbg->EnableDebugLayer();
            ID3D12Debug1* dbg1 = nullptr;
            if (SUCCEEDED(dbg->QueryInterface(IID_PPV_ARGS(&dbg1))) && dbg1) {
                dbg1->SetEnableSynchronizedCommandQueueValidation(FALSE);
                dbg1->Release();
            }
            dbg->Release();
            ::logger::write("DX12 DEBUG LAYER: ENABLED (ini dx12_debug_layer=1), synchronized queue validation "
                            "off -- still slow; every validation error is logged with DBGLAYER");
        } else {
            ::logger::write("DX12 DEBUG LAYER: requested but unavailable (hr=%08x) -- install the "
                            "Graphics Tools optional feature",
                (unsigned)hrd);
        }
    }
    ::logger::write("Config: debug=%d (diagnostics %s, log level %d)", (int)g_debug,
        g_debug ? "ON" : "off", g_debug ? g_log_verbosity : 0);

    {
        std::wstring   eng = std::wstring(win64) + L"..\\..\\Saved\\Config\\WindowsNoEditor\\Engine.ini";
#if defined(WWMI_RESEARCH_PRESERVE_GAME_SETTINGS) && WWMI_RESEARCH_PRESERVE_GAME_SETTINGS
        ::logger::write("Config: automatic Engine.ini streaming writes skipped (research build preserves game settings)");
#else
        const wchar_t* cvars[][2] = {
            { L"r.Streaming.FullyLoadUsedTextures", L"1" }, { L"r.MipMapLODBias", L"-1" },
            { L"r.Streaming.Boost", L"30" }, { L"r.Streaming.MinBoost", L"30" },
            { L"r.Streaming.HLODStrategy", L"2" }
        };
        unsigned written = 0;
        bool     failed = false;
        for (const auto& kv : cvars) {
            wchar_t cur[32] = {};
            GetPrivateProfileStringW(L"SystemSettings", kv[0], L"", cur, (DWORD)std::size(cur), eng.c_str());
            if (wcscmp(cur, kv[1]) == 0) continue; // already set: the file is left untouched
            if (WritePrivateProfileStringW(L"SystemSettings", kv[0], kv[1], eng.c_str()))
                ++written;
            else
                failed = true;
        }
        ::logger::write("Config: Engine.ini full-res + streaming cvars %s (%u written) (%S)",
            failed ? "FAILED" : written ? "applied"
                                        : "already set",
            written, eng.c_str());
#endif
        g_cfg_engine_path = eng;
        g_cfg_db_path = std::wstring(win64) + L"..\\..\\Saved\\LocalStorage\\LocalStorage.db";
    }

    s_win64 = win64;
    reshade::register_event<reshade::addon_event::init_device>(on_init_device);
    ::logger::write("Init: AddonInit complete; inactive until a D3D12 device is created");
    ::logger::probe("RTCAPTURE addon_init_success=1 pid=%lu candidate_session=%s", GetCurrentProcessId(),
        rt_session_n && rt_session_n < std::size(rt_session) ? rt_session : "missing");
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE)
{
    MigotoPort::ModUiClearShield();
    StopDescriptorPublisher();
    if (s_infoq1) {
        if (s_infoq1_cookie) s_infoq1->UnregisterMessageCallback(s_infoq1_cookie);
        s_infoq1->Release();
        s_infoq1 = nullptr;
    }
    if (g_infoq) {
        g_infoq->Release();
        g_infoq = nullptr;
    } // never survive a probe unload
    PortApi::SaveSettingsOnExit();
    StallWatchdogStop(); // no thread of this DLL may outlive its mapping
    DredWatchdogStop();
    RfxPatchWorkerStop();
    g_heap_to_res.unwatch_all(); // a live heap's release must not call into this DLL once it is unmapped
    RtStopStructTexFallbackWorker();
    g_tex_lod_worker.Stop();
    TexCrawlFlushIfQuiet(0);
    TexLodOnExit();
    TexLookOnExit();
    g_tex_defer.Stop();
    g_tex_gc.Stop();
    g_tex_replace.Stop();
    CrashHandler::Uninstall(); // the process's previous filter comes back before this code is unmapped
    wwmi::binding_client::Shutdown(); // drains observation callbacks before the host unloads this addon
    reshade::unregister_addon(addon_module);
}
