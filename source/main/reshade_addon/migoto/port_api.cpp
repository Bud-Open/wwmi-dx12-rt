// ===========================================================================
//  port_api.cpp -- OURS. The addon side of the seam, speaking their engine's language.
//
//  Compiled with THEIR flags (see tools/build_migoto_lib.bat) so it can include their headers; the addon
//  only ever sees port_api.h, which mentions nothing of theirs.
//
//  Every function here is a thin call into their code. That is the point: [Present] is their
//  present_command_list, key handling is their DispatchInputEvents, the override dispatch is their
//  lookup_shaderoverride / lookup_textureoverride. Nothing is re-derived here -- when this file starts
//  containing logic instead of calls, the port has begun drifting again.
// ===========================================================================
#include "port_api.h"
#include "port_api_internal.h"

#include "globals.h"
#include "IniHandler.h"
#include "CommandList.h"
#include "ResourceHash.h"
#include "DrawCallInfo.h"
#include "Input.h"
#include "Override.h" // CurrentTransition -- the commit half of a keypress, see TickInput
#include "Overlay.h"
#include "HackerDevice.h"
#include "HackerContext.h"
#include "frame_analysis_engine.h" // FrameAnalysisContext -- g_ctx IS one
#include "port_keygate.h"
#include "dx12_context.h"
#include "dx12_pipeline.h"
#include "dx12_wrap.h"
#include "../../src/log.h"

#include <mutex>
#include <vector>
#include <set>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <atomic>
#include <unordered_set>
#include <unordered_map>
#include <share.h> // _SH_DENYNO -- their exact log-open flags (see the pre-open in Load)

namespace MigotoPort {
void InitEngineGlobals();
}

