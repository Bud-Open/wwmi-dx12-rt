// The dispatch-scope queue: the lists a game dispatch's `run =` queued, tagged with that dispatch.
#include "sk_internal.h"
#include "../instrumentation/uav_observe.h"

std::mutex                 g_sk_pending_mtx;
std::vector<SkPendingList> g_sk_pending;
std::atomic<uint32_t>      g_sk_pending_count{ 0 };

static thread_local unsigned long long t_sk_cur_cs = 0;
static thread_local unsigned           t_sk_cur_tgy = 0;
static thread_local unsigned           t_sk_cur_u0hash = 0;
static thread_local int                t_sk_cur_owner = -1;
static thread_local ID3D12Resource*    t_sk_cur_inst = nullptr;
static thread_local uint64_t t_sk_probe_generation = 0;
static thread_local bool t_sk_cb0_failed = false;

void SkNoteDispatchList(const void* native_cl, uint32_t frame)
{
    SkDispAnchor& a = g_sk_disp[frame & 1];
    a.cl.store(native_cl, std::memory_order_relaxed);
    a.frame.store(frame, std::memory_order_relaxed);
}

static thread_local unsigned t_q_count = 0;
static thread_local uint8_t  t_q_reason = 0;

unsigned SkTakeQueueOutcome(uint8_t* reason)
{
    if (reason) *reason = t_q_reason;
    const unsigned n = t_q_count;
    t_q_count = 0;
    t_q_reason = 0;
    return n;
}

void SkBeginDispatchScope(uint64_t cs_hash, uint32_t group_count_y, const SkU0Ident& ck)
{
    t_q_count = 0;
    t_q_reason = ck.shapekey ? 3u : 1u;
    t_sk_cur_cs = (unsigned long long)cs_hash;
    t_sk_cur_tgy = group_count_y;
    t_sk_cur_u0hash = ck.desc;
    t_sk_cur_owner = ck.declared ? ck.owner : -1;
    t_sk_cur_inst = ck.inst;
    t_sk_probe_generation = ck.probe_instance_generation;
    t_sk_cb0_failed = ck.cb0_snapshot == SkSnapResult::failure;
    if (t_sk_cb0_failed && ck.owner >= 0 && ck.inst) {
        std::lock_guard<std::mutex> l(g_sk_pending_mtx);
        for (auto& e : g_sk_pending)
            if (e.owner_mod == ck.owner && e.inst == ck.inst) e.cb0_failed = true;
    }
    if (ck.shapekey) PortApi::SetTypedObserver([](const PortApi::ObservedCommand& oc, void*) {
        if (oc.post || oc.kind != PortApi::ObservedCommand::Kind::RunList || !oc.run_explicit) return;
        if (!t_sk_cur_inst) {
            t_q_reason = 5; // no instance: do not let a draw claim this list
            static std::atomic<unsigned> rejected{0};
            if (rejected.fetch_add(1, std::memory_order_relaxed) < 8)
                logger::write("SK-INSTANCE: rejected ShapeKey list without dispatch instance");
            return;
        }
        std::wstring                owner = oc.run_target ? oc.run_target : std::wstring();
        std::lock_guard<std::mutex> l(g_sk_pending_mtx);
        const unsigned              cur_tgy = t_sk_cur_tgy;
        ID3D12Resource* const       cur_inst = t_sk_cur_inst;
        for (auto& e : g_sk_pending)
            if (e.list == oc.run_explicit && e.tgy == cur_tgy && e.inst == cur_inst) {
                // Preserve the old cross-owner dedup for valid scopes, but never
                // let a failed entry or scope affect a different owner.
                if (e.owner_mod != t_sk_cur_owner && (t_sk_cb0_failed || e.cb0_failed)) continue;
                e.cb0_failed |= t_sk_cb0_failed;
                if (!t_q_count) t_q_reason = 4;
                return;
            } // already queued
        {
            const int cur_owner = t_sk_cur_owner;
            for (auto& e : g_sk_pending) {
                if (e.owner_mod != cur_owner || e.inst != cur_inst) continue;
                if (!PortApi::ListNests(e.list, oc.run_explicit)) continue;
                e.cb0_failed |= t_sk_cb0_failed;
                if (g_debug && DiagChanged("SK-NESTED", cur_owner, (unsigned long long)(uintptr_t)oc.run_explicit)) {
                    logger::write("SK-NESTED: not queuing list %p -- an already queued list (%p) of the "
                                  "same owner (mod %d) reaches it, so it runs through that one at the drain",
                        oc.run_explicit, e.list, cur_owner);
                }
                return;
            }
        }
        g_sk_pending.push_back(SkPendingList{ std::move(owner), oc.run_explicit,
            t_sk_cur_cs,
            t_sk_cur_u0hash,
            t_sk_cur_tgy,
            t_sk_cur_owner,
            cur_inst, t_sk_cb0_failed, t_sk_probe_generation });
        g_sk_pending_count.store(static_cast<uint32_t>(g_sk_pending.size()), std::memory_order_release);
        ++t_q_count;
        t_q_reason = 0; // 0 = queued, which is the success case
    },
        nullptr);
}

void SkForgetPendingFor(ID3D12Resource* dead)
{
    std::lock_guard<std::mutex> l(g_sk_pending_mtx);
    g_sk_pending.erase(std::remove_if(g_sk_pending.begin(), g_sk_pending.end(),
                           [dead](const SkPendingList& e) { return e.inst == dead; }),
        g_sk_pending.end());
    g_sk_pending_count.store(static_cast<uint32_t>(g_sk_pending.size()), std::memory_order_release);
}
