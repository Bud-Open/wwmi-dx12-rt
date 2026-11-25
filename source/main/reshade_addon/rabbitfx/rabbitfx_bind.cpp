#include "published_descriptor.h"
#include <span>
#include <algorithm>
#include "rt_material_tables.h"
#include "rt_native_binding_restore.h"
#include "rt_material_replica.hpp"
#include "rt_cpu_phase.hpp"
#include "../hooks/pipeline_hooks.h"
// The RabbitFX texture binder and the private shadow heap it binds through. See rabbitfx_bind.h.
#include "../addon_common.h"
#include "rabbitfx_bind.h"
#include "../instrumentation/removal_pin.h"
#include "../hooks/cb_ring.h"
#include "../../src/gpu_progress.h"
#include "../draw/read_slots.h"
#include "../draw/draw_inject.h"
#include "rt_hybrid_rfx_material.hpp"
#include "rt_material_draw_note.hpp"
#include <set>
#include <tuple>

struct RtPsMaterialHint {
    uint64_t uv_srvs = 0;
    uint64_t normal_srvs = 0;
    uint64_t non_cmp_2d = 0;
    uint8_t  first_order[16]{};
    uint8_t  lane_mask[32]{};
    bool     valid = false;
};
bool RtLookupPsMaterialHint(uint64_t ps_hash, RtPsMaterialHint& out);

static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_shadow_srv_heap;
static uint32_t                                     g_shadow_srv_cap = 0;
static uint32_t                                     g_shadow_srv_cur = 0;
static uint32_t                                     g_shadow_srv_inc = 0;
static std::mutex                                   g_shadow_heap_mtx;

std::atomic<unsigned long long> g_rfx_mirror_misses{ 0 }; // slots mirrored as null for want of a source record

static bool ShadowHeapsEnsure(ID3D12Device* dev)
{
    if (g_shadow_srv_heap) return true;
    g_shadow_srv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC d = {};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.NumDescriptors = 262144;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_shadow_srv_heap)))) return false;
    g_shadow_srv_cap = d.NumDescriptors;
    return true;
}

static std::vector<uint64_t>                                                          g_shadow_tag; // per block: GpuStamp() at its last write
static std::vector<std::pair<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, uint64_t>> g_shadow_retired;
static bool                                                                           ShadowReserve(uint32_t n)
{
    constexpr uint32_t kBlk = 4096;
    constexpr uint32_t kMax = 1000000;
    const uint64_t     now = GpuStamp();
    for (size_t i = 0; i < g_shadow_retired.size();)
        if (GpuPassed(g_shadow_retired[i].second)) {
            g_shadow_retired[i] = std::move(g_shadow_retired.back());
            g_shadow_retired.pop_back();
        } else
            ++i;
    for (;;) {
        const uint32_t cap = g_shadow_srv_cap;
        const uint32_t blocks = (cap + kBlk - 1) / kBlk;
        if (g_shadow_tag.size() != blocks) g_shadow_tag.assign(blocks, 0);
        uint32_t start = g_shadow_srv_cur;
        if (start + n > cap) start = 0;
        const uint32_t first = (start == g_shadow_srv_cur && start % kBlk) ? start / kBlk + 1 : start / kBlk;
        const uint32_t last = (start + n - 1) / kBlk;
        bool           free = start + n <= cap;
        for (uint32_t b = first; free && b <= last; ++b)
            if (g_shadow_tag[b] && !GpuPassed(g_shadow_tag[b])) free = false;
        if (free) {
            for (uint32_t b = start / kBlk; b <= last; ++b) g_shadow_tag[b] = now;
            g_shadow_srv_cur = start;
            return true;
        }
        uint32_t size = cap;
        do size = size >= kMax / 2 ? kMax : size * 2;
        while (size < n && size < kMax);
        if (size <= cap || size < n) return false; // at D3D12's limit and every slot in flight
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = size;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> h;
        if (FAILED(g_d3d12_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h)))) return false;
        g_shadow_retired.push_back({ g_shadow_srv_heap, now });
        g_shadow_srv_heap = h;
        g_shadow_srv_cap = size;
        g_shadow_srv_cur = 0;
        g_shadow_tag.clear();
        logger::write("RFX-SHADOW: shadow heap grown to %u descriptors (its slots were still in flight)", size);
    }
}

struct Param0Window {
    reshade::api::descriptor_table tabs[64] = {};
    std::vector<LayoutTableParam>  ltabs;
    ID3D12DescriptorHeap*          srv_heap = nullptr;
    ID3D12DescriptorHeap*          smp_heap = nullptr;
    const RtNativeDrawHeaps*       native_heaps = nullptr;
    uint8_t                        srv_param = 0;
    uint32_t                       srv_base = 0;
    uint32_t                       srv_basereg = 0;
    uint32_t                       srv_count = 0;
    int                            n_srv = 0;
};
struct RfxBoundSlot {unsigned reg;uint8_t param;unsigned offset;D3D12_CPU_DESCRIPTOR_HANDLE srv;uint8_t role;};
struct RtRecordedMaterialWindow {Param0Window params;};
static bool ResolveParam0Window(reshade::api::command_list* cmd_list, Param0Window& w)
{
    using namespace reshade::api;
    auto *native=reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
    if(RtHasNativeBindingScope(native)){
        w.native_heaps=RtOriginalDrawHeaps(native);
        if(!w.native_heaps)return false;
    }
    uint64_t layh = 0;
    {
        auto& s = CmdGfx(cmd_list);
        for (int p = 0; p < 64; ++p) w.tabs[p] = s.root[p].table;
        layh = CmdGfxLayout(cmd_list);
    }
    if (!layh) return false;
    {
        std::lock_guard l(g_layout_tables_mtx);
        auto            it = g_layout_tables.find(layh);
        if (it != g_layout_tables.end()) w.ltabs = it->second;
    }
    // RABBITFX-BAIL (g_debug, once per layout): the window could not be resolved, and from which step.
    if (w.ltabs.empty()) {
        if (g_debug && DiagOnce("RFX-NOTABLES", (int)(layh ^ (layh >> 32))))
            logger::write("RABBITFX-BAIL layout=%llx: no g_layout_tables entry", (unsigned long long)layh);
        return false;
    }
    int unbound = 0;
    for (const auto& t : w.ltabs) {
        const TableStart ts = TableStartOf(cmd_list->get_device(), w.tabs[t.param]);
        if (!ts || (w.native_heaps&&ts.Heap()!=(t.kind==0?w.native_heaps->samplers:w.native_heaps->resources))) {
            if(w.native_heaps)w.tabs[t.param]={};
            if (t.kind == 1) ++unbound;
            continue;
        }
        auto*          hp = ts.Heap();
        const uint32_t b = ts.base;
        if (t.kind == 1) {
            ++w.n_srv;
            if (!w.srv_heap) {
                w.srv_heap = hp;
                w.srv_param = t.param;
                w.srv_base = b;
                w.srv_basereg = t.base_reg;
                w.srv_count = t.count;
            }
        }
        if (t.kind == 0 && !w.smp_heap) w.smp_heap = hp;
    }
    if (!w.srv_heap && g_debug && DiagOnce("RFX-NOSRVTAB", (int)(layh ^ (layh >> 32))))
        logger::write("RABBITFX-BAIL layout=%llx: %zu table params, none a bound CBV_SRV_UAV table (%d srv/uav/cbv params with no bound table)",
            (unsigned long long)layh, w.ltabs.size(), unbound);
    return w.srv_heap != nullptr;
}