namespace PortApi {

HackerDevice         g_dev;
FrameAnalysisContext g_ctx{ nullptr, nullptr };
bool                 g_loaded = false;
static unsigned      g_constants_generation = ~0u;
unsigned             g_generation = 0;
static EngineValues  g_step_inputs;               // what the step's [Present] started from: a redraw runs from the same
static bool          g_step_inputs_taken = false; // ...for this load of the inis

FrameAnalysisContext* FrameAnalysisCtx()
{
    return &g_ctx;
}

std::recursive_mutex& EngineLock()
{
    static auto* m = new std::recursive_mutex();
    return *m;
}

bool Load(const wchar_t* ini_root)
{
    if (g_loaded) return true;

    MigotoPort::InitEngineGlobals();
    // Before LoadConfigFile: this is what it resolves d3dx.ini against.
    MigotoPort::SetIniRoot(ini_root);

    {
        wchar_t dir[MAX_PATH] = {};
        MigotoPort::MigotoDir(dir, MAX_PATH);
        wchar_t asseen[MAX_PATH] = {};
        PortRealGetModuleFileNameW(migoto_handle, asseen, MAX_PATH);
        logger::write("PORT-PATHS: ini_root='%S'  MigotoDir->'%S'  migoto_handle=%p  real module='%S'",
            ini_root ? ini_root : L"(null)", dir, (void*)migoto_handle, asseen);
    }
    g_dev.mHackerContext = &g_ctx;
    g_ctx.mHackerDevice = &g_dev;

    LoadConfigFile();
    RebuildParseSnapshots();

    g_dev.CreateIniParamResources();

    g_ctx.Bind3DMigotoResources();
    logger::write("PORT: IniParams t%d -- texture %p view %p (%d params reserved)",
        G->IniParamsReg, (void*)g_dev.mIniTexture, (void*)g_dev.mIniResourceView,
        G->iniParamsReserved);

    g_loaded = true;
    ++g_generation;
    g_step_inputs_taken = false;
    logger::write("PORT: engine loaded -- %zu command lists, %zu custom shaders, %zu resources, "
                  "%zu texture overrides, %zu shader overrides, %d ini params",
        registered_command_lists.size(), customShaders.size(), customResources.size(),
        G->mTextureOverrideMap.size(), G->mShaderOverrideMap.size(), G->iniParamsReserved);
    return true;
}

void SaveSettingsOnExit()
{
    if (!g_loaded) {
        logger::write("PORT-EXIT: no settings save -- engine not loaded (probe unload)");
        return;
    }
    if (!G->gConfigInitialized) {
        logger::write("PORT-EXIT: no settings save -- deferred config reload never ran, so the mod tree "
                      "was never parsed and every variable is still at its declared default");
        return;
    }
    if (g_constants_generation == ~0u) {
        logger::write("PORT-EXIT: no settings save -- [Constants] never applied, so d3dx_user.ini was "
                      "never read back into the variables it would be written from");
        return;
    }
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    logger::write("PORT-EXIT: saving persistent settings (dirty=%d)", G->user_config_dirty);
    SavePersistentSettings();
}

void SetFrameContext(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* queue,
    unsigned rt_width, unsigned rt_height,
    DXGI_FORMAT rt_format, DXGI_FORMAT ds_format,
    ID3D12Resource* back_buffer, D3D12_RESOURCE_STATES back_buffer_state,
    void (*restore_game_state)(void*), void*           restore_user)
{
    MigotoPort::Dx12Bridge b;
    b.restore_user = restore_user;
    b.device = dev;
    b.cl = cl;
    b.queue = queue;
    b.rt_width = rt_width;
    b.rt_height = rt_height;
    b.rt_format = rt_format;
    b.ds_format = ds_format;
    b.back_buffer = back_buffer;
    b.back_buffer_state = back_buffer_state;
    b.restore_game_state = restore_game_state;
    b.own_list = (cl != nullptr); // the [Present] bracket's list is ours
    MigotoPort::SetDx12Bridge(b);
    MigotoPort::BackBufferPublished(back_buffer, back_buffer_state);

    if (back_buffer && G->mResolutionInfo.from == GetResolutionFrom::SWAP_CHAIN) {
        const D3D12_RESOURCE_DESC rd = back_buffer->GetDesc();
        if (G->mResolutionInfo.width != (int)rd.Width || G->mResolutionInfo.height != (int)rd.Height) {
            G->mResolutionInfo.width = (int)rd.Width;
            G->mResolutionInfo.height = (int)rd.Height;
            logger::write("PORT: resolution from swap chain: %dx%d", G->mResolutionInfo.width,
                G->mResolutionInfo.height);
        }
    }
}

void SetGameWindow(void* hwnd)
{
    static std::atomic<void*> s_set{ nullptr };
    if (!hwnd || s_set.load(std::memory_order_relaxed) == hwnd) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    G->hWnd = (HWND)hwnd;
    s_set.store(hwnd, std::memory_order_relaxed);
    logger::write("PORT: game window %p -- cursor_x/y and window_width/height are taken from its client area", hwnd);
}

unsigned BridgeStackDepth()
{
    return MigotoPort::BridgeDepth();
}

DrawListScope::DrawListScope(ID3D12GraphicsCommandList* cl, void (*restore_game_state)(void*),
    void*                                               restore_user, int (*vs_cb_root)(void*, unsigned),
    void*                                               vs_cb_root_user)
{
    MigotoPort::Dx12Bridge b = MigotoPort::GetDx12Bridge();
    b.cl = cl;
    b.restore_game_state = restore_game_state;
    b.restore_user = restore_user;
    b.vs_cb_root = vs_cb_root;
    b.vs_cb_root_user = vs_cb_root_user;
    b.own_list = false; // a draw scope records onto the GAME's list -- inherit its state
    MigotoPort::BridgePush(b);
}

DrawListScope::~DrawListScope()
{
    MigotoPort::FlushUavBarriers(MigotoPort::GetDx12Bridge().cl);
    MigotoPort::BridgePop();
}

static std::atomic<unsigned> g_refused_no_cl{ 0 };
unsigned                     TakeRefusedNoCl()
{
    return g_refused_no_cl.exchange(0);
}

static bool HaveCommandList(const char* what)
{
    if (MigotoPort::GetDx12Bridge().cl) return true;
    if (!logger::enabled(1)) return false;
    const unsigned n = g_refused_no_cl.fetch_add(1);
    if ((n % 600u) == 0u)
        logger::write("PORT-REFUSE %s: no DX12 command list bound (occurrence %u). Running it would mutate "
                      "their variables while every dispatch/copy is discarded -- which is how the ini ends "
                      "up claiming work that never reached the GPU.",
            what, n + 1);
    return false;
}

static void RunConstantsOnce()
{
    if (g_constants_generation == g_generation) return;
    g_constants_generation = g_generation;

    RunCommandList(&g_dev, &g_ctx, &G->constants_command_list, nullptr, false);
    G->user_config_dirty = false;
    RunCommandList(&g_dev, &g_ctx, &G->post_constants_command_list, nullptr, true);

    logger::write("PORT: [Constants] applied (generation %u) -- persisted globals from d3dx_user.ini "
                  "are now live; hotkey toggles have a value to hold",
        g_generation);
}

static void FrameDriverClock()
{
    // Theirs: HackerSwapChain::RunFrameActions. fflush first so a crash loses at most one frame of their log.
    if (LogFile) fflush(LogFile);
    const uint64_t system_tick_count = GetSystemTicks();
    G->gFrameTime = (system_tick_count - G->gSystemTickCount) / 1'000'000.0f;
    G->gTime = (system_tick_count - G->ticks_at_launch) / 1'000'000.0f;
    G->gSystemTickCount = system_tick_count;
    G->gFPSCounter.Update(system_tick_count);
}

static void BumpGeneration()
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    volatile unsigned& gen = g_generation;
    gen = gen + 1;
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

static void BeginDeferredReload()
{
    // LoadConfigFile uses this flag to decide whether to parse the includes.
    // The callback runs only after the dirty-save precondition succeeds.
    G->gConfigInitialized = true;
    BumpGeneration();
}

static void FrameDriverConfig()
{
    // A failed delayed reload must not be retried and logged every frame.
    // An explicit reload request is the next retry opportunity.
    static bool deferred_reload_failed = false;
    if (G->gReloadConfigPending) {
        if (ReloadConfig(&g_dev, BumpGeneration)) {
            RebuildParseSnapshots();
            BumpGeneration();
            g_step_inputs_taken = false;
            MigotoPort::ClearSelectionVars();
            deferred_reload_failed = false;
        } else if (!G->gConfigInitialized) {
            deferred_reload_failed = true;
        }
    }

    if (G->gConfigInitialized) {
        if (G->gTime - G->gSettingsSaveTime > G->gSettingsAutoSaveInterval)
            SavePersistentSettings();
    } else if (!deferred_reload_failed && G->gTime > G->gConfigInitializationDelay) {
        logger::write("PORT-FRAME: deferred config reload firing at gTime=%.3f (delay=%d)",
            G->gTime, G->gConfigInitializationDelay);
        if (ReloadConfig(&g_dev, BeginDeferredReload)) {
            RebuildParseSnapshots();
            BumpGeneration();
            MigotoPort::ClearSelectionVars();
        } else {
            deferred_reload_failed = true;
        }
    }
}

void PresentPre()
{
    {
        std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
        if (g_loaded) FrameDriverClock();
    }
    if (!HaveCommandList("[Present] pre")) {
        std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
        if (g_loaded) FrameDriverConfig();
        return;
    }
    MigotoPort::UploadFrameTick(); // releases retired resources the GPU has passed (src/gpu_progress.h)
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    if (G->gReloadConfigPending) FrameDriverConfig();
    // Before the first [Present]: their load order is [Constants] then everything else.
    RunConstantsOnce();
    G->frame_no++;
    MigotoPort::SweepSelectionVars();
    g_step_inputs.Take();
    g_step_inputs_taken = true;
    RunCommandList(&g_dev, &g_ctx, &G->present_command_list, nullptr, false);
    RunSkelCopyUpdates();
    FrameDriverConfig();
    g_ctx.ResetCallCounters();
}

void PresentPost()
{
    if (!HaveCommandList("[Present] post")) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    RunCommandList(&g_dev, &g_ctx, &G->post_present_command_list, nullptr, true);
}

void PresentRedraw()
{
    if (!HaveCommandList("[Present] redraw")) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    EngineValues before;
    before.Take();
    if (g_step_inputs_taken) g_step_inputs.PutBack();
    MigotoPort::g_present_redraw = true;
    RunCommandList(&g_dev, &g_ctx, &G->present_command_list, nullptr, false);
    RunCommandList(&g_dev, &g_ctx, &G->post_present_command_list, nullptr, true);
    MigotoPort::g_present_redraw = false;
    before.PutBack();
    g_ctx.ResetCallCounters();
}

void SetKeyGate(bool (*fn)(const wchar_t* ns))
{
    MigotoPort::SetKeyGate(fn);
}

void TickInput()
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    DispatchInputEvents(&g_dev);

