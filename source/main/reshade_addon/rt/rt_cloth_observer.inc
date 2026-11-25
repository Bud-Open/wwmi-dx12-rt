// Observe any component in the armed window. Material data is collected
// only after identity; pre-identity outcomes are candidate diagnostics.
struct RtClothTexture {
    uint64_t resource=0,descriptor=0,bound_table=0,recorded_source=0;
    unsigned param=0xff,offset=0;
    char author_name[64]{};
};
struct RtClothPart {
    unsigned requested_first=0,requested_count=0,requested_flags=0;
    unsigned emitted_first=0,emitted_count=0;
    int emitted_base=0;
    uint64_t draw_pso=0,ps_declared=0;
    unsigned ps_known=0,ps_unbounded=0,carried=0;
    int formats[9]{};
    uint64_t format_generation=0;
    RtPsCarrySources carry_sources{};
    float mod_enabled=-1,instance_valid=-1,object_detected=-1;
    RtClothTexture tex[9]{};
};
struct RtClothSection {
    uint64_t trace_seq=0,trace_frame=0,trace_vs=0,trace_ps=0;
    int mod=-1;unsigned reason=0,status=0,requested=0,recorded=0,empty=0;
    unsigned reads_previous=0,previous_bound=0,previous_resolution=0,draw_component_result=0,replaced=0;
    uint64_t format_generation=0;
    RtPsCarrySources carry_sources{};
    uint64_t author_plan=0;
    unsigned author_disposition=0,native_choice_rejected=0;
    bool material_detail=false;
    float mod_before=-1,instance_before=-1,object_before=-1,mod_after=-1,instance_after=-1,object_after=-1;
    std::vector<RtClothPart> parts;
};
struct RtObservedPsView {
    uint64_t heap=0,resource=0,cpu_view=0,view_owner=0,swapped=0;
    unsigned slot=0,format=0,mip=0,mips=0,layer=0,layers=0;
    unsigned wrote_frame=0,wrote_source=0;
};
struct RtClothRow {
    uint64_t seq=0,elapsed=0,frame=0,vs=0,ps=0,pso=0,list=0,api_list=0;
    unsigned object=0,hash=0,ordering=0;
    int component=-1;
    unsigned first=0,count=0,rts=0,depth=0,identified=0,reason=0,status=0;
    unsigned instances=0,first_instance=0;int32_t base=0;
    unsigned stream=0,depth_only=0,palette=0,did=0,native_choice_rejected=0,author_disposition=0;
    uint64_t author_snapshot=0;
    uint64_t effective_pso=0;
    RtDrawExecution execution=RtDrawExecution::Unrelated;
    bool effects_observed=false;
    RtRasterEffects effects{};
    Ia ia{};
    WWMI_RtDrawIdentityReceipt_v1 receipt{};
    std::array<RtObservedPsView,3> original_ps{};
    std::vector<RtClothSection> sections;
    bool material_detail()const{return identified!=0;}
};
constexpr unsigned kRtAuditPsRegs[]={0,1,3};
// Reasons describe precisely where this call returned; they are not diagnoses
// of what was visible. A candidate without identity is never counted as identified.
static const char* RtClothReason(unsigned n){
    switch(n){
    case 1:return "reader_missing";case 2:return "ia_untracked";case 3:return "ia_invalid";
    case 4:return "identity_failed";case 5:return "unsupported_pass";case 6:return "no_rtv";
    case 7:return "ia_restore_incomplete";case 8:return "bone_inputs";case 9:return "no_section";
    case 10:return "geometry_disabled";case 11:return "world_surface_bind";case 12:return "native_palette";
    case 13:return "plan_missing";case 14:return "section_incomplete";case 15:return "selection_read";
    case 16:return "geometry_surface";case 17:return "previous_pose";case 18:return "material";
    case 19:return "geometry_acquire";case 20:return "geometry_size";case 21:return "empty_selection";
    case 22:return "private_draw_recorded";case 23:return "palette_result_draw_count_unknown";
    case 24:return "private_draw_history_invalid";
    case 25:return "draw_shape";
    case 26:return "coherent_commit";
    case 27:return "choice_native_before_vm";
    default:return "none";
    }
}
std::vector<RtClothRow> cloth_rows;
uint64_t cloth_seq=0;unsigned cloth_inflight=0,cloth_dropped=0,cloth_abandoned=0;
constexpr size_t kRtDrawBacklog=8192; // pending rows between writes, not total rows per window
struct RtClothDraw {
    std::optional<RtClothRow> row;
    explicit RtClothDraw(int component){if(component>=0)start(component);}
    void start(int component){
        if(row || field_state.load(std::memory_order_relaxed)!=1)return;
        std::lock_guard lock(field_mx);if(field_state.load()!=1)return;
        const auto elapsed=GetTickCount64()-field_begin;
        if(elapsed>=kRtDrawWindowMs)return;
        auto& r=row.emplace();
        ++cloth_inflight;r.seq=++cloth_seq;r.frame=uint64_t(g_frame_id)+1;r.elapsed=elapsed;
        r.component=component;r.reason=9;
    }
    void abandon(){
        if(!row)return;std::lock_guard lock(field_mx);--cloth_inflight;++cloth_abandoned;row.reset();
    }
    void capture(reshade::api::command_list* list,unsigned first,unsigned count,unsigned instances,int32_t base,unsigned first_instance){
        if(!row)return;auto& r=*row;
        r.first=first;r.count=count;r.list=reinterpret_cast<uint64_t>(list->get_native());r.api_list=reinterpret_cast<uint64_t>(list);
        r.instances=instances;r.base=base;r.first_instance=first_instance;
        list->get_private_data(s_iid_vs_hash,&r.vs);list->get_private_data(s_iid_ps_hash,&r.ps);
        const auto rt=CmdRt(list);r.pso=CmdGfx(list).gfx_pso;r.rts=rt.rtv_count;r.depth=rt.dsv!=0;
    }
    void capture_ia(const Ia& ia){if(row)row->ia=ia;}
    void capture_ps(reshade::api::command_list* list){
        if(!row||!row->material_detail())return;
        const auto reads=PipelineSrvRegs(row->pso,'p');
        if(!reads.parsed||reads.unbounded)return;
        RegBind bindings[std::size(kRtAuditPsRegs)]{};
        {
            std::lock_guard lock(g_layout_regmap_mtx);
            auto i=g_layout_regmap.find(CmdGfxLayout(list));if(i==g_layout_regmap.end()||!i->second.built)return;
            for(unsigned k=0;k<std::size(kRtAuditPsRegs);++k)bindings[k]=i->second.t_ps[kRtAuditPsRegs[k]];
        }
        auto* dev=list->get_device();
        const auto& g=CmdGfx(list);
        for(unsigned k=0;k<std::size(kRtAuditPsRegs);++k){
            const auto& b=bindings[k];
            if(!reads.reads(kRtAuditPsRegs[k])||b.param>=64||!b.is_table)continue;
            const auto table=TableStartOf(dev,g.root[b.param].table);
            if(!table||b.offset>UINT32_MAX-table.base)continue;
            auto& o=row->original_ps[k];o.heap=table.heap;o.slot=table.base+b.offset;
            ResInfo ri{};if(!g_heap_to_res.find({o.heap,o.slot},ri))continue;
            o.resource=reinterpret_cast<uint64_t>(ri.res);o.cpu_view=ri.view_handle;o.swapped=reinterpret_cast<uint64_t>(ri.swapped);
            o.wrote_frame=ri.wrote_frame;o.wrote_source=ri.wrote_src;
            if(!o.cpu_view||ri.is_buffer)continue;
            using namespace reshade::api;
            const resource_view view{o.cpu_view};
            o.view_owner=dev->get_resource_from_view(view).handle;
            if(!o.view_owner||o.view_owner!=o.resource)continue;
            const auto d=dev->get_resource_view_desc(view);
            if(d.type!=resource_view_type::texture_2d&&d.type!=resource_view_type::texture_2d_array)continue;
            o.format=unsigned(d.format);o.mip=d.texture.first_level;o.mips=d.texture.levels;
            o.layer=d.texture.first_layer;o.layers=d.texture.layers;
        }
    }
    void fail(unsigned reason,unsigned status=0){if(row){row->reason=reason;row->status=status;}}
    RtClothSection* section(int mod){
        if(!row)return nullptr;
        auto& s=row->sections.emplace_back();s.mod=mod;
        s.trace_seq=row->seq;s.trace_frame=row->frame;s.trace_vs=row->vs;s.trace_ps=row->ps;
        s.material_detail=row->material_detail();
        return &s;
    }
    ~RtClothDraw(){
        if(!row)return;std::lock_guard lock(field_mx);
        if(field_state.load(std::memory_order_relaxed)!=3){
            if(cloth_rows.size()<kRtDrawBacklog)cloth_rows.push_back(std::move(*row));else ++cloth_dropped;
        }
        --cloth_inflight;
    }
};

