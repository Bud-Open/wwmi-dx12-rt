#include "rt_stream_section.h"
#include "rt_pack_frame.h"
extern thread_local uint64_t wwmi_rt_section_token;
#include "../observation/producer/semantic_capture_adapter.hpp"
#include "../observation/binding_client/wwmi_binding_client.hpp"
// draw_section_draw.cpp -- a component section's `drawindexed`: the game's PSO and tables bent the way
// the section asks (blend variant, its ps-tN, RabbitFX maps and shader patch), the draw issued, and put
// back. See draw_inject.h.
#include "../addon_common.h"
#include "draw_hook.h"
#include "../instrumentation/removal_pin.h"
#include "../mods/claim_state.h"
#include "../instrumentation/occlusion_probe.h"
#include "../instrumentation/gpu_timer.h"
#include "../mods/skeleton.h"
#include "../mods/port_section.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "../rabbitfx/rabbitfx_bind.h"
#include "../hooks/pipeline_hooks.h"
#include "../rt/rt_hooks.h"
#include "../rt/rt_capture.h"
#include "../migoto/dx12_input_layout.h"
#include "../instrumentation/draw_dump_publish.h"
#include "draw_ctx.h"
#include "draw_parts.h"
#include "draw_inject.h"
#include "../observation/wwmi_executed_draw.h"
#include "../mods/wwmi_mod.h"
#include "../hooks/dxil_psv.h" // DxilSrvRegs -- A8-SKIP's PS texture count

static RfxGlowState WithSectionRfxMaps(const RfxGlowState& g, int dm, int comp, uint32_t srv_gen0)
{
    if (g.fxmap.ptr || g.active) return g;
    float on[4];
    if (!PortApi::GetIniParam(kRfxIniOnParam, on) || on[1] == 0.f) return g;
    const MigotoPort::PipeState& st = MigotoPort::State();
    const int                    si = MigotoPort::StageIndex('p');
    auto                         view = [&](unsigned reg) {
        if ((int32_t)(st.srv_gen[si][reg] - srv_gen0) <= 0) return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        MigotoPort::WrapView* wv = st.srv[si][reg] ? MigotoPort::AsWrapView(st.srv[si][reg]) : nullptr;
        return (wv && wv->kind == 't') ? wv->cpu : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
    };
    float shift[4];
    PortApi::GetIniParam(kRfxIniShiftParam, shift);
    RfxGlowState s = g;
    s.fxmap = view(kRfxCutoutReg);
    s.map = view(kRfxGlowReg);
    s.hue = shift[0];
    s.sat = shift[1];
    s.val = shift[2];
    s.brightness = shift[3];
    s.active = s.map.ptr != 0;
    if ((s.fxmap.ptr || s.active) && g_debug && DiagOnce("RFX-SECTION-MAPS", CompKey(dm, comp)))
        logger::write("RFX-SECTION-MAPS mod=%d comp=%d: the section bound RabbitFX's ps-t%u / ps-t%u itself with y%u on -- "
                      "fx map %s, glow map %s",
            dm, comp, kRfxGlowReg, kRfxCutoutReg, kRfxIniOnParam, s.fxmap.ptr ? "bound" : "none", s.active ? "bound" : "none");
    return s;
}