    CurrentTransition.UpdatePresets(&g_dev);
    CurrentTransition.UpdateTransitions(&g_dev);
    MigotoPort::SweepSelectionVars();
}

static bool IsPureStateCommandList(const CommandList& cl, int depth = 0)
{
    if (depth > 8) return false;
    for (const auto& cmd : cl.commands) {
        if (!cmd) continue;
        CommandListCommand* inner = MigotoPort::UnwrapObserved(cmd.get());
        if (!inner) continue;
        if (dynamic_cast<const AssignmentCommand*>(inner)) continue;
        if (const auto* ic = dynamic_cast<const IfCommand*>(inner)) {
            if (ic->true_commands_pre && !IsPureStateCommandList(*ic->true_commands_pre, depth + 1)) return false;
            if (ic->true_commands_post && !IsPureStateCommandList(*ic->true_commands_post, depth + 1)) return false;
            if (ic->false_commands_pre && !IsPureStateCommandList(*ic->false_commands_pre, depth + 1)) return false;
            if (ic->false_commands_post && !IsPureStateCommandList(*ic->false_commands_post, depth + 1)) return false;
            continue;
        }
        return false;
    }
    return true;
}

void RunSiblingHashMarkers(unsigned ib_hash)
{
    if (!ib_hash) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;

    static unsigned                                                    s_gen = ~0u;
    static std::unordered_map<unsigned, std::vector<TextureOverride*>> s_cache;
    if (s_gen != g_generation) {
        s_cache.clear();
        s_gen = g_generation;
    }

    auto cit = s_cache.find(ib_hash);
    if (cit == s_cache.end()) {
        std::vector<TextureOverride*> markers;
        auto                          it = G->mTextureOverrideMap.find(ib_hash);
        if (it != G->mTextureOverrideMap.end()) {
            for (TextureOverride& to : it->second) {
                if (to.has_draw_context_match) continue;
                if (to.format != -1 || to.width != -1 || to.height != -1 ||
                    to.override_byte_width != -1 || to.override_num_elements != -1)
                    continue;
                if (to.command_list.commands.empty() && to.post_command_list.commands.empty())
                    continue;
                if (!IsPureStateCommandList(to.command_list) || !IsPureStateCommandList(to.post_command_list))
                    continue;
                markers.push_back(&to);
            }
        }
        if (!markers.empty()) {
            logger::probe("WWMI-LOAD phase=sibling-hash-markers generation=%u ib_hash=%08x count=%zu first='%ls'",
                g_generation, ib_hash, markers.size(), markers.front()->ini_section.c_str());
        }
        cit = s_cache.emplace(ib_hash, std::move(markers)).first;
    }

    for (TextureOverride* to : cit->second) {
        if (!to->command_list.commands.empty())
            RunCommandList(&g_dev, &g_ctx, &to->command_list, nullptr, false);
        if (!to->post_command_list.commands.empty())
            RunCommandList(&g_dev, &g_ctx, &to->post_command_list, nullptr, true);
    }
}

