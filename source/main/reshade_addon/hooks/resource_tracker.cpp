// Resource, view and descriptor tracking: the shadow of the game's own bookkeeping.
// See resource_tracker.h.
#include "../mods/ib_hash.h"
#include "../addon_common.h"
#include "resource_tracker.h"
#include "rt_previous_descriptor.h"
#include "rt_aux_descriptor.h"
#include "rt_pack_frame.h"
#include "rt_cpu_profile.hpp"
#include "rt_cpu_phase.hpp"
#include "wwmi_descriptor_publish.h"
#include "descriptor_publish_main.hpp"
#include "published_descriptor.h"
#include "rt_material_replica.hpp"
#include <array>
#include <bit>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <variant>
#include <utility>
#include "../instrumentation/uav_observe.h"
#include "../textures/texture_module.h"     // kTexHashFloorPx
#include "../instrumentation/diagnostics.h" // g_texnoswap_arm / _n / _gen -- TEXNOSWAP
#include "../instrumentation/removal_pin.h" // DEAD-REF: a game buffer we named, destroyed before our list executed
#include "../instrumentation/hot_path.h"
#include "../mods/claim_state.h" // ForgetCharMeshVb0 / IbAnchorForget -- what is learned inside a buffer
#include "../rt/rt_hooks.h"      // RtForgetSkinOutRange
#include "../rt/rt_capture.h"    // diagnostic-only CPU shadow tracking while RT blocks mods
#include "../../src/mod_loader.h" // TexTouchRes
#include <unordered_set>
#include <cstring>
#include <optional>
#include <wrl/client.h>

// Buffer VA -> resource (for VA-based lookups in the bind hooks).
std::mutex                             g_mem_mutex; // declared in resource_tracker.h
std::unordered_map<uint64_t, ResInfo>  g_va_to_res; // (the chain resolves VAs too)
std::unordered_map<uint64_t, uint64_t> g_res_to_va;

#include "descriptor_native_access.inc"
#include "descriptor_publish_core.inc"

bool BufferLive(const void* res)
{
    std::lock_guard lock(g_mem_mutex);
    return g_res_to_va.find(reinterpret_cast<uint64_t>(res)) != g_res_to_va.end();
}

static bool IsBackBufferSized(command_list* cmd_list, resource res)
{
    const resource_desc d = cmd_list->get_device()->get_resource_desc(res);
    return d.type == resource_type::texture_2d && d.texture.width == g_bb_width.load(std::memory_order_relaxed) &&
        d.texture.height == g_bb_height.load(std::memory_order_relaxed);
}

void on_barrier(command_list* cmd_list, uint32_t count, const resource* res, [[maybe_unused]] const resource_usage* old_s,
    const resource_usage* new_s)
{
    PROF(PROF_Barrier);
    HotPath::GpuNoteBarriers(count);
    CmdBlock* const blk = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    for (uint32_t i = 0; i < count; ++i)
        if (res[i].handle) {
            if (blk && new_s[i] == resource_usage::general && g_frame_end_armed.load(std::memory_order_relaxed) &&
                IsBackBufferSized(cmd_list, res[i]))
                blk->frame_end_gen = blk->gen;
#ifdef WWMI_DEBUG
            if (blk && (new_s[i] == resource_usage::general || old_s[i] == resource_usage::general) &&
                g_present_log_left.load(std::memory_order_relaxed) > 0 && IsBackBufferSized(cmd_list, res[i])) {
                if (blk->bb_notes_gen != blk->gen) {
                    blk->bb_notes.clear();
                    blk->bb_notes_gen = blk->gen;
                }
                blk->bb_notes.push_back({ res[i].handle, (uint32_t)old_s[i], (uint32_t)new_s[i] });
            }
#endif
            g_res_state.put(res[i].handle, (uint32_t)new_s[i]);
            if (blk && ((uint32_t)new_s[i] & (uint32_t)resource_usage::unordered_access)) {
                auto&  v = blk->uav_recent;
                size_t old = 0;
                while (old < v.size() && v[old].frame + 1 < g_frame_id) ++old;
                if (old) v.erase(v.begin(), v.begin() + old);
                v.push_back(UavNote{ res[i].handle, g_frame_id });
            }
        }
}