void SectionDrawIndexed(SectionDraw& sd, uint32_t c, uint32_t s2, uint32_t b2)
{
    const uint64_t wwmi_observer_session = wwmi::executed_draw::SnapshotSession();
    const uint32_t wwmi_requested_count = wwmi_observer_session ? c : 0;
    const uint32_t wwmi_requested_first = wwmi_observer_session ? s2 : 0;
    const uint32_t wwmi_requested_base_bits = wwmi_observer_session ? b2 : 0;
    InjectFrame&                      fr = sd.fr;
    const DrawArgs&                   da = fr.da;
    DrawClaim&                        dc = fr.dc;
    const int                         dm = sd.dm;
    const WwmiMod::Component* const   wc = sd.wc;
    const PortHooks&                  hk = sd.hk;
    uint32_t&                         did = sd.did;
    const RfxGlowState                rfx_glow = g_disable_tex ? RfxGlowState{} : WithSectionRfxMaps(sd.rfx_glow, sd.dm, sd.wc ? (int)sd.wc->index : -1, sd.hk.srv_gen0);
    const RfxTexLatch                 rfx_tex = g_disable_tex ? RfxTexLatch{} : sd.rfx_tex;
    ID3D12Resource*&                  x24_vb6_res = sd.x24_vb6_res;
    uint64_t&                         x24_vb6_va = sd.x24_vb6_va;
    uint32_t&                         x24_vb6_bytes = sd.x24_vb6_bytes;
    uint8_t&                          x24_vb6_verdict = sd.x24_vb6_verdict;
    bool&                             rtpal_no_merge = sd.rtpal_no_merge;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12GraphicsCommandList* const  d3d12_cl = da.d3d12_cl;
    const uint32_t                    index_count = da.index_count;
    const uint32_t                    first_index = da.first_index;
    const int32_t                     vertex_offset = da.vertex_offset;
    const uint32_t                    first_instance = da.first_instance;
    const uint64_t                    vs_hash = da.vs_hash;
    const CmdRT&                      draw_rt = da.draw_rt;
    MergedComp*&                      mc = dc.mc;
    uint64_t&                         game_cb13_va = dc.cb13_va;
    uint64_t&                         game_cb14_va = dc.cb14_va;
    ID3D12Resource*&                  port_cb4_res = dc.port_cb4_res;
    int&                              game_cb13_root = dc.cb13_root;
    int&                              game_cb14_root = dc.cb14_root;
    bool&                             is_full_shadow_pass = dc.full_shadow;
    const bool                        cb_ok = fr.cb_ok;
    PROF(PROF_DI_PsDraw);
    (void)b2;
    auto a8_note = [&](uint64_t pso) {
        uint64_t ps_hash = 0;
        cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
        static std::mutex                   s_mtx;
        static std::unordered_set<uint64_t> s_seen;
        {
            std::lock_guard l(s_mtx);
            if (s_seen.size() >= 256 || !s_seen.insert(vs_hash ^ (ps_hash * 0x9E3779B97F4A7C15ull)).second) return;
        }
        const DxilSrvRegs pr = PipelineSrvRegs(pso, 'p');
        int               srvs = 0;
        for (unsigned r = 0; r < 128; ++r) srvs += pr.has(r) ? 1 : 0;
        logger::write("A8-SKIP mod=%d comp=%d vs=%016llx ps=%016llx pso=%llx rtv=%u dsv=%d idx=%u+%u ps_srvs=%d%s%s",
            dm, mc ? mc->comp_idx : -1, (unsigned long long)vs_hash, (unsigned long long)ps_hash,
            (unsigned long long)pso, draw_rt.rtv_count, (int)draw_rt.has_dsv, s2, c, srvs,
            pr.parsed ? "" : " (no PSV)", pr.unbounded ? " +unbounded" : "");
    };
    if (c == 0xFFFFFFFFu) {
        c = index_count;
        s2 = first_index;
    } // drawindexed=auto
    if (mc && mc->hide) return;
    if (g_hide_outline) { // A8: the outline shell is the front-culling pass
        reshade::api::cull_mode cm = reshade::api::cull_mode::none;
        const uint64_t          pso = CmdGfx(cmd_list).gfx_pso;
        if (PipelineCullMode(pso, &cm) && cm == reshade::api::cull_mode::front) {
            a8_note(pso);
            return;
        }
    }
    if (RtQueuePrivateSubdraw(sd,c,s2,int32_t(b2))) return;
    MigotoPort::FlushUavBarriers(d3d12_cl);
    const VfRestore        vf_restore = RtSkinDraw(cmd_list, d3d12_cl, dm, wc, c, s2, first_index, index_count,
        vertex_offset, vs_hash, game_cb13_va, game_cb13_root,
        rtpal_no_merge);
    const MigotoPort::LayoutOverride layout_ovr = MigotoPort::State().layout_override;
    ID3D12PipelineState*             blend_pso = nullptr;
    uint64_t                         restore_pso = 0;
    MigotoPort::StateDesc*           cs_blend =
        MigotoPort::AsStateDesc(MigotoPort::State().blend);
    if (cs_blend) {
        if (!g_custom_subdraw) return;
        ++g_custom_blend_count;
        if (g_custom_blend && !layout_ovr.count) {
            {
                restore_pso = CmdGfx(cmd_list).gfx_pso;
            }
            blend_pso = GetBlendVariantFromDesc(cmd_list->get_device(), restore_pso,
                &cs_blend->blend,
                MigotoPort::State().blend_factor, g_decensor);
            if (blend_pso) {
                wwmi::semantic::SetPipelineState(d3d12_cl, blend_pso, wwmi::semantic::SectionBlendPso);
                const FLOAT* bf = MigotoPort::State().blend_factor;
                float        fac[4];
                for (int i2 = 0; i2 < 3; ++i2)
                    fac[i2] = (g_custom_blend_factor >= 0.f) ? g_custom_blend_factor : bf[i2];
                fac[3] = bf[3];
                d3d12_cl->OMSetBlendFactor(fac);
                ++g_custom_blend_bound_count;
            }
            if (g_debug) {
                static std::atomic<bool> once{ false };
                bool                     e = false;
                const FLOAT*             dbf = MigotoPort::State().blend_factor;
                if (once.compare_exchange_strong(e, true))
                    logger::write("CUSTOMBLEND from PipeState: enable=%d op=%d src=%d dst=%d "
                                  "factor=%.3f,%.3f,%.3f,%.3f variant=%p",
                        (int)cs_blend->blend.RenderTarget[0].BlendEnable,
                        (int)cs_blend->blend.RenderTarget[0].BlendOp,
                        (int)cs_blend->blend.RenderTarget[0].SrcBlend,
                        (int)cs_blend->blend.RenderTarget[0].DestBlend,
                        dbf[0], dbf[1], dbf[2], dbf[3], (void*)blend_pso);
            }
        }
    }
    ID3D12PipelineState* layout_pso = nullptr;
    if (layout_ovr.count) {
        const uint64_t src = CmdGfx(cmd_list).gfx_pso;
        if (layout_ovr.pso == src)
            layout_pso = GetInputLayoutVariant(cmd_list->get_device(), src, layout_ovr.elements, layout_ovr.count, g_decensor, false);
        else if (DiagOnce("ILAYOUT-PSO", CompKey(dm, wc ? (int)wc->index : 0)))
            logger::write("ILAYOUT mod=%d comp=%d: the layout override was made for pso=%llx, this draw has pso=%llx -- drawn with the game's layout",
                dm, wc ? (int)wc->index : -1, layout_ovr.pso, (unsigned long long)src);
        if (layout_pso) wwmi::semantic::SetPipelineState(d3d12_cl, layout_pso, wwmi::semantic::SectionLayoutPso);
        if (cs_blend && g_custom_blend && DiagOnce("ILAYOUT-BLEND", CompKey(dm, wc ? (int)wc->index : 0)))
            logger::write("ILAYOUT mod=%d comp=%d: the section's blend is not applied -- the input-layout override takes the PSO",
                dm, wc ? (int)wc->index : -1);
    }
    const SectionPsStamps sect_ps = PlanSectionPsSlots(cmd_list, hk.srv_gen0, hk.this_gen, dm, wc ? (int)wc->index : -1);
    sect_ps.QueueForShadow();
    if (!sect_ps.shared) sect_ps.StampInPlace();
    ID3D12PipelineState*    rfxp_pso = nullptr;
    uint64_t                rfxp_restore = 0;
    bool                    rfxp_glow = false; // the variant carries the glow block AND this component armed it
    std::vector<WclSlotOvr> rfxp_binds;
    const bool              rfx_censor = g_decensor;
    if (g_rfx_patch && (rfx_glow.fxmap.ptr || rfx_glow.active) && !is_full_shadow_pass) {
        if (blend_pso) {
            if (DiagOnce("RFXPATCH-BLEND", CompKey(dm, wc ? (int)wc->index : 0)))
                logger::write("RFXPATCH mod=%d comp=%d: a blend variant is active on this draw -- the patch is not applied to it",
                    dm, wc ? (int)wc->index : -1);
        } else {
            rfxp_restore = CmdGfx(cmd_list).gfx_pso;
            unsigned ps_max = 0;
            unsigned win_base = 0;
            unsigned win_count = 0;
            bool     has_glow = false;
            rfxp_pso = GetRfxPatchVariant(cmd_list->get_device(), rfxp_restore, &ps_max, &has_glow, rfx_censor);
            if (rfxp_pso && layout_pso) {
                rfxp_pso = GetInputLayoutVariant(cmd_list->get_device(), rfxp_restore, layout_ovr.elements, layout_ovr.count,
                    rfx_censor, true);
                if (!rfxp_pso && DiagOnce("RFXPATCH-LAYOUT", CompKey(dm, wc ? (int)wc->index : 0)))
                    logger::write("RFXPATCH mod=%d comp=%d: no input-layout variant carrying the patched PS -- drawn unpatched",
                        dm, wc ? (int)wc->index : -1);
            }
            const unsigned top_reg = has_glow ? kRfxParamsReg : kRfxCutoutReg;
            if (rfxp_pso && !RfxParam0Covers(cmd_list, ps_max > top_reg ? ps_max : top_reg, &win_base, &win_count)) {
                if (DiagOnce("RFXPATCH-WINDOW", (int)(((rfxp_restore ^ (rfxp_restore >> 32)) * 31u) & 0x7fffffff)))
                    logger::write("RFXPATCH mod=%d comp=%d pso=%llx: the PS reads up to t%u and the patch needs t%u, the draw's first SRV table is t%u x%u -- drawn unpatched",
                        dm, wc ? (int)wc->index : -1, (unsigned long long)rfxp_restore, ps_max, top_reg, win_base, win_count);
                rfxp_pso = nullptr;
                ++g_rfx_patch_nobind;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE null_tex{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE null_buf{ 0 };
            if (rfxp_pso && !RfxPatchNullSrvs(&null_tex, &null_buf)) {
                rfxp_pso = nullptr;
                ++g_rfx_patch_nobind;
            }
            if (rfxp_pso) {
                rfxp_binds.push_back({ (int)kRfxCutoutReg, rfx_glow.fxmap.ptr ? rfx_glow.fxmap : null_tex, false });
                if (has_glow) {
                    D3D12_CPU_DESCRIPTOR_HANDLE prm{ 0 };
                    if (rfx_glow.active) prm = RfxPatchParamsSrv(rfx_glow.hue, rfx_glow.sat, rfx_glow.val, rfx_glow.brightness);
                    rfxp_glow = (prm.ptr != 0);
                    rfxp_binds.push_back({ (int)kRfxGlowReg, rfxp_glow ? rfx_glow.map : null_tex, false });
                    rfxp_binds.push_back({ (int)kRfxParamsReg, rfxp_glow ? prm : null_buf, false });
                }
            }
        }
    }
    RfxBindState tbind{};
    if (rfx_tex.d.ptr || rfx_tex.n.ptr || rfx_tex.l.ptr || rfx_tex.m.ptr) {
        PROF(PROF_DI_RfxDecide);
        const DxilSrvRegs ps_regs = PipelineSrvRegs(CmdGfx(cmd_list).gfx_pso, 'p');
        uint8_t           ps_reads = 0;
        for (unsigned r = 0; r < 4; ++r)
            if (ps_regs.reads(r)) ps_reads |= (uint8_t)(1u << r);
        tbind = RfxDecideAndBind(cmd_list, d3d12_cl, dm, wc ? (int)wc->index : -1, rfx_tex.d, rfx_tex.n, rfx_tex.l, rfx_tex.m,
            draw_rt.rtv_count >= 4, ps_reads, rfxp_pso ? &rfxp_binds : nullptr);
    }
    if (rfxp_pso) {
        if (!tbind.applied) tbind = ApplyDrawBindings(cmd_list, d3d12_cl, rfxp_binds, /*guard_bindless*/ false);
        const bool bound = tbind.applied;
        if (DiagOnce("RFXPATCH-DRAW", (int)((((rfxp_restore ^ (rfxp_restore >> 32)) * 31u) + (uint64_t)(wc ? wc->index : 0)) & 0x7fffffff)))
            logger::write("RFXPATCH draw mod=%d comp=%d pso=%llx rtv=%u -> variant %p, fx map %s, glow %s: %s",
                dm, wc ? (int)wc->index : -1, (unsigned long long)rfxp_restore, draw_rt.rtv_count,
                (void*)rfxp_pso, rfx_glow.fxmap.ptr ? "bound" : "null",
                rfxp_glow ? "bound" : (rfx_glow.active ? "ARMED BUT NOT IN THIS VARIANT" : "null"),
                bound ? "BOUND" : "NOT bindable (no param0 window: RABBITFX-BAIL above) -- drawn unpatched");
        if (bound) {
            wwmi::semantic::SetPipelineState(d3d12_cl, rfxp_pso, wwmi::semantic::SectionRfxPso);
            ++g_rfx_patch_draws;
            if (rfxp_glow) ++g_rfx_glow_count;
        } else {
            rfxp_pso = nullptr;
            rfxp_glow = false;
            ++g_rfx_patch_nobind;
        }
    }
    if (sect_ps.shared && !tbind.applied) {
        tbind = ApplyDrawBindings(cmd_list, d3d12_cl, {}, /*guard_bindless*/ false);
        if (!tbind.applied) {
            if (DiagOnce("PS-SLOT-SHARED-INPLACE", CompKey(dm, wc ? (int)wc->index : 0)))
                logger::write("PS-SLOT-STAMP mod=%d comp=%d: no private table for this draw (RABBITFX-BAIL above) -- its "
                              "ps-tN stamped in place over the earlier draw's",
                    dm, wc ? (int)wc->index : -1);
            sect_ps.StampInPlace();
        }
    }
    SetShadowExtraSlots(nullptr, 0); // RTFETCH's slots belonged to this draw's shadow bind only
    {
        auto*        odev = reinterpret_cast<ID3D12Device*>(cmd_list->get_device()->get_native());
        const CmdRT& ort = draw_rt;
        const int    oq = g_occl.Begin(d3d12_cl, odev, wc ? (int)wc->index : -1, c,
            ort.rtv_count, ort.has_dsv);
        if (g_vbdiag_frames.load(std::memory_order_relaxed) > 0) {
            uint64_t gva[8] = {};
            uint32_t gstr[8] = {};
            uint32_t gsz[8] = {};
            {
                auto& gs = CmdGfx(cmd_list);
                for (unsigned s = 0; s < 8; ++s) {
                    gva[s] = gs.vb[s].va;
                    gstr[s] = gs.vb[s].stride;
                    gsz[s] = gs.vb[s].size;
                }
            }
            char vbs[1024];
            int  off = 0;
            for (unsigned s = 0; s < 8 && off < (int)sizeof(vbs) - 128; ++s) {
                void*              r = nullptr;
                unsigned long long va = 0;
                unsigned           sz = 0;
                unsigned           str = 0;
                unsigned           stt = 0;
                const bool         got = PortApi::GetShimVertexBuffer(s, &r, &va, &sz, &str, &stt);
                off += _snprintf_s(vbs + off, sizeof(vbs) - off, _TRUNCATE,
                    " vb%u[shim=%d va=%llx str=%u | game va=%llx sz=%u str=%u]",
                    s, (int)got, va, str,
                    (unsigned long long)gva[s], gsz[s], gstr[s]);
            }
            logger::write("VBDIAG mod=%d comp=%d is_shadow=%d first=%u count=%u "
                          "vs=%016llx b4root=%d b4va=%llx |%s",
                dm, wc ? wc->index : -1, (int)is_full_shadow_pass, s2, c,
                vs_hash, game_cb13_root,
                (unsigned long long)game_cb13_va, vbs);
            D3D12_VIEWPORT vp0{};
            D3D12_RECT     sc0{};
            uint32_t       vpn = 0;
            uint32_t       scn = 0;
            uint32_t       topo = 0;
            uint32_t       sref = 0;
            uint64_t       pso = 0;
            char           roots[256] = {};
            int            ro = 0;
            {
                auto& gs = CmdGfx(cmd_list);
                vpn = gs.vp_count;
                scn = gs.scissor_count;
                topo = gs.topology;
                sref = gs.stencil_ref;
                pso = gs.gfx_pso;
                if (vpn) vp0 = gs.vp[0];
                if (scn) sc0 = gs.scissor[0];
                for (int p = 0; p < 16 && ro < (int)sizeof(roots) - 24; ++p)
                    if (gs.root[p].va)
                        ro += _snprintf_s(roots + ro, sizeof(roots) - ro, _TRUNCATE,
                            " r%d=%llx", p,
                            (unsigned long long)gs.root[p].va);
            }
            logger::write("VBDIAG2 mod=%d comp=%d is_shadow=%d vp=%u[%.0f,%.0f %.0fx%.0f "
                          "z=%.3f..%.3f] sc=%u[%ld,%ld %ldx%ld] topo=%u sref=%u pso=%llx |%s",
                dm, wc ? wc->index : -1, (int)is_full_shadow_pass,
                vpn, vp0.TopLeftX, vp0.TopLeftY, vp0.Width, vp0.Height,
                vp0.MinDepth, vp0.MaxDepth,
                scn, (long)sc0.left, (long)sc0.top,
                (long)(sc0.right - sc0.left), (long)(sc0.bottom - sc0.top),
                topo, sref, (unsigned long long)pso, roots);
        }
        if (MorphDump::Armed() || MigotoPort::FrameAnalysisArmed()) {
            X27Cb          cbs[8];
            const uint64_t layh = X27ResolveVsCbs(cmd_list, cbs);
            char           desc[420];
            X27Format(desc, sizeof desc, cbs);
            uint64_t pso_now = 0;
            {
                pso_now = CmdGfx(cmd_list).gfx_pso;
            }
            logger::write("X27 CBMAP mod=%d comp=%d is_shadow=%d vs=%016llx list=%p layout=%llx pso=%llx "
                          "port: b4root=%d b3root=%d rfx_bound=%d glow=%d blend_pso=%d |%s",
                dm, wc ? wc->index : -1, (int)is_full_shadow_pass,
                (unsigned long long)vs_hash, (void*)d3d12_cl,
                (unsigned long long)layh, (unsigned long long)pso_now,
                game_cb13_root, game_cb14_root, (int)tbind.bound,
                (int)rfx_glow.active, (int)(blend_pso != nullptr),
                desc[0] ? desc : " (layout not mapped)");
            if (DiagOnce("X27-LAYOUT", (int)(layh ^ (layh >> 32)))) {
                std::vector<LayoutTableParam> lt;
                {
                    std::lock_guard l(g_layout_tables_mtx);
                    auto            it = g_layout_tables.find(layh);
                    if (it != g_layout_tables.end()) lt = it->second;
                }
                char   conf[96] = "";
                size_t cn = 0;
                for (int r = 0; r < 8; ++r) {
                    if (cbs[r].kind != 1) continue;
                    for (auto const& t : lt) {
                        if (t.param != cbs[r].param || cn + 12 >= sizeof(conf)) continue;
                        const int k = _snprintf_s(conf + cn, sizeof(conf) - cn, _TRUNCATE, " b%d/p%u", r, (unsigned)t.param);
                        if (k > 0) cn += (size_t)k;
                    }
                }
                logger::write("X27 LAYOUT %llx: %zu table params recorded; root-CBV params also listed as tables:%s",
                    (unsigned long long)layh, lt.size(), cn ? conf : " none");
            }
        }
        if (MigotoPort::FrameAnalysisArmed()) {
            uint64_t fd_pso = 0;
            {
                fd_pso = CmdGfx(cmd_list).gfx_pso;
            }
            MigotoPort::SetCurrentInputLayout(fd_pso);
            PublishGameBoundStateForDump(cmd_list); // depth: always, 1/draw
            PublishGameTexturesForDump(cmd_list);
            logger::write("DUMPIDX %06u mod=%d comp=%d is_shadow=%d first=%u count=%u "
                          "vs=%016llx cb4=%p b4root=%d b4va=%llx b3root=%d b3va=%llx "
                          "cb_ok=%d",
                MigotoPort::FrameAnalysisDrawIndex(), dm,
                wc ? wc->index : -1, (int)is_full_shadow_pass,
                s2, c, vs_hash, (void*)port_cb4_res,
                game_cb13_root, (unsigned long long)game_cb13_va,
                game_cb14_root, (unsigned long long)game_cb14_va,
                (int)cb_ok);
            DrawCallInfo fci{};
            fci.type = DrawCall::DrawIndexedInstanced;
            fci.IndexCount = c;
            fci.InstanceCount = 1;
            fci.FirstIndex = s2;
            fci.FirstInstance = first_instance;
            uint64_t fa_ps = 0;
            cmd_list->get_private_data(s_iid_ps_hash, &fa_ps);
            PublishGameDrawForDump(cmd_list, /*injected*/ true, x24_vb6_res, x24_vb6_va,
                x24_vb6_bytes, x24_vb6_verdict);
            MigotoPort::FrameAnalysisAfterDraw(false, &fci, vs_hash, fa_ps, 0);
            PublishGameDrawForDumpEnd();
        }
        {
            uint64_t x20_pso = 0;
            {
                x20_pso = CmdGfx(cmd_list).gfx_pso;
            }
            const int x20_key = (int)((((x20_pso ^ (x20_pso >> 32)) * 31u) + (uint64_t)(wc ? wc->index : 0)) & 0x7fffffff);
            if (DiagOnce("X20-LAYOUT", x20_key)) {
                char           lay[1024];
                const unsigned ne = MigotoPort::DescribeInputLayout(x20_pso, lay, sizeof(lay));
                logger::write("X20 LAYOUT mod=%d comp=%d vs=%016llx pso=%llx is_shadow=%d elems=%u: %s",
                    dm, wc ? (int)wc->index : -1, (unsigned long long)vs_hash,
                    (unsigned long long)x20_pso, (int)is_full_shadow_pass, ne,
                    ne ? lay : "(layout not recorded for this PSO)");
            }
            if (const wchar_t* fa_dir = MigotoPort::FrameAnalysisFolder()) {
                const unsigned nw = PsoCloneWriteShaders(x20_pso, fa_dir);
                if (nw) logger::write("PSOSHADERS pso=%llx: %u shader file(s) added to the capture", (unsigned long long)x20_pso, nw);
            }
            if (DiagOnce("DSSTATE", (int)(((x20_pso ^ (x20_pso >> 32)) * 31u) & 0x7fffffff))) {
                char           dss[512];
                const unsigned dn = PsoCloneDescribeDepthStencil(x20_pso, dss, sizeof(dss));
                logger::write("DSSTATE pso=%llx is_shadow=%d rtv=%u stencil_ref_at_draw=%u: %s",
                    (unsigned long long)x20_pso, (int)is_full_shadow_pass,
                    draw_rt.rtv_count, CmdGfx(cmd_list).stencil_ref,
                    dn ? dss : "(PSO not captured -- no clone, or no depth_stencil_state subobject)");
            }
        }
        if (mc && mc->freeze && g_identity_cb_va &&
            game_cb13_root >= 0 && game_cb13_root < 64) {
            d3d12_cl->SetGraphicsRootConstantBufferView((UINT)game_cb13_root, g_identity_cb_va);
            if (game_cb14_root >= 0 && game_cb14_root < 64)
                d3d12_cl->SetGraphicsRootConstantBufferView((UINT)game_cb14_root, g_identity_cb_va);
            g_freeze_binds.fetch_add(1, std::memory_order_relaxed);
            if (DiagOnce("X19-FREEZE", CompKey(dm, mc->comp_idx) * 64 + game_cb13_root))
                logger::write("X19: identity bones bound at root %d (b3 root %d) inside the section's "
                              "draw for mod=%d comp=%d -- F now reaches the draw",
                    game_cb13_root, game_cb14_root, dm, mc->comp_idx);
        }
        const int   gt = g_gpu_timer.Begin(d3d12_cl, GT_SectionDraw);
        void* const hp = HotPath::GpuBracketBegin(d3d12_cl, GT_SectionDraw);
        const wchar_t *capture_dir = dm >= 0 && dm < (int)g_geomods.size() ? g_geomods[dm].mod.dir.c_str() : nullptr;
        uint32_t capture_receipt = 0xffffffffu;
        try { if (wwmi_observer_session && !is_full_shadow_pass) capture_receipt = RtCaptureBegin(capture_dir, wc ? (int)wc->index : -1, c, s2, 0); }
        catch (...) { RtCaptureFailed(); }
        RtTargetFocusScope capture_focus(capture_receipt != 0xffffffffu);
        wwmi::executed_draw::Token wwmi_draw_token{};
        if (wwmi_observer_session) {
            try {
                WWMI_ExecutedDrawRecord_v1 observation{};
                observation.struct_size = sizeof(observation);
                observation.abi_version = WWMI_EXECUTED_DRAW_ABI_V1;
                observation.incomplete_fields = WWMI_EXEC_MISSING_ALL_V1;
                observation.known_fields = WWMI_EXEC_KNOWN_REQUEST | WWMI_EXEC_KNOWN_ACTUAL_ARGUMENTS |
                    WWMI_EXEC_KNOWN_ORIGINAL_ARGUMENTS | WWMI_EXEC_KNOWN_NATIVE_LIST;
                observation.native_command_list = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d3d12_cl));
                observation.public_command_list = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cmd_list));
                observation.host_list_generation_hint = CmdGen(cmd_list);
                if (observation.host_list_generation_hint)
                    observation.known_fields |= WWMI_EXEC_KNOWN_HOST_LIST_GENERATION_HINT;
                if (dm >= 0) {
                    observation.mod_index = static_cast<uint32_t>(dm);
                    observation.known_fields |= WWMI_EXEC_KNOWN_MOD_INDEX;
                }
                if (wc && wc->index >= 0) {
                    observation.component_index = static_cast<uint32_t>(wc->index);
                    observation.configured_rule_ib_hash = wc->ib_hash;
                    observation.configured_rule_first_index = wc->match_first_index;
                    observation.configured_rule_index_count = wc->match_index_count;
                    observation.known_fields |= WWMI_EXEC_KNOWN_COMPONENT_RULE;
                }
                observation.original_vs_hash = vs_hash;
                if (vs_hash) observation.known_fields |= WWMI_EXEC_KNOWN_ORIGINAL_VS_HASH;
                if (draw_rt.valid) {
                    observation.original_depth_view = draw_rt.dsv;
                    observation.host_pass_generation_hint = draw_rt.pass_gen;
                    observation.original_render_target_count = draw_rt.rtv_count;
                    observation.original_has_depth = draw_rt.has_dsv ? 1u : 0u;
                    observation.known_fields |= WWMI_EXEC_KNOWN_RENDER_TARGET_CONTEXT;
                }
                observation.full_shadow_path = is_full_shadow_pass ? 1u : 0u;
                observation.requested_index_count = wwmi_requested_count;
                observation.requested_first_index = wwmi_requested_first;
                observation.requested_base_vertex_bits = wwmi_requested_base_bits;
                observation.original_arguments = {da.index_count, da.instance_count, da.first_index,
                    da.vertex_offset, da.first_instance, 0};
                observation.actual_arguments = {c, 1, s2, 0, first_instance, 0};
                // These are separately labelled evidence sources, NOT merged
                // into a guessed final binding. Native IA writes can bypass the
                // game shadow; shim writes and RELOCBIND have distinct histories.
                for (unsigned slot = 0; slot < 8; ++slot) {
                    if (fr.save_vb_set[slot]) {
                        const auto &view = fr.save_vb[slot];
                        observation.entry_game_vb[slot] = {0, view.BufferLocation,
                            view.SizeInBytes, view.StrideInBytes,
                            WWMI_EXEC_VIEW_RAW_CAPTURED | WWMI_EXEC_VIEW_STRIDE_FORMAT_KNOWN,
                            WWMI_EXEC_VIEW_ENTRY_GAME};
                    }
                    void *resource = nullptr;
                    unsigned long long va = 0;
                    unsigned bytes = 0, stride = 0;
                    if (PortApi::GetShimVertexBuffer(slot, &resource, &va, &bytes, &stride, nullptr)) {
                        observation.shim_last_write_vb[slot] = {
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(resource)), va, bytes, stride,
                            WWMI_EXEC_VIEW_RAW_CAPTURED | WWMI_EXEC_VIEW_RESOURCE_POINTER_KNOWN |
                                WWMI_EXEC_VIEW_STRIDE_FORMAT_KNOWN,
                            WWMI_EXEC_VIEW_SHIM_LAST_WRITE};
                    }
                }
                if (fr.save_ib_set) {
                    observation.entry_game_ib = {0, fr.save_ib.BufferLocation, fr.save_ib.SizeInBytes,
                        static_cast<uint32_t>(fr.save_ib.Format),
                        WWMI_EXEC_VIEW_RAW_CAPTURED | WWMI_EXEC_VIEW_STRIDE_FORMAT_KNOWN,
                        WWMI_EXEC_VIEW_ENTRY_GAME};
                }
                if (x24_vb6_verdict == 1 && x24_vb6_res && x24_vb6_va && x24_vb6_bytes) {
                    observation.relocation_slot6_hint = {
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(x24_vb6_res)),
                        x24_vb6_va, x24_vb6_bytes, 0,
                        WWMI_EXEC_VIEW_RAW_CAPTURED | WWMI_EXEC_VIEW_RESOURCE_POINTER_KNOWN,
                        WWMI_EXEC_VIEW_RELOCATION_HINT};
                }
                // actual_vb/actual_ib, identity generations, material, epoch and
                // deformation receipts remain zero with explicit missing bits.
                wwmi_draw_token = wwmi::executed_draw::Before(wwmi_observer_session, observation);
            } catch (...) {
                wwmi::executed_draw::ObservationFailed();
            }
        }
        wwmi::binding_client::DrawIndexedInstanced(d3d12_cl, c, 1, s2, 0, first_instance, 101,
            wwmi_draw_token.session, wwmi_draw_token.call_id);
        if (wwmi_rt_section_token) WWMI_RtModSectionDraw_v1(wwmi_rt_section_token,c,s2,0);
        // Observe only a real recorded draw from the exact target mod folder; never alter it.
        try {
            if (capture_receipt != 0xffffffffu) {
                RtCaptureCommit();
                if (!wwmi_draw_token.session || !wwmi_draw_token.call_id) RtCaptureFailed();
                rt_section::Input in{};
                in.mod = dm; in.component = wc ? (int)wc->index : -1;
                in.process_id = GetCurrentProcessId();
                in.executed_session = wwmi_draw_token.session; in.executed_call = wwmi_draw_token.call_id;
                in.original_pso = CmdGfx(cmd_list).gfx_pso; in.vs_hash = vs_hash;
                in.pso = rfxp_pso ? (uint64_t)rfxp_pso : layout_pso ? (uint64_t)layout_pso :
                    blend_pso ? (uint64_t)blend_pso : CmdGfx(cmd_list).gfx_restore();
                in.count=c; in.first=s2; in.base=0; in.instances=1; in.first_instance=first_instance;
                MigotoPort::CapturedInputElement captured[32]{}; unsigned count=0;
                const bool applied = layout_pso != nullptr;
                const bool original = !applied && MigotoPort::CopyInputLayout(in.original_pso, captured, 32, &count);
                if (applied) count = layout_ovr.count;
                const auto layout_source = rt_section::SelectLayout(applied && layout_ovr.elements && count && count <= 32, original);
                if (layout_source != rt_section::Source::unknown) {
                    rt_section::Element converted[32]{}; bool names_complete=true;
                    for (unsigned i=0; i<count; ++i) {
                        const auto& e=applied ? layout_ovr.elements[i] : captured[i].desc; auto& out=converted[i];
                        if (!e.SemanticName || strlen(e.SemanticName) >= sizeof(out.semantic)) { names_complete=false; break; }
                        memcpy(out.semantic, e.SemanticName, strlen(e.SemanticName)+1);
                        out.attribute=_stricmp(e.SemanticName, "ATTRIBUTE")==0;
                        out.semantic_index=e.SemanticIndex; out.format=(uint32_t)e.Format; out.slot=e.InputSlot;
                        out.offset=e.AlignedByteOffset; out.slot_class=(uint32_t)e.InputSlotClass; out.step=e.InstanceDataStepRate;
                    }
                    if (names_complete) rt_section::InstallLayout(in, layout_source, converted, count);
                }
                for (unsigned slot : {0u, 4u}) {
                    void* res=nullptr; unsigned long long va=0; unsigned bytes=0, stride=0;
                    if (PortApi::GetShimVertexBuffer(slot, &res, &va, &bytes, &stride, nullptr))
                        in.vb[slot] = {(uint64_t)res, va, bytes, stride, rt_section::Source::shim_last_write};
                }
                for (unsigned slot=2; slot<=4; ++slot) {
                    void* res=nullptr; unsigned long long va=0; unsigned bytes=0;
                    if (PortApi::GetLogicalVertexConstantBuffer(slot, &res, &va, &bytes))
                        in.cb[slot-2] = {(uint64_t)res, va, bytes, 0, rt_section::Source::port_logical};
                }
                RtCaptureSection(in, capture_receipt, is_full_shadow_pass);
            }
        } catch (...) { RtCaptureFailed(); }
        wwmi::executed_draw::After(wwmi_draw_token);
        ++t_x23_injected;
        HotPath::GpuBracketEnd(d3d12_cl, hp);
        g_gpu_timer.End(d3d12_cl, gt);
        // RTFETCH: the game's LocalVF back at b2 -- its next draw on this list may not rebind it.
        if (vf_restore.param != 0xFF)
            d3d12_cl->SetGraphicsRootConstantBufferView(vf_restore.param, vf_restore.va);
        RemovalPin::Ours(d3d12_cl, "x23-draw");
        ++did;
        g_occl.End(d3d12_cl, oq);
    }
    if (tbind.bound) RfxRestore(d3d12_cl, tbind);
    if (rfxp_pso && rfxp_restore) // the patched PS wrote the glow / cutout; back to the game's PSO
        wwmi::semantic::SetPipelineState(d3d12_cl, reinterpret_cast<ID3D12PipelineState*>(CmdGfx(cmd_list).gfx_restore()), wwmi::semantic::SectionRestorePso);
    if (layout_pso)
        wwmi::semantic::SetPipelineState(d3d12_cl, reinterpret_cast<ID3D12PipelineState*>(CmdGfx(cmd_list).gfx_restore()), wwmi::semantic::SectionRestorePso);
    if (blend_pso && restore_pso) {
        const CmdState& gs = CmdGfx(cmd_list);
        wwmi::semantic::SetPipelineState(d3d12_cl, reinterpret_cast<ID3D12PipelineState*>(gs.gfx_restore()), wwmi::semantic::SectionRestorePso);
        const float def[4] = { 1.f, 1.f, 1.f, 1.f };
        d3d12_cl->OMSetBlendFactor(gs.blend_factor_set ? gs.blend_factor : def);
    }
}
