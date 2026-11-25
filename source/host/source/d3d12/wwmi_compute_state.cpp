// SPDX-License-Identifier: GPL-3.0-only
#define WWMI_COMPUTE_API __declspec(dllexport)
#include "wwmi_compute_state.h"
#include "wwmi_compute_state.hpp"
#define WWMI_RAY_AS_API __declspec(dllexport)
#include "wwmi_rt_ray_as_slot.h"
#include "compute_scope_native.hpp"
#if RESHADE_ADDON >= 2
#include "d3d12_impl_device.hpp"
#include "wwmi_as_slots.hpp"
#endif
#include <reshade_api_device.hpp>
#include <atomic>
#include <cstring>
namespace wwmi::compute_state {
static std::atomic<uint64_t> enabled_epoch{0};
uint64_t current_epoch() noexcept {return enabled_epoch.load(std::memory_order_acquire);}
static State *lookup(uint64_t list) {
    return list?reinterpret_cast<reshade::api::command_list*>(list)->get_private_data<State>():nullptr;
}
static int32_t available(State *state) {
    const auto epoch=current_epoch();
    if(!(epoch&1))return 1;
    if(!state)return 2;
    if(state->inside)return 4;
    return state->epoch==epoch&&state->recording_known&&state->snapshot.ready()?0:2;
}
}
extern "C" uint64_t WWMI_ComputeState_Enable_v1(uint32_t enabled) {
    using namespace wwmi::compute_state;
    uint64_t old=current_epoch();
    while((old&1)!=uint64_t(enabled!=0)) {
        if(enabled_epoch.compare_exchange_weak(old,old+1,std::memory_order_acq_rel))return old+1;
    }
    return old;
}
extern "C" int32_t WWMI_ComputeState_Query_v1(uint64_t list, WWMI_ComputeStateInfo_v1 *info) {
    using namespace wwmi::compute_state;
    if(!list||!info||info->size!=sizeof(*info))return 3;
    const auto state=lookup(list);const int32_t status=available(state);
    *info={sizeof(*info),1,0,0,0,0,current_epoch()};
    if(state&&state->epoch==info->epoch) {
        info->root_signature=state->snapshot.root_value();
        info->pipeline=state->snapshot.pipeline_value();
        info->pipeline_kind=uint32_t(state->snapshot.pipeline_kind());
        info->ready=status==0;
    }
    return status;
}
extern "C" int32_t WWMI_ComputeState_ReadRootArgument_v1(
    uint64_t list,const WWMI_ComputeStateInfo_v1 *expected,uint32_t parameter,WWMI_ComputeRootArgument_v1 *out) {
    using namespace wwmi::compute_state;
    if(!out||out->size!=sizeof(*out))return 3;
    *out={sizeof(*out),1,parameter,WWMI_COMPUTE_ROOT_UNKNOWN,0,0,0,0,0,0};
    if(!list||!expected||expected->size!=sizeof(*expected)||expected->version!=1||parameter>=64)return 3;
    const auto state=lookup(list);const auto status=available(state);
    if(status)return status;
    const auto &s=state->snapshot;
    const auto kind=uint32_t(s.pipeline_kind());
    if(!expected->ready||state->epoch!=expected->epoch||s.root_value()!=expected->root_signature||
       s.pipeline_value()!=expected->pipeline||kind!=expected->pipeline_kind)return 2;
    // Visit the unchanged CPU snapshot without restoring or modifying it.
    struct ReadOne {
        WWMI_ComputeRootArgument_v1 &out;
        void signature(uint64_t) {}
        void pipeline(hsin_rt::PipelineKind,uint64_t) {}
        void binding(hsin_rt::RootKind type,uint32_t param,uint64_t value) {
            if(param!=out.parameter)return;
            switch(type){
            case hsin_rt::RootKind::table:out.kind=WWMI_COMPUTE_ROOT_TABLE;break;
            case hsin_rt::RootKind::cbv:out.kind=WWMI_COMPUTE_ROOT_CBV;break;
            case hsin_rt::RootKind::srv:out.kind=WWMI_COMPUTE_ROOT_SRV;break;
            case hsin_rt::RootKind::uav:out.kind=WWMI_COMPUTE_ROOT_UAV;break;
            default:return;
            }
            out.address=value;
        }
        void constants(uint32_t param,uint32_t,uint32_t,const uint32_t*) {
            if(param==out.parameter)out.kind=WWMI_COMPUTE_ROOT_CONSTANTS;
        }
    } visitor{*out};
    s.apply(visitor);
    if(out->kind==WWMI_COMPUTE_ROOT_UNKNOWN)return 2;
    out->epoch=state->epoch;out->root_signature=s.root_value();out->pipeline=s.pipeline_value();out->pipeline_kind=kind;
    return 0;
}
extern "C" int32_t WWMI_ComputeState_ReadAsTableSlot_v1(
    uint64_t list,const WWMI_ComputeStateInfo_v1 *expected,uint32_t parameter,uint32_t table_offset,WWMI_AsTableSlot_v1 *out) {
    if(!out||out->size!=sizeof(*out))return 3;
    *out={sizeof(*out),1,parameter,table_offset,0,0,0,0,0,0,0,0};
    WWMI_ComputeRootArgument_v1 argument{sizeof(argument)};
    const auto status=WWMI_ComputeState_ReadRootArgument_v1(list,expected,parameter,&argument);
    if(status)return status;
#if RESHADE_ADDON >= 2
    if(argument.kind!=WWMI_COMPUTE_ROOT_TABLE||!argument.address)return 2;
    const auto state=wwmi::compute_state::lookup(list);
    if(!state->heaps[0])return 2;
    const auto cmd=reinterpret_cast<reshade::api::command_list *>(list);
    const auto device=static_cast<reshade::d3d12::device_impl *>(cmd->get_device());
    if(device->wwmi_read_as_slot(argument.address,table_offset,state->heaps[0],*out))return 0;
#endif
    return 2;
}
extern "C" int32_t WWMI_ComputeState_ReadRayAsSlot_v1(
    uint64_t list,const WWMI_ComputeStateInfo_v1 *observed,uint32_t parameter,uint32_t table_offset,WWMI_AsTableSlot_v1 *out)
{
    using namespace wwmi::compute_state;
    if(!out||out->size!=sizeof(*out))return 3;
    *out={sizeof(*out),1,parameter,table_offset,0,0,0,0,0,0,0,0};
    if(!list||!observed||observed->size!=sizeof(*observed)||observed->version!=1||parameter>=64)return 3;
    const auto epoch=current_epoch();
    if(!(epoch&1))return 1;
    const State *state=lookup(list);
    if(!state)return 2;
    if(state->inside)return 4;
    const auto &s=state->snapshot;
    const auto &proof=state->restore;
    // Non-table arguments can be unknown (e.g. after native ExecuteIndirect).
    // This read neither restores them nor treats the legacy snapshot as ready.
    if(state->epoch!=epoch||observed->epoch!=epoch||
        !proof.root_known||!proof.tables_known||!proof.heaps_known||
        !observed->root_signature||observed->root_signature!=s.root_value()||
        !observed->pipeline||observed->pipeline!=s.pipeline_value()||
        observed->pipeline_kind!=1||s.pipeline_kind()!=hsin_rt::PipelineKind::ray_tracing)return 2;
#if RESHADE_ADDON >= 2
    if(!state->heaps[0])return 2;
    struct ReadTable {
        uint32_t parameter;
        uint64_t table=0;
        void binding(hsin_rt::RootKind kind,uint32_t index,uint64_t value) {
            if(kind==hsin_rt::RootKind::table&&index==parameter)table=value;
        }
        void constants(uint32_t,uint32_t,uint32_t,const uint32_t*) {}
    } slot{parameter};
    s.visit_arguments(slot,true);
    if(!slot.table)return 2;
    const auto cmd=reinterpret_cast<reshade::api::command_list *>(list);
    const auto device=static_cast<reshade::d3d12::device_impl *>(cmd->get_device());
    if(device->wwmi_read_as_slot(slot.table,table_offset,state->heaps[0],*out))return 0;
#endif
    return 2;
}
extern "C" int32_t WWMI_ComputeState_ReadStructuredTableSlot_v1(
    uint64_t list,const WWMI_ComputeStateInfo_v1 *expected,uint32_t parameter,
    uint32_t table_offset,WWMI_StructuredTableSlot_v1 *out)
{
    if(!out||out->size!=sizeof(*out))return 3;
    *out={sizeof(*out),1,parameter,table_offset};
    WWMI_ComputeRootArgument_v1 argument{sizeof(argument)};
    const auto status=WWMI_ComputeState_ReadRootArgument_v1(list,expected,parameter,&argument);
    if(status)return status;
#if RESHADE_ADDON >= 2
    if(argument.kind!=WWMI_COMPUTE_ROOT_TABLE||!argument.address)return 2;
    const auto state=wwmi::compute_state::lookup(list);
    const auto &proof=state->restore;
    if(!proof.root_known||!proof.tables_known||!proof.heaps_known||!state->heaps[0])return 2;
    const auto cmd=reinterpret_cast<reshade::api::command_list *>(list);
    const auto device=static_cast<reshade::d3d12::device_impl *>(cmd->get_device());
    if(device->wwmi_read_structured_slot(argument.address,table_offset,state->heaps[0],*out))return 0;
#endif
    return 2;
}

