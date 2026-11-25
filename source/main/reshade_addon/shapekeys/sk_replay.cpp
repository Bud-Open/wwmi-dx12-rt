#include "../observation/producer/producer_hooks.hpp"
// RunShapeKeyPasses: replay an instance's queued lists at its first eligible draw of the frame. See sk_internal.h.
#include "sk_internal.h"
#include "../migoto/u1_isolation.h"
#include "../instrumentation/sk_ring.h"
#include "../instrumentation/uav_observe.h"
#include "../instrumentation/morph_dump.h"
#include "../instrumentation/profiler.h"

static int SkInstDiagKey(int mod_id, ID3D12Resource* inst)
{
    return (int)(((uint64_t)(unsigned)mod_id << 24) ^ (reinterpret_cast<uintptr_t>(inst) >> 4));
}

// This mod instance's queued lists, taken out of the queue (the rest stay for their owners).
static std::vector<SkPendingList> SkTakeOwnLists(const std::wstring& ns, int mod_id, ID3D12Resource* inst,
    MorphDump::DrainRecord& dr,
    size_t& left_for_others, size_t& sk_dropped_unowned,
    size_t& other_inst_lists, ID3D12Resource*& other_inst)
{
    std::vector<SkPendingList> lists;
    auto                       owned_by_ns = [&](const std::wstring& key) {
        if (key.empty() || ns.empty()) return false;
        std::wstring k = key;
        std::wstring n = ns;
        for (wchar_t& c : k) c = (wchar_t)towlower(c);
        for (wchar_t& c : n) c = (wchar_t)towlower(c);
        return k.find(n) != std::wstring::npos;
    };
    {
        std::lock_guard<std::mutex> l(g_sk_pending_mtx);
        dr.pending_in = (unsigned)g_sk_pending.size(); // the queue's depth BEFORE we filter it
        std::vector<SkPendingList> keep;
        for (auto& e : g_sk_pending) {
            bool mine;
            if (e.owner_mod >= 0) {
                mine = (e.owner_mod == mod_id);
            } else if (g_sk_strict_owner) {
                mine = false;
                ++sk_dropped_unowned;
                continue; // dropped, NOT kept: nothing else will ever claim it either
            } else {
                mine = (e.u0hash && PortApi::ModDeclaresHash(ns.c_str(), e.u0hash)) || owned_by_ns(e.ns);
            }
            if (mine && !e.inst) {
                ++sk_dropped_unowned; // discard only this owner's legacy anonymous entry
                continue;
            }
            if (mine && e.inst != inst) {
                mine = false;
                ++other_inst_lists;
                other_inst = e.inst;
            }
            if (mine)
                lists.push_back(e);
            else
                keep.push_back(std::move(e));
        }
        left_for_others = keep.size();
        g_sk_pending.swap(keep); // whatever we did not claim stays queued for its owner
    }
    return lists;
}