static std::array<RegBind,4> RfxPsLocations(reshade::api::command_list* list){
    std::array<RegBind,4> regs{};
    const auto layout=CmdGfxLayout(list);
    std::lock_guard lock(g_layout_regmap_mtx);
    const auto found=g_layout_regmap.find(layout);
    if(found!=g_layout_regmap.end()&&found->second.built)
        for(unsigned i=0;i<4;++i)regs[i]=found->second.t_ps[i];
    return regs;
}

#include "rt_material_reads.inc"
static unsigned MirrorTable(uint64_t heap, uint32_t base, uint32_t count, uint32_t cur, SIZE_T sh_cpu0,uint32_t increment,
    D3D12_CPU_DESCRIPTOR_HANDLE null_srv,
    const std::bitset<kTexScanMax>* want, std::span<const ShadowSlotOvr> overrides={}, unsigned parameter=~0u,RtMaterialTables* prepared=nullptr,reshade::api::command_list* list=nullptr)
{
    unsigned misses = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (want && i < kTexScanMax && !want->test(i)) continue;
        if(std::any_of(overrides.begin(),overrides.end(),[&](const auto &e){ return e.param==parameter && e.offset==i; })) continue;
        ResInfo ri{};
        const D3D12_CPU_DESCRIPTOR_HANDLE destination{sh_cpu0+SIZE_T(cur+i)*increment};
        if(!MaterializePublishedDescriptor(g_d3d12_device,{heap,base+i},destination,ri)){
            ++misses;
            if(prepared&&prepared->missing_param==~0u){
                prepared->missing_param=parameter;prepared->missing_offset=i;
                RtNoteMaterialSource(list,heap,base,parameter,i,ri.res!=nullptr,ri);
            }
            g_d3d12_device->CopyDescriptorsSimple(1,destination,null_srv,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
    return misses;
}

static thread_local std::vector<RfxDumpBind> t_dump_binds;
static thread_local unsigned                 t_dump_draw = ~0u;
const std::vector<RfxDumpBind>*              RfxBindsForDump(unsigned draw_index)
{
    return (draw_index == t_dump_draw && !t_dump_binds.empty()) ? &t_dump_binds : nullptr;
}

static thread_local std::vector<ShadowSlotOvr> t_extra;
void                                           SetShadowExtraSlots(const ShadowSlotOvr* s, unsigned n)
{
    t_extra.assign(s, s ? s + n : s);
}
void AddShadowExtraSlots(const ShadowSlotOvr* s, unsigned n)
{
    if (s) t_extra.insert(t_extra.end(), s, s + n);
}

static RfxBindState BindThroughShadow(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl, const Param0Window& w,
    const std::vector<WclSlotOvr>& binds,std::span<const ShadowSlotOvr> overrides,
    RtMaterialTables *prepared=nullptr,RtRecordedMaterial *recorded=nullptr,std::span<const RfxBoundSlot> semantic={})
{
    RfxBindState st{};
    if (MigotoPort::FrameAnalysisArmed()) {
        t_dump_draw = MigotoPort::FrameAnalysisDrawIndex();
        t_dump_binds.clear();
        for (const auto& b : semantic) t_dump_binds.push_back(RfxDumpBind{ int(b.reg), b.srv, ModLoader::TexResOfSrv(b.srv) });
        for (const auto& b : binds) t_dump_binds.push_back(RfxDumpBind{ b.reg, b.srv, ModLoader::TexResOfSrv(b.srv) });
    }
    uint32_t maxwi = 0;
    for(const auto& b:semantic)if(b.param==w.srv_param)maxwi=(std::max)(maxwi,b.offset);
    for (const auto& b : binds)
        if (b.reg >= (int)w.srv_basereg) maxwi = std::max<uint32_t>(maxwi, (uint32_t)b.reg - w.srv_basereg);
    for (const ShadowSlotOvr& e : overrides)
        if (e.param == w.srv_param) maxwi = std::max<uint32_t>(maxwi, e.offset);
    uint32_t win = w.srv_count ? w.srv_count : (maxwi + 1);
    if (win < maxwi + 1) win = maxwi + 1;
    if (win == 0 || win > 128) return st; // not the small param0 table -> bail (avoid bindless mirror)
    struct Mirror {
        uint8_t  param;
        uint64_t heap;
        uint32_t base;
        uint32_t count;
        uint32_t cur;
    };
    Mirror   mir[64];
    unsigned nm = 0;
    uint32_t total = 0;
    for (const auto& t : w.ltabs) {
        if (t.kind != 1 || !w.tabs[t.param].handle) continue;
        const TableStart ts = TableStartOf(g_reshade_device, w.tabs[t.param]);
        if (!ts) continue;
        const uint32_t cnt = (t.param == w.srv_param) ? win : t.count;
        if (cnt == 0 || cnt > 1024 || nm >= 64) return st;
        mir[nm++] = Mirror{ t.param, ts.heap, ts.base, cnt, 0 };
        total += cnt;
    }
    if (!nm) return st;
    if(!semantic.empty()&&!std::all_of(semantic.begin(),semantic.end(),[&](const auto& b){
        return std::any_of(mir,mir+nm,[&](const auto& m){return m.param==b.param&&b.offset<m.count;});
    }))return st;
    if(prepared)for(const auto& e:overrides){
        if(!e.src.ptr || !std::any_of(mir,mir+nm,[&](const auto& m){return m.param==e.param && e.offset<m.count;}))return st;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE null_srv{};
    if (!RfxPatchNullSrvs(&null_srv, nullptr)) return st;
    std::unique_lock lh(g_shadow_heap_mtx,std::defer_lock);
    ID3D12DescriptorHeap *heap=nullptr;
    SIZE_T sh_cpu0=0;UINT64 sh_gpu0=0;uint32_t increment=0,cursor=0;
    struct MaterialCopy {uint32_t offset;RtMaterialReplica view;};
    std::vector<MaterialCopy> copies;
    const auto* note=recorded&&prepared?rt_material_copy_note:nullptr;
    std::array<RtRfxDrawFacts::Destination,4> ps_locations{};
    if(note){
        if(note->facts->locations_known)ps_locations=note->facts->ps_locations;
        else{
            const auto live=RfxPsLocations(cmd_list);
            for(unsigned i=0;i<4;++i)if(live[i].is_table)ps_locations[i]={live[i].param,live[i].offset};
        }
    }
    const auto note_written=[&](unsigned param,unsigned offset,uint64_t resource,uint64_t source,char role){
        for(unsigned r=0;r<4;++r)if(ps_locations[r].param==param&&ps_locations[r].offset==offset)
            note->facts->final[r]={resource,source,0,param,offset,role};
    };
    if(recorded){
        const ReadSlots& rs=RtMaterialReadSlots(cmd_list,w);
        struct Stats {
            unsigned created=0,reused=0,stamped=0,copied=0,missed=0;
            ~Stats(){RtCountRecordedMaterial(created,reused,stamped,copied,missed);}
        } stats;
        auto &r=recorded->request.base;auto &b=recorded->backing.base;r.descriptor_count=total;
        // Freeze the exact mirrored sources once, before allocating the
        // recording's table. The same reserve retains effective resources
        // and UAV counters together with any authored texture overrides.
        std::vector<uint64_t> resources;
        if(r.resource_count)resources.assign(r.resources,r.resources+r.resource_count);
        for(unsigned k=0;k<nm;++k){
            const auto& range=mir[k];const unsigned pm=range.param;
            const auto *want=pm<64&&rs.known[pm]?&rs.want[pm]:nullptr;
            unsigned observed[4]{~0u,~0u,~0u,~0u};
            if(note)for(unsigned r=0;r<4;++r)
                if(ps_locations[r].param==pm)observed[r]=ps_locations[r].offset;
            for(uint32_t i=0;i<range.count;++i){
                if(want&&i<kTexScanMax&&!want->test(i))continue;
                if(std::any_of(overrides.begin(),overrides.end(),[&](const auto &e){return e.param==pm&&e.offset==i;}))continue;
                ResInfo info{};RtMaterialReplica view{};
                if(!AcquirePublishedMaterialReplica(g_d3d12_device,{range.heap,range.base+i},info,view)){
                    ++stats.missed;
                    ReadPublishedDescriptor({range.heap,range.base+i},info);
                    prepared->missing_param=pm;prepared->missing_offset=i;prepared->failure=8;
                    RtNoteMaterialSource(cmd_list,range.heap,range.base,pm,i,info.res!=nullptr,info);
                    return st;
                }
                stats.created+=view.initialized;stats.stamped+=view.stamped;
                stats.reused+=!view.initialized&&!view.stamped;
                if(note&&(i==observed[0]||i==observed[1]||i==observed[2]||i==observed[3]))
                    note_written(pm,i,reinterpret_cast<uint64_t>(view.resources[0].Get()),view.cpu.ptr,'G');
                for(const auto& resource:view.resources)if(resource){
                    const auto handle=reinterpret_cast<uint64_t>(resource.Get());
                    if(std::find(resources.begin(),resources.end(),handle)==resources.end())resources.push_back(handle);
                }
                copies.push_back({cursor+i,std::move(view)});
            }
            cursor+=range.count;
        }
        cursor=0;
        auto request=recorded->request;
        request.base.resources=resources.data();request.base.resource_count=unsigned(resources.size());
        unsigned result=WWMI_RT_SURFACE_BAD_INPUT;
        if(recorded->reserve2){
            result=recorded->reserve2(&request,sizeof(request),&recorded->backing,sizeof(recorded->backing));
            if(result==WWMI_RT_SURFACE_OK)for(unsigned i=0;i<WWMI_RT_NH_CB_COUNT;++i){
                const auto va=recorded->backing.no_history_va[i];
                if(bool(va)!=bool(recorded->request.no_history_cb[i])||va% D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT){
                    result=WWMI_RT_SURFACE_BAD_INPUT;break;
                }
            }
        }else if(recorded->reserve)result=recorded->reserve(&request.base,sizeof(request.base),&b,sizeof(b));
        if(result!=WWMI_RT_SURFACE_OK||
           b.struct_size!=sizeof(b)||b.abi_version!=1||!b.heap||!b.cpu_base||!b.gpu_base||!b.descriptor_increment||!b.local_vf_va){
            prepared->failure=10;return st;
        }
        heap=reinterpret_cast<ID3D12DescriptorHeap *>(b.heap);sh_cpu0=SIZE_T(b.cpu_base);sh_gpu0=b.gpu_base;increment=b.descriptor_increment;
        for(const auto& copy:copies)
            g_d3d12_device->CopyDescriptorsSimple(1,{sh_cpu0+SIZE_T(copy.offset)*increment},copy.view.cpu,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        stats.copied=unsigned(copies.size());
    }else{
        // Ordinary RabbitFX retains its original allocator and progress contract.
        lh.lock();
        if (!ShadowHeapsEnsure(g_d3d12_device) || !g_shadow_srv_inc) return st;
        if (!ShadowReserve(total)) return st;
        heap=g_shadow_srv_heap.Get();increment=g_shadow_srv_inc;cursor=g_shadow_srv_cur;
        sh_cpu0=heap->GetCPUDescriptorHandleForHeapStart().ptr;
        sh_gpu0=heap->GetGPUDescriptorHandleForHeapStart().ptr;
    }
    const ReadSlots *rs=recorded?nullptr:std::addressof(prepared?RtMaterialReadSlots(cmd_list,w):DrawReadSlots(cmd_list));
    unsigned         misses = 0;
    uint32_t         cur0 = 0;
    for (unsigned k = 0; k < nm; ++k) {
        mir[k].cur = cursor;
        cursor += mir[k].count;
        if(!recorded)g_shadow_srv_cur=cursor;
        const uint8_t pm = mir[k].param;
        if(!recorded)misses += MirrorTable(mir[k].heap, mir[k].base, mir[k].count, mir[k].cur, sh_cpu0, increment, null_srv,
            pm < 64 && rs->known[pm] ? &rs->want[pm] : nullptr, prepared?overrides:std::span<const ShadowSlotOvr>{},pm,prepared,cmd_list);
        if (mir[k].param == w.srv_param) cur0 = mir[k].cur;
    }
    for(const auto& b:semantic){
        for(unsigned k=0;k<nm;++k)if(mir[k].param==b.param&&b.offset<mir[k].count){
            g_d3d12_device->CopyDescriptorsSimple(1,{sh_cpu0+SIZE_T(mir[k].cur+b.offset)*increment},b.srv,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            break;
        }
    }
    for (const auto& b : binds) {
        if (b.is_null || !b.srv.ptr || b.reg < (int)w.srv_basereg) continue;
        uint32_t wi = (uint32_t)b.reg - w.srv_basereg;
        if (wi >= win) continue;
        D3D12_CPU_DESCRIPTOR_HANDLE dd = { sh_cpu0 + (SIZE_T)(cur0 + wi) * increment };
        g_d3d12_device->CopyDescriptorsSimple(1, dd, b.srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    for(size_t n=0;n<overrides.size();++n) {
        const auto& e=overrides[n];
        bool put = false;
        for (unsigned k = 0; k < nm && !put; ++k)
            if (mir[k].param == e.param && e.offset < mir[k].count) {
                g_d3d12_device->CopyDescriptorsSimple(1, { sh_cpu0 + (SIZE_T)(mir[k].cur + e.offset) * increment },
                    e.src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                if(note){
                    const bool author=n<note->owners.size();
                    const auto role=n<note->facts->bound_count?RtRfxRole(note->facts->bound_role[n]):author?'E':'V';
                    uint64_t resource=author?note->owners[n]:0;
                    if(!author)for(const auto& v:note->vs_inputs)if(v.view==e.src.ptr){resource=v.resource;break;}
                    note_written(e.param,e.offset,resource,e.src.ptr,role);
                }
                put = true;
            }
        if (!put && DiagOnce("SHADOW-EXTRA-MISS", e.param * 4096 + (int)e.offset))
            logger::write("SHADOW-EXTRA: p%u+%u is in no mirrored table -- the draw reads the game's descriptor there",
                (unsigned)e.param, e.offset);
    }
    if(prepared && misses){prepared->failure=8;return st;}
    if (misses) {
        g_rfx_mirror_misses.fetch_add(misses, std::memory_order_relaxed);
        if (g_debug && DiagOnce("RFX-MIRROR-MISS", (int)(w.srv_param * 4096u + total)))
            logger::write("RFX-MIRROR: %u of %u mirrored slot(s) had no CPU-only source record -> null SRV (param0 %u, %u table(s))",
                misses, total, (unsigned)w.srv_param, nm);
    }
    ID3D12DescriptorHeap* game_srv = w.native_heaps?w.native_heaps->resources:GameHeap(cmd_list, 0);
    ID3D12DescriptorHeap* game_smp = w.native_heaps?w.native_heaps->samplers:GameHeap(cmd_list, 1);
    if (!w.native_heaps&&!game_srv) game_srv = w.srv_heap;
    if (!w.native_heaps&&!game_smp) game_smp = w.smp_heap;
    if(prepared){
        prepared->heap=heap;
        for(auto& tab:w.ltabs)if(tab.kind==0 && w.tabs[tab.param].handle)
            prepared->tables.emplace_back(tab.param,D3D12_GPU_DESCRIPTOR_HANDLE{w.tabs[tab.param].handle});
        for(unsigned k=0;k<nm;++k)prepared->tables.emplace_back(mir[k].param,D3D12_GPU_DESCRIPTOR_HANDLE{sh_gpu0+(UINT64)mir[k].cur*increment});
        if(note){
            for(auto& slot:note->facts->final)for(const auto& [p,table]:prepared->tables)
                if(slot.param==p)slot.root_table=table.ptr;
            note->facts->copied=true;
        }
    }else{
    if(!RtSetDrawHeaps(cl,heap,game_smp))return st;
    for (unsigned k = 0; k < nm; ++k) // every mirrored table -> its shadow copy
        cl->SetGraphicsRootDescriptorTable(mir[k].param, { sh_gpu0 + (UINT64)mir[k].cur * increment });
    RemovalPin::Ours(cl, "rfx-bind");
    }
    for (const auto& t : w.ltabs)
        if (w.tabs[t.param].handle) st.tables.push_back({ t.param, w.tabs[t.param].handle });
    st.bound = true;
    st.applied = true;
    st.game_srv = game_srv;
    st.game_smp = game_smp;
    return st;
}

RfxBindState ApplyDrawBindings(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl,
    const std::vector<WclSlotOvr>& binds, bool guard_bindless)
{
    RfxBindState st{};
    if ((binds.empty() && t_extra.empty()) || !cmd_list || !cl || !g_d3d12_device) return st;
    Param0Window w;
    if (!ResolveParam0Window(cmd_list, w)) return st;
    if (guard_bindless && w.n_srv > 1) return st; // bindless pass -> BAIL (a shadow swap would unbind it)
    return BindThroughShadow(cmd_list, cl, w, binds, t_extra);
}

static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_rfxp_cpu_heap;
static constexpr uint32_t                           kRfxpCpuSlots = 4096;
static std::atomic<uint32_t>                        g_rfxp_cpu_next{ 0 };
static std::mutex                                   g_rfxp_cpu_mtx;
static CbRing                                       g_rfxp_params_ring;
static constexpr uint32_t                           kRfxpParamsBytes = 256u * 1024u; // the ring's first buffer: 1024 draws

static bool RfxPatchCpuHeapEnsure()
{
    static std::atomic<bool> s_made{ false };
    if (s_made.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> l(g_rfxp_cpu_mtx);
    if (g_rfxp_cpu_heap) return true;
    if (!g_d3d12_device) return false;
    D3D12_DESCRIPTOR_HEAP_DESC d = {};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.NumDescriptors = kRfxpCpuSlots; // not shader-visible: a legal copy SOURCE
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> h;
    if (FAILED(g_d3d12_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h)))) return false;
    if (!g_shadow_srv_inc) g_shadow_srv_inc = g_d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const SIZE_T                    base = h->GetCPUDescriptorHandleForHeapStart().ptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC t = {};
    t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    t.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    t.Texture2D.MipLevels = 1;
    g_d3d12_device->CreateShaderResourceView(nullptr, &t, { base });
    D3D12_SHADER_RESOURCE_VIEW_DESC b = {};
    b.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    b.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    b.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    b.Buffer.NumElements = 1;
    g_d3d12_device->CreateShaderResourceView(nullptr, &b, { base + g_shadow_srv_inc });
    g_rfxp_cpu_heap = h;
    s_made.store(true, std::memory_order_release);
    return true;
}
bool RfxPatchNullSrvs(D3D12_CPU_DESCRIPTOR_HANDLE* null_tex, D3D12_CPU_DESCRIPTOR_HANDLE* null_buf)
{
    if (!RfxPatchCpuHeapEnsure()) return false;
    const SIZE_T base = g_rfxp_cpu_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (null_tex) *null_tex = { base };
    if (null_buf) *null_buf = { base + g_shadow_srv_inc };
    return true;
}
D3D12_CPU_DESCRIPTOR_HANDLE RfxPatchParamsSrv(float hue, float sat, float val, float brightness)
{
    if (!RfxPatchCpuHeapEnsure() || !g_rfxp_params_ring.Ensure(g_d3d12_device, kRfxpParamsBytes)) return { 0 };
    uint64_t        va = 0;
    ID3D12Resource* ring = nullptr;
    float*          p = (float*)g_rfxp_params_ring.Take(&va, &ring);
    if (!p) return { 0 };
    p[0] = hue;
    p[1] = sat;
    p[2] = val;
    p[3] = brightness; // x217..w217
    const uint64_t                  off = va - ring->GetGPUVirtualAddress();
    D3D12_SHADER_RESOURCE_VIEW_DESC b = {};
    b.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    b.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    b.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    b.Buffer.FirstElement = off / 16;
    b.Buffer.NumElements = 1;
    const uint32_t                    slot = 2 + g_rfxp_cpu_next.fetch_add(1, std::memory_order_relaxed) % (kRfxpCpuSlots - 2);
    const D3D12_CPU_DESCRIPTOR_HANDLE h = { g_rfxp_cpu_heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)slot * g_shadow_srv_inc };
    g_d3d12_device->CreateShaderResourceView(ring, &b, h);
    return h;
}

bool RfxParam0Covers(reshade::api::command_list* cmd_list, unsigned max_reg, unsigned* base_reg, unsigned* count)
{
    Param0Window w;
    if (!cmd_list || !ResolveParam0Window(cmd_list, w)) return false;
    if (base_reg) *base_reg = w.srv_basereg;
    if (count) *count = w.srv_count;
    return w.srv_basereg == 0 && max_reg < w.srv_count;
}

// BEGIN TESTABLE RFX PLAN
enum class RfxPlan { unknown, empty, ready, rejected };

static bool RfxPhysicalOffset(const Param0Window& w,const RegBind& bind,unsigned heap_count,uint32_t& offset)
{
    // LayoutRegMap accounts for root parameter, register space and declared
    // range binding offset. srv_basereg alone describes only the first range.
    if(!bind.is_table||bind.param!=w.srv_param||w.srv_base>=heap_count||
       bind.offset>=heap_count-w.srv_base||(w.srv_count&&bind.offset>=w.srv_count))return false;
    offset=bind.offset;
    return true;
}

static RfxPlan PlanRabbitFxMaps(const Param0Window& w,const std::array<RegBind,4>& ps,
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE,4>& maps,std::vector<RfxBoundSlot>& binds,RtRfxDrawFacts& facts)
{
    const auto heap=reinterpret_cast<uint64_t>(w.srv_heap);
    const auto heap_count=w.srv_heap->GetDesc().NumDescriptors;
    std::array<uint32_t,4> physical{};
    unsigned mapped=0;
    for(unsigned s=0;s<4;++s)if(RfxPhysicalOffset(w,ps[s],heap_count,physical[s]))mapped|=1u<<s;
    facts.native_mapped=uint8_t(mapped&7u);
    int cls[3]{};
    for(unsigned s=0;s<3;++s){
        if(!(mapped&(1u<<s)))continue;
        RtPublishedTextureShape d{};
        const auto source=ReadPublishedTextureShape(HeapKey{heap,w.srv_base+physical[s]},d);
        facts.native_sources[s]=source;
        if(source==RtPublishedTexture::ready||source==RtPublishedTexture::unknown||
           source==RtPublishedTexture::non_texture_2d)facts.native_present|=uint8_t(1u<<s);
        if(source!=RtPublishedTexture::ready)continue;
        facts.native_valid|=uint8_t(1u<<s);
        cls[s]=RfxTexClass(d.format,d.width,d.height,d.mips,d.arrays);
        facts.native[s]={d.format,d.width,d.height,d.mips,unsigned(cls[s])};
    }
    if(facts.native_valid!=7){
        for(unsigned s=0;s<3;++s)if(!(facts.native_valid&(1u<<s))){facts.missing_slot=uint8_t(s);break;}
        if(facts.native_mapped!=7)facts.step=RtRfxStep::window;
        else switch(facts.native_sources[facts.missing_slot]){
            case RtPublishedTexture::wrong_view:facts.step=RtRfxStep::wrong_native_view;break;
            case RtPublishedTexture::non_texture_2d:facts.step=RtRfxStep::not_native_2d;break;
            case RtPublishedTexture::stale:facts.step=RtRfxStep::stale_native;break;
            case RtPublishedTexture::unknown:facts.step=RtRfxStep::unknown_description;break;
            default:facts.step=RtRfxStep::missing_native;break;
        }
        return RfxPlan::unknown;
    }
    auto& fresh=facts.fresh;
    char flags[8]{};
    if(!RfxSetTexturesDecision(cls,bool(maps[0].ptr),bool(maps[1].ptr),bool(maps[2].ptr),bool(maps[3].ptr),
       fresh[0],fresh[1],fresh[2],fresh[3],flags)){
        facts.step=RtRfxStep::known_empty;
        return RfxPlan::empty;
    }
    if(!g_d3d12_device){facts.step=RtRfxStep::no_device;return RfxPlan::rejected;}
    for(unsigned i=0;i<4;++i)if(fresh[i]&~mapped){
        facts.step=RtRfxStep::offset;return RfxPlan::rejected;
    }
    // Complete Native metadata wins on this very draw. No per-Mod/PS latch
    // can replace a new valid classification with a previous map.
    for(unsigned i=0;i<4;++i)for(unsigned r=0;r<4;++r)if(fresh[i]&(1u<<r))
        binds.push_back({r,w.srv_param,physical[r],maps[i],uint8_t(1u<<i)});
    facts.masks=fresh;
    facts.step=RtRfxStep::plan_ready;
    return RfxPlan::ready;
}
// END TESTABLE RFX PLAN

static bool RfxLowSlotsAreEnginePrefix(const RtRfxDrawFacts& facts)
{
    // Classic GBuffer draws bind character textures directly on t0..t2.
    // Forward / translucent UE4 draws bind engine shadow/light-grid/LUT resources on t0..t2
    // (non-Texture2D buffers/3D textures, non-square shadow atlases, or <64x64 LUTs) and place
    // character material Texture2Ds in higher PS SRV slots (t3..t31).
    if(facts.step==RtRfxStep::not_native_2d)return true;
    for(unsigned s=0;s<3;++s){
        if(!(facts.native_valid&(1u<<s)))continue;
        const auto& n=facts.native[s];
        const bool square_mat=(n.width>=64&&n.width==n.height);
        const bool mat_fmt=(n.format>=97&&n.format<=99)||(n.format>=70&&n.format<=72)||
                           (n.format>=76&&n.format<=78)||(n.format>=82&&n.format<=84)||
                           n.format==61||((n.format>=87&&n.format<=91)&&n.mips>1&&n.width<512);
        if(square_mat&&mat_fmt)return false;
    }
    return true;
}

static RfxPlan PlanRabbitFxHighSlots(reshade::api::command_list* list,const Param0Window& w,
    int mod_idx,int comp_idx,const std::array<D3D12_CPU_DESCRIPTOR_HANDLE,4>& maps,
    uint32_t ps_reads_32,std::vector<RfxBoundSlot>& binds,RtRfxDrawFacts& facts)
{
    if(!g_d3d12_device){facts.step=RtRfxStep::no_device;return RfxPlan::rejected;}
    const uint64_t layout=CmdGfxLayout(list);
    if(!layout)return RfxPlan::unknown;
    std::array<RegBind,32> t_ps{};
    {
        std::lock_guard lock(g_layout_regmap_mtx);
        const auto found=g_layout_regmap.find(layout);
        if(found==g_layout_regmap.end()||!found->second.built)return RfxPlan::unknown;
        const unsigned lim=(std::min)(32u,unsigned(RegisterFileWidth(TargetClass::ShaderResource)));
        for(unsigned i=0;i<lim;++i)t_ps[i]=found->second.t_ps[i];
    }
    uint64_t ps_hash=0;
    list->get_private_data(s_iid_ps_hash,&ps_hash);
    RtPsMaterialHint hint{};
    RtLookupPsMaterialHint(ps_hash,hint);

    struct SlotCand {
        unsigned reg=0;
        uint8_t  param=0xff;
        uint32_t offset=0;
        uint32_t fmt=0,vfmt=0,w=0,h=0,mips=0;
        bool     is_bc7=false,is_bc5=false,is_bc1=false,is_bc3=false,is_r8=false,is_bgra=false,is_rgba=false,is_srgb=false;
        bool     is_magic_dot=false;
        bool     in_uv=false,in_norm=false;
        uint8_t  lanes=0;
        unsigned order=999;
    };
    std::vector<SlotCand> cands;
    std::string scan_diag;
    auto* dev=list->get_device();

    for(unsigned r=0;r<32;++r){
        if(!(ps_reads_32&(1u<<r)))continue;
        const RegBind& b=t_ps[r];
        if(!b.is_table||b.param>=64||!w.tabs[b.param].handle)continue;
        uint32_t tab_count=0;
        bool tab_found=false;
        for(const auto& lt:w.ltabs){
            if(lt.kind==1&&lt.param==b.param){tab_count=lt.count;tab_found=true;break;}
        }
        if(!tab_found)continue;
        if(tab_count&&b.offset>=tab_count)continue;
        const TableStart ts=TableStartOf(dev,w.tabs[b.param]);
        if(!ts)continue;
        if(w.native_heaps&&ts.Heap()!=w.native_heaps->resources)continue;
        const uint32_t heap_count=ts.Heap()->GetDesc().NumDescriptors;
        if(ts.base>=heap_count||b.offset>=heap_count-ts.base)continue;
        const HeapKey key{ts.heap,ts.base+b.offset};
        RtPublishedTextureShape d{};
        const auto src=ReadPublishedTextureShape(key,d);
        if(src!=RtPublishedTexture::ready){
            if(r<16){
                char buf[32];
                std::snprintf(buf,sizeof(buf),"t%u=s%u ",r,unsigned(src));
                scan_diag+=buf;
            }
            continue;
        }
        const uint32_t raw_vfmt=uint32_t(PublishedViewFormat(key));
        const uint32_t vfmt=raw_vfmt?raw_vfmt:d.format;
        const bool in_uv=hint.valid&&((hint.uv_srvs&(1ull<<r))!=0);
        const bool in_norm=hint.valid&&((hint.normal_srvs&(1ull<<r))!=0);
        const bool non_cmp=!hint.valid||hint.non_cmp_2d==0||((hint.non_cmp_2d&(1ull<<r))!=0);
        const uint8_t lanes=hint.valid?hint.lane_mask[r]:0;
        if(r<16){
            char lbuf[8]{};
            unsigned lp=0;
            if(lanes){
                lbuf[lp++]='[';
                if(lanes&1)lbuf[lp++]='x';
                if(lanes&2)lbuf[lp++]='y';
                if(lanes&4)lbuf[lp++]='z';
                if(lanes&8)lbuf[lp++]='w';
                lbuf[lp++]=']';
            }
            char buf[80];
            std::snprintf(buf,sizeof(buf),"t%u=%u/%u:%ux%u:m%u%s%s%s ",
                r,d.format,vfmt,d.width,d.height,d.mips,in_uv?"U":"",in_norm?"N":"",lbuf);
            scan_diag+=buf;
        }
        if(!non_cmp||d.arrays!=1||d.width!=d.height)continue;
        const bool is_bgra=(d.format>=87&&d.format<=91)||(vfmt>=87&&vfmt<=91);
        const bool is_rgba=(d.format>=27&&d.format<=32)||(vfmt>=27&&vfmt<=32);
        // UE4 folds uniform material parameters (e.g. translucent Lightmap constants) into a
        // 1x1 BGRA8/RGBA8 Texture2D ("MagicDot" in RabbitFX). Admit it only when DXIL proves
        // direct TEXCOORD UV sampling and reserve it exclusively as a Lightmap fallback.
        const bool is_magic_dot=in_uv&&d.width==1&&d.height==1&&d.mips==1&&(is_bgra||is_rgba);
        if(d.width<64&&!is_magic_dot)continue;

        SlotCand c{};
        c.reg=r;c.param=b.param;c.offset=b.offset;
        c.fmt=d.format;c.vfmt=vfmt;c.w=d.width;c.h=d.height;c.mips=d.mips;
        c.is_bc7=(d.format>=97&&d.format<=99)||(vfmt>=97&&vfmt<=99);
        c.is_bc5=(d.format>=82&&d.format<=84)||(vfmt>=82&&vfmt<=84);
        c.is_bc1=(d.format>=70&&d.format<=72)||(vfmt>=70&&vfmt<=72);
        c.is_bc3=(d.format>=76&&d.format<=78)||(vfmt>=76&&vfmt<=78);
        c.is_r8=(d.format==61||vfmt==61);
        c.is_bgra=is_bgra;
        c.is_rgba=is_rgba;
        c.is_srgb=(vfmt==99||vfmt==72||vfmt==78||vfmt==91||vfmt==29);
        c.is_magic_dot=is_magic_dot;
        c.in_uv=in_uv;c.in_norm=in_norm;c.lanes=lanes;
        if(hint.valid){
            for(unsigned k=0;k<16&&hint.first_order[k];++k){
                if(unsigned(hint.first_order[k]-1)==r){c.order=k;break;}
            }
        }
        const bool mat_ok=c.is_bc7||(c.is_bc5&&c.mips>1)||((c.is_bc1||c.is_bc3)&&c.mips>1)||
                          (c.is_r8&&c.mips>1)||(c.is_bgra&&c.mips>1&&c.w<512)||c.is_magic_dot;
        if(!mat_ok)continue;
        cands.push_back(c);
    }

    if(cands.empty())return RfxPlan::unknown;
    const bool have_uv=std::any_of(cands.begin(),cands.end(),[](const SlotCand& c){return c.in_uv;});
    uint32_t used_mask=0;
    auto pick=[&](auto pred)->const SlotCand* {
        const SlotCand* best=nullptr;
        for(const auto& c:cands){
            if(used_mask&(1u<<c.reg))continue;
            if(!pred(c))continue;
            if(!best||c.w>best->w||(c.w==best->w&&(c.order<best->order||(c.order==best->order&&c.reg<best->reg))))
                best=&c;
        }
        return best;
    };

    const SlotCand* sel[4]{}; // 0=D, 1=N, 2=L, 3=M
    // 1. Normalmap: prefer DXIL 2*v-1 proof, then BC5, then linear UV-sampled BC7.
    if(maps[1].ptr){
        sel[1]=pick([&](const SlotCand& c){return c.in_uv&&c.in_norm&&(c.is_bc7||c.is_bc5)&&!c.is_srgb&&c.mips>1;});
        if(!sel[1])sel[1]=pick([&](const SlotCand& c){return c.in_norm&&(c.is_bc7||c.is_bc5)&&!c.is_srgb&&c.mips>1;});
        if(!sel[1])sel[1]=pick([&](const SlotCand& c){return (!have_uv||c.in_uv)&&c.is_bc5&&c.mips>1;});
        if(!sel[1])sel[1]=pick([&](const SlotCand& c){return (!have_uv||c.in_uv)&&c.is_bc7&&!c.is_srgb&&c.mips>1;});
        if(sel[1])used_mask|=1u<<sel[1]->reg;
    }
    // 2. Diffuse: prefer sRGB BC7 (strict PBR/UE4 BaseColor), then sRGB BGRA/BC1/BC3, then non-normal BC7.
    if(maps[0].ptr){
        sel[0]=pick([&](const SlotCand& c){return (!have_uv||c.in_uv)&&c.is_bc7&&c.is_srgb&&c.mips>1;});
        if(!sel[0])sel[0]=pick([&](const SlotCand& c){return c.is_bc7&&c.is_srgb&&c.mips>1;});
        if(!sel[0])sel[0]=pick([&](const SlotCand& c){return (!have_uv||c.in_uv)&&(c.is_bgra||c.is_bc1||c.is_bc3)&&c.is_srgb&&c.mips>1;});
        if(!sel[0])sel[0]=pick([&](const SlotCand& c){return (!have_uv||c.in_uv)&&c.is_bc7&&c.mips>1&&!c.in_norm;});
        if(sel[0])used_mask|=1u<<sel[0]->reg;
    }
    // 3. Lightmap: prefer BC1/BC3 (standard UE4/Kuro Lightmap), resolution-matched linear non-normal BC7,
    //    or UE4 1x1 UV-sampled BGRA/RGBA MagicDot placeholder when native lightmap is a constant.
    if(maps[2].ptr){
        const uint32_t ref_w=sel[0]?sel[0]->w:(sel[1]?sel[1]->w:0u);
        const uint32_t ref_mips=sel[0]?sel[0]->mips:(sel[1]?sel[1]->mips:0u);
        sel[2]=pick([&](const SlotCand& c){return (!have_uv||c.in_uv)&&(c.is_bc1||c.is_bc3)&&!c.is_srgb&&c.mips>1;});
        if(!sel[2])sel[2]=pick([&](const SlotCand& c){return (!have_uv||c.in_uv)&&(c.is_bc1||c.is_bc3)&&c.mips>1;});
        if(!sel[2])sel[2]=pick([&](const SlotCand& c){return (c.is_bc1||c.is_bc3)&&!c.is_srgb&&c.mips>1;});
        if(!sel[2]&&ref_w)sel[2]=pick([&](const SlotCand& c){
            return (!have_uv||c.in_uv)&&c.is_bc7&&!c.is_srgb&&!c.in_norm&&c.mips>1&&c.w==ref_w&&c.mips==ref_mips;
        });
        if(!sel[2])sel[2]=pick([&](const SlotCand& c){return c.is_magic_dot;});
        if(sel[2])used_mask|=1u<<sel[2]->reg;
    }
    // 4. Materialmap: R8_UNORM (mips>1) or linear single-mip BC7.
    if(maps[3].ptr){
        sel[3]=pick([&](const SlotCand& c){return (!have_uv||c.in_uv)&&((c.is_r8&&c.mips>1)||(c.is_bc7&&!c.is_srgb&&c.mips==1));});
        if(!sel[3])sel[3]=pick([&](const SlotCand& c){return (c.is_r8&&c.mips>1)||(c.is_bc7&&!c.is_srgb&&c.mips==1);});
        if(sel[3])used_mask|=1u<<sel[3]->reg;
    }

    unsigned bound_sig=0;
    for(unsigned i=0;i<4;++i){
        if(!sel[i])continue;
        binds.push_back({sel[i]->reg,sel[i]->param,sel[i]->offset,maps[i],uint8_t(1u<<i)});
        if(sel[i]->reg<8){
            facts.fresh[i]|=uint8_t(1u<<sel[i]->reg);
            facts.masks[i]|=uint8_t(1u<<sel[i]->reg);
        }
        bound_sig|=((sel[i]->reg+1)&0x3Fu)<<(i*6);
    }

    {
        static std::mutex s_log_mtx;
        static std::set<std::tuple<int,int,uint64_t,unsigned>> s_logged;
        std::lock_guard lk(s_log_mtx);
        if(s_logged.size()<64&&s_logged.emplace(mod_idx,comp_idx,ps_hash,bound_sig).second){
            std::string ord_diag;
            if(hint.valid){
                for(unsigned k=0;k<16&&hint.first_order[k];++k){
                    char obuf[12];
                    std::snprintf(obuf,sizeof(obuf),"%s%u",k?",":"",unsigned(hint.first_order[k]-1));
                    ord_diag+=obuf;
                }
            }
            auto fmt_sel=[](const SlotCand* c)->int {return c?int(c->reg):-1;};
            logger::probe("RFX-HIGHSLOT-PLAN mod=%d comp=%d ps=%016llx decl=%x hint=%d(uv=%llx,n=%llx,ord=%s) pick=D:t%d,N:t%d,L:t%d,M:t%d scan=\"%s\"",
                mod_idx+1,comp_idx,(unsigned long long)ps_hash,ps_reads_32,int(hint.valid),
                (unsigned long long)hint.uv_srvs,(unsigned long long)hint.normal_srvs,ord_diag.c_str(),
                fmt_sel(sel[0]),fmt_sel(sel[1]),fmt_sel(sel[2]),fmt_sel(sel[3]),scan_diag.c_str());
        }
    }

    if(binds.empty())return RfxPlan::unknown;
    facts.step=RtRfxStep::plan_ready;
    return RfxPlan::ready;
}

static RfxPlan ResolveRabbitFxPlan(reshade::api::command_list* list,Param0Window& w,
    int mod_idx,int comp_idx,const std::array<D3D12_CPU_DESCRIPTOR_HANDLE,4>& maps,
    bool gbuffer,uint32_t reads_32,std::vector<RfxBoundSlot>& binds,RtRfxDrawFacts& facts)
{
    facts.reads=uint8_t(reads_32&0x0Fu);
    if(!g_rabbitfx_tex){facts.step=RtRfxStep::disabled;return RfxPlan::empty;}
    if(g_rabbitfx_tex_gbuffer&&!gbuffer){facts.step=RtRfxStep::low_mrt;return RfxPlan::empty;}
    const bool has_low3=(reads_32&7u)==7u;
    const bool has_high=(reads_32&~7u)!=0u;
    if(g_rabbitfx_tex_main&&!has_low3&&!has_high){facts.step=RtRfxStep::ps_reads;return RfxPlan::empty;}
    if(!ResolveParam0Window(list,w)){facts.step=RtRfxStep::window;return RfxPlan::unknown;}
    const auto regs=RfxPsLocations(list);
    for(unsigned i=0;i<4;++i)if(regs[i].is_table)facts.ps_locations[i]={regs[i].param,regs[i].offset};
    facts.locations_known=true;
    if(has_low3){
        const auto classic=PlanRabbitFxMaps(w,regs,maps,binds,facts);
        if(classic==RfxPlan::ready)return RfxPlan::ready;
        if(has_high&&RfxLowSlotsAreEnginePrefix(facts)){
            const auto saved_step=facts.step;
            binds.clear();
            const auto hi=PlanRabbitFxHighSlots(list,w,mod_idx,comp_idx,maps,reads_32,binds,facts);
            if(hi==RfxPlan::ready)return RfxPlan::ready;
            facts.step=saved_step;
        }
        return classic;
    }
    if(has_high){
        const auto hi=PlanRabbitFxHighSlots(list,w,mod_idx,comp_idx,maps,reads_32,binds,facts);
        if(hi==RfxPlan::ready)return RfxPlan::ready;
        facts.step=RtRfxStep::ps_reads;
        return RfxPlan::empty;
    }
    facts.step=RtRfxStep::ps_reads;
    return RfxPlan::empty;
}

RfxBindState RfxDecideAndBind(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl,
    int mod_idx, int comp_idx,
    D3D12_CPU_DESCRIPTOR_HANDLE dif, D3D12_CPU_DESCRIPTOR_HANDLE nor, D3D12_CPU_DESCRIPTOR_HANDLE lit,
    D3D12_CPU_DESCRIPTOR_HANDLE mat, bool gbuffer_pass, uint8_t ps_reads, const std::vector<WclSlotOvr>* extra)
{
    RfxBindState st{};
    const int bail_key = (mod_idx << 16) | (comp_idx & 0xFFFF);
    if (!cmd_list || !cl) {
        if (g_debug && DiagOnce("RFX-BAIL-GATE", bail_key))
            logger::write("RABBITFX-BAIL mod=%d comp=%d: cmd_list=%p cl=%p", mod_idx, comp_idx, (void*)cmd_list, (void*)cl);
        return st;
    }
    if (!dif.ptr && !nor.ptr && !lit.ptr && !mat.ptr) {
        if (g_debug && DiagOnce("RFX-BAIL-NOMAPS", bail_key))
            logger::write("RABBITFX-BAIL mod=%d comp=%d: no diffuse/normal/light SRV resolved", mod_idx, comp_idx);
        return st;
    }
    const auto full_reads=PipelineSrvRegs(CmdGfx(cmd_list).gfx_pso,'p');
    const uint32_t ps_reads_32=full_reads.parsed?uint32_t(full_reads.lo):uint32_t(ps_reads);
    Param0Window w;
    std::vector<RfxBoundSlot> semantic;
    RtRfxDrawFacts facts;
    if(ResolveRabbitFxPlan(cmd_list,w,mod_idx,comp_idx,{dif,nor,lit,mat},gbuffer_pass,ps_reads_32,semantic,facts)!=RfxPlan::ready){
        if(g_debug&&DiagChanged("RABBITFX-PLAN-SKIP",bail_key,unsigned(facts.step)))
            logger::write("RABBITFX-PLAN-SKIP mod=%d comp=%d step=%u valid=%x present=%x mapped=%x",
                mod_idx,comp_idx,unsigned(facts.step),unsigned(facts.native_valid),
                unsigned(facts.native_present),unsigned(facts.native_mapped));
        return st; // ordinary caller retains the existing fallback/explicit path
    }
    // The shared plan supplies physical SRV offsets. Ordinary extras and
    // caller stamps follow it, in the same order as their previous binding.
    const std::vector<WclSlotOvr> none;
    st=BindThroughShadow(cmd_list,cl,w,extra?*extra:none,t_extra,nullptr,nullptr,semantic);
    if (st.applied) ++g_rabbitfx_tex_count;
    return st;
}

bool RtFreezeRabbitFxSlots(reshade::api::command_list* list, int mod, int component,
    const RfxTexLatch& maps, const RtRfxTextureSources& sources, bool gbuffer, std::vector<ShadowSlotOvr>& out,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& textures, std::vector<uint64_t>& ps_owners, RtRfxDrawFacts& facts)
{
    if (g_disable_tex || !g_rabbitfx_tex) {facts.step=RtRfxStep::disabled;return true;}
    if (!maps.d.ptr && !maps.n.ptr && !maps.l.ptr && !maps.m.ptr) {
        facts.step=!facts.called?RtRfxStep::no_call:
            (facts.requested&~facts.resolved)?RtRfxStep::pending:RtRfxStep::no_maps;
        return true;
    }
    const auto reads=PipelineSrvRegs(CmdGfx(list).gfx_pso,'p');
    const uint32_t ps_reads_32=uint32_t(reads.lo);
    Param0Window w;
    std::vector<RfxBoundSlot> binds;
    const auto plan=ResolveRabbitFxPlan(list,w,mod,component,{maps.d,maps.n,maps.l,maps.m},gbuffer,ps_reads_32,binds,facts);
    if(plan==RfxPlan::empty)return true; // still run the caller's explicit ps-tN
    if(plan!=RfxPlan::ready)return false;
    for(const auto& b:binds){
        const auto& owner=sources[b.role==1?0:b.role==2?1:b.role==4?2:3];
        if(owner.srv.ptr!=b.srv.ptr||!owner.resource){facts.step=RtRfxStep::owner;return false;}
        if(facts.bound_count<facts.bound_role.size())facts.bound_role[facts.bound_count++]=b.role;
        out.push_back({b.param,b.offset,b.srv});
        textures.push_back(owner.resource);
        ps_owners.push_back(reinterpret_cast<uint64_t>(owner.resource.Get()));
    }
    return true;
}
void RfxRestore(ID3D12GraphicsCommandList* cl, const RfxBindState& st)
{
    if (!st.bound || !cl) return;
    if(!RtRestoreDrawHeaps(cl)&&(st.game_srv||st.game_smp))
        RtSetDrawHeaps(cl,st.game_srv,st.game_smp);
    for (const auto& t : st.tables) cl->SetGraphicsRootDescriptorTable(t.first, { t.second }); // restore ALL tables
}

bool RtPrepareMaterialTables(reshade::api::command_list* list,const std::vector<ShadowSlotOvr>& overrides,RtMaterialTables& result){
    if(!list || overrides.empty())return false;
    Param0Window w;if(!ResolveParam0Window(list,w)){result.failure=6;return false;}result.failure=7;
    result.original=BindThroughShadow(list,nullptr,w,{},overrides,&result);
    if(result.original.applied)result.failure=0;return result.original.applied;
}
bool RtPrepareRecordedMaterialTables(reshade::api::command_list* list,const std::vector<ShadowSlotOvr>& overrides,
                                     RtRecordedMaterial& recording,RtMaterialTables& result){
    if(!list||overrides.empty())return false;
    RtRecordedMaterialWindow window;
    if(!ResolveParam0Window(list,window.params)){result.failure=6;return false;}
    return RtPrepareRecordedMaterialTablesInWindow(list,window,overrides,recording,result);
}
std::shared_ptr<const RtRecordedMaterialWindow> RtReadRecordedMaterialWindow(reshade::api::command_list* list){
    auto out=std::make_shared<RtRecordedMaterialWindow>();
    if(!ResolveParam0Window(list,out->params))return {};
    return out;
}
bool RtPrepareRecordedMaterialTablesInWindow(reshade::api::command_list* list,const RtRecordedMaterialWindow& window,
    const std::vector<ShadowSlotOvr>& overrides,RtRecordedMaterial& recording,RtMaterialTables& result){
    if(overrides.empty())return false;
    rt_phase_cpu::Scope material_cpu(rt_phase_cpu::material);
    result.failure=7;
    // The caller owns overrides and scratch through synchronous reserve and copy.
    result.original=BindThroughShadow(list,nullptr,window.params,{},overrides,&result,&recording);
    if(result.original.applied)result.failure=0;return result.original.applied;
}
void RtBindMaterialTables(ID3D12GraphicsCommandList* cl,const RtMaterialTables& prepared){
    // Preparation already proved the live original heap pair before commit.
    RtBindPreparedDrawHeaps(cl,prepared.heap.Get(),prepared.original.game_smp);
    for(auto& [p,gpu]:prepared.tables)cl->SetGraphicsRootDescriptorTable(p,gpu);
}