extern "C" int32_t WWMI_GraphicsState_CaptureTables_v1(
    uint64_t api_list,WWMI_GraphicsRestoreInfo_v1 *out,
    WWMI_GraphicsRestoreRecord_v1 *records,uint32_t record_capacity)
{
    using namespace wwmi::compute_state;
    if(!out||out->size!=sizeof(*out))return 3;
    *out={sizeof(*out),1,WWMI_GRAPHICS_RESTORE_UNKNOWN};
    if(!api_list||(!records&&record_capacity))return 3;
    const auto epoch=current_epoch();
    if(!(epoch&1))return 1;
    const auto *state=lookup(api_list);
    if(!state)return 2;
    if(state->inside)return 4;

    const auto &gfx=state->graphics;
    if(state->epoch!=epoch||!state->restore.heaps_known||!gfx.root_known||
       (gfx.root&&!gfx.tables_known)||gfx.pass!=GraphicsTables::Pass::outside)return 2;
    const auto cmd=reinterpret_cast<reshade::api::command_list *>(api_list);
    const uint64_t native_list=cmd->get_native();
    if(!native_list)return 2;

    uint32_t count=0;
    uint64_t heaps[2]={0,0};
#if RESHADE_ADDON >= 2
    wwmi::rt_as::HeapBounds bounds[2];
    if(state->heaps[0]||state->heaps[1]){
        const auto device=static_cast<reshade::d3d12::device_impl *>(cmd->get_device());
        if(!device->wwmi_resolve_restore_heaps(state->restore.heap_tags,state->heaps,heaps,bounds))return 2;
    }
    if(gfx.root){
        uint64_t mask=gfx.written;
        for(uint32_t parameter=0;mask;mask>>=1,++parameter){
            if(!(mask&1))continue;
            const uint64_t base=gfx.bases[parameter];
            if(!bounds[0].contains(base)&&!bounds[1].contains(base))return 2;
            if(count==record_capacity)return 5;
            records[count++]={parameter,0,base};
        }
    }
#else
    if(gfx.root&&gfx.written)return 2; // No registered range proof in this build.
    heaps[0]=state->heaps[0];heaps[1]=state->heaps[1];
#endif
    const uint32_t level=gfx.root?WWMI_GRAPHICS_RESTORE_TABLES:WWMI_GRAPHICS_RESTORE_EMPTY;
    *out={sizeof(*out),1,level,count,1,0,native_list,heaps[0],heaps[1],gfx.root};
    return 0;
}