// Record every draw, but claim this instance's once-per-frame opportunity only
// when its own lists are actually removed from the queue. Lock order is ord_m ->
// g_sk_pending_mtx (inside SkTakeOwnLists); no replay or diagnostic output is
// performed under either lock. Other queue operations never acquire ord_m.
static bool SkClaimDrawLists(const std::wstring& ns, int mod_id, ID3D12Resource* inst, const void* cl,
    MorphDump::DrainRecord& dr, std::vector<SkPendingList>& lists, size_t& left_for_others,
    size_t& sk_dropped_unowned, size_t& other_inst_lists, ID3D12Resource*& other_inst)
{
    struct OrdState {
        uint32_t    frame = 0;
        uint32_t    draws = 0;
        uint32_t    nlists = 0;
        const void* lists[8] = {};
        const void* pass_cl = nullptr;
        bool        claimed = false;
    };
    static std::mutex                                          ord_m;
    static std::map<std::pair<int, ID3D12Resource*>, OrdState> ord_st;
    bool        report = false;
    uint32_t    rep_frame = 0;
    uint32_t    rep_draws = 0;
    uint32_t    rep_nlists = 0;
    const void* rep_pass_cl = nullptr;
    const void* rep_lists[8] = {};
    bool        rep_claimed = false;
    bool        already_claimed = false;
    const uint32_t frame = dr.frame;
    {
        std::lock_guard<std::mutex> l(ord_m);
        if (ord_st.size() > 64) {
            for (auto it = ord_st.begin(); it != ord_st.end();)
                if ((uint32_t)(frame - it->second.frame) > 300u)
                    it = ord_st.erase(it);
                else
                    ++it;
        }
        OrdState& s = ord_st[{ mod_id, inst }];
        if (s.frame != frame) {
            if (s.frame && logger::enabled(1)) {
                report = true;
                rep_frame = s.frame;
                rep_draws = s.draws;
                rep_nlists = s.nlists;
                rep_pass_cl = s.pass_cl;
                rep_claimed = s.claimed;
                for (uint32_t i = 0; i < s.nlists; ++i) rep_lists[i] = s.lists[i];
            }
            s = OrdState{};
            s.frame = frame;
        }
        ++s.draws;
        bool known = false;
        for (uint32_t i = 0; i < s.nlists; ++i)
            if (s.lists[i] == cl) {
                known = true;
                break;
            }
        if (!known && s.nlists < 8) s.lists[s.nlists++] = cl;
        if (s.claimed) {
            already_claimed = true;
        } else {
            lists = SkTakeOwnLists(ns, mod_id, inst, dr, left_for_others, sk_dropped_unowned,
                other_inst_lists, other_inst);
            if (!lists.empty()) {
                s.claimed = true;
                s.pass_cl = cl;
            }
        }
    }
    if (report) {
        const int   claim_at = rep_claimed ? SubmitIndexOf(rep_frame, rep_pass_cl) : -2;
        const void* disp_cl = SkDispListForFrame(rep_frame);
        const int   disp_at = disp_cl ? SubmitIndexOf(rep_frame, disp_cl) : -2;
        int         earliest = 0x7fffffff;
        int         unsub = 0;
        int         before = 0;
        for (uint32_t i = 0; i < rep_nlists; ++i) {
            const int at = SubmitIndexOf(rep_frame, rep_lists[i]);
            if (at < 0) {
                ++unsub;
                continue;
            }
            if (at < earliest) earliest = at;
            if (claim_at >= 0 && at < claim_at) ++before;
        }
        if (earliest == 0x7fffffff) earliest = -1;
        const uint64_t key = (uint64_t)rep_nlists * 1000000000ull + (uint64_t)(claim_at + 2) * 1000000ull + (uint64_t)(disp_at + 3) * 1000ull + (uint64_t)(before + 1);
        if (DiagChanged("SHAPEKEY-ORDER", SkInstDiagKey(mod_id, inst), key))
            logger::write("SHAPEKEY-ORDER mod=%d inst=%p f=%u: %u draw(s) across %u list(s); claim@%d "
                          "earliest@%d disp@%d before_claim=%d unsubmitted=%d (submissions seen: %llu). "
                          "claim@ is where lists were claimed, not proof that replay completed; "
                          "-2 = no claim, -1 = claim list not submitted. "
                          "before_claim counts submitted lists before the claim. "
                          "disp@ is the GAME's shapekey dispatch list; -2 = no dispatch seen that frame",
                mod_id, (void*)inst, rep_frame, rep_draws, rep_nlists, claim_at, earliest, disp_at, before, unsub,
                (unsigned long long)g_sub_seen.load());
    }
    if (already_claimed) {
        dr.exit = 2;
        MorphDump::RecordDrain(dr);
        return false;
    }
    return true;
}

// A store binding was captured under (namespace, dispatch instance); never guess a replacement
// from another resource with the same size-derived hash.
static bool SkLiveOwnU0(ID3D12Resource* inst, ID3D12Resource* res, unsigned bytes)
{
    if (!inst || !res || !bytes) return false;
    std::lock_guard lock(g_mem_mutex);
    auto a = g_res_to_va.find(reinterpret_cast<uint64_t>(inst));
    auto b = g_res_to_va.find(reinterpret_cast<uint64_t>(res));
    return a != g_res_to_va.end() && a->second && b != g_res_to_va.end() && b->second;
}