uint32_t ResourceStateOf(void* res)
{
    if (ModLoader::TexIndexOf(reinterpret_cast<ID3D12Resource*>(res)) >= 0) return (uint32_t)D3D12_RESOURCE_STATE_COMMON;
    uint32_t state = 0;
    if (g_res_state.find(reinterpret_cast<uint64_t>(res), state)) return state;
    return MigotoPort::kResStateUnknown;
}

bool on_create_resource(device*, resource_desc& desc, subresource_data*, resource_usage)
{
    if (g_is_dx12 && !g_addon_disabled &&
        desc.type == resource_type::buffer &&
        (desc.usage & resource_usage::unordered_access) != resource_usage::unordered_access) {
        const uint64_t sz = desc.buffer.size;
        if (sz && sz <= 0xFFFFFFFFull) {
            const unsigned h = PortApi::GameComputeUavBufferHash((unsigned)sz);
            if (h && PortApi::ShapeKeyCallbackDeclaresHash(h)) {
                desc.usage |= resource_usage::unordered_access;
                if (ProbeArmed() && DiagOnce("UAVFLAG", (int)h))
                    logger::write("UAVFLAG: buffer %llu B (desc %08x) created WITHOUT "
                                  "ALLOW_UNORDERED_ACCESS -- added it, so WWMI's atomic_iadd into u0 has "
                                  "a valid view. Size and placement unchanged.",
                        (unsigned long long)sz, h);
                return true; // desc modified
            }
        }
    }

    return false;
}

bool TexResGenKnownLocked(uint64_t res);
void TexNoteResourceInitLocked(uint64_t res);
void TexNoteResourceDestroyLocked(uint64_t res);

void on_init_resource(device* device, const resource_desc& desc, const subresource_data* init_data, resource_usage, resource res)
{
    PreviousRegisterResource(device,res.handle,desc.type==resource_type::buffer?desc.buffer.size:0); // native descriptor identity, before the Mod guard
    if (g_addon_disabled) {
        if (g_rt_blocked && !g_faulted && desc.type == resource_type::texture_2d) {
            {
                std::lock_guard lock(g_tex_mutex);
                g_tex_dims[res.handle] = { desc.texture.width, desc.texture.height, (uint32_t)desc.texture.format,
                    desc.texture.levels ? desc.texture.levels : 1u,
                    desc.texture.depth_or_layers ? desc.texture.depth_or_layers : 1u,
                    (uint32_t)(desc.usage & (resource_usage::render_target | resource_usage::depth_stencil | resource_usage::unordered_access)) == 0 &&
                        (uint64_t)desc.texture.width * desc.texture.height >= kTexHashFloorPx };
                TexNoteResourceInitLocked(res.handle);
            }
            if (init_data && init_data[0].data && res.handle)
                TexHashFromUpload(reinterpret_cast<ID3D12Resource*>(res.handle), (const uint8_t*)init_data[0].data, init_data[0].row_pitch);
        }
        return;
    }
    g_init_res_all.fetch_add(1, std::memory_order_relaxed);
    if (desc.type == resource_type::buffer) {
        UavObserve::g_probe.resource_create(res.handle);
        ID3D12Resource* d3d12_res = reinterpret_cast<ID3D12Resource*>(res.handle);
        if (d3d12_res) {
            uint64_t va = d3d12_res->GetGPUVirtualAddress();
            if (va != 0) {
                g_init_res_buf.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard lock(g_mem_mutex);
                g_va_to_res[va] = { (uint32_t)desc.buffer.size, d3d12_res, va };
                g_va_to_res[va].buffer_width = desc.buffer.size;
                g_res_to_va[res.handle] = va;
            }
        }
    } else if (desc.type == resource_type::texture_2d) {
        {
            std::lock_guard lock(g_tex_mutex);
            g_tex_dims[res.handle] = { desc.texture.width, desc.texture.height, (uint32_t)desc.texture.format,
                desc.texture.levels ? desc.texture.levels : 1u,
                desc.texture.depth_or_layers ? desc.texture.depth_or_layers : 1u, // mips/arr for RfxTexClass
                (uint32_t)(desc.usage & (resource_usage::render_target | resource_usage::depth_stencil | resource_usage::unordered_access)) == 0 &&
                    (uint64_t)desc.texture.width * desc.texture.height >= kTexHashFloorPx };
            TexNoteResourceInitLocked(res.handle);
        }
        if (g_debug) {
            uint32_t tf = (uint32_t)desc.texture.format;
            uint32_t tw = desc.texture.width;
            uint32_t th = desc.texture.height;
            if (DxgiIsBc(tf) && (uint64_t)tw * th >= 256ull * 256ull) {
                static ProbeBudget sn;
                if (sn.take(601))
                    logger::trace("STREAMNEW res=%llx %ux%u mips=%u fmt=%u initData=%d",
                        (unsigned long long)res.handle, tw, th, desc.texture.levels, tf, (int)(init_data != nullptr));
            }
        }
        if (init_data && init_data[0].data && res.handle)
            TexHashFromUpload(reinterpret_cast<ID3D12Resource*>(res.handle), (const uint8_t*)init_data[0].data, init_data[0].row_pitch);
    }
}