namespace {

struct DdsFmtInfo {
    unsigned fmt = 0;
    unsigned w = 0;
    unsigned h = 0;
};

static DdsFmtInfo ReadDdsFmtCached(const CustomResource* cr, std::unordered_map<const CustomResource*, DdsFmtInfo>& memo)
{
    if (!cr) return {};
    if (auto it = memo.find(cr); it != memo.end()) return it->second;
    DdsFmtInfo info{};
    if (cr->resource) {
        if (auto* wt = dynamic_cast<MigotoPort::WrapTexture2D*>(cr->resource)) {
            info.fmt = (unsigned)wt->desc.Format;
            info.w = wt->desc.Width;
            info.h = wt->desc.Height;
        }
    }
    if (!info.fmt && cr->format != DXGI_FORMAT_UNKNOWN && cr->format != (DXGI_FORMAT)-1)
        info.fmt = (unsigned)cr->format;
    if ((!info.fmt || !info.w) && !cr->filename.empty()) {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, cr->filename.c_str(), L"rb") == 0 && fp) {
            unsigned char hdr[148]{};
            const size_t  nr = fread(hdr, 1, sizeof(hdr), fp);
            fclose(fp);
            if (nr >= 128 && hdr[0] == 'D' && hdr[1] == 'D' && hdr[2] == 'S' && hdr[3] == ' ') {
                uint32_t h = 0, w = 0;
                std::memcpy(&h, hdr + 12, 4);
                std::memcpy(&w, hdr + 16, 4);
                info.w = w;
                info.h = h;
                const char* fcc = reinterpret_cast<const char*>(hdr + 84);
                if (nr >= 148 && std::memcmp(fcc, "DX10", 4) == 0) {
                    uint32_t dxgi = 0;
                    std::memcpy(&dxgi, hdr + 128, 4);
                    info.fmt = dxgi;
                } else if (std::memcmp(fcc, "DXT1", 4) == 0) {
                    info.fmt = (unsigned)DXGI_FORMAT_BC1_UNORM;
                } else if (std::memcmp(fcc, "DXT3", 4) == 0) {
                    info.fmt = (unsigned)DXGI_FORMAT_BC2_UNORM;
                } else if (std::memcmp(fcc, "DXT5", 4) == 0) {
                    info.fmt = (unsigned)DXGI_FORMAT_BC3_UNORM;
                } else if (std::memcmp(fcc, "ATI1", 4) == 0 || std::memcmp(fcc, "BC4U", 4) == 0) {
                    info.fmt = (unsigned)DXGI_FORMAT_BC4_UNORM;
                } else if (std::memcmp(fcc, "ATI2", 4) == 0 || std::memcmp(fcc, "BC5U", 4) == 0) {
                    info.fmt = (unsigned)DXGI_FORMAT_BC5_UNORM;
                }
            }
        }
    }
    memo.emplace(cr, info);
    return info;
}

static bool ParsePsEqFilter(const std::wstring& line, float* out_f)
{
    if (!out_f || line.empty()) return false;
    auto is_tok_char = [](wchar_t c) {
        return iswalnum(c) || c == L'_' || c == L'.' || c == L'$' || c == L'\\' || c == L'-';
    };
    auto parse_float_exact = [](const std::wstring& tok, float* v) {
        if (tok.empty()) return false;
        wchar_t* end = nullptr;
        const float f = wcstof(tok.c_str(), &end);
        if (!end || *end != L'\0' || !std::isfinite(f)) return false;
        *v = f;
        return true;
    };
    const size_t n = line.size();
    for (size_t i = 0; i + 1 < n; ++i) {
        if (line[i] != L'=' || line[i + 1] != L'=') continue;
        if (i > 0 && (line[i - 1] == L'!' || line[i - 1] == L'=' || line[i - 1] == L'<' || line[i - 1] == L'>'))
            continue;
        if (i + 2 < n && line[i + 2] == L'=') continue;

        size_t le = i;
        while (le > 0 && iswspace(line[le - 1])) --le;
        size_t lb = le;
        while (lb > 0 && is_tok_char(line[lb - 1])) --lb;

        size_t rb = i + 2;
        while (rb < n && iswspace(line[rb])) ++rb;
        size_t re = rb;
        while (re < n && is_tok_char(line[re])) ++re;

        std::wstring lhs = line.substr(lb, le - lb);
        std::wstring rhs = line.substr(rb, re - rb);
        std::wstring llhs = lhs, lrhs = rhs;
        for (wchar_t& c : llhs) c = (wchar_t)towlower(c);
        for (wchar_t& c : lrhs) c = (wchar_t)towlower(c);

        float val = 0.f;
        if (llhs == L"ps" && parse_float_exact(rhs, &val)) {
            *out_f = val;
            return true;
        }
        if (lrhs == L"ps" && parse_float_exact(lhs, &val)) {
            *out_f = val;
            return true;
        }
    }
    return false;
}