// cs-u0, u1 and cb0 republished for this mod instance before its passes run.
static void SkRepublishBindings(const std::wstring& ns, ID3D12Resource* inst, int mod_id,
    ID3D12Resource* u0_res, unsigned u0_bytes)
{
    if (u0_res && u0_bytes) {
        uint64_t va = 0;
        {
            std::lock_guard lock(g_mem_mutex);
            auto            vi = g_res_to_va.find(reinterpret_cast<uint64_t>(u0_res));
            if (vi != g_res_to_va.end()) va = vi->second;
        }
        wwmi::producer::OutputHint(2021, reinterpret_cast<uint64_t>(u0_res), va, u0_bytes, va ? 3 : 4);
        if (va) PortApi::PushGameComputeUav(0, u0_res, u0_bytes, va);
        {
            ID3D12Resource* u1r = nullptr;
            unsigned        u1b = 0;
            uint64_t        u1va = 0;
            SkBufferBinding u1_bind{};
            if (SkGetBinding(ns, inst, TargetClass::UnorderedAccessView, 1, &u1_bind)) {
                u1r = u1_bind.res;
                u1b = u1_bind.bytes;
            } else {
                u1r = nullptr;
                u1b = 0;
            }
            if (u1r && u1b) {
                std::lock_guard lock(g_mem_mutex);
                auto            vi = g_res_to_va.find(reinterpret_cast<uint64_t>(u1r));
                if (vi != g_res_to_va.end()) u1va = vi->second; // liveness + the live VA, one lookup
            }
            if (u1r && u1va) {
                const int rc = PortApi::PushGameComputeUav(1, u1r, u1b, u1va);
                if ((rc != PortApi::kPushUavPublished && rc != PortApi::kPushUavOverMod) ||
                    !MigotoPort::U1Isolation::Published(u1r)) MigotoPort::U1Isolation::FailedPublish();
            }
            SkRing::Note(SkRing::U1Bind, (uint64_t)mod_id, (uint64_t)u1r, u1b, (u1r && u1va) ? 1u : 0u);
            if (logger::enabled(1) && DiagChanged("SHAPEKEY-U1", mod_id, u1b))
                logger::write("SHAPEKEY-U1 mod=%d ns='%ls' -> %s (%u bytes) [from the dispatch's own "
                              "table, by declared ShapeKeyScale hash]",
                    mod_id, ns.c_str(),
                    (u1r && u1va) ? "bound" : "NOT CAPTURED at the dispatch", u1b);
        }
        ID3D12Resource* ns_cb0_res = nullptr;
        uint64_t        ns_cb0_va = 0;
        unsigned        ns_cb0_size = 0;
        {
            SkBufferBinding cb{};
            if (SkGetBinding(ns, inst, TargetClass::ConstantBuffer, 0, &cb)) {
                ns_cb0_res = cb.res;
                ns_cb0_va = cb.va;
                ns_cb0_size = cb.bytes;
            }
        }
        if (ns_cb0_res && ns_cb0_va) {
            uint64_t vac = 0;
            {
                std::lock_guard lock(g_mem_mutex);
                auto            vi = g_res_to_va.find(reinterpret_cast<uint64_t>(ns_cb0_res));
                if (vi != g_res_to_va.end()) vac = vi->second;
            }
            constexpr unsigned kShapeKeyCB0Bytes = ((66u * 16u) + 255u) & ~255u; // cb0[66] -> 1280
            if (vac) PortApi::PushGameConstantBuffer('c', 0, ns_cb0_res, ns_cb0_va, kShapeKeyCB0Bytes);
        } else if (logger::enabled(1)) {
            static std::atomic<bool> said{ false };
            if (!said.exchange(true))
                logger::write("SHAPEKEY-PASSES: no cb0 recorded for this instance at draw time -- cb0 NOT "
                              "republished.");
        }
    }
}