bool on_copy_buffer_region(command_list*, resource, uint64_t, resource dest, uint64_t dest_offset, uint64_t size)
{
    PROF(PROF_CopyBufRegion);
    if (!dest.handle || !size) return false;
    if (const uint64_t va = reinterpret_cast<ID3D12Resource*>(dest.handle)->GetGPUVirtualAddress())
        IbHash::ForgetRange(va + dest_offset, size); // a mesh streamed over this region replaces the hashes taken inside it
    return false;
}

void on_destroy_resource(device*, resource res)
{
    PreviousForgetResource(res.handle); // metadata only; never AddRef a dying native object
    UavObserve::g_probe.resource_destroy(res.handle);
    RemovalPin::OnDestroyResource(reinterpret_cast<const void*>(res.handle));
    uint64_t gone_va = 0;
    uint64_t gone_bytes = 0;
    {
        std::lock_guard lock(g_mem_mutex);
        auto            it = g_res_to_va.find(res.handle);
        if (it != g_res_to_va.end()) {
            // Record BEFORE erasing: this is the only moment the handle is known to have been ours.
            MorphDump::NoteResourceErased(res.handle);
            gone_va = it->second;
            auto vi = g_va_to_res.find(gone_va);
            if (vi != g_va_to_res.end()) gone_bytes = vi->second.size;
            g_va_to_res.erase(it->second);
            g_res_to_va.erase(it);
        }
    }
    ForgetCharMeshVb0(gone_va, gone_bytes);    // a learned character VB0 dies with its buffer
    IbHash::ForgetRange(gone_va, gone_bytes);  // and so do the mesh hashes taken inside it
    IbAnchorForget(gone_va, gone_bytes);       // and the index-buffer anchor learned inside it
    RtForgetSkinOutRange(gone_va, gone_bytes); // and the RT skin rings inside it
    if (gone_va && gone_bytes) {
        std::lock_guard l(g_skincache_mtx); // and the skin-cache outputs
        std::erase_if(g_skincache_out, [&](const SkinOut& o) { return o.va >= gone_va && o.va < gone_va + gone_bytes; });
    }
    bool tex_known;
    {
        std::shared_lock l(g_tex_mutex);
        tex_known = g_res_texhash.count(res.handle) || g_modtex_res.count(res.handle) ||
            g_res_inherited.count(res.handle) || g_tex_dims.count(res.handle) ||
            TexResGenKnownLocked(res.handle);
    }
    if (tex_known) {
        std::lock_guard l(g_tex_mutex);
        extern bool     TexResHashEraseLocked(uint64_t key);
        TexResHashEraseLocked(res.handle);
        g_modtex_res.erase(res.handle);
        g_res_inherited.erase(res.handle);
        g_tex_dims.erase(res.handle);
        TexNoteResourceDestroyLocked(res.handle);
    }
    TexMipIdentityForget(res.handle);
    {
        std::lock_guard l(g_res_to_mod_mtx);
        g_res_to_mod_srv.erase(res.handle);
    }
    if (g_reloc_forget_on_destroy) g_buffer_reloc.ForgetResource((uint64_t)res.handle);
    g_res_to_slot.erase(res.handle); // re-stamp must not target a reused ptr's old slot
    g_res_state.erase(res.handle);
    SkInvalidateSnapshotFor(reinterpret_cast<ID3D12Resource*>(res.handle));
    ForgetGameCB(reinterpret_cast<ID3D12Resource*>(res.handle));
}