struct PsFilterRule {
    float    filter_index = 0.f;
    uint64_t dx11_so_hash = 0;
    unsigned true_mask = 0;
    unsigned true_fmt[8]{};
    unsigned false_mask = 0;
    unsigned false_fmt[8]{};
};

struct PsSlotAccum {
    unsigned                                 mask = 0;
    std::vector<const CustomResource*>       res[8];
};

static void CollectIndirectResources(const CommandList& cl,
    std::unordered_map<const CustomResource*, std::vector<const CustomResource*>>& indirect,
    std::unordered_set<const CommandList*>& visited, int depth)
{
    if (depth > 16 || !visited.insert(&cl).second) return;
    for (const auto& cmd : cl.commands) {
        if (!cmd) continue;
        CommandListCommand* inner = MigotoPort::UnwrapObserved(cmd.get());
        if (!inner) continue;
        if (auto* r = dynamic_cast<RunExplicitCommandList*>(inner)) {
            if (r->command_list_section)
                CollectIndirectResources(r->command_list_section->command_list, indirect, visited, depth + 1);
        } else if (auto* l = dynamic_cast<RunLinkedCommandList*>(inner)) {
            if (l->link) CollectIndirectResources(*l->link, indirect, visited, depth + 1);
        } else if (auto* ic = dynamic_cast<IfCommand*>(inner)) {
            if (ic->true_commands_pre) CollectIndirectResources(*ic->true_commands_pre, indirect, visited, depth + 1);
            if (ic->false_commands_pre) CollectIndirectResources(*ic->false_commands_pre, indirect, visited, depth + 1);
        } else if (auto* rc = dynamic_cast<ResourceCopyOperation*>(inner)) {
            if (rc->dst.type == ResourceCopyTargetType::CUSTOM_RESOURCE && !rc->dst.custom_resource_pool &&
                rc->src.type == ResourceCopyTargetType::CUSTOM_RESOURCE && !rc->src.custom_resource_pool) {
                CustomResource* dst = rc->dst.GetCustomResource(nullptr);
                CustomResource* src = rc->src.GetCustomResource(nullptr);
                if (dst && src && dst != src) indirect[dst].push_back(src);
            }
        }
    }
}

static void CollectPsSlotBinds(const CommandList& cl, PsSlotAccum& out,
    std::unordered_set<const CommandList*>& visited, int depth)
{
    if (depth > 12 || !visited.insert(&cl).second) return;
    for (const auto& cmd : cl.commands) {
        if (!cmd) continue;
        CommandListCommand* inner = MigotoPort::UnwrapObserved(cmd.get());
        if (!inner) continue;
        if (auto* r = dynamic_cast<RunExplicitCommandList*>(inner)) {
            if (r->command_list_section)
                CollectPsSlotBinds(r->command_list_section->command_list, out, visited, depth + 1);
        } else if (auto* l = dynamic_cast<RunLinkedCommandList*>(inner)) {
            if (l->link) CollectPsSlotBinds(*l->link, out, visited, depth + 1);
        } else if (auto* ic = dynamic_cast<IfCommand*>(inner)) {
            if (ic->true_commands_pre) CollectPsSlotBinds(*ic->true_commands_pre, out, visited, depth + 1);
            if (ic->false_commands_pre) CollectPsSlotBinds(*ic->false_commands_pre, out, visited, depth + 1);
        } else if (auto* rc = dynamic_cast<ResourceCopyOperation*>(inner)) {
            if (rc->dst.type == ResourceCopyTargetType::SHADER_RESOURCE &&
                rc->dst.shader_type == L'p' && !rc->dst.slot_expression && rc->dst.slot < 8u &&
                rc->src.type == ResourceCopyTargetType::CUSTOM_RESOURCE && !rc->src.custom_resource_pool) {
                out.mask |= (1u << rc->dst.slot);
                if (CustomResource* src = rc->src.GetCustomResource(nullptr))
                    out.res[rc->dst.slot].push_back(src);
            }
        }
    }
}