// The game's whole compute state for this mod instance replayed under the passes (UAVs past u1, CBVs).
static void SkReplayComputeState(const std::wstring& ns, ID3D12Resource* inst, int mod_id)
{
    {
        SkComputeState s;
        if (!SkGetStateForNs(ns, inst, &s)) s = SkComputeState{};
        if (s.valid) {
            unsigned n_uav = 0;
            unsigned n_cbv = 0;
            for (unsigned u = 0; u < RegisterFileWidth(TargetClass::UnorderedAccessView); ++u) {
                if (u <= 1) continue;
                if (!SkReg(s, TargetClass::UnorderedAccessView, u).res || !SkReg(s, TargetClass::UnorderedAccessView, u).bytes) continue;
                uint64_t va = 0;
                {
                    std::lock_guard lock(g_mem_mutex);
                    auto            vi = g_res_to_va.find(reinterpret_cast<uint64_t>(SkReg(s, TargetClass::UnorderedAccessView, u).res));
                    if (vi != g_res_to_va.end()) va = vi->second;
                }
                if (!va) continue;
                if ((SkReg(s, TargetClass::UnorderedAccessView, u).bytes & 3u) || (va & 3ull)) continue;
                PortApi::PushGameComputeUav(u, SkReg(s, TargetClass::UnorderedAccessView, u).res, SkReg(s, TargetClass::UnorderedAccessView, u).bytes, va);
                ++n_uav;
            }
            for (unsigned b = 0; b < RegisterFileWidth(TargetClass::ConstantBuffer); ++b) {
                if (b == 0 && g_sk_cb0_snapshot) {
                    if (g_debug && LogBudget("CB0SNAP-SKIP", ns, 4))
                        logger::write("CB0SNAP: state replay SKIPPED b0 for ns='%ls' -- each pass "
                                      "publishes its own snapshot just before it runs (CB0PASS).",
                            ns.c_str());
                    continue;
                }
                if (!SkReg(s, TargetClass::ConstantBuffer, b).res || !SkReg(s, TargetClass::ConstantBuffer, b).va) continue;
                uint64_t live = 0;
                {
                    std::lock_guard lock(g_mem_mutex);
                    auto            vi = g_res_to_va.find(reinterpret_cast<uint64_t>(SkReg(s, TargetClass::ConstantBuffer, b).res));
                    if (vi != g_res_to_va.end()) live = vi->second;
                }
                if (!live) continue; // destroyed since capture: bind nothing, never deref
                unsigned sz = SkReg(s, TargetClass::ConstantBuffer, b).bytes;
                if (b == 0) sz = ((66u * 16u) + 255u) & ~255u;
                if (sz == 0u || sz == 0xFFFFFFFFu) {
                    // Safe to dereference ONLY because `live` above proved it is still alive.
                    const D3D12_RESOURCE_DESC rd = SkReg(s, TargetClass::ConstantBuffer, b).res->GetDesc();
                    const uint64_t            base = live; // the VA the map just gave us
                    const uint64_t            off = (SkReg(s, TargetClass::ConstantBuffer, b).va >= base) ? (SkReg(s, TargetClass::ConstantBuffer, b).va - base) : 0;
                    sz = (rd.Width > off) ? (unsigned)(rd.Width - off) : 0;
                    if (sz > 65536u) sz = 65536u; // D3D12 CBV maximum
                    sz &= ~255u;                  // CBVs must be 256-byte aligned
                }
                if (!sz) continue;
                if (SkReg(s, TargetClass::ConstantBuffer, b).va & 255ull) {
                    if (logger::enabled(1)) {
                        static std::atomic<bool> said{ false };
                        if (!said.exchange(true))
                            logger::write("SKREPLAY: b%u SKIPPED -- va=%llx is not 256-byte aligned, and "
                                          "CreateConstantBufferView rejects that (INVALID_CALL -> device "
                                          "removed). Binding nothing is the safe answer.",
                                b, (unsigned long long)SkReg(s, TargetClass::ConstantBuffer, b).va);
                    }
                    continue;
                }
                PortApi::PushGameConstantBuffer('c', b, SkReg(s, TargetClass::ConstantBuffer, b).res, SkReg(s, TargetClass::ConstantBuffer, b).va, sz);
                ++n_cbv;
            }
            if (logger::enabled(1) && DiagChanged("SKREPLAY", mod_id, (unsigned long long)n_uav * 100u + n_cbv))
                logger::write("SKREPLAY mod=%d ns='%ls' republished the game's compute state: "
                              "%u UAV, %u CBV (SRVs are never replayed) -- the mod's own binds "
                              "are applied after this and win, exactly as on DX11's immediate context",
                    mod_id, ns.c_str(), n_uav, n_cbv);
        }
    }
}