void on_init_resource_view(device* device, resource res, resource_usage usage, const resource_view_desc& desc, resource_view view)
{
    PROF(PROF_InitView);
    g_init_view_all.fetch_add(1, std::memory_order_relaxed);
    if (!g_is_dx12 || view.handle == 0) return;
    // CBV/SRV/UAV state and its probe are published by the real native
    // write. This SDK event keeps the view count and texture diagnostic.
    (void)device;
    (void)usage;
    if (res.handle == 0) return;

    if (g_debug && desc.type == resource_view_type::texture_2d) {
        uint32_t tw = 0;
        uint32_t th = 0;
        uint32_t tf = 0;
        {
            TexInfo di{};
            if (TexDimsOf(res.handle, &di)) {
                tw = di.width;
                th = di.height;
                tf = di.format;
            }
        }
        if (DxgiIsBc(tf) && (uint64_t)tw * th >= 256ull * 256ull) {
            static ProbeBudget sv;
            if (sv.take(601))
                logger::trace("STREAMSRV res=%llx %ux%u fmt=%u firstMip=%u mipCount=%u",
                    (unsigned long long)res.handle, tw, th, tf, desc.texture.first_level, desc.texture.levels);
        }
    }
}

static void TexNoSwapNote(uint64_t heap, uint32_t off, ID3D12Resource* res, const char* how)
{
    TexInfo di{};
    if (!res || !TexDimsOf(reinterpret_cast<uint64_t>(res), &di) || !di.hashable) return;
    const uint32_t h = TexHashOf(reinterpret_cast<uint64_t>(res));
    const uint32_t rule = TexRuleHashFor(h);
    if (h && !rule) return;
    bool shaped = false;
    {
        std::lock_guard l(g_match_mtx);
        for (uint32_t k = 0; k <= 4 && !shaped; ++k)
            shaped = g_match_profiles.count((((uint64_t)di.width << k) << 40) | (((uint64_t)di.height << k) << 16) |
                         (di.format & 0xffffu)) != 0;
    }
    if (!shaped) return;
    {
        static std::mutex                   m;
        static std::unordered_set<uint64_t> seen;
        static unsigned                     gen = ~0u;
        std::lock_guard                     l(m);
        const unsigned                      g = g_texnoswap_gen.load(std::memory_order_relaxed);
        if (gen != g) {
            gen = g;
            seen.clear();
        }
        if (!seen.insert(reinterpret_cast<uint64_t>(res)).second) return;
    }
    if (g_texnoswap_n.fetch_add(1, std::memory_order_relaxed) >= 400) return;
    logger::write("TEXNOSWAP f=%u heap=%llx slot=%u by %s: res=%p %ux%u m%u fmt=%u hash=%08x rule=%08x -- %s",
        g_frame_id, (unsigned long long)heap, off, how, (void*)res, di.width, di.height, di.mips, di.format,
        h, rule, !h ? "no identity yet" : "a target, no mod SRV selected");
}

#include "descriptor_publish_views.inc"

bool TrackedSlotOfCpu(uint64_t cpu, HeapKey* out)
{
    return g_heap_to_res.find_cpu_entry(cpu, CurrentDescriptorIncrement(), out) != nullptr;
}

void ResetDescriptorTracking()
{
    PreviousReset();
    g_res_to_slot.erase_if([](const HeapKey&) { return true; });
    logger::write("A1: addon re-enabled -- descriptor slot records cleared (stale while bypassed); they refill as the game writes descriptors");
}

bool UnstampResource(uint64_t resH, bool log)
{
    if (!g_d3d12_device || !resH) return false;
    HeapKey key{ 0, 0 };
    if (!g_res_to_slot.find(resH, key)) return false;
    if (!key.heap) return false;
    ResInfo ri;
    if (!ReadPublishedDescriptor(key, ri)) return false;
    if (ri.res != reinterpret_cast<ID3D12Resource*>(resH)) return false; // slot reused
    return UnstampSlot(key.heap, key.offset, log);
}

#include "descriptor_publish_stamp.inc"
#include "descriptor_consumers.inc"

std::vector<std::pair<HeapKey, uint64_t>> StampedCpuOnlySlots()
{
    std::vector<std::pair<HeapKey, uint64_t>> out;
    ForEachPublishedStamp([&](uint64_t h,bool vis,uint32_t o,ID3D12Resource*,uint64_t) {
        ResInfo ri;
        if (!vis && ReadPublishedDescriptor({ h, o }, ri)) out.push_back({ HeapKey{ h, o }, reinterpret_cast<uint64_t>(ri.res) });
    });
    return out;
}

