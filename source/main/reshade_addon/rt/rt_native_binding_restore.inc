namespace {
using CaptureNativeBindingRestore=decltype(&WWMI_ComputeState_CaptureRestore_v1);
struct NativeBindingRestore;
thread_local NativeBindingRestore *native_binding_scope=nullptr;

// One CPU snapshot for this actual original draw, before the VM has issued
// anything through the unwrapped list. The host observes only the game.
struct NativeBindingRestore {
    ID3D12GraphicsCommandList *const list;
    NativeBindingRestore *const outer;
    WWMI_ComputeRestoreInfo_v1 info{sizeof(info)};
    WWMI_ComputeRestoreRecord_v1 records[64];
    uint32_t words[64];
    RtNativeDrawHeaps original{},current{};
    ID3D12RootSignature *last_compute_root=nullptr;
    bool known=false,graphics_pso=false,heaps_changed=false,compute_changed=false,refused=false,masked_trial=false,hybrid_section=false;

    NativeBindingRestore(CaptureNativeBindingRestore read,reshade::api::command_list *api,
                         ID3D12GraphicsCommandList *native,bool can_restore_graphics_pso) noexcept
        :list(native),outer(native_binding_scope),graphics_pso(can_restore_graphics_pso){
        known=read&&read(reinterpret_cast<uint64_t>(api),&info,records,64,words,64)==0&&
            info.version==1&&info.native_list==reinterpret_cast<uint64_t>(list)&&
            info.level>=WWMI_COMPUTE_RESTORE_EMPTY&&info.level<=WWMI_COMPUTE_RESTORE_ROOT_ARGUMENTS&&
            info.record_count<=64&&info.word_count<=64;
        if(known){
            original={reinterpret_cast<ID3D12DescriptorHeap*>(info.cbv_srv_uav_heap),
                      reinterpret_cast<ID3D12DescriptorHeap*>(info.sampler_heap)};
            current=original;
            last_compute_root=reinterpret_cast<ID3D12RootSignature*>(info.root_signature);
        }
        native_binding_scope=this;
    }
    NativeBindingRestore(const NativeBindingRestore&)=delete;
    NativeBindingRestore& operator=(const NativeBindingRestore&)=delete;

    bool can_restore_compute()const noexcept{
        return known&&graphics_pso&&info.level!=WWMI_COMPUTE_RESTORE_TABLES;
    }
    void set_heaps(RtNativeDrawHeaps next) noexcept{
        if(current.resources==next.resources&&current.samplers==next.samplers)return;
        ID3D12DescriptorHeap* heaps[2];unsigned n=0;
        if(next.resources)heaps[n++]=next.resources;
        if(next.samplers)heaps[n++]=next.samplers;
        list->SetDescriptorHeaps(n,n?heaps:nullptr);
        current=next;
        heaps_changed=true;
    }
    ~NativeBindingRestore(){
        // PortRestore (including BridgePop) and RfxRestore have already put
        // back the original heaps and the graphics bindings. No more heap
        // changes may be recorded after this compute replay in the callback.
        if(compute_changed){
            auto *root=reinterpret_cast<ID3D12RootSignature*>(info.root_signature);
            // Re-setting the same root does not clear the VM's arguments.
            if(root&&root==last_compute_root)list->SetComputeRootSignature(nullptr);
            list->SetComputeRootSignature(root);
        }
        if(heaps_changed||compute_changed)for(uint32_t i=0;i<info.record_count;++i){
            const auto &r=records[i];
            if(r.kind==WWMI_COMPUTE_ROOT_TABLE)list->SetComputeRootDescriptorTable(r.parameter,{r.value});
            else if(compute_changed)switch(r.kind){
            case WWMI_COMPUTE_ROOT_CBV:list->SetComputeRootConstantBufferView(r.parameter,r.value);break;
            case WWMI_COMPUTE_ROOT_SRV:list->SetComputeRootShaderResourceView(r.parameter,r.value);break;
            case WWMI_COMPUTE_ROOT_UAV:list->SetComputeRootUnorderedAccessView(r.parameter,r.value);break;
            case WWMI_COMPUTE_ROOT_CONSTANTS:
                list->SetComputeRoot32BitConstants(r.parameter,r.word_count,words+r.value,r.first_word);break;
            }
        }
        native_binding_scope=outer;
    }
};
NativeBindingRestore *BindingScope(ID3D12GraphicsCommandList *list) noexcept{
    auto *s=native_binding_scope;
    return s&&s->list==list?s:nullptr;
}
}

bool RtHasNativeBindingScope(ID3D12GraphicsCommandList *list) noexcept{return BindingScope(list)!=nullptr;}
const RtNativeDrawHeaps* RtOriginalDrawHeaps(ID3D12GraphicsCommandList *list) noexcept{
    auto *s=BindingScope(list);
    return s&&s->known?&s->original:nullptr;
}
void RtBindPreparedDrawHeaps(ID3D12GraphicsCommandList *list,ID3D12DescriptorHeap *resources,ID3D12DescriptorHeap *samplers) noexcept{
    if(auto *s=BindingScope(list)){
        s->set_heaps({resources,samplers});
    }else{
        ID3D12DescriptorHeap *heaps[2];unsigned n=0;
        if(resources)heaps[n++]=resources;
        if(samplers)heaps[n++]=samplers;
        list->SetDescriptorHeaps(n,n?heaps:nullptr);
    }
}
bool RtSetDrawHeaps(ID3D12GraphicsCommandList *list,ID3D12DescriptorHeap *resources,ID3D12DescriptorHeap *samplers) noexcept{
    if(auto *s=BindingScope(list);s&&(!s->known||s->refused)){
        if(s->hybrid_section&&!s->refused)return false;
        s->refused=true;return false;
    }
    RtBindPreparedDrawHeaps(list,resources,samplers);
    return true;
}
bool RtRestoreDrawHeaps(ID3D12GraphicsCommandList *list) noexcept{
    auto *s=BindingScope(list);
    if(!s)return false;
    if(s->known)s->set_heaps(s->original);
    return true;
}
bool RtBeforePortComputeRoots(ID3D12GraphicsCommandList *list,ID3D12RootSignature *next) noexcept{
    if(auto *s=BindingScope(list)){
        if(!s->can_restore_compute()||s->refused){
            // Hybrid raster executes skinning in zz_wwmi_rt_pack from the already-captured
            // native palette and immutable static mod streams; it never consumes VM compute
            // outputs (such as SkeletonMerger) on the GPU. Returning false before any D3D12
            // compute state mutation preserves the command list's unknown indirect compute
            // root arguments without aborting the hybrid draw as vm-not-complete.
            if(s->hybrid_section&&!s->refused)return false;
            s->refused=true;return false;
        }
        s->last_compute_root=next;
        s->compute_changed=true;
    }
    return true;
}
void RtRefusePortWork(ID3D12GraphicsCommandList *list) noexcept{
    if(auto *s=BindingScope(list))s->refused=true;
}
bool RtPortWorkRefused(ID3D12GraphicsCommandList *list) noexcept{
    auto *s=BindingScope(list);
    return s&&s->refused;
}

bool RtPortMaskedTrialActive(ID3D12GraphicsCommandList *list) noexcept{
    auto *s=BindingScope(list);
    return s&&s->masked_trial;
}

bool RtNativeBindingRestoreInfo(ID3D12GraphicsCommandList *list,bool& known,uint32_t& level,bool& gfx_pso,bool& refused) noexcept{
    auto *s=BindingScope(list);
    if(!s)return false;
    known=s->known;level=s->info.level;gfx_pso=s->graphics_pso;refused=s->refused;
    return true;
}