// The mod's lists run, each on its own cb0 snapshot and batch selector; one SKPASS row per pass.
static void SkRunLists(const std::wstring& ns, ID3D12Resource* inst, int mod_id, reshade::api::command_list* cmd_list,
    const std::vector<SkPendingList>& lists, MorphDump::DrainRecord& dr,
    ID3D12Resource* u0_res, unsigned u0_bytes)
{
    if (u0_res && u0_bytes) SkNoteChainWroteU0(u0_res, g_frame_id);
    PortApi::DrawInfo di;
    di.dispatch = true;
    {
        PROF(PROF_SK_Lists); // the four passes themselves; IssueDispatch nests inside this
        for (size_t i = 0; i < lists.size(); ++i) {
            UavObserve::TargetScope observe_scope(lists[i].u0hash==0x3c20751d && lists[i].owner_mod==mod_id && lists[i].inst==inst, lists[i].owner_mod, (uint64_t)lists[i].inst, lists[i].probe_instance_generation);
            auto producer_pass_input = wwmi::producer::ContextualInput(WWMI_PRODUCER_VM_PASS, 2010);
            if (wwmi::producer::CurrentParent().session) {
                const auto& parent = wwmi::producer::current_context.input;
                producer_pass_input.native_command_list = parent.native_command_list;
                producer_pass_input.known_fields |= parent.known_fields & WWMI_PRODUCER_KNOWN_LIST;
            }
            producer_pass_input.owner_mod_index = lists[i].owner_mod;
            if (i < UINT32_MAX && lists.size() <= UINT32_MAX) {
                producer_pass_input.pass_index = static_cast<uint32_t>(i + 1);
                producer_pass_input.pass_count = static_cast<uint32_t>(lists.size());
                producer_pass_input.known_fields |= WWMI_PRODUCER_KNOWN_PASS;
            } else producer_pass_input.flags |= WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
            producer_pass_input.shader_hash = lists[i].cs; // queued hash is only a candidate hint
            if (lists[i].cs) producer_pass_input.known_fields |= WWMI_PRODUCER_KNOWN_SHADER_HASH;
            wwmi::producer::Scope producer_pass(producer_pass_input);
            ++dr.ran;
            /* Stamp the capture with WHICH game dispatch queued this pass, before it issues. */
            MigotoPort::SkCapSetCaller(ns.c_str(), lists[i].cs, lists[i].tgy, mod_id);
            uint64_t sk_snap_res = 0;
            uint64_t sk_snap_va = 0;
            unsigned sk_snap_bytes = 0;
            int      sk_snap_hit = -1;
            if (g_sk_cb0_snapshot) {
                PROF(PROF_SK_Cb0);
                sk_snap_hit = 0; // attempted; 1 only once SkGetSnap actually answers
                /* SkGetSnap refuses an entry with no res or no va, so what it returns is usable as-is. */
                SkBufferBinding sb{};
                if (SkGetSnap(ns, inst, 0, lists[i].cs, lists[i].tgy, &sb)) {
                    PortApi::PushGameConstantBuffer('c', 0, sb.res, sb.va, sb.bytes);
                    sk_snap_hit = 1;
                    sk_snap_res = reinterpret_cast<uint64_t>(sb.res);
                    sk_snap_va = sb.va;
                    sk_snap_bytes = sb.bytes;
                    if (g_debug) {
                        if (LogBudget("CB0PASS", ns, 12))
                            logger::write("CB0PASS: pass %zu <- cb0 snapshot of cs=%016llx (%u B, va=%llx)",
                                i, lists[i].cs, sb.bytes, (unsigned long long)sb.va);
                    }
                } else if (g_debug) {
                    if (LogBudget("CB0PASS-MISS", ns, 12))
                        logger::write("CB0PASS: pass %zu has NO snapshot for cs=%016llx -- it will run on "
                                      "whatever b0 the previous pass left bound.",
                            i, lists[i].cs);
                }
            }
            if (g_sk_freeze && i + 1 == lists.size()) {
                if (SkZeroBinding(ns, inst, TargetClass::UnorderedAccessView, 0, cmd_list, "SKFREEZE") == 1 && g_debug && LogBudget("SKFREEZE-ON", ns, 4))
                    logger::write("SKFREEZE: ns='%ls' -- u0 zeroed before the final pass (%zu of %zu); the "
                                  "Applier will write base positions with no morph.",
                        ns.c_str(), i + 1, lists.size());
            }
            di.thread_group_y = lists[i].tgy;
            MigotoPort::ResetEmittedDispatch();
            {
                PROF(PROF_SK_Run);
                PortApi::RunResolvedCommandList(lists[i].list, &di);
            }
            uint32_t       em_x = 0;
            uint32_t       em_y = 0;
            uint32_t       em_z = 0;
            const unsigned em_n = MigotoPort::TakeEmittedDispatch(&em_x, &em_y, &em_z);
            if (wwmi::producer::SnapshotSession()) {
                auto summary = wwmi::producer::ContextualInput(WWMI_PRODUCER_VM_DISPATCH_SUMMARY, 2011);
                summary.emitted_count = em_n;
                summary.dispatch_x = em_x; summary.dispatch_y = em_y; summary.dispatch_z = em_z;
                summary.known_fields |= WWMI_PRODUCER_KNOWN_DISPATCH;
                summary.reason = em_n ? 1 : 2;
                wwmi::producer::Event(summary, wwmi::producer::CurrentParent());
            }
            producer_pass.data.emitted_count = em_n;
            producer_pass.data.reason = em_n ? 1 : 2;
            PROF(PROF_SK_Row);              // everything below, to the end of this iteration: the row and its log
            const int              csi = 5; // 'c' -- v h d g p c (MigotoPort::StageIndex)
            MigotoPort::PipeState& ps = MigotoPort::State();
            auto                   srv = [&](int r) { return ps.srv[csi][r] ? 1 : 0; };
            auto                   uav = [&](int r) { return ps.uav[r] ? 1 : 0; };
            const float            ck = PortApi::GetVarNs(L"WWMIv1", L"shapekey_checksum", -1.f);
            const float            skv = PortApi::GetVarNs(L"WWMIv1", L"shapekey_vertex_count", -1.f);
            const float            cvc = PortApi::GetVarNs(L"WWMIv1", L"custom_vertex_count", -1.f);
            if (MorphDump::Armed()) {
                MorphDump::PassRecord pr{};
                pr.frame = g_frame_id;
                pr.mod_id = mod_id;
                pr.owner_mod = lists[i].owner_mod;
                pr.pass_index = (unsigned)(i + 1);
                pr.pass_count = (unsigned)lists.size();
                pr.queued_cs = lists[i].cs;
                pr.queued_tgy = lists[i].tgy;
                pr.emitted_n = em_n;
                pr.emitted_x = em_x;
                pr.emitted_y = em_y;
                pr.emitted_z = em_z;
                pr.t0 = srv(0);
                pr.t1 = srv(1);
                pr.t6 = srv(6);
                pr.t33 = srv(33);
                pr.u5 = uav(5);
                pr.u6 = uav(6);
                pr.checksum = ck;
                pr.shapekey_vtx = skv;
                pr.custom_vtx = cvc;
                // S6: their live IniParams, two texels = x0y0z0w0 / x1y1z1w1.
                {
                    float ip[8] = {};
                    PortApi::GetIniParams(ip, 2);
                    pr.x0 = ip[0];
                    pr.y0 = ip[1];
                    pr.z0 = ip[2];
                    pr.x1 = ip[4];
                    pr.y1 = ip[5];
                    pr.z1 = ip[6];
                }
                pr.cb0_res = reinterpret_cast<uint64_t>(ps.cb[csi][0]);
                pr.snap_hit = sk_snap_hit;
                pr.snap_res = sk_snap_res;
                pr.snap_va = sk_snap_va;
                pr.snap_bytes = sk_snap_bytes;
                // S17 identity: WHICH PipeState we just read, against the one the shim binds through.
                pr.ps_addr = reinterpret_cast<uint64_t>(&ps);
                MigotoPort::VbBindStats(&pr.vb_binds, &pr.vb_nonnull, &pr.vb_last_ps);
                // S17. Both views, because they disagree exactly when it matters.
                pr.vb0_state = reinterpret_cast<uint64_t>(ps.vb[0]);
                pr.vb0_shim = reinterpret_cast<uint64_t>(ps.shim_vb[0].res);
                pr.vb0_shim_size = ps.shim_vb[0].size;
                pr.vb0_shim_stride = ps.shim_vb[0].stride;
                static_assert(MigotoPort::kMaxVB > 6);
                pr.vb6_state = reinterpret_cast<uint64_t>(ps.vb[6]);
                pr.vb6_shim = reinterpret_cast<uint64_t>(ps.shim_vb[6].res);
                pr.ns = ns.c_str();
                MorphDump::RecordPass(pr);
            }
            if (logger::enabled(1)) {
                char line[256];
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "t0=%d t1=%d t6=%d t33=%d u5=%d u6=%d | checksum=%g shapekey_vtx=%g "
                    "custom_vtx=%g | emitted=%u x %u,%u,%u tgy=%u",
                    srv(0), srv(1), srv(6), srv(33), uav(5), uav(6), ck, skv, cvc,
                    em_n, em_x, em_y, em_z, lists[i].tgy);
                // on change of the whole line, per (mod, pass)
                if (DiagChanged("SKPASS", mod_id * 16 + (int)i, DiagFnv(line)))
                    logger::write("SKPASS mod=%d ns='%ls' pass=%zu/%zu | %s",
                        mod_id, ns.c_str(), i + 1, lists.size(), line);
            }
        }
    }
}