// Only a currently loaded resource whose actual pointer matches a stamped
// descriptor AND this mod's own texture resource receives an author name.
// ForEachCustomResource is read-only; FindCustomResource is deliberately not
// used here because it can substantiate/load a resource.
static void RtClothSectionVariables(RtClothSection* section,int mod,bool after){
    if(!section||!section->material_detail)return;const auto& ns=g_geomods[mod].wcl_ns;
    const float enabled=PortApi::GetVarNs(ns.c_str(),L"mod_enabled",-1.f);
    const float instance=PortApi::GetVarNs(ns.c_str(),L"instance_valid",-1.f);
    const float object=PortApi::GetVarNs(ns.c_str(),L"object_detected",-1.f);
    if(after){section->mod_after=enabled;section->instance_after=instance;section->object_after=object;}
    else{section->mod_before=enabled;section->instance_before=instance;section->object_before=object;}
}
static void RtClothTextureNames(RtClothPart& part,int mod){
    struct Query { RtClothPart* part;const WwmiMod::Mod* model; } q{&part,&g_geomods[mod].mod};
    PortApi::ForEachCustomResource(
        [](const wchar_t* fq,const wchar_t*,int,bool,ID3D12Resource* dx12,void* opaque){
            if(!fq || !dx12)return;auto& q=*static_cast<Query*>(opaque);
            for(const auto& source:q.model->resources){
                if(source.fq!=fq || source.filename.empty() || (source.name.find(L"Resourcetexture_")!=0 && source.name.find(L"ResourceTexture_")!=0))continue;
                for(auto& t:q.part->tex){
                    if(t.resource!=reinterpret_cast<uint64_t>(dx12))continue;
                    unsigned i=0;for(wchar_t c:source.name){if(i+1>=sizeof(t.author_name))break;t.author_name[i++]=c>=32&&c<127?char(c):'?';}t.author_name[i]=0;
                }
                break;
            }
        },&q,
        [](ID3D12Resource* dx12,void* opaque){
            if(!dx12)return false;auto& q=*static_cast<Query*>(opaque);
            for(const auto& t:q.part->tex)if(t.resource==reinterpret_cast<uint64_t>(dx12))return true;
            return false;
        });
}
static const char* RtIdentifyReason(unsigned n){
    switch(n){
    case WWMI_RT_IDENTIFY_OK:return "identified";
    case WWMI_RT_IDENTIFY_QUERY:return "query_contract";
    case WWMI_RT_IDENTIFY_DEVICE:return "device";
    case WWMI_RT_IDENTIFY_INACTIVE:return "inactive";
    case WWMI_RT_IDENTIFY_RECORDING:return "recording_unknown";
    case WWMI_RT_IDENTIFY_RAW_SUBVIEW:return "no_live_blas_subview";
    case WWMI_RT_IDENTIFY_VERTEX_RANGE:return "vertex_range";
    case WWMI_RT_IDENTIFY_INDEX_RANGE:return "index_range";
    case WWMI_RT_IDENTIFY_RESOURCE_KEYS:return "resource_key_mismatch";
    case WWMI_RT_IDENTIFY_ORDER_PENDING:return "order_pending";
    case WWMI_RT_IDENTIFY_AMBIGUOUS:return "ambiguous";
    case WWMI_RT_IDENTIFY_SOURCE_EXPIRED:return "source_expired";
    default:return "unknown";
    }
}
static const char *RtExecutionName(RtDrawExecution execution){
    switch(execution){
    case RtDrawExecution::NativeSelected:return "native_selected";
    case RtDrawExecution::PrivateRecorded:return "private_recorded";
    case RtDrawExecution::HiddenSelected:return "hidden_selected";
    case RtDrawExecution::PlanViolation:return "plan_violation";
    default:return "unrelated";
    }
}
static void RtClothLog(const std::vector<RtClothRow>& rows){
    for(const auto& r:rows){
        char line[1240];
        std::snprintf(line,sizeof(line),"RT-CLOTH draw seq=%llu ms=%llu frame=%llu identity=%s component=%d object=%u content=%08x vs=%016llx ps=%016llx pso=%llx list=%llx api_list=%llx host_record=%llu/%llu bind_seq=%llu native_first=%u native_count=%u base=%d instances=%u first_instance=%u vb=%llx+%u/%u ib=%llx+%u/%u rts=%u depth=%u stream=%u depth_only=%u palette=%u did=%u reason=%s status=%u sections=%zu native_choice_rejected=%u author_disposition=%u execution=%s",
            (unsigned long long)r.seq,(unsigned long long)r.elapsed,(unsigned long long)r.frame,
            r.identified?"identified":"candidate_unidentified",r.component,r.object,r.hash,
            (unsigned long long)r.vs,(unsigned long long)r.ps,(unsigned long long)r.pso,(unsigned long long)r.list,(unsigned long long)r.api_list,
            (unsigned long long)r.ia.creation,(unsigned long long)r.ia.recording,(unsigned long long)r.ia.binding_sequence,
            r.first,r.count,r.base,r.instances,r.first_instance,
            (unsigned long long)r.ia.vb[0].BufferLocation,r.ia.vb[0].SizeInBytes,r.ia.vb[0].StrideInBytes,
            (unsigned long long)r.ia.ib.BufferLocation,r.ia.ib.SizeInBytes,unsigned(r.ia.ib.Format),
            r.rts,r.depth,r.stream,r.depth_only,r.palette,r.did,RtClothReason(r.reason),r.status,r.sections.size(),r.native_choice_rejected,r.author_disposition,RtExecutionName(r.execution));
        reshade::log::message(reshade::log::level::info,line);
        if(r.effects_observed&&r.execution!=RtDrawExecution::PrivateRecorded&&r.execution!=RtDrawExecution::HiddenSelected){
            const auto &e=r.effects;
            std::snprintf(line,sizeof(line),"RT-CLOTH effects seq=%llu native_pso=%llx effective_pso=%llx color=%s basis=%u color_slots=%x declared=%x write=%x attached=%x shader_shape=%u depth_test=%u depth_write=%u stencil_test=%u stencil_write=%u coverage=%u uav=%u stream_output=%u queries=%u predication=%u ps_depth=%u ps_coverage=%u ps_stencil_ref=%u",
                (unsigned long long)r.seq,(unsigned long long)r.pso,(unsigned long long)r.effective_pso,
                e.color==RtColorKnowledge::KnownMask?"known_mask":e.color==RtColorKnowledge::KnownZero?"known_zero":"unknown",
                unsigned(e.basis),unsigned(e.color_mask),unsigned(e.declared_slots),unsigned(e.write_slots),unsigned(e.attached_slots),e.standard_pixel_shape,
                unsigned(e.depth_test),unsigned(e.depth_write),unsigned(e.stencil_test),unsigned(e.stencil_write),
                unsigned(e.coverage),unsigned(e.uav),unsigned(e.stream_output),unsigned(e.queries),unsigned(e.predication),
                e.pixel_depth,e.pixel_coverage,e.pixel_stencil_ref);
            reshade::log::message(reshade::log::level::info,line);
        }
        const auto& d=r.receipt;
        if(d.struct_size){
            std::snprintf(line,sizeof(line),"RT-CLOTH identity seq=%llu decision=%s ordering=%u exact=%u client_record=%llu/%llu vb=%llx/%llu ib=%llx/%llu source=%u/%08x/%u content_window=%llx/%llu+%llu blas=%llx/%llu@%llx pose_order=%llu writer=%llx/%llu/%llu submits=%u queue=%llx fence=%llx value=%llu done=%llu candidate_write=%llu+%llu/%llu candidate_writer=%llx/%llu/%llu candidate_writer_submits=%u author_snapshot=%llu",
                (unsigned long long)r.seq,RtIdentifyReason(d.reason),r.ordering,d.exact_matches,
                (unsigned long long)d.consumer_creation,(unsigned long long)d.consumer_recording,
                (unsigned long long)d.vertex_resource,(unsigned long long)d.vertex_creation,(unsigned long long)d.index_resource,(unsigned long long)d.index_creation,
                d.object_guid,d.content_hash,d.component,(unsigned long long)d.content_resource,(unsigned long long)d.content_creation,(unsigned long long)d.content_offset,
                (unsigned long long)d.blas_resource,(unsigned long long)d.blas_creation,(unsigned long long)d.blas_va,(unsigned long long)d.pose_order,
                (unsigned long long)d.writer_list,(unsigned long long)d.writer_creation,(unsigned long long)d.writer_recording,
                d.submissions,(unsigned long long)d.producer_queue,(unsigned long long)d.producer_fence,(unsigned long long)d.producer_value,(unsigned long long)d.observed_completion,
                (unsigned long long)d.candidate_write_offset,(unsigned long long)d.candidate_write_bytes,(unsigned long long)d.candidate_write_order,
                (unsigned long long)d.candidate_writer_list,(unsigned long long)d.candidate_writer_creation,(unsigned long long)d.candidate_writer_recording,d.candidate_writer_submissions,
                (unsigned long long)r.author_snapshot);
            reshade::log::message(reshade::log::level::info,line);
        }
        for(unsigned k=0;k<r.original_ps.size();++k){
            const auto& o=r.original_ps[k];if(!o.heap)continue;
            std::snprintf(line,sizeof(line),"RT-CLOTH original_ps seq=%llu reg=t%u heap_slot=%llx/%u shadow_resource=%llx cpu_view=%llx cpu_view_owner=%llx swapped=%llx fmt=%u subview=%u+%u/%u+%u write_frame=%u write_source=%u",
                (unsigned long long)r.seq,kRtAuditPsRegs[k],(unsigned long long)o.heap,o.slot,
                (unsigned long long)o.resource,(unsigned long long)o.cpu_view,(unsigned long long)o.view_owner,(unsigned long long)o.swapped,
                o.format,o.mip,o.mips,o.layer,o.layers,o.wrote_frame,o.wrote_source);
            reshade::log::message(reshade::log::level::info,line);
        }
        for(size_t i=0;i<r.sections.size();++i){const auto& s=r.sections[i];
            std::snprintf(line,sizeof(line),"RT-CLOTH section seq=%llu section=%zu mod=%d reason=%s status=%u requested=%u recorded_nonempty=%u empty=%u prev_reads=%u prev_bound=%u prev_resolution=%u draw_result=%u replaced=%u vars_before=%g,%g,%g vars_after=%g,%g,%g author_plan=%llu author_disposition=%u native_choice_rejected=%u",
                (unsigned long long)r.seq,i,s.mod,RtClothReason(s.reason),s.status,s.requested,s.recorded,s.empty,s.reads_previous,s.previous_bound,s.previous_resolution,s.draw_component_result,s.replaced,s.mod_before,s.instance_before,s.object_before,s.mod_after,s.instance_after,s.object_after,
                (unsigned long long)s.author_plan,s.author_disposition,s.native_choice_rejected);
            reshade::log::message(reshade::log::level::info,line);
            for(size_t j=0;j<s.parts.size();++j){const auto& p=s.parts[j];
                std::snprintf(line,sizeof(line),"RT-CLOTH part seq=%llu section=%zu part=%zu requested=%u+%u flags=%u emitted=%u+%u base=%d actual_pso=%llx ps_declared=%llx ps_known=%u ps_unbounded=%u carried=%x carry_gen=%llu vm_formats=%d,%d,%d,%d,%d,%d,%d,%d,%d t0_override_planned=%u t0_author_planned=%u t0_override_recorded=%u t0_author_recorded=%u mod_enabled=%g instance_valid=%g object_detected=%g",
                    (unsigned long long)r.seq,i,j,p.requested_first,p.requested_count,p.requested_flags,p.emitted_first,p.emitted_count,p.emitted_base,
                    (unsigned long long)p.draw_pso,(unsigned long long)p.ps_declared,p.ps_known,p.ps_unbounded,p.carried,
                    (unsigned long long)p.format_generation,p.formats[0],p.formats[1],p.formats[2],p.formats[3],p.formats[4],
                    p.formats[5],p.formats[6],p.formats[7],p.formats[8],
                    unsigned(p.tex[0].descriptor!=0),unsigned(p.tex[0].descriptor&&p.tex[0].author_name[0]),
                    unsigned(p.tex[0].bound_table&&p.tex[0].recorded_source==p.tex[0].descriptor),
                    unsigned(p.tex[0].bound_table&&p.tex[0].recorded_source==p.tex[0].descriptor&&p.tex[0].author_name[0]),
                    p.mod_enabled,p.instance_valid,p.object_detected);
                reshade::log::message(reshade::log::level::info,line);
                for(unsigned k=0;k<9;++k){const auto& t=p.tex[k];if(!t.descriptor)continue;
                    std::snprintf(line,sizeof(line),"RT-CLOTH texture seq=%llu section=%zu part=%zu reg=t%u resource=%llx descriptor=%llx author=%s ps_declared=%u ps_known=%u recorded_table=%llx recorded_src=%llx root=%u offset=%u",
                        (unsigned long long)r.seq,i,j,k,(unsigned long long)t.resource,(unsigned long long)t.descriptor,
                        t.author_name[0]?t.author_name:"unresolved",unsigned((p.ps_declared>>k)&1u),p.ps_known,
                        (unsigned long long)t.bound_table,(unsigned long long)t.recorded_source,t.param,t.offset);
                    reshade::log::message(reshade::log::level::info,line);
                }
                if(p.carried){
                    // Same list/recording as the receiving row. A zero source
                    // sequence denotes a source without an audited draw row.
                    size_t used=std::snprintf(line,sizeof(line),"RT-CLOTH carry seq=%llu section=%zu part=%zu",
                        (unsigned long long)r.seq,i,j);
                    for(unsigned k=0;k<p.carry_sources.size();++k)if(p.carry_sources[k].value){
                        const auto& source=p.carry_sources[k];
                        used+=std::snprintf(line+used,sizeof(line)-used," t%u=%u/%llu/%llu/%016llx/%016llx",k,source.value,
                            (unsigned long long)source.seq,(unsigned long long)source.frame,(unsigned long long)source.vs,(unsigned long long)source.ps);
                    }
                    reshade::log::message(reshade::log::level::info,line);
                }
            }
        }
    }
}
