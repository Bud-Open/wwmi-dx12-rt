// Included in hsin_rt, using only existing callback-owned CPU metadata.
void InputCollector::shape_evidence_dispatch(api::command_list *c,const WWMI_RtNativeDispatch_v2 &event,const EvidenceRootReader &root){
    std::lock_guard lock(mutex_);if(!shape_evidence_.wants_commands())return;
    const auto list=lists_.find(id(c));if(list==lists_.end())return;const auto recording=list->second.epoch;
    if(event.phase!=WWMI_RT_DISPATCH_BEFORE_NATIVE){shape_evidence_.after(recording,event.call_id,event.phase);return;}
    NativeShapeEvidence::Fact f;f.recording=recording;f.call=event.call_id;f.flags=event.flags;
    f.x=event.group_x;f.y=event.group_y;f.z=event.group_z;f.pso_from_last_bind=!(event.flags&WWMI_RT_DISPATCH_STATE_KNOWN);
    if(auto s=evidence_shaders_.find(event.pipeline);s!=evidence_shaders_.end())f.shader=s->second;
    if(auto l=evidence_layouts_.find(event.layout);l!=evidence_layouts_.end())f.layout=l->second;
    if(!f.layout&&f.shader){f.layout=f.shader->layout;f.layout_from_pipeline_creation=true;}
    if(!f.layout){f.bindings_truncated=true;shape_evidence_.before(std::move(f));return;}
    std::map<unsigned,EvidenceRoot> roots;
    // Root observations have no descriptor budget. Flagged push-descriptor
    // declarations can be native root CBVs: the actual root kind decides.
    for(const auto &d:f.layout->ranges){const auto [it,added]=roots.try_emplace(d.parameter);if(added)it->second=root(d.parameter);}
    const auto emit=[&](const ShapeEvidenceRange &d,unsigned j,const EvidenceRoot &a){
            ShapeEvidenceBinding b;b.declaration=d;b.slot_in_range=j;
            b.declaration.root=a.status?d.root:(a.kind==2||a.kind==3||a.kind==4);
            b.status=a.status;b.root_kind=a.kind;b.argument=a.address;b.query_epoch=a.epoch;
            b.uav=d.type==unsigned(api::descriptor_type::unordered_access_view);
            if(!a.status&&a.address){
                // Root descriptor addresses are VAs, never COM objects. Only locate()
                // against the collector's creation index; do not dereference/Map.
                if(a.kind==2||a.kind==3||a.kind==4){
                    if(const auto *r=locate(a.address,1)){b.resource=r->key.object;b.creation=r->key.creation;b.offset=a.address-r->base;b.bytes=r->bytes-b.offset;b.extent_unknown=true;b.uav=a.kind==4;}
                    else b.status=2;
                }else if(a.kind==1&&source_){
                    api::descriptor_heap heap{};unsigned slot=0;
                    device_->get_descriptor_heap_offset(api::descriptor_table{a.address},d.binding+j,0,&heap,&slot);b.heap=heap.handle;b.slot=slot;
                    WWMI_RtDescriptorBuffer_v2 v{};
                    if(!heap.handle||!source_(device_->get_native(),heap.handle,slot,&v.base,sizeof(v))||v.base.abi_version!=2||v.base.struct_size!=sizeof(v))b.status=2;
                    else {
                        b.descriptor_version=2;b.wrote_frame=v.base.wrote_frame;b.wrote_source=v.base.wrote_source;b.resource=v.base.resource;b.format=v.format;b.stride=v.stride;
                        if(const auto r=resources_.find(v.base.resource);r!=resources_.end()&&current(r->second)){
                            b.creation=r->second.key.creation;unsigned unit=v.stride;
                            if(v.format==unsigned(api::format::r32_float)||v.format==unsigned(api::format::r32_uint)||v.format==unsigned(api::format::r32_sint)||v.format==unsigned(api::format::r32_typeless))unit=4;
                            else if(v.format==unsigned(api::format::r32g32_float)||v.format==unsigned(api::format::r32g32_uint))unit=8;
                            else if(v.format==unsigned(api::format::r32g32b32_float))unit=12;
                            else if(v.format==unsigned(api::format::r32g32b32a32_float)||v.format==unsigned(api::format::r32g32b32a32_uint))unit=16;
                            else if(v.format)unit=0;
                            if(unit&&v.first<=UINT64_MAX/unit&&v.count<=UINT64_MAX/unit){b.offset=v.first*unit;b.bytes=std::uint64_t(v.count)*unit;
                                if(b.offset>r->second.bytes||b.bytes>r->second.bytes-b.offset){b.bytes=0;b.status=2;}}
                            else b.extent_unknown=true;
                        }else b.status=2;
                    }
                }else if(a.kind!=5)b.status=2;
            }
            f.bindings.push_back(b);
    };
    // Independent buffer categories prevent an SRV table from starving UAVs
    // or b0. Within SRVs, retain the declared t3 slot before the remaining range.
    unsigned remaining[3]={16,32,64}; // CBV, UAV, SRV table slots
    const auto category=[](unsigned type){
        if(type==unsigned(api::descriptor_type::constant_buffer))return 0;
        if(type==unsigned(api::descriptor_type::unordered_access_view))return 1;
        if(type==unsigned(api::descriptor_type::shader_resource_view))return 2;
        return -1;
    };
    for(int kind=0;kind<3;++kind){
        struct Table {const ShapeEvidenceRange *d;EvidenceRoot a;unsigned preferred=0,next=0,emitted=0;};
        std::vector<Table> tables;
        for(const auto &d:f.layout->ranges){if(category(d.type)!=kind)continue;const auto a=roots.at(d.parameter);
            if(a.kind==2||a.kind==3||a.kind==4||a.kind==5||d.root){emit(d,0,a);continue;}
            if(d.count)tables.push_back({&d,a,kind==2&&d.reg<=3&&std::uint64_t(3-d.reg)<d.count?3-d.reg:0});
        }
        auto &budget=remaining[kind];
        // Priority slot of every range first, then round-robin remainder.
        for(auto &t:tables)if(budget){emit(*t.d,t.preferred,t.a);--budget;++t.emitted;}
        while(budget){bool progress=false;for(auto &t:tables){
            if(!budget)break;if(t.emitted>=t.d->count)continue;
            while(t.next==t.preferred)++t.next;
            emit(*t.d,t.next++,t.a);--budget;++t.emitted;progress=true;
        }if(!progress)break;}
        for(const auto &t:tables)if(t.emitted<t.d->count)f.bindings_truncated=true;
    }
    for(const auto &d:f.layout->ranges)if(category(d.type)<0){
        const auto a=roots.at(d.parameter);
        if(a.kind==5)emit(d,0,a);else ++f.non_buffer_ranges_omitted;
    }
    shape_evidence_.before(std::move(f));
}