// The drain row and the SHAPEKEY-PASSES line for what ran.
static void SkReportDrain(const std::wstring& ns, int mod_id, const std::vector<SkPendingList>& lists,
    MorphDump::DrainRecord& dr, size_t left_for_others, size_t sk_dropped_unowned,
    ID3D12Resource* u0_res, unsigned u0_bytes)
{
    { // THE DRAIN ROW. Sampled here, at the draw, where the mod's own vb0/vb6 binds have landed.
        dr.matched = (unsigned)lists.size();
        dr.left = (unsigned)left_for_others;
        dr.dropped = (unsigned)sk_dropped_unowned;
        dr.disp_seen = SkDispListForFrame(g_frame_id) ? 1 : 0;
        dr.u0_res = reinterpret_cast<uint64_t>(u0_res);
        dr.u0_bytes = u0_bytes;
        dr.u0_identity = u0_res ? g_buffer_reloc.IdentityBytes(u0_res, u0_bytes) : 0;
        if (u0_res && dr.u0_identity != u0_bytes) {
            dr.reloc_from = dr.u0_identity;
            dr.reloc_to = u0_bytes;
        }
        MigotoPort::PipeState& dps = MigotoPort::State();
        dr.ps_addr = reinterpret_cast<uint64_t>(&dps);
        MigotoPort::VbBindStats(&dr.vb_binds, &dr.vb_nonnull, &dr.vb_last_ps);
        dr.vb0_state = reinterpret_cast<uint64_t>(dps.vb[0]);
        dr.vb0_shim = reinterpret_cast<uint64_t>(dps.shim_vb[0].res);
        static_assert(MigotoPort::kMaxVB > 6);
        dr.vb6_state = reinterpret_cast<uint64_t>(dps.vb[6]);
        dr.vb6_shim = reinterpret_cast<uint64_t>(dps.shim_vb[6].res);
        MorphDump::RecordDrain(dr);
    }
    if (logger::enabled(1) && DiagChanged("SHAPEKEY-PASSES", mod_id, (unsigned long long)lists.size())) {
        const size_t named = (size_t)std::count_if(lists.begin(), lists.end(),
            [](const SkPendingList& e) { return e.owner_mod >= 0; });
        logger::write("SHAPEKEY-PASSES mod=%d ns='%ls' ran %zu of ITS OWN list(s) (%zu named this mod at their dispatch, %zu fell back to hash/name), %zu left queued for "
                      "other mods or instances, %zu dropped-unowned, cs-u0 %s (%u bytes), cmdlist=%p",
            mod_id, ns.c_str(), lists.size(), named, lists.size() - named,
            left_for_others, sk_dropped_unowned,
            u0_res ? "republished" : "NOT AVAILABLE", u0_bytes,
            (void*)MigotoPort::GetDx12Bridge().cl);
    }
}