static unsigned ResolveSlotFmt(const std::vector<const CustomResource*>& resources,
    const std::unordered_map<const CustomResource*, std::vector<const CustomResource*>>& indirect,
    std::unordered_map<const CustomResource*, DdsFmtInfo>& dds_memo)
{
    std::unordered_map<unsigned, int> votes_real, votes_any;
    std::vector<const CustomResource*> stack = resources;
    std::unordered_set<const CustomResource*> seen;
    while (!stack.empty()) {
        const CustomResource* cr = stack.back();
        stack.pop_back();
        if (!cr || !seen.insert(cr).second) continue;
        const DdsFmtInfo di = ReadDdsFmtCached(cr, dds_memo);
        if (di.fmt) {
            votes_any[di.fmt]++;
            if (di.w >= 64u && di.h >= 64u) votes_real[di.fmt]++;
        }
        if (auto it = indirect.find(cr); it != indirect.end()) {
            for (const CustomResource* s : it->second) stack.push_back(s);
        }
    }
    const auto& pick = !votes_real.empty() ? votes_real : votes_any;
    unsigned best_fmt = 0;
    int      best_cnt = 0;
    for (const auto& kv : pick) {
        if (kv.second > best_cnt) {
            best_cnt = kv.second;
            best_fmt = kv.first;
        }
    }
    return best_fmt;
}

static void ExtractPsFilterRules(const CommandList& cl,
    const std::unordered_map<uint32_t, std::pair<float, uint64_t>>& filter_map,
    const std::unordered_map<const CustomResource*, std::vector<const CustomResource*>>& indirect,
    std::unordered_map<const CustomResource*, DdsFmtInfo>& dds_memo,
    std::vector<PsFilterRule>& out_rules,
    std::unordered_set<uint32_t>& out_tested_filters,
    std::unordered_set<const CommandList*>& visited, int depth)
{
    if (depth > 16 || !visited.insert(&cl).second) return;
    for (const auto& cmd : cl.commands) {
        if (!cmd) continue;
        CommandListCommand* inner = MigotoPort::UnwrapObserved(cmd.get());
        if (!inner) continue;
        if (auto* r = dynamic_cast<RunExplicitCommandList*>(inner)) {
            if (r->command_list_section)
                ExtractPsFilterRules(r->command_list_section->command_list, filter_map, indirect, dds_memo, out_rules, out_tested_filters, visited, depth + 1);
        } else if (auto* l = dynamic_cast<RunLinkedCommandList*>(inner)) {
            if (l->link)
                ExtractPsFilterRules(*l->link, filter_map, indirect, dds_memo, out_rules, out_tested_filters, visited, depth + 1);
        } else if (auto* ic = dynamic_cast<IfCommand*>(inner)) {
            float f = 0.f;
            if (ParsePsEqFilter(ic->ini_line, &f)) {
                uint32_t fbits = 0;
                std::memcpy(&fbits, &f, 4);
                out_tested_filters.insert(fbits);
                if (auto fit = filter_map.find(fbits); fit != filter_map.end()) {
                    PsSlotAccum ta{}, fa{};
                    if (ic->true_commands_pre) {
                        std::unordered_set<const CommandList*> sv;
                        CollectPsSlotBinds(*ic->true_commands_pre, ta, sv, 0);
                    }
                    if (ic->false_commands_pre) {
                        std::unordered_set<const CommandList*> sv;
                        CollectPsSlotBinds(*ic->false_commands_pre, fa, sv, 0);
                    }
                    if (ta.mask != 0) {
                        PsFilterRule rule{};
                        rule.filter_index = fit->second.first;
                        rule.dx11_so_hash = fit->second.second;
                        rule.true_mask = ta.mask;
                        rule.false_mask = fa.mask;
                        for (unsigned r = 0; r < 8; ++r) {
                            if (ta.mask & (1u << r)) rule.true_fmt[r] = ResolveSlotFmt(ta.res[r], indirect, dds_memo);
                            if (fa.mask & (1u << r)) rule.false_fmt[r] = ResolveSlotFmt(fa.res[r], indirect, dds_memo);
                        }
                        out_rules.push_back(rule);
                    }
                }
            }
            if (ic->true_commands_pre)
                ExtractPsFilterRules(*ic->true_commands_pre, filter_map, indirect, dds_memo, out_rules, out_tested_filters, visited, depth + 1);
            if (ic->false_commands_pre)
                ExtractPsFilterRules(*ic->false_commands_pre, filter_map, indirect, dds_memo, out_rules, out_tested_filters, visited, depth + 1);
        }
    }
}

struct SectionPsRuleSet {
    std::vector<PsFilterRule>    rules;
    std::unordered_set<uint32_t> tested_filters;
};

struct PsFilterBridgeState {
    unsigned                                                     gen = ~0u;
    std::unordered_map<uint32_t, std::pair<float, uint64_t>>     filter_to_so;
    std::unordered_map<const CustomResource*, DdsFmtInfo>        dds_memo;
    std::unordered_map<std::wstring, SectionPsRuleSet>           section_rules;

