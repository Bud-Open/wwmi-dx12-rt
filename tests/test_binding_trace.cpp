#include "wwmi_binding_trace.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Log {
    std::vector<uint32_t> kinds;
    std::vector<uint64_t> seqs;
    uint32_t last_vertex_mask = 0;
    uint32_t last_index_known = 0;
};

void WWMI_BIND_CALL OnEvent(const WWMI_BindingRecord_v1* r, void* user)
{
    auto* log = static_cast<Log*>(user);
    log->kinds.push_back(r->input.kind);
    log->seqs.push_back(r->sequence);
    log->last_vertex_mask = r->vertex_known_mask;
    log->last_index_known = r->index_known;
}

WWMI_BindingInput_v1 MakeInput(uint32_t kind, uint64_t list = 0x1000)
{
    WWMI_BindingInput_v1 x{};
    x.struct_size = sizeof(x);
    x.abi_version = WWMI_BIND_ABI_V1;
    x.kind = kind;
    x.source = WWMI_BIND_SOURCE_WRAPPER;
    x.native_list = list;
    return x;
}

void TestLegacyRegisterReceivesDraws()
{
    assert(WWMI_BindingTrace_DrawTraceActive_v1() == 0);
    Log log;
    assert(WWMI_BindingTrace_Register_v1(OnEvent, &log) == 0);
    assert(WWMI_BindingTrace_DrawTraceActive_v1() == 1);

    auto c = MakeInput(WWMI_BIND_LIST_CREATE);
    c.initial_state = WWMI_BIND_INITIAL_STATE_RECORDING;
    const uint64_t s1 = WWMI_BindingTrace_Submit_v1(&c);
    assert(s1 == 1);

    auto db = MakeInput(WWMI_BIND_DRAW_BEFORE);
    db.draw = {36, 1, 0, 0, 0, 0};
    const uint64_t s2 = WWMI_BindingTrace_Submit_v1(&db);
    assert(s2 == 2);

    auto da = MakeInput(WWMI_BIND_DRAW_AFTER);
    da.paired_before_sequence = s2;
    da.draw = db.draw;
    const uint64_t s3 = WWMI_BindingTrace_Submit_v1(&da);
    assert(s3 == 3);

    WWMI_BindingStatus_v1 st{};
    st.struct_size = sizeof(st);
    st.abi_version = WWMI_BIND_ABI_V1;
    assert(WWMI_BindingTrace_GetStatus_v1(&st) == 0);
    assert(st.active == 1);
    assert(st.last_sequence == 3);
    assert(st.pending_draws == 0);
    assert(st.observation_failures_total == 0);
    assert(log.kinds.size() == 3);
    assert(log.kinds[0] == WWMI_BIND_LIST_CREATE);
    assert(log.kinds[1] == WWMI_BIND_DRAW_BEFORE);
    assert(log.kinds[2] == WWMI_BIND_DRAW_AFTER);

    assert(WWMI_BindingTrace_Unregister_v1(OnEvent, &log) == 0);
    assert(WWMI_BindingTrace_DrawTraceActive_v1() == 0);
}

void TestSkipDrawsRegistration()
{
    Log log;
    // Unknown flag bits must be rejected.
    assert(WWMI_BindingTrace_RegisterEx_v1(OnEvent, &log, 1u << 7) == -1);
    assert(WWMI_BindingTrace_DrawTraceActive_v1() == 0);

    assert(WWMI_BindingTrace_RegisterEx_v1(OnEvent, &log, WWMI_BIND_REG_SKIP_DRAWS) == 0);
    assert(WWMI_BindingTrace_DrawTraceActive_v1() == 0);

    // Re-register with identical callback/user/flags is idempotent; mismatched flags fail with -2.
    assert(WWMI_BindingTrace_RegisterEx_v1(OnEvent, &log, WWMI_BIND_REG_SKIP_DRAWS) == 0);
    assert(WWMI_BindingTrace_RegisterEx_v1(OnEvent, &log, 0) == -2);

    auto c = MakeInput(WWMI_BIND_LIST_CREATE);
    c.initial_state = WWMI_BIND_INITIAL_STATE_RECORDING;
    assert(WWMI_BindingTrace_Submit_v1(&c) == 1);

    auto vb = MakeInput(WWMI_BIND_VERTEX_BUFFERS);
    vb.start_slot = 0;
    vb.count = 1;
    vb.views[0] = {0x20000, 4096, 12};
    assert(WWMI_BindingTrace_Submit_v1(&vb) == 2);

    auto ib = MakeInput(WWMI_BIND_INDEX_BUFFER);
    ib.count = 1;
    ib.views[0] = {0x30000, 1024, 42};
    assert(WWMI_BindingTrace_Submit_v1(&ib) == 3);

    // Draw events must be skipped with zero sequence advance and zero errors.
    auto db = MakeInput(WWMI_BIND_DRAW_BEFORE);
    db.draw = {36, 1, 0, 0, 0, 0};
    assert(WWMI_BindingTrace_Submit_v1(&db) == 0);

    auto da = MakeInput(WWMI_BIND_DRAW_AFTER);
    da.paired_before_sequence = 3;
    assert(WWMI_BindingTrace_Submit_v1(&da) == 0);

    auto close = MakeInput(WWMI_BIND_LIST_CLOSE);
    assert(WWMI_BindingTrace_Submit_v1(&close) == 4);

    WWMI_BindingStatus_v1 st{};
    st.struct_size = sizeof(st);
    st.abi_version = WWMI_BIND_ABI_V1;
    assert(WWMI_BindingTrace_GetStatus_v1(&st) == 0);
    assert(st.active == 1);
    assert(st.last_sequence == 4);
    assert(st.pending_draws == 0);
    assert(st.observation_failures_total == 0);
    assert(log.kinds.size() == 4);
    assert(log.last_vertex_mask == 1u);
    assert(log.last_index_known == 1u);

    assert(WWMI_BindingTrace_Unregister_v1(OnEvent, &log) == 0);

    // Next legacy session must re-enable draw tracing cleanly.
    Log log2;
    assert(WWMI_BindingTrace_Register_v1(OnEvent, &log2) == 0);
    assert(WWMI_BindingTrace_DrawTraceActive_v1() == 1);
    assert(WWMI_BindingTrace_Unregister_v1(OnEvent, &log2) == 0);
    assert(WWMI_BindingTrace_DrawTraceActive_v1() == 0);
}

} // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "legacy" || mode == "all") TestLegacyRegisterReceivesDraws();
    if (mode == "skip_draws" || mode == "all") TestSkipDrawsRegistration();
    return 0;
}