UnstampTally UnstampAll(void (*hold)(uint64_t res))
{
    UnstampTally          t;
    std::vector<uint64_t> recorded;
    g_res_to_slot.for_each([&](const uint64_t& r, const HeapKey&) { recorded.push_back(r); });
    for (uint64_t r : recorded) {
        if (!UnstampResource(r, /*log*/ false)) continue;
        ++t.recorded;
        if (hold) hold(r);
    }
    const std::vector<std::pair<HeapKey, uint64_t>> sources = StampedCpuOnlySlots();
    t.cpu_only_stamped = sources.size();
    for (const auto& [key, res] : sources) {
        if (!UnstampSlot(key.heap, key.offset)) continue;
        ++t.cpu_only;
        if (hold) hold(res);
    }
    return t;
}

void SetAddonDisabled(bool dis)
{
    if (dis == g_addon_disabled.load()) return;
    if (dis) {
        const UnstampTally t = UnstampAll(nullptr);
        g_addon_disabled.store(true);
        logger::write("A1: addon DISABLED -- %u stamped slot(s) given the game's texture back, and %u of %zu still "
                      "stamped in CPU-only heaps",
            t.recorded, t.cpu_only, t.cpu_only_stamped);
    } else {
        ResetDescriptorTracking(); // BEFORE the hooks resume: nothing may read a stale slot
        g_addon_disabled.store(false);
    }
}

bool RestampResource(uint64_t resH)
{
    if (!g_d3d12_device || !resH || !g_tex_lineage) return false;
    HeapKey key{ 0, 0 };
    if (!g_res_to_slot.find(resH, key) || !key.heap) {
        if (g_debug && DiagOnce("RESTAMP-NOSLOT", (int)(resH >> 4)))
            logger::write("RESTAMP-FAIL res=%p: no slot recorded for it", (void*)resH);
        return false;
    }
    ID3D12Resource* res = nullptr;
    {
        ResInfo ri;
        if (ReadPublishedDescriptor(key, ri)) res = ri.res;
    }
    if (res != reinterpret_cast<ID3D12Resource*>(resH)) {
        if (g_debug && DiagOnce("RESTAMP-REUSED", (int)(resH >> 4)))
            logger::write("RESTAMP-FAIL res=%p: its slot heap=%llx off=%u now holds %p", (void*)resH,
                (unsigned long long)key.heap, (unsigned)key.offset, (void*)res);
        return false;
    }
    const bool ok = GlobalTexSwap(g_d3d12_device, key.heap, key.offset, res);
    if (!ok && g_debug && DiagOnce("RESTAMP-REFUSED", (int)(resH >> 4)))
        logger::write("RESTAMP-FAIL res=%p: slot heap=%llx off=%u is its own, the swap itself declined (%s)", (void*)resH,
            (unsigned long long)key.heap, (unsigned)key.offset, GlobalTexSwapDecline()[0] ? GlobalTexSwapDecline() : "miss: no mod SRV");
    return ok;
}

StampTally MoveStamps(const std::unordered_map<ID3D12Resource*, StampMove>& moves)
{
    StampTally t;
    if (!g_d3d12_device || moves.empty()) return t;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ForEachPublishedStamp([&](uint64_t heap,bool vis,uint32_t slot,ID3D12Resource* was,uint64_t expected_epoch) {
        const auto it=moves.find(was);
        if(it==moves.end())return;
        try{
            PreviousPair pair;
            auto h=RawDestination({heap,slot},pair);
            if(!h||pair.epoch!=expected_epoch){++t.raced;return;}
            RawDescriptorAdmission admission(HeapCpu(*h,slot).ptr,it->second.to_srv.ptr);
            if(!admission){++t.raced;return;}
            std::unique_lock lock(h->page(slot).mutex);
            const auto* old=PreviousCurrent(pair,*h)?PublishedDefinition(*h,slot):nullptr;
            if(!old||old->replacement!=was){++t.raced;return;}
            auto stamp=DefineTextureStamp(pair,*h,slot,it->second.to_srv,it->second.to);
            if(!stamp){++t.raced;return;}
            if(old->original_replica)++t.backups;
            const auto replica=stamp->definition.replica->cpu;
            RawCopyTo(admission,*h,slot,replica,std::move(stamp));
            if(vis)++t.moved_visible;
            else ++t.moved_cpu_only;
        }catch(...){++t.raced;}
    });
    return t;
}