    void RefreshForGen(unsigned cur_gen)
    {
        if (gen == cur_gen) return;
        gen = cur_gen;
        filter_to_so.clear();
        dds_memo.clear();
        section_rules.clear();
        for (const auto& kv : G->mShaderOverrideMap) {
            const float fi = kv.second.filter_index;
            if (fi == FLT_MAX || !std::isfinite(fi)) continue;
            uint32_t fbits = 0;
            std::memcpy(&fbits, &fi, 4);
            filter_to_so.emplace(fbits, std::make_pair(fi, kv.first));
        }
    }

    const SectionPsRuleSet& RuleSetForSection(const wchar_t* fq_section)
    {
        static const SectionPsRuleSet kEmpty;
        if (filter_to_so.empty() || !fq_section || !*fq_section) return kEmpty;
        std::wstring key(fq_section);
        auto         it = section_rules.find(key);
        if (it != section_rules.end()) return it->second;

        SectionPsRuleSet rs{};
        auto&            sidx = SectionIndex();
        TextureOverride* to = nullptr;
        if (auto sit = sidx.find(key); sit != sidx.end()) {
            to = sit->second;
        } else {
            std::wstring lkey = key;
            for (wchar_t& c : lkey) c = (wchar_t)towlower(c);
            if (auto sit2 = sidx.find(lkey); sit2 != sidx.end()) to = sit2->second;
        }
        if (to) {
            std::unordered_map<const CustomResource*, std::vector<const CustomResource*>> indirect;
            {
                std::unordered_set<const CommandList*> vis;
                CollectIndirectResources(to->command_list, indirect, vis, 0);
            }
            {
                std::unordered_set<const CommandList*> vis;
                ExtractPsFilterRules(to->command_list, filter_to_so, indirect, dds_memo, rs.rules, rs.tested_filters, vis, 0);
            }
        }
        return section_rules.emplace(std::move(key), std::move(rs)).first->second;
    }
};

static PsFilterBridgeState& PsBridgeState()
{
    static PsFilterBridgeState s;
    return s;
}

static bool IsSrgbDxgi(unsigned f)
{
    return f == 29u || f == 72u || f == 75u || f == 78u || f == 91u || f == 99u;
}

} // namespace

bool SectionHasPsFilterRules(const wchar_t* fq_section)
{
    if (!fq_section || !*fq_section) return false;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return false;
    auto& st = PsBridgeState();
    st.RefreshForGen(g_generation);
    return !st.RuleSetForSection(fq_section).rules.empty();
}

bool SectionTestsPsFilter(const wchar_t* fq_section, float filter_index)
{
    if (!fq_section || !*fq_section || !std::isfinite(filter_index)) return false;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return false;
    auto& st = PsBridgeState();
    st.RefreshForGen(g_generation);
    uint32_t fbits = 0;
    std::memcpy(&fbits, &filter_index, 4);
    return st.RuleSetForSection(fq_section).tested_filters.count(fbits) != 0;
}

