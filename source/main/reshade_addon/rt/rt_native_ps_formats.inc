// The same exact native observation for every identified author draw, before the VM and geometry admission.
namespace {
using RtFormatListApi=reshade::api::command_list;
static unsigned RtReadNativePsFormats(RtFormatListApi* list,unsigned mask,std::array<RtNativeFormatView,16>& out){
    using namespace reshade::api;
    std::array<RegBind,16> bindings{};
    {
        std::lock_guard lock(g_layout_regmap_mtx);
        const auto it=g_layout_regmap.find(CmdGfxLayout(list));
        if(it==g_layout_regmap.end()||!it->second.built)return 0;
        for(unsigned r=0;r<bindings.size();++r)if(mask&(1u<<r))bindings[r]=it->second.t_ps[r];
    }
    auto* dev=list->get_device();
    const auto& gfx=CmdGfx(list);
    unsigned found=0;
    for(unsigned r=0;r<bindings.size();++r){
        const auto& b=bindings[r];
        if(!(mask&(1u<<r))||b.param>=64||!b.is_table)continue;
        const auto table=TableStartOf(dev,gfx.root[b.param].table);
        if(!table||b.offset>UINT32_MAX-table.base)continue;
        auto* entry=g_heap_to_res.entry_of(table.heap);
        const auto offset=table.base+b.offset;
        const auto* slot=FlatHeapMap::at(entry,offset);if(!slot)continue;
        const auto seq=slot->seq.load(std::memory_order_acquire);
        const auto cpu=slot->src.load(std::memory_order_relaxed);
        if(!seq||!cpu)continue;
        auto* view=g_view_to_res.rec_of(cpu,false);if(!view)continue;
        const auto generation=view->seq.load(std::memory_order_acquire);
        ResInfo info{};
        if(!generation||!g_heap_to_res.find_in(entry,offset,info)||info.view_handle!=cpu||info.is_buffer||info.swapped||!info.res)continue;
        const auto resource=dev->get_resource_from_view(resource_view{cpu}).handle;
        if(!resource||resource!=reinterpret_cast<uint64_t>(info.res))continue;
        const auto desc=dev->get_resource_view_desc(resource_view{cpu});
        if(!unsigned(desc.format)||(desc.type!=resource_view_type::texture_2d&&desc.type!=resource_view_type::texture_2d_array))continue;
        if(entry->id.load(std::memory_order_acquire)!=table.heap||slot->seq.load(std::memory_order_acquire)!=seq||
           slot->src.load(std::memory_order_relaxed)!=cpu||slot->swapped.load(std::memory_order_acquire)||
           view->key.load(std::memory_order_acquire)!=cpu||view->seq.load(std::memory_order_acquire)!=generation)continue;
        out[r]={resource,cpu,generation,unsigned(desc.format),unsigned(desc.type),desc.texture.first_level,desc.texture.levels,
            desc.texture.first_layer,desc.texture.layers};
        ++found;
    }
    return found;
}
struct RtObservedPsFormats {
    std::array<unsigned,16> current{};
    std::array<RtSampledPsFormat,16> carry{};
};
static RtObservedPsFormats RtObserveAuthorPsFormats(RtFormatListApi* list,const RtFormatOwner& owner,uint64_t vs,
    const RtClothRow* trace){
    RtObservedPsFormats result{};
    const auto reads=PipelineSrvRegs(CmdGfx(list).gfx_pso,'p');
    const auto mask=unsigned(reads.lo)&0xffffu;
    if(!reads.parsed||reads.unbounded||!mask)return result;
    std::array<RtNativeFormatView,16> native{};
    RtReadNativePsFormats(list,mask,native);
    uint64_t ps=0;list->get_private_data(s_iid_ps_hash,&ps);
    const auto generation=CmdGen(list);if(!ps||!generation)return result;
    const auto cl=list->get_native();
    const RtSampledPsFormat receipt{0,trace?trace->seq:0,trace?trace->frame:uint64_t(g_frame_id)+1,vs,ps};
    std::lock_guard lock(mutex);
    if(!views.contains(cl)||CmdGen(list)!=generation)return result;
    auto& state=format_lists[cl];
    if(state.generation!=generation){state.formats.clear();state.generation=generation;state.observation=0;}
    auto& entry=state.formats[owner];
    if(!entry.observed)entry.observed=std::make_unique<std::array<RtNativePsFormat,16>>();
    auto& prior=*entry.observed;
    for(unsigned r=0;r<prior.size();++r){
        if((mask&(1u<<r))||!prior[r].view.format)continue;
        // A pass can move the native anchor to another register (t3 -> t0
        // in the actual GBuffer pair). A sampled view with exact lifetime
        // and subview, from the same earlier observation as the format,
        // joins the logical registers; unrelated prior formats stay unknown.
        const bool anchor=std::any_of(prior.begin(),prior.end(),[&](const auto& p){
            return p.observation==prior[r].observation&&p.view.format&&
                std::find(native.begin(),native.end(),p.view)!=native.end();
        });
        if(anchor)result.carry[r]=prior[r].provenance;
    }
    const auto observation=++state.observation;
    for(unsigned r=0;r<prior.size();++r)if(mask&(1u<<r)){
        result.current[r]=native[r].format;
        prior[r]={native[r],receipt,observation};prior[r].provenance.value=native[r].format;
    }
    return result;
}
}