D3D12_CPU_DESCRIPTOR_HANDLE NullSrvDescriptor(ID3D12Device* dev, uint8_t kind)
{
    static std::mutex                  s_mtx;
    static ID3D12DescriptorHeap*       s_heap = nullptr;
    static D3D12_CPU_DESCRIPTOR_HANDLE s_h[8] = {}; // index = slot below
    static UINT                        s_inc = 0;
    // DXIL kind -> our slot -> D3D12 view dimension
    static const D3D12_SRV_DIMENSION dims[8] = {
        D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE1D, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
        D3D12_SRV_DIMENSION_TEXTURE3D, D3D12_SRV_DIMENSION_TEXTURECUBE, D3D12_SRV_DIMENSION_TEXTURE1DARRAY,
        D3D12_SRV_DIMENSION_TEXTURECUBEARRAY, D3D12_SRV_DIMENSION_TEXTURE2DMS
    };
    unsigned slot = 0;
    switch (kind) {
    case 1: slot = 1; break;
    case 3: slot = 2; break;
    case 4: slot = 3; break;
    case 5: slot = 4; break;
    case 6: slot = 5; break;
    case 7: slot = 6; break;
    case 8: slot = 7; break;
    default: slot = 0; break;
    }
    std::lock_guard<std::mutex> l(s_mtx);
    if (!dev) return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
    if (!s_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = 8;
        if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&s_heap)))) return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        s_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (!s_h[slot].ptr) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = dims[slot];
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (dims[slot]) {
        case D3D12_SRV_DIMENSION_TEXTURE1D: sd.Texture1D.MipLevels = 1; break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
            sd.Texture1DArray.MipLevels = 1;
            sd.Texture1DArray.ArraySize = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            sd.Texture2DArray.MipLevels = 1;
            sd.Texture2DArray.ArraySize = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D: sd.Texture3D.MipLevels = 1; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE: sd.TextureCube.MipLevels = 1; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            sd.TextureCubeArray.MipLevels = 1;
            sd.TextureCubeArray.NumCubes = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMS: break;
        default: sd.Texture2D.MipLevels = 1; break;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h{ s_heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)slot * s_inc };
        dev->CreateShaderResourceView(nullptr, &sd, h);
        s_h[slot] = h;
    }
    return s_h[slot];
}

// A slot stamped with a texture that is dying given back: the game's descriptor if one can be had, else a null view.
// False when the game wrote the slot meanwhile.
static bool GiveBack(const HeapKey& key, ID3D12Resource* res, StampTally& t)
{
    ResInfo ri{};
    const bool game_res=ReadPublishedDescriptor(key,ri)&&ri.res;
    if(UnstampSlotImpl(key.heap,key.offset,true,res)){
        ++t.restored;
    }else try{
        const auto null_srv=NullSrvDescriptor(g_d3d12_device,0);
        PreviousPair pair;
        auto h=RawDestination(key,pair);
        if(!h||!null_srv.ptr){++t.raced;return false;}
        RawDescriptorAdmission admission(HeapCpu(*h,key.offset).ptr,null_srv.ptr);
        if(!admission){++t.raced;return false;}
        std::unique_lock lock(h->page(key.offset).mutex);
        const auto* old=PreviousCurrent(pair,*h)?PublishedDefinition(*h,key.offset):nullptr;
        if(!old||old->replacement!=res){++t.raced;return false;}
        auto nulled=DefineTextureStamp(pair,*h,key.offset,null_srv,nullptr);
        if(!nulled){++t.raced;return false;}
        const auto replica=nulled->definition.replica->cpu;
        RawCopyTo(admission,*h,key.offset,replica,std::move(nulled));
        ++t.nulled;
    }catch(...){++t.raced;return false;}
    if(game_res)TexHoldForRetry(reinterpret_cast<uint64_t>(ri.res));
    return true;
}

StampTally UnstampDying(const std::unordered_set<ID3D12Resource*>& dying)
{
    StampTally t;
    if(!g_d3d12_device||dying.empty())return t;
    std::vector<std::pair<HeapKey,ID3D12Resource*>> found;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ForEachPublishedStamp([&](uint64_t heap,bool,uint32_t slot,ID3D12Resource* was,uint64_t) {
        if(was&&dying.count(was))found.push_back({{heap,slot},was});
    });
    for(const auto& [key,was]:found)GiveBack(key,was,t);
    return t;
}

void StampedStoreTexture(uint64_t heap, uint32_t offset, ID3D12Resource* res)
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if(!res||!g_d3d12_device||ModLoader::TexIndexOf(res)>=0)return;
    StampTally t;
    GiveBack({heap,offset},res,t);
}