bool ResolvePsFilterBridge(const wchar_t* fq_section,
    unsigned ps_declared_mask, bool ps_declared_known, unsigned rtv_count,
    const unsigned live_vfmt[8], const unsigned live_w[8], const unsigned live_h[8],
    uint64_t* out_dx11_so_hash, float* out_filter_index, bool* out_slots_ready)
{
    if (out_dx11_so_hash) *out_dx11_so_hash = 0;
    if (out_filter_index) *out_filter_index = 0.f;
    if (out_slots_ready) *out_slots_ready = true;
    if (!fq_section || !*fq_section) return false;

    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return false;
    auto& st = PsBridgeState();
    st.RefreshForGen(g_generation);
    const auto& rules = st.RuleSetForSection(fq_section).rules;
    if (rules.empty()) return false;

    bool     all_needed_ready = true;
    bool     have_match = false;
    bool     ambiguous = false;
    float    matched_fi = 0.f;
    uint64_t matched_so = 0;

    for (const PsFilterRule& R : rules) {
        if (!R.true_mask) continue;
        if (ps_declared_known && (ps_declared_mask & R.true_mask) != R.true_mask) continue;

        bool rule_match = false;
        if (R.false_mask != 0) {
            // Explicit if / else pass split (e.g., 1-slot BackFace/Cutout vs 4-slot PBR G-Buffer).
            if (ps_declared_known && (ps_declared_mask & R.false_mask) != R.false_mask) {
                bool conflict = false;
                for (unsigned r = 0; r < 8; ++r) {
                    if (!(R.true_mask & (1u << r))) continue;
                    if (R.true_fmt[r] && live_vfmt[r] &&
                        R.true_fmt[r] != live_vfmt[r] &&
                        IsSrgbDxgi(R.true_fmt[r]) != IsSrgbDxgi(live_vfmt[r])) {
                        conflict = true;
                        break;
                    }
                }
                rule_match = !conflict;
            } else {
                int th = 0, tm = 0, fh = 0, fm = 0;
                for (unsigned r = 0; r < 8; ++r) {
                    if (R.true_mask & (1u << r)) {
                        if (!live_vfmt[r]) {
                            all_needed_ready = false;
                        } else if (R.true_fmt[r]) {
                            if (live_vfmt[r] == R.true_fmt[r]) ++th;
                            else ++tm;
                        }
                    }
                    if (R.false_mask & (1u << r)) {
                        if (!live_vfmt[r]) {
                            all_needed_ready = false;
                        } else if (R.false_fmt[r]) {
                            if (live_vfmt[r] == R.false_fmt[r]) ++fh;
                            else ++fm;
                        }
                    }
                }
                if (th > 0 && tm == 0 && (fm > 0 || th > fh)) rule_match = true;
            }
        } else {
            // Guarded pass-specific override (e.g., Face Diffuse pass binding ps-t1 = BC7_UNORM_SRGB).
            if (rtv_count < 2u) continue;
            bool ok = true;
            int  exact_fmt_hits = 0;
            for (unsigned r = 0; r < 8; ++r) {
                if (!(R.true_mask & (1u << r))) continue;
                if (!live_vfmt[r]) {
                    all_needed_ready = false;
                    ok = false;
                    break;
                }
                if (live_w[r] < 64u || live_h[r] < 64u) {
                    ok = false;
                    break;
                }
                if (R.true_fmt[r]) {
                    if (live_vfmt[r] == R.true_fmt[r]) ++exact_fmt_hits;
                    else {
                        ok = false;
                        break;
                    }
                }
            }
            if (ok && exact_fmt_hits > 0) rule_match = true;
        }

        if (rule_match) {
            if (!have_match) {
                have_match = true;
                matched_fi = R.filter_index;
                matched_so = R.dx11_so_hash;
            } else if (matched_fi != R.filter_index) {
                ambiguous = true;
                break;
            }
        }
    }

    if (out_slots_ready) *out_slots_ready = all_needed_ready;
    if (have_match && !ambiguous) {
        if (out_dx11_so_hash) *out_dx11_so_hash = matched_so;
        if (out_filter_index) *out_filter_index = matched_fi;
        return true;
    }
    return false;
}

EngineScope::EngineScope()
{
    EngineLock().lock();
}
EngineScope::~EngineScope()
{
    EngineLock().unlock();
}

unsigned LoadGeneration()
{
    return g_generation;
}
long WrapResourcesCreated()
{
    long c = 0;
    long d = 0;
    long l = 0;
    MigotoPort::WrapStats(&c, &d, &l);
    return c;
}
long WrapChurn()
{
    long c = 0;
    long d = 0;
    MigotoPort::WrapStats(&c, &d, nullptr);
    return c + d;
}
unsigned SelectionInputsGen()
{
    return MigotoPort::SelectionInputsGen();
}
bool SelectionCacheOk()
{
    return g_loaded && !G->analyse_frame && !MigotoPort::AnyObserver();
}

Stats GetStats()
{
    Stats s;
    s.loaded = g_loaded;
    if (!g_loaded) return s;
    s.command_lists = registered_command_lists.size();
    s.custom_shaders = customShaders.size();
    s.custom_resources = customResources.size();
    s.texture_overrides = G->mTextureOverrideMap.size();
    s.shader_overrides = G->mShaderOverrideMap.size();
    s.ini_params = G->iniParamsReserved;
    return s;
}

void GetGpuWorkTotals(GpuWorkTotals* out)
{
    if (!out) return;
    out->iniparams_updates = Profiling::iniparams_updates;
    out->full_copies = Profiling::resource_full_copies;
    out->reference_copies = Profiling::resource_reference_copies;
    out->region_copies = Profiling::buffer_region_copies;
    out->inter_device_copies = Profiling::inter_device_copies;
    out->msaa_resolutions = Profiling::msaa_resolutions;
    out->views_cleared = Profiling::views_cleared;
    out->resources_created = Profiling::resources_created;
    out->pool_swaps = Profiling::resource_pool_swaps;
    out->max_copies_exceeded = Profiling::max_copies_per_frame_exceeded;
    out->injected_draws = Profiling::injected_draw_calls;
    out->max_executions_exceeded = Profiling::max_executions_per_frame_exceeded;
    ReplayCounts(&out->replay_top_replayed, &out->replay_top_recorded, &out->replay_top_plain,
        &out->replay_sub_replayed, &out->replay_sub_recorded, &out->replay_sub_plain);
}

} // namespace PortApi

namespace PortApi {
bool RtPackConfigReady() {
    std::lock_guard<std::recursive_mutex> lock(EngineLock());
    return g_loaded && G->gConfigInitialized && g_constants_generation==g_generation;
}
void RequestReload() {
    std::lock_guard<std::recursive_mutex> lock(EngineLock());
    if (!g_loaded) return;
    G->gWipeUserConfig = false;
    G->gReloadConfigPending = true;
}
}