extern "C" int32_t WWMI_ComputeState_CaptureRestore_v1(
    uint64_t api_list, WWMI_ComputeRestoreInfo_v1 *out,
    WWMI_ComputeRestoreRecord_v1 *records, uint32_t record_capacity,
    uint32_t *words, uint32_t word_capacity)
{
    using namespace wwmi::compute_state;
    if(!out||out->size!=sizeof(*out))return 3;
    *out={sizeof(*out),1,WWMI_COMPUTE_RESTORE_UNKNOWN,0,0,0,0,0,0,0};
    if(!api_list||(!records&&record_capacity)||(!words&&word_capacity))return 3;

    // This proof is independent of legacy readiness (which includes a PSO
    // and rejects indirect-altered recordings for Query/Run and AS reads).
    const uint64_t epoch=current_epoch();
    if(!(epoch&1))return 1;
    const State *state=lookup(api_list);
    if(!state)return 2;
    if(state->inside)return 4;
    const auto &proof=state->restore;
    if(state->epoch!=epoch||!proof.heaps_known||!proof.root_known)return 2;
    const uint64_t root=state->snapshot.root_value();
    if(root&&!proof.tables_known)return 2;
    const uint32_t level=!root?WWMI_COMPUTE_RESTORE_EMPTY:
        proof.non_tables_known?WWMI_COMPUTE_RESTORE_ROOT_ARGUMENTS:WWMI_COMPUTE_RESTORE_TABLES;

    const auto cmd=reinterpret_cast<reshade::api::command_list *>(api_list);
    const uint64_t native_list=cmd->get_native();
    if(!native_list)return 2;
    uint64_t heaps[2]={0,0};
#if RESHADE_ADDON >= 2
    if(state->heaps[0]||state->heaps[1]){
        const auto device=static_cast<reshade::d3d12::device_impl *>(cmd->get_device());
        if(!device->wwmi_resolve_restore_heaps(proof.heap_tags,state->heaps,heaps))return 2;
    }
#else
    heaps[0]=state->heaps[0];heaps[1]=state->heaps[1];
#endif

    static_assert(uint32_t(hsin_rt::RootKind::table)==WWMI_COMPUTE_ROOT_TABLE &&
        uint32_t(hsin_rt::RootKind::cbv)==WWMI_COMPUTE_ROOT_CBV &&
        uint32_t(hsin_rt::RootKind::srv)==WWMI_COMPUTE_ROOT_SRV &&
        uint32_t(hsin_rt::RootKind::uav)==WWMI_COMPUTE_ROOT_UAV &&
        uint32_t(hsin_rt::RootKind::constants)==WWMI_COMPUTE_ROOT_CONSTANTS);
    struct CopyArguments {
        WWMI_ComputeRestoreRecord_v1 *records;
        uint32_t record_capacity;
        uint32_t *words;
        uint32_t word_capacity;
        uint32_t record_count=0,word_count=0;
        bool overflow=false;

        void binding(hsin_rt::RootKind kind,uint32_t parameter,uint64_t value) {
            if(overflow||record_count==record_capacity){overflow=true;return;}
            records[record_count++]={uint32_t(kind),parameter,0,0,value};
        }
        void constants(uint32_t parameter,uint32_t first,uint32_t count,const uint32_t *values) {
            if(overflow||record_count==record_capacity||count>word_capacity-word_count){overflow=true;return;}
            records[record_count++]={WWMI_COMPUTE_ROOT_CONSTANTS,parameter,first,count,word_count};
            std::memcpy(words+word_count,values,count*sizeof(uint32_t));
            word_count+=count;
        }
    } args{records,record_capacity,words,word_capacity};
    if(root)state->snapshot.visit_arguments(args,level==WWMI_COMPUTE_RESTORE_TABLES);
    if(args.overflow)return 5;
    // Publish counts and borrowed handles only after a complete copy.
    *out={sizeof(*out),1,level,args.record_count,args.word_count,0,native_list,heaps[0],heaps[1],root};
    return 0;
}
extern "C" int32_t WWMI_ComputeState_Run_v1(uint64_t list, WWMI_ComputeWork_v1 work, void *context) {
    using namespace wwmi::compute_state;
    if(!list||!work)return 3;
    State *state=lookup(list);const int32_t status=available(state);if(status)return status;
    auto *native=reinterpret_cast<ID3D12GraphicsCommandList*>(reinterpret_cast<reshade::api::command_list*>(list)->get_native());
    // Obtain a real v4 interface for RT PSO restoration; never reinterpret a lower interface.
    ID3D12GraphicsCommandList4 *native4=nullptr;
    if(FAILED(native->QueryInterface(__uuidof(ID3D12GraphicsCommandList4),reinterpret_cast<void**>(&native4))))return 2;
    state->inside=true;
    int32_t result=5;
    try {result=work(reinterpret_cast<uint64_t>(native4),context)==0?0:5;} catch(...) {}
    hsin_rt::NativeComputeSink sink{native4};
    state->snapshot.apply(sink);
    state->inside=false;
    native4->Release();
    return result;
}