void RunShapeKeyPasses(const std::wstring& ns, int mod_id, ID3D12Resource* inst, reshade::api::command_list* cmd_list)
{
    const void* cl = cmd_list ? reinterpret_cast<const void*>(cmd_list->get_native()) : nullptr; // every record/order key below is the native list
    wwmi::producer::Scope producer_drain(wwmi::producer::HintInput(WWMI_PRODUCER_DRAIN, 2001,
        reinterpret_cast<uint64_t>(cl), g_frame_id, mod_id, reinterpret_cast<uint64_t>(inst), ns));
    // Every exit records: "never called" and "called and did nothing" are indistinguishable by absence.
    MorphDump::DrainRecord dr{};
    dr.frame = g_frame_id;
    dr.mod_id = mod_id;
    dr.ns = ns.c_str();
    if (ns.empty()) {
        producer_drain.data.reason = 1;
        dr.exit = 1;
        MorphDump::RecordDrain(dr);
        return;
    }
    PROF(PROF_DI_ShapeKeys);
    size_t                     left_for_others = 0;
    size_t                     sk_dropped_unowned = 0; // lists whose queuing dispatch named no mod
    ID3D12Resource*            u0_res = nullptr;
    unsigned                   u0_bytes = 0;
    size_t                     other_inst_lists = 0;
    ID3D12Resource*            other_inst = nullptr;
    std::vector<SkPendingList> lists;
    if (!SkClaimDrawLists(ns, mod_id, inst, cl, dr, lists, left_for_others, sk_dropped_unowned,
            other_inst_lists, other_inst)) {
        producer_drain.data.reason = 2;
        return;
    }
    if (lists.size() <= UINT32_MAX) {
        producer_drain.data.pass_count = static_cast<uint32_t>(lists.size());
        producer_drain.data.known_fields |= WWMI_PRODUCER_KNOWN_PASS;
        producer_drain.UpdateHints();
    } else producer_drain.data.flags |= WWMI_PRODUCER_FLAG_METADATA_INCOMPLETE;
    // THIS INSTANCE'S u0, or nothing. Inheriting the previous dispatcher's position buffer is the same leak.
    {
        SkBufferBinding b0b{};
        if (SkGetBinding(ns, inst, TargetClass::UnorderedAccessView, 0, &b0b)) {
            u0_res = b0b.res;
            u0_bytes = b0b.bytes;
            wwmi::producer::OutputHint(2020, reinterpret_cast<uint64_t>(b0b.res), b0b.va, b0b.bytes, 1);
        } else {
            u0_res = nullptr;
            u0_bytes = 0;
            wwmi::producer::OutputHint(2020, 0, 0, 0, 2);
        }
    }
    if (!lists.empty() && DiagOnce("SKINST", SkInstDiagKey(mod_id, inst)))
        logger::write("SKINST mod=%d ns='%ls' inst=%p: first drain of this instance -- %zu list(s), u0=%p (%u B); "
                      "%zu of this mod's lists wait for another instance",
            mod_id, ns.c_str(), (void*)inst, lists.size(), (void*)u0_res, u0_bytes, other_inst_lists);
    else if (lists.empty() && other_inst_lists && DiagOnce("SKINST-WAIT", SkInstDiagKey(mod_id, inst)))
        logger::write("SKINST-WAIT mod=%d ns='%ls' inst=%p: nothing queued for this draw's vb6; %zu of this mod's "
                      "lists wait for instance %p",
            mod_id, ns.c_str(), (void*)inst, other_inst_lists, (void*)other_inst);
    if (lists.empty()) {
        producer_drain.data.reason = 3;
        dr.matched = 0;
        dr.left = (unsigned)left_for_others;
        dr.dropped = (unsigned)sk_dropped_unowned;
        dr.disp_seen = SkDispListForFrame(g_frame_id) ? 1 : 0;
        MorphDump::RecordDrain(dr);
        return;
    }

    if (std::any_of(lists.begin(), lists.end(), [](const SkPendingList& p) { return p.cb0_failed; })) {
        producer_drain.data.reason = 5; // cb0 snapshot failed: do not clear, bind or run partial chain
        MorphDump::RecordDrain(dr);
        return;
    }

    if (!SkLiveOwnU0(inst, u0_res, u0_bytes)) {
        producer_drain.data.reason = 6; // missing or dead captured binding
        static std::atomic<unsigned> rejected{0};
        if (rejected.fetch_add(1, std::memory_order_relaxed) < 8)
            logger::write("SK-INSTANCE: rejected drain without a live captured u0 binding");
        MorphDump::RecordDrain(dr);
        return;
    }
    if (g_sk_u0_clear) dr.u0_clear = SkZeroBinding(ns, inst, TargetClass::UnorderedAccessView, 0, cmd_list, "U0CLEAR");
    if (g_sk_u1_clear) dr.u1_clear = SkZeroBinding(ns, inst, TargetClass::UnorderedAccessView, 1, cmd_list, "U1CLEAR");

    MigotoPort::U1Isolation::Scope u1_scope(mod_id, inst);
    SkRepublishBindings(ns, inst, mod_id, u0_res, u0_bytes);

    SkReplayComputeState(ns, inst, mod_id);

    SkRunLists(ns, inst, mod_id, cmd_list, lists, dr, u0_res, u0_bytes);

    SkReportDrain(ns, mod_id, lists, dr, left_for_others, sk_dropped_unowned, u0_res, u0_bytes);
    producer_drain.data.reason = 4;
}
